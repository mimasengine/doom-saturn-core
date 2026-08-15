//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Handles WAD file header, directory, lump I/O.
//




#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"

#include "config.h"
#include "d_iwad.h"
#include "i_swap.h"
#include "i_system.h"
#include "i_video.h"
#include "m_misc.h"
#include "z_zone.h"

#include "w_wad.h"

typedef struct
{
    // Should be "IWAD" or "PWAD".
    char		identification[4];		
    int			numlumps;
    int			infotableofs;
} PACKEDATTR wadinfo_t;


typedef struct
{
    int			filepos;
    int			size;
    char		name[8];
} PACKEDATTR filelump_t;

//
// GLOBALS
//

// Location of each lump on disk.

lumpinfo_t *lumpinfo;
unsigned int numlumps = 0;

// SATURN garde-W_ReadLump: count of streaming reads that came up short and were
// zero-filled (crash-proofed) instead of I_Error-freezing.  Surfaced on the debug
// overlay (LIM row `rl`).  0 = every read was complete this session; >0 = a short CD
// read was sunk (see W_ReadLump below).
int r_readlump_short = 0;

/* SATURN 2026-08-07: cumulative W_ReadLump calls = LUMPS actually pulled off the medium.  The
   platform's sat_cd_loads counts GFS *chunk commands* (several per big lump) and lives outside
   core, so it cannot be read from here; this one is core, so p_setup.c can bracket the level-load
   phases with it and DoomJo still compiles.  Answers "the load is 4704 CD commands -- issued BY
   WHAT?" ([[streaming-load-budget-and-flat-treadmill]]).  Cart builds read every lump once at
   W_AddFile and never again, so this stays flat there. */
int w_lump_reads = 0;

/* SATURN 2026-08-07: cumulative wall-clock spent INSIDE medium commands, in tenths of a ms.
   Owned by core (not by the platform) for one reason: the per-frame texture LOAD BUDGET
   (r_segs.c R_LoadBudgetLeft) is a TIME budget, and a time budget cannot live in core while
   its clock lives in src/.  The platform feeds it -- Mimas from sat_cd_clock_add
   (w_file_saturn.cxx), which FRT-times every GFS command.  A port that never feeds it (DoomJo,
   any cart build) leaves it at 0, the budget then never trips, and every texture faults in on
   sight exactly as before: the budget is inert, not broken, on a medium with no latency. */
unsigned int w_cd_ms10 = 0;

// SATURN R4.3c: the single WAD file backing every lump (was a per-lump lumpinfo.wad_file
// pointer -- dropped to save 4B/lump off the zone).  Set once by W_AddFile; a second file
// trips a loud guard there.  All read sites below use this instead of lump->wad_file.
static wad_file_t *w_wadfile = NULL;

// SATURN R4.3c: accessor for the single WAD file -- w_checksum.c needs it now that the
// per-lump lumpinfo.wad_file field is gone (with one file every lump is file #0).
wad_file_t *W_MainWadFile(void) { return w_wadfile; }

// Hash table for fast lookups

static lumpinfo_t **lumphash;

// Hash function used for lump names.

unsigned int W_LumpNameHash(const char *s)
{
    // This is the djb2 string hash function, modded to work on strings
    // that have a maximum length of 8.

    unsigned int result = 5381;
    unsigned int i;

    for (i=0; i < 8 && s[i] != '\0'; ++i)
    {
        result = ((result << 5) ^ result ) ^ toupper((int)s[i]);
    }

    return result;
}

// Increase the size of the lumpinfo[] array to the specified size.
static void ExtendLumpInfo(int newnumlumps)
{
    lumpinfo_t *newlumpinfo;
    unsigned int i;

    // SATURN: lumpinfo lives in the Doom zone (roomy ~1MB LWRAM), NOT the small libc heap.
    // It is the heap's dominant consumer (numlumps * sizeof(lumpinfo_t)); keeping it out of
    // the HWRAM libc heap lets HEAP_SIZE (syscalls.c) shrink, which grows the SRL TLSF pool.
    // Z_Malloc does not zero, so clear it (calloc semantics: entries past the copied range
    // need NULL cache/next).  Z_Init runs long before the first W_AddFile, so the zone is up.
    newlumpinfo = Z_Malloc(newnumlumps * sizeof(lumpinfo_t), PU_STATIC, NULL);

    if (newlumpinfo == NULL)
    {
	I_Error ("Couldn't realloc lumpinfo");
    }
    memset(newlumpinfo, 0, newnumlumps * sizeof(lumpinfo_t));

    // Copy over lumpinfo_t structures from the old array. If any of
    // these lumps have been cached, we need to update the user
    // pointers to the new location.
    for (i = 0; i < numlumps && i < newnumlumps; ++i)
    {
        memcpy(&newlumpinfo[i], &lumpinfo[i], sizeof(lumpinfo_t));

        if (newlumpinfo[i].cache != NULL)
        {
            Z_ChangeUser(newlumpinfo[i].cache, &newlumpinfo[i].cache);
        }

        // We shouldn't be generating a hash table until after all WADs have
        // been loaded, but just in case...
        if (lumpinfo[i].next != NULL)
        {
            int nextlumpnum = lumpinfo[i].next - lumpinfo;
            newlumpinfo[i].next = &newlumpinfo[nextlumpnum];
        }
    }

    // All done.  (Guard: the first call has lumpinfo == NULL; Z_Free(NULL) would crash.)
    if (lumpinfo != NULL)
	Z_Free (lumpinfo);
    lumpinfo = newlumpinfo;
    numlumps = newnumlumps;
}

//
// LUMP BASED ROUTINES.
//

//
// W_AddFile
// All files are optional, but at least one file must be
//  found (PWAD, if all required lumps are present).
// Files with a .wad extension are wadlink files
//  with multiple lumps.
// Other files are single lumps with the base filename
//  for the lump name.

wad_file_t *W_AddFile (char *filename)
{
    wadinfo_t header;
    lumpinfo_t *lump_p;
    unsigned int i;
    wad_file_t *wad_file;
    int length;
    int startlump;
    filelump_t *fileinfo;
    filelump_t *filerover;
    int newnumlumps;

    // open the file and add to directory

    wad_file = W_OpenFile(filename);

    if (wad_file == NULL)
    {
		printf (" couldn't open %s\n", filename);
		return NULL;
    }

    // SATURN R4.3c: lumpinfo dropped its per-lump wad_file pointer -> one global file.
    // Mimas only ever adds a single WAD; fail loud rather than silently corrupt reads
    // if a second (different) file is ever added (would need the per-lump field back).
    if (w_wadfile != NULL && w_wadfile != wad_file)
        I_Error ("W_AddFile: multi-file not supported (lumpinfo.wad_file dropped, R4.3c)");
    w_wadfile = wad_file;

    newnumlumps = numlumps;

    if (strcasecmp(filename+strlen(filename)-3 , "wad" ) )
    {
    	// single lump file

        // fraggle: Swap the filepos and size here.  The WAD directory
        // parsing code expects a little-endian directory, so will swap
        // them back.  Effectively we're constructing a "fake WAD directory"
        // here, as it would appear on disk.

		fileinfo = Z_Malloc(sizeof(filelump_t), PU_STATIC, 0);
		fileinfo->filepos = LONG(0);
		fileinfo->size = LONG(wad_file->length);

        // Name the lump after the base of the filename (without the
        // extension).

		M_ExtractFileBase (filename, fileinfo->name);
		newnumlumps++;
    }
    else 
    {
    	// WAD file
        W_Read(wad_file, 0, &header, sizeof(header));

		if (strncmp(header.identification,"IWAD",4))
		{
			// Homebrew levels?
			if (strncmp(header.identification,"PWAD",4))
			{
			I_Error ("Wad file %s doesn't have IWAD "
				 "or PWAD id\n", filename);
			}

			// ???modifiedgame = true;
		}

		header.numlumps = LONG(header.numlumps);
		header.infotableofs = LONG(header.infotableofs);
		length = header.numlumps*sizeof(filelump_t);
		fileinfo = Z_Malloc(length, PU_STATIC, 0);

        W_Read(wad_file, header.infotableofs, fileinfo, length);
        newnumlumps += header.numlumps;
    }

    // Increase size of numlumps array to accomodate the new file.
    startlump = numlumps;
    ExtendLumpInfo(newnumlumps);

    lump_p = &lumpinfo[startlump];

    filerover = fileinfo;

    for (i=startlump; i<numlumps; ++i)
    {
		lump_p->position = LONG(filerover->filepos);
		lump_p->size = LONG(filerover->size);
			lump_p->cache = NULL;
		strncpy(lump_p->name, filerover->name, 8);

			++lump_p;
			++filerover;
    }

    Z_Free(fileinfo);

    if (lumphash != NULL)
    {
        Z_Free(lumphash);
        lumphash = NULL;
    }

    return wad_file;
}



//
// W_NumLumps
//
int W_NumLumps (void)
{
    return numlumps;
}



//
// W_CheckNumForName
// Returns -1 if name not found.
//

int W_CheckNumForName (char* name)
{
    lumpinfo_t *lump_p;
    int i;

    // Do we have a hash table yet?

    if (lumphash != NULL)
    {
        int hash;
        
        // We do! Excellent.

        hash = W_LumpNameHash(name) % numlumps;
        
        for (lump_p = lumphash[hash]; lump_p != NULL; lump_p = lump_p->next)
        {
            if (!strncasecmp(lump_p->name, name, 8))
            {
                return lump_p - lumpinfo;
            }
        }
    } 
    else
    {
        // We don't have a hash table generate yet. Linear search :-(
        // 
        // scan backwards so patch lump files take precedence

        for (i=numlumps-1; i >= 0; --i)
        {
            if (!strncasecmp(lumpinfo[i].name, name, 8))
            {
                return i;
            }
        }
    }

    // TFB. Not found.

    return -1;
}




//
// W_GetNumForName
// Calls W_CheckNumForName, but bombs out if not found.
//
int W_GetNumForName (char* name)
{
    int	i;

    i = W_CheckNumForName (name);

    if (i < 0)
    {
        I_Error ("W_GetNumForName: %s not found!", name);
    }
 
    return i;
}


//
// W_LumpLength
// Returns the buffer size needed to load the given lump.
//
int W_LumpLength (unsigned int lump)
{
    if (lump >= numlumps)
    {
	I_Error ("W_LumpLength: %i >= numlumps", lump);
    }

    return lumpinfo[lump].size;
}



//
// W_ReadLump
// Loads the lump into the given buffer,
//  which must be >= W_LumpLength().
//
void W_ReadLump(unsigned int lump, void *dest)
{
    int c;
    lumpinfo_t *l;

    if (lump >= numlumps)
    {
	I_Error ("W_ReadLump: %i >= numlumps", lump);
    }

    l = lumpinfo+lump;
    w_lump_reads++;

    I_BeginRead ();

    /* SATURN: wad_file->mapped = sat_wad_base (direct cart RAM pointer).
       Bypass Saturn_Read (which gates on sat_wad_size and can return 0 for
       lumps in the second half of the WAD) and memcpy directly from the
       memory-mapped region, exactly as W_CacheLumpNum does. */
    if (w_wadfile->mapped != NULL)
    {
        memcpy(dest, w_wadfile->mapped + l->position, l->size);
        I_EndRead();
        return;
    }

#ifdef SAT_REPACK
    /* SATURN per-level repack (STREAMING_ANALYSIS.md §7.4/7.9-7.11): if this lump is
       in the current map's DOOMRP.DRP blob, read + LZSS-decode it from there.  Returns
       1 = served; 0 = not in subset / no .DRP -> fall through to the full-WAD read. */
    {
        extern int sat_drp_read_lump(unsigned int lump, void *dest, int size);
        if (sat_drp_read_lump(lump, dest, l->size)) { I_EndRead(); return; }
    }
#endif

    c = W_Read(w_wadfile, l->position, dest, l->size);

    if (c < l->size)
    {
	// SATURN garde-W_ReadLump: a streaming CD read that returns short (transient GFS
	// truncation, or an endgame-fragmented heap starving the bounce staging) used to
	// I_Error-freeze here -- the LAST fatal I_Error on the gameplay hot path (the TNT
	// halt seen in capture #9).  Sink it gracefully instead: zero-fill the shortfall
	// and continue, so a missing lump renders as HOM / a silent lump (survivable)
	// rather than a hard crash.  Same graceful-sink policy as the visplane / openings
	// / composite guards.  r_readlump_short (overlay LIM `rl`) flags how often it fired.
	if (c < 0)
	    c = 0;
	memset((byte *) dest + c, 0, l->size - c);
	r_readlump_short++;
    }

    I_EndRead ();
}




/* (W_ReadHeaderSweep removed 2026-08-07 -- the .DRP R3.1 sprite index supersedes it and the
   TLSF pool could not carry both.  See r_data.c R_InitSpriteLumps.) */

/* SATURN 2026-08-07 -- PINNED LUMPS.  For the handful of lumps read EVERY frame, where the
   allocate/purge/re-read cycle costs more than the residency.  Today that is exactly one: the SKY
   PATCH.  Doom's sky is a single-patch texture, so `R_GetColumn(skytexture, x)` takes the
   `collump[x] >= 0` path and calls W_CacheLumpNum on the whole patch -- **34 KB loaded to read one
   128-byte column**, tagged PU_CACHE, purged by the next allocation, re-read next frame.  That one
   lump was simultaneously the biggest recurring CD read AND the biggest contiguous request in the
   zone: `Zmalloc fail 35104 t8 (fr205K lg32K)` is RSKY1 (35080 B + 24 B header, a 256x128 patch).
   A 34 KB block making that round trip every frame is the worst fragmenter the zone can have, so
   pinning it does not COST contiguity -- it BUYS it.
   Kept deliberately tiny and fixed: this is not a cache, it is a short list of exceptions. */
#define W_PIN_MAX 2
static int w_pin[W_PIN_MAX] = { -1, -1 };

int W_LumpPinned (int lumpnum)
{
    int i;
    for (i = 0; i < W_PIN_MAX; i++)
        if (w_pin[i] == lumpnum) return 1;
    return 0;
}

void W_UnpinAll (void)
{
    int i;
    for (i = 0; i < W_PIN_MAX; i++) w_pin[i] = -1;
}

/* Cache `lumpnum` with `tag` and mark it no-demote.  Pin AFTER the cache call so a re-pin of an
   already-resident lump still gets its tag refreshed (a level boundary re-pins the same sky). */
void W_PinLump (int lumpnum, int tag)
{
    int i;
    if ((unsigned)lumpnum >= numlumps) return;
    for (i = 0; i < W_PIN_MAX; i++)
        if (w_pin[i] < 0)
        {
            W_CacheLumpNum (lumpnum, tag);
            w_pin[i] = lumpnum;
            return;
        }
}


//
// W_CacheLumpNum
//
// Load a lump into memory and return a pointer to a buffer containing
// the lump data.
//
// 'tag' is the type of zone memory buffer to allocate for the lump
// (usually PU_STATIC or PU_CACHE).  If the lump is loaded as 
// PU_STATIC, it should be released back using W_ReleaseLumpNum
// when no longer needed (do not use Z_ChangeTag).
//

/* SATURN: "would W_CacheLumpNum(lumpnum) do I/O?", answered WITHOUT doing any.  In the
   CD-streaming build a miss is a synchronous ~42 ms disc read charged to whatever render phase
   asked for it (`Bp` for a wall patch, `P` for a flat) -- this is the predicate the per-frame
   load budget uses to decide "draw it flat this frame instead".  A memory-mapped WAD (cart) is
   always resident. */
/* SATURN 2026-08-08: a PURE READ of a lump's cached pointer -- no allocation, no disc, and above
   all NO Z_ChangeTag.  It exists for the SLAVE SH-2.
   R_SlaveDrawVisSprite called W_CacheLumpNum, whose already-cached branch still runs
   Z_ChangeTag(lump->cache, tag) -- i.e. the slave WRITES a zone block header, and reads
   `block->id != ZONEID` to validate it, while the master may be mid-Z_Malloc splitting that very
   block.  Two consequences, both bad and both worse the busier the scene (more sprites -> more
   slave draws -> more exposure): a torn tag, and an I_Error raised ON THE SLAVE, which has no way
   to report it and simply stops -- which is what `SLV b0% id100%` with `to9:9490` looks like.
   The retag is not needed here anyway: the master's own pass over the same sprite sets it.
   Returns NULL when the lump is not cached; the slave must then skip, which it already does. */
void* W_LumpCached (int lumpnum)
{
    if ((unsigned)lumpnum >= numlumps) return NULL;
    /* CART/mapped WAD: W_CacheLumpNum returns a pointer INTO the mapped region and never fills
       lumpinfo[].cache, so a bare `.cache` read would be NULL for every lump and the slave would
       silently draw NO sprites at all.  Mirror what W_CacheLumpNum does (w_wad.c ~551). */
    if (w_wadfile && w_wadfile->mapped != NULL)
	return w_wadfile->mapped + lumpinfo[lumpnum].position;
    return lumpinfo[lumpnum].cache;
}

int W_LumpResident (int lumpnum)
{
    if ((unsigned)lumpnum >= numlumps) return 1;    /* bogus -> caller will I_Error anyway */
    if (w_wadfile && w_wadfile->mapped != NULL) return 1;
    return lumpinfo[lumpnum].cache != NULL;
}

void *W_CacheLumpNum(int lumpnum, int tag)
{
    byte *result;
    lumpinfo_t *lump;

    if ((unsigned)lumpnum >= numlumps)
    {
	I_Error ("W_CacheLumpNum: %i >= numlumps", lumpnum);
    }

    lump = &lumpinfo[lumpnum];

    // Get the pointer to return.  If the lump is in a memory-mapped
    // (SATURN: W_LumpResident below answers "would this call do I/O?" -- see r_data.c
    //  R_TextureIOFree and the wall load budget in r_segs.c)
    // file, we can just return a pointer to within the memory-mapped
    // region.  If the lump is in an ordinary file, we may already
    // have it cached; otherwise, load it into memory.

    if (w_wadfile->mapped != NULL)
    {
        // Memory mapped file, return from the mmapped region.

        result = w_wadfile->mapped + lump->position;
    }
    else if (lump->cache != NULL)
    {
        // Already cached, so just switch the zone tag.

        result = lump->cache;
        /* SATURN: ...unless the lump is PINNED.  This retag is what defeats every attempt to keep
           a hot lump resident: the first R_GetColumn asking for PU_CACHE demotes it and the
           treadmill restarts.  Same defect the resident flat pool exists to fix. */
        if (!W_LumpPinned (lumpnum))
            Z_ChangeTag(lump->cache, tag);
    }
    else
    {
        // Not yet loaded, so load it now

        lump->cache = Z_Malloc(W_LumpLength(lumpnum), tag, &lump->cache);
	W_ReadLump (lumpnum, lump->cache);
        result = lump->cache;
    }
	
    return result;
}



/* SATURN 2026-08-14 -- HEADER-ONLY FETCH, the fix for the measured 46 ms `e`.
**
** R_GenerateLookup needs a patch's `width` + `columnofs[]` (8 + 4*width, ~1 KB) and NOTHING else --
** it never reads a texel.  Serving that through W_CacheLumpNum LZSS-decoded the whole 35 080-byte
** patch: `k12` per patch, ~4 patches per directory rebuild, ~4 rebuilds a frame = the whole hole.
**
** THE HAZARD, and why the scratch buffer is not optional: a prefix is NOT a lump.  If it were
** published into lumpinfo[].cache, the next caller wanting real pixels would get 1 KB of truth
** followed by garbage -- silent, and exactly the shape of the wrong-texture bugs already fought in
** r_data.c.  So this decodes into a private scratch and leaves lump->cache untouched.
**
** LIFETIME: the returned pointer is valid only until the NEXT call.  R_GenerateLookup uses it
** within one loop iteration, which is the only supported pattern.
**
** Returns NULL when it cannot serve a prefix cheaply (mapped WAD, already-cached lump, request too
** big, backend declined) -- the caller then falls back to plain W_CacheLumpNum, which is always
** correct, just slower. */
/* Sized, not rounded: this array is .bss and costs the TLSF pool 1:1 (a 2 KB first cut took the pool
   15.47 -> 13.28 KB).  8 + 4*320 covers the widest patch a Doom WAD can hold; anything beyond falls
   back to W_CacheLumpNum, which is correct, just slower. */
#define W_PREFIX_SCRATCH (8 + 4 * 320 + 8)   /* +8: room for the first column's post header + a texel
					        (R_WallPotatoSeed), still one .bss KB */
static byte w_prefix_buf[W_PREFIX_SCRATCH];

void *W_CacheLumpPrefix (int lumpnum, int nbytes)
{
    lumpinfo_t *lump;

    if ((unsigned)lumpnum >= numlumps) return NULL;
    if (nbytes <= 0 || nbytes > W_PREFIX_SCRATCH) return NULL;

    lump = &lumpinfo[lumpnum];

    /* Already free: a mapped WAD hands back a real pointer, and a resident lump is already paid
       for.  In both cases the FULL lump is correct and costs nothing, so prefer it. */
    if (w_wadfile != NULL && w_wadfile->mapped != NULL)
        return w_wadfile->mapped + lump->position;
    if (lump->cache != NULL)
        return lump->cache;

    if (nbytes > lump->size) nbytes = lump->size;

#ifdef SAT_REPACK
    {
        extern int sat_drp_read_lump_n (unsigned int lump, void *dest, int size, int want);
        if (sat_drp_read_lump_n ((unsigned int)lumpnum, w_prefix_buf, lump->size, nbytes))
            return w_prefix_buf;
    }
#endif

    /* Not in the repack subset: the full-WAD backend has no prefix path (and a CD read is a CD read
       either way), so let the caller take the classic route. */
    return NULL;
}

//
// W_CacheLumpName
//
void *W_CacheLumpName(char *name, int tag)
{
    return W_CacheLumpNum(W_GetNumForName(name), tag);
}

// 
// Release a lump back to the cache, so that it can be reused later 
// without having to read from disk again, or alternatively, discarded
// if we run out of memory.
//
// Back in Vanilla Doom, this was just done using Z_ChangeTag 
// directly, but now that we have WAD mmap, things are a bit more
// complicated ...
//

void W_ReleaseLumpNum(int lumpnum)
{
    lumpinfo_t *lump;

    if ((unsigned)lumpnum >= numlumps)
    {
	I_Error ("W_ReleaseLumpNum: %i >= numlumps", lumpnum);
    }

    lump = &lumpinfo[lumpnum];

    if (w_wadfile->mapped != NULL)
    {
        // Memory-mapped file, so nothing needs to be done here.
    }
    else
    {
        Z_ChangeTag(lump->cache, PU_CACHE);
    }
}

void W_ReleaseLumpName(char *name)
{
    W_ReleaseLumpNum(W_GetNumForName(name));
}

//
// W_PtrIsMapped
// True if `p` points inside a memory-mapped (cart) WAD region.  In SATURN cart mode
// W_CacheLumpNum returns mapped cart pointers, NOT zone blocks, so they must never be
// Z_Free'd (W_ReleaseLumpNum already no-ops them).  Z_Free uses this to turn a stray
// free of a cached lump -- harmless in CD-streaming (the lump is a zone block) but a
// "Z_Free without ZONEID" crash in cart mode -- into the correct no-op.  Inert when
// nothing is mapped (CD-streaming, or a port that buffers the WAD).
//
boolean W_PtrIsMapped(const void *p)
{
    wad_file_t *wf;
    const byte *m;

    if (numlumps == 0 || p == NULL)
        return false;
    wf = w_wadfile;
    if (wf == NULL || wf->mapped == NULL)
        return false;
    m = (const byte *)wf->mapped;
    return (const byte *)p >= m && (const byte *)p < m + wf->length;
}

#if 0

//
// W_Profile
//
int		info[2500][10];
int		profilecount;

void W_Profile (void)
{
    int		i;
    memblock_t*	block;
    void*	ptr;
    char	ch;
    FILE*	f;
    int		j;
    char	name[9];
	
	
    for (i=0 ; i<numlumps ; i++)
    {	
	ptr = lumpinfo[i].cache;
	if (!ptr)
	{
	    ch = ' ';
	    continue;
	}
	else
	{
	    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));
	    if (block->tag < PU_PURGELEVEL)
		ch = 'S';
	    else
		ch = 'P';
	}
	info[i][profilecount] = ch;
    }
    profilecount++;
#if ORIGCODE
    f = fopen ("waddump.txt","w");
    name[8] = 0;

    for (i=0 ; i<numlumps ; i++)
    {
	memcpy (name,lumpinfo[i].name,8);

	for (j=0 ; j<8 ; j++)
	    if (!name[j])
		break;

	for ( ; j<8 ; j++)
	    name[j] = ' ';

	fprintf (f,"%s ",name);

	for (j=0 ; j<profilecount ; j++)
	    fprintf (f,"    %c",info[i][j]);

	fprintf (f,"\n");
    }
    fclose (f);
#endif
}


#endif

// Generate a hash table for fast lookups

void W_GenerateHashTable(void)
{
    unsigned int i;

    // Free the old hash table, if there is one

    if (lumphash != NULL)
    {
        Z_Free(lumphash);
    }

    // Generate hash table
    if (numlumps > 0)
    {
        lumphash = Z_Malloc(sizeof(lumpinfo_t *) * numlumps, PU_STATIC, NULL);
        memset(lumphash, 0, sizeof(lumpinfo_t *) * numlumps);

        for (i=0; i<numlumps; ++i)
        {
            unsigned int hash;

            hash = W_LumpNameHash(lumpinfo[i].name) % numlumps;

            // Hook into the hash table

            lumpinfo[i].next = lumphash[hash];
            lumphash[hash] = &lumpinfo[i];
        }
    }

    // All done!
}

// Lump names that are unique to particular game types. This lets us check
// the user is not trying to play with the wrong executable, eg.
// chocolate-doom -iwad hexen.wad.
static const struct
{
    GameMission_t mission;
    char *lumpname;
} unique_lumps[] = {
    { doom,    "POSSA1" },
    { heretic, "IMPXA1" },
    { hexen,   "ETTNA1" },
    { strife,  "AGRDA1" },
};

void W_CheckCorrectIWAD(GameMission_t mission)
{
    int i;
    int lumpnum;

    for (i = 0; i < arrlen(unique_lumps); ++i)
    {
        if (mission != unique_lumps[i].mission)
        {
            lumpnum = W_CheckNumForName(unique_lumps[i].lumpname);

            if (lumpnum >= 0)
            {
                I_Error("\nYou are trying to use a %s IWAD file with "
                        "the %s%s binary.\nThis isn't going to work.\n"
                        "You probably want to use the %s%s binary.",
                        D_SuggestGameName(unique_lumps[i].mission,
                                          indetermined),
                        PROGRAM_PREFIX,
                        D_GameMissionString(mission),
                        PROGRAM_PREFIX,
                        D_GameMissionString(unique_lumps[i].mission));
            }
        }
    }
}

