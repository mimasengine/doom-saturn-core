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
//	BSP traversal, handling of LineSegs for rendering.
//




/* SATURN: O3 for the BSP traversal unit — called every frame for every visible segment. */
#pragma GCC optimize("O3")

#include "doomdef.h"

#include "m_bbox.h"

#include "i_system.h"

#include "r_main.h"
#include "r_plane.h"
#include "r_things.h"

// State.
#include "doomstat.h"
#include "r_state.h"

//#include "r_local.h"



seg_t*		curline;
side_t*		sidedef;
line_t*		linedef;
sector_t*	frontsector;
sector_t*	backsector;

drawseg_t	drawsegs[MAXDRAWSEGS];
drawseg_t*	ds_p;

/* SATURN: running high-water of drawsegs used (ds_p - drawsegs), folded once per pass in
   R_ClearDrawSegs -- a mirror of r_visplane_peak.  Read + zeroed each window by the platform
   "limits" overlay (dg_saturn.cxx) to catch how close a big WAD gets to the MAXDRAWSEGS
   I_Error hard-halt (see docs/ENDGAME_ROADMAP.md Axis 2).  Running max, reset at the overlay
   window boundary; harmless when unread (0). */
int		r_drawseg_peak = 0;

/* SATURN: solidsegs[] high-water (vanilla MAXSEGS=32), folded in R_ClearClipSegs.  The array is
   immediately followed in .bss by newend / r_drawseg_peak / ds_p (build/Mimas.map), and vanilla
   R_ClipSolidWallSegment has NO overflow guard -> an over-budget view walks newend past solidsegs[31]
   and the insert-shuffle stomps those globals (ds_p wild -> HARD FREEZE on real HW; Ymir tolerates the
   wild write, ds peak reads ~1.4e9).  r_solidseg_ovf latches when the new guard drops a post; the peak
   caps at MAXSEGS.  Read on the platform "limits" overlay ('ss'). */
int		r_solidseg_peak = 0;
int		r_solidseg_ovf  = 0;


void
R_StoreWallRange
( int	start,
  int	stop );

/* SATURN parallel-REC: the clip functions queue wall ranges instead of running the
   wall-prep inline; RP_FlushWalls runs them after the BSP walk (r_segs.c). */
void RP_QueueWall (int start, int stop);
void RP_FlushWalls (void);




/* SATURN 2026-08-18: the one out-of-line resolver for seg_t's 16-bit backsector index.
   SEG_NOSECTOR = one-sided (NULL, which no index can express); SEG_NULLSECTOR = the vanilla
   "glass hack" static sector, which lives outside sectors[] and so needs a sentinel of its own
   rather than being folded into one-sided -- that would change what the renderer draws. */
sector_t* SEG_BACKSECTOR (const seg_t *s)
{
    if (s->bsi == SEG_NOSECTOR)
	return (sector_t *)0;
    if (s->bsi == SEG_NULLSECTOR)
	return GetSectorAtNullAddress ();
    return &sectors[s->bsi];
}

//
// R_ClearDrawSegs
//
void R_ClearDrawSegs (void)
{
    int n = (int)(ds_p - drawsegs);          /* SATURN: fold the prior pass's count into the peak */
    if (n > r_drawseg_peak) r_drawseg_peak = n;
    ds_p = drawsegs;
    /* SATURN LEAD-FILL: advance the per-frame quad-history ring -- it has exactly this lifetime,
       one view's front-to-back BSP walk, so it rides the drawsegs it reasons about.  1p only: in
       split both views share this call site and would interleave two histories into one ring. */
    {
	extern int sat_split_active;
	extern void sat_lead_frame_begin (void);
	if (!sat_split_active) sat_lead_frame_begin ();
    }
    /* SATURN per-frame TEXTURE LOAD BUDGET: refill here, the one call site every rendered frame
       passes through exactly once (split included -- the 4 views share one frame's disc budget,
       which is what we want: the budget bounds the FRAME's stall, not the view's). */
    {
	extern void R_LoadBudgetFrame (void);
	R_LoadBudgetFrame ();
    }
}



//
// ClipWallSegment
// Clips the given range of columns
// and includes it in the new clip list.
//
typedef	struct
{
    int	first;
    int last;
    
} cliprange_t;


#define MAXSEGS		32

// newend is one past the last valid seg
cliprange_t*	newend;
cliprange_t	solidsegs[MAXSEGS];




//
// R_ClipSolidWallSegment
// Does handle solid walls,
//  e.g. single sided LineDefs (middle texture)
//  that entirely block the view.
// 
void
R_ClipSolidWallSegment
( int			first,
  int			last )
{
    cliprange_t*	next;
    cliprange_t*	start;

    // Find the first range that touches the range
    //  (adjacent pixels are touching).
    start = solidsegs;
    while (start->last < first-1)
	start++;

    if (first < start->first)
    {
	if (last < start->first-1)
	{
	    // Post is entirely visible (above start),
	    //  so insert a new clippost.
	    RP_QueueWall (first, last);
	    next = newend;
	    /* SATURN overflow SINK (root-cause of the M7/lowres level-start HARD FREEZE): vanilla
	       MAXSEGS=32 solidsegs[] has NO bound here, so an over-budget view (>32 disjoint solid
	       posts) walks newend PAST solidsegs[31]; the shuffle below then writes solidsegs[32],
	       which the linker puts exactly on newend / r_drawseg_peak / ds_p (build/Mimas.map) ->
	       ds_p wild -> freeze on real HW (Ymir tolerates the wild write).  Drop the extra post
	       (like the visplane/opening sinks: at worst a HOM sliver, never a crash).  The wall was
	       already queued above, so nothing vanishes -- only its clip entry is skipped. */
	    if (newend == &solidsegs[MAXSEGS]) { r_solidseg_ovf = 1; return; }
	    newend++;

	    while (next != start)
	    {
		*next = *(next-1);
		next--;
	    }
	    next->first = first;
	    next->last = last;
	    return;
	}
		
	// There is a fragment above *start.
	RP_QueueWall (first, start->first - 1);
	// Now adjust the clip size.
	start->first = first;	
    }

    // Bottom contained in start?
    if (last <= start->last)
	return;			
		
    next = start;
    while (last >= (next+1)->first-1)
    {
	// There is a fragment between two posts.
	RP_QueueWall (next->last + 1, (next+1)->first - 1);
	next++;
	
	if (last <= next->last)
	{
	    // Bottom is contained in next.
	    // Adjust the clip size.
	    start->last = next->last;	
	    goto crunch;
	}
    }
	
    // There is a fragment after *next.
    RP_QueueWall (next->last + 1, last);
    // Adjust the clip size.
    start->last = last;
	
    // Remove start+1 to next from the clip list,
    // because start now covers their area.
  crunch:
    if (next == start)
    {
	// Post just extended past the bottom of one post.
	return;
    }
    

    while (next++ != newend)
    {
	// Remove a post.
	*++start = *next;
    }

    newend = start+1;
}



//
// R_ClipPassWallSegment
// Clips the given range of columns,
//  but does not includes it in the clip list.
// Does handle windows,
//  e.g. LineDefs with upper and lower texture.
//
void
R_ClipPassWallSegment
( int	first,
  int	last )
{
    cliprange_t*	start;

    // Find the first range that touches the range
    //  (adjacent pixels are touching).
    start = solidsegs;
    while (start->last < first-1)
	start++;

    if (first < start->first)
    {
	if (last < start->first-1)
	{
	    // Post is entirely visible (above start).
	    RP_QueueWall (first, last);
	    return;
	}
		
	// There is a fragment above *start.
	RP_QueueWall (first, start->first - 1);
    }

    // Bottom contained in start?
    if (last <= start->last)
	return;			
		
    while (last >= (start+1)->first-1)
    {
	// There is a fragment between two posts.
	RP_QueueWall (start->last + 1, (start+1)->first - 1);
	start++;
	
	if (last <= start->last)
	    return;
    }
	
    // There is a fragment after *next.
    RP_QueueWall (start->last + 1, last);
}



//
// R_ClearClipSegs
//
/* SATURN x-split foundation (parallel-REC / multiplayer, docs/MULTIPLAYER_PLAN.md).
   Confine the WHOLE render to a screen-x sub-range [sat_view_x0, sat_view_x1) by
   marking everything outside it as already-solid: the BSP walk and every downstream
   clip (visplanes, walls, sprites) then emit ONLY inside the range, while the
   full-width projection stays intact -- it is the left/right slice of the SAME view,
   so the perspective is exactly right.  This single chokepoint confines the whole
   pipeline; nothing else needs an x-range argument.
   Default x0=0, x1<=0 -> full [0,viewwidth) == vanilla (byte-identical; DoomJo and
   the single-pass 1p build never set these, so they are unaffected). */
int sat_view_x0 = 0;
int sat_view_x1 = 0;   /* <=0 or > viewwidth => viewwidth (full screen) */

void R_ClearClipSegs (void)
{
    int n = (int)(newend - solidsegs);          /* SATURN: fold the prior view's solidseg high-water */
    if (n > r_solidseg_peak) r_solidseg_peak = n;
    int x0 = sat_view_x0;
    int x1 = sat_view_x1;
    if (x0 < 0) x0 = 0;
    if (x1 <= 0 || x1 > viewwidth) x1 = viewwidth;

    solidsegs[0].first = -0x7fffffff;
    solidsegs[0].last = x0 - 1;          /* [.., x0-1] = solid (off to the left)  */
    solidsegs[1].first = x1;             /* [x1, ..]  = solid (off to the right)  */
    solidsegs[1].last = 0x7fffffff;
    newend = solidsegs+2;

#if SAT_PSW
    {   /* PSW portal bands (r_segs.c): fresh open bands for this walk.  This is
	   the one pre-walk chokepoint, same as solidsegs. */
	extern int  sat_psw_active;
	extern void R_PswBandsReset (void);
	if (sat_psw_active)
	    R_PswBandsReset ();
    }
#endif
}

//
// R_AddLine
// Clips the given segment
// and adds any visible pieces to the line list.
//
void R_AddLine (seg_t*	line)
{
    int			x1;
    int			x2;
    angle_t		angle1;
    angle_t		angle2;
    angle_t		span;
    angle_t		tspan;
    
    curline = line;

    // OPTIMIZE: quickly reject orthogonal back sides.
    angle1 = R_PointToAngle (SEG_V1(line)->x, SEG_V1(line)->y);
    angle2 = R_PointToAngle (SEG_V2(line)->x, SEG_V2(line)->y);
    
    // Clip to view edges.
    // OPTIMIZE: make constant out of 2*clipangle (FIELDOFVIEW).
    span = angle1 - angle2;
    
    // Back side? I.e. backface culling?
    if (span >= ANG180)
	return;		

    // Global angle needed by segcalc.
    rw_angle1 = angle1;
    angle1 -= viewangle;
    angle2 -= viewangle;
	
    tspan = angle1 + clipangle;
    if (tspan > 2*clipangle)
    {
	tspan -= 2*clipangle;

	// Totally off the left edge?
	if (tspan >= span)
	    return;
	
	angle1 = clipangle;
    }
    tspan = clipangle - angle2;
    if (tspan > 2*clipangle)
    {
	tspan -= 2*clipangle;

	// Totally off the left edge?
	if (tspan >= span)
	    return;	
	angle2 = -clipangle;
    }
    
    // The seg is in the view range,
    // but not necessarily visible.
    angle1 = (angle1+ANG90)>>ANGLETOFINESHIFT;
    angle2 = (angle2+ANG90)>>ANGLETOFINESHIFT;
    x1 = viewangletox[angle1];
    x2 = viewangletox[angle2];

    // Does not cross a pixel?
    if (x1 == x2)
	return;				
	
    backsector = SEG_BACKSECTOR(line);

    // Single sided line?
    if (!backsector)
	goto clipsolid;		

    // Closed door.
    if (backsector->ceilingheight <= frontsector->floorheight
	|| backsector->floorheight >= frontsector->ceilingheight)
	goto clipsolid;		

    // Window.
    if (backsector->ceilingheight != frontsector->ceilingheight
	|| backsector->floorheight != frontsector->floorheight)
	goto clippass;	
		
    // Reject empty lines used for triggers
    //  and special events.
    // Identical floor and ceiling on both sides,
    // identical light levels on both sides,
    // and no middle texture.
    if (backsector->ceilingpic == frontsector->ceilingpic
	&& backsector->floorpic == frontsector->floorpic
	&& backsector->lightlevel == frontsector->lightlevel
	&& SEG_SIDEDEF(curline)->midtexture == 0)
    {
	return;
    }
    
				
  clippass:
    R_ClipPassWallSegment (x1, x2-1);	
    return;
		
  clipsolid:
    R_ClipSolidWallSegment (x1, x2-1);
}


//
// R_CheckBBox
// Checks BSP node/subtree bounding box.
// Returns true
//  if some part of the bbox might be visible.
//
int	checkcoord[12][4] =
{
    {3,0,2,1},
    {3,0,2,0},
    {3,1,2,0},
    {0},
    {2,0,2,1},
    {0,0,0,0},
    {3,1,3,0},
    {0},
    {2,0,3,1},
    {2,1,3,1},
    {2,1,3,0}
};


/* SATURN 2026-08-16 -- ADAPTIVE VIEW DISTANCE: BUILT, MEASURED, REMOVED THE SAME DAY.
   It pruned any BSP subtree whose bounding box lay past 2048 map units, which is the only knob `Bw`
   has ever had and cut all four phases at once.  It worked -- row-24 `fc` counted 12 to 136 subtrees
   pruned per second -- and it was still WORTHLESS, for a reason no amount of tuning fixes:
   **`fc` read 0 on the three heaviest captures.**  The expensive scenes on TNT MAP11 are ENCLOSED;
   nothing in them is far enough to prune.  The clip fired only in the scenes that were already fast,
   while charging every frame a full-view memset (the backstop against pruned pixels showing last
   frame's picture).  Inert where it was needed, costly everywhere: measured-negative, not unproven.
   Keep this note so the idea is not re-derived: the prerequisite for a far clip is a map whose COST
   is in its DISTANCE.  Measure `fc` against fps before writing the clip, not after. */

/* SATURN 2026-08-18: takes the node's SHORT bbox directly (node_t is 28 bytes now).  The four
   comparisons shift the stored short up rather than the caller shifting all four into a temp --
   same arithmetic, no copy, and `viewx >> FRACBITS` would NOT be equivalent (it truncates the
   fractional part of the view position and would flip the test on a boundary). */
boolean R_CheckBBox (const short*	bspcoord)
{
    int			boxx;
    int			boxy;
    int			boxpos;

    fixed_t		x1;
    fixed_t		y1;
    fixed_t		x2;
    fixed_t		y2;
    
    angle_t		angle1;
    angle_t		angle2;
    angle_t		span;
    angle_t		tspan;
    
    cliprange_t*	start;

    int			sx1;
    int			sx2;
    
    // Find the corners of the box
    // that define the edges from current viewpoint.
    if (viewx <= ((fixed_t)bspcoord[BOXLEFT]   << FRACBITS))
	boxx = 0;
    else if (viewx < ((fixed_t)bspcoord[BOXRIGHT]  << FRACBITS))
	boxx = 1;
    else
	boxx = 2;
		
    if (viewy >= ((fixed_t)bspcoord[BOXTOP]    << FRACBITS))
	boxy = 0;
    else if (viewy > ((fixed_t)bspcoord[BOXBOTTOM] << FRACBITS))
	boxy = 1;
    else
	boxy = 2;
		
    boxpos = (boxy<<2)+boxx;
    if (boxpos == 5)
	return true;
	
    x1 = ((fixed_t)bspcoord[checkcoord[boxpos][0]] << FRACBITS);
    y1 = ((fixed_t)bspcoord[checkcoord[boxpos][1]] << FRACBITS);
    x2 = ((fixed_t)bspcoord[checkcoord[boxpos][2]] << FRACBITS);
    y2 = ((fixed_t)bspcoord[checkcoord[boxpos][3]] << FRACBITS);
    
    // check clip list for an open space
    angle1 = R_PointToAngle (x1, y1) - viewangle;
    angle2 = R_PointToAngle (x2, y2) - viewangle;
	
    span = angle1 - angle2;

    // Sitting on a line?
    if (span >= ANG180)
	return true;
    
    tspan = angle1 + clipangle;

    if (tspan > 2*clipangle)
    {
	tspan -= 2*clipangle;

	// Totally off the left edge?
	if (tspan >= span)
	    return false;	

	angle1 = clipangle;
    }
    tspan = clipangle - angle2;
    if (tspan > 2*clipangle)
    {
	tspan -= 2*clipangle;

	// Totally off the left edge?
	if (tspan >= span)
	    return false;
	
	angle2 = -clipangle;
    }


    // Find the first clippost
    //  that touches the source post
    //  (adjacent pixels are touching).
    angle1 = (angle1+ANG90)>>ANGLETOFINESHIFT;
    angle2 = (angle2+ANG90)>>ANGLETOFINESHIFT;
    sx1 = viewangletox[angle1];
    sx2 = viewangletox[angle2];

    // Does not cross a pixel.
    if (sx1 == sx2)
	return false;			
    sx2--;
	
    start = solidsegs;
    while (start->last < sx2)
	start++;
    
    if (sx1 >= start->first
	&& sx2 <= start->last)
    {
	// The clippost contains the new span.
	return false;
    }

    return true;
}



/* ============================================================================
 * SATURN PSW (psw-world experiment, step 2 -- docs/PSW_WORLD_PLAN.md).
 * Subsector POLYGONS, built lazily once per level: the BSP guarantees every
 * subsector is convex, but its segs alone do not close the polygon (the implicit
 * edges cut by ancestor splitlines are not stored).  Reconstruction = clip the
 * padded map bbox by every ancestor splitline along the leaf's path (keep-side),
 * then by the subsector's own seg lines (a seg always FACES its subsector, so
 * the interior is its front side).  Two identical walks: count then fill, into
 * one PU_LEVEL pool (auto-freed at the next P_SetupLevel).  The platform fans
 * each polygon into DISTORSP quads (stretched full flat) in painter order.
 * Compiled out entirely unless SAT_PSW. */
#ifndef SAT_PSW
#define SAT_PSW 0
#endif
#if SAT_PSW
#include "z_zone.h"                  /* Z_Malloc / PU_LEVEL (polygon pools)     */
extern int sat_psw_active;           /* platform: latched at the frame boundary */
extern int firstflat;                /* r_data.c: flat lump base                */
extern int *flattranslation;         /* r_data.c: animated-flat indirection     */
/* platform recorder: (subnum, floor_h, ceil_h, floorpic, floor_lump, ceil_lump
   [-1 = sky], lightlevel, vis0 = live vissprite count), called ONCE per visited
   subsector BEFORE its walls are queued AND before R_AddSprites, in BSP visit
   order = near-first -- so vis0 is a per-subsector sprite watermark exactly
   like the platform's wall_acc one. */
void (*sat_psw_sub_hook)(int subnum, int fh, int ch, int fpic,
                         int flump, int clump, int light, int vis0) = 0;

/* floor height at a world point (the platform's pit-visibility cull: one
   sightline/dominant-crossing test per tile).  Pure node walk, no allocation --
   safe at flush time. */
int R_PswFloorAt (fixed_t x, fixed_t y)
{
    return R_PointInSubsector (x, y)->sector->floorheight;
}

/* ceiling twin (the symmetric cull).  A SKY ceiling occludes at its height too:
   that is exactly Doom's sky-hack convention (nothing shows above a sky edge). */
int R_PswCeilingAt (fixed_t x, fixed_t y)
{
    return R_PointInSubsector (x, y)->sector->ceilingheight;
}

/* SATURN PSW round 26 -- THE OVERDRAW MODEL IS DEAD (owner: "les murs doivent
   s'afficher par dessus les plans, a distance equivalente au moins").  Rounds
   22-25 let a full grid square overdraw its leaf border under a "something
   covers it" rule (facing walls / risers / nearer subs / soft-line
   classification), and every version of that rule leaked, for measured
   reasons: BSP order between side-by-side subs is arbitrary (the covering
   wall may emit FIRST), the seg list is structurally blind to sector borders
   on bare BSP splitlines (a partition picked ALONG a linedef leaves the far
   cell segless there), zero-thickness double-faced walls hide real content
   behind "one-sided = covered", and every static classification of "what
   lies beyond an edge" is a sampling (offline sweeps: 248-1092 leaking
   borders/tiles across the 9 shareware maps depending on the rule).  The
   platform emitter now enforces CONTAINMENT instead: a 1-cmd full square or
   strip only for tiles FULLY INSIDE the leaf polygon, the exact clipped
   piece for every border tile -- a flat can never paint a texel outside its
   own leaf, so it can never touch a wall, on any WAD, in any order.  The
   soft-line machinery (R_PswSoftLines + the round-26 edge-neighbour probes)
   is deleted; the pools below are the whole contract. */
int             psw_polys_ok = 0;    /* 1 = pools below are valid for this level */
fixed_t        *psw_pvx = 0, *psw_pvy = 0;   /* vertex pool (world, 16.16)      */
unsigned short *psw_pvi = 0;         /* per-subsector: pool start index          */
unsigned char  *psw_pvn = 0;         /* per-subsector: vertex count (0 = none)   */
static void    *psw_poly_level = 0;  /* level identity = subsectors[] pointer    */

#define PSW_POLY_VMAX 20
#define PSW_CLIP_VMAX 40
#define PSW_SUBS_MAX  900            /* big-WAD guard: beyond this, walls-only PSW */
#define PSW_VERT_MAX  9000           /* pool guard (~72 KB of PU_LEVEL zone)       */
#define PSW_PATH_MAX  96

typedef struct { fixed_t ox, oy, dx, dy; } pswclip_t;   /* keep: dx*(y-oy)-dy*(x-ox) <= 0 */
static pswclip_t psw_path[PSW_PATH_MAX];
static int       psw_depth;
static fixed_t   psw_bbx0, psw_bby0, psw_bbx1, psw_bby1;
static int       psw_vtotal, psw_fillpos, psw_pass;

/* Sutherland-Hodgman against one keep-line; 64-bit crosses (load-time only). */
static int psw_clip_line (const pswclip_t *L, const fixed_t *ax, const fixed_t *ay,
                          int n, fixed_t *bx, fixed_t *by)
{
    int i, m = 0;
    long long ca, cb;
    if (n < 3) return 0;
    ca = (long long)L->dx * (ay[0] - L->oy) - (long long)L->dy * (ax[0] - L->ox);
    for (i = 0; i < n; ++i)
    {
	int j = (i + 1 == n) ? 0 : i + 1;
	cb = (long long)L->dx * (ay[j] - L->oy) - (long long)L->dy * (ax[j] - L->ox);
	if (ca <= 0 && m < PSW_CLIP_VMAX) { bx[m] = ax[i]; by[m] = ay[i]; m++; }
	if ((ca <= 0) != (cb <= 0))
	{
	    long long d = ca - cb;
	    if (d != 0 && m < PSW_CLIP_VMAX)
	    {
		/* crosses of 16.16 deltas reach ~2^57 on a real map, so ca<<16 would
		   overflow 64 bits; shrink both equally first (|d| >= |ca| when the
		   signs differ, so nd cannot hit zero before na). */
		long long na = ca, nd = d;
		fixed_t t;
		while (na >= (1LL << 46) || na <= -(1LL << 46)) { na >>= 8; nd >>= 8; }
		t = nd ? (fixed_t)((na << 16) / nd) : 0;        /* 16.16, 0..1 */
		if (t < 0) t = 0; else if (t > FRACUNIT) t = FRACUNIT;
		bx[m] = ax[i] + (fixed_t)(((long long)(ax[j] - ax[i]) * t) >> 16);
		by[m] = ay[i] + (fixed_t)(((long long)(ay[j] - ay[i]) * t) >> 16);
		m++;
	    }
	}
	ca = cb;
    }
    return m;
}

/* n > cap: remove the FLATTEST corners (smallest |cross| at the vertex), one at
   a time.  The old `n = PSW_POLY_VMAX` tail-chop closed the polygon with a
   CHORD from vertex cap-1 back to vertex 0 and cut a whole WEDGE out of the
   subsector -- a fixed-spot floor/ceiling hole on every big leaf (console
   2026-09-02, "trous dans les plafonds": ceilings show it, the dominant floor
   hides it under RBG0).  Load-time only, O(n^2) is fine. */
static int psw_poly_shave (fixed_t *ax, fixed_t *ay, int n, int cap)
{
    while (n > cap)
    {
	int i, best = 0;
	long long bestc = -1;
	for (i = 0; i < n; ++i)
	{
	    int p = (i == 0) ? n - 1 : i - 1;
	    int j = (i + 1 == n) ? 0 : i + 1;
	    long long c = (long long)(ax[i] - ax[p]) * (ay[j] - ay[i])
	                - (long long)(ay[i] - ay[p]) * (ax[j] - ax[i]);
	    if (c < 0) c = -c;
	    if (bestc < 0 || c < bestc) { bestc = c; best = i; }
	}
	for (i = best; i + 1 < n; ++i) { ax[i] = ax[i + 1]; ay[i] = ay[i + 1]; }
	n--;
    }
    return n;
}

static void psw_leaf_poly (int num)
{
    static fixed_t wxa[PSW_CLIP_VMAX], wya[PSW_CLIP_VMAX];
    static fixed_t wxb[PSW_CLIP_VMAX], wyb[PSW_CLIP_VMAX];
    fixed_t *ax = wxa, *ay = wya, *bx = wxb, *by = wyb, *sw;
    subsector_t *sub = &subsectors[num];
    int n = 4, i;
    ax[0] = psw_bbx0; ay[0] = psw_bby0;
    ax[1] = psw_bbx1; ay[1] = psw_bby0;
    ax[2] = psw_bbx1; ay[2] = psw_bby1;
    ax[3] = psw_bbx0; ay[3] = psw_bby1;
    for (i = 0; i < psw_depth && n >= 3; ++i)
    {
	n = psw_clip_line(&psw_path[i], ax, ay, n, bx, by);
	sw = ax; ax = bx; bx = sw;  sw = ay; ay = by; by = sw;
	if (n > PSW_CLIP_VMAX - 6)                  /* headroom: psw_clip_line must
	                                               never hit ITS cap (same silent
	                                               vertex-drop corruption) */
	    n = psw_poly_shave(ax, ay, n, PSW_CLIP_VMAX - 6);
    }
    for (i = 0; i < sub->numlines && n >= 3; ++i)
    {
	seg_t *sg = &segs[sub->firstline + i];
	pswclip_t L;
	L.ox = SEG_V1(sg)->x;             L.oy = SEG_V1(sg)->y;
	L.dx = SEG_V2(sg)->x - L.ox;      L.dy = SEG_V2(sg)->y - L.oy;
	n = psw_clip_line(&L, ax, ay, n, bx, by);
	sw = ax; ax = bx; bx = sw;  sw = ay; ay = by; by = sw;
	if (n > PSW_CLIP_VMAX - 6)
	    n = psw_poly_shave(ax, ay, n, PSW_CLIP_VMAX - 6);
    }
    if (n > PSW_POLY_VMAX)
	n = psw_poly_shave(ax, ay, n, PSW_POLY_VMAX);
    if (n < 3) n = 0;
    if (psw_pass == 0) { psw_vtotal += n; return; }
    psw_pvi[num] = (unsigned short)psw_fillpos;
    psw_pvn[num] = (unsigned char)n;
    for (i = 0; i < n; ++i) { psw_pvx[psw_fillpos + i] = ax[i]; psw_pvy[psw_fillpos + i] = ay[i]; }
    psw_fillpos += n;
}

static void psw_poly_walk (int bspnum)
{
    node_t *bsp;
    if (bspnum & NF_SUBSECTOR)
    {
	psw_leaf_poly(bspnum == -1 ? 0 : (bspnum & ~NF_SUBSECTOR));
	return;
    }
    bsp = &nodes[bspnum];
    if (psw_depth < PSW_PATH_MAX)
    {
	pswclip_t *L = &psw_path[psw_depth++];
	L->ox = NODE_X(bsp);  L->oy = NODE_Y(bsp);
	/* front child (0): R_PointOnSide returns front when ndx*dy - ndy*dx < 0,
	   which is exactly the keep-rule's cross <= 0 (boundary given to both sides
	   = a hairline overlap, harmless). */
	L->dx = NODE_DX(bsp); L->dy = NODE_DY(bsp);
	psw_poly_walk(bsp->children[0]);
	L->dx = -L->dx; L->dy = -L->dy;                 /* back child: keep the other side */
	psw_poly_walk(bsp->children[1]);
	psw_depth--;
    }
    else
    {   /* path overflow (pathological BSP depth): children go unclipped by this line --
	   their polygons come out too large; the platform's near-clip + painter order
	   absorb it (oversize flats are overdrawn by nearer geometry). */
	psw_poly_walk(bsp->children[0]);
	psw_poly_walk(bsp->children[1]);
    }
}

/* (Re)build for the current level; called once per PSW frame from R_DrawPlanes'
   PSW block (cheap identity check after the first build). */
void R_PswPolysEnsure (void)
{
    int i;
    if (psw_poly_level == (void *)subsectors) return;
    psw_poly_level = (void *)subsectors;
    psw_polys_ok = 0;
    psw_pvx = psw_pvy = 0; psw_pvi = 0; psw_pvn = 0;    /* old blocks died with PU_LEVEL */
    if (numsubsectors < 1 || numsubsectors > PSW_SUBS_MAX || numnodes < 1) return;
    psw_bbx0 = psw_bby0 = 0x7fffffff; psw_bbx1 = psw_bby1 = (fixed_t)0x80000000;
    for (i = 0; i < numvertexes; ++i)
    {
	if (vertexes[i].x < psw_bbx0) psw_bbx0 = vertexes[i].x;
	if (vertexes[i].x > psw_bbx1) psw_bbx1 = vertexes[i].x;
	if (vertexes[i].y < psw_bby0) psw_bby0 = vertexes[i].y;
	if (vertexes[i].y > psw_bby1) psw_bby1 = vertexes[i].y;
    }
    psw_bbx0 -= 128 << FRACBITS; psw_bby0 -= 128 << FRACBITS;
    psw_bbx1 += 128 << FRACBITS; psw_bby1 += 128 << FRACBITS;
    psw_pass = 0; psw_vtotal = 0; psw_depth = 0;
    psw_poly_walk(numnodes - 1);
    if (psw_vtotal < 3 || psw_vtotal > PSW_VERT_MAX) return;
    psw_pvx = Z_Malloc(psw_vtotal * (int)sizeof(fixed_t), PU_LEVEL, 0);
    psw_pvy = Z_Malloc(psw_vtotal * (int)sizeof(fixed_t), PU_LEVEL, 0);
    psw_pvi = Z_Malloc(numsubsectors * (int)sizeof(unsigned short), PU_LEVEL, 0);
    psw_pvn = Z_Malloc(numsubsectors, PU_LEVEL, 0);
    memset(psw_pvn, 0, numsubsectors);
    psw_pass = 1; psw_fillpos = 0; psw_depth = 0;
    psw_poly_walk(numnodes - 1);
    psw_polys_ok = 1;
}
#endif /* SAT_PSW */

//
// R_Subsector
// Determine floor/ceiling planes.
// Add sprites of things in sector.
// Draw one or more line segments.
//
void R_Subsector (int num)
{
    int			count;
    seg_t*		line;
    subsector_t*	sub;
	
#ifdef RANGECHECK
    if (num>=numsubsectors)
	I_Error ("R_Subsector: ss %i with numss = %i",
		 num,
		 numsubsectors);
#endif

    sscount++;
    sub = &subsectors[num];
    frontsector = sub->sector;
    count = sub->numlines;
    line = &segs[sub->firstline];

#if SAT_PSW
    /* PSW: record the visit (near-first painter order) BEFORE this subsector's walls
       are queued -- the platform interleaves its floor/ceiling quads with its wall
       slice at flush time.  Ceiling lump -1 = sky (never emitted). */
    if (sat_psw_active && sat_psw_sub_hook)
	sat_psw_sub_hook(num,
	                 frontsector->floorheight, frontsector->ceilingheight,
	                 frontsector->floorpic,
	                 firstflat + flattranslation[frontsector->floorpic],
	                 (frontsector->ceilingpic == skyflatnum) ? -1
	                     : firstflat + flattranslation[frontsector->ceilingpic],
	                 frontsector->lightlevel,
	                 (int)(vissprite_p - vissprites));
#endif

    if (frontsector->floorheight < viewz)
    {
	floorplane = R_FindPlane (frontsector->floorheight,
				  frontsector->floorpic,
				  frontsector->lightlevel,
				  (int)(frontsector - sectors));   /* SATURN: plane identity */
    }
    else
	floorplane = NULL;
    
    if (frontsector->ceilingheight > viewz 
	|| frontsector->ceilingpic == skyflatnum)
    {
	ceilingplane = R_FindPlane (frontsector->ceilingheight,
				    frontsector->ceilingpic,
				    frontsector->lightlevel,
				    (int)(frontsector - sectors));  /* SATURN: plane identity */
    }
    else
	ceilingplane = NULL;

    /* (RP_Subsector REMOVED 2026-08-26 -- dead work.  It ran ONCE PER VISIBLE SUBSECTOR to size
       the "all floors/ceilings as VDP1 quads" study; that study's row-20 fields were cut and
       nothing has read sat_prof_ss_* since.  The pari-A numbers live in the study doc.) */

    R_AddSprites (frontsector);

    while (count--)
    {
	R_AddLine (line);
	line++;
    }
}




//
// RenderBSPNode
// Renders all subsectors below a given node,
//  traversing subtree recursively.
// Just call with BSP root.
void R_RenderBSPNode (int bspnum)
{
    node_t*	bsp;
    int		side;

    // Found a subsector?
    if (bspnum & NF_SUBSECTOR)
    {
	if (bspnum == -1)			
	    R_Subsector (0);
	else
	    R_Subsector (bspnum&(~NF_SUBSECTOR));
	return;
    }
		
    bsp = &nodes[bspnum];
    
    // Decide which side the view point is on.
    side = R_PointOnSide (viewx, viewy, bsp);

    // Recursively divide front space.
    R_RenderBSPNode (bsp->children[side]); 

    // Possibly divide back space.
    if (R_CheckBBox (bsp->bbox16[side^1]))	
	R_RenderBSPNode (bsp->children[side^1]);
}


