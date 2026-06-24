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
//      Zone Memory Allocation, perhaps NeXT ObjectiveC inspired.
//	Remark: this was the only stuff that, according
//	 to John Carmack, might have been useful for
//	 Quake.
//



#ifndef __Z_ZONE__
#define __Z_ZONE__

#include <stdio.h>

//
// ZONE MEMORY
// PU - purge tags.

enum
{
    PU_STATIC = 1,                  // static entire execution time
    PU_SOUND,                       // static while playing
    PU_MUSIC,                       // static while playing
    PU_FREE,                        // a free block
    PU_LEVEL,                       // static until level exited
    PU_LEVSPEC,                     // a special thinker in a level
    
    // Tags >= PU_PURGELEVEL are purgable whenever needed.

    PU_PURGELEVEL,
    PU_CACHE,

    // Total number of different tag types

    PU_NUM_TAGS
};
        

void	Z_Init (void);
void*	Z_Malloc (int size, int tag, void *ptr);
void    Z_Free (void *ptr);
void    Z_FreeTags (int lowtag, int hightag);
void    Z_DumpHeap (int lowtag, int hightag);
void    Z_FileDumpHeap (FILE *f);
void    Z_CheckHeap (void);
void    Z_ChangeTag2 (void *ptr, int tag, char *file, int line);
void    Z_ChangeUser(void *ptr, void **user);
int     Z_FreeMemory (void);
int     Z_LargestAllocatable (void);   // largest contiguous run after purging (frag vs exhaustion)
unsigned int Z_ZoneSize(void);

//
// SATURN multi-zone extensions (bounded texture cache, core/r_cache.c).
// A "zone" handle is opaque (void*); these operate on a self-contained sub-zone
// laid over a caller-owned buffer, independent of the global mainzone, and add
// no cost to the normal Z_Malloc path.  Pure C (compiles under DoomJo's GCC 9.3).
//
typedef void (*Z_BlockIter)(void *ptr, void *userp);

void *Z_MainZone (void);                          // opaque handle to the global mainzone
void *Z_InitZone (void *base, int size);          // lay an empty zone over base; returns handle
void *Z_Malloc2  (void *zone, int size, int tag); // first-fit, no purge; NULL on exhaustion
void  Z_Free2    (void *zone, void *ptr);
int   Z_LargestFreeBlock (void *zone);            // bytes (incl header) of the biggest free block
void  Z_ForEachBlock (void *zone, Z_BlockIter cb, void *userp); // cb over every allocated block

//
// This is used to get the local FILE:LINE info from CPP
// prior to really call the function in question.
//
#define Z_ChangeTag(p,t)                                       \
    Z_ChangeTag2((p), (t), __FILE__, __LINE__)


#endif
