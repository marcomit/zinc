#include "zcli.h"
#include "zcolors.h"

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
    OPT_XLINKER,
    OPT_DUMP_AST,
    OPT_DUMP_TOKENS,
    OPT_NOINJECT
};

#define EMPTY_COMMAND (ZCliCommand){ NULL, NULL, NULL, 0, 0, NULL, NULL, 0 }

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
    {"dump-ast",                no_argument,        NULL,   OPT_DUMP_AST            },
    {"dump-tokens",             no_argument,        NULL,   OPT_DUMP_TOKENS         },
    {"noinject",                no_argument,        NULL,   OPT_NOINJECT            },
    {NULL,                      0,                  NULL,   0                       }
};

static ZErrorCode compile   (ZState *);
static ZErrorCode run       (ZState *);
static ZErrorCode version   (ZState *);
static ZErrorCode help      (ZState *);

static const ZCliCommand ZCommands[] = {
    { "build",      "Compile a source file",    long_options,   1, 1,   compile,    NULL, Z_CMD_NEEDS_INPUT },
    { "run",        "Compile and execute",      long_options,   1, 1,   run,        NULL, 0 },
    { "version",    "Show zinc's version",      NULL,           0, 0,   version,    NULL, 0 },
    { "help",       "Show help for a command",  NULL,           0, 1,   help,       NULL, 0 },
    EMPTY_COMMAND
};

void usage(char *program) {
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
        "\t --dump-ast               Prints the AST of the entire program with all files\n"
        "\nOptimization:\n"
        "\t -O0 -O1 -O2 -O3 -Os -Oz  Set optimization level (default: -O2)\n"
        "\t --release                 Alias for -O2\n"
        "\t --release-fast            Alias for -O3\n"
        "\t --release-small           Alias for -Os\n"
        "\nLink-time optimization:\n"
        "\t --lto=off|thin|full     Set LTO mode (default: off)\n"
        "\t --target                Target triple used by LLVM\n"
        "\t --mcpu                  CPU Target\n"
        "\t --mfeatures             LLVM Features\n"
        "\t --nostdlib              Link freestanding: no libc, CRT, or dynamic linker\n"
        "\t --Xlinker <arg>         Pass <arg> straight through to the linker\n"
        "\t --dump-ast              Prints the AST\n"
        "\t --dump-tokens           Prints the list of tokens\n"
        "\t --noinject              Do not inject the zinc's pre-import\n"
    );
}

static void printAllocation(ZState *state) {
    if (!state->cli.verbose) return;

    double used = arenaLength(state->allocator->ctx);
    double allocated = arenaSize(state->allocator->ctx);
    for (usize i = 0; i < veclen(state->modules); i++) {
        used += arenaLength(state->modules[i]->allocator->ctx);
        allocated += arenaSize(state->modules[i]->allocator->ctx);
    }

    static const char *labels[] = {
        "b",
        "Kb",
        "Mb",
        "Gb",
    };

    static const int labelsize = sizeof(labels) / sizeof(labels[0]);

    int label = 0;
    while (allocated > 1024 && label < labelsize) {
        used = used / 1024;
        allocated = allocated / 1024;
        label++;
    }

    printf("  " COLOR_BOLD COLOR_CYAN "Memory:    " COLOR_RESET " %.1f/%.1f %s\n",
        used, allocated, labels[label]
    );
}

static void initState(ZState *state) {
    char *filename = state->cli.argv[0];
    if (!state->cli.output) {
        char *copy = zstrdup(state->allocator, filename);

        char *base = basename(copy);
        char *dot = strrchr(base, '.');
        if (dot) *dot = '\0';
        state->cli.output = base;
    }

    visit(state, filename);
}

static ZErrorCode pipeline(ZState *state) {
    initState(state);
    if (state->cli.verbose) timer_start(&state->phaseTime);

    ZToken **tokens = ztokenize(state);
    if (!tokens) return Z_LEXICAL_ERROR;
    if (state->cli.dumpTokens) printTokens(tokens);

    if (!initTargetMachine(state)) return Z_CODEGEN_ERROR;
    initPrimitiveTypes(state);

    if (!canAdvance(state)) return Z_LEXICAL_ERROR;

    ZNode *root = zparse(state, tokens);

    if (!canAdvance(state)) return Z_SYNTAX_ERROR;
    zanalyze(state, root);

    if (state->cli.dumpAst) printNode(root, 0);

    if (!canAdvance(state)) return Z_SEMANTIC_ERROR;

    if (state->cli.verbose) {
        const char *format;
        double elapsed = timer_elapsed(state->phaseTime, &format);
        printf(COLOR_BOLD COLOR_CYAN "  Frontend:   " COLOR_RESET "%.2f%s\n", elapsed, format);
    }

    zcompile(state, root, state->cli.output);

    if (!canAdvance(state)) return Z_CODEGEN_ERROR;

    if (state->cli.verbose) printAllocation(state);

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

const ZCliCommand *getCmd(int *argc, char ***argv) {
    const ZCliCommand *res = NULL, *root = ZCommands;
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

bool loadOptions(ZState *state, const ZCliCommand *cmd, int argc, char **argv) {
    int opt;
    /* Leading '+' disables argv permutation (POSIX mode): getopt stops at the
     * first non-option and returns -1 with optind pointing at it. We collect
     * that positional ourselves and resume, so options and positionals work in
     * any order. Unlike the default permuting mode - whose behaviour differs
     * across glibc/BSD/mingw - '+' is honoured consistently everywhere, and it
     * avoids relying on the RETURN_IN_ORDER ('-') mode that mingw lacks. */
    optind = 1;
    while (optind < argc) {
        opt = getopt_long(argc, argv, "+dvo:l:L:O:", cmd->options, NULL);
        if (opt == -1) {
            if (optind < argc) {
                if ((int)veclen(state->cli.argv) < cmd->maxArgs)
                    vecpush(state->cli.argv, argv[optind]);
                optind++;
            }
            continue;
        }
        switch (opt) {
        case 'L': {
            usize len = 3 + strlen(optarg);
            char *lib = znalloc(state->allocator, char, len);
            snprintf(lib, len, "-L%s", optarg);
            lib[len-1] = '\0';
            vecpush(state->cli.extraArgs, lib);
            break;
        }
        case 'l': {
            usize len = 3 + strlen(optarg);
            char *lib = znalloc(state->allocator, char, len);
            snprintf(lib, len, "-l%s", optarg);
            lib[len-1] = '\0';
            vecpush(state->cli.extraArgs, lib);
            break;
        }
        case 'o':   SET_ARG(state->cli.output, "Output file");  break;
        case 'd':
            SET_FLAG(state->cli.debug, "Debug mode");
            state->cli.optimizationLevel = '0';
            state->cli.mode = Z_MODE_DEBUG;
            break;
        case 'O': {
            char lvl = optarg[0];
            if (optarg[1] != '\0' || ((lvl < '0' || lvl > '3') && lvl != 's' && lvl != 'z')) {
                printf("Error: invalid optimization level '%s'\n", optarg);
                usage(argv[0]);
                return NULL;
            }
            state->cli.optimizationLevel = lvl;
            break;
        }
        case OPT_EMIT:
            if      (strcmp(optarg, "ir")   == 0) state->cli.emit = Z_EMIT_IR;
            else if (strcmp(optarg, "obj")  == 0) state->cli.emit = Z_EMIT_OBJ;
            else if (strcmp(optarg, "asm")  == 0) state->cli.emit = Z_EMIT_ASM;
            else if (strcmp(optarg, "exe")  == 0) state->cli.emit = Z_EMIT_EXE;
            break;
        case 'v':                       SET_FLAG(state->cli.verbose,            "Verbose");                 break;
        case OPT_UNUSED_FUNC:           SET_FLAG(state->cli.unusedFunc,         "Unused function flag");    break;
        case OPT_UNUSED_VAR:            SET_FLAG(state->cli.unusedVar,          "Unused variable flag");    break;
        case OPT_UNUSED_STRUCT:         SET_FLAG(state->cli.unusedStruct,       "Unused struct flag");      break;
        case OPT_SKIP_LLVM_VALIDATION:  SET_FLAG(state->cli.skipLLVMValidation, "Skip llvm validation");    break;
        case OPT_NOSTDLIB:              SET_FLAG(state->cli.nostdlib,           "No libc");                 break;
        case OPT_DUMP_AST:              SET_FLAG(state->cli.dumpAst,            "Dump ast");                break;
        case OPT_DUMP_TOKENS:           SET_FLAG(state->cli.dumpTokens,         "Dump tokens");             break;
        case OPT_NOINJECT:              SET_FLAG(state->cli.noInject,           "No Inject");               break;
        case OPT_XLINKER:               vecpush(state->cli.extraArgs, zstrdup(state->allocator, optarg));   break;
        case OPT_RELEASE:               state->cli.optimizationLevel = '2';                                 break;
        case OPT_RELEASE_FAST:          state->cli.optimizationLevel = '3';                                 break;
        case OPT_RELEASE_SMALL:         state->cli.optimizationLevel = 's';                                 break;
        case OPT_TARGET:                state->cli.targetTriple = optarg;                                   break;
        case OPT_MCPU:                  state->cli.targetCPU = optarg;                                      break;
        case OPT_MFEATURES:             state->cli.targetFeatures = optarg;                                 break;
        case OPT_LTO:
            if      (strcmp(optarg, "off")  == 0) state->cli.ltoMode = Z_LTO_OFF;
            else if (strcmp(optarg, "thin") == 0) state->cli.ltoMode = Z_LTO_THIN;
            else if (strcmp(optarg, "full") == 0) state->cli.ltoMode = Z_LTO_FULL;
            else {
                printf("Error: invalid lto mode '%s' (expected: off, thin, full)\n", optarg);
                usage(argv[0]);
                return NULL;
            }
            break;
        default: usage(argv[0]); return false;
        }
    }

    if ((int)veclen(state->cli.argv) < cmd->minArgs) {
        fprintf(
            stderr,
            "Expected at least %d argument(s), got %d\n",
            cmd->minArgs, (int)veclen(state->cli.argv)
        );
        return false;
    }
    return true;
}
