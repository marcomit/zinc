#ifndef ZMEM_H
#define ZMEM_H

#include "base.h"

#include <stddef.h>
#include <stdint.h>

#define zalloc(t) (t *)allocator.alloc(sizeof(t))
#define znalloc(t, n) (t *)allocator.alloc(sizeof(t) * (n))

#define KiB(n) ((n) << 10)
#define MiB(n) ((n) << 20)
#define GiB(n) ((n) << 30)

typedef struct Allocator {
	void *(*alloc)(usize);
	void *(*realloc)(void *, usize);
	void (*free)(void *);
	void (*open)();
	void (*close)();

	void (*startScope)();
	void (*endScope)();

	void *ctx;
} Allocator;

extern Allocator allocator;

#endif
