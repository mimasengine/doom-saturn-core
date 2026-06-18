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
//	Here is a core component: drawing the floors and ceilings,
//	 while maintaining a per column clipping list only.
//	Moreover, the sky areas have to be determined.
//


/* SATURN: O3 for the floor/ceiling renderer — 8 FixedMul calls per visible span. */
#pragma GCC optimize("O3")

#include <stdio.h>
#include <stdlib.h>

#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"
#include "r_parallel.h"	/* SATURN PERF Phase-0a: RP_FlatCache/MakeSpans brackets (profiler) */



planefunction_t		floorfunc;
planefunction_t		ceilingfunc;

//
// opening
//

// Here comes the obnoxious "visplane".
/* SATURN: 128 was too low for complex E1 scenes.  Array heap-allocated in
   R_InitPlanes (zone heap, low WRAM) so it doesn't count against the 1MB
   high WRAM BSS limit.  512 * 664B = 332KB of the 864KB zone heap; ~532KB
   remains for the game — sufficient for E1 shareware maps.
   vpsort[] stays in BSS (512 * 4B = 2KB, harmless).                      */
#define MAXVISPLANES	512
static visplane_t	*visplanes;
/* SATURN: peak visplane count per frame, exposed for the debug overlay. */
int r_visplane_peak = 0;
visplane_t*		lastvisplane;
visplane_t*		floorplane;
visplane_t*		ceilingplane;

/* SATURN PERF L1: visplane hash (d32xr-style).  Vanilla R_FindPlane is a linear
   O(n) scan over visplanes, called per subsector for floor AND ceiling => O(n^2)
   of SLOW low-WRAM reads (visplanes live in the zone heap) in plane-heavy rooms.
   A picnum/height/light hash bucket cuts the scan to same-key planes.
   BYTE-IDENTICAL to the linear scan: a plane is appended at the bucket TAIL (FIFO),
   so chain order == creation/array order == vanilla's first-match-in-array-order.
   Both creation sites (R_FindPlane + R_CheckPlane splits) feed the hash, exactly
   the set vanilla scans.  Chain links are SHORT indices in BSS (fast high-WRAM),
   so the walk never pointer-chases the slow visplane struct except to compare the
   three key fields.  Gated for a hardware A/B (set 0 = original linear scan).
   Pure C, DoomJo-safe, no new cross-CPU coherency surface (master-only generation). */
#define SAT_VISPLANE_HASH 1
#if SAT_VISPLANE_HASH
#define VISPLANE_HASH_SIZE 128			/* power of two */
#define VISPLANE_HASH_MASK (VISPLANE_HASH_SIZE-1)
static short	visplane_hashhead[VISPLANE_HASH_SIZE];	/* first index in bucket, -1 = empty */
static short	visplane_hashtail[VISPLANE_HASH_SIZE];	/* last index (for FIFO append)        */
static short	visplane_hashnext[MAXVISPLANES];	/* next index in chain, -1 = end       */

static int R_PlaneHash (fixed_t height, int picnum, int lightlevel)
{
    /* Distribution only -- the field compare still verifies the exact key, so any
       deterministic mix stays byte-identical. */
    unsigned h = (unsigned)picnum * 3u
	       + (unsigned)lightlevel
	       + ((unsigned)height >> 16);
    return (int)(h & VISPLANE_HASH_MASK);
}

static void R_HashInsert (int bucket, int idx)
{
    visplane_hashnext[idx] = -1;
    if (visplane_hashhead[bucket] < 0)
	visplane_hashhead[bucket] = (short)idx;
    else
	visplane_hashnext[visplane_hashtail[bucket]] = (short)idx;
    visplane_hashtail[bucket] = (short)idx;
}
#endif

// ?
#define MAXOPENINGS	SCREENWIDTH*64
short			openings[MAXOPENINGS];
short*			lastopening;


//
// Clip values are the solid pixel bounding the range.
//  floorclip starts out SCREENHEIGHT
//  ceilingclip starts out -1
//
short			floorclip[SCREENWIDTH];
short			ceilingclip[SCREENWIDTH];

//
// spanstart holds the start of a plane span
// initialized to 0 at start
//
int			spanstart[SCREENHEIGHT];
int			spanstop[SCREENHEIGHT];

//
// texture mapping
//
lighttable_t**		planezlight;
fixed_t			planeheight;

fixed_t			yslope[SCREENHEIGHT];
fixed_t			distscale[SCREENWIDTH];
fixed_t			basexscale;
fixed_t			baseyscale;

fixed_t			cachedheight[SCREENHEIGHT];
fixed_t			cacheddistance[SCREENHEIGHT];
fixed_t			cachedxstep[SCREENHEIGHT];
fixed_t			cachedystep[SCREENHEIGHT];


/* SATURN DIAG: visplane-corruption hunt.  Symptom on hardware: a visplane
   reaches R_DrawPlanes with maxx ~2315 (>> SCREENWIDTH).  That makes
   `pl->top[pl->maxx+1]=0xff` stomp the zone heap and the span loop iterate
   thousands of times reading OOB top[]/bottom[] -> screen-wide moire + a
   flood of OOB R_MapPlane calls (old "MPOOB") + a big FPS hit.
   We (1) SKIP such visplanes in R_DrawPlanes (a maxx>=SCREENWIDTH plane is
   definitionally invalid; drawing it corrupts memory), and (2) instrument
   both ends to localise the source:
     - VPIN  : R_CheckPlane received a start/stop outside [0,SCREENWIDTH).
     - VPDRAW: a corrupt minx/maxx reached R_DrawPlanes.
     - MPOOB : R_MapPlane skipped OOB args (should fall to ~0 once VPDRAW skips).
   If VPIN fires  -> a caller (BSP/seg clip) passes a bad column range.
   If VPIN stays 0 while VPDRAW fires -> the plane was stomped by a foreign
   write AFTER being built (heap corruption elsewhere).
   Counters reset each frame in R_ClearPlanes, printed once in R_DrawPlanes.
   Set VP_DIAG 0 to silence.  (OFF now: zero corruption confirmed across all of
   hardware level 1 on CPU blit; the R_DrawPlanes/R_MapPlane skip-guards stay,
   only the counting + per-frame prints are dropped -- a real per-frame win.)   */
#define VP_DIAG 0
#if VP_DIAG
extern void dbg_print(int x, int y, char *str);
static int vp_in_bad,   vp_in_lo,   vp_in_hi;
static int vp_draw_bad, vp_draw_mn, vp_draw_mx;
static int vp_map_bad,  vp_map_x1,  vp_map_x2, vp_map_y;
#endif



/* SATURN: forward declaration -- the definition lives just above R_DrawPlanes,
   but R_MapPlane (which uses it for the step-2 generation skip) comes first. */
extern int sat_potato_floors;
/* SATURN: framebuffer row/column lookup -- also externed below for R_DrawPlanes;
   hoisted here so R_MapPlane's inline Potato span memset can reach them. */
extern byte *ylookup[];
extern int   columnofs[];

//
// R_InitPlanes
// Only at game startup.
//
void R_InitPlanes (void)
{
    /* SATURN: allocate from zone heap (low WRAM) — keeps high WRAM BSS within
       the 1MB limit.  Z_Init runs before R_Init so the heap is ready. */
    visplanes = Z_Malloc(MAXVISPLANES * sizeof(visplane_t), PU_STATIC, 0);
    r_visplane_peak = 0;
}


//
// R_MapPlane
//
// Uses global vars:
//  planeheight
//  ds_source
//  basexscale
//  baseyscale
//  viewx
//  viewy
//
// BASIC PRIMITIVE
//
void
R_MapPlane
( int		y,
  int		x1,
  int		x2 )
{
    angle_t	angle;
    fixed_t	distance;
    fixed_t	length;
    unsigned	index;
	
    /* SATURN: guard OOB BEFORE the RANGECHECK below.  RANGECHECK is defined
       (doomdef.h), so its I_Error("R_MapPlane: ...") would otherwise fire
       first and HALT the game (-> DG_Fatal freeze) -- e.g. "R_MapPlane:
       202,202 at 255" at level load, where 255 = 0xFF = an uninitialised
       visplane top[]/bottom[] sentinel leaking in as y.  Skip the bad span
       instead of halting; also protects cachedheight[y] etc. from an OOB
       index. */
    if (x2 < x1 || x1 < 0 || x2 >= viewwidth ||
        y > viewheight || (unsigned int)y >= (unsigned int)SCREENHEIGHT)
    {
#if VP_DIAG
        vp_map_bad++; vp_map_x1 = x1; vp_map_x2 = x2; vp_map_y = y;
#endif
        return;
    }

#ifdef RANGECHECK
    if (x2 < x1
     || x1 < 0
     || x2 >= viewwidth
     || y > viewheight)
    {
	I_Error ("R_MapPlane: %i, %i at %i",x1,x2,y);
    }
#endif

    if (planeheight != cachedheight[y])
    {
	cachedheight[y] = planeheight;
	distance = cacheddistance[y] = FixedMul (planeheight, yslope[y]);
	ds_xstep = cachedxstep[y] = FixedMul (distance,basexscale);
	ds_ystep = cachedystep[y] = FixedMul (distance,baseyscale);
    }
    else
    {
	distance = cacheddistance[y];
	ds_xstep = cachedxstep[y];
	ds_ystep = cachedystep[y];
    }
	
    /* SATURN PERF (step 2): in Potato floors the span executor memsets a fixed
       texel shaded by ds_colormap and IGNORES ds_xfrac/yfrac (and xstep/ystep) ->
       skip this per-span texture-coordinate math (length/angle + 2 FixedMul + 2
       trig-table reads).  Cuts plane GENERATION (REC's "P"), compounding the EX
       fill win Potato already gives.  Gated on the flag so pot0 is byte-identical
       and DoomJo (no Potato) is unaffected. */
    if (!sat_potato_floors)
    {
	length = FixedMul (distance,distscale[x1]);
	angle = (viewangle + xtoviewangle[x1])>>ANGLETOFINESHIFT;
	ds_xfrac = viewx + FixedMul(finecosine[angle], length);
	ds_yfrac = -viewy - FixedMul(finesine[angle], length);
    }

    if (fixedcolormap)
	ds_colormap = fixedcolormap;
    else
    {
	index = distance >> LIGHTZSHIFT;
	
	if (index >= MAXLIGHTZ )
	    index = MAXLIGHTZ-1;

	ds_colormap = planezlight[index];
    }

    /* SATURN PERF (REC P-cut): in Potato floors the span is a flat memset (one
       distance-shaded colour).  Draw it INLINE here and SKIP the command record --
       RP_RecordSpan writes a 32-byte command to slow low work-RAM (the command queue)
       and the executor reads it back; both are pure memory traffic on the memory-bound
       plane phase (P), the #1 REC cost at pot0/pot1.  Pixel-safe: floor/ceiling spans
       never overlap the slave's concurrently-drawn wall/sprite columns (Doom marks no
       overdraw between planes and walls), the master's writes are write-through (the
       blit purges before reading), so there is no new cross-CPU coherency surface.
       Mirrors rp_exec_span / rp_exec_span_low byte-for-byte (fixed flat texel,
       distance-shaded via ds_colormap).  Gated on sat_potato_floors -> pot0 stays
       byte-identical and DoomJo (never sets Potato) is unaffected.  Set
       SAT_POTATO_INLINE_SPANS 0 to revert to the record+execute path. */
#define SAT_POTATO_INLINE_SPANS 1
#if SAT_POTATO_INLINE_SPANS
#define R_POTATO_TEXEL 2080   /* centre texel of a 64x64 flat (v32,u32); == r_parallel.c POTATO_TEXEL */
    if (sat_potato_floors)
    {
	byte  c = ds_colormap[ds_source[R_POTATO_TEXEL]];
	byte *d;
	if (detailshift)
	{
	    d = ylookup[y] + columnofs[x1 << 1];
	    memset(d, c, (size_t)((x2 - x1 + 1) * 2));   /* low-detail: 2 screen px / source */
	}
	else
	{
	    d = ylookup[y] + columnofs[x1];
	    memset(d, c, (size_t)(x2 - x1 + 1));
	}
	return;
    }
#endif

    ds_y = y;
    ds_x1 = x1;
    ds_x2 = x2;

    // high or low detail
    spanfunc ();
}


//
// R_ClearPlanes
// At begining of frame.
//
void R_ClearPlanes (void)
{
    int		i;
    angle_t	angle;
    
    // opening / clipping determination
    for (i=0 ; i<viewwidth ; i++)
    {
	floorclip[i] = viewheight;
	ceilingclip[i] = -1;
    }

    /* SATURN: record peak visplane usage for the debug overlay. */
    {
        int n = (int)(lastvisplane - visplanes);
        if (n > r_visplane_peak) r_visplane_peak = n;
    }
#if VP_DIAG
    vp_in_bad = vp_draw_bad = vp_map_bad = 0;   /* per-frame reset */
#endif
    lastvisplane = visplanes;
    lastopening = openings;

#if SAT_VISPLANE_HASH
    /* SATURN PERF L1: empty every hash bucket for the new frame (0xff -> -1).
       Only heads need clearing; a tail is read only once its head is set. */
    memset (visplane_hashhead, 0xff, sizeof(visplane_hashhead));
#endif

    // texture calculation
    memset (cachedheight, 0, sizeof(cachedheight));

    // left to right mapping
    angle = (viewangle-ANG90)>>ANGLETOFINESHIFT;
	
    // scale will be unit scale at SCREENWIDTH/2 distance
    basexscale = FixedDiv (finecosine[angle],centerxfrac);
    baseyscale = -FixedDiv (finesine[angle],centerxfrac);
}




//
// R_FindPlane
//
visplane_t*
R_FindPlane
( fixed_t	height,
  int		picnum,
  int		lightlevel )
{
    visplane_t*	check;
#if SAT_VISPLANE_HASH
    int		bucket;
    int		idx;
#endif

    if (picnum == skyflatnum)
    {
	height = 0;			// all skys map together
	lightlevel = 0;
    }

#if SAT_VISPLANE_HASH
    /* SATURN PERF L1: scan only the planes of this key's bucket (FIFO chain ==
       array order, so the first match is vanilla's first match). */
    bucket = R_PlaneHash (height, picnum, lightlevel);
    for (idx = visplane_hashhead[bucket]; idx >= 0; idx = visplane_hashnext[idx])
    {
	check = visplanes + idx;
	if (height == check->height
	    && picnum == check->picnum
	    && lightlevel == check->lightlevel)
	{
	    return check;
	}
    }
#else
    for (check=visplanes; check<lastvisplane; check++)
    {
	if (height == check->height
	    && picnum == check->picnum
	    && lightlevel == check->lightlevel)
	{
	    break;
	}
    }

    if (check < lastvisplane)
	return check;
#endif

    if (lastvisplane - visplanes == MAXVISPLANES)
	I_Error ("R_FindPlane: no more visplanes");

    check = lastvisplane;
    lastvisplane++;

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->minx = SCREENWIDTH;
    check->maxx = -1;

    memset (check->top,0xff,sizeof(check->top));

#if SAT_VISPLANE_HASH
    R_HashInsert (bucket, (int)(check - visplanes));
#endif

    return check;
}


//
// R_CheckPlane
//
visplane_t*
R_CheckPlane
( visplane_t*	pl,
  int		start,
  int		stop )
{
    int		intrl;
    int		intrh;
    int		unionl;
    int		unionh;
    int		x;

#if VP_DIAG
    /* A valid wall column range is start,stop in [0,SCREENWIDTH).  Anything
       outside means the caller (BSP/seg clip) handed us garbage -> would
       propagate straight into pl->maxx/minx. */
    if (start < 0 || start >= SCREENWIDTH || stop < 0 || stop >= SCREENWIDTH)
    {
        vp_in_bad++; vp_in_lo = start; vp_in_hi = stop;
    }
#endif

    if (start < pl->minx)
    {
	intrl = pl->minx;
	unionl = start;
    }
    else
    {
	unionl = pl->minx;
	intrl = start;
    }
	
    if (stop > pl->maxx)
    {
	intrh = pl->maxx;
	unionh = stop;
    }
    else
    {
	unionh = pl->maxx;
	intrh = stop;
    }

    for (x=intrl ; x<= intrh ; x++)
	if (pl->top[x] != 0xff)
	    break;

    if (x > intrh)
    {
	pl->minx = unionl;
	pl->maxx = unionh;

	// use the same one
	return pl;		
    }
	
    /* SATURN: vanilla R_CheckPlane had no bounds check here — silent overflow
       into the zone heap, corrupting allocator blocks and causing a hang with
       no I_Error.  Guard added to match R_FindPlane's existing check. */
    if (lastvisplane - visplanes == MAXVISPLANES)
        I_Error("R_CheckPlane: no more visplanes");

    // make a new visplane
    lastvisplane->height = pl->height;
    lastvisplane->picnum = pl->picnum;
    lastvisplane->lightlevel = pl->lightlevel;

    pl = lastvisplane++;
    pl->minx = start;
    pl->maxx = stop;

    memset (pl->top,0xff,sizeof(pl->top));

#if SAT_VISPLANE_HASH
    /* SATURN PERF L1: a split plane is scanned by vanilla R_FindPlane too, so it
       must join the hash (FIFO -> preserves first-match order). */
    R_HashInsert (R_PlaneHash (pl->height, pl->picnum, pl->lightlevel),
		  (int)(pl - visplanes));
#endif

    return pl;
}


//
// R_MakeSpans
//
void
R_MakeSpans
( int		x,
  int		t1,
  int		b1,
  int		t2,
  int		b2 )
{
    while (t1 < t2 && t1<=b1)
    {
	R_MapPlane (t1,spanstart[t1],x-1);
	t1++;
    }
    while (b1 > b2 && b1>=t1)
    {
	R_MapPlane (b1,spanstart[b1],x-1);
	b1--;
    }
	
    while (t2 < t1 && t2<=b2)
    {
	spanstart[t2] = x;
	t2++;
    }
    while (b2 > b1 && b2>=t2)
    {
	spanstart[b2] = x;
	b2--;
    }
}



/* SATURN: sky -> VDP2 NBG0 layer.  When set by the platform, R_DrawPlanes leaves
   the sky region as index 0 (the VDP2 transparent code) instead of drawing the
   sky texture.  Default 0 => vanilla software sky (DoomJo, which has no VDP2 sky
   layer, links the same core and keeps drawing the sky). */
int sat_vdp2_sky = 0;
/* SATURN: floor -> VDP2 RBG0 hardware Mode-7 plane.  When set by the platform,
   R_DrawPlanes leaves the FLOOR visplanes (a flat below the eye) as index 0 so the
   RBG0 floor composited behind the framebuffer shows through -- exactly like the sky
   skip.  Ceilings (above the eye) still draw in software.  Default 0 => DoomJo and the
   normal build draw floors normally. */
int sat_vdp2_floor = 0;
/* SATURN: the player's CURRENT floor (height + flat) -- the single floor RBG0 renders.
   Set each frame in R_DrawPlanes from the view sector.  The floor-skip leaves ONLY the
   visplanes matching BOTH (so coplanar same-flat floors are covered, other heights/flats
   stay software); the platform reads sat_vdp2_floor_h to anchor the RBG0 plane's height. */
fixed_t sat_vdp2_floor_h   = 0;
int     sat_vdp2_floor_pic = -1;
/* SATURN: colormap for the RBG0 floor = the player sector's light level (+extralight),
   so the hardware floor dims with the room like the software floor.  The HW plane is one
   uniform brightness (no per-distance gradient -- that would need a per-line K-table), so
   this is the sector base shade.  Set each frame in R_DrawPlanes; 0 => full bright. */
lighttable_t *sat_vdp2_floor_cmap = 0;
/* SATURN: the player's-floor flat data (64x64 = 4096 bytes) for the platform to swizzle
   into the RBG0 cells.  Same lump the software floor would use (animated-flat aware via
   flattranslation).  Returns 0 outside a level.  Off-path for DoomJo (never called). */
unsigned char *sat_vdp2_floor_data(void)
{
    if (sat_vdp2_floor_pic < 0) return 0;
    return (unsigned char *)W_CacheLumpNum(firstflat + flattranslation[sat_vdp2_floor_pic],
                                           PU_STATIC);
}
/* SATURN: Potato mode -- draw floor/ceiling spans as a single distance-shaded
   colour instead of texture-mapping them (big EX/fillrate win).  Set by the
   platform; default 0 (vanilla textured floors, incl. DoomJo). */
int sat_potato_floors = 0;
/* SATURN: Potato walls -- opaque wall columns drawn as a single distance-shaded
   colour (a fixed texel), in rp_exec_col.  Sprites (masked RP_COL) stay textured.
   Default 0.  Aimed at the future 2/4-player split-screen builds (more views,
   tighter budget). */
int sat_potato_walls = 0;
extern byte *ylookup[];
extern int   columnofs[];

//
// R_DrawPlanes
// At the end of each frame.
//
void R_DrawPlanes (void)
{
    visplane_t*		pl;
    int			light;
    int			x;
    int			stop;
    int			angle;
    int                 lumpnum;
				
#ifdef RANGECHECK
    if (ds_p - drawsegs > MAXDRAWSEGS)
	I_Error ("R_DrawPlanes: drawsegs overflow (%i)",
		 ds_p - drawsegs);
    
    if (lastvisplane - visplanes > MAXVISPLANES)
	I_Error ("R_DrawPlanes: visplane overflow (%i)",
		 lastvisplane - visplanes);
    
    if (lastopening - openings > MAXOPENINGS)
	I_Error ("R_DrawPlanes: opening overflow (%i)",
		 lastopening - openings);
#endif

    /* SATURN: RBG0 renders the player's CURRENT floor only.  Capture the view sector's
       floor height + flat here; the floor-skip below leaves just those visplanes as
       index 0 (other heights/flats keep drawing in software). */
    if (sat_vdp2_floor)
    {
	sector_t *vs = R_PointInSubsector(viewx, viewy)->sector;
	sat_vdp2_floor_h   = vs->floorheight;
	sat_vdp2_floor_pic = vs->floorpic;
	/* light: map the view sector's light band (+extralight) to a colormap level
	   (0 = brightest .. NUMCOLORMAPS-1 = darkest) so the RBG0 floor dims with the room. */
	{
	    int li = (vs->lightlevel >> LIGHTSEGSHIFT) + extralight;
	    int lvl;
	    if (li < 0) li = 0; else if (li >= LIGHTLEVELS) li = LIGHTLEVELS - 1;
	    lvl = (LIGHTLEVELS - 1 - li) * NUMCOLORMAPS / LIGHTLEVELS;   /* bright band -> level 0 */
	    if (lvl < 0) lvl = 0; else if (lvl >= NUMCOLORMAPS) lvl = NUMCOLORMAPS - 1;
	    sat_vdp2_floor_cmap = colormaps + lvl * 256;
	}
    }

    /* SATURN: insertion-sort visplanes by picnum so consecutive R_MakeSpans calls
       share the same 4KB flat in the SH-2 D-cache instead of evicting it.
       n ≤ 128 → O(n²) is negligible.                                          */
    {
        static visplane_t *vpsort[MAXVISPLANES];
        int i, j, vp_n = (int)(lastvisplane - visplanes);
        for (i = 0; i < vp_n; i++) vpsort[i] = &visplanes[i];
        for (i = 1; i < vp_n; i++)
        {
            visplane_t *t = vpsort[i];
            for (j = i - 1; j >= 0 && vpsort[j]->picnum > t->picnum; j--)
                vpsort[j+1] = vpsort[j];
            vpsort[j+1] = t;
        }
        for (i = 0; i < vp_n; i++) { pl = vpsort[i];
	if (pl->minx > pl->maxx)
	    continue;

	/* SATURN: skip a corrupt visplane (minx<0 or maxx>=SCREENWIDTH).
	   Drawing it would `top[maxx+1]=0xff` into the heap and loop
	   maxx+1 times reading OOB top[]/bottom[].  See VP_DIAG block. */
	if (pl->minx < 0 || pl->maxx >= SCREENWIDTH)
	{
#if VP_DIAG
	    vp_draw_bad++; vp_draw_mn = pl->minx; vp_draw_mx = pl->maxx;
#endif
	    continue;
	}

	
	// sky flat
	if (pl->picnum == skyflatnum)
	{
	    // SATURN: sky -> VDP2.  Leave the sky region as index 0 (the VDP2
	    // transparent code) instead of drawing it; the platform composites a
	    // scrolling VDP2 sky layer behind the framebuffer.  Writing 0 directly
	    // (no colfunc/R_GetColumn) also drops the sky from REC/EX and the command
	    // count.  sat_vdp2_sky is 0 by default so DoomJo keeps its software sky.
	    if (sat_vdp2_sky)
	    {
		for (x=pl->minx ; x <= pl->maxx ; x++)
		{
		    int yl = pl->top[x];
		    int yh = pl->bottom[x];
		    int n;
		    if (yl > yh) continue;
		    n = yh - yl + 1;
		    if (detailshift)
		    {
			/* low-detail: the visplane x is the halved column; each one
			   covers two real screen pixels (x<<1, x<<1+1). */
			int sx = x << 1;
			byte *d0 = ylookup[yl] + columnofs[sx];
			byte *d1 = ylookup[yl] + columnofs[sx + 1];
			do { *d0 = 0; *d1 = 0; d0 += SCREENWIDTH; d1 += SCREENWIDTH; }
			while (--n);
		    }
		    else
		    {
			byte *d = ylookup[yl] + columnofs[x];
			do { *d = 0; d += SCREENWIDTH; } while (--n);
		    }
		}
		continue;
	    }

	    dc_iscale = pspriteiscale>>detailshift;

	    // Sky is allways drawn full bright,
	    //  i.e. colormaps[0] is used.
	    // Because of this hack, sky is not affected
	    //  by INVUL inverse mapping.
	    dc_colormap = colormaps;
	    dc_texturemid = skytexturemid;
	    for (x=pl->minx ; x <= pl->maxx ; x++)
	    {
		dc_yl = pl->top[x];
		dc_yh = pl->bottom[x];

		if (dc_yl <= dc_yh)
		{
		    angle = (viewangle + xtoviewangle[x])>>ANGLETOSKYSHIFT;
		    dc_x = x;
		    dc_source = R_GetColumn(skytexture, angle);
		    colfunc ();
		}
	    }
	    continue;
	}

	// SATURN: floor -> VDP2 RBG0.  Leave ONLY the player's-floor visplanes (matching
	// height AND flat) as index 0 so the hardware Mode-7 RBG0 floor -- which is anchored
	// at that height -- shows through.  Other floor heights / flats and all ceilings keep
	// drawing in software.  Off by default (DoomJo).
	if (sat_vdp2_floor
	    && pl->height == sat_vdp2_floor_h
	    && pl->picnum == sat_vdp2_floor_pic)
	{
	    for (x=pl->minx ; x <= pl->maxx ; x++)
	    {
		int yl = pl->top[x];
		int yh = pl->bottom[x];
		int n;
		if (yl > yh) continue;
		n = yh - yl + 1;
		if (detailshift)
		{
		    int sx = x << 1;
		    byte *d0 = ylookup[yl] + columnofs[sx];
		    byte *d1 = ylookup[yl] + columnofs[sx + 1];
		    do { *d0 = 0; *d1 = 0; d0 += SCREENWIDTH; d1 += SCREENWIDTH; } while (--n);
		}
		else
		{
		    byte *d = ylookup[yl] + columnofs[x];
		    do { *d = 0; d += SCREENWIDTH; } while (--n);
		}
	    }
	    continue;
	}

	// regular flat
        lumpnum = firstflat + flattranslation[pl->picnum];
	RP_FlatCacheEnter();   /* SATURN PERF Phase-0a: per-visplane flat allocator cost (c P) */
	ds_source = W_CacheLumpNum(lumpnum, PU_STATIC);
	RP_FlatCacheLeave();

	planeheight = abs(pl->height-viewz);
	light = (pl->lightlevel >> LIGHTSEGSHIFT)+extralight;

	if (light >= LIGHTLEVELS)
	    light = LIGHTLEVELS-1;

	if (light < 0)
	    light = 0;

	planezlight = zlight[light];

	pl->top[pl->maxx+1] = 0xff;
	pl->top[pl->minx-1] = 0xff;
		
	stop = pl->maxx + 1;

	RP_MakeSpansEnter();   /* SATURN PERF Phase-0a: R_MakeSpans walk + R_MapPlane (c P) */
	for (x=pl->minx ; x<= stop ; x++)
	{
	    R_MakeSpans(x,pl->top[x-1],
			pl->bottom[x-1],
			pl->top[x],
			pl->bottom[x]);
	}
	RP_MakeSpansLeave();

	/* SATURN PERF (RBG0 candidate sizing, profiler): hand this flat to the
	   profiler so it can size the single largest floor/ceiling -- the VDP2 RBG0
	   offload candidate -- against the total plane fill.  Outside the RP_MakeSpans
	   timer so it doesn't pollute the P 'm' sub-time; no-op unless RP_PROF. */
	RP_PlanePixels(pl->picnum, (int)pl->height, pl->minx, pl->maxx,
		       pl->top, pl->bottom);

	RP_FlatCacheEnter();
        W_ReleaseLumpNum(lumpnum);
	RP_FlatCacheLeave();
        } /* SATURN: end sorted visplane loop */
    }

#if VP_DIAG
    /* One print per frame (rows 11/13/14 per the overlay map). */
    {
        static char b[48];
        snprintf(b, sizeof b, "MPOOB  n%-5d x%d>%d y%d   ",
                 vp_map_bad, vp_map_x1, vp_map_x2, vp_map_y);
        dbg_print(0, 11, b);
        snprintf(b, sizeof b, "VPDRAW n%-5d mn%d mx%d     ",
                 vp_draw_bad, vp_draw_mn, vp_draw_mx);
        dbg_print(0, 13, b);
        snprintf(b, sizeof b, "VPIN   n%-5d lo%d hi%d     ",
                 vp_in_bad, vp_in_lo, vp_in_hi);
        dbg_print(0, 14, b);
    }
#endif
}
