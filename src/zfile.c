#include "zinc/zforward.h"
#include "zinc/zinc.h"
#include "zmem.h"
#include "zstr.h"

#define ANON_FILE 0x21092005

#define LOCK(ft) pthread_mutex_lock(&(ft)->mutex)
#define UNLOCK(ft) pthread_mutex_unlock(&(ft)->mutex)

struct ZFile {
    Allocator   *allocator;
    const char  *filename;
    zstr        content;
    ZNode       *root;
    u32         id;
};

struct ZFileTable {
    Allocator                   *allocator;
    vec(ZFile *)                files;
    ZMap(zsv, usize)            file_map;
    pthread_mutex_t             mutex;
};

ZFile *zfile_create(Allocator *a, const char *filename, zstr content) {
    ZFile *f = aalloc(a, sizeof(ZFile));
    *f = (ZFile){
        .allocator  = a,
        .filename   = filename,
        .content    = content,
    };
    return f;
}

const char *zfile_name(ZFile *file) {
    return file->filename;
}
zstr zfile_content(ZFile *file) {
    return file->content;
}

u32 zfile_id(ZFile *file) {
    return file->id;
}

ZNode *zfile_root(ZFile *file) {
    return file ? file->root : NULL;
}

Allocator *zfile_allocator(ZFile *file) {
    return file ? file->allocator : NULL;
}

ZFileTable *ft_create() {
    ZFileTable *self = copy(heapAllocator, ZFileTable, ((ZFileTable){
        .allocator = heapAllocator,
        .files = NULL
    }));
    mapinitzsv(self->file_map, heapAllocator);
    pthread_mutex_init(&self->mutex, NULL);
    return self;
}

static zstr ft_readfile(Allocator *allocator, const char *filename) {
    FILE *fp = fopen(filename, "r");
    if (!fp) return NULL;

    zstr s = zs_with(allocator);
    char line[1024];
    while (( fgets(line, sizeof line, fp) ))
        zs_catf(&s, "%s", line);

    fclose(fp);
    return s;
}

ZFile *ft_get(ZFileTable *ft, const char *filename) {
    LOCK(ft);

    ZFile *f = NULL;
    usize *index = mapget(ft->file_map, ZSV(filename));
    if (index) f = ft->files[*index];

    UNLOCK(ft);

    return f;
}

void zfile_set_root(ZFile *file, ZNode *root) {
    if (file) file->root = root;
}

ZFile *ft_insert(Allocator *a, ZFileTable *ft, const char *filename, bool *created) {
    LOCK(ft);
    ZFile *res = NULL;
    usize *index = mapget(ft->file_map, ZSV(filename));
    bool c = false;
    if (index) {
        res = ft->files[*index];
        goto end;
    }

    zstr file_content = ft_readfile(ft->allocator, filename);
    if (!file_content) goto end;

    ZFile *f = zfile_create(a, filename, file_content);

    vecpush(ft->files, f);
    f->id = veclen(ft->files) - 1;
    mapput(ft->file_map, ZSV(f->filename), f->id);
    res = ft->files[f->id];
    c = true;

end:
    if (created) *created = c;
    UNLOCK(ft);
    return res;
}

vec(ZFile *) ft_files(ZFileTable *ft) { return ft->files; }

zstr zfile_str(ZFile *f, u32 offset, u32 len) {
    zstr s = zs_with(f->allocator);
    zs_catn(&s, f->content + offset, len);
    return s;
}

void zfile_destroy(ZFile *file) {
    zs_free(file->content);
    afree(file->allocator, file);
}

void ft_destroy(ZFileTable *ft) {
    for (usize i = 0; i < veclen(ft->files); i++)
        zfile_destroy(ft->files[i]);

    mapfree(ft->file_map);
    vecfree(ft->files);
    afree(ft->allocator, ft);
}

ZFile *zfile_get(ZFileTable *ft, u32 file_id) {
    if (file_id >= veclen(ft->files)) return NULL;
    return ft->files[file_id];
}

// ZFile *zfile_get_from_tok(ZFileTable *ft, ZToken *t) {
//     return zfile_get(ft, t->file_id);
// }
//
// const char *zfilename_from_tok(ZFileTable *ft, ZToken *t) {
//     if (!ft || !t) return NULL;
//     ZFile *f = zfile_get(ft, t->file_id);
//     return f ? f->filename : NULL;
// }

static bool file_exists(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    fclose(f);
    return true;
}

static bool is_absolute_path(const char *path) {
    if (path[0] == sep) return true;
    // Windows absolute path: "C:/..." or "C:\..."
    return path[0] && path[1] == ':' && (path[2] == '/' || path[2] == '\\');
}

static char *join_path(ZState *state, const char *dir, const char *module, const char *suffix) {
    usize len = strlen(dir) + 1 + strlen(module) + strlen(suffix) + 1;
    char *out = znalloc(state->allocator, char, len);
    if (*dir)   snprintf(out, len, "%s%c%s%s", dir, sep, module, suffix);
    else        snprintf(out, len, "%s%s", module, suffix);
    return out;
}

static char *packages_dir(ZState *state) {
    return join_path(state, state->homePath ? state->homePath : "", ".zinc", "/packages");
}

/* Tries <dir>/<module>/lib.zn, then <dir>/<module>.zn. */
static char *resolve_in(ZState *state, const char *dir, const char *module) {
    char *path = join_path(state, dir, module, "/" ENTRY_IMPORT_FILE ".zn");
    if (file_exists(path)) return path;

    path = join_path(state, dir, module, ".zn");
    if (file_exists(path)) return path;
    return NULL;
}

/* Resolves an import string (e.g. "std/list") to a file path.
 * The lookup is relative to the importing file first, then the package directory:
 *   1. <dir>/<module>/lib.zn
 *   2. <dir>/<module>.zn
 *   3. ~/.zinc/packages/<module>/lib.zn or ~/.zinc/packages/<module>.zn
 * It returns null if none of them exists.
 * */
char *resolve_import(ZState *state, const char *filename, const char *module) {
    if (is_absolute_path(module)) return resolve_in(state, "", module);

    char *dir = ".";
    if (filename) {
        dir = dirname(zstrdup(state->allocator, (char *)filename));
    }

    char *path = resolve_in(state, dir, module);
    if (path) return path;

    return resolve_in(state, packages_dir(state), module);
}
