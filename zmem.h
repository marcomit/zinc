#ifndef ZMEM_H
#define ZMEM_H

#include <stddef.h>
#include <stdint.h>

#define zalloc(t) allocator.alloc(sizeof(t))
#define znalloc(t, n) allocator.alloc(sizeof((t)) * (n))

typedef struct Allocator {
	void *(*alloc)(size_t);
	void (*free)(void *);
	void (*init)();
	void (*close)();

	void *ctx;
} Allocator;

extern Allocator allocator;

#endif
