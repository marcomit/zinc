/**
 * @file zgen.c
 * @brief Code generator that emits LLVM-IR
 *
 * It generates the LLVM-IR from the AST.
 * It does a first pass where store all variable declarations
 * such that the ordering doesn't matter.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */

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
    /* The variable stored in the stack. */
    LLVMValueRef    stack;

    /* the data stored in the stack.
     * It differs from the stack field only for prefixed types like arrays.
     * */
    LLVMValueRef    elem;
    LLVMTypeRef     stackType;
    LLVMTypeRef     elemType;
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

    /* Struct type cache - parallel arrays keyed by name */
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
static LLVMValueRef genLvalue       (ZCodegen *ctx, ZNode *node);

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

/**
 * @brief Returns the variable saved in the stack.
 *
 * It walks the symbols in the scope and go up while it reaches the highest scope.
 *
 * @see genIdent to see how it's used.
 */
static LLVMValueRef getLLVMValueRef(ZCodegen *ctx, char *key) {
    ZLLVMScope *cur = ctx->scope;
    while (cur) {
        usize len = veclen(cur->symbols);
        for (usize i = len; i-- > 0;) {
            if (strcmp(cur->symbols[i]->name, key) == 0) {
                return cur->symbols[i]->value;
            }
        }
        cur = cur->parent;
    }
    return NULL;
}

/**
 * @brief Initialize LLVM types
 *
 * Integer types are used everywhere in the code generation
 * so they are 'cached' and initialized only once
 * at the stack of the code generation.
 */
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
       For the root module ctx->mod is NULL - create the one module
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

/**
 * @brief Used to name LLVM instructions.
 *
 * If the compiler is in dev mode it emits only
 * the prefix "zn" + a progressive counter in hex format.
 * If the dev mode is not enabled it emits the source string given by the token
 */
char *label(ZCodegen *ctx, ZToken *tok) {
    memset(ctx->str, 0, veclen(ctx->str));
    vecsetlen(ctx->str, 0);
    if (ctx->state->debug && tok) {
        char *str = stoken(tok);
        vecunion(ctx->str, str, strlen(str));
    } else {
        sprintf(ctx->str, "zn%.3zx", ctx->count);

        ctx->count++;
    }
    return ctx->str;
}

usize typeSize(ZCodegen *, ZType *);

/**
 * @brief Calculates padding of types.
 *
 * This function is used to calculate the padding of struct/tuple fields.
 * It is also used for enums.
 *
 * @param iter used to take the type from the current field.
 * @param fields must be a dynamic array because it uses veclen.
 */
static usize alignFields(ZCodegen *ctx, void **fields, ZType *(*iter)(void *)) {
    usize cur = 0;
    usize res = 0;
    for (usize i = 0; i < veclen(fields); i++) {
        cur = typeSize(ctx, iter(fields[i]));
        if (cur) res = (res + cur - 1) / cur * cur;
        res += cur;
    }

    return res;
}

static inline ZType *alignStructFieldIter(void *item) {
    return ((ZNode *)item)->resolved;
}
static inline ZType *alignTupleFieldIter(void *item) { return (ZType *)item; }

/**
 * @brief Calculates the size of the type.
 *
 * Primitives type have a constant size.
 * Structs and tuples are just the sum of their fields + the padding
 * calculated with alignFields.
 *
 * For enums it creates a struct with two fields:
 * - The flag that indicates the active variant and it is u8
 * - The buffer where its size is the size of the largest variant.
 */
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
            error(ctx->state, type->tok,
                "Unknown type (zsem didn't resolve this type)");
            return 0;
        }
    case Z_TYPE_POINTER:    return 8; /* 64-bit pointer */
    case Z_TYPE_FUNCTION:   return 8; /* function pointer */
    case Z_TYPE_ARRAY:      return 16;/* {length: u64, ptr: *u8}*/

    case Z_TYPE_STRUCT: {
        res = alignFields(
            ctx,
            (void **)type->strct.fields,
            alignStructFieldIter
        );

        return res;
    }

    case Z_TYPE_ENUM: {
        usize max = 0;
        usize cur = 0;
        
        for (usize i = 0; i < veclen(type->enm.fields); i++) {
            cur = typeSize(ctx, type->enm.fields[i]);
            if (cur > max) max = cur;
        }
        if (max == 0) {
            error(ctx->state, type->tok, "Invalid enum size");
        }
        return max + 1;
    }

    case Z_TYPE_TUPLE:
        return alignFields(
            ctx,
            (void **)type->tuple,
            alignTupleFieldIter
        );
    default: return 0;
    }
}

static bool _getStructIndex(ZType *strct, char *fieldName, u32 **path) {
    for (usize i = 0; i < veclen(strct->strct.fields); i++) {
        ZNode *field = strct->strct.fields[i];
        if (field->type == NODE_EMBED_FIELD) {
            vecpush(*path, i);
            bool found = _getStructIndex(field->resolved, fieldName, path);

            if (!found) vecpop(*path);
            else return true;
        } else if (strcmp(fieldName, field->field.identifier->str) == 0) {
            vecpush(*path, i);
            return true;
        }
    }
    return false;
}

/**
 * @brief Takes the struct index of a field.
 *
 * A struct can be composed by embedded fields which means
 * that a field can be accessed directly but compiled in a nested struct.
 * So it search recursively by its embedded fields and return the path
 */
static u32 *getStructIndex(ZType *strct, char *fieldName) {
    u32 *path = NULL;
    if (!_getStructIndex(strct, fieldName, &path)) {
        return NULL;
    }
    return path;
}

/**
 * @brief get the index of a variant's enum.
 *
 * @return -1 if not found
 */
static i32 enumIndexField(ZType *enumType, ZToken *field) {
    for (usize i = 0; i < veclen(enumType->enm.fields); i++) {
        if (tokeneq(enumType->enm.fields[i]->strct.name, field)) {
            return i;
        }
    }
    return -1;
}

static ZNode *getStructField(ZType *strct, char *fieldName) {
    for (usize i = 0; i < veclen(strct->strct.fields); i++) {
        ZNode *field = strct->strct.fields[i];
        if (field->type == NODE_EMBED_FIELD) {
            ZNode *found = getStructField(field->resolved, fieldName);
            if (found) return found;
        } else if (field->type == NODE_FIELD) {
            if (strcmp(field->field.identifier->str, fieldName) == 0) {
                return field;
            }
        }
    }
    return NULL;
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

/**
 * @brief Translate a ZType to an LLVM type.
 *
 * For primitives it returns directly
 * the cached values (loaded at the start of the code generation).
 * 
 * Arrays are compiled with a struct of two fields:
 * - The size of the array as a u64
 * - The pointer to the elements.
 *
 * Struct is compiled by call itself for each field (embedded and normal).
 * Tuple is just an unnamed struct.
 * Enum is a struct with two fields:
 * - u8 is the tag that represent the active variant
 * - *u8 is the buffer where the variant is stored (with the size of the largest field).
 */
static LLVMTypeRef genType(ZCodegen *ctx, ZType *type) {
    if (!type) {
        error(ctx->state, NULL, "Invalid 'genType' call");
        return LLVMVoidTypeInContext(ctx->ctx);
    }

    switch (type->kind) {
    case Z_TYPE_GENERIC:
        error(ctx->state, type->tok, "Generics not resolved");
        return NULL;
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
        case TOK_RUNE:
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
        LLVMTypeRef descriptorFields[] = {
            i64Type, LLVMPointerType(base, 0)
        };
        return LLVMStructTypeInContext(ctx->ctx, descriptorFields, 2, 0);
    }

    case Z_TYPE_FUNCTION: {
        usize argc = veclen(type->func.args);
        LLVMTypeRef *params = znalloc(LLVMTypeRef, argc ? argc : 1);
        for (usize i = 0; i < argc; i++) {
            params[i] = genType(ctx, type->func.args[i]);
            if (!params[i]) return NULL;
        }
        LLVMTypeRef ret = genType(ctx, type->func.ret);
        return LLVMFunctionType(ret, params, (unsigned)argc, type->func.variadic);
    }

    case Z_TYPE_STRUCT: {
        const char *name    = type->strct.name->str;

        /* Check cache */
        LLVMTypeRef cached  = getCachedStruct(ctx, name);
        if (cached) return cached;

        /* Cache the opaque named struct before generating its body so that
         * self-referential fields find the entry and don't recurse infinitely. */
        LLVMTypeRef structType  = LLVMStructCreateNamed(ctx->ctx, name);
        putStructInCache(ctx, (char *)name, structType);

        usize nfields           = veclen(type->strct.fields);
        LLVMTypeRef ftypes[nfields];

        for (usize i = 0; i < veclen(type->strct.fields); i++) {
            ZType *ft = type->strct.fields[i]->resolved;
            LLVMTypeRef field = genType(ctx, ft);
            /* Function types cannot be embedded directly in a struct - store
             * as a function pointer instead. */
            if (ft && ft->kind == Z_TYPE_FUNCTION)
                field = LLVMPointerType(field, 0);
            ftypes[i] = field;
        }

        LLVMStructSetBody(structType, ftypes, nfields, /*packed=*/0);
        return structType;
    }

    /* Enums are generated like tagged unions in c.
     * They are simpy a struct with an integer also called flag
     * that represents the active field of the enum
     * and a buffer with the size of the biggest field.
     * The buffer is stored like an array of bytes.
     * Every time i want to get the active field i just lookup the flag
     * and cast the buffer as the ith field.
     * */
    case Z_TYPE_ENUM: {
        const char *name = type->enm.name->str;
        LLVMTypeRef cached = getCachedStruct(ctx, name);
        if (cached) return cached;

        LLVMTypeRef enumType = LLVMStructCreateNamed(ctx->ctx, name);
        putStructInCache(ctx, (char *)name, enumType);

        for (usize i = 0; i < veclen(type->enm.fields); i++) {
            genType(ctx, type->enm.fields[i]);
        }

        usize largest = typeSize(ctx, type);
        LLVMStructSetBody(enumType,
                (LLVMTypeRef[]){
            // Flag integer
            i8Type,

            // Buffer array with the largest field
            LLVMArrayType(i8Type, largest - 1)
        }, 2, 0);

        return enumType;
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
        /* none literal - represent as i8* null */
        return LLVMPointerType(i8Type, 0);

    default:
        error(ctx->state, NULL, "genType: unhandled type kind %d", type->kind);
        return NULL;
    }
}

static LLVMBasicBlockRef makeblock(ZCodegen *ctx) {
    return LLVMAppendBasicBlockInContext(
        ctx->ctx, ctx->currentFunc, label(ctx, NULL)
    );
}

/**
 * @brief Emits all defer statements.
 *
 * Defer statements are stored in the scope.
 * They are emitted in three ways:
 * - At the end of the block
 * - On a break/continue
 * - On a return statement
 *
 * @param scope is the target scope.
 * For break, continue and at the end of the block must be passed the parent scope.
 * For return statements must be passed the function-level scope.
 */
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

/**
 * @brief Generate literals
 */
static LLVMValueRef genLit(ZCodegen *ctx, ZNode *node) {
    ZToken *tok = node->tok;
    switch (tok->type) {
    case TOK_STR_LIT:
        return LLVMBuildGlobalStringPtr(ctx->builder, tok->str, label(ctx, node->tok));
    case TOK_INT_LIT:
    case TOK_RUNE_LIT:
        return LLVMConstInt(i32Type, tok->integer, true);
    case TOK_TRUE:
        return LLVMConstInt(i1Type, 1, false);
    case TOK_FALSE:
        return LLVMConstInt(i1Type, 0, false);
    case TOK_FLOAT_LIT:
        return LLVMConstReal(f64Type, tok->floating);
    case TOK_NONE: {
        LLVMTypeRef type = genType(ctx, node->resolved);
        return LLVMConstPointerNull(type);
    }
    default: return NULL;
    }
}

/**
 * @brief Emits the IR for identifiers (variables).
 *
 * It takes the stack allocation of the identifier saved in the scope.
 * */
static LLVMValueRef genIdent(ZCodegen *ctx, ZNode *node) {
    if (!node) {
        error(ctx->state, NULL, "'genIdent' called with a null node");
        return NULL;
    } else if (!node->tok) {
        error(ctx->state, NULL,
                "'genIdent' called with a null token on node %d", node->type);
        return NULL;
    } else if (!node->resolved) {
        error(ctx->state, node->tok, "%zu doesn't have the resolved field", node->type);
        return NULL;
    }

    char *key = node->identNode.mangled ? node->identNode.mangled : node->tok->str;
    LLVMValueRef val = getLLVMValueRef(ctx, key);
    if (!val) {
        error(ctx->state, node->tok, "'%s' not found in the current scope", node->tok->str);
        return NULL;
    }
    /* Local variables are stored as allocas - load to get the value.
       Functions are stored directly - return as-is. */
    if (LLVMGetValueKind(val) == LLVMInstructionValueKind) {
        LLVMTypeRef type = genType(ctx, node->resolved);
        return LLVMBuildLoad2(ctx->builder, type, val, node->tok->str);
    }
    return val;
}

/**
 * @brief returns the cached stack pointer
 *
 * Stack pointers are 'cached' by the node and saved in the scope.
 * @param key is the cached key.
 *
 */
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

/**
 * @brief returns true if the value fits in a register.
 */
static bool fitsInRegister(LLVMValueRef val) {
    LLVMTypeKind kind = LLVMGetTypeKind(LLVMTypeOf(val));

    return (
        kind == LLVMPointerTypeKind || 
        kind == LLVMIntegerTypeKind ||
        kind == LLVMFloatTypeKind   ||
        kind == LLVMDoubleTypeKind  ||
        kind == LLVMFunctionTypeKind);
}

/**
 * @brief Stores every identifier of a destructured pattern in the stack
 *
 * It takes a stack allocation (LLVMValueRef) and follwos a simple pattern:
 * - An identifier stores directly the value into the scope.
 * - A struct generates a pointer for each field and call itself.
 * - A tuple do the same of the struct
 * */
static void putDestructuredPatternInStack(
        ZCodegen *ctx, ZType *type,
        ZVarDestructPattern *pattern, LLVMValueRef ptr) {
    if (!pattern || !type) return;

    switch (pattern->type) {
    case Z_VAR_IDENT:
        putLLVMValueRef(
            ctx,
            pattern->ident->str,
            ptr
        );
        break;
    case Z_VAR_STRUCT: {
        LLVMTypeRef typeRef = genType(ctx, type);
        for (usize i = 0; i < veclen(pattern->fields); i++) {

            int idx = -1;

            for (usize j = 0; j < veclen(type->strct.fields); j++) {
                if (tokeneq(
                        type->strct.fields[j]->field.identifier,
                        pattern->fields[i]->key)
                    ) {
                    idx = (int)j;
                    break;
                }
            }

            if (idx == -1) {
                error(ctx->state, pattern->fields[i]->key,
                        "Invalid struct field '%s'",
                        pattern->fields[i]->key->str);
                continue;
            }

            LLVMValueRef gep = LLVMBuildStructGEP2(
                ctx->builder, typeRef,
                ptr, idx, label(ctx, pattern->fields[i]->key)
            );
            putDestructuredPatternInStack(
                ctx,
                type->strct.fields[idx]->field.type,
                pattern->fields[i]->value,
                gep
            );
        }
        break;
    }
    case Z_VAR_TUPLE: {
        LLVMTypeRef typeRef = genType(ctx, type);
        for (usize i = 0; i < veclen(pattern->tuple); i++) {
            LLVMValueRef gep = LLVMBuildStructGEP2(
                ctx->builder, typeRef, ptr, i, label(ctx, pattern->tuple[i]->tok)
            );
            putDestructuredPatternInStack(
                ctx,
                type->tuple[i],
                pattern->tuple[i],
                gep
            );
        }
        break;
    }
    case Z_VAR_ENUM: {
        i32 variantIndex = enumIndexField(type, pattern->prop);
        if (variantIndex == -1) {
            error(ctx->state, pattern->prop, "Variant not found");
            return;
        }
        ZType *variantType          = type->enm.fields[variantIndex];
        LLVMTypeRef variantTypeRef  = genType(ctx, variantType);

        for (usize i = 0; i < veclen(pattern->args); i++) {
            LLVMValueRef fieldPtr = LLVMBuildStructGEP2(
                ctx->builder, variantTypeRef, ptr,
                i + 1, label(ctx, pattern->args[i]->tok));
            putDestructuredPatternInStack(
                ctx,                variantType->strct.fields[i + 1]->resolved,
                pattern->args[i],   fieldPtr
            );
        }
        break;
    }
    default:
        error(ctx->state, pattern->tok, "Unhandled destruct pattern");
        break;
    }
}

/**
 * @brief Generate variable declarations
 *
 * Variable declarations are allocated into the stack in the first-pass.
 * So the node must be cached and the call getStackValue cannot fail.
 *
 * Once the pointer is looked up it stores the pattern on the left (destructured var).
 * Then generates the expression and store the result into the stack pointer.
 */
static void genVarDecl(ZCodegen *ctx, ZNode *node) {
    if (!node->varDecl.rvalue || !node->resolved) {
        error(ctx->state, node->tok, "Invalid 'genVarDecl' call");
        return;
    }

    ZLLVMStack *stack = getStackValue(ctx, node->varDecl.rvalue);
    if (!stack) {
        error(ctx->state, node->tok, "Missing stack allocation for '%s'", node->tok->str);
        return;
    }

    putDestructuredPatternInStack(
        ctx, node->resolved, node->varDecl.pattern, stack->stack);

    LLVMValueRef val = genExpr(ctx, node->varDecl.rvalue);

    if (val && val != stack->stack) {
        if (fitsInRegister(val)) {
            val = castValue(ctx, val, node->varDecl.rvalue->resolved, node->resolved);
        }
        LLVMBuildStore(ctx->builder, val, stack->stack);
    }
}

/**
 * @brief Generates the field access for an embedded field.
 */
static LLVMValueRef genStructGEPChain(ZCodegen *ctx,
    ZType *base, LLVMValueRef origin, u32 *path) {
    LLVMValueRef prev   = origin;
    LLVMValueRef ptr    = NULL;

    for (usize i = 0; i < veclen(path); i++) {
        ptr = LLVMBuildStructGEP2(
            ctx->builder,
            genType(ctx, base),
            prev,
            path[i],
            label(ctx, base->tok)
        );
        base = base->strct.fields[path[i]]->resolved;
        prev = ptr;
    }

    return ptr;
}

/**
 * @brief store the array metadata into the compiled struct.
 */
static void storeArray(ZCodegen *ctx, ZLLVMStack *stack, LLVMValueRef length) {
    LLVMValueRef lenField = LLVMBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 0, "len");

    LLVMBuildStore(ctx->builder, length, lenField);

    LLVMValueRef indices[] = {
        LLVMConstInt(i32Type, 0, false),
        LLVMConstInt(i32Type, 0, false)
    };
    LLVMValueRef dataPtr = LLVMBuildGEP2(
        ctx->builder, stack->elemType, stack->elem, indices, 2, "ptr"
    );
    LLVMValueRef dataField = LLVMBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 1, "ptr");
    LLVMBuildStore(ctx->builder, dataPtr, dataField);
}

/**
 * @brief Generates array literal.
 */
static LLVMValueRef genArrayLitPtr(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);

    if (!stack || !stack->elem) {
        error(ctx->state, node->tok, "Missing stack value");
        return NULL;
    }

    LLVMTypeRef elemType = genType(ctx, node->resolved->array.base);

    for (usize i = 0; i < veclen(node->arraylit); i++) {
        LLVMValueRef indices[] = {
            LLVMConstInt(i32Type, 0, false),
            LLVMConstInt(i32Type, i, false)
        };
        LLVMValueRef gep = LLVMBuildGEP2(
            ctx->builder,   stack->elemType,
            stack->elem,    indices,
            2,              label(ctx, node->tok)
        );

        ZNode *elem = node->arraylit[i];
        LLVMValueRef val = genExpr(ctx, elem);
        if (!val) {
            error(ctx->state, elem->tok, "Array element could not be compiled");
            return NULL;
        }
        /* A call returning a struct comes back as an aggregate value,
           not a pointer - store it directly. */
        if (LLVMGetTypeKind(LLVMTypeOf(val)) == LLVMPointerTypeKind &&
            node->resolved->array.base->kind == Z_TYPE_STRUCT) {
            val = LLVMBuildLoad2(ctx->builder, elemType, val, label(ctx, node->tok));
        }
        LLVMBuildStore(ctx->builder, val, gep);
    }

    storeArray(
        ctx, stack, LLVMConstInt(i64Type, veclen(node->arraylit), false)
    );
    
    return stack->stack;
}

/**
 * @brief Generates the slice from an array.
 *
 * Slices are generated like array with the difference that
 * its length is calculated at runtime.
 */
static LLVMValueRef genSlicePtr(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack       = getStackValue(ctx, node);
    ZType *baseType         = node->slice.base->resolved;
    LLVMValueRef ptr        = genLvalue(ctx, node->slice.base);
    LLVMTypeRef sliceType   = genType(ctx, baseType);
    LLVMTypeRef elemType    = genType(ctx, baseType->array.base);
    if (!ptr) {
        error(ctx->state, node->slice.base->tok, "pointer not found");
        return NULL;
    }
    if (!stack) {
        error(ctx->state, node->tok, "Missing stack allocation");
        return NULL;
    }

    LLVMValueRef start = node->slice.start  ?
        genExpr(ctx, node->slice.start)     :
        LLVMConstInt(i64Type, 0, false);

    LLVMValueRef end = NULL;

    if (node->slice.end) {
        end = genExpr(ctx, node->slice.end);
    } else {
        end = LLVMBuildStructGEP2(
            ctx->builder,
            sliceType,
            ptr, 0, "len_ptr"
        );
        end = LLVMBuildLoad2(ctx->builder, i64Type, end, "len");
    }

    LLVMValueRef length = LLVMBuildSub(
        ctx->builder, end, start, label(ctx, node->tok)
    );

    LLVMValueRef lenField = LLVMBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 0, "len");
    LLVMBuildStore(ctx->builder, length, lenField);

    LLVMValueRef ptrField = LLVMBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 1, "ptr");

    LLVMValueRef originPtrField = LLVMBuildStructGEP2(
        ctx->builder, sliceType, ptr, 1, "ptr"
    );

    LLVMValueRef basePtr = LLVMBuildLoad2(
        ctx->builder, LLVMPointerType(elemType, 0),
        originPtrField, "ptr"
    );

    LLVMValueRef dataPtr = LLVMBuildGEP2(
        ctx->builder, elemType, basePtr, &start, 1, "ptr");

    LLVMBuildStore(
        ctx->builder,
        dataPtr, ptrField
    );

    return stack->stack;
}

static LLVMValueRef genStructLitPtr(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);
    if (!stack) {
        error(ctx->state, node->tok, "Stack value not found");
        return NULL;
    }
    return genStructLitInto(ctx, node, stack->stack);
}

static LLVMValueRef genTupleLitInto(ZCodegen *ctx, ZNode *node, LLVMValueRef dest) {
    LLVMTypeRef type = genType(ctx, node->resolved);
    const char *name = label(ctx, node->tok);

    LLVMTypeRef tupleType = genType(ctx, node->resolved);
    LLVMValueRef ptr = dest ? dest : LLVMBuildAlloca(ctx->builder, type, name);
    LLVMValueRef fieldPtr, val;

    usize len = veclen(node->tuplelit);
    for (usize i = 0; i < len; i++) {
        val = genExpr(ctx, node->tuplelit[i]);

        fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, tupleType,
            ptr, i, label(ctx, node->tok)
        );

        LLVMBuildStore(ctx->builder, val, fieldPtr);
    }
    return ptr;
}

static LLVMValueRef genTupleLitPtr(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);

    if (!stack) {
        error(ctx->state, node->tok, "Stack allocation not found");
    }
    return genTupleLitInto(ctx, node, stack->stack);
}

static LLVMValueRef genMemberAccessPtr(ZCodegen *ctx, ZNode *node) {
    ZType *objType  = node->memberAccess.object->resolved;
    ZToken *tok     = node->memberAccess.field;

    ZType *baseType = objType;
    while (baseType && baseType->kind == Z_TYPE_POINTER)
        baseType    = baseType->base;

    u32 *path = NULL;
    if (baseType->kind == Z_TYPE_STRUCT) {
        path = getStructIndex(baseType, tok->str);
        if (!path) {
            error(ctx->state, tok, "'%s' member not found", tok->str);
            return NULL;
        }
    } else if (baseType->kind == Z_TYPE_TUPLE) {
        if (tok->integer > (i32)veclen(baseType->tuple) ||
            tok->integer < 0) {
            error(ctx->state, tok, "Invalid index %d for tuple", tok->integer);
            return NULL;
        }
        return LLVMBuildStructGEP2(
            ctx->builder,
            genType(ctx, baseType),
            genLvalue(ctx, node->memberAccess.object),
            tok->integer,
            label(ctx, tok)
        );
    } else if (objType->kind == Z_TYPE_ARRAY) {
        LLVMValueRef ptr = genLvalue(ctx, node->memberAccess.object);
        i32 index = -1;

        if      (strcmp(tok->str, "len") == 0) index = 0;
        else if (strcmp(tok->str, "ptr") == 0) index = 1;
        else {
            error(ctx->state, tok, "Unknown field");
            return NULL;
        }

        return LLVMBuildStructGEP2(
            ctx->builder, genType(ctx, objType),
            ptr, (unsigned)index, label(ctx, tok)
        );
    }

    LLVMValueRef objPtr    = objType->kind == Z_TYPE_POINTER
        ? genExpr  (ctx, node->memberAccess.object)
        : genLvalue(ctx, node->memberAccess.object);

    LLVMValueRef chain = genStructGEPChain(
        ctx, baseType, objPtr, path
    );
    return chain;
}

static LLVMValueRef genSubscriptPtr(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr        = genLvalue(ctx, node->subscript.arr);
    LLVMValueRef i          = genExpr(ctx, node->subscript.index);
    ZType *arrType          = node->subscript.arr->resolved;
    LLVMTypeRef type        = genType(ctx, arrType);
    const char *name        = label(ctx, node->tok);
    
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
    } else if (arrType->kind == Z_TYPE_ARRAY) {
        LLVMTypeRef elemType = genType(ctx, arrType->array.base);
        LLVMTypeRef ptrType = LLVMPointerType(elemType, 0);
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, type, ptr,
            1, label(ctx, node->subscript.arr->tok)
        );
        LLVMValueRef basePtr = LLVMBuildLoad2(
            ctx->builder, ptrType, fieldPtr, name
        );
        return LLVMBuildGEP2(
            ctx->builder,   elemType,
            basePtr,        &i,
            1,              name
        );
    }
    error(ctx->state, node->tok,
        "Invalid subscript for type %s",
        stype(node->resolved)
    );
    return NULL;
}

static LLVMValueRef genEnumLitPtr(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);
    if (!stack) {
        error(ctx->state, node->tok, "Missing stack value");
        return NULL;
    }

    i32 index = enumIndexField(
        node->resolved,
        node->call.callee->staticAccess.prop
    );

    if (index == -1) {
        error(ctx->state, node->staticAccess.prop, "Field not found");
        return NULL;
    }

    LLVMTypeRef fieldType   = genType(ctx, node->resolved->enm.fields[index]);

    LLVMBuildStore(
        ctx->builder,
        LLVMConstInt(i8Type, (u32)index, false),
        stack->stack
    );

    ZNode **args = node->call.args;
    for (usize i = 0; i < veclen(args); i++) {
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, fieldType, stack->stack, i + 1, label(ctx, args[i]->tok));

        LLVMBuildStore(
            ctx->builder,
            genExpr(ctx, args[i]),
            fieldPtr
        );

    }

    return stack->stack;
}

/**
 * @brief Generates a call to a function.
 *
 * The name of the function depends on the 'type' of the function:
 * - Raw functions have the normal name.
 * - Receiver functions have a mangled name.
 * - Static functions have also a mangled name but with a different rule
 *   to avoid conflicts with receiver functions.
 *
 * The parameter list is compiled as follow:
 * - The first parameter is the receiver, if present.
 * - The list of capabilities.
 * - The list of normal arguments.
 */
static LLVMValueRef genCall(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef func   = NULL;
    LLVMValueRef *args  = NULL;
    ZNode *callee       = node->call.callee;

    if (callee->type == NODE_MEMBER && callee->memberAccess.mangled) {
        /* Receiver method call: look up the global function and inject self. */
        func = getLLVMValueRef(ctx, callee->memberAccess.mangled);

        if (!func) error(ctx->state,
                    callee->tok,
                    "Receiver function '%s' not found",
                    callee->memberAccess.mangled);

        LLVMValueRef self = genExpr(ctx, callee->memberAccess.object);
        vecpush(args, self);

    }  else {
        /* Expression call: covers identifiers, static access, subscripts, and
         * function-pointer fields (NODE_MEMBER without mangled). */
        func = genExpr(ctx, callee);
        if (!func) return NULL;
    }

    /* For indirect calls (locally-loaded function pointers) the LLVM value is
     * not a global, so LLVMGlobalGetValueType is invalid. Derive the function
     * type from the semantic info instead. mangled == NULL means indirect. */
    LLVMTypeRef funcType;
    if (callee->type == NODE_MEMBER && !callee->memberAccess.mangled) {
        funcType = genType(ctx, callee->resolved);
    } else if (LLVMGetValueKind(func) != LLVMFunctionValueKind) {
        /* Indirect call through a function pointer variable. */
        funcType = genType(ctx, callee->resolved);
    } else {
        funcType = LLVMGlobalGetValueType(func);
    }


    usize fixedParamCount = LLVMCountParamTypes(funcType);
    LLVMTypeRef *fixedParamTypes = NULL;
    if (fixedParamCount > 0) {
        fixedParamTypes = znalloc(LLVMTypeRef, fixedParamCount);
        LLVMGetParamTypes(funcType, fixedParamTypes);
    }

    for (usize i = 0; i < veclen(node->call.capabilities); i++) {
        if (!node->call.capabilities[i]) {
            warning(ctx->state, node->tok, "Empty capability\n");
            continue;
        } else if (!node->call.capabilities[i]->tok) {
            warning(ctx->state, node->tok, "Empty tok field\n");
            continue;
        }
        LLVMValueRef capability = getLLVMValueRef(
            ctx, node->call.capabilities[i]->tok->str
        );
        if (!capability) {
            error(
                ctx->state,
                node->call.capabilities[i]->tok,
                "Capability '%s' not found",
                stoken(node->call.capabilities[i]->tok)
            );
        } else {
            capability = LLVMBuildLoad2(
                ctx->builder,
                genType(ctx, node->call.capabilities[i]->resolved),
                capability, label(ctx, node->call.capabilities[i]->tok)
            );
        }
        vecpush(args, capability);
    }

    for (usize i = 0; i < veclen(node->call.args); i++) {
        LLVMValueRef arg = genExpr(ctx, node->call.args[i]);

        /* ABI adaptation: foreign functions declare small struct params as
         * i32/i64 (packed integer).  If the Zinc-side arg is a struct,
         * store it to a temp slot and reload as the packed integer so the
         * backend emits a single-register load instead of per-field loads.
         * FIXME: Implement a specific annotation [[packed]] instead of 'understand'
         * if the argument should be packed.
         * */
        if (fixedParamTypes && i < fixedParamCount) {
            LLVMTypeRef expected = fixedParamTypes[i];
            LLVMTypeRef actual   = LLVMTypeOf(arg);
            if (LLVMGetTypeKind(actual)   == LLVMStructTypeKind &&
                LLVMGetTypeKind(expected) == LLVMIntegerTypeKind) {
                LLVMValueRef tmp = LLVMBuildAlloca(ctx->builder, actual, "");
                LLVMBuildStore(ctx->builder, arg, tmp);
                arg = LLVMBuildLoad2(ctx->builder, expected, tmp, "");
            }
        }

        usize totalArgIndex = veclen(node->call.capabilities) + i;

        /* C default argument promotions for variadic arguments:
         *  f32         -> f64
         *  i1/i8/i16   -> i32
         *  The backend rely on the frontend to emit these, zinc must do it explicitly
         *  or callers like printf read garbage.
         */
        if (LLVMIsFunctionVarArg(funcType) && totalArgIndex >= (usize)fixedParamCount) {
            LLVMTypeRef argType = LLVMTypeOf(arg);
            LLVMTypeKind kind   = LLVMGetTypeKind(argType);
            if (kind == LLVMFloatTypeKind) {
                arg = LLVMBuildFPExt(ctx->builder, arg, f64Type, "");
            } else if (kind == LLVMIntegerTypeKind &&
                       LLVMGetIntTypeWidth(argType) < 32) {
                arg = LLVMBuildZExt(ctx->builder, arg, i32Type, "");
            }
        }

        vecpush(args, arg);
    }

    LLVMValueRef call = LLVMBuildCall2(
        ctx->builder,
        funcType,
        func,
        args,
        veclen(args),
        isVoid(node->resolved) ? "" : label(ctx, NULL)
    );

    if (!fitsInRegister(call)) {
        ZLLVMStack *stack = getStackValue(ctx, node);
        if (stack) {
            LLVMBuildStore(ctx->builder, call, stack->stack);
        }
    }
    return call;
}

/**
 * @brief Loads the addresso of the expression.
 *
 * genLvalue is used to load the address of the expression rather than the value.
 * meanwhile the function to load the value is genExpr.
 */
static LLVMValueRef genLvalue(ZCodegen *ctx, ZNode *node) {
    if (!node) return NULL;
    switch (node->type) {
    case NODE_ARRAY_LIT:        return genArrayLitPtr       (ctx, node);
    case NODE_SLICE:            return genSlicePtr          (ctx, node);
    case NODE_ENUM_LIT:         return genEnumLitPtr        (ctx, node);
    case NODE_STRUCT_LIT:       return genStructLitPtr      (ctx, node);
    case NODE_TUPLE_LIT:        return genTupleLitPtr       (ctx, node);
    case NODE_MEMBER:           return genMemberAccessPtr   (ctx, node);
    case NODE_SUBSCRIPT:        return genSubscriptPtr      (ctx, node);
    case NODE_CALL: {
        genCall(ctx, node);
        ZLLVMStack *stack = getStackValue(ctx, node);
        if (!stack) {
            error(ctx->state, node->tok, "Invalid call");
            return NULL;
        }
        return stack->stack;
    }
    case NODE_IDENTIFIER: {
        char *key = node->identNode.mangled ?
                    node->identNode.mangled :
                    stoken(node->tok);
        LLVMValueRef val = getLLVMValueRef(ctx, key);
        if (!val) {
            error(ctx->state, node->tok,
                    "'%s' not found in the current scope",
                    node->tok->str);
            return NULL;
        }
        return val;
    }
    case NODE_UNARY: {
        if (node->unary.operat->type != TOK_STAR) {
            error(ctx->state, node->tok, "Unhandled unary operator");
            return NULL;
        }

        LLVMValueRef ptr = genLvalue(ctx, node->unary.operand);
        LLVMTypeRef typeRef = genType(ctx, node->unary.operand->resolved);
        ptr = LLVMBuildLoad2(ctx->builder, typeRef, ptr, label(ctx, node->tok));
        return ptr;
    }
    default:
        error(ctx->state,
                node->tok,
                "Node '%d' not handled in 'genLvalue'",
                node->type);
        return NULL;
    }
}

static bool typeIsUnsigned(ZType *type) {
    if (!type || type->kind != Z_TYPE_PRIMITIVE) return false;
    return (bool)(type->primitive.token->type & TOK_UNSIGNED);
}

/**
 * @brief Generates inline if.
 *
 * This uses a common instruction of LLVM called phi.
 * In LLVM values are constants and can't be reassigned.
 * So to generates the inline if three blocks are emitted:
 * - The branch for the true value
 * - The branch for the false value
 * - The branch that takes the value from the previous one.
 *
 * It generates the condition and the conditional branch.
 * Then append to the true and false branch their respectively expressions.
 *
 * At the end it generates the phi instruction.
 * It takes the value of the branch comes from.
 */
static LLVMValueRef genInlineIf(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef cond = genExpr(ctx, node->ifStmt.cond);
    LLVMBasicBlockRef tBranch = makeblock(ctx);
    LLVMBasicBlockRef fBranch = makeblock(ctx);
    LLVMBasicBlockRef merge = makeblock(ctx);

    cond = LLVMBuildICmp(
        ctx->builder, LLVMIntNE, cond,
        LLVMConstInt(LLVMTypeOf(cond), 0, false),
        label(ctx, node->tok)
    );
    LLVMBuildCondBr(ctx->builder, cond, tBranch, fBranch);

    LLVMPositionBuilderAtEnd(ctx->builder, tBranch);
    LLVMValueRef tValue = genExpr(ctx, node->ifStmt.body);
    LLVMBasicBlockRef tExit = LLVMGetInsertBlock(ctx->builder);
    LLVMBuildBr(ctx->builder, merge);

    LLVMPositionBuilderAtEnd(ctx->builder, fBranch);
    LLVMValueRef fValue = genExpr(ctx, node->ifStmt.elseBranch);
    LLVMBasicBlockRef fExit = LLVMGetInsertBlock(ctx->builder);
    LLVMBuildBr(ctx->builder, merge);

    LLVMPositionBuilderAtEnd(ctx->builder, merge);
    LLVMTypeRef resultType = genType(ctx, node->resolved);
    LLVMValueRef phi = LLVMBuildPhi(
        ctx->builder, resultType, label(ctx, node->tok)
    );

    LLVMValueRef vals[2]            = {tValue,  fValue};
    LLVMBasicBlockRef branches[2]   = {tExit,   fExit};

    LLVMAddIncoming(phi, vals, branches, 2);

    return phi;
}

/**
 * @brief Generates null coalescing.
 * 
 * It uses the phi instruction to break the runtime and go directly to the merge.
 * Example: expr1 ?? expr2
 *
 * It generates expr1 and compare it to 0 (normal if condition).
 * If true it goes to the merge branch.
 * If false it takes the right value (also if expr2 is false)
 */
static LLVMValueRef genNullCoalescing(ZCodegen *ctx, ZNode *root) {
    LLVMTypeRef typeRef = genType(ctx, root->resolved);
    LLVMValueRef left   = genExpr(ctx, root->binary.left);
    LLVMValueRef cond   = LLVMBuildICmp(
        ctx->builder, LLVMIntNE, left,
        root->resolved->kind == Z_TYPE_POINTER ?
            LLVMConstPointerNull(typeRef) :
            LLVMConstInt(typeRef, 0, false), label(ctx, root->binary.left->tok)
    );

    LLVMBasicBlockRef entryBranch   = LLVMGetInsertBlock(ctx->builder);
    LLVMBasicBlockRef rightBranch   = makeblock(ctx);
    LLVMBasicBlockRef mergeBranch   = makeblock(ctx);

    LLVMBuildCondBr(ctx->builder, cond, mergeBranch, rightBranch);

    LLVMPositionBuilderAtEnd(ctx->builder, rightBranch);
    LLVMValueRef right  = genExpr(ctx, root->binary.right);
    rightBranch         = LLVMGetInsertBlock(ctx->builder);
    LLVMBuildBr(ctx->builder, mergeBranch);

    LLVMPositionBuilderAtEnd(ctx->builder, mergeBranch);
    LLVMValueRef phi                = LLVMBuildPhi(
        ctx->builder, typeRef, label(ctx, root->tok)
    );

    LLVMAddIncoming(phi,
        (LLVMValueRef[]){left, right},
        (LLVMBasicBlockRef[]){entryBranch, rightBranch},
        2
    );
    return phi;
}

/**
 * @brief Generates all binary operators.
 *
 * Binary operators are divided into:
 * - Integer operations.
 * - Boolean operations.
 * A special case is the assign that generates the assignment.
 */
static LLVMValueRef genBinary(ZCodegen *ctx, ZNode *root) {
    if (root->binary.op->type == TOK_EQ) {
        LLVMValueRef ptr = genLvalue(ctx, root->binary.left);
        LLVMValueRef val = genExpr(ctx, root->binary.right);
        if (!ptr || !val) return NULL;
        LLVMBuildStore(ctx->builder, val, ptr);
        return val;
    }

    ZTokenType op = root->binary.op->type;

    /* Null coalescing operator. */
    if (op == TOK_COALESCING) {
        return genNullCoalescing(ctx, root);
    }

    /* Logical operator. */
    if (op == TOK_AND || op == TOK_OR) {
        bool is_and = op == TOK_AND;

        LLVMValueRef lv = genExpr(ctx, root->binary.left);
        if (!lv) return NULL;
        LLVMValueRef lv_bool = LLVMBuildICmp(ctx->builder, LLVMIntNE, lv,
            LLVMConstInt(LLVMTypeOf(lv), 0, false), label(ctx, root->tok));

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
            LLVMConstInt(LLVMTypeOf(rv), 0, false), label(ctx, root->tok));
        LLVMBuildBr(ctx->builder, merge_bb);
        LLVMBasicBlockRef rhs_end = LLVMGetInsertBlock(ctx->builder);

        LLVMPositionBuilderAtEnd(ctx->builder, merge_bb);
        LLVMValueRef phi = LLVMBuildPhi(ctx->builder, i1Type, label(ctx, root->tok));
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


    bool bothUnsigned = typeIsUnsigned(root->binary.left->resolved) &&
                        typeIsUnsigned(root->binary.right->resolved);

    char *l = label(ctx, root->tok);

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
    }


    let div = bothUnsigned ? LLVMBuildUDiv : LLVMBuildSDiv;
    let mod = bothUnsigned ? LLVMBuildURem : LLVMBuildSRem;
    let rightShift = bothUnsigned ? LLVMBuildLShr : LLVMBuildAShr;

    let lt  = bothUnsigned ? LLVMIntULT : LLVMIntSLT;
    let gt  = bothUnsigned ? LLVMIntUGT : LLVMIntSGT;
    let lte = bothUnsigned ? LLVMIntULE : LLVMIntSLE;
    let gte = bothUnsigned ? LLVMIntUGE : LLVMIntSGE;

    switch (op) {
    case TOK_PLUS:  return LLVMBuildAdd (ctx->builder, left, right, l);
    case TOK_MINUS: return LLVMBuildSub (ctx->builder, left, right, l);
    case TOK_STAR:  return LLVMBuildMul (ctx->builder, left, right, l);
    case TOK_DIV:   return div          (ctx->builder, left, right, l);
    case TOK_MOD:   return mod          (ctx->builder, left, right, l);
    case TOK_LT:    return LLVMBuildICmp(ctx->builder, lt,  left, right, l);
    case TOK_GT:    return LLVMBuildICmp(ctx->builder, gt,  left, right, l);
    case TOK_LTE:   return LLVMBuildICmp(ctx->builder, lte, left, right, l);
    case TOK_GTE:   return LLVMBuildICmp(ctx->builder, gte, left, right, l);
    case TOK_EQEQ:  return LLVMBuildICmp(ctx->builder, LLVMIntEQ, left, right, l);
    case TOK_NOTEQ: return LLVMBuildICmp(ctx->builder, LLVMIntNE, left, right, l);
    case TOK_BITL:  return LLVMBuildShl (ctx->builder, left, right, l);
    case TOK_BITR:  return rightShift   (ctx->builder, left, right, l);
    case TOK_BITOR: return LLVMBuildOr  (ctx->builder, left, right, l);
    case TOK_BITXOR:return LLVMBuildXor (ctx->builder, left, right, l);
    case TOK_REF:   return LLVMBuildAnd (ctx->builder, left, right, l);
    default:        error(ctx->state, root->tok, "Unknown binary operator"); return NULL;
    }
}

static LLVMValueRef genUnary(ZCodegen *ctx, ZNode *node) {
    if (node->unary.operat->type == TOK_REF)
        return genLvalue(ctx, node->unary.operand);

    LLVMValueRef arg = genExpr(ctx, node->unary.operand);
    ZTokenType op = node->unary.operat->type;

    LLVMTypeRef argType = LLVMTypeOf(arg);
    bool isFloat = (LLVMGetTypeKind(argType) == LLVMFloatTypeKind ||
                     LLVMGetTypeKind(argType) == LLVMDoubleTypeKind);

    char *l = label(ctx, node->tok);
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
        LLVMValueRef loaded = LLVMBuildLoad2(ctx->builder, base, arg, l);
        /* Non-register loads (structs/enums) must land in the pre-allocated
         * slot so destructure/pattern-match callers can read from a stable
         * address  -  mirrors what genCall does for aggregate returns. */
        if (!fitsInRegister(loaded)) {
            ZLLVMStack *stack = getStackValue(ctx, node);
            if (stack) LLVMBuildStore(ctx->builder, loaded, stack->stack);
        }
        return loaded;
    }
    case TOK_NOT:   return LLVMBuildNot(ctx->builder, arg, l);
    case TOK_REF:   return genLvalue(ctx, node->unary.operand);
    case TOK_BITNOT: {
        LLVMTypeRef ref         = LLVMTypeOf(arg);
        LLVMValueRef allOnes    = LLVMConstAllOnes(ref);
        return LLVMBuildXor(ctx->builder, arg, allOnes, l);
     }

    default:
        error(ctx->state, node->unary.operat, "Unknown unary operator");
        return NULL;
    }

    return NULL;
}

/**
 * @brief Generates the explicit cast
 * Emit the appropriate LLVM cast to convert val (of Zinc type `from`) to
 * Zinc type `to`. Returns val unchanged when the types are already equal.
 */
static LLVMValueRef castValue(ZCodegen *ctx, LLVMValueRef val, ZType *from, ZType *to) {
    if (!val || !from || !to) return val;
    if (typesEqual(from, to)) return val;

    /* none (null pointer) is compatible with any pointer - val is already ptr null */
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

    char *l = label(ctx, NULL);
    unsigned toBits = LLVMGetTypeKind(toType) == LLVMIntegerTypeKind
        ? LLVMGetIntTypeWidth(toType) : 0;

    if (toBits == 1) {
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

    /* Array-literal cast: [n]T as []U - write each element directly into
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
                ctx->builder, stack->stackType,
                stack->stack, indices, 2, label(ctx, NULL));
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
static LLVMValueRef genStructLitInto(
        ZCodegen *ctx, ZNode *node, LLVMValueRef dest) {
    LLVMTypeRef structType  = genType(ctx, node->resolved);
    LLVMValueRef ptr        = dest ? dest :
                                LLVMBuildAlloca(
                                        ctx->builder, structType, label(ctx, node->tok));
    ZNode **fields          = node->structlit.fields;

    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *var = fields[i];
        ZToken *name = var->varDecl.pattern->tok;
        if (!var->varDecl.rvalue) {
            error(ctx->state,
                    var->tok,
                    "Missing rvalue in struct literal for field '%s'",
                    name);
        }
        LLVMValueRef val = genExpr(ctx, var->varDecl.rvalue);

        u32 *path = getStructIndex(node->resolved, name->str);
        ZNode *field = getStructField(node->resolved, name->str);

        if (!field) {
            error(ctx->state, name, "Unknown field '%s'", name->str);
            continue;
        }

        ZType *fieldType = field->field.type;
        val = castValue(ctx, val, var->varDecl.rvalue->resolved, fieldType);

        LLVMValueRef fieldPtr = genStructGEPChain(
            ctx, node->resolved, ptr, path
        );

        LLVMBuildStore(ctx->builder, val, fieldPtr);
    }
    return ptr;
}

static LLVMValueRef genStaticAccess(ZCodegen *ctx, ZNode *node) {
    if (!node || !node->resolved || node->resolved->kind != Z_TYPE_FUNCTION) {
        error(ctx->state, node->tok, "Invalid genStaticAccess");
        return NULL;
    }

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

static LLVMValueRef genArrayInit(ZCodegen *ctx, ZNode *node) {
    ZLLVMStack *stack = getStackValue(ctx, node);

    if (!stack) return NULL;

    storeArray(
        ctx, stack, LLVMConstInt(i64Type, node->arrayinit->array.size, false)
    );

    return stack->stack;
}

static void matchPattern(
    ZCodegen *,             ZType *,
    ZVarDestructPattern *,  LLVMValueRef, LLVMBasicBlockRef
);

static void matchTuplePattern(
        ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
        LLVMValueRef ptr, LLVMBasicBlockRef failBranch) {
    LLVMTypeRef typeRef = genType(ctx, type);
    for (usize i = 0; i < veclen(pattern->tuple); i++) {
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, typeRef, ptr, i, label(ctx, pattern->tuple[i]->tok));
        matchPattern(
            ctx, type->tuple[i],
            pattern->tuple[i], fieldPtr, failBranch);
    }
}

static void matchStructPattern(
        ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
        LLVMValueRef ptr, LLVMBasicBlockRef failBranch) {
    LLVMTypeRef typeRef = genType(ctx, type);
    for (usize i = 0; i < veclen(pattern->fields); i++) {
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, typeRef, ptr, i, label(ctx, pattern->fields[i]->key)
        );
        matchPattern(
            ctx, type->strct.fields[i]->resolved,
            pattern->fields[i]->value, fieldPtr, failBranch
        );
    }
}

static void matchEnumPattern(
        ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
        LLVMValueRef ptr, LLVMBasicBlockRef failBranch) {
    i32 variantIndex = enumIndexField(type, pattern->prop);

    if (variantIndex == -1) {
        error(ctx->state,
            pattern->prop,
            "Enum variant '%s' not found",
            stoken(pattern->prop)
        );
        return;
    }
    ZType *variantType = type->enm.fields[variantIndex];

    LLVMValueRef tagPtr = LLVMBuildStructGEP2(
        ctx->builder, genType(ctx, type), ptr, 0, label(ctx, pattern->prop));

    LLVMValueRef tag = LLVMBuildLoad2(
        ctx->builder, i8Type, tagPtr, label(ctx, pattern->prop)
    );

    LLVMValueRef cond = LLVMBuildICmp(
        ctx->builder, LLVMIntEQ, tag,
        LLVMConstInt(i8Type, variantIndex, false),
        label(ctx, pattern->prop)
    );

    LLVMBasicBlockRef entry = makeblock(ctx);
    LLVMBuildCondBr(ctx->builder, cond,
            entry, failBranch);

    LLVMPositionBuilderAtEnd(ctx->builder, entry);
    LLVMTypeRef variantTypeRef = genType(ctx, variantType);

    for (usize i = 0; i < veclen(pattern->args); i++) {
        LLVMValueRef fieldPtr = LLVMBuildStructGEP2(
            ctx->builder, variantTypeRef, ptr,
            i + 1, label(ctx, pattern->args[i]->tok));

        matchPattern(ctx,
            variantType->strct.fields[i + 1]->resolved,
            pattern->args[i], fieldPtr, failBranch);
    }
}

static void matchPattern(
        ZCodegen *ctx,
        ZType *type,
        ZVarDestructPattern *pattern,
        LLVMValueRef ptr,
        LLVMBasicBlockRef failBranch) {
    switch (pattern->type) {
    case Z_VAR_IDENT:                                                               break;
    case Z_VAR_TUPLE:   matchTuplePattern    (ctx, type, pattern, ptr, failBranch); break;
    case Z_VAR_STRUCT:  matchStructPattern   (ctx, type, pattern, ptr, failBranch); break;
    case Z_VAR_ENUM:    matchEnumPattern     (ctx, type, pattern, ptr, failBranch); break;
    default:
        error(ctx->state, pattern->tok, "Unknown pattern %d", pattern->type);
        break;
    }
}

static LLVMValueRef genMatchCond(
    ZCodegen *ctx,                  ZType *type,
    ZVarDestructPattern *pattern,   LLVMValueRef ptr) {
    LLVMBasicBlockRef success   = makeblock(ctx);
    LLVMBasicBlockRef failure   = makeblock(ctx);
    LLVMBasicBlockRef merge     = makeblock(ctx);

    matchPattern(ctx, type, pattern, ptr, failure);
    LLVMBuildBr(ctx->builder, success);

    LLVMPositionBuilderAtEnd(ctx->builder, success);
    LLVMValueRef trueVal        = LLVMConstInt(i1Type, 1, false);
    LLVMBuildBr(ctx->builder, merge);

    LLVMPositionBuilderAtEnd(ctx->builder, failure);
    LLVMValueRef falseVal       = LLVMConstInt(i1Type, 0, false);
    LLVMBuildBr(ctx->builder, merge);

    LLVMPositionBuilderAtEnd(ctx->builder, merge);
    LLVMValueRef phi = LLVMBuildPhi(ctx->builder, i1Type, label(ctx, pattern->tok));

    LLVMAddIncoming(
        phi,
        (LLVMValueRef[])        {   trueVal, falseVal   },
        (LLVMBasicBlockRef[])   {   success, failure    },
        2
    );
    return phi;
}

static LLVMValueRef genVarDestruct(ZCodegen *ctx, ZNode *node) {
    genVarDecl(ctx, node);
    ZNode *rvalue = node->varDecl.rvalue;
    LLVMValueRef ptr = NULL;

    if (rvalue->type == NODE_IDENTIFIER) {
        char *key = rvalue->identNode.mangled ? rvalue->identNode.mangled : rvalue->tok->str;
        ptr = getLLVMValueRef(ctx, key);
        if (!ptr) {
            error(ctx->state, rvalue->tok, "'%s' not found", rvalue->tok->str);
            return NULL;
        }
    } else {
        ZLLVMStack *stack = getStackValue(ctx, rvalue);
        if (!stack) {
            error(ctx->state, node->tok, "Missing stack value");
            return NULL;
        }
        ptr = stack->stack;
    }

    putDestructuredPatternInStack(ctx, node->resolved, node->varDecl.pattern, ptr);
    return genMatchCond(ctx, node->resolved, node->varDecl.pattern, ptr);
}

static LLVMValueRef genExpr(ZCodegen *ctx, ZNode *node) {
    switch (node->type) {
        case NODE_IF:               return genInlineIf      (ctx, node);
        case NODE_CALL:             return genCall          (ctx, node);
        case NODE_CAST:             return genCast          (ctx, node);
        case NODE_UNARY:            return genUnary         (ctx, node);
        case NODE_BINARY:           return genBinary        (ctx, node);
        case NODE_LITERAL:          return genLit           (ctx, node);
        case NODE_ARRAY_INIT:       return genArrayInit     (ctx, node);
        case NODE_IDENTIFIER:       return genIdent         (ctx, node);
        case NODE_STATIC_ACCESS:    return genStaticAccess  (ctx, node);
        case NODE_VAR_DECL:         return genVarDestruct   (ctx, node);

        case NODE_MEMBER:
        case NODE_SUBSCRIPT:
        case NODE_TUPLE_LIT:
        case NODE_STRUCT_LIT:
        case NODE_ARRAY_LIT:
        case NODE_ENUM_LIT:
        case NODE_SLICE: {
            LLVMValueRef ptr = genLvalue(ctx, node);
            /* Function fields are stored as ptr in structs (unsized types can't
             * be embedded directly), so load as ptr rather than the bare
             * function type. */
            LLVMTypeRef loadType = genType(ctx, node->resolved);
            if (node->resolved->kind == Z_TYPE_FUNCTION) {
                loadType = LLVMPointerType(loadType, 0);
            }
            return LLVMBuildLoad2(
                ctx->builder,   loadType,
                ptr,            label(ctx, node->tok)
            );
        }

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

/* A Homogeneous Float Aggregate (HFA) is a struct with 1-4 fields all of the
 * same float type (f32 or f64).  HFAs are passed in SIMD/FP registers on both
 * AArch64 and x86-64, so they must NOT be repacked as integers. */
static bool isHFA(ZType *type) {
    if (type->kind != Z_TYPE_STRUCT) return false;
    usize n = veclen(type->strct.fields);
    if (n == 0 || n > 4) return false;

    ZType *first = type->strct.fields[0]->resolved;
    if (!first || first->kind != Z_TYPE_PRIMITIVE) return false;
    ZTokenType ft = first->primitive.token->type;
    if (ft != TOK_F32 && ft != TOK_F64) return false;

    for (usize i = 1; i < n; i++) {
        ZType *f = type->strct.fields[i]->resolved;
        if (!f || f->kind != Z_TYPE_PRIMITIVE) return false;
        if (f->primitive.token->type != ft) return false;
    }
    return true;
}

static LLVMValueRef genForeign(ZCodegen *ctx, ZNode *node) {
    LLVMTypeRef ret = genType(ctx, node->foreignFunc.ret);
    usize argc = veclen(node->foreignFunc.args);

    LLVMTypeRef *paramTypes = znalloc(LLVMTypeRef, argc ? argc : 1);
    for (usize i = 0; i < argc; i++) {
        ZType *at = node->foreignFunc.args[i];
        paramTypes[i] = genType(ctx, at);
        if (at->kind == Z_TYPE_FUNCTION) {
            paramTypes[i] = LLVMPointerType(paramTypes[i], 0);
        } else if (at->kind == Z_TYPE_STRUCT && !isHFA(at)) {
            /* C ABI on all supported targets (x86-64, AArch64) passes small
             * non-HFA structs as a packed integer of the same size.  HFAs use
             * SIMD/FP registers and LLVM handles them correctly as-is. */
            usize sz = typeSize(ctx, at);
            if      (sz <= 4) paramTypes[i] = i32Type;
            else if (sz <= 8) paramTypes[i] = i64Type;
        }
    }
    LLVMTypeRef funcType = LLVMFunctionType(
        ret,
        paramTypes,
        (unsigned)argc,
        node->resolved->func.variadic
    );

    LLVMValueRef func = LLVMGetNamedFunction(ctx->mod, node->foreignFunc.tok->str);
    if (!func) {
        func = LLVMAddFunction(ctx->mod, node->foreignFunc.tok->str, funcType);
    }

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

static inline bool LLVMCanInsertRet(ZCodegen *ctx) {
    return !LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder));
}

static LLVMValueRef genRet(ZCodegen *ctx, ZNode *ret) {
    if (!ret->returnStmt.expr || isVoid(ret->resolved)) {
        if (ret->returnStmt.expr) genExpr(ctx, ret->returnStmt.expr);
        genRetChainDefer(ctx);
        if (!LLVMCanInsertRet(ctx)) return NULL;
        return LLVMBuildRetVoid(ctx->builder);
    }

    LLVMValueRef val = genExpr(ctx, ret->returnStmt.expr);

    /* If the expression produced i1 (e.g. a comparison) but the
       function's declared return type is a wider integer, zero-extend. */
    LLVMTypeRef funcType = LLVMGlobalGetValueType(ctx->currentFunc);
    LLVMTypeRef retType  = LLVMGetReturnType(funcType);
    if (LLVMTypeOf(val) == i1Type && retType != i1Type &&
            LLVMGetTypeKind(retType) == LLVMIntegerTypeKind) {
        val = LLVMBuildZExt(ctx->builder, val, retType, label(ctx, ret->tok));
    }

    genRetChainDefer(ctx);

    if (!LLVMCanInsertRet(ctx)) return NULL;
    return LLVMBuildRet(ctx->builder, val);
}

/**
 * @brief Generate if.
 */
static void genIf(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_BLOCK, ctx);
    bool hasElse = node->ifStmt.elseBranch != NULL;
    LLVMBasicBlockRef then = makeblock(ctx);

    LLVMBasicBlockRef elseBranch = NULL;

    if (hasElse) elseBranch = makeblock(ctx);

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

/**
 * @brief Generate while statement.
 *
 * The while statement generates three blocks:
 * - The entry block: where it checks the condition and emit a conditional jump.
 * - The body: it generates each statement of the while loop.
 * - The end block: it is the end of the block.
 *      it can be reached by the conditional jump of the entry block or
 *      a break statement inside the body.
 */
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

/**
 * @brief Generates for statement.
 */
static void genFor(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_LOOP, ctx);
    LLVMBasicBlockRef entry     = makeblock(ctx);
    LLVMBasicBlockRef body      = makeblock(ctx);
    LLVMBasicBlockRef endfor    = makeblock(ctx);

    /* Save the labels for the continue and break statement. */
    ctx->scope->startLoop       = entry;
    ctx->scope->endLoop         = endfor;

    // Bind the loop variable in the loop scope so it shadows any stale
    // function-scope entry that was inserted by the pre-pass (genFuncVars).
    if (node->forStmt.var) {
        ZNode *varDecl = node->forStmt.var;
        ZLLVMStack *loopSlot = getStackValue(ctx, varDecl->varDecl.rvalue);
        if (loopSlot) {
            putDestructuredPatternInStack(
                ctx, varDecl->resolved, varDecl->varDecl.pattern, loopSlot->stack);
        }
    }
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

/**
 * @brief Genearetes break instruction.
 *
 * The break statement is just a jump to the end of the block.
 * The end of the block is a label saved in the current scope
 * when a loop is generated.
 */
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

/**
 * @brief Generate the continue instruction.
 *
 * The 'continue' instruction is just a jump to the start of the loop.
 * The start of the loop is a label saved during the loop generation.
 */
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

/**
 * @brief Generate defer statement
 *
 * Defer statement is compiled using a per-scope stack.
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

/**
 * @breif Generate capability.
 *
 * Capabilities are compiled like normal variables.
 */
static void genCapability(ZCodegen *ctx, ZNode *node) {
    genStmt(ctx, node->capability.capability);
    genStmt(ctx, node->capability.block);
}

static void genStmt(ZCodegen *ctx, ZNode *stmt) {
    if (!stmt) return;
    
    switch (stmt->type) {
    /* Variable already declared at the start of the function*/
    case NODE_IF:           genIf           (ctx, stmt);    break;
    case NODE_FOR:          genFor          (ctx, stmt);    break;
    case NODE_RETURN:       genRet          (ctx, stmt);    break;
    case NODE_CALL:         genCall         (ctx, stmt);    break;
    case NODE_BLOCK:        genBlock        (ctx, stmt);    break;
    case NODE_WHILE:        genWhile        (ctx, stmt);    break;
    case NODE_BREAK:        genBreak        (ctx, stmt);    break;
    case NODE_DEFER:        genDefer        (ctx, stmt);    break;
    case NODE_VAR_DECL:     genVarDecl      (ctx, stmt);    break;
    case NODE_CONTINUE:     genContinue     (ctx, stmt);    break;
    case NODE_CAPABILITY:   genCapability   (ctx, stmt);    break;
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

/**
 * @brief Saves the stack allocation indexed by node.
 */
static void addFuncVar(ZCodegen *ctx,
    LLVMValueRef stack,
    LLVMValueRef elem,
    LLVMTypeRef stackType,
    LLVMTypeRef elemType,
    ZNode *node) {

    ZLLVMStack *item    = zalloc(ZLLVMStack);
    *item = (ZLLVMStack){
        .stack          = stack,
        .elem           = elem,
        .stackType      = stackType,
        .elemType       = elemType,
        .node           = node,
    };
    vecpush(ctx->scope->stackAlloca, item);
}

/**
 * @brief Stores nested pointer for a given stack allocation.
 *
 * Saves the pointer for nested struct literals to avoi creating
 * another stack allocation.
 * Example:
 * MyStruct{
 *   field: OtherStruct{ ...fields }
 * }
 *
 * In this case the stack allocation has the size of MyStruct.
 * For the nested struct (OtherStruct) It uses just a pointer
 * to the previous stack allocation.
 *
 * */
static void buildNestedFuncVar(
    ZCodegen *ctx, ZNode *node, LLVMValueRef parent) {
    if (node->type == NODE_STRUCT_LIT) {
        ZNode **fields      = node->structlit.fields;

        for (usize i = 0; i < veclen(fields); i++) {
            ZNode *val      = fields[i]->varDecl.rvalue;
            ZToken *name    = fields[i]->varDecl.pattern->tok;
            ZType *type     = val->resolved;

            if (type->kind == Z_TYPE_PRIMITIVE  ||
                type->kind == Z_TYPE_FUNCTION   ||
                type->kind == Z_TYPE_POINTER    ) {
                continue;
            }
            
            u32 *path       = getStructIndex(
                node->resolved,
                name->str
            );

            if (veclen(path) == 0) {
                error(ctx->state, name, "Field not found");
                continue;
            }

            LLVMValueRef ptr = genStructGEPChain(
                ctx, node->resolved, parent, path
            );

            LLVMTypeRef typeRef = genType(ctx, type);

            addFuncVar(ctx, ptr, NULL, typeRef, NULL, val);
            buildNestedFuncVar(ctx, val, ptr);
        }
    } else if (node->type == NODE_ARRAY_LIT) {
        LLVMTypeRef elemType = genType(ctx, node->resolved->array.base);
        for (usize i = 0; i < veclen(node->arraylit); i++) {
            ZNode *val  = node->arraylit[i];
            LLVMValueRef index = LLVMConstInt(i32Type, i, false);
            LLVMValueRef ptr = LLVMBuildGEP2(
                ctx->builder, elemType, parent, &index, 1, "index"
            );

            LLVMTypeRef typeRef = genType(ctx, val->resolved);
            LLVMValueRef innerElem = NULL;
            LLVMTypeRef innerElemType = NULL;
            LLVMValueRef nestedParent = ptr;
            if (val->resolved->kind == Z_TYPE_ARRAY && val->resolved->array.size > 0) {
                innerElemType = LLVMArrayType2(
                    genType(ctx, val->resolved->array.base),
                    val->resolved->array.size
                );
                innerElem = LLVMBuildAlloca(ctx->builder, innerElemType, label(ctx, val->tok));
                nestedParent = innerElem;
            }
            addFuncVar(ctx, ptr, innerElem, typeRef, innerElemType, val);
            buildNestedFuncVar(ctx, val, nestedParent);
        }
    } else if (node->type == NODE_ENUM_LIT) {
        ZToken *variant = node->call.callee->staticAccess.prop;
        i32 variantIndex = enumIndexField(
            node->resolved, variant
        );
        ZType *variantType = node->resolved->enm.fields[variantIndex];
        LLVMTypeRef variantTypeRef = genType(ctx, variantType);
        for (usize i = 0; i < veclen(node->call.args); i++) {
            ZNode *val = node->call.args[i];
            LLVMValueRef ptr = LLVMBuildStructGEP2(ctx->builder,
                variantTypeRef, parent, i + 1, label(ctx, variant)
            );
            LLVMTypeRef fieldType = genType(ctx, variantType->strct.fields[i]->resolved);
            addFuncVar(ctx, ptr, NULL, fieldType, NULL, val);
            buildNestedFuncVar(ctx, val, ptr);
        }
    }
}

/* @brief Generates the stack allocation.
 *
 * Stores the node in a list of stack allocation such that in the second pass
 * the expression knows where it should be stored.
 * NOTE: this method stores always the expression that requires the stack allocation.
 * So for variable declaration always store the rvalue node and not the variable node.
 *
 * @param force Forces a stack allocation also when the size fits in a register.
 *
 * NOTE: Array literals generates two allocations:
 * - One for the metadata {len + ptr} (saved in stack)
 * - One for the items (saved in elem)
 * */
static LLVMValueRef buildFuncVar(ZCodegen *ctx, ZNode *node, bool force) {
    if (!node) {
        error(ctx->state, NULL, "'buildFuncVar' called with a null node");
        return NULL;
    } else if (!node->resolved) {
        error(ctx->state, node->tok,
                "Missing resolved type for node %d", node->type);
        return NULL;;
    }

    /* Function types are passed as pointers and fit in a register - no alloca needed. */
    if (!force && node->resolved->kind == Z_TYPE_FUNCTION) return NULL;

    LLVMValueRef stackPointer = NULL;
    LLVMValueRef elem = NULL;
    LLVMTypeRef elemType = NULL;
    if (node->resolved->kind == Z_TYPE_ARRAY && node->resolved->array.size > 0) {
        elemType = LLVMArrayType2(
            genType(ctx, node->resolved->array.base),
            node->resolved->array.size
        );
        elem = LLVMBuildAlloca(ctx->builder, elemType, label(ctx, node->tok));
        stackPointer = elem;
    }

    LLVMTypeRef type = genType(ctx, node->resolved);
    LLVMValueRef val = LLVMBuildAlloca(ctx->builder, type, label(ctx, node->tok));
    addFuncVar(ctx, val, elem, type, elemType, node);

    if (!stackPointer) stackPointer = val;
    buildNestedFuncVar(ctx, node, stackPointer);
    return val;
}

/* @brief Walks the AST of a function block to create stack allocations.
 *
 * All variables of the function body are allocated at the start of the block.
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
        buildFuncVar(ctx, node, false);
        for (usize i = 0; i < veclen(node->tuplelit); i++) {
            genFuncVars(ctx, node->tuplelit[i]);
        }
        break;
    case NODE_STRUCT_LIT:
        buildFuncVar(ctx, node, false);
        break;
    case NODE_ENUM_LIT:
        buildFuncVar(ctx, node, false);
    case NODE_CAST:
        genFuncVars(ctx, node->castExpr.expr);
        break;
    case NODE_ARRAY_LIT:
        buildFuncVar(ctx, node, false);
        break;
    case NODE_ARRAY_INIT:
        buildFuncVar(ctx, node, false);
        break;
    case NODE_MEMBER:
        genFuncVars(ctx, node->memberAccess.object);
        break;
    case NODE_SUBSCRIPT:
        genFuncVars(ctx, node->subscript.arr);
        genFuncVars(ctx, node->subscript.index);
        break;
    case NODE_FOR:
        if (node->forStmt.var) {
            buildFuncVar(ctx, node->forStmt.var->varDecl.rvalue, true);
        }
        genFuncVars(ctx, node->forStmt.block);
        break;
    case NODE_WHILE:
        genFuncVars(ctx, node->whileStmt.cond);
        genFuncVars(ctx, node->whileStmt.branch);
        break;
    case NODE_IF:
        genFuncVars(ctx, node->ifStmt.cond);
        genFuncVars(ctx, node->ifStmt.body);
        genFuncVars(ctx, node->ifStmt.elseBranch);
        break;
    case NODE_CAPABILITY:
        genFuncVars(ctx, node->capability.capability);
        genFuncVars(ctx, node->capability.block);
        break;
    case NODE_UNARY:
        genFuncVars(ctx, node->unary.operand);
        break;
    case NODE_VAR_DECL: {
        LLVMValueRef ptr = buildFuncVar(ctx, node->varDecl.rvalue, true);
        putDestructuredPatternInStack(ctx, node->resolved, node->varDecl.pattern, ptr);
        break;
    }
    case NODE_CALL:
        if (!node->resolved) {
            error(ctx->state, node->tok, "Unresolved type");
            break;
        }
        if (!typesPrimitive(node->resolved)) {
            buildFuncVar(ctx, node, false);
        }
        for (usize i = 0; i < veclen(node->call.args); i++) {
            ZNode *arg = node->call.args[i];
            genFuncVars(ctx, arg);
            if (!typesPrimitive(arg->resolved) && !getStackValue(ctx, arg)) {
                buildFuncVar(ctx, arg, false);
            }
        }
        break;

    case NODE_DEFER:
        genFuncVars(ctx, node->deferStmt.expr);
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
    default:
        break;
    }
}

static void addFuncArgs(ZCodegen *ctx,
        LLVMValueRef func,
        ZNode **funcArgs, usize paramOffset) {
    for (usize i = 0; i < veclen(funcArgs); i++) {
        char *name = funcArgs[i]->field.identifier->str;
        ZType *argType = funcArgs[i]->field.type;
        LLVMTypeRef paramType = NULL;
        LLVMValueRef slot = NULL;
        paramType = genType(ctx, argType);
        if (argType->kind == Z_TYPE_FUNCTION) {
            paramType = LLVMPointerType(
                LLVMInt8TypeInContext(ctx->ctx), 0
            );
            slot = LLVMGetParam(func, i + paramOffset);
        } else {
            slot = LLVMBuildAlloca(ctx->builder, paramType, name);
            LLVMBuildStore(ctx->builder, LLVMGetParam(func, i + paramOffset), slot);
        }
        putLLVMValueRef(ctx, name, slot);
    }
}

static LLVMValueRef genFunc(ZCodegen *ctx, ZNode *f) {
    LLVMTypeRef ret = genType(ctx, f->funcDef.ret);
    LLVMTypeRef *args = NULL;

    if (!f->funcDef.mangled) {
        f->funcDef.mangled = mangler((ZToken*[]) { f->funcDef.name, NULL });
    }

    if (f->funcDef.receiver) {
        LLVMTypeRef receiverType = genType(ctx, f->funcDef.receiver->resolved);
        vecpush(args, receiverType);
    }

    for (usize i = 0; i < veclen(f->funcDef.capabilities); i++) {
        LLVMTypeRef refType = genType(ctx, f->funcDef.capabilities[i]->field.type);
        vecpush(args, refType);
    }

    for (usize i = 0; i < veclen(f->funcDef.args); i++) {
        ZType *at = f->funcDef.args[i]->field.type;
        LLVMTypeRef arg = genType(ctx, at);
        if (at->kind == Z_TYPE_FUNCTION)
            arg = LLVMPointerType(arg, 0);
        vecpush(args, arg);
    }

    LLVMTypeRef funcType = LLVMFunctionType(ret, args, veclen(args), false);
    LLVMValueRef func = LLVMGetNamedFunction(ctx->mod, f->funcDef.mangled);
    if (!func)
        func = LLVMAddFunction(ctx->mod, f->funcDef.mangled, funcType);
    ctx->currentFunc = func;

    putLLVMValueRef(ctx, f->funcDef.mangled, func);
    beginScope(Z_SCOPE_FUNC, ctx);
    LLVMBasicBlockRef entry = makeblock(ctx);
    LLVMPositionBuilderAtEnd(ctx->builder, entry);

    /* Allocate a stack slot for each parameter so they can be reassigned.
     * The receiver (if present) occupies param index 0, so regular args
     * start at offset 1. */
    usize paramOffset = 0;
    if (f->funcDef.receiver) {
        char *name = f->funcDef.receiver->field.identifier->str;
        LLVMTypeRef recType = genType(ctx, f->funcDef.receiver->resolved);
        LLVMValueRef slot = LLVMBuildAlloca(ctx->builder, recType, name);
        LLVMBuildStore(ctx->builder, LLVMGetParam(func, 0), slot);
        putLLVMValueRef(ctx, name, slot);
        paramOffset++;
    }

    addFuncArgs(ctx, func, f->funcDef.capabilities, paramOffset);
    addFuncArgs(
        ctx, func,
        f->funcDef.args,
        paramOffset + veclen(f->funcDef.capabilities)
    );

    /* All variable declarations are declared at the start of the function. */
    genFuncVars(ctx, f->funcDef.body);

    genBlock(ctx, f->funcDef.body);

    genChainDefer(ctx, ctx->scope->parent);

    /* Add implicit return only if the block has no terminator yet. */
    if (!LLVMGetBasicBlockTerminator(LLVMGetInsertBlock(ctx->builder))) {
        LLVMTypeRef funcType = LLVMGlobalGetValueType(ctx->currentFunc);
        LLVMTypeRef retType  = LLVMGetReturnType(funcType);
        if (LLVMGetTypeKind(retType) == LLVMVoidTypeKind)
            LLVMBuildRetVoid(ctx->builder);
        else
            LLVMBuildRet(ctx->builder, LLVMConstNull(retType));
    }
    endScope(ctx);


    return func;
}

static void compile(ZCodegen *, ZNode *);
static void genForwardDecl(ZCodegen *, ZNode *);

static void genNamespace(ZCodegen *ctx, ZNode *node) {
    for (usize i = 0; i < veclen(node->block); i++) {
        compile(ctx, node->block[i]);
    }
}

static void compile(ZCodegen *ctx, ZNode *root) {
    switch (root->type) {
    case NODE_ENUM:
    case NODE_STRUCT:       genType     (ctx, root->resolved);  break;
    case NODE_FOREIGN:      genForeign  (ctx, root);            break;
    case NODE_FUNC:         genFunc     (ctx, root);            break;
    case NODE_NAMESPACE:    genNamespace(ctx, root);            break;
    case NODE_TYPEDEF:
    case NODE_MACRO:        /* Doesn't generate anything. */    break;
    case NODE_MODULE:
        beginModule(ctx, root);
        /* Pass 1: emit all struct/enum LLVM types so forward declarations
         * can reference them without hitting getCachedStruct misses. */
        for (usize i = 0; i < veclen(root->module.root); i++) {
            ZNode *child = root->module.root[i];
            if (child->type == NODE_STRUCT || child->type == NODE_ENUM)
                genType(ctx, child->resolved);
        }
        /* Pass 2: emit forward declarations for every function so
         * out-of-order and mutually-recursive calls resolve correctly. */
        for (usize i = 0; i < veclen(root->module.root); i++)
            genForwardDecl(ctx, root->module.root[i]);
        /* Pass 3: emit function bodies and remaining declarations. */
        for (usize i = 0; i < veclen(root->module.root); i++)
            compile(ctx, root->module.root[i]);
        endModule(ctx);
        break;
    default:
        error(ctx->state, root->tok, "(compilation not yet implemented for %d)", root->type);
        break;
    }
}

static void genForwardDecl(ZCodegen *ctx, ZNode *node) {
    switch (node->type) {
    case NODE_NAMESPACE:
        for (usize i = 0; i < veclen(node->block); i++)
            genForwardDecl(ctx, node->block[i]);
        break;
    case NODE_FUNC: {
        if (!node->funcDef.mangled)
            node->funcDef.mangled = mangler((ZToken*[]) { node->funcDef.name, NULL });

        LLVMTypeRef ret      = genType(ctx, node->funcDef.ret);
        LLVMTypeRef *args    = NULL;

        if (node->funcDef.receiver) {
            vecpush(args, genType(ctx, node->funcDef.receiver->resolved));
        }
        for (usize i = 0; i < veclen(node->funcDef.capabilities); i++) {
            vecpush(args, genType(ctx, node->funcDef.capabilities[i]->field.type));
        }
        for (usize i = 0; i < veclen(node->funcDef.args); i++) {
            ZType *at = node->funcDef.args[i]->field.type;
            LLVMTypeRef arg = genType(ctx, at);
            if (at->kind == Z_TYPE_FUNCTION)
                arg = LLVMPointerType(arg, 0);
            vecpush(args, arg);
        }

        LLVMTypeRef funcType = LLVMFunctionType(ret, args, veclen(args), false);
        LLVMValueRef decl    = LLVMAddFunction(ctx->mod, node->funcDef.mangled, funcType);
        putLLVMValueRef(ctx, node->funcDef.mangled, decl);
        break;
    }
    default: break;
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
#ifdef _WIN32
    /* The PE linker uses the exact name given; ensure .exe extension. */
    char *outname_buf = NULL;
    {
        size_t n = strlen(outname);
        if (n < 4 || strcmp(outname + n - 4, ".exe") != 0) {
            outname_buf = (char *)malloc(n + 5);
            memcpy(outname_buf, outname, n);
            memcpy(outname_buf + n, ".exe", 5);
            outname = outname_buf;
        }
    }
#endif

    int ret = zinc_lld_link(objfile, outname,
            (const char**)state->extraArgs, veclen(state->extraArgs));
    if (ret != 0) {
        error(state, NULL, "Linker failed with code %d", ret);
    } else {
        printf(COLOR_BLUE COLOR_BOLD "  Generated " COLOR_RESET "%s\n", outname);
    }
#ifdef _WIN32
    free(outname_buf);
#endif

    remove(objfile);

    freeCodegen(ctx);
}
