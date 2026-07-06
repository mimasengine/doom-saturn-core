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

// R_GenerateComposite
// Using the texture definition,
//  the composite texture is created from the patches,
//  and each column is cached.
//
void R_GenerateComposite (int texnum)
{
    byte*		block;
    int			cached = 0;
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
    else
    {
	block = Z_Malloc (texturecompositesize[texnum],
			  PU_STATIC,
			  &texturecomposite[texnum]);
    }

    collump = texturecolumnlump[texnum];
    colofs = texturecolumnofs[texnum];

    // Composite the columns together.
    patch = texture->patches;
		
    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
	realpatch = W_CacheLumpNum (patch->patch, PU_CACHE);
	x1 = patch->originx;
	x2 = x1 + SHORT(realpatch->width);

	if (x1<0)
	    x = 0;
	else
	    x = x1;
	
	if (x2 > texture->width)
	    x2 = texture->width;

	for ( ; x<x2 ; x++)
	{
	    // Column does not have multiple patches?
	    if (collump[x] >= 0)
		continue;
	    
	    patchcol = (column_t *)((byte *)realpatch
				    + LONG(realpatch->columnofs[x-x1]));
	    R_DrawColumnInCache (patchcol,
				 block + colofs[x],
				 patch->originy,
				 texture->height);
	}
						
    }

    // Classic path: now that the texture is built it is purgable from the zone.
    // Cache-pool blocks are managed by the LRU (R_PostTexCacheFrame), not the
    // zone purger, so they are left alone here.
    if (!cached)
	Z_ChangeTag (block, PU_CACHE);

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
    patch = texture->patches;

    for (i=0 , patch = texture->patches;
	 i<texture->patchcount;
	 i++, patch++)
    {
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
	
    for (x=0 ; x<texture->width ; x++)
    {
	if (!patchcount[x])
	{
	    printf ("R_GenerateLookup: column without a patch (%s)\n",
		    texture->name);
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
    Z_ChangeTag (texturecolumnlump[tex], PU_CACHE);
    Z_ChangeTag (texturecolumnofs[tex],  PU_CACHE);
}




//
// R_GetColumn
//
byte*
R_GetColumn
( int		tex,
  int		col )
{
    int		lump;
    int		ofs;
	
    col &= texturewidthmask[tex];
    R_EnsureLookup (tex);   // SATURN R4: build the directory on first use (or after a purge)
    lump = texturecolumnlump[tex][col];
    ofs = texturecolumnofs[tex][col];
    
    if (lump > 0)
	return (byte *)W_CacheLumpNum(lump,PU_CACHE)+ofs;

    if (!texturecomposite[tex])
	R_GenerateComposite (tex);
    else if (sat_texcache_active)
	R_TexCacheTouch (texturecomposite[tex]);   // keep visible composites resident

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
int R_FlatPotatoColor (int lumpnum)
{
    static short *cache = NULL;
    static int    base = 0, count = 0;
    int   hist[256];
    byte *src;
    int   i, fi;
    if (!cache)
    {
	base = firstflat; count = numflats;
	cache = Z_Malloc (count * (int)sizeof(short), PU_STATIC, 0);
	for (i = 0; i < count; i++) cache[i] = -1;
    }
    fi = lumpnum - base;
    if (fi < 0 || fi >= count) return 0;
    if (cache[fi] >= 0) return cache[fi];
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
/* SATURN Potato walls: set per seg in r_segs.c = (the seg's linedef has a special).
   Interactive surfaces (doors, switches, ...) are special lines; keep them TEXTURED
   even in Potato so they stay readable (a flat-grey door in a flat-grey corridor
   is unfindable). */
int sat_wall_textured = 0;
int R_WallPotatoColor (int tex)
{
    static short *cache = NULL;
    int   w, h, col, y, i;
    int   hist[256];
    byte *p;

    if (tex < 0 || tex >= numtextures) return 0;
    if (!cache)
    {
	cache = Z_Malloc(numtextures * (int)sizeof(short), PU_STATIC, 0);
	for (i = 0; i < numtextures; i++) cache[i] = -1;
    }
    if (cache[tex] >= 0) return cache[tex];

    memset (hist, 0, sizeof hist);
    w = texturewidthmask[tex] + 1;
    h = textureheight[tex] >> FRACBITS;
    for (col = 0; col < w; col += 2)
    {
	p = R_GetColumn(tex, col);
	for (y = 0; y < h; y += 2) hist[p[y]]++;
    }
    cache[tex] = (short) R_PotatoRepColor (hist);
    return cache[tex];
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

	texture = textures[i] =
	    Z_Malloc (sizeof(texture_t)
		      + sizeof(texpatch_t)*(SHORT(mtexture->patchcount)-1),
		      PU_STATIC, 0);
	
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




