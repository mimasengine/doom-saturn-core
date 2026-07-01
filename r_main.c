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
#include "r_cache.h"





// Fineangles in the SCREENWIDTH wide window.
#define FIELDOFVIEW		2048	



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
	
    if (!node->dx)
    {
	if (x <= node->x)
	    return node->dy > 0;
	
	return node->dy < 0;
    }
    if (!node->dy)
    {
	if (y <= node->y)
	    return node->dx < 0;
	
	return node->dx > 0;
    }
	
    dx = (x - node->x);
    dy = (y - node->y);
	
    // Try to quickly decide by looking at sign bits.
    if ( (node->dy ^ node->dx ^ dx ^ dy)&0x80000000 )
    {
	if  ( (node->dy ^ dx) & 0x80000000 )
	{
	    // (left is negative)
	    return 1;
	}
	return 0;
    }

    left = FixedMul ( node->dy>>FRACBITS , dx );
    right = FixedMul ( dy , node->dx>>FRACBITS );
	
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
	
    lx = line->v1->x;
    ly = line->v1->y;
	
    ldx = line->v2->x - lx;
    ldy = line->v2->y - ly;
	
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
			    finetangent[FINEANGLES/4+FIELDOFVIEW/2] );
	
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
    
    detailshift = setdetail;
    viewwidth = scaledviewwidth>>detailshift;
	
    centery = viewheight/2;
    centerx = viewwidth/2;
    centerxfrac = centerx<<FRACBITS;
    centeryfrac = centery<<FRACBITS;
    projection = centerxfrac;

    if (!detailshift)
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
	yslope[i] = FixedDiv ( (viewwidth<<detailshift)/2*FRACUNIT, dy);
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
	projection = centerxfrac;

	R_InitTextureMapping ();                    /* xtoviewangle from centerx/viewwidth */

	pspritescale  = FRACUNIT*viewwidth/SCREENWIDTH;
	pspriteiscale = FRACUNIT*SCREENWIDTH/viewwidth;

	for (i=0 ; i<viewwidth ; i++)
	    screenheightarray[i] = viewheight;
	for (i=0 ; i<viewheight ; i++)
	{
	    dy = ((i-viewheight/2)<<FRACBITS)+FRACUNIT/2;  dy = abs(dy);
	    yslope[i] = FixedDiv ( (viewwidth<<detailshift)/2*FRACUNIT, dy);
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

	/* SATURN: select the draw funcs for the detail level (mirrors R_ExecuteSetViewSize) so
	   the split low-detail path gets R_Draw*Low; self-corrects back to high when detailshift
	   returns to 0 (the cache keys on detailshift).  detailshift=0 == the existing defaults. */
	if (!detailshift)
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
    }

    /* framebuffer pointers at the explicit origin -- ALWAYS (cheap, per-view).  columnofs is
       indexed by SCREEN x (full width w = viewwidth<<detailshift); R_DrawColumnLow writes
       columnofs[dc_x<<1] and [dc_x<<1 +1].  At detailshift=0, w==viewwidth -> identical loop. */
    viewwindowx = wx;
    viewwindowy = wy;
    for (i=0 ; i<w ; i++)
	columnofs[i] = viewwindowx + i;
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

void R_SetupFrame (player_t* player)
{
    int		i;
    
    viewplayer = player;
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
#define SAT_RP_MASKED()   ((void)0)
#define SAT_RP_END()      ((void)0)
#else
#define SAT_RP_BEGIN()    RP_BeginFrame()
#define SAT_RP_BSPDONE()  RP_MarkBSPDone()
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
    // Clear buffers.
    R_ClearClipSegs ();
    R_ClearDrawSegs ();
    R_ClearPlanes ();
    R_ClearSprites ();

    // check for new console commands.
    NetUpdate ();

    SAT_RP_BEGIN ();

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

    SAT_RP_BSPDONE ();   // SATURN: profiler BSP/planes boundary (row-20 B/P/M)

    /* SATURN: walls are accumulated -> kick VDP1 NOW so it draws in parallel with the CPU
       floors/sprites below and presents the SAME frame (no 1-frame lag vs the framebuffer).
       In x-split this fires only on the final pass so VDP1 is kicked once with every wall.
       In split-screen (sat_split_active) the VDP1 hybrid is off (it is single-view) -> no kick. */
    if (last_pass && sat_walls_done_hook && !sat_split_active) sat_walls_done_hook ();

    V_Canary ("bsp");

    // Check for new console commands.
    NetUpdate ();

    R_DrawPlanes ();

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

    R_SetupFrame (player);

    // SATURN: age the bounded streaming texture cache once per view, before the
    // BSP walk re-touches the visible composites (no-op unless sat_streaming).
    R_PostTexCacheFrame ();

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
        if (sat_plane_parallel) rp_disabled = 1;
        R_RenderViewPass (1);            /* single full-width pass (sat_view_* default to full) */
    }
}

/* NOTE: the RP_SLAVE_BUILD dual-compile entries (R_SlaveRenderHalf / R_InitForSplit) were
   removed -- the full per-CPU-duplicated slave renderer overflows the Saturn's 2MB.  The
   sat_xsplit driver above keeps the x-range clip + serial scaffold + the sat_xsplit_dispatch/
   wait hooks (NULL -> serial fallback) for reuse by the d32xr-style phase-split (the viable
   parallel-REC path).  See docs/PARALLEL_REC_AUDIT.md.  The SAT_RP_* macros stay (= RP_* in a
   normal build). */
