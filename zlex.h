#ifndef ZLEX_H
#define ZLEX_H

#include "zvec.h"
#include <stddef.h>
#define TOK_IDENT 256

typedef enum {
	TOK_LAST = TOK_IDENT - 1
#define DEF(id, str) ,id
#include "ztok.h"
#undef DEF
} zcc_token;

typedef struct {
	zcc_token type;
	char *value;
} zcc_Token;

typedef struct {
	char *program;
	char *current;
	vec(zcc_Token *) tokens;
	size_t row, col;
} lexer_t;

lexer_t *ztokenize(char *);

#endif
