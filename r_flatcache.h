//
// r_flatcache.h -- SATURN resident flat pool: the fix for the "flat treadmill".
//
// THE DEFECT.  A visplane does W_CacheLumpNum(flat, PU_STATIC) ... draw ...
// W_ReleaseLumpNum(flat), and W_ReleaseLumpNum does Z_ChangeTag(cache, PU_CACHE).
// So the lock a visible plane holds on its flat is demoted to PURGEABLE after every
// plane, every frame.  Z_Malloc's rover then purges blindly, in ADDRESS order: it
// has no idea the 4 KB block it just reclaimed is the floor the player is standing
// on.  Next frame the flat is gone and costs a synchronous ~42 ms CD read, charged
// to `P`.  Measured on TNT MAP11: 80..221 non-resident flat fetches PER SECOND, on a
// level that only owns ~40 distinct flats.  That is the treadmill.
//
// THE FIX.  Take flats out of the zone entirely.  One CONTIGUOUS PU_STATIC slab of
// N fixed 4096-byte slots, filled by W_ReadLump directly, recency-evicted (LRU).  A
// pooled flat never allocates, never fragments, and is never purged behind our back,
// so a flat the player keeps looking at is read from the disc ONCE.
//
// Why a slab and not just "leave the lock at PU_STATIC": r_cache.c already learned
// (the hard way, 1p) that scattered PU_STATIC blocks are WALLS that fragment the
// zone -- and N scattered 4 KB pins is precisely that.  One contiguous slab with
// uniform slots has zero internal fragmentation and puts exactly ONE wall in the
// zone.  It also REMOVES a fragmentation engine: the ~200/s 4 KB alloc/purge cycle
// the treadmill was running through the main zone stops entirely.
//
// SIZING (measured over the witness WADs, distinct flats per map from SECTORS):
//   Doom1 med 22 / max 28   TNT med 31 / max 55   Plutonia med 17 / max 28
//   Doom2 med 29 / max 49   Scythe med 13 / max 27   HR med 23 / max 41
// A LEVEL's flats are 68..124 KB -- far too much to hold.  So the pool does not
// target the level, it targets the NEIGHBOURHOOD: a viewpoint sees 2..6 distinct
// flats, a couple of rooms ~10..16.  The LRU evicts the room you left.  Even a
// small pool kills the treadmill; the top rung (16 slots) covers moving play.
//
// Active only in sat_streaming_mode (the big-WAD CD path).  DoomJo leaves that 0,
// so R_SetupFlatCache carves nothing, R_FlatCacheGet returns NULL, and every caller
// takes the classic W_CacheLumpNum path -- byte-identical behaviour there.
// Graceful by design: no pool, or a pool with every slot in use this view, returns
// NULL and the caller falls back, so the worst case is exactly today's behaviour.
//
// Pure C (compiles under DoomJo's GCC 9.3).
//
#ifndef __R_FLATCACHE__
#define __R_FLATCACHE__

#include "doomtype.h"

// Per-level pool lifecycle (called from P_SetupLevel, beside the r_cache pair).
void  R_ClearFlatCache (void);       // release the slab BEFORE this level's geometry loads
void  R_SetupFlatCache (void);       // carve a fresh slab from what geometry left over

// R_FlatCachePeek: the slot pointer if this flat is ALREADY resident, else NULL.
//   Never loads, never evicts -- this is the "would a fetch hit the disc?" predicate
//   the per-frame load budget must consult before W_LumpResident (a pooled flat is
//   NOT in lumpinfo[].cache, so W_LumpResident alone would answer wrongly).
// R_FlatCacheGet: the slot pointer, loading the flat from the WAD on a miss.
//   NULL means "not poolable" -- no pool, not a 4096-byte lump, or every slot was
//   already touched THIS view (see the worklist invariant below) -> caller falls back.
byte *R_FlatCachePeek (int lumpnum);
byte *R_FlatCacheGet  (int lumpnum);

// Per-view aging (called from R_RenderPlayerView, beside R_PostTexCacheFrame).
//
// WORKLIST INVARIANT: R_DrawPlanes queues planes with a raw `w->src` pointer that
// the SLAVE dereferences after the loop.  A slot touched this view therefore MUST
// NOT be reused under it -- so eviction only ever takes a slot with age > 0, and
// aging runs once per view BEFORE the BSP walk.  Correct by construction, no
// cross-CPU handshake needed.
void  R_PostFlatCacheFrame (void);

extern int sat_flatcache_on;       // live A/B bypass (pad R+Z).  The slab stays carved
                                   //   either way, so both sides of the A/B have an
                                   //   IDENTICAL memory layout -- see [[interbuild-perf-noise]].
extern int sat_flatcache_slots;    // slots carved this level (0 = no pool)
extern int sat_flatcache_live;     // slots currently holding a flat
extern int sat_flatcache_load;     // cumulative slot fills = the REAL disc reads for flats
extern int sat_flatcache_full;     // times every slot was busy this view -> classic path

#endif
