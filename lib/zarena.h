// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#ifndef Z_ARENA_H
#define Z_ARENA_H

#include "base.h"

typedef struct ArenaBucket {
    usize len;
    usize size;
    struct ArenaBucket *next;
} ArenaBucket;

typedef struct ArenaScope {
    ArenaBucket *bucket;
    usize pos;
} ArenaScope;

typedef struct arena_t {
    ArenaBucket *head;
    ArenaBucket *tail;

    ArenaScope **scopes;
} arena_t;

arena_t *createArena();
void    *arenaAlloc(arena_t *, usize);
void    arenaFree(arena_t *);
void    arenaScope(arena_t *);
void    arenaEndScope(arena_t *);
usize   arenaSize(arena_t *);
usize   arenaLength(arena_t *);

#endif //Z_ARENA_H
