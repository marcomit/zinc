#include "zinc.h"
#include "zcolors.h"

#include <stdlib.h>
#include <stdio.h>
#include <getopt.h>
#include <signal.h>
#include <execinfo.h>
#include <time.h>

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

static ZState *state = NULL;

static void usage(char *program) {
    printf("Usage: %s <filename> [options]\n", program);
    printf("Options:\n");
    printf("\t --debug -d           Enable debug mode\n");
    printf("\t --emit-llvm          Emit LLVM IR (.ll) instead of a native binary\n");
    printf("\t --unused-variable    Suppress 'unused variable' warnings\n");
    printf("\t --unused-function    Suppress 'unused function' warnings\n");
    printf("\t --unused-struct      Suppress 'unused struct' warnings\n");
}

#define CHECK_FLAG(flag, name) if (flag) {                                  \
    printf("Error %s already set\n", name);                                 \
    usage(argv[0]);                                                         \
    return NULL;                                                            \
}

#define SET_FLAG(flag, name) do {                                           \
    CHECK_FLAG(flag, name)                                                  \
    (flag) = true;                                                          \
} while(0)

#define SET_ARG(flag, name) do {                                            \
    CHECK_FLAG(flag, name)                                                  \
    (flag) = optarg;                                                        \
} while (0)

enum {
    OPT_EMIT_LLVM = 1 << 8,
    OPT_UNUSED_FUNC,
    OPT_UNUSED_VAR,
    OPT_UNUSED_STRUCT
};

static struct option long_options[] = {
    {"debug",           no_argument,        NULL, 'd'               },
    {"verbose",         no_argument,        NULL, 'v'               },
    {"emit-llvm",       no_argument,        NULL, OPT_EMIT_LLVM     },
    {"unused-function", no_argument,        NULL, OPT_UNUSED_FUNC   },
    {"unused-variable", no_argument,        NULL, OPT_UNUSED_VAR    },
    {"unused-struct",   no_argument,        NULL, OPT_UNUSED_STRUCT },
    {"output",          required_argument,  NULL, 'o'               },
    {NULL,              0,                  NULL, 0                 }
};

ZState *loadState(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return NULL; }

    char *filename = argv[1];

    argc--; argv++;

    ZState *state = makestate(argv[1]);

    int opt;   

    while (( opt = getopt_long(argc, argv, "dvo:", long_options, NULL) ) != -1) {
        switch (opt) {
        case 'o':               SET_ARG(state->output,          "Output file");             break;
        case 'd':               SET_FLAG(state->debug,          "Debug mode");              break;
        case 'v':               SET_FLAG(state->verbose,        "Verbose");                 break;
        case OPT_EMIT_LLVM:     SET_FLAG(state->emitLLVM,       "emit-llvm");               break;
        case OPT_UNUSED_FUNC:   SET_FLAG(state->unusedFunc,     "Unused function flag");    break;
        case OPT_UNUSED_VAR:    SET_FLAG(state->unusedVar,      "Unused function flag");    break;
        case OPT_UNUSED_STRUCT: SET_FLAG(state->unusedStruct,   "Unused function flag");    break;


        default: usage(argv[0]); return NULL;
        }
    }

    visit(state, filename);

    return state;

}

void handler(int sig) {
    (void)sig;
    void *array[20];
    size_t size;

    size = backtrace(array, 20);
    write(STDERR_FILENO, "Error: signal received\n", 23);
    backtrace_symbols_fd(array, size, STDERR_FILENO);

    if (state->debug) printLogs(state);
    _exit(1);
}

int pipeline(ZState *state) {
    ZToken **tokens = ztokenize(state);

    if (state->debug) printTokens(tokens);

    if (!canAdvance(state)) return 1;

    ZNode *root = zparse(state, tokens);

    if (!canAdvance(state)) return 2;
    zanalyze(state, root);

    if (state->debug) printNode(root, 0);

    if (!canAdvance(state)) return 3;
    zcompile(state, root, state->output);

    if (!canAdvance(state)) return 4;

    return 0;
}

int main(int argc, char **argv) {
    signal(SIGSEGV, handler);
    signal(SIGTRAP, handler);
    allocator.open();
    state = loadState(argc, argv);

    if (!state) return 1;

    struct timespec start, end;

    clock_gettime(CLOCK_MONOTONIC, &start);

    int res = pipeline(state);

    clock_gettime(CLOCK_MONOTONIC, &end);

    double elapsed =
        (end.tv_sec - start.tv_sec) +
        (end.tv_nsec - start.tv_nsec) / 1e9;
    
    printLogs(state);

    allocator.close();

    printf("  " COLOR_BOLD COLOR_GREEN "Finished" COLOR_RESET
            " build in %.02fs\n", elapsed);

    return res;
}
