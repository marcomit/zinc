#include "zlex.h"
#include "zmem.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#define FNV_OFFSET 2166136261u
#define FNV_PRIME	 16777619u

#define HASHMAP_TOK_LEN 64
#define HASHMAP_TOK_MASK (HASHMAP_TOK_LEN - 1)

typedef struct {
	char *program;
	char *current;
	ZTokens *tokens;
	size_t row, col;
} lexer_t;

typedef struct {
	const char *keyword;
	ZTokenType type;
} KeyboardEntry;

static KeyboardEntry keyboardEntries[HASHMAP_TOK_LEN];

static uint32_t hashtoken(const char *buff, size_t len) {
	uint32_t hash = FNV_OFFSET;
	for (size_t i = 0; i < len; i++) {
		hash ^= (uint8_t)(buff[i]);
		hash *= FNV_PRIME;
	}
	return hash;
}


static void initKeywords() {
	KeyboardEntry keywords[] = {
		#define DEF(id, str, _) { str, id },

		#define TOK_FLOWS
		#define TOK_TYPES

		#include "ztok.h"
	
		#undef TOK_TYPES
		#undef TOK_FLOWS

		#undef DEF
	};

	for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
		const char *name = keywords[i].keyword;
		uint32_t hash = hashtoken(name, strlen(name)) & HASHMAP_TOK_MASK;

		while (keyboardEntries[hash].keyword != NULL) {
			hash = (hash + 1) & HASHMAP_TOK_MASK;
		}
		keyboardEntries[hash].keyword = keywords[i].keyword;
		keyboardEntries[hash].type = keywords[i].type;
	}
}

ZTokenType findKeyword(const char *ident, size_t len) {
	uint32_t hash = hashtoken(ident, len) & HASHMAP_TOK_MASK;

	while (keyboardEntries[hash].keyword != NULL) {
		if (strlen(keyboardEntries[hash].keyword) == len &&
				memcmp(keyboardEntries[hash].keyword, ident, len) == 0) {
			return keyboardEntries[hash].type;
		}
		hash = (hash + 1) & HASHMAP_TOK_MASK;
	}

	return TOK_IDENT;
}

static ZToken *createToken(ZTokenType type) {
	ZToken *self = zalloc(ZToken);
	self->type = type;
	return self;
}

static ZToken *createIdent(char *name) {
	ZToken *self = createToken(TOK_IDENT);
	self->str = name;
	return self;
}

static ZToken *createInteger(int64_t value) {
	ZToken *self = createToken(TOK_INT_LIT);
	self->integer = value;
	return self;
}

static ZToken *createString(char *str) {
	ZToken *self = createToken(TOK_STR_LIT);
	self->str = str;
	return self;
}

static void addToken(lexer_t *l, ZToken *token) {
	token->row = l->row;
	token->col = l->col;
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
	l->current++;
}

static void error(lexer_t *l, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	fprintf(stderr, "%zu,%zu: ", l->row, l->col);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);

	va_end(args);
	// exit(1);
}

static ZToken *parseString(lexer_t *l) {
	if (*l->current != '"') return NULL;
	next(l);

	char *start = l->current;
	while (*l->current && *l->current != '"') next(l);

	if (!l->current) {
		error(l, "Unterinated string %.10s", start);
		return NULL;
	}

	int len = l->current - start - 1;
	char *buff = allocator.alloc(len + 1);
	buff[len] = 0;

	return createString(buff);
}

static ZToken *parseSymbol(lexer_t *l) {
	if (false) { }
	#define DEF(id, str, _) else if(!strncmp(str, l->current, strlen(str))) { \
		l->current += strlen(str);																							\
		return createToken(id);																									\
	}

	#define TOK_SYMBOLS

	#include "ztok.h"

	#undef TOK_SYMBOLS
	#undef DEF
	
	error(l, "Unexpected symbol near '%.5s'", l->current);

	while (*l->current && !isspace(*l->current)) next(l);

	return NULL;
}

static ZToken *parseInt(lexer_t *l) {
	char *start = l->current;
	if (!isdigit(*l->current)) return NULL;

	while (isdigit(*l->current)) next(l);

	long long value = strtoll(start, NULL, 10);
	if (errno == ERANGE) error(l, "Invalid integer range %.10s", start);

	return createInteger(value);
}

static ZToken *parseLiteral(lexer_t *l) {
	if (!isalpha(*l->current) && *l->current != '_') return NULL;

	char *start = l->current;
	while (isalpha(*l->current) || *l->current == '_') next(l);

	size_t len = l->current - start;
	ZTokenType type = findKeyword(start, len);

	if (type == TOK_IDENT) {
		return createIdent(strndup(start, len));
	}

	return createToken(type);
}

void printToken(ZToken *token) {
	switch(token->type) {
		case TOK_INT_LIT:
			printf("int(%llu)", token->integer);
			break;
		case TOK_STR_LIT:
			printf("string(%s)", token->str);
			break;
		case TOK_BOOL_LIT:
			printf("bool(%s)", token->boolean ? "true" : "false");
			break;
		case TOK_IDENT:
			printf("ident(%s)", token->str);
			break;
		#define DEF(id, str, _) case id: printf(str); break;

		#define TOK_FLOWS
		#define TOK_TYPES
		#define TOK_SYMBOLS

		#include "ztok.h"

		#undef TOK_SYMBOLS
		#undef TOK_TYPES
		#undef TOK_FLOWS

		#undef DEF
	}
}

void printTokens(ZTokens *tokens) {
	printf("Tokens: %zu\n", tokens->len);
	foreach(tok, tokens) {
		printToken(tok);
		printf("\n");
	}
}

static inline void skipSpaces(lexer_t *l) {
	while (*l->current && isspace(*l->current)) next(l);
}

static void skipInlineComments(lexer_t *l) {
	if (!*l->current || !*(l->current + 1)) return;
	if (*l->current != '/' || *(l->current + 1) != '/') return;

	while (*l->current && *l->current != '\n') next(l);
}

static void skipMultilineComments(lexer_t *l) {
	if (!*l->current || !*(l->current + 1)) return;
	if (*l->current != '/' || *(l->current + 1) != '*') return;

	next(l); next(l);
	while (l->current && l->current + 1) {
		if (*l->current == '*' && *(l->current + 1) == '/') break;
		next(l);
	}
	if (!*l->current || !*(l->current + 1)) error(l, "Unterminated multiline comments");

	next(l); next(l);
}

ZTokens *ztokenize(char * program) {
	lexer_t l;

	l.program = program;
	l.current = program;
	l.row = 0;
	l.col = 0;
	l.tokens = zalloc(ZTokens);
	ZTokens *tokens = l.tokens;
	ZToken *curr;

	vec_init(l.tokens, 32);
	initKeywords();

	while (*l.current) {
		curr = NULL;
		skipSpaces(&l);
		skipInlineComments(&l);
		skipSpaces(&l);
		skipMultilineComments(&l);
		if (!*l.current) break;

		

		if (*l.current == '"') {
			curr = parseString(&l);
		} else if (isalpha(*l.current) || *l.current == '_') {
			curr = parseLiteral(&l);
		} else if (isdigit(*l.current)) {
			curr = parseInt(&l);
		} 

		if (!curr) {
			curr = parseSymbol(&l);
		}

		if (!curr) error(&l, "Error parsing near %.10s\n", l.current);
		addToken(&l, curr);
	}
	return tokens;
}
