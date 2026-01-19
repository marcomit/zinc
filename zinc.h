#ifndef ZINC_H
#define ZINC_H

#include "base.h"
#include "zvec.h"
#include "zmem.h"

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
	char *sourcePtr;
	char *sourceLinePtr;
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
	Z_PHASE_LEXICAL,
	Z_PHASE_SYNTAX,
	Z_PHASE_SEMANTIC,
	Z_PHASE_GENERATE
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


/* ================== Syntax analysis	================== */
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
	NODE_TYPEDEF,
	NODE_FOREIGN,
	NODE_DEFER
} ZNodeType;

typedef struct ZNode ZNode;
typedef struct ZType ZType;

typedef enum ZTypeKind {
	Z_TYPE_PRIMITIVE,
	Z_TYPE_STRUCT,
	Z_TYPE_ARRAY,
	Z_TYPE_FUNCTION,
	Z_TYPE_POINTER,
	Z_TYPE_TUPLE
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

		ZType **tuple;
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
			ZNode *receiver;

			ZToken **generics;
		} funcDef;

		struct {
			ZType 	*ret;
			ZToken 	*tok;
			ZType 	**args;
		} foreignFunc;

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
			ZNode *expr;
		} deferStmt;

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

/* ================== Semantic analysis	================== */
typedef enum {
	Z_SYM_VAR,
	Z_SYM_FUNC,
	Z_SYM_STRUCT,
	Z_SYM_RECFUN
} ZSymType;

typedef struct ZSymbol {
	ZSymType 	kind;
	char 			*name;
	ZType 		*type;
	ZNode 		*node;
	bool 			isPublic;
} ZSymbol;

typedef struct ZScope {
	ZSymbol 			**symbols;
	struct ZScope *parent;
	u32 					depth;
} ZScope;

typedef struct ZSymTable {
	ZScope *global;
	ZScope *current;
} ZSymTable;

typedef struct ZSemantic {
	ZState 		*state;
	ZNode 		*root;
	ZSymTable *table;
	ZType 		*currentFuncRet;
	u16 			loopDepth;
} ZSemantic;

/* Lexer */
ZToken **ztokenize(ZState *);
ZToken *maketoken(ZTokenType);

/* Parser */
ZNode *zparse(ZState *, ZToken **);
ZType *maketype(ZTypeKind);

/* Semantic */
void zanalyze(ZState *, ZNode *);

bool typesEqual(ZType *, ZType *);
ZType *typesCompatible(ZState *, ZType *, ZType *);

/* ================== Zinc state ================== */
ZState *makestate(char *);

char *readfile(char *);

void error	(ZState *, ZToken *, const char *, ...);
void warning(ZState *, ZToken *, const char *, ...);
void info		(ZState *, ZToken *, const char *, ...);

void printLogs(ZState *state);

bool visit(ZState *, char *);
void undoVisit(ZState *);

char *stoken(ZToken *);
void printToken(ZToken *);
void printTokens(ZToken **);

void printType(ZType *);
void printNode(ZNode *, u8);

void printScope(ZScope *);

#endif
