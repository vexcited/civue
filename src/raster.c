#include "internal.h"

#include <civue/raster.h>

#include <math.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

// sse2 is every x86-64's, and what a build for one processor has past it,
// avx2 for one, is used where it is there.
#ifdef __SSE2__
#include <immintrin.h>
#endif

// ---- damage ----

static bool empty(civue_box b) {
  return b.x0 >= b.x1 || b.y0 >= b.y1;
}

static i64 area(civue_box b) {
  return empty(b) ? 0 : (i64)(b.x1 - b.x0) * (b.y1 - b.y0);
}

static civue_box cut(civue_box a, civue_box b) {
  return (civue_box){MAX(a.x0, b.x0), MAX(a.y0, b.y0), MIN(a.x1, b.x1),
                     MIN(a.y1, b.y1)};
}

// tiles across a row, and rows of them.
constexpr u32 ACROSS = 128;
constexpr u32 DOWN = CIVUE_DAMAGE_ROWS;

void civue_damage_begin(civue_damage *d, u32 width, u32 height) {
  // a tile is a cache line wide at least, and as low as text is cut fine.
  *d = (civue_damage){.across = 4, .down = 3};

  while (width && (width - 1) >> d->across >= ACROSS) {
    d->across++;
  }

  while (height && (height - 1) >> d->down >= DOWN) {
    d->down++;
  }
}

// the tiles from `first` to `last` of a row.
static void columns(u32 first, u32 last, u64 out[2]) {
  for (u32 w = 0; w < 2; w++) {
    u32 from = MAX(first, w * 64), to = MIN(last, w * 64 + 63);

    out[w] = 0;

    if (from <= to) {
      u32 n = to - from + 1;
      u64 bits = n == 64 ? ~0ull : (1ull << n) - 1;

      out[w] = bits << (from - w * 64);
    }
  }
}

// the rows and the tiles of each that `b` is over.
static void tiles(const civue_damage *d, civue_box b, u32 *first, u32 *last,
                  u64 across[2]) {
  u32 c0 = MIN((u32)MAX(b.x0, 0) >> d->across, ACROSS - 1);
  u32 c1 = MIN((u32)MAX(b.x1 - 1, 0) >> d->across, ACROSS - 1);

  *first = MIN((u32)MAX(b.y0, 0) >> d->down, DOWN - 1);
  *last = MIN((u32)MAX(b.y1 - 1, 0) >> d->down, DOWN - 1);
  columns(c0, c1, across);
}

void civue_damage_add(civue_damage *d, civue_box b) {
  if (d->all || empty(b)) {
    return;
  }

  u32 first, last;
  u64 across[2];
  tiles(d, b, &first, &last, across);

  for (u32 r = first; r <= last; r++) {
    d->rows[r][0] |= across[0];
    d->rows[r][1] |= across[1];
  }

  d->any = true;
}

static bool none(const civue_damage *d) {
  return !d->all && !d->any;
}

void civue_damage_join(civue_damage *d, const civue_damage *other) {
  if (d->all || none(other)) {
    return;
  }

  if (none(d)) {
    *d = *other;
    return;
  }

  // tiles of another size are another window's.
  if (other->all || other->across != d->across || other->down != d->down) {
    *d = (civue_damage){.all = true};
    return;
  }

  for (u32 r = 0; r < DOWN; r++) {
    d->rows[r][0] |= other->rows[r][0];
    d->rows[r][1] |= other->rows[r][1];
  }
}

void civue_damage_without(civue_damage *d, const civue_damage *other) {
  if (none(d) || none(other)) {
    return;
  }

  if (other->all) {
    *d = (civue_damage){};
    return;
  }

  // all of it but some tiles is not something tiles of no size can tell.
  if (d->all || other->across != d->across || other->down != d->down) {
    return;
  }

  u64 left = 0;

  for (u32 r = 0; r < DOWN; r++) {
    d->rows[r][0] &= ~other->rows[r][0];
    d->rows[r][1] &= ~other->rows[r][1];
    left |= d->rows[r][0] | d->rows[r][1];
  }

  d->any = left != 0;
}

uint64_t civue_damage_area(const civue_damage *d, u32 width, u32 height) {
  u64 whole = (u64)width * height;

  if (d->all) {
    return whole;
  }

  if (!d->any) {
    return 0;
  }

  u64 marked = 0;

  for (u32 r = 0; r < DOWN; r++) {
    marked += (u64)__builtin_popcountll(d->rows[r][0]) +
              (u64)__builtin_popcountll(d->rows[r][1]);
  }

  return MIN(marked << (d->across + d->down), whole);
}

static bool marked(const u64 row[2], u32 c) {
  return row[c / 64] >> (c % 64) & 1;
}

// the first run of marked tiles of a row from tile `from` on, false when
// there is none.
static bool run(const u64 row[2], u32 from, u32 *first, u32 *last) {
  u32 c = from;

  while (c < ACROSS && !marked(row, c)) {
    // a word with none is skipped whole.
    c += !(c % 64) && !row[c / 64] ? 64 : 1;
  }

  if (c >= ACROSS) {
    return false;
  }

  *first = c;

  while (c < ACROSS && marked(row, c)) {
    c++;
  }

  *last = c - 1;
  return true;
}

void civue_damage_each(const civue_damage *d, u32 width, u32 height,
                       void (*box)(void *ctx, civue_box b), void *ctx) {
  civue_box window = {0, 0, (i32)width, (i32)height};

  if (d->all) {
    box(ctx, window);
    return;
  }

  if (!d->any) {
    return;
  }

  for (u32 r = 0; r < DOWN; r++) {
    // rows marked alike are one box.
    u32 end = r;

    while (end + 1 < DOWN && d->rows[end + 1][0] == d->rows[r][0] &&
           d->rows[end + 1][1] == d->rows[r][1]) {
      end++;
    }

    u32 first, last;

    for (u32 from = 0; run(d->rows[r], from, &first, &last); from = last + 1) {
      civue_box b = {(i32)(first << d->across), (i32)(r << d->down),
                     (i32)((last + 1) << d->across),
                     (i32)((end + 1) << d->down)};

      b = cut(b, window);

      if (!empty(b)) {
        box(ctx, b);
      }
    }

    r = end;
  }
}

// ceilf of a value no farther from zero than a window is wide, without the
// call it is where the processor has no instruction for it.
static i32 up(f32 v) {
  i32 whole = (i32)v;

  return whole + ((f32)whole < v);
}

// the pixels whose centers are in x0 to x1 and y0 to y1, as the gpu fills
// them, inside `limit`.
static civue_box cover(f32 x0, f32 y0, f32 x1, f32 y1, civue_box limit) {
  f32 lx0 = (f32)limit.x0, ly0 = (f32)limit.y0;
  f32 lx1 = (f32)limit.x1, ly1 = (f32)limit.y1;
  // clamped first, a clip may be a billion pixels wide.
  x0 = fminf(fmaxf(x0, lx0), lx1), x1 = fminf(fmaxf(x1, lx0), lx1);
  y0 = fminf(fmaxf(y0, ly0), ly1), y1 = fminf(fmaxf(y1, ly0), ly1);

  civue_box b = {up(x0 - 0.5f), up(y0 - 0.5f), up(x1 - 0.5f), up(y1 - 0.5f)};

  return cut(b, limit);
}

static bool turned(const civue_quad *q) {
  return q->turn[0] != 0 || q->turn[1] != 0;
}

// what a quad is drawn over, past its rect for a shadow.
static void geometry(const civue_quad *q, f32 g[4]) {
  f32 m = q->params[1] == CIVUE_QUAD_SHADOW ? q->params[2] * 3 : 0;

  g[0] = q->rect[0] - m, g[1] = q->rect[1] - m;
  g[2] = q->rect[0] + q->rect[2] + m, g[3] = q->rect[1] + q->rect[3] + m;
}

// the pixels a quad may change inside `limit`.
static civue_box bounds(const civue_quad *q, civue_box limit) {
#if defined(__SSE2__) && !defined(__ARM_NEON)
  // the four edges at once: asked of every quad of a frame, several times.
  if (!turned(q)) {
    __m128 rect = _mm_loadu_ps(q->rect), clip = _mm_loadu_ps(q->clip);
    __m128 margin = _mm_set1_ps(
      q->params[1] == CIVUE_QUAD_SHADOW ? q->params[2] * 3 : 0);
    __m128 from = _mm_sub_ps(rect, margin);
    __m128 to = _mm_add_ps(
      _mm_add_ps(rect, _mm_movehl_ps(rect, rect)), margin);
    // x0, y0, x1 and y1, the first two no less than the clip's and the last
    // two no more.
    __m128 g = _mm_movelh_ps(from, to);
    __m128 inside = _mm_shuffle_ps(_mm_max_ps(g, clip), _mm_min_ps(g, clip),
                                   _MM_SHUFFLE(3, 2, 1, 0));
    __m128 low = _mm_setr_ps((f32)limit.x0, (f32)limit.y0, (f32)limit.x0,
                             (f32)limit.y0);
    __m128 high = _mm_setr_ps((f32)limit.x1, (f32)limit.y1, (f32)limit.x1,
                              (f32)limit.y1);
    __m128 v = _mm_sub_ps(_mm_min_ps(_mm_max_ps(inside, low), high),
                          _mm_set1_ps(0.5f));
    // rounded up: cut, and one more where that fell short.
    __m128i whole = _mm_cvttps_epi32(v);

    whole = _mm_sub_epi32(
      whole, _mm_castps_si128(_mm_cmplt_ps(_mm_cvtepi32_ps(whole), v)));

    civue_box b;
    _mm_storeu_si128((__m128i *)(void *)&b, whole);
    return cut(b, limit);
  }
#endif

  f32 g[4];
  geometry(q, g);

  if (!turned(q)) {
    return cover(fmaxf(g[0], q->clip[0]), fmaxf(g[1], q->clip[1]),
                 fminf(g[2], q->clip[2]), fminf(g[3], q->clip[3]), limit);
  }

  // turned, its clip turns with it: the box around its four corners.
  f32 c = q->turn[0], s = q->turn[1], px = q->turn[2], py = q->turn[3];
  f32 x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;

  for (u32 i = 0; i < 4; i++) {
    f32 dx = g[i & 1 ? 2 : 0] - px, dy = g[i & 2 ? 3 : 1] - py;
    f32 x = px + dx * c - dy * s, y = py + dx * s + dy * c;

    x0 = fminf(x0, x), y0 = fminf(y0, y);
    x1 = fmaxf(x1, x), y1 = fmaxf(y1, y);
  }

  return cover(x0, y0, x1, y1, limit);
}

static bool same_quad(const civue_quad *a, const civue_quad *b) {
  u64 x[20], y[20], differ = 0;
  static_assert(sizeof x == sizeof *a);
  memcpy(x, a, sizeof x);
  memcpy(y, b, sizeof y);

  for (u32 i = 0; i < 20; i++) {
    differ |= x[i] ^ y[i];
  }

  return !differ;
}

static bool same_batches(const civue_draw_list *a, const civue_draw_list *b) {
  if (a->batches.len != b->batches.len) {
    return false;
  }

  for (u32 i = 0; i < a->batches.len; i++) {
    const civue_batch *x = &a->batches.v[i], *y = &b->batches.v[i];

    if (x->start != y->start || x->texture != y->texture ||
        x->multiply != y->multiply || x->erase != y->erase) {
      return false;
    }
  }

  return true;
}

// the batch of a list's quad.
static const civue_batch *batch_at(const civue_draw_list *l, u32 quad) {
  u32 lo = 0, hi = l->batches.len;

  while (hi - lo > 1) {
    u32 mid = (lo + hi) / 2;

    if (l->batches.v[mid].start <= quad) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }

  return &l->batches.v[lo];
}

// whether a quad of each list draws the same: itself, its image and how it
// blends.
static bool same_drawn(const civue_draw_list *a, u32 i,
                       const civue_draw_list *b, u32 j) {
  if (!same_quad(&a->quads.v[i], &b->quads.v[j])) {
    return false;
  }

  const civue_batch *x = batch_at(a, i), *y = batch_at(b, j);

  return x->texture == y->texture && x->multiply == y->multiply &&
         x->erase == y->erase;
}

// past this many changed quads the whole window is drawn again: telling
// where each is takes longer than what it would save.
constexpr u32 CHANGES = 1024;

void civue_damage_between(const civue_draw_list *last,
                          const civue_draw_list *next, u32 width, u32 height,
                          civue_damage *out) {
  civue_damage_begin(out, width, height);

  civue_box window = {0, 0, (i32)width, (i32)height};
  const civue_quad *a = last->quads.v, *b = next->quads.v;
  u32 n = last->quads.len, m = next->quads.len;
  u32 changed = 0;

  if (n == m && (same_batches(last, next) || n == 0)) {
    for (u32 i = 0; i < n; i++) {
      if (same_quad(&a[i], &b[i])) {
        continue;
      }

      if (++changed > CHANGES) {
        out->all = true;
        break;
      }

      civue_damage_add(out, bounds(&a[i], window));
      civue_damage_add(out, bounds(&b[i], window));
    }
  }
  else if (n == m) {
    // as many quads in other batches: an image came in place of what stood
    // for it.
    for (u32 i = 0; i < n; i++) {
      if (same_drawn(last, i, next, i)) {
        continue;
      }

      if (++changed > CHANGES) {
        out->all = true;
        break;
      }

      civue_damage_add(out, bounds(&a[i], window));
      civue_damage_add(out, bounds(&b[i], window));
    }
  }
  else if (n && m) {
    // quads came or went: what is between those both lists start and end
    // with, an image's quads with the same image.
    u32 least = MIN(n, m), head = 0, tail = 0;

    while (head < least && same_drawn(last, head, next, head)) {
      head++;
    }

    while (tail < least - head &&
           same_drawn(last, n - 1 - tail, next, m - 1 - tail)) {
      tail++;
    }

    if (n + m - 2 * (head + tail) > 2 * CHANGES) {
      out->all = true;
    }
    else {
      for (u32 i = head; i < n - tail; i++) {
        civue_damage_add(out, bounds(&a[i], window));
      }

      for (u32 i = head; i < m - tail; i++) {
        civue_damage_add(out, bounds(&b[i], window));
      }
    }
  }
  else {
    out->all = true;
  }

  u64 whole = (u64)width * height;

  if (out->all || civue_damage_area(out, width, height) * 8 >= whole * 7) {
    *out = (civue_damage){.all = true};
  }
}

// ---- the shader's math, a pixel at a time ----

static f32 clamp01(f32 v) {
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

static f32 clampf(f32 v, f32 lo, f32 hi) {
  return fminf(fmaxf(v, lo), hi);
}

static f32 sd_rounded(f32 px, f32 py, f32 hx, f32 hy, const f32 radii[4]) {
  f32 r = px > 0 ? radii[1] : radii[0];

  if (py > 0) {
    r = px > 0 ? radii[2] : radii[3];
  }

  r = fminf(r, fminf(hx, hy));
  f32 qx = fabsf(px) - hx + r, qy = fabsf(py) - hy + r;
  f32 mx = fmaxf(qx, 0), my = fmaxf(qy, 0);

  return fminf(fmaxf(qx, qy), 0) + sqrtf(mx * mx + my * my) - r;
}

static void scaled(const f32 c[4], f32 by, f32 out[4]) {
  for (u32 i = 0; i < 4; i++) {
    out[i] = c[i] * by;
  }
}

static f32 fract(f32 v) {
  return v - floorf(v);
}

static void pixel_grid(const f32 c[4], f32 x, f32 y, const f32 grid[4],
                       f32 out[4]) {
  f32 fx = fract((x - grid[0]) / grid[2]);
  f32 fy = fract((y - grid[1]) / grid[2]);
  f32 d = 1 / grid[2];

  memcpy(out, c, 4 * sizeof *out);

  if (fx >= d && fy >= d) {
    return;
  }

  const f32 h[3] = {0.299f, 0.587f, 0.114f};
  f32 i = c[0] * c[0] * h[0] + c[1] * c[1] * h[1] + c[2] * c[2] * h[2];
  f32 j = sqrtf(i);

  if (j > 0.5f) {
    for (u32 k = 0; k < 3; k++) {
      out[k] = c[k] * ((j - 0.05f) / j);
    }

    return;
  }

  f32 k = j * 0.8f + 0.15f;
  f32 l = c[0] * h[0] + c[1] * h[1] + c[2] * h[2];
  f32 m = 1 - 2 * l + i;
  f32 n = i - l;
  f32 t = (n + sqrtf(n * n - m * (i - k * k))) / m;

  for (u32 e = 0; e < 3; e++) {
    out[e] = c[e] + (1 - c[e]) * t;
  }
}

static void shade_rect(const civue_quad *q, f32 x, f32 y, f32 out[4]) {
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  f32 px = x - q->rect[0] - hx, py = y - q->rect[1] - hy;
  f32 outer = clamp01(0.5f - sd_rounded(px, py, hx, hy, q->radii));
  f32 bw = q->params[0];

  if (q->params[2] > 0.5f) {
    pixel_grid(q->color, x, y, q->border, out);
    return;
  }

  if (bw == 0) {
    scaled(q->color, outer, out);
    return;
  }

  f32 inner;

  if (bw < 0) {
    // top, right, bottom, left.
    f32 s[4];

    for (u32 i = 0; i < 4; i++) {
      s[i] = q->uv[i] == 0 ? -1 : q->uv[i];
    }

    f32 ihx = fmaxf((q->rect[2] - (s[3] + s[1])) * 0.5f, 0);
    f32 ihy = fmaxf((q->rect[3] - (s[0] + s[2])) * 0.5f, 0);
    f32 cx = q->rect[0] + s[3] + ihx, cy = q->rect[1] + s[0] + ihy;
    f32 wider[4] = {fmaxf(s[0], s[3]), fmaxf(s[0], s[1]), fmaxf(s[2], s[1]),
                    fmaxf(s[2], s[3])};
    f32 radii[4];

    for (u32 i = 0; i < 4; i++) {
      radii[i] = fmaxf(q->radii[i] - wider[i], 0);
    }

    inner = clamp01(0.5f - sd_rounded(x - cx, y - cy, ihx, ihy, radii));

    if (ihx <= 0 || ihy <= 0) {
      inner = 0;
    }
  }
  else {
    f32 radii[4];

    for (u32 i = 0; i < 4; i++) {
      radii[i] = fmaxf(q->radii[i] - bw, 0);
    }

    inner = clamp01(
      0.5f - sd_rounded(px, py, fmaxf(hx - bw, 0), fmaxf(hy - bw, 0), radii));
  }

  for (u32 i = 0; i < 4; i++) {
    out[i] = (q->border[i] * (1 - inner) + q->color[i] * inner) * outer;
  }
}

static f32 gaussian(f32 x, f32 sigma) {
  return expf(-(x * x) / (2 * sigma * sigma)) / (2.50662827f * sigma);
}

static f32 erf_of(f32 x) {
  f32 s = x > 0 ? 1 : x < 0 ? -1 : 0;
  f32 a = fabsf(x);
  f32 r = 1 + (0.278393f + (0.230389f + 0.078108f * (a * a)) * a) * a;

  r *= r;
  return s - s / (r * r);
}

static f32 shadow_x(f32 x, f32 y, f32 sigma, f32 corner, f32 hx, f32 hy) {
  f32 delta = fminf(hy - corner - fabsf(y), 0);
  f32 curved = hx - corner + sqrtf(fmaxf(0, corner * corner - delta * delta));
  f32 k = 0.70710678f / sigma;
  f32 low = 0.5f + 0.5f * erf_of((x - curved) * k);
  f32 high = 0.5f + 0.5f * erf_of((x + curved) * k);

  return high - low;
}

static f32 shadow(const civue_quad *q, f32 x, f32 y) {
  f32 sigma = fmaxf(q->params[2], 0.01f), corner = q->radii[0];
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  f32 px = x - q->rect[0] - hx, py = y - q->rect[1] - hy;
  f32 low = py - hy, high = py + hy;
  f32 start = clampf(-3 * sigma, low, high);
  f32 end = clampf(3 * sigma, low, high);
  f32 step = (end - start) / 4;
  f32 at = start + step * 0.5f;
  f32 v = 0;

  for (u32 i = 0; i < 4; i++) {
    v +=
      shadow_x(px, py - at, sigma, corner, hx, hy) * gaussian(at, sigma) * step;
    at += step;
  }

  return v;
}

static f32 line(const civue_quad *q, f32 x, f32 y) {
  f32 ax = q->radii[0], ay = q->radii[1];
  f32 bx = q->radii[2] - ax, by = q->radii[3] - ay;
  f32 width = q->params[0];

  if (q->params[2] > 0.5f) {
    f32 len = fmaxf(sqrtf(bx * bx + by * by), 1e-6f);
    f32 along = ((x - ax) * bx + (y - ay) * by) / len;
    f32 across = fabsf(bx * (y - ay) - by * (x - ax)) / len;
    f32 ends = clamp01(fminf(along, len - along) + 0.5f);

    return clamp01(width * 0.5f + 0.5f - across) * ends;
  }

  f32 t =
    clamp01(((x - ax) * bx + (y - ay) * by) / fmaxf(bx * bx + by * by, 1e-6f));
  f32 dx = x - ax - bx * t, dy = y - ay - by * t;

  return clamp01(width * 0.5f + 0.5f - sqrtf(dx * dx + dy * dy));
}

typedef struct {
  const civue_surface *s;
  // the atlases' pixels, null when they have none yet.
  const u8 *mask, *color;
  // what is drawn now.
  civue_box box;
  // how the batch drawn now blends, BLEND_OVER for most.
  u8 blending;
  // the pixel of the color under everything.
  u32 clear;
  // where images are found, and the one the batch drawn now shows.
  const civue_raster_images *images;
  civue_texture texture;
  civue_pixels image;
} Raster;

// where a quad's point is in an atlas.
static void atlas_at(const civue_quad *q, f32 x, f32 y, f32 *u, f32 *v) {
  f32 tx = (x - q->rect[0]) / q->rect[2], ty = (y - q->rect[1]) / q->rect[3];

  *u = (q->uv[0] + (q->uv[2] - q->uv[0]) * tx) * CIVUE_ATLAS_SIZE;
  *v = (q->uv[1] + (q->uv[3] - q->uv[1]) * ty) * CIVUE_ATLAS_SIZE;
}

static i32 texel(f32 v) {
  return (i32)clampf(floorf(v), 0, CIVUE_ATLAS_SIZE - 1);
}

// the nearest texel of the mask atlas.
static f32 mask_at(const Raster *r, const civue_quad *q, f32 x, f32 y) {
  if (!r->mask) {
    return 0;
  }

  f32 u, v;
  atlas_at(q, x, y, &u, &v);

  return r->mask[(usize)texel(v) * CIVUE_ATLAS_SIZE + texel(u)] / 255.0f;
}

// the color atlas between its texels.
static void color_at(const Raster *r, const civue_quad *q, f32 x, f32 y,
                     f32 out[4]) {
  memset(out, 0, 4 * sizeof *out);

  if (!r->color) {
    return;
  }

  f32 u, v;
  atlas_at(q, x, y, &u, &v);
  u -= 0.5f, v -= 0.5f;

  f32 fx = fract(u), fy = fract(v);
  i32 x0 = texel(u), x1 = texel(u + 1), y0 = texel(v), y1 = texel(v + 1);
  const u8 *rows[2] = {r->color + (usize)y0 * CIVUE_ATLAS_SIZE * 4,
                       r->color + (usize)y1 * CIVUE_ATLAS_SIZE * 4};

  for (u32 i = 0; i < 4; i++) {
    f32 top = rows[0][x0 * 4 + i] * (1 - fx) + rows[0][x1 * 4 + i] * fx;
    f32 bottom = rows[1][x0 * 4 + i] * (1 - fx) + rows[1][x1 * 4 + i] * fx;

    out[i] = (top * (1 - fy) + bottom * fy) / 255.0f;
  }
}

// the image between its texels, or at the nearest one, as the gpu samples
// it: clamped to its edges.
// where a point of an image's quad is in its texels, across and down.
static f32 image_u(const civue_quad *q, const civue_pixels *im, f32 x) {
  f32 tx = (x - q->rect[0]) / q->rect[2];

  return (q->uv[0] + (q->uv[2] - q->uv[0]) * tx) * (f32)im->width;
}

static f32 image_v(const civue_quad *q, const civue_pixels *im, f32 y) {
  f32 ty = (y - q->rect[1]) / q->rect[3];

  return (q->uv[1] + (q->uv[3] - q->uv[1]) * ty) * (f32)im->height;
}

static void image_at(const Raster *r, const civue_quad *q, f32 x, f32 y,
                     f32 out[4]) {
  const civue_pixels *im = &r->image;

  memset(out, 0, 4 * sizeof *out);

  if (!im->pixels || !im->width || !im->height) {
    return;
  }

  f32 u = image_u(q, im, x), v = image_v(q, im, y);
  f32 w1 = (f32)(im->width - 1), h1 = (f32)(im->height - 1);

  if (q->params[3] > 0.5f) {
    const u8 *p = im->pixels + (usize)clampf(floorf(v), 0, h1) * im->stride +
                  (usize)clampf(floorf(u), 0, w1) * 4;

    for (u32 i = 0; i < 4; i++) {
      out[i] = p[i] / 255.0f;
    }

    return;
  }

  u -= 0.5f, v -= 0.5f;

  f32 fx = fract(u), fy = fract(v);
  usize x0 = (usize)clampf(floorf(u), 0, w1) * 4;
  usize x1 = (usize)clampf(floorf(u) + 1, 0, w1) * 4;
  const u8 *rows[2] = {
    im->pixels + (usize)clampf(floorf(v), 0, h1) * im->stride,
    im->pixels + (usize)clampf(floorf(v) + 1, 0, h1) * im->stride};

  for (u32 i = 0; i < 4; i++) {
    f32 top = rows[0][x0 + i] * (1 - fx) + rows[0][x1 + i] * fx;
    f32 bottom = rows[1][x0 + i] * (1 - fx) + rows[1][x1 + i] * fx;

    out[i] = (top * (1 - fy) + bottom * fy) / 255.0f;
  }
}

static bool rounded(const civue_quad *q) {
  return q->round_radii[0] > 0 || q->round_radii[1] > 0 ||
         q->round_radii[2] > 0 || q->round_radii[3] > 0;
}

// what a rounded clip leaves of a pixel: only its corners cut, the sides
// are the rect clip's.
static f32 corners(const civue_quad *q, f32 x, f32 y) {
  f32 hx = q->round[2] * 0.5f, hy = q->round[3] * 0.5f;
  f32 px = x - q->round[0] - hx, py = y - q->round[1] - hy;
  f32 r = px > 0 ? q->round_radii[1] : q->round_radii[0];

  if (py > 0) {
    r = px > 0 ? q->round_radii[2] : q->round_radii[3];
  }

  r = fminf(r, fminf(hx, hy));
  f32 qx = fabsf(px) - hx + r, qy = fabsf(py) - hy + r;

  if (r <= 0 || qx <= 0 || qy <= 0) {
    return 1;
  }

  return clamp01(0.5f + r - sqrtf(qx * qx + qy * qy));
}

static bool shade_kind(const Raster *r, const civue_quad *q, f32 x, f32 y,
                       f32 out[4]);

// what a quad's fragment shader returns at a point, false when it discards.
static bool shade(const Raster *r, const civue_quad *q, f32 x, f32 y,
                  f32 out[4]) {
  if (!shade_kind(r, q, x, y, out)) {
    return false;
  }

  if (rounded(q)) {
    f32 inside = corners(q, x, y);

    for (u32 i = 0; i < 4; i++) {
      out[i] *= inside;
    }
  }

  return true;
}

static bool shade_kind(const Raster *r, const civue_quad *q, f32 x, f32 y,
                       f32 out[4]) {
  if (x < q->clip[0] || y < q->clip[1] || x >= q->clip[2] || y >= q->clip[3]) {
    return false;
  }

  switch ((u32)q->params[1]) {
    case CIVUE_QUAD_SHADOW:
      scaled(q->color, shadow(q, x, y), out);
      return true;

    case CIVUE_QUAD_MASK:
      scaled(q->color, powf(mask_at(r, q, x, y), q->params[2]), out);
      return true;

    case CIVUE_QUAD_COLOR_GLYPH: {
      f32 c[4];
      color_at(r, q, x, y, c);
      scaled(c, q->color[3], out);
      return true;
    }

    case CIVUE_QUAD_LINE:
      scaled(q->color, line(q, x, y), out);
      return true;

    case CIVUE_QUAD_RECT:
      shade_rect(q, x, y, out);
      return true;

    case CIVUE_QUAD_IMAGE: {
      f32 c[4];
      image_at(r, q, x, y, c);

      if (q->params[2] > 0.5f) {
        pixel_grid(c, x, y, q->border, out);
        return true;
      }

      // canvas tiles cover their pixels whole, so neighbors meet seamlessly.
      if (q->params[0] > 0.5f) {
        scaled(c, q->color[3], out);
        return true;
      }

      f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
      f32 px = x - q->rect[0] - hx, py = y - q->rect[1] - hy;

      scaled(c, q->color[3], out);
      scaled(out, clamp01(0.5f - sd_rounded(px, py, hx, hy, q->radii)), out);
      return true;
    }

    default:
      return false;
  }
}

// ---- pixels ----

static u8 *pixel(const civue_surface *s, i32 x, i32 y) {
  return s->pixels + (usize)y * s->stride + (usize)x * 4;
}

// a value out of 255, rounded.
static u8 byte(f32 v) {
  v += 0.5f;
  return !(v > 0) ? 0 : v >= 255 ? 255 : (u8)v;
}

// a premultiplied color over a pixel, as the gpu blends it.
enum : u8 { BLEND_OVER, BLEND_MULTIPLY, BLEND_ERASE };

static u8 blending_of(const civue_batch *b) {
  return b->erase ? BLEND_ERASE : b->multiply ? BLEND_MULTIPLY : BLEND_OVER;
}

#if defined(__SSE2__) && !defined(__ARM_NEON)
// a premultiplied color, blue first, over a pixel: the four channels at
// once, rounded as `byte` does.
static inline void blend_bgra(u8 *d, __m128 c) {
  u32 was;
  memcpy(&was, d, sizeof was);

  __m128i zero = _mm_setzero_si128();
  __m128 inv = _mm_sub_ps(_mm_set1_ps(1), _mm_shuffle_ps(c, c, 0xff));
  __m128 over = _mm_mul_ps(c, _mm_set1_ps(255));
  __m128i wide = _mm_unpacklo_epi16(
    _mm_unpacklo_epi8(_mm_cvtsi32_si128((i32)was), zero), zero);
  __m128 sum = _mm_add_ps(_mm_add_ps(over, _mm_mul_ps(_mm_cvtepi32_ps(wide), inv)),
                          _mm_set1_ps(0.5f));
  // cut toward zero, then kept in a byte: under zero and no number are 0.
  __m128i narrow = _mm_cvttps_epi32(sum);

  narrow = _mm_packs_epi32(narrow, narrow);
  u32 now = (u32)_mm_cvtsi128_si32(_mm_packus_epi16(narrow, narrow));

  memcpy(d, &now, sizeof now);
}
#endif

static void blend(u8 *d, const f32 c[4], u8 blending) {
  f32 inv = 1 - c[3];

  if (blending == BLEND_ERASE) {
    for (u32 k = 0; k < 4; k++) {
      d[k] = byte(d[k] * inv);
    }

    return;
  }
  else if (blending == BLEND_MULTIPLY) {
    d[0] = byte(d[0] * (c[2] + inv));
    d[1] = byte(d[1] * (c[1] + inv));
    d[2] = byte(d[2] * (c[0] + inv));
  }
  else {
#ifdef __ARM_NEON
    // the four channels at once, blue first, rounded as `byte` does.
    u32 was;
    memcpy(&was, d, sizeof was);

    float32x4_t over = vmulq_n_f32((float32x4_t){c[2], c[1], c[0], c[3]}, 255);
    uint16x8_t wide = vmovl_u8(vcreate_u8(was));
    float32x4_t under = vcvtq_f32_u32(vmovl_u16(vget_low_u16(wide)));
    float32x4_t sum =
      vaddq_f32(vaddq_f32(over, vmulq_n_f32(under, inv)), vdupq_n_f32(0.5f));
    uint16x4_t narrow = vqmovn_u32(vcvtq_u32_f32(sum));
    u32 now = vget_lane_u32(
      vreinterpret_u32_u8(vqmovn_u16(vcombine_u16(narrow, narrow))), 0);

    memcpy(d, &now, sizeof now);
    return;
#elif defined(__SSE2__)
    blend_bgra(d, _mm_setr_ps(c[2], c[1], c[0], c[3]));
    return;
#else
    d[0] = byte(c[2] * 255 + d[0] * inv);
    d[1] = byte(c[1] * 255 + d[1] * inv);
    d[2] = byte(c[0] * 255 + d[2] * inv);
#endif
  }

  d[3] = byte(c[3] * 255 + d[3] * inv);
}

// `n` pixels set to `v`. in the loop of every row that calls it.
[[gnu::always_inline]] static inline void fill(u8 *d, u32 n, u32 v) {
  u32 i = 0;

#ifdef __ARM_NEON
  uint32x4_t lanes = vdupq_n_u32(v);

  for (; i + 16 <= n; i += 16) {
    vst1q_u32((u32 *)(void *)(d + i * 4), lanes);
    vst1q_u32((u32 *)(void *)(d + i * 4 + 16), lanes);
    vst1q_u32((u32 *)(void *)(d + i * 4 + 32), lanes);
    vst1q_u32((u32 *)(void *)(d + i * 4 + 48), lanes);
  }

  for (; i + 4 <= n; i += 4) {
    vst1q_u32((u32 *)(void *)(d + i * 4), lanes);
  }
#elif defined(__AVX2__)
  __m256i lanes = _mm256_set1_epi32((i32)v);

  for (; i + 32 <= n; i += 32) {
    _mm256_storeu_si256((__m256i *)(void *)(d + i * 4), lanes);
    _mm256_storeu_si256((__m256i *)(void *)(d + i * 4 + 32), lanes);
    _mm256_storeu_si256((__m256i *)(void *)(d + i * 4 + 64), lanes);
    _mm256_storeu_si256((__m256i *)(void *)(d + i * 4 + 96), lanes);
  }

  for (; i + 8 <= n; i += 8) {
    _mm256_storeu_si256((__m256i *)(void *)(d + i * 4), lanes);
  }
#elif defined(__SSE2__)
  __m128i lanes = _mm_set1_epi32((i32)v);

  for (; i + 16 <= n; i += 16) {
    _mm_storeu_si128((__m128i *)(void *)(d + i * 4), lanes);
    _mm_storeu_si128((__m128i *)(void *)(d + i * 4 + 16), lanes);
    _mm_storeu_si128((__m128i *)(void *)(d + i * 4 + 32), lanes);
    _mm_storeu_si128((__m128i *)(void *)(d + i * 4 + 48), lanes);
  }

  for (; i + 4 <= n; i += 4) {
    _mm_storeu_si128((__m128i *)(void *)(d + i * 4), lanes);
  }
#endif

  for (; i < n; i++) {
    memcpy(d + i * 4, &v, sizeof v);
  }
}

// `n` pixels set to `v` in memory that is far from the processor's caches
// and will not be read back soon: written past them, which spares reading
// each line before it is written whole. x86 has to be told. the arm cores
// that can do it see a long fill by themselves, so theirs is a plain one.
#ifdef __SSE2__
static void fill_far(u8 *d, u32 n, u32 v) {
  u32 i = 0;

  // up to a line's start, then a line at a time.
  while (i < n && ((uintptr_t)(d + i * 4) & 63)) {
    memcpy(d + i * 4, &v, sizeof v);
    i++;
  }

  __m128i lanes = _mm_set1_epi32((i32)v);

  for (; i + 16 <= n; i += 16) {
    _mm_stream_si128((__m128i *)(void *)(d + i * 4), lanes);
    _mm_stream_si128((__m128i *)(void *)(d + i * 4 + 16), lanes);
    _mm_stream_si128((__m128i *)(void *)(d + i * 4 + 32), lanes);
    _mm_stream_si128((__m128i *)(void *)(d + i * 4 + 48), lanes);
  }

  fill(d + i * 4, n - i, v);
}
#else
[[gnu::always_inline]] static inline void fill_far(u8 *d, u32 n, u32 v) {
  fill(d, n, v);
}
#endif

// one color over a row's pixels from x0 to x1.
static void span(const Raster *r, i32 y, i32 x0, i32 x1, const f32 c[4]) {
  if (x0 >= x1) {
    return;
  }

  u8 *d = pixel(r->s, x0, y);
  u32 n = (u32)(x1 - x0);

  if (r->blending) {
    for (u32 i = 0; i < n; i++) {
      blend(d + i * 4, c, r->blending);
    }

    return;
  }

  if (c[3] >= 1) {
    u8 p[4] = {byte(c[2] * 255), byte(c[1] * 255), byte(c[0] * 255), 255};
    u32 v;
    memcpy(&v, p, sizeof v);
    fill(d, n, v);
    return;
  }

  if (c[3] <= 0 && c[0] <= 0 && c[1] <= 0 && c[2] <= 0) {
    return;
  }

  // sixteen bits of fraction.
  constexpr u32 ONE = 65536;
  u32 inv = (u32)(clamp01(1 - c[3]) * ONE + 0.5f);
  u32 add[4] = {(u32)(clamp01(c[2]) * 255 * ONE + 0.5f) + ONE / 2,
                (u32)(clamp01(c[1]) * 255 * ONE + 0.5f) + ONE / 2,
                (u32)(clamp01(c[0]) * 255 * ONE + 0.5f) + ONE / 2,
                (u32)(clamp01(c[3]) * 255 * ONE + 0.5f) + ONE / 2};

  u32 i = 0;

#ifdef __ARM_NEON
  // four pixels at a time, each channel in a lane of its own.
  uint32x4_t adds = vld1q_u32(add);

  for (; i + 4 <= n; i += 4) {
    uint8x16_t px = vld1q_u8(d + i * 4);
    uint16x8_t lo = vmovl_u8(vget_low_u8(px)), hi = vmovl_high_u8(px);
    uint32x4_t a = vmlaq_n_u32(adds, vmovl_u16(vget_low_u16(lo)), inv);
    uint32x4_t b = vmlaq_n_u32(adds, vmovl_high_u16(lo), inv);
    uint32x4_t e = vmlaq_n_u32(adds, vmovl_u16(vget_low_u16(hi)), inv);
    uint32x4_t f = vmlaq_n_u32(adds, vmovl_high_u16(hi), inv);
    uint16x8_t first = vcombine_u16(vqshrn_n_u32(a, 16), vqshrn_n_u32(b, 16));
    uint16x8_t second = vcombine_u16(vqshrn_n_u32(e, 16), vqshrn_n_u32(f, 16));

    vst1q_u8(d + i * 4, vcombine_u8(vqmovn_u16(first), vqmovn_u16(second)));
  }
#elif defined(__SSE2__)
  // sixteen bits a channel, eight or four pixels at a time. what is added
  // is its whole part and its fraction apart, and the product its high and
  // low halves: the sum's whole part is theirs and the carry of the low
  // ones, the same as the 32 bits below give.
  u16 whole[4], part[4];

  for (u32 k = 0; k < 4; k++) {
    whole[k] = (u16)(add[k] >> 16);
    part[k] = (u16)(add[k] & 0xffff);
  }

  if (inv < ONE) {
#ifdef __AVX2__
    __m256i zero = _mm256_setzero_si256(), sign = _mm256_set1_epi16(-32768);
    __m256i by = _mm256_set1_epi16((i16)inv);
    __m256i wholes = _mm256_setr_epi16(
      (i16)whole[0], (i16)whole[1], (i16)whole[2], (i16)whole[3],
      (i16)whole[0], (i16)whole[1], (i16)whole[2], (i16)whole[3],
      (i16)whole[0], (i16)whole[1], (i16)whole[2], (i16)whole[3],
      (i16)whole[0], (i16)whole[1], (i16)whole[2], (i16)whole[3]);
    __m256i parts = _mm256_setr_epi16(
      (i16)part[0], (i16)part[1], (i16)part[2], (i16)part[3], (i16)part[0],
      (i16)part[1], (i16)part[2], (i16)part[3], (i16)part[0], (i16)part[1],
      (i16)part[2], (i16)part[3], (i16)part[0], (i16)part[1], (i16)part[2],
      (i16)part[3]);
    __m256i parts_signed = _mm256_xor_si256(parts, sign);

    for (; i + 8 <= n; i += 8) {
      __m256i px = _mm256_loadu_si256((const __m256i *)(const void *)(d + i * 4));
      __m256i half[2] = {_mm256_unpacklo_epi8(px, zero),
                         _mm256_unpackhi_epi8(px, zero)};

      for (u32 h = 0; h < 2; h++) {
        __m256i high = _mm256_mulhi_epu16(half[h], by);
        __m256i low = _mm256_add_epi16(_mm256_mullo_epi16(half[h], by), parts);
        // all ones where the low halves went past sixteen bits.
        __m256i carry =
          _mm256_cmpgt_epi16(parts_signed, _mm256_xor_si256(low, sign));

        half[h] = _mm256_sub_epi16(_mm256_add_epi16(high, wholes), carry);
      }

      _mm256_storeu_si256((__m256i *)(void *)(d + i * 4),
                          _mm256_packus_epi16(half[0], half[1]));
    }
#endif

    __m128i zero4 = _mm_setzero_si128(), sign4 = _mm_set1_epi16(-32768);
    __m128i by4 = _mm_set1_epi16((i16)inv);
    __m128i wholes4 = _mm_setr_epi16(
      (i16)whole[0], (i16)whole[1], (i16)whole[2], (i16)whole[3],
      (i16)whole[0], (i16)whole[1], (i16)whole[2], (i16)whole[3]);
    __m128i parts4 = _mm_setr_epi16(
      (i16)part[0], (i16)part[1], (i16)part[2], (i16)part[3], (i16)part[0],
      (i16)part[1], (i16)part[2], (i16)part[3]);
    __m128i parts4_signed = _mm_xor_si128(parts4, sign4);

    for (; i + 4 <= n; i += 4) {
      __m128i px = _mm_loadu_si128((const __m128i *)(const void *)(d + i * 4));
      __m128i half[2] = {_mm_unpacklo_epi8(px, zero4),
                         _mm_unpackhi_epi8(px, zero4)};

      for (u32 h = 0; h < 2; h++) {
        __m128i high = _mm_mulhi_epu16(half[h], by4);
        __m128i low = _mm_add_epi16(_mm_mullo_epi16(half[h], by4), parts4);
        __m128i carry =
          _mm_cmpgt_epi16(parts4_signed, _mm_xor_si128(low, sign4));

        half[h] = _mm_sub_epi16(_mm_add_epi16(high, wholes4), carry);
      }

      _mm_storeu_si128((__m128i *)(void *)(d + i * 4),
                       _mm_packus_epi16(half[0], half[1]));
    }
  }
#endif

  for (; i < n; i++) {
    for (u32 k = 0; k < 4; k++) {
      u32 v = (add[k] + d[i * 4 + k] * inv) >> 16;
      d[i * 4 + k] = (u8)MIN(v, 255u);
    }
  }
}

// a quad's own pixels, each asked to the shader's math.
static void each(const Raster *r, const civue_quad *q, civue_box b) {
  for (i32 y = b.y0; y < b.y1; y++) {
    u8 *d = pixel(r->s, b.x0, y);

    for (i32 x = b.x0; x < b.x1; x++, d += 4) {
      f32 c[4];

      if (shade(r, q, (f32)x + 0.5f, (f32)y + 0.5f, c)) {
        blend(d, c, r->blending);
      }
    }
  }
}

// a turned quad: each pixel around it turned back to where the quad lies
// unturned.
static void each_turned(const Raster *r, const civue_quad *q) {
  civue_box b = bounds(q, r->box);
  f32 g[4];
  geometry(q, g);
  f32 c = q->turn[0], s = q->turn[1], px = q->turn[2], py = q->turn[3];

  for (i32 y = b.y0; y < b.y1; y++) {
    u8 *d = pixel(r->s, b.x0, y);

    for (i32 x = b.x0; x < b.x1; x++, d += 4) {
      f32 dx = (f32)x + 0.5f - px, dy = (f32)y + 0.5f - py;
      f32 lx = px + dx * c + dy * s, ly = py - dx * s + dy * c;
      f32 color[4];

      if (lx < g[0] || ly < g[1] || lx >= g[2] || ly >= g[3]) {
        continue;
      }

      if (shade(r, q, lx, ly, color)) {
        blend(d, color, r->blending);
      }
    }
  }
}

// where a rect is one color whatever the pixel, x0, y0, x1 and y1: inside
// its borders and past its corners. between its corners a row is one color
// too, from mid[0] to mid[2].
typedef struct {
  f32 whole[4];
  f32 mid[4];
} Flat;

static Flat flat(const civue_quad *q) {
  f32 x0 = q->rect[0], y0 = q->rect[1];
  f32 x1 = x0 + q->rect[2], y1 = y0 + q->rect[3];
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  f32 bw = q->params[0];
  // the borders from the top clockwise as the shader has them, a side
  // without one a pixel outside, and what they take of the rect.
  f32 t[4], s[4];

  for (u32 i = 0; i < 4; i++) {
    t[i] = bw > 0 ? bw : bw == 0 ? 0 : q->uv[i] == 0 ? -1 : q->uv[i];
    s[i] = fmaxf(t[i], 0);
  }

  f32 ihx = fmaxf(hx - (t[3] + t[1]) * 0.5f, 0);
  f32 ihy = fmaxf(hy - (t[0] + t[2]) * 0.5f, 0);
  // how far each corner's curves reach along x and y, the corners from the
  // top left clockwise.
  f32 reach_x[4], reach_y[4];

  for (u32 i = 0; i < 4; i++) {
    f32 side_x = i == 0 || i == 3 ? t[3] : t[1];
    f32 side_y = i < 2 ? t[0] : t[2];
    f32 outer = fminf(q->radii[i], fminf(hx, hy));
    f32 inner = bw == 0 ? 0
                        : fminf(fmaxf(q->radii[i] - fmaxf(side_x, side_y), 0),
                                fminf(ihx, ihy));

    reach_x[i] = fmaxf(outer, side_x + inner);
    reach_y[i] = fmaxf(outer, side_y + inner);
  }

  f32 left = fmaxf(reach_x[0], reach_x[3]);
  f32 right = fmaxf(reach_x[1], reach_x[2]);
  f32 top = fmaxf(reach_y[0], reach_y[1]);
  f32 bottom = fmaxf(reach_y[2], reach_y[3]);

  // a pixel is whole once its center is half a pixel inside an edge.
  return (Flat){
    {ceilf(x0 + s[3]), ceilf(y0 + fmaxf(top - 0.5f, s[0])), floorf(x1 - s[1]),
     floorf(y1 - fmaxf(bottom - 0.5f, s[2]))},
    {ceilf(x0 + fmaxf(left - 0.5f, s[3])), 0,
     floorf(x1 - fmaxf(right - 0.5f, s[1])), 0},
  };
}

// a rect drawn in rows, past what a rounded clip cuts.
static bool flat_rect(const civue_quad *q) {
  return (u32)q->params[1] == CIVUE_QUAD_RECT && !turned(q) &&
         q->params[2] <= 0.5f;
}

static bool is_flat(const civue_quad *q) {
  return flat_rect(q) && !rounded(q);
}

// a rounded rect, its curves and edges asked a pixel at a time and the
// rest filled in rows.
// how many rows ahead of the one written its memory is asked for: what
// was last drawn frames ago is far away.
constexpr i32 AHEAD = 4;

static void ask_row(const Raster *r, civue_box b, i32 y) {
  const u8 *p = pixel(r->s, b.x0, y);
  // a long row is one the processor asks for by itself once it is begun.
  usize n = MIN((usize)(b.x1 - b.x0) * 4, (usize)2048);

  for (usize i = 0; i < n; i += 64) {
    __builtin_prefetch(p + i, 1, 3);
  }
}

// a rect that is nothing but its color, which hides what is under it.
static bool opaque_rect(const civue_quad *q) {
  return is_flat(q) && q->params[0] == 0 && q->color[3] >= 1 &&
         q->radii[0] == 0 && q->radii[1] == 0 && q->radii[2] == 0 &&
         q->radii[3] == 0;
}

#ifdef __ARM_NEON
// what shade_rect works out once for a quad: its rounded rect, and the one
// inside its border.
typedef struct {
  f32 hx, hy, radii[4];
  // 0 without a border, 1 for one as wide on each side, -1 for sides of
  // their own, whose inside has a center of its own and is none when flat.
  i32 border;
  bool no_inside;
  f32 ihx, ihy, icx, icy, iradii[4];
  // blue, green, red and alpha, as the pixels have them.
  float32x4_t color, edge;
} RectShape;

static RectShape rect_shape(const civue_quad *q) {
  RectShape s = {
    .hx = q->rect[2] * 0.5f,
    .hy = q->rect[3] * 0.5f,
    .color = {q->color[2], q->color[1], q->color[0], q->color[3]},
    .edge = {q->border[2], q->border[1], q->border[0], q->border[3]},
  };
  f32 bw = q->params[0];

  memcpy(s.radii, q->radii, sizeof s.radii);

  if (bw < 0) {
    f32 t[4];

    for (u32 i = 0; i < 4; i++) {
      t[i] = q->uv[i] == 0 ? -1 : q->uv[i];
    }

    s.border = -1;
    s.ihx = fmaxf((q->rect[2] - (t[3] + t[1])) * 0.5f, 0);
    s.ihy = fmaxf((q->rect[3] - (t[0] + t[2])) * 0.5f, 0);
    s.icx = q->rect[0] + t[3] + s.ihx;
    s.icy = q->rect[1] + t[0] + s.ihy;
    s.no_inside = s.ihx <= 0 || s.ihy <= 0;

    f32 wider[4] = {fmaxf(t[0], t[3]), fmaxf(t[0], t[1]), fmaxf(t[2], t[1]),
                    fmaxf(t[2], t[3])};

    for (u32 i = 0; i < 4; i++) {
      s.iradii[i] = fmaxf(q->radii[i] - wider[i], 0);
    }
  }
  else if (bw > 0) {
    s.border = 1;
    s.ihx = fmaxf(s.hx - bw, 0);
    s.ihy = fmaxf(s.hy - bw, 0);

    for (u32 i = 0; i < 4; i++) {
      s.iradii[i] = fmaxf(q->radii[i] - bw, 0);
    }
  }

  return s;
}

// sd_rounded at four points.
static float32x4_t sd_rounded4(float32x4_t px, float32x4_t py, f32 hx, f32 hy,
                               const f32 radii[4]) {
  uint32x4_t right = vcgtq_f32(px, vdupq_n_f32(0));
  uint32x4_t below = vcgtq_f32(py, vdupq_n_f32(0));
  float32x4_t top =
    vbslq_f32(right, vdupq_n_f32(radii[1]), vdupq_n_f32(radii[0]));
  float32x4_t bottom =
    vbslq_f32(right, vdupq_n_f32(radii[2]), vdupq_n_f32(radii[3]));
  float32x4_t r =
    vminq_f32(vbslq_f32(below, bottom, top), vdupq_n_f32(fminf(hx, hy)));
  float32x4_t qx = vaddq_f32(vsubq_f32(vabsq_f32(px), vdupq_n_f32(hx)), r);
  float32x4_t qy = vaddq_f32(vsubq_f32(vabsq_f32(py), vdupq_n_f32(hy)), r);
  float32x4_t mx = vmaxq_f32(qx, vdupq_n_f32(0));
  float32x4_t my = vmaxq_f32(qy, vdupq_n_f32(0));
  float32x4_t len = vsqrtq_f32(vfmaq_f32(vmulq_f32(my, my), mx, mx));

  return vsubq_f32(vaddq_f32(vminq_f32(vmaxq_f32(qx, qy), vdupq_n_f32(0)), len),
                   r);
}

static float32x4_t coverage4(float32x4_t sd) {
  return vminq_f32(vmaxq_f32(vsubq_f32(vdupq_n_f32(0.5f), sd), vdupq_n_f32(0)),
                   vdupq_n_f32(1));
}

// a premultiplied color, blue first, over a pixel: blend's own math.
static void blend_bgra(u8 *d, float32x4_t c) {
  u32 was;
  memcpy(&was, d, sizeof was);

  f32 inv = 1 - vgetq_lane_f32(c, 3);
  float32x4_t over = vmulq_n_f32(c, 255);
  uint16x8_t wide = vmovl_u8(vcreate_u8(was));
  float32x4_t under = vcvtq_f32_u32(vmovl_u16(vget_low_u16(wide)));
  float32x4_t sum =
    vaddq_f32(vaddq_f32(over, vmulq_n_f32(under, inv)), vdupq_n_f32(0.5f));
  uint16x4_t narrow = vqmovn_u32(vcvtq_u32_f32(sum));
  u32 now = vget_lane_u32(
    vreinterpret_u32_u8(vqmovn_u16(vcombine_u16(narrow, narrow))), 0);

  memcpy(d, &now, sizeof now);
}
#elif defined(__SSE2__)
// the same on x86: what shade_rect works out once for a quad.
typedef struct {
  f32 hx, hy, radii[4];
  i32 border;
  bool no_inside;
  f32 ihx, ihy, icx, icy, iradii[4];
  // blue, green, red and alpha, as the pixels have them.
  __m128 color, edge;
} RectShape;

static RectShape rect_shape(const civue_quad *q) {
  RectShape s = {
    .hx = q->rect[2] * 0.5f,
    .hy = q->rect[3] * 0.5f,
    .color = _mm_setr_ps(q->color[2], q->color[1], q->color[0], q->color[3]),
    .edge = _mm_setr_ps(q->border[2], q->border[1], q->border[0], q->border[3]),
  };
  f32 bw = q->params[0];

  memcpy(s.radii, q->radii, sizeof s.radii);

  if (bw < 0) {
    f32 t[4];

    for (u32 i = 0; i < 4; i++) {
      t[i] = q->uv[i] == 0 ? -1 : q->uv[i];
    }

    s.border = -1;
    s.ihx = fmaxf((q->rect[2] - (t[3] + t[1])) * 0.5f, 0);
    s.ihy = fmaxf((q->rect[3] - (t[0] + t[2])) * 0.5f, 0);
    s.icx = q->rect[0] + t[3] + s.ihx;
    s.icy = q->rect[1] + t[0] + s.ihy;
    s.no_inside = s.ihx <= 0 || s.ihy <= 0;

    f32 wider[4] = {fmaxf(t[0], t[3]), fmaxf(t[0], t[1]), fmaxf(t[2], t[1]),
                    fmaxf(t[2], t[3])};

    for (u32 i = 0; i < 4; i++) {
      s.iradii[i] = fmaxf(q->radii[i] - wider[i], 0);
    }
  }
  else if (bw > 0) {
    s.border = 1;
    s.ihx = fmaxf(s.hx - bw, 0);
    s.ihy = fmaxf(s.hy - bw, 0);

    for (u32 i = 0; i < 4; i++) {
      s.iradii[i] = fmaxf(q->radii[i] - bw, 0);
    }
  }

  return s;
}

// `a` where `mask` is set, `b` elsewhere.
static inline __m128 pick(__m128 mask, __m128 a, __m128 b) {
  return _mm_or_ps(_mm_and_ps(mask, a), _mm_andnot_ps(mask, b));
}

static inline __m128 abs4(__m128 v) {
  return _mm_andnot_ps(_mm_set1_ps(-0.0f), v);
}

// sd_rounded at four points.
static inline __m128 sd_rounded4(__m128 px, __m128 py, f32 hx, f32 hy,
                                 const f32 radii[4]) {
  __m128 zero = _mm_setzero_ps();
  __m128 right = _mm_cmpgt_ps(px, zero), below = _mm_cmpgt_ps(py, zero);
  __m128 top = pick(right, _mm_set1_ps(radii[1]), _mm_set1_ps(radii[0]));
  __m128 bottom = pick(right, _mm_set1_ps(radii[2]), _mm_set1_ps(radii[3]));
  __m128 r = _mm_min_ps(pick(below, bottom, top), _mm_set1_ps(fminf(hx, hy)));
  __m128 qx = _mm_add_ps(_mm_sub_ps(abs4(px), _mm_set1_ps(hx)), r);
  __m128 qy = _mm_add_ps(_mm_sub_ps(abs4(py), _mm_set1_ps(hy)), r);
  __m128 mx = _mm_max_ps(qx, zero), my = _mm_max_ps(qy, zero);
  __m128 len = _mm_sqrt_ps(_mm_add_ps(_mm_mul_ps(mx, mx), _mm_mul_ps(my, my)));

  return _mm_sub_ps(_mm_add_ps(_mm_min_ps(_mm_max_ps(qx, qy), zero), len), r);
}

static inline __m128 coverage4(__m128 sd) {
  return _mm_min_ps(_mm_max_ps(_mm_sub_ps(_mm_set1_ps(0.5f), sd),
                               _mm_setzero_ps()),
                    _mm_set1_ps(1));
}
#endif

// pixels x0 to x1 of a row of a rect, each asked: four at a time with neon.
static void rect_run(const Raster *r, const civue_quad *q, const void *shape,
                     i32 y, i32 x0, i32 x1) {
  u8 *d = pixel(r->s, x0, y);
  i32 x = x0;

#ifdef __ARM_NEON
  const RectShape *s = shape;

  if (s) {
    f32 fy = (f32)y + 0.5f;
    float32x4_t py = vdupq_n_f32(fy - q->rect[1] - s->hy);
    float32x4_t iy = vdupq_n_f32(fy - s->icy);

    for (; x + 4 <= x1; x += 4, d += 16) {
      float32x4_t fx =
        vaddq_f32(vdupq_n_f32((f32)x), (float32x4_t){0.5f, 1.5f, 2.5f, 3.5f});
      float32x4_t px =
        vsubq_f32(vsubq_f32(fx, vdupq_n_f32(q->rect[0])), vdupq_n_f32(s->hx));
      float32x4_t outer =
        coverage4(sd_rounded4(px, py, s->hx, s->hy, s->radii));
      float32x4_t inner = vdupq_n_f32(1);

      if (s->border < 0) {
        inner = s->no_inside
                  ? vdupq_n_f32(0)
                  : coverage4(sd_rounded4(vsubq_f32(fx, vdupq_n_f32(s->icx)),
                                          iy, s->ihx, s->ihy, s->iradii));
      }
      else if (s->border > 0) {
        inner = coverage4(sd_rounded4(px, py, s->ihx, s->ihy, s->iradii));
      }

      f32 o[4], in[4];
      vst1q_f32(o, outer);
      vst1q_f32(in, inner);

      for (u32 i = 0; i < 4; i++) {
        // nothing over the pixel leaves it as it was.
        if (o[i] == 0) {
          continue;
        }

        float32x4_t c =
          s->border ? vmulq_n_f32(vaddq_f32(vmulq_n_f32(s->edge, 1 - in[i]),
                                            vmulq_n_f32(s->color, in[i])),
                                  o[i])
                    : vmulq_n_f32(s->color, o[i]);

        blend_bgra(d + i * 4, c);
      }
    }
  }
#elif defined(__SSE2__)
  const RectShape *s = shape;

  if (s) {
    f32 fy = (f32)y + 0.5f;
    __m128 py = _mm_set1_ps(fy - q->rect[1] - s->hy);
    __m128 iy = _mm_set1_ps(fy - s->icy);

    for (; x + 4 <= x1; x += 4, d += 16) {
      __m128 fx =
        _mm_add_ps(_mm_set1_ps((f32)x), _mm_setr_ps(0.5f, 1.5f, 2.5f, 3.5f));
      __m128 px =
        _mm_sub_ps(_mm_sub_ps(fx, _mm_set1_ps(q->rect[0])), _mm_set1_ps(s->hx));
      __m128 outer = coverage4(sd_rounded4(px, py, s->hx, s->hy, s->radii));
      __m128 inner = _mm_set1_ps(1);

      // nothing over the four leaves them as they were.
      if (!_mm_movemask_ps(_mm_cmpneq_ps(outer, _mm_setzero_ps()))) {
        continue;
      }

      if (s->border < 0) {
        inner = s->no_inside
                  ? _mm_setzero_ps()
                  : coverage4(sd_rounded4(_mm_sub_ps(fx, _mm_set1_ps(s->icx)),
                                          iy, s->ihx, s->ihy, s->iradii));
      }
      else if (s->border > 0) {
        inner = coverage4(sd_rounded4(px, py, s->ihx, s->ihy, s->iradii));
      }

      f32 o[4], in[4];
      _mm_storeu_ps(o, outer);
      _mm_storeu_ps(in, inner);

      for (u32 i = 0; i < 4; i++) {
        // nothing over the pixel leaves it as it was.
        if (o[i] == 0) {
          continue;
        }

        __m128 c = s->border
                     ? _mm_mul_ps(
                         _mm_add_ps(_mm_mul_ps(s->edge, _mm_set1_ps(1 - in[i])),
                                    _mm_mul_ps(s->color, _mm_set1_ps(in[i]))),
                         _mm_set1_ps(o[i]))
                     : _mm_mul_ps(s->color, _mm_set1_ps(o[i]));

        blend_bgra(d + i * 4, c);
      }
    }
  }
#else
  (void)shape;
#endif

  for (; x < x1; x++, d += 4) {
    f32 c[4];
    shade_rect(q, (f32)x + 0.5f, (f32)y + 0.5f, c);
    blend(d, c, r->blending);
  }
}

static void whole_span(const civue_quad *q, f32 cy, i32 *from, i32 *to);

static void rect_quad(const Raster *r, const civue_quad *q, civue_box b) {
  Flat f = flat(q);
#ifdef __SSE2__
  // a fill of this many pixels is larger than the caches near a core: what
  // it writes is on its way out of them before the fill ends, and an opaque
  // rect that large is written past them.
  constexpr i64 FAR_PIXELS = 1 << 17;
  bool far = !r->blending && q->color[3] >= 1 && area(b) >= FAR_PIXELS;
#else
  constexpr bool far = false;
#endif
  u32 far_color = 0;
#if defined(__ARM_NEON) || defined(__SSE2__)
  // pixel grids and multiplying are asked a pixel at a time.
  RectShape shape;
  const void *vector = nullptr;

  if (q->params[2] <= 0.5f && !r->blending) {
    shape = rect_shape(q);
    vector = &shape;
  }
#else
  const void *vector = nullptr;
#endif
  f32 inside[4];
  // its color as it is where the rect is whole, whatever was there.
  bool has_inside = opaque_rect(q);

  if (has_inside) {
    memcpy(inside, q->color, sizeof inside);

    u8 p[4] = {byte(inside[2] * 255), byte(inside[1] * 255),
               byte(inside[0] * 255), 255};
    memcpy(&far_color, p, sizeof far_color);
  }

  for (i32 y = b.y0; y < b.y1; y++) {
    bool whole = (f32)y >= f.whole[1] && (f32)y < f.whole[3];
    f32 from = whole ? f.whole[0] : f.mid[0];
    f32 to = whole ? f.whole[2] : f.mid[2];
    i32 a = (i32)clampf(from, (f32)b.x0, (f32)b.x1);
    i32 e = (i32)clampf(to, (f32)a, (f32)b.x1);

    // a row through the curves of a rect without a border, a circle's
    // all of them, is whole between them.
    if (!whole && q->params[0] == 0 && q->params[2] <= 0.5f) {
      i32 wa, we;

      whole_span(q, (f32)y + 0.5f, &wa, &we);
      wa = MAX(wa, b.x0);
      we = MIN(we, b.x1);

      if (we - wa > e - a) {
        rect_run(r, q, vector, y, b.x0, wa);
        span(r, y, wa, we, q->color);
        rect_run(r, q, vector, y, we, b.x1);
        continue;
      }
    }

    rect_run(r, q, vector, y, b.x0, a);

    if (a < e) {
      f32 c[4];

      if (y + AHEAD < b.y1 && !far) {
        ask_row(r, (civue_box){a, y, e, y + 1}, y + AHEAD);
      }

      if (!whole) {
        shade_rect(q, from + 0.5f, (f32)y + 0.5f, c);
        span(r, y, a, e, c);
      }
      else {
        if (!has_inside) {
          shade_rect(q, from + 0.5f, (f32)y + 0.5f, inside);
          has_inside = true;

          u8 p[4] = {byte(inside[2] * 255), byte(inside[1] * 255),
                     byte(inside[0] * 255), 255};
          memcpy(&far_color, p, sizeof far_color);
        }

        if (far && inside[3] >= 1) {
          fill_far(pixel(r->s, a, y), (u32)(e - a), far_color);
        }
        else {
          span(r, y, a, e, inside);
        }
      }
    }

    rect_run(r, q, vector, y, e, b.x1);
  }
}

// whether a quad hides all that was drawn in `b` before it.
static bool hides(const civue_quad *q, civue_box b) {
  if (!is_flat(q) || q->color[3] < 1) {
    return false;
  }

  if (q->clip[0] > (f32)b.x0 || q->clip[1] > (f32)b.y0 ||
      q->clip[2] < (f32)b.x1 || q->clip[3] < (f32)b.y1) {
    return false;
  }

  Flat f = flat(q);

  return f.whole[0] <= (f32)b.x0 && f.whole[1] <= (f32)b.y0 &&
         f.whole[2] >= (f32)b.x1 && f.whole[3] >= (f32)b.y1;
}

static bool whole(f32 v) {
  return v == floorf(v);
}

// whether a glyph's quad shows its texels one for a pixel, and which ones.
static bool one_to_one(const civue_quad *q, i32 *u, i32 *v) {
  f32 s = CIVUE_ATLAS_SIZE;
  f32 u0 = q->uv[0] * s, v0 = q->uv[1] * s;

  if (turned(q) || !whole(q->rect[0]) || !whole(q->rect[1]) || !whole(u0) ||
      !whole(v0)) {
    return false;
  }

  if (q->uv[2] * s - u0 != q->rect[2] || q->uv[3] * s - v0 != q->rect[3]) {
    return false;
  }

  *u = (i32)u0 - (i32)q->rect[0];
  *v = (i32)v0 - (i32)q->rect[1];
  return true;
}

// x over 255, rounded, for x up to 255 * 255.
static u32 div255(u32 x) {
  x += 128;
  return (x + (x >> 8)) >> 8;
}

// the coverage a gamma makes of each coverage, the last few kept.
static const u8 *gamma_table(f32 gamma) {
  constexpr u32 KEPT = 4;

  static thread_local struct {
    f32 gamma;
    bool made;
    u8 v[256];
  } tables[KEPT];

  static thread_local u32 next;

  for (u32 i = 0; i < KEPT; i++) {
    if (tables[i].made && tables[i].gamma == gamma) {
      return tables[i].v;
    }
  }

  u32 at = next++ % KEPT;
  tables[at].gamma = gamma;
  tables[at].made = true;

  for (u32 i = 0; i < 256; i++) {
    tables[at].v[i] = byte(powf((f32)i / 255.0f, gamma) * 255);
  }

  return tables[at].v;
}

#ifdef __ARM_NEON
// x over 255, rounded, for each x up to 65279.
static uint8x8_t div255_x8(uint16x8_t x) {
  return vrshrn_n_u16(vrsraq_n_u16(x, x, 8), 8);
}

// the first n of 16 lanes.
static uint8x16_t first_lanes(u32 n) {
  static const u8 lanes[32] = {255, 255, 255, 255, 255, 255, 255, 255,
                               255, 255, 255, 255, 255, 255, 255, 255};

  return vld1q_u8(lanes + 16 - n);
}

// what tints a glyph: the gamma's table, 64 entries to a lookup, and the
// color. kept in registers over a glyph's rows.
#define TINT(table, c, is_plain)                                      \
  const uint8x16x4_t tint_0 = vld1q_u8_x4(table);                     \
  const uint8x16x4_t tint_1 = vld1q_u8_x4(table + 64);                \
  const uint8x16x4_t tint_2 = vld1q_u8_x4(table + 128);               \
  const uint8x16x4_t tint_3 = vld1q_u8_x4(table + 192);               \
  const bool tint_plain = is_plain;                                   \
  const uint8x8_t tint_b = vdup_n_u8(c[0]), tint_g = vdup_n_u8(c[1]), \
                  tint_r = vdup_n_u8(c[2]), tint_a = vdup_n_u8(c[3])

// a channel of sixteen pixels under coverage `lo` and `hi`.
static inline uint8x16_t mask_channel(uint8x16_t d, uint8x8_t tint,
                                      uint8x8_t lo, uint8x8_t hi,
                                      uint8x8_t inv_lo, uint8x8_t inv_hi) {
  uint16x8_t low = vmlal_u8(vmull_u8(tint, lo), vget_low_u8(d), inv_lo);
  uint16x8_t high = vmlal_u8(vmull_u8(tint, hi), vget_high_u8(d), inv_hi);

  return vcombine_u8(div255_x8(low), div255_x8(high));
}

// sixteen pixels of coverage `m` tinted over `d`. a lookup gives zero past
// its 64 entries.
#define MASK_X16(d, m)                                                      \
  do {                                                                      \
    uint8x16_t a_ = (m);                                                    \
    if (!tint_plain) {                                                      \
      uint8x16_t m_ = a_;                                                   \
      a_ = vqtbl4q_u8(tint_0, m_);                                          \
      a_ = vorrq_u8(a_, vqtbl4q_u8(tint_1, vsubq_u8(m_, vdupq_n_u8(64))));  \
      a_ = vorrq_u8(a_, vqtbl4q_u8(tint_2, vsubq_u8(m_, vdupq_n_u8(128)))); \
      a_ = vorrq_u8(a_, vqtbl4q_u8(tint_3, vsubq_u8(m_, vdupq_n_u8(192)))); \
    }                                                                       \
    uint8x16x4_t px_ = vld4q_u8(d);                                         \
    uint8x8_t lo_ = vget_low_u8(a_), hi_ = vget_high_u8(a_);                \
    uint8x8_t il_ = vmvn_u8(div255_x8(vmull_u8(tint_a, lo_)));              \
    uint8x8_t ih_ = vmvn_u8(div255_x8(vmull_u8(tint_a, hi_)));              \
    px_.val[0] = mask_channel(px_.val[0], tint_b, lo_, hi_, il_, ih_);      \
    px_.val[1] = mask_channel(px_.val[1], tint_g, lo_, hi_, il_, ih_);      \
    px_.val[2] = mask_channel(px_.val[2], tint_r, lo_, hi_, il_, ih_);      \
    px_.val[3] = mask_channel(px_.val[3], tint_a, lo_, hi_, il_, ih_);      \
    vst4q_u8(d, px_);                                                       \
  } while (0)

static bool none_x16(uint8x16_t m) {
  return !(vgetq_lane_u64(vreinterpretq_u64_u8(m), 0) |
           vgetq_lane_u64(vreinterpretq_u64_u8(m), 1));
}
#endif

#if defined(__SSE2__) && !defined(__ARM_NEON)
// x over 255, rounded, for each x up to 65279, in sixteen bits.
static inline __m128i div255_x8(__m128i x) {
  x = _mm_add_epi16(x, _mm_set1_epi16(128));
  return _mm_srli_epi16(_mm_add_epi16(x, _mm_srli_epi16(x, 8)), 8);
}

// how many pixels of coverage are tinted at a time.
#ifdef __AVX2__
constexpr u32 MASK_STEP = 8;

static inline __m256i div255_x16(__m256i x) {
  x = _mm256_add_epi16(x, _mm256_set1_epi16(128));
  return _mm256_srli_epi16(_mm256_add_epi16(x, _mm256_srli_epi16(x, 8)), 8);
}

// eight pixels of coverage `m`, a byte each, tinted `tint` over `d`: mask
// pixel's own math, sixteen bits a channel. `tint` is the color blue first
// four times over, and `alpha` its alpha in every lane.
static inline void mask_step(u8 *d, u64 m, __m256i tint, __m256i alpha) {
  __m256i zero = _mm256_setzero_si256();
  // each coverage four times, a channel each.
  __m128i twice = _mm_cvtsi64_si128((i64)m);

  twice = _mm_unpacklo_epi8(twice, twice);

  __m256i a = _mm256_set_m128i(_mm_unpackhi_epi16(twice, twice),
                               _mm_unpacklo_epi16(twice, twice));
  __m256i px = _mm256_loadu_si256((const __m256i *)(const void *)d);
  __m256i half[2] = {_mm256_unpacklo_epi8(px, zero),
                     _mm256_unpackhi_epi8(px, zero)};
  __m256i cover[2] = {_mm256_unpacklo_epi8(a, zero),
                      _mm256_unpackhi_epi8(a, zero)};

  for (u32 h = 0; h < 2; h++) {
    __m256i inv = _mm256_sub_epi16(
      _mm256_set1_epi16(255),
      div255_x16(_mm256_mullo_epi16(alpha, cover[h])));

    half[h] = div255_x16(_mm256_add_epi16(_mm256_mullo_epi16(tint, cover[h]),
                                          _mm256_mullo_epi16(half[h], inv)));
  }

  _mm256_storeu_si256((__m256i *)(void *)d,
                      _mm256_packus_epi16(half[0], half[1]));
}

#define MASK_TINT(c)                                                        \
  const __m256i tint_color = _mm256_setr_epi16(                             \
    c[0], c[1], c[2], c[3], c[0], c[1], c[2], c[3], c[0], c[1], c[2], c[3], \
    c[0], c[1], c[2], c[3]);                                                \
  const __m256i tint_alpha = _mm256_set1_epi16(c[3])
#else
constexpr u32 MASK_STEP = 4;

// four pixels of coverage, as above.
static inline void mask_step(u8 *d, u64 m, __m128i tint, __m128i alpha) {
  __m128i zero = _mm_setzero_si128();
  __m128i a = _mm_cvtsi32_si128((i32)(u32)m);

  a = _mm_unpacklo_epi8(a, a);
  a = _mm_unpacklo_epi16(a, a);

  __m128i px = _mm_loadu_si128((const __m128i *)(const void *)d);
  __m128i half[2] = {_mm_unpacklo_epi8(px, zero), _mm_unpackhi_epi8(px, zero)};
  __m128i cover[2] = {_mm_unpacklo_epi8(a, zero), _mm_unpackhi_epi8(a, zero)};

  for (u32 h = 0; h < 2; h++) {
    __m128i inv = _mm_sub_epi16(_mm_set1_epi16(255),
                                div255_x8(_mm_mullo_epi16(alpha, cover[h])));

    half[h] = div255_x8(_mm_add_epi16(_mm_mullo_epi16(tint, cover[h]),
                                      _mm_mullo_epi16(half[h], inv)));
  }

  _mm_storeu_si128((__m128i *)(void *)d, _mm_packus_epi16(half[0], half[1]));
}

#define MASK_TINT(c)                                              \
  const __m128i tint_color =                                      \
    _mm_setr_epi16(c[0], c[1], c[2], c[3], c[0], c[1], c[2], c[3]); \
  const __m128i tint_alpha = _mm_set1_epi16(c[3])
#endif

// the coverage of MASK_STEP pixels read at once, and what the gamma makes
// of each.
static inline u64 mask_read(const u8 *m) {
  u64 v = 0;

  memcpy(&v, m, MASK_STEP);
  return v;
}

static inline u64 mask_gamma(u64 m, const u8 *table) {
  // nothing and all of a pixel stay what they are whatever the gamma, and
  // are most of what is not a glyph's edge.
  u64 full = ((m >> 7) & 0x0101010101010101u) * 0xff;

  if (m == full) {
    return m;
  }

  u64 v = 0;

  for (u32 k = 0; k < MASK_STEP; k++) {
    v |= (u64)table[(m >> (k * 8)) & 0xff] << (k * 8);
  }

  return v;
}
#endif

// a pixel of coverage `a` tinted `c`, blue first, over `p`.
static void mask_pixel(u8 *p, u32 a, const u8 c[4]) {
  u32 inv = 255 - div255(c[3] * a);

  for (u32 k = 0; k < 4; k++) {
    p[k] = (u8)div255(c[k] * a + p[k] * inv);
  }
}

static void mask_quad(const Raster *r, const civue_quad *q, civue_box b) {
  i32 u, v;

  if (!r->mask || r->blending || !one_to_one(q, &u, &v)) {
    each(r, q, b);
    return;
  }

  f32 gamma = q->params[2];
  const u8 *table = gamma_table(gamma);
  u8 c[4] = {byte(q->color[2] * 255), byte(q->color[1] * 255),
             byte(q->color[0] * 255), byte(q->color[3] * 255)};
  u32 n = (u32)(b.x1 - b.x0);

#ifdef __ARM_NEON
  TINT(table, c, gamma == 1);
  // sixteen pixels are read and written at a time, the ones past the row
  // left as they are: only where the surface and the atlas have them.
  bool wide = b.x0 + ((n + 15) & ~15u) <= r->s->width &&
              b.x0 + u + ((n + 15) & ~15u) <= CIVUE_ATLAS_SIZE;
#elif defined(__SSE2__)
  MASK_TINT(c);
  bool plain = gamma == 1;
  u32 whole_steps = (n + MASK_STEP - 1) & ~(MASK_STEP - 1);
  // a step is read and written whole, the pixels past the row left as they
  // are: only where the surface and the atlas have them.
  bool wide = b.x0 + whole_steps <= r->s->width &&
              b.x0 + u + whole_steps <= CIVUE_ATLAS_SIZE;
#endif

  for (i32 y = b.y0; y < b.y1; y++) {
    const u8 *m = r->mask + (usize)(y + v) * CIVUE_ATLAS_SIZE + (b.x0 + u);
    u8 *d = pixel(r->s, b.x0, y);
    u32 i = 0;

#ifdef __ARM_NEON
    for (; i + 16 <= n; i += 16) {
      uint8x16_t lanes = vld1q_u8(m + i);

      if (!none_x16(lanes)) {
        MASK_X16(d + i * 4, lanes);
      }
    }

    if (i < n && wide) {
      uint8x16_t lanes = vandq_u8(vld1q_u8(m + i), first_lanes(n - i));

      if (!none_x16(lanes)) {
        MASK_X16(d + i * 4, lanes);
      }

      continue;
    }
#elif defined(__SSE2__)
    for (; i + MASK_STEP <= n; i += MASK_STEP) {
      u64 lanes = mask_read(m + i);

      // most of a glyph's box is nothing.
      if (lanes) {
        mask_step(d + i * 4, plain ? lanes : mask_gamma(lanes, table),
                  tint_color, tint_alpha);
      }
    }

    if (i < n && wide) {
      // the pixels past the row have no coverage, and are left as they are.
      u64 lanes = mask_read(m + i) & (((u64)1 << ((n - i) * 8)) - 1);

      if (lanes) {
        mask_step(d + i * 4, plain ? lanes : mask_gamma(lanes, table),
                  tint_color, tint_alpha);
      }

      continue;
    }
#endif

    for (; i < n; i++) {
      u32 a = table[m[i]];

      if (a) {
        mask_pixel(d + i * 4, a, c);
      }
    }
  }
}

static void color_quad(const Raster *r, const civue_quad *q, civue_box b) {
  i32 u, v;

  if (!r->color || r->blending || !one_to_one(q, &u, &v)) {
    each(r, q, b);
    return;
  }

  u32 opacity = byte(q->color[3] * 255);

  for (i32 y = b.y0; y < b.y1; y++) {
    const u8 *t =
      r->color + ((usize)(y + v) * CIVUE_ATLAS_SIZE + (b.x0 + u)) * 4;
    u8 *d = pixel(r->s, b.x0, y);
    i32 x = b.x0;

#if defined(__SSE2__) && !defined(__ARM_NEON)
    // four pixels at a time, sixteen bits a channel: the math below.
    __m128i zero = _mm_setzero_si128(), by = _mm_set1_epi16((i16)opacity);
    __m128i full = _mm_set1_epi16(255), even = _mm_set1_epi32(0x00ff00ff);

    for (; x + 4 <= b.x1; x += 4, t += 16, d += 16) {
      __m128i texels = _mm_loadu_si128((const __m128i *)(const void *)t);

      // most of an emoji's box is nothing.
      if (_mm_movemask_epi8(_mm_cmpeq_epi8(texels, zero)) == 0xffff) {
        continue;
      }

      // red and blue change places.
      __m128i ends = _mm_and_si128(texels, even);

      texels = _mm_or_si128(
        _mm_andnot_si128(even, texels),
        _mm_or_si128(_mm_slli_epi32(ends, 16), _mm_srli_epi32(ends, 16)));

      __m128i px = _mm_loadu_si128((const __m128i *)(const void *)d);
      __m128i src[2] = {_mm_unpacklo_epi8(texels, zero),
                        _mm_unpackhi_epi8(texels, zero)};
      __m128i dst[2] = {_mm_unpacklo_epi8(px, zero),
                        _mm_unpackhi_epi8(px, zero)};

      for (u32 h = 0; h < 2; h++) {
        __m128i over = div255_x8(_mm_mullo_epi16(src[h], by));
        // each pixel's alpha in its four lanes.
        __m128i alpha =
          _mm_shufflehi_epi16(_mm_shufflelo_epi16(over, 0xff), 0xff);
        __m128i under =
          div255_x8(_mm_mullo_epi16(dst[h], _mm_sub_epi16(full, alpha)));

        dst[h] = _mm_add_epi16(over, under);
      }

      _mm_storeu_si128((__m128i *)(void *)d,
                       _mm_packus_epi16(dst[0], dst[1]));
    }
#endif

    for (; x < b.x1; x++, t += 4, d += 4) {
      // the atlas has red first, the pixels blue.
      u32 s[4] = {div255(t[2] * opacity), div255(t[1] * opacity),
                  div255(t[0] * opacity), div255(t[3] * opacity)};
      u32 inv = 255 - s[3];

      for (u32 k = 0; k < 4; k++) {
        u32 w = s[k] + div255(d[k] * inv);
        d[k] = (u8)MIN(w, 255u);
      }
    }
  }
}

// where a shadow needs no pixel asked on its own: between x[0] and x[1] a
// row is one value, its ends' falloff four sigmas away, and from y[0] to
// y[1] the rows are far enough from the corners to fall off alike.
typedef struct {
  f32 x[2], y[2];
} Soft;

static Soft soft(const civue_quad *q) {
  f32 sigma = fmaxf(q->params[2], 0.01f), corner = q->radii[0];
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  f32 cx = q->rect[0] + hx, cy = q->rect[1] + hy;
  f32 mx = hx - corner - 4 * sigma, my = hy - corner - 3 * sigma;

  return (Soft){{cx - mx, cx + mx}, {cy - my, cy + my}};
}

// erf of four values, the shader's approximation.
static void erf_x4(const f32 x[4], f32 out[4]) {
#ifdef __ARM_NEON
  float32x4_t v = vld1q_f32(x), a = vabsq_f32(v), one = vdupq_n_f32(1);
  float32x4_t r =
    vmlaq_f32(vdupq_n_f32(0.230389f), vdupq_n_f32(0.078108f), vmulq_f32(a, a));

  r = vmlaq_f32(vdupq_n_f32(0.278393f), r, a);
  r = vmlaq_f32(one, r, a);
  r = vmulq_f32(r, r);
  r = vmulq_f32(r, r);

  float32x4_t m = vsubq_f32(one, vdivq_f32(one, r));
  // the sign of x.
  vst1q_f32(out, vbslq_f32(vdupq_n_u32(0x80000000u), v, m));
#elif defined(__SSE2__)
  __m128 sign = _mm_set1_ps(-0.0f), one = _mm_set1_ps(1);
  __m128 v = _mm_loadu_ps(x), a = _mm_andnot_ps(sign, v);
  __m128 r = _mm_add_ps(_mm_set1_ps(0.230389f),
                        _mm_mul_ps(_mm_set1_ps(0.078108f), _mm_mul_ps(a, a)));

  r = _mm_add_ps(_mm_set1_ps(0.278393f), _mm_mul_ps(r, a));
  r = _mm_add_ps(one, _mm_mul_ps(r, a));
  r = _mm_mul_ps(r, r);
  r = _mm_mul_ps(r, r);

  __m128 m = _mm_sub_ps(one, _mm_div_ps(one, r));
  // the sign of x, and nothing at zero as erf_of has it.
  m = _mm_or_ps(m, _mm_and_ps(sign, v));
  _mm_storeu_ps(out, _mm_and_ps(m, _mm_cmpneq_ps(v, _mm_setzero_ps())));
#else
  for (u32 i = 0; i < 4; i++) {
    out[i] = erf_of(x[i]);
  }
#endif
}

// a row of a shadow: what each of the shader's four samples weighs and how
// far its edge is from the center.
typedef struct {
  f32 weight[4], curved[4];
  f32 k, px0;
  bool straight;
} SoftRow;

static SoftRow soft_row(const civue_quad *q, f32 y) {
  f32 sigma = fmaxf(q->params[2], 0.01f), corner = q->radii[0];
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  f32 py = y - q->rect[1] - hy;
  f32 low = py - hy, high = py + hy;
  f32 start = clampf(-3 * sigma, low, high);
  f32 end = clampf(3 * sigma, low, high);
  f32 step = (end - start) / 4;
  f32 at = start + step * 0.5f;
  SoftRow row = {
    .k = 0.70710678f / sigma, .px0 = q->rect[0] + hx, .straight = true};

  for (u32 i = 0; i < 4; i++) {
    f32 delta = fminf(hy - corner - fabsf(py - at), 0);

    row.weight[i] = gaussian(at, sigma) * step;
    row.curved[i] =
      hx - corner + sqrtf(fmaxf(0, corner * corner - delta * delta));
    row.straight = row.straight && delta == 0;
    at += step;
  }

  return row;
}

// the shadow at `x` of a row.
static f32 soft_at(const SoftRow *row, f32 x) {
  f32 px = x - row->px0;
  f32 from[4], to[4], low[4], high[4];

  for (u32 i = 0; i < 4; i++) {
    from[i] = (px - row->curved[i]) * row->k;
    to[i] = (px + row->curved[i]) * row->k;
  }

  erf_x4(from, low);
  erf_x4(to, high);

  f32 v = 0;

  for (u32 i = 0; i < 4; i++) {
    v += 0.5f * (high[i] - low[i]) * row->weight[i];
  }

  return v;
}

// a shadow: one value between a row's ends, and rows far from the corners
// fall off the same way at theirs, found once.
static void shadow_quad(const Raster *r, const civue_quad *q, civue_box b) {
  constexpr u32 KEPT = 512;
  Soft s = soft(q);
  i32 a = (i32)clampf(ceilf(s.x[0] - 0.5f), (f32)b.x0, (f32)b.x1);
  i32 e = (i32)clampf(ceilf(s.x[1] - 0.5f), (f32)a, (f32)b.x1);
  u32 ends = (u32)(a - b.x0) + (u32)(b.x1 - e);
  // the falloff of a straight row at each pixel of its ends, over its
  // weights.
  f32 falloff[KEPT];
  bool has_falloff = false;

  for (i32 y = b.y0; y < b.y1; y++) {
    SoftRow row = soft_row(q, (f32)y + 0.5f);
    f32 total = row.weight[0] + row.weight[1] + row.weight[2] + row.weight[3];
    bool kept = row.straight && ends <= KEPT && total > 0;

    if (kept && !has_falloff) {
      u32 n = 0;

      for (i32 x = b.x0; x < a; x++) {
        falloff[n++] = soft_at(&row, (f32)x + 0.5f) / total;
      }

      for (i32 x = e; x < b.x1; x++) {
        falloff[n++] = soft_at(&row, (f32)x + 0.5f) / total;
      }

      has_falloff = true;
    }

    u8 *d = pixel(r->s, b.x0, y);
    u32 n = 0;

    for (i32 x = b.x0; x < a; x++, d += 4) {
      f32 c[4];
      f32 v = kept ? falloff[n++] * total : soft_at(&row, (f32)x + 0.5f);

      scaled(q->color, v, c);
      blend(d, c, r->blending);
    }

    if (a < e) {
      f32 c[4];

      scaled(q->color, soft_at(&row, row.px0), c);
      span(r, y, a, e, c);
    }

    d = pixel(r->s, e, y);

    for (i32 x = e; x < b.x1; x++, d += 4) {
      f32 c[4];
      f32 v = kept ? falloff[n++] * total : soft_at(&row, (f32)x + 0.5f);

      scaled(q->color, v, c);
      blend(d, c, r->blending);
    }
  }
}

// the pixels of `b` a rounded clip may cut, at most one box a corner and
// none over another. the others are the rect clip's alone.
static u32 corner_boxes(const civue_quad *q, civue_box b, civue_box out[4]) {
  f32 hx = q->round[2] * 0.5f, hy = q->round[3] * 0.5f;
  f32 cx = q->round[0] + hx, cy = q->round[1] + hy;
  f32 x0 = q->round[0], y0 = q->round[1];
  f32 x1 = x0 + q->round[2], y1 = y0 + q->round[3];
  // the pixels whose center is left of the middle, and above it.
  i32 sx = (i32)ceilf(cx - 0.5f), sy = (i32)ceilf(cy - 0.5f);
  u32 n = 0;

  // top left, top right, bottom right, bottom left, a pixel to spare.
  for (u32 i = 0; i < 4; i++) {
    f32 r = fminf(q->round_radii[i], fminf(hx, hy));

    if (!(r > 0)) {
      continue;
    }

    bool right = i == 1 || i == 2, bottom = i >= 2;
    civue_box c = {
      right ? MAX((i32)floorf(x1 - r - 0.5f), sx) : b.x0,
      bottom ? MAX((i32)floorf(y1 - r - 0.5f), sy) : b.y0,
      right ? b.x1 : MIN((i32)ceilf(x0 + r - 0.5f) + 1, sx),
      bottom ? b.y1 : MIN((i32)ceilf(y0 + r - 0.5f) + 1, sy),
    };

    c = cut(c, b);

    if (!empty(c)) {
      out[n++] = c;
    }
  }

  return n;
}

// what of `b` is not in `c`, in at most four boxes.
static u32 without(civue_box b, civue_box c, civue_box out[4]) {
  civue_box in = cut(b, c);
  u32 n = 0;

  if (empty(in)) {
    out[0] = b;
    return 1;
  }

  civue_box parts[4] = {
    {b.x0, b.y0, b.x1, in.y0},
    {b.x0, in.y1, b.x1, b.y1},
    {b.x0, in.y0, in.x0, in.y1},
    {in.x1, in.y0, b.x1, in.y1},
  };

  for (u32 i = 0; i < 4; i++) {
    if (!empty(parts[i])) {
      out[n++] = parts[i];
    }
  }

  return n;
}

static void plain_quad(const Raster *r, const civue_quad *q, civue_box b);

static void quad(const Raster *r, const civue_quad *q) {
  if (turned(q)) {
    each_turned(r, q);
    return;
  }

  civue_box b = bounds(q, r->box);

  if (empty(b)) {
    return;
  }

  if (!rounded(q)) {
    plain_quad(r, q, b);
    return;
  }

  // rounded off, the pixels at its corners are each asked, and the rest
  // draws as it would unrounded.
  civue_box corners[4], rest[64];
  u32 ncorners = corner_boxes(q, b, corners), nrest = 1;

  rest[0] = b;

  for (u32 i = 0; i < ncorners; i++) {
    each(r, q, corners[i]);

    civue_box next[64];
    u32 n = 0;

    for (u32 k = 0; k < nrest; k++) {
      n += without(rest[k], corners[i], next + n);
    }

    memcpy(rest, next, n * sizeof *next);
    nrest = n;
  }

  for (u32 k = 0; k < nrest; k++) {
    plain_quad(r, q, rest[k]);
  }
}

// whether an image's quad shows its texels one for a pixel, at whole
// pixels, and how far its texels are from the pixels they land on.
static bool image_one_to_one(const civue_pixels *im, const civue_quad *q,
                             i32 *ox, i32 *oy) {
  if (!im->pixels || !im->width || !im->height || q->params[2] > 0.5f) {
    return false;
  }

  f32 w = (f32)im->width, h = (f32)im->height;
  f32 u0 = q->uv[0] * w, v0 = q->uv[1] * h;

  if (!whole(q->rect[0]) || !whole(q->rect[1]) || !whole(u0) || !whole(v0) ||
      q->uv[2] * w - u0 != q->rect[2] || q->uv[3] * h - v0 != q->rect[3]) {
    return false;
  }

  *ox = (i32)u0 - (i32)q->rect[0];
  *oy = (i32)v0 - (i32)q->rect[1];
  return true;
}

// where a row of a rounded rect covers its pixels whole: from the first
// pixel to the one past the last, a pixel kept from each end so that what
// the rounding asks is never guessed.
static void whole_span(const civue_quad *q, f32 cy, i32 *from, i32 *to) {
  f32 x0 = q->rect[0], y0 = q->rect[1];
  f32 x1 = x0 + q->rect[2], y1 = y0 + q->rect[3];
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  f32 left = x0 + 0.5f, right = x1 - 0.5f;

  if (cy < y0 + 0.5f || cy > y1 - 0.5f) {
    *from = *to = 0;
    return;
  }

  // the corners from the top left clockwise.
  for (u32 i = 0; i < 4; i++) {
    f32 r = fminf(q->radii[i], fminf(hx, hy));
    bool bottom = i >= 2, right_side = i == 1 || i == 2;
    f32 dy = bottom ? cy - (y1 - r) : (y0 + r) - cy;

    if (!(r > 0) || dy <= 0) {
      continue;
    }

    f32 rr = r - 0.5f, k = rr * rr - dy * dy;
    f32 reach = r - (k > 0 && rr > 0 ? sqrtf(k) : 0);

    if (right_side) {
      right = fminf(right, x1 - reach);
    }
    else {
      left = fmaxf(left, x0 + reach);
    }
  }

  *from = (i32)ceilf(left - 0.5f) + 1;
  *to = (i32)floorf(right - 0.5f);
}

// an image a texel a pixel: its texels go as they are where it covers its
// pixels whole, the rest is asked a pixel at a time.
// pixels x0 to x1 of a row of an image a texel a pixel, where its rounding
// covers them in part: shade's math with the texel as it is, and nothing
// for the pixels it does not cover.
static void image_rim(const Raster *r, const civue_quad *q, const u8 *row,
                      i32 ox, i32 y, i32 x0, i32 x1) {
  const civue_pixels *im = &r->image;
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  f32 py = (f32)y + 0.5f - q->rect[1] - hy;
  bool tile = q->params[0] > 0.5f;
  u8 *d = pixel(r->s, x0, y);

  for (i32 x = x0; x < x1; x++, d += 4) {
    f32 px = (f32)x + 0.5f - q->rect[0] - hx;
    f32 cover = tile ? 1 : clamp01(0.5f - sd_rounded(px, py, hx, hy, q->radii));

    if (cover == 0) {
      continue;
    }

    const u8 *t = row + (usize)MIN(MAX(x + ox, 0), (i32)im->width - 1) * 4;
    f32 c[4] = {t[0] / 255.0f, t[1] / 255.0f, t[2] / 255.0f, t[3] / 255.0f};

    scaled(c, q->color[3], c);

    if (!tile) {
      scaled(c, cover, c);
    }

    blend(d, c, r->blending);
  }
}

// four floats at once, a channel each, where the processor has them: neon
// on arm64 and sse2 on x86. they add, multiply and divide like floats.
#if defined(__ARM_NEON) && defined(__aarch64__)
#define CIVUE_F4 1
typedef float32x4_t f4;

static inline f4 f4_of(f32 v) {
  return vdupq_n_f32(v);
}

// a texel's four bytes.
static inline f4 f4_bytes(const u8 *p) {
  u32 t;
  memcpy(&t, p, sizeof t);

  return vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(vcreate_u8(t)))));
}
#elif defined(__SSE2__)
#define CIVUE_F4 1
typedef __m128 f4;

static inline f4 f4_of(f32 v) {
  return _mm_set1_ps(v);
}

static inline f4 f4_bytes(const u8 *p) {
  u32 t;
  memcpy(&t, p, sizeof t);

  __m128i zero = _mm_setzero_si128();

  return _mm_cvtepi32_ps(_mm_unpacklo_epi16(
    _mm_unpacklo_epi8(_mm_cvtsi32_si128((i32)t), zero), zero));
}
#endif

// an image scaled: image_at's math a row at a time, and nothing for the
// pixels whose texels are all see through, which most of a frame around an
// avatar is.
static void image_scaled(const Raster *r, const civue_quad *q, civue_box b) {
  const civue_pixels *im = &r->image;
  f32 w1 = (f32)(im->width - 1), h1 = (f32)(im->height - 1);
  f32 hx = q->rect[2] * 0.5f, hy = q->rect[3] * 0.5f;
  bool nearest = q->params[3] > 0.5f, tile = q->params[0] > 0.5f;

  for (i32 y = b.y0; y < b.y1; y++) {
    f32 fy = (f32)y + 0.5f, v = image_v(q, im, fy), py = fy - q->rect[1] - hy;
    f32 vv = v - 0.5f, wy = fract(vv);
    const u8 *rows[2] = {
      im->pixels + (usize)clampf(floorf(vv), 0, h1) * im->stride,
      im->pixels + (usize)clampf(floorf(vv) + 1, 0, h1) * im->stride};
    const u8 *near = im->pixels + (usize)clampf(floorf(v), 0, h1) * im->stride;
    u8 *d = pixel(r->s, b.x0, y);
#ifdef CIVUE_F4
    // where the rounding covers the row's pixels whole, and asks nothing.
    i32 whole_from = b.x0, whole_to = b.x1;

    if (!tile) {
      whole_span(q, fy, &whole_from, &whole_to);
    }
#endif

    for (i32 x = b.x0; x < b.x1; x++, d += 4) {
      f32 fx = (f32)x + 0.5f, u = image_u(q, im, fx), c[4];

      if (nearest) {
        const u8 *p = near + (usize)clampf(floorf(u), 0, w1) * 4;
        u32 t;
        memcpy(&t, p, sizeof t);

        if (!t) {
          continue;
        }

        for (u32 i = 0; i < 4; i++) {
          c[i] = p[i] / 255.0f;
        }
      }
      else {
        f32 uu = u - 0.5f, wx = fract(uu);
        usize x0 = (usize)clampf(floorf(uu), 0, w1) * 4;
        usize x1 = (usize)clampf(floorf(uu) + 1, 0, w1) * 4;
        u32 t[4];
        memcpy(&t[0], rows[0] + x0, 4);
        memcpy(&t[1], rows[0] + x1, 4);
        memcpy(&t[2], rows[1] + x0, 4);
        memcpy(&t[3], rows[1] + x1, 4);

        if (!(t[0] | t[1] | t[2] | t[3])) {
          continue;
        }

#ifdef CIVUE_F4
        // the four channels at once: the loop below, a lane a channel.
        if (!r->blending) {
          f4 left = f4_of(1 - wx), right = f4_of(wx);
          f4 top = f4_bytes(rows[0] + x0) * left + f4_bytes(rows[0] + x1) * right;
          f4 bottom =
            f4_bytes(rows[1] + x0) * left + f4_bytes(rows[1] + x1) * right;
          f4 color = (top * f4_of(1 - wy) + bottom * f4_of(wy)) / f4_of(255.0f);

          color = color * f4_of(q->color[3]);

          if (x < whole_from || x >= whole_to) {
            f32 px = fx - q->rect[0] - hx;

            color = color *
                    f4_of(clamp01(0.5f - sd_rounded(px, py, hx, hy, q->radii)));
          }

          // red first in the image, blue first in the pixels.
          blend_bgra(d, __builtin_shufflevector(color, color, 2, 1, 0, 3));
          continue;
        }
#endif

        for (u32 i = 0; i < 4; i++) {
          f32 top = rows[0][x0 + i] * (1 - wx) + rows[0][x1 + i] * wx;
          f32 bottom = rows[1][x0 + i] * (1 - wx) + rows[1][x1 + i] * wx;

          c[i] = (top * (1 - wy) + bottom * wy) / 255.0f;
        }
      }

      scaled(c, q->color[3], c);

      if (!tile) {
        f32 px = fx - q->rect[0] - hx;
        scaled(c, clamp01(0.5f - sd_rounded(px, py, hx, hy, q->radii)), c);
      }

      blend(d, c, r->blending);
    }
  }
}

static void image_quad(const Raster *r, const civue_quad *q, civue_box b) {
  i32 ox, oy;

  if (!image_one_to_one(&r->image, q, &ox, &oy)) {
    if (r->image.pixels && r->image.width && r->image.height &&
        q->params[2] <= 0.5f) {
      image_scaled(r, q, b);
    }
    else {
      each(r, q, b);
    }

    return;
  }

  const civue_pixels *im = &r->image;
  f32 alpha = q->color[3];
  bool copy = !r->blending && alpha >= 1;

  for (i32 y = b.y0; y < b.y1; y++) {
    i32 a = b.x0, e = b.x1;

    // canvas tiles cover their pixels whole.
    if (q->params[0] <= 0.5f) {
      whole_span(q, (f32)y + 0.5f, &a, &e);
      a = MAX(a, b.x0);
      e = MIN(e, b.x1);
      e = MAX(e, a);
    }

    i32 ty = MIN(MAX(y + oy, 0), (i32)im->height - 1);
    const u8 *row = im->pixels + (usize)ty * im->stride;
    u8 *d = pixel(r->s, b.x0, y);

    image_rim(r, q, row, ox, y, b.x0, a);
    d += (a - b.x0) * 4;

    i32 x = a;

    // the texels are rgba, the pixels bgra.
    if (copy && x + ox >= 0 && e + ox <= (i32)im->width) {
#ifdef __ARM_NEON
      const uint8x16_t swap = {2,  1, 0, 3,  6,  5,  4,  7,
                               10, 9, 8, 11, 14, 13, 12, 15};

      for (; x + 4 <= e; x += 4, d += 16) {
        uint8x16_t t = vld1q_u8(row + (usize)(x + ox) * 4);

        // see through somewhere: blended a pixel at a time below.
        if (vminvq_u8(vorrq_u8(
              t, vreinterpretq_u8_u32(vdupq_n_u32(0x00ffffff)))) != 255) {
          break;
        }

        vst1q_u8(d, vqtbl1q_u8(t, swap));
      }
#elif defined(__SSE2__)
      __m128i even = _mm_set1_epi32(0x00ff00ff);
      __m128i alpha = _mm_set1_epi32((i32)0xff000000u);

      for (; x + 4 <= e; x += 4, d += 16) {
        __m128i t = _mm_loadu_si128(
          (const __m128i *)(const void *)(row + (usize)(x + ox) * 4));

        // see through somewhere: blended a pixel at a time below.
        if (_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_and_si128(t, alpha), alpha)) !=
            0xffff) {
          break;
        }

        // red and blue change places.
        __m128i ends = _mm_and_si128(t, even);

        _mm_storeu_si128(
          (__m128i *)(void *)d,
          _mm_or_si128(
            _mm_andnot_si128(even, t),
            _mm_or_si128(_mm_slli_epi32(ends, 16), _mm_srli_epi32(ends, 16))));
      }
#endif
    }

    for (; x < e; x++, d += 4) {
      const u8 *t = row + (usize)MIN(MAX(x + ox, 0), (i32)im->width - 1) * 4;

      if (copy && t[3] == 255) {
        d[0] = t[2], d[1] = t[1], d[2] = t[0], d[3] = 255;
        continue;
      }

      f32 c[4] = {t[0] / 255.0f, t[1] / 255.0f, t[2] / 255.0f, t[3] / 255.0f};

      scaled(c, alpha, c);
      blend(d, c, r->blending);
    }

    image_rim(r, q, row, ox, y, MAX(e, b.x0), b.x1);
  }
}

// a quad drawn in `b` as if it had no rounded clip.
static void plain_quad(const Raster *r, const civue_quad *q, civue_box b) {
  u32 kind = (u32)q->params[1];

  if (kind == CIVUE_QUAD_IMAGE) {
    image_quad(r, q, b);
  }
  else if (flat_rect(q)) {
    rect_quad(r, q, b);
  }
  else if (kind == CIVUE_QUAD_MASK) {
    mask_quad(r, q, b);
  }
  else if (kind == CIVUE_QUAD_COLOR_GLYPH) {
    color_quad(r, q, b);
  }
  else if (kind == CIVUE_QUAD_SHADOW) {
    shadow_quad(r, q, b);
  }
  else {
    each(r, q, b);
  }
}

// which batch a quad is in.
static u32 batch_of(const civue_draw_list *list, u32 quad, u32 from) {
  u32 b = from;

  while (b + 1 < list->batches.len && list->batches.v[b + 1].start <= quad) {
    b++;
  }

  return b;
}

// boxes to clear, gathered so that each row's memory is asked for before
// the rows of the box before are done.
typedef struct {
  const Raster *r;
  civue_box boxes[64];
  u32 len;
  // a window cleared whole is written from end to end, nothing to ask for.
  bool whole;
} Clears;

static void clear_boxes(Clears *c) {
  for (u32 i = 0; i < c->len; i++) {
    civue_box b = c->boxes[i];

    for (i32 y = b.y0; y < b.y1; y++) {
      // past its last rows, the next box's first ones.
      if (c->whole) {}
      else if (y + AHEAD < b.y1) {
        ask_row(c->r, b, y + AHEAD);
      }
      else if (i + 1 < c->len) {
        civue_box next = c->boxes[i + 1];
        i32 row = next.y0 + (y + AHEAD - b.y1);

        if (row < next.y1) {
          ask_row(c->r, next, row);
        }
      }

      if (c->whole) {
        fill_far(pixel(c->r->s, b.x0, y), (u32)(b.x1 - b.x0), c->r->clear);
      }
      else {
        fill(pixel(c->r->s, b.x0, y), (u32)(b.x1 - b.x0), c->r->clear);
      }
    }
  }

  c->len = 0;
}

static void clear_box(void *ctx, civue_box b) {
  Clears *c = ctx;

  if (c->len == countof(c->boxes)) {
    clear_boxes(c);
  }

  c->boxes[c->len++] = b;
}

// takes out of `d` the tiles an opaque rect of the list covers whole: they
// need no clearing, the rect writes every pixel of them.
static void uncovered(const Raster *r, const civue_draw_list *list,
                      civue_damage *d) {
  civue_box window = {0, 0, (i32)r->s->width, (i32)r->s->height};
  civue_damage covered;
  u32 batch = 0;

  civue_damage_begin(&covered, r->s->width, r->s->height);

  for (u32 i = 0; i < list->quads.len; i++) {
    const civue_quad *q = &list->quads.v[i];

    if (!opaque_rect(q)) {
      continue;
    }

    batch = batch_of(list, i, batch);

    if (blending_of(&list->batches.v[batch])) {
      continue;
    }

    // the pixels it is whole on, and the tiles inside of those.
    Flat f = flat(q);
    constexpr f32 FAR = 1e6f;
    civue_box b =
      cut(bounds(q, window), (civue_box){(i32)clampf(f.whole[0], -FAR, FAR),
                                         (i32)clampf(f.whole[1], -FAR, FAR),
                                         (i32)clampf(f.whole[2], -FAR, FAR),
                                         (i32)clampf(f.whole[3], -FAR, FAR)});
    i32 across = 1 << covered.across, down = 1 << covered.down;
    civue_box inner = {(b.x0 + across - 1) & ~(across - 1),
                       (b.y0 + down - 1) & ~(down - 1), b.x1 & ~(across - 1),
                       b.y1 & ~(down - 1)};

    civue_damage_add(&covered, inner);
  }

  civue_damage_without(d, &covered);
}

// clears what of `d` no opaque rect covers.
static void clear_tiles(const Raster *r, const civue_draw_list *list,
                        civue_damage d, bool whole) {
  Clears clears = {.r = r, .whole = whole};

  uncovered(r, list, &d);
  civue_damage_each(&d, r->s->width, r->s->height, clear_box, &clears);
  clear_boxes(&clears);
}

// the pixels of the image a batch draws, none when only the gpu has them.
static void use_texture(Raster *r, civue_texture t) {
  if (t == r->texture) {
    return;
  }

  r->texture = t;
  r->image = (civue_pixels){};

  if (t && r->images && r->images->find) {
    r->images->find(r->images->ctx, t, &r->image);
  }
}

// an opaque rect drawn after others, and the pixels it covers whole with
// its own color: what is under it there needs no drawing.
typedef struct {
  u32 at;
  civue_box box;
} Cover;

constexpr u32 COVERS = 32;
// less than this saves less than cutting around it takes.
constexpr i64 COVER_AREA = 128 * 128;

static civue_box covered(const civue_quad *q, civue_box limit) {
  if (!flat_rect(q) || q->color[3] < 1) {
    return (civue_box){};
  }

  Flat f = flat(q);
  constexpr f32 FAR = 1e6f;
  civue_box b =
    cut(bounds(q, limit), (civue_box){(i32)clampf(f.whole[0], -FAR, FAR),
                                      (i32)clampf(f.whole[1], -FAR, FAR),
                                      (i32)clampf(f.whole[2], -FAR, FAR),
                                      (i32)clampf(f.whole[3], -FAR, FAR)});
  civue_box corners[4];

  // a rounded clip may cut it there.
  if (rounded(q) && corner_boxes(q, b, corners)) {
    return (civue_box){};
  }

  return b;
}

// draws a quad in `box` but where a cover after it hides it.
static void quad_under(Raster *r, const civue_quad *q, civue_box box,
                       const Cover *covers, u32 n) {
  civue_box b = bounds(q, box), parts[64];
  u32 len = 1;

  parts[0] = b;

  for (u32 k = 0; k < n && len; k++) {
    if (empty(cut(covers[k].box, b))) {
      continue;
    }

    civue_box next[64];
    u32 m = 0;

    for (u32 i = 0; i < len && m + 4 <= countof(next); i++) {
      m += without(parts[i], covers[k].box, next + m);
    }

    // in too many pieces: drawn whole.
    if (m + 4 > countof(next)) {
      parts[0] = b;
      len = 1;
      break;
    }

    memcpy(parts, next, m * sizeof *next);
    len = m;
  }

  for (u32 i = 0; i < len; i++) {
    r->box = parts[i];
    quad(r, q);
  }

  r->box = box;
}

static void draw_box(Raster *r, const civue_draw_list *list, civue_box box) {
  if (empty(box)) {
    return;
  }

  r->box = box;
  r->blending = BLEND_OVER;

  // nothing under the last quad that hides the whole box needs drawing.
  u32 n = list->quads.len, first = 0;
  u32 batch = list->batches.len ? list->batches.len - 1 : 0;
  bool hidden = false;

  for (u32 i = n; i-- > 0 && !hidden;) {
    while (batch > 0 && list->batches.v[batch].start > i) {
      batch--;
    }

    if (!blending_of(&list->batches.v[batch]) && hides(&list->quads.v[i], box)) {
      first = i;
      hidden = true;
    }
  }

  if (!hidden) {
    civue_damage whole;

    civue_damage_begin(&whole, r->s->width, r->s->height);
    civue_damage_add(&whole, box);
    clear_tiles(r, list, whole, true);
  }

  Cover covers[COVERS];
  u32 ncovers = 0;

  batch = 0;

  for (u32 i = first + 1; i < n && ncovers < COVERS; i++) {
    batch = batch_of(list, i, batch);

    if (!blending_of(&list->batches.v[batch])) {
      civue_box c = covered(&list->quads.v[i], box);

      if (area(c) >= COVER_AREA) {
        covers[ncovers++] = (Cover){i, c};
      }
    }
  }

  batch = 0;

  for (u32 i = first, next = 0; i < n; i++) {
    batch = batch_of(list, i, batch);
    r->blending = blending_of(&list->batches.v[batch]);
    use_texture(r, list->batches.v[batch].texture);

    while (next < ncovers && covers[next].at <= i) {
      next++;
    }

    if (next < ncovers) {
      quad_under(r, &list->quads.v[i], box, covers + next, ncovers - next);
    }
    else {
      quad(r, &list->quads.v[i]);
    }
  }
}

static civue_box whole_window(u32 width, u32 height) {
  return (civue_box){0, 0, (i32)width, (i32)height};
}

// draws the marked tiles: cleared, then each quad over the ones it is over.
static void draw_tiles(Raster *r, const civue_draw_list *list,
                       const civue_damage *d) {
  civue_box window = whole_window(r->s->width, r->s->height);
  u32 batch = 0;

  clear_tiles(r, list, *d, false);

  for (u32 i = 0; i < list->quads.len; i++) {
    const civue_quad *q = &list->quads.v[i];
    civue_box b = bounds(q, window);

    if (empty(b)) {
      continue;
    }

    u32 first, last;
    u64 across[2];
    tiles(d, b, &first, &last, across);

    // most are over marked tiles only, or over none.
    u64 some = 0;
    bool whole = true;

    for (u32 row = first; row <= last; row++) {
      u64 lo = d->rows[row][0] & across[0], hi = d->rows[row][1] & across[1];

      some |= lo | hi;
      whole = whole && lo == across[0] && hi == across[1];
    }

    if (!some) {
      continue;
    }

    batch = batch_of(list, i, batch);
    r->blending = blending_of(&list->batches.v[batch]);
    use_texture(r, list->batches.v[batch].texture);

    if (whole) {
      r->box = window;
      quad(r, q);
      continue;
    }

    for (u32 row = first; row <= last; row++) {
      u64 over[2] = {d->rows[row][0] & across[0], d->rows[row][1] & across[1]};
      u32 from, to;

      for (u32 c = 0; run(over, c, &from, &to); c = to + 1) {
        civue_box tile = {(i32)(from << d->across), (i32)(row << d->down),
                          (i32)((to + 1) << d->across),
                          (i32)((row + 1) << d->down)};

        r->box = cut(tile, window);
        quad(r, q);
      }
    }
  }
}

void civue_raster(const civue_surface *s, const civue_draw_list *list,
                  civue_color clear, const civue_damage *damage,
                  const civue_atlas *mask, const civue_atlas *color,
                  const civue_raster_images *images) {
  Raster r = {
    .s = s, .mask = mask->pixels, .color = color->pixels, .images = images};
  // over nothing, see through or not.
  u8 p[4] = {byte(clear.b * clear.a * 255), byte(clear.g * clear.a * 255),
             byte(clear.r * clear.a * 255), byte(clear.a * 255)};
  memcpy(&r.clear, p, sizeof r.clear);

  if (damage->all) {
    draw_box(&r, list, whole_window(s->width, s->height));
  }
  else if (damage->any) {
    draw_tiles(&r, list, damage);
  }

#ifdef __SSE2__
  // what was written past the caches is in memory before anyone reads it.
  _mm_sfence();
#endif
}

// what a pixel asked to the shader's math takes, next to one filled, and
// one blended.
constexpr u64 ASKED = 32;
constexpr u64 BLENDED = 4;

// what a quad takes to draw in `b` as if it had no rounded clip, past filling
// its pixels.
static void plain_cost(const civue_quad *q, civue_box b,
                       const civue_pixels *image, u64 *cost) {
  u32 kind = (u32)q->params[1];
  i32 ox, oy;

  if (kind == CIVUE_QUAD_IMAGE && image_one_to_one(image, q, &ox, &oy)) {
    // its rim is asked, its corners' curves and two pixels a row and a
    // column.
    f32 corners = 0;

    for (u32 i = 0; i < 4; i++) {
      f32 r = fminf(q->radii[i], fminf(q->rect[2], q->rect[3]) * 0.5f);
      corners += r * r;
    }

    f32 asked = corners + 2 * (f32)(b.y1 - b.y0) + 2 * (f32)(b.x1 - b.x0);

    *cost += (u64)fminf(asked, (f32)area(b)) * ASKED + (u64)area(b) * BLENDED;
    return;
  }

  if (kind == CIVUE_QUAD_MASK) {
    return;
  }

  // a rect is filled but for its edges.
  if (flat_rect(q) && q->params[0] == 0 && q->radii[0] == 0 &&
      q->radii[1] == 0 && q->radii[2] == 0 && q->radii[3] == 0) {
    return;
  }

  if (flat_rect(q)) {
    // its corners and edges are asked, at most two pixels a row and what
    // its corners reach.
    Flat f = flat(q);
    f32 across = fmaxf(q->rect[2] - (f.mid[2] - f.mid[0]), 0);
    f32 rows = fmaxf(q->rect[3] - (f.whole[3] - f.whole[1]), 0);
    f32 asked = across * rows + 2 * (f32)(b.y1 - b.y0);

    *cost += (u64)fminf(asked, (f32)area(b)) * ASKED;
    return;
  }

  if (kind == CIVUE_QUAD_SHADOW) {
    // its ends are blended, and asked on the rows next to its corners.
    Soft s = soft(q);
    i64 ends =
      area(b) - area(cut(b, cover(s.x[0], -INFINITY, s.x[1], INFINITY, b)));
    civue_box rows = cover(-INFINITY, s.y[0], INFINITY, s.y[1], b);
    i64 straight =
      area(rows) -
      area(cut(rows, cover(s.x[0], -INFINITY, s.x[1], INFINITY, b)));

    *cost += (u64)(ends - straight) * ASKED + (u64)straight * BLENDED;
    return;
  }

  if (kind == CIVUE_QUAD_COLOR_GLYPH) {
    *cost += (u64)area(b) * BLENDED;
    return;
  }

  *cost += (u64)area(b) * ASKED;
}

// what a quad takes to draw in `box` past filling its pixels, false when
// only the gpu draws it: an image the cpu has no pixels of.
static bool quad_cost(const civue_quad *q, civue_box box,
                      const civue_pixels *image, u64 *cost) {
  if ((u32)q->params[1] == CIVUE_QUAD_IMAGE && !image->pixels) {
    return false;
  }

  civue_box b = bounds(q, box);

  if (empty(b)) {
    return true;
  }

  if (turned(q)) {
    *cost += (u64)area(b) * ASKED;
    return true;
  }

  if (rounded(q)) {
    civue_box corners[4];
    u32 n = corner_boxes(q, b, corners);

    for (u32 i = 0; i < n; i++) {
      *cost += (u64)area(corners[i]) * ASKED;
    }
  }

  plain_cost(q, b, image, cost);
  return true;
}

bool civue_raster_cost(const civue_draw_list *list, const civue_damage *damage,
                       u32 width, u32 height, const civue_raster_images *images,
                       uint64_t *cost) {
  civue_box window = whole_window(width, height);
  u64 sum = civue_damage_area(damage, width, height);
  u32 batch = 0;
  civue_texture texture = 0;
  civue_pixels image = {};

  if (damage->all || damage->any) {
    for (u32 i = 0; i < list->quads.len; i++) {
      const civue_quad *q = &list->quads.v[i];
      civue_box b = bounds(q, window);

      if (empty(b)) {
        continue;
      }

      if (!damage->all) {
        u32 first, last;
        u64 across[2], some = 0;
        tiles(damage, b, &first, &last, across);

        for (u32 row = first; row <= last; row++) {
          some |= (damage->rows[row][0] & across[0]) |
                  (damage->rows[row][1] & across[1]);
        }

        if (!some) {
          continue;
        }
      }

      if ((u32)q->params[1] == CIVUE_QUAD_IMAGE) {
        batch = batch_of(list, i, batch);
        civue_texture t = list->batches.v[batch].texture;

        if (t != texture || !texture) {
          texture = t;
          image = (civue_pixels){};

          if (t && images && images->find) {
            images->find(images->ctx, t, &image);
          }
        }
      }

      if (!quad_cost(q, window, &image, &sum)) {
        return false;
      }
    }
  }

  *cost = sum;
  return true;
}
