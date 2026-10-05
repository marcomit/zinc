// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#ifndef Z_ARENA_H
#define Z_ARENA_H

#include "base.h"
#include "zmem.h"

#include <stdatomic.h>

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

    /* Containers allocate lazily from whichever thread first touches them, so an
     * arena can be hit by several threads at once (sem workers, codegen threads). */
    atomic_flag lock;
} arena_t;

arena_t     *createArena();
void        *arenaAlloc(arena_t *, usize);
void        arenaFree(arena_t *);
ArenaScope  arenaScope(arena_t *);
void        arenaEndScope(arena_t *, ArenaScope);
usize       arenaSize(arena_t *);
usize       arenaLength(arena_t *);

Allocator   arena2Allocator(arena_t *);

#endif //Z_ARENA_H
