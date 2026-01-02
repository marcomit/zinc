#ifndef ZPARSE_H
#define ZPARSE_H

#include "zlex.h"

typedef enum {
	IF_NODE,
	WHILE_NODE,
	BLOCK_NODE,
	DECL_VAR,
	BINARY_OP,
} ZNodeType;

typedef struct ZNode ZNode;

typedef struct ZNodeIf {
	ZNode *cond;
	ZNode *branchTrue; // Always present
	ZNode *branchFalse; // Only if there is the else
} ZNodeIf;

typedef struct ZNodeWhile {
	ZNode *cond;
	ZNode *branch;
} ZNodeWhile;

typedef struct ZNodeExpr {
	ZToken *tok;
} ZNodeExpr;

typedef struct ZNodeBinary {
	ZNode *left;
	ZNode *operand;
	ZNode *right;
} ZNodeBinary;

struct ZNode {
	ZNodeType type;
	union {
		ZNodeIf *ifNode;
		ZNodeWhile *whileNode;
		ZNodeExpr *expr;
		ZNodeBinary *binary;
	};
};

ZNode *parse(ZTokens *);

#endif
