// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zinc.h"

typedef struct ZAnnotationSpec ZAnnotationSpec;

typedef enum {
    Z_TRG_ANY       = 1 << 0,
    Z_TRG_FUNC      = 1 << 1,
    Z_TRG_STRUCT    = 1 << 2,
    Z_TRG_ENUM      = 1 << 3,
    Z_TRG_VAR       = 1 << 4,
    Z_TRG_FOREIGN   = 1 << 5,
    Z_TRG_IMPL      = 1 << 6
} ZAnnotationTarget;

struct ZAnnotationSpec {
    const char *name;
    u32 spec;
    ZLangItem langItem;
    ZAnnotationTarget targetMask;
    u8 minArg, maxArg;
    bool repeatable;
    ZAnnotationSpec *args;
};

ZNode *LangItems[Z_LANG_COUNT] = { NULL };

static const ZAnnotationSpec Literal = {
    NULL, Z_ANN_LIT, 0, Z_TRG_ANY, 0, 0, true, NULL
};

#define None { NULL, 0, 0, 0, 0, 0, false, NULL }

static ZAnnotationSpec LangItemsSpec[] = {
    { "type_info",              Z_ANN_IDENT, Z_LANG_TYPE_INFO,                 Z_TRG_ENUM,     0, 0, false, NULL },
    { "type_info_struct",       Z_ANN_IDENT, Z_LANG_TYPE_INFO_STRUCT,          Z_TRG_STRUCT,   0, 0, false, NULL },
    { "type_info_struct_field", Z_ANN_IDENT, Z_LANG_TYPE_INFO_STRUCT_FIELD,    Z_TRG_STRUCT,   0, 0, false, NULL },

    { "reflect",                Z_ANN_IDENT, Z_LANG_REFLECT,                   Z_TRG_FOREIGN,  0, 0, false, NULL },

    { "source_location_type",   Z_ANN_IDENT, Z_LANG_SOURCE_LOCATION_TYPE,      Z_TRG_STRUCT,   0, 0, false, NULL },
    { "source_location_func",   Z_ANN_IDENT, Z_LANG_SOURCE_LOCATION_FUNC,      Z_TRG_FOREIGN,  0, 0, false, NULL },

    None,
};

static ZAnnotationSpec InlineArgs[] = {
    { "always", Z_ANN_IDENT, 0, Z_TRG_FUNC, 0, 0, true, NULL },
    { "never",  Z_ANN_IDENT, 0, Z_TRG_FUNC, 0, 0, true, NULL },

    None
};

static ZAnnotationSpec Annotations[] = {
    { "lang",   Z_ANN_NESTED,               0,              Z_TRG_ANY,  1, 1, false, LangItemsSpec  },
    { "here",   Z_ANN_IDENT,                Z_LANG_HERE,    Z_TRG_VAR,  0, 0, false, NULL           },
    { "export", Z_ANN_IDENT | Z_ANN_ASSIGN, 0,              Z_TRG_FUNC, 0, 0, true,
        (ZAnnotationSpec[]){ Literal, None }
    },

    { "inline", Z_ANN_IDENT | Z_ANN_NESTED, 0,              Z_TRG_FUNC, 1, 1, true, InlineArgs      },

    { "packed", Z_ANN_IDENT,                0,              Z_TRG_STRUCT,   0, 0, false, NULL       },
    { "cold",   Z_ANN_IDENT,                0,              Z_TRG_FUNC,     0, 0, false, NULL       },

    { "overload", Z_ANN_NESTED,             0,              Z_TRG_FUNC,     1, 1, true,
        (ZAnnotationSpec[]){ Literal, None }
    },

    None
};

typedef void (*AnnotationFunc)(ZState *, ZNode *, ZAnnotation *);

#define FLAG(flag, mask) (((flag) & (mask)) != 0)

static void analyzeAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation,
    ZAnnotationSpec *spec, u16 targetMask) {
    ZAnnotationSpec *item   = NULL;
    bool mistargeted        = false;

    for (; spec->spec; spec++) {
        if (spec->name) {
            if (strcmp(spec->name, stoken(annotation->tok)) != 0)    continue;
        } else if (!FLAG(annotation->kind, spec->spec))              continue;

        if (!FLAG(spec->targetMask, Z_TRG_ANY | targetMask)) {
            mistargeted = true;
            continue;
        }

        if (!FLAG(annotation->kind, spec->spec)) {
            error(state, annotation->tok, "Invalid annotation kind");
            return;
        }
        item = spec;
        break;
    }

    if (!item) {
        if (mistargeted) {
            error(state, annotation->tok,
                "Annotation '%s' is not valid on this declaration",
                stoken(annotation->tok)
            );
        } else {
            error(state, annotation->tok,
                "Unknown annotation '%s'", stoken(annotation->tok)
            );
        }
        return;
    }
    if (item->langItem) {
        if (LangItems[item->langItem]) {
            error(state, annotation->tok, "Lang-item already registered");
            return;
        }
        LangItems[item->langItem] = node;
        info(state, annotation->tok, "Registered lang item");
    }

    if (annotation->kind == Z_ANN_NESTED) {
        usize len = veclen(annotation->nested);
        if (len < item->minArg || len > item->maxArg) {
            error(state, annotation->tok,
                "Accepted from %d to %d, got %zu argument(s)",
                item->minArg, item->maxArg, len
            );
            return;
        }
        for (usize i = 0; i < veclen(annotation->nested); i++) {
            analyzeAnnotation(
                state, node, annotation->nested[i], item->args, targetMask
            );
        }
    } else if (annotation->kind == Z_ANN_ASSIGN) {
        analyzeAnnotation(
            state, node, annotation->assign.value, item->args, targetMask
        );
    }
}

static void analyzeFuncAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_FUNC);
}

static void analyzeStructAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_STRUCT);
}

static void analyzeEnumAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_ENUM);
}

static void analyzeForeignAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_FOREIGN);
}

static void analyzeImplAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_IMPL);
}

static bool isRepeatable(ZAnnotation *annotation) {
    for (ZAnnotationSpec *spec = Annotations; spec->spec; spec++) {
        if (!spec->name)                                        continue;
        if (strcmp(spec->name, stoken(annotation->tok)) != 0)   continue;
        return spec->repeatable;
    }
    return false;
}

void analyzeAnnotations(ZState *state, ZNode *node) {
    if (!node) return;

    ZAnnotation **annotations   = NULL;
    AnnotationFunc func         = NULL;

    switch (node->type) {
    case NODE_ENUM:
        annotations = node->enumDef.annotations;
        func = analyzeEnumAnnotation;
        break;
    case NODE_FUNC:
        annotations = node->funcDef.annotations;
        func = analyzeFuncAnnotation;
        break;
    case NODE_STRUCT:
        annotations = node->structDef.annotations;
        func = analyzeStructAnnotation;
        break;
    case NODE_FOREIGN:
        annotations = node->foreignDecl.annotations;
        func = analyzeForeignAnnotation;
        break;
    case NODE_IMPL:
        annotations = node->impl.annotations;
        func = analyzeImplAnnotation;

        /* Methods carry their own annotations and are never visited by the
         * top-level walk in discoverGlobalScope. */
        for (usize i = 0; i < veclen(node->impl.funcs); i++) {
            analyzeAnnotations(state, node->impl.funcs[i]);
        }
        break;
    default:
        /* Not every declaration accepts annotations; that is not an error. */
        return;
    }

    hashset_t seen = NULL;

    for (usize i = 0; i < veclen(annotations); i++) {
        if (!hashset_insert(&seen, stoken(annotations[i]->tok))
            && !isRepeatable(annotations[i])) {
            error(state, annotations[i]->tok,
                "Annotation '%s' already declared",
                stoken(annotations[i]->tok)
            );
            continue;
        }
        func(state, node, annotations[i]);
    }

    hashset_free(&seen);
}
