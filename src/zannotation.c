// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2025, Marco Menegazzi

#include "zinc.h"

typedef struct ZAnnotationSpec ZAnnotationSpec;

struct ZAnnotationSpec {
    const char *name;
    usize minArgs, maxArgs;
    ZAnnotationSpec *variants;
    ZLangItem langItem;
};

static ZAnnotationSpec inlineModes[] = {
    { "always", 0, 0, NULL, Z_LANG_NONE },
    { "never",  0, 0, NULL, Z_LANG_NONE },
    { NULL,     0, 0, NULL, Z_LANG_NONE },
};

static ZAnnotationSpec langItemsSpec[] = {
    { "type_info",                  0, 0, NULL, Z_LANG_NONE },
    { "type_info_struct_member",    0, 0, NULL, Z_LANG_NONE },
    { "type_info_enum_variant",     0, 0, NULL, Z_LANG_NONE },

    { "reflect",                    0, 0, NULL, Z_LANG_NONE },

    { "source_location_type",       0, 0, NULL, Z_LANG_NONE },
    { "source_location_func",       0, 0, NULL, Z_LANG_NONE },

    { NULL,                         0, 0, NULL, Z_LANG_NONE },
};

static ZAnnotationSpec annotations[] = {
    { "packed", 0, 0, NULL,             Z_LANG_NONE },
    { "zero",   0, 0, NULL,             Z_LANG_NONE },
    { "inline", 0, 1, inlineModes,      Z_LANG_NONE },
    { "lang",   1, 1, langItemsSpec,    Z_LANG_NONE },
    { "export", 1, 1, NULL,             Z_LANG_NONE },
    { NULL,     0, 0, NULL,             Z_LANG_NONE },
};

// static ZAnnotationSpec *_queryAnnotation(ZAnnotationSpec *spec, ZAnnotation *annotation) {
//     if (strcmp(spec->name, stoken(annotation->name)) != 0) {
//         return NULL;
//     }
//
//     usize args = veclen(annotation->args);
//
//     if (args < spec->minArgs || args > spec->maxArgs) return NULL;
//
//     for (usize i = 0; i < args; i++) {
//         ZAnnotationSpec *res = _queryAnnotation(
//             spec->variants + i, annotation->args[i]);
//     }
//
//     return NULL;
// }
//
// static void queryAnnotationSpec(ZState *state, ZAnnotation *annotation) {
//
// }
