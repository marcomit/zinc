#ifndef ZLEX_H
#define ZLEX_H

#include "base.h"
#include "zvec.h"

typedef enum {

#define DEF(id, str, m) id = m,
#define TOK_FLOWS
#define TOK_TYPES
#define TOK_DYN
#define TOK_SYMBOLS

#include "ztok.h"

#undef TOK_FLOWS
#undef TOK_TYPES
#undef TOK_DYN
#undef TOK_SYMBOLS

#undef DEF

} ZTokenType;


typedef struct {
	ZTokenType type;
	union {
		char *str;
		i64 integer;
		bool boolean;
	};
	usize row;
	usize col;
} ZToken;

ZToken **ztokenize(char *);

void printTokens(ZToken **);
void printToken(ZToken *);
#endif
