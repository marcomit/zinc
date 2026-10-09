/**
 * @file zllvm_debug_assert.c
 *
 * @brief This file generates all debug assertion functions.
 * It builds the function that emits the bound check panic for array supscript.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */
#include "base.h"
#include "zgen.h"
#include "zinc.h"

extern _Thread_local LLVMTypeRef i0Type;
extern _Thread_local LLVMTypeRef i1Type;
extern _Thread_local LLVMTypeRef i8Type;
extern _Thread_local LLVMTypeRef i16Type;
extern _Thread_local LLVMTypeRef i32Type;
extern _Thread_local LLVMTypeRef i64Type;
extern _Thread_local LLVMTypeRef f32Type;
extern _Thread_local LLVMTypeRef f64Type;

extern ZNode *LangItems[Z_LANG_COUNT];

#define LLVM_UBSAN_TRAP     "llvm.ubsantrap"
#define LLVM_DEBUG_TRACE    "zn.debug.trace"

void LLVMBuildTrap(ZCodegen *ctx) {
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

void emitRuntimeDebugPrint(ZCodegen *ctx, ZToken *tok, const char *message) {
    LLVMTypeRef ptrType         = LLVMPointerTypeInContext(ctx->ctx, 0);
    LLVMTypeRef funcType        = LLVMFunctionType(i32Type, &ptrType, 1, true);

    LLVMValueRef func           = LLVMGetNamedFunction(ctx->mod, "printf");
    if (!func) func             = LLVMAddFunction(ctx->mod, "printf", funcType);

    const char *fmt = "%s:%llu:%llu: %s\n";
    LLVMBuildCall2(
        ctx->builder, funcType, func, (LLVMValueRef[]){
            LLVMBuildGlobalString(ctx->builder, fmt, label(ctx, "fmt")),
            LLVMBuildGlobalString(ctx->builder, tok->filename, label(ctx, "file")),
            LLVMConstInt(i64Type, tok->row, 0),
            LLVMConstInt(i64Type, tok->col, 0),
            LLVMBuildGlobalString(ctx->builder, message, label(ctx, "msg")),
        }, 5, ""
    );
}

void emitRuntimePanic(ZCodegen *ctx, ZToken *tok, LLVMValueRef msg) {
    LLVMTypeRef ptrType         = LLVMPointerTypeInContext(ctx->ctx, 0);
    LLVMTypeRef funcType        = LLVMFunctionType(i32Type, &ptrType, 1, true);

    LLVMValueRef func           = LLVMGetNamedFunction(ctx->mod, "printf");
    if (!func) func             = LLVMAddFunction(ctx->mod, "printf", funcType);

    const char *fmt = "%s:%llu:%llu: %s\n";
    LLVMBuildCall2(
        ctx->builder, funcType, func, (LLVMValueRef[]){
            LLVMBuildGlobalString(ctx->builder, fmt, label(ctx, "fmt")),
            LLVMBuildGlobalString(ctx->builder, tok->filename, label(ctx, "file")),
            LLVMConstInt(i64Type, tok->row, 0),
            LLVMConstInt(i64Type, tok->col, 0),
            msg,
        }, 5, ""
    );
}

void emitRuntimeError(ZCodegen *ctx, ZToken *tok, const char *message) {
    LLVMBasicBlockRef fail = makeblock(ctx, "fail");

    LLVMPositionBuilderAtEnd(ctx->builder, fail);
    emitRuntimeDebugPrint(ctx, tok, message);
    LLVMBuildTrap(ctx);
}

void emitBoundCheck(ZCodegen *ctx, ZToken *tok, LLVMValueRef index,
        LLVMTypeRef arrType,
        LLVMValueRef ptr) {
    if (ctx->state->cli.mode != Z_MODE_DEBUG) return;


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

    emitPanic(ctx, tok, cond, "Index out of range");
}

/*
 * @brief Initialize the stack allocation memory to zero
 * using the intrinsic memset function.
 *
 * The memory will be zeroed only in debug mode.
 * */
void initializeMemoryToZero(ZCodegen *ctx, LLVMValueRef value, LLVMTypeRef type) {
    if (ctx->state->cli.mode != Z_MODE_DEBUG) return;
    LLVMBuildMemSet(
        ctx->builder, value,
        LLVMConstInt(i8Type, 0, 0),
        LLVMSizeOf(type),
        LLVMGetAlignment(value)
    );
}

void emitPanic(ZCodegen *ctx, ZToken *loc, LLVMValueRef cond, const char *msg) {
    LLVMBasicBlockRef fail = makeblock(ctx, "fail");
    LLVMBasicBlockRef cont = makeblock(ctx, "continue");

    makecondbr(ctx->builder, cond, fail, cont);

    LLVMPositionBuilderAtEnd(ctx->builder, fail);
    emitRuntimeDebugPrint(ctx, loc, msg);
    LLVMBuildTrap(ctx);

    LLVMPositionBuilderAtEnd(ctx->builder, cont);
}


/**
 * @brief Check whether the facet object is valid (does not contain null pointers).
 *
 * The facet is built with {objptr, vtable}.
 * Both of these fields are pointers and could be null.
 * So this function checks whether these fields are not null and emit a runtime panic.
 * */
void checkFacet(ZCodegen *ctx, LLVMValueRef facet, ZToken *tok) {
    LLVMValueRef vtable = LLVMBuildExtractValue(
        ctx->builder, facet, 1, label(ctx, "facet.vtable"));

    LLVMValueRef vtableObj  = LLVMBuildICmp(
        ctx->builder, LLVMIntEQ, vtable,
        LLVMConstPointerNull(i8Type), label(ctx, "vtable.cond")
    );

    emitPanic(ctx, tok, vtableObj,  "Facet vtable pointer is null");
}

/**
 * @brief Check if the unwrap is unsafe.
 *
 * For optional types checks the flag (the data if the base type is a pointer).
 * For result types it checks also the flag.
 *
 * If the flag is zero then it emits an llvm.trap function.
 *
 * */
void checkUnsafeUnwrap(ZCodegen *ctx,
    LLVMValueRef value, ZType *type, ZToken *loc) {
    // if (ctx->state->mode != Z_MODE_DEBUG) return;
    if (!type) return;
    LLVMValueRef cond = NULL;
    if (type->kind == Z_TYPE_OPTIONAL) {
        cond = getFlagOptional(ctx, type, value);
    }

    if (!cond) return;

    emitPanic(ctx, loc, cond, "Unwrap a none value");
}

void emitNullCheck(ZCodegen *ctx, LLVMValueRef value, ZToken *tok) {
    LLVMValueRef cond = LLVMBuildICmp(
        ctx->builder, LLVMIntEQ, value,
        LLVMConstNull(LLVMPointerTypeInContext(ctx->ctx, 0)),
        label(ctx, "null.check")
    );

    emitPanic(ctx, tok, cond, "Trying to read null value, maybe uninitialized or freed memory");
}

LLVMValueRef loadWithNullDeref(ZCodegen *ctx, LLVMTypeRef type, LLVMValueRef value, ZToken *tok) {
    emitNullCheck(ctx, value, tok);
    return LLVMBuildLoad2(ctx->builder, type, value, stoken(tok));
}
