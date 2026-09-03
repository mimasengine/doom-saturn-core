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
#include "r_flatcache.h"	/* SATURN: resident flat pool -- kills the per-frame flat re-read */



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
/* SATURN 2026-08-19 (VDP1 floors inc-0): de-static'd -- the platform's kick-time claim
   pass (dg_saturn vdp1_floors_flush) walks [visplanes, lastvisplane) after the BSP walk,
   before R_DrawPlanes.  Pure C, no behaviour change; DoomJo links it unused. */
visplane_t	*visplanes;
/* SATURN: peak visplane count per frame, exposed for the debug overlay. */
int r_visplane_peak = 0;
/* SATURN VALIDATION (#1 sizing): peak SUM of live-plane column-spans per frame =
   the top-bytes a TIGHT pooled arena would need (x2 for bottom).  Tells us, on Ymir
   (deterministic, identical to HW), exactly how much a span pool could save WITHOUT
   committing to the invasive layout.  Cheap per-frame loop (~n planes), both ports. */
/* high-water BYTES of the #1 span pool (0 when SAT_VISPLANE_POOL is off) */
/* SATURN: planes that overflowed VP_POOL_PLANES this frame -> handed a SHARED
   fallback slice (harmless span glitch, NOT a crash).  If this is ever non-zero
   on a scene you care about, raise VP_POOL_PLANES.  (On the overlay.) */
int r_visplane_pool_ovf = 0;
/* SATURN 2026-08-09: WINDOW HIGH-WATER of the above.  r_visplane_pool_ovf is zeroed every
   R_ClearPlanes, i.e. once per VIEW, while the overlay prints once per ~1 s -- so the printed value
   was almost always 0 even on a frame that overflowed, which is worse than not printing it.  Same
   pattern as r_visplane_peak: core accumulates the max, the overlay prints and zeroes it.
   ⚠ WHY THIS MATTERS: VP_POOL_PLANES is 64 while the overlay shows `vp` against MAXVISPLANES 256,
   so at vp120 the screen actively reassures you while every plane past the 64th shares ONE fallback
   slice pair and each new overflower's memset wipes the previous one's spans. */
int r_visplane_pool_ovf_pk = 0;
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
/* SATURN 2026-08-25 -- THE STRIDE FOLLOWS THE VIEW, NOT THE SCREEN.  pl->minx/maxx are
   VIEW-relative (r_segs.c hands R_CheckPlane an x in [0,viewwidth); the VP_DIAG check below
   asserts it), and plane_pool_ptr is bump-reset PER VIEW in R_ClearPlanes -- so in a 160-px
   3/4p quadrant the top 160 bytes of every 322-byte slice were never touched.  Deriving the
   stride from viewwidth holds 127 planes in 4p out of the SAME 41 KB allocation, which is
   exactly where the pool was measured overflowing (TNT MAP01 4p: vp66.2 / vp67.8, i.e. 2 then
   8 slices served from the shared vp_fallback, whose spans are corrupt by construction).
   The alternative -- VP_POOL_PLANES 64 -> 80 -- costs 10 304 B of the zone the big-WAD loader
   is already short of.  This costs one load instead of a constant, ~130 calls a frame.
   ⚠ THE STRIDE AND THE THREE SCREENWIDTH SITES BELOW MUST MOVE TOGETHER: R_FindPlane and
   R_CheckPlane each memset SCREENWIDTH bytes into a slice and R_FindPlane inits minx to
   SCREENWIDTH.  Leaving any of them at 320 with a 162-byte stride overruns the NEXT slice by
   158 bytes -- silent, and it would look like a visplane corruption bug.  All four are marked
   `SATURN fov/stride` so a grep finds the set.
   ⚠ VP_SLICE_MAX vs VP_SLICE_BYTES: the three STATIC arrays below (vp_fallback and the two
   overflow slices) must be sized by a compile-time constant, so they keep the 1p worst case.
   Only the POOL ARITHMETIC uses the per-view stride. */
#define VP_SLICE_MAX     (SCREENWIDTH + 2)            /* compile-time: static array sizing */
static int vp_slice_bytes = VP_SLICE_MAX;             /* per view, set in R_ClearPlanes */
#define VP_SLICE_BYTES   vp_slice_bytes
static byte	*plane_pool;
static byte	*plane_pool_ptr;
static byte	*plane_pool_end;

static byte vp_fallback[VP_SLICE_MAX];   /* shared slice handed out on overflow */
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
static byte overflow_top_slice[VP_SLICE_MAX];
static byte overflow_bot_slice[VP_SLICE_MAX];
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

/* 🔴 SATURN 2026-08-26 -- 64 -> 16 ROWS, sized by MEASUREMENT for the first time.
   Vanilla's 64 is a guess carried since 1993 ("// ?" was literally the whole comment).  It cost
   SCREENWIDTH*64*2 = 40 960 B of .bss -- MORE THAN THE ENTIRE TLSF POOL (29 088 B) -- for an
   array nothing had ever profiled.  r_opening_demand (below) was added on 2026-08-25 precisely
   to size it, and the answer came back the same on every capture taken since:
     * row 11 `o1`..`o2` on 14/14 Ymir captures -- shareware E1M1 AND TNT MAP01, 1p through 4p,
       action included -- i.e. peak per-view demand <= 640 of the 20 480 words, under 3.2 %.
     * row 22 `op0` throughout: the overflow redirect has never once fired.
   16 rows = 5 120 words = 10 240 B keeps a 8x margin over the worst reading and returns
   30 720 B.  Deliberately NOT the 8 rows the first arithmetic suggested: the extra 5 KB of
   margin is cheap next to a 30 KB win, and `o` is a high-water, not a ceiling -- the
   theoretical worst is 3 writes x width x MAXDRAWSEGS = 245 760 words, 12x even the OLD array.
   That gap is why the garde below is the real safety net and this number is a probability bet:
   an overrun is a bounded HOM on those segs (never corruption), and row-22 `op` rings when it
   happens.  ⚠ If `op` is ever non-zero on a real map, raise this -- do not remove the garde.
   ⚠ Checked before cutting, per the ATLAS warning: row-4 `tl` reads 0.7 ms on the same frame,
   so the four openings memcpy ARE running -- `o1` is genuine headroom and not the symptom of a
   Mimas wall mode skipping the silhouette store. */
#define MAXOPENINGS	SCREENWIDTH*16
short			openings[MAXOPENINGS];
short*			lastopening;
/* SATURN garde-OPENINGS (crash-proof, mirrors the P0 overflowplane/pool sinks): the shared openings
   pool (masked-column tables + sprite clip snapshots, filled in r_segs.c) has NO bounds guard in
   vanilla -- a big PWAD with many silhouetted segs overruns it, corrupting RAM past openings[] and
   tripping the post-hoc RANGECHECK I_Error freeze.  openings_end bounds the r_segs.c writes; anything
   that would overflow is redirected into opening_overflow (a single SCREENWIDTH sink) = a HOM/wrong-clip
   glitch on those segs, never a corruption/freeze.  r_opening_ovf counts redirects/frame (instrument). */
short* const		openings_end = openings + MAXOPENINGS;
short			opening_overflow[SCREENWIDTH];
int			r_opening_ovf = 0;
/* SATURN 2026-08-25 -- the number that SIZES the array, which r_opening_ovf cannot give.
   openings[] is SCREENWIDTH*64 = 40 960 B of .bss, LARGER THAN THE WHOLE TLSF POOL, and 64 is a
   vanilla guess: nothing has ever recorded max(lastopening - openings).  r_opening_ovf only
   fires once it is ALREADY too late (the redirect into opening_overflow).  Folded per view in
   R_ClearPlanes below; the platform keeps the ~1 s window high-water (row 11 `o`). */
int			r_opening_peak = 0;
/* 🔴 SATURN 2026-08-25 -- WHY THIS IS DEMAND AND NOT CONSUMPTION.  r_opening_peak used to fold
   (lastopening - openings), which SATURATES BY CONSTRUCTION: all three sinks in r_segs.c
   redirect into opening_overflow WITHOUT advancing lastopening, so the moment the array is too
   small the counter pins at MAXOPENINGS and can never say BY HOW MUCH.  Cutting MAXOPENINGS on
   a consumption reading would therefore have destroyed the only instrument that can size the
   cut -- the classic case of an instrument that measures its own ceiling.  r_opening_demand
   accumulates what every caller ASKED for, sink or no sink, so the reading stays meaningful at
   any array size and the cut becomes order-independent. */
int			r_opening_demand = 0;


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
/* SATURN low-res (docs/LOWRES_RENDER_STUDY.md): the framebuffer is physically 160-wide, so the
   Potato span memsets must PACK (1 px/source col), NOT upsample to 320 like the detailshift path
   does -- otherwise the ceiling is written past the blitted 160 and cut.  Default 0 = untouched. */
extern int sat_lowres;
/* SATURN: framebuffer row/column lookup -- also externed below for R_DrawPlanes;
   hoisted here so R_MapPlane's inline Potato span memset can reach them. */
extern byte *ylookup[];
extern int   columnofs[];

//
// R_InitPlanes
// Only at game startup.
//
/* SATURN 2026-08-24 (owner) -- PLANE IDENTITY, option A'.
   A visplane carries no sector, no polygon and no world extent: R_FindPlane keys on
   (height, picnum, lightlevel) and NOTHING else, and R_CheckPlane FORKS per seg whenever
   the column interval hits a column the plane has already marked.  So a visplane is a
   SCREEN FRAGMENT, and any renderer asking "is this a whole bounded surface?" had to
   approximate it from screen space -- which is why the VDP1 floor claim kept oscillating.
   These two arrays answer it exactly, for 768 B of zone and two stores on the rare paths:
     vp_sector[i] = the sector whose floor/ceiling created plane i
     vp_flags[i]  = VPF_SPLIT (R_CheckPlane forked this plane, so it is a PIECE of that
                    sector's surface) | VPF_MULTI (R_FindPlane merged a second sector into
                    it, so it is not one surface at all).
   flags == 0 therefore means: this visplane IS the whole of exactly one sector's surface --
   the predicate the owner's rules are written in.  Paired with sat_sector_bbox (p_setup.c)
   it also yields that surface's EXACT world AABB, instead of one reconstructed by
   inverse-projecting the four corners of a screen bbox.
   PARALLEL arrays, not new fields: visplane_t stays 28 B and byte-identical, so the hash
   walk the pooling change exists to keep cache-friendly is untouched. */
/* inc-A'b (owner's console probe read @9 with 3000 px twice: the MERGE rule was refusing the
   corridor and the lit zone).  VPF_MULTI is NOT a fragment -- merging is the NORM in Doom,
   most levels share a floor height/flat/lightlevel across many sectors, and R_FindPlane joins
   them into one visplane.  Refusing that refused nearly every floor.
   So carry the plane's own world AABB instead of one sector index: seeded from the creating
   sector, UNIONED with each sector that merges in.  It stays an exact world bound (never a
   screen-space reconstruction), just a looser one when several sectors share the plane -- and
   the claim intersects it with the visible extent, which tightens it back.
   4 shorts per plane, world units, [BOXTOP, BOXBOTTOM, BOXLEFT, BOXRIGHT]. */
extern short *sat_sector_bbox;   /* p_setup.c: exact per-sector world bbox (PU_LEVEL) */
short *vp_bbox   = NULL;
byte  *vp_flags  = NULL;
static void vp_bbox_add (int ni, int secnum)
{
    const short *sb;
    short *db;
    if (!vp_bbox || !sat_sector_bbox || secnum < 0) return;
    sb = sat_sector_bbox + secnum * 4;
    db = vp_bbox + ni * 4;
    if (sb[0] > db[0]) db[0] = sb[0];      /* BOXTOP    = max y */
    if (sb[1] < db[1]) db[1] = sb[1];      /* BOXBOTTOM = min y */
    if (sb[2] < db[2]) db[2] = sb[2];      /* BOXLEFT   = min x */
    if (sb[3] > db[3]) db[3] = sb[3];      /* BOXRIGHT  = max x */
}

void R_InitPlanes (void)
{
    /* SATURN: allocate from zone heap (low WRAM) — keeps high WRAM BSS within
       the 1MB limit.  Z_Init runs before R_Init so the heap is ready. */
    visplanes = Z_Malloc(MAXVISPLANES * sizeof(visplane_t), PU_STATIC, 0);
    /* PARKED with the VDP1 floor claim, its only consumer (dg_saturn.cxx SAT_VDP1_FLOORS,
       where the console A/B that closed it is written out).  Leaving the pointers NULL is
       the whole park: every writer here is already guarded on vp_flags, so the identity
       simply never accumulates and the 2.3 KB of PU_STATIC is never taken.  Set to 1
       together with SAT_VDP1_FLOORS to bring it back. */
#define SAT_PLANE_IDENTITY 0
#if SAT_PLANE_IDENTITY
    vp_bbox   = Z_Malloc(MAXVISPLANES * 4 * sizeof(short), PU_STATIC, 0);
    vp_flags  = Z_Malloc(MAXVISPLANES * sizeof(byte),  PU_STATIC, 0);
#endif
    r_visplane_peak = 0;
#if SAT_VISPLANE_POOL
    /* one (top+bottom) slice-pair per plane, capped at VP_POOL_PLANES */
    plane_pool     = Z_Malloc(VP_POOL_PLANES * 2 * VP_SLICE_BYTES, PU_STATIC, 0);
    plane_pool_ptr = plane_pool;
    plane_pool_end = plane_pool + VP_POOL_PLANES * 2 * VP_SLICE_BYTES;
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
	if (detailshift && !sat_lowres)
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
        /* SATURN 2026-08-25: the O(visplanes) span-coverage walk that used to sit here is GONE.
           r_visplane_coverage_peak had exactly one other mention in the whole tree -- an
           `extern` in dg_saturn.cxx that no format string ever consumed -- so it was up to 66
           iterations of load/compare/add PER VIEW (264 a frame in 4p) feeding a number nobody
           read, on the one per-view block that sits OUTSIDE every phase bracket.  The peak
           itself is kept: it is one comparison and rows 11/22 both depend on it. */
        int n = (int)(lastvisplane - visplanes);
        if (n > r_visplane_peak) r_visplane_peak = n;
    }
#if VP_DIAG
    vp_in_bad = vp_draw_bad = vp_map_bad = 0;   /* per-frame reset */
#endif
    lastvisplane = visplanes;
    /* SATURN 2026-08-25: fold the HIGH-WATER before the reset wipes it.  R_ClearPlanes runs
       once per VIEW, so this is the max over every view of the window, and the 1p full-width
       case is the maximum by construction (a 160-px quadrant cannot out-consume 320). */
    { if (r_opening_demand > r_opening_peak) r_opening_peak = r_opening_demand; }
    r_opening_demand = 0;
    lastopening = openings;
    r_opening_ovf = 0;   /* SATURN garde-OPENINGS: per-frame reset of the overflow-redirect count */
#if SAT_VISPLANE_POOL
    /* SATURN fov/stride: size this view's slices before the first R_PoolSlice of the pass.
       viewwidth is 320 in 1p and 160 in 2p/3p/4p, so the same 41 KB holds 64 or 127 planes. */
    vp_slice_bytes = viewwidth + 2;
    plane_pool_ptr = plane_pool;   /* bump-reset the span pool for the new frame */
    if (r_visplane_pool_ovf > r_visplane_pool_ovf_pk)
	r_visplane_pool_ovf_pk = r_visplane_pool_ovf;   /* survive the per-view reset -> overlay `vp<peak>.<ovf>` */
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
    /* SATURN fov: the span step divides by the FOCAL, which vanilla wrote as centerxfrac
       because the two coincide at 90 degrees.  `projection` carries the focal now and is
       byte-for-byte centerxfrac at the default, so this is an identity until L+Y moves it. */
    basexscale = FixedDiv (finecosine[angle],projection);
    baseyscale = -FixedDiv (finesine[angle],projection);
}




//
// R_FindPlane
//
visplane_t*
R_FindPlane
( fixed_t	height,
  int		picnum,
  int		lightlevel,
  int		secnum )      /* SATURN: the sector this call speaks for (-1 = unknown) */
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
	    /* a second sector reaching the same key MERGES into this plane: widen the plane's
	       world box to cover it, and record that it is no longer a single sector. */
	    if (vp_flags && secnum >= 0)
	    { vp_bbox_add (idx, secnum); vp_flags[idx] |= VPF_MULTI; }
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
    {
	int li = (int)(check - visplanes);      /* same merge rule, unhashed path */
	if (vp_flags && secnum >= 0)
	{ vp_bbox_add (li, secnum); vp_flags[li] |= VPF_MULTI; }
	return check;
    }
    }

    if (lastvisplane - visplanes == MAXVISPLANES)
	return R_OverflowPlane (height, picnum, lightlevel, SCREENWIDTH, -1);   /* SATURN P0: graceful sink, not a hard freeze */

    check = lastvisplane;
    lastvisplane++;

    check->height = height;
    check->picnum = picnum;
    check->lightlevel = lightlevel;
    check->minx = viewwidth;   /* SATURN fov/stride: the slice is viewwidth+2 wide */
    check->maxx = -1;
    if (vp_flags)
    {
	int ni = (int)(check - visplanes);
	short *db = vp_bbox + ni * 4;
	db[0] = -32767; db[1] = 32767; db[2] = 32767; db[3] = -32767;   /* empty box */
	vp_bbox_add (ni, secnum);
	vp_flags[ni]  = 0;                       /* one whole sector until proven otherwise */
    }

#if SAT_VISPLANE_POOL
    check->top    = R_PoolSlice ();   /* fresh top+bottom slices from the frame pool */
    check->bottom = R_PoolSlice ();
#endif
    /* SATURN: explicit length (was sizeof(top)) -- correct for BOTH the inline array
       (==SCREENWIDTH, byte-identical) and the pooled pointer (sizeof would be 4!).
       Covers top[0..SCREENWIDTH-1]; the [minx-1]/[maxx+1] sentinels are set at draw. */
    memset (check->top,0xff,viewwidth);   /* SATURN fov/stride: NOT SCREENWIDTH -- see VP_SLICE_BYTES */

    if (sat_visplane_hash)
        R_HashInsert (bucket, (int)(check - visplanes));

    return check;
}


//
// R_CheckPlane
//
/* forward: mark-suppress reads these RBG0-dominant-floor globals + the punch predicate, all
   defined later in this file.  sat_floor_punch_here() is the SAME gate the draw-skip uses
   (r_plane.c ~1359) -- it is FALSE in a non-punching split view, where the floor IS software-
   drawn, so suppressing a split there would leave holes: we must gate on it, not on sat_vdp2_floor. */
extern int     sat_mark_suppress, sat_vdp2_floor_pic, sat_vdp2_floor_band;
extern fixed_t sat_vdp2_floor_h;
extern int     sat_floor_punch_here(void);

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

    /* SATURN mark-suppress: the elected RBG0 dominant floor is never rasterised, so it never needs
       to split -- keep it as ONE plane (skip the scan + the fork's memset).  Same triple the draw-
       skip uses (r_plane.c dominant test), gated on the RBG0 path + toggle so DoomJo / M0 and every
       non-dominant plane fall through to the exact vanilla split logic below. */
    if (sat_mark_suppress && sat_floor_punch_here()
	&& pl->height == sat_vdp2_floor_h && pl->picnum == sat_vdp2_floor_pic
	&& (pl->lightlevel >> LIGHTSEGSHIFT) == sat_vdp2_floor_band)
    {
	pl->minx = unionl;
	pl->maxx = unionh;
	return pl;
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
    /* SATURN: the FORK is the moment a surface stops being one visplane.  Both halves are
       pieces from here on -- the parent as much as the child -- so both are marked, and the
       child inherits the sector identity (and any MULTI already on the parent). */
    if (vp_flags)
    {
	int pi = (int)(pl - visplanes), ci = (int)(lastvisplane - visplanes), q;
	for (q = 0; q < 4; ++q) vp_bbox[ci*4 + q] = vp_bbox[pi*4 + q];
	vp_flags[ci]  = (byte)(vp_flags[pi] | VPF_SPLIT);
	vp_flags[pi] |= VPF_SPLIT;
    }

    pl = lastvisplane++;
    pl->minx = start;
    pl->maxx = stop;

#if SAT_VISPLANE_POOL
    pl->top    = R_PoolSlice ();   /* the split plane gets its own slices */
    pl->bottom = R_PoolSlice ();
#endif
    memset (pl->top,0xff,viewwidth);   /* SATURN fov/stride: NOT SCREENWIDTH -- see VP_SLICE_BYTES */

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
    if (detailshift && !sat_lowres)
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
/* SATURN L1 (sat_opt >= 1, see core/r_segs.c for the lever registry).  This span fill is the #1
   inner loop of the whole M7 frame: the shipped SQ is ld for BOTH floor and ceiling, so every
   software plane pixel goes through the `ld` branch below.  At level 0 it costs two separate BYTE
   stores plus an `if (count)` branch per pixel PAIR -- yet the pair is two IDENTICAL bytes.  So
   emit the pair as ONE 16-bit store and hoist the tail out of the loop.
   The framebuffer is 8bpp with a constant 320-byte stride and columnofs[x] == x in M7, so a span
   is a contiguous byte run and `dest` parity alone selects which of the two shapes applies.
   Output is BYTE-IDENTICAL to level 0 (checked for spans of 1..4 px, both parities); big-endian
   SH-2 puts the high byte at the lower address, which is what the odd-start carry relies on.
   may_alias is required: the build passes -Wno-strict-aliasing (the WARNING only, NOT
   -fno-strict-aliasing), so the wide store must be declared to alias the byte buffer.  Supported
   by GCC 9.3 (DoomJo) and 14.2 alike. */
typedef unsigned short __attribute__((__may_alias__)) sat_u16a_t;
extern int sat_opt;

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
        if (sat_opt >= 1)   /* SATURN L1: same bytes, half the stores, no in-loop tail branch */
        {
            int   npx = count + 1;                  /* pixels this span writes */
            byte *d   = dest;
            byte  t;
#define SAT_LD_TEXEL()  cmap[src[(int)(position >> 26) | (int)((position >> 4) & 0x0fc0)]]
            if ((unsigned int)(unsigned long)d & 1u)
            {   /* odd start: px0 goes out alone, then every aligned 16-bit unit carries
                   (second half of pair k, first half of pair k+1) -- identical byte sequence. */
                t = SAT_LD_TEXEL(); position += step;
                *d++ = t; npx--;
                while (npx >= 2)
                {
                    byte t2 = SAT_LD_TEXEL(); position += step;
                    *(sat_u16a_t *)d = (unsigned short)(((unsigned int)t << 8) | t2);
                    d += 2; npx -= 2; t = t2;
                }
                if (npx) *d = t;                    /* trailing half of the carried pair */
            }
            else
            {   /* even start: one aligned 16-bit store per pair, both bytes equal */
                while (npx >= 2)
                {
                    t = SAT_LD_TEXEL(); position += step;
                    *(sat_u16a_t *)d = (unsigned short)(((unsigned int)t << 8) | t);
                    d += 2; npx -= 2;
                }
                if (npx) { t = SAT_LD_TEXEL(); *d = t; }
            }
#undef SAT_LD_TEXEL
            return;
        }
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

/* draw worklist entries [from,to) -- run by BOTH CPUs via r_parallel.c RP_DrawPlanesSplit.
   🔴 CORRECTED 2026-08-26: this used to describe a CHOICE -- "the static half-split (master
   [0,half) / slave [half,n)) or, when sat_plane_steal=1, the two-pointer work-steal".  There is
   no choice.  The static half-split is GONE from the code and `sat_plane_steal` was a GHOST:
   declared extern in src/dg_saturn.cxx, read only inside `#if SAT_DIAG_SLAVE_TOGGLES` (0),
   defined nowhere, absent from the linker map.  RP_DrawPlanesSplit does one thing -- a
   meet-in-the-middle TAS work-steal, master DOWN from n-1 against slave UP from 0.
   ⚠ And that shape has a ceiling nothing in the code states: the slave's share is bounded by
   slSlaveFunc's DISPATCH LATENCY, because the master starts claiming immediately.  On the light,
   packed 160-wide planes of a split view the master routinely takes them all (`m<0`, the branch
   below that skips the join entirely).  Hardware reads SLV b6% / Pb33% in 4p: the slave wins
   about a third of the race and is idle the rest of the frame.  A work-steal CANNOT load an idle
   slave -- only work handed over before the master can reach it can.
   DoomJo / sat_plane_parallel=0 calls it once as (0,n) on the master. */
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
/* SATURN 2026-08-28 -- SKY COLUMNS, and it is the unit the cost actually has.  R_DrawSkyColumn is
   a FIXED bill per column (a 128-byte copy on a cart build; the grain/clamp setup either way) plus
   a short per-pixel loop, so `px` alone cannot be turned into ms -- the factor swings 2-4x with
   geometry, which is exactly why the ms bracket had to be added beside it in the first place.
   `cols` closes that: px and cols together determine the bill, and BOTH ARE COUNTS, which is what
   Ymir IS authoritative for.  The division of labour is then clean and neither half lies -- Ymir
   sizes the JOB (how many columns of sky does a 4p outdoor spot actually have), the console prices
   the RATE (ms per column).  Counted on the SOFTWARE path only, so the elected HW-sky view
   contributes 0 by the same construction that makes its ms read 0.0. */
unsigned int sat_sky_cols = 0;
unsigned int sat_sky_cols_view[4] = { 0, 0, 0, 0 };
/* sat_floor_px REMOVED 2026-08-26 -- dead work.  It was incremented once per span row and read
   by nobody: the row-13 classifier that consumed it was cut on 2026-08-06 and only the producer
   survived.  sat_sky_px stays -- it still prints. */
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
/* SATURN 2026-08-25 -- what the SOFTWARE sky COSTS, per view, in FRT ticks.  The px twin above
   elects the HW-sky view; this one sizes the 3-quadrant HW-sky plan, and a pixel count cannot
   stand in for it: R_DrawSkyColumn does a 128-byte per-column memcpy plus the grain loop, so
   the px->ms factor swings 2-4x with scene geometry.  The HW-sky branch draws nothing, so the
   elected view reads 0 BY CONSTRUCTION -- that is how a capture identifies it.  Reset per view
   at the top of R_DrawPlanes (with sat_sky_px), copied out by d_main's split loop. */
unsigned int sat_sky_frt = 0;
unsigned int sat_sky_frt_view[4] = { 0, 0, 0, 0 };
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
/* (Two SOFTWARE-side attempts at the VDP1 wall-lag gap were tried and REMOVED on 2026-08-02 -- the
   fix that shipped grows the VDP1 WALL instead, which is the layer that is actually late:
   dg_saturn.cxx sat_wall_grow + the matelas.
     - sat_plane_wallband: extend each plane to its own junction row of the previous frame.  Dead by
       construction -- ONE history slot per column (sat_ceil_bot_cur) against a doorway/window column
       holding SEVERAL ceilings, so the high ceiling reads the low one's edge as its own and bleeds
       over the wall between them, at rest as much as in motion.  Per-plane history is the only
       correct form and this build has no RAM for it.
     - sat_plane_grow: let the plane simply overflow its junction by 1-2 px, all four sides.  Owner
       tested: it does not close what he sees, and growing the ON-TIME layer to chase the LATE one is
       the wrong end of the problem anyway.
   The `fclaim == 3` machinery they drove is untouched and still reachable through ftex mode 5; its
   two real bugs found on the way ARE fixed (the lowres double-write, and the reject/hole-only band
   guards).  Do not re-derive either attempt from that code without reading this note.) */
/* SATURN partial claim (hook returns 2): per-column VDP1/CPU split edge for the plane being
   claimed, filled by the platform hook BEFORE returning.  For a FLOOR, rows [edge..bottom]
   are punched (VDP1 tiles own them) and rows [top..edge-1] fall through to the normal
   software span path (the far field AND the chunk-clip wedge triangles render as real
   texels); for a CEILING the split mirrors ([top..edge] punched, [edge+1..bottom] software).
   NULL (DoomJo / not armed) => return 2 degrades to a full claim. */
short *sat_floor_punch_edge = NULL;
short *sat_floor_punch_near = NULL;  /* SATURN inc-0d (VDP1 floor rect-bands): per-column punch
                                        BOTTOM row, same contract as _edge (platform-owned, NULL =
                                        legacy single-row sat_floor_punch_nrow).  With both arrays
                                        the punched band is fully per-column: [edge[x]..near[x]],
                                        edge[x] = 0x7fff marks an unpunched (all-software) column. */
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
/* ONE level of history (1 frame ago).  The 2-frame slot was dropped 2026-08-02: it cost 1280 bytes
   of .bss against a TLSF pool sitting AT its 4 KB floor ([[boot-loop-can-be-tlsf-pool-starvation]]),
   and its only user was the wall-lag band's Wb2 depth -- speculative, same day.  If the VDP1 lag
   turns out to be 2 frames rather than 1, this comes back WITH a real diet, not instead of one. */
static short sat_ceil_bot_hist[SCREENWIDTH];
static short sat_floor_top_hist[SCREENWIDTH];

int (*sat_floor_vdp1_hook)(int picnum, int height, int minx, int maxx,
                           const unsigned char *top, const unsigned char *bottom,
                           int lightlevel) = NULL;
/* SATURN (owner 2026-07-02): px widths of the plane-colour fill border painted at the deported plane's
   silhouette edge to hide the VDP1-lag gap (see r_main.c).  _border = horizontal (yaw) on the L/R edge,
   _border_v = vertical (forward-motion + viewz) on the top/bottom edge.  Both 0 = pure punch (rest / DoomJo). */
extern int sat_plane_border;
extern int sat_plane_border_v;
extern int sat_plane_border_max;   /* px cap on both borders -- also the wall-lag band's reject limit */

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
/* SATURN 2026-08-16 -- RELAXING THE PUNCH'S LIGHT TERM: BUILT, MEASURED, REMOVED THE SAME DAY.
   The idea was sound on paper -- the punch requires (height, picnum, light band), so a same-flat
   plane at another brightness keeps drawing in software, and outdoors that ought to be most of the
   floor.  The counter said otherwise: row-24 `pe` read **0 on six captures out of eight** (23 and 8
   on the others).  The light band was almost never what kept a plane in software on TNT MAP11, so
   dropping it bought nothing while making dark alcove floors snap to the dominant's brightness.
   Kept as a note so the reasoning is not re-run: the term to attack is HEIGHT or PICNUM, not the
   light band -- and measure `pe`-equivalent first. */
/* SATURN (Romain 2026-06-30): alternate RBG0 floor pick.  0 (default, and DoomJo) -> RBG0 renders
   the floor UNDER THE EYE (legacy).  1 -> RBG0 renders the DOMINANT visible floor (the flat
   covering the most on-screen pixels), recomputed ONLY when the view sector changes -- kept
   latched within a sector so there is no per-frame flicker (which is exactly why the old
   per-frame dominant pick was dropped).  Runtime toggle so BOTH paths stay compiled and are
   A/B-switchable without a rebuild; the platform sets it.  DoomJo never sets it. */
int sat_vdp2_floor_dominant = 0;
/* SATURN mark-suppress (2026-07-09): the RBG0 dominant floor is NEVER drawn (R_DrawPlanes hands
   it to VDP2 RBG0 and skips its span fill).  A never-drawn plane never needs to SPLIT: R_CheckPlane
   normally forks a new visplane (+320-byte memset, +pool slices, +MAXVISPLANES pressure) whenever a
   later overlapping seg finds already-marked columns.  With this on, the dominant floor is forced to
   stay ONE plane (expand, never split) -> those per-split memsets vanish from Bp (the audit sized
   ~2-5ms) and the visplane count drops (helps big WADs).  Only the top/bottom of a plane that is
   never rasterised become last-write-wins over seam overlaps -- harmless; floorclip (sprite
   occlusion) is untouched.  Toggle only (default 0 / DoomJo); gated on the RBG0 path (sat_vdp2_floor)
   and the elected dominant triple so a non-dominant floor still splits and draws correctly. */
int sat_mark_suppress = 0;
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
/* SATURN per-frame texture LOAD BUDGET, plane half (walls own the budget in r_segs.c). */
extern int sat_tex_load_budget, sat_tex_load_spent;
extern int R_LoadBudgetLeft (void);   /* SATURN: r_segs.c -- 1 = the frame can still afford a fault */
extern int sat_budget_refused;        /* SATURN: r_segs.c -- 1 once the budget has refused something */
extern int W_LumpResident (int lump);
extern int R_FlatPotatoColorPeek (int lumpnum);
int sat_plane_flat_io    = 0;   /* visplanes drawn potato for want of residency (~1 s window)  */
int sat_plane_flat_nocol = 0;   /* ...of which we had no cached dominant colour either         */
#define SAT_FLAT_UNKNOWN 100    /* neutral index; ds_colormap still shades it by distance       */
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
/* SATURN 2026-08-24: what the PARTIAL claim costs on the other side of the ledger.  A punch
   that does not reach the plane's near edge leaves a FAR residue, and that residue is drawn
   HERE -- per pixel, master only, no slave share, no span batching -- where the untouched
   plane would have been drawn in spans.  If this counter approaches the punched-pixel count
   the mode is paying more than it saves, and no frame-time reading will say so on its own. */
int sat_plane_texcol_px = 0;
static void sat_plane_texcol(int x, int y0, int y1)
{
    if (y1 >= y0) sat_plane_texcol_px += y1 - y0 + 1;
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
	    if (detailshift && !sat_lowres)
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
	    sat_ceil_bot_hist[c]  = sat_ceil_bot_cur[c];
	    sat_floor_top_hist[c] = sat_floor_top_cur[c];
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
    sat_sky_px = 0;   /* SATURN: sky coverage this frame (sat_floor_px removed 2026-08-26) */
    sat_sky_cols = 0;                   /* SATURN 2026-08-28: and its per-COLUMN twin (row 12 `c`) */
    sat_sky_frt = 0;                    /* SATURN 2026-08-25: software-sky ms, same per-view clock */
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

#ifndef SAT_PSW
#define SAT_PSW 0
#endif
#if SAT_PSW
    /* SATURN PSW (psw-world experiment, docs/PSW_WORLD_PLAN.md): painter mode -- no
       span fill, no punch, no sky columns.  The dominant election ABOVE already ran
       (with empty visplane coverage the dominant pick degrades to its under-eye
       fallback, which is exactly the PSW step-1 spec).  Two things the platform still
       needs from this frame:
         - sky presence: any sky visplane touched by the walk (coverage not needed --
           R_FindPlane created it from a visited subsector);
         - the RBG0/sky boundary: centery, the vanishing line of every horizontal
           plane.  Rows below it that no VDP1 quad covers show the RBG0 dominant
           floor (the intended failure mode); rows above are sky/ceiling territory. */
    {
	extern int sat_psw_active;   /* platform: latched at the frame boundary */
	if (sat_psw_active)
	{
	    extern void R_PswPolysEnsure (void);   /* r_bsp.c: lazy per-level polygon build */
	    /* SATURN round 29: the visplane walk that lived here (sky presence +
	       flat-dalle residency) moved to the platform, fed by its own notes --
	       R_Subsector skips R_FindPlane entirely under PSW, so there are no
	       visplanes to walk (and the dominant election above, whose coverage
	       sums were all zero without span marking, keeps taking the same
	       under-eye fallback it always took in this mode). */
	    extern void R_PswFrameFlats (void);    /* platform: sky flag + R_FlatCacheGet per noted lump */
	    R_PswFrameFlats ();
	    sat_vdp2_floor_top_y = centery;
	    R_PswPolysEnsure();      /* the kick (walls+flats flush) runs AFTER this */
	    return;
	}
    }
#endif

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
	    RP_SkyEnter ();   /* SATURN 2026-08-25: row 12 `SKY` -- the SOFTWARE sky's own ms */
	    for (x=pl->minx ; x <= pl->maxx ; x++)
	    {
		dc_yl = pl->top[x];
		dc_yh = pl->bottom[x];

		if (dc_yl <= dc_yh)
		{
		    sat_sky_px += (unsigned)(dc_yh - dc_yl + 1);   /* SATURN classifier: sky coverage (software-sky path) */
		    sat_sky_cols++;                               /* SATURN row 12 `c`: the unit the drawer's cost actually has */
		    angle = (viewangle + xtoviewangle[x])>>ANGLETOSKYSHIFT;
		    dc_x = x;
		    dc_source = R_GetColumn(skytexture, angle);
		    if (!sat_split_active)
			colfunc ();               /* 1p: byte-identical (span never overflows the texture) */
		    else if (detailshift && !sat_lowres)
			R_DrawSkyColumnLow ();    /* split low-detail: DUPLICATING drawer, clamp the top overflow (no wrap) */
		    else
			R_DrawSkyColumn ();       /* split hi-detail OR M7-lowres: PACKED drawer (1 col; lowres packs to fb[0,80)) */
		}
	    }
	    RP_SkyLeave ();
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
	    }
	    continue;
	}

	// regular flat
        /* row 5 `Pv`'s SECOND half opens here: everything to the R_FlatPotatoColor prime below
           is flat RESOLUTION, and it allocates -- so it can never leave the master.  What sits
           after it (lighting, planeheight, span setup) is the parallelisable remainder. */
        { extern void RP_FlatResEnter (void); RP_FlatResEnter (); }
        lumpnum = firstflat + flattranslation[pl->picnum];
	/* SATURN M/SQ: independent floor vs ceiling software quality.  is_ceil = height>viewz.
	   eff_potato/eff_ld pick the floor or ceiling SQ flag, carried per-plane on the worklist
	   (w->potato/w->ld) so the master+slave draw halves read no shared per-plane global.
	   DoomJo never sets sat_ceil_* (default 0) -> byte-identical there. */
	int is_ceil = (pl->height > viewz);
	int eff_potato = is_ceil ? sat_ceil_potato : sat_potato_floors;
	int eff_ld     = is_ceil ? sat_ceil_ld     : sat_floor_ld;
	int   io_flat_plane = 0; /* SATURN load budget: 1 = flat NOT loaded this frame (see below) */
	int   flat_locked   = 0; /* SATURN: 1 = a W_CacheLumpNum lock was taken -> MUST be released */
	int   flat_paid     = 0; /* SATURN: 1 = the budget paid for a real load this frame */
	byte *fc_src        = NULL;   /* SATURN: resident flat-pool slot (no zone block, no lock) */
	if (eff_potato) sat_floor_color = R_FlatPotatoColor(lumpnum);  /* dominant/avg, cached */
	else
	{
	/* SATURN RESIDENT FLAT POOL (r_flatcache.c).  Ask the pool FIRST: a pooled flat lives in
	   the slab, NOT in lumpinfo[].cache, so W_LumpResident would report it missing and the
	   load budget below would gate a flat that is already in RAM.  A hit here is the whole
	   point of the pool -- no disc, no zone block, no lock to release. */
	fc_src = R_FlatCachePeek (lumpnum);
	/* SATURN LOAD BUDGET (flats, 2026-08-06 -- the `P` half of the same defect as the walls).
	   The fetch below is a SYNCHRONOUS ~42 ms disc read in the streaming build when the flat
	   is not resident, charged to `P`: that is the P=149/229/284 ms frames in the owner's TNT
	   captures, and why an OUTDOOR map with almost no flat floor still spiked.  Past the frame's
	   budget, draw the plane in POTATO (one dominant colour, the existing w->potato path, which
	   reads w->color and never touches w->src) and skip the load entirely. */
	if (!fc_src && sat_tex_load_budget && !W_LumpResident (lumpnum))
	{
	    if (R_LoadBudgetLeft ())
		flat_paid = 1;
	    else
	    {
		int c = R_FlatPotatoColorPeek (lumpnum);   /* MUST peek: R_FlatPotatoColor reads the lump */
		if (c < 0) { c = SAT_FLAT_UNKNOWN; sat_plane_flat_nocol++; }
		sat_floor_color = c; eff_potato = 1; io_flat_plane = 1; sat_plane_flat_io++;
	    }
	}
	/* Fill a pool slot -- THE disc read, once per flat per residency instead of once per plane
	   per frame.  NULL = not poolable (no slab, not 4096 bytes, or every slot is already in use
	   by this view) -> fall through to the classic zone path below, exactly as before. */
	if (!fc_src && !io_flat_plane)
	    fc_src = R_FlatCacheGet (lumpnum);
	}
	if (fc_src)
	    ds_source = fc_src;                                   /* pooled: nothing to release */
	else if (!io_flat_plane)
	{
	    ds_source = W_CacheLumpNum(lumpnum, PU_STATIC);
	    flat_locked = 1;                                      /* the lock the release sites undo */
	}
	/* PRIME the dominant colour on the frame the budget decides to PAY, now that the flat IS in
	   memory (pool slot or zone block -- R_FlatPotatoColor peeks the pool, so this never costs a
	   second read).  It is otherwise only called by the potato mode, which is off in normal play,
	   so the cache stayed empty and every gated plane fell back to neutral grey: measured
	   `nocol` == the plane count on the owner's captures. */
	/* LAZY since 2026-08-08, same subject as the wall side: until the budget has actually
	   refused something (sat_budget_refused, r_segs.c) this dominant colour is never read. */
	if (flat_paid && sat_budget_refused) R_FlatPotatoColor (lumpnum);
	{ extern void RP_FlatResLeave (void); RP_FlatResLeave (); }   /* row 5 `Pv` 2nd half */

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
	   counts it; releases the flat lock it would otherwise leak.  Hook NULL on DoomJo + the
	   normal build => unchanged.
	   ⚠ DECOUPLED from sat_vdp1_floor (2026-08-20, owner round 2): that flag is the DEPORTED-ftex
	   master switch and ALSO wakes the r_segs.c cross_hi wall cuts + wedges, built for one-frame-
	   late ceiling quads that no longer exist -- setting it for same-frame kick-time claims cut
	   walls against a phantom ceiling line (the owner's "murs coupés / trous").  The hook install
	   is the platform opt-in; an empty claim table already means "draw everything in software". */
	{
	int fclaim = (sat_floor_vdp1_hook)
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
	    int swband  = (fclaim == 3);   /* SOFTWARE plane + textured wall-lag catch-up band */
	    /* How far back to extend the plane.  The hook-driven mode 5 uses the owner-tuned
	       sat_plane_lag; the forced M7 band carries its own depth so the toggle IS the tuning
	       (1 = last frame = the VDP1 plot lag; 2 = two frames = wider band, more master fill). */
	    /* Largest junction move the band will believe.  Doubles as the contamination reject
	       below: further than this and the history slot is holding ANOTHER plane's edge. */
	    int band_cap = sat_plane_border_max; if (band_cap < 1) band_cap = 1; else if (band_cap > 40) band_cap = 40;
	    byte bc = 0;
	    if (swband) { }              /* the band computes its own texels -- no border colour needed */
	    else if (swept)
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
	    /* SATURN inc-0d: the motion border + swept band exist for quads presented a frame
	       LATE (the old ftex tiles).  The rect-band claims are plotted the SAME frame under
	       the manual present, and their software residual is drawn at mask latency by the
	       trims below -- painting potato borders over them would re-create the round-1
	       "bords flat" artefact.  Punch clean, always. */
	    if (partial) swept = 0;
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
			int j_old = sat_ceil_bot_hist[x];
			if (yh > sat_ceil_bot_cur[x]) sat_ceil_bot_cur[x] = (short)yh;
			e0 = yh + 1;
			/* SATURN 2026-08-02 -- REJECT, do not CLAMP (owner: "certains petits murs sont
			   integralement ecrases par de la texture du plafond").  There is ONE history slot
			   per column, but a column can hold SEVERAL ceilings: the front ceiling, the upper
			   texture, then the BACK sector's ceiling seen through the opening.  `cur` keeps the
			   MAX, so next frame the HIGH ceiling reads the LOW ceiling's bottom as its own
			   previous edge and paints its texels straight down over the wall between them -- at
			   rest as much as in motion, since that mismatch never goes away.  Clamping to 40
			   rows only shortened the bleed.  A genuine one-frame junction move is a few rows;
			   beyond that it is another plane's edge, so DROP the band.  Cost: doorway/window
			   columns get no band at all -- which is what Wb0 gives, so never worse. */
			e1 = j_old;
			if (e1 > yh + band_cap) continue;   /* another plane's edge, not our junction */
			if (e1 > viewheight - 1) e1 = viewheight - 1;
		    }
		    else
		    {
			int k_old = sat_floor_top_hist[x];
			if (yl < sat_floor_top_cur[x]) sat_floor_top_cur[x] = (short)yl;
			e1 = yl - 1;
			e0 = k_old;
			if (e0 < yl - band_cap) continue;   /* idem, floor side */
			if (e0 < 0) e0 = 0;
		    }
		    if (e0 <= e1)
		    {
			unsigned tang = (unsigned)(viewangle + xtoviewangle[x]) >> ANGLETOFINESHIFT;
			fixed_t tcos = finecosine[tang], tsin = finesine[tang];
			fixed_t tdsc = distscale[x];
			int y2;
			/* HOLE-ONLY: never repaint a pixel the software renderer already owns.  The
			   band runs inside R_DrawPlanes, i.e. AFTER the seg loop, so a software wall
			   (close/magnified fallback, or one held by sat_wall_entry) and the software
			   sky are already in the framebuffer AT THE CORRECT POSITION -- painting plane
			   texels over those is always wrong.  Index 0 means nobody drew: a real hole,
			   or a surface VDP1 owns.  One byte read per band pixel, and a hit skips the
			   whole perspective texel fetch. */
			/* `detailshift && !sat_lowres` -- NOT bare detailshift.  M7 runs at
			   detailshift=1 with the PACKED drawers (r_main.c ~721/855 select them on
			   the same predicate): one byte per logical column into a 160-wide
			   framebuffer, x2-zoomed by NBG1 afterwards.  Doubling to x<<1 there writes
			   at twice the column, into the neighbour's pixels -- the owner's "positions
			   calculees en high res alors qu'on est en low res".  Only split low-detail
			   (detailshift=1, sat_lowres=0) uses the duplicating layout. */
			int dup = (detailshift && !sat_lowres);
			int co  = columnofs[dup ? (x << 1) : x];
			for (y2 = e0; y2 <= e1; y2++)
			{
			    byte *p = ylookup[y2] + co;
			    if (p[0] && (!dup || p[1])) continue;
			    fixed_t dist = FixedMul(planeheight, yslope[y2]);
			    fixed_t len  = FixedMul(dist, tdsc);
			    fixed_t xf   = viewx + FixedMul(tcos, len);
			    fixed_t yf   = -viewy - FixedMul(tsin, len);
			    int zi = dist >> LIGHTZSHIFT; if (zi >= MAXLIGHTZ) zi = MAXLIGHTZ - 1;
			    byte v = (fixedcolormap ? fixedcolormap : planezlight[zi])
				     [ds_source[((yf >> 10) & 0x0FC0) | ((xf >> 16) & 63)]];
			    if (!p[0])         p[0] = v;
			    if (dup && !p[1])  p[1] = v;
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
		    if (sat_floor_punch_near)                 /* inc-0d: per-column punch bottom */
			pn = (int)sat_floor_punch_near[x];
		    int f0, f1;                                       /* far residue -> texels */
		    if (is_ceil)
		    {
			/* SATURN 2026-08-24: the punch arrays have ONE meaning for both sides --
			   edge[x] = the punch's TOP row, near[x] = its BOTTOM row (that is what
			   the platform writes: fvdp1_ptop/fvdp1_pbot, a min and a max).  This
			   branch used to read them the other way round, which was harmless only
			   because ceilings were never claimed.  They are now, so read them the
			   same way; what stays MIRRORED is which residue is which -- for a
			   ceiling the near part is ABOVE the punch and the far part BELOW. */
			pb0 = pe > yl ? pe : yl;                      /* punch top    */
			pb1 = (pn > 0 && pn < yh) ? pn : yh;          /* punch bottom */
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
			int j_old = sat_ceil_bot_hist[x];
			if (pb1 > sat_ceil_bot_cur[x]) sat_ceil_bot_cur[x] = (short)pb1;
			fb2 = j_old + 1; if (fb2 < pb0) fb2 = pb0;  /* newly-ceiling rows only */
			fe  = pb1;
		    }
		    else
		    {
			int k_old = sat_floor_top_hist[x];
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
			if (detailshift && !sat_lowres)
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
		else if (B <= 0 || partial)
		{
		    /* SATURN CONTRACT (Mimas): pure punch at rest -- the platform memsets the
		       view rows to 0 after EVERY blit and visplane regions exclude wall columns,
		       so the punched interior is ALREADY index 0.  The old zero loops re-wrote
		       10-30K px/frame for nothing (the biggest single master-P bite after the
		       slave F-build offload).  DoomJo never claims (hook NULL) -> never here.
		       (inc-0d: partial claims always take this path -- see the swept note above.) */
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
		    if (detailshift && !sat_lowres)
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
		if (flat_locked)             /* SATURN: 0 = gated (load budget) or POOLED -> no lock */
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
	   reads src during the draw) -> done post-loop.
	   SATURN M7 (2026-07-17): lowres (detailshift=1, PACKED 160) ALSO queues.  R_TexturedSpan
	   writes a CONTIGUOUS run from columnofs[x1] (== R_DrawSpan), which IS the packed layout in
	   lowres (columnofs is 1:1) -> the slave draws M7 planes byte-identically to the inline
	   R_MapPlane it was falling back to.  This was the sole thing making M7 != "M4 + lowres":
	   textured+detailshift planes went inline (master-only) -> slave IDLE in M7 -> the slave
	   plane dispatch (which rewinds the SGL GBR+72 work pointer AND drives the VDP1 coherent-pair
	   present) never ran -> GBR creep + the present never reset on a New Game (M7-only stale walls
	   on level restart).  Routing them to the slave puts M7 on M4's exact render path.  Non-lowres
	   low-detail (detailshift=1, sat_lowres=0) still keeps the inline path -- R_TexturedSpan packs
	   but cannot DOUBLE to 320, so that case is not added. */
	if ((eff_potato || !detailshift || sat_lowres) && plane_worklist_n < MAXVISPLANES)
	{
	    planework_t *w = &plane_worklist[plane_worklist_n++];
	    w->pl = pl; w->src = ds_source; w->plzlight = planezlight;
	    /* SATURN: lumpnum -1 = NO ZONE LOCK WAS TAKEN for this plane -- either the load budget
	       gated it, or (2026-08-06) the flat came from the RESIDENT POOL, which is not a zone
	       block at all.  The drain below must NOT release a lock we never took: W_ReleaseLumpNum
	       would Z_ChangeTag a NULL cache pointer ("block without a ZONEID").  `w->src` still
	       points at the pool slot, which the LRU cannot reuse while this view is being drawn. */
	    w->plheight = planeheight; w->potato = eff_potato; w->ld = eff_ld;
	    w->lumpnum = flat_locked ? lumpnum : -1;
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

        if (flat_locked) W_ReleaseLumpNum(lumpnum);   /* SATURN: 0 = gated or POOLED -> no lock taken */
        } /* SATURN: end sorted visplane loop */
    }

#if SAT_PLANE_LOCAL
    /* P3: draw the queued regular flats -- master + slave each draw a half (the d32xr visplane
       split).  sat_plane_parallel (set by the Mimas platform via main.cxx) enables the slave
       half via r_parallel.c; otherwise (DoomJo / off) the master draws them all.  Then release
       the deferred flat locks (no-op on the cart, where W_CacheLumpNum is a direct pointer). */
    {
        extern int sat_plane_parallel, sat_local_players;
        extern void RP_DrawPlanesSplit(int n);
        int n = plane_worklist_n, i;
        /* SATURN M7 (2026-07-17): draw the queued planes MASTER-ONLY in lowres.  The slave
           plane-split does NOT help in M7 -- packed planes are light, so the master's work-steal
           claims them all before the slave even starts (SLV id100%, b0 Pb0 at the peak) -- yet
           RP_DrawPlanesSplit's RP_WaitPlanes join still spins up to 30M iterations (~2-3s = the
           "game stops for several seconds, regularly") waiting for a PLANE_DONE the idle slave
           never publishes.  Master-only loses zero work and removes the spin.  The floors-done
           hook (sat_vdp1_floors_done -> VDP1 present flip) fires at the END of R_DrawPlanes on
           BOTH paths, so New Game still clears the stale VDP1 walls; GBR+72 is rewound every
           frame by the DG_DrawFrame reset, independent of this dispatch. */
        /* SATURN 2026-07-20 (freeze fix): ALSO require single-player.  !sat_lowres was a proxy for
           "M7 = master-only", but the M7 pause-fullres flips sat_lowres->0 for a 1p menu; starting a
           New Game INTO co-op from that menu renders ONE split frame while sat_lowres is still 0 (the
           count-change hook re-pins it only on the next poll_pad) -> the slave plane-split gets
           dispatched in a split it was never set up for -> the RP_WaitPlanes 30M spin below wedged =
           the reported freeze.  In the parked-M7 world split is always master-only anyway, so this
           only makes it explicit + race-proof.  (Un-parked M4 co-op would lose dual-CPU planes here --
           acceptable, flagged: M7 split is already >= M4 split on HW.) */
        /* SATURN 2026-08-20: `sat_local_players <= 1` relaxed behind sat_mp_slave -- the freeze
           this gate guarded (RP_WaitPlanes unbounded spin on a mid-menu count flip) was hardened
           away long before (FRT-bounded + idempotent fallback); the MP-off just outlived it.
           Each view's dispatch JOINS inside this call, so no slave work crosses a view switch. */
        /* row 5 `Pv` closes HERE: everything above in R_DrawPlanes is the worklist BUILD
           (flat resolution + lighting + span setup, per visplane), everything below is the FILL
           (Pm on the master, Ps on the slave, concurrently).  See RP_BeginMasked for the
           subtraction that made this bracket necessary. */
        { extern void RP_MarkP (int); RP_MarkP (1); }
        if (sat_plane_parallel && n > 1)   /* sat_mp_slave baked ON 2026-08-26: split shares the slave */
            RP_DrawPlanesSplit(n);           /* master+slave: static half-split or work-steal (pad Y).
                                                SATURN M7 2026-07-30: the `!sat_lowres` hard-off is GONE --
                                                HW-validated as the shipped default (SLV b23% Pb59%, to=0,
                                                27fps vs 24).  The 2026-07-18 freeze it guarded against is
                                                gone (RP_WaitPlanes is FRT-bounded + m<0-skips + idempotent
                                                fallback, r_parallel.c); the packed spanfunc is identical
                                                on both CPUs so the disjoint visplanes write byte-correct.
                                                The masked-split (r_things.c) primes SGL slave scheduling. */
        else
        {
            extern void RP_MPlaneEnter(void), RP_MPlaneLeave(void);   /* row 5 `Pm` bracket */
            RP_MPlaneEnter();
            R_DrawPlaneWorklist(0, n);       /* MP / single plane / DoomJo: MASTER-ONLY */
            RP_MPlaneLeave();
        }
        for (i = 0; i < n; i++)
            if (plane_worklist[i].lumpnum >= 0)      /* -1 = never cached (load budget) */
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
