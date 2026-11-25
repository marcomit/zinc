#include "zinc/zsem.h"
#include "zsem.h"

/* ================== Scope / Symbol helpers ================== */

ZNode *makeNodeThread(ZThreadSem *ctx, ZNodeType type) {
    ZNode *node = zalloc(ctx->allocator, ZNode);
    *node = (ZNode){ 0 };
    node->type = type;
    return node;
}

ZType *makeTypeThread(ZThreadSem *ctx, ZTypeKind kind) {
    ZType *type = zalloc(ctx->allocator, ZType);
    *type = (ZType){ 0 };
    type->kind = kind;
    return type;
}

ZToken *makeTokenThread(ZThreadSem *ctx, ZTokenType type, char *start) {
    ZToken *tok = zalloc(ctx->allocator, ZToken);
    *tok = (ZToken){ 0 };
    tok->type = type;
    tok->start = start;
    return tok;
}

ZScope *makescope(Allocator *allocator, ZScope *parent, ZNode *node) {
    ZScope *self        = zalloc(allocator, ZScope);
    self->depth         = parent ? parent->depth + 1 : 0;
    self->parent        = parent;
    self->node          = node;
    self->symbols       = NULL;
    self->seen          = NULL;
    self->capabilities  = NULL;


    return self;
}

ZThreadSem *makethreadsem(ZSemantic *ctx, ZScope *current, ZNode *root) {
    Allocator *a            = ctx->state->allocator;
    ZThreadSem *self        = zalloc(a, ZThreadSem);

    self->allocator         = a;
    self->currentFunc       = NULL;
    self->currentFuncRet    = NULL;
    self->loopDepth         = 0;
    self->semantic          = ctx;
    self->state             = ctx->state;
    self->global            = current;
    self->local             = makescope(a, current, root);
    self->current           = self->local;
    self->root              = root;
    self->funcs             = NULL;
    self->exportedFuncs     = NULL;

    return self;
}

ZSymbol *makesymbol(Allocator *allocator, ZSymType kind) {
    ZSymbol *self       = zalloc(allocator, ZSymbol);
    self->kind          = kind;
    self->useCount      = 0;
    self->reachable     = false;
    return self;
}

ZSemantic *makesemantic(ZState *state) {
    ZSemantic *self         = zalloc(state->allocator, ZSemantic);
    self->state             = state;
    self->scopes            = NULL;
    return self;
}


void putSymbol(ZThreadSem *ctx, ZSymbol *symbol) {
    ZScope *scope = symbol->isPublic ? ctx->global : ctx->current;

    if (!symbol || !symbol->name) {
        zlog(ctx->state, NULL, Z9001);
        return;
    }

    if (strcmp(symbol->name->str, "_") != 0 &&
        !hashset_insert(&scope->seen, symbol->name->str)) {
        /* Duplicate pub foreign declarations are valid: multiple modules
           may re-export the same C extern (like a shared header). Skip
           silently instead of raising an error. */
        if (symbol->isPublic && symbol->kind == Z_SYM_FUNC &&
                symbol->node && symbol->node->type == NODE_FOREIGN) {
            return;
        }
        zlog(ctx->state, symbol->name, Z3001, symbol->name->str);
    } else {
        vecpush(scope->symbols, symbol);
    }

}

ZSymbol *makeRawSymbol(
                        Allocator *allocator,
                        ZSymType kind,
                        ZToken *name,
                        ZType *type,
                        ZNode *node,
                        bool isPublic) {
    ZSymbol *symbol = makesymbol(allocator, kind);

    symbol->name        = name;
    symbol->type        = type;
    symbol->node        = node;
    symbol->isPublic    = isPublic;
    symbol->reachable   = strcmp(name->str, "_") == 0;

    return symbol;
}

void putRawSymbol(ZThreadSem *ctx,
                        ZSymType kind,
                        ZToken *name,
                        ZType *type,
                        ZNode *node,
                        bool isPublic) {
    putSymbol(ctx,
        makeRawSymbol(
            ctx->allocator,
            kind,
            name,
            type,
            node,
            isPublic
        ));
}

static ZFuncTable *makefunctable(Allocator *allocator, ZType *base) {
    ZFuncTable *func        = zalloc(allocator, ZFuncTable);
    *func                   = (ZFuncTable){ 0 };
    func->base              = base;
    return func;
}

ZFuncTable *putOrInsertFuncTable(ZThreadSem *ctx, ZType *type) {
    ZFuncTable *table = resolveFuncTable(ctx, type);
    if (table) return table;

    table = makefunctable(ctx->allocator, type);
    vecpush(ctx->funcs, table);

    return table;
}

static void addStaticFunc(ZThreadSem *ctx, ZNode *func) {
    ZType *base = func->funcDef.base;

    ZFuncTable *cur = putOrInsertFuncTable(ctx, base);
    char *name = func->funcDef.name->str;
    if (!hashset_insert(&cur->seenStaticFuncs, name)) {
        zlog(ctx->state, func->tok, Z0001, name);
        return;
    }

    vecpush(cur->staticFuncDef, func);
}

static void addReceiverFunc(ZThreadSem *ctx, ZNode *node) {
    ZNode *receiver         = node->funcDef.receiver;

    // if (!receiver->field.type) {
    //     zlog(ctx->state, node->tok, Z9041);
    //     return;
    // }

    ZFuncTable *table       = putOrInsertFuncTable(ctx, receiver->field.type);

    if (!hashset_insert(&table->seenReceiverFuncs, node->funcDef.name->str)) {
        ZLog *diag = zlog(ctx->state, node->funcDef.name, Z0002,
                               stoken(node->funcDef.name));
        emitHint(diag, "rename '%s' or move it into a separate impl",
                 stoken(node->funcDef.name));
    }
    vecpush(table->funcDef, node);
}

static void putReceiverFunc(ZThreadSem *ctx, ZNode *node) {
    if (!node->funcDef.receiver) {
        zlog(ctx->state, node->tok, Z0003);
        return;
    } else if (node->funcDef.base) {
        zlog(ctx->state, node->tok, Z0004);
        return;
    }
    addReceiverFunc(ctx, node);
}

static void putStaticFunc(ZThreadSem *ctx, ZNode *node) {
    ZType *baseType = node->funcDef.base;

    if (baseType->kind != Z_TYPE_PRIMITIVE) {
        zlog(ctx->state, node->tok, Z0005);
        return;
    }
    ZToken *base = baseType->primitive.token;
    if (!base) {
        zlog(ctx->state, node->tok, Z0007);
        return;
    } else if (node->funcDef.receiver) {
        zlog(ctx->state, node->tok, Z0008);
        return;
    }

    addStaticFunc(ctx, node);
}

void putFunc(ZThreadSem *ctx, ZNode *node) {
    if (node->funcDef.receiver) {
        if (node->funcDef.base) {
            zlog(ctx->state, node->tok, Z3002);
        }

        putReceiverFunc(ctx, node);
        if (node->funcDef.pub) vecpush(ctx->exportedFuncs, node);
    } else if (node->funcDef.base) {
        putStaticFunc(ctx, node);
        if (node->funcDef.pub) vecpush(ctx->exportedFuncs, node);
    } else {
        ZSymbol *f = makeRawSymbol(
                ctx->allocator,
                Z_SYM_FUNC,
                node->funcDef.name,
                node->resolved,
                node,
                node->funcDef.pub);

        if (strcmp(node->funcDef.name->str, "main") == 0) {
            ctx->semantic->main = f;
            if (isVoid(node->resolved->func.ret)) {
                zlog(ctx->state, node->funcDef.name, Z3003);
            }
        }
        putSymbol(ctx, f);
    }
}

void putImpl(ZThreadSem *ctx, ZNode *node) {
    hashset_t seen = NULL;
    char **facetNames = NULL;
    hashset_t funcs = NULL;
    usize funcLen = veclen(node->impl.funcs);

    ZType *recv = NULL;
    for (usize i = 0; veclen(node->impl.facets) && i < funcLen; i++) {
        ZNode *f = node->impl.funcs[i];
        ZNode *r = f->funcDef.receiver;
        if (!r) {
            zlog(ctx->state, f->tok, Z3061, stoken(f->funcDef.name));
            continue;
        }
        ZType *t = r->resolved;
        if (!t) continue;
        if (t->kind != Z_TYPE_POINTER) {
            zlog(ctx->state, r->tok, Z303C);
        } else if (!recv) {
            recv = t;
        } else if (!typesEqual(recv, t)) {
            zlog(ctx->state, r->tok, Z3062, stype(recv), stype(t));
        }
    }

    for (usize i = 0; i < funcLen; i++) {
        ZNode *func = node->impl.funcs[i];
        putFunc(ctx, func);
        hashset_insert(&funcs, func->funcDef.name->str);
    }

    ZType **facets = node->impl.facets;
    for (usize i = 0; i < veclen(facets); i++) {
        if (!facets[i]) continue;
        ZToken *facetRef = facets[i]->tok;
        ZType *facet = resolveTypeRef(ctx, facets[i]);

        if (!facet) continue;
        if (facet->kind != Z_TYPE_FACET) {
            zlog(ctx->state,
                facets[i]->tok,
                Z304D, stype(facet)
            );
            continue;
        }
        node->impl.facets[i] = facet;

        usize facetFuncs = veclen(facet->facet.funcs);
        for (usize j = 0; j < facetFuncs; j++) {
            ZNode *func = facet->facet.funcs[j];
            char *name  = func->field.identifier->str;
            if (!hashset_insert(&seen, name)) {
                zlog(ctx->state, node->tok, Z304E, name);
                continue;
            }

            if (!hashset_has(funcs, name)) {
                zlog(ctx->state, facetRef,
                    Z304F,
                    stype(facet), stype(node->impl.base), name
                );
            }
            vecpush(facetNames, name);
        }
    }
    hashset_free(&seen);
    hashset_free(&funcs);
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
void putVarPattern(
        ZThreadSem *ctx, ZNode *node,
        ZType *type, ZVarDestructPattern *pattern, bool condition) {
    if (!type) {
        zlog(ctx->state, node->tok, Z9001);
        return;
    }
    pattern->resolved = type;
    if (pattern->type == Z_VAR_LIT && condition) {
        ZType *literalType = resolveLiteralType(ctx, pattern->ident, NULL);
        if (!typesCompatible(ctx, literalType, type).promoted) {
            zlog(ctx->state, pattern->tok,
                Z0009, stype(type), stype(literalType)
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
            zlog(ctx->state, pattern->tok, Z000A, stype(type));
            return;
        }

        usize expected  = veclen(type->tuple);
        usize got       = veclen(pattern->tuple);
        if (expected != got) {
            zlog(ctx->state, pattern->tok,
                    Z000B, expected, got, stoken(pattern->tok));
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
            zlog(ctx->state, pattern->tok, Z000C, stype(type));
            return;
        }

        for (usize i = 0; i < veclen(pattern->fields); i++) {
            ZNode *structField = getStructField(ctx, type, pattern->fields[i]->key);

            if (!structField) {
                zlog(ctx->state, pattern->fields[i]->key,
                    Z000D,
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
            zlog(ctx->state, pattern->tok, Z000A, stype(type));
            return;
        } else if (!tokeneq(type->enm.name, pattern->base)) {
            zlog(ctx->state, pattern->tok,
                    Z0009,
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
            zlog(ctx->state, pattern->tok, Z000E,
                pattern->prop->str, stype(type));
            return;
        }

        usize expected  = veclen(variant->strct.fields) - 1;
        usize got       = veclen(pattern->args);

        if (expected != got) {
            zlog(ctx->state, pattern->tok, Z000F, expected, got);
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
            zlog(ctx->state, pattern->tok,
                Z00A0,
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
        zlog(ctx->state, pattern->tok, Z9002);
    }
}

void putGeneric(ZThreadSem *ctx, ZType *type) {
    putRawSymbol(
        ctx,
        Z_SYM_GENERIC,
        type->generic.name,
        type,
        NULL,
        false
    );
}

void putStruct(ZThreadSem *ctx, ZNode *node) {
    ZType *type             = maketype(ctx->allocator, Z_TYPE_STRUCT);
    type->strct.name        = node->structDef.ident;
    type->strct.fields      = node->structDef.fields;
    type->strct.generics    = NULL;
    type->annotations       = node->annotations;
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

void putEnum(ZThreadSem *ctx, ZNode *node) {
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

void putTypedef(ZThreadSem *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_TYPEDEF,
        node->typeDef.alias,
        node->typeDef.type,
        node,
        node->typeDef.pub
    );
}

void putFacet(ZThreadSem *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_FACET,
        node->facet.name,
        node->resolved,
        node,
        node->facet.pub
    );
}

void putNamespace(ZThreadSem *ctx, ZNode *node) {
    putRawSymbol(ctx,
        Z_SYM_FOREIGN,
        node->tok,
        node->resolved,
        node,
        node->pub
    );
}

static ZCastInfo checkCapability(ZThreadSem *ctx, ZType *capability, ZType *requested) {
    if (!capability || !requested) return (ZCastInfo){ 0 };

    if (typesEqual(capability, requested)) return (ZCastInfo){
        capability, requested, requested, Z_CAST_NOOP
    };

    return typesCompatible(ctx, capability, requested);
}

static ZCapabilityCast lookupCapabilityByType(ZThreadSem *ctx, ZScope *scope, ZType *capability) {
    for (usize i = 0; i < veclen(scope->capabilities); i++) {
        ZCastInfo c = checkCapability(ctx, scope->capabilities[i]->type, capability);
        if (c.promoted) return (ZCapabilityCast){ NULL, c, i };
    }
    return (ZCapabilityCast){ NULL, { 0 }, -1 };
}

ZCapability *putCapability(ZThreadSem *ctx, ZNode *var) {
    if (!var || !var->resolved) {
        zlog(ctx->state, var ? var->tok : NULL, Z9003);
        return NULL;
    }

    ZScope *cur = ctx->current;

    ZCapabilityCast c = lookupCapabilityByType(ctx, cur, var->resolved);

    ZCapability *capability = NULL;
    if (c.index == -1) {
        capability          = zalloc(ctx->allocator, ZCapability);
        capability->nodes   = NULL;
        capability->type    = var->resolved;
        vecpush(cur->capabilities, capability);
    } else {
        capability = cur->capabilities[c.index];
    }
    vecpush(capability->nodes, (var));
    return capability;
}

// ZThreadSem *getRegisteredModule(ZSemantic *ctx, char *module) {
//     for (usize i = 0; i < veclen(ctx->scopes); i++) {
//         if (strcmp(ctx->scopes[i]->module->module.filename, module) == 0) {
//             return ctx->scopes[i]->ctx;
//         }
//     }
//     return NULL;
// }

// ZThreadSem *registerModule(ZSemantic *ctx, ZFile *f) {
//     Allocator *allocator = ctx->state->allocator;
//     for (usize i = 0; i < veclen(ctx->scopes); i++) {
//         if (ctx->scopes[i]->module == module) {
//             ctx->semantics[i]->current = ctx->semantics[i]->local;
//             return ctx->semantics[i];
//         }
//     }
//
//     /* Parent the module scope to the lexically enclosing scope (the importer),
//      * so a module can resolve names from the scope that pulled it in. */
//     ZScopeTable *table  = zalloc(allocator, ZScopeTable);
//     table->module       = module;
//     table->scope        = makescope(allocator, NULL, module);
//
//     vecpush(ctx->scopes, table);
//
//     ZThreadSem *threadCtx = makethreadsem(
//         ctx, table->scope, module, m->allocator
//     );
//     table->ctx = threadCtx;
//     vecpush(ctx->semantics, threadCtx);
//     return threadCtx;
// }

static void warnUnused(ZThreadSem *ctx, ZSymbol *symbol) {
    if (!symbol) return;
    if (query(symbol->node->annotations, "unused")) return;
    switch (symbol->kind) {
    case Z_SYM_FUNC:
        if (ctx->state->cli.unusedFunc) break;
        zlog(ctx->state, symbol->name, Z3004, symbol->name->str);
        break;
    case Z_SYM_STRUCT:
        if (ctx->state->cli.unusedStruct) break;
        zlog(ctx->state, symbol->name, Z3005, symbol->name->str);
        break;
    case Z_SYM_VAR:
        if (ctx->state->cli.unusedVar) break;
        zlog(ctx->state, symbol->name, Z3006, symbol->name->str);
        break;
    default:
        zlog(ctx->state, symbol->name, Z3008);
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

void beginScope(ZThreadSem *ctx, ZNode *curr) {
    ZScope *scope       = makescope(ctx->allocator, ctx->current, curr);
    ctx->current = scope;
}

void endScope(ZThreadSem *ctx) {
    if (!ctx->current) {
        zlog(ctx->state, NULL, Z9004);
        return;
    } else if (!ctx->current->parent) {
        zlog(ctx->state, NULL, Z9005);
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

static ZTokenType toSigned(u8 rank) {
    switch (rank) {
    case 1:     return TOK_I8;
    case 2:     return TOK_I16;
    case 3:     return TOK_I32;
    case 4:
    default:    return TOK_I64;
    }
}

bool isComparable(ZThreadSem *ctx, ZType *type) {
    type = resolveTypeRef(ctx, type);
    if (!type) return false;
    return typeKindIs(type->kind, TYPE_COMPARABLE_MASK);
}

bool isZeroable(ZThreadSem *ctx, ZType *type) {
    if (!type) return false;
    switch (type->kind) {
    case Z_TYPE_POINTER:    return false;
    case Z_TYPE_PRIMITIVE:  return true;
    case Z_TYPE_FUNCTION:   return false;
    case Z_TYPE_FACET:      return false;
    case Z_TYPE_NONE:       return true;
    case Z_TYPE_OPTIONAL:   return true;
    case Z_TYPE_RESULT:     return true;
    case Z_TYPE_SUM:
        if (veclen(type->sumType) == 0) return false;
        return isZeroable(ctx, type->sumType[0]);
    case Z_TYPE_ENUM:
        if (veclen(type->enm.fields) == 0) return false;
        return isZeroable(ctx, type->enm.fields[0]->resolved);

    case Z_TYPE_ARRAY:
        if (type->array.dynamic) return true;
        else if (type->array.size == 0) return true;
        return isZeroable(ctx, type->array.base);
    case Z_TYPE_STRUCT:
        for (usize i = 0; i < veclen(type->strct.fields); i++) {
            if (!isZeroable(ctx, type->strct.fields[i]->resolved)) return false;
        }
        return true;
    case Z_TYPE_TUPLE:
        for (usize i = 0; i < veclen(type->tuple); i++) {
            if (!isZeroable(ctx, type->tuple[i])) return false;
        }
        return true;
    default: return false;
    }
}

/*
 * @brief Find the '#as' field of a struct (searching embedded fields too)
 * whose type is `to`.
 *
 * If path is not NULL the struct indices to reach the field are pushed into it.
 */
ZType *implicitStructFieldCast(
        ZThreadSem *ctx, ZType *strct, ZType *to, u32 **path) {
    if (!strct || strct->kind != Z_TYPE_STRUCT) return NULL;
    if (typesEqual(strct, to)) return strct;
    for (usize i = 0; i < veclen(strct->strct.fields); i++) {
        ZNode *field = strct->strct.fields[i];
        if (!field) continue;
        if (!query(field->annotations, "as")) continue;
        if (path) vecpush(*path, (u32)i);
        if (typesEqual(field->resolved, to)) return field->resolved;
        if (field->type == NODE_EMBED_FIELD) {
            ZType *res = implicitStructFieldCast(ctx, field->resolved, to, path);
            if (res) return res;
        }
        if (path) vecpop(*path);
    }
    return NULL;
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
ZCastInfo typesCompatible(ZThreadSem *ctx, ZType *from, ZType *to) {
    #define CAST(k, p) (ZCastInfo) { .from=from, .to=to, .kind=k, .promoted=p }

    if (!from || !to) return CAST(Z_CAST_INVALID, NULL);

    if (to->kind == Z_TYPE_NONE && isZeroable(ctx, from)) {
        return CAST(Z_CAST_NONE, from);
    } else if (from->kind == Z_TYPE_NONE && isZeroable(ctx, to)) {
        return CAST(Z_CAST_NONE, to);
    }

    /* satisfyFacet already rejects everything that is not a pointer. */
    if (to->kind == Z_TYPE_FACET        &&
        from->kind != Z_TYPE_FACET      &&
        satisfyFacet(ctx, from, to)     )
        return CAST(Z_CAST_FACET, from);

    if (to->kind == Z_TYPE_POINTER      &&
        from->kind == Z_TYPE_ARRAY      )
        return CAST(Z_CAST_ARRAY_PTR, to);

    if (from->kind == Z_TYPE_POINTER && to->kind == Z_TYPE_POINTER)
        return CAST(Z_CAST_POINTER, from);

    if (from->kind == Z_TYPE_TYPE && to->kind == Z_TYPE_TYPE)
        return CAST(Z_CAST_NOOP, from);

    if (typesEqual(from, to)) return CAST(Z_CAST_NOOP, to);

    if (from->kind == Z_TYPE_OPTIONAL) {
        if (isOptionalPointer(from) && isOptionalPointer(to))
            return CAST(Z_CAST_OPTIONAL, from);
    } else if (to->kind == Z_TYPE_OPTIONAL) {
        ZCastInfo c = typesCompatible(ctx, from, to->optional);
        if (c.kind == Z_CAST_INVALID) return CAST(Z_CAST_INVALID, NULL);
        return CAST(Z_CAST_OPTIONAL, c.promoted);
    }

    ZType *interpType   = consts(ctx->state, interpType);
    ZType *strType      = consts(ctx->state, strType);

    if (!interpType) {
        ZNode *interp = LangItems[Z_LANG_INTERPOLATED_STRING];
        if (interp) {
            ZType *arr          = maketype(ctx->allocator, Z_TYPE_ARRAY);
            arr->array.base     = interp->resolved;
            arr->array.size     = 1;
            arr->array.dynamic  = false;
            interpType = consts(ctx->state, interpType) = arr;
        }
    }

    if (typesEqual(to, interpType)) {
        ZNode *writable = LangItems[Z_LANG_WRITABLE];
        if (typesEqual(from, strType)) {
            return CAST(Z_CAST_INTERP, interpType);
        } else if (writable && satisfyFacet(ctx, from, writable->resolved)) {
            return CAST(Z_CAST_INTERP, interpType);
        }
    }
    if (typesEqual(from, strType)) {
        if (typesEqual(to, interpType)) {
            return CAST(Z_CAST_INTERP, interpType);
        }
    }

    if (to->kind == Z_TYPE_SUM) {
        for (usize i = 0; i < veclen(to->sumType); i++)
            if (typesEqual(from, to->sumType[i]))
                return CAST(Z_CAST_SUM, to);
    }
    if (from->kind == Z_TYPE_SUM) {
        for (usize i = 0; i < veclen(from->sumType); i++)
            if (typesEqual(to, from->sumType[i]))
                return CAST(Z_CAST_SUM, from);
    }

    if (from->kind == Z_TYPE_STRUCT) {
        ZType *field = implicitStructFieldCast(ctx, from, to, NULL);
        if (field) return CAST(Z_CAST_FIELD, field);

    }

    if (from->kind != Z_TYPE_PRIMITIVE || to->kind != Z_TYPE_PRIMITIVE)
        return CAST(Z_CAST_INVALID, NULL);

    ZToken *tokA    = from->primitive.token;
    ZToken *tokB    = to->primitive.token;
    ZTokenType ta   = tokA->type;
    ZTokenType tb   = tokB->type;

    if (ta == TOK_VOID || tb == TOK_VOID)
        return CAST(Z_CAST_INVALID, NULL);

    u8 ra = typeRank(ta);
    u8 rb = typeRank(tb);

    if (isFloat(tokA) || isFloat(tokB)) {
        return CAST(Z_CAST_NUMERIC, ra > rb ? from : to);
    }

    if ((isSigned(tokA) && isSigned(tokB)) ||
        (isUnsigned(tokA) && isUnsigned(tokB)))
        return CAST(Z_CAST_NUMERIC, ra > rb ? from : to);

    /* signed vs unsigned */
    u8    signedRank   = isSigned(tokA) ? ra : rb;
    u8    unsignedRank = isSigned(tokA) ? rb : ra;
    ZType *signedType  = isSigned(tokA) ? from  : to;

    if (signedRank > unsignedRank)
        return CAST(Z_CAST_NUMERIC, signedType);

    // if (signedRank == 4) {
    //     zlog(ctx->state, signedType->tok,
    //             Z3009);
    // }

    ZType *promoted             = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
    promoted->primitive.token   = makeTokenThread(ctx, toSigned(signedRank + 1), NULL);
    promoted->tok               = from->tok;
    return CAST(Z_CAST_NUMERIC, promoted);
    #undef CAST
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

    return (typeA->hash > typeB->hash) - (typeA->hash < typeB->hash);
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

    if (!a->hash) a->hash = hash(a);
    if (!b->hash) b->hash = hash(b);
    if (a->hash == b->hash) return true;
    // if (a->hash && b->hash && a->hash == b->hash) return true;

    switch (a->kind) {
    case Z_TYPE_PRIMITIVE:
        if (a->primitive.base &&
            b->primitive.base &&
            !typesEqual(a->primitive.base, b->primitive.base))
            return false;
        return tokeneq(a->primitive.token, b->primitive.token);
    case Z_TYPE_POINTER:
        return typesEqual(a->base, b->base);
    case Z_TYPE_ARRAY: {
        bool ad = a->array.dynamic;
        bool bd = b->array.dynamic;

        // A dynamic with non-dynamic array are not compatible
        if ((ad && !bd) || (!ad && bd)) return false;
        return typesEqual(a->array.base, b->array.base);
    }
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

    case Z_TYPE_TYPE:
        return true;
    default:
        return false;
    }
}

i32 sumTypeIndexOf(ZType *sum, ZType *concrete) {
    for (usize i = 0; i < veclen(sum->sumType); i++) {
        if (typesEqual(sum->sumType[i], concrete))
            return (i32)i;
    }
    return -1;
}



/*
 * Returns true if `type` embeds `root` by value (not through a pointer),
 * which would make the struct infinitely large. `seen` is a visited-struct
 * set to avoid re-entering mutual-recursion loops.
 */
bool isInfiniteSize(ZType *type, ZType *root, ZType ***seen) {
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

static ZCapabilityCast lookupScopedCapability(ZThreadSem *ctx, ZType *required) {
    ZScope *curr = ctx->current;
    ZCapabilityCast c;
    while (curr) {
        c = lookupCapabilityByType(ctx, curr, required);
        if (c.index != -1) break;
        if (curr->node->type == NODE_FUNC) break;
        curr = curr->parent;
    }

    ZCapability **cc = curr->capabilities;

    if (c.index == -1 || veclen(cc[c.index]->nodes) == 0) return c;

    ZNode *node = veclast(cc[c.index]->nodes);
    c.node = implicitCast(ctx, node, c.cast.promoted);
    return c;
}

ZCapabilityCast *analyzeCapabilities(ZThreadSem *ctx, ZToken *start, ZType **capabilities) {
    ZCapabilityCast *capabilityRefs = NULL;
    for (usize i = 0; i < veclen(capabilities); i++) {
        ZType *old = capabilities[i];
        capabilities[i] = resolveTypeRef(ctx, old);

        if (!capabilities[i]) {
            zlog(ctx->state, start, Z00A4, stype(old));
            continue;
        }
        ZCapabilityCast reference = lookupScopedCapability(
            ctx, capabilities[i]
        );
        if (!reference.node) {
            zlog(ctx->state, start, Z00A4, stype(capabilities[i]));
        }
        vecpush(capabilityRefs, reference);
    }
    return capabilityRefs;
}

