/*
** Shared dual-SH2 renderer back end (doom-saturn-core).
**
** Pure C, SDK-agnostic: the command queue, executors, sync protocol and
** cache-coherency rules are all hardware-level (SH-2 / SGL), so this exact
** file is compiled by BOTH ports (Mimas and DoomJo).  The only platform
** touch-points are:
**   - slSlaveFunc()  : SGL, linked by both ports
**   - dbg_print()     : a thin debug-overlay shim each port provides
**                      (SRL::Debug::Print on Mimas, native on DoomJo)
**   - cache purge    : direct CCR register write (same hardware op everywhere)
*/
#include <stdio.h>
#include <string.h>
#include "doomtype.h"
#include "doomdef.h"
#include "m_fixed.h"
#include "r_main.h"
#include "r_draw.h"
#include "r_state.h"
#include "r_parallel.h"

/* Platform-provided (SGL on both ports; dbg_print implemented per platform). */
extern void slSlaveFunc(void (*func)(void *), void *param);
extern void dbg_print(int x, int y, char *str);

/* SATURN PERF (1.4): -O3 on the executors was A/B-tested via the row-19 profiler
   and showed NO gain (EX ~neutral, slightly worse per-command: 61.9 -> 63.0us),
   consistent with I-cache bloat on the slave's 4KB cache.  Reverted -> default
   -O2.  Do not re-add without re-measuring. */

/* Per-frame slave-timing overlay (row 2).  Off by default (sprintf/frame). */
#define RP_DEBUG 0

/* SATURN DIAG: slave-side command-corruption counter (overlay row 15).  Detects
   the hardware-only stale-command-buffer read.  OFF now: zero corruption
   confirmed on hardware (CPU blit).  Re-enable if revisiting the SCU DMA. */
#define RP_CDIAG 0

/* SATURN PERF PROFILER (overlay row 19): clean FRT-based timing of the render's
   two halves -- RECORD (BSP+planes+sprites command generation, mono-CPU master,
   while the slave executes in the background) vs EXECUTE (rp_finish: master draws
   its half + waits for the slave).  ms, NTSC-calibrated (FRT = sysclk/128 ~=
   4.47us/tick -> ~224 ticks/ms).  Decides record-bound vs execute-bound.
   Off in shipped builds; flip to 1 to re-profile (e.g. for the SFX hunt).
   ON during the perf phase: slave confirmed reliable, W measured (~0 = master-
   bound during EX -> the lever is to give the slave more, not less). */
#define RP_PROF 1

#if RP_DEBUG
extern unsigned short sat_frt(void);
#define frt_now sat_frt
static unsigned short rp_t_begin, rp_t_rec, rp_t_fin;
extern unsigned short rp_frt_entry;
unsigned short rp_frt_entry;
#endif

extern byte *ylookup[];
extern int   columnofs[];
extern int   fuzzoffset[];
extern int   fuzzpos;
extern int   detailshift;
extern int   sat_lowres;          /* SATURN M7: packed-160 render -> the REC executors must PACK, not
                                     double to x<<1 (=[160,320) off-screen), see rp_exec / rp_exec_fuzz */
extern int   sat_potato_floors;   /* SATURN: solid-colour floors/ceilings (Potato) */
extern int   sat_potato_walls;    /* SATURN: solid-colour walls (opaque RP_COL only) */
extern int   sat_wall_color;      /* SATURN: current wall's dominant colour (r_segs) */
extern int   sat_wall_paint;      /* SATURN debug paint (r_data.c): bit1 = CPU walls flat red */
extern int   sat_wall_textured;   /* SATURN: keep this wall textured (special line) */
/* Potato: one FIXED texel of the 64x64 flat (centre = v32,u32 = 32*64+32) as the
   span's base colour.  Using a fixed texel (not the view-dependent span-start one)
   makes the whole flat a single colour that does NOT shift/rotate as the player
   turns; distance fog is still applied per span via the colormap.  (Walls use the
   per-texture dominant colour carried in the command's f3 field instead.) */
#define POTATO_TEXEL 2080
#define FUZZTABLE 50

/* ------------------------------------------------------------------ */
/* Command queue                                                       */
/* ------------------------------------------------------------------ */

enum { RP_COL, RP_TRANS, RP_FUZZ, RP_SPAN };

typedef struct
{
    unsigned char  type;
    unsigned char  unused;
    short          a;
    short          b;
    short          c;
    byte          *src;
    byte          *cmap;
    fixed_t        f1, f2, f3, f4;
} rp_cmd_t;

#define RP_CMDS  ((rp_cmd_t *)RP_CMD_BUF_ADDR)
#define RP_MAX   (RP_CMD_BUF_SIZE / (int)sizeof(rp_cmd_t))

typedef struct
{
    int ready;
    int masked_at;
    int total;
    int go_masked;
    int slave_opaque_done;
    int slave_masked_done;
    int slave_alive;
    int slave_execs;
    /* SATURN PERF 2.4 Stage 1 (profiler): the slave self-times its opaque phase
       on its OWN free-running counter.  opq_total = ticks from dispatch to
       slave_opaque_done; opq_draw = ticks actually spent inside the draw loops.
       Reported as a divider-independent ratio (idle% = (total-draw)/total) so the
       slave FRT's clock divider need not match the master's.  High idle% => the
       slave spends REC waiting for the master to produce commands => there is room
       to offload wall-prep onto it (2.4); low idle% => it is saturated drawing. */
    int slave_opq_total, slave_opq_draw;
    /* SATURN PERF (2026-07-08): MEASURED slave occupancy.  slave_busy = monotonic sum of every
       live slave-body duration, timed on the slave's OWN FRT forced to phi/128 (== the master
       divider SGL sets) so the master converts ticks->ms directly.  The master diffs it per frame
       in rp_p3_prof_show -> the TRUE busy% of MST, next to the DERIVED SLVi (which only assumes the
       slave is busy during P/M and is blind to intra-phase idle).  Single writer (slave), read via
       the uncached SYNC alias -> no cross-CPU race, no reset needed (master keeps its own snapshot). */
    int slave_busy;
    /* Same, but PLANE bodies only (tas/steal/static/rows) -> the master derives Pb = the slave's
       share of the plane phase P (=50% balanced, <50% master-heavy on a dominant flat).  slave_busy
       is the TOTAL (planes + masked-half + aux/wallprep); slave_pbusy is the plane subset. */
    int slave_pbusy;
    /* SATURN PERF 2.5 (two-pointers): self-balancing EX opaque drain.  The master
       draws parity-0 opaque FORWARD from 0 (m_pos = highest index it has drawn);
       the slave draws parity-0 opaque BACKWARD from mat-1 (s_pos = lowest it has
       drawn).  Each stops when it reaches the other's pointer -> the slower CPU
       just covers fewer commands, so W->0 by construction (no static split, no
       atomics).  Both live in this uncached SYNC struct so writes cross CPUs
       immediately.  The 0-or-1-command overlap at the crossing is harmless: Doom
       opaque has no overdraw, so a column drawn by both gets the identical pixel. */
    int m_pos;
    int s_pos;
    /* SATURN: Potato flags snapshot (bit0=floors, bit1=walls), published by the
       master each frame in uncached SYNC so the slave sees the SAME state -- the
       cached globals would otherwise lag a frame on a toggle (master writes
       write-back; slave reads stale RAM) -> for one frame the two CPUs disagree
       and you see half the columns textured, half solid. */
    int potato;
    /* SATURN DIAG: commands the SLAVE read as out-of-range.  The slave reads
       the command buffer from RAM; if the master writes it write-back (not
       write-through), the slave occasionally reads a not-yet-evicted (stale)
       line -> garbage command -> wrong odd-column pixels.  Invisible on
       coherent-memory emulators, so this counts it on real hardware.  The
       master reads its OWN cache and never sees the corruption, hence we must
       measure on the slave.  bad_* hold the first offending command. */
    int slave_bad;
    int bad_t, bad_a, bad_b, bad_c;
    /* SATURN DIAG: SH-2 cache-control-register readout (overlay row 16).
       master_ccr = master's CCR (SGL's config, the known-good "cache on" value).
       slave_ccr0 = slave's CCR on its first-ever dispatch, BEFORE we touch it
                    (its pristine state -- is the slave's cache even enabled?).
       slave_ccr1 = slave's CCR AFTER our `|= 0x02` (confirms what the bit did).
       Per SATURN_HARDWARE_REF: bit0 OD=operand-cache-disable, bit1 ID=
       instruction-cache-disable, bit2 TW=scratchpad, bit4 CP=purge.  The slave
       sets 0x02 with a wrong "WT=1" comment -> that's ID, crippling its I-cache. */
    int master_ccr, slave_ccr0, slave_ccr1;
    /* SATURN: slave opaque-loop guard.  The slave was wedging in its opaque
       for(;;) (od0, then al0 next frames -> parallel disabled).  The guard breaks
       the loop if it spins without progress (masked_at never becomes reachable),
       so the slave always returns and stays re-dispatchable.  slave_guard counts
       breaks (cumulative); g_mat/g_i/g_ready = what the slave saw when it bailed
       -> reveals WHY (e.g. g_mat=-1 means masked_at was never set this frame). */
    int slave_guard, slave_g_mat, slave_g_i, slave_g_ready;
} rp_sync_t;

static rp_sync_t rp_sync __attribute__((aligned(16)));
#define SYNC ((volatile rp_sync_t *)((unsigned int)&rp_sync | 0x20000000u))

static int  rec_count;
static int  rec_masked_at;
static int  in_masked;
static int  rp_active;
int         rp_disabled;   /* exposed: r_segs.c gates Potato-walls skip on it */
static int  rp_consec_timeouts = 0;   /* slave timeouts in a row; re-arm unless persistent */
int rp_timeout_count = 0;

#if RP_PROF
static unsigned short rp_frt(void);   /* fwd: slave self-timing in rp_slave_body */
/* P3 profiler state (defined early: the plane dispatch below references it).  Used when the
   parity renderer is OFF (rp_disabled / sat_plane_parallel), where the existing B/P/M/SLV rows
   are all gated on rp_active and go blank.  Phase marks sampled unconditionally; the slave
   plane-draw time is the slave's own FRT delta; p3_wait = master idle in RP_WaitPlanes. */
static unsigned short p3_t_begin, p3_t_bsp, p3_t_planes;
/* SATURN 2026-08-06: sub-brackets INSIDE `P` (see RP_MarkP in r_parallel.h).  p3_t_p[0..2] =
   after the VDP1 wall kick / before R_DrawPlanes / after R_DrawPlanes.  Published as tenths-ms
   for the row-20 `PSP` readout so a `P` spike can be attributed instead of assumed. */
static unsigned short p3_t_p[2];
unsigned int sat_p_kick10 = 0;   /* VDP1 wall kick + R_DrawPlayerSprites (weapon)               */
unsigned int sat_p_build10 = 0;  /* row 5 `Pv`: R_DrawPlanes entry -> dispatch (worklist build) */
/* SATURN 2026-08-25: row 4's FOUR Bp sub-terms, summed over the FRAME's views (the raw prof_*
   accumulators they come from are per-view).  Tenths of a ms.  Written by rp_p3_prof_show on the
   last view, read from dg_saturn's per-frame block at the end of DG_DrawFrame.
   hd+pr+lp+tl == row-2 `Bp` by construction.  (`wp` retired the same day: it WAS prof_wallprep,
   i.e. row-2 `Bp` restated, and it cost 7 of 40 columns to say so.) */
unsigned int sat_bps_pr10 = 0, sat_bps_lp10 = 0, sat_bps_hd10 = 0, sat_bps_tl10 = 0;
/* SATURN 2026-08-25: rows 14 (`SEG`) and 16 (`GCS`) summed over the FRAME's views -- same law,
   same site and same writer as sat_bps_* above.  RAW counts / RAW FRT ticks: dg_saturn keeps the
   clamping and the tick->ms conversion it already did on the per-view prof_* these replace. */
unsigned int sat_seg_cols_f = 0, sat_seg_fill_f = 0, sat_seg_px_f = 0, sat_lead_px_f = 0;
/* [!] SATURN 2026-08-26 -- row 14 `ov`: WHAT THE PROFILER ITSELF COSTS, frame-summed tenths-ms.
   rp_p3_prof_show is 788 lines with ~36 divisions and it runs ONCE PER VIEW -- four times a frame
   in 4p -- from RP_EndFrame, i.e. INSIDE the render, so its cost is charged to row-1 `R` and to
   each view's row-17 time.  Only TWO blocks inside it are gated on sat_dbg_overlay_mode == 0;
   everything else (the histograms, the peak folds, the per-view accumulators, the governor's
   input) runs in fps-only and overlay-off builds too.  Nobody has ever measured it.
   Bracketed from OUTSIDE the call so the probe cannot hide inside the thing it measures -- the
   exact mistake RP_PlanePixels was making inside `Pv` until this same day.  Published on the
   frame's LAST view, so what you read is the PREVIOUS frame's total (same law as row-19 `v`).
   ⚠ Read it before restructuring the function: a blanket early return is NOT available, because
   the LOD governor reads `p10` from in here and is not mode-gated (gsum_p, ~line 3010). */
unsigned int sat_prof_show10 = 0;
unsigned int sat_gc_st_f[4], sat_gc_sn_f[4];
/* (n / d / j RETIRED 2026-08-06 -- all three measured ~0 and are SETTLED NEGATIVE: NetUpdate and
   the canaries are free, RP_LeadJoin never waits, and R_DrawPlanes itself is 1 ms, i.e. THE PLANES
   ARE INNOCENT.  They were still computed and window-folded for a row that no longer prints them,
   which costs HWRAM -- and this config's MEASURED boot floor is 4.8..5.0 KB of TLSF pool. */
static unsigned short p3_wait_ticks;   /* master idle in RP_WaitPlanes (master FRT, reliable) */
/* NOTE: the slave's OWN FRT can't be used for a duration -- it's 16-bit and runs fast enough to
   wrap several times per frame, so any slave busy/period read is garbage (>100%).  Slave slack
   is derived from the master-FRT phase times instead: the slave works during P, idles in B+M. */
#endif

static void (*saved_col)(void);
static void (*saved_base)(void);
static void (*saved_fuzz)(void);
static void (*saved_trans)(void);
static void (*saved_span)(void);

/* ------------------------------------------------------------------ */
/* Executors                                                           */
/* ------------------------------------------------------------------ */

static void rp_exec_col(const rp_cmd_t *cm, const int *colofs)
{
    int           count = cm->c - cm->b + 1;
    byte         *dest;
    fixed_t       frac, step, step2, step3, step4, step5, step6, step7, step8;
    const byte   *src  = cm->src;
    const byte   *cmap = cm->cmap;

    if ((unsigned short)cm->a >= SCREENWIDTH  ||
        (unsigned short)cm->b >= SCREENHEIGHT ||
        (unsigned short)cm->c >= SCREENHEIGHT) return;
    if (count <= 0) return;

    /* SATURN PERF (2.2): index cm->src directly with the `& 127` wrap (composite
       wall columns are tiled at 128, exactly as vanilla R_DrawColumn).  The old
       per-column `memcpy(col_cache, src, 128)` copied a full 128 bytes to draw as
       few as ~10 pixels -- pure overhead on every column, and assumed 128-tall
       sources anyway.  d32xr's hand-asm column does zero source copy; same here. */
    dest  = ylookup[cm->b] + colofs[cm->a];
    /* SATURN Potato walls: a wall column (opaque, cm->unused==0) becomes one
       distance-shaded colour (a fixed texel of its source) -- vertical detail is
       lost but the per-column horizontal variation stays.  cm->unused==1 = a
       masked sprite column (also RP_COL): leave it textured. */
    if ((sat_potato_walls || (sat_wall_paint & 2)) && !cm->unused)
    {
        byte c = cmap[(unsigned char)cm->f3];   /* wall's dominant colour, light-shaded */
        do { *dest = c; dest += SCREENWIDTH; } while (--count);
        return;
    }
    step  = cm->f1;
    frac  = cm->f2 + (cm->b - centery) * step;
    step2 = step  + step;
    step3 = step2 + step;
    step4 = step2 + step2;
    step5 = step4 + step;
    step6 = step4 + step2;
    step7 = step4 + step3;
    step8 = step4 + step4;

    while (count >= 8)
    {
        dest[0]             = cmap[src[(frac)         >> FRACBITS & 127]];
        dest[SCREENWIDTH]   = cmap[src[(frac + step)  >> FRACBITS & 127]];
        dest[SCREENWIDTH*2] = cmap[src[(frac + step2) >> FRACBITS & 127]];
        dest[SCREENWIDTH*3] = cmap[src[(frac + step3) >> FRACBITS & 127]];
        dest[SCREENWIDTH*4] = cmap[src[(frac + step4) >> FRACBITS & 127]];
        dest[SCREENWIDTH*5] = cmap[src[(frac + step5) >> FRACBITS & 127]];
        dest[SCREENWIDTH*6] = cmap[src[(frac + step6) >> FRACBITS & 127]];
        dest[SCREENWIDTH*7] = cmap[src[(frac + step7) >> FRACBITS & 127]];
        dest  += SCREENWIDTH * 8;
        frac  += step8;
        count -= 8;
    }
    while (count >= 4)
    {
        dest[0]             = cmap[src[(frac)         >> FRACBITS & 127]];
        dest[SCREENWIDTH]   = cmap[src[(frac + step)  >> FRACBITS & 127]];
        dest[SCREENWIDTH*2] = cmap[src[(frac + step2) >> FRACBITS & 127]];
        dest[SCREENWIDTH*3] = cmap[src[(frac + step3) >> FRACBITS & 127]];
        dest  += SCREENWIDTH * 4;
        frac  += step4;
        count -= 4;
    }
    while (count > 0)
    {
        *dest = cmap[src[frac >> FRACBITS & 127]];
        dest += SCREENWIDTH;
        frac += step;
        count--;
    }
}

static void rp_exec_trans(const rp_cmd_t *cm, const int *colofs)
{
    int     count = cm->c - cm->b;
    byte   *dest;
    byte   *xlat = (byte *)cm->f3;
    fixed_t frac, step;

    if (count < 0) return;
    if ((unsigned short)cm->a >= SCREENWIDTH  ||
        (unsigned short)cm->b >= SCREENHEIGHT ||
        (unsigned short)cm->c >= SCREENHEIGHT) return;
    dest = ylookup[cm->b] + colofs[cm->a];
    step = cm->f1;
    frac = cm->f2 + (cm->b - centery) * step;
    do {
        *dest = cm->cmap[xlat[cm->src[frac >> FRACBITS]]];
        dest += SCREENWIDTH;
        frac += step;
    } while (count--);
}

static void rp_exec_span(const rp_cmd_t *cm, const int *colofs)
{
    unsigned int  position, step;
    byte         *dest;
    int           count;
    const byte   *src  = cm->src;
    const byte   *cmap = cm->cmap;

    position = (((unsigned int)cm->f1 << 10) & 0xffff0000)
             | (((unsigned int)cm->f2 >> 6)  & 0x0000ffff);
    step     = (((unsigned int)cm->f3 << 10) & 0xffff0000)
             | (((unsigned int)cm->f4 >> 6)  & 0x0000ffff);

    if ((unsigned short)cm->a >= SCREENHEIGHT ||
        (unsigned short)cm->b >= SCREENWIDTH  ||
        (unsigned short)cm->c >= SCREENWIDTH)  return;
    dest  = ylookup[cm->a] + colofs[cm->b];
    count = cm->c - cm->b + 1;

#define SPAN_PIX(pos) cmap[src[((pos) >> 26) | (((pos) >> 4) & 0x0fc0)]]
    if (sat_potato_floors)
    {
        /* SATURN Potato: flat-shade the floor/ceiling span -- one FIXED flat texel
           (POTATO_TEXEL), distance-shaded via cmap, memset across the span.  Much
           cheaper than the per-pixel textured fill; the fixed texel makes the whole
           flat a single colour that doesn't shift/rotate with the view, while the
           per-span cmap keeps distance fog. */
        byte c = cmap[src[POTATO_TEXEL]];
        memset(dest, c, (size_t)count);
        return;
    }
    while (count >= 8)
    {
        unsigned int p1=position+step,p2=p1+step,p3=p2+step,
                     p4=p3+step,p5=p4+step,p6=p5+step,p7=p6+step;
        dest[0]=SPAN_PIX(position); dest[1]=SPAN_PIX(p1); dest[2]=SPAN_PIX(p2);
        dest[3]=SPAN_PIX(p3);       dest[4]=SPAN_PIX(p4); dest[5]=SPAN_PIX(p5);
        dest[6]=SPAN_PIX(p6);       dest[7]=SPAN_PIX(p7);
        dest+=8; position=p7+step; count-=8;
    }
    while (count >= 4)
    {
        unsigned int p1=position+step,p2=p1+step,p3=p2+step;
        dest[0]=SPAN_PIX(position); dest[1]=SPAN_PIX(p1);
        dest[2]=SPAN_PIX(p2);       dest[3]=SPAN_PIX(p3);
        dest+=4; position=p3+step; count-=4;
    }
    while (count > 0) { *dest++=SPAN_PIX(position); position+=step; count--; }
#undef SPAN_PIX
}

static void rp_exec_fuzz(const rp_cmd_t *cm)
{
    int   yl=cm->b, yh=cm->c, count;
    byte *dest;
    /* SATURN: bounds check — rp_exec_col/trans/span check a, b AND c; fuzz
       used to check only cm->a, so a corrupted cm->b made ylookup[cm->b] a
       stale/uninitialised pointer -> wild dest -> stomps vbl_count / gametic
       / us_acc and freezes the game.  Check all three like the others.
       (cm->a is checked per detail mode below: full width vs the halved x.) */
    if ((unsigned short)cm->b >= SCREENHEIGHT ||
        (unsigned short)cm->c >= SCREENHEIGHT) return;
    if (!yl) yl=1;
    if (yh==viewheight-1) yh=viewheight-2;
    count=yh-yl;
    if (count<0 || count>=SCREENHEIGHT) return;
    if (detailshift && !sat_lowres)   /* SATURN PERF 2.3: blocky fuzz (NON-lowres); M7 packs via the path below */
    {
        int   x = (int)cm->a << 1;
        byte *dest2;
        if ((unsigned short)cm->a >= (SCREENWIDTH >> 1)) return;
        dest  = ylookup[yl] + columnofs[x];
        dest2 = ylookup[yl] + columnofs[x + 1];
        do {
            *dest  = colormaps[6*256 + dest [fuzzoffset[fuzzpos]]];
            *dest2 = colormaps[6*256 + dest2[fuzzoffset[fuzzpos]]];
            if (++fuzzpos==FUZZTABLE) fuzzpos=0;
            dest  += SCREENWIDTH;
            dest2 += SCREENWIDTH;
        } while (count--);
        return;
    }
    if ((unsigned short)cm->a >= SCREENWIDTH) return;
    dest=ylookup[yl]+columnofs[cm->a];
    do {
        *dest=colormaps[6*256+dest[fuzzoffset[fuzzpos]]];
        if (++fuzzpos==FUZZTABLE) fuzzpos=0;
        dest+=SCREENWIDTH;
    } while (count--);
}

/* ------------------------------------------------------------------ */
/* Low-detail executors (SATURN PERF 2.3)                              */
/*                                                                     */
/* detailshift!=0 = "blocky" mode: viewwidth is halved and each column */
/* paints TWO adjacent screen pixels (x = recorded-x << 1).  These     */
/* mirror R_DrawColumnLow / R_DrawTranslatedColumnLow / R_DrawSpanLow  */
/* (r_draw.c) byte-for-byte so the parallel path produces identical    */
/* pixels to the serial low path -- we only split the work across the  */
/* two SH-2s (parity on the *halved* column index).  Bounds use        */
/* SCREENWIDTH>>1 because the recorded x is the halved coordinate, and  */
/* x+1 (= 2*recorded_x + 1) must stay < SCREENWIDTH.                    */
/* ------------------------------------------------------------------ */

static void rp_exec_col_low(const rp_cmd_t *cm, const int *colofs)
{
    int           count = cm->c - cm->b;
    int           x;
    byte         *dest, *dest2;
    const byte   *src  = cm->src;
    const byte   *cmap = cm->cmap;
    fixed_t       frac, step;

    if ((unsigned short)cm->a >= (SCREENWIDTH >> 1) ||
        (unsigned short)cm->b >= SCREENHEIGHT       ||
        (unsigned short)cm->c >= SCREENHEIGHT) return;
    if (count < 0) return;
    x     = (int)cm->a << 1;
    dest  = ylookup[cm->b] + colofs[x];
    dest2 = ylookup[cm->b] + colofs[x + 1];
    if ((sat_potato_walls || (sat_wall_paint & 2)) && !cm->unused)   /* opaque wall column -> single colour */
    {
        byte c = cmap[(unsigned char)cm->f3];
        do { *dest = *dest2 = c; dest += SCREENWIDTH; dest2 += SCREENWIDTH; }
        while (count--);
        return;
    }
    step  = cm->f1;
    frac  = cm->f2 + (cm->b - centery) * step;
    do {
        byte p = cmap[src[(frac >> FRACBITS) & 127]];
        *dest = *dest2 = p;
        dest  += SCREENWIDTH;
        dest2 += SCREENWIDTH;
        frac  += step;
    } while (count--);
}

static void rp_exec_trans_low(const rp_cmd_t *cm, const int *colofs)
{
    int     count = cm->c - cm->b;
    int     x;
    byte   *dest, *dest2;
    byte   *xlat = (byte *)cm->f3;
    fixed_t frac, step;

    if ((unsigned short)cm->a >= (SCREENWIDTH >> 1) ||
        (unsigned short)cm->b >= SCREENHEIGHT       ||
        (unsigned short)cm->c >= SCREENHEIGHT) return;
    if (count < 0) return;
    x     = (int)cm->a << 1;
    dest  = ylookup[cm->b] + colofs[x];
    dest2 = ylookup[cm->b] + colofs[x + 1];
    step  = cm->f1;
    frac  = cm->f2 + (cm->b - centery) * step;
    do {
        byte p = cm->cmap[xlat[cm->src[frac >> FRACBITS]]];
        *dest = *dest2 = p;
        dest  += SCREENWIDTH;
        dest2 += SCREENWIDTH;
        frac  += step;
    } while (count--);
}

static void rp_exec_span_low(const rp_cmd_t *cm, const int *colofs)
{
    unsigned int  position, step;
    byte         *dest;
    int           count, x1;
    const byte   *src  = cm->src;
    const byte   *cmap = cm->cmap;

    position = (((unsigned int)cm->f1 << 10) & 0xffff0000)
             | (((unsigned int)cm->f2 >> 6)  & 0x0000ffff);
    step     = (((unsigned int)cm->f3 << 10) & 0xffff0000)
             | (((unsigned int)cm->f4 >> 6)  & 0x0000ffff);

    if ((unsigned short)cm->a >= SCREENHEIGHT       ||
        (unsigned short)cm->b >= (SCREENWIDTH >> 1) ||
        (unsigned short)cm->c >= (SCREENWIDTH >> 1)) return;
    count = cm->c - cm->b;
    if (count < 0) return;      /* corrupt/stale cmd guard (do/while runs >=1x) */
    x1    = (int)cm->b << 1;
    dest  = ylookup[cm->a] + colofs[x1];
    if (sat_potato_floors)
    {
        byte c = cmap[src[POTATO_TEXEL]];             /* fixed flat texel (no rotation) */
        memset(dest, c, (size_t)((count + 1) * 2));   /* low = 2 screen px/source */
        return;
    }
    do {
        byte p = cmap[src[((position >> 26)) | ((position >> 4) & 0x0fc0)]];
        *dest++ = p;
        *dest++ = p;
        position += step;
    } while (count--);
}

static void rp_exec(const rp_cmd_t *cm, int parity, const int *colofs)
{
    if ((cm->a & 1) != parity) return;
    if (detailshift && !sat_lowres)   /* SATURN M7: pack in lowres (fall to the packed executors below) --
                                         the *_low executors double to x<<1=[160,320), off the blitted 160
                                         region (same bug class as the slave sprite drawers, r_things.c). */
    {
        switch (cm->type)
        {
            case RP_COL:   rp_exec_col_low(cm, colofs);   break;
            case RP_TRANS: rp_exec_trans_low(cm, colofs); break;
            case RP_SPAN:  rp_exec_span_low(cm, colofs);  break;
            default: break;
        }
        return;
    }
    switch (cm->type)
    {
        case RP_COL:   rp_exec_col(cm, colofs);  break;
        case RP_TRANS: rp_exec_trans(cm, colofs); break;
        case RP_SPAN:  rp_exec_span(cm, colofs);  break;
        default: break;
    }
}

/* ------------------------------------------------------------------ */
/* Slave side (runs as SRL::Slave::ITask::Start)                       */
/* ------------------------------------------------------------------ */

#if RP_CDIAG
/* Does this command's indices fall outside what its executor accepts?  Matches
   the per-type bounds in rp_exec_col/trans/span/fuzz.  A corrupt (stale) read
   typically has a garbage type byte or an out-of-range index (e.g. b=228). */
static int rp_cmd_corrupt(const rp_cmd_t *c)
{
    if ((unsigned char)c->type > RP_SPAN) return 1;
    if (c->type == RP_SPAN)
        return ((unsigned short)c->a >= SCREENHEIGHT ||
                (unsigned short)c->b >= SCREENWIDTH  ||
                (unsigned short)c->c >= SCREENWIDTH);
    return ((unsigned short)c->a >= SCREENWIDTH  ||
            (unsigned short)c->b >= SCREENHEIGHT ||
            (unsigned short)c->c >= SCREENHEIGHT);
}
#endif

static void rp_slave_body(void)
{
    const rp_cmd_t *cmds = RP_CMDS;
    int i=0, lim, opq, execs=0;
#if RP_CDIAG
    int bad=0;
#endif
#if RP_PROF
    unsigned short t_prev=0;     /* last FRT sample (bounded-delta accumulation) */
    unsigned int   total_acc=0;  /* slave FRT ticks: the WHOLE opaque phase */
    unsigned int   draw_acc=0;   /* slave FRT ticks actually spent drawing */
    /* The slave FRT runs fast (~phi/8), so the opaque phase can exceed the
       65535-tick 16-bit range -> a single end-start subtraction wraps (it made
       total<draw, busy% explode, idle% stick at 0).  Accumulate both as 32-bit
       sums of <16-bit deltas instead: sample at the top of each opaque iteration
       (bounds spin) and per steal chunk (bounds draw bursts). */
#endif

    /* SATURN: slave cache setup.
       The CCR ID bit (0x02 = instruction-cache-replacement disable) was removed
       as the "1.3" speed-up, but on hardware the slave then WEDGES after a few
       frames (row-20 diag showed al0 = the slave stops running, master times out
       -> to climbs -> parallel disabled).  Leading cause: with the I-cache
       enabled, the per-frame CP purge below invalidates the unified 4KB cache
       *while the slave is executing its own cached code* -> instruction-fetch
       hazard -> wedge.  With ID=1 the I-cache isn't used, so the purge is safe on
       instructions (the slave ran fine for minutes this way pre-1.3).
       RP_SLAVE_ICACHE_OFF: tested BOTH ways on hardware -- the slave wedges
       regardless, so the I-cache is NOT the wedge cause (the real fix is the
       opaque-loop guard below).  I-cache ON is ~2x faster on EX, so keep it ON
       (=0).  ID=1 (=1) only as a fallback if a future issue implicates it. */
#define RP_SLAVE_ICACHE_OFF 0
    {
        volatile unsigned char *ccr=(volatile unsigned char *)0xFFFFFE92;
#if RP_SLAVE_ICACHE_OFF
        *ccr=(unsigned char)(*ccr|0x02);   /* ID=1: keep instructions out of cache */
#endif
#if RP_CDIAG
        { static int first=1; if (first) { first=0; SYNC->slave_ccr0=*ccr; } }
        SYNC->slave_ccr1=*ccr;
#endif
        *ccr=(unsigned char)(*ccr|0x10);   /* CP: purge for command-buffer coherency */
    }
    SYNC->slave_alive=1;
#if RP_PROF
    t_prev = rp_frt();
#endif

    /* Adopt the master's Potato state from uncached SYNC (the cached globals could
       be a frame stale on this CPU after a toggle).  Writing our own cached copies
       to the same value the master holds keeps both CPUs in agreement this frame. */
    sat_potato_floors = (SYNC->potato & 1) ? 1 : 0;
    sat_potato_walls  = (SYNC->potato & 2) ? 1 : 0;

    {
        int guard = 100000;         /* anti-wedge: bound pure-spin on masked_at
                                       (~tens of ms worst case; resets on any
                                       progress, so legit brief waits don't trip) */
        for (;;)
        {
#if RP_PROF
            /* sample the previous full iteration (drawing + spin) into total */
            { unsigned short now=rp_frt();
              total_acc += (unsigned short)(now - t_prev); t_prev = now; }
#endif
            int i0 = i;
            opq=SYNC->masked_at;
            lim=(opq>=0 && opq<SYNC->ready) ? opq : SYNC->ready;
#if RP_PROF
            if (i<lim) { unsigned short ts=rp_frt();
#endif
            while (i<lim)
            {
#if RP_CDIAG
                if (rp_cmd_corrupt(&cmds[i]))
                {
                    if (!bad) { SYNC->bad_t=cmds[i].type; SYNC->bad_a=cmds[i].a;
                                SYNC->bad_b=cmds[i].b;    SYNC->bad_c=cmds[i].c; }
                    bad++;
                }
#endif
                rp_exec(&cmds[i++],1,columnofs); execs++;
            }
#if RP_PROF
            draw_acc += (unsigned short)(rp_frt()-ts); }
#endif
            if (opq>=0 && i>=opq) break;
            if (i != i0) guard = 1000000;   /* progress -> reset the guard */
            else if (--guard <= 0)
            {
                /* Never spin forever waiting for masked_at.  Record what we saw
                   and bail; opaque_done is set just below so the master doesn't
                   time out, and the masked loop (clamped) draws the remainder. */
                SYNC->slave_guard++;
                SYNC->slave_g_mat   = opq;
                SYNC->slave_g_i     = i;
                SYNC->slave_g_ready = SYNC->ready;
                break;
            }
        }
    }
    /* SATURN PERF 2.5: two-pointer work-steal.  Help drain the master's parity-0
       opaque, drawing BACKWARD from mat-1 while the master draws forward from 0.
       Stop where we meet the master (j <= m_pos).  Self-balancing: if we're the
       slower CPU (we read commands from RAM, the master has them cached) we simply
       cover fewer commands and W stays ~0 -- no fixed split to mis-tune per scene.
       All of [0,mat) is generated before masked_at is set, so these are valid; the
       parity-1 loop above already read every cmd[] here (no new coherency surface).
       We work in 16-cmd chunks (1 uncached SYNC touch per chunk); the small (<=~1
       chunk) overlap at the crossing is harmless -- opaque has no overdraw, so a
       column drawn by both CPUs gets the identical pixel. */
#if RP_PROF
    /* total-only sample: fold the breaking iteration's tail in before the steal
       (t_prev now starts the steal cleanly, so steal deltas aren't double-counted
       into draw). */
    { unsigned short now=rp_frt(); total_acc += (unsigned short)(now - t_prev); t_prev = now; }
#endif
    {
        int mat = SYNC->masked_at;
        int j = mat, end, k;
        while (j > 0)
        {
            if (j - 1 <= SYNC->m_pos) break;      /* master covers [0, m_pos] */
            end = j - 16;                          /* claim a 16-cmd chunk backward */
            if (end < 0) end = 0;
            for (k = j - 1; k >= end; --k) { rp_exec(&cmds[k], 0, columnofs); execs++; }
            j = end;
            SYNC->s_pos = j;                        /* publish: slave drew [j, mat-1] */
#if RP_PROF
            /* steal is pure drawing -> one bounded delta feeds both accumulators */
            { unsigned short now=rp_frt(); unsigned short d=(unsigned short)(now - t_prev);
              draw_acc += d; total_acc += d; t_prev = now; }
#endif
        }
    }
    SYNC->slave_opaque_done=1;
#if RP_PROF
    /* fold the tail (steal setup / no-steal gap) into total, then publish.  Both
       accumulators are 32-bit sums of <16-bit deltas, so neither wraps even though
       the slave FRT is fast.  total = whole opaque phase (incl. spin-waiting for the
       master); draw = time actually drawing; total-draw = the slack 2.4 reclaims. */
    total_acc += (unsigned short)(rp_frt() - t_prev);
    SYNC->slave_opq_total = (int)total_acc;
    SYNC->slave_opq_draw  = (int)draw_acc;
#endif

    while (!SYNC->go_masked) ;
    {
        int mi = SYNC->masked_at;
        if (mi < 0) mi = 0;     /* guard: masked_at unset -> draw all as masked */
    for (i=mi; i<SYNC->total; ++i)
    {
#if RP_CDIAG
        if (rp_cmd_corrupt(&cmds[i]))
        {
            if (!bad) { SYNC->bad_t=cmds[i].type; SYNC->bad_a=cmds[i].a;
                        SYNC->bad_b=cmds[i].b;    SYNC->bad_c=cmds[i].c; }
            bad++;
        }
#endif
        rp_exec(&cmds[i],1,columnofs); execs++;
    }
    }   /* end masked_at-clamp block */
#if RP_CDIAG
    SYNC->slave_bad=bad;
#endif
    SYNC->slave_execs=execs;
    SYNC->slave_masked_done=1;
}

/* Direct SGL wrapper -- avoids SRL::Types::ITask cache-coherency issues.
** Completion is tracked via SYNC in uncached low WRAM, so no C++ object
** state is needed between frames. */
static void rp_slave_wrapper(void *arg) { (void)arg; rp_slave_body(); }

/* ------------------------------------------------------------------ */
/* Master side                                                          */
/* ------------------------------------------------------------------ */

/* SATURN freeze-proofing (2026-07-19): wall-clock-BOUNDED slave wait -- returns 1 if the slave set
   *flag in time, 0 on timeout (caller then runs the work itself; every fallback below is idempotent
   so a false timeout only costs redundant work, never corruption).  WHY not a spin-COUNT: the
   completion flags live in UNCACHED WRAM (the 0x2xxxxxxx alias), so one loop iteration is a ~25-cycle
   uncached read -> the old 30M/1M count = up to ~26s = a HARD FREEZE whenever a dispatched slave body
   never starts (the SGL slave-scheduler-after-idle quirk: e.g. an M7->M4 mode switch re-arming the
   masked-split -- the reported freeze).  Bound by the FRT (real wall clock) instead.  rp_timeout_count
   (shown on the overlay) tallies every fallback, so a persistently-dead slave is visible, not silent. */
/* 🔴 24 ms -> 100 ms, 2026-08-17.  "> any live slave half" was written when a frame was a few tens
   of milliseconds.  Console frames now run 200-550 ms, the slave's masked half scales with them, and
   `to0:0090` -> `to2:0090` says the MASKED site (and only that site) is firing: cumulative 9+, live
   rate 2.  Every one of those is a frame of masked sprites deliberately NOT drawn (see RP_WaitMasked
   -- the fallback is cosmetic BY DESIGN and must stay that way).  So the timeout was almost certainly
   cutting off a slave that was merely SLOW, not dead, and paying for it in dropped sprites.
   100 ms still does the one job this constant exists for -- never a 26 s wedge -- and stays well
   under the 293 ms at which the 16-bit FRT delta wraps and would read as an INSTANT timeout.
   ⚠ Raising it means a genuinely dead slave now costs 100 ms per site instead of 24.  That trade is
   only defensible because the next line MEASURES the wait: `mw` on row 5 is the max any site waited
   this window.  `mw` ~30 => the old 24 ms was marginal and this fixed real dropped sprites; `mw`
   pinned at 100 => the slave really is dying and that is a different bug, now visible instead of
   hidden behind a counter that only said "it happened". */
#define RP_WAIT_TIMEOUT_FRT 22400u  /* ~100 ms @ ~224 FRT ticks/ms */
/* SATURN 2026-07-31: rp_timeout_count is a SINGLE aggregate over five call sites and is NEVER
   reset, so "it climbs" could not distinguish a level-load burst from a steady leak, and could not
   say WHICH wait failed -- it cannot support any conclusion on its own.  Tally per site too; the
   platform prints a per-second RATE plus the split (overlay `to<rate>:<A><P><M><W>`). */
#define RP_TO_AUX   0   /* RP_AuxWait                    -- clear-on-slave / F-build aux job */
#define RP_TO_PLANE 1   /* RP_WaitPlanes + RP_PlaneJoin  -- plane-split                      */
#define RP_TO_MASK  2   /* RP_DispatchMasked             -- masked/sprite split              */
#define RP_TO_WALL  3   /* RP_WaitWallPrep + the (1p-dead) REC joins                         */
#define RP_TO_SITES 4
int rp_to_site[RP_TO_SITES] = { 0, 0, 0, 0 };

/* 🔴 SATURN 2026-08-17 -- BACK OFF THE SPIN.  `flag` is deliberately UNCACHED (the callers OR in
   0x20000000), so the bare `while (!*flag)` below issued one SYSTEM-BUS read every few cycles for
   the entire wait.  There is ONE bus for the two SH-2s: while the master waits for the slave, that
   poll stream is stealing bandwidth from the very job it is waiting on -- and every slave job we
   own (framebuffer clear, lead-fill) is pure memory traffic, i.e. exactly what bus pressure slows
   down.  Pausing between polls costs nothing: the pause loop runs out of the I-cache and touches
   no bus at all.  64 iterations ~= 200 cycles ~= 7 us, so the join granularity stays three orders
   of magnitude below RP_WAIT_TIMEOUT_FRT and four below any job we dispatch, while the poll rate
   drops from millions/s to ~140k/s.  A job already finished still returns on the FIRST test, so
   the common case (the clear, joined late) pays literally nothing. */
#define RP_SPIN_PAUSE 64
/* 🔴 HOW LONG the master actually waited, max over the window, all sites (row 5 `mw`, ms).  `to`
   only ever said "a wait hit the ceiling" -- it could not say whether the ceiling was 1 ms too low
   or 200, which is exactly the question raising it asks.  Recorded on the SUCCESS path too: a wait
   that returns at 30 ms tells us more than one that times out, because it proves the slave lives. */
unsigned short rp_wait_mx = 0;
static int rp_wait(volatile int *flag, int site)
{
    unsigned short t0 = rp_frt();
    unsigned short el;
    for (;;) {
        int k;
        el = (unsigned short)(rp_frt() - t0);
        if (*flag) {
            if (el > rp_wait_mx) rp_wait_mx = el;
            return 1;
        }
        if (el >= RP_WAIT_TIMEOUT_FRT) {
            rp_wait_mx = el;                 /* the ceiling itself -- always the window max */
            rp_timeout_count++;
            if ((unsigned int)site < (unsigned int)RP_TO_SITES) rp_to_site[site]++;
            return 0;
        }
        for (k = RP_SPIN_PAUSE ; k > 0 ; k--) __asm__ volatile ("nop");
    }
}

/* TEST (2026-06-15): 0 = disable the manual GBR+72 reset, to confirm it is what
   desyncs the slave dispatch (slave dies after a few frames -> al0).  With it
   off the slave should dispatch normally until the ~2-min GBR+72-creep freeze
   returns -- watch row 19 on Ymir for ~30s.  If row 19 then moves continuously,
   the reset is the dispatch-breaker and the real fix is the proper slSynch
   slave-resync (which our manual write-pointer-only reset omits). */
#define RP_GBR_RESET 1

void RP_AuxWait(void);     /* fwd: join the platform's aux slave job before any rewind */
static void master_cache_purge(void);   /* fwd: defined below, used by RP_LeadJoin */
void RP_PlaneJoin(void);   /* fwd: join an OUTSTANDING plane dispatch before any rewind (see below) */

/* SATURN: THE ~1-2min freeze fix.  slSlaveFunc bump-allocates a 12-byte record
   {0x30, func, arg} from the SGL transient work buffer at GBR+72 and advances
   the pointer every call, but NEVER resets it -- SGL normally resets it once
   per frame inside slSynch(), which Mimas replaced with its own vblank sync.
   Because rp_restart() calls slSlaveFunc every frame, GBR+72 crept forward 12
   bytes/frame into the SGL system area and after ~1-2 min overran the VBlank
   user-callback pointer at GBR+20 (0x060FFC14); _BlankIn then jsr'd to garbage
   -> CPU illegal-instruction exception -> SGL halt-loop = the freeze.  We
   restore GBR+72 to its post-init base before each slSlaveFunc, exactly as
   slSynch would, so the single per-frame record always reuses the same slot. */
void rp_sgl_workptr_reset(void)
{
    /* The rewind reuses SGL record slot 0, so it must never run while the slave still
       owns a record: JOIN the platform's aux job first (RP_AuxDispatch).  Every dispatch
       site funnels through this reset, so the slave job queue stays strictly sequential.
       No aux job pending (DoomJo: always) => a single uncached read. */
    RP_AuxWait();
    /* SATURN 2026-07-30 (`to` climbing on Ymir, root-caused): the aux job was NOT the only record
       the slave could still own.  RP_DrawPlanesSplit's `m < 0` fast path -- the master's work-steal
       claimed EVERY plane before the slave got scheduled -- returns WITHOUT joining, yet slSlaveFunc
       has already queued a plane record.  The rewind below then reuses SGL slot 0 under that live
       record: the slave either loses the next dispatch (master spins the full 24ms -> rp_timeout_count++)
       or wakes onto the NEXT frame's rp_plane_lock[]/worklist (the .bss-stomp class).  Joining here --
       at the rewind, which is where correctness actually demands it, not at the end of the plane phase --
       keeps the `m < 0` fast path free (the master never blocks in the plane phase) while guaranteeing
       the slot is idle before it is reused.  By this point the slave has had a whole phase to finish,
       so the join is normally a single uncached read. */
    RP_PlaneJoin();
    /* The SGL slave work area has TWO pointers that slSlaveFunc/the slave bump
       +12B per frame and that slSynch normally resets together: the WRITE pointer
       at GBR+72 and the slave's READ pointer at GBR+68 (confirmed on hardware via
       a work-area dump: both hold the same value and creep in lockstep up to
       GBR+20, the VBlank callback, = the freeze).  The original fix reset ONLY
       +72 -> the read pointer (+68) stayed desynced (slave stopped dispatching ->
       we rendered serial) AND kept creeping (-> the freeze was never really
       gone).  Reset BOTH to their captured base each frame, exactly as slSynch
       would, but without slSynch's vblank-cap and SCSP-sound side effects. */
    static volatile unsigned int *wp72 = 0;
    static volatile unsigned int *wp68 = 0;
    static unsigned int           base72 = 0, base68 = 0;
    if (!wp72)
    {
        unsigned int gbr;
        __asm__ volatile ("stc gbr,%0" : "=r"(gbr));
        wp72   = (volatile unsigned int *)(gbr + 72);
        wp68   = (volatile unsigned int *)(gbr + 68);
        base72 = *wp72;             /* capture clean bases on the first frame */
        base68 = *wp68;
    }
    else
    {
        *wp72 = base72;             /* write pointer */
        *wp68 = base68;             /* read pointer (the bit the old fix missed) */
    }
}

static void rp_restart(void)
{
#if RP_GBR_RESET
    rp_sgl_workptr_reset();
#endif
    SYNC->ready=0;
    SYNC->masked_at=in_masked?0:-1;
    SYNC->total=0;
    SYNC->go_masked=0;
    SYNC->slave_opaque_done=0;
    SYNC->slave_masked_done=0;
    SYNC->slave_alive=0;
#if RP_PROF
    SYNC->slave_opq_total=0;   /* stale-guard: cleared in case the slave wedges */
    SYNC->slave_opq_draw=0;
#endif
    /* SATURN PERF 2.5: two-pointer EX drain.  m_pos=-1 so the slave can draw down
       to index 0; s_pos = a sentinel above any possible mat so the master can draw
       forward freely until the slave starts decrementing s_pos.  Set before the
       slave is dispatched, so it sees initialised values. */
    SYNC->m_pos=-1;
    SYNC->s_pos=0x7fffffff;
    /* publish the Potato state for the slave (uncached -> coherent this frame) */
    SYNC->potato = (sat_potato_floors ? 1 : 0)
                 | ((sat_potato_walls || (sat_wall_paint & 2)) ? 2 : 0);        /* the debug paint rides the
                       existing SYNC bit so the SLAVE's solid-wall test agrees without reading the
                       (cached, master-written) sat_wall_paint itself */
#if RP_CDIAG
    SYNC->slave_bad=0;
#endif
    rec_count=0;
    rec_masked_at=in_masked?0:-1;
    slSlaveFunc(rp_slave_wrapper, 0);
}

static void master_cache_purge(void)
{
    volatile unsigned char *ccr=(volatile unsigned char *)0xFFFFFE92;
    *ccr=(unsigned char)(*ccr|0x10);
}

/* ------------------------------------------------------------------------------------------ *
 * SATURN parallel-REC (Option C / P3) -- the d32xr visplane split.                            *
 * The slave SH-2 draws a HALF of the regular-flat worklist (r_plane.c R_DrawPlaneWorklist)    *
 * while the master draws the other half.  This offloads the master-only plane phase (P) onto  *
 * BOTH CPUs.  It REPLACES the command-renderer parity for planes, so the platform forces       *
 * rp_disabled=1 (src/main.cxx) when sat_plane_parallel: the parity slave is then NEVER          *
 * dispatched, so the slave SH-2 is free for RP_DispatchPlanes, and there is no second-dispatch  *
 * conflict.  Pre-conditions held by the master: every flat is already cached (the worklist     *
 * stores src), so the slave never touches the zone allocator; the visplanes the two CPUs draw  *
 * are disjoint, so their framebuffer writes never overlap (Doom has no plane overdraw).        *
 * A 2nd slSlaveFunc per frame -> rewind the SGL slave work pointer first (the same GBR-creep    *
 * guard rp_restart uses).  No big slave stack: there is NO BSP recursion in the plane draw      *
 * (R_DrawPlaneWorklist -> R_DrawVisplane* -> R_*Span, shallow; the only stack cost is the       *
 * local spanstart_l[SCREENHEIGHT] ~0.9KB).                                                      *
 * ------------------------------------------------------------------------------------------ */
int sat_plane_parallel = 0;              /* set by the Mimas platform (src/main.cxx) */
/* SATURN 2026-08-20: allow the slave shares (plane-split here, masked-split in r_things.c) in
   SPLIT co-op too.  The `sat_local_players <= 1` gates were the 2026-07-20 freeze guard, kept
   after the mechanism they guarded was hardened away (RP_WaitPlanes FRT-bounded + idempotent
   fallback, RP_WaitMasked 100 ms timeout with the cosmetic no-draw fallback) -- re-enabled for
   1p on 2026-07-30 and simply never retested in MP (owner, 2026-08-20).  HW 2p/3p/4p captures
   read SLV b0% across entire sessions while the per-view render sums ran 50-140 ms: a whole
   CPU idle in exactly the modes that need it most.  1 = both shares follow the 1p rules in
   split (each view's dispatch joins inside its own R_DrawPlanes / R_DrawMasked, so no
   cross-view state is ever live on the slave); 0 = the old MP-off behaviour (platform chord
   pad R+B, togglable from the 1p menu side and persistent into the split session). */
/* (sat_mp_slave REMOVED 2026-08-26 -- baked ON.  The fix it guarded is console-validated:
   the 4p captures of 2026-08-26 read SLV b4%..b35% where every pre-fix video read b0%.
   Its pad R+B also collided with the sprite-SQ chord in 1p.) */
extern void R_DrawPlaneWorklist(int from, int to);

static volatile int rp_plane_done = 1;
#define PLANE_DONE (*(volatile int *)((unsigned int)&rp_plane_done | 0x20000000u))

/* SGL's slave stack (0x06001e00) is only ~1-2KB before it hits SGL system data, but
   R_DrawVisplane* puts a local spanstart_l[SCREENHEIGHT] (~0.9KB) + frames on the stack ->
   it would overflow and corrupt SGL.  Give the plane slave its OWN 4KB stack and switch to
   it for the draw.  (Only the slave touches this, so its write-through writes need no purge.) */
static char rp_plane_slave_stack[4 * 1024] __attribute__((aligned(16)));
/* SH-2 trampoline: save r14 on the old stack, keep the old SP in r14 (callee-saved, so the
   call preserves it), switch r15 to the dedicated stack, call fn, restore.  Shared by the plane
   AND masked slave dispatch (they run in different frame phases, never concurrently). */
static void rp_run_on_stack(void (*fn)(void))
{
    void *newsp = rp_plane_slave_stack + sizeof(rp_plane_slave_stack);
    __asm__ volatile (
        "mov.l  r14, @-r15\n\t"   /* save r14 on the OLD stack */
        "mov    r15, r14\n\t"     /* r14 = old SP (survives the call) */
        "mov    %[ns], r15\n\t"   /* switch to the dedicated stack */
        "jsr    @%[fn]\n\t"
        "nop\n\t"
        "mov    r14, r15\n\t"     /* restore old SP */
        "mov.l  @r15+, r14\n\t"   /* restore r14 */
        :
        : [ns]"r"(newsp), [fn]"r"(fn)
        : "r0","r1","r2","r3","r4","r5","r6","r7","pr","t","mach","macl","memory");
}

/* ------------------------------------------------------------------------------------------ *
 * SATURN generic AUX slave job (platform end-of-frame work; Mimas: the VDP1 F-bank tile      *
 * build, dg_saturn vdp1_ftex_flush -> ftex_slave_build).  ONE job at a time, dispatched by   *
 * the platform AFTER its render-phase master work; every dispatcher funnels through           *
 * rp_sgl_workptr_reset(), which now JOINS a pending aux job first, so the SGL slave record    *
 * queue stays strictly sequential (the rewind reuses slot 0).  The job runs on the dedicated  *
 * 4KB stack (the plane/masked jobs it shares it with are joined before any new dispatch).     *
 * DoomJo: never dispatched -> the done flag stays 1 and every join is a single uncached read. */
static volatile int rp_aux_done_v = 1;
#define RP_AUX_DONE (*(volatile int *)((unsigned int)&rp_aux_done_v | 0x20000000u))
static void (*rp_aux_fn)(void);

/* Join the aux job AND purge -- for a job that wrote PIXELS (the lead-fill spans).  The plain
   RP_AuxWait skips the purge because the framebuffer clear it was built for is read next by the
   blit, which purges itself; the masked pass that follows the lead spans does not. */
void RP_LeadJoin(void)
{
    RP_AuxWait();
    master_cache_purge();
}

void RP_AuxWait(void)
{
    /* FRT-bounded (see rp_wait).  No master fallback: the aux job is a cosmetic VDP1-floor build --
       a missed frame is a 1-frame floor glitch, not a crash, and the next frame re-dispatches. */
    rp_wait(&RP_AUX_DONE, RP_TO_AUX);
}

/* SATURN PERF (2026-07-08): slave self-timing brackets for the MEASURED busy%.  BEGIN forces the
   slave FRT divider to phi/128 (== the master's, set by SGL) and stamps t0; END adds the elapsed
   ticks to the monotonic uncached slave_busy accumulator the master diffs each frame.  Wrap-safe
   (16-bit delta < ~293ms).  Fully compiled out unless RP_PROF, so DoomJo / a shipping build pay
   nothing (and the slave FRT is never touched there).  __sb_t0 is block-scoped per body. */
#if RP_PROF
#define SLAVE_BUSY_BEGIN() *(volatile unsigned char *)0xFFFFFE16 = 0x02; \
                           unsigned short __sb_t0 = rp_frt()
#define SLAVE_BUSY_END()   (SYNC->slave_busy += (unsigned short)(rp_frt() - __sb_t0))
/* plane bodies: count into BOTH the total and the plane-only accumulator (for Pb). */
#define SLAVE_PBUSY_END()  do { unsigned short __d = (unsigned short)(rp_frt() - __sb_t0); \
                                SYNC->slave_busy += __d; SYNC->slave_pbusy += __d; } while (0)
#else
#define SLAVE_BUSY_BEGIN() ((void)0)
#define SLAVE_BUSY_END()   ((void)0)
#define SLAVE_PBUSY_END()  ((void)0)
#endif

static void rp_aux_body(void *param)
{
    SLAVE_BUSY_BEGIN();
    (void)param;
    master_cache_purge();                     /* slave: read the master's fresh accumulators/tables */
    rp_run_on_stack(rp_aux_fn);
    SLAVE_BUSY_END();
    RP_AUX_DONE = 1;
}

void RP_AuxDispatch(void (*fn)(void))
{
    rp_sgl_workptr_reset();                   /* joins any pending aux + rewinds the SGL work slot */
    rp_aux_fn = fn;                           /* write-through -> in RAM before the slave's purge+read */
    RP_AUX_DONE = 0;
    slSlaveFunc(rp_aux_body, 0);
}

/* ---- aux PIGGYBACK (Mimas, owner GO 2026-07-03): the platform ARMS the job at the end of
   R_DrawPlanes; the masked slave body CLAIMS it after its sprite half and runs it right
   after publishing MASK_DONE -- the build then overlaps the master's masked tail + the DG
   pre-work instead of stalling in the pre-blit join.  No TAS needed: the arm (master, end
   of P) strictly precedes the masked dispatch, and the DG fallback only runs after
   RP_WaitMasked, by which time TAKEN (written BEFORE MASK_DONE; SH-2 write-through buffer
   drains in order) is visible.  Masked not dispatched this frame (no sprites / toggle off /
   menu) -> RP_AuxKick falls back to a plain aux dispatch.  ARMED never leaks a frame: the
   platform always calls RP_AuxKick from DG.  DoomJo: never armed -> two one-read checks. */
static volatile int rp_aux_armed_v = 0, rp_aux_taken_v = 0;
#define RP_AUX_ARMED (*(volatile int *)((unsigned int)&rp_aux_armed_v | 0x20000000u))
#define RP_AUX_TAKEN (*(volatile int *)((unsigned int)&rp_aux_taken_v | 0x20000000u))

void RP_AuxArm(void (*fn)(void))
{
    RP_AuxWait();                  /* the previous job must be fully retired */
    rp_aux_fn = fn;                /* DONE stays 1 (nothing runs yet) so the masked
                                      dispatch's rewind-join cannot deadlock on the arm */
    RP_AUX_TAKEN = 0;
    RP_AUX_ARMED = 1;
}

int RP_AuxKick(void)               /* platform DG-entry consumer; 1 = a job was armed */
{
    if (!RP_AUX_ARMED) return 0;
    RP_AUX_ARMED = 0;
    if (!RP_AUX_TAKEN)
        RP_AuxDispatch(rp_aux_fn); /* the masked body never took it -> plain aux job */
    return 1;
}

/* SATURN PERF (Fafling-style / sat_plane_tas): TAS.B plane work-steal.  Both CPUs
   claim planes from opposite ends and meet in the middle; the lock byte is read via SH-2 TAS.B
   (bus-locked + cache-bypassing for the RMW, like Fafling's R_Render_Span), so there is NO per-plane
   uncached cursor publish/read (the cost that made the cursor steal regress) and the single meeting
   plane is resolved exactly-once (no <=1-plane double-draw).  The slave still whole-cache-purges once
   at start for per-frame worklist/visplane/table coherency (the flat+tables re-fault is ~1-2ms, far
   below the count-split imbalance the steal removes at low n).  DoomJo keeps sat_plane_parallel=0 so
   it never reaches here.  rp_plane_lock[] = 256 bytes .bss, harmless/unused in DoomJo. */
static volatile unsigned char rp_plane_lock[MAXVISPLANES];
/* DEFAULT ON (2026-06-29): HW-validated >= static EVERYWHERE (+0.9..+0.3 fps, never regresses).
   The master idle it leaves (w) is FREE -- the master has nothing else in the plane phase -- so
   row-split (which fills that idle by DUPLICATING the plane walk on both CPUs) is reproducibly
   ~0.3 fps SLOWER and stays parked (sat_plane_rowsplit, off).  Toggle off via pad-C for A/B. */
int sat_plane_tas = 1;

/* TAS.B claim of rp_plane_lock[i]: returns 1 if WE won it (byte was 0), 0 if already taken. */
static inline int rp_tas_claim(int i)
{
    int won;
    __asm__ volatile ("tas.b @%1\n\tmovt %0\n\t"
                      : "=r"(won) : "r"(&rp_plane_lock[i]) : "t","memory");
    return won;   /* TAS sets T = (byte==0); movt -> 1 means we claimed it */
}

static int rp_tas_n;
static void rp_plane_tas_slave(void)     /* slave: claim UP from 0, stop where it meets the master */
{
    int i = 0;
    while (i < rp_tas_n) { if (!rp_tas_claim(i)) break; R_DrawPlaneWorklist(i, i + 1); i++; }
}
static void rp_plane_tas_body(void *param)
{
    SLAVE_BUSY_BEGIN();
    master_cache_purge();                /* per-frame worklist/visplane/table coherency (slave cold) */
    rp_tas_n = (int)(unsigned int)param;
    rp_run_on_stack(rp_plane_tas_slave); /* draw on the dedicated 4KB stack */
    SLAVE_PBUSY_END();
    PLANE_DONE = 1;
}

void RP_WaitPlanes(void);   /* defined just below */

/* Visplane split: master + slave via TAS.B meet-in-the-middle work-steal (the shipped default;
   the static + row-split + cursor-steal A/B losers were removed 2026-07-16, docs/TOGGLE_AUDIT.md). */
static int rp_plane_disp_n;                  /* # planes dispatched, for the RP_WaitPlanes timeout fallback */
/* MASTER-ONLY: a plane record has been queued with slSlaveFunc and not yet joined.  It exists purely
   so the deferred join in rp_sgl_workptr_reset knows whether the slave still owns SGL slot 0. */
static int rp_plane_pending;

/* Deferred join of an outstanding plane dispatch.  Called from rp_sgl_workptr_reset (i.e. from the
   NEXT dispatch of any kind), never from the plane phase itself, so the `m < 0` fast path stays
   non-blocking.  Deliberately does NOT run RP_WaitPlanes's idempotent redraw: by now the worklist
   may belong to the next frame, and a slave that really stalled cost us at most one frame of
   not-drawn planes (cosmetic, self-correcting) -- never a redraw into the wrong frame. */
static int rp_plane_join_fails;              /* consecutive deferred-join timeouts */
static int rp_plane_dead;                    /* latched: stop dispatching, draw planes master-only */

void RP_PlaneJoin(void)
{
    if (!rp_plane_pending) return;
    rp_plane_pending = 0;                    /* clear FIRST: a timed-out join must not re-arm itself */
    if (rp_wait(&PLANE_DONE, RP_TO_PLANE))   /* FRT-bounded; tallies rp_timeout_count if the slave died */
        rp_plane_join_fails = 0;
    /* SELF-HEAL: a genuinely dead slave would otherwise burn the full 24ms timeout on EVERY frame's
       deferred join -- far worse than the un-joined record we are fixing.  After 4 consecutive
       failures, latch the split off and draw planes master-only (correct, just serial).  `to` still
       shows the 4 failures, so the degradation is visible rather than silent. */
    else if (++rp_plane_join_fails >= 4)
        rp_plane_dead = 1;
    master_cache_purge();                    /* read whatever the slave drew before anything else runs */
}

void RP_DrawPlanesSplit(int n)
{
    int m = n - 1;
    if (rp_plane_dead) { RP_MPlaneEnter(); R_DrawPlaneWorklist(0, n); RP_MPlaneLeave(); return; }   /* self-healed to master-only */
    rp_plane_disp_n = n;
    rp_sgl_workptr_reset();                  /* GBR-creep guard + joins aux AND any outstanding plane job */
    PLANE_DONE = 0;                          /* AFTER the join above -- it waits on this very flag */
    for (int i = 0; i < n; i++) rp_plane_lock[i] = 0;         /* arm claims (write-through to RAM) */
    slSlaveFunc(rp_plane_tas_body, (void *)(unsigned int)n);  /* slave claims UP from 0 */
    rp_plane_pending = 1;                    /* a record is now live -> must be joined before any rewind */
    {   /* SATURN 2026-08-26 -- THE LATE KICK LANDS HERE, and this line is the whole point: the
           slave is running (slSlaveFunc above), the master has nothing claimed yet, so the VDP1
           emission now happens IN PARALLEL with the slave's plane share instead of ahead of it.
           r_main.c owns the flag and the fallback; see the note at sat_kick_late there. */
        extern int sat_kick_pending;
        extern void (*sat_walls_done_hook)(void);
        if (sat_kick_pending) { sat_kick_pending = 0; if (sat_walls_done_hook) sat_walls_done_hook (); }
    }
    RP_MPlaneEnter();
    while (m >= 0) { if (!rp_tas_claim(m)) break; R_DrawPlaneWorklist(m, m + 1); m--; }  /* master DOWN */
    RP_MPlaneLeave();                        /* row 5 `Pm`: the master's fill share of the steal */
    /* m<0 => the master's meet-in-the-middle steal claimed+drew EVERY plane before the slave got
       scheduled => there is no WORK to wait for, so we do not block here.  The record is still live
       though: rp_plane_pending leaves it to the deferred join (rp_sgl_workptr_reset).  Only block now
       when the master broke on a plane the slave had locked (m>=0) -- the slave is provably alive. */
    if (m >= 0) { RP_WaitPlanes(); rp_plane_pending = 0; }    /* joined here; bounded + fallback + purge */
    else        master_cache_purge();
}

void RP_WaitPlanes(void)
{
#if RP_PROF
    unsigned short t0 = rp_frt();             /* master idle while the slave finishes = imbalance */
#endif
    int ok = rp_wait(&PLANE_DONE, RP_TO_PLANE);   /* FRT-bounded (see rp_wait) -- never a 26s wedge */
#if RP_PROF
    p3_wait_ticks = (unsigned short)(rp_frt() - t0);
#endif
    /* slave stalled mid-draw (claimed planes it never finished): idempotently redraw the whole
       worklist.  Opaque flats -> the redraw writes the SAME pixels, so no missing planes, no double
       image; the only cost is the redundant draw of the ones already done (a slow frame, not a hang). */
    if (!ok) { RP_MPlaneEnter(); R_DrawPlaneWorklist(0, rp_plane_disp_n); RP_MPlaneLeave(); }
    master_cache_purge();                     /* read the slave's drawn plane pixels before the blit */
}

/* SATURN (2026-07-18): M7 slave plane-split ABANDONED on HW.  A TRIVIAL probe body ran on the slave
   in M7 (overlay stage 2 + done), but the REAL draw body never even ENTERED it (overlay s0, SLV Pb0
   id100 -- the slave stays 100% idle), so the master fell back every frame; with the wait guard
   sized for a working slave, the per-frame spin (an uncached PLANE_DONE poll x400k) cost ~350ms =
   2.4fps at spawn.  Root: the SGL slave doesn't reliably execute a dispatched DRAW body when the
   probe/split is the ONLY per-frame slSlaveFunc (M7 runs no masked-split/aux to pump the SGL slave
   scheduler) -- the same thing that froze the original 30M-guard split.  The trivial probe's
   apparent success was misleading (a microsecond body that runs != a real draw body that doesn't).
   Reclaiming the M7 idle slave needs the SGL slave-pump understood first; until then M7 planes stay
   MASTER-ONLY (r_plane.c), which is the stable ~13fps behaviour.  Do NOT re-wire RP_DrawPlanes* into
   the sat_lowres branch without solving the pump. */

/* SATURN (2026-07-19) M7 SLAVE-PUMP PROBE -- built + run on HW, then REMOVED (it answered its
   question).  Verdict: a real slave DRAW body does NOT run in single-mode M7 (trivial body ran,
   realwork body never entered, a second dispatch did not "pump" it, and a failed dispatch wedges
   the slave).  The idle M7 slave cannot be given real draw work via slSlaveFunc -- see the memory
   [[slave-sh2-vdp1-flicker-offload]].  The gate above stands: do NOT re-wire RP_DrawPlanes* / any
   slave draw offload into the sat_lowres branch. */

/* ------------------------------------------------------------------------------------------ *
 * SATURN masked-by-half (Option B): the slave draws the RIGHT-half vissprites (r_things.c        *
 * R_SlaveDrawMasked) while the master draws the LEFT half, during the masked phase.  Same        *
 * dedicated stack + GBR-creep guard as the plane dispatch; runs in a different phase so there    *
 * is no overlap.  The slave's masked column state is its own (s_* in r_things.c).                *
 * ------------------------------------------------------------------------------------------ */
extern void R_SlaveDrawMasked(int x0, int x1);
static volatile int rp_mask_done = 1;
#define MASK_DONE (*(volatile int *)((unsigned int)&rp_mask_done | 0x20000000u))
static int rp_mask_x0, rp_mask_x1;

static void rp_masked_noarg(void) { R_SlaveDrawMasked(rp_mask_x0, rp_mask_x1); }

static void rp_masked_slave_body(void *param)
{
    int packed = (int)(unsigned int)param;
    int take;
    SLAVE_BUSY_BEGIN();
    master_cache_purge();                    /* slave: read the master's fresh vissprites + drawsegs */
    rp_mask_x0 = (packed >> 16) & 0xffff;
    rp_mask_x1 = packed & 0xffff;
    rp_run_on_stack(rp_masked_noarg);
    /* aux PIGGYBACK: claim the armed job BEFORE publishing MASK_DONE (in-order write-through
       -> the master's DG fallback can never see MASK_DONE with an unclaimed job), run it
       AFTER -- the master resumes its masked tail + DG pre while the build runs here. */
    take = RP_AUX_ARMED;
    if (take) { RP_AUX_TAKEN = 1; RP_AUX_DONE = 0; }
    SLAVE_BUSY_END();                        /* masked-half work (before publishing MASK_DONE) */
    MASK_DONE = 1;
    if (take) {
#if RP_PROF
        unsigned short __ax_t0 = rp_frt();   /* time the F-build piggyback on the slave too */
#endif
        rp_run_on_stack(rp_aux_fn); RP_AUX_DONE = 1;
#if RP_PROF
        SYNC->slave_busy += (unsigned short)(rp_frt() - __ax_t0);
#endif
    }
}

void RP_DispatchMasked(int x0, int x1)
{
    MASK_DONE = 0;
    rp_sgl_workptr_reset();
    slSlaveFunc(rp_masked_slave_body, (void *)(unsigned int)(((x0 & 0xffff) << 16) | (x1 & 0xffff)));
}

void RP_WaitMasked(void)
{
    /* FRT-bounded (see rp_wait) -- never a 26s wedge.  On timeout we deliberately DO NOT redraw the
       slave's [x0,x1) half on the master: that master-side R_SlaveDrawMasked while a LATE (not dead)
       slave may still own its SGL command slot was the mode-switch whole-screen corruption suspect
       (2026-07-19).  A missed half is ONE frame of not-drawn masked sprites (cosmetic), which the
       next frame corrects -- a far safer failure mode than risking VDP1 command-list corruption. */
    rp_wait(&MASK_DONE, RP_TO_MASK);
    master_cache_purge();                     /* read the slave's drawn sprite pixels before the blit */
}

/* ------------------------------------------------------------------------------------------ *
 * SATURN RANK 3 (docs/RANK3_WALLPREP.md) inc-1: run the deferred wall-prep flush on the SLAVE   *
 * instead of the master.  NON-overlapped -- the master dispatches AFTER the BSP walk (walljob_n  *
 * final) and spins in RP_WaitWallPrep.  This relocates the Bp bucket to the slave timeline       *
 * (validates byte-identity + the cache/stack/coherency path) but yields NO fps win yet (inc-2    *
 * pipelines it behind the walk).  The slave is the SINGLE in-order consumer of walljobs[0..n)    *
 * (= BSP order), so the floorclip/ceilingclip occlusion chain stays byte-identical.  Same        *
 * dedicated 4KB stack + GBR-creep guard as the plane/masked dispatch; runs in the B phase,       *
 * strictly before P and M, so the single slave never overlaps its own later work.                *
 * ------------------------------------------------------------------------------------------ */
extern void RP_FlushWallsRange(int from, int to);

static volatile int rp_wp_done = 1;
#define WP_DONE (*(volatile int *)((unsigned int)&rp_wp_done | 0x20000000u))
static int rp_wp_n;          /* slave-side: how many walls to flush (n via slSlaveFunc param) */
static int rp_wp_disp_n;     /* master-side: n dispatched, for the serial timeout fallback     */
/* master-FRT ticks the master spent in RP_WaitWallPrep = the slave's flush wall-clock.  This is
   the inc-2 go/no-go number: if it's >= the old on-master Bp, the slave is slower (LWRAM cold) and
   inc-2's overlap won't pay.  Surfaced as 'Bp' on row 5 when sat_wallprep_slave is on (so Bp always
   = wall-prep cost wherever it ran; the slave's own FRT cannot time a duration, see :176). */
static unsigned int prof_wpwait;

int sat_wallprep_slave = 0;  /* live pad toggle (L+R), 0/1/2; the platform ties sat_wallprep_defer.
   1 = slave + full cache purge (cold, correct).  2 = slave + NO purge = WARM diagnostic (d32xr keeps
   its secondary warm; here the slave reuses last frame's cache -> CORRECT ONLY in a static scene,
   garbage if you move, but it measures the warm 'Bp' ceiling to confirm the cold-purge hypothesis). */

static void rp_wallprep_slave_fn(void)   /* runs on the dedicated 4KB stack */
{
    RP_FlushWallsRange(0, rp_wp_n);       /* flush all queued walls, in BSP order */
}

static void rp_wallprep_body(void *param)
{
    unsigned int p = (unsigned int)param; /* (mode<<16)|n -- mode via the register, always fresh */
    SLAVE_BUSY_BEGIN();
    if (((p >> 16) & 0xff) == 1)
        master_cache_purge();             /* mode 1: full purge (slave reads master's fresh data).
                                             mode 2: SKIP = warm (static-scene-correct diagnostic). */
    rp_wp_n = (int)(p & 0xffff);          /* set AFTER any purge -- n via param, no cached word */
    rp_run_on_stack(rp_wallprep_slave_fn);
    SLAVE_BUSY_END();
    WP_DONE = 1;                          /* last statement, uncached */
}

void RP_DispatchWallPrep(int n)
{
    int mode = sat_wallprep_slave;        /* master reads it fresh; passed to the slave via the param */
    rp_wp_disp_n = n;
    WP_DONE = 0;
    rp_sgl_workptr_reset();               /* GBR-creep guard for this dispatch */
    slSlaveFunc(rp_wallprep_body, (void *)(unsigned int)(((mode & 0xff) << 16) | (n & 0xffff)));
}

void RP_WaitWallPrep(void)
{
#if RP_PROF
    unsigned short t0 = rp_frt();          /* master idle while the slave flushes = the flush time */
#endif
    int ok = rp_wait(&WP_DONE, RP_TO_WALL);   /* FRT-bounded (see rp_wait) -- never a 26s wedge */
#if RP_PROF
    prof_wpwait = (unsigned short)(rp_frt() - t0);
#endif
    if (!ok)
        RP_FlushWallsRange(0, rp_wp_disp_n);   /* slave died -> serial fallback (idempotent enough) */
    master_cache_purge();                 /* read the slave's drawsegs/visplanes/clip/wall_acc */
}

/* SATURN PERF: master frame ms, set once/sec by the platform fps_update
   (dg_saturn.cxx) and printed as the prefix of the slave's row-18 SLV line (the
   standalone MST row was dropped -- it only pointed at rows 19/20).  Defined
   unconditionally so the platform's extern links even when RP_PROF is off. */
unsigned int rp_master_ms = 0;
/* SATURN 2026-08-24: the frame's TOTAL render (Bw+Bp+P+M, all views summed) in TENTHS of a ms --
   the LOD governor's own decision quantity, exported so the platform's wall-span governor can
   steer on the same clock instead of the 1 Hz overlay period.  0 until the first sane frame. */
unsigned int rp_rend10 = 0;

/* SATURN (VDP1-floor inc-0): the floor-quad estimate surfaced to a VISIBLE overlay row.
   Row 13 (FLAT) sits in the split-screen viewport band = unreadable, so the platform
   also prints these on row 2.  cur = this frame's VDP1 candidate cost (Vs); peak =
   monotonic worst case (the go/no-go number).  Defined unconditionally so the platform
   extern links even when RP_PROF is off (they then stay 0). */
int sat_floor_vq_cur  = 0;
int sat_floor_vq_peak = 0;

/* SATURN overlay cost control (written by the platform pad; defined here so both ports link).
   sat_dbg_overlay_mode: 0 = full perf overlay / 1 = fps-only / 2 = off.  Gates the per-frame
   profiler PRINTS (rows below) AND the expensive RP_PlanePixels rescan, so the fps-only mode
   measures the TRUE overlay tax (not just the display).
   sat_prof_planepix: runtime gate for RP_PlanePixels -- a per-visplane, per-COLUMN rescan inside
   R_DrawPlanes that feeds only the FLR/CLS floor sizer (off the default overlay), and which the
   code notes "inflates P".  Default 0 = no rescan in normal play; set 1 (pad) only when sizing the
   non-dominant-floor lever. */
int sat_dbg_overlay_mode = 0;
int sat_prof_planepix    = 0;

#if RP_PROF
/* Read the SH-2 free-running timer (FRC @ 0xFFFFFE12/13).  Read H then L: the H read latches the
   low byte into a temp register that the L read returns.
   SATURN MEASUREMENT FIX 2026-07-31 -- INTERRUPTS MUST BE MASKED ACROSS THE PAIR.  That temp
   register is GLOBAL to the CPU, and dg_saturn's vblank_handler calls its own frt_read 60x/s
   (src/dg_saturn.cxx:1593).  A vblank IRQ landing between our H and L reads hands us the ISR's
   latched low byte instead of ours -> a composed value up to 256 ticks BELOW the truth.  Inside
   rp_wait's tight spin, during the first ~1.14ms of a wait the true elapsed is under 256 ticks, so
   that underflows (unsigned short)(now - t0) to ~65000 >= RP_WAIT_TIMEOUT_FRT and fires an INSTANT
   FALSE TIMEOUT (rp_timeout_count++ = the overlay `to` climbing).  It also silently perturbs every
   master-side RP_PROF delta built on this function.  dg_saturn's frt_read has always masked for
   exactly this reason (src/dg_saturn.cxx:1565-1576); this one never did.  Same 4-instruction guard. */
static unsigned short rp_frt(void)
{
    unsigned int  sr, sr_masked;
    unsigned char h, l;
    __asm__ volatile ("stc sr, %0" : "=r"(sr));
    sr_masked = sr | 0xF0;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr_masked) : "memory");
    h = *(volatile unsigned char *)0xFFFFFE12;
    l = *(volatile unsigned char *)0xFFFFFE13;
    __asm__ volatile ("ldc %0, sr" :: "r"(sr) : "memory");
    return (unsigned short)((h << 8) | l);
}

/* SATURN 2026-08-16 -- THE GAME TIC HAS NO INSTRUMENTATION AND IT IS THE BIGGEST TERM ON HARDWARE.
   Four hardware captures put row-1 `T` at 69-83 ms of a 181-222 ms frame -- ~40 % -- while the SAME
   build on Ymir reads T 8-14.  A 6-9x gap the emulator hides completely, so the entire renderer
   hunt was optimising `R` with the largest single cost invisible.  Nothing inside `T` has ever been
   timed; the row that says `LOS` is the WALL row (its sight counters were dropped for space in
   August) and `sightcounts[]` has sat in core/p_sight.c unprinted the whole time.
   Two brackets are enough to split it three ways: thinkers (one bracket per tic) and P_CheckSight
   (accumulated per call, it is called from inside the thinkers), leaving `T - thinkers` as the
   rest.  Raw FRT ticks -- the PLATFORM converts and resets, because only it knows the window's
   frame count and `T` is a per-frame mean it must be comparable to ([[budget-before-mechanism]]:
   write the subtraction before naming a cause).
   ⚠ Individual brackets are short, so the (unsigned short) FRT deltas cannot wrap (292 ms bound);
   the ACCUMULATORS are unsigned int and hold a full second (223 600 ticks) with room to spare.

   🔴 2026-08-16, SECOND HARDWARE VIDEO -- `th` EXCEEDED row-1 `T` BY 50 % BELOW 4,3 fps, which is
   impossible for a subset.  Row-1 `T` comes from `d_ms()` (DG_GetTicksMs) and it SATURATES at
   72-73 ms across three different frame rates (5,2 / 3,9 / 4,3 fps) while `th` keeps climbing
   48 -> 70 -> 106 -> 110.  A quantity that stops moving while the work grows is a clock artefact,
   not work.  So do not compare across clocks: RP_TicBegin/End brackets the WHOLE TryRunTics with
   THE SAME FRT the thinkers use, and row 24 prints its own `T`.  Row-1 `T` stays as the
   cross-check -- when the two disagree, believe the FRT one and suspect d_ms.
   (Same disease as every unit error this session: compare only within one clock.) */
/* SATURN 2026-08-17: `T` left row 24 on 08-16 (T - th was 2-4 ms on every capture, i.e. the game
   tic IS the thinkers).  The accumulator and its bracket are kept but the reads are gone; they cost
   the TLSF pool 1:1, so if the pool ever binds again this is the next thing to delete outright. */
/* sat_tic_total_frt REMOVED 2026-08-26.  The comment above already said it: "the accumulator and
   its bracket are kept but the reads are gone ... if the pool ever binds again this is the next
   thing to delete outright."  Two FRT reads per tic for a value nobody read.  sat_tic_think_frt
   and sat_tic_sight_frt below STAY -- row 24 prints both. */
unsigned int sat_tic_think_frt = 0;
unsigned int sat_tic_sight_frt = 0;
static unsigned short tic_think_t0, tic_sight_t0;
/* SATURN 2026-08-16 -- TICS PER FRAME.  `th` is a PER-FRAME mean, so it conflates two different
   things: how long one tic's thinkers take, and HOW MANY TICS landed in this frame.  The maketic
   cap is +8 ([[gametic-slowmotion-tic-cap]]), so at 3,6 fps up to EIGHT tics run inside one frame
   -- and a frame that runs more tics is slower, which makes the next frame run more tics still.
   Without this counter `th111` at 3,6 fps and `th18` at 16,5 fps cannot be compared at all: they
   may be the same cost per tic.  One increment per P_RunThinkers call = one per tic, exactly. */
unsigned int sat_tic_runs = 0;
/* SATURN 2026-08-21: LAST tic's P_RunThinkers cost in tenths of a ms -- the measured input of
   the TIC governor (p_tick.c decimation law).  One RP_Think bracket = exactly one tic, so this
   is already per-tic; no frame-sum ambiguity (the row-23 lesson). */
int sat_think_last10 = 0;
/* SATURN 2026-08-25: one tic = one sample-phase step, and the lazy one-shot calibration of
   the probe itself.  Both live in the THK block below; this is the forward hook. */
static void thk_tic_begin (void);
extern int thk_tic_sampled;              /* defined with the THK block below */
void RP_ThinkBegin (void) { sat_tic_runs++; thk_tic_begin (); tic_think_t0 = rp_frt(); }
void RP_ThinkEnd   (void)
{
    unsigned short d = (unsigned short)(rp_frt() - tic_think_t0);
    sat_tic_think_frt += d;
    /* SATURN 2026-08-25 round 6: bank the SAMPLED tic's own total, so row 23's `w` subtracts
       terms that all describe THE SAME TICS.  See the THK block below -- mixing a sampled
       estimate with an exact total is what made the first `w` unreadable. */
    if (thk_tic_sampled) sat_thk_th_frt += d;
    sat_think_last10 = (int)((unsigned)d * 10u / 224u);
}
void RP_SightBegin (void) { tic_sight_t0 = rp_frt(); }
void RP_SightEnd   (void) { sat_tic_sight_frt += (unsigned short)(rp_frt() - tic_sight_t0); }
/* SATURN 2026-08-17 -- DECOMPOSE THE THINKERS (row 23 `THK`).  Hardware reads `th` at 24-38 ms on
   178-200 ms frames -- a fifth of the frame that is not rendering and that nobody has looked at all
   session.  `s` (P_CheckSight's BSP walk) already carves one piece out; these carve the rest:
     mo = P_MobjThinker, summed over the tic -- actors moving/animating
     mv = P_CheckPosition + P_TryMove inside it, the BLOCKMAP walk that is Doom's classic hot spot
     ph = P_XYMovement / P_ZMovement;  sm = P_SetMobjState + every action function
     n  = thinkers actually RUN, so a big `th` can be read as "many" or as "expensive"
   `th - mo - sc` is then the bare list walk.  The platform converts and resets.

   ROUND 5, 2026-08-25 -- THE INSTRUMENT BECAME A MEASURABLE FRACTION OF WHAT IT MEASURES.
   The 2026-08-21 captures read `n` 2870-2905 mobj thinker calls per frame.  At two rp_frt() per
   call -- and rp_frt is not free: it masks SR, does TWO byte reads in the on-chip register area
   (uncached, several cycles each) and restores SR -- the probe alone plausibly accounts for a
   large slice of the ~24 ms `th - mo - sc` residual, and inflates every `mo` reading with it.
   An A/B on `mo` would then be measuring the probe.  Two changes close that:

     1. SAMPLING -- BY TIC, NOT BY CALL.  Round 5 timed every 8th mobj thinker.  Ymir 2026-08-25
        showed why that was wrong: `w` (th - mo - sc - pt) read 18 ms of a 26 ms `th` the moment
        three monsters woke up, while `mv0 s0 sm3` said the thinkers had done almost nothing --
        and `w` had barely moved when the LIST itself grew.  A term that does not scale with the
        list cannot be the list walk.  The defect is structural: **`th` was EXACT and `mo` was an
        ESTIMATE, and I subtracted one from the other.**  With only a handful of expensive actors
        in a 375-thinker list, a 1-in-8 CALL sample has enormous variance, and every miss lands in
        `w` -- which then reads as a discovery instead of as noise.
        Now a whole TIC is sampled or not: on a sampled tic EVERY bracket fires (mobj, ph, sm, mv
        AND the sector thinkers) and RP_ThinkEnd banks that tic's own `th` into sat_thk_th_frt.
        Row 23 derives `w` from terms that all describe THE SAME TICS, and one scale factor
        (sat_tic_runs / sat_thk_tics) lifts the whole set to a per-frame mean.
        RP_THK_SAMPLE is 4, not 8: the remaining variance is tic-to-tic, so what matters is the
        NUMBER OF SAMPLED TICS per 1 s window (~6 at console frame rates), not the call count.
        WARNING: every THK term is now an ESTIMATE from those few tics.  Cross-check on the
        photograph: `mo + pt + w` must land near row 24's `th`, which is exact.
     2. CALIBRATION.  thk_frt() counts its own calls, and rp_frt_calibrate() times 512 of them
        back to back, ONCE, so the platform can print `pt` -- the probe's cost in ms, MEASURED on
        the console instead of estimated from a cycle count.  `w` (the list walk) is then
        th - mo - sc - pt, with the instrument subtracted out.
        (The calibration loop's own overhead is counted into the cost, so `pt` slightly
        OVER-estimates the read alone -- the conservative direction for a number used to subtract.)

   RETIRED THE SAME DAY -- `sb` / `bt` (R_PointInSubsector and the THINGS blockmap loop inside
   P_CheckPosition).  They were added on 2026-08-17 when `mv` was 66-75 ms, the biggest single item
   in the frame.  The level-structs merge (line_t 64->24, side_t 20->16, mobj slab) collapsed `mv`
   to 1-3 ms on the same class of scene, so two probe pairs per P_CheckPosition were splitting a
   term smaller than their own cost.  They ANSWERED; rebuilding the split is ten lines the day `mv`
   climbs again.

   Resolution: one FRT tick is 4,47 us, so a bracket around a ~15 us call carries one tick of
   truncation either way.  It averages out over the samples summed per frame; a PER-CALL figure
   derived from these is good to ~20 %, not better. */
#define RP_THK_SAMPLE   4u      /* fully time 1 TIC in 4 (power of two: the mask below) */
unsigned int sat_thk_mobj_frt = 0, sat_thk_move_frt = 0, sat_thk_n = 0;
unsigned int sat_thk_tics = 0;           /* tics fully timed -- the scale DENOMINATOR */
unsigned int sat_thk_th_frt = 0;         /* P_RunThinkers total over THOSE tics only */
unsigned int sat_thk_frt_calls = 0;      /* thk_frt() calls made inside the tic -> `pt` */
unsigned int sat_thk_frt_cost_x256 = 0;  /* measured cost of ONE thk_frt(), in 1/256 FRT ticks */
static unsigned short thk_mo_t0, thk_mv_t0;
static unsigned int   thk_tic_phase = 0;
int                   thk_tic_sampled = 0;  /* 1 for the WHOLE of a sampled tic (RP_ThinkEnd reads it) */
static int            thk_in_mo = 0;        /* 1 while a mobj thinker is on the stack, sampled tics only */

/* Every timer read on the thinker path goes through here, so `pt` is an exact count and not an
   assumption about how many brackets fired. */
static unsigned short thk_frt (void) { sat_thk_frt_calls++; return rp_frt(); }

/* Measure thk_frt() against itself, once.  512 calls put a round at a few hundred FRT ticks --
   far enough above the 1-tick quantum to mean something, far below the 65535 wrap.
   THREE ROUNDS, KEEP THE MINIMUM.  rp_frt masks interrupts across its own H/L pair but not
   between calls, so a vblank landing inside a round adds the whole ISR to that round -- a ~0,8 ms
   round at 60 Hz catches one about 5 % of the time, and one hit inflates the estimate by >10 %.
   The minimum of three is the round that ran clean.  This matters: `pt` is SUBTRACTED to get `w`,
   so an inflated cost silently invents a smaller list walk.
   The sink keeps the compiler from folding away the value composition that the real probe pays. */
static volatile unsigned short thk_frt_sink;
static void rp_frt_calibrate (void)
{
    unsigned int saved = sat_thk_frt_calls;
    unsigned int best  = 0xffffffffu;
    int r, i;
    for (r = 0; r < 3; r++)
    {
	unsigned short t0, t1, sink = 0;
	unsigned int   d;
	t0 = rp_frt();
	for (i = 0; i < 512; i++) sink = (unsigned short)(sink + thk_frt ());
	t1 = rp_frt();
	thk_frt_sink = sink;
	d = (unsigned int)(unsigned short)(t1 - t0);
	if (d < best) best = d;
    }
    sat_thk_frt_calls = saved;           /* the calibration must not bill itself to the tic */
    sat_thk_frt_cost_x256 = (best * 256u) / 512u;
    if (!sat_thk_frt_cost_x256) sat_thk_frt_cost_x256 = 1u;   /* never re-run on a 0 reading */
}

/* Called once per tic from RP_ThinkBegin, BEFORE its own bracket opens, so neither the
   calibration nor the phase step is billed to `th`. */
static void thk_tic_begin (void)
{
    if (!sat_thk_frt_cost_x256) rp_frt_calibrate ();
    thk_tic_sampled = ((++thk_tic_phase & (RP_THK_SAMPLE - 1u)) == 0u);
    if (thk_tic_sampled) sat_thk_tics++;
    thk_in_mo = 0;                /* no bracket can survive a tic boundary */
}


void RP_ThkMobjBegin (void)
{
    sat_thk_n++;                  /* ALWAYS: `n` is a population, not a sample */
    if (thk_tic_sampled)
    {
	thk_in_mo = 1;
	thk_mo_t0 = thk_frt ();
    }
}
void RP_ThkMobjEnd (void)
{
    if (thk_in_mo)
    {
	sat_thk_mobj_frt += (unsigned short)(thk_frt () - thk_mo_t0);
	thk_in_mo = 0;
    }
}
/* P_CheckPosition is ALSO reached from P_PlayerThink, outside any mobj bracket.  Gating on
   thk_in_mo (not on the tic flag) keeps `mv` a strict subset of `mo`, which is what the row has
   always claimed -- the player's own P_TryMove is not an actor thinker. */
void RP_ThkMoveBegin (void) { if (thk_in_mo) thk_mv_t0 = thk_frt (); }
void RP_ThkMoveEnd   (void) { if (thk_in_mo) sat_thk_move_frt += (unsigned short)(thk_frt () - thk_mv_t0); }

unsigned int sat_thk_phys_frt = 0, sat_thk_state_frt = 0, sat_thk_sect_frt = 0;
unsigned int sat_r_setup_frt = 0;
static unsigned short thk_ph_t0, thk_sm_t0, thk_sc_t0, r_su_t0;
static int            thk_ph_depth = 0, thk_sm_depth = 0, thk_sc_depth = 0;
void RP_ThkPhysBegin  (void) { if (thk_in_mo && !thk_ph_depth++) thk_ph_t0 = thk_frt (); }
void RP_ThkPhysEnd    (void)
{
    if (thk_in_mo && --thk_ph_depth <= 0)
    { thk_ph_depth = 0; sat_thk_phys_frt += (unsigned short)(thk_frt () - thk_ph_t0); }
}
void RP_ThkStateBegin (void) { if (thk_in_mo && !thk_sm_depth++) thk_sm_t0 = thk_frt (); }
void RP_ThkStateEnd   (void)
{
    if (thk_in_mo && --thk_sm_depth <= 0)
    { thk_sm_depth = 0; sat_thk_state_frt += (unsigned short)(thk_frt () - thk_sm_t0); }
}
/* Sampled on the SAME tics as the rest: `w` subtracts `sc`, so it has to describe the same tics.
   (Round 5 left this one exact, which was half of why `w` did not add up.) */
void RP_ThkSectBegin  (void) { if (thk_tic_sampled && !thk_sc_depth++) thk_sc_t0 = thk_frt (); }
void RP_ThkSectEnd    (void)
{
    if (thk_tic_sampled && --thk_sc_depth <= 0)
    { thk_sc_depth = 0; sat_thk_sect_frt += (unsigned short)(thk_frt () - thk_sc_t0); }
}
/* The one term OUTSIDE the game tic: R_RenderPlayerView's pre-BSP setup (the slave-clear join,
   R_SetupFrame, R_PostFlatCacheFrame).  Row 1 `R` is DERIVED (MST - T - S - b - dg) while
   Bw/Bp/P/M are measured, and the two differ by ~11,6 ms.  This says whether that is a real phase
   or just the slop of a derived number -- the only honest way to close the last gap. */
void RP_RSetupBegin   (void) { r_su_t0 = rp_frt(); }
void RP_RSetupEnd     (void) { sat_r_setup_frt += (unsigned short)(rp_frt() - r_su_t0); }
static unsigned short prof_begin, prof_recend, prof_wait;
/* SATURN PERF 2.4 Stage 0: split REC into BSP / planes / masked sub-times to
   find which generation phase dominates REC (decides what to offload).  Marks:
   prof_begin (start, RP_BeginFrame) -> prof_bsp_end (RP_MarkBSPDone, after the
   BSP walk) -> prof_planes_end (RP_BeginMasked, after R_DrawPlanes) -> prof_recend
   (RP_EndFrame, after R_DrawMasked gen). */
static unsigned short prof_bsp_end, prof_planes_end;
/* SATURN PERF 2.4 Stage 1: time spent inside R_StoreWallRange (wall generation)
   accumulated across the BSP walk.  prof_wallprep is a subset of B, so the pure
   BSP traversal = B - prof_wallprep.  prof_wp_t0 = enter timestamp. */
/* SATURN 2026-08-17: EXPORTED (was static).  The owner is right that Bp cannot be fill --
   R_StoreWallRange runs for VDP1 walls too -- so the split between per-seg SETUP and the
   per-column LOOP is now the number that picks the lever.  Row 4 prints all three in ms. */
unsigned int   prof_wallprep;
static unsigned short prof_wp_t0;
/* SATURN 2026-08-25 -- THE UNNAMED HOLE INSIDE Bp.  prof_wallprep brackets ALL of
   R_StoreWallRange; prof_segrout + prof_segloop bracket only R_RenderSegLoop.  hd = enter ->
   RP_SegLoopEnter; tl = RP_SegLoopLeave -> leave.  hd+pr+lp+tl == wp by construction, so any
   residual on row 4 is bracket overhead (each is an upper bound by one FRT read) or the
   drawseg-overflow early return.  prof_tl_armed keeps that early return out of `tl`. */
static unsigned int   prof_wallhead;   /* row 4 `hd` */
static unsigned int   prof_walltail;   /* row 4 `tl` */
static unsigned short prof_tl_t0;
static unsigned char  prof_tl_armed;
/* SATURN PERF Phase-0a: finer Bp/P sub-splits (each a subset of Bp or P). */
/* SATURN 2026-08-17: EXPORTED (was static).  The owner is right that Bp cannot be fill --
   R_StoreWallRange runs for VDP1 walls too -- so the split between per-seg SETUP and the
   per-column LOOP is now the number that picks the lever.  Row 4 prints all three in ms. */
unsigned int   prof_segloop;     /* R_RenderSegLoop's PER-COLUMN loop only, c Bp (see RP_SegRoutMark) */
/* SATURN 2026-08-17: EXPORTED (was static).  The owner is right that Bp cannot be fill --
   R_StoreWallRange runs for VDP1 walls too -- so the split between per-seg SETUP and the
   per-column LOOP is now the number that picks the lever.  Row 4 prints all three in ms. */
unsigned int   prof_segrout;     /* R_RenderSegLoop's per-seg routing preamble, c Bp                  */
/* 🔴 SATURN 2026-08-17 -- SIZE `lp` BEFORE NAMING ITS CAUSE.  Hardware refuted the decision rule
   written on row 4 four hours earlier: `pr` was supposed to dominate and it does not -- four
   captures read `pr 9,9-12,4` against `lp 85,1-105,0`.  The per-column loop owns Bp, and the
   comment's own arithmetic then says something is impossible: ~320-600 wall columns cannot cost
   100 ms in adds.  So COUNT, do not guess.  These are increments only (no FRT read, no call), so
   they cannot inflate what they measure:
     prof_seg_cols = column iterations of R_RenderSegLoop
     prof_seg_fill = colfunc() calls made from inside it (the 3 tiers + the lead-fill's own)
     prof_seg_px   = pixels those calls wrote
     prof_lead_px  = the share of prof_seg_px that belongs to the LEAD-FILL
   THE SUBTRACTION THE ROW EXISTS FOR: R_DrawColumn costs ~7 cycles/pixel = ~0,25 us at 28,6 MHz,
   so a fill-bound `lp` of 100 ms means ~400 000 pixels -- twelve times the whole 160x200 M7 screen.
   If `k` comes back far below that, `lp` is NOT fill and no amount of flattening will move it. */
unsigned int   prof_seg_cols, prof_seg_fill, prof_seg_px, prof_lead_px;
static unsigned short prof_sl_t0;
/* Bp sub-split as PERCENTAGES, latched on the frame that set the window's Bp peak (row 20 `BP`). */
static unsigned int   prof_bp_r_pct, prof_bp_c_pct, prof_bp_g_pct, prof_bp_g_n, prof_bp_g_ms;
static unsigned int   prof_bp_g_x;    /* worst SINGLE R_GetColumn call on the PK-Bp frame, in US */
static unsigned int   prof_bp_g_e, prof_bp_g_a, prof_bp_g_k;  /* its parts, in MS.  `z` (slot 3,
                                                                  Z_Malloc) removed 2026-08-26 with
                                                                  its bracket -- see z_zone.c. */
static unsigned int   prof_bp_g_q, prof_bp_g_c;   /* SATURN 2026-08-17: garde / W_CacheLumpNum */
unsigned int          sat_bp_zw;     /* zone blocks walked on the PK-Bp frame (overlay row 22 `zw`) */
static int            prof_bp_bad;   /* 1 = a ratio came out impossible -> row prints `B!` */
/* SATURN 2026-08-08: R_GetColumn's own share of Bp -- the PER-COLUMN half of the ~60% that the
   pad L+X mode-2 A/B attributed to texturing (that A/B removed the per-column fetch AND the
   per-pixel texel read together).  prof_in_wp gates the accumulation to calls made from inside
   R_StoreWallRange: R_GetColumn is also called for the sky column (r_plane.c, billed to P) and
   the masked mid-texture (r_segs.c:181, billed to M), and counting those against a prof_wallprep
   denominator would be a percentage of the wrong thing. */
static unsigned int   prof_getcol, prof_getcol_n;
static unsigned short prof_gc_t0;
/* SATURN 2026-08-14: `g`/`n` alone cannot separate "a few fat calls" from "every call slow", and
   both fit the hole because it is ONE equation.  `gx` = the LARGEST single R_GetColumn delta on
   the frame settles it with no model at all:
     gx ~ tens of ms  -> a handful of calls do real work (disc I/O is the only candidate left)
     gx ~ g/n         -> the cost is UNIFORM, which no 157-instruction body can produce, so `g`
                         is billing time that is not R_GetColumn's (ISRs inside the bracket).
   `gd` = calls that found the single-patch lump NON-RESIDENT, i.e. calls that WILL hit the disc
   (r_data.c:615).  Measured average CD load = 6-7 s / ~184 loads = ~33 ms, so gd x 33 ms is the
   disc budget of the frame, comparable to `g` directly. */
static unsigned short prof_gc_mx;       /* max single-call FRT delta this frame */
/* SATURN 2026-08-14 (round 2): `x46680 d0` answered it -- ~4 calls of 46,7 ms make the whole 182 ms
   `g`, and NONE of them touch the disc (`d0`, and `ld` moved +6 over the run).  46,7 ms also
   matches the 42 ms/composite that the 08-12 `b4` latch measured and that I threw away because a
   MODEL said 2,8-7,7 ms.  Two independent instruments, one number: R_GenerateComposite.
   ROUND 2 (`e0 a0 k2` against `x46769`) CLEARED THE COMPOSITE TOO: the whole build is 2 ms.  And it
   cleared it twice over -- `e` sat on the R_EnsureLookup INSIDE R_GenerateComposite, which always
   finds the directory resident because R_GetColumn rebuilt it one line earlier.  The slots now point
   at the sites that were never bracketed at all:
     e = R_EnsureLookup called from R_GetColumn itself (the R4 lazy directory rebuild)
     a = W_CacheLumpNum on the single-patch path (`d0` says its already-cached branch)
     k = the composite patch loop + R_DrawColumnInCache  (kept as a CONTROL: must stay ~2)
     z = the worst single Z_Malloc -- the rover scan that purges and coalesces as it walks, which
         sits under e and a both, and which `zw` does NOT see (zw counts only Z_CanAllocate /
         Z_LargestAllocatable).  Nothing has ever timed it; the 08-07 note retired a STEP counter.
   Each is the WORST single invocation on the frame, latched with `g`.  `x` bounds them all. */
/* SATURN 2026-08-17: 4 -> 6 slots.  4 = the single-patch GARDE (W_LumpResident + Z_CanAllocate),
   5 = W_CacheLumpNum on that same path.  Both were unbracketed, and row 16 proved the frame's cost
   is exactly there -- see the note at their call sites in r_data.c. */
static unsigned short prof_st_t0[6], prof_st_mx[6];
static unsigned int   prof_st_sum[6];   /* SATURN 2026-08-23: the SUM per sub-bracket (global, not peak) */
static int            prof_in_wp;
static unsigned int   prof_makespans;   /* R_MakeSpans walk + R_MapPlane span math c P */
static unsigned short prof_ms_t0;
/* SATURN 2026-08-19 (plane-fill accounting, VDP1-planes verdict probe): the MASTER's share of the
   worklist drain -- the TAS steal loop, the rp_plane_dead fallback, the timeout redraw and the
   r_plane.c master-only branch.  Row 5 `Pm` = (prof_mplane + prof_makespans)/224 ms; the slave
   twin `Ps` is slave_pbusy's per-frame delta (already accumulated, was never printed in ms). */
static unsigned int   prof_mplane;
static unsigned short prof_mp_t0;
/* [!] SATURN 2026-08-26 -- prof_flatres: THE FLAT-RESOLUTION SLICE OF `Pv`.
   This bracket existed once as RP_FlatCacheEnter/Leave and I DELETED IT EARLIER TONIGHT, with
   the note "two FRT reads per visplane for prof_flatalloc, which nothing has read".  Deleting
   the inputs is the right cure for a probe nobody wants; it is the WRONG one for a probe whose
   print was dropped by accident -- and this is that probe.  It is back, and it is DISPLAYED.
   What it separates, and why the split is the whole point: `Pv` (11.2 ms in 4p) is the visplane
   worklist build, and the slave is idle through all of it.  The part that RESOLVES THE FLAT
   allocates (W_CacheLumpNum / R_FlatCacheGet), so the second SH-2 can never run it.  The part
   after it -- lighting, planeheight, span setup -- is pure compute over independent visplanes,
   exactly the profile the plane work-steal already exploits.  `Pv - flat` IS the parallelisable
   budget, and if it is small the offload is not worth its mechanism. */
static unsigned short prof_fr_t0;
/* [!] SATURN 2026-08-26 -- prof_planescan: WHAT THE Q PROBE COSTS, next to what it reports.
   RP_PlanePixels is PURE INSTRUMENTATION -- a per-visplane, per-COLUMN rescan of top[]/bottom[]
   feeding row-8 `Q`, the piecewise-quad estimate for the VDP1-planes study.  It early-returns
   outside full-overlay mode, which means it runs on EXACTLY the frames the owner photographs,
   INSIDE the `Pv` bracket, and its own header has said so since it was written: "the historical
   'inflates P' tax, accepted until the planes verdict lands".
   That tax was contaminating the number `Pv` exists to report.  `Pv` now EXCLUDES it (subtracted
   at print time, not bracketed away silently), and row 8 prints `Q<n>/<ms>` so the probe declares
   its own price beside its own answer.  A probe that hides its cost inside the phase it is
   measuring is not a measurement, and no offload should ever be designed against one. */
static unsigned int   prof_planescan;
unsigned int          sat_q_scan10 = 0;   /* row 8 `Q<n>/<ms>`: the probe's own cost, tenths-ms */
static unsigned int   prof_flatres;
/* SATURN PERF (RBG0 candidate sizing): per-frame floor/ceiling FILL accounting.
   pix = total non-sky span pixels (the P fill workload); dom = the largest single
   (picnum,height) flat group's pixels (the RBG0 offload prize); n = non-sky
   visplane count.  pp_cur_* = the group currently being accumulated (visplanes
   arrive picnum-sorted, so a same-key run is contiguous). */
static unsigned int   prof_plane_pix;
static unsigned int   prof_plane_dom;
static unsigned int   prof_plane_n;
static unsigned int   prof_pp_cur_sum;
static int            prof_pp_cur_pic;
static int            prof_pp_cur_h;
/* SATURN 2026-08-19 (piecewise-quad plane probe v2, RESIDENT in the full overlay): quadn =
   straight RUNS >= RPQ_MINW cols of visplane silhouette (per-column top/bottom steps confined
   to two adjacent values) = the would-be VDP1 command count; quadpx = their pixels.  Surfaced
   as row 8 `Q<n>/<pct>`: runs / their share of all software plane pixels.  v1 (whole-plane
   one-quad test, armed by pad L+B) read Q0/0 everywhere -- silhouette corners; that zero was
   data, not a bug. */
static unsigned int   prof_plane_quadn;
int sat_plane_quad_n   = 0;             /* per-frame publish for the platform overlay (row 8) */
/* (sat_plane_quad_pct REMOVED 2026-08-26 -- row 8 prints `Q<n>` only; the percentage half lost
   its field and kept a per-plane-run accumulator alive to compute it.  sat_plane_quad_n stays.) */
/* SATURN 2026-08-19 (probe inc-1, "le budget avant le mécanisme"): the run count is an
   OPTIMISTIC bound -- an emitted run must be BANDED (FLOOR_HBAND rows, anti-swim) and TILED
   (a 64x64 flat cannot wrap on VDP1), so the real bill is sum(bands x u-tiles), not 1/run.
   Owner's level-1 TNT round: Q 1-23 runs / 77-99% pixel coverage -> the deciding numbers for
   increment 0 (emit the TOP-4 runs) are that set's REAL command bill + its pixel share.
   prof_q4_px/cmd = the frame's 4 largest runs by pixels + each one's banded-tile cost
   (FLOOR_HBAND/FLOOR_MAXTILES arithmetic, same as the removed ftex estimator).
   Row 8 `E<cmds>/<pct>`. */
static unsigned int   prof_q4_px[4];
static unsigned int   prof_q4_cmd[4];
int sat_plane_q4cmd = 0;                /* top-4 runs: real VDP1 command bill (banded+tiled) */
int sat_plane_q4pct = 0;                /* top-4 runs: % of all software plane pixels        */
/* SATURN (VDP1-floor inc-0): for each non-sky regular flat (= the surfaces that would
   be deported to VDP1 affine strips -- other-height floors + ceilings; the RBG0 view-
   sector floor is already `continue`d before RP_PlanePixels, so it is excluded), estimate
   the would-be VDP1 DISTORSP command count.  Model = bbox-clamped horizontal strips
   (the "overspill, masked by NBG1" approach): split the visplane's screen-y extent into
   FLOOR_HBAND-row bands; each band's flat (64x64) wraps every 64 texels across the bbox
   width -> tiles = u-span/64 + 1 quads.  vq = total; vq_dom = the pixel-dominant group's
   quads (RBG0's, subtracted when sat_vdp2_floor is off); vq_peak = monotonic worst case
   = the go/no-go number.  All compiled out unless RP_PROF. */
static unsigned int   prof_pp_cur_vq;     /* current (picnum,height) group's VDP1-quad estimate */
static unsigned int   prof_floor_vq;      /* this frame: total VDP1 floor/ceiling quad estimate */
static unsigned int   prof_floor_vq_dom;  /* the pixel-dominant group's quads (RBG0 would take it) */
static unsigned int   prof_floor_vq_peak; /* monotonic peak of the VDP1 candidate cost (go/no-go) */
/* Q2 probe: quads for INTERIOR surfaces only -- those NOT touching their near screen edge (bottom
   for a floor, top for a ceiling).  These are the bounded-depth patches (cheap + low-swim with
   coarse bands); the near/edge surfaces (the expensive, swim-prone ones) would stay software/RBG0. */
#define FLOOR_HBAND    16   /* screen rows per affine strip band (the Mode-7 strip granularity) */
#define FLOOR_MAXTILES 16   /* clamp on 64-texel u-tiles/band (the emitter would cap too) */
#endif

/* SATURN PERF 2.4 Stage 1: wall-prep timer, called by R_StoreWallRange (r_segs.c)
   on the master during the BSP walk.  A bare empty call unless RP_PROF.  Always
   defined so the shared core links on both ports regardless of the flag. */
void RP_WallPrepEnter(void)
{
#if RP_PROF
    if (sat_wallprep_slave) return;   /* runs on the SLAVE -> its FRT is a different clock; don't
                                         time it here (the flush cost comes from prof_wpwait). */
    prof_wp_t0 = rp_frt();
    prof_in_wp = 1;             /* gate R_GetColumn accounting to the wall-prep calls only */
    prof_tl_armed = 0;          /* SATURN 2026-08-25: the drawseg-overflow early return (r_segs.c
                                   :2428) leaves without ever reaching RP_WallTailMark -- disarm on
                                   entry so `tl` can never swallow a call that had no tail. */
#endif
}
void RP_WallPrepLeave(void)
{
#if RP_PROF
    if (sat_wallprep_slave) return;
    /* SATURN 2026-08-25: ONE FRT read closes both `tl` and `wp` -- reading it twice would bill the
       second read to wp and make hd+pr+lp+tl > wp, breaking the identity this row exists for. */
    unsigned short now = rp_frt();
    prof_in_wp = 0;
    if (prof_tl_armed) { prof_walltail += (unsigned short)(now - prof_tl_t0); prof_tl_armed = 0; }
    prof_wallprep += (unsigned short)(now - prof_wp_t0);
#endif
}

/* SATURN 2026-08-25 -- THE TWO MARKS THAT NAME THE HOLE INSIDE Bp.  Called from r_segs.c around
   the RP_SegLoopEnter/Leave pair, they split prof_wallprep four ways: hd (head: scale + texture
   resolution, silhouette setup, BOTH R_CheckPlane calls) / pr / lp / tl (tail: the four openings
   memcpy + the drawseg store).  Console medians of the unnamed remainder were 1p 2.5 / 2p 7.0 /
   3p 11.3 / 4p 11.2 ms -- 24 % of the 4p Bp.
   BOTH gates are load-bearing.  sat_wallprep_slave: under the L+R flush the prep runs on the OTHER
   SH-2, whose FRT is a different clock -- without this bail prof_tl_armed could latch on the slave
   and be consumed by a later master-side RP_WallPrepLeave, poisoning `tl` with a cross-CPU delta.
   sat_dbg_overlay_mode: matches RP_SegRoutMark/RP_SegLoopLeave so the whole row-4 partition lives
   on ONE gate and costs nothing when row 4 is not rendering.  Consequence to know: outside mode 0,
   prof_wallprep still accrues while hd/tl do not, so the identity holds only in mode 0 -- which is
   the only mode that prints it. */
void RP_WallHeadMark(void)
{
#if RP_PROF
    if (sat_wallprep_slave || sat_dbg_overlay_mode != 0) return;
    prof_wallhead += (unsigned short)(rp_frt() - prof_wp_t0);
#endif
}
void RP_WallTailMark(void)
{
#if RP_PROF
    if (sat_wallprep_slave || sat_dbg_overlay_mode != 0) return;
    prof_tl_t0 = rp_frt();
    prof_tl_armed = 1;
#endif
}

/* SATURN 2026-08-25 -- THE SOFTWARE SKY, PER VIEW.  The 4-quadrant HW-sky plan needs to know what
   the SOFTWARE sky costs in the views that do NOT carry NBG0, and nothing measured it: sat_sky_px
   counts PIXELS, and pixels cannot be converted to ms on this path -- R_DrawSkyColumn does a
   128-byte per-column memcpy plus the grain loop, so the px->ms factor swings 2-4x with geometry.
   Brackets the sky loop in R_DrawPlanes only; the HW-sky branch draws nothing and therefore reads
   0.0 BY CONSTRUCTION, which is exactly how a capture identifies the elected view.
   Gated on the overlay mode alone (the sky loop is master-side, inside R_DrawPlanes), so the
   shipped build pays nothing. */
static unsigned short prof_sky_t0;
void RP_SkyEnter(void)
{
#if RP_PROF
    if (sat_dbg_overlay_mode != 0) return;
    prof_sky_t0 = rp_frt();
#endif
}
void RP_SkyLeave(void)
{
#if RP_PROF
    extern unsigned int sat_sky_frt;
    if (sat_dbg_overlay_mode != 0) return;
    sat_sky_frt += (unsigned short)(rp_frt() - prof_sky_t0);
#endif
}

/* SATURN 2026-08-08: bracket R_GetColumn (core r_data.c wraps its body for this).  Costs two FRT
   reads per column, ~2% of a heavy frame, and that cost lands INSIDE the measurement -- so `g` is
   an UPPER BOUND, and `n` is printed beside it so the inflation stays auditable. */
/* 🔴 SATURN 2026-08-17 -- WHICH CALL SITE MAKES THE CALLS?  One capture reads `SEG c320 f0`
   beside `BP g98 n402`: 402 R_GetColumn calls resolving columns that NOTHING fills, for 98 ms,
   while every bracketed sub-site of the worst call reads ~0 (`a0 e1 k0 z0`).  The owner authorised
   cutting the useless resolutions; cutting them before knowing WHO makes them would be guessing,
   and guessing is what has cost this session two builds already.
   So bin every call by its caller.  UNGATED on purpose: `g`/`n` are gated on prof_in_wp, which
   hides the masked-midtexture path completely -- and that path is one of the three suspects.
     1 = the seg loop's own tier resolution (the one a VDP1-routed wall should not need)
     2 = R_StoreWallRange's routing preamble (R_WallPotatoColor walks a whole texture)
     3 = R_RenderMaskedSegRange (grates), billed to M and invisible in `g`
     0 = everything else (r_plane.c's sky column, ...) */
int sat_gc_site = 0;
unsigned int prof_gc_st[4], prof_gc_sn[4];
static unsigned short prof_gcs_t0;

void RP_GetColEnter(void)
{
#if RP_PROF
    prof_gcs_t0 = rp_frt();          /* site split: every call, gated or not */
    if (!prof_in_wp) return;
    prof_gc_t0 = rp_frt();
#endif
}
void RP_GetColLeave(void)
{
#if RP_PROF
    unsigned short d;
    {
        int s = sat_gc_site & 3;
        prof_gc_st[s] += (unsigned short)(rp_frt() - prof_gcs_t0);
        prof_gc_sn[s]++;
    }
    if (!prof_in_wp) return;
    d = (unsigned short)(rp_frt() - prof_gc_t0);
    prof_getcol += d;
    prof_getcol_n++;
    if (d > prof_gc_mx) prof_gc_mx = d;   /* SATURN 2026-08-14: few-fat-calls vs uniformly-slow */
#endif
}

/* SATURN 2026-08-14: three shared max-timers for the R_GenerateComposite split (row 20 e/a/k).
   Max, not sum: the question is "which third is the 46,7 ms call", and a sum over a frame that
   builds 4-7 composites cannot answer it. */
void RP_StampBegin(int slot)
{
#if RP_PROF
    if (!prof_in_wp || (unsigned)slot >= 6u) return;
    prof_st_t0[slot] = rp_frt();
#endif
}
void RP_StampEnd(int slot)
{
#if RP_PROF
    unsigned short d;
    if (!prof_in_wp || (unsigned)slot >= 6u) return;
    d = (unsigned short)(rp_frt() - prof_st_t0[slot]);
    if (d > prof_st_mx[slot]) prof_st_mx[slot] = d;
    /* SATURN 2026-08-26 -- ONLY SLOT 5 IS READ.  prof_bp_g_c (row-20 `c`) is the one consumer of
       this array; slots 0/1/2/4 were accumulated on every stamp and printed nowhere, and slot 4
       is on the per-column path.  A compare now replaces a literal-pool load + indexed load +
       add + store on every other stamp.  (No site stamps slot 3 at all any more -- the Z_Malloc
       bracket was removed earlier today.) */
    if (slot == 5) prof_st_sum[5] += d;
#endif
}

/* SATURN PERF Phase-0a fine split (per-seg / per-visplane brackets; profiler).
   Always defined so the shared core links on both ports; no-op unless RP_PROF. */
void RP_SegLoopEnter(void)   {
#if RP_PROF
    prof_sl_t0 = rp_frt();
#endif
}
/* SATURN 2026-08-08: split R_RenderSegLoop in TWO.  Called once, immediately before the
   per-column `for (; rw_x < rw_stopx; rw_x++)` loop: everything before it is the PER-SEG
   ROUTING PREAMBLE (the Saturn-added CPU/VDP1 tier decisions, hysteresis, clamp, perspective
   subdivision, lead-fill arming), everything after is vanilla's PER-COLUMN fill.  The two
   scale with completely different things -- segs vs screen columns -- so a single `segloop`
   number could not rank them, and Bp is now the whole frame (ld flat, disc silent). */
void RP_SegRoutMark(void)    {
#if RP_PROF
    if (sat_dbg_overlay_mode == 0) {
        unsigned short now = rp_frt();
        prof_segrout += (unsigned short)(now - prof_sl_t0);
        prof_sl_t0 = now;
    }
#endif
}
void RP_SegLoopLeave(void)   {
#if RP_PROF
    if (sat_dbg_overlay_mode == 0)
        prof_segloop += (unsigned short)(rp_frt() - prof_sl_t0);
#endif
}
/* (RP_FlatCacheEnter/Leave REMOVED 2026-08-26 -- two FRT reads per visplane for prof_flatalloc,
   which nothing has read since its row-4 field was folded away.) */
void RP_MakeSpansEnter(void) {
#if RP_PROF
    prof_ms_t0 = rp_frt();
#endif
}
void RP_MakeSpansLeave(void) {
#if RP_PROF
    prof_makespans += (unsigned short)(rp_frt() - prof_ms_t0);
#endif
}
void RP_FlatResEnter(void) {
#if RP_PROF
    prof_fr_t0 = rp_frt();
#endif
}
void RP_FlatResLeave(void) {
#if RP_PROF
    prof_flatres += (unsigned short)(rp_frt() - prof_fr_t0);
#endif
}
void RP_MPlaneEnter(void) {
#if RP_PROF
    prof_mp_t0 = rp_frt();
#endif
}
void RP_MPlaneLeave(void) {
#if RP_PROF
    prof_mplane += (unsigned short)(rp_frt() - prof_mp_t0);
#endif
}

/* SATURN sprite-cost profiler (SCU-DSP feasibility study, deliverable #1): isolate
   sprite PROJECTION (R_ProjectSprite -- folded into Bw during the BSP walk today)
   from sprite FILL (R_DrawVisSprite -- folded into M with masked walls today).  These
   are the two halves the DSP question hinges on: the DSP could only do the arithmetic
   PROJECTION, never the memory-bound FILL.  MASTER-side only (rp_frt clock, 224 ticks/
   ms).  With sat_masked_parallel=1 (ship) the fill number is the master's LEFT-HALF
   share; the slave draws the right half untimed here -- so total fill ~= 2x prof_spr_fill
   (or read the existing M row).  Projection is master-only always, so it is complete.
   All bodies compile out unless RP_PROF -> DoomJo and shipping builds link empty stubs. */
static unsigned int   prof_spr_fill;   /* accumulated FRT ticks, this frame (prof_spr_proj removed 2026-08-26) */
static unsigned short prof_spr_fl0;
static int            prof_spr_n, prof_spr_draw;       /* things projected / vissprites filled */
void RP_SprReset(void) {
#if RP_PROF
    prof_spr_fill = 0; prof_spr_n = prof_spr_draw = 0;
#endif
}
/* RP_SprProjEnter REMOVED 2026-08-26: its product, prof_spr_proj (`pj`), lost its field and had
   no reader -- RP_SprStats still writes *proj10 but the platform drops it.  The COUNT half
   (prof_spr_n, row-15 `n`) is live, so Leave stays. */
void RP_SprProjLeave(int nthings) {
#if RP_PROF
    prof_spr_n += nthings;
#else
    (void)nthings;
#endif
}
void RP_SprFillEnter(void) {
#if RP_PROF
    prof_spr_fl0 = rp_frt();
#endif
}
void RP_SprFillLeave(void) {
#if RP_PROF
    prof_spr_fill += (unsigned short)(rp_frt() - prof_spr_fl0);
    prof_spr_draw++;
#endif
}
/* getter for the platform overlay: tenths-ms (proj, fill) + counts (things, draws). */
void RP_SprStats(int *proj10, int *fill10, int *nproj, int *ndraw) {
#if RP_PROF
    *proj10 = 0;   /* `pj` retired 2026-08-26 -- the platform drops this out-param */
    *fill10 = (int)(prof_spr_fill * 10u / 224u);
    *nproj  = prof_spr_n;
    *ndraw  = prof_spr_draw;
#else
    *proj10 = *fill10 = *nproj = *ndraw = 0;
#endif
}

/* SATURN PERF (RBG0 candidate sizing, profiler).  Walk the visplane's span pixels
   (O(width) per visplane -- bounded by, and cheaper than, its R_MakeSpans cost) and
   fold them into the total + the running same-(picnum,height) group.  A key change
   finalises the finished group into the max.  Always defined; the whole body is
   compiled out unless RP_PROF, so a shipping build (RP_PROF 0) pays only an empty
   call.  Visplanes arrive picnum-sorted, so same-key runs are contiguous. */
#if RP_PROF
/* For the VDP1-floor quad estimate: the floor texel-step bases (r_plane.c) + view
   depth/slope.  Redundant-but-legal externs keep this profiler self-contained. */
extern fixed_t basexscale, baseyscale, viewz, yslope[];
extern int     sat_vdp2_floor;

/* Close one straight run of the piecewise-quad probe: count it (>= RPQ_MINW cols), then price
   it the way the emitter would pay -- FLOOR_HBAND bands over the run's own y-extent, u-tiles
   per band from the depth at the band's midline (same arithmetic as the legacy whole-plane
   estimator below, scoped to the run's bbox) -- and fold it into the frame's top-4-by-pixels
   set (prof_q4_*).  wcols = run width in columns. */
#define RPQ_MINW 8
static void rpq_close(int wcols, unsigned int rpix, int rymin, int rymax, int height)
{
    unsigned int tiles = 0u;
    fixed_t ph;
    int yb;
    if (wcols < RPQ_MINW) return;
    prof_plane_quadn++;
    ph = (height >= viewz) ? ((fixed_t)height - viewz) : (viewz - (fixed_t)height);
    for (yb = rymin; yb <= rymax; yb += FLOOR_HBAND)
    {
        fixed_t dist, xs, ys, axs, ays;
        unsigned int du, dv, span;
        int t;
        int ym = yb + (FLOOR_HBAND >> 1);
        if (ym > rymax) ym = rymax;
        if (ym < 0) ym = 0; else if (ym >= viewheight) ym = viewheight - 1;
        dist = FixedMul(ph, yslope[ym]);
        xs = FixedMul(dist, basexscale); axs = (xs < 0) ? -xs : xs;
        ys = FixedMul(dist, baseyscale); ays = (ys < 0) ? -ys : ys;
        du = ((unsigned int)axs >> 8) * (unsigned int)wcols >> 8;   /* texels across the run */
        dv = ((unsigned int)ays >> 8) * (unsigned int)wcols >> 8;
        span = (du > dv) ? du : dv;
        t = (int)(span / 64u) + 1;
        if (t > FLOOR_MAXTILES) t = FLOOR_MAXTILES;
        tiles += (unsigned int)t;
    }
    {   /* top-4 insertion by pixel mass (4 slots: increment-0's candidate set) */
        int i, j;
        for (i = 0; i < 4; i++)
            if (rpix > prof_q4_px[i])
            {
                for (j = 3; j > i; j--)
                { prof_q4_px[j] = prof_q4_px[j-1]; prof_q4_cmd[j] = prof_q4_cmd[j-1]; }
                prof_q4_px[i] = rpix; prof_q4_cmd[i] = tiles;
                break;
            }
    }
}
#endif
void RP_PlanePixels(int picnum, int height, int minx, int maxx,
                    const unsigned char *top, const unsigned char *bottom)
{
#if RP_PROF
    unsigned int pix = 0u, vq = 0u;
    /* SATURN 2026-08-19 v2 (owner: "pourquoi un toggle si la sonde ne change rien ?"): the QUAD
       probe below is RESIDENT in the full overlay -- one fused O(width) pass, byte reads only
       (~0.5-1 ms worst plane-heavy frame; the historical "inflates P" tax, accepted until the
       planes verdict lands, then this whole probe goes).  Only the LEGACY floor-sizer estimate
       (the FixedMul band loop, consumers = the cut FLT row) still hides behind sat_prof_planepix
       (pad L+B).  Overlay fps-only/off still skips everything: no measurement, no tax. */
    /* [!] OPT-IN SINCE 2026-08-26 -- IT COST 9.1 ms AND IT WAS MEASURING ITSELF.
       This scan went RESIDENT on 2026-08-19 on the reasoning "a pure probe should not need
       arming".  The reasoning was right and the price was never checked: made to declare its own
       cost, it read `Q30/9.1` against a `Pv` of 11.1 -- 82 % of the visplane-build phase this
       project spent an evening trying to explain was the overlay observing itself, and ~9 ms of
       row-2 `P` in 4p has been observer cost on EVERY capture ever taken.  The real build is
       2.8 ms, 0.8 of it allocating, which killed the two-CPU split it was about to justify: you
       do not TAS 2 ms, you delete 9.
       Not deleted, GATED -- the VDP1-planes study it feeds is not closed in writing, so it keeps
       its answer behind its own switch instead of billing every frame for it.  One flag, one
       subject: sat_prof_planepix now means "run the plane rescan", legacy sizer included, and it
       already had the pad L+B binding.  Q/E read 0 until armed; that zero is the flag, not a
       measurement. */
    if (sat_dbg_overlay_mode != 0 || !sat_prof_planepix) return;
    unsigned short __ps_t0 = rp_frt();
    int x, ymin = 255, ymax = -1;
    /* v1 counted visplanes that are ONE screen quad (top[]+bottom[] linear over the whole span)
       and read Q0/0 on every owner capture.  ⚠ That zero was never a measurement: the publish
       sat in RP_EndFrame's rp_active block, which the shipping config (rp_disabled) never
       reaches -- found on the owner's THIRD all-zero round, fixed in rp_p3_prof_show.  The
       "silhouette corners" reading of those zeros was a rationalisation of a dead pipe; corners
       are real, but v1 was never actually tried.  v2 measures the better population anyway --
       what a VDP1 plane path would actually emit:
       maximal RUNS of columns whose top[] and bottom[] per-column steps each stay confined to
       two adjacent values {k,k+1} (= a rasterised straight line; a corner breaks the pair).
       Each run >= RPQ_MINW cols counts as ONE would-be VDP1 command (distorted sprite) into
       prof_plane_quadn, its pixels into prof_plane_quadpx (row 8 `Q<n>/<pct>`).  OPTIMISTIC by
       design (a gentle corner drifting < 1 px/col inside a run is missed): if even this upper
       bound is small, the VDP1-planes burial is final. */
    {
        int runx = -1;                    /* current run start col, -1 = closed */
        unsigned int rpix = 0u;           /* pixels inside the current run      */
        int pt = 0, pb = 0;               /* previous column's top/bottom       */
        int rymin = 0, rymax = 0;         /* the run's own screen-y bbox (tile pricing) */
        int tmin = 0, tmax = 0, bmin = 0, bmax = 0, seeded = 0;
        for (x = minx; x <= maxx; x++)
        {
            unsigned int t = top[x];
            unsigned int b = bottom[x];
            if (t != 0xffu && b >= t)
            {
                unsigned int span = b - t + 1u;
                pix += span;
                if ((int)t < ymin) ymin = (int)t;     /* visplane screen-y extent */
                if ((int)b > ymax) ymax = (int)b;
                if (runx < 0)
                { runx = x; rpix = span; seeded = 0; rymin = (int)t; rymax = (int)b; }
                else
                {
                    int st = (int)t - pt, sb = (int)b - pb;
                    if (!seeded) { tmin = tmax = st; bmin = bmax = sb; seeded = 1; }
                    else
                    {
                        if (st < tmin) tmin = st; else if (st > tmax) tmax = st;
                        if (sb < bmin) bmin = sb; else if (sb > bmax) bmax = sb;
                    }
                    if (tmax - tmin >= 2 || bmax - bmin >= 2)
                    {   /* corner at x: close [runx,x-1], reopen AT x */
                        rpq_close(x - runx, rpix, rymin, rymax, height);
                        runx = x; rpix = span; seeded = 0;
                        rymin = (int)t; rymax = (int)b;
                    }
                    else
                    {
                        rpix += span;
                        if ((int)t < rymin) rymin = (int)t;
                        if ((int)b > rymax) rymax = (int)b;
                    }
                }
                pt = (int)t; pb = (int)b;
            }
            else if (runx >= 0)           /* empty column: close [runx,x-1] */
            {
                rpq_close(x - runx, rpix, rymin, rymax, height);
                runx = -1;
            }
        }
        if (runx >= 0)
            rpq_close(maxx + 1 - runx, rpix, rymin, rymax, height);
    }
    /* VDP1-floor quad estimate (inc-0): bbox-clamped affine strips.  Split [ymin,ymax]
       into FLOOR_HBAND bands; per band the flat wraps every 64 texels across the bbox
       width -> tiles = u-span/64 + 1.  Mirrors what the emitter would produce.
       LEGACY sizer (cut FLT row consumers) -- the only part still behind pad L+B. */
    if (sat_prof_planepix && ymax >= ymin)
    {
        fixed_t ph = (height >= viewz) ? (height - viewz) : (viewz - height);
        int width = maxx - minx + 1;
        int yb;
        for (yb = ymin; yb <= ymax; yb += FLOOR_HBAND)
        {
            fixed_t dist, xs, ys, axs, ays;
            unsigned int du, dv, span;
            int ym = yb + (FLOOR_HBAND >> 1);
            int tiles;
            if (ym > ymax) ym = ymax;
            if (ym < 0) ym = 0; else if (ym >= viewheight) ym = viewheight - 1;
            dist = FixedMul(ph, yslope[ym]);
            xs = FixedMul(dist, basexscale); axs = (xs < 0) ? -xs : xs;
            ys = FixedMul(dist, baseyscale); ays = (ys < 0) ? -ys : ys;
            du = ((unsigned int)axs >> 8) * (unsigned int)width >> 8;   /* texels across width */
            dv = ((unsigned int)ays >> 8) * (unsigned int)width >> 8;
            span = (du > dv) ? du : dv;
            tiles = (int)(span / 64u) + 1;
            if (tiles > FLOOR_MAXTILES) tiles = FLOOR_MAXTILES;
            vq += (unsigned int)tiles;
        }
    }
    /* (v1 whole-plane NATURAL-QUAD test removed same day: silhouette corners make one-quad
       visplanes near-nonexistent -- Q0/0 on every capture.  The piecewise-run pass above is v2.) */
    prof_plane_pix += pix;
    prof_floor_vq  += vq;
    prof_planescan += (unsigned short)(rp_frt() - __ps_t0);
    /* (the interior/near-edge split REMOVED 2026-08-26: it ran per VISPLANE to feed
       prof_floor_vq_int_peak, whose only reader -- a FLAT snprintf -- was cut long ago.) */
    prof_plane_n++;
    if (picnum == prof_pp_cur_pic && height == prof_pp_cur_h)
    {
        prof_pp_cur_sum += pix;
        prof_pp_cur_vq  += vq;
    }
    else
    {
        if (prof_pp_cur_sum > prof_plane_dom)
        {
            prof_plane_dom    = prof_pp_cur_sum;
            prof_floor_vq_dom = prof_pp_cur_vq;
        }
        prof_pp_cur_pic = picnum;
        prof_pp_cur_h   = height;
        prof_pp_cur_sum = pix;
        prof_pp_cur_vq  = vq;
    }
#else
    (void)picnum; (void)height; (void)minx; (void)maxx; (void)top; (void)bottom;
#endif
}

/* (REMOVED 2026-08-26) per visible subsector, accumulate the would-be VDP1 quad cost if
   floors+ceilings were drawn as per-subsector quads (the PowerSlave model).  numlines = side
   count; nsurf = deportable surfaces (floor + non-sky ceiling).  pieces = quad-fan pieces for an
   numlines-gon (<=4 sides -> 1 quad; VDP1 draws a 4-gon as one distorted sprite).  GEOMETRY-only
   (untextured) -- texture tiling (bands x 64-wrap) would multiply it.  No-op unless RP_PROF; pure
   C, DoomJo-safe (DoomJo links it but never reads the globals). */

static void rp_finish(void)
{
    const rp_cmd_t *cmds=RP_CMDS;
    int i, mat, tot, ok, oend;
#if RP_DEBUG
    rp_t_rec=frt_now();
#endif
#if RP_PROF
    prof_wait = 0;      /* accumulate slave-wait ticks within this rp_finish */
#endif
    if (SYNC->ready!=rec_count)
    {
        __asm__ volatile("":::"memory");
        SYNC->ready=rec_count;
    }
    mat=(rec_masked_at>=0)?rec_masked_at:rec_count;
    tot=rec_count;
    SYNC->masked_at=mat;
    SYNC->total=tot;

    /* SATURN PERF 2.5: two-pointer EX drain.  Master draws parity-0 opaque FORWARD
       from 0, publishing m_pos; the slave draws parity-0 opaque BACKWARD from
       mat-1 (rp_slave_body), each stopping at the other's pointer.  Self-balancing
       -> the slower CPU covers fewer commands, W->0 without a static split.  i ends
       at the crossing = the first index the master did NOT draw. */
    {
        int end;
        i = 0;
        while (i < mat)
        {
            if (i >= SYNC->s_pos) break;          /* slave covers [s_pos, mat-1] */
            end = i + 16;                          /* claim a 16-cmd chunk so the */
            if (end > mat) end = mat;              /*  uncached SYNC touch is 1/16 */
            while (i < end) { rp_exec(&cmds[i],0,columnofs); ++i; }
            SYNC->m_pos = i - 1;                   /* publish: master drew [0, i-1] */
        }
        oend = i;
    }
#if RP_PROF
    { unsigned short w=rp_frt();
      ok=rp_wait(&SYNC->slave_opaque_done, RP_TO_WALL);
      prof_wait += (unsigned short)(rp_frt()-w); }
#else
    ok=rp_wait(&SYNC->slave_opaque_done, RP_TO_WALL);
#endif
    SYNC->go_masked=1;
    for (i=mat; i<tot; ++i) rp_exec(&cmds[i],0,columnofs);
#if RP_PROF
    if (ok) { unsigned short w=rp_frt();
              ok=rp_wait(&SYNC->slave_masked_done, RP_TO_WALL);
              prof_wait += (unsigned short)(rp_frt()-w); }
#else
    if (ok) ok=rp_wait(&SYNC->slave_masked_done, RP_TO_WALL);
#endif

    if (!ok)
    {
        /* Slave didn't signal done in time.  Draw everything on the master for
           THIS frame, but do NOT kill the parallel path on a single hiccup --
           re-arm next frame (RP_BeginFrame re-dispatches).  Only give up after
           several CONSECUTIVE timeouts (a genuinely wedged slave), so one
           transient/slow frame doesn't drop us to serial for the whole session. */
        for (i=0; i<tot; ++i) rp_exec(&cmds[i],1,columnofs);
        /* SATURN PERF 2.5: also cover the parity-0 opaque the slave was meant to
           draw backward [oend, mat) -- the master only drew forward [0, oend). */
        for (i=oend; i<mat; ++i) rp_exec(&cmds[i],0,columnofs);
#if RP_PROF
        {   /* row 20: where did the slave stall?  al=alive od=opaque-done
               ex=commands it drew  r/tot=ready/total.  al0=never started;
               al1 od0=hung in the opaque loop; ex stuck across frames=wedged. */
            static char t[44];
            snprintf(t, sizeof t, "TMO#%d al%d od%d ex%d r%d/%d   ",
                     rp_timeout_count, SYNC->slave_alive, SYNC->slave_opaque_done,
                     SYNC->slave_execs, SYNC->ready, tot);
            dbg_print(0, 5, t);   /* TMO overwrites the Bw/Bp row (now 5) on a timeout */
        }
#endif
        if (++rp_consec_timeouts >= 6)
        {
            rp_disabled=1;
            printf("r_parallel: slave SH-2 wedged, disabled after %d timeouts\n",
                   rp_consec_timeouts);
        }
    }
    else
    {
        rp_consec_timeouts = 0;   /* slave responded -- healthy again */
    }
    for (i=mat; i<tot; ++i)
        if (cmds[i].type==RP_FUZZ) rp_exec_fuzz(&cmds[i]);

    master_cache_purge();

#if RP_CDIAG
    {
        static char d[44];
        volatile unsigned char *mccr=(volatile unsigned char *)0xFFFFFE92;
        SYNC->master_ccr=*mccr;                 /* master's CCR (this CPU) */
        snprintf(d, sizeof d, "RPBAD  n%-5d t%d a%d b%d c%d   ",
                 SYNC->slave_bad, SYNC->bad_t, SYNC->bad_a, SYNC->bad_b, SYNC->bad_c);
        dbg_print(0, 15, d);
        snprintf(d, sizeof d, "CCR m%02x s0%02x s1%02x        ",
                 SYNC->master_ccr & 0xff, SYNC->slave_ccr0 & 0xff,
                 SYNC->slave_ccr1 & 0xff);
        dbg_print(0, 16, d);
    }
#endif

#if RP_DEBUG
    {
        static char dbg[41];
        rp_t_fin=frt_now();
        sprintf(dbg,"c%4d a%d p%5u r%5u f%5u",tot,
                (int)SYNC->slave_alive,
                (unsigned short)(rp_t_begin-rp_frt_entry),
                (unsigned short)(rp_t_rec-rp_t_begin),
                (unsigned short)(rp_t_fin-rp_t_rec));
        dbg_print(0, 2, dbg);
    }
#endif
}

static void rp_flush(void)
{
    rp_finish();
    if (!rp_disabled) rp_restart();
}

/* ------------------------------------------------------------------ */
/* Recorders                                                           */
/* ------------------------------------------------------------------ */

static rp_cmd_t *rp_alloc(void)
{
    if (rec_count==RP_MAX) rp_flush();
    return &RP_CMDS[rec_count];
}

static void rp_commit(void)
{
    __asm__ volatile("":::"memory");
    rec_count++;
    if ((rec_count&7)==0) SYNC->ready=rec_count;
}

static void RP_RecordColumn(void)
{
    if (rp_disabled) { if (detailshift) R_DrawColumnLow(); else R_DrawColumn(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_COL; cm->a=(short)dc_x; cm->b=(short)dc_yl; cm->c=(short)dc_yh;
    cm->src=dc_source; cm->cmap=(byte *)dc_colormap;
    cm->f1=dc_iscale; cm->f2=dc_texturemid;
    /* Potato walls: unused 0 = plain wall (-> solid colour), 1 = keep textured.
       Sprites (in_masked) and interactive walls (special lines: doors/switches,
       sat_wall_textured) stay textured so they remain readable. */
    cm->unused=(unsigned char)((in_masked || sat_wall_textured) ? 1 : 0);
    cm->f3=(sat_wall_paint & 2) ? 176 : sat_wall_color;
                                           /* Potato walls: dominant colour (opaque only).  DEBUG
                                              PAINT bit1 -> flat RED instead, so a CPU wall is
                                              unmistakable next to a green VDP1 one; one site
                                              because every sat_wall_color assignment funnels here.
                                              ⚠ THIS EXECUTOR IS DEAD in the shipping config
                                              (rp_disabled) -- the live solid-column path is
                                              sat_dc_solid in r_draw.c.  See that note. */
    rp_commit();
}

static void RP_RecordTrans(void)
{
    if (rp_disabled) { if (detailshift) R_DrawTranslatedColumnLow(); else R_DrawTranslatedColumn(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_TRANS; cm->a=(short)dc_x; cm->b=(short)dc_yl; cm->c=(short)dc_yh;
    cm->src=dc_source; cm->cmap=(byte *)dc_colormap;
    cm->f1=dc_iscale; cm->f2=dc_texturemid; cm->f3=(fixed_t)dc_translation;
    rp_commit();
}

static void RP_RecordFuzz(void)
{
    if (rp_disabled) { if (detailshift) R_DrawFuzzColumnLow(); else R_DrawFuzzColumn(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_FUZZ; cm->a=(short)dc_x; cm->b=(short)dc_yl; cm->c=(short)dc_yh;
    rp_commit();
}

static void RP_RecordSpan(void)
{
    if (rp_disabled) { if (detailshift) R_DrawSpanLow(); else R_DrawSpan(); return; }
    rp_cmd_t *cm=rp_alloc();
    cm->type=RP_SPAN; cm->a=(short)ds_y; cm->b=(short)ds_x1; cm->c=(short)ds_x2;
    cm->src=ds_source; cm->cmap=(byte *)ds_colormap;
    cm->f1=ds_xfrac; cm->f2=ds_yfrac; cm->f3=ds_xstep; cm->f4=ds_ystep;
    rp_commit();
}

/* ------------------------------------------------------------------ */
/* Frame hooks (extern "C" for r_main.c)                               */
/* ------------------------------------------------------------------ */

void RP_BeginFrame(void)
{
    /* SATURN 2026-08-16: the drawseg budget is PER VIEW, and this is the one call every view makes
       before its BSP walk -- so a split-screen player cannot inherit the previous view's spend.
       Outside RP_PROF on purpose: the budget is a shipping feature, not instrumentation. */
    sat_seg_count = 0;
    /* SATURN PERF 2.3: low-detail (detailshift!=0) now runs through the parallel
       path too (rp_exec dispatches to the *_low executors).  Previously this
       bailed to fully-serial master rendering, so low-detail had no working
       parallel mode at all. */
#if RP_PROF
    /* SATURN 2026-08-08: EVERY per-frame profiler accumulator resets HERE, ABOVE the rp_disabled
       branch.  It used to be duplicated in both branches, and that duplication rotted silently:
       the Phase-0a split (segloop/segrout) and the R_GetColumn counters were added to the LOWER
       copy only, while SHIPPING TAKES THE UPPER ONE (rp_disabled is true in the shipping config --
       [[rp-disabled-kills-flat-wall-modes]]).  They accumulated across the whole level against a
       per-frame prof_wallprep denominator, so every ratio pegged at 100% and read as a clean,
       confident finding.  It cost a capture round and a wrong entry in the overlay legend.
       ONE reset site, both paths.  Anything per-frame added below MUST go here. */
    prof_wallprep = 0;                                                   /* Bp accumulator */
    prof_segloop = prof_segrout = prof_makespans = 0;                    /* Phase-0a fine split */
    prof_wallhead = prof_walltail = 0; prof_tl_armed = 0;                /* row 4 `hd`/`tl` */
    prof_mplane = 0;                                                     /* row 5 `Pm` (master plane drain) */
    prof_flatres = 0;                                                    /* row 5 `Pv` 2nd half (flat resolve) */
    prof_seg_cols = prof_seg_fill = prof_seg_px = prof_lead_px = 0;      /* row 14 `SEG` -- lp sizing */
    prof_gc_st[0] = prof_gc_st[1] = prof_gc_st[2] = prof_gc_st[3] = 0;   /* row 16 `GCS` -- who calls */
    prof_gc_sn[0] = prof_gc_sn[1] = prof_gc_sn[2] = prof_gc_sn[3] = 0;
    prof_getcol = prof_getcol_n = 0;                                     /* R_GetColumn share of Bp */
    prof_gc_mx  = 0;                                                     /* worst single call (row 20 `x`) */
    prof_st_mx[0] = prof_st_mx[1] = prof_st_mx[2] = prof_st_mx[3] = 0;   /* row 20 e/a/k    */
    prof_st_mx[4] = prof_st_mx[5] = 0;                                   /* row 20 q/c      */
    prof_st_sum[5] = 0;   /* the only slot still accumulated -- see RP_StampEnd */
    { extern int z_walk_blocks; z_walk_blocks = 0; }                     /* zone blocks walked / frame */
    /* (r_lookup_rebuilds reset REMOVED 2026-08-10 with row-20 `e` -- see core/r_data.c:507) */
    prof_plane_pix = prof_plane_dom = prof_plane_n = 0;                  /* RBG0 candidate sizing */
    prof_plane_quadn = 0;                                                /* piecewise-quad probe (row 8 Q) */
    prof_planescan = 0;                                                  /* ... and what it costs (row 8, Pv) */
    prof_q4_px[0] = prof_q4_px[1] = prof_q4_px[2] = prof_q4_px[3] = 0u;  /* top-4 runs (row 8 E) */
    prof_q4_cmd[0] = prof_q4_cmd[1] = prof_q4_cmd[2] = prof_q4_cmd[3] = 0u;
    prof_pp_cur_sum = prof_pp_cur_vq = 0;
    prof_pp_cur_pic = -2147483647;   /* sentinel: no flat group open yet */
    prof_pp_cur_h   = 0;
    prof_floor_vq = prof_floor_vq_dom = 0;                               /* VDP1 floor estimate */
#endif
    if (rp_disabled) { rp_active=0;
#if RP_PROF
        p3_t_begin = rp_frt();   /* P3 profiler: frame start (parity rows are off here) */
#endif
        return; }
#if RP_DEBUG
    rp_t_begin=frt_now();
#endif
    rp_active=1; in_masked=0;
    saved_col=colfunc; saved_base=basecolfunc;
    saved_fuzz=fuzzcolfunc; saved_trans=transcolfunc; saved_span=spanfunc;
    colfunc=basecolfunc=RP_RecordColumn;
    fuzzcolfunc=RP_RecordFuzz;
    transcolfunc=RP_RecordTrans;
    spanfunc=RP_RecordSpan;
    rp_restart();
#if RP_PROF
    prof_begin = rp_frt();      /* recording starts now (slave runs in bg) -- the accumulators
                                   themselves were reset above the rp_disabled branch. */
#endif
}

/* SATURN PERF 2.4 Stage 0: called from R_RenderPlayerView right after the BSP
   walk, before R_DrawPlanes.  Captures the BSP/planes boundary for the row-20
   B/P/M breakdown.  No-op (and free) unless RP_PROF; always defined so the
   shared r_main.c can call it unconditionally on both ports. */
void RP_MarkBSPDone(void)
{
#if RP_PROF
    p3_t_bsp = rp_frt();                       /* P3: BSP done (unconditional) */
    if (rp_active && !rp_disabled) prof_bsp_end = p3_t_bsp;
#endif
}

void RP_MarkP(int slot)
{
#if RP_PROF
    if ((unsigned)slot < 2u) p3_t_p[slot] = rp_frt();
#else
    (void)slot;
#endif
}

void RP_BeginMasked(void)
{
#if RP_PROF
    if (rp_disabled) p3_t_planes = rp_frt();   /* P3: R_DrawPlanes done (parity path is off) */
    /* Attribute `P`.  Note the LAST slice (p3_t_planes - p3_t_p[2]) is the lead-fill JOIN plus the
       trailing NetUpdate/canary -- the join dominates it whenever the slave is late, which is
       exactly the failure mode a bounded spin produces: a huge outlier on an otherwise idle frame. */
    sat_p_kick10  = (unsigned short)(p3_t_p[0] - p3_t_bsp)  * 10u / 224u;
    /* [!] SATURN 2026-08-26 -- `Pv`: THE VISPLANE WORKLIST BUILD, the last unnamed slice of `P`.
       Slot 1 is stamped in r_plane.c immediately before RP_DrawPlanesSplit, so this is
       R_DrawPlanes from its entry to the dispatch: ~720 lines of flat resolution, lighting and
       span setup, PER VISPLANE, before either CPU writes a pixel.
       THE SUBTRACTION THAT MADE IT NECESSARY (4p, TNT, vp50/view): row 2 read P21.7 with a
       measured kick of 2.2, while row 5 read Pm8.0 / Ps7.1 -- and master and slave run those
       CONCURRENTLY with w0.0, so the fill costs max(8.0, 7.1) = 8.0, not 15.1.  21.7 - 2.2 - 8.0
       leaves ~11.5 ms that was neither fill nor wait: ~57 us per visplane of pure preparation.
       Read it as the CLOSING term: kick + Pv + max(Pm, Ps) must land on `P`. */
    sat_p_build10 = (unsigned short)(p3_t_p[1] - p3_t_p[0]) * 10u / 224u;
#endif
    if (!rp_active||rp_disabled) return;
#if RP_PROF
    prof_planes_end = rp_frt();   /* R_DrawPlanes done; masked gen starts next */
#endif
    in_masked=1; rec_masked_at=rec_count;
    __asm__ volatile("":::"memory");
    SYNC->ready=rec_count;
    SYNC->masked_at=rec_count;
}

/* SATURN PERF (2026-06-24): windowed REC min/avg/max + the CONTEXT of the worst
   frame, so a single overlay photo is meaningful no matter when it is snapped (the
   instantaneous Bw/Bp/P/M flicker every frame -> impossible to catch the peak).  All
   REC fields are tenths-of-ms; map/x/y/ang/t = the player's render pose at the REC max
   (= where/when to walk back to).  The platform resets the window (RP_ProfReset) when
   the config under test changes (map / potato / hash / blit).  Defined unconditionally
   so the platform extern links with RP_PROF off (they then stay 0). */
/* Windowed REC distribution via a bucketed histogram (-> p50/p95, robust to the single-
   outlier max and to an arbitrary threshold) + per-PHASE independent peaks (each phase's
   own worst across the window -- Bp and P do NOT peak on the same frame, so this is the
   right basis to size an offload) + the REC-max LOCATOR (map/x/y/ang/leveltime).  All
   defined unconditionally so the platform links them with RP_PROF off (then 0). */
#define RP_HBUCKETS  64
#define RP_HBWIDTH   20            /* tenths-ms per bucket = 2.0 ms; histogram covers 0..128 ms */
#define RP_REC_SANE  6000          /* tenths-ms.  🔴 2026-08-12: 3000 -> 6000, because at 300 ms this
                                      guard had started CENSORING THE SIGNAL and could never catch the
                                      artefact it was written for.
                                        THE ARITHMETIC: b10/p10/m10 are each ONE 16-bit FRT delta
                                      (`(unsigned short)(a - b) * 10u / 224u`, :1995-1997), so a wrapped
                                      delta cannot exceed 65535/224 = 292.6 ms -- it aliases DOWN, into
                                      the range the bound calls sane.  A 300 ms cut therefore rejects
                                      approximately zero glitches and an increasing number of REAL
                                      frames: the owner's TNT MAP11 capture read mx295.4, i.e. 1.5%
                                      under the clip, with d5 = five frames thrown away.  A maximum
                                      pinned just below its own censor is the signature of censoring,
                                      not of a physical ceiling -- and everything downstream (PK, MXd,
                                      the g/b latch, the histogram, the MX locator) lives inside the
                                      gate, so the whole profiler was reporting maxima over the
                                      SURVIVING population only.
                                        6000 keeps a net (the sum of three phases maxes at ~878 ms by
                                      the same wrap arithmetic, so 600 ms is still well inside the
                                      representable range) while clearing the 40-300 ms band the port
                                      actually operates in.  If a glitch class needs catching again,
                                      discriminate on the SIGNATURE the comment already named --
                                      impossible M, out-of-bounds MX -- not on a total-ms bound. */
int sat_prof_rec_max=0;            /* window max (= p100), tenths-ms */
int sat_prof_dropped=0;            /* glitch/transition frames excluded from the window */
/* SATURN 2026-08-24: the peak Bp of the CURRENT 1 s overlay window -- what row 20's sub-split
   describes.  Zeroed by the platform on its window tick, so the row follows the game instead of
   freezing on the map's post-load record.  Separate from sat_prof_pk_bp on purpose (row 4's peak
   is the A/B reference and must survive the whole toggle window). */
int sat_prof_bp_win=0;
/* SATURN 2026-08-15: the LOD governor's state lives in r_segs.c beside the knob it drives. */
extern int sat_lod_eff, sat_lod_auto_step, sat_gov_debt;
extern int sat_lead_mode, sat_wall_lead_x;   /* r_segs.c: the lead-fill's mode and depth */
int sat_gov_lead_step = 0;   /* 0 = as the pad asked, 1 = force FLAT spans, 2 = lead-fill off */
/* 🔴 SATURN 2026-08-17 -- THE GOVERNOR MUST PROVE ITS OWN LEVERS.  Owner: "le gouverneur a dégradé
   tous les murs cpu en flats et ne les a jamais remontés", and his captures show exactly that --
   `w3 p2 L1` with the debt STILL POSITIVE (e108, e191).  It is not a bug in the relaxation: it is a
   RATCHET.  The `w` rung costs real picture (flat CPU walls) but attacks fill that is already on
   VDP1 -- open scenes, mostly hardware, `Bp` unmoved -- so `rend` does not improve, so the debt
   never clears, so it degrades again.  An ineffective degradation is a one-way trap.
   So: after every fire, watch `rend` for GOV_PROBE frames.  If the step bought less than
   GOV_PROOF tenths of a ms, UNDO it and mark that axis INERT -- never elected again this level.
   The governor then spends its debt only on levers that have been observed to work. */
#define GOV_PROBE 24        /* frames to wait before judging a step            */
#define GOV_PROOF 80        /* 8,0 ms: less than this and the step bought nothing */
static int gov_pr_wait = 0, gov_pr_axis = 0; static unsigned int gov_pr_rend = 0;
int sat_gov_inert = 0;      /* bit0 = `w` axis proven inert, bit1 = `p`, bit2 = lead */
/* 🔴 SATURN 2026-08-17 -- AN INERT MARK IS A PAROLE, NOT A LIFE SENTENCE.  Owner: *"il faut lui
   rendre les leviers si on change de secteur.  Ce qui est vrai dans une partie de niveau ne l'est
   pas partout."*  He is right, and the first version had exactly the defect shape this project has
   now shipped three times: a FAILURE THAT LATCHES.  The `w` rung is inert in an open TNT courtyard
   because the walls are far and already on VDP1 -- move the same player into a close corridor and
   flattening segs is precisely what buys the frame back.  Freezing a scene-local verdict into a
   level-long one throws away the lever exactly where it would have worked.
   Crossing into a new SECTOR is the cheapest honest "the world changed" signal available: the game
   already maintains mo->subsector->sector, so it costs one compare per frame (r_main.c publishes
   it).  Re-arming is RATE LIMITED to once per GOV_RETEST frames, because a doorway crossed twice a
   second would otherwise put the governor straight back into the probe/degrade cycle the parole
   exists to end -- each retest costs GOV_PROBE frames of degraded picture, so the worst-case duty
   cycle of "degraded for nothing" is bounded at 24/256 ~= 9 %.  A level change clears everything
   outright: nothing measured on the old map is evidence about the new one. */
/* 🔴 SATURN 2026-08-17 -- INERT BY CONSTRUCTION BEATS INERT BY MEASUREMENT.  The ms probe needs 24
   frames and a >=8,0 ms signal to convict a rung, and on a 190 ms frame that signal is inside the
   noise.  But the owner's captures already convicted the `w` axis in ONE glance: `sb96/0` with
   `ds113-120` -- the budget was ARMED and flattened ZERO segs, because the cut also demands
   rw_distance > sat_lod_mindist and nothing in that scene qualified.  A rung that performs no
   ACTION cannot buy time, and that needs no statistics: count the actions, and if the count does
   not move at all in GOV_ACTWAIT frames, unwind and blacklist immediately.
   Cheap, fast (4 frames instead of 24), and it gives the picture back before the player sees it.
   Only axes with a real per-item action counter qualify -- `w` (tiers flattened) and the lead-fill
   (spans emitted).  The `p` axis changes a MODE rather than taking per-item decisions, so it has no
   such count and keeps the ms probe; that is a gap, not an oversight. */
#define GOV_ACTWAIT 4       /* frames to see at least ONE action from the rung just fired        */
unsigned int sat_gov_act_w = 0, sat_gov_act_l = 0;
/* SATURN 2026-08-24 -- THE `p` AXIS FINALLY HAS ONE, and the rung it counts is finally real.
   Two defects, one cause: the plane rung is applied as a FLOOR over the owner's own SQ
   (dg_saturn gov_sq[] + max()), and the shipped SQ is already `ld` for BOTH planes -- so rung 1
   (`ld`) was BIT-IDENTICAL to rung 0.  Every election of `P` spent a fire and 24 probe frames on
   a no-op, measured no gain, and the unwind then blacklisted the WHOLE axis -- taking rung 2
   (`flat`, a real memset instead of a textured span) with it.  So:
     - the election now JUMPS to sat_gov_p_min, the first rung that can actually raise the level
       above what the owner asked for (published by the platform, which owns gov_sq[]);
     - and sat_gov_act_p counts PLANES the governor actually degraded, so GOV_ACTWAIT can convict
       the axis when it truly does nothing -- which is the case in SPLIT, where sat_view_sq_apply
       rewrites the four flags per view WITHOUT the max() and the governor's floor never lands. */
unsigned int sat_gov_act_p = 0;
int sat_gov_p_min = 1;   /* platform-published: first p rung that changes a pixel (1 or 2) */
static unsigned int gov_pr_act0 = 0;
#define GOV_RETEST 256      /* frames a parole must ripen before the next sector change re-arms */
extern void *sat_view_sector;                                   /* r_main.c, identity token only */
extern int   gamemap;
static void *gov_last_sector = NULL;
static int   gov_last_map = -1, gov_retest_wait = 0;
extern int sat_gov_axis, sat_gov_p_step, sat_gov_p_dirty;
int sat_prof_mx_map=0, sat_prof_mx_x=0, sat_prof_mx_y=0, sat_prof_mx_ang=0, sat_prof_mx_t=0;
/* SATURN PERF (2026-07-09): full detail of the worst-REC frame, snapshotted at each new peak
   (persists until beaten / config change).  Phase split + slave b/Pb AT that frame. tenths-ms / %. */
int sat_prof_mx_bw=0, sat_prof_mx_bp=0, sat_prof_mx_p=0, sat_prof_mx_m=0, sat_prof_mx_b=0, sat_prof_mx_pb=0;
int sat_prof_dom_pct=0, sat_prof_plane_n=0;   /* RBG0-floor sizer: dominant-flat pixel share + visplane count */
#if RP_PROF
static unsigned int prof_w_recmax=0, prof_w_recn=0;
static unsigned int prof_hist[RP_HBUCKETS];
extern fixed_t viewx, viewy;
extern angle_t viewangle;
extern int     gamemap, leveltime;
#endif
/* Reset the windowed stats (the platform calls it when the variable under test changes).
   Unconditional body so Mimas links it even with RP_PROF off. */
void RP_ProfReset(void)
{
#if RP_PROF
    int i; prof_w_recmax=0; prof_w_recn=0;
    for (i=0;i<RP_HBUCKETS;i++) prof_hist[i]=0;
#endif
    sat_prof_rec_max=0;
    sat_prof_mx_map=sat_prof_mx_x=sat_prof_mx_y=sat_prof_mx_ang=sat_prof_mx_t=0;
    sat_prof_mx_bw=sat_prof_mx_bp=sat_prof_mx_p=sat_prof_mx_m=sat_prof_mx_b=sat_prof_mx_pb=0;
    sat_prof_dom_pct=sat_prof_plane_n=0;
    sat_prof_dropped=0;
}
/* Windowed REC percentile (pct 0..100) in tenths-ms, walked from the histogram (<=64
   buckets, called 1/s by the platform -> cheap).  Returns 0 with RP_PROF off. */
int RP_ProfPercentile(int pct)
{
#if RP_PROF
    unsigned int target = prof_w_recn * (unsigned)pct / 100u, cum = 0; int i;
    if (!prof_w_recn) return 0;
    for (i = 0; i < RP_HBUCKETS; i++) { cum += prof_hist[i]; if (cum >= target) return i * RP_HBWIDTH; }
    return (RP_HBUCKETS - 1) * RP_HBWIDTH;
#else
    (void)pct; return 0;
#endif
}

#if RP_PROF
/* P3 profiler readout (rows 18/20), used when the parity renderer is OFF (rp_disabled, the
   sat_plane_parallel config).  B = full BSP walk, P = plane phase (master half + the wait for
   the slave half), M = masked.  SLVp = the slave's plane-draw ms, w = master wait (imbalance:
   high w => the master's half was lighter and it idled), u% = slave busy as a share of P.
   VDP1 utilisation is dg_saturn row 16 (VD1 cmds + D/B). */
static void rp_p3_prof_show(void)
{
    unsigned short t_mask = rp_frt();
    unsigned int b10  = (unsigned short)(p3_t_bsp    - p3_t_begin)  * 10u / 224u;
    unsigned int p10  = (unsigned short)(p3_t_planes - p3_t_bsp)    * 10u / 224u;
    unsigned int m10  = (unsigned short)(t_mask      - p3_t_planes) * 10u / 224u;
    unsigned int w10;   /* master FRT -> ms (reliable); frame sum in split, assigned below */
    extern int sat_masked_parallel;
    unsigned int rend = b10 + p10 + m10;                /* render = B+P+M (tenths-ms) */
    /* the slave is busy during P, and during M too once masked-by-half is on -> it idles only in
       B+M (planes only) or B (planes+masked).  idle% DROPS as each phase is offloaded. */
    unsigned int sidle = b10 + (sat_masked_parallel ? 0u : m10);
    unsigned int idle = rend ? (sidle * 100u / rend) : 0u;
    /* split B: Bp = wall-prep (R_StoreWallRange, the part the slave could take, d32xr-style);
       Bw = the BSP walk + clip + sprite projection (inherently serial). */
    /* Bp = wall-prep cost wherever it ran: prof_wallprep when inline on the master, or the master's
       wait for the slave flush (prof_wpwait) when sat_wallprep_slave.  Bw = B minus that = the pure
       BSP walk in BOTH modes (so a wp0/wp1 A/B reads cleanly: Bp(wp1) > Bp(wp0) => slave slower). */
    unsigned int bp10 = (sat_wallprep_slave ? prof_wpwait : prof_wallprep) * 10u / 224u;
    unsigned int bw10 = (b10 > bp10) ? (b10 - bp10) : 0u;
    char p[44];
    /* row 2: render phase split Bw/Bp/P/M (the memory-bound generation breakdown).  Printed only
       in the full overlay (mode 0) -- this runs EVERY frame, so gating it is part of the fps-only
       overlay-cost measurement.  The histogram/peak folding below stays unconditional (cheap FRT).

       SATURN 2026-08-25 -- ROW 2 IS THE FRAME, NOT THE LAST VIEW.  This function runs once per
       VIEW, and each view overwrote row 2, so in split the photograph showed ONE QUADRANT while
       row 1's MST showed the whole frame.  Every 2p/3p/4p phase number this project has ever
       quoted is that last view multiplied by N in a spreadsheet -- docs/RESOURCE_BUDGETS.md 9.2
       calls it the single biggest hole in the cost model, and it is the denominator of the one
       constant that sizes half the roadmap (ms per drawseg, against row-11 `ds`).
       The governor five lines below has summed across views since 2026-08-21; this is the same
       law applied to the READOUT, and it reuses nothing of the governor's state on purpose (the
       governor drops glitched views from ITS sum -- a measurement must show them, not hide them).
       `n<v>` leads the row so a capture states how many views it is the sum of; 1p reads `n1` and
       is bit-identical to the old line.  Frame-sum vs MST is now a legal subtraction. */
    /* SATURN 2026-08-25 (2nd round): `last` is HOISTED to FUNCTION scope.  rp_p3_prof_show runs
       once per VIEW and FOUR row families now need to know whether this is the frame's final view
       -- rows 2 and 4 and the new 14/16 block below, plus rows 5 and 20 at the bottom of the
       function, which were still paying 4x in 4p.  It stays 1 in 1p, so every 1p row is
       bit-identical.  One mistake here silently desynchronises all four at once. */
    int last = 1;
    {
        extern int sat_split_active, sat_split_view, sat_local_players;
        static unsigned int r2_bw = 0, r2_bp = 0, r2_p = 0, r2_m = 0, r2_pk = 0;
        unsigned int s_bw = bw10, s_bp = bp10, s_p = p10, s_m = m10, s_pk = sat_p_kick10;
        int nv = 1;
        if (sat_split_active)
        {
            nv = sat_local_players; if (nv > 4) nv = 4; if (nv < 1) nv = 1;
            r2_bw += bw10; r2_bp += bp10; r2_p += p10; r2_m += m10; r2_pk += sat_p_kick10;
            last = (sat_split_view >= nv - 1);
            s_bw = r2_bw; s_bp = r2_bp; s_p = r2_p; s_m = r2_m; s_pk = r2_pk;
            if (last) { r2_bw = r2_bp = r2_p = r2_m = r2_pk = 0; }
        }
        else r2_bw = r2_bp = r2_p = r2_m = r2_pk = 0;   /* leaving split mid-window must not
                                                   inflate the first frame of the next one */
        if (s_pk > 9999u) s_pk = 9999u;
        /* SATURN 2026-08-25 -- THE DENOMINATOR, latched on the SAME law as the numerator.
           `Bp / ds` is the one console constant that sizes DECIM, the light-only pass, the
           cheap-seg rung and the FOV cut all at once -- and row-11 `ds` is r_drawseg_peak, a
           high-water of ONE VIEW (R_ClearDrawSegs folds per view).  Dividing a frame-sum Bp by a
           per-view ds is a category error that would have overstated the constant ~4x in 4p.
           Summed here rather than in R_ClearDrawSegs because THIS is the site that knows where a
           view ends: ds_p is still this view's when we run (before the next view's clear). */
        static int r2_ds = 0;
        {
            extern drawseg_t drawsegs[]; extern drawseg_t *ds_p;
            if (!sat_split_active) r2_ds = 0;   /* same rule as the phase sums above */
            r2_ds += (int)(ds_p - drawsegs);
        }
        if (sat_dbg_overlay_mode == 0 && last) {
            /* [!] SATURN 2026-08-26 -- `P` IS PRINTED SPLIT: P<total>/<kick>.  The second half is
               sat_p_kick10, which has been COMPUTED EVERY FRAME SINCE THE MarkP(0) probe was
               written and DISPLAYED NOWHERE -- the exact anti-pattern this project keeps finding:
               the inputs kept running, only the print was dropped.  r_main.c:1315 already says
               what it is in words: "Everything above this line is charged to P today but is not a
               plane" -- the wall-list flush, the VDP1 kick and, IN SPLIT, this view's
               R_EmitWorldThingsVDP1 (the kick itself is skipped in split, d_main fires one for all
               four views, but the per-view SPRITE emission is not).
               Why it was needed: row 5 reads Pm7.2 / Ps7.1 with w0.2, and master and slave run
               those CONCURRENTLY, so the plane phase should last ~max(7.2, 7.1) + join = ~7.5 ms.
               Row 2 read P22.6.  ~15 ms of "P" was neither master plane fill, nor slave plane
               fill, nor waiting -- and it was the largest unnamed block left on the critical path.
               Accumulated on the SAME law as bw/bp/p/m so a split photo is a FRAME sum. */
            snprintf(p, sizeof p, "n%d Bw%u.%u Bp%u.%u P%u.%u/%u.%u M%u.%u d%d ", nv,
                     s_bw/10,s_bw%10, s_bp/10,s_bp%10, s_p/10,s_p%10, s_pk/10,s_pk%10,
                     s_m/10,s_m%10,
                     r2_ds > 9999 ? 9999 : r2_ds);
            dbg_print(0, 2, p);
        }
        if (last) r2_ds = 0;
        /* SATURN 2026-08-25 -- ROW 4 (BPS) OBEYS THE SAME LAW, and the owner's own Ymir pass is
           what caught it: one 3p capture read `pr0.0 lp0.0 wp0.0` beside `Bp9.9`, because
           prof_segrout/segloop/wallprep are reset per VIEW in RP_BeginFrame and the LAST view
           that frame was a 1 ms sliver.  Another read `wp6.6` against a frame `Bp25.1` -- one
           quarter, exactly.  A row-4 that describes the last view while row 2 describes the frame
           cannot be differenced against it, which is the only reason row 4 exists. */
        /* SATURN 2026-08-25 (2nd round) -- `wp` RETIRED, `hd`/`tl` ADDED.  `wp` was prof_wallprep,
           i.e. LITERALLY row-2 `Bp` (:2636 reads the same variable), so it spent 7 of 40 columns
           restating a number one row above.  In its place, the two brackets that name the hole:
           console medians of (wp - pr - lp) were 1p 2.5 / 2p 7.0 / 3p 11.3 / 4p 11.2 ms -- 24 % of
           the 4p `Bp` with no name on it.  `hd` = the head of R_StoreWallRange (scale + texture
           resolution, silhouette setup, BOTH R_CheckPlane calls); `tl` = the tail (the four
           openings memcpy + the drawseg store).  hd+pr+lp+tl == Bp is now an IDENTITY and is the
           first thing a post-fix console pass must check before reading anything into hd/tl. */
        {
            static unsigned int f_pr = 0, f_lp = 0, f_hd = 0, f_tl = 0;
            if (!sat_split_active) f_pr = f_lp = f_hd = f_tl = 0;
            f_pr += prof_segrout; f_lp += prof_segloop;
            f_hd += prof_wallhead; f_tl += prof_walltail;
            if (last)
            {
                sat_bps_pr10 = f_pr * 10u / 224u;
                sat_bps_lp10 = f_lp * 10u / 224u;
                sat_bps_hd10 = f_hd * 10u / 224u;
                sat_bps_tl10 = f_tl * 10u / 224u;
                f_pr = f_lp = f_hd = f_tl = 0;
            }
        }
        /* SATURN 2026-08-25 -- ROWS 14 (`SEG`) AND 16 (`GCS`) OBEY THE SAME LAW.  prof_seg_* and
           prof_gc_* are zeroed in RP_BeginFrame (:2381-2383), which runs once per VIEW, but both
           rows are printed by dg_saturn's overlay burst at the end of the FRAME -> in split they
           showed the LAST VIEW ONLY.  The 08-25 console CSV is the proof: row-14 `c` medians 433
           in 1p (range 160-937) against a flat 257 in 2p and 256 in 4p -- a per-view constant that
           cannot be a frame census.
           NOT the p10 trap: nothing else reads prof_seg_ or prof_gc_ (r_segs.c writes them,
           RP_BeginFrame zeroes them, dg_saturn prints them, full stop), and accumulating IN them
           is impossible anyway since RP_BeginFrame wipes them at the top of the next view -- hence
           external accumulators, exactly as row 4 does.  Unrolled x4: no loop counter, so no
           identifier can shadow anything in the enclosing scopes. */
        {
            static unsigned int f_sc = 0, f_sf = 0, f_sp = 0, f_slp = 0;
            static unsigned int f_gt[4] = {0,0,0,0}, f_gn[4] = {0,0,0,0};
            if (!sat_split_active) {
                f_sc = f_sf = f_sp = f_slp = 0;
                f_gt[0]=f_gt[1]=f_gt[2]=f_gt[3]=0;
                f_gn[0]=f_gn[1]=f_gn[2]=f_gn[3]=0;
            }
            f_sc += prof_seg_cols;  f_sf  += prof_seg_fill;
            f_sp += prof_seg_px;    f_slp += prof_lead_px;
            f_gt[0]+=prof_gc_st[0]; f_gt[1]+=prof_gc_st[1];
            f_gt[2]+=prof_gc_st[2]; f_gt[3]+=prof_gc_st[3];
            f_gn[0]+=prof_gc_sn[0]; f_gn[1]+=prof_gc_sn[1];
            f_gn[2]+=prof_gc_sn[2]; f_gn[3]+=prof_gc_sn[3];
            if (last)
            {
                sat_seg_cols_f = f_sc;  sat_seg_fill_f = f_sf;
                sat_seg_px_f   = f_sp;  sat_lead_px_f  = f_slp;
                sat_gc_st_f[0]=f_gt[0]; sat_gc_st_f[1]=f_gt[1];
                sat_gc_st_f[2]=f_gt[2]; sat_gc_st_f[3]=f_gt[3];
                sat_gc_sn_f[0]=f_gn[0]; sat_gc_sn_f[1]=f_gn[1];
                sat_gc_sn_f[2]=f_gn[2]; sat_gc_sn_f[3]=f_gn[3];
                f_sc = f_sf = f_sp = f_slp = 0;
                f_gt[0]=f_gt[1]=f_gt[2]=f_gt[3]=0;
                f_gn[0]=f_gn[1]=f_gn[2]=f_gn[3]=0;
            }
        }
    }
    /* SATURN PERF (2026-07-08): MEASURED slave busy% = slave_busy ticks THIS frame (diff of the
       monotonic accumulator the slave bracketed on its own phi/128 FRT) / MST -- the real occupancy
       the DERIVED SLVi cannot see (it assumes the slave busy through all of P/M).  Computed HERE
       (before the fold) so the peak snapshot can record b/Pb of the worst-REC frame too. */
    static unsigned int sb_last = 0, pb_last = 0;
    unsigned int sb_now = (unsigned int)SYNC->slave_busy;
    unsigned int pb_now = (unsigned int)SYNC->slave_pbusy;
    unsigned int sb_d   = sb_now - sb_last;                 /* total slave ticks this frame (wraps cleanly) */
    unsigned int pb_d   = pb_now - pb_last;                 /* plane-only slave ticks this frame            */
    sb_last = sb_now; pb_last = pb_now;
    (void)idle;                                             /* derived SLVi retired: b/id/Pb are all MEASURED */
    /* SATURN 2026-08-25 -- b%/Pb% ARE FRAME QUANTITIES TOO, and in split they were neither.
       `b` divides THIS VIEW's slave ticks by rp_master_ms, which is the WHOLE frame -> the last
       view (the one that survives on screen) reported roughly a quarter of the truth in 4p.  `Pb`
       divided this view's plane ticks by this view's p10, which is at least self-consistent but
       answers a per-view question nobody asked.  Both now accumulate across the frame exactly as
       row 2 does, so `SLV b` can finally be read against `n4 P` on the same photograph -- the
       measurement docs/RESOURCE_BUDGETS.md 9.10 has been waiting for since the 08-20 fixes lifted
       the split gate.  The console baseline `b0 %` in every split frame is PRE-fix AND was read
       through this bug; do not compare against it. */
    /* THE THREE READ-ONLY COPIES: p10 itself must NOT be touched -- the governor below does its
       own per-view accumulation (`gsum_p += p10`), so overwriting p10 with the frame sum on the
       last view would make the governor count the whole frame twice.  Same for sb_d/pb_d, which
       the peak snapshot folds per view. */
    unsigned int sb_f = sb_d, pb_f = pb_d, p10_f = p10;
    unsigned int mp_f = prof_mplane + prof_makespans, w_f = p3_wait_ticks;
    unsigned int pvb_f = sat_p_build10;   /* row 5 `Pv`, frame-summed below like the rest */
    unsigned int fr_f  = prof_flatres;    /* row 5 `Pv`'s 2nd half: the allocating part          */
    unsigned int qsc_f = prof_planescan;  /* row 8 `Q<n>/<ms>`: subtracted out of `Pv` below      */
    {
        extern int sat_split_active, sat_split_view, sat_local_players;
        static unsigned int fsb = 0, fpb = 0, fp10 = 0, fmp = 0, fw = 0, fpv = 0, ffr = 0, fqs = 0;
        if (sat_split_active)
        {
            int nv = sat_local_players; if (nv > 4) nv = 4; if (nv < 1) nv = 1;
            fsb += sb_d; fpb += pb_d; fp10 += p10; fpv += sat_p_build10; ffr += prof_flatres;
            fmp += prof_mplane + prof_makespans; fw += p3_wait_ticks; fqs += prof_planescan;
            sb_f = fsb; pb_f = fpb; p10_f = fp10; mp_f = fmp; w_f = fw; pvb_f = fpv; fr_f = ffr;
            qsc_f = fqs;
            if (sat_split_view >= nv - 1) { fsb = fpb = fp10 = fmp = fw = fpv = ffr = fqs = 0; }
        }
        else fsb = fpb = fp10 = fmp = fw = fpv = ffr = fqs = 0;   /* same rule as row 2 above */
    }
    w10 = w_f * 10u / 224u;
    unsigned int busy_pct = rp_master_ms ? (sb_f * 100u / (224u * rp_master_ms)) : 0u;
    if (busy_pct > 999u) busy_pct = 999u;                   /* first-frame baseline / glitch clamp */
    /* (idle_pct dropped 2026-08-19 with the row-5 `id` field: it was 100-b, pure redundancy) */
    unsigned int pb_pct   = p10_f ? (pb_f * 1000u / (224u * (unsigned)p10_f)) : 0u;  /* plane balance */
    if (pb_pct > 999u) pb_pct = 999u;

    /* SATURN PERF (2026-06-24): fold this frame into the window.  Per-PHASE peaks are
       tracked INDEPENDENTLY (Bp and P do not peak on the same frame -> the right basis
       to size each offload); the REC histogram feeds p50/p95 (RP_ProfPercentile); the
       REC max also snapshots WHERE it happened (the MX locator).  All read on the
       platform's 1/s overlay tick so the photo is stable.  GLITCH GUARD: a frame whose
       FRT phase mark went stale (rp_active path / rp_disabled flip) yields a wrapped
       delta -> a bogus >300ms REC (impossible M, out-of-bounds MX); drop it so the
       peaks/max/histogram are not poisoned (Ymir shareware proved these are NOT CD
       stalls -- they survive into resident mode -> a profiler artifact, now excluded). */
    /* ------------------------------------------------------------------------------------------
       SATURN -- THE LOD GOVERNOR.  Triggers on the whole render frame, then degrades whichever
       phase dominated it: `B` -> the wall size-LOD rung, `P` -> the plane SQ floor.  Always on;
       there is no manual rung and no chord (owner 2026-08-16: *"active le gouverneur par défaut,
       enlève le toggle"*).

       🔴 REWRITTEN 2026-08-16 AFTER EIGHT CAPTURES SHOWED IT NEVER FIRED.  Every heavy capture read
       `u0 r0 px0 w0` while the game sat at 7-10 fps.  The bug was not the ceiling, it was the SHAPE:
       the previous version kept two counters and had each branch RESET THE OTHER.  Row 3 says this
       workload is bimodal -- `REC 50:36.0 95:86.0` -- so the median frame landed under the 60 ms
       floor, took the `dn` branch, and wiped the debt to zero.  Reaching 300 ms of debt would have
       required an unbroken run of >100 ms frames in a workload whose median is 36.  **The
       accumulator was structurally unable to fill.**  `u31` on one capture is exactly that: debt
       rising, one light frame, back to nothing.

       The fix is the standard integrator: ONE SIGNED accumulator, no cross-reset, so a bimodal load
       nets out instead of cancelling.  Fast frames now genuinely PAY DOWN debt rather than erasing
       it, which is the behaviour a frame counter can never have.

       ⚠ `rend` is Bw+Bp+P+M ONLY -- it is NOT the frame.  MST runs ~20-25 ms higher (T + S + blit +
       dg).  That gap is the trap the first version fell into: the owner's *"100 ms au total"* was a
       FRAME number and I applied it to `rend`, a smaller quantity, which put the ceiling ABOVE the
       p95 of the thing being compared to it.
       ⚠ `bp10`/`p10`/`m10`/`rend` are TENTHS OF A MS (`prof_x * 10 / 224`), not FRT ticks.

       🔴 THE TARGET IS CALIBRATED ON **HARDWARE**, NOT ON YMIR (2026-08-16, four console captures).
       The two machines do not share a distribution: Ymir reads `REC 50:36.0 95:86.0`, the Saturn
       reads `REC 50:82.0 95:126.0`.  A 55 ms target set from Ymir's numbers puts EVERY hardware
       frame above the band, so the integral only ever climbs and the governor slams to the top rung
       and stays there -- a constant wearing a governor's clothes.  95 ms sits just above the
       console's p50 and well under its p95, which is what makes it adapt on the machine that
       matters.  Ymir will now under-drive it; that is correct, Ymir is genuinely faster.
       ⚠ Ymir is NOT a valid oracle for the game tic either -- row-1 `T` is 8-14 ms there and 69-83
       on console.  Only the RENDER transposes between the two at all.

       ASYMMETRIC IN BOTH PLACES: fire at +300 ms, release at -900 ms, AND credit integrates at HALF
       rate.  Thresholds alone were not enough -- with symmetric rates a single quiet second erased a
       heavy one.  Quality that oscillates is worse to look at than quality that is merely low.

       ⚠ These constants are still a judgement call against a distribution known only through its
       p50/p95.  `e` on row 21 IS the falsifier: pinned negative while the game crawls = target still
       too high; rungs moving every second = pumping, target too low. */
#define GOV_TARGET10   950        /*  95,0 ms of render -- from the CONSOLE's p50/p95, see above    */
#define GOV_BAND10     150        /* +/- 15 ms dead band: inside it the integral is left alone      */
#define GOV_FIRE10    3000        /* +300 ms integrated -> degrade                                  */
#define GOV_REL10    (-9000)      /* -900 ms integrated -> give quality back                        */
    {
        static const int gov_rung[4] = { 0, 200, 400, 800 };
        /* SATURN 2026-08-21 -- PER-FRAME SUM IN SPLIT (owner: "on peut faire le gouverneur par
           vue, ça me parait plus quick win"; RESOURCE_BUDGETS opp 2).  rp_p3_prof_show runs once
           per VIEW, and the governor compared ONE view's rend (a quadrant: ~25-45 ms) to a
           target calibrated on a FULL 1p frame (95 ms) -> the integral only ever paid down, `e`
           pinned negative, and the emission levers (wall LOD, seg budget) never engaged exactly
           where the master is the wall (4p render 140 ms/frame).  The law that is right in
           EVERY mode is "the frame's TOTAL render vs GOV_TARGET10": accumulate B+P+M (and the
           vote's bp/p/m) across the frame's views, decide ONCE on the last view.  1p degenerates
           to the old behaviour exactly (sum of one view).  A glitched view poisons the whole
           frame's decision (the 1p sane-guard, applied frame-wide); the probe/undo then compares
           frame-sums to frame-sums, and the parole's sector check always samples the SAME (last)
           view.  Levers stay GLOBAL -- all views degrade together; per-view lever state is the
           next iteration IF the console A/B asks for it (falsifier: SPL per view drops when
           `w`/`lb` engage in 4p; SPL immobile = lever dead, undo does its job). */
        extern int sat_split_active, sat_split_view, sat_local_players;
        static unsigned int gsum_rend = 0, gsum_bp = 0, gsum_p = 0, gsum_m = 0;
        static int gsum_bad = 0;
        unsigned int g_rend = rend, g_bp = bp10, g_p = p10, g_m = m10;
        int gov_decide = (rend <= RP_REC_SANE);       /* never steer on a glitched frame */
        if (sat_split_active)
        {
            int nv = sat_local_players; if (nv > 4) nv = 4;
            if (rend > RP_REC_SANE) gsum_bad = 1;
            else { gsum_rend += rend; gsum_bp += bp10; gsum_p += p10; gsum_m += m10; }
            if (sat_split_view >= nv - 1)
            {
                g_rend = gsum_rend; g_bp = gsum_bp; g_p = gsum_p; g_m = gsum_m;
                gov_decide = !gsum_bad;
                gsum_rend = gsum_bp = gsum_p = gsum_m = 0; gsum_bad = 0;
            }
            else gov_decide = 0;                      /* mid-frame view: accumulate only */
        }
        if (gov_decide)
        {
            /* SATURN 2026-08-24 -- PUBLISH THE FRAME'S RENDER CLOCK.  The platform's wall-span
               governor (dg_saturn.cxx) had no per-frame CPU signal at all: it steered on
               rp_master_ms, which is the 1 Hz overlay period (MST), so it compared a whole-frame
               wall-clock average -- tic, sound and blit included -- against a 50 ms threshold the
               game never sees.  This is the same number THIS governor decides on: render only
               (B+P+M), tenths of a ms, already summed across the split views. */
            rp_rend10 = g_rend;
            int err = (int)g_rend - GOV_TARGET10;
            if (err > GOV_BAND10)        sat_gov_debt += err - GOV_BAND10;
            else if (err < -GOV_BAND10)  sat_gov_debt += (err + GOV_BAND10) / 2;   /* half rate */
            /* inside the band: hold the integral.  NEVER reset it -- that was the whole bug. */

            /* WHICH KNOB: the dominant phase of THIS frame owns the degradation.  Bw is measured and
               deliberately absent from the vote -- it is the BSP walk plus sprite projection and has
               no quality knob at all, so electing it would degrade something innocent.  When Bw is
               what dominates, the governor holds and row 21 says `d-`: an honest "I cannot help
               here" beats a confident wrong move. */
            /* PAROLE first (see GOV_RETEST): give the levers back before deciding anything, so a
               fire that lands on the frame after a sector crossing can already elect the axis the
               previous room had condemned. */
            if (gamemap != gov_last_map)
            {
                gov_last_map = gamemap; gov_last_sector = sat_view_sector;
                sat_gov_inert = 0; gov_retest_wait = 0;
            }
            else
            {
                if (gov_retest_wait > 0) gov_retest_wait--;
                if (sat_view_sector != gov_last_sector)
                {
                    gov_last_sector = sat_view_sector;
                    if (sat_gov_inert && gov_retest_wait == 0)
                        { sat_gov_inert = 0; gov_retest_wait = GOV_RETEST; }
                }
            }
            /* FAST PATH: the rung fired GOV_ACTWAIT frames ago and has not acted ONCE.  No timing
               is involved, so there is nothing to be fooled by -- convict and give the picture back
               now rather than after 24 frames of degradation bought for nothing. */
            if (gov_pr_wait == GOV_PROBE - GOV_ACTWAIT && gov_pr_axis >= 0)
            {
                unsigned int act = (gov_pr_axis == 0) ? sat_gov_act_w
                                 : (gov_pr_axis == 2) ? sat_gov_act_l
                                 : sat_gov_act_p;       /* SATURN 2026-08-24: `p` can be convicted too */
                if (act == gov_pr_act0)
                {
                    if      (gov_pr_axis == 0) { sat_lod_auto_step = 0; sat_gov_inert |= 1; }
                    else if (gov_pr_axis == 1) { sat_gov_p_step = 0; sat_gov_p_dirty = 1; sat_gov_inert |= 2; }
                    else if (gov_pr_axis == 2) { sat_gov_lead_step = 0; sat_gov_inert |= 4; }
                    sat_gov_debt = 0; gov_pr_wait = 0; gov_pr_axis = -1;
                }
            }
            if (gov_pr_wait > 0 && --gov_pr_wait == 0)
            {
                if (g_rend + GOV_PROOF > gov_pr_rend)    /* no measurable gain -> undo + blacklist */
                {
                    /* 🔴 UNWIND THE WHOLE AXIS, not one rung.  First hardware run of the probe read
                       `i1` on ten captures out of eleven -- it correctly proved the `w` axis inert --
                       and yet `w1`..`w3` PERSISTED, because backing off a single step leaves the rest
                       of the ladder standing.  A rung that has been shown to buy nothing is pure
                       visual cost, so give the picture back in full: that was the owner's original
                       complaint ("dégradé et jamais remonté") and a one-step undo does not answer it. */
                    if      (gov_pr_axis == 0) { sat_lod_auto_step = 0; sat_gov_inert |= 1; }
                    else if (gov_pr_axis == 1) { sat_gov_p_step = 0; sat_gov_p_dirty = 1; sat_gov_inert |= 2; }
                    else if (gov_pr_axis == 2) { sat_gov_lead_step = 0; sat_gov_inert |= 4; }
                    sat_gov_debt = 0;                    /* do not immediately re-fire the same trap */
                }
            }
            if (sat_gov_debt >= GOV_FIRE10 && gov_pr_wait == 0)
            {
                unsigned bp = g_bp, p = g_p, m = g_m;   /* split: frame-sums -> same dominance vote */
                sat_gov_debt = 0;
                if (bp >= p && bp >= m)      sat_gov_axis = 'B';
                else if (p >= bp && p >= m)  sat_gov_axis = 'P';
                else                         sat_gov_axis = 'M';

                /* 🔴 FALL THROUGH WHEN THE ELECTED AXIS IS RAILED (fix 2026-08-16, seen on console:
                   `dB w3 p0` with `e` cycling back to 0).  The first version elected the dominant
                   phase, found its rung already at maximum, DID NOTHING, and still reset the debt --
                   so it burned a fire every 300 ms and `p` stayed 0 forever while `P` and `M` were
                   both large.  Electing an axis you cannot move is the same as not firing.
                   'M' still has no proven live knob (the thing cap is not validated for a
                   controller), so it is only ever REPORTED -- but it must not BLOCK the others. */
                gov_pr_rend = g_rend; gov_pr_axis = -1;
                if (sat_gov_axis == 'B' && sat_lod_auto_step < 3 && !(sat_gov_inert & 1))
                    { sat_lod_auto_step++; gov_pr_axis = 0; }
                else if (sat_gov_axis == 'P' && sat_gov_p_step < 2 && !(sat_gov_inert & 2))
                    { sat_gov_p_step = (sat_gov_p_step < sat_gov_p_min)
                                     ? sat_gov_p_min : sat_gov_p_step + 1;
                      if (sat_gov_p_step > 2) sat_gov_p_step = 2;
                      sat_gov_p_dirty = 1; gov_pr_axis = 1; }
                /* 🔴 THE FALL-THROUGH USED TO IGNORE THE BLACKLIST, and that is why the owner's
                   ratchet SURVIVED the probe.  Four captures read `dB ... w1`/`w2` WITH `i1` set:
                   the first branch correctly refused the inert `w` axis, the second did not match
                   (the elected axis was 'B', not 'P'), and this third branch then raised `w`
                   anyway -- with no gov_pr_axis, so no probe, so no undo.  Every fire re-degraded
                   the one axis already proven useless.  The blacklist must bind on EVERY path that
                   can move a rung, not only on the elected one. */
                else if (sat_lod_auto_step < 3 && !(sat_gov_inert & 1))
                    { sat_lod_auto_step++; gov_pr_axis = 0; }
                else if (sat_gov_p_step < 2 && !(sat_gov_inert & 2))
                    { sat_gov_p_step = (sat_gov_p_step < sat_gov_p_min)
                                     ? sat_gov_p_min : sat_gov_p_step + 1;
                      if (sat_gov_p_step > 2) sat_gov_p_step = 2;
                      sat_gov_p_dirty = 1; gov_pr_axis = 1; }
                /* 🔴 SATURN 2026-08-17 -- THE LEAD-FILL LADDER, the owner's own proposal: "full quand
                   tout va bien, flat quand ça dégénère, désactivé quand c'est catastrophique".
                   It is LAST on purpose.  The lead-fill is his correction for VDP1's motion lag and
                   it stays until the VDP1/CPU sync is mastered -- but hardware says it costs the
                   majority of the frame (L1s 5,4 fps vs L0s 16,0), so a frame that is already
                   collapsing has to be allowed to trade it.  Step 1 keeps FULL COVERAGE and only
                   drops the texturing (mode 2 = flat spans): no holes, and holes are the one thing
                   that must never appear.  Step 2 is the last resort. */
                /* 🔴 SATURN 2026-08-24 -- GATED ON THE MECHANISM BEING ARMED AT ALL.
                   The lead-fill is PARKED: sat_wall_lead_x is 0 at boot (r_segs.c) and no pad chord
                   re-arms it any more, so sat_lead_arm always returns .on = 0 and SAT_LEAD_EMIT is
                   unreachable -- the fill cannot draw a single pixel.  The LADDER, however, was
                   still live: every fire that reached this rung spent GOV_FIRE10 of debt and
                   GOV_ACTWAIT frames of probe blackout on a lever with no effect, then convicted
                   itself, and on release burned the FIRST quality step on this dead axis before
                   giving anything back to `w`/`p`.  Worse, sat_lead_mode is an A/B window key
                   (dg_saturn RP_ProfReset): each 1->2->1 cycle wiped the REC p50/p95 histogram.
                   Testing sat_wall_lead_x instead of deleting the rung keeps the documented
                   revival hook honest -- re-arm the fill and the governor axis comes back with it,
                   with no second place to remember. */
                else if (sat_wall_lead_x > 0 && sat_gov_lead_step < 2 && !(sat_gov_inert & 4))
                    { sat_gov_lead_step++; gov_pr_axis = 2; }
                /* ARM THE PROBE ONLY IF A RUNG ACTUALLY MOVED.  Arming it unconditionally spent
                   GOV_PROBE frames of blackout -- during which no further fire can happen -- on a
                   fire that changed nothing, and then judged the result of a no-op. */
                if (gov_pr_axis >= 0)
                {
                    gov_pr_wait = GOV_PROBE;
                    gov_pr_act0 = (gov_pr_axis == 0) ? sat_gov_act_w
                                : (gov_pr_axis == 2) ? sat_gov_act_l
                                : (gov_pr_axis == 1) ? sat_gov_act_p : 0u;
                }
            }
            else if (sat_gov_debt <= GOV_REL10)
            {
                /* Release the MOST degraded axis first, so quality comes back in the reverse order
                   it was given up and the picture converges on full rather than on whichever axis
                   happened to be cheap to restore. */
                sat_gov_debt = 0;
                if (sat_gov_lead_step > 0)      /* give the picture back BEFORE the geometry */
                    sat_gov_lead_step--;
                else if (sat_lod_auto_step >= sat_gov_p_step && sat_lod_auto_step > 0)
                    sat_lod_auto_step--;
                else if (sat_gov_p_step > 0)
                    { sat_gov_p_step--; sat_gov_p_dirty = 1; }
                else
                    sat_gov_axis = '-';               /* fully released */
            }
            /* CLAMP AT THE RAILS.  Fully degraded and still behind, the integral would run away and
               then owe a huge unwind before quality could ever come back -- integrator windup, the
               classic failure of this exact loop.  Hold it one step short of firing instead. */
            if (sat_lod_auto_step >= 3 && sat_gov_p_step >= 2 && sat_gov_debt > GOV_FIRE10)
                sat_gov_debt = GOV_FIRE10;   /* every axis railed: hold, do not wind up */
            if (sat_lod_auto_step <= 0 && sat_gov_p_step <= 0 && sat_gov_debt < GOV_REL10)
                sat_gov_debt = GOV_REL10;
        }
        {
            static int lead_pad_mode = -1, lead_pad_x = -1;
            if (sat_gov_lead_step == 0)
            {
                if (lead_pad_mode >= 0)      /* restore whatever R+Right had selected */
                    { sat_lead_mode = lead_pad_mode; sat_wall_lead_x = lead_pad_x; lead_pad_mode = -1; }
            }
            else
            {
                if (lead_pad_mode < 0)
                    { lead_pad_mode = sat_lead_mode; lead_pad_x = sat_wall_lead_x; }
                sat_lead_mode   = 2;                                   /* flat spans, FULL coverage */
                sat_wall_lead_x = (sat_gov_lead_step >= 2) ? 0 : lead_pad_x;
            }
        }
        sat_lod_eff = gov_rung[sat_lod_auto_step & 3];
        /* SATURN 2026-08-16: the SAME rung also bounds the drawseg COUNT.  Sized on the hardware
           capture that motivated it -- `ds118` at `Bp110,8` -- so rung 1 barely bites on a normal
           view (measured ds5-30 in open play) and only the crowded frames pay.  0 = unbounded. */
        {
            static const int gov_segb[4] = { 0, 96, 64, 40 };
            sat_seg_budget = gov_segb[sat_lod_auto_step & 3];
        }
        /* SATURN 2026-08-24 -- THE SUBDIVISION SKIP IS A RUNG OF THIS SAME AXIS (owner: *"le skip
           subdivision devrait aussi etre une option du gouverneur, pas la norme"*).  It belongs on
           `B`/`w` and nowhere else: the SAT_VROWS divides it removes are billed to Bp, and the N-1
           VDP1 commands it also removes are the same budget the seg rung is defending.
           Rung 1 already, not 3 -- it is the CHEAPEST of the three: the area rung and the seg
           budget both replace a texture with a flat colour, while this one keeps the texture and
           only lets it swim on a wall you are standing in front of.  Degrade the picture last.
           r_segs.c bumps sat_gov_act_w every time the skip actually takes a wall off the
           subdivided path, so GOV_ACTWAIT convicts this rung like any other if it never bites. */
        {
            static const int gov_sub[4] = { 0, 1, 1, 1 };
            sat_wall_subdiv_skip = gov_sub[sat_lod_auto_step & 3];
        }
    }

    if (rend <= RP_REC_SANE) {
        /* SATURN 2026-08-24 -- THE SUB-SPLIT NOW RIDES A 1-SECOND PEAK, NOT THE SESSION PEAK.
           It used to latch on sat_prof_pk_bp, which RP_ProfReset only clears on a map or toggle
           change: once the post-load frame set the record, row 20 froze on it and answered
           "what was the worst frame of this map" instead of "where is the time going NOW".
           sat_prof_bp_win is the same peak over the platform's 1 s overlay window (dg_saturn
           zeroes it on the window tick), so the row stays a WORST-frame view -- single frames
           jitter far too much at 5-10 fps to read -- but a RECENT one.  Row 4's PK Bp keeps its
           session semantics untouched: that peak is what the A/B toggles compare. */
        if (bp10 > (unsigned)sat_prof_bp_win) {
            sat_prof_bp_win = (int)bp10;
            /* SATURN 2026-08-08: latch the Bp SUB-SPLIT of THIS SAME FRAME.  Three independent
               maxima would describe three different frames and prove nothing -- the lesson row 20
               already taught with `c`/`n` against `e`.  So the split always describes the frame
               that set `PK Bp` on row 4, which is the frame worth explaining.
               Skipped under sat_wallprep_slave: bp10 is then the master's WAIT (prof_wpwait) and
               the sub-terms were measured on the slave's own clock -- percentages of the wrong
               denominator.  Row 20 then holds the last inline-mode value; read wp on row 7. */
            if (!sat_wallprep_slave && prof_wallprep) {
                unsigned int pc = prof_segloop * 100u / prof_wallprep;   /* per-column loop      */
                unsigned int pr = prof_segrout * 100u / prof_wallprep;   /* per-seg routing      */
                unsigned int pg = prof_getcol  * 100u / prof_wallprep;   /* R_GetColumn (c pc)   */
                /* NO SILENT CLAMP.  A ratio over 100, or g > c when g is a SUBSET of c, means an
                   accumulator outlived its frame -- exactly the defect that made `s00 r00 c100`
                   read as a confident finding on 2026-08-08 (the resets sat in the branch of
                   RP_BeginFrame that shipping never takes).  A truncated number that still looks
                   plausible is worse than no number, so say it on the row: `B!` instead of `BP`.
                   ⚠ The invariant is `g <= r + c`, NOT `g <= c`: R_GetColumn is called from the
                   ROUTING PREAMBLE too, via R_WallPotatoColor in sat_wall_io_flat, which walks
                   every other column of a whole texture.  `g <= c` was my first guess and the row
                   correctly cried `B!` at it -- the model was wrong, not the measurement. */
                prof_bp_bad   = (pc > 100u || pr > 100u || pg > 100u || pg > pc + pr);
                prof_bp_c_pct = pc > 99u ? 99u : pc;
                prof_bp_r_pct = pr > 99u ? 99u : pr;
                prof_bp_g_pct = pg > 99u ? 99u : pg;
                prof_bp_g_n   = prof_getcol_n > 99999u ? 99999u : prof_getcol_n;
                /* SATURN 2026-08-10: `g` restored, in absolute MILLISECONDS this time (it was a
                   PERCENTAGE until 2026-08-09, then cut for `e`).  `e` is gone: it was rebuilds
                   measured ANYWHERE over calls counted only INSIDE Bp -- two different populations,
                   so it could legitimately exceed 100 and was silently clamped to 99, and the
                   rebuilds it counts are worth ~0.18 ms each (3 zone allocs, ~384 iterations), i.e.
                   ~3 ms for the 17 the 165 ms frame did.  It cannot be the hole and it cost the one
                   number that can size it.  ms, not %, because a percentage of Bp cannot separate
                   "Bp grew and g kept its share" from "g IS the growth". */
                {   /* Latch the zone-walk work of THIS frame, so it can be subtracted from `g`
                       with no clock conversion at all: ms = zw x ~30 cycles / 28600. */
                    extern int z_walk_blocks;
                    sat_bp_zw = (unsigned int)z_walk_blocks;
                }
                prof_bp_g_ms  = prof_getcol / 224u;   /* FRT ticks -> ms, same divisor as bp10 */
                if (prof_bp_g_ms > 999u) prof_bp_g_ms = 999u;
                /* SATURN 2026-08-14: `x` = worst SINGLE call, in the same ms as `g` (FRT/224), and
                   `d` = calls that will hit the disc.  Both latched to THIS frame, so `x`, `d` and
                   `g` are one clock and subtract directly -- the mistake `zc` made and `zw` fixed. */
                prof_bp_g_x   = (unsigned int)prof_gc_mx * 447u / 100u;   /* FRT tick = 4.47 us */
                if (prof_bp_g_x > 99999u) prof_bp_g_x = 99999u;
                /* e/a/k in MILLISECONDS: the question is which third carries ~46, and a sub-ms
                   third reading 0 is the answer, not a loss of resolution. */
                /* SATURN 2026-08-24 -- TENTHS, not whole ms.  Whole milliseconds were the right
                   unit when the question was "which third carries the 46 ms"; they are the wrong
                   one now that the same brackets are being read on YMIR, where the identical work
                   costs 0,1-0,4 ms and every field truncates to a confident `0`.  One FRT tick is
                   4,47 us, 224 of them ~= 1,0 ms, so `x10u/224u` = tenths and the printed width is
                   unchanged in the common case (1,4 ms prints `14` where it printed `1`).
                   Clamp at 999 = 99,9 ms; a bracket past that is a `B!` frame anyway. */
#define BP_T10(v) (((v) * 10u / 224u) > 999u ? 999u : ((v) * 10u / 224u))
                prof_bp_g_e = BP_T10(prof_st_mx[0]);
                prof_bp_g_a = BP_T10(prof_st_mx[1]);
                prof_bp_g_k = BP_T10(prof_st_mx[2]);
                prof_bp_g_q = BP_T10(prof_st_mx[4]);   /* the garde (W_LumpResident + Z_CanAllocate) */
                prof_bp_g_c = BP_T10(prof_st_sum[5]);  /* W_CacheLumpNum single-patch, SUM (global) -- the CD-read bill */
#undef BP_T10
                /* (`b` = per-frame composite count RETIRED 2026-08-12, one capture after it was
                   added.  It did its job: g30/b0 and g197/b4 killed R_GenerateComposite as the
                   explanation of the R_GetColumn hole, so keeping the latch would be a value
                   computed for nobody.  r_composite_builds still prints as row-18 `cb` if the
                   question ever reopens.) */
            }
        }
        if (rend > prof_w_recmax) {
            prof_w_recmax = rend;
            sat_prof_mx_map=gamemap;
            sat_prof_mx_x=(int)(viewx>>16); sat_prof_mx_y=(int)(viewy>>16);
            sat_prof_mx_ang=(int)(viewangle>>24); sat_prof_mx_t=leveltime;
            /* SATURN PERF (2026-07-09, user req): snapshot the FULL detail of the worst-REC frame
               (persists until beaten or a config change) so a new peak is a capture opportunity.
               Phase split says WHICH phase spiked; b/Pb say whether the slave was idle during it. */
            sat_prof_mx_bw=(int)bw10; sat_prof_mx_bp=(int)bp10;
            sat_prof_mx_p =(int)p10;  sat_prof_mx_m =(int)m10;
            sat_prof_mx_b =(int)busy_pct; sat_prof_mx_pb=(int)pb_pct;
        }
        {
            unsigned int bi = rend / RP_HBWIDTH;
            if (bi >= RP_HBUCKETS) bi = RP_HBUCKETS - 1;
            prof_hist[bi]++; prof_w_recn++;
        }
        sat_prof_rec_max = (int)prof_w_recmax;
    } else {
        sat_prof_dropped++;
    }
    /* w = master idle waiting for the slave at the plane barrier (master FRT): w~0 => balanced.
       (b/id/Pb are computed above, before the fold, so the peak snapshot can record them.) */
    /* row 5: MEASURED slave occupancy + the plane-fill ms split (2026-08-19, VDP1-planes verdict
       probe).  b = slave busy% of MST; Pb = the slave's share of the plane phase P (~50% balanced);
       Pm = MASTER plane-fill ms (worklist drain: TAS steal + fallbacks + the inline makespans
       path) -- THE number a VDP1-planes offload would recover; Ps = SLAVE plane-fill ms (incl. its
       cache purge + claim overhead) -- rides an otherwise idle CPU; w = master idle at the plane
       barrier.  Full mode only.  (`id` dropped 2026-08-19: it was 100-b, pure redundancy.) */
    /* SATURN 2026-08-25: `&& last` -- rp_p3_prof_show runs once per VIEW, so before this gate a 4p
       frame formatted and blitted rows 5 AND 20 four times for one visible line each (row 2 and
       row 4 were given `last` earlier the same day; these two were missed).  One gate covers both:
       the block closes after row 20's dbg_print.  ⚠ `mw` CHANGES VALUE with this: rp_wait_mx is
       zeroed at the print site below, so the surviving `mw` used to be the max master-wait of the
       LAST VIEW ONLY (views 0..n-2 were zeroed away unread).  It is now the frame max -- LARGER in
       split and correct.  Do not read the rise as a slave regression against a pre-08-25 capture;
       1p is byte-identical. */
    if (sat_dbg_overlay_mode == 0 && last) {
        /* `mw` = the LONGEST the master waited for the slave this window, ms, any site.  Read it
           against RP_WAIT_TIMEOUT_FRT (100 ms): pinned at the ceiling = a dying slave, well under =
           the ceiling is doing no harm.  It is what `to` could never say. */
        /* every ms on this row is now the same quantity: the FRAME (see sb_f/pb_f/mp_f/w_f).
           Pm and Ps are a PAIR -- the plane-phase balance meter -- so summing one and not the
           other would have made the row read like an imbalance that is not there. */
        unsigned int mp10 = mp_f * 10u / 224u;
        unsigned int ps10 = pb_f * 10u / 224u;
        if (mp10 > 999u) mp10 = 999u;
        if (ps10 > 999u) ps10 = 999u;
        /* [!] `Pb%` RETIRED 2026-08-26 and its cells pay for `Pv`.  Pb was pb_f/p10, i.e.
           exactly Ps / row-2 P -- derivable on sight from two numbers already printed (7.1 / 21.7
           = 33 %), so it spent 7 of 40 cells restating them.  `Pv` is the visplane worklist
           build and is derivable from nothing.  With it the plane budget CLOSES on one photo:
           row-2 kick + Pv + max(Pm, Ps) == row-2 P. */
        /* `Pv` MINUS THE OBSERVER.  RP_PlanePixels runs inside this bracket and only on
           full-overlay frames, so an un-subtracted Pv measured the overlay as much as the game. */
        unsigned int qs10 = qsc_f * 10u / 224u;
        unsigned int pv10 = (pvb_f > qs10) ? (pvb_f - qs10) : 0u;
        unsigned int pf10 = fr_f * 10u / 224u;
        sat_q_scan10 = qs10;
        if (pv10 > 999u) pv10 = 999u;
        if (pf10 > 999u) pf10 = 999u;
        (void)pb_pct;
        /* [!] PADDED TO 40 AND CUT AT 40, like the BPS row.  The 4p capture of 2026-08-26 read
           "... mw0 1": a glyph from a LONGER previous render surviving past the end of a shorter
           one, because this row was never fixed-width.  Retiring Pb% shortened it and made the
           ragged tail visible -- the bug was always there, it just had nothing to expose it.
           A row that can ghost is worse than a missing row: it reads as a live measurement. */
        /* `Pv<build>/<flat>` and `w<sum>/<max>`.  The wait pair was two fields (`w` and `mw`)
           asking ONE question -- how long did the master spend waiting on the slave -- so folding
           them into sum/max is the coherent spelling AND it buys the four cells `Pv`'s second
           half needs.  `mw` keeps its meaning: the LONGEST single wait this window, read against
           RP_WAIT_TIMEOUT_FRT (100 ms); pinned at the ceiling = a dying slave. */
        snprintf(p, sizeof p, "SLV b%u%% Pm%u.%u Ps%u.%u Pv%u.%u/%u.%u w%u.%u/%u"
                              "                                        ",
                 busy_pct, mp10/10, mp10%10, ps10/10, ps10%10,
                 pv10/10, pv10%10, pf10/10, pf10%10,
                 w10/10, w10%10, (unsigned int)(rp_wait_mx / 224u));
        p[40] = 0;
        rp_wait_mx = 0;
        dbg_print(0, 5, p);
        /* row 20 -- the Bp DECOMPOSITION, all of it describing the frame that set `PK Bp` on row 4.
           `r` = the per-SEG routing preamble (CPU/VDP1 tier choice, hysteresis, clamp, perspective
           subdivision, lead-fill arming, the load-budget gates).  `c` = the per-COLUMN loop.  `g` =
           R_GetColumn alone, a SUBSET of c, counting only calls made from inside R_StoreWallRange
           (the sky column and the masked mid-texture are billed to P and M).  The remainder,
           100-r-c, is R_StoreWallRange's own setup plus the sprite clip save.
             g high  => the texture cost is PER-COLUMN: three calls per column on the single-patch
                        path (R_EnsureLookup, W_LumpResident, W_CacheLumpNum + its Z_ChangeTag),
                        so hoist the per-texture invariants -- minding that the R4 directory is
                        purgeable.
             g low   => it is PER-PIXEL, i.e. the R_DrawColumn inner loop.  Different chantier.
           ⚠ `g` is an UPPER BOUND: two FRT reads per column (~2% of a heavy frame) land inside it.
           ⚠ FIELDS MUST STAY IN COLUMNS 0-13 -- the VDP1 weapon sprite covers columns 14-27 of rows
           19-21.  `BP r99 c99 g99` is exactly 14 chars; do not widen it. */
        /* `r` and `c` are still COMPUTED (the B! invariant needs them) but no longer PRINTED: eight
           captures settled them as near-CONSTANTS in absolute terms -- routing ~8 ms and per-pixel
           fill ~14 ms whether the frame costs 40 ms or 312.  All of Bp's variance is `g`, which
           went 10 -> 191 ms across the same captures.  So the row now carries `g` and, back in its
           place, `n`: the COLUMN COUNT is what separates the two remaining hypotheses --
             n roughly flat while g explodes => the cost PER CALL exploded (a purged R4 directory or
               composite being REBUILT inside the column loop; watch `lg` on row 11, which fell
               38-48K on the light frames to 22-29K on the heavy ones),
             n explodes with g            => it is simply call volume, and the fix is d32xr's:
               resolve the texture pointer ONCE PER WALL instead of once per column. */
        /* 🔴 2026-08-10 -- `g` IS BACK, and in ms.  It was cut for `e` on 08-09; the very next
           capture (TNT MAP11, Bp = 164.7 ms on ONE frame with n = 388) needed exactly it and could
           not be read.  The arithmetic forks and BOTH horns are refuted without g:
             164.7 ms / 388 calls = 424 us/call = ~12 150 cycles -- nothing on R_GetColumn's
             resident fast path costs that, AND 17 directory rebuilds at ~0.18 ms are ~3 ms, not
             130.  So one of the two INPUTS is wrong, and g is the only unmeasured one.
           Owner measured the free A/B first (pad R+Right -> `L0-`): Bp did NOT collapse, so the
           calls are real wall columns, not lead-fill.  Read it as:
             g ~= Bp - 25 ms  => the cost is PER CALL (texture management; hoist per-texture
                                 invariants out of the column loop, minding the purgeable R4 dir),
             g < 30 ms        => it is PER PIXEL (R_DrawColumn) and R_GetColumn is exonerated.
           ⚠ `g` is an UPPER BOUND: two FRT reads per column (~2% of a heavy frame) land inside it.
           ⚠ FIELDS MUST STAY IN COLUMNS 0-13 (the VDP1 weapon covers 14-27 of rows 19-21):
           `B! g999 n99999` is exactly 14 chars, the worst case.  Do not widen it. */
        /* `n` RETIRED 2026-08-12: it answered its question -- the call count is FLAT (169..421) across
           both regimes while us/call went 81 -> 590, so the explosion is per-CALL, not volume.  Its
           column goes to `b`, the only number that can size the surviving candidate.  Worst case
           `B! g999 b99` = 11 chars, columns 0-10, against the 0-13 limit (the VDP1 weapon covers
           14-27 of rows 19-21) -- 3 columns of margin where `n%u` had zero. */
        /* 🔴 2026-08-12, ONE CAPTURE LATER: `b` answered its question and `n` comes back.
           Measured on TNT MAP11: `g30 b0` on a 57 ms Bp frame and `g197 b4` on a 225 ms one.  The
           delta is +167 ms of g for +4 composites = 42 ms each, against a cost model of 2.8 ms
           (median) to 7.7 ms (worst texture + both zone walks) -- off by 5-15x.  And `b0` with
           `g30` settles it from the other side: 30 ms of R_GetColumn with ZERO composites built.
           COMPOSITES ARE NOT THE HOLE.
           Retiring `n` on 2026-08-10 was a mistake made one turn after writing down the rule that
           forbids it: `g` alone cannot separate "350 calls at 560 us" from "2000 calls at 100 us",
           and those two worlds have opposite fixes.  Never delete the only unmeasured input to an
           open question -- that is exactly how `g` itself was lost the day before it was needed.
           `BP g197 n363` = 12 chars, inside the columns 0-13 the VDP1 weapon leaves. */
        /* `d` RETIRED after ONE capture, and that is the correct lifetime for it: `d0` on every
           photo, with `ld` moving +6 over the whole run as the independent witness.  The disc is
           not in R_GetColumn's hot path.  (r_getcol_disc was REMOVED 2026-08-26 -- no reader.) */
        /* `a` RETIRED 2026-08-14: it bracketed R_GenerateLookup's patch loop and, ever since the
           header-only fetch, reads exactly `e` on every capture -- a field that can only repeat its
           neighbour is a field that has answered.  Its slot still accumulates; only the print is
           gone, so re-adding it is one line if the question reopens. */
        /* Trailing spaces sized for the widest form (`x99999` + 3-digit e/k/z): dbg_print does not
           clear the tail, so a shorter line leaves the previous one's digits behind -- the owner's
           `z0     0` ghost. */
        /* 🔴 SATURN 2026-08-17 -- PRINT `a`.  It has been COMPUTED since 2026-08-14 and never shown,
           and that omission just cost a whole build: the WAD transform killed the composite
           (`cb0/0`, `k0` on four captures, both witnesses agreeing) and `x` did not move -- 15 175 us
           for one R_GetColumn against `e1 k0 z1`, i.e. ~14 ms in the one bracket the row refused to
           display.  `a` is W_CacheLumpNum on the single-patch path: the LUMP FAULT itself. */
        /* `z` RETIRED 2026-08-17 -- it read 0-1 on every capture ever taken, its question (is the
           worst Z_Malloc the hole?) is answered NO, and its two columns are worth more to `q`/`c`,
           the pair that now decides between "make the garde cheap" and "stop faulting lumps". */
        /* UNITS (2026-08-24): `g` = whole ms, `n` = calls, `x` = MICROseconds (worst single call),
           and `a e k q c` = TENTHS of a ms.  They were whole ms until today and read `0 0 0 0 0`
           on Ymir for the arithmetic reason, not for a defect: the same brackets cost 1-7 ms on
           console and 0,1-0,4 ms there.  `c` is the exception that no unit can fix -- it is the
           W_CacheLumpNum disc bill, and Ymir serves the image from host RAM, so `c0` is STRUCTURAL
           there and only the console can answer that field. */
        /* TRAILING SPACES: dbg_print does not clear the tail, and the fields just got SHORTER
           (whole ms -> tenths only widens the small values, while g/n/x shrink to 0 the moment
           the walls all ride VDP1).  The owner's 4p capture read `BP g0 n0 x0 a0 e0 k0 q0 c0 87`
           -- the `87` is the tail of a previous, longer line.  Pad to the widest form. */
        snprintf(p, sizeof p, "%s g%u n%u x%u a%u e%u k%u q%u c%u                    ",
                 prof_bp_bad ? "B!" : "BP", prof_bp_g_ms, prof_bp_g_n,
                 prof_bp_g_x, prof_bp_g_a, prof_bp_g_e, prof_bp_g_k,
                 prof_bp_g_q, prof_bp_g_c);
        dbg_print(0, 20, p);
    }
    /* SATURN (VDP1-floor inc-0): surface the floor-quad estimate.  This P3 path is the one
       that actually runs (parity disabled), so the setter MUST live here too -- not only in
       the rp_active block.  The full FLAT line (row 13) is hidden behind split viewports, so
       dg_saturn mirrors Vs/Vp onto row 16.  vsec = VDP1 candidate cost (= total when RBG0 owns
       its plane via sat_vdp2_floor, else total minus the pixel-dominant group). */
    {
        unsigned int vqtot = prof_floor_vq, vdom = prof_floor_vq_dom, vsec;
        if (prof_pp_cur_sum > prof_plane_dom) vdom = prof_pp_cur_vq;   /* fold the last open group */
        vsec = sat_vdp2_floor ? vqtot : (vqtot >= vdom ? vqtot - vdom : 0u);
        if (vsec > prof_floor_vq_peak) prof_floor_vq_peak = vsec;
        sat_floor_vq_cur  = (int)vsec;
        sat_floor_vq_peak = (int)prof_floor_vq_peak;
        /* RBG0-floor sizer: the dominant single-flat share of plane pixels + visplane
           count (sweet spot = high dom% + low n => one flat owns the floor -> RBG0). */
        {
            unsigned int dom = (prof_pp_cur_sum > prof_plane_dom) ? prof_pp_cur_sum : prof_plane_dom;
            sat_prof_dom_pct = prof_plane_pix ? (int)(dom * 100u / prof_plane_pix) : 0;
            sat_prof_plane_n = (int)prof_plane_n;
        }
        /* SATURN 2026-08-25: the FLAT snprintf that stood here is DELETED.  It formatted a string
           into `p` and threw it away one line later with `(void)p` -- and it sat OUTSIDE the
           overlay-mode gate (which closes above), so it ran once per VIEW in EVERY mode including
           overlay-OFF, i.e. in the SHIPPED build, forever, for nobody.  The "is this output
           thrown away?" test answers YES before any "can it be tighter?".  The assignments around
           it STAY: sat_floor_vq_cur/_peak and sat_plane_quad_n/_q4cmd/_q4pct have live consumers.
           Consequence to know: prof_floor_vq_int_peak (:1736, fed per visplane at :2116) was read
           ONLY by this string and is now write-only -- a candidate for a later cut, listed here so
           the next pass does not have to re-find it. */
        /* SATURN 2026-08-19 (4th build of the day): the row-8 `Q` publish MUST live HERE -- this
           P3 path is the only one that runs in the shipping config (rp_disabled).  The first
           THREE probe builds published only from the rp_active block below (two near-identical
           FLAT blocks; the setter landed in the dead one), so the owner read Q0/0 on every
           capture regardless of scene or arming: the probe never published.  Same trap as
           [[rp-disabled-kills-flat-wall-modes]] and the 2026-08-08 reset-site rot -- when a
           stat has consumers, grep BOTH copies of the block before believing a zero. */
        sat_plane_quad_n   = (int)prof_plane_quadn;
        {   /* inc-1: increment-0's decision pair -- top-4 runs' real command bill + pixel share */
            unsigned int q4px = prof_q4_px[0] + prof_q4_px[1] + prof_q4_px[2] + prof_q4_px[3];
            sat_plane_q4cmd = (int)(prof_q4_cmd[0] + prof_q4_cmd[1] + prof_q4_cmd[2] + prof_q4_cmd[3]);
            sat_plane_q4pct = prof_plane_pix ? (int)(q4px * 100u / prof_plane_pix) : 0;
        }
    }
}
#endif

void RP_EndFrame(void)
{
    if (!rp_active) {
#if RP_PROF
        if (rp_disabled)
        {   /* row 14 `ov` -- see the note at sat_prof_show10 */
            unsigned short ov_t0 = rp_frt();
            rp_p3_prof_show();                /* P3: the only readout when parity is off */
            {
                extern int sat_split_active, sat_split_view, sat_local_players;
                static unsigned int ov_acc = 0;
                int nv = 1;
                ov_acc += (unsigned short)(rp_frt() - ov_t0);
                if (sat_split_active) { nv = sat_local_players; if (nv > 4) nv = 4; if (nv < 1) nv = 1; }
                if (!sat_split_active || sat_split_view >= nv - 1)
                {   sat_prof_show10 = ov_acc * 10u / 224u; ov_acc = 0; }
            }
        }
#endif
        return;
    }
#if RP_PROF
    prof_recend = rp_frt();     /* recording done; rp_finish = execute+wait */
#endif
    rp_finish();
#if RP_PROF
    {
        unsigned int rec = (unsigned short)(prof_recend - prof_begin);
        unsigned int exe = (unsigned short)(rp_frt() - prof_recend);
        unsigned int rec10 = rec * 10u / 224u;   /* NTSC: ~224 FRT ticks/ms */
        unsigned int exe10 = exe * 10u / 224u;
        unsigned int wai10 = prof_wait * 10u / 224u;   /* slave-wait within EX */
        static char p[44];
        (void)wai10;
        /* SATURN OVERLAY 2026-07-29: level-4 REC must NOT clobber dg_saturn's shared rows.
           The record/execute split (rec10/exe10) that used to own row 4 (over PK) is folded into
           the row-5 headline below; the old rows 11/12 (over LIM/V1) are dropped entirely.  Only
           rows 2 + 5 differ from the levels-0-3 layout now -- every other row stays dg_saturn's
           shared content, so s0..s4 are directly comparable line-for-line. */
        /* Row 20 (SATURN PERF 2.4 Stage 1): B (the BSP walk) split into pure BSP
           traversal (Bw) vs wall-prep (Bp = time in R_StoreWallRange), plus the
           planes (P) and masked (M) generation.  Bp is what 2.4 would offload to
           the slave; Bw is the inherently-serial visibility walk that cannot be.
           (The TMO timeout path still writes row 20 on the ~never timeout, so a
           regression would still surface there.) */
        {
            unsigned int bt  = (unsigned short)(prof_bsp_end - prof_begin);
            unsigned int bpt = prof_wallprep;
            unsigned int bwt = (bt > bpt) ? (bt - bpt) : 0u;   /* pure traversal */
            unsigned int bw10 = bwt * 10u / 224u;
            unsigned int bp10 = bpt * 10u / 224u;
            unsigned int p10 = (unsigned short)(prof_planes_end - prof_bsp_end)   * 10u / 224u;
            unsigned int m10 = (unsigned short)(prof_recend     - prof_planes_end)* 10u / 224u;
            snprintf(p, sizeof p, "Bw%u.%u Bp%u.%u P%u.%u M%u.%u ",
                     bw10/10, bw10%10, bp10/10, bp10%10,
                     p10/10, p10%10, m10/10, m10%10);
            /* SATURN OVERLAY 2026-07-29: REC (level-4) phase-split -> ROW 2, the SAME row the
               parity-off path (rp_p3_prof_show, levels 0-3) uses.  Was row 5, which left row 2
               showing a FROZEN level-0-3 phase-split (nobody rewrote it in level 4) = the
               "parasité" the tester saw.  Row 5 is now the slave-occupancy line in BOTH regimes. */
            dbg_print(0, 2, p);
            /* (2026-07-29) The BP s/l (row 11) + P a/m/o (row 12) fine-split writes were removed
               here: they overwrote dg_saturn's LIM (zone/frag) and V1 (VDP1 budget) rows -- both
               more valuable during an s0..s4 A/B than the record-phase micro-breakdown, which row 2
               (Bw/Bp/P/M) already summarises. */
        }
        /* Row 13 (SATURN PERF, RBG0 candidate sizing): floor/ceiling FILL and the
           share owned by the single largest flat -- the VDP2 RBG0 offload candidate.
           t = total non-sky span pixels (the P fill workload), d = the largest
           (picnum,height) group's pixels, then dom% = d/t, n = the non-sky visplane
           count.  Low n + high dom% => P is concentrated in one flat (the single-
           flat RBG0 trick bites); high n + low dom% => fragmented (it won't).
           Pixels in thousands (k).  NB: the pixel scan runs inside R_DrawPlanes so it
           inflates row-20 P / row-12 'o' slightly -- this is a measurement build; the
           dom% RATIO is overhead-insensitive (t and d scale together). */
        {
            unsigned int dom = prof_plane_dom;
            unsigned int tot = prof_plane_pix;
            unsigned int vdom = prof_floor_vq_dom;
            unsigned int vqtot = prof_floor_vq, vsec, pct;
            if (prof_pp_cur_sum > dom)                 /* fold the last open group */
            {
                dom = prof_pp_cur_sum;
                vdom = prof_pp_cur_vq;
            }
            pct = (tot > 0u) ? (dom * 100u / tot) : 0u;
            /* VDP1 floor candidate cost = ALL of vqtot when sat_vdp2_floor is on (RBG0 already
               took its plane, excluded above), else vqtot minus the pixel-dominant group
               (which RBG0 would take).  Vp = monotonic peak = the inc-0 go/no-go number. */
            vsec = sat_vdp2_floor ? vqtot : (vqtot >= vdom ? vqtot - vdom : 0u);
            if (vsec > prof_floor_vq_peak) prof_floor_vq_peak = vsec;
            sat_floor_vq_cur  = (int)vsec;                 /* surfaced on visible row 2 */
            sat_floor_vq_peak = (int)prof_floor_vq_peak;   /* (row 13 is hidden in split) */
            snprintf(p, sizeof p, "FLAT d%u%% n%u Vt%u Vs%u Vp%u   ",
                     pct, prof_plane_n, vqtot, vsec, prof_floor_vq_peak);
            (void)p;   /* FLAT (parked VDP1-floor telemetry) cut from overlay -- VDP1_FLOOR_PLAN.md */
            /* Row-8 `Q`/`E` publish, rp_active MIRROR only: the LIVE setter is in rp_p3_prof_show
               (rp_disabled ships and returns before reaching here -- the first three probe
               builds published ONLY from this dead block, hence the owner's Q0/0 rounds). */
            sat_plane_quad_n   = (int)prof_plane_quadn;
            {
                unsigned int q4px = prof_q4_px[0] + prof_q4_px[1] + prof_q4_px[2] + prof_q4_px[3];
                sat_plane_q4cmd = (int)(prof_q4_cmd[0] + prof_q4_cmd[1] + prof_q4_cmd[2] + prof_q4_cmd[3]);
                sat_plane_q4pct = (tot > 0u) ? (int)(q4px * 100u / tot) : 0;
            }
        }
        /* Row 18: MST (master frame ms, set by dg_saturn.cxx fps_update -- the
           synchronous bottleneck; the standalone MST row 15 was dropped) + the slave
           opaque-phase occupancy (SATURN PERF 2.4 Stage 1).  i = idle% (waiting for the
           master to produce commands during REC), b = busy% drawing.  High idle% => the
           slave has slack REC time that wall-prep could fill (2.4 viable); low idle% =>
           it is saturated drawing.  Ratio is divider-independent (slave FRT need not
           match the master's); t/d are the raw slave ticks. */
        {
            unsigned int st = (unsigned int)SYNC->slave_opq_total;
            unsigned int sd = (unsigned int)SYNC->slave_opq_draw;
            unsigned int busy = (st > 0u) ? (sd * 100u / st) : 0u;
            /* SATURN OVERLAY 2026-07-29: single level-4 headline on ROW 5 (same row as the levels-
               0-3 `SLV` line, for a direct A/B; leading token `L4` disambiguates the regime).
               R = record ms (master-serial command GENERATION -- the real M7 bottleneck), X =
               execute ms (the ONLY phase the REC slave can share; near-zero in M7 because walls/
               things live on VDP1 and the dominant floor on RBG0), SLVb = slave draw share of the
               opaque phase, d = raw slave draw ticks, c = commands recorded.  R>>X ⇒ record-bound
               ⇒ the REC slave has almost nothing to steal -- which is why s4 shows the slave LESS
               active than plane-split s2/s3 (those parallelise the plane phase P itself). */
            snprintf(p, sizeof p, "L4 R%u.%u X%u.%u SLVb%u%% d%u c%u ",
                     rec10/10, rec10%10, exe10/10, exe10%10, busy, sd, (unsigned)rec_count);
            dbg_print(0, 5, p);
        }
    }
#endif
    colfunc=saved_col; basecolfunc=saved_base;
    fuzzcolfunc=saved_fuzz; transcolfunc=saved_trans; spanfunc=saved_span;
    rp_active=0;
}
