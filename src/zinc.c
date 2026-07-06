// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zinc.h"
#include "base.h"
#include "zcolors.h"

#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
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
    printf("\t -d --debug               Enable debug mode (sets -O0)\n");
    printf("\t -v --verbose             Enable verbose mode\n");
    printf("\t --emit=exe|obj|ir|asm    Select output type (default: exe)\n");
    printf("\t --unused-variable        Suppress 'unused variable' warnings\n");
    printf("\t --unused-function        Suppress 'unused function' warnings\n");
    printf("\t --unused-struct          Suppress 'unused struct' warnings\n");
    printf("\t --skip-llvm-validation   Does not verify the generated LLVM code\n");
    printf("\nOptimization:\n");
    printf("\t -O0 -O1 -O2 -O3 -Os -Oz  Set optimization level (default: -O2)\n");
    printf("\t --release                 Alias for -O2\n");
    printf("\t --release-fast            Alias for -O3\n");
    printf("\t --release-small           Alias for -Os\n");
    printf("\nLink-time optimization:\n");
    printf("\t --lto=off|thin|full       Set LTO mode (default: off)\n");
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
    OPT_EMIT = 1 << 8,
    OPT_UNUSED_FUNC,
    OPT_UNUSED_VAR,
    OPT_UNUSED_STRUCT,
    OPT_SKIP_LLVM_VALIDATION,
    OPT_LTO,
    OPT_RELEASE,
    OPT_RELEASE_FAST,
    OPT_RELEASE_SMALL
};

static struct option long_options[] = {
    {"debug",                   no_argument,        NULL,   'd'                     },
    {"emit",                    required_argument,  NULL,   OPT_EMIT                },
    {"unused-function",         no_argument,        NULL,   OPT_UNUSED_FUNC         },
    {"unused-variable",         no_argument,        NULL,   OPT_UNUSED_VAR          },
    {"unused-struct",           no_argument,        NULL,   OPT_UNUSED_STRUCT       },
    {"skip-llvm-validation",    no_argument,        NULL,   OPT_SKIP_LLVM_VALIDATION},
    {"output",                  required_argument,  NULL,   'o'                     },
    {"verbose",                 no_argument,        NULL,   'v'                     },
    {"lto",                     required_argument,  NULL,   OPT_LTO                 },
    {"release",                 no_argument,        NULL,   OPT_RELEASE             },
    {"release-fast",            no_argument,        NULL,   OPT_RELEASE_FAST        },
    {"release-small",           no_argument,        NULL,   OPT_RELEASE_SMALL       },
    {NULL,                      0,                  NULL,   0                       }
};

ZState *loadState(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return NULL; }

    char *filename = argv[1];
    state = makestate();

    int opt;
    optind = 2;

    while (( opt = getopt_long(argc, argv, "dvo:l:L:O:", long_options, NULL) ) != -1) {
        switch (opt) {
        case 'L': {
            usize len = 3 + strlen(optarg);
            char *lib = znalloc(char, len);
            snprintf(lib, len, "-L%s", optarg);
            lib[len-1] = '\0';
            vecpush(state->extraArgs, lib);
            break;
        }
        case 'l': {
            usize len = 3 + strlen(optarg);
            char *lib = znalloc(char, len);
            snprintf(lib, len, "-l%s", optarg);
            lib[len-1] = '\0';
            vecpush(state->extraArgs, lib);
            break;
        }
        case 'o':   SET_ARG(state->output, "Output file");  break;
        case 'd':
            SET_FLAG(state->debug, "Debug mode");
            state->optimizationLevel = '0';
            break;
        case 'O': {
            char lvl = optarg[0];
            if (optarg[1] != '\0' || ((lvl < '0' || lvl > '3') && lvl != 's' && lvl != 'z')) {
                printf("Error: invalid optimization level '%s'\n", optarg);
                usage(argv[0]);
                return NULL;
            }
            state->optimizationLevel = lvl;
            break;
        }
        case OPT_EMIT:
            if      (strcmp(optarg, "ir")   == 0) state->emit = Z_EMIT_IR;
            else if (strcmp(optarg, "obj")  == 0) state->emit = Z_EMIT_OBJ;
            else if (strcmp(optarg, "asm")  == 0) state->emit = Z_EMIT_ASM;
            else if (strcmp(optarg, "exe")  == 0) state->emit = Z_EMIT_EXE;
            break;
        case 'v':                       SET_FLAG(state->verbose,            "Verbose");                 break;
        case OPT_UNUSED_FUNC:           SET_FLAG(state->unusedFunc,         "Unused function flag");    break;
        case OPT_UNUSED_VAR:            SET_FLAG(state->unusedVar,          "Unused variable flag");    break;
        case OPT_UNUSED_STRUCT:         SET_FLAG(state->unusedStruct,       "Unused struct flag");      break;
        case OPT_SKIP_LLVM_VALIDATION:  SET_FLAG(state->skipLLVMValidation, "Skip llvm validation");    break;
        case OPT_RELEASE:       state->optimizationLevel = '2'; break;
        case OPT_RELEASE_FAST:  state->optimizationLevel = '3'; break;
        case OPT_RELEASE_SMALL: state->optimizationLevel = 's'; break;
        case OPT_LTO:
            if      (strcmp(optarg, "off")  == 0) state->ltoMode = Z_LTO_OFF;
            else if (strcmp(optarg, "thin") == 0) state->ltoMode = Z_LTO_THIN;
            else if (strcmp(optarg, "full") == 0) state->ltoMode = Z_LTO_FULL;
            else {
                printf("Error: invalid lto mode '%s' (expected: off, thin, full)\n", optarg);
                usage(argv[0]);
                return NULL;
            }
            break;
        default: usage(argv[0]); return NULL;
        }
    }

    if (!state->output) {
        char *copy = strdup(filename);

        char *base = basename(copy);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        state->output = base;
    }

    visit(state, &filename, false);

    return state;

}

void handler(int sig) {
    (void)sig;
    void *array[20];
    size_t size;

    size = backtrace(array, 20);
    write(STDERR_FILENO, "Error: signal received\n", 23);
    backtrace_symbols_fd(array, size, STDERR_FILENO);

    if (state && state->debug) printLogs(state);
    allocator.close();
    _exit(1);
}

int pipeline(ZState *state) {
    if (state->verbose) timer_start(&state->phaseTime);

    ZToken **tokens = ztokenize(state);
    if (!tokens) return 1;

    if (!canAdvance(state)) return 2;

    ZNode *root = zparse(state, tokens);

    if (!canAdvance(state)) return 3;
    zanalyze(state, root);

    if (state->debug) printNode(root, 0);

    if (!canAdvance(state)) return 4;

    if (state->verbose) {
        const char *format;
        double elapsed = timer_elapsed(state->phaseTime, &format);
        printf(COLOR_BOLD COLOR_CYAN "  Frontend:   " COLOR_RESET "%.2f%s\n", elapsed, format);
    }

    zcompile(state, root, state->output);

    if (!canAdvance(state)) return 5;

    return 0;
}

int main(int argc, char **argv) {
    signal(SIGSEGV, handler);
    signal(SIGTRAP, handler);
    allocator.open();
    state = loadState(argc, argv);

    if (!state) return 1;

    struct timespec start;
    timer_start(&start);

    int res = pipeline(state);

    printLogs(state);

    allocator.close();

    if (!res) {
        const char *format;
        double elapsed = timer_elapsed(start, &format);
        printf("  " COLOR_BOLD COLOR_GREEN "Total:      " COLOR_RESET
            "%.02f%s\n", elapsed, format);
    }

    return res;
}
