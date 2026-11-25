#ifndef ZFILE_H
#define ZFILE_H

#include "zmem.h"
#include "zstr.h"
#include "ztok.h"

typedef struct ZFileTable ZFileTable;
typedef struct ZFile ZFile;

ZFile *zfile_create(Allocator *, const char *filename, zstr content);
zstr zfile_content(ZFile *);
u32 zfile_id(ZFile *);
const char *zfile_name(ZFile *);
ZNode *zfile_root(ZFile *);
void zfile_set_root(ZFile *, ZNode *);
Allocator *zfile_allocator(ZFile *);

zstr zfile_str(ZFile *, u32 offset, u32 len);
zstr zfile_tok(ZFile *, ZToken *);

ZFile *zfile_get(ZFileTable *, u32 file_id);
ZFile *zfile_get_from_tok(ZFileTable *, ZToken *);
const char *zfilename_from_tok(ZFileTable *, ZToken *);

ZFileTable *ft_create();
ZFile *ft_insert(Allocator *, ZFileTable *, const char *, bool *);
ZFile *ft_get(ZFileTable *, const char *);
vec(ZFile *) ft_files(ZFileTable *);
void ft_destroy(ZFileTable *ft);

#endif // ZFILE_H
