/**
 * @file zllvm_debug_assert.c
 *
 * @brief This file generates all debug assertion functions.
 * It builds the function that emits the bound check panic for array supscript.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */
#include "zgen.h"

extern _Thread_local LLVMTypeRef i0Type;
extern _Thread_local LLVMTypeRef i1Type;
extern _Thread_local LLVMTypeRef i8Type;
extern _Thread_local LLVMTypeRef i16Type;
extern _Thread_local LLVMTypeRef i32Type;
extern _Thread_local LLVMTypeRef i64Type;
extern _Thread_local LLVMTypeRef f32Type;
extern _Thread_local LLVMTypeRef f64Type;

static LLVMValueRef emitBoundsFailFunction(ZCodegen *ctx) {

    LLVMTypeRef fnty = LLVMFunctionType(
        i0Type, (LLVMTypeRef []){
            i64Type, i64Type, LLVMPointerType(i8Type, 0)
        }, 3, false
    );
    LLVMValueRef fn = LLVMAddFunction(ctx->mod, "zn.bounds_fail", fnty);

    LLVMAddFuncAttribute(ctx, fn, "cold");
    LLVMAddFuncAttribute(ctx, fn, "noreturn");
    LLVMAddFuncAttribute(ctx, fn, "noinline");
    LLVMSetLinkage(fn, LLVMInternalLinkage);

    return fn;
}
