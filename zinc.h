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
		f64 floating;
		bool boolean;
	};
	char *sourcePtr;
	char *sourceLinePtr;
	usize row;
	usize col;
	bool newlineBefore;
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
	char 		*output;
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
	NODE_FOR,
	NODE_RETURN,
	NODE_VAR_DECL,
	NODE_BINARY,
	NODE_UNARY,
	NODE_CALL,     	// Function call
	NODE_FUNC,     	// Function definition
	NODE_LITERAL,  	// Numbers, strings, etc.
	NODE_IDENTIFIER,
	NODE_STRUCT,
	NODE_SUBSCRIPT,
	NODE_MEMBER,
	NODE_MODULE,
	NODE_PROGRAM,
	NODE_UNION,
	NODE_FIELD,
	NODE_TYPEDEF,
	NODE_FOREIGN,
	NODE_DEFER,
	NODE_STRUCT_LIT,
	NODE_TUPLE_LIT,
	NODE_ARRAY_LIT,
	NODE_MACRO,
	NODE_GOTO,
	NODE_LABEL,
	NODE_TYPE
} ZNodeType;

typedef struct ZNode ZNode;
typedef struct ZType ZType;

typedef enum ZTypeKind {
	Z_TYPE_PRIMITIVE,
	Z_TYPE_STRUCT,
	Z_TYPE_ARRAY,
	Z_TYPE_FUNCTION,
	Z_TYPE_POINTER,
	Z_TYPE_TUPLE,
	Z_TYPE_GENERIC		// Instantiated generic type, e.g. List[int]
} ZTypeKind;

struct ZType {
	ZTypeKind kind;

	union {
		ZToken *tok;
		// For PRIMITIVE (e.g. void or int)
		struct {
			ZToken *token;
			ZType *base;
			ZType **generics;
		} primitive;

		// For POINTER (The type the pointer points to)
		ZType *base;

		struct {
			ZToken *name;
			ZNode **fields;
			ZType **generics;
		} strct;

		struct {
			ZType *ret;
			ZType **args;
			ZType **generics;
		} func;

		struct {
			ZType *base;
			usize size;
		} array;

		ZType **tuple;

		// For GENERIC instantiation (e.g. List[int], Map[str, int])
		struct {
			ZToken *name;		// The generic type name (e.g. "List")
			ZType **args;		// The type arguments (e.g. [int])
		} generic;
	};

	bool constant;
};

typedef enum {
	Z_MACRO_KEY, 		// Captured keyword
	Z_MACRO_EXPR, 	// Captured expression
	Z_MACRO_IDENT, 	// Captured identifier
	Z_MACRO_TYPE, 	// Captured type
	Z_MACRO_ZM, 		// Zero or more
	Z_MACRO_OM, 		// One or more
	Z_MACRO_SEQ			// Sequence
} ZMacroType;

typedef struct ZMacroPattern {
	ZMacroType kind;
	union {
		/* Used for keyword, expression and identifier*/
		ZToken *ident;
		struct ZMacroPattern *zeroOrMore;
		struct ZMacroPattern *oneOrMore;
		struct ZMacroPattern **sequence;
	};
} ZMacroPattern;

typedef struct ZMacroVar {
	ZToken *name;
	usize tokenStart;  // Start index of captured tokens
	usize tokenEnd;    // End index (exclusive)
} ZMacroVar;

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
			ZToken *ident;
			ZNode *iterator;
			ZNode *block;
		} forStmt;

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

		ZNode ** block;

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
			ZToken **generics;
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
			ZNode **fields;
		} tuplelit;

		struct {
			ZNode **fields;
		} arraylit;

		struct {
			ZToken *ident;
			ZNode **fields;
			ZType **generics;
		} structlit;

		struct {
			ZToken *alias;
			ZType *type;
		} typeDef;

		struct {
			ZToken *name;
			ZNode *root;
		} module;

		/* For macros don't parse the body.
		 * Just save where the body starts and ends.
		 * When it tries to expand a macro it parse the body recursively.
		 **/
		struct {
			ZMacroPattern *pattern;
			usize startBody;
			usize endBody;
			ZMacroVar **captured;
			ZToken *start;
			usize consumed;
			ZToken **sourceTokens;  // Original token array where the macro was defined
		} macro;

		ZToken *gotoLabel;  // For NODE_GOTO and NODE_LABEL

		ZNode **program;

		ZToken *literalTok;
		ZToken *identTok;
	};
};

typedef struct ZParser {
	ZState *state;
	ZToken **tokens;
	u64 current;

	/*
	 * Used to track temporary errors and find a valid path.
	 */
	usize *errstack;
	ZNode **macros;

	/* Setted when it parses the body of a macro. */
	ZNode *currentMacro;

	/* Stack of macros currently being expanded (for cycle detection) */
	ZNode **expandingMacros;

	u8 depth;
} ZParser;

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
bool tokeneq(ZToken *, ZToken *);

/* Parser */
ZNode *zparse(ZState *, ZToken **);
ZNode *parseExpr(ZParser *);
ZNode *parseStmt(ZParser *);
ZType *parseType(ZParser *);

bool canPeek(ZParser *);
bool check(ZParser *, ZTokenType);
bool checkMask(ZParser *, u32);
ZToken *peek(ZParser *);
ZToken *consume(ZParser *);

ZNode *getMacroVar(ZNode *, ZToken *);
ZNode *getMacroCapturedVar(ZNode *, ZToken *);
ZNode *expandMacro(ZParser *);
ZNode *copynode(ZNode *);
bool macroeq(ZNode *, ZNode *);
bool macropatterneq(ZMacroPattern *, ZMacroPattern *);

ZNode *makenode(ZNodeType);
ZType *maketype(ZTypeKind);

/* Semantic */
void zanalyze(ZState *, ZNode *);

/* Code generation */
void zcompile(ZState *, ZNode *, const char *output);

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
char *tokname(ZTokenType);
void printToken(ZToken *);
void printTokens(ZToken **);

void printType(ZType *);
void printNode(ZNode *, u8);

void printScope(ZScope *);

#endif
