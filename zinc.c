#include "zlex.h"
#include "zmem.h"
#include "zparse.h"
#include "zdebug.h"
#include "zsem.h"

#include <stdlib.h>
#include <stdio.h>

// #ifdef VEC_ALLOC
// #undef VEC_ALLOC
// #define VEC_ALLOC allocator.alloc
// #endif
//
// #ifdef VEC_REALLOC
// #undef VEC_REALLOC
// #define VEC_REALLOC allocator.realloc
// #endif
//
// #ifdef VEC_FREE
// #undef VEC_FREE
// #define VEC_FREE allocator.free
// #endif
//
int main(int argc, char **argv) {
	if (argc < 2) {
		printf("Usage: %s, <filename>", *argv);
		return 1;
	}

	allocator.open();

	ZToken **tokens = ztokenize(argv[1]);

	printTokens(tokens);
	ZNode *root = zparse(tokens, argv[1]);

	printNode(root, 0);

	zanalyze(root);

	allocator.close();

	return 0;
}
