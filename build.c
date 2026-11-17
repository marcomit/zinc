#include "lib/zvec.h"
#include "lib/zstr.h"
#include "lib/zbuild.h"

#define BUILD_DIR "build"
#define LLVM_VERSION "22"

#ifdef _WIN32
#else
#endif

typedef void (*CommandFn)(ZBuild *, int , char *[]);
typedef struct {
    const char *name;
    CommandFn fn;
} Command;

static const char *TARGET               = "zinc";
static const char *ZMODE_FLAGS          = "-DZMODE=ZMODE_COMPILER";
static const char *SANITIZE             = NULL;
static vec(const char *) CFLAGS         = NULL;
static vec(const char *) CPPFLAGS       = NULL;
static vec(const char *) INCLUDES       = NULL;
static vec(const char *) WIN_DEFS       = NULL;
static vec(const char *) LLD_LIBS       = NULL;
static vec(const char *) LLD_INCLUDES   = NULL;
static vec(const char *) LLVM_CFLAGS    = NULL;
static vec(const char *) LLVM_LDFLAGS   = NULL;

static void build_link(ZBuild *b, vec(ZCmd *) cmds) {
    ZCmd *l = zb_cmd(b, "clang++");
    zb_arg(l, "-o");
    zb_out(l, TARGET);

    for (size_t i = 0; i < veclen(cmds); i++) {
        ZCmd *c = cmds[i];
        zb_after(l, c);
        for (size_t j = 0; j < veclen(c->outputs); j++) {
            zb_in(l, c->outputs[j]);
        }
    }

    zb_argsv(l, LLD_LIBS);
    zb_argsv(l, LLVM_LDFLAGS);
    if (SANITIZE) zb_arg(l, SANITIZE);
}

static ZCmd *build_cpp(ZBuild *b, const char *input) {
    const char *output = zb_path_map(
        input,
        zb_starts_with(input, "lib/") ? "lib/" : "src/",
        "build/", ".o"
    );
    ZCmd *c = zb_cmd(b, "clang++");
    zb_argsv(c, CPPFLAGS);
    zb_argsv(c, INCLUDES);
    zb_argsv(c, WIN_DEFS);
    zb_argsv(c, LLVM_CFLAGS);
    zb_argsv(c, LLD_INCLUDES);
    zb_arg  (c, SANITIZE);
    zb_arg  (c, ZMODE_FLAGS);
    zb_arg  (c, "-c");
    zb_in   (c, input);
    zb_arg  (c, "-o");
    zb_out  (c, output);

    return c;
}

static ZCmd *build_c(ZBuild *b, const char *input) {
    const char *output = zb_path_map(
        input,
        zb_starts_with(input, "lib/") ? "lib/" : "src/",
        "build/", ".o"
    );

    ZCmd *c = zb_cmd(b, "clang");
    zb_argsv(c, CFLAGS);
    zb_argsv(c, INCLUDES);
    zb_argsv(c, WIN_DEFS);
    zb_argsv(c, LLVM_CFLAGS);
    zb_argsv(c, LLD_INCLUDES);
    zb_arg  (c, SANITIZE);
    zb_arg  (c, ZMODE_FLAGS);
    zb_arg  (c, "-c");
    zb_in   (c, input);
    zb_arg  (c, "-o");
    zb_out  (c, output);

    return c;
}

static void build_all(ZBuild *b, int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    vec(const char *) c_files = zb_walk("src", (ZBWalkFilter){
        .skip = ZB_LIST("src/lsp"),
        .suffix = ZB_LIST(".c", ".cpp")
    });

    vec(const char *) lib_files = zb_walk("lib", (ZBWalkFilter){
        .skip = ZB_LIST("lib/json.c"),
        .suffix = ZB_LIST(".c")
    });

    vecunion(c_files, lib_files, veclen(lib_files));
    vecfree(lib_files);

    vec(char) llvm_flags = NULL;

    if (zb_shell(&llvm_flags, "llvm-config --cflags") != 0) {
        printf("Error llvm-config\n");
    }

    vec(ZCmd *) outs = NULL;
    for (size_t i = 0; i < veclen(c_files); i++) {
        ZCmd *c = zb_ends_with(c_files[i], ".cpp") ?
            build_cpp(b, c_files[i]) : build_c(b, c_files[i]);
        vecpush(outs, c);
    }

    build_link(b, outs);
}

static void build_lsp(ZBuild *b, int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    ZMODE_FLAGS = "-DZMODE=ZMODE_LSP";
    vec(const char *) c_files = zb_walk("src", (ZBWalkFilter){
        .skip = ZB_LIST("src/codegen"),
        .suffix = ZB_LIST(".c")
    });

    vec(ZCmd *) outs = NULL;
    for (size_t i = 0; i < veclen(c_files); i++) {
        ZCmd *c = zb_cmd(b, "clang");
        zb_argsv(c, CFLAGS);
        zb_argsv(c, INCLUDES);
        zb_arg(c, ZMODE_FLAGS);
        vecpush(outs, c);
    }
}

static void build_clean(ZBuild *b, int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    ZCmd *rm_bin = zb_cmd(b, "rm");
    zb_args(rm_bin, "-f", "zinc", "zinc-lsp", "zinc-asan", "zinc-ubsan", "zinc-ubsan-int");

    ZCmd *rm_build = zb_cmd(b, "rm");
    zb_args(rm_build, "-rf", "build");
}

static void build_debug(ZBuild *b, int argc, char *argv[]) {
    SANITIZE = "-fsanitize=address,undefined";
    TARGET = "zinc-debug";
    build_all(b, argc, argv);
}

static void build_asan(ZBuild *b, int argc, char *argv[]) {
    SANITIZE = "-fsanitize=address,undefined";
    TARGET = "zinc-asan";
    build_all(b, argc, argv);
}

static void build_ubsan(ZBuild *b, int argc, char *argv[]) {
    SANITIZE = "-fsanitize=undefined,local-bounds";
    TARGET = "zinc-ubsan";
    build_all(b, argc, argv);
}

static void build_ubsan_int(ZBuild *b, int argc, char *argv[]) {
    SANITIZE = "-fsanitize=undefined,integer,implicit-conversion,local-bounds";
    TARGET = "zinc-ubsan-int";
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
    snprintf(cmd, sizeof(cmd),
        "rmdir /s /q \"%s\\.zinc\\packages\\std\" 2>nul & "
        "xcopy /e /i /y /q \"std\" \"%s\\.zinc\\packages\\std\" >nul",
        home, home);
#else
    const char *home_var = "HOME";
    const char *home = getenv(home_var);
    snprintf(cmd, sizeof(cmd),
        "rm -rf \"%s/.zinc/packages/std\" && "
        "mkdir -p \"%s/.zinc/packages/std\" && "
        "cp -R std/. \"%s/.zinc/packages/std/\"",
        home, home, home);
#endif

    if (!home) {
        printf("Error: %s not found\n", home_var);
        return;
    }

    if (system(cmd) != 0) return;
#ifdef _WIN32
    printf("installed stdlib -> %s\\.zinc\\packages\\std\n", home);
#else
    printf("installed stdlib -> %s/.zinc/packages/std\n", home);
#endif
}

static void build_test(ZBuild *b, int argc, char *argv[]) {
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
#if defined(__APPLE__) && defined(__MACH__)
    const char *lld_prefix = zb_sprint("/opt/homebrew/opt/lld@%s", LLVM_VERSION);

    if (!lld_prefix) {
        printf("LLD %s not found, Run brew install lld@%s\n", LLVM_VERSION, LLVM_VERSION);
        return;
    }

    vecpush(LLD_INCLUDES, zb_sprint("-I%s/include", lld_prefix));
    vecunion(LLD_LIBS, ((const char *[]){
        zb_sprint("-L%s/lib", lld_prefix),
        zb_sprint("-Wl,-rpath,%s/lib", lld_prefix),
        "-llldMachO", "-llldCommon"
    }), 4);
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

static vec(const char *) shell_split(vec(char) *buf, const char *cmd) {
    if (zb_shell(buf, cmd) != 0) {
        printf("Error with command: %s\n", cmd);
        // exit(1);
    }
    return zb_split_any(*buf, " \n\r\t");
}

static vec(const char *) llvm_flags(vec(char) *out, const char *env, const char *cmd) {
    const char *v = getenv(env);
    if (v && *v) return zb_split_any(v, " \r\t\n");
    if (*out) vecsetlen(*out, 0);   // vecsetlen dereferences: the vec may still be NULL
    return shell_split(out, cmd);
}

static void init_llvm_flags() {
    vec(char) out   = NULL;
    LLVM_CFLAGS     = llvm_flags(&out, "LLVM_CFLAGS", "llvm-config --cflags");
    LLVM_LDFLAGS    = llvm_flags(&out, "LLVM_LDFLAGS", "llvm-config --ldflags --libs core");

    if (out) vecsetlen(out, 0);
    vec(const char *) libdir = shell_split(&out, "llvm-config --libdir");
    if (veclen(libdir)) vecpush(LLVM_LDFLAGS, zb_sprint("-Wl,-rpath,%s", libdir[0]));
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
        printf("all done\n");
        return 0;
    case Z_BUILD_CYCLE:
        printf("detected cycle\n");
        return 1;
    case Z_BUILD_GENERIC_ERROR: return 2;
    case Z_BUILD_DONE:          return 0;
    }
}
