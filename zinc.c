#include "zinc.h"

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
	ZState *state = makestate(argv[1]);

	ZToken **tokens = ztokenize(state);

	printTokens(tokens);
	ZNode *root = zparse(state, tokens);

	printNode(root, 0);

	// zanalyze(state, root);

	printLogs(state);

	allocator.close();

	return 0;
}
