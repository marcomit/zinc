/**
 * @file zhset.h
 * @brief A lightweight, header-only string hashset for C
 *
 * This library provides an open-addressing hashset for C strings using
 * the same hidden-metadata approach as zvec.h.
 *
 * Memory layout:
 * @code
 *   [hashset_metadata | bucket0 | bucket1 | ... | bucketN]
 *                      ^
 *                      |
 *                  User pointer points here (array of const char*)
 * @endcode
 *
 * Buckets hold three sentinel states:
 *   NULL      = empty (never used)
 *   TOMBSTONE = deleted (skip during probe, reusable on insert)
 *   other     = occupied (points to a live string)
 *
 * @section usage Usage Example
 *
 * @code
 * #include "zhset.h"
 *
 * hashset_t set = NULL;
 *
 * hashset_insert(&set, "foo");
 * hashset_insert(&set, "bar");
 *
 * if (hashset_has(set, "foo"))  printf("found foo\n");
 * if (!hashset_has(set, "baz")) printf("no baz\n");
 *
 * hashset_remove(&set, "foo");
 *
 * printf("count: %zu\n", hashset_len(set));   // 1
 *
 * hashset_free(&set);  // set is now NULL
 * @endcode
 *
 * @section notes Important Notes
 *
 * - Always initialize to NULL
 * - The set does NOT copy strings; the caller must keep them alive
 * - Capacity is always a power of two
 * - Grows at 70% load factor
 * - Uses FNV-1a hashing
 * - Not thread-safe
 */

#ifndef ZHASHSET_H
#define ZHASHSET_H

#include "base.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Configuration
 * ============================================================================ */

#define HASHSET_ALLOC   malloc
#define HASHSET_FREE    free
#define HASHSET_DEFAULT_CAP 16
#define HASHSET_MAX_LOAD 70 /* percent */

/* ============================================================================
 * Internals
 * ============================================================================ */

/** @brief Tombstone marker for deleted slots */
#define HASHSET_TOMBSTONE ((const char *)(uintptr_t)1)

/** @brief Metadata stored before the bucket array */
typedef struct hashset_metadata {
	usize capacity; /**< Number of buckets (always power of two) */
	usize length;   /**< Number of live entries */
} hashset_metadata;

/** @brief The user-facing type: pointer to bucket array */
typedef const char **hashset_t;

#define HASHSET_META(s) ((hashset_metadata *)((u8 *)(s) - sizeof(hashset_metadata)))

/* ============================================================================
 * Hash function — FNV-1a
 * ============================================================================ */

static inline u32 hashset__fnv1a(const char *s) {
	u32 h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (u8)*s) * 16777619u;
	return h;
}

/* ============================================================================
 * Core helpers
 * ============================================================================ */

static inline hashset_t hashset__create(usize cap) {
	usize total = sizeof(hashset_metadata) + cap * sizeof(const char *);
	hashset_metadata *meta = (hashset_metadata *)HASHSET_ALLOC(total);
	if (!meta) return NULL;
	meta->capacity = cap;
	meta->length = 0;
	const char **buckets = (const char **)(meta + 1);
	memset(buckets, 0, cap * sizeof(const char *));
	return buckets;
}

/**
 * @brief Find the bucket index for a key
 * @param buckets  Bucket array
 * @param cap      Current capacity
 * @param key      String to look up
 * @param[out] out_idx  Set to the found or insertion index
 * @return true if the key is already present
 */
static inline bool hashset__find(const char **buckets, usize cap,
                                 const char *key, usize *out_idx) {
	u32 h = hashset__fnv1a(key);
	usize mask = cap - 1;
	usize idx = h & mask;
	usize first_tombstone = (usize)-1;

	for (usize i = 0; i < cap; i++) {
		const char *slot = buckets[idx];
		if (slot == NULL) {
			/* empty — key not in table */
			*out_idx = (first_tombstone != (usize)-1) ? first_tombstone : idx;
			return false;
		}
		if (slot == HASHSET_TOMBSTONE) {
			if (first_tombstone == (usize)-1) first_tombstone = idx;
		} else if (strcmp(slot, key) == 0) {
			*out_idx = idx;
			return true;
		}
		idx = (idx + 1) & mask;
	}
	/* table is full of tombstones — shouldn't happen with proper load factor */
	*out_idx = first_tombstone;
	return false;
}

static inline void hashset__grow(hashset_t *set) {
	hashset_metadata *old_meta = HASHSET_META(*set);
	usize old_cap = old_meta->capacity;
	usize new_cap = old_cap << 1;

	hashset_t new_set = hashset__create(new_cap);
	if (!new_set) return;

	const char **old_buckets = *set;
	for (usize i = 0; i < old_cap; i++) {
		const char *slot = old_buckets[i];
		if (slot && slot != HASHSET_TOMBSTONE) {
			usize idx;
			hashset__find(new_set, new_cap, slot, &idx);
			new_set[idx] = slot;
		}
	}

	HASHSET_META(new_set)->length = old_meta->length;
	HASHSET_FREE(old_meta);
	*set = new_set;
}

/* ============================================================================
 * Public API
 * ============================================================================ */

/** @brief Number of live entries */
static inline usize hashset_len(hashset_t set) {
	return set ? HASHSET_META(set)->length : 0;
}

/** @brief Current bucket count */
static inline usize hashset_cap(hashset_t set) {
	return set ? HASHSET_META(set)->capacity : 0;
}

/**
 * @brief Insert a string into the set
 * @param set  Pointer to hashset (may be NULL; will be created)
 * @param key  String to insert (not copied — caller owns the memory)
 * @return true if inserted, false if already present or OOM
 */
static inline bool hashset_insert(hashset_t *set, const char *key) {
	if (!*set) {
		*set = hashset__create(HASHSET_DEFAULT_CAP);
		if (!*set) return false;
	}

	hashset_metadata *meta = HASHSET_META(*set);
	/* grow if load factor >= 70% */
	if ((meta->length + 1) * 100 >= meta->capacity * HASHSET_MAX_LOAD) {
		hashset__grow(set);
		meta = HASHSET_META(*set);
	}

	usize idx;
	if (hashset__find(*set, meta->capacity, key, &idx))
		return false; /* duplicate */

	(*set)[idx] = key;
	meta->length++;
	return true;
}

/**
 * @brief Check if a string is in the set
 * @return true if present
 */
static inline bool hashset_has(hashset_t set, const char *key) {
	if (!set) return false;
	usize idx;
	return hashset__find(set, HASHSET_META(set)->capacity, key, &idx);
}

/**
 * @brief Remove a string from the set
 * @return true if the key was found and removed
 */
static inline bool hashset_remove(hashset_t *set, const char *key) {
	if (!*set) return false;
	hashset_metadata *meta = HASHSET_META(*set);
	usize idx;
	if (!hashset__find(*set, meta->capacity, key, &idx))
		return false;
	(*set)[idx] = HASHSET_TOMBSTONE;
	meta->length--;
	return true;
}

/** @brief Free the set and set pointer to NULL */
static inline void hashset_free(hashset_t *set) {
	if (*set) {
		HASHSET_FREE(HASHSET_META(*set));
		*set = NULL;
	}
}

/** @brief Reset length to 0 and clear all buckets */
static inline void hashset_clear(hashset_t set) {
	if (!set) return;
	hashset_metadata *meta = HASHSET_META(set);
	memset(set, 0, meta->capacity * sizeof(const char *));
	meta->length = 0;
}

#endif /* ZHASHSET_H */
