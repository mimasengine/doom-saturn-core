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
//	All the clipping: columns, horizontal spans, sky columns.
//






/* SATURN: O3 for the wall renderer — the single hottest compilation unit.
   Enables aggressive scheduling and inlining for the FixedMul-heavy paths. */
#pragma GCC optimize("O3")

#include <stdio.h>
#include <stdlib.h>

#include "i_system.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"
#include "z_zone.h"	/* SATURN: Z_Malloc for the lead-fill quad history (Doom heap, not the pool) */
#include "r_parallel.h"	/* SATURN PERF: RP_WallPrep{Enter,Leave} (profiler) */


// OPTIMIZE: closed two sided lines as single sided

// True if any of the segs textures might be visible.
boolean		segtextured;	

// False if the back side is the same plane.
boolean		markfloor;	
boolean		markceiling;

boolean		maskedtexture;
int		toptexture;
int		bottomtexture;
int		midtexture;


angle_t		rw_normalangle;
// angle to line origin
int		rw_angle1;	

//
// regular wall
//
int		rw_x;
int		rw_stopx;
angle_t		rw_centerangle;
fixed_t		rw_offset;
fixed_t		rw_distance;
fixed_t		rw_scale;
fixed_t		rw_scalestep;
fixed_t		rw_midtexturemid;
fixed_t		rw_toptexturemid;
fixed_t		rw_bottomtexturemid;

int		worldtop;
int		worldbottom;
int		worldhigh;
int		worldlow;

fixed_t		pixhigh;
fixed_t		pixlow;
fixed_t		pixhighstep;
fixed_t		pixlowstep;

fixed_t		topfrac;
fixed_t		topstep;

fixed_t		bottomfrac;
fixed_t		bottomstep;


lighttable_t**	walllights;

short*		maskedtexturecol;



//
// R_RenderMaskedSegRange
//
void
R_RenderMaskedSegRange
( drawseg_t*	ds,
  int		x1,
  int		x2 )
{
    unsigned	index;
    column_t*	col;
    int		lightnum;
    int		texnum;
    
    // Calculate light table.
    // Use different light tables
    //   for horizontal / vertical / diagonal. Diagonal?
    // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
    curline = ds->curline;
    frontsector = curline->frontsector;
    backsector = curline->backsector;
    texnum = texturetranslation[curline->sidedef->midtexture];
	
    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

    if (curline->v1->y == curline->v2->y)
	lightnum--;
    else if (curline->v1->x == curline->v2->x)
	lightnum++;

    if (lightnum < 0)		
	walllights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
	walllights = scalelight[LIGHTLEVELS-1];
    else
	walllights = scalelight[lightnum];

    maskedtexturecol = ds->maskedtexturecol;

    rw_scalestep = ds->scalestep;		
    spryscale = ds->scale1 + (x1 - ds->x1)*rw_scalestep;
    mfloorclip = ds->sprbottomclip;
    mceilingclip = ds->sprtopclip;
    
    // find positioning
    if (curline->linedef->flags & ML_DONTPEGBOTTOM)
    {
	dc_texturemid = frontsector->floorheight > backsector->floorheight
	    ? frontsector->floorheight : backsector->floorheight;
	dc_texturemid = dc_texturemid + textureheight[texnum] - viewz;
    }
    else
    {
	dc_texturemid =frontsector->ceilingheight<backsector->ceilingheight
	    ? frontsector->ceilingheight : backsector->ceilingheight;
	dc_texturemid = dc_texturemid - viewz;
    }
    dc_texturemid += curline->sidedef->rowoffset;
			
    if (fixedcolormap)
	dc_colormap = fixedcolormap;
    
    // draw the columns
    for (dc_x = x1 ; dc_x <= x2 ; dc_x++)
    {
	// calculate lighting
	if (maskedtexturecol[dc_x] != SHRT_MAX)
	{
	    if (!fixedcolormap)
	    {
		index = spryscale>>LIGHTSCALESHIFT;

		if (index >=  MAXLIGHTSCALE )
		    index = MAXLIGHTSCALE-1;

		dc_colormap = walllights[index];
	    }
			
	    sprtopscreen = centeryfrac - FixedMul(dc_texturemid, spryscale);
	    dc_iscale = 0xffffffffu / (unsigned)spryscale;
	    
	    // draw the texture
	    col = (column_t *)( 
		(byte *)R_GetColumn(texnum,maskedtexturecol[dc_x]) -3);
			
	    R_DrawMaskedColumn (col);
	    maskedtexturecol[dc_x] = SHRT_MAX;
	}
	spryscale += rw_scalestep;
    }
	
}




//
// R_RenderSegLoop
// Draws zero, one, or two textures (and possibly a masked
//  texture) for walls.
// Can draw or mark the starting pixel of floor and ceiling
//  textures.
// CALLED: CORE LOOPING ROUTINE.
//
#define HEIGHTBITS		12
#define HEIGHTUNIT		(1<<HEIGHTBITS)

/* SATURN Potato walls: when enabled, the wall recorder paints each opaque wall
   column with the texture's dominant colour (sat_wall_color), set here per wall
   section so the whole wall is one continuous hue.  Off by default. */
extern int  sat_potato_walls;
extern int  sat_wall_nocpu;     /* SATURN: banded/flat VDP1 modes skip the close-wall CPU fallback */
extern int  sat_wall_color;
/* SATURN per-frame texture LOAD BUDGET -- see the block above the column loop in R_StoreWallRange.
   budget 0 = OFF = every texture faults in on sight (the pre-2026-08-06 behaviour). */
int sat_tex_load_budget = 0;    /* textures allowed to fault in per frame (pad chord)          */
int sat_tex_load_spent  = 0;    /* spent this frame (reset in R_ClearDrawSegs)                 */
int sat_wall_flat_io    = 0;    /* tiers drawn flat for want of residency  (~1 s window)       */
int sat_wall_flat_nocol = 0;    /* ...of which we had no cached colour either (~1 s window)    */
/* Neutral index used when a texture has never been resident, so its dominant colour was never
   computed and CANNOT be without loading it.  Mid-grey in the Doom palette; dc_colormap still
   shades it by distance/sector light, so it reads as a lit surface, not a hole. */
#define SAT_WALL_FLAT_UNKNOWN 100

extern int  R_TextureIOFree (int tex);
extern int  R_WallPotatoColorPeek (int tex);
extern int  R_WallPotatoColor (int tex);

/* 1 = draw this tier FLAT because texturing it would hit the disc and the frame's budget is spent. */
static int sat_wall_io_flat (int tex)
{
    if (!sat_tex_load_budget)      return 0;   /* feature off */
    if (R_TextureIOFree (tex))     return 0;   /* free -> draw it properly, budget untouched */
    if (sat_tex_load_spent < sat_tex_load_budget)
    {
	/* Pay for it (BSP order => nearest walls win).  We are faulting the texture in anyway, so
	   compute its DOMINANT COLOUR in the same breath: R_WallPotatoColor is only ever called by
	   the potato mode, which is off in normal play, so without this the colour cache stays EMPTY
	   and every flattened wall falls back to neutral grey -- measured `nocol` = 100% of `flat`
	   on the owner's TNT MAP11 captures.  Priming here makes every later fallback for this
	   texture exact, at the cost of one extra pass over a texture we are already loading. */
	sat_tex_load_spent++;
	R_WallPotatoColor (tex);
	return 0;
    }
    sat_wall_flat_io++;
    return 1;
}

/* Flat colour for an IO-flattened tier, resolved ONCE PER TIER (the draw sites below live INSIDE
   the column loop -- calling this there counted `nocol` per COLUMN, which is why it read larger
   than `flat`, and paid a peek per column for nothing).  MUST NOT call R_WallPotatoColor: that
   walks the texture through R_GetColumn and would perform the very read we are avoiding. */
static int sat_wall_flat_color (int tex)
{
    int c = R_WallPotatoColorPeek (tex);
    if (c < 0) { c = SAT_WALL_FLAT_UNKNOWN; sat_wall_flat_nocol++; }
    return c;
}
extern int  sat_wall_paint;   /* SATURN debug paint (r_data.c): bit1 = CPU walls flat red */
extern int  sat_wall_textured;
extern int  R_WallPotatoColor (int tex);
extern int* texturewidthmask;   /* r_data.c: texture u-period, for the subdiv squish guards */

/* SATURN: hand one-sided (solid) walls to a platform VDP1 world renderer.  NULL on
   DoomJo / when unused -> normal software wall.  Called per seg with the wall's 4
   SCREEN corners + texture + light (corners from the same topfrac/bottomfrac the
   software loop steps).  Step 2 = validate the real-wall projection + affine warp.
   RETURNS: 0 = the platform queued the wall for VDP1; 1 = REJECTED (its command list is
   full / VDP1 starved) -> the caller draws that tier in SOFTWARE instead of dropping it
   (no sky-through-walls under load -- Romain's "fallback CPU plutot que skip"). */
int (*sat_wall_hook)(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                     int texnum, int u1, int u2, int v0, int v1,
                     const unsigned char *cmap) = 0;

/* SATURN L5 (near-wall edge split): ask the PLATFORM which sub-range of a tier's columns it can
   actually emit.  The acceptance test lives in dg_saturn's wall_emit_band -- the view window +/-
   wall_ext, plus the 13-bit VDP1 coordinate clamp -- and a tile that fails it takes the "clamp +
   squish" fallback that compresses a whole texw-wide texture into the clipped span.  THAT is the
   ecrasement.  Duplicating the test in core would let the two drift, so the splitter REPLAYS the
   emitter's own tile loop and returns the widest run of ACCEPTED tiles.
   RETURNS 1 = usable interior: *xL/*xR = its screen columns, *uL/*uR = its tile-aligned texture
   bounds.  0 = nothing emittable, and *why says which clause lost:
     1 = LATERAL  (tile lands outside the view window +/- wall_ext) -> CPU borders can rescue it;
     2 = MAGNITUDE(texw * magnification exceeds the frame-buffer plane) -> no screen-space split can
         ever help; only a narrower BAKED sub-texture would.  This one is HARDWARE, not caution:
         VDP1 UM p.21 -- "nothing is written for parts that exceed the range of the frame buffer
         plane" (+/-1024).  A quad always draws a WHOLE character, so the smallest unit VDP1 can
         place is one full texw tile; once texw * magnification leaves that plane the tile is
         undrawable at ANY screen subdivision (docs/VDP1_LIMITS_SOURCED.md 1.3/1.5).
   So one capture of row-8 `e`/`b` tells us whether building that rebake is worth it. */
int (*sat_wall_edge_hook)(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                          int u1, int u2, int texw,
                          int *xL, int *xR, int *uL, int *uR, int *why) = 0;
/* Row-8 sizer for L5.  `e<got>/<want>` + `b<L><M><T><R>`: a single capture must be able to explain a
   NULL result, or a flat Bp proves nothing (the `to` lesson -- never judge a lever through an
   instrument that cannot show why it did nothing).  want = tiers that ASKED for the split. */
int sat_fb_edge_w = 0;     /* tiers that armed the split this frame (the denominator)            */
int sat_fb_edge_b[4] = {0,0,0,0};  /* bail causes: 0 lateral, 1 magnitude, 2 too-thin, 3 refused */

/* SATURN: when the VDP1 world renderer owns the one-sided walls, skip the software
   midtexture column draw (R_GetColumn + colfunc) for them -> measure the perf the
   VDP1 path buys back.  The ceiling/floor clip + visplane marking still run (floors,
   ceilings, sprite occlusion stay correct).  0 on DoomJo / when unused. */
int sat_wall_skip = 0;
extern int sat_split_active;   /* SATURN split-screen: VDP1 is single-view -> emit software only */
extern int sat_split_vdp1;     /* ...unless Step 3 keeps walls on VDP1 per-view (platform offsets x) */
#define SAT_WALL_VDP1_OK (!sat_split_active || sat_split_vdp1)  /* wall hook fires: 1p OR VDP1-split */
/* SATURN: set when the floor is rendered on the VDP2 RBG0 hardware plane (r_plane.c).
   The RBG0 floor is transparent (index 0), so unlike the old opaque software floor it no
   longer occludes the walls behind it -> lower-area walls would show through the floor.
   We restore that occlusion by CULLING (not emitting) any wall whose whole screen span is
   below the floor line -- not by clamping the quad (which would SQUISH the texture, since
   VDP1 maps the full texture across the quad).  Off => no change (DoomJo, normal build). */
extern int sat_vdp2_floor;
extern int sat_floor_punch_here(void);   /* SATURN split: gate the floor cohesion sites per-viewport (1p: == sat_vdp2_floor) */
extern int sat_vdp1_floor;               /* SATURN: secondary floors/ceilings deported to VDP1 (gates the above-ceiling wall clamp) */

/* SATURN close-wall CPU fallback: a seg whose projected vertical span (px) exceeds this is so
   close/grazing that its VDP1 textured quad would explode the fill (VDP1 rasterises the whole
   off-screen-tall quad) -> overrun -> sky through walls.  Render THOSE in SOFTWARE instead:
   Doom's per-column renderer clips them to the screen correctly (no explosion, no texture
   swim), and via the layer inversion they land in NBG1 ON TOP of the farther VDP1 walls --
   correct, since a too-close wall is the nearest.  Match the magnitude to the platform; only
   near-touching walls trip it (few columns of CPU work, occasional). */
#define SAT_WALL_CPU_SPAN 480   /* DEFAULT span > this: too close for VDP1 -> render in SOFTWARE (CPU). */
/* SATURN 2026-07-26: RUNTIME so the platform's budget-driven LOD can LOWER it (push more near walls to
   software to free the VDP1 raster) when the MEASURED VDP1 budget is tight and the master has headroom,
   then relax it back to the default.  DoomJo links it and never writes it -> stays 480 = old behaviour. */
int sat_wall_cpu_span = SAT_WALL_CPU_SPAN;
#define SAT_WALL_CPU_V1   576   /* DEFAULT: VDP1 starts EARLY (span < this, above the CPU threshold) so
                                   it pre-warms the pipeline a frame+ before the CPU hands off -- on
                                   Saturn the VDP1 presents >2 frames late, so the CPU exit-frames
                                   alone still showed sky.  Band [SPAN,V1] = CPU + VDP1 both. */
/* SATURN 2026-07-26: RUNTIME, driven WITH sat_wall_cpu_span (the platform keeps V1 = span + 96 so the
   pre-warm band width is preserved as the budget LOD shifts the whole near-wall CPU window down/up).
   V1 is the true VDP1-exit threshold (s >= V1 -> off VDP1), so LOWERING it is what actually frees the
   VDP1 raster.  DoomJo links it, never writes it -> stays 576 = old behaviour. */
int sat_wall_cpu_v1 = SAT_WALL_CPU_V1;
/* SATURN 2026-08-03: hysteresis width on the CPU<->VDP1 span threshold, in screen px.  A seg that
   was on the CPU within the last 2 frames (= its exit countdown is still armed) keeps the CPU until
   `s` falls this far BELOW sat_wall_cpu_span, instead of flipping the instant it crosses.  Sized as
   half the one-sided pre-warm band (96): wide enough that ordinary walking cannot oscillate across
   it, narrow enough that it never holds a wall on the CPU that clearly belongs on VDP1. */
#define SAT_WALL_HYST     48
#define SAT_WALL_CPU_MAG  3     /* MAGNIFICATION (screen px per texel of u) above which a wall is so
                                   close/face-on that its VDP1 tiling extrapolates past the screen edge
                                   and the platform SQUISHES it ("ecrasement", worst on doors) -- and
                                   VDP1 can't draw it right (DISTORSP has no texture column-subrange).
                                   Render those in SOFTWARE.  Catches close DOORS (short bands the span
                                   test misses).  Grazing walls have LOW mag -> stay on VDP1.  Lower =
                                   more walls to CPU (safer, costlier); higher = fewer (risk squish). */

/* PERSPECTIVE-CORRECT NEAR-WALL SUBDIVISION (owner 2026-07-02; workflow wbq3s2c52).  A magnified
   (close/face-on) wall squishes on VDP1 because wall_emit_band inverts u->x LINEARLY -- a 2-point
   affine chord through the true tangent curve u(x)=rw_offset-finetangent[..]*rw_distance.  Instead of
   dumping it to the CPU, split it into N narrow SCREEN-COLUMN sub-segments and RE-SAMPLE u at each
   boundary via that exact formula -> each affine tile is a local chord where the curve is ~linear ->
   sub-pixel error, no squish; u is world-anchored so NO swim; yl/yh stay exact (linear in x).  N is
   derived for free from the same mag quantity (columns-per-texel), capped so a few close walls can't
   starve the far ones.  0 = old behaviour (magnified -> CPU), byte-identical (A/B). */
#define SAT_WALL_SUBDIV      1
#define SAT_WALL_SUBDIV_MAX  6   /* level-0 cap on sub-segments per magnified wall (budget guard).
                                    SATURN L4 (sat_opt >= 4) lowers it to sat_wall_subdiv_max (3):
                                    each sub-segment costs a VDP1 quad AND a SAT_VROWS divide, and
                                    the affine error it buys back is sub-pixel past ~3 chords. */

/* EDGE FILL (owner 2026-07-02): a VDP1 wall is only visible through NBG1's index-0 mask, so in rotation
   its VDP1 body (current) is offset from that lagged NBG1 mask -> a horizontal "decrochage" sky gap at
   the wall's LEFT/RIGHT edges.  SOFTWARE pixels are at NBG1's OWN latency = aligned with the mask, so the
   CPU drawing the wall's first/last N columns in software COVERS that gap (body stays VDP1/fast; only the
   thin edge strips are CPU).  This is the horizontal analog of Topic-B, done on NBG1 so it aligns.
   0 = off.  DoomJo-safe: gated on sat_wall_skip (0 on DoomJo -> inert). */
#define SAT_WALL_EDGE_FILL   0    /* DISABLED (owner 2026-07-02): wrong target.  The rotation sky-gap is NOT at the
                                     wall's L/R edges -- it is the deported CEILING/FLOOR VDP1 quad lagging its NBG1
                                     silhouette mask.  Filling wall edges (textured, = the VDP1 wall visually) never
                                     touched that region -> "identique".  The fix lives on the deported-quad emit
                                     (dg_saturn sat_floor_vdp1_emit: HORIZONTAL overflow), not here.  Kept as an
                                     inert A/B stub. */

/* SATURN Phase-0 CPU-fallback profiler (measures the "walls handed to software" prize BEFORE
   building the VDP1 wall clamp of Phase 1).  By cause:
     clamp  = vertical SPAN (>V1, fully CPU) or partially-below-the-RBG0-floor-line -- a world-
              anchored VDP1 vertical clamp (Phase 1) would keep these on VDP1;
     mag    = face-on MAGNIFICATION (short face-on doors) -- the hard residue the clamp can't fix;
     starve = the VDP1 command bank was full -- Phase-1 subdivision would make this WORSE.
   px = span*cols fill-work proxy for the clampable tiers (the master software cost Phase 1 removes).
   Plain ints, 0-default, reset per frame by the platform (dg_saturn vdp1_wpn_kick) -> DoomJo links
   AND renders unchanged: it never reads them, and every increment lives inside the sat_wall_skip
   hybrid routing, which is inert on DoomJo (sat_wall_skip == 0). */
int sat_fb_clamp_t = 0, sat_fb_mag_t = 0, sat_fb_starve_t = 0, sat_fb_px = 0;
int sat_fb_wclamp_t = 0;   /* Phase-1: tiers KEPT on VDP1 by the cut+wedge clamp this frame */

/* SATURN Phase-1 wall clamp ([[wall-clamp-world-anchored]]): when set, a SPAN-close one-sided
   wall STAYS on VDP1 (clamped swim-free in wall_emit_band via the constant-z linear v->y map)
   instead of the CPU software fallback -- the Option-2 lever.  MAGNIFIED walls still go to CPU
   (the vertical clamp can't fix the horizontal squish).  Default 0 = shipping byte-identical;
   the platform sets it from the SAT_WALL_CLAMP compile flag (dg_saturn) for the HW A/B. */
int sat_wall_clamp = 0;

/* SATURN PERF LEVERS (2026-07-30) -- CUMULATIVE A/B level, pad L+C (1p).  Each step ADDS one
   optimisation so the tester can bisect the gain LIVE: build-vs-build fps photos are invalid
   (+-6ms of Bp noise from ~600B .bss shifts), so every lever must be togglable in-session.
     0 = all off -- the 2026-07-29 shipping code, the byte-identical reference.
     1 = + L1 span fill (r_plane.c R_TexturedSpan, the ld path = THE #1 inner loop): the two
           pixels of an ld pair are IDENTICAL bytes, so emit them as ONE 16-bit store and drop
           the odd-tail branch from the hot loop.  Byte-identical output.
     2 = + L2 clip-scan hoist: ONE per-seg min(floorclip)/max(ceilingclip) pass replaces the
           per-TIER full column scans in sat_wall_cross_lo and sat_wall_cut_floor.  EXACT: yh(x)
           is linear, so max(yh1,yh2) < min(floorclip) proves "crosses nowhere" in O(1).
     3 = + L3 reciprocal hoist: rw_scale is CONSTANT throughout the whole claim phase (it is only
           stepped in the column loop, which runs AFTER), so SAT_VROWS's 0xffffffff/rw_scale --
           re-evaluated up to 3 tiers x (1 + SUBDIV) = 21 times per seg, always with the same
           operand -- is computed ONCE.  Byte-identical.
     4 = + L4 subdivision cap SAT_WALL_SUBDIV_MAX(6) -> sat_wall_subdiv_max(3): fewer VDP1 quads
           AND fewer SAT_VROWS divides on magnified walls.  **NOT byte-identical** -- slightly
           more affine error on a near face-on wall.  The only step with a visible tradeoff.
     5 = + L5 near-wall EDGE SPLIT: a wall too close for a VDP1 quad is no longer dumped whole to
           the CPU.  VDP1 gets the tile-aligned INTERIOR (never extrapolated -> no squish), the CPU
           keeps only the partial edge tiles, where u is exact per column.  Targets the ~22ms
           nose-to-wall Bp.  Changes pixels (it REPLACES a full software wall with a hybrid), and
           bails out to the old full-CPU wall whenever the interior is too thin.  Overlay row-8 `e`
           counts the tiers it actually saved -- `e0` means it never engaged.
   Default 5.  DoomJo compiles and runs this unchanged; only steps 4 and 5 alter pixels. */
int sat_opt = 5;
int sat_wall_subdiv_max = 3;   /* the L4 cap (sat_opt >= 4); SAT_WALL_SUBDIV_MAX is the level-0 one */

/* SATURN L2: the per-SEG hoisted extremes of the two clip arrays over [rw_x, rw_stopx).
   Valid for the whole tier-claim phase because NOTHING writes floorclip/ceilingclip between the
   scan and the column loop.  sat_clip_have = 0 makes every consumer fall back to its own scan
   (so sat_opt < 2 is byte-identical, and so is a seg the scan skipped). */
static int sat_clip_fcm, sat_clip_ccm, sat_clip_have;

/* SATURN L5 -- NEAR-WALL EDGE SPLIT (sat_opt >= 5): CPU borders, VDP1 core.
   WHY the near wall squishes: dg_saturn's wall_emit_band tiles the wall in TEXTURE space and places
   each tile with a LINEAR u->x map anchored at (u1,x1) and (u2,x2).  Inside that interval it is
   INTERPOLATION -- error bounded by the chord, zero at the anchors.  But the tile loop starts at
   `umin & ~(texw-1)`, i.e. BEFORE u1, so the first and last tiles are placed by EXTRAPOLATING that
   map outside its fitted range, and extrapolation error is unbounded: nose-to-wall with ~2 visible
   tiles the edge tile lands most of a screen-width off.  That is the "ecrasement", and it is why the
   whole tier was routed to the CPU in 2026-07-02 ("v0 near-wall affine perspective warp = moche";
   the same comment names the way back in: finer near-tile u handling).
   THE SPLIT: give VDP1 only the TILE-ALIGNED INTERIOR [uL,uR] -> every emitted tile is interpolated,
   never extrapolated.  The partial edge tiles stay on the CPU, where u is resampled per column from
   finetangent[] and is therefore perspective-EXACT by construction -- strictly better than any extra
   sub-quad, which would still be affine within itself.  Cost is paid only on the thin borders.
   sat_we_on gates it per seg; 0 = the untouched full-CPU wall, so bailing out never regresses. */
#define SAT_WALL_EDGE_MIN 12      /* interior columns below which the split is not worth a quad */
static int sat_we_on, sat_we_lo, sat_we_hi;
int sat_fb_edge_t = 0;            /* tiers rendered as CPU-borders + VDP1-core this frame (overlay `e`) */

/* LAZY on purpose (Ymir A/B 2026-07-30): the first cut of L2 ran this eagerly once per seg and
   measured EXACTLY ZERO -- because sat_wall_cross_lo returns on its two END-column tests for most
   segs and never reaches its own scan, so an eager pass ADDS a full sweep instead of replacing one.
   Scanning on first demand makes L2 monotonically >= 0: the caller that asks was about to sweep
   anyway, and every later tier (plus sat_wall_cut_floor) then reuses the result for free. */
static void sat_wall_clip_need (void)
{
    int x, fcm = viewheight, ccm = -1;
    if (sat_clip_have) return;
    for (x = rw_x; x < rw_stopx; x++)
    {
	if (floorclip[x]   < fcm) fcm = floorclip[x];
	if (ceilingclip[x] > ccm) ccm = ceilingclip[x];
    }
    sat_clip_fcm = fcm; sat_clip_ccm = ccm; sat_clip_have = 1;
}

/* EXTRA CPU FRAMES on EXIT (Romain): when a one-sided wall leaves the CPU path (moves away,
   CPU->VDP1), the VDP1 presents several frames late -> a sky gap at the seam.  So for N frames after
   it stops being CPU, the CPU ALSO draws it (overlap) while VDP1 catches up.  N=2.  Per-seg (not a
   span band): only the wall that actually transitions pays, those frames only -- economical.  Keyed
   by the seg index (seg_t's are
   static level data -> stable pointer); 1 byte = CPU-exit-frame countdown. */
#define SAT_SEG_MAX 4096
static unsigned char sat_seg_cpu[SAT_SEG_MAX];

/* EXTRA CPU FRAMES on ENTRY (SATURN 2026-08-02) -- the missing symmetry of the exit overlap above.
   A wall that was NOT visible last frame and goes straight to VDP1 is drawn by NOBODY on its first
   frame: VDP1 owns it so the software column loop skips it, but the VDP1 list only reaches the
   screen a field later, so those pixels stay index 0 (sky) for one frame.  Nil at rest, and it
   scales with movement speed because that is what governs how many walls come into view per frame.
   No shift of the VDP1 layer can fix it -- the quad is not misplaced, it is not there yet.  So the
   CPU also draws a NEW VDP1 wall for its first sat_wall_entry frames.  Only walls that actually
   appear pay, and only those frames.

   Packed into the EXISTING per-seg byte, no new RAM (the TLSF pool has ~4.4KB of headroom and a
   second [4096] array would black-screen the boot -- [[boot-loop-can-be-tlsf-pool-starvation]]):
       [7:4] frame tag     [3:2] entry countdown     [1:0] exit countdown
   The tag is the low nibble of sat_seg_frame, so "was visible last frame" is a gap of exactly 1.
   A seg absent for exactly 16 (or 32, ...) frames aliases back onto the current tag and silently
   misses its coverage -- ~1.2s at M7 rates, and that failure mode is just today's behaviour. */
#define SAT_SEG_ENTRY_MAX 3
int sat_wall_entry = 1;   /* CPU frames covering a newly-visible VDP1 wall, 0..3; 0 = off.
                             Live on pad L+Left / L+Right (1p), read back as row-13 `En<n>`. */
int sat_seg_frame  = 0;   /* advanced ONCE PER RENDERED FRAME by the platform, after the BSP walk.
                             A port that never advances it (DoomJo) sees gap 0 -> coverage inert. */
/* ORPHAN COUNT (2026-08-03).  Wall TIERS claimed by NEITHER path this frame -- neither the software
   column loop (sat_sw_*) nor VDP1 (sat_v1_*, incl. the subdivided variants).  Such a tier is a hole
   by construction: it is exactly the "mur qui disparait" the owner sees, and the count says whether
   it happens WITHOUT having to catch the frame by eye (row 13 `N<n>`, summed over the ~1 s overlay
   window, reset at print).  N0 while a wall still vanishes => the routing is fine and the loss is
   further down, at emission or in the VDP1 plot -- a completely different search. */
int sat_wall_nodraw = 0;
/* PATH FLIPS (2026-08-03).  Segs that were on the CPU last frame and are not this frame -- i.e. a
   CPU -> VDP1 handoff actually happened.  Detected with no new state: the exit countdown is ARMed to
   2 on every CPU frame and decremented once per frame after, so reading exactly 2 while cpu_now is
   false means "CPU last frame, VDP1 now".  This is the number the HYSTERESIS is supposed to drive to
   ~0; if it stays high the hysteresis is not biting, and if it reaches 0 while walls still vanish
   then the flip was never the cause. */
int sat_wall_flip = 0;
extern int sat_dc_solid;   /* r_draw.c: this colfunc() call is an opaque WALL column -> solid colour */

/* ===== SATURN VDP1 LEAD-FILL (owner's spec, 2026-08-03) ==================================
   *"JE VEUX QUE TU IMPLEMENTE, SUR LE CPU, LE DESSIN DE CE QUI N'EST PAS COUVERT PAR LA POSITION
   DE L'ANCIEN MUR A LA FRAME N-X.  TON TOGGLE DEVRAIT ME PERMETTRE DE CHANGER X.  SPOILER, CE
   N'EST PAS UNE BANDE, C'EST SURFACE DU NOUVEAU MUR AMPUTEE DE CE QUI ETAIT DEJA COUVERT PAR
   L'ANCIEN MUR"*

   VDP1 shows a list that is X rendered frames old.  Wherever the wall is NOW but was NOT then,
   NBG1 has been left at index 0 (the software skipped the tier, VDP1 owns it) and nothing is drawn
   -- that is the hole.  So the software draws exactly the SET DIFFERENCE, per column:

       new tier rows [yl(x), yh(x)]   MINUS   old quad rows [oyl(x), oyh(x)]

   which is 0, 1 or 2 spans per column (the old quad can sit inside the new one), plus the whole
   column wherever the old quad did not reach at all.  The complement -- where the old wall was and
   the new one is not -- needs nothing: the software draws the plane there and NBG1 sits ABOVE VDP1,
   so the stale quad is erased for free.

   SELF-CALIBRATING: if VDP1 is in phase, old == new, the difference is empty and it costs nothing.
   It never needs to know how late VDP1 actually is -- only X, which the owner sets on the pad.

   History = the quads actually handed to VDP1, one ring slot per rendered frame, keyed by
   (seg << 2 | tier).  Z_Malloc'd on first use so it lives in the Doom heap (LWRAM) and costs the
   HWRAM TLSF pool nothing.  Walls past SAT_LEADH_MAX in a frame simply get no history (they draw
   as before) -- a bounded degradation, never a wrong pixel. */
int sat_wall_lead_x = 1;   /* 0 = off, else compare against the quad emitted X frames ago (pad R+A) */
int sat_lead_cols   = 0;   /* diagnostic: extra software column-spans drawn, per overlay window     */
/* DIFFERENCE-SPAN DRAW MODE (pad R+Right, row-13 `L<X><m>/<spans>`).
     0 = '-' master, TEXTURED -- the reference.
     1 = 's' SLAVE, textured (owner 2026-08-03: *"le mode flat n'est pas ideal (visible meme en
         mouvement), je prefere qu'on garde texture. deporte sur le slave"*).  The spans are
         RECORDED during the BSP walk and drawn by the 2nd SH-2 while the master draws the planes.
         See sat_lead_spans / R_LeadSlaveDraw.
     2 = 'f' master, FLAT (the texture's own dominant colour): skips R_GetColumn, the composite,
         i.e. the memory-bound half of wall-prep.  Kept as the cheap fallback -- the owner judged it
         visible in motion, so it is no longer the default.
   ⚠ Mode 1 moves the FILL, not the composite: R_GetColumn mutates the shared texture cache, so it
   has to stay on the master (that is what killed wall-prep-on-slave three times over). */
int sat_lead_mode   = 1;
#define sat_lead_flat (sat_lead_mode == 2)

/* Recorded difference spans, drained by the slave.  Z_Malloc'd with the quad history (Doom heap,
   LWRAM) so the HWRAM pool pays nothing.  src == 0 means SOLID, colour in `col`. */
typedef struct { short x, yl, yh, col; const byte *src; const byte *cmap;
		 fixed_t iscale, texmid; } sat_leadspan_t;
#define SAT_LEADSPAN_MAX 1536
static sat_leadspan_t *sat_lead_spans;
static int sat_lead_span_n;
int sat_lead_span_drop;      /* spans the cap refused this frame -- silence would read as "nothing to do" */

/* Drain the recorded spans.  Runs on the SLAVE, CONCURRENTLY with the master's R_DrawPlanes, so it
   must not touch a single dc_* global (the master's sky columns use them).  Everything it needs is
   in the record; the inner loops mirror R_DrawColumn / R_DrawColumnLow with locals.
   Pixel-safe against the planes it overlaps: Doom clips every visplane to the ceilingclip/floorclip
   the wall loop just wrote, so a plane never owns a row a wall tier owns. */
void R_LeadSlaveDraw (void)
{
    extern byte *ylookup[]; extern int columnofs[];   /* r_draw.c, no header */
    extern int sat_lowres;                           /* r_main.c */
    /* ⚠ MIRROR R_ExecuteSetViewSize'S DRAWER CHOICE EXACTLY.  In M7 detailshift is 1 -- it drives
       the 160-column projection and the VDP1 x<<1 -- but the master still uses the NORMAL
       R_DrawColumn, because the framebuffer is PACKED 160 wide (one byte per logical column) and
       the *Low drawers would re-duplicate it back to 320.  Branching on detailshift alone put every
       slave span at double x, duplicated: the owner's *"les dessins du slave ne sont pas au bon
       endroit sur l'image"*. */
    int lowdraw = (detailshift && !sat_lowres);
    int i;
    for (i = 0 ; i < sat_lead_span_n ; i++)
    {
	const sat_leadspan_t *sp = &sat_lead_spans[i];
	int count = sp->yh - sp->yl;
	byte *dest;
	if (count < 0) continue;
	if ((unsigned)sp->x >= (unsigned)SCREENWIDTH || sp->yl < 0 || sp->yh >= viewheight) continue;
	if (lowdraw)
	{
	    int x = sp->x << 1;
	    byte *d2;
	    dest = ylookup[sp->yl] + columnofs[x];
	    d2   = ylookup[sp->yl] + columnofs[x + 1];
	    if (!sp->src)
	    {
		byte c = sp->cmap[(unsigned char)sp->col];
		do { *d2 = *dest = c; dest += SCREENWIDTH; d2 += SCREENWIDTH; } while (count--);
	    }
	    else
	    {
		fixed_t frac = sp->texmid + (sp->yl - centery) * sp->iscale;
		do { byte px = sp->cmap[sp->src[(frac >> FRACBITS) & 127]];
		     *d2 = *dest = px; dest += SCREENWIDTH; d2 += SCREENWIDTH; frac += sp->iscale; }
		while (count--);
	    }
	}
	else
	{
	    dest = ylookup[sp->yl] + columnofs[sp->x];
	    if (!sp->src)
	    {
		byte c = sp->cmap[(unsigned char)sp->col];
		do { *dest = c; dest += SCREENWIDTH; } while (count--);
	    }
	    else
	    {
		fixed_t frac = sp->texmid + (sp->yl - centery) * sp->iscale;
		do { *dest = sp->cmap[sp->src[(frac >> FRACBITS) & 127]];
		     dest += SCREENWIDTH; frac += sp->iscale; }
		while (count--);
	    }
	}
    }
}

int  R_LeadSpanCount (void) { return sat_lead_span_n; }
void R_LeadSpanReset (void) { sat_lead_span_n = 0; }

static void sat_lead_span_add (int yl, int yh)
{
    sat_leadspan_t *sp;
    if (sat_lead_span_n >= SAT_LEADSPAN_MAX || !sat_lead_spans) { sat_lead_span_drop++; return; }
    sp = &sat_lead_spans[sat_lead_span_n++];
    sp->x = (short)dc_x; sp->yl = (short)yl; sp->yh = (short)yh;
    sp->cmap = dc_colormap;
    if (sat_dc_solid) { sp->src = 0; sp->col = (short)sat_wall_color; sp->iscale = 0; sp->texmid = 0; }
    else              { sp->src = dc_source; sp->col = 0;
			sp->iscale = dc_iscale; sp->texmid = dc_texturemid; }
}

#define SAT_LEADH_DEPTH 6     /* ring slots: current + X up to 3, and the jitter pair needs X+1 */
#define SAT_LEADH_MAX   128   /* quads recorded per frame (matches the platform's WALL_ACC_MAX) */

typedef struct { short key, x1, x2, yl1, yh1, yl2, yh2; } sat_leadq_t;
static sat_leadq_t *sat_leadh[SAT_LEADH_DEPTH];
static short        sat_leadh_n[SAT_LEADH_DEPTH];
static int          sat_leadh_cur = 0;
static int          sat_leadh_hint = 0;   /* rolling scan start: BSP order is stable frame to frame */

/* Per-tier state for the fragment being drawn: the old quad, stepped per column. */
typedef struct { int on, x1, x2;   fixed_t ylf,  ylstep,  yhf,  yhstep;
                 int on2, x1b, x2b; fixed_t ylfb, ylstepb, yhfb, yhstepb; } sat_lead_t;
static sat_lead_t sat_lead_mid, sat_lead_up, sat_lead_lo;

void sat_lead_frame_begin (void)   /* once per view, from R_ClearDrawSegs */
{
    int i;
    if (!sat_wall_lead_x) return;
    if (!sat_leadh[0])
	{
	    for (i = 0 ; i < SAT_LEADH_DEPTH ; i++)
		sat_leadh[i] = Z_Malloc (sizeof(sat_leadq_t) * SAT_LEADH_MAX, PU_STATIC, NULL);
	    sat_lead_spans = Z_Malloc (sizeof(sat_leadspan_t) * SAT_LEADSPAN_MAX, PU_STATIC, NULL);
	}
    sat_leadh_cur  = (sat_leadh_cur + 1) % SAT_LEADH_DEPTH;
    sat_leadh_n[sat_leadh_cur] = 0;
    sat_leadh_hint = 0;
}

static void sat_lead_record (int key, int x1, int yl1, int yh1, int x2, int yl2, int yh2)
{
    sat_leadq_t *q;
    if (!sat_wall_lead_x || !sat_leadh[0]) return;
    if (sat_leadh_n[sat_leadh_cur] >= SAT_LEADH_MAX) return;
    q = &sat_leadh[sat_leadh_cur][sat_leadh_n[sat_leadh_cur]++];
    q->key = (short)key;
    q->x1  = (short)x1;  q->yl1 = (short)yl1;  q->yh1 = (short)yh1;
    q->x2  = (short)x2;  q->yl2 = (short)yl2;  q->yh2 = (short)yh2;
}

/* Find this tier's quad in ring slot `back` frames ago.  ⚠ MATCHED ON THE KEY **AND** ON COLUMN
   OVERLAP: a seg clipped by solidsegs calls R_StoreWallRange once per FRAGMENT, and every fragment
   records under the same (seg<<2)|tier -- keying on the key alone hands back a sibling fragment's
   geometry, i.e. the wrong subtraction.  (Owner's review, 2026-08-03.) */
static const sat_leadq_t *sat_lead_look (int key, int back, int x0, int x1)
{
    int f, n, i, j;
    if (!sat_leadh[0]) return 0;
    f = (sat_leadh_cur - back + SAT_LEADH_DEPTH * 4) % SAT_LEADH_DEPTH;
    n = sat_leadh_n[f];
    for (i = 0 ; i < n ; i++)
    {
	const sat_leadq_t *q;
	j = sat_leadh_hint + i; if (j >= n) j -= n;
	q = &sat_leadh[f][j];
	if (q->key == (short)key && q->x1 <= x1 && q->x2 >= x0)
	    { sat_leadh_hint = j; return q; }
    }
    return 0;
}

/* Arm one edge pair from a recorded quad, evaluated from column x0. */
static void sat_lead_set (int *on, int *px1, int *px2, fixed_t *ylf, fixed_t *ylst,
			  fixed_t *yhf, fixed_t *yhst, const sat_leadq_t *q, int x0)
{
    int dx;
    if (!q) { *on = 0; return; }
    dx = q->x2 - q->x1;
    *px1 = q->x1; *px2 = q->x2;
    *ylst = dx > 0 ? (((fixed_t)(q->yl2 - q->yl1)) << FRACBITS) / dx : 0;
    *yhst = dx > 0 ? (((fixed_t)(q->yh2 - q->yh1)) << FRACBITS) / dx : 0;
    *ylf  = ((fixed_t)q->yl1 << FRACBITS) + *ylst * (x0 - q->x1);
    *yhf  = ((fixed_t)q->yh1 << FRACBITS) + *yhst * (x0 - q->x1);
    *on   = 1;
}

/* Arm L for the fragment [x0,x1].  No record (the wall was not on VDP1 then -- it just came into
   view, or handed over from the CPU) -> L->on = 0 and nothing extra is drawn: that case belongs to
   sat_wall_entry / the exit countdown, not here.

   JITTER GUARD (owner's review, 2026-08-03): VDP1's lag is not necessarily a constant, so no single
   X can be right if it oscillates.  Arm frames n-X **and** n-(X+1) and subtract only their
   INTERSECTION -- what BOTH covered.  A row VDP1 showed on one of the two frames but not the other
   is then redrawn, so either lag is covered, at the cost of a slightly wider difference.  Only one
   of the two recorded -> use it alone (the pair is a refinement, not a requirement). */
static void sat_lead_arm (sat_lead_t *L, int key, int x0, int x1)
{
    const sat_leadq_t *qa, *qb;
    L->on = L->on2 = 0;
    if (!sat_wall_lead_x || !sat_leadh[0]) return;
    qa = sat_lead_look (key, sat_wall_lead_x,     x0, x1);
    qb = sat_lead_look (key, sat_wall_lead_x + 1, x0, x1);
    if (!qa) { qa = qb; qb = 0; }
    sat_lead_set (&L->on,  &L->x1,  &L->x2,  &L->ylf,  &L->ylstep,  &L->yhf,  &L->yhstep,  qa, x0);
    sat_lead_set (&L->on2, &L->x1b, &L->x2b, &L->ylfb, &L->ylstepb, &L->yhfb, &L->yhstepb, qb, x0);
}

/* One difference span: straight to the framebuffer, or into the slave's list. */
#define SAT_LEAD_EMIT(a,b) do { 	if (sat_lead_mode == 1) sat_lead_span_add ((a), (b)); 	else { dc_yl = (a); dc_yh = (b); colfunc (); } 	sat_lead_cols++; } while (0)

/* Draw [yl,yh] MINUS the rows BOTH armed quads covered at column x.  dc_source / sat_wall_color /
   dc_texturemid are already set by the caller; this only chooses the spans. */
static void sat_lead_draw (sat_lead_t *L, int x, int yl, int yh)
{
    int cyl, cyh;
    if (yl > yh) return;
    if (x < L->x1 || x > L->x2) goto full;      /* the older quad did not reach this column */
    cyl = L->ylf >> FRACBITS;
    cyh = L->yhf >> FRACBITS;
    if (L->on2)                                 /* intersect with frame n-(X+1) */
    {
	int c2l, c2h;
	if (x < L->x1b || x > L->x2b) goto full;
	c2l = L->ylfb >> FRACBITS;
	c2h = L->yhfb >> FRACBITS;
	if (c2l > cyl) cyl = c2l;
	if (c2h < cyh) cyh = c2h;
    }
    if (cyh < cyl || cyh < yl || cyl > yh) goto full;   /* empty intersection, or misses these rows */
    if (cyl > yl) SAT_LEAD_EMIT (yl,      cyl - 1);   /* above */
    if (cyh < yh) SAT_LEAD_EMIT (cyh + 1, yh);        /* below */
    return;
full:
    SAT_LEAD_EMIT (yl, yh);
}

/* ===== PATH DWELL (owner 2026-08-05) =====================================================
   *"on devrait aussi limiter ces bascules pour eviter des changements trop frequents (ex:
   bloquer le mur pendant x frames sur le chemin bascule)"*.

   The hysteresis widened the THRESHOLD; this bounds the RATE.  After a seg changes path, it is
   pinned for `sat_wall_dwell` frames -- and pinned to the CPU, never to VDP1.  That asymmetry is
   deliberate and it is what makes this safe:
     - the software can draw any tier, so forcing CPU is never wrong, only slower;
     - forcing VDP1 could hit a tier with no VDP1 claim at all (magnified, no subdivision, bank
       full) and produce a tier drawn by NOBODY -- a hole, which is the bug we are chasing.
   So it is exactly the existing 2-frame exit overlap, generalised to X frames and made symmetric:
   whichever way the wall just flipped, the CPU keeps covering it for X frames.  Costs software
   columns -> watch row-2 `Bp`.  0 = off = the 2-frame overlap alone.

   State: bit7 = the last path seen (1 = CPU), bits 3:0 = the countdown.  Its own array because the
   per-seg byte's 4+2+2 bits are full; Z_Malloc'd into the Doom heap so the HWRAM pool pays nothing.
   Advanced ONCE PER FRAME (first visit), like every other per-seg countdown here. */
int sat_wall_dwell = 0;      /* frames a flipped seg stays covered by the CPU, 0..15 (pad R+Up) */
static unsigned char *sat_seg_dwell;

static int sat_dwell_cpu (int segidx, int cpu_now, int first_visit)
{
    unsigned char *d;
    if (!sat_wall_dwell || segidx < 0 || segidx >= SAT_SEG_MAX) return cpu_now;
    if (!sat_seg_dwell)
    {
	sat_seg_dwell = Z_Malloc (SAT_SEG_MAX, PU_STATIC, NULL);
	memset (sat_seg_dwell, 0, SAT_SEG_MAX);
    }
    d = &sat_seg_dwell[segidx];
    if (first_visit)
    {
	int was = (*d >> 7) & 1;
	if (was != (cpu_now & 1))                       /* a real flip -> arm the pin */
	    *d = (unsigned char)((cpu_now ? 0x80 : 0)
				 | (sat_wall_dwell > 15 ? 15 : sat_wall_dwell));
	else if (*d & 0x0f)
	    (*d)--;                                     /* count down (nibble > 0: no borrow into bit7) */
    }
    return (*d & 0x0f) ? 1 : cpu_now;                   /* pinned -> the CPU covers it */
}

#define SAT_SEG_EXIT(st)      ((*(st)) & 3)
#define SAT_SEG_EXIT_ARM(st)  (*(st) = (unsigned char)(((*(st)) & 0xfc) | 2))
#define SAT_SEG_EXIT_DEC(st)  (*(st) = (unsigned char)((*(st)) - 1))   /* guarded by EXIT() != 0 */

/* Fold this frame's visit into the seg byte.  Returns bit0 = the CPU must cover a wall that just
   came into view, bit1 = this was the FIRST visit this frame.  A seg clipped into several fragments
   calls R_StoreWallRange more than once per frame, so only the first visit advances the tag and the
   countdown -- the others just read it.

   ⚠ bit1 EXISTS BECAUSE THE EXIT COUNTDOWN HAD THE SAME MULTI-VISIT DEFECT AND NO GUARD (owner
   2026-08-03, seen through the wall-path paint: *"la disparition a l'air d'être sur une frame entre
   le texturé et le vert"* -- a wall drawn by NOBODY on the frame it moves CPU -> VDP1).  The exit
   overlap is the ONLY cover the door tiers have: unlike the one-sided mid tier, which gets a 96 px
   pre-warm band where both paths draw ([SPAN, V1], sat_wall_cpu_v1 = span + 96), `sat_v1_up/lo` key
   straight off `!cpu_up/!cpu_lo`, so VDP1 takes the tier the instant the CPU drops it.  The exit
   countdown was decremented once per VISIT, so a seg clipped into 2 fragments burned its 2 frames of
   overlap inside a SINGLE frame and the handoff went uncovered.  Now it decrements once per frame,
   like the entry countdown next to it, and the 2 frames mean 2 frames.  (The `sat_wall_entry` gate
   moved off the early return: the visit must be folded even at En0, or the tag stops advancing and
   every seg reads as "not visible last frame" forever.) */
static int sat_seg_entry_cover (unsigned char *st)
{
    unsigned char b, tag;
    int first = 0;
    if (!st) return 0;
    b   = *st;
    tag = (unsigned char)(sat_seg_frame & 15);
    if ((unsigned char)(b >> 4) != tag)                       /* first visit this frame */
    {
	int e = (b >> 2) & 3;
	int n = sat_wall_entry;
	if (n > SAT_SEG_ENTRY_MAX) n = SAT_SEG_ENTRY_MAX;
	if (((unsigned int)(tag - (b >> 4)) & 15u) != 1u) e = n;  /* gap != 1 -> not visible last frame */
	else if (e) e--;
	b = (unsigned char)((tag << 4) | (e << 2) | (b & 3));
	*st = b;
	first = 2;
    }
    return first | ((sat_wall_entry && (((b >> 2) & 3) != 0)) ? 1 : 0);
}

/* SATURN Phase-1 wall clamp ([[wall-clamp-world-anchored]]), below-floor side.  The failed 1b
   (owner's red/purple 2026-07-02) attached the quad bottom to floorclip = a SCREEN-anchored
   sloped edge -> squish + holes.  This is the WORLD-anchored version: cut the tier at a WHOLE
   texel row vcut -- its projection is a straight screen line (constant world height, linear in
   x, EXACT at both ends because scale steps linearly) -- chosen so the line stays above
   min(floorclip) over the whole span.  The VDP1 quad keeps its top corners and takes
   (e-1, vcut) as bottom (the -1 absorbs the platform's 1px generous pad -> the painted edge
   lands ON the line, never past floorclip).  The residual WEDGE between the line and the true
   per-column floorclip stays SOFTWARE (the column loop below, its top raised to the line):
   no hole, no bleed, a few rows of fill instead of the whole tier.
   Returns 1 = VDP1 emitted (wedge armed unless the tier was already full-software, e.g. the
   2-frame CPU-exit overlap, which must keep covering the WHOLE tier); 0 = no useful cut or
   bank full -> caller keeps the status-quo CPU fallback. */
static int sat_wall_cut_floor(fixed_t texmid, int v0, int yl1, int yl2,
                              int texture, int u1, int u2, const lighttable_t *cm,
                              int sw_already, fixed_t *w_ef, fixed_t *w_es, int *w_flag)
{
    int x, fcm = viewheight, ccm = -1;
    int n = rw_stopx - 1 - rw_x;
    fixed_t sc2 = rw_scale + rw_scalestep * n;
    unsigned int is1, is2;
    int ylim, vc1, vc2, vcut, e1, e2, tries;
    fixed_t dv = 0;
    if (sat_opt >= 2)                              /* SATURN L2: reuse the per-seg scan (identical range).
                                                      Reached only via cross_lo, which already forced it. */
	{ sat_wall_clip_need(); fcm = sat_clip_fcm; ccm = sat_clip_ccm; }
    else
	for (x = rw_x; x < rw_stopx; x++)
	{
	    if (floorclip[x]   < fcm) fcm = floorclip[x];
	    if (ceilingclip[x] > ccm) ccm = ceilingclip[x];
	}
    if (fcm <= 1) return 0;                        /* some column fully occluded -> not worth it */
    /* SUBSET invariant (owner's Ymir z-order report 2026-07-03): the software tier painted on
       NBG1 (above ALL VDP1) and per-column clipped, so it could never lose to a farther wall.
       The VDP1 piece lives in painter order, and its UNCUT side keeps the RAW edge -- checked
       against the clip at the END columns only by the claim chain.  An INTERIOR column with a
       tighter ceilingclip (arch lintel, stair profile of a closer seg) would let a later
       (farther) quad overpaint us -- "les murs de derriere devant".  Bound BOTH sides over the
       WHOLE span (+1 row of margin for the platform's 1px pad): the piece is then a strict
       SUBSET of the pixels the software fallback used to paint -> z-safe by construction. */
    if (yl1 <= ccm + 1 || yl2 <= ccm + 1) return 0;
    ylim = fcm - 1;                                /* deepest row the VDP1 piece may touch */
    if (sc2 <= 0) return 0;
    is1 = 0xffffffffu / (unsigned)rw_scale;        /* same reciprocal form as SAT_VROWS */
    is2 = 0xffffffffu / (unsigned)sc2;
    vc1 = (int)((texmid + (fixed_t)((ylim - centery) * (int)is1)) >> FRACBITS);
    vc2 = (int)((texmid + (fixed_t)((ylim - centery) * (int)is2)) >> FRACBITS);
    vcut = vc1 < vc2 ? vc1 : vc2;                  /* floor-round + min: line above ylim at BOTH ends */
    e1 = e2 = 0;
    for (tries = 0; tries < 3; tries++)            /* whole-texel granularity: back off if rounding overshoots */
    {
	if (vcut <= v0) return 0;                  /* cut at/above the tier top -> nothing useful on VDP1 */
	dv = ((fixed_t)vcut << FRACBITS) - texmid;
	e1 = centery + (int)(FixedMul(dv, rw_scale) >> FRACBITS);
	e2 = centery + (int)(FixedMul(dv, sc2)      >> FRACBITS);
	if (e1 <= ylim && e2 <= ylim) break;
	vcut--;
    }
    if (tries == 3) return 0;
    if (e1 - 1 < yl1 || e2 - 1 < yl2) return 0;    /* degenerate: the cut crosses the tier top */
    if (sat_wall_hook (rw_x, yl1, e1 - 1, rw_stopx - 1, yl2, e2 - 1,
                       texture, u1, u2, v0, vcut, cm))
	return 0;                                  /* bank full -> caller falls back to full CPU */
    if (!sw_already)
    {
	/* wedge edge(x) in the loop's HEIGHTBITS domain, 1 row above the line for a guaranteed
	   overlap with the VDP1 piece's padded/interpolated bottom (gap impossible, overlap
	   harmless: same texture, and VDP1 wins the composite over NBG1 anyway). */
	*w_ef = (centeryfrac >> 4) + (FixedMul(dv, rw_scale) >> 4) - (1 << HEIGHTBITS);
	*w_es = FixedMul(dv, rw_scalestep) >> 4;
	*w_flag = 1;
    }
    sat_fb_wclamp_t++;
    return 1;
}

/* Above-ceiling mirror (deported VDP1 ceilings, sat_vdp1_floor): cut the tier TOP at the
   whole-texel line kept below max(ceilingclip); the wedge (rows above the line down from
   ceilingclip+1) stays software.  Same guarantees as the floor side, all roundings mirrored
   (ceil-round vcut up, verify the line sits at/below ylim, +1 pad absorption). */
static int sat_wall_cut_ceil(fixed_t texmid, int v1, int yh1, int yh2,
                             int texture, int u1, int u2, const lighttable_t *cm,
                             int sw_already, fixed_t *w_ef, fixed_t *w_es, int *w_flag)
{
    int x, ccm = -1, fcm = viewheight;
    int n = rw_stopx - 1 - rw_x;
    fixed_t sc2 = rw_scale + rw_scalestep * n;
    unsigned int is1, is2;
    int ylim, vc1, vc2, vcut, e1, e2, tries;
    fixed_t dv = 0;
    for (x = rw_x; x < rw_stopx; x++)
    {
	if (ceilingclip[x] > ccm) ccm = ceilingclip[x];
	if (floorclip[x]   < fcm) fcm = floorclip[x];
    }
    if (ccm >= viewheight - 2) return 0;           /* some column fully occluded -> not worth it */
    /* SUBSET invariant, mirrored (see sat_wall_cut_floor): bound the UNCUT bottom against the
       tightest interior floorclip too, so the piece never paints pixels the software tier
       would not have -- a later quad can then never (newly) overpaint a nearer wall. */
    if (yh1 >= fcm - 1 || yh2 >= fcm - 1) return 0;
    ylim = ccm + 1;                                /* highest row the VDP1 piece may touch */
    if (sc2 <= 0) return 0;
    is1 = 0xffffffffu / (unsigned)rw_scale;
    is2 = 0xffffffffu / (unsigned)sc2;
    vc1 = (int)((texmid + (fixed_t)((ylim - centery) * (int)is1) + 0xFFFF) >> FRACBITS);
    vc2 = (int)((texmid + (fixed_t)((ylim - centery) * (int)is2) + 0xFFFF) >> FRACBITS);
    vcut = vc1 > vc2 ? vc1 : vc2;                  /* ceil-round + max: line below ylim at BOTH ends */
    e1 = e2 = 0;
    for (tries = 0; tries < 3; tries++)
    {
	if (vcut >= v1) return 0;                  /* cut at/below the tier bottom -> nothing useful */
	dv = ((fixed_t)vcut << FRACBITS) - texmid;
	e1 = centery + (int)(FixedMul(dv, rw_scale) >> FRACBITS);
	e2 = centery + (int)(FixedMul(dv, sc2)      >> FRACBITS);
	if (e1 >= ylim && e2 >= ylim) break;
	vcut++;
    }
    if (tries == 3) return 0;
    if (e1 + 1 > yh1 || e2 + 1 > yh2) return 0;    /* degenerate: the cut crosses the tier bottom */
    if (sat_wall_hook (rw_x, e1 + 1, yh1, rw_stopx - 1, e2 + 1, yh2,
                       texture, u1, u2, vcut, v1, cm))
	return 0;
    if (!sw_already)
    {
	*w_ef = (centeryfrac >> 4) + (FixedMul(dv, rw_scale) >> 4) + (1 << HEIGHTBITS);
	*w_es = FixedMul(dv, rw_scalestep) >> 4;
	*w_flag = 2;
    }
    sat_fb_wclamp_t++;
    return 1;
}

/* Does the tier's linear BOTTOM edge cross floorclip ANYWHERE in the span?  The END columns
   catch the common case (and are the whole pre-clamp test); the INTERIOR scan -- active only
   under sat_wall_clamp, so clamp-off stays byte-identical -- catches the pedestal/stair
   profile: a wall whose bottom is visible at BOTH ends but occluded mid-span used to emit a
   FULL quad painting below the interior floorclip.  Invisible while its victims were software
   (NBG1 above all VDP1), it became "les murs de derriere devant" (owner Ymir 2026-07-03) once
   the clamp made the victims VDP1: the W bank paints near->far, later = farther WINS every
   overlap.  Near-first is a deliberate law (an overrunning plot must cut the FARTHEST walls,
   dg_saturn flush comment) -- so fix the overlap at the source: route these walls through the
   same cut+wedge.  The edge is linear -> incremental 12-bit frac, adds+compare per column. */
static int sat_wall_cross_lo(int yh1, int yh2)
{
    int x, n = rw_stopx - 1 - rw_x;
    fixed_t f, s;
    if (yh1 >= floorclip[rw_x] || yh2 >= floorclip[rw_stopx - 1]) return 1;
    if (!sat_wall_clamp || n <= 1) return 0;
    /* SATURN L2: yh(x) interpolates yh1..yh2 linearly and the >>12 (arithmetic, = floor) only
       LOWERS it, so every column obeys yh(x) <= max(yh1,yh2).  If that bound is already under the
       seg's minimum floorclip, no column can cross -- O(1), and it also skips the divide below.
       EXACT (same predicate, not an approximation). */
    if (sat_opt >= 2)
    {
	int m = yh1 > yh2 ? yh1 : yh2;
	sat_wall_clip_need();          /* we were about to sweep anyway -> sweep the CHEAP way, once */
	if (m < sat_clip_fcm) return 0;
    }
    f = yh1 << 12; s = ((yh2 - yh1) << 12) / n;
    for (x = rw_x; x < rw_stopx; x++)
    {
	if ((int)(f >> 12) >= floorclip[x]) return 1;
	f += s;
    }
    return 0;
}
/* Mirror: does the tier's linear TOP edge cross ceilingclip anywhere in the span? */
static int sat_wall_cross_hi(int yl1, int yl2)
{
    int x, n = rw_stopx - 1 - rw_x;
    fixed_t f, s;
    if (yl1 <= ceilingclip[rw_x] || yl2 <= ceilingclip[rw_stopx - 1]) return 1;
    if (!sat_wall_clamp || n <= 1) return 0;
    f = yl1 << 12; s = ((yl2 - yl1) << 12) / n;
    for (x = rw_x; x < rw_stopx; x++)
    {
	if ((int)(f >> 12) <= ceilingclip[x]) return 1;
	f += s;
    }
    return 0;
}
/* VISIBILITY audit of a tier over its whole span, BOTH clip arrays combined per column
   (owner Ymir 2026-07-03, the outer-border question): a tier whose visible band
   [max(yl,cc+1) .. min(yh,fc-1)] is EMPTY at EVERY column -- the level border behind an
   upstairs window: lintel+sill close the band, the software renderer draws 0 px of it --
   still passed both END-column tests and emitted a FULL quad: wasted commands AND, in the
   near-first painter, a far wall painted over everything.  Returns 1 = some pixel visible
   somewhere (claim normally), 0 = invisible everywhere (claim NOTHING, exactly like the
   software).  Linear edges -> one incremental scan. */
static int sat_wall_span_visible(int yl1, int yl2, int yh1, int yh2)
{
    int x, n = rw_stopx - 1 - rw_x;
    fixed_t fl, fh, sl, sh;
    fl = yl1 << 12; sl = n > 0 ? (fixed_t)((yl2 - yl1) << 12) / n : 0;
    fh = yh1 << 12; sh = n > 0 ? (fixed_t)((yh2 - yh1) << 12) / n : 0;
    for (x = rw_x; x < rw_stopx; x++)
    {
	int a = (int)(fl >> 12), b = (int)(fh >> 12);
	if (b > floorclip[x] - 1)   b = floorclip[x] - 1;
	if (a < ceilingclip[x] + 1) a = ceilingclip[x] + 1;
	if (a <= b) return 1;
	fl += sl; fh += sh;
    }
    return 0;
}

/* SATURN L5: try to render this tier as CPU-BORDERS + VDP1-CORE instead of a full software wall.
   Returns 1 when armed (sat_we_* set) -- the caller must then clear its sat_sw_* tier flag so the
   column loop draws ONLY the border columns, via is_edge.  Returns 0 = the caller keeps the
   untouched full-CPU wall, so every bail-out here is a no-op, never a regression.
   need_floor_clear: the caller is the floor-CROSSING path.  The RBG0 floor is transparent (index 0),
   so a VDP1 quad dipping under floorclip would bleed through it -- prove the whole interior stays
   above the seg's minimum floorclip (the same O(1) argument as L2) before emitting. */
static int sat_wall_try_edge(int texture, int yl1, int yh1, int yl2, int yh2,
                             int u1, int u2, int v0, int v1, const lighttable_t *cm,
                             int need_floor_clear)
{
    int tw, xL, xR, uL, uR, why = 0, dL, dR, ylL, yhL, ylR, yhR;
    if (!sat_wall_edge_hook || !sat_wall_hook || texture <= 0) return 0;
    tw = texturewidthmask[texture] + 1;
    if (tw <= 1) return 0;
    if (!sat_wall_edge_hook(rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2, u1, u2, tw,
                            &xL, &xR, &uL, &uR, &why))
	{ if (why >= 1 && why <= 2) sat_fb_edge_b[why - 1]++; return 0; }
    if (xL < rw_x)         xL = rw_x;
    if (xR > rw_stopx - 1) xR = rw_stopx - 1;
    if (xR - xL < SAT_WALL_EDGE_MIN) { sat_fb_edge_b[2]++; return 0; }
    dL = xL - rw_x; dR = xR - rw_x;
    ylL = (topfrac    + topstep    * dL + HEIGHTUNIT - 1) >> HEIGHTBITS;
    yhL = (bottomfrac + bottomstep * dL)                  >> HEIGHTBITS;
    ylR = (topfrac    + topstep    * dR + HEIGHTUNIT - 1) >> HEIGHTBITS;
    yhR = (bottomfrac + bottomstep * dR)                  >> HEIGHTBITS;
    if (need_floor_clear)
    {
	int m = yhL > yhR ? yhL : yhR;
	sat_wall_clip_need();
	if (m >= sat_clip_fcm) { sat_fb_edge_b[3]++; return 0; }  /* may dip under a nearer floor -> CPU */
    }
    if (sat_wall_hook(xL, ylL, yhL, xR, ylR, yhR, texture, uL, uR, v0, v1, cm))
	{ sat_fb_edge_b[3]++; return 0; }     /* VDP1 bank full */
    sat_we_on = 1; sat_we_lo = xL; sat_we_hi = xR;
    sat_fb_edge_t++;
    return 1;
}

void R_RenderSegLoop (void)
{
    angle_t		angle;
    unsigned		index;
    int			yl;
    int			yh;
    int			mid;
    fixed_t		texturecolumn;
    int			top;
    int			bottom;
    int			wall_solid;
    int			sw_draws = 0;   /* SATURN PERF (lever C): does software draw a tier this seg? */
    /* SATURN per-tier draw gates: sat_sw_* = the software draws this tier (CPU + transition zone);
       sat_v1_* = the VDP1 hook draws it (VDP1 + transition zone).  Both true in [LOW,HIGH] = overlap. */
    int			sat_sw_mid = 0, sat_sw_up = 0, sat_sw_lo = 0;
    /* SATURN: seg index at FUNCTION scope -- the lead-fill keys its per-tier quad history on it and
       needs it at the emit sites AND in the column loop, not just inside the claim block. */
    int			segidx0 = (int)(curline - segs);
    int			sat_v1_mid = 0, sat_v1_up = 0, sat_v1_lo = 0;
    int			sat_v1_mid_sub = 0, sat_v1_up_sub = 0, sat_v1_lo_sub = 0;   /* magnified tier -> perspective-subdivide on VDP1 (not CPU) */
    int			sat_v1_mid_edge = 0;   /* SATURN L5: try CPU-borders + VDP1-core instead of a full CPU wall */
    /* SATURN Phase-1 wall clamp: per-tier residual-WEDGE state.  0 = off (software draws the
       full tier when sat_sw_* is set); 1 = below-floor cut, software draws only rows >= edge(x);
       2 = above-ceiling cut, software draws only rows <= edge(x).  edge(x) steps linearly per
       column exactly like bottomfrac (HEIGHTBITS domain), armed by sat_wall_cut_floor/_ceil. */
    int			sat_wcl_mid = 0, sat_wcl_up = 0, sat_wcl_lo = 0;
    fixed_t		sat_wcl_mid_ef = 0, sat_wcl_mid_es = 0;
    fixed_t		sat_wcl_up_ef = 0,  sat_wcl_up_es = 0;
    fixed_t		sat_wcl_lo_ef = 0,  sat_wcl_lo_es = 0;
    /* SATURN L3: 0xffffffff/rw_scale, hoisted.  rw_scale does NOT change during the tier-claim
       phase below (it is only stepped inside the column loop, which runs after), so every
       SAT_VROWS in this seg divides the SAME operand.  0 = not computed -> fall back. */
    unsigned int	sat_is0 = 0;

    /* Keep doors/switches (special lines) textured even in Potato walls, so they
       stay readable against the flat-shaded plain walls. */
    /* DEBUG PAINT bit1 (r_data.c sat_wall_paint): paint EVERY CPU wall, doors and switches too --
       the point is to see which path owns each wall, and an exception would read as a hole. */
    sat_wall_textured = (sat_wall_paint & 2) ? 0 : (curline->linedef->special != 0);
    /* SATURN PERF (step 2): a plain opaque wall in Potato mode is drawn as one
       solid colour by rp_exec_col (it reads cm->f3 + cm->cmap, NEVER cm->src) ->
       skip R_GetColumn (the memory-bound texture composite = the bulk of wall-prep
       "Bp") and the per-column dc_iscale division.  wall_solid matches the
       executor's solid test exactly (cm->unused = in_masked||sat_wall_textured,
       and in_masked is 0 during opaque wall generation). */
    /* The `&& !rp_disabled` this test used to carry was a BUG, not a guard: it said "flat walls only
       when the parity executors run", and rp_disabled is 1 in the SHIPPING config (main.cxx
       sat_plane_parallel -> r_main.c), so every flat-wall mode was dead.  r_draw.c now implements
       the solid column on the master path too, which is the one that actually runs. */
    wall_solid = (sat_potato_walls || (sat_wall_paint & 2))
	         && !sat_wall_textured;
    sat_clip_have = 0;   /* SATURN L2: per-SEG validity -- never inherit the previous seg's scan */
    /* LEAD-FILL: same rule.  The arm sites live inside the VDP1 emit blocks, which are gated, so
       without this a tier VDP1 does not own would inherit the previous seg's old quad. */
    sat_lead_mid.on = sat_lead_up.on = sat_lead_lo.on = 0;
    sat_lead_mid.on2 = sat_lead_up.on2 = sat_lead_lo.on2 = 0;
    sat_we_on     = 0;   /* SATURN L5: edge-split disarmed until this seg's claim block arms it */

    /* texture ROW (v) at a wall's top/bottom screen y, so the platform maps the right
       vertical SUBRANGE of the texture (charAddr/height) instead of stretching the whole
       texture onto the band (the "vertical squish").  ~constant across the seg, so compute
       it at x1 (rw_scale = scale1 here). */
#define SAT_VROWS(texmid, ytop, ybot, v0o, v1o) do { \
	unsigned int _is = (sat_opt >= 3 && sat_is0) ? sat_is0 : (0xffffffffu / (unsigned int)rw_scale); \
	(v0o) = (int)(((texmid) + (fixed_t)(((ytop) - centery) * (int)_is)) >> FRACBITS); \
	(v1o) = (int)(((texmid) + (fixed_t)(((ybot) - centery) * (int)_is)) >> FRACBITS); \
	} while (0)

    /* SATURN: per tier, gate the software draw (sat_sw_*) and the VDP1 hook (sat_v1_*).  A tier goes
       to SOFTWARE when it is too close for VDP1 -- vertical SPAN explosion (span > SPAN, tall walls)
       OR horizontal MAGNIFICATION (close/face-on, catches short DOOR bands the span test misses).
       Both one-sided mid and two-sided upper/lower get the per-seg 3-frame exit handoff (sat_seg_cpu)
       to cover the VDP1's multi-frame lag when a wall hands back to VDP1.  Else VDP1 owns it. */
    if (sat_wall_hook && SAT_WALL_VDP1_OK && sat_wall_skip && rw_stopx > rw_x)
    {
	int n = rw_stopx - 1 - rw_x;
	/* SATURN L3: hoist the SAT_VROWS reciprocal -- rw_scale is constant for the whole claim
	   phase, so every tier (and every sub-segment) below divides the SAME operand.
	   (L2's clip scan is deliberately NOT done here: it is lazy, see sat_wall_clip_need.) */
	if (sat_opt >= 3 && rw_scale > 0) sat_is0 = 0xffffffffu / (unsigned int)rw_scale;
	/* MAGNIFICATION = screen px per texel of u (du = the seg's tex u-span over its visible columns,
	   tier-independent).  HIGH = close/face-on -> the VDP1 world-anchored tiling extrapolates past
	   the screen edge and the platform squishes ("ecrasement", worst on DOORS) -> render in CPU.
	   Grazing walls have a huge du -> LOW mag -> stay on VDP1 (cheap). */
	int sx  = rw_stopx - rw_x;
	int ma1 = (rw_centerangle + xtoviewangle[rw_x])        >> ANGLETOFINESHIFT;
	int ma2 = (rw_centerangle + xtoviewangle[rw_stopx - 1]) >> ANGLETOFINESHIFT;
	int mdu = ((rw_offset - FixedMul(finetangent[ma1], rw_distance)) >> FRACBITS)
		- ((rw_offset - FixedMul(finetangent[ma2], rw_distance)) >> FRACBITS);
	int magnified;
	/* Per-seg CPU memory, hoisted up here because BOTH terms of cpu_now need it: the exit countdown
	   is still armed <=> this seg was on the CPU within the last 2 frames. */
	int segidx = (int)(curline - segs);
	unsigned char *segst = (segidx >= 0 && segidx < SAT_SEG_MAX) ? &sat_seg_cpu[segidx] : 0;
	int seg_hyst = (segst && SAT_SEG_EXIT(segst)) ? 1 : 0;
	if (mdu < 0) mdu = -mdu;
	if (mdu < 1) mdu = 1;
	/* HYSTERESIS ON MAGNIFICATION.  The span threshold got one first and the owner still saw the
	   alternation -- because cpu_now is `span_close || magnified` and this is the OTHER term, the
	   one that moves when you walk AT a wall, i.e. exactly his "avant / arriere" case.  Quarter
	   steps so the band is half a MAG unit (~17%), enough that ordinary walking cannot dither
	   across it, small enough never to hold a genuinely magnified wall on VDP1 (where it squishes). */
	magnified = (sx * 4 > mdu * (SAT_WALL_CPU_MAG * 4 - (seg_hyst ? 2 : 0)));

	if (midtexture && !curline->backsector)
	{
	    int s1 = (bottomfrac - topfrac) >> HEIGHTBITS;
	    int s2 = ((bottomfrac + bottomstep * n) - (topfrac + topstep * n)) >> HEIGHTBITS;
	    int s = s1 > s2 ? s1 : s2;
	    unsigned char *st = segst;
	    /* HYSTERESIS on the CPU<->VDP1 span threshold (owner 2026-08-03, read off the wall-path
	       paint: *"c'est sur une alternance : le mur ne reste pas vert.  On passe de texture - vert
	       - texture"*).  The wall was not migrating, it was OSCILLATING: `s` jitters either side of
	       a bare threshold as the player moves, so the tier flips path every frame or two, and each
	       flip is a handoff that has to be covered.  Hysteresis makes a wall that was recently on
	       the CPU keep a LOWER bar to stay there, so `s` has to move a real distance to flip -- no
	       flip, no handoff, no hole, and the texture stops strobing against the flat quad either
	       way.  No new state: the exit countdown already means "this seg was on the CPU within the
	       last 2 frames", which is exactly the memory hysteresis needs. */
	    int span_close = (s > sat_wall_cpu_span - (seg_hyst ? SAT_WALL_HYST : 0));
	    /* SPAN clamp DISABLED (v0 near-wall affine perspective warp = "moche", owner 2026-07-02):
	       span-close one-sided walls stay on the CPU (shipping).  sat_wall_clamp now drives ONLY the
	       BELOW-FLOOR cut (Phase 1b).  Revisit SPAN only with finer near-tile u-subdivision. */
	    int cpu_now = span_close || magnified;
	    if (!cpu_now && st && SAT_SEG_EXIT(st) == 2) sat_wall_flip++;   /* CPU last frame, VDP1 now */
	    int entry;
	    sat_v1_mid = (s < sat_wall_cpu_v1) && !magnified;  /* magnified -> NO VDP1 quad (it squishes) */
#if SAT_WALL_SUBDIV
	    if (magnified && !span_close) { sat_v1_mid_sub = 1; cpu_now = 0; }  /* keep on VDP1 via perspective subdivision (emit site), not CPU */
#endif
	    entry = sat_seg_entry_cover(st);   /* just came into view -> the CPU covers VDP1's first frame */
	    cpu_now = sat_dwell_cpu(segidx, cpu_now, entry & 2);   /* bound the FLIP RATE, see sat_wall_dwell */
	    if (cpu_now)
	    {
		if (!sat_v1_mid) {   /* Phase-0: count only the FULLY-CPU tiers (not the [SPAN,V1] VDP1-also pre-warm) */
		    if (magnified)                  sat_fb_mag_t++;                             /* squish -> clamp can't fix   */
		    else if (span_close)            { sat_fb_clamp_t++; sat_fb_px += s * sx; }  /* pure span  -> clampable     */
		}
		sat_sw_mid = 1;  if (st) SAT_SEG_EXIT_ARM(st);           /* CPU draws (close/magnified); arm 2 CPU exit-frames */
		    /* SATURN L5: this tier is about to cost a FULL-SCREEN software wall (the ~22ms
		       nose-to-wall Bp).  Let the emit site below try the CPU-borders/VDP1-core split
		       first; it silently leaves everything as-is when the interior is too thin. */
		    if (sat_opt >= 5) { sat_v1_mid_edge = 1; sat_fb_edge_w++; }
	    }
	    else
	    {
		sat_sw_mid = ((st && SAT_SEG_EXIT(st)) || (entry & 1)) ? 1 : 0;   /* CPU also draws 2 frames after exit, and the first frames after entry */
		if (st && SAT_SEG_EXIT(st) && (entry & 2)) SAT_SEG_EXIT_DEC(st);  /* ONCE PER FRAME (entry&2 = first visit), not per fragment */
	    }
	    /* ORPHAN COUNT -- see sat_wall_nodraw. */
	    if (!sat_sw_mid && !sat_v1_mid && !sat_v1_mid_sub) sat_wall_nodraw++;
	    {
	    }
	}
	else if (curline->backsector)
	{
	    /* doors: upper/lower are SHORT bands -> the span test never trips them even up close, but
	       they squish at the edge when magnified.  Route the whole seg (both tiers) to CPU on span
	       OR magnification, with the same per-seg 3-frame exit handoff as the one-sided mid. */
	    int cpu_up = 0, cpu_lo = 0;
	    unsigned char *dst = segst;
	    /* Same HYSTERESIS as the one-sided mid tier above, and these tiers need it MORE: they have
	       no [SPAN,V1] pre-warm band, so every flip is a bare handoff.  See the note there. */
	    int dhy = seg_hyst ? SAT_WALL_HYST : 0;
	    if (toptexture)
	    {
		int s1 = (pixhigh - topfrac) >> HEIGHTBITS;
		int s2 = ((pixhigh + pixhighstep * n) - (topfrac + topstep * n)) >> HEIGHTBITS;
		int s = s1 > s2 ? s1 : s2;
		cpu_up = (s > sat_wall_cpu_span - dhy);
	    }
	    if (bottomtexture)
	    {
		int s1 = (bottomfrac - pixlow) >> HEIGHTBITS;
		int s2 = ((bottomfrac + bottomstep * n) - (pixlow + pixlowstep * n)) >> HEIGHTBITS;
		int s = s1 > s2 ? s1 : s2;
		cpu_lo = (s > sat_wall_cpu_span - dhy);
	    }
	    {
		int cpu_now = cpu_up || cpu_lo || magnified;
		if (!cpu_now && dst && SAT_SEG_EXIT(dst) == 2) sat_wall_flip++;   /* CPU last frame, VDP1 now */
		if (cpu_up) sat_fb_clamp_t++;                        /* Phase-0: clampable span (upper door tier) */
		if (cpu_lo) sat_fb_clamp_t++;                        /* Phase-0: clampable span (lower door tier) */
#if !SAT_WALL_SUBDIV
		if (magnified && !cpu_up && !cpu_lo) sat_fb_mag_t++; /* Phase-0: magnified-only door residue      */
#endif
		unsigned char *st = dst;
		int vis     = sat_seg_entry_cover(st);  /* bit0 = just came into view, bit1 = first visit this frame */
		cpu_now = sat_dwell_cpu(segidx, cpu_now, vis & 2);      /* bound the FLIP RATE */
		int overlap = vis & 1;                  /* -> CPU covers VDP1's first frame */
		if (cpu_now) { if (st) SAT_SEG_EXIT_ARM(st); }                    /* arm 2 CPU exit-frames */
		else if (st && SAT_SEG_EXIT(st))
		{   /* CPU also draws 2 frames after exit.  These tiers have NO pre-warm band (sat_v1_up/lo
		       key straight off !cpu_up/!cpu_lo), so this overlap is their only handoff cover --
		       decrement it ONCE PER FRAME, never once per clipped fragment. */
		    overlap = 1;
		    if (vis & 2) SAT_SEG_EXIT_DEC(st);
		}
		/* VDP1 owns a tier only when it is neither magnified nor span-close (so it never draws the
		   squishing quad); the CPU draws it when close/magnified OR during the exit overlap. */
		sat_v1_up = (toptexture    && !magnified && !cpu_up) ? 1 : 0;
		sat_v1_lo = (bottomtexture && !magnified && !cpu_lo) ? 1 : 0;
#if SAT_WALL_SUBDIV
		/* magnified (not span-close) door tiers -> perspective-subdivide on VDP1 (emit site), not CPU */
		sat_v1_up_sub = (toptexture    && magnified && !cpu_up) ? 1 : 0;
		sat_v1_lo_sub = (bottomtexture && magnified && !cpu_lo) ? 1 : 0;
		sat_sw_up = (toptexture    && (cpu_up || (magnified && !sat_v1_up_sub) || overlap)) ? 1 : 0;
		sat_sw_lo = (bottomtexture && (cpu_lo || (magnified && !sat_v1_lo_sub) || overlap)) ? 1 : 0;
#else
		sat_sw_up = (toptexture    && (cpu_up || magnified || overlap)) ? 1 : 0;
		sat_sw_lo = (bottomtexture && (cpu_lo || magnified || overlap)) ? 1 : 0;
#endif
		/* ORPHAN COUNT -- see sat_wall_nodraw. */
		if (toptexture    && !sat_sw_up && !sat_v1_up && !sat_v1_up_sub) sat_wall_nodraw++;
		if (bottomtexture && !sat_sw_lo && !sat_v1_lo && !sat_v1_lo_sub) sat_wall_nodraw++;
	    }
	}
    }

    /* SATURN: pot2 (banded/flat) -- a plain (non-special) wall draws as a VDP1 quad, so the close-wall
       CPU fallback above is skipped.  FLAT (pot2-fl) clamps -> can't swim; BANDED (pot2-bd) CAN swim/
       squish on close walls, but in the tiny split windows that's accepted (Romain) for the master Bp
       win.  Force VDP1, no software, for every tier.  SPECIAL walls (sat_wall_textured) stay TEXTURED
       and keep their own fallback. */
    if (sat_wall_nocpu && !sat_wall_textured && SAT_WALL_VDP1_OK && sat_wall_skip && rw_stopx > rw_x)
    {
	sat_sw_mid = sat_sw_up = sat_sw_lo = 0;
	if (midtexture && !curline->backsector) sat_v1_mid = 1;
	if (curline->backsector)
	{
	    sat_v1_up = toptexture    ? 1 : 0;
	    sat_v1_lo = bottomtexture ? 1 : 0;
	}
    }

    /* SATURN VDP1 world renderer (Step 2): one-sided (solid) walls -> the platform as
       a quad.  The 4 screen corners come from the same topfrac/bottomfrac the loop
       below steps; midtexture = the full-height wall texture. */
    if (sat_wall_hook && SAT_WALL_VDP1_OK && midtexture && !curline->backsector && rw_stopx > rw_x && (sat_v1_mid || sat_v1_mid_sub || sat_v1_mid_edge))
    {
	int n   = rw_stopx - 1 - rw_x;
	int yl1 = (topfrac + HEIGHTUNIT - 1) >> HEIGHTBITS;
	int yh1 = bottomfrac >> HEIGHTBITS;
	int yl2 = (topfrac + topstep * n + HEIGHTUNIT - 1) >> HEIGHTBITS;
	int yh2 = (bottomfrac + bottomstep * n) >> HEIGHTBITS;
	/* texture u at the two ends (same perspective formula as the loop below) */
	int a1 = (rw_centerangle + xtoviewangle[rw_x])        >> ANGLETOFINESHIFT;
	int a2 = (rw_centerangle + xtoviewangle[rw_stopx - 1]) >> ANGLETOFINESHIFT;
	int u1 = (rw_offset - FixedMul(finetangent[a1], rw_distance)) >> FRACBITS;
	int u2 = (rw_offset - FixedMul(finetangent[a2], rw_distance)) >> FRACBITS;
	int v0, v1; SAT_VROWS(rw_midtexturemid, yl1, yh1, v0, v1);
	/* LEAD-FILL: this tier's quad as it was X frames ago (-> the software draws the difference in
	   the column loop), then this frame's for the next lookup.  Recorded from the tier extent, not
	   per sub-quad: the subdivided/edge emitters cover exactly the same area. */
	sat_lead_arm (&sat_lead_mid, (segidx0 << 2) | 0, rw_x, rw_stopx - 1);
	/* distance-correct light = the colormap the software loop picks (was a FIXED mid-level,
	   so VDP1 walls did not match the room's per-distance lighting). */
	int _li = rw_scale >> LIGHTSCALESHIFT;
	if (_li >= MAXLIGHTSCALE) _li = MAXLIGHTSCALE - 1; else if (_li < 0) _li = 0;
	const lighttable_t *cm = walllights[_li];
	/* SATURN: the RBG0 floor is transparent (index 0), so a VDP1 wall quad reaching below
	   the floor line bleeds through it.  Three cases vs the floor line (RBG0 floor on):
	   - ENTIRELY below (top at/below floorclip at BOTH ends): fully occluded -> cull.
	   - PARTIALLY below (bottom dips past floorclip at either end): VDP1 can't clip the quad
	     -> hand the tier to the SOFTWARE renderer, which clips each column to floorclip (no
	     texture squish).  sat_sw_mid forces the column loop + sw_draws below.
	   - fully above: VDP1 as usual. */
	if (sat_wall_clamp && !sat_wall_span_visible(yl1, yl2, yh1, yh2))
	    { /* no visible pixel at ANY column (empty window band / full occlusion profile): the
	         software draws 0 px of it -- claim NOTHING (the old full-quad claim painted it
	         over nearer walls in the near-first painter; owner's outer-border capture). */ }
	else if (sat_floor_punch_here() && yl1 >= floorclip[rw_x] && yl2 >= floorclip[rw_stopx - 1])
	    { if (sat_wall_clamp) sat_sw_mid = 1; /* SATURN: reached only when 711 passed => span_visible said VISIBLE, so this end-only "below floor" is a pedestal false-positive -> draw in SOFTWARE (per-column floorclip clips the truly-below columns; no bleed).  clamp off: cull as before. */ }
	else if (sat_floor_punch_here() && sat_wall_cross_lo(yh1, yh2))
	{
	    /* Occluded below a NEARER floor somewhere in the span (ends OR interior -- the pedestal
	       profile).  Phase-1 clamp (sat_wall_clamp): cut the quad at a whole-texel WORLD-anchored
	       line above min(floorclip) + software wedge below it (sat_wall_cut_floor above; the
	       failed screen-anchored 1b is its header comment).  Not for magnified tiers
	       (!sat_v1_mid: the cut can't fix the horizontal squish) nor when the tier ALSO crosses
	       a deported ceiling (both-sides cut = two wedges; rare -> keep full CPU).
	       Clamp off / no useful cut / bank full -> the status-quo full-software fallback. */
	    if (!(sat_wall_clamp && sat_v1_mid
	          && !(sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
	          && sat_wall_cut_floor(rw_midtexturemid, v0, yl1, yl2, midtexture, u1, u2, cm,
	                                sat_sw_mid, &sat_wcl_mid_ef, &sat_wcl_mid_es, &sat_wcl_mid)))
	        { sat_fb_clamp_t++; sat_fb_px += (yh1 - yl1) * (rw_stopx - rw_x); }
	    sat_sw_mid = 1;   /* full tier on a failed clamp; only the WEDGE rows when sat_wcl_mid is armed */
	    /* SATURN L5 COMPOSITION: the vertical cut above is gated on !magnified, so a MAGNIFIED wall
	       that ALSO crosses the floor line used to fall straight through to a full software wall --
	       the case the owner rightly called out.  The lateral split is ORTHOGONAL to the vertical
	       cut, so try it here too; the floor-clearance proof inside keeps any VDP1 pixel from
	       bleeding through the transparent RBG0 floor.  Skipped when a wedge is already armed
	       (that regime has its own software rows and its own z-contract). */
	    if (sat_v1_mid_edge && !sat_wcl_mid
	        && sat_wall_try_edge(midtexture, yl1, yh1, yl2, yh2, u1, u2, v0, v1, cm, 1))
		sat_sw_mid = 0;
	}
	else if (sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
	{
	    /* Occluded ABOVE a NEARER/lower ceiling (yl <= ceilingclip): a raw VDP1 quad drawn to its
	       natural top covers the DEPORTED ceiling quad (painter order: ceilings emit BEFORE walls,
	       same prio 5; owner 2026-07-02).  Only when ceilings are deported (sat_vdp1_floor); else
	       the software ceiling (NBG1 prio 6) covered it.  Phase-1 clamp: mirrored top cut +
	       software wedge above (sat_wall_cut_ceil); else full CPU as before. */
	    if (!(sat_wall_clamp && sat_v1_mid
	          && sat_wall_cut_ceil(rw_midtexturemid, v1, yh1, yh2, midtexture, u1, u2, cm,
	                               sat_sw_mid, &sat_wcl_mid_ef, &sat_wcl_mid_es, &sat_wcl_mid)))
	        { sat_fb_clamp_t++; sat_fb_px += (yh1 - yl1) * (rw_stopx - rw_x); }
	    sat_sw_mid = 1;   /* full tier on a failed clamp; only the WEDGE rows when sat_wcl_mid is armed */
	}
	else if (sat_v1_mid_edge)
	{
	    /* SATURN L5: hand VDP1 only the columns its own emitter accepts (sat_wall_edge_hook
	       replays wall_emit_band's guard), and keep the rejected borders on the CPU, where u is
	       resampled per column from finetangent[] and is therefore perspective-EXACT -- strictly
	       better than another affine sub-quad.  Not armed -> the untouched full-CPU wall. */
	    if (sat_wall_try_edge(midtexture, yl1, yh1, yl2, yh2, u1, u2, v0, v1, cm, 0))
		sat_sw_mid = 0;   /* the column loop now draws ONLY the borders (is_edge) */
	}
#if SAT_WALL_SUBDIV
	else if (sat_v1_mid_sub && !sat_v1_mid)   /* normal path only (pot2 force-sets sat_v1_mid=1 -> single quad) */
	{
	    /* PERSPECTIVE SUBDIVISION of a magnified wall: split [rw_x, rw_stopx-1] into N narrow sub-segs,
	       RE-SAMPLE u at each endpoint via the true tangent formula (perspective-correct) + EXACT linear
	       yl/yh -> no squish, no swim.  Sub-segs abut column-for-column (no seam).  N ~ columns-per-texel
	       (the same magnification quantity), capped.  Whole wall -> CPU on a bank-full reject. */
	    int sx = rw_stopx - rw_x, mdu = u2 - u1; if (mdu < 0) mdu = -mdu; if (mdu < 1) mdu = 1;
	    int N = 1 + sx / mdu; if (N < 2) N = 2; { int cap = (sat_opt >= 4) ? sat_wall_subdiv_max : SAT_WALL_SUBDIV_MAX; if (N > cap) N = cap; }   /* SATURN L4 */
	    int tw = texturewidthmask[midtexture] + 1;
	    int prev_b = rw_x, k;
	    for (k = 1; k <= N; k++)
	    {
		int b = rw_x + (sx * k) / N;               /* right boundary (exclusive) of sub-seg k */
		int xl = prev_b, xr = b - 1; prev_b = b;
		if (xr < xl) continue;                     /* degenerate (N > visible columns) */
		{
		    int dnl = xl - rw_x, dnr = xr - rw_x;
		    int al  = (rw_centerangle + xtoviewangle[xl]) >> ANGLETOFINESHIFT;
		    int ar  = (rw_centerangle + xtoviewangle[xr]) >> ANGLETOFINESHIFT;
		    int ul  = (rw_offset - FixedMul(finetangent[al], rw_distance)) >> FRACBITS;  /* PERSP u */
		    int ur  = (rw_offset - FixedMul(finetangent[ar], rw_distance)) >> FRACBITS;
		    /* per-SUB-SEG squish guard, LOCAL slope (the wall-average sx/mdu underestimates the
		       closest sub-seg of a perspective-skewed seg): if this sub-seg's texture tile would
		       extrapolate past the platform's coordinate allowance, the emitter could only draw
		       it as a clamp+squish quad -> route the whole wall to SOFTWARE instead.  sdu < 1
		       also catches the du==0 degenerate (full-char single quad = worst squish).  1024
		       mirrors the platform bound (view width + 2*wall_ext, conservatively). */
		    int sdu = ur - ul; if (sdu < 0) sdu = -sdu;
		    if (sdu < 1 || (long long)tw * (xr - xl + 1) > 1024LL * sdu)
			{ sat_sw_mid = 1; sat_fb_mag_t++; break; }
		    int yll = (topfrac    + topstep    * dnl + HEIGHTUNIT - 1) >> HEIGHTBITS;    /* EXACT */
		    int ylr = (topfrac    + topstep    * dnr + HEIGHTUNIT - 1) >> HEIGHTBITS;
		    int yhl = (bottomfrac + bottomstep * dnl) >> HEIGHTBITS;
		    int yhr = (bottomfrac + bottomstep * dnr) >> HEIGHTBITS;
		    int sv0, sv1; SAT_VROWS(rw_midtexturemid, yll, yhl, sv0, sv1);
		    if (sat_wall_hook (xl, yll, yhl, xr, ylr, yhr, midtexture, ul, ur, sv0, sv1, cm))
			{ sat_sw_mid = 1; sat_fb_starve_t++; break; }   /* bank full -> whole wall SW */
		}
	    }
	}
#endif
	else if (sat_wall_hook (rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2, midtexture, u1, u2, v0, v1, cm))
	    { sat_sw_mid = 1; sat_fb_starve_t++; }   /* VDP1 starved (command list full) -> draw this wall in SOFTWARE, not sky */
	/* LEAD-FILL: record ONLY what VDP1 really took.  Every branch above can hand the tier back to
	   the software (cull, floor clamp, edge split, starve) -- recording those would make the next
	   frame subtract an area VDP1 never covered, i.e. UNDER-draw and leave the hole we are here to
	   close.  sat_sw_mid is the one flag they all set. */
	if (!sat_sw_mid)
	    sat_lead_record ((segidx0 << 2) | 0, rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2);
    }

    /* SATURN VDP1 world renderer: two-sided walls -> upper (toptexture) + lower
       (bottomtexture) quads into the SAME painter list as the one-sided walls, so a
       NEAR two-sided frame correctly draws over a FAR one-sided wall seen through the
       opening (the gap between upper/lower has no texture -> the far wall shows there). */
    if (sat_wall_hook && SAT_WALL_VDP1_OK && curline->backsector && rw_stopx > rw_x)
    {
	int n   = rw_stopx - 1 - rw_x;
	int a1 = (rw_centerangle + xtoviewangle[rw_x])        >> ANGLETOFINESHIFT;
	int a2 = (rw_centerangle + xtoviewangle[rw_stopx - 1]) >> ANGLETOFINESHIFT;
	int u1 = (rw_offset - FixedMul(finetangent[a1], rw_distance)) >> FRACBITS;
	int u2 = (rw_offset - FixedMul(finetangent[a2], rw_distance)) >> FRACBITS;
	int _li = rw_scale >> LIGHTSCALESHIFT;   /* distance-correct light (was fixed mid-level) */
	if (_li >= MAXLIGHTSCALE) _li = MAXLIGHTSCALE - 1; else if (_li < 0) _li = 0;
	const lighttable_t *cm = walllights[_li];
	if (toptexture && (sat_v1_up || sat_v1_up_sub))   /* ceiling -> top of the opening */
	{
	    int yl1 = (topfrac + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yl2 = (topfrac + topstep * n + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yh1 = pixhigh >> HEIGHTBITS;
	    int yh2 = (pixhigh + pixhighstep * n) >> HEIGHTBITS;
	    int v0, v1; SAT_VROWS(rw_toptexturemid, yl1, yh1, v0, v1);
	    sat_lead_arm (&sat_lead_up, (segidx0 << 2) | 1, rw_x, rw_stopx - 1);   /* LEAD-FILL, see the mid tier */
	    /* SATURN: same floor handling as the other tiers -- cull an upper (toptexture) wall
	       entirely below the RBG0 floor line; hand a partially-below one to the CPU (clips to
	       floorclip).  (Rare for a ceiling-side tier, but completes the set.) */
	    if (sat_wall_clamp && !sat_wall_span_visible(yl1, yl2, yh1, yh2))
		{ /* invisible at every column -> claim nothing (see the mid tier) */ }
	    else if (sat_floor_punch_here() && yl1 >= floorclip[rw_x] && yl2 >= floorclip[rw_stopx - 1])
		{ if (sat_wall_clamp) sat_sw_up = 1; /* SATURN: 816 passed => VISIBLE; end-only "below floor" = pedestal false-positive -> SOFTWARE (per-column floorclip clips).  clamp off: cull as before. */ }
	    else if (sat_floor_punch_here() && sat_wall_cross_lo(yh1, yh2))
	    {   /* below the floor somewhere -> Phase-1 world-anchored cut + software wedge; else CPU */
		if (!(sat_wall_clamp && sat_v1_up
		      && !(sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
		      && sat_wall_cut_floor(rw_toptexturemid, v0, yl1, yl2, toptexture, u1, u2, cm,
		                            sat_sw_up, &sat_wcl_up_ef, &sat_wcl_up_es, &sat_wcl_up)))
		    sat_fb_clamp_t++;
		sat_sw_up = 1;
	    }
	    else if (sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
	    {   /* above a nearer deported ceiling -> mirrored top cut + wedge; else CPU as before */
		if (!(sat_wall_clamp && sat_v1_up
		      && sat_wall_cut_ceil(rw_toptexturemid, v1, yh1, yh2, toptexture, u1, u2, cm,
		                           sat_sw_up, &sat_wcl_up_ef, &sat_wcl_up_es, &sat_wcl_up)))
		    sat_fb_clamp_t++;
		sat_sw_up = 1;
	    }
#if SAT_WALL_SUBDIV
	    else if (sat_v1_up_sub && !sat_v1_up)   /* magnified door LINTEL -> perspective subdivision (top=topfrac, bottom=pixhigh) */
	    {
		int sx = rw_stopx - rw_x, mdu = u2 - u1; if (mdu < 0) mdu = -mdu; if (mdu < 1) mdu = 1;
		int N = 1 + sx / mdu; if (N < 2) N = 2; { int cap = (sat_opt >= 4) ? sat_wall_subdiv_max : SAT_WALL_SUBDIV_MAX; if (N > cap) N = cap; }   /* SATURN L4 */
		int tw = texturewidthmask[toptexture] + 1;
		int prev_b = rw_x, k;
		for (k = 1; k <= N; k++)
		{
		    int b = rw_x + (sx * k) / N;
		    int xl = prev_b, xr = b - 1; prev_b = b;
		    if (xr < xl) continue;
		    {
			int dnl = xl - rw_x, dnr = xr - rw_x;
			int al  = (rw_centerangle + xtoviewangle[xl]) >> ANGLETOFINESHIFT;
			int ar  = (rw_centerangle + xtoviewangle[xr]) >> ANGLETOFINESHIFT;
			int ul  = (rw_offset - FixedMul(finetangent[al], rw_distance)) >> FRACBITS;
			int ur  = (rw_offset - FixedMul(finetangent[ar], rw_distance)) >> FRACBITS;
			/* per-sub-seg squish guard, local slope (see the mid tier) */
			int sdu = ur - ul; if (sdu < 0) sdu = -sdu;
			if (sdu < 1 || (long long)tw * (xr - xl + 1) > 1024LL * sdu)
			    { sat_sw_up = 1; sat_fb_mag_t++; break; }
			int yll = (topfrac + topstep     * dnl + HEIGHTUNIT - 1) >> HEIGHTBITS;
			int ylr = (topfrac + topstep     * dnr + HEIGHTUNIT - 1) >> HEIGHTBITS;
			int yhl = (pixhigh + pixhighstep * dnl) >> HEIGHTBITS;
			int yhr = (pixhigh + pixhighstep * dnr) >> HEIGHTBITS;
			int sv0, sv1; SAT_VROWS(rw_toptexturemid, yll, yhl, sv0, sv1);
			if (sat_wall_hook (xl, yll, yhl, xr, ylr, yhr, toptexture, ul, ur, sv0, sv1, cm))
			    { sat_sw_up = 1; sat_fb_starve_t++; break; }
		    }
		}
	    }
#endif
	    else
		if (sat_wall_hook (rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2, toptexture, u1, u2, v0, v1, cm))
		    { sat_sw_up = 1; sat_fb_starve_t++; }   /* VDP1 starved (list full) -> upper in SOFTWARE, not sky */
	    if (!sat_sw_up)   /* LEAD-FILL: record only what VDP1 really took (see the mid tier) */
		sat_lead_record ((segidx0 << 2) | 1, rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2);
	}
	if (bottomtexture && (sat_v1_lo || sat_v1_lo_sub))   /* bottom of the opening -> floor.
	       sat_v1_lo_sub MUST be in this gate like the mid/top tiers: a magnified lower tier
	       (face-on step riser) has sat_v1_lo==0 + sat_sw_lo==0 (the subdivision owns it), so
	       without it NOBODY drew the tier -> invisible riser (owner capture 2026-07-03). */
	{
	    int yl1 = (pixlow + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yl2 = (pixlow + pixlowstep * n + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yh1 = bottomfrac >> HEIGHTBITS;
	    int yh2 = (bottomfrac + bottomstep * n) >> HEIGHTBITS;
	    int v0, v1; SAT_VROWS(rw_bottomtexturemid, yl1, yh1, v0, v1);
	    sat_lead_arm (&sat_lead_lo, (segidx0 << 2) | 2, rw_x, rw_stopx - 1);   /* LEAD-FILL, see the mid tier */
	    /* SATURN: same floor handling as the one-sided wall -- cull a lower (bottomtexture)
	       wall entirely below the floor; hand a partially-below one to the CPU, which clips
	       each column to floorclip (no VDP1 bleed-through, no squish). */
	    if (sat_wall_clamp && !sat_wall_span_visible(yl1, yl2, yh1, yh2))
		{ /* invisible at every column -> claim nothing (see the mid tier) */ }
	    else if (sat_floor_punch_here() && yl1 >= floorclip[rw_x] && yl2 >= floorclip[rw_stopx - 1])
		{ if (sat_wall_clamp) sat_sw_lo = 1; /* SATURN: 887 passed => VISIBLE; end-only "below floor" = pedestal false-positive -> SOFTWARE (per-column floorclip clips).  clamp off: cull as before. */ }
	    else if (sat_floor_punch_here() && sat_wall_cross_lo(yh1, yh2))
	    {   /* below the floor somewhere -> Phase-1 world-anchored cut + software wedge; else CPU */
		if (!(sat_wall_clamp && sat_v1_lo
		      && !(sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
		      && sat_wall_cut_floor(rw_bottomtexturemid, v0, yl1, yl2, bottomtexture, u1, u2, cm,
		                            sat_sw_lo, &sat_wcl_lo_ef, &sat_wcl_lo_es, &sat_wcl_lo)))
		    sat_fb_clamp_t++;
		sat_sw_lo = 1;
	    }
	    else if (sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
	    {   /* above a nearer deported ceiling -> mirrored top cut + wedge; else CPU as before */
		if (!(sat_wall_clamp && sat_v1_lo
		      && sat_wall_cut_ceil(rw_bottomtexturemid, v1, yh1, yh2, bottomtexture, u1, u2, cm,
		                           sat_sw_lo, &sat_wcl_lo_ef, &sat_wcl_lo_es, &sat_wcl_lo)))
		    sat_fb_clamp_t++;
		sat_sw_lo = 1;
	    }
#if SAT_WALL_SUBDIV
	    else if (sat_v1_lo_sub && !sat_v1_lo)   /* magnified door SILL -> perspective subdivision (top=pixlow, bottom=bottomfrac) */
	    {
		int sx = rw_stopx - rw_x, mdu = u2 - u1; if (mdu < 0) mdu = -mdu; if (mdu < 1) mdu = 1;
		int N = 1 + sx / mdu; if (N < 2) N = 2; { int cap = (sat_opt >= 4) ? sat_wall_subdiv_max : SAT_WALL_SUBDIV_MAX; if (N > cap) N = cap; }   /* SATURN L4 */
		int tw = texturewidthmask[bottomtexture] + 1;
		int prev_b = rw_x, k;
		for (k = 1; k <= N; k++)
		{
		    int b = rw_x + (sx * k) / N;
		    int xl = prev_b, xr = b - 1; prev_b = b;
		    if (xr < xl) continue;
		    {
			int dnl = xl - rw_x, dnr = xr - rw_x;
			int al  = (rw_centerangle + xtoviewangle[xl]) >> ANGLETOFINESHIFT;
			int ar  = (rw_centerangle + xtoviewangle[xr]) >> ANGLETOFINESHIFT;
			int ul  = (rw_offset - FixedMul(finetangent[al], rw_distance)) >> FRACBITS;
			int ur  = (rw_offset - FixedMul(finetangent[ar], rw_distance)) >> FRACBITS;
			/* per-sub-seg squish guard, local slope (see the mid tier) */
			int sdu = ur - ul; if (sdu < 0) sdu = -sdu;
			if (sdu < 1 || (long long)tw * (xr - xl + 1) > 1024LL * sdu)
			    { sat_sw_lo = 1; sat_fb_mag_t++; break; }
			int yll = (pixlow     + pixlowstep * dnl + HEIGHTUNIT - 1) >> HEIGHTBITS;
			int ylr = (pixlow     + pixlowstep * dnr + HEIGHTUNIT - 1) >> HEIGHTBITS;
			int yhl = (bottomfrac + bottomstep * dnl) >> HEIGHTBITS;
			int yhr = (bottomfrac + bottomstep * dnr) >> HEIGHTBITS;
			int sv0, sv1; SAT_VROWS(rw_bottomtexturemid, yll, yhl, sv0, sv1);
			if (sat_wall_hook (xl, yll, yhl, xr, ylr, yhr, bottomtexture, ul, ur, sv0, sv1, cm))
			    { sat_sw_lo = 1; sat_fb_starve_t++; break; }
		    }
		}
	    }
#endif
	    else
		if (sat_wall_hook (rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2, bottomtexture, u1, u2, v0, v1, cm))
		    { sat_sw_lo = 1; sat_fb_starve_t++; }   /* VDP1 starved (list full) -> lower in SOFTWARE, not sky */
	    if (!sat_sw_lo)   /* LEAD-FILL: record only what VDP1 really took (see the mid tier) */
		sat_lead_record ((segidx0 << 2) | 2, rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2);
	}
    }
#undef SAT_VROWS

    /* SATURN PERF (lever C): does the SOFTWARE renderer draw any wall tier this seg?
       When the VDP1 world renderer owns every tier (sat_wall_skip set, no close/
       transition CPU fallback) no colfunc runs in the loop below, so the per-column
       lighting lookup + the dc_iscale divide are dead work -> skip them (the divide is
       the costly part of wall-prep, Bp).  DoomJo / VDP1-off: sat_wall_skip is 0, so
       sw_draws stays true for any textured tier and the segtextured block is unchanged. */
    if (midtexture)
        sw_draws = (!sat_wall_skip || sat_sw_mid);
    else
        sw_draws = (toptexture    && (!sat_wall_skip || sat_sw_up))
                || (bottomtexture && (!sat_wall_skip || sat_sw_lo));

    /* LEAD-FILL: the difference spans are drawn by the SOFTWARE, so the per-column lighting and
       dc_iscale must be computed for this seg even when VDP1 owns every tier. */
    if (sat_lead_mid.on || sat_lead_up.on || sat_lead_lo.on) sw_draws = 1;

#if SAT_WALL_EDGE_FILL
    int sat_ef_x0 = rw_x, sat_ef_x1 = rw_stopx - 1;   /* seg screen extent, for the edge-fill margin */
#endif
    /* SATURN LOAD BUDGET (2026-08-06).  Decided ONCE per tier per seg, before the column loop.
       In the CD-streaming build a texture that is not resident costs a SYNCHRONOUS ~42 ms disc read
       inside R_GetColumn -- charged to `Bp`, for a wall that may be three screen columns wide.  That
       is the whole of the 480..790 ms `Bp` frames in the owner's TNT captures.  Beyond
       sat_tex_load_budget faults per frame we draw the tier FLAT instead: sat_dc_solid skips
       R_GetColumn entirely, so no composite, no patch, NO DISC.  The wall textures itself over the
       next frames as the budget refills -- bounded loading, no hitch, no async machinery.
       No distance test is needed: the BSP walk is front-to-back, so the budget is spent on the
       NEAREST walls by construction and the far ones are what degrades. */
    int io_flat_mid = midtexture    ? sat_wall_io_flat (midtexture)    : 0;
    int io_flat_up  = toptexture    ? sat_wall_io_flat (toptexture)    : 0;
    int io_flat_lo  = bottomtexture ? sat_wall_io_flat (bottomtexture) : 0;
    int io_col_mid  = io_flat_mid ? sat_wall_flat_color (midtexture)    : 0;
    int io_col_up   = io_flat_up  ? sat_wall_flat_color (toptexture)    : 0;
    int io_col_lo   = io_flat_lo  ? sat_wall_flat_color (bottomtexture) : 0;
    for ( ; rw_x < rw_stopx ; rw_x++)
    {
	/* SATURN L5: the CPU border columns of an edge-split wall (sat_we_on, armed in the claim
	   block above).  Disarmed -> a constant 0 the compiler folds away, exactly like the old
	   SAT_WALL_EDGE_FILL enum it replaces.  DoomJo / VDP1-off never arm it. */
	int is_edge = sat_we_on && (rw_x <= sat_we_lo || rw_x >= sat_we_hi);
	// mark floor / ceiling areas
	yl = (topfrac+HEIGHTUNIT-1)>>HEIGHTBITS;

	// no space above wall?
	if (yl < ceilingclip[rw_x]+1)
	    yl = ceilingclip[rw_x]+1;
	
	if (markceiling)
	{
	    top = ceilingclip[rw_x]+1;
	    bottom = yl-1;

	    if (bottom >= floorclip[rw_x])
		bottom = floorclip[rw_x]-1;

	    if (top <= bottom)
	    {
		ceilingplane->top[rw_x] = top;
		ceilingplane->bottom[rw_x] = bottom;
	    }
	}
		
	yh = bottomfrac>>HEIGHTBITS;

	if (yh >= floorclip[rw_x])
	    yh = floorclip[rw_x]-1;

	if (markfloor)
	{
	    top = yh+1;
	    bottom = floorclip[rw_x]-1;
	    if (top <= ceilingclip[rw_x])
		top = ceilingclip[rw_x]+1;
	    if (top <= bottom)
	    {
		floorplane->top[rw_x] = top;
		floorplane->bottom[rw_x] = bottom;
	    }
	}
	
	// texturecolumn and lighting are independent of wall tiers
	/* SATURN PERF (lever C2, REC Bp-cut): texturecolumn feeds ONLY R_GetColumn (the
	   software column draw, gated by sw_draws) and the masked-midtexture column save
	   (gated by maskedtexture).  When VDP1 owns every tier of a non-masked seg
	   (sw_draws==0 && !maskedtexture) it is dead work -- skip the per-column angle
	   lookup + finetangent read + FixedMul + shift.  On DoomJo / VDP1-off, sat_wall_skip
	   is 0 so sw_draws is always 1 -> the condition is always true -> byte-identical. */
	if (segtextured && (sw_draws || is_edge || maskedtexture))
	{
	    // calculate texture offset
	    angle = (rw_centerangle + xtoviewangle[rw_x])>>ANGLETOFINESHIFT;
	    texturecolumn = rw_offset-FixedMul(finetangent[angle],rw_distance);
	    texturecolumn >>= FRACBITS;
	    /* SATURN PERF (lever C): the lighting lookup + the per-column dc_iscale divide
	       feed only the software column draw (colfunc).  When VDP1 owns every tier this
	       seg (sw_draws == 0) no colfunc runs -> skip them; the divide is the costly bit. */
	    if (sw_draws || is_edge)
	    {
		// calculate lighting
		index = rw_scale>>LIGHTSCALESHIFT;

		if (index >=  MAXLIGHTSCALE )
		    index = MAXLIGHTSCALE-1;

		dc_colormap = walllights[index];
		dc_x = rw_x;
		/* solid Potato walls ignore dc_iscale -> skip the per-column division */
		if (!wall_solid)
		    dc_iscale = 0xffffffffu / (unsigned)rw_scale;
	    }
	}
        else
        {
            // purely to shut up the compiler

            texturecolumn = 0;
        }
	
	// draw the wall tiers
	if (midtexture)
	{
	    // single sided line
	    /* SATURN: VDP1 owns this wall -> skip the software column draw, but KEEP the
	       clip update so floors/ceilings/sprite occlusion stay correct.  EXCEPT a
	       too-close OR transition wall (sat_sw_mid): the CPU draws it (no swim/explosion). */
	    if (!sat_wall_skip || sat_sw_mid || is_edge)
	    {
		dc_yl = yl;
		dc_yh = yh;
		/* Phase-1 wedge: VDP1 owns the tier up to the cut line -> software draws only the
		   residue past it (colfunc tolerates yl > yh, same as vanilla off-screen columns). */
		if (sat_wcl_mid == 1)
		    { int e = (int)(sat_wcl_mid_ef >> HEIGHTBITS); if (dc_yl < e) dc_yl = e; }
		else if (sat_wcl_mid == 2)
		    { int e = (int)(sat_wcl_mid_ef >> HEIGHTBITS); if (dc_yh > e) dc_yh = e; }
		dc_texturemid = rw_midtexturemid;
		if (wall_solid)
		    sat_wall_color = R_WallPotatoColor(midtexture);
		else if (io_flat_mid)
		    sat_wall_color = io_col_mid;   /* SATURN load budget: no disc this frame */
		else
		    dc_source = R_GetColumn(midtexture,texturecolumn);
		sat_dc_solid = wall_solid || io_flat_mid;   /* SATURN: armed for WALL columns only (see r_draw.c) */
		colfunc ();
		sat_dc_solid = 0;
	    }
	    else if (sat_lead_mid.on)   /* LEAD-FILL: VDP1 owns it -> draw only what the OLD quad missed */
	    {
		dc_texturemid = rw_midtexturemid;
		int lflat = wall_solid || sat_lead_flat;   /* see sat_lead_flat */
		if (lflat)            sat_wall_color = R_WallPotatoColor(midtexture);
		else if (io_flat_mid) { sat_wall_color = io_col_mid; lflat = 1; }
		else                  dc_source = R_GetColumn(midtexture, texturecolumn);
		sat_dc_solid = lflat;
		sat_lead_draw (&sat_lead_mid, rw_x, yl, yh);
		sat_dc_solid = 0;
	    }
	    if (sat_wcl_mid) sat_wcl_mid_ef += sat_wcl_mid_es;
	    ceilingclip[rw_x] = viewheight;
	    floorclip[rw_x] = -1;
	}
	else
	{
	    // two sided line
	    if (toptexture)
	    {
		// top wall
		mid = pixhigh>>HEIGHTBITS;
		pixhigh += pixhighstep;

		if (mid >= floorclip[rw_x])
		    mid = floorclip[rw_x]-1;

		if (mid >= yl)
		{
		    if (!sat_wall_skip || sat_sw_up || is_edge)   /* VDP1 owns it (unless close/transition); is_edge = edge-fill */
		    {
			dc_yl = yl;
			dc_yh = mid;
			if (sat_wcl_up == 1)      /* Phase-1 wedge: only the residue past the VDP1 cut line */
			    { int e = (int)(sat_wcl_up_ef >> HEIGHTBITS); if (dc_yl < e) dc_yl = e; }
			else if (sat_wcl_up == 2)
			    { int e = (int)(sat_wcl_up_ef >> HEIGHTBITS); if (dc_yh > e) dc_yh = e; }
			dc_texturemid = rw_toptexturemid;
			if (wall_solid)
			    sat_wall_color = R_WallPotatoColor(toptexture);
			else if (io_flat_up)
			    sat_wall_color = io_col_up;
			else
			    dc_source = R_GetColumn(toptexture,texturecolumn);
			sat_dc_solid = wall_solid || io_flat_up;   /* SATURN: WALL columns only (see r_draw.c) */
			colfunc ();
			sat_dc_solid = 0;
		    }
		    else if (sat_lead_up.on)   /* LEAD-FILL, see the mid tier */
		    {
			dc_texturemid = rw_toptexturemid;
			int lflat = wall_solid || sat_lead_flat;   /* see sat_lead_flat */
			if (lflat)           sat_wall_color = R_WallPotatoColor(toptexture);
			else if (io_flat_up) { sat_wall_color = io_col_up; lflat = 1; }
			else                 dc_source = R_GetColumn(toptexture, texturecolumn);
			sat_dc_solid = lflat;
			sat_lead_draw (&sat_lead_up, rw_x, yl, mid);
			sat_dc_solid = 0;
		    }
		    ceilingclip[rw_x] = mid;
		}
		else
		    ceilingclip[rw_x] = yl-1;
		if (sat_wcl_up) sat_wcl_up_ef += sat_wcl_up_es;   /* step the wedge edge per column */
	    }
	    else
	    {
		// no top wall
		if (markceiling)
		    ceilingclip[rw_x] = yl-1;
	    }
			
	    if (bottomtexture)
	    {
		// bottom wall
		mid = (pixlow+HEIGHTUNIT-1)>>HEIGHTBITS;
		pixlow += pixlowstep;

		// no space above wall?
		if (mid <= ceilingclip[rw_x])
		    mid = ceilingclip[rw_x]+1;
		
		if (mid <= yh)
		{
		    if (!sat_wall_skip || sat_sw_lo || is_edge)   /* VDP1 owns it (unless close/transition); is_edge = edge-fill */
		    {
			dc_yl = mid;
			dc_yh = yh;
			if (sat_wcl_lo == 1)      /* Phase-1 wedge: only the residue past the VDP1 cut line */
			    { int e = (int)(sat_wcl_lo_ef >> HEIGHTBITS); if (dc_yl < e) dc_yl = e; }
			else if (sat_wcl_lo == 2)
			    { int e = (int)(sat_wcl_lo_ef >> HEIGHTBITS); if (dc_yh > e) dc_yh = e; }
			dc_texturemid = rw_bottomtexturemid;
			if (wall_solid)
			    sat_wall_color = R_WallPotatoColor(bottomtexture);
			else if (io_flat_lo)
			    sat_wall_color = io_col_lo;
			else
			    dc_source = R_GetColumn(bottomtexture,
						    texturecolumn);
			sat_dc_solid = wall_solid || io_flat_lo;   /* SATURN: WALL columns only (see r_draw.c) */
			colfunc ();
			sat_dc_solid = 0;
		    }
		    else if (sat_lead_lo.on)   /* LEAD-FILL, see the mid tier */
		    {
			dc_texturemid = rw_bottomtexturemid;
			int lflat = wall_solid || sat_lead_flat;   /* see sat_lead_flat */
			if (lflat)           sat_wall_color = R_WallPotatoColor(bottomtexture);
			else if (io_flat_lo) { sat_wall_color = io_col_lo; lflat = 1; }
			else                 dc_source = R_GetColumn(bottomtexture, texturecolumn);
			sat_dc_solid = lflat;
			sat_lead_draw (&sat_lead_lo, rw_x, mid, yh);
			sat_dc_solid = 0;
		    }
		    floorclip[rw_x] = mid;
		}
		else
		    floorclip[rw_x] = yh+1;
		if (sat_wcl_lo) sat_wcl_lo_ef += sat_wcl_lo_es;   /* step the wedge edge per column */
	    }
	    else
	    {
		// no bottom wall
		if (markfloor)
		    floorclip[rw_x] = yh+1;
	    }
			
	    if (maskedtexture)
	    {
		// save texturecol
		//  for backdrawing of masked mid texture
		maskedtexturecol[rw_x] = texturecolumn;
	    }
	}
		
	rw_scale += rw_scalestep;
	topfrac += topstep;
	bottomfrac += bottomstep;
	/* LEAD-FILL: step each armed tier's OLD quad edges one column, like the wedge edges above. */
	if (sat_lead_mid.on) { sat_lead_mid.ylf += sat_lead_mid.ylstep; sat_lead_mid.yhf += sat_lead_mid.yhstep; }
	if (sat_lead_up.on)  { sat_lead_up.ylf  += sat_lead_up.ylstep;  sat_lead_up.yhf  += sat_lead_up.yhstep;  }
	if (sat_lead_lo.on)  { sat_lead_lo.ylf  += sat_lead_lo.ylstep;  sat_lead_lo.yhf  += sat_lead_lo.yhstep;  }
	if (sat_lead_mid.on2){ sat_lead_mid.ylfb += sat_lead_mid.ylstepb; sat_lead_mid.yhfb += sat_lead_mid.yhstepb; }
	if (sat_lead_up.on2) { sat_lead_up.ylfb  += sat_lead_up.ylstepb;  sat_lead_up.yhfb  += sat_lead_up.yhstepb;  }
	if (sat_lead_lo.on2) { sat_lead_lo.ylfb  += sat_lead_lo.ylstepb;  sat_lead_lo.yhfb  += sat_lead_lo.yhstepb;  }
    }
}




//
// R_StoreWallRange
// A wall segment will be drawn
//  between start and stop pixels (inclusive).
//
// SATURN PERF 2.4 Stage 1: the public entry is a thin wrapper that brackets the
// wall-prep work (texture setup + R_RenderSegLoop column recording) with
// RP_WallPrep{Enter,Leave} so the profiler can subtract wall-prep from B and
// expose the pure BSP-walk cost -- the number that bounds the 2.4 slave-offload
// payoff.  No-op (a bare call) unless RP_PROF.  The real body is _impl below.
//
static void R_StoreWallRange_impl(int start, int stop);

void R_StoreWallRange(int start, int stop)
{
    RP_WallPrepEnter();
    R_StoreWallRange_impl(start, stop);
    RP_WallPrepLeave();
}

/* ============================================================================
 * SATURN parallel-REC -- wall-prep producer/consumer (d32xr-style).  The master
 * walks the BSP + clips (Bw, updating solidsegs) and QUEUES each visible wall range
 * here instead of running the wall-prep (R_StoreWallRange) inline.  RP_FlushWalls
 * then runs them all in BSP order, so the floorclip/ceilingclip occlusion chain is
 * identical.  STEP 1 (now): a master-only defer == byte-identical render, the
 * validation harness for the STEP 2 slave consumer.  Gated on sat_wallprep_defer
 * (0 = inline => DoomJo + the baseline are unchanged). */
typedef struct {
    seg_t      *curline;
    sector_t   *frontsector, *backsector;
    angle_t     rw_angle1;
    visplane_t *floorplane, *ceilingplane;
    int         start, stop;
} walljob_t;
static walljob_t walljobs[MAXDRAWSEGS];
int  walljob_n = 0;
int  sat_wallprep_defer = 0;

void RP_QueueWall(int start, int stop)
{
    walljob_t *w;
    if (!sat_wallprep_defer || walljob_n >= MAXDRAWSEGS)
        { R_StoreWallRange(start, stop); return; }
    w = &walljobs[walljob_n++];
    w->curline      = curline;
    w->frontsector  = frontsector;
    w->backsector   = backsector;
    w->rw_angle1    = rw_angle1;
    w->floorplane   = floorplane;
    w->ceilingplane = ceilingplane;
    w->start = start; w->stop = stop;
}

/* Replay queued walls [from,to) in BSP order (single in-order consumer => the floorclip/
   ceilingclip occlusion chain is identical).  Does NOT reset walljob_n -- the caller does, so
   the slave (RANK 3 inc-1, r_parallel.c) can flush a range without owning the master's counter. */
void RP_FlushWallsRange(int from, int to)
{
    int i;
    for (i = from; i < to; i++)
    {
        walljob_t *w = &walljobs[i];
        curline      = w->curline;
        frontsector  = w->frontsector;
        backsector   = w->backsector;
        rw_angle1    = w->rw_angle1;
        floorplane   = w->floorplane;
        ceilingplane = w->ceilingplane;
        R_StoreWallRange(w->start, w->stop);
    }
}

void RP_FlushWalls(void)
{
    RP_FlushWallsRange(0, walljob_n);
    walljob_n = 0;
}

static void
R_StoreWallRange_impl
( int	start,
  int	stop )
{
    fixed_t		hyp;
    fixed_t		sineval;
    angle_t		distangle, offsetangle;
    fixed_t		vtop;
    int			lightnum;

    // don't overflow and crash
    if (ds_p == &drawsegs[MAXDRAWSEGS])
	return;		
		
#ifdef RANGECHECK
    if (start >=viewwidth || start > stop)
	I_Error ("Bad R_RenderWallRange: %i to %i", start , stop);
#endif
    
    sidedef = curline->sidedef;
    linedef = curline->linedef;

    // mark the segment as visible for auto map
    linedef->flags |= ML_MAPPED;
    
    // calculate rw_distance for scale calculation
    rw_normalangle = curline->angle + ANG90;
    offsetangle = abs(rw_normalangle-rw_angle1);
    
    if (offsetangle > ANG90)
	offsetangle = ANG90;

    distangle = ANG90 - offsetangle;
    hyp = R_PointToDist (curline->v1->x, curline->v1->y);
    sineval = finesine[distangle>>ANGLETOFINESHIFT];
    rw_distance = FixedMul (hyp, sineval);
		
	
    ds_p->x1 = rw_x = start;
    ds_p->x2 = stop;
    ds_p->curline = curline;
    rw_stopx = stop+1;
    
    // calculate scale at both ends and step
    ds_p->scale1 = rw_scale = 
	R_ScaleFromGlobalAngle (viewangle + xtoviewangle[start]);
    
    if (stop > start )
    {
	ds_p->scale2 = R_ScaleFromGlobalAngle (viewangle + xtoviewangle[stop]);
	ds_p->scalestep = rw_scalestep = 
	    (ds_p->scale2 - rw_scale) / (stop-start);
    }
    else
    {
	// UNUSED: try to fix the stretched line bug
#if 0
	if (rw_distance < FRACUNIT/2)
	{
	    fixed_t		trx,try;
	    fixed_t		gxt,gyt;

	    trx = curline->v1->x - viewx;
	    try = curline->v1->y - viewy;
			
	    gxt = FixedMul(trx,viewcos); 
	    gyt = -FixedMul(try,viewsin); 
	    ds_p->scale1 = FixedDiv(projection, gxt-gyt)<<detailshift;
	}
#endif
	ds_p->scale2 = ds_p->scale1;
    }
    
    // calculate texture boundaries
    //  and decide if floor / ceiling marks are needed
    worldtop = frontsector->ceilingheight - viewz;
    worldbottom = frontsector->floorheight - viewz;
	
    midtexture = toptexture = bottomtexture = maskedtexture = 0;
    ds_p->maskedtexturecol = NULL;
	
    if (!backsector)
    {
	// single sided line
	midtexture = texturetranslation[sidedef->midtexture];
	// a single sided line is terminal, so it must mark ends
	markfloor = markceiling = true;
	if (linedef->flags & ML_DONTPEGBOTTOM)
	{
	    vtop = frontsector->floorheight +
		textureheight[sidedef->midtexture];
	    // bottom of texture at bottom
	    rw_midtexturemid = vtop - viewz;	
	}
	else
	{
	    // top of texture at top
	    rw_midtexturemid = worldtop;
	}
	rw_midtexturemid += sidedef->rowoffset;

	ds_p->silhouette = SIL_BOTH;
	ds_p->sprtopclip = screenheightarray;
	ds_p->sprbottomclip = negonearray;
	ds_p->bsilheight = INT_MAX;
	ds_p->tsilheight = INT_MIN;
    }
    else
    {
	// two sided line
	ds_p->sprtopclip = ds_p->sprbottomclip = NULL;
	ds_p->silhouette = 0;
	
	if (frontsector->floorheight > backsector->floorheight)
	{
	    ds_p->silhouette = SIL_BOTTOM;
	    ds_p->bsilheight = frontsector->floorheight;
	}
	else if (backsector->floorheight > viewz)
	{
	    ds_p->silhouette = SIL_BOTTOM;
	    ds_p->bsilheight = INT_MAX;
	    // ds_p->sprbottomclip = negonearray;
	}
	
	if (frontsector->ceilingheight < backsector->ceilingheight)
	{
	    ds_p->silhouette |= SIL_TOP;
	    ds_p->tsilheight = frontsector->ceilingheight;
	}
	else if (backsector->ceilingheight < viewz)
	{
	    ds_p->silhouette |= SIL_TOP;
	    ds_p->tsilheight = INT_MIN;
	    // ds_p->sprtopclip = screenheightarray;
	}
		
	if (backsector->ceilingheight <= frontsector->floorheight)
	{
	    ds_p->sprbottomclip = negonearray;
	    ds_p->bsilheight = INT_MAX;
	    ds_p->silhouette |= SIL_BOTTOM;
	}
	
	if (backsector->floorheight >= frontsector->ceilingheight)
	{
	    ds_p->sprtopclip = screenheightarray;
	    ds_p->tsilheight = INT_MIN;
	    ds_p->silhouette |= SIL_TOP;
	}
	
	worldhigh = backsector->ceilingheight - viewz;
	worldlow = backsector->floorheight - viewz;
		
	// hack to allow height changes in outdoor areas
	if (frontsector->ceilingpic == skyflatnum 
	    && backsector->ceilingpic == skyflatnum)
	{
	    worldtop = worldhigh;
	}
	
			
	if (worldlow != worldbottom 
	    || backsector->floorpic != frontsector->floorpic
	    || backsector->lightlevel != frontsector->lightlevel)
	{
	    markfloor = true;
	}
	else
	{
	    // same plane on both sides
	    markfloor = false;
	}
	
			
	if (worldhigh != worldtop 
	    || backsector->ceilingpic != frontsector->ceilingpic
	    || backsector->lightlevel != frontsector->lightlevel)
	{
	    markceiling = true;
	}
	else
	{
	    // same plane on both sides
	    markceiling = false;
	}
	
	if (backsector->ceilingheight <= frontsector->floorheight
	    || backsector->floorheight >= frontsector->ceilingheight)
	{
	    // closed door
	    markceiling = markfloor = true;
	}
	

	if (worldhigh < worldtop)
	{
	    // top texture
	    toptexture = texturetranslation[sidedef->toptexture];
	    if (linedef->flags & ML_DONTPEGTOP)
	    {
		// top of texture at top
		rw_toptexturemid = worldtop;
	    }
	    else
	    {
		vtop =
		    backsector->ceilingheight
		    + textureheight[sidedef->toptexture];
		
		// bottom of texture
		rw_toptexturemid = vtop - viewz;	
	    }
	}
	if (worldlow > worldbottom)
	{
	    // bottom texture
	    bottomtexture = texturetranslation[sidedef->bottomtexture];

	    if (linedef->flags & ML_DONTPEGBOTTOM )
	    {
		// bottom of texture at bottom
		// top of texture at top
		rw_bottomtexturemid = worldtop;
	    }
	    else	// top of texture at top
		rw_bottomtexturemid = worldlow;
	}
	rw_toptexturemid += sidedef->rowoffset;
	rw_bottomtexturemid += sidedef->rowoffset;
	
	// allocate space for masked texture tables
	if (sidedef->midtexture)
	{
	    // masked midtexture
	    maskedtexture = true;
	    // SATURN garde-OPENINGS: if the shared pool would overflow, sink this seg's masked-column
	    // table into opening_overflow (harmless writes, garbage-column HOM) instead of corrupting RAM.
	    if (lastopening + (rw_stopx - rw_x) > openings_end)
	    { ds_p->maskedtexturecol = maskedtexturecol = opening_overflow - rw_x; r_opening_ovf++; }
	    else
	    {
		ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
		lastopening += rw_stopx - rw_x;
	    }
	}
    }
    
    // calculate rw_offset (only needed for textured lines)
    segtextured = midtexture | toptexture | bottomtexture | maskedtexture;

    if (segtextured)
    {
	offsetangle = rw_normalangle-rw_angle1;
	
	if (offsetangle > ANG180)
	    offsetangle = -offsetangle;

	if (offsetangle > ANG90)
	    offsetangle = ANG90;

	sineval = finesine[offsetangle >>ANGLETOFINESHIFT];
	rw_offset = FixedMul (hyp, sineval);

	if (rw_normalangle-rw_angle1 < ANG180)
	    rw_offset = -rw_offset;

	rw_offset += sidedef->textureoffset + curline->offset;
	rw_centerangle = ANG90 + viewangle - rw_normalangle;
	
	// calculate light table
	//  use different light tables
	//  for horizontal / vertical / diagonal
	// OPTIMIZE: get rid of LIGHTSEGSHIFT globally
	if (!fixedcolormap)
	{
	    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

	    if (curline->v1->y == curline->v2->y)
		lightnum--;
	    else if (curline->v1->x == curline->v2->x)
		lightnum++;

	    if (lightnum < 0)		
		walllights = scalelight[0];
	    else if (lightnum >= LIGHTLEVELS)
		walllights = scalelight[LIGHTLEVELS-1];
	    else
		walllights = scalelight[lightnum];
	}
    }
    
    // if a floor / ceiling plane is on the wrong side
    //  of the view plane, it is definitely invisible
    //  and doesn't need to be marked.
    
  
    if (frontsector->floorheight >= viewz)
    {
	// above view plane
	markfloor = false;
    }
    
    if (frontsector->ceilingheight <= viewz 
	&& frontsector->ceilingpic != skyflatnum)
    {
	// below view plane
	markceiling = false;
    }

    
    // calculate incremental stepping values for texture edges
    worldtop >>= 4;
    worldbottom >>= 4;
	
    topstep = -FixedMul (rw_scalestep, worldtop);
    topfrac = (centeryfrac>>4) - FixedMul (worldtop, rw_scale);

    bottomstep = -FixedMul (rw_scalestep,worldbottom);
    bottomfrac = (centeryfrac>>4) - FixedMul (worldbottom, rw_scale);
	
    if (backsector)
    {	
	worldhigh >>= 4;
	worldlow >>= 4;

	if (worldhigh < worldtop)
	{
	    pixhigh = (centeryfrac>>4) - FixedMul (worldhigh, rw_scale);
	    pixhighstep = -FixedMul (rw_scalestep,worldhigh);
	}
	
	if (worldlow > worldbottom)
	{
	    pixlow = (centeryfrac>>4) - FixedMul (worldlow, rw_scale);
	    pixlowstep = -FixedMul (rw_scalestep,worldlow);
	}
    }
    
    // render it
    if (markceiling)
	ceilingplane = R_CheckPlane (ceilingplane, rw_x, rw_stopx-1);
    
    if (markfloor)
	floorplane = R_CheckPlane (floorplane, rw_x, rw_stopx-1);

    RP_SegLoopEnter ();   /* SATURN PERF Phase-0a: bracket the per-column loop (c Bp) */
    R_RenderSegLoop ();
    RP_SegLoopLeave ();


    // save sprite clipping info
    if ( ((ds_p->silhouette & SIL_TOP) || maskedtexture)
	 && !ds_p->sprtopclip)
    {
	if (lastopening + (rw_stopx - start) > openings_end)   /* SATURN garde-OPENINGS: sink (bounded copy = HOM, not corruption) */
	{ memcpy (opening_overflow, ceilingclip+start, 2*(rw_stopx-start)); ds_p->sprtopclip = opening_overflow - start; r_opening_ovf++; }
	else
	{
	    memcpy (lastopening, ceilingclip+start, 2*(rw_stopx-start));
	    ds_p->sprtopclip = lastopening - start;
	    lastopening += rw_stopx - start;
	}
    }
    
    if ( ((ds_p->silhouette & SIL_BOTTOM) || maskedtexture)
	 && !ds_p->sprbottomclip)
    {
	if (lastopening + (rw_stopx - start) > openings_end)   /* SATURN garde-OPENINGS: sink (bounded copy = HOM, not corruption) */
	{ memcpy (opening_overflow, floorclip+start, 2*(rw_stopx-start)); ds_p->sprbottomclip = opening_overflow - start; r_opening_ovf++; }
	else
	{
	    memcpy (lastopening, floorclip+start, 2*(rw_stopx-start));
	    ds_p->sprbottomclip = lastopening - start;
	    lastopening += rw_stopx - start;
	}
    }

    if (maskedtexture && !(ds_p->silhouette&SIL_TOP))
    {
	ds_p->silhouette |= SIL_TOP;
	ds_p->tsilheight = INT_MIN;
    }
    if (maskedtexture && !(ds_p->silhouette&SIL_BOTTOM))
    {
	ds_p->silhouette |= SIL_BOTTOM;
	ds_p->bsilheight = INT_MAX;
    }
    ds_p++;
}

