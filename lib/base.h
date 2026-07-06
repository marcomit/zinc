// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#ifndef BASE_H
#define BASE_H

#include "compat.h"

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdbool.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>

#ifndef min
#define min(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef max
#define max(a, b) ((a) > (b) ? (a) : (b))
#endif

#define let __auto_type

#if defined(__clang__) || defined(__GNUC__)
#   define NOSANITIZE(attr) __attribute__((no_sanitize(attr)))
#else
#   define NOSANITIZE(attr)
#endif

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;

typedef float f32;
typedef double f64;

typedef size_t usize;

static inline void timer_start(struct timespec *t) {
    clock_gettime(CLOCK_MONOTONIC, t);
}

static inline double timer_elapsed(struct timespec start,
                                   const char **unit) {
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);

    double ns =
        (end.tv_sec - start.tv_sec) * 1e9 +
        (end.tv_nsec - start.tv_nsec);

    if (!unit)
        return ns;

    if (ns < 1e3) {
        *unit = "ns";
        return ns;
    }
    if (ns < 1e6) {
        *unit = "us";
        return ns / 1e3;
    }
    if (ns < 1e9) {
        *unit = "ms";
        return ns / 1e6;
    }

    *unit = "s";
    return ns / 1e9;
}

#endif
