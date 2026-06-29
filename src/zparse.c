// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zinc.h"
#include "zmem.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

#define ENTRY_IMPORT_FILE "lib"

#define arrlen(arr) (sizeof(arr) / sizeof((arr)[0]))

#define ensure(c, msg) do {                                                     \
    if (!(c)) {                                                                 \
        return NULL;                                                            \
    }                                                                           \
} while (0)

#define guard(c) if (!(c)) return NULL

#define expect(l, t) if (!match(l, t)) {                                        \
        error((l)->state, peek(l),                                              \
                "Expected %s, got %s", tokname(t), stoken(peek(parser)));       \
        return NULL;                                                            \
    }

#define tryParse(p, func) ({                                                    \
    ZParserSnapshot *saved = store(p);                                          \
    pushErrorCheckpoint(p);                                                     \
    let res = (func);                                                           \
    if (!res) {                                                                 \
        undo(p, saved);                                                         \
        rollbackErrors(p);                                                      \
    } else {                                                                    \
        commitErrors(p);                                                        \
    }                                                                           \
    res;                                                                        \
})

#define sinchronize(parser, type) if (!check(parser, type))                     \
    while (canPeek(parser) && !check(parser, type)) consume(parser);            \



typedef ZNode *(*ZParseFunc)(ZParser *);

ZType *parseType                            (ZParser *);
ZNode *parseExpr                            (ZParser *);
static ZNode *parse                         (ZParser *);
static ZNode *parseIf                       (ZParser *);
static ZNode *parseBreak                    (ZParser *);
static ZNode *parseBlock                    (ZParser *);
static ZNode *parseDefer                    (ZParser *);
static ZNode *parseMatch                    (ZParser *, bool);
static ZNode *parseUnary                    (ZParser *);
static ZNode *parseLoops                    (ZParser *);
static ZNode *parseReturn                   (ZParser *);
static ZNode *parseVarDef                   (ZParser *);
static ZNode *parseBinary                   (ZParser *);
static ZNode *parseContinue                 (ZParser *);
static ZNode *parseArrayLit                 (ZParser *);
static ZNode *parseTupleLit                 (ZParser *);
static ZType *parseTypeArray                (ZParser *);
static ZNode *parseStructLit                (ZParser *);
static ZNode *parseVarInferred              (ZParser *);
static ZNode *parseVarDefTyped              (ZParser *);
static ZNode *parseBlockOrInline            (ZParser *);

/* File-level parsing functions */
static ZNode *parseImport                   (ZParser *);
static ZNode *skipMacro                     (ZParser *, bool);
static ZNode *parseTypedef                  (ZParser *, bool);
static ZNode *parseFuncDecl                 (ZParser *, ZAnnotation **, bool);
static ZNode *parseEnumDecl                 (ZParser *, ZAnnotation **, bool);
static ZNode *parseStructDecl               (ZParser *, ZAnnotation **, bool);

static ZType **parseGenericsDecl            (ZParser *, bool);
static ZMacroPattern *parseMacroPattern     (ZParser *, ZNode *);
static ZVarDestructPattern *parseDestructVar(ZParser *, bool);
static ZParseFunc exprFunc[] = {
    parseBinary,
    parseTupleLit,
};

static ZParser *makeparser(ZState *state, ZToken **tokens) {
    ZParser *self                       = zalloc(ZParser);
    self->source                        = maketokstream(tokens, NULL);
    self->tokenIndex                    = 0;
    self->errstack                      = NULL;
    self->depth                         = 0;
    self->state                         = state;
    self->noFuncType                    = false;
    self->noStructLit                   = false;

    self->macroParser.currentMacro      = NULL;
    self->macroParser.expandingMacros   = NULL;
    self->macroParser.currentIndex      = 0;
    self->macroParser.macros            = NULL;
    return self;
}

ZNode *makenode(ZNodeType type) {
    ZNode *self = zalloc(ZNode);
    *self = (ZNode){ 0 };
    self->type = type;

    return self;
}

ZType *maketype(ZTypeKind kind) {
    ZType *self = zalloc(ZType);
    *self = (ZType){ 0 };
    self->kind = kind;
    return self;
}

static ZVarDestructPattern *makeVarDestructPattern(int type) {
    ZVarDestructPattern *self = zalloc(ZVarDestructPattern);
    self->type = type;
    return self;
}

static ZVarDestructPattern *makeDestructIdent(ZToken *tok) {
    ZVarDestructPattern *pattern    = makeVarDestructPattern(Z_VAR_IDENT);
    pattern->tok                    = tok;
    pattern->ident                  = tok;
    return pattern;
}

static ZNode *makenodevar(ZVarDestructPattern *pattern, ZType *type, ZNode *expr) {
    if (!pattern) return NULL;
    ZNode *node             = makenode(NODE_VAR_DECL);
    node->tok               = pattern->tok;
    node->varDecl.pattern   = pattern;
    node->varDecl.rvalue    = expr;
    node->resolved          = type;
    return node;
}

bool canPeek(ZParser *p) {
    while (p->source->current >= p->source->end && p->source->prev) {
        p->source = p->source->prev;
    }
    return p->source->current < p->source->end;
}

static ZToken *peekAhead(ZParser *parser, usize next) {
    ZTokenStream *stream = parser->source;
    while (stream->current + next >= stream->end && stream->prev) {
        stream = stream->prev;
    }
    
    if (stream->current + next >= veclen(stream->list)) return NULL;

    return stream->list[stream->current + next];
}

ZToken *peek(ZParser *parser) {
    if (!canPeek(parser)) return NULL;
    while (parser->source && parser->source->current >= parser->source->end) {
        parser->source = parser->source->prev;
    }

    if (!parser->source) return NULL;

    return parser->source->list[parser->source->current];
}

ZToken *consume(ZParser *parser) {
    guard(canPeek(parser));

    ZToken *curr = peek(parser);
    
    parser->source->current++;
    parser->tokenIndex++;
    return curr;
}

bool check(ZParser *parser, ZTokenType type) {
    return canPeek(parser) && peek(parser)->type == type;
}

bool checkAhead(ZParser *parser, ZTokenType type, usize next) {
    ZToken *cur = peekAhead(parser, next);
    if (!cur) return false;
    return cur->type == type;
}

bool checkMask(ZParser *tokens, u32 mask) {
    return canPeek(tokens) && peek(tokens)->type & mask;
}

bool match(ZParser *parser, ZTokenType type) {
    bool res = check(parser, type);
    if (res) consume(parser);
    return res;
}

static void pushErrorCheckpoint(ZParser *parser) {
    usize errlen = veclen(parser->state->logs);
    vecpush(parser->errstack, errlen);
    parser->depth++;
}

static void commitErrors(ZParser *parser) {
    if (parser->depth > 0) {
        vecpop(parser->errstack);
        parser->depth--;
    }
}

static void rollbackErrors(ZParser *parser) {
    if (parser->depth > 0) {
        usize checkpoint = vecpop(parser->errstack);
        while (veclen(parser->state->logs) > checkpoint) vecpop(parser->state->logs);
        parser->depth--;
    }
}

static bool isValidToken(ZParser *parser, ZTokenType *validTokens, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (check(parser, validTokens[i])) return true;
    }
    return false;
}

typedef struct ZParserSnapshot {
    ZTokenStream    *stream;
    usize           streamIndex;
    usize           tokenIndex;
} ZParserSnapshot;

static ZParserSnapshot *store(ZParser *parser) {
    ZParserSnapshot *self = zalloc(ZParserSnapshot);
    (*self) = (ZParserSnapshot){
        .stream         = parser->source,
        .streamIndex    = parser->source->current,
        .tokenIndex     = parser->tokenIndex
    };
    return self;
}

static void undo(ZParser *parser, ZParserSnapshot *snap) {
    parser->source          = snap->stream;
    parser->source->current = snap->streamIndex;
    parser->tokenIndex      = snap->tokenIndex;
}

static ZNode *parseOrGrammar(ZParser *parser, ZParseFunc *pf, usize len) {
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
        node = makenode(NODE_BINARY);
        ZToken *op = consume(parser);

        ZNode *right = tryParse(parser, parseRight(parser));

        if (!right) {
            error(
                parser->state, op,
                "expected expression after '%s'", stoken(op)
            );
            return NULL;
        }

        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = right;
        node->tok = op;
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

static ZNode *parseArrayInit(ZParser *parser) {
    ZType *arr = parseTypeArray(parser);

    guard(arr);
    guard(arr->array.size > 0);

    ZNode *node = makenode(NODE_ARRAY_INIT);
    node->arrayinit = arr;

    return node;
}

static ZNode *parseInlineIf(ZParser *parser) {
    expect(parser, TOK_IF);
    ZNode *cond         = parseExpr(parser);
    guard(cond);
    
    expect(parser, TOK_ARROW);
    ZNode *body         = parseExpr(parser);
    guard(body);

    expect(parser, TOK_ELSE);
    expect(parser, TOK_ARROW);
    ZNode *elseBranch   = parseExpr(parser);
    guard(elseBranch);

    ZNode *ifExpr               = makenode(NODE_IF);
    ifExpr->ifStmt.cond         = cond;
    ifExpr->ifStmt.body         = body;
    ifExpr->ifStmt.elseBranch   = elseBranch;
    return ifExpr;
}

static ZNode *parsePrimary(ZParser *parser) {
    ZToken *start = peek(parser);
    guard(start);

    if (parser->macroParser.currentMacro && match(parser, TOK_MACRO_IDENT)) {
        ensure(check(parser, TOK_IDENT), "Expected an identifier after @");
        
        ZToken *tok = consume(parser);
        return getMacroCapturedVar(parser->macroParser.currentMacro, tok);
    } else if (match(parser, TOK_LPAREN)) {
        bool prevNoStructLit = parser->noStructLit;
        parser->noStructLit  = false;
        ZNode *node          = tryParse(parser, parseExpr(parser));
        parser->noStructLit  = prevNoStructLit;
        expect(parser, TOK_RPAREN);
        return node;
    } else if (check(parser, TOK_LSBRACKET)) {
        return parseOrGrammar(parser, (ZParseFunc[]){
            parseArrayInit,
            parseArrayLit
        }, 2);
    } else if (check(parser, TOK_IDENT)) {
        if (checkAhead(parser, TOK_DOUBLE_COLON, 1)) {
            if (!checkAhead(parser, TOK_IDENT, 2)) {
                error(parser->state, start, "Expected static call or enum literal");
                return NULL;
            }
            ZNode *node             = makenode(NODE_STATIC_ACCESS);
            node->tok               = start;
            node->staticAccess.base = consume(parser);
            consume(parser);
            node->staticAccess.prop = consume(parser);

            char *segments[] = {
                node->staticAccess.base->str,
                node->staticAccess.prop->str,
                NULL
            };

            node->staticAccess.mangled = mangler(segments);
            return node;
        }
        if (!parser->noStructLit && checkAhead(parser, TOK_LBRACKET, 1)) {
            ZNode *structlit = tryParse(parser, parseStructLit(parser));
            if (structlit) return structlit;
        }
        ZNode *node         = makenode(NODE_IDENTIFIER);
        node->identNode.tok = consume(parser);
        node->tok           = node->identNode.tok;
        return node;
    } else if (checkMask(parser, TOK_LITERAL)) {
        ZNode *node         = makenode(NODE_LITERAL);
        node->literalTok    = consume(parser);
        node->tok           = node->literalTok;
        return node;
    } else if (check(parser, TOK_NONE)) {
        ZNode *node         = makenode(NODE_LITERAL);
        node->literalTok    = consume(parser);
        node->tok           = node->literalTok;
        return node;
    } else if (check(parser, TOK_SIZEOF)) {
        ZToken *tok         = consume(parser);
        ZType *type         = parseType(parser);
        if (!type) {
            error(parser->state, tok, "Expected type argument to sizeof");
            return NULL;
        }
        ZNode *node = makenode(NODE_SIZEOF);
        node->sizeofExpr.type   = type;
        node->tok               = tok;
        return node;
    } else if (check(parser, TOK_IF)) {
        return parseInlineIf(parser);
    }

    return NULL;
}

static ZNode *parseSlice(ZParser *parser, ZNode *previous) {
    expect(parser, TOK_LSBRACKET);
    ZNode *start    = tryParse(parser, parseExpr(parser));
    expect(parser, TOK_COLON);
    ZNode *end      = tryParse(parser, parseExpr(parser));
    expect(parser, TOK_RSBRACKET);

    ZNode *node     = makenode(NODE_SLICE);
    node->slice.base    = previous;
    node->slice.start   = start;
    node->slice.end     = end;
    return node;
}

static ZNode *parseArrSubscript(ZParser *parser, ZNode *previous) {
    expect(parser, TOK_LSBRACKET);
    ZNode *index = tryParse(parser, parseExpr(parser));
    expect(parser, TOK_RSBRACKET);

    ZNode *node             = makenode(NODE_SUBSCRIPT);
    node->subscript.index   = index;
    node->subscript.arr     = previous;
    node->tok               = previous->tok;
    return node;
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
    expect(parser, TOK_DOT);

    if (!check(parser, TOK_IDENT) &&
        !check(parser, TOK_INT_LIT)) {
        error(parser->state, peek(parser),
                "Expected an identifier or a number");
        return NULL;
    }

    ZToken *member = consume(parser);
    ZNode *node = makenode(NODE_MEMBER);

    node->memberAccess.field    = member;
    node->memberAccess.object   = previous;
    node->memberAccess.path     = NULL;
    node->tok = member;
    return node;
}

static ZNode *parseFuncCall(ZParser *parser, ZNode *previous) {
    ZToken *start = peek(parser);
    expect(parser, TOK_LPAREN);
    ZNode **args = parseArgs(parser);
    expect(parser, TOK_RPAREN);

    ZNode *node = makenode(NODE_CALL);
    node->call.args = args;
    node->call.callee = previous;
    node->tok = previous->tok;

    if (!previous) {
        warning(parser->state, start, "Previous does not exist\n");
    } else if (!previous->tok) {
        warning(parser->state, start, "Node %d does not have tok\n", previous->type);
    }
    return node;
}

static ZNode *parseCast(ZParser *parser, ZNode *previous) {
    ensure(match(parser, TOK_CAST), "Expected 'as' keyword for casting types");
    ZType *type = parseType(parser);

    guard(type);

    ZNode *node = makenode(NODE_CAST);
    node->castExpr.expr = previous;
    node->castExpr.toType = type;
    node->tok = previous->tok;
    return node;
}

static ZNode *parseSquareBracket(ZParser *parser, ZNode *previous) {
    ZParserSnapshot *snap = store(parser);
    pushErrorCheckpoint(parser);
    ZNode *res = parseArrSubscript(parser, previous);
    if (res) {
        commitErrors(parser);
        return res;
    }
    rollbackErrors(parser);
    undo(parser, snap);
    snap = store(parser);

    res = parseSlice(parser, previous);
    if (!res) {
        error(parser->state, peek(parser), "is not a slice");
        rollbackErrors(parser);
        return NULL;
    }
    commitErrors(parser);
    return res;
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
        error(parser->state, start,
                "Expected expression, got '%s'", stoken(start));
        return NULL;
    }

    node = makenode(NODE_UNARY);
    node->unary.operat = consume(parser);
    node->unary.operand = expr ? expr : tryParse(parser, parseUnary(parser));

    guard(node->unary.operand);

    return node;
}

static ZNode *parseUnary(ZParser *parser) {
    return _parseUnary(parser, NULL);
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
    ZTokenType valid = TOK_BITXOR;
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
    ZTokenType valids[] = {TOK_OR};
    return parseGenericBinary(
        parser, parseLogicalAnd, parseLogicalAnd,
        valids, arrlen(valids)
    );
}

static ZNode *parseNullCoalescing(ZParser *parser) {
    ZTokenType valid = TOK_COALESCING;
    return parseGenericBinary(
        parser, parseLogicalOr, parseLogicalOr,
        &valid, 1
    );
}

static ZNode *parseUpdateRhs(ZParser *parser, ZNode *lhs) {
    ZToken *tok = peek(parser);
    guard(tok);

    /* Binary op: x: + 1  ->  x + 1 */
    ZTokenType binOps[] = {
        TOK_PLUS, TOK_MINUS,  TOK_STAR, TOK_DIV, TOK_MOD,
        TOK_BITL, TOK_BITR,   TOK_REF,  TOK_BITOR, TOK_BITXOR,
        TOK_AND,  TOK_OR,     TOK_COALESCING
    };
    if (isValidToken(parser, binOps, arrlen(binOps))) {
        ZToken *op  = consume(parser);
        ZNode  *rhs = tryParse(parser, parseExpr(parser));
        if (!rhs) {
            error(parser->state, op, "expected expression after '%s'", stoken(op));
            return NULL;
        }
        ZNode *node        = makenode(NODE_BINARY);
        node->binary.op    = op;
        node->binary.left  = lhs;
        node->binary.right = rhs;
        node->tok          = op;
        return node;
    }

    /* Postfix on lhs: x: .field  ->  x.field,  x: [i]  ->  x[i] */
    if (tok->type == TOK_DOT || tok->type == TOK_LSBRACKET || tok->type == TOK_CAST) {
        return _parsePostfixExpr(parser, lhs);
    }

    /* Prefix unary on lhs: x: not  ->  not x,  x: ~  ->  ~x */
    ZTokenType unaryOps[] = { TOK_NOT, TOK_BITNOT };
    if (isValidToken(parser, unaryOps, arrlen(unaryOps))) {
        ZNode *node         = makenode(NODE_UNARY);
        node->unary.operat  = consume(parser);
        node->unary.operand = lhs;
        node->tok           = node->unary.operat;
        return node;
    }

    /* Function call with lhs prepended: x: f(args)  ->  f(x, args) */
    ZNode *callee = tryParse(parser, parsePrimary(parser));
    if (callee) {
        if (!check(parser, TOK_LPAREN)) {
            error(parser->state, peek(parser),
                    "expected '(' after '%s' in update expression",
                    stoken(callee->tok));
            return NULL;
        }
        consume(parser); /* ( */
        ZNode **args = NULL;
        vecpush(args, lhs);
        ZNode **rest = parseArgs(parser);
        for (usize i = 0; i < veclen(rest); i++)
            vecpush(args, rest[i]);
        expect(parser, TOK_RPAREN);
        ZNode *call       = makenode(NODE_CALL);
        call->call.callee = callee;
        call->call.args   = args;
        call->tok         = callee->tok;
        return _parsePostfixExpr(parser, call);
    }

    error(parser->state, tok, "unexpected token '%s' in update expression", stoken(tok));
    return NULL;
}

static ZNode *parseUpdate(ZParser *parser) {
    ZNode *lhs = parseNullCoalescing(parser);
    guard(lhs);

    ZToken *colon = peek(parser);
    if (!colon || colon->type != TOK_COLON || colon->newlineBefore) return NULL;
    consume(parser);

    ZNode *rhs = parseUpdateRhs(parser, lhs);
    guard(rhs);

    ZNode *assign        = makenode(NODE_BINARY);
    assign->binary.op    = maketoken(TOK_EQ, NULL);
    assign->binary.left  = lhs;
    assign->binary.right = rhs;
    assign->tok          = lhs->tok;
    return assign;
}

static ZNode *parseBinary(ZParser *parser) {
    ZTokenType valids[] = {TOK_EQ};
    return parseGenericBinary(
        parser, parseNullCoalescing, parseBinary,
        valids, arrlen(valids)
    );
}

static ZNode **parseGenericList(ZParser *parser,
                                ZTokenType left,
                                ZTokenType right,
                                ZParseFunc func,
                                bool commaSeparated) {
    expect(parser, left);
    ZNode **list = NULL;
    
    do {
        if (check(parser, right)) break;
        ZNode *cur = tryParse(parser, func(parser));
        if (!cur) {
            error(parser->state, peek(parser), "Expected an expression");
            return list;
        }
        vecpush(list, cur);
        if (commaSeparated && !match(parser, TOK_COMMA)) break;
    } while (true);

    if (!match(parser, right)) {
        ZToken *tok = peek(parser);
        error(parser->state, tok, "Expected '%s' to close the list, got '%s'",
                    tokname(right), stoken(tok));
        return NULL;
    }

    return list;
}

/* Parses a list of types delimited by left and right.
 * It assumes that the elements are separated by a TOK_COMMA.
 *
 * Example: [u8, char, [u8]]
 * You can parse this by using this function
 * by calling with TOK_LSBRACKET and TOK_RSBRACKET.
 * */
static ZType **parseTypeList(ZParser *parser, ZTokenType left, ZTokenType right) {
    expect(parser, left);
    ZType **args = NULL;
    do {
        ZType *curr = tryParse(parser, parseType(parser));
        if (!curr) {
            error(parser->state, peek(parser), "Expected a type in type list");
            return NULL;
        }
        vecpush(args, curr);
        if (!match(parser, TOK_COMMA)) break;
        if (check(parser, right)) break;
    } while (true);
    if (!match(parser, right)) {
        error(parser->state, peek(parser), "Expected '%s' to close type list, got '%s'", tokname(right), stoken(peek(parser)));
        return NULL;
    }
    return args;
}

static ZType *applyStarsToType(ZType *base, u8 stars) {
    for (u8 i = 0; i < stars; i++) {
        ZType *node = maketype(Z_TYPE_POINTER);
        node->base = base;
        base = node;
    }
    return base;
}

static ZType *parseTypeArray(ZParser *parser) {
    expect(parser, TOK_LSBRACKET);

    usize size = 0;
    if (check(parser, TOK_INT_LIT)) {
        size = consume(parser)->integer;
    }

    expect(parser, TOK_RSBRACKET);

    ZType *type = parseType(parser);
    ensure(type, "Expected a type after [] brackets");

    ZType *arr = maketype(Z_TYPE_ARRAY);
    arr->array.base = type;
    arr->array.size = size;
    return arr;
}

/* A tuple must have at least 2 types */
static ZType *parseTypeTuple(ZParser *parser) {
    ZType **types = parseTypeList(parser, TOK_LPAREN, TOK_RPAREN);

    if (!types) {
        error(parser->state, peek(parser), "Expected types in tuple type");
        return NULL;
    } else if (veclen(types) == 1) {
        return types[0];
    } else if (veclen(types) < 2) {
        error(parser->state, peek(parser),
                "Tuple type requires at least 2 elements, got %zu",
                veclen(types));
        return NULL;
    }

    ZType *type = maketype(Z_TYPE_TUPLE);
    type->tuple = types;
    return type;
}

static ZType *parseTypeFunc(ZParser *parser, ZType *previous) {
    ZType **args = tryParse(parser, parseTypeList(parser, TOK_LPAREN, TOK_RPAREN));
    if (!args) return previous;
    ZType **generics = NULL;

    if (check(parser, TOK_LSBRACKET)) {
        generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
        if (!generics) {
            return previous;
        }
    }

    ZType *type = maketype(Z_TYPE_FUNCTION);
    type->func.ret = previous;
    type->func.args = args;
    type->func.generics = generics;
    return type;
}

static ZType *parseAtom(ZParser *parser) {
    if (check(parser, TOK_LSBRACKET)) {
        return parseTypeArray(parser);
    } else if (check(parser, TOK_LPAREN)) {
        return parseTypeTuple(parser);
    }

    if (checkMask(parser, TOK_TYPES_MASK) || check(parser, TOK_IDENT)) {
        ZType *base = maketype(Z_TYPE_PRIMITIVE);
        base->primitive.token = consume(parser);
        base->primitive.base     = NULL;
        return base;
    }
    return NULL;
}

static ZType *parseBaseType(ZParser *parser) {
    ZToken *start   = peek(parser);
    bool constant   = match(parser, TOK_CONST);

    u8 stars        = 0;
    while (match(parser, TOK_STAR)) stars++;

    ZType *base     = tryParse(parser, parseAtom(parser));
    ensure(base, "Failed to parse atom type");

    base = applyStarsToType(base, stars);

    base->constant  = constant;

    if (!parser->noFuncType && check(parser, TOK_LPAREN)) {
        base        = parseTypeFunc(parser, base);
    } else if (check(parser, TOK_LSBRACKET)) {
        // Generic type instantiation like List[int] or Map[str, int]
        ZType **generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
        base->primitive.generics = generics;
    }
    base->tok = start;
    return base;
}

static ZType *parseSumType(ZParser *parser) {
    ZType *curr     = parseBaseType(parser);
    ZType **types   = NULL;
    vecpush(types, curr);
    while (match(parser, TOK_BITOR)) {
        curr = tryParse(parser, parseBaseType(parser));

        if (!curr) break;
        vecpush(types, curr);
    }

    if (veclen(types) == 1) {
        return types[0];
    }

    ZType *sumType      = maketype(Z_TYPE_SUM);
    typesSort(types);
    sumType->sumType    = types;

    return sumType;
}

ZType *parseType(ZParser *parser) {
    return parseSumType(parser);
}

static ZNode *parseDefer(ZParser *parser) {
    expect(parser, TOK_DEFER);
    ZNode *expr = parseStmt(parser);

    ensure(expr, "Expected an expression after 'defer' keyword");

    ZNode *node = makenode(NODE_DEFER);
    node->deferStmt.expr = expr;
    return node;
}

static ZNode *parseMatchArm(ZParser *parser, bool asExpr) {
    ZVarDestructPattern *pattern = parseDestructVar(parser, true);
    guard(pattern);

    ZNode *arm              = makenode(NODE_MATCH_ARM);

    if (asExpr && !check(parser, TOK_ARROW)) {
        error(parser->state, peek(parser), "Match expression must have ->");
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
    return arm;
}

static ZNode *parseMatch(ZParser *parser, bool asExpr) {
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
    expect(parser, TOK_RBRACKET);

    ZNode *match        = makenode(NODE_MATCH);
    match->tok          = start;
    match->match.cond   = expr;
    match->match.arms   = arms;
    return match;
}

/* Not handled yet. */
ZNode *expandListMacro(ZParser *parser) {
    if (!parser->macroParser.currentMacro) return NULL;

    expect(parser, TOK_MACRO_EXPR);
    expect(parser, TOK_LPAREN);

    ZNode *stmt = parseStmt(parser);

    // Failed to parse statement
    if (!stmt) return NULL;

    if (!match(parser, TOK_RPAREN)) {
        error(parser->state, peek(parser),
                    "Expected ')' to close the macro pattern");
        return NULL;
    }

    ZNode *node = makenode(NODE_BLOCK);
    node->block = NULL;

    return node;
}

static ZNode *parseCapabilityBlock(ZParser *parser) {
    expect(parser, TOK_WITH);

    ZNode *capability   = parseVarDef(parser);
    if (!capability) {
        error(parser->state, peek(parser), "Invalid expression");
        return NULL;
    }

    ZNode *block                            = parseBlockOrInline(parser);
    ZNode *capabilityBlock                  = makenode(NODE_CAPABILITY);
    capabilityBlock->capability.capability  = capability;
    capabilityBlock->capability.block       = block;
    return capabilityBlock;
}

ZNode *parseStmt(ZParser *parser) {
    guard(canPeek(parser));

    ZTokenType t = peek(parser)->type;


    switch (t) {
    case TOK_IF:        return parseIf              (parser);
    case TOK_FOR:       return parseLoops           (parser);
    case TOK_MATCH:     return parseMatch           (parser, false);
    case TOK_DEFER:     return parseDefer           (parser);
    case TOK_RETURN:    return parseReturn          (parser);
    case TOK_BREAK:     return parseBreak           (parser);
    case TOK_CONTINUE:  return parseContinue        (parser);
    case TOK_WITH:      return parseCapabilityBlock (parser);
    default: {
        ZParseFunc funcs[] = {
            parseVarInferred,
            parseVarDefTyped,
            parseBlock,
            parseUpdate,
            parseExpr
        };
        return parseOrGrammar(parser, funcs, arrlen(funcs));
    }
    }
}

static ZNode *parseBlockOrInline(ZParser *parser) {
    if (match(parser, TOK_ARROW)) {
        ZNode *expr = parseExpr(parser);
        if (!expr) {
            error(parser->state, peek(parser), "Invalid expression");
            return NULL;
        }
        ZNode *body = makenode(NODE_BLOCK);
        vecpush(body->block, expr);
        return body;
    } else if (check(parser, TOK_LBRACKET)) {
        return parseBlock(parser);
    } else {
        error(parser->state, peek(parser), "Unexpected token");
        return NULL;
    }
}

static ZNode *parseBlock(ZParser *parser) {
    let start = peek(parser);
    expect(parser, TOK_LBRACKET);

    ZNode *block = makenode(NODE_BLOCK);
    ZNode *stmt = NULL;
    do {
        stmt = parseStmt(parser);
        if (stmt) vecpush(block->block, stmt);
    } while (stmt);

    if (!check(parser, TOK_RBRACKET)) {
        sinchronize(parser, TOK_RBRACKET);
        ensure(canPeek(parser), "Expected a '}' to close the block");
    }

    expect(parser, TOK_RBRACKET);

    if (veclen(block->block) == 0) {
        warning(parser->state, start, "A block cannot be empty");
    }

    return block;
}

static ZNode *parseField(ZParser *parser) {
    ensure(check(parser, TOK_IDENT), "Expected an identifier");
    ZToken *ident = consume(parser);

    expect(parser, TOK_COLON);

    ZType *type = tryParse(parser, parseType(parser));
    guard(type);

    ZNode *node             = makenode(NODE_FIELD);
    node->field.type        = type;
    node->field.identifier  = ident;
    node->resolved          = type;
    node->tok               = ident;
    return node;
}

static ZNode *parseFieldOptName(ZParser *parser) {
    ZToken *ident = NULL;
    if (check(parser, TOK_IDENT) && checkAhead(parser, TOK_COLON, 1)) {
        ident = consume(parser);
        expect(parser, TOK_COLON);
    } else {
        ZToken *tok = peek(parser);
        if (!tok) return NULL;
        ident = makeident("_", tok->start);
    }

    ZType *type = tryParse(parser, parseType(parser));
    guard(type);

    ZNode *node             = makenode(NODE_FIELD);
    node->field.type        = type;
    node->field.identifier  = ident;
    node->resolved          = type;
    node->tok               = ident;
    return node;
}

static ZNode *parseStructField(ZParser *parser) {
    guard(canPeek(parser));

    if (match(parser, TOK_TRIPLE_DOT)) {
        if (!check(parser, TOK_IDENT)) {
            error(parser->state, peek(parser), "Expected a struct here");
            return NULL;
        }
        ZNode *node             = makenode(NODE_EMBED_FIELD);
        ZType *type             = maketype(Z_TYPE_PRIMITIVE);
        type->primitive.token   = consume(parser);
        node->resolved          = type;
        return node;
    } else {
        return parseField(parser);
    }
}

static ZNode *parseEnumField(ZParser *parser) {
    if (!check(parser, TOK_IDENT)) {
        error(parser->state, peek(parser), "Expected an identifier");
        return NULL;
    }
    ZToken *name = consume(parser);

    ZType **types = NULL;
    if (check(parser, TOK_LPAREN)) {
        types = parseTypeList(parser, TOK_LPAREN, TOK_RPAREN);
        if (!types) {
            error(parser->state, peek(parser), "Failed to parse the type list");
            return NULL;
        }
    }

    ZNode *node                 = makenode(NODE_ENUM_FIELD);
    node->enumField.name        = name;
    node->tok                   = name;
    node->enumField.captured    = types;

    ZType *enm                  = maketype(Z_TYPE_STRUCT);
    enm->strct.name             = name;
    enm->strct.fields           = NULL;

    /* Prepend the flag type. */
    ZNode *field                = makenode(NODE_FIELD);
    field->field.identifier     = NULL;

    ZType *flag                 = maketype(Z_TYPE_PRIMITIVE);
    flag->primitive.token       = maketoken(TOK_U8, NULL);
    field->field.type           = flag;

    vecpush(enm->strct.fields, field);

    for (usize i = 0; i < veclen(types); i++) {
        field                   = makenode(NODE_FIELD);
        field->field.identifier = NULL;
        field->field.type       = types[i];    

        vecpush(enm->strct.fields, field);
    }

    node->resolved = enm;
    return node;
}

static ZNode *parseEnumDecl(ZParser *parser,
    ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_ENUM);

    ZType **generics = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser, true);
    }

    ZNode **fields = parseGenericList(parser,
            TOK_LBRACKET, TOK_RBRACKET,
            parseEnumField, false);

    if (!fields || veclen(fields) < 2) {
        error(parser->state, start, "Expected at least 2 variants");
        return NULL;
    }

    ZNode *node                 = makenode(NODE_ENUM);
    node->enumDef.name          = start;
    node->enumDef.pub           = public;
    node->enumDef.fields        = fields;
    node->enumDef.annotations   = annotations;
    node->tok                   = node->enumDef.name;

    ZType *type                 = maketype(Z_TYPE_ENUM);
    type->enm.name              = start;
    type->enm.generics          = generics;
    type->enm.fields            = NULL;

    for (usize i = 0; i < veclen(fields); i++) {
        vecpush(type->enm.fields, fields[i]->resolved);
    }

    node->resolved          = type;
    
    return node;
}

static ZNode *parseStructDecl(ZParser *parser,
    ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_STRUCT);

    ZType **generics = NULL;

    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser, true);
        if (!generics) {
            error(parser->state, peek(parser),
                    "Expected generic parameters after struct name");
        }
    }

    ZNode **fields = parseGenericList(parser,
            TOK_LBRACKET, TOK_RBRACKET,
            parseStructField, false);

    ZNode *node                 = makenode(NODE_STRUCT);
    node->tok                   = start;
    node->structDef.fields      = fields;
    node->structDef.generics    = generics;
    node->structDef.ident       = start;
    node->structDef.pub         = public;
    node->structDef.annotations = annotations;

    ZType *type                 = maketype(Z_TYPE_STRUCT);
    type->strct.annotations     = annotations;
    type->strct.name            = start;
    type->strct.generics        = generics;
    type->strct.fields          = fields;

    return node;
}

ZNode *parseExpr(ZParser *parser) {
    if (parser->macroParser.currentMacro && match(parser, TOK_MACRO_EXPR)) {
        if (!check(parser, TOK_IDENT)) {
            error(parser->state, peek(parser), "Expected an identifier");
            return NULL;
        }

        ZToken *var         = consume(parser);
        ZNode *currentMacro = parser->macroParser.currentMacro;
        ZNode *placeholder  = getMacroCapturedVar(currentMacro, var);
        if (!placeholder) {
            error(parser->state, peek(parser),
                    "%s is not a valid macro variable", var->str);
            return NULL;
        }
        return placeholder;
    }

    ZToken *curr = peek(parser);
    if (!curr) return NULL;

    switch (curr->type) {
    case TOK_IF:    return parseInlineIf(parser);
    case TOK_MATCH: return parseMatch(parser, true);
    default:        return parseOrGrammar(parser, exprFunc, arrlen(exprFunc));
    }
}

static ZNode *parseReturn(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_RETURN);
    ZNode *ret = makenode(NODE_RETURN);

    ret->returnStmt.expr = parseExpr(parser);
    ret->tok = start;
    return ret;
}

static ZAnnotation *parseAnnotation(ZParser *parser) {
    guard(canPeek(parser));
    guard(check(parser, TOK_IDENT) || peek(parser)->type & TOK_OVERLOADABLE);

    ZToken *name                = consume(parser);
    ZAnnotation **annotations   = NULL;
    ZAnnotation *annotation     = NULL;

    if (match(parser, TOK_LPAREN)) {
        do {
            annotation = parseAnnotation(parser);
            if (!annotation) break;
            vecpush(annotations, annotation);
        } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));
        expect(parser, TOK_RPAREN);
    }
    annotation          = zalloc(ZAnnotation);
    annotation->name    = name;
    annotation->args    = annotations;
    annotation->used    = false;
    return annotation;
}

static ZAnnotation **parseAnnotations(ZParser *parser) {
    expect(parser, TOK_HASHTAG);
    expect(parser, TOK_LSBRACKET);

    ZAnnotation **annotations   = NULL;
    ZAnnotation *annotation     = NULL;

    do {
        annotation = parseAnnotation(parser);
        if (!annotation) break;
        vecpush(annotations, annotation);
    } while (!check(parser, TOK_RSBRACKET) && match(parser, TOK_COMMA));

    expect(parser, TOK_RSBRACKET);

    return annotations;
}

static ZNode *parseCondDestructVar(ZParser *parser) {
    ZVarDestructPattern *pattern = parseDestructVar(parser, true);
    guard(pattern);
    expect(parser, TOK_ASSIGN);
    parser->noStructLit = true;
    ZNode *expr = tryParse(parser, parseExpr(parser));
    parser->noStructLit = false;

    guard(expr);
    
    return makenodevar(pattern, NULL, expr);
}

static ZNode *parseIfLet(ZParser *parser) {
    expect(parser, TOK_IF);
    ZVarDestructPattern *pattern = parseDestructVar(parser, true);
    guard(pattern);
    expect(parser, TOK_ASSIGN);
    bool savedNoStructLit = parser->noStructLit;
    parser->noStructLit = true;
    ZNode *expr = parseExpr(parser);
    parser->noStructLit = savedNoStructLit;
    guard(expr);
    ZNode *body = parseBlockOrInline(parser);

    ZNode *var = makenodevar(pattern, NULL, expr);

    ZNode *elseBranch = NULL;
    if (match(parser, TOK_ELSE)) {
        elseBranch = parseOrGrammar(
            parser, (ZParseFunc[]){ parseIf, parseBlockOrInline }, 2
        );
    }

    ZNode *iflet                = makenode(NODE_IF);
    iflet->ifStmt.cond          = var;
    iflet->ifStmt.body          = body;
    iflet->ifStmt.elseBranch    = elseBranch;
    return iflet;
}

static ZNode *parseIfStmt(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IF);

    parser->noStructLit = true;
    ZNode *cond = parseExpr(parser);
    parser->noStructLit = false;
    guard(cond);

    ZNode *body = parseBlockOrInline(parser);
    guard(body);

    ZNode *node = makenode(NODE_IF);

    if (canPeek(parser) && match(parser, TOK_ELSE)) {
        ZNode *elseBody = parseOrGrammar(parser, (ZParseFunc[]){
            parseIf, parseBlockOrInline
        }, 2);
        if (!elseBody) return NULL;
        node->ifStmt.elseBranch = elseBody;
    }

    node->ifStmt.cond   = cond;
    node->ifStmt.body   = body;
    node->tok           = start;
    return node;
}

static ZNode *parseIf(ZParser *parser) {
    return parseOrGrammar(parser, (ZParseFunc[]){parseIfLet, parseIfStmt}, 2);
}

/* While parsed with 'for' token instead of standard while. */
static ZNode *parseWhile(ZParser *parser) {
    expect(parser, TOK_FOR);

    parser->noStructLit = true;
    ZNode *cond = tryParse(parser, parseExpr(parser));
    parser->noStructLit = false;
    if (!cond) {
        error(parser->state, peek(parser),
                "Expected condition expression after 'while'");
    }
    ZNode *body = tryParse(parser, parseBlockOrInline(parser));

    ZNode *node = makenode(NODE_WHILE);
    node->whileStmt.branch  = body;
    node->whileStmt.cond    = cond;
    return node;
}

static ZNode *parseForIn(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_FOR);

    ZVarDestructPattern *binding = parseDestructVar(parser, true);
    expect(parser, TOK_IN);
    ZNode *iter = tryParse(parser, parseExpr(parser));
    ZNode *block = tryParse(parser, parseBlockOrInline(parser));

    guard(binding && iter && block);

    ZNode *node = makenode(NODE_FORIN);
    node->forin.binding = binding;
    node->forin.iter    = iter;
    node->forin.body    = block;
    node->tok = start;

    return node;
}

static ZNode *parseForLet(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_FOR);

    ZNode *cond = tryParse(parser, parseCondDestructVar(parser));
    ZNode *body = tryParse(parser, parseBlockOrInline(parser));
    guard(body);

    ZNode *node = makenode(NODE_WHILE);
    node->whileStmt.branch  = body;
    node->whileStmt.cond    = cond;
    node->tok               = start;
    return node;
}

static ZNode *parseLoops(ZParser *parser) {
    ZToken *start = peek(parser);
    ensure(check(parser, TOK_FOR), "Expected 'for' keyword");

    // Infinite loop without condition
    if (checkAhead(parser, TOK_LBRACKET, 1)) {
        ZNode *cond             = makenode(NODE_LITERAL);
        cond->tok               = maketoken(TOK_TRUE, NULL);
        cond->literalTok        = cond->tok;
        ZNode *node             = makenode(NODE_WHILE);
        node->tok               = consume(parser);
        node->whileStmt.branch  = parseBlock(parser);
        node->whileStmt.cond    = cond;

        return node;
    }

    ZParseFunc f[] = { parseForLet, parseForIn, parseWhile };
    ZNode *node = parseOrGrammar(parser, f, sizeof(f) / sizeof(f[0]));

    if (node) node->tok = start;

    return node;
}

/* K[V: Display[T] + Drop]
 *
 *
 * generic_arg =
 *              identifier |
 *              identifier ':' generic_decl { '+', generic_decl }
 *
 * generic_decl = ident '[' generic_arg, { ',', generic_arg } ']'
 * */
static ZType *parseGenericDecl(ZParser *);

static ZType *parseGenericArgument(ZParser *parser) {
    ensure(check(parser, TOK_IDENT), "Expected an identifier");

    ZType *generic = maketype(Z_TYPE_GENERIC);
    generic->generic.name = consume(parser);

    generic->generic.extensions = NULL;

    if (match(parser, TOK_COLON)) {
        ZType *arg = parseGenericDecl(parser);
        if (!arg) {
            error(parser->state, peek(parser), "Unexpected token");
            return NULL;
        }

        vecpush(generic->generic.extensions, arg);

        if (!match(parser, TOK_PLUS)) return generic;

        while (true) {
            arg = parseGenericDecl(parser);
            if (!arg) break;
            vecpush(generic->generic.extensions, arg);

            if (!match(parser, TOK_PLUS)) break;
        }
    }

    return generic;
}

static ZType *parseGenericDecl(ZParser *parser) {
    ensure(check(parser, TOK_IDENT), "Expected an identifier");

    ZType *generic              = maketype(Z_TYPE_GENERIC);
    generic->generic.name       = consume(parser);
    generic->generic.extensions = NULL;

    if (match(parser, TOK_LBRACKET)) {
        ZType *argument = parseGenericArgument(parser);

        if (!argument) {
            error(parser->state, peek(parser),
                    "Expected at least one generic argument");
        }
        vecpush(generic->generic.extensions, argument);

        while (true) {
            if (!match(parser, TOK_COMMA)) break;
            if (!check(parser, TOK_IDENT)) break;

            argument = parseGenericArgument(parser);
            if (!argument) break;
            vecpush(generic->generic.extensions, argument);
        }
        expect(parser, TOK_RBRACKET);
    }
    return generic;
}


/*
 *  [K, V]
 *  [K: Display + Drop]
 * */
static ZType **parseGenericsDecl(ZParser *parser, bool brackets) {
    ZType **generics = NULL;
    
    if (brackets) {
        expect(parser, TOK_LSBRACKET);
    }

    ZType *generic = NULL;
    while (true) {
        if (!check(parser, TOK_IDENT)) break;

        ZToken *ident               = consume(parser);
        generic                     = maketype(Z_TYPE_GENERIC);
        generic->generic.name       = ident;
        generic->generic.extensions = NULL;

        if (match(parser, TOK_COLON)) {
            if (!check(parser, TOK_IDENT)) {
                error(parser->state, peek(parser), "Expected a facet here");
                break;
            }
            ZType *extension = maketype(Z_TYPE_PRIMITIVE);
            extension->primitive.token = consume(parser);
            vecpush(generic->generic.extensions, extension);

            while (match(parser, TOK_PLUS)) {
                if (!check(parser, TOK_IDENT)) {
                    error(parser->state, peek(parser), "Expected a facet here");
                    break;
                }
                extension = maketype(Z_TYPE_PRIMITIVE);
                extension->primitive.token = consume(parser);
                vecpush(generic->generic.extensions, extension);
            }
        }

        vecpush(generics, generic);
        if (!match(parser, TOK_COMMA)) break;
        if (check(parser, TOK_RBRACKET)) break;
    }


    if (brackets) {
        expect(parser, TOK_RSBRACKET);
    }

    return generics;
}

/* The caller must handle:
 * - the mangling name.
 * - the receiver node if it is a receiver function.
 * - the base type if it is a static function.
 *   */
static ZNode *parseFuncDecl(ZParser *parser,
    ZAnnotation **annotations, bool public) {
    ZToken *start = peek(parser);

    if (!check(parser, TOK_IDENT)) return NULL;
    ZToken *name = consume(parser);

    ZType **generics = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser, true);
        if (!generics) {
            error(parser->state, peek(parser),
                    "Expected generic type parameters after function name");
        }
    }

    expect(parser, TOK_DOUBLE_COLON);

    bool prev = parser->noFuncType;
    parser->noFuncType = true;
    ZType *ret = tryParse(parser, parseType(parser));
    parser->noFuncType = prev;

    if (!ret) {
        error(parser->state, start, "Expected return type after '::'");
        return NULL;
    }

    ZNode **args = parseGenericList(parser,
        TOK_LPAREN,     TOK_RPAREN,
        parseField,     true
    );

    ZNode **capabilities = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        capabilities = parseGenericList(
            parser,
            TOK_LSBRACKET,      TOK_RSBRACKET,
            parseFieldOptName,  true
        );
    }

    ZNode *body = NULL;

    if (match(parser, TOK_ARROW)) {
        ZNode *expr = tryParse(parser, parseExpr(parser));
        if (expr) {
            ZNode *ret = makenode(NODE_RETURN);
            ret->returnStmt.expr = expr;
            body = makenode(NODE_BLOCK);
            vecpush(body->block, ret);
        } else {
            error(parser->state, peek(parser), "Failed to parse inline return");
            return NULL;
        }
    } else if (check(parser, TOK_LBRACKET)) {
        body = tryParse(parser, parseBlock(parser));
    } else {
        error(parser->state, peek(parser), "Unexpected token");
    }

    if (!body) {
        error(parser->state, peek(parser),
                "Expected function body '{...}' after declaration");
    }

    ZType *func                 = maketype(Z_TYPE_FUNCTION);
    func->func.ret              = ret;
    func->func.args             = NULL;
    func->func.capabilities     = NULL;

    for (usize i = 0; i < veclen(args); i++)
        vecpush(func->func.args, args[i]->field.type);
    for (usize i = 0; i < veclen(capabilities); i++)
        vecpush(func->func.capabilities, capabilities[i]->field.type);

    char *mangled = strcmp(name->str, "main") == 0 ? name->str :
        mangler((char *[]){
            name->filename,
            name->str,
            NULL
        });

    ZNode *node                 = makenode(NODE_FUNC);
    node->tok                   = name;
    node->resolved              = func;
    node->funcDef.ret           = ret;
    node->funcDef.name          = name;
    node->funcDef.args          = args;
    node->funcDef.body          = body;
    node->funcDef.pub           = public;
    node->funcDef.generics      = generics;
    node->funcDef.base          = NULL;
    node->funcDef.receiver      = NULL;
    node->funcDef.mangled       = mangled;
    node->funcDef.annotations   = annotations;
    node->funcDef.capabilities  = capabilities;

    return node;
}

static ZVarDestructPattern *parseDestructVar(ZParser *parser, bool conditional) {
    guard(canPeek(parser));

    ZVarDestructPattern *cur    = NULL;
    ZVarDestructPattern **list  = NULL;

    if (conditional) {
        bool isSumPattern =
            checkMask(parser, TOK_TYPES_MASK) ||
            check(parser, TOK_STAR) ||
            (check(parser, TOK_IDENT) && checkAhead(parser, TOK_LPAREN, 1));

        if (isSumPattern) {
            ZToken *start       = peek(parser);
            parser->noFuncType  = true;
            ZType *sumType      = parseType(parser);
            parser->noFuncType  = false;

            expect(parser, TOK_LPAREN);
            ZVarDestructPattern *child = parseDestructVar(parser, conditional);
            expect(parser, TOK_RPAREN);

            cur         = makeVarDestructPattern(Z_VAR_SUM);
            cur->tok    = start;
            cur->sum.type  = sumType;
            cur->sum.child = child;
            return cur;
        }
    }

    ZToken *tok                 = consume(parser);

    if (tok->type & TOK_LITERAL) {
        cur = makeVarDestructPattern(Z_VAR_LIT);
        cur->ident = tok;
    } else if (tok->type == TOK_IDENT) {
        if (conditional && match(parser, TOK_DOUBLE_COLON)) {
            if (!check(parser, TOK_IDENT)) {
                error(parser->state, peek(parser), "Expected identifier");
            }
            cur = makeVarDestructPattern(Z_VAR_ENUM);
            cur->base = tok;
            cur->prop = consume(parser);
            cur->args = NULL;

            expect(parser, TOK_LPAREN);
            if (!check(parser, TOK_RPAREN)) {
                do {
                    ZVarDestructPattern *item = parseDestructVar(parser, conditional);
                    if (!item) break;
                    vecpush(cur->args, item);
                } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));
            }
            expect(parser, TOK_RPAREN);
        } else {
            cur = makeVarDestructPattern(Z_VAR_IDENT);
            cur->ident = tok;
        }
    } else if (tok->type == TOK_LBRACKET) {
        ZToken *key = NULL;
        while (true) {
            if (!check(parser, TOK_IDENT)) break;
            key = consume(parser);

            if (check(parser, TOK_COMMA) || check(parser, TOK_RBRACKET)) {
                cur = makeDestructIdent(key);
            } else if (match(parser, TOK_COLON)) {
                cur = parseDestructVar(parser, conditional);
            } else {
                error(parser->state, peek(parser),
                        "Unexpected token");
                break;
            }

            if (!parser) {
                error(parser->state, key, "Failed to deconstruct %s",
                        stoken(key));
                return NULL;
            }

            ZVarDestructPattern *pair = makeVarDestructPattern(Z_VAR_PAIR);
            pair->key = key;
            pair->value = cur;

            vecpush(list, pair);

            if (!match(parser, TOK_COMMA)) break;
            if (check(parser, TOK_RBRACKET)) break;
        }
        expect(parser, TOK_RBRACKET);

        cur = makeVarDestructPattern(Z_VAR_STRUCT);
        cur->fields = list;
    } else if (tok->type == TOK_LPAREN) {
        do {
            cur = parseDestructVar(parser, conditional);
            if (!cur) break;
            vecpush(list, cur);
        } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));

        expect(parser, TOK_RPAREN);

        cur = makeVarDestructPattern(Z_VAR_TUPLE);
        cur->tuple = list;
    } else {
        error(parser->state, tok, "Cannot deconstruct variable");
        return NULL;
    }

    cur->tok = tok;
    cur->resolved = NULL;
    return cur;
}

static ZNode *parseVarInferred(ZParser *parser) {
    ZVarDestructPattern *pattern = parseDestructVar(parser, false);

    expect(parser, TOK_ASSIGN);
    ZNode *expr = tryParse(parser, parseExpr(parser));

    if (!expr) {
        error(parser->state, peek(parser), "Expected expression after ':='");
    }

    return makenodevar(pattern, NULL, expr);
}

static ZNode *parseVarDefTyped(ZParser *parser) {
    ZToken *start = peek(parser);

    if (!check(parser, TOK_IDENT) && !check(parser, TOK_LPAREN) && !check(parser, TOK_LBRACKET)) {
        error(parser->state, start, "Expected an identifier or destructure pattern");
        return NULL;
    }
    // else if (!start->newlineBefore) {
    //     error(parser->state, start,
    //             "Variable declaration must be defined in the same line");
    // }

    ZVarDestructPattern *var = parseDestructVar(parser, false);

    expect(parser, TOK_COLON);

    ZType *type = parseType(parser);
    guard(type);

    ZNode *expr = NULL;

    expect(parser, TOK_EQ);
    
    expr = tryParse(parser, parseExpr(parser));
    if (!expr) {
        error(parser->state, peek(parser), "Expected expression after '='");
        return NULL;
    }
    
    return makenodevar(var, type, expr);
}

static ZNode *parseVarDef(ZParser *parser) {
    ZParseFunc func[] = { parseVarInferred, parseVarDefTyped };
    return parseOrGrammar(parser, func, 2);
}

static ZNode *parseBreak(ZParser *parser) {
    ZNode *node = makenode(NODE_BREAK);
    node->tok = consume(parser);
    return node;
}

static ZNode *parseContinue(ZParser *parser) {
    ZNode *node = makenode(NODE_CONTINUE);
    node->tok = consume(parser);

    return node;
}

static ZNode *parseTupleLit(ZParser *parser) {
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

    ZNode *node = makenode(NODE_TUPLE_LIT);
    node->tuplelit = fields;

    if (veclen(fields) < 2) {
        error(parser->state, start, "Expected at least 2 itesm");
    }

    return node;
}

static ZNode *parseArrayLit(ZParser *parser) {
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
        sinchronize(parser, TOK_RSBRACKET);
    }

    expect(parser, TOK_RSBRACKET);

    ZNode *node = makenode(NODE_ARRAY_LIT);

    node->arraylit  = values;
    node->tok       = start;

    return node;
}

static ZNode *parseStructLit(ZParser *parser) {
    if (parser->noStructLit) return NULL;
    if (!check(parser, TOK_IDENT)) return NULL;

    ZToken *ident = consume(parser);
    ZType **generics = NULL;

    if (check(parser, TOK_LSBRACKET)) {
        generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
        if (!generics) {
            error(parser->state,
                        peek(parser),
                        "Expected generic type arguments in '[...]'");
            return NULL;
        }
    }

    expect(parser, TOK_LBRACKET);

    ZNode *structlit = makenode(NODE_STRUCT_LIT);
    structlit->structlit.ident      = ident;
    structlit->structlit.generics   = generics;
    structlit->tok                  = ident;

    while (true) {
        if (!check(parser, TOK_IDENT)) break;
        ZVarDestructPattern *node = makeDestructIdent(consume(parser));

        if (!match(parser, TOK_COLON)) {
            error(parser->state,
                        peek(parser),
                        "Expected a ':', got %s",
                        stoken(peek(parser)));
            return NULL;
        }

        ZNode *expr = tryParse(parser, parseExpr(parser));

        if (!expr) return NULL;

        ZNode *var = makenodevar(node, NULL, expr);
        vecpush(structlit->structlit.fields, var);
        if (check(parser, TOK_RBRACKET)) break;
        if (!match(parser, TOK_COMMA)) {
            break;
        }
    }

    expect(parser, TOK_RBRACKET);

    return structlit;
}

static ZNode *getModuleByName(ZParser *parser, ZToken **module, bool external) {
    char *filename = NULL;
    usize len = veclen(module);
    if (len == 0) return NULL;

    if (external) {
        const char *home = parser->state->homePath;
        if (!home) home = "";

        vecunion(filename, home, strlen(home));
        vecpush(filename, sep);
        vecunion(filename, ".zinc", 5);
        vecpush(filename, sep);
        vecunion(filename, "packages", 8);
        vecpush(filename, sep);

        const char *pkg = module[0]->str;
        vecunion(filename, pkg, strlen(pkg));
        vecpush(filename, sep);

        if (len > 1) {
            for (usize i = 1; i < len; i++) {
                const char *seg = module[i]->str;
                vecunion(filename, seg, strlen(seg));
                if (i < len - 1) vecpush(filename, sep);
            }
        } else {
            vecunion(filename, ENTRY_IMPORT_FILE, strlen(ENTRY_IMPORT_FILE));
        }
    } else {
        for (usize i = 0; i < len; i++) {
            const char *seg = strcmp(module[i]->str, "super") == 0 ?
                ".." :
                module[i]->str;
            vecunion(filename, seg, strlen(seg));
            if (i < len - 1) vecpush(filename, sep);
        }
    }
    vecunion(filename, ".zn", 3);
    vecpush(filename, '\0');

    bool canVisit = visit(parser->state, filename, external);
    ZNode *node = makenode(NODE_MODULE);

    if (!canVisit) {
        node->module.name = filename;
        node->module.root = NULL;
        return node;
    }

    ZToken **tokens = ztokenize(parser->state);

    node = zparse(parser->state, tokens);

    undoVisit(parser->state);
    return node;
}

static ZNode *parseImport(ZParser *parser) {
    expect(parser, TOK_MODULE);

    bool isStd = match(parser, TOK_LT);

    ZToken **module = NULL;
    ZToken *segment;
    while (canPeek(parser)) {
        if (peek(parser)->newlineBefore) break;
        if (!check(parser, TOK_IDENT)) {
            error(parser->state, peek(parser), "Unexpected token");
        }

        segment = consume(parser);
        vecpush(module, segment);
        if (check(parser, TOK_GT)) break;
        if (!match(parser, TOK_DOUBLE_COLON)) {
            break;
        }
    }

    if (isStd) expect(parser, TOK_GT);

    return getModuleByName(parser, module, isStd);
}

static ZNode *parseTypedef(ZParser *parser, bool public) {
    ZToken *alias = peek(parser);
    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_TYPEDEF);

    ZType *type = tryParse(parser, parseType(parser));
    
    ensure(type, "Invalid type");

    ZNode *node         = makenode(NODE_TYPEDEF);
    node->typeDef.alias = alias;
    node->typeDef.type  = type;
    node->typeDef.pub   = public;
    node->tok           = alias;
    return node;
}

static ZType *parseFuncType(ZParser *parser) {
    ZToken *start   = peek(parser);

    bool prev = parser->noFuncType;
    parser->noFuncType = true;
    ZType *ret      = tryParse(parser, parseType(parser));
    parser->noFuncType = prev;
    guard(ret);
    expect(parser, TOK_LPAREN);

    ZType **args    = NULL;
    ZType *arg      = NULL;
    bool variadic   = false;

    do {
        if (match(parser, TOK_TRIPLE_DOT)) {
            variadic = true;
            // break;
        } else {
            arg = tryParse(parser, parseType(parser));
            if (arg) vecpush(args, arg);
        }
    } while (!check(parser, TOK_RPAREN) && match(parser, TOK_COMMA));

    expect(parser, TOK_RPAREN);

    ZType *func         = maketype(Z_TYPE_FUNCTION);
    func->func.ret      = ret;
    func->func.args     = args;
    func->func.generics = NULL;
    func->func.variadic = variadic;
    func->tok           = start;
    return func;
}

static ZNode *parseForeignBlock(ZParser *parser, bool public) {
    ZToken *start = peek(parser);
    ZToken *lib = start;

    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_FOREIGN);
    expect(parser, TOK_LBRACKET);

    ZType *func         = NULL;
    ZNode *namespace    = makenode(NODE_NAMESPACE);
    namespace->tok      = lib;
    namespace->block    = NULL;
    namespace->pub      = public;
    ZNode *node         = NULL;


    ZToken *name = NULL;
    while (!check(parser, TOK_RBRACKET)) {
        public = match(parser, TOK_PUB);
        name = peek(parser);

        expect(parser, TOK_IDENT);
        expect(parser, TOK_DOUBLE_COLON);

        func = parseFuncType(parser);
        if (!func) {
            error(parser->state, peek(parser), "Error parsing function");
            return NULL;
        }
        node                    = makenode(NODE_FOREIGN);
        node->foreignFunc.ret   = func->func.ret;
        node->foreignFunc.tok   = name;
        node->foreignFunc.args  = func->func.args;
        node->foreignFunc.pub   = public;
        node->tok               = name;
        node->resolved          = func;

        vecpush(namespace->block, node);
    }
    expect(parser, TOK_RBRACKET);

    return namespace;
}

static ZNode *parseForeignInlineDecl(ZParser *parser, bool public) {
    expect(parser, TOK_FOREIGN);
    ZToken *start = peek(parser);
    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);

    ZType *func = parseFuncType(parser);
    guard(func);

    ZNode *node = makenode(NODE_FOREIGN);
    node->foreignFunc.ret   = func->func.ret;
    node->foreignFunc.tok   = start;
    node->foreignFunc.args  = func->func.args;
    node->foreignFunc.pub   = public;
    node->tok               = start;
    node->resolved          = func;
    return node;
}

static ZNode *parseForeignUse(ZParser *parser, bool public) {
    expect(parser, TOK_FOREIGN);
    expect(parser, TOK_MODULE);
    if (!check(parser, TOK_STR_LIT)) {
        error(parser->state, peek(parser), "foreign use expects a string literal");
    }
    ZToken *import = consume(parser);
    // convertHeaderToZNode(parser, import);
    return NULL;
}

/* Parse the pattern of the macro.
 * The pattern can be formed by a combination of these elements:
 * - ident: this captures an identifier (like a variable name or the name of a struct ...).
 * - key: tcaptures a keyword like if or for.
 * - expr: captures an expression (expressions is whatever you get a result).
 * - block: captures a block of code like function body or the body of a loop.
 * - stmt: captures a statement.
 * 
 * These operations can compbined with each other into a list:
 * - a sequence: the pattern must follow all elements in the sequence.
 *   When the parser see elements separated by a space it parses elements like a sequence.
 * - optional [element]: an optional element wrapped by '[]' is an element not required.
 *   An example is the 'else' after the if block.
 * - zero or more (element)+: captures a list of zero or more elements.
 * - one or more (element)*: captures a list of at least one element.
 *
 * Note: you cannot define a pattern that can be accepted by an empty pattern.
 * */
static ZMacroPattern *macroPatternElement(ZParser *parser, ZNode *macro) {
    ZMacroPattern *self = zalloc(ZMacroPattern);
    ZMacroVar *var = zalloc(ZMacroVar);

    // Arrows break the pattern parsing
    if (match(parser, TOK_ARROW)) return NULL;

    if (match(parser, TOK_MACRO_IDENT)) {
        if (!check(parser, TOK_IDENT)) {
            error(parser->state, peek(parser), "Expected identifier after '$'");
            return NULL;
        }
        self->kind = Z_MACRO_IDENT;
        self->ident = consume(parser);
        var->name = self->ident;
        var->startIndex = 0;
        var->endIndex = 0;
        var->useCount = 0;
        vecpush(macro->macro.captured, var);
        return self;

    } else if (match(parser, TOK_HASHTAG)) {
        if (!check(parser, TOK_IDENT)) {
            return NULL;
        }
        self->kind = Z_MACRO_TYPE;
        self->ident = consume(parser);
        var->name = self->ident;
        var->startIndex = 0;
        var->endIndex = 0;
        vecpush(macro->macro.captured, var);
        return self;
    } else if (match(parser, TOK_MACRO_EXPR)) {
        if (match(parser, TOK_LPAREN)) {
            //TODO: handle properly sequences
            ZMacroPattern *seq = parseMacroPattern(parser, macro);
            if (!match(parser, TOK_RPAREN)) {
                error(parser->state,
                            peek(parser),
                            "Expected ')' to close macro sequence");
                return NULL;
            }

            if (match(parser, TOK_PLUS)) {
                self->kind = Z_MACRO_OM;
                self->oneOrMore = seq;
            } else if (match(parser, TOK_STAR)) {
                self->kind = Z_MACRO_ZM;
                self->zeroOrMore = seq;
            }
            return self;
        } else if (check(parser, TOK_IDENT)) {
            self->kind = Z_MACRO_EXPR;
            self->ident = consume(parser);
            var->name = self->ident;
            var->startIndex = 0;
            var->endIndex = 0;
            vecpush(macro->macro.captured, var);
            return self;
        }
    } else if (match(parser, TOK_QUOTE)) {
        // Treat next token as a literal keyword regardless of type
        if (!canPeek(parser)) return NULL;
        self->kind = Z_MACRO_KEY;
        self->ident = consume(parser);
        return self;
    } else if (checkMask(parser, TOK_EXPANDABLE)) {
        self->kind = Z_MACRO_KEY;
        self->ident = consume(parser);
        return self;
    }
    return NULL;
}

static ZMacroPattern *parseMacroPattern(ZParser *parser, ZNode *macro) {
    ZMacroPattern *seq = zalloc(ZMacroPattern);
    seq->kind = Z_MACRO_SEQ;
    seq->sequence = NULL;
    ZMacroPattern *curr = NULL;
    while (( curr = macroPatternElement(parser, macro) )) {
        vecpush(seq->sequence, curr);
    }
    return seq;
}

static void skipBlock(ZParser *parser) {
    i64 level = 1;  // Already inside { from caller

    while (level > 0) {
        if (check(parser, TOK_LBRACKET)) level++;
        else if (check(parser, TOK_RBRACKET)) level--;
        consume(parser);
    }
}

static ZNode *parseMacro(ZParser *parser) {
    ZToken *start = peek(parser);
    bool public = match(parser, TOK_PUB);
    expect(parser, TOK_MACRO);

    if (!checkMask(parser, TOK_EXPANDABLE)) {
        error(parser->state,
                    peek(parser),
                    "Expected overridable keyword after 'macro', got '%s'",
                    stoken(peek(parser)));
        return NULL;
    }

    ZNode *node = makenode(NODE_MACRO);
    node->macro.captured = NULL;
    node->macro.start = start;
    node->macro.pub = public;

    usize saved = parser->source->current;
    ZMacroPattern *pattern = parseMacroPattern(parser, node);

    if (!pattern || veclen(pattern->sequence) < 1) {
        error(parser->state, peek(parser), "Macro pattern is empty or invalid");
        return NULL;
    }

    node->macro.pattern = pattern;
    node->macro.sourceTokens = parser->source->list;

    // Arrow (->) is already consumed by macroPatternElement as the sentinel
    expect(parser, TOK_LBRACKET);  // Consume opening '{'

    node->macro.startBody = parser->source->current;  // First token after {
    skipBlock(parser);
    node->macro.endBody = parser->source->current - 1;  // Exclude closing }

    if (node->macro.endBody - node->macro.startBody == 0) {
        error(parser->state, start, "Body's macro cannot be empty");
        return NULL;
    }

    node->macro.consumed = parser->source->current - saved;

    
    vecpush(parser->macroParser.macros, node);

    return node;
}

/* Macros captured the start token. Skip over the macro declaration in pass 2. */
static ZNode *skipMacro(ZParser *parser, bool public) {
    (void)public;
    ZToken *curr = peek(parser);

    if (!match(parser, TOK_MACRO)) return NULL;

    ZNode *macro = NULL;

    for (usize i = 0; i < veclen(parser->macroParser.macros) && !macro; i++) {
        if (parser->macroParser.macros[i]->macro.start == curr) {
            macro = parser->macroParser.macros[i];
        }
    }

    if (!macro) return NULL;

    usize toConsume = macro->macro.consumed;
    while (toConsume --> 0) {
        consume(parser);
    }
    return macro;
}


static bool checkPubMacro(ZParser *parser) {
    if (!check(parser, TOK_PUB)) return false;
    if (parser->source->current + 1 >= parser->source->end) return false;
    return parser->source->list[parser->source->current + 1]->type == TOK_MACRO;
}

static void discoverMacros(ZParser *parser) {
    usize saved = parser->source->current;
    while (canPeek(parser)) {
        if (check(parser, TOK_MACRO) || checkPubMacro(parser)) {
            parseMacro(parser);
        } else {
            consume(parser);
        }
    }
    parser->source->current = saved;
}

static ZNode *parseModule(ZParser *parser) {
    ZNode *root = makenode(NODE_MODULE);

    root->module.root = NULL;
    root->module.name = parser->state->filename;

    while (canPeek(parser)) {
        ZNode *child = parse(parser);
        if (!child) break;

        // Block of functions
        if (child->type == NODE_BLOCK) {
            for (usize i = 0; i < veclen(child->block); i++) {
                vecpush(root->module.root, child->block[i]);
            }
        } else {
            vecpush(root->module.root, child);
        }
    }
    return root;
}

static ZNode *parseImpl(ZParser *parser, bool public) {
    ZType *type = parseType(parser);
    ZToken *rec = NULL;

    guard(type);

    if (check(parser, TOK_IDENT)) rec = consume(parser);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_IMPL);

    /* Declare facets this block must implement. */
    ZType **facets = NULL;
    if (check(parser, TOK_IDENT)) {
        do {

        ZType *facet            = maketype(Z_TYPE_PRIMITIVE);
        facet->primitive.token  = consume(parser);
        facet->tok              = facet->primitive.token;
        vecpush(facets, facet);
        } while (!check(parser, TOK_LBRACKET) && match(parser, TOK_PLUS));
    }

    /* Declare generics that every function in this block inherit. */
    ZType **generics = NULL;
    if (match(parser, TOK_WHERE)) {
        generics = parseGenericsDecl(parser, false);

        if (!generics) {
            error(parser->state, peek(parser), "Generics failed to parse");
        }
    }

    expect(parser, TOK_LBRACKET);

    ZNode *func                 = NULL;

    ZNode *block                = makenode(NODE_IMPL);
    block->impl.base            = NULL;
    block->impl.self            = NULL;
    block->impl.funcs           = NULL;
    block->impl.facets          = facets;
    block->impl.generics        = generics;
    block->impl.pub             = public;

    ZAnnotation **annotations   = NULL;
    while (true) {
        annotations = NULL;
        if (check(parser, TOK_HASHTAG) && checkAhead(parser, TOK_LSBRACKET, 1)) {
            annotations = parseAnnotations(parser);
        }
        public = match(parser, TOK_PUB);
        func = parseFuncDecl(parser, annotations, public);
        if (!func) {
            if (public) {
                error(parser->state, peek(parser),
                        "Expected a function declaration after 'pub'");
                return NULL;
            }
            break;
        }
        vecpush(block->impl.funcs, func);

        if (check(parser, TOK_RBRACKET)) break;
    }

    expect(parser, TOK_RBRACKET);

    usize len = veclen(block->impl.funcs);

    if (len == 0) return block;

    /* For static mangling we key on the primitive type name (matching what the
     * call-site parser emits for `Type::method`). Strip pointer levels to get
     * the base primitive token. */
    ZType *baseType = type;
    while (baseType->kind == Z_TYPE_POINTER) baseType = baseType->base;
    ZToken *typeNameTok = baseType->primitive.token;

    block->impl.base = type;
    block->impl.self = rec;
    if (rec) { // receiver functions
        ZNode *receiver = makenode(NODE_FIELD);
        receiver->field.identifier = rec;
        receiver->field.type = type;

        for (usize i = 0; i < len; i++) {
            /* manglerM encodes the full receiver type (pointer or not), so
             * `for String self` and `for *String self` get distinct names. */
            block->impl.funcs[i]->funcDef.mangled = manglerM(
                type,
                block->impl.funcs[i]->funcDef.name
            );
            block->impl.funcs[i]->funcDef.receiver = receiver;
        }
    } else { // static functions
        for (usize i = 0; i < len; i++) {
            block->impl.funcs[i]->funcDef.mangled = mangler((char *[]) {
                typeNameTok->str,
                block->impl.funcs[i]->funcDef.name->str,
                NULL
            });
            block->impl.funcs[i]->funcDef.base = type;

            vecunion(block->impl.funcs[i]->funcDef.generics,
                    generics, veclen(generics));
        }
    }

    return block;
}

static ZNode *parseFacet(ZParser *parser, bool public) {
    ZToken *start = peek(parser);

    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);
    expect(parser, TOK_FACET);
    expect(parser, TOK_LBRACKET);

    ZNode *facet        = makenode(NODE_FACET);
    facet->facet.name   = start;
    facet->facet.pub    = public;
    facet->facet.funcs  = NULL;

    do {
        if (!check(parser, TOK_IDENT)) break;
        ZToken *name = consume(parser);

        if (!match(parser, TOK_DOUBLE_COLON)) break;

        ZType *func = parseFuncType(parser);

        ZNode *field = makenode(NODE_FIELD);

        field->field.identifier = name;
        field->field.type       = func;
        field->resolved         = func;

        vecpush(facet->facet.funcs, field);
    } while (!check(parser, TOK_RBRACKET));
    
    expect(parser, TOK_RBRACKET);

    if (veclen(facet->facet.funcs) == 0) {
        error(parser->state, start, "Expected at least one function");
    }

    ZType *type         = maketype(Z_TYPE_FACET);
    type->tok           = start;
    type->facet.name    = start;
    type->facet.funcs   = facet->facet.funcs;
    facet->resolved     = type;

    return facet;
}

static ZNode *parseConst(ZParser *parser) {
    ZToken *start = peek(parser);

    expect(parser, TOK_IDENT);
    expect(parser, TOK_DOUBLE_COLON);

    ZNode *expr = parseExpr(parser);

    ZVarDestructPattern *pattern = makeDestructIdent(start);
    return makenodevar(pattern, NULL, expr);
}

static ZNode *parse(ZParser *parser) {
    guard(canPeek(parser));
    ZToken *start = peek(parser);
    ZTokenType t = start->type;
    ZAnnotation **annotations = NULL;

    if (t == TOK_MODULE) {
        return parseImport(parser);
    } else if (t == TOK_HASHTAG && checkAhead(parser, TOK_LSBRACKET, 1)) {
        annotations = parseAnnotations(parser);
    }

    bool public = match(parser, TOK_PUB);

    if (check(parser, TOK_FOREIGN)) {
        if (checkAhead(parser, TOK_IDENT, 1)) {
            return parseForeignInlineDecl(parser, public);
        } else if (checkAhead(parser, TOK_MODULE, 1)) {
            return parseForeignUse(parser, public);
        }
    }

    ZParserSnapshot *snap = store(parser);

    ZType *base = parseType(parser);
    guard(base);
    if (check(parser, TOK_IDENT)) {
        undo(parser, snap);
        return parseImpl(parser, public);
    }
    expect(parser, TOK_DOUBLE_COLON);
    guard(canPeek(parser));
    t = peek(parser)->type;

    undo(parser, snap);

    switch (t) {
    case TOK_FACET:     return parseFacet       (parser, public);
    case TOK_FOREIGN:   return parseForeignBlock(parser, public);
    case TOK_IMPL:      return parseImpl        (parser, public);
    case TOK_TYPEDEF:   return parseTypedef     (parser, public);
    case TOK_MACRO:     return skipMacro        (parser, public);
    case TOK_STRUCT:    return parseStructDecl  (parser, annotations, public);
    case TOK_ENUM:      return parseEnumDecl    (parser, annotations, public);
    default: {

        ZNode *res = tryParse(
            parser, parseFuncDecl(parser, annotations, public)
        );
        if (res) return res;

        res = parseConst(parser);
        if (res) return res;

        error(parser->state,
            peek(parser),
            "Unexpected token '%s', expected a top-level declaration",
            stoken(peek(parser)));

        return NULL;
    }
    }
}

ZNode *zparse(ZState *state, ZToken **tokens) {
    state->currentPhase = Z_PHASE_SYNTAX;
    ZParser *parser = makeparser(state, tokens);

    discoverMacros(parser);

    ZNode *root = parseModule(parser);

    if (canPeek(parser)) {
        error(state,
                peek(parser),
                "Unexpected token '%s', expected a top-level declaration",
                stoken(peek(parser)));
    }

    return root;
}
