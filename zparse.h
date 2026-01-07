#ifndef ZPARSE_H
#define ZPARSE_H

#include "base.h"
#include "zlex.h"

typedef enum {
	NODE_BLOCK, 		// All inside a {} is a block. A list of statement
	NODE_IF,
	NODE_WHILE,
	NODE_RETURN,
	NODE_VAR_DECL,
	NODE_ASSIGN,
	NODE_BINARY,
	NODE_UNARY,
	NODE_CALL,     	// Function call
	NODE_FUNC,     	// Function definition
	NODE_LITERAL,  	// Numbers, strings, etc.
	NODE_IDENTIFIER,
	NODE_CAST,
	NODE_STRUCT,
	NODE_SUBSCRIPT,
	NODE_MEMBER,
	NODE_PROGRAM
} ZNodeType;

typedef struct ZNode ZNode;
typedef struct ZType ZType;
typedef struct ZField ZField;

typedef enum ZTypeKind {
	Z_TYPE_PRIMITIVE,
	Z_TYPE_STRUCT,
	Z_TYPE_ARRAY,
	Z_TYPE_FUNCTION,
	Z_TYPE_POINTER
} ZTypeKind;

struct ZField {
	ZType *type;
	ZToken *field;
};

struct ZType {
	ZTypeKind kind;

	union {
		// For PRIMITIVE (e.g. void or int)
		ZToken *token;

		// For POINTER (The type the pointer points to)
		ZType *base;

		struct {
			ZToken *name;
			ZField **fields;
		} strct;

		struct {
			ZType *ret;
			ZType **args;
		} func;

		struct {
			ZType *base;
			usize size;
		} array;
	};

	bool constant;
};

struct ZNode {
	ZNodeType type;
	ZType *resolved;
	ZToken *tok;
	union {
		// Can be used for both if and ternary operator
		struct {
			ZNode *cond;
			ZNode *body;
			ZNode *elseBranch;
		} ifStmt;

		struct {
			ZNode *cond;
			ZNode *branch;
		} whileStmt;

		struct {
			ZToken *op;
			ZNode *left;
			ZNode *right;
		} binary;

		struct {
			ZToken *op;
			ZNode *operand;
		} unary;

		struct {
			ZType *type;
			ZNode *lvalue;
			ZNode *rvalue;
		} varDecl;

		struct {
			ZNode *lvalue;
			ZNode *rvalue;
		} varAssign;

		ZNode ** block;

		struct {
			ZType *target;
			ZNode *expr;
		} cast;

		struct {
			ZType *ret;
			ZToken *ident;

			ZField **args;

			ZNode *body;
			// One of main feature of zinc is receiver functions
			// Used to attach functions to every type of types.
			ZType *receiver;
		} funcDef;

		struct {
			ZNode *callee;
			ZNode ** args;
		} call;

		struct {
			ZToken *ident;
			ZField **fields;
		} structDef;

		struct {
			ZNode *object;
			ZToken *field;
			bool isArrow;
		} memberAccess;

		struct {
			ZNode *expr; // Can be NULL for void returns
		} returnStmt;

		struct {
			ZNode *arr;
			ZNode *index;
		} subscript;

		ZNode **program;

		ZToken *literalTok;
		ZToken *identTok;
	};
};

ZNode *zparse(ZToken **);

void printNode(ZNode *, u8);

#endif
