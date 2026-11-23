#ifndef ZPOOL_H
#define ZPOOL_H

#include "zmem.h"

typedef struct {
    void        *ctx;
    usize       worker;
    Allocator   *allocator;
} ZTaskCtx;

typedef void (*ZTaskFn)(ZTaskCtx *);
typedef struct ZPool ZPool;

ZPool *pool_create(Allocator *allocator, usize threads);
void pool_submit(ZPool *pool, ZTaskFn fn, void *ctx);
void pool_wait(ZPool *pool);
void pool_destroy(ZPool *pool);

#endif // ZPOOL_H
