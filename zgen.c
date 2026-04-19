#include "base.h"
#include "zcolors.h"
#include "zinc.h"
#include "zlink.h"

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>

typedef struct ZLLVMSymbol {
    ZToken *token;
    char *name;
    ZNode *node;
    LLVMValueRef value;
    LLVMTypeRef type;
} ZLLVMSymbol;

typedef struct {
    LLVMValueRef    stack;
    LLVMTypeRef     type;
    ZNode           *node;
} ZLLVMStack;

enum {
    Z_SCOPE_BLOCK,
    Z_SCOPE_LOOP,
    Z_SCOPE_FUNC,
    Z_SCOPE_FILE,
    Z_SCOPE_GLOB
};

typedef struct ZLLVMScope {
    struct ZLLVMScope   *parent;
    ZLLVMSymbol         **symbols;

    /* Capture the start label of the loop (used by the continue statement). */
    LLVMBasicBlockRef   startLoop;

    /* Capture the end label of the loop (used by the break statement). */
    LLVMBasicBlockRef   endLoop;

    /* Capture all stack allocated variables (it is allocated only at function-level). */
    ZLLVMStack          **stackAlloca;

    /* Captures all defer statements of the current block. */
    ZNode               **defers;

    int                 type;
} ZLLVMScope;

typedef struct {
    LLVMContextRef  ctx;

    LLVMModuleRef   *modules;
    LLVMModuleRef   mod;

    LLVMBuilderRef  builder;
    ZSemantic       *semantic;
    ZState          *state;
    ZScope          *current;

    ZLLVMScope      *scope;

    /* Struct type cache — parallel arrays keyed by name */
    char            **structNames;
    LLVMTypeRef     *structTypes;

    LLVMValueRef    currentFunc;

    /* all operations are named with an incremental number
     * and converted to hex format. */
    usize           count;

    /* buffer for operation names for storing the hex number. */
    char            *str;
} ZCodegen;

static void         genStmt         (ZCodegen *, ZNode *);
static LLVMTypeRef  genType         (ZCodegen *, ZType *);
static LLVMValueRef genExpr         (ZCodegen *, ZNode *);
static LLVMValueRef genStructLitInto(ZCodegen *, ZNode *, LLVMValueRef);


/* ========== Native types ==========*/
static LLVMTypeRef i0Type   = NULL;
static LLVMTypeRef i1Type   = NULL;
static LLVMTypeRef i8Type   = NULL;
static LLVMTypeRef i16Type  = NULL;
static LLVMTypeRef i32Type  = NULL;
static LLVMTypeRef i64Type  = NULL;

static LLVMTypeRef f32Type  = NULL;
static LLVMTypeRef f64Type  = NULL;

static ZLLVMSymbol *makesymbol() {
    ZLLVMSymbol *self = zalloc(ZLLVMSymbol);
    return self;
}

static ZLLVMScope *makescope(int type, ZLLVMScope *parent) {
    ZLLVMScope *self    = zalloc(ZLLVMScope);
    self->parent        = parent;
    self->symbols       = NULL;
    self->startLoop     = parent ? parent->startLoop : NULL;
    self->endLoop       = parent ? parent->endLoop : NULL;
    self->stackAlloca   = NULL;
    self->defers        = NULL;
    self->type          = type;
    return self;
}

static void beginScope(int type, ZCodegen *ctx) {
    ctx->scope = makescope(type, ctx->scope);
}

static void endScope(ZCodegen *ctx) {
    if (!ctx->scope) return;
    if (!ctx->scope->parent) return;
    ctx->scope = ctx->scope->parent;
}

static void putLLVMValueRef(ZCodegen *ctx, char *key, LLVMValueRef value) {
    ZLLVMSymbol *symbol = makesymbol();
    symbol->name = key;
    symbol->value = value;
    vecpush(ctx->scope->symbols, symbol);
}

static LLVMValueRef getLLVMValueRef(ZCodegen *ctx, char *key) {
    ZLLVMScope *cur = ctx->scope;
    while (cur) {
        for (usize i = 0;i < veclen(cur->symbols); i++) {
            if (strcmp(cur->symbols[i]->name,  key) == 0) {
                return cur->symbols[i]->value;
            }
        }
        cur = cur->parent;
    }
    return NULL;
}

static void initNativeTypes(ZCodegen *ctx) {
    if (i0Type) return;
    i0Type  = LLVMVoidTypeInContext(ctx->ctx);
    i1Type  = LLVMInt1TypeInContext(ctx->ctx);
    i8Type  = LLVMInt8TypeInContext(ctx->ctx);
    i16Type = LLVMInt16TypeInContext(ctx->ctx);
    i32Type = LLVMInt32TypeInContext(ctx->ctx);
    i64Type = LLVMInt64TypeInContext(ctx->ctx);

    f32Type = LLVMFloatTypeInContext(ctx->ctx);
    f64Type = LLVMDoubleTypeInContext(ctx->ctx);
}

static void beginModule(ZCodegen *ctx, ZNode *node) {
    /* Save the current LLVM module so endModule can restore it.
       For the root module ctx->mod is NULL — create the one module
       that the whole compilation shares.  Imported modules reuse it. */
    LLVMModuleRef prev = ctx->mod;
    if (!ctx->mod) {
        ctx->mod = LLVMModuleCreateWithNameInContext(
            node->module.name, ctx->ctx
        );
    }
    vecpush(ctx->modules, prev);
    ctx->current = node->module.scope;
}

static void endModule(ZCodegen *ctx) {
    if (veclen(ctx->modules) == 0) {
        printf("Invalid call 'endModule'. The stack of modules is empty\n");
        return;
    }
    LLVMModuleRef prev = vecpop(ctx->modules);
    /* Only restore if we actually had a parent module (non-root case).
       For the root the saved value is NULL; keep ctx->mod as-is. */
    if (prev) ctx->mod = prev;
}

ZCodegen *makecodegen(ZState *state, ZSemantic *semantic) {
    ZCodegen *self      = zalloc(ZCodegen);
    self->ctx           = LLVMContextCreate();
    self->builder       = LLVMCreateBuilderInContext(self->ctx);

    self->modules       = NULL;
    self->structNames   = NULL;
    self->structTypes   = NULL;
    self->scope         = makescope(Z_SCOPE_GLOB, NULL);
    self->state         = state;
    self->semantic      = semantic;
    self->count         = 0;
    self->str           = NULL;
    vecunion(self->str, "        ", 9);
    return self;
}

char *label(ZCodegen *ctx) {
    vecsetlen(ctx->str, 0);
    sprintf(ctx->str, "zn%zx", ctx->count);

    ctx->count++;
    return ctx->str;
}

/* Does not consider alignment. */
usize typeSize(ZCodegen *ctx, ZType *type) {
    usize res = 0;
    switch (type->kind) {
    case Z_TYPE_PRIMITIVE:
        switch (type->primitive.token->type) {
        case TOK_VOID:  return 0;
        case TOK_BOOL:
        case TOK_I8:
        case TOK_U8:
        case TOK_CHAR:  return 1;
        case TOK_I16:
        case TOK_U16:   return 2;
        case TOK_I32:
        case TOK_U32:
        case TOK_F32:   return 4;
        case TOK_I64:
        case TOK_U64:
        case TOK_F64:   return 8;
        default:
            error(ctx->state, type->tok, "Unknown type");
            return 0;
        }
    case Z_TYPE_POINTER:
        return 8; /* 64-bit pointer */
    case Z_TYPE_ARRAY:
        return typeSize(ctx, type->array.base) * type->array.size;
    case Z_TYPE_FUNCTION:
        return 8; /* function pointer */

    case Z_TYPE_STRUCT: {
        usize cur = 0;
        for (usize i = 0; i < veclen(type->strct.fields); i++) {
            cur = typeSize(ctx, type->strct.fields[i]->field.type);
            if (cur) res = (res + cur - 1) / cur * cur;
            res += cur;
        }
        return res;
    }

    case Z_TYPE_TUPLE:
        for (usize i = 0; i < veclen(type->tuple); i++) {
            res += typeSize(ctx, type->tuple[i]);
        }
        return res;
    default: return 0;
    }
}

/* Returns the index of the field for a struct.
 * Returns -1 if it does not exist. */
static i32 typeIndex(ZType *strct, char *fieldName) {
    for (usize i = 0; i < veclen(strct->strct.fields); i++) {
        ZNode *field = strct->strct.fields[i];
        if (strcmp(field->field.identifier->str, fieldName) == 0) {
            return i;
        }
    }
    return -1;
}

static LLVMTypeRef getCachedStruct(ZCodegen *ctx, const char *name) {
    for (usize i = 0; i < veclen(ctx->structNames); i++) {
        if (strcmp(ctx->structNames[i], name) == 0) {
            return ctx->structTypes[i];
        }
    }
    return NULL;
}

static void putStructInCache(ZCodegen *ctx, char *name, LLVMTypeRef strct) {
    vecpush(ctx->structNames, name);
    vecpush(ctx->structTypes, strct);
}

static LLVMTypeRef genStruct(ZCodegen *ctx, ZType *strct) {
    LLVMTypeRef structType = LLVMStructCreateNamed(ctx->ctx,
            strct->strct.name->str);

    ZNode **fields = strct->strct.fields;
    usize nfields = veclen(fields);
    LLVMTypeRef *ftypes = znalloc(LLVMTypeRef, nfields ? nfields : 1);

    for (usize i = 0; i < nfields; i++) {

        if (!fields[i]->field.type) {
            error(ctx->state, strct->strct.name, "Type not found");
        }

        ftypes[i] = genType(ctx, fields[i]->field.type);
        if (!ftypes[i]) return NULL;
    }
    LLVMStructSetBody(structType, ftypes, (unsigned)nfields, /*packed=*/0);

    return structType;
}

static LLVMTypeRef genType(ZCodegen *ctx, ZType *type) {
    if (!type) {
        error(ctx->state, NULL, "Invalid 'genType' call");
        return LLVMVoidTypeInContext(ctx->ctx);
    }

    switch (type->kind) {

    case Z_TYPE_PRIMITIVE: {
        const ZToken *name = type->primitive.token;
        switch (name->type) {
        case TOK_VOID:  return i0Type;
        case TOK_BOOL:  return i1Type;
        case TOK_CHAR:
        case TOK_I8:
        case TOK_U8:    return i8Type;
        case TOK_I16:
        case TOK_U16:   return i16Type;
        case TOK_I32:
        case TOK_U32:   return i32Type;
        case TOK_I64:
        case TOK_U64:   return i64Type;
        case TOK_F32:   return f32Type;
        case TOK_F64:   return f64Type;
        default: {
            LLVMTypeRef ref = getCachedStruct(ctx, name->str);
            if (ref) return ref;
            error(ctx->state,
                        type->primitive.token,
                        "unknown primitive type '%s'",
                        name->str);
            return NULL;
         }
        }
    }

    case Z_TYPE_POINTER: {
        ZType *base = type->base;
        if (!base) {
            error(ctx->state, type->tok, "Pointer must have a base type");
            return NULL;
        }
        if (base && base->kind == Z_TYPE_PRIMITIVE &&
            base->primitive.token->type == TOK_VOID) {
            return LLVMPointerType(i8Type, 0);
        }
        return LLVMPointerType(genType(ctx, base), 0);
    }

    case Z_TYPE_ARRAY: {
        LLVMTypeRef base = genType(ctx, type->array.base);
        if (!base) return NULL;
        return LLVMArrayType(base, (unsigned)type->array.size);
    }

    case Z_TYPE_FUNCTION: {
        usize argc = veclen(type->func.args);
        LLVMTypeRef *params = znalloc(LLVMTypeRef, argc ? argc : 1);
        for (usize i = 0; i < argc; i++) {
            params[i] = genType(ctx, type->func.args[i]);
            if (!params[i]) return NULL;
        }
        LLVMTypeRef ret = genType(ctx, type->func.ret);
        return LLVMFunctionType(ret, params, (unsigned)argc, 0);
    }

    case Z_TYPE_STRUCT: {
        const char *name = type->strct.name->str;

        /* Check cache */
        LLVMTypeRef cached = getCachedStruct(ctx, name);
        if (cached) return cached;

        LLVMTypeRef structType = genStruct(ctx, type);
        putStructInCache(ctx, (char *)name, structType);
        return structType;
    }

    case Z_TYPE_TUPLE: {
        usize len = veclen(type->tuple);
        LLVMTypeRef *elems = znalloc(LLVMTypeRef, len ? len : 1);
        for (usize i = 0; i < len; i++) {
            elems[i] = genType(ctx, type->tuple[i]);
            if (!elems[i]) return NULL;
        }
        return LLVMStructTypeInContext(ctx->ctx, elems, len, /*packed=*/ 0);
    }

    case Z_TYPE_NONE:
        /* none literal — represent as i8* null */
        return LLVMPointerType(i8Type, 0);

    default:
        error(ctx->state, NULL, "genType: unhandled type kind %d", type->kind);
        return NULL;
    }
}

static LLVMBasicBlockRef makeblock(ZCodegen *ctx) {
    return LLVMAppendBasicBlockInContext(
        ctx->ctx, ctx->currentFunc, label(ctx)
    );
}

static void genChainDefer(ZCodegen *ctx, ZLLVMScope *scope) {
    if (LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) return;

    ZLLVMScope *cur = ctx->scope;

    while (cur != scope) {
        for (int i = (int)veclen(cur->defers) - 1; i >= 0; i--) {
            genStmt(ctx, cur->defers[i]);
        }
        cur = cur->parent;
    }
}

static LLVMValueRef genLit(ZCodegen *ctx, ZNode *node) {
    ZToken *tok = node->tok;
    switch (tok->type) {
    case TOK_STR_LIT:
        return LLVMBuildGlobalStringPtr(ctx->builder, tok->str, label(ctx));
    case TOK_INT_LIT:
        return LLVMConstInt(i32Type, tok->integer, true);
    case TOK_TRUE:
        return LLVMConstInt(i1Type, true, false);
    case TOK_FALSE:
        return LLVMConstInt(i1Type, false, false);
    case TOK_FLOAT_LIT:
        return LLVMConstReal(f64Type, tok->floating);
    case TOK_NONE: {
        LLVMTypeRef type = genType(ctx, node->resolved);
        return LLVMConstPointerNull(type);
    }
    default: return NULL;
    }
}

static LLVMValueRef genIdent(ZCodegen *ctx, ZNode *node) {
    if (!node) {
        error(ctx->state, NULL, "'genIdent' called with a null node");
        return NULL;
    } else if (!node->tok) {
        error(ctx->state, NULL,
                "'genIdent' called with a null token on node %d", node->type);
    }

    char *key = node->identNode.mangled ? node->identNode.mangled : node->tok->str;
    LLVMValueRef val = getLLVMValueRef(ctx, key);
    if (!val) {
        error(ctx->state, node->tok, "'%s' not found in the current scope", node->tok->str);
        return NULL;
    }
    /* Local variables are stored as allocas — load to get the value.
       Functions are stored directly — return as-is. */
    if (LLVMGetValueKind(val) == LLVMInstructionValueKind) {
        LLVMTypeRef type = genType(ctx, node->resolved);
        return LLVMBuildLoad2(ctx->builder, type, val, node->tok->str);
    }
    return val;
}

static ZLLVMStack *getStackValue(ZCodegen *ctx, ZNode *key) {
    ZLLVMScope *scope = ctx->scope;
    ZLLVMStack **stack = NULL;

    while (scope) {
        stack = scope->stackAlloca;
        for (usize i = 0; i < veclen(stack); i++) {
            if (stack[i]->node == key) {
                return stack[i];
            }
        }
        scope = scope->parent;
    }
    return NULL;
}

static LLVMValueRef castValue(ZCodegen *ctx, LLVMValueRef val, ZType *from, ZType *to);

static bool fitsInRegister(LLVMValueRef val) {
    LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(val));

    return (
        kind == LLVMPointerTypeKind || 
        kind == LLVMIntegerTypeKind ||
        kind == LLVMFloatTypeKind   ||
        kind == LLVMDoubleTypeKind  );
}

static LLVMValueRef genVarDecl(ZCodegen *ctx, ZNode *node) {
    if (!node->varDecl.rvalue) return NULL;

    ZLLVMStack *stack = getStackValue(ctx, node->varDecl.rvalue);
    if (!stack) {
        printf("Stack allocation not found\n");
        error(ctx->state, node->tok, "Missing stack allocation for '%s'", node->tok->str);
        return NULL;
    }

    LLVMValueRef val = genExpr(ctx, node->varDecl.rvalue);

    /* If the type does not fit in a register (like an array or a struct)
     * genExpr stores the value directly. This check is necessary
     * to avoid store the value twice.
    */
    /* Skip the store if genExpr already wrote the value in-place (e.g. struct/array
     * literals return the pre-allocated slot pointer — storing it would self-overwrite).
     * For register-sized values, cast to the variable's declared type first. */
    if (val && val != stack->stack && (!node->resolved || fitsInRegister(val))) {
        val = castValue(ctx, val, node->varDecl.rvalue->resolved, node->resolved);
        LLVMBuildStore(ctx->builder, val, stack->stack);
    }

    putLLVMValueRef(ctx, node->tok->str, stack->stack);
    return stack->stack;
}

static LLVMValueRef genCall(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef func;
    LLVMValueRef *args = NULL;
    ZNode *callee = node->call.callee;

    bool isRecFunc = callee->type == NODE_MEMBER;
    if (isRecFunc) {
        if (!callee->memberAccess.mangled) {
            error(ctx->state, callee->memberAccess.object->tok,
                    "Mangling name not found");
        }

        func = getLLVMValueRef(ctx, callee->memberAccess.mangled);

        if (!func) error(ctx->state,
                    callee->tok,
                    "Receiver function '%s' not found",
                    callee->memberAccess.mangled);

        // Prepend 'self' as the first argument
        LLVMValueRef self = genExpr(ctx, callee->memberAccess.object);
        vecpush(args, self);

    } else {
        func = genExpr(ctx, callee);
    }
    LLVMTypeRef funcType = LLVMGlobalGetValueType(func);


    for (usize i = 0; i < veclen(node->call.args); i++) {
        LLVMValueRef arg = genExpr(ctx, node->call.args[i]);
        vecpush(args, arg);
    }

    LLVMValueRef call = LLVMBuildCall2(
        ctx->builder,
        funcType,
        func,
        args,
        veclen(args),
        isVoid(node->resolved) ? "" : label(ctx)
    );

    if (!fitsInRegister(call)) {
        ZLLVMStack *stack = getStackValue(ctx, node);
        if (stack) {
            LLVMBuildStore(ctx->builder, call, stack->stack);
        }
    }
    return call;
}

/* genLvalue is used to load the address of the expression rather than the value.
 * meanwhile the function to load the value is genExpr.
 * */
static LLVMValueRef genLvalue(ZCodegen *ctx, ZNode *node) {
    switch (node->type) {
    case NODE_IDENTIFIER: {
        char *key = node->identNode.mangled ?
                    node->identNode.mangled :
                    node->tok->str;
        LLVMValueRef val = getLLVMValueRef(ctx, key);
        if (!val) {
            error(ctx->state, node->tok,
                    "'%s' not found in the current scope",
                    node->tok->str);
            return NULL;
        }
        return val;
    }
    case NODE_SUBSCRIPT: {
        LLVMValueRef ptr        = genLvalue(ctx, node->subscript.arr);
        LLVMValueRef i          = genExpr(ctx, node->subscript.index);
        ZType *arrType          = node->subscript.arr->resolved;
        LLVMTypeRef type        = genType(ctx, arrType);
        const char *name        = label(ctx);
        
        if (arrType->kind == Z_TYPE_POINTER) {
            LLVMTypeRef elemType = genType(ctx, arrType->base);
            LLVMValueRef loaded = LLVMBuildLoad2(
                ctx->builder,   type,
                ptr,            name
            );
            return LLVMBuildGEP2(
                ctx->builder,   elemType,
                loaded,         &i,
                1,              name
            );
        }

        LLVMValueRef indices[]  = {
            LLVMConstInt(i64Type, 0, false),
            i
        };
        return LLVMBuildGEP2(
            ctx->builder,
            type, ptr,
            indices, 2,
            label(ctx)
        );
    }
    case NODE_MEMBER: {
        ZType *objType = node->memberAccess.object->resolved;
        ZToken *tok = node->memberAccess.field;
        i32 index = -1;

        if (objType->kind == Z_TYPE_STRUCT) {
            index = typeIndex(objType, tok->str);
        } else if (objType->kind == Z_TYPE_TUPLE) {
            index = tok->integer;
        }

        if (index == -1) {
            error(ctx->state, tok, "'%s' member not found", tok->str);
            return NULL;
        }

        LLVMTypeRef strctType   = genType(ctx, objType);
        LLVMValueRef objPtr     = genLvalue(ctx, node->memberAccess.object);

        return LLVMBuildStructGEP2(
            ctx->builder,
            strctType,
            objPtr,
            (u32)index,
            label(ctx)
        );

    }

    case NODE_STRUCT_LIT: {
        ZLLVMStack *stack = getStackValue(ctx, node);
        if (!stack) {
            error(ctx->state, node->tok, "Missing stack value");
            return NULL;
        }
        genStructLitInto(ctx, node, stack->stack);
        return stack->stack;
    }
    case NODE_UNARY: {
        if (node->unary.operat->type != TOK_STAR) {
            error(ctx->state, node->tok, "Unhandled unary operator");
            return NULL;
        }

        LLVMTypeRef type = genType(ctx, node->resolved);
        LLVMValueRef ptr = genLvalue(ctx, node->unary.operand);
        return LLVMBuildLoad2(ctx->builder, type, ptr, label(ctx));
    }
    default:
        error(ctx->state,
                node->tok,
                "Node '%d' not handled in 'genLvalue'",
                node->type);
        return NULL;
    }
}

static LLVMValueRef genBinary(ZCodegen *ctx, ZNode *root) {
    if (root->binary.op->type == TOK_EQ) {
        LLVMValueRef ptr = genLvalue(ctx, root->binary.left);
        LLVMValueRef val = genExpr(ctx, root->binary.right);
        if (!ptr || !val) return NULL;
        return LLVMBuildStore(ctx->builder, val, ptr);
    }

    ZTokenType op = root->binary.op->type;
    /* Logical operator. */
    if (op == TOK_AND || op == TOK_SAND ||
        op == TOK_OR  || op == TOK_SOR) {
        bool is_and = (op == TOK_AND || op == TOK_SAND);

        LLVMValueRef lv = genExpr(ctx, root->binary.left);
        if (!lv) return NULL;
        LLVMValueRef lv_bool = LLVMBuildICmp(ctx->builder, LLVMIntNE, lv,
            LLVMConstInt(LLVMTypeOf(lv), 0, false), label(ctx));

        LLVMBasicBlockRef entry_bb = LLVMGetInsertBlock(ctx->builder);
        LLVMBasicBlockRef rhs_bb   = makeblock(ctx);
        LLVMBasicBlockRef merge_bb = makeblock(ctx);

        if (is_and)
            LLVMBuildCondBr(ctx->builder, lv_bool, rhs_bb, merge_bb);
        else
            LLVMBuildCondBr(ctx->builder, lv_bool, merge_bb, rhs_bb);

        LLVMPositionBuilderAtEnd(ctx->builder, rhs_bb);
        LLVMValueRef rv = genExpr(ctx, root->binary.right);
        if (!rv) return NULL;
        LLVMValueRef rv_bool = LLVMBuildICmp(ctx->builder, LLVMIntNE, rv,
            LLVMConstInt(LLVMTypeOf(rv), 0, false), label(ctx));
        LLVMBuildBr(ctx->builder, merge_bb);
        LLVMBasicBlockRef rhs_end = LLVMGetInsertBlock(ctx->builder);

        LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(ctx->builder, i1Type, label(ctx));
        LLVMValueRef short_val = LLVMConstInt(i1Type, is_and ? 0 : 1, false);
        LLVMValueRef  vals[2]   = {short_val, rv_bool};
        LLVMBasicBlockRef bbs[2] = {entry_bb, rhs_end};
        LLVMAddIncoming(phi, vals, bbs, 2);
        return phi;
    }

    LLVMValueRef left = genExpr(ctx, root->binary.left);
    LLVMValueRef right = genExpr(ctx, root->binary.right);

    if (!left || !right) return NULL;

    LLVMTypeRef left_type = LLVMTypeOf(left);
    bool is_float = (LLVMGetTypeKind(left_type) == LLVMFloatTypeKind ||
                     LLVMGetTypeKind(left_type) == LLVMDoubleTypeKind);


    bool bothUnsigned = true;

    char *l = label(ctx);

    if (is_float) {
        switch (op) {
        case TOK_PLUS:  return LLVMBuildFAdd(ctx->builder, left, right, l);
        case TOK_MINUS: return LLVMBuildFSub(ctx->builder, left, right, l);
        case TOK_STAR:  return LLVMBuildFMul(ctx->builder, left, right, l);
        case TOK_DIV:   return LLVMBuildFDiv(ctx->builder, left, right, l);
        case TOK_MOD:   return LLVMBuildFRem(ctx->builder, left, right, l);
        case TOK_LT:    return LLVMBuildFCmp(ctx->builder, LLVMRealOLT, left, right, l);
        case TOK_GT:    return LLVMBuildFCmp(ctx->builder, LLVMRealOGT, left, right, l);
        case TOK_LTE:   return LLVMBuildFCmp(ctx->builder, LLVMRealOLE, left, right, l);
        case TOK_GTE:   return LLVMBuildFCmp(ctx->builder, LLVMRealOGE, left, right, l);
        case TOK_EQEQ:  return LLVMBuildFCmp(ctx->builder, LLVMRealOEQ, left, right, l);
        case TOK_NOTEQ: return LLVMBuildFCmp(ctx->builder, LLVMRealONE, left, right, l);
        default:        error(ctx->state, root->tok, "Unknown binary operator"); return NULL;
        }
    } else if (bothUnsigned) {

    }


    let div = bothUnsigned ? LLVMBuildUDiv : LLVMBuildSDiv;
    let mod = bothUnsigned ? LLVMBuildURem : LLVMBuildSRem;

    switch (op) {
    case TOK_PLUS:  return LLVMBuildAdd (ctx->builder, left, right, l);
    case TOK_MINUS: return LLVMBuildSub (ctx->builder, left, right, l);
    case TOK_STAR:  return LLVMBuildMul (ctx->builder, left, right, l);
    case TOK_DIV:   return div          (ctx->builder, left, right, l);
    case TOK_MOD:   return mod          (ctx->builder, left, right, l);
    case TOK_LT:    return LLVMBuildICmp(ctx->builder, LLVMIntSLT, left, right, l);
    case TOK_GT:    return LLVMBuildICmp(ctx->builder, LLVMIntSGT, left, right, l);
    case TOK_LTE:   return LLVMBuildICmp(ctx->builder, LLVMIntSLE, left, right, l);
    case TOK_GTE:   return LLVMBuildICmp(ctx->builder, LLVMIntSGE, left, right, l);
    case TOK_EQEQ:  return LLVMBuildICmp(ctx->builder, LLVMIntEQ, left, right, l);
    case TOK_NOTEQ: return LLVMBuildICmp(ctx->builder, LLVMIntNE, left, right, l);

    default:        error(ctx->state, root->tok, "Unknown binary operator"); return NULL;
    }
}

static LLVMValueRef genRef(ZCodegen *ctx, ZNode *node, LLVMValueRef val) {
    (void)val;
    error(ctx->state, node->tok, "Reference not yet implemented");
    return NULL;
}

static LLVMValueRef genUnary(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef arg = genExpr(ctx, node->unary.operand);
    ZTokenType op = node->unary.operat->type;

    LLVMTypeRef argType = LLVMTypeOf(arg);
    bool isFloat = (LLVMGetTypeKind(argType) == LLVMFloatTypeKind ||
                     LLVMGetTypeKind(argType) == LLVMDoubleTypeKind);

    char *l = label(ctx);
    switch (op) {
    case TOK_PLUS:
    case TOK_MINUS: {
        typedef LLVMValueRef (*LLVMBinary)(LLVMBuilderRef, LLVMValueRef, LLVMValueRef, const char *);

        LLVMBinary arr[][2] = {
            {LLVMBuildAdd, LLVMBuildFAdd},
            {LLVMBuildSub, LLVMBuildFSub}
        };
        LLVMValueRef zero =  isFloat ?
            LLVMConstReal   (argType, 0) :
            LLVMConstInt    (argType, 0, 0);

        return arr[op == TOK_MINUS][isFloat](ctx->builder, zero, arg, l);
    }
    case TOK_STAR: {
        LLVMTypeRef base = genType(ctx, node->resolved);
        return LLVMBuildLoad2(ctx->builder, base, arg, l);
    }
    case TOK_SNOT:
    case TOK_NOT:   return LLVMBuildNot(ctx->builder, arg, l);
    case TOK_REF:   return genRef(ctx, node, arg);
    default:
        error(ctx->state, node->unary.operat, "Unknown binary operator");
        return NULL;
    }

    return NULL;
}

/* Emit the appropriate LLVM cast to convert val (of Zinc type `from`) to
   Zinc type `to`. Returns val unchanged when the types are already equal. */
static LLVMValueRef castValue(ZCodegen *ctx, LLVMValueRef val, ZType *from, ZType *to) {
    if (!val || !from || !to) return val;
    if (typesEqual(from, to)) return val;

    /* none (null pointer) is compatible with any pointer — val is already ptr null */
    if (from->kind == Z_TYPE_NONE && to->kind == Z_TYPE_POINTER)
        return val;

    LLVMTypeRef toType = genType(ctx, to);
    if (!toType) return val;

    bool fromIsFloat = false, toIsFloat = false;
    bool fromIsPtr   = (from->kind == Z_TYPE_POINTER);
    bool toIsPtr     = (to->kind   == Z_TYPE_POINTER);
    bool fromIsSigned = false;

    if (from->kind == Z_TYPE_PRIMITIVE) {
        ZTokenType tt = from->primitive.token->type;
        fromIsFloat   = (tt == TOK_F32 || tt == TOK_F64);
        fromIsSigned  = (tt & TOK_SIGNED) != 0;
    }
    if (to->kind == Z_TYPE_PRIMITIVE) {
        ZTokenType tt = to->primitive.token->type;
        toIsFloat = (tt == TOK_F32 || tt == TOK_F64);
    }

    char *l = label(ctx);
    unsigned toBits = LLVMGetIntTypeWidth(toType);

    if (LLVMGetTypeKind(toType) == LLVMIntegerTypeKind && toBits == 1) {
        LLVMValueRef zero;
        if (fromIsPtr) {
            zero = LLVMConstNull(LLVMTypeOf(val));
            return LLVMBuildICmp(ctx->builder, LLVMIntNE, val, zero, l);
        }
        if (fromIsFloat) {
            zero = LLVMConstReal(LLVMTypeOf(val), 0.0);
            return LLVMBuildFCmp(ctx->builder, LLVMRealONE, val, zero, l);
        }
        zero = LLVMConstInt(LLVMTypeOf(val), 0, false);
        return LLVMBuildICmp(ctx->builder, LLVMIntNE, val, zero, l);
    }

    if (fromIsPtr && toIsPtr)
        return LLVMBuildBitCast(ctx->builder, val, toType, l);

    if (fromIsPtr && !toIsFloat)
        return LLVMBuildPtrToInt(ctx->builder, val, toType, l);

    if (!fromIsFloat && toIsPtr)
        return LLVMBuildIntToPtr(ctx->builder, val, toType, l);

    if (fromIsFloat && toIsFloat) {
        if (from->primitive.token->type == TOK_F64 &&
            to->primitive.token->type   == TOK_F32)
            return LLVMBuildFPTrunc(ctx->builder, val, toType, l);
        return LLVMBuildFPExt(ctx->builder, val, toType, l);
    }

    if (fromIsFloat)
        return fromIsSigned
            ? LLVMBuildFPToSI(ctx->builder, val, toType, l)
            : LLVMBuildFPToUI(ctx->builder, val, toType, l);

    if (toIsFloat)
        return fromIsSigned
            ? LLVMBuildSIToFP(ctx->builder, val, toType, l)
            : LLVMBuildUIToFP(ctx->builder, val, toType, l);

    /* Both integers: trunc or extend */
    LLVMTypeRef fromType = LLVMTypeOf(val);
    u32 fromBits = LLVMGetIntTypeWidth(fromType);
    if (fromBits > toBits)
        return LLVMBuildTrunc(ctx->builder, val, toType, l);
    if (fromBits < toBits)
        return fromIsSigned
            ? LLVMBuildSExt(ctx->builder, val, toType, l)
            : LLVMBuildZExt(ctx->builder, val, toType, l);

    return LLVMBuildBitCast(ctx->builder, val, toType, l);
}

static LLVMValueRef genCast(ZCodegen *ctx, ZNode *node) {
    ZType *from = node->castExpr.expr->resolved;
    ZType *to   = node->castExpr.toType;

    /* Array-literal cast: [n]T as []U — write each element directly into
     * the pre-allocated slot with per-element casting.
     * genArrayLit can't be used here because the stack slot is keyed on
     * this cast node, not on the inner array-literal node. */
    if (from->kind == Z_TYPE_ARRAY && to->kind == Z_TYPE_ARRAY &&
        node->castExpr.expr->type == NODE_ARRAY_LIT) {
        ZLLVMStack *stack = getStackValue(ctx, node);
        if (!stack) {
            error(ctx->state, node->tok, "Missing stack value for array cast");
            return NULL;
        }
        ZNode *lit = node->castExpr.expr;
        for (usize i = 0; i < veclen(lit->arraylit); i++) {
            LLVMValueRef indices[] = {
                LLVMConstInt(i32Type, 0, false),
                LLVMConstInt(i32Type, i, false)
            };
            LLVMValueRef gep = LLVMBuildGEP2(
                ctx->builder, stack->type,
                stack->stack, indices, 2, label(ctx));
            LLVMValueRef elem = genExpr(ctx, lit->arraylit[i]);
            elem = castValue(ctx, elem, from->array.base, to->array.base);
            LLVMBuildStore(ctx->builder, elem, gep);
        }
        return stack->stack;
    }

    LLVMValueRef val = genExpr(ctx, node->castExpr.expr);
    return castValue(ctx, val, from, to);
}

/* dest: optional pre-allocated slot to write into (e.g. an array element GEP).
   If NULL, a fresh alloca is emitted. Returns the destination pointer. */
static LLVMValueRef genStructLitInto(ZCodegen *ctx, ZNode *node, LLVMValueRef dest) {
    LLVMTypeRef structType = genType(ctx, node->resolved);

    LLVMValueRef ptr = dest ? dest : LLVMBuildAlloca(ctx->builder, structType, label(ctx));

    ZNode **fields = node->structlit.fields;

    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *var = fields[i];
        ZToken *name = var->varDecl.ident->identNode.tok;
        if (!var->varDecl.rvalue) {
            error(ctx->state,
                    var->tok,
                    "Missing rvalue in struct literal for field '%s'",
                    var->varDecl.ident->identNode.tok);
        }
        LLVMValueRef val = genExpr(ctx, var->varDecl.rvalue);

        i32 index = typeIndex(node->resolved, name->str);

        if (index == -1) {
            error(ctx->state, name, "Unknown field '%s'", name->str);
            continue;
        }

        ZType *fieldType = node->resolved->strct.fields[index]->field.type;
        val = castValue(ctx, val, var->varDecl.rvalue->resolved, fieldType);

        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, structType,
            ptr, index, name->str
        );

        LLVMBuildStore(ctx->builder, val, fieldPtr);
    }
    return ptr;
}

static LLVMValueRef genStructLit(
        ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);
    if (!stack) {
        error(ctx->state, node->tok, "Stack value not found");
        return NULL;
    }
    LLVMValueRef val = genStructLitInto(ctx, node, stack->stack);
    return LLVMBuildLoad2(ctx->builder,
        stack->type,
        val,
        label(ctx)
    );
}

static LLVMValueRef genTupleLitInto(ZCodegen *ctx, ZNode *node, LLVMValueRef dest) {
    LLVMTypeRef type = genType(ctx, node->resolved);
    const char *name = label(ctx);

    LLVMTypeRef tupleType = genType(ctx, node->resolved);
    LLVMValueRef ptr = dest ? dest : LLVMBuildAlloca(ctx->builder, type, name);
    LLVMValueRef fieldPtr, val;

    usize len = veclen(node->tuplelit);
    for (usize i = 0; i < len; i++) {
        val = genExpr(ctx, node->tuplelit[i]);

        fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, tupleType,
            ptr, i, label(ctx)
        );

        LLVMBuildStore(ctx->builder, val, fieldPtr);
    }
    return ptr;
}

static LLVMValueRef genTupleLit(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);

    if (!stack) {
        error(ctx->state, node->tok, "Stack allocation not found");
    }

    LLVMValueRef val = genTupleLitInto(ctx, node, stack->stack);
    return LLVMBuildLoad2(
        ctx->builder,
        stack->type,
        val,
        label(ctx)
    );
}

static LLVMValueRef genArrayLit(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);

    if (!stack) {
        error(ctx->state, node->tok, "Missing stack value %p", node);
        return NULL;
    }

    LLVMTypeRef elemType = genType(ctx, node->resolved->array.base);

    for (usize i = 0; i < veclen(node->arraylit); i++) {
        LLVMValueRef indices[] = {
            LLVMConstInt(i32Type, 0, false),
            LLVMConstInt(i32Type, i, false)
        };
        LLVMValueRef gep = LLVMBuildGEP2(
            ctx->builder,   stack->type,
            stack->stack,   indices,
            2,              label(ctx)
        );

        ZNode *elem = node->arraylit[i];
        LLVMValueRef val = genExpr(ctx, elem);
        if (!val) {
            error(ctx->state, elem->tok, "Array element could not be compiled");
            return NULL;
        }
        /* A call returning a struct comes back as an aggregate value,
           not a pointer — store it directly. */
        if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMPointerTypeKind &&
            node->resolved->array.base->kind == Z_TYPE_STRUCT) {
            val = LLVMBuildLoad2(ctx->builder, elemType, val, label(ctx));
        }
        LLVMBuildStore(ctx->builder, val, gep);
    }
    return NULL;
}

static LLVMValueRef genSubscript(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef base = genLvalue(ctx, node);
    ZType *arrType = node->subscript.arr->resolved;
    
    ZType *baseType = arrType->kind == Z_TYPE_ARRAY ?
        arrType->array.base :
        arrType->base;
    LLVMTypeRef elemType = genType(ctx, baseType);

    return LLVMBuildLoad2(
        ctx->builder,   elemType,
        base,           label(ctx)
    );
}

static LLVMValueRef genStaticAccess(ZCodegen *ctx, ZNode *node) {
    char *mangled = node->staticAccess.mangled;

    if (!mangled) {
        error(ctx->state, node->tok, "Mangled name not saved");
    }

    LLVMValueRef val = getLLVMValueRef(ctx, mangled);

    if (!val) {
        error(ctx->state, node->tok, "Unknown name '%s'", mangled);
        return NULL;
    }
    return val;
}

static LLVMValueRef genMemberAccess(ZCodegen *ctx, ZNode *node) {

    ZNode *obj = node->memberAccess.object;

    LLVMValueRef ptr = genLvalue(ctx, obj);
    LLVMTypeRef objType = genType(ctx, obj->resolved);

    ZToken *field = node->memberAccess.field;

    int index = -1;
    if (obj->resolved->kind == Z_TYPE_STRUCT) {
        ZNode **fields = obj->resolved->strct.fields;
        for (usize i = 0; i < veclen(fields) && index == -1; i++) {
            if (tokeneq(fields[i]->field.identifier, field)) {
                index = i;
            }
        }
    } else if (obj->resolved->kind == Z_TYPE_TUPLE) {
        index = field->integer;
    }
    

    if (index == -1) {
        error(ctx->state, field, "'%s' member not found", field->str);
        return NULL;
    }
    
    ptr = LLVMBuildStructGEP2(
        ctx->builder,   objType,
        ptr,            (u32)index,
        label(ctx)
    );
    if (!node->resolved) {
        error(ctx->state, field, "Type not resolved");
    }
    LLVMTypeRef fieldType = genType(ctx, node->resolved);
    return LLVMBuildLoad2(
        ctx->builder,   fieldType,
        ptr,            label(ctx) 
    );
}

static LLVMValueRef genArrayInit(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);

    if (!stack) return NULL;

    return stack->stack;
}

static LLVMValueRef genExpr(ZCodegen *ctx, ZNode *node) {
    switch (node->type) {
        case NODE_STRUCT_LIT:       return genStructLit     (ctx, node);
        case NODE_ARRAY_LIT:        return genArrayLit      (ctx, node);
        case NODE_TUPLE_LIT:        return genTupleLit      (ctx, node);
        case NODE_LITERAL:          return genLit           (ctx, node);
        case NODE_IDENTIFIER:       return genIdent         (ctx, node);
        case NODE_CALL:             return genCall          (ctx, node);
        case NODE_SUBSCRIPT:        return genSubscript     (ctx, node);
        case NODE_CAST:             return genCast          (ctx, node);
        case NODE_STATIC_ACCESS:    return genStaticAccess  (ctx, node);
        case NODE_MEMBER:           return genMemberAccess  (ctx, node);
        case NODE_BINARY:           return genBinary        (ctx, node);
        case NODE_UNARY:            return genUnary         (ctx, node);
        case NODE_ARRAY_INIT:       return genArrayInit     (ctx, node);

        case NODE_SIZEOF: {
            usize size = typeSize(ctx, node->sizeofExpr.type);
            return LLVMConstInt(i64Type, (u64)size, /*sign_extend=*/0);
        }


        default: 
            printf("Node '%d' not handled\n", node->type);
            error(ctx->state,
                    node->tok,
                    "Node '%d' not yet implemented in the code generator",
                    node->type);
            break;
    }
    return NULL;
}

static LLVMValueRef genForeign(ZCodegen *ctx, ZNode *node) {
    LLVMTypeRef ret = genType(ctx, node->foreignFunc.ret);
    usize argc = veclen(node->foreignFunc.args);

    LLVMTypeRef *paramTypes = znalloc(LLVMTypeRef, argc ? argc : 1);
    for (usize i = 0; i < argc; i++) {
        paramTypes[i] = genType(ctx, node->foreignFunc.args[i]);
    }
    LLVMTypeRef funcType = LLVMFunctionType(ret, paramTypes, (unsigned)argc, 0);

    LLVMValueRef func = LLVMAddFunction(
        ctx->mod,
        node->foreignFunc.tok->str,
        funcType
    );

    putLLVMValueRef(ctx, node->foreignFunc.tok->str, func);

    return func;
}

static void genRetChainDefer(ZCodegen *ctx) {
    ZLLVMScope *scope = ctx->scope;

    while (scope && scope->type != Z_SCOPE_FUNC) {
        scope = scope->parent;
    }

    if (scope) scope = scope->parent;

    genChainDefer(ctx, scope);
}

static LLVMValueRef genRet(ZCodegen *ctx, ZNode *ret) {
    if (!ret->returnStmt.expr) {
        genRetChainDefer(ctx);
        return LLVMBuildRetVoid(ctx->builder);
    }

    LLVMValueRef val = genExpr(ctx, ret->returnStmt.expr);

    /* If the expression produced i1 (e.g. a comparison) but the
       function's declared return type is a wider integer, zero-extend. */
    LLVMTypeRef funcType = LLVMGlobalGetValueType(ctx->currentFunc);
    LLVMTypeRef retType  = LLVMGetReturnType(funcType);
    if (LLVMTypeOf(val) == i1Type && retType != i1Type &&
            LLVMGetTypeKind(retType) == LLVMIntegerTypeKind) {
        val = LLVMBuildZExt(ctx->builder, val, retType, label(ctx));
    }

    genRetChainDefer(ctx);

    return LLVMBuildRet(ctx->builder, val);
}

static void genIf(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_BLOCK, ctx);
    bool hasElse = node->ifStmt.elseBranch != NULL;
    LLVMBasicBlockRef then = makeblock(ctx);

    LLVMBasicBlockRef elseBranch = NULL;

    if (hasElse) {
        elseBranch = makeblock(ctx);
    }

    LLVMBasicBlockRef endif = makeblock(ctx);
    LLVMBasicBlockRef nextBlock = hasElse ? elseBranch : endif;

    LLVMValueRef cond = genExpr(ctx, node->ifStmt.cond);

    LLVMBuildCondBr(ctx->builder, cond, then, nextBlock);

    LLVMPositionBuilderAtEnd(ctx->builder, then);
    genStmt(ctx, node->ifStmt.body);

    genChainDefer(ctx, ctx->scope->parent);

    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
        LLVMBuildBr(ctx->builder, endif);
    }

    if (hasElse) {
        LLVMPositionBuilderAtEnd(ctx->builder, elseBranch);
        genStmt(ctx, node->ifStmt.elseBranch);

        genChainDefer(ctx, ctx->scope->parent);
        
        if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
            LLVMBuildBr(ctx->builder, endif);
        }
    }

    LLVMPositionBuilderAtEnd(ctx->builder, endif);
    endScope(ctx);
}

static void genWhile(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_LOOP, ctx);
    LLVMBasicBlockRef entry     = makeblock(ctx);
    LLVMBasicBlockRef block     = makeblock(ctx);
    LLVMBasicBlockRef endwhile  = makeblock(ctx);
    ctx->scope->startLoop       = entry;
    ctx->scope->endLoop         = endwhile;

    LLVMBuildBr             (ctx->builder, entry);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef cond = genExpr(ctx, node->whileStmt.cond);
    LLVMBuildCondBr(ctx->builder, cond, block, endwhile);

    LLVMPositionBuilderAtEnd(ctx->builder, block);

    // Build block
    genStmt(ctx, node->whileStmt.branch);

    // Fire all defer statements
    genChainDefer(ctx, ctx->scope->parent);

    /* If the block contains a break or a continue this block is already terminated.*/
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
        LLVMBuildBr(ctx->builder, entry);
    }

    LLVMPositionBuilderAtEnd(ctx->builder, endwhile);

    endScope(ctx);
}

static void genFor(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_LOOP, ctx);
    LLVMBasicBlockRef entry     = makeblock(ctx);
    LLVMBasicBlockRef body      = makeblock(ctx);
    LLVMBasicBlockRef endfor    = makeblock(ctx);

    /* Save the labels for the continue and break statement. */
    ctx->scope->startLoop       = entry;
    ctx->scope->endLoop         = endfor;

    genVarDecl(ctx, node->forStmt.var);

    LLVMBuildBr             (ctx->builder, entry);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    LLVMValueRef cond = genExpr(ctx, node->forStmt.cond);
    LLVMBuildCondBr(ctx->builder, cond, body, endfor);

    LLVMPositionBuilderAtEnd(ctx->builder, body);
    genStmt(ctx, node->forStmt.block);
    genStmt(ctx, node->forStmt.incr);

    // Fire all defer statements
    genChainDefer(ctx, ctx->scope->parent);

    /* If the block contains a break or a continue this block is already terminated.*/
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
        LLVMBuildBr(ctx->builder, entry);
    }

    LLVMPositionBuilderAtEnd(ctx->builder, endfor);
    endScope(ctx);
}

static void genBlock(ZCodegen *ctx, ZNode *block) {
    for (usize i = 0; i < veclen(block->block); i++) {
        genStmt(ctx, block->block[i]);
    }
}

static void genBreak(ZCodegen *ctx, ZNode *node) {
    if (!ctx->scope->endLoop) {
        error(ctx->state, node->tok, "'break' statement is not in a loop");
        return;
    }

    ZLLVMScope *scope = ctx->scope;
    while (scope && scope->type != Z_SCOPE_LOOP) {
        scope = scope->parent;
    }

    genChainDefer(ctx, scope);

    /* If the block contains a break or a continue this block is already terminated.*/
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
        LLVMBuildBr(ctx->builder, ctx->scope->endLoop);
    }
}

static void genContinue(ZCodegen *ctx, ZNode *node) {
    if (!ctx->scope->startLoop) {
        error(ctx->state, node->tok, "'continue' statement is not in a loop");
        return;
    }

    ZLLVMScope *scope = ctx->scope;
    while (scope && scope->type != Z_SCOPE_LOOP) {
        scope = scope->parent;
    }

    genChainDefer(ctx, scope);

    /* If the block contains a break or a continue this block is already terminated.*/
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
        LLVMBuildBr(ctx->builder, ctx->scope->startLoop);
    }
}

/* Defer statement is compiled using a per-scope stack.
 *
 * At compile-time, every scope (function body, if, loops ...)
 * owns its own defer stack. Each defer statement is pushed into the stack
 * if its enclosing scope.
 * At the end of the scope each defer statement is fired in reverse order.
 *
 * A special case is an early exit (break, continue or return). inside a nested
 * scope: The compiler walks up the chain from the current scope to the target scope,
 * emitting each defer stack in reverse order.
 *
 * Note: Since the stack is known at compile-time the defer statement
 * has zero overhead at runtime.
 *
 * Note: One defer statement can be duplicated depending on its 'destination'.
 * for example a defer inside a loop is compiled:
 * - one for the exit block.
 * - one for every break statement
 * - one for every continue statement
 * */
static void genDefer(ZCodegen *ctx, ZNode *node) {
    vecpush(ctx->scope->defers, node->deferStmt.expr);
}

static void genStmt(ZCodegen *ctx, ZNode *stmt) {
    if (!stmt) return;
    switch (stmt->type) {
    /* Variable already declared at the start of the function*/
    case NODE_VAR_DECL: genVarDecl  (ctx, stmt);    break;
    case NODE_RETURN:   genRet      (ctx, stmt);    break;
    case NODE_CALL:     genCall     (ctx, stmt);    break;
    case NODE_IF:       genIf       (ctx, stmt);    break;
    case NODE_BLOCK:    genBlock    (ctx, stmt);    break;
    case NODE_WHILE:    genWhile    (ctx, stmt);    break;
    case NODE_FOR:      genFor      (ctx, stmt);    break;
    case NODE_BREAK:    genBreak    (ctx, stmt);    break;
    case NODE_CONTINUE: genContinue (ctx, stmt);    break;
    case NODE_DEFER:    genDefer    (ctx, stmt);    break;
    default: {
        LLVMValueRef compiled = genExpr(ctx, stmt);
        if (compiled) return;
        error(ctx->state, stmt->tok,
                "Node '%d' does not compile yet",
                stmt->type);
        break;
    }
    }
}

/* Stores the node in a list of stack allocation such that in the second pass
 * the expression knows where it should be stored.
 * NOTE: this method stores always the expression that requires the stack allocation.
 * So for variable declaration always store the rvalue node and not the variable node.
 * */
static void buildFuncVar(ZCodegen *ctx, ZNode *node, const char *name, ZType *typeOverride) {
    if (!node) {
        error(ctx->state, NULL, "'buildFuncVar' called with a null node");
        return;
    } else if (!node->resolved) {
        error(ctx->state, node->tok,
                "Missing resolved type for node %d", node->type);
        return;
    }

    ZType *allocaType = typeOverride ? typeOverride : node->resolved;
    LLVMTypeRef type = genType(ctx, allocaType);
    LLVMValueRef val = LLVMBuildAlloca(ctx->builder, type, name);

    ZLLVMStack *item = zalloc(ZLLVMStack);
    *item = (ZLLVMStack){ .stack = val, .type = type, .node = node };

    vecpush(ctx->scope->stackAlloca, item);
}

/* All variables of the function body are allocated at the start of the block.
 * This is the first pass where it navigates the AST
 * and allocate to the stack (LLVMBuildAlloca) the variables and store the node
 * as allocated such that the function genExpr knows if the generated values
 * must be stored in the stack. 
 * Now The stack allocations are just variable declarations and function calls
 * that return a non-primitive type (so a type that does not fit in a register).
 * */
static void genFuncVars(ZCodegen *ctx, ZNode *node) {
    if (!node) return;
    switch (node->type) {
    case NODE_RETURN:
        genFuncVars(ctx, node->returnStmt.expr);
        break;
    case NODE_TUPLE_LIT:
        buildFuncVar(ctx, node, label(ctx), NULL);
        break;
    case NODE_STRUCT_LIT:
        buildFuncVar(ctx, node, label(ctx), NULL);
        break;
    case NODE_CAST:
        genFuncVars(ctx, node->castExpr.expr);
        break;
    case NODE_MEMBER:
        genFuncVars(ctx, node->memberAccess.object);
        break;
    case NODE_FOR: {
        ZNode *forVar = node->forStmt.var;
        char *name = forVar->varDecl.ident->identNode.tok->str;
        buildFuncVar(ctx, forVar->varDecl.rvalue, name, forVar->resolved);
        genFuncVars(ctx, node->forStmt.block);
        break;
   }
    case NODE_WHILE:
        genFuncVars(ctx, node->whileStmt.cond);
        genFuncVars(ctx, node->whileStmt.branch);
        break;
    case NODE_IF:
        genFuncVars(ctx, node->ifStmt.cond);
        genFuncVars(ctx, node->ifStmt.body);
        genFuncVars(ctx, node->ifStmt.elseBranch);
        break;
    case NODE_VAR_DECL:
        buildFuncVar(ctx, node->varDecl.rvalue,
            node->varDecl.ident->identNode.tok->str,
            node->resolved);
        break;
    case NODE_CALL:
        if (!node->resolved) {
            error(ctx->state, node->tok, "Unresolved type");
        }
        if (!typesPrimitive(node->resolved)) {
            buildFuncVar(ctx, node, label(ctx), NULL);
        }
        for (usize i = 0; i < veclen(node->call.args); i++) {
            if (!typesPrimitive(node->call.args[i]->resolved)) {
                buildFuncVar(ctx, node->call.args[i], label(ctx), NULL);
            }
        }
        break;

    case NODE_BINARY:
        genFuncVars(ctx, node->binary.left);
        genFuncVars(ctx, node->binary.right);
        break;
    case NODE_BLOCK:
        for (usize i = 0; i < veclen(node->block); i++) {
            genFuncVars(ctx, node->block[i]);
        }
        break;
    default: break;
    }
}


static LLVMValueRef genFunc(ZCodegen *ctx, ZNode *f) {
    beginScope(Z_SCOPE_FUNC, ctx);
    LLVMTypeRef ret = genType(ctx, f->funcDef.ret);
    LLVMTypeRef *args = NULL;

    if (!f->funcDef.mangled) {
        f->funcDef.mangled = mangler((ZToken*[]) { f->funcDef.name, NULL });
    }

    if (f->funcDef.receiver) {
        LLVMTypeRef receiverType = genType(ctx, f->funcDef.receiver->resolved);
        vecpush(args, receiverType);
    }

    for (usize i = 0; i < veclen(f->funcDef.args); i++) {
        LLVMTypeRef arg = genType(ctx, f->funcDef.args[i]->field.type);
        vecpush(args, arg);
    }

    LLVMTypeRef funcType = LLVMFunctionType(ret, args, veclen(args), false);
    LLVMValueRef func =  LLVMAddFunction(ctx->mod, f->funcDef.mangled, funcType);
    ctx->currentFunc = func;

    LLVMBasicBlockRef entry = makeblock(ctx);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    if (f->funcDef.receiver) {
      char *name = f->funcDef.receiver->field.identifier->str;
      LLVMTypeRef recType = genType(ctx, f->funcDef.receiver->resolved);
      LLVMValueRef slot = LLVMBuildAlloca(ctx->builder, recType, name);
      LLVMBuildStore(ctx->builder, LLVMGetParam(func, 0), slot);
      putLLVMValueRef(ctx, name, slot);
    }

    /* Allocate a stack slot for each parameter so they can be reassigned.
     * The receiver (if present) occupies param index 0, so regular args
     * start at offset 1. */
    usize paramOffset = f->funcDef.receiver ? 1 : 0;
    for (usize i = 0; i < veclen(f->funcDef.args); i++) {
        char *name = f->funcDef.args[i]->field.identifier->str;
        LLVMTypeRef paramType = genType(ctx, f->funcDef.args[i]->field.type);

        LLVMValueRef slot = LLVMBuildAlloca(ctx->builder, paramType, name);
        LLVMBuildStore(ctx->builder, LLVMGetParam(func, i + paramOffset), slot);

        putLLVMValueRef(ctx, name, slot);
    }

    /* All variable declarations are declared at the start of the function. */
    genFuncVars(ctx, f->funcDef.body);

    genBlock(ctx, f->funcDef.body);

    genChainDefer(ctx, ctx->scope->parent);

    /* Add implicit ret void only if the block has no terminator yet. */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
        LLVMBuildRetVoid(ctx->builder);
    }
    endScope(ctx);

    putLLVMValueRef(ctx, f->funcDef.mangled, func);

    return func;
}

static void compile(ZCodegen *ctx, ZNode *root) {
    switch (root->type) {
    case NODE_FOREIGN:  genForeign  (ctx, root);            break;
    case NODE_FUNC:     genFunc     (ctx, root);            break;
    case NODE_STRUCT:   genType     (ctx, root->resolved);  break;
    case NODE_MACRO:                                        break;

    case NODE_MODULE:
        beginModule(ctx, root);
        for (usize i = 0; i < veclen(root->module.root); i++) {
            compile(ctx, root->module.root[i]);
        }
        endModule(ctx);
        break;
    default:
        error(ctx->state, root->tok, "(compilation not yet implemented for %d)", root->type);
        break;
    }
}

static bool emitObjectFile(ZCodegen *ctx, const char *filename) {
    LLVMInitializeAllTargetInfos();
    LLVMInitializeAllTargets();
    LLVMInitializeAllTargetMCs();
    LLVMInitializeAllAsmParsers();
    LLVMInitializeAllAsmPrinters();

    char *triple = LLVMGetDefaultTargetTriple();
    LLVMSetTarget(ctx->mod, triple);

    LLVMTargetRef target;
    char *errmsg = NULL;
    if (LLVMGetTargetFromTriple(triple, &target, &errmsg)) {
        error(ctx->state, NULL, "Failed to get target: %s", errmsg);
        LLVMDisposeMessage(errmsg);
        LLVMDisposeMessage(triple);
        return false;
    }

    LLVMTargetMachineRef machine = LLVMCreateTargetMachine(
        target, triple, "generic", "",
        LLVMCodeGenLevelDefault,
        LLVMRelocPIC,
        LLVMCodeModelDefault
    );

    if (LLVMTargetMachineEmitToFile(machine, ctx->mod, (char *)filename,
                                     LLVMObjectFile, &errmsg)) {
        error(ctx->state, NULL, "Failed to emit object file: %s", errmsg);
        LLVMDisposeMessage(errmsg);
        LLVMDisposeTargetMachine(machine);
        LLVMDisposeMessage(triple);
        return false;
    }

    LLVMDisposeTargetMachine(machine);
    LLVMDisposeMessage(triple);
    return true;
}

static void freeCodegen(ZCodegen *ctx) {
    LLVMDisposeBuilder(ctx->builder);
    LLVMDisposeModule(ctx->mod);
    LLVMContextDispose(ctx->ctx);
}

void zcompile(ZState *state, ZNode *root, const char *output, ZSemantic *semantic) {
    ZCodegen *ctx = makecodegen(state, semantic);
    initNativeTypes(ctx);
    compile(ctx, root);

    char *errmsg = NULL;
    if (!state->skipLLVMValidation && LLVMVerifyModule(ctx->mod, LLVMReturnStatusAction, &errmsg)) {
        error(state, NULL, "LLVM: %s", errmsg);
        LLVMDisposeMessage(errmsg);
        freeCodegen(ctx);
        return;
    }
    LLVMDisposeMessage(errmsg);

    if (state->emitLLVM) {
        const char *llfile = output ? output : "output.ll";
        if (LLVMPrintModuleToFile(ctx->mod, llfile, &errmsg)) {
            error(state, NULL, "Failed to write IR file: %s", errmsg);
            LLVMDisposeMessage(errmsg);
        } else {
            printf("LLVM IR written to %s\n", llfile);
        }
        freeCodegen(ctx);
        return;
    }

    const char *objfile = "output.o";
    if (!emitObjectFile(ctx, objfile)) {
        freeCodegen(ctx);
        return;
    }

    const char *outname = output ? output : "a.out";
    printf(COLOR_BLUE COLOR_BOLD "  Generated " COLOR_RESET "%s\n", output);
    int ret = zinc_lld_link(objfile, outname);
    if (ret != 0) {
        error(state, NULL, "Linker failed with code %d", ret);
    }

    remove(objfile);

    freeCodegen(ctx);
}
