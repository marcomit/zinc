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

#define ZINC_VERSION "0.2.0"

static ZState *state    = NULL;

ZType *none     = NULL;
ZType *u0Type   = NULL;
ZType *charType = NULL;
ZType *u1Type   = NULL;
ZType *u64Type  = NULL;
ZType *modType  = NULL;

typedef enum {
    Z_OK = 0,
    Z_INVALID_COMMAND,
    Z_INVALID_STATE,
    Z_LEXICAL_ERROR,
    Z_SYNTAX_ERROR,
    Z_SEMANTIC_ERROR,
    Z_CODEGEN_ERROR,
} ZErrorCode;

typedef enum {
    Z_CMD_NEEDS_INPUT = 1 << 0x00
} ZCliFlags;

typedef ZErrorCode (*ZCliCallback)(ZState *);
typedef struct ZCliCommand ZCliCommand;
struct ZCliCommand {
    const char      *name;
    const char      *summary;
    struct option   *options;

    int minArgs;
    int maxArgs;

    ZCliCallback    callback;
    ZCliCommand     *subcommand;

    /* The number of arguments this command expect.
     * e.g. the command build needs the file as input. */
    int             argc;
};

static void usage(char *program) {
    printf(
        "Usage: %s <filename> [options]\n%s", program,
        "Options:\n"
        "\t -d --debug               Enable debug mode (sets -O0)\n"
        "\t -v --verbose             Enable verbose mode\n"
        "\t --emit=exe|obj|ir|asm    Select output type (default: exe)\n"
        "\t --unused-variable        Suppress 'unused variable' warnings\n"
        "\t --unused-function        Suppress 'unused function' warnings\n"
        "\t --unused-struct          Suppress 'unused struct' warnings\n"
        "\t --skip-llvm-validation   Does not verify the generated LLVM code\n"
        "\nOptimization:\n"
        "\t -O0 -O1 -O2 -O3 -Os -Oz  Set optimization level (default: -O2)\n"
        "\t --release                 Alias for -O2\n"
        "\t --release-fast            Alias for -O3\n"
        "\t --release-small           Alias for -Os\n"
        "\nLink-time optimization:\n"
        "\t --lto=off|thin|full      Set LTO mode (default: off)\n"
        "\t --target                 Target triple used by LLVM\n"
        "\t --mcpu                   CPU Target\n"
        "\t --mfeatures              LLVM Features\n"
        "\t --nostdlib               Link freestanding: no libc, CRT, or dynamic linker\n"
        "\t --Xlinker <arg>          Pass <arg> straight through to the linker\n"
    );
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
    OPT_RELEASE_SMALL,
    OPT_TARGET,
    OPT_MCPU,
    OPT_MFEATURES,
    OPT_NOSTDLIB,
    OPT_XLINKER
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
    {"target",                  required_argument,  NULL,   OPT_TARGET              },
    {"mcpu",                    required_argument,  NULL,   OPT_MCPU                },
    {"mfeatures",               required_argument,  NULL,   OPT_MFEATURES           },
    {"nostdlib",                no_argument,        NULL,   OPT_NOSTDLIB            },
    {"Xlinker",                 required_argument,  NULL,   OPT_XLINKER             },
    {NULL,                      0,                  NULL,   0                       }
};

void printAllocation(ZState *state) {
    if (!state->verbose) return;

    usize used = arenaLength(allocator.ctx);
    usize allocated = arenaSize(allocator.ctx);
    for (usize i = 0; i < veclen(state->modules); i++) {
        used += arenaLength(state->modules[i]->allocator);
        allocated += arenaSize(state->modules[i]->allocator);
    }

    static const char *labels[] = {
        "b",
        "Kb",
        "Mb",
        "Gb",
    };

    int label = 0;
    while (allocated > 1024 && label < 3) {
        used >>= 10;
        allocated >>= 10;
        label++;
    }

    printf("  " COLOR_BOLD COLOR_CYAN "Memory:    " COLOR_RESET " %zu/%zu %s\n",
        used, allocated, labels[label]
    );
}

static void initState(ZState *state) {
    char *filename = state->argv[0];
    if (!state->output) {
        char *copy = strdup(filename);

        char *base = basename(copy);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        state->output = base;
    }

    visit(state, &filename, false);
}

static ZErrorCode pipeline(ZState *state) {
    initState(state);
    if (state->verbose) timer_start(&state->phaseTime);

    ZToken **tokens = ztokenize(state);
    if (!tokens) return Z_LEXICAL_ERROR;

    if (!initTargetMachine(state)) return Z_CODEGEN_ERROR;
    initPrimitiveTypes();

    if (!canAdvance(state)) return Z_LEXICAL_ERROR;

    ZNode *root = zparse(state, tokens);

    if (!canAdvance(state)) return Z_SYNTAX_ERROR;
    zanalyze(state, root);

    if (state->debug) printNode(root, 0);

    if (!canAdvance(state)) return Z_SEMANTIC_ERROR;

    if (state->verbose) {
        const char *format;
        double elapsed = timer_elapsed(state->phaseTime, &format);
        printf(COLOR_BOLD COLOR_CYAN "  Frontend:   " COLOR_RESET "%.2f%s\n", elapsed, format);
    }

    zcompile(state, root, state->output);

    if (!canAdvance(state)) return Z_CODEGEN_ERROR;

    if (state->verbose) printAllocation(state);

    for (usize i = 0; i < veclen(state->modules); i++) {
        arenaFree(state->modules[i]->allocator);
    }

    return Z_OK;
}

static ZErrorCode run(ZState *state) {
    return pipeline(state);
}

static ZErrorCode compile(ZState *state) {
    struct timespec start;
    timer_start(&start);

    ZErrorCode res = pipeline(state);

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

static ZErrorCode version(ZState *state) {
    (void)state;
    printf("v" ZINC_VERSION "\n");
    return Z_OK;
}

static ZErrorCode help(ZState *state) {
    (void)state;
    usage("");
    return Z_OK;
}

#define EMPTY_COMMAND (ZCliCommand){ NULL, NULL, NULL, 0, 0, NULL, NULL, 0 }

static const ZCliCommand ZCommands[] = {
    { "build",      "Compile a source file",    long_options,   1, 1,   compile,    NULL, Z_CMD_NEEDS_INPUT },
    { "run",        "Compile and execute",      long_options,   1, 1,   run,        NULL, 0 },
    { "version",    "Show zinc's version",      NULL,           0, 0,   version,    NULL, 0 },
    { "help",       "Show help for a command",  NULL,           0, 1,   help,       NULL, 0 },
    EMPTY_COMMAND
};

bool loadOptions(const ZCliCommand *cmd, int argc, char **argv) {
    int opt;
    optind = 1;
    while (( opt = getopt_long(argc, argv, "-dvo:l:L:O:", cmd->options, NULL) ) != -1) {
        switch (opt) {
        case 1:
            if ((int)veclen(state->argv) < cmd->maxArgs)
                vecpush(state->argv, optarg);
            break;
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
            state->mode = Z_MODE_DEBUG;
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
        case OPT_NOSTDLIB:              SET_FLAG(state->nostdlib,           "No libc");                 break;
        case OPT_XLINKER:               vecpush(state->extraArgs, strdup(optarg));                      break;
        case OPT_RELEASE:               state->optimizationLevel = '2';                                 break;
        case OPT_RELEASE_FAST:          state->optimizationLevel = '3';                                 break;
        case OPT_RELEASE_SMALL:         state->optimizationLevel = 's';                                 break;
        case OPT_TARGET:                state->targetTriple = optarg;                                   break;
        case OPT_MCPU:                  state->targetCPU = optarg;                                      break;
        case OPT_MFEATURES:             state->targetFeatures = optarg;                                 break;
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
        default: usage(argv[0]); return false;
        }
    }

    if ((int)veclen(state->argv) < cmd->minArgs) {
        fprintf(
            stderr,
            "Expected at least %d argument(s), got %d\n",
            cmd->minArgs, (int)veclen(state->argv)
        );
        return false;
    }
    return true;
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

static const ZCliCommand *getCmd(const ZCliCommand *root, int *argc, char ***argv) {
    const ZCliCommand *res = NULL;
    while (*argc >= 2) {
        const char *word = (*argv)[1];
        const ZCliCommand *cmd = NULL;
        for (const ZCliCommand *curr = root; curr->name && !cmd; curr++) {
            if (strcmp(curr->name, word) == 0) cmd = curr;
        }
        if (!cmd) return NULL;
        (*argc)--; (*argv)++;

        if (!cmd->subcommand) {
            res = cmd;
            break;
        }
        root = cmd->subcommand;
    }
    if (!res) return NULL;

    return res;
}

int main(int argc, char **argv) {
#define err(code, ...) { fprintf(stderr, __VA_ARGS__); usage(program); return code; }
    char *program = *argv;


    if (argc < 2) err(Z_INVALID_COMMAND, "Invalid argument\n");


    signal(SIGSEGV, handler);
    signal(SIGTRAP, handler);
    allocator.open();

    state = makestate();

    if (!state) err(Z_INVALID_STATE, "Invalid state\n");

    const ZCliCommand *cmd = getCmd(ZCommands, &argc, &argv);

    if (!cmd) err(Z_INVALID_COMMAND, "Invalid command\n");
    if (!loadOptions(cmd, argc, argv)) {
        fprintf(stderr, "Invalid option\n");
        return Z_INVALID_COMMAND;
    }

    return cmd->callback(state);
}
