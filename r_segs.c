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
    int sat_gc_save = sat_gc_site;
    sat_gc_site = 3;                  /* SATURN row 16 `GCS`: grate columns, invisible in `g` */

    // Calculate light table.
    // Use different light tables
    //   for horizontal / vertical / diagonal. Diagonal?
    // OPTIMIZE: get rid of LIGHTSEGSHIFT globally
    curline = ds->curline;
    frontsector = SEG_FRONTSECTOR(curline);
    backsector = SEG_BACKSECTOR(curline);
    texnum = texturetranslation[SEG_SIDEDEF(curline)->midtexture];
	
    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

    if (SEG_V1(curline)->y == SEG_V2(curline)->y)
	lightnum--;
    else if (SEG_V1(curline)->x == SEG_V2(curline)->x)
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
    if (SEG_LINEDEF(curline)->flags & ML_DONTPEGBOTTOM)
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
    dc_texturemid += SEG_SIDEDEF(curline)->rowoffset;
			
    if (fixedcolormap)
	dc_colormap = fixedcolormap;
    
    // draw the columns
    for (dc_x = x1 ; dc_x <= x2 ; dc_x++)
    {
	/* ⚠ SATURN 2026-08-17 -- THE GRATE MASKING WAS HERE AND IS WITHDRAWN, on the owner's call
	   (*"annule le masquage pour l'instant, on y reviendra plus précisément plus tard"*).
	   It dropped every masked-midtexture column covered by a NEARER VDP1 sprite -- correct in
	   principle, since NBG1 sits above the VDP1 sprite layer and the two cannot z-sort, so a grate
	   BEHIND a monster shows through it.  What made it too blunt: the test used the sprite's
	   BOUNDING BOX, and a monster does not fill its box, so the grate also vanished in the
	   transparent margins beside it.  A precise version has to work per ROW, not per column.
	   Its publisher (r_things.c sat_v1spr_sc[]) went with it, so nothing is left half-wired. */
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
    sat_gc_site = sat_gc_save;   /* SATURN row 16 `GCS` */
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

/* 🔴 SATURN 2026-08-26 -- `>> HEIGHTBITS` IS A FUNCTION CALL ON THE SH-2.
   The SH7604 has no dynamic shift (only 1, 2, 8 and 16), so GCC lowers a constant >>12 to
   ___ashiftrt_r4_12: a jsr into twelve chained `shar` plus an rts.  The disassembly of the
   SHIPPED r_segs.o carried TWELVE relocations to that helper, up to FOUR of them per column of
   the hottest loop in the renderer (yl, yh and the two tier mids) and ~30 more per SEG in the
   routing preamble.
   Worse than the call overhead: the helper's ABI PINS its operand to r4, so every use forces a
   shuffle in and out of one fixed register inside a loop the same disassembly shows to be
   register-starved -- 42 stack-address formations (`mov #N,rX; add r15,rX`) in a single body,
   three instructions just to reach one variable.  Inline, it is the SAME twelve instructions
   minus the jsr, the rts, the delay slot and the r4 straitjacket.
   ⚠ (x << 4) >> 16 -- two instructions via swap.w/exts.w -- WAS CONSIDERED AND REJECTED.  It
   needs |x| < 2^27, and topfrac reaches ~5e8 on a close tall wall (centeryfrac -
   FixedMul(worldtop, rw_scale), with rw_scale clamped at 64.0 by R_ScaleFromGlobalAngle).
   Exactness is not negotiable here: yl/yh are SCREEN ROWS and the clip compares below read the
   true value, so a wrapped one picks the wrong branch -- a mis-clipped wall, not a rounding.
   Non-SH builds keep the plain shift, so the C is the definition and the asm is the SH-2 spelling
   of it.
   [!] ROUND 2, 2026-08-26 -- THE PREAMBLE SITES WERE MISSED, AND THIS NOTE SAID SO.  Round 1
   converted the SIX sites inside the per-column loop and stopped there; the paragraph above had
   already counted "~30 more per SEG in the routing preamble" and nothing was done with the
   sentence.  The 4p photo is what made that matter: at c1679/d187 a quadrant averages 9,0 wall
   columns per drawseg against 17,5 in 1p (Doom's FOV is nailed to 90 deg by centerxfrac, so a
   160-wide viewport shows the SAME world as a 320-wide one), which puts 51 % of the 4p wall bill
   in per-SEG fixed cost -- row-4 hd8,2 + pr11,1 over d187 = 103 us a seg against 11 us a column.
   Round 1 optimised the half that 4p amortises BEST.
   35 sites converted here: 4 in sat_wall_try_edge (called at :1963/:1987, i.e. inside `pr`),
   30 in R_RenderSegLoop's routing preamble, 1 in the distance-LOD height.  Six to twelve execute
   per seg depending on the tier mix (mid vs top+bottom, plus the squish guards and the sub-seg
   splitters).
   [!] AND HERE IS THE ARITHMETIC I GOT WRONG WHEN I SHIPPED IT.  I sized this at "180-360 cycles a
   seg" -- that is the cost of the SHIFT SEQUENCES, and they do not go away: the inline spelling is
   the same twelve `shar`.  What the inline removes is ONLY the call envelope -- jsr + delay slot +
   rts + delay slot + the two r4 marshalling movs, about 8 cycles out of ~20 -- so the saving is
   6..12 x 8 = 50-100 cycles a seg, x187 segs = **0,3-0,6 ms on a 116 ms frame**, not the 1-2 ms I
   quoted.  Cost of a lever is not the same number as the SIZE of what the lever touches, and I
   presented one as the other ([[budget-before-mechanism]]).
   YMIR 4p TNT, same spot, identical fingerprint (d187 c1679 em9,7 hd8,2 tl0,6 Bw12,8 R88 MST116):
       before   hd 8,2   pr 11,1   lp 18,7   Bp 40,1
       after    hd 8,2   pr 11,3   lp 17,9   Bp 39,4
   `pr` DID NOT MOVE and `lp` did, which is the opposite of the prediction -- every converted site
   is inside the `pr` bracket.  The believable reading is that R_RenderSegLoop is ONE function with
   ONE register allocator: unpinning r4 in the preamble changed allocation in the loop as well,
   which is exactly the second mechanism the paragraph above claims and the one nobody can aim.
   ⚠ SO: KEEP IT, BUT DO NOT SPEND CONSOLE TIME ON IT.  The change is strictly fewer instructions
   for a bit-identical result and costs 133 bytes of pool; the expected 0,3-0,6 ms sits UNDER the
   console noise floor measured the same day from the wall-fill videos (1,1-1,4 ms of spread on
   `Bp` between two same-configuration captures, see sat_wallfill_min).  A one-shot hardware A/B
   cannot resolve it, so asking for one would burn a console session to learn nothing. */
#if HEIGHTBITS != 12
#error "SAT_SHR12 is the SH-2 spelling of >>12 -- retune it if HEIGHTBITS moves"
#endif
#ifdef __sh__
static inline int SAT_SHR12 (int x)
{
    __asm__ ("shar %0; shar %0; shar %0; shar %0; shar %0; shar %0;"
	     " shar %0; shar %0; shar %0; shar %0; shar %0; shar %0"
	     : "+r" (x) : : "t");
    return x;
}
#else
#define SAT_SHR12(x) ((int)(x) >> 12)
#endif
#define HEIGHTUNIT		(1<<HEIGHTBITS)

/* SATURN Potato walls: when enabled, the wall recorder paints each opaque wall
   column with the texture's dominant colour (sat_wall_color), set here per wall
   section so the whole wall is one continuous hue.  Off by default. */
extern int  sat_potato_walls;
extern int  sat_wall_nocpu;     /* SATURN: banded/flat VDP1 modes skip the close-wall CPU fallback */
extern int  sat_wall_color;
/* SATURN per-frame texture LOAD BUDGET -- see the block above the column loop in R_StoreWallRange.
   budget 0 = OFF = every texture faults in on sight (the pre-2026-08-06 behaviour).

   2026-08-07 -- the budget counts MILLISECONDS OF DISC, not reads, and it is ON by default.
   Two defects, both found by asking what the Ymir numbers are worth on a console:
     * it counted READS.  A read is not a unit of cost: it is ~35 ms under Ymir's CD model and a
       seek plus a 150 KB/s transfer on a console, so the same `4` bounded the frame's stall at
       two very different places.  The thing we actually want bounded is the STALL, so spend the
       clock (core w_cd_ms10, fed by the platform's per-command FRT timing) and let each medium
       price its own reads.  A medium with no latency never advances it, so the budget goes inert
       instead of throttling a port that has nothing to throttle.
     * it defaulted to 0 = OFF, and its ONLY writer was the R+X chord.  Every gate added on
       2026-08-06/07 -- walls, flats, sprites, and the VDP1 emit that took `P` from 163 ms to
       7 -- was therefore DEAD in a default boot; the owner's captures only showed them working
       because he had pressed R+X three times.  A protection nobody arms is not a protection.

   Overshoot is by construction: a read's price is known only after paying it, so the frame can
   exceed the budget by one read (~35 ms here).  That residue is exactly what an ASYNC read would
   remove, and it is the reason R2.3 stays on the roadmap rather than being closed by this. */
int sat_tex_load_budget = 20;   /* ms of medium wall-clock allowed per frame, 0 = off (pad R+X) */
int sat_tex_load_spent  = 0;    /* tenths of a ms spent this frame (read off the clock, not counted) */
static unsigned int sat_tex_load_mark = 0;   /* w_cd_ms10 at the start of this frame */
extern unsigned int w_cd_ms10;               /* core w_wad.c */
/* SATURN 2026-08-08: 1 once the budget has ACTUALLY refused something on this run.  It gates the
   dominant-colour PRIMING (R_WallPotatoColor / R_FlatPotatoColor), which exists solely so a
   budget-flattened surface has a colour instead of neutral grey.  Measured cost of priming
   eagerly: R_WallPotatoColor walks every other column of the WHOLE texture through R_GetColumn,
   and the overlay caught it at up to ~34% of a peak Bp frame -- on a disc where `lb20:0/0/0` says
   the budget never flattened anything, so every one of those walks produced a value nobody read.
   It only became reachable when the budget was armed by default on 2026-08-07 (before that,
   sat_wall_io_flat returned on its first line).  Sticky, never cleared: once the medium has proved
   slow, keep priming -- the eager behaviour is the safe one, it is only the FREE case we owe. */
int sat_budget_refused = 0;

/* Refill: called once per rendered frame from R_ClearDrawSegs (r_bsp.c). */
void R_LoadBudgetFrame (void)
{
    sat_tex_load_mark  = w_cd_ms10;
    sat_tex_load_spent = 0;
}

/* 1 = this frame can still afford to fault something in off the medium.
   Budget off -> always 1, so every call site degrades to the pre-budget behaviour. */
int R_LoadBudgetLeft (void)
{
    if (!sat_tex_load_budget) return 1;
    sat_tex_load_spent = (int)(w_cd_ms10 - sat_tex_load_mark);
    if (sat_tex_load_spent < sat_tex_load_budget * 10) return 1;
    sat_budget_refused = 1;   /* the ONE funnel every refusal passes through -- see below */
    return 0;
}
int sat_wall_flat_io    = 0;    /* tiers drawn flat for want of residency  (~1 s window)       */
int sat_wall_flat_nocol = 0;    /* ...of which we had no cached colour either (~1 s window)    */
/* SATURN 2026-08-15: SIZE LOD.  A tier whose SCREEN AREA (columns x pixel height) falls under this
   threshold draws flat in its dominant colour instead of paying R_GenerateComposite.  0 = OFF,
   cycled live by pad L+B.  Units are PIXELS -- the name kept `_scale` from the first, wrong version
   that tested rw_scale (= 1/distance) and flattened big mid-distance walls.  Row 22 `Lo`. */
int sat_wall_lod_hits  = 0;     /* tiers flattened by distance this ~1 s window                 */
/* SATURN 2026-08-16 -- the LOD's DISTANCE FLOOR, in MAP UNITS (rw_distance >> FRACBITS).  A tier
   nearer than this is never flattened however small it scores, so the LOD can only ever eat the
   background.  384 ~ three player-widths: inside that, a sliver is something you are standing next
   to.  `sat_wall_lod_near` counts tiers the floor rescued (~1 s window, row 21 `nr`). */
int sat_lod_mindist    = 384;
/* sat_wall_lod_near REMOVED 2026-08-26 -- dead work: row-21 `nr` was never actually added, so
   this counted once per LOD-rescued seg for nobody. */
/* SATURN LOD GOVERNOR (controller in r_parallel.c, reported on row 21).  `sat_lod_eff` is the
   threshold the renderer ACTUALLY uses.  One writer (the governor), one reader (below).  There is
   no manual rung and no chord any more -- the governor is unconditional (owner 2026-08-16). */
int sat_lod_eff        = 0;
int sat_lod_auto_step  = 0;     /* 0..3, index into the governor's rung table                   */
/* SATURN 2026-08-16 -- drawseg budget, the COUNT half of the `B` axis (see R_StoreWallRange).
   `sat_seg_budget` = textured segs allowed per view, 0 = unbounded.  `sat_seg_count` is reset
   per view in RP_BeginFrame; `sat_seg_budget_cut` is the ~1 s window tally on row 21 `sb`. */
int sat_seg_budget     = 0;
int sat_seg_count      = 0;
int sat_seg_budget_cut = 0;
/* SIGNED integral of (render - target) in tenths of a ms: >0 = behind, <0 = ahead.  A single
   accumulator with NO cross-reset -- two counters that reset each other could never fill on a
   bimodal load, which is why the governor never fired before 2026-08-16. */
int sat_gov_debt       = 0;
/* SATURN 2026-08-15 -- the governor became MULTI-AXIS: it triggers on the WHOLE frame and then
   degrades whichever phase dominated it.  `sat_gov_axis` is the letter row 21 prints (B/P/M/-),
   `sat_gov_p_step` is the plane axis (0 = untouched, 1 = at least LD, 2 = FLAT) applied by
   sat_apply_mode() as a FLOOR over the owner's own SQ setting -- it can only ever degrade, never
   silently improve past what he chose.  `_dirty` asks the platform to re-apply. */
int sat_gov_axis       = '-';
int sat_gov_p_step     = 0;
int sat_gov_p_dirty    = 0;
/* Neutral index used when a texture has never been resident, so its dominant colour was never
   computed and CANNOT be without loading it.  Mid-grey in the Doom palette; dc_colormap still
   shades it by distance/sector light, so it reads as a lit surface, not a hole. */
#define SAT_WALL_FLAT_UNKNOWN 100

extern int  R_TextureIOFree (int tex);
extern int  R_WallPotatoColorPeek (int tex);
extern int  R_WallPotatoSeed (int tex);   /* SATURN: one texel from the first patch, no composite */
extern int  R_WallPotatoColor (int tex);
/* Read-only, never allocates, NULL for any doubt -- the ONLY column fetch legal off the master or
   across a phase boundary.  See the long note at its definition in r_data.c. */
extern const byte* R_GetColumnCached (int tex, int col);

/* 1 = draw this tier FLAT because texturing it would hit the disc and the frame's budget is spent. */
static int sat_wall_io_flat (int tex)
{
    if (!sat_tex_load_budget)      return 0;   /* feature off */
    if (R_TextureIOFree (tex))     return 0;   /* free -> draw it properly, budget untouched */
    if (R_LoadBudgetLeft ())
    {
	/* Pay for it (BSP order => nearest walls win).  We are faulting the texture in anyway, so
	   compute its DOMINANT COLOUR in the same breath: R_WallPotatoColor is only ever called by
	   the potato mode, which is off in normal play, so without this the colour cache stays EMPTY
	   and every flattened wall falls back to neutral grey -- measured `nocol` = 100% of `flat`
	   on the owner's TNT MAP11 captures.  Priming here makes every later fallback for this
	   texture exact, at the cost of one extra pass over a texture we are already loading.
	   No `spent++`: the clock does the counting, and R_WallPotatoColor's own read is on it.
	   LAZY since 2026-08-08 (see sat_budget_refused): that "one extra pass" is a walk of every
	   other column of the whole texture through R_GetColumn, and until the budget has refused
	   something its product is never read.  Costs one neutral-grey frame at the first refusal. */
	/* SATURN 2026-08-15: SEED, not WALK -- the second instance of the same defect the owner's
	   potato question exposed.  R_WallPotatoColor walks every other column through R_GetColumn and
	   BUILDS THE COMPOSITE (31 ms), and it was doing that here purely to prime a colour that might
	   later be used for a flat fallback.  R_WallPotatoSeed takes one texel from the first patch
	   through a ~1,3 KB prefix decode instead: ~0,4 ms, no composite.  The exact dominant colour
	   still lands the first time the texture is genuinely drawn close up. */
	if (sat_budget_refused) R_WallPotatoSeed (tex);
	return 0;
    }
    sat_wall_flat_io++;
    return 1;
}

/* Flat colour for a flattened tier, resolved ONCE PER TIER (the draw sites below live INSIDE the
   column loop -- calling this there counted `nocol` per COLUMN, which is why it read larger than
   `flat`, and paid a peek per column for nothing).
   SATURN 2026-08-08 -- THE GREY WALLS.  Owner: *"je vois parfois les mauvaises textures... des
   textures grises au lieu des vertes"*, then the decisive clarification: *"ça arrive quand l'écran
   est surchargé de murs"*.  Grey IS `SAT_WALL_FLAT_UNKNOWN`: the tier drew flat with the neutral
   index because nothing had primed its dominant colour.  The peek-only rule dates from when the
   budget counted READS and computing a colour meant a possible ~42 ms disc fault -- the owner's
   own 2026-08-06 catch, *"on a pas la couleur si on ne lit pas le cd"*.  But on HIS trigger there
   is no disc at all: a wall-dense view exhausts the VDP1 wtex slots, `wall_tex_resolve` returns
   -1, the tier degrades to a flat quad -- and the texture is RESIDENT.  Walking it is pure CPU and
   memoised for the level.  So compute it whenever we can look without paying disc, and keep grey
   for the one case it was written for: a texture that really is not there yet. */
static int sat_wall_flat_color (int tex)
{
    int c = R_WallPotatoColorPeek (tex);
    if (c < 0)
    {
	/* SATURN 2026-08-15: SEED, never WALK.  This used to call R_WallPotatoColor whenever IO was
	   free -- but "free" means no DISC, and the real bill is R_GenerateComposite: that walk faults
	   the whole texture in through R_GetColumn and builds the composite, measured at 31 ms
	   (row-20 `k`).  Paying it to colour a tier we have just decided NOT to texture is precisely
	   the work the distance LOD exists to remove.  R_WallPotatoSeed reads ONE texel out of the
	   first patch through a ~1,3 KB prefix decode: no composite, no full decode, ~0,4 ms once per
	   texture, and it upgrades itself to the exact dominant colour the first time the texture is
	   genuinely drawn close up. */
	c = R_WallPotatoSeed (tex);
	if (c < 0)
	    { c = SAT_WALL_FLAT_UNKNOWN; sat_wall_flat_nocol++; }
    }
    return c;
}
extern int  sat_wall_paint;   /* SATURN debug paint (r_data.c): bit1 = CPU walls flat red */
extern int  sat_dbg_overlay_mode;   /* r_parallel.c: 0 full / 1 fps-only / 2 off -- gates the per-column probes */
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
/* (sat_fb_edge_w REMOVED 2026-08-26 -- denominator of a rate nothing prints.)                   */
/* (sat_fb_edge_b[] REMOVED 2026-08-26 -- four bail counters, no reader.)                        */

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
int sat_fb_mag_t = 0, sat_fb_starve_t = 0;   /* sat_fb_clamp_t / sat_fb_px REMOVED 2026-08-26:
                                                their only readers (fb_cur_*, fb_pk_clamp, fb_pk_px)
                                                were themselves write-only.  mag/starve DO print. */
/* SATURN 2026-08-24 -- THE SPAN LEVER'S ACTION COUNTER.  Free-running and GOVERNOR-OWNED, exactly
   like sat_gov_act_w/l and for the same reason: sat_fb_clamp_t above is a PER-FRAME tally the
   platform zeroes on its own schedule, so a controller sampling it would read "no action" at
   random.  Incremented once per tier the CPU takes BECAUSE OF THE SPAN -- which is precisely what
   raising the span would hand back to VDP1.  If it does not move while the span governor's probe
   runs, that direction has nothing to buy and is convicted without waiting for the timing. */
unsigned int sat_gov_act_s = 0;
/* SATURN 2026-08-24 (owner) -- SPECIALS BLOCKER, part 2: the ROUTE.
   Part 1 (2026-08-20, `keep_tex` in R_RenderSegLoop) keeps a special TEXTURED once it is on
   the CPU path.  It said nothing about the VDP1 path, where `special` only set the INTENT:
   the platform's wall budget marked the wall textured and then dropped it to a FLAT whenever
   it lost the surplus race or no texture slot could be freed.  A flat switch or door face is
   invisible as an interactive element -- "ca rend la progression dans le jeu impossible".
   The contract is: VDP1 textured if possible, CPU textured otherwise, NEVER flat.
   The PLATFORM owns this table (dg_saturn.cxx, wall flush pass 0): it sets the bit for a
   texture whose special wall it could not give a VDP1 slot, and clears the table at level
   load.  A set bit routes every tier carrying that texture to the software renderer, where
   keep_tex forbids both flatten rules.  Hash by (texnum & 255): a collision demotes an
   innocent texture to the CPU, which is only ever slower, never wrong. */
unsigned int sat_wall_spec_cpu[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
#define SAT_SPEC_CPU(tex) (sat_wall_textured && (tex) > 0 && \
    ((sat_wall_spec_cpu[(((tex) & 255) >> 5)] >> ((tex) & 31)) & 1u))
/* (sat_fb_wclamp_t REMOVED 2026-08-26 -- it fed fb_cur_wclamp, which nothing printed.) */

/* SATURN Phase-1 wall clamp ([[wall-clamp-world-anchored]]): when set, a SPAN-close one-sided
   wall STAYS on VDP1 (clamped swim-free in wall_emit_band via the constant-z linear v->y map)
   instead of the CPU software fallback -- the Option-2 lever.  MAGNIFIED walls still go to CPU
   (the vertical clamp can't fix the horizontal squish).  Default 0 = shipping byte-identical;
   the platform sets it from the SAT_WALL_CLAMP compile flag (dg_saturn) for the HW A/B. */
int sat_wall_clamp = 0;
/* SATURN 2026-08-24 -- SUBDIVISION SKIP, NOW A GOVERNOR RUNG (owner: *"le skip subdivision devrait
   aussi etre une option du gouverneur, pas la norme"*).  1 = a magnified wall emits as ONE quad
   (it swims/squishes) instead of the perspective-subdivided N-quad path, saving both the
   SAT_VROWS divides (Bp, master CPU) and N-1 VDP1 commands.  "vaut mieux nager que ramer" -- but
   only when the frame cannot afford to row.
   Shipped 2026-08-23 as a hardcoded 1, which was wrong twice over: it had NO writer at all (so no
   live A/B, against the rule written 8 lines below), and it made the three sat_v1_*_sub emitters
   -- and with them sat_wall_subdiv_max, the whole L4 rung -- DEAD CODE.
   DEFAULT 0 = quality: the governor raises it as the FIRST step of its `B`/`w` axis
   (r_parallel.c gov_sub[]) and lowers it back on release.  Nothing else writes it. */
int sat_wall_subdiv_skip = 0;

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
/* (sat_fb_edge_t REMOVED 2026-08-26 -- the row-8 `e` rate instrumentation was cut and left it.) */

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
/* BAKED at 1 on 2026-08-26 and the variable removed (SAT_SEG_ENTRY_MAX with it).  Its pad
   L+Left shared a BYTE-IDENTICAL predicate with the lump-pin cycle, and this side only ever
   DECREMENTED, with a floor at 0 -- so the first press meant to step the pin also pinned the
   entry coverage at 0 for good, which is the pre-2026-08-02 behaviour where a wall entering the
   view shows sky for one frame.  Reviving the knob means restoring the variable at the two
   sites in R_SegEntryTag below. */
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
/* SATURN 2026-08-19: PARKED, boot default 0 (was 1).  The stale pair this repainted no longer
   exists: the platform present is now the manual VBE-timed swap (dg_saturn.cxx sat_mp_*), which
   commits the VDP1 wall list and the software picture on the same field -- the set difference
   below is empty by construction.  Owner-validated on Ymir (holes gone, +1.5-5 fps with the
   lead-fill off).  The whole mechanism stays intact and dormant (every entry point early-outs on
   sat_wall_lead_x == 0); set > 0 to revive for an A/B.  No pad chord re-arms it any more. */
int sat_wall_lead_x = 0;   /* 0 = off, else compare against the quad emitted X frames ago */
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
   has to stay on the master (that is what killed wall-prep-on-slave three times over).

   ⚠ 2026-08-17 -- I MOVED THIS DEFAULT TO 0 AND MOVED IT BACK THE SAME DAY.  The move rested on ONE
   A/B (`L1s` 5,4 fps vs `L1-` 6,9) and the owner's next four captures did not reproduce it:
   `L1s` MST 113 / 175 against `L1-` MST 192 / 185, i.e. the slave side at least as good.  Neither
   run was same-spot (the two pairs peak at different map positions), so the honest verdict is
   UNRESOLVED, not "slave loses" -- and an unresolved verdict does not get to change a default.
   What survives from that analysis, because it is read from the code and not from fps:
     - the overlap window is R_DrawPlanes ALONE (r_main.c dispatches just before it and joins just
       after) and R_DrawPlanes is ~1 ms, so the CEILING on this offload is about 1 ms;
     - the slave path does strictly more work -- it re-resolves every span through
       R_GetColumnCached at drain time, on a cache rp_aux_body purged on entry.
   What did NOT survive: I read row-12 `st` as a per-window RATE and concluded the re-resolve was
   failing for nearly every span.  `st` is CUMULATIVE and never reset (two captures show the same
   2839 with the slave path not even running), so it says nothing about the failure rate. */
int sat_lead_mode   = 1;
#define sat_lead_flat (sat_lead_mode == 2)

/* Recorded difference spans, drained by the slave.  Z_Malloc'd with the quad history (Doom heap,
   LWRAM) so the HWRAM pool pays nothing.  `tex < 0` means SOLID, colour in `col`.
   ⚠ 2026-08-09 -- THIS RECORD USED TO HOLD A RAW `dc_source` POINTER, AND THAT WAS THE
   WRONG-TEXTURE BUG.  The span is recorded during the BSP walk and drawn by the SLAVE a whole
   phase later, concurrently with the master's R_DrawPlanes; every allocation in between (the rest
   of the wall loop, then every flat the planes load) can purge the PU_CACHE block the pointer
   aimed at, and Z_Free NULLs only REGISTERED user pointers -- `src` was not one.  The freed run is
   reused immediately by the next texture, so the span drew an IN-RANGE read of a fully-built
   neighbour: a different REAL texture, changing every frame, only under zone pressure, counted by
   nothing.  Owner-confirmed by A/B on pad R+Right (row 13 `L1s` shows it, `L1-` does not).
   The fix is to store the KEY, not the address, and re-resolve at drain time through the
   allocation-free R_GetColumnCached -- which answers NULL for anything doubtful, so a purged
   column draws FLAT for one frame instead of drawing somebody else's pixels.
   `col` is therefore filled on BOTH paths now: it is the solid colour, and it is also the fallback
   when the re-resolve fails.  Same 24-byte record as before (two shorts replace one pointer). */
typedef struct { short x, yl, yh, col; short tex, tcol; const byte *cmap;
		 fixed_t iscale, texmid; } sat_leadspan_t;
#define SAT_LEADSPAN_MAX 1536
static sat_leadspan_t *sat_lead_spans;
static int sat_lead_span_n;
int sat_lead_span_drop;      /* spans the cap refused this frame -- silence would read as "nothing to do" */
int sat_lead_stale;          /* spans whose source was GONE by drain time -> drawn flat instead of
			        drawing a neighbour's pixels.  Overlay `st` on the CD row, with the other
			        gardes that must read 0.  Every one of these was a wrong texture
			        before 2026-08-09; a persistent non-zero means the purge pressure is
			        real and the lead-fill is fighting the zone, not that it is broken. */

/* Drain the recorded spans.  Runs on the SLAVE, CONCURRENTLY with the master's R_DrawPlanes, so it
   must not touch a single dc_* global (the master's sky columns use them).  Everything it needs is
   in the record; the inner loops mirror R_DrawColumn / R_DrawColumnLow with locals.
   Pixel-safe against the planes it overlaps: Doom clips every visplane to the ceilingclip/floorclip
   the wall loop just wrote, so a plane never owns a row a wall tier owns. */
/* One recorded span, drawn.  Split out of R_LeadSlaveDraw 2026-08-26 so the STREAMING drain
   below can consume the queue one entry at a time while the master is still appending to it. */
static void sat_lead_draw_span (const sat_leadspan_t *sp, int lowdraw)
{
    extern byte *ylookup[]; extern int columnofs[];   /* r_draw.c, no header */
    {
    /* ⚠ MIRROR R_ExecuteSetViewSize'S DRAWER CHOICE EXACTLY.  In M7 detailshift is 1 -- it drives
       the 160-column projection and the VDP1 x<<1 -- but the master still uses the NORMAL
       R_DrawColumn, because the framebuffer is PACKED 160 wide (one byte per logical column) and
       the *Low drawers would re-duplicate it back to 320.  Branching on detailshift alone put every
       slave span at double x, duplicated: the owner's *"les dessins du slave ne sont pas au bon
       endroit sur l'image"*. */
	int count = sp->yh - sp->yl;
	const byte *src;
	byte *dest;
	if (count < 0) return;
	if ((unsigned)sp->x >= (unsigned)SCREENWIDTH || sp->yl < 0 || sp->yh >= viewheight) return;
	/* RE-RESOLVE HERE, not at record time.  R_GetColumnCached only reads -- it never allocates,
	   so it is safe on the slave, and it answers NULL the moment the source stopped being
	   reachable (directory purged, composite purged/stubbed, lump evicted).  NULL => this span
	   draws FLAT in the texture's own dominant colour for one frame, which is the whole point:
	   the old code held the stale ADDRESS and drew a neighbour's pixels instead. */
	src = (sp->tex < 0) ? NULL : R_GetColumnCached (sp->tex, sp->tcol);
	if (!src && sp->tex >= 0) sat_lead_stale++;   /* overlay `st` on the CD row -- MUST tend to 0 */
	if (lowdraw)
	{
	    int x = sp->x << 1;
	    byte *d2;
	    dest = ylookup[sp->yl] + columnofs[x];
	    d2   = ylookup[sp->yl] + columnofs[x + 1];
	    if (!src)
	    {
		byte c = sp->cmap[(unsigned char)sp->col];
		do { *d2 = *dest = c; dest += SCREENWIDTH; d2 += SCREENWIDTH; } while (count--);
	    }
	    else
	    {
		fixed_t frac = sp->texmid + (sp->yl - centery) * sp->iscale;
		do { byte px = sp->cmap[src[(frac >> FRACBITS) & 127]];
		     *d2 = *dest = px; dest += SCREENWIDTH; d2 += SCREENWIDTH; frac += sp->iscale; }
		while (count--);
	    }
	}
	else
	{
	    dest = ylookup[sp->yl] + columnofs[sp->x];
	    if (!src)
	    {
		byte c = sp->cmap[(unsigned char)sp->col];
		do { *dest = c; dest += SCREENWIDTH; } while (count--);
	    }
	    else
	    {
		fixed_t frac = sp->texmid + (sp->yl - centery) * sp->iscale;
		do { *dest = sp->cmap[src[(frac >> FRACBITS) & 127]];
		     dest += SCREENWIDTH; frac += sp->iscale; }
		while (count--);
	    }
	}
    }
}

/* The batch form: everything recorded, drawn in one go.  Kept for the 1p lead-fill path. */
void R_LeadSlaveDraw (void)
{
    extern int sat_lowres;                           /* r_main.c */
    int lowdraw = (detailshift && !sat_lowres);
    int i;
    for (i = 0 ; i < sat_lead_span_n ; i++)
	sat_lead_draw_span (&sat_lead_spans[i], lowdraw);
}

/* 🔴 SATURN 2026-08-26 -- WALL FILL ON THE SLAVE, the streaming half.
   WHY STREAMING AND NOT A HANDOVER.  The obvious shape -- record the whole view, then hand the
   queue over -- has to run somewhere, and the only window after the wall phase is R_DrawPlanes,
   where the slave is ALREADY earning its keep (row 5 `Pm`/`Ps` read 8.5/7.9 on hardware: the
   plane work-steal splits that phase almost evenly).  Taking the planes away to give it the walls
   would trade one job for another and improve nothing globally.
   The window that is actually free is `Bp` itself -- 48.6 ms of 4p frame in which the master does
   pure geometry and the slave does NOTHING.  So the slave drains spans as they appear inside that
   window, joining after the wall flush and before R_DrawPlanes.  It keeps its half of the planes.
   🔴 THE DISPATCH IS LAZY -- it fires on the FIRST span of the view, from sat_wallfill_take,
   NOT at arm time.  Arming before the walk held the slave in the back-off spin for the whole wall
   phase even when the threshold went on to reject every column, and that spin is NOT free: the 1p
   A/B of 2026-08-26 read `lp` 12.2 with `lk0` and `b47%` against `lp` 11.8 with the toggle off --
   ~0.4 ms of master time bought zero pixels.  Dispatching on the first append means the slave's
   first read already finds work, and a view that queues nothing never wakes it at all.
   WHY THIS IS SAFE WITHOUT A LOCK, and the fact that makes it so: the SH-2 cache is WRITE-THROUGH
   by construction -- "Writing from the CPU always produces a write cycle externally", there is no
   write-back bit (saturn-refs/knowledge/HW_MEMORY_AND_BUS.md, from the manual).  Two CPUs writing
   different bytes of one cache line therefore cannot clobber each other, so the pixel-disjointness
   argument that already licenses the plane steal is sufficient here too.  The queue index is read
   through the UNCACHED mirror so the slave sees the master's appends.
   The spin between appends touches NO memory (empty asm, registers only): an idle slave polling a
   cached-through address would steal bus cycles from a master that is memory-bound. */
static volatile int sat_lead_prod_done_v = 1;
#define SAT_LEAD_PROD_DONE (*(volatile int *)((unsigned int)&sat_lead_prod_done_v | 0x20000000u))
#define SAT_LEAD_SPAN_N    (*(volatile int *)((unsigned int)&sat_lead_span_n     | 0x20000000u))

void R_LeadSlaveStream (void)
{
    extern int sat_lowres;
    int lowdraw = (detailshift && !sat_lowres);
    int drawn = 0;
    for (;;)
    {
	int n = SAT_LEAD_SPAN_N;
	while (drawn < n)
	    sat_lead_draw_span (&sat_lead_spans[drawn++], lowdraw);
	if (SAT_LEAD_PROD_DONE)
	{
	    if (drawn >= SAT_LEAD_SPAN_N) return;   /* re-read: the last append can race the flag */
	    continue;
	}
	{ int k; for (k = 0; k < 24; k++) __asm__ __volatile__(""); }   /* back off, no bus access */
    }
}

int  R_LeadSpanCount (void) { return sat_lead_span_n; }
void R_LeadSpanReset (void) { sat_lead_span_n = 0; }

/* The COLUMN KEY of whatever dc_source was last set from, stamped by SAT_LEAD_KEY at each of the
   three R_GetColumn sites in the seg loop.  This is what the record stores instead of the address
   -- see the sat_leadspan_t comment.  `fb` is the flat fallback colour, peeked (never loaded) once
   per column rather than per span. */
static short sat_lead_tex = -1, sat_lead_tcol = 0, sat_lead_fb = SAT_WALL_FLAT_UNKNOWN;
#define SAT_LEAD_KEY(t,c) do {						\
	sat_lead_tex = (short)(t); sat_lead_tcol = (short)(c);		\
	{ int _fb = R_WallPotatoColorPeek (t);				\
	  sat_lead_fb = (short)(_fb < 0 ? SAT_WALL_FLAT_UNKNOWN : _fb); }	\
    } while (0)

static void sat_lead_span_add (int yl, int yh)
{
    sat_leadspan_t *sp;
    if (sat_lead_span_n >= SAT_LEADSPAN_MAX || !sat_lead_spans) { sat_lead_span_drop++; return; }
    sp = &sat_lead_spans[sat_lead_span_n++];
    sp->x = (short)dc_x; sp->yl = (short)yl; sp->yh = (short)yh;
    sp->cmap = dc_colormap;
    if (sat_dc_solid) { sp->tex = -1; sp->tcol = 0; sp->col = (short)sat_wall_color;
			sp->iscale = 0; sp->texmid = 0; }
    else              { sp->tex = sat_lead_tex; sp->tcol = sat_lead_tcol; sp->col = sat_lead_fb;
			sp->iscale = dc_iscale; sp->texmid = dc_texturemid; }
}

/* 🔴 THE HEIGHT THRESHOLD IS NOT A COMPROMISE, IT IS THE BREAK-EVEN.  A record is 24 bytes;
   drawing the column costs `count` bytes of framebuffer plus the texture reads.  Below ~24 rows
   recording is pure loss, above it the slave draws for free in a window the master cannot use.
   The 1536-entry cap says the same thing from the other side: a 4p view is 160 columns x up to 3
   tiers = 480 spans, four views = 1920, so an UNFILTERED producer would overflow and
   sat_lead_span_drop would eat the tail.  One knob, one subject: 0 = off, else the minimum column
   height that goes to the slave.  Live on pad R+X. */
/* [!] DEFAULT 48 -- SET BY CONSOLE, AGAINST YMIR.  The bus contention flagged as "the one risk
   console alone can price" is REAL, and hardware priced it exactly.  4p TNT, same spot, all five
   videos decoded (fingerprint identical on every frame quoted: c1679 d187 ld201/43 vp50 ds65
   zf457 lg103), only frames with a == fps admitted:

       rung   video          f   lk   b%    hd     pr     lp     tl     Bp
       OFF    mode3         49    0    4  25.6   29.8   30.8    1.8   89.3
       96     mode2         10    3   11  26.5   30.7   28.3    1.8   88.6
       48     mode1         10    3   11  25.4   30.3   28.2    2.0   87.6
       24     no-mode+m4     0    4   35  26.5   32.7   28.7    2.4   91.9

   THE 96 ROW ARRIVED 2026-08-27 AND IT SAYS SOMETHING BETTER THAN A NUMBER: 96 AND 48 SELECT THE
   SAME COLUMNS.  `f10` and `lk3` are identical, so the same 10 candidates stay on the master and
   the same ~39 go to the slave; `b11%` confirms it from the other side.  Read the `f` column as a
   HISTOGRAM of the ~49 CPU-drawn candidates: f0 at rung 24 => every candidate is >= 24 rows tall;
   f10 at rung 48 => ten of them fall in [24,48); f10 STILL at rung 96 => *zero* fall in [48,96).
   The population is bimodal -- ten short slivers, and forty near-full-height columns, which is what
   a 4p quadrant does to geometry: the viewport is 112 rows, so a close wall spans nearly all of it
   and there is no middle.  So the top rung is not "worse", it is INERT at this spot, and since it
   can only ever offload a SUBSET of what 48 offloads it can never beat it here.  Whether a higher
   threshold helps anywhere is still UNTESTED -- this video did not test it, it re-tested 48.
   AND IT GIVES THE NOISE FLOOR, WHICH IS THE PART THAT SHOULD CHANGE HOW THE ROWS ABOVE ARE READ.
   Two pairs of same-configuration console captures now exist: 48 vs 96 (identical selection),
   Bp 87.2 / 88.6, and the two rung-24 videos differ Bp 91.8 / 90.7.  **Same config, same spot:
   1,1 to 1,4 ms of spread on Bp.**  The headline "-1,7 ms for 48 over OFF" is therefore barely
   outside the noise of a single pair, and no future one-shot A/B on this metric should be believed
   under ~3 ms.  What IS solid is the MONOTONE TREND, because it has four independent points:
   `pr` = 29.8 at b4%, 30.3 and 30.7 at b11%, 32.0 and 32.2 at b35%.  A trend over four samples
   survives a noise floor that kills any single difference.
   `lp` falls by the SAME amount at every armed rung.  What separates them is `pr` -- the VDP1 wall
   geometry preamble, which the master walks through scattered sidedef/sector memory -- and it
   rises with SLAVE OCCUPANCY.  The second CPU filling columns is stealing bus cycles from a
   memory-bound master, and past ~b11% it costs more than the fill it saved.
   YMIR SHOWED NONE OF THIS -- it has no bus model, so its ladder was flat (lk 3/4/3 at 24/48/96)
   and 24 looked free.  This is precisely what [[ymir-not-a-perf-oracle]] is about, and it is why
   the threshold was shipped as a LADDER instead of an on/off: the break-even was a guess until
   hardware priced it, and hardware put it between 24 and 48.
   ⚠ NONE of this moves the fps counter: MST reads 250 on all four rungs because 250.2 ms is
   exactly 15 NTSC fields.  Read `Bp`, never MST, for a change of this size.

   [!] 2026-08-28 -- THE RUNG IS MODE-DEPENDENT, BUT *NOT* FOR THE REASON THE GEOMETRY SUGGESTS.
   The owner asked whether the height threshold should scale with the view count, since 3/4p cuts
   the screen horizontally and halves every column.  A full Ymir sweep of the ladder in 1p/2p/3p/4p
   says the opposite of that intuition.  Read `f` as a HISTOGRAM of the CPU-drawn candidates (`f` =
   the ones the master still fills, `GCS w?/<n>` = the candidate count):

     mode  viewport  cand.   f(24) f(48) f(96)   => where the candidate heights actually sit
      1p    200 rows   26      -     0     26       ALL 26 in [48, 96)      = 24..48 % of the view
      2p    224 rows   13      0    13     13       ALL 13 in [24, 48)      = 11..21 % of the view
      3p    112 rows   46      0     7      -       7 in [24,48), 39 >= 48  = >= 43 %
      4p    112 rows   49      0    10     10       10 in [24,48), 39 >= 96 = >= 86 % of the quad

   THE SHORTEST VIEWPORT HAS THE TALLEST CANDIDATES.  2p has the tallest viewport (224, a vertical
   split keeps full height) and every one of its candidates is UNDER 48 rows; 4p has the shortest
   (112) and 39 of 49 candidates fill 86 % of it.  So the population is not set by the geometry --
   it is set by WHICH WALLS THE VDP1 BUDGET REJECTS.  In 3/4p the command budget is split four ways,
   so the big near walls fall back to the CPU and the candidates are near-full-height; in 1p/2p
   there is budget to spare and only slivers and edge cases fall back.
   ⇒ A single absolute rung cannot serve modes whose candidate bands are DISJOINT.  What the sweep
   supports as per-mode defaults, targeting the +7..+9 points of `b%` the 4p console table showed
   to be the affordable increment:  1p -> 48 (b22->31),  2p -> 24 (b14->21),  3p -> 48 (b5->14),
   4p -> 48 (b4->11).  96 is dominated in every mode measured.
   ⚠ AND THE HONEST CAVEAT ON THOSE MS: this sweep is YMIR, which has NO BUS MODEL, so it can price
   the SELECTION (`f`/`lk`/`b%` -- counts, which is what Ymir is for) and NOT the contention that is
   the whole reason this threshold exists.  Its `Bp` spread across all four rungs was 16.1-17.1 in
   1p and 21.2-21.7 in 2p -- i.e. nothing, at or under the 1.1-1.4 ms console noise floor.  Do not
   read those as "the ladder is worthless"; read them as "Ymir cannot see this lever at all". */
/* [!] 2026-08-28, SAME DAY, SECOND HALF -- THE PER-PLAYER-COUNT TABLE IS GONE AND `wh` IS WHAT
   KILLED IT.  It was added this morning on the reading that the candidate bands are DISJOINT
   between modes (1p in [48,96), 2p in [24,48), 3/4p bimodal at 96+).  That reading came from
   walking the ladder across fifteen captures, one rung per scene, and the histogram -- which
   measures the WHOLE curve in ONE frame -- refutes it on twenty-eight fresh ones:
       4p, twenty captures, mean of `wh`   :  2,7 / 5,0 / 1,2 / 0,9   (<24 / [24,48) / [48,96) / >=96)
       and the SPREAD WITHIN 4p            :  wh9000 (nothing at all above 24)
                                              wh0414 (half the population at or above 48)
   The between-mode difference is smaller than the between-SCENE difference inside a single mode,
   by a wide margin, so a per-mode constant cannot track this population -- it just freezes one
   scene's answer and calls it a mode.  ONE global rung, 48.
   THE OLD TABLE'S OTHER CLAIM DIES TOO: "3/4p is bimodal with ~80 % at or above 96" came from
   reading row-14 `f` as a histogram back when it was the only witness.  `f` counts columns SHORTER
   than the live rung, so at a single rung it cannot separate "nothing above 48" from "everything
   above 96" -- exactly the ambiguity `wh` was built to remove.  Bucket d (>=96) reads 0 or 1 tenth
   in EIGHTEEN of the twenty 4p captures.
   ⚠ AND THE LEVER ITSELF IS NOW ON TRIAL, which is the more useful finding: mean 4p `lk` at rung
   48 is 1,35 -- 1350 offloaded pixels per frame -- because the VDP1 owns nearly every wall and the
   CPU fallback is only ~100 columns.  Console has already priced the rungs (slave busy 4 / 11 /
   35 %, `pr` 29,8 / 30,3 / 32,7 for OFF / 48 / 24), so rung 24 buys 3,5x those pixels for +2,4 ms
   of B-bus tax on `pr`.  There is no case for 24 anywhere.  If the console session confirms the
   tax at 48 as well, the whole wall-fill offload goes -- and takes `w`, `f`, `lk`, `wh` and the
   R+X chord with it. */
int sat_wallfill_min = 48;   /* LIVE value for the hot path -- a plain int, never an array read */

/* [!] `wh<a><b><c><d>` on row 14 -- THE CANDIDATE-HEIGHT HISTOGRAM, and it exists because deriving
   the same thing from the ladder cost FIFTEEN captures and still could not be trusted.  Walking the
   rungs samples each one in a DIFFERENT scene (the tester is moving), which is
   [[interbuild-perf-noise]] transposed into time.  This measures the whole selection curve in ONE
   frame, at ANY rung: four buckets at the ladder's own boundaries, one increment per candidate
   column, no division in the hot path.  Printed as tenths of the candidate count.
   [!] IT NEEDS AN ARMED RUNG.  R_WallFillArm returns early at rung 0, so sat_wallfill_take is never
   reached and `wh` reads dots.  That is honest -- no producer, no candidates -- but it means the
   histogram cannot be read with the feature off: park on any non-zero rung to read it. */
unsigned short sat_wf_hist[4];      /* <24 | [24,48) | [48,96) | >=96 */
static int sat_wallfill_on;      /* producer OPEN for this view: buffer present, queue reset      */
static int sat_wallfill_live;    /* slave ACTUALLY dispatched -- i.e. at least one span exists.
				    Split from `on` so R_WallFillDone can skip the join AND the
				    cache purge that RP_LeadJoin carries when nothing ever ran. */

/* Record this column for the slave instead of drawing it.  Returns 0 = caller must draw it.
   Everything the drain needs is already in the dc_* globals the caller just set; the texture is
   stored as a KEY and re-resolved on the slave, never as an address (see sat_leadspan_t). */
static int sat_wallfill_take (int tex, int col)
{
    if (!sat_wallfill_on || sat_dc_solid) return 0;
    {   /* row 14 `wh` -- census EVERY candidate, before the threshold decides anything about it.
           Three compares, no division; the buckets are the ladder's own rungs, so the digits read
           straight off as "what each rung would select". */
        int h = dc_yh - dc_yl + 1;
        sat_wf_hist[h < 24 ? 0 : h < 48 ? 1 : h < 96 ? 2 : 3]++;
    }
    if (dc_yh - dc_yl + 1 < sat_wallfill_min) return 0;
    if (sat_lead_span_n >= SAT_LEADSPAN_MAX) { sat_lead_span_drop++; return 0; }
    SAT_LEAD_KEY (tex, col);
    sat_lead_span_add (dc_yl, dc_yh);
    /* WAKE THE SLAVE ON THE FIRST SPAN, once per view -- the append above is already in RAM
       (write-through), so its first read finds work instead of an empty queue to spin on. */
    if (!sat_wallfill_live)
    {
	extern void RP_AuxDispatch (void (*fn)(void));
	sat_wallfill_live = 1;
	RP_AuxDispatch (R_LeadSlaveStream);
    }
    /* Row 14 `lk` becomes the OFFLOAD METER: pixels this frame the master did not fill.  `f`/`k`
       fall by the same amount, which is the point -- they count the master's own colfunc work. */
    prof_lead_px += (unsigned)(dc_yh - dc_yl + 1);
    sat_lead_cols++;
    return 1;
}

/* Arm the producer for this view and hand the slave the queue.  Lazy, ASK-DO-NOT-DEMAND alloc:
   the span buffer is 36 KB of zone and this is an OPTIONAL subsystem -- a fatal allocation for one
   would be the SCYTHE MAP30 fault all over again.  Off => the tiers all draw on the master, byte
   for byte as before. */
void R_WallFillArm (void)
{
    sat_wallfill_on = 0;
    sat_wallfill_live = 0;
    if (sat_wallfill_min <= 0) return;
    if (!sat_lead_spans)
    {
	if (!Z_CanAllocate (sizeof(sat_leadspan_t) * SAT_LEADSPAN_MAX)) return;
	sat_lead_spans = Z_Malloc (sizeof(sat_leadspan_t) * SAT_LEADSPAN_MAX, PU_STATIC, NULL);
    }
    sat_lead_span_n = 0;
    sat_lead_prod_done_v = 0;      /* write-through -> in RAM before the slave's first read */
    sat_wallfill_on = 1;      /* the slave is woken by the first sat_wallfill_take, not here */
}

/* Close the producer and join.  MUST run before R_DrawPlanes: the plane split wants the slave
   back, and the sprites of the masked pass must land on top of these wall pixels. */
void R_WallFillDone (void)
{
    extern void RP_LeadJoin (void);
    if (!sat_wallfill_on) return;
    sat_wallfill_on = 0;
    sat_lead_prod_done_v = 1;                     /* set BEFORE the join: it is the spin's exit */
    if (sat_wallfill_live) { RP_LeadJoin (); sat_wallfill_live = 0; }
    sat_lead_span_n = 0;
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
	    /* SATURN 2026-08-18 -- ASK, DO NOT DEMAND.  These are LAZY PU_STATIC blocks claimed on
	       the first rendered frame -- i.e. AFTER the level has taken its share of the zone.  On
	       SCYTHE MAP30 the level fits and this did not: Z_Malloc I_Errored on 36888 bytes with
	       fr18K lg11K and killed a level that had loaded perfectly.  A fatal allocation for an
	       OPTIONAL subsystem is a design fault, not a lack of RAM: sat_lead_record and
	       sat_lead_span_add both bail on a NULL pointer already, and the header above calls a
	       missing history "a bounded degradation, never a wrong pixel".  So ask, and stay off if
	       the answer is no -- the same graceful-sink policy as the visplane, openings, composite
	       and W_ReadLump guards.
	       The six rings are now ONE block sliced six ways: all-or-nothing BY CONSTRUCTION, so a
	       partial success can never leave sat_leadh[0] valid and sat_leadh[5] NULL for
	       sat_lead_record to write through.  Small block first, then re-ask for the big one --
	       two Z_CanAllocate calls up front would both pass on a single free run that only fits
	       one of them. */
	    sat_leadq_t*	heads;

	    if (!Z_CanAllocate (sizeof(sat_leadq_t) * SAT_LEADH_MAX * SAT_LEADH_DEPTH))
		return;
	    heads = Z_Malloc (sizeof(sat_leadq_t) * SAT_LEADH_MAX * SAT_LEADH_DEPTH, PU_STATIC, NULL);

	    if (!Z_CanAllocate (sizeof(sat_leadspan_t) * SAT_LEADSPAN_MAX))
		{ Z_Free (heads); return; }
	    sat_lead_spans = Z_Malloc (sizeof(sat_leadspan_t) * SAT_LEADSPAN_MAX, PU_STATIC, NULL);

	    for (i = 0 ; i < SAT_LEADH_DEPTH ; i++)
		sat_leadh[i] = heads + i * SAT_LEADH_MAX;
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

/* SATURN 2026-08-17 (row 14 `SEG`): one increment and one add per fill, NO timer -- a probe that
   cannot inflate the very number it is sizing.  Wraps every colfunc() reachable from the seg loop. */
#define SAT_PROF_FILL() do { if (sat_dbg_overlay_mode == 0) { prof_seg_fill++;	\
	if (dc_yh >= dc_yl) prof_seg_px += (unsigned)(dc_yh - dc_yl + 1); } } while (0)

/* One difference span: straight to the framebuffer, or into the slave's list.
   SATURN 2026-08-17: the span's PIXELS are counted on both paths (row 14 `SEG`), because the whole
   question about the lead-fill is how its pixel bill compares with the walls' own -- `L1s/1224` on
   hardware says it emits as many column-spans as the walls do, and that is a guess until counted. */
#define SAT_LEAD_EMIT(a,b) do { 	if ((b) >= (a)) prof_lead_px += (unsigned)((b) - (a) + 1); 	if (sat_lead_mode == 1) sat_lead_span_add ((a), (b)); 	else { dc_yl = (a); dc_yh = (b); SAT_PROF_FILL (); colfunc (); } 	sat_lead_cols++; sat_gov_act_l++; } while (0)

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
	/* SATURN 2026-08-18: same graceful rule as the lead-fill rings above -- this is a
	   heuristic cache, so a full zone turns it off instead of killing the level. */
	if (!Z_CanAllocate (SAT_SEG_MAX))
	    return cpu_now;
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
	/* sat_wall_entry BAKED at 1 (2026-08-26), the documented default; the SAT_SEG_ENTRY_MAX
	   clamp went with it (1 <= 3 always).  See the note at the definition site. */
	if (((unsigned int)(tag - (b >> 4)) & 15u) != 1u) e = 1;  /* gap != 1 -> not visible last frame */
	else if (e) e--;
	b = (unsigned char)((tag << 4) | (e << 2) | (b & 3));
	*st = b;
	first = 2;
    }
    return first | ((((b >> 2) & 3) != 0) ? 1 : 0);
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
	return 0;
    if (xL < rw_x)         xL = rw_x;
    if (xR > rw_stopx - 1) xR = rw_stopx - 1;
    if (xR - xL < SAT_WALL_EDGE_MIN) return 0;
    dL = xL - rw_x; dR = xR - rw_x;
    ylL = SAT_SHR12 ((topfrac    + topstep    * dL + HEIGHTUNIT - 1));
    yhL = SAT_SHR12 ((bottomfrac + bottomstep * dL));
    ylR = SAT_SHR12 ((topfrac    + topstep    * dR + HEIGHTUNIT - 1));
    yhR = SAT_SHR12 ((bottomfrac + bottomstep * dR));
    if (need_floor_clear)
    {
	int m = yhL > yhR ? yhL : yhR;
	sat_wall_clip_need();
	if (m >= sat_clip_fcm) return 0;   /* may dip under a nearer floor -> CPU */
    }
    if (sat_wall_hook(xL, ylL, yhL, xR, ylR, yhR, texture, uL, uR, v0, v1, cm))
	return 0;                             /* VDP1 bank full */
    sat_we_on = 1; sat_we_lo = xL; sat_we_hi = xR;
    return 1;
}

/* 🔴 SATURN 2026-08-26 -- THE SOFTWARE TIER DRAW, LIFTED OUT OF THE COLUMN LOOP.
   The disassembly of the shipped r_segs.o measured ONE loop body at 888 instructions, 336 of them
   memory accesses (38 %) and 42 stack-address formations -- "mov #N,rX; add r15,rX" before the
   access, three instructions to reach one variable, the signature of a function that has run out
   of registers.  For 1630 of 1679 columns (row 14 `c` against `f`) that body has to produce six
   array writes and nothing else: the VDP1 owns the wall, and `GCS w1/49` proves not even a
   texture column is resolved for them.
   The six software-draw blocks -- three tiers x {draw, lead-fill} -- were ~90 lines INLINE in
   that body.  They never execute on the VDP1 path, but they are still ALLOCATED for: their
   register demand is what spills the hot path onto the stack, and the hot path jumps over ~600
   bytes of code it never runs, straddling cache lines that are mostly cold.  Lifting them out
   costs one call on the 3 % of columns that already call colfunc().
   [!] ONE implementation, not a duplicated fast loop (owner call, and the right one): the three
   tiers differ only in WHICH texture / texmid / flat-substitute / wedge they use, so they are one
   function over a per-seg descriptor.  There is no second copy of this logic to drift.
   [!] The descriptors are snapshots taken ONCE per seg, which is only legal because every field
   is constant across the loop: rw_*texturemid is written exclusively in R_StoreWallRange_impl
   (all of it before this function is entered), and midtexture / toptexture / bottomtexture,
   wall_solid, io_flat_* and sat_wcl_* are all fixed by the routing preamble above
   RP_SegRoutMark.  The one field that MOVES per column is the wedge edge -- held BY POINTER.
   [!] Measured on the SAME clone, before -> after: see docs/ATLAS.md row 4. */
typedef struct {
    int		 tex;		/* midtexture / toptexture / bottomtexture		*/
    fixed_t	 texmid;	/* rw_midtexturemid / rw_toptexturemid / rw_bottom...	*/
    int		 io_flat;	/* load budget said "no disc this frame" for this tier	*/
    int		 io_col;	/*   ... and this is the flat colour to use instead	*/
    int		 solid;		/* wall_solid (potato): one colour, no texture read	*/
    int		 wcl;		/* Phase-1 wedge: 0 none / 1 clamp dc_yl / 2 clamp dc_yh	*/
    fixed_t	*wcl_ef;	/* the wedge edge -- STEPPED per column, hence a pointer	*/
    sat_lead_t	*lead;		/* &sat_lead_mid / _up / _lo				*/
} sat_tier_t;

/* The software column draw for one tier.  Byte-for-byte the block that was inline here. */
static void sat_tier_draw (const sat_tier_t *t, int yl, int yh, int texturecolumn)
{
    dc_yl = yl;
    dc_yh = yh;
    /* Phase-1 wedge: VDP1 owns the tier up to the cut line -> software draws only the residue
       past it (colfunc tolerates yl > yh, same as vanilla off-screen columns). */
    if (t->wcl == 1)
	{ int e = SAT_SHR12 (*t->wcl_ef); if (dc_yl < e) dc_yl = e; }
    else if (t->wcl == 2)
	{ int e = SAT_SHR12 (*t->wcl_ef); if (dc_yh > e) dc_yh = e; }
    dc_texturemid = t->texmid;
    if (t->solid)
	sat_wall_color = R_WallPotatoColor (t->tex);
    else if (t->io_flat)
	sat_wall_color = t->io_col;		/* SATURN load budget: no disc this frame */
    else
	dc_source = R_GetColumn (t->tex, texturecolumn);
    sat_dc_solid = t->solid || t->io_flat;	/* SATURN: armed for WALL columns only (r_draw.c) */
    if (!sat_wallfill_take (t->tex, texturecolumn))
    {	SAT_PROF_FILL ();
	colfunc (); }
    sat_dc_solid = 0;
}

/* LEAD-FILL: VDP1 owns the tier -> draw only what the OLD quad missed. */
static void sat_tier_lead (const sat_tier_t *t, int x, int yl, int yh, int texturecolumn)
{
    int lflat = t->solid || sat_lead_flat;	/* see sat_lead_flat */
    dc_texturemid = t->texmid;
    if (lflat)		 sat_wall_color = R_WallPotatoColor (t->tex);
    else if (t->io_flat) { sat_wall_color = t->io_col; lflat = 1; }
    else		 { dc_source = R_GetColumn (t->tex, texturecolumn);
			   SAT_LEAD_KEY (t->tex, texturecolumn); }
    sat_dc_solid = lflat;
    sat_lead_draw (t->lead, x, yl, yh);
    sat_dc_solid = 0;
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
    sat_gc_site = 2;   /* SATURN row 16 `GCS`: everything up to the column loop is the PREAMBLE */
    sat_wall_textured = (sat_wall_paint & 2) ? 0 : (SEG_LINEDEF(curline)->special != 0);
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

	/* SATURN 2026-08-24 -- THE SQUISH GUARD, HOISTED OUT OF THE SUBDIVISION LOOP.
	   The subdivided path tests every sub-seg before emitting it (`sdu < 1 || tw*cols > 1024*sdu`
	   -> route the whole wall to SOFTWARE, sat_fb_mag_t) because a tile that extrapolates past the
	   platform's coordinate allowance can only be drawn CLAMPED = the "ecrasement" that raising
	   wall_ext 96 -> 768 was meant to end (dg_saturn.cxx wall_ext).  The skip path emits ONE quad
	   and had NO such test, so the guard has to live at the ROUTING decision -- which is free,
	   because sx and mdu are the very quantities `magnified` above already computed, and for a
	   single whole-seg quad they ARE the exact operands (no sub-seg to under-estimate).
	   Fails -> the tier stays on the CPU, i.e. exactly the pre-skip behaviour, and the L5 edge
	   split still gets its chance.
	   32-bit on purpose: tw <= 1024 and sx <= the view width, so tw*sx <= ~330 000, and mdu is a
	   texel span, so 1024*mdu stays far inside int on any seg the BSP can hand us -- the 64-bit
	   form the subdivision loop uses would cost a __muldi3 per tier for no reachable case. */
#define SAT_QUAD_FITS(texnum)  ((texturewidthmask[texnum] + 1) * sx <= 1024 * mdu)

	if (midtexture && !SEG_BACKSECTOR(curline))
	{
	    int s1 = SAT_SHR12 ((bottomfrac - topfrac));
	    int s2 = SAT_SHR12 (((bottomfrac + bottomstep * n) - (topfrac + topstep * n)));
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
	    /* magnified -> NO single quad (it squishes), unless the governor asked for the skip AND the
	       quad actually fits the platform's extrapolation window (SAT_QUAD_FITS). */
	    sat_v1_mid = (s < sat_wall_cpu_v1)
		      && (!magnified || (sat_wall_subdiv_skip && SAT_QUAD_FITS(midtexture)));
#if SAT_WALL_SUBDIV
	    if (magnified && !span_close)
	    {   /* keep it on VDP1: one quad if the governor is shedding CPU and the tile fits,
		   otherwise the perspective-subdivided N-quad path (emit site).  Neither -> CPU. */
		if (sat_wall_subdiv_skip && SAT_QUAD_FITS(midtexture))
		    { sat_v1_mid = 1; cpu_now = 0; sat_gov_act_w++; }
		else
		    { sat_v1_mid_sub = 1; cpu_now = 0; }   /* skip off, or the quad would squish */
	    }
#endif
	    /* part 2: a demoted SPECIAL never rides VDP1 -- every tier goes software, textured. */
	    if (SAT_SPEC_CPU(midtexture))
	    { sat_v1_mid = 0; sat_v1_mid_sub = 0; cpu_now = 1; }
	    entry = sat_seg_entry_cover(st);   /* just came into view -> the CPU covers VDP1's first frame */
	    cpu_now = sat_dwell_cpu(segidx, cpu_now, entry & 2);   /* bound the FLIP RATE, see sat_wall_dwell */
	    if (cpu_now)
	    {
		if (!sat_v1_mid) {   /* Phase-0: count only the FULLY-CPU tiers (not the [SPAN,V1] VDP1-also pre-warm) */
		    if (magnified)                  sat_fb_mag_t++;                             /* squish -> clamp can't fix   */
		    else if (span_close)            { sat_gov_act_s++; }                                          /* pure span  -> clampable     */
		}
		sat_sw_mid = 1;  if (st) SAT_SEG_EXIT_ARM(st);           /* CPU draws (close/magnified); arm 2 CPU exit-frames */
		    /* SATURN L5: this tier is about to cost a FULL-SCREEN software wall (the ~22ms
		       nose-to-wall Bp).  Let the emit site below try the CPU-borders/VDP1-core split
		       first; it silently leaves everything as-is when the interior is too thin. */
		    if (sat_opt >= 5) sat_v1_mid_edge = 1;
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
	else if (SEG_BACKSECTOR(curline))
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
		int s1 = SAT_SHR12 ((pixhigh - topfrac));
		int s2 = SAT_SHR12 (((pixhigh + pixhighstep * n) - (topfrac + topstep * n)));
		int s = s1 > s2 ? s1 : s2;
		cpu_up = (s > sat_wall_cpu_span - dhy);
	    }
	    if (bottomtexture)
	    {
		int s1 = SAT_SHR12 ((bottomfrac - pixlow));
		int s2 = SAT_SHR12 (((bottomfrac + bottomstep * n) - (pixlow + pixlowstep * n)));
		int s = s1 > s2 ? s1 : s2;
		cpu_lo = (s > sat_wall_cpu_span - dhy);
	    }
	    {
		/* part 2, door/switch tiers: a demoted SPECIAL texture is forced software.  Applied
		   AFTER the Phase-0 counters below so the span governor is not credited for it. */
		int spec_up = SAT_SPEC_CPU(toptexture), spec_lo = SAT_SPEC_CPU(bottomtexture);
		int cpu_now = cpu_up || cpu_lo || magnified || spec_up || spec_lo;
		if (!cpu_now && dst && SAT_SEG_EXIT(dst) == 2) sat_wall_flip++;   /* CPU last frame, VDP1 now */
		if (cpu_up) { sat_gov_act_s++; }                     /* Phase-0: clampable span (upper door tier) */
		if (cpu_lo) { sat_gov_act_s++; }                     /* Phase-0: clampable span (lower door tier) */
		if (spec_up) cpu_up = 1;
		if (spec_lo) cpu_lo = 1;
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
		int skip_up = sat_wall_subdiv_skip && toptexture    && SAT_QUAD_FITS(toptexture);
		int skip_lo = sat_wall_subdiv_skip && bottomtexture && SAT_QUAD_FITS(bottomtexture);
		sat_v1_up = (toptexture    && (!magnified || skip_up) && !cpu_up) ? 1 : 0;
		sat_v1_lo = (bottomtexture && (!magnified || skip_lo) && !cpu_lo) ? 1 : 0;
		if (magnified && ((skip_up && !cpu_up) || (skip_lo && !cpu_lo))) sat_gov_act_w++;
#if SAT_WALL_SUBDIV
		/* magnified (not span-close) door tiers -> perspective-subdivide on VDP1 (emit site), not CPU */
		sat_v1_up_sub = (toptexture    && magnified && !skip_up && !cpu_up) ? 1 : 0;
		sat_v1_lo_sub = (bottomtexture && magnified && !skip_lo && !cpu_lo) ? 1 : 0;
		sat_sw_up = (toptexture    && (cpu_up || (magnified && !sat_v1_up_sub && !sat_v1_up) || overlap)) ? 1 : 0;
		sat_sw_lo = (bottomtexture && (cpu_lo || (magnified && !sat_v1_lo_sub && !sat_v1_lo) || overlap)) ? 1 : 0;
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
#undef SAT_QUAD_FITS

    /* SATURN: pot2 (banded/flat) -- a plain (non-special) wall draws as a VDP1 quad, so the close-wall
       CPU fallback above is skipped.  FLAT (pot2-fl) clamps -> can't swim; BANDED (pot2-bd) CAN swim/
       squish on close walls, but in the tiny split windows that's accepted (Romain) for the master Bp
       win.  Force VDP1, no software, for every tier.  SPECIAL walls (sat_wall_textured) stay TEXTURED
       and keep their own fallback. */
    if (sat_wall_nocpu && !sat_wall_textured && SAT_WALL_VDP1_OK && sat_wall_skip && rw_stopx > rw_x)
    {
	sat_sw_mid = sat_sw_up = sat_sw_lo = 0;
	if (midtexture && !SEG_BACKSECTOR(curline)) sat_v1_mid = 1;
	if (SEG_BACKSECTOR(curline))
	{
	    sat_v1_up = toptexture    ? 1 : 0;
	    sat_v1_lo = bottomtexture ? 1 : 0;
	}
    }

    /* SATURN VDP1 world renderer (Step 2): one-sided (solid) walls -> the platform as
       a quad.  The 4 screen corners come from the same topfrac/bottomfrac the loop
       below steps; midtexture = the full-height wall texture. */
    if (sat_wall_hook && SAT_WALL_VDP1_OK && midtexture && !SEG_BACKSECTOR(curline) && rw_stopx > rw_x && (sat_v1_mid || sat_v1_mid_sub || sat_v1_mid_edge))
    {
	int n   = rw_stopx - 1 - rw_x;
	int yl1 = SAT_SHR12 ((topfrac + HEIGHTUNIT - 1));
	int yh1 = SAT_SHR12 (bottomfrac);
	int yl2 = SAT_SHR12 ((topfrac + topstep * n + HEIGHTUNIT - 1));
	int yh2 = SAT_SHR12 ((bottomfrac + bottomstep * n));
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
	    /* Evaluated for its SIDE EFFECT: the sat_wall_cut_* call performs the clamp.  This was an
	       if (!(...)) whose body only bumped sat_fb_clamp_t / sat_fb_px -- both had no reader, so the
	       branch went with them (2026-08-26).  The && chain stays: it short-circuits the call. */
	    if (sat_wall_clamp && sat_v1_mid
	        && !(sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2)))
	        sat_wall_cut_floor(rw_midtexturemid, v0, yl1, yl2, midtexture, u1, u2, cm,
	                           sat_sw_mid, &sat_wcl_mid_ef, &sat_wcl_mid_es, &sat_wcl_mid);
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
	    /* Evaluated for its SIDE EFFECT: the sat_wall_cut_* call performs the clamp.  This was an
	       if (!(...)) whose body only bumped sat_fb_clamp_t / sat_fb_px -- both had no reader, so the
	       branch went with them (2026-08-26).  The && chain stays: it short-circuits the call. */
	    if (sat_wall_clamp && sat_v1_mid)
	        sat_wall_cut_ceil(rw_midtexturemid, v1, yh1, yh2, midtexture, u1, u2, cm,
	                          sat_sw_mid, &sat_wcl_mid_ef, &sat_wcl_mid_es, &sat_wcl_mid);
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
		    int yll = SAT_SHR12 ((topfrac    + topstep    * dnl + HEIGHTUNIT - 1));    /* EXACT */
		    int ylr = SAT_SHR12 ((topfrac    + topstep    * dnr + HEIGHTUNIT - 1));
		    int yhl = SAT_SHR12 ((bottomfrac + bottomstep * dnl));
		    int yhr = SAT_SHR12 ((bottomfrac + bottomstep * dnr));
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
    if (sat_wall_hook && SAT_WALL_VDP1_OK && SEG_BACKSECTOR(curline) && rw_stopx > rw_x)
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
	    int yl1 = SAT_SHR12 ((topfrac + HEIGHTUNIT - 1));
	    int yl2 = SAT_SHR12 ((topfrac + topstep * n + HEIGHTUNIT - 1));
	    int yh1 = SAT_SHR12 (pixhigh);
	    int yh2 = SAT_SHR12 ((pixhigh + pixhighstep * n));
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
		/* Evaluated for its SIDE EFFECT: the sat_wall_cut_* call performs the clamp.  This was an
		   if (!(...)) whose body only bumped sat_fb_clamp_t / sat_fb_px -- both had no reader, so the
		   branch went with them (2026-08-26).  The && chain stays: it short-circuits the call. */
		if (sat_wall_clamp && sat_v1_up
		       && !(sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2)))
		    sat_wall_cut_floor(rw_toptexturemid, v0, yl1, yl2, toptexture, u1, u2, cm,
		                             sat_sw_up, &sat_wcl_up_ef, &sat_wcl_up_es, &sat_wcl_up);
		sat_sw_up = 1;
	    }
	    else if (sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
	    {   /* above a nearer deported ceiling -> mirrored top cut + wedge; else CPU as before */
		/* Evaluated for its SIDE EFFECT: the sat_wall_cut_* call performs the clamp.  This was an
		   if (!(...)) whose body only bumped sat_fb_clamp_t / sat_fb_px -- both had no reader, so the
		   branch went with them (2026-08-26).  The && chain stays: it short-circuits the call. */
		if (sat_wall_clamp && sat_v1_up)
		    sat_wall_cut_ceil(rw_toptexturemid, v1, yh1, yh2, toptexture, u1, u2, cm,
		                            sat_sw_up, &sat_wcl_up_ef, &sat_wcl_up_es, &sat_wcl_up);
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
			int yll = SAT_SHR12 ((topfrac + topstep     * dnl + HEIGHTUNIT - 1));
			int ylr = SAT_SHR12 ((topfrac + topstep     * dnr + HEIGHTUNIT - 1));
			int yhl = SAT_SHR12 ((pixhigh + pixhighstep * dnl));
			int yhr = SAT_SHR12 ((pixhigh + pixhighstep * dnr));
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
	    int yl1 = SAT_SHR12 ((pixlow + HEIGHTUNIT - 1));
	    int yl2 = SAT_SHR12 ((pixlow + pixlowstep * n + HEIGHTUNIT - 1));
	    int yh1 = SAT_SHR12 (bottomfrac);
	    int yh2 = SAT_SHR12 ((bottomfrac + bottomstep * n));
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
		/* Evaluated for its SIDE EFFECT: the sat_wall_cut_* call performs the clamp.  This was an
		   if (!(...)) whose body only bumped sat_fb_clamp_t / sat_fb_px -- both had no reader, so the
		   branch went with them (2026-08-26).  The && chain stays: it short-circuits the call. */
		if (sat_wall_clamp && sat_v1_lo
		       && !(sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2)))
		    sat_wall_cut_floor(rw_bottomtexturemid, v0, yl1, yl2, bottomtexture, u1, u2, cm,
		                             sat_sw_lo, &sat_wcl_lo_ef, &sat_wcl_lo_es, &sat_wcl_lo);
		sat_sw_lo = 1;
	    }
	    else if (sat_vdp1_floor && sat_wall_cross_hi(yl1, yl2))
	    {   /* above a nearer deported ceiling -> mirrored top cut + wedge; else CPU as before */
		/* Evaluated for its SIDE EFFECT: the sat_wall_cut_* call performs the clamp.  This was an
		   if (!(...)) whose body only bumped sat_fb_clamp_t / sat_fb_px -- both had no reader, so the
		   branch went with them (2026-08-26).  The && chain stays: it short-circuits the call. */
		if (sat_wall_clamp && sat_v1_lo)
		    sat_wall_cut_ceil(rw_bottomtexturemid, v1, yh1, yh2, bottomtexture, u1, u2, cm,
		                            sat_sw_lo, &sat_wcl_lo_ef, &sat_wcl_lo_es, &sat_wcl_lo);
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
			int yll = SAT_SHR12 ((pixlow     + pixlowstep * dnl + HEIGHTUNIT - 1));
			int ylr = SAT_SHR12 ((pixlow     + pixlowstep * dnr + HEIGHTUNIT - 1));
			int yhl = SAT_SHR12 ((bottomfrac + bottomstep * dnl));
			int yhr = SAT_SHR12 ((bottomfrac + bottomstep * dnr));
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
    /* SATURN 2026-08-15 -- DISTANCE LOD, the first rule of the LOD governor.
       The budget gate above answers *"can I afford to LOAD this?"*.  This one answers a different
       question the budget never asked: *"is this tier worth TEXTURING at all?"*  A wall whose
       on-screen scale is tiny costs a full R_GenerateComposite -- 31 ms, measured, `cb21/8` says 8
       distinct textures a second on TNT MAP11 -- to produce a few pixels nobody can read.  Flatten
       it to its dominant colour instead: sat_dc_solid then skips R_GetColumn entirely, so no
       composite, no patch decode, no purge pressure on the composites that ARE close.
       ⚠ CORRECTED 2026-08-15 on the owner's report -- *"il aplatit un grand mur à mi-distance, très
       visible"*.  The first version tested `rw_scale` and I claimed it was "how big the tier lands on
       screen".  IT IS NOT: `rw_scale` is FixedDiv(projection, rw_distance), i.e. **1/distance and
       nothing else**.  A large wall and a small one at the same distance share it, so the big one
       flattened too -- a distance test wearing a screen-size costume.
       The honest predicate is SCREEN AREA: columns x tier pixel height.  Both are in hand here
       (worldtop/worldbottom are set by R_StoreWallRange before this loop runs), and it separates
       exactly the case that went wrong: at scale FRACUNIT/4 a 128-unit tier is ~32 px tall, so a
       20-column sliver scores 640 and flattens while a 100-column facade scores 3200 and keeps its
       texture.  Threshold is in PIXELS; 0 = off (pad L+B cycles it). */
    /* SATURN 2026-08-20 (owner): SPECIALS BLOCKER.  A linedef with a special (door, switch,
       exit, teleport, lift...) must NEVER draw flat: a flat-shaded door face or switch is
       invisible as an interactive element -> the level becomes unplayable.  Gates BOTH flatten
       rules below (distance LOD + drawseg budget) AND the io-budget path: for a special we
       always texture, even when that costs the synchronous disc read the budget exists to
       avoid -- specials are a tiny fraction of segs and the texture self-heals resident. */
    int keep_tex = SEG_LINEDEF(curline)->special != 0;
    int lod_flat = 0;
    if (sat_lod_eff > 0 && !keep_tex)
    {
	/* ⚠ HEIGHTBITS, not FRACBITS.  R_StoreWallRange already does `worldtop >>= 4` before this
	   loop, so FixedMul(world, rw_scale) lands in 1/2^12 pixel units -- the same scale the tier
	   coordinates use two lines below (`>> HEIGHTBITS`).  Shifting by 16 divided the height by a
	   further 16, every tier scored under the smallest rung, and all three steps flattened the
	   WHOLE level identically: `n0 g0 cb0/0`, 1215 hits at every step. */
	int lod_h = SAT_SHR12 (FixedMul (worldtop - worldbottom, rw_scale));
	int lod_w = rw_stopx - rw_x;
	if (lod_h < 0) lod_h = 0;
	if (lod_w < 0) lod_w = 0;
	/* SATURN 2026-08-16 -- DISTANCE FLOOR, on the owner's rule: *"si on coupe des éléments plus
	   loin, c'est potentiellement moins grave que de dégrader le premier plan"*.  Area alone is
	   not enough: a big wall seen edge-on a few units away subtends a THIN sliver and scores
	   under the rung, so the purely-areal predicate flattened foreground geometry -- the same
	   complaint that killed the rw_scale version, arriving by a different road.  Requiring BOTH
	   makes the LOD monotone in distance: nothing inside sat_lod_mindist ever flattens, whatever
	   its area.  `nr` on row 21 counts what the floor SAVED -- if it stays 0 the floor is inert
	   and the area rung is doing all the work; if it is large the rung is far too aggressive. */
	if ((lod_h * lod_w) < sat_lod_eff)
	{
	    if ((rw_distance >> FRACBITS) > sat_lod_mindist)
		{ lod_flat = 1; sat_gov_act_w++; }
	}
    }
    /* 🔴 SATURN 2026-08-16 -- THE DRAWSEG BUDGET.  The area rung above degrades walls that are
       SMALL; hardware says the frames that hurt are the ones with MANY (`Bp110,8` over `ds118` =
       ~0,9 ms per drawseg, on a 181 ms frame).  A rung sized on area cannot see that: 118 walls
       each above the threshold cost 110 ms and the governor keeps electing an axis that refuses
       to bite.  So bound the COUNT as well as the size.
       ⚠ This never removes a wall -- it removes its TEXTURING.  Skipping a solid wall outright
       would leave solidsegs open and the visplanes behind it unclosed: a see-through hole, not a
       degradation.  Everything past the budget takes the same `lod_flat` path the area rung
       already uses and that is already shipped, so the wall is still drawn, still clips, still
       marks its planes -- flat-shaded instead of textured, which reads as distance haze.
       Segs arrive front-to-back from the BSP, so a plain counter spends the budget NEAR-FIRST and
       the far walls are the ones that flatten -- the owner's rule, and the opposite of the mistake
       that killed the VDP1 wall offload ([[wall-offload-vdp1-slave-dead]]).  The distance floor
       applies here too: nothing inside sat_lod_mindist ever flattens, however crowded the frame. */
    sat_seg_count++;
    if (!lod_flat && !keep_tex && sat_seg_budget > 0 && sat_seg_count > sat_seg_budget
	&& (rw_distance >> FRACBITS) > sat_lod_mindist)
    {
	lod_flat = 1;
	sat_seg_budget_cut++;
	sat_gov_act_w++;
    }
    if (lod_flat) sat_wall_lod_hits++;
    int io_flat_mid = midtexture    ? (!keep_tex && (lod_flat || sat_wall_io_flat (midtexture)))    : 0;
    int io_flat_up  = toptexture    ? (!keep_tex && (lod_flat || sat_wall_io_flat (toptexture)))    : 0;
    int io_flat_lo  = bottomtexture ? (!keep_tex && (lod_flat || sat_wall_io_flat (bottomtexture))) : 0;
    int io_col_mid  = io_flat_mid ? sat_wall_flat_color (midtexture)    : 0;
    int io_col_up   = io_flat_up  ? sat_wall_flat_color (toptexture)    : 0;
    int io_col_lo   = io_flat_lo  ? sat_wall_flat_color (bottomtexture) : 0;
    /* SATURN PERF 2026-08-08: THE Bp SPLIT POINT.  Everything above is per-SEG (the VDP1/CPU
       tier routing, hysteresis, clamp, subdivision, lead-fill arming, the load-budget gates);
       everything below is per-COLUMN.  They scale with different quantities, so `BP` on row 20
       reports them apart.  Single call, no early return above it -- audited 2026-08-08. */
    /* The per-seg tier snapshots the lifted helpers read (see sat_tier_t).  Taken HERE, after
       every routing decision above is final and before the first column.
       [!] FILLED ON THE SAME TEST THE LOOP READS THEM UNDER.  A seg is EITHER one-sided (mid) OR
       two-sided (top/bottom), never both, so filling all three unconditionally wrote 96 bytes of
       stack per seg with half of it unreachable -- and the Ymir A/B priced it exactly: `pr` went
       10.8-11.6 -> 11.7-12.1 in 4p (187 segs) while `lp` fell 20.6 -> 18.7.  Cutting `lp` by
       putting the cost back into `pr` would have been the trade the owner rejects; this is the
       version that does not make it. */
    sat_tier_t t_mid, t_up, t_lo;
    if (midtexture)
	t_mid = (sat_tier_t){ midtexture,    rw_midtexturemid,    io_flat_mid, io_col_mid,
			      wall_solid, sat_wcl_mid, &sat_wcl_mid_ef, &sat_lead_mid };
    else
    {
	if (toptexture)
	    t_up = (sat_tier_t){ toptexture,    rw_toptexturemid,    io_flat_up, io_col_up,
				 wall_solid, sat_wcl_up,  &sat_wcl_up_ef,  &sat_lead_up };
	if (bottomtexture)
	    t_lo = (sat_tier_t){ bottomtexture, rw_bottomtexturemid, io_flat_lo, io_col_lo,
				 wall_solid, sat_wcl_lo,  &sat_wcl_lo_ef,  &sat_lead_lo };
    }
    RP_SegRoutMark ();
    sat_gc_site = 1;   /* SATURN row 16 `GCS`: from here on, the per-column loop owns the calls */
    for ( ; rw_x < rw_stopx ; rw_x++)
    {
	/* SATURN L5: the CPU border columns of an edge-split wall (sat_we_on, armed in the claim
	   block above).  Disarmed -> a constant 0 the compiler folds away, exactly like the old
	   SAT_WALL_EDGE_FILL enum it replaces.  DoomJo / VDP1-off never arm it. */
	int is_edge = sat_we_on && (rw_x <= sat_we_lo || rw_x >= sat_we_hi);
	if (sat_dbg_overlay_mode == 0) prof_seg_cols++;   /* SATURN row 14 `SEG`: the loop's trip count (gated 2026-08-22) */
	// mark floor / ceiling areas
	yl = SAT_SHR12 (topfrac+HEIGHTUNIT-1);

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
		
	yh = SAT_SHR12 (bottomfrac);

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
		sat_tier_draw (&t_mid, yl, yh, texturecolumn);
	    else if (sat_lead_mid.on)
		sat_tier_lead (&t_mid, rw_x, yl, yh, texturecolumn);
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
		mid = SAT_SHR12 (pixhigh);
		pixhigh += pixhighstep;

		if (mid >= floorclip[rw_x])
		    mid = floorclip[rw_x]-1;

		if (mid >= yl)
		{
		    if (!sat_wall_skip || sat_sw_up || is_edge)   /* VDP1 owns it (unless close/transition); is_edge = edge-fill */
			sat_tier_draw (&t_up, yl, mid, texturecolumn);
		    else if (sat_lead_up.on)
			sat_tier_lead (&t_up, rw_x, yl, mid, texturecolumn);
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
		mid = SAT_SHR12 (pixlow+HEIGHTUNIT-1);
		pixlow += pixlowstep;

		// no space above wall?
		if (mid <= ceilingclip[rw_x])
		    mid = ceilingclip[rw_x]+1;
		
		if (mid <= yh)
		{
		    if (!sat_wall_skip || sat_sw_lo || is_edge)   /* VDP1 owns it (unless close/transition); is_edge = edge-fill */
			sat_tier_draw (&t_lo, mid, yh, texturecolumn);
		    else if (sat_lead_lo.on)
			sat_tier_lead (&t_lo, rw_x, mid, yh, texturecolumn);
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
	/* LEAD-FILL: step each armed tier's OLD quad edges one column, like the wedge edges above.
	   SATURN 2026-08-19: hoisted under ONE parked-check -- with the lead-fill parked
	   (sat_wall_lead_x == 0, no tier ever arms) these were 6 always-false tests per column
	   on the hottest loop in the game (~0.1-0.8 ms/frame of pure branch overhead). */
	if (sat_wall_lead_x)
	{
	    if (sat_lead_mid.on) { sat_lead_mid.ylf += sat_lead_mid.ylstep; sat_lead_mid.yhf += sat_lead_mid.yhstep; }
	    if (sat_lead_up.on)  { sat_lead_up.ylf  += sat_lead_up.ylstep;  sat_lead_up.yhf  += sat_lead_up.yhstep;  }
	    if (sat_lead_lo.on)  { sat_lead_lo.ylf  += sat_lead_lo.ylstep;  sat_lead_lo.yhf  += sat_lead_lo.yhstep;  }
	    if (sat_lead_mid.on2){ sat_lead_mid.ylfb += sat_lead_mid.ylstepb; sat_lead_mid.yhfb += sat_lead_mid.yhstepb; }
	    if (sat_lead_up.on2) { sat_lead_up.ylfb  += sat_lead_up.ylstepb;  sat_lead_up.yhfb  += sat_lead_up.yhstepb;  }
	    if (sat_lead_lo.on2) { sat_lead_lo.ylfb  += sat_lead_lo.ylstepb;  sat_lead_lo.yhfb  += sat_lead_lo.yhstepb;  }
	}
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
/* 🔴 SATURN 2026-08-24 -- THE QUEUE ITSELF IS COMPILED OUT, AND HERE IS WHY.
   walljobs[MAXDRAWSEGS] is 256 x 32 B = 8 192 BYTES OF .bss -- and the .bss sits between _end and
   the work area, so every byte of it is taken straight off the TLSF pool the game boots on (the
   pool was 15,2 KB on the TNT build that motivated this: the array was more than HALF of it).
   It can only ever be written when sat_wallprep_defer is non-zero, and that variable has exactly
   ONE writer in the whole tree -- the pad L+R diagnostic block in dg_saturn.cxx, which lives
   under `#if SAT_DIAG_SLAVE_TOGGLES` and that macro is 0.  So the queue is unreachable in every
   binary that has ever shipped, while its storage was paid in full.
   The defer harness stays here, intact, behind one flag: STEP 2 (the slave consumer) is still a
   live idea and this is its validation harness.
   ⚠ TO REVIVE IT YOU MUST FLIP **BOTH**: SAT_WALLPREP_DEFER here AND SAT_DIAG_SLAVE_TOGGLES in
   dg_saturn.cxx.  They cannot see each other (different translation units, different build
   systems), which is exactly why this warning is written in both places. */
#ifndef SAT_WALLPREP_DEFER
#define SAT_WALLPREP_DEFER 0
#endif
#if SAT_WALLPREP_DEFER
static walljob_t walljobs[MAXDRAWSEGS];
#endif
int  walljob_n = 0;
int  sat_wallprep_defer = 0;

void RP_QueueWall(int start, int stop)
{
#if !SAT_WALLPREP_DEFER
    R_StoreWallRange(start, stop);   /* queue compiled out -- see the note at walljobs[] */
    return;
#else
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
#endif
}

/* Replay queued walls [from,to) in BSP order (single in-order consumer => the floorclip/
   ceilingclip occlusion chain is identical).  Does NOT reset walljob_n -- the caller does, so
   the slave (RANK 3 inc-1, r_parallel.c) can flush a range without owning the master's counter. */
void RP_FlushWallsRange(int from, int to)
{
#if !SAT_WALLPREP_DEFER
    (void)from; (void)to;            /* nothing is ever queued -- see the note at walljobs[] */
#else
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
#endif
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
    
    sidedef = SEG_SIDEDEF(curline);
    linedef = SEG_LINEDEF(curline);

    // mark the segment as visible for auto map
    linedef->flags |= ML_MAPPED;
    
    // calculate rw_distance for scale calculation
    rw_normalangle = SEG_ANGLE(curline) + ANG90;
    offsetangle = abs(rw_normalangle-rw_angle1);
    
    if (offsetangle > ANG90)
	offsetangle = ANG90;

    distangle = ANG90 - offsetangle;
    hyp = R_PointToDist (SEG_V1(curline)->x, SEG_V1(curline)->y);
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

	    trx = SEG_V1(curline)->x - viewx;
	    try = SEG_V1(curline)->y - viewy;
			
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
	    r_opening_demand += rw_stopx - rw_x;   /* SATURN: DEMAND, counted on both branches */
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

	rw_offset += sidedef->textureoffset + SEG_OFFSET(curline);
	rw_centerangle = ANG90 + viewangle - rw_normalangle;
	
	// calculate light table
	//  use different light tables
	//  for horizontal / vertical / diagonal
	// OPTIMIZE: get rid of LIGHTSEGSHIFT globally
	if (!fixedcolormap)
	{
	    lightnum = (frontsector->lightlevel >> LIGHTSEGSHIFT)+extralight;

	    if (SEG_V1(curline)->y == SEG_V2(curline)->y)
		lightnum--;
	    else if (SEG_V1(curline)->x == SEG_V2(curline)->x)
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

    RP_WallHeadMark ();   /* SATURN 2026-08-25: closes row-4 `hd` -- everything since the wrapper
                             (scale + texture resolution, silhouettes, BOTH R_CheckPlane calls
                             just above) is the HEAD.  It was 24 % of the 4p `Bp`, unnamed. */
    RP_SegLoopEnter ();   /* SATURN PERF Phase-0a: bracket the per-column loop (c Bp) */
    R_RenderSegLoop ();
    RP_SegLoopLeave ();
    RP_WallTailMark ();   /* SATURN 2026-08-25: opens row-4 `tl` (the four openings memcpy + the
                             drawseg store below); closed by RP_WallPrepLeave in the wrapper. */
    sat_gc_site = 0;   /* SATURN row 16 `GCS`: back to "other" (r_plane.c's sky column, ...) */


    // save sprite clipping info
    if ( ((ds_p->silhouette & SIL_TOP) || maskedtexture)
	 && !ds_p->sprtopclip)
    {
	r_opening_demand += rw_stopx - start;   /* SATURN: DEMAND, counted on both branches */
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
	r_opening_demand += rw_stopx - start;   /* SATURN: DEMAND, counted on both branches */
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

