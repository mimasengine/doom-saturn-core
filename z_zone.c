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

typedef struct memblock_s
{
    int			size;	// including the header and possibly tiny fragments
    void**		user;
    int			tag;	// PU_FREE if this is free
    int			id;	// should be ZONEID
    struct memblock_s*	next;
    struct memblock_s*	prev;
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

    int z_emergency = 0;   // SATURN: allow ONE re-anchored retry before declaring OOM
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
            for (b = mainzone->blocklist.next ; b != &mainzone->blocklist ; b = b->next)
            {
                if (b->tag == PU_FREE || b->tag >= PU_PURGELEVEL)   continue;
                else if (b->tag == PU_LEVEL || b->tag == PU_LEVSPEC) lv += b->size;
                else                                                 st += b->size;
                if (b->size > t_sz[7])   /* insertion into the descending top-8 */
                {
                    int j = 7;
                    while (j > 0 && t_sz[j-1] < b->size)
                    { t_sz[j]=t_sz[j-1]; t_tag[j]=t_tag[j-1]; t_off[j]=t_off[j-1]; j--; }
                    t_sz[j]  = b->size;
                    t_tag[j] = b->tag;
                    t_off[j] = (int)((char*)b - (char*)mainzone);
                }
            }
            printf("ZONE top8 resident (KB t=tag @KB from base):\n");
            for (int i = 0 ; i < 8 && t_sz[i] ; i++)
                printf(" %2dK t%d @%dK\n", t_sz[i]>>10, t_tag[i], t_off[i]>>10);
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
int Z_LargestAllocatable (void)
{
    memblock_t*	block;
    int		run = 0;
    int		largest = 0;

    for (block = mainzone->blocklist.next ;
         block != &mainzone->blocklist ;
         block = block->next)
    {
        if (block->tag == PU_FREE || block->tag >= PU_PURGELEVEL)
        {
            run += block->size;
            if (run > largest)
                largest = run;
        }
        else
            run = 0;   // unpurgeable block breaks the contiguous run
    }

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

