/*
** DoomSRL -- dual-SH2 renderer back end (see r_parallel.cxx).
*/
#ifndef R_PARALLEL_H
#define R_PARALLEL_H

/* Draw-command queue, carved from the top of low work RAM.
   DG_ZoneBase (dg_saturn.cxx) shrinks Doom's zone heap accordingly. */
#define RP_CMD_BUF_ADDR  0x002D8000
#define RP_CMD_BUF_SIZE  0x00028000   /* 160KB = 5120 commands of 32 bytes */

/* Called from R_RenderPlayerView (r_main.c). */
void RP_BeginFrame(void);
void RP_MarkBSPDone(void);   /* after the BSP walk, before R_DrawPlanes (profiler) */
void RP_BeginMasked(void);
void RP_EndFrame(void);

/* SATURN PERF 2.4 Stage 1 (profiler): R_StoreWallRange (r_segs.c) brackets its
   work with these so the profiler can split B (the BSP walk) into pure BSP
   traversal vs wall-prep -- the number that bounds how much offloading wall-prep
   to the slave (2.4) could buy.  A bare empty call unless RP_PROF; safe on both
   ports (the shared core compiles this on GCC 9.3 too). */
void RP_WallPrepEnter(void);
void RP_WallPrepLeave(void);

/* Non-zero once the slave SH-2 has wedged and the renderer fell back to serial
   (master-only) drawing.  r_segs.c reads it so the Potato-walls generation skip
   stays off on the serial path (where R_DrawColumn would deref the skipped
   dc_source).  Essentially always 0 on current hardware (slave reliable). */
extern int rp_disabled;

#endif
