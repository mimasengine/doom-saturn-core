//
// r_cache.c -- SATURN bounded texture-composite cache (LRU).
//
// See r_cache.h.  A miss in CD-streaming mode means a composite rebuild (and a
// CD re-read of its patches), so keeping recently-visible composites resident
// in a bounded, recency-evicted sub-zone amortizes the CD traffic and caps the
// graphics working set to roughly the visible set -- the d32xr r_cache.c model,
// adapted to this core's texturecomposite[] directory.
//
// The pool is a self-contained sub-zone (z_zone multi-zone extensions) laid
// over one PU_STATIC slab carved from leftover main-zone RAM after the level's
// geometry has loaded.  Each entry is [texcache_hdr_t][composite pixels]; the
// header's userp points at the owner's directory slot (&texturecomposite[tex])
// so eviction can NULL it and R_GetColumn transparently rebuilds on next use.
//
// Pure C (compiles under DoomJo's GCC 9.3).
//
// ---------------------------------------------------------------------------
// Portions of this file are derived from d32xr's r_cache.c, used under the MIT
// License:
//
//   The MIT License (MIT)
//   Copyright (c) 2021 Victor Luchits, Derek John Evans, id Software and
//   ZeniMax Media
//
//   Permission is hereby granted, free of charge, to any person obtaining a
//   copy of this software and associated documentation files (the "Software"),
//   to deal in the Software without restriction, including without limitation
//   the rights to use, copy, modify, merge, publish, distribute, sublicense,
//   and/or sell copies of the Software, and to permit persons to whom the
//   Software is furnished to do so, subject to the following conditions:
//
//   The above copyright notice and this permission notice shall be included in
//   all copies or substantial portions of the Software.
//
//   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
//   IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
//   FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
//   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
//   LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
//   FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
//   DEALINGS IN THE SOFTWARE.
// ---------------------------------------------------------------------------
//
#include "r_cache.h"
#include "z_zone.h"

// --- tunables (Ymir-validate; all guarded by the graceful fallback) ---------
#ifndef TEXCACHE_LIFE
#define TEXCACHE_LIFE   3            // frames a composite survives unseen (d32xr CACHE_FRAMES)
#endif
#ifndef TEXCACHE_MARGIN
#define TEXCACHE_MARGIN (128*1024)   // main-zone RAM reserved for runtime PU_CACHE
#endif                               //   (sprites/flats/sfx/UI/savegame) during play
#ifndef TEXCACHE_MAX
#define TEXCACHE_MAX    (256*1024)   // cap on the pool (no point exceeding the visible set)
#endif
#ifndef TEXCACHE_MIN
#define TEXCACHE_MIN    (24*1024)    // below this the pool is not worth carving -> run cacheless
#endif
#ifndef TEXCACHE_FIXED
#define TEXCACHE_FIXED  (96*1024)    // FIXED slab size (2p/4p).  Carving leftover-minus-MARGIN
#endif                               //   grabbed almost all spare and left only a MARGIN-sized
                                     //   free-tail, which the big ~35K sky/face patches + the
                                     //   mid-play MUS lump fragmented below 35K -> contiguous-OOM.
                                     //   A small FIXED slab leaves (leftover - FIXED) as ONE large
                                     //   contiguous tail for those allocations (the boot-carve's
                                     //   essential property, realizable per-level since the player
                                     //   count is only known at level load, not at boot).

#define TEXCACHE_MAGIC  0x7A436163   /* 'zCac' -- marks a managed cache block */

typedef struct
{
    int    magic;
    void **userp;       // &texturecomposite[tex]; NULLed on evict so R_GetColumn rebuilds
    int    lifecount;   // touch -> TEXCACHE_LIFE ; -- each view ; <=0 => evictable
} texcache_hdr_t;

extern int sat_streaming_mode;   // p_setup.c -- big-WAD CD path
extern int sat_xsplit;           // r_main.c  -- parallel x-split render in progress
extern int sat_local_players;    // platform  -- >1 => local split-screen

int sat_texcache_active  = 0;   // 1 while a pool is live this level
int sat_texcache_poolkb  = 0;   // pool size in KB (0 if no pool)
int sat_texcache_entries = 0;   // live composites currently in the pool
int sat_texcache_builds  = 0;   // cumulative pool allocs this level (misses built in)
int sat_texcache_evicts  = 0;   // cumulative evictions this level
int sat_texcache_carve_lf= 0;   // SATURN diag: largest free block (KB) at the last carve attempt
                                //   (overlay TXC row -- a0 with lf<~88K = zone too tight, not a bug)

static void *texcache_zone = NULL;   // opaque sub-zone handle (Z_InitZone)
static void *texcache_slab = NULL;   // the main-zone block backing the sub-zone


// --- per-level lifecycle ----------------------------------------------------

static void R_TexCacheClearCB (void *payload, void *userp)
{
    texcache_hdr_t *h = (texcache_hdr_t *)payload;
    (void)userp;
    if (h->magic == TEXCACHE_MAGIC && h->userp)
        *h->userp = NULL;            // drop the soon-to-dangle composite pointer
}

void R_ClearTextureCaches (void)
{
    if (texcache_zone)
        Z_ForEachBlock (texcache_zone, R_TexCacheClearCB, NULL);
    if (texcache_slab)
        Z_Free (texcache_slab);
    texcache_zone = NULL;
    texcache_slab = NULL;
    sat_texcache_active = 0;
    sat_texcache_poolkb = 0;
    sat_texcache_entries = 0;
}

void R_SetupTextureCaches (void)
{
    // SATURN R4.4: adaptive slab ladder.  Take the biggest (slab,margin) rung that
    // fits Z_LargestAllocatable, instead of the old all-or-nothing FIXED+MARGIN carve
    // that ran split-screen CACHELESS on big WADs (the 96K+128K=224K contiguous run
    // never exists on TNT/Plutonia and 23/32 Doom II maps -- exactly where the 2-4p
    // anti-frag slab is needed).  Each rung's margin floor stays >= the ~35K sky/face
    // contiguous patch (smallest margin = 64K) so the play tail never re-fragments
    // below it.  Rungs derive from TEXCACHE_FIXED/MARGIN so the Makefile override
    // still tunes the top rung.  DoomJo (sat_streaming_mode==0) never reaches here.
    static const int rungs[3][2] = {
        { TEXCACHE_FIXED,       TEXCACHE_MARGIN       },   // 96K + 128K (best)
        { TEXCACHE_FIXED*2/3,   TEXCACHE_MARGIN*3/4   },   // 64K + 96K
        { TEXCACHE_FIXED/2,     TEXCACHE_MARGIN/2     },   // 48K + 64K (floor)
    };
    int largest, sz, i;

    R_ClearTextureCaches ();          // drop any prior pool (no-op the first time)

    // CD-streaming, SPLIT-SCREEN ONLY (2p/4p).  The slab is a ~150K PU_STATIC
    // reservation AND a mid-zone wall: in 1p it STARVES the general PU_CACHE
    // (sprites/flats stream lazily) and fragments the zone -> measured WORSE than
    // cacheless (zone-change stutter + the 35K sky/face-patch contiguous-OOM).  So
    // 1p streaming runs cacheless (the proven-playable baseline; composites are the
    // cheaper, not the binding, working set in 1p).  Split-screen genuinely needs
    // it: 2-4 viewports reference disjoint texture sets whose interleaved purgeable
    // composites fragment the heap -> the contiguous slab segregates them and FIXES
    // the 2p composite-fragmentation OOM.  Per-view aging is fine at 2 views (a
    // composite seen by EITHER player is re-touched); 3-4p would want once-per-frame
    // aging.  The alloc path still gates on sat_xsplit.  DoomJo (sat_streaming_mode
    // == 0) is unaffected.
    if (!sat_streaming_mode || sat_local_players <= 1)
        return;

    // `largest` = the run Z_Malloc could hand out after purging PU_CACHE
    // (Z_LargestAllocatable, not Z_LargestFreeBlock which counts only the tiny PU_FREE
    // runs at level start and kept TXC a0).  Carve the biggest slab rung whose slab +
    // its play-tail margin still fit -- that tail is what serves the big ~35K sky/face
    // patches + the mid-play MUS lump.  Below the smallest rung the tightest maps run
    // cacheless (the proven-playable baseline).
    largest = Z_LargestAllocatable ();
    sat_texcache_carve_lf = largest >> 10;   // diag: the run the carve saw (overlay TXC lf)
    sz = 0;
    for (i = 0; i < 3; i++)
        if (largest >= rungs[i][0] + rungs[i][1]) { sz = rungs[i][0]; break; }
    if (!sz)
        return;                       // even the 48K rung won't fit -> cacheless

    texcache_slab = Z_Malloc (sz, PU_STATIC, NULL);  // non-purgable; we own its lifetime
    texcache_zone = Z_InitZone (texcache_slab, sz);
    sat_texcache_active  = 1;
    sat_texcache_poolkb  = sz >> 10;
    sat_texcache_entries = 0;
    sat_texcache_builds  = 0;
    sat_texcache_evicts  = 0;
}


// --- allocate / evict / touch / age ----------------------------------------

static void R_TexCacheEvictCB (void *payload, void *userp)
{
    texcache_hdr_t *h = (texcache_hdr_t *)payload;
    (void)userp;
    if (h->magic != TEXCACHE_MAGIC)
        return;
    if (h->lifecount <= 0)            // not seen for TEXCACHE_LIFE views -> reclaim
    {
        if (h->userp)
            *h->userp = NULL;
        Z_Free2 (texcache_zone, payload);
        sat_texcache_entries--;
        sat_texcache_evicts++;
    }
}

byte *R_TexCacheAlloc (int size, void **userp)
{
    texcache_hdr_t *h;
    byte           *blk;
    int             need;

    if (!sat_texcache_active || sat_xsplit)
        return NULL;                  // inactive, or a parallel x-split pass owns the allocator

    need = size + (int)sizeof(texcache_hdr_t);

    blk = (byte *)Z_Malloc2 (texcache_zone, need, PU_STATIC);
    if (!blk)
    {
        Z_ForEachBlock (texcache_zone, R_TexCacheEvictCB, NULL);  // reclaim stale entries
        blk = (byte *)Z_Malloc2 (texcache_zone, need, PU_STATIC);
    }
    if (!blk)
        return NULL;                  // still no room -> caller uses the classic path

    h = (texcache_hdr_t *)blk;
    h->magic     = TEXCACHE_MAGIC;
    h->userp     = userp;
    h->lifecount = TEXCACHE_LIFE;

    blk += sizeof(texcache_hdr_t);
    if (userp)
        *userp = blk;                 // publish the composite pointer (texturecomposite[tex])
    sat_texcache_entries++;
    sat_texcache_builds++;
    return blk;
}

void R_TexCacheTouch (byte *data)
{
    texcache_hdr_t *h;

    if (!sat_texcache_active || sat_xsplit || !data)
        return;                       // never touch the pool during a parallel x-split pass
    // data-12 lands on our header for a pool block, or harmlessly inside the
    // preceding 24-byte memblock_t header (id==ZONEID) for a classic main-zone
    // fallback composite -- whose magic never matches, so it is skipped.  Relies
    // on sizeof(memblock_t) (24) >= sizeof(texcache_hdr_t) (12).
    h = (texcache_hdr_t *)data - 1;
    if (h->magic == TEXCACHE_MAGIC)   // skip classic-path (main-zone) composites
        h->lifecount = TEXCACHE_LIFE;
}

static void R_TexCacheAgeCB (void *payload, void *userp)
{
    texcache_hdr_t *h = (texcache_hdr_t *)payload;
    (void)userp;
    if (h->magic == TEXCACHE_MAGIC && h->lifecount > 0)
        h->lifecount--;
}

void R_PostTexCacheFrame (void)
{
    if (!sat_texcache_active)
        return;
    Z_ForEachBlock (texcache_zone, R_TexCacheAgeCB, NULL);
}
