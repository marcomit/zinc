#include "zinc.h"
#include "zgen.h"
#include "zlink.h"

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

bool genBuiltin(ZCodegen *, ZNode *, ZMIRValueRef *);

typedef ZMIRValueRef (*ZBuiltinFn) (ZCodegen *, ZNode *);
typedef ZType *(*ZValidateFn) (ZState *, ZNode *);

static ZMIRValueRef genPanic(ZCodegen *ctx, ZNode *call) {
    if (veclen(call->call.args) != 1) {
        zlog(ctx->state, call->tok, Z4020, veclen(call->call.args));
    }

    ZMIRValueRef msg = genExpr(ctx, call->call.args[0]);
    emitRuntimePanic(ctx, call->tok, msg);
    ZMIRBuildTrap(ctx->builder);
    return ZMIRConstNull(i0Type);
}

typedef struct {
    ZCodegen    *ctx;

    ZType       *typeInfo;
    ZType       *typeStructField;
    ZType       *typeEnumVariant;

    ZMIRTypeRef llvmInfo;
    ZMIRTypeRef llvmStructField;
    ZMIRTypeRef llvmEnumVariant;
} ZLLVMGenTypeInfo;

static ZMIRValueRef _genTypeType(ZLLVMGenTypeInfo *, ZType *);

static void putTypeInfo(ZCodegen *ctx, ZType *type, ZMIRValueRef value) {
    vecpush(ctx->typeInfos, ((ZLLVMTypeInfo){ type, value }));
}

static ZMIRValueRef getTypeInfo(ZCodegen *ctx, ZType *type) {
    for (usize i = 0; i < veclen(ctx->typeInfos); i++) {
        if (typesEqual(ctx->typeInfos[i].type, type))
            return ctx->typeInfos[i].value;
    }
    return NULL;
}

static ZMIRTypeRef typeInfoEnumVariant(ZCodegen *ctx, ZType *enm, int index) {
    return genType(ctx, enm->enm.fields[index]->resolved);
}

static int typeInfoEnum(ZType *type, const char *variant) {
    if (!type || !variant) return -1;
    for (usize i = 0; i < veclen(type->enm.fields); i++)
        if (strcmp(stoken(type->enm.fields[i]->tok), variant) == 0)
            return (int)i;
    return -1;
}

static inline void genTypePrimitiveVariantName(ZType *type, const char **name) {
    ZToken *tok = type->primitive.token;
    switch (tok->type) {
    case TOK_F32:   *name = "Float";     break;
    case TOK_F64:   *name = "Double";    break;
    case TOK_VOID:  *name = "Void";      break;
    case TOK_BOOL:  *name = "Bool";      break;
    default:
        if (tokmask(tok, TOK_SIGNED | TOK_UNSIGNED))
            *name = "Integer";
        break;
    }
}

static void genTypePrimitive(ZLLVMGenTypeInfo *ctx, ZMIRValueRef **fields, ZMIRTypeRef variant, ZType *type) {
    ZToken *tok = type->primitive.token;
    switch (tok->type) {
    case TOK_VOID:
    case TOK_BOOL:
    case TOK_F32:
    case TOK_F64:
        break;
    default:
        if (!tokmask(tok, TOK_SIGNED | TOK_UNSIGNED)) return;
        bool isSigned = tokmask(tok, TOK_SIGNED);
        usize bits = typeSize(ctx->ctx->state, type) * 8;

        vecpush(*fields, ZMIRConstInt(ZMIRStructGetTypeAtIndex(variant, 1), bits, false));
        vecpush(*fields, ZMIRConstInt(ZMIRStructGetTypeAtIndex(variant, 2), isSigned, false));
        break;
    }
}

static ZMIRValueRef genTypeInfoGlobal(ZLLVMGenTypeInfo *ctx, ZMIRValueRef init, const char *name) {
    ZMIRValueRef g = ZMIRAddGlobal(ctx->ctx->mod, ZMIRTypeOf(init), name);
    ZMIRSetInitializer(g, init);
    ZMIRSetGlobalConstant(g, true);
    ZMIRSetLinkage(g, ZMIRPrivateLinkage);
    ZMIRSetUnnamedAddr(g, ZMIRGlobalUnnamedAddr);
    return g;
}

static ZMIRValueRef genTypeInfoStr(ZLLVMGenTypeInfo *ctx, ZToken *tok) {
    return ZMIRGetInitializer(genStrLitGlobal(ctx->ctx, tok));
}

static ZMIRValueRef genTypeInfoSlice(ZLLVMGenTypeInfo *ctx, ZMIRTypeRef sliceT,
                                     ZMIRTypeRef elemT, ZMIRValueRef *elems) {
    usize n = veclen(elems);
    ZMIRValueRef data;
    if (n == 0) data = ZMIRConstPointerNull(ptrType);
    else        data = genTypeInfoGlobal(ctx, ZMIRConstArray2(elemT, elems, n), "typeinfo.arr");

    ZMIRValueRef parts[] = {
        ZMIRConstInt(usizeType, n, false),
        data
    };
    return ZMIRIsLiteralStruct(sliceT)
        ? ZMIRConstStructInContext(ctx->ctx->ctx, parts, 2, false)
        : ZMIRConstNamedStruct(sliceT, parts, 2);
}

static ZMIRValueRef genTypeInfoField(ZLLVMGenTypeInfo *ctx, ZToken *tok, ZType *type) {
    ZMIRValueRef parts[] = {
        genTypeInfoStr(ctx, tok),
        _genTypeType(ctx, type)
    };
    if (!parts[1]) parts[1] = ZMIRConstPointerNull(ptrType);
    return ZMIRConstNamedStruct(ctx->llvmStructField, parts, 2);
}

static ZMIRValueRef genTypeInfoFields(ZLLVMGenTypeInfo *ctx, ZMIRTypeRef sliceT, ZNode **fields, usize skip) {
    ZMIRValueRef *elems = NULL;
    for (usize i = skip; i < veclen(fields); i++) {
        ZNode *field = fields[i];
        vecpush(elems, genTypeInfoField(ctx, field->field.identifier, field->resolved));
    }
    return genTypeInfoSlice(
        ctx, sliceT, ctx->llvmStructField, elems
    );
}

static ZMIRValueRef genTypeInfoTypes(ZLLVMGenTypeInfo *ctx, ZMIRTypeRef sliceT, ZType **types) {
    ZMIRValueRef *elems = NULL;
    for (usize i = 0; i < veclen(types); i++) {
        ZMIRValueRef t = _genTypeType(ctx, types[i]);
        if (!t) t = ZMIRConstPointerNull(ptrType);
        vecpush(elems, t);
    }
    return genTypeInfoSlice(
        ctx, sliceT, ptrType, elems
    );
}

static ZMIRValueRef genTypeInfoVariants(ZLLVMGenTypeInfo *ctx, ZMIRTypeRef sliceT, ZType *enm) {
    ZMIRTypeRef elemT   = ZMIRStructGetTypeAtIndex(ctx->llvmEnumVariant, 1);
    ZMIRValueRef *elems = NULL;
    for (usize i = 0; i < veclen(enm->enm.fields); i++) {
        ZType *variant = enm->enm.fields[i]->resolved;
        ZMIRValueRef parts[] = {
            genTypeInfoStr(ctx, variant->strct.name),
            genTypeInfoFields(ctx, elemT, variant->strct.fields, 1)
        };
        vecpush(elems, ZMIRConstNamedStruct(ctx->llvmEnumVariant, parts, 2));
    }

    return genTypeInfoSlice(ctx, sliceT, ctx->llvmEnumVariant, elems);
}

static ZMIRValueRef genTypeInfoNodes(ZLLVMGenTypeInfo *ctx, ZMIRTypeRef sliceT, ZNode **nodes) {
    ZMIRTypeRef elemT   = ZMIRStructGetTypeAtIndex(ctx->llvmEnumVariant, 1);
    ZMIRValueRef *elems = NULL;

    for (usize i = 0; i < veclen(nodes); i++) {
        ZMIRValueRef field = genTypeInfoField(
            ctx, nodes[i]->funcDef.name, nodes[i]->resolved
        );
        vecpush(elems, field);
    }

    return genTypeInfoSlice(ctx, sliceT, elemT, elems);
}

static void genTypePayload(ZLLVMGenTypeInfo *ctx, ZMIRValueRef **fields, ZMIRTypeRef variant, ZType *type) {
    #define SLOT(i) ZMIRStructGetTypeAtIndex(variant, (i) + 1)
    switch (type->kind) {
    case Z_TYPE_NAMESPACE:
    case Z_TYPE_GENERIC:
    case Z_TYPE_NONE:
    case Z_TYPE_COUNT:
        break;
    case Z_TYPE_ARRAY:
        vecpush(*fields, _genTypeType(ctx, type->array.base));
        break;
    case Z_TYPE_ENUM:
        vecpush(*fields, genTypeInfoStr(ctx, type->enm.name));
        vecpush(*fields, genTypeInfoVariants(ctx, SLOT(1), type));
        break;
    case Z_TYPE_STRUCT:
        vecpush(*fields, genTypeInfoStr(ctx, type->strct.name));
        vecpush(*fields, genTypeInfoFields(ctx, SLOT(1), type->strct.fields, 0));
        break;
    case Z_TYPE_FACET:
        vecpush(*fields, genTypeInfoStr(ctx, type->facet.name));
        vecpush(*fields, genTypeInfoNodes(ctx, SLOT(1), type->facet.funcs));
        break;
    case Z_TYPE_POINTER:
        vecpush(*fields, _genTypeType(ctx, type->base));
        break;
    case Z_TYPE_FUNCTION:
        vecpush(*fields, _genTypeType(ctx, type->func.ret));
        vecpush(*fields, genTypeInfoTypes(ctx, SLOT(1), type->func.args));
        break;
    case Z_TYPE_TUPLE:
        vecpush(*fields, genTypeInfoTypes(ctx, SLOT(0), type->tuple));
        break;
    case Z_TYPE_SUM:
        vecpush(*fields, genTypeInfoTypes(ctx, SLOT(0), type->sumType));
        break;
    case Z_TYPE_OPTIONAL:
        vecpush(*fields, _genTypeType(ctx, type->optional));
        break;
    case Z_TYPE_RESULT:
        vecpush(*fields, _genTypeType(ctx, type->result.success));
        vecpush(*fields, _genTypeType(ctx, type->result.error));
        break;
    case Z_TYPE_TYPE:
        break;
    case Z_TYPE_PRIMITIVE:
        genTypePrimitive(ctx, fields, variant, type);
    }
}

static ZMIRValueRef _genTypeType(ZLLVMGenTypeInfo *ctx, ZType *type) {
    ZMIRValueRef cached     = getTypeInfo(ctx->ctx, type);
    if (cached) return cached;
    const char *name        = NULL;
    switch (type->kind) {
    case Z_TYPE_NAMESPACE:
    case Z_TYPE_GENERIC:
    case Z_TYPE_NONE:
    case Z_TYPE_COUNT:      name = NULL;        break;
    case Z_TYPE_ARRAY:      name = "Array";     break;
    case Z_TYPE_ENUM:       name = "Enum";      break;
    case Z_TYPE_FACET:      name = "Facet";     break;
    case Z_TYPE_POINTER:    name = "Pointer";   break;
    case Z_TYPE_STRUCT:     name = "Struct";    break;
    case Z_TYPE_FUNCTION:   name = "Func";      break;
    case Z_TYPE_TUPLE:      name = "Tuple";     break;
    case Z_TYPE_SUM:        name = "Sum";       break;
    case Z_TYPE_OPTIONAL:   name = "Optional";  break;
    case Z_TYPE_RESULT:     name = "Result";    break;
    case Z_TYPE_TYPE:       name = "Type";      break;
    case Z_TYPE_PRIMITIVE:
        genTypePrimitiveVariantName(type, &name);
        break;
    }

    if (!name) {
        zlog(ctx->ctx->state, type->tok, Z9040, stype(type));
        return NULL;
    }
    int index = typeInfoEnum(ctx->typeInfo, name);
    if (index == -1) {
        zlog(ctx->ctx->state, type->tok, Z903F, name);
        return NULL;
    }

    ZMIRTypeRef variantT    = typeInfoEnumVariant(ctx->ctx, ctx->typeInfo, index);
    ZMIRTargetDataRef td    = ZMIRGetModuleDataLayout(ctx->ctx->mod);
    usize enumSize          = ZMIRABISizeOfType(td, ctx->llvmInfo);
    usize variantSize       = ZMIRABISizeOfType(td, variantT);
    usize pad               = enumSize > variantSize ? enumSize - variantSize : 0;
    ZMIRTypeRef parts[2]    = { variantT, NULL };
    int n                   = 1;
    if (pad > 0) parts[n++] = ZMIRArrayType2(i8Type, pad);
    ZMIRTypeRef wrapT       = ZMIRStructTypeInContext(ctx->ctx->ctx, parts, n, false);

    ZMIRValueRef global     = ZMIRAddGlobal(ctx->ctx->mod, wrapT, "typeinfo.");
    putTypeInfo(ctx->ctx, type, global);

    ZMIRTypeRef tagType     = ZMIRStructGetTypeAtIndex(ctx->llvmInfo, 0);
    ZMIRValueRef tag        = ZMIRConstInt(tagType, (unsigned)index, false);
    ZMIRValueRef *fields    = NULL;
    vecpush(fields, tag);

    genTypePayload(ctx, &fields, variantT, type);

    unsigned want           = ZMIRCountStructElementTypes(variantT);
    for (unsigned i = 0; i < want; i++) {
        ZMIRTypeRef ft = ZMIRStructGetTypeAtIndex(variantT, i);
        if (i >= veclen(fields))    vecpush(fields, ZMIRConstNull(ft));
        else if (!fields[i])        fields[i] = ZMIRConstNull(ft);
    }

    ZMIRValueRef value      = ZMIRConstNamedStruct(variantT, fields, want);
    ZMIRValueRef partVal[2] = { value, NULL };
    if (n == 2) partVal[1]  = ZMIRConstNull(ZMIRStructGetTypeAtIndex(wrapT, 1));
    ZMIRSetInitializer(global, ZMIRConstStructInContext(ctx->ctx->ctx, partVal, n, false));
    ZMIRSetGlobalConstant(global, true);
    ZMIRSetLinkage(global, ZMIRPrivateLinkage);
    unsigned enumAlign      = ZMIRABIAlignmentOfType(td, ctx->llvmInfo);
    unsigned variantAlign   = ZMIRABIAlignmentOfType(td, variantT);
    ZMIRSetAlignment(global, enumAlign > variantAlign ? enumAlign : variantAlign);

    return global;
}

ZMIRValueRef genTypeType(ZCodegen *ctx, ZNode *node) {
    ZNode *info         = LangItems[Z_LANG_TYPE_INFO];
    ZNode *structField  = LangItems[Z_LANG_TYPE_INFO_STRUCT_FIELD];
    ZNode *enumVariant  = LangItems[Z_LANG_TYPE_INFO_ENUM_VARIANT];
    if (!info || !structField || !enumVariant) return NULL;
    ZLLVMGenTypeInfo gen = {
        .ctx                = ctx,
        .typeInfo           = info->resolved,
        .typeStructField    = structField->resolved,
        .typeEnumVariant    = enumVariant->resolved,

        .llvmInfo           = genType(ctx, info->resolved),
        .llvmStructField    = genType(ctx, structField->resolved),
        .llvmEnumVariant    = genType(ctx, enumVariant->resolved),
    };
    return _genTypeType(&gen, node->resolved->typeType);
}

static ZMIRValueRef genReflect(ZCodegen *ctx, ZNode *node) {
    (void)ctx;
    (void)node;
    return NULL;
}

static ZMIRValueRef genHere(ZCodegen *ctx, ZNode *node) {
    ZNode *sourceLocType = LangItems[Z_LANG_SOURCE_LOCATION_TYPE];
    if (!sourceLocType) {
        zlog(ctx->state, node->tok, Z4021);
        return NULL;
    }

    ZType *type = sourceLocType->resolved;
    return ZMIRConstNull(genType(ctx, type));
}

static ZMIRValueRef genVolatileStore(ZCodegen *ctx, ZNode *node) {
    ZNode *base = node->call.args[0];
    ZNode *val  = node->call.args[1];
    ZMIRValueRef store = ZMIRBuildStore(
        ctx->builder, genExpr(ctx, val), genExpr(ctx, base)
    );
    ZMIRSetVolatile(store, true);
    return NULL;
}

static ZMIRValueRef genVolatileLoad(ZCodegen *ctx, ZNode *node) {
    ZMIRTypeRef typeRef = genType(ctx, node->resolved);
    ZMIRValueRef load = ZMIRBuildLoad2(
        ctx->builder, typeRef, genExpr(ctx, node->call.args[0]), label(ctx, "volatile.load")
    );
    ZMIRSetVolatile(load, true);
    return load;
}

static ZMIRValueRef genPtrOffset(ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef ptr    = genExpr(ctx, node->call.args[0]);
    ZMIRValueRef offset = genExpr(ctx, node->call.args[1]);

    ZType *ptrType      = node->call.args[0]->resolved;
    ZType *pointeeType  = ptrType;
    if (ptrType && ptrType->kind == Z_TYPE_POINTER) {

        pointeeType = ptrType->base;
    }
    ZMIRTypeRef type    = genType(ctx, pointeeType);

    return ZMIRBuildGEP2(
        ctx->builder, type, ptr, &offset, 1, label(ctx, "builtin.ptr_offset")
    );
}

static ZMIRValueRef genPtrToInt(ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef ptr = genExpr(ctx, node->call.args[0]);
    return ZMIRBuildPtrToInt(
        ctx->builder, ptr, i64Type, label(ctx, "builtin.ptr_to_int")
    );
}

static ZMIRValueRef genPtrFromInt(ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef ptr = genExpr(ctx, node->call.args[0]);
    return ZMIRBuildIntToPtr(
        ctx->builder, ptr, ZMIRPointerTypeInContext(ctx->ctx, 0), label(ctx, "builtin.ptr_from_int")
    );
}

static ZMIRValueRef genEnumTagConst(ZCodegen *ctx, ZNode *enumNode, u64 tag) {
    if (!enumNode) return NULL;
    ZMIRTypeRef enumType        = genType(ctx, enumNode->resolved);
    unsigned count              = ZMIRCountStructElementTypes(enumType);
    ZMIRValueRef fields[2]      = {
        ZMIRConstInt(ZMIRStructGetTypeAtIndex(enumType, 0), tag, false),
        count > 1 ? ZMIRConstNull(ZMIRStructGetTypeAtIndex(enumType, 1)) : NULL,
    };
    return ZMIRConstNamedStruct(enumType, fields, count);
}

static ZMIRValueRef genTargetOS(ZCodegen *ctx, ZNode *node) {
    (void)node;
    ZBuiltinOS os               = ztripleOS(ctx->state->resolvedTriple);
    return genEnumTagConst(ctx, LangItems[Z_LANG_TARGET_OS_TYPE], os);
}

static ZMIRValueRef genTargetArch(ZCodegen *ctx, ZNode *node) {
    (void)node;
    ZBuiltinArch arch           = ztripleArch(ctx->state->resolvedTriple);
    return genEnumTagConst(ctx, LangItems[Z_LANG_TARGET_ARCH_TYPE], arch);
}

#define ensure(cond, tok, ...) if (!(cond)) {                               \
    zlog(state, tok, __VA_ARGS__);                                          \
    return;                                                                 \
}

ZType *LangExpectedType[Z_LANG_COUNT] = { NULL };

ZBuiltinFn LangBuiltins[Z_LANG_COUNT] = {
    #define LANG(id, name, type, func, valid) [id] = func,
    #include "zlang.def"
    #undef LANG
};

ZValidateFn LangValidators[Z_LANG_COUNT] = {
    #define LANG(id, name, type, func, valid) [id] = valid,
    #include "zlang.def"
    #undef LANG
};

bool genBuiltin(ZCodegen *ctx, ZNode *node, ZMIRValueRef *out) {
    ZNode *langNode = node;
    if (node->type == NODE_CALL) langNode = node->call.callee;

    ZLangItemType li = getLangItemType(langNode);
    if (!li || !LangBuiltins[li]) return false;

    ZMIRValueRef result = LangBuiltins[li](ctx, node);
    if (out) *out = result;
    return true;
}

void validate(ZState *ctx, ZNode *node) {
    ZLangItemType li = getLangItemType(node);
    if (!LangValidators[li]) return;
    if (!LangExpectedType[li]) {
        LangExpectedType[li] = LangValidators[li](ctx, node);
    }

    if (!typesEqual(node->resolved, LangExpectedType[li])) {
        zlog(
            ctx, node->tok, Z4022,
            stype(LangExpectedType[li]), stype(node->resolved)
        );
    }
}
