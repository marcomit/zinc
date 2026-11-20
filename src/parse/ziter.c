#include "zparse.h"

ZParser *makeparser(ZState *state, ZToken **tokens) {
    ZParser *self                       = zalloc(state->allocator, ZParser);
    *self                               = (ZParser){ 0 };
    self->source                        = maketokstream(state->allocator, tokens, NULL);
    self->state                         = state;
    self->allocator                     = state->allocator;
    return self;
}

ZNode *makenode(Allocator *allocator, ZNodeType type) {
    ZNode *self = zalloc(allocator, ZNode);
    *self = (ZNode){ 0 };
    self->type = type;

    return self;
}

ZInterpolation *makeinterpolation(Allocator *allocator, ZInterpolationType type, void *ptr) {
    ZInterpolation *self    = zalloc(allocator, ZInterpolation);
    self->expr              = ptr;
    self->type              = type;
    return self;
}

/* Sets the source range of a node: from `start` to the last consumed token.
 * A NULL `start` clears it (the node doesn't belong to this file's source).
 * Only the LSP keeps spans, the compiler compiles this out. */
ZNode *setspan(ZParser *parser, ZNode *node, ZToken *start) {
#if Z_LSP
    if (!node) return node;
    node->start = start;
    node->end   = start && parser->last ? parser->last : start;
#else
    (void)parser;
    (void)start;
#endif
    return node;
}

/* Like setspan, for nodes that extend an already parsed node to the right
 * (binary operators, calls, member accesses, casts, ...). */
ZNode *setspanfrom(ZParser *parser, ZNode *node, ZNode *left) {
#if Z_LSP
    return setspan(parser, node, left ? left->start : NULL);
#else
    (void)parser;
    (void)left;
    return node;
#endif
}

ZType *maketype(Allocator *allocator, ZTypeKind kind) {
    ZType *self = zalloc(allocator, ZType);
    *self = (ZType){ 0 };
    self->kind = kind;
    return self;
}

ZType *makePrimitiveType(Allocator *allocator, ZTokenType type) {
    ZType *self = maketype(allocator, Z_TYPE_PRIMITIVE);
    self->primitive.token = maketoken(allocator, type, NULL, NULL);
    return self;
}

ZRegion *makeregion(Allocator *allocator, const char *name, int depth) {
    ZRegion *region     = zalloc(allocator, ZRegion);
    region->depth       = depth;
    region->name        = name;
    region->origin      = NULL;
    return region;
}

ZVarDestructPattern *makeVarDestructPattern(Allocator *allocator, int type) {
    ZVarDestructPattern *self = zalloc(allocator, ZVarDestructPattern);
    self->type = type;
    return self;
}

ZVarDestructPattern *makeDestructIdent(Allocator *a, ZToken *tok) {
    ZVarDestructPattern *pattern    = makeVarDestructPattern(a, Z_VAR_IDENT);
    pattern->tok                    = tok;
    pattern->ident                  = tok;
    return pattern;
}

ZNode *makenodevar(Allocator *a, ZVarDestructPattern *pattern, ZType *type, ZNode *expr) {
    if (!pattern) return NULL;
    ZNode *node             = makenode(a, NODE_VAR_DECL);
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

ZToken *peekAhead(ZParser *parser, usize next) {
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
    parser->last = curr;
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
    return canPeek(tokens) && tokmask(peek(tokens), mask);
}

bool match(ZParser *parser, ZTokenType type) {
    bool res = check(parser, type);
    if (res) consume(parser);
    return res;
}

void pushErrorCheckpoint(ZParser *parser) {
    usize errlen = veclen(parser->state->logs);
    vecpush(parser->errstack, errlen);
    parser->depth++;
}

void commitErrors(ZParser *parser) {
    if (parser->depth > 0) {
        vecpop(parser->errstack);
        parser->depth--;
    }
}

void rollbackErrors(ZParser *parser) {
    if (parser->depth > 0) {
        usize checkpoint = vecpop(parser->errstack);
        while (veclen(parser->state->logs) > checkpoint) vecpop(parser->state->logs);
        parser->depth--;
    }
}

bool isValidToken(ZParser *parser, ZTokenType *validTokens, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (check(parser, validTokens[i])) return true;
    }
    return false;
}

ZParserSnapshot *store(ZParser *parser) {
    ZParserSnapshot *self = zalloc(parser->allocator, ZParserSnapshot);
    (*self) = (ZParserSnapshot){
        .stream         = parser->source,
        .streamIndex    = parser->source->current,
        .tokenIndex     = parser->tokenIndex,
        .last           = parser->last
    };
    return self;
}

void undo(ZParser *parser, ZParserSnapshot *snap) {
    parser->source          = snap->stream;
    parser->source->current = snap->streamIndex;
    parser->tokenIndex      = snap->tokenIndex;
    parser->last            = snap->last;
}


ZNode **parseGenericList(ZParser *parser,
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
            zlog(parser->state, peek(parser), Z2009);
            return list;
        }
        vecpush(list, cur);
        if (commaSeparated && !match(parser, TOK_COMMA)) break;
    } while (true);

    if (!match(parser, right)) {
        ZToken *tok = peek(parser);
        zlog(parser->state, tok, Z200A, tokname(right), stoken(tok));
        return NULL;
    }

    return list;
}
