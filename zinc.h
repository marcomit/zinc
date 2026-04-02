#ifndef ZINC_H
#define ZINC_H

#include "base.h"
#include "zvec.h"
#include "zhset.h"
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

typedef struct ZToken {
    ZTokenType type;
    union {
        char *str;
        i64 integer;
        f64 floating;
        bool boolean;
    };
    char *sourcePtr;
    char *sourceLinePtr;
    char *start;
    usize row;
    usize col;
    bool newlineBefore;
} ZToken;

typedef enum {
    Z_ERROR,
    Z_WARNING,
    Z_INFO,
    Z_DEBUG
} ZLogLevel;

typedef enum {
    Z_PHASE_LEXICAL,
    Z_PHASE_SYNTAX,
    Z_PHASE_SEMANTIC,
    Z_PHASE_GENERATE
} ZPhase;

typedef struct {
    char            *filename;
    char            *message;
    ZToken          *token;
    ZLogLevel       level;
    ZPhase          phase;
    const char      *src_file;
    int              src_line;
} ZLog;

typedef struct {
    char        *output;
    ZLog        **errors;
    ZPhase      currentPhase;
    char        *currentPath;
    char        *filename;

    char        **pathFiles;
    char        **visitedFiles;

    bool        debug;

    bool        unusedVar;
    bool        unusedFunc;
    bool        unusedStruct;

    bool        emitLLVM;   /* --emit-llvm: write .ll IR file instead of native binary */

    /* Not yet implemented */
    bool        verbose;
    u8          optimizationLevel;
} ZState;

// FIXME: use these masks in the enum
#define NODE_STMT_MASK (1 << 0x08)
#define NODE_EXPR_MASK (1 << 0x09)
#define NODE_DATA_MASK (1 << 0x0A)
#define NODE_DECL_MASK (1 << 0x0B)

/* ================== Syntax analysis    ================== */
typedef enum {
    NODE_BLOCK,         // All inside a {} is a block. A list of statement
    NODE_IF,
    NODE_WHILE,
    NODE_FOR,
    NODE_RETURN,
    NODE_VAR_DECL,
    NODE_BINARY,
    NODE_UNARY,
    NODE_CALL,         // Function call
    NODE_FUNC,         // Function definition
    NODE_LITERAL,      // Numbers, strings, etc.
    NODE_IDENTIFIER,
    NODE_STRUCT,
    NODE_SUBSCRIPT,
    NODE_MEMBER,
    NODE_MODULE,
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
    NODE_TYPE,
    NODE_ENUM,
    NODE_BREAK,
    NODE_CONTINUE,
    NODE_ENUM_FIELD,
    NODE_CAST,
    NODE_SIZEOF
} ZNodeType;

typedef struct ZNode ZNode;
typedef struct ZType ZType;
typedef struct ZScope ZScope;

typedef enum ZTypeKind {
    Z_TYPE_PRIMITIVE,
    Z_TYPE_POINTER,

    Z_TYPE_STRUCT,
    Z_TYPE_ARRAY,
    Z_TYPE_FUNCTION,
    Z_TYPE_TUPLE,
    Z_TYPE_GENERIC,        // Instantiated generic type, e.g. List[int]
    Z_TYPE_NONE
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

            /* Array of NODE_FIELD */
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
            ZToken *name;        // The generic type name (e.g. "List")
            ZType **args;        // The type arguments (e.g. [int])
        } generic;
    };

    /* Future implementation:
     * Contant values for now are not checked in the semantic analyzer.
     * So you can assign a value to a constant variable. */
    bool constant;
};

typedef enum {
    Z_MACRO_KEY,         // Captured keyword
    Z_MACRO_EXPR,     // Captured expression
    Z_MACRO_IDENT,     // Captured identifier
    Z_MACRO_TYPE,     // Captured type
    Z_MACRO_ZM,         // Zero or more
    Z_MACRO_OM,         // One or more
    Z_MACRO_SEQ            // Sequence
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
    usize startIndex;   // Start index into source token array
    usize endIndex;     // End index (exclusive)
    ZNode *captured;    // The parsed AST node for this captured variable
    usize useCount;            // Count how many timee the variable is used in the body
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
            ZNode *var;
            ZNode *cond;
            ZNode *incr;
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
            ZNode *ident; // It is a NODE_IDENTIFIER
            ZNode *rvalue; // Null if not initialized
        } varDecl;

        struct {
            /* Scope is assigned in the semantic analyzer.
             * Used for every type of block statement (e.g. if/for/functions...)
             * The scope is used to lookup symbols during the code generation.
             * */
            ZScope *scope;
            /* The list of statements. */
            ZNode **block;
        };
        struct {
            ZType *type;
            ZToken *identifier;
        } field;

        struct {
            ZType *ret;
            ZToken *name;

            /* Always parsed as Z_TYPE_PRIMITIVE. */
            ZType *base;

            ZNode **args;

            ZNode *body;

            /* NODE_FIELD */
            ZNode *receiver;

            ZToken **generics;
            bool pub;
        } funcDef;

        struct {
            ZType     *ret;
            ZToken     *tok;
            ZType     **args;
        } foreignFunc;

        struct {
            ZNode     *callee;
            ZNode     ** args;
        } call;

        struct {
            ZToken     *ident;
            ZNode     **fields;
            ZToken     **generics;
            bool pub;
        } structDef;

        struct {
            ZToken     *ident;
            ZNode   **fields;
            bool    pub;
        } unionDef;

        /* Enums are the combination of a union with an integer
         * that indicates which field is 'active'.
         * enum Shape {
         *     Square(f32),
         *     Rectangle(f32, f32),
         *     Circle(f32)
         * }
         * */
        struct {
            /* The name of the enum. */
            ZToken  *name;

            /* Fields are a list of enumField. */
            ZNode   **fields;

            bool    pub;
        } enumDef;

        /* Representation of an enum's field.
         * It stores the name of the field (e.g. Square or Circle)
         * and its captured types.
         * */
        struct {
            ZToken      *name;
            ZType       **captured;
        } enumField;

        struct {
            ZNode       *object;
            ZToken      *field;
        } memberAccess;

        struct {
            ZNode       *expr; // Can be NULL for void returns
        } returnStmt;

        struct {
            ZNode       *expr;
        } deferStmt;

        struct {
            ZNode       *arr;
            ZNode       *index;
        } subscript;

        ZNode           **tuplelit;

        ZNode           **arraylit;

        struct {
            ZToken      *ident;
            ZNode       **fields;
            ZType       **generics;
        } structlit;

        struct {
            ZToken      *alias;
            ZType       *type;
            bool        pub;
        } typeDef;

        struct {
            char        *name;
            ZNode       **root;

            /* Initialized in the semantic analyzer with all top-level symbols. */
            ZScope      *scope;
        } module;

        /* For macros don't parse the body.
         * Just save where the body starts and ends.
         * When it tries to expand a macro it parse the body recursively.
         **/
        struct {
            ZMacroPattern   *pattern;
            usize           startBody;        // Index of first token after {
            usize           endBody;          // Index of } (exclusive)
            ZMacroVar       **captured;
            ZToken          *start;          // First token of macro definition
            usize           consumed;         // Tokens consumed by pattern + body
            ZToken          **sourceTokens;  // Original token array where the macro was defined
            bool            pub;
        } macro;

        ZToken              *gotoLabel;  // For NODE_GOTO and NODE_LABEL

        ZToken              *literalTok;
        ZToken              *identTok;

        struct {
            ZType           *toType;
            ZNode           *expr;
        } castExpr;

        struct {
            ZType           *type;
        } sizeofExpr;
    };
};

typedef struct ZTokenStream {
    ZToken **list;
    usize current;
    usize end; // Cached vector length
    struct ZTokenStream *prev;
} ZTokenStream;

typedef struct ZMacroParser {
    ZNode             **macros;

    /* Setted when it parses the body of a macro. */
    ZNode             *currentMacro;


    /* Stack of macros currently being expanded (for cycle detection) */
    ZNode             **expandingMacros;

    /* Current pattern list. */
    ZMacroPattern    *currentList;

    /* Used for list expansion. */
    usize             currentIndex;
} ZMacroParser;

typedef struct ZParser {
    ZState          *state;
    ZTokenStream    *source;
    usize           tokenIndex;

    /* Used to track temporary errors and find a valid path. */
    usize           *errstack;

    ZMacroParser    macroParser;

    u8              depth;
} ZParser;

/* ================== Semantic analysis    ================== */
typedef enum {
    Z_SYM_VAR,
    Z_SYM_FUNC,
    Z_SYM_STRUCT,
    Z_SYM_TYPEDEF
} ZSymType;

typedef struct ZSymbol {
    ZSymType        kind;
    ZToken          *name;
    ZType           *type;
    ZNode           *node;
    usize           useCount;
    bool            isPublic;
} ZSymbol;

typedef struct ZScope {
    ZSymbol         **symbols;
    struct ZScope   *parent;
    ZNode           *node;
    u32             depth;
    hashset_t       seen;
} ZScope;

/* Contains a type with a list of functions that accept
 * that type as a receiver. */
typedef struct ZFuncTable {
    /* The receiver type, could be every possible type (e.g. u8 or *MyStruct) */
    ZType           *receiver;

    /* A list of functions that have [receiver] as a receiver type */
    ZNode           **funcDef;

    /* A list of static functions for that type.
     * A static function is available only when the type is an identifier.
     */
    ZNode           **staticFuncDef;
} ZFuncTable;

typedef struct ZSymTable {
    /* Global scope used to store globam symbols. */
    ZScope         *global;

    ZScope         *current;

    /* Used to track the current module. */
    ZScope         *module;

    /* Imagine this like an hashmap where:
     * the key is the type 
     * the value is a list of receiver functions for that type. */
    ZFuncTable     **funcs;
} ZSymTable;

typedef struct ZScopeTable {
    ZNode           *module;
    ZScope          *scope;
} ZScopeTable;

typedef struct ZSemantic {
    ZState          *state;
    ZNode           *root;
    ZSymTable       *table;
    ZScopeTable     **scopes;
    ZType           *currentFuncRet;
    ZNode           *currentFunc;
    u16             loopDepth;

    /* Set of seen symbols (by name) */
    hashset_t       seen;    
} ZSemantic;

/* Lexer */
ZToken **ztokenize(ZState *);
ZToken *maketoken(ZTokenType, char *);
ZTokenStream *maketokstream(ZToken **, ZTokenStream *);
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
ZType *resolveType(ZSemantic *, ZNode *);
ZSymbol *resolve(ZSemantic *, ZToken *);
void zanalyze(ZState *, ZNode *);

/* Code generation */
void zcompile(ZState *, ZNode *, const char *output);

bool typesEqual(ZType *, ZType *);
ZType *typesCompatible(ZState *, ZType *, ZType *);

/* ================== Zinc state ================== */
ZState *makestate(char *);

char *readfile(char *);

void _error  (ZState *, ZToken *, const char *, int, const char *, ...);
void _warning(ZState *, ZToken *, const char *, int, const char *, ...);
void _info   (ZState *, ZToken *, const char *, int, const char *, ...);
void _debug  (ZState *, ZToken *, const char *, int, const char *, ...);

#define error(state, tok, ...)   _error  (state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define warning(state, tok, ...) _warning(state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define info(state, tok, ...)    _info   (state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define debug(state, tok, ...)   _debug  (state, tok, __FILE__, __LINE__, __VA_ARGS__)

void printLogs(ZState *state);

bool visit(ZState *, char *);
void undoVisit(ZState *);

char *stoken(ZToken *);
void stype(ZType *, char **);
char *tokname(ZTokenType);
void printToken(ZToken *);
void printTokens(ZToken **);

void printType(ZType *);
void printNode(ZNode *, u8);
void printSymbol(ZSymbol *);

void printScope(ZScope *);

#endif
