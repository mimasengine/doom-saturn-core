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
//	Archiving: SaveGame I/O.
//	Thinker, Ticker.
//


#include "z_zone.h"
#include "p_local.h"
#include "r_parallel.h"   /* SATURN: RP_ThinkBegin/End -- the game-tic breakdown */

#include "doomstat.h"


int	leveltime;

//
// THINKERS
// All thinkers should be allocated by Z_Malloc
// so they can be operated on uniformly.
// The actual structures will vary in size,
// but the first element must be thinker_t.
//



// Both the head and tail of the thinker list.
thinker_t	thinkercap;


//
// P_InitThinkers
//
void P_InitThinkers (void)
{
    thinkercap.prev = thinkercap.next  = &thinkercap;
}




//
// P_AddThinker
// Adds a new thinker at the end of the list.
//
void P_AddThinker (thinker_t* thinker)
{
    thinkercap.prev->next = thinker;
    thinker->next = &thinkercap;
    thinker->prev = thinkercap.prev;
    thinkercap.prev = thinker;
}



//
// P_RemoveThinker
// Deallocation is lazy -- it will not actually be freed
// until its thinking turn comes up.
//
void P_RemoveThinker (thinker_t* thinker)
{
  // FIXME: NOP.
  thinker->function.acv = (actionf_v)(-1);
}



//
// P_AllocateThinker
// Allocates memory and adds a new thinker at the end of the list.
//
void P_AllocateThinker (thinker_t*	thinker)
{
}



//
// P_RunThinkers
//
void P_RunThinkers (void)
{
    thinker_t*	currentthinker;

    /* SATURN 2026-08-16: the thinker half of the game-tic split (row 24 `TIC`).  See r_parallel.c
       -- on hardware `T` is ~40 % of the frame and nothing inside it had ever been timed. */
    RP_ThinkBegin ();

    currentthinker = thinkercap.next;
    while (currentthinker != &thinkercap)
    {
	if ( currentthinker->function.acv == (actionf_v)(-1) )
	{
	    // time to remove it
	    currentthinker->next->prev = currentthinker->prev;
	    currentthinker->prev->next = currentthinker->next;
	    Z_Free (currentthinker);
	}
	else
	{
	    /* SATURN 2026-08-17 (row 23 `THK`): bill the MOBJ thinkers separately from the sector
	       ones -- `th - mo` is then doors/platforms/lights, `mo` is the actor world. */
	    if (currentthinker->function.acp1)
	    {
		if (currentthinker->function.acp1 == (actionf_p1)P_MobjThinker)
		{
		    RP_ThkMobjBegin ();
		    currentthinker->function.acp1 (currentthinker);
		    RP_ThkMobjEnd ();
		}
		else
		    currentthinker->function.acp1 (currentthinker);
	    }
	}
	currentthinker = currentthinker->next;
    }

    RP_ThinkEnd ();
}



//
// SATURN test cheats (pad chord, src/dg_saturn.cxx R+Down).  The Saturn has no keyboard, so the
// classic IDDQD/IDCLIP string entry (st_stuff.c) is unreachable; instead the platform cycles a
// desired state here and P_Ticker RE-APPLIES it every tic to all local players, so it survives
// level changes, player reborns and the E1M8 super-damage floor (p_spec.c case 11 clears
// CF_GODMODE).  0 = off, 1 = god (all damage < 1000 ignored), 2 = god + noclip.
//   DoomJo-safe: SAT_ApplyCheats is inert until the platform first calls SAT_CycleCheat
//   (sat_cheat_engaged latch), so a port that never wires the chord (DoomJo) keeps its own
//   keyboard cheats untouched -- we only OWN the two bits once the Saturn chord is used.
int		sat_cheat_want = 0;
static int	sat_cheat_engaged = 0;

void SAT_ApplyCheats (void)
{
    int i;
    if (!sat_cheat_engaged)
	return;
    for (i = 0 ; i < MAXPLAYERS ; i++)
    {
	if (!playeringame[i])
	    continue;
	if (sat_cheat_want >= 1) players[i].cheats |=  CF_GODMODE;
	else                     players[i].cheats &= ~CF_GODMODE;
	if (sat_cheat_want >= 2) players[i].cheats |=  CF_NOCLIP;
	else                     players[i].cheats &= ~CF_NOCLIP;
    }
}

void SAT_CycleCheat (void)
{
    sat_cheat_engaged = 1;
    sat_cheat_want = (sat_cheat_want + 1) % 3;
    SAT_ApplyCheats ();
    players[consoleplayer].message = (sat_cheat_want == 2) ? (char *)"GOD + NOCLIP" :
				     (sat_cheat_want == 1) ? (char *)"GOD MODE"     :
							     (char *)"MORTAL AGAIN";
}

//
// P_Ticker
//

void P_Ticker (void)
{
    int		i;

    // run the tic
    if (paused)
	return;
		
    // pause if in menu and at least one tic has been run
    if ( !netgame
	 && menuactive
	 && !demoplayback
	 && players[consoleplayer].viewz != 1)
    {
	return;
    }
    
		
    SAT_ApplyCheats ();		// SATURN: re-establish god/noclip before player think + damage

    for (i=0 ; i<MAXPLAYERS ; i++)
	if (playeringame[i])
	    P_PlayerThink (&players[i]);

    P_RunThinkers ();
    P_UpdateSpecials ();
    P_RespawnSpecials ();

    // for par times
    leveltime++;	
}
