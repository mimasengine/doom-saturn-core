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
	I_Error ("R_FindPlane: no more visplanes");

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
        I_Error("R_CheckPlane: no more visplanes");

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
                                  lighttable_t **plzlight, fixed_t plheight)
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

        while (t1 < t2 && t1 <= b1) { R_PotatoSpan (t1, spanstart_l[t1], x-1, plheight, plzlight, color); t1++; }
        while (b1 > b2 && b1 >= t1) { R_PotatoSpan (b1, spanstart_l[b1], x-1, plheight, plzlight, color); b1--; }
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
                                   lighttable_t **plzlight, byte *src)
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

    if (sat_floor_ld)   /* pot0.5: low-detail floors -- 1 texel fetch per 2 screen px */
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
                                    lighttable_t **plzlight, fixed_t plheight)
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

        while (t1 < t2 && t1 <= b1) { R_TexturedSpan (t1, spanstart_l[t1], x-1, plheight, plzlight, src); t1++; }
        while (b1 > b2 && b1 >= t1) { R_TexturedSpan (b1, spanstart_l[b1], x-1, plheight, plzlight, src); b1--; }
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
                 fixed_t plheight; int potato, lumpnum, color; } planework_t;
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
void R_DrawPlaneWorklist (int from, int to)
{
    int i;
    for (i = from; i < to; i++)
    {
        planework_t *w = &plane_worklist[i];
        if (w->potato)
            R_DrawVisplanePotato   (w->pl, w->color, w->plzlight, w->plheight);
        else
            R_DrawVisplaneTextured (w->pl, w->src, w->plzlight, w->plheight);
    }
}
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
/* SATURN: floor -> VDP2 RBG0 hardware Mode-7 plane.  When set by the platform,
   R_DrawPlanes leaves the FLOOR visplanes (a flat below the eye) as index 0 so the
   RBG0 floor composited behind the framebuffer shows through -- exactly like the sky
   skip.  Ceilings (above the eye) still draw in software.  Default 0 => DoomJo and the
   normal build draw floors normally. */
int sat_vdp2_floor = 0;

/* SATURN (VDP1 floor, inc-1): deport SECONDARY floors/ceilings (every visplane reaching the
   regular-flat path -- i.e. NOT sky, NOT the RBG0 dominant) to the VDP1 affine-strip layer.
   When sat_vdp1_floor is set AND the platform hook claims a visplane (returns 1), R_DrawPlanes
   leaves it index 0 (the VDP1 strips fill it below NBG1, like the walls) and skips the software
   span draw.  Hook NULL / flag 0 on DoomJo + the normal build => unchanged software floors. */
int sat_vdp1_floor = 0;
int (*sat_floor_vdp1_hook)(int picnum, int height, int minx, int maxx,
                           const unsigned char *top, const unsigned char *bottom,
                           int lightlevel) = NULL;

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

    sat_frame_has_sky = 0;   /* set below if any sky visplane is in view (platform drops NBG0 if not) */
#if SAT_PLANE_LOCAL
    plane_worklist_n = 0;   /* P3: reset the regular-flat worklist for this frame */
#endif

    /* SATURN: RBG0 renders the player's CURRENT floor.  Capture the view sector's floor
       height + flat + light band here; the floor-skip below leaves just those visplanes as
       index 0 (other heights/flats/bands keep drawing in software).  (A per-frame "dominant
       floor" pick was tried and dropped -- it flickers when two floors trade the lead.) */
    if (sat_vdp2_floor)
    {
	sector_t *vs = R_PointInSubsector(viewx, viewy)->sector;
	sat_vdp2_floor_h    = vs->floorheight;
	sat_vdp2_floor_pic  = vs->floorpic;
	sat_vdp2_floor_band = vs->lightlevel >> LIGHTSEGSHIFT;   /* light band, for the skip match */
	/* light: map the view sector's light band (+extralight, like the software floor) to a
	   colormap level (0 = brightest .. NUMCOLORMAPS-1 = darkest) so the RBG0 floor dims with
	   the room.  The floor-skip below leaves ONLY same-band visplanes for RBG0; differently-
	   lit same-flat sectors (a brighter/darker ZONE) keep drawing in software at THEIR own
	   brightness -> the bright zone stays bright regardless of where the player stands. */
	{
	    int li = sat_vdp2_floor_band + extralight;
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
	    if (pl->minx <= pl->maxx) sat_frame_has_sky = 1;   /* SATURN: sky is in view this frame */
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
	// height, flat AND light band) as index 0 so the hardware Mode-7 RBG0 floor -- which is
	// anchored at that height and shaded at that one brightness -- shows through.  Other
	// heights/flats, a same-flat sector at a DIFFERENT light band (a bright/dark zone), and
	// all ceilings keep drawing in software (at their own brightness).  Off by default (DoomJo).
	if (sat_vdp2_floor
	    && pl->height == sat_vdp2_floor_h
	    && pl->picnum == sat_vdp2_floor_pic
	    && (pl->lightlevel >> LIGHTSEGSHIFT) == sat_vdp2_floor_band)
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
	if (sat_potato_floors) sat_floor_color = R_FlatPotatoColor(lumpnum);  /* dominant/avg, cached */
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
	if (sat_vdp1_floor && sat_floor_vdp1_hook
	    && sat_floor_vdp1_hook(pl->picnum, (int)pl->height, pl->minx, pl->maxx,
				   pl->top, pl->bottom, pl->lightlevel))
	{
	    for (x = pl->minx ; x <= pl->maxx ; x++)
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
	    W_ReleaseLumpNum(lumpnum);   /* we cached it above but skip the draw -> release the lock */
	    continue;
	}

#if SAT_PLANE_LOCAL
	/* P3: QUEUE this regular flat (potato or textured high-detail) for the master+slave
	   visplane split after the loop (R_DrawPlaneWorklist).  The flat is ALREADY cached, so
	   the slave never touches the zone allocator; the lump release is DEFERRED (the slave
	   reads src during the draw) -> done post-loop.  Low-detail keeps the global path. */
	if ((sat_potato_floors || !detailshift) && plane_worklist_n < MAXVISPLANES)
	{
	    planework_t *w = &plane_worklist[plane_worklist_n++];
	    w->pl = pl; w->src = ds_source; w->plzlight = planezlight;
	    w->plheight = planeheight; w->potato = sat_potato_floors; w->lumpnum = lumpnum;
	    w->color = sat_floor_color;
	    continue;
	}
#endif
	RP_MakeSpansEnter();   /* SATURN PERF Phase-0a: R_MakeSpans walk + R_MapPlane (c P) */
	for (x=pl->minx ; x<= stop ; x++)
	{
	    R_MakeSpans(x,pl->top[x-1],
			pl->bottom[x-1],
			pl->top[x],
			pl->bottom[x]);
	}
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
}
