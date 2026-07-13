// #include "zinc.h"
//
// typedef struct ZAllowedAnnotation {
//     const char *name;
//     struct ZAllowedAnnotation *children;
//     ZNode *node;
// } ZAllowedAnnotation;
//
// ZAllowedAnnotation langItems[] = {
//     { "reflect",                    NULL, NULL      },
//     { "type_info",                  NULL, NULL      },
//     { "type_info_enum_variant",     NULL, NULL      },
//     { "type_info_struct_member",    NULL, NULL      },
//     NULL
// };
//
// static ZAllowedAnnotation annotations[] = {
//     { "lang", (ZAllowedAnnotation *)langItems,  NULL },
//     NULL
// };
//
// ZAllowedAnnotation *queryAnnotation(ZAnnotation *annotation) {
//     const char *name = stoken(annotation->name);
//     for (usize i = 0; annotations + i; i++) {
//         if (strcmp(annotations[i].name, name) == 0) {
//
//         }
//     }
//     return NULL;
// }
