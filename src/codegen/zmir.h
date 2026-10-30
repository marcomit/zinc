#ifndef ZMIR_H
#define ZMIR_H

#include "zinc.h"

typedef struct ZMIRType         ZMIRType;
typedef struct ZMIRValue        ZMIRValue;
typedef struct ZMIRModule       ZMIRModule;
typedef struct ZMIRBuilder      ZMIRBuilder;
typedef struct ZMIRContext      ZMIRContext;
typedef struct ZMIRBasicBlock   ZMIRBasicBlock;

typedef ZMIRValue       *ZMIRValueRef;
typedef ZMIRType        *ZMIRTypeRef;
typedef ZMIRContext     *ZMIRContextRef;
typedef ZMIRModule      *ZMIRModuleRef;
typedef ZMIRBuilder     *ZMIRBuilderRef;
typedef ZMIRBasicBlock  *ZMIRBasicBlockRef;
typedef void            *ZMIRTargetMachineRef;
typedef void            *ZMIRTargetDataRef;

typedef enum {
  ZMIRVoidTypeKind = 0,     /**< type with no size */
  ZMIRHalfTypeKind = 1,     /**< 16 bit floating point type */
  ZMIRFloatTypeKind = 2,    /**< 32 bit floating point type */
  ZMIRDoubleTypeKind = 3,   /**< 64 bit floating point type */
  ZMIRX86_FP80TypeKind = 4, /**< 80 bit floating point type (X87) */
  ZMIRFP128TypeKind = 5, /**< 128 bit floating point type (112-bit mantissa)*/
  ZMIRPPC_FP128TypeKind = 6, /**< 128 bit floating point type (two 64-bits) */
  ZMIRLabelTypeKind = 7,     /**< Labels */
  ZMIRIntegerTypeKind = 8,   /**< Arbitrary bit width integers */
  ZMIRFunctionTypeKind = 9,  /**< Functions */
  ZMIRStructTypeKind = 10,   /**< Structures */
  ZMIRArrayTypeKind = 11,    /**< Arrays */
  ZMIRPointerTypeKind = 12,  /**< Pointers */
  ZMIRVectorTypeKind = 13,   /**< Fixed width SIMD vector type */
  ZMIRMetadataTypeKind = 14, /**< Metadata */
                                    /* 15 previously used by LLVMX86_MMXTypeKind */
  ZMIRTokenTypeKind = 16,           /**< Tokens */
  ZMIRScalableVectorTypeKind = 17, /**< Scalable SIMD vector type */
  ZMIRBFloatTypeKind = 18,         /**< 16 bit brain floating point type */
  ZMIRX86_AMXTypeKind = 19,        /**< X86 AMX */
  ZMIRTargetExtTypeKind = 20,      /**< Target extension type */
} ZMIRTypeKind;

typedef enum {
  ZMIRArgumentValueKind,
  ZMIRBasicBlockValueKind,
  ZMIRMemoryUseValueKind,
  ZMIRMemoryDefValueKind,
  ZMIRMemoryPhiValueKind,

  ZMIRFunctionValueKind,
  ZMIRGlobalAliasValueKind,
  ZMIRGlobalIFuncValueKind,
  ZMIRGlobalVariableValueKind,
  ZMIRBlockAddressValueKind,
  ZMIRConstantExprValueKind,
  ZMIRConstantArrayValueKind,
  ZMIRConstantStructValueKind,
  ZMIRConstantVectorValueKind,

  ZMIRUndefValueValueKind,
  ZMIRConstantAggregateZeroValueKind,
  ZMIRConstantDataArrayValueKind,
  ZMIRConstantDataVectorValueKind,
  ZMIRConstantIntValueKind,
  ZMIRConstantFPValueKind,
  ZMIRConstantPointerNullValueKind,
  ZMIRConstantTokenNoneValueKind,

  ZMIRMetadataAsValueValueKind,
  ZMIRInlineAsmValueKind,

  ZMIRInstructionValueKind,
  ZMIRPoisonValueValueKind,
  ZMIRConstantTargetNoneValueKind,
  ZMIRConstantPtrAuthValueKind,
} ZMIRValueKind;

typedef enum {
  ZMIRExternalLinkage,    /**< Externally visible function */
  ZMIRAvailableExternallyLinkage,
  ZMIRLinkOnceAnyLinkage, /**< Keep one copy of function when linking (inline)*/
  ZMIRLinkOnceODRLinkage, /**< Same, but only replaced by something
                            equivalent. */
  ZMIRLinkOnceODRAutoHideLinkage, /**< Obsolete */
  ZMIRWeakAnyLinkage,     /**< Keep one copy of function when linking (weak) */
  ZMIRWeakODRLinkage,     /**< Same, but only replaced by something
                            equivalent. */
  ZMIRAppendingLinkage,   /**< Special purpose, only applies to global arrays */
  ZMIRInternalLinkage,    /**< Rename collisions when linking (static
                               functions) */
  ZMIRPrivateLinkage,     /**< Like Internal, but omit from symbol table */
  ZMIRDLLImportLinkage,   /**< Obsolete */
  ZMIRDLLExportLinkage,   /**< Obsolete */
  ZMIRExternalWeakLinkage,/**< ExternalWeak linkage description */
  ZMIRGhostLinkage,       /**< Obsolete */
  ZMIRCommonLinkage,      /**< Tentative definitions */
  ZMIRLinkerPrivateLinkage, /**< Like Private, but linker removes. */
  ZMIRLinkerPrivateWeakLinkage /**< Like LinkerPrivate, but is weak. */
} ZMIRLinkage;

typedef enum {
  ZMIRNoUnnamedAddr,
  ZMIRLocalUnnamedAddr,
  ZMIRGlobalUnnamedAddr
} ZMIRUnnamedAddr;

typedef enum {
  ZMIRIntEQ = 32,
  ZMIRIntNE,
  ZMIRIntUGT,
  ZMIRIntUGE,
  ZMIRIntULT,
  ZMIRIntULE,
  ZMIRIntSGT,
  ZMIRIntSGE,
  ZMIRIntSLT,
  ZMIRIntSLE
} ZMIRIntPredicate;

typedef enum {
  ZMIRRealPredicateFalse,
  ZMIRRealOEQ,
  ZMIRRealOGT,
  ZMIRRealOGE,
  ZMIRRealOLT,
  ZMIRRealOLE,
  ZMIRRealONE,
  ZMIRRealORD,
  ZMIRRealUNO,
  ZMIRRealUEQ,
  ZMIRRealUGT,
  ZMIRRealUGE,
  ZMIRRealULT,
  ZMIRRealULE,
  ZMIRRealUNE,
  ZMIRRealPredicateTrue
} ZMIRRealPredicate;

struct ZMIRContext {
    ZState      *state;
    Allocator   *allocator;
};

struct ZMIRModule {
    ZMIRContextRef  ctx;
    const char      *name;
    ZMIRValueRef    *decls;
};

struct ZMIRBuilder {
    ZMIRContextRef      ctx;
    ZMIRBasicBlockRef   block;
};

typedef enum {
  ZMIRRet            = 1,
  ZMIRBr             = 2,
  ZMIRSwitch         = 3,
  ZMIRIndirectBr     = 4,
  ZMIRInvoke         = 5,
  /* removed 6 due to API changes */
  ZMIRUnreachable    = 7,
  ZMIRCallBr         = 67,

  /* Standard Unary Operators */
  ZMIRFNeg           = 66,

  /* Standard Binary Operators */
  ZMIRAdd            = 8,
  ZMIRFAdd           = 9,
  ZMIRSub            = 10,
  ZMIRFSub           = 11,
  ZMIRMul            = 12,
  ZMIRFMul           = 13,
  ZMIRUDiv           = 14,
  ZMIRSDiv           = 15,
  ZMIRFDiv           = 16,
  ZMIRURem           = 17,
  ZMIRSRem           = 18,
  ZMIRFRem           = 19,

  /* Logical Operators */
  ZMIRShl            = 20,
  ZMIRLShr           = 21,
  ZMIRAShr           = 22,
  ZMIRAnd            = 23,
  ZMIROr             = 24,
  ZMIRXor            = 25,

  /* Memory Operators */
  ZMIRAlloca         = 26,
  ZMIRLoad           = 27,
  ZMIRStore          = 28,
  ZMIRGetElementPtr  = 29,

  /* Cast Operators */
  ZMIRTrunc          = 30,
  ZMIRZExt           = 31,
  ZMIRSExt           = 32,
  ZMIRFPToUI         = 33,
  ZMIRFPToSI         = 34,
  ZMIRUIToFP         = 35,
  ZMIRSIToFP         = 36,
  ZMIRFPTrunc        = 37,
  ZMIRFPExt          = 38,
  ZMIRPtrToInt       = 39,
  ZMIRPtrToAddr      = 69,
  ZMIRIntToPtr       = 40,
  ZMIRBitCast        = 41,
  ZMIRAddrSpaceCast  = 60,

  /* Other Operators */
  ZMIRICmp           = 42,
  ZMIRFCmp           = 43,
  ZMIRPHI            = 44,
  ZMIRCall           = 45,
  ZMIRSelect         = 46,
  ZMIRUserOp1        = 47,
  ZMIRUserOp2        = 48,
  ZMIRVAArg          = 49,
  ZMIRExtractElement = 50,
  ZMIRInsertElement  = 51,
  ZMIRShuffleVector  = 52,
  ZMIRExtractValue   = 53,
  ZMIRInsertValue    = 54,
  ZMIRFreeze         = 68,

  /* zmir-only: lowered to llvm.memset */
  ZMIRMemSet         = 70,

  /* Atomic operators */
  ZMIRFence          = 55,
  ZMIRAtomicCmpXchg  = 56,
  ZMIRAtomicRMW      = 57,

  /* Exception Handling Operators */
  ZMIRResume         = 58,
  ZMIRLandingPad     = 59,
  ZMIRCleanupRet     = 61,
  ZMIRCatchRet       = 62,
  ZMIRCatchPad       = 63,
  ZMIRCleanupPad     = 64,
  ZMIRCatchSwitch    = 65
} ZMIROpcode;

struct ZMIRType {
    ZMIRTypeKind        kind;
    const char          *name;
    union {
        u32             width; // integer

        struct {
            ZMIRTypeRef *fields;
            bool        packed;
        } strct;

        struct {
            ZMIRTypeRef elem;
            usize       len;
        } array;

        struct {
            ZMIRTypeRef ret;
            ZMIRTypeRef *args;
            bool        vararg;
        } func;
    };
};

struct ZMIRBasicBlock {
    const char      *name;
    ZMIRValueRef    parent;
    ZMIRValueRef    *instrs;
};

struct ZMIRValue {
    ZMIRValueKind   kind;
    ZMIRTypeRef     type;
    const char      *name;
    union {
        struct { ZMIRValueRef fn; u32 index; } arg;
        struct {
            ZMIRTypeRef         fnType;
            ZMIRValueRef        *params;
            ZMIRBasicBlockRef   *blocks;
            ZMIRLinkage         linkage;
            const char          **attrs;
            ZMIRModuleRef       mod;
        } fn;

        struct {
            ZMIRTypeRef     valueType;
            ZMIRValueRef    init;
            ZMIRLinkage     linkage;
            ZMIRUnnamedAddr unnamedAddr;
            usize           align;
            bool            isConst;
        } global;

        struct {
            u64     value;
            bool    signext;
        } cint;

        double creal;

        ZMIRValueRef *aggregate;

        struct {
            const char  *data;
            usize       len;
        } str;

        struct {
            ZMIROpcode          op;
            ZMIRBasicBlockRef   parent;
            ZMIRValueRef        *ops;
            ZMIRBasicBlockRef   *blocks;

            ZMIRTypeRef         auxType;
            union {
                ZMIRIntPredicate    ipred;
                ZMIRRealPredicate   fpred;
                u32                 index;
                struct {
                    usize len, align;
                } builtin_memset;
            };
            usize align;
            bool isVolatile;
        } instr;
    };
};

ZMIRTypeRef ZMIRVoidTypeInContext   (ZMIRContextRef);
ZMIRTypeRef ZMIRInt1TypeInContext   (ZMIRContextRef);
ZMIRTypeRef ZMIRInt8TypeInContext   (ZMIRContextRef);
ZMIRTypeRef ZMIRInt16TypeInContext  (ZMIRContextRef);
ZMIRTypeRef ZMIRInt32TypeInContext  (ZMIRContextRef);
ZMIRTypeRef ZMIRInt64TypeInContext  (ZMIRContextRef);
ZMIRTypeRef ZMIRFloatTypeInContext  (ZMIRContextRef);
ZMIRTypeRef ZMIRDoubleTypeInContext (ZMIRContextRef);
ZMIRTypeRef ZMIRIntTypeInContext    (ZMIRContextRef, usize);
ZMIRTypeRef ZMIRPointerTypeInContext(ZMIRContextRef, usize);
ZMIRTypeRef ZMIRArrayTypeInContext  (ZMIRContextRef, ZMIRTypeRef, usize);
ZMIRTypeRef ZMIRStructTypeInContext (ZMIRContextRef, ZMIRTypeRef *, usize, bool);
ZMIRTypeRef ZMIRStructCreateNamed   (ZMIRContextRef, const char *);
ZMIRTypeRef ZMIRPointerType         (ZMIRTypeRef, usize);
ZMIRTypeRef ZMIRArrayType           (ZMIRTypeRef, usize);
ZMIRTypeRef ZMIRArrayType2          (ZMIRTypeRef, usize);
ZMIRTypeRef ZMIRFunctionType        (ZMIRTypeRef, ZMIRTypeRef *, usize, bool);

ZMIRModuleRef   ZMIRModuleCreateWithNameInContext(const char *name, ZMIRContextRef);
ZMIRContextRef  ZMIRContextCreate(ZState *, Allocator *);
ZMIRBuilderRef  ZMIRCreateBuilderInContext(ZMIRContextRef);

void ZMIRStructSetBody(ZMIRTypeRef structType, ZMIRTypeRef *, usize, bool);

ZMIRBasicBlockRef ZMIRAppendBasicBlockInContext(ZMIRContextRef, ZMIRValueRef, const char *);
void ZMIRBuildBr(ZMIRBuilderRef, ZMIRBasicBlockRef);
void ZMIRBuildCondBr(ZMIRBuilderRef, ZMIRValueRef, ZMIRBasicBlockRef, ZMIRBasicBlockRef);

bool ZMIRGetBasicBlockTerminator(ZMIRBasicBlockRef);
ZMIRBasicBlockRef ZMIRGetInsertBlock(ZMIRBuilderRef);

ZMIRValueRef ZMIRBuildStore(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef);
ZMIRValueRef ZMIRBuildLoad2(ZMIRBuilderRef, ZMIRTypeRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildStructGEP2(ZMIRBuilderRef, ZMIRTypeRef, ZMIRValueRef, usize, const char *);
ZMIRValueRef ZMIRBuildGEP2(ZMIRBuilderRef, ZMIRTypeRef, ZMIRValueRef, ZMIRValueRef *, usize, const char *);
ZMIRValueRef ZMIRBuildAdd(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildSub(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildMul(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildShl(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildUDiv(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildSDiv(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildURem(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildSRem(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildLShr(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildAShr(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildFAdd(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildFSub(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildFMul(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildFDiv(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildFRem(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildOr(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildXor(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildAnd(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildAlloca(ZMIRBuilderRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildICmp(ZMIRBuilderRef, ZMIRIntPredicate, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildFCmp(ZMIRBuilderRef, ZMIRRealPredicate, ZMIRValueRef, ZMIRValueRef, const char *);
ZMIRValueRef ZMIRBuildFPExt(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildFPTrunc(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildZExt(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildCall2(ZMIRBuilderRef, ZMIRTypeRef, ZMIRValueRef, ZMIRValueRef *, usize, const char *);
ZMIRValueRef ZMIRBuildBitCast(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildPtrToInt(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildIntToPtr(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildFPToSI(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildFPToUI(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildUIToFP(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildSIToFP(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildTrunc(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildSExt(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildZExt(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildZExtOrBitCast(ZMIRBuilderRef, ZMIRValueRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildPhi(ZMIRBuilderRef, ZMIRTypeRef, const char *);
void ZMIRAddIncoming(ZMIRValueRef, ZMIRValueRef *, ZMIRBasicBlockRef *, usize);
ZMIRValueRef ZMIRBuildExtractValue(ZMIRBuilderRef, ZMIRValueRef, usize, const char *);
void ZMIRBuildRetVoid(ZMIRBuilderRef);
void ZMIRBuildRet(ZMIRBuilderRef, ZMIRValueRef);
void ZMIRBuildUnreachable(ZMIRBuilderRef);
void ZMIRBuildTrap(ZMIRModuleRef, ZMIRBuilderRef);
void ZMIRBuildMemSet(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, usize, usize);

void ZMIRPositionBuilderAtEnd(ZMIRBuilderRef, ZMIRBasicBlockRef);

ZMIRValueRef ZMIRConstInt(ZMIRTypeRef, isize, bool);
ZMIRValueRef ZMIRConstReal(ZMIRTypeRef, double);
ZMIRValueRef ZMIRConstPointerNull(ZMIRTypeRef);
ZMIRValueRef ZMIRConstNull(ZMIRTypeRef);
ZMIRValueRef ZMIRConstStringInContext(ZMIRContextRef, const char *, usize, bool);
ZMIRValueRef ZMIRConstStructInContext(ZMIRContextRef, ZMIRValueRef *, usize, bool);
ZMIRValueRef ZMIRConstAllOnes(ZMIRTypeRef);
ZMIRValueRef ZMIRConstNamedStruct(ZMIRTypeRef, ZMIRValueRef *, usize);
ZMIRValueRef ZMIRConstArray2(ZMIRTypeRef, ZMIRValueRef *, usize);

ZMIRValueRef ZMIRAddFunction(ZMIRModuleRef, const char *, ZMIRTypeRef);
ZMIRValueRef ZMIRAddGlobal(ZMIRModuleRef, ZMIRTypeRef, const char *);
ZMIRValueRef ZMIRBuildGlobalString(ZMIRBuilderRef, const char *, const char *);
void ZMIRSetAlignment(ZMIRValueRef, usize);
void ZMIRSetGlobalConstant(ZMIRValueRef, bool);
void ZMIRSetLinkage(ZMIRValueRef, ZMIRLinkage);
void ZMIRSetUnnamedAddr(ZMIRValueRef, ZMIRUnnamedAddr);

ZMIRValueRef ZMIRGetNamedFunction(ZMIRModuleRef, const char *);
ZMIRValueRef ZMIRGetNamedGlobal(ZMIRModuleRef, const char *);
ZMIRTypeRef ZMIRGetReturnType(ZMIRTypeRef);
ZMIRValueRef ZMIRGetParam(ZMIRValueRef, usize);
ZMIRTypeRef ZMIRStructGetTypeAtIndex(ZMIRTypeRef, usize);
ZMIRValueKind ZMIRGetValueKind(ZMIRValueRef);
usize ZMIRGetIntTypeWidth(ZMIRTypeRef);
bool ZMIRIsLiteralStruct(ZMIRTypeRef);
bool ZMIRIsFunctionVarArg(ZMIRTypeRef);
void ZMIRAddAttribute(ZMIRValueRef, const char *);
char ** ZMIRGetAttribute(ZMIRValueRef);
usize ZMIRCountStructElementTypes(ZMIRTypeRef);

usize ZMIRSizeOf(ZMIRTypeRef);
usize ZMIRGetAlignment(ZMIRValueRef);
u32 ZMIRLookupIntrinsicID(const char *, usize);
ZMIRValueRef ZMIRGetIntrinsicDeclaration(ZMIRModuleRef, u32, ZMIRTypeRef *, usize);

ZMIRTypeRef ZMIRTypeOf(ZMIRValueRef);
ZMIRTargetDataRef ZMIRGetModuleDataLayout(ZMIRModuleRef);
usize ZMIRABIAlignmentOfType(ZMIRTargetDataRef, ZMIRTypeRef);
usize ZMIRABISizeOfType(ZMIRTargetDataRef, ZMIRTypeRef);
ZMIRTypeKind ZMIRGetTypeKind(ZMIRTypeRef);
bool ZMIRTypeEqual(ZMIRTypeRef, ZMIRTypeRef);
ZMIRTypeRef ZMIRGlobalGetValueType(ZMIRValueRef);
usize ZMIRCountParamTypes(ZMIRTypeRef);
void ZMIRGetParamTypes(ZMIRTypeRef, ZMIRTypeRef *);

void ZMIRSetInitializer(ZMIRValueRef, ZMIRValueRef);
ZMIRValueRef ZMIRGetInitializer(ZMIRValueRef);
void ZMIRSetVolatile(ZMIRValueRef, bool);
bool ZMIRIsConstant(ZMIRValueRef);

#endif
