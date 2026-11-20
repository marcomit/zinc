#ifndef ZTYPE_H
#define ZTYPE_H

#include "zforward.h"
#include "base.h"
#include "zmem.h"
#include "zconfig.h"
#include "ztok.h"
#include "zann.h"

typedef enum ZTypeKind {
    #define TYPE(name, prime, masks) name,
    #include "ztype.def"
    #undef TYPE

    Z_TYPE_COUNT
} ZTypeKind;

extern const u16 ZTypeMasks[Z_TYPE_COUNT];

#define typeKindIs(kind, mask) ((ZTypeMasks[(kind)] & (mask)) != 0)

struct ZType {
    ZTypeKind   kind;
    ZToken      *tok;
    u32         hash;
    // ZRegion     *region;
    ZAnnotation **annotations;
#if Z_LSP
    ZToken *lsp_start, *lsp_end;
#endif

    union {
        struct {
            ZType   *base;
            ZToken  *prop;
        } space;

        // For PRIMITIVE (e.g. void or int)
        struct {
            ZToken  *token;
            ZType   *base;
            ZType   **generics;
        } primitive;

        // For POINTER (The type the pointer points to)
        ZType       *base;

        struct {
            ZToken      *name;

            /* Array of NODE_FIELD */
            ZNode       **fields;
            ZType       **generics;
            ZToken      **regions;
        } strct;

        struct {
            ZType   *ret;
            ZType   **args;
            ZType   **generics;
            ZType   **capabilities;
            bool    variadic;
        } func;

        struct {
            ZType   *base;
            usize   size;
            bool    dynamic;
        } array;

        struct {
            ZToken  *name;

            /* List of NODE_FIELD */
            ZNode   **funcs;

            /* List of satisfied types.
             * Types are added in this list during the semantic pass
             * and only when a type tries to convert into a facet
             * */
            ZType   **satisfied;
        } facet;

        struct {
            ZToken      *name;

            /* Array of Z_TYPE_STRUCT. */
            ZNode       **fields;
            ZType       **generics;

            ZToken      *integer;
            ZToken      **regions;
        } enm;

        ZType **tuple;

        struct {
            ZToken  *name;

            /* A generic can extend a facets:
             * T: Display + Drop
             * */
            ZType   **extensions;

            ZType   **instantiations;
        } generic;

        ZType   **sumType;

        ZType   *optional;

        struct {
            ZType *success;
            ZType *error;
        } result;

        ZToken **chain;

        ZType *typeType;
    };

    /* Future implementation:
     * Constant values for now are not checked in the semantic analyzer.
     * So you can assign a value to a constant variable. */
    bool constant;
};

ZType *maketype(Allocator *, ZTypeKind);
ZType *makePrimitiveType(Allocator *, ZTokenType);

u32 hashType(ZType *);

void typesSort(ZType **);
bool isVoid(ZType *);
bool typesEqual(ZType *, ZType *);
ZType *typeReplace(ZType *root, ZType *from, ZType *to);
bool typesPrimitive(ZType *);
i32 sumTypeIndexOf(ZType *sum, ZType *concrete);

char *stype(ZType *);
void encodeType(ZType *, char **);
char *manglerM(ZType *, ZToken *);

#endif // ZTYPE_H
