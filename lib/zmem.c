// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zmem.h"
#include "zarena.h"

Allocator *heapAllocator           = NULL;
Allocator *arenaAllocator          = NULL;
_Thread_local Allocator *vecDefaultAllocator     = NULL;
_Thread_local Allocator *hashsetDefaultAllocator = NULL;

inline void *aalloc(Allocator *allocator, usize size) {
    return allocator->alloc(allocator->ctx, size);
}

inline void *arealloc(Allocator *allocator, void *old, usize size) {
    return allocator->realloc(allocator->ctx, old, size);
}

inline void afree(Allocator *allocator, void *ptr) {
    if (allocator->free) allocator->free(allocator->ctx, ptr);
}

inline void aopen(Allocator *allocator) {
    if (allocator->open) allocator->open(allocator->ctx);
}

inline void aclose(Allocator *allocator) {
    if (allocator->close) allocator->close(allocator->ctx);
}

/* Close an allocator made by getHeapAllocator / getArenaAllocator and free the
 * Allocator itself. */
void adestroy(Allocator *allocator) {
    if (!allocator) return;
    aclose(allocator);
    free(allocator);
}

/* Make `allocator` the calling thread's default for new containers. Returns the
 * previous default so callers can restore it. */
Allocator *useAllocator(Allocator *allocator) {
    Allocator *prev         = vecDefaultAllocator;
    vecDefaultAllocator     = allocator;
    hashsetDefaultAllocator = allocator;
    return prev;
}

void *heap_alloc(void *allocator, usize size) {
    (void)allocator;
    return malloc(size);
}

void *heap_realloc(void *allocator, void *ptr, usize size) {
    (void)allocator;
    return realloc(ptr, size);
}

void heap_free(void *allocator, void *ptr) {
    (void)allocator;
    free(ptr);
}

Allocator *getHeapAllocator() {
    Allocator *self = (Allocator *)malloc(sizeof(Allocator));
    *self = (Allocator) {
        .alloc      = heap_alloc,
        .realloc    = heap_realloc,
        .free       = heap_free,
        .open       = NULL,
        .close      = NULL,
        .ctx        = NULL
    };
    return self;
}

Allocator *getArenaAllocator() {
    arena_t *arena = createArena();
    if (!arena) return NULL;

    Allocator self = arena2Allocator(arena);
    Allocator *a    = (Allocator *)malloc(sizeof(Allocator));
    memcpy(a, &self, sizeof(Allocator));
    return a;
}

/**
 * TODO: implement a different kind of arena where every allocation
 * starts from the beginning of the bucket.
 * if the allocator creates another bucket the arena should not allocate new buckets.
 * Instead ose those buckets as a pre allocated buckets.
 * The memory will not zeroed.
 * */
Allocator *getTempAllocator() {
    return getArenaAllocator();
}

void init_allocators() {
    if (!heapAllocator)
        heapAllocator = getHeapAllocator();
    if (!arenaAllocator)
        arenaAllocator = getArenaAllocator();
    if (!vecDefaultAllocator)
        vecDefaultAllocator = heapAllocator;
    if (!hashsetDefaultAllocator)
        hashsetDefaultAllocator = heapAllocator;
}

inline char *zstrndup(Allocator *allocator, char *str, usize len) {
    char *copy = aalloc(allocator, len + 1);
    memcpy(copy, str, len);
    copy[len] = '\0';
    return copy;
}

char *zstrdup(Allocator *allocator, char *str) {
    return zstrndup(allocator, str, strlen(str));
}
