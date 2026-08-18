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
//      Refresh/rendering module, shared data struct definitions.
//


#ifndef __R_DEFS__
#define __R_DEFS__


// Screenwidth.
#include "doomdef.h"

// Some more or less basic data types
// we depend on.
#include "m_fixed.h"

// We rely on the thinker data struct
// to handle sound origins in sectors.
#include "d_think.h"
// SECTORS do store MObjs anyway.
#include "p_mobj.h"

#include "i_video.h"

#include "v_patch.h"




// Silhouette, needed for clipping Segs (mainly)
// and sprites representing things.
#define SIL_NONE		0
#define SIL_BOTTOM		1
#define SIL_TOP			2
#define SIL_BOTH		3

#define MAXDRAWSEGS		256





//
// INTERNAL MAP TYPES
//  used by play and refresh
//

//
// Your plain vanilla vertex.
// Note: transformed values not buffered locally,
//  like some DOOM-alikes ("wt", "WebView") did.
//
typedef struct
{
    fixed_t	x;
    fixed_t	y;
    
} vertex_t;


// Forward of LineDefs, for Sectors.
struct line_s;

// Each sector has a degenmobj_t in its center
//  for sound origin purposes.
// I suppose this does not handle sound from
//  moving objects (doppler), because
//  position is prolly just buffered, not
//  updated.
typedef struct
{
    thinker_t		thinker;	// not used for anything
    fixed_t		x;
    fixed_t		y;
    fixed_t		z;

} degenmobj_t;

//
// The SECTORS record, at runtime.
// Stores things/mobjs.
//
typedef	struct
{
    fixed_t	floorheight;
    fixed_t	ceilingheight;
    short	floorpic;
    short	ceilingpic;
    short	lightlevel;
    short	special;
    short	tag;

    // 0 = untraversed, 1,2 = sndlines -1
    int		soundtraversed;

    // thing that made a sound (or null)
    mobj_t*	soundtarget;

    // mapblock bounding box for height changes
    int		blockbox[4];

    // origin for any sounds played by the sector
    degenmobj_t	soundorg;

    // if == validcount, already checked
    int		validcount;

    // list of mobjs in sector
    mobj_t*	thinglist;

    // thinker_t for reversable actions
    void*	specialdata;

    int			linecount;
    struct line_s**	lines;	// [linecount] size
    
} sector_t;




//
// The SideDef.
//

typedef struct
{
    // add this to the calculated texture column
    fixed_t	textureoffset;
    
    // add this to the calculated texture top
    fixed_t	rowoffset;

    // Texture indices.
    // We do not maintain names here. 
    short	toptexture;
    short	bottomtexture;
    short	midtexture;

    // Sector the SideDef is facing.
    sector_t*	sector;
    
} side_t;



//
// Move clipping aid for LineDefs.
//
typedef enum
{
    ST_HORIZONTAL,
    ST_VERTICAL,
    ST_POSITIVE,
    ST_NEGATIVE

} slopetype_t;



typedef struct line_s
{
    // Vertices, from v1 to v2.
    vertex_t*	v1;
    vertex_t*	v2;

    // Precalculated v2 - v1 for side checking.
    fixed_t	dx;
    fixed_t	dy;

    // Animation related.
    short	flags;
    short	special;
    short	tag;

    // Visual appearance: SideDefs.
    //  sidenum[1] will be -1 if one sided
    short	sidenum[2];			

    // Neat. Another bounding box, for the extent
    //  of the LineDef.
    fixed_t	bbox[4];

    // To aid move clipping.
    slopetype_t	slopetype;

    // Front and back sector.
    // Note: redundant? Can be retrieved from SideDefs.
    sector_t*	frontsector;
    sector_t*	backsector;

    // if == validcount, already checked
    int		validcount;

    // thinker_t for reversable actions
    void*	specialdata;		
} line_t;




//
// A SubSector.
// References a Sector.
// Basically, this is a list of LineSegs,
//  indicating the visible walls that define
//  (all or some) sides of a convex BSP leaf.
//
typedef struct subsector_s
{
    sector_t*	sector;
    short	numlines;
    short	firstline;
    
} subsector_t;



//
// The LineSeg.
//
/* 🔴 SATURN 2026-08-18 -- seg_t 32 -> 14 BYTES.  The boot wall is CONTIGUITY: P_LoadSegs asks
   Z_Malloc for numsegs*sizeof(seg_t) in ONE run, and the zone's longest run at level load is
   ~110 KB.  Measured offline over 161 witness maps: 35 of them demand more than that TODAY (TNT
   MAP31 180 KB, SCYTHE MAP30 245 KB, Nuts3 210 KB) -- and TNT MAP19, the map already known not to
   boot, is one of them.  At 14 bytes NONE of the 161 does.

   The shrink is LOSSLESS, not a compromise, because the WAD already stores all of it in 16 bits:
     offset  fixed_t   <- mapseg_t.offset is a short, loaded as (short)<<FRACBITS
     angle   angle_t   <- mapseg_t.angle  is a short, loaded as (short)<<16
   so storing the short and shifting at USE is bit-identical to what Doom kept in 32 bits.
   v1/v2/linedef/front/back become INDICES: a pointer costs 4 bytes to say what 2 can.

   WHY sidedef IS DERIVED BUT front/backsector ARE NOT.  d32xr derives all three and reaches 6
   bytes.  `sidedef` is read 6 times in the whole engine, so deriving it is free.  `frontsector`
   and `backsector` are read 54 times, on the renderer's hottest path -- deriving them turns one
   load into THREE DEPENDENT loads (line -> sidenum -> side -> sector), which on a memory-bound
   machine is exactly the trade this project keeps losing.  They stay as one indexed load.
   14 vs 6 bytes changes nothing about the wall (62 KB vs 26 KB on the worst map, both far under
   110), so the safe form wins.  Going to 6 stays available if RAM ever gets tight.

   Fields are RENAMED, deliberately: every old `seg->v1` must become a compile error so the
   compiler -- not my reading -- is the checklist of call sites. */
#define SEG_NOSECTOR	0xffffu		/* backsector index meaning "one-sided"            */
/* The "glass hack" (OTTAWAU.WAD): a two-sided line whose back sidenum is out of range.  Vanilla
   hands back a STATIC sector that lives outside sectors[], so an index cannot name it -- it gets a
   sentinel of its own rather than being silently folded into one-sided, which would change what the
   renderer draws on those lines. */
#define SEG_NULLSECTOR	0xfffeu

typedef struct
{
    unsigned short	v1i;		/* vertex INDEX  (was vertex_t*)                  */
    unsigned short	v2i;
    unsigned short	ldi;		/* linedef INDEX, SIDE PACKED IN BIT 0            */
    unsigned short	fsi;		/* frontsector INDEX                              */
    unsigned short	bsi;		/* backsector INDEX, or SEG_NOSECTOR              */
    short		off16;		/* fixed_t offset  = off16 << FRACBITS  (exact)    */
    unsigned short	ang16;		/* angle_t angle   = ang16 << 16       (exact)    */
} seg_t;

/* Accessors.  Read-only by construction: nothing in the engine writes a seg after P_LoadSegs, which
   is what makes step 3 (a WAD-resident, never-copied seg array) possible later. */
/* No externs here: r_state.h already declares vertexes/lines/sides/sectors, and a macro is only
   expanded at its USE site, which is always after that header. */
#define SEG_V1(s)		(&vertexes[(s)->v1i])
#define SEG_V2(s)		(&vertexes[(s)->v2i])
#define SEG_OFFSET(s)		((fixed_t)(s)->off16 << FRACBITS)
#define SEG_ANGLE(s)		((angle_t)(s)->ang16 << 16)
#define SEG_LINEDEF(s)		(&lines[(s)->ldi >> 1])
#define SEG_SIDE(s)		((s)->ldi & 1)
#define SEG_SIDEDEF(s)		(&sides[SEG_LINEDEF(s)->sidenum[SEG_SIDE(s)]])
#define SEG_FRONTSECTOR(s)	(&sectors[(s)->fsi])
/* A FUNCTION, not a macro, and deliberately so.  The first version inlined a two-way conditional at
   all 54 call sites and cost ~900 B of .text -- which on this target is 900 B of TLSF pool, and the
   pre-flight refused the build.  One out-of-line copy is smaller than 54 inlined ones, and the cost
   is nil where it matters: every hot user (R_AddLine, R_StoreWallRange, R_RenderMaskedSegRange)
   reads it ONCE per seg into a local, never per column.  Not `static inline` in the header for the
   same reason -- that would put it back in every translation unit. */
sector_t*	GetSectorAtNullAddress (void);   /* p_setup.c -- the vanilla glass hack */
sector_t*	SEG_BACKSECTOR (const seg_t *s);



//
// BSP node.
//
/* 🔴 SATURN 2026-08-18 -- node_t 52 -> 28 BYTES, and lossless for the same reason seg_t was:
   the WAD's mapnode_t already holds x/y/dx/dy and the two bounding boxes as SHORTS, and P_LoadNodes
   widened every one of them by <<FRACBITS.  Keep the short, shift at use, get the identical fixed_t.
   Nuts3's node array goes 132 KB -> 71 KB, under the 110 KB contiguous run the zone can promise.
   `children` is already 16-bit and stays as it is.
   NOT d32xr's 16-byte form: that packs the bbox into two uint16 (`encbbox`), which is a lossy
   re-encoding of the traversal test.  28 bytes clears the wall on every witness map, so there is
   nothing to buy with the risk.  Renamed fields again -- the compiler is the checklist. */
typedef struct
{
    // Partition line, as the WAD stores it (fixed_t = value << FRACBITS).
    short	x16;
    short	y16;
    short	dx16;
    short	dy16;

    // Bounding box for each child, same encoding.
    short	bbox16[2][4];

    // If NF_SUBSECTOR its a subsector.
    unsigned short children[2];
} node_t;

#define NODE_X(n)	((fixed_t)(n)->x16  << FRACBITS)
#define NODE_Y(n)	((fixed_t)(n)->y16  << FRACBITS)
#define NODE_DX(n)	((fixed_t)(n)->dx16 << FRACBITS)
#define NODE_DY(n)	((fixed_t)(n)->dy16 << FRACBITS)
#define NODE_BBOX(n,c,e) ((fixed_t)(n)->bbox16[c][e] << FRACBITS)




// PC direct to screen pointers
//B UNUSED - keep till detailshift in r_draw.c resolved
//extern byte*	destview;
//extern byte*	destscreen;





//
// OTHER TYPES
//

// This could be wider for >8 bit display.
// Indeed, true color support is posibble
//  precalculating 24bpp lightmap/colormap LUT.
//  from darkening PLAYPAL to all black.
// Could even us emore than 32 levels.
typedef byte	lighttable_t;	




//
// ?
//
typedef struct drawseg_s
{
    seg_t*		curline;
    int			x1;
    int			x2;

    fixed_t		scale1;
    fixed_t		scale2;
    fixed_t		scalestep;

    // 0=none, 1=bottom, 2=top, 3=both
    int			silhouette;

    // do not clip sprites above this
    fixed_t		bsilheight;

    // do not clip sprites below this
    fixed_t		tsilheight;
    
    // Pointers to lists for sprite clipping,
    //  all three adjusted so [x1] is first value.
    short*		sprtopclip;		
    short*		sprbottomclip;	
    short*		maskedtexturecol;
    
} drawseg_t;



// A vissprite_t is a thing
//  that will be drawn during a refresh.
// I.e. a sprite object that is partly visible.
typedef struct vissprite_s
{
    // Doubly linked list.
    struct vissprite_s*	prev;
    struct vissprite_s*	next;
    
    int			x1;
    int			x2;

    // for line side calculation
    fixed_t		gx;
    fixed_t		gy;		

    // global bottom / top for silhouette clipping
    fixed_t		gz;
    fixed_t		gzt;

    // horizontal position of x1
    fixed_t		startfrac;
    
    fixed_t		scale;
    
    // negative if flipped
    fixed_t		xiscale;	

    fixed_t		texturemid;
    int			patch;

    // for color translation and shadow draw,
    //  maxbright frames as well
    lighttable_t*	colormap;
   
    int			mobjflags;
    
} vissprite_t;


//	
// Sprites are patches with a special naming convention
//  so they can be recognized by R_InitSprites.
// The base name is NNNNFx or NNNNFxFx, with
//  x indicating the rotation, x = 0, 1-7.
// The sprite and frame specified by a thing_t
//  is range checked at run time.
// A sprite is a patch_t that is assumed to represent
//  a three dimensional object and may have multiple
//  rotations pre drawn.
// Horizontal flipping is used to save space,
//  thus NNNNF2F5 defines a mirrored patch.
// Some sprites will only have one picture used
// for all views: NNNNF0
//
typedef struct
{
    // If false use 0 for any position.
    // Note: as eight entries are available,
    //  we might as well insert the same name eight times.
    boolean	rotate;

    // Lump to use for view angles 0-7.
    short	lump[8];

    // Flip bit (1 = flip) to use for view angles 0-7.
    byte	flip[8];
    
} spriteframe_t;



//
// A sprite definition:
//  a number of animation frames.
//
typedef struct
{
    int			numframes;
    spriteframe_t*	spriteframes;

} spritedef_t;



//
// Now what is a visplane, anyway?
//

/* SATURN #1 (visplane span pooling, d32xr layout): default 0 == inline arrays ==
   vanilla == byte-identical (DoomJo unaffected).  When 1, top/bottom point into a
   shared per-frame bump pool (r_plane.c) so the struct shrinks ~664B -> ~28B: the
   hash/sort/header walk touches ~24x fewer cache lines, and a future 2nd split-view
   can share ONE arena.  Each pooled slice is (SCREENWIDTH+2) bytes with the pointer
   at base+1, reproducing the pad1/pad2/pad3/pad4 [-1..SCREENWIDTH] sentinel slots
   EXACTLY (so every `pl->top[x]` access site is unchanged).  Pixel output is
   Ymir-validatable (flat memory -> byte-identical).  See docs / memory. */
#ifndef SAT_VISPLANE_POOL
#define SAT_VISPLANE_POOL 0
#endif

typedef struct
{
  fixed_t		height;
  int			picnum;
  int			lightlevel;
  int			minx;
  int			maxx;

#if SAT_VISPLANE_POOL
  // pooled: point into the shared span arena; each slice has the [-1] and
  // [SCREENWIDTH] pad slots (base+1) so the sentinel accesses stay in-bounds.
  byte		*top;
  byte		*bottom;
#else
  // leave pads for [minx-1]/[maxx+1]

  byte		pad1;
  // Here lies the rub for all
  //  dynamic resize/change of resolution.
  byte		top[SCREENWIDTH];
  byte		pad2;
  byte		pad3;
  // See above.
  byte		bottom[SCREENWIDTH];
  byte		pad4;
#endif

} visplane_t;




#endif
