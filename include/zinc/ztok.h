#ifndef ZTOK_H
#define ZTOK_H

#include "base.h"
#include "zmem.h"
#include "zforward.h"
#include "zconfig.h"
#include "zstr.h"

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
    union { char *str; i64 integer; f64 floating; bool boolean; ZToken **stream; };
    u32 file_id, offset, len, row, col;
    u8  type;
    bool newlineBefore;
};

typedef struct ZTokenStream {
    ZToken **list;
    usize current;
    usize end;
    struct ZTokenStream *prev;
} ZTokenStream;

ZToken **ztokenize(Allocator *, ZState *, ZFile *);
ZToken **ztokenizeSource(Allocator *, ZState *, char *);
ZToken *maketoken(Allocator *, ZTokenType, u32 file_id, u32 offset, u32 len);
ZToken *makeident(Allocator *, char *content, u32 file_id, u32 offset);
ZTokenStream *maketokstream(Allocator *, ZToken **, ZTokenStream *);
bool tokeneq(ZToken *, ZToken *);
bool tokmask(ZToken *, u16);

u32 hashStr(const char *, usize);
u32 hashtoken(ZToken *);

char *stoken(ZToken *);
char *tokname(ZTokenType);
ZFile *tokfile(ZState *, ZToken *);
const char *tokfilename(ZState *, ZToken *);
const char *toksource(ZState *, ZToken *);
const char *tokline(ZState *, ZToken *);
zsv toksv(ZState *, ZToken *);
u32 tokcol(ZToken *);
void printToken(ZToken *);
void printTokens(ZToken **);

#endif // ZTOK_H
