#include "zinc.h"
#include "zgen.h"

extern _Thread_local LLVMTypeRef i0Type;
extern _Thread_local LLVMTypeRef i1Type;
extern _Thread_local LLVMTypeRef i8Type;
extern _Thread_local LLVMTypeRef i16Type;
extern _Thread_local LLVMTypeRef i32Type;
extern _Thread_local LLVMTypeRef i64Type;
extern _Thread_local LLVMTypeRef f32Type;
extern _Thread_local LLVMTypeRef f64Type;
extern ZNode *LangItems[Z_LANG_COUNT];

bool genBuiltin(ZCodegen *, ZNode *, LLVMValueRef *);

typedef LLVMValueRef (*ZBuiltinFn) (ZCodegen *, ZNode *);
typedef ZType *(*ZValidateFn) (ZState *, ZNode *);

static LLVMValueRef genPanic(ZCodegen *ctx, ZNode *call) {
    if (veclen(call->call.args) != 1) {
        zlog(ctx->state, call->tok, Z4020, veclen(call->call.args));
    }
    emitRuntimeDebugPrint(ctx, call->tok, stoken(call->call.args[0]->tok));
    LLVMBuildTrap(ctx);
    return LLVMConstNull(i0Type);
}

static LLVMValueRef genReflect(ZCodegen *ctx, ZNode *node) {
    return NULL;
}

static LLVMValueRef genHere(ZCodegen *ctx, ZNode *node) {
    ZNode *sourceLocType = LangItems[Z_LANG_SOURCE_LOCATION_TYPE];
    if (!sourceLocType) {
        zlog(ctx->state, node->tok, Z4021);
        return NULL;
    }

    ZType *type = sourceLocType->resolved;
    return LLVMConstNull(genType(ctx, type));
}

static LLVMValueRef genVolatileStore(ZCodegen *ctx, ZNode *node) {
    ZNode *base = node->call.args[0];
    ZNode *val  = node->call.args[1];
    LLVMValueRef store = LLVMBuildStore(
        ctx->builder, genExpr(ctx, val), genExpr(ctx, base)
    );
    LLVMSetVolatile(store, true);
    return NULL;
}

static LLVMValueRef genVolatileLoad(ZCodegen *ctx, ZNode *node) {
    LLVMTypeRef typeRef = genType(ctx, node->resolved);
    LLVMValueRef load = LLVMBuildLoad2(
        ctx->builder, typeRef, genExpr(ctx, node->call.args[0]), label(ctx, "volatile.load")
    );
    LLVMSetVolatile(load, true);
    return load;
}

static LLVMValueRef genPtrOffset(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr    = genExpr(ctx, node->call.args[0]);
    LLVMValueRef offset = genExpr(ctx, node->call.args[1]);

    ZType *ptrType      = node->call.args[0]->resolved;
    ZType *pointeeType  = ptrType;
    if (ptrType && ptrType->kind == Z_TYPE_POINTER) {

        pointeeType = ptrType->base;
    }
    LLVMTypeRef type    = genType(ctx, pointeeType);

    return LLVMBuildGEP2(
        ctx->builder, type, ptr, &offset, 1, label(ctx, "builtin.ptr_offset")
    );
}

static LLVMValueRef genPtrToInt(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr = genExpr(ctx, node->call.args[0]);
    return LLVMBuildPtrToInt(
        ctx->builder, ptr, i64Type, label(ctx, "builtin.ptr_to_int")
    );
}

static LLVMValueRef genPtrFromInt(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr = genExpr(ctx, node->call.args[0]);
    return LLVMBuildIntToPtr(
        ctx->builder, ptr, LLVMPointerTypeInContext(ctx->ctx, 0), label(ctx, "builtin.ptr_from_int")
    );
}

#define ensure(cond, tok, ...) if (!(cond)) {                               \
    zlog(state, tok, __VA_ARGS__);                                          \
    return;                                                                 \
}

static ZType *ZPtr(ZType *base) {
    ZType *res  = maketype(Z_TYPE_POINTER);
    res->tok    = base->tok;
    res->base   = base;
    res->hash   = hash(res);
    return res;
}

ZType *LangExpectedType[Z_LANG_COUNT] = { NULL };

ZBuiltinFn LangBuiltins[Z_LANG_COUNT] = {
    #define LANG(id, name, type, func, valid) [id] = func,
    #include "zlang.def"
    #undef LANG
};

ZValidateFn LangValidators[Z_LANG_COUNT] = {
    #define LANG(id, name, type, func, valid) [id] = valid,
    #include "zlang.def"
    #undef LANG
};

bool genBuiltin(ZCodegen *ctx, ZNode *node, LLVMValueRef *out) {
    ZNode *langNode = node;
    if (node->type == NODE_CALL) langNode = node->call.callee;

    ZLangItemType li = getLangItemType(langNode);
    if (!li || !LangBuiltins[li]) return false;

    LLVMValueRef result = LangBuiltins[li](ctx, node);
    if (out) *out = result;
    return true;
}

void validate(ZState *ctx, ZNode *node) {
    ZLangItemType li = getLangItemType(node);
    if (!LangValidators[li]) return;
    if (!LangExpectedType[li]) {
        LangExpectedType[li] = LangValidators[li](ctx, node);
    }

    if (!typesEqual(node->resolved, LangExpectedType[li])) {
        zlog(
            ctx, node->tok, Z4022,
            stype(LangExpectedType[li]), stype(node->resolved)
        );
    }
}
