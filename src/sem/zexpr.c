#include "zsem.h"

static ZType *resolveEnumLit        (ZThreadSem *, ZNode *, ZType *);
static ZSymbol *resolveModuleChain (ZThreadSem *, ZToken **, usize *);

/*
 * @brief Store the cast info in a cast node.
 *
 * `from` is always the type of the casted expression and `to` the type the
 * node evaluates to, so the codegen can switch on the kind only.
 */
static void setCastInfo(ZThreadSem *ctx, ZNode *cast, ZCastInfo info) {
    info.from               = cast->castExpr.expr->resolved;
    cast->castExpr.info     = info;
    cast->castExpr.fieldPath = NULL;
    if (info.kind == Z_CAST_FIELD) {
        implicitStructFieldCast(ctx, info.from, info.to, &cast->castExpr.fieldPath);
    }
}

/*
 * @brief Wrap node in a cast to `to`.
 *
 * The cast kind is computed on the (node->resolved, to) pair: a promoted type
 * can't be used as target since for assignments and arguments it may differ
 * from the destination type.
 */
ZNode *implicitCast(ZThreadSem *ctx, ZNode *node, ZType *to) {
    if (!node || !to) return node;
    if (node->resolved && typesEqual(node->resolved, to)) {
        return node;
    }

    ZNode *cast             = makeNodeThread(ctx, NODE_CAST);
    cast->castExpr.expr     = node;
    cast->resolved          = to;
    cast->tok               = node->tok;
    setCastInfo(ctx, cast, typesCompatible(ctx, node->resolved, to));
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
ZNode *coerceToSum(ZThreadSem *ctx, ZNode *node, ZType *sum) {
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
        int len = veclen(scope->symbols);
        if (len == 0) goto next;
        for (int i = len-1; i >= 0; i--) {
            if (tokeneq(scope->symbols[i]->name, ident)) {
                scope->symbols[i]->useCount++;
                return scope->symbols[i];
            }
        }
        next:
        scope = scope->parent;
    }
    return NULL;
}

ZSymbol *resolve(ZThreadSem *ctx, ZToken *ident) {
    if (ident && ident->type == TOK_IDENT && strcmp(ident->str, "_") == 0) {
        zlog(ctx->state, ident, Z300A);
    }
    return resolveByScope(ctx->current, ident);
}

/* ================== Type resolution ================== */

static inline ZType *derefType(ZType *t) {
    while (t && t->kind == Z_TYPE_POINTER) t = t->base;
    return t;
}

ZType *resolveLiteralType(ZThreadSem *ctx, ZToken *curr, ZType *inferred) {
    (void)inferred;
    if (curr->type == TOK_NONE) return consts(ctx->state, none);

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
        /* String literals are []char */
        ZType *base = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
        base->primitive.token = makeTokenThread(ctx, TOK_CHAR, NULL);
        t->kind = Z_TYPE_ARRAY;
        t->array.base = base;
        t->array.size = strlen(stoken(curr));
        break;
    }
    case TOK_STREAM: {
        ZNode *intstr = LangItems[Z_LANG_INTERPOLATED_STRING];
        if (!intstr) {
            zlog(ctx->state, curr, Z00AA);
            return NULL;
        }
        ZType *arr          = makeTypeThread(ctx, Z_TYPE_ARRAY);
        arr->array.size     = 0;
        arr->array.dynamic  = false;
        arr->array.base     = intstr->resolved;
        arr->tok            = curr;
        return arr;
    }
    default: {
        t->primitive.token = makeTokenThread(ctx, TOK_VOID, NULL);
        break;
    }
    }
    t->tok  = curr;
    return t;
}

static ZType *_resolveTypeRef(ZThreadSem *, ZType *, ZType ***);

static ZScope *resolveModulePath(ZThreadSem *ctx, ZType *path) {
    ZToken *name    = path->primitive.token;
    ZSymbol *sym    = NULL;

    if (path->primitive.base) {
        ZScope *outer = resolveModulePath(ctx, path->primitive.base);
        if (!outer) return NULL;
        sym = resolveByScope(outer, name);
    } else {
        sym = resolve(ctx, name);
    }

    if (!sym || sym->kind != Z_SYM_IMPORT) {
        zlog(ctx->state, name, Z305F);
        return NULL;
    }
    return sym->scope;
}

static ZType *resolvePrimitiveRef(ZThreadSem *ctx, ZType *type, ZType ***seen) {
    ZToken *tok = type->primitive.token;
    if (tok->type != TOK_IDENT) return type;

    ZSymbol *sym = NULL;
    if (type->primitive.base) {
        ZScope *mod = resolveModulePath(ctx, type->primitive.base);
        if (!mod) return NULL;
        sym = resolveByScope(mod, tok);
        if (!sym) {
            zlog(ctx->state, tok, Z3060, stoken(tok));
            return NULL;
        }
    } else {
        sym = resolve(ctx, type->primitive.token);
        if (!sym) {
            zlog(ctx->state, type->primitive.token, Z00AB, stype(type));
            return NULL;
        }
    }

    if (sym->kind == Z_SYM_TYPEDEF) {
        if (sym->node->resolved) return sym->node->resolved;
        vecpush(*seen, type);
        ZType *res = _resolveTypeRef(ctx, sym->node->typeDef.type, seen);
        vecpop(*seen);
        return res;
    }
    return sym->type;
}

static ZType *_resolveTypeRef(ZThreadSem *ctx, ZType *type, ZType ***seen) {
    if (!type) return NULL;

    for (usize i = 0; i < veclen(*seen); i++) {
        if (typesEqual((*seen)[i], type)) {
            zlog(ctx->state, type->tok, Z300B);
            return (*seen)[i];
        }
    }
    switch (type->kind) {
    case Z_TYPE_PRIMITIVE: return resolvePrimitiveRef(ctx, type, seen);
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

    case Z_TYPE_RESULT:
        _resolveTypeRef(ctx, type->result.success, seen);
        _resolveTypeRef(ctx, type->result.error, seen);
        if (typesEqual(type->result.success, type->result.error)) {
            zlog(ctx->state, type->tok, Z00A1);
        }
        return type;
    case Z_TYPE_SUM:
        for (usize i = 0; i < veclen(type->sumType); i++)
            type->sumType[i] = _resolveTypeRef(ctx, type->sumType[i], seen);
        return type;
    case Z_TYPE_TYPE:
        type->typeType = _resolveTypeRef(ctx, type->typeType, seen);
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
ZType *resolveTypeRef(ZThreadSem *ctx, ZType *type) {
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
            zlog(ctx->state, node->tok, Z00A2);
            continue;
        }
        ZType *resolved = resolveTypeRef(ctx, args[i]);
        if (!resolved) {
            zlog(ctx->state, args[i]->tok, Z00A2);
        } else {
            args[i] = resolved;
        }
    }
    return node;
}

/* A `type` value is a pointer to the typeinfo at runtime, so methods and
 * facets are looked up as if it were `*Type`. */
static inline ZType *resolveOperand(ZThreadSem *ctx, ZType *obj) {
    if (!obj || obj->kind != Z_TYPE_TYPE) return obj;
    ZNode *info = LangItems[Z_LANG_TYPE_INFO];
    if (!info || !info->resolved) return obj;
    ZType *ptr  = makeTypeThread(ctx, Z_TYPE_POINTER);
    ptr->base   = info->resolved;
    ptr->tok    = obj->tok;
    return ptr;
}

static bool funcTableBaseMatches(ZThreadSem *ctx, ZType *tableBase, ZType *obj) {
    if (!tableBase || !obj) return false;
    obj = resolveOperand(ctx, obj);
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

    if (obj->kind == Z_TYPE_PRIMITIVE && obj->primitive.token->type == TOK_IDENT &&
        tableBase->kind != Z_TYPE_PRIMITIVE)
        return funcTableBaseMatches(ctx, obj, tableBase);
    return false;
}

ZFuncTable *resolveFuncTable(ZThreadSem *ctx, ZType *obj) {
    for (usize i = 0; i < veclen(ctx->funcs); i++)
        if (funcTableBaseMatches(ctx, ctx->funcs[i]->base, obj))
            return ctx->funcs[i];
    return NULL;
}


static ZNode *resolveFuncCallEmbedded(ZThreadSem *ctx,
    ZNode *curr, ZType *obj, ZToken *prop) {
    ZNode *ptr = NULL;
    if (obj) switch (obj->kind) {
    case Z_TYPE_STRUCT:
        for (usize i = 0; i < veclen(obj->strct.fields); i++) {
            ZNode *field = obj->strct.fields[i];
            if (field->type != NODE_EMBED_FIELD) continue;
            if (ptr) {
                zlog(ctx->state, prop, Z00A3, stype(ptr->resolved));
            } else {
                ptr = resolveFuncCallEmbedded(
                    ctx, curr, field->resolved, prop);
            }
        }
        break;
    case Z_TYPE_FACET:
        for (usize i = 0; i < veclen(obj->facet.funcs); i++) {
            ZNode *field = obj->facet.funcs[i];
            if (field->type != NODE_EMBED_FIELD) continue;
            if (ptr) {
                zlog(ctx->state, prop, Z00A3, stype(ptr->resolved));
            } else {
                ptr = resolveFuncCallEmbedded(
                    ctx, curr, field->resolved, prop);
            }
        }
        break;

    default: break;
    }

    ZFuncTable *funcs = resolveFuncTable(ctx, obj);

    if (funcs) {
        for (usize i = 0; i < veclen(funcs->funcDef); i++) {
            ZNode *f = funcs->funcDef[i];
            if (tokeneq(f->funcDef.name, prop)) {
                if (ptr) {
                    zlog(ctx->state, prop, Z00A3, stype(ptr->resolved));
                }
                ptr = f;
            }
        }
    }

    if (ptr) {
        curr->memberAccess.mangled  = ptr->funcDef.mangled;
        curr->memberAccess.method   = ptr;
    }
    return ptr;
}
static inline usize countOptionalFields(ZNode **args) {
    usize res = 0;
    for (usize i = 0; i < veclen(args); i++) {
        if (args[i]->field.defaultValue) res++;
    }
    return res;
}

static void resolveFuncArgs(
    ZThreadSem *ctx, ZType **expectedArgs, ZNode **args,
    ZToken *start, bool variadic) {

    usize expectedArgsLen = veclen(expectedArgs);

    if ((!variadic && expectedArgsLen != veclen(args)) ||
        ( variadic && expectedArgsLen >  veclen(args))) {
        zlog(ctx->state, start, Z000F, expectedArgsLen, veclen(args));
        return;
    }

    for (usize i = 0; i < expectedArgsLen; i++) {
        ZType *expected     = expectedArgs[i];
        expected            = resolveTypeRef(ctx, expected);

        args[i]->resolved   = resolveType(ctx, args[i], expected);
        args[i]->resolved   = resolveTypeRef(ctx, args[i]->resolved);
        checkFunctionUsedAsValue(ctx, args[i]);

        /* If the argument is a generic skip the validation*/
        if (expected && expected->kind == Z_TYPE_GENERIC) {
            continue;
        }

        /* if they are equal they don't need implicit casting. */
        if (typesEqual(args[i]->resolved, expected)) continue;

        ZCastInfo cast = typesCompatible(
            ctx, args[i]->resolved, expected);

        if (!cast.promoted) {
            zlog(ctx->state, args[i]->tok,
                Z0009,
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
            zlog(ctx->state, callee->identNode.tok,
                  Z00A5, callee->identNode.tok->str);
            return NULL;
        }
        callee->identNode.ref = sym->node;
        callee->identNode.li  = getLangItemType(sym->node);

        if (sym->kind == Z_SYM_FACET) {
            if (veclen(args) != 1) {
                zlog(ctx->state, callee->tok, Z00A6, veclen(args));
                return sym->type;
            }
            ZType *arg = resolveType(ctx, args[0], inferred);
            args[0]->resolved = arg;
            if (!satisfyFacet(ctx, arg, sym->type)) {
                zlog(ctx->state, args[0]->tok,
                    Z00A7, stype(arg), stype(sym->type)
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
                zlog(ctx->state, callee->identNode.tok,
                      Z00A8, callee->identNode.tok->str);
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
                rawRet = sym->type->func.ret;
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
        if (!resolved) return NULL;

        if (resolved->kind == Z_TYPE_FUNCTION) {
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
                zlog(ctx->state, callee->tok, Z00A9, stoken(prop));
                return NULL;
            }

            curr->resolved = resolved;
            expectedFunc = makeTypeThread(ctx, Z_TYPE_FUNCTION);
            expectedFunc->func.ret = resolved;
            for (usize i = 1; i < veclen(fields); i++) {
                vecpush(expectedFunc->func.args, fields[i]->resolved);
            }
        } else {
            zlog(ctx->state, callee->tok, Z300C, stype(resolved));
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
            zlog(ctx->state, callee->tok, Z00A8, stype(calleeType));
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
        if (!inferred) {
            zlog(ctx->state, curr->tok, Z3031);
            return NULL;
        }
        ZType *expected = inferred->kind == Z_TYPE_OPTIONAL ?
            inferred->optional : inferred;
        symType         = resolveTypeRef(ctx, expected);
        if (!symType) {
            zlog(ctx->state, ident, Z300D);
            return NULL;
        }

        if (symType->kind != Z_TYPE_STRUCT) return NULL;
        structSym   = resolve(ctx, symType->strct.name);

    } else {
        usize i;
        structSym   = resolveModuleChain(ctx, curr->structlit.chain, &i);

        if (!structSym) {
            zlog(ctx->state, curr->tok, Z300E, curr->tok->str);
            return NULL;
        }

        symType = resolveType(ctx, structSym->node, inferred);
    }
    if (!symType || symType->kind != Z_TYPE_STRUCT) {
        zlog(ctx->state, ident, Z300F, stoken(ident));
        return NULL;
    }

    if (structSym && strcmp(structSym->name->filename, curr->tok->filename) != 0) {
        if (query(structSym->node->annotations, "sealed"))
            zlog(ctx->state, curr->tok, Z305C);
    }

    usize expectedLen   = veclen(symType->strct.fields);
    usize optionals     = countOptionalFields(symType->strct.fields);
    usize gotLen        = veclen(curr->structlit.fields);
    if (expectedLen - optionals != gotLen) {
        if (!query(curr->annotations, "partial")) {
            zlog(ctx->state, curr->tok, Z3010, expectedLen, gotLen);
        }
    }

    for (usize i = 0; i < veclen(curr->structlit.fields); i++) {
        ZNode *field        = curr->structlit.fields[i];
        ZNode *structField  = getStructField(ctx, symType, field->tok);

        if (!structField) {
            zlog(ctx->state, field->tok, Z3011);
            continue;
        }

        ZType *expectedType = resolveTypeRef(ctx, structField->field.type);
        ZType *type         = resolveType(ctx, field->varDecl.rvalue, expectedType);

        if (!type) {
            zlog(ctx->state, field->varDecl.rvalue->tok, Z00A2);
            continue;
        }

        field->resolved = type;

        if (!structField) {
            zlog(ctx->state, field->tok,
                Z000D,
                field->tok->str, stype(structSym->type)
            );
            return NULL;
        }
        structField->field.type = expectedType;
        ZCastInfo cast = typesCompatible(ctx, type, expectedType);
        if (!cast.promoted) {
            zlog(ctx->state,
                field->tok,
                Z0009,
                stype(expectedType),
                stype(type)
            );
        }
    }
    return symType;
}

void checkFunctionUsedAsValue(ZThreadSem *ctx, ZNode *node) {
    if (!node || node->type != NODE_MEMBER) return;
    if (node->memberAccess.mangled) {
        zlog(ctx->state, node->memberAccess.field,
            Z3013,
            node->memberAccess.field->str);
    }
}

static ZType *resolveIdentByScope(ZThreadSem *ctx, ZScope *scope, ZNode *node) {
    ZToken *tok = node->identNode.tok;
    ZSymbol *sym = resolveByScope(scope, tok);
    if (!sym) {
        zlog(ctx->state, tok, Z3014, tok->str);
        return NULL;
    }

    node->identNode.ref = sym->node;
    node->identNode.sym = sym;
    node->identNode.li  = getLangItemType(sym->node);

    if (sym->node->type == NODE_FUNC) {
        node->identNode.mangled = sym->node->funcDef.mangled;
        ZNode *fn = sym->node;
        if (fn->funcDef.receiver) {
            zlog(ctx->state, tok, Z3013, tok->str);
        } else if (veclen(fn->funcDef.capabilities) > 0) {
            zlog(ctx->state, tok, Z3015, tok->str);
        }
    }
    return sym->type = resolveTypeRef(ctx, sym->type);
}

static ZType* resolveIdent(ZThreadSem *ctx, ZNode *node, ZType *inferred) {
    (void)inferred;
    return resolveIdentByScope(ctx, ctx->current, node);
}

static ZType *resolveBinary(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZToken *tok         = curr->binary.op;
    ZTokenType op       = tok->type;
    ZType     *left     = resolveTypeRef(
        ctx, resolveType(ctx, curr->binary.left, inferred)
    );

    if (op == TOK_EQ || tokmask(tok, TOK_SELF_OPERATOR)) inferred = left;
    ZType     *right    = resolveType(ctx, curr->binary.right, inferred);
    right               = resolveTypeRef(ctx, right);

    if (tokmask(tok, TOK_BITOPERATOR_MASK) &&
            (!isNumeric(left) ||
             !isNumeric(right))) {
        zlog(ctx->state, curr->binary.op, Z3016);
        return NULL;
    }

    if (tokmask(tok, TOK_SELF_OPERATOR)) {
        if (!isNumeric(left) || !isNumeric(right)) {
            zlog(ctx->state, curr->binary.op, Z3017);
            return NULL;
        }
        if (!isLvalue(curr->binary.left)) {
            zlog(ctx->state, curr->binary.left->tok, Z3018);
            return NULL;
        }
        ZCastInfo cast = typesCompatible(ctx, left, right);
        if (!cast.promoted) {
            zlog(ctx->state, curr->binary.op, Z3019, stype(left), stype(right));
            return NULL;
        }
        /* The result and the stored value keep the type of the lhs:
         * only the rhs is coerced, the lhs must stay a plain lvalue. */
        curr->binary.right = implicitCast(ctx, curr->binary.right, left);
        return left;
    }

    if (op == TOK_EQ) {
        /* Assignment is directional: the rhs is coerced to the lhs, never the
         * other way round, so it must not go through the symmetric promotion. */
        if (!isLvalue(curr->binary.left)) {
            zlog(ctx->state, curr->binary.left->tok, Z3018);
        }
        ZCastInfo cast = typesCompatible(ctx, right, left);
        if (!cast.promoted) {
            zlog(ctx->state, curr->binary.op, Z3019, stype(left), stype(right));
        }
        /* Assignment yields the type of the left-hand side. */
        curr->binary.right = implicitCast(ctx, curr->binary.right, left);
        return left;
    }

    if (op == TOK_BITL || op == TOK_BITR) {
        /* A shift keeps the type of the shifted value: the amount must not
         * widen it or change its signedness. conn4 aaaaaaaaaaaaa */
        curr->binary.right = implicitCast(ctx, curr->binary.right, left);
        return left;
    }

    ZCastInfo info  = typesCompatible(ctx, left, right);
    if (!info.promoted && tokmask(tok, TOK_BOOL_MASK))
        info        = typesCompatible(ctx, right, left);

    if (tok->type == TOK_AND || tok->type == TOK_OR) {
        if (!isComparable(ctx, left))
            zlog(ctx->state, curr->binary.left->tok, Z3040, stype(left));
        if (!isComparable(ctx, right))
            zlog(ctx->state, curr->binary.right->tok, Z3040, stype(right));
        return consts(ctx->state, u1Type);
    }

    if (!info.promoted) {
        zlog(ctx->state, curr->binary.op, Z3019, stype(left), stype(right));
    }
    curr->binary.left = implicitCast(ctx, curr->binary.left, info.promoted);
    curr->binary.right = implicitCast(ctx, curr->binary.right, info.promoted);

    if (tokmask(tok, TOK_BOOL_MASK)) {
        return consts(ctx->state, u1Type);
    }

    switch (op) {
    case TOK_PLUS:
        if (left && left->kind == Z_TYPE_POINTER &&
            isNumeric(right)             ) {
            return left;
        }
        if (right && right->kind == Z_TYPE_POINTER    &&
            isNumeric(left)) {
            return right;
        }
    case TOK_DIV:
    case TOK_STAR:
    case TOK_MINUS: {
        if (!isNumeric(curr->binary.left->resolved) ||
            !isNumeric(curr->binary.right->resolved)) {
            zlog(ctx->state, curr->tok, Z301A);
        }
        return info.promoted;
    }
    default: return info.promoted;
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
            ZCastInfo info = typesCompatible(ctx, arrType, fieldType);
            arrType = info.promoted;
            if (!arrType) {
                ZToken *tok = fieldType ? fieldType->tok : NULL;
                zlog(ctx->state, tok, Z301B);
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
            zlog(ctx->state, fields[i]->tok, Z301C);
        } else {
            vecpush(types, fieldType);
        }
    }

    ZType *result   = makeTypeThread(ctx, Z_TYPE_TUPLE);
    result->tuple   = types;
    result->tok     = node->tok;
    return result;
}

static ZType *resolveUnary(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZTokenType op      = curr->unary.operat->type;
    if (inferred) {
        if (op == TOK_REF) inferred = inferred->base;
    }
    ZType     *operand = resolveType(ctx, curr->unary.operand, inferred);

    if (!operand) {
        zlog(ctx->state, curr->tok, Z00A2);
        return NULL;
    }

    switch (op) {
    case TOK_REF: {
        ZType *ptr  = makeTypeThread(ctx, Z_TYPE_POINTER);
        ptr->base   = operand;
        ptr->tok    = curr->tok;
        return ptr;
    }

    case TOK_STAR:
        if (operand->kind != Z_TYPE_POINTER) {
            zlog(ctx->state, curr->unary.operat, Z301D);
        }
        return operand->base;

    case TOK_NOT:

        // curr->unary.operand = implicitCast(ctx, curr->unary.operand, u1Type);
        return consts(ctx->state, u1Type);

    case TOK_ESCL:
        if (!typeKindIs(operand->kind, TYPE_WRAPPER_MASK)) {
            zlog(ctx->state, curr->unary.operat, Z301E, stype(operand));
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
            zlog(ctx->state, chain[*i], Z301F, stoken(chain[*i]));
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
        zlog(ctx->state, prop, Z000D, stoken(prop), stoken(base->tok));
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
        zlog(ctx->state, curr->slice.start->tok, Z3020);
    }
    if (curr->slice.end &&
            (!end                                   ||
            !isPrimitive(end)                       ||
            !isInteger(end->primitive.token))
        ) {
        zlog(ctx->state, curr->slice.end->tok, Z3020);
    }

    ZType *u64Type = consts(ctx->state, u64Type);
    curr->slice.start   = implicitCast(ctx, curr->slice.start, u64Type);
    curr->slice.end     = implicitCast(ctx, curr->slice.end, u64Type);

    return res;
}

static ZType *resolveIf(ZThreadSem *ctx, ZNode *node, ZType *inferred) {
    if (!node || !node->ifStmt.cond || !node->ifStmt.body || !node->ifStmt.elseBranch) {
        zlog(ctx->state, node->tok, Z9006);
        return NULL;
    }
    ZType *cond         = resolveType(ctx, node->ifStmt.cond, inferred);
    ZType *trueBranch   = resolveType(ctx, node->ifStmt.body, inferred);
    ZType *falseBranch  = resolveType(ctx, node->ifStmt.elseBranch, inferred);

    if (!isComparable(ctx, cond)) {
        zlog(ctx->state, node->ifStmt.cond->tok, Z3021);
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
        zlog(ctx->state, block->tok, Z3022);
    }
    return breakType;
}

static ZType *resolveAnonFunc(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    if (inferred) {
        inferred = resolveTypeRef(ctx, inferred);
        if (!inferred) {
            zlog(ctx->state, curr->tok, Z3023);
            return NULL;
        }

        if (inferred->kind != Z_TYPE_FUNCTION) {
            zlog(ctx->state, curr->tok, Z3024);
            return NULL;
        }
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
    bool isBool     = base->kind == Z_TYPE_PRIMITIVE &&
            base->primitive.token->type == TOK_BOOL;

    if (!typeKindIs(base->kind, TYPE_WRAPPER_MASK) &&
        base->kind != Z_TYPE_NONE && !isBool) {
        zlog(ctx->state, curr->tok, Z3025, stype(base));
        return NULL;
    }

    ZType *success = NULL;
    switch (base->kind) {
    case Z_TYPE_RESULT:     success = base->result.success;     break;
    case Z_TYPE_POINTER:    success = base;                     break;
    case Z_TYPE_OPTIONAL:   success = base->optional;           break;
    default: break;
    }

    switch (curr->unwrap.kind) {
    case UNWRAP_DO: {
        ZType *orelse = resolveType(ctx, curr->unwrap.orExpr, inferred);
        ZCastInfo info = typesCompatible(ctx, orelse, success);
        if (!info.promoted) {
            zlog(ctx->state, curr->unwrap.orExpr->tok,
                Z0009, stype(success), stype(orelse));
            return NULL;
        }
        curr->unwrap.orExpr = implicitCast(ctx, curr->unwrap.orExpr, success);
        return success;
        break;
    }

    case UNWRAP_RETURN:
        analyzeStmt(ctx, curr->unwrap.orExpr);
        if (ctx->currentFuncRet->kind == Z_TYPE_RESULT && isOptional) {
            zlog(ctx->state, curr->tok, Z3026);
            return success;
        } else if (ctx->currentFuncRet->kind == Z_TYPE_RESULT && isResult) {
            ZCastInfo info = typesCompatible(ctx,
                    base->result.error,
                    ctx->currentFuncRet->result.error);
            if (!info.promoted) {
                zlog(ctx->state, curr->tok,
                    Z3027,
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
            zlog(ctx->state, curr->tok, Z3028);
        }
        return success;
    default:
        return success;
    }
}

static ZType *resolveInterpolation(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZNode *writable = LangItems[Z_LANG_WRITABLE];
    ZType *strType = consts(ctx->state, strType);
    if (veclen(curr->interpolation) == 1 && typesEqual(inferred, strType)) {
        if (typesEqual(inferred, strType))
            return strType;
        if (satisfyFacet(ctx, inferred, writable->resolved))
            return writable->resolved;
    }
    for (usize i = 0; i < veclen(curr->interpolation); i++) {
        ZInterpolation *interp = curr->interpolation[i];
        switch (interp->type) {
        case Z_INTERP_EXPR:
            if (!interp->expr) {
                error(ctx->state, curr->tok, "Got an empty expression");
                continue;
            }
            interp->expr->resolved = resolveType(ctx, interp->expr, NULL);
            if (!interp->expr->resolved) continue;
            if (writable &&
                satisfyFacet(ctx, interp->expr->resolved, writable->resolved)) {
                interp->expr = implicitCast(
                    ctx, interp->expr, writable->resolved
                );
            } else {
                zlog(ctx->state,
                    interp->expr->tok, Z305D,
                    stype(interp->expr->resolved)
                );
            }
            break;
        case Z_INTERP_LIT: break;
        }
    }
    ZNode *intstr = LangItems[Z_LANG_INTERPOLATED_STRING];
    if (!intstr) {
        zlog(ctx->state, curr->tok, Z00AA);
        return NULL;
    }
    ZType *arr          = makeTypeThread(ctx, Z_TYPE_ARRAY);
    arr->array.size     = veclen(curr->interpolation);
    arr->array.dynamic  = false;
    arr->array.base     = intstr->resolved;
    arr->tok            = curr->tok;
    return arr;
}

static ZType *_exprAsType(ZThreadSem *ctx, ZNode *curr) {
    if (!curr) return NULL;
    switch (curr->type) {
    case NODE_IDENTIFIER: {
        ZType *type = makeTypeThread(ctx, Z_TYPE_PRIMITIVE);
        type->primitive.token = curr->identNode.tok;
        return type;
    }
    case NODE_UNARY: {
        if (curr->unary.operat->type != TOK_STAR) return NULL;
        ZType *base = _exprAsType(ctx, curr->unary.operand);
        if (!base) return NULL;
        ZType *ptr  = makeTypeThread(ctx, Z_TYPE_POINTER);
        ptr->base   = base;
        return ptr;
    }
    case NODE_TYPE: return curr->resolved;
    default: return NULL;
    }
    return NULL;
}

static ZType *exprAsType(ZThreadSem *ctx, ZNode *curr) {
    ZType *res      = _exprAsType(ctx, curr);
    if (!res) {
        zlog(ctx->state, curr->tok, Z305E);
        return NULL;
    }
    ZType *type     = makeTypeThread(ctx, Z_TYPE_TYPE);
    type->typeType  = res;
    return type;
}

/*
 * Resolve the type of any expression node and cache the result in node->resolved.
 * Returns the resolved ZType* or NULL on error.
 */
ZType *resolveType(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    if (!curr)           return NULL;

    if (typesEqual(inferred, ctx->state->consts.typeType)) {
        return curr->resolved = exprAsType(ctx, curr);
    }

    analyzeAnnotations(ctx->state, curr);
    /* NODE_FUNC always carries a pre-built shape (arg/ret types) set by the
     * parser, so the generic "already resolved" cache check below would skip
     * resolveAnonFunc entirely and leave the mangled name/body unanalyzed. */
    if (curr->type == NODE_FUNC) return resolveAnonFunc(ctx, curr, inferred);
    if (curr->resolved)  {
        if (curr->type == NODE_TYPE && curr->resolved->kind != Z_TYPE_TYPE) {
            ZType *res = makeTypeThread(ctx, Z_TYPE_TYPE);
            res->tok = curr->tok;
            res->typeType = curr->resolved;
            return curr->resolved = res;
        }
        return curr->resolved;
    }

    ZType *result = NULL;

    switch (curr->type) {
    case NODE_BLOCK:        result = resolveBlock           (ctx, curr, inferred);      break;
    case NODE_CALL:         result = resolveFuncCall        (ctx, curr, inferred);      break;
    case NODE_UNARY:        result = resolveUnary           (ctx, curr, inferred);      break;
    case NODE_BINARY:       result = resolveBinary          (ctx, curr, inferred);      break;
    case NODE_MEMBER:       result = resolveMemberAccess    (ctx, curr, inferred);      break;
    case NODE_ARRAY_LIT:    result = resolveArrayLiteral    (ctx, curr, inferred);      break;
    case NODE_SUBSCRIPT:    result = resolveArrSubscript    (ctx, curr, inferred);      break;
    case NODE_IDENTIFIER:   result = resolveIdent           (ctx, curr, inferred);      break;
    case NODE_STRUCT_LIT:   result = resolveStructLit       (ctx, curr, inferred);      break;
    case NODE_TUPLE_LIT:    result = resolveTupleLiteral    (ctx, curr, inferred);      break;
    case NODE_SLICE:        result = resolveSlice           (ctx, curr, inferred);      break;
    case NODE_IF:           result = resolveIf              (ctx, curr, inferred);      break;
    case NODE_UNWRAP:       result = resolveUnwrap          (ctx, curr, inferred);      break;
    case NODE_INTERPOLATION:result = resolveInterpolation   (ctx, curr, inferred);      break;
    case NODE_LITERAL:      result = resolveLiteralType     (ctx, curr->literalTok, inferred);    break;
    case NODE_RANGE: {
        ZType *left     = resolveType(ctx, curr->binary.left, inferred);
        ZType *right    = resolveType(ctx, curr->binary.right, inferred);

        if (!isPrimitive(left) && !isInteger(left->primitive.token)) {
            zlog(ctx->state, curr->binary.left->tok, Z3029);
        }

        if (!isPrimitive(right) && !isInteger(right->primitive.token)) {
            zlog(ctx->state, curr->binary.right->tok, Z3029);
        }

        ZCastInfo info = typesCompatible(ctx, left, right);
        if (!info.promoted) {
            zlog(ctx->state, curr->tok, Z302A, stype(left), stype(right));
        } else {
            result = info.promoted;
            curr->binary.left = implicitCast(ctx, curr->binary.left, info.promoted);
            curr->binary.right = implicitCast(ctx, curr->binary.right, info.promoted);
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
        result = resolveTypeRef(ctx, curr->castExpr.info.to);

        if (expr && expr->kind == Z_TYPE_ARRAY &&
            result->kind == Z_TYPE_ARRAY) {
            // result->array.size = expr->array.size;
        }

        ZType *castFrom = expr;
        if (isOptionalPointer(castFrom) && result && result->kind == Z_TYPE_POINTER)
            castFrom = castFrom->optional;

        ZCastInfo cast = typesCompatible(ctx, castFrom, result);

        /* An array literal can be casted element by element into a fixed
         * array big enough to hold it. */
        if (cast.kind == Z_CAST_INVALID                     &&
            curr->castExpr.expr->type == NODE_ARRAY_LIT     &&
            expr && expr->kind == Z_TYPE_ARRAY              &&
            result && result->kind == Z_TYPE_ARRAY          &&
            !result->array.dynamic                          &&
            result->array.size >= expr->array.size          &&
            typesCompatible(ctx, expr->array.base, result->array.base).kind
                == Z_CAST_NUMERIC) {
            cast.kind       = Z_CAST_ARRAY_LIT;
            cast.promoted   = result;
        }

        if (!cast.promoted) {
            zlog(ctx->state, curr->tok, Z302B, stype(expr), stype(result));
        }

        cast.to = result;
        setCastInfo(ctx, curr, cast);
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
            zlog(ctx->state, curr->tok, Z302C);
        }
        break;
    case NODE_CONTINUE:
        if (ctx->loopDepth == 0) {
            zlog(ctx->state, curr->tok, Z302D);
        }
        break;

    default:
        zlog(ctx->state, curr->tok, Z9007, curr->type);
        break;
    }

    curr->resolved  = result;
    if (result && !result->hash) result->hash    = hashType(result);

    ZAnnotation *print_ast = NULL;
    if (( print_ast = query(curr->annotations, "print_ast"))) {
        printNode(curr, 1);
    }
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
        zlog(ctx->state, curr->tok, Z302E);
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
        zlog(ctx->state, field, Z000E, stoken(field), stype(type));
        return NULL;
    }

    zlog(ctx->state, field, Z3030, stoken(field), stype(type));
    return NULL;
}

#if Z_LSP
static ZThreadSem *findLocalSemantic(ZSemantic *ctx, ZFile *module_file) {
    for (usize i = 0; i < veclen(ctx->scopes); i++)
        if (ctx->scopes[zfile_id(module_file)]->file == module_file)
            return ctx->scopes[i]->ctx;
    return NULL;
}

// #else
static void listStatic(ZThreadSem *ctx, ZType *type, ZMemberFn fn, void *ud) {
    if (!type) return;
    type = resolveTypeRef(ctx, type);
    ZFuncTable *table = resolveFuncTable(ctx, type);

    if (table) for (usize i = 0; i < veclen(table->staticFuncDef); i++) {
        ZNode *funcDef = table->staticFuncDef[i];
        fn(
            stoken(funcDef->funcDef.name),
            funcDef->resolved,
            funcDef,
            Z_LSP_METHOD,
            ud
        );
    }
    if (type->kind != Z_TYPE_ENUM) return;
    ZNode **variants = type->enm.fields;
    for (usize i = 0; i < veclen(variants); i++) {
        ZNode *variant = variants[i];
        fn(
            stoken(variant->resolved->strct.name),
            variant->resolved,
            variant,
            Z_LSP_ENUM_MEMBER,
            ud
        );
    }
}

static void listScope(ZScope *scope, ZMemberFn fn, void *ud) {
    for (usize i = 0; i < veclen(scope->symbols); i++) {
        ZSymbol *sym = scope->symbols[i];
        fn(
            stoken(sym->name),
            sym->type,
            sym->node,
            completion_kind_from_symbol(sym->kind),
            ud
        );
    }
}

static void listForeign(ZNode *node, ZMemberFn fn, void *ud) {
    for (usize i = 0; i < veclen(node->block); i++) {
        ZNode *foreign = node->block[i];
        fn(
            stoken(foreign->foreignDecl.name),
            foreign->resolved,
            foreign,
            Z_LSP_FUNCTION,
            ud
        );
    }
}

static void dispatchFuncTable(ZFuncTable *table, ZMemberFn fn, void *ud) {
    if (table) for (usize i = 0; i < veclen(table->funcDef); i++) {
        ZNode *f = table->funcDef[i];
        fn(
            stoken(f->funcDef.name),
            f->resolved,
            f,
            Z_LSP_METHOD,
            ud
        );
    }
}

static void listInstance(ZThreadSem *ctx, ZType *type, ZMemberFn fn, void *ud) {
    if (!type) return;
    type = resolveTypeRef(ctx, type);
    ZFuncTable *table = resolveFuncTable(ctx, type);
    dispatchFuncTable(table, fn, ud);

    ZType *derefType = type;
    while (derefType && derefType->kind == Z_TYPE_POINTER) {
        derefType = derefType->base;
        table = resolveFuncTable(ctx, derefType);
        dispatchFuncTable(table, fn, ud);
    }

    if (derefType != type) {
        type = derefType;
    }

    switch (type->kind) {
    case Z_TYPE_ENUM: {
        ZTokenType t = TOK_U8;
        if (type->enm.integer) t = type->enm.integer->type;
        fn("tag", makePrimitiveType(ctx->allocator, t), NULL, Z_LSP_FIELD, ud);
        break;
    }
    case Z_TYPE_STRUCT:
        for (usize i = 0; i < veclen(type->strct.fields); i++) {
            ZNode *field = type->strct.fields[i];
            fn(
                stoken(field->tok),
                field->resolved,
                NULL,
                Z_LSP_FIELD,
                ud
            );
        }
        break;

    case Z_TYPE_ARRAY:
        fn("len", consts(ctx->state, u64Type), NULL, Z_LSP_FIELD, ud);
        fn("ptr", type->array.base, NULL, Z_LSP_FIELD, ud);
        break;

    case Z_TYPE_FACET:
        for (usize i = 0; i < veclen(type->facet.funcs); i++) {
            ZNode *func = type->facet.funcs[i];
            fn(
                stoken(func->field.identifier),
                func->resolved,
                NULL,
                Z_LSP_FIELD,
                ud
            );
        }
        break;

    case Z_TYPE_TUPLE: {
        char buff[32];
        for (usize i = 0; i < veclen(type->tuple); i++) {
            int len = snprintf(buff, 31, "%zu", i);
            buff[len] = '\0';
            fn(zstrdup(ctx->allocator, buff), type->tuple[i], NULL, Z_LSP_FIELD, ud);
        }
        break;
    }
    default: break;
    }
}

void zforEachMember(ZSemantic *ctx,
                    ZFile *file,
                    ZNode *member,
                    ZMemberFn fn,
                    void *ud) {
    if (!ctx || !file || !member || member->type != NODE_MEMBER) return;
    ZThreadSem *sem = findLocalSemantic(ctx, file);
    if (!sem) return;

    ZNode *obj = member->memberAccess.object;

    if (obj->type == NODE_IDENTIFIER && strcmp(stoken(obj->tok), ".") == 0) {
        listStatic(sem, obj->resolved, fn, ud); return;
    }

    ZSymbol *sym = nodeSymbol(obj);

    if (sym) switch (sym->kind) {
    case Z_SYM_STRUCT:
    case Z_SYM_ENUM:
    case Z_SYM_TYPEDEF: listStatic      (sem, sym->type,    fn, ud);    return;
    case Z_SYM_IMPORT:  listScope       (sym->scope,   fn, ud);    return;
    case Z_SYM_FOREIGN: listForeign     (sym->node,    fn, ud);    return;
    default:            listInstance    (sem, obj->resolved,fn, ud);    return;
    } else              listInstance    (sem, obj->resolved,fn, ud);    return;
}
#endif

static ZType *resolveIntrinsicAccess(ZThreadSem *ctx, ZType *objType, ZToken *field) {
    if (objType->kind == Z_TYPE_ARRAY) {
        if (strcmp(field->str, "len") == 0)
            return consts(ctx->state, u64Type);
        if (strcmp(field->str, "ptr") == 0) {
            ZType *ptr  = makeTypeThread(ctx, Z_TYPE_POINTER);
            ptr->tok    = field;
            ptr->base   = objType->array.base;
            return ptr;
        }
    }

    if (objType->kind == Z_TYPE_ENUM && strcmp(field->str, "tag") == 0) {
        ZTokenType t = objType->enm.integer ?
            objType->enm.integer->type : TOK_U8;
        return makePrimitiveType(ctx->allocator, t);
    }

    return NULL;
}

static ZType *resolveMemberAccess(ZThreadSem *ctx, ZNode *curr, ZType *inferred) {
    ZType *objType  = NULL;
    ZToken *field   = curr->memberAccess.field;

    if (curr->memberAccess.object->type == NODE_IDENTIFIER &&
        strcmp(curr->memberAccess.object->tok->str, ".") == 0) {
        if (!inferred) {
            zlog(ctx->state, curr->tok, Z3031);
            return NULL;
        }

        if (inferred->kind == Z_TYPE_OPTIONAL)
            inferred = inferred->optional;

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

    #if Z_LSP
    if (strcmp(stoken(field), DEFAULT_MEMBER_ACCESS) == 0) return NULL;
    #endif

    if (objSym) {
        if (!curr->memberAccess.object->resolved)
            curr->memberAccess.object->resolved = consts(ctx->state, modType);
        switch (objSym->kind) {
        case Z_SYM_IMPORT: {
            ZSymbol *sym = resolveByScope(objSym->scope, field);
            if (!sym) {
                zlog(ctx->state, field,
                    Z3032,
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
            zlog(ctx->state, field, Z3033, stoken(field), stoken(objSym->name));
            return NULL;
        }
        default: break;
        }
    }

    if (!objType) {
        zlog(ctx->state, curr->tok, Z3034);
        return NULL;
    }

    ZType *base = derefType(objType);



    if (!base) {
        zlog(ctx->state, curr->tok, Z302E);
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
        zlog(ctx->state, field, Z3035, field->str, stype(base));
        return NULL;
    } else if (base->kind == Z_TYPE_TUPLE) {
        usize len = veclen(base->tuple);

        if (field->type != TOK_INT_LIT) {
            zlog(ctx->state, field, Z3036);
            return NULL;
        }

        if (field->integer < 0 || field->integer >= (i64)len) {
            zlog(ctx->state, field, Z3037);
            return NULL;
        }

        return base->tuple[field->integer];
    } else if (objType->kind == Z_TYPE_FACET) {
        ZType *func = NULL;
        ZNode *node = resolveStaticFuncTable(ctx, objType, field);
        if (node) return node->resolved;

        for (usize i = 0; i < veclen(objType->facet.funcs); i++) {
            ZNode *funcField = objType->facet.funcs[i];
            if (tokeneq(field, funcField->field.identifier)) {
                func = funcField->resolved;
                break;
            }
        }

        if (!func) {
            zlog(ctx->state, curr->tok, Z3038, stype(objType), stoken(field));
            return NULL;
        }

        return func;
    } else {
        ZNode *resolved = resolveFuncCallEmbedded(ctx, curr, objType, field);
        if (!resolved) {
            ZType *res = resolveIntrinsicAccess(ctx, base, field);
            if (res) return res;
            zlog(ctx->state, curr->tok, Z3039);
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
        zlog(ctx->state, curr->tok, Z303A);
        return NULL;
    }

    if (!indexType || indexType->kind != Z_TYPE_PRIMITIVE ||
        !isInteger(indexType->primitive.token)) {
        zlog(ctx->state, curr->tok, Z303B);
        return NULL;
    }

    if (arrType->kind == Z_TYPE_ARRAY) {
        return arrType->array.base;
    } else if (arrType->kind == Z_TYPE_POINTER) {
        return arrType->base;
    }
    return NULL;
}

bool satisfyFacet(ZThreadSem *ctx, ZType *type, ZType *facet) {
    type = resolveOperand(ctx, type);
    if (!type) return false;
    if (type->kind != Z_TYPE_POINTER) {
        // zlog(ctx->state, type->tok, Z303C);
        return false;
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
