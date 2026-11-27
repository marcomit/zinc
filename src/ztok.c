#include "zinc/zinc.h"

char *stoken(ZToken *token) {
    if (!token) return "(null)";
    switch(token->type) {
    case TOK_STR_START: return "{";
    case TOK_STR_END:   return "}";
    case TOK_STREAM: {
        zstr buff = NULL;
        for (usize i = 0; i < veclen(token->stream); i++)
            zs_cat(&buff, stoken(token->stream[i]));
        return buff;
    }
    case TOK_STR_LIT:
    case TOK_IDENT:     return token->str;

    #if Z_LSP
    case TOK_COMMENT:
    #endif
    case TOK_INT_LIT:
    case TOK_RUNE_LIT:
    case TOK_FLOAT_LIT: {
        const char *src = toksource(zActiveState, token);
        if (!src) return "";
        return zstrndup(arenaAllocator, (char *)src + token->offset, token->len);
    }
    #define DEF(id, str, _) case id: return str;

    #define TOK_FLOWS
    #define TOK_TYPES
    #define TOK_SYMBOLS

    #include "ztok.def"

    #undef TOK_SYMBOLS
    #undef TOK_TYPES
    #undef TOK_FLOWS

    #undef DEF


    default:
        break;
    }

    return "(not found)";
}

char *tokname(ZTokenType type) {
    switch (type) {
#define DEF(id, str, _) case id: return str;

    #define TOK_FLOWS
    #define TOK_TYPES
    #define TOK_SYMBOLS

    #include "ztok.def"

    #undef TOK_SYMBOLS
    #undef TOK_TYPES
    #undef TOK_FLOWS

    #undef DEF
        default: return NULL;
    }
}

void printToken(ZToken *token) {
    char *tok = stoken(token);
    printf("%s", tok);
}


void printTokens(ZToken **tokens) {
    printf("==== Tokens: %zu ====\n", veclen(tokens));
    for (usize i = 0; i < veclen(tokens); i++) {
        if (tokens[i]->newlineBefore) printf("\n");
        else printf(" ");
        printToken(tokens[i]);
    }
    printf("\n==== End tokens ====\n");
}

ZFile *tokfile(ZState *state, ZToken *t) {
    if (!state || !t || t->file_id == ANON_FILE_ID) return NULL;
    return zfile_get(state->fileTable, t->file_id);
}

const char *tokfilename(ZState *state, ZToken *t) {
    ZFile *f = tokfile(state, t);
    return f ? zfile_name(f) : NULL;
}

const char *toksource(ZState *state, ZToken *t) {
    ZFile *f = tokfile(state, t);
    return f ? zfile_content(f) : NULL;
}

const char *tokline(ZState *state, ZToken *t) {
    const char *source = toksource(state, t);
    if (!source) return NULL;
    const char *line = source + t->offset;
    while (line > source && line[-1] != '\n') line--;
    return line;
}

u32 tokcol(ZToken *t) {
    if (!t) return 0;
    return t->col >= t->len ? t->col - t->len + 1 : 1;
}

zsv toksv(ZState *state, ZToken *t) {
    const char *src = toksource(state, t);
    if (!src) return zsv_new(NULL, 0);
    return zsv_new(src + t->offset, t->len);
}
