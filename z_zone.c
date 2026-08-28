//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	Zone Memory Allocation. Neat.
//


#include "z_zone.h"
#include "i_system.h"
#include "doomtype.h"

/* SATURN 2026-08-14: profiler max-timers (r_parallel.c).  No-ops unless RP_PROF and unless the
   caller is inside the wall-prep bracket, so this costs two FRT reads on the render path only. */
void RP_StampBegin (int slot);
void RP_StampEnd (int slot);

// SATURN: in cart mode W_CacheLumpNum returns memory-mapped (cart) lump pointers,
// which are NOT zone blocks.  Z_Free/Z_Free2 use this to no-op a stray free of such
// a lump instead of crashing ("Z_Free without ZONEID").  Inert when nothing mapped.
extern boolean W_PtrIsMapped(const void *p);


//
// ZONE MEMORY ALLOCATION
//
// There is never any space between memblocks,
//  and there will never be two contiguous free memblocks.
// The rover can be left pointing at a non-empty block.
//
// It is of no value to free a cachable block,
//  because it will get overwritten automatically if needed.
// 
 
#define MEM_ALIGN sizeof(void *)
#define ZONEID	0x1d4a11

// SATURN diag (SAT_ZONE_RA): tag every block with its Z_Malloc caller so the top-8 resident
// dump can NAME the big blocks (resolve ra vs build/Mimas.map) -> attribute the RAM-diet targets.
// +4 bytes/block header (a few KB) -- keep ON while dieting the zone, flip to 0 to ship.
/* SATURN 2026-08-06: flipped to 0.  The `ra` alloc-site tag did its job -- it named P_LoadSegs as
   the TNT MAP19 boot failure ([[zone-contiguity-wall-loadsegs]]) -- and the current hunt is entirely
   inside the VDP1 wall kick, with no zone component.  Off it costs nothing to re-enable, and it buys
   back HWRAM .text (the top-8 forensics dump + its printf format strings) for the TLSF pool, which
   the pre-flight had pushed below its 4 KB floor.  The halt still prints fr/lg/st/lv AND the top-8
   block sizes/tags/offsets -- only the caller address is gone. */
/* SATURN 2026-08-07: ON for one build, then OFF again -- it answered completely.  The eight walls
   of TNT MAP11, resolved from `ra` against build/Mimas-Tnt.map, so nobody re-measures them:
     101K PU_LEVEL  P_LoadLineDefs      85K PU_LEVEL  P_LoadSegs
      40K PU_LEVEL  P_LoadNodes         23K PU_LEVEL  P_LoadSectors      (= 249K of map geometry)
      72K PU_STATIC W_AddFile (lumpinfo, bottom of zone -- structural)
      60K PU_STATIC R_InitPlanes  <- the visplane span pool, the one big STATIC sitting MID-ZONE
      36K PU_STATIC the lead-fill ring (r_segs, bottom of zone)
      26K PU_STATIC the .DRP entry table (moved to the bottom the same day)
   VERDICT: none of it is junk.  st438K + lv377K = 815 KB of an ~864 KB zone -- the zone is FULL,
   not merely fragmented.  Flip this back to 1 only for a NEW unexplained Zmalloc fail; it costs
   HWRAM .text = TLSF pool, and the pool lives at its floor. */
#ifndef SAT_ZONE_RA
#define SAT_ZONE_RA 0
#endif

typedef struct memblock_s
{
    int			size;	// including the header and possibly tiny fragments
    void**		user;
    int			tag;	// PU_FREE if this is free
    int			id;	// should be ZONEID
    struct memblock_s*	next;
    struct memblock_s*	prev;
#if SAT_ZONE_RA
    void*		ra;	// Z_Malloc caller (alloc site)
#endif
} memblock_t;


typedef struct
{
    // total bytes malloced, including header
    int		size;

    // start / end cap for linked list
    memblock_t	blocklist;
    
    memblock_t*	rover;
    
} memzone_t;



memzone_t*	mainzone;

/* (SATURN diag `z_scan_steps` REMOVED 2026-08-07: it read 0 -- fewer than 1000 rover steps per
   frame on TNT MAP11 at 4 fps.  That REFUTES "a starved, fragmented zone makes every Z_Malloc walk
   and purge the whole block list", which was the leading explanation for the 32..232 ms swing in
   R_EmitWorldThingsVDP1.  The allocator is innocent; do not revive the theory without a fresh
   reading.  Re-adding it is two lines: this one and an increment in the Z_Malloc scan below. */



//
// Z_ClearZone
//
void Z_ClearZone (memzone_t* zone)
{
    memblock_t*		block;
	
    // set the entire zone to one free block
    zone->blocklist.next =
	zone->blocklist.prev =
	block = (memblock_t *)( (byte *)zone + sizeof(memzone_t) );
    
    zone->blocklist.user = (void *)zone;
    zone->blocklist.tag = PU_STATIC;
    zone->rover = block;
	
    block->prev = block->next = &zone->blocklist;
    
    // a free block.
    block->tag = PU_FREE;

    block->size = zone->size - sizeof(memzone_t);
}



//
// Z_Init
//
void Z_Init (void)
{
    memblock_t*	block;
    int		size;

    mainzone = (memzone_t *)I_ZoneBase (&size);
    mainzone->size = size;

    // set the entire zone to one free block
    mainzone->blocklist.next =
	mainzone->blocklist.prev =
	block = (memblock_t *)( (byte *)mainzone + sizeof(memzone_t) );

    mainzone->blocklist.user = (void *)mainzone;
    mainzone->blocklist.tag = PU_STATIC;
    mainzone->rover = block;
	
    block->prev = block->next = &mainzone->blocklist;

    // free block
    block->tag = PU_FREE;
    
    block->size = mainzone->size - sizeof(memzone_t);
}


//
// Z_Free
//
void Z_Free (void* ptr)
{
    memblock_t*		block;
    memblock_t*		other;

    // libc-style: Z_Free(NULL) is a no-op.  In SATURN cart mode W_CacheLumpNum returns
    // a mapped pointer WITHOUT setting lump->cache, so lump->cache stays NULL -- a path
    // that frees it (a real zone block in CD-streaming) must not crash on the NULL.
    if (ptr == NULL)
	return;

    block = (memblock_t *) ( (byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
    {
	// SATURN cart mode: a memory-mapped cart lump is not a zone block -- freeing
	// it is a no-op (as W_ReleaseLumpNum already does for mapped lumps).
	if (W_PtrIsMapped(ptr))
	    return;
	// DIAGNOSTIC: p=freed ptr, ra=caller return addr (-> build/Mimas.map),
	// id/tag = block state (id=0 tag=0 => double-free; garbage id => non-zone ptr).
	I_Error ("Z_Free bad p=%p ra=%p id=%08x tag=%d", ptr,
	         __builtin_return_address(0), (unsigned)block->id, block->tag);
    }

    if (block->tag != PU_FREE && block->user != NULL)
    {
    	// clear the user's mark
	    *block->user = 0;
    }

    // mark as free
    block->tag = PU_FREE;
    block->user = NULL;
    block->id = 0;
	
    other = block->prev;

    if (other->tag == PU_FREE)
    {
        // merge with previous free block
        other->size += block->size;
        other->next = block->next;
        other->next->prev = other;

        if (block == mainzone->rover)
            mainzone->rover = other;

        block = other;
    }
	
    other = block->next;
    if (other->tag == PU_FREE)
    {
        // merge the next free block onto the end
        block->size += other->size;
        block->next = other->next;
        block->next->prev = block;

        if (other == mainzone->rover)
            mainzone->rover = block;
    }
}



//
// Z_Malloc
// You can pass a NULL user if the tag is < PU_PURGELEVEL.
//
#define MINFRAGMENT		64


void*
Z_Malloc
( int		size,
  int		tag,
  void*		user )
{
    int		extra;
    memblock_t*	start;
    memblock_t* rover;
    memblock_t* newblock;
    memblock_t*	base;
    void *result;

    /* SATURN 2026-08-14: Z_Malloc's own rover scan is the ONE thing under R_GetColumn that has never
       been timed.  It sits beneath BOTH surviving candidates (R_EnsureLookup does 3 allocs;
       W_CacheLumpNum does one when it misses), it purges and coalesces as it walks, and `zw` does
       NOT see it -- zw only counts Z_CanAllocate/Z_LargestAllocatable.  Latched as row-20 `z` =
       the worst single Z_Malloc on the frame.  The 08-07 note "z_scan_steps read 0" retired a STEP
       counter, never a timer, so nothing here has actually been measured.
       🔴 REMOVED 2026-08-26 -- DEAD WORK.  Row-20 `z` was retired on 2026-08-17 ("it read
       0-1 on every capture ever taken, its question is answered NO") and its two display columns
       were given to `q`/`c`.  The DISPLAY went; the BRACKET did not.  Since then every single
       Z_Malloc has paid two out-of-line calls and two FRT register reads to latch a maximum that
       r_parallel.c then threw away with an explicit `(void)prof_bp_g_z`.  There is no measurement
       to weigh here: the output was discarded, so the cost was 100 %% waste at any call count. */

    size = (size + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
    
    // scan through the block list,
    // looking for the first free block
    // of sufficient size,
    // throwing out any purgable blocks along the way.

    // account for size of block header
    size += sizeof(memblock_t);

    // SATURN DIAGNOSTIC: a bogus (negative / >zone) request size means a corrupt caller
    // computation (e.g. the cart-launch "req -134218728").  Catch it AT THE SOURCE with
    // the caller's return address (-> build/Mimas.map) instead of a confusing scan halt.
    if (size <= 0 || (unsigned int)size > (unsigned int)mainzone->size)
        I_Error ("Z_Malloc bad size=%i ra=%p", size, __builtin_return_address(0));

    /* \[!] SATURN 2026-08-28 -- PASS 1: SERVE THE REQUEST WITHOUT EVICTING THE CACHE.
       THE BUG, and it is vanilla Doom's, not ours: the rover scan below frees EVERY
       PU_PURGELEVEL block it walks past while looking for a fit -- unconditionally, whatever
       is free elsewhere in the zone.  On a PC with the WAD on a fast disk that is free.  On
       Saturn every evicted lump is a ~29 ms synchronous CD read the next time a wall needs it.
       MEASURED (the two 4p Ymir captures of 2026-08-28, row 0 `ld<chunks>/<refaults>`):
       between two photos, +266 chunk commands for +171 LUMP RE-FAULTS -- about two thirds of
       all disc traffic was re-reading lumps already loaded once, and the marginal rate (64 %)
       was far above the cumulative one (23 % -> 32 %), i.e. the boot reads new lumps and PLAY
       re-reads old ones.  That is the definition of thrash, and it is what the re-fault witness
       was added to separate from churn.
       THE FIX is not a bigger zone, it is not evicting when we do not have to: walk a BOUNDED
       window from the rover looking ONLY at blocks that are already PU_FREE, take the BEST
       (smallest sufficient) one, and touch nothing else.  If the window has no fit, fall
       straight through to the vanilla scan below, unchanged -- so the worst case is the old
       behaviour plus at most Z_KEEPCACHE_HOPS pointer hops.
       WHY BEST-FIT AND NOT FIRST-FIT: first-fit would happily carve the LARGEST free run for a
       2 KB patch, and that run is exactly what the level-load composite (96 KB contiguous)
       needs -- see [[zone-contiguity-wall-loadsegs]].  Best-fit inside the window protects it.
       WHY BOUNDED: the block list runs ~790 entries (row-22 `zw`), an unbounded non-purging
       scan would be O(blocks) and would FAIL often (it cannot make room), so we would pay the
       full walk twice.  64 hops is ~500 cycles against the 29 ms it is defending.
       Z_Free coalesces with both neighbours, so a PU_FREE block's size is already the whole
       run -- no accumulation needed here.
       HOW TO SEE IT WORK: row-0 `ld<chunks>/<refaults>` is the receipt (refaults must stop
       tracking `ld`).  If they do not, row-11 `zf` -- now TRUE free, not free+purgeable --
       says which failure it is: `zf` still large means pass 1 is not firing, `zf` near zero
       means the zone really is full and there was nothing to keep. */
#define Z_KEEPCACHE_HOPS 64
    {
        memblock_t *p    = mainzone->rover;
        memblock_t *best = NULL;
        int hops = Z_KEEPCACHE_HOPS;

        if ((byte *)p < (byte *)mainzone ||
            (byte *)p >= (byte *)mainzone + mainzone->size)
            p = mainzone->blocklist.next;          /* wild rover: let pass 2's guard deal with it */

        while (hops-- > 0)
        {
            if (p->tag == PU_FREE && p->size >= size &&
                (best == NULL || p->size < best->size))
            {
                best = p;
                if (p->size <= size + MINFRAGMENT) break;   /* exact enough: stop looking */
            }
            p = p->next;
            if ((byte *)p < (byte *)mainzone ||
                (byte *)p >= (byte *)mainzone + mainzone->size)
                break;                              /* walked off: abandon pass 1, never crash in it */
        }
        if (best != NULL) { base = best; goto z_got_block; }
    }

    int z_emergency = 0;   // SATURN: allow ONE re-anchored retry before declaring OOM
    /* SATURN 2026-08-07: ROVER STEP COUNTER.  Z_Malloc's scan walks the block list purging every
       PU_CACHE block it passes, and on a straddle the z_emergency path RESCANS THE WHOLE LIST.  In a
       starved, fragmented zone that is O(blocks) -- twice -- PER ALLOCATION, and it is invisible in
       every existing counter: no disc, no bake, no draw.  It is the leading suspect for the kick's
       44..227 ms world-things emit (which does one W_CacheLumpNum per sprite) AND it is what made the
       1p composite cache 3-4x SLOWER.  Row 20 `z` = thousands of steps per frame. */
 z_retry_scan:
    // if there is a free block behind the rover,
    //  back up over them
    base = mainzone->rover;
    
    if (base->prev->tag == PU_FREE)
        base = base->prev;
	
    rover = base;
    start = base->prev;
	
    do
    {
        if (rover == start)
        {
            // SATURN: the rover anchor (mainzone->rover) can sit INSIDE the largest
            // free run, so the scan reaches its own start sentinel before spanning
            // that run -- a contiguous run that straddles the anchor is wrongly
            // reported as OOM (the lg>=size paradox).  The first full scan also
            // already purged every PU_CACHE block it walked and coalesced the frees,
            // so re-anchoring at the list head and rescanning ONCE recovers a run the
            // straddle hid.  Only a genuine exhaustion (no inter-wall free run >=
            // size anywhere) falls through to the halt below.
            if (!z_emergency)
            {
                z_emergency = 1;
                mainzone->rover = mainzone->blocklist.next;
                goto z_retry_scan;
            }
            // Scanned the whole list with no fit.  Report the zone state in the
            // halt message itself (the overlay row is overwritten by the halt):
            //   fr = total reclaimable (free + purgeable)
            //   lg = largest CONTIGUOUS run after purging  -> lg<size & fr>>size = FRAGMENTATION
            //   st = unpurgeable PU_STATIC bytes, lv = PU_LEVEL bytes (the floor)  -> fr<size = EXHAUSTION
            memblock_t* b;
            int st = 0, lv = 0;
            /* SATURN forensics (regression hunt): while summing residents, capture the 8
               biggest so a wall that GREW/APPEARED stands out next to the halt (printed to
               the boot console).  ra on the halt line = the caller that requested `size`
               (resolve against build/Mimas.map to name the victim alloc).  Tags:
               1=STATIC 2=SOUND 3=MUSIC 50=LEVEL 51=LEVSPEC. */
            int t_sz[8] = {0}, t_tag[8] = {0}, t_off[8] = {0};
#if SAT_ZONE_RA
            void* t_ra[8] = {0};
#endif
            for (b = mainzone->blocklist.next ; b != &mainzone->blocklist ; b = b->next)
            {
                if (b->tag == PU_FREE || b->tag >= PU_PURGELEVEL)   continue;
                else if (b->tag == PU_LEVEL || b->tag == PU_LEVSPEC) lv += b->size;
                else                                                 st += b->size;
                if (b->size > t_sz[7])   /* insertion into the descending top-8 */
                {
                    int j = 7;
                    while (j > 0 && t_sz[j-1] < b->size)
                    { t_sz[j]=t_sz[j-1]; t_tag[j]=t_tag[j-1]; t_off[j]=t_off[j-1];
#if SAT_ZONE_RA
                      t_ra[j]=t_ra[j-1];
#endif
                      j--; }
                    t_sz[j]  = b->size;
                    t_tag[j] = b->tag;
                    t_off[j] = (int)((char*)b - (char*)mainzone);
#if SAT_ZONE_RA
                    t_ra[j]  = b->ra;
#endif
                }
            }
            printf("ZONE top8 resident (KB t=tag @KB ra=allocsite):\n");
            for (int i = 0 ; i < 8 && t_sz[i] ; i++)
#if SAT_ZONE_RA
                printf(" %2dK t%d @%dK ra=%p\n", t_sz[i]>>10, t_tag[i], t_off[i]>>10, t_ra[i]);
#else
                printf(" %2dK t%d @%dK\n", t_sz[i]>>10, t_tag[i], t_off[i]>>10);
#endif
            I_Error ("Zmalloc fail %i t%d ra=%p (fr%dK lg%dK st%dK lv%dK)",
                     size, tag, __builtin_return_address(0),
                     Z_FreeMemory()>>10, Z_LargestAllocatable()>>10,
                     st>>10, lv>>10);
        }

        // SATURN: addr-0 is a readable cached mirror on SH-2, so a NULL / out-of-zone
        // scan pointer is NOT trapped -- the loop would read ROM garbage as a purgeable
        // tag and Z_Free(rover+0x18) (the "Z_Free bad p=0x18" crash).  Catch a wild rover
        // BEFORE dereferencing rover->tag: re-anchor at the list head and rescan ONCE
        // (recovers a transient walk-off), else halt cleanly instead of wild-freeing.
        if ((byte *)rover < (byte *)mainzone ||
            (byte *)rover >= (byte *)mainzone + mainzone->size)
        {
            if (!z_emergency)
            {
                z_emergency = 1;
                mainzone->rover = mainzone->blocklist.next;
                goto z_retry_scan;
            }
            I_Error ("Z_Malloc: corrupt zone scan rover=%p (req %i)", rover, size);
        }

        if (rover->tag != PU_FREE)
        {
            if (rover->tag < PU_PURGELEVEL)
            {
                // hit a block that can't be purged,
                // so move base past it
                base = rover = rover->next;
            }
            else
            {
                // free the rover block (adding the size to base)

                // the rover can be the base block
                base = base->prev;
                Z_Free ((byte *)rover+sizeof(memblock_t));
                base = base->next;
                rover = base->next;
            }
        }
        else
        {
            rover = rover->next;
        }

    } while (base->tag != PU_FREE || base->size < size);

 z_got_block:                     /* SATURN 2026-08-28: pass 1 lands here, having purged nothing */
    // found a block big enough
    extra = base->size - size;
    
    if (extra >  MINFRAGMENT)
    {
        // there will be a free fragment after the allocated block
        newblock = (memblock_t *) ((byte *)base + size );
        newblock->size = extra;
	
        newblock->tag = PU_FREE;
        newblock->user = NULL;	
        newblock->prev = base;
        newblock->next = base->next;
        newblock->next->prev = newblock;

        base->next = newblock;
        base->size = size;
    }
	
	if (user == NULL && tag >= PU_PURGELEVEL)
	    I_Error ("Z_Malloc: an owner is required for purgable blocks");

    base->user = user;
    base->tag = tag;
#if SAT_ZONE_RA
    base->ra = __builtin_return_address(0);   // who called Z_Malloc (W_CacheLumpNum / R_GenerateComposite / P_Setup...)
#endif

    result  = (void *) ((byte *)base + sizeof(memblock_t));

    if (base->user)
    {
        *base->user = result;
    }

    // next allocation will start looking here
    mainzone->rover = base->next;	
	
    base->id = ZONEID;

    return result;
}



//
// Z_FreeTags
//
void
Z_FreeTags
( int		lowtag,
  int		hightag )
{
    memblock_t*	block;
    memblock_t*	next;
	
    for (block = mainzone->blocklist.next ;
	 block != &mainzone->blocklist ;
	 block = next)
    {
	// get link before freeing
	next = block->next;

	// free block?
	if (block->tag == PU_FREE)
	    continue;
	
	if (block->tag >= lowtag && block->tag <= hightag)
	    Z_Free ( (byte *)block+sizeof(memblock_t));
    }
}



//
// Z_DumpHeap
// Note: TFileDumpHeap( stdout ) ?
//
void
Z_DumpHeap
( int		lowtag,
  int		hightag )
{
    memblock_t*	block;
	
    printf ("zone size: %i  location: %p\n",
	    mainzone->size,mainzone);
    
    printf ("tag range: %i to %i\n",
	    lowtag, hightag);
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	if (block->tag >= lowtag && block->tag <= hightag)
	    printf ("block:%p    size:%7i    user:%p    tag:%3i\n",
		    block, block->size, block->user, block->tag);
		
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    printf ("ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    printf ("ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    printf ("ERROR: two consecutive free blocks\n");
    }
}


//
// Z_FileDumpHeap
//
void Z_FileDumpHeap (FILE* f)
{
    memblock_t*	block;
	
    fprintf (f,"zone size: %i  location: %p\n",mainzone->size,mainzone);
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	fprintf (f,"block:%p    size:%7i    user:%p    tag:%3i\n",
		 block, block->size, block->user, block->tag);
		
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    fprintf (f,"ERROR: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    fprintf (f,"ERROR: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    fprintf (f,"ERROR: two consecutive free blocks\n");
    }
}



//
// Z_CheckHeap
//
void Z_CheckHeap (void)
{
    memblock_t*	block;
	
    for (block = mainzone->blocklist.next ; ; block = block->next)
    {
	if (block->next == &mainzone->blocklist)
	{
	    // all blocks have been hit
	    break;
	}
	
	if ( (byte *)block + block->size != (byte *)block->next)
	    I_Error ("Z_CheckHeap: block size does not touch the next block\n");

	if ( block->next->prev != block)
	    I_Error ("Z_CheckHeap: next block doesn't have proper back link\n");

	if (block->tag == PU_FREE && block->next->tag == PU_FREE)
	    I_Error ("Z_CheckHeap: two consecutive free blocks\n");
    }
}




//
// Z_ChangeTag
//
void Z_ChangeTag2(void *ptr, int tag, char *file, int line)
{
    memblock_t*	block;
	
    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
        I_Error("%s:%i: Z_ChangeTag: block without a ZONEID!",
                file, line);

    if (tag >= PU_PURGELEVEL && block->user == NULL)
        I_Error("%s:%i: Z_ChangeTag: an owner is required "
                "for purgable blocks", file, line);

    block->tag = tag;
}

void Z_ChangeUser(void *ptr, void **user)
{
    memblock_t*	block;

    block = (memblock_t *) ((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
    {
        I_Error("Z_ChangeUser: Tried to change user for invalid block!");
    }

    block->user = user;
    *user = ptr;
}



//
// Z_FreeMemory
//
int Z_FreeMemory (void)
{
    memblock_t*		block;
    int			free;
	
    free = 0;
    
    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist;
         block = block->next)
    {
        if (block->tag == PU_FREE || block->tag >= PU_PURGELEVEL)
            free += block->size;
    }

    return free;
}

/* SATURN 2026-08-28 -- FREE MEANS FREE.  Z_FreeMemory above counts PU_PURGELEVEL as free, so
   overlay row-11 `zf` and `lg` were BOTH reporting the zone on the assumption that the whole lump
   cache is expendable -- two views of one number, and neither could say how much of the zone is
   actually unused.  That mattered the moment the re-fault witness proved the cache is being
   evicted: "441 KB free" was not evidence that the zone has room, it was 441 KB of free PLUS
   cache.  This counts only PU_FREE, so `zf` vs `lg` now reads as TRULY FREE vs OBTAINABLE BY
   PURGING, and the gap between them IS the resident cache -- which is also the receipt for the
   non-purging pass in Z_Malloc: if that pass fires, the cache grows and `zf` falls. */
int Z_TrueFree (void)
{
    memblock_t *block;
    int free = 0;
    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist;
         block = block->next)
        if (block->tag == PU_FREE) free += block->size;
    return free;
}

unsigned int Z_ZoneSize(void)
{
    return mainzone->size;
}


//
// Z_LargestAllocatable
// Largest CONTIGUOUS run the allocator could hand out right now if it purged
// everything purgeable -- i.e. treats PU_FREE and tag>=PU_PURGELEVEL as
// available, and an unpurgeable (PU_STATIC/PU_LEVEL) block as a wall that breaks
// the run.  This is the real "could Z_Malloc(N) succeed" number: compare it to
// the failing size to tell FRAGMENTATION (this is < N but Z_FreeMemory >> N)
// from true EXHAUSTION (Z_FreeMemory itself < N).  O(blocks); call sparingly
// (overlay rate), not on the hot path.
//
/* SATURN 2026-08-07: rewind the allocation rover to the bottom of the zone.
   Z_Malloc resumes scanning from wherever the LAST allocation left the rover, so a PU_STATIC slab
   carved after a level's geometry lands high -- a fresh CUT through the free space instead of
   packing against the boot statics (lumpinfo, the visplane pool, the .DRP table) that already sit
   at the bottom.  That is what the halt dumps kept showing: `fr190K lg32K` -- 190 KB free, never
   34 KB in one run, because six or more unpurgeable blocks are spread across the zone.
   Call this immediately before carving a long-lived slab: the scan then starts at block 0 and the
   slab drops into the LOWEST hole that fits, next to its own kind.  It costs one extra walk of the
   block list, once per level -- nothing next to a ~57 ms CD command.
   (Same one-line mechanism the z_emergency retry already uses, promoted to a named intent.) */
void Z_RoverToStart (void)
{
    if (mainzone) mainzone->rover = mainzone->blocklist.next;
}

/* SATURN 2026-08-12 (macro plan P1): the zone's BLOCK COUNT, a free by-product of the walk below.
   It is the missing factor in the only surviving explanation of the 214 ms R_GetColumn hole:
   R_GenerateComposite calls Z_LargestAllocatable TWICE per build on the 1p path (r_data.c:294 and
   :321, because R_TexCacheAlloc returns NULL with the pool dead in 1p), this function is O(blocks)
   with two cache-missing header reads per block, and estimates of the block count ranged 600..1500 --
   i.e. 0.4 ms vs 1.6 ms PER WALK, which decides whether the allocator is the subject or a detail.
   Measure it instead of arguing about it.  Overlay row 22 `zb`. */
/* z_block_count REMOVED 2026-08-26 -- row-22 `zb` was cut and left the three stores behind.
   z_walk_blocks, on the adjacent lines, is the one that still prints. */
/* SATURN 2026-08-12: how many times the walk ran this overlay window (overlay row 22 `zc`).  The
   product zb x zc is the real cost, and it is the leading explanation of the R_GetColumn hole:
   r_data.c:615 tests this function PER COLUMN on the single-patch path, and at zb~790 blocks one
   walk is ~23 700 cycles = 0.83 ms.  392 calls x 54% = 211 walks = 175 ms, which is exactly the
   measured `g`.  Reset by the platform after printing. */
/* (z_walk_calls -- the 08-12 per-WINDOW call count -- REMOVED 08-14 after two captures: it summed
   over the ~1 s overlay window while row-20 `g` is latched to ONE frame, so it could never be divided
   into g.  Same clock error as `cb`, made twice.  Superseded by z_walk_blocks below.) */
/* SATURN 2026-08-14: BLOCKS walked this FRAME -- the number that needs no division.  z_walk_calls is
   a 1 s window and z_block_count is clobbered by the overlay's own Z_LargestAllocatable() for row-11
   `lg`, so neither could be put against row-20 `g`, which is latched to ONE frame.  This one is
   latched on the SAME frame as g (core/r_parallel.c, the PK-Bp block) and converts directly:
   ms = zw x ~30 cycles / 28600.  Reset in RP_BeginFrame. */
int	z_walk_blocks = 0;

/* SATURN 2026-08-12 -- THE EARLY-EXIT TWIN.  Six of this function's nine call sites do not want the
   largest run at all, they ask a THRESHOLD question (`Z_LargestAllocatable() < need`).  Answering it
   never required walking to the end: stop at the first purgeable run that already reaches `size`.
   Semantically identical to `Z_LargestAllocatable() >= size` -- same coalescing rule, same tags --
   but O(blocks-until-found) instead of O(all blocks).  It cannot be slower, and on the hot site it
   is the difference between 0.83 ms and a few microseconds whenever a big run sits early in the
   list.  The header's own warning ("O(blocks); call sparingly, at overlay rate") was written for a
   reason and then ignored by a per-column caller. */
int Z_CanAllocate (int size)
{
    memblock_t*	block;
    int		run = 0;
    int		nblk = 0;

    if (size <= 0) return 1;
    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist ;
         block = block->next)
    {
	nblk++;
        if (block->tag == PU_FREE || block->tag >= PU_PURGELEVEL)
        {
            run += block->size;
            if (run >= size)
            {
                z_walk_blocks += nblk;
                return 1;
            }
        }
        else
            run = 0;   // unpurgeable block breaks the contiguous run
    }
    z_walk_blocks += nblk;
    return 0;
}

int Z_LargestAllocatable (void)
{
    memblock_t*	block;
    int		run = 0;
    int		largest = 0;
    int		nblk = 0;


    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist ;
         block = block->next)
    {
	nblk++;
        if (block->tag == PU_FREE || block->tag >= PU_PURGELEVEL)
        {
            run += block->size;
            if (run > largest)
                largest = run;
        }
        else
            run = 0;   // unpurgeable block breaks the contiguous run
    }

    z_walk_blocks += nblk;
    return largest;
}


//
// SATURN multi-zone extensions
// ----------------------------
// A second, self-contained zone (same memblock_t/memzone_t layout) laid over a
// caller-owned buffer.  Used by the bounded texture cache (core/r_cache.c) to
// cap and recency-evict the streaming graphics working set.  These never touch
// mainzone's allocator, so the normal Z_Malloc path is unchanged.
//

void *Z_MainZone (void)
{
    return (void *)mainzone;
}

//
// Z_InitZone -- lay one empty free block over base[0..size).  Returns the handle.
//
void *Z_InitZone (void *base, int size)
{
    memzone_t  *zone = (memzone_t *)base;
    memblock_t *block;

    zone->size = size;
    zone->blocklist.next =
    zone->blocklist.prev =
        block = (memblock_t *)((byte *)zone + sizeof(memzone_t));
    zone->blocklist.user = (void *)zone;
    zone->blocklist.tag  = PU_STATIC;
    zone->rover = block;

    block->prev = block->next = &zone->blocklist;
    block->user = NULL;
    block->id   = 0;
    block->tag  = PU_FREE;
    block->size = size - sizeof(memzone_t);

    return (void *)zone;
}

//
// Z_Malloc2 -- first-fit from an explicit zone, NO purging, NULL on OOM.
// (The cache layer manages eviction itself via Z_Free2, so this never purges;
//  returning NULL lets the caller evict-and-retry instead of I_Error'ing.)
//
void *Z_Malloc2 (void *zoneptr, int size, int tag)
{
    memzone_t  *zone = (memzone_t *)zoneptr;
    int         extra;
    memblock_t *start, *rover, *base, *newblock;

    size = (size + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
    size += sizeof(memblock_t);

    base = zone->rover;
    if (base->prev->tag == PU_FREE)
        base = base->prev;

    rover = base;
    start = base->prev;

    do
    {
        if (rover == start)
            return NULL;                  // wrapped the whole zone: no room

        if (rover->tag != PU_FREE)
            base = rover = rover->next;   // skip allocated block (no purge here)
        else
            rover = rover->next;
    } while (base->tag != PU_FREE || base->size < size);

    extra = base->size - size;
    if (extra > MINFRAGMENT)
    {
        newblock = (memblock_t *)((byte *)base + size);
        newblock->size = extra;
        newblock->tag  = PU_FREE;
        newblock->user = NULL;
        newblock->prev = base;
        newblock->next = base->next;
        newblock->next->prev = newblock;
        base->next = newblock;
        base->size = size;
    }

    base->user = NULL;
    base->tag  = tag;
    base->id   = ZONEID;
    zone->rover = base->next;

    return (void *)((byte *)base + sizeof(memblock_t));
}

//
// Z_Free2 -- free a block in an explicit zone (parameterized Z_Free).
//
void Z_Free2 (void *zoneptr, void *ptr)
{
    memzone_t  *zone = (memzone_t *)zoneptr;
    memblock_t *block, *other;

    if (ptr == NULL)            // libc-style no-op (see Z_Free)
        return;

    block = (memblock_t *)((byte *)ptr - sizeof(memblock_t));

    if (block->id != ZONEID)
    {
        if (W_PtrIsMapped(ptr))     // SATURN: mapped cart lump, not a zone block -> no-op
            return;
        I_Error ("Z_Free2 bad p=%p ra=%p id=%08x tag=%d", ptr,
                 __builtin_return_address(0), (unsigned)block->id, block->tag);
    }

    if (block->tag != PU_FREE && block->user != NULL)
        *block->user = 0;

    block->tag  = PU_FREE;
    block->user = NULL;
    block->id   = 0;

    other = block->prev;
    if (other->tag == PU_FREE)
    {
        other->size += block->size;
        other->next  = block->next;
        other->next->prev = other;
        if (block == zone->rover)
            zone->rover = other;
        block = other;
    }

    other = block->next;
    if (other->tag == PU_FREE)
    {
        block->size += other->size;
        block->next  = other->next;
        block->next->prev = block;
        if (other == zone->rover)
            zone->rover = block;
    }
}

//
// Z_LargestFreeBlock -- size (incl header) of the biggest free block in a zone.
//
int Z_LargestFreeBlock (void *zoneptr)
{
    memzone_t  *zone = (memzone_t *)zoneptr;
    memblock_t *block;
    int         largest = 0;

    for (block = zone->blocklist.next ;
         block != &zone->blocklist ;
         block = block->next)
    {
        if (block->tag == PU_FREE && block->size > largest)
            largest = block->size;
    }
    return largest;
}

//
// Z_ForEachBlock -- invoke cb(payload, userp) for every ALLOCATED block.
// `next` is captured before the callback so cb may Z_Free2 the CURRENT block.
// Safe only because cb frees at most the current block: if that free forward-
// coalesces the captured `next` away, `next`'s header bytes are left intact and
// its tag stays PU_FREE (so the cb is skipped) with a still-valid forward link.
// cb must NOT allocate from this zone during the walk (that could reuse a header).
//
void Z_ForEachBlock (void *zoneptr, Z_BlockIter cb, void *userp)
{
    memzone_t  *zone = (memzone_t *)zoneptr;
    memblock_t *block, *next;

    for (block = zone->blocklist.next ;
         block != &zone->blocklist ;
         block = next)
    {
        next = block->next;
        if (block->tag != PU_FREE)
            cb ((void *)((byte *)block + sizeof(memblock_t)), userp);
    }
}

