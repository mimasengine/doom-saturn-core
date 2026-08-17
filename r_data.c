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
//	Preparation of data for rendering,
//	generation of lookups, caching, retrieval by name.
//

#include <stdio.h>

#include "deh_main.h"
#include "i_swap.h"
#include "i_system.h"
#include "z_zone.h"


#include "w_wad.h"

#include "doomdef.h"
#include "m_misc.h"
#include "r_local.h"
#include "p_local.h"

#include "doomstat.h"
#include "r_sky.h"
#include "r_cache.h"
#include "r_flatcache.h"


#include "r_data.h"

/* SATURN PERF (1.4): -O3 on R_GetColumn was A/B-tested (row-19 REC) and showed
   no measurable gain -- R_GetColumn is not a RECORD-pass bottleneck.  Reverted. */

//
// Graphics.
// DOOM graphics for walls and sprites
// is stored in vertical runs of opaque pixels (posts).
// A column is composed of zero or more posts,
// a patch or sprite is composed of zero or more columns.
// 



//
// Texture definition.
// Each texture is composed of one or more patches,
// with patches being lumps stored in the WAD.
// The lumps are referenced by number, and patched
// into the rectangular texture space using origin
// and possibly other attributes.
//
typedef struct
{
    short	originx;
    short	originy;
    short	patch;
    short	stepdir;
    short	colormap;
} PACKEDATTR mappatch_t;


//
// Texture definition.
// A DOOM wall texture is a list of patches
// which are to be combined in a predefined order.
//
typedef struct
{
    char		name[8];
    int			masked;	
    short		width;
    short		height;
    int                 obsolete;
    short		patchcount;
    mappatch_t	patches[1];
} PACKEDATTR maptexture_t;


// A single patch from a texture definition,
//  basically a rectangular area within
//  the texture rectangle.
typedef struct
{
    // Block origin (allways UL),
    // which has allready accounted
    // for the internal origin of the patch.
    short	originx;	
    short	originy;
    int		patch;
} texpatch_t;


// A maptexturedef_t describes a rectangular texture,
//  which is composed of one or more mappatch_t structures
//  that arrange graphic patches.

typedef struct texture_s texture_t;

struct texture_s
{
    // Keep name for switch changing, etc.
    char	name[8];		
    short	width;
    short	height;

    // Index in textures list

    int         index;

    // Next in hash table chain

    texture_t  *next;
    
    // All the patches[patchcount]
    //  are drawn back to front into the cached texture.
    short	patchcount;
    texpatch_t	patches[1];		
};



int		firstflat;
int		lastflat;
int		numflats;

int		firstpatch;
int		lastpatch;
int		numpatches;

int		firstspritelump;
int		lastspritelump;
int		numspritelumps;

int		numtextures;
texture_t**	textures;
texture_t**     textures_hashtable;


int*			texturewidthmask;
// needed for texture pegging
fixed_t*		textureheight;		
int*			texturecompositesize;
short**			texturecolumnlump;
unsigned short**	texturecolumnofs;
byte**			texturecomposite;

// SATURN crash-proof (garde-COMPOSITE, same lineage as garde-OPENINGS / overflowplane): when a
// multi-patch texture's composite cannot be allocated (zone full/fragmented -- TNT MAP15: 32KB
// PU_STATIC vs lg31K), R_GenerateComposite USED to I_Error-freeze.  Now it bails and publishes this
// shared placeholder column as a sentinel; R_GetColumn serves it (the texture renders as a flat
// placeholder, never a crash/OOB).  256 bytes covers any standard Doom column height; zero-init.
byte			r_column_stub[256];
int			r_composite_ovf = 0;   // # textures stubbed (extern, overlay 'tc')
/* SATURN garde-PATCH: columns served from the placeholder because the zone had no run big enough
   for the whole patch (see R_GetColumn).  Cumulative.  >0 means walls are drawing flat somewhere
   AND that this build would have HALTED before 2026-08-07 -- it is the crash, made measurable. */
int			r_patch_ovf = 0;
/* SATURN 2026-08-14: R_GetColumn calls THIS FRAME that found the single-patch lump non-resident,
   i.e. calls that go to the disc.  Reset in RP_BeginFrame, latched with `g` as row-20 `d`. */
int			r_getcol_disc = 0;
/* SATURN 2026-08-14: textures abandoned by R_GenerateLookup because a column has no patch -- the
   vanilla printf site, which cost 46 ms A PIECE on Saturn.  Cumulative; overlay row 22 `np`.
   Non-zero is NORMAL on TNT/Plutonia; it means those textures render from the composite path. */
int			r_nopatch_col = 0;
/* SATURN 2026-08-08: columns whose composite offset lay OUTSIDE texturecompositesize -- the
   wrong-texture-for-one-frame bug (see the long note in R_GetColumn).  Cumulative; overlay `ob`
   on row 12.  **It must read 0.**  Non-zero means R_GenerateLookup left a directory that does not
   match the composite it sized, and every such column would have read another texture's pixels. */
int			r_composite_oob = 0;
/* SATURN: composites BUILT this ~1 s window.  R_GenerateComposite is a 8..32 KB column copy with
   NO disc read when the patches are still cached -- so the load budget's W_LumpResident predicate
   answers "free" and lets it through.  If `Bp` spikes track this counter, the cost is the REBUILD,
   not the CD, and the fix is to stop the composite being purged (or to key the budget on it too). */
int			r_composite_builds = 0;
/* SATURN 2026-08-14: DISTINCT textures behind those builds, this same window, saturating at 16.
   `cb<builds>/<distinct>` separates the only two worlds left now that the 1p composite pool is dead
   by arithmetic (`xc0/0/60`: the floor rung needs 96 KB contiguous and the level had 60):
     distinct << builds  =>  THRASH -- the same few textures destroyed and rebuilt in a loop.  The
                             work is wasted and something can be won by keeping them alive.
     distinct ~= builds  =>  CHURN -- the player keeps meeting NEW multi-patch textures.  The cost is
                             intrinsic, no cache helps, and the only lever is a cheaper BUILD.
   16 slots because the answer is "a handful" vs "many"; a bitmap over ~1500 texnums would cost the
   TLSF pool 1:1 for a resolution the question does not need. */
int			r_composite_distinct = 0;
static short		r_cd_seen[16];
static int		r_cd_seen_n = 0;

/* SATURN 2026-08-14: worst "useful fraction" of a patch decode this window, in percent -- the last
   column offset R_GenerateComposite actually READS, over the lump length.  It sizes the OTHER lever
   (bounding the decode the way R_GenerateLookup's header-only fetch already does) BEFORE writing it:
     pf 30 => 70 % of every patch decode is thrown away, bound it and take that back for free
     pf 95 => the needed columns run to the end of the patch, bounding it buys nothing.
   A percentage, so a window max is meaningful -- unlike a duration, it cannot be corrupted by being
   read on a different clock than row 20 ([[debug-overlay-legend]]). */
int			r_composite_pf = 0;

void R_CompositeWindowReset (void)
{
    r_composite_builds  = 0;
    r_composite_distinct = 0;
    r_cd_seen_n = 0;
    /* ⚠ `r_composite_pf` is NOT reset here.  This function is called right after row 18 prints, and
       `pf` lives on row 22, which prints LATER in the same overlay block -- resetting it here zeroed
       it before it was ever displayed, and `pf0` on the first seven captures was that bug, not a
       measurement.  Exactly the clobber that made `zb` useless (row 11's `lg` overwrote it).
       Row 22 clears it itself, immediately after printing. */
}

/* ------------------------------------------------------------------------------------------------
   SATURN 2026-08-14 -- COMPOSITE PIN.  Measured: `cb13/6` and `cb20/6` = SIX distinct textures
   rebuilt 13-20 times a second, i.e. each of them destroyed and rebuilt almost every frame, at
   `k33` a piece.  THRASH, and the working set is tiny.

   They are not purged for lack of room (`zf241k` free) but for lack of CONTIGUITY: every ~32 KB
   composite alloc has to carve a run and sweeps its neighbours -- which are the composites just
   built.  Self-sustaining, hence the stable ~5/frame.

   Why this is not the pool that just failed (`lf60`, [[getcolumn-per-call-explosion]]): the slab
   needed 96 KB CONTIGUOUS up front.  This needs none -- the blocks already exist, they merely stop
   being purge victims.  And the clause that makes it safe: if a 48 KB run can no longer be found,
   the whole ring is released at once, so the pin can never cause the ~35 KB sky/face contiguous-OOM
   that killed the pool.  It yields before it can hurt.

   Tag is PU_LEVEL, not PU_STATIC: non-purgeable, but freed by P_SetupLevel's Z_FreeTags -- so a
   level change reclaims every pinned composite with no lifecycle code, and Z_Free NULLs the user
   pointer (`&texturecomposite[tex]`) exactly as the classic purge does.
   ------------------------------------------------------------------------------------------------ */
#define R_CPIN_MAX     8
#define R_CPIN_BUDGET  (64*1024)
#define R_CPIN_FLOOR   (48*1024)

/* DEFAULT 0 since 2026-08-14, and the reason is arithmetic, not taste.  Shipped ON, it yielded on
   essentially every build: seven captures read `pn0/1` .. `pn0/25` -- zero KB held, the ring released
   again and again -- because row-11 `lg` sits at 24-38 KB against the 48 KB floor.  The zone cannot
   spare a 48 KB run, so the pin never engages; worse, `lg` already dips BELOW the ~35 KB a sky/face
   patch needs, so relaxing the floor would trade a real crash risk for a pin that still barely holds.
   Same wall as the 1p slab (`lf60`), reached from the other side.  The chord and the yield counter
   stay as the witness -- if a future change frees real contiguous zone, `pn` will say so. */
int			sat_cpin_on   = 0;   /* live A/B, pad L+Left */
int			r_cpin_kb     = 0;   /* overlay row 22 `pn<kb>/<yields>` */
int			r_cpin_yield  = 0;   /* times the ring was released under pressure */
static int		r_cpin_tex[R_CPIN_MAX];
static byte*		r_cpin_ptr[R_CPIN_MAX];
static int		r_cpin_sz [R_CPIN_MAX];
static int		r_cpin_n     = 0;
static int		r_cpin_bytes = 0;

static void R_CPinDrop (int i)
{
    /* Only retag if the composite is still OURS.  After a level change Z_FreeTags has already freed
       the block and NULLed texturecomposite[tex], so the compare fails and we just drop the slot --
       no touch, no leak, self-healing without a per-level hook. */
    if (texturecomposite[r_cpin_tex[i]] == r_cpin_ptr[i])
	Z_ChangeTag (r_cpin_ptr[i], PU_CACHE);
    r_cpin_bytes -= r_cpin_sz[i];
    for (; i < r_cpin_n - 1; i++)
    {
	r_cpin_tex[i] = r_cpin_tex[i+1];
	r_cpin_ptr[i] = r_cpin_ptr[i+1];
	r_cpin_sz [i] = r_cpin_sz [i+1];
    }
    r_cpin_n--;
    if (r_cpin_bytes < 0) r_cpin_bytes = 0;
    r_cpin_kb = r_cpin_bytes >> 10;
}

void R_CompositePinFlush (void)
{
    while (r_cpin_n)
	R_CPinDrop (0);
    r_cpin_bytes = 0;
    r_cpin_kb    = 0;
}

static void R_CPinAdd (int texnum, byte *block, int size)
{
    int i;

    if (size <= 0 || size > R_CPIN_BUDGET)
	return;				/* one composite bigger than the whole budget: never pin it */

    for (i = 0; i < r_cpin_n; i++)	/* rebuilt in place -> refresh its slot, do not double-count */
	if (r_cpin_tex[i] == texnum) { R_CPinDrop (i); break; }

    while (r_cpin_n && (r_cpin_n >= R_CPIN_MAX || r_cpin_bytes + size > R_CPIN_BUDGET))
	R_CPinDrop (0);			/* oldest out first */

    r_cpin_tex[r_cpin_n] = texnum;
    r_cpin_ptr[r_cpin_n] = block;
    r_cpin_sz [r_cpin_n] = size;
    r_cpin_n++;
    r_cpin_bytes += size;
    r_cpin_kb = r_cpin_bytes >> 10;
    Z_ChangeTag (block, PU_LEVEL);
}

static void R_CompositeNoteDistinct (int texnum)
{
    int i;
    for (i = 0; i < r_cd_seen_n; i++)
	if (r_cd_seen[i] == (short)texnum) return;
    if (r_cd_seen_n < 16)
	r_cd_seen[r_cd_seen_n++] = (short)texnum;
    r_composite_distinct++;	/* keeps counting past 16 even though the set stops growing,
				   so `distinct > 16` still reads as "many" rather than saturating
				   into a number that could be mistaken for thrash */
}
/* (r_composite_n -- the per-FRAME twin added 2026-08-12 -- REMOVED the same day.  It existed to put
   the composite count on the same clock and the same frame as row-20 `g`, and it answered in one
   capture: g30 with b0 (zero composites, 30 ms of R_GetColumn) and g197 with b4 (+167 ms for +4
   composites = 42 ms each, against a 2.8-7.7 ms cost model).  R_GenerateComposite is NOT the hole.
   r_composite_builds above still carries the window count as row-18 `cb`.) */

// for global animation
int*		flattranslation;
int*		texturetranslation;

// needed for pre rendering
fixed_t*	spritewidth;	
fixed_t*	spriteoffset;
fixed_t*	spritetopoffset;

lighttable_t	*colormaps;


//
// MAPTEXTURE_T CACHING
// When a texture is first needed,
//  it counts the number of composite columns
//  required in the texture and allocates space
//  for a column directory and any new columns.
// The directory will simply point inside other patches
//  if there is only one patch in a given column,
//  but any columns with multiple patches
//  will have new column_ts generated.
//



//
// R_DrawColumnInCache
// Clip and draw a column
//  from a patch into a cached post.
//
void
R_DrawColumnInCache
( column_t*	patch,
  byte*		cache,
  int		originy,
  int		cacheheight )
{
    int		count;
    int		position;
    byte*	source;

    while (patch->topdelta != 0xff)
    {
	source = (byte *)patch + 3;
	count = patch->length;
	position = originy + patch->topdelta;

	if (position < 0)
	{
	    count += position;
	    position = 0;
	}

	if (position + count > cacheheight)
	    count = cacheheight - position;

	if (count > 0)
	    memcpy (cache + position, source, count);
		
	patch = (column_t *)(  (byte *)patch + patch->length + 4); 
    }
}



//
// SATURN R4: build a texture's per-column directory lazily + purgeable (defined below,
// after R_GenerateLookup, which it drives).  Replaces the ~157K PU_STATIC upfront directory.
static void R_EnsureLookup (int tex);
/* SATURN 2026-08-14: the three max-timers that split a composite build (row 20 e/a/k). */
void RP_StampBegin (int slot);
void RP_StampEnd (int slot);

// R_GenerateComposite
// Using the texture definition,
//  the composite texture is created from the patches,
//  and each column is cached.
//
void R_GenerateComposite (int texnum)
{
    byte*		block;
    extern int		r_composite_builds;
    int			cached = 0;
    int			pf_maxofs, ofs_used;   /* SATURN: useful-fraction probe, row-22 `pf` */
    texture_t*		texture;
    texpatch_t*		patch;	
    patch_t*		realpatch;
    int			x;
    int			x1;
    int			x2;
    int			i;
    column_t*		patchcol;
    short*		collump;
    unsigned short*	colofs;
	
    texture = textures[texnum];

    // SATURN R4: build the column directory + compositesize if never built or purged, then
    // PIN it PU_STATIC across the composite alloc below (which can purge PU_CACHE) -- we read
    // collump/colofs from it after the alloc.  Unpinned at the end.
    r_composite_builds++;
    R_CompositeNoteDistinct (texnum);
    R_EnsureLookup (texnum);
    Z_ChangeTag (texturecolumnlump[texnum], PU_STATIC);
    Z_ChangeTag (texturecolumnofs[texnum],  PU_STATIC);

    // SATURN: in CD-streaming mode build the composite into the bounded LRU
    // texture cache (recency-evicted, capped) instead of the main zone, so the
    // streaming working set is bounded and the CD reads amortized.  A NULL
    // return (cache inactive / pool full / parallel pass) falls back to the
    // classic main-zone PU_CACHE composite -- i.e. exactly today's behaviour.
    block = R_TexCacheAlloc (texturecompositesize[texnum],
			     (void **)&texturecomposite[texnum]);
    if (block)
    {
	cached = 1;	// pool block; texturecomposite[texnum] already published
    }
    else if (!Z_CanAllocate (texturecompositesize[texnum]))
    {
	// SATURN garde-COMPOSITE: the composite will not fit the zone even after purging PU_CACHE
	// (Z_LargestAllocatable == what Z_Malloc could get).  A Z_Malloc here would I_Error-freeze.
	// Publish the shared stub as a sentinel + bail; R_GetColumn serves the placeholder column.
	texturecomposite[texnum] = r_column_stub;
	r_composite_ovf++;
	Z_ChangeTag (texturecolumnlump[texnum], PU_CACHE);   // unpin (mirrors the tail below)
	Z_ChangeTag (texturecolumnofs[texnum],  PU_CACHE);
	return;
    }
    else
    {
	block = Z_Malloc (texturecompositesize[texnum],
			  PU_STATIC,
			  &texturecomposite[texnum]);
    }

    // SATURN garde-COMPOSITE (patch phase): with the composite now allocated + pinned, verify the
    // BIGGEST source patch will still fit alongside it -- else the W_CacheLumpNum(patch) loop below
    // would I_Error-freeze (the observed TNT 29KB t8 crash: a big texture patch on a fragmented zone).
    // Bail to the placeholder stub instead (giving back the composite block if it was ours).
    {
	int big = 0, k;
	texpatch_t *pp = texture->patches;
	for (k = 0; k < texture->patchcount; k++, pp++)
	{ int pl = W_LumpLength (pp->patch); if (pl > big) big = pl; }
	if (!Z_CanAllocate (big))
	{
	    if (!cached) Z_Free (block);
	    texturecomposite[texnum] = r_column_stub;
	    r_composite_ovf++;
	    Z_ChangeTag (texturecolumnlump[texnum], PU_CACHE);
	    Z_ChangeTag (texturecolumnofs[texnum],  PU_CACHE);
	    return;
	}
    }

    collump = texturecolumnlump[texnum];
    colofs = texturecolumnofs[texnum];

    // Composite the columns together.
    patch = texture->patches;                     /* cleared: `k2..k3` on 2026-08-14 */
		
    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
	/* SATURN garde-PATCH, third and last site (see R_GenerateLookup).  Skip a patch the zone
	   cannot hold rather than I_Error: the composite is already allocated, so the missing patch
	   just leaves its columns as they are -- a partial texture, which is what the neighbouring
	   garde-COMPOSITE already accepts. */
	if (!W_LumpResident (patch->patch)
	    && !Z_CanAllocate (W_LumpLength (patch->patch) + 64))
	{ r_patch_ovf++; continue; }
	realpatch = W_CacheLumpNum (patch->patch, PU_CACHE);
	x1 = patch->originx;
	x2 = x1 + SHORT(realpatch->width);

	if (x1<0)
	    x = 0;
	else
	    x = x1;
	
	if (x2 > texture->width)
	    x2 = texture->width;

	pf_maxofs = 0;
	for ( ; x<x2 ; x++)
	{
	    // Column does not have multiple patches?
	    if (collump[x] >= 0)
		continue;

	    ofs_used = LONG(realpatch->columnofs[x-x1]);
	    if (ofs_used > pf_maxofs) pf_maxofs = ofs_used;   /* SATURN: row-22 `pf` */

	    patchcol = (column_t *)((byte *)realpatch + ofs_used);
	    R_DrawColumnInCache (patchcol,
				 block + colofs[x],
				 patch->originy,
				 texture->height);
	}
	/* SATURN 2026-08-14: how much of this patch the copy actually REACHED.  `+ height + 8` is the
	   slack for the last column's posts; clamped, and kept as the window's worst case.  This sizes
	   a decode bound before anyone writes one -- see r_composite_pf. */
	{
	    int plen = W_LumpLength (patch->patch);
	    if (plen > 0 && pf_maxofs > 0)
	    {
		int pct = (pf_maxofs + texture->height + 8) * 100 / plen;
		if (pct > 100) pct = 100;
		if (pct > r_composite_pf) r_composite_pf = pct;
	    }
	}
    }

    // Classic path: now that the texture is built it is purgable from the zone.
    // Cache-pool blocks are managed by the LRU (R_PostTexCacheFrame), not the
    // zone purger, so they are left alone here.
    if (!cached)
    {
	/* SATURN 2026-08-14: PIN instead of demote, while the zone can still hand out a 48 KB run.
	   Z_CanAllocate early-exits on the first run that fits, so the healthy case is cheap (`zw`
	   measures it: ~0,2 ms a frame).  The moment it cannot, release the WHOLE ring in one go and
	   fall back to the classic purgeable composite -- the pin yields before it can starve the
	   ~35 KB sky/face patches, which is exactly what the 1p slab could not do. */
	if (sat_cpin_on && Z_CanAllocate (R_CPIN_FLOOR))
	    R_CPinAdd (texnum, block, texturecompositesize[texnum]);
	else
	{
	    if (r_cpin_n) { R_CompositePinFlush (); r_cpin_yield++; }
	    Z_ChangeTag (block, PU_CACHE);
	}
    }

    // SATURN R4: unpin the directory -- purgeable again.
    Z_ChangeTag (texturecolumnlump[texnum], PU_CACHE);
    Z_ChangeTag (texturecolumnofs[texnum],  PU_CACHE);
}



//
// R_GenerateLookup
//
void R_GenerateLookup (int texnum)
{
    texture_t*		texture;
    byte*		patchcount;	// patchcount[texture->width]
    texpatch_t*		patch;	
    patch_t*		realpatch;
    int			x;
    int			x1;
    int			x2;
    int			i;
    short*		collump;
    unsigned short*	colofs;
	
    texture = textures[texnum];

    // Composited texture not created yet.
    texturecomposite[texnum] = 0;
    
    texturecompositesize[texnum] = 0;
    collump = texturecolumnlump[texnum];
    colofs = texturecolumnofs[texnum];
    
    // Now count the number of columns
    //  that are covered by more than one patch.
    // Fill in the lump / offset, so columns
    //  with only a single patch are all done.
    patchcount = (byte *) Z_Malloc(texture->width, PU_STATIC, &patchcount);
    memset (patchcount, 0, texture->width);
    /* SATURN 2026-08-07: pre-seed the directory to "use the composite" (-1) BEFORE anything can
       return early.  R_EnsureLookup allocates these two arrays UNINITIALISED and trusts this
       function to fill them, so ANY early exit used to leave garbage that R_GetColumn then fed to
       W_CacheLumpNum -- the `W_CacheLumpNum: 32122 >= numlumps` halt.  That was latent in the
       vanilla "column without a patch" return too; the garde-PATCH bail-out below just made it
       reachable.  Seeded, every early exit degrades to the composite path, which has its own OOM
       sentinel (r_column_stub) -- a flat wall, never a crash. */
    for (x = 0 ; x < texture->width ; x++) { collump[x] = -1; colofs[x] = 0; }
    patch = texture->patches;

    /* SATURN 2026-08-14 (round 4): `e46` survived `np0`, so the printf branch is NEVER TAKEN and my
       printf story was wrong -- the 46 ms is elsewhere inside R_GenerateLookup.  The slots move in:
	 a = this whole patch loop (fetch + the per-column write-back)
	 k = the worst single W_CacheLumpNum(patch->patch) inside it -- the ONLY call left that was
	     never bracketed.  `d`/`ld` argued the disc is out, but `d` counted the OTHER
	     W_CacheLumpNum (R_GetColumn's single-patch return); this one is a different lump.
       a~46 & k~46 => the patch fetch.  a~46 & k~0 => the per-column write-back loop.
       a~0        => the cost is the init loop or the final width loop below. */
    RP_StampBegin (1);
    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
	/* SATURN garde-PATCH (2026-08-07), the site that actually fires.  R4 builds this directory
	   LAZILY, on the first frame that touches the texture -- which is why the halt happens "au
	   chargement" (the owner's own observation; it is what located this after I had guarded
	   R_GetColumn first and the halt came back unchanged).  A 256x128 TNT patch is 35080 B and
	   the zone's longest run is 32 KB, so Z_Malloc I_Errors here and the game is dead.
	   Bail exactly the way the existing "column without a patch" early-return below already
	   does: free the temp, leave the texture without a directory.  R_EnsureLookup retries on a
	   later frame, so the texture builds itself the moment a 35 KB run exists. */
	if (!W_LumpResident (patch->patch)
	    && !Z_CanAllocate (W_LumpLength (patch->patch) + 64))
	{
	    r_patch_ovf++;
	    Z_Free (patchcount);
	    /* 🔴 SATURN 2026-08-16 -- "R_EnsureLookup retries on a later frame" WAS NOT TRUE, and it
	       is the owner's missing wall: *"c'est toujours le même mur qui manque à cet endroit"*.
	       R_EnsureLookup returns early when BOTH directories are non-NULL -- and they are, it
	       allocated them itself two lines before calling us.  So this bail left the PRE-SEEDED
	       directory (collump=-1, colofs=0 = "composite at offset 0") in place with
	       texturecompositesize still 0, PERMANENTLY: R_GetColumn then took the composite path,
	       the out-of-range guard refused every column, and the wall drew as the placeholder for
	       the rest of the level.  Measured `ob26` on three hardware captures out of four,
	       travelling with `px4` exactly as this path predicts.
	       Destroy the directory so the retry the comment promised can actually happen -- the
	       blocks carry registered user pointers, so Z_Free NULLs texturecolumn{lump,ofs}[texnum]
	       and the next R_EnsureLookup rebuilds from scratch.  (Third instance today of the same
	       defect shape: LATCHING A FAILURE. The sky did it, sky_loaded_tex did it, this does it.) */
	    if (texturecolumnlump[texnum]) Z_Free (texturecolumnlump[texnum]);
	    if (texturecolumnofs[texnum])  Z_Free (texturecolumnofs[texnum]);
	    RP_StampEnd (1);
	    return;
	}
	RP_StampBegin (2);
	/* SATURN 2026-08-14 -- HEADER ONLY.  This loop reads `width` and `columnofs[]` and never a
	   texel, but W_CacheLumpNum LZSS-decoded the whole 35 080-byte patch to serve them: measured
	   `k12` per patch, ~4 patches per rebuild, ~4 rebuilds per frame = the whole 46 ms `e`.
	   Two steps because `width` lives inside the header we are trying to size: 8 bytes to read it,
	   then 8 + 4*width for the offset table.  The second decode subsumes the first and both are
	   ~1 KB, so the pair costs ~3 % of one full decode.
	   W_CacheLumpPrefix returns NULL whenever it cannot serve cheaply (mapped WAD, resident lump,
	   oversized request, lump outside the repack subset) -- fall back to the classic path, which is
	   always correct.  The prefix lives in a scratch buffer valid until the next call, which is why
	   it is consumed entirely within this iteration and never stored. */
	realpatch = (patch_t *) W_CacheLumpPrefix (patch->patch, 8);
	if (realpatch)
	{
	    int pw = SHORT(realpatch->width);
	    realpatch = (pw > 0)
		? (patch_t *) W_CacheLumpPrefix (patch->patch, 8 + 4 * pw)
		: NULL;
	}
	if (!realpatch)
	    realpatch = W_CacheLumpNum (patch->patch, PU_CACHE);
	x1 = patch->originx;
	x2 = x1 + SHORT(realpatch->width);
	
	if (x1 < 0)
	    x = 0;
	else
	    x = x1;

	if (x2 > texture->width)
	    x2 = texture->width;
	for ( ; x<x2 ; x++)
	{
	    patchcount[x]++;
	    collump[x] = patch->patch;
	    colofs[x] = LONG(realpatch->columnofs[x-x1])+3;
	}
    }
    RP_StampEnd (1);

    for (x=0 ; x<texture->width ; x++)
    {
	if (!patchcount[x])
	{
	    /* SATURN 2026-08-14 -- THE 46 ms.  Vanilla prints here; on Saturn `printf` goes through
	       newlib to _write (src/syscalls.c), which pushes EVERY CHARACTER through
	       sat_console_putc + a volatile A-bus store at 0x22100001, and the trailing '\n' then
	       calls console_redraw(): 26 rows x SRL::Debug::Print of a 44-char padded string =
	       ~1150 VDP2 VRAM writes, plus a console_scroll once the console is full.  Measured, on
	       g's own frame: `e46` inside `x46648` = 99 % of the worst R_GetColumn call, with `z0`
	       (not the allocator), `a0` (not W_CacheLumpNum) and the control `k3` steady.  ~4 such
	       textures visible per frame accounted for the whole 176 ms `g`.
	       Two independent corroborations: `x` reproduced to 0,3 % across five different frames --
	       the cost is a FIXED 26-row redraw, only the `%s` name varies -- and row-10 `hp1256` is
	       newlib's 1024-byte stdio buffer, which nothing else in this build allocates.
	       TNT textures legitimately have patchless columns, so this fires forever, every frame.
	       Keep the diagnostic, drop the console blit: count it and read it on row 22 as `np`. */
	    r_nopatch_col++;
	    Z_Free (patchcount);   // SATURN R4: the vanilla early-return leaked this PU_STATIC temp;
	    return;                // under lazy rebuilds (M4 purge churn) that leak accretes + fragments
	}
	// I_Error ("R_GenerateLookup: column without a patch");
	
	if (patchcount[x] > 1)
	{
	    // Use the cached block.
	    collump[x] = -1;	
	    colofs[x] = texturecompositesize[texnum];
	    
	    if (texturecompositesize[texnum] > 0x10000-texture->height)
	    {
		I_Error ("R_GenerateLookup: texture %i is >64k",
			 texnum);
	    }
	    
	    texturecompositesize[texnum] += texture->height;
	}
    }

    Z_Free(patchcount);
}


// SATURN R4 (memory diet): lazily build ONE texture's per-column directory.  The full set
// (texturecolumnlump[]/texturecolumnofs[], 4*Sum(width) ~= 157K on Doom II) used to be built
// PU_STATIC at R_InitTextures -- a permanent zone wall that fragmented big-WAD level loads
// (Doom II MAP13 P_LoadSegs OOM: 245K free but no 57K contiguous run).  Now each texture's
// directory is built on first R_GetColumn/R_GenerateComposite and left PU_CACHE (purgeable),
// so Z_Malloc reclaims it under pressure.  Kept PU_STATIC only while R_GenerateLookup fills it
// (its patchcount alloc must not purge the half-built directory), then demoted to PU_CACHE.
/* SATURN 2026-08-10: `r_lookup_rebuilds` (added 08-08, printed as row-20 `e` on 08-09) is GONE, and
   the theory it was built to prove is DEAD.  It was meant to show a directory purge treadmill paying
   for R_GetColumn's measured 500 us/call.  It was then MEASURED on TNT MAP11: e04 with n388 means
   ~17 rebuilds on a 164.7 ms Bp frame, and ONE rebuild is 3 zone allocs + ~384 loop iterations +
   ~1.4 KB written = ~0.18 ms.  17 x 0.18 = ~3 ms of a 130 ms hole -- you would need ~720.
   The cost half of the treadmill story is separately refuted: z_zone.c's z_scan_steps read 0 on this
   same map, so Z_Malloc is NOT walking the whole block list.  The PREMISE (blocks are being purged
   under the render) survives and already has a better witness on screen: row 12 `st`, counted on the
   SLAVE through R_GetColumnCached, which went 0.5% -> 5% of lead spans over the same 8 seconds.
   Row 20 now carries `g` = R_GetColumn's own milliseconds, which is the number that actually sizes
   this.  Do not re-add a rebuild COUNT; if g comes back high, add a per-rebuild ms TIMER instead. */

static void R_EnsureLookup (int tex)
{
    texture_t* t;
    if (texturecolumnlump[tex] && texturecolumnofs[tex])
	return;					  // already resident
    t = textures[tex];
    // Pin BOTH dirs PU_STATIC across R_GenerateLookup.  lump[] and ofs[] are SEPARATE blocks:
    // a purge can free one while the other stays resident (PU_CACHE), so we can arrive here with
    // one NULL and one still live.  The live survivor must ALSO be pinned -- otherwise
    // R_GenerateLookup's internal allocs (patchcount, patch caches) purge it mid-build and the
    // demote-to-PU_CACHE below runs Z_ChangeTag on a freed (nulled) block -> the "block without a
    // ZONEID" fatal seen after running M4 a while (mixed partial-purge state).
    if (!texturecolumnlump[tex])
	texturecolumnlump[tex] = Z_Malloc (t->width*sizeof(**texturecolumnlump), PU_STATIC, &texturecolumnlump[tex]);
    else
	Z_ChangeTag (texturecolumnlump[tex], PU_STATIC);
    if (!texturecolumnofs[tex])
	texturecolumnofs[tex]  = Z_Malloc (t->width*sizeof(**texturecolumnofs),  PU_STATIC, &texturecolumnofs[tex]);
    else
	Z_ChangeTag (texturecolumnofs[tex],  PU_STATIC);
    R_GenerateLookup (tex);			  // fills both + compositesize; safe while PU_STATIC
    /* SATURN 2026-08-16: R_GenerateLookup's garde-PATCH bail now DESTROYS both directories so the
       failure is not latched (see the note there), so either may be NULL on return -- Z_ChangeTag
       on NULL is the "block without a ZONEID" fatal this function's own comment above warns about. */
    if (texturecolumnlump[tex]) Z_ChangeTag (texturecolumnlump[tex], PU_CACHE);
    if (texturecolumnofs[tex])  Z_ChangeTag (texturecolumnofs[tex],  PU_CACHE);
}




//
// R_GetColumn
//
// SATURN PERF 2026-08-08: the public entry is a thin wrapper that brackets the real body, the
// same shape R_StoreWallRange already uses.  The A/B on pad L+X mode 2 (sat_dc_solid: no
// R_GetColumn, no per-pixel texel read) cut the worst Bp from 220.2 to 80.4 ms -- so texturing
// is ~60% of the wall column loop -- but it removed a PER-COLUMN cost and a PER-PIXEL cost in
// one gesture, and those two want completely different fixes.  This separates them: RP_GetCol*
// accumulates ONLY the calls made from inside R_StoreWallRange (the sky column in r_plane.c and
// the masked mid-texture in r_segs.c are billed to P and M, not Bp, and are gated out in
// r_parallel.c).  Read `BP g<%%> n<calls>` on overlay row 20.
//
// The body is NOT trivial and that is the point: on the single-patch path it makes THREE calls
// per column -- R_EnsureLookup (our R4 lazy directory), W_LumpResident (the 2026-08-07 garde),
// and W_CacheLumpNum, whose already-cached branch still rewrites the tag through Z_ChangeTag.
//
void RP_GetColEnter (void);
void RP_GetColLeave (void);
static byte* R_GetColumn_impl (int tex, int col);

byte*
R_GetColumn
( int		tex,
  int		col )
{
    byte *r;
    RP_GetColEnter ();
    r = R_GetColumn_impl (tex, col);
    RP_GetColLeave ();
    return r;
}

static byte*
R_GetColumn_impl
( int		tex,
  int		col )
{
    int		lump;
    int		ofs;
	
    col &= texturewidthmask[tex];
    /* SATURN 2026-08-14 (round 3): THIS call site is the one that was never bracketed.  Round 2 put
       `e` on the R_EnsureLookup INSIDE R_GenerateComposite -- which by then always finds the
       directory resident, because THIS line already rebuilt it.  It duly read 0.  `e0 a0 k2`
       against `x46769` says the whole composite build is 2 ms, so the 46,8 ms is here or at the
       W_CacheLumpNum below, and nowhere else in the body. */
    RP_StampBegin (0);                            /* row-20 `e`: the R4 lazy directory rebuild */
    R_EnsureLookup (tex);   // SATURN R4: build the directory on first use (or after a purge)
    RP_StampEnd (0);
    lump = texturecolumnlump[tex][col];
    ofs = texturecolumnofs[tex][col];
    
    if (lump > 0)
    {
	/* SATURN garde-PATCH (2026-08-07) -- the LAST fatal I_Error left on the render hot path.
	   A single-patch texture is served straight out of its patch, so this caches the WHOLE
	   lump to read one column.  In TNT that is routinely **35080 bytes** (26 lumps are exactly
	   that: every 256x128 patch -- RSKY1/2/3, RWDMON1..10, DO[ENWS][DAY|NITE|HELL], ASPHALT,
	   BIGMURAL, LONGWALL), and the zone cannot always find 35 KB in one run: the owner's halts
	   read `fr225K lg32K` -- 225 KB free, longest run 32 KB, the same layout every boot,
	   because ~366 KB of SMALL unpurgeable blocks (R_InitTextures allocates one PU_STATIC per
	   texture, ~1500 of them in TNT) chop the middle of the zone.  Z_Malloc then I_Errors and
	   the game is DEAD at the loading screen.
	   Sink it like every neighbour already does (garde-COMPOSITE / garde-OPENINGS /
	   garde-VISPLANE / garde-W_ReadLump): serve the shared placeholder column instead.  The
	   wall renders flat for as long as the zone stays that tight and heals by itself the moment
	   a run opens up -- survivable, and above all MEASURABLE, which a halt is not.
	   The test is Z_LargestAllocatable, i.e. free + purgeable after coalescing -- exactly what
	   Z_Malloc's own scan can reach -- so this only fires when the allocation really would
	   fail.  Resident lumps never reach the test. */
	if (!W_LumpResident (lump))
	{
	    /* SATURN 2026-08-14: this is the ONLY branch of R_GetColumn that can reach the disc --
	       W_CacheLumpNum below will W_ReadLump a patch that is routinely 35080 B in TNT.  Counted
	       per FRAME and latched beside `g` as row-20 `d`, because the measured average CD load is
	       ~33 ms (row-12 `t`s / row-0 `ld`): d x 33 ms IS the frame's disc budget, in `g`'s own
	       milliseconds, with no model in between.  `px` (r_patch_ovf) has read 0 on every capture,
	       so the garde below never fires and every one of these calls really does go to the disc. */
	    r_getcol_disc++;
	    if (!Z_CanAllocate (W_LumpLength (lump) + 64))
	    {
		r_patch_ovf++;
		return r_column_stub;
	    }
	}
	return (byte *)W_CacheLumpNum(lump,PU_CACHE)+ofs;   /* cleared: `a0` on 2026-08-14 */
    }

    if (!texturecomposite[tex])
    {
	/* SATURN 2026-08-14 (round 5): `k` retired from the patch fetch, which now reads 0 on every
	   capture -- its question is answered.  Re-pointed at the whole composite build, which is
	   the only unmeasured term left in the body now that `e` is down to 0-6 ms while `x` still
	   reaches 15,7 ms on the residual slow frames.  x - e - k ~ 0 would close the body entirely. */
	RP_StampBegin (2);
	R_GenerateComposite (tex);
	RP_StampEnd (2);
	/* SATURN 2026-08-08 (a): `ofs` was read ABOVE, and R_GenerateComposite re-runs
	   R_EnsureLookup, which can REBUILD the directory -- so the offset in hand may now be
	   stale and point at another column of the freshly-sized composite.  Re-read it. */
	ofs = texturecolumnofs[tex][col];
    }
    else if (sat_texcache_active && texturecomposite[tex] != r_column_stub)
	R_TexCacheTouch (texturecomposite[tex]);   // keep visible composites resident

    if (texturecomposite[tex] == r_column_stub)   // SATURN garde-COMPOSITE: OOM sentinel -> placeholder, no crash/OOB
	return r_column_stub;

    /* SATURN 2026-08-08 (b) -- THE WRONG-TEXTURE-FOR-ONE-FRAME BUG, owner-reported: *"je vois
       parfois les mauvaises textures apparaitre a certains endroits, sur une frame"*, and
       crucially *"c'est une autre VRAIE texture"*.  A real, coherent texture is not a
       use-after-free (that reads whatever took the block: noise); it is an IN-RANGE read of a
       block that is not ours.  Mechanism, all of it in R_GenerateLookup above: it sets
       `texturecompositesize[texnum] = 0`, pre-seeds every column to (-1, 0) = "use the composite
       at offset 0", and then BOTH early returns -- the garde-PATCH one and vanilla's own "column
       without a patch" -- leave that state in place.  R_GetColumn then takes the composite path,
       R_GenerateComposite allocates a ZERO-sized block, and R_DrawColumn reads `height` bytes off
       the end of it, straight into the neighbouring zone block -- which in a zone full of
       composites is very often ANOTHER TEXTURE'S COMPOSITE.  The pre-seed did not create the
       hole (vanilla's early return has the same shape) but it made it reachable and coherent:
       before, those arrays were UNINITIALISED, so the same path gave noise or a crash.
       Cheapest correct answer: refuse the out-of-range read.  The wall draws the placeholder for
       as long as the directory stays inconsistent and heals the moment it is rebuilt properly --
       exactly the contract the neighbouring gardes already have.  Counted as `ob` on row 12. */
    if ((unsigned)ofs + (unsigned)textures[tex]->height
	> (unsigned)texturecompositesize[tex])
    {
	r_composite_oob++;
	return r_column_stub;
    }

    return texturecomposite[tex] + ofs;
}


/* SATURN Potato representative-colour mode: 0 = DOMINANT (most-frequent texel; the default
   Romain chose) ; 1 = AVERAGE (histogram-weighted RGB mean -> nearest palette index).  Both
   modes apply to floors AND walls (R_FlatPotatoColor / R_WallPotatoColor).  Flip to 1 to A/B
   the average in-game later.  (Future idea to test: blend the top 2-3 dominant texels.) */
#ifndef SAT_POTATO_AVG
#define SAT_POTATO_AVG 0
#endif

/* From a 256-bucket texel histogram, return the representative PLAYPAL index: the DOMINANT
   (most-frequent) index, or -- if SAT_POTATO_AVG -- the histogram-weighted RGB average snapped
   to the nearest palette index (PLAYPAL palette 0; lighting is applied later via the colormap,
   so this is the unlit base colour). */
static int R_PotatoRepColor (const int *hist)
{
#if SAT_POTATO_AVG
    static byte *pal = NULL;
    long rs = 0, gs = 0, bs = 0, n = 0, bestd = 0x7fffffffL;
    int  i, r, g, b, best = 0;
    if (!pal) pal = W_CacheLumpName ("PLAYPAL", PU_STATIC);
    for (i = 0; i < 256; i++)
	if (hist[i])
	{
	    rs += (long)pal[i*3+0]*hist[i]; gs += (long)pal[i*3+1]*hist[i];
	    bs += (long)pal[i*3+2]*hist[i]; n  += hist[i];
	}
    if (!n) return 0;
    r = (int)(rs/n); g = (int)(gs/n); b = (int)(bs/n);
    for (i = 0; i < 256; i++)
    {
	int  dr = r-pal[i*3+0], dg = g-pal[i*3+1], db = b-pal[i*3+2];
	long d  = (long)dr*dr + (long)dg*dg + (long)db*db;
	if (d < bestd) { bestd = d; best = i; }
    }
    return best;
#else
    int i, best = 0, bestn = -1;
    for (i = 0; i < 256; i++) if (hist[i] > bestn) { bestn = hist[i]; best = i; }
    return best;
#endif
}

/* SATURN Potato floors: a flat's representative colour as a PLAYPAL index, cached per flat lump.
   Doom flats are always 64x64 = 4096 raw indices; histogram them (subsampled step 2) and pick
   the dominant/average via R_PotatoRepColor -- far truer to the surface than the old arbitrary
   centre texel (2080).  Master-only (R_DrawPlanes) -> the slave reads the cached short, no
   cross-CPU compute.  Pure C, DoomJo-safe. */
static short *flatpot_cache = NULL;   /* SATURN: per-flat dominant colour, -1 = not computed */
static int    flatpot_base = 0, flatpot_count = 0;

/* SATURN: the flat's dominant colour PEEKED -- -1 when never computed, and NEVER loads.  Same
   contract as R_WallPotatoColorPeek: R_FlatPotatoColor below reads the 4 KB flat lump, which in the
   streaming build is the ~42 ms disc read the load budget exists to avoid. */
int R_FlatPotatoColorPeek (int lumpnum)
{
    int fi;
    if (!flatpot_cache) return -1;
    fi = lumpnum - flatpot_base;
    if (fi < 0 || fi >= flatpot_count) return -1;
    return flatpot_cache[fi];
}

int R_FlatPotatoColor (int lumpnum)
{
    short *cache;
    int    base, count;
    int   hist[256];
    byte *src;
    int   i, fi;
    if (!flatpot_cache)
    {
	flatpot_base = firstflat; flatpot_count = numflats;
	flatpot_cache = Z_Malloc (flatpot_count * (int)sizeof(short), PU_STATIC, 0);
	for (i = 0; i < flatpot_count; i++) flatpot_cache[i] = -1;
    }
    cache = flatpot_cache; base = flatpot_base; count = flatpot_count;
    fi = lumpnum - base;
    if (fi < 0 || fi >= count) return 0;
    if (cache[fi] >= 0) return cache[fi];
    /* SATURN: prefer the RESIDENT FLAT POOL slot.  The caller that primes this colour (the load
       budget in r_plane.c) has just pulled the flat into a pool slot, and a pooled flat is NOT in
       lumpinfo[].cache -- so going straight to W_CacheLumpNum here would fire a SECOND ~42 ms disc
       read for a flat already sitting in RAM.  NULL (no pool / not pooled) keeps the classic path. */
    src = R_FlatCachePeek (lumpnum);
    if (!src)
	src = W_CacheLumpNum (lumpnum, PU_STATIC);
    memset (hist, 0, sizeof hist);
    for (i = 0; i < 4096; i += 2) hist[src[i]]++;
    cache[fi] = (short) R_PotatoRepColor (hist);
    return cache[fi];
}

/* SATURN Potato walls: the texture's representative palette index (DOMINANT by default, or
   AVERAGE if SAT_POTATO_AVG, via R_PotatoRepColor), cached per texture, used as the wall's
   single "continuous" colour (one hue for the whole wall, then light-shaded per column by the
   colormap).  Subsampled (step 2) for cheapness; computed lazily on first use and cached.
   sat_wall_color is the global the wall recorder reads (set per wall section in r_segs.c). */
int sat_wall_color = 0;
/* SATURN DEBUG PAINT (owner 2026-08-03: *"est-ce que j'ai un moyen de changer les murs vdp1 ou cpu
   (exclusivement) en flat pour m'assurer que c'est bien a la transition que le mur disparait"*).
   bit0 = every VDP1 wall drawn as a flat GREEN quad (platform side, dg_saturn wall_emit_flat).
   bit1 = every CPU wall drawn as flat RED columns (this file's SAT_WALL_PAINT_CPU, forced through
          the existing Potato-walls executor -- the platform must also set sat_potato_walls).
   With both on, every wall on screen is green or red and NOTHING else, so: which path owns a wall
   is readable at a glance, a wall CHANGING path changes colour on the exact frame it happens, and a
   wall that is drawn by NEITHER is a hole with no texture to hide it.  Debug only, default 0;
   DoomJo never sets it. */
int sat_wall_paint = 0;
#define SAT_WALL_PAINT_CPU 176   /* PLAYPAL: base of the red ramp = bright red */
/* SATURN Potato walls: set per seg in r_segs.c = (the seg's linedef has a special).
   Interactive surfaces (doors, switches, ...) are special lines; keep them TEXTURED
   even in Potato so they stay readable (a flat-grey door in a flat-grey corridor
   is unfindable). */
int sat_wall_textured = 0;
static short *wallpot_cache = NULL;   /* SATURN: per-texture dominant colour, -1 = not computed */

int R_WallPotatoColor (int tex)
{
    int   w, h, col, y, i;
    int   hist[256];
    byte *p;

    if (tex < 0 || tex >= numtextures) return 0;
    if (!wallpot_cache)
    {
	wallpot_cache = Z_Malloc(numtextures * (int)sizeof(short), PU_STATIC, 0);
	for (i = 0; i < numtextures; i++) wallpot_cache[i] = -1;
    }
    /* SATURN 2026-08-15: bit 8 = EXACT (the full dominant-colour walk below).  A cheap seed from
       R_WallPotatoSeed stores the colour with bit 8 CLEAR, so it serves distant walls immediately
       and still gets upgraded the first time the texture is genuinely drawn up close. */
    if (wallpot_cache[tex] >= 0 && (wallpot_cache[tex] & 0x100))
	return wallpot_cache[tex] & 0xff;

    memset (hist, 0, sizeof hist);
    w = texturewidthmask[tex] + 1;
    h = textureheight[tex] >> FRACBITS;
    for (col = 0; col < w; col += 2)
    {
	p = R_GetColumn(tex, col);
	for (y = 0; y < h; y += 2) hist[p[y]]++;
    }
    wallpot_cache[tex] = (short) (R_PotatoRepColor (hist) | 0x100);
    return wallpot_cache[tex] & 0xff;
}

/* SATURN 2026-08-15 -- CHEAP POTATO SEED, the owner's question answered: *"pour avoir la couleur
   dominante, il faut avoir lu la texture non ? et si on lisait le premier pixel à la place ?"*
   Yes -- and worse than reading it: R_WallPotatoColor above walks every other column through
   R_GetColumn, which BUILDS THE COMPOSITE (row-20 `k31`).  Calling it to colour a wall we have just
   decided NOT to texture would pay the exact cost the LOD exists to avoid.
   So take one texel out of the FIRST PATCH, with no composite and no full decode: a patch's first
   column starts right after `8 + 4*width`, so a ~1,3 KB prefix carries the header, the offset table
   and the first post -- the same W_CacheLumpPrefix machinery that killed the 46 ms LZSS storm.
   ~0,4 ms, once per texture, and it self-heals: bit 8 stays clear, so the first close-up textured
   draw replaces it with the true dominant colour.
   Returns -1 when it cannot look cheaply (no patch, oversized offset table, empty first column) --
   the caller then uses the neutral index and counts it (`nocol`). */
int R_WallPotatoSeed (int tex)
{
    texture_t  *t;
    patch_t    *ph;
    const byte *post;
    int         lump, w, ofs, want, i;

    if (tex < 0 || tex >= numtextures) return -1;
    if (!wallpot_cache)
    {
	wallpot_cache = Z_Malloc(numtextures * (int)sizeof(short), PU_STATIC, 0);
	for (i = 0; i < numtextures; i++) wallpot_cache[i] = -1;
    }
    if (wallpot_cache[tex] >= 0) return wallpot_cache[tex] & 0xff;

    t = textures[tex];
    if (!t || t->patchcount <= 0) return -1;
    lump = t->patches[0].patch;

    ph = (patch_t *) W_CacheLumpPrefix (lump, 8);          /* just the header, for `width` */
    if (!ph) return -1;
    w = SHORT(ph->width);
    if (w <= 0) return -1;

    want = 8 + 4 * w + 8;                                  /* + one post header + one texel */
    ph = (patch_t *) W_CacheLumpPrefix (lump, want);
    if (!ph) return -1;

    ofs = LONG(ph->columnofs[0]);
    if (ofs < 8 + 4 * w || ofs + 4 > want) return -1;      /* first column outside what we decoded */
    post = (const byte *)ph + ofs;
    if (post[0] == 0xff || post[1] == 0) return -1;        /* column empty -> no honest colour */

    wallpot_cache[tex] = (short)(post[3] & 0xff);          /* topdelta, length, pad, then texels */
    return wallpot_cache[tex] & 0xff;                      /* bit 8 clear = CHEAP, upgradable */
}

/* SATURN: the dominant colour PEEKED -- returns it if already computed, -1 otherwise, and NEVER
   loads anything.  R_WallPotatoColor above walks every other COLUMN through R_GetColumn, i.e. it
   faults the WHOLE texture in: calling it on a non-resident texture would perform exactly the disc
   read the flat fallback exists to avoid.  So there is no way to know the right colour for a
   texture that has never been seen -- the caller must supply a neutral one and count the case
   (r_segs.c `sat_wall_flat_nocol`).  It self-heals: the first time the texture IS drawn textured
   the colour lands in the cache and every later flat fallback for it is exact.  Baking a 1-byte
   per-texture table offline into the repack would remove even the first-sighting case. */
int R_WallPotatoColorPeek (int tex)
{
    if (tex < 0 || tex >= numtextures || !wallpot_cache) return -1;
    if (wallpot_cache[tex] < 0) return -1;
    return wallpot_cache[tex] & 0xff;   /* strip the EXACT bit; a cheap seed answers just as well */
}

/* SATURN 2026-08-09 -- ALLOCATION-FREE, SLAVE-SAFE column fetch.  R_GetColumn cannot be used off
   the master or across a frame: it calls R_EnsureLookup / R_GenerateComposite / W_CacheLumpNum,
   all of which Z_Malloc, and Z_Malloc purges PU_CACHE blindly.
   THE BUG THIS EXISTS FOR (owner-confirmed by A/B on pad R+Right, row 13 `L1s` <-> `L1-`):
   the LEAD-FILL recorded a RAW `dc_source` pointer during the BSP walk (r_segs.c) and the SLAVE
   dereferenced it a whole frame later, concurrently with the master's R_DrawPlanes.  Every
   allocation in between -- the rest of the wall loop, then every flat R_DrawPlanes loads -- could
   purge the very block it pointed into, and the freed run was immediately handed to the next
   texture.  `sp->src` is not a registered zone user pointer, so nothing NULLed it: the span drew
   an IN-RANGE read of a fully-built NEIGHBOUR = "une autre VRAIE texture", changing every frame,
   only under zone pressure, and tripping NO counter (every existing garde validates at
   R_GetColumn time, none at drain time).  It was 1p-only because the drain is.
   Contract: read-only, never allocates, never touches the texture cache, and answers NULL for any
   doubt -- purged directory, purged/stubbed composite, non-resident lump, out-of-range offset.
   NULL means "draw this span flat", never "read it anyway". */
const byte* R_GetColumnCached (int tex, int col)
{
    int   lump, ofs;
    byte *p;

    if ((unsigned)tex >= (unsigned)numtextures) return NULL;
    col &= texturewidthmask[tex];
    /* Both directories are PU_CACHE with REGISTERED user pointers (R_EnsureLookup), so a purge
       NULLs the slot we are about to read -- that is what makes this test sound.  They are also
       two SEPARATE blocks: one can be gone while the other lives, hence both tests. */
    if (!texturecolumnlump[tex] || !texturecolumnofs[tex])
	return NULL;
    lump = texturecolumnlump[tex][col];
    ofs  = texturecolumnofs[tex][col];

    if (lump > 0)				  /* single-patch column: served from the patch */
    {
	p = (byte *)W_LumpCached (lump);	  /* resident-or-NULL; never reads the disc */
	return p ? p + ofs : NULL;
    }

    if (!texturecomposite[tex] || texturecomposite[tex] == r_column_stub)
	return NULL;				  /* purged, or the OOM sentinel */
    if ((unsigned)ofs + (unsigned)textures[tex]->height
	> (unsigned)texturecompositesize[tex])
	return NULL;				  /* same garde as R_GetColumn's `ob` */
    return texturecomposite[tex] + ofs;
}

/* SATURN: 1 = drawing this texture costs NO disc I/O this frame.  Conservative -- any doubt
   (directory purged, composite gone, a patch not cached) answers 0, so the budget errs toward
   drawing flat rather than toward a surprise 42 ms read inside the wall loop. */
int R_TextureIOFree (int tex)
{
    texture_t *t;
    int        i;

    if (tex <= 0 || tex >= numtextures) return 1;    /* 0 = "no texture" -> nothing to load */
    if (!texturecolumnlump[tex]) return 0;           /* R4 directory purged -> R_EnsureLookup works */
    if (texturecomposite[tex])   return 1;           /* composite resident (or the OOM stub) */
    t = textures[tex];
    for (i = 0; i < t->patchcount; i++)              /* single-patch columns come straight from these,
						        and a composite build would read them ALL */
	if (!W_LumpResident (t->patches[i].patch)) return 0;
    return 1;
}


static void GenerateTextureHashTable(void)
{
    texture_t **rover;
    int i;
    int key;

    textures_hashtable 
            = Z_Malloc(sizeof(texture_t *) * numtextures, PU_STATIC, 0);

    memset(textures_hashtable, 0, sizeof(texture_t *) * numtextures);

    // Add all textures to hash table

    for (i=0; i<numtextures; ++i)
    {
        // Store index

        textures[i]->index = i;

        // Vanilla Doom does a linear search of the texures array
        // and stops at the first entry it finds.  If there are two
        // entries with the same name, the first one in the array
        // wins. The new entry must therefore be added at the end
        // of the hash chain, so that earlier entries win.

        key = W_LumpNameHash(textures[i]->name) % numtextures;

        rover = &textures_hashtable[key];

        while (*rover != NULL)
        {
            rover = &(*rover)->next;
        }

        // Hook into hash table

        textures[i]->next = NULL;
        *rover = textures[i];
    }
}


// SATURN Phase-0 measurement (docs/TEXTURECOLUMNLUMP_PLAN.md): the unconditional
// PU_STATIC per-column directory floor, measured once at load, shown on the overlay
// (TEX row).  Confirms the ~400-600 KB estimate on the real shipping WADs before
// committing to the composite-on-demand refactor.
int sat_tex_numtex   = 0;   // numtextures
int sat_tex_sumwidth = 0;   // Sum of texture widths = total columns
int sat_tex_dirbytes = 0;   // texturecolumnlump + texturecolumnofs bytes = 4 * sumwidth (the floor)
int sat_tex_mptex    = 0;   // # multi-patch textures (patchcount > 1) -> need a composite
int sat_tex_mpwidth  = 0;   // Sum width of multi-patch textures (Option-E whole-slab size proxy)

//
// R_InitTextures
// Initializes the texture list
//  with the textures from the world map.
//
void R_InitTextures (void)
{
    maptexture_t*	mtexture;
    texture_t*		texture;
    mappatch_t*		mpatch;
    texpatch_t*		patch;

    int			i;
    int			j;

    int*		maptex;
    int*		maptex2;
    int*		maptex1;
    byte*		tex_slab = NULL;   /* SATURN: one block for all texture_t (see below) */
    byte*		tex_slab_end = NULL;

    char		name[9];
    char*		names;
    char*		name_p;
    
    int*		patchlookup;
    
    int			totalwidth;
    int			nummappatches;
    int			offset;
    int			maxoff;
    int			maxoff2;
    int			numtextures1;
    int			numtextures2;

    int*		directory;
    
    int			temp1;
    int			temp2;
    int			temp3;

    
    // Load the patch names from pnames.lmp.
    name[8] = 0;
    names = W_CacheLumpName (DEH_String("PNAMES"), PU_STATIC);
    nummappatches = LONG ( *((int *)names) );
    name_p = names + 4;
    patchlookup = Z_Malloc(nummappatches*sizeof(*patchlookup), PU_STATIC, NULL);

    for (i = 0; i < nummappatches; i++)
    {
        M_StringCopy(name, name_p + i * 8, sizeof(name));
        patchlookup[i] = W_CheckNumForName(name);
    }
    W_ReleaseLumpName(DEH_String("PNAMES"));

    // Load the map texture definitions from textures.lmp.
    // The data is contained in one or two lumps,
    //  TEXTURE1 for shareware, plus TEXTURE2 for commercial.
    maptex = maptex1 = W_CacheLumpName (DEH_String("TEXTURE1"), PU_STATIC);
    numtextures1 = LONG(*maptex);
    maxoff = W_LumpLength (W_GetNumForName (DEH_String("TEXTURE1")));
    directory = maptex+1;
	
    if (W_CheckNumForName (DEH_String("TEXTURE2")) != -1)
    {
	maptex2 = W_CacheLumpName (DEH_String("TEXTURE2"), PU_STATIC);
	numtextures2 = LONG(*maptex2);
	maxoff2 = W_LumpLength (W_GetNumForName (DEH_String("TEXTURE2")));
    }
    else
    {
	maptex2 = NULL;
	numtextures2 = 0;
	maxoff2 = 0;
    }
    numtextures = numtextures1 + numtextures2;
	
    textures = Z_Malloc (numtextures * sizeof(*textures), PU_STATIC, 0);
    texturecolumnlump = Z_Malloc (numtextures * sizeof(*texturecolumnlump), PU_STATIC, 0);
    texturecolumnofs = Z_Malloc (numtextures * sizeof(*texturecolumnofs), PU_STATIC, 0);
    texturecomposite = Z_Malloc (numtextures * sizeof(*texturecomposite), PU_STATIC, 0);
    texturecompositesize = Z_Malloc (numtextures * sizeof(*texturecompositesize), PU_STATIC, 0);
    texturewidthmask = Z_Malloc (numtextures * sizeof(*texturewidthmask), PU_STATIC, 0);
    textureheight = Z_Malloc (numtextures * sizeof(*textureheight), PU_STATIC, 0);

    totalwidth = 0;
    
    //	Really complex printing shit...
    temp1 = W_GetNumForName (DEH_String("S_START"));  // P_???????
    temp2 = W_GetNumForName (DEH_String("S_END")) - 1;
    temp3 = ((temp2-temp1+63)/64) + ((numtextures+63)/64);

    // If stdout is a real console, use the classic vanilla "filling
    // up the box" effect, which uses backspace to "step back" inside
    // the box.  If stdout is a file, don't draw the box.

    if (I_ConsoleStdout())
    {
        printf("[");
        for (i = 0; i < temp3 + 9; i++)
            printf(" ");
        printf("]");
        for (i = 0; i < temp3 + 10; i++)
            printf("\b");
    }
	
    /* SATURN 2026-08-07: ONE SLAB for every texture_t, instead of one Z_Malloc per texture.
       ~1500 textures in TNT = ~1500 separate PU_STATIC blocks, and THAT is the ~240 KB of small
       unpurgeable blocks that chop the middle of the zone.  Measured consequence: the longest free
       run sat at 19-38 KB all session, so a 256x128 patch (35080 B) could not be cached (the
       garde-PATCH halt) AND r_flatcache never carved -- its smallest rung needs 48 KB, so the
       resident flat pool shipped this morning read `p0` in every single capture.
       Two passes over the SAME directory walk: sum, then carve pointers into one block.  Byte-
       identical contents, identical API, but the zone sees 1 block instead of ~1500 -- and every
       Z_Malloc stops walking them.  Judge it on LIM `lg` and on FLT `p`.
       Fails soft: if the slab cannot be allocated, tex_slab stays NULL and the per-texture
       Z_Malloc below runs exactly as before. */
    {
	int*  dscan = directory;
	int*  mscan = maptex;
	int   moff  = maxoff;
	long  need  = 0;
	for (i=0 ; i<numtextures ; i++, dscan++)
	{
	    maptexture_t* mt;
	    int off;
	    if (i == numtextures1) { mscan = maptex2; moff = maxoff2; dscan = mscan+1; }
	    off = LONG(*dscan);
	    if (off > moff) break;                      /* corrupt -> let the main loop I_Error */
	    mt = (maptexture_t *)((byte *)mscan + off);
	    need += (sizeof(texture_t) + sizeof(texpatch_t)*(SHORT(mt->patchcount)-1) + 3) & ~3L;
	}
	if (i == numtextures && need > 0 && Z_LargestAllocatable() > need + 128*1024)
	{
	    tex_slab     = (byte *)Z_Malloc((int)need, PU_STATIC, 0);
	    tex_slab_end = tex_slab + need;
	}
    }

    for (i=0 ; i<numtextures ; i++, directory++)
    {
	if (!(i&63))
	    printf (".");

	if (i == numtextures1)
	{
	    // Start looking in second texture file.
	    maptex = maptex2;
	    maxoff = maxoff2;
	    directory = maptex+1;
	}
		
	offset = LONG(*directory);

	if (offset > maxoff)
	    I_Error ("R_InitTextures: bad texture directory");
	
	mtexture = (maptexture_t *) ( (byte *)maptex + offset);

	{   /* SATURN: carve from the slab (see above); fall back to a private block if there is
	       none, or -- defensively -- if the slab ran short of what the sizing pass computed. */
	    int tsz = (sizeof(texture_t)
		       + sizeof(texpatch_t)*(SHORT(mtexture->patchcount)-1) + 3) & ~3;
	    if (tex_slab && tex_slab + tsz <= tex_slab_end)
	    { texture = textures[i] = (texture_t *)tex_slab; tex_slab += tsz; }
	    else
		texture = textures[i] = Z_Malloc (tsz, PU_STATIC, 0);
	}
	
	texture->width = SHORT(mtexture->width);
	texture->height = SHORT(mtexture->height);
	texture->patchcount = SHORT(mtexture->patchcount);
	
	memcpy (texture->name, mtexture->name, sizeof(texture->name));
	mpatch = &mtexture->patches[0];
	patch = &texture->patches[0];

	for (j=0 ; j<texture->patchcount ; j++, mpatch++, patch++)
	{
	    patch->originx = SHORT(mpatch->originx);
	    patch->originy = SHORT(mpatch->originy);
	    patch->patch = patchlookup[SHORT(mpatch->patch)];
	    if (patch->patch == -1)
	    {
		I_Error ("R_InitTextures: Missing patch in texture %s",
			 texture->name);
	    }
	}		
	// SATURN R4 (memory diet): the per-texture column directory (4*Sum(width) ~= 157K PU_STATIC
	// on Doom II) is NOT built up front any more -- it fragmented big-WAD level loads
	// (P_LoadSegs OOM).  Left lazy; built purgeable on first use by R_EnsureLookup.
	texturecolumnlump[i]    = 0;
	texturecolumnofs[i]     = 0;
	texturecomposite[i]     = 0;
	texturecompositesize[i] = 0;

	j = 1;
	while (j*2 <= texture->width)
	    j<<=1;

	texturewidthmask[i] = j-1;
	textureheight[i] = texture->height<<FRACBITS;
		
	totalwidth += texture->width;
	if (texture->patchcount > 1)     // SATURN Phase-0: multi-patch => needs a composite
	{ sat_tex_mptex++; sat_tex_mpwidth += texture->width; }
    }

    // SATURN Phase-0 measurement: the per-column directory floor (4 * Sum width).
    sat_tex_numtex   = numtextures;
    sat_tex_sumwidth = totalwidth;
    sat_tex_dirbytes = totalwidth * 4;   // texturecolumnlump(2) + texturecolumnofs(2) per column

    Z_Free(patchlookup);

    W_ReleaseLumpName(DEH_String("TEXTURE1"));
    if (maptex2)
        W_ReleaseLumpName(DEH_String("TEXTURE2"));
    
    // Precalculate whatever possible.	

    // SATURN R4: per-texture column directories are built lazily now (R_EnsureLookup, on first
    // R_GetColumn/R_GenerateComposite), not all up front -- see the per-texture init above.
    
    // Create translation table for global animation.
    texturetranslation = Z_Malloc ((numtextures+1)*sizeof(*texturetranslation), PU_STATIC, 0);
    
    for (i=0 ; i<numtextures ; i++)
	texturetranslation[i] = i;

    GenerateTextureHashTable();
}



//
// R_InitFlats
//
void R_InitFlats (void)
{
    int		i;
	
    firstflat = W_GetNumForName (DEH_String("F_START")) + 1;
    lastflat = W_GetNumForName (DEH_String("F_END")) - 1;
    numflats = lastflat - firstflat + 1;
	
    // Create translation table for global animation.
    flattranslation = Z_Malloc ((numflats+1)*sizeof(*flattranslation), PU_STATIC, 0);
    
    for (i=0 ; i<numflats ; i++)
	flattranslation[i] = i;
}


//
// R_InitSpriteLumps
// Finds the width and hoffset of all sprites in the wad,
//  so the sprite does not need to be cached completely
//  just for having the header info ready during rendering.
//
void R_InitSpriteLumps (void)
{
    int		i;
    patch_t	*patch;
	
    firstspritelump = W_GetNumForName (DEH_String("S_START")) + 1;
    lastspritelump = W_GetNumForName (DEH_String("S_END")) - 1;
    
    numspritelumps = lastspritelump - firstspritelump + 1;
    spritewidth = Z_Malloc (numspritelumps*sizeof(*spritewidth), PU_STATIC, 0);
    spriteoffset = Z_Malloc (numspritelumps*sizeof(*spriteoffset), PU_STATIC, 0);
    spritetopoffset = Z_Malloc (numspritelumps*sizeof(*spritetopoffset), PU_STATIC, 0);

#ifdef SAT_REPACK
    // SATURN R3.1 boot index (STREAMING_FLUIDITY_ROADMAP.md §6): if the DOOMRP.DRP
    // carries a precomputed sprite-header section, fill all three arrays from one
    // sequential read instead of caching every sprite lump (in CD-streaming mode
    // that is ~1381 CD reads for 6 useful header bytes each).  Fails closed: absent
    // section / stale count / read error -> fall through to the classic loop below.
    {
        extern int sat_drp_sprite_headers (int *w, int *lo, int *to, int n);
        if (sat_drp_sprite_headers (spritewidth, spriteoffset, spritetopoffset,
                                    numspritelumps))
        {
            // Trust-but-verify: cross-check the first and last entries against the
            // real patch headers (2 lump reads instead of numspritelumps).  A
            // container/decode bug fails loudly here and the classic loop rebuilds.
            boolean ok = true;
            for (i = 0; i < 2 && ok; i++)
            {
                int k = i ? numspritelumps - 1 : 0;
                patch = W_CacheLumpNum (firstspritelump + k, PU_CACHE);
                ok = spritewidth[k]     == (SHORT(patch->width)      << FRACBITS)
                  && spriteoffset[k]    == (SHORT(patch->leftoffset) << FRACBITS)
                  && spritetopoffset[k] == (SHORT(patch->topoffset)  << FRACBITS);
            }
            if (ok)
                return;
            printf ("R3.1 sprite index MISMATCH -> classic rebuild\n");
        }
    }
#endif

    /* SATURN 2026-08-07: CHUNKED header sweep -- the same three arrays, but one 32 KB read covers
       10-30 consecutive sprite lumps instead of one CD command per lump.  MEASURED: the boot+load
       costs 4704 CD commands / 270 s at ~40-57 ms each (overlay row 12 `L`), and only 73 lumps of
       that are inside P_SetupLevel (`S73`) -- the boot owns it, and THIS loop is its biggest single
       contributor (~1381 commands on TNT, for 6 useful bytes each; the comment above the R3.1 block
       already said so).  Unlike R3.1 this needs NO .DRP, so it works on any WAD.  Fails closed:
       W_ReadHeaderSweep returns 0 (zone too tight, short read, lumps out of file order) and the
       classic per-lump loop below runs unchanged. */
    /* (The WAD-independent chunked sweep that used to sit here is REMOVED, 2026-08-07: the R3.1
       .DRP index above covers the same ~1381 reads, `-Repack` is now a standing build rule
       ([[drp-repack-must-be-rebuilt]]), and the HWRAM TLSF pool could not carry both.  Re-add
       W_ReadHeaderSweep (w_wad.c history) if a WAD ever ships without a .DRP.) */
    for (i=0 ; i< numspritelumps ; i++)
    {
	if (!(i&63))
	    printf (".");

	patch = W_CacheLumpNum (firstspritelump+i, PU_CACHE);
	spritewidth[i] = SHORT(patch->width)<<FRACBITS;
	spriteoffset[i] = SHORT(patch->leftoffset)<<FRACBITS;
	spritetopoffset[i] = SHORT(patch->topoffset)<<FRACBITS;
    }
}



/* SATURN 2026-08-07: R_PinSkyPatch is REMOVED -- it worked, and it was the wrong idea.
   MEASURED on the HW halt dump: the sky patch WAS pinned (`34K t5 @968K`, PU_LEVEL, resident) and
   the Zmalloc still failed on another 35104-byte request.  Cause: **26 lumps in TNT are exactly
   35080 bytes** -- every 256x128 patch (RSKY1/2/3, RWDMON1..10, DO[ENWS][DAY|NITE|HELL], ASPHALT,
   BIGMURAL, LONGWALL) -- so an outdoor TNT map cycles SEVERAL of them and the sky is just the one
   I happened to identify first.  Pinning them all is 800 KB.
   And the accounting got WORSE: the pin converts 34 KB of PURGEABLE into 34 KB of permanent
   PU_LEVEL (`lv` 377K -> 410K) while `lg` did not move at all (32K both sides).  In a zone with
   190 KB free but never 34 KB contiguous, that is a pure loss.
   LESSON: this class of failure is about the NUMBER OF CUTS, not the size of any one block.  Do
   not pin another lump to fix it.  (The W_PinLump machinery in w_wad.c stays -- the no-demote
   guard it adds to W_CacheLumpNum is the correct fix for a real trap -- but it has no user.) */


//
// R_InitColormaps
//
void R_InitColormaps (void)
{
    int	lump;

    // Load in the light tables,
    //  256 byte align tables.
    lump = W_GetNumForName(DEH_String("COLORMAP"));
    colormaps = W_CacheLumpNum(lump, PU_STATIC);
    /* SATURN: COLORMAP lump is a direct pointer into cart RAM (A-Bus, 16-bit,
       ~22.9 MHz), shared between master and slave SH-2.  Both column renderers
       do TWO A-Bus reads per pixel: one for the texture byte and one for the
       colormap lookup.  Copy the 8704-byte table to a static buffer in high
       WRAM (32-bit bus, ~28.6 MHz, no inter-CPU contention) so the colormap
       lookup is served from cache instead of competing for the A-Bus. */
    {
        static byte saturn_cmap[34 * 256];   /* 8704 B in high-WRAM BSS */
        int sz = (int)W_LumpLength(lump);
        if (sz > (int)sizeof(saturn_cmap)) sz = (int)sizeof(saturn_cmap);
        memcpy(saturn_cmap, colormaps, sz);
        colormaps = saturn_cmap;
    }
    /* SATURN: the lump was copied to high-WRAM BSS above and `colormaps` now points
       there, so the original PU_STATIC zone copy is dead.  Release it (-> PU_CACHE,
       reclaimable) instead of leaking ~8.7K pinned for the whole session. */
    W_ReleaseLumpNum(lump);
}



//
// R_InitData
// Locates all the lumps
//  that will be used by all views
// Must be called after W_Init.
//
void R_InitData (void)
{
    R_InitTextures ();
    printf (".");
    R_InitFlats ();
    printf (".");
    R_InitSpriteLumps ();
    printf (".");
    R_InitColormaps ();
}



//
// R_FlatNumForName
// Retrieval, get a flat number for a flat name.
//
int R_FlatNumForName (char* name)
{
    int		i;
    char	namet[9];

    i = W_CheckNumForName (name);

    if (i == -1)
    {
	namet[8] = 0;
	memcpy (namet, name,8);
	I_Error ("R_FlatNumForName: %s not found",namet);
    }
    return i - firstflat;
}




//
// R_CheckTextureNumForName
// Check whether texture is available.
// Filter out NoTexture indicator.
//
int	R_CheckTextureNumForName (char *name)
{
    texture_t *texture;
    int key;

    // "NoTexture" marker.
    if (name[0] == '-')		
	return 0;
		
    key = W_LumpNameHash(name) % numtextures;

    texture=textures_hashtable[key]; 
    
    while (texture != NULL)
    {
	if (!strncasecmp (texture->name, name, 8) )
	    return texture->index;

        texture = texture->next;
    }
    
    return -1;
}



//
// R_TextureNumForName
// Calls R_CheckTextureNumForName,
//  aborts with error message.
//
int	R_TextureNumForName (char* name)
{
    int		i;
	
    i = R_CheckTextureNumForName (name);

    if (i==-1)
    {
	I_Error ("R_TextureNumForName: %s not found",
		 name);
    }
    return i;
}




//
// R_PrecacheLevel
// Preloads all relevant graphics for the level.
//
int		flatmemory;
int		texturememory;
int		spritememory;

void R_PrecacheLevel (void)
{
    char*		flatpresent;
    char*		texturepresent;
    char*		spritepresent;

    int			i;
    int			j;
    int			k;
    int			lump;
    
    texture_t*		texture;
    thinker_t*		th;
    spriteframe_t*	sf;

    if (demoplayback)
	return;
    
    // Precache flats.
    flatpresent = Z_Malloc(numflats, PU_STATIC, NULL);
    memset (flatpresent,0,numflats);	

    for (i=0 ; i<numsectors ; i++)
    {
	flatpresent[sectors[i].floorpic] = 1;
	flatpresent[sectors[i].ceilingpic] = 1;
    }
	
    flatmemory = 0;

    for (i=0 ; i<numflats ; i++)
    {
	if (flatpresent[i])
	{
	    lump = firstflat + i;
	    flatmemory += lumpinfo[lump].size;
	    W_CacheLumpNum(lump, PU_CACHE);
	}
    }

    Z_Free(flatpresent);
    
    // Precache textures.
    texturepresent = Z_Malloc(numtextures, PU_STATIC, NULL);
    memset (texturepresent,0, numtextures);
	
    for (i=0 ; i<numsides ; i++)
    {
	texturepresent[sides[i].toptexture] = 1;
	texturepresent[sides[i].midtexture] = 1;
	texturepresent[sides[i].bottomtexture] = 1;
    }

    // Sky texture is always present.
    // Note that F_SKY1 is the name used to
    //  indicate a sky floor/ceiling as a flat,
    //  while the sky texture is stored like
    //  a wall texture, with an episode dependend
    //  name.
    texturepresent[skytexture] = 1;
	
    texturememory = 0;
    for (i=0 ; i<numtextures ; i++)
    {
	if (!texturepresent[i])
	    continue;

	texture = textures[i];

	for (j=0 ; j<texture->patchcount ; j++)
	{
	    lump = texture->patches[j].patch;
	    texturememory += lumpinfo[lump].size;
	    W_CacheLumpNum(lump , PU_CACHE);
	}

	/* SATURN: precompute the Potato-walls dominant colour here (level load,
	   under the loading screen) so enabling Potato walls in-game -- or a future
	   fps-adaptive switch -- doesn't hitch on the first frame. */
	R_WallPotatoColor (i);
    }

    Z_Free(texturepresent);
    
    // Precache sprites.
    spritepresent = Z_Malloc(numsprites, PU_STATIC, NULL);
    memset (spritepresent,0, numsprites);
	
    for (th = thinkercap.next ; th != &thinkercap ; th=th->next)
    {
	if (th->function.acp1 == (actionf_p1)P_MobjThinker)
	    spritepresent[((mobj_t *)th)->sprite] = 1;
    }
	
    spritememory = 0;
    for (i=0 ; i<numsprites ; i++)
    {
	if (!spritepresent[i])
	    continue;

	for (j=0 ; j<sprites[i].numframes ; j++)
	{
	    sf = &sprites[i].spriteframes[j];
	    for (k=0 ; k<8 ; k++)
	    {
		lump = firstspritelump + sf->lump[k];
		spritememory += lumpinfo[lump].size;
		W_CacheLumpNum(lump , PU_CACHE);
	    }
	}
    }

    Z_Free(spritepresent);
}




