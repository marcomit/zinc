#ifndef ZERR_H
#define ZERR_H

#include "zlex.h"
#include "base.h"

typedef enum {
	Z_ERROR,
	Z_WARNING,
	Z_INFO
} ZErrorLevel;

typedef struct ZError {
	char *filename;
	char *message;
	ZToken *token;
} ZError;

ZError *makeerr(char *, ZToken *, char *, ...);
ZError *makewarn(char *, ZToken *, char *, ...);

#endif
