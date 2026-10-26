/**
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */
#ifndef Z_GEN
#define Z_GEN

#include "base.h"
#include "zcolors.h"
#include "zinc.h"
#include "zlink.h"
#include "zvec.h"
#include "zmir.h"

#define LABEL_RESET(c) do {                                                     \
    memset(ctx->str, 0, veclen(ctx->str));                                      \
    vecsetlen(ctx->str, 0);                                                     \
} while (0)

#define label(ctx, X) _Generic((X), \
    ZToken*: labelTok,              \
    char*:   labelStr               \
)(ctx, X)



typedef struct ZMIRSymbol {
    ZToken *token;
    char *name;
    ZNode *node;
    ZMIRValueRef value;
    ZMIRTypeRef type;
    bool isValue;
} ZMIRSymbol;

typedef struct {
    /* The variable stored in the stack. */
    ZMIRValueRef    stack;

    /* the data stored in the stack.
     * It differs from the stack field only for prefixed types like arrays.
     * */
    ZMIRValueRef    elem;
    ZMIRTypeRef     stackType;
    ZMIRTypeRef     elemType;
    ZNode           *node;
} ZMIRStack;

typedef enum {
    Z_SCOPE_BLOCK,
    Z_SCOPE_LOOP,
    Z_SCOPE_FUNC,
    Z_SCOPE_FILE,
    Z_SCOPE_GLOB
} ZGenScope;

typedef enum {
    Z_PREDICATE_EQ,
    Z_PREDICATE_NE,
} ZPredicate;

typedef struct ZMIRCapability {
    ZType           *capability;
    ZMIRValueRef    ref;
} ZMIRCapability;

typedef struct ZMIRScope {
    struct ZMIRScope   *parent;

    ZMIRCapability     **capabilities;

    ZMIRSymbol         **symbols;

    /* Capture the start label of the loop (used by the continue statement). */
    ZMIRBasicBlockRef   startLoop;

    /* Capture the end label of the loop (used by the break statement). */
    ZMIRBasicBlockRef   endLoop;

    /* Capture the block's label. */
    ZMIRBasicBlockRef   breakBlock;

    /* Capture all stack allocated variables (it is allocated only at function-level). */
    ZMIRStack          **stackAlloca;

    /* Captures all defer statements of the current block. */
    ZNode               **defers;

    int                 type;
} ZMIRScope;

typedef struct  {
    ZType *type;
    ZMIRValueRef value;
} ZLLVMTypeInfo;

typedef struct {
    ZState                  *state;
    ZModuleAllocator        *module;
    ZMIRContextRef          ctx;

    ZMIRModuleRef           *modules;
    ZMIRModuleRef           mod;

    ZMIRBuilderRef          builder;

    ZMIRScope              *scope;

    /* Struct type cache - parallel arrays keyed by name */
    const char              **structNames;
    ZMIRTypeRef             *structTypes;

    ZMIRValueRef            currentFunc;
    ZNode                   *currentFuncNode;

    /* all operations are named with an incremental number
     * and converted to hex format. */
    usize                   count;

    /* buffer for operation names for storing the hex number. */
    char                    *str;

    /* Modules already forward-declared / defined into ctx->mod, keyed by
     * module name. A module can be reached through several import paths (and
     * re-imports carry an empty module.root), so these guard against emitting
     * a body twice - which would append a second definition to the same
     * function - and terminate on cyclic imports. */
    hashset_t               seenModuleDecls;
    hashset_t               seenModuleDefs;

    ZMIRTargetMachineRef    machine;
    ZLLVMTypeInfo           *typeInfos;
} ZCodegen;


void LLVMAddFuncAttribute(ZCodegen *ctx, ZMIRValueRef func, const char *llvmAttr);

ZMIRTypeRef genType             (ZCodegen *, ZType *);
ZMIRValueRef genExpr            (ZCodegen *, ZNode *);
ZMIRValueRef genLValue          (ZCodegen *, ZNode *);
ZMIRValueRef genStrLitGlobal    (ZCodegen *, ZToken *);

ZMIRBasicBlockRef makeblock(ZCodegen *, char *);
void makebr(ZMIRBuilderRef, ZMIRBasicBlockRef);
void makecondbr(ZMIRBuilderRef, ZMIRValueRef, ZMIRBasicBlockRef, ZMIRBasicBlockRef);

void labelCnt   (ZCodegen *);
char *labelTok  (ZCodegen *, ZToken *);
char *labelStr  (ZCodegen *, char *);

void emitPanic(ZCodegen *, ZToken *, ZMIRValueRef, const char *);
void emitRuntimeError(ZCodegen *, ZToken *, const char *);
void emitRuntimeDebugPrint(ZCodegen *, ZToken *, const char *);
void emitRuntimePanic(ZCodegen *, ZToken *, ZMIRValueRef);
void emitBoundCheck(ZCodegen *, ZToken *, ZMIRValueRef, ZMIRTypeRef, ZMIRValueRef);
void emitNullCheck(ZCodegen *, ZMIRValueRef, ZToken *);
void initializeMemoryToZero(ZCodegen *, ZMIRValueRef, ZMIRTypeRef);
void checkUnsafeUnwrap(ZCodegen *, ZMIRValueRef, ZType *, ZToken *);
void checkFacet(ZCodegen *, ZMIRValueRef, ZToken *);
void LLVMBuildTrap(ZCodegen *);
bool genBuiltin(ZCodegen *, ZNode *, ZMIRValueRef *out);

ZMIRValueRef loadWithNullDeref(ZCodegen *, ZMIRTypeRef, ZMIRValueRef, ZToken *);
ZMIRValueRef getFlagOptional(ZCodegen *, ZType *, ZMIRValueRef);
ZMIRValueRef genTypeType(ZCodegen *, ZNode *);

#endif //!Z_GEN
