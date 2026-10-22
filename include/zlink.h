// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#ifndef ZLINK_H
#define ZLINK_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    Z_OS_MACOS,
    Z_OS_IOS,
    Z_OS_LINUX,
    Z_OS_WINDOWS,
    Z_OS_WASM,
    Z_OS_FREEBSD,
    Z_OS_FREESTANDING,
    Z_OS_UNKNOWN
} ZBuiltinOS;

typedef enum {
    Z_ARCH_X86_64,
    Z_ARCH_AARCH64,
    Z_ARCH_RISCV64,
    Z_ARCH_WASM32,
    Z_ARCH_AVR,
    Z_ARCH_UNKNOWN
} ZBuiltinArch;

ZBuiltinOS ztripleOS(const char *);
ZBuiltinArch ztripleArch(const char *);

int zinc_lld_link(
    bool nostdlib,
    const char *objfile,        const char *outfile,
    const char **extra_args,    int extra_args_count);

#ifdef __cplusplus
}
#endif

#endif
