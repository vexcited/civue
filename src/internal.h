#ifndef CIVUE_INTERNAL_H
#define CIVUE_INTERNAL_H

#ifdef _WIN32
#include "windows/posix.h"
#endif

#include <civue/base.h>

#include <cilo/cilo.h>
#include <stdcountof.h>
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

// foundation has its own.
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define CLAMP(x, lo, hi) ((x) < (lo) ? (lo) : (x) > (hi) ? (hi) : (x))
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)

/* ========================================================================== */
/* diagnostics and memory. what the app may free comes from malloc, what      */
/* civue keeps to itself from cilo.                                           */
/* ========================================================================== */

[[noreturn, gnu::cold, gnu::format(printf, 1, 2)]]
void civue__die(const char *fmt, ...);
[[gnu::cold, gnu::format(printf, 1, 2)]] void civue__log(const char *fmt, ...);
#define die civue__die
#define LOG civue__log
#define now_s civue_now

[[nodiscard, gnu::malloc, gnu::alloc_size(1), gnu::returns_nonnull]]
void *civue__xmalloc(usize n);
[[nodiscard, gnu::malloc, gnu::alloc_size(1, 2), gnu::returns_nonnull]]
void *civue__xcalloc(usize n, usize size);
[[nodiscard, gnu::alloc_size(2), gnu::returns_nonnull]]
void *civue__xrealloc(void *p, usize n);
[[nodiscard, gnu::malloc, gnu::returns_nonnull]]
char *civue__xstrndup(const char *s, usize n);
[[nodiscard, gnu::malloc, gnu::returns_nonnull, gnu::format(printf, 1, 2)]]
char *civue__xfmt(const char *fmt, ...);
#define xmalloc civue__xmalloc
#define xcalloc civue__xcalloc
#define xrealloc civue__xrealloc
#define xstrndup civue__xstrndup
#define xfmt civue__xfmt

static inline char *xstrdup(const char *s) {
  return civue__xstrndup(s, strlen(s));
}

// zeroed memory civue allocates and frees itself.
static inline void *own_new(usize size) {
  return cilo_calloc(1, size);
}

static inline void own_free(void *p) {
  cilo_mfree(p);
}

/* ========================================================================== */
/* growable arrays, the layout of CIVUE_VEC, grown with realloc.              */
/* ========================================================================== */

#define Vec(T) CIVUE_VEC(T)
void civue__vec_grow(void **v, u32 *cap, u32 need, usize item);
#define vec_reserve(a, n)                                           \
  (unlikely((a)->len + (n) > (a)->cap)                              \
     ? civue__vec_grow((void **)&(a)->v, &(a)->cap, (a)->len + (n), \
                       sizeof(*(a)->v))                             \
     : (void)0)
#define vec_push(a, x) (vec_reserve(a, 1), (a)->v[(a)->len++] = (x))
#define vec_add(a)                                                   \
  (vec_reserve(a, 1), memset(&(a)->v[(a)->len], 0, sizeof(*(a)->v)), \
   &(a)->v[(a)->len++])
#define vec_extend(a, src, n)                                    \
  ((n) ? (vec_reserve(a, n),                                     \
          memcpy(&(a)->v[(a)->len], src, sizeof(*(a)->v) * (n)), \
          (a)->len += (n))                                       \
       : 0)
#define vec_pop(a) ((a)->v[--(a)->len])
#define vec_last(a) ((a)->v[(a)->len - 1])
#define vec_free(a) (free((a)->v), (a)->v = nullptr, (a)->len = (a)->cap = 0)
#define vec_clear(a) ((a)->len = 0)
#define vec_each(a, it) \
  for (typeof_unqual((a)->v) it = (a)->v; it && it < (a)->v + (a)->len; it++)
#define vec_remove(a, i)                            \
  (memmove(&(a)->v[i], &(a)->v[(i) + 1],            \
           sizeof(*(a)->v) * ((a)->len - (i) - 1)), \
   (a)->len--)
#define vec_insert(a, i, x)                                                   \
  (vec_reserve(a, 1),                                                         \
   memmove(&(a)->v[(i) + 1], &(a)->v[i], sizeof(*(a)->v) * ((a)->len - (i))), \
   (a)->v[i] = (x), (a)->len++)

void civue__buf_put(civue_bytes *b, const void *p, usize n);
[[gnu::format(printf, 2, 3)]] void civue__buf_printf(civue_bytes *b,
                                                     const char *fmt, ...);
[[nodiscard, gnu::returns_nonnull]] char *civue__buf_take(civue_bytes *b);
#define buf_put civue__buf_put
#define buf_printf civue__buf_printf
#define buf_take civue__buf_take

static inline void buf_puts(civue_bytes *b, const char *s) {
  civue__buf_put(b, s, strlen(s));
}

static inline void buf_putc(civue_bytes *b, char c) {
  vec_push(b, (u8)c);
}

// decodes png, jpeg, webp or the first frame of a gif into premultiplied
// rgba8, rows from the top, its longest side `max_pixels` at most when that
// is not 0. malloc'd, null when the bytes are no image. from any thread.
u8 *civue__image_decode(const u8 *bytes, usize n, u32 max_pixels, u32 *width,
                        u32 *height);

typedef struct {
  // every frame one after the other, each `width` by `height`, like
  // civue__image_decode's pixels.
  u8 *rgba;
  // the seconds each frame shows for.
  f32 *delays;
  u32 width, height, count;
} civue__frames;

// every frame of an animated gif, webp or png, decoded smaller than
// `max_pixels` when all of them would take more than `max_bytes`. false for
// a still image, and where the system cannot tell the frames apart.
bool civue__image_decode_frames(const u8 *bytes, usize n, u32 max_pixels,
                                usize max_bytes, civue__frames *out);

// the word around unit `at` of the `n` utf-16 units at `s`, all of a script
// written without spaces, as the system's dictionaries cut it. false where
// it has none.
bool civue__word_at(const u16 *s, u32 n, u32 at, u32 *from, u32 *to);

/* ========================================================================== */
/* maps, hashing and text.                                                    */
/* ========================================================================== */

static inline bool map_get(const civue_map *m, u64 key, u64 *val) {
  const u64 *v = CILO_MAP_GET(m, key);
  if (v && val) {
    *val = *v;
  }
  return v;
}

static inline void map_put(civue_map *m, u64 key, u64 val) {
  *CILO_MAP_PUT(&cilo_heap, m, key) = val;
}

static inline bool map_del(civue_map *m, u64 key) {
  return CILO_MAP_DEL(m, key);
}

static inline void map_free(civue_map *m) {
  CILO_MAP_DROP(&cilo_heap, m);
}

static inline void map_clear(civue_map *m) {
  CILO_MAP_CLEAR(m);
}

#define hash_u64 civue_hash_u64
#define hash_bytes civue_hash
#define hash_str civue_hash_str
#define utf8_next civue_utf8_next
#define utf8_put civue_utf8_put

/* ========================================================================== */
/* geometry, the short names.                                                 */
/* ========================================================================== */

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

typedef struct civue_ui civue_ui;
typedef struct civue_tree civue_tree;

// how many times the coverage and the color atlases started over.
void civue__generations(const civue_ui *ui, u64 out[2]);

// the nodes' solve as a frame ends, true when nothing moved.
bool civue__tree_end(civue_ui *ui);
void civue__tree_free(civue_tree *t);

// names the app after its window's title for the gpu's drivers, unless the app
// named itself.
void civue__gpu_title(const char *title);

/* ========================================================================== */
/* files.                                                                     */
/* ========================================================================== */

bool civue__file_read(const char *path, civue_bytes *out);
const char *civue__home_dir();
#define file_read civue__file_read
#define home_dir civue__home_dir

static inline char *path_join(const char *a, const char *b) {
  return civue__xfmt("%s/%s", a, b);
}

#endif
