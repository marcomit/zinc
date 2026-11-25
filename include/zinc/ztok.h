#ifndef ZTOK_H
#define ZTOK_H

#include "base.h"
#include "zmem.h"
#include "zforward.h"

typedef enum {

#define DEF(id, str, m) id,
#define TOK_FLOWS
#define TOK_TYPES
#define TOK_DYN
#define TOK_SYMBOLS

#include "ztok.def"

#undef TOK_FLOWS
#undef TOK_TYPES
#undef TOK_DYN
#undef TOK_SYMBOLS

#undef DEF

} ZTokenType;

typedef struct ZToken ZToken;

struct ZToken {
    ZTokenType  type;
    union {
        char    *str;
        i64     integer;
        f64     floating;
        bool    boolean;
        ZToken  **stream;
    };

    /* The index in the FileTable, stored in the ZState object. */
    // u32         fileId;
    //
    // u32         srcOffset;
    // u32         srcLineOffset;
    //
    // u32         startOffset;
    // u32         len;

    char        *filename;
    char        *sourcePtr;
    char        *sourceLinePtr;
    char        *start;
    char        *end;
    usize       row;
    usize       col;
    bool        newlineBefore;
};

typedef struct ZTokenStream {
    ZToken **list;
    usize current;
    usize end; // Cached vector length
    struct ZTokenStream *prev;
} ZTokenStream;

ZToken **ztokenize(Allocator *, ZState *, ZFile *);
ZToken **ztokenizeSource(Allocator *, ZState *, char *);
ZToken *maketoken(Allocator *, ZTokenType, char *, char *);
ZToken *makeident(Allocator *, char *, char *, char *);
ZTokenStream *maketokstream(Allocator *, ZToken **, ZTokenStream *);
bool tokeneq(ZToken *, ZToken *);
bool tokmask(ZToken *, u16);

u32 hashStr(const char *, usize);
u32 hashtoken(ZToken *);

char *stoken(ZToken *);
char *tokname(ZTokenType);
void printToken(ZToken *);
void printTokens(ZToken **);

#endif // ZTOK_H
