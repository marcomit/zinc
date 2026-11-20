#include "zbuild.h"
#include "zvec.h"

#include <stdlib.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdatomic.h>

#if defined(_WIN32)
#   include <windows.h>
#else
#   include <unistd.h>
#   include <sys/stat.h>
#endif
#ifdef __linux__
#   include <sched.h>
#endif

#define vec(T) T *

typedef struct {

} ZQueue;

struct ZBuild {
    vec(ZCmd *) cmds;
    usize max_jobs;
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
    vec(const char *)   argv;
    vec(const char *)   inputs;
    vec(const char *)   outputs;
    vec(ZCmd *)         after;
    usize               remaining;
    ZCmdState           state;
};

static void zb_cmd_free(ZCmd *);
static int is_out_of_date(ZCmd *);
static char *zb_get_cmd(ZCmd *);

int cpu_count() {
#if defined(_WIN32)
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n > 0 ? (int)n : 1;
#elif defined(__linux__)
    cpu_set_t set;
    if (sched_getaffinity(0, sizeof set, &set) == 0) {
        int n = CPU_COUNT(&set);
        if (n > 0) return n;
    }
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* Create a build. jobs = max commands running at once (0 = number of CPUs). */
ZBuild *zb_new(int jobs) {
    ZBuild *self = (ZBuild *)malloc(sizeof(ZBuild));
    *self = (ZBuild){ 0 };
    self->max_jobs = jobs == 0 ? cpu_count() : jobs;
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
int has_cycle(ZCmd *cmd) {
    if (cmd->state == Z_CMD_DONE) return 0;
    if (cmd->state == Z_CMD_RUNNING) return 1;
    cmd->state = Z_CMD_RUNNING;

    for (usize i = 0; i < veclen(cmd->after); i++)
        if (has_cycle(cmd->after[i]))
            return 1;

    cmd->state = Z_CMD_DONE;
    return 0;
}

int run_cmd(ZCmd *c) {
    if (c->state & Z_CMD_END_MASK) return 0;
    c->state = Z_CMD_RUNNING;

    usize i = 0;
    for (; i < veclen(c->after); i++) {
        if (!run_cmd(c->after[i])) {
            c->state = Z_CMD_SKIPPED;
            break;
        }
    }

    if (c->state == Z_CMD_SKIPPED) for (; i < veclen(c->after); i++)
        c->after[i]->state = Z_CMD_SKIPPED;

    if (c->state != Z_CMD_RUNNING) return 0;
    int code = system(zb_get_cmd(c));
    if (code != 0) {
        c->state = Z_CMD_FAILED;
        return 0;
    } else {
        c->state = Z_CMD_DONE;
        return 1;
    }
}

/* Run every out-of-date command, in parallel where dependencies allow.
 * Returns 0 on success, 1 if a command failed or there is a cycle. */
ZBuildState zb_run(ZBuild *b) {
    for (usize i = 0; i < veclen(b->cmds); i++)
        b->cmds[i]->state = Z_CMD_PENDING;

    for (usize i = 0; i < veclen(b->cmds); i++)
        if (has_cycle(b->cmds[i]))
            return Z_BUILD_CYCLE;

    for (usize i = 0; i < veclen(b->cmds); i++)
        if (!is_out_of_date(b->cmds[i]))
            run_cmd(b->cmds[i]);


    return Z_BUILD_DONE;
}

/* Delete every output file and depfile. */
void zb_clean(ZBuild *b) {

}

/* Free the build and all its commands. */
void zb_free(ZBuild *b) {
    for (usize i = 0; i < veclen(b->cmds); i++)
        zb_cmd_free(b->cmds[i]);
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
static int is_out_of_date(ZCmd *cmd) {
    usize in_len = veclen(cmd->inputs);
    usize out_len = veclen(cmd->outputs);

    if (in_len == 0 || out_len == 0) return 0;

    long long newest_input  = file_mtime(cmd->inputs[0]);
    long long oldest_output = file_mtime(cmd->outputs[0]);

    for (usize i = 1; i < in_len; i++) {
        long long cur = file_mtime(cmd->inputs[i]);
        if (cur > newest_input) cur = newest_input;
    }

    for (usize i = 1; i < out_len; i++) {
        long long cur = file_mtime(cmd->outputs[i]);
        if (cur < oldest_output) cur = oldest_output;
    }

    if (newest_input <= oldest_output) return 1;
    return 0;
}

/* Create a command and add it to the build. `program` becomes argv[0]. */
ZCmd *zb_cmd(ZBuild *b, const char *program) {
    ZCmd *self = (ZCmd *)malloc(sizeof(ZCmd));
    *self = (ZCmd){ 0 };
    vecpush(self->argv, strdup(program));
    return self;
}

static void zb_cmd_free(ZCmd *cmd) {
    #define freelist(l) for (usize i = 0; i < veclen(l); i++) free((void *)l[i]);
    freelist(cmd->argv);
    freelist(cmd->inputs);
    freelist(cmd->outputs);
    #undef freelist

    for (usize i = 0; i < veclen(cmd->after); i++) {
        zb_cmd_free(cmd->after[i]);
        cmd->after[i] = NULL;
    }

    vecfree(cmd->argv);
    vecfree(cmd->inputs);
    vecfree(cmd->outputs);
    vecfree(cmd->after);

    free(cmd);
}

/* Append one argument. All strings are copied. */
void zb_arg(ZCmd *c, const char *arg) {
    vecpush(c->argv, strdup(arg));
}

/* Append several arguments: zb_args(c, "-O2", "-Wall"); */
#define zb_args(c, ...) zb__args((c), __VA_ARGS__, (const char *)NULL)
void zb__args(ZCmd *c, ...) {
    va_list ap;
    va_start(ap, c);

    const char *arg;
    while (( arg = va_arg(ap, const char *)))
        vecpush(c->argv, strdup(arg));

    va_end(ap);
}

static char *vformat(const char *fmt, va_list args) {
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (len < 0) return NULL;

    char *out = (char *)malloc((size_t)len + 1);
    if (out) vsnprintf(out, (size_t)len + 1, fmt, args);
    return out;
}

/* Append ONE argument built printf-style: zb_argf(c, "-DVERSION=%d", 3); */
void zb_argf(ZCmd *c, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    const char *formatted = vformat(fmt, ap);
    va_end(ap);
    zb_arg(c, formatted);
}

/* ---- Files ----------------------------------------------------------- */

/* Append `path` as an argument AND record it as an input / output. */
void zb_in (ZCmd *c, const char *path) {
    zb_arg(c, path);
    zb_input(c, path);
}
void zb_out(ZCmd *c, const char *path) {
    zb_arg(c, path);
    zb_output(c, path);
}

/* Record a file without adding an argument (headers, side outputs...). */
void zb_input (ZCmd *c, const char *path) {
    vecpush(c->outputs, strdup(path));
}

void zb_output(ZCmd *c, const char *path) {
    vecpush(c->outputs, strdup(path));
}

/* A gcc/clang depfile (-MMD -MF path); headers listed in it become inputs. */
void zb_depfile(ZCmd *c, const char *path) {

}

static char *zb_get_cmd(ZCmd *cmd) {
    char *out = NULL;
    for (usize i = 0; i < veclen(cmd->argv); i++) {
        if (i != 0) vecpush(out, ' ');
        usize arglen = strlen(cmd->argv[i]);
        vecunion(out, cmd->argv[i], arglen);
    }
    vecpush(out, '\0');
    return out;
}

/* ---- Ordering -------------------------------------------------------- */

/* Run c after dep. Usually not needed: if c has an input that another
 * command lists as an output, that edge is added automatically. */
void zb_after(ZCmd *c, ZCmd *dep) {
    vecpush(c->after, dep);
}

/* ---- Self rebuild ---------------------------------------------------- */

/* Put at the top of main(): if build.c, zbuild.c or zbuild.h is newer than
 * the running executable, recompile it and restart with the same arguments. */
void zb__rebuild_self(int argc, char **argv, const char *build_src) {

}
