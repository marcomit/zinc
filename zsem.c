/* This file is the Semantic analyzer.
 *
 * Now what is a semantic analyzer?
 * In this part of the compiler/interpreter
 * we have already built the AST (Abstract Syntax Tree) and
 * we want to make sure that all the types are correctly.
 * for example this expression 1230 + "hello"
 * is not valid because i cannot add a number to a string
 * but the parser parse it correctly. so we need to check every single expression/statement
 * that all types are correctly.
 * In this phase we also care about the scope of functions/variables.
 *
 * How do we handle the scope?
 *
 * We have a global scope for the entire project,
 * then a child scope for the current file and then a child for blocks like functions, loops etc..
 * */
#include "zinc.h"
#include "zvec.h"

static void analyzeStmt(ZSemantic *, ZNode *);
static void analyzeBlock(ZSemantic *, ZNode *, bool);
static ZType *resolveTypeRef(ZSemantic *, ZType *);

/* ================== Scope / Symbol helpers ================== */

static ZType *none      = NULL;
static ZType *zvoid     = NULL;
static ZType *u1Type    = NULL;
static ZType *ztrue     = NULL;
static ZType *zfalse    = NULL;

static ZScope *makescope(ZScope *parent, ZNode *node) {
    ZScope *self        = zalloc(ZScope);
    self->depth         = parent ? parent->depth + 1 : 0;
    self->parent        = parent;
    self->node          = node;
    self->symbols       = NULL;
    self->seen          = NULL;
    return self;
}

static ZSymbol *makesymbol(ZSymType kind) {
    ZSymbol *self       = zalloc(ZSymbol);
    self->kind          = kind;
    self->useCount      = 0;
    return self;
}

static ZSymTable *makesymtable(void) {
    ZSymTable *self     = zalloc(ZSymTable);
    self->global        = makescope(NULL, NULL);
    self->current       = self->global;
    self->module        = NULL;
    self->funcs         = NULL;
    return self;
}

static ZSemantic *makesemantic(ZState *state, ZNode *root) {
    ZSemantic *self         = zalloc(ZSemantic);
    self->root              = root;
    self->currentFuncRet    = NULL;
    self->currentFunc       = NULL;
    self->loopDepth         = 0;
    self->state             = state;
    self->table             = makesymtable();
    self->scopes            = NULL;
    self->seen              = NULL;
    
    return self;
}


static void putSymbol(ZSemantic *semantic, ZSymbol *symbol) {
    ZScope *scope = semantic->table->current;

    if (symbol->isPublic) scope = semantic->table->global;

    if (!hashset_insert(&scope->seen, symbol->name->str)) {
        /* Duplicate pub foreign declarations are valid: multiple modules
           may re-export the same C extern (like a shared header). Skip
           silently instead of raising an error. */
        if (symbol->isPublic && symbol->kind == Z_SYM_FUNC &&
                symbol->node && symbol->node->type == NODE_FOREIGN) {
            return;
        }
        error(semantic->state, symbol->name,
                "'%s' already defined in the same scope",
                symbol->name->str);
    } else {
        vecpush(scope->symbols, symbol);
    }

}

static void putRawSymbol(ZSemantic *semantic,
                        ZSymType kind,
                        ZToken *name,
                        ZType *type,
                        ZNode *node,
                        bool isGlobal) {
    ZSymbol *symbol = makesymbol(kind);

    symbol->name        = name;
    symbol->type        = type;
    symbol->node        = node;
    symbol->isPublic    = isGlobal;

    putSymbol(semantic, symbol);
}

static ZFuncTable *makefunctable(ZType *base) {
    ZFuncTable *func        = zalloc(ZFuncTable);
    func->base              = base;
    func->funcDef           = NULL;
    func->seenReceiverFuncs = NULL;
    func->staticFuncDef     = NULL;
    func->seenStaticFuncs   = NULL;
    return func;
}

static ZFuncTable *addfunctable(ZSemantic *semantic, ZType *base) {
    ZFuncTable *table = makefunctable(base);
    vecpush(semantic->table->funcs, table);
    return table;
}

static void addStaticFunc(ZSemantic *semantic, ZNode *func) {
    ZType *base = func->funcDef.base;

    ZFuncTable *cur = NULL;
    for (usize i = 0; i < veclen(semantic->table->funcs); i++) {
        ZFuncTable *table = semantic->table->funcs[i];

        if (typesEqual(table->base, base)) {
            cur = table;
            break;
        }
    }

    if (!cur) {
        cur = addfunctable(semantic, base);
    }
    char *name = func->funcDef.name->str;
    if (!hashset_insert(&cur->seenStaticFuncs, name)) {
        error(semantic->state, func->tok,
                "Duplicate static function '%s'", name);
        return;
    }


    vecpush(cur->staticFuncDef, func);
}

static void putReceiverFunc(ZSemantic *semantic, ZNode *node) {
    if (!node->funcDef.receiver) {
        error(semantic->state, node->tok, "receiver must be setted");
        return;
    } else if (node->funcDef.base) {
        error(semantic->state, node->tok,
                "receiver functions cannot have a base");
        return;
    }

    ZNode *receiver         = node->funcDef.receiver;
    ZFuncTable **funcs      = semantic->table->funcs;
    ZFuncTable *table       = NULL;
    for (usize i = 0; i < veclen(funcs) && !table; i++) {
        if (typesEqual(funcs[i]->base, receiver->field.type)) {
            table = funcs[i];
        }
    }

    if (!table) {
        table = makefunctable(receiver->field.type);
        vecpush(semantic->table->funcs, table);
    }

    vecpush(table->funcDef, node);
}

static void putStaticFunc(ZSemantic *semantic, ZNode *node) {
    ZType *baseType = node->funcDef.base;
    
    if (baseType->kind != Z_TYPE_PRIMITIVE) {
        error(semantic->state, node->tok,
                "Static function must be attached to a primitive type");
    }
    ZToken *base = baseType->primitive.token;
    if (!base) {
        error(semantic->state,
                node->tok,
                "Invalid 'putStaticFunc' call, base is not setted");
        return;
    } else if (node->funcDef.receiver) {
        error(semantic->state,
                node->tok,
                "Invalid 'putStaticFunc' call, receiver cannot be setted");
    }

    addStaticFunc(semantic, node);
}

static void putFunc(ZSemantic *semantic, ZNode *node) {
    if (node->funcDef.receiver) {
        if (node->funcDef.base) {
            error(semantic->state, node->tok,
                    "A static function cannot accept any receiver");
        }

        putReceiverFunc(semantic, node);
    } else if (node->funcDef.base) {
        putStaticFunc(semantic, node);
    } else {
        putRawSymbol(semantic,
                Z_SYM_FUNC,
                node->funcDef.name,
                node->resolved,
                node,
                node->funcDef.pub);
    }
}

static void putVar(ZSemantic *semantic, ZNode *node, bool isGlobal) {
    putRawSymbol(semantic,
            Z_SYM_VAR,
            node->varDecl.ident->identNode.tok,
            node->resolved,
            node,
            isGlobal);
}

static void putStruct(ZSemantic *semantic, ZNode *node) {
    ZType *type             = maketype(Z_TYPE_STRUCT);
    type->strct.name        = node->structDef.ident;
    type->strct.fields      = node->structDef.fields;
    type->strct.generics    = NULL;
    node->resolved          = type;

    putRawSymbol(semantic,
            Z_SYM_STRUCT,
            node->structDef.ident,
            type,
            node,
            node->structDef.pub);
}

static void putTypedef(ZSemantic *semantic, ZNode *node) {
    putRawSymbol(semantic,
            Z_SYM_TYPEDEF,
            node->typeDef.alias,
            node->typeDef.type,
            node,
            node->typeDef.pub);
}

static void registerModule(ZSemantic *semantic, ZNode *module) {
    ZScope *scope = NULL;
    for (usize i = 0; i < veclen(semantic->scopes); i++) {
        if (semantic->scopes[i]->module == module) {
            scope = semantic->scopes[i]->scope;
            goto setScope;
        }
    }

    ZScopeTable *table = zalloc(ZScopeTable);
    table->module = module;
    table->scope = makescope(semantic->table->global, module);

    vecpush(semantic->scopes, table);
    scope = table->scope;

setScope:
    semantic->table->module = semantic->table->current;
    semantic->table->current = scope;
}

static void warnUnused(ZSemantic *semantic, ZSymbol *symbol) {
    switch (symbol->kind) {
    case Z_SYM_FUNC:
        if (semantic->state->unusedFunc) break;
        warning(semantic->state,
                symbol->name,
                "Unused function '%s'",
                symbol->name->str);
        break;
    case Z_SYM_STRUCT:
        if (semantic->state->unusedStruct) break;
        warning(semantic->state,
                symbol->name,
                "Unused struct '%s'",
                symbol->name->str);
        break;
    case Z_SYM_VAR:
        if (semantic->state->unusedVar) break;
        warning(semantic->state,
                symbol->name,
                "Unused variable '%s'",
                symbol->name->str);
        break;
    default:
        warning(semantic->state, symbol->name, "Unused a generic symbol");
        break;
    }
}

static void checkUnusedSymbols(ZSemantic *semantic) {
    ZScope *scope = semantic->table->current;
    for (usize i = 0; i < veclen(scope->symbols); i++) {
        let symbol = scope->symbols[i];
        if (symbol->useCount == 0) {
            warnUnused(semantic, symbol);
        }
    }
}

static void endModule(ZSemantic *semantic) {
    if (!semantic || !semantic->table || !semantic->table->module) return;
    checkUnusedSymbols(semantic);
    semantic->table->current = semantic->table->module;
}

static void beginScope(ZSemantic *semantic, ZNode *curr) {
    ZScope *scope               = makescope(semantic->table->current, curr);
    semantic->table->current    = scope;
}

static void endScope(ZSemantic *semantic) {
    if (!semantic->table->current || !semantic->table->current->parent) {
        error(semantic->state, NULL, "Called endScope at the highest level");
        return;
    }
    checkUnusedSymbols(semantic);
    semantic->table->current = semantic->table->current->parent;
}

/* ================== Type arithmetic ================== */

static u8 typeRank(ZTokenType t) {
    switch (t) {
    case TOK_CHAR:              return 0;
    case TOK_I8:  case TOK_U8:  return 1;
    case TOK_I16: case TOK_U16: return 2;
    case TOK_I32: case TOK_U32: return 3;
    case TOK_I64: case TOK_U64: return 4;
    case TOK_F32:               return 5;
    case TOK_F64:               return 6;
    default:                    return 0;
    }
}

static inline bool isUnsigned(ZTokenType t) { return (bool)(t & TOK_UNSIGNED); }
static inline bool isSigned  (ZTokenType t) { return (bool)(t & TOK_SIGNED);   }
static inline bool isFloat   (ZTokenType t) { return (bool)(t & TOK_FLOAT);    }
static inline bool isInteger (ZTokenType t) { return isSigned(t) || isUnsigned(t); }

static ZTokenType toSigned(u8 rank) {
    switch (rank) {
    case 1: return TOK_I8;
    case 2: return TOK_I16;
    case 3: return TOK_I32;
    case 4: return TOK_I64;
    default: return TOK_I32;
    }
}

static bool isComparable(ZSemantic *semantic, ZType *type) {
    type = resolveTypeRef(semantic, type);
    if (!type) return false;

    if (type->kind == Z_TYPE_FUNCTION       ||
            type->kind == Z_TYPE_ARRAY      ||
            type->kind == Z_TYPE_GENERIC    ||
            type->kind == Z_TYPE_STRUCT     ||
            type->kind == Z_TYPE_TUPLE) {
        return false;
    }

    return true;
}

/* An implementation note:
 * if a and b are pointers the returned type is a (used for implicit casting).
 * if a or b is a float the return type is always a float.
 * if a and b are both signed or unsigned return the type with the highest rank.
 * if they are unsigned vs signed and the unsigned is u64 can't promote to i128
 * so in this case the compiler shows a warning (explicit casting requested).
 *
 * Note: this function does not work if a primitive type is aliased.
 * */
ZType *typesCompatible(ZState *state, ZType *a, ZType *b) {
    if (!a || !b) return NULL;
    
    if (a->kind == Z_TYPE_POINTER && b->kind == Z_TYPE_NONE) {
        return a;
    } else if (b->kind == Z_TYPE_POINTER && a->kind == Z_TYPE_NONE) {
        return b;
    } else if (a->kind == Z_TYPE_POINTER && b->kind == Z_TYPE_POINTER) {
        return a;
    }

    if (typesEqual(a, b)) return b;

    if (a->kind != Z_TYPE_PRIMITIVE || b->kind != Z_TYPE_PRIMITIVE)
        return NULL;

    ZTokenType ta = a->primitive.token->type;
    ZTokenType tb = b->primitive.token->type;

    if (ta == TOK_VOID || tb == TOK_VOID) return NULL;

    u8 ra = typeRank(ta);
    u8 rb = typeRank(tb);

    if (isFloat(ta) || isFloat(tb)) {
        u8 minRank = min(ra, rb);
        if (minRank <= 2)
            return ra > rb ? a : b;
        ZType *promoted = maketype(Z_TYPE_PRIMITIVE);
        promoted->primitive.token = maketoken(TOK_F64, NULL);
        return promoted;
    }

    if ((isSigned(ta) && isSigned(tb)) ||
        (isUnsigned(ta) && isUnsigned(tb)))
        return ra > rb ? a : b;

    /* signed vs unsigned */
    u8    signedRank   = isSigned(ta) ? ra : rb;
    u8    unsignedRank = isSigned(ta) ? rb : ra;
    ZType *signedType  = isSigned(ta) ? a  : b;

    if (signedRank > unsignedRank) return signedType;

    if (signedRank == 4) {
        warning(state, signedType->primitive.token,
                "Cannot promote an i64, try with an explicit cast");
    }

    ZType *promoted = maketype(Z_TYPE_PRIMITIVE);
    promoted->primitive.token = maketoken(toSigned(signedRank + 1), NULL);
    return promoted;
}

bool isVoid(ZType *t) {
    if (!t) return true;
    if (t->kind != Z_TYPE_PRIMITIVE) return false;
    return t->primitive.token->type == TOK_VOID;
}

bool typesPrimitive(ZType *t) {
    if (!t) return true;

    if (t->kind != Z_TYPE_PRIMITIVE) return false;

    switch (t->primitive.token->type) {
    case TOK_VOID:
    case TOK_CHAR:
    case TOK_I8:
    case TOK_U8:
    case TOK_I16:
    case TOK_U16:
    case TOK_I32:
    case TOK_U32:
    case TOK_I64:
    case TOK_U64:
    case TOK_F32:
    case TOK_F64: return true;
    default: return false;
    }

    return false;
}

/* Note: this function works only for non-aliased types.
 * Aliases are resolved through the semantic table.
 *
 * */
bool typesEqual(ZType *a, ZType *b) {
    if (!a || !b) return false;

    if (a->kind != b->kind) return false;

    switch (a->kind) {
    case Z_TYPE_PRIMITIVE:
        return tokeneq(a->primitive.token, b->primitive.token);
    case Z_TYPE_POINTER:
        return typesEqual(a->base, b->base);
    case Z_TYPE_ARRAY:
        if (a->array.size == 0 && b->array.size > 0) {
            a->array.size = b->array.size;
        } else if (b->array.size == 0 && a->array.size > 0) {
            b->array.size = a->array.size;
        } else if (a->array.size != b->array.size) {
            return false;
        }
        return typesEqual(a->array.base, b->array.base);
    case Z_TYPE_STRUCT:
    case Z_TYPE_FUNCTION:
        return a == b;
    case Z_TYPE_TUPLE: {
        if (veclen(a->tuple) != veclen(b->tuple)) return false;
        for (usize i = 0; i < veclen(a->tuple); i++)
            if (!typesEqual(a->tuple[i], b->tuple[i])) return false;
        return true;
    }
    case Z_TYPE_GENERIC: {
        if (strcmp(a->generic.name->str, b->generic.name->str) != 0) return false;
        if (veclen(a->generic.args) != veclen(b->generic.args)) return false;
        for (usize i = 0; i < veclen(a->generic.args); i++)
            if (!typesEqual(a->generic.args[i], b->generic.args[i])) return false;
        return true;
    }
    default:
        return false;
    }
}

ZNode *implicitCast(ZNode *node, ZType *type) {
    if (node->resolved && typesEqual(node->resolved, type)) {
        return node;
    }

    ZNode *cast = makenode(NODE_CAST);
    cast->castExpr.expr = node;
    cast->castExpr.toType = type;
    return cast;
}

/* ================== Symbol lookup ================== */
ZSymbol *resolve(ZSemantic *semantic, ZToken *ident) {
    ZScope *curr = semantic->table->current;
    while (curr) {
        for (usize i = 0; i < veclen(curr->symbols); i++) {
            if (tokeneq(curr->symbols[i]->name, ident)) {
                curr->symbols[i]->useCount++;
                return curr->symbols[i];
            }
        }
        curr = curr->parent;
    }
    return NULL;
}

/* ================== Type resolution ================== */

static ZType *derefType(ZType *t) {
    while (t && t->kind == Z_TYPE_POINTER) t = t->base;
    return t;
}

static ZType *resolveLiteralType(ZNode *curr) {
    // None guard
    if (curr->literalTok->type == TOK_NONE) {
        return none;
    }

    ZType *t = maketype(Z_TYPE_PRIMITIVE);
    switch (curr->literalTok->type) {
    case TOK_INT_LIT: {
        t->primitive.token = maketoken(TOK_I32, NULL);
        return t;
    }
    case TOK_FLOAT_LIT: {
        t->primitive.token = maketoken(TOK_F64, NULL);
        return t;
    }
    case TOK_FALSE:
    case TOK_TRUE: {
        t->primitive.token = maketoken(TOK_BOOL, NULL);
        return t;
    }
    case TOK_STR_LIT: {
        /* String literals are *char */
        ZType *base = maketype(Z_TYPE_PRIMITIVE);
        base->primitive.token = maketoken(TOK_CHAR, NULL);
        t->kind = Z_TYPE_POINTER;
        t->base   = base;
        return t;
    }
    default: {
        t->primitive.token = maketoken(TOK_VOID, NULL);
        return t;
    }
    }
}


static ZType *_resolveTypeRef(ZSemantic *semantic, ZType *type, ZType **seen) {
    if (!type) return NULL;

    // for (usize i = 0; i < veclen(seen); i++) {
    //     if (typesEqual(type, seen[i])) {
    //         printf("Cycle detected\n");
    //         return NULL;
    //     }
    // }

    // vecpush(seen, type);

    switch (type->kind) {
    case Z_TYPE_PRIMITIVE: {
        if (type->primitive.token->type != TOK_IDENT) return type;
        ZSymbol *sym = resolve(semantic, type->primitive.token);
        if (!sym) {
            error(semantic->state, type->primitive.token,
                  "Unknown type '%s'", type->primitive.token->str);
            return NULL;
        }
        return sym->type;
    }
    case Z_TYPE_POINTER:
        type->base = _resolveTypeRef(semantic, type->base, seen);
        return type;
    case Z_TYPE_ARRAY:
        type->array.base = _resolveTypeRef(semantic, type->array.base, seen);
        return type;
    case Z_TYPE_FUNCTION:
        type->func.ret = _resolveTypeRef(semantic, type->func.ret, seen);
        for (usize i = 0; i < veclen(type->func.args); i++)
            type->func.args[i] = _resolveTypeRef(semantic, type->func.args[i], seen);
        return type;
    case Z_TYPE_TUPLE:
        for (usize i = 0; i < veclen(type->tuple); i++)
            type->tuple[i] = _resolveTypeRef(semantic, type->tuple[i], seen);
        return type;
    default: return type;
    }
}

/*
 * Resolve a ZType that may contain named references (user-defined types).
 *
 * When the parser sees `MyStruct x`, the type is stored as a Z_TYPE_PRIMITIVE
 * with primitive.token->type == TOK_IDENT and primitive.token->str == "MyStruct".
 * This function looks that name up in the symbol table and returns the actual
 * ZType registered by putStruct() / a typedef entry.
 *
 * For compound types (pointer, array, function, tuple) it recurses into their
 * sub-types so that e.g. `*MyStruct` gets fully resolved.
 *
 * NOTE: this function doesn't handle cycles.
 * So a struct that has a pointer to itself
 * (like linked-lists, trees, graphs ...) cause an in infinite loop.
 * To handle it properly the function have to early return
 * if the current type is already visited.
 */
static ZType *resolveTypeRef(ZSemantic *semantic, ZType *type) {
    if (!type) return NULL;
    ZType **seen = NULL;
    return _resolveTypeRef(semantic, type, seen);
}

static ZType *resolveMemberAccess(ZSemantic *, ZNode *);
static ZType *resolveArrSubscript(ZSemantic *, ZNode *);

static ZType *resolveFuncCall(ZSemantic *semantic, ZNode *curr) {
    ZNode *callee = curr->call.callee;
    ZNode **args = curr->call.args;

    for (usize i = 0; i < veclen(args); i++) {
        args[i]->resolved = resolveType(semantic, args[i]);
    }

    ZType *result = NULL;
    ZType **expectedArgs = NULL;
    if (callee->type == NODE_IDENTIFIER) {
        ZSymbol *sym = resolve(semantic, callee->identNode.tok);
        if (!sym) {
            error(semantic->state, callee->identNode.tok,
                  "Undefined function '%s'", callee->identNode.tok->str);
            return NULL;
        }
        if (sym->kind != Z_SYM_FUNC) {
            error(semantic->state, callee->identNode.tok,
                  "'%s' is not callable", callee->identNode.tok->str);
            return NULL;
        }
        if (sym->node->type == NODE_FUNC) {
            callee->identNode.mangled = sym->node->funcDef.mangled;
        }
        /* sym->type is the raw parsed return type — resolve it so that named
     * types (e.g. "Vec2" → Z_TYPE_STRUCT) are expanded before the
     * result is used downstream (e.g. for member access on return value). */

        for (usize i = 0; i < veclen(sym->type->func.args); i++) {
            sym->type->func.args[i] = resolveTypeRef(
                semantic, sym->type->func.args[i]
            );
        }
        expectedArgs = sym->type->func.args;

        result = resolveTypeRef(semantic, sym->node->funcDef.ret);
        sym->node->funcDef.ret = result;

        callee->resolved = result;
    } else if (callee->type == NODE_STATIC_ACCESS) {
        ZToken *base = callee->staticAccess.base;
        ZToken *prop = callee->staticAccess.prop;
        ZSymbol *baseSym = resolve(semantic, base);
        if (!baseSym) {
            error(semantic->state, base, "Base not found");
        } else if ( baseSym->kind != Z_SYM_STRUCT &&
                    baseSym->kind != Z_SYM_TYPEDEF) {
            error(semantic->state, base, "Base should refer to a type");
        }

        ZNode **staticFuncs = NULL;
        for (usize i = 0; i < veclen(semantic->table->funcs); i++) {
            ZFuncTable *table = semantic->table->funcs[i];
            if (table->base->kind != Z_TYPE_PRIMITIVE) {
                continue;
            }
            if (!tokeneq(table->base->primitive.token, base)) continue;

            if (!hashset_has(table->seenStaticFuncs, prop->str)) {
                error(semantic->state, prop,
                        "'%s' method does not exist", prop->str);
            }
            staticFuncs = table->staticFuncDef;
        }
        if (!staticFuncs) {
            error(semantic->state, prop,
                    "Static method '%s' not found", prop->str);
        }

        for (usize i = 0; i < veclen(staticFuncs) && !result; i++) {
            ZNode *func = staticFuncs[i];
            if (tokeneq(func->funcDef.base->primitive.token, base) &&
                tokeneq(func->funcDef.name, prop)) {
                result = func->funcDef.ret;
                expectedArgs = func->resolved->func.args;
            }
        }
    } else if (callee->type == NODE_MEMBER) {
        ZType *obj = resolveType(semantic, callee->memberAccess.object);

        ZToken *prop = callee->memberAccess.field;

        ZFuncTable **table = semantic->table->funcs;

        ZFuncTable *funcs = NULL;
        for (usize i = 0; i < veclen(table) && !funcs; i++) {
            ZType *base = resolveTypeRef(semantic, table[i]->base);
            if (typesEqual(base, obj)) funcs = table[i];
        }

        if (!funcs) {
            error(semantic->state, curr->tok, "Receiver function not registered");
            return NULL;
        }

        for (usize i = 0; i < veclen(funcs->funcDef); i++) {
            ZNode *f = funcs->funcDef[i];
            if (tokeneq(f->funcDef.name, prop)) {
                callee->memberAccess.mangled = f->funcDef.mangled;
                expectedArgs = f->resolved->func.args;
                result = resolveTypeRef(semantic, f->funcDef.ret);
                break;
            }
        }
    } else {
        /* Expression call: resolve callee type and extract return type. */
        ZType *calleeType = resolveType(semantic, callee);
        if (calleeType && calleeType->kind == Z_TYPE_FUNCTION) {
            result = resolveTypeRef(semantic, calleeType->func.ret);
            calleeType->func.ret = result;
        }

        for (usize i = 0; i < veclen(calleeType->func.args); i++) {
            calleeType->func.args[i] = resolveTypeRef(
                semantic, calleeType->func.args[i]
            );
        }
        expectedArgs = calleeType->func.args;
    }

    if (!result) return NULL;

    if (veclen(expectedArgs) != veclen(args)) {
        error(semantic->state, curr->tok,
                "Expected %zu arguments, got %zu",
                veclen(expectedArgs), veclen(args));
        return NULL;
    }

    for (usize i = 0; i < veclen(args); i++) {
        /* if they are equal they don't need implicit casting. */
        if (typesEqual(args[i]->resolved, expectedArgs[i])) continue;

        ZType *promoted = typesCompatible(
            semantic->state, args[i]->resolved, expectedArgs[i]);

        if (!promoted) {
            error(semantic->state, args[i]->tok,
                "Expected %s, got %s",
                stype(args[i]->resolved),
                stype(expectedArgs[i])
            );
        }
        curr->call.args[i] = implicitCast(args[i], expectedArgs[i]);
    }

    return result;
}

static bool isLvalue(ZNode *node) {
    if (!node) return false;

    switch (node->type) {
    case NODE_IDENTIFIER:   return true;
    case NODE_SUBSCRIPT:    return true;
    case NODE_MEMBER:       return true;
    case NODE_UNARY:        return node->unary.operat->type == TOK_STAR;
    default: return false;
    }
}

static ZType *resolveStructLit(ZSemantic *semantic, ZNode *curr) {
    ZSymbol *structSym = resolve(semantic, curr->structlit.ident);

    if (!structSym) {
        error(semantic->state, curr->tok,
                "struct '%s' not found", curr->tok->str);
        return NULL;
    }
    if (structSym->kind != Z_SYM_STRUCT) {
        error(semantic->state, structSym->name,
                    "'%s' is not a struct", stoken(structSym->name));
        return NULL;
    }

    ZNode **structFields = structSym->type->strct.fields;
    usize structLen = veclen(structFields);

    if (veclen(curr->structlit.fields) < structLen) {
        warning(semantic->state, curr->tok, "Some fields not initialized");
    }

    for (usize i = 0; i < veclen(curr->structlit.fields); i++) {
        ZNode *field = curr->structlit.fields[i];
        ZType *type = resolveType(semantic, field->varDecl.rvalue);
        ZType *expectedType;
        ZType *promoted;

        field->resolved = type;
        for (usize j = 0; j < structLen; j++) {
            if (!tokeneq(structFields[j]->field.identifier, field->tok)) continue;
            expectedType = resolveTypeRef(semantic, structFields[j]->field.type);
            structFields[j]->field.type = expectedType;
            promoted = typesCompatible(semantic->state, expectedType, type);
            if (!promoted) {
                error(semantic->state,
                    field->varDecl.ident->identNode.tok,
                    "Expected %s, got %s",
                    stype(expectedType),
                    stype(type)
                );
            } else {
            }

            break;
        }
    }
    ZSymbol *resolved = resolve(semantic, curr->structlit.ident);

    if (!resolved) {
        error(semantic->state, curr->structlit.ident,
                "Unknown struct literal");
    } else {
        if (resolved->kind != Z_SYM_STRUCT) {
            error(semantic->state, curr->structlit.ident,
                    "This is not a struct literal");
        }
        return resolved->type;
    }
    return NULL;
}

static ZType* resolveIdentifier(ZSemantic *semantic, ZNode *node) {
    ZToken *tok = node->identNode.tok;
    ZSymbol *sym = resolve(semantic, tok);
    if (!sym) {
        error(semantic->state, tok,
              "Undefined identifier '%s'", tok->str);
        return NULL;
    }
    if (sym->node->type == NODE_FUNC) {
        node->identNode.mangled = sym->node->funcDef.mangled;
    }
    return sym->type;
}

static ZType *resolveBinary(ZSemantic *semantic, ZNode *curr) {
    ZTokenType op       = curr->binary.op->type;
    ZType     *left     = resolveType(semantic, curr->binary.left);
    ZType     *right    = resolveType(semantic, curr->binary.right);


    /* Auto promotion rules should be handled by typesCompatible. */
    ZType *promoted     = typesCompatible(semantic->state, left, right);

    if (!promoted) {
        error(semantic->state,
            curr->binary.op,
            "Incompatible type '%s' with '%s'",
            stype(left),
            stype(right)
        );
    }

    if (op == TOK_EQ) {
        /* Assignment yields the type of the left-hand side. */
        if (!isLvalue(curr->binary.left)) {
            error(semantic->state, curr->binary.left->tok,
                    "is not a valid lvalue");
        }
        return left;
    }

    curr->binary.left = implicitCast(curr->binary.left, promoted);
    curr->binary.right = implicitCast(curr->binary.right, promoted);

    /* Comparison / logical operators always produce a bool. */
    if (
        op == TOK_EQEQ || op == TOK_NOTEQ ||
        op == TOK_LT   || op == TOK_GT    ||
        op == TOK_LTE  || op == TOK_GTE   ||
        op == TOK_AND  || op == TOK_OR    ||
        op == TOK_SAND || op == TOK_SOR) {
        ZType *boolType = maketype(Z_TYPE_PRIMITIVE);
        boolType->primitive.token = maketoken(TOK_BOOL, NULL);
        return boolType;
    }

    return promoted;
}

static ZType *resolveArrayLiteral(ZSemantic *semantic, ZNode *curr) {
    ZType *arrType = NULL;
    usize len = veclen(curr->arraylit);

    for (usize i = 0; i < len; i++) {
        ZNode *field = curr->arraylit[i];
        ZType *fieldType = resolveType(semantic, field);

        if (!arrType) {
            arrType = fieldType;
        } else {
            arrType = typesCompatible(semantic->state, arrType, fieldType);

            if (!arrType) {
                ZToken *tok = NULL;
                if (fieldType) tok = fieldType->tok;
                error(semantic->state, tok,
                             "Array literals should have the same type");
            }
        }
    }

    ZType *result       = maketype(Z_TYPE_ARRAY);
    result->array.base  = arrType;
    result->array.size  = len;


    return result;
}

static ZType *resolveTupleLiteral(ZSemantic *semantic, ZNode *node) {
    ZType **types = NULL;
    ZType *fieldType = NULL;

    ZNode **fields = node->tuplelit;
    for (usize i = 0; i < veclen(fields); i++) {
        fieldType = resolveType(semantic, fields[i]);
        if (!fieldType) {
            error(semantic->state, fields[i]->tok,
                    "Unresolved type of tuple");
        } else {
            vecpush(types, fieldType);
        }
    }

    ZType *result = maketype(Z_TYPE_TUPLE);
    result->tuple = types;
    return result;
}

static ZType *resolveArrayInit(ZSemantic *semantic, ZNode *node) {
    node->arrayinit = resolveTypeRef(semantic, node->arrayinit);
    node->resolved = node->arrayinit;
    return node->arrayinit;
}

/*
 * Resolve the type of any expression node and cache the result in node->resolved.
 * Returns the resolved ZType* or NULL on error.
 */
ZType *resolveType(ZSemantic *semantic, ZNode *curr) {
    if (!curr)           return NULL;
    if (curr->resolved)  return curr->resolved;

    ZType *result = NULL;

    switch (curr->type) {
    case NODE_CALL:         result = resolveFuncCall    (semantic, curr);   break;
    case NODE_LITERAL:      result = resolveLiteralType (curr);             break;
    case NODE_MEMBER:       result = resolveMemberAccess(semantic, curr);   break;
    case NODE_SUBSCRIPT:    result = resolveArrSubscript(semantic, curr);   break;
    case NODE_STRUCT_LIT:   result = resolveStructLit   (semantic, curr);   break;
    case NODE_IDENTIFIER:   result = resolveIdentifier  (semantic, curr);   break;
    case NODE_ARRAY_LIT:    result = resolveArrayLiteral(semantic, curr);   break;
    case NODE_TUPLE_LIT:    result = resolveTupleLiteral(semantic, curr);   break;
    case NODE_BINARY:       result = resolveBinary      (semantic, curr);   break;
    case NODE_ARRAY_INIT:   result = resolveArrayInit   (semantic, curr);   break;

    case NODE_UNARY: {
        ZType     *operand = resolveType(semantic, curr->unary.operand);
        ZTokenType op      = curr->unary.operat->type;

        if (!operand) {
            error(semantic->state, curr->tok, "Unresolved type");
            return NULL;
        }

        switch (op) {
        case TOK_REF: {/* &expr => *T */
            ZType *ptr  = maketype(Z_TYPE_POINTER);
            ptr->base   = operand;
            result      = ptr;
            break;
        }

        case TOK_STAR:
            if (operand->kind != Z_TYPE_POINTER) {
                error(semantic->state, curr->unary.operat,
                    "Cannot dereference a non-pointer type"
                );
            }
            result = operand->base;
            break;

        case TOK_NOT:
        case TOK_SNOT:
            curr->unary.operand = implicitCast(curr->unary.operand, u1Type);
            result = u1Type;
        default: result = operand; break;
        }
        break;
    }

    case NODE_VAR_DECL:
        /* Used when a var-decl appears as a sub-expression (unusual but safe). */
        if (curr->resolved) {
            result = resolveTypeRef(semantic, curr->resolved);
        } else if (curr->varDecl.rvalue) {
            result = resolveType(semantic, curr->varDecl.rvalue);
        }
        break;

    case NODE_CAST: {
        /* Resolve the inner expression type (for side-effects / validation). */
        ZType *expr = resolveType(semantic, curr->castExpr.expr);
        result = resolveTypeRef(semantic, curr->castExpr.toType);

        if (expr && expr->kind == Z_TYPE_ARRAY &&
            result->kind == Z_TYPE_ARRAY) {
            result->array.size = expr->array.size;
        }

        curr->castExpr.toType = result;
        break;
    }

    case NODE_SIZEOF: {
        /* sizeof yields u64. */
        curr->sizeofExpr.type = resolveTypeRef(semantic, curr->sizeofExpr.type);
        result = maketype(Z_TYPE_PRIMITIVE);
        result->primitive.token = maketoken(TOK_U64, "u64");
        break;
    }

    case NODE_BREAK:
        if (semantic->loopDepth == 0) {
            error(semantic->state, curr->tok, "break must be inside a loop");
        }
        break;
    case NODE_CONTINUE:
        if (semantic->loopDepth == 0) {
            error(semantic->state, curr->tok, "continue must be inside a loop");
        }
        break;

    default:
        warning(semantic->state, curr->tok,
                "Trying to resolve the type of node's type %d", curr->type);
        break;
    }

    curr->resolved = result;
    return result;
}

static ZType *resolveMemberAccess(ZSemantic *semantic, ZNode *curr) {
    ZType *objType = resolveType(semantic, curr->memberAccess.object);
    if (!objType) {
        error(semantic->state, curr->tok,
              "Cannot resolve object type in member access");
        return NULL;
    }

    ZType *base = derefType(objType);
    ZToken *field = curr->memberAccess.field;

    if (!base) {
        error(semantic->state, curr->tok,
              "Base type not found");
        return NULL;
    }
    
    if (base->kind == Z_TYPE_STRUCT) {
        ZNode **fields = base->strct.fields;
        for (usize i = 0; i < veclen(fields); i++) {
            if (tokeneq(fields[i]->field.identifier, field))
                return resolveTypeRef(semantic, fields[i]->field.type);
        }

        error(semantic->state, field,
              "Member '%s' not found in struct", field->str);
        return NULL;
    } else if (base->kind == Z_TYPE_TUPLE) {
        usize len = veclen(base->tuple);

        if (field->type != TOK_INT_LIT) {
            error(semantic->state, field, "Expected integer literal");
            return NULL;
        }

        if (field->integer < 0 || field->integer >= (i64)len) {
            error(semantic->state, field,
                    "Integer literal out of range for tuple indexing");
        }

        return base->tuple[field->integer];
    } else {
        error(semantic->state, curr->tok,
                "Expected a struct or tuple for '.' access");
        return NULL;
    }
}

static ZType *resolveArrSubscript(ZSemantic *semantic, ZNode *curr) {
    ZType *arrType      = resolveType(semantic, curr->subscript.arr);
    ZType *indexType    = resolveType(semantic, curr->subscript.index);

    /* Type not resolved */
    if (!arrType) return NULL;


    if (arrType->kind != Z_TYPE_ARRAY &&
        arrType->kind != Z_TYPE_POINTER) {
        error(semantic->state, curr->tok,
              "Expected an array type for subscript");
        return NULL;
    }

    if (!indexType || indexType->kind != Z_TYPE_PRIMITIVE ||
        !isInteger(indexType->primitive.token->type)) {
        error(semantic->state, curr->tok,
              "Array index must be an integer");
        return NULL;
    }

    if (arrType->kind == Z_TYPE_ARRAY) {
        return arrType->array.base;
    }
    return arrType->base;
}

/* ================== Statement analysis ================== */

static void analyzeVar(ZSemantic *semantic, ZNode *curr, bool isGlobal) {
    ZToken *var = curr->varDecl.ident->identNode.tok;
    if (resolve(semantic, var)) {
        error(semantic->state, var,
                    "Redefinition of variable %s",
                    stoken(var));
    }

    ZType *rvalueType   = NULL;
    ZType *declaredType = NULL;

    if (curr->varDecl.rvalue) {
        rvalueType = resolveType(semantic, curr->varDecl.rvalue);
        rvalueType = resolveTypeRef(semantic, rvalueType);
    }

    if (curr->resolved) {
        declaredType = resolveTypeRef(semantic, curr->resolved);
        curr->resolved = declaredType;
        if (rvalueType &&
            !typesCompatible(semantic->state, declaredType, rvalueType)) {
            error(semantic->state, curr->tok,
                "Type mismatch: lvalue has type '%s' and rvalue has type '%s'",
                stype(declaredType),
                stype(rvalueType)
            );
        }
    } else {
        /* Inferred type (:= syntax) */
        declaredType        = rvalueType;
        curr->resolved      = rvalueType;
    }

    curr->resolved = declaredType;

    ZSymbol *symbol     = makesymbol(Z_SYM_VAR);
    symbol->name        = var;
    symbol->type        = declaredType;
    symbol->node        = curr;
    symbol->isPublic    = isGlobal;
    putSymbol(semantic, symbol);
}

static void analyzeIf(ZSemantic *semantic, ZNode *curr) {
    ZType *cond = resolveType(semantic, curr->ifStmt.cond);
    
    if (!cond) {
        error(semantic->state, curr->ifStmt.cond->tok, "Unknown type condition");
    } else if (!isComparable(semantic, cond)) {
        error(semantic->state,
            curr->ifStmt.cond->tok,
            "%s cannot be used as a condition",
            stype(cond)
        );
    }

    curr->ifStmt.cond = implicitCast(curr->ifStmt.cond, u1Type);

    analyzeBlock(semantic, curr->ifStmt.body, true);

    if (curr->ifStmt.elseBranch) {
        ZNode *el = curr->ifStmt.elseBranch;
        if (el->type == NODE_IF)
            analyzeIf(semantic, el);
        else
            analyzeBlock(semantic, el, true);
    }
}

static void analyzeWhile(ZSemantic *semantic, ZNode *curr) {
    ZType *cond = resolveType(semantic, curr->whileStmt.cond);

    if (!isComparable(semantic, cond)) {
        error(semantic->state, curr->whileStmt.cond->tok,
                "Is not a comparable value");
    }

    curr->whileStmt.cond = implicitCast(curr->whileStmt.cond, u1Type);

    semantic->loopDepth++;
    analyzeBlock(semantic, curr->whileStmt.branch, true);
    semantic->loopDepth--;
}

static void analyzeFor(ZSemantic *semantic, ZNode *curr) {
    beginScope(semantic, curr);
    let f = curr->forStmt;
    analyzeVar(semantic, f.var, false);

    ZType *cond = resolveType(semantic, f.cond);
    curr->forStmt.cond->resolved = cond;

    if (!isComparable(semantic, cond)) {
        error(semantic->state, f.cond->tok, "Is not a comparable value");
    }

    curr->forStmt.cond = implicitCast(curr->forStmt.cond, u1Type);

    resolveType(semantic, f.incr);

    semantic->loopDepth++;
    analyzeBlock(semantic, curr->forStmt.block, false);
    semantic->loopDepth--;

    endScope(semantic);
}

static void analyzeForeign(ZSemantic *semantic, ZNode *curr) {
    if (!resolveTypeRef(semantic, curr->foreignFunc.ret)) {
        error(semantic->state, curr->tok, "Unknown type");
    }
    for (usize i = 0; i < veclen(curr->foreignFunc.args); i++) {
        ZType *arg = curr->foreignFunc.args[i];
        ZType *t = resolveTypeRef(semantic, arg);
        curr->foreignFunc.args[i] = t;
        if (!t) {
            error(semantic->state, arg->tok, "Unknown type");
        }
    }
}

static void analyzeFunc(ZSemantic *semantic, ZNode *curr) {
    beginScope(semantic, curr);
    curr->funcDef.body->scope = semantic->table->current;

    if (curr->funcDef.base) {
        ZType *res = resolveTypeRef(semantic, curr->funcDef.base);
        if (!res) {
            error(semantic->state,
                    curr->funcDef.base->primitive.token,
                    "'%s' is not a valid identifier",
                    curr->funcDef.base->primitive.token->str);
            return;
        }
    }

    if (curr->funcDef.receiver) {
        ZNode *receiver = curr->funcDef.receiver;
        ZType *recType  = resolveTypeRef(semantic, receiver->field.type);
        curr->funcDef.receiver->resolved = recType;
        receiver->field.type = recType;

        ZSymbol *sym = makesymbol(Z_SYM_VAR);
        sym->name       = receiver->field.identifier;
        sym->type       = recType;
        sym->node       = curr->funcDef.receiver;
        sym->isPublic   = false;
        putSymbol(semantic, sym);
    }

    for (usize i = 0; i < veclen(curr->funcDef.args); i++) {
        ZNode  *arg     = curr->funcDef.args[i];
        ZType  *argType = resolveTypeRef(semantic, arg->field.type);
        arg->field.type = argType;

        if (!argType) {
            error(semantic->state, curr->funcDef.name, "Unknown type");
        }
        putRawSymbol(semantic,
                Z_SYM_VAR,
                arg->field.identifier,
                argType,
                arg,
                false);
    }

    ZType *savedRet             = semantic->currentFuncRet;
    ZNode *savedFunc            = semantic->currentFunc;

    semantic->currentFuncRet    = resolveTypeRef(semantic, curr->funcDef.ret);
    semantic->currentFunc       = curr;

    analyzeBlock(semantic, curr->funcDef.body, false);

    semantic->currentFuncRet    = savedRet;
    semantic->currentFunc       = savedFunc;
    
    endScope(semantic);
}

static bool isType(ZType *type, ZTokenType tok) {
    if (type->kind != Z_TYPE_PRIMITIVE) return false;
    return type->primitive.token->type == tok;
}

static void analyzeReturn(ZSemantic *semantic, ZNode *curr) {
    ZType *retType  = NULL;
    ZType *promoted = NULL;
    if (curr->returnStmt.expr) {
        retType     = resolveType(semantic, curr->returnStmt.expr);
        if (!retType) {
            error(semantic->state, curr->tok, "Invalid expression");
            return;
        }
    }

    curr->resolved  = retType;

    if (!semantic->currentFuncRet) return;

    bool isVoidRet  = retType == NULL;
    bool isVoidFunc = isType(semantic->currentFuncRet, TOK_VOID);

    if (isVoidFunc && !isVoidRet) {
        error(semantic->state, semantic->currentFunc->tok,
              "Unexpected return value in void function");
        return;
    } else if (!isVoidFunc && isVoidRet) {
        error(semantic->state, semantic->currentFunc->tok,
              "Expected a return value");
        return;
    } else if (!isVoidFunc && !isVoidRet) {
        promoted = typesCompatible(
            semantic->state, retType, semantic->currentFuncRet
        );

        if (!promoted) {
            error(semantic->state, curr->tok,
                "Expected type %s, got %s",
                stype(semantic->currentFuncRet),
                stype(retType)
            );
        } else {
            /* Implici casting. */
            curr->returnStmt.expr = implicitCast(
                curr->returnStmt.expr, semantic->currentFuncRet
            );
        }
    }

    
}

static void analyzeStmt(ZSemantic *semantic, ZNode *curr) {
    switch (curr->type) {
    case NODE_VAR_DECL: analyzeVar(semantic, curr, false);              break;
    case NODE_IF:       analyzeIf(semantic, curr);                      break;
    case NODE_WHILE:    analyzeWhile(semantic, curr);                   break;
    case NODE_FOR:      analyzeFor(semantic, curr);                     break;
    case NODE_BLOCK:    analyzeBlock(semantic, curr, false);            break;
    case NODE_DEFER:    resolveType(semantic, curr->deferStmt.expr);    break;
    case NODE_RETURN:   analyzeReturn(semantic, curr);                  break;
    default:            resolveType(semantic, curr);                    break;
    }
}

static void analyzeBlock(ZSemantic *semantic, ZNode *block, bool scoped) {
    if (scoped) beginScope(semantic, block);

    ZNode **stmts = block->block;
    usize len = veclen(stmts);
    for (usize i = 0; i < len; i++) {
        if (i + 1 < len && 
            (stmts[i]->type == NODE_BREAK ||
            stmts[i]->type == NODE_CONTINUE ||
            stmts[i]->type == NODE_RETURN)) {
            error(semantic->state, stmts[i+1]->tok, "Unreachable code");

            vecsetlen(block->block, i);

            break;
        }
        analyzeStmt(semantic, stmts[i]);
    }

    if (scoped) endScope(semantic);
}

/* ================== Global scope discovery ================== */

static void discoverGlobalScope(ZSemantic *semantic, ZNode *root) {
    for (usize i = 0; i < veclen(root->module.root); i++) {
        ZNode *child = root->module.root[i];

        switch (child->type) {
        case NODE_FUNC:     putFunc(semantic, child);       break;
        case NODE_STRUCT:   putStruct(semantic, child);     break;
        case NODE_VAR_DECL: putVar(semantic, child, false); break;

        case NODE_TYPEDEF: {
            putTypedef(semantic, child);
            break;
        }

        case NODE_FOREIGN: {
            /* Foreign functions are callable like regular functions.
               pub foreign declarations are placed in the global scope
               so importers of this module can call them without
               re-declaring the foreign themselves. */
            ZSymbol *symbol   = makesymbol(Z_SYM_FUNC);
            symbol->name      = child->foreignFunc.tok;
            symbol->node      = child;
            symbol->type      = child->resolved;
            symbol->isPublic  = child->foreignFunc.pub;
            putSymbol(semantic, symbol);
            break;
        }

        case NODE_MODULE:
            if (child->module.root) {
                registerModule(semantic, child);
                discoverGlobalScope(semantic, child);
                endModule(semantic);
            }
            break;

        default: break;
        }
    }
}

/* ================== Main analysis pass ================== */

static void analyze(ZSemantic *semantic, ZNode *root) {
    root->module.scope = semantic->table->current;
    for (usize i = 0; i < veclen(root->module.root); i++) {
        ZNode *child = root->module.root[i];

        switch (child->type) {
        case NODE_FOREIGN:  analyzeForeign(semantic, child);    break;
        case NODE_FUNC:     analyzeFunc(semantic, child);       break;
        case NODE_VAR_DECL: analyzeVar(semantic, child, true);  break;
        case NODE_MACRO:                                        break;

        case NODE_STRUCT:
            for (usize i = 0; i < veclen(child->structDef.fields); i++) {
                let field = child->structDef.fields[i];
                field->field.type = resolveTypeRef(semantic, field->field.type);
            }
            break;

        case NODE_MODULE:
            if (child->module.root) {
                registerModule(semantic, child);
                analyze(semantic, child);
                endModule(semantic);
            }
            break;


        default: 
            warning(semantic->state, root->tok,
                    "node '%zu' not yet analyzed",
                    root->type);
            break;
        }
    }
}

ZType *makePrimitiveType(ZTokenType type) {
    ZType *self = maketype(Z_TYPE_PRIMITIVE);
    self->primitive.token = maketoken(type, NULL);
    return self;
}

ZSemantic *zanalyze(ZState *state, ZNode *root) {
    state->currentPhase = Z_PHASE_SEMANTIC;
    ZSemantic *semantic = makesemantic(state, root);

    if (!none)      none    = maketype(Z_TYPE_NONE);
    if (!ztrue)     ztrue   = makePrimitiveType(TOK_TRUE);
    if (!zfalse)    zfalse  = makePrimitiveType(TOK_FALSE);
    if (!zvoid)     zvoid   = makePrimitiveType(TOK_VOID);
    if (!u1Type)    u1Type  = makePrimitiveType(TOK_BOOL);

    registerModule(semantic, root);
    discoverGlobalScope(semantic, root);
    analyze(semantic, root);
    return semantic;
}
