#ifndef ZSEM_INTERNAL_H
#define ZSEM_INTERNAL_H

#include "base.h"
#include "zhset.h"
#include "zinc/zinc.h"
#include "zmem.h"
#include "zvec.h"
#include "zvisit.h"

extern ZNode *LangItems[Z_LANG_COUNT];

static inline bool isTypeKind(ZType *t, ZTypeKind kind) {
    return t && t->kind == kind;
}
static inline bool isUnsigned(ZToken *t) { return tokmask(t, TOK_UNSIGNED);  }
static inline bool isSigned  (ZToken *t) { return tokmask(t, TOK_SIGNED);    }
static inline bool isFloat   (ZToken *t) { return tokmask(t, TOK_FLOAT);     }
static inline bool isInteger (ZToken *t) { return isSigned(t) || isUnsigned(t);     }
static inline bool isPrimitive(ZType *t) { return isTypeKind(t, Z_TYPE_PRIMITIVE);  }
static inline bool isNumeric(ZType *t) {
    if (!t || !isPrimitive(t)) return false;
    return tokmask(t->primitive.token, (TOK_SIGNED | TOK_UNSIGNED | TOK_FLOAT));
}
static inline bool isOptionalPointer(ZType *t) {
    return  isTypeKind(t,           Z_TYPE_OPTIONAL) &&
            isTypeKind(t->optional, Z_TYPE_POINTER);
}

void analyzeStruct           (ZThreadSem *, ZNode *);
void analyzeEnum             (ZThreadSem *, ZNode *);
void analyzeTypedef          (ZThreadSem *, ZNode *);
void analyzeStmt             (ZThreadSem *, ZNode *);
void analyzeBlock            (ZThreadSem *, ZNode *, bool);
void analyzeNamespace        (ZThreadSem *, ZNode *);
void analyzeFuncArgs         (ZThreadSem *, ZType **, ZNode **, ZType **);
bool satisfyFacet            (ZThreadSem *, ZType *, ZType *);
ZType *resolveTypeRef        (ZThreadSem *, ZType *);
void checkFunctionUsedAsValue(ZThreadSem *, ZNode *);
ZFuncTable *resolveFuncTable (ZThreadSem *, ZType *);
ZType *resolveType           (ZThreadSem *, ZNode *, ZType *);
ZSymbol *resolve             (ZThreadSem *, ZToken *);
ZCastInfo typesCompatible    (ZThreadSem *, ZType *, ZType *);
ZType *resolveLiteralType    (ZThreadSem *, ZToken *, ZType *);

ZCapabilityCast *analyzeCapabilities(ZThreadSem *ctx, ZToken *start, ZType **capabilities);
void beginScope(ZThreadSem *ctx, ZNode *curr);
void endScope(ZThreadSem *ctx);
// ZThreadSem *getRegisteredModule(ZSemantic *ctx, char *module);
ZNode *getStructField(ZThreadSem *ctx, ZType *strct, ZToken *field);
ZType *implicitStructFieldCast(ZThreadSem *ctx, ZType *strct, ZType *to, u32 **path);
bool isComparable(ZThreadSem *ctx, ZType *type);
bool isInfiniteSize(ZType *type, ZType *root, ZType ***seen);
bool isVoid(ZType *t);
bool isZeroable(ZThreadSem *ctx, ZType *type);
ZNode *makeNodeThread(ZThreadSem *ctx, ZNodeType type);
ZSymbol *makeRawSymbol(Allocator *allocator, ZSymType kind, ZToken *name, ZType *type, ZNode *node, bool isPublic);
ZToken *makeTokenThread(ZThreadSem *ctx, ZTokenType type, ZToken *at);
ZType *makeTypeThread(ZThreadSem *ctx, ZTypeKind kind);
ZScope *makescope(Allocator *allocator, ZScope *parent, ZNode *node);
ZSemantic *makesemantic(ZState *state);
ZSymbol *makesymbol(Allocator *allocator, ZSymType kind);
ZThreadSem *makethreadsem(ZSemantic *ctx, ZScope *current, ZNode *root);
ZCapability *putCapability(ZThreadSem *ctx, ZNode *var);
void putEnum(ZThreadSem *ctx, ZNode *node);
void putFacet(ZThreadSem *ctx, ZNode *node);
void putFunc(ZThreadSem *ctx, ZNode *node);
void putGeneric(ZThreadSem *ctx, ZType *type);
void putImpl(ZThreadSem *ctx, ZNode *node);
void putNamespace(ZThreadSem *ctx, ZNode *node);
ZFuncTable *putOrInsertFuncTable(ZThreadSem *ctx, ZType *type);
void putRawSymbol(ZThreadSem *ctx, ZSymType kind, ZToken *name, ZType *type, ZNode *node, bool isPublic);
void putStruct(ZThreadSem *ctx, ZNode *node);
void putSymbol(ZThreadSem *ctx, ZSymbol *symbol);
void putTypedef(ZThreadSem *ctx, ZNode *node);
void putVarPattern(ZThreadSem *ctx, ZNode *node, ZType *type, ZVarDestructPattern *pattern, bool condition);
// ZThreadSem *registerModule(ZSemantic *ctx, ZNode *module);
i32 sumTypeIndexOf(ZType *sum, ZType *concrete);
bool typesEqual(ZType *a, ZType *b);
ZNode *coerceToSum(ZThreadSem *ctx, ZNode *node, ZType *sum);
ZNode *implicitCast(ZThreadSem *ctx, ZNode *node, ZType *to);
void analyzeForeign(ZThreadSem *ctx, ZNode *curr);
void analyzeFunc(ZThreadSem *ctx, ZNode *curr);
void analyzeVar(ZThreadSem *ctx, ZNode *curr, bool isGlobal);

#endif // ZSEM_INTERNAL_H
