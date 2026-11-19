#include "lib/zvec.h"
#include "lib/zstr.h"
#include "lib/zbuild.h"
#include <ctype.h>

#define LLVM_VERSION "22"

#ifdef _WIN32
#   define NULLDEV "2>nul"
#else
#   define NULLDEV "2>/dev/null"
#endif

typedef void (*CommandFn)(ZBuild *, int , char *[]);
typedef struct {
    const char *name;
    CommandFn fn;
} Command;

static const char *TARGET               = "zinc";
static const char *ZMODE_FLAGS          = "-DZMODE=ZMODE_COMPILER";
static const char *SANITIZE             = NULL;
static const char *OBJ_DIR              = ".build/default/";
static vec(const char *) CFLAGS         = NULL;
static vec(const char *) CPPFLAGS       = NULL;
static vec(const char *) INCLUDES       = NULL;
static vec(const char *) WIN_DEFS       = NULL;
static vec(const char *) LLD_LIBS       = NULL;
static vec(const char *) LLD_INCLUDES   = NULL;
static vec(const char *) LLVM_CFLAGS    = NULL;
static vec(const char *) LLVM_LDFLAGS   = NULL;
static vec(const char *) LSP_LDFLAGS    = NULL;
static zstr LLVM_CONFIG_PATH            = NULL;
static zstr LLD_PREFIX                  = NULL;

static void build_link(ZBuild *b, vec(ZCmd *) cmds) {
    ZCmd *l = zb_cmd(b, "clang++");
    zb_arg(l, "-o");
    zb_out(l, "%s", TARGET);

    for (size_t i = 0; i < veclen(cmds); i++) {
        ZCmd *c = cmds[i];
        zb_after(l, c);
        for (size_t j = 0; j < veclen(c->outputs); j++) {
            zb_in(l, "%s", c->outputs[j]);
        }
    }

    zb_argsv(l, LLD_LIBS);
    zb_argsv(l, LLVM_LDFLAGS);
    if (SANITIZE) zb_arg(l, "%s", SANITIZE);
}

static ZCmd *build_cpp(ZBuild *b, const char *input) {
    zstr output = zb_path_map(
        input,
        zs_starts(input, "lib/") ? "lib/" : "src/",
        OBJ_DIR, ".o"
    );
    ZCmd *c = zb_cmd(b, "clang++");
    zb_argsv(c, CPPFLAGS);
    zb_argsv(c, INCLUDES);
    zb_argsv(c, WIN_DEFS);
    zb_argsv(c, LLVM_CFLAGS);
    zb_argsv(c, LLD_INCLUDES);
    zb_arg  (c, "%s", ZMODE_FLAGS);
    zb_arg  (c, "-c");
    zb_in   (c, "%s", input);
    zb_arg  (c, "-o");
    zb_out  (c, "%s", output);
    if (SANITIZE) zb_arg  (c, "%s", SANITIZE);

    zs_free(output);
    return c;
}

static ZCmd *build_c(ZBuild *b, const char *input) {
    zstr output = zb_path_map(
        input,
        zs_starts(input, "lib/") ? "lib/" : "src/",
        OBJ_DIR, ".o"
    );

    ZCmd *c = zb_cmd(b, "clang");
    zb_argsv(c, CFLAGS);
    zb_argsv(c, INCLUDES);
    zb_argsv(c, WIN_DEFS);
    zb_argsv(c, LLVM_CFLAGS);
    zb_argsv(c, LLD_INCLUDES);
    zb_arg  (c, "%s", ZMODE_FLAGS);
    zb_arg  (c, "-c");
    zb_in   (c, "%s", input);
    zb_arg  (c, "-o");
    zb_out  (c, "%s", output);
    if (SANITIZE) zb_arg(c, "%s", SANITIZE);

    zs_free(output);
    return c;
}

static void build_all(ZBuild *b, int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    vec(zstr) c_files = zb_walk("src", (ZBWalkFilter){
        .skip = ZB_LIST("src/lsp"),
        .suffix = ZB_LIST(".c", ".cpp")
    });

    vec(zstr) lib_files = zb_walk("lib", (ZBWalkFilter){
        .skip = ZB_LIST("lib/json.c"),
        .suffix = ZB_LIST(".c")
    });

    // not optimal, every header change will trigger a full rebuild
    vec(zstr) hdr_files = zb_walk(".", (ZBWalkFilter){
        .skip = ZB_LIST(".git", ".github", ".build", "tests", "std", "examples", "docs"),
        .suffix = ZB_LIST(".h", ".def")
    });

    vecunion(c_files, lib_files, veclen(lib_files));
    vecfree(lib_files);

    vec(ZCmd *) outs = NULL;
    for (size_t i = 0; i < veclen(c_files); i++) {
        ZCmd *c = zs_ends(c_files[i], ".cpp") ?
            build_cpp(b, c_files[i]) : build_c(b, c_files[i]);

        vecpush(outs, c);
        vecunion(c->inputs, hdr_files, veclen(hdr_files));
        zb_inputs(c, hdr_files);
    }

    build_link(b, outs);
    vecfree(outs);
    zs_free_all(c_files);
}

static void build_lsp(ZBuild *b, int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    TARGET              = "zinc-lsp";
    ZMODE_FLAGS         = "-DZMODE=ZMODE_LSP";
    OBJ_DIR             = ".build/lsp/";
    vec(zstr) c_files   = zb_walk("src", (ZBWalkFilter){
        .skip = ZB_LIST("src/codegen", "src/zinc.c", "src/zcli.c"),
        .suffix = ZB_LIST(".c")
    });
    vec(zstr) lib_files = zb_walk("lib", (ZBWalkFilter){
        .suffix = ZB_LIST(".c")
    });

    vecunion(c_files, lib_files, veclen(lib_files));
    vecfree(lib_files);

    ZCmd *l = zb_cmd(b, "clang");
    zb_arg(l, "-o");
    zb_out(l, "%s", TARGET);

    for (size_t i = 0; i < veclen(c_files); i++) {
        ZCmd *c = build_c(b, c_files[i]);
        zb_after(l, c);
        for (size_t j = 0; j < veclen(c->outputs); j++)
            zb_in(l, "%s", c->outputs[j]);
    }

    zb_argsv(l, LSP_LDFLAGS);
    if (SANITIZE) zb_arg(l, "%s", SANITIZE);
    zs_free_all(c_files);
}

static void build_clean(ZBuild *b, int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    ZCmd *rm_bin = zb_cmd(b, "rm");
    zb_args(rm_bin, "-f", "zinc", "zinc-lsp", "zinc-asan", "zinc-ubsan", "zinc-ubsan-int");

    ZCmd *rm_build = zb_cmd(b, "rm");
    zb_args(rm_build, "-rf", ".build");
}

static void build_debug(ZBuild *b, int argc, char *argv[]) {
    SANITIZE    = "-fsanitize=address,undefined";
    TARGET      = "zinc-debug";
    OBJ_DIR     = ".build/debug/";
    build_all(b, argc, argv);
}

static void build_asan(ZBuild *b, int argc, char *argv[]) {
    SANITIZE    = "-fsanitize=address,undefined";
    TARGET      = "zinc-asan";
    OBJ_DIR     = ".build/asan/";
    build_all(b, argc, argv);
}

static void build_ubsan(ZBuild *b, int argc, char *argv[]) {
    SANITIZE    = "-fsanitize=undefined,local-bounds";
    TARGET      = "zinc-ubsan";
    OBJ_DIR     = ".build/ubsan/";
    build_all(b, argc, argv);
}

static void build_ubsan_int(ZBuild *b, int argc, char *argv[]) {
    SANITIZE    = "-fsanitize=undefined,integer,implicit-conversion,local-bounds";
    TARGET      = "zinc-ubsan-int";
    OBJ_DIR     = ".build/ubsan-int/";
    build_all(b, argc, argv);
}

static void build_install(ZBuild *b, int argc, char *argv[]) {
    (void)b;
    (void)argc;
    (void)argv;
    char cmd[1024];
#ifdef _WIN32
    const char *home_var = "USERPROFILE";
    const char *home = getenv(home_var);
    if (!home) {
        printf("Error: %s not found\n", home_var);
        return;
    }
    snprintf(cmd, sizeof(cmd),
        "rmdir /s /q \"%s\\.zinc\\packages\\std\" 2>nul & "
        "xcopy /e /i /y /q \"std\" \"%s\\.zinc\\packages\\std\" >nul",
        home, home);
#else
    const char *home_var = "HOME";
    const char *home = getenv(home_var);
    if (!home) {
        printf("Error: %s not found\n", home_var);
        return;
    }
    snprintf(cmd, sizeof(cmd),
        "rm -rf \"%s/.zinc/packages/std\" && "
        "mkdir -p \"%s/.zinc/packages/std\" && "
        "cp -R std/. \"%s/.zinc/packages/std/\"",
        home, home, home);
#endif

    if (system(cmd) != 0) return;
#ifdef _WIN32
    printf("installed stdlib -> %s\\.zinc\\packages\\std\n", home);
#else
    printf("installed stdlib -> %s/.zinc/packages/std\n", home);
#endif
}

static void build_test(ZBuild *b, int argc, char *argv[]) {
    (void)argc;
    (void)argv;
    zb_cmd(b, "./run_tests.sh");
}

static const Command Commands[] = {
    { "all",        build_all       },
    { "clean",      build_clean     },
    { "lsp",        build_lsp       },
    { "debug",      build_debug     },
    { "asan",       build_asan      },
    { "ubsan",      build_ubsan     },
    { "ubsan-int",  build_ubsan_int },
    { "install",    build_install   },
    { "test",       build_test      },
    { NULL,         NULL            },
};

static CommandFn get_cmd(int argc, char *argv[]) {
    if (argc == 1) return build_all;
    const Command *cmds = Commands;
    while (cmds->name && cmds->fn) {
        if (strcmp(cmds->name, argv[1]) == 0) return cmds->fn;
        cmds++;
    }
    return NULL;
}

static void init_lld_flags() {
    vecpush(LLD_INCLUDES, zs_fmt("-I%s/include", LLD_PREFIX));
    vecpush(LLD_LIBS, zs_fmt("-L%s/lib", LLD_PREFIX));
#ifndef _WIN32
    vecpush(LLD_LIBS, zs_fmt("-Wl,-rpath,%s/lib", LLD_PREFIX));
#endif

#if defined(__APPLE__) && defined(__MACH__)
    vecunion(LLD_LIBS, ((const char *[]){
        "-llldMachO", "-llldCommon"
    }), 2);
#elif defined(__linux__)
    vecunion(LLD_LIBS, ((const char *[]){
        "-llldELF", "-llldCommon", "-lz", "-lzstd"
    }), 4);
#elif defined(_WIN32)
    vecunion(LLD_LIBS, ((const char *[]){
        "-llldCOFF", "-llldCommon"
    }), 2);
#endif
}

static vec(zstr) shell_split(zstr *buf, const char *cmd) {
    if (zb_shell(buf, cmd) != 0) {
        fprintf(stderr, "Error with command: %s\n", cmd);
        exit(1);
    }
    return zs_split_any(*buf, " \n\r\t");
}

static zstr shell_line(const char *cmd) {
    zstr out    = NULL;
    zstr line   = NULL;
    if (zb_shell(&out, cmd) == 0 && out) {
        size_t n = strcspn(out, "\r\n");
        if (n) line = zs_newn(out, n);
    }
    zs_free(out);
    return line;
}

static int version_major(const char *s) {
    for (; *s; s++)
        if (isdigit((unsigned char)s[0]) && (s[1] == '.' || isdigit((unsigned char)s[1])))
            return atoi(s);
    return -1;
}

static bool cmd_has_version(const char *exe, int version) {
    zstr cmd = zs_fmt("\"%s\" --version " NULLDEV, exe);
    zstr v = shell_line(cmd);
    bool ok = v && version_major(v) == version;
    zs_free(cmd);
    if (v) zs_free(v);
    return ok;
}

static zstr find_llvm_config() {
    const char *env = getenv("LLVM_CONFIG");
    int version = atoi(LLVM_VERSION);
    if (env && *env) {
        if (cmd_has_version(env, version)) return zs_new(env);
        fprintf(stderr, "LLVM_CONFIG=%s is not LLVM %s\n", env, LLVM_VERSION);
        exit(1);
    }

    vec(zstr) candidates = NULL;

    zstr brew = shell_line("brew --prefix llvm@" LLVM_VERSION " " NULLDEV);
    if (brew) {
        vecpush(candidates, zs_fmt("%s/bin/llvm-config", brew));
        zs_free(brew);
    }
    vecpush(candidates, zs_new("llvm-config-" LLVM_VERSION));
    vecpush(candidates, zs_new("/usr/lib/llvm-" LLVM_VERSION "/bin/llvm-config"));
    vecpush(candidates, zs_new("llvm-config"));

    zstr found = NULL;

    for (size_t i = 0; i < veclen(candidates) && !found; i++)
        if (cmd_has_version(candidates[i], version))
            found = zs_dup(candidates[i]);
    zs_free_all(candidates);

    if (!found) {
        fprintf(stderr,
            "LLVM %s not found.\n"
            " - macOS: brew install llvm@%s lld@%s"
            " - Linux: see .github/actions/setup-llvm/action.yml\n"
            " - or set LLVM_CONFIG\n",
            LLVM_VERSION, LLVM_VERSION, LLVM_VERSION
        );
        exit(1);
    }
    return found;
}

static bool is_lld_prefix(const char *p) {
    zstr hdr = zs_fmt("%s/include/lld/Common/Driver.h", p);
    zstr a   = zs_fmt("%s/lib/liblldCommon.a", p);
    zstr so  = zs_fmt("%s/lib/liblldCommon.dylib", p);
    bool ok  = zb_exists(hdr) && (zb_exists(a) || zb_exists(so));
    zs_free(hdr); zs_free(a); zs_free(so);
    if (!ok) return false;

    /* If an lld binary ships with it, make sure it's the right major. */
#ifdef _WIN32
    const char *bins[] = { "ld.lld.exe", "lld.exe", NULL };
#else
    const char *bins[] = { "ld64.lld", "ld.lld", "lld", NULL };
#endif
    for (const char **b = bins; *b; b++) {
        zstr exe = zs_fmt("%s/bin/%s", p, *b);
        bool present = zb_exists(exe);
        bool good    = present && cmd_has_version(exe, atoi(LLVM_VERSION));
        zs_free(exe);
        if (present) return good;
    }
    return true;
}

static zstr find_lld_prefix(const char *llvm_config) {
    const char *env = getenv("LLD_PREFIX");
    if (env && *env) return zs_new(env);

    vec(zstr) candidates = NULL;
    zstr cmd = zs_fmt("\"%s\" --prefix", llvm_config);
    zstr llvm_prefix = shell_line(cmd); zs_free(cmd);
    if (llvm_prefix) vecpush(candidates, llvm_prefix);
#ifdef __APPLE__
    zstr p;
    if ((p = shell_line("brew --prefix lld@" LLVM_VERSION " " NULLDEV)))
        vecpush(candidates, p);

    if ((p = shell_line("brew --prefix lld " NULLDEV)))
        vecpush(candidates, p);
#endif

    zstr found = NULL;
    for (size_t i = 0; i < veclen(candidates) && !found; i++)
        if (is_lld_prefix(candidates[i])) found = zs_dup(candidates[i]);
    zs_free_all(candidates);

    if (!found) {
        fprintf(stderr,
            "LLD %s not found (brew install lld@%s, or set LLD_PREFIX)\n",
            LLVM_VERSION, LLVM_VERSION);

        exit(1);
    }
    return found;
}

static vec(zstr) llvm_flags(const char *env, const char *args) {
    const char *v = env ? getenv(env) : NULL;
    if (v && *v) return zs_split_any(v, " \r\t\n");

    zstr cmd = zs_fmt("\"%s\" %s", LLVM_CONFIG_PATH, args);
    zstr out = NULL;
    vec(zstr) flags = shell_split(&out, cmd);
    zs_free(out);
    zs_free(cmd);
    return flags;
}

static void init_llvm_flags() {
    LLVM_CFLAGS      = (vec(const char *))llvm_flags("LLVM_CFLAGS", "--cflags");
    LLVM_LDFLAGS     = (vec(const char *))llvm_flags("LLVM_LDFLAGS", "--ldflags --libs core");
    vec(zstr) libdir = llvm_flags(NULL, "--libdir");
    vecpush(LLVM_LDFLAGS, "-lclang");
    vecpush(LSP_LDFLAGS, "-lclang");
    if (veclen(libdir)) {
        vecpush(LLVM_LDFLAGS, zs_fmt("-Wl,-rpath,%s", libdir[0]));
        vecpush(LSP_LDFLAGS, zs_fmt("-L%s", libdir[0]));
        vecpush(LSP_LDFLAGS, zs_fmt("-Wl,-rpath,%s", libdir[0]));
    }
    zs_free_all(libdir);
}

static void init_flags() {
    vecunion(CFLAGS, ((const char *[]) {
        "-g", "-Wall", "-Wextra", "-Wdeprecated-declarations",
        "-O2", "-DZVEC_USE_ZMEM"
    }), 6);

    vecunion(CPPFLAGS, ((const char *[]) {
        "-g", "-O2", "-std=c++17"
    }), 3);

    vecunion(INCLUDES, ((const char *[]) {
        "-I", "include",
        "-I", "lib"
    }), 4);

#ifdef _WIN32
    vecunion(WIN_DEFS, ((const char *[]){
        "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_WARNINGS"
    }), 2);
#endif
    LLVM_CONFIG_PATH = find_llvm_config();
    LLD_PREFIX       = find_lld_prefix(LLVM_CONFIG_PATH);
    init_lld_flags();
    init_llvm_flags();
}

int main(int argc, char *argv[]) {
    ZB_REBUILD_SELF(argc, argv);

    ZBuild *b = zb_new(0);
    b->show_command = true;

    init_flags();

    CommandFn fn = get_cmd(argc, argv);
    if (!fn) {
        printf("Unknown command %s\n", argv[1]);
        return 1;
    }

    fn(b, argc, argv);

    switch (zb_run(b)) {
    case Z_BUILD_NOOP:
        printf("nothing changed\n");
        return 0;
    case Z_BUILD_CYCLE:
        printf("detected cycle\n");
        return 1;
    case Z_BUILD_GENERIC_ERROR:
        printf("error\n");
        return 2;
    case Z_BUILD_DONE:
        printf("all done!\n");
        return 0;
    }
}
