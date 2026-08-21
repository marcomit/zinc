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

#include <llvm-c/Core.h>
#include <llvm-c/Target.h>
#include <llvm-c/TargetMachine.h>
#include <llvm-c/Analysis.h>
#include <llvm-c/BitWriter.h>
#include <llvm-c/Error.h>
#include <llvm-c/Linker.h>
#include <llvm-c/IRReader.h>
#include <llvm-c/BitReader.h>
#include <llvm-c/Transforms/PassBuilder.h>
#include <stdio.h>
#include <unistd.h>

#define LABEL_RESET(c) do {                                                     \
    memset(ctx->str, 0, veclen(ctx->str));                                      \
    vecsetlen(ctx->str, 0);                                                     \
} while (0)

#define label(ctx, X) _Generic((X), \
    ZToken*: labelTok,              \
    char*:   labelStr               \
)(ctx, X)



typedef struct ZLLVMSymbol {
    ZToken *token;
    char *name;
    ZNode *node;
    LLVMValueRef value;
    LLVMTypeRef type;
    bool isValue;
} ZLLVMSymbol;

typedef struct {
    /* The variable stored in the stack. */
    LLVMValueRef    stack;

    /* the data stored in the stack.
     * It differs from the stack field only for prefixed types like arrays.
     * */
    LLVMValueRef    elem;
    LLVMTypeRef     stackType;
    LLVMTypeRef     elemType;
    ZNode           *node;
} ZLLVMStack;

enum {
    Z_SCOPE_BLOCK,
    Z_SCOPE_LOOP,
    Z_SCOPE_FUNC,
    Z_SCOPE_FILE,
    Z_SCOPE_GLOB
};

typedef struct ZLLVMCapability {
    ZType           *capability;
    LLVMValueRef    ref;
} ZLLVMCapability;

typedef struct ZLLVMScope {
    struct ZLLVMScope   *parent;

    ZLLVMCapability     **capabilities;

    ZLLVMSymbol         **symbols;

    /* Capture the start label of the loop (used by the continue statement). */
    LLVMBasicBlockRef   startLoop;

    /* Capture the end label of the loop (used by the break statement). */
    LLVMBasicBlockRef   endLoop;

    /* Capture the block's label. */
    LLVMBasicBlockRef   breakBlock;

    /* Capture all stack allocated variables (it is allocated only at function-level). */
    ZLLVMStack          **stackAlloca;

    /* Captures all defer statements of the current block. */
    ZNode               **defers;

    int                 type;
} ZLLVMScope;

typedef struct {
    ZState                  *state;
    ZModuleAllocator        *module;
    LLVMContextRef          ctx;

    LLVMModuleRef           *modules;
    LLVMModuleRef           mod;

    LLVMBuilderRef          builder;

    ZLLVMScope              *scope;

    /* Struct type cache - parallel arrays keyed by name */
    const char              **structNames;
    LLVMTypeRef             *structTypes;

    LLVMValueRef            currentFunc;
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

    LLVMTargetMachineRef    machine;
} ZCodegen;

void LLVMAddFuncAttribute(ZCodegen *ctx, LLVMValueRef func, const char *llvmAttr);

LLVMTypeRef genType(ZCodegen *, ZType *);
LLVMValueRef genExpr(ZCodegen *, ZNode *);
LLVMValueRef genLValue(ZCodegen *, ZNode *);

LLVMBasicBlockRef makeblock(ZCodegen *, char *);
void makebr(LLVMBuilderRef, LLVMBasicBlockRef);
void makecondbr(LLVMBuilderRef, LLVMValueRef, LLVMBasicBlockRef, LLVMBasicBlockRef);

void labelCnt   (ZCodegen *);
char *labelTok  (ZCodegen *, ZToken *);
char *labelStr  (ZCodegen *, char *);

void emitRuntimeError(ZCodegen *, ZToken *, const char *);
void emitRuntimeDebugPrint(ZCodegen *, ZToken *, const char *);
void emitBoundCheck(ZCodegen *, ZToken *, LLVMValueRef, LLVMTypeRef, LLVMValueRef);
void initializeMemoryToZero(ZCodegen *, LLVMValueRef, ZType *);
void checkUnsafeUnwrap(ZCodegen *, LLVMValueRef, ZType *, ZToken *);
void LLVMBuildTrap(ZCodegen *);
bool genBuiltin(ZCodegen *, ZNode *, LLVMValueRef *out);

LLVMValueRef getFlagOptional(ZCodegen *, ZType *, LLVMValueRef);

#endif //!Z_GEN
