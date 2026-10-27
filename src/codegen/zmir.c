#include "zmir.h"

struct ZMIRContext {
    ZState *state;
    Allocator *allocator;
};

struct ZMIRType {
    ZMIRTypeKind kind;
    const char *name;
    union {
        u32 width; // integer

        struct {
            ZMIRTypeRef *fields;
            bool packed;
        } strct;

        struct {
            ZMIRTypeRef elem;
            usize len;
        } array;

        struct {
            ZMIRTypeRef ret;
            ZMIRTypeRef *args;
            bool vararg;
        } func;
    };
};

struct ZMIRValue {

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
ZMIRTypeRef ZMIRStructCreateNamed   (ZMIRContextRef, const char *);
ZMIRTypeRef ZMIRPointerType         (ZMIRTypeRef, usize);
ZMIRTypeRef ZMIRArrayType           (ZMIRTypeRef, usize);
ZMIRTypeRef ZMIRArrayType2          (ZMIRTypeRef, usize);
ZMIRTypeRef ZMIRFunctionType        (ZMIRTypeRef, ZMIRTypeRef *, usize, bool);

// void ZMIRBuildTrap(ZMIRBuilderRef builder) {
//     u32 ubsanId                 = ZMIRLookupIntrinsicID(
//         ZMIR_UBSAN_TRAP, strlen(ZMIR_UBSAN_TRAP));
//
//     ZMIRValueRef ubsanFunc      = ZMIRGetIntrinsicDeclaration(
//         ctx->mod, ubsanId, NULL, 0);
//
//     ZMIRTypeRef funcType        = ZMIRFunctionType(
//         i0Type, (ZMIRTypeRef[]){i8Type}, 1, 0);
//
//     ZMIRBuildCall2(
//         builder, funcType, ubsanFunc, (ZMIRValueRef[]){
//             ZMIRConstInt(i8Type, 0, 0),
//         }, 1, ""
//     );
//     ZMIRBuildUnreachable(builder);
// }
