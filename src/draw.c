#include "internal.h"

#include <civue/draw.h>

#include <stdlib.h>

void civue_dl_clear(civue_draw_list *d) {
  d->quads.len = 0;
  d->batches.len = 0;
  d->clips.len = 0;
  d->multiply = false;
  memset(d->turn, 0, sizeof d->turn);
}

void civue_dl_free(civue_draw_list *d) {
  vec_free(&d->quads);
  vec_free(&d->batches);
  vec_free(&d->clips);
}

void civue_dl_append(civue_draw_list *d, const civue_draw_list *other) {
  u32 offset = d->quads.len;
  vec_each(&other->quads, q) vec_push(&d->quads, *q);
  vec_each(&other->batches, b) {
    civue_batch c = *b;
    c.start += offset;
    vec_push(&d->batches, c);
  }
}

civue_rect civue_dl_clip(const civue_draw_list *d) {
  return d->clips.len ? vec_last(&d->clips) : rect(-1e9, -1e9, 1e9, 1e9);
}

void civue_dl_push_clip(civue_draw_list *d, civue_rect r) {
  civue_rect c = civue_dl_clip(d);
  vec_push(&d->clips, rect(fmax(r.x0, c.x0), fmax(r.y0, c.y0), fmin(r.x1, c.x1), fmin(r.y1, c.y1)));
}

void civue_dl_pop_clip(civue_draw_list *d) {
  if (d->clips.len) d->clips.len--;
}

void civue_dl_set_turn(civue_draw_list *d, f32 angle, civue_vec2 pivot) {
  if (angle == 0)
    memset(d->turn, 0, sizeof d->turn);
  else {
    d->turn[0] = cosf(angle), d->turn[1] = sinf(angle);
    d->turn[2] = (f32)pivot.x, d->turn[3] = (f32)pivot.y;
  }
}

void civue_dl_set_multiply(civue_draw_list *d, bool on) { d->multiply = on; }

static void premul(civue_color c, f32 out[4]) {
  out[0] = c.r * c.a, out[1] = c.g * c.a, out[2] = c.b * c.a, out[3] = c.a;
}

static void set_rect(civue_quad *q, civue_rect r) {
  q->rect[0] = (f32)r.x0, q->rect[1] = (f32)r.y0;
  q->rect[2] = (f32)(r.x1 - r.x0), q->rect[3] = (f32)(r.y1 - r.y0);
}

static void push(civue_draw_list *d, civue_quad q, civue_texture texture) {
  civue_rect c = civue_dl_clip(d);
  q.clip[0] = (f32)c.x0, q.clip[1] = (f32)c.y0;
  q.clip[2] = (f32)c.x1, q.clip[3] = (f32)c.y1;
  memcpy(q.turn, d->turn, sizeof q.turn);
  bool turned = d->turn[0] != 0 || d->turn[1] != 0;
  // nothing visible outside the clip.
  f32 m = q.params[1] == CIVUE_QUAD_SHADOW ? q.params[2] * 3 : 0;
  f32 *r = q.rect;
  if (!turned &&
      (r[0] - m >= q.clip[2] || r[1] - m >= q.clip[3] || r[0] + r[2] + m <= q.clip[0] || r[1] + r[3] + m <= q.clip[1]))
    return;
  civue_batch *last = d->batches.len ? &vec_last(&d->batches) : nullptr;
  if (!last || (texture && last->texture != texture) || last->multiply != d->multiply)
    vec_push(&d->batches, ((civue_batch){d->quads.len, texture, d->multiply}));
  else if (!last->texture)
    last->texture = texture;
  vec_push(&d->quads, q);
}

void civue_dl_rect(civue_draw_list *d, civue_rect r, const f32 radii[4], civue_color fill, civue_color border,
                   f32 border_width) {
  civue_quad q = {.params = {border_width, CIVUE_QUAD_RECT}};
  set_rect(&q, r);
  premul(fill, q.color);
  premul(border, q.border);
  memcpy(q.radii, radii, sizeof q.radii);
  push(d, q, 0);
}

void civue_dl_shadow(civue_draw_list *d, civue_rect r, f32 radius, f32 sigma, civue_color color) {
  civue_quad q = {.radii = {radius, radius, radius, radius}, .params = {0, CIVUE_QUAD_SHADOW, sigma}};
  set_rect(&q, r);
  premul(color, q.color);
  push(d, q, 0);
}

void civue_dl_line(civue_draw_list *d, civue_vec2 a, civue_vec2 b, f32 width, civue_color color, bool square) {
  // the quad covers the segment and its width around it.
  f64 m = width / 2 + 1;
  civue_rect r = rect(fmin(a.x, b.x) - m, fmin(a.y, b.y) - m, fmax(a.x, b.x) + m, fmax(a.y, b.y) + m);
  civue_quad q = {.radii = {(f32)a.x, (f32)a.y, (f32)b.x, (f32)b.y}, .params = {width, CIVUE_QUAD_LINE, square}};
  set_rect(&q, r);
  premul(color, q.color);
  push(d, q, 0);
}

void civue_dl_mask(civue_draw_list *d, civue_rect r, const f32 uv[4], civue_color color, f32 gamma) {
  civue_quad q = {.params = {0, CIVUE_QUAD_MASK, gamma}};
  set_rect(&q, r);
  premul(color, q.color);
  memcpy(q.uv, uv, sizeof q.uv);
  push(d, q, 0);
}

void civue_dl_color_glyph(civue_draw_list *d, civue_rect r, const f32 uv[4], f32 opacity) {
  civue_quad q = {.color = {opacity, opacity, opacity, opacity}, .params = {0, CIVUE_QUAD_COLOR_GLYPH}};
  set_rect(&q, r);
  memcpy(q.uv, uv, sizeof q.uv);
  push(d, q, 0);
}

void civue_dl_image(civue_draw_list *d, civue_texture t, civue_rect r, const f32 uv[4], f32 opacity, f32 radius,
                    bool nearest) {
  civue_quad q = {.color = {opacity, opacity, opacity, opacity},
                  .radii = {radius, radius, radius, radius},
                  .params = {0, CIVUE_QUAD_IMAGE, 0, nearest}};
  set_rect(&q, r);
  memcpy(q.uv, uv, sizeof q.uv);
  push(d, q, t);
}

// the origin moved by whole units next to the screen, positions far from
// the origin lose their fractions in f32.
static void encode_grid(const civue_pixel_grid *g, f32 out[4]) {
  out[0] = (f32)(g->origin.x - floor(g->origin.x / g->scale) * g->scale);
  out[1] = (f32)(g->origin.y - floor(g->origin.y / g->scale) * g->scale);
  out[2] = (f32)g->scale;
  out[3] = 0;
}

void civue_dl_rect_grid(civue_draw_list *d, civue_rect r, civue_color fill, const civue_pixel_grid *grid) {
  civue_quad q = {.params = {0, CIVUE_QUAD_RECT, grid != nullptr}};
  set_rect(&q, r);
  premul(fill, q.color);
  if (grid) encode_grid(grid, q.border);
  push(d, q, 0);
}

void civue_dl_image_grid(civue_draw_list *d, civue_texture t, civue_rect r, const f32 uv[4], bool nearest,
                         const civue_pixel_grid *grid) {
  // hard edges: tiles meeting at fractional pixels leave no seam.
  civue_quad q = {.color = {1, 1, 1, 1}, .params = {1, CIVUE_QUAD_IMAGE, grid != nullptr, nearest}};
  set_rect(&q, r);
  memcpy(q.uv, uv, sizeof q.uv);
  if (grid) encode_grid(grid, q.border);
  push(d, q, t);
}

// ---- atlas ----

bool civue_atlas_insert(civue_atlas *a, u32 w, u32 h, const u8 *data, f32 uv[4]) {
  if (!w || !h) {
    memset(uv, 0, 4 * sizeof *uv);
    return true;
  }
  // a texel of padding keeps neighbors out of filtered samples, shelves
  // come in heights rounded to 4 so similar glyphs share them.
  u32 pw = w + 1, ph = (h + 1 + 3) & ~3u;
  if (pw > CIVUE_ATLAS_SIZE || ph > CIVUE_ATLAS_SIZE) return false;
  civue_shelf *best = nullptr;
  vec_each(&a->shelves, s) {
    if (s->height < ph || s->height > ph * 3 / 2 + 4 || s->x + pw > CIVUE_ATLAS_SIZE) continue;
    if (!best || s->height < best->height) best = s;
  }
  if (!best) {
    if (a->top + ph > CIVUE_ATLAS_SIZE) return false;
    vec_push(&a->shelves, ((civue_shelf){a->top, ph, 0}));
    a->top += ph;
    best = &vec_last(&a->shelves);
  }
  u32 x = best->x, y = best->y;
  best->x += pw;
  civue_atlas_upload up = {x, y, w, h};
  buf_put(&up.data, data, (usize)w * h * a->bytes_per_pixel);
  vec_push(&a->pending, up);
  f32 s = CIVUE_ATLAS_SIZE;
  uv[0] = x / s, uv[1] = y / s, uv[2] = (x + w) / s, uv[3] = (y + h) / s;
  return true;
}

void civue_atlas_clear(civue_atlas *a) {
  a->shelves.len = 0;
  a->top = 0;
  vec_each(&a->pending, p) vec_free(&p->data);
  a->pending.len = 0;
  a->generation++;
}

void civue_atlas_free(civue_atlas *a) {
  civue_atlas_clear(a);
  vec_free(&a->shelves);
  vec_free(&a->pending);
}
