#include "zmir.h"
#include "zmem.h"

extern _Thread_local ZMIRTypeRef i0Type;
extern _Thread_local ZMIRTypeRef i1Type;
extern _Thread_local ZMIRTypeRef i8Type;
extern _Thread_local ZMIRTypeRef i16Type;
extern _Thread_local ZMIRTypeRef i32Type;
extern _Thread_local ZMIRTypeRef i64Type;
extern _Thread_local ZMIRTypeRef f32Type;
extern _Thread_local ZMIRTypeRef f64Type;
extern _Thread_local ZMIRTypeRef ptrType;
extern _Thread_local ZMIRTypeRef strType;
extern _Thread_local ZMIRTypeRef usizeType;
extern _Thread_local ZMIRTypeRef isizeType;

extern ZNode *LangItems[Z_LANG_COUNT];

struct ZMIRContext {
    ZState      *state;
    Allocator   *allocator;
};

struct ZMIRModule {
    ZMIRContextRef ctx;
    ZMIRValueRef *decls;
};

struct ZMIRBuilder {
    ZMIRContextRef ctx;
    ZMIRBasicBlockRef block;
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

static inline ZMIRTypeRef ZMIRTypeInContext(ZMIRContextRef ctx, ZMIRTypeKind kind) {
    return copy(ctx->allocator, ZMIRType, ((ZMIRType){ .kind = kind }));
}

ZMIRTypeRef ZMIRIntTypeInContext(ZMIRContextRef ctx, usize width) {
    ZMIRTypeRef type = ZMIRTypeInContext(ctx, ZMIRIntegerTypeKind);
    type->width = width;
    return type;
}

ZMIRTypeRef ZMIRInt1TypeInContext   (ZMIRContextRef ctx)            { return ZMIRIntTypeInContext(ctx, 1);                  }
ZMIRTypeRef ZMIRInt8TypeInContext   (ZMIRContextRef ctx)            { return ZMIRIntTypeInContext(ctx, 8);                  }
ZMIRTypeRef ZMIRInt16TypeInContext  (ZMIRContextRef ctx)            { return ZMIRIntTypeInContext(ctx, 16);                 }
ZMIRTypeRef ZMIRInt32TypeInContext  (ZMIRContextRef ctx)            { return ZMIRIntTypeInContext(ctx, 32);                 }
ZMIRTypeRef ZMIRInt64TypeInContext  (ZMIRContextRef ctx)            { return ZMIRIntTypeInContext(ctx, 64);                 }
ZMIRTypeRef ZMIRVoidTypeInContext   (ZMIRContextRef ctx)            { return ZMIRTypeInContext(ctx, ZMIRVoidTypeKind);      }
ZMIRTypeRef ZMIRFloatTypeInContext  (ZMIRContextRef ctx)            { return ZMIRTypeInContext(ctx, ZMIRFloatTypeKind);     }
ZMIRTypeRef ZMIRDoubleTypeInContext (ZMIRContextRef ctx)            { return ZMIRTypeInContext(ctx, ZMIRDoubleTypeKind);    }
ZMIRTypeRef ZMIRPointerTypeInContext(ZMIRContextRef ctx, usize ptr) { return ZMIRTypeInContext(ctx, ZMIRPointerTypeKind);   }

ZMIRTypeRef ZMIRStructTypeInContext (ZMIRContextRef ctx, ZMIRTypeRef *fields, usize len, bool packed) {
    ZMIRTypeRef *strctFields = NULL;
    vecreservewith(strctFields, len, ctx->allocator);
    for (usize i = 0; i < len; i++)
        strctFields[i] = fields[i];
    return copy(ctx->allocator, ZMIRType, ((ZMIRType) {
        .kind = ZMIRStructTypeKind,
        .strct = { strctFields, packed }
    }));
}

ZMIRTypeRef ZMIRStructCreateNamed   (ZMIRContextRef ctx, const char *name) {
    return copy(ctx->allocator, ZMIRType, ((ZMIRType){
        .kind   = ZMIRStructTypeKind,
        .name   = name,
        .strct  = { NULL, false }
    }));
}

[[deprecated("Use ZMIRPointerTypeInContext instead")]]
ZMIRTypeRef ZMIRPointerType         (ZMIRTypeRef type, usize ptr) {
    return NULL;
}

[[deprecated("Use ZMIRArrayTypeInContext instead")]]
ZMIRTypeRef ZMIRArrayType           (ZMIRTypeRef type, usize len) {
    return NULL;
}

[[deprecated("Use ZMIRArrayTypeInContext instead")]]
ZMIRTypeRef ZMIRArrayType2          (ZMIRTypeRef type, usize len) {
    return NULL;
}

ZMIRTypeRef ZMIRFunctionType        (ZMIRTypeRef ret, ZMIRTypeRef *args, usize len, bool vararg) {
    ZMIRTypeRef *fargs = NULL;
    vecreserve(fargs, len);
    memcpy(fargs, args, sizeof(ZMIRTypeRef) * len);
    return copy(arenaAllocator, ZMIRType, ((ZMIRType){
        .kind = ZMIRFunctionTypeKind,
        .func = { ret, fargs, vararg }
    }));
}

static ZMIRValueRef instr(ZMIRBuilderRef b, ZMIROpcode op, ZMIRTypeRef type, const char *name) {
    ZMIRValueRef v = copy(b->ctx->allocator, ZMIRValue, ((ZMIRValue){
        .kind   = ZMIRInstructionValueKind,
        .type   = type,
        .name   = name,
        .instr  = { .op = op, .parent = b->block }
    }));
    vecpush(b->block->instrs, v);
    return v;
}

void ZMIRBuildBr(ZMIRBuilderRef b, ZMIRBasicBlockRef block) {
    ZMIRValueRef br = instr(b, ZMIRBr, NULL, NULL);
    vecpushwith(br->instr.blocks, block, b->ctx->allocator);
}

void ZMIRBuildCondBr(ZMIRBuilderRef b, ZMIRValueRef cond, ZMIRBasicBlockRef trueBranch, ZMIRBasicBlockRef falseBranch) {
    ZMIRValueRef v = instr(b, ZMIRBr, NULL, NULL);
    Allocator *a = b->ctx->allocator;
    vecpushwith(v->instr.ops, cond, a);
    vecpushwith(v->instr.blocks, trueBranch, a);
    vecpushwith(v->instr.blocks, falseBranch, a);
}

ZMIRValueRef ZMIRBuildAlloca(ZMIRBuilderRef b, ZMIRTypeRef type, const char *name) {
    return instr(b, ZMIRAlloca, type, name);
}

ZMIRValueRef ZMIRBuildStore(ZMIRBuilderRef b, ZMIRValueRef ptr, ZMIRValueRef val) {
    ZMIRValueRef v = instr(b, ZMIRStore, NULL, NULL);
    vecpushwith(v->instr.ops, ptr, b->ctx->allocator);
    vecpushwith(v->instr.ops, val, b->ctx->allocator);
    return v;
}

ZMIRValueRef ZMIRBuildLoad2(ZMIRBuilderRef b, ZMIRTypeRef type, ZMIRValueRef value, const char *name) {
    ZMIRValueRef v = instr(b, ZMIRLoad, type, name);
    vecpushwith(v->instr.ops, value, b->ctx->allocator);
    return v;
}

ZMIRValueRef ZMIRBuildICmp(ZMIRBuilderRef b, ZMIRIntPredicate predicate, ZMIRValueRef lhs, ZMIRValueRef rhs, const char *name) {
    if (!lhs || !rhs) return NULL;
    if (lhs->type != rhs->type) return NULL;
    ZMIRValueRef v = instr(b, ZMIRICmp, lhs->type, name);
    v->instr.ipred = predicate;
    vecpushwith(v->instr.ops, lhs, b->ctx->allocator);
    vecpushwith(v->instr.ops, rhs, b->ctx->allocator);
    return v;
}

ZMIRValueRef ZMIRBuildFCmp(ZMIRBuilderRef b, ZMIRRealPredicate predicate, ZMIRValueRef lhs, ZMIRValueRef rhs, const char *name) {
    if (!lhs || !rhs) return NULL;
    if (lhs->type != rhs->type) return NULL;
    ZMIRValueRef v = instr(b, ZMIRICmp, lhs->type, name);
    v->instr.fpred = predicate;
    vecpushwith(v->instr.ops, lhs, b->ctx->allocator);
    vecpushwith(v->instr.ops, rhs, b->ctx->allocator);
    return v;
}

void ZMIRAddAttribute(ZMIRValueRef v, const char *name) {
    if (!v || v->kind != ZMIRFunctionValueKind) return;
    vecpush(v->fn.attrs, name);
}

#define ZMIRValueNew(a, ...) copy(a, ZMIRValue, ((ZMIRValue) { __VA_ARGS__ }))

ZMIRValueRef ZMIRAddFunction(ZMIRModuleRef mod, const char *name, ZMIRTypeRef type) {
    Allocator *a = mod->ctx->allocator;
    ZMIRValueRef func = ZMIRValueNew(a,
        .kind = ZMIRFunctionValueKind,
        .fn = {
            .fnType     = type,
            .linkage    = ZMIRLinkOnceODRLinkage,
            .attrs      = NULL,
            .blocks     = NULL
        }
    );
    vecpushwith(mod->decls, func, a);
    return func;
}

#define ZMIR_UBSAN_TRAP     "llvm.ubsantrap"
#define ZMIR_DEBUG_TRACE    "zn.debug.trace"

void ZMIRBuildTrap(ZMIRModuleRef mod, ZMIRBuilderRef builder) {
    u32 ubsanId                 = ZMIRLookupIntrinsicID(
        ZMIR_UBSAN_TRAP, strlen(ZMIR_UBSAN_TRAP));

    ZMIRValueRef ubsanFunc      = ZMIRGetIntrinsicDeclaration(
        mod, ubsanId, NULL, 0);

    ZMIRTypeRef funcType        = ZMIRFunctionType(
        i0Type, (ZMIRTypeRef[]){i8Type}, 1, 0);

    ZMIRBuildCall2(
        builder, funcType, ubsanFunc, (ZMIRValueRef[]){
            ZMIRConstInt(i8Type, 0, 0),
        }, 1, ""
    );
    ZMIRBuildUnreachable(builder);
}
