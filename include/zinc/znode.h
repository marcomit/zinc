#ifndef ZNODE_H
#define ZNODE_H

#include "zconfig.h"
#include "zforward.h"
#include "ztok.h"
#include "ztype.h"
#include "zann.h"
#include "zmem.h"

typedef enum {
    Z_VAR_IDENT,
    Z_VAR_TUPLE,
    Z_VAR_STRUCT,
    Z_VAR_PAIR,
    Z_VAR_ENUM,
    Z_VAR_LIT,
    Z_VAR_SUM
} ZPatternKind;

typedef enum {
    Z_INTERP_LIT,
    Z_INTERP_EXPR
} ZInterpolationType;

typedef struct {
    ZInterpolationType type;
    union {
        ZToken  *literal;
        struct {
            ZNode   *expr;

            /* Reference to the implementation of the write method. */
            ZNode   *writable;
        };
    };
} ZInterpolation;


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

typedef enum {
    Z_CAST_INVALID,
    Z_CAST_NOOP,
    Z_CAST_NUMERIC,
    Z_CAST_POINTER,
    Z_CAST_NONE,
    Z_CAST_ARRAY_PTR,
    Z_CAST_ARRAY_LIT,
    Z_CAST_OPTIONAL,
    Z_CAST_SUM,
    Z_CAST_FACET,
    Z_CAST_INTERP,
    Z_CAST_FIELD
} ZCastKind;

typedef struct {
    ZType       *from;
    ZType       *to;
    ZType       *promoted;
    ZCastKind   kind;
} ZCastInfo;

struct ZCapability {
    ZType *type;
    ZNode **nodes;
};

struct ZCapabilityCast {
    ZNode *node;
    ZCastInfo cast;
    i32 index;
};

typedef enum {
#define X(name, masks) name,
#define NODE_BASE

#include "znode.def"

#undef NODE_BASE
#undef X
    NODE_TYPE_COUNT
} ZNodeType;

struct ZNode {
    ZNodeType       type;
    ZType           *resolved;
    ZToken          *tok;
    ZAnnotation     **annotations;
#if Z_LSP
    /* Source range: the first and the last token of the node (both inclusive).
     * NULL for nodes that don't come from this file's source (synthesized nodes,
     * the injected prelude). Set by setspan / setspanfrom in the parser. */
    ZToken          *start;
    ZToken          *end;

    ZToken          *comment;
#endif
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
            bool    pub;
            bool    constant;
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
                ZType       **capabilities;
            };
        };
        struct {
            ZType   *type;
            ZToken  *identifier;
            ZNode   *defaultValue;
        } field;

        union {
            struct {
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
            } foreignDecl;
        };

        struct {
            ZNode   *callee;
            ZNode   **args;

            /* It is a list of references to the capabilites' definition.
             * This list MUST be set in the semantic analysis. */
            ZCapabilityCast *capabilities;

            /* The semantic analyzer attach the reference to the called function
             */
            ZNode   *func;
        } call;

        struct {
            ZToken      *ident;
            ZNode       **fields;
            ZType       **generics;
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
            ZNode       *method; // Receiver method target (self injected)
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
        } typeDef;

        struct {
            /* Named imports. */
            ZToken *name;

            /* Full path. */
            const char  *filename;

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

        ZInterpolation      **interpolation;
        ZToken              *literalTok;
        struct {
            ZToken          *tok;
            char            *mangled;
            ZSymbol         *sym;
            ZNode           *ref;
            ZLangItemType   li;
        } identNode;

        struct {
            ZNode           *expr;
            u32             *fieldPath;
            ZCastInfo       info;
        } castExpr;

        struct {
            ZType           *type;
        } sizeofExpr;

        struct {
            ZNode   **capabilities;
            ZNode   *block;
            bool    isInline;
        } capability;

        struct {
            ZToken      *name;
            bool        pub;

            /* Array of NODE_FIELD */
            ZNode       **funcs;

            ZType       **generics;
        } facet;

        struct {
            bool        pub;
            ZType       **facets;
            ZType       **generics;
            ZType       *base;
            // ZToken      *self;
            ZNode       **funcs;
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

ZNode *makenode(Allocator *, ZNodeType);
ZNode *copynode(ZNode *);
u32 hashNode(ZNode *);

void printNode(ZNode *, u8);
void printDestructedVar(ZVarDestructPattern *, u8);

#endif // ZNODE_H
