//
// r_flatcache.c -- SATURN resident flat pool (LRU).  See r_flatcache.h for the
// defect this fixes (the "flat treadmill") and why it is a contiguous slab rather
// than a set of scattered PU_STATIC pins.
//
// Pure C (compiles under DoomJo's GCC 9.3).
//
#include "r_flatcache.h"
#include "z_zone.h"
#include "w_wad.h"

// --- tunables ---------------------------------------------------------------

#define FLATCACHE_FLATSZ  4096       // a Doom flat is ALWAYS 64x64 raw indices; R_DrawSpan's
                                     //   ((yfrac>>10)&0x0fc0)+((xfrac>>16)&0x3f) assumes it.
                                     //   Anything else in the F_ namespace is not poolable.
#define FLATCACHE_MAX     16         // top rung.  Beyond this the slab competes with the
                                     //   texcache slab and the play tail for the same run,
                                     //   and the marginal hit rate is small (a viewpoint
                                     //   sees 2..6 distinct flats).

// Adaptive ladder, same shape as r_cache.c's: take the biggest (slots, margin) rung
// whose slab AND its play-tail margin both still fit in the largest contiguous run.
// The margin is what serves the big ~35 KB sky/face patches and the mid-play MUS lump,
// so its floor stays comfortably above them -- carving a slab that leaves no such tail
// is how r_cache.c's first version produced contiguous-OOM.
static const int flatcache_rungs[4][2] =
{
    { 16, 96*1024 },                 // 64 KB slab -- a couple of rooms
    { 12, 80*1024 },                 // 48 KB
    {  8, 64*1024 },                 // 32 KB -- still kills a static viewpoint's treadmill
    {  5, 48*1024 },                 // 20 KB floor; below this, run pool-less (today's behaviour)
};

extern int sat_streaming_mode;       // p_setup.c -- big-WAD CD path
extern int sat_xsplit;               // r_main.c  -- parallel x-split render in progress

int sat_flatcache_on    = 1;         // live A/B bypass (pad R+Z)
int sat_flatcache_slots = 0;
int sat_flatcache_live  = 0;
int sat_flatcache_load  = 0;
int sat_flatcache_full  = 0;

static byte *fc_slab = NULL;                    // the one contiguous PU_STATIC block
static int   fc_lump[FLATCACHE_MAX];            // lump in each slot, -1 = empty
static int   fc_age [FLATCACHE_MAX];            // views since last touched (0 = THIS view)


// --- per-level lifecycle ----------------------------------------------------

void R_ClearFlatCache (void)
{
    if (fc_slab)
        Z_Free (fc_slab);
    fc_slab = NULL;
    sat_flatcache_slots = 0;
    sat_flatcache_live  = 0;
    sat_flatcache_load  = 0;
    sat_flatcache_full  = 0;
}

void R_SetupFlatCache (void)
{
    int largest, i, n = 0;

    R_ClearFlatCache ();             // drop any prior slab (no-op the first time)

    // Cart / DoomJo: W_CacheLumpNum already returns a direct mapped pointer, nothing
    // is ever purged and there is no disc to re-read from -- the pool would be pure
    // waste.  Only the CD-streaming build has the treadmill.
    if (!sat_streaming_mode)
        return;

    largest = Z_LargestAllocatable ();
    for (i = 0; i < 4; i++)
        if (largest >= flatcache_rungs[i][0] * FLATCACHE_FLATSZ + flatcache_rungs[i][1])
        {
            n = flatcache_rungs[i][0];
            break;
        }
    if (!n)
        return;                      // zone too tight -> pool-less, the proven-playable baseline

    fc_slab = (byte *)Z_Malloc (n * FLATCACHE_FLATSZ, PU_STATIC, NULL);
    for (i = 0; i < n; i++)
    {
        fc_lump[i] = -1;
        fc_age [i] = 0;
    }
    sat_flatcache_slots = n;
}


// --- lookup / fill / evict --------------------------------------------------

byte *R_FlatCachePeek (int lumpnum)
{
    int i;

    if (!fc_slab || !sat_flatcache_on)
        return NULL;
    for (i = 0; i < sat_flatcache_slots; i++)
        if (fc_lump[i] == lumpnum)
            return fc_slab + i * FLATCACHE_FLATSZ;
    return NULL;
}

byte *R_FlatCacheGet (int lumpnum)
{
    int i, victim, oldest;

    if (!fc_slab || !sat_flatcache_on)
        return NULL;

    // Hit: bump recency and hand back the slot.  Linear over <= 16 entries, once per
    // VISPLANE (not per span, not per pixel) -- immaterial next to a 4 KB flat draw.
    for (i = 0; i < sat_flatcache_slots; i++)
        if (fc_lump[i] == lumpnum)
        {
            fc_age[i] = 0;
            return fc_slab + i * FLATCACHE_FLATSZ;
        }

    // Only 64x64 flats are poolable (fixed slot size; and R_DrawSpan assumes 4096).
    if (W_LumpLength ((unsigned int)lumpnum) != FLATCACHE_FLATSZ)
        return NULL;

    // Everything past this point can FILL a slot -- i.e. do disc I/O and mutate the table.
    // The slave SH-2 must never do either, so an x-split pass takes the classic path (the
    // same guard r_cache.c uses).  Inert today (SAT_XSPLIT 0), explicit for whoever revives it.
    if (sat_xsplit)
        return NULL;

    // Miss: take an empty slot, else the least-recently-used one.  age == 0 means the
    // slot was touched THIS view, so a queued visplane may still hold a raw pointer
    // into it (the worklist invariant in r_flatcache.h) -- those are untouchable.
    victim = -1;
    for (i = 0; i < sat_flatcache_slots; i++)
        if (fc_lump[i] < 0)                  { victim = i; break; }   // empty: always take it
    if (victim < 0)
        for (i = 0, oldest = 0; i < sat_flatcache_slots; i++)
            if (fc_age[i] > oldest)          { victim = i; oldest = fc_age[i]; }
    if (victim < 0)
    {
        sat_flatcache_full++;                // every slot busy this view -> classic path
        return NULL;
    }

    /* (sat_flatcache_evict REMOVED 2026-08-26 -- row-19 `ev` had no printer.) */
    if (fc_lump[victim] < 0)
        sat_flatcache_live++;

    // THE disc read -- once per flat per residency, instead of once per plane per frame.
    W_ReadLump ((unsigned int)lumpnum, fc_slab + victim * FLATCACHE_FLATSZ);
    fc_lump[victim] = lumpnum;
    fc_age [victim] = 0;
    sat_flatcache_load++;
    return fc_slab + victim * FLATCACHE_FLATSZ;
}

void R_PostFlatCacheFrame (void)
{
    int i;

    if (!fc_slab)
        return;
    // Runs at the TOP of R_RenderPlayerView, before the BSP walk re-touches this
    // view's flats -- so "age 0" is exactly "in use by the view being drawn".
    for (i = 0; i < sat_flatcache_slots; i++)
        if (fc_lump[i] >= 0 && fc_age[i] < 0x7ffffff0)
            fc_age[i]++;
}
