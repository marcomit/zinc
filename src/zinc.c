// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zinc.h"
#include "base.h"
#include "zcli.h"

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

static ZState *state    = NULL;

ZType *none     = NULL;
ZType *u0Type   = NULL;
ZType *charType = NULL;
ZType *u1Type   = NULL;
ZType *u64Type  = NULL;
ZType *modType  = NULL;

static void handler(int sig) {
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

int main(int argc, char **argv) {
#define err(code, ...) { fprintf(stderr, __VA_ARGS__); usage(program); return code; }

    char *program = *argv;

    if (argc < 2) err(Z_INVALID_COMMAND, "Invalid argument\n");

    signal(SIGSEGV, handler);
    signal(SIGTRAP, handler);
    allocator.open();

    state = makestate();

    if (!state) err(Z_INVALID_STATE, "Invalid state\n");

    const ZCliCommand *cmd = getCmd(&argc, &argv);

    if (!cmd) err(Z_INVALID_COMMAND, "Invalid command\n");
    if (!loadOptions(state, cmd, argc, argv)) {
        fprintf(stderr, "Invalid option\n");
        return Z_INVALID_COMMAND;
    }

    return cmd->callback(state);
}
