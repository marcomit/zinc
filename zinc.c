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
	printf("Usage: %s <filename> [options]\n", program);
	printf("Options:\n");
	printf("\t --debug -d Enable debug mode\n");
	printf("\t --unused-variable Suppress 'unsused variable' warnings\n");
	printf("\t --unused-function Suppress 'unsused function' warnings\n");
	printf("\t --unused-struct Suppress 'unsused struct' warnings\n");
}

#define cmp(s, l) (strcmp(s, l) == 0)

ZState *loadState(int argc, char **argv) {
	char *err = NULL;
	if (argc < 2) goto stateErr;

	ZState *state = makestate(argv[1]);

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
		} else if (cmp(arg, "--unused-struct")) {
			if (state->unusedStruct) {
				err = "Unused struct flag already setted";
				goto stateErr;
			}
			state->unusedStruct = true;
		}	else if (cmp(arg, "--output") || cmp(arg, "-o")) {
			if (state->output) {
				err = "Output already setted";
				goto stateErr;
			}
			i++;
			if (i >= argc) {
				err = "Missing output file";
				goto stateErr;
			}
			state->output = argv[i];
		} else {
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

	zanalyze(state, root);
	printNode(root, 0);
	zcompile(state, root, state->output);


	printLogs(state);

	allocator.close();

	return 0;
}
