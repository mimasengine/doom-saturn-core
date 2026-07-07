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
   vpsort[] stays in BSS (512 * 4B = 2KB, harmless).
   SATURN streaming: Mimas overrides this to 256 via -DMAXVISPLANES in its
   Makefile to reclaim ~166KB of the zone for big-WAD (Doom II) streaming, where
   the per-level PU_STATIC floor + geometry must fit the 884KB zone with no cart.
   256 is still 2x vanilla's 128; overflow -> clean I_Error (r_plane.c:476/567),
   not corruption.  Core default stays 512 (DoomJo unchanged).               */
#ifndef MAXVISPLANES
#define MAXVISPLANES	512
#endif
static visplane_t	*visplanes;
/* SATURN: peak visplane count per frame, exposed for the debug overlay. */
int r_visplane_peak = 0;
/* SATURN VALIDATION (#1 sizing): peak SUM of live-plane column-spans per frame =
   the top-bytes a TIGHT pooled arena would need (x2 for bottom).  Tells us, on Ymir
   (deterministic, identical to HW), exactly how much a span pool could save WITHOUT
   committing to the invasive layout.  Cheap per-frame loop (~n planes), both ports. */
int r_visplane_coverage_peak = 0;
/* high-water BYTES of the #1 span pool (0 when SAT_VISPLANE_POOL is off) */
int r_visplane_pool_peak = 0;
/* SATURN: planes that overflowed VP_POOL_PLANES this frame -> handed a SHARED
   fallback slice (harmless span glitch, NOT a crash).  If this is ever non-zero
   on a scene you care about, raise VP_POOL_PLANES.  (On the overlay.) */
int r_visplane_pool_ovf = 0;
visplane_t*		lastvisplane;
visplane_t*		floorplane;
visplane_t*		ceilingplane;

#if SAT_VISPLANE_POOL
/* SATURN #1: the shared per-frame visplane span pool (bump-allocated, reset each
   frame in R_ClearPlanes).  Holds VP_POOL_PLANES plane-pairs of (top+bottom) slices,
   each slice (SCREENWIDTH+2) bytes; the returned pointer is base+1 so [-1..SCREENWIDTH]
   (the old pad slots) stay in-bounds.  Default cap == MAXVISPLANES => never overflows
   (one pair per plane, plane count <= MAXVISPLANES) and gives NO memory saving yet
   (correctness A/B only).  Lower VP_POOL_PLANES, gated on r_visplane_peak telemetry,
   to realise the saving (overflow => I_Error, same hard-limit semantics as the pool). */
#ifndef VP_POOL_PLANES
#define VP_POOL_PLANES   MAXVISPLANES   /* core default = no saving (safe). Mimas overrides via -DVP_POOL_PLANES=N in its Makefile to reclaim zone for big WADs; size to r_visplane_peak + margin. */
#endif
#define VP_SLICE_BYTES   (SCREENWIDTH + 2)
static byte	*plane_pool;
static byte	*plane_pool_ptr;
static byte	*plane_pool_end;

static byte vp_fallback[VP_SLICE_BYTES];   /* shared slice handed out on overflow */
static byte *R_PoolSlice (void)
{
    byte *p = plane_pool_ptr;
    int   used;
    if (p + VP_SLICE_BYTES > plane_pool_end)
    {
	/* SATURN: graceful overflow.  Hand out a shared fallback slice instead of
	   halting; overflowing planes then share one (top+bottom) slice -> their
	   spans glitch visually but the renderer never crashes.  Sized so this
	   essentially never trips at normal visplane counts; bump VP_POOL_PLANES
	   if r_visplane_pool_ovf shows up. */
	r_visplane_pool_ovf++;
	return vp_fallback + 1;
    }
    plane_pool_ptr += VP_SLICE_BYTES;
    used = (int)(plane_pool_ptr - plane_pool);
    if (used > r_visplane_pool_peak)
	r_visplane_pool_peak = used;
    return p + 1;   /* base+1: the [-1] pad slot is p[0], [SCREENWIDTH] is p[SCREENWIDTH+1] */
}
#endif

/* SATURN P0 (crash-proofing, endgame): graceful visplane-COUNT overflow.  When the visplane
   array fills (count == MAXVISPLANES) R_FindPlane / R_CheckPlane used to I_Error -- a hard
   freeze, fatal on a wide-open big-WAD vista.  Instead hand out this shared write-sink plane:
   it is NOT in [visplanes, lastvisplane), so R_DrawPlanes never draws it, and it is never
   inserted in the hash (its array index would be OOB).  The excess flat then silently fails to
   render (a localised HOM on a pathologically plane-dense frame), the same graceful degrade as
   overflowsprite and the vp_fallback span slice -- no crash.  Its own static slices keep the
   caller's memset + span writes off the zone heap. */
int r_visplane_ovf = 0;   /* overflow hand-outs (endgame limits telemetry; peers r_visplane_peak) */
#if SAT_VISPLANE_POOL
static byte overflow_top_slice[VP_SLICE_BYTES];
static byte overflow_bot_slice[VP_SLICE_BYTES];
#endif
static visplane_t overflowplane;
static visplane_t *R_OverflowPlane (fixed_t height, int picnum, int lightlevel,
				    int minx, int maxx)
{
    r_visplane_ovf++;
#if SAT_VISPLANE_POOL
    overflowplane.top    = overflow_top_slice + 1;   /* base+1 so [-1]/[SCREENWIDTH] pads stay in-bounds */
    overflowplane.bottom = overflow_bot_slice + 1;
#endif
    overflowplane.height     = height;
    overflowplane.picnum     = picnum;
    overflowplane.lightlevel = lightlevel;
    overflowplane.minx       = minx;
    overflowplane.maxx       = maxx;
    memset (overflowplane.top, 0xff, SCREENWIDTH);
    return &overflowplane;
}

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
   Pure C, DoomJo-safe, no new cross-CPU coherency surface (master-only generation).
   SATURN PERF: a runtime int (was a compile-time #define) so Mimas can A/B the
   hash vs the vanilla linear scan live on hardware via pad Y; DoomJo never toggles
   it -> stays 1 -> byte-identical.  The hash machinery below is now always compiled. */
int sat_visplane_hash = 1;
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
/* SATURN BUGFIX: [256] not [SCREENHEIGHT] -- indexed by pl->top/bottom (BYTE 0..255:
   0xff sentinel + bottom==viewheight).  At [SCREENHEIGHT(224)] R_MakeSpans overran
   these into adjacent BSS; the stack-local twins in R_DrawVisplane* smashed the
   return address.  Sizing to the full byte range makes every index in-bounds. */
int			spanstart[256];
int			spanstop[256];

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
/* SATURN pot0.5: low-detail TEXTURED floors -- the textured span fill samples 1 texel
   per 2 screen px (full screen width, walls untouched, UNLIKE global detailshift which
   halves the whole render geometry).  Set by the platform (pot0.5 mode); 0 = full-rate. */
extern int sat_floor_ld;
/* SATURN Potato floors: the current flat's dominant/average colour (R_FlatPotatoColor,
   r_data.c), set per-visplane in R_DrawPlanes; replaces the old centre-texel sample. */
int sat_floor_color = 0;
extern int R_FlatPotatoColor (int lumpnum);
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
#if SAT_VISPLANE_POOL
    /* one (top+bottom) slice-pair per plane, capped at VP_POOL_PLANES */
    plane_pool     = Z_Malloc(VP_POOL_PLANES * 2 * VP_SLICE_BYTES, PU_STATIC, 0);
    plane_pool_ptr = plane_pool;
    plane_pool_end = plane_pool + VP_POOL_PLANES * 2 * VP_SLICE_BYTES;
    r_visplane_pool_peak = 0;
#endif
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
       SAT_POTATO_INLINE_SPANS 0 to revert to the record+execute path.  Mimas makes this
       -D-overridable (Makefile knob) so the inline-vs-span A/B is a build flag, not a
       source edit; default stays 1. */
#ifndef SAT_POTATO_INLINE_SPANS
#define SAT_POTATO_INLINE_SPANS 1
#endif
#if SAT_POTATO_INLINE_SPANS
#define R_POTATO_TEXEL 2080   /* centre texel of a 64x64 flat (v32,u32); == r_parallel.c POTATO_TEXEL */
    if (sat_potato_floors)
    {
	byte  c = ds_colormap[sat_floor_color];   /* flat dominant/average (R_FlatPotatoColor) */
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

    /* SATURN: record peak visplane usage + span coverage for the debug overlay
       (sizes #2 MAXVISPLANES and #1 the pooled arena -- both deterministic, so
       Ymir's reading == hardware's). */
    {
        int n = (int)(lastvisplane - visplanes);
        visplane_t *p;
        int cov = 0;
        if (n > r_visplane_peak) r_visplane_peak = n;
        for (p = visplanes; p < lastvisplane; p++)
            if (p->maxx >= p->minx)
                cov += p->maxx - p->minx + 1;
        if (cov > r_visplane_coverage_peak) r_visplane_coverage_peak = cov;
    }
#if VP_DIAG
    vp_in_bad = vp_draw_bad = vp_map_bad = 0;   /* per-frame reset */
#endif
    lastvisplane = visplanes;
    lastopening = openings;
#if SAT_VISPLANE_POOL
    plane_pool_ptr = plane_pool;   /* bump-reset the span pool for the new frame */
    r_visplane_pool_ovf = 0;
#endif

    /* SATURN PERF L1: empty every hash bucket for the new frame (0xff -> -1).
       Only heads need clearing; a tail is read only once its head is set.
       Unconditional: a cheap 256-byte memset, harmless when the hash is toggled off. */
    memset (visplane_hashhead, 0xff, sizeof(visplane_hashhead));

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
    int		bucket = 0;   /* SATURN PERF L1: set in the hash path, read by its insert */
    int		idx;

    if (picnum == skyflatnum)
    {
	height = 0;			// all skys map together
	lightlevel = 0;
    }

    if (sat_visplane_hash)
    {
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
    }
    else
    {
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
    }

    if (lastvisplane - visplanes == MAXVISPLANES)
	return R_OverflowPlane (height, picnum, lightlevel, SCREENWIDTH, -1);   /* SATURN P0: graceful sink, not a hard freeze */

    check = lastvisplane;
    lastvisplane++;

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->minx = SCREENWIDTH;
    check->maxx = -1;

#if SAT_VISPLANE_POOL
    check->top    = R_PoolSlice ();   /* fresh top+bottom slices from the frame pool */
    check->bottom = R_PoolSlice ();
#endif
    /* SATURN: explicit length (was sizeof(top)) -- correct for BOTH the inline array
       (==SCREENWIDTH, byte-identical) and the pooled pointer (sizeof would be 4!).
       Covers top[0..SCREENWIDTH-1]; the [minx-1]/[maxx+1] sentinels are set at draw. */
    memset (check->top,0xff,SCREENWIDTH);

    if (sat_visplane_hash)
        R_HashInsert (bucket, (int)(check - visplanes));

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
        return R_OverflowPlane (pl->height, pl->picnum, pl->lightlevel, start, stop);   /* SATURN P0: graceful sink, not a hard freeze */

    // make a new visplane
    lastvisplane->height = pl->height;
    lastvisplane->picnum = pl->picnum;
    lastvisplane->lightlevel = pl->lightlevel;

    pl = lastvisplane++;
    pl->minx = start;
    pl->maxx = stop;

#if SAT_VISPLANE_POOL
    pl->top    = R_PoolSlice ();   /* the split plane gets its own slices */
    pl->bottom = R_PoolSlice ();
#endif
    memset (pl->top,0xff,SCREENWIDTH);   /* explicit length (see R_FindPlane note) */

    if (sat_visplane_hash)
    {
    /* SATURN PERF L1: a split plane is scanned by vanilla R_FindPlane too, so it
       must join the hash (FIFO -> preserves first-match order). */
    R_HashInsert (R_PlaneHash (pl->height, pl->picnum, pl->lightlevel),
		  (int)(pl - visplanes));
    }

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


/* SATURN parallel-REC (Option C / P1) -- the d32xr r_phase7 plane model adapted to Mimas.
   A POTATO-floor visplane drawn SELF-CONTAINED: ALL per-CPU state on the STACK (a local
   spanstart[] + the height/colormap/source passed by value) with an inline span memset --
   NO plane globals (planeheight, planezlight, the ds_ span state, cachedheight, spanstart).  This is the unit
   the visplane WORK-STEAL (P3) will run on both SH-2 concurrently, each with its own stack,
   so it needs no duplicated BSS (the 117KB trap of the abandoned full-duplication x-split).
   Render-IDENTICAL to the global path (R_MakeSpans -> R_MapPlane potato-inline): the only
   change is distance is recomputed per span (drops the cachedheight[y] cache, exactly like
   d32xr's R_MapPlane) -- same pixels.  Only the potato path (the ship config); textured
   planes keep the global path.  Gated SAT_PLANE_LOCAL for a clean A/B on Ymir. */
#define SAT_PLANE_LOCAL 1
#if SAT_PLANE_LOCAL
static inline void R_PotatoSpan (int y, int x1, int x2, fixed_t plheight,
                                 lighttable_t **plzlight, int color)
{
    fixed_t       distance;
    unsigned      index;
    lighttable_t *cmap;
    byte          c, *d;

    if (x2 < x1 || x1 < 0 || x2 >= viewwidth || (unsigned int)y >= (unsigned int)SCREENHEIGHT)
        return;

    distance = FixedMul (plheight, yslope[y]);   /* per span (no cachedheight cache, d32xr-style) */

    if (fixedcolormap)
        cmap = fixedcolormap;
    else
    {
        index = distance >> LIGHTZSHIFT;
        if (index >= MAXLIGHTZ) index = MAXLIGHTZ-1;
        cmap = plzlight[index];
    }

    c = cmap[color];   /* the flat's dominant/average colour (R_FlatPotatoColor), distance-shaded */
    if (detailshift)
    {
        d = ylookup[y] + columnofs[x1 << 1];
        memset (d, c, (size_t)((x2 - x1 + 1) * 2));
    }
    else
    {
        d = ylookup[y] + columnofs[x1];
        memset (d, c, (size_t)(x2 - x1 + 1));
    }
}

static void R_DrawVisplanePotato (visplane_t *pl, int color,
                                  lighttable_t **plzlight, fixed_t plheight,
                                  int row_lo, int row_hi)   /* SATURN row-split: only fill spans whose row is in [row_lo,row_hi) */
{
    int spanstart_l[256];   /* per-CPU, on the stack.  SATURN BUGFIX: [256] not
                               [SCREENHEIGHT(224)] -- this is indexed by pl->top[x]/
                               bottom[x] which are BYTE (0..255): the 0xff column
                               sentinel and bottom==viewheight wrote past a [224]
                               STACK array, smashing the saved return address ->
                               master CPU exception on RETURN from the render (the
                               Doom II MAP01 freeze).  256 covers the full byte range. */
    int x, stop = pl->maxx + 1;

    /* the R_MakeSpans walk, inline-drawing each completed span via R_PotatoSpan.
       top[minx-1]/top[maxx+1] sentinels (0xff) are set by the caller, as for R_MakeSpans. */
    for (x = pl->minx; x <= stop; x++)
    {
        int t1 = pl->top[x-1], b1 = pl->bottom[x-1];
        int t2 = pl->top[x],   b2 = pl->bottom[x];

        while (t1 < t2 && t1 <= b1) { if (t1 >= row_lo && t1 < row_hi) R_PotatoSpan (t1, spanstart_l[t1], x-1, plheight, plzlight, color); t1++; }
        while (b1 > b2 && b1 >= t1) { if (b1 >= row_lo && b1 < row_hi) R_PotatoSpan (b1, spanstart_l[b1], x-1, plheight, plzlight, color); b1--; }
        while (t2 < t1 && t2 <= b2) { spanstart_l[t2] = x; t2++; }
        while (b2 > b1 && b2 >= t2) { spanstart_l[b2] = x; b2--; }
    }
}

/* SATURN parallel-REC (Option C / P1) -- TEXTURED self-contained span, the 1p-bonus case
   (no Potato).  Computes the texture coordinates locally (distance/xstep/ystep/xfrac/yfrac
   + the distance colormap) and fills the span inline, replicating R_DrawSpan but with NO
   ds_* globals (so two CPU can work-steal it, P3).  High-detail only (detailshift==0, the
   native 320 render); low-detail keeps the global path.  Render-identical to R_MapPlane +
   R_DrawSpan (basexscale/baseyscale/distscale/viewangle are shared read-only). */
static inline void R_TexturedSpan (int y, int x1, int x2, fixed_t plheight,
                                   lighttable_t **plzlight, byte *src, int ld)
{
    fixed_t       distance, length, xfrac, yfrac, xstep, ystep;
    angle_t       angle;
    unsigned int  index, position, step, xtemp, ytemp;
    lighttable_t *cmap;
    byte         *dest;
    int           count, spot;

    if (x2 < x1 || x1 < 0 || x2 >= viewwidth || (unsigned int)y >= (unsigned int)SCREENHEIGHT)
        return;

    distance = FixedMul (plheight, yslope[y]);
    xstep    = FixedMul (distance, basexscale);
    ystep    = FixedMul (distance, baseyscale);
    length   = FixedMul (distance, distscale[x1]);
    angle    = (viewangle + xtoviewangle[x1]) >> ANGLETOFINESHIFT;
    xfrac    =  viewx + FixedMul (finecosine[angle], length);
    yfrac    = -viewy - FixedMul (finesine[angle], length);

    if (fixedcolormap)
        cmap = fixedcolormap;
    else
    {
        index = distance >> LIGHTZSHIFT;
        if (index >= MAXLIGHTZ) index = MAXLIGHTZ-1;
        cmap = plzlight[index];
    }

    /* span fill -- identical packing/loop to R_DrawSpan, local args instead of ds_* globals */
    position = ((xfrac << 10) & 0xffff0000) | ((yfrac >> 6) & 0x0000ffff);
    step     = ((xstep << 10) & 0xffff0000) | ((ystep >> 6) & 0x0000ffff);
    dest = ylookup[y] + columnofs[x1];
    count = x2 - x1;

    if (ld)   /* SQ low-detail (per-plane): 1 texel fetch per 2 screen px */
    {
        step <<= 1;     /* advance two source steps between fetches */
        do {
            byte t;
            ytemp = (position >> 4) & 0x0fc0;
            xtemp = (position >> 26);
            spot  = xtemp | ytemp;
            t = cmap[src[spot]];
            *dest++ = t;
            if (count) { *dest++ = t; count--; }   /* paired px, guarding the odd tail */
            position += step;
        } while (count-- > 0);
        return;
    }

    do {
        ytemp = (position >> 4) & 0x0fc0;
        xtemp = (position >> 26);
        spot  = xtemp | ytemp;
        *dest++ = cmap[src[spot]];
        position += step;
    } while (count--);
}

static void R_DrawVisplaneTextured (visplane_t *pl, byte *src,
                                    lighttable_t **plzlight, fixed_t plheight,
                                    int row_lo, int row_hi, int ld)   /* SATURN row-split [row_lo,row_hi) + per-plane SQ low-detail (ld) */
{
    int spanstart_l[256];   /* per-CPU, on the stack.  SATURN BUGFIX: [256] not
                               [SCREENHEIGHT(224)] -- this is indexed by pl->top[x]/
                               bottom[x] which are BYTE (0..255): the 0xff column
                               sentinel and bottom==viewheight wrote past a [224]
                               STACK array, smashing the saved return address ->
                               master CPU exception on RETURN from the render (the
                               Doom II MAP01 freeze).  256 covers the full byte range. */
    int x, stop = pl->maxx + 1;

    for (x = pl->minx; x <= stop; x++)
    {
        int t1 = pl->top[x-1], b1 = pl->bottom[x-1];
        int t2 = pl->top[x],   b2 = pl->bottom[x];

        while (t1 < t2 && t1 <= b1) { if (t1 >= row_lo && t1 < row_hi) R_TexturedSpan (t1, spanstart_l[t1], x-1, plheight, plzlight, src, ld); t1++; }
        while (b1 > b2 && b1 >= t1) { if (b1 >= row_lo && b1 < row_hi) R_TexturedSpan (b1, spanstart_l[b1], x-1, plheight, plzlight, src, ld); b1--; }
        while (t2 < t1 && t2 <= b2) { spanstart_l[t2] = x; t2++; }
        while (b2 > b1 && b2 >= t2) { spanstart_l[b2] = x; b2--; }
    }
}

/* SATURN parallel-REC (Option C / P3) -- the d32xr visplane split.  The master accumulates
   the regular-flat visplanes (flat ALREADY cached, so the slave never touches the zone
   allocator) into this worklist; then master + slave each draw a half via the self-contained
   R_DrawVisplane* (stack-local + shared read-only tables -> NO per-CPU state, NO big slave
   stack: there is no BSP recursion here).  Disjoint visplanes -> disjoint framebuffer (Doom
   has no plane overdraw), so the two halves are race-free. */
typedef struct { visplane_t *pl; byte *src; lighttable_t **plzlight;
                 fixed_t plheight; int potato, ld, lumpnum, color; } planework_t;   /* ld: per-plane SQ low-detail (independent floor/ceiling) */
planework_t plane_worklist[MAXVISPLANES];
int         plane_worklist_n;
/* master gate: 0 = old global record/parity path (DoomJo + the working baseline, byte-
   identical); 1 = the P3 worklist + master/slave visplane split (set by the Mimas
   platform, main.cxx).  Defined in r_parallel.c with the dispatch. */
extern int  sat_plane_parallel;

/* draw worklist entries [from,to) -- run by BOTH CPUs via r_parallel.c RP_DrawPlanesSplit:
   the static half-split (master [0,half) / slave [half,n)) or, when sat_plane_steal=1, the
   two-pointer work-steal (master fwd from 0 / slave bwd from n-1, one plane per call).  DoomJo /
   sat_plane_parallel=0 calls it once as (0,n) on the master. */
void R_DrawPlaneWorklistRows (int from, int to, int row_lo, int row_hi)
{
    int i;
    for (i = from; i < to; i++)
    {
        planework_t *w = &plane_worklist[i];
        if (w->potato)
            R_DrawVisplanePotato   (w->pl, w->color, w->plzlight, w->plheight, row_lo, row_hi);
        else
            R_DrawVisplaneTextured (w->pl, w->src, w->plzlight, w->plheight, row_lo, row_hi, w->ld);
    }
}
/* SATURN row-split (the universal balancer): both CPUs draw ALL planes but only the spans whose ROW
   is in [row_lo,row_hi) -- splits the per-row FILL (the real cost) regardless of plane sizes, so a
   single DOMINANT plane (d99%) is split across both SH-2, which the plane-granularity split cannot do.
   The spanstart walk runs fully on both CPUs (cheap); only R_*Span is gated. row_hi=256 = full byte
   range => the non-split callers below stay render-identical (gate always true). */
void R_DrawPlaneWorklist (int from, int to) { R_DrawPlaneWorklistRows(from, to, 0, 256); }
#endif



/* SATURN: sky -> VDP2 NBG0 layer.  When set by the platform, R_DrawPlanes leaves
   the sky region as index 0 (the VDP2 transparent code) instead of drawing the
   sky texture.  Default 0 => vanilla software sky (DoomJo, which has no VDP2 sky
   layer, links the same core and keeps drawing the sky). */
int sat_vdp2_sky = 0;
/* SATURN: set to 1 by R_DrawPlanes when ANY sky visplane is rendered this frame (an opening
   to the sky is in view).  The platform drops the hardware sky layer (NBG0) when this is 0 ->
   in fully-enclosed rooms the VDP1 walls' (torn) index-0 gaps show the dark backdrop instead
   of the bright sky, so the tearing is far less visible.  DoomJo ignores it (software sky). */
int sat_frame_has_sky = 0;
/* SATURN (sky-vs-floor classifier, Romain 2026-06-26): per-frame pixel coverage of the SKY vs the
   dominant (player) floor, so we can measure map by map whether the HW-sky bank is worth keeping
   (sky is NOT everywhere; the floor is) or better freed for a textured VDP2 floor.  sat_sky_px counts
   every sky visplane (any sky mode); sat_floor_px counts the dominant-floor skip => read both in a
   perf-sim floor-on mode (pad-Y mode 1/3).  Absolute pixel counts, reset each frame.  DoomJo-safe. */
unsigned int sat_sky_px   = 0;
unsigned int sat_floor_px = 0;
/* SATURN split HW sky (Part 5 -- docs/RBG0_SKY_SPLIT_ANALYSIS.md §5): the SINGLE split view that gets
   the hardware NBG0 sky (its sky region is left index-0, exactly like 1p) which the platform windows to
   that view's band; every OTHER view keeps its software sky.  -1 (default, and DoomJo, and any build
   whose platform never elects one) = no HW-sky view => every view draws the software sky (today's
   behaviour, byte-identical).  Set by the platform each frame BEFORE D_Display's split loop (from last
   frame's per-view coverage + hysteresis); read there to drive the per-view sky-skip. */
int sat_sky_view = -1;
/* SATURN: per-view SKY pixel coverage.  D_Display copies sat_sky_px (reset per view at the top of
   R_DrawPlanes) into sat_sky_px_view[i] after each view renders, so the platform can elect the view
   that gains the most from a HW sky.  DoomJo never reads it. */
unsigned int sat_sky_px_view[4] = { 0, 0, 0, 0 };
/* SATURN: the elected view's viewangle, captured in the split loop so the platform scrolls the single
   NBG0 sky layer by the RIGHT view's angle (the global viewangle at present time is the LAST view's).
   angle_t == unsigned int; DoomJo never reads it. */
angle_t sat_sky_view_angle = 0;
/* SATURN: floor -> VDP2 RBG0 hardware Mode-7 plane.  When set by the platform,
   R_DrawPlanes leaves the FLOOR visplanes (a flat below the eye) as index 0 so the
   RBG0 floor composited behind the framebuffer shows through -- exactly like the sky
   skip.  Ceilings (above the eye) still draw in software.  Default 0 => DoomJo and the
   normal build draw floors normally. */
int sat_vdp2_floor = 0;
/* SATURN split: the SINGLE view that punches the HW floor in split-screen (0 = P1, default).
   Set by the platform; DoomJo never touches it. */
int sat_rbg0_view = 0;
int sat_split_p1hw = 0;   /* SATURN split: platform enables "P1 floor in HW" (pot0 + 2p); read by d_main (per-view punch) */
extern int sat_split_active, sat_split_view;   /* split state (r_main.c / d_main.c) -- for the per-view top reset + the punch helper */
/* SATURN split: true only if THIS view must punch the HW floor.  Outside split (sat_split_active==0)
   it is exactly sat_vdp2_floor -> 1-player unchanged.  In split, only sat_rbg0_view punches; the
   other views draw their software floor.  DoomJo-safe: sat_vdp2_floor==0 short-circuits before the
   split globals are read.  Pure C (used by both r_plane.c and r_segs.c). */
int sat_floor_punch_here(void)
{
    extern int sat_split_active, sat_split_view;
    return sat_vdp2_floor && (!sat_split_active || sat_split_view == sat_rbg0_view);
}

/* SATURN (VDP1 floor, inc-1): deport SECONDARY floors/ceilings (every visplane reaching the
   regular-flat path -- i.e. NOT sky, NOT the RBG0 dominant) to the VDP1 affine-strip layer.
   When sat_vdp1_floor is set AND the platform hook claims a visplane (returns 1), R_DrawPlanes
   leaves it index 0 (the VDP1 strips fill it below NBG1, like the walls) and skips the software
   span draw.  Hook NULL / flag 0 on DoomJo + the normal build => unchanged software floors. */
int sat_vdp1_floor = 0;
/* SATURN swept-region decrochage fill (fill mode 1, owner's design 2026-07-02): per-column
   HISTORY of the claimed-plane region boundaries.  Instead of a uniform B-px perimeter (which
   blankets any small plane during a turn), the CPU paints ONLY the swept band: the rows that
   are plane NOW but were NOT claimed-plane sat_plane_lag frames ago = the exact gap the lagged
   VDP1 content cannot cover (a diagonal band along a moving wall junction).  Platform-armed
   via sat_plane_fill_mode=1; default 0 = the uniform-B legacy path (DoomJo untouched). */
int sat_plane_fill_mode = 0;
/* SATURN partial claim (hook returns 2): per-column VDP1/CPU split edge for the plane being
   claimed, filled by the platform hook BEFORE returning.  For a FLOOR, rows [edge..bottom]
   are punched (VDP1 tiles own them) and rows [top..edge-1] fall through to the normal
   software span path (the far field AND the chunk-clip wedge triangles render as real
   texels); for a CEILING the split mirrors ([top..edge] punched, [edge+1..bottom] software).
   NULL (DoomJo / not armed) => return 2 degrades to a full claim. */
short *sat_floor_punch_edge = NULL;
int sat_floor_punch_nrow = 0;   /* SATURN partial claim, NEAR tile limit (a single screen row --
                                   the near boundary is horizon-parallel): rows nearer the eye
                                   than it are handed to the SOFTWARE spans (VDP1 magnified tiles
                                   there cost ms of iteration for few px; CPU spans are cheap on
                                   magnified rows).  <= 0 = no near limit (legacy behaviour). */
/* SATURN: fired at the very END of R_DrawPlanes, when every visplane (claims, punch edges,
   software residue) is final -- the platform builds + atomically chains its VDP1 floor bank
   here, so the floors go live in the SAME frame as the walls instead of one frame later
   (the forward/backward wall-vs-ceiling slip the owner reported).  NULL on DoomJo. */
void (*sat_floors_done_hook)(void) = NULL;
extern int sat_plane_lag;                        /* r_main.c: N frames of VDP1-vs-mask latency */
extern int sat_split_active;                     /* split shares one fb: per-view histories would
                                                    mix -> swept fill is 1p-only (like the legacy
                                                    border, forced 0 in split by r_main.c) */
static short sat_ceil_bot_cur[SCREENWIDTH];      /* this frame: lowest claimed-CEILING row per column */
static short sat_floor_top_cur[SCREENWIDTH];     /* this frame: highest claimed-FLOOR row per column  */
static short sat_ceil_bot_hist[2][SCREENWIDTH];  /* [0] = 1 frame ago, [1] = 2 frames ago             */
static short sat_floor_top_hist[2][SCREENWIDTH];

int (*sat_floor_vdp1_hook)(int picnum, int height, int minx, int maxx,
                           const unsigned char *top, const unsigned char *bottom,
                           int lightlevel) = NULL;
/* SATURN (owner 2026-07-02): px widths of the plane-colour fill border painted at the deported plane's
   silhouette edge to hide the VDP1-lag gap (see r_main.c).  _border = horizontal (yaw) on the L/R edge,
   _border_v = vertical (forward-motion + viewz) on the top/bottom edge.  Both 0 = pure punch (rest / DoomJo). */
extern int sat_plane_border;
extern int sat_plane_border_v;

/* SATURN: the player's CURRENT floor (height + flat) -- the single floor RBG0 renders.
   Set each frame in R_DrawPlanes from the view sector.  The floor-skip leaves ONLY the
   visplanes matching BOTH (so coplanar same-flat floors are covered, other heights/flats
   stay software); the platform reads sat_vdp2_floor_h to anchor the RBG0 plane's height. */
fixed_t sat_vdp2_floor_h   = 0;
int     sat_vdp2_floor_pic = -1;
/* SATURN: colormap for the RBG0 floor = the player sector's light band (+extralight, like
   the software floor) -> one uniform brightness (no per-distance gradient; that needs a
   per-line K-table).  Per-region lighting is handled by the band-matched floor-skip, not
   here.  Set each frame in R_DrawPlanes; 0 => full bright. */
lighttable_t *sat_vdp2_floor_cmap = 0;
/* SATURN: the player floor's light BAND (lightlevel>>LIGHTSEGSHIFT).  The floor-skip leaves
   for RBG0 ONLY visplanes whose band matches -- a same-flat sector lit differently (a bright
   or dark ZONE) keeps drawing in software at its own brightness, so it stays correct no
   matter where the player stands (instead of the whole HW floor flipping brightness). */
int sat_vdp2_floor_band = 0;
/* SATURN (Romain 2026-06-30): alternate RBG0 floor pick.  0 (default, and DoomJo) -> RBG0 renders
   the floor UNDER THE EYE (legacy).  1 -> RBG0 renders the DOMINANT visible floor (the flat
   covering the most on-screen pixels), recomputed ONLY when the view sector changes -- kept
   latched within a sector so there is no per-frame flicker (which is exactly why the old
   per-frame dominant pick was dropped).  Runtime toggle so BOTH paths stay compiled and are
   A/B-switchable without a rebuild; the platform sets it.  DoomJo never sets it. */
int sat_vdp2_floor_dominant = 0;
/* SATURN (Romain 2026-06-30): the TOP screen row (framebuffer row) of the floor actually punched this
   frame -- the floor plane's real on-screen horizon.  The platform clips the RBG0 window AND the
   HW-sky transparent boundary to THIS row so the sky always comes down exactly to the floor (no
   sky/floor decalage at any vantage).  Reset to a large sentinel each frame; stays there if no floor
   is in view (platform falls back to its static horizon).  DoomJo never reads it. */
int sat_vdp2_floor_top_y = 0x3FFF;
/* SATURN (Romain 2026-06-30): floorheight of the sector the PLAYER stands in (the view sector under the
   eye), independent of the dominant pick.  The platform's player-height horizon (the "line-color" upper
   bound) keys on THIS, not on sat_vdp2_floor_h -- which is the DOMINANT floor's height when
   sat_vdp2_floor_dominant is set (they were the same before that feature).  Set in R_DrawPlanes when the
   HW floor is active; DoomJo never reads it. */
int sat_view_floor_h = 0;
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
int sat_floor_ld = 0;   /* pot0.5: half-rate textured-floor fill (forward-declared above) */
/* SATURN: independent CEILING software quality (M/SQ refactor).  sat_potato_floors/sat_floor_ld
   above act on floors; these mirror them for ceilings so SQ_ceil can differ from SQ_floor.
   Resolved per-plane at enqueue time via is_ceil (see R_DrawPlanes worklist tag).  Default 0
   (ceilings follow the textured path, incl. DoomJo which never sets them). */
int sat_ceil_potato = 0;
int sat_ceil_ld = 0;
/* SATURN: Potato walls -- opaque wall columns drawn as a single distance-shaded
   colour (a fixed texel), in rp_exec_col.  Sprites (masked RP_COL) stay textured.
   Default 0.  Aimed at the future 2/4-player split-screen builds (more views,
   tighter budget). */
int sat_potato_walls = 0;
/* SATURN: skip the close-wall CPU fallback (force every tier to VDP1) for the BANDED and FLAT
   VDP1 wall modes (wmode>=1).  Flat quads can't swim; banded quads CAN swim/squish on close
   walls, accepted in the tiny split windows for the master Bp win.  Set by sat_apply_potato.
   Distinct from sat_potato_walls (flat-only software solid-colour parity).  Default 0. */
int sat_wall_nocpu = 0;
extern byte *ylookup[];
extern int   columnofs[];
extern int   viewwindowy;   /* SATURN: framebuffer Y offset of the view -> screen row = top[x] + viewwindowy */

/* SATURN: derive the RBG0 floor colormap from a light band, EXACTLY like the software floor's
   nearest-distance shade (zlight[band+extralight][0]).  Shared by the under-eye and dominant
   floor picks so both stay luminosity-identical.  See the under-eye block for the rationale. */
static void sat_floor_cmap_from_band(int band)
{
    int li = band + extralight;
    if (li < 0) li = 0; else if (li >= LIGHTLEVELS) li = LIGHTLEVELS - 1;
    sat_vdp2_floor_cmap = zlight[li][0];
}

//
// R_DrawPlanes
// At the end of each frame.
//
/* SATURN: draw rows [y0..y1] of column x of the CURRENT plane (ds_source/planeheight/
   planezlight set by the R_DrawPlanes loop) with real per-pixel flat texels -- R_MapPlane's
   exact mapping + per-row zlight.  Used for the residual bands the VDP1 tiles cannot serve
   (the far sliver past the mip clamp in a partial claim).  Row counts there are small. */
static void sat_plane_texcol(int x, int y0, int y1)
{
    unsigned tang = (unsigned)(viewangle + xtoviewangle[x]) >> ANGLETOFINESHIFT;
    fixed_t tcos = finecosine[tang], tsin = finesine[tang];
    fixed_t tdsc = distscale[x];
    int y;
    for (y = y0; y <= y1; y++)
    {
	fixed_t dist = FixedMul(planeheight, yslope[y]);
	fixed_t len  = FixedMul(dist, tdsc);
	fixed_t xf   = viewx + FixedMul(tcos, len);
	fixed_t yf   = -viewy - FixedMul(tsin, len);
	int zi = dist >> LIGHTZSHIFT; if (zi >= MAXLIGHTZ) zi = MAXLIGHTZ - 1;
	{
	    byte v = (fixedcolormap ? fixedcolormap : planezlight[zi])
		     [ds_source[((yf >> 10) & 0x0FC0) | ((xf >> 16) & 63)]];
	    if (detailshift)
	    {
		int sx = x << 1;
		ylookup[y][columnofs[sx]]     = v;
		ylookup[y][columnofs[sx + 1]] = v;
	    }
	    else
		ylookup[y][columnofs[x]] = v;
	}
    }
}

void R_DrawPlanes (void)
{
    visplane_t*		pl;
    int			light;
    int			x;
    int			stop;
    int			angle;
    int                 lumpnum;

    /* SATURN swept-fill history (fill mode 1): rotate the claimed-plane boundary arrays.
       cur (last frame's fills) -> hist[0] -> hist[1]; reset cur to "nothing" so a column
       with no claimed plane this frame reads as fully-new next frame (conservative fill). */
    if (sat_plane_fill_mode)
    {
	int c;
	for (c = 0; c < SCREENWIDTH; c++)
	{
	    sat_ceil_bot_hist[1][c]  = sat_ceil_bot_hist[0][c];
	    sat_floor_top_hist[1][c] = sat_floor_top_hist[0][c];
	    sat_ceil_bot_hist[0][c]  = sat_ceil_bot_cur[c];
	    sat_floor_top_hist[0][c] = sat_floor_top_cur[c];
	    sat_ceil_bot_cur[c]  = -1;
	    sat_floor_top_cur[c] = (short)viewheight;
	}
    }

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

    sat_frame_has_sky = 0;   /* set below if any sky visplane is in view (platform drops NBG0 if not) */
    sat_sky_px = 0; sat_floor_px = 0;   /* SATURN: sky-vs-floor coverage this frame (classifier) */
    if (!sat_split_active || sat_split_view == sat_rbg0_view)   /* SATURN split: only the punching view resets, so P2 doesn't wipe P1's floor top */
        sat_vdp2_floor_top_y = 0x3FFF;  /* reset; the floor punch below lowers it to the floor's top screen row */
#if SAT_PLANE_LOCAL
    plane_worklist_n = 0;   /* P3: reset the regular-flat worklist for this frame */
#endif

    /* SATURN: pick the floor RBG0 renders.  Two modes (sat_vdp2_floor_dominant):
         0 (default) -- the floor UNDER THE EYE (view sector), captured every frame.
         1           -- the DOMINANT visible floor, recomputed ONLY when the view sector changes
                        (kept latched within a sector -> stable, no per-frame flicker, the reason
                        the old per-frame dominant pick was dropped).
       The floor-skip below leaves index 0 ONLY on visplanes matching the chosen (height,flat,band)
       triple; other heights/flats/bands keep drawing in software at their own brightness.  The
       colormap (nearest-distance shade) is derived from the chosen band via sat_floor_cmap_from_band. */
    if (sat_vdp2_floor || sat_vdp1_floor)   /* SATURN: also compute it for the VDP1/perf-sim path, which
                                               needs the dominant identity to EXCLUDE it (skip secondary only) */
    {
	sector_t *vs = R_PointInSubsector(viewx, viewy)->sector;
	sat_view_floor_h = vs->floorheight;   /* player's view-sector floor height -> the platform's player-height horizon */
	if (!sat_vdp2_floor_dominant)
	{
	    /* legacy: the floor the player stands in */
	    sat_vdp2_floor_h    = vs->floorheight;
	    sat_vdp2_floor_pic  = vs->floorpic;
	    sat_vdp2_floor_band = vs->lightlevel >> LIGHTSEGSHIFT;   /* light band, for the skip match */
	    sat_floor_cmap_from_band(sat_vdp2_floor_band);
	}
	else
	{
	    /* dominant: recompute only on a view-sector change; otherwise keep the latched floor */
	    static sector_t *sat_dom_last_sec = (sector_t *)0;
	    static int       sat_dom_last_lt  = -1;
	    /* SATURN: a fresh level realloc's sectors[]; the cached sat_dom_last_sec then DANGLES and
	       (Doom's zone allocator is deterministic) can land on a NEW sector's address -> the recompute
	       is wrongly SKIPPED and the HW floor keeps the PREVIOUS level's stale pic/height = the
	       intermittent-black P1 split floor at menu-start.  leveltime drops to 0 on every P_SetupLevel,
	       so a drop forces one fresh dominant-floor pick on the new level. */
	    if (leveltime < sat_dom_last_lt) sat_dom_last_sec = (sector_t *)0;
	    sat_dom_last_lt = leveltime;
	    if (vs != sat_dom_last_sec)
	    {
		/* sum visible-FLOOR coverage by (picnum,height,band) triple, then pick the largest.
		   Cheap because it runs only on a sector change.  Sky and ceilings (height >= viewz)
		   are excluded so the winner is always a flat the floor-skip below can hand to RBG0.
		   visplanes are already complete here (built during the BSP/seg pass). */
		struct { fixed_t h; int pic; int band; unsigned int cov; } acc[16];
		int nacc = 0, bi = -1, k, x;
		unsigned int bestcov = 0;
		visplane_t *p;
		sat_dom_last_sec = vs;
		for (p = visplanes ; p < lastvisplane ; p++)
		{
		    unsigned int cov = 0;
		    int band;
		    if (p->minx > p->maxx) continue;
		    if (p->minx < 0 || p->maxx >= SCREENWIDTH) continue;   /* skip corrupt visplane */
		    if (p->picnum == skyflatnum) continue;                 /* sky is not a floor */
		    if (p->height >= viewz) continue;                      /* ceiling (at/above the eye) */
		    for (x = p->minx ; x <= p->maxx ; x++)
		    {
			int t = p->top[x], b = p->bottom[x];
			if (t <= b) cov += (unsigned)(b - t + 1);           /* 0xff sentinel -> t>b -> skipped */
		    }
		    if (!cov) continue;
		    band = p->lightlevel >> LIGHTSEGSHIFT;
		    for (k = 0 ; k < nacc ; k++)
			if (acc[k].h == p->height && acc[k].pic == p->picnum && acc[k].band == band)
			    break;
		    if (k == nacc)
		    {
			if (nacc >= 16) continue;   /* table full: ignore further minor triples */
			acc[k].h = p->height; acc[k].pic = p->picnum; acc[k].band = band; acc[k].cov = 0;
			nacc++;
		    }
		    acc[k].cov += cov;
		    if (acc[k].cov > bestcov) { bestcov = acc[k].cov; bi = k; }
		}
		if (bi >= 0)
		{
		    sat_vdp2_floor_h    = acc[bi].h;
		    sat_vdp2_floor_pic  = acc[bi].pic;
		    sat_vdp2_floor_band = acc[bi].band;
		}
		else
		{
		    /* no floor in view (looking at sky/ceiling): fall back to the under-eye floor */
		    sat_vdp2_floor_h    = vs->floorheight;
		    sat_vdp2_floor_pic  = vs->floorpic;
		    sat_vdp2_floor_band = vs->lightlevel >> LIGHTSEGSHIFT;
		}
		sat_floor_cmap_from_band(sat_vdp2_floor_band);
	    }
	    /* else: keep the latched sat_vdp2_floor_* from the last sector change */
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
	    if (pl->minx <= pl->maxx) sat_frame_has_sky = 1;   /* SATURN: sky is in view this frame */
	    // SATURN: sky -> VDP2.  Leave the sky region as index 0 (the VDP2
	    // transparent code) instead of drawing it; the platform composites a
	    // scrolling VDP2 sky layer behind the framebuffer.  Writing 0 directly
	    // (no colfunc/R_GetColumn) also drops the sky from REC/EX and the command
	    // count.  sat_vdp2_sky is 0 by default so DoomJo keeps its software sky.
	    if (sat_vdp2_sky)
	    {
		/* SATURN CONTRACT (Mimas): the platform memsets the view rows to index 0 after
		   EVERY blit (DG_DrawFrame layer-inversion clear), and visplane regions exclude
		   wall columns -- so the sky region is ALREADY 0 here.  The old per-pixel zero
		   loops were pure redundant bandwidth (up to ~26K px/frame outdoors, row-13 CLS)
		   burning master P.  Only the classifier survives.  DoomJo: sat_vdp2_sky==0. */
		for (x=pl->minx ; x <= pl->maxx ; x++)
		{
		    int yl = pl->top[x];
		    int yh = pl->bottom[x];
		    if (yl > yh) continue;
		    sat_sky_px += (unsigned)(yh - yl + 1);   /* classifier: sky coverage (VDP2-sky path) */
		}
		continue;
	    }

	    /* SATURN sky vertical scale: the ORIGINAL pspriteiscale is CORRECT -- it reproduces 1p's sky
	       proportions (mountains at their normal size).  Do NOT change it: pinning it smaller squishes
	       the mountains, larger stretches them.  In split-screen the halved viewwidth makes it ~2*FRACUNIT,
	       so the visible span exceeds the 128-tall texture and the TOP would WRAP around to the mountains
	       (the dark band = the 2p/3p/4p sky bug).  The real fix is to CLAMP the vertical texture read to
	       its top row in split (skycolfunc below) so the overflow shows a clean uniform sky band instead
	       of a wrapped mountain band -- the mountains keep their correct proportions.  1p
	       (sat_split_active==0) keeps colfunc byte-identical (its span never overflows).  DoomJo never
	       splits -> unaffected.  Pure C (no C++isms). */
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
		    sat_sky_px += (unsigned)(dc_yh - dc_yl + 1);   /* SATURN classifier: sky coverage (software-sky path) */
		    angle = (viewangle + xtoviewangle[x])>>ANGLETOSKYSHIFT;
		    dc_x = x;
		    dc_source = R_GetColumn(skytexture, angle);
		    if (!sat_split_active)
			colfunc ();               /* 1p: byte-identical (span never overflows the texture) */
		    else if (detailshift)
			R_DrawSkyColumnLow ();    /* split low-detail: clamp the top overflow (no wrap) */
		    else
			R_DrawSkyColumn ();       /* split hi-detail:  clamp the top overflow (no wrap) */
		}
	    }
	    continue;
	}

	// SATURN: floor -> VDP2 RBG0.  Leave ONLY the player's-floor visplanes (matching
	// height, flat AND light band) as index 0 so the hardware Mode-7 RBG0 floor -- which is
	// anchored at that height and shaded at that one brightness -- shows through.  Other
	// heights/flats, a same-flat sector at a DIFFERENT light band (a bright/dark zone), and
	// all ceilings keep drawing in software (at their own brightness).  Off by default (DoomJo).
	if (sat_floor_punch_here()
	    && pl->height == sat_vdp2_floor_h
	    && pl->picnum == sat_vdp2_floor_pic
	    && (pl->lightlevel >> LIGHTSEGSHIFT) == sat_vdp2_floor_band)
	{
	    /* SATURN CONTRACT (Mimas): same as the sky punch above -- the platform's per-frame
	       clear already left this region index 0, so the old zero loops (flr = 20K+ px/frame
	       on the row-13 CLS classifier) were redundant master bandwidth.  Trackers survive. */
	    for (x=pl->minx ; x <= pl->maxx ; x++)
	    {
		int yl = pl->top[x];
		int yh = pl->bottom[x];
		if (yl > yh) continue;
		{ int sr = yl + viewwindowy; if (sr < sat_vdp2_floor_top_y) sat_vdp2_floor_top_y = sr; }  /* track the floor's TOP screen row (its real horizon) */
		sat_floor_px += (unsigned)(yh - yl + 1);   /* classifier: dominant-floor coverage */
	    }
	    continue;
	}

	// regular flat
        lumpnum = firstflat + flattranslation[pl->picnum];
	/* SATURN M/SQ: independent floor vs ceiling software quality.  is_ceil = height>viewz.
	   eff_potato/eff_ld pick the floor or ceiling SQ flag, carried per-plane on the worklist
	   (w->potato/w->ld) so the master+slave draw halves read no shared per-plane global.
	   DoomJo never sets sat_ceil_* (default 0) -> byte-identical there. */
	int is_ceil = (pl->height > viewz);
	int eff_potato = is_ceil ? sat_ceil_potato : sat_potato_floors;
	int eff_ld     = is_ceil ? sat_ceil_ld     : sat_floor_ld;
	if (eff_potato) sat_floor_color = R_FlatPotatoColor(lumpnum);  /* dominant/avg, cached */
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

	/* SATURN PERF (RBG0 candidate sizing, profiler): no-op unless RP_PROF. */
	RP_PlanePixels(pl->picnum, (int)pl->height, pl->minx, pl->maxx,
		       pl->top, pl->bottom);

	/* SATURN (VDP1 floor, inc-1): the platform owns this secondary floor/ceiling on the VDP1
	   affine-strip layer -> leave it index 0 (the strips fill it below NBG1, like the walls)
	   and SKIP the software span draw.  Placed AFTER RP_PlanePixels so the inc-0 profiler still
	   counts it; releases the flat lock it would otherwise leak.  Hook NULL / flag 0 on
	   DoomJo + the normal build => unchanged. */
	{
	int fclaim = (sat_vdp1_floor && sat_floor_vdp1_hook)
	    ? sat_floor_vdp1_hook(pl->picnum, (int)pl->height, pl->minx, pl->maxx,
				  pl->top, pl->bottom, pl->lightlevel)
	    : 0;
	if (fclaim)
	{
	    /* SATURN rotation-decrochage fill (owner's spec): the CPU draws the plane's OWN colour, in
	       software (NBG1 latency = aligned with THIS frame's mask), in a border sat_plane_border px
	       wide at the silhouette edge -- exactly the strip between the wall's lagged VDP1 position and
	       its current one.  The interior stays index 0 (the VDP1 quad still fills the bulk, fast).  At
	       rest sat_plane_border==0 -> the original pure-punch fast path, byte-identical (and DoomJo). */
	    /* One UNIFORM perimeter border = max(horizontal yaw shift, vertical viewz shift).  NOT split per
	       axis: the silhouette edges are SLOPED, so a HORIZONTAL view shift moves a near-horizontal edge
	       (the wall/ceiling junction) and opens a VERTICAL gap there -- the axes are coupled.  The border
	       must therefore wrap the whole silhouette by the larger shift component (this is what the working
	       horizontal-only build did, applying its single B to top/bottom too). */
	    int Bh = sat_plane_border;      /* horizontal shift (yaw)         */
	    int Bv = sat_plane_border_v;    /* vertical   shift (fwd + viewz) */
	    int B  = Bh > Bv ? Bh : Bv;     /* uniform border (sloped edges couple axes); 0 -> fast path */
	    /* SWEPT mode (sat_plane_fill_mode=1): per-column band between the plane's CURRENT
	       span and its claimed region sat_plane_lag frames ago -- the owner's "red band".
	       The border colour is COLORMAP-SHADED (a mid zlight band) so it blends with the
	       CRAM-lit VDP1 flat instead of glowing full-bright in dark rooms. */
	    int swept = sat_plane_fill_mode && !sat_split_active;
	    int is_ceil = (pl->height > viewz);
	    int hist_i = (sat_plane_lag >= 2) ? 1 : 0;
	    byte bc = 0;
	    if (swept)
	    {
		int li = (pl->lightlevel >> LIGHTSEGSHIFT) + extralight;
		if (li < 0) li = 0; else if (li >= LIGHTLEVELS) li = LIGHTLEVELS - 1;
		bc = zlight[li][8][R_FlatPotatoColor(lumpnum)];
	    }
	    else if (B > 0)
		bc = (byte)R_FlatPotatoColor(lumpnum);
	    int lb = pl->minx + B, rb = pl->maxx - B;                 /* L/R margin columns -> whole span */
	    /* PARTIAL claim (fclaim == 2, mode 3): the platform filled sat_floor_punch_edge[]
	       with the per-column VDP1/CPU split; only [pb0..pb1] is punched, and top[]/bottom[]
	       are trimmed to the SOFTWARE leftover (far field + chunk-clip wedges) which then
	       falls through to the normal span draw below -- real texels, aligned latency. */
	    int partial = (fclaim == 2 && sat_floor_punch_edge != NULL);
	    int swband  = (fclaim == 3);   /* SOFTWARE plane + textured wall-lag catch-up band */
	    for (x = pl->minx ; x <= pl->maxx ; x++)
	    {
		int yl = pl->top[x];
		int yh = pl->bottom[x];
		int pb0, pb1;
		int n;
		if (yl > yh) continue;
		if (swband)
		{
		    /* fclaim 3 (VDP1 walls + software planes): the plane itself falls through to
		       the normal span draw untouched; here we paint ONLY the catch-up band -- the
		       rows this plane covered sat_plane_lag frames ago that are WALL-punched now:
		       during the wall-lag window VDP1 has nothing plotted there (black sliver at
		       the moving junction).  Real plane texels at mask latency; at rest the band
		       is empty and the walls show through untouched. */
		    int e0, e1;
		    if (is_ceil)
		    {
			int j_old = sat_ceil_bot_hist[hist_i][x];
			if (yh > sat_ceil_bot_cur[x]) sat_ceil_bot_cur[x] = (short)yh;
			e0 = yh + 1;
			e1 = j_old; if (e1 > yh + 40) e1 = yh + 40;
			if (e1 > viewheight - 1) e1 = viewheight - 1;
		    }
		    else
		    {
			int k_old = sat_floor_top_hist[hist_i][x];
			if (yl < sat_floor_top_cur[x]) sat_floor_top_cur[x] = (short)yl;
			e1 = yl - 1;
			e0 = k_old; if (e0 < yl - 40) e0 = yl - 40;
			if (e0 < 0) e0 = 0;
		    }
		    if (e0 <= e1)
		    {
			unsigned tang = (unsigned)(viewangle + xtoviewangle[x]) >> ANGLETOFINESHIFT;
			fixed_t tcos = finecosine[tang], tsin = finesine[tang];
			fixed_t tdsc = distscale[x];
			int y2;
			for (y2 = e0; y2 <= e1; y2++)
			{
			    fixed_t dist = FixedMul(planeheight, yslope[y2]);
			    fixed_t len  = FixedMul(dist, tdsc);
			    fixed_t xf   = viewx + FixedMul(tcos, len);
			    fixed_t yf   = -viewy - FixedMul(tsin, len);
			    int zi = dist >> LIGHTZSHIFT; if (zi >= MAXLIGHTZ) zi = MAXLIGHTZ - 1;
			    byte v = (fixedcolormap ? fixedcolormap : planezlight[zi])
				     [ds_source[((yf >> 10) & 0x0FC0) | ((xf >> 16) & 63)]];
			    if (detailshift)
			    {
				int sx2 = x << 1;
				ylookup[y2][columnofs[sx2]]     = v;
				ylookup[y2][columnofs[sx2 + 1]] = v;
			    }
			    else
				ylookup[y2][columnofs[x]] = v;
			}
		    }
		    continue;      /* bounds untouched -> the plane renders fully in software */
		}
		pb0 = yl; pb1 = yh;
		if (partial)
		{
		    /* punch band = what the tiles can SERVE: [far edge pe .. near limit pn].
		       The NEAR band (magnified -- ms of VDP1 iteration for few px, cheap for
		       CPU spans) goes to the normal span path via the top/bottom trim; the
		       FAR residue (few rows past the mip clamp) is drawn here per-pixel. */
		    int pe = (int)sat_floor_punch_edge[x];
		    int pn = sat_floor_punch_nrow;
		    int f0, f1;                                       /* far residue -> texels */
		    if (is_ceil)
		    {
			pb0 = (pn > 0 && pn > yl) ? pn : yl;          /* near rows excluded     */
			pb1 = pe < yh ? pe : yh;
			if (pb0 > pb1) continue;                      /* nothing tileable -> the
			                                                 whole span stays software */
			{ int nb = pb0 - 1;                           /* NEAR band -> span path  */
			  if (nb < 0) pl->top[x] = 0xff;
			  else        pl->bottom[x] = (unsigned char)nb; }
			f0 = pb1 + 1; f1 = yh;                        /* FAR residue -> texels   */
		    }
		    else
		    {
			pb0 = pe > yl ? pe : yl;
			pb1 = (pn > 0 && pn < yh) ? pn : yh;          /* near rows excluded     */
			if (pb0 > pb1) continue;
			{ int nt = pb1 + 1; if (nt < yl) nt = yl;     /* NEAR band -> span path  */
			  pl->top[x] = (unsigned char)nt; }
			f0 = yl; f1 = pb0 - 1;                        /* FAR residue -> texels   */
		    }
		    if (f0 < yl) f0 = yl;
		    if (f1 > yh) f1 = yh;
		    if (f0 <= f1)
			sat_plane_texcol(x, f0, f1);
		}
		n = pb1 - pb0 + 1;
		if (swept)
		{
		    int fb2, fe;                    /* paint bc on [fb2..fe], punch 0 elsewhere */
		    if (is_ceil)
		    {
			int j_old = sat_ceil_bot_hist[hist_i][x];
			if (pb1 > sat_ceil_bot_cur[x]) sat_ceil_bot_cur[x] = (short)pb1;
			fb2 = j_old + 1; if (fb2 < pb0) fb2 = pb0;  /* newly-ceiling rows only */
			fe  = pb1;
		    }
		    else
		    {
			int k_old = sat_floor_top_hist[hist_i][x];
			if (pb0 < sat_floor_top_cur[x]) sat_floor_top_cur[x] = (short)pb0;
			fb2 = pb0;                                  /* newly-floor rows only */
			fe  = k_old - 1; if (fe > pb1) fe = pb1;
		    }
		    {
			int y;
			/* fill_mode 2: the band gets the REAL flat texels (R_MapPlane's exact
			   mapping + per-row zlight -- ds_source/planeheight/planezlight are
			   already in scope, cached above the hook) instead of the potato
			   colour: the decrochage cover becomes invisible texture.  Cost only
			   on band rows, i.e. only during motion. */
			int texband = (sat_plane_fill_mode >= 2);
			/* SATURN CONTRACT (Mimas): the punched interior is ALREADY index 0 (the
			   platform memsets the view rows after EVERY blit; visplane regions exclude
			   wall columns) -- so write ONLY the band rows.  The old full-height loop
			   re-zeroed the whole span for nothing (master P bandwidth). */
			if (fb2 > fe) continue;              /* band empty (at rest): nothing to write */
			y = fb2; n = fe - fb2 + 1;
			unsigned tang = 0; fixed_t tcos = 0, tsin = 0, tdsc = 0;
			if (texband)
			{
			    tang = (unsigned)(viewangle + xtoviewangle[x]) >> ANGLETOFINESHIFT;
			    tcos = finecosine[tang]; tsin = finesine[tang];
			    tdsc = distscale[x];
			}
			if (detailshift)
			{
			    int sx = x << 1;
			    byte *d0 = ylookup[fb2] + columnofs[sx];
			    byte *d1 = ylookup[fb2] + columnofs[sx + 1];
			    do {
				byte v;
				{
				    if (texband)
				    {
					fixed_t dist = FixedMul(planeheight, yslope[y]);
					fixed_t len  = FixedMul(dist, tdsc);
					fixed_t xf   = viewx + FixedMul(tcos, len);
					fixed_t yf   = -viewy - FixedMul(tsin, len);
					int zi = dist >> LIGHTZSHIFT; if (zi >= MAXLIGHTZ) zi = MAXLIGHTZ - 1;
					v = (fixedcolormap ? fixedcolormap : planezlight[zi])
					    [ds_source[((yf >> 10) & 0x0FC0) | ((xf >> 16) & 63)]];
				    }
				    else v = bc;
				}
				*d0 = v; *d1 = v; d0 += SCREENWIDTH; d1 += SCREENWIDTH; y++;
			    } while (--n);
			}
			else
			{
			    byte *d = ylookup[fb2] + columnofs[x];
			    do {
				byte v;
				{
				    if (texband)
				    {
					fixed_t dist = FixedMul(planeheight, yslope[y]);
					fixed_t len  = FixedMul(dist, tdsc);
					fixed_t xf   = viewx + FixedMul(tcos, len);
					fixed_t yf   = -viewy - FixedMul(tsin, len);
					int zi = dist >> LIGHTZSHIFT; if (zi >= MAXLIGHTZ) zi = MAXLIGHTZ - 1;
					v = (fixedcolormap ? fixedcolormap : planezlight[zi])
					    [ds_source[((yf >> 10) & 0x0FC0) | ((xf >> 16) & 63)]];
				    }
				    else v = bc;
				}
				*d = v; d += SCREENWIDTH; y++;
			    } while (--n);
			}
		    }
		}
		else if (B <= 0)
		{
		    /* SATURN CONTRACT (Mimas): pure punch at rest -- the platform memsets the
		       view rows to 0 after EVERY blit and visplane regions exclude wall columns,
		       so the punched interior is ALREADY index 0.  The old zero loops re-wrote
		       10-30K px/frame for nothing (the biggest single master-P bite after the
		       slave F-build offload).  DoomJo never claims (hook NULL) -> never here. */
		}
		else
		{
		    /* motion border: write ONLY the bc border cells -- the interior is already
		       cleared-0 (platform per-frame clear).  Top band [pb0..t1], bottom band
		       [b0..pb1]; L/R margin or band overlap = the whole column. */
		    int col_edge = (x < lb || x > rb);
		    int t1 = pb0 + B - 1;                 /* last row of the top border band     */
		    int b0 = pb1 - B + 1;                 /* first row of the bottom border band */
		    int m;
		    if (col_edge || t1 >= b0 - 1) { t1 = pb1; b0 = pb1 + 1; }   /* whole column */
		    if (detailshift)
		    {
			int sx = x << 1;
			byte *d0 = ylookup[pb0] + columnofs[sx];
			byte *d1 = ylookup[pb0] + columnofs[sx + 1];
			for (m = t1 - pb0 + 1; m > 0; --m)
			{ *d0 = bc; *d1 = bc; d0 += SCREENWIDTH; d1 += SCREENWIDTH; }
			if (b0 <= pb1)
			{
			    d0 = ylookup[b0] + columnofs[sx];
			    d1 = ylookup[b0] + columnofs[sx + 1];
			    for (m = pb1 - b0 + 1; m > 0; --m)
			    { *d0 = bc; *d1 = bc; d0 += SCREENWIDTH; d1 += SCREENWIDTH; }
			}
		    }
		    else
		    {
			byte *d = ylookup[pb0] + columnofs[x];
			for (m = t1 - pb0 + 1; m > 0; --m)
			{ *d = bc; d += SCREENWIDTH; }
			if (b0 <= pb1)
			{
			    d = ylookup[b0] + columnofs[x];
			    for (m = pb1 - b0 + 1; m > 0; --m)
			    { *d = bc; d += SCREENWIDTH; }
			}
		    }
		}
	    }
	    if (!partial && !swband)
	    {
		W_ReleaseLumpNum(lumpnum);   /* we cached it above but skip the draw -> release the lock */
		continue;
	    }
	    /* partial/swband: FALL THROUGH -- the regular span path below draws the software part
	       with the (possibly trimmed) top[]/bottom[] (the flat stays cached; released at the
	       loop end). */
	}
	}

#if SAT_PLANE_LOCAL
	/* P3: QUEUE this regular flat (potato or textured high-detail) for the master+slave
	   visplane split after the loop (R_DrawPlaneWorklist).  The flat is ALREADY cached, so
	   the slave never touches the zone allocator; the lump release is DEFERRED (the slave
	   reads src during the draw) -> done post-loop.  Low-detail keeps the global path. */
	if ((eff_potato || !detailshift) && plane_worklist_n < MAXVISPLANES)
	{
	    planework_t *w = &plane_worklist[plane_worklist_n++];
	    w->pl = pl; w->src = ds_source; w->plzlight = planezlight;
	    w->plheight = planeheight; w->potato = eff_potato; w->ld = eff_ld; w->lumpnum = lumpnum;
	    w->color = sat_floor_color;
	    continue;
	}
#endif
	RP_MakeSpansEnter();   /* SATURN PERF Phase-0a: R_MakeSpans walk + R_MapPlane (c P) */
	{ int _save_pf = sat_potato_floors; sat_potato_floors = eff_potato;  /* SATURN M/SQ: per-plane potato for the inline R_MapPlane fallback (worklist-full / low-detail) */
	for (x=pl->minx ; x<= stop ; x++)
	{
	    R_MakeSpans(x,pl->top[x-1],
			pl->bottom[x-1],
			pl->top[x],
			pl->bottom[x]);
	}
	sat_potato_floors = _save_pf; }
	RP_MakeSpansLeave();

	RP_FlatCacheEnter();
        W_ReleaseLumpNum(lumpnum);
	RP_FlatCacheLeave();
        } /* SATURN: end sorted visplane loop */
    }

#if SAT_PLANE_LOCAL
    /* P3: draw the queued regular flats -- master + slave each draw a half (the d32xr visplane
       split).  sat_plane_parallel (set by the Mimas platform via main.cxx) enables the slave
       half via r_parallel.c; otherwise (DoomJo / off) the master draws them all.  Then release
       the deferred flat locks (no-op on the cart, where W_CacheLumpNum is a direct pointer). */
    {
        extern int sat_plane_parallel;
        extern void RP_DrawPlanesSplit(int n);
        int n = plane_worklist_n, i;
        if (sat_plane_parallel && n > 1)
            RP_DrawPlanesSplit(n);           /* master+slave: static half-split or work-steal (pad Y) */
        else
            R_DrawPlaneWorklist(0, n);
        for (i = 0; i < n; i++)
            W_ReleaseLumpNum(plane_worklist[i].lumpnum);
    }
#endif

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

    /* SATURN: floors final -> platform builds/chains its VDP1 floor bank now (same frame). */
    if (sat_floors_done_hook)
	sat_floors_done_hook();
}
