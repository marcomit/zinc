#ifndef ZANN_H
#define ZANN_H

#include "zforward.h"
#include "ztok.h"

typedef enum {
    Z_LANG_NONE,

    #define LANG(id, n, t, f, v) id,
    #include "zlang.def"
    #undef LANG

    Z_LANG_COUNT
} ZLangItemType;

typedef struct {
    ZLangItemType   type;
    ZNode           *node;
} ZLangItem;


typedef enum ZAnnotationKind {
    Z_ANN_IDENT     = 1 << 0,
    Z_ANN_LIT       = 1 << 1,
    Z_ANN_NESTED    = 1 << 2,
    Z_ANN_ASSIGN    = 1 << 3
} ZAnnotationKind;

struct ZAnnotation {
    ZAnnotationKind     kind;
    ZToken              *tok;
    union {
        struct {
            ZLangItemType   li;
            ZToken          *tok;
        } ident;
        ZToken          *literal;
        ZAnnotation     **nested;
        struct {
            ZToken      *name;
            ZAnnotation *value;
        } assign;
    };
    bool                used;
};

typedef struct ZAnnotationSpec ZAnnotationSpec;
typedef struct ZAnnotationQuery ZAnnotationQuery;

typedef enum {
    Z_TRG_ANY       = 1 << 0x00,
    Z_TRG_FUNC      = 1 << 0x01,
    Z_TRG_STRUCT    = 1 << 0x02,
    Z_TRG_ENUM      = 1 << 0x03,
    Z_TRG_VAR       = 1 << 0x04,
    Z_TRG_FOREIGN   = 1 << 0x05,
    Z_TRG_IMPL      = 1 << 0x06,
    Z_TRG_FACET     = 1 << 0x07,
    Z_TRG_EXPR      = 1 << 0x08,
    Z_TRG_STMT      = 1 << 0x09,
    Z_TRG_VAR_DECL  = 1 << 0x0A
} ZAnnotationTarget;

ZAnnotation *query(ZAnnotation **, const char *);
ZAnnotation *queryFrom(ZAnnotation **, const char *, usize *);
ZAnnotation *queryArg(ZAnnotation *, const char *);
ZAnnotation *annArg(ZAnnotation *, usize);
usize annLen(ZAnnotation *);
ZLangItemType getLangItemType(ZNode *);
void analyzeAnnotations(ZState *, ZNode *);


#endif // ZANN_H
