#include "zparse.h"

static ZParseFunc exprFunc[] = {
    parseBinary,
    parseTupleLit,
};

static ZNode *parseLit(ZParser *parser) {
    ZToken *start = peek(parser);
    if (!start) return NULL;
    if (start->type != TOK_STREAM) {
        ZNode *node         = makenode(parser->allocator, NODE_LITERAL);
        node->literalTok    = consume(parser);
        node->tok           = start;
        return setspan(parser, node, start);
    }

    consume(parser);
    ZNode *node = makenode(parser->allocator, NODE_INTERPOLATION);
    node->tok = start;
    ZTokenStream *prev = parser->source;
    parser->source = maketokstream(parser->allocator, start->stream, NULL);

    while (canPeek(parser)) {
        ZInterpolation *interp = NULL;
        if (check(parser, TOK_STR_START)) {
            consume(parser);
            ZNode *expr = parseExpr(parser);
            if (!match(parser, TOK_STR_END)) {
                error(parser->state, peek(parser), "Expected the end of the interpolation");
            }
            interp = makeinterpolation(parser->allocator, Z_INTERP_EXPR, expr);
        } else if (check(parser, TOK_STR_LIT)) {
            interp = makeinterpolation(parser->allocator, Z_INTERP_LIT, consume(parser));
        } else {
            // error
            consume(parser); // consume the token to avoid a loop.
        }

        if (!interp) {
            // error
        }

        vecpush(node->interpolation, interp);
    }

    /* The parts were consumed from the inner stream: the literal ends at the stream token. */
    parser->source = prev;
    parser->last   = start;
    return setspan(parser, node, start);
}

ZNode *parsePrimary(ZParser *parser) {
    ZToken *start = peek(parser);
    guard(start);

    if (match(parser, TOK_LPAREN)) {
        bool prevNoStructLit = parser->noStructLit;
        parser->noStructLit  = false;
        ZNode *node          = tryParse(parser, parseExpr(parser));
        parser->noStructLit  = prevNoStructLit;
        expect(parser, TOK_RPAREN);
        return node;
    } else if (check(parser, TOK_LSBRACKET)) {
        if (checkAhead(parser, TOK_RSBRACKET, 1)) {
            ZToken *t = peekAhead(parser, 2);
            u16 mask = TOK_SIGNED | TOK_UNSIGNED | TOK_FLOAT;
            if (t && !t->newlineBefore)
                if (t->type == TOK_IDENT    ||
                    t->type == TOK_STAR     ||
                    t->type == TOK_OPT      ||
                    t->type == TOK_LSBRACKET||
                    t->type == TOK_LPAREN   ||
                    tokmask(t, mask)) {
                return parseTypeAsExpr(parser);
            }
        }
        ZNode *expr = tryParse(parser, parseArrayLit(parser));
        if (expr) return expr;
    } else if (check(parser, TOK_IDENT)) {
        if (!parser->noStructLit && checkAhead(parser, TOK_LBRACKET, 1)) {
            ZNode *structlit = tryParse(parser, parseStructLit(parser));
            if (structlit) return structlit;
        }
        ZNode *node         = makenode(parser->allocator, NODE_IDENTIFIER);
        node->identNode.tok = consume(parser);
        node->tok           = node->identNode.tok;
        return setspan(parser, node, start);
    } else if (checkMask(parser, TOK_LITERAL)) {
        return parseLit(parser);
    } else if (check(parser, TOK_SIZEOF)) {
        ZToken *tok         = consume(parser);
        ZType *type         = parseType(parser);
        if (!type) {
            zlog(parser->state, tok, Z2002);
            return NULL;
        }
        ZNode *node = makenode(parser->allocator, NODE_SIZEOF);
        node->sizeofExpr.type   = type;
        node->tok               = tok;
        return setspan(parser, node, start);
    } else if (check(parser, TOK_IF)) {
        return parseIf(parser);
    } else if (check(parser, TOK_LBRACKET)) {
        return parseBlock(parser);
    } else if (check(parser, TOK_DOT)) {
        if (checkAhead(parser, TOK_IDENT, 1)) {
            ZToken *base = peek(parser);
            ZNode *node                 = makenode(parser->allocator, NODE_MEMBER);

            ZNode *ident                = makenode(parser->allocator, NODE_IDENTIFIER);
            ident->tok                  = consume(parser);
            ident->identNode.tok        = ident->tok;
            setspan(parser, ident, base);
            node->memberAccess.object   = ident;
            node->memberAccess.field    = consume(parser);
            node->tok                   = base;

            return setspan(parser, node, base);
        } else if (checkAhead(parser, TOK_LBRACKET, 1)) {
            return parseStructLit(parser);
        } else {
            zlog(parser->state, peek(parser), Z2003);
        }
    } else if (check(parser, TOK_FN)) {
        return parseAnonFunc(parser);
    } else if (checkMask(parser, TOK_SIGNED | TOK_UNSIGNED | TOK_FLOAT)) {
        return parseTypeAsExpr(parser);
    }

    return NULL;
}

static ZNode *parseArrSubscript(ZParser *parser, ZNode *previous) {
    if (!canPeek(parser) || peek(parser)->newlineBefore) return previous;

    expect(parser, TOK_LSBRACKET);
    ZNode *index = tryParse(parser, parseExpr(parser));
    expect(parser, TOK_RSBRACKET);

    ZNode *node             = makenode(parser->allocator, NODE_SUBSCRIPT);
    node->subscript.index   = index;
    node->subscript.arr     = previous;
    node->tok               = previous->tok;
    return setspanfrom(parser, node, previous);
}

static ZNode **parseArgs(ZParser *parser) {
    ZNode **args = NULL;

    ZNode *expr = tryParse(parser, parseExpr(parser));
    if (!expr) return args;

    vecpush(args, expr);

    while (match(parser, TOK_COMMA)) {
        expr = tryParse(parser, parseExpr(parser));
        if (!expr) break;
        vecpush(args, expr);
    }

    return args;
}

static ZNode *parseMemberAccess(ZParser *parser, ZNode *previous) {
    ZToken *start = peek(parser);
    if (!match(parser, TOK_DOT)) return previous;
    if (check(parser, TOK_REF) ||
        check(parser, TOK_STAR)) {
        ZNode *unary            = makenode(parser->allocator, NODE_UNARY);
        unary->tok              = start;
        unary->unary.operand    = previous;
        unary->unary.operat     = consume(parser);
        return setspanfrom(parser, unary, previous);
    }


    ZToken *member = NULL;

    bool missing = !check(parser, TOK_IDENT) && !check(parser, TOK_INT_LIT);
#if Z_LSP
    missing = missing || !canPeek(parser) || peek(parser)->newlineBefore;
#endif
    if (missing) {
#if Z_COMPILER
        zlog(parser->state, peek(parser), Z2004);
        return previous;
#else
        member = makeident(
            parser->allocator, DEFAULT_MEMBER_ACCESS, start->end, start->end
        );
        member->row = start->row;
        member->col = start->col;
#endif
    } else {
        member = consume(parser);
    }

    ZNode *node = makenode(parser->allocator, NODE_MEMBER);

    node->memberAccess.field    = member;
    node->memberAccess.object   = previous;
    node->memberAccess.path     = NULL;
    node->tok = member;
    return setspanfrom(parser, node, previous);
}

static ZNode *parseFuncCall(ZParser *parser, ZNode *previous) {
    ZToken *start = peek(parser);

    if (!canPeek(parser) || peek(parser)->newlineBefore) return previous;

    expect(parser, TOK_LPAREN);
    ZNode **args = parseArgs(parser);
    expect(parser, TOK_RPAREN);

    ZNode *node = makenode(parser->allocator, NODE_CALL);
    node->call.args = args;
    node->call.callee = previous;
    node->tok = previous->tok;

    if (!previous) {
        zlog(parser->state, start, Z900A);
    } else if (!previous->tok) {
        zlog(parser->state, start, Z900B, previous->type);
    }
    return setspanfrom(parser, node, previous);
}

static ZNode *parseCast(ZParser *parser, ZNode *previous) {
    ensure(match(parser, TOK_CAST), "Expected 'as' keyword for casting types");
    ZType *type = parseType(parser);

    guard(type);

    ZNode *node = makenode(parser->allocator, NODE_CAST);
    node->castExpr.expr = previous;
    node->castExpr.info.to = type;
    node->tok = previous->tok;
    return setspanfrom(parser, node, previous);
}

static ZNode *parseSquareBracket(ZParser *parser, ZNode *previous) {
    ZNode *node = parseArrSubscript(parser, previous);
    if (!node) return previous;

    if (node->type != NODE_SUBSCRIPT || !node->subscript.index) return node;

    if (node->subscript.index->type == NODE_RANGE) {
        ZNode *i = node->subscript.index;
        ZNode *slice        = makenode(parser->allocator, NODE_SLICE);
        slice->slice.start  = i->binary.left;
        slice->slice.end    = i->binary.right;
        slice->slice.base   = node->subscript.arr;
        slice->tok          = previous->tok;
        return setspanfrom(parser, slice, previous);
    }

    return node;
}

static ZNode *parseExtractValue(ZParser *parser, ZNode *previous) {
    if (!check(parser, TOK_ESCL)) return previous;
    ZToken *start       = consume(parser);
    ZNode *node         = makenode(parser->allocator, NODE_UNARY);
    node->unary.operand = previous;
    node->unary.operat  = start;
    node->tok           = start;
    return setspanfrom(parser, node, previous);
}

static ZNode *parsePostfixOper(ZParser *parser, ZNode *previous) {
    ZParserSnapshot *snap = store(parser);
    ZNode *res = NULL;

    ZToken *tok = peek(parser);

    /* Postfix operator MUST BE on the same line. */
    if (!tok || tok->newlineBefore) goto cleanup;

    switch (tok->type) {
    case TOK_DOT:       res = parseMemberAccess (parser, previous); break;
    case TOK_CAST:      res = parseCast         (parser, previous); break;
    case TOK_LPAREN:    res = parseFuncCall     (parser, previous); break;
    case TOK_LSBRACKET: res = parseSquareBracket(parser, previous); break;
    case TOK_ESCL:      res = parseExtractValue (parser, previous); break;
    default:            return NULL;
    }

    if (res) {
        commitErrors(parser);
        return res;
    }
    cleanup:
        undo(parser, snap);

    return res;
}

static ZNode *_parsePostfixExpr(ZParser *parser, ZNode *base) {
    ZNode *left = base ? base : tryParse(parser, parsePrimary(parser));

    guard(left);

    ZNode *node = NULL;
    do {

        node = parsePostfixOper(parser, left);
        if (!node) break;

        left = node;
    } while (node);
    return left;
}

static ZNode *parsePostfixExpr(ZParser *parser) {
    return _parsePostfixExpr(parser, NULL);
}

static ZNode *_parseUnary(ZParser *parser, ZNode *expr) {
    ZToken *start = peek(parser);
    guard(start);

    ZNode *node = tryParse(parser, parsePostfixExpr(parser));

    if (node) return node;

    ZTokenType valids[] = {
        TOK_PLUS,   TOK_MINUS,  TOK_NOT,
        TOK_STAR,   TOK_REF,    TOK_BITNOT
    };

    if (!isValidToken(parser, valids, arrlen(valids))) {
        zlog(parser->state, start, Z2005, stoken(start));
        return NULL;
    }

    node                = makenode(parser->allocator, NODE_UNARY);
    node->tok           = start;
    node->unary.operat  = consume(parser);
    node->unary.operand = expr ? expr : tryParse(parser, parseUnary(parser));

    guard(node->unary.operand);

    return setspan(parser, node, start);
}

ZNode *parseUnary(ZParser *parser) {
    return _parseUnary(parser, NULL);
}

ZNode *parseOrGrammar(ZParser *parser, ZParseFunc *pf, usize len) {
    for (usize i = 0; i < len; i++) {
        ZNode *parsed = tryParse(parser, pf[i](parser));
        if (parsed) return parsed;
    }
    return NULL;
}

static ZNode *_parseGenericBinary(ZParser *parser,
                                    ZParseFunc parseLeft,
                                    ZParseFunc parseRight,
                                    ZTokenType *validTokens,
                                    size_t validTokensLen,
                                    ZNode *expr) {
    ZNode *node = NULL;

    ZNode *left = expr ? expr : tryParse(parser, parseLeft(parser));

    guard(left);

    while (canPeek(parser) &&
                    isValidToken(parser, validTokens, validTokensLen) &&
                    !peek(parser)->newlineBefore) {
        node = makenode(parser->allocator, NODE_BINARY);
        ZToken *op = consume(parser);

        ZNode *right = tryParse(parser, parseRight(parser));

        if (!right) {
            zlog(parser->state, op, Z2001, stoken(op));
            return NULL;
        }

        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = right;
        node->tok = op;
        setspanfrom(parser, node, left);
        left = node;
    }

    return node ? node : left;
}

static ZNode *parseGenericBinary(ZParser *parser,
                                    ZParseFunc parseLeft,
                                    ZParseFunc parseRight,
                                    ZTokenType *validTokens,
                                    size_t validTokensLen) {
    return _parseGenericBinary(
        parser,         parseLeft,
        parseRight,     validTokens,
        validTokensLen, NULL
    );
}

static ZNode *parseFactor(ZParser *parser) {
    ZTokenType valids[] = {TOK_STAR, TOK_DIV, TOK_MOD};
    return parseGenericBinary(parser, parseUnary, parseUnary, valids, arrlen(valids));
}

static ZNode *parseTerm(ZParser *parser) {
    ZTokenType valids[] = {TOK_PLUS, TOK_MINUS};
    return parseGenericBinary(parser, parseFactor, parseFactor, valids, arrlen(valids));
}

static ZNode *parseShiftBit(ZParser *parser) {
    ZTokenType valids[] = {TOK_BITL, TOK_BITR};
    return parseGenericBinary(parser, parseTerm, parseTerm, valids, arrlen(valids));
}

static ZNode *parseComparison(ZParser *parser) {
    ZTokenType valids[] = {TOK_LT, TOK_GT, TOK_LTE, TOK_GTE};
    return parseGenericBinary(parser, parseShiftBit, parseShiftBit, valids, arrlen(valids));
}

static ZNode *parseEquality(ZParser *parser) {
    ZTokenType valids[] = {TOK_EQEQ, TOK_NOTEQ};
    return parseGenericBinary(
        parser, parseComparison, parseComparison, valids, arrlen(valids)
    );
}

static ZNode *parseBitAnd(ZParser *parser) {
    ZTokenType valid = TOK_REF;
    return parseGenericBinary(parser, parseEquality, parseEquality, &valid, 1);
}

static ZNode *parseBitXor(ZParser *parser) {
    ZTokenType valid = TOK_BITNOT;
    return parseGenericBinary(parser, parseBitAnd, parseBitAnd, &valid, 1);
}

static ZNode *parseBitOr(ZParser *parser) {
    ZTokenType valid = TOK_BITOR;
    return parseGenericBinary(parser, parseBitXor, parseBitXor, &valid, 1);
}

static ZNode *parseLogicalAnd(ZParser *parser) {
    ZTokenType valids[] = {TOK_AND};
    return parseGenericBinary(parser, parseBitOr, parseBitOr, valids, arrlen(valids));
}

static ZNode *parseLogicalOr(ZParser *parser) {
    ZNode *node = parseLogicalAnd(parser);

    while (true) {
        ZToken *or      = peek(parser);
        ZNode *right    = NULL, *expr = NULL;
        int unwrapKind  = 0;
        if (!match(parser, TOK_OR)) return node;
        else if (check(parser, TOK_BREAK)) {
            expr        = parseBreak(parser);
            unwrapKind  = UNWRAP_BREAK;
        } else if (check(parser, TOK_CONTINUE)) {
            expr        = parseContinue(parser);
            unwrapKind  = UNWRAP_CONTINUE;
        } else if (check(parser, TOK_RETURN)) {
            expr        = parseReturn(parser);
            unwrapKind  = UNWRAP_RETURN;
        } else if (match(parser, TOK_DO)) {
            expr        = parseExpr(parser);
            unwrapKind  = UNWRAP_DO;
        }

        if (expr) {
            right                   = makenode(parser->allocator, NODE_UNWRAP);
            right->unwrap.base      = node;
            right->unwrap.orExpr    = expr;
            right->unwrap.kind      = unwrapKind;
        } else {
            ZNode *expr             = tryParse(parser, parseLogicalAnd(parser));
            if (!expr) return node;

            right               = makenode(parser->allocator, NODE_BINARY);
            right->tok          = or;
            right->binary.left  = node;
            right->binary.right = expr;
            right->binary.op    = or;
        }

        setspanfrom(parser, right, node);
        node = right;
    }

    return node;
}

static ZNode *parseUpdateRhs(ZParser *parser, ZNode *lhs) {
    ZToken *tok = peek(parser);
    guard(tok);

    /* Binary op: x: + 1  ->  x + 1 */
    ZTokenType binOps[] = {
        TOK_PLUS, TOK_MINUS,  TOK_STAR, TOK_DIV, TOK_MOD,
        TOK_BITL, TOK_BITR,   TOK_REF,  TOK_BITOR, TOK_BITNOT,
        TOK_AND,  TOK_OR
    };
    if (isValidToken(parser, binOps, arrlen(binOps))) {
        ZToken *op  = consume(parser);
        ZNode  *rhs = tryParse(parser, parseExpr(parser));
        if (!rhs) {
            zlog(parser->state, op, Z2001, stoken(op));
            return NULL;
        }
        ZNode *node        = makenode(parser->allocator, NODE_BINARY);
        node->binary.op    = op;
        node->binary.left  = lhs;
        node->binary.right = rhs;
        node->tok          = op;
        return setspanfrom(parser, node, lhs);
    }

    /* Postfix on lhs: x: .field  ->  x.field,  x: [i]  ->  x[i] */
    if (tok->type == TOK_DOT || tok->type == TOK_LSBRACKET || tok->type == TOK_CAST) {
        return _parsePostfixExpr(parser, lhs);
    }

    /* Prefix unary on lhs: x: not  ->  not x,  x: ~  ->  ~x */
    ZTokenType unaryOps[] = { TOK_NOT, TOK_BITNOT };
    if (isValidToken(parser, unaryOps, arrlen(unaryOps))) {
        ZNode *node         = makenode(parser->allocator, NODE_UNARY);
        node->unary.operat  = consume(parser);
        node->unary.operand = lhs;
        node->tok           = node->unary.operat;
        return setspanfrom(parser, node, lhs);
    }

    /* Function call with lhs prepended: x: f(args)  ->  f(x, args) */
    ZNode *callee = tryParse(parser, parsePrimary(parser));
    if (callee) {
        if (!check(parser, TOK_LPAREN)) {
            zlog(parser->state, peek(parser), Z2006, stoken(callee->tok));
            return NULL;
        }
        consume(parser); /* ( */
        ZNode **args = NULL;
        vecpush(args, lhs);
        ZNode **rest = parseArgs(parser);
        for (usize i = 0; i < veclen(rest); i++)
            vecpush(args, rest[i]);
        expect(parser, TOK_RPAREN);
        ZNode *call       = makenode(parser->allocator, NODE_CALL);
        call->call.callee = callee;
        call->call.args   = args;
        call->tok         = callee->tok;
        setspanfrom(parser, call, callee);
        return _parsePostfixExpr(parser, call);
    }

    zlog(parser->state, tok, Z2007, stoken(tok));
    return NULL;
}

ZNode *parseUpdate(ZParser *parser) {
    ZNode *lhs = parseLogicalOr(parser);
    guard(lhs);

    ZToken *colon = peek(parser);
    if (!colon || colon->type != TOK_LARROW || colon->newlineBefore) return NULL;
    consume(parser);

    ZNode *rhs = parseUpdateRhs(parser, lhs);
    guard(rhs);

    ZNode *assign        = makenode(parser->allocator, NODE_BINARY);
    assign->binary.op    = maketoken(parser->allocator, TOK_EQ, NULL, NULL);
    assign->binary.left  = lhs;
    assign->binary.right = rhs;
    assign->tok          = lhs->tok;
    return setspanfrom(parser, assign, lhs);
}

ZNode *parseBreak(ZParser *parser) {
    ZNode *node = makenode(parser->allocator, NODE_BREAK);
    node->tok = consume(parser);

    if (!canPeek(parser) || peek(parser)->newlineBefore) return setspan(parser, node, node->tok);

    node->breakStmt.expr = tryParse(parser, parseExpr(parser));

    return setspan(parser, node, node->tok);
}

ZNode *parseContinue(ZParser *parser) {
    ZNode *node = makenode(parser->allocator, NODE_CONTINUE);
    node->tok = consume(parser);

    return setspan(parser, node, node->tok);
}

ZNode *parseTupleLit(ZParser *parser) {
    ZToken *start = peek(parser);

    expect(parser, TOK_LPAREN);
    ZNode *expr = NULL;
    ZNode **fields = NULL;
    while (( expr = tryParse(parser, parseExpr(parser)) )) {
        vecpush(fields, expr);
        if (check(parser, TOK_RPAREN)) break;
        if (!match(parser, TOK_COMMA)) break;
    }
    expect(parser, TOK_RPAREN);

    ZNode *node     = makenode(parser->allocator, NODE_TUPLE_LIT);
    node->tok       = start;
    node->tuplelit  = fields;

    if (veclen(fields) < 2) {
        zlog(parser->state, start, Z202E);
    }

    return setspan(parser, node, start);
}

ZNode *parseArrayLit(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_LSBRACKET);

    ZNode **values = NULL;
    ZNode *expr = NULL;

    do {
        expr = parseExpr(parser);
        if (!expr) break;
        vecpush(values, expr);
    } while (!check(parser, TOK_RSBRACKET) && match(parser, TOK_COMMA));

    if (!check(parser, TOK_RSBRACKET)) {
        sinchronize(parser, TOK_LBRACKET, TOK_RSBRACKET);
    }

    expect(parser, TOK_RSBRACKET);

    ZNode *node = makenode(parser->allocator, NODE_ARRAY_LIT);

    node->arraylit  = values;
    node->tok       = start;

    return setspan(parser, node, start);
}

ZNode *_parseStructLit(ZParser *parser, ZToken **chain) {
    ZType **generics = NULL;

    if (check(parser, TOK_LSBRACKET)) {
        generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
        if (!generics) {
            zlog(parser->state, peek(parser), Z202F);
            return NULL;
        }
    }

    expect(parser, TOK_LBRACKET);

    ZNode *structlit = makenode(parser->allocator, NODE_STRUCT_LIT);
    structlit->structlit.chain      = chain;
    structlit->structlit.generics   = generics;
    structlit->tok                  = veclast(chain);
    ZNode *expr                     = NULL;
    ZNode *var                      = NULL;

    do {
        if (!check(parser, TOK_IDENT)) break;
        ZToken *tok                 = consume(parser);
        ZVarDestructPattern *node   = makeDestructIdent(parser->allocator, tok);
        expr                        = NULL;
        var                         = NULL;

        if (match(parser, TOK_EQ)) {
            expr = tryParse(parser, parseExpr(parser));
            if (!expr) return NULL;
        } else if (check(parser, TOK_COMMA) || check(parser, TOK_RBRACKET)) {
            expr = makenode(parser->allocator, NODE_IDENTIFIER);
            expr->tok = tok;
            expr->identNode.tok = tok;
            setspan(parser, expr, tok);
        } else {
            zlog(parser->state, peek(parser), Z2030, peek(parser));
        }
        var = makenodevar(parser->allocator, node, NULL, expr);
        setspan(parser, var, tok);
        vecpush(structlit->structlit.fields, var);
    } while (!check(parser, TOK_RBRACKET) && match(parser, TOK_COMMA));

    expect(parser, TOK_RBRACKET);

    return setspan(parser, structlit, chain ? chain[0] : NULL);
}

ZNode *parseStructLit(ZParser *parser) {
    if (parser->noStructLit) return NULL;
    if (!check(parser, TOK_IDENT) &&
        !check(parser, TOK_DOT)) return NULL;

    ZToken **chain = NULL;

    do {
        ZToken *ident = consume(parser);
        vecpush(chain, ident);
    } while (   !check(parser, TOK_LBRACKET)    &&
                !check(parser, TOK_LSBRACKET)   &&
                match(parser, TOK_DOUBLE_COLON) &&
                check(parser, TOK_IDENT)        );

    return _parseStructLit(parser, chain);
}

static ZVarDestructPattern *parseDestructEnum(ZParser *parser, ZToken *base) {
    if (base->type == TOK_IDENT && !match(parser, TOK_DOUBLE_COLON)) {
        zlog(parser->state, base, Z2026);
        return NULL;
    } else if (base->type != TOK_DOT) {
        zlog(parser->state, base, Z2027);
        return NULL;
    }

    if (!check(parser, TOK_IDENT)) return NULL;

    ZVarDestructPattern *cur = makeVarDestructPattern(parser->allocator, Z_VAR_ENUM);
    cur->base = base;
    cur->prop = consume(parser);
    cur->args = NULL;

    if (!match(parser, TOK_LPAREN)) return cur;

    if (!check(parser, TOK_RPAREN)) {
        do {
            ZVarDestructPattern *item = parseDestructVar(parser, true);
            if (!item) break;
            vecpush(cur->args, item);
        } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));
    }

    if (!match(parser, TOK_RPAREN)) {
        zlog(parser->state, peek(parser), Z2028);
        sinchronize(parser, TOK_LPAREN, TOK_RPAREN);
    }

    return cur;
}

ZVarDestructPattern *parseDestructVar(ZParser *parser, bool conditional) {
    guard(canPeek(parser));

    ZVarDestructPattern *cur    = NULL;
    ZVarDestructPattern **list  = NULL;

    if (conditional) {
        bool isSumPattern =
            checkMask(parser, TOK_TYPES_MASK) ||
            check(parser, TOK_STAR) ||
            check(parser, TOK_LSBRACKET) ||
            (check(parser, TOK_IDENT) && checkAhead(parser, TOK_LPAREN, 1));

        if (isSumPattern) {
            ZToken *start       = peek(parser);
            ZType *sumType      = parseType(parser);

            expect(parser, TOK_LPAREN);
            ZVarDestructPattern *child = parseDestructVar(parser, conditional);
            expect(parser, TOK_RPAREN);

            cur             = makeVarDestructPattern(parser->allocator, Z_VAR_SUM);
            cur->tok        = start;
            cur->sum.type   = sumType;
            cur->sum.child  = child;
            return cur;
        }
    }

    ZToken *tok = consume(parser);

    if (tokmask(tok, TOK_LITERAL) && tok->type != TOK_STR_LIT) {
        cur = makeVarDestructPattern(parser->allocator, Z_VAR_LIT);
        cur->ident = tok;
    } else if (tok->type == TOK_IDENT) {
        if (conditional && match(parser, TOK_DOT)) {
            if (!check(parser, TOK_IDENT)) {
                zlog(parser->state, peek(parser), Z2018);
            }
            cur = makeVarDestructPattern(parser->allocator, Z_VAR_ENUM);
            cur->base = tok;
            cur->prop = consume(parser);
            cur->args = NULL;

            if (match(parser, TOK_LPAREN)) {
                if (!check(parser, TOK_RPAREN)) {
                    do {
                        ZVarDestructPattern *item = parseDestructVar(parser, conditional);
                        if (!item) break;
                        vecpush(cur->args, item);
                    } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));
                }
                expect(parser, TOK_RPAREN);
            }
        } else {
            cur = makeVarDestructPattern(parser->allocator, Z_VAR_IDENT);
            cur->ident = tok;
        }
    } else if (tok->type == TOK_LBRACKET) {
        ZToken *key = NULL;
        do {
            if (!check(parser, TOK_IDENT)) break;
            key = consume(parser);

            if (check(parser, TOK_COMMA) || check(parser, TOK_RBRACKET)) {
                cur = makeDestructIdent(parser->allocator, key);
            } else if (match(parser, TOK_COLON)) {
                cur = parseDestructVar(parser, conditional);
            } else {
                zlog(parser->state, peek(parser), Z2015);
                break;
            }

            if (!parser) {
                zlog(parser->state, key, Z2029, stoken(key));
                return NULL;
            }

            ZVarDestructPattern *pair = makeVarDestructPattern(parser->allocator, Z_VAR_PAIR);
            pair->key = key;
            pair->value = cur;

            vecpush(list, pair);

        } while (!check(parser, TOK_RBRACKET) && match(parser, TOK_COMMA));
        expect(parser, TOK_RBRACKET);

        cur = makeVarDestructPattern(parser->allocator, Z_VAR_STRUCT);
        cur->fields = list;
    } else if (tok->type == TOK_LPAREN) {
        do {
            cur = parseDestructVar(parser, conditional);
            if (!cur) break;
            vecpush(list, cur);
        } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));

        expect(parser, TOK_RPAREN);

        cur = makeVarDestructPattern(parser->allocator, Z_VAR_TUPLE);
        cur->tuple = list;
        cur->tok = tok;
    } else if (tok->type == TOK_DOT) {
        cur = parseDestructEnum(parser, tok);
    } else {
        zlog(parser->state, tok, Z202A);
        return NULL;
    }

    cur->tok = tok;
    cur->resolved = NULL;
    return cur;
}

ZVarDestructPattern *parseMultiDestructVar(ZParser *parser) {
    ZToken *start = peek(parser);
    ZVarDestructPattern *pattern = parseDestructVar(parser, false);
    if (match(parser, TOK_COMMA)) {
        ZVarDestructPattern **list = NULL;
        vecpush(list, pattern);
        do {
            ZVarDestructPattern *next = parseDestructVar(parser, false);
            if (!next) break;
            vecpush(list, next);
        } while (match(parser, TOK_COMMA));
        pattern         = makeVarDestructPattern(parser->allocator, Z_VAR_TUPLE);
        pattern->tuple  = list;
        pattern->tok    = start;
    }
    return pattern;
}

ZNode *parseMultiExpr(ZParser *parser) {
    ZToken *start = peek(parser);
    ZNode *curr = tryParse(parser, parseExpr(parser));
    if (!curr || !check(parser, TOK_COMMA)) return curr;

    ZNode **list = NULL;
    vecpush(list, curr);

    while (curr && match(parser, TOK_COMMA)) {
        curr = tryParse(parser, parseExpr(parser));
        if (!curr) break;
        vecpush(list, curr);
    }

    ZNode *tuple    = makenode(parser->allocator, NODE_TUPLE_LIT);
    tuple->tok      = start;
    tuple->tuplelit = list;
    return setspan(parser, tuple, start);
}


static ZNode *parseRangeExpr(ZParser *parser) {
    ZNode *left = tryParse(parser, parseLogicalOr(parser));

    if (!canPeek(parser)                ||
        !check(parser, TOK_DOUBLE_DOT)  ||
        peek(parser)->newlineBefore     ) {
        return left;
    }

    ZToken *op   = consume(parser);
    ZNode *right = tryParse(parser, parseLogicalOr(parser));

    if (!left && !right) {
        zlog(parser->state, op, Z2008);
        return NULL;
    }

    ZNode *range        = makenode(parser->allocator, NODE_RANGE);
    range->binary.op    = op;
    range->binary.left  = left;
    range->binary.right = right;
    range->tok          = op;

    /* `..b` has no left operand: the range starts at the operator. */
    return left ? setspanfrom(parser, range, left) : setspan(parser, range, op);
}


ZNode *parseBinary(ZParser *parser) {
    ZTokenType valids[] = { TOK_EQ };
    return parseGenericBinary(
        parser, parseRangeExpr, parseLogicalOr,
        valids, arrlen(valids)
    );
}


ZNode *parseTypeAsExpr(ZParser *parser) {
    ZToken *start   = peek(parser);
    ZType *type     = parseType(parser);
    guard(type);

    ZNode *node     = makenode(parser->allocator, NODE_TYPE);
    node->resolved  = type;
    return setspan(parser, node, start);
}


ZNode *parseExpr(ZParser *parser) {
    ZAnnotation **annotations = NULL;
    if (check(parser, TOK_HASHTAG)) {
        annotations = parseAnnotations(parser);
    }

    ZToken *curr    = peek(parser);
    ZNode *res      = NULL;
    if (!curr) return NULL;

    switch (curr->type) {
    case TOK_IF:        res = parseIf(parser);              break;
    case TOK_MATCH:     res = parseMatch(parser, true);     break;
    case TOK_WITH:      res = parseCapabilityBlock(parser); break;
    case TOK_OPT:       res = parseTypeAsExpr(parser);      break;
    default:            res = parseOrGrammar(
            parser, exprFunc, arrlen(exprFunc));            break;
    }

    if (res) res->annotations = annotations;
    return res;
}


static ZNode *parseAnonFuncArgument(ZParser *parser) {
    if (!check(parser, TOK_IDENT)) return NULL;
    ZToken *name = consume(parser);

    ZNode *field = makenode(parser->allocator, NODE_FIELD);
    field->field.identifier = name;
    field->tok              = name;

    if (match(parser, TOK_COLON)) {
        field->field.type   = parseType(parser);
        field->resolved     = field->field.type;
    }

    return setspan(parser, field, name);
}


ZNode *parseAnonFunc(ZParser *parser) {
    ZToken *start   = peek(parser);
    ZType *type     = NULL;

    expect(parser, TOK_FN);

    ZType **generics = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser);
    }

    if (!check(parser, TOK_LPAREN)) return NULL;

    ZNode **args = parseGenericList(parser,
        TOK_LPAREN,         TOK_RPAREN,
        parseAnonFuncArgument,  true
    );

    if (!check(parser, TOK_WITH)    &&
        !check(parser, TOK_ARROW)   &&
        !check(parser, TOK_LBRACKET)) {
        type = tryParse(parser, parseType(parser));
        if (!type) return NULL;
    }

    ZNode **capabilities = parseCapabilityList(parser);

    ZNode *body     = NULL;
    ZToken *arrow   = peek(parser);

    if (match(parser, TOK_ARROW)) {
        ZNode *expr = tryParse(parser, parseExpr(parser));
        if (!expr) return NULL;
        ZNode *ret = makenode(parser->allocator, NODE_RETURN);
        ret->returnStmt.expr = expr;
        setspanfrom(parser, ret, expr);

        body = makenode(parser->allocator, NODE_BLOCK);
        body->block = NULL;
        vecpush(body->block, ret);
        setspan(parser, body, arrow);
    } else if (check(parser, TOK_LBRACKET) && !parser->noStructLit) {
        body = tryParse(parser, parseBlock(parser));
    }

    if (!body) return NULL;

    ZNode *func = NULL;
    func                        = makenode(parser->allocator, NODE_FUNC);
    func->funcDef.args          = args;
    func->funcDef.generics      = generics;
    func->funcDef.capabilities  = capabilities;
    func->funcDef.pub           = false;
    func->funcDef.annotations   = NULL;
    func->funcDef.receiver      = NULL;
    func->funcDef.base          = NULL;
    func->funcDef.name          = NULL;
    func->funcDef.body          = body;
    func->funcDef.mangled       = NULL;
    func->tok                   = start;

    ZType *funcType             = maketype(parser->allocator, Z_TYPE_FUNCTION);
    funcType->func.ret          = type;
    funcType->func.capabilities = NULL;
    funcType->func.variadic     = false;
    funcType->func.args         = NULL;
    funcType->tok               = start;
    func->resolved              = funcType;

    for (usize i = 0; i < veclen(capabilities); i++) {
        vecpush(funcType->func.capabilities, capabilities[i]->resolved);
    }

    for (usize i = 0; i < veclen(args); i++) {
        vecpush(funcType->func.args, args[i]->resolved);
    }
    return setspan(parser, func, start);
}
