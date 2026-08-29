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
//	Rendering main loop and setup functions,
//	 utility functions (BSP, geometry, trigonometry).
//	See tables.c, too.
//





/* SATURN: O3 — R_PointToAngle (SlopeDiv hot), R_PointOnSide (FixedMul), R_ScaleFromGlobalAngle */
#pragma GCC optimize("O3")

#include <stdlib.h>
#include <math.h>


#include "doomdef.h"
#include "d_loop.h"

#include "m_bbox.h"
#include "m_menu.h"

#include "r_local.h"
#include "r_sky.h"
#include "r_flatcache.h"





// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW		2048	

/* SATURN 2026-08-25 -- LIVE FIELD OF VIEW (platform chord pad L+Y, row-7 `f<deg>`).
   WHY IT EXISTS: FIELDOFVIEW is a compile-time constant and focallength is built on
   centerxfrac = viewwidth/2, so a 160-px SPLIT QUADRANT still shows a full 90 degrees.
   Four views therefore accept four complete 90-degree arcs to paint 1.00x the pixels of
   the 1p view -- and `d` (drawsegs) is the term the split law says inflates with player
   count.  Narrowing the arc narrows clipangle (= xtoviewangle[0], set at the bottom of
   R_InitTextureMapping), so the BSP genuinely accepts fewer segs: that is the mechanism,
   and it is why the lever is falsifiable by a COUNT.

   sat_fov_mul is tan(45deg)/tan(half) -- the RATIO of the vanilla focal to the new one.
   It must be a ratio and not a focal, because vanilla is already inconsistent here:
   projection is centerxfrac while focallength divides centerxfrac by
   finetangent[FINEANGLES/4+1024], and that entry is 65185, NOT 65536 (counted in
   core/tables.c: the table is sampled at the MIDDLE of each fine step, so tan(45 deg) is
   not one of its samples).  Setting projection = focallength would therefore have moved
   the SHIPPED image by 0.54 %, silently, on a change advertised as default-off.  As a
   ratio, sat_fov_mul is EXACTLY FRACUNIT at the default whatever the table holds, so
   every consumer below is bit-identical until the chord moves it -- which is what makes
   the 90-degree arm of the A/B a genuine identity rather than a near-miss.

   ⚠ PROBE-GRADE, NOT SHIPPABLE AS-IS.  The HW sky's scroll law is derived from the
   90-degree geometry ([[part5-hw-sky-split]]) and is NOT scaled here, so the sky will
   mis-track at any other setting.  That is cosmetic and does not touch `d`, which is the
   only thing this toggle is built to measure.  Same for slSetScreenDist (the RBG0 focal),
   which matters in 1p/2p only -- RBG0 is off in 3/4p, so the 4p reading is clean.
   ⚠ It is also a GAME change (less peripheral vision in versus), so it stays default-off
   and the owner judges it, not a benchmark. */
int     sat_fov_half = FIELDOFVIEW/2;   /* fine-angle HALF-fov; 1024 = 45 deg = 90 deg total */

/* [!] 2026-08-28 -- PER-PLAYER-COUNT DEFAULT, index = live count, [0] unused.  1024 = 90 deg,
   740 = 65.  BAKED: the platform's count-change block is now the ONLY writer -- the R+C chord and
   its 45-degree rung were removed the same day the values were chosen (owner: "supprime 45 et le
   toggle.  On garde ces defauts jusqu'a nouvel ordre").  Change a default HERE, rebuild.
   THE LAW BEHIND THE NUMBERS is Hor+ -- preserve the VERTICAL fov and let the horizontal follow the
   viewport aspect -- which Halo's published figures show Bungie applying to the degree: CE 70 deg
   at 4:3 becomes 108 at 8:3, Halo 2 62 becomes 44 at 8:9, and in BOTH the implied vertical fov is
   preserved within one degree.  Mimas' 1p 3D area is 320x168 with 1,2:1 pixels => aspect 1,59 and
   V = 64,5 deg.  Carried across: a 3/4p quadrant (160x96 after the HUD band) wants ~82 deg, and the
   2p side-by-side view (160x208, aspect 0,64) wants ~44 -- so the CORRECT values DISAGREE between
   the split modes, and 2p is where the shipped 90 is most wrong: its vertical fov is 109 deg today
   against 1p's 64.
   SHIPPED CHOICE: one value for every split mode, 65.  The owner's call and it is the right one --
   in versus an fov difference is a competitive advantage, so it must be identical for every player
   and stable between rounds; consistent aiming across 2p/3p/4p beats per-mode geometric purity. */
int     sat_fov_mode[5] = { 1024, 1024, 740, 740, 740 };
fixed_t sat_fov_mul  = FRACUNIT;        /* tan(45)/tan(half); EXACTLY FRACUNIT at the default */



int			viewangleoffset;

// increment every time a check is made
int			validcount = 1;		


lighttable_t*		fixedcolormap;
extern lighttable_t**	walllights;

int			centerx;
int			centery;

fixed_t			centerxfrac;
fixed_t			centeryfrac;
fixed_t			projection;

// just for profiling purposes
int			framecount;	

int			sscount;
int			linecount;
int			loopcount;

fixed_t			viewx;
fixed_t			viewy;
fixed_t			viewz;

angle_t			viewangle;

fixed_t			viewcos;
fixed_t			viewsin;

player_t*		viewplayer;

/* SATURN 2026-08-17: the sector the eye is in, republished every frame for the LOD governor's
   PAROLE (r_parallel.c).  Owner: *"il faut lui rendre les leviers si on change de secteur.  Ce qui
   est vrai dans une partie de niveau ne l'est pas partout."*  Stored as a bare pointer used ONLY
   as an identity token -- no dereference, no header dependency, and the game already maintains it
   (P_SetThingPosition), so this costs one store per frame and no BSP descent.  DoomJo-safe. */
void*			sat_view_sector = NULL;

// 0 = high, 1 = low
int			detailshift;

//
// precalculated math tables
//
angle_t			clipangle;

// The viewangletox[viewangle + FINEANGLES/4] lookup
// maps the visible view angles to screen X coordinates,
// flattening the arc to a flat projection plane.
// There will be many angles mapped to the same X. 
int			viewangletox[FINEANGLES/2];

// The xtoviewangleangle[] table maps a screen pixel
// to the lowest viewangle that maps back to x ranges
// from clipangle to -clipangle.
angle_t			xtoviewangle[SCREENWIDTH+1];

lighttable_t*		scalelight[LIGHTLEVELS][MAXLIGHTSCALE];
lighttable_t*		scalelightfixed[MAXLIGHTSCALE];
lighttable_t*		zlight[LIGHTLEVELS][MAXLIGHTZ];

// bumped light from gun blasts
int			extralight;			



void (*colfunc) (void);
void (*basecolfunc) (void);
void (*fuzzcolfunc) (void);
void (*transcolfunc) (void);
void (*spanfunc) (void);



//
// R_AddPointToBox
// Expand a given bbox
// so that it encloses a given point.
//
void
R_AddPointToBox
( int		x,
  int		y,
  fixed_t*	box )
{
    if (x< box[BOXLEFT])
	box[BOXLEFT] = x;
    if (x> box[BOXRIGHT])
	box[BOXRIGHT] = x;
    if (y< box[BOXBOTTOM])
	box[BOXBOTTOM] = y;
    if (y> box[BOXTOP])
	box[BOXTOP] = y;
}


//
// R_PointOnSide
// Traverse BSP (sub) tree,
//  check point against partition plane.
// Returns side 0 (front) or 1 (back).
//
int
R_PointOnSide
( fixed_t	x,
  fixed_t	y,
  node_t*	node )
{
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    if (!NODE_DX(node))
    {
	if (x <= NODE_X(node))
	    return NODE_DY(node) > 0;
	
	return NODE_DY(node) < 0;
    }
    if (!NODE_DY(node))
    {
	if (y <= NODE_Y(node))
	    return NODE_DX(node) < 0;
	
	return NODE_DX(node) > 0;
    }
	
    dx = (x - NODE_X(node));
    dy = (y - NODE_Y(node));
	
    // Try to quickly decide by looking at sign bits.
    if ( (NODE_DY(node) ^ NODE_DX(node) ^ dx ^ dy)&0x80000000 )
    {
	if  ( (NODE_DY(node) ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( NODE_DY(node)>>FRACBITS , dx );
    right = FixedMul ( dy , NODE_DX(node)>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}


int
R_PointOnSegSide
( fixed_t	x,
  fixed_t	y,
  seg_t*	line )
{
    fixed_t	lx;
    fixed_t	ly;
    fixed_t	ldx;
    fixed_t	ldy;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	left;
    fixed_t	right;
	
    lx = SEG_V1(line)->x;
    ly = SEG_V1(line)->y;

    ldx = SEG_V2(line)->x - lx;
    ldy = SEG_V2(line)->y - ly;
	
    if (!ldx)
    {
	if (x <= lx)
	    return ldy > 0;
	
	return ldy < 0;
    }
    if (!ldy)
    {
	if (y <= ly)
	    return ldx < 0;
	
	return ldx > 0;
    }
	
    dx = (x - lx);
    dy = (y - ly);
	
    // Try to quickly decide by looking at sign bits.
    if ( (ldy ^ ldx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (ldy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( ldy>>FRACBITS , dx );
    right = FixedMul ( dy , ldx>>FRACBITS );
	
    if (right < left)
    {
	// front side
	return 0;
    }
    // back side
    return 1;			
}


//
// R_PointToAngle
// To get a global angle from cartesian coordinates,
//  the coordinates are flipped until they are in
//  the first octant of the coordinate system, then
//  the y (<=x) is scaled and divided by x to get a
//  tangent (slope) value which is looked up in the
//  tantoangle[] table.

//




angle_t
R_PointToAngle
( fixed_t	x,
  fixed_t	y )
{	
    x -= viewx;
    y -= viewy;
    
    if ( (!x) && (!y) )
	return 0;

    if (x>= 0)
    {
	// x >=0
	if (y>= 0)
	{
	    // y>= 0

	    if (x>y)
	    {
		// octant 0
		return tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 1
		return ANG90-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 8
		return -tantoangle[SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 7
		return ANG270+tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    else
    {
	// x<0
	x = -x;

	if (y>= 0)
	{
	    // y>= 0
	    if (x>y)
	    {
		// octant 3
		return ANG180-1-tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		// octant 2
		return ANG90+ tantoangle[ SlopeDiv(x,y)];
	    }
	}
	else
	{
	    // y<0
	    y = -y;

	    if (x>y)
	    {
		// octant 4
		return ANG180+tantoangle[ SlopeDiv(y,x)];
	    }
	    else
	    {
		 // octant 5
		return ANG270-1-tantoangle[ SlopeDiv(x,y)];
	    }
	}
    }
    return 0;
}


angle_t
R_PointToAngle2
( fixed_t	x1,
  fixed_t	y1,
  fixed_t	x2,
  fixed_t	y2 )
{	
    viewx = x1;
    viewy = y1;
    
    return R_PointToAngle (x2, y2);
}


fixed_t
R_PointToDist
( fixed_t	x,
  fixed_t	y )
{
    int		angle;
    fixed_t	dx;
    fixed_t	dy;
    fixed_t	temp;
    fixed_t	dist;
    fixed_t     frac;
	
    dx = abs(x - viewx);
    dy = abs(y - viewy);
	
    if (dy>dx)
    {
	temp = dx;
	dx = dy;
	dy = temp;
    }

    // Fix crashes in udm1.wad

    if (dx != 0)
    {
        frac = FixedDiv(dy, dx);
    }
    else
    {
	frac = 0;
    }
	
    angle = (tantoangle[frac>>DBITS]+ANG90) >> ANGLETOFINESHIFT;

    // use as cosine
    dist = FixedDiv (dx, finesine[angle] );	
	
    return dist;
}




//
// R_InitPointToAngle
//
void R_InitPointToAngle (void)
{
    // UNUSED - now getting from tables.c
#if 0
    int	i;
    long	t;
    float	f;
//
// slope (tangent) to angle lookup
//
    for (i=0 ; i<=SLOPERANGE ; i++)
    {
	f = atan( (float)i/SLOPERANGE )/(3.141592657*2);
	t = 0xffffffff*f;
	tantoangle[i] = t;
    }
#endif
}


//
// R_ScaleFromGlobalAngle
// Returns the texture mapping scale
//  for the current line (horizontal span)
//  at the given angle.
// rw_distance must be calculated first.
//
fixed_t R_ScaleFromGlobalAngle (angle_t visangle)
{
    fixed_t		scale;
    angle_t		anglea;
    angle_t		angleb;
    int			sinea;
    int			sineb;
    fixed_t		num;
    int			den;

    // UNUSED
#if 0
{
    fixed_t		dist;
    fixed_t		z;
    fixed_t		sinv;
    fixed_t		cosv;
	
    sinv = finesine[(visangle-rw_normalangle)>>ANGLETOFINESHIFT];	
    dist = FixedDiv (rw_distance, sinv);
    cosv = finecosine[(viewangle-visangle)>>ANGLETOFINESHIFT];
    z = abs(FixedMul (dist, cosv));
    scale = FixedDiv(projection, z);
    return scale;
}
#endif

    anglea = ANG90 + (visangle-viewangle);
    angleb = ANG90 + (visangle-rw_normalangle);

    // both sines are allways positive
    sinea = finesine[anglea>>ANGLETOFINESHIFT];	
    sineb = finesine[angleb>>ANGLETOFINESHIFT];
    num = FixedMul(projection,sineb)<<detailshift;
    den = FixedMul(rw_distance,sinea);

    if (den > num>>16)
    {
	scale = FixedDiv (num, den);

	if (scale > 64*FRACUNIT)
	    scale = 64*FRACUNIT;
	else if (scale < 256)
	    scale = 256;
    }
    else
	scale = 64*FRACUNIT;
	
    return scale;
}



//
// R_InitTables
//
void R_InitTables (void)
{
    // UNUSED: now getting from tables.c
#if 0
    int		i;
    float	a;
    float	fv;
    int		t;
    
    // viewangle tangent table
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	a = (i-FINEANGLES/4+0.5)*PI*2/FINEANGLES;
	fv = FRACUNIT*tan (a);
	t = fv;
	finetangent[i] = t;
    }
    
    // finesine table
    for (i=0 ; i<5*FINEANGLES/4 ; i++)
    {
	// OPTIMIZE: mirror...
	a = (i+0.5)*PI*2/FINEANGLES;
	t = FRACUNIT*sin (a);
	finesine[i] = t;
    }
#endif

}



//
// R_InitTextureMapping
//
void R_InitTextureMapping (void)
{
    int			i;
    int			x;
    int			t;
    fixed_t		focallength;
    
    // Use tangent table to generate viewangletox:
    //  viewangletox will give the next greatest x
    //  after the view angle.
    //
    // Calc focallength
    //  so FIELDOFVIEW angles covers SCREENWIDTH.
    focallength = FixedDiv (centerxfrac,
			    finetangent[FINEANGLES/4+sat_fov_half] );   /* SATURN: was FIELDOFVIEW/2 */
	
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	if (finetangent[i] > FRACUNIT*2)
	    t = -1;
	else if (finetangent[i] < -FRACUNIT*2)
	    t = viewwidth+1;
	else
	{
	    t = FixedMul (finetangent[i], focallength);
	    t = (centerxfrac - t+FRACUNIT-1)>>FRACBITS;

	    if (t < -1)
		t = -1;
	    else if (t>viewwidth+1)
		t = viewwidth+1;
	}
	viewangletox[i] = t;
    }
    
    // Scan viewangletox[] to generate xtoviewangle[]:
    //  xtoviewangle will give the smallest view angle
    //  that maps to x.	
    for (x=0;x<=viewwidth;x++)
    {
	i = 0;
	while (viewangletox[i]>x)
	    i++;
	xtoviewangle[x] = (i<<ANGLETOFINESHIFT)-ANG90;
    }
    
    // Take out the fencepost cases from viewangletox.
    for (i=0 ; i<FINEANGLES/2 ; i++)
    {
	t = FixedMul (finetangent[i], focallength);
	t = centerx - t;
	
	if (viewangletox[i] == -1)
	    viewangletox[i] = 0;
	else if (viewangletox[i] == viewwidth+1)
	    viewangletox[i]  = viewwidth;
    }
	
    clipangle = xtoviewangle[0];
}



//
// R_InitLightTables
// Only inits the zlight table,
//  because the scalelight table changes with view size.
//
#define DISTMAP		2

void R_InitLightTables (void)
{
    int		i;
    int		j;
    int		level;
    int		startmap; 	
    int		scale;
    
    // Calculate the light levels to use
    //  for each level / distance combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTZ ; j++)
	{
	    scale = FixedDiv ((SCREENWIDTH/2*FRACUNIT), (j+1)<<LIGHTZSHIFT);
	    scale >>= LIGHTSCALESHIFT;
	    level = startmap - scale/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    zlight[i][j] = colormaps + level*256;
	}
    }
}



//
// R_SetViewSize
// Do not really change anything here,
//  because it might be in the middle of a refresh.
// The change will take effect next refresh.
//
boolean		setsizeneeded;
int		setblocks;
int		setdetail;


void
R_SetViewSize
( int		blocks,
  int		detail )
{
    setsizeneeded = true;
    setblocks = blocks;
    setdetail = detail;
}


//
// R_ExecuteSetViewSize
//
/* SATURN split perf: R_SetViewWindow's size-dependent tables (R_InitTextureMapping +
   yslope/distscale/scalelight = ~74ms PER CALL on HW -- measured as the SPL `sw` term,
   the dominant 2-player cost) are cached on (w,h,detailshift) so the two SAME-size
   half-views don't recompute them twice every frame.  R_ExecuteSetViewSize (the 1p path)
   overwrites the same shared tables, so it invalidates this cache below. */
static int satvw_w = -1, satvw_h = -1, satvw_ds = -1;

// SATURN low-res 3D-view mode (docs/LOWRES_RENDER_STUDY.md): render the software
// framebuffer at HALF horizontal resolution (viewwidth 160, PACKED) so the platform
// can VDP2 hardware-x2-zoom NBG1 -> real fill+blit halving.  We force detailshift=1
// (=> viewwidth 160, and the VDP1 walls/weapon + RBG0 floor keep emitting at x<<1 =
// full 320, so they stay crisp and ALIGNED with the 2x-zoomed software) BUT keep the
// NORMAL (packed, 1 byte/column) drawers instead of the *Low duplicators -- that is
// what makes the framebuffer physically 160-wide.  Platform flips it via R_SetLowRes.
// DoomJo: default 0 = byte-identical (detailshift path untouched).
int sat_lowres = 0;
void R_SetLowRes (int on) { sat_lowres = on; setsizeneeded = true; }

/* SATURN 2026-08-25: the only writer of sat_fov_half.  Both view-setup paths cache their
   size-dependent tables (1p on setsizeneeded, split on the satvw_* triple), and NEITHER
   keys on the fov -- so a chord that only assigned the angle would change nothing until
   the next resize.  Invalidate both here, or the toggle silently does nothing and the
   A/B reads as "the lever is worth zero".  Clamped to never WIDEN past vanilla: the whole
   point is fewer accepted segs, and a wider arc would also overrun viewangletox. */
void R_SetFovHalf (int half)
{
    if (half < 256)              half = 256;               /* ~22.5 deg total, absurd but safe */
    if (half > FIELDOFVIEW/2)    half = FIELDOFVIEW/2;     /* never wider than vanilla 90 */
    if (half == sat_fov_half)    return;
    sat_fov_half = half;
    sat_fov_mul  = FixedDiv (finetangent[FINEANGLES/4+FIELDOFVIEW/2],
			     finetangent[FINEANGLES/4+half] );
    satvw_w = satvw_h = satvw_ds = -1;   /* split path: force R_SetViewWindow to rebuild */
    setsizeneeded = true;                /* 1p path: force R_ExecuteSetViewSize */
}

void R_ExecuteSetViewSize (void)
{
    fixed_t	cosadj;
    fixed_t	dy;
    int		i;
    int		j;
    int		level;
    int		startmap;

    setsizeneeded = false;
    satvw_w = -1;   /* invalidate R_SetViewWindow's cache: we overwrite the size tables */

    if (setblocks == 11)
    {
	scaledviewwidth = SCREENWIDTH;
	viewheight = SCREENHEIGHT;
    }
    else
    {
	scaledviewwidth = setblocks*32;
	viewheight = (setblocks*(SCREENHEIGHT-32)/10)&~7;   /* SATURN: 168->192 for 224 (bar=32) */
    }
    
    detailshift = sat_lowres ? 1 : setdetail;   /* SATURN: lowres forces the 160-col projection */
    viewwidth = scaledviewwidth>>detailshift;
	
    centery = viewheight/2;
    centerx = viewwidth/2;
    centerxfrac = centerx<<FRACBITS;
    centeryfrac = centery<<FRACBITS;
    projection = FixedMul (centerxfrac, sat_fov_mul);   /* SATURN fov: == centerxfrac at 90 deg */

    if (!detailshift || sat_lowres)
    {
	/* SATURN lowres: detailshift is 1 (for the 160-col projection + VDP1 x<<1) but we
	   use the NORMAL drawers -- they write ONE byte per logical column 0..159 = a
	   PACKED 160-wide framebuffer (the *Low drawers would re-duplicate back to 320). */
	colfunc = basecolfunc = R_DrawColumn;
	fuzzcolfunc = R_DrawFuzzColumn;
	transcolfunc = R_DrawTranslatedColumn;
	spanfunc = R_DrawSpan;
    }
    else
    {
	colfunc = basecolfunc = R_DrawColumnLow;
	fuzzcolfunc = R_DrawFuzzColumnLow;
	transcolfunc = R_DrawTranslatedColumnLow;
	spanfunc = R_DrawSpanLow;
    }

    R_InitBuffer (scaledviewwidth, viewheight);
	
    R_InitTextureMapping ();
    
    // psprite scales
    pspritescale = FRACUNIT*viewwidth/SCREENWIDTH;
    pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;
    
    // thing clipping
    for (i=0 ; i<viewwidth ; i++)
	screenheightarray[i] = viewheight;
    
    // planes
    for (i=0 ; i<viewheight ; i++)
    {
	dy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;
	dy = abs(dy);
	yslope[i] = FixedDiv ( FixedMul((viewwidth<<detailshift)/2*FRACUNIT, sat_fov_mul), dy);
    }
	
    for (i=0 ; i<viewwidth ; i++)
    {
	cosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
	distscale[i] = FixedDiv (FRACUNIT,cosadj);
    }
    
    // Calculate the light levels to use
    //  for each level / scale combination.
    for (i=0 ; i< LIGHTLEVELS ; i++)
    {
	startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	for (j=0 ; j<MAXLIGHTSCALE ; j++)
	{
	    level = startmap - j*SCREENWIDTH/(viewwidth<<detailshift)/DISTMAP;
	    
	    if (level < 0)
		level = 0;

	    if (level >= NUMCOLORMAPS)
		level = NUMCOLORMAPS-1;

	    scalelight[i][j] = colormaps + level*256;
	}
    }
}

/* SATURN split-screen (docs/MULTIPLAYER_PLAN.md Iter 2, §3.5): set an explicit viewport
   (origin wx,wy + size w,h) and recompute the size-dependent tables + the framebuffer
   pointers for it.  Same work as R_ExecuteSetViewSize but with an arbitrary origin (not
   centered) and an explicit w/h (not setblocks-coupled) -- so two views of half width can
   be rendered side by side.  Both views share the size, so for the common case the size
   tables are identical; only wx + columnofs/ylookup change between them. */
void R_SetViewWindow (int wx, int wy, int w, int h)
{
    extern byte *ylookup[];
    extern int   columnofs[];
    extern byte *I_VideoBuffer;
    fixed_t cosadj, dy;
    int i, j, level, startmap;

    viewwidth  = w >> detailshift;   /* low-detail: half the internal columns (screen span stays w) */
    viewheight = h;

    /* SATURN split perf: recompute the SIZE-dependent tables ONLY when (w,h,detailshift)
       changes.  Both half-views share the size, so this fires once (not 2x/frame) -- it
       was the ~148ms `sw` term that pinned the 2p split at ~5fps.  The per-view origin
       (columnofs/ylookup below) is always redone (cheap). */
    if (w != satvw_w || h != satvw_h || detailshift != satvw_ds)
    {
	satvw_w = w; satvw_h = h; satvw_ds = detailshift;

	centery = viewheight/2;
	centerx = viewwidth/2;
	centerxfrac = centerx<<FRACBITS;
	centeryfrac = centery<<FRACBITS;
	projection = FixedMul (centerxfrac, sat_fov_mul);   /* SATURN fov: == centerxfrac at 90 deg */

	R_InitTextureMapping ();                    /* xtoviewangle from centerx/viewwidth */

	pspritescale  = FRACUNIT*viewwidth/SCREENWIDTH;
	pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;

	for (i=0 ; i<viewwidth ; i++)
	    screenheightarray[i] = viewheight;
	for (i=0 ; i<viewheight ; i++)
	{
	    dy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;  dy = abs(dy);
	    yslope[i] = FixedDiv ( FixedMul((viewwidth<<detailshift)/2*FRACUNIT, sat_fov_mul), dy);
	}
	for (i=0 ; i<viewwidth ; i++)
	{
	    cosadj = abs(finecosine[xtoviewangle[i]>>ANGLETOFINESHIFT]);
	    distscale[i] = FixedDiv (FRACUNIT,cosadj);
	}
	for (i=0 ; i< LIGHTLEVELS ; i++)
	{
	    startmap = ((LIGHTLEVELS-1-i)*2)*NUMCOLORMAPS/LIGHTLEVELS;
	    for (j=0 ; j<MAXLIGHTSCALE ; j++)
	    {
		level = startmap - j*SCREENWIDTH/(viewwidth<<detailshift)/DISTMAP;
		if (level < 0) level = 0;
		if (level >= NUMCOLORMAPS) level = NUMCOLORMAPS-1;
		scalelight[i][j] = colormaps + level*256;
	    }
	}

    }

    /* SATURN: select the draw funcs (mirrors R_ExecuteSetViewSize).  HOISTED OUT of the
       size-table cache above: it must reflect sat_lowres, which is NOT in the cache key
       (w,h,detailshift).  Split low-detail and M7-lowres BOTH run at detailshift=1 but need
       DIFFERENT drawers -- duplicating *Low vs packed -- so a cache hit on unchanged
       (w,h,detailshift) must still update this.  Cheap (4 ptr writes/view).  M7-multi:
       sat_lowres picks the PACKED drawers at detailshift=1 so each viewport writes ONE byte
       per logical column (the *Low drawers would re-duplicate to full width, which the x2
       NBG1 zoom would then double AGAIN = garbage). */
    if (!detailshift || sat_lowres)
    {
	colfunc = basecolfunc = R_DrawColumn;
	fuzzcolfunc = R_DrawFuzzColumn;
	transcolfunc = R_DrawTranslatedColumn;
	spanfunc = R_DrawSpan;
    }
    else
    {
	colfunc = basecolfunc = R_DrawColumnLow;
	fuzzcolfunc = R_DrawFuzzColumnLow;
	transcolfunc = R_DrawTranslatedColumnLow;
	spanfunc = R_DrawSpanLow;
    }

    /* framebuffer pointers at the explicit origin -- ALWAYS (cheap, per-view).
       NON-lowres: columnofs base = viewwindowx (screen x), w entries (R_DrawColumnLow writes
       columnofs[dc_x<<1] and [+1]); at detailshift=0, w==viewwidth -> identity loop.
       M7-multi lowres: the software render is PACKED, so the base is viewwindowx>>1 (P2's
       screen origin 160 -> fb col 80) and the packed drawers index [0,viewwidth=80); the
       whole-layer x2 NBG1 zoom stretches fb[0,160) back to screen[0,320).  viewwindowx itself
       stays = wx (the FULL-res screen origin the UN-zoomed VDP1 walls/weapon/sprites read via
       (x<<detailshift)+viewwindowx -- keeping the two coordinate spaces distinct). */
    viewwindowx = wx;
    viewwindowy = wy;
    {
	int pack_base = sat_lowres ? (wx >> 1) : wx;
	for (i=0 ; i<w ; i++)
	    columnofs[i] = pack_base + i;
    }
    for (i=0 ; i<viewheight ; i++)
	ylookup[i] = I_VideoBuffer + (i+viewwindowy)*SCREENWIDTH;
}



//
// R_Init
//



void R_Init (void)
{
    R_InitData ();
    printf (".");
    R_InitPointToAngle ();
    printf (".");
    R_InitTables ();
    // viewwidth / viewheight / detailLevel are set by the defaults
    printf (".");

    R_SetViewSize (screenblocks, detailLevel);
    R_InitPlanes ();
    printf (".");
    R_InitLightTables ();
    printf (".");
    R_InitSkyMap ();
    R_InitTranslationTables ();
    /* SATURN 2026-08-29: claim the lead-fill rings HERE, while the zone is still empty, instead of
       on the first rendered frame of every level -- see R_LeadFillInit in r_segs.c.  Same bytes,
       placed low instead of dropped into the middle of the run P_SetupLevel leaves behind. */
    { extern void R_LeadFillInit (void); R_LeadFillInit (); }
    printf (".");
	
    framecount = 0;
}


//
// R_PointInSubsector
//
subsector_t*
R_PointInSubsector
( fixed_t	x,
  fixed_t	y )
{
    node_t*	node;
    int		side;
    int		nodenum;

    // single subsector is a special case
    if (!numnodes)				
	return subsectors;
		
    nodenum = numnodes-1;

    while (! (nodenum & NF_SUBSECTOR) )
    {
	node = &nodes[nodenum];
	side = R_PointOnSide (x, y, node);
	nodenum = node->children[side];
    }
	
    return &subsectors[nodenum & ~NF_SUBSECTOR];
}



//
// R_SetupFrame
//
/* SATURN split: re-point the view globals at player 0 (P1) so the post-render-loop RBG0 transform
   (rbg0_set_transform, platform side) anchors on P1, not the LAST split view rendered.  Sets only what
   the transform reads -- viewx/y/z/angle (+ sin/cos) -- WITHOUT R_SetupFrame's per-frame side effects
   (extralight, BSP validcount).  Off-path for DoomJo / 1-player. */
void sat_setup_view_p1 (void)
{
    extern player_t players[];
    player_t *p = &players[0];
    if (!p->mo) return;
    viewx = p->mo->x;
    viewy = p->mo->y;
    viewz = p->viewz;
    viewangle = p->mo->angle + viewangleoffset;
    viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];
}

/* SATURN (owner 2026-07-02): deported-plane ROTATION-DECROCHAGE fill.  A secondary floor/ceiling is
   deported to a VDP1 flat quad + an NBG1 index-0 silhouette mask.  VDP1 presents ~sat_plane_lag frames
   behind NBG1, so under yaw the quad sits a few px sideways of the (current) mask -> the leading mask
   edge exposes a strip with no quad and the sky/RBG0 shows through ("le ciel entre mur et plafond").
   Owner's fix: have the CPU draw the plane's OWN colour, in SOFTWARE (NBG1 = the mask's own latency =
   aligned by construction), in a border exactly as wide as that sideways offset -- i.e. the horizontal
   screen shift over the last sat_plane_lag frames.  0 at rest (no yaw -> no offset -> no border -> no
   cost); grows with turn speed.  sat_plane_lag is owner-tunable (the "nombre de frame de decalage").
   DoomJo never installs sat_floor_vdp1_hook, so R_DrawPlanes never reads sat_plane_border there. */
int sat_plane_border   = 0;    /* HORIZONTAL fill border px (from yaw)  -> L/R silhouette edge   */
int sat_plane_border_v = 0;    /* VERTICAL   fill border px (fwd+viewz) -> top/bottom edge       */
int sat_plane_lag      = 2;    /* N = frames VDP1 trails NBG1 (owner-tunable)                    */
int sat_plane_vscale   = 4;    /* vertical fill gain (owner-tuned to 4; pad R+Up/Down to adjust) */
int sat_plane_border_max = 40; /* SATURN: platform-clampable px cap on BOTH borders.  Default 40 =
                                  the legacy fast-spin guard -> byte-identical for DoomJo / any
                                  platform that never sets it.  With TEXTURED VDP1 planes a fast
                                  turn saturating the border at 40px paints the potato colour over
                                  every plane narrower than 80px (fully covering its texture); the
                                  platform caps this (pad R+Left/Right live) to trade a thin stale
                                  strip for keeping the texture visible. */
#define SAT_PLANE_LAG_MAX 8
static angle_t sat_va_hist[SAT_PLANE_LAG_MAX];   /* yaw   history -> horizontal shift            */
static fixed_t sat_vz_hist[SAT_PLANE_LAG_MAX];   /* viewz history -> vertical shift (stairs/bob) */
static fixed_t sat_vx_hist[SAT_PLANE_LAG_MAX];   /* viewx history -> forward-motion drift        */
static fixed_t sat_vy_hist[SAT_PLANE_LAG_MAX];   /* viewy history -> forward-motion drift        */
static int     sat_va_head = 0;

void R_SetupFrame (player_t* player)
{
    int		i;

    viewplayer = player;
    sat_view_sector = (void *)player->mo->subsector->sector;   /* governor parole, see the decl */
    viewx = player->mo->x;
    viewy = player->mo->y;
    viewangle = player->mo->angle + viewangleoffset;
    extralight = player->extralight;

    viewz = player->viewz;
    
    viewsin = finesine[viewangle>>ANGLETOFINESHIFT];
    viewcos = finecosine[viewangle>>ANGLETOFINESHIFT];

    sscount = 0;
	
    if (player->fixedcolormap)
    {
	fixedcolormap =
	    colormaps
	    + player->fixedcolormap*256*sizeof(lighttable_t);
	
	walllights = scalelightfixed;

	for (i=0 ; i<MAXLIGHTSCALE ; i++)
	    scalelightfixed[i] = fixedcolormap;
    }
    else
	fixedcolormap = 0;
		
    /* SATURN plane-decrochage: this frame's fill-border widths, each = the plane's screen shift over the
       last sat_plane_lag frames (= the VDP1-vs-mask offset to hide).  HORIZONTAL from yaw (linear-at-centre
       projection); VERTICAL from viewz (bob / stairs / lifts), px per world-unit tunable via sat_plane_vscale.
       1p only -- split shares one framebuffer and the per-view history would be mixed. */
    {
	extern int sat_split_active, viewwidth;
	int N = sat_plane_lag; if (N < 1) N = 1; else if (N > SAT_PLANE_LAG_MAX) N = SAT_PLANE_LAG_MAX;
	int slot = sat_va_head % SAT_PLANE_LAG_MAX;
	sat_va_hist[slot] = viewangle;
	sat_vz_hist[slot] = viewz;
	sat_vx_hist[slot] = viewx;
	sat_vy_hist[slot] = viewy;
	if (sat_split_active || sat_va_head < N)
	{
	    sat_plane_border = 0; sat_plane_border_v = 0;
	}
	else
	{
	    int old = (sat_va_head - N) % SAT_PLANE_LAG_MAX;
	    int cap = sat_plane_border_max; if (cap > 40) cap = 40; if (cap < 0) cap = 0;
	    int d = (int)(viewangle - sat_va_hist[old]); if (d < 0) d = -d;      /* signed wrap -> |dtheta| */
	    long long px = ((long long)d * (long long)viewwidth) / (long long)ANG90;
	    if (px > cap) px = cap;                                              /* fast-spin guard + platform cap */
	    sat_plane_border = (int)px;
	    /* VERTICAL: the gap while WALKING comes from forward-motion perspective drift (walls grow as you
	       approach -> the wall/ceiling & wall/floor junctions slide vertically), plus viewz for stairs/lifts/
	       bob.  fwd = forward component of the translation; viewz shifts the screen far more per world-unit
	       (it sits at the eye) -> weight it up (<<4).  One shared gain (sat_plane_vscale, owner-tuned to 4). */
	    fixed_t dz = viewz - sat_vz_hist[old]; if (dz < 0) dz = -dz;
	    fixed_t dvx = viewx - sat_vx_hist[old];
	    fixed_t dvy = viewy - sat_vy_hist[old];
	    fixed_t fwd = FixedMul(dvx, viewcos) + FixedMul(dvy, viewsin); if (fwd < 0) fwd = -fwd;
	    long long mv = (long long)fwd + ((long long)dz << 4);
	    int pv = (int)((mv * (long long)sat_plane_vscale) >> 20);
	    if (pv > cap) pv = cap;                                              /* same platform cap as H */
	    sat_plane_border_v = pv;
	}
	sat_va_head++;
    }

    framecount++;
    validcount++;
}



//
// R_RenderView
//
// SATURN: the frame is recorded as draw commands and executed on both
// SH-2 CPUs (see ../r_parallel.c). The hooks are no-ops if the slave
// CPU is unavailable or low-detail mode is active.
#include "r_parallel.h"

/* SATURN: viewangleoffset corruption canary.
   No longer halts — resets and prints so we can locate the writer without
   freezing the game.  If this fires, look for the OOB write adjacent to
   the r_plane.o BSS boundary (BSS map: viewangleoffset @ 0x060cadf8,
   cachedystep[0] @ 0x060cadfc). */
#include "i_system.h"
void V_Canary (const char* where)
{
    if (viewangleoffset != 0)
    {
        static int canary_count = 0;
        if (canary_count < 8)
        {
            printf("CANARY: vao=%08x @%s cnt=%d\n",
                   (unsigned int)viewangleoffset, where, ++canary_count);
        }
        viewangleoffset = 0;   /* reset so the view does not rotate away */
    }
}

/* SATURN: phase indicator (defined in dg_saturn.c). */
extern volatile int game_phase;

/* SATURN: kick the VDP1 world (walls) as soon as the BSP walk has accumulated them, BEFORE the
   CPU draws floors/sprites -- so VDP1 renders in PARALLEL with the CPU and is ready the SAME
   frame, instead of being kicked at end-of-frame and lagging a frame behind the software
   framebuffer (the seam that showed sky between CPU-drawn close walls and VDP1 far walls).
   NULL on DoomJo / when the VDP1 world renderer is off. */
void (*sat_walls_done_hook)(void) = 0;

/* [!] SATURN 2026-08-26 -- LATE KICK (owner: *"on ne pouvait pas faire remonter le travail pour que
   le slave puisse travailler ?"*).  This does NOT move work onto the slave.  It stops SERIALISING
   the master's VDP1 emission against a slave that is standing still.

   The measurement that produced it (1p console-shaped Ymir capture, MST51.5):
       row-1 `pr` 3.1 ms  = the whole VDP1 kick        row-4 `em` 3.0 = 97 % of it is the wall emit
       row-2 `P` 12.3     = planes, row-5 `Pm6.7` master / `Ps8.3` slave, CONCURRENT
       row-5 `b29%`       = the slave is idle 71 % of the frame
   Today the order is  BSP -> kick (3.1, slave IDLE) -> build worklist (Pv 0.7) -> dispatch slave.
   The slave therefore starts its plane share 3.8 ms after the BSP walk ends, and `Ps` is the LONGER
   side of the plane phase -- so the slave is the critical path and we hand it a late start.

   With sat_kick_late the order becomes  BSP -> Pv -> DISPATCH SLAVE -> kick (on the master, while
   the slave draws planes) -> master steals its share.  The slave starts 3.1 ms earlier; VDP1 starts
   0.7 ms later (the worklist build is all that now precedes it).  Nothing else moves.

   WHY IT IS SAFE, checked rather than assumed:
     - the kick path writes RAW VDP1 command lists and registers -- there is NOT ONE sl* call in
       vdp1_walls_flush / wall_emit* / the things, weapon and HUD emits / vdp1_wpn_kick.  That is
       what matters: the documented M7 freeze is SGL work-area pointer creep while the slave runs
       (see rp_sgl_workptr_reset), and a path that never touches SGL cannot cause it.
     - the master writes VDP1 VRAM, the slave writes the NBG1 framebuffer.  Disjoint memories.
     - it is the same overlap the shipped clear-on-slave and plane work-steal already rely on.
   ⚠ READ IT ON MST, NOT ON `P`.  With the kick moved inside R_DrawPlanes its time lands in row-2
   `P`, while row-1 `pr` still reports it from its own FRT pair -- so P and pr DOUBLE-COUNT while
   this is on.  In 1p MST is the right judge anyway and it sits 1.5 ms above the 3-field line
   (3 fields = 50.05 ms, the capture reads 51.5), which is exactly the size of this lever.

   VALIDATED AND BAKED 2026-08-26, same afternoon, on a scene that was NOT sitting on a field
   line -- which is what made the win legible.  Same spot, same geometry (d53, Bw4.4, Bp14.5,
   pr7.8, em7.0 identical on both sides), 1p:
       before   16.0 fps   MST 62   R42   Pm4.2 / Ps4.4   b10%
       after    19.9 fps   MST 50   R35   Pm0.4 / Ps8.2   b21%
   Read the plane pair: after the change the master takes 0.4 ms of the planes and the slave 8.2 --
   the master spends that window on the 7.8 ms kick instead, which is the mechanism in its pure
   form.  R falls 7 ms; MST falls 12, because 62 ms is 3.7 fields (about 75 % of frames spilling
   onto a fourth) and 50 ms is three.  The pad R+Z A/B and the row-7 `K` field went with the
   verdict -- a settled knob is not a knob ([[toggle-audit-cleanup]]). */
int sat_kick_pending = 0;   /* a deferred kick is owed this frame -- flushed by the fallback below */

/* [!] SATURN 2026-08-26 -- THE SAME LEVER IN SPLIT, and it is the OWNER'S correction that opened it.
   I had closed this on a z-order argument: in split the per-view sprites are queued during their
   own view and the walls flush once at the end, so bringing the wall emission forward looked like
   it would reorder walls against sprites.  He answered: *"le z-order serait bon par vue, c'est tout
   ce qui importe ici"*.  He is right, and it is not even a trade -- the four views own DISJOINT
   screen quadrants, so a wall of view 0 and a sprite of view 1 can never touch the same pixel and
   their relative position in the command list is unobservable.  Only WITHIN a view does order
   matter, and within a view nothing moves.
   This flag ships the CHEAP HALF of that opening: the kick stays ONE atomic call (so the command
   budget, the things reserve and the wtex priority still see all four views at once -- the machinery
   the 4p flicker session paid for on hardware) and only moves EARLIER, from d_main's post-loop site
   to the LAST view's plane dispatch.  Everything it reads is already final there: `wall_acc[]` is
   complete (view 3's R_StoreWallRange ran during its BSP walk), the thing queue is complete
   (R_EmitWorldThingsVDP1 runs at :1358, before R_DrawPlanes), and every global it reads --
   sat_split_view, detailshift, the view window, the split SQ -- holds the SAME value at view 3's
   dispatch as at d_main's kick, because the view loop leaves them there.  That last sentence is the
   whole safety argument and it was checked, not assumed.
   THE BUDGET, before the mechanism (4p Ymir, TNT).  Planes are Pm7,0 + Ps7,3 = 14,3 ms of WORK run
   concurrently on two CPUs; the kick is 10,5 ms and master-pinned.  Today the pair costs
   max(7,0;7,3) + 10,5 = 17,8 ms.
     LAST VIEW ONLY (this flag): that view holds a quarter of the plane work, 3,6 ms, split 1,8 master
     / 1,8 slave.  Handing the master's 1,8 to the slave and spending it on the kick turns
     1,8 + 10,5 = 12,3 into max(3,6 ; 10,5) = 10,5.  The saving is the MASTER'S SHARE OF ONE VIEW:
     ~1,8 ms, not the view's whole plane time.  (I wrote 3,6 first and it was wrong by 2x -- the
     slave's half was never on the critical path to begin with.)
     PER VIEW (deferred): total work 14,3 planes + 10,5 kick over two CPUs with the kick pinned
     balances at 12,4 ms -> ~5,4 ms, THREE TIMES the cheap half.
   So this flag is not the prize; it is the MECHANISM TEST -- does bringing the split kick forward
   corrupt anything, does VDP1 mind starting a frame earlier, does the B-bus tax eat it.

   [!] YMIR 4p TNT A/B, SAME SPOT, IDENTICAL RENDER FINGERPRINT (Bw12,8 Bp39,4 hd8,2 lp17,9 tl0,6
   d187 c1679).  THE MECHANISM FIRED EXACTLY AS DESIGNED -- AND THE WIN CAME FROM SOMEWHERE ELSE:

                    K0        K1
                    K0        K1 (four captures)
       SPL v3       24        31, 32, 31, 32     kick moved INTO the last view (+8 of its ~10 ms)
       SPL k        10         0,  0,  0,  0     ...and left its own bracket
       SPL =        70        78, 78, 78, 78
       Pm / Ps  7,0/7,7   4,7..5,1 / 9,6..10,0   master hands the planes to the slave, as in 1p
       b%           12        15, 15, 15, 16
       a (fps)     8,7       9,1 9,4 9,4 9,5  -> 9,35   (+7,5 %)
       MST         116       105 108 106 103  -> 105,5
       R            88        86  86  86  85  -> 85,75

   THE SPLIT OF THE 10,5 ms, AND BOTH HALVES COME FROM WINDOWED MEANS ONLY:
     `R` is DERIVED as MST - (tic + snd + blit + dg), and tic/snd/dg are flat across all five
     captures, so MST - R IS the blit term -- and the presentation fence lives inside it (stated at
     the `rs` note, dg_saturn.cxx).  It falls 28 -> 19,75.
       CPU overlap  = R          : -2,25 ms   <- the 1,8 ms this note predicted.  Correct.
       present fence = MST - R   : -8,25 ms   <- not predicted at all.
   Firing the kick ~10 ms earlier changes the PHASE at which the frame reaches a fence that blocks
   to a vblank edge, and phase at a fence turns into whole fields.  I costed this lever purely as
   CPU overlap and ignored the VDP1/present latency term, which this project's own record states in
   four words: VDP1 needs a HEAD START ([[m7-vdp1-latency-coherent-pair-hold]]).
   ⚠ RETRACTED ON THE WAY: I first read that fence term off row-8 `<n>ms`, 16 -> 3.  That field was
   a SINGLE frame's align-to-vblank wait, near-uniform over a field -- the next three K1 captures
   read 3, 18 and 4 at the same spot on the same build.  Row-13 `F` is the same story at n~9:
   40 % -> 30/50/40/20.  The conclusion survives ONLY because MST and R are 1 s means.  Both probes
   were windowed the same hour; neither number above is quotable from the pre-fix builds.
   I costed this lever purely as CPU overlap and ignored the VDP1/present latency term, which this
   project's own record states in four words: VDP1 needs a HEAD START ([[m7-vdp1-latency-coherent-
   pair-hold]]).  Sizing a lever by the one mechanism you happen to be looking at is how you get the
   right number for the wrong reason.
   ⚠ WHY THIS YMIR READING IS ADMISSIBLE, unusually: the half of the fence that collapsed is
   align-to-vblank, driven by the same field clock that produces the MST quantisation Ymir models
   correctly.  The half Ymir does NOT model -- the VDP1 plot-done gate -- read g0 on both sides, so
   it contributes nothing either way here.  On CONSOLE the gate is real and the frame is 250 ms
   (15 fields) instead of 105-116, so the phase win can be larger or smaller.  Still needs hardware.
   ⚠ AND IT WEAKENS THE PER-VIEW VARIANT, not strengthens it.  Per-view emission would still START
   VDP1 at the same instant (one kick, at the last view), so it cannot buy any MORE phase -- only
   the extra ~3,6 ms of CPU overlap, which this A/B just showed to be the secondary term.  The cheap
   half may already have taken most of what is there.
   ⚠ Ymir cannot price this (ms are not its business) and the console session is tomorrow -> it
   ships behind pad R+Z with row-7 `K`, exactly like the 1p version did for one afternoon. */
int sat_kick_split  = 0;    /* pad R+Z: in split, kick at the LAST view's plane dispatch */
int sat_split_lastview = 0; /* d_main: 1 while rendering the last view of the split */
int sat_split_kicked   = 0; /* d_main: the single split kick has already been armed this frame */

/* SATURN split-screen (Iter 2): set while rendering the per-player half-views.  The VDP1
   wall emit (r_segs.c) + the VDP1 kick are skipped (the VDP1/VDP2 hybrid is single-view),
   so each half renders in pure software into its framebuffer region. */
int sat_split_active = 0;

/* SATURN split-screen Step 3 (docs/MULTIPLAYER_PLAN.md): when set, the per-player half-views
   keep their walls on VDP1 (the platform offsets each quad by viewwindowx + clips to the view's
   x-range; the walls are accumulated across BOTH views and kicked ONCE per frame from d_main.c).
   0 = the software-only split baseline (the A/B reference).  Default 0 -> DoomJo/1p unaffected;
   the platform sets it (and a live pad chord toggles it for hardware A/B). */
int sat_split_vdp1 = 0;
/* SATURN: low-detail (detailshift=1) in the split -- the platform sets it per the Z cycle; the
   d_main.c split block applies it as detailshift around the two views.  Default 0 -> 1p/DoomJo
   never low-detail (no split), and detailshift=0 makes every dependent change a no-op. */
int sat_split_lowdetail = 0;

/* SATURN x-split (parallel-REC / multiplayer foundation, docs/MULTIPLAYER_PLAN.md).
   Render the frame in two screen-x halves so the second SH-2 can eventually render
   one of them.  STEP A (this build): a SERIAL correctness scaffold -- both halves are
   drawn on the master with the slave off (rp_disabled forced across the two passes), to
   validate that the spatial partition composes a pixel-correct full frame (clean seam at
   the midline, sprites straddling it drawn in both halves) BEFORE the parallel + per-CPU
   render-state work (Step B).  sat_xsplit default 0 => one full-width pass == vanilla, so
   DoomJo and the single-player shipping build are unaffected (pure C, runtime-gated). */
#define SAT_XSPLIT 0
int sat_xsplit = SAT_XSPLIT;
extern int sat_view_x0, sat_view_x1;   /* x-range clip window (r_bsp.c R_ClearClipSegs) */
extern int rp_disabled;                /* r_parallel.c serial latch */

/* SATURN x-split (Step B3c): the Mimas platform (src/xsplit_slave.cxx) wires these to
   dispatch/await the SLAVE SH-2 rendering its half via the dual-compiled slave_ renderer.
   NULL on DoomJo and until the platform sets them -> the driver falls back to the A1 serial
   2-pass.  dispatch(player, x0, x1): point the slave's clip range at [x0,x1) and slSlaveFunc. */
void (*sat_xsplit_dispatch)(player_t *player, int x0, int x1) = 0;
void (*sat_xsplit_wait)(void) = 0;
/* 0 = SEQUENTIAL (slave half, wait, then master half -> only one CPU in the renderer at a
   time -> NO concurrent allocator race; validates the slave renderer in isolation).
   1 = PARALLEL (master half runs while the slave renders -> the real speedup, but needs the
   B4 allocator pre-cache gate first, else concurrent R_GenerateComposite/Z_Malloc corrupt
   the zone heap -- even on Ymir, it's a logical race not just a coherency one). */
#define SAT_XSPLIT_PARALLEL 0

/* SATURN x-split (Step B): in the slave dual-compile (RP_SLAVE_BUILD) the slave draws its
   half DIRECTLY (its colfunc/spanfunc are the direct R_Draw* set by its own
   R_ExecuteSetViewSize), so it must NOT touch the master's command-renderer brackets --
   they manage the SYNC mailbox and dispatch the slave (it would dispatch itself).  No-op
   them for the slave; the master and DoomJo keep the real RP_* path unchanged (identical). */
#ifdef RP_SLAVE_BUILD
#define SAT_RP_BEGIN()    ((void)0)
#define SAT_RP_BSPDONE()  ((void)0)
#define SAT_RP_MARKP(s)   ((void)0)
#define SAT_RP_MASKED()   ((void)0)
#define SAT_RP_END()      ((void)0)
#else
#define SAT_RP_BEGIN()    RP_BeginFrame()
#define SAT_RP_BSPDONE()  RP_MarkBSPDone()
#define SAT_RP_MARKP(s)   RP_MarkP(s)
#define SAT_RP_MASKED()   RP_BeginMasked()
#define SAT_RP_END()      RP_EndFrame()
#endif

/* One render pass over the current sat_view_[x0,x1) screen-x range.  Identical to the old
   R_RenderPlayerView body minus R_SetupFrame.  last_pass gates the VDP1 walls kick so it
   fires EXACTLY once per frame (after the last pass's BSP -> all halves' walls accumulated).
   In the slave build sat_walls_done_hook is its own NULL pointer (never assigned), so the
   slave never kicks VDP1 -- correct (VDP1 walls are the master's / software in x-split). */
static void R_RenderViewPass (int last_pass)
{
    /* 🔴 SATURN 2026-08-25 -- `rs` MOVED HERE, and this is where the missing time actually is.
       The old bracket (still live in R_RenderPlayerView) wrapped RP_AuxWait + R_SetupFrame +
       R_PostFlatCacheFrame and read rs0.0 on every capture -- it proved a negative.  Meanwhile
       the console ledger says R - (Bw+Bp+P+M) - kick = -2.0 / 5.9 / 19.7 / 20.8 ms across
       1p/2p/3p/4p: ZERO in 1p and ~5-7 ms PER VIEW in split, which makes it a SPLIT LAW member
       rather than the slop of a derived number.  THIS block is the candidate: four Clear* calls
       and a NetUpdate that run once per VIEW and sit OUTSIDE every phase bracket.  R_ClearPlanes
       alone memsets the 256-byte hash head and the cachedheight table and folds the per-view
       peaks; R_ClearSprites, R_ClearDrawSegs and R_ClearClipSegs each walk their own arrays.
       Both bracket sites accumulate into the same sat_r_setup_frt, so `rs` now reads ALL the
       unbracketed per-view setup and costs no overlay column.  ⚠ If `rs` still reads ~0 in 4p
       on console, the term is further in -- inside R_RenderBSPNode or between MarkP and
       BeginMasked -- and the next FRT pair goes there, not here. */
    RP_RSetupBegin ();

    // Clear buffers.
    R_ClearClipSegs ();
    R_ClearDrawSegs ();
    R_ClearPlanes ();
    R_ClearSprites ();

    // check for new console commands.
    NetUpdate ();

    RP_RSetupEnd ();

    SAT_RP_BEGIN ();

    /* 🔴 SATURN 2026-08-26 -- WALL FILL ON THE SLAVE: open the producer here.
       `Bp` is the window: 48.6 ms of a 4p frame in which the master does pure geometry and the
       slave does nothing at all.  The later window, R_DrawPlanes, is NOT free -- the plane
       work-steal already splits it almost evenly (row 5 read Pm8.5 / Ps7.9 on hardware), so
       handing the walls over there would only swap one job for another.  The slave DRAINS spans
       while the master keeps recording them, and the join below gives it back in time to take its
       half of the planes.
       ⚠ THIS ONLY ARMS -- it does not dispatch.  The slave is woken by the FIRST recorded span,
       inside the walk (see sat_wallfill_take), so a view whose threshold rejects every column
       never wakes it: the 1p A/B measured ~0.4 ms of master time lost to a slave spinning on an
       empty queue.  No-op unless sat_wallfill_min > 0 (pad R+X). */
    { extern void R_WallFillArm (void); R_WallFillArm (); }

    // The head node is the last node output.
    R_RenderBSPNode (numnodes-1);

    /* SATURN parallel-REC: run the deferred wall-prep (R_StoreWallRange queued during the
       BSP walk).  No-op when sat_wallprep_defer is 0 (the walls ran inline already).
       RANK 3 inc-1 (docs/RANK3_WALLPREP.md): when sat_wallprep_slave is on, the whole flush
       runs on the SLAVE (non-overlapped: dispatch here, walk is done, master waits). */
    { extern void RP_FlushWalls(void);
      extern int sat_wallprep_slave, walljob_n;
      extern void RP_DispatchWallPrep(int n); extern void RP_WaitWallPrep(void);
      if (sat_wallprep_slave) { RP_DispatchWallPrep(walljob_n); RP_WaitWallPrep(); walljob_n = 0; }
      else                      RP_FlushWalls();
    }

    /* Close the span queue and join the slave.  MUST be here: R_DrawPlanes below wants the slave
       for the plane split, and the masked pass after it must paint sprites ON TOP of these wall
       pixels. */
    { extern void R_WallFillDone (void); R_WallFillDone (); }

    SAT_RP_BSPDONE ();   // SATURN: profiler BSP/planes boundary (row-20 B/P/M)

    /* SATURN: walls are accumulated -> kick VDP1 NOW so it draws in parallel with the CPU
       floors/sprites below and presents the SAME frame (no 1-frame lag vs the framebuffer).
       In x-split this fires only on the final pass so VDP1 is kicked once with every wall.
       In split-screen (sat_split_active) the VDP1 hybrid is off (it is single-view) -> no kick. */
    if (last_pass && sat_walls_done_hook
        && (!sat_split_active || (sat_kick_split && sat_split_lastview)))
    {
        /* Hand it to RP_DrawPlanesSplit, which runs it the instant the slave has been launched
           onto the planes.  See the note at sat_kick_pending. */
        sat_kick_pending = 1;
        /* Armed => fired before this view returns: RP_DrawPlanesSplit consumes it, and the
           fallback below consumes it on every path that does not reach RP_DrawPlanesSplit.  So
           d_main can read this as "the kick is handled" without a second flag. */
        if (sat_split_active) sat_split_kicked = 1;
    }

    /* SATURN split world-things-on-VDP1: in split the kick above is skipped (d_main.c kicks ONCE
       after ALL views), but vissprites/drawsegs/view window are per-view state that is gone by
       then -- so emit THIS view's world sprites NOW.  The platform hook queues them (and bakes
       their textures into the tear-safe next-parity slots); the d_main kick flushes the queue
       AFTER the walls, keeping the 1p painter order (walls -> things -> weapon).  Gated like the
       split wall emit (sat_split_vdp1); R_EmitWorldThingsVDP1 re-checks hook/skip/hw itself. */
    if (last_pass && sat_split_active && sat_split_vdp1)
    {
	extern void R_EmitWorldThingsVDP1 (void);
	R_EmitWorldThingsVDP1 ();
    }

    /* SATURN: `P` sub-bracket 0 -- the VDP1 wall kick is DONE.  Everything above this line is
       charged to `P` today but is not a plane: the wall-list flush, the VDP1 kick, and
       R_DrawPlayerSprites (weapon projection + VDP1 texture bake) inside the hook. */
    SAT_RP_MARKP (0);

    V_Canary ("bsp");

    // Check for new console commands.
    NetUpdate ();

    /* SATURN LEAD-FILL on the SLAVE (sat_lead_mode 1).  The difference spans were RECORDED during
       the BSP walk instead of drawn; hand them to the 2nd SH-2 here and let it fill them while the
       master draws the planes.  This is the ONE window where the slave is provably usable in M7:
       planes are master-only there (see the r_parallel gate) and the aux dispatch is the same path
       the shipped, HW-validated framebuffer clear already takes every frame.
       Pixel-safe against the planes it overlaps -- Doom clips every visplane to the ceilingclip /
       floorclip the wall loop just wrote, so no plane owns a row a wall tier owns -- and register-
       safe, because R_LeadSlaveDraw touches no dc_* global (the master's sky columns use them). */
    {
	extern int  sat_lead_mode, sat_local_players;
	extern int  R_LeadSpanCount (void);
	extern void R_LeadSpanReset (void), R_LeadSlaveDraw (void);
	extern void RP_AuxDispatch (void (*fn)(void)), RP_LeadJoin (void);
	int lead_slave = (sat_lead_mode == 1 && !sat_split_active && sat_local_players <= 1
			  && R_LeadSpanCount () > 0);
	if (lead_slave) RP_AuxDispatch (R_LeadSlaveDraw);

	R_DrawPlanes ();

	/* LATE-KICK FALLBACK -- this is what makes the deferral provably once-per-frame.  R_DrawPlanes
	   does not always reach RP_DrawPlanesSplit (worklist of 0 or 1, sat_plane_parallel off, or the
	   split self-healed to master-only), and a kick that never fires is a frame with no walls. */
	if (sat_kick_pending) { sat_kick_pending = 0; if (sat_walls_done_hook) sat_walls_done_hook (); }

	/* Join BEFORE the masked pass: sprites must land on top of the wall pixels. */
	if (lead_slave) { RP_LeadJoin (); R_LeadSpanReset (); }
	else if (R_LeadSpanCount () > 0)   /* mode changed mid-frame, or the dispatch was skipped */
	    { R_LeadSlaveDraw (); R_LeadSpanReset (); }
    }

    V_Canary ("planes");

    // Check for new console commands.
    NetUpdate ();

    SAT_RP_MASKED ();

    game_phase = 5; /* R_DrawMasked */
    R_DrawMasked ();

    V_Canary ("masked");

    game_phase = 4; /* back to render (RP_EndFrame) */
    SAT_RP_END ();

    V_Canary ("endframe");

    // Check for new console commands.
    NetUpdate ();
}

void R_RenderPlayerView (player_t* player)
{
    V_Canary ("frame start");

    game_phase = 4; /* R_RenderPlayerView (BSP + execute) */

    /* SATURN: if the platform dispatched the end-of-frame framebuffer clear to the SLAVE SH-2
       (sat_clear_slave, docs/BLIT_DMA_PLAN.md Inc3), that clear has been running concurrently with
       the game tic.  JOIN it HERE -- before the BSP walk writes the first pixel -- so a still-running
       slave clear can never race the master's render.  No-op (returns instantly) when no aux job is
       pending (clear-on-master, DoomJo, or a prior split view already joined it). */
    /* SATURN 2026-08-18 (row 1 `rs`): everything R_RenderPlayerView does BEFORE the first phase
       mark.  Row-1 `R` is derived (MST - T - S - b - dg) while Bw/Bp/P/M are measured, and the two
       differ by ~11,6 ms -- this says whether that gap is a real phase or the slop of a derived
       number.  It is the last unnamed term in the frame. */
    RP_RSetupBegin ();
    { extern void RP_AuxWait(void); RP_AuxWait(); }

    R_SetupFrame (player);

    /* (R_PostTexCacheFrame DELETED with core/r_cache.c on 2026-08-17 -- it aged an LRU pool that
       never allocated a block.  The flat pool below keeps its own, real, aging beat.) */
    /* SATURN: age the resident flat pool on the same beat.  MUST be here -- before the BSP
       walk re-touches this view's flats -- so "age 0" means exactly "in use by the view being
       drawn" and the LRU can never reuse a slot a queued visplane still points into. */
    R_PostFlatCacheFrame ();
    RP_RSetupEnd ();

    if (sat_xsplit)
    {
        /* x-split: render the frame in two screen-x halves.  If the platform wired the slave
           dispatch (B3c), the 2nd SH-2 renders the RIGHT half via the dual-compiled slave_
           renderer CONCURRENTLY with the master's LEFT half (parallel-REC); else both halves
           run on the master serially (A1 scaffold).  Either way the master draws DIRECTLY
           (rp_disabled) and one-sided walls go to software (sat_wall_skip=0 -- the VDP1
           per-view wall integration is later). */
        extern int sat_wall_skip;
        int saved_rp   = rp_disabled;
        int saved_skip = sat_wall_skip;
        int half = viewwidth / 2;
        rp_disabled   = 1;
        sat_wall_skip = 0;

        if (sat_xsplit_dispatch)
        {
            /* slave renders the RIGHT half [half,viewwidth) on the 2nd SH-2. */
            sat_xsplit_dispatch (player, half, viewwidth);   /* slave clip range + slSlaveFunc */
            sat_view_x0 = 0; sat_view_x1 = half;
#if SAT_XSPLIT_PARALLEL
            R_RenderViewPass (1);     /* master's half CONCURRENTLY (needs the B4 gate) */
            sat_xsplit_wait ();
#else
            sat_xsplit_wait ();       /* SEQUENTIAL: slave alone first -> no allocator race */
            R_RenderViewPass (1);     /* then the master's half, alone */
#endif
        }
        else
        {
            /* A1 serial fallback: both halves on the master.  validcount++ before pass 2 so
               R_AddSprites re-adds sectors seen in pass 1 (straddling sprites kept). */
            sat_view_x0 = 0;    sat_view_x1 = half;      R_RenderViewPass (0);
            validcount++;
            sat_view_x0 = half; sat_view_x1 = viewwidth; R_RenderViewPass (1);
        }

        sat_view_x0 = 0; sat_view_x1 = 0;
        sat_wall_skip = saved_skip;
        rp_disabled   = saved_rp;
    }
    else
    {
        /* SATURN P3 (parallel-REC plane split, r_plane.c + r_parallel.c RP_DispatchPlanes):
           force the command-renderer parity OFF so the slave SH-2 is free for the visplane
           work-steal dispatch (no 2nd-dispatch conflict).  Walls stay on VDP1 (sat_wall_skip
           untouched), sprites/masked draw direct (master) -- cheap in the 1p ship config; the
           win is the master-only P phase split across both CPUs.  One full-width pass. */
        extern int sat_plane_parallel;
        /* Force the command-renderer parity OFF (rp_disabled=1) so the slave SH-2 is free for the
           plane/masked-split dispatch -- the productive M7 slave path, hard-wired ON since
           2026-07-30 (the sat_m7_slave 0-3 A/B level was removed once level 3 shipped).
           (The level-4 unified-REC path was REMOVED 2026-07-29: HW proved M7 is command-GENERATION-
           bound -- record ~26ms vs execute ~0.4ms -- so REC, which only parallelises the draw/execute
           phase, left the slave near-idle and was SLOWER than plane-split.  See m7-slave-share notes.) */
        if (sat_plane_parallel)
            rp_disabled = 1;
        R_RenderViewPass (1);            /* single full-width pass (sat_view_* default to full) */
    }
}

/* NOTE: the RP_SLAVE_BUILD dual-compile entries (R_SlaveRenderHalf / R_InitForSplit) were
   removed -- the full per-CPU-duplicated slave renderer overflows the Saturn's 2MB.  The
   sat_xsplit driver above keeps the x-range clip + serial scaffold + the sat_xsplit_dispatch/
   wait hooks (NULL -> serial fallback) for reuse by the d32xr-style phase-split (the viable
   parallel-REC path).  See docs/PARALLEL_REC_AUDIT.md.  The SAT_RP_* macros stay (= RP_* in a
   normal build). */
