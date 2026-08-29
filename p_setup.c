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
//	Do all the WAD I/O, get map description,
//	set up initial state and misc. LUTs.
//



#include <math.h>
#include <stddef.h>   /* SATURN: offsetof, for the mobj_t layout asserts below */

#include "z_zone.h"

#include "deh_main.h"
#include "i_swap.h"
#include "m_argv.h"
#include "m_bbox.h"

#include "g_game.h"

#include "i_system.h"
#include "w_wad.h"

#include "doomdef.h"
#include "p_local.h"

#include "s_sound.h"

#include "doomstat.h"
#include "r_flatcache.h"


void	P_SpawnMapThing (mapthing_t*	mthing);


//
// MAP related Lookup tables.
// Store VERTEXES, LINEDEFS, SIDEDEFS, etc.
//
/* SATURN 2026-08-07: the level-load lump probe (`S<lumps><tag><n>` on overlay row 12) lived here
   and is REMOVED, having answered its question in one capture: **P_SetupLevel reads 73 lumps**
   against a boot+load costing **4704 CD commands / 270 s** -- so the level load is NOT the load,
   the BOOT is (R_InitSpriteLumps above all).  Its worst phase was `A`, the sfx precache, at 63 of
   the 73.  Do not rebuild this probe to re-ask that question; see
   [[streaming-load-budget-and-flat-treadmill]].  The core counter it used, `w_lump_reads`
   (w_wad.c), stays -- it is two lines to bracket anything again. */
int		numvertexes;
vertex_t*	vertexes;

int		numsegs;
seg_t*		segs;

int		numsectors;
sector_t*	sectors;

int		numsubsectors;
subsector_t*	subsectors;

int		numnodes;
node_t*		nodes;

/* SATURN 2026-08-18 -- THE BOOT WALL, PINNED.  These three sizes are the whole point of the
   level-structs work: on a 2 MB Saturn the zone's longest contiguous run at level load is ~110 KB,
   and LINEDEFS/SEGS/NODES are the only allocations that approach it.  Measured over the 235 maps
   of wads_temoins, 37 do not fit at the vanilla sizes and all 37 fit at these.  If a field is ever
   added back, the wall returns silently on the biggest maps only -- so fail the BUILD instead. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert (sizeof(seg_t)  == 14, "seg_t must stay 14 bytes (was 32) -- see r_defs.h");
_Static_assert (sizeof(node_t) == 28, "node_t must stay 28 bytes (was 52) -- see r_defs.h");
_Static_assert (sizeof(line_t) == 24, "line_t must stay 24 bytes (was 64) -- see r_defs.h");
_Static_assert (sizeof(side_t) == 16, "side_t must stay 16 bytes (was 20) -- see r_defs.h");
/* SATURN 2026-08-18 -- the level's BIGGEST consumer, pinned so the analysis quotes the compiler
   and not my field count.  SCYTHE MAP30 spawns 1082 mobjs on skill 4; the zone charges
   156 + sizeof(memblock_t) = 180 bytes for each (the figure the failing Z_Malloc reported), so
   190 KB -- of which 26 KB is BLOCK HEADER, one per mobj.  That header, not the struct, is the
   cheapest thing to attack next. */
_Static_assert (sizeof(mobj_t) == 156, "mobj_t is 156 bytes; the zone charges 180 with its header");
/* SATURN 2026-08-25 -- mobj_t FIELD ORDER, pinned by the compiler (see the long note in
   p_mobj.h).  Two invariants, two different failure modes:
     - offsets 0..23 are the degenmobj_t pun (sector soundorg).  Break it and the SOUND code
       reads a mobj's momentum as a coordinate -- silently, at run time, on hardware only.
     - `state` at 60 pins the HOT BLOCK to cache lines 1..3.  Push a field in above it and
       P_MobjThinker's working set spills into a fourth 16-byte line -- one more cold LWRAM
       fetch per mobj per tic, ~2900 times a frame, invisible on Ymir and invisible in the
       diff. */
_Static_assert (offsetof(mobj_t, x) == 12 && offsetof(mobj_t, y) == 16 && offsetof(mobj_t, z) == 20,
                "degenmobj_t pun: mobj_t's {thinker,x,y,z} prefix must stay at offsets 0..23");
_Static_assert (offsetof(mobj_t, state) == 60,
                "mobj_t hot set must stay inside cache lines 1..3 (see p_mobj.h)");
/* The in-place load below computes its overlap bound from BOTH sides of each conversion, so the
   on-disc record sizes are load-bearing too.  They are all-short structs, so PACKEDATTR is a
   no-op for them today -- which is exactly why a silent change here would go unnoticed. */
_Static_assert (sizeof(mapvertex_t)  ==  4, "mapvertex_t must stay 4 bytes");
_Static_assert (sizeof(mapseg_t)     == 12, "mapseg_t must stay 12 bytes");
_Static_assert (sizeof(maplinedef_t) == 14, "maplinedef_t must stay 14 bytes");
_Static_assert (sizeof(mapnode_t)    == 28, "mapnode_t must stay 28 bytes -- P_LoadNodes reads it IN PLACE over node_t");
#endif

int		numlines;
line_t*		lines;
int*		lines_validcount;	/* SATURN: mutable half of line_t (r_defs.h) */

int		numsides;
side_t*		sides;

static int      totallines;

// BLOCKMAP
// Created from axis aligned bounding box
// of the map, a rectangular array of
// blocks of size ...
// Used to speed up collision detection
// by spatial subdivision in 2D.
//
// Blockmap size.
int		bmapwidth;
int		bmapheight;	// size in mapblocks
short*		blockmap;	// int for larger maps
// offsets in blockmap are from here
short*		blockmaplump;		
// origin of block map
fixed_t		bmaporgx;
fixed_t		bmaporgy;
// for thing chains
mobj_t**	blocklinks;		


// REJECT
// For fast sight rejection.
// Speeds up enemy AI by skipping detailed
//  LineOf Sight calculation.
// Without special effect, this could be
//  used as a PVS lookup as well.
//
byte*		rejectmatrix;


// Maintain single and multi player starting spots.
#define MAX_DEATHMATCH_STARTS	10

mapthing_t	deathmatchstarts[MAX_DEATHMATCH_STARTS];
mapthing_t*	deathmatch_p;
mapthing_t	playerstarts[MAXPLAYERS];





//
// P_LoadVertexes
//
/* SATURN 2026-08-18 -- IN-PLACE LEVEL LOAD.  Every P_LoadX below used to hold TWO buffers at
   once: the final Z_Malloc'd array AND W_CacheLumpNum's copy of the raw lump.  Across the four
   geometry lumps that second buffer is 83 KB of transient peak on Tnt MAP11 and 189 KB on
   SCYTHE MAP30 -- demanded at the worst possible moment, while the zone is at its most
   fragmented and P_SetupLevel still has SECTORS, SIDEDEFS, REJECT and BLOCKMAP to place.

   Instead, read the raw records into the TAIL of the final array and expand forward.  Record i
   is written to [D*i, D*i+D) and read from [off + S*i, ...); since D > S the destination always
   runs BEHIND the source, except for the last few records, which are copied to a small stack
   buffer first.  Nothing extra is allocated and NOT ONE BYTE is added to the disc.

   That last clause is why this, and not a pre-baked WAD.  Baking the structs into the lumps
   (tools/bake_levels.py, written and then rejected) removes the conversion too -- but it costs
   +16 KB of CD read per map on LINEDEFS and +6 KB on VERTEXES, to save a few ms of CPU.  On a
   load whose spikes ARE synchronous CD reads that is the wrong direction; only NODES was free,
   and NODES needs no format change at all because mapnode_t and node_t are already the same
   28 bytes in the same order.

   `off` is rounded DOWN to a multiple of 4 so W_ReadLump keeps GFS's aligned fast path instead
   of bouncing through the staging scratch (w_file_saturn.cxx).  Rounding down only ever moves
   the source EARLIER, so it stays inside the array; it costs at most one more tail record. */
/* PROVED, not sampled.  With ntail = 16 the last index read from the array is i = n-17, and the
   no-overwrite condition D*i + D <= off + S*i (with off >= n*(D-S) - 3 after the align-down)
   reduces to 16*D >= 3 + 17*S -- independent of n.  SEGS 224 >= 207, LINEDEFS 384 >= 241,
   VERTEXES 128 >= 71.  For n <= 16 every record is stashed, so it holds trivially. */
#define SAT_INPLACE_TAIL 16

static int P_InPlaceOffset (int n, int dstsize, int srcsize)
{
    return (n * (dstsize - srcsize)) & ~3;
}

void P_LoadVertexes (int lump)
{
    int			i;
    int			off;
    int			ntail;
    mapvertex_t*	ml;
    vertex_t*		li;
    mapvertex_t		tail[SAT_INPLACE_TAIL];

    // Determine number of lumps:
    //  total lump length / vertex record length.
    numvertexes = W_LumpLength (lump) / sizeof(mapvertex_t);

    // Allocate zone memory for buffer.
    vertexes = Z_Malloc (numvertexes*sizeof(vertex_t),PU_LEVEL,0);	

    off = P_InPlaceOffset (numvertexes, sizeof(vertex_t), sizeof(mapvertex_t));
    W_ReadLump (lump, (byte *)vertexes + off);
    ml = (mapvertex_t *)((byte *)vertexes + off);

    ntail = (numvertexes < SAT_INPLACE_TAIL) ? numvertexes : SAT_INPLACE_TAIL;
    memcpy (tail, ml + (numvertexes - ntail), ntail*sizeof(mapvertex_t));

    // Copy and convert vertex coordinates,
    // internal representation as fixed.
    li = vertexes;
    for (i=0 ; i<numvertexes ; i++, li++)
    {
	const mapvertex_t*	m = (i >= numvertexes - ntail)
				  ? &tail[i - (numvertexes - ntail)] : &ml[i];

	li->x = SHORT(m->x)<<FRACBITS;
	li->y = SHORT(m->y)<<FRACBITS;
    }
}

//
// GetSectorAtNullAddress
//
sector_t* GetSectorAtNullAddress(void)
{
    static boolean null_sector_is_initialized = false;
    static sector_t null_sector;

    if (!null_sector_is_initialized)
    {
        memset(&null_sector, 0, sizeof(null_sector));
        I_GetMemoryValue(0, &null_sector.floorheight, 4);
        I_GetMemoryValue(4, &null_sector.ceilingheight, 4);
        null_sector_is_initialized = true;
    }

    return &null_sector;
}

//
// P_LoadSegs
//
void P_LoadSegs (int lump)
{
    int			i;
    int			off;
    int			ntail;
    mapseg_t*		ml;
    seg_t*		li;
    line_t*		ldef;
    int			linedef;
    int			side;
    int                 sidenum;
    mapseg_t		tail[SAT_INPLACE_TAIL];
	
    numsegs = W_LumpLength (lump) / sizeof(mapseg_t);
    segs = Z_Malloc (numsegs*sizeof(seg_t),PU_LEVEL,0);	
    memset (segs, 0, numsegs*sizeof(seg_t));

    /* SATURN: in-place expansion, 12 -> 14 bytes (see the note above P_LoadVertexes).  This is
       the biggest of the four: 33 KB of staging buffer on Tnt MAP11, 71 KB on SCYTHE MAP30. */
    off = P_InPlaceOffset (numsegs, sizeof(seg_t), sizeof(mapseg_t));
    W_ReadLump (lump, (byte *)segs + off);
    ml = (mapseg_t *)((byte *)segs + off);

    ntail = (numsegs < SAT_INPLACE_TAIL) ? numsegs : SAT_INPLACE_TAIL;
    memcpy (tail, ml + (numsegs - ntail), ntail*sizeof(mapseg_t));

    li = segs;
    for (i=0 ; i<numsegs ; i++, li++)
    {
	const mapseg_t*	m = (i >= numsegs - ntail)
			  ? &tail[i - (numsegs - ntail)] : &ml[i];

	/* SATURN 2026-08-18: seg_t is now 14 bytes of INDICES (see r_defs.h).  The two shifted
	   fields are stored exactly as the WAD holds them -- Doom widened them to 32 bits here and
	   the accessors shift them back at use, so this is a lossless re-encoding, not a rounding. */
	li->v1i = (unsigned short)SHORT(m->v1);
	li->v2i = (unsigned short)SHORT(m->v2);

	li->ang16 = (unsigned short)SHORT(m->angle);
	li->off16 = (short)SHORT(m->offset);
	linedef = SHORT(m->linedef);
	ldef = &lines[linedef];
	side = SHORT(m->side);
	li->ldi = (unsigned short)((linedef << 1) | (side & 1));
	li->fsi = sides[ldef->sidenum[side]].seci;

        if (ldef-> flags & ML_TWOSIDED)
        {
            sidenum = ldef->sidenum[side ^ 1];

            // If the sidenum is out of range, this may be a "glass hack"
            // impassible window.  Point at side #0 (this may not be
            // the correct Vanilla behavior; however, it seems to work for
            // OTTAWAU.WAD, which is the one place I've seen this trick
            // used).

            if (sidenum < 0 || sidenum >= numsides)
            {
                li->bsi = SEG_NULLSECTOR;
            }
            else
            {
                li->bsi = sides[sidenum].seci;
            }
        }
        else
        {
	    li->bsi = SEG_NOSECTOR;
        }
    }
}


//
// P_LoadSubsectors
//
void P_LoadSubsectors (int lump)
{
    byte*		data;
    int			i;
    mapsubsector_t*	ms;
    subsector_t*	ss;
	
    numsubsectors = W_LumpLength (lump) / sizeof(mapsubsector_t);
    subsectors = Z_Malloc (numsubsectors*sizeof(subsector_t),PU_LEVEL,0);	
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    ms = (mapsubsector_t *)data;
    memset (subsectors,0, numsubsectors*sizeof(subsector_t));
    ss = subsectors;
    
    for (i=0 ; i<numsubsectors ; i++, ss++, ms++)
    {
	ss->numlines = SHORT(ms->numsegs);
	ss->firstline = SHORT(ms->firstseg);
    }
	
    W_ReleaseLumpNum(lump);
}



//
// P_LoadSectors
//
void P_LoadSectors (int lump)
{
    byte*		data;
    int			i;
    mapsector_t*	ms;
    sector_t*		ss;
	
    numsectors = W_LumpLength (lump) / sizeof(mapsector_t);
    sectors = Z_Malloc (numsectors*sizeof(sector_t),PU_LEVEL,0);	
    memset (sectors, 0, numsectors*sizeof(sector_t));
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    ms = (mapsector_t *)data;
    ss = sectors;
    for (i=0 ; i<numsectors ; i++, ss++, ms++)
    {
	ss->floorheight = SHORT(ms->floorheight)<<FRACBITS;
	ss->ceilingheight = SHORT(ms->ceilingheight)<<FRACBITS;
	ss->floorpic = R_FlatNumForName(ms->floorpic);
	ss->ceilingpic = R_FlatNumForName(ms->ceilingpic);
	ss->lightlevel = SHORT(ms->lightlevel);
	ss->special = SHORT(ms->special);
	ss->tag = SHORT(ms->tag);
	ss->thinglist = NULL;
    }
	
    W_ReleaseLumpNum(lump);
}


//
// P_LoadNodes
//
void P_LoadNodes (int lump)
{
    int		i;
    int		j;
    int		k;
    node_t*	no;
	
    numnodes = W_LumpLength (lump) / sizeof(mapnode_t);
    nodes = Z_Malloc (numnodes*sizeof(node_t),PU_LEVEL,0);	

    /* SATURN: the only lump that needs no expansion at all -- mapnode_t and node_t are both 28
       bytes in the same field order, so the raw records land exactly where they belong and the
       loop is a pure byteswap over the SAME addresses.  That kills a staging buffer equal to the
       whole array (22 KB on Tnt MAP11, 53 KB on SCYTHE MAP30): NODES was the worst offender,
       doubling its own peak. */
    W_ReadLump (lump, nodes);

    no = nodes;
    for (i=0 ; i<numnodes ; i++, no++)
    {
	mapnode_t*	mn = (mapnode_t *)no;	/* same bytes: read each short, swap, write back */

	/* SATURN 2026-08-18: stored exactly as the WAD holds it; the <<FRACBITS moved to the
	   NODE_* accessors, which makes this a pure swap and the struct 28 bytes instead of 52. */
	no->x16 = SHORT(mn->x);
	no->y16 = SHORT(mn->y);
	no->dx16 = SHORT(mn->dx);
	no->dy16 = SHORT(mn->dy);
	for (j=0 ; j<2 ; j++)
	{
	    for (k=0 ; k<4 ; k++)
		no->bbox16[j][k] = SHORT(mn->bbox[j][k]);
	    no->children[j] = SHORT(mn->children[j]);
	}
    }
}


//
// P_LoadThings
//
void P_LoadThings (int lump)
{
    byte               *data;
    int			i;
    mapthing_t         *mt;
    mapthing_t          spawnthing;
    int			numthings;
    boolean		spawn;

    data = W_CacheLumpNum (lump,PU_STATIC);
    numthings = W_LumpLength (lump) / sizeof(mapthing_t);
	
    mt = (mapthing_t *)data;
    for (i=0 ; i<numthings ; i++, mt++)
    {
	spawn = true;

	// Do not spawn cool, new monsters if !commercial
	if (gamemode != commercial)
	{
	    switch (SHORT(mt->type))
	    {
	      case 68:	// Arachnotron
	      case 64:	// Archvile
	      case 88:	// Boss Brain
	      case 89:	// Boss Shooter
	      case 69:	// Hell Knight
	      case 67:	// Mancubus
	      case 71:	// Pain Elemental
	      case 65:	// Former Human Commando
	      case 66:	// Revenant
	      case 84:	// Wolf SS
		spawn = false;
		break;
	    }
	}
	if (spawn == false)
	    break;

	// Do spawn all other stuff. 
	spawnthing.x = SHORT(mt->x);
	spawnthing.y = SHORT(mt->y);
	spawnthing.angle = SHORT(mt->angle);
	spawnthing.type = SHORT(mt->type);
	spawnthing.options = SHORT(mt->options);
	
	P_SpawnMapThing(&spawnthing);
    }

    W_ReleaseLumpNum(lump);
}


//
// P_LoadLineDefs
// Also counts secret lines for intermissions.
//
void P_LoadLineDefs (int lump)
{
    int			i;
    int			off;
    int			ntail;
    maplinedef_t*	mld;
    line_t*		ld;
    vertex_t*		v1;
    vertex_t*		v2;
    maplinedef_t	tail[SAT_INPLACE_TAIL];
	
    numlines = W_LumpLength (lump) / sizeof(maplinedef_t);
    lines = Z_Malloc (numlines*sizeof(line_t),PU_LEVEL,0);	
    memset (lines, 0, numlines*sizeof(line_t));
    /* SATURN: the MUTABLE half of line_t, split out so lines[] stays pure geometry (r_defs.h).
       An order of magnitude smaller than the bytes the shrink just gave back, so it cannot
       reopen the contiguity wall it was made to clear. */
    lines_validcount = Z_Malloc (numlines*sizeof(int),PU_LEVEL,0);
    memset (lines_validcount, 0, numlines*sizeof(int));

    /* SATURN: in-place expansion, 14 -> 24 bytes (see the note above P_LoadVertexes). */
    off = P_InPlaceOffset (numlines, sizeof(line_t), sizeof(maplinedef_t));
    W_ReadLump (lump, (byte *)lines + off);
    mld = (maplinedef_t *)((byte *)lines + off);

    ntail = (numlines < SAT_INPLACE_TAIL) ? numlines : SAT_INPLACE_TAIL;
    memcpy (tail, mld + (numlines - ntail), ntail*sizeof(maplinedef_t));

    ld = lines;
    for (i=0 ; i<numlines ; i++, ld++)
    {
	const maplinedef_t*	m = (i >= numlines - ntail)
				  ? &tail[i - (numlines - ntail)] : &mld[i];
	fixed_t			dx;
	fixed_t			dy;

	ld->flags = SHORT(m->flags);
	ld->special = SHORT(m->special);
	ld->tag = SHORT(m->tag);
	ld->v1i = (unsigned short)SHORT(m->v1);
	ld->v2i = (unsigned short)SHORT(m->v2);
	v1 = &vertexes[ld->v1i];
	v2 = &vertexes[ld->v2i];
	dx = v2->x - v1->x;
	dy = v2->y - v1->y;

	/* SATURN: slopetype AND the four sign bits, resolved once here so the axis-aligned
	   early-outs in P_PointOnLineSide / P_BoxOnLineSide -- 3 linedefs out of 4 -- never
	   touch vertexes[] at run time.  See the LS_* note in r_defs.h. */
	if (!dx)
	    ld->slope = ST_VERTICAL;
	else if (!dy)
	    ld->slope = ST_HORIZONTAL;
	else if (FixedDiv (dy , dx) > 0)
	    ld->slope = ST_POSITIVE;
	else
	    ld->slope = ST_NEGATIVE;

	if (dx > 0)		ld->slope |= LS_DXPOS;
	else if (dx < 0)	ld->slope |= LS_DXNEG;
	if (dy > 0)		ld->slope |= LS_DYPOS;
	else if (dy < 0)	ld->slope |= LS_DYNEG;

	/* A bbox corner is a vertex coordinate, so it round-trips through a short exactly. */
	if (v1->x < v2->x)
	{
	    ld->bbox16[BOXLEFT]  = (short)(v1->x >> FRACBITS);
	    ld->bbox16[BOXRIGHT] = (short)(v2->x >> FRACBITS);
	}
	else
	{
	    ld->bbox16[BOXLEFT]  = (short)(v2->x >> FRACBITS);
	    ld->bbox16[BOXRIGHT] = (short)(v1->x >> FRACBITS);
	}

	if (v1->y < v2->y)
	{
	    ld->bbox16[BOXBOTTOM] = (short)(v1->y >> FRACBITS);
	    ld->bbox16[BOXTOP]    = (short)(v2->y >> FRACBITS);
	}
	else
	{
	    ld->bbox16[BOXBOTTOM] = (short)(v2->y >> FRACBITS);
	    ld->bbox16[BOXTOP]    = (short)(v1->y >> FRACBITS);
	}

	ld->sidenum[0] = SHORT(m->sidenum[0]);
	ld->sidenum[1] = SHORT(m->sidenum[1]);
	/* frontsector/backsector are no longer stored: LINE_FRONTSECTOR/LINE_BACKSECTOR do
	   exactly the lookup this loop used to bake in. */
    }
}


//
// P_LoadSideDefs
//
void P_LoadSideDefs (int lump)
{
    byte*		data;
    int			i;
    mapsidedef_t*	msd;
    side_t*		sd;
	
    numsides = W_LumpLength (lump) / sizeof(mapsidedef_t);
    sides = Z_Malloc (numsides*sizeof(side_t),PU_LEVEL,0);	
    memset (sides, 0, numsides*sizeof(side_t));
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    msd = (mapsidedef_t *)data;
    sd = sides;
    for (i=0 ; i<numsides ; i++, msd++, sd++)
    {
	sd->textureoffset = SHORT(msd->textureoffset)<<FRACBITS;
	sd->rowoffset = SHORT(msd->rowoffset)<<FRACBITS;
	sd->toptexture = R_TextureNumForName(msd->toptexture);
	sd->bottomtexture = R_TextureNumForName(msd->bottomtexture);
	sd->midtexture = R_TextureNumForName(msd->midtexture);
	{   /* SATURN: index, not pointer (r_defs.h).  Vanilla indexed sectors[] with whatever the
	       WAD said; an out-of-range value used to build a wild pointer, and now it would build
	       a wild INDEX -- same bug, so clamp it once here where it costs nothing. */
	    int	secnum = SHORT(msd->sector);
	    if (secnum < 0 || secnum >= numsectors)
		secnum = 0;
	    sd->seci = (unsigned short)secnum;
	}
    }

    W_ReleaseLumpNum(lump);
}


//
// P_LoadBlockMap
//
void P_LoadBlockMap (int lump)
{
    int i;
    int count;
    int lumplen;

    lumplen = W_LumpLength(lump);
    count = lumplen / 2;
	
    blockmaplump = Z_Malloc(lumplen, PU_LEVEL, NULL);
    W_ReadLump(lump, blockmaplump);
    blockmap = blockmaplump + 4;

    // Swap all short integers to native byte ordering.
  
    for (i=0; i<count; i++)
    {
	blockmaplump[i] = SHORT(blockmaplump[i]);
    }
		
    // Read the header

    bmaporgx = blockmaplump[0]<<FRACBITS;
    bmaporgy = blockmaplump[1]<<FRACBITS;
    bmapwidth = blockmaplump[2];
    bmapheight = blockmaplump[3];
	
    // Clear out mobj chains

    count = sizeof(*blocklinks) * bmapwidth * bmapheight;
    blocklinks = Z_Malloc(count, PU_LEVEL, 0);
    memset(blocklinks, 0, count);
}



//
// P_GroupLines
// Builds sector line lists and subsector sector numbers.
// Finds block bounding boxes for sectors.
//
/* SATURN: exact per-sector world bbox, 4 shorts each (BOXTOP/BOTTOM/LEFT/RIGHT, world
   units), (re)built by P_GroupLines every level load.  NULL before the first level. */
short *sat_sector_bbox = NULL;

void P_GroupLines (void)
{
    line_t**		linebuffer;
    int			i;
    int			j;
    line_t*		li;
    sector_t*		sector;
    subsector_t*	ss;
    seg_t*		seg;
    fixed_t		bbox[4];
    int			block;
	
    // look up sector number for each subsector
    ss = subsectors;
    for (i=0 ; i<numsubsectors ; i++, ss++)
    {
	seg = &segs[ss->firstline];
	ss->sector = SIDE_SECTOR(SEG_SIDEDEF(seg));
    }

    // count number of lines in each sector
    li = lines;
    totallines = 0;
    for (i=0 ; i<numlines ; i++, li++)
    {
	sector_t*	fs = LINE_FRONTSECTOR (li);
	sector_t*	bs = LINE_BACKSECTOR (li);

	totallines++;
	fs->linecount++;

	if (bs && bs != fs)
	{
	    bs->linecount++;
	    totallines++;
	}
    }

    // build line tables for each sector	
    linebuffer = Z_Malloc (totallines*sizeof(line_t *), PU_LEVEL, 0);

    for (i=0; i<numsectors; ++i)
    {
        // Assign the line buffer for this sector

        sectors[i].lines = linebuffer;
        linebuffer += sectors[i].linecount;

        // Reset linecount to zero so in the next stage we can count
        // lines into the list.

        sectors[i].linecount = 0;
    }

    // Assign lines to sectors

    for (i=0; i<numlines; ++i)
    { 
        sector_t*	fs;
        sector_t*	bs;

        li = &lines[i];
        fs = LINE_FRONTSECTOR (li);
        bs = LINE_BACKSECTOR (li);

        if (fs != NULL)
        {
            fs->lines[fs->linecount] = li;
            ++fs->linecount;
        }

        if (bs != NULL && bs != fs)
        {
            bs->lines[bs->linecount] = li;
            ++bs->linecount;
        }
    }
    
    // Generate bounding boxes for sectors

    /* SATURN 2026-08-24 (owner): KEEP the exact world bbox.
       Vanilla computes it right here and then throws it away -- only the blockmap-quantised
       version (sector->blockbox, 128-unit grain) and the midpoint (soundorg) survive.  The
       renderer therefore has no way to ask "how big is this floor in the world, and where",
       and the VDP1 floor claim had been RECONSTRUCTING an AABB by inverse-projecting the four
       corners of a visplane's SCREEN bbox -- a rectangle that overshoots the true polygon
       badly for anything seen at an angle, which is why a lit inset rectangle or a stair
       tread kept being refused or claimed with holes.
       This is the missing input, and it costs nothing to compute: the loop below already has
       the exact numbers.  4 shorts per sector (world units fit a short exactly -- Doom map
       coordinates are +-32768), from the LEVEL zone (PU_LEVEL, freed on level change), NOT
       from the boot TLSF pool.  ~720 B on E1M1, ~16 KB on a 2000-sector monster.
       Layout mirrors bbox[]: BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT. */
    /* PARKED with the VDP1 floor claim (via vp_bbox, its only reader -- see
       r_plane.c SAT_PLANE_IDENTITY and dg_saturn.cxx SAT_VDP1_FLOORS).  NULL here costs
       nothing and the fill below is guarded on it; the table is 8 B/sector of PU_LEVEL,
       ~720 B on E1M1 and up to 16 KB on a 2000-sector map.  Flip SAT_SECTOR_BBOX to 1
       with them.  The computation itself is free -- the loop below already has the exact
       numbers and vanilla throws them away, keeping only the 128-unit blockmap version. */
#define SAT_SECTOR_BBOX 0
    sat_sector_bbox = (SAT_SECTOR_BBOX && numsectors > 0)
        ? Z_Malloc (numsectors * 4 * sizeof(short), PU_LEVEL, 0) : NULL;

    sector = sectors;
    for (i=0 ; i<numsectors ; i++, sector++)
    {
	M_ClearBox (bbox);

	for (j=0 ; j<sector->linecount; j++)
	{
            li = sector->lines[j];

            M_AddToBox (bbox, LINE_V1(li)->x, LINE_V1(li)->y);
            M_AddToBox (bbox, LINE_V2(li)->x, LINE_V2(li)->y);
	}

	// SATURN: the exact world bbox, before the blockmap quantisation below destroys it
	if (sat_sector_bbox)
	{
	    short *sb = sat_sector_bbox + i*4;
	    sb[BOXTOP]    = (short)(bbox[BOXTOP]    >> FRACBITS);
	    sb[BOXBOTTOM] = (short)(bbox[BOXBOTTOM] >> FRACBITS);
	    sb[BOXLEFT]   = (short)(bbox[BOXLEFT]   >> FRACBITS);
	    sb[BOXRIGHT]  = (short)(bbox[BOXRIGHT]  >> FRACBITS);
	}

	// set the degenmobj_t to the middle of the bounding box
	sector->soundorg.x = (bbox[BOXRIGHT]+bbox[BOXLEFT])/2;
	sector->soundorg.y = (bbox[BOXTOP]+bbox[BOXBOTTOM])/2;
		
	// adjust bounding box to map blocks
	block = (bbox[BOXTOP]-bmaporgy+MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block >= bmapheight ? bmapheight-1 : block;
	sector->blockbox[BOXTOP]=block;

	block = (bbox[BOXBOTTOM]-bmaporgy-MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block < 0 ? 0 : block;
	sector->blockbox[BOXBOTTOM]=block;

	block = (bbox[BOXRIGHT]-bmaporgx+MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block >= bmapwidth ? bmapwidth-1 : block;
	sector->blockbox[BOXRIGHT]=block;

	block = (bbox[BOXLEFT]-bmaporgx-MAXRADIUS)>>MAPBLOCKSHIFT;
	block = block < 0 ? 0 : block;
	sector->blockbox[BOXLEFT]=block;
    }
	
}

// Pad the REJECT lump with extra data when the lump is too small,
// to simulate a REJECT buffer overflow in Vanilla Doom.

static void PadRejectArray(byte *array, unsigned int len)
{
    unsigned int i;
    unsigned int byte_num;
    byte *dest;
    unsigned int padvalue;

    // Values to pad the REJECT array with:

    unsigned int rejectpad[4] =
    {
        ((totallines * 4 + 3) & ~3) + 24,     // Size
        0,                                    // Part of z_zone block header
        50,                                   // PU_LEVEL
        0x1d4a11                              // DOOM_CONST_ZONEID
    };

    // Copy values from rejectpad into the destination array.

    dest = array;

    for (i=0; i<len && i<sizeof(rejectpad); ++i)
    {
        byte_num = i % 4;
        *dest = (rejectpad[i / 4] >> (byte_num * 8)) & 0xff;
        ++dest;
    }

    // We only have a limited pad size.  Print a warning if the
    // REJECT lump is too small.

    if (len > sizeof(rejectpad))
    {
        fprintf(stderr, "PadRejectArray: REJECT lump too short to pad! (%i > %i)\n",
                        len, (int) sizeof(rejectpad));

        // Pad remaining space with 0 (or 0xff, if specified on command line).

        if (M_CheckParm("-reject_pad_with_ff"))
        {
            padvalue = 0xff;
        }
        else
        {
            padvalue = 0xf00;
        }

        memset(array + sizeof(rejectpad), padvalue, len - sizeof(rejectpad));
    }
}

extern int sat_streaming_mode;   // defined below; CD-streaming (big-WAD) path

static void P_LoadReject(int lumpnum)
{
    int minlength;
    int lumplen;

    // Calculate the size that the REJECT lump *should* be.

    minlength = (numsectors * numsectors + 7) / 8;

#ifndef SAT_KEEP_REJECT
    /* 🔴 SATURN 2026-08-16 -- THE BLANKET SKIP WAS A MEASURED MISTAKE, NOW SIZE-GATED.
       The original comment justified dropping REJECT in streaming mode because it is
       "numsectors^2/8 bytes -- 45-125 KB on big Doom II maps", and asserted the cost of a NULL
       matrix was "a bit more CPU, IMPERCEPTIBLE at Saturn frame rates".
       Hardware measured it: **13,0 to 20,7 ms PER FRAME** of full BSP sight walks on TNT MAP11
       (row 24 `s`), with `sc0/124..346` -- ZERO trivial rejects, every single check walking the
       tree.  On a 48-73 ms tic that is a fifth to a third of it.  "Imperceptible" was an
       assumption nobody had ever put a number on.
       And the size that justified the skip is not this map's size: TNT MAP11 has 274 sectors, so
       its REJECT is **9,2 KB**, not 45-125.  The 45-125 KB case is real but it is the TAIL, and a
       blanket skip paid the whole tail's price on every map to avoid it.
       So gate on the ACTUAL size: keep the matrix when it fits the budget, skip it only when it
       is genuinely the fragmentation magnet the comment describes.  Z_CanAllocate is checked too
       -- on a tight level the sight speedup is not worth a level that will not load
       ([[zone-contiguity-wall-loadsegs]]).  Non-streaming (shareware/cart; DoomJo always has
       sat_streaming_mode==0) is unaffected either way.

       The owner then reported "le ciel a disparu, du noir, partout", and the hardware capture
       named the mechanism: `px1` on row 13 (r_patch_ovf, which had read 0 on EVERY capture before)
       = the garde-PATCH fired once.  REJECT is PU_LEVEL -- NOT purgeable -- so it permanently
       shortens the longest contiguous run, and at the first frame of the level the renderer needs
       **35080 bytes IN ONE RUN**: the size of every 256x128 TNT patch, RSKY1 included.  It missed,
       R_GetColumn served the zero-init placeholder for all 256 sky columns, and the uploader turned
       every zero into near-black -- a black sky, made permanent by a latch that has since been
       fixed (dg_saturn.cxx sky_cell_upload now refuses to latch a stubbed read and retries).

       🔴 AND THE OBVIOUS FIX HERE WAS THE WRONG LEVER -- the same capture killed it.  Reserving a
       big headroom (64 KB was tried) sounds right and is not: row 12 reads **`lg37k`..`lg38k`**
       steadily through the whole level, because ~366 KB of small unpurgeable PU_STATIC texture
       blocks chop the zone (r_data.c) -- 38 KB is the STRUCTURAL ceiling of this zone, REJECT or
       no REJECT.  Any headroom above ~28 KB therefore refuses the matrix on every map, and the
       matrix is worth **13-21 ms -> 1,2-4,9 ms** of sight per frame, measured on console
       (`sc544/28` vs the old `sc0/346`).  Keep the gate permissive and let the SKY heal instead:
       mid-level lg (37-38 KB) clears the 35 KB patch comfortably, so the retry always wins.
       `px` is the live check -- it must stop climbing once the sky is up. */
#define SAT_REJECT_MAX      (32*1024)   /* keep up to 32 KB; above that it is the OOM magnet    */
#define SAT_REJECT_HEADROOM (16*1024)   /* modest ON PURPOSE: lg is structurally ~38 KB here    */
    if (sat_streaming_mode
        && (minlength > SAT_REJECT_MAX
            || !Z_CanAllocate (minlength + SAT_REJECT_HEADROOM)))
    {
        rejectmatrix = NULL;
        return;
    }
#endif

    // If the lump meets the minimum length, it can be loaded directly.
    // Otherwise, we need to allocate a buffer of the correct size
    // and pad it with appropriate data.

    lumplen = W_LumpLength(lumpnum);

    if (lumplen >= minlength)
    {
        rejectmatrix = W_CacheLumpNum(lumpnum, PU_LEVEL);
    }
    else
    {
        rejectmatrix = Z_Malloc(minlength, PU_LEVEL, &rejectmatrix);
        W_ReadLump(lumpnum, rejectmatrix);

        PadRejectArray(rejectmatrix + lumplen, minlength - lumplen);
    }
}

// SATURN streaming: set to 1 by the platform layer (dg_saturn.cxx) when the
// IWAD is too big for the optional 4MB cart and is streamed from CD instead of
// mapped.  In that mode every cached lump is COPIED into the 884KB LWRAM zone,
// so we MUST NOT front-load all of a level's graphics via R_PrecacheLevel --
// they stream lazily as self-purging PU_CACHE instead (the model shareware
// already proves fits 884KB).  Defined here in the shared core so both ports
// link (DoomJo leaves it 0 -> precache stays on, unchanged behaviour).
int sat_streaming_mode = 0;

// SATURN M5 (CRITICAL_PATH.md §4): BSP-geometry staging.  The runtime BSP arrays
// (nodes/subsectors/vertexes/segs) are Z_Malloc'd into the LWRAM zone, which the
// SH-2 reads ~2.1x slower than high work RAM (rL, REC_BENCHMARKS.md §C.2).  The
// BSP walk (Bw), wall-prep (Bp) and P_PointInSubsector (every game tic) chase
// these arrays all frame long, so after level load we COPY as many of them as
// fit into a small high-RAM arena the platform donates (sat_bsp_stage_buf;
// NULL = feature off, so DoomJo links and behaves unchanged) and repoint the
// globals.  The arrays are immutable once P_GroupLines has run, so the LWRAM
// originals stay valid and P_BspStageApply can swap either set live for an A/B.
// Priority when the arena is too small for everything: nodes (hottest per byte)
// -> subsectors -> vertexes -> segs (largest; wall-prep + R_AddLine).
unsigned char *sat_bsp_stage_buf = 0;   /* platform-donated high-RAM arena (main.cxx) */
int sat_bsp_stage_size = 0;             /* arena bytes (0 = off) */
int sat_bsp_stage_on   = 1;             /* runtime A/B: 1 = staged copies live */
int sat_bsp_stage_used = 0;             /* bytes staged this level (overlay row 1) */
int sat_bsp_stage_want = 0;             /* bytes a full stage would have needed */

static node_t      *nodes_lw,      *nodes_hw;
static subsector_t *subsectors_lw, *subsectors_hw;
static vertex_t    *vertexes_lw,   *vertexes_hw;
static seg_t       *segs_lw,       *segs_hw;

// Swap the renderer/game globals between the LWRAM originals (on=0) and the
// staged high-RAM copies (on=1; arrays that did not fit stay on LWRAM).  Both
// copies are identical and read-only, so swapping between frames is safe.
void P_BspStageApply (int on)
{
    if (!nodes_lw)                      /* no level loaded/staged yet */
        return;
    nodes      = (on && nodes_hw)      ? nodes_hw      : nodes_lw;
    subsectors = (on && subsectors_hw) ? subsectors_hw : subsectors_lw;
    vertexes   = (on && vertexes_hw)   ? vertexes_hw   : vertexes_lw;
    segs       = (on && segs_hw)       ? segs_hw       : segs_lw;
    sat_bsp_stage_on = on;
}

/* Claim `bytes` from the arena and copy `src` there; NULL when it no longer fits
   (the array then simply stays on LWRAM). */
static void *P_StageTake (unsigned char **p, int *left, const void *src, int bytes)
{
    void *dst;
    if (*left < bytes)
        return 0;
    dst = *p;
    memcpy (dst, src, bytes);
    *p    += bytes;
    *left -= bytes;
    return dst;
}

// Staging order A/B (HW 2026-07-02: vertex staging alone moved Bp -4.5/-9.2 ms
// while Bw barely moved -> the seg/vertex reads dominate).  0 (default) = nodes
// -> subsectors -> vertexes -> segs.  1 = vertexes (the seg fixup needs them
// first) -> segs -> subsectors -> nodes: with a 32 KB arena this stages
// everything Bp + R_AddLine read (~29 KB on E1M1) at the price of the nodes
// (Bw + P_PointInSubsector) going back to LWRAM.
#ifndef SAT_BSP_STAGE_SEGS_FIRST
#define SAT_BSP_STAGE_SEGS_FIRST 0
#endif

static void P_StageBSP (void)
{
    unsigned char *p    = sat_bsp_stage_buf;
    int            left = sat_bsp_stage_size;
    int nb = numnodes      * (int)sizeof(node_t);
    int sb = numsubsectors * (int)sizeof(subsector_t);
    int vb = numvertexes   * (int)sizeof(vertex_t);
    int gb = numsegs       * (int)sizeof(seg_t);

    nodes_lw    = nodes;      subsectors_lw = subsectors;
    vertexes_lw = vertexes;   segs_lw       = segs;
    nodes_hw = 0;  subsectors_hw = 0;  vertexes_hw = 0;  segs_hw = 0;
    sat_bsp_stage_used = 0;
    sat_bsp_stage_want = nb + sb + vb + gb;

    if (!p || left <= 0)
        return;

#if SAT_BSP_STAGE_SEGS_FIRST
    vertexes_hw   = P_StageTake (&p, &left, vertexes_lw,   vb);
    segs_hw       = P_StageTake (&p, &left, segs_lw,       gb);
    subsectors_hw = P_StageTake (&p, &left, subsectors_lw, sb);
    nodes_hw      = P_StageTake (&p, &left, nodes_lw,      nb);
#else
    nodes_hw      = P_StageTake (&p, &left, nodes_lw,      nb);
    subsectors_hw = P_StageTake (&p, &left, subsectors_lw, sb);
    vertexes_hw   = P_StageTake (&p, &left, vertexes_lw,   vb);
    segs_hw       = P_StageTake (&p, &left, segs_lw,       gb);
#endif

    /* (the staged-segs pointer fix-up is GONE: seg_t now holds vertex INDICES, which are
       position-independent, so a staged copy needs no patching at all.  One of the small dividends
       of indices over pointers.) */
    sat_bsp_stage_used = (int)(p - sat_bsp_stage_buf);

    P_BspStageApply (sat_bsp_stage_on);
}

#ifdef SAT_SND_PRECACHE
// SATURN (streaming fluidity): warm THIS level's USED sound effects into SCSP RAM at load,
// so a never-before-heard sfx never does its blocking lump read + PCM upload MID-FIGHT the
// first time it fires (I_PrecacheSounds is a no-op stub on this platform, and R_PrecacheLevel
// precaches graphics only).  Zero Doom-zone cost -- the PCM lives in the separate SCSP RAM
// and each transient lump is purgeable PU_CACHE -- so this runs safely even on the big maps
// where the zone is nearly full; it is called AFTER R_SetupTextureCaches so the transient
// reads never shrink the texcache pool.  Fail-safe: any sfx not caught here (e.g. a monster
// that teleports in later) simply lazy-loads on first play, exactly as today.
static void SAT_MarkSfx (byte *used, int s)
{
    if (s > 0 && s < NUMSFX) used[s] = 1;
}

static void SAT_PrecacheLevelSounds (void)
{
    static byte used[NUMSFX];
    thinker_t  *th;
    int         i;

    // World / player sounds not tied to a spawned monster type: weapons, doors, switches,
    // lifts/floors, pickups, oof, teleport, barrel + rocket + imp-fireball explosions.
    // These fire in virtually every level regardless of the monster roster.
    static const short always[] = {
        sfx_pistol, sfx_shotgn, sfx_sgcock, sfx_dshtgn, sfx_dbopn, sfx_dbcls, sfx_dbload,
        sfx_plasma, sfx_bfg,    sfx_sawup,  sfx_sawidl, sfx_sawful, sfx_sawhit, sfx_chgun,
        sfx_rlaunc, sfx_rxplod, sfx_firsht, sfx_firxpl, sfx_barexp, sfx_punch,  sfx_slop,
        sfx_noway,  sfx_oof,    sfx_itemup, sfx_wpnup,  sfx_getpow, sfx_tink,
        sfx_doropn, sfx_dorcls, sfx_bdopn,  sfx_bdcls,  sfx_swtchn, sfx_swtchx,
        sfx_pstart, sfx_pstop,  sfx_stnmov, sfx_telept, sfx_itmbk,
    };

    memset (used, 0, sizeof(used));
    for (i = 0; i < (int)(sizeof(always) / sizeof(always[0])); i++)
        SAT_MarkSfx (used, always[i]);

    // Spawn-derived: every mobj present at level start contributes its five sounds.
    for (th = thinkercap.next; th != &thinkercap; th = th->next)
    {
        mobj_t *mo;
        if (th->function.acp1 != (actionf_p1)P_MobjThinker)
            continue;
        mo = (mobj_t *)th;
        if (mo->info == NULL) continue;
        SAT_MarkSfx (used, mo->info->seesound);
        SAT_MarkSfx (used, mo->info->attacksound);
        SAT_MarkSfx (used, mo->info->painsound);
        SAT_MarkSfx (used, mo->info->deathsound);
        SAT_MarkSfx (used, mo->info->activesound);
    }

    // Upload each unique used sfx (index 0 = sfx_None is skipped).  cache_sfx dedupes and
    // caps itself, and skips gracefully if the SCSP fills -- so this is bounded by design.
    for (i = 1; i < NUMSFX; i++)
        if (used[i])
            I_CacheSound (&S_sfx[i]);
}
#endif

/* 🔴 SATURN 2026-08-16 -- SKY PRECACHE HOOK.  Owner: "le ciel est toujours absent", twice.
   The platform uploads the sky into VDP2 VRAM from R_GetColumn, which needs the sky PATCH in
   **one contiguous run of 35080 bytes** (every 256x128 TNT patch, RSKY included).  It used to do
   that on the FIRST DISPLAYED FRAME, by which point the zone is already filling with composites
   and flats -- it passed, but only just.  Restoring the REJECT matrix took 9,2 KB of PU_LEVEL and
   that margin went: R_GetColumn served the zero-init placeholder, every zero became near-black,
   and the sky went black.  A per-frame retry did NOT save it -- hardware read `px37`->`px40`
   (climbing = retrying) against `lg22k`->`lg20k`, so the run NEVER comes back mid-level; in that
   scene no 35 KB patch fits at all, sky or wall.
   The defect is an ORDER, not a size.  The sky is a ONE-SHOT 35 KB read; REJECT is a 9,2 KB block
   held for the whole level.  So serve the sky FIRST, here, where the zone is at its emptiest for
   this level (geometry in, things/composites/flats not yet), and let REJECT take what is left.
   NULL on DoomJo (never assigned) -> plain C, zero behaviour change there. */
void (*sat_sky_precache_hook)(void) = NULL;

//
// P_SetupLevel
//
/* SATURN 2026-08-29: the zone's fragmenter census counts only what is allocated DURING PLAY, so
   it is disarmed for the whole of the level build and re-armed (and reset) once the build is
   done.  Row 12 `ip<n>/<KB>@<ra>`. */
void
P_SetupLevel
( int		episode,
  int		map,
  int		playermask,
  skill_t	skill)
{
    int		i;
    char	lumpname[9];
    int		lumpnum;
	
    Z_InPlayArm (0);      /* SATURN: the level BUILD is not what fragments the zone across a
                             session -- only what play does after it.  Re-armed at the end. */
    totalkills = totalitems = totalsecret = wminfo.maxfrags = 0;
    wminfo.partime = 180;
    for (i=0 ; i<MAXPLAYERS ; i++)
    {
	players[i].killcount = players[i].secretcount 
	    = players[i].itemcount = 0;
    }

    // Initial height of PointOfView
    // will be set by player think.
    players[consoleplayer].viewz = 1; 

    // Make sure all sounds are stopped before Z_FreeTags.
    S_Start ();			

    Z_FreeTags (PU_LEVEL, PU_PURGELEVEL-1);
    /* SATURN: the slab's chunks were PU_LEVEL, so they have just been freed -- drop the lists
       rather than walk them (p_mobj.c). */
    P_MobjSlabReset ();

    /* (R_ClearTextureCaches DELETED with core/r_cache.c on 2026-08-17.  Its slab needed 96 KB
       CONTIGUOUS and `xc0/0/60` says the carve never found more than 60 -- the pool never existed,
       so there was never anything to free here.  The flat pool below is the one that IS real.) */
    /* SATURN: same contract for the resident flat pool -- its slab is PU_STATIC and would
       otherwise survive Z_FreeTags as a MID-ZONE WALL while P_LoadSegs is asking for its one
       big contiguous SEGS array (see [[zone-contiguity-wall-loadsegs]]).  Release it here,
       re-carve after the geometry has landed. */
    R_ClearFlatCache ();

    // UNUSED W_Profile ();
    P_InitThinkers ();
	   
    // find map name
    if ( gamemode == commercial)
    {
	if (map<10)
	    DEH_snprintf(lumpname, 9, "map0%i", map);
	else
	    DEH_snprintf(lumpname, 9, "map%i", map);
    }
    else
    {
	lumpname[0] = 'E';
	lumpname[1] = '0' + episode;
	lumpname[2] = 'M';
	lumpname[3] = '0' + map;
	lumpname[4] = 0;
    }

    lumpnum = W_GetNumForName (lumpname);

    leveltime = 0;

#ifdef SAT_REPACK
    // SATURN per-level repack (STREAMING_ANALYSIS.md §7.4/7.9-7.11): point the .DRP
    // loader at this map's blob BEFORE its lumps page in (no-op without a valid .DRP).
    {
        extern void sat_drp_select_map(const char *lumpname);
        sat_drp_select_map (lumpname);
    }
#endif

    /* SATURN 2026-08-07: the level load costs 4704 CD commands / 270 s of disc on TNT MAP11
       (overlay row 12 `L`).  Which CALL issues them?  Bracket each phase on w_lump_reads (core,
       so DoomJo still compiles) and keep the TOTAL plus the single worst phase.  If the total is
       small next to `L`, P_SetupLevel is NOT the load and the boot path (W_Init / R_Init / the
       sprite index) owns it -- which is the first thing to know, and it is one capture away. */
    // note: most of this ordering is important
    P_LoadBlockMap (lumpnum+ML_BLOCKMAP);
    P_LoadVertexes (lumpnum+ML_VERTEXES);
    P_LoadSectors  (lumpnum+ML_SECTORS);
    P_LoadSideDefs (lumpnum+ML_SIDEDEFS);

    P_LoadLineDefs (lumpnum+ML_LINEDEFS);
    P_LoadSubsectors(lumpnum+ML_SSECTORS);
    P_LoadNodes    (lumpnum+ML_NODES);
    P_LoadSegs     (lumpnum+ML_SEGS);

    P_GroupLines ();
    /* SATURN: the sky gets its 35 KB run BEFORE P_LoadReject competes for it (see the hook note
       above P_SetupLevel).  skytexture is already set -- G_DoLoadLevel assigns it before calling
       us -- and the platform's uploader is idempotent: it latches and never runs again this level. */
    if (sat_sky_precache_hook)
	sat_sky_precache_hook ();
    P_LoadReject (lumpnum+ML_REJECT);

    // SATURN M5: geometry is final after P_GroupLines -- stage the hot BSP
    // arrays into the platform's high-RAM arena (no-op when none is donated).
    P_StageBSP ();

    bodyqueslot = 0;
    deathmatch_p = deathmatchstarts;
    P_LoadThings (lumpnum+ML_THINGS);
    
    // if deathmatch, randomly spawn the active players
    if (deathmatch)
    {
	for (i=0 ; i<MAXPLAYERS ; i++)
	    if (playeringame[i])
	    {
		players[i].mo = NULL;
		G_DeathMatchSpawnPlayer (i);
	    }
			
    }

    // clear special respawning que
    iquehead = iquetail = 0;		
	
    // set up world state
    P_SpawnSpecials ();
	
    // build subsector connect matrix
    //	UNUSED P_ConnectSubsectors ();

    // preload graphics
    // SATURN: skip the up-front precache in CD-streaming mode -- it would copy
    // ALL of a level's flats/patches/sprites into the small LWRAM zone at once
    // (overflow).  Lazy on-demand PU_CACHE streaming is used instead.
    if (precache && !sat_streaming_mode)
	R_PrecacheLevel ();

    // SATURN: carve the bounded streaming texture-cache pool from whatever
    // contiguous zone RAM is left after this level's geometry (no-op unless
    // sat_streaming_mode).  Done last so geometry never competes with the pool.
    /* SATURN: carve the resident flat pool BEFORE the composite pool.  Flats are the measured
       binding CD sink in play (80..221 non-resident fetches/s on TNT MAP11 vs 0..4 composite
       rebuilds), and unlike composites they are re-read on EVERY frame they are visible, so
       when only one slab fits, flats are the one that must get it. */
    /* SATURN 2026-08-07: GROUP the long-lived slabs LOW.  Both carves below are PU_STATIC and
       outlive the frame, so wherever they land they are a permanent cut through the free space --
       and left to the rover they land HIGH, right after the geometry that was just loaded, i.e. in
       the middle of the run gameplay needs.  Rewinding the rover first drops them into the lowest
       hole that fits, packed against the boot statics (lumpinfo / visplane pool / .DRP table).
       The problem these halts kept showing is the NUMBER OF CUTS, not any one block's size:
       `fr190K lg32K` = 190 KB free and never 34 KB in one piece.  Fewer, lower cuts = longer runs.
       Order is unchanged (flats before composites, both AFTER the geometry) -- carving before
       P_LoadSegs is what [[zone-contiguity-wall-loadsegs]] proved fatal on TNT MAP19. */
    Z_RoverToStart ();
    R_SetupFlatCache ();
    /* (R_SetupTextureCaches DELETED with core/r_cache.c -- see above.) */

#ifdef SAT_SND_PRECACHE
    // SATURN: warm this level's sound effects into SCSP RAM (off the gameplay frame).
    // Runs in both cart and CD-streaming modes -- the SCSP is separate from the Doom zone
    // so there is no OOM risk; the win is largest in streaming mode (it also moves the
    // blocking CD read to the load screen).  Skipped for demos (precache == false).
    if (precache)
        SAT_PrecacheLevelSounds ();   /* 63 of P_SetupLevel's 73 lumps -- its biggest phase, and
                                         still negligible next to the 4704-command boot */
#endif

#ifdef SAT_REPACK
    // SATURN R5.1 (STREAMING_FLUIDITY_ROADMAP.md §8): budgeted preload of this map's
    // .DRP subset (sprites + flats first) into purgeable PU_CACHE, under the load fade
    // -- first-sight assets stop paying their CD read mid-combat.  Runs LAST so neither
    // the texcache carve nor the sfx warm-up competes with it; its keep-free guard makes
    // it a no-op on zone-tight maps.  Demos skip it (precache == false), same convention
    // as the precaches above.  No-op without an active .DRP.
    if (precache && sat_streaming_mode)
    {
        extern void sat_drp_preload (void);
        sat_drp_preload ();
    }
#endif

    //printf ("free memory: 0x%x\n", Z_FreeMemory());

    /* SATURN: the build is over -- start counting the long-lived allocations that PLAY makes.
       Everything above this line is the level itself, and it is not what fragments the zone
       across a session; what happens after it is. */
    Z_InPlayArm (1);
}



//
// P_Init
//
void P_Init (void)
{
    P_InitSwitchList ();
    P_InitPicAnims ();
    R_InitSprites (sprnames);
}



