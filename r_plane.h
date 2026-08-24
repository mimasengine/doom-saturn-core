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
//	Refresh, visplane stuff (floor, ceilings).
//


#ifndef __R_PLANE__
#define __R_PLANE__


#include "r_data.h"



// Visplane related.
extern  short*		lastopening;
// SATURN garde-OPENINGS: overflow guard for the shared openings pool (see r_plane.c / r_segs.c).
extern  short* const	openings_end;       // == openings + MAXOPENINGS
extern  short		opening_overflow[];  // graceful sink (SCREENWIDTH) for overflowing writes
extern  int		r_opening_ovf;       // redirects this frame (0 = no overflow)


typedef void (*planefunction_t) (int top, int bottom);

extern planefunction_t	floorfunc;
extern planefunction_t	ceilingfunc_t;

extern short		floorclip[SCREENWIDTH];
extern short		ceilingclip[SCREENWIDTH];

extern fixed_t		yslope[SCREENHEIGHT];
extern fixed_t		distscale[SCREENWIDTH];

void R_InitPlanes (void);
void R_ClearPlanes (void);

void
R_MapPlane
( int		y,
  int		x1,
  int		x2 );

void
R_MakeSpans
( int		x,
  int		t1,
  int		b1,
  int		t2,
  int		b2 );

void R_DrawPlanes (void);

/* SATURN 2026-08-24 -- PLANE IDENTITY (option A'), see the long note in r_plane.c.
   vp_sector[i] / vp_flags[i] run parallel to visplanes[] (visplane_t itself is unchanged,
   28 B, so the hash walk stays cache-tight).  flags == 0 means: plane i IS the whole of
   exactly one sector's surface -- the only form in which the question can be asked, since a
   visplane keys on (height, picnum, lightlevel) and forks per seg.  Combined with
   sat_sector_bbox (p_local.h) it gives that surface's EXACT world AABB. */
#define VPF_SPLIT  1   /* R_CheckPlane forked: this plane is a PIECE of its sector's surface */
#define VPF_MULTI  2   /* R_FindPlane merged another sector in: not one surface at all       */
extern short *vp_bbox;    /* 4 shorts per plane: world AABB, union of the sectors in it */
extern byte  *vp_flags;

visplane_t*
R_FindPlane
( fixed_t	height,
  int		picnum,
  int		lightlevel,
  int		secnum );

visplane_t*
R_CheckPlane
( visplane_t*	pl,
  int		start,
  int		stop );



#endif
