#include "zmem.h"
#include "zvec.h"
#include "zarena.h"

#include <stdlib.h>

#define ARENA_ALIGNMENT 8
#define ARENA_ALIGN(size) (((size) + (ARENA_ALIGNMENT - 1)) & ~(ARENA_ALIGNMENT - 1))

#define ARENA_PAGE_SIZE MiB(2)

static ArenaBucket *createArenaBucket(usize requested) {
    usize actual = requested > ARENA_PAGE_SIZE ? requested : ARENA_PAGE_SIZE;

    ArenaBucket *self = malloc(sizeof(ArenaBucket) + actual);
    self->len = 0;
    self->size = ARENA_PAGE_SIZE;
    self->next = NULL;
    return self;
}

arena_t *createArena() {
    arena_t *self = malloc(sizeof(arena_t));
    self->head = createArenaBucket(0);
    self->tail = self->head;
    self->scopes = NULL;
    return self;
}

static void freeArenaBucket(ArenaBucket *arena, bool recursive) {
    if (!arena) return;
    // printf("BUCKET ALLOCATION: %zu\n", arena->len);
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

    ArenaBucket *next = createArenaBucket(size);
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

static inline void initArena() {
    allocator.ctx = createArena();
}

static void *aalloc(usize size) { return arenaAlloc(allocator.ctx, size); }

static void *arealloc(void *ptr, usize size) {
    (void)ptr;
    return aalloc(size);
}

void arenaScope(arena_t *arena) {
    ArenaScope *scope = malloc(sizeof(ArenaScope));
    scope->bucket = arena->tail;
    scope->pos = arena->tail->len;
    vecpush(arena->scopes, scope);
}

void arenaEndScope(arena_t *arena) {
    if (vecempty(arena->scopes)) return;

    ArenaScope *scope = vecpop(arena->scopes);
    
    ArenaBucket *curr = arena->head;
    while (curr && curr != scope->bucket) curr = curr->next;

    if (!curr) return;

    curr->len = scope->pos;
    curr->next = NULL;
    arena->tail = curr;
    freeArenaBucket(scope->bucket->next, true);
    u8 *buffer = (u8 *)(arena->tail + 1);
    memset(buffer + curr->len, 0, curr->size - curr->len);
}

static void empty       (void *ptr) { (void)ptr; }
static void aclose      ()          { arenaFree(allocator.ctx); }
static void aascope     ()          { arenaScope(allocator.ctx); }
static void aaendscope  ()          { arenaEndScope(allocator.ctx); }


#define ARENA_ALLOCATOR
#ifdef ARENA_ALLOCATOR
Allocator allocator = {
    .alloc          = aalloc,
    .realloc        = arealloc,
    .free           = empty,
    .open           = initArena,
    .close          = aclose,
    .startScope     = aascope,
    .endScope       = aaendscope,
    .ctx            = NULL
};
#else
Allocator allocator = {
    .alloc          = malloc,
    .realloc        = realloc,
    .free           = free,
    .open           = empty,
    .close          = empty,
    .startScope     = empty,
    .endScope       = empty,
    .ctx            = NULL
};
#endif
