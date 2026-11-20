/*
 * zbuild.h - a tiny build system for C. Public API.
 *
 * Bootstrap once:  cc -o build build.c zbuild.c
 * Then just run:   ./build
 */
#ifndef ZBUILD_H
#define ZBUILD_H

#include <stddef.h>

typedef struct ZCmd   ZCmd;     /* one command (argv + files + deps) */
typedef struct ZBuild ZBuild;   /* a set of commands                 */

typedef enum {
    Z_BUILD_DONE,
    Z_BUILD_CYCLE,
    Z_BUILD_ERROR
} ZBuildState;

/* ---- Build ----------------------------------------------------------- */

/* Create a build. jobs = max commands running at once (0 = number of CPUs). */
ZBuild *zb_new(int jobs);

/* Run every out-of-date command, in parallel where dependencies allow.
 * Returns 0 on success, 1 if a command failed or there is a cycle. */
ZBuildState zb_run(ZBuild *b);

/* Delete every output file and depfile. */
void zb_clean(ZBuild *b);

/* Free the build and all its commands. */
void zb_free(ZBuild *b);

/* ---- Commands -------------------------------------------------------- */

/* Create a command and add it to the build. `program` becomes argv[0]. */
ZCmd *zb_cmd(ZBuild *b, const char *program);

/* Append one argument. All strings are copied. */
void zb_arg(ZCmd *c, const char *arg);

/* Append several arguments: zb_args(c, "-O2", "-Wall"); */
#define zb_args(c, ...) zb__args((c), __VA_ARGS__, (const char *)NULL)
void zb__args(ZCmd *c, ...);

/* Append ONE argument built printf-style: zb_argf(c, "-DVERSION=%d", 3); */
void zb_argf(ZCmd *c, const char *fmt, ...);

/* ---- Files ----------------------------------------------------------- */

/* Append `path` as an argument AND record it as an input / output. */
void zb_in (ZCmd *c, const char *path);
void zb_out(ZCmd *c, const char *path);

/* Record a file without adding an argument (headers, side outputs...). */
void zb_input (ZCmd *c, const char *path);
void zb_output(ZCmd *c, const char *path);

/* A gcc/clang depfile (-MMD -MF path); headers listed in it become inputs. */
void zb_depfile(ZCmd *c, const char *path);

/* ---- Ordering -------------------------------------------------------- */

/* Run c after dep. Usually not needed: if c has an input that another
 * command lists as an output, that edge is added automatically. */
void zb_after(ZCmd *c, ZCmd *dep);

/* ---- Self rebuild ---------------------------------------------------- */

/* Put at the top of main(): if build.c, zbuild.c or zbuild.h is newer than
 * the running executable, recompile it and restart with the same arguments. */
#define ZB_REBUILD_SELF(argc, argv) zb__rebuild_self((argc), (argv), __FILE__)
void zb__rebuild_self(int argc, char **argv, const char *build_src);

#endif /* ZBUILD_H */
