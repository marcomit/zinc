#include "zparse.h"

ZNode *parseCapabilityBlock(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_WITH);

    ZNode **capabilities    = NULL;
    ZNode *capability       = NULL;
    do {
        capability = parseOrGrammar(parser, (ZParseFunc[]){
            parseVarDef, parseExpr
        }, 2);
        if (!capability) {
            zlog(parser->state, peek(parser), Z2013);
            return NULL;
        }
        vecpush(capabilities, capability);
    } while (!check(parser, TOK_LBRACKET)   &&
             !check(parser, TOK_DO)         &&
              match(parser, TOK_SEMICOLON)  );

    ZToken *cur = peek(parser);
    ZNode *block = NULL;
    if (!cur->newlineBefore) {
        block = parseBlockOrInline(parser, false);
    }

    ZNode *capabilityBlock                      = makenode(parser->allocator, NODE_CAPABILITY);
    capabilityBlock->capability.capabilities    = capabilities;
    capabilityBlock->capability.block           = block;
    capabilityBlock->capability.isInline        = block == NULL ? true : false;
    return setspan(parser, capabilityBlock, start);
}

ZNode *parseCompoundOperator(ZParser *parser) {
    ZNode *left = parseExpr(parser);
    if (!left) return NULL;

    if (!checkMask(parser, TOK_SELF_OPERATOR)) return left;
    ZToken *op = consume(parser);

    ZNode *right = parseExpr(parser);

    if (!right) return NULL;

    ZNode *node         = makenode(parser->allocator, NODE_BINARY);
    node->binary.left   = left;
    node->binary.right  = right;
    node->binary.op     = op;
    node->tok           = op;
    return setspanfrom(parser, node, left);
}

ZNode *parseStmt(ZParser *parser) {
    guard(canPeek(parser));

    ZAnnotation **annotations   = NULL;
    ZNode *res                  = NULL;

    if (check(parser, TOK_HASHTAG)) {
        annotations = parseAnnotations(parser);
        guard(canPeek(parser));
    }

    /* Read after the annotations, so '#comp for ...' dispatches on 'for'. */
    ZTokenType t                = peek(parser)->type;

    if (check(parser, TOK_IDENT) &&
        checkAhead(parser, TOK_DOUBLE_COLON, 1)) {
        ZToken *next = peekAhead(parser, 2);
        if (!next) return NULL;
        switch (next->type) {
        case TOK_STRUCT:    res = parseStructDecl  (parser, NULL, false);   break;
        case TOK_ENUM:      res = parseEnumDecl    (parser, NULL, false);   break;
        case TOK_TYPEDEF:   res = parseTypedef     (parser, NULL, false);   break;
        case TOK_FOREIGN:   res = parseForeign     (parser, NULL, false);   break;
        default: break;
        }
    } else {
        switch (t) {
        case TOK_IF:        res = parseIf              (parser);        break;
        case TOK_FOR:       res = parseLoops           (parser);        break;
        case TOK_MATCH:     res = parseMatch           (parser, false); break;
        case TOK_DEFER:     res = parseDefer           (parser);        break;
        case TOK_BREAK:     res = parseBreak           (parser);        break;
        case TOK_CONTINUE:  res = parseContinue        (parser);        break;
        case TOK_WITH:      res = parseCapabilityBlock (parser);        break;
        case TOK_RETURN:
            if (parser->noReturnStmt) {
                zlog(parser->state, peek(parser), Z2014);
                return NULL;
            }
            return parseReturn          (parser);
        default: {
            ZParseFunc funcs[] = {
                parseVarInferred,
                parseVarDefTyped,
                parseBlock,
                parseUpdate,
                parseCompoundOperator,
                parseExpr
            };
            res = parseOrGrammar(parser, funcs, arrlen(funcs));
            break;
        }
        }
    }

    if (res) res->annotations = annotations;
    return res;
}

ZNode *parseVarInferred(ZParser *parser) {
    ZToken *start = peek(parser);
    ZVarDestructPattern *pattern = parseMultiDestructVar(parser);

    bool constant = false;
    if (match(parser, TOK_DOUBLE_COLON)) constant = true;
    else if (!match(parser, TOK_ASSIGN)) return NULL;

    ZNode *expr = parseMultiExpr(parser);

    if (!expr) {
        zlog(parser->state, peek(parser), Z202B);
    }

    ZNode *var = makenodevar(parser->allocator, pattern, NULL, expr);
    var->varDecl.constant = constant;
    return setspan(parser, var, start);
}

ZNode *parseVarDefTyped(ZParser *parser) {
    ZToken *start = peek(parser);

    if (!check(parser, TOK_IDENT) && !check(parser, TOK_LPAREN) && !check(parser, TOK_LBRACKET)) {
        zlog(parser->state, start, Z202C);
        return NULL;
    }

    ZVarDestructPattern *var = parseDestructVar(parser, false);

    expect(parser, TOK_COLON);

    ZType *type = parseType(parser);
    guard(type);

    ZNode *expr = NULL;
    bool uninit = false;

    /* No '=' at all: zero-initialized by default.
     * '= ?'         : explicitly left uninitialized.
     * '= <expr>'    : normal initializer. */
    if (match(parser, TOK_EQ)) {
        if (match(parser, TOK_OPT)) {
            uninit = true;
        } else {
            expr = parseMultiExpr(parser);
            if (!expr) {
                zlog(parser->state, peek(parser), Z202D);
                return NULL;
            }
        }
    }

    ZNode *node = makenodevar(parser->allocator, var, type, expr);
    if (node) node->varDecl.uninit = uninit;
    return setspan(parser, node, start);
}

ZNode *parseVarDef(ZParser *parser) {
    ZParseFunc func[] = { parseVarInferred, parseVarDefTyped };
    return parseOrGrammar(parser, func, 2);
}


ZNode *parseDefer(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_DEFER);
    parser->noReturnStmt = true;
    ZNode *expr = parseStmt(parser);
    parser->noReturnStmt = false;

    ensure(expr, "Expected an expression after 'defer' keyword");

    ZNode *node = makenode(parser->allocator, NODE_DEFER);
    node->deferStmt.expr = expr;
    return setspan(parser, node, start);
}


static ZNode *parseMatchArm(ZParser *parser, bool asExpr) {
    ZToken *start = peek(parser);
    ZVarDestructPattern *pattern = parseDestructVar(parser, true);
    guard(pattern);

    ZNode *arm              = makenode(parser->allocator, NODE_MATCH_ARM);

    if (asExpr && !check(parser, TOK_ARROW)) {
        zlog(parser->state, peek(parser), Z2010);
        while ( canPeek(parser)                 &&
                (!check(parser, TOK_LBRACKET)   ||
                !check(parser, TOK_COMMA)       )) {
            consume(parser);
        }
        return NULL;
    }
    if (match(parser, TOK_ARROW)) {
        arm->matchArm.expr  = parseExpr(parser);
    } else if (check(parser, TOK_LBRACKET)) {
        arm->matchArm.expr  = parseBlock(parser);
    }
    arm->matchArm.pattern   = pattern;
    return setspan(parser, arm, start);
}


ZNode *parseMatch(ZParser *parser, bool asExpr) {
    ZToken *start = peek(parser);
    expect(parser, TOK_MATCH);


    parser->noStructLit = true;
    ZNode *expr = parseExpr(parser);
    parser->noStructLit = false;

    ensure(expr, "Expected an expression");

    expect(parser, TOK_LBRACKET);
    ZNode **arms        = NULL;
    ZNode *arm          = NULL;
    do {
        arm = tryParse(parser, parseMatchArm(parser, asExpr));
        if (!arm) break;
        vecpush(arms, arm);
    } while (!check(parser, TOK_RBRACKET) && match(parser, TOK_COMMA));
    if (!match(parser, TOK_RBRACKET)) {
        zlog(parser->state, peek(parser), Z2011);
        sinchronize(parser, TOK_LBRACKET, TOK_RBRACKET);
    }

    ZNode *match        = makenode(parser->allocator, NODE_MATCH);
    match->tok          = start;
    match->match.cond   = expr;
    match->match.arms   = arms;
    return setspan(parser, match, start);
}


ZNode *parseBlockOrInline(ZParser *parser, bool wrap) {
    ZToken *start = peek(parser);
    if (match(parser, TOK_DO)) {
        ZNode *expr = parseExpr(parser);
        if (!expr) {
            zlog(parser->state, peek(parser), Z2013);
            return NULL;
        }
        if (!wrap) return expr;
        ZNode *body = makenode(parser->allocator, NODE_BLOCK);
        body->tok = start;
        vecpush(body->block, expr);
        return setspan(parser, body, start);
    } else if (check(parser, TOK_LBRACKET) || check(parser, TOK_HASHTAG)) {
        return parseBlock(parser);
    } else {
        zlog(parser->state, peek(parser), Z2015);
        return NULL;
    }
}


ZNode *parseBlock(ZParser *parser) {
    ZToken *start   = peek(parser);

    ZAnnotation **annotations = NULL;
    if (check(parser, TOK_HASHTAG)) {
        annotations = parseAnnotations(parser);
    }
    expect(parser, TOK_LBRACKET);
    ZNode *block        = makenode(parser->allocator, NODE_BLOCK);
    block->tok          = start;
    block->annotations  = annotations;
    ZNode *stmt         = NULL;
    do {
        stmt = parseStmt(parser);
        if (stmt) vecpush(block->block, stmt);
        if (check(parser, TOK_RBRACKET)) break;
    } while (stmt);

    if (!check(parser, TOK_RBRACKET)) {
        ZToken *cur = peek(parser);
        zlog(parser->state, cur, Z2016, stoken(cur));
        sinchronize(parser, TOK_LBRACKET, TOK_RBRACKET);
    }

    expect(parser, TOK_RBRACKET);

    return setspan(parser, block, start);
}


ZNode *parseReturn(ZParser *parser) {
    ZToken *start           = peek(parser);
    expect(parser, TOK_RETURN);

    ZNode *ret              = makenode(parser->allocator, NODE_RETURN);
    ZNode *expr             = NULL;
    ZNode **list            = NULL;
    ret->returnStmt.expr    = NULL;
    ret->tok                = start;

    if (!canPeek(parser) || peek(parser)->newlineBefore) return setspan(parser, ret, start);

    do {
        expr = tryParse(parser, parseExpr(parser));
        if (!expr) break;
        vecpush(list, expr);

    } while (match(parser, TOK_COMMA));

    if (veclen(list) > 1) {
        expr                    = makenode(parser->allocator, NODE_TUPLE_LIT);
        expr->tuplelit          = list;
        expr->tok               = start;
        ret->returnStmt.expr    = setspanfrom(parser, expr, list[0]);
    } else {
        ret->returnStmt.expr    = expr;
    }
    return setspan(parser, ret, start);
}


static ZNode *parseCondDestructVar(ZParser *parser) {
    ZToken *start = peek(parser);
    ZVarDestructPattern *pattern = parseDestructVar(parser, true);
    guard(pattern);
    expect(parser, TOK_ASSIGN);
    parser->noStructLit = true;
    ZNode *expr = tryParse(parser, parseExpr(parser));
    parser->noStructLit = false;

    guard(expr);

    return setspan(parser, makenodevar(parser->allocator, pattern, NULL, expr), start);
}


static ZNode *parseIfBlock(ZParser *parser) {
    ZToken *start = peek(parser);
    if (check(parser, TOK_DO)       ||
        check(parser, TOK_LBRACKET) ||
        check(parser, TOK_HASHTAG)  ) {
        return parseBlockOrInline(parser, false);
    }

    ZNode *node = NULL;
    if (check(parser, TOK_BREAK)) {
        node = parseBreak(parser);
    } else if (check(parser, TOK_CONTINUE)) {
        node = parseContinue(parser);
    } else if (check(parser, TOK_RETURN)) {
        node = parseReturn(parser);
    } else if (check(parser, TOK_MATCH)) {
        node = parseMatch(parser, false);
    } else {
        zlog(parser->state, peek(parser), Z201E, stoken(peek(parser)));
    }

    if (!node) return NULL;

    ZNode *block = makenode(parser->allocator, NODE_BLOCK);
    block->block = NULL;

    vecpush(block->block, node);

    return setspan(parser, block, start);
}


static ZNode *parseIfLet(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IF);
    ZToken *bindingStart = peek(parser);
    ZVarDestructPattern *pattern = parseDestructVar(parser, true);
    guard(pattern);
    expect(parser, TOK_ASSIGN);
    bool savedNoStructLit = parser->noStructLit;
    parser->noStructLit = true;
    ZNode *expr = parseExpr(parser);
    parser->noStructLit = savedNoStructLit;
    guard(expr);
    ZNode *var  = makenodevar(parser->allocator, pattern, NULL, expr);
    setspan(parser, var, bindingStart);

    ZNode *body = parseIfBlock(parser);

    ZNode *elseBranch = NULL;
    if (match(parser, TOK_ELSE)) {
        elseBranch = parseOrGrammar(
            parser, (ZParseFunc[]){ parseIf, parseIfBlock }, 2
        );
    }

    ZNode *iflet                = makenode(parser->allocator, NODE_IF);
    iflet->ifStmt.cond          = var;
    iflet->ifStmt.body          = body;
    iflet->ifStmt.elseBranch    = elseBranch;
    return setspan(parser, iflet, start);
}


static ZNode *parseIfStmt(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IF);

    parser->noStructLit = true;
    ZNode *cond = parseExpr(parser);
    parser->noStructLit = false;
    guard(cond);

    ZNode *body = parseIfBlock(parser);
    guard(body);

    ZNode *node = makenode(parser->allocator, NODE_IF);

    if (canPeek(parser) && match(parser, TOK_ELSE)) {
        ZNode *elseBody = parseOrGrammar(parser, (ZParseFunc[]){
            parseIf, parseExpr
        }, 2);
        if (!elseBody) return NULL;
        node->ifStmt.elseBranch = elseBody;
    }

    node->ifStmt.cond   = cond;
    node->ifStmt.body   = body;
    node->tok           = start;
    return setspan(parser, node, start);
}


ZNode *parseIf(ZParser *parser) {
    return parseOrGrammar(parser, (ZParseFunc[]){parseIfLet, parseIfStmt}, 2);
}


static ZNode *parseWhile(ZParser *parser) {
    ZToken *start = peek(parser);

    if (!check(parser, TOK_FOR))
        return NULL;
    consume(parser);

    parser->noStructLit = true;
    ZNode *cond = tryParse(parser, parseExpr(parser));
    parser->noStructLit = false;
    if (!cond) {
        zlog(parser->state, peek(parser), Z201F);
    }
    ZNode *body = tryParse(parser, parseBlockOrInline(parser, true));

    ZNode *node = makenode(parser->allocator, NODE_WHILE);
    node->whileStmt.branch  = body;
    node->whileStmt.cond    = cond;
    return setspan(parser, node, start);
}


static ZNode *parseForIn(ZParser *parser) {
    ZToken *start = peek(parser);

    if (!check(parser, TOK_FOR))
        return NULL;
    consume(parser);

    ZVarDestructPattern *binding = parseMultiDestructVar(parser);
    expect(parser, TOK_IN);
    parser->noStructLit = true;
    ZNode *iter = tryParse(parser, parseExpr(parser));
    parser->noStructLit = false;
    ZNode *block = tryParse(parser, parseBlockOrInline(parser, true));

    guard(binding && iter && block);

    ZNode *node = makenode(parser->allocator, NODE_FORIN);
    node->forin.binding = binding;
    node->forin.iter    = iter;
    node->forin.body    = block;
    node->tok = start;

    return setspan(parser, node, start);
}


static ZNode *parseForLet(ZParser *parser) {
    ZToken *start = peek(parser);

    if (!check(parser, TOK_FOR))
        return NULL;
    consume(parser);

    ZNode *cond = tryParse(parser, parseCondDestructVar(parser));
    ZNode *body = tryParse(parser, parseBlockOrInline(parser, true));
    guard(body);

    ZNode *node = makenode(parser->allocator, NODE_WHILE);
    node->whileStmt.branch  = body;
    node->whileStmt.cond    = cond;
    node->tok               = start;
    return setspan(parser, node, start);
}


ZNode *parseLoops(ZParser *parser) {
    ZToken *start = peek(parser);
    ensure(check(parser, TOK_FOR), "Expected 'for' keyword");

    // Infinite loop without condition
    if (checkAhead(parser, TOK_LBRACKET, 1) ||
        checkAhead(parser, TOK_DO, 1)       ) {
        Allocator *allocator    = parser->allocator;
        ZNode *node             = makenode(allocator, NODE_WHILE);
        node->tok               = consume(parser);
        node->whileStmt.branch  = parseBlockOrInline(parser, true);
        node->whileStmt.cond    = NULL;

        return setspan(parser, node, start);
    }

    ZParseFunc f[] = { parseForIn, parseForLet, parseWhile };
    ZNode *node = parseOrGrammar(parser, f, sizeof(f) / sizeof(f[0]));

    if (node) node->tok = start;

    return node;
}
