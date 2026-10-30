#include "zmir.h"
#include "zmem.h"

#define CHECK_KIND(t, k) ((t) && (t)->kind == (k))
#define ZMIRValueNew(a, ...) copy(a, ZMIRValue, ((ZMIRValue) { __VA_ARGS__ }))

/* ==================Externs          ============*/

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

/* ==================Context          ============*/

ZMIRContextRef ZMIRContextCreate(ZState *state, Allocator *a) {
    return copy(a, ZMIRContext, ((ZMIRContext){ .state = state, .allocator = a}));
}

/* ==================Modules          ============*/

ZMIRModuleRef ZMIRModuleCreateWithNameInContext(const char *name, ZMIRContextRef ctx) {
    return copy(ctx->allocator, ZMIRModule, ((ZMIRModule){
        .ctx    = ctx,
        .name   = name,
        .decls  = NULL,
    }));
}

ZMIRTargetDataRef ZMIRGetModuleDataLayout(ZMIRModuleRef mod) {
    return mod ? mod->ctx : NULL;
}

static const char *dupname(Allocator *a, const char *name) {
    return name ? zstrdup(a, (char *)name) : NULL;
}

/* ==================Types            ============*/

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
ZMIRTypeRef ZMIRPointerTypeInContext(ZMIRContextRef ctx, usize as) { (void)as; return ZMIRTypeInContext(ctx, ZMIRPointerTypeKind); }

ZMIRTypeRef ZMIRStructTypeInContext (ZMIRContextRef ctx, ZMIRTypeRef *fields, usize len, bool packed) {
    ZMIRTypeRef *strctFields = NULL;
    vecreservewith(strctFields, len, ctx->allocator);
    if (len) memcpy(strctFields, fields, sizeof(ZMIRTypeRef) * len);
    vecsetlen(strctFields, len);

    return copy(ctx->allocator, ZMIRType, ((ZMIRType) {
        .kind = ZMIRStructTypeKind,
        .strct = { strctFields, packed }
    }));
}

ZMIRTypeRef ZMIRStructCreateNamed   (ZMIRContextRef ctx, const char *name) {
    return copy(ctx->allocator, ZMIRType, ((ZMIRType){
        .kind   = ZMIRStructTypeKind,
        .name   = dupname(ctx->allocator, name),
        .strct  = { NULL, false }
    }));
}

void ZMIRStructSetBody(ZMIRTypeRef structType, ZMIRTypeRef *fields, usize len, bool packed) {
    if (!structType || structType->kind != ZMIRStructTypeKind) return;
    if (structType->strct.fields != NULL) return;
    ZMIRTypeRef  *structFields = NULL;
    vecreservewith(structFields, len, arenaAllocator);
    if (len) memcpy(structFields, fields, sizeof(ZMIRTypeRef) * len);
    vecsetlen(structFields, len);

    structType->strct.fields = structFields;
    structType->strct.packed = packed;
}

static ZMIRTypeRef arrayType(Allocator *a, ZMIRTypeRef elem, usize len) {
    return copy(a, ZMIRType, ((ZMIRType){
        .kind   = ZMIRArrayTypeKind,
        .array  = { elem, len }
    }));
}

ZMIRTypeRef ZMIRArrayTypeInContext  (ZMIRContextRef ctx, ZMIRTypeRef elem, usize len) {
    return arrayType(ctx->allocator, elem, len);
}

[[deprecated("Use ZMIRPointerTypeInContext instead")]]
ZMIRTypeRef ZMIRPointerType         (ZMIRTypeRef type, usize as) {
    (void)type; (void)as;
    return copy(arenaAllocator, ZMIRType, ((ZMIRType){ .kind = ZMIRPointerTypeKind }));
}

[[deprecated("Use ZMIRArrayTypeInContext instead")]]
ZMIRTypeRef ZMIRArrayType           (ZMIRTypeRef type, usize len) {
    return arrayType(arenaAllocator, type, len);
}

[[deprecated("Use ZMIRArrayTypeInContext instead")]]
ZMIRTypeRef ZMIRArrayType2          (ZMIRTypeRef type, usize len) {
    return arrayType(arenaAllocator, type, len);
}

ZMIRTypeRef ZMIRFunctionType        (ZMIRTypeRef ret, ZMIRTypeRef *args, usize len, bool vararg) {
    ZMIRTypeRef *fargs = NULL;
    vecreserve(fargs, len);
    if (len) memcpy(fargs, args, sizeof(ZMIRTypeRef) * len);
    vecsetlen(fargs, len);
    return copy(arenaAllocator, ZMIRType, ((ZMIRType){
        .kind = ZMIRFunctionTypeKind,
        .func = { ret, fargs, vararg }
    }));
}

/* ==================Type Queries     ============*/

ZMIRTypeRef ZMIRStructGetTypeAtIndex(ZMIRTypeRef strct, usize index) {

    if (!strct || strct->kind != ZMIRStructTypeKind) return NULL;
    if (index >= veclen(strct->strct.fields)) return NULL;
    return strct->strct.fields[index];
}

static inline usize alignTo(usize n, usize align) {
    return align ? (n + align - 1) / align * align : n;
}

/* A ZMIRTargetDataRef is the module's context; without one, the pointer
 * size falls back to the width of usize. */
static usize pointerSize(ZMIRTargetDataRef td) {
    ZMIRContextRef ctx = td;
    if (ctx && ctx->state && ctx->state->pointerSize) return ctx->state->pointerSize;
    return usizeType ? usizeType->width / 8 : 8;
}

/* Natural C layout, which matches LLVM's default data layout on the
 * targets we support. */
static void layout(ZMIRTypeRef t, usize ptr, usize *size, usize *align) {
    *size = 0; *align = 1;
    if (!t) return;

    switch (t->kind) {
    case ZMIRIntegerTypeKind: {
        usize bytes = (t->width + 7) / 8, p = 1;
        if (bytes == 0) return;
        while (p < bytes) p <<= 1;
        *size = p; *align = p < 16 ? p : 16;
        return;
    }
    case ZMIRHalfTypeKind:
    case ZMIRBFloatTypeKind:    *size = 2;   *align = 2;   return;
    case ZMIRFloatTypeKind:     *size = 4;   *align = 4;   return;
    case ZMIRDoubleTypeKind:    *size = 8;   *align = 8;   return;
    case ZMIRX86_FP80TypeKind:
    case ZMIRFP128TypeKind:
    case ZMIRPPC_FP128TypeKind: *size = 16;  *align = 16;  return;
    case ZMIRPointerTypeKind:   *size = ptr; *align = ptr; return;
    case ZMIRArrayTypeKind: {
        usize esize, ealign;
        layout(t->array.elem, ptr, &esize, &ealign);
        *size  = alignTo(esize, ealign) * t->array.len;
        *align = ealign;
        return;
    }
    case ZMIRStructTypeKind: {
        usize offset = 0, maxAlign = 1;
        for (usize i = 0; i < veclen(t->strct.fields); i++) {
            usize fsize, falign;
            layout(t->strct.fields[i], ptr, &fsize, &falign);
            if (t->strct.packed) falign = 1;
            offset = alignTo(offset, falign) + fsize;
            if (falign > maxAlign) maxAlign = falign;
        }
        *size  = alignTo(offset, maxAlign);
        *align = maxAlign;
        return;
    }
    default: return;
    }
}

usize ZMIRABISizeOfType(ZMIRTargetDataRef td, ZMIRTypeRef t) {
    usize size, align;
    layout(t, pointerSize(td), &size, &align);
    return alignTo(size, align);
}

usize ZMIRABIAlignmentOfType(ZMIRTargetDataRef td, ZMIRTypeRef t) {
    usize size, align;
    layout(t, pointerSize(td), &size, &align);
    return align;
}

usize ZMIRSizeOf(ZMIRTypeRef type) {
    return ZMIRABISizeOfType(NULL, type);
}

ZMIRTypeKind ZMIRGetTypeKind(ZMIRTypeRef type) {
    return type->kind;
}

/* Named structs are compared by identity, everything else structurally. */
bool ZMIRTypeEqual(ZMIRTypeRef a, ZMIRTypeRef b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;

    switch (a->kind) {
    case ZMIRIntegerTypeKind:
        return a->width == b->width;
    case ZMIRArrayTypeKind:
        return a->array.len == b->array.len && ZMIRTypeEqual(a->array.elem, b->array.elem);
    case ZMIRStructTypeKind:
        if (a->name || b->name)                                     return false;
        if (a->strct.packed != b->strct.packed)                     return false;
        if (veclen(a->strct.fields) != veclen(b->strct.fields))     return false;
        for (usize i = 0; i < veclen(a->strct.fields); i++)
            if (!ZMIRTypeEqual(a->strct.fields[i], b->strct.fields[i])) return false;
        return true;
    case ZMIRFunctionTypeKind:
        if (a->func.vararg != b->func.vararg)                       return false;
        if (veclen(a->func.args) != veclen(b->func.args))           return false;
        if (!ZMIRTypeEqual(a->func.ret, b->func.ret))               return false;
        for (usize i = 0; i < veclen(a->func.args); i++)
            if (!ZMIRTypeEqual(a->func.args[i], b->func.args[i]))   return false;
        return true;
    default:
        return true;
    }
}

bool ZMIRIsFunctionVarArg(ZMIRTypeRef func) {
    return CHECK_KIND(func, ZMIRFunctionTypeKind) ? func->func.vararg : false;
}

bool ZMIRIsLiteralStruct(ZMIRTypeRef type) {
    return CHECK_KIND(type, ZMIRStructTypeKind) && type->name == NULL;
}

ZMIRTypeRef ZMIRGetReturnType(ZMIRTypeRef t) {
    return CHECK_KIND(t, ZMIRFunctionTypeKind) ? t->func.ret : NULL;
}

usize ZMIRCountParamTypes(ZMIRTypeRef type) {
    return CHECK_KIND(type, ZMIRFunctionTypeKind) ? veclen(type->func.args) : 0;
}

void ZMIRGetParamTypes(ZMIRTypeRef t, ZMIRTypeRef *dest) {
    if (!CHECK_KIND(t, ZMIRFunctionTypeKind)) return;
    usize len = veclen(t->func.args);
    if (len) memcpy(dest, t->func.args, sizeof(ZMIRTypeRef) * len);
}

usize ZMIRGetIntTypeWidth(ZMIRTypeRef t) {
    return CHECK_KIND(t, ZMIRIntegerTypeKind) ? t->width : 0;
}

usize ZMIRCountStructElementTypes(ZMIRTypeRef t) {
    return CHECK_KIND(t, ZMIRStructTypeKind) ? veclen(t->strct.fields) : 0;
}

/* ==================Value Queries    ============*/

ZMIRTypeRef ZMIRTypeOf(ZMIRValueRef val) {
    return val ? val->type : NULL;
}

ZMIRValueKind ZMIRGetValueKind(ZMIRValueRef val) {
    return val->kind;
}

/* Like LLVMIsConstant: true for constants and for the address of a global. */
bool ZMIRIsConstant(ZMIRValueRef val) {
    if (!val) return false;
    switch (val->kind) {
    case ZMIRFunctionValueKind:
    case ZMIRGlobalAliasValueKind:
    case ZMIRGlobalIFuncValueKind:
    case ZMIRGlobalVariableValueKind:
    case ZMIRBlockAddressValueKind:
    case ZMIRConstantExprValueKind:
    case ZMIRConstantArrayValueKind:
    case ZMIRConstantStructValueKind:
    case ZMIRConstantVectorValueKind:
    case ZMIRUndefValueValueKind:
    case ZMIRConstantAggregateZeroValueKind:
    case ZMIRConstantDataArrayValueKind:
    case ZMIRConstantDataVectorValueKind:
    case ZMIRConstantIntValueKind:
    case ZMIRConstantFPValueKind:
    case ZMIRConstantPointerNullValueKind:
    case ZMIRConstantTokenNoneValueKind:
    case ZMIRPoisonValueValueKind:
    case ZMIRConstantTargetNoneValueKind:
    case ZMIRConstantPtrAuthValueKind:
        return true;
    default:
        return false;
    }
}

char ** ZMIRGetAttribute(ZMIRValueRef fn) {
    return CHECK_KIND(fn, ZMIRFunctionValueKind) ? (char **)fn->fn.attrs : NULL;
}

usize ZMIRGetAlignment(ZMIRValueRef val) {
    if (!val) return 0;
    switch (val->kind) {
    case ZMIRInstructionValueKind:      return val->instr.align;
    case ZMIRGlobalVariableValueKind:   return val->global.align;
    default:                            return 0;
    }
}

ZMIRValueRef ZMIRGetInitializer(ZMIRValueRef global) {
    return CHECK_KIND(global, ZMIRGlobalVariableValueKind) ? global->global.init : NULL;
}

ZMIRTypeRef ZMIRGlobalGetValueType(ZMIRValueRef val) {
    if (!val) return NULL;
    switch (val->kind) {
    case ZMIRFunctionValueKind:         return val->fn.fnType;
    case ZMIRGlobalVariableValueKind:   return val->global.valueType;
    default:                            return NULL;
    }
}

/* ==================Value Attributes ============*/

void ZMIRSetAlignment(ZMIRValueRef val, usize alignment) {
    if (!val) return;
    switch (val->kind) {
    case ZMIRInstructionValueKind:      val->instr.align  = alignment; break;
    case ZMIRGlobalVariableValueKind:   val->global.align = alignment; break;
    default: break;
    }
}

void ZMIRSetLinkage(ZMIRValueRef val, ZMIRLinkage linkage) {
    if (!val) return;
    switch (val->kind) {
    case ZMIRGlobalVariableValueKind:   val->global.linkage = linkage;  break;
    case ZMIRFunctionValueKind:         val->fn.linkage     = linkage;  break;
    default: break;
    }
}

void ZMIRSetUnnamedAddr(ZMIRValueRef value, ZMIRUnnamedAddr addr) {
    if (!CHECK_KIND(value, ZMIRGlobalVariableValueKind)) return;
    value->global.unnamedAddr = addr;
}

void ZMIRSetVolatile(ZMIRValueRef val, bool vol) {
    if (!CHECK_KIND(val, ZMIRInstructionValueKind)) return;
    val->instr.isVolatile = vol;
}

void ZMIRSetGlobalConstant(ZMIRValueRef val, bool isConst) {
    if (!CHECK_KIND(val, ZMIRGlobalVariableValueKind)) return;
    val->global.isConst = isConst;
}

void ZMIRSetInitializer(ZMIRValueRef globalRef, ZMIRValueRef initializer) {
    if (!CHECK_KIND(globalRef, ZMIRGlobalVariableValueKind)) return;
    globalRef->global.init = initializer;
}

/* ==================Constants        ============*/

static inline ZMIRValueRef constant(Allocator *a, ZMIRValueKind kind, ZMIRTypeRef type) {
    return ZMIRValueNew(a, .kind = kind, .type = type);
}

static ZMIRValueRef *copyValues(Allocator *a, ZMIRValueRef *vals, usize len) {
    ZMIRValueRef *out = NULL;
    vecreservewith(out, len, a);
    if (len) memcpy(out, vals, sizeof(ZMIRValueRef) * len);
    vecsetlen(out, len);
    return out;
}

ZMIRValueRef ZMIRConstInt(ZMIRTypeRef t, isize val, bool signext) {
    ZMIRValueRef v  = constant(arenaAllocator, ZMIRConstantIntValueKind, t);
    u32 width       = ZMIRGetIntTypeWidth(t);
    u64 bits        = (u64)val;
    if (width && width < 64) bits &= (1ULL << width) - 1;
    v->cint.value   = bits;
    v->cint.signext = signext;
    return v;
}

ZMIRValueRef ZMIRConstReal(ZMIRTypeRef t, double creal) {
    ZMIRValueRef v  = constant(arenaAllocator, ZMIRConstantFPValueKind, t);
    v->creal        = creal;
    return v;
}

ZMIRValueRef ZMIRConstPointerNull(ZMIRTypeRef t) {
    return constant(arenaAllocator, ZMIRConstantPointerNullValueKind, t);
}

ZMIRValueRef ZMIRConstNull(ZMIRTypeRef t) {
    switch (t->kind) {
    case ZMIRIntegerTypeKind:   return ZMIRConstInt(t, 0, false);
    case ZMIRHalfTypeKind:
    case ZMIRFloatTypeKind:
    case ZMIRDoubleTypeKind:    return ZMIRConstReal(t, 0.0);
    case ZMIRPointerTypeKind:   return ZMIRConstPointerNull(t);
    case ZMIRStructTypeKind:
    case ZMIRArrayTypeKind:
    case ZMIRVectorTypeKind:    return constant(arenaAllocator, ZMIRConstantAggregateZeroValueKind, t);
    default:                    return NULL;
    }
}

ZMIRValueRef ZMIRConstAllOnes(ZMIRTypeRef t) {
    return CHECK_KIND(t, ZMIRIntegerTypeKind) ?
            ZMIRConstInt(t, -1, true) : NULL;
}

ZMIRValueRef ZMIRConstStringInContext(ZMIRContextRef ctx, const char *s, usize len, bool dontNullTerminate) {
    Allocator *a    = ctx->allocator;
    usize n         = len + (dontNullTerminate ? 0 : 1);
    char *data      = znalloc(a, char, n);
    if (len) memcpy(data, s, len);
    if (!dontNullTerminate) data[len] = '\0';

    ZMIRValueRef v  = constant(
        a, ZMIRConstantDataArrayValueKind,
        arrayType(a, ZMIRInt8TypeInContext(ctx), n)
    );

    v->str.data = data;
    v->str.len  = len;
    return v;
}

ZMIRValueRef ZMIRConstStructInContext(ZMIRContextRef ctx, ZMIRValueRef *vals, usize len, bool packed) {
    Allocator *a = ctx->allocator;
    ZMIRTypeRef *fields = znalloc(a, ZMIRTypeRef, len);

    for (usize i = 0; i < len; i++) fields[i] = vals[i]->type;

    ZMIRValueRef v = constant(
        a, ZMIRConstantStructValueKind,
        ZMIRStructTypeInContext(ctx, fields, len, packed)
    );
    v->aggregate = copyValues(a, vals, len);
    return v;
}

ZMIRValueRef ZMIRConstNamedStruct(ZMIRTypeRef t, ZMIRValueRef *vals, usize len) {
    if (!CHECK_KIND(t, ZMIRStructTypeKind)) return NULL;
    ZMIRValueRef v  = constant(arenaAllocator, ZMIRConstantStructValueKind, t);
    v->aggregate    = copyValues(arenaAllocator, vals, len);
    return v;
}

ZMIRValueRef ZMIRConstArray2(ZMIRTypeRef t, ZMIRValueRef *vals, usize len) {
    ZMIRValueRef v = constant(
        arenaAllocator, ZMIRConstantArrayValueKind,
        arrayType(arenaAllocator, t, len)
    );
    v->aggregate = copyValues(arenaAllocator, vals, len);
    return v;
}

/* ==================Globals          ============*/

static ZMIRValueRef getNamedDecl(ZMIRModuleRef mod, ZMIRValueKind kind, const char *name) {
    if (!mod || !name) return NULL;
    for (usize i = 0; i < veclen(mod->decls); i++) {
        ZMIRValueRef d = mod->decls[i];
        if (d->kind == kind && d->name && strcmp(d->name, name) == 0) return d;
    }
    return NULL;
}

ZMIRValueRef ZMIRAddGlobal(ZMIRModuleRef mod, ZMIRTypeRef type, const char *name) {
    Allocator *a = mod->ctx->allocator;
    ZMIRValueRef global = ZMIRValueNew(a,
        .kind   = ZMIRGlobalVariableValueKind,
        .type   = ZMIRPointerTypeInContext(mod->ctx, 0),
        .name   = dupname(a, name),
        .global = {
            .valueType  = type,
            .linkage    = ZMIRExternalLinkage,
        }
    );
    vecpushwith(mod->decls, global, a);
    return global;
}

ZMIRValueRef ZMIRGetNamedGlobal(ZMIRModuleRef mod, const char *name) {
    return getNamedDecl(mod, ZMIRGlobalVariableValueKind, name);
}

/* ==================Functions        ============*/

void ZMIRAddAttribute(ZMIRValueRef v, const char *name) {
    if (!CHECK_KIND(v, ZMIRFunctionValueKind)) return;
    vecpush(v->fn.attrs, name);
}

ZMIRValueRef ZMIRAddFunction(ZMIRModuleRef mod, const char *name, ZMIRTypeRef type) {
    Allocator *a = mod->ctx->allocator;
    ZMIRValueRef func = ZMIRValueNew(a,
        .kind   = ZMIRFunctionValueKind,
        .type   = ZMIRPointerTypeInContext(mod->ctx, 0),
        .name   = dupname(a, name),
        .fn     = {
            .fnType     = type,
            .linkage    = ZMIRExternalLinkage,
            .mod        = mod,
        }
    );

    usize nparams = ZMIRCountParamTypes(type);
    vecreservewith(func->fn.params, nparams, a);
    for (usize i = 0; i < nparams; i++) {
        vecpushwith(func->fn.params, ZMIRValueNew(a,
            .kind   = ZMIRArgumentValueKind,
            .type   = type->func.args[i],
            .arg    = { func, i }
        ), a);
    }

    vecpushwith(mod->decls, func, a);
    return func;
}

ZMIRValueRef ZMIRGetNamedFunction(ZMIRModuleRef mod, const char *name) {
    return getNamedDecl(mod, ZMIRFunctionValueKind, name);
}

ZMIRValueRef ZMIRGetParam(ZMIRValueRef fn, usize index) {
    if (!CHECK_KIND(fn, ZMIRFunctionValueKind)) return NULL;
    return index < veclen(fn->fn.params) ? fn->fn.params[index] : NULL;
}

/* ==================Basic Blocks     ============*/

ZMIRBasicBlockRef ZMIRAppendBasicBlockInContext(ZMIRContextRef ctx, ZMIRValueRef fn, const char *name) {
    if (!CHECK_KIND(fn, ZMIRFunctionValueKind)) return NULL;
    ZMIRBasicBlockRef block = copy(ctx->allocator, ZMIRBasicBlock, ((ZMIRBasicBlock){
        .name   = dupname(ctx->allocator, name),
        .parent = fn,
    }));
    vecpushwith(fn->fn.blocks, block, ctx->allocator);
    return block;
}

static bool isTerminator(ZMIROpcode op) {
    switch (op) {
    case ZMIRRet:
    case ZMIRBr:
    case ZMIRSwitch:
    case ZMIRIndirectBr:
    case ZMIRInvoke:
    case ZMIRUnreachable:
    case ZMIRCallBr:
    case ZMIRResume:
    case ZMIRCleanupRet:
    case ZMIRCatchRet:
    case ZMIRCatchSwitch:
        return true;
    default:
        return false;
    }
}

bool ZMIRGetBasicBlockTerminator(ZMIRBasicBlockRef b) {
    if (!b || vecempty(b->instrs)) return false;
    return isTerminator(veclast(b->instrs)->instr.op);
}

/* ==================Builder          ============*/

void ZMIRPositionBuilderAtEnd(ZMIRBuilderRef b, ZMIRBasicBlockRef block) {
    b->block = block;
}

ZMIRBasicBlockRef ZMIRGetInsertBlock(ZMIRBuilderRef b) {
    return b ? b->block : NULL;
}

static ZMIRValueRef instr(ZMIRBuilderRef b, ZMIROpcode op, ZMIRTypeRef type, const char *name) {
    Allocator *a = b->ctx->allocator;
    ZMIRValueRef v = copy(a, ZMIRValue, ((ZMIRValue){
        .kind   = ZMIRInstructionValueKind,
        .type   = type,
        .name   = name && *name ? dupname(a, name) : NULL,
        .instr  = { .op = op, .parent = b->block }
    }));
    vecpushwith(b->block->instrs, v, a);
    return v;
}

ZMIRBuilderRef ZMIRCreateBuilderInContext(ZMIRContextRef ctx) {
    return copy(ctx->allocator, ZMIRBuilder, ((ZMIRBuilder){ .ctx = ctx }));
}

/* The module the builder is inserting into, through the current function. */
static ZMIRModuleRef builderModule(ZMIRBuilderRef b) {
    if (!b->block || !b->block->parent) return NULL;
    return b->block->parent->fn.mod;
}

ZMIRValueRef ZMIRBuildGlobalString(ZMIRBuilderRef b, const char *str, const char *name) {
    ZMIRModuleRef mod   = builderModule(b);
    if (!mod) return NULL;

    ZMIRValueRef init   = ZMIRConstStringInContext(b->ctx, str, strlen(str), false);
    ZMIRValueRef global = ZMIRAddGlobal(mod, init->type, name);
    ZMIRSetInitializer(global, init);
    ZMIRSetGlobalConstant(global, true);
    ZMIRSetLinkage(global, ZMIRPrivateLinkage);
    ZMIRSetUnnamedAddr(global, ZMIRGlobalUnnamedAddr);
    ZMIRSetAlignment(global, 1);
    return global;
}

/* ==================Instructions     ============*/

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
    ZMIRValueRef v      = instr(b, ZMIRAlloca, ZMIRPointerTypeInContext(b->ctx, 0), name);
    v->instr.auxType    = type;                     // allocated type
    return v;
}

/* Same operand order as LLVM: ops = [val, ptr]. */
ZMIRValueRef ZMIRBuildStore(ZMIRBuilderRef b, ZMIRValueRef val, ZMIRValueRef ptr) {
    ZMIRValueRef v = instr(b, ZMIRStore, NULL, NULL);
    vecpushwith(v->instr.ops, val, b->ctx->allocator);
    vecpushwith(v->instr.ops, ptr, b->ctx->allocator);
    return v;
}

ZMIRValueRef ZMIRBuildLoad2(ZMIRBuilderRef b, ZMIRTypeRef type, ZMIRValueRef value, const char *name) {
    ZMIRValueRef v = instr(b, ZMIRLoad, type, name);
    vecpushwith(v->instr.ops, value, b->ctx->allocator);
    return v;
}

ZMIRValueRef ZMIRBinaryInstr(ZMIRBuilderRef b, ZMIROpcode op, ZMIRValueRef lhs, ZMIRValueRef rhs, const char *name) {
    if (!lhs || !rhs) return NULL;
    if (!ZMIRTypeEqual(lhs->type, rhs->type)) return NULL;
    ZMIRValueRef v = instr(b, op, lhs->type, name);
    vecpushwith(v->instr.ops, lhs, b->ctx->allocator);
    vecpushwith(v->instr.ops, rhs, b->ctx->allocator);
    return v;
}

ZMIRValueRef ZMIRBuildICmp(ZMIRBuilderRef b, ZMIRIntPredicate predicate, ZMIRValueRef lhs, ZMIRValueRef rhs, const char *name) {
    ZMIRValueRef v = ZMIRBinaryInstr(b, ZMIRICmp, lhs, rhs, name);
    if (!v) return NULL;
    v->type        = ZMIRInt1TypeInContext(b->ctx);
    v->instr.ipred = predicate;
    return v;
}

ZMIRValueRef ZMIRBuildFCmp(ZMIRBuilderRef b, ZMIRRealPredicate predicate, ZMIRValueRef lhs, ZMIRValueRef rhs, const char *name) {
    ZMIRValueRef v = ZMIRBinaryInstr(b, ZMIRFCmp, lhs, rhs, name);
    if (!v) return NULL;
    v->type        = ZMIRInt1TypeInContext(b->ctx);
    v->instr.fpred = predicate;
    return v;
}

#define ZMIRBuildBinary(op)                                                                             \
ZMIRValueRef ZMIRBuild##op(ZMIRBuilderRef b, ZMIRValueRef lhs, ZMIRValueRef rhs, const char *name) {     \
    return ZMIRBinaryInstr(b, ZMIR##op, lhs, rhs, name);                                                 \
}

ZMIRBuildBinary(FAdd)
ZMIRBuildBinary(FSub)
ZMIRBuildBinary(FMul)
ZMIRBuildBinary(FDiv)
ZMIRBuildBinary(FRem)
ZMIRBuildBinary(Add)
ZMIRBuildBinary(Sub)
ZMIRBuildBinary(Mul)
ZMIRBuildBinary(UDiv)
ZMIRBuildBinary(SDiv)
ZMIRBuildBinary(URem)
ZMIRBuildBinary(SRem)
ZMIRBuildBinary(AShr)
ZMIRBuildBinary(LShr)
ZMIRBuildBinary(Shl)
ZMIRBuildBinary(Or)
ZMIRBuildBinary(Xor)
ZMIRBuildBinary(And)

#undef ZMIRBuildBinary

static ZMIRValueRef castInstr(ZMIRBuilderRef b, ZMIROpcode op,
    ZMIRValueRef val, ZMIRTypeRef dest, const char *name) {
    if (!val || !dest) return NULL;
    ZMIRValueRef v = instr(b, op, dest, name);
    vecpushwith(v->instr.ops, val, b->ctx->allocator);
    return v;
}

#define ZMIRBuildCast(op)                                                                               \
ZMIRValueRef ZMIRBuild##op(ZMIRBuilderRef b, ZMIRValueRef val, ZMIRTypeRef dest, const char *name) {    \
    return castInstr(b, ZMIR##op, val, dest, name);                                                     \
}

ZMIRBuildCast(Trunc)
ZMIRBuildCast(ZExt)
ZMIRBuildCast(SExt)
ZMIRBuildCast(FPTrunc)
ZMIRBuildCast(FPExt)
ZMIRBuildCast(FPToUI)
ZMIRBuildCast(FPToSI)
ZMIRBuildCast(UIToFP)
ZMIRBuildCast(SIToFP)
ZMIRBuildCast(PtrToInt)
ZMIRBuildCast(IntToPtr)
ZMIRBuildCast(BitCast)

#undef ZMIRBuildCast

ZMIRValueRef ZMIRBuildZExtOrBitCast(ZMIRBuilderRef b, ZMIRValueRef val, ZMIRTypeRef dest, const char *name) {
    if (!val || !dest) return NULL;
    if (ZMIRGetIntTypeWidth(val->type) != ZMIRGetIntTypeWidth(dest))
        return ZMIRBuildZExt(b, val, dest, name);
    return ZMIRBuildBitCast(b, val, dest, name);
}

ZMIRValueRef ZMIRBuildGEP2(ZMIRBuilderRef b, ZMIRTypeRef type, ZMIRValueRef ptr, ZMIRValueRef *geps, usize len, const char *name) {
    Allocator *a        = b->ctx->allocator;
    ZMIRValueRef v      = instr(
        b, ZMIRGetElementPtr, ZMIRPointerTypeInContext(b->ctx, 0), name
    );
    v->instr.auxType    = type;
    vecpushwith(v->instr.ops, ptr, a);
    for (usize i = 0; i < len; i++)
        vecpushwith(v->instr.ops, geps[i], a);
    return v;
}

ZMIRValueRef ZMIRBuildStructGEP2(ZMIRBuilderRef b, ZMIRTypeRef type, ZMIRValueRef ptr, usize index, const char * name) {
    return ZMIRBuildGEP2(b, type, ptr, (ZMIRValueRef[]){
        ZMIRConstInt(i32Type, 0, false),
        ZMIRConstInt(i32Type, index, false)
    }, 2, name);
}

ZMIRValueRef ZMIRBuildExtractValue(ZMIRBuilderRef b, ZMIRValueRef agg, usize index, const char *name) {
    if (!agg) return NULL;
    ZMIRTypeRef t = agg->type;
    ZMIRTypeRef field = NULL;

    switch (t->kind) {
    case ZMIRStructTypeKind:    field = ZMIRStructGetTypeAtIndex(t, index); break;
    case ZMIRArrayTypeKind:     field = t->array.elem;                      break;
    default:                    return NULL;
    }
    ZMIRValueRef v = instr(b, ZMIRExtractValue, field, name);
    v->instr.index = index;
    vecpushwith(v->instr.ops, agg, b->ctx->allocator);
    return v;
}

void ZMIRBuildMemSet(ZMIRBuilderRef b, ZMIRValueRef ptr, ZMIRValueRef val, usize len, usize align) {
    ZMIRValueRef v = instr(b, ZMIRMemSet, NULL, NULL);
    v->instr.builtin_memset.len   = len;
    v->instr.builtin_memset.align = align;
    vecpushwith(v->instr.ops, ptr, b->ctx->allocator);
    vecpushwith(v->instr.ops, val, b->ctx->allocator);
}

ZMIRValueRef ZMIRBuildCall2(ZMIRBuilderRef b, ZMIRTypeRef fnType, ZMIRValueRef fn, ZMIRValueRef *args, usize len, const char *name) {
    if (!CHECK_KIND(fnType, ZMIRFunctionTypeKind)) return NULL;
    Allocator *a = b->ctx->allocator;
    ZMIRValueRef v = instr(b, ZMIRCall, fnType->func.ret, name);
    v->instr.auxType = fnType;
    vecpushwith(v->instr.ops, fn, a);
    for (usize i = 0; i < len; i++)
        vecpushwith(v->instr.ops, args[i], a);
    return v;
}

ZMIRValueRef ZMIRBuildPhi(ZMIRBuilderRef b, ZMIRTypeRef type, const char *name) {
    return instr(b, ZMIRPHI, type, name);
}

void ZMIRAddIncoming(ZMIRValueRef phi, ZMIRValueRef *vals, ZMIRBasicBlockRef *blocks, usize len) {
    if (!CHECK_KIND(phi, ZMIRInstructionValueKind) || phi->instr.op != ZMIRPHI) return;
    Allocator *a = phi->instr.parent->parent->fn.mod->ctx->allocator;
    for (usize i = 0; i < len; i++) {
        vecpushwith(phi->instr.ops, vals[i], a);
        vecpushwith(phi->instr.blocks, blocks[i], a);
    }
}

void ZMIRBuildRet(ZMIRBuilderRef b, ZMIRValueRef val) {
    ZMIRValueRef v = instr(b, ZMIRRet, NULL, NULL);
    vecpushwith(v->instr.ops, val, b->ctx->allocator);
}

void ZMIRBuildRetVoid(ZMIRBuilderRef b) {
    instr(b, ZMIRRet, NULL, NULL);
}

void ZMIRBuildUnreachable(ZMIRBuilderRef b) {
    instr(b, ZMIRUnreachable, NULL, NULL);
}

/* ==================Intrinsics       ============*/

/* Index + 1 is the intrinsic ID; 0 means "not an intrinsic", like LLVM. */
static const char *intrinsics[] = {
    "llvm.trap",
    "llvm.debugtrap",
    "llvm.ubsantrap",
};

u32 ZMIRLookupIntrinsicID(const char *name, usize len) {
    for (u32 i = 0; i < sizeof(intrinsics) / sizeof(*intrinsics); i++) {
        if (strlen(intrinsics[i]) == len && strncmp(intrinsics[i], name, len) == 0)
            return i + 1;
    }
    return 0;
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

/* None of the supported intrinsics are overloaded, so the overload types are ignored. */
ZMIRValueRef ZMIRGetIntrinsicDeclaration(ZMIRModuleRef mod, u32 id, ZMIRTypeRef *types, usize len) {
    (void)types; (void)len;
    if (id == 0 || id > sizeof(intrinsics) / sizeof(*intrinsics)) return NULL;

    const char *name = intrinsics[id - 1];
    ZMIRValueRef fn  = ZMIRGetNamedFunction(mod, name);
    if (fn) return fn;

    ZMIRTypeRef voidType = ZMIRVoidTypeInContext(mod->ctx);
    ZMIRTypeRef fnType   = strcmp(name, "llvm.ubsantrap") == 0
        ? ZMIRFunctionType(voidType, (ZMIRTypeRef[]){ ZMIRInt8TypeInContext(mod->ctx) }, 1, false)
        : ZMIRFunctionType(voidType, NULL, 0, false);
    return ZMIRAddFunction(mod, name, fnType);
}
