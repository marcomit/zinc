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

static void usage(char *program) {
	printf("Usage: %s <filename>\n", program);
	printf("Options:\n");
	printf("\t --debug -d Used for debug mode and inspect the insight of the compilation\n");
}

#define cmp(s, l) (strcmp(s, l) == 0)

ZState *loadState(int argc, char **argv) {
	char *err = NULL;
	if (argc < 2) goto stateErr;

	for (int i = 0; i < argc; i++) {
		printf("Arg %d: %s\n", i, argv[i]);
	}
	printf("End args\n");

	ZState *state = makestate(argv[1]);
	printf("State created\n");

	char *arg;
	for (int i = 2; i < argc; i++) {
		arg = argv[i];
		if (cmp(arg, "--debug") || cmp(arg, "-d")) {
			if (state->debug) {
				err = "Debug mode already setted";
				goto stateErr;
			}

			state->debug = true;
		} else if (cmp(arg, "--verbose") || cmp(arg, "-v")) {
			if (state->verbose) {
				err = "Verbose already setted";
				goto stateErr;
			}
			state->verbose = true;
		} else if (cmp(arg, "--unused-function")) {
			if (state->unusedFunc) {
				err = "Unused function flag already setted";
				goto stateErr;
			}
			state->unusedFunc = true;
		} else if (cmp(arg, "--unused-variable")) {
			if (state->unusedVar) {
				err = "Unused variable flag already setted";
				goto stateErr;
			}
			state->unusedVar = true;
		} else if (cmp(arg, "--unused-parameter")) {
			if (state->unusedStruct) {
				err = "Unused struct flag already setted";
				goto stateErr;
			}
			state->unusedStruct = true;
		}	else {
			err = "Undefined argument";
			goto stateErr;
		}
	}

	return state;

stateErr:
	if (err) printf("Error: %s\n", err);
	usage(argv[0]);
	return NULL;
}

int main(int argc, char **argv) {
	printf("Trying to load state\n");
	allocator.open();
	ZState *state = loadState(argc, argv);

	if (!state) return 1;
	printf("State loaded\n");

	visit(state, argv[1]);

	ZToken **tokens = ztokenize(state);
	printTokens(tokens);

	ZNode *root = zparse(state, tokens);

	printNode(root, 0);
	zanalyze(state, root);
	// zcompile(state, root, state->output);


	printLogs(state);

	allocator.close();

	return 0;
}
