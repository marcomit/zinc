// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zinc.h"
#include <ctype.h>

#define FNV_OFFSET 2166136261u
#define FNV_PRIME    16777619u

#define HASHMAP_TOK_LEN 256
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
    usize row, col;

    /* Track if newline was seen since last token */
    bool sawNewline;

} ZLexer;

typedef struct {
    const char *keyword;
    ZTokenType type;
} KeywordEntry;

/* hashmap with fixed size.
 * The fixed size is not a problem because the entries are limited
 * to the number of symbols (defined in ztok.def). */
static KeywordEntry keywordEntries[HASHMAP_TOK_LEN];

u16 ZTokenMask[] = {
#define DEF(name, n, masks) [name] = (u16)masks,

#define TOK_FLOWS
#define TOK_TYPES
#define TOK_DYN
#define TOK_SYMBOLS

#include "ztok.def"

#undef TOK_FLOWS
#undef TOK_TYPES
#undef TOK_DYN
#undef TOK_SYMBOLS

#undef DEF
};

inline bool tokmask(ZToken *tok, u16 mask) {
    return ZTokenMask[tok->type] & mask;
}

ZTokenStream *maketokstream(ZToken **tokens, ZTokenStream *prev) {
    ZTokenStream *self = zalloc(ZTokenStream);
    self->list = tokens;
    self->current = 0;
    self->prev = prev;
    self->end = veclen(tokens);
    return self;
}

// FNV-1a is defined to wrap modulo 2^32; the unsigned overflow is intentional
// (and legal C), so exempt it from -fsanitize=unsigned-integer-overflow.
NOSANITIZE("unsigned-integer-overflow")
u32 hashStr(const char *buff, size_t len) {
    u32 hash = FNV_OFFSET;
    for (size_t i = 0; i < len; i++) {
        hash ^= (u8)(buff[i]);
        hash *= FNV_PRIME;
    }
    return hash;
}

static KeywordEntry keywords[] = {
    #define DEF(id, str, _) { str, id },

    #define TOK_FLOWS
    #define TOK_TYPES

    #include "ztok.def"

    #undef TOK_TYPES
    #undef TOK_FLOWS

    #undef DEF
};

static void initKeywords() {
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    usize len = sizeof(keywords) / sizeof(keywords[0]);
    for (size_t i = 0; i < len; i++) {
        const char *name = keywords[i].keyword;
        u32 hash = hashStr(name, strlen(name)) & HASHMAP_TOK_MASK;

        while (keywordEntries[hash].keyword != NULL) {
            hash = (hash + 1) & HASHMAP_TOK_MASK;
        }
        keywordEntries[hash].keyword = keywords[i].keyword;
        keywordEntries[hash].type = keywords[i].type;
    }
}

ZTokenType findKeyword(const char *ident, size_t len) {
    u32 hash = hashStr(ident, len) & HASHMAP_TOK_MASK;

    while (keywordEntries[hash].keyword != NULL) {
        if (strlen(keywordEntries[hash].keyword) == len &&
                memcmp(keywordEntries[hash].keyword, ident, len) == 0) {
            return keywordEntries[hash].type;
        }
        hash = (hash + 1) & HASHMAP_TOK_MASK;
    }

    return TOK_IDENT;
}

ZToken *maketoken(ZTokenType type, char *start, char *end) {
    ZToken *self    = zalloc(ZToken);
    self->type      = type;
    self->start     = start;
    self->end       = end;
    return self;
}

ZToken *makeident(char *name, char *start, char *end) {
    ZToken *self = maketoken(TOK_IDENT, start, end);
    self->str = name;
    return self;
}

static ZToken *makeinteger(i64 value, char *start, char *end) {
    ZToken *self = maketoken(TOK_INT_LIT, start, end);
    self->integer = value;
    return self;
}

static ZToken *makefloat(double value, char *start, char *end) {
    ZToken *self = maketoken(TOK_FLOAT_LIT, start, end);
    self->floating = value;
    return self;
}

static ZToken *makestring(char *str, char *start, char *end) {
    ZToken *self = maketoken(TOK_STR_LIT, start, end);
    self->str = str;
    return self;
}

bool tokeneq(ZToken *a, ZToken *b) {
    if (!a || !b) return false;
    if (a->type != b->type) return false;

    if (a->type == TOK_IDENT || tokmask(a, TOK_FLOWS_MASK)) {
        return strcmp(a->str, b->str) == 0;
    }

    return true;
}

static void addToken(ZLexer *l, ZToken *token) {
    token->row      = l->row;
    token->col      = l->col;
    token->filename = l->state->filename;
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
    while (chars) {
        next(l);
        chars--;
    }
}

static u32 decodeUtf8(char **src) {
    u8 c = (u8)**src;
    if ((c & 0x80) == 0x00) {
        (*src)++;
        return c;
    } else if ((c & 0xE0) == 0xC0) {
        if (((u8)(*src)[1] & 0xC0) != 0x80) { (*src)++; return 0xFFFD; }
        u32 cp = c & 0x1F; (*src)++;
        cp = (cp << 6) | ((u8)**src & 0x3F); (*src)++;
        return cp;
    } else if ((c & 0xF0) == 0xE0) {
        if (((u8)(*src)[1] & 0xC0) != 0x80 ||
            ((u8)(*src)[2] & 0xC0) != 0x80) { (*src)++; return 0xFFFD; }
        u32 cp = c & 0x0F; (*src)++;
        cp = (cp << 6) | ((u8)**src & 0x3F); (*src)++;
        cp = (cp << 6) | ((u8)**src & 0x3F); (*src)++;
        return cp;
    } else if ((c & 0xF8) == 0xF0) {
        if (((u8)(*src)[1] & 0xC0) != 0x80 ||
            ((u8)(*src)[2] & 0xC0) != 0x80 ||
            ((u8)(*src)[3] & 0xC0) != 0x80) { (*src)++; return 0xFFFD; }
        u32 cp = c & 0x07; (*src)++;
        cp = (cp << 6) | ((u8)**src & 0x3F); (*src)++;
        cp = (cp << 6) | ((u8)**src & 0x3F); (*src)++;
        cp = (cp << 6) | ((u8)**src & 0x3F); (*src)++;
        return cp;
    }
    (*src)++;
    return 0xFFFD;
}

static u32 decodeHexCode(ZLexer *l, char **src, int len) {
    (*src)++;
    u32 cp = 0;

    for (int i = 0; i < len; i++) {
        char c = **src;
        if (!c) {
            zlog(l->state, veclast(l->tokens), Z1001);
            return 0xFFFD;
        }
        if      (c >= '0' && c <= '9') cp = (cp << 4) | (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') cp = (cp << 4) | (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') cp = (cp << 4) | (u32)(c - 'A' + 10);
        else {
            zlog(l->state, veclast(l->tokens), Z1002);
            return 0xFFFD;
        }
        (*src)++;
    }

    return cp;
}

static u32 parseEscapeChar(ZLexer *l, char **src) {
    switch (**src) {
    case 'n':  (*src)++; return '\n';
    case 't':  (*src)++; return '\t';
    case 'r':  (*src)++; return '\r';
    case '\\': (*src)++; return '\\';
    case '"':  (*src)++; return '"';
    case '\'': (*src)++; return '\'';
    case '0':  (*src)++; return '\0';
    case 'u': return decodeHexCode(l, src, 4);
    case 'U': return decodeHexCode(l, src, 8);
    default: {
        u32 cp = (u8)**src;
        (*src)++;
        return cp;
    }
    }
}

static void pushCodepointUtf8(char **buff, u32 cp) {
    if (cp < 0x80) { // starts with 0
        vecpush(*buff, (char)cp);
    } else if (cp < 0x800) { // starts with 110
        vecpush(*buff, (char)(0xC0 | (cp >> 6)));
        vecpush(*buff, (char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) { // starts with 1110
        vecpush(*buff, (char)(0xE0 | (cp >> 12)));
        vecpush(*buff, (char)(0x80 | ((cp >> 6) & 0x3F)));
        vecpush(*buff, (char)(0x80 | (cp & 0x3F)));
    } else { // starts with 11110
        vecpush(*buff, (char)(0xF0 | (cp >> 18)));
        vecpush(*buff, (char)(0x80 | ((cp >> 12) & 0x3F)));
        vecpush(*buff, (char)(0x80 | ((cp >> 6) & 0x3F)));
        vecpush(*buff, (char)(0x80 | (cp & 0x3F)));
    }
}

static ZToken *parseString(ZLexer *l) {
    if (*l->current != '"') return NULL;
    next(l);

    char *start = l->current;
    char *buff = NULL;
    char *src = l->current;

    while (*src && *src != '"') {
        u32 cp;
        if (*src == '\\' && *(src + 1)) {
            src++;
            cp = parseEscapeChar(l, &src);
        } else {
            cp = decodeUtf8(&src);
        }
        pushCodepointUtf8(&buff, cp);
    }

    if (*src == '"') src++;
    else {
        ZToken *tok = maketoken(TOK_STR_LIT, start - 1, l->current);
        tok->row    = l->row;
        tok->col    = l->col;
        tok->sourcePtr = start - 1;
        tok->sourceLinePtr = l->line;
        zlog(l->state, tok, Z1003);
        return NULL;
    }
    vecpush(buff, '\0');
    l->current = src;

    return makestring(buff, start, l->current);
}

static ZToken *makeRune(u32 codepoint, char *start, char *end) {
    ZToken *self = maketoken(TOK_RUNE_LIT, start, end);
    self->integer = (i64)codepoint;
    return self;
}

static ZToken *parseRune(ZLexer *l) {
    if (*l->current != '\'') return NULL;
    char *start = l->current;
    char *src = l->current + 1;

    if (!*src) return NULL;

    u32 cp;
    if (*src == '\\' && *(src + 1)) {
        src++;
        cp = parseEscapeChar(l, &src);
    } else {
        cp = decodeUtf8(&src);
    }

    if (*src != '\'') return NULL;
    src++;

    l->current = src;
    return makeRune(cp, start, src);
}

static ZToken *parseSymbol(ZLexer *l) {
    if (false) { /* Empty if statement only for macro definition*/ }
    #define DEF(id, s, _) else if(!strncmp(s, l->current, strlen(s))) {         \
        ZToken *tok = maketoken(id, l->current, l->current + strlen(s));        \
        skip(l, strlen(s));                                                     \
        tok->str = s;                                                           \
        return tok;                                                             \
    }

    #define TOK_SYMBOLS
    #define TOK_FLOWS
    #define TOK_TYPES

    #include "ztok.def"

    #undef TOK_TYPES
    #undef TOK_FLOWS
    #undef TOK_SYMBOLS
    #undef DEF

    zlog(l->state, veclast(l->tokens), Z1004);


    ZToken *tok = maketoken(0, l->current, l->current);
    tok->str = "";
    return NULL;
}

static ZToken *parseHexNumber(ZLexer *l) {
    if (strncmp(l->current, "0x", 2) != 0) return NULL;
    next(l); next(l);
    char *start = l->current;
    while (isdigit(*l->current) ||
            (tolower(*l->current) >= 'a' &&
             tolower(*l->current) <= 'f')) next(l);

    errno = 0;
    unsigned long long value = strtoull(start, NULL, 16);
    if (errno == ERANGE) zlog(l->state, veclast(l->tokens), Z1005, start);

    return makeinteger((i64)value, start, l->current);
}

static ZToken *parseBinNumber(ZLexer *l) {
    if (strncmp(l->current, "0b", 2) != 0) return NULL;
    next(l); next(l);
    char *start = l->current;

    while (*l->current == '0' || *l->current == '1') next(l);

    errno = 0;
    unsigned long long value = strtoull(start, NULL, 2);
    if (errno == ERANGE) zlog(l->state, veclast(l->tokens), Z1005, start);

    return makeinteger((i64)value, start, l->current);
}

static ZToken *parseNumber(ZLexer *l) {
    char *start = l->current;
    if (strncmp(start, "0x", 2) == 0) {
        return parseHexNumber(l);
    } else if (strncmp(start, "0b", 2) == 0) {
        return parseBinNumber(l);
    }

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
        if (!isdigit(*l->current)) {
            zlog(l->state, veclast(l->tokens), Z1006);
            return NULL;
        }
        while (isdigit(*l->current)) next(l);
    }

    if (isFloat) {
        errno = 0;
        double value = strtod(start, NULL);
        if (errno == ERANGE) zlog(l->state, veclast(l->tokens), Z1007, start);
        return makefloat(value, start, l->current);
    }

    errno = 0;
    long long value = strtoll(start, NULL, 10);
    if (errno == ERANGE) zlog(l->state, veclast(l->tokens), Z1005, start);

    return makeinteger(value, start, l->current);
}

static ZToken *parseLiteral(ZLexer *l) {
    if (!isalpha(*l->current) && *l->current != '_') return NULL;

    char *start = l->current;
    while (isalnum(*l->current) || *l->current == '_') next(l);

    size_t len = l->current - start;
    ZTokenType type = findKeyword(start, len);

    if (type == TOK_IDENT) {
        return makeident(strndup(start, len), start, l->current);
    }

    // Also set str field for keywords so getMacroByName can compare them
    ZToken *tok = maketoken(type, start, l->current);
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
    while (*l->current && l->current + 1) {
        if (*l->current == '*' && *(l->current + 1) == '/') break;
        next(l);
    }
    if (!l->current[0] || !l->current[1]) {
        zlog(l->state, veclast(l->tokens), Z1008);
        return;
    }

    next(l); next(l);
}

ZLexer *makelexer(ZState *state) {
    char *program = readfile(state->filename);

    if (!program) {
        zlog(state, NULL, Z1009, state->filename, strerror(errno));
        return NULL;
    }

    ZLexer *self = zalloc(ZLexer);

    self->row           = 1;
    self->col           = 0;
    self->tokens        = NULL;
    self->program       = program;
    self->current       = program;
    self->line          = program;
    self->state         = state;
    self->sawNewline    = true;  // First token is "after" a newline
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
        } else if (*l->current == '\'') {
            curr = parseRune(l);
            if (!curr) curr = parseSymbol(l);
        } else if (isalpha(*l->current) || *l->current == '_') {
            curr = parseLiteral(l);
        } else if (isdigit(*l->current)) {
            curr = parseNumber(l);
        } else {
            curr = parseSymbol(l);
        }

        if (!curr) {
            zlog(l->state, veclast(l->tokens), Z1004);
            next(l);
        } else {
            curr->sourceLinePtr = sourceLinePtr;
            curr->sourcePtr     = sourcePtr;
            curr->newlineBefore = l->sawNewline;
            l->sawNewline       = false;
            addToken(l, curr);
        }
    }
    return l->tokens;
}
