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



/* SATURN 2026-08-21 -- THE TIC GOVERNOR, part 2 (owner: "fais 1 et 2").  Adaptive decimation of
   FAR monsters: when the measured per-tic thinker cost (sat_think_last10, r_parallel.c) runs
   over budget, live monsters far from EVERY player think every 2nd tic (D1), then every 4th
   (D2).  Deliberately NOT compensated in speed: a far monster walking/attacking at half rate is
   the least visible artifact there is (nobody clocks a 6-px sprite's gait), where doubled steps
   would visibly teleport.  Exemptions keep it honest where it could be seen or felt: players,
   corpses (parked anyway), SKULLFLY (a charging soul must not stutter), MISSILE-flagged
   shootables, JUSTHIT (a monster that was just shot retaliates at full rate), and anything
   within SAT_DECIM_DIST of any live player.  The per-mobj phase stagger spreads the skipped
   population across tics so the load stays flat.  sat_tic_decim is the ladder rung (0/1/2),
   THK row `dc`. */
#define SAT_DECIM_DIST  (1536 * FRACUNIT)   /* ~6 px tall sprite at 320w -- decimation invisible past this */
int sat_tic_decim = 0;        /* current rung: 0 = full rate, 1 = far think 1/2, 2 = far think 1/4 */
int sat_tic_decim_auto = 1;   /* the measured law below drives the rung (no pad chord yet) */
int sat_tic_parked = 0;       /* running count of parked (unlinked, kept) corpse/prop thinkers */

static int sat_mobj_decim_skip (mobj_t* mo)
{
    int i;
    unsigned mask = (sat_tic_decim >= 2) ? 3u : 1u;
    if (!(mo->flags & MF_SHOOTABLE) || mo->player)
	return 0;
    if (mo->flags & (MF_SKULLFLY | MF_MISSILE | MF_JUSTHIT | MF_CORPSE))
	return 0;
    if (mo->health <= 0)
	return 0;
    /* phase first (cheap): this mobj's turn to think this tic? */
    if (((((unsigned)(long)mo) >> 4) + (unsigned)gametic) % (mask + 1u) == 0)
	return 0;
    /* near ANY player -> never decimate (the only per-monster cost: <= 4 approx-distances) */
    for (i = 0; i < MAXPLAYERS; i++)
	if (playeringame[i] && players[i].mo)
	{
	    if (P_AproxDistance (mo->x - players[i].mo->x,
				 mo->y - players[i].mo->y) < SAT_DECIM_DIST)
		return 0;
	}
    return 1;
}

//
// P_RunThinkers
//
void P_RunThinkers (void)
{
    thinker_t*	currentthinker;
    int		nshoot = 0;	/* SATURN 2026-08-21: LIVE SHOOTABLE count -> sight-cache governor.
				   ⚠ Round 2 (owner: "300 me semble gigantesque -- c'est le nombre
				   d'ennemis ?"): round 1 counted ALL mobj thinkers (corpses, items,
				   decorations, projectiles included) with thresholds read off the
				   overlay THK `n` -- which is a PER-FRAME sum, inflated by the 2-3
				   tics each frame runs.  Now: MF_SHOOTABLE only = live monsters +
				   barrels + players (death clears the flag), the population the
				   sight bill actually scales with (A_Chase/P_LookForPlayers). */

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
	    if (!P_MobjSlabFree (currentthinker))
		Z_Free (currentthinker);
	}
	/* SATURN 2026-08-21 -- THE TIC GOVERNOR, part 1: PARKED thinker (-2, set by p_mobj.c on a
	   settled corpse/prop).  Unlink like a removal but KEEP the memory: the mobj stays in the
	   sector/blockmap lists, so it still renders, still rides moving floors (P_ThingHeightClip
	   is sector-driven), the Arch-Vile still finds it (blockmap search) -- it just stops
	   burning a list walk + a no-op P_MobjThinker call every tic.  A_VileChase re-links it on
	   raise (p_enemy.c).  ⚠ If SAVEGAMES ever land: P_ArchiveThinkers only walks this list --
	   parked mobjs must be re-linked (or archived via sector lists) before saving. */
	else if ( currentthinker->function.acv == (actionf_v)(-2) )
	{
	    currentthinker->next->prev = currentthinker->prev;
	    currentthinker->prev->next = currentthinker->next;
	    sat_tic_parked++;
	}
	else
	{
	    /* SATURN 2026-08-17 (row 23 `THK`): bill the MOBJ thinkers separately from the sector
	       ones -- `th - mo` is then doors/platforms/lights, `mo` is the actor world. */
	    if (currentthinker->function.acp1)
	    {
		if (currentthinker->function.acp1 == (actionf_p1)P_MobjThinker)
		{
		    /* thinker_t is mobj_t's first member (vanilla layout) -> the cast is exact */
		    mobj_t* mo = (mobj_t *)currentthinker;
		    if (mo->flags & MF_SHOOTABLE)
			nshoot++;		/* counted BEFORE any decimation skip: still alive */
		    if (!(sat_tic_decim && sat_mobj_decim_skip (mo)))
		    {
			RP_ThkMobjBegin ();
			currentthinker->function.acp1 (currentthinker);
			RP_ThkMobjEnd ();
		    }
		}
		else
		{
		    /* SATURN 2026-08-18 (row 24 `sc`): the SECTOR thinkers -- doors, platforms,
		       lights.  With this, `th - mo - sc` is the list walk itself plus the Z_Free
		       of removed thinkers, and nothing in P_RunThinkers is unnamed any more. */
		    RP_ThkSectBegin ();
		    currentthinker->function.acp1 (currentthinker);
		    RP_ThkSectEnd ();
		}
	    }
	}
	currentthinker = currentthinker->next;
    }

    RP_ThinkEnd ();

    /* SATURN 2026-08-21 (owner: "SIGHT_CACHE_TICS pourrait être décidé par le gouverneur en
       fonction du nombre d'ennemis ?  s'il y en a plein... ça se remarque moins"): the sight-cache
       window follows the LIVE SHOOTABLE population.  The two curves move together by construction
       -- the sight bill is (awake lookers x walks) so it grows with the crowd, and the
       per-monster staleness is least visible exactly when the crowd is large.
       Thresholds (round 2): shareware UV maps carry ~50-90 monsters, crowded Doom II/TNT maps
       150-400+ -- so 4 below 120 live shootables (shareware never leaves the shipped default),
       8 from 120, 16 from 300; down-edges 100/260 (hysteresis bands, no boundary flap).
       These are calibration STARTING POINTS -- the console falsifier is TIC `ca` stepping up on
       a crowded TNT map while `s` (sight ms) drops and the AI still feels right.
       This is a dedicated tic-side law, NOT the render governor: that one is per-view/fill-
       calibrated (and blind in split) -- wiring an AI-latency knob to render debt would degrade
       the AI in scenes where the sight bill is small.  sat_sight_cache_auto = 1 is the boot
       default; pad L+A cycles auto -> 4 -> 8 -> 16 (TIC `ca`, `a` suffix = auto). */
    {
	extern int sat_sight_cache_auto, sat_sight_cache_tics;
	if (sat_sight_cache_auto)
	{
	    int t = sat_sight_cache_tics;
	    switch (t)
	    {
	      case 4:  if (nshoot >= 120) t = 8;                                 break;
	      case 8:  if (nshoot >= 300) t = 16; else if (nshoot <= 100) t = 4; break;
	      default: if (nshoot <= 260) t = 8;                                 break;
	    }
	    sat_sight_cache_tics = t;
	}
    }

    /* SATURN 2026-08-21 -- the DECIMATION LAW (governor part 2): the same integrator shape as
       the render governor (r_parallel.c), on the same discipline -- ONE signed accumulator, no
       cross-reset, asymmetric credit -- but fed by the MEASURED per-tic thinker cost
       (sat_think_last10; RP_ThinkEnd just ran, so this reads THIS tic).  Constants are console
       STARTING POINTS from the owner's MAP20 videos (calm ~5 ms/tic, carnage 15-25; Ymir reads
       8-14 ms a FRAME and must never calibrate this -- ymir-not-a-perf-oracle): target 12 ms,
       band +/-3, fire at +120 ms integrated (~0.5-1 s of carnage), release at -240 (~3 s of
       calm).  Falsifier on console: row 23 `mo` drops ~a third when `dc` steps to 1 with the
       eye seeing nothing; `dc` pumping 0<->1 every second = band too tight, widen it. */
#define TICGOV_TARGET10  120
#define TICGOV_BAND10     30
#define TICGOV_FIRE10   1200
#define TICGOV_REL10   (-2400)
    /* ⚠ DEMO GATE: decimation DIVERGES from the vanilla sim (skipped thinks change monster
       behaviour), so it must never run under a recorded demo -- the attract loop would desync
       into nonsense on any demo heavy enough to fire the law.  (The corpse PARKING needs no
       gate: the parked call was a pure no-op, the sim is bit-identical.)  Drop the rung
       immediately on entering playback; the law stays armed for real play. */
    if (demoplayback)
	sat_tic_decim = 0;
    else if (sat_tic_decim_auto)
    {
	extern int sat_think_last10;
	static int tdebt = 0;
	int err = sat_think_last10 - TICGOV_TARGET10;
	if (err > TICGOV_BAND10)        tdebt += err - TICGOV_BAND10;
	else if (err < -TICGOV_BAND10)  tdebt += (err + TICGOV_BAND10) / 2;   /* credit at half rate */
	/* the threshold reset doubles as the anti-windup rail: at the top rung a re-fire is a
	   no-op that re-arms the climb, at the bottom a re-release re-arms the credit -- the
	   integral never runs away in either direction. */
	if (tdebt >= TICGOV_FIRE10) { if (sat_tic_decim < 2) sat_tic_decim++; tdebt = 0; }
	else if (tdebt <= TICGOV_REL10) { if (sat_tic_decim > 0) sat_tic_decim--; tdebt = 0; }
    }
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
