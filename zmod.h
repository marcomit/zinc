#ifndef ZMOD_H
#define ZMOD_H

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

typedef enum {
	Z_ERROR,
	Z_WARNING,
	Z_INFO
} ZLogLevel;

typedef struct {
	char 			*filename;
	char 			*message;
	ZToken 		*token;
	ZLogLevel level;
} ZLog;

typedef enum {
	Z_LEXICAL,
	Z_SYNTAX,
	Z_SEMANTIC,
	Z_GENERATE
} ZPhase;

typedef struct {
	ZLog 		**errors;
	ZPhase 	currentPhase;
	char 		*filename;

	char 		**pathFiles;
	char 		**visitedFiles;
	/* Not yet implemented */
	bool 		verbose;
	u8 			optimizationLevel;
} ZState;

ZState *makestate(char *);

char *readfile(char *);

void error	(ZState *, ZToken *, char *, ...);
void warning(ZState *, ZToken *, char *, ...);
void info		(ZState *, ZToken *, char *, ...);

void visit(ZState *, char *);

void undoVisit(ZState *);

#endif
