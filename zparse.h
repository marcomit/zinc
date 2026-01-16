#ifndef ZPARSE_H
#define ZPARSE_H

#include "base.h"
#include "zlex.h"
#include "zmod.h"

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
	NODE_MODULE,
	NODE_PROGRAM,
	NODE_UNION,
	NODE_FIELD,
	NODE_TYPEDEF
} ZNodeType;

typedef struct ZNode ZNode;
typedef struct ZType ZType;

typedef enum ZTypeKind {
	Z_TYPE_PRIMITIVE,
	Z_TYPE_STRUCT,
	Z_TYPE_ARRAY,
	Z_TYPE_FUNCTION,
	Z_TYPE_POINTER
} ZTypeKind;

struct ZType {
	ZTypeKind kind;

	union {
		// For PRIMITIVE (e.g. void or int)
		ZToken *token;

		// For POINTER (The type the pointer points to)
		ZType *base;

		struct {
			ZToken *name;
			ZNode **fields;
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
			ZToken *operat;
			ZNode *operand;
		} unary;

		struct {
			ZType *type;
			ZToken *ident;
			ZNode *rvalue; // Null if not initialized
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
			ZType *type;
			ZToken *identifier;
		} field;

		struct {
			ZType *ret;
			ZToken *ident;

			ZNode **args;

			ZNode *body;
			// One of main feature of zinc is receiver functions
			// Used to attach functions to every type of types.
			ZNode *receiver;
		} funcDef;

		struct {
			ZNode *callee;
			ZNode ** args;
		} call;

		struct {
			ZToken *ident;
			ZNode **fields;
		} structDef;

		struct {
			ZToken *ident;
			ZNode **fields;
		} unionDef;

		struct {
			ZNode *object;
			ZToken *field;
		} memberAccess;

		struct {
			ZNode *expr; // Can be NULL for void returns
		} returnStmt;

		struct {
			ZNode *arr;
			ZNode *index;
		} subscript;

		struct {
			ZToken *alias;
			ZType *type;
		} typeDef;

		struct {
			ZToken *name;
			ZNode *root;
		} module;

		ZNode **program;

		ZToken *literalTok;
		ZToken *identTok;
		
	};
};

ZNode *zparse(ZState *, ZToken **);
#endif
