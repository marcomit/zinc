#include "zmem.h"

#include <stdlib.h>

#define ARENA_ALIGNMENT 8
#define ARENA_ALIGN(size) (((size) + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1))

#define ARENA_PAGE_SIZE MiB(1)

typedef struct ArenaBucket {
	size_t len;
	size_t size;
	uint8_t *buffer;
	struct ArenaBucket *next;
} ArenaBucket;

typedef struct arena_t {
	ArenaBucket *head;
	ArenaBucket *tail;
} arena_t;

static ArenaBucket *createArenaBucket(size_t requested) {
	size_t actual = requested > ARENA_PAGE_SIZE ? requested : ARENA_PAGE_SIZE;

	ArenaBucket *self = malloc(sizeof(ArenaBucket) + actual);
	self->len = 0;
	self->size = ARENA_PAGE_SIZE;
	self->buffer = (uint8_t *)(self + 1);
	self->next = NULL;
	return self;
}

static arena_t *createArena() {
	arena_t *self = malloc(sizeof(arena_t));
	self->head = createArenaBucket(0);
	self->tail = self->head;
	return self;
}

static void freeArenaBucket(ArenaBucket *arena) {
	while (arena) {
		ArenaBucket *tmp = arena->next;
		free(arena);
		arena = tmp;
	}
}

static void *arenaAlloc(arena_t *arena, size_t size) {
	size = ARENA_ALIGN(size);

	if (arena->tail->len + size <= arena->tail->size) {
		void *ptr = arena->tail->buffer + arena->tail->len;
		arena->tail->len += size;
		return ptr;
	}

	ArenaBucket *next = createArenaBucket(size);
	arena->tail->next = next;
	arena->tail = next;
	arena->tail->len = size;
	return arena->tail->buffer;
}

static void arenaFree(arena_t *arena) {
	freeArenaBucket(arena->head);
	arena->head = NULL;
	arena->tail = NULL;
	free(arena);
}

static inline void initArena() {
	allocator.ctx = createArena();
}

static void *aalloc(size_t size) { return arenaAlloc(allocator.ctx, size); }

static void *arealloc(void *ptr, usize size) {
	(void)ptr;
	return aalloc(size);
}

static void empty(void *ptr) { (void)ptr; }

static void aclose() { arenaFree(allocator.ctx); }

#define ARENA_ALLOCATOR

#ifdef ARENA_ALLOCATOR
Allocator allocator = {
	.alloc 		= aalloc,
	.realloc 	= arealloc,
	.free			= empty,
	.init			= initArena,
	.close 		= aclose,
	.ctx 			= NULL
};
#else
Allocator allocator = {
	.alloc = malloc,
	.free = free,
	.init = empty,
	.close = empty,
	.ctx = NULL
};
#endif
