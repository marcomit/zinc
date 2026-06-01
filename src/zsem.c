// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

/* This file is the Semantic analyzer.
 *
 * Now what is a semantic analyzer?
 * In this part of the compiler/interpreter
 * we have already built the AST (Abstract Syntax Tree) and
 * we want to make sure that all the types are correctly.
 * for example this expression 1230 + "hello"
 * is not valid because i cannot add a number to a string
 * but the parser parse it correctly. so we need to check every single expression/statement
 * that all types are correctly.
 * In this phase we also care about the scope of functions/variables.
 *
 * How do we handle the scope?
 *
 * We have a global scope for the entire project,
 * then a child scope for the current file and then a child for blocks like functions, loops etc..
 * */
#include "base.h"
#include "zhset.h"
#include "zinc.h"
#include "zvec.h"
#include "zarena.h"
#include <stdbool.h>

/* The semantic analyzer has 2 phases.
 * 1. Analyze only the declarations such that every
 *  module knows the other declarations and they don't need
 *  to write to other modules.
 * 2. Creates a thread's pool where each thread analyze N functions.
 *  So when a function is analyzed it doesn't need to wrinte in the global scope
 *  because it is already writte in the phase 1.
 *  To to the second phase The semantic analyzer needs a shared context (ZSemantic)
 *  and a thread local context (the struct below) to make sure every function
 *  has its own thread-safe state.
 * */
typedef struct {
    ZSemantic   *semantic;
    arena_t     *arena;
    ZType       *currentFuncRet;
    ZNode       *currentFunc;
    u16         loopDepth;
} ZThreadSem;

static void analyze(ZSemantic *, ZNode *);
static void analyzeStmt(ZSemantic *, ZNode *);
static void analyzeBlock(ZSemantic *, ZNode *, bool);
static ZType *resolveTypeRef(ZSemantic *, ZType *);
static void checkFunctionUsedAsValue(ZSemantic *, ZNode *);

/* ================== Scope / Symbol helpers ================== */

static ZType *none      = NULL;
static ZType *u1Type    = NULL;
static ZType *u64Type   = NULL;

static ZScope *makescope(ZScope *parent, ZNode *node) {
    ZScope *self        = zalloc(ZScope);
    self->depth         = parent ? parent->depth + 1 : 0;
    self->parent        = parent;
    self->node          = node;
    self->symbols       = NULL;
    self->seen          = NULL;
    self->capabilities  = NULL;
    return self;
}

// static ZThreadSem *makethreadsem(ZSemantic *ctx) {
//     ZThreadSem *self        = zalloc(ZThreadSem);
//
//     self->arena             = createArena();
//     self->currentFunc       = NULL;
//     self->currentFuncRet    = NULL;
//     self->loopDepth         = 0;
//     self->semantic          = ctx;
//
//     return self;
// }

static ZSymbol *makesymbol(ZSymType kind) {
    ZSymbol *self       = zalloc(ZSymbol);
    self->kind          = kind;
    self->useCount      = 0;
    self->reachable     = false;
    return self;
}

static ZSymTable *makesymtable(void) {
    ZSymTable *self     = zalloc(ZSymTable);
    self->global        = makescope(NULL, NULL);
    self->current       = self->global;
    self->module        = NULL;
    self->funcs         = NULL;
    return self;
}

static ZSemantic *makesemantic(ZState *state, ZNode *root) {
    ZSemantic *self         = zalloc(ZSemantic);
    self->root              = root;
    self->currentFuncRet    = NULL;
    self->currentFunc       = NULL;
    self->loopDepth         = 0;
    self->state             = state;
    self->table             = makesymtable();
    self->scopes            = NULL;
    self->seen              = NULL;
    return self;
}


static void putSymbol(ZSemantic *ctx, ZSymbol *symbol) {
    ZScope *scope = ctx->table->current;

    if (symbol->isPublic) scope = ctx->table->global;


    if (strcmp(symbol->name->str, "_") != 0 &&
        !hashset_insert(&scope->seen, symbol->name->str)) {
        /* Duplicate pub foreign declarations are valid: multiple modules
           may re-export the same C extern (like a shared header). Skip
           silently instead of raising an error. */
        if (symbol->isPublic && symbol->kind == Z_SYM_FUNC &&
                symbol->node && symbol->node->type == NODE_FOREIGN) {
            return;
        }
        error(ctx->state, symbol->name,
                "'%s' already defined in the same scope",
                symbol->name->str);
    } else {
        vecpush(scope->symbols, symbol);
    }

}

static ZSymbol *makeRawSymbol(
                        ZSymType kind,
                        ZToken *name,
                        ZType *type,
                        ZNode *node,
                        bool isGlobal) {
    ZSymbol *symbol = makesymbol(kind);

    symbol->name        = name;
    symbol->type        = type;
    symbol->node        = node;
    symbol->isPublic    = isGlobal;
    symbol->generics    = NULL;
    symbol->reachable   = strcmp(name->str, "_") == 0;

    return symbol;
}

static void putRawSymbol(ZSemantic *ctx,
                        ZSymType kind,
                        ZToken *name,
                        ZType *type,
                        ZNode *node,
                        bool isGlobal) {
    putSymbol(ctx,
        makeRawSymbol(
            kind,
            name,
            type,
            node,
            isGlobal
        ));
}

static ZFuncTable *makefunctable(ZType *base) {
    ZFuncTable *func        = zalloc(ZFuncTable);
    func->base              = base;
    func->funcDef           = NULL;
    func->seenReceiverFuncs = NULL;
    func->staticFuncDef     = NULL;
    func->seenStaticFuncs   = NULL;
    return func;
}

static ZFuncTable *addfunctable(ZSemantic *ctx, ZType *base) {
    ZFuncTable *table = makefunctable(base);
    vecpush(ctx->table->funcs, table);
    return table;
}

static void addStaticFunc(ZSemantic *ctx, ZNode *func) {
    ZType *base = func->funcDef.base;

    ZFuncTable *cur = NULL;
    for (usize i = 0; i < veclen(ctx->table->funcs); i++) {
        ZFuncTable *table = ctx->table->funcs[i];

        if (typesEqual(table->base, base)) {
            cur = table;
            break;
        }
    }

    if (!cur) {
        cur = addfunctable(ctx, base);
    }
    char *name = func->funcDef.name->str;
    if (!hashset_insert(&cur->seenStaticFuncs, name)) {
        error(ctx->state, func->tok,
                "Duplicate static function '%s'", name);
        return;
    }


    vecpush(cur->staticFuncDef, func);
}

static void putReceiverFunc(ZSemantic *ctx, ZNode *node) {
    if (!node->funcDef.receiver) {
        error(ctx->state, node->tok, "receiver must be setted");
        return;
    } else if (node->funcDef.base) {
        error(ctx->state, node->tok,
                "receiver functions cannot have a base");
        return;
    }

    ZNode *receiver         = node->funcDef.receiver;
    ZFuncTable **funcs      = ctx->table->funcs;
    ZFuncTable *table       = NULL;
    for (usize i = 0; i < veclen(funcs) && !table; i++) {
        if (typesEqual(funcs[i]->base, receiver->field.type)) {
            table = funcs[i];
        }
    }

    if (!table) {
        table = makefunctable(receiver->field.type);
        vecpush(ctx->table->funcs, table);
    }

    vecpush(table->funcDef, node);
}

static void putStaticFunc(ZSemantic *ctx, ZNode *node) {
    ZType *baseType = node->funcDef.base;
    
    if (baseType->kind != Z_TYPE_PRIMITIVE) {
        error(ctx->state, node->tok,
                "Static function must be attached to a primitive type");
        return;
    }
    ZToken *base = baseType->primitive.token;
    if (!base) {
        error(ctx->state,
                node->tok,
                "Invalid 'putStaticFunc' call, base is not setted");
        return;
    } else if (node->funcDef.receiver) {
        error(ctx->state,
                node->tok,
                "Invalid 'putStaticFunc' call, receiver cannot be setted");
    }

    addStaticFunc(ctx, node);
}

static void putFunc(ZSemantic *ctx, ZNode *node) {
    if (node->funcDef.receiver) {
        if (node->funcDef.base) {
            error(ctx->state, node->tok,
                    "A static function cannot accept any receiver");
        }

        putReceiverFunc(ctx, node);
    } else if (node->funcDef.base) {
        putStaticFunc(ctx, node);
    } else {
        ZSymbol *f = makeRawSymbol(
                Z_SYM_FUNC,
                node->funcDef.name,
                node->resolved,
                node,
                node->funcDef.pub);

        if (strcmp(node->funcDef.name->str, "main") == 0) {
            ctx->main = f;
            if (node->funcDef.ret && isVoid(node->funcDef.ret)) {
                error(ctx->state, node->funcDef.name,
                      "'main' must return i32");
            }
        }
        putSymbol(ctx, f);
    }
}

ZNode *getStructField(ZSemantic *ctx, ZType *strct, ZToken *field) {
    if (!strct) return NULL;
    strct = resolveTypeRef(ctx, strct);
    for (usize i = 0; i < veclen(strct->strct.fields); i++) {
        ZNode *structField = strct->strct.fields[i];
        if (structField->type == NODE_EMBED_FIELD) {
            ZNode *res = getStructField(ctx, structField->resolved, field);
            if (res) return res;
        } else if (structField->type == NODE_FIELD) {
            if (tokeneq(structField->field.identifier, field)) {
                return structField;
            }
        }
    }
    return NULL;
}

/**
 * @brief Put all the pattern identifiers in the current scope.
 *
 * Note: It takes the type to validate the pattern.
 * If the pattern doesn't follow the type it emits an error.
 */
static void putVarPattern(
        ZSemantic *ctx, ZNode *node,
        ZType *type, ZVarDestructPattern *pattern, bool condition) {
    if (!type) {
        warning(ctx->state, node->tok, "No type provided for putVarPattern");
        return;
    }
    if (pattern->type == Z_VAR_LIT && condition) {
        ZType *literalType = resolveLiteralType(pattern->ident);
        if (!typesCompatible(ctx->state, literalType, type)) {
            error(ctx->state, pattern->tok,
                "Expected '%s', got '%s'",
                stype(type), stype(literalType)
            );
        }
        return;
    } else if (pattern->type == Z_VAR_IDENT) {
        putRawSymbol(
            ctx,
            Z_SYM_VAR,
            pattern->ident,
            type,
            node,
            false
        );
    } else if (pattern->type == Z_VAR_TUPLE) {
        if (type->kind != Z_TYPE_TUPLE) {
            error(ctx->state, pattern->tok,
                    "'%s' doesn't support destructuring",
                    stype(type));
            return;
        }

        usize expected  = veclen(type->tuple);
        usize got       = veclen(pattern->tuple);
        if (expected != got) {
            error(ctx->state, pattern->tok,
                    "Expected %zu, got %zu elements", expected, got);
            return;
        }

        for (usize i = 0; i < got; i++) {
            putVarPattern(ctx,
                node,
                type->tuple[i],
                pattern->tuple[i],
                condition
            );
        }
    } else if (pattern->type == Z_VAR_STRUCT) {
        if (type->kind != Z_TYPE_STRUCT) {
            error(ctx->state, pattern->tok,
                    "'%s' doesn't support destructuring",
                    stype(type));
            return;
        }

        for (usize i = 0; i < veclen(pattern->fields); i++) {
            ZNode *structField = getStructField(ctx, type, pattern->fields[i]->key);

            if (!structField) {
                error(ctx->state, pattern->fields[i]->key,
                    "Field '%s' not found in %s",
                    pattern->fields[i]->key->str,
                    stype(type)
                );
            } else if (!pattern->fields[i]->value) {
                /* Shorthand {x} - bind to the field name itself. */
                putRawSymbol(ctx, Z_SYM_VAR,
                    pattern->fields[i]->key,
                    structField->resolved,
                    node, false);
            } else {
                putVarPattern(ctx,
                    node,
                    structField->resolved,
                    pattern->fields[i]->value,
                    condition
                );
            }
        }
    } else if (pattern->type == Z_VAR_ENUM) {
        if (type->kind != Z_TYPE_ENUM) {
            error(ctx->state, pattern->tok,
                "'%s' doesn't support destructuring", stype(type));
            return;
        } else if (!tokeneq(type->enm.name, pattern->base)) {
            error(ctx->state, pattern->tok,
                    "Expected '%s', got '%s'",
                    type->enm.name->str,
                    pattern->base->str);
        }
        ZType *variant = NULL;
        for (usize i = 0; i < veclen(type->enm.fields) && !variant; i++) {
            ZType *field = type->enm.fields[i];
            if (tokeneq(field->strct.name, pattern->prop))
                variant = type->enm.fields[i];
        }

        if (!variant) {
            error(ctx->state, pattern->tok,
                "enum variant '%s' not found for '%s'",
                pattern->prop->str, stype(type));
            return;
        }

        usize expected  = veclen(variant->strct.fields) - 1;
        usize got       = veclen(pattern->args);

        if (expected != got) {
            error(ctx->state, pattern->tok,
                "Expected %zu args, got %zu", expected, got);
            return;
        }

        for (usize i = 0; i < expected; i++) {
            putVarPattern(ctx, node,
                variant->strct.fields[i + 1]->field.type,
                pattern->args[i], condition
            );
        }
        
    } else {
        error(ctx->state, pattern->tok, "Unhandled destructure pattern");
    }
}

/* Store a node of type NODE_VAR_DECL in the ctx table. */
static void putVar(ZSemantic *ctx, ZNode *node, bool isGlobal) {
    (void)isGlobal;
    if (!node->resolved) {
        error(ctx->state,
                node->tok,
                "Cannot register var, got null type");
    }

    putVarPattern(ctx,
        node,
        node->resolved,
        node->varDecl.pattern,
        false
    );
}

static void putGeneric(ZSemantic *ctx, ZType *type) {
    putRawSymbol(
        ctx,
        Z_SYM_GENERIC,
        type->generic.name,
        type,
        NULL,
        false
    );
}

static void putStruct(ZSemantic *ctx, ZNode *node) {
    ZType *type             = maketype(Z_TYPE_STRUCT);
    type->strct.name        = node->structDef.ident;
    type->strct.fields      = node->structDef.fields;
    type->strct.generics    = NULL;
    type->tok               = node->tok;
    node->resolved          = type;

    putRawSymbol(ctx,
            Z_SYM_STRUCT,
            node->structDef.ident,
            type,
            node,
            node->structDef.pub);
}

static void putEnum(ZSemantic *ctx, ZNode *node) {
    putRawSymbol(
        ctx,
        Z_SYM_ENUM,
        node->enumDef.name,
        node->resolved,
        node,
        node->enumDef.pub
    );
}

static void putTypedef(ZSemantic *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_TYPEDEF,
        node->typeDef.alias,
        node->typeDef.type,
        node,
        node->typeDef.pub
    );
}

static void putFacet(ZSemantic *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_FACET,
        node->facet.name,
        node->resolved,
        node,
        node->facet.pub
    );
}

static void putNamespace(ZSemantic *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_NAMESPACE,
        node->tok,
        node->resolved,
        node,
        false
    );
}

static i32 lookupCapabilityByType(ZScope *scope, ZType *capability) {
    for (usize i = 0; i < veclen(scope->capabilities); i++) {
        if (typesEqual(scope->capabilities[i]->type, capability)) {
            return i;
        }
    }
    return -1;
}

static ZCapability *putCapability(ZSemantic *ctx, ZNode *var) {
    if (!var || !var->resolved) {
        error(ctx->state, var ? var->tok : NULL,
            "Invalid 'putCapability' call");
        return NULL;
    }

    ZScope *cur = ctx->table->current;

    i32 i = lookupCapabilityByType(cur, var->resolved);
    ZCapability *capability = NULL;
    if (i == -1) {
        capability          = zalloc(ZCapability);
        capability->nodes   = NULL;
        capability->type    = var->resolved;
        vecpush(cur->capabilities, capability);
    } else {
        capability = cur->capabilities[i];
    }
    vecpush(capability->nodes, var);
    return capability;
}

static void registerModule(ZSemantic *ctx, ZNode *module) {
    ZScope *scope = NULL;
    for (usize i = 0; i < veclen(ctx->scopes); i++) {
        if (ctx->scopes[i]->module == module) {
            scope = ctx->scopes[i]->scope;
            goto setScope;
        }
    }

    ZScopeTable *table = zalloc(ZScopeTable);
    table->module = module;
    table->scope = makescope(ctx->table->global, module);

    vecpush(ctx->scopes, table);
    scope = table->scope;

setScope:
    ctx->table->module = ctx->table->current;
    ctx->table->current = scope;
}

static void warnUnused(ZSemantic *ctx, ZSymbol *symbol) {
    switch (symbol->kind) {
    case Z_SYM_FUNC:
        if (ctx->state->unusedFunc) break;
        warning(ctx->state,
                symbol->name,
                "Unused function '%s'",
                symbol->name->str);
        break;
    case Z_SYM_STRUCT:
        if (ctx->state->unusedStruct) break;
        warning(ctx->state,
                symbol->name,
                "Unused struct '%s'",
                symbol->name->str);
        break;
    case Z_SYM_VAR:
        if (ctx->state->unusedVar) break;
        warning(ctx->state,
                symbol->name,
                "Unused variable '%s'",
                symbol->name->str);
        break;
    default:
        warning(ctx->state, symbol->name, "Unused a generic symbol");
        break;
    }
}

static void checkUnusedSymbols(ZSemantic *ctx) {
    ZScope *scope = ctx->table->current;
    for (usize i = 0; i < veclen(scope->symbols); i++) {
        let symbol = scope->symbols[i];
        if (!symbol->reachable && symbol->useCount == 0) {
            warnUnused(ctx, symbol);
        }
    }
}

static void endModule(ZSemantic *ctx, bool checkSymbols) {
    if (!ctx || !ctx->table || !ctx->table->module) return;
    if (checkSymbols) checkUnusedSymbols(ctx);
    ctx->table->current = ctx->table->module;
}

static void beginScope(ZSemantic *ctx, ZNode *curr) {
    ZScope *scope       = makescope(ctx->table->current, curr);
    ctx->table->current = scope;
}

static void endScope(ZSemantic *ctx) {
    if (!ctx->table->current || !ctx->table->current->parent) {
        error(ctx->state, NULL, "Called endScope at the highest level");
        return;
    }
    checkUnusedSymbols(ctx);
    ctx->table->current = ctx->table->current->parent;
}

/* ================== Type arithmetic ================== */

static u8 typeRank(ZTokenType t) {
    switch (t) {
    case TOK_CHAR:              return 0;
    case TOK_I8:  case TOK_U8:  return 1;
    case TOK_I16: case TOK_U16: return 2;
    case TOK_I32: case TOK_U32: return 3;
    case TOK_I64: case TOK_U64: return 4;
    case TOK_F32:               return 5;
    case TOK_F64:               return 6;
    default:                    return 0;
    }
}

static inline bool isUnsigned(ZTokenType t) { return (bool)(t & TOK_UNSIGNED);  }
static inline bool isSigned  (ZTokenType t) { return (bool)(t & TOK_SIGNED);    }
static inline bool isFloat   (ZTokenType t) { return (bool)(t & TOK_FLOAT);     }
static inline bool isInteger (ZTokenType t) { return isSigned(t) || isUnsigned(t);  }
static inline bool isPrimitive(ZType *t)    { return t->kind == Z_TYPE_PRIMITIVE;   }

static ZTokenType toSigned(u8 rank) {
    switch (rank) {
    case 1: return TOK_I8;
    case 2: return TOK_I16;
    case 3: return TOK_I32;
    case 4: return TOK_I64;
    default: return TOK_I32;
    }
}

static bool isComparable(ZSemantic *ctx, ZType *type) {
    type = resolveTypeRef(ctx, type);
    if (!type) return false;

    if (type->kind == Z_TYPE_FUNCTION       ||
            type->kind == Z_TYPE_ARRAY      ||
            type->kind == Z_TYPE_GENERIC    ||
            type->kind == Z_TYPE_STRUCT     ||
            type->kind == Z_TYPE_TUPLE) {
        return false;
    }

    return true;
}

/* An implementation note:
 * if a and b are pointers the returned type is a (used for implicit casting).
 * if a or b is a float the return type is always a float.
 * if a and b are both signed or unsigned return the type with the highest rank.
 * if they are unsigned vs signed and the unsigned is u64 can't promote to i128
 * so in this case the compiler shows a warning (explicit casting requested).
 *
 * Note: this function does not work if a primitive type is aliased.
 * */
ZType *typesCompatible(ZState *state, ZType *a, ZType *b) {
    if (!a || !b) return NULL;
    
    if (a->kind == Z_TYPE_POINTER && b->kind == Z_TYPE_NONE) {
        return a;
    } else if (b->kind == Z_TYPE_POINTER && a->kind == Z_TYPE_NONE) {
        return b;
    } else if (a->kind == Z_TYPE_POINTER && b->kind == Z_TYPE_POINTER) {
        return a;
    }

    if (typesEqual(a, b)) return b;

    if (a->kind != Z_TYPE_PRIMITIVE || b->kind != Z_TYPE_PRIMITIVE)
        return NULL;

    ZTokenType ta = a->primitive.token->type;
    ZTokenType tb = b->primitive.token->type;

    if (ta == TOK_VOID || tb == TOK_VOID) return NULL;

    u8 ra = typeRank(ta);
    u8 rb = typeRank(tb);

    if (isFloat(ta) || isFloat(tb)) {
        return ra > rb ? a : b;
    }

    if ((isSigned(ta) && isSigned(tb)) ||
        (isUnsigned(ta) && isUnsigned(tb)))
        return ra > rb ? a : b;

    /* signed vs unsigned */
    u8    signedRank   = isSigned(ta) ? ra : rb;
    u8    unsignedRank = isSigned(ta) ? rb : ra;
    ZType *signedType  = isSigned(ta) ? a  : b;

    if (signedRank > unsignedRank) return signedType;

    if (signedRank == 4) {
        warning(state, signedType->tok,
                "Cannot promote a 64-bits integer, try with an explicit cast");
    }

    ZType *promoted             = maketype(Z_TYPE_PRIMITIVE);
    promoted->primitive.token   = maketoken(toSigned(signedRank + 1), NULL);
    promoted->tok               = a->tok;
    return promoted;
}

bool isVoid(ZType *t) {
    if (!t) return true;
    if (t->kind != Z_TYPE_PRIMITIVE) return false;
    return t->primitive.token->type == TOK_VOID;
}

bool typesPrimitive(ZType *t) {
    if (!t) return true;

    if (t->kind == Z_TYPE_POINTER) return true;
    if (t->kind != Z_TYPE_PRIMITIVE) return false;

    switch (t->primitive.token->type) {
    case TOK_VOID:
    case TOK_CHAR:
    case TOK_I8:
    case TOK_U8:
    case TOK_I16:
    case TOK_U16:
    case TOK_I32:
    case TOK_U32:
    case TOK_I64:
    case TOK_U64:
    case TOK_F32:
    case TOK_F64: return true;
    default: return false;
    }

    return false;
}

int compareTypes(const void *a, const void *b) {
    ZType *typeA = (ZType *)a;
    ZType *typeB = (ZType *)b;

    int sizeA = (int)typeSize(typeA) << 2;
    int sizeB = (int)typeSize(typeB) << 2;

    // return sizeB - sizeA;
    return sizeA - sizeB;
}

inline void typesSort(ZType **types) {
    qsort(types, veclen(types), sizeof(ZType *), compareTypes);
}

/* Note: this function works only for non-aliased types.
 * Aliases are resolved through the ctx table.
 *
 * */
bool typesEqual(ZType *a, ZType *b) {
    if (!a || !b) return false;

    if (a->kind != b->kind) return false;

    switch (a->kind) {
    case Z_TYPE_PRIMITIVE:
        return tokeneq(a->primitive.token, b->primitive.token);
    case Z_TYPE_POINTER:
        return typesEqual(a->base, b->base);
    case Z_TYPE_ARRAY:
        // if (a->array.size == 0 && b->array.size > 0) {
        //     a->array.size = b->array.size;
        // } else if (b->array.size == 0 && a->array.size > 0) {
        //     b->array.size = a->array.size;
        // } else if (a->array.size != b->array.size) {
        //     return false;
        // }
        return typesEqual(a->array.base, b->array.base);
    case Z_TYPE_STRUCT:
        return a == b;
    case Z_TYPE_FUNCTION:
        if (!typesEqual(a->func.ret, b->func.ret)) return false;
        if (veclen(a->func.args) != veclen(b->func.args)) return false;
        if (veclen(a->func.generics) != veclen(b->func.generics)) return false;
        if (veclen(a->func.capabilities) != veclen(b->func.capabilities)) return false;

        for (usize i = 0; i < veclen(a->func.args); i++)
            if (!typesEqual(a->func.args[i], b->func.args[i]))
                return false;

        for (usize i = 0; i < veclen(a->func.capabilities); i++)
            if (!typesEqual(a->func.capabilities[i], b->func.capabilities[i]))
                return false;

        return true;
    case Z_TYPE_ENUM:
        return a == b;
    case Z_TYPE_TUPLE: {
        if (veclen(a->tuple) != veclen(b->tuple)) return false;
        for (usize i = 0; i < veclen(a->tuple); i++)
            if (!typesEqual(a->tuple[i], b->tuple[i])) return false;
        return true;
    }
    case Z_TYPE_GENERIC:
        if (!tokeneq(a->generic.name, b->generic.name)) return false;
        return true;
    case Z_TYPE_SUM: {
        usize aLen = veclen(a->sumType);
        usize bLen = veclen(b->sumType);

        if (aLen != bLen) return false;

        bool seen[bLen];
        memset(seen, 0, sizeof(bool) * aLen);

        for (usize i = 0; i < aLen; i++) {
            bool res = false;
            for (usize j = 0; j < bLen; j++) {
                if (seen[j]) continue;

                if (typesEqual(a->sumType[i], b->sumType[j])) {
                    res = true;
                    seen[j] = true;
                }
            }
            if (!res) return false;
        }
        return true;
    }
    default:
        return false;
    }
}

ZNode *implicitCast(ZSemantic *ctx, ZNode *node, ZType *type) {
    if (!node) return node;
    if (node->resolved && typesEqual(node->resolved, type)) {
        return node;
    }

    ZNode *cast             = makenode(NODE_CAST);
    cast->castExpr.expr     = node;
    cast->castExpr.toType   = type;
    cast->resolved          = type;
    return cast;
}

/* ================== Symbol lookup ================== */
ZSymbol *resolve(ZSemantic *ctx, ZToken *ident) {
    if (ident && ident->type == TOK_IDENT && strcmp(ident->str, "_") == 0) {
        error(ctx->state, ident, "Use '_' only for unused variables");
    }
    ZScope *curr = ctx->table->current;
    while (curr) {
        for (usize i = 0; i < veclen(curr->symbols); i++) {
            if (tokeneq(curr->symbols[i]->name, ident)) {
                curr->symbols[i]->useCount++;
                return curr->symbols[i];
            }
        }
        curr = curr->parent;
    }
    return NULL;
}

/* ================== Type resolution ================== */

static ZType *derefType(ZType *t) {
    while (t && t->kind == Z_TYPE_POINTER) t = t->base;
    return t;
}

ZType *resolveLiteralType(ZToken *curr) {
    // None guard
    if (curr->type == TOK_NONE) {
        return none;
    }

    ZType *t = maketype(Z_TYPE_PRIMITIVE);
    switch (curr->type) {
    case TOK_RUNE_LIT:
    case TOK_INT_LIT: {
        t->primitive.token = maketoken(TOK_I32, NULL);
        break;
    }
    case TOK_FLOAT_LIT: {
        t->primitive.token = maketoken(TOK_F64, NULL);
        break;
    }
    case TOK_FALSE:
    case TOK_TRUE: {
        t->primitive.token = maketoken(TOK_BOOL, NULL);
        break;
    }
    case TOK_STR_LIT: {
        /* String literals are *char */
        ZType *base = maketype(Z_TYPE_PRIMITIVE);
        base->primitive.token = maketoken(TOK_CHAR, NULL);
        t->kind = Z_TYPE_POINTER;
        t->base   = base;
        break;
    }
    default: {
        t->primitive.token = maketoken(TOK_VOID, NULL);
        break;
    }
    }
    t->tok  = curr;
    return t;
}

/*
 * Returns true if `type` embeds `root` by value (not through a pointer),
 * which would make the struct infinitely large. `seen` is a visited-struct
 * set to avoid re-entering mutual-recursion loops.
 */
static bool isInfiniteSize(ZType *type, ZType *root, ZType **seen) {
    if (!type) return false;
    switch (type->kind) {
    case Z_TYPE_STRUCT:
        if (type == root) return true;

        for (usize i = 0; i < veclen(seen); i++)
            if (typesEqual(seen[i], type)) return false;

        vecpush(seen, type);
        for (usize i = 0; i < veclen(type->strct.fields); i++)
            if (isInfiniteSize(type->strct.fields[i]->resolved, root, seen))
                return true;
        return false;

    case Z_TYPE_ENUM:
        if (type == root) return true;

        for (usize i = 0; i < veclen(seen); i++)
            if (typesEqual(seen[i], type)) return false;

        vecpush(seen, type);

        for (usize i = 0; i < veclen(type->enm.fields); i++) {
            if (isInfiniteSize(type->enm.fields[i], root, seen)) return true;
        }
        return false;

    case Z_TYPE_POINTER:
        return false;
    case Z_TYPE_ARRAY:
        if (type->array.size == 0) return false;
        return isInfiniteSize(type->array.base, root, seen);
    case Z_TYPE_TUPLE:
        for (usize i = 0; i < veclen(type->tuple); i++)
            if (isInfiniteSize(type->tuple[i], root, seen)) return true;
        return false;
    default:
        return false;
    }
}

static ZType *_resolveTypeRef(ZSemantic *ctx, ZType *type, ZType ***seen) {
    if (!type) return NULL;

    for (usize i = 0; i < veclen(*seen); i++) {
        if (typesEqual((*seen)[i], type)) {
            error(ctx->state, type->tok, "Circular types");
            return (*seen)[i];
        }
    }
    switch (type->kind) {
    case Z_TYPE_PRIMITIVE: {
        if (type->primitive.token->type != TOK_IDENT) return type;
        ZSymbol *sym = resolve(ctx, type->primitive.token);
        if (!sym) {
            error(ctx->state, type->primitive.token,
                  "Unknown type '%s'", type->primitive.token->str);
            return NULL;
        }
        if (sym->kind == Z_SYM_STRUCT) {
            // for (usize i = 0; i < veclen(sym->type->strct.fields); i++) {
            //     ZNode *field = sym->type->strct.fields[i];
            //     field->resolved = _resolveTypeRef(
            //         ctx, field->resolved, NULL
            //     );
            //     sym->node->structDef.fields[i]->resolved = field->resolved;
            // }
        } else if (sym->kind == Z_SYM_TYPEDEF) {
            if (sym->node->resolved) return sym->node->resolved;
            vecpush(*seen, type);
            return _resolveTypeRef(ctx, sym->node->typeDef.type, seen);
        }
        return sym->type;
    }
    case Z_TYPE_POINTER:
        type->base = _resolveTypeRef(ctx, type->base, seen);
        return type;
    case Z_TYPE_ARRAY:
        type->array.base = _resolveTypeRef(ctx, type->array.base, seen);
        return type;
    case Z_TYPE_FUNCTION:
        type->func.ret = _resolveTypeRef(ctx, type->func.ret, seen);
        for (usize i = 0; i < veclen(type->func.args); i++) {
            type->func.args[i] = _resolveTypeRef(ctx,
                type->func.args[i],
                seen);
        }
        return type;
    case Z_TYPE_TUPLE:
        for (usize i = 0; i < veclen(type->tuple); i++)
            type->tuple[i] = _resolveTypeRef(ctx, type->tuple[i], seen);
        return type;
    default: return type;
    }
}

/*
 * Resolve a ZType that may contain named references (user-defined types).
 *
 * When the parser sees `MyStruct x`, the type is stored as a Z_TYPE_PRIMITIVE
 * with primitive.token->type == TOK_IDENT and primitive.token->str == "MyStruct".
 * This function looks that name up in the symbol table and returns the actual
 * ZType registered by putStruct() / a typedef entry.
 *
 * For compound types (pointer, array, function, tuple) it recurses into their
 * sub-types so that e.g. `*MyStruct` gets fully resolved.
 *
 * NOTE: this function doesn't handle cycles.
 * So a struct that has a pointer to itself
 * (like linked-lists, trees, graphs ...) cause an in infinite loop.
 * To handle it properly the function have to early return
 * if the current type is already visited.
 */
static ZType *resolveTypeRef(ZSemantic *ctx, ZType *type) {
    if (!type) return NULL;
    ZType **seen = NULL;
    return _resolveTypeRef(ctx, type, &seen);
}

static ZType *resolveMemberAccess(ZSemantic *, ZNode *);
static ZType *resolveArrSubscript(ZSemantic *, ZNode *);

static ZNode *resolveStaticFuncTable(ZSemantic *ctx,
    ZToken *base, ZToken *prop) {
    ZNode **staticFuncs = NULL;
    for (usize i = 0; i < veclen(ctx->table->funcs); i++) {
        ZFuncTable *table = ctx->table->funcs[i];
        if (table->base->kind != Z_TYPE_PRIMITIVE       ||
            !tokeneq(table->base->primitive.token, base)) continue;

        if (!hashset_has(table->seenStaticFuncs, prop->str)) return NULL;
        staticFuncs = table->staticFuncDef;
    }
    if (!staticFuncs) return NULL;

    ZNode *node     = NULL;
    for (usize i = 0; i < veclen(staticFuncs) && !node; i++) {
        ZNode *func = staticFuncs[i];
        if (tokeneq(func->funcDef.base->primitive.token, base) &&
            tokeneq(func->funcDef.name, prop)) {
            node    = func;
        }
    }

    if (!node) return NULL;

    for (usize i = 0; i < veclen(node->resolved->func.args); i++) {
        ZType *resolved = resolveTypeRef(ctx, node->resolved->func.args[i]);
        if (!resolved) {
            error(ctx->state, node->resolved->func.args[i]->tok,
                "Unresolved type");
        } else {
            node->resolved->func.args[i] = resolved;
        }
    }
    return node;
}

static bool funcTableBaseMatches(ZSemantic *ctx, ZType *tableBase, ZType *obj) {
    if (!tableBase || !obj) return false;
    if (typesEqual(tableBase, obj)) return true;

    if (tableBase->kind == Z_TYPE_PRIMITIVE &&
        tableBase->primitive.token->type == TOK_IDENT) {
        if (obj->kind == Z_TYPE_STRUCT &&
            strcmp(tableBase->primitive.token->str, obj->strct.name->str) == 0) {
            return true;
        }
        ZSymbol *sym = resolve(ctx, tableBase->primitive.token);
        if (sym) {
            ZType *resolved = sym->type;
            if (sym->kind == Z_SYM_TYPEDEF && sym->node) {
                resolved = sym->node->resolved
                    ? sym->node->resolved
                    : sym->node->typeDef.type;
            }
            if (resolved && typesEqual(resolved, obj)) return true;
        }
    }

    if (tableBase->kind == Z_TYPE_POINTER && obj->kind == Z_TYPE_POINTER) {
        return funcTableBaseMatches(ctx, tableBase->base, obj->base);
    }
    return false;
}

static ZFuncTable *resolveFuncTable(ZSemantic *ctx, ZType *obj) {
    ZFuncTable **table = ctx->table->funcs;
    for (usize i = 0; i < veclen(table); i++) {
        if (funcTableBaseMatches(ctx, table[i]->base, obj)) return table[i];
    }
    return NULL;
}


static ZNode *resolveFuncCallEmbedded(ZSemantic *ctx,
    ZNode *curr, ZType *obj, ZToken *prop) {
    ZNode *ptr = NULL;
    if (obj && obj->kind == Z_TYPE_STRUCT) {
        for (usize i = 0; i < veclen(obj->strct.fields); i++) {
            ZNode *field = obj->strct.fields[i];
            if (field->type != NODE_EMBED_FIELD) continue;
            if (ptr) {
                error(ctx->state, prop,
                    "Function conflict with type '%s'",
                    stype(ptr->resolved)
                );
            } else {
                ptr = resolveFuncCallEmbedded(
                    ctx, curr, field->resolved, prop);
            }
        }
    }

    ZFuncTable *funcs = resolveFuncTable(ctx, obj);

    if (funcs) {
        for (usize i = 0; i < veclen(funcs->funcDef); i++) {
            ZNode *f = funcs->funcDef[i];
            if (tokeneq(f->funcDef.name, prop)) {
                if (ptr) {
                    error(ctx->state, prop,
                        "Function conflict with type '%s'",
                        stype(ptr->resolved)
                    );
                }
                ptr = f;
            }
        }
    }

    if (ptr) {
        curr->memberAccess.mangled = ptr->funcDef.mangled;
    }
    return ptr;
}

static ZNode *lookupScopedCapability(ZSemantic *ctx, ZType *required) {
    ZScope *curr = ctx->table->current;
    i32 i = -1;
    while (curr) {
        i = lookupCapabilityByType(curr, required);
        if (i != -1) break;
        curr = curr->parent;
    }
    if (i == -1 || veclen(curr->capabilities[i]->nodes) == 0) return NULL;
    return veclast(curr->capabilities[i]->nodes);
}

static ZType *resolveFuncCall(ZSemantic *ctx, ZNode *curr) {
    ZNode *callee = curr->call.callee;
    ZNode **args = curr->call.args;

    for (usize i = 0; i < veclen(args); i++) {
        args[i]->resolved = resolveType(ctx, args[i]);
        checkFunctionUsedAsValue(ctx, args[i]);
    }

    ZType *expectedFunc = NULL;
    if (callee->type == NODE_IDENTIFIER) {
        ZSymbol *sym = resolve(ctx, callee->identNode.tok);
        if (!sym || !sym->type) {
            error(ctx->state, callee->identNode.tok,
                  "Undefined function '%s'", callee->identNode.tok->str);
            return NULL;
        }
        if (sym->kind != Z_SYM_FUNC) {
            if (sym->type && sym->type->kind == Z_TYPE_FUNCTION) {
                expectedFunc = sym->type;
                expectedFunc->func.ret = resolveTypeRef(ctx, expectedFunc->func.ret);
                for (usize i = 0; i < veclen(expectedFunc->func.args); i++) {
                    expectedFunc->func.args[i] = resolveTypeRef(
                        ctx,
                        expectedFunc->func.args[i]
                    );
                }
                curr->call.func = sym->node;
                callee->resolved = expectedFunc;
            } else {
                error(ctx->state, callee->identNode.tok,
                      "'%s' is not callable", callee->identNode.tok->str);
                return NULL;
            }
        } else {
            if (sym->node->type == NODE_FUNC) {
                callee->identNode.mangled = sym->node->funcDef.mangled;
            }

            /* sym->type is the raw parsed return type - resolve it so that named
             * types (e.g. "Vec2" -> Z_TYPE_STRUCT) are expanded before the
             * result is used downstream (e.g. for member access on return value).
             * */
            for (usize i = 0; i < veclen(sym->type->func.args); i++) {
                sym->type->func.args[i] = resolveTypeRef(
                    ctx, sym->type->func.args[i]
                );
            }
            sym->type->func.ret     = resolveTypeRef(ctx, sym->node->funcDef.ret);
            sym->node->funcDef.ret  = sym->type->func.ret;
            expectedFunc            = sym->type;
            callee->resolved        = expectedFunc->func.ret;
        }
    } else if (callee->type == NODE_STATIC_ACCESS) {
        ZType *resolved = resolveType(ctx, callee);
        if (callee->type == NODE_ENUM_LIT) {
            curr->type = NODE_ENUM_LIT;
            callee->type = NODE_STATIC_ACCESS;
        }
        if (!resolved) {
            error(ctx->state, callee->tok, "Unresolved type");
            return NULL;
        } else if (resolved->kind == Z_TYPE_FUNCTION) {
            for (usize i = 0; i < veclen(resolved->func.args); i++) {
                resolved->func.args[i] = resolveTypeRef(
                    ctx, resolved->func.args[i]
                );
            }
            resolved->func.ret  = resolveTypeRef(ctx, resolved->func.ret);
            expectedFunc        = resolved;
            callee->resolved    = resolved;
            curr->resolved      = resolved->func.ret;
        } else if (resolved->kind == Z_TYPE_ENUM) {
            ZType **variants      = resolved->enm.fields;
            ZNode **fields        = NULL;
            for (usize i = 0; i < veclen(resolved->enm.fields) && !fields; i++) {
                if (tokeneq(variants[i]->strct.name, callee->staticAccess.prop)) {
                    fields = variants[i]->strct.fields;
                }
            }
            if (!fields) {
                error(ctx->state, callee->tok,
                    "Invalid enum variant '%s'",
                    stoken(callee->staticAccess.prop)
                );
                return NULL;
            }

            curr->resolved = resolved;
            expectedFunc = maketype(Z_TYPE_FUNCTION);
            expectedFunc->func.ret = resolved;
            for (usize i = 1; i < veclen(fields); i++) {
                vecpush(expectedFunc->func.args, fields[i]->resolved);
            }
        } else {
            error(ctx->state, callee->tok,
                "Expected function type, got %s",
                stype(resolved)
            );
            return NULL;
        }
    } else {
        /* Expression call (includes NODE_MEMBER, subscripts, etc.):
         * resolveType handles all callee forms uniformly. For NODE_MEMBER,
         * resolveMemberAccess now covers both struct fields and receiver
         * methods, setting memberAccess.mangled as a side-effect so that
         * codegen can inject self for method calls. */
        ZType *calleeType = resolveType(ctx, callee);
        if (!calleeType || calleeType->kind != Z_TYPE_FUNCTION) {
            error(ctx->state, callee->tok,
                "type '%s' is not callable",
                stype(calleeType));
            return NULL;
        }
        for (usize i = 0; i < veclen(calleeType->func.args); i++) {
            calleeType->func.args[i] = resolveTypeRef(
                ctx, calleeType->func.args[i]
            );
        }
        calleeType->func.ret    = resolveTypeRef(ctx, calleeType->func.ret);
        expectedFunc            = calleeType;
        /* callee->resolved is already set to calleeType (the function type)
         * by resolveType above - leave it as the function type so that genCall
         * can derive the LLVM funcType for indirect/function-pointer calls. */
    }

    if (!expectedFunc) return NULL;

    usize expectedArgsLen = veclen(expectedFunc->func.args);
    if (!expectedFunc->func.variadic && expectedArgsLen != veclen(args)) {
        if (!curr->tok) {
            warning(ctx->state, NULL,
                "Node %d does not have tok\n", curr->type);
        }
        error(ctx->state, curr->tok,
                "Expected %zu argument(s), got %zu",
                expectedArgsLen, veclen(args));
        return NULL;
    }

    for (usize i = 0; i < expectedArgsLen; i++) {
        ZType *expected = expectedFunc->func.args[i];
        /* If the argument is a generic skip the validation*/
        if (expected && expected->kind == Z_TYPE_GENERIC) {
            continue;
        }

        /* if they are equal they don't need implicit casting. */
        if (typesEqual(args[i]->resolved, expected)) continue;

        ZType *promoted = typesCompatible(
            ctx->state, args[i]->resolved, expected);

        if (!promoted) {
            error(ctx->state, args[i]->tok,
                "Expected %s, got %s",
                stype(args[i]->resolved),
                stype(expected)
            );
        }
        curr->call.args[i] = implicitCast(ctx, args[i], expected);
    }

    for (usize i = 0; i < veclen(expectedFunc->func.capabilities); i++) {
        expectedFunc->func.capabilities[i] = resolveTypeRef(ctx,
                expectedFunc->func.capabilities[i]
        );
        ZNode *reference = lookupScopedCapability(
            ctx, expectedFunc->func.capabilities[i]
        );
        if (!reference) {
            error(ctx->state, curr->tok,
                "Capability '%s' not found in the current scope",
                stype(expectedFunc->func.capabilities[i]));
        }
        vecpush(curr->call.capabilities, reference);
    }
    return expectedFunc->func.ret;
}

static bool isLvalue(ZNode *node) {
    if (!node) return false;

    switch (node->type) {
    case NODE_IDENTIFIER:   return true;
    case NODE_SUBSCRIPT:    return true;
    case NODE_MEMBER:       return true;
    case NODE_UNARY:        return node->unary.operat->type == TOK_STAR;
    default: return false;
    }
}

static ZType *resolveStructLit(ZSemantic *ctx, ZNode *curr) {
    ZSymbol *structSym = resolve(ctx, curr->structlit.ident);

    if (!structSym) {
        error(ctx->state, curr->tok,
                "struct '%s' not found", curr->tok->str);
        return NULL;
    }
    ZType *symType = resolveType(ctx, structSym->node);
    if (!symType || symType->kind != Z_TYPE_STRUCT) {
        error(ctx->state, curr->structlit.ident,
                    "'%s' is not a struct", stoken(curr->structlit.ident));
        return NULL;
    }

    if (veclen(curr->structlit.fields) == 0) {
        warning(ctx->state, curr->tok, "Some fields not initialized");
    }

    for (usize i = 0; i < veclen(curr->structlit.fields); i++) {
        ZNode *field = curr->structlit.fields[i];
        ZType *type = resolveType(ctx, field->varDecl.rvalue);
        ZType *expectedType;
        ZType *promoted;

        if (!type) {
            error(ctx->state, field->varDecl.rvalue->tok,
                "Unresolved type");
            continue;
        }

        field->resolved = type;
        ZNode *structField = getStructField(ctx, structSym->type, field->tok);

        if (!structField) {
            error(ctx->state, field->tok,
                "Field '%s' not found for struct '%s'",
                field->tok->str, stype(structSym->type)
            );
            return NULL;
        }
        expectedType = resolveTypeRef(ctx, structField->field.type);
        structField->field.type = expectedType;
        promoted = typesCompatible(ctx->state, expectedType, type);
        if (!promoted) {
            error(ctx->state,
                field->tok,
                "Expected %s, got %s",
                stype(expectedType),
                stype(type)
            );
        }
    }
    return symType;
}

static void checkFunctionUsedAsValue(ZSemantic *ctx, ZNode *node) {
    if (!node || node->type != NODE_MEMBER) return;
    if (node->memberAccess.mangled) {
        error(ctx->state, node->memberAccess.field,
            "cannot use receiver method '%s' as a value",
            node->memberAccess.field->str);
    }
}

static ZType* resolveIdentifier(ZSemantic *ctx, ZNode *node) {
    ZToken *tok = node->identNode.tok;
    ZSymbol *sym = resolve(ctx, tok);
    if (!sym) {
        error(ctx->state, tok,
              "Undefined identifier '%s'", tok->str);
        return NULL;
    }
    if (sym->node->type == NODE_FUNC) {
        node->identNode.mangled = sym->node->funcDef.mangled;
        ZNode *fn = sym->node;
        if (fn->funcDef.receiver) {
            error(ctx->state, tok,
                "cannot use receiver method '%s' as a value",
                tok->str);
        } else if (veclen(fn->funcDef.capabilities) > 0) {
            error(ctx->state, tok,
                "cannot use capability-requiring function '%s' as a value",
                tok->str);
        }
    }
    return sym->type;
}

static bool hasOverloadAnnotation(
    ZSemantic *ctx, ZAnnotation **annotations, ZTokenType op) {
    for (usize i = 0; i < veclen(annotations); i++) {
        ZAnnotation *annotation = annotations[i];
        if (strcmp(annotation->name->str, "overload") != 0) continue;
        if (veclen(annotation->args) != 1) {
            error(ctx->state, annotation->name,
                "Annotation 'overload' must contain 1 argument"
            );
        } else if (!(annotation->args[0]->name->type & TOK_OVERLOADABLE)) {
            error(ctx->state, annotation->name,
                "'%s' is not an overridable operator",
                stoken(annotation->args[0]->name)
            );
        } else if (annotation->args[0]->name->type == op) {
            return true;
        }
    }
    return false;
}

static ZNode *resolveOverloadOperator(ZSemantic *ctx, ZType *type, ZTokenType op) {
    if (!(op & TOK_OVERLOADABLE)) return NULL;
    ZFuncTable *table = resolveFuncTable(ctx, type);

    if (!table) return NULL;

    for (usize i = 0; i < veclen(table->funcDef); i++) {
        ZNode *func = table->funcDef[i];
        if (hasOverloadAnnotation(ctx, func->funcDef.annotations, op)) {
            return func;
        }
    }

    return NULL;
}

static ZType *resolveBinary(ZSemantic *ctx, ZNode *curr) {
    ZTokenType op       = curr->binary.op->type;
    ZType     *left     = resolveType(ctx, curr->binary.left);
    ZType     *right    = resolveType(ctx, curr->binary.right);

    if (op & TOK_BITOPERATOR_MASK) {
        if (isPrimitive(left)   &&  !isInteger(left->primitive.token->type) &&
            isPrimitive(right)  &&  !isInteger(right->primitive.token->type)) {
            error(ctx->state, curr->binary.op,
                "Bit operators can be used only with integers");
            return NULL;
        }
    }

    if (op & TOK_OVERLOADABLE) {
        ZNode *overload = resolveOverloadOperator(ctx, left, op);
        curr->binary.overload = overload;
        if (overload) {
            return overload->resolved->func.ret;
        }
    }

    /* Auto promotion rules should be handled by typesCompatible. */
    ZType *promoted     = typesCompatible(ctx->state, left, right);

    if (!promoted) {
        error(ctx->state,
            curr->binary.op,
            "Incompatible type '%s' with '%s'",
            stype(left),
            stype(right)
        );
    }

    if (op == TOK_EQ) {
        /* Assignment yields the type of the left-hand side. */
        if (!isLvalue(curr->binary.left)) {
            error(ctx->state, curr->binary.left->tok,
                    "is not a valid lvalue");
        }
        curr->binary.right = implicitCast(ctx, curr->binary.right, left);
        return left;
    }

    curr->binary.left = implicitCast(ctx, curr->binary.left, promoted);
    curr->binary.right = implicitCast(ctx, curr->binary.right, promoted);

    /* Comparison / logical operators always produce a bool. */
    if (op == TOK_EQEQ  || op == TOK_NOTEQ  ||
        op == TOK_LT    || op == TOK_GT     ||
        op == TOK_LTE   || op == TOK_GTE    ||
        op == TOK_AND   || op == TOK_OR     ) {
        ZType *boolType = maketype(Z_TYPE_PRIMITIVE);
        boolType->primitive.token = maketoken(TOK_BOOL, NULL);
        boolType->tok = curr->tok;
        return boolType;
    }

    return promoted;
}

static ZType *resolveArrayLiteral(ZSemantic *ctx, ZNode *curr) {
    ZType *arrType = NULL;
    usize len = veclen(curr->arraylit);

    for (usize i = 0; i < len; i++) {
        ZNode *field = curr->arraylit[i];
        ZType *fieldType = resolveType(ctx, field);
        checkFunctionUsedAsValue(ctx, field);

        if (!arrType) {
            arrType = fieldType;
        } else {
            arrType = typesCompatible(ctx->state, arrType, fieldType);

            if (!arrType) {
                ZToken *tok = fieldType ? fieldType->tok : NULL;
                error(ctx->state, tok,
                             "Array literals should have the same type");
            }
        }
    }

    ZType *result       = maketype(Z_TYPE_ARRAY);
    result->array.base  = arrType;
    result->array.size  = len;
    result->tok         = curr->tok;

    return result;
}

static ZType *resolveTupleLiteral(ZSemantic *ctx, ZNode *node) {
    ZType **types = NULL;
    ZType *fieldType = NULL;

    ZNode **fields = node->tuplelit;
    for (usize i = 0; i < veclen(fields); i++) {
        fieldType = resolveType(ctx, fields[i]);
        if (!fieldType) {
            error(ctx->state, fields[i]->tok,
                    "Unresolved type of tuple");
        } else {
            vecpush(types, fieldType);
        }
    }

    ZType *result   = maketype(Z_TYPE_TUPLE);
    result->tuple   = types;
    result->tok     = node->tok;
    return result;
}

static ZType *resolveArrayInit(ZSemantic *ctx, ZNode *node) {
    node->arrayinit = resolveTypeRef(ctx, node->arrayinit);
    node->resolved = node->arrayinit;
    return node->arrayinit;
}

static ZType *resolveUnary(ZSemantic *ctx, ZNode *curr) {
    ZType     *operand = resolveType(ctx, curr->unary.operand);
    ZTokenType op      = curr->unary.operat->type;

    if (!operand) {
        error(ctx->state, curr->tok, "Unresolved type");
        return NULL;
    }

    switch (op) {
    case TOK_REF: {/* &expr => *T */
        ZType *ptr  = maketype(Z_TYPE_POINTER);
        ptr->base   = operand;
        ptr->tok    = curr->tok;
        return ptr;
    }

    case TOK_STAR:
        if (operand->kind != Z_TYPE_POINTER) {
            error(ctx->state, curr->unary.operat,
                "Cannot dereference a non-pointer type"
            );
        }
        return operand->base;

    case TOK_NOT:
        curr->unary.operand = implicitCast(ctx, curr->unary.operand, u1Type);
        return u1Type;
    default: return operand;
    }
}

static ZType *resolveStaticAccess(ZSemantic *ctx, ZNode *curr) {
    ZToken *base            = curr->staticAccess.base;
    ZToken *prop            = curr->staticAccess.prop;
    curr->staticAccess.func = NULL;

    ZSymbol *baseSym = resolve(ctx, base);
    if (!baseSym) {
        error(ctx->state, base, "Base not found");
        return NULL;
    }

    ZNode *staticMethod = resolveStaticFuncTable(ctx, base, prop);

    if (staticMethod) {
        curr->staticAccess.func = staticMethod;
        return staticMethod->resolved;
    }
    if (baseSym->kind == Z_SYM_TYPEDEF) {
        baseSym->type = resolveTypeRef(ctx, baseSym->type);
        if (!baseSym->type) {
            error(ctx->state, baseSym->node->tok, "Undefined base");
        }
        return NULL;
    }
    if (baseSym->kind == Z_SYM_STRUCT) {
        error(ctx->state, prop,
            "Static method '%s' not found", prop->str);
        
        return NULL;
    } else if (baseSym->type && baseSym->type->kind == Z_TYPE_ENUM) {
        ZType **fields  = baseSym->type->enm.fields;
        ZType *strct    = NULL;
        for (usize i = 0; i < veclen(fields) && !strct; i++) {
            if (tokeneq(fields[i]->strct.name, prop)) {
                strct = fields[i];
            }
        }

        if (!strct) {
            error(ctx->state, prop,
                "Field '%s' not found for enum '%s'",
                prop->str, base->str
            );
            return NULL;
        }

        /* Skip the first argument (always the flag). */
        for (usize i = 1; i < veclen(strct->strct.fields); i++) {
            strct->strct.fields[i]->field.type  = resolveTypeRef(
                ctx, strct->strct.fields[i]->field.type);
            strct->strct.fields[i]->resolved    = strct->strct.fields[i]->field.type;
        }
        curr->type = NODE_ENUM_LIT;
        return baseSym->type;
    } else if (baseSym->kind == Z_SYM_NAMESPACE) {
        ZType *resolved = NULL;
        for (usize i = 0; i < veclen(baseSym->node->block); i++) {
            ZNode *child = baseSym->node->block[i];
            if (child->type == NODE_FOREIGN && tokeneq(child->foreignFunc.tok, prop)) {
                resolved = child->resolved;
                break;
            }
        }
        if (!resolved) {
            error(ctx->state, prop,
                "'%s' not found in the namespace '%s'",
                prop->str, base->str
            );
            return NULL;
        } else {
            for (usize i = 0; i < veclen(resolved->func.args); i++) {
                resolved->func.args[i] = resolveTypeRef(
                    ctx,
                    resolved->func.args[i]
                );
            }
            resolved->func.ret = resolveTypeRef(
                ctx, resolved->func.ret
            );
            /* genStaticAccess looks up by mangled name, but genForeign
             * registers foreign functions under their plain C name. */
            curr->staticAccess.mangled = prop->str;
            return resolved;
        }
    } else {
        error(ctx->state, base, "Base should refer to a type");
        return NULL;
    }
}

static ZType *resolveSlice(ZSemantic *ctx, ZNode *curr) {
    ZType *res      = resolveType(ctx, curr->slice.base);
    ZType *start    = resolveType(ctx, curr->slice.start);
    ZType *end      = resolveType(ctx, curr->slice.end);
    
    if (curr->slice.start &&
            (!start                                 ||
            !isPrimitive(start)                     ||
            !isInteger(start->primitive.token->type))
        ) {
        error(ctx->state, curr->slice.start->tok, "Must be an integer");
    }
    if (curr->slice.end &&
            (!end                                   ||
            !isPrimitive(end)                       ||
            !isInteger(end->primitive.token->type)  )
        ) {
        error(ctx->state, curr->slice.end->tok, "Must be an integer");
    }

    curr->slice.start   = implicitCast(ctx, curr->slice.start, u64Type);
    curr->slice.end     = implicitCast(ctx, curr->slice.end, u64Type);

    return res;
}

static ZType *resolveInlineIf(ZSemantic *ctx, ZNode *node) {
    ZType *cond         = resolveType(ctx, node->ifStmt.cond);
    ZType *trueBranch   = resolveType(ctx, node->ifStmt.body);
    ZType *falseBranch  = resolveType(ctx, node->ifStmt.elseBranch);

    if (!isComparable(ctx, cond)) {
        error(ctx->state, node->ifStmt.cond->tok,
            "is not a comparable value");
        return NULL;
    }
    if (typesEqual(trueBranch, falseBranch)) {
        return trueBranch;
    }

    ZType *sum = maketype(Z_TYPE_SUM);
    sum->sumType = NULL;

    if (trueBranch->kind == Z_TYPE_SUM) {
        for (usize i = 0; i < veclen(trueBranch->sumType); i++) {
            vecpush(sum->sumType, trueBranch->sumType[i]);
        }
    } else {
        vecpush(sum->sumType, trueBranch);
    }

    if (falseBranch->kind == Z_TYPE_SUM) {
        for (usize i = 0; i < veclen(falseBranch->sumType); i++) {
            vecpush(sum->sumType, falseBranch->sumType[i]);
        }
    } else {
        vecpush(sum->sumType, falseBranch);
    }
    return sum;
}

/*
 * Resolve the type of any expression node and cache the result in node->resolved.
 * Returns the resolved ZType* or NULL on error.
 */
ZType *resolveType(ZSemantic *ctx, ZNode *curr) {
    if (!curr)           return NULL;
    if (curr->resolved)  return curr->resolved;

    ZType *result = NULL;

    switch (curr->type) {
    case NODE_CALL:         result = resolveFuncCall    (ctx, curr);    break;
    case NODE_UNARY:        result = resolveUnary       (ctx, curr);    break;
    case NODE_BINARY:       result = resolveBinary      (ctx, curr);    break;
    case NODE_MEMBER:       result = resolveMemberAccess(ctx, curr);    break;
    case NODE_LITERAL:      result = resolveLiteralType (curr->literalTok);         break;
    case NODE_ARRAY_LIT:    result = resolveArrayLiteral(ctx, curr);    break;
    case NODE_SUBSCRIPT:    result = resolveArrSubscript(ctx, curr);    break;
    case NODE_ARRAY_INIT:   result = resolveArrayInit   (ctx, curr);    break;
    case NODE_IDENTIFIER:   result = resolveIdentifier  (ctx, curr);    break;
    case NODE_STRUCT_LIT:   result = resolveStructLit   (ctx, curr);    break;
    case NODE_TUPLE_LIT:    result = resolveTupleLiteral(ctx, curr);    break;
    case NODE_STATIC_ACCESS:result = resolveStaticAccess(ctx, curr);    break;
    case NODE_SLICE:        result = resolveSlice       (ctx, curr);    break;
    case NODE_IF:           result = resolveInlineIf    (ctx, curr);    break;
    case NODE_VAR_DECL:
        /* Used when a var-decl appears as a sub-expression (unusual but safe). */
        if (curr->resolved) {
            result = resolveTypeRef(ctx, curr->resolved);
        } else if (curr->varDecl.rvalue) {
            result = resolveType(ctx, curr->varDecl.rvalue);
        }
        putVarPattern(ctx, curr, result, curr->varDecl.pattern, false);
        break;

    case NODE_CAST: {
        /* Resolve the inner expression type (for side-effects / validation). */
        ZType *expr = resolveType(ctx, curr->castExpr.expr);
        result = resolveTypeRef(ctx, curr->castExpr.toType);

        if (expr && expr->kind == Z_TYPE_ARRAY &&
            result->kind == Z_TYPE_ARRAY) {
            result->array.size = expr->array.size;
        }

        if (!typesCompatible(ctx->state, expr, result)) {
            error(ctx->state, curr->tok,
                "'%s' can't be casted to '%s'",
                stype(expr), stype(result)
            );
        }

        curr->castExpr.toType = result;
        break;
    }

    case NODE_SIZEOF: {
        /* sizeof yields u64. */
        curr->sizeofExpr.type = resolveTypeRef(ctx, curr->sizeofExpr.type);
        result = maketype(Z_TYPE_PRIMITIVE);
        result->primitive.token = maketoken(TOK_U64, "u64");
        result->tok             = curr->tok;
        break;
    }

    case NODE_BREAK:
        if (ctx->loopDepth == 0) {
            error(ctx->state, curr->tok, "break must be inside a loop");
        }
        break;
    case NODE_CONTINUE:
        if (ctx->loopDepth == 0) {
            error(ctx->state, curr->tok, "continue must be inside a loop");
        }
        break;

    default:
        warning(ctx->state, curr->tok,
                "Trying to resolve the type of node's type %d", curr->type);
        break;
    }

    curr->resolved = result;
    return result;
}

static ZType *resolveMemberAccess(ZSemantic *ctx, ZNode *curr) {
    ZType *objType = resolveType(ctx, curr->memberAccess.object);
    if (!objType) {
        error(ctx->state, curr->tok,
              "Cannot resolve object type in member access");
        return NULL;
    }

    ZType *base = derefType(objType);
    ZToken *field = curr->memberAccess.field;

    if (!base) {
        error(ctx->state, curr->tok,
              "Base type not found");
        return NULL;
    }
    
    if (base->kind == Z_TYPE_STRUCT) {
        ZNode *structField = getStructField(ctx, base, field);
        if (structField) {
            return structField->field.type;
        }
        ZNode *method = resolveFuncCallEmbedded(ctx, curr, objType, field);
        if (method) {
            return method->resolved;
        }
        error(ctx->state, field,
              "Member '%s' not found in '%s'", field->str, stype(base));
        return NULL;
    } else if (base->kind == Z_TYPE_TUPLE) {
        usize len = veclen(base->tuple);

        if (field->type != TOK_INT_LIT) {
            error(ctx->state, field, "Expected integer literal");
            return NULL;
        }

        if (field->integer < 0 || field->integer >= (i64)len) {
            error(ctx->state, field,
                    "Integer literal out of range for tuple indexing");
        }

        return base->tuple[field->integer];
    } else if (base->kind == Z_TYPE_ARRAY && strcmp(field->str, "len") == 0) {
        return u64Type;
    } else if (base->kind == Z_TYPE_ARRAY && strcmp(field->str, "ptr") == 0) {
        ZType *pointer = maketype(Z_TYPE_POINTER);
        pointer->base = base->array.base;
        return pointer;
    } else if (objType->kind == Z_TYPE_FACET) {
        ZType *func = NULL;
        for (usize i = 0; i < veclen(objType->facet.funcs); i++) {
            ZNode *funcField = objType->facet.funcs[i];
            if (tokeneq(field, funcField->field.identifier)) {
                func = funcField->resolved;
                break;
            }
        }

        if (!func) {
            error(ctx->state, curr->tok,
                "%s has no function called %s",
                stype(objType), stoken(field)
            );
        }
        return func;
    } else {
        ZNode *resolved = resolveFuncCallEmbedded(ctx, curr, objType, field);
        if (!resolved) {
            if (objType->kind == Z_TYPE_ARRAY &&
                    strcmp(field->str, "len") == 0) {
                return u64Type;
            } else if (objType->kind == Z_TYPE_ARRAY &&
                    strcmp(field->str, "ptr") == 0) {
                ZType *pointer = maketype(Z_TYPE_POINTER);
                pointer->base = objType->array.base;
                return pointer;
            }
            error(ctx->state, curr->tok,
                    "Expected a struct or tuple for '.' access");
            return NULL;
        }
        return resolved->resolved;
    }
}

static ZType *resolveArrSubscript(ZSemantic *ctx, ZNode *curr) {
    ZType *arrType      = resolveType(ctx, curr->subscript.arr);
    ZType *indexType    = resolveType(ctx, curr->subscript.index);

    /* Type not resolved */
    if (!arrType) return NULL;


    if (arrType->kind != Z_TYPE_ARRAY &&
        arrType->kind != Z_TYPE_POINTER) {
        error(ctx->state, curr->tok,
              "Expected an array type for subscript");
        return NULL;
    }

    if (!indexType || indexType->kind != Z_TYPE_PRIMITIVE ||
        !isInteger(indexType->primitive.token->type)) {
        error(ctx->state, curr->tok,
              "Array index must be an integer");
        return NULL;
    }

    if (arrType->kind == Z_TYPE_ARRAY) {
        return arrType->array.base;
    }
    return arrType->base;
}

static bool satisfyFacet(ZSemantic *ctx, ZType *type, ZType *facet) {

}

/* ================== Statement analysis ================== */

static void analyzeVar(ZSemantic *ctx, ZNode *curr, bool isGlobal) {
    (void)isGlobal;
    ZType *rvalueType   = NULL;
    ZType *declaredType = NULL;

    if (curr->varDecl.rvalue) {
        rvalueType = resolveType(ctx, curr->varDecl.rvalue);
        checkFunctionUsedAsValue(ctx, curr->varDecl.rvalue);
        rvalueType = resolveTypeRef(ctx, rvalueType);
    }

    if (curr->resolved) {
        declaredType = resolveTypeRef(ctx, curr->resolved);
        curr->resolved = declaredType;
        if (rvalueType &&
            !typesCompatible(ctx->state, declaredType, rvalueType)) {
            error(ctx->state, curr->tok,
                "Type mismatch: lvalue has type '%s' and rvalue has type '%s'",
                stype(declaredType),
                stype(rvalueType)
            );
        }
    } else {
        /* Inferred type (:= syntax) */
        declaredType        = rvalueType;
        curr->resolved      = rvalueType;
    }

    curr->resolved = declaredType;

    putVarPattern(
        ctx,
        curr,
        curr->resolved,
        curr->varDecl.pattern,
        false
    );
}

static void analyzeIf(ZSemantic *ctx, ZNode *curr) {
    bool isIfLet = curr->ifStmt.cond->type == NODE_VAR_DECL;
    if (isIfLet) beginScope(ctx, curr);
    ZType *cond = resolveType(ctx, curr->ifStmt.cond);
    
    if (!cond) {
        error(ctx->state, curr->ifStmt.cond->tok, "Unknown type condition");
    } else if (!isComparable(ctx, cond)) {
        error(ctx->state,
            curr->ifStmt.cond->tok,
            "%s cannot be used as a condition",
            stype(cond)
        );
    }

    if (curr->ifStmt.cond->type != NODE_VAR_DECL) {
        curr->ifStmt.cond = implicitCast(ctx, curr->ifStmt.cond, u1Type);
    }

    analyzeBlock(ctx, curr->ifStmt.body, true);
    if (isIfLet) endScope(ctx);

    if (curr->ifStmt.elseBranch) {
        ZNode *el = curr->ifStmt.elseBranch;
        if (el->type == NODE_IF)
            analyzeIf(ctx, el);
        else
            analyzeBlock(ctx, el, true);
    }
}

static void analyzeWhile(ZSemantic *ctx, ZNode *curr) {
    ZType *cond = resolveType(ctx, curr->whileStmt.cond);

    if (!isComparable(ctx, cond)) {
        error(ctx->state, curr->whileStmt.cond->tok,
                "Is not a comparable value");
    }

    curr->whileStmt.cond = implicitCast(ctx, curr->whileStmt.cond, u1Type);

    ctx->loopDepth++;
    analyzeBlock(ctx, curr->whileStmt.branch, true);
    ctx->loopDepth--;
}

static void analyzeFor(ZSemantic *ctx, ZNode *curr) {
    beginScope(ctx, curr);
    let f = curr->forStmt;
    analyzeVar(ctx, f.var, false);

    ZType *cond = resolveType(ctx, f.cond);
    curr->forStmt.cond->resolved = cond;

    if (!isComparable(ctx, cond)) {
        error(ctx->state, f.cond->tok, "Is not a comparable value");
    }

    curr->forStmt.cond = implicitCast(ctx, curr->forStmt.cond, u1Type);

    resolveType(ctx, f.incr);

    ctx->loopDepth++;
    analyzeBlock(ctx, curr->forStmt.block, false);
    ctx->loopDepth--;

    endScope(ctx);
}

static void analyzeForeign(ZSemantic *ctx, ZNode *curr) {
    if (!resolveTypeRef(ctx, curr->foreignFunc.ret)) {
        error(ctx->state, curr->tok, "Unknown type");
    }
    for (usize i = 0; i < veclen(curr->foreignFunc.args); i++) {
        ZType *arg = curr->foreignFunc.args[i];
        ZType *t = resolveTypeRef(ctx, arg);
        if (!t) {
            error(ctx->state, arg->tok, "Unknown type");
        } else {
            curr->foreignFunc.args[i] = t;
        }
    }
}

static void analyzeFuncArgs(ZSemantic *ctx, ZType **types, ZNode **fields) {
    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *field        = fields[i];
        ZType *fieldType    = resolveTypeRef(ctx, field->field.type);

        if (!fieldType) {
            error(ctx->state, field->field.identifier, "Unknown type resolved");
            continue;
        }
        field->field.type   = fieldType;
        field->resolved     = fieldType;
        types[i]            = fieldType;
        putRawSymbol(
            ctx,        Z_SYM_VAR,  field->field.identifier,
            fieldType,  field,      false
        );
    }
}

static bool satisfyReturn(ZSemantic *ctx, ZNode *node) {
    if (!node) return false;

    switch (node->type) {
    case NODE_RETURN: return true;
    case NODE_CAPABILITY:
        return satisfyReturn(ctx, node->capability.block);
    case NODE_BLOCK:
        for (usize i = 0; i < veclen(node->block); i++) {
            if (satisfyReturn(ctx, node->block[i])) return true;
        }
        return false;

    case NODE_IF:
        if (!node->ifStmt.elseBranch) return false;
        return  satisfyReturn(ctx, node->ifStmt.body) &&
                satisfyReturn(ctx, node->ifStmt.elseBranch);
    default: return false;
    }
    
}


static void analyzeFunc(ZSemantic *ctx, ZNode *curr) {
    for (usize i = 0; i < veclen(curr->funcDef.generics); i++) {
        putGeneric(ctx, curr->funcDef.generics[i]);
    }

    beginScope(ctx, curr);
    curr->funcDef.body->scope = ctx->table->current;
    if (curr->funcDef.base) {
        ZType *res = resolveTypeRef(ctx, curr->funcDef.base);
        if (!res) {
            error(ctx->state,
                    curr->funcDef.base->primitive.token,
                    "'%s' is not a valid identifier",
                    curr->funcDef.base->primitive.token->str);
            return;
        }
    }

    if (curr->funcDef.receiver) {
        ZNode *receiver = curr->funcDef.receiver;
        ZType *recType  = resolveTypeRef(ctx, receiver->field.type);
        curr->funcDef.receiver->resolved = recType;
        receiver->field.type = recType;

        ZSymbol *sym = makesymbol(Z_SYM_VAR);
        sym->name       = receiver->field.identifier;
        sym->type       = recType;
        sym->node       = curr->funcDef.receiver;
        sym->isPublic   = false;
        sym->useCount   = 1;
        putSymbol(ctx, sym);
    }

    analyzeFuncArgs(ctx,
        curr->resolved->func.args,
        curr->funcDef.args
    );

    analyzeFuncArgs(ctx,
        curr->resolved->func.capabilities,
        curr->funcDef.capabilities
    );

    for (usize i = 0; i < veclen(curr->funcDef.capabilities); i++) {
        putCapability(ctx, curr->funcDef.capabilities[i]);
    }

    ZType *savedRet             = ctx->currentFuncRet;
    ZNode *savedFunc            = ctx->currentFunc;

    curr->funcDef.ret           = resolveTypeRef(ctx, curr->funcDef.ret);
    curr->resolved->func.ret    = curr->funcDef.ret;
    ctx->currentFuncRet         = curr->funcDef.ret;
    ctx->currentFunc            = curr;

    analyzeBlock(ctx, curr->funcDef.body, false);
    if (!isVoid(ctx->currentFuncRet) &&
        !satisfyReturn(ctx, curr->funcDef.body)) {
        error(ctx->state, curr->tok, "Missing a return statement");
    }

    ctx->currentFuncRet    = savedRet;
    ctx->currentFunc       = savedFunc;
    
    endScope(ctx);
}

static bool isType(ZType *type, ZTokenType tok) {
    if (type->kind != Z_TYPE_PRIMITIVE) return false;
    return type->primitive.token->type == tok;
}

static void analyzeReturn(ZSemantic *ctx, ZNode *curr) {
    ZType *retType  = NULL;
    ZType *promoted = NULL;
    if (curr->returnStmt.expr) {
        retType     = resolveType(ctx, curr->returnStmt.expr);
        if (!retType) {
            error(ctx->state, curr->tok, "Return type not resolved");
            return;
        }
        retType     = resolveTypeRef(ctx, retType);
    }

    curr->resolved  = retType;

    if (!ctx->currentFuncRet) return;

    bool isVoidRet  = isVoid(retType);
    bool isVoidFunc = isType(ctx->currentFuncRet, TOK_VOID);

    if (isVoidFunc && !isVoidRet) {
        error(ctx->state, ctx->currentFunc->tok,
              "Unexpected return value in void function '%s'",
              stype(retType));
        return;
    } else if (!isVoidFunc && isVoidRet) {
        error(ctx->state, ctx->currentFunc->tok,
              "Expected a return value");
        return;
    } else if (!isVoidFunc && !isVoidRet) {
        promoted = typesCompatible(
            ctx->state, retType, ctx->currentFuncRet
        );

        if (!promoted) {
            error(ctx->state, curr->tok,
                "Expected type %s, got %s",
                stype(ctx->currentFuncRet),
                stype(retType)
            );
        } else {
            /* Implicit casting. */
            curr->returnStmt.expr = implicitCast(ctx, 
                curr->returnStmt.expr, ctx->currentFuncRet
            );
        }
    }
}

static void analyzeCapability(ZSemantic *ctx, ZNode *curr) {
    beginScope(ctx, curr);
    analyzeVar(ctx, curr->capability.capability, false);
    putCapability(ctx, curr->capability.capability);
    analyzeBlock(ctx, curr->capability.block, false);
    endScope(ctx);
}

static bool patternEq(ZState *state, ZVarDestructPattern *a, ZVarDestructPattern *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;

    switch (a->type) {
    case Z_VAR_IDENT: return true;
    case Z_VAR_TUPLE: {
        usize aLen = veclen(a->tuple);
        usize bLen = veclen(b->tuple);
        if (aLen != bLen) return false;

        for (usize i = 0; i < aLen; i++) {
            if (!patternEq(state, a->tuple[i], b->tuple[i])) return false;
        }
        return true;
    }
    case Z_VAR_STRUCT: {
        usize aLen = veclen(a->fields);
        usize bLen = veclen(b->fields);

        if (aLen != bLen) return false;

        for (usize i = 0; i < aLen; i++) {
            i32 index = -1;
            for (usize j = 0; i < bLen && index == -1; i++)
                if (tokeneq(b->fields[j]->key, a->fields[i]->key) == 0)
                    index = j;

            if (index == -1) return false;

            if (!patternEq(state,
                    a->fields[i]->value,
                    b->fields[index]->value)) {
                return false;
            }
        }

        return true;
    }
    case Z_VAR_ENUM: {
        if (!tokeneq(a->base, b->base)) return false;
        if (!tokeneq(a->prop, b->prop)) return false;

        usize aLen = veclen(a->args);
        usize bLen = veclen(b->args);

        if (aLen != bLen) return false;

        for (usize i = 0; i < aLen; i++) {
            if (!patternEq(state, a->args[i], b->args[i])) return false;
        }
        return true;
    }
    }
    return true;
}

static void analyzeMatchStmt(ZSemantic *ctx, ZNode *curr) {
    ZType *condType = resolveType(ctx, curr->match.cond);
    condType = resolveTypeRef(ctx, condType);
    if (!condType) return;

    for (usize i = 0; i < veclen(curr->match.arms); i++) {
        ZNode *arm = curr->match.arms[i];

        if (!arm->matchArm.expr) {
            error(ctx->state, arm->tok, "Invalid match arm");
            continue;
        }

        beginScope(ctx, arm);
        putVarPattern(ctx, arm, condType, arm->matchArm.pattern, true);
        if (arm->matchArm.expr->type == NODE_BLOCK) {
            analyzeBlock(ctx, arm->matchArm.expr, false);
        } else {
            resolveType(ctx, arm->matchArm.expr);
        }
        endScope(ctx);
    }
}

static void analyzeStmt(ZSemantic *ctx, ZNode *curr) {
    switch (curr->type) {
    case NODE_VAR_DECL:     analyzeVar(ctx, curr, false);           break;
    case NODE_IF:           analyzeIf(ctx, curr);                   break;
    case NODE_WHILE:        analyzeWhile(ctx, curr);                break;
    case NODE_FOR:          analyzeFor(ctx, curr);                  break;
    case NODE_BLOCK:        analyzeBlock(ctx, curr, false);         break;
    case NODE_DEFER:        resolveType(ctx, curr->deferStmt.expr); break;
    case NODE_RETURN:       analyzeReturn(ctx, curr);               break;
    case NODE_MATCH:        analyzeMatchStmt(ctx, curr);            break;
    case NODE_CAPABILITY:   analyzeCapability(ctx, curr);           break;
    default:                resolveType(ctx, curr);                 break;
    }
}

static void analyzeBlock(ZSemantic *ctx, ZNode *block, bool scoped) {
    if (scoped) beginScope(ctx, block);

    ZNode **stmts = block->block;
    usize len = veclen(stmts);
    for (usize i = 0; i < len; i++) {
        if (i + 1 < len && 
            (stmts[i]->type == NODE_BREAK ||
            stmts[i]->type == NODE_CONTINUE ||
            stmts[i]->type == NODE_RETURN)) {
            error(ctx->state, stmts[i+1]->tok, "Unreachable code");

            vecsetlen(block->block, i);

            break;
        }
        analyzeStmt(ctx, stmts[i]);
    }

    if (scoped) endScope(ctx);
}

/* ================== Global scope discovery ================== */

static void discoverGlobalScope(ZSemantic *ctx, ZNode *root) {
    for (usize i = 0; i < veclen(root->module.root); i++) {
        ZNode *child = root->module.root[i];

        switch (child->type) {
        case NODE_FUNC:         putFunc(ctx, child);        break;
        case NODE_STRUCT:       putStruct(ctx, child);      break;
        case NODE_VAR_DECL:     putVar(ctx, child, false);  break;
        case NODE_ENUM:         putEnum(ctx, child);        break;
        case NODE_NAMESPACE:    putNamespace(ctx, child);   break;
        case NODE_TYPEDEF:      putTypedef(ctx, child);     break;
        case NODE_FACET:        putFacet(ctx, child);       break;
        case NODE_FUNC_BLOCK:
            for (usize i = 0; i < veclen(child->funcblock.funcs); i++) {
                ZNode *func = child->funcblock.funcs[i];
                putFunc(ctx, func);
            }
            break;

        case NODE_FOREIGN: {
            /* Foreign functions are callable like regular functions.
               pub foreign declarations are placed in the global scope
               so importers of this module can call them without
               re-declaring the foreign themselves. */
            ZSymbol *symbol   = makesymbol(Z_SYM_FUNC);
            symbol->name      = child->foreignFunc.tok;
            symbol->node      = child;
            symbol->type      = child->resolved;
            symbol->isPublic  = child->foreignFunc.pub;
            putSymbol(ctx, symbol);
            break;
        }

        case NODE_MODULE:
            if (child->module.root) {
                registerModule(ctx, child);
                discoverGlobalScope(ctx, child);
                endModule(ctx, false);
            }
            break;

        default: break;
        }
    }
}

static void checkEmbedFieldConflicts(ZSemantic *ctx, ZType *strct, hashset_t *seen, ZToken *embedTok) {
    if (!strct || strct->kind != Z_TYPE_STRUCT) return;
    ZNode **fields = strct->strct.fields;
    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *field = fields[i];
        if (field->type == NODE_EMBED_FIELD) {
            ZType *nested = field->resolved;
            if (nested && nested->kind == Z_TYPE_STRUCT)
                checkEmbedFieldConflicts(ctx, nested, seen, embedTok);
        } else if (field->type == NODE_FIELD) {
            if (!hashset_insert(seen, field->field.identifier->str)) {
                error(ctx->state, embedTok,
                    "field '%s' conflicts with embedded struct '%s'",
                    field->field.identifier->str, stype(strct));
            }
        }
    }
}

static void analyzeStruct(ZSemantic *ctx, ZNode *structDef) {
    ZNode **fields = structDef->structDef.fields;
    usize len = veclen(fields);

    for (usize i = 0; i < len; i++) {
        ZNode *field = fields[i];

        if (field->type == NODE_EMBED_FIELD) {
            if (!field->tok && field->resolved && field->resolved->kind == Z_TYPE_PRIMITIVE)
                field->tok = field->resolved->primitive.token;
            field->resolved = resolveTypeRef(ctx, field->resolved);
        } else if (field->type == NODE_FIELD) {
            field->field.type = resolveTypeRef(ctx, field->field.type);
            field->resolved = field->field.type;
        } else {
            error(ctx->state, structDef->tok, "Invalid field type");
        }
    }

    ZType *structType = structDef->resolved;

    for (usize i = 0; i < len; i++) {
        let field = fields[i];
        ZType **szSeen = NULL;

        if (isInfiniteSize(field->field.type, structType, szSeen)) {
            error(ctx->state, field->field.identifier,
                  "field '%s' embeds struct by value causing infinite size; use a pointer",
                  field->field.identifier->str);
        }
    }

    hashset_t fieldSeen = NULL;
    for (usize i = 0; i < len; i++) {
        ZNode *field = fields[i];
        if (field->type == NODE_FIELD) {
            if (!hashset_insert(&fieldSeen, field->field.identifier->str)) {
                error(ctx->state, field->field.identifier,
                    "field '%s' already declared", field->field.identifier->str);
            }
        }
    }
    for (usize i = 0; i < len; i++) {
        ZNode *field = fields[i];
        if (field->type != NODE_EMBED_FIELD) continue;
        ZType *embedded = field->resolved;
        if (!embedded || embedded->kind != Z_TYPE_STRUCT) continue;
        checkEmbedFieldConflicts(ctx, embedded, &fieldSeen, field->tok);
    }
}

static void analyzeEnum(ZSemantic *ctx, ZNode *enumDef) {
    if (!enumDef->resolved) {
        error(ctx->state, enumDef->tok, "Expected a resolved type");
        return;
    }

    ZSymbol *sym = resolve(ctx, enumDef->enumDef.name);
    if (!sym) {
        error(ctx->state, enumDef->enumDef.name, "Enum not found");
        return;
    }

    if (sym->type->kind != Z_TYPE_ENUM) {
        error(ctx->state, enumDef->enumDef.name, "Type is not an enum");
        return;
    }

    ZType *enm = sym->type;
    ZType **fields = enm->enm.fields;
    hashset_t seen = NULL;

    for (usize i = 0; i < veclen(fields); i++) {
        if (!hashset_insert(&seen, fields[i]->strct.name->str)) {
            error(ctx->state, fields[i]->strct.name,
                "This field already declared in the same enum");
        }
        ZNode **enumField = fields[i]->strct.fields;

        for (usize j = 0; j < veclen(enumField); j++) {
            if (!enumField[j] || 
                !enumField[j]->field.type) continue; 
            ZType **szSeen = NULL;
            ZType *resolved = resolveTypeRef(ctx, enumField[j]->field.type);
            if (!resolved) continue;
            if (isInfiniteSize(resolved, enm, szSeen)) {
                error(ctx->state,
                    enumField[j]->field.type->tok,
                    "field '%s' embeds enum by value causing infinite size; use a pointer",
                    enumField[j]->field.type->tok->str);
            } else {
                enumField[j]->field.type = resolved;
                enumField[j]->resolved = resolved;
            }
        }
    }
}

static void analyzeNamespace(ZSemantic *ctx, ZNode *node) {
    for (usize i = 0; i < veclen(node->block); i++) {
        ZNode *child = node->block[i];
        if (child->type == NODE_FOREIGN) {
            analyzeForeign(ctx, child);
        } else if (child->type == NODE_NAMESPACE) {
            analyzeNamespace(ctx, child);
        }
    }
}

static void analyzeTypedef(ZSemantic *ctx, ZNode *node) {
    ZType *resolved = resolveTypeRef(ctx, node->typeDef.type);
    node->resolved = resolved;
}

static void analyzeFacet(ZSemantic *ctx, ZNode *node) {

}

/* ================== Main analysis pass ================== */

static void analyze(ZSemantic *ctx, ZNode *root) {
    root->module.scope = ctx->table->current;
    for (usize i = 0; i < veclen(root->module.root); i++) {
        ZNode *child = root->module.root[i];

        switch (child->type) {
        case NODE_FOREIGN:      analyzeForeign(ctx, child);     break;
        case NODE_FUNC:         analyzeFunc(ctx, child);        break;
        case NODE_TYPEDEF:      analyzeTypedef(ctx, child);     break;
        case NODE_VAR_DECL:     analyzeVar(ctx, child, true);   break;
        case NODE_STRUCT:       analyzeStruct(ctx, child);      break;
        case NODE_ENUM:         analyzeEnum(ctx, child);        break;
        case NODE_NAMESPACE:    analyzeNamespace(ctx, child);   break;
        case NODE_FACET:        analyzeFacet(ctx, child);       break;
        case NODE_MACRO:    /* does't require any validation*/  break;

        case NODE_MODULE:
            if (child->module.root) {
                registerModule(ctx, child);
                analyze(ctx, child);
                endModule(ctx, true);
            }
            break;

        case NODE_FUNC_BLOCK:
            for (usize i = 0; i < veclen(child->funcblock.funcs); i++) {
                ZNode *func = child->funcblock.funcs[i];
                analyzeFunc(ctx, func);
            }
            break;

        default: 
            warning(ctx->state, root->tok,
                    "node '%zu' not yet analyzed",
                    root->type);
            break;
        }
    }
}

static ZType *makePrimitiveType(ZTokenType type) {
    ZType *self = maketype(Z_TYPE_PRIMITIVE);
    self->primitive.token = maketoken(type, NULL);
    return self;
} 

ZSemantic *zanalyze(ZState *state, ZNode *root) {
    state->currentPhase = Z_PHASE_SEMANTIC;
    ZSemantic *ctx = makesemantic(state, root);

    if (!none)      none    = maketype(Z_TYPE_NONE);
    if (!u1Type)    u1Type  = makePrimitiveType(TOK_BOOL);
    if (!u64Type)   u64Type = makePrimitiveType(TOK_U64);

    registerModule(ctx, root);
    discoverGlobalScope(ctx, root);
    
    analyze(ctx, root);

    if (!ctx->main) {
        error(ctx->state, root->tok, "Missing 'main' declaration");
    }

    return ctx;
}
