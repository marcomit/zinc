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
#include "zinc.h"

extern _Thread_local ZMIRTypeRef i0Type;
extern _Thread_local ZMIRTypeRef i1Type;
extern _Thread_local ZMIRTypeRef i8Type;
extern _Thread_local ZMIRTypeRef i16Type;
extern _Thread_local ZMIRTypeRef i32Type;
extern _Thread_local ZMIRTypeRef i64Type;
extern _Thread_local ZMIRTypeRef f32Type;
extern _Thread_local ZMIRTypeRef f64Type;

extern ZNode *LangItems[Z_LANG_COUNT];


void emitRuntimeDebugPrint(ZCodegen *ctx, ZToken *tok, const char *message) {
    ZMIRTypeRef ptrType         = ZMIRPointerTypeInContext(ctx->ctx, 0);
    ZMIRTypeRef funcType        = ZMIRFunctionType(i32Type, &ptrType, 1, true);

    ZMIRValueRef func           = ZMIRGetNamedFunction(ctx->mod, "printf");
    if (!func) func             = ZMIRAddFunction(ctx->mod, "printf", funcType);

    const char *fmt = "%s:%llu:%llu: %s\n";
    ZMIRBuildCall2(
        ctx->builder, funcType, func, (ZMIRValueRef[]){
            ZMIRBuildGlobalString(ctx->builder, fmt, label(ctx, "fmt")),
            ZMIRBuildGlobalString(ctx->builder, tok->filename, label(ctx, "file")),
            ZMIRConstInt(i64Type, tok->row, 0),
            ZMIRConstInt(i64Type, tok->col, 0),
            ZMIRBuildGlobalString(ctx->builder, message, label(ctx, "msg")),
        }, 5, ""
    );
}

void emitRuntimePanic(ZCodegen *ctx, ZToken *tok, ZMIRValueRef msg) {
    ZMIRTypeRef ptrType         = ZMIRPointerTypeInContext(ctx->ctx, 0);
    ZMIRTypeRef funcType        = ZMIRFunctionType(i32Type, &ptrType, 1, true);

    ZMIRValueRef func           = ZMIRGetNamedFunction(ctx->mod, "printf");
    if (!func) func             = ZMIRAddFunction(ctx->mod, "printf", funcType);

    const char *fmt = "%s:%llu:%llu: %s\n";
    ZMIRBuildCall2(
        ctx->builder, funcType, func, (ZMIRValueRef[]){
            ZMIRBuildGlobalString(ctx->builder, fmt, label(ctx, "fmt")),
            ZMIRBuildGlobalString(ctx->builder, tok->filename, label(ctx, "file")),
            ZMIRConstInt(i64Type, tok->row, 0),
            ZMIRConstInt(i64Type, tok->col, 0),
            msg,
        }, 5, ""
    );
}

void emitRuntimeError(ZCodegen *ctx, ZToken *tok, const char *message) {
    ZMIRBasicBlockRef fail = makeblock(ctx, "fail");

    ZMIRPositionBuilderAtEnd(ctx->builder, fail);
    emitRuntimeDebugPrint(ctx, tok, message);
    ZMIRBuildTrap(ctx->mod, ctx->builder);
}

void emitBoundCheck(ZCodegen *ctx, ZToken *tok, ZMIRValueRef index,
        ZMIRTypeRef arrType,
        ZMIRValueRef ptr) {
    if (ctx->state->cli.mode != Z_MODE_DEBUG) return;


    ZMIRValueRef lenPtr = ZMIRBuildStructGEP2(
        ctx->builder, arrType, ptr,
        0, label(ctx, "len.ptr")
    );
    ZMIRValueRef len = ZMIRBuildLoad2(
        ctx->builder, i64Type, lenPtr, label(ctx, "bound.check")
    );

    /* The index may be narrower than the u64 length field. */
    ZMIRValueRef idx = ZMIRBuildZExtOrBitCast(
        ctx->builder, index, i64Type, label(ctx, "bound.idx")
    );

    ZMIRValueRef cond = ZMIRBuildICmp(
        ctx->builder, ZMIRIntUGE, idx, len, label(ctx, "bound.cond")
    );

    emitPanic(ctx, tok, cond, "Index out of range");
}

/*
 * @brief Initialize the stack allocation memory to zero
 * using the intrinsic memset function.
 *
 * The memory will be zeroed only in debug mode.
 * */
void initializeMemoryToZero(ZCodegen *ctx, ZMIRValueRef value, ZMIRTypeRef type) {
    if (ctx->state->cli.mode != Z_MODE_DEBUG) return;
    ZMIRBuildMemSet(
        ctx->builder, value,
        ZMIRConstInt(i8Type, 0, 0),
        ZMIRSizeOf(type),
        ZMIRGetAlignment(value)
    );
}

void emitPanic(ZCodegen *ctx, ZToken *loc, ZMIRValueRef cond, const char *msg) {
    ZMIRBasicBlockRef fail = makeblock(ctx, "fail");
    ZMIRBasicBlockRef cont = makeblock(ctx, "continue");

    makecondbr(ctx->builder, cond, fail, cont);

    ZMIRPositionBuilderAtEnd(ctx->builder, fail);
    emitRuntimeDebugPrint(ctx, loc, msg);
    ZMIRBuildTrap(ctx->mod, ctx->builder);

    ZMIRPositionBuilderAtEnd(ctx->builder, cont);
}


/**
 * @brief Check whether the facet object is valid (does not contain null pointers).
 *
 * The facet is built with {objptr, vtable}.
 * Both of these fields are pointers and could be null.
 * So this function checks whether these fields are not null and emit a runtime panic.
 * */
void checkFacet(ZCodegen *ctx, ZMIRValueRef facet, ZToken *tok) {
    ZMIRValueRef vtable = ZMIRBuildExtractValue(
        ctx->builder, facet, 1, label(ctx, "facet.vtable"));

    ZMIRValueRef vtableObj  = ZMIRBuildICmp(
        ctx->builder, ZMIRIntEQ, vtable,
        ZMIRConstPointerNull(i8Type), label(ctx, "vtable.cond")
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
    ZMIRValueRef value, ZType *type, ZToken *loc) {
    // if (ctx->state->mode != Z_MODE_DEBUG) return;
    if (!type) return;
    ZMIRValueRef cond = NULL;
    if (type->kind == Z_TYPE_OPTIONAL) {
        cond = getFlagOptional(ctx, type, value);
    }

    if (!cond) return;

    emitPanic(ctx, loc, cond, "Unwrap a none value");
}

void emitNullCheck(ZCodegen *ctx, ZMIRValueRef value, ZToken *tok) {
    ZMIRValueRef cond = ZMIRBuildICmp(
        ctx->builder, ZMIRIntEQ, value,
        ZMIRConstNull(ZMIRPointerTypeInContext(ctx->ctx, 0)),
        label(ctx, "null.check")
    );

    emitPanic(ctx, tok, cond, "Trying to read null value, maybe uninitialized or freed memory");
}

ZMIRValueRef loadWithNullDeref(ZCodegen *ctx, ZMIRTypeRef type, ZMIRValueRef value, ZToken *tok) {
    emitNullCheck(ctx, value, tok);
    return ZMIRBuildLoad2(ctx->builder, type, value, stoken(tok));
}
