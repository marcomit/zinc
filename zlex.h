#ifndef ZLEX_H
#define ZLEX_H

#include "zvec.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
	TOK_LAST = 255

#define DEF(id, str) ,id
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
} ZToken;

typedef struct {
	size_t current;
	asVec(ZToken *);
} ZTokens;

ZTokens *ztokenize(char *);

void printTokens(ZTokens *);
#endif
