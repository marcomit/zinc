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

#include "zsem.h"

static void analyze                 (ZThreadSem *, ZNode *);
static void analyzeFacet            (ZThreadSem *, ZNode *);

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
            parent->allocator,  Z_SYM_IMPORT,
            node->module.name,  consts(child->state, modType),
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
        case NODE_VAR_DECL:
            putRawSymbol(
                ctx, Z_SYM_VAR, node->varDecl.pattern->ident,
                node->resolved, node, true
            );
            break;

        case NODE_FOREIGN: {
            ZSymType kind = node->resolved->kind == Z_TYPE_FUNCTION ? Z_SYM_FUNC : Z_SYM_VAR;
            ZSymbol *symbol   = makesymbol(ctx->allocator, kind);
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

static void resolveGlobalScope(ZSemantic *semantic) {
    for (usize i = 0; i < veclen(semantic->semantics); i++) {
        ZThreadSem *ctx = semantic->semantics[i];
        ZNode **nodes   = ctx->root->module.root;
        ZScope *saved   = ctx->current;

        for (usize j = 0; j < veclen(nodes); j++) {
            ZNode *node = nodes[j];
            if (node->type != NODE_FUNC) continue;
            node->resolved = resolveTypeRef(ctx, node->resolved);
        }
        ctx->current = saved;
    }
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
                zlog(ctx->state, embedTok,
                    Z3050,
                    field->field.identifier->str, stype(strct));
            }
        }
    }
}

static void checkEmbedFieldConflicts(ZThreadSem *ctx, ZType *strct, ZToken *embedTok) {
    hashset_t structSeen    = NULL;
    hashset_t fieldSeen     = NULL;
    _checkEmbedFieldConflicts(ctx, strct, &fieldSeen, &structSeen, embedTok);
    hashset_free(&structSeen);
    hashset_free(&fieldSeen);
}
void analyzeStruct(ZThreadSem *ctx, ZNode *structDef) {
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
            zlog(ctx->state, structDef->tok, Z3051);
        }
    }

    ZType *structType = structDef->resolved;

    for (usize i = 0; i < len; i++) {
        ZNode *field = fields[i];
        if (!field || !field->resolved) continue;
        ZType **szSeen = NULL;

        if (isInfiniteSize(field->resolved, structType, &szSeen)) {
            zlog(ctx->state, field->tok, Z3052, stoken(field->tok));
        }
    }

    hashset_t fieldSeen = NULL;
    for (usize i = 0; i < len; i++) {
        ZNode *field = fields[i];
        if (field->type == NODE_FIELD) {
            if (!hashset_insert(&fieldSeen, field->field.identifier->str)) {
                zlog(ctx->state, field->field.identifier,
                    Z3053, field->field.identifier->str);
            }
        }
    }
    hashset_free(&fieldSeen);
    for (usize i = 0; i < len; i++) {
        ZNode *field = fields[i];
        if (field->type != NODE_EMBED_FIELD) continue;
        ZType *embedded = field->resolved;
        if (!embedded || embedded->kind != Z_TYPE_STRUCT) continue;
        checkEmbedFieldConflicts(ctx, embedded, field->tok);
    }
}

void analyzeEnum(ZThreadSem *ctx, ZNode *enumDef) {
    if (!enumDef->resolved) {
        zlog(ctx->state, enumDef->tok, Z3054);
        return;
    }

    ZSymbol *sym = resolve(ctx, enumDef->enumDef.name);
    if (!sym) {
        zlog(ctx->state, enumDef->enumDef.name, Z3055);
        return;
    }

    if (sym->type->kind != Z_TYPE_ENUM) {
        zlog(ctx->state, enumDef->enumDef.name, Z3056);
        return;
    }

    ZType *enm = sym->type;
    ZNode **fields = enm->enm.fields;
    hashset_t seen = NULL;

    for (usize i = 0; i < veclen(fields); i++) {
        if (!hashset_insert(&seen, fields[i]->resolved->strct.name->str)) {
            zlog(ctx->state, fields[i]->resolved->strct.name, Z3057);
        }
        ZNode **enumField = fields[i]->resolved->strct.fields;

        for (usize j = 0; j < veclen(enumField); j++) {
            if (!enumField[j] ||
                !enumField[j]->field.type) continue;
            ZType **szSeen = NULL;
            ZType *resolved = resolveTypeRef(ctx, enumField[j]->field.type);
            if (!resolved) continue;
            if (isInfiniteSize(resolved, enm, &szSeen)) {
                zlog(ctx->state,
                    enumField[j]->field.type->tok,
                    Z3058,
                    enumField[j]->field.type->tok->str);
            } else {
                enumField[j]->field.type = resolved;
                enumField[j]->resolved = resolved;
            }
        }
    }
    hashset_free(&seen);
}

void analyzeNamespace(ZThreadSem *ctx, ZNode *node) {
    for (usize i = 0; i < veclen(node->block); i++) {
        ZNode *child = node->block[i];
        if (child->type == NODE_FOREIGN) {
            analyzeForeign(ctx, child);
        } else if (child->type == NODE_NAMESPACE) {
            analyzeNamespace(ctx, child);
        }
    }
}

void analyzeTypedef(ZThreadSem *ctx, ZNode *node) {
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
            zlog(ctx->state, root->tok, Z9009, root->type);
            break;
        }
    }
}

static inline void *worker(void *arg) {
    ZThreadSem *ctx = (ZThreadSem *)arg;
    useAllocator(ctx->allocator);
    analyze(ctx, ctx->root);
    return NULL;
}

ZSemantic *zanalyze(ZState *state, ZNode *root) {
    state->currentPhase = Z_PHASE_SEMANTIC;
    ZSemantic *ctx = makesemantic(state, root);

    ZScope *globalScope     = makescope(state->allocator, NULL, root);

    ZThreadSem *first       = makethreadsem(
        ctx, globalScope, root, state->allocator
    );
    discoverGlobalScope(first, root);
    resolveGlobalScope(ctx);

    usize len                   = veclen(ctx->scopes);
    pthread_t *threads          = znalloc(state->allocator, pthread_t, len);
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
    //     zlog(ctx->state, root->tok, Z3059);
    // }

    return ctx;
}
