/*
** Mimas -- dual-SH2 renderer back end (see r_parallel.cxx).
*/
#ifndef R_PARALLEL_H
#define R_PARALLEL_H

/* Draw-command queue, carved from the top of low work RAM.
   DG_ZoneBase (dg_saturn.cxx) shrinks Doom's zone heap accordingly.
   RP_CMD_BUF_SIZE is -D-overridable per port: Mimas shrinks it (walls go to
   VDP1, so the slave's column-command traffic is low) to GROW the streaming zone
   -- the buffer is a ring that auto-flushes at RP_MAX (r_parallel.c), so a
   smaller buffer only flushes more often on command-heavy frames, never drops a
   command.  DoomJo keeps the 160KB default (no VDP1 walls => higher traffic).
   ADDR is DERIVED from SIZE so the buffer always sits flush against the top of
   low work RAM (end = 0x00300000) and the grown zone can never overlap it
   (default size keeps ADDR at the historical 0x002D8000). */
#ifndef RP_CMD_BUF_SIZE
#define RP_CMD_BUF_SIZE  0x00028000   /* 160KB = 5120 commands of 32 bytes */
#endif
#define RP_CMD_BUF_ADDR  (0x00300000 - RP_CMD_BUF_SIZE)

/* Called from R_RenderPlayerView (r_main.c). */
void RP_BeginFrame(void);
void RP_MarkBSPDone(void);   /* after the BSP walk, before R_DrawPlanes (profiler) */
/* SATURN 2026-08-06: SPLIT `P` INTO ITS PARTS.  Row-2 `P` is NOT "the planes" -- it is everything
   between RP_MarkBSPDone and RP_BeginMasked, and the owner was right to challenge a 191.9 ms `P`
   on a frame showing one floor, sky and distant walls.  That interval contains FIVE things, two of
   which are WAITS: the VDP1 wall kick (`sat_walls_done_hook`, which also runs R_DrawPlayerSprites
   -> weapon projection + its VDP1 texture bake, i.e. a possible disc read), 2x NetUpdate, the
   lead-fill slave dispatch + RP_LeadJoin (an FRT-bounded spin on the 2nd SH-2), R_DrawPlanes
   itself, and 2 canaries.  RP_MarkP stamps the boundaries so row 20 `PSP` can name the culprit.
   Slots: 0 = after the VDP1 kick, 1 = just before R_DrawPlanes, 2 = just after R_DrawPlanes. */
void RP_MarkP(int slot);
void RP_BeginMasked(void);
void RP_EndFrame(void);

/* SATURN PERF 2.4 Stage 1 (profiler): R_StoreWallRange (r_segs.c) brackets its
   work with these so the profiler can split B (the BSP walk) into pure BSP
   traversal vs wall-prep -- the number that bounds how much offloading wall-prep
   to the slave (2.4) could buy.  A bare empty call unless RP_PROF; safe on both
   ports (the shared core compiles this on GCC 9.3 too). */
void RP_WallPrepEnter(void);
void RP_WallPrepLeave(void);

/* SATURN PERF (Phase-0a fine split, profiler): localise WHERE Bp and P spend
   their time, to rank the REC-reduction levers.  All per-seg / per-visplane (not
   per-column / per-span), so the overhead is negligible even when on; bare empty
   calls unless RP_PROF.  Safe on both ports (pure C, GCC 9.3).
     Bp  = setup (R_StoreWallRange per-seg trig/scale) + loop (R_RenderSegLoop
           per-column clip/visplane-mark/texturecolumn).  SegLoop brackets the loop.
     P   = alloc (W_CacheLumpNum/Release per visplane) + makespans (the R_MakeSpans
           walk + R_MapPlane span math) + other (sort/sky/control). */
void RP_SegLoopEnter(void);
void RP_SegRoutMark(void);   /* SATURN: splits SegLoop into routing preamble | per-column loop */
void RP_SegLoopLeave(void);
void RP_FlatCacheEnter(void);
void RP_FlatCacheLeave(void);
void RP_MakeSpansEnter(void);
void RP_MakeSpansLeave(void);

/* SATURN PERF (RBG0 candidate sizing, profiler): R_DrawPlanes hands each regular
   (non-sky) visplane here.  The hook tallies its span pixels and tracks the
   largest single (picnum,height) flat -- the VDP2 RBG0 offload candidate -- so the
   overlay can report what share of the floor/ceiling FILL that one flat owns
   (decides whether deporting the biggest flat to RBG0 is worth the subsystem).
   The pixel walk lives INSIDE the hook so it is fully compiled out unless RP_PROF
   (zero overhead off).  Visplanes arrive picnum-sorted (R_DrawPlanes vpsort) so a
   same-key run is contiguous and groups in one linear pass.  Pure C, DoomJo-safe. */
void RP_PlanePixels(int picnum, int height, int minx, int maxx,
                    const unsigned char *top, const unsigned char *bottom);

/* Non-zero once the slave SH-2 has wedged and the renderer fell back to serial
   (master-only) drawing.  r_segs.c reads it so the Potato-walls generation skip
   stays off on the serial path (where R_DrawColumn would deref the skipped
   dc_source).  Essentially always 0 on current hardware (slave reliable). */
extern int rp_disabled;

/* SATURN (Mimas platform only): rewind the SGL slave work pointers (GBR+72/+68)
   to their captured base, exactly as slSynch would.  Normally called once per
   frame from rp_restart; exposed so the platform layer (dg_saturn.cxx dual-CPU
   blit) can rewind before a 2nd slSlaveFunc/frame and avoid the work-pointer
   creep that caused the ~2-min freeze.  Safe no-op for DoomJo (it never calls it;
   declaration only -> zero impact). */
void rp_sgl_workptr_reset(void);

/* SATURN 2026-08-16 -- GAME-TIC BREAKDOWN.  On hardware row-1 `T` is 69-83 ms of a 181-222 ms
   frame (~40 %), against 8-14 ms for the same build on Ymir -- so the biggest single cost in the
   frame has been invisible for the whole renderer hunt.  These bracket the two halves worth
   naming: P_RunThinkers (p_tick.c, once per tic) and P_CheckSight's full BSP walk (p_sight.c,
   accumulated per call).  Accumulators are RAW FRT TICKS; the platform converts to ms and resets
   them, because only it knows the window's frame count.  DoomJo-safe: plain C, and the port simply
   gets two extra timer reads per tic. */
void RP_TicBegin(void);
void RP_TicEnd(void);
void RP_ThinkBegin(void);
void RP_ThinkEnd(void);
void RP_SightBegin(void);
void RP_SightEnd(void);
extern unsigned int sat_tic_total_frt;   /* TryRunTics,    cumulative FRT ticks -- row 24's own `T` */
extern unsigned int sat_tic_think_frt;   /* P_RunThinkers, cumulative FRT ticks */
extern unsigned int sat_tic_sight_frt;   /* P_CrossBSPNode, cumulative FRT ticks */
extern unsigned int sat_tic_runs;        /* P_RunThinkers CALLS = tics run; divides `th` into cost x count */
/* SATURN 2026-08-17 -- row 23 `THK`: the inside of `th` (see the note in r_parallel.c). */
void RP_ThkMobjBegin(void);
void RP_ThkMobjEnd(void);
void RP_ThkMoveBegin(void);
void RP_ThkMoveEnd(void);
extern unsigned int sat_thk_mobj_frt;    /* P_MobjThinker, cumulative FRT ticks           */
extern unsigned int sat_thk_move_frt;    /* P_CheckPosition/P_TryMove inside it (SUBSET)  */
extern unsigned int sat_thk_n;           /* thinkers RUN -- "many" vs "expensive"         */
/* round 2: carve the ~69 ms of `mo` that `mv` and `s` do not explain (see r_parallel.c). */
void RP_ThkPathBegin(void);
void RP_ThkPathEnd(void);
void RP_ThkSpawnBegin(void);
void RP_ThkSpawnEnd(void);
extern unsigned int sat_thk_path_frt;    /* P_PathTraverse -- hitscan shots, NOT part of `mv`      */
extern unsigned int sat_thk_spawn_frt;   /* P_SpawnMobj (the Z_Malloc): puffs, blood, drops        */
extern unsigned int sat_thk_spawn_n;     /* ...and how many: expensive vs merely frequent          */
extern unsigned int sat_tic_avail;       /* d_loop.c: tics TryRunTics ELECTED to run -- `a` vs `x` says
                                            whether the lost tics were never built or built-then-skipped */
extern unsigned int sat_tic_built;       /* d_loop.c: tics NetUpdate WANTED (`newtics`) -- row 24 `b` */
extern int sat_thing_masked_cut;         /* r_things.c: sprites sent back to software for a grate -- `mk` */

/* SATURN 2026-08-16 -- DRAWSEG BUDGET (defined in r_segs.c, driven by the governor in r_parallel.c,
   spent in R_StoreWallRange, reset per view in RP_BeginFrame).  Bounds the NUMBER of textured segs,
   which is what `Bp` actually scales with (`ds118` at `Bp110,8`); the area rung bounds their SIZE.
   Past the budget a seg draws FLAT -- never skipped, or solidsegs would leave a see-through hole. */
/* SATURN 2026-08-17 -- row 14 `SEG`: the COUNTS that size `lp` (see the note in r_parallel.c).
   Plain increments, reset per frame with the rest of the Phase-0a split. */
extern unsigned int prof_seg_cols, prof_seg_fill, prof_seg_px, prof_lead_px;

/* SATURN 2026-08-17 -- GOVERNOR ACTION COUNTERS.  One increment each time a governor rung actually
   CHANGES A PIXEL: `w` when a tier is flattened (area rung or drawseg budget), `l` when the
   lead-fill emits a span.  Free-running and GOVERNOR-OWNED -- deliberately NOT the overlay's
   `sb`/`L<n>` tallies, which the platform zeroes on its own window and would make the governor's
   delta read as "no action" at random.  See the inert-by-construction test in r_parallel.c. */
extern unsigned int sat_gov_act_w, sat_gov_act_l;

/* SATURN 2026-08-17 -- row 16 `GCS`: R_GetColumn split by CALL SITE (see the note in r_parallel.c).
   Callers stamp sat_gc_site; 1 = seg-loop tier, 2 = routing preamble, 3 = masked midtexture. */
extern int sat_gc_site;
extern unsigned int prof_gc_st[4], prof_gc_sn[4];

extern int sat_seg_budget;       /* textured segs allowed per view, 0 = unbounded */
extern int sat_seg_count;        /* segs stored so far this view                  */
extern int sat_seg_budget_cut;   /* ~1 s tally of segs flattened by the budget    */

#endif
