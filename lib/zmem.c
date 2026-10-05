// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zmem.h"
#include "zarena.h"
#include "zvec.h"
#include "zhset.h"

Allocator *heapAllocator           = NULL;
Allocator *arenaAllocator          = NULL;
Allocator *vecDefaultAllocator     = NULL;
Allocator *hashsetDefaultAllocator = NULL;

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
