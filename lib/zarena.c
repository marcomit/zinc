// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zarena.h"
#include <stdint.h>

#define ARENA_ALIGNMENT ((usize)8)
#define ARENA_ALIGN(size) (((size) + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1))

#define ARENA_MIN_BUCKET KiB(16)
#define ARENA_MAX_BUCKET MiB(2)

/* Allocate a bucket with an exact buffer size. Callers compute the size:
 * geometric growth for default buckets, exact fit for oversized allocations. */
static ArenaBucket *createArenaBucket(usize size) {
    ArenaBucket *self = malloc(sizeof(ArenaBucket) + size);
    self->len = 0;
    self->size = size;
    self->next = NULL;
    return self;
}

arena_t *createArena() {
    arena_t *self = malloc(sizeof(arena_t));
    self->head = createArenaBucket(ARENA_MIN_BUCKET);
    self->tail = self->head;
    self->scopes = NULL;
    return self;
}

static void _arenaSize(ArenaBucket *bucket, usize *curr) {
    while (bucket) {
        *curr += bucket->size;
        bucket = bucket->next;
    }
}

static void _arenaLength(ArenaBucket *bucket, usize *curr) {
    while (bucket) {
        *curr += bucket->len;
        bucket = bucket->next;
    }
}

usize arenaSize(arena_t *arena) {
    usize curr = 0;
    _arenaSize(arena->head, &curr);
    return curr;
}

usize arenaLength(arena_t *arena) {
    usize curr = 0;
    _arenaLength(arena->head, &curr);
    return curr;
}

static void freeArenaBucket(ArenaBucket *arena, bool recursive) {
    if (!arena) return;
    if (recursive) freeArenaBucket(arena->next, recursive);
    free(arena);
}

void *arenaAlloc(arena_t *arena, usize size) {
    size = ARENA_ALIGN(size);

    if (arena->tail->len + size <= arena->tail->size) {
        void *ptr = (u8 *)(arena->tail + 1) + arena->tail->len;
        arena->tail->len += size;
        return ptr;
    }

    usize grown = arena->tail->size << 1;
    if (grown > ARENA_MAX_BUCKET) grown = ARENA_MAX_BUCKET;
    usize actual = size > grown ? size : grown;

    ArenaBucket *next = createArenaBucket(actual);
    arena->tail->next = next;
    arena->tail = next;
    arena->tail->len = size;
    return arena->tail + 1;
}

void arenaFree(arena_t *arena) {
    freeArenaBucket(arena->head, true);
    arena->head = NULL;
    arena->tail = NULL;
    free(arena);
}

inline ArenaScope arenaScope(arena_t *arena) {
    return (ArenaScope) {
        .bucket = arena->tail,
        .pos    = arena->tail->len
    };
}

void arenaEndScope(arena_t *arena, ArenaScope scope) {

    ArenaBucket *curr = arena->head;
    while (curr && curr != scope.bucket) curr = curr->next;

    if (!curr) return;

    curr->len = scope.pos;
    curr->next = NULL;
    arena->tail = curr;
    freeArenaBucket(scope.bucket->next, true);
    u8 *buffer = (u8 *)(arena->tail + 1);
    memset(buffer + curr->len, 0, curr->size - curr->len);
}

void *arena_alloc(void *ctx, usize size) {
    return arenaAlloc((arena_t *)ctx, size);
}

void *arena_realloc(void *ctx, void *old, usize size) {
    (void)old;
    return arenaAlloc((arena_t *)ctx, size);
}

void arena_open(void *ctx) {
    (void)ctx;
}

void arena_close(void *ctx) {
    arenaFree((arena_t *)ctx);
}

Allocator arena2Allocator(arena_t *arena) {
    return (Allocator) {
        .alloc      = arena_alloc,
        .realloc    = arena_realloc,
        .open       = arena_open,
        .close      = arena_close,
        .ctx        = arena
    };
}
