#ifndef COMPAT_H
#define COMPAT_H

#ifdef _WIN32

#include <windows.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include <io.h>
#include <signal.h>

/* ---- strdup / strndup -------------------------------------------- */
#define strdup _strdup

static inline char *strndup(const char *s, size_t n) {
    size_t len = strnlen(s, n);
    char  *p   = (char *)malloc(len + 1);
    if (!p) return NULL;
    memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

/* ---- clock_gettime / CLOCK_MONOTONIC ----------------------------- */
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

/* MinGW (MSYS2) already provides clock_gettime via pthread_time.h */
#ifndef __MINGW32__
static inline int clock_gettime(int clk_id, struct timespec *tp) {
    (void)clk_id;
    return timespec_get(tp, TIME_UTC) == TIME_UTC ? 0 : -1;
}
#endif

/* ---- dirname / basename ------------------------------------------ */

static inline char *basename(char *path) {
    if (!path || !*path) return (char *)".";
    char *end = path + strlen(path) - 1;
    while (end > path && (*end == '/' || *end == '\\')) end--;
    char *p = end;
    while (p > path && *p != '/' && *p != '\\') p--;
    return (*p == '/' || *p == '\\') ? p + 1 : path;
}

static inline char *dirname(char *path) {
    if (!path || !*path) return (char *)".";
    char *end = path + strlen(path) - 1;
    while (end > path && (*end == '/' || *end == '\\')) *end-- = '\0';
    char *p = end;
    while (p > path && *p != '/' && *p != '\\') p--;
    if (p == path) return (*p == '/' || *p == '\\') ? path : (char *)".";
    *p = '\0';
    return path;
}

/* ---- execinfo stubs ---------------------------------------------- */

static inline int backtrace(void **array, int size) {
    (void)array; (void)size; return 0;
}
static inline void backtrace_symbols_fd(void **array, int size, int fd) {
    (void)array; (void)size; (void)fd;
}

/* ---- POSIX I/O aliases ------------------------------------------- */

#ifndef STDERR_FILENO
#define STDERR_FILENO 2
#endif

static inline int _compat_write(int fd, const void *buf, unsigned n) {
    return _write(fd, buf, n);
}
#define write(fd, buf, n) _compat_write((fd), (buf), (unsigned)(n))

/* ---- signal ------------------------------------------------------ */

#ifndef SIGTRAP
#define SIGTRAP SIGBREAK
#endif

/* ---- getopt_long -------------------------------------------------- */

#define no_argument        0
#define required_argument  1
#define optional_argument  2

struct option {
    const char *name;
    int         has_arg;
    int        *flag;
    int         val;
};

static int  opterr  = 1;
static int  optind  = 1;
static int  optopt  = 0;
static char *optarg = NULL;
static int  _subopt = 0;

static inline int getopt_long(int argc, char *const argv[],
                               const char *optstring,
                               const struct option *longopts,
                               int *longindex) {
    if (optind >= argc) return -1;
    char *arg = argv[optind];
    if (!arg || arg[0] != '-' || arg[1] == '\0') return -1;

    /* "--" ends option parsing */
    if (arg[1] == '-' && arg[2] == '\0') { optind++; return -1; }

    /* Long option */
    if (arg[1] == '-' && _subopt == 0) {
        const char *name = arg + 2;
        const char *eq   = strchr(name, '=');
        size_t       nlen = eq ? (size_t)(eq - name) : strlen(name);
        for (int i = 0; longopts && longopts[i].name; i++) {
            if (strlen(longopts[i].name) == nlen &&
                strncmp(longopts[i].name, name, nlen) == 0) {
                optind++;
                if (longindex) *longindex = i;
                optarg = NULL;
                if (longopts[i].has_arg == required_argument) {
                    if (eq) { optarg = (char *)(eq + 1); }
                    else if (optind < argc) { optarg = argv[optind++]; }
                    else {
                        if (opterr) fprintf(stderr, "%s: option '--%s' requires an argument\n",
                                            argv[0], longopts[i].name);
                        return '?';
                    }
                } else if (longopts[i].has_arg == optional_argument && eq) {
                    optarg = (char *)(eq + 1);
                }
                if (longopts[i].flag) { *longopts[i].flag = longopts[i].val; return 0; }
                return longopts[i].val;
            }
        }
        if (opterr) fprintf(stderr, "%s: unrecognized option '%s'\n", argv[0], arg);
        optind++;
        return '?';
    }

    /* Short option */
    if (_subopt == 0) _subopt = 1;
    int c = (unsigned char)arg[_subopt];
    if (c == '\0') { optind++; _subopt = 0; return getopt_long(argc, argv, optstring, longopts, longindex); }

    const char *p = strchr(optstring, c);
    if (!p) {
        if (opterr) fprintf(stderr, "%s: invalid option '-%c'\n", argv[0], c);
        optopt = c;
        if (arg[++_subopt] == '\0') { optind++; _subopt = 0; }
        return '?';
    }

    if (p[1] == ':') {
        char *rest = arg + _subopt + 1;
        if (*rest) { optarg = rest; }
        else if (optind + 1 < argc) { optarg = argv[++optind]; }
        else {
            if (opterr) fprintf(stderr, "%s: option '-%c' requires an argument\n", argv[0], c);
            optind++; _subopt = 0;
            return '?';
        }
        optind++; _subopt = 0;
    } else {
        optarg = NULL;
        if (arg[++_subopt] == '\0') { optind++; _subopt = 0; }
    }
    return c;
}

#else /* POSIX */

#include <getopt.h>
#include <execinfo.h>
#include <libgen.h>
#include <unistd.h>

#endif /* _WIN32 */
#endif /* COMPAT_H */
