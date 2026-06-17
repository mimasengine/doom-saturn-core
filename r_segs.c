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
extern int  sat_wall_color;
extern int  sat_wall_textured;
extern int  R_WallPotatoColor (int tex);

/* SATURN: hand one-sided (solid) walls to a platform VDP1 world renderer.  NULL on
   DoomJo / when unused -> normal software wall.  Called per seg with the wall's 4
   SCREEN corners + texture + light (corners from the same topfrac/bottomfrac the
   software loop steps).  Step 2 = validate the real-wall projection + affine warp. */
void (*sat_wall_hook)(int x1, int yl1, int yh1, int x2, int yl2, int yh2,
                      int texnum, int u1, int u2, int v0, int v1,
                      const unsigned char *cmap) = 0;

/* SATURN: when the VDP1 world renderer owns the one-sided walls, skip the software
   midtexture column draw (R_GetColumn + colfunc) for them -> measure the perf the
   VDP1 path buys back.  The ceiling/floor clip + visplane marking still run (floors,
   ceilings, sprite occlusion stay correct).  0 on DoomJo / when unused. */
int sat_wall_skip = 0;

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

/* EXTRA CPU FRAMES on EXIT (Romain): when a one-sided wall leaves the CPU path (moves away,
   CPU->VDP1), the VDP1 presents several frames late -> a sky gap at the seam.  So for N frames after
   it stops being CPU, the CPU ALSO draws it (overlap) while VDP1 catches up.  N=2.  Per-seg (not a
   span band): only the wall that actually transitions pays, those frames only -- economical.  Keyed
   by the seg index (seg_t's are
   static level data -> stable pointer); 1 byte = CPU-exit-frame countdown. */
#define SAT_SEG_MAX 4096
static unsigned char sat_seg_cpu[SAT_SEG_MAX];

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
    if (sat_wall_hook && sat_wall_skip && rw_stopx > rw_x)
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
	    int cpu_now = (s > SAT_WALL_CPU_SPAN) || magnified;
	    int idx = (int)(curline - segs);
	    unsigned char *st = (idx >= 0 && idx < SAT_SEG_MAX) ? &sat_seg_cpu[idx] : 0;
	    sat_v1_mid = (s < SAT_WALL_CPU_V1) && !magnified;  /* magnified -> NO VDP1 quad (it squishes) */
	    if (cpu_now)
	    {
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
		int idx = (int)(curline - segs);
		unsigned char *st = (idx >= 0 && idx < SAT_SEG_MAX) ? &sat_seg_cpu[idx] : 0;
		int overlap = 0;
		if (cpu_now) { if (st) *st = 2; }       /* arm 2 CPU exit-frames */
		else if (st && *st) { overlap = 1; (*st)--; }  /* CPU also draws 2 frames after exit */
		/* VDP1 owns a tier only when it is neither magnified nor span-close (so it never draws the
		   squishing quad); the CPU draws it when close/magnified OR during the exit overlap. */
		sat_v1_up = (toptexture    && !magnified && !cpu_up) ? 1 : 0;
		sat_v1_lo = (bottomtexture && !magnified && !cpu_lo) ? 1 : 0;
		sat_sw_up = (toptexture    && (cpu_up || magnified || overlap)) ? 1 : 0;
		sat_sw_lo = (bottomtexture && (cpu_lo || magnified || overlap)) ? 1 : 0;
	    }
	}
    }

    /* SATURN VDP1 world renderer (Step 2): one-sided (solid) walls -> the platform as
       a quad.  The 4 screen corners come from the same topfrac/bottomfrac the loop
       below steps; midtexture = the full-height wall texture. */
    if (sat_wall_hook && midtexture && !curline->backsector && rw_stopx > rw_x && sat_v1_mid)
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
	sat_wall_hook (rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2, midtexture, u1, u2, v0, v1, cm);
    }

    /* SATURN VDP1 world renderer: two-sided walls -> upper (toptexture) + lower
       (bottomtexture) quads into the SAME painter list as the one-sided walls, so a
       NEAR two-sided frame correctly draws over a FAR one-sided wall seen through the
       opening (the gap between upper/lower has no texture -> the far wall shows there). */
    if (sat_wall_hook && curline->backsector && rw_stopx > rw_x)
    {
	int n   = rw_stopx - 1 - rw_x;
	int a1 = (rw_centerangle + xtoviewangle[rw_x])        >> ANGLETOFINESHIFT;
	int a2 = (rw_centerangle + xtoviewangle[rw_stopx - 1]) >> ANGLETOFINESHIFT;
	int u1 = (rw_offset - FixedMul(finetangent[a1], rw_distance)) >> FRACBITS;
	int u2 = (rw_offset - FixedMul(finetangent[a2], rw_distance)) >> FRACBITS;
	int _li = rw_scale >> LIGHTSCALESHIFT;   /* distance-correct light (was fixed mid-level) */
	if (_li >= MAXLIGHTSCALE) _li = MAXLIGHTSCALE - 1; else if (_li < 0) _li = 0;
	const lighttable_t *cm = walllights[_li];
	if (toptexture && sat_v1_up)   /* ceiling -> top of the opening */
	{
	    int yl1 = (topfrac + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yl2 = (topfrac + topstep * n + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yh1 = pixhigh >> HEIGHTBITS;
	    int yh2 = (pixhigh + pixhighstep * n) >> HEIGHTBITS;
	    int v0, v1; SAT_VROWS(rw_toptexturemid, yl1, yh1, v0, v1);
	    sat_wall_hook (rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2, toptexture, u1, u2, v0, v1, cm);
	}
	if (bottomtexture && sat_v1_lo)   /* bottom of the opening -> floor */
	{
	    int yl1 = (pixlow + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yl2 = (pixlow + pixlowstep * n + HEIGHTUNIT - 1) >> HEIGHTBITS;
	    int yh1 = bottomfrac >> HEIGHTBITS;
	    int yh2 = (bottomfrac + bottomstep * n) >> HEIGHTBITS;
	    int v0, v1; SAT_VROWS(rw_bottomtexturemid, yl1, yh1, v0, v1);
	    sat_wall_hook (rw_x, yl1, yh1, rw_stopx - 1, yl2, yh2, bottomtexture, u1, u2, v0, v1, cm);
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

    for ( ; rw_x < rw_stopx ; rw_x++)
    {
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
	if (segtextured && (sw_draws || maskedtexture))
	{
	    // calculate texture offset
	    angle = (rw_centerangle + xtoviewangle[rw_x])>>ANGLETOFINESHIFT;
	    texturecolumn = rw_offset-FixedMul(finetangent[angle],rw_distance);
	    texturecolumn >>= FRACBITS;
	    /* SATURN PERF (lever C): the lighting lookup + the per-column dc_iscale divide
	       feed only the software column draw (colfunc).  When VDP1 owns every tier this
	       seg (sw_draws == 0) no colfunc runs -> skip them; the divide is the costly bit. */
	    if (sw_draws)
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
	    if (!sat_wall_skip || sat_sw_mid)
	    {
		dc_yl = yl;
		dc_yh = yh;
		dc_texturemid = rw_midtexturemid;
		if (wall_solid)
		    sat_wall_color = R_WallPotatoColor(midtexture);
		else
		    dc_source = R_GetColumn(midtexture,texturecolumn);
		colfunc ();
	    }
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
		    if (!sat_wall_skip || sat_sw_up)   /* VDP1 owns it (unless close/transition) */
		    {
			dc_yl = yl;
			dc_yh = mid;
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
		    if (!sat_wall_skip || sat_sw_lo)   /* VDP1 owns it (unless close/transition) */
		    {
			dc_yl = mid;
			dc_yh = yh;
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
	    ds_p->maskedtexturecol = maskedtexturecol = lastopening - rw_x;
	    lastopening += rw_stopx - rw_x;
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
	memcpy (lastopening, ceilingclip+start, 2*(rw_stopx-start));
	ds_p->sprtopclip = lastopening - start;
	lastopening += rw_stopx - start;
    }
    
    if ( ((ds_p->silhouette & SIL_BOTTOM) || maskedtexture)
	 && !ds_p->sprbottomclip)
    {
	memcpy (lastopening, floorclip+start, 2*(rw_stopx-start));
	ds_p->sprbottomclip = lastopening - start;
	lastopening += rw_stopx - start;	
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

