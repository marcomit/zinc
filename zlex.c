#include "zlex.h"

#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define TLEN 33

static const char *tokens[] = {
#define DEF(id, str) str,

#define TOK_TYPES
#define TOK_SYMBOLS
#define TOK_FLOWS

#include "ztok.h"

#undef TOK_FLOWS
#undef TOK_SYMBOLS
#undef TOK_TYPES

#undef DEF
};

typedef struct {
	char *program;
	char *current;
	ZTokens *tokens;
	size_t row, col;
} lexer_t;

static void *xmalloc(size_t size) {
	void *ptr = malloc(size);
	return ptr;
}

static ZToken *createToken(char *value, ZTokenType type) {
	ZToken *self = malloc(sizeof(ZToken));
	self->type = type;
	self->value = value;
	return self;
}

static void addToken(lexer_t *l, ZToken *token) {
	vec_push(l->tokens, token);
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

	if (!l->current) error(l, "Unterinated string %.10s", start);

	int len = l->current - start - 1;
	char *buff = malloc(len + 1);
	buff[len] = 0;

	ZToken *tok = createToken(buff, TOK_STR_LIT);
	addToken(l, tok);
}

static bool parseLiteral(lexer_t *l) {
	for (size_t i = 0; i < TLEN; i++) {
		int tlen = strlen(tokens[i]);
		if (strncmp(tokens[i], l->current, tlen) == 0) {
			// ZToken *tok = createToken(strncpy(l->current, tlen));
			return true;
		}
	}
	return false;
}

static void parseInt(lexer_t *l) {
	if (!*l->current || !isdigit(*l->current)) return;

}

static void addSymbol(lexer_t *l, ZTokenType type) {
	addToken(l, createToken(NULL, type));
	l->current++;
}

ZTokens *ztokenize(char * program) {
	lexer_t l;

	l.program = program;
	l.current = program;
	l.row = 0;
	l.col = 0;
	l.tokens = xmalloc(sizeof(ZTokens));
	ZTokens *tokens = l.tokens;

	while (*l.current) {
		printf("Current: %c", *l.current);
		while (*l.current && isspace(*l.current)) next(&l);
		if (!*l.current) break;

		if (isdigit(*l.current) || *l.current == '-') {
			parseInt(&l);
		} else {
			switch(*l.current) {
			case '"': parseString(&l); break;
			case '(': addSymbol(&l, TOK_LPAREN); 		break;
			case ')': addSymbol(&l, TOK_RPAREN); 		break;
			case '[': addSymbol(&l, TOK_LSBRACKET); 	break;
			case ']': addSymbol(&l, TOK_RSBRACKET); 	break;
			case '{': addSymbol(&l, TOK_LBRACKET); 	break;
			case '}': addSymbol(&l, TOK_RBRACKET); 	break;
			case '*': addSymbol(&l, TOK_DEREF);			break;
			case '&': addSymbol(&l, TOK_REF);				break;
			}
		}
	}
	return tokens;
}
