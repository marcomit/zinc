#include "zlink.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(__APPLE__)
LLD_HAS_DRIVER(macho)
#elif defined(_WIN32)
LLD_HAS_DRIVER(coff)
#else
LLD_HAS_DRIVER(elf)
#endif

static std::string runCmd(const char *cmd) {
    char buf[1024] = {};
    FILE *f = popen(cmd, "r");
    if (!f) return {};
    if (!fgets(buf, sizeof(buf), f)) { pclose(f); return {}; }
    pclose(f);
    std::string s(buf);
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s;
}

extern "C" int zinc_lld_link(const char *objfile, const char *outfile,
    const char **extra_args, int extra_args_count) {
#if defined(__APPLE__)
    std::string sdk = runCmd("xcrun --sdk macosx --show-sdk-path 2>/dev/null");
    std::string ver = runCmd("sw_vers -productVersion 2>/dev/null");

    // Trim to major.minor
    auto p = ver.find('.');
    if (p != std::string::npos) {
        auto p2 = ver.find('.', p + 1);
        if (p2 != std::string::npos) ver = ver.substr(0, p2);
    }
    if (ver.empty()) ver = "13.0";
    if (sdk.empty()) sdk = "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk";

    std::vector<const char *> args = {
        "ld64.lld",
        "-arch",
#if defined(__aarch64__)
        "arm64",
#else
        "x86_64",
#endif
        "-platform_version", "macos",
        ver.c_str(), ver.c_str(),
        "-syslibroot", sdk.c_str(),
        "-lSystem",
        objfile,
    };

    for (int i = 0; i < extra_args_count; i++) {
        args.push_back(extra_args[i]);
    }

    args.push_back("-o");
    args.push_back(outfile);

    lld::DriverDef drivers[] = {{lld::Darwin, &lld::macho::link}};
    auto res = lld::lldMain(
        llvm::ArrayRef<const char *>(args.data(), args.size()),
        llvm::outs(), llvm::errs(),
        llvm::ArrayRef<lld::DriverDef>(drivers, 1)
    );
    return res.retCode;

#elif defined(_WIN32) && defined(__MINGW32__)
    // On MSYS2/MinGW64, delegate linking to clang — it handles the sysroot,
    // CRT startup files, and MinGW runtime libraries automatically.
    // system() on MinGW uses cmd.exe; errors from clang flow to our stderr.
    char cmd[8192];
    // Quote both paths — they may contain spaces or backslashes.
    int n = snprintf(cmd, sizeof(cmd), "clang -o \"%s\" \"%s\"", outfile, objfile);
    for (int i = 0; i < extra_args_count && n < (int)sizeof(cmd) - 256; i++)
        n += snprintf(cmd + n, sizeof(cmd) - n, " \"%s\"", extra_args[i]);
    int ret = system(cmd);
    return (ret == 0) ? 0 : 1;

#elif defined(_WIN32)
    // MSVC: lld-link with the MSVC / Windows SDK runtime libraries.
    std::string outarg = "/out:" + std::string(outfile);
    std::vector<const char *> args = {
        "lld-link",
        "/nologo",
        "/subsystem:console",
        "/defaultlib:libcmt",
        "/defaultlib:oldnames",
    };
    args.push_back(objfile);
    for (int i = 0; i < extra_args_count; i++)
        args.push_back(extra_args[i]);
    args.push_back(outarg.c_str());

    lld::DriverDef drivers[] = {{lld::WinLink, &lld::coff::link}};
    auto res = lld::lldMain(
        llvm::ArrayRef<const char *>(args.data(), args.size()),
        llvm::outs(), llvm::errs(),
        llvm::ArrayRef<lld::DriverDef>(drivers, 1)
    );
    return res.retCode;

#else
    // Use clang to locate CRT files and the system lib directory.
    auto findFile = [](const char *name) -> std::string {
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "clang -print-file-name=%s 2>/dev/null", name);
        return runCmd(cmd);
    };

    std::string crt1 = findFile("crt1.o");
    std::string crti = findFile("crti.o");
    std::string crtn = findFile("crtn.o");

    // Derive the multiarch lib directory from libc.so location.
    std::string libc_path = findFile("libc.so");
    std::string libdir;
    auto slash = libc_path.rfind('/');
    if (slash != std::string::npos)
        libdir = libc_path.substr(0, slash);

#if defined(__x86_64__)
    const char *emulation  = "elf_x86_64";
    const char *dynlinker  = "/lib64/ld-linux-x86-64.so.2";
#elif defined(__aarch64__)
    const char *emulation  = "aarch64linux";
    const char *dynlinker  = "/lib/ld-linux-aarch64.so.1";
#else
    const char *emulation  = "elf_i386";
    const char *dynlinker  = "/lib/ld-linux.so.2";
#endif

    std::vector<const char *> args = {
        "ld.lld",
        "-m",               emulation,
        "--dynamic-linker", dynlinker,
    };
    if (!libdir.empty()) {
        args.push_back("-L");
        args.push_back(libdir.c_str());
    }
    args.insert(args.end(), {
        crt1.c_str(),
        crti.c_str(),
        objfile,
        "-lc",
    });

    for (int i = 0; i < extra_args_count; i++) {
        args.push_back(extra_args[i]);
    }

    args.push_back(crtn.c_str());
    args.push_back("-o");
    args.push_back(outfile);

    lld::DriverDef drivers[] = {{lld::Gnu, &lld::elf::link}};
    auto res = lld::lldMain(
        llvm::ArrayRef<const char *>(args.data(), args.size()),
        llvm::outs(), llvm::errs(),
        llvm::ArrayRef<lld::DriverDef>(drivers, 1)
    );
    return res.retCode;
#endif
}
