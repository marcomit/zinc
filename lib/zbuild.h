/**
 * @file zbuild.h
 * @brief A tiny build system for C. Public API.
 *
 * Bootstrap once:  cc -o build build.c
 * Then just run:   ./build
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ZBUILD_H
#define ZBUILD_H

#include <stddef.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

#if defined(_WIN32)
#   include <windows.h>
#   include <direct.h>
#   define MKDIR(path) _mkdir(path)
#   define popen _popen
#   define pclose _pclose
#else
#   include <dirent.h>
#   include <unistd.h>
#   include <sys/wait.h>
#   define MKDIR(path) mkdir(path, 0755)
#endif

#ifdef _WIN32
typedef HANDLE ZProc;
#else
typedef pid_t  ZProc;
#endif

typedef struct ZCmd   ZCmd;     /* one command (argv + files + deps) */
typedef struct ZBuild ZBuild;   /* a set of commands                 */

typedef enum {
    Z_BUILD_DONE,
    Z_BUILD_CYCLE,
    Z_BUILD_NOOP,
    Z_BUILD_GENERIC_ERROR,
} ZBuildState;

struct ZBuild {
    vec(ZCmd *) cmds;
    int max_jobs;
    bool show_command;
    bool show_output;
    bool show_skipped_command;
};

#define Z_CMD_END_MASK (1 << 0x8)

typedef enum {
    Z_CMD_PENDING,
    Z_CMD_RUNNING,
    Z_CMD_DONE      = Z_CMD_END_MASK,
    Z_CMD_FAILED,
    Z_CMD_SKIPPED
} ZCmdState;

struct ZCmd {
    vec(zstr)   argv;
    vec(zstr)   inputs;
    vec(zstr)   outputs;
    vec(ZCmd *) after;
    size_t      remaining;
    ZCmdState   state;
    ZProc       proc;
};

static inline void zb_cmd_free(ZCmd *);
static inline int zb_is_out_of_date(ZCmd *);
static inline char *zb_get_cmd(ZCmd *);
static inline const char *zb_mkdir(const char *);

static inline int cpu_count() {
#if defined(_WIN32)
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* Create a build. jobs = max commands running at once (0 = number of CPUs). */
static inline ZBuild *zb_new(int jobs) {
    ZBuild *self = (ZBuild *)malloc(sizeof(ZBuild));
    *self = (ZBuild){ 0 };
    self->max_jobs = jobs <= 0 ? cpu_count() : jobs;
#ifdef _WIN32
    if (self->max_jobs > MAXIMUM_WAIT_OBJECTS) self->max_jobs = MAXIMUM_WAIT_OBJECTS;
#endif
    return self;
}

/*
 * It checks whether the dependency-graph has a cycle or not.
 * Checked by the topological sort.
 * Tipically the TS needs a `color` field of three variants (white, gray, blakc)
 * but here the command has the state which can be used as a color:
 * PENDING  = white -> not yet visited
 * RUNNING  = gray  -> visiting
 * DONE     = black -> visited
 * */
static inline int zb_has_cycle(ZCmd *cmd) {
    if (cmd->state == Z_CMD_DONE) return 0;
    if (cmd->state == Z_CMD_RUNNING) return 1;
    cmd->state = Z_CMD_RUNNING;

    for (size_t i = 0; i < veclen(cmd->after); i++)
        if (zb_has_cycle(cmd->after[i]))
            return 1;

    cmd->state = Z_CMD_DONE;
    return 0;
}

static inline void zb_set_all_pendings(ZBuild *b) {
    for (size_t i = 0; i < veclen(b->cmds); i++)
        b->cmds[i]->state = Z_CMD_PENDING;
}

static inline int zb__deps_ready(ZCmd *c) {
    for (size_t i = 0; i < veclen(c->after); i++) switch (c->after[i]->state) {
        case Z_CMD_FAILED:
        case Z_CMD_SKIPPED: return -1;
        case Z_CMD_DONE:    break;
        default:            return 0;
    }
    return 1;
}

static inline void zb__print_and_mkdir(ZBuild *b, ZCmd *c) {
    if (b->show_command) {
        char *s = zb_get_cmd(c);
        printf("[zb]: %s\n", s);
        vecfree(s);
    }

    for (size_t i = 0; i < veclen(c->outputs); i++) {
        const char *o = c->outputs[i];
        const char *fwd = strrchr(o, '/');
        const char *back = strrchr(o, '\\');
        const char *slash = !back || (fwd && fwd > back) ? fwd : back;
        if (!slash) continue;
        zstr dir = zs_newn(o, slash - o);
        zb_mkdir(dir);
        zs_free(dir);
    }
}

#ifdef _WIN32
static inline void zb__win_quote(char **line, const char *arg) {
    if (*arg && !strpbrk(arg, " \t\n\v\"")) {
        vecunion(*line, arg, strlen(arg));
        return;
    }
    vecpush(*line, '"');
    for (const char *p = arg;; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (!*p) {
            for (size_t i = 0; i < bs * 2; i++) vecpush(*line, '\\');
            break;
        }
        if (*p == '"') {
            for (size_t i = 0; i < bs * 2 + 1; i++) vecpush(*line, '\\');
        } else {
            for (size_t i = 0; i < bs; i++) vecpush(*line, '\\');
        }
        vecpush(*line, *p);
    }
    vecpush(*line, '"');
}
#endif

static inline bool zb__spawn(ZCmd *c) {
    fflush(stdout);
    fflush(stderr);
#ifdef _WIN32
    char *line = NULL;
    for (size_t i = 0; i < veclen(c->argv); i++) {
        if (i) vecpush(line, ' ');
        zb__win_quote(&line, c->argv[i]);
    }
    vecpush(line, '\0');

    STARTUPINFOA si = { .cb = sizeof si };
    PROCESS_INFORMATION pi;
    BOOL ok = CreateProcessA(NULL, line, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi);
    vecfree(line);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    c->proc = pi.hProcess;
    return true;
#else
    size_t n = veclen(c->argv);
    char **argv = (char **)malloc(sizeof(char *) * (n + 1));
    for (size_t i = 0; i < n; i++) argv[i] = c->argv[i];
    argv[n] = NULL;

    pid_t p = fork();
    if (p == 0) {
        execvp(argv[0], argv);
        perror(argv[0]);
        _exit(127);
    }
    free(argv);
    if (p < 0) return false;
    c->proc = p;
    return true;
#endif
}

static inline ZCmd *zb__wait_any(ZBuild *b, bool *ok) {
#ifdef _WIN32
    HANDLE hs[MAXIMUM_WAIT_OBJECTS];
    ZCmd  *cs[MAXIMUM_WAIT_OBJECTS];
    DWORD n = 0;
    for (size_t i = 0; i < veclen(b->cmds) && n < MAXIMUM_WAIT_OBJECTS; i++)
        if (b->cmds[i]->state == Z_CMD_RUNNING) {
            hs[n] = b->cmds[i]->proc;
            cs[n++] = b->cmds[i];
        }

    DWORD r = WaitForMultipleObjects(n, hs, FALSE, INFINITE);
    if (r >= WAIT_OBJECT_0 + n) return NULL;

    ZCmd *c = cs[r - WAIT_OBJECT_0];
    DWORD code = 1;
    GetExitCodeProcess(c->proc, &code);
    CloseHandle(c->proc);
    *ok = code == 0;
    return c;
#else
    for (;;) {
        int status;
        pid_t p = waitpid(-1, &status, 0);
        if (p < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        for (size_t i = 0; i < veclen(b->cmds); i++) {
            ZCmd *c = b->cmds[i];
            if (c->state == Z_CMD_RUNNING && c->proc == p) {
                *ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
                return c;
            }
        }
    }
#endif
}

static inline ZBuildState zb_run(ZBuild *b) {
    zb_set_all_pendings(b);

    for (size_t i = 0; i < veclen(b->cmds); i++)
        if (zb_has_cycle(b->cmds[i]))
            return Z_BUILD_CYCLE;

    zb_set_all_pendings(b);

    int running = 0;
    bool failed = false, did_work = false;
    for (;;) {
        bool progress = true;
        while (progress) {
            progress = false;
            for (size_t i = 0; i < veclen(b->cmds); i++) {
                ZCmd *c = b->cmds[i];
                if (c->state != Z_CMD_PENDING) continue;

                int r = zb__deps_ready(c);
                if (r < 0) {
                    c->state = Z_CMD_SKIPPED;
                    progress = true;
                    continue;
                } else if (r == 0) continue;

                if (!zb_is_out_of_date(c)) {
                    c->state = Z_CMD_DONE;
                    progress = true;
                    continue;
                }

                if (failed || running >= b->max_jobs) continue;

                zb__print_and_mkdir(b, c);
                if (!zb__spawn(c)) {
                    c->state = Z_CMD_FAILED;
                    failed = true;
                    continue;
                }

                c->state = Z_CMD_RUNNING;
                running++;
                did_work = true;
            }
        }

        if (running == 0) break;

        bool ok = false;
        ZCmd *c = zb__wait_any(b, &ok);
        if (!c) break;
        running--;
        c->state = ok ? Z_CMD_DONE : Z_CMD_FAILED;
        if (!ok) failed = true;
    }

    if (failed) return Z_BUILD_GENERIC_ERROR;
    return did_work ? Z_BUILD_DONE : Z_BUILD_NOOP;
}

/* Delete every output file and depfile. */
static inline void zb_clean(ZBuild *b) {
    for (size_t i = 0; i < veclen(b->cmds); i++) {
         ZCmd *c = b->cmds[i];
         for (size_t j = 0; j < veclen(c->outputs); j++) {
            printf("[zb]: Removing %s\n", c->outputs[j]);
            remove(c->outputs[j]);
        }
    }
}

/* Free the build and all its commands. */
static inline void zb_free(ZBuild *b) {
    for (size_t i = 0; i < veclen(b->cmds); i++)
        zb_cmd_free(b->cmds[i]);
    vecfree(b->cmds);
    free(b);
}

/* ---- Commands -------------------------------------------------------- */

// Last modification time in nanoseconds since 1970-01-01, or -1 if the file doesn't exist.
static inline long long file_mtime(const char *path) {
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &d)) return -1;
    // FILETIME = 100 ns ticks since 1601-01-01
    long long t = ((long long)d.ftLastWriteTime.dwHighDateTime << 32)
                | d.ftLastWriteTime.dwLowDateTime;
    return (t - 116444736000000000LL) * 100;   // shift to 1970, convert to ns
#else
    struct stat st;
    if (stat(path, &st) != 0) return -1;
#if defined(__APPLE__)
    return (long long)st.st_mtimespec.tv_sec * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
    return (long long)st.st_mtim.tv_sec * 1000000000LL + st.st_mtim.tv_nsec;
#endif
#endif
}

/* Checks if the command is out-of-date.
 * A command is out of date if there's an input file
 * which is newer than any output:
 * conceptually: newest input < oldest input
 * */
static inline int zb_is_out_of_date(ZCmd *cmd) {
    size_t in_len = veclen(cmd->inputs);
    size_t out_len = veclen(cmd->outputs);

    if (in_len == 0 || out_len == 0) return 1;

    long long newest_input  = file_mtime(cmd->inputs[0]);
    long long oldest_output = file_mtime(cmd->outputs[0]);

    if (newest_input == -1 || oldest_output == -1)
        return 1;

    for (size_t i = 1; i < in_len; i++) {
        long long cur = file_mtime(cmd->inputs[i]);
        if (cur == -1) return 1;
        if (cur > newest_input) newest_input = cur;
    }

    for (size_t i = 1; i < out_len; i++) {
        long long cur = file_mtime(cmd->outputs[i]);
        if (cur == -1) return 1;
        if (cur < oldest_output) oldest_output = cur;
    }

    if (newest_input <= oldest_output) return 0;
    return 1;
}

/* Create a command and add it to the build. `program` becomes argv[0]. */
static inline ZCmd *zb_cmd(ZBuild *b, const char *program) {
    ZCmd *self = (ZCmd *)malloc(sizeof(ZCmd));
    *self = (ZCmd){ 0 };
    vecpush(self->argv, zs_new(program));
    vecpush(b->cmds, self);
    return self;
}

/* Get the outputs of a command. */
static inline vec(zstr) zb_cmd_outputs(ZCmd *c) {
    return c->outputs;
}

static inline vec(zstr) zm_cmd_inputs(ZCmd *c) {
    return c->inputs;
}

static inline void zb_cmd_free(ZCmd *cmd) {
    if (!cmd) return;

    zs_free_all(cmd->argv);
    zs_free_all(cmd->inputs);
    zs_free_all(cmd->outputs);

    vecfree(cmd->after);
    free(cmd);
}

#define zb_args(c, ...) _zb_args(c, __VA_ARGS__, NULL)
static inline void _zb_args(ZCmd *c, ...) {
    va_list ap;
    const char *arg;
    va_start(ap, c);

    while (( arg = va_arg(ap, const char *)))
        vecpush(c->argv, zs_new(arg));

    va_end(ap);
}

static inline void zb_argsv(ZCmd *c, vec(const char *) args) {
    for (size_t i = 0; i < veclen(args); i++)
        vecpush(c->argv, zs_new(args[i]));
}

/* Append ONE argument built printf-style: zb_argf(c, "-DVERSION=%d", 3); */
static inline void zb_arg(ZCmd *c, const char *fmt, ...) {
    if (!fmt) return;
    va_list ap;
    va_start(ap, fmt);

    vecpush(c->argv, zs_vfmt(fmt, ap));
    va_end(ap);
}

/* ---- Files ----------------------------------------------------------- */

/* Record a file without adding an argument (headers, side outputs...). */
static inline void zb_input (ZCmd *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vecpush(c->inputs, zs_vfmt(fmt, ap));
    va_end(ap);
}

static inline void zb_output(ZCmd *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    vecpush(c->outputs, zs_vfmt(fmt, ap));
    va_end(ap);
}

/* Append `path` as an argument AND record it as an input / output. */
static inline void zb_in(ZCmd *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    zstr s = zs_vfmt(fmt, ap);
    va_end(ap);
    vecpush(c->argv, s);
    vecpush(c->inputs, zs_dup(s));
}

static inline void zb_out(ZCmd *c, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    zstr s = zs_vfmt(fmt, ap);
    va_end(ap);
    vecpush(c->argv, s);
    vecpush(c->outputs, zs_dup(s));
}

/* A gcc/clang depfile (-MMD -MF path); headers listed in it become inputs. */
static inline void zb_depfile(ZCmd *c, const char *path) {
    (void)c;
    (void)path;
}

static inline char *zb_get_cmd(ZCmd *cmd) {
    char *out = NULL;
    for (size_t i = 0; i < veclen(cmd->argv); i++) {
        if (i != 0) vecpush(out, ' ');
        size_t arglen = strlen(cmd->argv[i]);
        vecunion(out, cmd->argv[i], arglen);
    }
    vecpush(out, '\0');
    return out;
}

/* ---- Ordering -------------------------------------------------------- */

/* Run c after dep. Usually not needed: if c has an input that another
 * command lists as an output, that edge is added automatically. */
static inline void zb_after(ZCmd *c, ZCmd *dep) {
    vecpush(c->after, dep);
}

/* ---- Utilities ------------------------------------------------------- */

static inline int zb_shell(zstr *out, const char *cmd) {
    char line[512];
    FILE *f = popen(cmd, "r");

    if (!f) return -1;

    if (out) while (fgets(line, sizeof line, f))
        zs_catn(out, line, strlen(line));

    int status = pclose(f);
#ifdef _WIN32
    return status;
#else
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return -1;
#endif
}

#ifdef _WIN32
#   define STAT_STRUCT struct _stat
#   define STAT_FUNC _stat
#   define IS_DIR(m) (((m) & _S_IFMT) == _S_IFDIR)
#else
#   define STAT_STRUCT struct stat
#   define STAT_FUNC stat
#   define IS_DIR(m) S_ISDIR(m)
#endif

static inline bool zb_isdir(const char *path) {
    STAT_STRUCT info;
    if (STAT_FUNC(path, &info) != 0) {
        return false;
    }
    return IS_DIR(info.st_mode);

}

static inline bool zb_exists(const char *path) {
    STAT_STRUCT info;
    return STAT_FUNC(path, &info) == 0;
}

#undef STAT_FUNC
#undef STAT_STRUCT
#undef IS_DIR

static inline const char *zb_mkdir(const char *path) {
    if (!path || !*path) return path;

    zstr copied = zs_new(path);

    const char *s = copied;
    while (true) {
        char *slash = strpbrk(s, "/\\");
        if (!slash) break;

        char orig = *slash;
        *slash = '\0';

        bool is_drive = (slash - copied == 2 && copied[1] == ':');
        if (*copied != '\0' && !is_drive && !zb_exists(copied)) {
            MKDIR(copied);
        }

        *slash = orig;
        s = slash + 1;
    }

    if (*copied != '\0' && !zb_exists(copied)) {
        MKDIR(copied);
    }

    zs_free(copied);
    return path;
}

static inline bool zb_ls(const char *path, vec(zstr) *entries) {
#ifdef _WIN32
    char search_path[MAX_PATH];
    snprintf(search_path, sizeof(search_path), "%s/*", path);

    WIN32_FIND_DATAA data;
    HANDLE hFind = FindFirstFileA(search_path, &data);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    do {
        if (strcmp(data.cFileName, ".") == 0 ||
            strcmp(data.cFileName, "..") == 0)
            continue;

        vecpush(*entries, zs_fmt("%s/%s", path, data.cFileName));
    } while (FindNextFileA(hFind, &data));

    FindClose(hFind);
#else
    DIR *d = opendir(path);
    if (!d) return false;

    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0)
            continue;
        vecpush(*entries, zs_fmt("%s/%s", path, entry->d_name));
    }

    closedir(d);
#endif
    return true;
}


typedef struct {
    const char **suffix;
    const char **skip;
    bool (*fn)(const char *, void *);
    void *ctx;
} ZBWalkFilter;

#define ZB_LIST(...) ((const char *[]){ __VA_ARGS__, NULL })

static inline bool zb__filter_skip(const char *p, ZBWalkFilter filter) {
    if (!filter.skip) return false;
    const char **skip = filter.skip;
    while (*skip) {
        if (strstr(p, *skip)) return true;
        skip++;
    }
    return false;
}

static inline bool zb__filter_suffix(const char *p, ZBWalkFilter filter) {
    if (!filter.suffix) return true;
    const char **suffix = filter.suffix;
    while (*suffix) {
        if (zs_ends(p, *suffix)) return true;
        suffix++;
    }
    return false;
}

static vec(zstr) zb_walk(const char *root, ZBWalkFilter filter) {
    vec(zstr) entries   = NULL;
    vec(zstr) out       = NULL;

    vecpush(entries, zs_new(root));
    for (size_t i = 0; i < veclen(entries); i++) {
        const char *p = entries[i];

        if (zb__filter_skip(p, filter)) continue;

        if (zb_isdir(p)) {
            zb_ls(p, &entries);
            continue;
        }

        if (!zb__filter_suffix(p, filter)) continue;
        if (filter.fn && !filter.fn(p, filter.ctx)) continue;

        vecpush(out, zs_new(p));
    }

    zs_free_all(entries);
    return out;
}

static inline zstr zb_path_map(const char *path,
                                const char *from,
                                const char *to,
                                const char *ext) {
    size_t from_len = strlen(from);
    if (strncmp(path, from, from_len) != 0) return NULL;

    const char *stem = path + from_len;
    const char *slash = strrchr(stem, '/');
    const char *dot = strrchr(slash ? slash : stem, '.');
    size_t stem_len = dot ? (size_t)(dot - stem) : strlen(stem);

    return zs_fmt("%s%.*s%s", to, (int)stem_len, stem, ext);
}

/* ---- Self rebuild ---------------------------------------------------- */

/* Put at the top of main(): if build.c, zbuild.c or zbuild.h is newer than
 * the running executable, recompile it and restart with the same arguments. */
#define ZB_REBUILD_SELF(argc, argv) zb__rebuild_self((argc), (argv), __FILE__)
static inline void zb__rebuild_self(int argc, char **argv, const char *build_src) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    (void)build_src;
#else
    (void)argc;
    ZBuild *b = zb_new(0);
    b->show_command = true;

    ZCmd *c = zb_cmd(b, "clang");
    zb_args(c, "-O2", "-Wall", "-Wextra");
    zb_in(c, build_src);
    zb_arg(c, "-o");
    zb_out(c, argv[0]);

    ZBuildState state = zb_run(b);

    if (state == Z_BUILD_NOOP) return;
    else printf("Rebuilding...\n");

    if (state != Z_BUILD_DONE) exit(1);

    execvp(argv[0], argv);
    perror("execv");
    exit(1);
#endif
}

#endif /* ZBUILD_H */
