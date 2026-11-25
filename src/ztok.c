#include "zinc/zinc.h"

char *stoken(ZToken *token) {
    if (!token) return "(null)";
    switch(token->type) {
    case TOK_STR_START: return "{";
    case TOK_STR_END:   return "}";
    case TOK_STREAM: {
        char *buff = NULL;
        for (usize i = 0; i < veclen(token->stream); i++) {
            char *s = stoken(token->stream[i]);
            vecunion(buff, s, strlen(s));
        }
        return buff;
    }
    case TOK_STR_LIT:
    case TOK_IDENT:     return token->str;

    #if Z_LSP
    case TOK_COMMENT:
    #endif
    case TOK_INT_LIT:
    case TOK_RUNE_LIT:
    case TOK_FLOAT_LIT: return zstrndup(arenaAllocator, token->start, token->end - token->start);
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
