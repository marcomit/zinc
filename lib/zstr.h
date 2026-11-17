/**
 * @file zstr.h
 * @brief String library on top of zvec.h
 *
 * It has the string type whic is a vector of char `vec(char)`
 * It provides also a string-view called `zsv` which is a read-only string.
 *
 * @copyright Copyright (c) 2025, Marco Menegazzi
 *            SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef ZSTR_H
#define ZSTR_H

#include <ctype.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "zvec.h"

typedef vec(char) zstr;

typedef struct {
    size_t len;
    const char *ptr;
} zsv;

#define ZSV(s) ((zsv){ strlen(s), s })

static inline void zs_reserve(zstr *s, size_t n);
static inline size_t zs_len(zstr s);

static inline zsv zsv_new(const char *s, size_t n) {
    return (zsv){ n, s };
}


/**
 * @brief frees the owned string.
 * If the string has an allocator it uses that to free itself.
 * */
static inline void zs_free(zstr s) {
    vecfree(s);
}

/**
 * @brief Creates a `zstr` from a string with `n` bytes
 * */
static inline zstr zs_newn(const char *c_str, size_t n) {
    zstr s = NULL;
    vecreserve(s, n + 1);
    if (!s) return NULL;
    vecsetlen(s, n);
    memcpy(s, c_str, n);
    s[n] = '\0';
    return s;
}

/**
 * @brief Creates a `zstr` from a null-terminated string.
 * */
static inline zstr zs_new(const char *c_str) {
    return zs_newn(c_str, strlen(c_str));
}

static inline zstr zs_from_sv(zsv sv) {
    return zs_newn(sv.ptr ? sv.ptr : "", sv.ptr ? sv.len : 0);
}

/**
 * @brief Creates the string given an allocator.
 * Strings use zvec which stores the allocator in the header.
 * So every push to the string uses always the same allocator
 * */
static inline zstr zs_with(Allocator *a) {
    zstr s = NULL;
    vecpushwith(s, '\0', a);
    if (!s) return NULL;
    vecsetlen(s, 0);
    return s;
}

static inline zstr zs_vfmt(const char *fmt, va_list ap) {
    zstr res = NULL;

    va_list copy;
    va_copy(copy, ap);
    int len = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);

    if (len < 0) return NULL;

    zs_reserve(&res, (size_t)len);
    if (!res) return NULL;
    vsnprintf(res, (size_t)len + 1, fmt, ap);
    vecsetlen(res, (size_t)len);
    return res;
}

static inline zstr zs_fmt(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    zstr res = zs_vfmt(fmt, ap);
    va_end(ap);
    return res;
}

/**
 * @brief Duplicates the `src` string using the same allocator.
 * */
static inline zstr zs_dup(zstr src) {
    if (!src) return NULL;
    zstr zdup = NULL;
    size_t src_len = veclen(src);
    vecreservewith(zdup, src_len + 1, VEC_METADATA(src)->allocator);
    vecsetlen(zdup, src_len);
    memcpy(zdup, src, src_len);
    zdup[src_len] = '\0';
    return zdup;
}

/**
 * @brief Reserves `n` slots in the string.
 * It takes a pointer to the string because
 * reserving bytes should hit a reallocation.
 * */
static inline void zs_reserve(zstr *s, size_t n) {
    size_t need = n + 1;
    if (!*s) {
        vecreserve(*s, need);
    } else if (veccap(*s) < need) {
        size_t cap = veccap(*s) * 2;
        vecreserve(*s, cap < need ? need : cap);
    }
    if (*s) (*s)[zs_len(*s)] = '\0';
}

static inline size_t zs_len(zstr s) {
    return veclen(s);
}

/**
 * @brief Returns true if the string `s` is empty.
 * */
static inline bool zs_empty(zstr s) {
    return veclen(s) == 0;
}

static inline void zs_truncate(zstr s, size_t n) {
    if (n >= zs_len(s)) return;
    vecsetlen(s, n);
    s[n] = '\0'; // this is safe since n is garanteed to be smaller than len(s)
}

/**
 * @brief Clears the string setting the length to 0.
 * */
static inline void zs_clear(zstr s) {
    if (!s) return;
    if (!zs_empty(s)) s[0] = '\0';
    vecsetlen(s, 0);
}

/**
 * @brief Appends the first `c_len` characters of `c_str` to the string
 * */
static inline void zs_catn(zstr *s, const char *c_str, size_t c_len) {
    size_t s_len = zs_len(*s), new_len = s_len + c_len;
    uintptr_t base = (uintptr_t)*s, src = (uintptr_t)c_str;
    bool aliased = *s && src >= base && src <= base + s_len;
    size_t offset = aliased ? (size_t)(src - base) : 0;

    zs_reserve(s, new_len);
    if (!*s) return;
    if (aliased) c_str = *s + offset;

    memmove(*s + s_len, c_str, c_len);
    vecsetlen(*s, new_len);
    (*s)[new_len] = '\0';
}

/**
 * @brief Appends a null-terminated string `c_str` to the `zstr` s.
 * */
static inline void zs_cat(zstr *s, const char *c_str) {
    zs_catn(s, c_str, strlen(c_str));
}

/**
 * @brief Appends a format-string to the `zstr` s
 * */
static inline void zs_catf(zstr *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    zstr formatted = zs_vfmt(fmt, ap);
    va_end(ap);
    zs_cat(s, formatted);
    zs_free(formatted);
}

static inline void zs_catsv(zstr *s, zsv sv) {
    zs_catn(s, sv.ptr ? sv.ptr : "", sv.ptr ? sv.len : 0);
}

/**
 * @brief Pushes a character in the string
 * */
static inline void zs_push(zstr *s, char c) {
    size_t s_len = zs_len(*s);
    zs_reserve(s, s_len + 1);
    if (!*s) return;
    (*s)[s_len] = c;
    vecsetlen(*s, s_len + 1);
    (*s)[s_len + 1] = '\0';
}

/**
 * @brief Inserts `content` at the `at` character
 * */
static inline void zs_insert(zstr *s, size_t at, const char *content) {
    size_t s_len = zs_len(*s), c_len = strlen(content);
    if (at > s_len) at = s_len;

    zs_reserve(s, s_len + c_len);
    if (!*s) return;

    memmove(*s + at + c_len, *s + at, s_len - at);
    memcpy(*s + at, content, c_len);
    vecsetlen(*s, s_len + c_len);
    (*s)[s_len + c_len] = '\0';
}

static inline void zs_erase(zstr *s, size_t at, size_t n) {
    if (!s || !*s) return;
    size_t s_len = zs_len(*s);
    if (at >= s_len) return;
    if (n > s_len - at) n = s_len - at;

    memmove(*s + at, *s + at + n, s_len - at - n);
    vecsetlen(*s, s_len - n);
    (*s)[s_len - n] = '\0';
}

static inline const char *zs__memfind(const char *hay, size_t hay_len,
                                      const char *needle, size_t needle_len) {
    if (needle_len == 0) return hay;
    while (hay_len >= needle_len) {
        const char *p = memchr(hay, needle[0], hay_len - needle_len + 1);
        if (!p) return NULL;
        if (memcmp(p, needle, needle_len) == 0) return p;
        hay_len -= (size_t)(p - hay) + 1;
        hay = p + 1;
    }
    return NULL;
}

static inline size_t zs_replace(zstr *s, const char *from, const char *to) {
    if (!s || !*s || !from || !*from) return 0;
    if (!to) to = "";

    size_t s_len = zs_len(*s), from_len = strlen(from), to_len = strlen(to), count = 0;
    const char *p = *s, *end = *s + s_len, *hit;

    while ((hit = zs__memfind(p, (size_t)(end - p), from, from_len))) {
        count++;
        p = hit + from_len;
    }
    if (count == 0) return 0;

    zstr out = NULL;
    vecreservewith(out, s_len - count * from_len + count * to_len + 1, VEC_METADATA(*s)->allocator);
    if (!out) return 0;

    p = *s;
    while ((hit = zs__memfind(p, (size_t)(end - p), from, from_len))) {
        zs_catn(&out, p, (size_t)(hit - p));
        zs_catn(&out, to, to_len);
        p = hit + from_len;
    }
    zs_catn(&out, p, (size_t)(end - p));

    zs_free(*s);
    *s = out;
    return count;
}

static inline void zs_upper(zstr s) {
    for (size_t i = 0; i < zs_len(s); i++)
        s[i] = (char)toupper((unsigned char)s[i]);
}

static inline void zs_lower(zstr s) {
    for (size_t i = 0; i < zs_len(s); i++)
        s[i] = (char)tolower((unsigned char)s[i]);
}

static inline bool zs_starts(const char *s, const char *prefix) {
    if (!s || !prefix) return false;
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static inline bool zs_ends(const char *s, const char *suffix) {
    if (!s || !suffix) return false;
    size_t s_len = strlen(s), suffix_len = strlen(suffix);
    if (suffix_len > s_len) return false;
    return memcmp(s + s_len - suffix_len, suffix, suffix_len) == 0;
}

static inline bool zs_contains(const char *s, const char *content) {
    if (!s || !content) return false;
    return strstr(s, content) != NULL;
}

static inline ptrdiff_t zs_find(const char *s, const char *needle) {
    if (!s || !needle) return -1;
    const char *p = strstr(s, needle);
    return p ? p - s : -1;
}

static inline ptrdiff_t zs_rfind(const char *s, const char *needle) {
    if (!s || !needle) return -1;
    size_t s_len = strlen(s), needle_len = strlen(needle);
    if (needle_len > s_len) return -1;
    for (size_t i = s_len - needle_len + 1; i-- > 0;)
        if (memcmp(s + i, needle, needle_len) == 0) return (ptrdiff_t)i;
    return -1;
}

static inline ptrdiff_t zs_find_char(const char *s, char c) {
    if (!s || c == '\0') return -1;
    const char *p = strchr(s, c);
    return p ? p - s : -1;
}

static inline ptrdiff_t zs_rfind_char(const char *s, char c) {
    if (!s || c == '\0') return -1;
    const char *p = strrchr(s, c);
    return p ? p - s : -1;
}

static inline size_t zs_count(const char *s, const char *needle) {
    if (!s || !needle || !*needle) return 0;
    size_t n = 0, needle_len = strlen(needle);
    for (const char *p = s; (p = strstr(p, needle)); p += needle_len) n++;
    return n;
}

static inline bool zs__has_in_set(const char c, const char *set) {
    if (set) for (; *set; set++) if (*set == c) return true;
    return false;
}

static inline size_t zs_trim_left(zstr s, const char *set) {
    if (!s) return 0;
    size_t s_len = zs_len(s), i = 0;
    while (i < s_len && zs__has_in_set(s[i], set)) i++;
    if (i == 0) return 0;
    memmove(s, s + i, s_len - i);
    vecsetlen(s, s_len - i);
    s[s_len - i] = '\0';
    return i;
}

static inline size_t zs_trim_right(zstr s, const char *set) {
    if (!s) return 0;
    size_t s_len = zs_len(s), n = s_len;
    while (n > 0 && zs__has_in_set(s[n - 1], set)) n--;
    vecsetlen(s, n);
    s[n] = '\0';
    return s_len - n;
}

static inline size_t zs_trim(zstr s, const char *set) {
    size_t right = zs_trim_right(s, set);
    return right + zs_trim_left(s, set);
}

static inline zsv zsv_from(const char *s, size_t n) {
    return (zsv){ n, s };
}

static inline zsv zs_to_zsv(zstr s) {
    return zsv_from(s, zs_len(s));
}

static inline bool zs_eq(const char *a, const char *b) {
    if (!a && !b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static inline bool zsv_eq(zsv a, zsv b) {
    if (a.len != b.len) return false;
    return a.len == 0 || memcmp(a.ptr, b.ptr, a.len) == 0;
}

static inline int zsv_cmp(zsv a, zsv b) {
    size_t n = a.len < b.len ? a.len : b.len;
    int c = n ? memcmp(a.ptr, b.ptr, n) : 0;
    if (c) return c;
    return (a.len > b.len) - (a.len < b.len);
}

static inline zsv zsv_sub(zsv sv, size_t start, size_t end) {
    if (end > sv.len) end = sv.len;
    if (start > end) start = end;
    return (zsv){ end - start, sv.ptr ? sv.ptr + start : NULL };
}

static inline zsv zsv_start(zsv sv, size_t start) {
    return zsv_sub(sv, start, sv.len);
}

static inline zsv zsv_end(zsv sv, size_t end) {
    return zsv_sub(sv, 0, end);
}

static inline zsv zsv_trim_left(zsv sv, const char *set) {
    if (!sv.ptr) return (zsv){ 0, NULL };
    while (sv.len > 0 && zs__has_in_set(*sv.ptr, set)) {
        sv.ptr++;
        sv.len--;
    }
    return sv;
}

static inline zsv zsv_trim_right(zsv sv, const char *set) {
    if (!sv.ptr) return (zsv){ 0,  NULL };
    while (sv.len > 0 && zs__has_in_set(sv.ptr[sv.len - 1], set)) sv.len--;
    return sv;
}

static inline zsv zsv_trim(zsv sv, const char *set) {
    return zsv_trim_right(zsv_trim_left(sv, set), set);
}

static inline bool zsv_next(zsv *src, const char *delim, zsv *out) {
    if (!src || !src->ptr || !out) return false;
    size_t delim_len = delim ? strlen(delim) : 0;

    for (size_t i = 0; delim_len && i + delim_len <= src->len; i++) {
        if (memcmp(src->ptr + i, delim, delim_len) == 0) {
            *out = (zsv){ i, src->ptr };
            src->ptr += i + delim_len;
            src->len -= i + delim_len;
            return true;
        }
    }

    *out = *src;
    *src = (zsv){ 0, NULL };
    return true;
}

static inline bool zsv_chop(zsv *src, char c, zsv *head) {
    if (!src || !src->ptr || src->len == 0 || !head) return false;
    const char *end = memchr(src->ptr, c, src->len);

    if (!end) {
        *head = *src;
        src->ptr += src->len;
        src->len = 0;
        return true;
    }

    *head = (zsv){ (size_t)(end - src->ptr), src->ptr };
    src->len -= head->len + 1;
    src->ptr = end + 1;
    return true;
}

static inline vec(zstr) zs_split(const char *s, const char *delim) {
    if (!s) return NULL;
    zsv cur = ZSV(s), tok;
    vec(zstr) splitted = NULL;

    while (zsv_next(&cur, delim, &tok))
        vecpush(splitted, zs_from_sv(tok));

    return splitted;
}

static inline vec(zstr) zs_split_any(const char *src, const char *delim) {
    vec(zstr) out = NULL;
    if (!src) return NULL;
    if (!delim) delim = "";
    while(*src) {
        src += strspn(src, delim);
        size_t len = strcspn(src, delim);
        if (len == 0) break;
        vecpush(out, zs_newn(src, len));
        src += len;
    }
    return out;
}

static inline vec(zstr) zs_lines(const char *s) {
    if (!s) return NULL;
    zsv src = ZSV(s), line;
    vec(zstr) out = NULL;

    while (zsv_chop(&src, '\n', &line)) {
        if (line.len > 0 && line.ptr[line.len - 1] == '\r') line.len--;
        vecpush(out, zs_from_sv(line));
    }

    return out;
}

static inline zstr zs_join(const char **parts, size_t n, const char *sep) {
    zstr out = zs_new("");
    if (!sep) sep = "";
    for (size_t i = 0; i < n; i++) {
        if (i != 0) zs_cat(&out, sep);
        if (parts[i]) zs_cat(&out, parts[i]);
    }
    return out;
}

static inline void zs_free_all(vec(zstr) s) {
    for (size_t i = 0; i < veclen(s); i++)
        if (s[i]) zs_free(s[i]);
    vecfree(s);
}

static inline uint64_t zs_hash(zsv sv) {
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < sv.len; i++) {
        h ^= (unsigned char)sv.ptr[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

#endif
