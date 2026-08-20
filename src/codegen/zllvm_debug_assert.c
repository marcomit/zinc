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

static void LLVMBuildTrap(ZCodegen *ctx) {
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

    const char *fmt = "%s(%llu:%llu): %s\n";
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

void emitRuntimeError(ZCodegen *ctx, ZToken *tok, const char *message) {
    LLVMBasicBlockRef fail = makeblock(ctx, "fail");

    LLVMPositionBuilderAtEnd(ctx->builder, fail);
    emitRuntimeDebugPrint(ctx, tok, message);
    LLVMBuildTrap(ctx);
}

void emitBoundCheck(ZCodegen *ctx, ZToken *tok, LLVMValueRef index,
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
    LLVMPositionBuilderAtEnd(ctx->builder, fail);
    emitRuntimeDebugPrint(ctx, tok, "Index out of range");
    LLVMBuildTrap(ctx);

    emitRuntimeError(ctx, tok, "Index out of range");
    LLVMPositionBuilderAtEnd(ctx->builder, cont);
}

/*
 * @brief Initialize the stack allocation memory to zero
 * using the intrinsic memset function.
 *
 * The memory will be zeroed only in debug mode.
 * */
void initializeMemoryToZero(ZCodegen *ctx, LLVMValueRef value, ZType *type) {
    if (ctx->state->mode != Z_MODE_DEBUG) return;
    LLVMBuildMemSet(
        ctx->builder, value,
        LLVMConstInt(i8Type, 0, 0),
        LLVMConstInt(i64Type, typeSize(type), 0),
        8
    );
}

/*
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

    LLVMBasicBlockRef fail = makeblock(ctx, "fail");
    LLVMBasicBlockRef cont = makeblock(ctx, "continue");

    makecondbr(ctx->builder, cond, fail, cont);
    LLVMPositionBuilderAtEnd(ctx->builder, fail);
    emitRuntimeDebugPrint(ctx, loc, "Unwrap a none value");
    LLVMBuildTrap(ctx);

    LLVMPositionBuilderAtEnd(ctx->builder, cont);
}

LLVMValueRef genPanic(ZCodegen *ctx, ZNode *call) {
    if (veclen(call->call.args) != 1) {
        error(
            ctx->state, call->tok,
            "Expected 1 argument, got %zu", veclen(call->call.args));
    }
    emitRuntimeDebugPrint(ctx, call->tok, stoken(call->call.args[0]->tok));
    LLVMBuildTrap(ctx);
    return LLVMConstNull(i0Type);
}

LLVMValueRef genReflect(ZCodegen *ctx, ZNode *node) {
    return NULL;
}

LLVMValueRef genHere(ZCodegen *ctx, ZNode *node) {
    ZNode *sourceLocType = LangItems[Z_LANG_SOURCE_LOCATION_TYPE];
    if (!sourceLocType) {
        error(ctx->state, node->tok, "SourceLocationType not found");
        return NULL;
    }

    ZType *type = sourceLocType->resolved;
    return LLVMConstNull(genType(ctx, type));
}

LLVMValueRef genVolatileStore(ZCodegen *ctx, ZNode *node) {
    ZNode *base = node->call.args[0];
    ZNode *val  = node->call.args[1];
    LLVMValueRef store = LLVMBuildStore(
        ctx->builder, genExpr(ctx, val), genExpr(ctx, base)
    );
    LLVMSetVolatile(store, true);
    return NULL;
}

LLVMValueRef genVolatileLoad(ZCodegen *ctx, ZNode *node) {
    LLVMTypeRef typeRef = genType(ctx, node->resolved);
    LLVMValueRef load = LLVMBuildLoad2(
        ctx->builder, typeRef, genExpr(ctx, node->call.args[0]), label(ctx, "volatile.load")
    );
    LLVMSetVolatile(load, true);
    return load;
}

LLVMValueRef genPtrOffset(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr    = genExpr(ctx, node->call.args[0]);
    LLVMValueRef offset = genExpr(ctx, node->call.args[1]);
    LLVMTypeRef type    = genType(ctx, node->call.args[0]->resolved);

    return LLVMBuildGEP2(
        ctx->builder, type, ptr, &offset, 1, label(ctx, "builtin.ptr_offset")
    );
}

LLVMValueRef genPtrToInt(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr = genExpr(ctx, node->call.args[0]);
    return LLVMBuildPtrToInt(
        ctx->builder, ptr, i64Type, label(ctx, "builtin.ptr_to_int")
    );
}

LLVMValueRef genPtrFromInt(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr = genExpr(ctx, node->call.args[0]);
    return LLVMBuildIntToPtr(
        ctx->builder, ptr, LLVMPointerTypeInContext(ctx->ctx, 0), label(ctx, "builtin.ptr_from_int")
    );
}
