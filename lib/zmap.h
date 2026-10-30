// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

/**
 * @file zmap.h
 * @brief A lightweight, header-only hash map library for C
 *
 * The map counterpart of zvec.h: the same hidden-header design and the same
 * macro API style, but every element is a key/value pair and elements are
 * looked up by key instead of by index.
 *
 * @section design Design Overview
 *
 * A map is a pointer to its first slot. Metadata is stored immediately before
 * it, and one state byte per slot (empty, used or deleted) immediately after
 * the last slot. Collisions are resolved by linear probing.
 *
 * Memory layout:
 * @code
 *   [map_metadata | slot0 | slot1 | ... | slotN | state0 | ... | stateN]
 *                  ^
 *                  |
 *              User pointer points here
 * @endcode
 *
 * Each slot is a `struct { K key; V value; }`. Keys are hashed and compared
 * byte by byte, so they must be plain values without padding: integers,
 * pointers, enums. A `char *` key is compared by address, not by content.
 *
 * @section usage Usage Example
 *
 * @code
 * #include "zmap.h"
 *
 * ZMap(int, const char *) names = NULL;   // Always initialize to NULL
 *
 * // Insert or overwrite - the map grows automatically
 * mapput(names, 1, "one");
 * mapput(names, 2, "two");
 * mapput(names, 1, "uno");
 *
 * // Look up: a pointer to the value, or NULL when the key is missing
 * const char **one = mapget(names, 1);
 * printf("%s %zu\n", *one, maplen(names)); // uno 2
 *
 * // Iterate over every used slot
 * for (usize i = 0; i < mapcap(names); i++) {
 *     if (!mapused(names, i)) continue;
 *     printf("%d = %s\n", names[i].key, names[i].value);
 * }
 *
 * mapdel(names, 2);
 * mapfree(names);  // Sets names to NULL
 * @endcode
 *
 * @section notes Important Notes
 *
 * - Always initialize maps to NULL
 * - mapput updates the map pointer in place (it must be an lvalue)
 * - A map keeps the allocator it was created with; mapput uses
 *   mapDefaultAllocator only when it has to create the map
 * - Every ZMap(K, V) is a distinct anonymous struct type: typedef it to pass
 *   the same map type around (`typedef ZMap(int, int) IntMap;`)
 * - Default initial capacity is 16 slots, always a power of two
 * - Capacity doubles when the map is more than half full
 *
 * @section limitations Limitations
 *
 * - Not thread-safe
 * - Growing (on mapput) moves every slot: pointers from mapget are only
 *   valid until the next mapput
 *
 * @author Marco Menegazzi
 * @date 2026
 * @version 1.0
 */

#ifndef ZMAP_H
#define ZMAP_H

#include "base.h"
#include "zmem.h"
#include <stdlib.h>
#include <string.h>


/* ============================================================================
 * Configuration Macros
 * ============================================================================ */

/**
 * @brief Initial capacity (number of slots) for new maps
 */
#define MAP_DEFAULT_SIZE 16

/**
 * @brief Maximum percentage of non-empty slots (used + deleted) before a rehash
 */
#define MAP_MAX_LOAD 70

/* ============================================================================
 * Internal Implementation Macros - Do Not Use Directly
 * ============================================================================ */

#define MAP_SLOT_EMPTY   0
#define MAP_SLOT_USED    1
#define MAP_SLOT_DELETED 2

/**
 * @internal
 * @brief Get pointer to the metadata header from a map pointer
 */
#define MAP_METADATA(m) ((map_metadata *)((u8 *)(m) - sizeof(map_metadata)))

/**
 * @internal
 * @brief Pointer to the state bytes of a map
 */
#define MAP_STATES(m) mapstates((m), sizeof(*(m)))

/**
 * @internal
 * @brief Size of a map's key type
 *
 * Taken from the type rather than from a key expression: for pointer keys
 * the pointer's own size is what gets hashed and compared, and
 * sizeof(pointer expression) reads as a bug to linters.
 */
#define MAP_KEY_SIZE(m) sizeof(__typeof__((m)->key))

/* ============================================================================
 * Public API Macros
 * ============================================================================ */

/**
 * @brief Declare a map type from `K` to `V`
 *
 * @code
 * ZMap(void *, int) counts = NULL;
 * typedef ZMap(u32, char *) NameTable;
 * @endcode
 */
#define ZMap(K, V) struct { K key; V value; } *

/**
 * @brief Get the number of slots of a map (0 for a NULL map)
 */
#define mapcap(m) ((m) ? MAP_METADATA(m)->capacity : 0)

/**
 * @brief Get the number of keys in a map (0 for a NULL map)
 */
#define maplen(m) ((m) ? MAP_METADATA(m)->length : 0)

/**
 * @brief Check if a map has no keys
 */
#define mapempty(m) (maplen(m) == 0)

/**
 * @brief Check if slot `i` holds a key, for iterating with `m[i].key` and `m[i].value`
 * @param m Map pointer (must not be NULL)
 * @param i Slot index, below mapcap(m)
 */
#define mapused(m, i) (MAP_STATES(m)[i] == MAP_SLOT_USED)

/**
 * @brief Insert a key, or overwrite its value if it is already there
 * @param m Map pointer (may be NULL for the first insertion)
 * @param k Key
 * @param v Value
 *
 * @note If the map is NULL, it will be created with mapDefaultAllocator.
 * @note On allocation failure, the operation fails silently and the map remains unchanged.
 */
#define mapput(m, k, v) mapputwith(m, k, v, mapDefaultAllocator ? mapDefaultAllocator : heapAllocator)

#define mapputwith(m, k, v, a)                                                  \
do {                                                                            \
    __typeof__((m)->key) mapKey_ = (k);                                         \
    void *mapTmp_ = (m);                                                        \
    if (!mapreserveone(&mapTmp_, (a), sizeof(*(m)), MAP_KEY_SIZE(m))) break;   \
    (m) = mapTmp_;                                                              \
    bool mapFound_;                                                             \
    usize mapSlot_ = mapslot((m), sizeof(*(m)), &mapKey_, MAP_KEY_SIZE(m), &mapFound_); \
    if (!mapFound_) mapclaim((m), sizeof(*(m)), mapSlot_);                      \
    (m)[mapSlot_].key   = mapKey_;                                              \
    (m)[mapSlot_].value = (v);                                                  \
} while (0)

/**
 * @brief Look up a key
 * @param m Map pointer (may be NULL)
 * @param k Key
 * @return Pointer to the value, or NULL if the key is not in the map
 *
 * @code
 * int *count = mapget(counts, ptr);
 * if (count) (*count)++;
 * @endcode
 */
#define mapget(m, k) ({                                                         \
    __typeof__((m)->key) mapKey_ = (k);                                         \
    bool mapFound_ = false;                                                     \
    usize mapSlot_ = (m) ? mapslot((m), sizeof(*(m)), &mapKey_, MAP_KEY_SIZE(m), &mapFound_) : 0; \
    mapFound_ ? &(m)[mapSlot_].value : NULL;                                    \
})

/**
 * @brief Check if a key is in the map
 */
#define maphas(m, k) (mapget(m, k) != NULL)

/**
 * @brief Remove a key, if it is in the map
 * @param m Map pointer (may be NULL)
 * @param k Key
 */
#define mapdel(m, k)                                                            \
do {                                                                            \
    if (!(m)) break;                                                            \
    __typeof__((m)->key) mapKey_ = (k);                                         \
    bool mapFound_;                                                             \
    usize mapSlot_ = mapslot((m), sizeof(*(m)), &mapKey_, MAP_KEY_SIZE(m), &mapFound_); \
    if (!mapFound_) break;                                                      \
    MAP_STATES(m)[mapSlot_] = MAP_SLOT_DELETED;                                 \
    MAP_METADATA(m)->length--;                                                  \
    MAP_METADATA(m)->deleted++;                                                 \
} while (0)

/**
 * @brief Remove every key from a map
 * @note Does not deallocate memory or change capacity
 */
#define mapclear(m)                                                             \
do {                                                                            \
    if (!(m)) break;                                                            \
    memset(MAP_STATES(m), MAP_SLOT_EMPTY, MAP_METADATA(m)->capacity);           \
    MAP_METADATA(m)->length  = 0;                                               \
    MAP_METADATA(m)->deleted = 0;                                               \
} while (0)

/**
 * @brief Free a map and set pointer to NULL
 */
#define mapfree(m)                                                              \
do {                                                                            \
    if ((m)) {                                                                  \
        afree(MAP_METADATA(m)->allocator, MAP_METADATA(m));                     \
        (m) = NULL;                                                             \
    }                                                                           \
} while (0)

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * @brief Metadata stored before the map slots
 */
typedef struct map_metadata {
    usize       capacity;   /**< Number of slots, always a power of two */
    usize       length;     /**< Number of keys in the map */
    usize       deleted;    /**< Number of deleted slots, reclaimed on the next rehash */
    Allocator   *allocator; /**< Allocator the map was created with, used for every growth and free */
} map_metadata;

/* ============================================================================
 * Functions
 * ============================================================================ */

/**
 * @internal
 * @brief State bytes of a map with slots of `slot_size` bytes
 */
static inline u8 *mapstates(void *m, usize slot_size) {
    return (u8 *)m + MAP_METADATA(m)->capacity * slot_size;
}

/**
 * @internal
 * @brief FNV-1a over the key bytes, with a final mix so pointer keys
 * (whose low bits are mostly zero) spread over the whole table
 */
static inline u64 maphash(const void *key, usize key_size) {
    const u8 *p = (const u8 *)key;
    u64 h = 0xcbf29ce484222325ULL;
    for (usize i = 0; i < key_size; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h;
}

/**
 * @brief Create a new, empty map with room for at least `capacity` slots
 * @param allocator Allocator that owns the map (NULL fails)
 * @param slot_size Size of each slot in bytes (sizeof(*map))
 * @param capacity Minimum capacity; rounded up to a power of two, never less than MAP_DEFAULT_SIZE
 * @return Pointer to the first slot, or NULL on allocation failure
 */
static inline void *mapcreatecap(Allocator *allocator, usize slot_size, usize capacity) {
    if (!allocator) return NULL;
    usize cap = MAP_DEFAULT_SIZE;
    while (cap < capacity) cap <<= 1;

    usize total_bytes = sizeof(map_metadata) + cap * slot_size + cap;
    map_metadata *metadata = (map_metadata *)aalloc(allocator, total_bytes);
    if (!metadata) return NULL;

    metadata->capacity  = cap;
    metadata->length    = 0;
    metadata->deleted   = 0;
    metadata->allocator = allocator;

    void *m = (void *)(metadata + 1);
    memset(mapstates(m, slot_size), MAP_SLOT_EMPTY, cap);
    return m;
}

/**
 * @internal
 * @brief Find the slot of `key`
 *
 * Sets `found` and returns the slot holding the key, or the slot where it
 * should be inserted (reusing the first deleted slot met on the way).
 */
static inline usize mapslot(void *m, usize slot_size, const void *key, usize key_size, bool *found) {
    usize mask      = MAP_METADATA(m)->capacity - 1;
    u8 *states      = mapstates(m, slot_size);
    usize deleted   = (usize)-1;

    for (usize i = maphash(key, key_size) & mask;; i = (i + 1) & mask) {
        switch (states[i]) {
        case MAP_SLOT_EMPTY:
            *found = false;
            return deleted != (usize)-1 ? deleted : i;
        case MAP_SLOT_DELETED:
            if (deleted == (usize)-1) deleted = i;
            break;
        default:
            /* The key is always the first member of a slot. */
            if (memcmp((u8 *)m + i * slot_size, key, key_size) == 0) {
                *found = true;
                return i;
            }
            break;
        }
    }
}

/**
 * @internal
 * @brief Mark a free slot (from mapslot) as used
 */
static inline void mapclaim(void *m, usize slot_size, usize slot) {
    u8 *states = mapstates(m, slot_size);
    if (states[slot] == MAP_SLOT_DELETED) MAP_METADATA(m)->deleted--;
    states[slot] = MAP_SLOT_USED;
    MAP_METADATA(m)->length++;
}

/**
 * @internal
 * @brief Make sure `*m` exists and has room for one more key
 *
 * Creates the map, or rehashes it into a new allocation when too few empty
 * slots are left: doubling it when it's more than half full, otherwise just
 * dropping the deleted slots.
 *
 * @return false on allocation failure; `*m` is left untouched
 */
static inline bool mapreserveone(void **m, Allocator *allocator, usize slot_size, usize key_size) {
    if (!*m) {
        *m = mapcreatecap(allocator, slot_size, MAP_DEFAULT_SIZE);
        return *m != NULL;
    }

    map_metadata *old = MAP_METADATA(*m);
    if ((old->length + old->deleted + 1) * 100 <= old->capacity * MAP_MAX_LOAD) return true;

    usize cap = (old->length + 1) * 2 > old->capacity ? old->capacity * 2 : old->capacity;
    void *grown = mapcreatecap(old->allocator, slot_size, cap);
    if (!grown) return false;

    u8 *states = mapstates(*m, slot_size);
    for (usize i = 0; i < old->capacity; i++) {
        if (states[i] != MAP_SLOT_USED) continue;
        void *slot = (u8 *)*m + i * slot_size;
        bool found;
        usize at = mapslot(grown, slot_size, slot, key_size, &found);
        memcpy((u8 *)grown + at * slot_size, slot, slot_size);
        mapclaim(grown, slot_size, at);
    }

    afree(old->allocator, old);
    *m = grown;
    return true;
}

#endif /* ZMAP_H */
