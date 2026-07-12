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
#include "r_cache.h"


void	P_SpawnMapThing (mapthing_t*	mthing);


//
// MAP related Lookup tables.
// Store VERTEXES, LINEDEFS, SIDEDEFS, etc.
//
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

int		numlines;
line_t*		lines;

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
void P_LoadVertexes (int lump)
{
    byte*		data;
    int			i;
    mapvertex_t*	ml;
    vertex_t*		li;

    // Determine number of lumps:
    //  total lump length / vertex record length.
    numvertexes = W_LumpLength (lump) / sizeof(mapvertex_t);

    // Allocate zone memory for buffer.
    vertexes = Z_Malloc (numvertexes*sizeof(vertex_t),PU_LEVEL,0);	

    // Load data into cache.
    data = W_CacheLumpNum (lump, PU_STATIC);
	
    ml = (mapvertex_t *)data;
    li = vertexes;

    // Copy and convert vertex coordinates,
    // internal representation as fixed.
    for (i=0 ; i<numvertexes ; i++, li++, ml++)
    {
	li->x = SHORT(ml->x)<<FRACBITS;
	li->y = SHORT(ml->y)<<FRACBITS;
    }

    // Free buffer memory.
    W_ReleaseLumpNum(lump);
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
    byte*		data;
    int			i;
    mapseg_t*		ml;
    seg_t*		li;
    line_t*		ldef;
    int			linedef;
    int			side;
    int                 sidenum;
	
    numsegs = W_LumpLength (lump) / sizeof(mapseg_t);
    segs = Z_Malloc (numsegs*sizeof(seg_t),PU_LEVEL,0);	
    memset (segs, 0, numsegs*sizeof(seg_t));
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    ml = (mapseg_t *)data;
    li = segs;
    for (i=0 ; i<numsegs ; i++, li++, ml++)
    {
	li->v1 = &vertexes[SHORT(ml->v1)];
	li->v2 = &vertexes[SHORT(ml->v2)];

	li->angle = (SHORT(ml->angle))<<16;
	li->offset = (SHORT(ml->offset))<<16;
	linedef = SHORT(ml->linedef);
	ldef = &lines[linedef];
	li->linedef = ldef;
	side = SHORT(ml->side);
	li->sidedef = &sides[ldef->sidenum[side]];
	li->frontsector = sides[ldef->sidenum[side]].sector;

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
                li->backsector = GetSectorAtNullAddress();
            }
            else
            {
                li->backsector = sides[sidenum].sector;
            }
        }
        else
        {
	    li->backsector = 0;
        }
    }
	
    W_ReleaseLumpNum(lump);
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
    byte*	data;
    int		i;
    int		j;
    int		k;
    mapnode_t*	mn;
    node_t*	no;
	
    numnodes = W_LumpLength (lump) / sizeof(mapnode_t);
    nodes = Z_Malloc (numnodes*sizeof(node_t),PU_LEVEL,0);	
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    mn = (mapnode_t *)data;
    no = nodes;
    
    for (i=0 ; i<numnodes ; i++, no++, mn++)
    {
	no->x = SHORT(mn->x)<<FRACBITS;
	no->y = SHORT(mn->y)<<FRACBITS;
	no->dx = SHORT(mn->dx)<<FRACBITS;
	no->dy = SHORT(mn->dy)<<FRACBITS;
	for (j=0 ; j<2 ; j++)
	{
	    no->children[j] = SHORT(mn->children[j]);
	    for (k=0 ; k<4 ; k++)
		no->bbox[j][k] = SHORT(mn->bbox[j][k])<<FRACBITS;
	}
    }
	
    W_ReleaseLumpNum(lump);
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
    byte*		data;
    int			i;
    maplinedef_t*	mld;
    line_t*		ld;
    vertex_t*		v1;
    vertex_t*		v2;
	
    numlines = W_LumpLength (lump) / sizeof(maplinedef_t);
    lines = Z_Malloc (numlines*sizeof(line_t),PU_LEVEL,0);	
    memset (lines, 0, numlines*sizeof(line_t));
    data = W_CacheLumpNum (lump,PU_STATIC);
	
    mld = (maplinedef_t *)data;
    ld = lines;
    for (i=0 ; i<numlines ; i++, mld++, ld++)
    {
	ld->flags = SHORT(mld->flags);
	ld->special = SHORT(mld->special);
	ld->tag = SHORT(mld->tag);
	v1 = ld->v1 = &vertexes[SHORT(mld->v1)];
	v2 = ld->v2 = &vertexes[SHORT(mld->v2)];
	ld->dx = v2->x - v1->x;
	ld->dy = v2->y - v1->y;
	
	if (!ld->dx)
	    ld->slopetype = ST_VERTICAL;
	else if (!ld->dy)
	    ld->slopetype = ST_HORIZONTAL;
	else
	{
	    if (FixedDiv (ld->dy , ld->dx) > 0)
		ld->slopetype = ST_POSITIVE;
	    else
		ld->slopetype = ST_NEGATIVE;
	}
		
	if (v1->x < v2->x)
	{
	    ld->bbox[BOXLEFT] = v1->x;
	    ld->bbox[BOXRIGHT] = v2->x;
	}
	else
	{
	    ld->bbox[BOXLEFT] = v2->x;
	    ld->bbox[BOXRIGHT] = v1->x;
	}

	if (v1->y < v2->y)
	{
	    ld->bbox[BOXBOTTOM] = v1->y;
	    ld->bbox[BOXTOP] = v2->y;
	}
	else
	{
	    ld->bbox[BOXBOTTOM] = v2->y;
	    ld->bbox[BOXTOP] = v1->y;
	}

	ld->sidenum[0] = SHORT(mld->sidenum[0]);
	ld->sidenum[1] = SHORT(mld->sidenum[1]);

	if (ld->sidenum[0] != -1)
	    ld->frontsector = sides[ld->sidenum[0]].sector;
	else
	    ld->frontsector = 0;

	if (ld->sidenum[1] != -1)
	    ld->backsector = sides[ld->sidenum[1]].sector;
	else
	    ld->backsector = 0;
    }

    W_ReleaseLumpNum(lump);
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
	sd->sector = &sectors[SHORT(msd->sector)];
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
	ss->sector = seg->sidedef->sector;
    }

    // count number of lines in each sector
    li = lines;
    totallines = 0;
    for (i=0 ; i<numlines ; i++, li++)
    {
	totallines++;
	li->frontsector->linecount++;

	if (li->backsector && li->backsector != li->frontsector)
	{
	    li->backsector->linecount++;
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
        li = &lines[i];

        if (li->frontsector != NULL)
        {
            sector = li->frontsector;

            sector->lines[sector->linecount] = li;
            ++sector->linecount;
        }

        if (li->backsector != NULL && li->frontsector != li->backsector)
        {
            sector = li->backsector;

            sector->lines[sector->linecount] = li;
            ++sector->linecount;
        }
    }
    
    // Generate bounding boxes for sectors
	
    sector = sectors;
    for (i=0 ; i<numsectors ; i++, sector++)
    {
	M_ClearBox (bbox);

	for (j=0 ; j<sector->linecount; j++)
	{
            li = sector->lines[j];

            M_AddToBox (bbox, li->v1->x, li->v1->y);
            M_AddToBox (bbox, li->v2->x, li->v2->y);
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

#ifndef SAT_KEEP_REJECT
    /* SATURN streaming (DEFAULT; build with -DSAT_KEEP_REJECT=1 to force-keep):
       the REJECT matrix is numsectors^2/8 bytes -- 45-125 KB of PU_LEVEL on big
       Doom II maps, the largest LATE level-load alloc (a fragmentation-OOM magnet,
       e.g. the "Z_Malloc fail 68192" freeze).  It is PURELY a P_CheckSight early-
       out optimization: a NULL rejectmatrix just makes sight checks do the full
       LOS test (a bit more CPU, imperceptible at Saturn frame rates -- see the
       NULL guard in p_sight.c P_CheckSight).  In the tight CD-streaming zone that
       45-125 KB matters far more than the sight-check speedup, so skip it.  Non-
       streaming (shareware/cart; DoomJo always has sat_streaming_mode==0) keeps
       the matrix unchanged. */
    if (sat_streaming_mode)
    {
        rejectmatrix = NULL;
        return;
    }
#endif

    // Calculate the size that the REJECT lump *should* be.

    minlength = (numsectors * numsectors + 7) / 8;

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

    if (segs_hw && vertexes_hw)         /* staged segs read staged vertexes */
    {
        int i;
        for (i = 0; i < numsegs; i++)
        {
            segs_hw[i].v1 = vertexes_hw + (segs_lw[i].v1 - vertexes_lw);
            segs_hw[i].v2 = vertexes_hw + (segs_lw[i].v2 - vertexes_lw);
        }
    }
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

//
// P_SetupLevel
//
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

    // SATURN: free the previous level's bounded texture-cache pool (a PU_STATIC
    // slab that survives Z_FreeTags) and drop its composite back-pointers BEFORE
    // this level's geometry loads, so geometry gets the full free zone.
    R_ClearTextureCaches ();

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

    // note: most of this ordering is important
    P_LoadBlockMap (lumpnum+ML_BLOCKMAP);
    P_LoadVertexes (lumpnum+ML_VERTEXES);
    P_LoadSectors (lumpnum+ML_SECTORS);
    P_LoadSideDefs (lumpnum+ML_SIDEDEFS);

    P_LoadLineDefs (lumpnum+ML_LINEDEFS);
    P_LoadSubsectors (lumpnum+ML_SSECTORS);
    P_LoadNodes (lumpnum+ML_NODES);
    P_LoadSegs (lumpnum+ML_SEGS);

    P_GroupLines ();
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
    R_SetupTextureCaches ();

#ifdef SAT_SND_PRECACHE
    // SATURN: warm this level's sound effects into SCSP RAM (off the gameplay frame).
    // Runs in both cart and CD-streaming modes -- the SCSP is separate from the Doom zone
    // so there is no OOM risk; the win is largest in streaming mode (it also moves the
    // blocking CD read to the load screen).  Skipped for demos (precache == false).
    if (precache)
        SAT_PrecacheLevelSounds ();
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



