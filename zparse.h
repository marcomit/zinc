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
typedef struct ZNodeExpr ZNodeExpr;
typedef struct ZNodeStmt ZNodeStmt;
typedef struct ZNodeBlock ZNodeBlock;
typedef struct ZNodeIf ZNodeIf;
typedef struct ZNodeVar ZNodeVar;
typedef struct ZNodeBinary ZNodeBinary;

struct ZNodeExpr {
	ZToken *tok;
};

struct ZNodeBinary {
	ZToken *op;
	ZNodeExpr *left;
	ZNodeExpr *right;
};

struct ZNodeUnary {
	ZToken *op;
	ZNodeExpr *operand;
};

struct ZNodeVar {
	ZToken *lvalue;
	ZNodeExpr *rvalue;
};

struct ZNodeStmt {
};

struct ZNodeBlock {
	vec(ZNodeStmt *) stmts;
};

struct ZNodeIf {
	ZNodeExpr *cond;
	ZNodeBlock *branchTrue;
	ZNodeBlock *branchFalse;
};

struct ZNodeWhile {
	ZNodeExpr *cond;
	ZNodeBlock *branch;
};

struct ZNode {
	ZNodeType type;
	union {
		ZNodeExpr *expr;
		ZNodeStmt *stmt;
		ZNodeBlock *block;
		ZNodeIf *ifNode;
		ZNodeWhile *whileNode;
		ZNodeVar *var;
	};
};

ZNode *parse(ZTokens *);

#endif
