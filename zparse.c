#include "zinc.h"
#include "zmem.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

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

typedef ZNode *(*ZParseFunc)(ZParser *);

ZType *parseType                            (ZParser *);
ZNode *parseExpr                            (ZParser *);
static ZNode *parse                         (ZParser *);
static ZNode *parseIf                       (ZParser *);
static ZNode *parseBreak                    (ZParser *);
static ZNode *parseBlock                    (ZParser *);
static ZNode *parseMatch                    (ZParser *);
static ZNode *parseDefer                    (ZParser *);
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

/* File-level parsing functions */
static ZNode *parseImport                   (ZParser *);
static ZNode *skipMacro                     (ZParser *, bool);
static ZNode *parseTypedef                  (ZParser *, bool);
static ZNode *parseFuncDecl                 (ZParser *, bool);
static ZNode *parseEnumDecl                 (ZParser *, bool);
static ZNode *parseStructDecl               (ZParser *, bool);
static ZNode *parseForeignDecl              (ZParser *, bool);

static ZType **parseGenericsDecl            (ZParser *, bool);
static ZMacroPattern *parseMacroPattern     (ZParser *, ZNode *);

static ZParseFunc exprFunc[] = {
    // parseStructLit,
    // parseArrayLit,
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

/* Since this function tries to parse macros for now i save the current source of tokens
 * such that after the execution of the function i can undo the modification. */
static ZNode *wrapNode(ZParser *parser, ZParseFunc parse) {
    ZParserSnapshot *saved = store(parser);

    pushErrorCheckpoint(parser);
    ZNode *res = parse(parser);

    if (!res) {
        // Consumed parser->tokenIndex - tokIndex
        undo(parser, saved);
        rollbackErrors(parser);
    } else {
        commitErrors(parser);
    }
    return res;
}

static ZType *wrapType(ZParser *parser, ZType *(*parse)(ZParser *)) {
    ZParserSnapshot *saved = store(parser);

    pushErrorCheckpoint(parser);
    ZType *res = parse(parser);

    if (!res) {
        undo(parser, saved);
        rollbackErrors(parser);
    } else {
        commitErrors(parser);
    }
    return res;
}

static ZNode *parseOrGrammar(ZParser *parser, ZParseFunc *pf, usize len) {
    for (usize i = 0; i < len; i++) {
        ZNode *parsed = wrapNode(parser, pf[i]);
        if (parsed) return parsed;
    }
    return NULL;
}

static ZNode *parseGenericBinary(ZParser *parser,
                                    ZParseFunc parseLeft,
                                    ZParseFunc parseRight,
                                    ZTokenType *validTokens,
                                    size_t validTokensLen) {
    ZNode *node = NULL;

    ZNode *left = wrapNode(parser, parseLeft);

    guard(left);

    while (canPeek(parser) &&
                    isValidToken(parser, validTokens, validTokensLen) &&
                    !peek(parser)->newlineBefore) {
        node = makenode(NODE_BINARY);
        ZToken *op = consume(parser);

        ZNode *right = wrapNode(parser, parseRight);

        if (!right) {
            node = NULL;
            break;
        }

        node->binary.op = op;
        node->binary.left = left;
        node->binary.right = right;
        node->tok = op;
        left = node;
    }

    return node ? node : left;
}

static ZNode *parseArrayInit(ZParser *parser) {
    ZType *arr = parseTypeArray(parser);

    guard(arr);
    guard(arr->array.size > 0);

    ZNode *node = makenode(NODE_ARRAY_INIT);
    node->arrayinit = arr;

    return node;
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
        ZNode *node          = wrapNode(parser, parseExpr);
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

            ZToken *segments[] = {
                node->staticAccess.base,
                node->staticAccess.prop,
                NULL
            };

            node->staticAccess.mangled = mangler(segments);
            return node;
        }
        if (!parser->noStructLit && checkAhead(parser, TOK_LBRACKET, 1)) {
            return parseStructLit(parser);
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
    } else if (match(parser, TOK_SIZEOF)) {
        ZToken *tok         = peek(parser);
        ZType *type         = parseType(parser);
        if (!type) {
            error(parser->state, tok, "Expected type argument to sizeof");
            return NULL;
        }
        ZNode *node = makenode(NODE_SIZEOF);
        node->sizeofExpr.type   = type;
        node->tok               = tok;
        return node;
    }

    return NULL;
}

static ZNode *parseArrSubscript(ZParser *parser, ZNode *previous) {
    expect(parser, TOK_LSBRACKET);
    ZNode *index = wrapNode(parser, parseExpr);
    expect(parser, TOK_RSBRACKET);

    ZNode *node = makenode(NODE_SUBSCRIPT);
    node->subscript.index = index;
    node->subscript.arr = previous;
    return node;
}

static ZNode **parseArgs(ZParser *parser) {
    ZNode **args = NULL;
    
    ZNode *expr = wrapNode(parser, parseExpr);
    if (!expr) return args;
    
    vecpush(args, expr);
    
    while (match(parser, TOK_COMMA)) {
        expr = wrapNode(parser, parseExpr);
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

    node->memberAccess.field = member;
    node->memberAccess.object = previous;
    node->tok = previous->tok;
    return node;
}

static ZNode *parseFuncCall(ZParser *parser, ZNode *previous) {
    expect(parser, TOK_LPAREN);
    ZNode **args = parseArgs(parser);
    expect(parser, TOK_RPAREN);

    ZNode *node = makenode(NODE_CALL);
    node->call.args = args;
    node->call.callee = previous;
    node->tok = previous->tok;
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

static ZNode *parsePostfixOper(ZParser *parser, ZNode *previous) {
    usize saved = parser->source->current;
    pushErrorCheckpoint(parser);
    ZNode *res = NULL;

    ZToken *tok = peek(parser);

    /* Postfix operator MUST BE on the same line. */
    if (!tok || tok->newlineBefore) goto cleanup;

    switch (tok->type) {
    case TOK_LSBRACKET: res = parseArrSubscript(parser, previous);  break;
    case TOK_LPAREN:    res = parseFuncCall(parser, previous);      break;
    case TOK_CAST:      res = parseCast(parser, previous);          break;
    case TOK_DOT:       res = parseMemberAccess(parser, previous);  break;
    default:            break;
    }

    if (res) {
        commitErrors(parser);
        return res;
    }
    cleanup:
        rollbackErrors(parser);
        parser->source->current = saved;

    return res;
}

static ZNode *parsePostfixExpr(ZParser *parser) {
    ZNode *left = wrapNode(parser, parsePrimary);

    guard(left);

    ZNode *node = NULL;
    do {

        node = parsePostfixOper(parser, left);
        if (!node) break;

        left = node;
    } while (node);
    return left;
}

static ZNode *parseUnary(ZParser *parser) {
    ZToken *start = peek(parser);
    guard(start);

    ZNode *node = wrapNode(parser, parsePostfixExpr);

    if (node) return node;

    ZTokenType valids[] = {
        TOK_PLUS,   TOK_MINUS,  TOK_NOT,
        TOK_SNOT,   TOK_STAR,   TOK_REF
    };

    usize len = sizeof(valids) / sizeof(valids[0]);

    if (!isValidToken(parser, valids, len)) {
        error(parser->state, start,
                "Expected expression, got '%s'", stoken(start));
        return NULL;
    }

    node = makenode(NODE_UNARY);
    node->unary.operat = consume(parser);
    node->unary.operand = wrapNode(parser, parseUnary);

    guard(node->unary.operand);

    return node;
}

#define arrlen(arr) (sizeof(arr) / sizeof((arr)[0]))
static ZNode *parseFactor(ZParser *parser) {
    ZTokenType valids[] = {TOK_STAR, TOK_DIV, TOK_MOD};
    return parseGenericBinary(parser,
            parseUnary,
            parseUnary,
            valids,
            arrlen(valids));
}

static ZNode *parseTerm(ZParser *parser) {
    ZTokenType valids[] = {TOK_PLUS, TOK_MINUS};
    return parseGenericBinary(parser,
            parseFactor,
            parseFactor,
            valids,
            arrlen(valids));
}

static ZNode *parseComparison(ZParser *parser) {
    ZTokenType valids[] = {TOK_LT, TOK_GT, TOK_LTE, TOK_GTE};
    return parseGenericBinary(parser,
            parseTerm,
            parseTerm,
            valids,
            arrlen(valids));
}

static ZNode *parseEquality(ZParser *parser) {
    ZTokenType valids[] = {TOK_EQEQ, TOK_NOTEQ};
    return parseGenericBinary(parser,
            parseComparison,
            parseComparison,
            valids,
            arrlen(valids));
}

static ZNode *parseLogicalAnd(ZParser *parser) {
    ZTokenType valids[] = {TOK_AND, TOK_SAND};
    return parseGenericBinary(parser,
            parseEquality,
            parseEquality,
            valids,
            arrlen(valids));
}

static ZNode *parseLogicalOr(ZParser *parser) {
    ZTokenType valids[] = {TOK_OR, TOK_SOR};
    return parseGenericBinary(parser,
            parseLogicalAnd,
            parseLogicalAnd,
            valids,
            arrlen(valids));
}

static ZNode *parseBinary(ZParser *parser) {
    ZTokenType valids[] = {TOK_EQ};
    return parseGenericBinary(parser,
            parseLogicalOr,
            parseBinary,
            valids,
            arrlen(valids));
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
        ZNode *cur = wrapNode(parser, func);
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
        ZType *curr = wrapType(parser, parseType);
        if (!curr) {
            error(parser->state, peek(parser), "Expected a type in type list");
            return args;
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
    ZType **args = parseTypeList(parser, TOK_LPAREN, TOK_RPAREN);
    ZType **generics = NULL;

    if (check(parser, TOK_LSBRACKET)) {
        generics = parseTypeList(parser, TOK_LSBRACKET, TOK_RSBRACKET);
        if (!generics) {
            error(parser->state, peek(parser),
                    "Expected generic type parameters in '[...]'");
            return NULL;
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

ZType *parseType(ZParser *parser) {
    ZToken *start   = peek(parser);
    bool constant   = match(parser, TOK_CONST);

    u8 stars        = 0;
    while (match(parser, TOK_STAR)) stars++;

    ZType *base     = wrapType(parser, parseAtom);
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

static ZNode *parseDefer(ZParser *parser) {
    expect(parser, TOK_DEFER);
    ZNode *expr = parseExpr(parser);

    ensure(expr, "Expected an expression after 'defer' keyword");

    ZNode *node = makenode(NODE_DEFER);
    node->deferStmt.expr = expr;
    return node;
}

//FIXME: Not yet implemented. Use else-if chain instead.
static ZNode *parseMatch(ZParser *parser) {
    expect(parser, TOK_MATCH);

    error(parser->state, peek(parser),
            "'match' statement are not implemented yet!");

    ZNode *expr = wrapNode(parser, parseExpr);

    ensure(expr, "Expected an expression");

    expect(parser, TOK_LBRACKET);
    expect(parser, TOK_RBRACKET);

    return NULL;
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

ZNode *parseStmt(ZParser *parser) {
    guard(canPeek(parser));

    ZTokenType t = peek(parser)->type;


    switch (t) {
    case TOK_IF:        return parseIf      (parser);
    case TOK_FOR:       return parseLoops   (parser);
    case TOK_MATCH:     return parseMatch   (parser);
    case TOK_DEFER:     return parseDefer   (parser);
    case TOK_RETURN:    return parseReturn  (parser);
    case TOK_BREAK:     return parseBreak   (parser);
    case TOK_CONTINUE:  return parseContinue(parser);
    default: {
        ZParserSnapshot *snap = store(parser);

        pushErrorCheckpoint(parser);
        ZNode *node = parseVarInferred(parser);
        if (node) goto ret;
        rollbackErrors(parser);

        undo(parser, snap);
        pushErrorCheckpoint(parser);
        node = parseVarDefTyped(parser);
        if (node) goto ret;
        rollbackErrors(parser);

        undo(parser, snap);
        node = parseExpr(parser);

        if (!node) undo(parser, snap);

ret:
        commitErrors(parser);
        return node;
    }
    }
}

static ZNode *parseBlock(ZParser *parser) {
    let start = peek(parser);
    expect(parser, TOK_LBRACKET);

    ZNode *block = makenode(NODE_BLOCK);
    ZNode *stmt = NULL;
    do {
        stmt = wrapNode(parser, parseStmt);
        if (stmt) vecpush(block->block, stmt);
    } while (stmt);

    if (!check(parser, TOK_RBRACKET)) {
        error(parser->state, peek(parser), "A statement cannot be parsed");
        while (canPeek(parser) && !check(parser, TOK_RBRACKET)) consume(parser);

        ensure(canPeek(parser), "Expected a '}' to close the block");
    }

    expect(parser, TOK_RBRACKET);

    if (veclen(block->block) == 0) {
        warning(parser->state, start, "A block cannot be empty");
    }

    return block;
}

static ZNode *parseField(ZParser *parser) {
    ZType *type = wrapType(parser, parseType);

    if (!type) {
        return NULL;
    }
    ensure(check(parser, TOK_IDENT), "Expected an identifier");

    ZToken *ident = consume(parser);

    if (check(parser, TOK_LPAREN)) {
    }

    ZNode *node = makenode(NODE_FIELD);
    node->field.type = type;
    node->field.identifier = ident;
    return node;
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

    // vecpush(enm->strct.fields, );
    for (usize i = 0; i < veclen(types); i++) {
        field                   = makenode(NODE_FIELD);
        field->field.identifier = NULL;
        field->field.type       = types[i];    

        vecpush(enm->strct.fields, field);
    }

    node->resolved = enm;
    return node;
}

static ZNode *parseEnumDecl(ZParser *parser, bool public) {
    ZToken *start = peek(parser);
    expect(parser, TOK_ENUM);


    if (!check(parser, TOK_IDENT)) {
        error(parser->state, peek(parser), "Expected an identifier");
        return NULL;
    }
    ZToken *name = consume(parser);

    ZType **generics = NULL;
    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser, true);
    }

    ZNode **fields = parseGenericList(parser,
            TOK_LBRACKET, TOK_RBRACKET,
            parseEnumField, false);

    if (!fields || veclen(fields) < 2) {
        error(parser->state, start, "Failed to parse enum declaration");
        return NULL;
    }

    ZNode *node             = makenode(NODE_ENUM);
    node->enumDef.name      = name;
    node->enumDef.pub       = public;
    node->enumDef.fields    = fields;
    node->tok               = node->enumDef.name;

    ZType *type             = maketype(Z_TYPE_ENUM);
    type->enm.name          = name;
    type->enm.generics      = generics;
    type->enm.fields        = NULL;

    for (usize i = 0; i < veclen(fields); i++) {
        vecpush(type->enm.fields, fields[i]->resolved);
    }

    node->resolved          = type;
    
    return node;
}

static ZNode *parseStructDecl(ZParser *parser, bool public) {
    ZToken *start = peek(parser);
    expect(parser, TOK_STRUCT);
    ensure(check(parser, TOK_IDENT), "Expected an identifier");

    ZToken *name = consume(parser);
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
            parseField, false);

    if (veclen(fields) < 1) {
        error(parser->state, start, "Expected at least one field");
        return NULL;
    }

    hashset_t seen = NULL;
    for (usize i = 0; i < veclen(fields); i++) {
        ZToken *ti = fields[i]->field.identifier;
        if (!hashset_insert(&seen, ti->str)) {
            error(parser->state, ti,
                    "Field '%s' already declared", ti->str);
        }
    }
    hashset_free(&seen);

    ZNode *node                 = makenode(NODE_STRUCT);
    node->structDef.fields      = fields;
    node->structDef.generics    = generics;
    node->structDef.ident       = name;
    node->structDef.pub         = public;

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

    return parseOrGrammar(parser, exprFunc, arrlen(exprFunc));

}

static ZNode *parseReturn(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_RETURN);
    ZNode *ret = makenode(NODE_RETURN);

    ret->returnStmt.expr = parseExpr(parser);
    ret->tok = start;
    return ret;
}

static ZNode *parseIf(ZParser *parser) {
    ZToken *start = peek(parser);
    expect(parser, TOK_IF);

    parser->noStructLit = true;
    ZNode *cond = parseExpr(parser);
    parser->noStructLit = false;

    guard(cond);

    ZParseFunc elseBranch[] = {
        parseIf, parseBlock
    };

    ZNode *body = parseBlock(parser);

    guard(body);

    ZNode *node = makenode(NODE_IF);

    if (canPeek(parser) && match(parser, TOK_ELSE)) {
        ZNode *elseBody = parseOrGrammar(parser, elseBranch, 2);
        if (!elseBody) return NULL;
        node->ifStmt.elseBranch = elseBody;
    }

    node->ifStmt.cond   = cond;
    node->ifStmt.body   = body;
    node->tok           = start;
    return node;
}

static ZNode *parseFor(ZParser *parser) {
    expect(parser, TOK_FOR);

    ZNode *var = parseVarDef(parser);

    ensure(var, "Expected a variable declaration");
    expect(parser, TOK_SEMICOLON);

    parser->noStructLit = true;
    ZNode *cond = parseExpr(parser);
    parser->noStructLit = false;

    ensure(cond, "Expected an expression");
    expect(parser, TOK_SEMICOLON);

    ZNode *incr = parseExpr(parser);

    ensure(incr, "Expected an expression");
    ZNode *block = parseBlock(parser);

    ensure(block, "Expected a block");

    ZNode *node = makenode(NODE_FOR);
    node->forStmt.var   = var;
    node->forStmt.cond  = cond;
    node->forStmt.incr  = incr;
    node->forStmt.block = block;
    return node;
}

/* While parsed with 'for' token instead of standard while. */
static ZNode *parseWhile(ZParser *parser) {
    expect(parser, TOK_FOR);

    parser->noStructLit = true;
    ZNode *cond = wrapNode(parser, parseExpr);
    parser->noStructLit = false;
    if (!cond) {
        error(parser->state, peek(parser),
                "Expected condition expression after 'while'");
    }
    ZNode *body = wrapNode(parser, parseBlock);

    ZNode *node = makenode(NODE_WHILE);
    node->whileStmt.branch  = body;
    node->whileStmt.cond    = cond;
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

    ZParseFunc f[] = { parseFor, parseWhile };
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
static ZNode *parseFuncDecl(ZParser *parser, bool public) {
    ZToken *start = peek(parser);
    ZType *ret = wrapType(parser, parseType);

    if (!ret) {
        error(parser->state, start, "Expected return type");
        return NULL;
    } else if (!check(parser, TOK_IDENT)) {
        error(parser->state, start, "Expected an identifier");
        return NULL;
    }

    ZToken *name    = consume(parser);
    ZType **generics = NULL;

    if (check(parser, TOK_LSBRACKET)) {
        generics = parseGenericsDecl(parser, true);
        if (!generics) {
            error(parser->state, peek(parser),
                    "Expected generic type parameters after function name");
        }
    }

    ZNode **args = parseGenericList(parser,
        TOK_LPAREN, TOK_RPAREN,
        parseField,
        true);

    ZNode *body = wrapNode(parser, parseBlock);

    if (!body) {
        error(parser->state, peek(parser),
                "Expected function body '{...}' after declaration");
    }

    ZNode *node = makenode(NODE_FUNC);
    node->funcDef.ret       = ret;
    node->funcDef.name      = name;
    node->funcDef.args      = args;
    node->funcDef.body      = body;
    node->funcDef.pub       = public;
    node->funcDef.generics  = generics;
    node->tok               = name;
    node->funcDef.base      = NULL;
    node->funcDef.receiver  = NULL;
    node->funcDef.mangled   = name->str;

    ZType *func             = maketype(Z_TYPE_FUNCTION);
    func->func.ret          = ret;
    func->func.args         = NULL;

    for (usize i = 0; i < veclen(args); i++) {
        vecpush(func->func.args, args[i]->field.type);
    }

    node->resolved = func;
    return node;
}

static ZVarDestructPattern *parseDestructVar(ZParser *parser) {
    guard(canPeek(parser));

    ZVarDestructPattern *cur    = NULL;
    ZVarDestructPattern **list  = NULL;
    ZToken *tok                 = consume(parser);

    if (tok->type == TOK_IDENT) {
        cur = makeVarDestructPattern(Z_VAR_IDENT);
        cur->ident = tok;
    } else if (tok->type == TOK_LBRACKET) {
        ZToken *key = NULL;
        while (true) {
            if (!check(parser, TOK_IDENT)) break;
            key = consume(parser);

            if (check(parser, TOK_COMMA) || check(parser, TOK_RBRACKET)) {
                cur = makeDestructIdent(key);
            } else if (match(parser, TOK_COLON)) {
                cur = parseDestructVar(parser);
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
        while (true) {
            cur = parseDestructVar(parser);
            if (!cur) break;
            vecpush(list, cur);
            if (!match(parser, TOK_COMMA)) break;
            if (check(parser, TOK_RPAREN)) break;
        }

        expect(parser, TOK_RPAREN);
        cur = makeVarDestructPattern(Z_VAR_TUPLE);
        cur->tuple = list;
    } else {
        error(parser->state, tok, "Cannot deconstruct variable");
        return NULL;
    }

    cur->tok = tok;
    return cur;
}

static ZNode *parseVarInferred(ZParser *parser) {
    ZVarDestructPattern *pattern = parseDestructVar(parser);

    expect(parser, TOK_ASSIGN);
    ZNode *expr = wrapNode(parser, parseExpr);

    if (!expr) {
        error(parser->state, peek(parser), "Expected expression after ':='");
    }

    return makenodevar(pattern, NULL, expr);
}

static ZNode *parseVarDefTyped(ZParser *parser) {
    ZToken *start = peek(parser);
    parser->noFuncType = true;
    ZType *type = parseType(parser);
    parser->noFuncType = false;

    if (!type) {
        error(parser->state, start, "Failed to parse type");
        return NULL;
    } else if (!check(parser, TOK_IDENT) && !check(parser, TOK_LPAREN) && !check(parser, TOK_LBRACKET)) {
        error(parser->state, start, "Expected an identifier or destructure pattern");
        return NULL;
    }
    // else if (!start->newlineBefore) {
    //     error(parser->state, start,
    //             "Variable declaration must be defined in the same line");
    // }

    ZVarDestructPattern *var = parseDestructVar(parser);
    ZNode *expr = NULL;

    expect(parser, TOK_EQ);
    
    expr = wrapNode(parser, parseExpr);
    if (!expr) {
        error(parser->state, peek(parser), "Expected expression after '='");
        return NULL;
    }

    
    return makenodevar(var, type, expr);
}

static ZNode *parseVarDef(ZParser *parser) {
    ZParseFunc func[] = {
        parseVarInferred, parseVarDefTyped
    };
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
    while (( expr = wrapNode(parser, parseExpr) )) {
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

    while (( expr = wrapNode(parser, parseExpr) )) {
        vecpush(values, expr);
        if (check(parser, TOK_RSBRACKET)) break;
        if (!match(parser, TOK_COMMA)) break;
    }

    expect(parser, TOK_RSBRACKET);

    ZNode *node = makenode(NODE_ARRAY_LIT);

    node->arraylit  = values;
    node->tok       = start;

    return node;
}

static ZNode *parseStructLit(ZParser *parser) {
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

        ZNode *expr = wrapNode(parser, parseExpr);

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

static ZNode *getModuleByName(ZParser *parser, ZToken **module, bool isStd) {
    char *filename = NULL;
    if (isStd) {
        usize len = strlen(parser->state->compilerPath);
        vecunion(filename, parser->state->compilerPath, len);
        vecpush(filename, sep);
        vecunion(filename, "std", 3);
        vecpush(filename, sep);
    }

    usize len = veclen(module);
    for (usize i = 0; i < len; i++) {
        const char *seg = strcmp(module[i]->str, "super") == 0 ?
            ".." :
            module[i]->str;
        for (usize j = 0; j < strlen(seg); j++) {
            vecpush(filename, seg[j]);
        }
        if (i < len - 1) vecpush(filename, sep);
    }
    vecunion(filename, ".zn", 3);
    vecpush(filename, '\0');

    bool canVisit = visit(parser->state, filename);
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
    expect(parser, TOK_TYPEDEF);
    ensure(check(parser, TOK_IDENT), "Expected an identifier");

    ZToken *alias = consume(parser);

    ZType *type = wrapType(parser, parseType);
    
    ensure(type, "Invalid type");

    ZNode *node = makenode(NODE_TYPEDEF);
    node->typeDef.alias = alias;
    node->typeDef.type  = type;
    node->typeDef.pub     = public;
    return node;
}

static ZNode *parseForeignDecl(ZParser *parser, bool public) {
    ZToken *start   = peek(parser);
    expect(parser, TOK_FOREIGN);

    ZType *ret      = wrapType(parser, parseType);

    ensure(check(parser, TOK_IDENT), "Expected an identifier");
    ZToken *name    = consume(parser);

    expect(parser, TOK_LPAREN);

    ZType **args    = NULL;
    ZType *arg     = NULL;
    bool variadic   = false;
    while (true) {
        if (check(parser, TOK_TRIPLE_DOT)) {
            variadic = true;
            consume(parser);
        } else {
            arg = wrapType(parser, parseType);
            if (arg) vecpush(args, arg);
        }
        if (check(parser, TOK_RPAREN)) break;
        if (!match(parser, TOK_COMMA)) break;
    }

    expect(parser, TOK_RPAREN);

    ZNode *node = makenode(NODE_FOREIGN);
    node->foreignFunc.ret   = ret;
    node->foreignFunc.tok   = name;
    node->foreignFunc.args  = args;
    node->foreignFunc.pub   = public;
    node->tok               = start;
    ZType *type             = maketype(Z_TYPE_FUNCTION);
    type->func.ret          = ret;
    type->func.args         = args;
    type->func.variadic     = variadic;
    node->resolved          = type;
    return node;
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

    } else if (match(parser, TOK_MACRO_TYPE)) {
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
    } else if (checkMask(parser, TOK_OVERRIDABLE)) {
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

    if (!checkMask(parser, TOK_OVERRIDABLE)) {
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

static ZNode *parseFuncBlock(ZParser *parser) {
    expect(parser, TOK_FOR);
    ZType *type = parseType(parser);

    guard(type);

    ZToken *rec = NULL;
    if (check(parser, TOK_IDENT)) rec = consume(parser);

    /* Declare facets this block must implement. */
    ZType **facets = NULL;
    if (match(parser, TOK_WITH)) {
        ZType *type = wrapType(parser, parseGenericDecl);
        if (!type) {
            error(parser->state, peek(parser),
                    "Unespected token");
            return NULL;
        }

        vecpush(facets, type);

        while (match(parser, TOK_PLUS)) {
            type = wrapType(parser, parseGenericDecl);
            if (!type) break;
            vecpush(facets, type);
        }
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

    ZNode *func = NULL;
    bool public = false;

    ZNode *block = makenode(NODE_BLOCK);
    block->block = NULL;
    while (true) {
        public = match(parser, TOK_PUB);
        func = parseFuncDecl(parser, public);
        if (!func) {
            if (public) {
                error(parser->state, peek(parser),
                        "Expected a function declaration after 'pub'");
                return NULL;
            }
            break;
        }
        vecpush(block->block, func);

        if (check(parser, TOK_RBRACKET)) break;
    }

    expect(parser, TOK_RBRACKET);

    usize len = veclen(block->block);

    if (len == 0) return block;

    /* For static mangling we key on the primitive type name (matching what the
     * call-site parser emits for `Type::method`). Strip pointer levels to get
     * the base primitive token. */
    ZType *baseType = type;
    while (baseType->kind == Z_TYPE_POINTER) baseType = baseType->base;
    ZToken *typeNameTok = baseType->primitive.token;

    if (rec) { // receiver functions
        ZNode *receiver = makenode(NODE_FIELD);
        receiver->field.identifier = rec;
        receiver->field.type = type;
        for (usize i = 0; i < len; i++) {
            /* manglerM encodes the full receiver type (pointer or not), so
             * `for String self` and `for *String self` get distinct names. */
            block->block[i]->funcDef.mangled = manglerM(
                type,
                block->block[i]->funcDef.name
            );
            block->block[i]->funcDef.receiver = receiver;
        }
    } else { // static functions
        for (usize i = 0; i < len; i++) {
            block->block[i]->funcDef.mangled = mangler((ZToken*[]) {
                typeNameTok,
                block->block[i]->funcDef.name,
                NULL
            });
            block->block[i]->funcDef.base = type;

            vecunion(block->block[i]->funcDef.generics,
                    generics, veclen(generics));
        }
    }

    return block;
}

static ZNode *parse(ZParser *parser) {
    if (!canPeek(parser)) return NULL;

    ZTokenType t = peek(parser)->type;

    // 'use' (module import) never takes pub
    if (t == TOK_MODULE) {
        return parseImport(parser);
    }

    bool public = match(parser, TOK_PUB);

    guard(canPeek(parser));

    t = peek(parser)->type;

    if (t == TOK_IDENT && checkAhead(parser, TOK_ASSIGN, 1)) {
        return parseVarInferred(parser);
    }


    switch (t) {
    case TOK_FOREIGN:   return parseForeignDecl (parser, public);
    case TOK_TYPEDEF:   return parseTypedef     (parser, public);
    case TOK_STRUCT:    return parseStructDecl  (parser, public);
    case TOK_MACRO:     return skipMacro        (parser, public);
    case TOK_ENUM:      return parseEnumDecl    (parser, public);
    case TOK_FOR:       return parseFuncBlock   (parser);
    default: {
        ZParserSnapshot *snap = store(parser);
        ZNode *res = parseFuncDecl(parser, public);

        if (res) return res;

        undo(parser, snap);
        res = parseVarDef(parser);

        if (res) return res;
        
        undo(parser, snap);
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
