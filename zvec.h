#ifndef ZVEC_H
#define ZVEC_H

#include <stddef.h>
#include <stdlib.h>

#define vec(T)                                                                 \
  struct {                                                                     \
    size_t len;                                                                \
    size_t size;                                                               \
    T *ptr;                                                                    \
  }

#define vec_init(v, i)                                                         \
  do {                                                                         \
    (v)->len = 0;                                                              \
    (v)->size = (i);                                                           \
    (v)->ptr = malloc(sizeof(*(v)->ptr) * (i));                                \
  } while (0)

#define vec_push(v, e)                                                         \
  do {                                                                         \
    if (!(v)->ptr)                                                             \
      break;                                                                   \
    if ((v)->len >= (v)->size) {                                               \
      size_t new_size = ((v)->size == 0) ? 1 : (v)->size << 1;                 \
      void *tmp = realloc((v)->ptr, new_size * sizeof(*(v)->ptr));             \
      if (!tmp)                                                                \
        break;                                                                 \
      (v)->ptr = tmp;                                                          \
      (v)->size = new_size;                                                    \
    }                                                                          \
    (v)->ptr[(v)->len++] = (e);                                                \
  } while (0)

#define vec_pop(v) ({ (v)->len > 0 ? (v)->ptr[--(v)->len] : (v)->ptr[0]; })

#endif
