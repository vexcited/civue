#include "internal.h"

#include <civue/raster.h>

#include <math.h>

#ifdef __ARM_NEON
#include <arm_neon.h>
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

// the pixels whose centers are in x0 to x1 and y0 to y1, as the gpu fills
// them, inside `limit`.
static civue_box cover(f32 x0, f32 y0, f32 x1, f32 y1, civue_box limit) {
  f32 lx0 = (f32)limit.x0, ly0 = (f32)limit.y0;
  f32 lx1 = (f32)limit.x1, ly1 = (f32)limit.y1;
  // clamped first, a clip may be a billion pixels wide.
  x0 = fminf(fmaxf(x0, lx0), lx1), x1 = fminf(fmaxf(x1, lx0), lx1);
  y0 = fminf(fmaxf(y0, ly0), ly1), y1 = fminf(fmaxf(y1, ly0), ly1);

  civue_box b = {(i32)ceilf(x0 - 0.5f), (i32)ceilf(y0 - 0.5f),
                 (i32)ceilf(x1 - 0.5f), (i32)ceilf(y1 - 0.5f)};

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
        x->multiply != y->multiply) {
      return false;
    }
  }

  return true;
}

// no image nor multiplying: a quad draws the same wherever it is in the list.
static bool plain(const civue_draw_list *l) {
  vec_each(&l->batches, b) {
    if (b->texture || b->multiply) {
      return false;
    }
  }

  return true;
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

  if (n == m && same_batches(last, next)) {
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
  else if (plain(last) && plain(next)) {
    // quads came or went: what is between those both lists start and end
    // with.
    u32 least = MIN(n, m), head = 0, tail = 0;

    while (head < least && same_quad(&a[head], &b[head])) {
      head++;
    }

    while (tail < least - head &&
           same_quad(&a[n - 1 - tail], &b[m - 1 - tail])) {
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
  bool multiply;
  // the pixel of the color under everything.
  u32 clear;
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
static void blend(u8 *d, const f32 c[4], bool multiply) {
  f32 inv = 1 - c[3];

  if (multiply) {
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
#else
    d[0] = byte(c[2] * 255 + d[0] * inv);
    d[1] = byte(c[1] * 255 + d[1] * inv);
    d[2] = byte(c[0] * 255 + d[2] * inv);
#endif
  }

  d[3] = byte(c[3] * 255 + d[3] * inv);
}

// `n` pixels set to `v`.
static void fill(u8 *d, u32 n, u32 v) {
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
#endif

  for (; i < n; i++) {
    memcpy(d + i * 4, &v, sizeof v);
  }
}

// one color over a row's pixels from x0 to x1.
static void span(const Raster *r, i32 y, i32 x0, i32 x1, const f32 c[4]) {
  if (x0 >= x1) {
    return;
  }

  u8 *d = pixel(r->s, x0, y);
  u32 n = (u32)(x1 - x0);

  if (r->multiply) {
    for (u32 i = 0; i < n; i++) {
      blend(d + i * 4, c, true);
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
        blend(d, c, r->multiply);
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
        blend(d, color, r->multiply);
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

static bool is_flat(const civue_quad *q) {
  return (u32)q->params[1] == CIVUE_QUAD_RECT && !turned(q) && !rounded(q) &&
         q->params[2] <= 0.5f;
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

static void rect_quad(const Raster *r, const civue_quad *q, civue_box b) {
  Flat f = flat(q);
  f32 inside[4];
  // its color as it is where the rect is whole, whatever was there.
  bool has_inside = opaque_rect(q);

  if (has_inside) {
    memcpy(inside, q->color, sizeof inside);
  }

  for (i32 y = b.y0; y < b.y1; y++) {
    bool whole = (f32)y >= f.whole[1] && (f32)y < f.whole[3];
    f32 from = whole ? f.whole[0] : f.mid[0];
    f32 to = whole ? f.whole[2] : f.mid[2];
    i32 a = (i32)clampf(from, (f32)b.x0, (f32)b.x1);
    i32 e = (i32)clampf(to, (f32)a, (f32)b.x1);
    u8 *d = pixel(r->s, b.x0, y);

    for (i32 x = b.x0; x < a; x++, d += 4) {
      f32 c[4];
      shade_rect(q, (f32)x + 0.5f, (f32)y + 0.5f, c);
      blend(d, c, r->multiply);
    }

    if (a < e) {
      f32 c[4];

      if (y + AHEAD < b.y1) {
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
        }

        span(r, y, a, e, inside);
      }
    }

    d = pixel(r->s, e, y);

    for (i32 x = e; x < b.x1; x++, d += 4) {
      f32 c[4];
      shade_rect(q, (f32)x + 0.5f, (f32)y + 0.5f, c);
      blend(d, c, r->multiply);
    }
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

// a pixel of coverage `a` tinted `c`, blue first, over `p`.
static void mask_pixel(u8 *p, u32 a, const u8 c[4]) {
  u32 inv = 255 - div255(c[3] * a);

  for (u32 k = 0; k < 4; k++) {
    p[k] = (u8)div255(c[k] * a + p[k] * inv);
  }
}

static void mask_quad(const Raster *r, const civue_quad *q, civue_box b) {
  i32 u, v;

  if (!r->mask || r->multiply || !one_to_one(q, &u, &v)) {
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

  if (!r->color || r->multiply || !one_to_one(q, &u, &v)) {
    each(r, q, b);
    return;
  }

  u32 opacity = byte(q->color[3] * 255);

  for (i32 y = b.y0; y < b.y1; y++) {
    const u8 *t =
      r->color + ((usize)(y + v) * CIVUE_ATLAS_SIZE + (b.x0 + u)) * 4;
    u8 *d = pixel(r->s, b.x0, y);

    for (i32 x = b.x0; x < b.x1; x++, t += 4, d += 4) {
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
      blend(d, c, r->multiply);
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
      blend(d, c, r->multiply);
    }
  }
}

static void quad(const Raster *r, const civue_quad *q) {
  u32 kind = (u32)q->params[1];

  if (kind == CIVUE_QUAD_IMAGE) {
    return;
  }

  if (turned(q)) {
    each_turned(r, q);
    return;
  }

  civue_box b = bounds(q, r->box);

  if (empty(b)) {
    return;
  }

  // rounded off, its pixels are each asked.
  if (rounded(q)) {
    each(r, q, b);
  }
  else if (is_flat(q)) {
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

      fill(pixel(c->r->s, b.x0, y), (u32)(b.x1 - b.x0), c->r->clear);
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

    if (list->batches.v[batch].multiply) {
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

static void draw_box(Raster *r, const civue_draw_list *list, civue_box box) {
  if (empty(box)) {
    return;
  }

  r->box = box;
  r->multiply = false;

  // nothing under the last quad that hides the whole box needs drawing.
  u32 n = list->quads.len, first = 0;
  u32 batch = list->batches.len ? list->batches.len - 1 : 0;
  bool hidden = false;

  for (u32 i = n; i-- > 0 && !hidden;) {
    while (batch > 0 && list->batches.v[batch].start > i) {
      batch--;
    }

    if (!list->batches.v[batch].multiply && hides(&list->quads.v[i], box)) {
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

  batch = 0;

  for (u32 i = first; i < n; i++) {
    batch = batch_of(list, i, batch);
    r->multiply = list->batches.v[batch].multiply;
    quad(r, &list->quads.v[i]);
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
    r->multiply = list->batches.v[batch].multiply;

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
                  const civue_atlas *mask, const civue_atlas *color) {
  Raster r = {.s = s, .mask = mask->pixels, .color = color->pixels};
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
}

// what a pixel asked to the shader's math takes, next to one filled, and
// one blended.
constexpr u64 ASKED = 32;
constexpr u64 BLENDED = 4;

// what a quad takes to draw in `box` past filling its pixels, false when
// only the gpu draws it.
static bool quad_cost(const civue_quad *q, civue_box box, u64 *cost) {
  u32 kind = (u32)q->params[1];

  if (kind == CIVUE_QUAD_MASK && !turned(q)) {
    return true;
  }

  // a rect is filled but for its edges.
  if (is_flat(q) && q->params[0] == 0 && q->radii[0] == 0 && q->radii[1] == 0 &&
      q->radii[2] == 0 && q->radii[3] == 0) {
    return true;
  }

  civue_box b = bounds(q, box);

  if (empty(b)) {
    return true;
  }

  if (kind == CIVUE_QUAD_IMAGE) {
    return false;
  }

  if (turned(q)) {
    *cost += (u64)area(b) * ASKED;
    return true;
  }

  if (is_flat(q)) {
    // its corners and edges are asked, at most two pixels a row and what
    // its corners reach.
    Flat f = flat(q);
    f32 across = fmaxf(q->rect[2] - (f.mid[2] - f.mid[0]), 0);
    f32 rows = fmaxf(q->rect[3] - (f.whole[3] - f.whole[1]), 0);
    f32 asked = across * rows + 2 * (f32)(b.y1 - b.y0);

    *cost += (u64)fminf(asked, (f32)area(b)) * ASKED;
    return true;
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
    return true;
  }

  if (kind == CIVUE_QUAD_COLOR_GLYPH) {
    *cost += (u64)area(b) * BLENDED;
    return true;
  }

  *cost += (u64)area(b) * ASKED;
  return true;
}

bool civue_raster_cost(const civue_draw_list *list, const civue_damage *damage,
                       u32 width, u32 height, uint64_t *cost) {
  civue_box window = whole_window(width, height);
  u64 sum = civue_damage_area(damage, width, height);

  if (damage->all || damage->any) {
    vec_each(&list->quads, q) {
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

      if (!quad_cost(q, window, &sum)) {
        return false;
      }
    }
  }

  *cost = sum;
  return true;
}
