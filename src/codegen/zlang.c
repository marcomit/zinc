#include "zinc.h"
#include "zgen.h"
#include "zlink.h"

extern _Thread_local LLVMTypeRef i0Type;
extern _Thread_local LLVMTypeRef i1Type;
extern _Thread_local LLVMTypeRef i8Type;
extern _Thread_local LLVMTypeRef i16Type;
extern _Thread_local LLVMTypeRef i32Type;
extern _Thread_local LLVMTypeRef i64Type;
extern _Thread_local LLVMTypeRef f32Type;
extern _Thread_local LLVMTypeRef f64Type;
extern _Thread_local LLVMTypeRef ptrType;
extern _Thread_local LLVMTypeRef strType;
extern _Thread_local LLVMTypeRef usizeType;
extern _Thread_local LLVMTypeRef isizeType;

extern ZNode *LangItems[Z_LANG_COUNT];

bool genBuiltin(ZCodegen *, ZNode *, LLVMValueRef *);

typedef LLVMValueRef (*ZBuiltinFn) (ZCodegen *, ZNode *);
typedef ZType *(*ZValidateFn) (ZState *, ZNode *);

static LLVMValueRef genPanic(ZCodegen *ctx, ZNode *call) {
    if (veclen(call->call.args) != 1) {
        zlog(ctx->state, call->tok, Z4020, veclen(call->call.args));
    }

    LLVMValueRef msg = genExpr(ctx, call->call.args[0]);
    emitRuntimePanic(ctx, call->tok, msg);
    LLVMBuildTrap(ctx);
    return LLVMConstNull(i0Type);
}

typedef struct {
    ZCodegen    *ctx;

    ZType       *typeInfo;
    ZType       *typeStructField;
    ZType       *typeEnumVariant;

    LLVMTypeRef llvmInfo;
    LLVMTypeRef llvmStructField;
    LLVMTypeRef llvmEnumVariant;
} ZLLVMGenTypeInfo;

static LLVMValueRef _genTypeType(ZLLVMGenTypeInfo *, ZType *);

static void putTypeInfo(ZCodegen *ctx, ZType *type, LLVMValueRef value) {
    vecpush(ctx->typeInfos, ((ZLLVMTypeInfo){ type, value }));
}

static LLVMValueRef getTypeInfo(ZCodegen *ctx, ZType *type) {
    for (usize i = 0; i < veclen(ctx->typeInfos); i++) {
        if (typesEqual(ctx->typeInfos[i].type, type))
            return ctx->typeInfos[i].value;
    }
    return NULL;
}

static LLVMTypeRef typeInfoEnumVariant(ZCodegen *ctx, ZType *enm, int index) {
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

static void genTypePrimitive(ZLLVMGenTypeInfo *ctx, LLVMValueRef **fields, LLVMTypeRef variant, ZType *type) {
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

        vecpush(*fields, LLVMConstInt(LLVMStructGetTypeAtIndex(variant, 1), bits, false));
        vecpush(*fields, LLVMConstInt(LLVMStructGetTypeAtIndex(variant, 2), isSigned, false));
        break;
    }
}

static LLVMValueRef genTypeInfoGlobal(ZLLVMGenTypeInfo *ctx, LLVMValueRef init, const char *name) {
    LLVMValueRef g = LLVMAddGlobal(ctx->ctx->mod, LLVMTypeOf(init), name);
    LLVMSetInitializer(g, init);
    LLVMSetGlobalConstant(g, true);
    LLVMSetLinkage(g, LLVMPrivateLinkage);
    LLVMSetUnnamedAddr(g, LLVMGlobalUnnamedAddr);
    return g;
}

static LLVMValueRef genTypeInfoStr(ZLLVMGenTypeInfo *ctx, ZToken *tok) {
    return LLVMGetInitializer(genStrLitGlobal(ctx->ctx, tok));
}

static LLVMValueRef genTypeInfoSlice(ZLLVMGenTypeInfo *ctx, LLVMTypeRef sliceT,
                                     LLVMTypeRef elemT, LLVMValueRef *elems) {
    usize n = veclen(elems);
    LLVMValueRef data;
    if (n == 0) data = LLVMConstPointerNull(ptrType);
    else        data = genTypeInfoGlobal(ctx, LLVMConstArray2(elemT, elems, n), "typeinfo.arr");

    LLVMValueRef parts[] = {
        LLVMConstInt(usizeType, n, false),
        data
    };
    return LLVMIsLiteralStruct(sliceT)
        ? LLVMConstStructInContext(ctx->ctx->ctx, parts, 2, false)
        : LLVMConstNamedStruct(sliceT, parts, 2);
}

static LLVMValueRef genTypeInfoField(ZLLVMGenTypeInfo *ctx, ZToken *tok, ZType *type) {
    LLVMValueRef parts[] = {
        genTypeInfoStr(ctx, tok),
        _genTypeType(ctx, type)
    };
    if (!parts[1]) parts[1] = LLVMConstPointerNull(ptrType);
    return LLVMConstNamedStruct(ctx->llvmStructField, parts, 2);
}

static LLVMValueRef genTypeInfoFields(ZLLVMGenTypeInfo *ctx, LLVMTypeRef sliceT, ZNode **fields, usize skip) {
    LLVMValueRef *elems = NULL;
    for (usize i = skip; i < veclen(fields); i++) {
        ZNode *field = fields[i];
        vecpush(elems, genTypeInfoField(ctx, field->field.identifier, field->resolved));
    }
    return genTypeInfoSlice(
        ctx, sliceT, ctx->llvmStructField, elems
    );
}

static LLVMValueRef genTypeInfoTypes(ZLLVMGenTypeInfo *ctx, LLVMTypeRef sliceT, ZType **types) {
    LLVMValueRef *elems = NULL;
    for (usize i = 0; i < veclen(types); i++) {
        LLVMValueRef t = _genTypeType(ctx, types[i]);
        if (!t) t = LLVMConstPointerNull(ptrType);
        vecpush(elems, t);
    }
    return genTypeInfoSlice(
        ctx, sliceT, ptrType, elems
    );
}

static LLVMValueRef genTypeInfoVariants(ZLLVMGenTypeInfo *ctx, LLVMTypeRef sliceT, ZType *enm) {
    LLVMTypeRef elemT   = LLVMStructGetTypeAtIndex(ctx->llvmEnumVariant, 1);
    LLVMValueRef *elems = NULL;
    for (usize i = 0; i < veclen(enm->enm.fields); i++) {
        ZType *variant = enm->enm.fields[i]->resolved;
        LLVMValueRef parts[] = {
            genTypeInfoStr(ctx, variant->strct.name),
            genTypeInfoFields(ctx, elemT, variant->strct.fields, 1)
        };
        vecpush(elems, LLVMConstNamedStruct(ctx->llvmEnumVariant, parts, 2));
    }

    return genTypeInfoSlice(ctx, sliceT, ctx->llvmEnumVariant, elems);
}

static LLVMValueRef genTypeInfoNodes(ZLLVMGenTypeInfo *ctx, LLVMTypeRef sliceT, ZNode **nodes) {
    LLVMTypeRef elemT   = LLVMStructGetTypeAtIndex(ctx->llvmEnumVariant, 1);
    LLVMValueRef *elems = NULL;

    for (usize i = 0; i < veclen(nodes); i++) {
        LLVMValueRef field = genTypeInfoField(
            ctx, nodes[i]->funcDef.name, nodes[i]->resolved
        );
        vecpush(elems, field);
    }

    return genTypeInfoSlice(ctx, sliceT, elemT, elems);
}

static void genTypePayload(ZLLVMGenTypeInfo *ctx, LLVMValueRef **fields, LLVMTypeRef variant, ZType *type) {
    #define SLOT(i) LLVMStructGetTypeAtIndex(variant, (i) + 1)
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

static LLVMValueRef _genTypeType(ZLLVMGenTypeInfo *ctx, ZType *type) {
    LLVMValueRef cached     = getTypeInfo(ctx->ctx, type);
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

    LLVMTypeRef variantT    = typeInfoEnumVariant(ctx->ctx, ctx->typeInfo, index);
    LLVMTargetDataRef td    = LLVMGetModuleDataLayout(ctx->ctx->mod);
    usize enumSize          = LLVMABISizeOfType(td, ctx->llvmInfo);
    usize variantSize       = LLVMABISizeOfType(td, variantT);
    usize pad               = enumSize > variantSize ? enumSize - variantSize : 0;
    LLVMTypeRef parts[2]    = { variantT, NULL };
    int n                   = 1;
    if (pad > 0) parts[n++] = LLVMArrayType2(i8Type, pad);
    LLVMTypeRef wrapT       = LLVMStructTypeInContext(ctx->ctx->ctx, parts, n, false);

    LLVMValueRef global     = LLVMAddGlobal(ctx->ctx->mod, wrapT, "typeinfo.");
    putTypeInfo(ctx->ctx, type, global);

    LLVMTypeRef tagType     = LLVMStructGetTypeAtIndex(ctx->llvmInfo, 0);
    LLVMValueRef tag        = LLVMConstInt(tagType, (unsigned)index, false);
    LLVMValueRef *fields    = NULL;
    vecpush(fields, tag);

    genTypePayload(ctx, &fields, variantT, type);

    unsigned want           = LLVMCountStructElementTypes(variantT);
    for (unsigned i = 0; i < want; i++) {
        LLVMTypeRef ft = LLVMStructGetTypeAtIndex(variantT, i);
        if (i >= veclen(fields))    vecpush(fields, LLVMConstNull(ft));
        else if (!fields[i])        fields[i] = LLVMConstNull(ft);
    }

    LLVMValueRef value      = LLVMConstNamedStruct(variantT, fields, want);
    LLVMValueRef partVal[2] = { value, NULL };
    if (n == 2) partVal[1]  = LLVMConstNull(LLVMStructGetTypeAtIndex(wrapT, 1));
    LLVMSetInitializer(global, LLVMConstStructInContext(ctx->ctx->ctx, partVal, n, false));
    LLVMSetGlobalConstant(global, true);
    LLVMSetLinkage(global, LLVMPrivateLinkage);
    unsigned enumAlign      = LLVMABIAlignmentOfType(td, ctx->llvmInfo);
    unsigned variantAlign   = LLVMABIAlignmentOfType(td, variantT);
    LLVMSetAlignment(global, enumAlign > variantAlign ? enumAlign : variantAlign);

    return global;
}

LLVMValueRef genTypeType(ZCodegen *ctx, ZNode *node) {
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

static LLVMValueRef genReflect(ZCodegen *ctx, ZNode *node) {
    (void)ctx;
    (void)node;
    return NULL;
}

static LLVMValueRef genHere(ZCodegen *ctx, ZNode *node) {
    ZNode *sourceLocType = LangItems[Z_LANG_SOURCE_LOCATION_TYPE];
    if (!sourceLocType) {
        zlog(ctx->state, node->tok, Z4021);
        return NULL;
    }

    ZType *type = sourceLocType->resolved;
    return LLVMConstNull(genType(ctx, type));
}

static LLVMValueRef genVolatileStore(ZCodegen *ctx, ZNode *node) {
    ZNode *base = node->call.args[0];
    ZNode *val  = node->call.args[1];
    LLVMValueRef store = LLVMBuildStore(
        ctx->builder, genExpr(ctx, val), genExpr(ctx, base)
    );
    LLVMSetVolatile(store, true);
    return NULL;
}

static LLVMValueRef genVolatileLoad(ZCodegen *ctx, ZNode *node) {
    LLVMTypeRef typeRef = genType(ctx, node->resolved);
    LLVMValueRef load = LLVMBuildLoad2(
        ctx->builder, typeRef, genExpr(ctx, node->call.args[0]), label(ctx, "volatile.load")
    );
    LLVMSetVolatile(load, true);
    return load;
}

static LLVMValueRef genPtrOffset(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr    = genExpr(ctx, node->call.args[0]);
    LLVMValueRef offset = genExpr(ctx, node->call.args[1]);

    ZType *ptrType      = node->call.args[0]->resolved;
    ZType *pointeeType  = ptrType;
    if (ptrType && ptrType->kind == Z_TYPE_POINTER) {

        pointeeType = ptrType->base;
    }
    LLVMTypeRef type    = genType(ctx, pointeeType);

    return LLVMBuildGEP2(
        ctx->builder, type, ptr, &offset, 1, label(ctx, "builtin.ptr_offset")
    );
}

static LLVMValueRef genPtrToInt(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr = genExpr(ctx, node->call.args[0]);
    return LLVMBuildPtrToInt(
        ctx->builder, ptr, i64Type, label(ctx, "builtin.ptr_to_int")
    );
}

static LLVMValueRef genPtrFromInt(ZCodegen *ctx, ZNode *node) {
    LLVMValueRef ptr = genExpr(ctx, node->call.args[0]);
    return LLVMBuildIntToPtr(
        ctx->builder, ptr, LLVMPointerTypeInContext(ctx->ctx, 0), label(ctx, "builtin.ptr_from_int")
    );
}

static LLVMValueRef genEnumTagConst(ZCodegen *ctx, ZNode *enumNode, u64 tag) {
    if (!enumNode) return NULL;
    LLVMTypeRef enumType        = genType(ctx, enumNode->resolved);
    unsigned count              = LLVMCountStructElementTypes(enumType);
    LLVMValueRef fields[2]      = {
        LLVMConstInt(LLVMStructGetTypeAtIndex(enumType, 0), tag, false),
        count > 1 ? LLVMConstNull(LLVMStructGetTypeAtIndex(enumType, 1)) : NULL,
    };
    return LLVMConstNamedStruct(enumType, fields, count);
}

static LLVMValueRef genTargetOS(ZCodegen *ctx, ZNode *node) {
    (void)node;
    ZBuiltinOS os               = ztripleOS(ctx->state->resolvedTriple);
    return genEnumTagConst(ctx, LangItems[Z_LANG_TARGET_OS_TYPE], os);
}

static LLVMValueRef genTargetArch(ZCodegen *ctx, ZNode *node) {
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

bool genBuiltin(ZCodegen *ctx, ZNode *node, LLVMValueRef *out) {
    ZNode *langNode = node;
    if (node->type == NODE_CALL) langNode = node->call.callee;

    ZLangItemType li = getLangItemType(langNode);
    if (!li || !LangBuiltins[li]) return false;

    LLVMValueRef result = LangBuiltins[li](ctx, node);
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
