/*
** Shared dual-SH2 renderer back end (doom-saturn-core).
**
** Pure C, SDK-agnostic: the command queue, executors, sync protocol and
** cache-coherency rules are all hardware-level (SH-2 / SGL), so this exact
** file is compiled by BOTH ports (DoomSRL and DoomJo).  The only platform
** touch-points are:
**   - slSlaveFunc()  : SGL, linked by both ports
**   - dbg_print()     : a thin debug-overlay shim each port provides
**                      (SRL::Debug::Print on DoomSRL, native on DoomJo)
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
extern int   sat_potato_floors;   /* SATURN: solid-colour floors/ceilings (Potato) */
extern int   sat_potato_walls;    /* SATURN: solid-colour walls (opaque RP_COL only) */
extern int   sat_wall_color;      /* SATURN: current wall's dominant colour (r_segs) */
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
    if (sat_potato_walls && !cm->unused)
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
    if (detailshift)   /* SATURN PERF 2.3: blocky fuzz, mirrors R_DrawFuzzColumnLow */
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
    if (sat_potato_walls && !cm->unused)   /* opaque wall column -> single colour */
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
    if (detailshift)
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

static int rp_wait(volatile int *flag)
{
    int guard=1000000;
    while (!*flag && --guard) ;
    if (!guard) rp_timeout_count++;
    return guard!=0;
}

/* TEST (2026-06-15): 0 = disable the manual GBR+72 reset, to confirm it is what
   desyncs the slave dispatch (slave dies after a few frames -> al0).  With it
   off the slave should dispatch normally until the ~2-min GBR+72-creep freeze
   returns -- watch row 19 on Ymir for ~30s.  If row 19 then moves continuously,
   the reset is the dispatch-breaker and the real fix is the proper slSynch
   slave-resync (which our manual write-pointer-only reset omits). */
#define RP_GBR_RESET 1

/* SATURN: THE ~1-2min freeze fix.  slSlaveFunc bump-allocates a 12-byte record
   {0x30, func, arg} from the SGL transient work buffer at GBR+72 and advances
   the pointer every call, but NEVER resets it -- SGL normally resets it once
   per frame inside slSynch(), which DoomSRL replaced with its own vblank sync.
   Because rp_restart() calls slSlaveFunc every frame, GBR+72 crept forward 12
   bytes/frame into the SGL system area and after ~1-2 min overran the VBlank
   user-callback pointer at GBR+20 (0x060FFC14); _BlankIn then jsr'd to garbage
   -> CPU illegal-instruction exception -> SGL halt-loop = the freeze.  We
   restore GBR+72 to its post-init base before each slSlaveFunc, exactly as
   slSynch would, so the single per-frame record always reuses the same slot. */
void rp_sgl_workptr_reset(void)
{
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
    SYNC->potato = (sat_potato_floors ? 1 : 0) | (sat_potato_walls ? 2 : 0);
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
int sat_plane_parallel = 0;              /* set by the DoomSRL platform (src/main.cxx) */
extern void R_DrawPlaneWorklist(int from, int to);

static volatile int rp_plane_done = 1;
#define PLANE_DONE (*(volatile int *)((unsigned int)&rp_plane_done | 0x20000000u))

/* SGL's slave stack (0x06001e00) is only ~1-2KB before it hits SGL system data, but
   R_DrawVisplane* puts a local spanstart_l[SCREENHEIGHT] (~0.9KB) + frames on the stack ->
   it would overflow and corrupt SGL.  Give the plane slave its OWN 4KB stack and switch to
   it for the draw.  (Only the slave touches this, so its write-through writes need no purge.) */
static char rp_plane_slave_stack[4 * 1024] __attribute__((aligned(16)));
static int  rp_plane_from, rp_plane_to;

static void rp_plane_worklist_noarg(void)
{
    R_DrawPlaneWorklist(rp_plane_from, rp_plane_to);
}

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

static void rp_plane_slave_body(void *param)
{
    int packed = (int)(unsigned int)param;   /* (from<<16)|to -- avoids a shared coherency word */
    master_cache_purge();                    /* slave: read the master's fresh worklist + tables */
    rp_plane_from = (packed >> 16) & 0xffff;
    rp_plane_to   = packed & 0xffff;
    rp_run_on_stack(rp_plane_worklist_noarg);/* draw on the dedicated stack */
    PLANE_DONE = 1;
}

void RP_DispatchPlanes(int from, int to)
{
    PLANE_DONE = 0;
    rp_sgl_workptr_reset();                  /* GBR-creep guard for this 2nd dispatch/frame */
    slSlaveFunc(rp_plane_slave_body, (void *)(unsigned int)(((from & 0xffff) << 16) | (to & 0xffff)));
}

void RP_WaitPlanes(void)
{
    volatile int guard = 30000000;           /* ~bounded spin; never wedge if the slave dies */
#if RP_PROF
    unsigned short t0 = rp_frt();             /* master idle while the slave finishes = imbalance */
#endif
    while (!PLANE_DONE && guard-- > 0) { }
#if RP_PROF
    p3_wait_ticks = (unsigned short)(rp_frt() - t0);
#endif
    master_cache_purge();                     /* read the slave's drawn plane pixels before the blit */
}

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
    master_cache_purge();                    /* slave: read the master's fresh vissprites + drawsegs */
    rp_mask_x0 = (packed >> 16) & 0xffff;
    rp_mask_x1 = packed & 0xffff;
    rp_run_on_stack(rp_masked_noarg);
    MASK_DONE = 1;
}

void RP_DispatchMasked(int x0, int x1)
{
    MASK_DONE = 0;
    rp_sgl_workptr_reset();
    slSlaveFunc(rp_masked_slave_body, (void *)(unsigned int)(((x0 & 0xffff) << 16) | (x1 & 0xffff)));
}

void RP_WaitMasked(void)
{
    volatile int guard = 30000000;
    while (!MASK_DONE && guard-- > 0) { }
    master_cache_purge();                     /* read the slave's drawn sprite pixels before the blit */
}

/* SATURN PERF: master frame ms, set once/sec by the platform fps_update
   (dg_saturn.cxx) and printed as the prefix of the slave's row-18 SLV line (the
   standalone MST row was dropped -- it only pointed at rows 19/20).  Defined
   unconditionally so the platform's extern links even when RP_PROF is off. */
unsigned int rp_master_ms = 0;

/* SATURN (VDP1-floor inc-0): the floor-quad estimate surfaced to a VISIBLE overlay row.
   Row 13 (FLAT) sits in the split-screen viewport band = unreadable, so the platform
   also prints these on row 2.  cur = this frame's VDP1 candidate cost (Vs); peak =
   monotonic worst case (the go/no-go number).  Defined unconditionally so the platform
   extern links even when RP_PROF is off (they then stay 0). */
int sat_floor_vq_cur  = 0;
int sat_floor_vq_peak = 0;

#if RP_PROF
/* Read the SH-2 free-running timer (FRC @ 0xFFFFFE12/13).  Read H then L so the
   low byte is latched coherently. */
static unsigned short rp_frt(void)
{
    unsigned char h = *(volatile unsigned char *)0xFFFFFE12;
    unsigned char l = *(volatile unsigned char *)0xFFFFFE13;
    return (unsigned short)((h << 8) | l);
}
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
static unsigned int   prof_wallprep;
static unsigned short prof_wp_t0;
/* SATURN PERF Phase-0a: finer Bp/P sub-splits (each a subset of Bp or P). */
static unsigned int   prof_segloop;     /* R_RenderSegLoop (per-column loop) c Bp */
static unsigned short prof_sl_t0;
static unsigned int   prof_flatalloc;   /* W_CacheLumpNum/Release per visplane c P  */
static unsigned short prof_fc_t0;
static unsigned int   prof_makespans;   /* R_MakeSpans walk + R_MapPlane span math c P */
static unsigned short prof_ms_t0;
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
static unsigned int   prof_floor_vq_int;      /* this frame: interior-only quad estimate */
static unsigned int   prof_floor_vq_int_peak; /* monotonic peak of the interior-only cost */
#define FLOOR_HBAND    16   /* screen rows per affine strip band (the Mode-7 strip granularity) */
#define FLOOR_MAXTILES 16   /* clamp on 64-texel u-tiles/band (the emitter would cap too) */
#endif

/* SATURN PERF 2.4 Stage 1: wall-prep timer, called by R_StoreWallRange (r_segs.c)
   on the master during the BSP walk.  A bare empty call unless RP_PROF.  Always
   defined so the shared core links on both ports regardless of the flag. */
void RP_WallPrepEnter(void)
{
#if RP_PROF
    prof_wp_t0 = rp_frt();
#endif
}
void RP_WallPrepLeave(void)
{
#if RP_PROF
    prof_wallprep += (unsigned short)(rp_frt() - prof_wp_t0);
#endif
}

/* SATURN PERF Phase-0a fine split (per-seg / per-visplane brackets; profiler).
   Always defined so the shared core links on both ports; no-op unless RP_PROF. */
void RP_SegLoopEnter(void)   {
#if RP_PROF
    prof_sl_t0 = rp_frt();
#endif
}
void RP_SegLoopLeave(void)   {
#if RP_PROF
    prof_segloop += (unsigned short)(rp_frt() - prof_sl_t0);
#endif
}
void RP_FlatCacheEnter(void) {
#if RP_PROF
    prof_fc_t0 = rp_frt();
#endif
}
void RP_FlatCacheLeave(void) {
#if RP_PROF
    prof_flatalloc += (unsigned short)(rp_frt() - prof_fc_t0);
#endif
}
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
#endif
void RP_PlanePixels(int picnum, int height, int minx, int maxx,
                    const unsigned char *top, const unsigned char *bottom)
{
#if RP_PROF
    unsigned int pix = 0u, vq = 0u;
    int x, ymin = 255, ymax = -1;
    for (x = minx; x <= maxx; x++)
    {
        unsigned int t = top[x];
        if (t != 0xffu)
        {
            unsigned int b = bottom[x];
            if (b >= t)
            {
                pix += b - t + 1u;
                if ((int)t < ymin) ymin = (int)t;     /* visplane screen-y extent */
                if ((int)b > ymax) ymax = (int)b;
            }
        }
    }
    /* VDP1-floor quad estimate (inc-0): bbox-clamped affine strips.  Split [ymin,ymax]
       into FLOOR_HBAND bands; per band the flat wraps every 64 texels across the bbox
       width -> tiles = u-span/64 + 1.  Mirrors what the emitter would produce. */
    if (ymax >= ymin)
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
    prof_plane_pix += pix;
    prof_floor_vq  += vq;
    /* interior = does NOT touch the near screen edge (bottom for a floor, top for a ceiling) */
    if (ymax >= ymin)
    {
        int is_floor  = (height < viewz);
        int near_edge = is_floor ? (ymax >= viewheight - 1) : (ymin <= 0);
        if (!near_edge) prof_floor_vq_int += vq;
    }
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
      ok=rp_wait(&SYNC->slave_opaque_done);
      prof_wait += (unsigned short)(rp_frt()-w); }
#else
    ok=rp_wait(&SYNC->slave_opaque_done);
#endif
    SYNC->go_masked=1;
    for (i=mat; i<tot; ++i) rp_exec(&cmds[i],0,columnofs);
#if RP_PROF
    if (ok) { unsigned short w=rp_frt();
              ok=rp_wait(&SYNC->slave_masked_done);
              prof_wait += (unsigned short)(rp_frt()-w); }
#else
    if (ok) ok=rp_wait(&SYNC->slave_masked_done);
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
    cm->f3=sat_wall_color;                 /* Potato walls: dominant colour (opaque only) */
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
    /* SATURN PERF 2.3: low-detail (detailshift!=0) now runs through the parallel
       path too (rp_exec dispatches to the *_low executors).  Previously this
       bailed to fully-serial master rendering, so low-detail had no working
       parallel mode at all. */
    if (rp_disabled) { rp_active=0;
#if RP_PROF
        p3_t_begin = rp_frt();   /* P3 profiler: frame start (parity rows are off here) */
        prof_wallprep = 0;       /* Bp accumulator (R_StoreWallRange); the parity reset is skipped here */
        /* RBG0/VDP1-floor sizing: the P3 path skips the main reset below, so reset here too
           (else the prof_plane and prof_floor counters accumulate across frames -> Vs/Vp
           would never be per-frame). */
        prof_plane_pix = prof_plane_dom = prof_plane_n = 0;
        prof_pp_cur_sum = prof_pp_cur_vq = 0;
        prof_pp_cur_pic = -2147483647;
        prof_pp_cur_h   = 0;
        prof_floor_vq = prof_floor_vq_dom = prof_floor_vq_int = 0;
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
    prof_begin = rp_frt();      /* recording starts now (slave runs in bg) */
    prof_wallprep = 0;          /* reset the per-frame wall-prep accumulator */
    prof_segloop = prof_flatalloc = prof_makespans = 0;   /* Phase-0a fine split */
    prof_plane_pix = prof_plane_dom = prof_plane_n = 0;   /* RBG0 candidate sizing */
    prof_pp_cur_sum = 0;
    prof_pp_cur_pic = -2147483647;   /* sentinel: no flat group open yet */
    prof_pp_cur_h   = 0;
    prof_floor_vq = prof_floor_vq_dom = prof_pp_cur_vq = 0;   /* VDP1 floor estimate (peak persists) */
    prof_floor_vq_int = 0;
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

void RP_BeginMasked(void)
{
#if RP_PROF
    if (rp_disabled) p3_t_planes = rp_frt();   /* P3: R_DrawPlanes done (parity path is off) */
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
    unsigned int w10  = p3_wait_ticks  * 10u / 224u;   /* master FRT -> ms (reliable) */
    extern int sat_masked_parallel;
    unsigned int rend = b10 + p10 + m10;                /* render = B+P+M (tenths-ms) */
    /* the slave is busy during P, and during M too once masked-by-half is on -> it idles only in
       B+M (planes only) or B (planes+masked).  idle% DROPS as each phase is offloaded. */
    unsigned int sidle = b10 + (sat_masked_parallel ? 0u : m10);
    unsigned int idle = rend ? (sidle * 100u / rend) : 0u;
    /* split B: Bp = wall-prep (R_StoreWallRange, the part the slave could take, d32xr-style);
       Bw = the BSP walk + clip + sprite projection (inherently serial). */
    unsigned int bp10 = prof_wallprep * 10u / 224u;
    unsigned int bw10 = (b10 > bp10) ? (b10 - bp10) : 0u;
    char p[44];
    snprintf(p, sizeof p, "Bw%u.%u Bp%u.%u P%u.%u M%u.%u",
             bw10/10,bw10%10, bp10/10,bp10%10, p10/10,p10%10, m10/10,m10%10);
    dbg_print(0, 5, p);
    /* w = master idle waiting for the slave (master FRT): w~0 => the slave keeps up / balanced.
       idle% = the slave's idle share of the render: it works in P, sits idle in B+M -> this is
       the slack masked + wall-prep will fill (it should DROP as each phase is offloaded). */
    snprintf(p, sizeof p, "MST%ums w%u.%u SLVidle%u%% ",
             rp_master_ms, w10/10, w10%10, idle);
    dbg_print(0, 3, p);   /* OVERLAY 2026-06-24: critical-path packed to rows 3-5 */
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
        if (prof_floor_vq_int > prof_floor_vq_int_peak) prof_floor_vq_int_peak = prof_floor_vq_int;
        sat_floor_vq_cur  = (int)vsec;
        sat_floor_vq_peak = (int)prof_floor_vq_peak;
        snprintf(p, sizeof p, "FLAT Vs%u Vp%u Vi%u      ",
                 vsec, prof_floor_vq_peak, prof_floor_vq_int_peak);
        (void)p;   /* FLAT (parked VDP1-floor telemetry) cut from overlay -- VDP1_FLOOR_PLAN.md */
    }
}
#endif

void RP_EndFrame(void)
{
    if (!rp_active) {
#if RP_PROF
        if (rp_disabled) rp_p3_prof_show();   /* P3: the only readout when parity is off */
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
        snprintf(p, sizeof p, "REC%u.%u EX%u.%u W%u.%u c%-4d ",
                 rec10/10, rec10%10, exe10/10, exe10%10,
                 wai10/10, wai10%10, rec_count);
        dbg_print(0, 4, p);
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
            dbg_print(0, 5, p);
            /* Phase-0a fine split (rows 11/12).  Bp -> setup (per-seg trig/scale)
               + loop (R_RenderSegLoop per-column).  P -> alloc (flat W_Cache/
               Release) + makespans (R_MakeSpans walk + R_MapPlane) + other. */
            {
                unsigned int sll  = prof_segloop;
                unsigned int slps = (bpt > sll) ? (bpt - sll) : 0u;   /* Bp setup */
                unsigned int bps10 = slps * 10u / 224u;
                unsigned int bpl10 = sll  * 10u / 224u;
                unsigned int ptot = (unsigned short)(prof_planes_end - prof_bsp_end);
                unsigned int pa   = prof_flatalloc;
                unsigned int pm   = prof_makespans;
                unsigned int po   = (ptot > pa + pm) ? (ptot - pa - pm) : 0u;
                unsigned int pa10 = pa * 10u / 224u;
                unsigned int pm10 = pm * 10u / 224u;
                unsigned int po10 = po * 10u / 224u;
                snprintf(p, sizeof p, "BP s%u.%u l%u.%u   ",
                         bps10/10, bps10%10, bpl10/10, bpl10%10);
                dbg_print(0, 11, p);
                snprintf(p, sizeof p, "P a%u.%u m%u.%u o%u.%u ",
                         pa10/10, pa10%10, pm10/10, pm10%10, po10/10, po10%10);
                dbg_print(0, 12, p);
            }
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
            unsigned int idle = (st > sd) ? ((st - sd) * 100u / st) : 0u;
            snprintf(p, sizeof p, "MST%ums SLV i%u%% b%u%% t%u d%u  ",
                     rp_master_ms, idle, busy, st, sd);
            dbg_print(0, 3, p);   /* OVERLAY 2026-06-24: critical-path packed to rows 3-5 */
        }
    }
#endif
    colfunc=saved_col; basecolfunc=saved_base;
    fuzzcolfunc=saved_fuzz; transcolfunc=saved_trans; spanfunc=saved_span;
    rp_active=0;
}
