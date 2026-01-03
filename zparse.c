#include "zparse.h"
#include "zlex.h"
#include "zmem.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>

static void error(char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	exit(1);
}

static ZNode *makenode(ZNodeType type) {
	ZNode *self = allocator.alloc(sizeof(ZNode));
	self->type = type;
	return self;
}

static ZToken *peek(ZTokens *tokens) {
	if (tokens->current >= tokens->len) {
		error("Iterator tokens out of bound!");
	}
	return tokens->ptr[tokens->current];
}

static ZToken *peekAhead(ZTokens *tokens, int32_t offset) {
	int32_t index = tokens->current + offset;
	if (index < 0 || index >= tokens->len) {
		error("Index out of range %d", index);
	}
	return tokens->ptr[index];
}

static ZToken *consume(ZTokens *tokens) {
	ZToken *curr = peek(tokens);
	tokens->current++;
	return curr;
}

static bool match(ZTokens *tokens, ZTokenType type) {
	bool res = peek(tokens)->type == type;
	if (res) consume(tokens);
	return res;
}

static ZNode *parseUnary(ZTokens *tokens) {
	if (match(tokens, TOK_INT_LIT) ||
			match(tokens, TOK_STR_LIT) ||
			match(tokens, TOK_BOOL_LIT)) {
		ZNode *node = makenode(NODE_LITERAL);
		node->literalTok = peekAhead(tokens, -1);

		return node;
	} else if (match(tokens, TOK_IDENT)) {
		ZNode *node = makenode(NODE_IDENTIFIER);
		node->identTok = peekAhead(tokens, -1);

		return node;
	}
	return NULL;
}

static ZNode *parseFactor(ZTokens *tokens) {
	ZNode *node = parseUnary(tokens);
	return node;
}

static ZNode *parseTerm(ZTokens *tokens) {
	ZNode *left = parseFactor(tokens);

	 

	ZNode *term = makenode(NODE_BINARY);
	term->binary.left = left;
	term->binary.op = left;
	term->binary.right = left;
	return term;
}

ZNode *zparse(ZTokens *tokens) {
	tokens->current = 0;
	return NULL;
}
