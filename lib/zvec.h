// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

/**
 * @file zvec.h
 * @brief A lightweight, header-only dynamic array (vector) library for C
 *
 * This library provides a type-safe, generic vector implementation using C macros.
 * Vectors automatically grow as elements are added and store metadata inline with
 * the data for efficient access.
 *
 * @section design Design Overview
 *
 * Vectors use a hidden header approach: metadata (capacity and length) is stored
 * immediately before the data pointer returned to the user. This allows clean
 * syntax like `vec[i]` while maintaining O(1) access to metadata.
 *
 * Memory layout:
 * @code
 *   [vec_metadata | element0 | element1 | ... | elementN]
 *                  ^
 *                  |
 *              User pointer points here
 * @endcode
 *
 * @section usage Usage Example
 *
 * @code
 * #include "zvec.h"
 *
 * int *numbers = NULL;  // Always initialize to NULL
 *
 * // Push elements - vector grows automatically
 * vecpush(numbers, 10);
 * vecpush(numbers, 20);
 * vecpush(numbers, 30);
 *
 * // Access elements
 * printf("Length: %zu\n", veclen(numbers));     // 3
 * printf("Capacity: %zu\n", veccap(numbers));   // 16
 * printf("First: %d\n", numbers[0]);            // 10
 *
 * // Iterate
 * for (size_t i = 0; i < veclen(numbers); i++) {
 *     printf("%d ", numbers[i]);
 * }
 *
 * // Clean up
 * vecfree(numbers);  // Sets numbers to NULL
 * @endcode
 *
 * @section notes Important Notes
 *
 * - Always initialize vectors to NULL
 * - vecpush updates the vector pointer in place (it must be an lvalue)
 * - A vector keeps the allocator it was created with; vecpush uses
 *   vecDefaultAllocator only when it has to create the vector
 * - After vecfree, the pointer is set to NULL
 * - Capacity is measured in number of elements, not bytes
 * - Default initial capacity is 16 elements
 * - Capacity doubles when exceeded
 *
 * @section limitations Limitations
 *
 * - Not thread-safe
 * - No bounds checking in release builds
 * - Reallocation may invalidate existing pointers to elements
 * - vecpop returns the removed element; check vecempty first
 *
 * @author Marco Menegazzi
 * @date 2026
 * @version 1.0
 */

#ifndef ZVEC_H
#define ZVEC_H

#include "base.h"
#include "zmem.h"
#include <stdlib.h>
#include <string.h>

extern Allocator *vecDefaultAllocator;

/* ============================================================================
 * Configuration Macros
 * ============================================================================ */

/**
 * @brief Initial capacity (number of elements) for new vectors
 */
#define VEC_DEFAULT_SIZE 16

/* ============================================================================
 * Internal Implementation Macros - Do Not Use Directly
 * ============================================================================ */

/**
 * @internal
 * @brief Get pointer to the metadata header from a vector pointer
 * @param v Vector pointer
 * @return Pointer to vec_metadata structure
 */
#define VEC_METADATA(v) ((vec_metadata *)((u8 *)(v) - sizeof(vec_metadata)))

/**
 * @internal
 * @brief Reallocate vector to new capacity (in bytes, not elements)
 * @param v Vector pointer
 * @param new_capacity_bytes New total capacity in bytes
 * @return New vector pointer or NULL on failure
 */
#define VEC_REALLOC_IMPL(v, new_capacity_bytes) ({                                                      \
    vec_metadata *old_meta = VEC_METADATA(v);                                                           \
    usize used_bytes = sizeof(vec_metadata) + old_meta->length * sizeof(*(v));                          \
    vec_metadata *new_meta = aalloc(old_meta->allocator, sizeof(vec_metadata) + (new_capacity_bytes));  \
    if (new_meta) {                                                                                     \
        memcpy(new_meta, old_meta, used_bytes);                                                         \
        afree(old_meta->allocator, old_meta);                                                           \
    }                                                                                                   \
    (new_meta ? (void *)(new_meta + 1) : NULL);                                                         \
})

/* ============================================================================
 * Public API Macros
 * ============================================================================ */

/**
 * @brief Get the current capacity of a vector (number of elements)
 * @param v Vector pointer
 * @return Capacity as usize
 * @note Returns number of elements, not bytes
 */
#define veccap(v) (VEC_METADATA(v)->capacity)

/**
 * @brief Get the current length of a vector (number of elements)
 * @param v Vector pointer
 * @return Length as usize
 */
#define veclen(v) ((v) ? VEC_METADATA(v)->length : 0)

/**
 * @brief Set the length of a vector
 * @param v Vector pointer
 * @param l New length
 * @warning Does not actually add or remove elements, only changes the length counter
 */
#define vecsetlen(v, l) (VEC_METADATA(v)->length = (l))

/**
 * @brief Set the capacity of a vector
 * @param v Vector pointer
 * @param c New capacity
 * @internal This is for internal use; use vecreserve for user code
 */
#define vecsetcap(v, c) (VEC_METADATA(v)->capacity = (c))

#define vecnew(a, T) ((T *)veccreatewith((a), sizeof(T)))

/**
 * @brief Push an element to the end of a vector
 * @param v Vector pointer (may be NULL for first push)
 * @param i Element to push
 *
 * @note If the vector is NULL, it will be created automatically.
 * @note If capacity is exceeded, the vector will be reallocated (capacity doubles).
 * @note On reallocation failure, the operation fails silently and the vector remains unchanged.
 *
 * @code
 * int *vec = NULL;
 * vecpush(vec, 42);
 * vecpush(vec, 100);
 * @endcode
 */
#define vecpush(v, i) vecpushwith(v, i, vecDefaultAllocator)

#define vecpushwith(v, i, a)                                                    \
do {                                                                            \
    if (!(v)) {                                                                 \
        (v) = veccreatewith((a), sizeof(*(v)));                                 \
    }                                                                           \
    if (!(v)) break;                                                            \
    if (veclen(v) >= veccap(v)) {                                               \
        usize new_cap = veccap(v) << 1;                                         \
        void *new_v = VEC_REALLOC_IMPL((v), new_cap * sizeof(*(v)));            \
        if (!(new_v)) break;                                                    \
        (v) = new_v;                                                            \
        vecsetcap((v), new_cap);                                                \
    }                                                                           \
    (v)[veclen(v)] = (i);                                                       \
    vecsetlen((v), veclen(v) + 1);                                              \
} while (0)

/**
 * @brief Clear all elements from a vector (sets length to 0)
 * @param v Vector pointer
 * @note Does not deallocate memory or change capacity
 */
#define vecclear(v) vecsetlen((v), 0)

/**
 * @brief Free a vector and set pointer to NULL
 * @param v Vector pointer
 *
 * @code
 * int *vec = NULL;
 * vecpush(vec, 1);
 * vecfree(vec);  // vec is now NULL
 * @endcode
 */
#define vecfree(v)                                                              \
do {                                                                            \
    if ((v)) {                                                                  \
        afree(VEC_METADATA(v)->allocator, VEC_METADATA(v));                     \
        (v) = NULL;                                                             \
    }                                                                           \
} while (0)

/**
 * @brief Remove and return the last element of a vector
 * @param v Vector pointer
 * @warning Undefined behavior if the vector is NULL or empty; check vecempty first
 *
 * @code
 * int last = vecpop(vec);
 * @endcode
 */
#define vecpop(v) ({                                                            \
    if ((v) && veclen(v) > 0) {                                                 \
            vecsetlen((v), veclen(v) - 1);                                      \
    }                                                                           \
    (v)[veclen(v)];                                                             \
})

/**
 * @brief Check if a vector is empty
 * @param v Vector pointer
 * @return Non-zero if empty (length is 0 or vector is NULL), 0 otherwise
 */
#define vecempty(v) ((v) == NULL || veclen(v) == 0)

/**
 * @brief Get the last element of a vector
 * @param v Vector pointer
 * @return Last element
 * @warning Undefined behavior if vector is empty; check vecempty first
 */
#define veclast(v) ((v)[veclen(v) - 1])

/**
 * @brief Get the first element of a vector
 * @param v Vector pointer
 * @return First element
 * @warning Undefined behavior if vector is empty; check vecempty first
 */
#define vecfirst(v) ((v)[0])

/**
 * @brief Reserve capacity for at least n elements
 * @param v Vector pointer
 * @param n Minimum capacity needed
 * @note If current capacity is sufficient, does nothing
 *
 * @code
 * int *vec = NULL;
 * vecreserve(vec, 1000);  // Pre-allocate space for 1000 integers
 * @endcode
 */
#define vecreserve(v, n) vecreservewith(v, n, vecDefaultAllocator)

#define vecreservewith(v, n, a)                                                 \
do {                                                                            \
    if (!(v)) {                                                                 \
        (v) = veccreatecap((a), sizeof(*(v)), (n));                             \
    }                                                                           \
    if ((v) && veccap(v) < (n)) {                                               \
        void *new_v = VEC_REALLOC_IMPL((v), (n) * sizeof(*(v)));                \
        if (new_v) {                                                            \
            (v) = new_v;                                                        \
            vecsetcap((v), (n));                                                \
        }                                                                       \
    }                                                                           \
} while (0)

#define vecunion(v, r, n)                                                       \
do {                                                                            \
    for (usize vecunionIndex = 0; vecunionIndex < (n); vecunionIndex++) {       \
        vecpush(v, r[vecunionIndex]);                                           \
    }                                                                           \
} while(0)

/* ============================================================================
 * Data Structures
 * ============================================================================ */

/**
 * @brief Metadata stored before the vector data
 *
 * This structure is hidden from the user and accessed via macros.
 * It stores the vector's capacity and current length.
 */
typedef struct vec_metadata {
    usize       capacity;   /**< Maximum number of elements before reallocation */
    usize       length;     /**< Current number of elements in the vector */
    Allocator   *allocator; /**< Allocator the vector was created with, used for every growth and free */
} vec_metadata;

/* ============================================================================
 * Functions
 * ============================================================================ */

/**
 * @brief Create a new vector with room for at least `capacity` elements
 * @param allocator Allocator that owns the vector (NULL fails)
 * @param element_size Size of each element in bytes (use sizeof(type))
 * @param capacity Minimum capacity; never less than VEC_DEFAULT_SIZE
 * @return Pointer to the vector data, or NULL on allocation failure
 */
static inline void *veccreatecap(Allocator *allocator, usize element_size, usize capacity) {
    if (!allocator) return NULL;
    if (capacity < VEC_DEFAULT_SIZE) capacity = VEC_DEFAULT_SIZE;
    usize total_bytes = sizeof(vec_metadata) + (capacity * element_size);
    vec_metadata *metadata = (vec_metadata *)aalloc(allocator, total_bytes);

    if (!metadata) return NULL;
    metadata->capacity  = capacity;
    metadata->length    = 0;
    metadata->allocator = allocator;

    return (void *)(metadata + 1);
}

/**
 * @brief Create a new vector with default capacity
 * @param allocator Allocator that owns the vector (NULL fails)
 * @param element_size Size of each element in bytes (use sizeof(type))
 * @return Pointer to the vector data, or NULL on allocation failure
 *
 * @note Users typically don't call this directly; vecpush handles creation
 *
 * @code
 * int *vec = veccreatewith(heapAllocator, sizeof(int));
 * @endcode
 */
static inline void *veccreatewith(Allocator *allocator, usize element_size) {
    return veccreatecap(allocator, element_size, VEC_DEFAULT_SIZE);
}

#endif /* ZVEC_H */
