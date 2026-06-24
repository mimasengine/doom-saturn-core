//
// r_cache.h -- SATURN bounded texture-composite cache (LRU).
//
// Caps and recency-evicts multi-patch wall composites in a sub-zone carved from
// leftover main-zone RAM, so big-WAD CD-streaming play stops thrashing
// composites (evict -> rebuild -> patch re-read from CD) frame to frame.
// Ported from d32xr's r_cache.c, adapted to this core's classic composite model
// (texturecomposite[] directory + R_GenerateComposite producer).
//
// Active ONLY when sat_streaming_mode (the big-WAD CD path) and NOT during an
// x-split parallel pass (the slave must never touch the allocator).  DoomJo
// leaves sat_streaming_mode 0 -> the cache is inert and behaviour is identical.
// Graceful by design: if the pool can't be carved or is full, allocation
// returns NULL and the caller uses the classic main-zone PU_CACHE path, so the
// worst case is exactly today's behaviour.
//
// Pure C (compiles under DoomJo's GCC 9.3).
//
#ifndef __R_CACHE__
#define __R_CACHE__

#include "doomtype.h"

// Per-level pool lifecycle (called from P_SetupLevel).
void  R_ClearTextureCaches (void);   // free the pool + NULL evicted-entry back-pointers
void  R_SetupTextureCaches (void);   // carve a fresh pool from leftover main-zone RAM

// Composite cache (called from r_data.c: R_GenerateComposite / R_GetColumn).
// R_TexCacheAlloc returns the composite data area (and publishes it through
// *userp) or NULL if the cache is inactive/full -> caller falls back.
byte *R_TexCacheAlloc (int size, void **userp);
void  R_TexCacheTouch (byte *data);             // recency bump on a cache hit

// Per-view aging (called from R_RenderPlayerView, once per view).
void  R_PostTexCacheFrame (void);

extern int sat_texcache_active;       // 1 while a pool is live this level
extern int sat_texcache_poolkb;       // pool size (KB), 0 if none
extern int sat_texcache_entries;      // live composites in the pool
extern int sat_texcache_builds;       // cumulative pool allocs this level (misses built in)
extern int sat_texcache_evicts;       // cumulative evictions this level

#endif
