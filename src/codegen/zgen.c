/**
 * @file zgen.c
 * @brief Code generator that emits ZMIR-IR
 *
 * It generates the ZMIR-IR from the AST.
 * It does a first pass where store all variable declarations
 * such that the ordering doesn't matter.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */

#include "zgen.h"
#include "zinc.h"
#include "zmem.h"

static void         genStmt             (ZCodegen *, ZNode *);
static void         genBlock            (ZCodegen *, ZNode *);
static void         storeArray          (ZCodegen *, ZMIRStack *, ZMIRValueRef);
static void         genFuncVars         (ZCodegen *, ZNode *);
static usize        addFuncArgs         (ZCodegen *, ZMIRValueRef, ZNode **, usize, bool);
static void         genNamespace        (ZCodegen *, ZNode *);
ZMIRTypeRef         genType             (ZCodegen *, ZType *);
ZMIRValueRef        genExpr             (ZCodegen *, ZNode *);
ZMIRValueRef        genLValue           (ZCodegen *, ZNode *);
static ZMIRValueRef genForeign          (ZCodegen *, ZNode *);
static ZMIRValueRef genStructLitInto    (ZCodegen *, ZNode *, ZMIRValueRef);
static ZMIRValueRef genFacetConstruct   (ZCodegen *, ZMIRStack *, ZType *, ZNode *);
static ZMIRValueRef genCond             (ZCodegen *, ZMIRValueRef, ZType *, ZPredicate);
static ZMIRValueRef genInterpolation    (ZCodegen *, ZNode *);
static ZMIRStack   *getStackValue      (ZCodegen *, ZNode *);
static void         buildNestedFuncVar  (ZCodegen *, ZNode *, ZMIRValueRef);
static void         storeInterpolation  (ZCodegen *, ZMIRValueRef, ZInterpolation *);

/* ========== Native types ==========*/

_Thread_local ZMIRTypeRef i0Type        = NULL;
_Thread_local ZMIRTypeRef i1Type        = NULL;
_Thread_local ZMIRTypeRef i8Type        = NULL;
_Thread_local ZMIRTypeRef i16Type       = NULL;
_Thread_local ZMIRTypeRef i32Type       = NULL;
_Thread_local ZMIRTypeRef i64Type       = NULL;
_Thread_local ZMIRTypeRef usizeType     = NULL;
_Thread_local ZMIRTypeRef ptrType       = NULL;
_Thread_local ZMIRTypeRef strType       = NULL;
_Thread_local ZMIRTypeRef f32Type       = NULL;
_Thread_local ZMIRTypeRef f64Type       = NULL;

extern ZNode *LangItems[Z_LANG_COUNT];

static ZMIRSymbol *makesymbol(ZCodegen *ctx) {
    ZMIRSymbol *self = aalloc(ctx->module->allocator, sizeof(ZMIRSymbol));
    return self;
}

static ZMIRScope *makescope(ZCodegen *ctx, int type, ZMIRScope *parent) {
    ZMIRScope *self    = aalloc(ctx->module->allocator, sizeof(ZMIRScope));
    *self               = (ZMIRScope){ 0 };
    self->parent        = parent;
    self->startLoop     = parent ? parent->startLoop : NULL;
    self->endLoop       = parent ? parent->endLoop : NULL;
    self->type          = type;

    return self;
}

static void beginScope(int type, ZCodegen *ctx) {
    ctx->scope = makescope(ctx, type, ctx->scope);
}

static void endScope(ZCodegen *ctx) {
    if (!ctx->scope) return;
    if (!ctx->scope->parent) return;
    ctx->scope = ctx->scope->parent;
}

/**
 * @brief Insert a pointer of the stack into the current scope.
 */
static void putZMIRValueRef(ZCodegen *ctx, char *key, ZMIRValueRef value) {
    ZMIRSymbol *symbol = makesymbol(ctx);
    symbol->name        = key;
    symbol->value       = value;
    symbol->isValue     = false;
    vecpush(ctx->scope->symbols, symbol);
}

/**
 * @brief Insert an arbitrary value into the current scope.
 */
static void putZMIRValue(ZCodegen *ctx, char *key, ZMIRValueRef value) {
    ZMIRSymbol *symbol = makesymbol(ctx);
    symbol->name        = key;
    symbol->value       = value;
    symbol->isValue     = true;
    vecpush(ctx->scope->symbols, symbol);
}

/**
 * @brief Returns the symbol saved in the scope's chain.
 *
 * Used in `getZMIRValueRef`
 */
static ZMIRSymbol *getZMIRSymbol(ZCodegen *ctx, const char *key) {
    if (!key) return NULL;
    ZMIRScope *current = ctx->scope;
    while (current) {
        usize len = veclen(current->symbols);
        for (usize i = 0; i < len; i++) {
            if (strcmp(current->symbols[i]->name, key) == 0) {
                return current->symbols[i];
            }
        }
        current = current->parent;
    }
    return NULL;
}

/**
 * @brief Returns the variable saved in the stack.
 *
 * It walks the symbols in the scope and go up while it reaches the highest scope.
 *
 * @see genIdent to see how it's used.
 */
static ZMIRValueRef getZMIRValueRef(ZCodegen *ctx, const char *key) {
    ZMIRSymbol *sym = getZMIRSymbol(ctx, key);
    return sym ? sym->value : NULL;
}

static ZMIRValueRef getCapabilityRef(ZCodegen *ctx, ZType *capability) {
    ZMIRScope *cur = ctx->scope;
    while (cur) {
        usize len = veclen(cur->capabilities);
        for (usize i = len; i-- > 0;) {
            if (typesEqual(cur->capabilities[i]->capability, capability)) {
                return cur->capabilities[i]->ref;
            }
        }
        cur = cur->parent;
    }
    return NULL;
}

/**
 * @brief Initialize ZMIR types
 *
 * Integer types are used everywhere in the code generation
 * so they are 'cached' and initialized only once
 * at the stack of the code generation.
 */
static void initNativeTypes(ZCodegen *ctx) {
    if (i0Type) return;
    i0Type      = ZMIRVoidTypeInContext(ctx->ctx);
    i1Type      = ZMIRInt1TypeInContext(ctx->ctx);
    i8Type      = ZMIRInt8TypeInContext(ctx->ctx);
    i16Type     = ZMIRInt16TypeInContext(ctx->ctx);
    i32Type     = ZMIRInt32TypeInContext(ctx->ctx);
    i64Type     = ZMIRInt64TypeInContext(ctx->ctx);

    f32Type     = ZMIRFloatTypeInContext(ctx->ctx);
    f64Type     = ZMIRDoubleTypeInContext(ctx->ctx);

    if (!ctx->state->pointerSize) {
        error(ctx->state, NULL, "Unable to load usize type");
    }
    usizeType   = ZMIRIntTypeInContext(ctx->ctx, ctx->state->pointerSize * 8);
    ptrType     = ZMIRPointerTypeInContext(ctx->ctx, 0);
    strType     = ZMIRStructTypeInContext(
        ctx->ctx, (ZMIRTypeRef[]){ usizeType, ptrType }, 2, false
    );

    // ZMIRSourceLocation = ZMIRStructCreateNamed(
    //         ctx->ctx, "builtin.SourceLocation"
    // );
    //
    // ZMIRStructSetBody(ZMIRSourceLocation,
    //     (ZMIRTypeRef[]){
    //         ZMIRPointerTypeInContext(ctx->ctx, 0),
    //         i64Type, i64Type
    //     }, 3, false);
}

static void beginModule(ZCodegen *ctx, ZNode *node) {
    /* Save the current ZMIR module so endModule can restore it.
       For the root module ctx->mod is NULL - create the one module
       that the whole compilation shares.  Imported modules reuse it. */
    ZMIRModuleRef prev = ctx->mod;
    if (!ctx->mod) {
        ctx->mod = ZMIRModuleCreateWithNameInContext(
            node->module.filename, ctx->ctx
        );
    }
    vecpush(ctx->modules, prev);
}

static void endModule(ZCodegen *ctx) {
    if (veclen(ctx->modules) == 0) {
        printf("Invalid call 'endModule'. The stack of modules is empty\n");
        return;
    }
    ZMIRModuleRef prev = vecpop(ctx->modules);
    /* Only restore if we actually had a parent module (non-root case).
       For the root the saved value is NULL; keep ctx->mod as-is. */
    if (prev) ctx->mod = prev;
}

ZCodegen *makecodegen(ZState *state, ZModuleAllocator *module) {
    ZCodegen *self      = zalloc(state->allocator, ZCodegen);
    self->ctx           = ZMIRContextCreate(state, module->allocator);
    self->builder       = ZMIRCreateBuilderInContext(self->ctx);

    self->mod           = NULL;
    self->modules       = NULL;
    self->structNames   = NULL;
    self->structTypes   = NULL;
    self->module        = module;
    self->scope         = makescope(self, Z_SCOPE_GLOB, NULL);
    self->state         = state;
    self->count         = 0;
    self->str           = NULL;
    self->seenModuleDecls = NULL;
    self->seenModuleDefs  = NULL;
    vecunion(self->str, "        ", 9);
    return self;
}

void labelCnt(ZCodegen *ctx) {
    LABEL_RESET(ctx);
    snprintf(ctx->str, 6, "zn%.3zx", ctx->count);
    ctx->count++;
}

/**
 * @brief Used to name ZMIR instructions.
 *
 * If the compiler is in dev mode it emits only
 * the prefix "zn" + a progressive counter in hex format.
 * If the dev mode is not enabled it emits the source string given by the token
 */
char *labelTok(ZCodegen *ctx, ZToken *tok) {
    LABEL_RESET(ctx);
    if (ctx->state->cli.debug && tok) {
        char *str = stoken(tok);
        vecunion(ctx->str, str, strlen(str));
        vecpush(ctx->str, 0);
    } else {
        labelCnt(ctx);
    }
    return ctx->str;
}

char *labelStr(ZCodegen *ctx, char *msg) {
    LABEL_RESET(ctx);
    if (ctx->state->cli.debug && msg) {
        return msg;
    }
    labelCnt(ctx);
    return ctx->str;
}

char *manglingIdent(ZNode *ident) {
    if (!ident                                  ||
         ident->type != NODE_IDENTIFIER         ||
        !ident->identNode.ref                   ) {
        return NULL;
    }

    ZNode *ref = ident->identNode.ref;
    switch (ref->type) {
    case NODE_FUNC:     return ref->funcDef.mangled;
    case NODE_FOREIGN:  return ref->foreignDecl.name->str;
    default:            return ident->identNode.tok->str;
    }
}

static usize typeAlign(ZState *, ZType *type);

static inline usize alignUp(usize size, usize align) {
    if (!align) return size;
    return (size + align - 1) / align * align;
}

/**
 * @brief Calculates padding of types.
 *
 * This function is used to calculate the padding of struct/tuple fields.
 * It is also used for enums.
 *
 * The result is rounded up to the alignment of the whole aggregate, matching
 * what ZMIR does: without that trailing padding 'sizeof' would disagree with
 * the stride used to index an array of that type.
 *
 * @param iter used to take the type from the current field.
 * @param fields must be a dynamic array because it uses veclen.
 * @param packed when true fields are laid out back to back, with no padding.
 */
static usize alignFields(ZState *state, void **fields, ZType *(*iter)(void *), bool packed) {
    usize res = 0;
    usize aggAlign = 1;

    for (usize i = 0; i < veclen(fields); i++) {
        ZType *field = iter(fields[i]);
        if (!packed) {
            usize align = typeAlign(state, field);
            if (align > aggAlign) aggAlign = align;
            res = alignUp(res, align);
        }
        res += typeSize(state, field);
    }

    return packed ? res : alignUp(res, aggAlign);
}

static inline ZType *alignStructFieldIter(void *item) {
    return ((ZNode *)item)->resolved;
}
static inline ZType *alignTupleFieldIter(void *item) { return (ZType *)item; }

#define getType(t) _Generic((t),                                                \
    ZNode *: ((ZNode *)(uintptr_t)(t))->resolved,                               \
    ZType *: ((ZType *)(uintptr_t)(t))                                          \
)

#define typeLargestType(fields) ({                                              \
    usize largest = 0;                                                          \
    usize len_ = veclen(fields);                                                \
    for (usize i = 0; i < len_; i++) {                                          \
        usize cur = typeSize(state, getType((fields)[i]));                      \
        if (cur > largest) largest = cur;                                       \
    }                                                                           \
    largest;                                                                    \
})

static inline usize typePrimitiveSize(ZState *state, ZToken *tok) {
    switch (tok->type) {
    case TOK_VOID:  return 0;
    case TOK_BOOL:
    case TOK_I8:
    case TOK_U8:
    case TOK_CHAR:  return 1;
    case TOK_I16:
    case TOK_U16:   return 2;
    case TOK_I32:
    case TOK_U32:
    case TOK_F32:   return 4;
    case TOK_I64:
    case TOK_U64:
    case TOK_F64:   return 8;
    case TOK_USIZE:
    case TOK_ISIZE: return state->pointerSize;
    default:        return 0;
    }
}

/**
 * @brief Calculates the size of the type.
 *
 * Primitives type have a constant size.
 * Structs and tuples are just the sum of their fields + the padding
 * calculated with alignFields.
 *
 * For enums it creates a struct with two fields:
 * - The flag that indicates the active variant and it is u8
 * - The buffer where its size is the size of the largest variant.
 */
usize typeSize(ZState *state, ZType *type) {
    usize res = 0;
    switch (type->kind) {
    case Z_TYPE_PRIMITIVE:  return typePrimitiveSize(state, type->primitive.token);
    case Z_TYPE_TYPE:
    case Z_TYPE_POINTER:
    case Z_TYPE_FUNCTION:   return state->pointerSize;
    case Z_TYPE_ARRAY: {
        /* 3 different layouts:
         * slice: {len: usize, ptr: *u8}
         * array: {len: usize, ptr: *u8, buffer: [len * base size]}
         * dynamic array: {len: usize, cap: usize, allocator: facet, buffer: *u8}
         * */

        if (type->array.dynamic) {
            return state->pointerSize * 5;
        } else {
            usize base = state->pointerSize * 2;
            usize buffer = 0;
            if (type->array.size > 0) {
                buffer = typeSize(state, type->array.base);
            }
            return base + buffer;
        }
    }
    case Z_TYPE_FACET:      return state->pointerSize * 2;/* {obj: *u8, vtable: *u8}, see genFacetType */
    case Z_TYPE_ENUM: {
        usize flag = 1;
        if (type->enm.integer) flag = typePrimitiveSize(state, type->enm.integer);
        return typeLargestType(type->enm.fields) + flag;
    }
    case Z_TYPE_SUM:        return typeLargestType(type->sumType) + 1;

    case Z_TYPE_STRUCT: {
        res = alignFields(
            state,
            (void **)type->strct.fields,
            alignStructFieldIter,
            query(type->annotations, "packed") != NULL
        );
        return res;
    }

    case Z_TYPE_TUPLE:
        return alignFields(
            state,
            (void **)type->tuple,
            alignTupleFieldIter,
            false
        );

    /* Optional pointers uses `none` as the niche value,
     * other types uses a struct {data, flag:u1} */
    case Z_TYPE_OPTIONAL:
        if (type->optional->kind == Z_TYPE_POINTER) return state->pointerSize;
        return alignUp(
            typeSize(state, type->optional) + 1,
            typeAlign(state, type->optional)
        );

    default: return 0;
    }
}

/**
 * @brief Calculates the alignment of the type.
 *
 * Mirrors typeSize: every type laid out as an ZMIR aggregate is aligned like
 * its most strictly aligned member, and scalars are aligned like their size.
 */
static usize typeAlign(ZState *state, ZType *type) {
    if (!type) return 1;
    switch (type->kind) {
    case Z_TYPE_STRUCT: {
        if (query(type->annotations, "packed")) return 1;
        usize res = 1;
        for (usize i = 0; i < veclen(type->strct.fields); i++) {
            usize cur = typeAlign(state, type->strct.fields[i]->resolved);
            if (cur > res) res = cur;
        }
        return res;
    }

    case Z_TYPE_TUPLE: {
        usize res = 1;
        for (usize i = 0; i < veclen(type->tuple); i++) {
            usize cur = typeAlign(state, type->tuple[i]);
            if (cur > res) res = cur;
        }
        return res;
    }

    /* {len: u64, ptr: *u8} and {obj: *u8, vtable: *u8} */
    case Z_TYPE_ARRAY:
    case Z_TYPE_FACET:      return state->pointerSize;

    /* TODO: put the buffer before the flag. */
    /* {flag: i8, buf: [n x i8]} */
    case Z_TYPE_ENUM:
    case Z_TYPE_SUM:        return 1;

    case Z_TYPE_OPTIONAL:
        return type->optional->kind == Z_TYPE_POINTER
            ? state->pointerSize
            : typeAlign(state, type->optional);

    default: {
        usize size = typeSize(state, type);
        return size ? size : 1;
    }
    }
}

/**
 * @brief Tells whether a type is materialized as an ZMIR parameter.
 *
 * Zero sized types (empty structs used as capability tags) carry no runtime
 * value, so they are dropped from the ZMIR signature instead of being passed
 * as an empty aggregate. Every path that builds a parameter list, binds the
 * parameters of a body or pushes call arguments must agree on this, otherwise
 * the argument count of a call does not match its callee.
 */
static bool isRuntimeParam(ZState *state, ZType *type) {
    return type && typeSize(state, type) > 0;
}

static bool _getStructIndex(ZType *strct, char *fieldName, u32 **path) {
    for (usize i = 0; i < veclen(strct->strct.fields); i++) {
        ZNode *field = strct->strct.fields[i];
        if (field->type == NODE_EMBED_FIELD) {
            vecpush(*path, i);
            bool found = _getStructIndex(field->resolved, fieldName, path);

            if (!found) vecpop(*path);
            else return true;
        } else if (strcmp(fieldName, field->field.identifier->str) == 0) {
            vecpush(*path, i);
            return true;
        }
    }
    return false;
}

/**
 * @brief Takes the struct index of a field.
 *
 * A struct can be composed by embedded fields which means
 * that a field can be accessed directly but compiled in a nested struct.
 * So it search recursively by its embedded fields and return the path
 */
static u32 *getStructIndex(ZType *strct, char *fieldName) {
    u32 *path = NULL;
    if (!_getStructIndex(strct, fieldName, &path)) {
        return NULL;
    }
    return path;
}

/**
 * @brief get the index of a variant's enum.
 *
 * @return -1 if not found
 */
static i32 enumIndexField(ZType *enumType, ZToken *field) {
    ZNode **fields = enumType->enm.fields;
    for (usize i = 0; i < veclen(fields); i++) {
        if (tokeneq(fields[i]->resolved->strct.name, field)) {
            return i;
        }
    }
    return -1;
}

static ZNode *getStructField(ZType *strct, char *fieldName) {
    for (usize i = 0; i < veclen(strct->strct.fields); i++) {
        ZNode *field = strct->strct.fields[i];
        if (field->type == NODE_EMBED_FIELD) {
            ZNode *found = getStructField(field->resolved, fieldName);
            if (found) return found;
        } else if (field->type == NODE_FIELD) {
            if (strcmp(field->field.identifier->str, fieldName) == 0) {
                return field;
            }
        }
    }
    return NULL;
}

static ZMIRTypeRef getCachedStruct(ZCodegen *ctx, const char *name) {
    for (usize i = 0; i < veclen(ctx->structNames); i++) {
        if (strcmp(ctx->structNames[i], name) == 0) {
            return ctx->structTypes[i];
        }
    }
    return NULL;
}

static char *encodeFacetVTable(ZType *facet, ZType *type) {
    char *buf = NULL;
    vecpush(buf, 'V');
    encodeType(facet, &buf);
    vecpush(buf, '_');
    vecpush(buf, '_');
    encodeType(type, &buf);
    vecpush(buf, '\0');
    return buf;
}

static void putStructInCache(ZCodegen *ctx, const char *name, ZMIRTypeRef strct) {
    vecpush(ctx->structNames, name);
    vecpush(ctx->structTypes, strct);
}

static ZMIRTypeRef genPrimitiveType(ZCodegen *ctx, ZToken *tok) {
    switch (tok->type) {
    case TOK_VOID:  return i0Type;
    case TOK_BOOL:  return i1Type;
    case TOK_CHAR:
    case TOK_I8:
    case TOK_U8:    return i8Type;
    case TOK_I16:
    case TOK_U16:   return i16Type;
    case TOK_RUNE:
    case TOK_I32:
    case TOK_U32:   return i32Type;
    case TOK_I64:
    case TOK_U64:   return i64Type;
    case TOK_F32:   return f32Type;
    case TOK_F64:   return f64Type;
    case TOK_USIZE:
    case TOK_ISIZE:
        return ZMIRIntTypeInContext(ctx->ctx, ctx->state->pointerSize * 8);
    default: {
        const char *name = mangler((char *[]) {
            tok->filename,
            tok->str,
            NULL
        });
        ZMIRTypeRef ref = getCachedStruct(ctx, name);
        if (ref) return ref;
        zlog(ctx->state, tok, Z4001, tok->str);
        return NULL;
    }
    }
}

static ZMIRTypeRef genStructType(ZCodegen *ctx, ZType *type, char *enumName) {
    char *parts[4] = {
        type->strct.name->filename,
        NULL,
        NULL,
        NULL
    };

    int n = 1;
    if (enumName) parts[n++] = enumName;
    parts[n++] = type->strct.name->str;

    const char *name = mangler(parts);
    ZMIRTypeRef cached  = getCachedStruct(ctx, name);
    if (cached) return cached;

    ZMIRTypeRef structType  = ZMIRStructCreateNamed(ctx->ctx, name);
    putStructInCache(ctx, (char *)name, structType);

    usize nfields           = veclen(type->strct.fields);
    ZMIRTypeRef *ftypes     = aalloc(
        ctx->module->allocator,
        sizeof(ZMIRTypeRef) * nfields
    );

    for (usize i = 0; i < veclen(type->strct.fields); i++) {
        ZType *ft = type->strct.fields[i]->resolved;
        ZMIRTypeRef field = genType(ctx, ft);

        if (ft && ft->kind == Z_TYPE_FUNCTION)
            field = ZMIRPointerType(field, 0);
        ftypes[i] = field;
    }

    bool packed = query(type->annotations, "packed");
    ZMIRStructSetBody(structType, ftypes, nfields, packed);

    return structType;
}

/**
 *
 * @brief Generates enum type.
 *
 * Enums are generated like tagged unions in c.
 * They are simpy a struct with an integer also called flag
 * that represents the active field of the enum
 * and a buffer with the size of the biggest field.
 * The buffer is stored like an array of bytes.
 * Every time i want to get the active field i just lookup the flag
 * and cast the buffer as the ith field.
 */
static ZMIRTypeRef genEnumType(ZCodegen *ctx, ZType *type) {
    ZToken *tok = type->enm.name;
    const char *name = mangler((char *[]){
        tok->filename,
        tok->str,
        NULL
    });
    ZMIRTypeRef cached = getCachedStruct(ctx, name);
    if (cached) return cached;

    ZMIRTypeRef enumType = ZMIRStructCreateNamed(ctx->ctx, name);
    putStructInCache(ctx, (char *)name, enumType);

    ZNode **fields = type->enm.fields;
    for (usize i = 0; i < veclen(fields); i++) {
        genStructType(ctx, fields[i]->resolved, stoken(tok));
    }

    usize largest = typeSize(ctx->state, type);
    ZMIRTypeRef flag = i8Type;
    if (type->enm.integer) flag = genPrimitiveType(ctx, type->enm.integer);
    ZMIRStructSetBody(enumType, (ZMIRTypeRef[]){
        flag, // Flag integer
        ZMIRArrayType(i8Type, largest - 1), // Buffer with the largest field
    }, largest == 0 ? 1 : 2, 0);

    return enumType;
}

static ZMIRTypeRef genFacetDecl(ZCodegen *ctx, ZType *type) {
    ZToken *tok = type->facet.name;
    const char *name = mangler((char *[]) {
        tok->filename,
        tok->str,
        NULL
    });
    ZMIRTypeRef facet = getCachedStruct(ctx, name);
    if (facet) return facet;

    facet = ZMIRStructCreateNamed(ctx->ctx, name);
    putStructInCache(ctx, name, facet);

    usize len = veclen(type->facet.funcs);
    ZMIRTypeRef *funcs = aalloc(ctx->module->allocator, sizeof(ZMIRTypeRef) * len);

    for (usize i = 0; i < len; i++) {
        funcs[i] = genType(ctx, type->facet.funcs[i]->resolved);
    }

    ZMIRStructSetBody(facet, funcs, len, false);

    return facet;
}

static ZMIRTypeRef genFacetType(ZCodegen *ctx) {
    ZMIRTypeRef fields[] = {
        ZMIRPointerType(i8Type, 0),
        ZMIRPointerType(i8Type, 0)
    };
    return ZMIRStructTypeInContext(
        ctx->ctx, fields, 2, false
    );
}

static ZMIRTypeRef genArrayType(ZCodegen *ctx, ZType *type) {
    ZMIRTypeRef base = genType(ctx, type->array.base);
    if (!base) return NULL;
    if (type->array.dynamic) {
        ZMIRTypeRef descriptorFields[] = {
            usizeType, usizeType, genFacetType(ctx),
            ptrType
        };
        return ZMIRStructTypeInContext(ctx->ctx, descriptorFields, 4, 0);
    } else if (type->array.size != 0) {
        return ZMIRStructTypeInContext(
            ctx->ctx, (ZMIRTypeRef []) {
                usizeType, ptrType//, ZMIRArrayType(base, type->array.size)
            }, 2, 0
        );
    } else {
        return ZMIRStructTypeInContext(
            ctx->ctx, (ZMIRTypeRef []) {
                usizeType, ptrType
            }, 2, 0
        );
    }
}

/**
 * @brief Translate a ZType to an ZMIR type.
 *
 * For primitives it returns directly
 * the cached values (loaded at the start of the code generation).
 *
 * Arrays are compiled with a struct of two fields:
 * - The size of the array as a u64
 * - The pointer to the elements.
 *
 * Struct is compiled by call itself for each field (embedded and normal).
 * Tuple is just an unnamed struct.
 * Enum is a struct with two fields:
 * - u8 is the tag that represent the active variant
 * - *u8 is the buffer where the variant is stored (with the size of the largest field).
 */
ZMIRTypeRef genType(ZCodegen *ctx, ZType *type) {
    if (!type) {
        zlog(ctx->state, NULL, Z900C);
        return NULL;
        // return ZMIRVoidTypeInContext(ctx->ctx);
    }

    switch (type->kind) {
    /* Function types always generated like a pointer.
     * The real function type is generated by buildFuncType */
    case Z_TYPE_FUNCTION:   return ZMIRPointerType(i8Type, 0);
    case Z_TYPE_ENUM:       return genEnumType  (ctx, type);
    case Z_TYPE_STRUCT:     return genStructType(ctx, type, NULL);
    case Z_TYPE_FACET:      return genFacetType (ctx);
    case Z_TYPE_PRIMITIVE:  return genPrimitiveType(ctx, type->primitive.token);
    case Z_TYPE_ARRAY:      return genArrayType(ctx, type);

    case Z_TYPE_GENERIC:
        if (veclen(type->generic.instantiations) == 0) {
            zlog(ctx->state, type->tok, Z900D);
        }
        return NULL;


    case Z_TYPE_POINTER: {
        ZType *base = type->base;
        if (!base) {
            zlog(ctx->state, type->tok, Z4002);
            return NULL;
        }
        if (base && base->kind == Z_TYPE_PRIMITIVE &&
            base->primitive.token->type == TOK_VOID) {
            return ZMIRPointerType(i8Type, 0);
        }
        return ZMIRPointerType(genType(ctx, base), 0);
    }


    case Z_TYPE_TUPLE: {
        usize len = veclen(type->tuple);
        ZMIRTypeRef *elems = aalloc(ctx->module->allocator, sizeof(ZMIRTypeRef) * (len ? len : 1));
        for (usize i = 0; i < len; i++) {
            elems[i] = genType(ctx, type->tuple[i]);
            if (!elems[i]) return NULL;
        }
        return ZMIRStructTypeInContext(ctx->ctx, elems, len, /*packed=*/ 0);
    }

    case Z_TYPE_SUM: {
        usize largest = typeSize(ctx->state, type);
        ZMIRTypeRef elems[] = {
            i8Type,
            ZMIRArrayType(i8Type, largest - 1)
        };
        return ZMIRStructTypeInContext(ctx->ctx, elems, 2, 0);
    }

    case Z_TYPE_NONE:
        /* none literal - represent as i8* null */
        return ZMIRPointerTypeInContext(ctx->ctx, 0);

    case Z_TYPE_OPTIONAL: {
        if (type->optional->kind == Z_TYPE_POINTER) {
            return ZMIRPointerTypeInContext(ctx->ctx, 0);
        }
        return ZMIRStructTypeInContext(ctx->ctx, (ZMIRTypeRef []){
            genType(ctx, type->optional),
            i1Type
        }, 2, false);
    }

    case Z_TYPE_TYPE: {
        ZNode *typeinfo = LangItems[Z_LANG_TYPE_INFO];
        if (!typeinfo) {
            zlog(ctx->state, type->tok, Z903D);
            return NULL;
        }
        return ZMIRPointerType(genType(ctx, typeinfo->resolved), 0);
    }
    default:
        zlog(ctx->state, NULL, Z900E, stype(type));
        return NULL;
    }
}

ZMIRBasicBlockRef makeblock(ZCodegen *ctx, char *name) {
    return ZMIRAppendBasicBlockInContext(
        ctx->ctx, ctx->currentFunc, label(ctx, name)
    );
}

inline void makebr(ZMIRBuilderRef builder, ZMIRBasicBlockRef block) {
    if (ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(builder))) return;
    ZMIRBuildBr(builder, block);
}

inline void makecondbr(
    ZMIRBuilderRef builder,
    ZMIRValueRef cond,
    ZMIRBasicBlockRef thenBranch,
    ZMIRBasicBlockRef elseBranch) {
    if (ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(builder))) return;
    ZMIRBuildCondBr(builder, cond, thenBranch, elseBranch);
}

/**
 * @brief Emits all defer statements.
 *
 * Defer statements are stored in the scope.
 * They are emitted in three ways:
 * - At the end of the block
 * - On a break/continue
 * - On a return statement
 *
 * @param scope is the target scope.
 * For break, continue and at the end of the block must be passed the parent scope.
 * For return statements must be passed the function-level scope.
 */
static void genChainDefer(ZCodegen *ctx, ZMIRScope *scope) {
    if (ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) return;

    ZMIRScope *cur = ctx->scope;

    while (cur != scope) {
        for (int i = (int)veclen(cur->defers) - 1; i >= 0; i--) {
            genStmt(ctx, cur->defers[i]);
        }
        cur = cur->parent;
    }
}

// TODO: compile string literals as an array chars.
static ZMIRValueRef genLitTok(ZCodegen *ctx, ZToken *tok, ZType *type) {
    switch (tok->type) {
    case TOK_STREAM:
        return NULL;
    case TOK_STR_LIT: {
        ZMIRValueRef strPtr = genStrLitGlobal(ctx, tok);
        return ZMIRBuildLoad2(
            ctx->builder,
            genType(ctx, type), strPtr, label(ctx, "string.value")
       );
    }
    case TOK_INT_LIT:
    case TOK_RUNE_LIT:
        return ZMIRConstInt(i32Type, tok->integer, true);
    case TOK_TRUE:
        return ZMIRConstInt(i1Type, 1, false);
    case TOK_FALSE:
        return ZMIRConstInt(i1Type, 0, false);
    case TOK_FLOAT_LIT:
        return ZMIRConstReal(f64Type, tok->floating);
    case TOK_NONE: {
        ZMIRTypeRef typeRef = genType(ctx, type);
        return ZMIRConstPointerNull(typeRef);
    default : return NULL;
    }
    }
}

/**
 * @brief Generate literals
 */
static ZMIRValueRef genLit(ZCodegen *ctx, ZNode *node) {
    return genLitTok(ctx, node->tok, node->resolved);
}

/**
 * @brief Emits the IR for identifiers (variables).
 *
 * It takes the stack allocation of the identifier saved in the scope.
 * */
static ZMIRValueRef genIdent(ZCodegen *ctx, ZNode *node) {
    if (!node) {
        zlog(ctx->state, NULL, Z900F);
        return NULL;
    } else if (!node->tok) {
        zlog(ctx->state, NULL, Z9010, node->type);
        return NULL;
    } else if (!node->resolved) {
        zlog(ctx->state, node->tok, Z9011, node->type);
        return NULL;
    } else if (node->type == NODE_TYPE) {
        return genTypeType(ctx, node);
    }

    ZMIRValueRef builtin = NULL;
    if (genBuiltin(ctx, node, &builtin)) return builtin;

    char *key = manglingIdent(node);
    ZMIRSymbol *sym = getZMIRSymbol(ctx, key);
    if (!sym) {
        zlog(ctx->state, node->tok, Z301F, node->tok->str);
        return NULL;
    }

    /* Local allocas and global variables are pointers - load to get the value.
       Functions and other constants are returned directly. */
    ZMIRValueKind kind = ZMIRGetValueKind(sym->value);

    if (sym->isValue) return sym->value;

    if (kind == ZMIRInstructionValueKind || kind == ZMIRGlobalVariableValueKind) {
        ZMIRTypeRef type = genType(ctx, node->resolved);
        return ZMIRBuildLoad2(ctx->builder, type, sym->value, node->tok->str);
    }
    return sym->value;
}

/**
 * @brief returns the cached stack pointer
 *
 * Stack pointers are 'cached' by the node and saved in the scope.
 * @param key is the cached key.
 *
 */
static ZMIRStack *getStackValue(ZCodegen *ctx, ZNode *key) {
    ZMIRScope *scope = ctx->scope;
    ZMIRStack **stack = NULL;

    while (scope) {
        stack = scope->stackAlloca;
        for (usize i = 0; i < veclen(stack); i++) {
            if (stack[i]->node == key) {
                return stack[i];
            }
        }
        scope = scope->parent;
    }
    return NULL;
}

static ZMIRValueRef castValue(ZCodegen *ctx, ZMIRValueRef val, ZType *from, ZType *to);

/**
 * @brief returns true if the value fits in a register.
 */
static bool fitsInRegister(ZMIRValueRef val) {
    ZMIRTypeKind kind = ZMIRGetTypeKind(ZMIRTypeOf(val));

    return (
        kind == ZMIRPointerTypeKind ||
        kind == ZMIRIntegerTypeKind ||
        kind == ZMIRFloatTypeKind   ||
        kind == ZMIRDoubleTypeKind  ||
        kind == ZMIRFunctionTypeKind);
}

/**
 * @brief Stores every identifier of a destructured pattern in the stack
 *
 * It takes a stack allocation (ZMIRValueRef) and follwos a simple pattern:
 * - An identifier stores directly the value into the scope.
 * - A struct generates a pointer for each field and call itself.
 * - A tuple do the same of the struct
 * */
static void putDestructuredPatternInStack(
        ZCodegen *ctx, ZType *type,
        ZVarDestructPattern *pattern,
        ZMIRValueRef ptr) {
    if (!pattern) {
        zlog(ctx->state, NULL, Z9012);
        return;
    } else if (!type) {
        zlog(ctx->state, pattern->tok, Z9013);
        return;
    }

    switch (pattern->type) {
    case Z_VAR_LIT: break;
    case Z_VAR_IDENT:
        putZMIRValueRef(
            ctx,
            pattern->ident->str,
            ptr
        );
        break;
    case Z_VAR_STRUCT: {
        ZMIRTypeRef typeRef = genType(ctx, type);
        for (usize i = 0; i < veclen(pattern->fields); i++) {

            int idx = -1;

            for (usize j = 0; j < veclen(type->strct.fields); j++) {
                if (tokeneq(
                        type->strct.fields[j]->field.identifier,
                        pattern->fields[i]->key)
                    ) {
                    idx = (int)j;
                    break;
                }
            }

            if (idx == -1) {
                zlog(ctx->state, pattern->fields[i]->key,
                        Z4013,
                        pattern->fields[i]->key->str);
                continue;
            }

            ZMIRValueRef gep = ZMIRBuildStructGEP2(
                ctx->builder, typeRef,
                ptr, idx, label(ctx, pattern->fields[i]->key)
            );
            putDestructuredPatternInStack(
                ctx,
                type->strct.fields[idx]->field.type,
                pattern->fields[i]->value,
                gep
            );
        }
        break;
    }
    case Z_VAR_TUPLE: {
        ZMIRTypeRef typeRef = genType(ctx, type);
        for (usize i = 0; i < veclen(pattern->tuple); i++) {
            ZMIRValueRef gep = ZMIRBuildStructGEP2(
                ctx->builder, typeRef, ptr, i, label(ctx, pattern->tuple[i]->tok)
            );
            putDestructuredPatternInStack(
                ctx,
                type->tuple[i],
                pattern->tuple[i],
                gep
            );
        }
        break;
    }
    case Z_VAR_ENUM: {
        i32 variantIndex = enumIndexField(type, pattern->prop);
        if (variantIndex == -1) {
            zlog(ctx->state, pattern->prop, Z9014);
            return;
        }
        ZType *variantType          = type->enm.fields[variantIndex]->resolved;
        ZMIRTypeRef variantTypeRef  = genType(ctx, variantType);

        for (usize i = 0; i < veclen(pattern->args); i++) {
            ZMIRValueRef fieldPtr = ZMIRBuildStructGEP2(
                ctx->builder, variantTypeRef, ptr,
                i + 1, label(ctx, pattern->args[i]->tok));
            putDestructuredPatternInStack(
                ctx,                variantType->strct.fields[i + 1]->resolved,
                pattern->args[i],   fieldPtr
            );
        }
        break;
    }
    case Z_VAR_SUM: {
        ZMIRValueRef bufPtr = ZMIRBuildStructGEP2(
            ctx->builder, genType(ctx, type), ptr, 1, label(ctx, "sum.buf")
        );
        putDestructuredPatternInStack(
            ctx, pattern->sum.type, pattern->sum.child, bufPtr
        );
        break;
    }
    default:
        zlog(ctx->state, pattern->tok, Z9002);
        break;
    }
}

/**
 * @brief Generate variable declarations
 *
 * Variable declarations are allocated into the stack in the first-pass.
 * So the node must be cached and the call getStackValue cannot fail.
 *
 * Once the pointer is looked up it stores the pattern on the left (destructured var).
 * Then generates the expression and store the result into the stack pointer.
 */
static void genGlobalVar(ZCodegen *ctx, ZNode *node) {
    ZVarDestructPattern *pat = node->varDecl.pattern;
    if (!pat || pat->type != Z_VAR_IDENT) {
        zlog(ctx->state, node->tok, Z4003);
        return;
    }

    char *name = pat->ident->str;
    ZMIRValueRef global = getZMIRValueRef(ctx, name);
    if (!global) {
        zlog(ctx->state, node->tok, Z9015, name);
        return;
    }

    if (!node->varDecl.rvalue) {
        ZMIRSetInitializer(global, ZMIRConstNull(genType(ctx, node->resolved)));
        return;
    }

    ZMIRValueRef init = genExpr(ctx, node->varDecl.rvalue);
    if (!init) {
        zlog(ctx->state, node->tok, Z9016, name);
        return;
    }
    if (!ZMIRIsConstant(init)) {
        zlog(ctx->state, node->tok, Z4004, name);
        return;
    }
    ZMIRSetInitializer(global, init);
}

/**
 * @brief Zero-initializes a variable's stack allocation.
 *
 * Fixed-size arrays keep their data in a separate 'elem' buffer from the
 * {len, ptr} descriptor in 'stack' - both need to be filled in, mirroring
 * what genArrayLitPtr does for a partially-specified array literal.
 */
static void genZeroInit(ZCodegen *ctx, ZType *type, ZMIRStack *stack) {
    if (type->kind == Z_TYPE_ARRAY && type->array.size > 0 && stack->elem) {
        ZMIRBuildStore(ctx->builder, ZMIRConstNull(stack->elemType), stack->elem);
        storeArray(
            ctx, stack, ZMIRConstInt(i64Type, type->array.size, false)
        );
        return;
    }
    ZMIRBuildStore(ctx->builder, ZMIRConstNull(stack->stackType), stack->stack);
}

static void genVarDecl(ZCodegen *ctx, ZNode *node) {
    if (!node->resolved) {
        zlog(ctx->state, node->tok, Z9017);
        return;
    }

    if (!node->varDecl.rvalue) {
        ZMIRStack *stack = getStackValue(ctx, node);
        if (!stack) {
            zlog(ctx->state, node->tok, Z9018, node->tok->str);
            return;
        }

        putDestructuredPatternInStack(
            ctx, node->resolved, node->varDecl.pattern, stack->stack);

        if (!node->varDecl.uninit) genZeroInit(ctx, node->resolved, stack);
        return;
    }

    ZNode *rvalue = node->varDecl.rvalue;
    ZMIRStack *stack = getStackValue(ctx, rvalue);
    if (!stack) {
        zlog(ctx->state, node->tok, Z9018, node->tok->str);
        return;
    }

    putDestructuredPatternInStack(
        ctx, node->resolved, node->varDecl.pattern, stack->stack);

    ZMIRValueRef val = genExpr(ctx, rvalue);
    if (!val || val == stack->stack) return;

    if (fitsInRegister(val)) {
        val = castValue(ctx, val, rvalue->resolved, node->resolved);
    }
    ZMIRBuildStore(ctx->builder, val, stack->stack);
}

/**
 * @brief Generates the field access for an embedded field.
 */
static ZMIRValueRef genStructGEPChain(ZCodegen *ctx,
    ZType *base, ZMIRValueRef origin, u32 *path) {
    ZMIRValueRef prev   = origin;
    ZMIRValueRef ptr    = NULL;

    for (usize i = 0; i < veclen(path); i++) {
        ptr = ZMIRBuildStructGEP2(
            ctx->builder,
            genType(ctx, base),
            prev,
            path[i],
            label(ctx, base->tok)
        );
        base = base->strct.fields[path[i]]->resolved;
        prev = ptr;
    }

    return ptr;
}

/**
 * @brief store the array metadata into the compiled struct.
 */
static void storeArray(ZCodegen *ctx, ZMIRStack *stack, ZMIRValueRef length) {
    if (!stack->stackType) {
        zlog(ctx->state, NULL, Z9019);
        return;
    } else if (!stack->elemType) {
        zlog(ctx->state, NULL, Z901A);
    }
    ZMIRValueRef lenField = ZMIRBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 0, "len");

    ZMIRBuildStore(ctx->builder, length, lenField);

    ZMIRValueRef indices[] = {
        ZMIRConstInt(i32Type, 0, false),
        ZMIRConstInt(i32Type, 0, false)
    };
    ZMIRValueRef dataPtr = ZMIRBuildGEP2(
        ctx->builder, stack->elemType, stack->elem, indices, 2, "ptr"
    );
    ZMIRValueRef dataField = ZMIRBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 1, "ptr");
    ZMIRBuildStore(ctx->builder, dataPtr, dataField);
}

/**
 * @brief Generates array literal.
 */
static ZMIRValueRef genArrayLitPtr(ZCodegen *ctx, ZNode *node) {
    ZMIRStack *stack = getStackValue(ctx, node);

    if (!stack) {
        zlog(ctx->state, node->tok, Z901B);
        return NULL;
    } else if (!stack->elem) {
        zlog(ctx->state, node->tok, Z901C);
        return NULL;
    }

    ZMIRTypeRef elemType = genType(ctx, node->resolved->array.base);
    if (node->resolved->array.size > veclen(node->arraylit)) {
        ZMIRBuildStore(ctx->builder, ZMIRConstNull(stack->elemType), stack->elem);
    }

    for (usize i = 0; i < veclen(node->arraylit); i++) {
        ZMIRValueRef indices[] = {
            ZMIRConstInt(i32Type, 0, false),
            ZMIRConstInt(i32Type, i, false)
        };
        ZMIRValueRef gep = ZMIRBuildGEP2(
            ctx->builder,   stack->elemType,
            stack->elem,    indices,
            2,              label(ctx, node->tok)
        );

        ZNode *elem = node->arraylit[i];
        ZMIRValueRef val = genExpr(ctx, elem);
        if (!val) {
            zlog(ctx->state, elem->tok, Z901D);
            return NULL;
        }
        /* A call returning a struct comes back as an aggregate value,
           not a pointer - store it directly. */
        if (ZMIRGetTypeKind(ZMIRTypeOf(val)) == ZMIRPointerTypeKind &&
            node->resolved->array.base->kind == Z_TYPE_STRUCT) {
            val = ZMIRBuildLoad2(ctx->builder, elemType, val, label(ctx, node->tok));
        }
        ZMIRBuildStore(ctx->builder, val, gep);
    }

    storeArray(
        ctx, stack, ZMIRConstInt(usizeType, node->resolved->array.size, false)
    );

    return stack->stack;
}

/**
 * @brief Generates the slice from an array.
 *
 * Slices are generated like array with the difference that
 * its length is calculated at runtime.
 */
static ZMIRValueRef genSlicePtr(ZCodegen *ctx, ZNode *node) {
    ZMIRStack *stack       = getStackValue(ctx, node);
    ZType *baseType         = node->slice.base->resolved;
    ZMIRValueRef ptr        = genLValue(ctx, node->slice.base);
    ZMIRTypeRef sliceType   = genType(ctx, baseType);
    ZMIRTypeRef elemType    = genType(ctx, baseType->array.base);
    if (!ptr) {
        zlog(ctx->state, node->slice.base->tok, Z901E);
        return NULL;
    }
    if (!stack) {
        zlog(ctx->state, node->tok, Z901F);
        return NULL;
    }

    ZMIRValueRef start = node->slice.start  ?
        genExpr(ctx, node->slice.start)     :
        ZMIRConstInt(usizeType, 0, false);

    ZMIRValueRef end = NULL;

    if (node->slice.end) {
        end = genExpr(ctx, node->slice.end);
    } else {
        end = ZMIRBuildStructGEP2(
            ctx->builder,
            sliceType,
            ptr, 0, "len_ptr"
        );
        end = ZMIRBuildLoad2(ctx->builder, usizeType, end, "len");
    }

    ZMIRValueRef length = ZMIRBuildSub(
        ctx->builder, end, start, label(ctx, node->tok)
    );

    ZMIRValueRef lenField = ZMIRBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 0, "len");
    ZMIRBuildStore(ctx->builder, length, lenField);

    ZMIRValueRef ptrField = ZMIRBuildStructGEP2(
        ctx->builder, stack->stackType, stack->stack, 1, "ptr");

    ZMIRValueRef originPtrField = ZMIRBuildStructGEP2(
        ctx->builder, sliceType, ptr, 1, "ptr"
    );

    ZMIRValueRef basePtr = ZMIRBuildLoad2(
        ctx->builder, ZMIRPointerType(elemType, 0),
        originPtrField, "ptr"
    );

    ZMIRValueRef dataPtr = ZMIRBuildGEP2(
        ctx->builder, elemType, basePtr, &start, 1, "ptr");

    ZMIRBuildStore(
        ctx->builder,
        dataPtr, ptrField
    );

    return stack->stack;
}

static ZMIRValueRef genStructLitPtr(ZCodegen *ctx, ZNode *node) {
    ZMIRStack *stack = getStackValue(ctx, node);
    if (!stack) {
        zlog(ctx->state, node->tok, Z9020);
        return NULL;
    }
    return genStructLitInto(ctx, node, stack->stack);
}

static ZMIRValueRef genTupleLitInto(ZCodegen *ctx, ZNode *node, ZMIRValueRef dest) {
    ZMIRTypeRef type = genType(ctx, node->resolved);
    const char *name = label(ctx, node->tok);

    ZMIRTypeRef tupleType = genType(ctx, node->resolved);
    ZMIRValueRef ptr = dest ? dest : ZMIRBuildAlloca(ctx->builder, type, name);
    ZMIRValueRef fieldPtr, val;

    usize len = veclen(node->tuplelit);
    for (usize i = 0; i < len; i++) {
        val = genExpr(ctx, node->tuplelit[i]);

        fieldPtr = ZMIRBuildStructGEP2(
            ctx->builder, tupleType,
            ptr, i, label(ctx, node->tok)
        );

        ZMIRBuildStore(ctx->builder, val, fieldPtr);
    }
    return ptr;
}

static ZMIRValueRef genTupleLitPtr(ZCodegen *ctx, ZNode *node) {
    ZMIRStack *stack = getStackValue(ctx, node);

    if (!stack) {
        zlog(ctx->state, node->tok, Z9021);
    }
    return genTupleLitInto(ctx, node, stack->stack);
}

static ZMIRValueRef genMemberAccessPtr(ZCodegen *ctx, ZNode *node) {
    ZNode *target = node->memberAccess.func;
    if (!target && node->memberAccess.sym) target = node->memberAccess.sym->node;
    if (target) {
        char *key = NULL;
        if (target->type == NODE_FUNC)          key = target->funcDef.mangled;
        else if (target->type == NODE_FOREIGN)  key = stoken(target->foreignDecl.name);
        else if (node->memberAccess.sym)        key = stoken(node->memberAccess.sym->name);

        ZMIRValueRef val = key ? getZMIRValueRef(ctx, key) : NULL;
        if (!val) {
            zlog(ctx->state, node->tok,
                Z301F,
                key ? key : stoken(node->memberAccess.field));
        }
        return val;
    }

    ZType *objType  = node->memberAccess.object->resolved;
    ZToken *tok     = node->memberAccess.field;

    ZType *baseType = objType;
    while (baseType && baseType->kind == Z_TYPE_POINTER)
        baseType    = baseType->base;

    u32 *path = NULL;
    if (baseType->kind == Z_TYPE_FACET) {
        return NULL;
    } else if (baseType->kind == Z_TYPE_STRUCT) {
        path = getStructIndex(baseType, tok->str);
        if (!path) {
            zlog(ctx->state, tok, Z9022, tok->str);
            return NULL;
        }
    } else if (baseType->kind == Z_TYPE_TUPLE) {
        if (tok->integer > (i32)veclen(baseType->tuple) ||
            tok->integer < 0) {
            zlog(ctx->state, tok, Z4005, tok->integer);
            return NULL;
        }
        ZMIRValueRef ptr = genLValue(ctx, node->memberAccess.object);
        if (!ptr) return NULL;
        return ZMIRBuildStructGEP2(
            ctx->builder,
            genType(ctx, baseType),
            ptr,
            tok->integer,
            label(ctx, tok)
        );
    } else if (baseType->kind == Z_TYPE_ARRAY) {
        ZMIRValueRef ptr = objType->kind == Z_TYPE_POINTER
            ? genExpr  (ctx, node->memberAccess.object)
            : genLValue(ctx, node->memberAccess.object);
        if (!ptr) return NULL;
        i32 index = -1;

        /* See typeSize: the buffer is the last field of either layout. */
        if      (strcmp(tok->str, "len") == 0) index = 0;
        else if (strcmp(tok->str, "ptr") == 0)
            index = baseType->array.dynamic ? 3 : 1;
        else {
            zlog(ctx->state, tok, Z9023);
            return NULL;
        }

        return ZMIRBuildStructGEP2(
            ctx->builder, genType(ctx, baseType),
            ptr, (unsigned)index, label(ctx, tok)
        );
    } else if (baseType->kind == Z_TYPE_ENUM) {
        if (strcmp(tok->str, "tag") == 0) {
            vecpush(path, 0);
        }
    } else {
        zlog(ctx->state, tok, Z9023);
        return NULL;
    }

    ZMIRValueRef objPtr    = objType->kind == Z_TYPE_POINTER
        ? genExpr  (ctx, node->memberAccess.object)
        : genLValue(ctx, node->memberAccess.object);

    if (!objPtr) return NULL;
    emitNullCheck(ctx, objPtr, node->tok);
    ZMIRValueRef chain = genStructGEPChain(
        ctx, baseType, objPtr, path
    );
    return chain;
}

static ZMIRValueRef genSubscriptPtr(ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef ptr        = genLValue(ctx, node->subscript.arr);
    ZMIRValueRef i          = genExpr(ctx, node->subscript.index);
    ZType *arrType          = node->subscript.arr->resolved;
    ZMIRTypeRef type        = genType(ctx, arrType);
    const char *name        = label(ctx, node->tok);

    if (arrType->kind == Z_TYPE_POINTER) {
        ZMIRTypeRef elemType = genType(ctx, arrType->base);
        ZMIRValueRef loaded = ZMIRBuildLoad2(
            ctx->builder,   type,
            ptr,            name
        );
        return ZMIRBuildGEP2(
            ctx->builder,   elemType,
            loaded,         &i,
            1,              name
        );
    } else if (arrType->kind == Z_TYPE_ARRAY) {
        emitBoundCheck(ctx, node->tok, i, type, ptr);

        ZMIRTypeRef elemType = genType(ctx, arrType->array.base);
        ZMIRTypeRef ptrType = ZMIRPointerType(elemType, 0);

        int index = arrType->array.dynamic ? 3 : 1;
        ZMIRValueRef fieldPtr = ZMIRBuildStructGEP2(
            ctx->builder, type, ptr,
            index, label(ctx, node->subscript.arr->tok)
        );
        ZMIRValueRef basePtr = ZMIRBuildLoad2(
            ctx->builder, ptrType, fieldPtr, name
        );
        return ZMIRBuildGEP2(
            ctx->builder,   elemType,
            basePtr,        &i,
            1,              name
        );
    }
    zlog(ctx->state, node->tok, Z4006, stype(node->resolved));
    return NULL;
}

static ZMIRValueRef _genEnumLitPtr(ZCodegen *ctx,
        ZNode *node, ZToken *variant, ZNode **args) {
    ZMIRStack *stack   = getStackValue(ctx, node);
    if (!stack) {
        zlog(ctx->state, node->tok, Z901B);
        return NULL;
    }

    i32 index = enumIndexField(node->resolved, variant);
    if (index == -1) {
        zlog(ctx->state, variant, Z9024);
        return NULL;
    }

    ZMIRTypeRef fieldType = genType(
        ctx, node->resolved->enm.fields[index]->resolved
    );

    ZToken *tok = node->resolved->enm.integer;
    ZMIRTypeRef type = tok ? genPrimitiveType(ctx, tok) : i8Type;
    ZMIRBuildStore(
        ctx->builder,
        ZMIRConstInt(type, (u32)index, false),
        stack->stack
    );

    for (usize i = 0; i < veclen(args); i++) {
        ZMIRValueRef fieldPtr = ZMIRBuildStructGEP2(
            ctx->builder, fieldType, stack->stack, i + 1, label(ctx, args[i]->tok));

        ZMIRBuildStore(
            ctx->builder,
            genExpr(ctx, args[i]),
            fieldPtr
        );

    }

    return stack->stack;
}


static ZMIRValueRef genEnumNoPayloadPtr(ZCodegen *ctx, ZNode *node) {
    return _genEnumLitPtr(ctx, node, node->memberAccess.field, NULL);
}

static ZMIRValueRef genEnumLitPtr(ZCodegen *ctx, ZNode *node) {
    ZToken *variant     = node->call.callee->memberAccess.field;
    return _genEnumLitPtr(ctx, node, variant, node->call.args);
}

/* Build the actual ZMIRFunctionType from a ZType. Used for indirect calls where
 * genType returns an opaque ptr suitable for storage, not for ZMIRBuildCall2. */
static ZMIRTypeRef buildFuncType(ZCodegen *ctx, ZType *type) {
    usize args = veclen(type->func.args);
    usize argc = veclen(type->func.capabilities);
    ZMIRTypeRef *params = aalloc(ctx->module->allocator, sizeof(ZMIRTypeRef) * ((argc + args) ? (argc + args) : 1));
    usize len = 0;
    for (usize i = 0; i < argc; i++) {
        if (!isRuntimeParam(ctx->state, type->func.capabilities[i])) continue;
        params[len++] = genType(ctx, type->func.capabilities[i]);
    }
    for (usize i = 0; i < args; i++) {
        if (!isRuntimeParam(ctx->state, type->func.args[i])) continue;
        params[len++] = genType(ctx, type->func.args[i]);
    }
    ZMIRTypeRef ret = genType(ctx, type->func.ret);
    return ZMIRFunctionType(ret, params, (unsigned)len, type->func.variadic);
}

/* Facet methods are dispatched through a type-erased vtable. The concrete
 * receiver is passed as an opaque pointer prepended to the declared params,
 * matching the impl function whose first parameter is the receiver. */
static ZMIRTypeRef buildFacetFuncType(ZCodegen *ctx, ZType *type) {
    usize argc = veclen(type->func.args);
    ZMIRTypeRef *params = aalloc(ctx->module->allocator, sizeof(ZMIRTypeRef) * (argc + 1));

    params[0] = ZMIRPointerType(i8Type, 0);

    usize len = 1;
    for (usize i = 0; i < argc; i++) {
        if (!isRuntimeParam(ctx->state, type->func.args[i])) continue;
        params[len++] = genType(ctx, type->func.args[i]);
    }

    ZMIRTypeRef ret = genType(ctx, type->func.ret);
    return ZMIRFunctionType(
        ret, params, (unsigned)len, type->func.variadic
    );
}

static ZMIRValueRef genFacetMember(ZCodegen *ctx, ZNode *node) {
    ZNode *obj          = node->memberAccess.object;
    ZToken *field       = node->memberAccess.field;
    ZMIRTypeRef type    = genType(ctx, obj->resolved);
    ZMIRValueRef objRef = genLValue(ctx, obj);

    ZNode **funcs = obj->resolved->facet.funcs;
    i32 index = -1;
    for (usize i = 0; i < veclen(funcs); i++) {
        if (tokeneq(funcs[i]->field.identifier, field)) {
            index = i;
            break;
        }
    }
    if (index == -1) {
        zlog(ctx->state, field, Z4007, stoken(field));
        return NULL;
    }

    ZMIRValueRef vtableSlot = ZMIRBuildStructGEP2(
        ctx->builder, type, objRef, 1,
        label(ctx, "vtable.slot")
    );

    ZMIRValueRef vtable = ZMIRBuildLoad2(
        ctx->builder, ZMIRPointerType(i8Type, 0), vtableSlot,
        label(ctx, "vtable")
    );

    ZMIRValueRef cond = ZMIRBuildICmp(
        ctx->builder, ZMIRIntEQ, vtable,
        ZMIRConstPointerNull(ptrType), label(ctx, "facet.panic")
    );
    emitPanic(ctx, node->tok, cond, "Got null vtable");

    ZMIRTypeRef vtableType = genFacetDecl(ctx, obj->resolved);

    ZMIRValueRef funcSlot = ZMIRBuildStructGEP2(
        ctx->builder, vtableType, vtable, index,
        label(ctx, "vtable.func")
    );

    return ZMIRBuildLoad2(
        ctx->builder, ZMIRPointerType(i8Type, 0), funcSlot,
        label(ctx, "func")
    );
}

/**
 * @brief Generates a call to a function.
 *
 * The name of the function depends on the 'type' of the function:
 * - Raw functions have the normal name.
 * - Receiver functions have a mangled name.
 * - Static functions have also a mangled name but with a different rule
 *   to avoid conflicts with receiver functions.
 *
 * The parameter list is compiled as follow:
 * - The first parameter is the receiver, if present.
 * - The list of capabilities.
 * - The list of normal arguments.
 */
static ZMIRValueRef genCall(ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef func   = NULL;
    ZMIRValueRef *args  = NULL;
    ZNode *callee       = node->call.callee;

    ZMIRValueRef builtin = NULL;
    if (genBuiltin(ctx, node, &builtin)) return builtin;

    if (callee->type == NODE_IDENTIFIER && callee->resolved->kind == Z_TYPE_FACET) {
        ZMIRStack *stack = getStackValue(ctx, node);
        return genFacetConstruct(ctx, stack, callee->resolved, node->call.args[0]);
    } if (callee->type == NODE_MEMBER &&
        callee->memberAccess.object &&
        callee->memberAccess.object->resolved &&
        callee->memberAccess.object->resolved->kind == Z_TYPE_FACET) {
        ZNode *obj          = callee->memberAccess.object;
        ZMIRValueRef facet  = genLValue(ctx, obj);
        ZMIRTypeRef type    = genType(ctx, obj->resolved);

        ZMIRValueRef objSlot = ZMIRBuildStructGEP2(
            ctx->builder, type, facet, 0, label(ctx, "facet.obj")
        );

        ZMIRValueRef self = ZMIRBuildLoad2(
            ctx->builder, ZMIRPointerType(i8Type, 0),
            objSlot, label(ctx, "facet.self")
        );

        vecpush(args, self);
        func = genFacetMember(ctx, callee);
    } else if (callee->type == NODE_MEMBER &&
        (callee->memberAccess.sym || callee->memberAccess.func)) {
        /* Module member (mod.func), static function (Type.func) or foreign
         * namespace member (c.printf): a direct call, no self injection.
         * The semantic analyzer stored the target in sym (imports) or
         * func (static/foreign). */
        ZNode *target = callee->memberAccess.func;
        if (!target) target = callee->memberAccess.sym->node;

        char *key = NULL;
        if (target->type == NODE_FUNC)          key = target->funcDef.mangled;
        else if (target->type == NODE_FOREIGN)  key = stoken(target->foreignDecl.name);

        if (!key) {
            zlog(ctx->state, callee->tok,
                Z00A8, stoken(callee->memberAccess.field));
            return NULL;
        }

        func = getZMIRValueRef(ctx, key);
        if (!func) {
            zlog(ctx->state, callee->tok, Z4008, key);
            return NULL;
        }
    } else if (callee->type == NODE_MEMBER && callee->memberAccess.mangled) {
        /* Receiver method call: look up the global function and inject self. */
        func = getZMIRValueRef(ctx, callee->memberAccess.mangled);

        if (!func) zlog(ctx->state,
                    callee->tok,
                    Z4009,
                    callee->memberAccess.mangled);

        ZMIRValueRef self = genExpr(ctx, callee->memberAccess.object);
        vecpush(args, self);

    }  else {
        /* Expression call: covers identifiers, static access, subscripts, and
         * function-pointer fields (NODE_MEMBER without mangled). */
        func = genExpr(ctx, callee);
        if (!func) return NULL;
    }

    /* For indirect calls (locally-loaded function pointers) the ZMIR value is
     * not a global, so ZMIRGlobalGetValueType is invalid. Derive the function
     * type from the semantic info instead. mangled == NULL means indirect. */
    ZMIRTypeRef funcType;
    if (callee->type == NODE_MEMBER &&
        callee->memberAccess.object &&
        callee->memberAccess.object->resolved &&
        callee->memberAccess.object->resolved->kind == Z_TYPE_FACET) {
        funcType = buildFacetFuncType(ctx, callee->resolved);
    } else if (callee->type == NODE_MEMBER && !callee->memberAccess.mangled &&
        !callee->memberAccess.sym && !callee->memberAccess.func) {
        funcType = buildFuncType(ctx, callee->resolved);
    } else if (ZMIRGetValueKind(func) != ZMIRFunctionValueKind) {
        /* Indirect call through a function pointer variable. */
        funcType = buildFuncType(ctx, callee->resolved);
    } else {
        funcType = ZMIRGlobalGetValueType(func);
    }

    usize fixedParamCount = ZMIRCountParamTypes(funcType);
    ZMIRTypeRef *fixedParamTypes = NULL;
    if (fixedParamCount > 0) {
        fixedParamTypes = aalloc(ctx->module->allocator, sizeof(ZMIRTypeRef) * fixedParamCount);
        ZMIRGetParamTypes(funcType, fixedParamTypes);
    }

    for (usize i = 0; i < veclen(node->call.capabilities); i++) {
        ZNode *cap = node->call.capabilities[i];
        if (!cap) {
            zlog(ctx->state, node->tok, Z9025);
            continue;
        } else if (!cap->tok) {
            zlog(ctx->state, node->tok, Z9026);
            continue;
        } else if (!cap->resolved) {
            zlog(ctx->state, node->tok, Z9027);
            continue;
        }
        if (!isRuntimeParam(ctx->state, cap->resolved)) continue;
        ZMIRValueRef capability = getCapabilityRef(
            ctx, cap->resolved
        );
        if (!capability) {
            zlog(ctx->state, cap->tok, Z00A4, stoken(cap->tok));
        } else {
            capability = ZMIRBuildLoad2(
                ctx->builder,
                genType(ctx, cap->resolved),
                capability, label(ctx, cap->tok)
            );
        }
        vecpush(args, capability);
    }

    for (usize i = 0; i < veclen(node->call.args); i++) {
        ZMIRValueRef arg = genExpr(ctx, node->call.args[i]);

        if (!arg) {
            zlog(ctx->state, node->call.args[i]->tok, Z903E);
            continue;
        }

        /* The argument is still evaluated for its side effects, but a zero
         * sized one is not part of the callee signature, see isRuntimeParam. */
        if (!isRuntimeParam(ctx->state, node->call.args[i]->resolved)) continue;

        /* Args already holds the receiver and the capabilities that survived
         * the filtering, so its length is the ZMIR parameter index. */
        usize totalArgIndex = veclen(args);

        /* ABI adaptation: foreign functions declare small struct params as
         * i32/i64 (packed integer).  If the Zinc-side arg is a struct,
         * store it to a temp slot and reload as the packed integer so the
         * backend emits a single-register load instead of per-field loads.
         * FIXME: Implement a specific annotation [[packed]] instead of 'understand'
         * if the argument should be packed.
         * */
        if (fixedParamTypes && totalArgIndex < fixedParamCount) {
            ZMIRTypeRef expected = fixedParamTypes[totalArgIndex];
            ZMIRTypeRef actual   = ZMIRTypeOf(arg);
            if (ZMIRGetTypeKind(actual)   == ZMIRStructTypeKind &&
                ZMIRGetTypeKind(expected) == ZMIRIntegerTypeKind) {
                ZMIRValueRef tmp = ZMIRBuildAlloca(ctx->builder, actual, "");
                ZMIRBuildStore(ctx->builder, arg, tmp);
                arg = ZMIRBuildLoad2(ctx->builder, expected, tmp, "");
            }
        }

        /* C default argument promotions for variadic arguments:
         *  f32         -> f64
         *  i1/i8/i16   -> i32
         *  The backend rely on the frontend to emit these, zinc must do it explicitly
         *  or callers like printf read garbage.
         */
        if (ZMIRIsFunctionVarArg(funcType) && totalArgIndex >= (usize)fixedParamCount) {
            ZMIRTypeRef argType = ZMIRTypeOf(arg);
            ZMIRTypeKind kind   = ZMIRGetTypeKind(argType);
            if (kind == ZMIRFloatTypeKind) {
                arg = ZMIRBuildFPExt(ctx->builder, arg, f64Type, "");
            } else if (kind == ZMIRIntegerTypeKind &&
                       ZMIRGetIntTypeWidth(argType) < 32) {
                arg = ZMIRBuildZExt(ctx->builder, arg, i32Type, "");
            }
        }

        vecpush(args, arg);
    }

    ZMIRValueRef call = ZMIRBuildCall2(
        ctx->builder,
        funcType,
        func,
        args,
        veclen(args),
        isVoid(node->resolved) ? "" : label(ctx, "call")
    );

    if (!fitsInRegister(call)) {
        ZMIRStack *stack = getStackValue(ctx, node);
        if (stack) {
            ZMIRBuildStore(ctx->builder, call, stack->stack);
        }
    }
    return call;
}

ZMIRValueRef genStrLitGlobal(ZCodegen *ctx, ZToken *tok) {
    const char *s           = stoken(tok);
    usize len               = strlen(s);

    ZMIRTypeRef dataType    = ZMIRArrayType2(i8Type, len + 1);
    ZMIRValueRef dataGlobal = ZMIRAddGlobal(ctx->mod, dataType, label(ctx, "str.data"));

    ZMIRSetInitializer(
        dataGlobal,
        ZMIRConstStringInContext(ctx->ctx, s, len, false)
    );
    ZMIRSetGlobalConstant(dataGlobal, true);
    ZMIRSetLinkage(dataGlobal, ZMIRPrivateLinkage);
    ZMIRSetUnnamedAddr(dataGlobal, ZMIRGlobalUnnamedAddr);

    ZMIRValueRef fields[] = {
        ZMIRConstInt(usizeType, len, false),
        dataGlobal
    };

    ZMIRValueRef globVal = ZMIRAddGlobal(ctx->mod, strType, label(ctx, "str"));

    ZMIRSetInitializer(
        globVal,
        ZMIRConstStructInContext(ctx->ctx, fields, 2, false)
    );
    ZMIRSetGlobalConstant(globVal, true);
    ZMIRSetLinkage(globVal, ZMIRPrivateLinkage);
    ZMIRSetUnnamedAddr(globVal, ZMIRGlobalUnnamedAddr);

    return globVal;
}

/**
 * @brief Loads the addresso of the expression.
 *
 * genLValue is used to load the address of the expression rather than the value.
 * meanwhile the function to load the value is genExpr.
 */
ZMIRValueRef genLValue(ZCodegen *ctx, ZNode *node) {
    if (!node) return NULL;
    switch (node->type) {
    case NODE_ARRAY_LIT:            return genArrayLitPtr       (ctx, node);
    case NODE_SLICE:                return genSlicePtr          (ctx, node);
    case NODE_ENUM_LIT:             return genEnumLitPtr        (ctx, node);
    case NODE_ENUM_LIT_NO_PAYLOAD:  return genEnumNoPayloadPtr  (ctx, node);
    case NODE_STRUCT_LIT:           return genStructLitPtr      (ctx, node);
    case NODE_TUPLE_LIT:            return genTupleLitPtr       (ctx, node);
    case NODE_MEMBER:               return genMemberAccessPtr   (ctx, node);
    case NODE_SUBSCRIPT:            return genSubscriptPtr      (ctx, node);
    case NODE_INTERPOLATION:        return genInterpolation     (ctx, node);
    case NODE_LITERAL:
        if (node->tok->type == TOK_STR_LIT)
            return genStrLitGlobal(ctx, node->tok);
        return NULL;
    case NODE_CALL: {
        genCall(ctx, node);
        ZMIRStack *stack = getStackValue(ctx, node);
        if (!stack) {
            zlog(ctx->state, node->tok, Z9028);
            return NULL;
        }
        return stack->stack;
    }
    case NODE_IDENTIFIER: {
        char *key = manglingIdent(node);
        ZMIRValueRef val = getZMIRValueRef(ctx, key);
        if (!val) {
            zlog(ctx->state, node->tok, Z301F, node->tok->str);
            return NULL;
        }
        return val;
    }
    case NODE_UNARY: {
        ZToken *operator = node->unary.operat;
        if (operator->type == TOK_STAR) {
            ZMIRValueRef ptr = genLValue(ctx, node->unary.operand);
            ZMIRTypeRef typeRef = genType(ctx, node->unary.operand->resolved);
            ptr = ZMIRBuildLoad2(ctx->builder, typeRef, ptr, label(ctx, node->tok));
            return ptr;
        } else if (operator->type == TOK_ESCL) {
            ZType *opt = node->unary.operand->resolved;
            if (!opt || opt->kind != Z_TYPE_OPTIONAL) {
                zlog(ctx->state, node->tok, Z301E, stype(opt));
                return NULL;
            }
            ZMIRValueRef ptr    = genLValue(ctx, node->unary.operand);
            if (!ptr) return NULL;
            ZMIRTypeRef optType = genType(ctx, opt);
            ZMIRValueRef val    = ZMIRBuildLoad2(
                ctx->builder, optType, ptr, label(ctx, "unwrap.load")
            );

            checkUnsafeUnwrap(ctx, val, opt, node->tok);
            if (opt->optional->kind == Z_TYPE_POINTER) return ptr;
            return ZMIRBuildStructGEP2(
                ctx->builder, optType, ptr, 0, label(ctx, "unwrap.ptr")
            );
        }

        zlog(ctx->state, node->tok, Z9029);
        return NULL;
    }
    default:
        zlog(ctx->state, node->tok, Z902A, node->type);
        return NULL;
    }
}

static bool typeIsUnsigned(ZType *type) {
    if (!type || type->kind != Z_TYPE_PRIMITIVE) return false;
    return tokmask(type->primitive.token, TOK_UNSIGNED);
}

static void storeSumVariant(ZCodegen *ctx, ZMIRValueRef alloca, ZMIRTypeRef sumZMIRType,
                            i32 tag, ZMIRValueRef val, ZType *valType) {
    if (tag < 0) {
        zlog(ctx->state, NULL, Z400A);
    }
    ZMIRBuildStore(
        ctx->builder,
        ZMIRConstInt(i8Type, (u32)tag, false),
        alloca
    );
    ZMIRValueRef bufPtr = ZMIRBuildStructGEP2(
        ctx->builder, sumZMIRType, alloca, 1, label(ctx, "sum.buf")
    );
    if (valType && val) {
        ZMIRTypeRef valZMIRType = genType(ctx, valType);
        ZMIRValueRef typedPtr = ZMIRBuildBitCast(
            ctx->builder, bufPtr, ZMIRPointerType(valZMIRType, 0), label(ctx, "sum.buf.typed")
        );
        ZMIRBuildStore(ctx->builder, val, typedPtr);
    }
}

/**
 * @brief Stores an inline-if branch value into a sum slot.
 *
 * After semantic analysis a branch feeding a sum is either a concrete variant
 * or already a full value of the same sum type. In the latter
 * case the struct is stored as-is; otherwise it is tagged as a variant.
 */
static void storeSumBranch(ZCodegen *ctx, ZMIRValueRef alloca, ZMIRTypeRef sumZMIRType,
                           ZType *sumType, ZMIRValueRef val, ZType *valType) {
    if (valType && valType->kind == Z_TYPE_SUM) {
        ZMIRBuildStore(ctx->builder, val, alloca);
        return;
    }
    i32 tag = sumTypeIndexOf(sumType, valType);
    if (tag == -1) {
        zlog(ctx->state, valType ? valType->tok : NULL, Z400B);
        return;
    }
    storeSumVariant(ctx, alloca, sumZMIRType, tag, val, valType);
}

static inline ZMIRValueRef genTruth(
    ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef cond = genExpr(ctx, node);
    if (!cond || node->type == NODE_VAR_DECL) return cond;
    return genCond(ctx, cond, node->resolved, Z_PREDICATE_NE);
}

/**
 * @brief Generates inline if.
 *
 * This uses a common instruction of ZMIR called phi.
 * In ZMIR values are constants and can't be reassigned.
 * So to generates the inline if three blocks are emitted:
 * - The branch for the true value
 * - The branch for the false value
 * - The branch that takes the value from the previous one.
 *
 * It generates the condition and the conditional branch.
 * Then append to the true and false branch their respectively expressions.
 *
 * At the end it generates the phi instruction.
 * It takes the value of the branch comes from.
 */
static ZMIRValueRef genInlineIf(ZCodegen *ctx, ZNode *node) {
    ZMIRStack *stack = NULL;
    ZMIRTypeRef sumZMIRType = NULL;
    if (node->resolved && node->resolved->kind == Z_TYPE_SUM) {
        stack = getStackValue(ctx, node);
        if (!stack) {
            zlog(ctx->state, node->tok, Z901B);
            return NULL;
        }
        sumZMIRType = genType(ctx, node->resolved);
    }
    ZMIRValueRef cond = genTruth(ctx, node->ifStmt.cond);
    ZMIRBasicBlockRef tBranch = makeblock(ctx, "true");
    ZMIRBasicBlockRef fBranch = makeblock(ctx, "false");
    ZMIRBasicBlockRef merge = makeblock(ctx, "endif");
    makecondbr(ctx->builder, cond, tBranch, fBranch);

    ZMIRPositionBuilderAtEnd(ctx->builder, tBranch);
    ZMIRValueRef tValue = genExpr(ctx, node->ifStmt.body);
    if (stack) {
        storeSumBranch(ctx, stack->stack, sumZMIRType,
                       node->resolved, tValue, node->ifStmt.body->resolved);
    }
    ZMIRBasicBlockRef tExit = ZMIRGetInsertBlock(ctx->builder);
    makebr(ctx->builder, merge);

    ZMIRPositionBuilderAtEnd(ctx->builder, fBranch);
    ZMIRValueRef fValue = genExpr(ctx, node->ifStmt.elseBranch);
    if (stack) {
        storeSumBranch(ctx, stack->stack, sumZMIRType,
                       node->resolved, fValue, node->ifStmt.elseBranch->resolved);
    }
    ZMIRBasicBlockRef fExit = ZMIRGetInsertBlock(ctx->builder);
    makebr(ctx->builder, merge);

    ZMIRPositionBuilderAtEnd(ctx->builder, merge);

    ZMIRTypeRef resultType = genType(ctx, node->resolved);
    if (stack) {
        return ZMIRBuildLoad2(
            ctx->builder,
            resultType,
            stack->stack,
            label(ctx, node->tok)
        );
    } else {
        ZMIRValueRef phi = ZMIRBuildPhi(
            ctx->builder, resultType, label(ctx, node->tok)
        );

        ZMIRValueRef vals[2]            = {tValue,  fValue};
        ZMIRBasicBlockRef branches[2]   = {tExit,   fExit};

        ZMIRAddIncoming(phi, vals, branches, 2);

        return phi;
    }
}

static ZMIRValueRef genNumericOperator(ZCodegen *ctx,
        ZMIRValueRef left, ZMIRValueRef right,
        ZType *leftType, ZType *rightType,
        ZTokenType op, ZToken *tok) {
    if (!left || !right) return NULL;

    ZMIRTypeRef left_type = ZMIRTypeOf(left);
    bool is_float = (ZMIRGetTypeKind(left_type) == ZMIRFloatTypeKind ||
                     ZMIRGetTypeKind(left_type) == ZMIRDoubleTypeKind);


    bool bothUnsigned = typeIsUnsigned(leftType) &&
                        typeIsUnsigned(rightType);

    char *l = label(ctx, tok);

    if (is_float) {
        switch (op) {
        case TOK_PLUS:  return ZMIRBuildFAdd(ctx->builder, left, right, l);
        case TOK_MINUS: return ZMIRBuildFSub(ctx->builder, left, right, l);
        case TOK_STAR:  return ZMIRBuildFMul(ctx->builder, left, right, l);
        case TOK_DIV:   return ZMIRBuildFDiv(ctx->builder, left, right, l);
        case TOK_MOD:   return ZMIRBuildFRem(ctx->builder, left, right, l);
        case TOK_LT:    return ZMIRBuildFCmp(ctx->builder, ZMIRRealOLT, left, right, l);
        case TOK_GT:    return ZMIRBuildFCmp(ctx->builder, ZMIRRealOGT, left, right, l);
        case TOK_LTE:   return ZMIRBuildFCmp(ctx->builder, ZMIRRealOLE, left, right, l);
        case TOK_GTE:   return ZMIRBuildFCmp(ctx->builder, ZMIRRealOGE, left, right, l);
        case TOK_EQEQ:  return ZMIRBuildFCmp(ctx->builder, ZMIRRealOEQ, left, right, l);
        case TOK_NOTEQ: return ZMIRBuildFCmp(ctx->builder, ZMIRRealONE, left, right, l);
        default:        zlog(ctx->state, tok, Z902B); return NULL;
        }
    }


    let div         = bothUnsigned ? ZMIRBuildUDiv : ZMIRBuildSDiv;
    let mod         = bothUnsigned ? ZMIRBuildURem : ZMIRBuildSRem;
    let rightShift  = bothUnsigned ? ZMIRBuildLShr : ZMIRBuildAShr;
    let lt          = bothUnsigned ? ZMIRIntULT : ZMIRIntSLT;
    let gt          = bothUnsigned ? ZMIRIntUGT : ZMIRIntSGT;
    let lte         = bothUnsigned ? ZMIRIntULE : ZMIRIntSLE;
    let gte         = bothUnsigned ? ZMIRIntUGE : ZMIRIntSGE;

    switch (op) {
    case TOK_PLUS:  return ZMIRBuildAdd (ctx->builder, left, right, l);
    case TOK_MINUS: return ZMIRBuildSub (ctx->builder, left, right, l);
    case TOK_STAR:  return ZMIRBuildMul (ctx->builder, left, right, l);
    case TOK_DIV:   return div          (ctx->builder, left, right, l);
    case TOK_MOD:   return mod          (ctx->builder, left, right, l);
    case TOK_LT:    return ZMIRBuildICmp(ctx->builder, lt,  left, right, l);
    case TOK_GT:    return ZMIRBuildICmp(ctx->builder, gt,  left, right, l);
    case TOK_LTE:   return ZMIRBuildICmp(ctx->builder, lte, left, right, l);
    case TOK_GTE:   return ZMIRBuildICmp(ctx->builder, gte, left, right, l);
    case TOK_EQEQ:  return ZMIRBuildICmp(ctx->builder, ZMIRIntEQ, left, right, l);
    case TOK_NOTEQ: return ZMIRBuildICmp(ctx->builder, ZMIRIntNE, left, right, l);
    case TOK_BITL:  return ZMIRBuildShl (ctx->builder, left, right, l);
    case TOK_BITR:  return rightShift   (ctx->builder, left, right, l);
    case TOK_BITOR: return ZMIRBuildOr  (ctx->builder, left, right, l);
    case TOK_BITXOR:return ZMIRBuildXor (ctx->builder, left, right, l);
    case TOK_REF:   return ZMIRBuildAnd (ctx->builder, left, right, l);
    default:        zlog(ctx->state, tok, Z902B); return NULL;
    }
}

static ZMIRValueRef genCompoundOperator(ZCodegen *ctx, ZNode *root) {
    ZMIRValueRef leftRef    = genLValue(ctx, root->binary.left);
    if (!leftRef) return NULL;
    ZMIRValueRef leftValue  = ZMIRBuildLoad2(ctx->builder,
        genType(ctx, root->binary.left->resolved),
        leftRef, label(ctx, "left")
    );

    ZMIRValueRef right = genExpr(ctx, root->binary.right);


    ZTokenType op;
    switch (root->binary.op->type) {
    case TOK_SELF_BITAND:   op = TOK_REF;       break;
    case TOK_SELF_BITL:     op = TOK_BITL;      break;
    case TOK_SELF_BITR:     op = TOK_BITR;      break;
    case TOK_SELF_BITOR:    op = TOK_BITOR;     break;
    case TOK_SELF_BITXOR:   op = TOK_BITXOR;    break;
    case TOK_SELF_PLUS:     op = TOK_PLUS;      break;
    case TOK_SELF_MINUS:    op = TOK_MINUS;     break;
    case TOK_SELF_MUL:      op = TOK_STAR;      break;
    case TOK_SELF_DIV:      op = TOK_DIV;       break;
    default:
        zlog(ctx->state, root->tok, Z902C);
        return NULL;
    }

    ZMIRValueRef value = genNumericOperator(ctx,
        leftValue, right,
        root->binary.left->resolved,
        root->binary.right->resolved,
        op, root->tok
    );
    if (!value) return NULL;

    ZMIRBuildStore(ctx->builder, value, leftRef);
    return value;
}

/**
 * @brief Generates all binary operators.
 *
 * Binary operators are divided into:
 * - Overrided operations
 * - Integer operations.
 * - Boolean operations.
 * - Assignment operations.
 */
static ZMIRValueRef genBinary(ZCodegen *ctx, ZNode *root) {
    if (root->binary.overload) {
        ZNode *fn = root->binary.overload;
        ZMIRValueRef func = getZMIRValueRef(ctx, fn->funcDef.mangled);
        if (!func) return NULL;
        ZMIRTypeRef funcType = ZMIRGlobalGetValueType(func);
        ZMIRValueRef *args = NULL;
        vecpush(args, genExpr(ctx, root->binary.left));
        vecpush(args, genExpr(ctx, root->binary.right));
        ZMIRValueRef call = ZMIRBuildCall2(
            ctx->builder, funcType, func,
            args, veclen(args),
            isVoid(root->resolved) ? "" : label(ctx, "binary")
        );
        if (!fitsInRegister(call)) {
            ZMIRStack *stack = getStackValue(ctx, root);
            if (stack) ZMIRBuildStore(ctx->builder, call, stack->stack);
        }
        return call;
    }
    if (root->binary.op->type == TOK_EQ) {
        ZMIRValueRef ptr = genLValue(ctx, root->binary.left);
        ZMIRValueRef val = genExpr(ctx, root->binary.right);
        if (!ptr) {
            zlog(ctx->state, root->tok, Z902D);
            return NULL;
        }
        if (!val) {
            zlog(ctx->state, root->tok, Z902E);
            return NULL;
        }
        ZMIRBuildStore(ctx->builder, val, ptr);
        return val;
    }

    ZTokenType op = root->binary.op->type;

    if (tokmask(root->binary.op, TOK_SELF_OPERATOR)) {
        return genCompoundOperator(ctx, root);
    }

    /* Logical operator. */
    if (op == TOK_AND || op == TOK_OR) {
        bool is_and = op == TOK_AND;

        ZMIRValueRef lv = genTruth(ctx, root->binary.left);
        if (!lv) return NULL;

        ZMIRBasicBlockRef entry_bb = ZMIRGetInsertBlock(ctx->builder);
        ZMIRBasicBlockRef rhs_bb   = makeblock(ctx, is_and ? "and_right" : "or_right");
        ZMIRBasicBlockRef merge_bb = makeblock(ctx, is_and ? "and_merge" : "or_merge");

        if (is_and)
            makecondbr(ctx->builder, lv, rhs_bb, merge_bb);
        else
            makecondbr(ctx->builder, lv, merge_bb, rhs_bb);

        ZMIRPositionBuilderAtEnd(ctx->builder, rhs_bb);
        ZMIRValueRef rv = genTruth(ctx, root->binary.right);
        if (!rv) return NULL;
        makebr(ctx->builder, merge_bb);
        ZMIRBasicBlockRef rhs_end   = ZMIRGetInsertBlock(ctx->builder);

        ZMIRPositionBuilderAtEnd(ctx->builder, merge_bb);
        ZMIRValueRef phi            = ZMIRBuildPhi(
            ctx->builder, i1Type, label(ctx, root->tok));
        ZMIRValueRef short_val      = ZMIRConstInt(i1Type, is_and ? 0 : 1, false);
        ZMIRValueRef  vals[2]       = { short_val,  rv      };
        ZMIRBasicBlockRef bbs[2]    = { entry_bb,   rhs_end };
        ZMIRAddIncoming(phi, vals, bbs, 2);
        return phi;
    }

    ZMIRValueRef left = genExpr(ctx, root->binary.left);
    ZMIRValueRef right = genExpr(ctx, root->binary.right);

    return genNumericOperator(ctx,
        left, right,
        root->binary.left->resolved,
        root->binary.right->resolved,
        op, root->tok
    );
}

static ZMIRValueRef genUnsafeUnwrap(ZCodegen *ctx, ZNode *node, ZMIRValueRef arg) {
    ZType *resolved = node->unary.operand->resolved;

    if (!typeKindIs(resolved->kind, TYPE_WRAPPER_MASK)) {
        zlog(ctx->state, node->tok, Z301E, stype(resolved));
        return NULL;
    }

    if (resolved->kind == Z_TYPE_OPTIONAL) {
        checkUnsafeUnwrap(ctx, arg, resolved, node->tok);

        if (resolved->optional->kind == Z_TYPE_POINTER) return arg;

        return ZMIRBuildExtractValue(
            ctx->builder, arg, 0, label(ctx, "unwrap.ptr")
        );
    } else if (resolved->kind == Z_TYPE_RESULT) {
        zlog(ctx->state, node->tok, Z902F);
        return NULL;
    } else {
        return NULL;
    }
}

static inline ZMIRValueRef genNot(ZCodegen *ctx, ZType *base, ZMIRValueRef val) {
    return genCond(ctx, val, base, Z_PREDICATE_EQ);
}

static ZMIRValueRef genUnary(ZCodegen *ctx, ZNode *node) {
    if (node->unary.operat->type == TOK_REF)
        return genLValue(ctx, node->unary.operand);

    ZMIRValueRef arg = genExpr(ctx, node->unary.operand);
    ZTokenType op = node->unary.operat->type;

    ZMIRTypeRef argType = ZMIRTypeOf(arg);
    bool isFloat = (ZMIRGetTypeKind(argType) == ZMIRFloatTypeKind ||
                     ZMIRGetTypeKind(argType) == ZMIRDoubleTypeKind);

    char *l = label(ctx, node->tok);
    switch (op) {
    case TOK_PLUS:
    case TOK_MINUS: {
        typedef ZMIRValueRef (*ZMIRBinary)(ZMIRBuilderRef, ZMIRValueRef, ZMIRValueRef, const char *);

        ZMIRBinary arr[][2] = {
            {ZMIRBuildAdd, ZMIRBuildFAdd},
            {ZMIRBuildSub, ZMIRBuildFSub}
        };
        ZMIRValueRef zero =  isFloat ?
            ZMIRConstReal   (argType, 0) :
            ZMIRConstInt    (argType, 0, 0);

        return arr[op == TOK_MINUS][isFloat](ctx->builder, zero, arg, l);
    }
    case TOK_STAR: {
        ZMIRTypeRef base = genType(ctx, node->resolved);
        ZMIRValueRef loaded = ZMIRBuildLoad2(ctx->builder, base, arg, l);
        /* Non-register loads (structs/enums) must land in the pre-allocated
         * slot so destructure/pattern-match callers can read from a stable
         * address  -  mirrors what genCall does for aggregate returns. */
        if (!fitsInRegister(loaded)) {
            ZMIRStack *stack = getStackValue(ctx, node);
            if (stack) ZMIRBuildStore(ctx->builder, loaded, stack->stack);
        }
        return loaded;
    }
    case TOK_NOT:   return genNot(ctx, node->unary.operand->resolved, arg);
    case TOK_REF:   return genLValue(ctx, node->unary.operand);
    case TOK_BITNOT: {
        ZMIRTypeRef ref         = ZMIRTypeOf(arg);
        ZMIRValueRef allOnes    = ZMIRConstAllOnes(ref);
        return ZMIRBuildXor(ctx->builder, arg, allOnes, l);
    }

    case TOK_ESCL:  return genUnsafeUnwrap(ctx, node, arg);
    default:
        zlog(ctx->state, node->unary.operat, Z9029);
        return NULL;
    }

    return NULL;
}

static inline bool isPointer(ZType *type) {
    if (type->kind == Z_TYPE_POINTER) return true;
    if (type->kind != Z_TYPE_OPTIONAL) return false;
    return type->optional->kind == Z_TYPE_POINTER;
}

/**
 * @brief Generates the explicit cast
 * Emit the appropriate ZMIR cast to convert val (of Zinc type `from`) to
 * Zinc type `to`. Returns val unchanged when the types are already equal.
 * It is used only to convert numieric types.
 */
static ZMIRValueRef castValue(ZCodegen *ctx, ZMIRValueRef val, ZType *from, ZType *to) {
    if (!val || !from || !to) return val;
    if (typesEqual(from, to)) return val;

    /* none (null pointer) is compatible with any pointer - val is already ptr null */
    if (from->kind == Z_TYPE_NONE && to->kind == Z_TYPE_POINTER)
        return val;

    ZMIRTypeRef toType = genType(ctx, to);
    if (!toType) return val;

    bool fromIsFloat = false, toIsFloat = false;
    bool fromIsPtr   = isPointer(from);
    bool toIsPtr     = isPointer(to);
    bool fromIsSigned = false;

    if (from->kind == Z_TYPE_PRIMITIVE) {
        ZToken *tt = from->primitive.token;
        fromIsFloat   = tokmask(tt, TOK_FLOAT);
        fromIsSigned  = tokmask(tt, TOK_SIGNED);
    }
    if (to->kind == Z_TYPE_PRIMITIVE) {
        ZTokenType tt = to->primitive.token->type;
        toIsFloat = (tt == TOK_F32 || tt == TOK_F64);
    }

    char *l = label(ctx, "cast");
    unsigned toBits = ZMIRGetTypeKind(toType) == ZMIRIntegerTypeKind
        ? ZMIRGetIntTypeWidth(toType) : 0;

    if (toBits == 1) {
        ZMIRValueRef zero;
        if (fromIsPtr) {
            zero = ZMIRConstNull(ZMIRTypeOf(val));
            return ZMIRBuildICmp(ctx->builder, ZMIRIntNE, val, zero, l);
        }
        if (fromIsFloat) {
            zero = ZMIRConstReal(ZMIRTypeOf(val), 0.0);
            return ZMIRBuildFCmp(ctx->builder, ZMIRRealONE, val, zero, l);
        }
        zero = ZMIRConstInt(ZMIRTypeOf(val), 0, false);
        return ZMIRBuildICmp(ctx->builder, ZMIRIntNE, val, zero, l);
    }

    if (fromIsPtr && toIsPtr)
        return ZMIRBuildBitCast(ctx->builder, val, toType, l);

    if (fromIsPtr && !toIsFloat)
        return ZMIRBuildPtrToInt(ctx->builder, val, toType, l);

    if (!fromIsFloat && toIsPtr)
        return ZMIRBuildIntToPtr(ctx->builder, val, toType, l);

    if (fromIsFloat && toIsFloat) {
        if (from->primitive.token->type == TOK_F64 &&
            to->primitive.token->type   == TOK_F32)
            return ZMIRBuildFPTrunc(ctx->builder, val, toType, l);
        return ZMIRBuildFPExt(ctx->builder, val, toType, l);
    }

    if (fromIsFloat)
        return fromIsSigned
            ? ZMIRBuildFPToSI(ctx->builder, val, toType, l)
            : ZMIRBuildFPToUI(ctx->builder, val, toType, l);

    if (toIsFloat)
        return fromIsSigned
            ? ZMIRBuildSIToFP(ctx->builder, val, toType, l)
            : ZMIRBuildUIToFP(ctx->builder, val, toType, l);

    /* Both integers: trunc or extend */
    ZMIRTypeRef fromType = ZMIRTypeOf(val);
    u32 fromBits = ZMIRGetIntTypeWidth(fromType);
    if (fromBits > toBits)
        return ZMIRBuildTrunc(ctx->builder, val, toType, l);
    if (fromBits < toBits)
        return fromIsSigned
            ? ZMIRBuildSExt(ctx->builder, val, toType, l)
            : ZMIRBuildZExt(ctx->builder, val, toType, l);

    return ZMIRBuildBitCast(ctx->builder, val, toType, l);
}

static ZMIRValueRef genFacetConstruct(ZCodegen *ctx,
    ZMIRStack *stack, ZType *facet, ZNode *obj) {
    ZMIRValueRef objRef = genExpr(ctx, obj);
    if (!stack) {
        zlog(ctx->state, obj->tok, Z901B);
        return NULL;
    }

    /* A `type` value is a `*Type` at runtime, so it uses that vtable. */
    ZType *objType      = obj->resolved;
    ZType typeInfoPtr   = { .kind = Z_TYPE_POINTER };
    if (objType && objType->kind == Z_TYPE_TYPE && LangItems[Z_LANG_TYPE_INFO]) {
        typeInfoPtr.base    = LangItems[Z_LANG_TYPE_INFO]->resolved;
        objType             = &typeInfoPtr;
    }
    char *vtableName = encodeFacetVTable(facet, objType);
    ZMIRValueRef vtable = getZMIRValueRef(ctx, vtableName);

    if (!vtable) {
        zlog(ctx->state, obj->tok, Z9030);
        return NULL;
    }

    ZMIRValueRef vtablePtr = ZMIRBuildStructGEP2(
        ctx->builder, stack->stackType,
        stack->stack, 1, "facet.vtable"
    );

    ZMIRValueRef objPtr = ZMIRBuildStructGEP2(
        ctx->builder, stack->stackType,
        stack->stack, 0, "facet.obj"
    );

    ZMIRBuildStore(ctx->builder, vtable, vtablePtr);
    ZMIRBuildStore(ctx->builder, objRef, objPtr);

    /* Return the facet value (not the slot pointer) so assignment and other
     * consumers copy the whole {obj, vtable} struct rather than storing the
     * address of this temporary. Mirrors the sum-cast path above. */
    ZMIRTypeRef facetType = genType(ctx, facet);
    return ZMIRBuildLoad2(ctx->builder, facetType, stack->stack, label(ctx, obj->tok));
}

static ZMIRValueRef castToOptional(ZCodegen *ctx, ZNode *node) {
    ZType *from         = node->castExpr.expr->resolved;
    ZType *to           = node->castExpr.toType;
    ZMIRTypeRef toRef   = genType(ctx, to);

    if (from->kind == Z_TYPE_NONE) return ZMIRConstNull(toRef);

    ZMIRValueRef value = genExpr(ctx, node->castExpr.expr);

    if (isPointer(from) ||
        typesEqual(from, to)) return value;

    ZMIRValueRef stack      = ZMIRBuildAlloca(
        ctx->builder, genType(ctx, to), label(ctx, "optional"));
    ZMIRValueRef dataPtr    = ZMIRBuildStructGEP2(
        ctx->builder, toRef,
        stack, 0, label(ctx, "optional.data"));
    ZMIRValueRef isNonePtr  = ZMIRBuildStructGEP2(
        ctx->builder, toRef,
        stack, 1, label(ctx, "optional.flag"));

    ZMIRBuildStore(ctx->builder, value, dataPtr);
    ZMIRBuildStore(
        ctx->builder,
        ZMIRConstInt(i1Type, 1, false),
        isNonePtr
    );

    return ZMIRBuildLoad2(
        ctx->builder, toRef, stack, label(ctx, "optional.load")
    );
}

static ZMIRValueRef genCast(ZCodegen *ctx, ZNode *node) {
    ZType *from = node->castExpr.expr->resolved;
    ZType *to   = node->castExpr.toType;

    if (to->kind == Z_TYPE_SUM && from->kind != Z_TYPE_SUM) {
        ZMIRStack *stack = getStackValue(ctx, node);
        if (!stack) {
            zlog(ctx->state, node->tok, Z9031);
            return NULL;
        }
        ZMIRValueRef val = genExpr(ctx, node->castExpr.expr);
        i32 tag = sumTypeIndexOf(to, from);
        if (tag == -1) {
            zlog(ctx->state, node->tok, Z400C);
            return NULL;
        }
        ZMIRTypeRef sumZMIRType = genType(ctx, to);
        storeSumVariant(ctx, stack->stack, sumZMIRType, tag, val, from);
        return ZMIRBuildLoad2(ctx->builder, sumZMIRType, stack->stack, label(ctx, node->tok));
    }

    if (to->kind == Z_TYPE_OPTIONAL) return castToOptional(ctx, node);

    if (to->kind == Z_TYPE_FACET && from->kind != Z_TYPE_FACET) {
        ZMIRStack *stack = getStackValue(ctx, node);
        return genFacetConstruct(ctx, stack, to, node->castExpr.expr);
    }

    if (typesEqual(from,    consts(ctx->state, strType)) &&
        typesEqual(to,      consts(ctx->state, interpType))) {
        ZMIRStack *stack = getStackValue(ctx, node);
        if (!stack) {
            zlog(ctx->state, node->tok, Z901B);
            return NULL;
        }
        storeArray(ctx, stack, ZMIRConstInt(i64Type, 1, false));

        ZMIRValueRef zero = ZMIRConstInt(i64Type, 0, false);
        ZMIRValueRef slot = ZMIRBuildGEP2(
            ctx->builder,
            genType(ctx, LangItems[Z_LANG_INTERPOLATED_STRING]->resolved),
            stack->elem, &zero, 1, label(ctx, "interp.slot")
        );
        storeInterpolation(ctx, slot, &(ZInterpolation){
            .type       = Z_INTERP_LIT,
            .literal    = node->castExpr.expr->tok
        });
        return ZMIRBuildLoad2(
            ctx->builder,
            genType(ctx, to), stack->stack, label(ctx, "interp.load")
        );
    }

    if (from->kind == Z_TYPE_ARRAY && to->kind == Z_TYPE_POINTER) {
        ZMIRValueRef desc = genLValue(ctx, node->castExpr.expr);
        if (!desc) {
            zlog(ctx->state, node->tok, Z9028);
            return NULL;
        }
        ZMIRValueRef field = ZMIRBuildStructGEP2(
            ctx->builder, genType(ctx, from), desc, 1, label(ctx, "array.ptr")
        );
        return ZMIRBuildLoad2(
            ctx->builder, genType(ctx, to), field, label(ctx, "array.data")
        );
    }

    /* Array-literal cast: [n]T as []U - write each element directly into
     * the pre-allocated slot with per-element casting.
     * genArrayLit can't be used here because the stack slot is keyed on
     * this cast node, not on the inner array-literal node. */
    if (from->kind == Z_TYPE_ARRAY && to->kind == Z_TYPE_ARRAY &&
        node->castExpr.expr->type == NODE_ARRAY_LIT) {
        ZMIRStack *stack = getStackValue(ctx, node);
        if (!stack) {
            zlog(ctx->state, node->tok, Z9032);
            return NULL;
        }
        ZNode *lit = node->castExpr.expr;
        for (usize i = 0; i < veclen(lit->arraylit); i++) {
            ZMIRValueRef indices[] = {
                ZMIRConstInt(i32Type, 0, false),
                ZMIRConstInt(i32Type, i, false)
            };
            ZMIRValueRef gep = ZMIRBuildGEP2(
                ctx->builder, stack->stackType,
                stack->stack, indices, 2, label(ctx, "cast"));
            ZMIRValueRef elem = genExpr(ctx, lit->arraylit[i]);
            elem = castValue(ctx, elem, from->array.base, to->array.base);
            ZMIRBuildStore(ctx->builder, elem, gep);
        }
        return stack->stack;
    }

    ZMIRValueRef val = genExpr(ctx, node->castExpr.expr);
    return castValue(ctx, val, from, to);
}

/* dest: optional pre-allocated slot to write into (e.g. an array element GEP).
   If NULL, a fresh alloca is emitted. Returns the destination pointer. */
static ZMIRValueRef genStructLitInto(
        ZCodegen *ctx, ZNode *node, ZMIRValueRef dest) {
    ZMIRTypeRef structType  = genType(ctx, node->resolved);
    ZMIRValueRef ptr        = dest ? dest :
                                ZMIRBuildAlloca(
                                        ctx->builder, structType, label(ctx, node->tok));
    ZNode **fields          = node->structlit.fields;

    for (usize i = 0; i < veclen(fields); i++) {
        ZNode *var = fields[i];
        ZToken *name = var->varDecl.pattern->tok;
        if (!var->varDecl.rvalue) {
            zlog(ctx->state, var->tok, Z400D, name);
        }
        ZMIRValueRef val = genExpr(ctx, var->varDecl.rvalue);

        u32 *path = getStructIndex(node->resolved, name->str);
        ZNode *field = getStructField(node->resolved, name->str);

        if (!field) {
            zlog(ctx->state, name, Z400E, name->str);
            continue;
        }

        ZType *fieldType = field->field.type;
        val = castValue(ctx, val, var->varDecl.rvalue->resolved, fieldType);

        ZMIRValueRef fieldPtr = genStructGEPChain(
            ctx, node->resolved, ptr, path
        );

        ZMIRBuildStore(ctx->builder, val, fieldPtr);
    }
    return ptr;
}

static void matchPattern(
    ZCodegen *,             ZType *,
    ZVarDestructPattern *,  ZMIRValueRef, ZMIRBasicBlockRef
);

static void matchTuplePattern(
        ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
        ZMIRValueRef ptr, ZMIRBasicBlockRef failBranch) {
    ZMIRTypeRef typeRef = genType(ctx, type);
    for (usize i = 0; i < veclen(pattern->tuple); i++) {
        ZMIRValueRef fieldPtr = ZMIRBuildStructGEP2(
            ctx->builder, typeRef, ptr, i, label(ctx, pattern->tuple[i]->tok));
        matchPattern(
            ctx, type->tuple[i],
            pattern->tuple[i], fieldPtr, failBranch);
    }
}

static void matchStructPattern(
        ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
        ZMIRValueRef ptr, ZMIRBasicBlockRef failBranch) {
    ZMIRTypeRef typeRef = genType(ctx, type);
    for (usize i = 0; i < veclen(pattern->fields); i++) {
        ZMIRValueRef fieldPtr = ZMIRBuildStructGEP2(
            ctx->builder, typeRef, ptr, i, label(ctx, pattern->fields[i]->key)
        );
        matchPattern(
            ctx, type->strct.fields[i]->resolved,
            pattern->fields[i]->value, fieldPtr, failBranch
        );
    }
}

static void matchEnumPattern(
        ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
        ZMIRValueRef ptr, ZMIRBasicBlockRef failBranch) {
    i32 variantIndex = enumIndexField(type, pattern->prop);

    if (variantIndex == -1) {
        zlog(ctx->state, pattern->prop, Z4010, stoken(pattern->prop));
        return;
    }
    ZType *variantType = type->enm.fields[variantIndex]->resolved;

    ZMIRValueRef tagPtr = ZMIRBuildStructGEP2(
        ctx->builder, genType(ctx, type), ptr, 0, label(ctx, pattern->prop));

    ZMIRValueRef tag = ZMIRBuildLoad2(
        ctx->builder, i8Type, tagPtr, label(ctx, pattern->prop)
    );

    ZMIRValueRef cond = ZMIRBuildICmp(
        ctx->builder, ZMIRIntEQ, tag,
        ZMIRConstInt(i8Type, variantIndex, false),
        label(ctx, pattern->prop)
    );

    ZMIRBasicBlockRef entry = makeblock(ctx, "match");
    makecondbr(ctx->builder, cond,
            entry, failBranch);

    ZMIRPositionBuilderAtEnd(ctx->builder, entry);
    ZMIRTypeRef variantTypeRef = genType(ctx, variantType);

    for (usize i = 0; i < veclen(pattern->args); i++) {
        ZMIRValueRef fieldPtr = ZMIRBuildStructGEP2(
            ctx->builder, variantTypeRef, ptr,
            i + 1, label(ctx, pattern->args[i]->tok));

        matchPattern(ctx,
            variantType->strct.fields[i + 1]->resolved,
            pattern->args[i], fieldPtr, failBranch);
    }
}

static ZMIRValueRef genEqCmp(
    ZCodegen *ctx, ZMIRValueRef left, ZMIRValueRef right,
    bool negate) {
    char *l = "matchcmp";

    ZMIRTypeKind kind = ZMIRGetTypeKind(ZMIRTypeOf(left));

    if (kind == ZMIRFloatTypeKind || kind == ZMIRDoubleTypeKind) {
        return ZMIRBuildFCmp(
            ctx->builder, negate ? ZMIRRealONE : ZMIRRealOEQ, left, right, l
        );
    }

    return ZMIRBuildICmp(
        ctx->builder, negate ? ZMIRIntNE : ZMIRIntEQ, left, right, l
    );
}

static void matchLitPattern(
    ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
    ZMIRValueRef ptr, ZMIRBasicBlockRef failBranch) {
    ZMIRValueRef left       = ZMIRBuildLoad2(
        ctx->builder, genType(ctx, type), ptr, label(ctx, pattern->tok)
    );

    ZMIRValueRef right = genLitTok(ctx, pattern->ident, pattern->resolved);
    right = castValue(ctx, right, pattern->resolved, type);

    ZMIRValueRef cond = genEqCmp(ctx, left, right, false);
    ZMIRBasicBlockRef trueBranch = makeblock(ctx, "true");
    makecondbr(ctx->builder, cond, trueBranch, failBranch);
    ZMIRPositionBuilderAtEnd(ctx->builder, trueBranch);
}

static void matchSumPattern(
        ZCodegen *ctx, ZType *type, ZVarDestructPattern *pattern,
        ZMIRValueRef ptr, ZMIRBasicBlockRef failBranch) {
    i32 index = sumTypeIndexOf(type, pattern->sum.type);
    if (index == -1) {
        zlog(ctx->state, pattern->sum.type->tok, Z9035);
        return;
    }

    ZMIRValueRef flagPtr    = ZMIRBuildStructGEP2(
        ctx->builder, genType(ctx, type), ptr, 0, label(ctx, "sum.flag.ptr"));
    ZMIRValueRef right      = ZMIRBuildLoad2(
        ctx->builder, i8Type, flagPtr, label(ctx, "sum.flag"));
    ZMIRValueRef cond       = genEqCmp(
        ctx, ZMIRConstInt(i8Type, index, false), right, false
    );

    ZMIRBasicBlockRef trueBranch = makeblock(ctx, "sum.true");
    makecondbr(ctx->builder, cond, trueBranch, failBranch);
    ZMIRPositionBuilderAtEnd(ctx->builder, trueBranch);
}

static void matchPattern(
        ZCodegen *ctx,
        ZType *type,
        ZVarDestructPattern *pattern,
        ZMIRValueRef ptr,
        ZMIRBasicBlockRef failBranch) {
    switch (pattern->type) {
    case Z_VAR_IDENT:                                                              break;
    case Z_VAR_TUPLE:   matchTuplePattern   (ctx, type, pattern, ptr, failBranch); break;
    case Z_VAR_STRUCT:  matchStructPattern  (ctx, type, pattern, ptr, failBranch); break;
    case Z_VAR_ENUM:    matchEnumPattern    (ctx, type, pattern, ptr, failBranch); break;
    case Z_VAR_LIT:     matchLitPattern     (ctx, type, pattern, ptr, failBranch); break;
    case Z_VAR_SUM:     matchSumPattern     (ctx, type, pattern, ptr, failBranch); break;
    default:
        zlog(ctx->state, pattern->tok, Z9036, pattern->type);
        break;
    }
}

static ZMIRValueRef genMatchCond(
    ZCodegen *ctx,                  ZType *type,
    ZVarDestructPattern *pattern,   ZMIRValueRef ptr) {
    ZMIRBasicBlockRef success   = makeblock(ctx, "match.success");
    ZMIRBasicBlockRef failure   = makeblock(ctx, "match.failure");
    ZMIRBasicBlockRef merge     = makeblock(ctx, "match.merge");

    matchPattern(ctx, type, pattern, ptr, failure);
    makebr(ctx->builder, success);

    ZMIRPositionBuilderAtEnd(ctx->builder, success);
    ZMIRValueRef trueVal        = ZMIRConstInt(i1Type, 1, false);
    makebr(ctx->builder, merge);

    ZMIRPositionBuilderAtEnd(ctx->builder, failure);
    ZMIRValueRef falseVal       = ZMIRConstInt(i1Type, 0, false);
    makebr(ctx->builder, merge);

    ZMIRPositionBuilderAtEnd(ctx->builder, merge);
    ZMIRValueRef phi = ZMIRBuildPhi(ctx->builder, i1Type, label(ctx, pattern->tok));

    ZMIRAddIncoming(
        phi,
        (ZMIRValueRef[])        {   trueVal, falseVal   },
        (ZMIRBasicBlockRef[])   {   success, failure    },
        2
    );
    return phi;
}

/**
 * @brief Generate the address of an expression.
 *
 * Expressions that live in memory return their own address (see genLValue).
 * Everything else (lang items like ZINC_OS, binary ops, literals, ...) is
 * evaluated and spilled into a temporary stack slot.
 * */
static ZMIRValueRef genAddressable(ZCodegen *ctx, ZNode *node) {
    switch (node->type) {
    case NODE_IDENTIFIER:
        if (node->identNode.li) break;
        return genLValue(ctx, node);
    case NODE_CALL:
        if (getLangItemType(node->call.callee)) break;
        return genLValue(ctx, node);
    case NODE_UNARY:
        if (node->unary.operat->type != TOK_STAR) break;
        return genLValue(ctx, node);
    case NODE_ARRAY_LIT:
    case NODE_SLICE:
    case NODE_ENUM_LIT:
    case NODE_ENUM_LIT_NO_PAYLOAD:
    case NODE_STRUCT_LIT:
    case NODE_TUPLE_LIT:
    case NODE_MEMBER:
    case NODE_SUBSCRIPT:
        return genLValue(ctx, node);
    default: break;
    }

    ZMIRValueRef value  = genExpr(ctx, node);
    if (!value) return NULL;

    ZMIRValueRef tmp    = ZMIRBuildAlloca(
        ctx->builder, genType(ctx, node->resolved), label(ctx, "tmp")
    );
    ZMIRBuildStore(ctx->builder, value, tmp);
    return tmp;
}

static ZMIRValueRef genVarDestruct(ZCodegen *ctx, ZNode *node) {
    genVarDecl(ctx, node);
    ZNode *rvalue = node->varDecl.rvalue;
    ZMIRValueRef ptr = NULL;

    if (rvalue->type == NODE_IDENTIFIER && !rvalue->identNode.li) {
        char *key = manglingIdent(rvalue);
        ptr = getZMIRValueRef(ctx, key);
        if (!ptr) {
            zlog(ctx->state, rvalue->tok, Z4011, rvalue->tok->str);
            return NULL;
        }
    } else {
        ZMIRStack *stack = getStackValue(ctx, rvalue);
        ptr = stack ? stack->stack : genAddressable(ctx, rvalue);
        if (!ptr) {
            zlog(ctx->state, node->tok, Z901B);
            return NULL;
        }
    }

    putDestructuredPatternInStack(ctx, node->resolved, node->varDecl.pattern, ptr);
    return genMatchCond(ctx, node->resolved, node->varDecl.pattern, ptr);
}

static ZMIRValueRef genAnonFunc(ZCodegen *ctx, ZNode *node) {
    ZMIRTypeRef returnTypeRef   = genType(ctx, node->resolved->func.ret);
    usize argLen = veclen(node->resolved->func.args);
    usize capLen = veclen(node->resolved->func.capabilities);

    ZMIRTypeRef *arguments      = aalloc(
        ctx->module->allocator, sizeof(ZMIRTypeRef) * ((argLen + capLen) ? (argLen + capLen) : 1)
    );

    usize len = 0;

    for (usize i = 0; i < capLen; i++) {
        ZType *capType = node->resolved->func.capabilities[i];
        if (!isRuntimeParam(ctx->state, capType)) continue;
        arguments[len++] = genType(ctx, capType);
    }

    usize capParams = len;

    for (usize i = 0; i < argLen; i++) {
        ZType *at = node->resolved->func.args[i];
        if (!isRuntimeParam(ctx->state, at)) continue;
        arguments[len] = genType(ctx, at);
        if (at->kind == Z_TYPE_FUNCTION)
            arguments[len] = ZMIRPointerType(arguments[len], 0);
        len++;
    }
    ZMIRTypeRef funcTypeRef = ZMIRFunctionType(
        returnTypeRef, arguments, len, false);

    ZMIRValueRef func = ZMIRAddFunction(ctx->mod, node->funcDef.mangled, funcTypeRef);
    ZMIRSetLinkage(func, ZMIRInternalLinkage);

    ZMIRBasicBlockRef savedBlock   = ZMIRGetInsertBlock(ctx->builder);
    ZMIRValueRef savedFunc         = ctx->currentFunc;
    ZNode *savedFuncNode           = ctx->currentFuncNode;

    ctx->currentFunc     = func;
    ctx->currentFuncNode = node;

    beginScope(Z_SCOPE_FUNC, ctx);
    ZMIRBasicBlockRef entry = makeblock(ctx, "entry");
    ZMIRPositionBuilderAtEnd(ctx->builder, entry);

    addFuncArgs(ctx, func, node->funcDef.capabilities, 0, true);
    addFuncArgs(ctx, func, node->funcDef.args, capParams, false);

    genFuncVars(ctx, node->funcDef.body);
    genBlock(ctx, node->funcDef.body);

    genChainDefer(ctx, ctx->scope->parent);

    if (!ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) {
        if (ZMIRGetTypeKind(returnTypeRef) == ZMIRVoidTypeKind)
            ZMIRBuildRetVoid(ctx->builder);
        else
            ZMIRBuildRet(ctx->builder, ZMIRConstNull(returnTypeRef));
    }
    endScope(ctx);

    ctx->currentFunc     = savedFunc;
    ctx->currentFuncNode = savedFuncNode;
    ZMIRPositionBuilderAtEnd(ctx->builder, savedBlock);

    return func;
}

static ZMIRValueRef genBlockExpr(ZCodegen *ctx, ZNode *node) {
    for (usize i = 0; i < veclen(node->block); i++) {
        ZNode *stmt = node->block[i];
        if (stmt->type == NODE_BREAK && stmt->breakStmt.expr) {
            return genExpr(ctx, stmt->breakStmt.expr);
        } else {
            genStmt(ctx, stmt);
        }
    }
    return NULL;
}

ZMIRValueRef _getFlagOptional(ZCodegen *ctx,
        ZType *type, ZMIRValueRef value, ZMIRIntPredicate predicate) {
    ZType *base = type->optional;
    if (base->kind == Z_TYPE_POINTER) {
        return ZMIRBuildICmp(
            ctx->builder, predicate,
            value, ZMIRConstPointerNull(genType(ctx, base)),
            label(ctx, "optional.flag")
        );
    }

    ZMIRValueRef flag = ZMIRBuildExtractValue(
        ctx->builder, value, 1, label(ctx, "optional.flag")
    );

    return ZMIRBuildICmp(
        ctx->builder, predicate,
        flag, ZMIRConstNull(i1Type),
        label(ctx, "optional.flag")
    );
}

ZMIRValueRef getFlagOptional(ZCodegen *ctx,
        ZType *type, ZMIRValueRef value) {
    return _getFlagOptional(ctx, type, value, ZMIRIntEQ);
}

static inline ZMIRIntPredicate getIntPredicate(ZPredicate predicate) {
    switch (predicate) {
    case Z_PREDICATE_EQ:    return ZMIRIntEQ;
    case Z_PREDICATE_NE:    return ZMIRIntNE;
    default:                return ZMIRIntEQ;
    }
}

static inline ZMIRRealPredicate getRealPredicate(ZPredicate predicate) {
    switch (predicate) {
    case Z_PREDICATE_EQ:    return ZMIRRealOEQ;
    case Z_PREDICATE_NE:    return ZMIRRealUNE;
    default:                return ZMIRRealUNE;
    }
}

static ZMIRValueRef genCond(ZCodegen *ctx,
    ZMIRValueRef left, ZType *type, ZPredicate predicate) {
    if (!type || !left || !typeKindIs(type->kind, TYPE_COMPARABLE_MASK)) return NULL;
    ZMIRValueRef right = NULL;

    switch (type->kind) {
    case Z_TYPE_POINTER:
    case Z_TYPE_PRIMITIVE:
        right = ZMIRConstNull(genType(ctx, type));
        break;
    case Z_TYPE_OPTIONAL:
        if (type->optional->kind == Z_TYPE_POINTER) {
            right = ZMIRConstNull(genType(ctx, type->optional));
        } else {
            left = ZMIRBuildExtractValue(ctx->builder, left, 1, label(ctx, "cond"));
            right = ZMIRConstNull(i1Type);
        }
        break;
    case Z_TYPE_RESULT:
        right = ZMIRConstNull(i8Type);
        left = ZMIRBuildExtractValue(ctx->builder, left, 1, label(ctx, "cond"));
        break;
    default:
        error(ctx->state, type->tok, "Invalid type %s", stype(type));
        return NULL;
    }

    ZMIRTypeKind kind = ZMIRGetTypeKind(ZMIRTypeOf(left));
    if (kind == ZMIRFloatTypeKind || kind == ZMIRDoubleTypeKind) {
        return ZMIRBuildFCmp(
            ctx->builder, getRealPredicate(predicate),
            left, right, label(ctx, "cond")
        );
    }
    return ZMIRBuildICmp(
        ctx->builder, getIntPredicate(predicate),
        left, right, label(ctx, "cond")
    );
}

static ZMIRValueRef genUnwrap(ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef base   = genExpr(ctx, node->unwrap.base);

    switch (node->unwrap.kind) {
    case UNWRAP_BREAK:
    case UNWRAP_CONTINUE:
    case UNWRAP_RETURN: {
        ZMIRValueRef cond = genCond(ctx, base, node->unwrap.base->resolved, Z_PREDICATE_NE);
        ZMIRBasicBlockRef success = makeblock(ctx, "unwrap.success");
        ZMIRBasicBlockRef then = makeblock(ctx, "unwrap.failure");


        makecondbr(ctx->builder, cond, then, success);
        ZMIRPositionBuilderAtEnd(ctx->builder, success);

        genStmt(ctx, node->unwrap.orExpr);

        ZMIRPositionBuilderAtEnd(ctx->builder, then);
        return base;
    }
    case UNWRAP_DO: {
        ZMIRTypeRef typeRef         = genType(ctx, node->resolved);
        ZMIRValueRef payload        = ZMIRBuildExtractValue(
            ctx->builder, base, 0, label(ctx, "unwrap.payload")
        );

        ZMIRValueRef cond           = genCond(
            ctx, base, node->unwrap.base->resolved, Z_PREDICATE_NE
        );

        ZMIRBasicBlockRef origin    = ZMIRGetInsertBlock(ctx->builder);
        ZMIRBasicBlockRef fail      = makeblock(ctx, "unwrap.fail");
        ZMIRBasicBlockRef merge     = makeblock(ctx, "unwrap.merge");

        makecondbr(ctx->builder, cond, merge, fail);
        ZMIRPositionBuilderAtEnd(ctx->builder, fail);
        ZMIRValueRef orexpr = genExpr(ctx, node->unwrap.orExpr);
        fail = ZMIRGetInsertBlock(ctx->builder);
        makebr(ctx->builder, merge);

        ZMIRPositionBuilderAtEnd(ctx->builder, merge);
        ZMIRValueRef phi = ZMIRBuildPhi(
            ctx->builder, typeRef, label(ctx, "unwrap.phi")
        );
        ZMIRAddIncoming(phi,
            (ZMIRValueRef[]){ payload, orexpr },
            (ZMIRBasicBlockRef[]){ origin, fail },
            2
        );
        return phi;
    }
    default: return NULL;
    }

    return NULL;
}

static inline ZMIRValueRef buildInterpolationValue(
    ZCodegen *ctx, ZInterpolation *interp) {
    ZMIRValueRef value = NULL;
    switch (interp->type) {
    case Z_INTERP_LIT:  return genLitTok(ctx, interp->literal, consts(ctx->state, strType));
    case Z_INTERP_EXPR: return genExpr(ctx, interp->expr);
    }
    ZToken *tok = interp->type == Z_INTERP_LIT ?
        interp->literal : interp->expr->tok;

    error(ctx->state, tok, "Invalid interpolation value");
    return value;
}

static inline void storeInterpolation(ZCodegen *ctx, ZMIRValueRef slot, ZInterpolation *interp) {
    ZType *enumType         = LangItems[Z_LANG_INTERPOLATED_STRING]->resolved;
    ZMIRTypeRef typeRef     = genType(ctx, enumType);
    ZMIRValueRef value      = buildInterpolationValue(ctx, interp);

    if (!value) return;

    usize tag               = interp->type == Z_INTERP_EXPR ? 1 : 0;
    ZType *variant          = enumType->enm.fields[tag]->resolved;
    ZMIRTypeRef variantRef  = genType(ctx, variant);

    ZMIRValueRef tagPtr     = ZMIRBuildStructGEP2(
        ctx->builder, typeRef, slot, 0, label(ctx, "interp.index"));
    ZMIRValueRef dataPtr    = ZMIRBuildStructGEP2(
        ctx->builder, variantRef, slot, 1, label(ctx, "interp.data"));

    ZMIRTypeRef enumTag     = enumType->enm.integer ?
        genPrimitiveType(ctx, enumType->enm.integer) : i8Type;

    ZMIRBuildStore(ctx->builder, ZMIRConstInt(enumTag, tag, false), tagPtr);
    ZMIRBuildStore(ctx->builder, value, dataPtr);
}

static ZMIRValueRef genInterpolation(ZCodegen *ctx, ZNode *node) {
    ZNode *strInterp    = LangItems[Z_LANG_INTERPOLATED_STRING];
    ZNode *writable     = LangItems[Z_LANG_WRITABLE];
    if (!strInterp) {
        error(ctx->state, node->tok, "Unable to compile interpolation due to missing lang-item 'interpolated_string', please define it!");
        return NULL;
    }

    if (!writable) {
        error(ctx->state, node->tok, "Unable to compile interpolation due to missing lang-item 'writable'");
        return NULL;
    }
    ZMIRStack *stack = getStackValue(ctx, node);
    if (!stack) {
        error(ctx->state, node->tok, "Missing stack value");
        return NULL;
    }

    ZMIRValueRef len = ZMIRConstInt(i64Type, veclen(node->interpolation), false);
    storeArray(ctx, stack, len);

    ZMIRTypeRef type = genType(ctx, strInterp->resolved);
    for (usize i = 0; i < veclen(node->interpolation); i++) {
        ZMIRValueRef index  = ZMIRConstInt(i64Type, i, false);
        ZMIRValueRef slot   = ZMIRBuildGEP2(
            ctx->builder,   type,   stack->elem,
            &index,         1,      label(ctx, "interpolation.index")
        );
        storeInterpolation(ctx, slot, node->interpolation[i]);
    }
    return stack->stack;
}

ZMIRValueRef genExpr(ZCodegen *ctx, ZNode *node) {
    ZMIRValueRef res = NULL;
    /* Only a type expression has a denoted type; a variable of type `type`
     * is loaded like any other value. */
    if (node && node->resolved && node->resolved->kind == Z_TYPE_TYPE &&
        node->resolved->typeType) {
        return genTypeType(ctx, node);
    }
    switch (node->type) {
    case NODE_TYPE:             res = genTypeType       (ctx, node); break;
    case NODE_CALL:             res = genCall           (ctx, node); break;
    case NODE_CAST:             res = genCast           (ctx, node); break;
    case NODE_UNARY:            res = genUnary          (ctx, node); break;
    case NODE_BINARY:           res = genBinary         (ctx, node); break;
    case NODE_LITERAL:          res = genLit            (ctx, node); break;
    case NODE_IDENTIFIER:       res = genIdent          (ctx, node); break;
    case NODE_UNWRAP:           res = genUnwrap         (ctx, node); break;
    case NODE_FUNC:             res = genAnonFunc       (ctx, node); break;
    case NODE_IF:               res = genInlineIf       (ctx, node); break;
    case NODE_BLOCK:            res = genBlockExpr      (ctx, node); break;
    case NODE_VAR_DECL:         res = genVarDestruct    (ctx, node); break;
    // case NODE_INTERPOLATION:    res = genInterpolation  (ctx, node); break;

    case NODE_MEMBER:
        if (node->memberAccess.object &&
            node->memberAccess.object->resolved &&
            node->memberAccess.object->resolved->kind == Z_TYPE_FACET) {
            res = genFacetMember(ctx, node);
            break;
        }
    case NODE_INTERPOLATION:
    case NODE_SUBSCRIPT:
    case NODE_TUPLE_LIT:
    case NODE_STRUCT_LIT:
    case NODE_ARRAY_LIT:
    case NODE_ENUM_LIT:
    case NODE_ENUM_LIT_NO_PAYLOAD:
    case NODE_SLICE: {
        ZMIRValueRef ptr = genLValue(ctx, node);
        /* Function fields are stored as ptr in structs (unsized types can't
         * be embedded directly), so load as ptr rather than the bare
         * function type. */
        ZMIRTypeRef loadType = genType(ctx, node->resolved);
        if (node->resolved->kind == Z_TYPE_FUNCTION) {
            loadType = ZMIRPointerType(loadType, 0);
        }
        res = ZMIRBuildLoad2(
            ctx->builder,   loadType,
            ptr,            label(ctx, node->tok)
        );
        break;
    }

    case NODE_SIZEOF: {
        usize size = typeSize(ctx->state, node->sizeofExpr.type);
        res = ZMIRConstInt(i64Type, (u64)size, /*sign_extend=*/0);
        break;
    }

    default:
        printf("Node '%d' not handled\n", node->type);
        zlog(ctx->state, node->tok, Z9037, node->type);
        break;
    }

    return res;
}

/* A Homogeneous Float Aggregate (HFA) is a struct with 1-4 fields all of the
 * same float type (f32 or f64).  HFAs are passed in SIMD/FP registers on both
 * AArch64 and x86-64, so they must NOT be repacked as integers. */
static bool isHFA(ZType *type) {
    if (type->kind != Z_TYPE_STRUCT) return false;
    usize n = veclen(type->strct.fields);
    if (n == 0 || n > 4) return false;

    ZType *first = type->strct.fields[0]->resolved;
    if (!first || first->kind != Z_TYPE_PRIMITIVE) return false;
    ZTokenType ft = first->primitive.token->type;
    if (ft != TOK_F32 && ft != TOK_F64) return false;

    for (usize i = 1; i < n; i++) {
        ZType *f = type->strct.fields[i]->resolved;
        if (!f || f->kind != Z_TYPE_PRIMITIVE) return false;
        if (f->primitive.token->type != ft) return false;
    }
    return true;
}

static ZMIRValueRef genForeign(ZCodegen *ctx, ZNode *node) {
    /* Foreign variables are emitted as globals in genForwardDecl; only
     * foreign functions produce a function declaration here. */
    if (node->resolved->kind != Z_TYPE_FUNCTION) return NULL;

    ZMIRTypeRef ret = genType(ctx, node->resolved->func.ret);
    usize argc = veclen(node->resolved->func.args);

    ZMIRTypeRef *paramTypes = aalloc(ctx->module->allocator, sizeof(ZMIRTypeRef) * (argc ? argc : 1));
    usize len = 0;
    for (usize i = 0; i < argc; i++) {
        ZType *at = node->resolved->func.args[i];
        if (!isRuntimeParam(ctx->state, at)) continue;
        paramTypes[len] = genType(ctx, at);
        if (at->kind == Z_TYPE_FUNCTION) {
            paramTypes[len] = ZMIRPointerType(paramTypes[len], 0);
        } else if (at->kind == Z_TYPE_STRUCT && !isHFA(at)) {
            /* C ABI on all supported targets (x86-64, AArch64) passes small
             * non-HFA structs as a packed integer of the same size.  HFAs use
             * SIMD/FP registers and ZMIR handles them correctly as-is. */
            usize sz = typeSize(ctx->state, at);
            if      (sz <= 4) paramTypes[len] = i32Type;
            else if (sz <= 8) paramTypes[len] = i64Type;
        }
        len++;
    }
    ZMIRTypeRef funcType = ZMIRFunctionType(
        ret,
        paramTypes,
        (unsigned)len,
        node->resolved->func.variadic
    );

    char *name = stoken(node->foreignDecl.name);
    ZMIRValueRef func = ZMIRGetNamedFunction(ctx->mod, name);
    if (!func) {
        func = ZMIRAddFunction(ctx->mod, name, funcType);
    }

    putZMIRValueRef(ctx, name, func);

    return func;
}

static void genRetChainDefer(ZCodegen *ctx) {
    ZMIRScope *scope = ctx->scope;

    while (scope && scope->type != Z_SCOPE_FUNC) {
        scope = scope->parent;
    }

    if (scope) scope = scope->parent;

    genChainDefer(ctx, scope);
}

static inline bool ZMIRCanInsertRet(ZCodegen *ctx) {
    return !ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder));
}

static void genRet(ZCodegen *ctx, ZNode *ret) {
    ZType *expectedFuncType = ctx->currentFuncNode->resolved;
    ZType *expectedRetType  = expectedFuncType->func.ret;
    if (!expectedRetType) {
        zlog(ctx->state, ret->tok, Z3043);
        return;
    }


    ZMIRValueRef val = ret->returnStmt.expr ?
        genExpr(ctx, ret->returnStmt.expr) :
        NULL;

    if (expectedRetType->kind == Z_TYPE_SUM) {
        ZType *exprType     = ret->resolved;

        if (exprType && exprType->kind != Z_TYPE_SUM) {
            i32 tag = sumTypeIndexOf(expectedRetType, exprType);
            if (tag == -1) {
                zlog(ctx->state, ret->tok, Z4012);
                return;
            }
            ZMIRTypeRef sumZMIRType = genType(ctx, expectedRetType);
            ZMIRValueRef alloca = ZMIRBuildAlloca(ctx->builder, sumZMIRType, "sum.ret");
            storeSumVariant(ctx, alloca, sumZMIRType, tag, val, exprType);
            val = ZMIRBuildLoad2(ctx->builder, sumZMIRType, alloca, "sum.ret.val");
        }
    } else if (!ret->returnStmt.expr || isVoid(ret->resolved)) {
        genRetChainDefer(ctx);
        if (!ZMIRCanInsertRet(ctx)) return;
        ZMIRBuildRetVoid(ctx->builder);
        return;
    }
    /* If the expression produced i1 (e.g. a comparison) but the
       function's declared return type is a wider integer, zero-extend. */
    ZMIRTypeRef funcType = ZMIRGlobalGetValueType(ctx->currentFunc);
    ZMIRTypeRef retType  = ZMIRGetReturnType(funcType);
    if (val && ZMIRTypeOf(val) == i1Type && retType != i1Type &&
            ZMIRGetTypeKind(retType) == ZMIRIntegerTypeKind) {
        val = ZMIRBuildZExt(ctx->builder, val, retType, label(ctx, ret->tok));
    }

    genRetChainDefer(ctx);

    if (!val) {
        zlog(ctx->state, ret->tok, Z9038);
        return;
    }

    if (ZMIRCanInsertRet(ctx)) ZMIRBuildRet(ctx->builder, val);
}

/**
 * @brief Generate if.
 */
static void genIf(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_BLOCK, ctx);
    bool hasElse = node->ifStmt.elseBranch != NULL;
    ZMIRBasicBlockRef then = makeblock(ctx, "if.then");

    ZMIRBasicBlockRef elseBranch = NULL;

    if (hasElse) elseBranch = makeblock(ctx, "if.else");

    ZMIRBasicBlockRef endif = makeblock(ctx, "endif");
    ZMIRBasicBlockRef nextBlock = hasElse ? elseBranch : endif;

    ZMIRValueRef cond = genTruth(ctx, node->ifStmt.cond);

    makecondbr(ctx->builder, cond, then, nextBlock);

    ZMIRPositionBuilderAtEnd(ctx->builder, then);
    genStmt(ctx, node->ifStmt.body);

    genChainDefer(ctx, ctx->scope->parent);

    if (!ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) {
        makebr(ctx->builder, endif);
    }

    if (hasElse) {
        ZMIRPositionBuilderAtEnd(ctx->builder, elseBranch);
        genStmt(ctx, node->ifStmt.elseBranch);

        genChainDefer(ctx, ctx->scope->parent);

        if (!ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) {
            makebr(ctx->builder, endif);
        }
    }

    ZMIRPositionBuilderAtEnd(ctx->builder, endif);
    endScope(ctx);
}

/**
 * @brief Generate while statement.
 *
 * The while statement generates three blocks:
 * - The entry block: where it checks the condition and emit a conditional jump.
 * - The body: it generates each statement of the while loop.
 * - The end block: it is the end of the block.
 *      it can be reached by the conditional jump of the entry block or
 *      a break statement inside the body.
 */
static void genWhile(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_LOOP, ctx);
    ZMIRBasicBlockRef entry     = makeblock(ctx, "for.entry");
    ZMIRBasicBlockRef block     = makeblock(ctx, "for.body");
    ZMIRBasicBlockRef endwhile  = makeblock(ctx, "for.end");
    ctx->scope->startLoop       = entry;
    ctx->scope->endLoop         = endwhile;

    makebr             (ctx->builder, entry);
    ZMIRPositionBuilderAtEnd(ctx->builder, entry);

    ZMIRValueRef cond = genTruth(ctx, node->whileStmt.cond);
    makecondbr(ctx->builder, cond, block, endwhile);

    ZMIRPositionBuilderAtEnd(ctx->builder, block);

    // Build block
    genStmt(ctx, node->whileStmt.branch);

    // Fire all defer statements
    genChainDefer(ctx, ctx->scope->parent);

    /* If the block contains a break or a continue this block is already terminated.*/
    if (!ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) {
        makebr(ctx->builder, entry);
    }

    ZMIRPositionBuilderAtEnd(ctx->builder, endwhile);

    endScope(ctx);
}

static void genForInRange(ZCodegen *ctx, ZNode *node) {
    if (node->forin.iter->type != NODE_RANGE) return;
    ZNode *iter             = node->forin.iter;
    ZVarDestructPattern *b  = node->forin.binding;
    ZMIRTypeRef type        = genType(ctx, iter->resolved);
    ZMIRValueRef startExpr  = genExpr(ctx, iter->binary.left);
    ZMIRValueRef endExpr    = genExpr(ctx, iter->binary.right);

    ZMIRBasicBlockRef org   = ZMIRGetInsertBlock(ctx->builder);
    ZMIRBasicBlockRef entry = makeblock(ctx, "forin.entry");
    ZMIRBasicBlockRef body  = makeblock(ctx, "forin.body");
    ZMIRBasicBlockRef end   = makeblock(ctx, "forin.end");
    ZMIRBasicBlockRef step  = makeblock(ctx, "forin.step");

    bool uns = typeIsUnsigned(iter->resolved);

    makebr(ctx->builder, entry);


    ZMIRPositionBuilderAtEnd(ctx->builder, entry);
    ZMIRValueRef curr = ZMIRBuildPhi(
        ctx->builder, type, label(ctx, "forin.i"));
    ZMIRAddIncoming(curr, &startExpr, &org, 1);


    ZMIRValueRef cond = ZMIRBuildICmp(
        ctx->builder, uns ? ZMIRIntULT : ZMIRIntSLT, curr, endExpr, label(ctx, "forin.cond")
    );
    makecondbr(ctx->builder, cond, body, end);

    ZMIRPositionBuilderAtEnd(ctx->builder, body);

    beginScope(Z_SCOPE_LOOP, ctx);
    ctx->scope->startLoop   = step;
    ctx->scope->endLoop     = end;

    putZMIRValue(ctx, stoken(b->tok), curr);

    genStmt(ctx, node->forin.body);

    // Fire all defer statements
    genChainDefer(ctx, ctx->scope->parent);

    endScope(ctx);

    makebr(ctx->builder, step);

    ZMIRPositionBuilderAtEnd(ctx->builder, step);
    ZMIRValueRef next = ZMIRBuildAdd(
        ctx->builder, curr,
        ZMIRConstInt(type, 1, false),
        label(ctx, "forin.next")
    );

    ZMIRAddIncoming(curr, &next, &step, 1);
    makebr(ctx->builder, entry);

    ZMIRPositionBuilderAtEnd(ctx->builder, end);
}

static void genForInArray(ZCodegen *ctx, ZNode *node) {
    ZType *arrType          = node->forin.iter->resolved;
    ZType *base             = arrType->array.base;
    ZNode *iter             = node->forin.iter;
    ZMIRValueRef arr        = genExpr(ctx, iter);
    ZMIRTypeRef typeRef     = genType(ctx, base);
    ZVarDestructPattern *b  = node->forin.binding;
    usize arrayPtrIndex     = arrType->array.dynamic ? 3 : 1;

    ZMIRBasicBlockRef org   = ZMIRGetInsertBlock(ctx->builder);
    ZMIRBasicBlockRef entry = makeblock(ctx, "forin.entry");
    ZMIRBasicBlockRef body  = makeblock(ctx, "forin.body");
    ZMIRBasicBlockRef step  = makeblock(ctx, "forin.step");
    ZMIRBasicBlockRef end   = makeblock(ctx, "forin.end");
    ZMIRValueRef startIdx   = ZMIRConstNull(usizeType);
    ZMIRValueRef arrayLen   = ZMIRBuildExtractValue(
        ctx->builder, arr, 0, label(ctx, "arr.len"));
    ZMIRValueRef arrayPtr   = ZMIRBuildExtractValue(
        ctx->builder, arr, arrayPtrIndex, label(ctx, "arr.ptr"));

    makebr(ctx->builder, entry);
    ZMIRPositionBuilderAtEnd(ctx->builder, entry);
    ZMIRValueRef curr = ZMIRBuildPhi(ctx->builder, i64Type, label(ctx, "forin.index"));
    ZMIRValueRef cond = ZMIRBuildICmp(
        ctx->builder, ZMIRIntULT, curr, arrayLen, label(ctx, "forin.cond")
    );
    makecondbr(ctx->builder, cond, body, end);

    ZMIRPositionBuilderAtEnd(ctx->builder, body);
    ZMIRAddIncoming(curr, &startIdx, &org, 1);

    beginScope(Z_SCOPE_LOOP, ctx);

    ZMIRScope *scope       = ctx->scope->parent;
    ctx->scope->startLoop   = step;
    ctx->scope->endLoop     = end;

    ZMIRValueRef item = ZMIRBuildGEP2(
        ctx->builder, typeRef, arrayPtr,
        &curr, 1, label(ctx, "forin.item")
    );

    if (b->type == Z_VAR_TUPLE) {
        putDestructuredPatternInStack(ctx, iter->resolved->array.base, b->tuple[0], item);
        putZMIRValue(ctx, b->tuple[1]->tok->str, curr);
    } else {
        putDestructuredPatternInStack(ctx, iter->resolved->array.base, b, item);
    }

    genStmt(ctx, node->forin.body);
    genChainDefer(ctx, scope);
    endScope(ctx);
    makebr(ctx->builder, step);

    ZMIRPositionBuilderAtEnd(ctx->builder, step);
    ZMIRValueRef next = ZMIRBuildAdd(
        ctx->builder, curr, ZMIRConstInt(usizeType, 1, false), label(ctx, "forin.next")
    );

    ZMIRAddIncoming(curr, &next, &step, 1);
    makebr(ctx->builder, entry);


    ZMIRPositionBuilderAtEnd(ctx->builder, end);
}

static void genForIn(ZCodegen *ctx, ZNode *node) {
    if (node->forin.iter->type == NODE_RANGE) {
        genForInRange(ctx, node);
        return;
    }

    ZNode *callNode = node->forin.iterNextRef;

    if (!callNode && node->forin.iter->resolved->kind == Z_TYPE_ARRAY) {
        genForInArray(ctx, node);
        return;
    }

    ZNode *member           = callNode->call.callee;
    ZMIRValueRef func       = getZMIRValueRef(ctx, member->memberAccess.mangled);
    ZMIRTypeRef funcType    = ZMIRGlobalGetValueType(func);
    ZMIRValueRef self       = genLValue(ctx,
        member->memberAccess.object->unary.operand
    );

    ZMIRBasicBlockRef entry = makeblock(ctx, "forin.entry");
    ZMIRBasicBlockRef body  = makeblock(ctx, "forin.body");
    ZMIRBasicBlockRef end   = makeblock(ctx, "forin.end");

    ZMIRStack *stack       = getStackValue(ctx, callNode);

    if (!stack) {
        zlog(ctx->state, node->tok, Z901B);
        return;
    }

    makebr(ctx->builder, entry);
    ZMIRPositionBuilderAtEnd(ctx->builder, entry);
    ZMIRValueRef call = ZMIRBuildCall2(
        ctx->builder,
        funcType,
        func,
        &self, 1, label(ctx, "iter.next"));

    /* An optional pointer uses the null pointer as its niche, so it has no
     * separate flag field and the payload is the returned value itself. */
    bool nichePtr = callNode->resolved->optional->kind == Z_TYPE_POINTER;

    if (nichePtr || !fitsInRegister(call)) {
        ZMIRBuildStore(ctx->builder, call, stack->stack);
    }

    ZMIRValueRef value = nichePtr ? call : stack->stack;

    value = ZMIRBuildLoad2(
        ctx->builder, genType(ctx, callNode->resolved), value, label(ctx, "optional.data")
    );

    ZMIRValueRef cond = _getFlagOptional(ctx, callNode->resolved,
        value, ZMIRIntNE);

    makecondbr(ctx->builder, cond, body, end);

    ZMIRPositionBuilderAtEnd(ctx->builder, body);

    ZMIRValueRef dataPtr = nichePtr
        ? stack->stack
        : ZMIRBuildStructGEP2(
            ctx->builder,
            genType(ctx, callNode->resolved),
            stack->stack, 0, label(ctx, "optional.data.ptr")
        );

    beginScope(Z_SCOPE_LOOP, ctx);
    putDestructuredPatternInStack(
        ctx, callNode->resolved->optional,
        node->forin.binding, dataPtr
    );
    genStmt(ctx, node->forin.body);
    endScope(ctx);

    ZMIRBuildBr(ctx->builder, entry);

    ZMIRPositionBuilderAtEnd(ctx->builder, end);
}

static void genBlock(ZCodegen *ctx, ZNode *block) {
    for (usize i = 0; i < veclen(block->block); i++) {
        genStmt(ctx, block->block[i]);
    }
}

/**
 * @brief Genearetes break instruction.
 *
 * The break statement is just a jump to the end of the block.
 * The end of the block is a label saved in the current scope
 * when a loop is generated.
 */
static void genBreak(ZCodegen *ctx, ZNode *node) {
    if (!ctx->scope->endLoop) {
        zlog(ctx->state, node->tok, Z302C);
        return;
    }

    ZMIRScope *scope = ctx->scope;
    while (scope && scope->type != Z_SCOPE_LOOP) {
        scope = scope->parent;
    }

    genChainDefer(ctx, scope);

    /* If the block contains a break or a continue this block is already terminated.*/
    if (!ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) {
        makebr(ctx->builder, ctx->scope->endLoop);
    }
}

/**
 * @brief Generate the continue instruction.
 *
 * The 'continue' instruction is just a jump to the start of the loop.
 * The start of the loop is a label saved during the loop generation.
 */
static void genContinue(ZCodegen *ctx, ZNode *node) {
    if (!ctx->scope->startLoop) {
        zlog(ctx->state, node->tok, Z302D);
        return;
    }

    ZMIRScope *scope = ctx->scope;
    while (scope && scope->type != Z_SCOPE_LOOP) {
        scope = scope->parent;
    }

    genChainDefer(ctx, scope);

    /* If the block contains a break or a continue this block is already terminated.*/
    if (!ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) {
        makebr(ctx->builder, ctx->scope->startLoop);
    }
}

/**
 * @brief Generate defer statement
 *
 * Defer statement is compiled using a per-scope stack.
 *
 * At compile-time, every scope (function body, if, loops ...)
 * owns its own defer stack. Each defer statement is pushed into the stack
 * in its enclosing scope.
 * At the end of the scope each defer statement is fired in reverse order.
 *
 * A special case is an early exit (break, continue or return). inside a nested
 * scope: The compiler walks up the chain from the current scope to the target scope,
 * emitting each defer stack in reverse order.
 *
 * Note: Since the stack is known at compile-time the defer statement
 * has zero overhead at runtime.
 *
 * Note: One defer statement can be duplicated depending on its 'destination'.
 * for example a defer inside a loop is compiled:
 * - one for the exit block.
 * - one for every break statement
 * - one for every continue statement
 * */
static void genDefer(ZCodegen *ctx, ZNode *node) {
    vecpush(ctx->scope->defers, node->deferStmt.expr);
}

static void genMatchStmt(ZCodegen *ctx, ZNode *node) {
    ZType *condType         = node->match.cond->resolved;
    ZMIRValueRef condPtr    = genAddressable(ctx, node->match.cond);
    if (!condPtr) return;
    usize armLen            = veclen(node->match.arms);
    ZMIRBasicBlockRef *blocks = aalloc(
        ctx->module->allocator, sizeof(ZMIRBasicBlockRef) * (armLen+1));

    blocks[0] = ZMIRGetInsertBlock(ctx->builder);
    for (usize i = 1; i <= armLen; i++) blocks[i] = makeblock(ctx, "matcharm");

    ZMIRBasicBlockRef merge = blocks[armLen];

    for (usize i = 0; i < armLen; i++) {
        ZNode *arm = node->match.arms[i];
        ZMIRBasicBlockRef block = blocks[i];
        ZMIRPositionBuilderAtEnd(ctx->builder, block);
        ZMIRValueRef matchArm = genMatchCond(
            ctx, condType, arm->matchArm.pattern, condPtr
        );

        ZMIRBasicBlockRef exprBlock = makeblock(ctx, "match.block");

        makecondbr(ctx->builder, matchArm, exprBlock, blocks[i+1]);
        ZMIRPositionBuilderAtEnd(ctx->builder, exprBlock);
        beginScope(Z_SCOPE_BLOCK, ctx);
        putDestructuredPatternInStack(
            ctx, node->match.cond->resolved, arm->matchArm.pattern, condPtr);
        if (arm->matchArm.expr->type == NODE_BLOCK) {
            genBlock(ctx, arm->matchArm.expr);
        } else {
            genExpr(ctx, arm->matchArm.expr);
        }
        endScope(ctx);
        makebr(ctx->builder, merge);
    }
    ZMIRPositionBuilderAtEnd(ctx->builder, merge);
}

/**
 * @breif Generate capability.
 *
 * Capabilities are compiled like normal variables.
 */
static void genCapability(ZCodegen *ctx, ZNode *node) {
    beginScope(Z_SCOPE_BLOCK, ctx);

    for (usize i = 0; i < veclen(node->capability.capabilities); i++) {
        ZNode *decl = node->capability.capabilities[i];
        ZMIRValueRef ref = NULL;
        if (decl->type == NODE_VAR_DECL) {
            genStmt(ctx, decl);
            if (decl->varDecl.pattern->type == Z_VAR_IDENT) {
                ref = getZMIRValueRef(ctx, stoken(decl->tok));
            }
        } else {
            ZMIRStack *stack = getStackValue(ctx, decl);
            if (!stack) {
                zlog(ctx->state, decl->tok, Z9018, stoken(decl->tok));
                continue;
            }
            ZMIRValueRef val = genExpr(ctx, decl);
            if (val && val != stack->stack)
                ZMIRBuildStore(ctx->builder, val, stack->stack);
            ref = stack->stack;
        }

        if (!ref) continue;
        ZMIRCapability *capability = copy(
            ctx->module->allocator, ZMIRCapability, ((ZMIRCapability){
                .capability = decl->resolved,
                .ref = ref
            })
        );
        vecpush(ctx->scope->capabilities, capability);
    }

    genStmt(ctx, node->capability.block);
    endScope(ctx);
}

static void genStmt(ZCodegen *ctx, ZNode *stmt) {
    if (!stmt) return;

    switch (stmt->type) {
    /* Variable already declared at the start of the function*/
    case NODE_IF:           genIf           (ctx, stmt);    break;
    case NODE_RETURN:       genRet          (ctx, stmt);    break;
    case NODE_CALL:         genCall         (ctx, stmt);    break;
    case NODE_BLOCK:        genBlock        (ctx, stmt);    break;
    case NODE_WHILE:        genWhile        (ctx, stmt);    break;
    case NODE_FORIN:        genForIn        (ctx, stmt);    break;
    case NODE_BREAK:        genBreak        (ctx, stmt);    break;
    case NODE_DEFER:        genDefer        (ctx, stmt);    break;
    case NODE_VAR_DECL:     genVarDecl      (ctx, stmt);    break;
    case NODE_CONTINUE:     genContinue     (ctx, stmt);    break;
    case NODE_MATCH:        genMatchStmt    (ctx, stmt);    break;
    case NODE_CAPABILITY:   genCapability   (ctx, stmt);    break;
    case NODE_NAMESPACE:    genNamespace    (ctx, stmt);    break;
    case NODE_FOREIGN:      genForeign      (ctx, stmt);    break;

    /* Type declarations inside a function does not compile. */
    case NODE_STRUCT:
    case NODE_ENUM:
    case NODE_TYPEDEF:
                            break;
    default: {
        ZMIRValueRef compiled = genExpr(ctx, stmt);
        if (!compiled) {
            zlog(ctx->state, stmt->tok, Z9039, stmt->type);
        }
        break;
    }
    }
}

/**
 * @brief Saves the stack allocation indexed by node.
 *
 * FIXME: Implement hashset for lookup node pointer avoiding duplicate stack allocations.
 */
static void addFuncVar(ZCodegen *ctx,
    ZMIRValueRef stack,
    ZMIRValueRef elem,
    ZMIRTypeRef stackType,
    ZMIRTypeRef elemType,
    ZNode *node) {
    ZMIRStack **allocations = ctx->scope->stackAlloca;
    usize len = veclen(allocations);
    for (usize i = len; i > 0; i--)
        if (allocations[i - 1]->node == node) return;

    ZMIRStack *item    = aalloc(ctx->module->allocator, sizeof(ZMIRStack));
    *item = (ZMIRStack){
        .stack          = stack,
        .elem           = elem,
        .stackType      = stackType,
        .elemType       = elemType,
        .node           = node,
    };
    vecpush(ctx->scope->stackAlloca, item);
}

static void _buildNestedEnumStackRef(ZCodegen *ctx,
        ZNode *node, ZMIRValueRef parent, ZToken *variant, ZNode **args) {
    i32 variantIndex = enumIndexField(node->resolved, variant);

    if (variantIndex == -1) {
        zlog(ctx->state, variant, Z9024);
        return;
    }

    ZType *variantType          = node->resolved->enm.fields[variantIndex]->resolved;
    ZMIRTypeRef variantTypeRef  = genType(ctx, variantType);

    for (usize i = 0; i < veclen(args); i++) {
        ZNode *val = args[i];

        ZMIRValueRef ptr = ZMIRBuildStructGEP2(
            ctx->builder, variantTypeRef, parent, i + 1, label(ctx, variant)
        );

        ZMIRTypeRef fieldType = genType(ctx, variantType->strct.fields[i]->resolved);
        addFuncVar(ctx, ptr, NULL, fieldType, NULL, val);
        buildNestedFuncVar(ctx, val, ptr);
    }
}

/**
 * @brief Stores nested pointer for a given stack allocation.
 *
 * Saves the pointer for nested struct literals to avoi creating
 * another stack allocation.
 * Example:
 * MyStruct{
 *   field: OtherStruct{ ...fields }
 * }
 *
 * In this case the stack allocation has the size of MyStruct.
 * For the nested struct (OtherStruct) It uses just a pointer
 * to the previous stack allocation.
 *
 * */
static void buildNestedFuncVar(
    ZCodegen *ctx, ZNode *node, ZMIRValueRef parent) {
    switch (node->type) {
    case NODE_STRUCT_LIT: {
        ZNode **fields      = node->structlit.fields;

        for (usize i = 0; i < veclen(fields); i++) {
            ZNode *val      = fields[i]->varDecl.rvalue;
            ZToken *name    = fields[i]->varDecl.pattern->tok;
            ZType *type     = val->resolved;

            if (type->kind == Z_TYPE_PRIMITIVE  ||
                type->kind == Z_TYPE_FUNCTION   ||
                type->kind == Z_TYPE_POINTER    ) {
                continue;
            }

            u32 *path       = getStructIndex(
                node->resolved,
                name->str
            );

            if (veclen(path) == 0) {
                zlog(ctx->state, name, Z9024);
                continue;
            }

            ZMIRValueRef ptr = genStructGEPChain(
                ctx, node->resolved, parent, path
            );

            ZMIRTypeRef typeRef = genType(ctx, type);

            addFuncVar(ctx, ptr, NULL, typeRef, NULL, val);
            buildNestedFuncVar(ctx, val, ptr);
        }
        break;
    }
    case NODE_TUPLE_LIT: {
        ZNode **fields          = node->tuplelit;
        ZMIRTypeRef tupleRef     = genType(ctx, node->resolved);

        for (usize i = 0; i < veclen(fields); i++) {
            ZNode *val = node->tuplelit[i];
            ZMIRValueRef ptr    = ZMIRBuildStructGEP2(
                ctx->builder, tupleRef, parent,
                i, label(ctx, val->tok)
            );

            ZMIRTypeRef typeRef = genType(ctx, val->resolved);
            addFuncVar(ctx, ptr, NULL, typeRef, NULL, val);
            buildNestedFuncVar(ctx, val, ptr);
        }
        break;
    }
    case NODE_ARRAY_LIT: {
        ZMIRTypeRef elemType = genType(ctx, node->resolved->array.base);
        for (usize i = 0; i < veclen(node->arraylit); i++) {
            ZNode *val  = node->arraylit[i];
            ZMIRValueRef index = ZMIRConstInt(i32Type, i, false);
            ZMIRValueRef ptr = ZMIRBuildGEP2(
                ctx->builder, elemType, parent, &index, 1, "index"
            );

            ZMIRTypeRef typeRef = genType(ctx, val->resolved);
            ZMIRValueRef innerElem = NULL;
            ZMIRTypeRef innerElemType = NULL;
            ZMIRValueRef nestedParent = ptr;
            if (val->resolved->kind == Z_TYPE_ARRAY && val->resolved->array.size > 0) {
                innerElemType = ZMIRArrayType2(
                    genType(ctx, val->resolved->array.base),
                    val->resolved->array.size
                );
                innerElem = ZMIRBuildAlloca(ctx->builder, innerElemType, label(ctx, val->tok));
                nestedParent = innerElem;
            }
            addFuncVar(ctx, ptr, innerElem, typeRef, innerElemType, val);
            buildNestedFuncVar(ctx, val, nestedParent);
        }
        break;
    }
    case NODE_ENUM_LIT:
        _buildNestedEnumStackRef(
            ctx, node, parent,
            node->call.callee->memberAccess.field,
            node->call.args
        );
        break;
    case NODE_ENUM_LIT_NO_PAYLOAD:
        _buildNestedEnumStackRef(
            ctx, node, parent, node->memberAccess.field, NULL
        );
        break;
    default: break;
    }
}

/*
 * @brief Returns the length of element should be allocated in the stack.
 * It's used only innside buildFuncVar.
 * */
static inline usize backingLen(ZState *state, ZNode *node) {
    ZType *type = node->resolved;
    if (!type || type->kind != Z_TYPE_ARRAY) return 0;
    if (type->array.size > 0) return type->array.size;
    ZType *interpType = consts(state, interpType);
    if (node->type == NODE_CAST && interpType && typesEqual(type, interpType))
        return 1;
    return 0;
}

/*
 * @brief Generates the stack allocation.
 *
 * Stores the node in a list of stack allocation such that in the second pass
 * the expression knows where it should be stored.
 * NOTE: this method stores always the expression that requires the stack allocation.
 * So for variable declaration always store the rvalue node and not the variable node.
 *
 * @param force Forces a stack allocation also when the size fits in a register.
 *
 * NOTE: Array literals generates two allocations:
 * - One for the metadata {len + ptr} (saved in stack)
 * - One for the items (saved in elem)
 * */
static ZMIRValueRef buildFuncVar(ZCodegen *ctx, ZNode *node, ZType *overrided, bool force) {
    if (!node) {
        zlog(ctx->state, NULL, Z903A);
        return NULL;
    } else if (!node->resolved || !overrided) {
        zlog(ctx->state, node->tok, Z903B, node->type);
        return NULL;
    }

    /* Function types are passed as pointers and fit in a register - no alloca needed. */
    if (!force && node->resolved->kind == Z_TYPE_FUNCTION) return NULL;

    ZMIRValueRef stackPointer = NULL;
    ZMIRValueRef elem = NULL;
    ZMIRTypeRef elemType = NULL;

    usize count = backingLen(ctx->state, node);
    if (count > 0) {
        ZMIRTypeRef baseType = genType(ctx, node->resolved->array.base);
        elemType = ZMIRArrayType2(baseType, count);
        elem = ZMIRBuildAlloca(ctx->builder, elemType, label(ctx, node->tok));
        initializeMemoryToZero(ctx, elem, elemType);
        stackPointer = elem;
    }

    ZMIRTypeRef type = genType(ctx, overrided);
    ZMIRValueRef val = ZMIRBuildAlloca(ctx->builder, type, label(ctx, node->tok));

    initializeMemoryToZero(ctx, val, type);

    addFuncVar(ctx, val, elem, type, elemType, node);

    if (!stackPointer) stackPointer = val;
    buildNestedFuncVar(ctx, node, stackPointer);
    return val;
}

/* @brief Walks the AST of a function block to create stack allocations.
 *
 * All variables of the function body are allocated at the start of the block.
 * This is the first pass where it navigates the AST
 * and allocate to the stack (ZMIRBuildAlloca) the variables and store the node
 * as allocated such that the function genExpr knows if the generated values
 * must be stored in the stack.
 * Now The stack allocations are just variable declarations and function calls
 * that return a non-primitive type (so a type that does not fit in a register).
 * */
static void genFuncVars(ZCodegen *ctx, ZNode *node) {
    if (!node) return;
    switch (node->type) {
    case NODE_RETURN:
        genFuncVars(ctx, node->returnStmt.expr);
        break;
    case NODE_TUPLE_LIT:
        buildFuncVar(ctx, node, node->resolved, false);
        for (usize i = 0; i < veclen(node->tuplelit); i++) {
            genFuncVars(ctx, node->tuplelit[i]);
        }
        break;
    case NODE_STRUCT_LIT:
        buildFuncVar(ctx, node, node->resolved, false);
        break;
    case NODE_ENUM_LIT:
        buildFuncVar(ctx, node, node->resolved, false);
        break;
    case NODE_ENUM_LIT_NO_PAYLOAD:
        buildFuncVar(ctx, node, node->resolved, false);
        break;
    case NODE_CAST:
        if (!getStackValue(ctx, node) && (
                node->resolved->kind == Z_TYPE_SUM      ||
                node->resolved->kind == Z_TYPE_FACET    ||
                backingLen(ctx->state, node) > 0))
            buildFuncVar(ctx, node, node->resolved, false);
        genFuncVars(ctx, node->castExpr.expr);
        break;
    case NODE_ARRAY_LIT:
        buildFuncVar(ctx, node, node->resolved, false);
        break;
    case NODE_MEMBER:
        genFuncVars(ctx, node->memberAccess.object);
        break;
    case NODE_SUBSCRIPT:
        genFuncVars(ctx, node->subscript.arr);
        genFuncVars(ctx, node->subscript.index);
        break;
    case NODE_WHILE:
        genFuncVars(ctx, node->whileStmt.cond);
        genFuncVars(ctx, node->whileStmt.branch);
        break;
    case NODE_FORIN:
        genFuncVars(ctx, node->forin.iterNextRef);
        genFuncVars(ctx, node->forin.body);
        break;
    case NODE_IF:
        if (node->resolved && node->resolved->kind == Z_TYPE_SUM) {
            buildFuncVar(ctx, node, node->resolved, false);
        }
        genFuncVars(ctx, node->ifStmt.cond);
        genFuncVars(ctx, node->ifStmt.body);
        genFuncVars(ctx, node->ifStmt.elseBranch);
        break;
    case NODE_CAPABILITY:
        for (usize i = 0; i < veclen(node->capability.capabilities); i++) {
            ZNode *cap = node->capability.capabilities[i];
            genFuncVars(ctx, cap);
            if (cap->type != NODE_VAR_DECL && !getStackValue(ctx, cap))
                buildFuncVar(ctx, cap, cap->resolved, true);
        }
        genFuncVars(ctx, node->capability.block);
        break;
    case NODE_UNARY:
        genFuncVars(ctx, node->unary.operand);
        break;
    case NODE_VAR_DECL: {
        ZNode *allocKey = node->varDecl.rvalue ? node->varDecl.rvalue : node;
        ZMIRValueRef ptr = buildFuncVar(ctx, allocKey, node->resolved, true);
        putDestructuredPatternInStack(ctx, node->resolved, node->varDecl.pattern, ptr);
        genFuncVars(ctx, node->varDecl.rvalue);
        break;
    }
    case NODE_CALL:
        if (!node->resolved) {
            ZLog *err = zlog(ctx->state, node->tok, Z00A2);
            emitHint(err, "callee type: %s", stype(node->call.callee->resolved));
            break;
        }
        genFuncVars(ctx, node->call.callee);
        if (!typesPrimitive(node->resolved)) {
            buildFuncVar(ctx, node, node->resolved, false);
        }
        for (usize i = 0; i < veclen(node->call.args); i++) {
            ZNode *arg = node->call.args[i];
            genFuncVars(ctx, arg);
            if (!typesPrimitive(arg->resolved) && !getStackValue(ctx, arg)) {
                buildFuncVar(ctx, arg, arg->resolved, false);
            }
        }
        break;

    case NODE_DEFER:
        genFuncVars(ctx, node->deferStmt.expr);
        break;
    case NODE_BINARY:
        genFuncVars(ctx, node->binary.left);
        genFuncVars(ctx, node->binary.right);
        break;
    case NODE_BLOCK:
        for (usize i = 0; i < veclen(node->block); i++) {
            genFuncVars(ctx, node->block[i]);
        }
        break;
    case NODE_MATCH_ARM:
        genFuncVars(ctx, node->matchArm.expr);
        break;
    case NODE_MATCH:
        genFuncVars(ctx, node->match.cond);
        for (usize i = 0; i < veclen(node->match.arms); i++) {
            genFuncVars(ctx, node->match.arms[i]);
        }
        break;
    case NODE_INTERPOLATION:
        if (!getStackValue(ctx, node))
            buildFuncVar(ctx, node, node->resolved, false);

        for (usize i = 0; i < veclen(node->interpolation); i++) {
            ZInterpolation *interp = node->interpolation[i];
            if (interp->type == Z_INTERP_EXPR)
                genFuncVars(ctx, interp->expr);
        }
    case NODE_UNWRAP:
        genFuncVars(ctx, node->unwrap.base);
        genFuncVars(ctx, node->unwrap.orExpr);
        break;
    default:
        break;
    }
}

/* Binds the parameters of the body being generated, starting at paramOffset,
 * and returns how many ZMIR parameters were consumed. The count is not
 * veclen(funcArgs) because zero sized ones have no parameter to bind, see
 * isRuntimeParam, so the caller needs it to place the next group. */
static usize addFuncArgs(ZCodegen *ctx,
        ZMIRValueRef func,
        ZNode **funcArgs, usize paramOffset, bool capabilityParams) {
    usize param = paramOffset;
    for (usize i = 0; i < veclen(funcArgs); i++) {
        char *name = funcArgs[i]->field.identifier->str;
        ZType *argType = funcArgs[i]->field.type;
        ZMIRTypeRef paramType = NULL;
        ZMIRValueRef slot = NULL;
        paramType = genType(ctx, argType);
        if (!isRuntimeParam(ctx->state, argType)) {
            /* Nothing was passed, but the name still has to resolve in the
             * body: give it a slot of its own (zero sized) instead. */
            slot = ZMIRBuildAlloca(ctx->builder, paramType, name);
        } else if (argType->kind == Z_TYPE_FUNCTION) {
            paramType = ZMIRPointerType(
                i8Type, 0
            );
            slot = ZMIRGetParam(func, param++);
        } else {
            slot = ZMIRBuildAlloca(ctx->builder, paramType, name);
            ZMIRBuildStore(ctx->builder, ZMIRGetParam(func, param++), slot);
        }

        putZMIRValueRef(ctx, name, slot);

        if (capabilityParams) {
            ZMIRCapability *capability = aalloc(
                ctx->module->allocator, sizeof(ZMIRCapability)
            );
            capability->capability  = argType;
            capability->ref         = slot;
            vecpush(ctx->scope->capabilities, capability);
        }
    }
    return param - paramOffset;
}

static void genFuncAttrs(ZCodegen *ctx, ZNode *f, ZMIRValueRef func) {
    (void)ctx; (void)f; (void)func;
    ZAnnotation **annotations = f->funcDef.annotations;

    ZAnnotation *export = query(annotations, "export");

    if (strcmp(f->funcDef.mangled, "main") == 0 || export) {
        ZMIRSetLinkage(func, ZMIRExternalLinkage);
    } else {
        ZMIRSetLinkage(func, ZMIRLinkOnceODRLinkage);
    }

    ZAnnotation *inl = query(annotations, "inline");

    if (!inl) goto cold;
    if (queryArg(inl, "always")) {
        ZMIRAddAttribute(func, "alwaysinline");
    } else if (queryArg(inl, "never")) {
        ZMIRAddAttribute(func, "noinline");
    } else {
        ZMIRAddAttribute(func, "inlinehint");
    }

cold:
    if (query(annotations, "cold")) {
        ZMIRAddAttribute(func, "cold");
    }
}

static ZMIRValueRef genFunc(ZCodegen *ctx, ZNode *f) {
    ZMIRTypeRef ret = genType(ctx, f->resolved->func.ret);
    ZMIRTypeRef *args = NULL;

    ZAnnotation *export = query(f->funcDef.annotations, "export");
    if (export) {
        f->funcDef.mangled = stoken(annArg(export, 0)->tok);
    }
    if (!f->funcDef.mangled) {
        f->funcDef.mangled = mangler((char *[]) {
            f->funcDef.name->filename,
            f->funcDef.name->str,
            NULL
        });
    }

    if (f->funcDef.receiver) {
        ZMIRTypeRef receiverType = genType(ctx, f->funcDef.receiver->resolved);
        vecpush(args, receiverType);
    }

    for (usize i = 0; i < veclen(f->funcDef.capabilities); i++) {
        ZType *capType = f->funcDef.capabilities[i]->field.type;
        if (!isRuntimeParam(ctx->state, capType)) continue;
        ZMIRTypeRef refType = genType(ctx, capType);
        vecpush(args, refType);
    }

    for (usize i = 0; i < veclen(f->funcDef.args); i++) {
        ZType *at = f->funcDef.args[i]->field.type;
        if (!isRuntimeParam(ctx->state, at)) continue;
        ZMIRTypeRef arg = genType(ctx, at);
        if (at->kind == Z_TYPE_FUNCTION)
            arg = ZMIRPointerType(arg, 0);
        vecpush(args, arg);
    }

    ZMIRTypeRef funcType    = ZMIRFunctionType(ret, args, veclen(args), false);
    ZMIRValueRef func       = ZMIRGetNamedFunction(ctx->mod, f->funcDef.mangled);
    if (!func) {
        func = ZMIRAddFunction(ctx->mod, f->funcDef.mangled, funcType);
    }
    ctx->currentFunc        = func;
    ctx->currentFuncNode    = f;

    genFuncAttrs(ctx, f, func);

    putZMIRValueRef(ctx, f->funcDef.mangled, func);
    beginScope(Z_SCOPE_FUNC, ctx);
    ZMIRBasicBlockRef entry = makeblock(ctx, "entry");
    ZMIRPositionBuilderAtEnd(ctx->builder, entry);

    /* Allocate a stack slot for each parameter so they can be reassigned.
     * The receiver (if present) occupies param index 0, so regular args
     * start at offset 1. */
    usize paramOffset = 0;
    if (f->funcDef.receiver) {
        char *name = f->funcDef.receiver->field.identifier->str;
        ZMIRTypeRef recType = genType(ctx, f->funcDef.receiver->resolved);
        ZMIRValueRef slot = ZMIRBuildAlloca(ctx->builder, recType, name);
        ZMIRBuildStore(ctx->builder, ZMIRGetParam(func, 0), slot);
        putZMIRValueRef(ctx, name, slot);
        paramOffset++;
    }

    usize capParams = addFuncArgs(
        ctx, func, f->funcDef.capabilities, paramOffset, true
    );
    addFuncArgs(
        ctx, func,
        f->funcDef.args,
        paramOffset + capParams,
        false
    );

    /* All variable declarations are declared at the start of the function. */
    genFuncVars(ctx, f->funcDef.body);

    genBlock(ctx, f->funcDef.body);

    genChainDefer(ctx, ctx->scope->parent);

    /* Add implicit return only if the block has no terminator yet. */
    if (!ZMIRGetBasicBlockTerminator(ZMIRGetInsertBlock(ctx->builder))) {
        ZMIRTypeRef funcType = ZMIRGlobalGetValueType(ctx->currentFunc);
        ZMIRTypeRef retType  = ZMIRGetReturnType(funcType);
        if (ZMIRGetTypeKind(retType) == ZMIRVoidTypeKind)
            ZMIRBuildRetVoid(ctx->builder);
        else
            ZMIRBuildRet(ctx->builder, ZMIRConstNull(retType));
    }
    endScope(ctx);

    return func;
}

static void compile(ZCodegen *, ZNode *);
static ZMIRValueRef genForwardDecl(ZCodegen *, ZNode *);

/* Returns the declarations of an imported module. A module is parsed exactly
 * once: the first import site owns the parsed list in module.root, while every
 * later re-import carries an empty module.root and reaches the same list
 * through module.cached. Codegen must follow cached so functions declared in a
 * module that this file only re-imports still get emitted into ctx->mod. */
static inline ZNode **moduleBody(ZNode *node) {
    return veclen(node->module.root) > 0 ? node->module.root
                                         : node->module.cached;
}

static void genNamespace(ZCodegen *ctx, ZNode *node) {
    for (usize i = 0; i < veclen(node->block); i++) {
        compile(ctx, node->block[i]);
    }
}

static void genImpl(ZCodegen *ctx, ZNode *root) {
    usize len = veclen(root->impl.funcs);
    ZMIRValueRef *ptrs = aalloc(
        ctx->module->allocator, sizeof(ZMIRValueRef) * len
    );
    for (usize i = 0; i < len; i++) {
        ptrs[i] = genFunc(ctx, root->impl.funcs[i]);

    }
}

static void genFacetVtable(ZCodegen *ctx, ZNode *root, ZMIRValueRef *ptrs) {
    for (usize i = 0; i < veclen(root->impl.facets); i++) {
        ZType *facet = root->impl.facets[i];

        if (!facet || facet->kind != Z_TYPE_FACET) {
            zlog(ctx->state, facet->tok, Z304D, stype(facet));
        }
        char *buf = encodeFacetVTable(facet, root->impl.base);

        if (ZMIRGetNamedGlobal(ctx->mod, buf)) continue;

        ZNode **facetFuncs = facet->facet.funcs;
        usize vlen = veclen(facetFuncs);
        ZMIRValueRef vtable[vlen ? vlen : 1];

        for (usize j = 0; j < vlen; j++) {
            ZToken *fname = facetFuncs[j]->field.identifier;
            ZMIRValueRef fn = NULL;

            for (usize k = 0; k < veclen(root->impl.funcs); k++) {
                if (tokeneq(fname, root->impl.funcs[k]->funcDef.name)) {
                    fn = ptrs[k];
                    break;
                }
            }
            if (!fn) {
                zlog(ctx->state, fname, Z305A, stype(facet), stoken(fname));
            }
            vtable[j] = fn;
        }

        ZMIRTypeRef ptrType     = ZMIRPointerTypeInContext(ctx->ctx, 0);
        ZMIRTypeRef arrType     = ZMIRArrayType2(ptrType, vlen);
        ZMIRValueRef val        = ZMIRConstArray2(ptrType, vtable, vlen);
        ZMIRValueRef globalRef  = ZMIRAddGlobal(ctx->mod, arrType, buf);

        ZMIRSetInitializer(globalRef, val);
        ZMIRSetAlignment(globalRef, 16);
        ZMIRSetLinkage(globalRef, ZMIRLinkOnceODRLinkage);

        putZMIRValueRef(ctx, buf, globalRef);
    }
}

static void compile(ZCodegen *ctx, ZNode *root) {
    beginModule(ctx, root);
    genForwardDecl(ctx, root);
    switch (root->type) {
    case NODE_FACET:        genFacetDecl(ctx, root->resolved);  break;
    case NODE_ENUM:
    case NODE_STRUCT:       genType     (ctx, root->resolved);  break;
    case NODE_FOREIGN:      genForeign  (ctx, root);            break;
    case NODE_FUNC:         genFunc     (ctx, root);            break;
    case NODE_NAMESPACE:    genNamespace(ctx, root);            break;
    case NODE_VAR_DECL:     genGlobalVar(ctx, root);            break;
    case NODE_TYPEDEF:
    case NODE_MACRO:        /* Doesn't generate anything. */    break;
    case NODE_IMPL:         genImpl     (ctx, root);            break;
    case NODE_MODULE: {
        /* Emit each module body at most once per ZMIR module: repeated imports
         * and cyclic imports would otherwise append a second definition to a
         * function that already has one. */
        if (!hashset_insert(&ctx->seenModuleDefs, root->module.filename)) break;
        ZNode **body = moduleBody(root);
        for (usize i = 0; i < veclen(body); i++)
            genForwardDecl(ctx, body[i]);
        for (usize i = 0; i < veclen(body); i++)
            compile(ctx, body[i]);
        break;
    }
    default:
        zlog(ctx->state, root->tok, Z903C, root->type);
        break;
    }
    endModule(ctx);
}

static ZMIRValueRef genForwardDecl(ZCodegen *ctx, ZNode *node) {
    if (!node) return NULL;
    switch (node->type) {
    case NODE_MODULE: {
        /* Follow module.cached for re-imports and guard against cycles, so
         * every reachable declaration lands in ctx->mod exactly once. */
        if (!hashset_insert(&ctx->seenModuleDecls, node->module.filename)) break;
        ZNode **body = moduleBody(node);
        for (usize i = 0; i < veclen(body); i++)
            genForwardDecl(ctx, body[i]);
        break;
    }
    case NODE_NAMESPACE:
        for (usize i = 0; i < veclen(node->block); i++)
            genForwardDecl(ctx, node->block[i]);
        break;
    case NODE_VAR_DECL: {
        ZVarDestructPattern *pat = node->varDecl.pattern;
        if (!pat || pat->type != Z_VAR_IDENT) break;
        ZMIRValueRef global = ZMIRGetNamedGlobal(ctx->mod, pat->ident->str);
        if (!global) {
            ZMIRTypeRef type = genType(ctx, node->resolved);
            global = ZMIRAddGlobal(ctx->mod, type, pat->ident->str);
            ZMIRSetLinkage(global, ZMIRInternalLinkage);
        }
        putZMIRValueRef(ctx, pat->ident->str, global);
        return global;
        break;
    }
    case NODE_IMPL: {
        ZNode **funcs = node->impl.funcs;
        ZMIRValueRef *ptrs = aalloc(
            ctx->module->allocator,
            sizeof(ZMIRValueRef) * veclen(funcs)
        );
        for (usize i = 0; i < veclen(funcs); i++) {
            ZMIRValueRef func = genForwardDecl(ctx, funcs[i]);
            ptrs[i] = func;
        }
        genFacetVtable(ctx, node, ptrs);
        return NULL;
    }
    case NODE_FOREIGN: {
        if (node->resolved->kind != Z_TYPE_FUNCTION) {
            char *name = stoken(node->foreignDecl.name);
            ZMIRValueRef global = ZMIRGetNamedGlobal(ctx->mod, name);
            if (!global) {
                global = ZMIRAddGlobal(ctx->mod,
                    genType(ctx, node->resolved), name
                );
            }
            putZMIRValueRef(ctx, name, global);
            return global;
        }
        return NULL;
    }
    case NODE_FUNC: {
        ZAnnotation *export = annArg(query(node->funcDef.annotations, "export"), 0);
        if (export) {
            node->funcDef.mangled = stoken(export->tok);
        } else if (!node->funcDef.mangled) {
            node->funcDef.mangled = mangler((char *[]) {
                node->funcDef.name->filename,
                node->funcDef.name->str,
                NULL
            });
        }

        ZMIRValueRef existing = ZMIRGetNamedFunction(ctx->mod, node->funcDef.mangled);
        if (existing) {
            putZMIRValueRef(ctx, node->funcDef.mangled, existing);
            return existing;
        }

        ZMIRTypeRef ret      = genType(ctx, node->resolved->func.ret);
        ZMIRTypeRef *args    = NULL;

        if (node->funcDef.receiver) {
            vecpush(args, genType(ctx, node->funcDef.receiver->resolved));
        }
        /* Must drop the same parameters as genFunc, otherwise the declaration
         * and the definition of the same function disagree on the signature
         * and every call built from this type has the wrong argument count. */
        for (usize i = 0; i < veclen(node->funcDef.capabilities); i++) {
            ZType *capType = node->funcDef.capabilities[i]->field.type;
            if (!isRuntimeParam(ctx->state, capType)) continue;
            vecpush(args, genType(ctx, capType));
        }
        for (usize i = 0; i < veclen(node->funcDef.args); i++) {
            ZType *at = node->funcDef.args[i]->field.type;
            if (!isRuntimeParam(ctx->state, at)) continue;
            ZMIRTypeRef arg = genType(ctx, at);
            if (at->kind == Z_TYPE_FUNCTION)
                arg = ZMIRPointerType(arg, 0);
            vecpush(args, arg);
        }

        ZMIRTypeRef funcType = ZMIRFunctionType(ret, args, veclen(args), false);
        ZMIRValueRef decl    = ZMIRAddFunction(ctx->mod, node->funcDef.mangled, funcType);
        putZMIRValueRef(ctx, node->funcDef.mangled, decl);

        return decl;
        break;
    }
    default: return NULL;
    }
    return NULL;
}

static void *compileModule(void *arg) {
    ZCodegen *ctx = (ZCodegen *)arg;
    useAllocator(ctx->module->allocator);
    initNativeTypes(ctx);
    compile(ctx, ctx->module->module);
    return NULL;
}

ZMIRModuleRef *zgenModules(ZState *state) {
    usize len           = veclen(state->modules);

    ZCodegen **gens     = NULL;
    pthread_t *threads  = znalloc(state->allocator, pthread_t, len);

    for (usize i = 0; i < len; i++) {
        ZCodegen *gen = makecodegen(state, state->modules[i]);
        pthread_create(threads + i, NULL, compileModule, gen);
        vecpush(gens, gen);
    }

    for (usize i = 0; i < len; i++) pthread_join(threads[i], NULL);

    ZMIRModuleRef *modules = NULL;
    for (usize i = 0; i < len; i++) vecpush(modules, gens[i]->mod);

    return modules;
}
