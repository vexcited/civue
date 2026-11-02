#ifndef CIVUE_INTERNAL_H
#define CIVUE_INTERNAL_H

#include <civue/base.h>

#include <cilo/cilo.h>
#include <stdio.h>
#include <stdlib.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef float f32;
typedef double f64;
typedef size_t usize;

#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ========================================================================================================== */
/* diagnostics and memory. what the app may free comes from malloc, what civue keeps to itself from cilo.     */
/* ========================================================================================================== */

[[noreturn]] void civue__die(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void civue__log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define die civue__die
#define LOG civue__log
#define now_s civue_now

void *civue__xmalloc(usize n);
void *civue__xcalloc(usize n, usize size);
void *civue__xrealloc(void *p, usize n);
char *civue__xstrndup(const char *s, usize n);
char *civue__xfmt(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define xmalloc civue__xmalloc
#define xcalloc civue__xcalloc
#define xrealloc civue__xrealloc
#define xstrndup civue__xstrndup
#define xfmt civue__xfmt
static inline char *xstrdup(const char *s) { return civue__xstrndup(s, strlen(s)); }

// zeroed memory civue allocates and frees itself.
static inline void *own_new(usize size) { return cilo_calloc(1, size); }
static inline void own_free(void *p) { cilo_mfree(p); }

/* ========================================================================================================== */
/* growable arrays, the layout of CIVUE_VEC, grown with realloc.                                              */
/* ========================================================================================================== */

#define Vec(T) CIVUE_VEC(T)
void civue__vec_grow(void **v, u32 *cap, u32 need, usize item);
#define vec_reserve(a, n) \
  ((a)->len + (n) > (a)->cap ? civue__vec_grow((void **)&(a)->v, &(a)->cap, (a)->len + (n), sizeof(*(a)->v)) : (void)0)
#define vec_push(a, x) (vec_reserve(a, 1), (a)->v[(a)->len++] = (x))
#define vec_add(a) (vec_reserve(a, 1), memset(&(a)->v[(a)->len], 0, sizeof(*(a)->v)), &(a)->v[(a)->len++])
#define vec_extend(a, src, n) \
  ((n) ? (vec_reserve(a, n), memcpy(&(a)->v[(a)->len], src, sizeof(*(a)->v) * (n)), (a)->len += (n)) : 0)
#define vec_pop(a) ((a)->v[--(a)->len])
#define vec_last(a) ((a)->v[(a)->len - 1])
#define vec_free(a) (free((a)->v), (a)->v = nullptr, (a)->len = (a)->cap = 0)
#define vec_clear(a) ((a)->len = 0)
#define vec_each(a, it) for (typeof_unqual((a)->v) it = (a)->v; it && it < (a)->v + (a)->len; it++)
#define vec_remove(a, i) (memmove(&(a)->v[i], &(a)->v[(i) + 1], sizeof(*(a)->v) * ((a)->len - (i) - 1)), (a)->len--)
#define vec_insert(a, i, x)                                                                                       \
  (vec_reserve(a, 1), memmove(&(a)->v[(i) + 1], &(a)->v[i], sizeof(*(a)->v) * ((a)->len - (i))), (a)->v[i] = (x), \
   (a)->len++)

void civue__buf_put(civue_bytes *b, const void *p, usize n);
void civue__buf_printf(civue_bytes *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *civue__buf_take(civue_bytes *b);
#define buf_put civue__buf_put
#define buf_printf civue__buf_printf
#define buf_take civue__buf_take
static inline void buf_puts(civue_bytes *b, const char *s) { civue__buf_put(b, s, strlen(s)); }
static inline void buf_putc(civue_bytes *b, char c) { vec_push(b, (u8)c); }

/* ========================================================================================================== */
/* maps, hashing and text.                                                                                    */
/* ========================================================================================================== */

bool civue__map_get(const civue_map *m, u64 key, u64 *val);
void civue__map_put(civue_map *m, u64 key, u64 val);
bool civue__map_del(civue_map *m, u64 key);
void civue__map_free(civue_map *m);
void civue__map_clear(civue_map *m);
#define map_get civue__map_get
#define map_put civue__map_put
#define map_del civue__map_del
#define map_free civue__map_free
#define map_clear civue__map_clear
#define map_each(m, i)               \
  for (u32 i = 0; i < (m)->cap; i++) \
    if ((m)->slots[i].key)

#define hash_u64 civue_hash_u64
#define hash_bytes civue_hash
#define hash_str civue_hash_str
#define utf8_next civue_utf8_next
#define utf8_put civue_utf8_put

/* ========================================================================================================== */
/* geometry, the short names.                                                                                 */
/* ========================================================================================================== */

#define v2 civue_v2
#define v2_add civue_v2_add
#define v2_sub civue_v2_sub
#define v2_scale civue_v2_scale
#define rect(...) civue_rect_from(__VA_ARGS__)
#define rect_xywh civue_rect_xywh
#define rect_w civue_rect_w
#define rect_h civue_rect_h
#define rect_empty civue_rect_empty
#define rect_contains civue_rect_contains
#define rect_inflate civue_rect_inflate
#define rect_intersect civue_rect_intersect
#define rect_center civue_rect_center
#define color_hex civue_hex
#define color_alpha civue_alpha

/* ========================================================================================================== */
/* files.                                                                                                     */
/* ========================================================================================================== */

bool civue__file_read(const char *path, civue_bytes *out);
const char *civue__home_dir(void);
#define file_read civue__file_read
#define home_dir civue__home_dir
static inline char *path_join(const char *a, const char *b) { return civue__xfmt("%s/%s", a, b); }

#endif
