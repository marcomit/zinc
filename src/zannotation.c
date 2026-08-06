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

static ZAnnotationSpec Literal = {
    NULL, Z_ANN_LIT, 0, 0, 0, 0, true, NULL
};

static const ZAnnotationSpec None = {
    NULL, 0, 0, 0, 0, 0, false, NULL
};

static ZAnnotationSpec LangItemsSpec[] = {
    { "type_info",              Z_ANN_IDENT, Z_LANG_TYPE_INFO,                 Z_TRG_ENUM,     0, 0, false, NULL },
    { "type_info_struct",       Z_ANN_IDENT, Z_LANG_TYPE_INFO_STRUCT,          Z_TRG_STRUCT,   0, 0, false, NULL },
    { "type_info_struct_field", Z_ANN_IDENT, Z_LANG_TYPE_INFO_STRUCT_FIELD,    Z_TRG_STRUCT,   0, 0, false, NULL },
    { "reflect",                Z_ANN_IDENT, Z_LANG_REFLECT,                   Z_TRG_FUNC,     0, 0, false, NULL },

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
    { "export", Z_ANN_IDENT | Z_ANN_ASSIGN, 0,              Z_TRG_FUNC, 0, 0, true,  &Literal       },

    { "inline", Z_ANN_IDENT | Z_ANN_NESTED, 0,              Z_TRG_FUNC, 1, 1, true, InlineArgs      },

    None
};


typedef void (*AnnotationFunc)(ZState *, ZNode *, ZAnnotation *);

#define FLAG(flag, mask) (((flag) & (mask)) == 0)

static void analyzeAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation,
    ZAnnotationSpec *spec, u16 targetMask) {
    for (; spec != &None; spec++) {
        if (FLAG(spec->targetMask, targetMask))             continue;
        if (strcmp(spec->name, annotation->tok->str) != 0)  continue;

        if (!(annotation->kind & spec->spec)) {
            error(state, annotation->tok, "Invalid annotation kind");
            return;
        }

        if (spec->langItem) {
            if (LangItems[spec->langItem]) {
                error(state, annotation->tok, "Lang-item already registered");
                continue;
            }
            LangItems[spec->langItem] = node;
            error(state, annotation->tok, "Registered lang item");
        }

        if (annotation->kind == Z_ANN_NESTED) {
            for (usize i = 0; i < veclen(annotation->nested); i++) {
                analyzeAnnotation(
                    state, node, annotation->nested[i], spec->args, targetMask
                );
            }
        } else if (annotation->kind == Z_ANN_ASSIGN) {
            analyzeAnnotation(
                state, node, annotation->assign.value, spec->args, targetMask
            );
        }
    }
}

static void analyzeFuncAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_ANY | Z_TRG_FUNC);
}
static void analyzeStructAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_ANY | Z_TRG_STRUCT);
}

static void analyzeEnumAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_ANY | Z_TRG_ENUM);
}

static void analyzeForeignAnnotation(
    ZState *state, ZNode *node, ZAnnotation *annotation) {
    analyzeAnnotation(state, node, annotation, Annotations, Z_TRG_ANY | Z_TRG_FOREIGN);
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
    default:
        warning(state, node->tok, "This node doesn't contain annotations");
        return;
    }

    hashset_t seen = NULL;

    for (usize i = 0; i < veclen(annotations); i++) {
        if (!hashset_insert(&seen, annotations[i]->tok->str)) {
            error(state, annotations[i]->tok,
                "Annotation '%s' already declared",
                annotations[i]->tok->str
            );
            continue;
        }
        func(state, node, annotations[i]);
    }
}
