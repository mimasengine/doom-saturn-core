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
#define SAT_WALL_CPU_SPAN 480   /* span > this: too close for VDP1 -> render in SOFTWARE (CPU). */
#define SAT_WALL_CPU_V1   576   /* VDP1 starts EARLY (span < this, above the CPU threshold) so it
                                   pre-warms the pipeline a frame+ before the CPU hands off -- on
                                   Saturn the VDP1 presents >2 frames late, so the CPU exit-frames
                                   alone still showed sky.  Band [SPAN,V1] = CPU + VDP1 both. */
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
#define SAT_WALL_SUBDIV_MAX  6   /* hard cap on sub-segments per magnified wall (budget guard) */

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

/* EXTRA CPU FRAMES on EXIT (Romain): when a one-sided wall leaves the CPU path (moves away,
   CPU->VDP1), the VDP1 presents several frames late -> a sky gap at the seam.  So for N frames after
   it stops being CPU, the CPU ALSO draws it (overlap) while VDP1 catches up.  N=2.  Per-seg (not a
   span band): only the wall that actually transitions pays, those frames only -- economical.  Keyed
   by the seg index (seg_t's are
   static level data -> stable pointer); 1 byte = CPU-exit-frame countdown. */
#define SAT_SEG_MAX 4096
static unsigned char sat_seg_cpu[SAT_SEG_MAX];

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
    int			sat_v1_mid = 0, sat_v1_up = 0, sat_v1_lo = 0;
    int			sat_v1_mid_sub = 0, sat_v1_up_sub = 0, sat_v1_lo_sub = 0;   /* magnified tier -> perspective-subdivide on VDP1 (not CPU) */
    /* SATURN Phase-1 wall clamp: per-tier residual-WEDGE state.  0 = off (software draws the
       full tier when sat_sw_* is set); 1 = below-floor cut, software draws only rows >= edge(x);
       2 = above-ceiling cut, software draws only rows <= edge(x).  edge(x) steps linearly per
       column exactly like bottomfrac (HEIGHTBITS domain), armed by sat_wall_cut_floor/_ceil. */
    int			sat_wcl_mid = 0, sat_wcl_up = 0, sat_wcl_lo = 0;
    fixed_t		sat_wcl_mid_ef = 0, sat_wcl_mid_es = 0;
    fixed_t		sat_wcl_up_ef = 0,  sat_wcl_up_es = 0;
    fixed_t		sat_wcl_lo_ef = 0,  sat_wcl_lo_es = 0;

    /* Keep doors/switches (special lines) textured even in Potato walls, so they
       stay readable against the flat-shaded plain walls. */
    sat_wall_textured = (curline->linedef->special != 0);
    /* SATURN PERF (step 2): a plain opaque wall in Potato mode is drawn as one
       solid colour by rp_exec_col (it reads cm->f3 + cm->cmap, NEVER cm->src) ->
       skip R_GetColumn (the memory-bound texture composite = the bulk of wall-prep
       "Bp") and the per-column dc_iscale division.  wall_solid matches the
       executor's solid test exactly (cm->unused = in_masked||sat_wall_textured,
       and in_masked is 0 during opaque wall generation). */
    wall_solid = sat_potato_walls && !sat_wall_textured && !rp_disabled;

    /* texture ROW (v) at a wall's top/bottom screen y, so the platform maps the right
       vertical SUBRANGE of the texture (charAddr/height) instead of stretching the whole
       texture onto the band (the "vertical squish").  ~constant across the seg, so compute
       it at x1 (rw_scale = scale1 here). */
#define SAT_VROWS(texmid, ytop, ybot, v0o, v1o) do { \
	unsigned int _is = 0xffffffffu / (unsigned int)rw_scale; \
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
	if (mdu < 0) mdu = -mdu;
	if (mdu < 1) mdu = 1;
	magnified = (sx > mdu * SAT_WALL_CPU_MAG);

	if (midtexture && !curline->backsector)
	{
	    int s1 = (bottomfrac - topfrac) >> HEIGHTBITS;
	    int s2 = ((bottomfrac + bottomstep * n) - (topfrac + topstep * n)) >> HEIGHTBITS;
	    int s = s1 > s2 ? s1 : s2;
	    int span_close = (s > SAT_WALL_CPU_SPAN);   /* kept for the FBK counter */
	    /* SPAN clamp DISABLED (v0 near-wall affine perspective warp = "moche", owner 2026-07-02):
	       span-close one-sided walls stay on the CPU (shipping).  sat_wall_clamp now drives ONLY the
	       BELOW-FLOOR cut (Phase 1b).  Revisit SPAN only with finer near-tile u-subdivision. */
	    int cpu_now = span_close || magnified;
	    int idx = (int)(curline - segs);
	    unsigned char *st = (idx >= 0 && idx < SAT_SEG_MAX) ? &sat_seg_cpu[idx] : 0;
	    sat_v1_mid = (s < SAT_WALL_CPU_V1) && !magnified;  /* magnified -> NO VDP1 quad (it squishes) */
#if SAT_WALL_SUBDIV
	    if (magnified && !span_close) { sat_v1_mid_sub = 1; cpu_now = 0; }  /* keep on VDP1 via perspective subdivision (emit site), not CPU */
#endif
	    if (cpu_now)
	    {
		if (!sat_v1_mid) {   /* Phase-0: count only the FULLY-CPU tiers (not the [SPAN,V1] VDP1-also pre-warm) */
		    if (magnified)                  sat_fb_mag_t++;                             /* squish -> clamp can't fix   */
		    else if (span_close)            { sat_fb_clamp_t++; sat_fb_px += s * sx; }  /* pure span  -> clampable     */
		}
		sat_sw_mid = 1;  if (st) *st = 2;   /* CPU draws (close/magnified); arm 2 CPU exit-frames */
	    }
	    else
	    {
		sat_sw_mid = (st && *st) ? 1 : 0;   /* CPU also draws for 2 frames after exit */
		if (st && *st) (*st)--;
	    }
	}
	else if (curline->backsector)
	{
	    /* doors: upper/lower are SHORT bands -> the span test never trips them even up close, but
	       they squish at the edge when magnified.  Route the whole seg (both tiers) to CPU on span
	       OR magnification, with the same per-seg 3-frame exit handoff as the one-sided mid. */
	    int cpu_up = 0, cpu_lo = 0;
	    if (toptexture)
	    {
		int s1 = (pixhigh - topfrac) >> HEIGHTBITS;
		int s2 = ((pixhigh + pixhighstep * n) - (topfrac + topstep * n)) >> HEIGHTBITS;
		int s = s1 > s2 ? s1 : s2;
		cpu_up = (s > SAT_WALL_CPU_SPAN);
	    }
	    if (bottomtexture)
	    {
		int s1 = (bottomfrac - pixlow) >> HEIGHTBITS;
		int s2 = ((bottomfrac + bottomstep * n) - (pixlow + pixlowstep * n)) >> HEIGHTBITS;
		int s = s1 > s2 ? s1 : s2;
		cpu_lo = (s > SAT_WALL_CPU_SPAN);
	    }
	    {
		int cpu_now = cpu_up || cpu_lo || magnified;
		if (cpu_up) sat_fb_clamp_t++;                        /* Phase-0: clampable span (upper door tier) */
		if (cpu_lo) sat_fb_clamp_t++;                        /* Phase-0: clampable span (lower door tier) */
#if !SAT_WALL_SUBDIV
		if (magnified && !cpu_up && !cpu_lo) sat_fb_mag_t++; /* Phase-0: magnified-only door residue      */
#endif
		int idx = (int)(curline - segs);
		unsigned char *st = (idx >= 0 && idx < SAT_SEG_MAX) ? &sat_seg_cpu[idx] : 0;
		int overlap = 0;
		if (cpu_now) { if (st) *st = 2; }       /* arm 2 CPU exit-frames */
		else if (st && *st) { overlap = 1; (*st)--; }  /* CPU also draws 2 frames after exit */
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
    if (sat_wall_hook && SAT_WALL_VDP1_OK && midtexture && !curline->backsector && rw_stopx > rw_x && (sat_v1_mid || sat_v1_mid_sub))
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
	    { /* entirely below the floor -> cull (neither VDP1 nor CPU draws it) */ }
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
#if SAT_WALL_SUBDIV
	else if (sat_v1_mid_sub && !sat_v1_mid)   /* normal path only (pot2 force-sets sat_v1_mid=1 -> single quad) */
	{
	    /* PERSPECTIVE SUBDIVISION of a magnified wall: split [rw_x, rw_stopx-1] into N narrow sub-segs,
	       RE-SAMPLE u at each endpoint via the true tangent formula (perspective-correct) + EXACT linear
	       yl/yh -> no squish, no swim.  Sub-segs abut column-for-column (no seam).  N ~ columns-per-texel
	       (the same magnification quantity), capped.  Whole wall -> CPU on a bank-full reject. */
	    int sx = rw_stopx - rw_x, mdu = u2 - u1; if (mdu < 0) mdu = -mdu; if (mdu < 1) mdu = 1;
	    int N = 1 + sx / mdu; if (N < 2) N = 2; if (N > SAT_WALL_SUBDIV_MAX) N = SAT_WALL_SUBDIV_MAX;
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
	    /* SATURN: same floor handling as the other tiers -- cull an upper (toptexture) wall
	       entirely below the RBG0 floor line; hand a partially-below one to the CPU (clips to
	       floorclip).  (Rare for a ceiling-side tier, but completes the set.) */
	    if (sat_wall_clamp && !sat_wall_span_visible(yl1, yl2, yh1, yh2))
		{ /* invisible at every column -> claim nothing (see the mid tier) */ }
	    else if (sat_floor_punch_here() && yl1 >= floorclip[rw_x] && yl2 >= floorclip[rw_stopx - 1])
		{ /* entirely below the floor -> cull */ }
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
		int N = 1 + sx / mdu; if (N < 2) N = 2; if (N > SAT_WALL_SUBDIV_MAX) N = SAT_WALL_SUBDIV_MAX;
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
	    /* SATURN: same floor handling as the one-sided wall -- cull a lower (bottomtexture)
	       wall entirely below the floor; hand a partially-below one to the CPU, which clips
	       each column to floorclip (no VDP1 bleed-through, no squish). */
	    if (sat_wall_clamp && !sat_wall_span_visible(yl1, yl2, yh1, yh2))
		{ /* invisible at every column -> claim nothing (see the mid tier) */ }
	    else if (sat_floor_punch_here() && yl1 >= floorclip[rw_x] && yl2 >= floorclip[rw_stopx - 1])
		{ /* entirely below the floor -> cull */ }
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
		int N = 1 + sx / mdu; if (N < 2) N = 2; if (N > SAT_WALL_SUBDIV_MAX) N = SAT_WALL_SUBDIV_MAX;
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

#if SAT_WALL_EDGE_FILL
    int sat_ef_x0 = rw_x, sat_ef_x1 = rw_stopx - 1;   /* seg screen extent, for the edge-fill margin */
#endif
    for ( ; rw_x < rw_stopx ; rw_x++)
    {
#if SAT_WALL_EDGE_FILL
	/* software-draw the wall's first/last SAT_WALL_EDGE_FILL columns even when VDP1 owns it -> the NBG1
	   strips align with the lagged mask and fill the horizontal rotation decrochage.  sat_wall_skip==0
	   (DoomJo / VDP1-off) -> is_edge is always 0 -> inert. */
	int is_edge = sat_wall_skip && (rw_x - sat_ef_x0 < SAT_WALL_EDGE_FILL
	                             || sat_ef_x1 - rw_x < SAT_WALL_EDGE_FILL);
#else
	enum { is_edge = 0 };
#endif
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
		else
		    dc_source = R_GetColumn(midtexture,texturecolumn);
		colfunc ();
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
			else
			    dc_source = R_GetColumn(toptexture,texturecolumn);
			colfunc ();
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
			else
			    dc_source = R_GetColumn(bottomtexture,
						    texturecolumn);
			colfunc ();
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

