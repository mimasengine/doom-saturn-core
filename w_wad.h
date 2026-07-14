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
//	WAD I/O functions.
//


#ifndef __W_WAD__
#define __W_WAD__

#include <stdio.h>

#include "doomtype.h"
#include "d_mode.h"

#include "w_file.h"


//
// TYPES
//

//
// WADFILE I/O related stuff.
//

typedef struct lumpinfo_s lumpinfo_t;

struct lumpinfo_s
{
    char	name[8];
    // SATURN R4.3c: the per-lump `wad_file` pointer was dropped (-4B/lump, ~11K on a
    // ~2900-lump IWAD -> the 944K zone) -- Mimas loads exactly ONE WAD file (D_AddFile
    // once; the -file PWAD path is unreachable with no argv and FEATURE_WAD_MERGE off),
    // and W_PtrIsMapped already treated lumpinfo[0].wad_file as THE file.  The single
    // file now lives in w_wadfile (w_wad.c), guarded loud on a 2nd W_AddFile.
    // DoomJo: also single-file on console -> benign; a multi-file build would trip the
    // guard and must restore the per-lump field.
    int		position;
    int		size;
    void       *cache;

    // Used for hash table lookups

    lumpinfo_t *next;
};


extern lumpinfo_t *lumpinfo;
extern unsigned int numlumps;

wad_file_t *W_AddFile (char *filename);
wad_file_t *W_MainWadFile(void);   // SATURN R4.3c: the single WAD file (lumpinfo.wad_file dropped)

int	W_CheckNumForName (char* name);
int	W_GetNumForName (char* name);

int	W_LumpLength (unsigned int lump);
void    W_ReadLump (unsigned int lump, void *dest);

void*	W_CacheLumpNum (int lump, int tag);
void*	W_CacheLumpName (char* name, int tag);

void    W_GenerateHashTable(void);

extern unsigned int W_LumpNameHash(const char *s);

void    W_ReleaseLumpNum(int lump);
void    W_ReleaseLumpName(char *name);
boolean W_PtrIsMapped(const void *p);   // SATURN: p is a memory-mapped (cart) lump?

void W_CheckCorrectIWAD(GameMission_t mission);

#endif
