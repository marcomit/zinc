#include "zlex.h"
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>

static void *xmalloc(size_t size) {
	void *ptr = malloc(size);
	return ptr;
}

static lexer_t *createLexer() {
	lexer_t *l = xmalloc(sizeof(lexer_t));
	l->col = 0;
	l->row = 0;
	l->current = NULL;
	l->program = NULL;
	vec_init(&l->tokens, 8);
	return l;
}

static zcc_Token *createToken(char *value, zcc_token type) {
	zcc_Token *self = malloc(sizeof(zcc_Token));
	self->type = type;
	self->value = value;
	return self;
}

static void addToken(lexer_t *l, zcc_Token *token) {
	vec_push(&l->tokens, token);
}

static void next(lexer_t *l) {
	if (!*l->current) return;
	if (*l->current == '\n') {
		l->row++;
		l->col = 0;
	} else {
		l->col++;
	}
}

static void skipSpaces(lexer_t *l) {
	while (*l->current && isspace(*l->current)) next(l);
}

static void error(lexer_t *l, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	printf("%zu,%zu", l->row, l->col);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);

	va_end(args);
	exit(1);
}

static void parseString(lexer_t *l) {
	if (*l->current != '"') return;
	l->current++;

	char *start = l->current;
	while (*l->current && *l->current != '"') next(l);

	if (!l->current) error(l, "unterinated string"); 

	int len = l->current - start - 1;
	char *buff = malloc(len + 1);
	buff[len] = 0;

	zcc_Token *tok = createToken(buff, TOK_STR);
	addToken(l, tok);
}

static void parseLiteral(lexer_t *l) {

}

static void parseInt(lexer_t *l) {

}

lexer_t *ztokenize(char * program) {
	lexer_t *l = createLexer();

	l->program = program;
	l->current = program;

	while (*l->program) {
		skipSpaces(l);
		if (!*l->current) break;

		switch(*l->current) {
		case '"': parseString(l); break;
		case '(': addToken(createToken(NULL, TOK_LPAREN)
		}
	}

	return l;
}
