/**
 * @file zsem.c
 * @brief This file is the Semantic ANalyzer (zsem).
 *
 * It operates on the AST produced by the parser and runs in two passes:
 *  1. discoverGlobalScope: registers all top-level declarations into the symbol
 *      table before analyzing any body. This allows mutual/forward references within a module.
 *
 *  2. analyze: walks every declaration body and performs:
 *      - Type resolution: resolves named type references (e.g. "MyStruct") to
 *        their actual ZType through the symbol table.
 *      - Type checking: validates that operands, assignment, return values
 *        and call arguments have compatible types. Inserts implicit casts
 *        where numeric promotion rules allow it.
 *      - Operator overloading: routes binary expressions to receiver methods
 *        marked with the #[overload] annotation.
 *      - Pattern destructuring: validates and binds identifiers introduced by
 *        tuple, struct and enum destructure patterns.
 *      - Return analysis: checks that every non-void function has a reachable return
 *        statement and that the returned type matches the declaration.
 *      - Capability scoping: tracks capability variables in the scope chain
 *        and injects them as implicit arguments at call sites that require them.
 *      - Facet satisfaction: verifies that impl blocks provide every method required
 *        by the facets they declare.
 *      - Struct/enum validation: detects duplicate fields, embedded-field name
 *        conflicts, and value-type cycles that would produce infinite size.
 *      - Unreachable code: errors on statements following break/continue/return.
 *      - Unused symbol warning: wans on functions, structs, and variables whose
 *        useCount is zero at scope-exit.
 * Scope model: global -> per-module -> per-block (function, if/else, loops etc.).
 * Each scope holds a symbol list, a "seen" hashset for duplicate detection,
 * and a capability list for the capability system.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */
#include "base.h"
#include "zhset.h"
#include "zinc.h"
#include "zvec.h"
#include "zarena.h"
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>

static void analyze                 (ZThreadSem *, ZNode *);
static void analyzeStruct           (ZThreadSem *, ZNode *);
static void analyzeEnum             (ZThreadSem *, ZNode *);
static void analyzeFacet            (ZThreadSem *, ZNode *);
static void analyzeTypedef          (ZThreadSem *, ZNode *);
static void analyzeStmt             (ZThreadSem *, ZNode *);
static void analyzeBlock            (ZThreadSem *, ZNode *, bool);
static void analyzeNamespace        (ZThreadSem *, ZNode *);
static void analyzeFuncArgs         (ZThreadSem *, ZType **, ZNode **, ZType **);
static bool satisfyFacet            (ZThreadSem *, ZType *, ZType *);
static ZType *resolveTypeRef        (ZThreadSem *, ZType *);
static void checkFunctionUsedAsValue(ZThreadSem *, ZNode *);
static ZFuncTable *resolveFuncTable (ZThreadSem *, ZType *);
static ZType *resolveType           (ZThreadSem *, ZNode *, ZType *);
static ZSymbol *resolve             (ZThreadSem *, ZToken *);
static ZType *typesCompatible       (ZThreadSem *, ZType *, ZType *);
static ZType *resolveLiteralType    (ZThreadSem *, ZToken *);
static ZType *resolveEnumLit        (ZThreadSem *, ZNode *, ZType *);
static ZSymbol *resolveModuleChain (ZThreadSem *, ZToken **, usize *);
/* ================== Scope / Symbol helpers ================== */

static ZNode *makeNodeThread(ZThreadSem *ctx, ZNodeType type) {
    ZNode *node = arenaAlloc(ctx->arena, sizeof(ZNode));
    *node = (ZNode){ 0 };
    node->type = type;
    return node;
}

static ZType *makeTypeThread(ZThreadSem *ctx, ZTypeKind kind) {
    ZType *type = arenaAlloc(ctx->arena, sizeof(ZType));
    *type = (ZType){ 0 };
    type->kind = kind;
    return type;
}

static ZToken *makeTokenThread(ZThreadSem *ctx, ZTokenType type, char *start) {
    ZToken *tok = arenaAlloc(ctx->arena, sizeof(ZToken));
    *tok = (ZToken){ 0 };
    tok->type = type;
    tok->start = start;
    return tok;
}

static ZScope *makescope(arena_t *arena, ZScope *parent, ZNode *node) {
    ZScope *self        = arenaAlloc(arena, sizeof(ZScope));
    self->depth         = parent ? parent->depth + 1 : 0;
    self->parent        = parent;
    self->node          = node;
    self->symbols       = NULL;
    self->seen          = NULL;
    self->capabilities  = NULL;
    return self;
}

static ZThreadSem *makethreadsem(ZSemantic *ctx, ZScope *current, ZNode *root, arena_t *arena) {
    ZThreadSem *self        = zalloc(ZThreadSem);

    self->arena             = arena;
    self->currentFunc       = NULL;
    self->currentFuncRet    = NULL;
    self->loopDepth         = 0;
    self->semantic          = ctx;
    self->state             = ctx->state;
    self->global            = current;
    self->local             = makescope(arena, current, root);
    self->current           = self->local;
    self->root              = root;
    self->funcs             = NULL;
    self->exportedFuncs     = NULL;

    return self;
}

static ZSymbol *makesymbol(arena_t *arena, ZSymType kind) {
    ZSymbol *self       = arenaAlloc(arena, sizeof(ZSymbol));
    self->kind          = kind;
    self->useCount      = 0;
    self->reachable     = false;
    return self;
}

static ZSemantic *makesemantic(ZState *state, ZNode *root) {
    ZSemantic *self         = zalloc(ZSemantic);
    self->root              = root;
    self->state             = state;
    self->scopes            = NULL;
    self->semantics         = NULL;
    // self->table             = makesymtable(allocator.ctx);
    return self;
}


static void putSymbol(ZThreadSem *ctx, ZSymbol *symbol) {
    ZScope *scope = symbol->isPublic ? ctx->global : ctx->current;

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
                        arena_t *arena,
                        ZSymType kind,
                        ZToken *name,
                        ZType *type,
                        ZNode *node,
                        bool isPublic) {
    ZSymbol *symbol = makesymbol(arena, kind);

    symbol->name        = name;
    symbol->type        = type;
    symbol->node        = node;
    symbol->isPublic    = isPublic;
    symbol->generics    = NULL;
    symbol->reachable   = strcmp(name->str, "_") == 0;

    return symbol;
}

static void putRawSymbol(ZThreadSem *ctx,
                        ZSymType kind,
                        ZToken *name,
                        ZType *type,
                        ZNode *node,
                        bool isPublic) {
    putSymbol(ctx,
        makeRawSymbol(
            ctx->arena,
            kind,
            name,
            type,
            node,
            isPublic
        ));
}

static ZFuncTable *makefunctable(ZType *base) {
    ZFuncTable *func        = zalloc(ZFuncTable);
    *func                   = (ZFuncTable){ 0 };
    func->base              = base;
    return func;
}

static ZFuncTable *putOrInsertFuncTable(ZThreadSem *ctx, ZType *type) {
    ZFuncTable *table = resolveFuncTable(ctx, type);
    if (table) return table;

    table = makefunctable(type);
    vecpush(ctx->funcs, table);

    return table;
}

static void addStaticFunc(ZThreadSem *ctx, ZNode *func) {
    ZType *base = func->funcDef.base;

    ZFuncTable *cur = putOrInsertFuncTable(ctx, base);
    char *name = func->funcDef.name->str;
    if (!hashset_insert(&cur->seenStaticFuncs, name)) {
        error(ctx->state, func->tok,
                "Duplicate static function '%s'", name);
        return;
    }

    vecpush(cur->staticFuncDef, func);
}

static void addReceiverFunc(ZThreadSem *ctx, ZNode *node) {
    ZNode *receiver         = node->funcDef.receiver;
    ZFuncTable *table       = putOrInsertFuncTable(ctx, receiver->field.type);

    if (!hashset_insert(&table->seenReceiverFuncs, node->funcDef.name->str)) {
        error(ctx->state,
            node->funcDef.name,
            "Duplicate receiver function '%s'", stoken(node->funcDef.name)
        );
    }
    vecpush(table->funcDef, node);
}

static void putReceiverFunc(ZThreadSem *ctx, ZNode *node) {
    if (!node->funcDef.receiver) {
        error(ctx->state, node->tok, "receiver must be setted");
        return;
    } else if (node->funcDef.base) {
        error(ctx->state, node->tok,
                "receiver functions cannot have a base");
        return;
    }
    addReceiverFunc(ctx, node);
}

static void putStaticFunc(ZThreadSem *ctx, ZNode *node) {
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

static void putFunc(ZThreadSem *ctx, ZNode *node) {
    if (node->funcDef.receiver) {
        if (node->funcDef.base) {
            error(ctx->state, node->tok,
                    "A static function cannot accept any receiver");
        }

        putReceiverFunc(ctx, node);
        if (node->funcDef.pub) vecpush(ctx->exportedFuncs, node);
    } else if (node->funcDef.base) {
        putStaticFunc(ctx, node);
        if (node->funcDef.pub) vecpush(ctx->exportedFuncs, node);
    } else {
        ZSymbol *f = makeRawSymbol(
                allocator.ctx,
                Z_SYM_FUNC,
                node->funcDef.name,
                node->resolved,
                node,
                node->funcDef.pub);

        if (strcmp(node->funcDef.name->str, "main") == 0) {
            ctx->semantic->main = f;
            if (node->funcDef.ret && isVoid(node->funcDef.ret)) {
                error(ctx->state, node->funcDef.name,
                      "'main' must return i32");
            }
        }
        putSymbol(ctx, f);
    }
}

ZNode *getStructField(ZThreadSem *ctx, ZType *strct, ZToken *field) {
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
        ZThreadSem *ctx, ZNode *node,
        ZType *type, ZVarDestructPattern *pattern, bool condition) {
    if (!type) {
        warning(ctx->state, node->tok, "No type provided for putVarPattern");
        return;
    }
    pattern->resolved = type;
    if (pattern->type == Z_VAR_LIT && condition) {
        ZType *literalType = resolveLiteralType(ctx, pattern->ident);
        if (!typesCompatible(ctx, literalType, type)) {
            error(ctx->state, pattern->tok,
                "Expected '%s', got '%s'",
                stype(type), stype(literalType)
            );
        }
        pattern->resolved = literalType;
    } else if (pattern->type == Z_VAR_IDENT) {
        putRawSymbol(
            ctx,            Z_SYM_VAR,
            pattern->ident, type,
            node,           false
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
                    "Expected %zu, got %zu elements %s", expected, got, stoken(pattern->tok));
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
        if (pattern->base->type == TOK_DOT) {
            pattern->base = type->enm.name;
        }

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
        ZNode **fields = type->enm.fields;
        for (usize i = 0; i < veclen(fields) && !variant; i++) {
            if (tokeneq(fields[i]->resolved->strct.name, pattern->prop))
                variant = fields[i]->resolved;
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

    } else if (pattern->type == Z_VAR_SUM) {
        if (type->kind != Z_TYPE_SUM) {
            error(ctx->state, pattern->tok,
                "Expected a sum type with '%s', got %s",
                stype(pattern->sum.type), stype(type)
            );
            return;
        }
        putVarPattern(ctx, node,
            pattern->sum.type,
            pattern->sum.child,
            condition
        );
    } else {
        error(ctx->state, pattern->tok, "Unhandled destructure pattern");
    }
}

static void putGeneric(ZThreadSem *ctx, ZType *type) {
    putRawSymbol(
        ctx,
        Z_SYM_GENERIC,
        type->generic.name,
        type,
        NULL,
        false
    );
}

static void putStruct(ZThreadSem *ctx, ZNode *node) {
    ZType *type             = maketype(Z_TYPE_STRUCT);
    type->strct.name        = node->structDef.ident;
    type->strct.fields      = node->structDef.fields;
    type->strct.generics    = NULL;
    type->strct.annotations = node->structDef.annotations;
    type->tok               = node->tok;
    node->resolved          = type;

    putRawSymbol(ctx,
            Z_SYM_STRUCT,
            node->structDef.ident,
            type,
            node,
            node->structDef.pub
    );
}

static void putEnum(ZThreadSem *ctx, ZNode *node) {
    /* enum integers can't hold payload. */
    if (node->resolved->enm.integer) {
        for (usize i = 0; i < veclen(node->enumDef.fields); i++) {
            // (node->enumDef.fields[i]->field.type);
        }
    }
    putRawSymbol(
        ctx,
        Z_SYM_ENUM,
        node->enumDef.name,
        node->resolved,
        node,
        node->enumDef.pub
    );
}

static void putTypedef(ZThreadSem *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_TYPEDEF,
        node->typeDef.alias,
        node->typeDef.type,
        node,
        node->typeDef.pub
    );
}

static void putFacet(ZThreadSem *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_FACET,
        node->facet.name,
        node->resolved,
        node,
        node->facet.pub
    );
}

static void putNamespace(ZThreadSem *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_FOREIGN,
        node->tok,
        node->resolved,
        node,
        node->pub
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

static ZCapability *putCapability(ZThreadSem *ctx, ZNode *var) {
    if (!var || !var->resolved) {
        error(ctx->state, var ? var->tok : NULL,
            "Invalid 'putCapability' call");
        return NULL;
    }

    ZScope *cur = ctx->current;

    i32 i = lookupCapabilityByType(cur, var->resolved);
    ZCapability *capability = NULL;
    if (i == -1) {
        capability          = arenaAlloc(ctx->arena, sizeof(ZCapability));
        capability->nodes   = NULL;
        capability->type    = var->resolved;
        vecpush(cur->capabilities, capability);
    } else {
        capability = cur->capabilities[i];
    }
    vecpush(capability->nodes, var);
    return capability;
}

static ZThreadSem *getRegisteredModule(ZSemantic *ctx, char *module) {
    for (usize i = 0; i < veclen(ctx->scopes); i++) {
        if (strcmp(ctx->scopes[i]->module->module.filename, module) == 0) {
            return ctx->scopes[i]->ctx;
        }
    }
    return NULL;
}

static ZThreadSem *registerModule(ZSemantic *ctx, ZNode *module) {
    for (usize i = 0; i < veclen(ctx->scopes); i++) {
        if (ctx->scopes[i]->module == module) {
            ctx->semantics[i]->current = ctx->semantics[i]->local;
            return ctx->semantics[i];
        }
    }

    /* Parent the module scope to the lexically enclosing scope (the importer),
     * so a module can resolve names from the scope that pulled it in. */
    ZScopeTable *table  = zalloc(ZScopeTable);
    table->module       = module;
    table->scope        = makescope(allocator.ctx, NULL, module);

    ZModuleAllocator *m = zalloc(ZModuleAllocator);
    m->module           = module;
    m->allocator        = createArena();

    vecpush(ctx->state->modules, m);
    vecpush(ctx->scopes, table);

    ZThreadSem *threadCtx = makethreadsem(
        ctx, table->scope, module, m->allocator
    );
    table->ctx = threadCtx;
    vecpush(ctx->semantics, threadCtx);
    return threadCtx;
}

static void warnUnused(ZThreadSem *ctx, ZSymbol *symbol) {
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

static void checkUnusedSymbols(ZThreadSem *ctx) {
    ZScope *scope = ctx->current;
    for (usize i = 0; i < veclen(scope->symbols); i++) {
        let symbol = scope->symbols[i];
        if (!symbol->reachable && symbol->useCount == 0) {
            warnUnused(ctx, symbol);
        }
    }
}

static void beginScope(ZThreadSem *ctx, ZNode *curr) {
    ZScope *scope       = makescope(ctx->arena, ctx->current, curr);
    ctx->current = scope;
}

static void endScope(ZThreadSem *ctx) {
    if (!ctx->current) {
        error(ctx->state, NULL, "Exited highest level or scope not set");
        return;
    } else if (!ctx->current->parent) {
        error(ctx->state, NULL, "Called endScope at the highest level");
        return;
    }
    checkUnusedSymbols(ctx);
    ctx->current = ctx->current->parent;
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

static inline bool isUnsigned(ZToken *t) { return tokmask(t, TOK_UNSIGNED);  }
static inline bool isSigned  (ZToken *t) { return tokmask(t, TOK_SIGNED);    }
static inline bool isFloat   (ZToken *t) { return tokmask(t, TOK_FLOAT);     }
static inline bool isInteger (ZToken *t) { return isSigned(t) || isUnsigned(t);  }
static inline bool isPrimitive(ZType *t)    { return t->kind == Z_TYPE_PRIMITIVE;   }
static inline bool isNumeric(ZType *t) {
    if (!t || !isPrimitive(t)) return false;
    return tokmask(t->primitive.token, (TOK_SIGNED | TOK_UNSIGNED | TOK_FLOAT));
}

static ZTokenType toSigned(u8 rank) {
    switch (rank) {
    case 1: return TOK_I8;
    case 2: return TOK_I16;
    case 3: return TOK_I32;
    case 4: return TOK_I64;
    default: return TOK_I32;
    }
}

static bool isComparable(ZThreadSem *ctx, ZType *type) {
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
static ZType *typesCompatible(ZThreadSem *ctx, ZType *a, ZType *b) {
    if (!a || !b) return NULL;

    if (a->kind == Z_TYPE_FACET     &&
        b->kind == Z_TYPE_POINTER   &&
        satisfyFacet(ctx, b, a)     ) {
        return a;
    }

    if (a->kind == Z_TYPE_POINTER && isNumeric(b)) return a;
    if (b->kind == Z_TYPE_POINTER && isNumeric(a)) return b;

    if (a->kind == Z_TYPE_POINTER && b->kind == Z_TYPE_NONE) {
        return a;
    } else if (b->kind == Z_TYPE_POINTER && a->kind == Z_TYPE_NONE) {
        return b;
    } else if (a->kind == Z_TYPE_POINTER && b->kind == Z_TYPE_POINTER) {
        return a;
    }

    if (typesEqual(a, b)) return b;

    if (b->kind == Z_TYPE_SUM) {
        for (usize i = 0; i < veclen(b->sumType); i++)
            if (typesEqual(a, b->sumType[i])) return b;
    }
    if (a->kind == Z_TYPE_SUM) {
        for (usize i = 0; i < veclen(a->sumType); i++)
            if (typesEqual(b, a->sumType[i])) return a;
    }

    if (a->kind != Z_TYPE_PRIMITIVE || b->kind != Z_TYPE_PRIMITIVE)
        return NULL;

    ZToken *tokA    = a->primitive.token;
    ZToken *tokB    = b->primitive.token;
    ZTokenType ta   = tokA->type;
    ZTokenType tb   = tokB->type;

    if (ta == TOK_VOID || tb == TOK_VOID) return NULL;

    u8 ra = typeRank(ta);
    u8 rb = typeRank(tb);

    if (isFloat(tokA) || isFloat(tokB)) {
        return ra > rb ? a : b;
    }

    if ((isSigned(tokA) && isSigned(tokB)) ||
        (isUnsigned(tokA) && isUnsigned(tokB)))
        return ra > rb ? a : b;

    /* signed vs unsigned */
    u8    signedRank   = isSigned(tokA) ? ra : rb;
    u8    unsignedRank = isSigned(tokA) ? rb : ra;
    ZType *signedType  = isSigned(tokA) ? a  : b;

    if (signedRank > unsignedRank) return signedType;

    // if (signedRank == 4) {
    //     warning(ctx->state, signedType->tok,
    //             "Cannot promote a 64-bits integer, try with an explicit cast");
    // }

    ZType *promoted             = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
    promoted->primitive.token   = makeTokenThread(ctx, toSigned(signedRank + 1), NULL);
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
}

int compareTypes(const void *a, const void *b) {
    ZType *typeA = *(ZType * const *)a;
    ZType *typeB = *(ZType * const *)b;

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

    // if (a->hash && b->hash && a->hash == b->hash) return true;

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
    case Z_TYPE_FACET: {
        if (!tokeneq(a->facet.name, b->facet.name)) return false;

        if (veclen(a->facet.funcs) != veclen(b->facet.funcs)) return false;

        for (usize i = 0; i < veclen(a->facet.funcs); i++) {
            if (!typesEqual(
                    a->facet.funcs[i]->resolved,
                    b->facet.funcs[i]->resolved))
                return false;
        }
        return true;
    }
    case Z_TYPE_OPTIONAL:
        return typesEqual(a->optional, b->optional);
    case Z_TYPE_RESULT:
        return
            typesEqual(a->result.success,   b->result.success) &&
            typesEqual(a->result.error,     b->result.error);

    default:
        return false;
    }
}

static ZNode *implicitCast(ZThreadSem *ctx, ZNode *node, ZType *type) {
    (void)ctx;
    if (!node) return node;
    if (node->resolved && typesEqual(node->resolved, type)) {
        return node;
    }

    ZNode *cast             = makeNodeThread(ctx, NODE_CAST);
    cast->castExpr.expr     = node;
    cast->castExpr.toType   = type;
    cast->resolved          = type;
    cast->tok               = node->tok;
    return cast;
}

/*
 * @brief Coerce an expression to a (flattened) sum type.
 *
 * Inline-if branches are resolved bottom-up, so a nested if can end up with a
 * narrower sum than the context expects. A narrower sum has a different variant order,
 * so its runtime tags don't line up with the wider one.  Rather than re-tag at runtime,
 * we thread the final sum type down through every nested inline-if and cast each
 * leaf branch straight to it, so leaves are tagged in the outer variant order
 * and an if just stores the already-correct struct.
 */
static ZNode *coerceToSum(ZThreadSem *ctx, ZNode *node, ZType *sum) {
    if (!node || !node->resolved) return node;

    if (node->type == NODE_IF && node->resolved->kind == Z_TYPE_SUM) {
        node->ifStmt.body       = coerceToSum(ctx, node->ifStmt.body, sum);
        node->ifStmt.elseBranch = coerceToSum(ctx, node->ifStmt.elseBranch, sum);
        node->resolved          = sum;
        return node;
    }

    if (typesEqual(node->resolved, sum)) return node;
    return implicitCast(ctx, node, sum);
}

/* ================== Symbol lookup ================== */

static inline ZSymbol *resolveByScope(ZScope *scope, ZToken *ident) {
    while (scope) {
        for (usize i = 0; i < veclen(scope->symbols); i++) {
            if (tokeneq(scope->symbols[i]->name, ident)) {
                scope->symbols[i]->useCount++;
                return scope->symbols[i];
            }
        }
        scope = scope->parent;
    }
    return NULL;
}

static ZSymbol *resolve(ZThreadSem *ctx, ZToken *ident) {
    if (ident && ident->type == TOK_IDENT && strcmp(ident->str, "_") == 0) {
        error(ctx->state, ident, "Use '_' only for unused variables");
    }
    return resolveByScope(ctx->current, ident);
}

/* ================== Type resolution ================== */

static inline ZType *derefType(ZType *t) {
    while (t && t->kind == Z_TYPE_POINTER) t = t->base;
    return t;
}

static ZType *resolveLiteralType(ZThreadSem *ctx, ZToken *curr) {
    if (curr->type == TOK_NONE) return none;

    ZType *t = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
    switch (curr->type) {
    case TOK_RUNE_LIT:
    case TOK_INT_LIT: {
        t->primitive.token = makeTokenThread(ctx, TOK_I32, NULL);
        break;
    }
    case TOK_FLOAT_LIT: {
        t->primitive.token = makeTokenThread(ctx, TOK_F64, NULL);
        break;
    }
    case TOK_FALSE:
    case TOK_TRUE: {
        t->primitive.token = makeTokenThread(ctx, TOK_BOOL, NULL);
        break;
    }
    case TOK_STR_LIT: {
        /* String literals are *char */
        ZType *base = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
        base->primitive.token = makeTokenThread(ctx, TOK_CHAR, NULL);
        t->kind = Z_TYPE_POINTER;
        t->base   = base;
        break;
    }
    default: {
        t->primitive.token = makeTokenThread(ctx, TOK_VOID, NULL);
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
static bool isInfiniteSize(ZType *type, ZType *root, ZType ***seen) {
    if (!type) return false;
    switch (type->kind) {
    case Z_TYPE_STRUCT: {
        if (type == root) return true;

        for (usize i = 0; i < veclen(*seen); i++)
            if (typesEqual((*seen)[i], type)) return false;

        vecpush(*seen, type);
        ZNode **fields = type->strct.fields;
        for (usize i = 0; i < veclen(fields); i++)
            if (isInfiniteSize(fields[i]->resolved, root, seen))
                return true;
        return false;
    }

    case Z_TYPE_ENUM: {
        if (type == root) return true;

        for (usize i = 0; i < veclen(*seen); i++)
            if (typesEqual((*seen)[i], type)) return false;

        vecpush(*seen, type);

        ZNode **fields = type->enm.fields;
        for (usize i = 0; i < veclen(fields); i++) {
            if (isInfiniteSize(fields[i]->resolved, root, seen)) return true;
        }
        return false;
    }

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

static ZType *_resolveTypeRef(ZThreadSem *ctx, ZType *type, ZType ***seen) {
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
        if (!sym) return NULL;
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
        for (usize i = 0; i < veclen(type->func.capabilities); i++) {
            type->func.capabilities[i] = _resolveTypeRef(ctx,
                type->func.capabilities[i],
                seen);
        }
        return type;
    case Z_TYPE_TUPLE:
        for (usize i = 0; i < veclen(type->tuple); i++)
            type->tuple[i] = _resolveTypeRef(ctx, type->tuple[i], seen);
        return type;
    case Z_TYPE_OPTIONAL:
        type->optional = _resolveTypeRef(ctx, type->optional, seen);
        return type;
    case Z_TYPE_SUM:
        for (usize i = 0; i < veclen(type->sumType); i++)
            type->sumType[i] = _resolveTypeRef(ctx, type->sumType[i], seen);
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
static ZType *resolveTypeRef(ZThreadSem *ctx, ZType *type) {
    if (!type) return NULL;
    ZType **seen = NULL;
    return _resolveTypeRef(ctx, type, &seen);
}

static ZType *resolveMemberAccess(ZThreadSem *, ZNode *, ZType *);
static ZType *resolveArrSubscript(ZThreadSem *, ZNode *, ZType *);

static bool funcTableBaseMatches(ZThreadSem *, ZType *, ZType *);

static ZNode **resolveModuleStaticFuncTable(ZThreadSem *ctx,
    ZFuncTable **tables, ZType *base, ZToken *prop) {
    for (usize i = 0; i < veclen(tables); i++) {
        ZFuncTable *table = tables[i];
        if (!funcTableBaseMatches(ctx, table->base, base)) continue;

        if (hashset_has(table->seenStaticFuncs, prop->str))
            return table->staticFuncDef;
    }
    return NULL;
}

static ZNode *resolveStaticFuncTable(ZThreadSem *ctx,
    ZType *base, ZToken *prop) {
    ZNode **staticFuncs = resolveModuleStaticFuncTable(
        ctx, ctx->funcs, base, prop);

    if (!staticFuncs) return NULL;

    ZNode *node     = NULL;
    for (usize i = 0; i < veclen(staticFuncs) && !node; i++) {
        ZNode *func = staticFuncs[i];
        if (funcTableBaseMatches(ctx, func->funcDef.base, base) &&
            tokeneq(func->funcDef.name, prop)) {
            node    = func;
        }
    }

    if (!node) return NULL;

    ZType **args = node->resolved->func.args;
    for (usize i = 0; i < veclen(args); i++) {
        if (!args[i]) {
            error(ctx->state, node->tok, "unresolved type");
            continue;
        }
        ZType *resolved = resolveTypeRef(ctx, args[i]);
        if (!resolved) {
            error(ctx->state, args[i]->tok,
                "Unresolved type");
        } else {
            args[i] = resolved;
        }
    }
    return node;
}

static bool funcTableBaseMatches(ZThreadSem *ctx, ZType *tableBase, ZType *obj) {
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

static ZFuncTable *resolveFuncTable(ZThreadSem *ctx, ZType *obj) {
    for (usize i = 0; i < veclen(ctx->funcs); i++) {
        if (funcTableBaseMatches(ctx, ctx->funcs[i]->base, obj))
            return ctx->funcs[i];
    }
    return NULL;
}


static ZNode *resolveFuncCallEmbedded(ZThreadSem *ctx,
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

    if (ptr) curr->memberAccess.mangled = ptr->funcDef.mangled;
    return ptr;
}

static ZNode *lookupScopedCapability(ZThreadSem *ctx, ZType *required) {
    ZScope *curr = ctx->current;
    i32 i = -1;
    while (curr) {
        i = lookupCapabilityByType(curr, required);
        if (i != -1) break;
        curr = curr->parent;
    }
    if (i == -1 || veclen(curr->capabilities[i]->nodes) == 0) return NULL;
    return veclast(curr->capabilities[i]->nodes);
}

static ZNode **analyzeCapabilities(ZThreadSem *ctx, ZToken *start, ZType **capabilities) {
    ZNode **capabilityRefs = NULL;
    for (usize i = 0; i < veclen(capabilities); i++) {
        ZType *old = capabilities[i];
        capabilities[i] = resolveTypeRef(ctx, old);

        if (!capabilities[i]) {
            printf("error");
            error(ctx->state, start,
                "Capability '%s' not found", stype(old)
            );
            continue;
        }
        ZNode *reference = lookupScopedCapability(
            ctx, capabilities[i]
        );
        if (!reference) {
            error(ctx->state, start,
                "Capability '%s' not found in the current scope",
                stype(capabilities[i]));
        }
        vecpush(capabilityRefs, reference);
    }
    return capabilityRefs;
}

static void resolveFuncArgs(
    ZThreadSem *ctx, ZType **expectedArgs, ZNode **args,
    ZToken *start, bool variadic) {

    usize expectedArgsLen = veclen(expectedArgs);

    if ((!variadic && expectedArgsLen != veclen(args)) ||
        ( variadic && expectedArgsLen >  veclen(args))) {
        error(ctx->state, start,
                "Expected %zu argument(s), got %zu",
                expectedArgsLen, veclen(args));
        return;
    }


    for (usize i = 0; i < expectedArgsLen; i++) {
        ZType *expected = expectedArgs[i];

        args[i]->resolved = resolveType(ctx, args[i], expected);
        checkFunctionUsedAsValue(ctx, args[i]);

        /* If the argument is a generic skip the validation*/
        if (expected && expected->kind == Z_TYPE_GENERIC) {
            continue;
        }

        /* if they are equal they don't need implicit casting. */
        if (typesEqual(args[i]->resolved, expected)) continue;

        ZType *promoted = typesCompatible(
            ctx, args[i]->resolved, expected);

        if (!promoted) {
            error(ctx->state, args[i]->tok,
                "Expected %s, got %s",
                stype(expected),
                stype(args[i]->resolved)
            );
        }
        args[i] = implicitCast(ctx, args[i], expected);
    }

    for (usize i = expectedArgsLen; i < veclen(args); i++) {
        args[i]->resolved = resolveType(ctx, args[i], NULL);
        checkFunctionUsedAsValue(ctx, args[i]);
    }
}

static ZType *resolveFuncCall(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZNode *callee = curr->call.callee;
    ZNode **args = curr->call.args;

    ZType *expectedFunc = NULL;
    if (callee->type == NODE_IDENTIFIER) {
        ZSymbol *sym = resolve(ctx, callee->identNode.tok);
        if (!sym || !sym->type) {
            error(ctx->state, callee->identNode.tok,
                  "Undefined function '%s'", callee->identNode.tok->str);
            return NULL;
        }
        callee->identNode.ref = sym->node;
        callee->identNode.li  = getLangItemType(sym->node);

        if (sym->kind == Z_SYM_FACET) {
            if (veclen(args) != 1) {
                error(ctx->state, callee->tok,
                    "Expected 1 argument for facet construction, got %zu",
                    veclen(args)
                );
                return sym->type;
            }
            ZType *arg = resolveType(ctx, args[0], inferred);
            args[0]->resolved = arg;
            if (!satisfyFacet(ctx, arg, sym->type)) {
                error(ctx->state, args[0]->tok,
                    "'%s' doesn't implement facet '%s'", stype(arg), stype(sym->type)
                );
            }
            curr->call.callee->resolved = sym->type;
            return sym->type;
        } else if (sym->kind != Z_SYM_FUNC) {
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
            /* Foreign declarations store their fully-resolved signature in
             * sym->type (resolved in analyzeForeign); only NODE_FUNC keeps the
             * raw parsed return type in funcDef.ret. Reading funcDef.ret on a
             * NODE_FOREIGN would alias the wrong union member and corrupt the
             * return type. */
            ZType *rawRet = sym->type->func.ret;
            if (sym->node->type == NODE_FUNC) {
                callee->identNode.mangled = sym->node->funcDef.mangled;
                rawRet = sym->node->funcDef.ret;
            }

            /* Resolve the signature so that named types (e.g. "Vec2" ->
             * Z_TYPE_STRUCT) are expanded before the result is used downstream
             * (e.g. for member access on the return value). */
            for (usize i = 0; i < veclen(sym->type->func.args); i++) {
                sym->type->func.args[i] = resolveTypeRef(
                    ctx, sym->type->func.args[i]
                );
            }
            sym->type->func.ret     = resolveTypeRef(ctx, rawRet);
            if (sym->node->type == NODE_FUNC) {
                sym->node->funcDef.ret = sym->type->func.ret;
            }
            expectedFunc            = sym->type;
            callee->resolved        = expectedFunc->func.ret;
        }
    } else if (callee->type == NODE_MEMBER) {
        ZType *resolved = resolveType(ctx, callee, inferred);
        if (callee->type == NODE_ENUM_LIT_NO_PAYLOAD) callee->type = NODE_MEMBER;
        if (resolveEnumLit(ctx, callee, inferred)) {
            curr->type = NODE_ENUM_LIT;
            callee->type = NODE_MEMBER;
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
            ZNode **variants    = resolved->enm.fields;
            ZNode **fields      = NULL;
            ZToken *prop        = callee->memberAccess.field;
            for (usize i = 0; i < veclen(resolved->enm.fields) && !fields; i++) {
                if (tokeneq(variants[i]->resolved->strct.name, prop)) {
                    fields = variants[i]->resolved->strct.fields;
                }
            }
            if (!fields) {
                error(ctx->state, callee->tok,
                    "Invalid enum variant '%s'",
                    stoken(prop)
                );
                return NULL;
            }

            curr->resolved = resolved;
            expectedFunc = makeTypeThread(ctx, Z_TYPE_FUNCTION);
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
        ZType *calleeType = resolveType(ctx, callee, inferred);
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

    resolveFuncArgs(
        ctx,        expectedFunc->func.args,    args,
        curr->tok,  expectedFunc->func.variadic
    );

    curr->call.capabilities = analyzeCapabilities(
        ctx, curr->tok, expectedFunc->func.capabilities
    );

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

static ZType *resolveStructLit(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *symType      = NULL;
    ZSymbol *structSym  = NULL;
    ZToken **chain      = curr->structlit.chain;
    ZToken *ident       = veclast(curr->structlit.chain);
    if (veclen(chain) == 1 && ident->type == TOK_DOT) {
        symType         = resolveTypeRef(ctx, inferred);
        if (!symType) {
            error(ctx->state, ident,
                "struct type can't be inferred here"
            );
            return NULL;
        }

        if (symType->kind != Z_TYPE_STRUCT) return NULL;
        structSym   = resolve(ctx, symType->strct.name);

    } else {
        usize i;
        structSym   = resolveModuleChain(ctx, curr->structlit.chain, &i);

        if (!structSym) {
            error(ctx->state, curr->tok,
                    "struct '%s' not found", curr->tok->str);
            return NULL;
        }
        symType = resolveType(ctx, structSym->node, inferred);
    }
    if (!symType || symType->kind != Z_TYPE_STRUCT) {
        error(ctx->state, ident,
                    "'%s' is not a struct", stoken(ident));
        return NULL;
    }

    if (veclen(curr->structlit.fields) != veclen(symType->strct.fields)) {
        warning(ctx->state, curr->tok, "Some fields not initialized");
    }

    for (usize i = 0; i < veclen(curr->structlit.fields); i++) {
        ZNode *field        = curr->structlit.fields[i];
        ZNode *structField  = getStructField(ctx, symType, field->tok);

        if (!structField) {
            error(ctx->state, field->tok, "Invalid struct field");
            continue;
        }

        ZType *expectedType = resolveTypeRef(ctx, structField->field.type);
        ZType *type         = resolveType(ctx, field->varDecl.rvalue, expectedType);
        ZType *promoted     = NULL;

        if (!type) {
            error(ctx->state, field->varDecl.rvalue->tok,
                "Unresolved type");
            continue;
        }

        field->resolved = type;

        if (!structField) {
            error(ctx->state, field->tok,
                "Field '%s' not found for struct '%s'",
                field->tok->str, stype(structSym->type)
            );
            return NULL;
        }
        structField->field.type = expectedType;
        promoted = typesCompatible(ctx, expectedType, type);
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

static void checkFunctionUsedAsValue(ZThreadSem *ctx, ZNode *node) {
    if (!node || node->type != NODE_MEMBER) return;
    if (node->memberAccess.mangled) {
        error(ctx->state, node->memberAccess.field,
            "cannot use receiver method '%s' as a value",
            node->memberAccess.field->str);
    }
}

static ZType *resolveIdentByScope(ZThreadSem *ctx, ZScope *scope, ZNode *node) {
    ZToken *tok = node->identNode.tok;
    ZSymbol *sym = resolveByScope(scope, tok);
    if (!sym) {
        error(ctx->state, tok,
              "Undefined identifier '%s'", tok->str);
        return NULL;
    }
    node->identNode.ref = sym->node;
    node->identNode.sym = sym;
    node->identNode.li  = getLangItemType(sym->node);

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

static ZType* resolveIdent(ZThreadSem *ctx, ZNode *node, ZType *inferred) {
    (void)inferred;
    return resolveIdentByScope(ctx, ctx->current, node);
}

static ZType *resolveBinary(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZToken *tok         = curr->binary.op;
    ZTokenType op       = tok->type;
    ZType     *left     = resolveType(ctx, curr->binary.left, inferred);

    if (op == TOK_EQ || tokmask(tok, TOK_SELF_OPERATOR)) inferred = left;
    ZType     *right    = resolveType(ctx, curr->binary.right, inferred);

    if (tokmask(tok, TOK_BITOPERATOR_MASK) &&
            (!isNumeric(left) ||
             !isNumeric(right))) {
        error(ctx->state, curr->binary.op,
            "Bit operators can be used only with integers");
        return NULL;
    }

    if (tokmask(tok, TOK_SELF_OPERATOR)) {
        if (!isNumeric(left) || !isNumeric(right)) {
            error(ctx->state, curr->binary.op,
                "Compound operator can be used only with numeric types");
            return NULL;
        }
        if (!isLvalue(curr->binary.left)) {
            error(ctx->state, curr->binary.left->tok,
                    "is not a valid lvalue");
            return NULL;
        }
        if (!typesCompatible(ctx, left, right)) {
            error(ctx->state, curr->binary.op,
                "Incompatible type '%s' with '%s'",
                stype(left), stype(right));
            return NULL;
        }
        /* The result and the stored value keep the type of the lhs:
         * only the rhs is coerced, the lhs must stay a plain lvalue. */
        curr->binary.right = implicitCast(ctx, curr->binary.right, left);
        return left;
    }

    /* Auto promotion rules should be handled by typesCompatible. */
    ZType *promoted     = typesCompatible(ctx, left, right);

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
    switch (op) {
    case TOK_EQEQ:
    case TOK_NOTEQ:
    case TOK_LT:
    case TOK_GT:
    case TOK_LTE:
    case TOK_GTE:
    case TOK_AND:
    case TOK_OR: {
        ZType *boolType = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
        boolType->primitive.token = makeTokenThread(ctx, TOK_BOOL, NULL);
        boolType->tok = curr->tok;
        return boolType;
    }

    case TOK_PLUS:
        if (left->kind == Z_TYPE_POINTER &&
            isNumeric(right)             ) {
            return left;
        }
        if (right->kind == Z_TYPE_POINTER    &&
            isNumeric(left)) {
            return right;
        }
    case TOK_DIV:
    case TOK_STAR:
    case TOK_MINUS: {
        if (!isNumeric(curr->binary.left->resolved) ||
            !isNumeric(curr->binary.right->resolved)) {
            error(ctx->state, curr->tok, "Expected numeric types");
        }
        return promoted;
    }
    default: return promoted;
    }
}

static ZType *resolveArrayLiteral(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *arrType = NULL;
    usize len = veclen(curr->arraylit);

    if (inferred && inferred->kind == Z_TYPE_ARRAY) {
        inferred = inferred->array.base;
    }

    for (usize i = 0; i < len; i++) {
        ZNode *field = curr->arraylit[i];
        ZType *fieldType = resolveType(ctx, field, inferred);
        checkFunctionUsedAsValue(ctx, field);

        if (!arrType) {
            arrType = fieldType;
        } else {
            arrType = typesCompatible(ctx, arrType, fieldType);

            if (!arrType) {
                ZToken *tok = fieldType ? fieldType->tok : NULL;
                error(ctx->state, tok,
                             "Array literals should have the same type");
            }
        }
    }

    ZType *result       = makeTypeThread(ctx, Z_TYPE_ARRAY);
    result->array.base  = arrType;
    result->array.size  = len;
    result->tok         = curr->tok;

    return result;
}

static ZType *resolveTupleLiteral(ZThreadSem *ctx, ZNode *node, ZType *inferred) {
    ZType **types = NULL;
    ZType *fieldType = NULL;


    ZNode **fields = node->tuplelit;
    for (usize i = 0; i < veclen(fields); i++) {
        ZType *parent = inferred && inferred->kind == Z_TYPE_TUPLE ?
            inferred->tuple[i] : inferred;

        fieldType = resolveType(ctx, fields[i], parent);
        if (!fieldType) {
            error(ctx->state, fields[i]->tok,
                    "Unresolved type of tuple");
        } else {
            vecpush(types, fieldType);
        }
    }

    ZType *result   = makeTypeThread(ctx, Z_TYPE_TUPLE);
    result->tuple   = types;
    result->tok     = node->tok;
    return result;
}

static ZType *resolveArrayInit(ZThreadSem *ctx, ZNode *node, ZType *inferred) {
    (void)inferred;
    node->arrayinit = resolveTypeRef(ctx, node->arrayinit);
    node->resolved = node->arrayinit;
    return node->arrayinit;
}

static ZType *resolveUnary(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType     *operand = resolveType(ctx, curr->unary.operand, inferred);
    ZTokenType op      = curr->unary.operat->type;

    if (!operand) {
        error(ctx->state, curr->tok, "Unresolved type");
        return NULL;
    }

    switch (op) {
    case TOK_REF: {/* &expr => *T */
        ZType *ptr  = makeTypeThread(ctx, Z_TYPE_POINTER);
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

    case TOK_ESCL:
        if (operand->kind != Z_TYPE_OPTIONAL &&
            operand->kind != Z_TYPE_RESULT) {
            error(ctx->state, curr->unary.operat,
                "'!' can be used only with optional and result types, got '%s'",
                stype(operand)
            );
            return NULL;
        }
        if (operand->kind == Z_TYPE_OPTIONAL) return operand->optional;
        if (operand->kind == Z_TYPE_RESULT) return operand->result.success;

        return NULL;
    default: return operand;
    }
}

static ZSymbol *resolveModuleChain(ZThreadSem *ctx, ZToken **chain, usize *i) {
    ZScope *scope   = ctx->current;
    usize tmp = 0;
    if (!i) i = &tmp;

    ZSymbol *previous   = NULL;
    ZSymbol *base       = NULL;
    for (*i = 0; *i < veclen(chain); (*i)++) {
        previous = base;
        base = resolveByScope(scope, chain[*i]);

        if (!base) {
            error(ctx->state, chain[*i],
                "Symbol '%s' not found in the current scope",
                stoken(chain[*i])
            );
            return NULL;
        }

        if (base->kind != Z_SYM_IMPORT) return base;

        scope = base->scope;
    }
    return previous;
}

static ZType *resolveEnumLit(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    if (!curr || curr->type != NODE_MEMBER) return NULL;

    ZNode **fields  = NULL;
    ZType *base     = resolveType(ctx, curr->memberAccess.object, inferred);
    ZToken *prop    = curr->memberAccess.field;


    if (!base || base->kind != Z_TYPE_ENUM) return NULL;
    fields = base->enm.fields;

    ZType *strct    = NULL;
    for (usize i = 0; i < veclen(fields) && !strct; i++) {
        if (tokeneq(fields[i]->resolved->strct.name, prop)) {
            strct = fields[i]->resolved;
        }
    }

    if (!strct) {
        error(ctx->state, prop,
            "Field '%s' not found for enum '%s'",
            stoken(prop), stoken(base->tok)
        );
        return NULL;
    }

    /* Skip the first argument (always the flag). */
    for (usize i = 1; i < veclen(strct->strct.fields); i++) {
        strct->strct.fields[i]->field.type  = resolveTypeRef(
            ctx, strct->strct.fields[i]->field.type);
        strct->strct.fields[i]->resolved    = strct->strct.fields[i]->field.type;
    }

    return base;
}

static ZType *resolveSlice(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *res      = resolveType(ctx, curr->slice.base, inferred);
    ZType *start    = resolveType(ctx, curr->slice.start, inferred);
    ZType *end      = resolveType(ctx, curr->slice.end, inferred);

    if (curr->slice.start &&
            (!start                                 ||
            !isPrimitive(start)                     ||
            !isInteger(start->primitive.token))
        ) {
        error(ctx->state, curr->slice.start->tok, "Must be an integer");
    }
    if (curr->slice.end &&
            (!end                                   ||
            !isPrimitive(end)                       ||
            !isInteger(end->primitive.token))
        ) {
        error(ctx->state, curr->slice.end->tok, "Must be an integer");
    }

    curr->slice.start   = implicitCast(ctx, curr->slice.start, u64Type);
    curr->slice.end     = implicitCast(ctx, curr->slice.end, u64Type);

    return res;
}

static ZType *resolveIf(ZThreadSem *ctx, ZNode *node, ZType *inferred) {
    if (!node || !node->ifStmt.cond || !node->ifStmt.body || !node->ifStmt.elseBranch) {
        error(ctx->state, node->tok, "Error");
        return NULL;
    }
    ZType *cond         = resolveType(ctx, node->ifStmt.cond, inferred);
    ZType *trueBranch   = resolveType(ctx, node->ifStmt.body, inferred);
    ZType *falseBranch  = resolveType(ctx, node->ifStmt.elseBranch, inferred);

    if (!isComparable(ctx, cond)) {
        error(ctx->state, node->ifStmt.cond->tok,
            "is not a comparable value");
        return NULL;
    }

    if (!trueBranch || !falseBranch) return NULL;

    if (typesEqual(trueBranch, falseBranch)) return trueBranch;

    ZType *sum = makeTypeThread(ctx, Z_TYPE_SUM);
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

static ZType *resolveBlock(ZThreadSem *ctx, ZNode *block, ZType *inferred) {
    ZType *breakType = NULL;
    for (usize i = 0; i < veclen(block->block); i++) {
        ZNode *stmt = block->block[i];
        if (stmt->type == NODE_BREAK || stmt->type == NODE_IF) {
            breakType = resolveType(ctx, stmt, inferred);
        } else {
            analyzeStmt(ctx, stmt);
        }
    }

    if (!breakType) {
        error(ctx->state, block->tok, "Missing break statement");
    }
    return breakType;
}

static ZType *resolveAnonFunc(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    if (inferred) {
        inferred = resolveTypeRef(ctx, inferred);
        if (!inferred) {
            error(ctx->state, curr->tok, "Return type can't be inferred from the context");
            return NULL;
        }

        if (inferred->kind != Z_TYPE_FUNCTION) {
            error(ctx->state, curr->tok, "Expected a function here");
            return NULL;
        }

        curr->funcDef.ret = inferred->func.ret;
        curr->resolved = inferred;

        for (usize i = 0; i < veclen(curr->funcDef.args); i++) {
            curr->funcDef.args[i]->resolved = inferred->func.args[i];
        }
    } else {
        curr->resolved->func.ret = resolveTypeRef(ctx, curr->resolved->func.ret);
    }

    ZScope *saved = ctx->current;
    ctx->current = ctx->current->parent;

    char loc[32];
    snprintf(loc, sizeof(loc), "%zu_%zu", curr->tok->row, curr->tok->col);

    curr->funcDef.mangled = manglerA((char *[]){
        curr->tok->filename,
        ctx->currentFunc->funcDef.mangled,
        loc,
        NULL
    });

    beginScope(ctx, curr);
    ZNode *oldFunc = ctx->currentFunc;
    ZType *oldFuncRet = ctx->currentFuncRet;

    ctx->currentFunc = curr;
    ctx->currentFuncRet = curr->resolved->func.ret;

    analyzeFuncArgs(ctx,
        curr->resolved->func.capabilities,
        curr->funcDef.capabilities,
        inferred ? inferred->func.capabilities : NULL
    );

    for (usize i = 0; i < veclen(curr->funcDef.capabilities); i++)
        putCapability(ctx, curr->funcDef.capabilities[i]);

    analyzeFuncArgs(ctx,
        curr->resolved->func.args,
        curr->funcDef.args,
        inferred ? inferred->func.args : NULL
    );
    analyzeBlock(ctx, curr->funcDef.body, false);
    endScope(ctx);

    ctx->current            = saved;
    ctx->currentFunc        = oldFunc;
    ctx->currentFuncRet     = oldFuncRet;

    if (curr->resolved && !curr->resolved->hash)
        curr->resolved->hash = hash(curr->resolved);

    return curr->resolved;
}

static ZType *resolveUnwrap(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *base = resolveType(ctx, curr->unwrap.base, inferred);
    if (!base) return NULL;

    bool isOptional = base->kind == Z_TYPE_OPTIONAL;
    bool isResult   = base->kind == Z_TYPE_RESULT;

    if (base->kind != Z_TYPE_OPTIONAL   &&
        base->kind != Z_TYPE_RESULT     &&
        base->kind != Z_TYPE_NONE       ) {
        error(ctx->state, curr->tok,
            "Invalid unwrap expression, "
            "expected an optional or result type, got '%s'",
            stype(base)
        );
        return NULL;
    }

    ZType *success = isOptional ?
        base->optional :
        base->result.success;

    switch (curr->unwrap.kind) {
    case UNWRAP_DO: {
        ZType *orelse = resolveType(ctx, curr->unwrap.orExpr, inferred);
        ZType *promoted = typesCompatible(ctx, orelse, success);
        if (!promoted) {
            error(ctx->state, curr->unwrap.orExpr->tok,
                "Expected '%s', got '%s'", stype(success), stype(orelse));
            return NULL;
        }
        curr->unwrap.orExpr = implicitCast(ctx, curr->unwrap.orExpr, success);
        return success;
        break;
    }

    case UNWRAP_RETURN:
        analyzeStmt(ctx, curr->unwrap.orExpr);
        if (ctx->currentFuncRet->kind == Z_TYPE_RESULT && isOptional) {
            error(ctx->state, curr->tok, "Cannot convert an optional type to a result");
            return success;
        } else if (ctx->currentFuncRet->kind == Z_TYPE_RESULT && isResult) {
            if (!typesCompatible(ctx,
                    base->result.error,
                    ctx->currentFuncRet->result.error)) {
                error(ctx->state, curr->tok,
                    "Expected an error type '%s', got '%s'",
                    stype(ctx->currentFuncRet->result.error),
                    stype(base->result.error)
                );
            }
        }
        return success;

    case UNWRAP_BREAK:
    case UNWRAP_CONTINUE:
        analyzeStmt(ctx, curr->unwrap.orExpr);
        if (ctx->loopDepth == 0) {
            error(ctx->state, curr->tok, "Must be inside a loop");
        }
        return success;
    default:            return success;
    }
}

/*
 * Resolve the type of any expression node and cache the result in node->resolved.
 * Returns the resolved ZType* or NULL on error.
 */
static ZType *resolveType(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    if (!curr)           return NULL;
    /* NODE_FUNC always carries a pre-built shape (arg/ret types) set by the
     * parser, so the generic "already resolved" cache check below would skip
     * resolveAnonFunc entirely and leave the mangled name/body unanalyzed. */
    if (curr->type == NODE_FUNC) return resolveAnonFunc(ctx, curr, inferred);
    if (curr->resolved)  return curr->resolved;

    ZType *result = NULL;

    switch (curr->type) {
    case NODE_BLOCK:        result = resolveBlock       (ctx, curr, inferred);      break;
    case NODE_CALL:         result = resolveFuncCall    (ctx, curr, inferred);      break;
    case NODE_UNARY:        result = resolveUnary       (ctx, curr, inferred);      break;
    case NODE_BINARY:       result = resolveBinary      (ctx, curr, inferred);      break;
    case NODE_MEMBER:       result = resolveMemberAccess(ctx, curr, inferred);      break;
    case NODE_LITERAL:      result = resolveLiteralType (ctx, curr->literalTok);    break;
    case NODE_ARRAY_LIT:    result = resolveArrayLiteral(ctx, curr, inferred);      break;
    case NODE_SUBSCRIPT:    result = resolveArrSubscript(ctx, curr, inferred);      break;
    case NODE_ARRAY_INIT:   result = resolveArrayInit   (ctx, curr, inferred);      break;
    case NODE_IDENTIFIER:   result = resolveIdent       (ctx, curr, inferred);      break;
    case NODE_STRUCT_LIT:   result = resolveStructLit   (ctx, curr, inferred);      break;
    case NODE_TUPLE_LIT:    result = resolveTupleLiteral(ctx, curr, inferred);      break;
    case NODE_SLICE:        result = resolveSlice       (ctx, curr, inferred);      break;
    case NODE_IF:           result = resolveIf          (ctx, curr, inferred);      break;
    case NODE_UNWRAP:       result = resolveUnwrap      (ctx, curr, inferred);      break;
    case NODE_RANGE: {
        ZType *left     = resolveType(ctx, curr->binary.left, inferred);
        ZType *right    = resolveType(ctx, curr->binary.right, inferred);

        if (!isPrimitive(left) && !isInteger(left->primitive.token)) {
            error(ctx->state, curr->binary.left->tok, "operand of a range expression must be an integer");
        }

        if (!isPrimitive(right) && !isInteger(right->primitive.token)) {
            error(ctx->state, curr->binary.right->tok, "operand of a range expression must be an integer");
        }

        ZType *promoted = typesCompatible(ctx, left, right);
        if (!promoted) {
            error(ctx->state, curr->tok,
              "'%s' and '%s' are not compatible", stype(left), stype(right)
            );
        } else {
            result = promoted;
            curr->binary.left = implicitCast(ctx, curr->binary.left, promoted);
            curr->binary.right = implicitCast(ctx, curr->binary.right, promoted);
        }
        break;
    }
    case NODE_VAR_DECL:
        /* Used when a var-decl appears as a sub-expression (unusual but safe). */
        if (curr->resolved) {
            result = resolveTypeRef(ctx, curr->resolved);
        } else if (curr->varDecl.rvalue) {
            result = resolveType(ctx, curr->varDecl.rvalue, inferred);
        }
        putVarPattern(ctx, curr, result, curr->varDecl.pattern, false);
        break;

    case NODE_CAST: {
        /* Resolve the inner expression type (for side-effects / validation). */
        ZType *expr = resolveType(ctx, curr->castExpr.expr, inferred);
        result = resolveTypeRef(ctx, curr->castExpr.toType);

        if (expr && expr->kind == Z_TYPE_ARRAY &&
            result->kind == Z_TYPE_ARRAY) {
            // result->array.size = expr->array.size;
        }

        if (!typesCompatible(ctx, result, expr)) {
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
        result = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
        result->primitive.token = makeTokenThread(ctx, TOK_U64, "u64");
        result->tok             = curr->tok;
        break;
    }

    case NODE_BREAK:
        if (curr->breakStmt.expr) {
            result = resolveType(ctx, curr->breakStmt.expr, inferred);
        } else if (ctx->loopDepth == 0) {
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

    curr->resolved  = result;
    if (result && !result->hash) result->hash    = hashType(result);
    return result;
}

static inline ZSymbol *nodeSymbol(ZNode *node) {
    if (!node) return NULL;
    switch (node->type) {
    case NODE_IDENTIFIER:   return node->identNode.sym;
    case NODE_MEMBER:       return node->memberAccess.sym;
    default:                return NULL;
    }
}

/* Member access where the object names a type (struct/enum/typedef alias):
 * resolves static functions and enum variants, never instance fields. */
static ZType *resolveTypeStatic(
    ZThreadSem *ctx, ZNode *curr, ZType *type, ZToken *field) {

    type = resolveTypeRef(ctx, type);
    if (!type) {
        error(ctx->state, curr->tok, "Base type not found");
        return NULL;
    }

    ZNode *func = resolveStaticFuncTable(ctx, type, field);
    if (func) {
        curr->memberAccess.func = func;
        return func->resolved;
    }

    if (type->kind == Z_TYPE_ENUM) {
        ZNode **variants = type->enm.fields;
        for (usize i = 0; i < veclen(variants); i++) {
            if (tokeneq(variants[i]->resolved->strct.name, field)) {
                curr->type = NODE_ENUM_LIT_NO_PAYLOAD;
                return type;
            }
        }
        error(ctx->state, field,
            "Variant '%s' not found for enum '%s'",
            stoken(field), stype(type));
        return NULL;
    }

    error(ctx->state, field,
        "Static function '%s' not found for '%s'",
        stoken(field), stype(type));
    return NULL;
}

static ZType *resolveMemberAccess(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *objType  = NULL;
    ZToken *field   = curr->memberAccess.field;
    if (curr->memberAccess.object->type == NODE_IDENTIFIER &&
        strcmp(curr->memberAccess.object->tok->str, ".") == 0) {
        if (!inferred) {
            error(ctx->state, curr->tok, "Member field can't be inferred");
            return NULL;
        }

        inferred = resolveTypeRef(ctx, inferred);
        curr->memberAccess.object->resolved = inferred;
        objType = inferred;

        if (objType &&
            (objType->kind == Z_TYPE_ENUM || objType->kind == Z_TYPE_STRUCT)) {
            return resolveTypeStatic(ctx, curr, objType, field);
        }
    } else {
        objType = resolveType(ctx, curr->memberAccess.object, inferred);
    }

    ZSymbol *objSym = nodeSymbol(curr->memberAccess.object);
    if (objSym) {
        if (!curr->memberAccess.object->resolved)
            curr->memberAccess.object->resolved = modType;
        switch (objSym->kind) {
        case Z_SYM_IMPORT: {
            ZSymbol *sym = resolveByScope(objSym->scope, field);
            if (!sym) {
                error(ctx->state, field,
                    "'%s' not found in module '%s'",
                    stoken(field), stoken(objSym->name));
                return NULL;
            }
            curr->memberAccess.sym = sym;
            return resolveTypeRef(ctx, sym->type);
        }
        case Z_SYM_STRUCT:
        case Z_SYM_ENUM:
        case Z_SYM_TYPEDEF:
            return resolveTypeStatic(ctx, curr, objSym->type, field);
        case Z_SYM_FOREIGN: {
            ZNode **funcs = objSym->node->block;
            for (usize i = 0; i < veclen(funcs); i++) {
                if (tokeneq(funcs[i]->foreignDecl.name, field)) {
                    curr->memberAccess.func = funcs[i];
                    return funcs[i]->resolved;
                }
            }
            error(ctx->state, field,
                "'%s' not found in namespace '%s'",
                stoken(field), stoken(objSym->name));
            return NULL;
        }
        default: break;
        }
    }

    if (!objType) {
        error(ctx->state, curr->tok,
              "Cannot resolve object type in member access");
        return NULL;
    }

    ZType *base = derefType(objType);

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
            return NULL;
        }

        return base->tuple[field->integer];
    } else if (base->kind == Z_TYPE_ARRAY && strcmp(field->str, "len") == 0) {
        return u64Type;
    } else if (base->kind == Z_TYPE_ARRAY && strcmp(field->str, "ptr") == 0) {
        ZType *pointer = makeTypeThread(ctx, Z_TYPE_POINTER);
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
            return NULL;
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
                ZType *pointer = makeTypeThread(ctx, Z_TYPE_POINTER);
                pointer->base = objType->array.base;
                return pointer;
            }
            error(ctx->state, curr->tok,
                "Expected a struct or tuple for '.' access, got none"
            );
            return NULL;
        }
        return resolved->resolved;
    }
}

static ZType *resolveArrSubscript(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *arrType      = resolveType(ctx, curr->subscript.arr, NULL);
    ZType *indexType    = resolveType(ctx, curr->subscript.index, inferred);

    /* Type not resolved */
    if (!arrType) return NULL;


    if (arrType->kind != Z_TYPE_ARRAY &&
        arrType->kind != Z_TYPE_POINTER) {
        error(ctx->state, curr->tok,
              "Expected an array type for subscript");
        return NULL;
    }

    if (!indexType || indexType->kind != Z_TYPE_PRIMITIVE ||
        !isInteger(indexType->primitive.token)) {
        error(ctx->state, curr->tok,
              "Array index must be an integer");
        return NULL;
    }

    if (arrType->kind == Z_TYPE_ARRAY) {
        return arrType->array.base;
    }
    return arrType->base;
}

static bool satisfyFacet(ZThreadSem *ctx, ZType *type, ZType *facet) {
    if (!type) return false;
    if (type->kind != Z_TYPE_POINTER) {
        error(ctx->state, type->tok,
            "Facets must be implemented only for pointer types"
        );
    }
    ZFuncTable *table = resolveFuncTable(ctx, type);

    if (!table) return false;

    usize len = veclen(facet->facet.funcs);

    for (usize i = 0; i < len; i++) {
        ZNode *func = facet->facet.funcs[i];
        if (!hashset_has(table->seenReceiverFuncs, func->field.identifier->str)) {
            return false;
        }
    }

    vecpush(table->facets, facet);
    vecpush(facet->facet.satisfied, type);

    return true;
}

/* ================== Statement analysis ================== */

static void analyzeVar(ZThreadSem *ctx, ZNode *curr, bool isGlobal) {
    (void)isGlobal;
    ZType *rvalueType   = NULL;
    ZType *declaredType = NULL;

    if (curr->varDecl.rvalue) {
        rvalueType = resolveType(ctx, curr->varDecl.rvalue, curr->resolved);
        checkFunctionUsedAsValue(ctx, curr->varDecl.rvalue);
        rvalueType = resolveTypeRef(ctx, rvalueType);
    }


    if (curr->resolved) {
        declaredType = resolveTypeRef(ctx, curr->resolved);
        ZType *promoted = typesCompatible(ctx, declaredType, rvalueType);
        if (rvalueType &&
            !promoted) {

            if (declaredType->kind == Z_TYPE_FACET) {
                if (!satisfyFacet(ctx, rvalueType, declaredType)) {
                    error(ctx->state, curr->tok,  "Facet not satisfied\n");
                }
            } else {
                error(ctx->state, curr->tok,
                    "Type mismatch: lvalue has type '%s' an rvalue has type '%s'",
                    stype(declaredType),
                    stype(rvalueType)
                );
            }
        }
    } else {
        /* Inferred type (:= syntax) */
        declaredType        = rvalueType;
        curr->resolved      = rvalueType;
    }

    curr->resolved = declaredType;

    if (curr->varDecl.rvalue && rvalueType && declaredType &&
            declaredType->kind == Z_TYPE_ARRAY &&
            declaredType->array.size > 0 &&
            rvalueType->kind == Z_TYPE_ARRAY) {
        if (rvalueType->array.size > declaredType->array.size) {
            error(ctx->state, curr->varDecl.rvalue->tok,
                "'%s' is larger than '%s'",
                stype(rvalueType), stype(declaredType)
            );
        }
        rvalueType->array.size = declaredType->array.size;
    } else if (curr->varDecl.rvalue && rvalueType && declaredType &&
            declaredType->kind == Z_TYPE_SUM) {
        curr->varDecl.rvalue = coerceToSum(ctx, curr->varDecl.rvalue, declaredType);
    } else if (curr->varDecl.rvalue && rvalueType &&
            !typesEqual(declaredType, rvalueType)) {
        curr->varDecl.rvalue = implicitCast(ctx, curr->varDecl.rvalue, curr->resolved);
    }

    putVarPattern(
        ctx,
        curr,
        curr->resolved,
        curr->varDecl.pattern,
        false
    );
}

static void analyzeIf(ZThreadSem *ctx, ZNode *curr) {
    bool isIfLet = curr->ifStmt.cond->type == NODE_VAR_DECL;
    if (isIfLet) beginScope(ctx, curr);
    ZType *cond = resolveType(ctx, curr->ifStmt.cond, NULL);

    if (!cond) return;

    if (!isComparable(ctx, cond)) {
        error(ctx->state,
            curr->ifStmt.cond->tok,
            "%s cannot be used as a condition",
            stype(cond)
        );
    }

    if (curr->ifStmt.cond->type != NODE_VAR_DECL) {
        curr->ifStmt.cond = implicitCast(ctx, curr->ifStmt.cond, u1Type);
    }

    analyzeStmt(ctx, curr->ifStmt.body);
    if (isIfLet) endScope(ctx);

    if (curr->ifStmt.elseBranch) {
        ZNode *el = curr->ifStmt.elseBranch;
        if (el->type == NODE_IF)
            analyzeIf(ctx, el);
        else
            analyzeStmt(ctx, el);
    }
}

static void analyzeWhile(ZThreadSem *ctx, ZNode *curr) {
    bool isForLet = curr->whileStmt.cond->type == NODE_VAR_DECL;
    if (isForLet) beginScope(ctx, curr);
    ZType *cond = resolveType(ctx, curr->whileStmt.cond, NULL);

    if (!isForLet) {
        if (!isComparable(ctx, cond)) {
            error(ctx->state, curr->whileStmt.cond->tok,
                    "Is not a comparable value");
        }

        curr->whileStmt.cond = implicitCast(ctx, curr->whileStmt.cond, u1Type);
    }

    ctx->loopDepth++;
    analyzeBlock(ctx, curr->whileStmt.branch, isForLet);
    ctx->loopDepth--;
    if (isForLet) endScope(ctx);
}

static void analyzeForeign(ZThreadSem *ctx, ZNode *curr) {
    beginScope(ctx, curr);
    ZType **generics = curr->resolved->func.generics;
    for (usize i = 0; i < veclen(generics); i++) {
        putGeneric(ctx, generics[i]);
    }
    curr->resolved = resolveTypeRef(ctx, curr->resolved);
    endScope(ctx);
}

static void analyzeFuncArgs(ZThreadSem *ctx, ZType **types, ZNode **fields, ZType **inferred) {
    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *field        = fields[i];
        ZType *fieldType    = resolveTypeRef(ctx, field->field.type);

        if (!fieldType) {
            if (inferred) fieldType = inferred[i];
            else {
                error(ctx->state, field->field.identifier, "Unknown type resolved");
                continue;
            }
        }
        if (inferred) {
            inferred[i] = resolveTypeRef(ctx, inferred[i]);
            if (!typesEqual(inferred[i], fieldType)) {
                error(ctx->state, field->tok,
                    "Expected '%s', got '%s'",
                    stype(inferred[i]), stype(fieldType)
                );
            }
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

static bool satisfyReturn(ZThreadSem *ctx, ZNode *node) {
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

static void analyzeFunc(ZThreadSem *ctx, ZNode *curr) {
    for (usize i = 0; i < veclen(curr->funcDef.generics); i++) {
        putGeneric(ctx, curr->funcDef.generics[i]);
    }

    beginScope(ctx, curr);
    curr->funcDef.body->scope = ctx->current;
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

        ZSymbol *sym = makesymbol(ctx->arena, Z_SYM_VAR);
        sym->name       = receiver->field.identifier;
        sym->type       = recType;
        sym->node       = curr->funcDef.receiver;
        sym->isPublic   = false;
        sym->useCount   = 1;
        putSymbol(ctx, sym);
    }

    analyzeFuncArgs(ctx,
        curr->resolved->func.args,
        curr->funcDef.args,
        NULL
    );

    analyzeFuncArgs(ctx,
        curr->resolved->func.capabilities,
        curr->funcDef.capabilities,
        NULL
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

static void analyzeReturn(ZThreadSem *ctx, ZNode *curr) {
    ZType *retType  = NULL;
    ZType *promoted = NULL;

    if (curr->returnStmt.expr) {
        retType     = resolveType(ctx, curr->returnStmt.expr, ctx->currentFuncRet);
        if (!retType) {
            error(ctx->state, curr->tok, "Return type not resolved");
            return;
        }
        retType     = resolveTypeRef(ctx, retType);
    }

    curr->resolved  = retType;

    if (!ctx->currentFuncRet) return;

    bool sumAcceptVoid =
        ctx->currentFuncRet->kind == Z_TYPE_SUM &&
        sumTypeIndexOf(ctx->currentFuncRet, u0Type) != -1;

    bool isVoidRet  = isVoid(retType);
    bool isVoid = isType(ctx->currentFuncRet, TOK_VOID);

    if (isVoid && !isVoidRet) {
        error(ctx->state, ctx->currentFunc->tok,
              "Unexpected return value in void function '%s'",
              stype(retType));
    } else if (!isVoid && !sumAcceptVoid && isVoidRet) {
        error(ctx->state, ctx->currentFunc->tok,
              "Expected a return value of type '%s', got u0", ctx->currentFuncRet);
    } else if (!isVoid && !sumAcceptVoid && !isVoidRet) {
        promoted = typesCompatible(
            ctx, retType, ctx->currentFuncRet
        );

        if (ctx->currentFuncRet->kind == Z_TYPE_OPTIONAL) {
            curr->returnStmt.expr = implicitCast(ctx,
                curr->returnStmt.expr,
                ctx->currentFuncRet
            );
            curr->resolved = ctx->currentFuncRet;
            if (retType->kind == Z_TYPE_NONE ||
                typesCompatible(ctx, retType, ctx->currentFuncRet->optional))
                return;
        }
        if (!promoted) {
            error(ctx->state, curr->tok,
                "Expected type %s, got %s",
                stype(ctx->currentFuncRet),
                stype(retType)
            );
            return;
        } else if (ctx->currentFuncRet->kind == Z_TYPE_SUM) {
            /* Thread the sum type into nested inline-ifs so leaves are tagged
             * in the outer variant order. */
            curr->returnStmt.expr = coerceToSum(ctx,
                curr->returnStmt.expr, ctx->currentFuncRet
            );
            curr->resolved = ctx->currentFuncRet;
        }
        else {
            /* Implicit casting. */
            curr->returnStmt.expr = implicitCast(ctx,
                curr->returnStmt.expr, ctx->currentFuncRet
            );
        }
        curr->resolved = curr->returnStmt.expr->resolved;
    } else if (sumAcceptVoid && isVoidRet) {
        curr->resolved = u0Type;
    }
}

static void analyzeCapability(ZThreadSem *ctx, ZNode *curr) {
    beginScope(ctx, curr);
    ZNode **capabilities = curr->capability.capabilities;
    for (usize i = 0; i < veclen(capabilities); i++) {
        analyzeVar(ctx, capabilities[i], false);
        putCapability(ctx, capabilities[i]);
    }
    analyzeStmt(ctx, curr->capability.block);
    endScope(ctx);
}

static bool patternEq(ZState *state, ZVarDestructPattern *a, ZVarDestructPattern *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    if (a->type != b->type) return false;

    switch (a->type) {
    case Z_VAR_IDENT: return true;
    case Z_VAR_LIT: {
        ZToken *ta = a->ident;
        ZToken *tb = b->ident;
        if (!ta || !tb) return ta == tb;
        if (ta->type != tb->type) return false;
        if (ta->type == TOK_INT_LIT || ta->type == TOK_RUNE_LIT)
            return ta->integer == tb->integer;
        if (ta->type == TOK_FLOAT_LIT)
            return ta->floating == tb->floating;
        if (!ta->str || !tb->str) return ta->str == tb->str;
        return strcmp(ta->str, tb->str) == 0;
    }
    case Z_VAR_PAIR:
        return tokeneq(a->key, b->key) &&
               patternEq(state, a->value, b->value);
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
            for (usize j = 0; j < bLen && index == -1; j++)
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
    default: return false;
    }
}

static void analyzeMatchStmt(ZThreadSem *ctx, ZNode *curr) {
    ZType *condType = resolveType(ctx, curr->match.cond, NULL);
    condType = resolveTypeRef(ctx, condType);
    if (!condType) return;

    ZNode **arms = curr->match.arms;
    for (usize i = 0; i < veclen(arms); i++) {
        ZNode *arm = arms[i];

        if (!arm->matchArm.expr) {
            error(ctx->state, arm->tok, "Invalid match arm");
            continue;
        }

        for (usize j = 0; j < i; j++) {
            if (patternEq(ctx->state,
                arms[i]->matchArm.pattern,
                arms[j]->matchArm.pattern)) {
                error(ctx->state,
                    arms[i]->matchArm.pattern->tok,
                    "%zuth and %zuth arms are equal", j, i
                );
            }
        }

        beginScope(ctx, arm);
        putVarPattern(ctx, arm, condType, arm->matchArm.pattern, true);
        if (arm->matchArm.expr->type == NODE_BLOCK) {
            analyzeBlock(ctx, arm->matchArm.expr, false);
        } else {
            resolveType(ctx, arm->matchArm.expr, NULL);
        }
        endScope(ctx);
    }
}

static void analyzeForIn(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *iter = resolveType(ctx, curr->forin.iter, inferred);
    ZVarDestructPattern *binding = curr->forin.binding;

    ZType *itemType = NULL;
    ZType *elemType = iter;

    bool isInt = isPrimitive(iter) && isInteger(iter->primitive.token);
    bool isArr = iter->kind == Z_TYPE_ARRAY;

    if (curr->forin.iter->type != NODE_RANGE && !isInt && !isArr) {
        ZType *iterPtr = makeTypeThread(ctx, Z_TYPE_POINTER);
        iterPtr->tok = iter->tok;
        iterPtr->base = iter;

        ZFuncTable *table = resolveFuncTable(ctx, iterPtr);
        if (!table || !hashset_has(table->seenReceiverFuncs, "next")) {
            error(ctx->state, curr->forin.iter->tok,
                "'%s' doesn't implement 'next' function", stype(iter)
            );
            return;
        }

        usize idx = 0;
        for (usize i = 0; i < veclen(table->funcDef); i++) {
            if (strcmp(table->funcDef[i]->funcDef.name->str, "next") == 0) {
                idx = i;
                break;
            }
        }
        ZNode *funcRef = table->funcDef[idx];
        if (veclen(funcRef->resolved->func.args) != 0) {
            error(ctx->state, funcRef->tok,
                "'next' function expects 0 arguments, got %zu",
                veclen(funcRef->resolved->func.args)
            );
        }
        itemType = funcRef->resolved->func.ret;

        if (itemType->kind != Z_TYPE_OPTIONAL) {
            error(ctx->state, funcRef->tok,
                "'next' function must return an optional type, got '%s'",
                stype(itemType)
            );
            return;
        }
        ZToken *tok             = curr->forin.iter->tok;
        ZNode *call             = makeNodeThread(ctx, NODE_CALL);
        ZNode *iterAddr         = makeNodeThread(ctx, NODE_UNARY);
        iterAddr->unary.operand = curr->forin.iter;
        iterAddr->unary.operat  = makeTokenThread(ctx, TOK_REF, tok->start);

        ZNode *iterCall                     = makeNodeThread(ctx, NODE_MEMBER);
        iterCall->memberAccess.object       = iterAddr;
        iterCall->memberAccess.field        = makeTokenThread(ctx, TOK_IDENT, tok->start);
        iterCall->memberAccess.field->str   = "next";
        iterCall->memberAccess.mangled      = funcRef->funcDef.mangled;

        call->call.callee       = iterCall;
        call->call.args         = NULL;
        call->call.func         = funcRef;
        call->call.capabilities = NULL;
        call->resolved          = funcRef->resolved->func.ret;

        curr->forin.iterNextRef = call;

        elemType = itemType->optional;
    } else if (isArr) {
        elemType = iter->array.base;
    }
    beginScope(ctx, curr->forin.body);

    ctx->loopDepth++;

    if (isArr && binding->type == Z_VAR_TUPLE) {
         if (veclen(binding->tuple) > 2)
            error(ctx->state, binding->tok, "Must be at least two elements");

        putVarPattern(ctx, curr, elemType, binding->tuple[0], false);
        if (veclen(binding->tuple) == 2) {
            putVarPattern(ctx, curr, u64Type, binding->tuple[1], false);
        }
    } else {
        putVarPattern(ctx, curr, elemType, curr->forin.binding, true);
    }

    analyzeBlock(ctx, curr->forin.body, false);
    ctx->loopDepth--;

    endScope(ctx);
}

static void analyzeStmt(ZThreadSem *ctx, ZNode *curr) {
    switch (curr->type) {
    case NODE_VAR_DECL:     analyzeVar(ctx, curr, false);           break;
    case NODE_IF:           analyzeIf(ctx, curr);                   break;
    case NODE_WHILE:        analyzeWhile(ctx, curr);                break;
    case NODE_BLOCK:        analyzeBlock(ctx, curr, true);          break;
    case NODE_DEFER:        analyzeStmt(ctx, curr->deferStmt.expr); break;
    case NODE_RETURN:       analyzeReturn(ctx, curr);               break;
    case NODE_MATCH:        analyzeMatchStmt(ctx, curr);            break;
    case NODE_CAPABILITY:   analyzeCapability(ctx, curr);           break;
    case NODE_FORIN:        analyzeForIn(ctx, curr, NULL);          break;

    /* Type declarations */
    case NODE_TYPEDEF:
        putTypedef(ctx, curr);
        analyzeTypedef(ctx, curr);
        break;
    case NODE_STRUCT:
        putStruct(ctx, curr);
        analyzeStruct(ctx, curr);
        break;
    case NODE_ENUM:
        putEnum(ctx, curr);
        analyzeEnum(ctx, curr);
        break;
    case NODE_NAMESPACE:
        putNamespace(ctx, curr);
        analyzeNamespace(ctx, curr);
        break;
    default:                resolveType     (ctx, curr, NULL);      break;
    }
}

static void analyzeBlock(ZThreadSem *ctx, ZNode *block, bool scoped) {
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

static void putImpl(ZThreadSem *ctx, ZNode *node) {
    hashset_t seen = NULL;
    char **facetNames = NULL;
    hashset_t funcs = NULL;
    usize funcLen = veclen(node->impl.funcs);
    for (usize i = 0; i < funcLen; i++) {
        ZNode *func = node->impl.funcs[i];
        putFunc(ctx, func);
        hashset_insert(&funcs, func->funcDef.name->str);
    }

    if (veclen(node->impl.facets) > 0 &&
        node->impl.base->kind != Z_TYPE_POINTER) {
        error(ctx->state, node->impl.base->tok,
            "Facets must be implemented only by pointers"
        );
    } else {
        for (usize i = 0; i < veclen(node->impl.facets); i++) {
            ZToken *facetRef = node->impl.facets[i]->tok;
            ZType *facet = resolveTypeRef(ctx, node->impl.facets[i]);

            if (!facet) continue;
            node->impl.facets[i] = facet;

            if (facet->kind != Z_TYPE_FACET) {
                error(ctx->state,
                    node->impl.facets[i]->tok,
                    "Expected a facet, got '%s'", stype(facet)
                );
                continue;
            }

            usize facetFuncs = veclen(facet->facet.funcs);
            for (usize j = 0; j < facetFuncs; j++) {
                ZNode *func = facet->facet.funcs[j];
                char *name  = func->field.identifier->str;
                if (!hashset_insert(&seen, name)) {
                    error(ctx->state, node->tok,
                        "'%s' conflicts with another facet",
                        name
                    );
                    continue;
                }

                if (!hashset_has(funcs, name)) {
                    error(ctx->state, facetRef,
                        "%s for type '%s' requires '%s' but is not implemented",
                        stype(facet), stype(node->impl.base), name
                    );
                }
                vecpush(facetNames, name);
            }
        }
    }
}

static void addImportedFunc(ZThreadSem *parent, ZNode *func) {
    if (func->funcDef.receiver) {
        ZFuncTable *dst = putOrInsertFuncTable(
            parent, func->funcDef.receiver->field.type);
        if (hashset_insert(&dst->seenReceiverFuncs, func->funcDef.name->str))
            vecpush(dst->funcDef, func);
    } else if (func->funcDef.base) {
        ZFuncTable *dst = putOrInsertFuncTable(parent, func->funcDef.base);
        if (hashset_insert(&dst->seenStaticFuncs, func->funcDef.name->str))
            vecpush(dst->staticFuncDef, func);
    }
}

static void mergeFuncTable(ZThreadSem *parent, ZThreadSem *child, bool pub) {
    for (usize i = 0; i < veclen(child->exportedFuncs); i++) {
        ZNode *func = child->exportedFuncs[i];
        addImportedFunc(parent, func);
        if (pub) vecpush(parent->exportedFuncs, func);
    }
}

static void discoverImport(
    ZThreadSem *parent, ZThreadSem *child, ZNode *node, bool pub) {

    ZScope *parentScope     = pub ? parent->global : parent->local;
    ZSymbol **childSymbols   = child->global->symbols;

    if (node->module.name) {
        ZSymbol *import = makeRawSymbol(
            parent->arena,      Z_SYM_IMPORT,
            node->module.name,  modType,
            node,               pub
        );
        import->scope = child->global;

        putSymbol(parent, import);
    } else {
        for (usize i = 0; i < veclen(childSymbols); i++) {
            vecpush(parentScope->symbols, childSymbols[i]);
        }
    }

    mergeFuncTable(parent, child, pub);
}

/* ================== Global scope discovery ================== */

static ZThreadSem *discoverGlobalScope(ZThreadSem *ctx, ZNode *root) {
    ctx                 = registerModule(ctx->semantic, root);
    ZScope *saved       = ctx->current;
    for (usize i = 0; i < veclen(root->module.root); i++) {
        ZNode *node = root->module.root[i];

        if (node->type) {
            analyzeAnnotations(ctx->state, node);
        }

        switch (node->type) {
        case NODE_FUNC:         putFunc     (ctx, node);       break;
        case NODE_STRUCT:       putStruct   (ctx, node);       break;
        case NODE_ENUM:         putEnum     (ctx, node);       break;
        case NODE_NAMESPACE:    putNamespace(ctx, node);       break;
        case NODE_TYPEDEF:      putTypedef  (ctx, node);       break;
        case NODE_FACET:        putFacet    (ctx, node);       break;
        case NODE_IMPL:         putImpl     (ctx, node);       break;

        case NODE_FOREIGN: {
            ZSymType kind = node->resolved->kind == Z_TYPE_FUNCTION ? Z_SYM_FUNC : Z_SYM_VAR;
            ZSymbol *symbol   = makesymbol(allocator.ctx, kind);
            symbol->name      = node->foreignDecl.name;
            symbol->node      = node;
            symbol->type      = node->resolved;
            symbol->isPublic  = node->foreignDecl.pub;
            putSymbol(ctx, symbol);

            break;
        }

        case NODE_MODULE: {
            ZThreadSem *import = getRegisteredModule(
                ctx->semantic, node->module.filename
            );

            if (!import) import = discoverGlobalScope(ctx, node);

            discoverImport(ctx, import, node, node->module.pub);
            break;
        }
        default: break;
        }
    }
    ctx->current = saved;
    return ctx;
}

static void _checkEmbedFieldConflicts(
        ZThreadSem *ctx, ZType *strct,
        hashset_t *fieldSeen, hashset_t *structSeen, ZToken *embedTok) {
    if (!strct || strct->kind != Z_TYPE_STRUCT) return;
    if (!hashset_insert(structSeen, strct->strct.name->str)) return;
    ZNode **fields = strct->strct.fields;
    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *field = fields[i];
        if (field->type == NODE_EMBED_FIELD) {
            ZType *nested = field->resolved;
            if (nested && nested->kind == Z_TYPE_STRUCT)
                _checkEmbedFieldConflicts(ctx, nested, structSeen, fieldSeen, embedTok);
        } else if (field->type == NODE_FIELD) {
            if (!hashset_insert(fieldSeen, field->field.identifier->str)) {
                error(ctx->state, embedTok,
                    "field '%s' conflicts with embedded struct '%s'",
                    field->field.identifier->str, stype(strct));
            }
        }
    }
}

static void checkEmbedFieldConflicts(ZThreadSem *ctx, ZType *strct, ZToken *embedTok) {
    hashset_t structSeen    = NULL;
    hashset_t fieldSeen     = NULL;
    _checkEmbedFieldConflicts(ctx, strct, &fieldSeen, &structSeen, embedTok);
}

static void analyzeStruct(ZThreadSem *ctx, ZNode *structDef) {
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
        ZNode *field = fields[i];
        if (!field || !field->resolved) continue;
        ZType **szSeen = NULL;

        if (isInfiniteSize(field->resolved, structType, &szSeen)) {
            error(ctx->state, field->tok,
                  "field '%s' embeds struct by value causing infinite size; use a pointer",
                  stoken(field->tok));
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
        checkEmbedFieldConflicts(ctx, embedded, field->tok);
    }
}

static void analyzeEnum(ZThreadSem *ctx, ZNode *enumDef) {
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
    ZNode **fields = enm->enm.fields;
    hashset_t seen = NULL;

    for (usize i = 0; i < veclen(fields); i++) {
        if (!hashset_insert(&seen, fields[i]->resolved->strct.name->str)) {
            error(ctx->state, fields[i]->resolved->strct.name,
                "This field already declared in the same enum");
        }
        ZNode **enumField = fields[i]->resolved->strct.fields;

        for (usize j = 0; j < veclen(enumField); j++) {
            if (!enumField[j] ||
                !enumField[j]->field.type) continue;
            ZType **szSeen = NULL;
            ZType *resolved = resolveTypeRef(ctx, enumField[j]->field.type);
            if (!resolved) continue;
            if (isInfiniteSize(resolved, enm, &szSeen)) {
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

static void analyzeNamespace(ZThreadSem *ctx, ZNode *node) {
    for (usize i = 0; i < veclen(node->block); i++) {
        ZNode *child = node->block[i];
        if (child->type == NODE_FOREIGN) {
            analyzeForeign(ctx, child);
        } else if (child->type == NODE_NAMESPACE) {
            analyzeNamespace(ctx, child);
        }
    }
}

static void analyzeTypedef(ZThreadSem *ctx, ZNode *node) {
    ZType *resolved = resolveTypeRef(ctx, node->typeDef.type);
    node->resolved = resolved;
}

static void analyzeFacet(ZThreadSem *ctx, ZNode *node) {
    ZNode **funcs = node->facet.funcs;
    for (usize i = 0; i < veclen(funcs); i++) {
        ZType *resolved = resolveTypeRef(
            ctx, funcs[i]->resolved
        );
        if (!resolved) continue;
        funcs[i]->resolved = resolved;
        funcs[i]->field.type = resolved;
    }
}

/* ================== Main analysis pass ================== */

static void analyze(ZThreadSem *ctx, ZNode *root) {
    /* Pre-pass: analyze global vars before functions so that any function
     * body referencing a global can resolve it regardless of source order. */
    for (usize i = 0; i < veclen(root->module.root); i++) {
        ZNode *child = root->module.root[i];
        if (child->type == NODE_VAR_DECL)
            analyzeVar(ctx, child, true);
    }

    for (usize i = 0; i < veclen(root->module.root); i++) {
        ZNode *child = root->module.root[i];

        switch (child->type) {
        /* These nodes don't require any validation. */
        case NODE_VAR_DECL:                                     break;
        case NODE_MACRO:                                        break;
        case NODE_MODULE:                                       break;

        case NODE_FOREIGN:      analyzeForeign  (ctx, child);   break;
        case NODE_FUNC:         analyzeFunc     (ctx, child);   break;
        case NODE_TYPEDEF:      analyzeTypedef  (ctx, child);   break;
        case NODE_STRUCT:       analyzeStruct   (ctx, child);   break;
        case NODE_ENUM:         analyzeEnum     (ctx, child);   break;
        case NODE_NAMESPACE:    analyzeNamespace(ctx, child);   break;
        case NODE_FACET:        analyzeFacet    (ctx, child);   break;

        case NODE_IMPL:
            for (usize i = 0; i < veclen(child->impl.funcs); i++) {
                analyzeFunc(ctx, child->impl.funcs[i]);
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

static inline void *worker(void *arg) {
    ZThreadSem *ctx = (ZThreadSem *)arg;
    analyze(ctx, ctx->root);
    return NULL;
}

ZSemantic *zanalyze(ZState *state, ZNode *root) {
    state->currentPhase = Z_PHASE_SEMANTIC;
    ZSemantic *ctx = makesemantic(state, root);

    ZScope *globalScope     = makescope(allocator.ctx, NULL, root);
    ZThreadSem *first       = makethreadsem(
        ctx, globalScope, root, allocator.ctx
    );
    discoverGlobalScope(first, root);

    usize len                   = veclen(ctx->scopes);
    pthread_t *threads          = znalloc(pthread_t, len);

    ZScopeTable **scopes        = ctx->scopes;
    ZThreadSem **semantics      = ctx->semantics;
    for (usize i = 0; i < len; i++) {
        scopes[i]->ctx = semantics[i];
        pthread_create(threads + i, NULL, worker, semantics[i]);
    }

    for (usize i = 0; i < len; i++) {
        pthread_join(threads[i], NULL);
        // if (i != 0) checkUnusedSymbols(scopes[i]->ctx);
    }

    // if (!ctx->main) {
    //     error(ctx->state, root->tok, "Missing 'main' declaration");
    // }

    return ctx;
}
