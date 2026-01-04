#ifndef ZLEX_H
#define ZLEX_H

#include "zvec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
		int64_t integer;
		bool boolean;
	};
	size_t row;
	size_t col;
} ZToken;

typedef struct {
	asVec(ZToken *);
	int32_t current;
} ZTokens;

ZTokens *ztokenize(char *);

void printTokens(ZTokens *);
#endif
