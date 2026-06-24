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
    int largest, sz;

    R_ClearTextureCaches ();          // drop any prior pool (no-op the first time)

    // CD-streaming only (the cache helps the big-WAD streaming working set).  Now
    // ALSO active in local split-screen: with the visplane pool freeing the floor,
    // the contiguous slab is what FIXES the 2p composite-fragmentation OOM and
    // amortizes the CD reads behind the room-change stutters.  Per-view aging is
    // fine at 2 views (a composite seen by EITHER player is re-touched and
    // survives); 3-4p would want once-per-frame aging.  The alloc path still gates
    // on sat_xsplit so the slave never mutates the zone during a parallel pass.
    if (!sat_streaming_mode)
        return;

    // Size the slab against the largest run Z_Malloc could actually hand out -- free
    // PLUS purgeable (Z_Malloc purges PU_CACHE to make room).  NOT Z_LargestFreeBlock,
    // which counts only PU_FREE: at level-start the zone is full of just-loaded
    // PU_CACHE graphics, so it read ~10K (overlay lf10K) and the carve ALWAYS bailed,
    // even though ~232K (overlay ZON mx) was reclaimable.  This is what kept TXC a0.
    largest = Z_LargestAllocatable ();
    sat_texcache_carve_lf = largest >> 10;   // diag: the run the carve saw (overlay TXC lf)
    sz = largest - TEXCACHE_MARGIN;   // leave headroom for runtime PU_CACHE allocations
    if (sz < TEXCACHE_MIN)
        return;                       // too little spare -> run cacheless (classic behaviour)
    if (sz > TEXCACHE_MAX)
        sz = TEXCACHE_MAX;

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
