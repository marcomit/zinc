#include "zlink.h"
#include "lld/Common/Driver.h"
#include "llvm/Support/raw_ostream.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#if defined(__APPLE__)
LLD_HAS_DRIVER(macho)
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
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

extern "C" int zinc_lld_link(const char *objfile, const char *outfile) {
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
        "-o", outfile,
    };

    lld::DriverDef drivers[] = {{lld::Darwin, &lld::macho::link}};
    auto res = lld::lldMain(
        llvm::ArrayRef<const char *>(args.data(), args.size()),
        llvm::outs(), llvm::errs(),
        llvm::ArrayRef<lld::DriverDef>(drivers, 1)
    );
    return res.retCode;

#else
    std::vector<const char *> args = {
        "ld.lld", objfile, "-o", outfile,
    };
    lld::DriverDef drivers[] = {{lld::Gnu, &lld::elf::link}};
    auto res = lld::lldMain(
        llvm::ArrayRef<const char *>(args.data(), args.size()),
        llvm::outs(), llvm::errs(),
        llvm::ArrayRef<lld::DriverDef>(drivers, 1)
    );
    return res.retCode;
#endif
}
