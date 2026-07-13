#include "zinc.h"

typedef struct ZAnnotationTableEntry {
    const char *name;
    struct ZAnnotationTableEntry *children;
    ZLangItem langItem;
} ZAnnotationTableEntry;

ZAnnotationTableEntry langItems[] = {
    { "reflect",                    NULL, Z_LANG_REFLECT                    },
    { "type_info",                  NULL, Z_LANG_TYPE_INFO                  },
    { "type_info_enum_variant",     NULL, Z_LANG_TYPE_INFO_ENUM_VARIANT     },
    { "type_info_struct_member",    NULL, Z_LANG_TYPE_INFO_STRUCT_MEMBER    },
    
    { "source_location_type",       NULL, Z_LANG_SOURCE_LOCATION_TYPE       },
    { "source_location_func",       NULL, Z_LANG_SOURCE_LOCATION_FUNC       },
    { NULL,                         NULL, Z_LANG_NONE                       }
};

static ZAnnotationTableEntry annotations[] = {
    { "lang",                       langItems,  Z_LANG_NONE             },
    { NULL,                         NULL,       Z_LANG_NONE             }
};

ZAnnotationTableEntry *queryAnnotation(ZAnnotation *annotation) {
    const char *name = stoken(annotation->name);
    for (usize i = 0; annotations + i; i++) {
        if (strcmp(annotations[i].name, name) == 0) {

        }
    }
    return NULL;
}
