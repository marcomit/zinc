#ifndef ZSEM_H
#define ZSEM_H

#include "zparse.h"

typedef enum {
	Z_SYM_VAR,
	Z_SYM_FUNC,
	Z_SYM_STRUCT
} ZSymType;

typedef struct ZSymbol {
	ZSymType kind;
	char *name;
	ZType *type;
	ZNode *node;
} ZSymbol;

typedef struct ZScope {
	ZSymbol **symbols;
	struct ZScope *parent;
	u32 depth;
} ZScope;

typedef struct ZSymTable {
	ZScope *global;
	ZScope *current;
} ZSymTable;

typedef struct ZSemantic {
	ZNode *root;
	ZNode *current;

	ZSymTable *table;
} ZSemantic;

void zanalyze(ZNode *);

bool typesEqual(ZType *, ZType *);

#endif
