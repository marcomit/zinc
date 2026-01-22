#include "zinc.h"

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
	ZState *state;

	/* Pointer to the entire file */
	char *program;

	/* Pointer to the current text */
	char *current;

	/* List of generated tokens */
	ZToken **tokens;

	/* Pointer to the start of the current line */
	char *line;

	/* Current position of the text */
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

ZToken *maketoken(ZTokenType type) {
	ZToken *self = zalloc(ZToken);
	self->type = type;
	return self;
}

static ZToken *makeident(char *name) {
	ZToken *self = maketoken(TOK_IDENT);
	self->str = name;
	return self;
}

static ZToken *makeinteger(int64_t value) {
	ZToken *self = maketoken(TOK_INT_LIT);
	self->integer = value;
	return self;
}

static ZToken *makestring(char *str) {
	ZToken *self = maketoken(TOK_STR_LIT);
	self->str = str;
	return self;
}

static void addToken(ZLexer *l, ZToken *token) {
	token->row = l->row;
	token->col = l->col;
	vecpush(l->tokens, token);
}

static void next(ZLexer *l) {
	if (!l || !l->current || !*l->current) return;
	if (*l->current == '\n') {
		l->row++;
		l->col = 0;
		l->line = l->current + 1;
	} else {
		l->col++;
	}
	l->current++;
}

static void skip(ZLexer *l, u8 chars) {
	while (chars--) next(l);
}

static ZToken *parseString(ZLexer *l) {
	if (*l->current != '"') return NULL;
	next(l);

	char *start = l->current;
	while (*l->current && *l->current != '"') next(l);

	if (!l->current) {
		error(l->state, veclast(l->tokens), "Unterinated string %.10s", start);
		return NULL;
	}
	next(l);

	int len = l->current - start - 1;
	char *buff = allocator.alloc(len + 1);
	memcpy(buff, start, len);
	buff[len] = 0;

	return makestring(buff);
}

static ZToken *parseSymbol(ZLexer *l) {
	if (false) { /* Empty if statement only for macro definition*/ }
	#define DEF(id, str, _) else if(!strncmp(str, l->current, strlen(str))) { \
		skip(l, strlen(str));																										\
		return maketoken(id);																									\
	}

	#define TOK_SYMBOLS
	#define TOK_FLOWS
	#define TOK_TYPES

	#include "ztok.h"

	#undef TOK_TYPES
	#undef TOK_FLOWS
	#undef TOK_SYMBOLS
	#undef DEF
	
	error(l->state, veclast(l->tokens), "Unexpected symbol");

	return NULL;
}

static ZToken *parseInt(ZLexer *l) {
	char *start = l->current;
	if (!isdigit(*l->current)) return NULL;

	while (isdigit(*l->current)) next(l);

	long long value = strtoll(start, NULL, 10);
	if (errno == ERANGE) error(l->state, veclast(l->tokens), "Invalid integer range %.10s", start);

	return makeinteger(value);
}

static ZToken *parseLiteral(ZLexer *l) {
	if (!isalpha(*l->current) && *l->current != '_') return NULL;

	char *start = l->current;
	while (isalnum(*l->current) || *l->current == '_') next(l);

	size_t len = l->current - start;
	ZTokenType type = findKeyword(start, len);

	if (type == TOK_IDENT) {
		return makeident(strndup(start, len));
	}

	return maketoken(type);
}

static inline void skipSpaces(ZLexer *l) {
	while (*l->current && isspace(*l->current)) next(l);
}

static void skipInlineComments(ZLexer *l) {
	if (!*l->current || !*(l->current + 1)) return;
	if (*l->current != '/' || *(l->current + 1) != '/') return;

	while (*l->current && *l->current != '\n') next(l);
	if (*l->current) next(l);
}

static void skipMultilineComments(ZLexer *l) {
	if (!*l->current || !*(l->current + 1)) return;
	if (*l->current != '/' || *(l->current + 1) != '*') return;

	next(l); next(l);
	while (l->current && l->current + 1) {
		if (*l->current == '*' && *(l->current + 1) == '/') break;
		next(l);
	}
	if (!*l->current || !*(l->current + 1)) {
		error(l->state, veclast(l->tokens), "Unterminated multiline comments");
	}

	next(l); next(l);
}

ZLexer *makelexer(ZState *state) {
	char *program = readfile(state->filename);
	
	if (!program) return NULL;

	ZLexer *self = zalloc(ZLexer);

	self->row = 1;
	self->col = 0;
	self->tokens = NULL;
	self->program = program;
	self->current = program;
	self->line = program;
	self->state = state;
	return self;
}

ZToken **ztokenize(ZState *state) {
	state->currentPhase = Z_PHASE_LEXICAL;
	ZLexer *l = makelexer(state);

	if (!l) return NULL;

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

		char *sourcePtr = l->current;
		char *sourceLinePtr = l->line;
		if (*l->current == '"') {
			curr = parseString(l);
		} else if (isalpha(*l->current) || *l->current == '_') {
			curr = parseLiteral(l);
		} else if (isdigit(*l->current)) {
			curr = parseInt(l);
		} else {
			curr = parseSymbol(l);
		}

		if (!curr) {
			error(l->state, veclast(l->tokens), "Unexpected symbol\n");
			next(l);
		} else {
			curr->sourceLinePtr = sourceLinePtr;
			curr->sourcePtr  		= sourcePtr;
			addToken(l, curr);
		}
	}
	return l->tokens;
}
