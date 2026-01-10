#ifndef ZSEM_H
#define ZSEM_H

#include "zparse.h"

enum ZSymType {
	Z_SYM_VAR,
	Z_SYM_FUNC,
	Z_SYM_STRUCT
};

typedef struct ZSymbol {
	ZSymType kind;
	char *name;
	ZType *type;
	union {
		struct {
			ZNode *initializer;
			bool constant;
		} var;

		struct {
			ZNode **params;
			ZNode *receiver;
			ZNode *body;
		} func;

		struct {
			ZNode **fields;
			ZToken *name;
		} structDef;
	};
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
} ZSemantic;

void analyze(ZNode *);

bool typesEqual(ZType *, ZType *);

#endif
