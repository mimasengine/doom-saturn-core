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
//	Refresh of things, i.e. objects represented by sprites.
//




/* SATURN: O3 — sprite project/scale uses FixedMul/FixedDiv on every visible sprite */
#pragma GCC optimize("O3")

#include <stdio.h>
#include <stdlib.h>


#include "deh_main.h"
#include "doomdef.h"

#include "i_swap.h"
#include "i_system.h"
#include "z_zone.h"
#include "w_wad.h"

#include "r_local.h"

#include "doomstat.h"



#define MINZ				(FRACUNIT*4)
#define BASEYCENTER			100

//void R_DrawColumn (void);
//void R_DrawFuzzColumn (void);



typedef struct
{
    int		x1;
    int		x2;
	
    int		column;
    int		topclip;
    int		bottomclip;

} maskdraw_t;



//
// Sprite rotation 0 is facing the viewer,
//  rotation 1 is one angle turn CLOCKWISE around the axis.
// This is not the same as the angle,
//  which increases counter clockwise (protractor).
// There was a lot of stuff grabbed wrong, so I changed it...
//
fixed_t		pspritescale;
fixed_t		pspriteiscale;

/* SATURN 2-player split: shift the player weapon DOWN by this many screen pixels
   (the half-size split gun is anchored to the 200-line BASEYCENTER and otherwise
   floats high above the shortened view).  0 = no shift (1p / DoomJo unchanged);
   the split path sets it so the gun bottom sits on the view bottom (row 160). */
int		sat_psprite_yoff = 0;

/* SATURN: platform hooks to draw the player sprites (weapon / muzzle flash) on
   VDP1 instead of the software framebuffer.  NULL on DoomJo and when unused ->
   the normal R_DrawVisSprite software path runs.  sat_psprite_begin is called
   once at the top of R_DrawPlayerSprites (reset the VDP1 command list); the
   hook is called per psprite with the cached patch, screen top-left, flip and
   light colormap. */
void (*sat_psprite_begin)(void) = 0;
void (*sat_psprite_hook)(patch_t *patch, int lump, int sx, int sy, int flip,
                         const unsigned char *cmap) = 0;
/* SATURN: 1 = the platform calls R_DrawPlayerSprites EARLY (before the VDP1 end-of-planes
   present) so the weapon lands on the VDP1 sprite layer this frame; R_DrawMasked then skips
   the late software psprite draw.  0 on DoomJo / when the weapon stays software. */
int sat_psprite_early = 0;

/* SATURN world-things-on-VDP1 (de-risk probe, platform gate SAT_WORLD_THINGS_VDP1): draw the
   world sprites as prio-7 VDP1 quads (like the weapon) to offload the memory-bound masked FILL
   off the two SH-2s.  The platform hook bakes the sprite patch (full-patch, cache key lump+cmap)
   and emits a distorted quad into the screen rect computed here; it returns 1 if it took the
   sprite, 0 if it declined (texture too big / command budget full) -> that sprite falls back to
   the software masked draw.  Emitted at the post-BSP kick (vissprites + drawsegs ready, before
   the end-of-planes VDP1 present), so it lands THIS frame -- same window the weapon uses.
   NON-occlusion-clipped for now (viewport/system-clip only): a nearer wall does not yet hide a
   farther thing (that is the FUNC_UserClip follow-up).  NULL on DoomJo -> pure software. */
int (*sat_thing_hook)(patch_t *patch, int lump, const unsigned char *cmap,
                      int x0, int y0, int x1, int y1,          /* sprite screen quad (full-res) */
                      int cx0, int cy0, int cx1, int cy1,      /* visible clip rect (occlusion)  */
                      int flip) = 0;
int sat_things_emitted = 0;                 /* 1 = things went to VDP1 this view -> R_DrawMasked skips them */
int sat_things_occ = 0;                     /* fully-occluded sprites skipped this frame (occlusion metric) */
int sat_thing_cap = 4;                      /* platform sets = VDP1 thing slots/frame (VRAM cap); nearest win */
int sat_things_hw = 1;                      /* platform (sat_apply_mode): 1 = world sprites on VDP1; 0 = software (M0/M6) */
static char sat_thing_vdp1[MAXVISSPRITES];  /* per-vissprite (by array index): 1 = emitted on VDP1 */
static char sat_thing_elig[MAXVISSPRITES];  /* 1 = selected (nearest sat_thing_cap) for VDP1 this frame */

lighttable_t**	spritelights;

// constant arrays
//  used for psprite clipping and initializing clipping
short		negonearray[SCREENWIDTH];
short		screenheightarray[SCREENWIDTH];


//
// INITIALIZATION FUNCTIONS
//

// variables used to look up
//  and range check thing_t sprites patches
spritedef_t*	sprites;
int		numsprites;

spriteframe_t	sprtemp[29];
int		maxframe;
char*		spritename;




//
// R_InstallSpriteLump
// Local function for R_InitSprites.
//
void
R_InstallSpriteLump
( int		lump,
  unsigned	frame,
  unsigned	rotation,
  boolean	flipped )
{
    int		r;
	
    if (frame >= 29 || rotation > 8)
	I_Error("R_InstallSpriteLump: "
		"Bad frame characters in lump %i", lump);
	
    if ((int)frame > maxframe)
	maxframe = frame;
		
    if (rotation == 0)
    {
	// the lump should be used for all rotations
	if (sprtemp[frame].rotate == false)
	    I_Error ("R_InitSprites: Sprite %s frame %c has "
		     "multip rot=0 lump", spritename, 'A'+frame);

	if (sprtemp[frame].rotate == true)
	    I_Error ("R_InitSprites: Sprite %s frame %c has rotations "
		     "and a rot=0 lump", spritename, 'A'+frame);
			
	sprtemp[frame].rotate = false;
	for (r=0 ; r<8 ; r++)
	{
	    sprtemp[frame].lump[r] = lump - firstspritelump;
	    sprtemp[frame].flip[r] = (byte)flipped;
	}
	return;
    }
	
    // the lump is only used for one rotation
    if (sprtemp[frame].rotate == false)
	I_Error ("R_InitSprites: Sprite %s frame %c has rotations "
		 "and a rot=0 lump", spritename, 'A'+frame);
		
    sprtemp[frame].rotate = true;

    // make 0 based
    rotation--;		
    if (sprtemp[frame].lump[rotation] != -1)
	I_Error ("R_InitSprites: Sprite %s : %c : %c "
		 "has two lumps mapped to it",
		 spritename, 'A'+frame, '1'+rotation);
		
    sprtemp[frame].lump[rotation] = lump - firstspritelump;
    sprtemp[frame].flip[rotation] = (byte)flipped;
}




//
// R_InitSpriteDefs
// Pass a null terminated list of sprite names
//  (4 chars exactly) to be used.
// Builds the sprite rotation matrixes to account
//  for horizontally flipped sprites.
// Will report an error if the lumps are inconsistant. 
// Only called at startup.
//
// Sprite lump names are 4 characters for the actor,
//  a letter for the frame, and a number for the rotation.
// A sprite that is flippable will have an additional
//  letter/number appended.
// The rotation character can be 0 to signify no rotations.
//
void R_InitSpriteDefs (char** namelist) 
{ 
    char**	check;
    int		i;
    int		l;
    int		frame;
    int		rotation;
    int		start;
    int		end;
    int		patched;
		
    // count the number of sprite names
    check = namelist;
    while (*check != NULL)
	check++;

    numsprites = check-namelist;
	
    if (!numsprites)
	return;
		
    sprites = Z_Malloc(numsprites *sizeof(*sprites), PU_STATIC, NULL);
	
    start = firstspritelump-1;
    end = lastspritelump+1;
	
    // scan all the lump names for each of the names,
    //  noting the highest frame letter.
    // Just compare 4 characters as ints
    for (i=0 ; i<numsprites ; i++)
    {
	spritename = DEH_String(namelist[i]);
	memset (sprtemp,-1, sizeof(sprtemp));
		
	maxframe = -1;
	
	// scan the lumps,
	//  filling in the frames for whatever is found
	for (l=start+1 ; l<end ; l++)
	{
	    if (!strncasecmp(lumpinfo[l].name, spritename, 4))
	    {
		frame = lumpinfo[l].name[4] - 'A';
		rotation = lumpinfo[l].name[5] - '0';

		if (modifiedgame)
		    patched = W_GetNumForName (lumpinfo[l].name);
		else
		    patched = l;

		R_InstallSpriteLump (patched, frame, rotation, false);

		if (lumpinfo[l].name[6])
		{
		    frame = lumpinfo[l].name[6] - 'A';
		    rotation = lumpinfo[l].name[7] - '0';
		    R_InstallSpriteLump (l, frame, rotation, true);
		}
	    }
	}
	
	// check the frames that were found for completeness
	if (maxframe == -1)
	{
	    sprites[i].numframes = 0;
	    continue;
	}
		
	maxframe++;
	
	for (frame = 0 ; frame < maxframe ; frame++)
	{
	    switch ((int)sprtemp[frame].rotate)
	    {
	      case -1:
		// no rotations were found for that frame at all
		I_Error ("R_InitSprites: No patches found "
			 "for %s frame %c", spritename, frame+'A');
		break;
		
	      case 0:
		// only the first rotation is needed
		break;
			
	      case 1:
		// must have all 8 frames
		for (rotation=0 ; rotation<8 ; rotation++)
		    if (sprtemp[frame].lump[rotation] == -1)
			I_Error ("R_InitSprites: Sprite %s frame %c "
				 "is missing rotations",
				 spritename, frame+'A');
		break;
	    }
	}
	
	// allocate space for the frames present and copy sprtemp to it
	sprites[i].numframes = maxframe;
	sprites[i].spriteframes = 
	    Z_Malloc (maxframe * sizeof(spriteframe_t), PU_STATIC, NULL);
	memcpy (sprites[i].spriteframes, sprtemp, maxframe*sizeof(spriteframe_t));
    }

}




//
// GAME FUNCTIONS
//
vissprite_t	vissprites[MAXVISSPRITES];
vissprite_t*	vissprite_p;
int		newvissprite;



//
// R_InitSprites
// Called at program start.
//
void R_InitSprites (char** namelist)
{
    int		i;
	
    for (i=0 ; i<SCREENWIDTH ; i++)
    {
	negonearray[i] = -1;
    }
	
    R_InitSpriteDefs (namelist);
}



//
// R_ClearSprites
// Called at frame start.
//
void R_ClearSprites (void)
{
    extern void RP_SprReset(void);   /* SATURN sprite-cost profiler: zero the per-frame timers */
    RP_SprReset();
    sat_things_emitted = 0;          /* SATURN: reset per view -> R_DrawMasked only skips when the kick emitted this view */
    vissprite_p = vissprites;
}


//
// R_NewVisSprite
//
vissprite_t	overflowsprite;

vissprite_t* R_NewVisSprite (void)
{
    if (vissprite_p == &vissprites[MAXVISSPRITES])
	return &overflowsprite;
    
    vissprite_p++;
    return vissprite_p-1;
}



//
// R_DrawMaskedColumn
// Used for sprites and masked mid textures.
// Masked means: partly transparent, i.e. stored
//  in posts/runs of opaque pixels.
//
short*		mfloorclip;
short*		mceilingclip;

fixed_t		spryscale;
fixed_t		sprtopscreen;

void R_DrawMaskedColumn (column_t* column)
{
    int		topscreen;
    int 	bottomscreen;
    fixed_t	basetexturemid;
	
    basetexturemid = dc_texturemid;
	
    for ( ; column->topdelta != 0xff ; ) 
    {
	// calculate unclipped screen coordinates
	//  for post
	topscreen = sprtopscreen + spryscale*column->topdelta;
	bottomscreen = topscreen + spryscale*column->length;

	dc_yl = (topscreen+FRACUNIT-1)>>FRACBITS;
	dc_yh = (bottomscreen-1)>>FRACBITS;
		
	if (dc_yh >= mfloorclip[dc_x])
	    dc_yh = mfloorclip[dc_x]-1;
	if (dc_yl <= mceilingclip[dc_x])
	    dc_yl = mceilingclip[dc_x]+1;

	// SATURN: clamp to the (split-)viewport.  In 2p split a stale/garbage clip
	// value (mfloorclip > viewheight) can drive dc_yh up to 255, which trips the
	// master R_DrawColumn RANGECHECK -> I_Error (and the unguarded slave drawer
	// writes OOB).  Skip-not-crash; byte-identical when the clips are valid
	// (dc_yh < viewheight already, so the clamp never fires).
	if (dc_yh >= viewheight) dc_yh = viewheight - 1;
	if (dc_yl < 0)           dc_yl = 0;

	if (dc_yl <= dc_yh)
	{
	    dc_source = (byte *)column + 3;
	    dc_texturemid = basetexturemid - (column->topdelta<<FRACBITS);
	    // dc_source = (byte *)column + 3 - column->topdelta;

	    // Drawn by either R_DrawColumn
	    //  or (SHADOW) R_DrawFuzzColumn.
	    colfunc ();	
	}
	column = (column_t *)(  (byte *)column + column->length + 4);
    }
	
    dc_texturemid = basetexturemid;
}

/* SATURN sprite-SQ (independent 4th axis, platform sat_sprite_ld / pad R+B): draw ONE masked sprite
   column with the horizontal WRITE decoupled from the CLIP.  `clip_x` is the projection column whose
   silhouette clips this run (mfloorclip/mceilingclip[clip_x]); `write_x` is the SCREEN column written;
   `wide` also writes write_x+1 (the same texels).  Geometry and OCCLUSION stay in the projection's
   column space -- R_DrawSprite / the drawseg clip / mfloorclip are UNCHANGED -- so the sprite's
   horizontal resolution is decoupled from the wall/floor detailshift.  Two callers use it:
     - walls FULL + sprite LD (detailshift==0): step the projection column by 2, wide=1 -> half-res
       fill (one vertical pass per 2 screen columns), clip shared (<=1px silhouette approx, invisible).
     - walls LD + sprite FULL (detailshift==1): the projection is halved, so per projection column we
       write the two screen columns [c<<1, c<<1+1] with DISTINCT texels (two wide=0 calls) -> a full-res
       sprite over low-detail walls.  Both screen columns legitimately share the half-column clip.
   NORMAL (light-shaded) columns only; fuzz/translated fall back to full-res in the caller.  Mirrors the
   master R_DrawColumn inner loop (frac&127 texel, dc_iscale step).  Pure C (DoomJo builds it, never
   sets sat_sprite_ld). */
int sat_sprite_ld = 0;   /* 1 = LD software sprites; combines with detailshift for the 4 wall/sprite
                            quality combos (platform sat_apply_mode). */
static void R_DrawSpriteCol (column_t* column, int write_x, int clip_x, int wide)
{
    extern int   centery;
    extern byte *ylookup[];
    extern int   columnofs[];
    int     topscreen, bottomscreen, count;
    fixed_t basetexturemid = dc_texturemid;
    for ( ; column->topdelta != 0xff ; )
    {
        topscreen    = sprtopscreen + spryscale*column->topdelta;
        bottomscreen = topscreen + spryscale*column->length;
        dc_yl = (topscreen+FRACUNIT-1)>>FRACBITS;
        dc_yh = (bottomscreen-1)>>FRACBITS;
        if (dc_yh >= mfloorclip[clip_x])   dc_yh = mfloorclip[clip_x]-1;
        if (dc_yl <= mceilingclip[clip_x]) dc_yl = mceilingclip[clip_x]+1;
        if (dc_yh >= viewheight)           dc_yh = viewheight - 1;   /* split-viewport clamp (see R_DrawMaskedColumn) */
        if (dc_yl < 0)                     dc_yl = 0;
        if (dc_yl <= dc_yh)
        {
            byte   *src  = (byte *)column + 3;
            byte   *cmap = dc_colormap;
            fixed_t tm   = basetexturemid - (column->topdelta<<FRACBITS);
            fixed_t fracstep = dc_iscale;
            fixed_t frac = tm + (dc_yl-centery)*fracstep;
            byte   *d0 = ylookup[dc_yl] + columnofs[write_x];
            count = dc_yh - dc_yl + 1;
            if (wide)
            {
                byte *d1 = ylookup[dc_yl] + columnofs[write_x+1];
                while (count-- > 0)
                { byte c = cmap[src[(frac>>FRACBITS)&127]]; *d0 = c; *d1 = c;
                  d0 += SCREENWIDTH; d1 += SCREENWIDTH; frac += fracstep; }
            }
            else
            {
                while (count-- > 0)
                { *d0 = cmap[src[(frac>>FRACBITS)&127]]; d0 += SCREENWIDTH; frac += fracstep; }
            }
        }
        column = (column_t *)((byte *)column + column->length + 4);
    }
    dc_texturemid = basetexturemid;
}



//
// R_DrawVisSprite
//  mfloorclip and mceilingclip should also be set.
//
void
R_DrawVisSprite
( vissprite_t*		vis,
  int			x1,
  int			x2 )
{
    column_t*		column;
    int			texturecolumn;
    fixed_t		frac;
    patch_t*		patch;
    /* SATURN sprite-cost profiler: bracket the per-column masked FILL (master half). */
    extern void RP_SprFillEnter(void); extern void RP_SprFillLeave(void);
    RP_SprFillEnter();

    patch = W_CacheLumpNum (vis->patch+firstspritelump, PU_CACHE);

    dc_colormap = vis->colormap;
    
    if (!dc_colormap)
    {
	// NULL colormap = shadow draw
	colfunc = fuzzcolfunc;
    }
    else if (vis->mobjflags & MF_TRANSLATION)
    {
	colfunc = transcolfunc;
	dc_translation = translationtables - 256 +
	    ( (vis->mobjflags & MF_TRANSLATION) >> (MF_TRANSSHIFT-8) );
    }
	
    dc_iscale = abs(vis->xiscale)>>detailshift;
    dc_texturemid = vis->texturemid;
    frac = vis->startfrac;
    spryscale = vis->scale;
    sprtopscreen = centeryfrac - FixedMul(dc_texturemid,spryscale);

    {
    extern int g_mask_x1;   /* SATURN masked-by-half: the master draws sprite columns [0,x1) */
    /* SATURN sprite-SQ: the sprite's horizontal detail is decoupled from the wall/floor detailshift.
       NORMAL (light-shaded) columns only -- fuzz/translated keep the full-res colfunc path (rare).
       Four combos of (walls detailshift, sprite sat_sprite_ld):
         !ds,!sld -> full  : R_DrawMaskedColumn (colfunc, byte-identical to vanilla)
         !ds, sld -> LD     : step the projection column by 2, wide -> half-res fill
          ds,!sld -> FULL over LD walls : per half-column, write the 2 screen cols with distinct texels
          ds, sld -> LD (rides the low-detail colfunc) : R_DrawMaskedColumn */
    int normal = dc_colormap && !(vis->mobjflags & MF_TRANSLATION);
    if (normal && !detailshift && sat_sprite_ld)
    {   /* walls full, sprite LD: downsample (fill saved) */
	for (dc_x=vis->x1 ; dc_x<=vis->x2 ; dc_x+=2, frac += (vis->xiscale<<1))
	{
	    int wide;
	    if (dc_x >= g_mask_x1) break;
	    texturecolumn = frac>>FRACBITS;
	    if ((unsigned)texturecolumn >= (unsigned)SHORT(patch->width))
		continue;
	    column = (column_t *) ((byte *)patch +
				   LONG(patch->columnofs[texturecolumn]));
	    wide = (dc_x+1 <= vis->x2) && (dc_x+1 < g_mask_x1);   /* don't spill into the slave's half / off the sprite */
	    R_DrawSpriteCol (column, dc_x, dc_x, wide);
	}
    }
    else if (normal && detailshift && !sat_sprite_ld)
    {   /* walls LD, sprite FULL: upsample -- 2 distinct texels per (halved) projection column */
	for (dc_x=vis->x1 ; dc_x<=vis->x2 ; dc_x++, frac += vis->xiscale)
	{
	    int sx, tcL, tcR;
	    if (dc_x >= g_mask_x1) break;
	    sx  = dc_x << 1;                                      /* left screen column of this half-column */
	    tcL = frac >> FRACBITS;
	    tcR = (frac + (vis->xiscale>>1)) >> FRACBITS;         /* half a projection-column further into the patch */
	    if ((unsigned)tcL < (unsigned)SHORT(patch->width))
		R_DrawSpriteCol ((column_t *)((byte *)patch + LONG(patch->columnofs[tcL])), sx,   dc_x, 0);
	    if ((unsigned)tcR < (unsigned)SHORT(patch->width))
		R_DrawSpriteCol ((column_t *)((byte *)patch + LONG(patch->columnofs[tcR])), sx+1, dc_x, 0);
	}
    }
    else
    for (dc_x=vis->x1 ; dc_x<=vis->x2 ; dc_x++, frac += vis->xiscale)
    {
	if (dc_x >= g_mask_x1) break;
	texturecolumn = frac>>FRACBITS;
	/* SATURN: frac can go ±1 step past the patch boundary due to fixed-point
	   rounding in the FixedDiv/scale path.  Skip (continue) rather than
	   I_Error or clamping — clamping to width-1 caused R_DrawMaskedColumn to
	   walk corrupt WAD data without a 0xff terminator, scanning ~4MB of cart
	   RAM for ~350ms per bad column and dragging fps to 1.  Skipping the
	   column draws a transparent pixel for that step, which is invisible. */
	if ((unsigned)texturecolumn >= (unsigned)SHORT(patch->width))
	    continue;
	column = (column_t *) ((byte *)patch +
			       LONG(patch->columnofs[texturecolumn]));
	R_DrawMaskedColumn (column);
    }
    }   /* end g_mask_x1 block */

    colfunc = basecolfunc;
    RP_SprFillLeave();
}



//
// R_ProjectSprite
// Generates a vissprite for a thing
//  if it might be visible.
//
void R_ProjectSprite (mobj_t* thing)
{
    fixed_t		tr_x;
    fixed_t		tr_y;
    
    fixed_t		gxt;
    fixed_t		gyt;
    
    fixed_t		tx;
    fixed_t		tz;

    fixed_t		xscale;
    
    int			x1;
    int			x2;

    spritedef_t*	sprdef;
    spriteframe_t*	sprframe;
    int			lump;
    
    unsigned		rot;
    boolean		flip;
    
    int			index;

    vissprite_t*	vis;
    
    angle_t		ang;
    fixed_t		iscale;
    
    // transform the origin point
    tr_x = thing->x - viewx;
    tr_y = thing->y - viewy;
	
    gxt = FixedMul(tr_x,viewcos); 
    gyt = -FixedMul(tr_y,viewsin);
    
    tz = gxt-gyt; 

    // thing is behind view plane?
    if (tz < MINZ)
	return;
    
    xscale = FixedDiv(projection, tz);
	
    gxt = -FixedMul(tr_x,viewsin); 
    gyt = FixedMul(tr_y,viewcos); 
    tx = -(gyt+gxt); 

    // too far off the side?
    if (abs(tx)>(tz<<2))
	return;
    
    // decide which patch to use for sprite relative to player
#ifdef RANGECHECK
    if ((unsigned int) thing->sprite >= (unsigned int) numsprites)
	I_Error ("R_ProjectSprite: invalid sprite number %i ",
		 thing->sprite);
#endif
    sprdef = &sprites[thing->sprite];
#ifdef RANGECHECK
    if ( (thing->frame&FF_FRAMEMASK) >= sprdef->numframes )
	I_Error ("R_ProjectSprite: invalid sprite frame %i : %i ",
		 thing->sprite, thing->frame);
#endif
    sprframe = &sprdef->spriteframes[ thing->frame & FF_FRAMEMASK];

    if (sprframe->rotate)
    {
	// choose a different rotation based on player view
	ang = R_PointToAngle (thing->x, thing->y);
	rot = (ang-thing->angle+(unsigned)(ANG45/2)*9)>>29;
	lump = sprframe->lump[rot];
	flip = (boolean)sprframe->flip[rot];
    }
    else
    {
	// use single rotation for all views
	lump = sprframe->lump[0];
	flip = (boolean)sprframe->flip[0];
    }
    
    // calculate edges of the shape
    tx -= spriteoffset[lump];	
    x1 = (centerxfrac + FixedMul (tx,xscale) ) >>FRACBITS;

    // off the right side?
    if (x1 > viewwidth)
	return;
    
    tx +=  spritewidth[lump];
    x2 = ((centerxfrac + FixedMul (tx,xscale) ) >>FRACBITS) - 1;

    // off the left side
    if (x2 < 0)
	return;
    
    // store information in a vissprite
    vis = R_NewVisSprite ();
    vis->mobjflags = thing->flags;
    vis->scale = xscale<<detailshift;
    vis->gx = thing->x;
    vis->gy = thing->y;
    vis->gz = thing->z;
    vis->gzt = thing->z + spritetopoffset[lump];
    vis->texturemid = vis->gzt - viewz;
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;	
    iscale = FixedDiv (FRACUNIT, xscale);

    if (flip)
    {
	vis->startfrac = spritewidth[lump]-1;
	vis->xiscale = -iscale;
    }
    else
    {
	vis->startfrac = 0;
	vis->xiscale = iscale;
    }

    if (vis->x1 > x1)
	vis->startfrac += vis->xiscale*(vis->x1-x1);
    vis->patch = lump;
    
    // get light level
    if (thing->flags & MF_SHADOW)
    {
	// shadow draw
	vis->colormap = NULL;
    }
    else if (fixedcolormap)
    {
	// fixed map
	vis->colormap = fixedcolormap;
    }
    else if (thing->frame & FF_FULLBRIGHT)
    {
	// full bright
	vis->colormap = colormaps;
    }
    
    else
    {
	// diminished light
	index = xscale>>(LIGHTSCALESHIFT-detailshift);

	if (index >= MAXLIGHTSCALE) 
	    index = MAXLIGHTSCALE-1;

	vis->colormap = spritelights[index];
    }	
}




//
// R_AddSprites
// During BSP traversal, this adds sprites by sector.
//
void R_AddSprites (sector_t* sec)
{
    mobj_t*		thing;
    int			lightnum;

    // BSP is traversed by subsector.
    // A sector might have been split into several
    //  subsectors during BSP building.
    // Thus we check whether its already added.
    if (sec->validcount == validcount)
	return;		

    // Well, now it will be done.
    sec->validcount = validcount;
	
    lightnum = (sec->lightlevel >> LIGHTSEGSHIFT)+extralight;

    if (lightnum < 0)		
	spritelights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
	spritelights = scalelight[LIGHTLEVELS-1];
    else
	spritelights = scalelight[lightnum];

    // Handle all things in sector.
    /* SATURN sprite-cost profiler: bracket the projection (folded into Bw today). */
    {
	extern void RP_SprProjEnter(void); extern void RP_SprProjLeave(int);
	int _n = 0;
	RP_SprProjEnter();
	for (thing = sec->thinglist ; thing ; thing = thing->snext)
	{ R_ProjectSprite (thing); _n++; }
	RP_SprProjLeave(_n);
    }
}


//
// R_DrawPSprite
//
void R_DrawPSprite (pspdef_t* psp)
{
    fixed_t		tx;
    int			x1;
    int			x2;
    spritedef_t*	sprdef;
    spriteframe_t*	sprframe;
    int			lump;
    boolean		flip;
    vissprite_t*	vis;
    vissprite_t		avis;
    
    // decide which patch to use
#ifdef RANGECHECK
    if ( (unsigned)psp->state->sprite >= (unsigned int) numsprites)
	I_Error ("R_ProjectSprite: invalid sprite number %i ",
		 psp->state->sprite);
#endif
    sprdef = &sprites[psp->state->sprite];
#ifdef RANGECHECK
    if ( (psp->state->frame & FF_FRAMEMASK)  >= sprdef->numframes)
	I_Error ("R_ProjectSprite: invalid sprite frame %i : %i ",
		 psp->state->sprite, psp->state->frame);
#endif
    sprframe = &sprdef->spriteframes[ psp->state->frame & FF_FRAMEMASK ];

    lump = sprframe->lump[0];
    flip = (boolean)sprframe->flip[0];
    
    // calculate edges of the shape
    tx = psp->sx-160*FRACUNIT;
	
    tx -= spriteoffset[lump];	
    x1 = (centerxfrac + FixedMul (tx,pspritescale) ) >>FRACBITS;

    // off the right side
    if (x1 > viewwidth)
	return;		

    tx +=  spritewidth[lump];
    x2 = ((centerxfrac + FixedMul (tx, pspritescale) ) >>FRACBITS) - 1;

    // off the left side
    if (x2 < 0)
	return;
    
    // store information in a vissprite
    vis = &avis;
    vis->mobjflags = 0;
    vis->texturemid = (BASEYCENTER<<FRACBITS)+FRACUNIT/2-(psp->sy-spritetopoffset[lump]);
    vis->x1 = x1 < 0 ? 0 : x1;
    vis->x2 = x2 >= viewwidth ? viewwidth-1 : x2;
    vis->scale = pspritescale<<detailshift;
    /* SATURN: drop the weapon by sat_psprite_yoff screen px (2-player split). */
    if (sat_psprite_yoff)
	vis->texturemid -= FixedDiv(sat_psprite_yoff << FRACBITS, vis->scale);

    if (flip)
    {
	vis->xiscale = -pspriteiscale;
	vis->startfrac = spritewidth[lump]-1;
    }
    else
    {
	vis->xiscale = pspriteiscale;
	vis->startfrac = 0;
    }
    
    if (vis->x1 > x1)
	vis->startfrac += vis->xiscale*(vis->x1-x1);

    vis->patch = lump;

    if (viewplayer->powers[pw_invisibility] > 4*32
	|| viewplayer->powers[pw_invisibility] & 8)
    {
	// shadow draw
	vis->colormap = NULL;
    }
    else if (fixedcolormap)
    {
	// fixed color
	vis->colormap = fixedcolormap;
    }
    else if (psp->state->frame & FF_FULLBRIGHT)
    {
	// full bright
	vis->colormap = colormaps;
    }
    else
    {
	// local light
	vis->colormap = spritelights[MAXLIGHTSCALE-1];
    }

    /* SATURN: draw the weapon on VDP1 (hardware sprite layer, async, in parallel
       with the SH-2s) instead of the software framebuffer.  Only when VDP1 is active
       this view (sat_wall_skip) -- M0 / software-wall split MUST keep the SOFTWARE
       weapon (else the always-set hook routes it to VDP1 even in M0).  Only the opaque,
       non-translated case (vis->colormap != NULL); the invisibility "shadow" (NULL
       colormap = fuzz) falls through to the software path below. */
    { extern int sat_wall_skip;
    if (sat_psprite_hook && sat_wall_skip && vis->colormap)
    {
	int ytop = (centeryfrac - FixedMul(vis->texturemid, vis->scale)) >> FRACBITS;
	patch_t *wp = W_CacheLumpNum (vis->patch+firstspritelump, PU_CACHE);
	sat_psprite_hook (wp, vis->patch, x1, ytop, (int)flip, vis->colormap);
	return;
    } }

    R_DrawVisSprite (vis, vis->x1, vis->x2);
}



//
// R_DrawPlayerSprites
//
void R_DrawPlayerSprites (void)
{
    int		i;
    int		lightnum;
    pspdef_t*	psp;
    
    // get light level
    lightnum =
	(viewplayer->mo->subsector->sector->lightlevel >> LIGHTSEGSHIFT) 
	+extralight;

    if (lightnum < 0)		
	spritelights = scalelight[0];
    else if (lightnum >= LIGHTLEVELS)
	spritelights = scalelight[LIGHTLEVELS-1];
    else
	spritelights = scalelight[lightnum];
    
    // clip to screen bounds
    mfloorclip = screenheightarray;
    mceilingclip = negonearray;

    /* SATURN: start a fresh VDP1 command list for this frame's player sprites -- only when VDP1 is
       active this view (sat_wall_skip); in M0 / software-wall split the weapon is software (no VDP1
       clip command emitted either). */
    { extern int sat_wall_skip;
    if (sat_psprite_begin && sat_wall_skip)
	sat_psprite_begin();
    }

    // add all active psprites
    for (i=0, psp=viewplayer->psprites;
	 i<NUMPSPRITES;
	 i++,psp++)
    {
	if (psp->state)
	    R_DrawPSprite (psp);
    }
}




//
// R_SortVisSprites
//
vissprite_t	vsprsortedhead;


void R_SortVisSprites (void)
{
    int			i;
    int			count;
    vissprite_t*	ds;
    vissprite_t*	best;
    vissprite_t		unsorted;
    fixed_t		bestscale;

    count = vissprite_p - vissprites;
	
    unsorted.next = unsorted.prev = &unsorted;

    if (!count)
	return;
		
    for (ds=vissprites ; ds<vissprite_p ; ds++)
    {
	ds->next = ds+1;
	ds->prev = ds-1;
    }
    
    vissprites[0].prev = &unsorted;
    unsorted.next = &vissprites[0];
    (vissprite_p-1)->next = &unsorted;
    unsorted.prev = vissprite_p-1;
    
    // pull the vissprites out by scale

    vsprsortedhead.next = vsprsortedhead.prev = &vsprsortedhead;
    for (i=0 ; i<count ; i++)
    {
	bestscale = INT_MAX;
        best = unsorted.next;
	for (ds=unsorted.next ; ds!= &unsorted ; ds=ds->next)
	{
	    if (ds->scale < bestscale)
	    {
		bestscale = ds->scale;
		best = ds;
	    }
	}
	best->next->prev = best->prev;
	best->prev->next = best->next;
	best->next = &vsprsortedhead;
	best->prev = vsprsortedhead.prev;
	vsprsortedhead.prev->next = best;
	vsprsortedhead.prev = best;
    }
}



//
// R_DrawSprite
//
static short		clipbot[SCREENWIDTH];
static short		cliptop[SCREENWIDTH];
/* SATURN: split R_DrawSprite so the VDP1 world-things path can compute a sprite's per-column
   clip (clipbot/cliptop) against the drawsegs WITHOUT drawing it or rendering masked mid-textures
   (dorender=0).  The software path passes dorender=1 (identical to the old R_DrawSprite). */
static void R_ClipSprite (vissprite_t* spr, int dorender)
{
    drawseg_t*		ds;
    int			x;
    int			r1;
    int			r2;
    fixed_t		scale;
    fixed_t		lowscale;
    int			silhouette;
		
    for (x = spr->x1 ; x<=spr->x2 ; x++)
	clipbot[x] = cliptop[x] = -2;
    
    // Scan drawsegs from end to start for obscuring segs.
    // The first drawseg that has a greater scale
    //  is the clip seg.
    for (ds=ds_p-1 ; ds >= drawsegs ; ds--)
    {
	// determine if the drawseg obscures the sprite
	if (ds->x1 > spr->x2
	    || ds->x2 < spr->x1
	    || (!ds->silhouette
		&& !ds->maskedtexturecol) )
	{
	    // does not cover sprite
	    continue;
	}
			
	r1 = ds->x1 < spr->x1 ? spr->x1 : ds->x1;
	r2 = ds->x2 > spr->x2 ? spr->x2 : ds->x2;

	if (ds->scale1 > ds->scale2)
	{
	    lowscale = ds->scale2;
	    scale = ds->scale1;
	}
	else
	{
	    lowscale = ds->scale1;
	    scale = ds->scale2;
	}
		
	if (scale < spr->scale
	    || ( lowscale < spr->scale
		 && !R_PointOnSegSide (spr->gx, spr->gy, ds->curline) ) )
	{
	    // masked mid texture? (software draw only; the VDP1 clip pass passes dorender=0)
	    if (dorender && ds->maskedtexturecol)
		R_RenderMaskedSegRange (ds, r1, r2);
	    // seg is behind sprite
	    continue;			
	}

	
	// clip this piece of the sprite
	silhouette = ds->silhouette;
	
	if (spr->gz >= ds->bsilheight)
	    silhouette &= ~SIL_BOTTOM;

	if (spr->gzt <= ds->tsilheight)
	    silhouette &= ~SIL_TOP;
			
	if (silhouette == 1)
	{
	    // bottom sil
	    for (x=r1 ; x<=r2 ; x++)
		if (clipbot[x] == -2)
		    clipbot[x] = ds->sprbottomclip[x];
	}
	else if (silhouette == 2)
	{
	    // top sil
	    for (x=r1 ; x<=r2 ; x++)
		if (cliptop[x] == -2)
		    cliptop[x] = ds->sprtopclip[x];
	}
	else if (silhouette == 3)
	{
	    // both
	    for (x=r1 ; x<=r2 ; x++)
	    {
		if (clipbot[x] == -2)
		    clipbot[x] = ds->sprbottomclip[x];
		if (cliptop[x] == -2)
		    cliptop[x] = ds->sprtopclip[x];
	    }
	}
		
    }
    
    // all clipping has been performed, so draw the sprite

    // check for unclipped columns
    for (x = spr->x1 ; x<=spr->x2 ; x++)
    {
	if (clipbot[x] == -2)		
	    clipbot[x] = viewheight;

	if (cliptop[x] == -2)
	    cliptop[x] = -1;
    }
		
}

void R_DrawSprite (vissprite_t* spr)
{
    R_ClipSprite (spr, 1);
    mfloorclip = clipbot;
    mceilingclip = cliptop;
    R_DrawVisSprite (spr, spr->x1, spr->x2);
}




//
// R_DrawMasked
//
/* ============================================================================
 * SATURN parallel-REC (Option B / masked-by-half): the slave SH-2 draws the
 * vissprites in the RIGHT screen-x half while the master draws the LEFT half,
 * during the masked phase (M).  Self-contained -- its column state is a SEPARATE
 * static set (s_*) drawn on a dedicated stack (Mimas has no GBR-TLS, GBR is
 * SGL's), so it races nothing with the master.  Both iterate the SAME sorted
 * vissprites (z-order preserved); each clips its columns to its half -> disjoint
 * pixels.  Sprite CLIPPING against the shared drawsegs is replicated; masked WALLS
 * stay on the master full-width (rare in shareware; a grate behind a slave-half
 * sprite can race -- a known limit, fixed next).  Translated columns (other
 * players' colours in local MP) are handled too (R_SlaveDrawTransColumn), so the
 * slave (right) half shows the correct per-player colour, not the base green. */
int sat_masked_parallel = 0;          /* gate, set by the platform (src/main.cxx) */
int g_mask_x1 = 32767;                 /* master vissprite right clip [0,x1); reset to viewwidth each use */

static int      s_dc_x, s_dc_yl, s_dc_yh, s_fuzzpos, s_x0, s_x1, s_coltype;
static fixed_t  s_dc_iscale, s_dc_texturemid, s_spryscale, s_sprtopscreen;
static byte    *s_dc_source;
static byte    *s_dc_translation;   /* SATURN masked-by-half: player-colour translation table (MF_TRANSLATION) */
static lighttable_t *s_dc_colormap;
static short   *s_mfloorclip, *s_mceilingclip;
static short    s_clipbot[SCREENWIDTH], s_cliptop[SCREENWIDTH];

/* r_draw.c globals used by the self-contained slave column draw (not in r_things.c's headers) */
extern byte *ylookup[];
extern int   columnofs[];
extern int   fuzzoffset[];
#ifndef FUZZTABLE
#define FUZZTABLE 50
#endif

static void R_SlaveDrawColumn (void)
{
    int count = s_dc_yh - s_dc_yl + 1;
    byte *source = s_dc_source, *colormap = (byte *)s_dc_colormap;
    unsigned fracstep = s_dc_iscale<<9;
    unsigned frac = (s_dc_texturemid + (s_dc_yl-centery)*s_dc_iscale)<<9;
    if (detailshift)   /* low-detail: s_dc_x is the HALVED column -> pixel-double into 2 screen px */
    {
	int sx = s_dc_x << 1;
	byte *d0 = ylookup[s_dc_yl] + columnofs[sx];
	byte *d1 = ylookup[s_dc_yl] + columnofs[sx+1];
	while (count-- > 0) { byte c = colormap[source[frac>>25]]; *d0 = c; *d1 = c;
			      d0 += SCREENWIDTH; d1 += SCREENWIDTH; frac += fracstep; }
    }
    else
    {
	byte *dest = ylookup[s_dc_yl] + columnofs[s_dc_x];
	while (count-- > 0) { *dest = colormap[source[frac>>25]]; dest += SCREENWIDTH; frac += fracstep; }
    }
}

/* SATURN masked-by-half: like R_SlaveDrawColumn but with the player-colour
   translation (xlat) step, so the slave's RIGHT half recolours other players'
   marines (indigo/brown/red) instead of leaving them the base green.  Mirrors the
   master's R_DrawTranslatedColumn / the RP_TRANS executor (rp_exec_trans). */
static void R_SlaveDrawTransColumn (void)
{
    int count = s_dc_yh - s_dc_yl + 1;
    byte *source = s_dc_source, *colormap = (byte *)s_dc_colormap, *xlat = s_dc_translation;
    unsigned fracstep = s_dc_iscale<<9;
    unsigned frac = (s_dc_texturemid + (s_dc_yl-centery)*s_dc_iscale)<<9;
    if (detailshift)   /* low-detail: s_dc_x is the HALVED column -> pixel-double into 2 screen px */
    {
	int sx = s_dc_x << 1;
	byte *d0 = ylookup[s_dc_yl] + columnofs[sx];
	byte *d1 = ylookup[s_dc_yl] + columnofs[sx+1];
	while (count-- > 0) { byte c = colormap[xlat[source[frac>>25]]]; *d0 = c; *d1 = c;
			      d0 += SCREENWIDTH; d1 += SCREENWIDTH; frac += fracstep; }
    }
    else
    {
	byte *dest = ylookup[s_dc_yl] + columnofs[s_dc_x];
	while (count-- > 0) { *dest = colormap[xlat[source[frac>>25]]]; dest += SCREENWIDTH; frac += fracstep; }
    }
}

static void R_SlaveFuzzColumn (void)
{
    int count; byte *dest;
    if (!s_dc_yl) s_dc_yl = 1;
    if (s_dc_yh == viewheight-1) s_dc_yh = viewheight - 2;
    count = s_dc_yh - s_dc_yl;
    if (count < 0) return;
    if (detailshift)   /* low-detail: pixel-double the fuzz into the 2 screen columns */
    {
	int sx = s_dc_x << 1;
	byte *d0 = ylookup[s_dc_yl] + columnofs[sx];
	byte *d1 = ylookup[s_dc_yl] + columnofs[sx+1];
	do {
	    *d0 = colormaps[6*256+d0[fuzzoffset[s_fuzzpos]]];
	    *d1 = colormaps[6*256+d1[fuzzoffset[s_fuzzpos]]];
	    if (++s_fuzzpos == FUZZTABLE) s_fuzzpos = 0;
	    d0 += SCREENWIDTH; d1 += SCREENWIDTH;
	} while (count--);
	return;
    }
    dest = ylookup[s_dc_yl] + columnofs[s_dc_x];
    do {
        *dest = colormaps[6*256+dest[fuzzoffset[s_fuzzpos]]];
        if (++s_fuzzpos == FUZZTABLE) s_fuzzpos = 0;
        dest += SCREENWIDTH;
    } while (count--);
}

static void R_SlaveDrawMaskedColumn (column_t* column)
{
    int topscreen, bottomscreen;
    fixed_t basetexturemid = s_dc_texturemid;
    for ( ; column->topdelta != 0xff ; )
    {
        topscreen = s_sprtopscreen + s_spryscale*column->topdelta;
        bottomscreen = topscreen + s_spryscale*column->length;
        s_dc_yl = (topscreen+FRACUNIT-1)>>FRACBITS;
        s_dc_yh = (bottomscreen-1)>>FRACBITS;
        if (s_dc_yh >= s_mfloorclip[s_dc_x]) s_dc_yh = s_mfloorclip[s_dc_x]-1;
        if (s_dc_yl <= s_mceilingclip[s_dc_x]) s_dc_yl = s_mceilingclip[s_dc_x]+1;
        if (s_dc_yh >= viewheight) s_dc_yh = viewheight - 1;  /* SATURN: clamp to viewport (see R_DrawMaskedColumn); slave drawer is unguarded */
        if (s_dc_yl < 0)           s_dc_yl = 0;
        if (s_dc_yl <= s_dc_yh)
        {
            s_dc_source = (byte *)column + 3;
            s_dc_texturemid = basetexturemid - (column->topdelta<<FRACBITS);
            if (s_coltype == 1)      R_SlaveFuzzColumn();
            else if (s_coltype == 2) R_SlaveDrawTransColumn();
            else                     R_SlaveDrawColumn();
        }
        column = (column_t *)((byte *)column + column->length + 4);
    }
    s_dc_texturemid = basetexturemid;
}

/* SATURN sprite-SQ (slave half): the R_DrawSpriteCol twin -- write decoupled from clip (see the master
   version for the 4-combo rationale).  Normal columns only (s_coltype==0); clip in projection column
   space (s_mfloorclip/s_mceilingclip[clip_x]).  `wide` also writes write_x+1. */
static void R_SlaveDrawSpriteCol (column_t* column, int write_x, int clip_x, int wide)
{
    int topscreen, bottomscreen, count;
    fixed_t basetexturemid = s_dc_texturemid;
    for ( ; column->topdelta != 0xff ; )
    {
        topscreen    = s_sprtopscreen + s_spryscale*column->topdelta;
        bottomscreen = topscreen + s_spryscale*column->length;
        s_dc_yl = (topscreen+FRACUNIT-1)>>FRACBITS;
        s_dc_yh = (bottomscreen-1)>>FRACBITS;
        if (s_dc_yh >= s_mfloorclip[clip_x])   s_dc_yh = s_mfloorclip[clip_x]-1;
        if (s_dc_yl <= s_mceilingclip[clip_x]) s_dc_yl = s_mceilingclip[clip_x]+1;
        if (s_dc_yh >= viewheight)             s_dc_yh = viewheight - 1;
        if (s_dc_yl < 0)                       s_dc_yl = 0;
        if (s_dc_yl <= s_dc_yh)
        {
            byte     *source   = (byte *)column + 3, *colormap = (byte *)s_dc_colormap;
            unsigned  fracstep = s_dc_iscale<<9;
            unsigned  frac     = ((basetexturemid - (column->topdelta<<FRACBITS)) + (s_dc_yl-centery)*s_dc_iscale)<<9;
            byte     *d0 = ylookup[s_dc_yl] + columnofs[write_x];
            count = s_dc_yh - s_dc_yl + 1;
            if (wide)
            {
                byte *d1 = ylookup[s_dc_yl] + columnofs[write_x+1];
                while (count-- > 0) { byte c = colormap[source[frac>>25]]; *d0 = c; *d1 = c;
                                      d0 += SCREENWIDTH; d1 += SCREENWIDTH; frac += fracstep; }
            }
            else
                while (count-- > 0) { *d0 = colormap[source[frac>>25]]; d0 += SCREENWIDTH; frac += fracstep; }
        }
        column = (column_t *)((byte *)column + column->length + 4);
    }
    s_dc_texturemid = basetexturemid;
}

static void R_SlaveDrawVisSprite (vissprite_t* vis)
{
    column_t *column; int texturecolumn; fixed_t frac; patch_t *patch;
    patch = W_CacheLumpNum (vis->patch+firstspritelump, PU_CACHE);
    s_dc_colormap = vis->colormap;
    if (!s_dc_colormap)
        s_coltype = 1;                        /* NULL colormap = fuzz/shadow */
    else if (vis->mobjflags & MF_TRANSLATION) /* other players' colours (local MP) */
    {
        s_coltype = 2;
        s_dc_translation = translationtables - 256 +
            ( (vis->mobjflags & MF_TRANSLATION) >> (MF_TRANSSHIFT-8) );
    }
    else
        s_coltype = 0;                        /* normal */
    s_dc_iscale = abs(vis->xiscale)>>detailshift;
    s_dc_texturemid = vis->texturemid;
    frac = vis->startfrac;
    s_spryscale = vis->scale;
    s_sprtopscreen = centeryfrac - FixedMul(s_dc_texturemid,s_spryscale);
    {
    int normal = (s_coltype == 0);   /* fuzz/translated keep the full-res masked path */
    if (normal && !detailshift && sat_sprite_ld)
    {   /* walls full, sprite LD: downsample */
        for (s_dc_x=vis->x1 ; s_dc_x<=vis->x2 ; s_dc_x+=2, frac += (vis->xiscale<<1))
        {
            int wide;
            if (s_dc_x < s_x0 || s_dc_x >= s_x1) continue;
            texturecolumn = frac>>FRACBITS;
            if ((unsigned)texturecolumn >= (unsigned)SHORT(patch->width)) continue;
            column = (column_t *) ((byte *)patch + LONG(patch->columnofs[texturecolumn]));
            wide = (s_dc_x+1 <= vis->x2) && (s_dc_x+1 < s_x1);
            R_SlaveDrawSpriteCol (column, s_dc_x, s_dc_x, wide);
        }
    }
    else if (normal && detailshift && !sat_sprite_ld)
    {   /* walls LD, sprite FULL: upsample -- 2 distinct texels per half-column */
        for (s_dc_x=vis->x1 ; s_dc_x<=vis->x2 ; s_dc_x++, frac += vis->xiscale)
        {
            int sx, tcL, tcR;
            if (s_dc_x < s_x0 || s_dc_x >= s_x1) continue;
            sx  = s_dc_x << 1;
            tcL = frac >> FRACBITS;
            tcR = (frac + (vis->xiscale>>1)) >> FRACBITS;
            if ((unsigned)tcL < (unsigned)SHORT(patch->width))
                R_SlaveDrawSpriteCol ((column_t *)((byte *)patch + LONG(patch->columnofs[tcL])), sx,   s_dc_x, 0);
            if ((unsigned)tcR < (unsigned)SHORT(patch->width))
                R_SlaveDrawSpriteCol ((column_t *)((byte *)patch + LONG(patch->columnofs[tcR])), sx+1, s_dc_x, 0);
        }
    }
    else
    for (s_dc_x=vis->x1 ; s_dc_x<=vis->x2 ; s_dc_x++, frac += vis->xiscale)
    {
        if (s_dc_x < s_x0 || s_dc_x >= s_x1) continue;    /* this CPU's x-half only */
        texturecolumn = frac>>FRACBITS;
        if ((unsigned)texturecolumn >= (unsigned)SHORT(patch->width)) continue;
        column = (column_t *) ((byte *)patch + LONG(patch->columnofs[texturecolumn]));
        R_SlaveDrawMaskedColumn (column);
    }
    }
}

static void R_SlaveDrawSprite (vissprite_t* spr)
{
    drawseg_t *ds; int x, r1, r2; fixed_t scale, lowscale; int silhouette;
    int lo = spr->x1 < s_x0 ? s_x0 : spr->x1;     /* only this half's columns */
    int hi = spr->x2 >= s_x1 ? s_x1-1 : spr->x2;
    if (lo > hi) return;                          /* sprite entirely outside this half */
    for (x = lo ; x<=hi ; x++) s_clipbot[x] = s_cliptop[x] = -2;
    for (ds=ds_p-1 ; ds >= drawsegs ; ds--)
    {
        if (ds->x1 > spr->x2 || ds->x2 < spr->x1 || (!ds->silhouette && !ds->maskedtexturecol)) continue;
        r1 = ds->x1 < spr->x1 ? spr->x1 : ds->x1;
        r2 = ds->x2 > spr->x2 ? spr->x2 : ds->x2;
        if (r1 < lo) r1 = lo;
        if (r2 > hi) r2 = hi;
        if (r1 > r2) continue;                    /* seg doesn't touch this half */
        if (ds->scale1 > ds->scale2) { lowscale = ds->scale2; scale = ds->scale1; }
        else { lowscale = ds->scale1; scale = ds->scale2; }
        if (scale < spr->scale || (lowscale < spr->scale
            && !R_PointOnSegSide (spr->gx, spr->gy, ds->curline)))
            continue;                             /* seg behind sprite: master draws its wall */
        silhouette = ds->silhouette;
        if (spr->gz >= ds->bsilheight) silhouette &= ~SIL_BOTTOM;
        if (spr->gzt <= ds->tsilheight) silhouette &= ~SIL_TOP;
        if (silhouette == 1) { for (x=r1;x<=r2;x++) if (s_clipbot[x]==-2) s_clipbot[x]=ds->sprbottomclip[x]; }
        else if (silhouette == 2) { for (x=r1;x<=r2;x++) if (s_cliptop[x]==-2) s_cliptop[x]=ds->sprtopclip[x]; }
        else if (silhouette == 3) { for (x=r1;x<=r2;x++) { if (s_clipbot[x]==-2) s_clipbot[x]=ds->sprbottomclip[x]; if (s_cliptop[x]==-2) s_cliptop[x]=ds->sprtopclip[x]; } }
    }
    for (x = lo ; x<=hi ; x++) { if (s_clipbot[x]==-2) s_clipbot[x]=viewheight; if (s_cliptop[x]==-2) s_cliptop[x]=-1; }
    s_mfloorclip = s_clipbot; s_mceilingclip = s_cliptop;
    R_SlaveDrawVisSprite (spr);
}

/* slave entry (r_parallel.c dispatches it): draw the sorted vissprites in [x0,x1). */
void R_SlaveDrawMasked (int x0, int x1)
{
    vissprite_t *spr;
    s_x0 = x0; s_x1 = x1;
    for (spr = vsprsortedhead.next ; spr != &vsprsortedhead ; spr=spr->next)
        R_SlaveDrawSprite (spr);
}

/* World-things offload gate.  Two questions, two different metrics:
     (a) "is each pixel of this sprite worth offloading?"  -> the VDP1 texture BAKE vs the software
         FILL it removes.  With the platform TEXTURE CACHE (bake once per lump/colormap/flip, reused
         while the sprite stays on screen) the bake amortises over the sprite's dwell, so any on-screen
         sprite is a net win -- the old magnification threshold (bake paid EVERY frame) is obsolete.
     (b) "does this sprite deserve one of the scarce VDP1 slots (cap/VRAM/cmd)?"  -> its ABSOLUTE
         on-screen AREA (= software fill it saves = wpx*hpx).  A tiny close pickup (armour helmet)
         passes any magnification test but covers ~nothing, yet would steal a slot from a big monster
         behind it.  So the gate is: area >= a %-of-view FLOOR.
     (c) "how many enemies can we take?"  -> the cache made the scarce resource DISTINCT TEXTURES,
         not sprite count: N enemies of the same type+facing share ONE (lump,cmap) = ONE slot.  So
         the sat_thing_cap slots are GRANTED to the distinct textures with the largest sprite, and
         then EVERY above-floor sprite using a granted texture rides free -> a horde of one type
         offloads WHOLESALE for a few bakes.  THIS is what turns "more enemies" into a win.
   THING_MIN_SCREEN_PCT = min sprite area as a percent of the view area (excludes tiny pickups).
   THING_TEX_TRACK      = max distinct (lump,cmap) textures tracked per frame for the slot grant.
   THING_EMIT_MAX       = HARD max THINGS emitted to VDP1 per frame (array bound + outdoor ceiling).
     This is the VDP1 RASTER bound, NOT the VRAM/texture bound: the VDP1 (1-cycle-auto) plots the
     whole command list in one frame and DROPS the tail if it overruns -- and a dropped thing
     VANISHES (the software fill was already skipped when the platform accepted it) = enemy flicker.
     The walls SHARE that raster, and their share varies wildly (open outdoor = few segs, spare VDP1;
     tech room = dense architecture = VDP1 already near full -- flickered at only th4!).  So the
     ACTUAL per-frame cap is sat_thing_emit_cap, which the platform AIMD-adapts on the real overrun
     signal (grow when the plot finished, back off when it overran): it climbs outdoors and shrinks
     below the flicker threshold in the tech room automatically.  THING_EMIT_MAX just bounds the top
     end (and the selection scratch arrays); the rest stay software = drawn correctly, no flicker. */
#define THING_MIN_SCREEN_PCT 2
#define THING_TEX_TRACK      32
#define THING_EMIT_MAX       16

/* Per-frame VDP1 things budget -- AIMD-adapted by the platform on the VDP1 overrun signal
   (sat_walls_kick), clamped to [0, THING_EMIT_MAX] by PASS 1.  Shared with the platform. */
int sat_thing_emit_cap = 4;

/* On-screen area (wpx*hpx = the software fill this sprite would cost) or -1 if it must stay
   software (fuzz/shadow, other-player translation, degenerate).  Shared by the two selection
   passes; W_CacheLumpNum here is a cheap cache hit (the patch is (re)cached for the software draw
   anyway). */
static long R_ThingScreenArea (vissprite_t *spr)
{
    int      patchw, wpx, hpx;
    patch_t *patch;
    if (!spr->colormap)                  return -1;   /* fuzz/shadow -> software */
    if (spr->mobjflags & MF_TRANSLATION) return -1;   /* other-player colour -> software */
    patchw = spritewidth[spr->patch] >> FRACBITS;
    if (patchw < 1) return -1;
    patch = W_CacheLumpNum (spr->patch+firstspritelump, PU_CACHE);
    wpx = (int)(((long long)patchw * spr->scale) >> FRACBITS);                if (wpx < 1) wpx = 1;
    hpx = (int)(((long long)SHORT(patch->height) * spr->scale) >> FRACBITS);  if (hpx < 1) hpx = 1;
    return (long)wpx * hpx;
}

/* SATURN world-things-on-VDP1: emit the world sprites to the VDP1 prio-7 layer (platform
   sat_thing_hook) at the post-BSP kick, before the end-of-planes present.  vissprites are
   already projected (R_AddSprites ran during the BSP walk); drawsegs are complete too.  We
   compute each sprite's full-resolution screen rect (matching the software R_DrawVisSprite
   math: sprtopscreen for the top, spr->scale for the size) and the unclamped left edge
   x1full, then hand it to the platform.  Sprites the platform takes are marked so R_DrawMasked
   skips their software fill; fuzz/translated and platform-declined sprites stay software.
   1p: called by the platform kick (extern "C") after the walls flush -> direct emit.
   VDP1-split: called PER VIEW from R_RenderViewPass (the walls flush only at the d_main.c
   post-loop kick) while THIS view's vissprites/drawsegs/window are live; the platform hook
   detects split and QUEUES the commands (bake immediate, tear-safe next-parity slots), then
   flushes them after the walls at the kick -- painter order stays walls -> things -> weapon. */
void R_EmitWorldThingsVDP1 (void)
{
    vissprite_t*	spr;
    extern int		sat_wall_skip;
    extern int		sat_things_hw;
    extern int		viewwindowx, viewwindowy;

    sat_things_emitted = 0;
    sat_things_occ = 0;
    if (!sat_thing_hook || !sat_wall_skip || !sat_things_hw || viewangleoffset)
	return;                                   /* path inactive this view -> software sprites.
	   (split: sat_wall_skip is 0 unless sat_split_vdp1, so software-split stays software) */
    if (vissprite_p == vissprites)
    { sat_things_emitted = 1; return; }           /* no sprites, but path active -> R_DrawMasked skips its (empty) loop */

    R_SortVisSprites ();                           /* scale order: far (low scale) .. near (high scale) */
    memset (sat_thing_vdp1, 0, (size_t)(vissprite_p - vissprites));
    memset (sat_thing_elig, 0, (size_t)(vissprite_p - vissprites));

    /* PASS 1: GRANT the sat_thing_cap slots to the distinct (lump,cmap) TEXTURES with the largest
       sprite, then mark eligible EVERY above-floor sprite using a granted texture (the platform
       cache shares one baked slot across all of them).  A horde of one enemy type -> a few bakes,
       the whole crowd off the software fill.  Sprites below the %-view floor (tiny pickups) stay
       software.  Occlusion is resolved per-sprite in pass 2. */
    {
	int           cap = sat_thing_cap;  if (cap > THING_TEX_TRACK) cap = THING_TEX_TRACK;
	long          area_floor = (long)viewwidth * viewheight * THING_MIN_SCREEN_PCT / 100;
	int           tk_lump[THING_TEX_TRACK];
	lighttable_t *tk_cmap[THING_TEX_TRACK];
	long          tk_area[THING_TEX_TRACK];        /* largest sprite area seen per distinct texture */
	char          tk_grant[THING_TEX_TRACK];
	int           ntk = 0, i, g;

	/* fold every above-floor sprite into its distinct-texture record (keep the max area) */
	for (spr = vsprsortedhead.next ; spr != &vsprsortedhead ; spr = spr->next)
	{
	    long area = R_ThingScreenArea (spr);
	    int  j;
	    if (area < area_floor) continue;             /* ineligible (-1) or below the floor -> software */
	    for (j = 0 ; j < ntk ; j++)
		if (tk_lump[j] == spr->patch && tk_cmap[j] == spr->colormap) break;
	    if (j < ntk) { if (area > tk_area[j]) tk_area[j] = area; }
	    else if (ntk < THING_TEX_TRACK)
	    { tk_lump[ntk] = spr->patch; tk_cmap[ntk] = spr->colormap; tk_area[ntk] = area; tk_grant[ntk] = 0; ntk++; }
	}
	/* grant the cap distinct textures with the largest sprite (biggest fill per slot) */
	for (g = 0 ; g < cap ; g++)
	{
	    int best = -1;
	    for (i = 0 ; i < ntk ; i++)
		if (!tk_grant[i] && (best < 0 || tk_area[i] > tk_area[best])) best = i;
	    if (best < 0) break;
	    tk_grant[best] = 1;
	}
	/* mark eligible the emax LARGEST sprites among the granted textures, where emax is the
	   platform's adaptive VDP1-raster budget (sat_thing_emit_cap, clamped to THING_EMIT_MAX).
	   Top-N insertion by area; the rest stay software (drawn correctly, no flicker).  A same-type
	   horde still shares one baked slot for its emitted few. */
	{
	    int  emax = sat_thing_emit_cap;
	    if (emax > THING_EMIT_MAX) emax = THING_EMIT_MAX;
	    if (emax < 0)              emax = 0;
	    int  em_idx[THING_EMIT_MAX];
	    long em_area[THING_EMIT_MAX];
	    int  nem = 0, k;
	    for (spr = vsprsortedhead.next ; emax > 0 && spr != &vsprsortedhead ; spr = spr->next)
	    {
		int  idx = spr - vissprites, j;
		long area = R_ThingScreenArea (spr);
		if (idx < 0 || idx >= MAXVISSPRITES) continue;
		if (area < area_floor) continue;
		for (j = 0 ; j < ntk ; j++)
		    if (tk_lump[j] == spr->patch && tk_cmap[j] == spr->colormap) break;
		if (j >= ntk || !tk_grant[j]) continue;   /* texture not granted -> software */
		if (nem < emax) {                         /* insert (ascending, em_area[0] = smallest) */
		    for (k = nem ; k > 0 && em_area[k-1] > area ; k--)
		    { em_area[k] = em_area[k-1]; em_idx[k] = em_idx[k-1]; }
		    em_area[k] = area; em_idx[k] = idx; nem++;
		} else if (area > em_area[0]) {           /* drop the smallest, insert */
		    for (k = 1 ; k < emax && em_area[k] < area ; k++)
		    { em_area[k-1] = em_area[k]; em_idx[k-1] = em_idx[k]; }
		    em_area[k-1] = area; em_idx[k-1] = idx;
		}
	    }
	    for (k = 0 ; k < nem ; k++) sat_thing_elig[em_idx[k]] = 1;
	}
    }

    /* PASS 2 (far -> near = PAINTER order): emit the eligible sprites (nearer draws over farther). */
    for (spr = vsprsortedhead.next ; spr != &vsprsortedhead ; spr = spr->next)
    {
	int	idx = spr - vissprites;
	patch_t	*patch;
	int	patchw, x1full, wpx, ytop, hpx, ybot, x0s, x1s, y0s, y1s;
	int	cx0, cy0, cx1, cy1, xx, vx1, vx2, vtop, vbot;

	if (idx < 0 || idx >= MAXVISSPRITES)   continue;
	if (!sat_thing_elig[idx])              continue;   /* not among the nearest sat_thing_cap -> software */

	patch  = W_CacheLumpNum (spr->patch+firstspritelump, PU_CACHE);
	patchw = spritewidth[spr->patch] >> FRACBITS;
	if (patchw < 1) continue;

	/* unclamped left screen edge: invert startfrac = xiscale*(x1 - x1full) (see R_ProjectSprite).
	   non-flip: frac 0 at the left edge; flip: frac (patchw<<FRACBITS) at the left edge. */
	if (spr->xiscale >= 0)
	    x1full = spr->x1 - (int)(spr->startfrac / spr->xiscale);
	else
	    x1full = spr->x1 - (int)((((fixed_t)patchw << FRACBITS) - spr->startfrac) / (-spr->xiscale));

	wpx  = (int)(((long long)patchw * spr->scale) >> FRACBITS);          /* detailshift baked into spr->scale */
	if (wpx < 1) wpx = 1;
	ytop = (centeryfrac - FixedMul (spr->texturemid, spr->scale)) >> FRACBITS;   /* == software sprtopscreen */
	hpx  = (int)(((long long)SHORT(patch->height) * spr->scale) >> FRACBITS);
	if (hpx < 1) hpx = 1;

	ybot = ytop + hpx - 1;
	x0s = (x1full << detailshift) + viewwindowx;
	x1s = (((x1full + wpx) << detailshift) - 1) + viewwindowx;
	y0s = ytop + viewwindowy;
	y1s = ybot + viewwindowy;

	/* OCCLUSION: per-column clip vs the drawsegs (no draw/no masked-seg render), reduced to the
	   visible bounding box.  A nearer wall/floor edge shrinks the box; a fully-hidden sprite gives
	   an empty box -> skip it entirely (correct = invisible).  The box is EXACT for the common
	   single vertical/horizontal cut; a jagged (diagonal-wall) cut over-shows its bbox sliver. */
	R_ClipSprite (spr, 0);
	vx1 = 32767; vx2 = -1; vtop = 32767; vbot = -1;
	for (xx = spr->x1 ; xx <= spr->x2 ; xx++)
	{
	    int ct = cliptop[xx] + 1;  if (ct < ytop) ct = ytop;
	    int cb = clipbot[xx] - 1;  if (cb > ybot) cb = ybot;
	    if (ct > cb) continue;                          /* nothing of the sprite visible in this column */
	    if (xx < vx1) vx1 = xx;   if (xx > vx2) vx2 = xx;
	    if (ct < vtop) vtop = ct; if (cb > vbot) vbot = cb;
	}
	if (vx2 < vx1) { sat_thing_vdp1[idx] = 1; sat_things_occ++; continue; }   /* fully occluded -> hidden, skip software too */
	cx0 = (vx1 << detailshift) + viewwindowx;
	cx1 = (((vx2 + 1) << detailshift) - 1) + viewwindowx;
	cy0 = vtop + viewwindowy;
	cy1 = vbot + viewwindowy;

	if (sat_thing_hook (patch, spr->patch, spr->colormap,
			    x0s, y0s, x1s, y1s, cx0, cy0, cx1, cy1, (int)(spr->xiscale < 0)))
	    sat_thing_vdp1[idx] = 1;
    }
    sat_things_emitted = 1;
}


void R_DrawMasked (void)
{
    vissprite_t*	spr;
    drawseg_t*		ds;
    extern void RP_DispatchMasked(int x0, int x1);
    extern void RP_WaitMasked(void);
    int masked_split = 0;

    R_SortVisSprites ();

    if (vissprite_p > vissprites)
    {
	/* SATURN masked-by-half: dispatch the slave to draw the RIGHT-half vissprites while the
	   master draws the LEFT half (g_mask_x1).  Masked walls + psprites stay on the master,
	   full width, after the wait. */
	/* SATURN world-things-on-VDP1: when the kick emitted the sprites to VDP1, the masked-by-half
	   slave split is off (most sprites are skipped below, and the slave path does not honour the
	   per-sprite marks) -- the few software-fallback sprites draw full-width on the master. */
	if (sat_masked_parallel && !sat_things_emitted)
	{
	    int half = viewwidth >> 1;
	    /* pre-cache every sprite patch on the master so the slave's W_CacheLumpNum only ever
	       finds them already cached -> no concurrent zone alloc (which would race the heap
	       off-cart / CD-streaming).  Cheap: a cached lump is just a pointer return. */
	    for (spr = vsprsortedhead.next ; spr != &vsprsortedhead ; spr=spr->next)
		W_CacheLumpNum (spr->patch+firstspritelump, PU_CACHE);
	    masked_split = 1;
	    RP_DispatchMasked (half, viewwidth);   /* slave: vissprites in [half, viewwidth) */
	    g_mask_x1 = half;                       /* master: vissprites in [0, half)       */
	}
	// draw all vissprites back to front
	for (spr = vsprsortedhead.next ;
	     spr != &vsprsortedhead ;
	     spr=spr->next)
	{
	    /* SATURN: skip the sprites the platform already drew on the VDP1 prio-7 layer. */
	    if (sat_things_emitted)
	    {
		int idx = spr - vissprites;
		if (idx >= 0 && idx < MAXVISSPRITES && sat_thing_vdp1[idx])
		    continue;
	    }
	    R_DrawSprite (spr);
	}
	if (masked_split)
	{
	    g_mask_x1 = 32767;       /* reset to full width for the walls + psprites */
	    RP_WaitMasked ();        /* the slave's right half is done */
	}
    }
    
    // render any remaining masked mid textures
    for (ds=ds_p-1 ; ds >= drawsegs ; ds--)
	if (ds->maskedtexturecol)
	    R_RenderMaskedSegRange (ds, ds->x1, ds->x2);
    
    // draw the psprites on top of everything
    //  but does not draw on side views
    /* SATURN: draw the weapon in SOFTWARE unless the VDP1 weapon path will actually emit it -- which
       is exactly when this frame/view uses VDP1 walls (sat_wall_skip).  So M0 software mode (and a
       software-wall split) keep the SOFTWARE weapon -> M0 stays a clean pure-software reference; the
       VDP1 modes (M1-M4, VDP1 split) get the VDP1 weapon.  sat_psprite_early keeps DoomJo/off on the
       software path. */
    {
	extern int sat_wall_skip;
	int vdp1_will_emit = sat_psprite_early && sat_wall_skip;
	if (!viewangleoffset && !vdp1_will_emit)
	    R_DrawPlayerSprites ();
    }
}



