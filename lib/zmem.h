// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#ifndef ZMEM_H
#define ZMEM_H

#include "base.h"

#include <stddef.h>
#include <stdint.h>

#define KiB(n) ((n) << 10)
#define MiB(n) ((n) << 20)
#define GiB(n) ((n) << 30)

#define zalloc(a, T) ((T *)(a)->alloc((a)->ctx, sizeof(T)))
#define znalloc(a, T, n) ((T *)(a)->alloc((a)->ctx, (n) * sizeof(T)))


typedef struct Allocator {
	void *(*alloc)(void *, usize);
	void *(*realloc)(void *, void *, usize);
	void (*free)(void *, void *);
	void (*open)(void *);
	void (*close)(void *);

	void *ctx;
} Allocator;

extern Allocator *heapAllocator;
extern Allocator *arenaAllocator;

void *aalloc(Allocator *, usize);
void *arealloc(Allocator *, void *, usize);
void afree(Allocator *, void *);
void aopen(Allocator *);
void aclose(Allocator *);

Allocator *getHeapAllocator();
Allocator *getArenaAllocator();

void init_allocators();

#endif
