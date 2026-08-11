// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#ifndef ZINC_H
#define ZINC_H

#include "base.h"
#include "zarena.h"
#include "zvec.h"
#include "zhset.h"
#include "zmem.h"

#include <stdatomic.h>
#include <stdatomic.h>
#include <pthread.h>

static char sep = '/';

typedef enum {

#define DEF(id, str, m) id,
#define TOK_FLOWS
#define TOK_TYPES
#define TOK_DYN
#define TOK_SYMBOLS

#include "ztok.def"

#undef TOK_FLOWS
#undef TOK_TYPES
#undef TOK_DYN
#undef TOK_SYMBOLS

#undef DEF

} ZTokenType;

typedef struct ZToken {
    ZTokenType  type;
    union {
        char    *str;
        i64     integer;
        f64     floating;
        bool    boolean;
    };
    char        *filename;
    char        *sourcePtr;
    char        *sourceLinePtr;
    char        *start;
    char        *end;
    usize       row;
    usize       col;
    bool        newlineBefore;
} ZToken;

typedef struct ZNode        ZNode;
typedef struct ZType        ZType;
typedef struct ZScope       ZScope;
typedef struct ZSymbol      ZSymbol;
typedef struct ZThreadSem   ZThreadSem;
typedef struct ZAnnotation  ZAnnotation;

extern ZType *none;
extern ZType *u0Type;
extern ZType *u1Type;
extern ZType *u64Type;
extern ZType *modType;

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
    int             src_line;
} ZLog;

typedef enum {
    Z_LTO_OFF = 0,
    Z_LTO_THIN,
    Z_LTO_FULL
} ZLTOMode;

typedef enum {
    Z_EMIT_EXE,
    Z_EMIT_OBJ,
    Z_EMIT_IR,
    Z_EMIT_ASM
} ZEmitMode;

typedef enum {
    Z_LANG_NONE,

    Z_LANG_TYPE_INFO,
    Z_LANG_TYPE_INFO_STRUCT,
    Z_LANG_TYPE_INFO_STRUCT_FIELD,

    Z_LANG_REFLECT,

    Z_LANG_SOURCE_LOCATION_TYPE,
    Z_LANG_SOURCE_LOCATION_FUNC,
    Z_LANG_HERE,

    Z_LANG_COUNT
} ZLangItem;

typedef struct {
    ZNode   *module;
    arena_t *allocator;
} ZModuleAllocator;

typedef enum {
    Z_MODE_RELEASE,
    Z_MODE_DEBUG
} ZMode;

typedef struct {
    char            *output;
    ZLog            **logs;
    ZPhase          currentPhase;
    char            *currentPath;
    char            *filename;
    char            *homePath;

    char            **pathFiles;
    char            **visitedFiles;
    bool            canAdvance;

    bool            debug;

    bool            unusedVar;
    bool            unusedFunc;
    bool            unusedStruct;

    bool            skipLLVMValidation;
    bool            verbose;

    char            optimizationLevel;
    ZLTOMode        ltoMode;
    ZEmitMode       emit;
    ZMode           mode;
    ZNode           *root;

    /* Extra arguments should be passed in the linker. */
    char            **extraArgs;

    /* Allocator used for shared allocations. */
    arena_t         *globalAllocator;

    /* Each module carries its own arena allocator for thread-safety. */
    ZModuleAllocator **modules;

    /* Every module is parsed exactly once; every later import reuses the parsed
     * body from here. Shared across the per-file parser instances (each file
     * gets its own ZParser), so a re-import from any file resolves its cached
     * body regardless of which file first parsed it. */
    struct ZParserModule **cachedModules;

    /* Indicates the start time of the current phase to calculate the diagnostics. */
    struct timespec phaseTime;

    /* Save every 'here' call token such that the code generator build a
     * 'SourceLocation' struct and zinc can use the location to show diagnostics.
     * */
    ZToken **sourceLocations;

    ZNode **langItems;
    ZType **reflected;
} ZState;


/* ================== Syntax analysis    ================== */
typedef enum {
#define X(name, masks) name,
#define NODE_BASE

#include "znode.def"

#undef NODE_BASE
#undef X
    NODE_TYPE_COUNT
} ZNodeType;

typedef enum ZTypeKind {
    Z_TYPE_PRIMITIVE,
    Z_TYPE_POINTER,
    Z_TYPE_STRUCT,
    Z_TYPE_ARRAY,
    Z_TYPE_FUNCTION,
    Z_TYPE_TUPLE,
    Z_TYPE_GENERIC,        // Instantiated generic type, e.g. List[int]
    Z_TYPE_FACET,
    Z_TYPE_ENUM,
    Z_TYPE_NONE,
    Z_TYPE_NAMESPACE,
    Z_TYPE_SUM,
    Z_TYPE_OPTIONAL,
    Z_TYPE_RESULT
} ZTypeKind;

struct ZType {
    ZTypeKind   kind;
    ZToken      *tok;
    u32         hash;

    union {
        struct {
            ZType   *base;
            ZToken  *prop;
        } space;

        // For PRIMITIVE (e.g. void or int)
        struct {
            ZToken  *token;
            ZType   *base;
            ZType   **generics;
        } primitive;

        // For POINTER (The type the pointer points to)
        ZType       *base;

        struct {
            ZToken      *name;

            /* Array of NODE_FIELD */
            ZNode       **fields;
            ZType       **generics;
            ZAnnotation ** annotations;
        } strct;

        struct {
            ZType   *ret;
            ZType   **args;
            ZType   **generics;
            ZType   **capabilities;
            bool    variadic;
        } func;

        struct {
            ZType   *base;
            usize   size;
            bool    dynamic;
        } array;

        struct {
            ZToken  *name;

            /* List of NODE_FIELD */
            ZNode   **funcs;

            /* List of satisfied types.
             * Types are added in this list during the semantic pass
             * and only when a type tries to convert into a facet
             * */
            ZType   **satisfied;
        } facet;

        struct {
            ZToken      *name;

            /* Array of Z_TYPE_STRUCT. */
            ZNode       **fields;
            ZType       **generics;

            ZToken      *integer;
        } enm;

        ZType **tuple;

        struct {
            ZToken  *name;

            /* A generic can extend a facets:
             * T: Display + Drop
             * */
            ZType   **extensions;

            ZType   **instantiations;
        } generic;

        ZType   **sumType;

        ZType   *optional;

        struct {
            ZType *success;
            ZType *error;
        } result;
    };

    /* Future implementation:
     * Constant values for now are not checked in the semantic analyzer.
     * So you can assign a value to a constant variable. */
    bool constant;
};

typedef enum {
    Z_MACRO_KEY,        // Captured keyword
    Z_MACRO_EXPR,       // Captured expression
    Z_MACRO_IDENT,      // Captured identifier
    Z_MACRO_TYPE,       // Captured type
    Z_MACRO_ZM,         // Zero or more
    Z_MACRO_OM,         // One or more
    Z_MACRO_SEQ         // Sequence
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
    usize useCount;     // Count how many times the variable is used in the body
} ZMacroVar;

enum {
    Z_VAR_IDENT,
    Z_VAR_TUPLE,
    Z_VAR_STRUCT,
    Z_VAR_PAIR,
    Z_VAR_ENUM,
    Z_VAR_LIT,
    Z_VAR_SUM
};

/* Struct representing the destructuring of a variable.
 * A variable can be destructured in 2 ways:
 * 1. A tuple: (first, second, ...) := expression.
 * 2. A struct: {field1, field2} := expression
 * and the base case is the identifier.
 * */
typedef struct ZVarDestructPattern ZVarDestructPattern;
struct ZVarDestructPattern {
    int type;
    /* The start token of the pattern. */
    ZToken *tok;

    ZType *resolved;
    union {
        ZToken *ident;
        ZVarDestructPattern **tuple;

        struct {
            ZToken              *base;
            ZToken              *prop;
            ZVarDestructPattern **args;
        };

        /* Array of Z_VAR_PAIR */
        ZVarDestructPattern **fields;

        /* Z_VAR_PAIR */
        struct {
            ZToken *key;
            ZVarDestructPattern *value;
        };

        struct {
            ZType *type;
            ZVarDestructPattern *child;
        } sum;
    };
};

typedef enum ZAnnotationKind {
    Z_ANN_IDENT     = 1 << 0,
    Z_ANN_LIT       = 1 << 1,
    Z_ANN_NESTED    = 1 << 2,
    Z_ANN_ASSIGN    = 1 << 3
} ZAnnotationKind;

struct ZAnnotation {
    ZAnnotationKind     kind;
    ZToken              *tok;
    union {
        ZToken          *ident;
        ZToken          *literal;
        ZAnnotation     **nested;
        struct {
            ZToken      *name;
            ZAnnotation *value;
        } assign;
    };
    bool                used;
};

struct ZNode {
    ZNodeType       type;
    ZType           *resolved;
    ZToken          *tok;
    union {
        // Can be used for both if and ternary operator
        struct {
            ZNode   *cond;
            ZNode   *body;
            ZNode   *elseBranch;
        } ifStmt;

        struct {
            ZNode   *cond;
            ZNode   *branch;
        } whileStmt;

        struct {
            ZNode   *var;
            ZNode   *cond;
            ZNode   *incr;
            ZNode   *block;
        } forStmt;

        struct {
            ZVarDestructPattern *binding;
            ZNode               *iter;
            ZNode               *body;

            /* Cached function definition of the 'next' function.
             * */
            ZNode               *iterNextRef;
        } forin;

        struct {
            ZVarDestructPattern *pattern;
            ZNode   *expr;
        } matchArm;

        struct {
            ZNode   *cond;
            ZNode   **arms;
        } match;

        struct {
            ZToken  *op;
            ZNode   *left;
            ZNode   *right;
            ZNode   *overload;
        } binary;

        struct {
            ZToken  *operat;
            ZNode   *operand;
        } unary;

        struct {
            ZVarDestructPattern *pattern;
            // ZNode   *ident; // It is a NODE_IDENTIFIER
            ZNode   *rvalue; // Null if not initialized (zero-initialized unless 'uninit')
            bool    uninit;  // 'x: T = ?' - explicitly left uninitialized, no zero-init
        } varDecl;

        struct {
            /* Scope is assigned in the semantic analyzer.
             * Used for every type of block statement (e.g. if/for/functions...)
             * The scope is used to lookup symbols during the code generation.
             * */
            ZScope  *scope;
            /* The list of statements. */
            struct {
                bool        pub;
                ZNode       **block;
                ZAnnotation **annotations;
                ZType       **capabilities;
            };
        };
        struct {
            ZType   *type;
            ZToken  *identifier;
        } field;

        union {
            struct {
                ZType   *ret;
                ZToken  *name;
                char    *mangled;

                /* Always parsed as Z_TYPE_PRIMITIVE. */
                ZType   *base;

                ZNode   **args;

                ZNode   *body;

                /* NODE_FIELD */
                ZNode   *receiver;

                ZType   **generics;

                ZAnnotation **annotations;
                ZNode   **capabilities;

                bool    pub;
            } funcDef;

            struct {
                ZToken      *name;
                bool        pub;
                ZAnnotation **annotations;
            } foreignDecl;
        };

        struct {
            ZNode   *callee;
            ZNode   **args;

            /* It is a list of references to the capabilites' definition.
             * This list MUST be set in the semantic analysis. */
            ZNode   **capabilities;

            /* The semantic analyzer attach the reference to the called function
             */
            ZNode   *func;
        } call;

        struct {
            ZToken      *ident;
            ZNode       **fields;
            ZType       **generics;
            ZAnnotation **annotations;
            bool        pub;
        } structDef;

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
            ZToken      *name;
            /* Fields are a list of enumField. */
            ZNode       **fields;
            ZAnnotation **annotations;
            bool        pub;
        } enumDef;

        /* Representation of an enum's field.
         * It stores the name of the field (e.g. Square or Circle)
         * and its captured types.
         * */
         struct {
             ZToken *name;
         } enumField;

        struct {
            ZNode       *object;
            ZToken      *field;
            char        *mangled;
            u32         *path;
            ZNode       *func;
            ZSymbol     *sym;
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

        struct {
            ZNode       *start;
            ZNode       *end;
            ZNode       *base;
        } slice;

        ZNode           **tuplelit;

        ZNode           **arraylit;

        ZType           *arrayinit;

        struct {
            ZToken      **chain;
            ZNode       **fields;
            ZType       **generics;
        } structlit;

        struct {
            ZToken          *alias;
            ZType           *type;
            bool            pub;
            ZAnnotation     **annotations;
        } typeDef;

        struct {
            /* Named imports. */
            ZToken *name;

            /* Full path. */
            char        *filename;

            /* List of file-level declarations.
             * It is NULL if the module is already parsed
             * */
            ZNode       **root;

            /* List shared with every module that imports this module.
             * It'll be never NULL.
             * */
            ZNode       **cached;

            bool        pub;
        } module;

        /* For macros don't parse the body.
         * Just save where the body starts and ends.
         * When it tries to expand a macro it parse the body recursively.
         **/
        struct {
            ZMacroPattern   *pattern;
            usize           startBody;      // Index of first token after {
            usize           endBody;        // Index of } (exclusive)
            ZMacroVar       **captured;
            ZToken          *start;         // First token of macro definition
            usize           consumed;       // Tokens consumed by pattern + body
            ZToken          **sourceTokens; // Original token array where the macro was defined
            bool            pub;
        } macro;

        ZToken              *literalTok;
        struct {
            ZToken          *tok;
            char            *mangled;
            ZSymbol         *sym;
            ZNode           *ref;
        } identNode;

        struct {
            ZType           *toType;
            ZNode           *expr;
        } castExpr;

        struct {
            ZType           *type;
        } sizeofExpr;

        struct {
            ZNode **capabilities;
            ZNode *block;
        } capability;

        struct {
            ZToken      *name;
            bool        pub;

            /* Array of NODE_FIELD */
            ZNode       **funcs;

            ZAnnotation **annotations;
            ZType       **generics;
        } facet;

        struct {
            bool        pub;
            ZType       **facets;
            ZType       **generics;
            ZType       *base;
            ZToken      *self;
            ZNode       **funcs;
            ZAnnotation **annotations;
            ZNode       **capabilities;
        } impl;

        struct {
            ZNode *expr;
        } breakStmt;

        struct {
            enum {
                UNWRAP_DO,
                UNWRAP_RETURN,
                UNWRAP_BREAK,
                UNWRAP_CONTINUE
            } kind;
            ZNode *base;
            ZNode *orExpr;
        } unwrap;
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

    /* Set when it parses the body of a macro. */
    ZNode             *currentMacro;


    /* Stack of macros currently being expanded (for cycle detection) */
    ZNode             **expandingMacros;

    /* Current pattern list. */
    ZMacroPattern    *currentList;

    /* Used for list expansion. */
    usize             currentIndex;
} ZMacroParser;

typedef struct ZParserModule {
    char    *name;
    ZNode   **node;
} ZParserModule;

typedef struct ZParser {
    ZState          *state;
    ZTokenStream    *source;
    usize           tokenIndex;

    /* Used to track temporary errors and find a valid path. */
    usize           *errstack;

    ZMacroParser    macroParser;

    u8              depth;

    /* When true, parseExpr will not attempt to parse a struct literal.
     * Set by condition-parsing sites (if, for, while) to prevent `ident {`
     * from being mistaken for a struct literal instead of a block. */
    bool            noStructLit;

    /* When true, parseStmt will not attempt to parse a return statement.
     * Set by parseDefer so that `defer return ...` will be rejected by the parser.
     * */
    bool            noReturnStmt;


    /* Inside a struct or enum declaration it's very useful creating
     * anonymous structs. But anonymous structs can be parsed only inside a struct/enum
     * declaration and not everywhere (e.g. inside a function or as a return type). */
    bool            declAsType;
} ZParser;

/* ================== Semantic analysis    ================== */

typedef enum {
    Z_SYM_VAR       = 1 << 0,
    Z_SYM_FUNC      = 1 << 1,
    Z_SYM_ENUM      = 1 << 2,
    Z_SYM_STRUCT    = 1 << 3,
    Z_SYM_TYPEDEF   = 1 << 4,
    Z_SYM_GENERIC   = 1 << 5,
    Z_SYM_FOREIGN   = 1 << 6,
    Z_SYM_FACET     = 1 << 7,
    Z_SYM_IMPORT    = 1 << 8,
} ZSymType;

struct ZSymbol {
    ZSymType        kind;
    ZToken          *name;
    ZType           *type;
    ZNode           *node;
    ZScope          *scope;

    /* All types that resovled a generic.
     * Example:
     * Hashmap[K, V]
     * Every time in the code appear a Hashmap called with two generics
     * it saves these two type in the array.
     * So it is the list of all types that must be generated
     * */
    ZType           ***generics;

    usize           useCount;
    bool            isPublic;
    bool            reachable;
};

typedef struct ZCapability {
    ZType *type;
    ZNode **nodes;
} ZCapability;

struct ZScope {
    ZSymbol         **symbols;
    ZScope          *parent;
    ZNode           *node;
    ZCapability     **capabilities;
    u32             depth;
    hashset_t       seen;
};

/* Contains a type with a list of functions that accept
 * that type as a receiver. */
typedef struct ZFuncTable {
    /* The receiver type, could be every possible type (e.g. u8 or *MyStruct) */
    ZType           *base;

    /* A list of functions that have [receiver] as a receiver type */
    ZNode           **funcDef;
    hashset_t       seenReceiverFuncs;

    /* A list of static functions for that type.
     * A static function is available only when the type is an identifier.
     */
    ZNode           **staticFuncDef;
    hashset_t       seenStaticFuncs;

    ZType           **facets;
} ZFuncTable;

typedef struct ZScopeTable {
    ZNode           *module;
    ZScope          *scope;
    ZThreadSem      *ctx;
} ZScopeTable;

typedef struct ZSemantic {
    ZState          *state;

    /* Root node. */
    ZNode           *root;

    /* cached main function node. */
    ZSymbol         *main;
    ZScopeTable     **scopes;
    // ZSymTable       *table;
    ZThreadSem      **semantics;

    /* Atomic integer used to fetch the current module in the thread worker.
     * The thread worker fetch the index of the module to analyze and increment it.
     * */
    atomic_int      currentModule;
} ZSemantic;

struct ZThreadSem {
    ZSemantic   *semantic;
    ZState      *state;
    ZScope      *current;
    ZScope      *global;
    ZScope      *local;
    ZNode       *root;
    arena_t     *arena;
    ZType       *currentFuncRet;
    ZNode       *currentFunc;

    /* All func tables usable during this module's analysis (own funcs +
     * imported funcs), keyed by receiver/base type. Used by every lookup. */
    ZFuncTable  **funcs;

    /* Funcs this module exports: own pub funcs plus funcs pulled in via
     * `pub use`. Only discoverImport touches this. */
    ZNode       **exportedFuncs;

    u16         loopDepth;
};

#define hash(x) _Generic(x,                                                     \
    ZToken*: hashtoken,                                                         \
    ZNode* : hashNode,                                                          \
    ZType* : hashType                                                           \
)(x)

/* Lexer */
ZToken **ztokenize(ZState *);
ZToken *maketoken(ZTokenType, char *, char *);
ZToken *makeident(char *, char *, char *);
ZTokenStream *maketokstream(ZToken **, ZTokenStream *);
bool tokeneq(ZToken *, ZToken *);
bool tokmask(ZToken *, u16);

u32 hashStr(const char *, usize);
u32 hashNode(ZNode *);
u32 hashType(ZType *);
u32 hashtoken(ZToken *);

ZNode *convertHeaderToZNode(ZParser *, ZToken *);

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
ZType *makePrimitiveType(ZTokenType);

/* Semantic  -  public entry points. The per-traversal helpers (resolveType,
 * resolve, typesCompatible) now take the internal ZThreadSem and stay private
 * to zsem.c. */
ZSemantic *zanalyze(ZState *, ZNode *);

/* Code generation */
void zcompile(ZState *, ZNode *, const char *);

usize typeSize(ZType *);
void typesSort(ZType **);
bool isVoid(ZType *);
bool typesEqual(ZType *, ZType *);
bool typesPrimitive(ZType *);

/* ================== Zinc state ================== */
ZState *makestate();

char *readfile(char *);

void encodeType(ZType *, char **);
char *mangler(char *[]);
char *manglerA(char *[]);
char *manglerM(ZType *, ZToken *);

void _error  (ZState *, ZToken *, const char *, int, const char *, ...);
void _warning(ZState *, ZToken *, const char *, int, const char *, ...);
void _info   (ZState *, ZToken *, const char *, int, const char *, ...);
void _debug  (ZState *, ZToken *, const char *, int, const char *, ...);

#define error(state, tok, ...)   _error  (state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define warning(state, tok, ...) _warning(state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define info(state, tok, ...)    _info   (state, tok, __FILE__, __LINE__, __VA_ARGS__)
#define debug(state, tok, ...)   _debug  (state, tok, __FILE__, __LINE__, __VA_ARGS__)

void printLogs(ZState *);
void initPrimitiveTypes();
bool canAdvance(ZState *);

bool visit(ZState *, char **, bool);
void undoVisit(ZState *);

char *stoken(ZToken *);
char *stype(ZType *);
char *tokname(ZTokenType);
void printToken(ZToken *);
void printTokens(ZToken **);

void printNode(ZNode *, u8);
void printDestructedVar(ZVarDestructPattern *, u8);
void printSymbol(ZSymbol *);

void printScope(ZScope *);

i32 sumTypeIndexOf(ZType *sum, ZType *concrete);

/* ==================== ANNOTATIONS ====================== */

typedef struct ZAnnotationSpec ZAnnotationSpec;
typedef struct ZAnnotationQuery ZAnnotationQuery;

typedef enum {
    Z_TRG_ANY       = 1 << 0,
    Z_TRG_FUNC      = 1 << 1,
    Z_TRG_STRUCT    = 1 << 2,
    Z_TRG_ENUM      = 1 << 3,
    Z_TRG_VAR       = 1 << 4,
    Z_TRG_FOREIGN   = 1 << 5,
    Z_TRG_IMPL      = 1 << 6
} ZAnnotationTarget;

ZAnnotation *query(ZAnnotation **, const char *);
ZAnnotation *queryFrom(ZAnnotation **, const char *, usize *);
ZAnnotation *queryArg(ZAnnotation *, const char *);
ZAnnotation *annArg(ZAnnotation *, usize);
usize annLen(ZAnnotation *);

void analyzeAnnotations(ZState *state, ZNode *node);

#endif
