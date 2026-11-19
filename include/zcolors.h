// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#ifndef Z_COLORS_H
#define Z_COLORS_H

#include "zinc.h"

#define COLOR_RESET(s)  ((s)->cli.noColor ? "" : "\033[0m")
#define COLOR_RED(s)    ((s)->cli.noColor ? "" : "\033[1;31m")
#define COLOR_GREEN(s)  ((s)->cli.noColor ? "" : "\033[1;32m")
#define COLOR_YELLOW(s) ((s)->cli.noColor ? "" : "\033[1;33m")
#define COLOR_BLUE(s)   ((s)->cli.noColor ? "" : "\033[1;34m")
#define COLOR_CYAN(s)   ((s)->cli.noColor ? "" : "\033[1;36m")
#define COLOR_BOLD(s)   ((s)->cli.noColor ? "" : "\033[1m")

#endif // Z_COLORS_H
