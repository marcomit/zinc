#include "zvisit.h"

#define visitChild(n)      if (!visitNode(ctx, (n))) return false
#define visitChildren(v)   if (!visitNodeList(ctx, (v), 0)) return false

static bool visitNodeList(ZVisitor *ctx, ZNode **list, usize from) {
    for (usize i = from; i < veclen(list); i++) {
        if (!visitNode(ctx, list[i])) return false;
    }
    return true;
}

bool visitNode(ZVisitor *ctx, ZNode *node) {
    if (!node) return true;

    ZVisitorState state = ctx->visit(node, ctx->clientData);
    if (state == Z_STOP)  return false;
    if (state == Z_BREAK) return true;

    switch (node->type) {
    case NODE_MODULE:
        for (usize i = 0; i < veclen(node->module.root); i++) {
            ZNode *child = node->module.root[i];
            if (child && child->type == NODE_MODULE) continue;
            visitChild(child);
        }
        break;
    case NODE_FUNC:
        visitChildren(node->funcDef.args);
        visitChildren(node->funcDef.capabilities);
        visitChild(node->funcDef.body);
        break;
    case NODE_STRUCT:
        visitChildren(node->structDef.fields);
        break;
    case NODE_ENUM:
        visitChildren(node->enumDef.fields);
        break;
    case NODE_ENUM_FIELD:
        if (node->resolved && node->resolved->kind == Z_TYPE_STRUCT) {
            if (!visitNodeList(ctx, node->resolved->strct.fields, 1)) return false;
        }
        break;
    case NODE_NAMESPACE:
    case NODE_BLOCK:
        visitChildren(node->block);
        break;
    case NODE_FACET:
        visitChildren(node->facet.funcs);
        break;
    case NODE_IMPL:
        visitChildren(node->impl.capabilities);
        visitChildren(node->impl.funcs);
        break;
    case NODE_IF:
        visitChild(node->ifStmt.cond);
        visitChild(node->ifStmt.body);
        visitChild(node->ifStmt.elseBranch);
        break;
    case NODE_WHILE:
        visitChild(node->whileStmt.cond);
        visitChild(node->whileStmt.branch);
        break;
    case NODE_FORIN:
        visitChild(node->forin.iter);
        visitChild(node->forin.body);
        break;
    case NODE_MATCH:
        visitChild(node->match.cond);
        visitChildren(node->match.arms);
        break;
    case NODE_MATCH_ARM:
        visitChild(node->matchArm.expr);
        break;
    case NODE_CAPABILITY:
        visitChildren(node->capability.capabilities);
        visitChild(node->capability.block);
        break;
    case NODE_VAR_DECL:
        visitChild(node->varDecl.rvalue);
        break;
    case NODE_RETURN:
        visitChild(node->returnStmt.expr);
        break;
    case NODE_BREAK:
        visitChild(node->breakStmt.expr);
        break;
    case NODE_DEFER:
        visitChild(node->deferStmt.expr);
        break;
    case NODE_RANGE:
    case NODE_BINARY:
        visitChild(node->binary.left);
        visitChild(node->binary.right);
        break;
    case NODE_UNARY:
        visitChild(node->unary.operand);
        break;
    case NODE_UNWRAP:
        visitChild(node->unwrap.base);
        visitChild(node->unwrap.orExpr);
        break;
    case NODE_ENUM_LIT:
    case NODE_CALL:
        visitChild(node->call.callee);
        visitChildren(node->call.args);
        break;
    case NODE_ENUM_LIT_NO_PAYLOAD:
    case NODE_MEMBER:
        visitChild(node->memberAccess.object);
        break;
    case NODE_SUBSCRIPT:
        visitChild(node->subscript.arr);
        visitChild(node->subscript.index);
        break;
    case NODE_SLICE:
        visitChild(node->slice.base);
        visitChild(node->slice.start);
        visitChild(node->slice.end);
        break;
    case NODE_CAST:
        visitChild(node->castExpr.expr);
        break;
    case NODE_TUPLE_LIT:
        visitChildren(node->tuplelit);
        break;
    case NODE_ARRAY_LIT:
        visitChildren(node->arraylit);
        break;
    case NODE_STRUCT_LIT:
        visitChildren(node->structlit.fields);
        break;
    case NODE_INTERPOLATION:
        for (usize i = 0; i < veclen(node->interpolation); i++) {
            ZInterpolation *interp = node->interpolation[i];
            if (interp && interp->type == Z_INTERP_EXPR) visitChild(interp->expr);
        }
        break;
    case NODE_LITERAL:
    case NODE_IDENTIFIER:
    case NODE_FIELD:
    case NODE_EMBED_FIELD:
    case NODE_TYPEDEF:
    case NODE_FOREIGN:
    case NODE_MACRO:
    case NODE_TYPE:
    case NODE_SIZEOF:
    case NODE_ARRAY_INIT:
    case NODE_CONTINUE:
    case NODE_TYPE_COUNT:
        break;
    }
    return true;
}
