#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include "zparse.h"
#include "zlex.h"

static void error(char *fmt, ...) {
	va_list args;
	va_start(args, fmt);
	vfprintf(stderr, fmt, args);
	va_end(args);
	exit(1);
}

static ZToken *peek(ZTokens *tokens) {
	if (tokens->current >= tokens->len) {
		error("Iterator tokens out of bound!");
	}
	return tokens->ptr[tokens->current];
}

static ZToken *consume(ZTokens *tokens) {
	ZToken *curr = peek(tokens);
	tokens->current++;
	return curr;
}

static bool matchValue(ZTokens *tokens, char *value) {
	return strcmp(peek(tokens)->value, value) == 0;
}

static bool matchType(ZTokens *tokens, ZTokenType type) {
	return peek(tokens)->type == type;
}

static ZToken *skipByValue(ZTokens *tokens, char *value) {
	ZToken *curr = consume(tokens);
	if (strcmp(curr->value, value)) {
		error("Expected '%s' got '%s';", value, curr->value);
	}
	return curr + 1;
}

static ZToken *skipByType(ZTokens *tokens, ZTokenType type) {
	ZToken *curr = consume(tokens);
	if (curr->type != type) {
		error("Unexpected token '%s'", curr->value);
	}
	return curr + 1;
}


static ZNodeExpr *parseExpr(ZTokens *tokens) {
	if (matchType(tokens, TOK_INT_LIT)) {
		consume(tokens);
	}
	return NULL;
}

static ZNodeStmt *parseStmt(ZTokens *tokens) {
	return NULL;
}

static ZNodeBlock *parseBlock(ZTokens *tokens) {
	skipByType(tokens, TOK_LBRACKET);

	ZNodeBlock *block = malloc(sizeof(ZNodeBlock));
	vec_init(&block->stmts, 8);

	ZNodeStmt *stmt = NULL;

	do {

		stmt = parseStmt(tokens);
		if (stmt) vec_push(&block->stmts, stmt);

	} while (stmt);

	skipByType(tokens, TOK_RBRACKET);
	return block;
}

static ZNodeIf *parseIf(ZTokens *tokens) {
	skipByType(tokens, TOK_IF);
	skipByType(tokens, TOK_LPAREN);

	ZNodeExpr *expr = parseExpr(tokens);

	skipByType(tokens, TOK_RPAREN);

	ZNodeBlock *branchTrue = parseBlock(tokens);
	return NULL;
}

static ZNode *parseTokens(ZTokens *tokens) {

}

ZNode *parse(ZTokens *tokens) {
	tokens->current = 0;
	return NULL;
}
