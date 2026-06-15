/*
** Shared dual-SH2 renderer back end (doom-saturn-core).
**
** Pure C, SDK-agnostic: the command queue, executors, sync protocol and
** cache-coherency rules are all hardware-level (SH-2 / SGL), so this exact
** file is compiled by BOTH the SRL (DoomSRL) and Jo Engine (SaturnDoom)
** ports.  The only platform touch-points are:
**   - slSlaveFunc()  : SGL, linked by both ports
**   - jo_print()     : a thin debug-overlay shim each port provides
**                      (SRL::Debug::Print on DoomSRL, native on SaturnDoom)
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

/* Platform-provided (SGL on both ports; jo_print implemented per platform). */
extern void slSlaveFunc(void (*func)(void *), void *param);
extern void jo_print(int x, int y, char *str);

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
/* Potato: one FIXED texel of the 64x64 flat (centre = v32,u32 = 32*64+32) as the
   span's base colour.  Using a fixed texel (not the view-dependent span-start one)
   makes the whole flat a single colour that does NOT shift/rotate as the player
   turns; distance fog is still applied per span via the colormap. */
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
static int  rp_disabled;
static int  rp_consec_timeouts = 0;   /* slave timeouts in a row; re-arm unless persistent */
int rp_timeout_count = 0;

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

    {
        int guard = 100000;         /* anti-wedge: bound pure-spin on masked_at
                                       (~tens of ms worst case; resets on any
                                       progress, so legit brief waits don't trip) */
        for (;;)
        {
            int i0 = i;
            opq=SYNC->masked_at;
            lim=(opq>=0 && opq<SYNC->ready) ? opq : SYNC->ready;
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
        }
    }
    SYNC->slave_opaque_done=1;

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
static void rp_sgl_workptr_reset(void)
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
    /* SATURN PERF 2.5: two-pointer EX drain.  m_pos=-1 so the slave can draw down
       to index 0; s_pos = a sentinel above any possible mat so the master can draw
       forward freely until the slave starts decrementing s_pos.  Set before the
       slave is dispatched, so it sees initialised values. */
    SYNC->m_pos=-1;
    SYNC->s_pos=0x7fffffff;
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
#endif

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
            jo_print(0, 20, t);
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
        jo_print(0, 15, d);
        snprintf(d, sizeof d, "CCR m%02x s0%02x s1%02x        ",
                 SYNC->master_ccr & 0xff, SYNC->slave_ccr0 & 0xff,
                 SYNC->slave_ccr1 & 0xff);
        jo_print(0, 16, d);
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
        jo_print(0, 2, dbg);
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
    if (rp_disabled) { rp_active=0; return; }
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
#endif
}

/* SATURN PERF 2.4 Stage 0: called from R_RenderPlayerView right after the BSP
   walk, before R_DrawPlanes.  Captures the BSP/planes boundary for the row-20
   B/P/M breakdown.  No-op (and free) unless RP_PROF; always defined so the
   shared r_main.c can call it unconditionally on both ports. */
void RP_MarkBSPDone(void)
{
#if RP_PROF
    if (rp_active && !rp_disabled) prof_bsp_end = rp_frt();
#endif
}

void RP_BeginMasked(void)
{
    if (!rp_active||rp_disabled) return;
#if RP_PROF
    prof_planes_end = rp_frt();   /* R_DrawPlanes done; masked gen starts next */
#endif
    in_masked=1; rec_masked_at=rec_count;
    __asm__ volatile("":::"memory");
    SYNC->ready=rec_count;
    SYNC->masked_at=rec_count;
}

void RP_EndFrame(void)
{
    if (!rp_active) return;
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
        jo_print(0, 19, p);
        /* Row 20 (SATURN PERF 2.4 Stage 0): REC split into BSP / planes / masked
           generation sub-times -> tells us which phase owns REC's ~50-100ms and
           is worth offloading to the slave.  (The wedge GRD diagnostic that lived
           here is gone -- slave reliable; the TMO timeout path still writes row 20
           on the ~never timeout, so a regression would still surface.) */
        {
            unsigned int b10 = (unsigned short)(prof_bsp_end    - prof_begin)     * 10u / 224u;
            unsigned int p10 = (unsigned short)(prof_planes_end - prof_bsp_end)   * 10u / 224u;
            unsigned int m10 = (unsigned short)(prof_recend     - prof_planes_end)* 10u / 224u;
            snprintf(p, sizeof p, "B%u.%u P%u.%u M%u.%u   ",
                     b10/10, b10%10, p10/10, p10%10, m10/10, m10%10);
            jo_print(0, 20, p);
        }
    }
#endif
    colfunc=saved_col; basecolfunc=saved_base;
    fuzzcolfunc=saved_fuzz; transcolfunc=saved_trans; spanfunc=saved_span;
    rp_active=0;
}
