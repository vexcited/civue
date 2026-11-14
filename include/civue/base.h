#ifndef CIVUE_BASE_H
#define CIVUE_BASE_H

#include <math.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <cilo/map.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================================================
 */
/* values. positions and sizes are logical points as doubles, colors straight
 * alpha floats from 0 to 1.       */
/* ==========================================================================================================
 */

// an app whose own types have the same members defines CIVUE_VEC2, CIVUE_RECT,
// CIVUE_COLOR, CIVUE_BYTES, CIVUE_U32S or CIVUE_STRS as them before including
// civue, and passes its values straight through.
#ifdef CIVUE_VEC2
typedef CIVUE_VEC2 civue_vec2;
#else
typedef struct {
  double x, y;
} civue_vec2;
#endif

// a rectangle by its edges, x0 and y0 its top left.
#ifdef CIVUE_RECT
typedef CIVUE_RECT civue_rect;
#else
typedef struct {
  double x0, y0, x1, y1;
} civue_rect;
#endif

#ifdef CIVUE_COLOR
typedef CIVUE_COLOR civue_color;
#else
typedef struct {
  float r, g, b, a;
} civue_color;
#endif

// a growable array, items `v`, grown with realloc so the app may take it over.
// zero is an empty array.
#define CIVUE_VEC(T)   \
  struct {             \
    T *v;              \
    uint32_t len, cap; \
  }

#ifdef CIVUE_BYTES
typedef CIVUE_BYTES civue_bytes;
#else
typedef CIVUE_VEC(uint8_t) civue_bytes;
#endif
#ifdef CIVUE_U32S
typedef CIVUE_U32S civue_u32s;
#else
typedef CIVUE_VEC(uint32_t) civue_u32s;
#endif
// malloc'd strings, owned by whoever holds the array.
#ifdef CIVUE_STRS
typedef CIVUE_STRS civue_strs;
#else
typedef CIVUE_VEC(char *) civue_strs;
#endif

// a map from keys to values, cilo's.
typedef CILO_MAP(uint64_t, uint64_t) civue_map;

static constexpr civue_color CIVUE_BLACK = {0, 0, 0, 1};
static constexpr civue_color CIVUE_WHITE = {1, 1, 1, 1};
static constexpr civue_color CIVUE_CLEAR = {0, 0, 0, 0};

/* ==========================================================================================================
 */
/* geometry and colors. */
/* ==========================================================================================================
 */

static inline civue_vec2 civue_v2(double x, double y) {
  return (civue_vec2){x, y};
}

static inline civue_vec2 civue_v2_add(civue_vec2 a, civue_vec2 b) {
  return (civue_vec2){a.x + b.x, a.y + b.y};
}

static inline civue_vec2 civue_v2_sub(civue_vec2 a, civue_vec2 b) {
  return (civue_vec2){a.x - b.x, a.y - b.y};
}

static inline civue_vec2 civue_v2_scale(civue_vec2 a, double s) {
  return (civue_vec2){a.x * s, a.y * s};
}

static inline civue_rect civue_rect_from(double x0, double y0, double x1,
                                         double y1) {
  return (civue_rect){x0, y0, x1, y1};
}

static inline civue_rect civue_rect_xywh(double x, double y, double w,
                                         double h) {
  return (civue_rect){x, y, x + w, y + h};
}

static inline double civue_rect_w(civue_rect r) {
  return r.x1 - r.x0;
}

static inline double civue_rect_h(civue_rect r) {
  return r.y1 - r.y0;
}

static inline bool civue_rect_empty(civue_rect r) {
  return !(r.x1 > r.x0 && r.y1 > r.y0);
}

static inline bool civue_rect_contains(civue_rect r, civue_vec2 p) {
  return p.x >= r.x0 && p.x < r.x1 && p.y >= r.y0 && p.y < r.y1;
}

static inline civue_rect civue_rect_inflate(civue_rect r, double dx,
                                            double dy) {
  return (civue_rect){r.x0 - dx, r.y0 - dy, r.x1 + dx, r.y1 + dy};
}

static inline civue_rect civue_rect_intersect(civue_rect a, civue_rect b) {
  return (civue_rect){fmax(a.x0, b.x0), fmax(a.y0, b.y0), fmin(a.x1, b.x1),
                      fmin(a.y1, b.y1)};
}

static inline civue_vec2 civue_rect_center(civue_rect r) {
  return (civue_vec2){(r.x0 + r.x1) * 0.5, (r.y0 + r.y1) * 0.5};
}

// a color from 0xrrggbb.
static inline civue_color civue_hex(uint32_t rgb) {
  return (civue_color){((rgb >> 16) & 255) / 255.0f,
                       ((rgb >> 8) & 255) / 255.0f, (rgb & 255) / 255.0f, 1};
}

// a color from 0xrrggbbaa.
static inline civue_color civue_hexa(uint32_t rgba) {
  return (civue_color){(rgba >> 24) / 255.0f, ((rgba >> 16) & 255) / 255.0f,
                       ((rgba >> 8) & 255) / 255.0f, (rgba & 255) / 255.0f};
}

static inline civue_color civue_alpha(civue_color c, float a) {
  return (civue_color){c.r, c.g, c.b, c.a * a};
}

/* ==========================================================================================================
 */
/* hashing, text and time. */
/* ==========================================================================================================
 */

static inline uint64_t civue_hash_u64(uint64_t x) {
  x ^= x >> 33;
  x *= 0xff51afd7ed558ccdull;
  x ^= x >> 33;
  x *= 0xc4ceb9fe1a85ec53ull;
  x ^= x >> 33;
  return x;
}

// cilo's hash, for this process only: it may change between versions.
static inline uint64_t civue_hash(const void *p, size_t n) {
  return cilo_hash(p, n);
}

static inline uint64_t civue_hash_str(const char *s) {
  return cilo_hash_str(s);
}

// the code point at s[*i], *i moved past it. invalid bytes read as u+fffd.
uint32_t civue_utf8_next(const char *s, size_t n, size_t *i);
// the utf-8 of `cp` into `out`, its byte count returned.
size_t civue_utf8_put(char out[4], uint32_t cp);

// monotonic seconds, the clock frames and animations run on.
double civue_now(void);

// where civue's messages go, stderr when none is set.
void civue_set_log(void (*log)(const char *message));

#ifdef __cplusplus
}
#endif

#endif
