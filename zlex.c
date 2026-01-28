#include "zinc.h"

#define FNV_OFFSET 2166136261u
#define FNV_PRIME	 16777619u

#define HASHMAP_TOK_LEN 128
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

	/* Track if newline was seen since last token */
	bool sawNewline;

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

static KeyboardEntry keywords[] = {
	#define DEF(id, str, _) { str, id },

	#define TOK_FLOWS
	#define TOK_TYPES

	#include "ztok.h"

	#undef TOK_TYPES
	#undef TOK_FLOWS

	#undef DEF
};

static void initKeywords() {
	usize len = sizeof(keywords) / sizeof(keywords[0]);
	for (size_t i = 0; i < len; i++) {
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

static ZToken *makefloat(double value) {
	ZToken *self = maketoken(TOK_FLOAT_LIT);
	self->floating = value;
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
		l->sawNewline = true;
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

	// First pass: count length and check for unterminated string
	size_t len = 0;
	while (*l->current && *l->current != '"') {
		if (*l->current == '\\' && *(l->current + 1)) {
			next(l);  // skip backslash
		}
		next(l);
		len++;
	}

	if (!*l->current) {
		error(l->state, veclast(l->tokens), "Unterminated string %.10s", start);
		return NULL;
	}
	next(l);  // consume closing quote

	// Second pass: build string with escape sequences
	char *buff = allocator.alloc(len + 1);
	char *src = start;
	size_t i = 0;

	while (*src && src < l->current - 1) {
		if (*src == '\\' && *(src + 1)) {
			src++;
			switch (*src) {
				case 'n':  buff[i++] = '\n'; break;
				case 't':  buff[i++] = '\t'; break;
				case 'r':  buff[i++] = '\r'; break;
				case '\\': buff[i++] = '\\'; break;
				case '"':  buff[i++] = '"';  break;
				case '\'': buff[i++] = '\''; break;
				case '0':  buff[i++] = '\0'; break;
				default:   buff[i++] = *src; break;  // unknown escape, keep as-is
			}
		} else {
			buff[i++] = *src;
		}
		src++;
	}
	buff[i] = '\0';

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

static ZToken *parseNumber(ZLexer *l) {
	char *start = l->current;
	if (!isdigit(*l->current)) return NULL;

	while (isdigit(*l->current)) next(l);

	bool isFloat = false;
	if (*l->current == '.' && isdigit(*(l->current + 1))) {
		isFloat = true;
		next(l);  // consume '.'
		while (isdigit(*l->current)) next(l);
	}

	// Handle scientific notation (e.g., 1e10, 1.5e-3)
	if (*l->current == 'e' || *l->current == 'E') {
		isFloat = true;
		next(l);  // consume 'e' or 'E'
		if (*l->current == '+' || *l->current == '-') next(l);
		while (isdigit(*l->current)) next(l);
	}

	if (isFloat) {
		double value = strtod(start, NULL);
		if (errno == ERANGE) error(l->state, veclast(l->tokens), "Invalid float range %.10s", start);
		return makefloat(value);
	}

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

	// Also set str field for keywords so getMacroByName can compare them
	ZToken *tok = maketoken(type);
	tok->str = strndup(start, len);
	return tok;
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
	self->sawNewline = true;  // First token is "after" a newline
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
			curr = parseNumber(l);
		} else {
			curr = parseSymbol(l);
		}

		if (!curr) {
			error(l->state, veclast(l->tokens), "Unexpected symbol\n");
			next(l);
		} else {
			curr->sourceLinePtr = sourceLinePtr;
			curr->sourcePtr  		= sourcePtr;
			curr->newlineBefore = l->sawNewline;
			l->sawNewline = false;
			addToken(l, curr);
		}
	}
	return l->tokens;
}
