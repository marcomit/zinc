// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zinc.h"
#include "zcli.h"

#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <time.h>

static ZState *state    = NULL;

static void handler(int sig) {
    (void)sig;
    void *array[20];
    size_t size;

    size = backtrace(array, 20);
    write(STDERR_FILENO, "Error: signal received\n", 23);
    backtrace_symbols_fd(array, size, STDERR_FILENO);

    if (state && state->cli.debug) printLogs(state);
    _exit(1);
}

int main(int argc, char **argv) {
#define err(code, ...) { fprintf(stderr, __VA_ARGS__); usage(program); return code; }

    init_allocators();

    char *program = *argv;

    if (argc < 2) err(Z_INVALID_COMMAND, "Invalid argument\n");

    signal(SIGSEGV, handler);
    signal(SIGTRAP, handler);

    state = makestate();

    if (!state) err(Z_INVALID_STATE, "Invalid state\n");

    useAllocator(state->allocator);

    const ZCliCommand *cmd = getCmd(&argc, &argv);

    if (!cmd) err(Z_INVALID_COMMAND, "Invalid command\n");
    if (!loadOptions(state, cmd, argc, argv)) {
        fprintf(stderr, "Invalid option\n");
        return Z_INVALID_COMMAND;
    }

    ZErrorCode res = cmd->callback(state);

    ZState *done = state;
    state = NULL;
    freestate(done);

    return res;
}
