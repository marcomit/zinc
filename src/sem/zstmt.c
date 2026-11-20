#include "zsem.h"


/* ================== Statement analysis ================== */

void analyzeVar(ZThreadSem *ctx, ZNode *curr, bool isGlobal) {
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
        ZCastInfo cast = typesCompatible(ctx, rvalueType, declaredType);
        if (rvalueType &&
            !cast.promoted) {

            if (declaredType->kind == Z_TYPE_FACET) {
                if (!satisfyFacet(ctx, rvalueType, declaredType)) {
                    zlog(ctx->state, curr->tok,  Z303D);
                }
            } else {
                zlog(ctx->state, curr->tok,
                    Z303E,
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

    // Uninitialized value must be zeroable.
    if (!curr->varDecl.uninit && !curr->varDecl.rvalue && !isZeroable(ctx, declaredType)) {
        zlog(ctx->state, curr->tok, Z305B, stype(declaredType));
    }

    curr->resolved = declaredType;

    if (curr->varDecl.rvalue && rvalueType && declaredType &&
            declaredType->kind == Z_TYPE_ARRAY &&
            declaredType->array.size > 0 &&
            rvalueType->kind == Z_TYPE_ARRAY) {
        if (rvalueType->array.size > declaredType->array.size) {
            zlog(ctx->state, curr->varDecl.rvalue->tok,
                Z303F,
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

    if (!isIfLet && !isComparable(ctx, cond)) {
        zlog(ctx->state, curr->ifStmt.cond->tok, Z3040, stype(cond));
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
            zlog(ctx->state, curr->whileStmt.cond->tok, Z3021);
        }

        // curr->whileStmt.cond = implicitCast(
        //     ctx, curr->whileStmt.cond, consts(ctx->state, u1Type));
    }

    ctx->loopDepth++;
    analyzeBlock(ctx, curr->whileStmt.branch, isForLet);
    ctx->loopDepth--;
    if (isForLet) endScope(ctx);
}

void analyzeForeign(ZThreadSem *ctx, ZNode *curr) {
    beginScope(ctx, curr);
    ZType **generics = curr->resolved->func.generics;
    for (usize i = 0; i < veclen(generics); i++) {
        putGeneric(ctx, generics[i]);
    }
    curr->resolved = resolveTypeRef(ctx, curr->resolved);
    endScope(ctx);
}

void analyzeFuncArgs(ZThreadSem *ctx, ZType **types, ZNode **fields, ZType **inferred) {
    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *field        = fields[i];
        if (!field || field->type == NODE_FUNC_FIELD) continue;
        ZType *fieldType    = resolveTypeRef(ctx, field->resolved);

        if (!fieldType) {
            if (inferred) fieldType = inferred[i];
            else {
                zlog(ctx->state, field->tok, Z9008);
                continue;
            }
        }
        if (inferred) {
            inferred[i] = resolveTypeRef(ctx, inferred[i]);
            if (!typesEqual(inferred[i], fieldType)) {
                zlog(ctx->state, field->tok,
                    Z0009,
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

void analyzeFunc(ZThreadSem *ctx, ZNode *curr) {
    for (usize i = 0; i < veclen(curr->funcDef.generics); i++) {
        putGeneric(ctx, curr->funcDef.generics[i]);
    }

    beginScope(ctx, curr);
    if (curr->funcDef.body) {
        curr->funcDef.body->scope = ctx->current;
    }
    if (curr->funcDef.base) {
        ZType *res = resolveTypeRef(ctx, curr->funcDef.base);
        if (!res) {
            zlog(ctx->state,
                    curr->funcDef.base->primitive.token,
                    Z3041,
                    curr->funcDef.base->primitive.token->str);
            return;
        }
    }

    if (curr->funcDef.receiver) {
        ZNode *receiver = curr->funcDef.receiver;
        ZType *recType  = resolveTypeRef(ctx, receiver->resolved);
        receiver->resolved = recType;

        ZSymbol *sym = makesymbol(ctx->allocator, Z_SYM_VAR);
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

    curr->resolved              = resolveTypeRef(ctx, curr->resolved);
    ctx->currentFuncRet         = curr->resolved->func.ret;
    ctx->currentFunc            = curr;

    analyzeBlock(ctx, curr->funcDef.body, false);
    if (!isVoid(ctx->currentFuncRet) &&
        !satisfyReturn(ctx, curr->funcDef.body)) {
        zlog(ctx->state, curr->tok, Z3042);
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

    if (curr->returnStmt.expr) {
        retType     = resolveType(ctx, curr->returnStmt.expr, ctx->currentFuncRet);
        if (!retType) {
            zlog(ctx->state, curr->tok, Z3043);
            return;
        }
    }

    curr->resolved  = retType;

    if (!ctx->currentFuncRet) return;

    bool sumAcceptVoid =
        ctx->currentFuncRet->kind == Z_TYPE_SUM &&
        sumTypeIndexOf(ctx->currentFuncRet, consts(ctx->state, u0Type)) != -1;

    bool isVoidRet  = isVoid(retType);
    bool isVoid     = isType(ctx->currentFuncRet, TOK_VOID);

    if (isVoid && !isVoidRet) {
        zlog(ctx->state, ctx->currentFunc->tok, Z3044, stype(retType));
    } else if (!isVoid && !sumAcceptVoid && isVoidRet) {
        zlog(ctx->state, ctx->currentFunc->tok, Z3045, ctx->currentFuncRet);
    } else if (!isVoid && !sumAcceptVoid && !isVoidRet) {
        ZCastInfo cast = typesCompatible(
            ctx, retType, ctx->currentFuncRet
        );

        if (ctx->currentFuncRet->kind == Z_TYPE_OPTIONAL) {
            curr->returnStmt.expr = implicitCast(ctx,
                curr->returnStmt.expr,
                ctx->currentFuncRet
            );
            curr->resolved = ctx->currentFuncRet;

            if (retType->kind == Z_TYPE_NONE ||
                typesCompatible(ctx, retType, ctx->currentFuncRet->optional).promoted)
                return;
        } else if (ctx->currentFuncRet->kind == Z_TYPE_RESULT) {

        }

        if (!cast.promoted) {
            zlog(ctx->state, curr->tok,
                Z0009,
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
        curr->resolved = consts(ctx->state, u0Type);
    }
}

static void analyzeCapability(ZThreadSem *ctx, ZNode *curr) {
    bool isInline = curr->capability.isInline;

    if (!isInline) beginScope(ctx, curr);

    ZNode **capabilities = curr->capability.capabilities;
    for (usize i = 0; i < veclen(capabilities); i++) {
        resolveType(ctx, capabilities[i], NULL);
        putCapability(ctx, capabilities[i]);
    }

    if (!isInline) {
        analyzeStmt(ctx, curr->capability.block);
        endScope(ctx);
    }
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
            zlog(ctx->state, arm->tok, Z3046);
            continue;
        }

        for (usize j = 0; j < i; j++) {
            if (patternEq(ctx->state,
                arms[i]->matchArm.pattern,
                arms[j]->matchArm.pattern)) {
                zlog(ctx->state, arms[i]->matchArm.pattern->tok, Z3047, j, i);
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
            zlog(ctx->state, curr->forin.iter->tok, Z3048, stype(iter));
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
            zlog(ctx->state, funcRef->tok,
                Z3049,
                veclen(funcRef->resolved->func.args)
            );
        }
        itemType = funcRef->resolved->func.ret;

        if (itemType->kind != Z_TYPE_OPTIONAL) {
            zlog(ctx->state, funcRef->tok, Z304A, stype(itemType));
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
            zlog(ctx->state, binding->tok, Z304B);

        putVarPattern(ctx, curr, elemType, binding->tuple[0], false);
        if (veclen(binding->tuple) == 2) {
            putVarPattern(ctx, curr, consts(ctx->state, u64Type), binding->tuple[1], false);
        }
    } else {
        putVarPattern(ctx, curr, elemType, curr->forin.binding, true);
    }

    analyzeBlock(ctx, curr->forin.body, false);
    ctx->loopDepth--;

    endScope(ctx);
}

void analyzeStmt(ZThreadSem *ctx, ZNode *curr) {
    analyzeAnnotations(ctx->state, curr);
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

void analyzeBlock(ZThreadSem *ctx, ZNode *block, bool scoped) {
    if (scoped) beginScope(ctx, block);

    ZNode **stmts       = block->block;
    usize len           = veclen(stmts);
    ZAnnotation **anns  = block->annotations;

    if (len == 0 && !query(anns, "allow_empty")) {
        warning(ctx->state, block->tok, "Block cannot be empty");
    }

    for (usize i = 0; i < len; i++) {
        if (i + 1 < len &&
            (stmts[i]->type == NODE_BREAK ||
            stmts[i]->type == NODE_CONTINUE ||
            stmts[i]->type == NODE_RETURN)) {
            zlog(ctx->state, stmts[i+1]->tok, Z304C);

            vecsetlen(block->block, i);

            break;
        }
        analyzeStmt(ctx, stmts[i]);
    }

    if (scoped) endScope(ctx);
}
