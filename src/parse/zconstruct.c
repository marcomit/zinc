/**
 * @file zconstruct.c - a memory save strategy, maybe not so safe.
 *
 * Every tagged union allocates some shared fields and then the union,
 * which has the largest size of its variants.
 * Since ZNode and ZType don't change their kind, it should allocate more memory
 * than they need. So this file allocates the size of shared fields + the exact size variant.
 *
 * With this approach it is guaranteed to allocate <= sizeof(TaggedUnion),
 * which is about ~7% on a normal program.
 * But it's not the safest approach.
 * The user can access a field which is not the current variant and
 * It will be a memory access on a non-owned memory.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 * */

#include "zinc/znode.h"
#include "zparse.h"
#include <assert.h>

#define NODE_ALIGN(n)   (((n) + 7) & ~(usize)7)
#define NODE_HEADER     NODE_ALIGN(offsetof(ZNode, ifStmt))
#define NODE_SIZE(v)    NODE_ALIGN(offsetof(ZNode, v) + sizeof(((ZNode *)0)->v))

#define TYPE_ALIGN(n)   NODE_ALIGN(n)
#define TYPE_HEADER     TYPE_ALIGN(offsetof(ZType, space))
#define TYPE_SIZE(v)    TYPE_ALIGN(offsetof(ZType, v) + sizeof(((ZType *)0)->v))
#define TYPE_MAX(a, b)  ((a) > (b) ? (a) : (b))

static const usize NodeSize[NODE_TYPE_COUNT] = {
    [NODE_BLOCK]                = NODE_SIZE(capabilities),
    [NODE_IF]                   = NODE_SIZE(ifStmt),
    [NODE_WHILE]                = NODE_SIZE(whileStmt),
    [NODE_RETURN]               = NODE_SIZE(returnStmt),
    [NODE_VAR_DECL]             = NODE_SIZE(varDecl),
    [NODE_BINARY]               = NODE_SIZE(binary),
    [NODE_UNARY]                = NODE_SIZE(unary),
    [NODE_CALL]                 = NODE_SIZE(call),
    [NODE_FUNC]                 = NODE_SIZE(funcDef),
    [NODE_LITERAL]              = NODE_SIZE(literalTok),
    [NODE_IDENTIFIER]           = NODE_SIZE(identNode),
    [NODE_STRUCT]               = NODE_SIZE(structDef),
    [NODE_SUBSCRIPT]            = NODE_SIZE(subscript),
    [NODE_MEMBER]               = NODE_SIZE(memberAccess),
    [NODE_MODULE]               = NODE_SIZE(module),
    [NODE_FIELD]                = NODE_SIZE(field),
    [NODE_EMBED_FIELD]          = NODE_SIZE(field),
    [NODE_FUNC_FIELD]           = NODE_SIZE(field),
    [NODE_TYPEDEF]              = NODE_SIZE(typeDef),
    [NODE_FOREIGN]              = NODE_SIZE(foreignDecl),
    [NODE_DEFER]                = NODE_SIZE(deferStmt),
    [NODE_STRUCT_LIT]           = NODE_SIZE(structlit),
    [NODE_TUPLE_LIT]            = NODE_SIZE(tuplelit),
    [NODE_ARRAY_LIT]            = NODE_SIZE(arraylit),
    [NODE_MACRO]                = NODE_HEADER,
    [NODE_TYPE]                 = NODE_HEADER,
    [NODE_ENUM]                 = NODE_SIZE(enumDef),
    [NODE_BREAK]                = NODE_SIZE(breakStmt),
    [NODE_CONTINUE]             = NODE_SIZE(breakStmt),
    [NODE_ENUM_FIELD]           = NODE_SIZE(enumField),
    [NODE_CAST]                 = NODE_SIZE(castExpr),
    [NODE_SIZEOF]               = NODE_SIZE(sizeofExpr),
    [NODE_NAMESPACE]            = NODE_SIZE(capabilities),
    [NODE_SLICE]                = NODE_SIZE(slice),
    [NODE_CAPABILITY]           = NODE_SIZE(capability),
    [NODE_MATCH]                = NODE_SIZE(match),
    [NODE_MATCH_ARM]            = NODE_SIZE(matchArm),
    [NODE_ENUM_LIT]             = NODE_SIZE(call),
    [NODE_ENUM_LIT_NO_PAYLOAD]  = NODE_SIZE(memberAccess),
    [NODE_FACET]                = NODE_SIZE(facet),
    [NODE_IMPL]                 = NODE_SIZE(impl),
    [NODE_FORIN]                = NODE_SIZE(forin),
    [NODE_UNWRAP]               = NODE_SIZE(unwrap),
    [NODE_RANGE]                = NODE_SIZE(binary),
    [NODE_INTERPOLATION]        = NODE_SIZE(interpolation),
};

static const usize TypeSize[Z_TYPE_COUNT] = {
    [Z_TYPE_PRIMITIVE]  = TYPE_MAX(TYPE_SIZE(primitive), TYPE_SIZE(array)),
    [Z_TYPE_POINTER]    = TYPE_SIZE(base),
    [Z_TYPE_STRUCT]     = TYPE_SIZE(strct),
    [Z_TYPE_ARRAY]      = TYPE_SIZE(array),
    [Z_TYPE_FUNCTION]   = TYPE_SIZE(func),
    [Z_TYPE_TUPLE]      = TYPE_SIZE(tuple),
    [Z_TYPE_GENERIC]    = TYPE_SIZE(generic),
    [Z_TYPE_FACET]      = TYPE_SIZE(facet),
    [Z_TYPE_ENUM]       = TYPE_SIZE(enm),
    [Z_TYPE_NONE]       = TYPE_HEADER,
    [Z_TYPE_NAMESPACE]  = TYPE_HEADER,
    [Z_TYPE_SUM]        = TYPE_SIZE(sumType),
    [Z_TYPE_OPTIONAL]   = TYPE_SIZE(optional),
    [Z_TYPE_RESULT]     = TYPE_SIZE(result),
    [Z_TYPE_TYPE]       = TYPE_SIZE(typeType),
};

ZNode *makenode(Allocator *allocator, ZNodeType type) {
    assert(type < NODE_TYPE_COUNT && NodeSize[type] != 0);
    usize node_size = NodeSize[type];
    ZNode *self = aalloc(allocator, node_size);
    memset(self, 0, node_size);
    self->type = type;

    return self;
}

ZType *maketype(Allocator *allocator, ZTypeKind kind) {
    assert(kind < Z_TYPE_COUNT && TypeSize[kind] != 0);
    usize type_size = TypeSize[kind];
    ZType *self = aalloc(allocator, type_size);
    memset(self, 0, type_size);
    self->kind = kind;
    return self;
}
