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
	ZToken **tokens;
	size_t row, col;
} ZLexer;

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

static void addToken(ZLexer *l, ZToken *token) {
	token->row = l->row;
	token->col = l->col;
	vecpush(l->tokens, token);
}

static void next(ZLexer *l) {
	if (!*l->current) return;
	if (*l->current == '\n') {
		l->row++;
		l->col = 0;
	} else {
		l->col++;
	}
	l->current++;
}

static void error(ZLexer *l, const char *fmt, ...) {
	va_list args;
	va_start(args, fmt);

	fprintf(stderr, "%zu,%zu: ", l->row, l->col);
	vfprintf(stderr, fmt, args);
	fputc('\n', stderr);

	va_end(args);
	// exit(1);
}

static ZToken *parseString(ZLexer *l) {
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

static ZToken *parseSymbol(ZLexer *l) {
	if (false) { /* Empty if statement only for macro definition*/ }
	#define DEF(id, str, _) else if(!strncmp(str, l->current, strlen(str))) { \
		l->current += strlen(str);																							\
		return createToken(id);																									\
	}

	#define TOK_SYMBOLS
	#define TOK_FLOWS
	#define TOK_TYPES

	#include "ztok.h"

	#undef TOK_TYPES
	#undef TOK_FLOWS
	#undef TOK_SYMBOLS
	#undef DEF
	
	error(l, "Unexpected symbol near '%.5s'", l->current);

	while (*l->current && !isspace(*l->current)) next(l);

	return NULL;
}

static ZToken *parseInt(ZLexer *l) {
	char *start = l->current;
	if (!isdigit(*l->current)) return NULL;

	while (isdigit(*l->current)) next(l);

	long long value = strtoll(start, NULL, 10);
	if (errno == ERANGE) error(l, "Invalid integer range %.10s", start);

	return createInteger(value);
}

static ZToken *parseLiteral(ZLexer *l) {
	if (!isalpha(*l->current) && *l->current != '_') return NULL;

	char *start = l->current;
	while (isalnum(*l->current) || *l->current == '_') next(l);

	size_t len = l->current - start;
	ZTokenType type = findKeyword(start, len);

	if (type == TOK_IDENT) {
		return createIdent(strndup(start, len));
	}

	return createToken(type);
}

static inline void skipSpaces(ZLexer *l) {
	while (*l->current && isspace(*l->current)) next(l);
}

static void skipInlineComments(ZLexer *l) {
	if (!*l->current || !*(l->current + 1)) return;
	if (*l->current != '/' || *(l->current + 1) != '/') return;

	while (*l->current && *l->current != '\n') next(l);
}

static void skipMultilineComments(ZLexer *l) {
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

ZLexer *makelexer(char *program) {
	ZLexer *self = zalloc(ZLexer);

	self->row = 0;
	self->col = 0;
	self->tokens = NULL;
	self->program = program;
	self->current = program;
	return self;
}

ZToken **ztokenize(char * program) {
	ZLexer *l = makelexer(program);
	ZToken *curr;

	initKeywords();

	while (*l->current) {
		curr = NULL;
		
		while (true) {
			char *start = l->current;
			skipSpaces(l);
			skipInlineComments(l);
			skipMultilineComments(l);
			if (l->current == start) break;
		}

		if (!*l->current) break;

		if (*l->current == '"') {
			curr = parseString(l);
		} else if (isalpha(*l->current) || *l->current == '_') {
			curr = parseLiteral(l);
		} else if (isdigit(*l->current)) {
			curr = parseInt(l);
		} else {
			curr = parseSymbol(l);
		}

		if (!curr) error(l, "Error parsing near %.10s\n", l->current);
		addToken(l, curr);
	}
	return l->tokens;
}
