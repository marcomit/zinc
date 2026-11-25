#ifndef ZSEM_H
#define ZSEM_H

#include <stdatomic.h>

#include "zforward.h"
#include "zstate.h"
#include "ztok.h"
#include "znode.h"
#include "ztype.h"
#include "zhset.h"

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

    usize           useCount;
    bool            isPublic;
    bool            reachable;
};

struct ZRegion {
    ZNode *origin;
    const char *name;
    int depth;
};

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
    ZFile           *file;
    ZScope          *scope;
    ZThreadSem      *ctx;
} ZScopeTable;

typedef struct {
    ZNode *declaration;
    ZType **generics;
} ZInstance;

typedef struct ZSemantic {
    ZState              *state;
    /* cached main function node. */
    ZSymbol             *main;

    vec(ZScopeTable *)  scopes;
} ZSemantic;

struct ZThreadSem {
    ZSemantic   *semantic;
    ZState      *state;
    ZScope      *current;
    ZScope      *global;
    ZScope      *local;
    ZNode       *root;
    Allocator   *allocator;
    ZType       *currentFuncRet;
    ZNode       *currentFunc;

    /* All func tables usable during this module's analysis (own funcs +
     * imported funcs), keyed by receiver/base type. Used by every lookup. */
    ZFuncTable  **funcs;

    /* Funcs this module exports: own pub funcs plus funcs pulled in via
     * `pub use`. Only discoverImport touches this. */
    ZNode       **exportedFuncs;

    u16         loopDepth;
    bool        discovered;
};

/* Semantic  -  public entry points. The per-traversal helpers (resolveType,
 * resolve, typesCompatible) now take the internal ZThreadSem and stay private
 * to zsem.c. */
void zanalyze(ZState *);

void printSymbol(ZSymbol *);
char *ssymbol(Allocator *, ZSymbol *);
void printScope(ZScope *);

#endif // ZSEM_H
