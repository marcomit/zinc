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
#include <string.h>

extern _Thread_local LLVMTypeRef i0Type;
extern _Thread_local LLVMTypeRef i1Type;
extern _Thread_local LLVMTypeRef i8Type;
extern _Thread_local LLVMTypeRef i16Type;
extern _Thread_local LLVMTypeRef i32Type;
extern _Thread_local LLVMTypeRef i64Type;
extern _Thread_local LLVMTypeRef f32Type;
extern _Thread_local LLVMTypeRef f64Type;

#define LLVM_UBSAN_TRAP "llvm.ubsantrap"

static void emitBoundsFailBlock(ZCodegen *ctx, LLVMBasicBlockRef failBlock) {
    LLVMPositionBuilderAtEnd(ctx->builder, failBlock);

    u32 ubsanId                 = LLVMLookupIntrinsicID(
        LLVM_UBSAN_TRAP, strlen(LLVM_UBSAN_TRAP));

    LLVMValueRef ubsanFunc      = LLVMGetIntrinsicDeclaration(
        ctx->mod, ubsanId, NULL, 0);

    LLVMTypeRef funcType        = LLVMFunctionType(
        i0Type, (LLVMTypeRef[]){i8Type}, 1, 0);

    LLVMBuildCall2(
        ctx->builder, funcType, ubsanFunc, (LLVMValueRef[]){
            LLVMConstInt(i8Type, 0, 0),
        }, 1, ""
    );

    LLVMBuildUnreachable(ctx->builder);
}

void emitBoundCheck(ZCodegen *ctx, LLVMValueRef index,
        LLVMTypeRef arrType,
        LLVMValueRef ptr) {
    if (ctx->state->mode != Z_MODE_DEBUG) return;

    LLVMBasicBlockRef fail = makeblock(ctx, "bound.fail");
    LLVMBasicBlockRef cont = makeblock(ctx, "bound.cont");

    LLVMValueRef lenPtr = LLVMBuildStructGEP2(
        ctx->builder, arrType, ptr,
        0, label(ctx, "len.ptr")
    );
    LLVMValueRef len = LLVMBuildLoad2(
        ctx->builder, i64Type, lenPtr, label(ctx, "bound.check")
    );

    /* The index may be narrower than the u64 length field. */
    LLVMValueRef idx = LLVMBuildZExtOrBitCast(
        ctx->builder, index, i64Type, label(ctx, "bound.idx")
    );

    LLVMValueRef cond = LLVMBuildICmp(
        ctx->builder, LLVMIntULT, idx, len, label(ctx, "bound.cond")
    );

    makecondbr(ctx->builder, cond, cont, fail);

    emitBoundsFailBlock(ctx, fail);

    LLVMPositionBuilderAtEnd(ctx->builder, cont);
}
