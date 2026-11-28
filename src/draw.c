#include "internal.h"

#include <civue/draw.h>

#include <stdlib.h>

void civue_dl_clear(civue_draw_list *d) {
  d->quads.len = 0;
  d->batches.len = 0;
  d->clips.len = 0;
  d->rounds.len = 0;
  d->multiply = false;
  d->erase = false;
  d->fade = 0;
  d->zoom = (civue_zoom){};
  memset(d->turn, 0, sizeof d->turn);
  d->keep_hidden = false;
  d->cuts = 0;
}

void civue_dl_free(civue_draw_list *d) {
  vec_free(&d->quads);
  vec_free(&d->batches);
  vec_free(&d->clips);
  vec_free(&d->rounds);
}

void civue_dl_append(civue_draw_list *d, const civue_draw_list *other) {
  u32 offset = d->quads.len;
  vec_each(&other->quads, q) {
    vec_push(&d->quads, *q);
  }
  vec_each(&other->batches, b) {
    civue_batch c = *b;
    c.start += offset;
    vec_push(&d->batches, c);
  }
}

civue_rect civue_dl_clip(const civue_draw_list *d) {
  return d->clips.len ? vec_last(&d->clips) : rect(-1e9, -1e9, 1e9, 1e9);
}

static bool zoomed(const civue_draw_list *d) {
  civue_zoom z = d->zoom;

  return z.scale != 0 && (z.scale != 1 || z.x != 0 || z.y != 0);
}

static civue_rect zoom_rect(const civue_draw_list *d, civue_rect r) {
  civue_zoom z = d->zoom;

  if (!zoomed(d)) {
    return r;
  }

  return rect(z.x + r.x0 * z.scale, z.y + r.y0 * z.scale, z.x + r.x1 * z.scale,
              z.y + r.y1 * z.scale);
}

void civue_dl_push_clip(civue_draw_list *d, civue_rect r) {
  r = zoom_rect(d, r);
  civue_rect c = civue_dl_clip(d);
  if (r.x0 < c.x0 || r.y0 < c.y0 || r.x1 > c.x1 || r.y1 > c.y1) {
    d->cuts++;
  }
  vec_push(&d->clips, rect(fmax(r.x0, c.x0), fmax(r.y0, c.y0), fmin(r.x1, c.x1),
                           fmin(r.y1, c.y1)));
}

void civue_dl_pop_clip(civue_draw_list *d) {
  if (d->clips.len) {
    d->clips.len--;
  }
}

civue_round civue_dl_round(const civue_draw_list *d) {
  return d->rounds.len ? vec_last(&d->rounds) : (civue_round){};
}

void civue_dl_push_round(civue_draw_list *d, civue_rect r, const f32 radii[4]) {
  r = zoom_rect(d, r);
  civue_round round = {
    .rect = {(f32)r.x0, (f32)r.y0, (f32)(r.x1 - r.x0), (f32)(r.y1 - r.y0)},
  };

  for (u32 i = 0; i < 4; i++) {
    round.radii[i] = radii[i] * (zoomed(d) ? d->zoom.scale : 1);
  }
  // what is recorded under it only goes back where it was.
  d->cuts++;
  vec_push(&d->rounds, round);
}

void civue_dl_pop_round(civue_draw_list *d) {
  if (d->rounds.len) {
    d->rounds.len--;
  }
}

static void set_round(const civue_draw_list *d, civue_quad *q) {
  civue_round round = civue_dl_round(d);

  memcpy(q->round, round.rect, sizeof q->round);
  memcpy(q->round_radii, round.radii, sizeof q->round_radii);
}

void civue_dl_set_turn(civue_draw_list *d, f32 angle, civue_vec2 pivot) {
  if (angle == 0) {
    memset(d->turn, 0, sizeof d->turn);
  }
  else {
    d->turn[0] = cosf(angle), d->turn[1] = sinf(angle);
    d->turn[2] = (f32)pivot.x, d->turn[3] = (f32)pivot.y;
  }
}

void civue_dl_set_multiply(civue_draw_list *d, bool on) {
  d->multiply = on;
}

void civue_dl_set_erase(civue_draw_list *d, bool on) {
  d->erase = on;
}

void civue_dl_set_opacity(civue_draw_list *d, f32 opacity) {
  d->fade = 1 - fminf(fmaxf(opacity, 0), 1);
}

f32 civue_dl_opacity(const civue_draw_list *d) {
  return 1 - d->fade;
}

void civue_dl_set_zoom(civue_draw_list *d, civue_zoom zoom) {
  d->zoom = zoom;
}

civue_zoom civue_dl_zoom(const civue_draw_list *d) {
  return d->zoom.scale != 0 ? d->zoom : (civue_zoom){.scale = 1};
}

civue_zoom civue_zoom_around(civue_zoom z, f32 scale, civue_vec2 pivot) {
  if (z.scale == 0) {
    z = (civue_zoom){.scale = 1};
  }

  // around the pivot first, then as `z` does.
  f32 x = (f32)pivot.x * (1 - scale), y = (f32)pivot.y * (1 - scale);

  return (civue_zoom){z.scale * scale, z.x + z.scale * x, z.y + z.scale * y};
}

// a quad's geometry, by the list's zoom.
static void zoom(const civue_draw_list *d, civue_quad *q) {
  civue_zoom z = d->zoom;
  f32 s = z.scale;

  if (!zoomed(d)) {
    return;
  }

  q->rect[0] = z.x + q->rect[0] * s, q->rect[1] = z.y + q->rect[1] * s;
  q->rect[2] *= s, q->rect[3] *= s;

  // a line's radii are its ends.
  if (q->params[1] == CIVUE_QUAD_LINE) {
    q->radii[0] = z.x + q->radii[0] * s, q->radii[1] = z.y + q->radii[1] * s;
    q->radii[2] = z.x + q->radii[2] * s, q->radii[3] = z.y + q->radii[3] * s;
    q->params[0] *= s;
  }
  // an arc's are turns and a part of its radius.
  else if (q->params[1] != CIVUE_QUAD_ARC) {
    for (u32 i = 0; i < 4; i++) {
      q->radii[i] *= s;
    }
  }

  if (q->params[1] == CIVUE_QUAD_RECT && q->params[0] > 0) {
    q->params[0] *= s;
  }
  // a border of its own width on each side.
  else if (q->params[1] == CIVUE_QUAD_RECT && q->params[0] < 0) {
    for (u32 i = 0; i < 4; i++) {
      q->uv[i] *= s;
    }
  }
  else if (q->params[1] == CIVUE_QUAD_SHADOW) {
    q->params[2] *= s;
  }

  if (q->turn[0] != 0 || q->turn[1] != 0) {
    q->turn[2] = z.x + q->turn[2] * s, q->turn[3] = z.y + q->turn[3] * s;
  }
}

// a quad's colors, premultiplied, at the list's opacity.
static void fade(const civue_draw_list *d, civue_quad *q) {
  if (d->fade <= 0) {
    return;
  }

  // the border of an image revealed is its progress.
  bool progress = q->params[1] == CIVUE_QUAD_IMAGE && q->params[2] > 1.5f;

  for (u32 i = 0; i < 4; i++) {
    q->color[i] *= 1 - d->fade;
    q->border[i] *= progress ? 1 : 1 - d->fade;
  }
}

// whether the last batch blends the way the list does now.
static bool same_blend(const civue_batch *b, const civue_draw_list *d) {
  return b->multiply == d->multiply && b->erase == d->erase;
}

static void premul(civue_color c, f32 out[4]) {
  out[0] = c.r * c.a, out[1] = c.g * c.a, out[2] = c.b * c.a, out[3] = c.a;
}

static void set_rect(civue_quad *q, civue_rect r) {
  q->rect[0] = (f32)r.x0, q->rect[1] = (f32)r.y0;
  q->rect[2] = (f32)(r.x1 - r.x0), q->rect[3] = (f32)(r.y1 - r.y0);
}

static void push(civue_draw_list *d, civue_quad q, civue_texture texture) {
  civue_rect c = civue_dl_clip(d);
  fade(d, &q);
  q.clip[0] = (f32)c.x0, q.clip[1] = (f32)c.y0;
  q.clip[2] = (f32)c.x1, q.clip[3] = (f32)c.y1;
  set_round(d, &q);
  memcpy(q.turn, d->turn, sizeof q.turn);
  zoom(d, &q);
  bool turned = d->turn[0] != 0 || d->turn[1] != 0;
  // nothing visible outside the clip.
  f32 m = q.params[1] == CIVUE_QUAD_SHADOW ? q.params[2] * 3 : 0;
  f32 *r = q.rect;
  if (!turned && !d->keep_hidden &&
      (r[0] - m >= q.clip[2] || r[1] - m >= q.clip[3] ||
       r[0] + r[2] + m <= q.clip[0] || r[1] + r[3] + m <= q.clip[1])) {
    return;
  }
  civue_batch *last = d->batches.len ? &vec_last(&d->batches) : nullptr;
  if (!last || (texture && last->texture != texture) ||
      !same_blend(last, d)) {
    vec_push(&d->batches, ((civue_batch){d->quads.len, texture, d->multiply,
                                         d->erase}));
  }
  else if (!last->texture) {
    last->texture = texture;
  }
  vec_push(&d->quads, q);
}

void civue_dl_replay(civue_draw_list *d, const civue_quad *quads, u32 count,
                     f32 dx, f32 dy, const f32 was[4],
                     const civue_round *round_was) {
  if (!count) {
    return;
  }

  civue_rect c = civue_dl_clip(d);
  f32 now[4] = {(f32)c.x0, (f32)c.y0, (f32)c.x1, (f32)c.y1};
  civue_round round = civue_dl_round(d);
  bool still = dx == 0 && dy == 0 && !memcmp(now, was, sizeof now) &&
               !memcmp(&round, round_was, sizeof round);

  civue_batch *last = d->batches.len ? &vec_last(&d->batches) : nullptr;
  if (!last || !same_blend(last, d)) {
    vec_push(&d->batches,
             ((civue_batch){d->quads.len, 0, d->multiply, d->erase}));
  }

  vec_reserve(&d->quads, count);
  civue_quad *out = d->quads.v + d->quads.len;
  memcpy(out, quads, sizeof *out * count);
  d->quads.len += count;
  if (still) {
    return;
  }

  for (u32 i = 0; i < count; i++) {
    civue_quad *q = &out[i];
    q->rect[0] += dx, q->rect[1] += dy;
    if (q->params[1] == CIVUE_QUAD_LINE) {
      q->radii[0] += dx, q->radii[1] += dy;
      q->radii[2] += dx, q->radii[3] += dy;
    }

    if (!memcmp(q->round, round_was->rect, sizeof q->round) &&
        !memcmp(q->round_radii, round_was->radii, sizeof q->round_radii)) {
      memcpy(q->round, round.rect, sizeof q->round);
      memcpy(q->round_radii, round.radii, sizeof q->round_radii);
    }
    else {
      q->round[0] += dx, q->round[1] += dy;
    }

    if (!memcmp(q->clip, was, sizeof q->clip)) {
      memcpy(q->clip, now, sizeof now);
      continue;
    }

    q->clip[0] = fmaxf(q->clip[0] + dx, now[0]);
    q->clip[1] = fmaxf(q->clip[1] + dy, now[1]);
    q->clip[2] = fminf(q->clip[2] + dx, now[2]);
    q->clip[3] = fminf(q->clip[3] + dy, now[3]);
  }
}

static f32 fade_at(f32 y, f32 y0, f32 from, f32 y1, f32 to) {
  f32 t = y1 > y0 ? fminf(fmaxf((y - y0) / (y1 - y0), 0), 1) : 0;

  return fminf(fmaxf(from + (to - from) * t, 0), 1);
}

void civue_dl_fade_down(civue_draw_list *d, u32 first, f32 y0, f32 from,
                        f32 y1, f32 to, f32 step) {
  u32 count = d->quads.len > first ? d->quads.len - first : 0;

  if (!count) {
    return;
  }

  civue_quad *was = own_new(sizeof *was * count);
  u32 batch = 0;

  memcpy(was, d->quads.v + first, sizeof *was * count);
  d->quads.len = first;

  while (batch < d->batches.len && d->batches.v[batch].start < first) {
    batch++;
  }

  for (u32 i = 0; i < count; i++) {
    // the batches that started at it start at what it becomes.
    for (; batch < d->batches.len && d->batches.v[batch].start <= first + i;
         batch++) {
      d->batches.v[batch].start = d->quads.len;
    }

    civue_quad q = was[i];
    bool turned = q.turn[0] != 0 || q.turn[1] != 0;
    f32 m = q.params[1] == CIVUE_QUAD_SHADOW ? q.params[2] * 3 : 0;
    f32 top = fmaxf(q.rect[1] - m, q.clip[1]);
    f32 bottom = fminf(q.rect[1] + q.rect[3] + m, q.clip[3]);
    bool same = fabsf(fade_at(top, y0, from, y1, to) -
                      fade_at(bottom, y0, from, y1, to)) < 0.002f;
    u32 bands = turned || same || !(step > 0) || bottom - top <= step
                  ? 1
                  : (u32)ceilf((bottom - top) / step);
    bool progress = q.params[1] == CIVUE_QUAD_IMAGE && q.params[2] > 1.5f;

    for (u32 b = 0; b < bands; b++) {
      civue_quad cut = q;
      f32 a = top, z = bottom;

      if (bands > 1) {
        a = b ? roundf(top + (bottom - top) * (f32)b / (f32)bands) : q.clip[1];
        z = b + 1 < bands
              ? roundf(top + (bottom - top) * (f32)(b + 1) / (f32)bands)
              : q.clip[3];
        cut.clip[1] = a;
        cut.clip[3] = z;
        a = fmaxf(a, top);
        z = fminf(z, bottom);
      }

      f32 opacity = fade_at((a + z) / 2, y0, from, y1, to);

      if (opacity <= 0 || cut.clip[3] <= cut.clip[1]) {
        continue;
      }

      for (u32 k = 0; k < 4; k++) {
        cut.color[k] *= opacity;
        cut.border[k] *= progress ? 1 : opacity;
      }

      vec_push(&d->quads, cut);
    }
  }

  for (; batch < d->batches.len; batch++) {
    d->batches.v[batch].start = d->quads.len;
  }

  own_free(was);
}

void civue_dl_shift(civue_draw_list *d, f32 dx, f32 dy) {
  vec_each(&d->quads, q) {
    q->rect[0] += dx, q->rect[1] += dy;
    q->clip[0] += dx, q->clip[1] += dy;
    q->clip[2] += dx, q->clip[3] += dy;
    q->round[0] += dx, q->round[1] += dy;

    if (q->params[1] == CIVUE_QUAD_LINE) {
      q->radii[0] += dx, q->radii[1] += dy;
      q->radii[2] += dx, q->radii[3] += dy;
    }

    if (q->turn[0] != 0 || q->turn[1] != 0) {
      q->turn[2] += dx, q->turn[3] += dy;
    }
  }
}

void civue_dl_rect(civue_draw_list *d, civue_rect r, const f32 radii[4],
                   civue_color fill, civue_color border, f32 border_width) {
  civue_quad q = {.params = {border_width, CIVUE_QUAD_RECT}};
  set_rect(&q, r);
  premul(fill, q.color);
  premul(border, q.border);
  memcpy(q.radii, radii, sizeof q.radii);
  push(d, q, 0);
}

void civue_dl_rect_sides(civue_draw_list *d, civue_rect r, const f32 radii[4],
                         civue_color fill, civue_color border,
                         const f32 widths[4]) {
  civue_quad q = {.params = {-1, CIVUE_QUAD_RECT}};
  set_rect(&q, r);
  premul(fill, q.color);
  premul(border, q.border);
  memcpy(q.radii, radii, sizeof q.radii);
  memcpy(q.uv, widths, sizeof q.uv);
  push(d, q, 0);
}

void civue_dl_shadow(civue_draw_list *d, civue_rect r, f32 radius, f32 sigma,
                     civue_color color) {
  civue_quad q = {.radii = {radius, radius, radius, radius},
                  .params = {0, CIVUE_QUAD_SHADOW, sigma}};
  set_rect(&q, r);
  premul(color, q.color);
  push(d, q, 0);
}

void civue_dl_line(civue_draw_list *d, civue_vec2 a, civue_vec2 b, f32 width,
                   civue_color color, bool square) {
  // the quad covers the segment and its width around it.
  f64 m = width / 2 + 1;
  civue_rect r = rect(fmin(a.x, b.x) - m, fmin(a.y, b.y) - m,
                      fmax(a.x, b.x) + m, fmax(a.y, b.y) + m);
  civue_quad q = {.radii = {(f32)a.x, (f32)a.y, (f32)b.x, (f32)b.y},
                  .params = {width, CIVUE_QUAD_LINE, square}};
  set_rect(&q, r);
  premul(color, q.color);
  push(d, q, 0);
}

void civue_dl_arc(civue_draw_list *d, civue_vec2 center, f32 radius,
                  f32 thickness, f32 start, f32 sweep, civue_color top,
                  civue_color bottom) {
  if (sweep <= 0 || radius <= 0) {
    return;
  }

  civue_rect r = rect(center.x - radius, center.y - radius, center.x + radius,
                      center.y + radius);
  civue_quad q = {.radii = {start - floorf(start), sweep, thickness},
                  .params = {0, CIVUE_QUAD_ARC}};
  set_rect(&q, r);
  premul(top, q.color);
  premul(bottom, q.border);
  push(d, q, 0);
}

void civue_dl_mask(civue_draw_list *d, civue_rect r, const f32 uv[4],
                   civue_color color, f32 gamma) {
  civue_quad q = {.params = {0, CIVUE_QUAD_MASK, gamma}};
  set_rect(&q, r);
  premul(color, q.color);
  memcpy(q.uv, uv, sizeof q.uv);
  push(d, q, 0);
}

void civue_dl_masks(civue_draw_list *d, const civue_glyph_quad *quads,
                    u32 count, civue_color color, f32 gamma) {
  if (!count) {
    return;
  }

  civue_rect c = civue_dl_clip(d);
  civue_quad q = {.params = {0, CIVUE_QUAD_MASK, gamma}};
  q.clip[0] = (f32)c.x0, q.clip[1] = (f32)c.y0;
  q.clip[2] = (f32)c.x1, q.clip[3] = (f32)c.y1;
  premul(color, q.color);
  fade(d, &q);
  set_round(d, &q);
  memcpy(q.turn, d->turn, sizeof q.turn);
  bool all = d->keep_hidden || d->turn[0] != 0 || d->turn[1] != 0;

  civue_batch *last = d->batches.len ? &vec_last(&d->batches) : nullptr;
  bool begun = !last || !same_blend(last, d);
  u32 start = d->quads.len;
  vec_reserve(&d->quads, count);
  civue_quad *out = d->quads.v + d->quads.len;

  for (u32 i = 0; i < count; i++) {
    civue_quad g = q;

    memcpy(g.rect, quads[i].rect, sizeof g.rect);
    zoom(d, &g);

    const f32 *r = g.rect;
    // nothing visible outside the clip.
    if (!all && (r[0] >= g.clip[2] || r[1] >= g.clip[3] ||
                 r[0] + r[2] <= g.clip[0] || r[1] + r[3] <= g.clip[1])) {
      continue;
    }

    memcpy(g.uv, quads[i].uv, sizeof g.uv);
    *out++ = g;
  }

  d->quads.len = (u32)(out - d->quads.v);
  if (begun && d->quads.len > start) {
    vec_push(&d->batches, ((civue_batch){start, 0, d->multiply, d->erase}));
  }
}

void civue_dl_color_glyph(civue_draw_list *d, civue_rect r, const f32 uv[4],
                          f32 opacity) {
  civue_quad q = {.color = {opacity, opacity, opacity, opacity},
                  .params = {0, CIVUE_QUAD_COLOR_GLYPH}};
  set_rect(&q, r);
  memcpy(q.uv, uv, sizeof q.uv);
  push(d, q, 0);
}

void civue_dl_image(civue_draw_list *d, civue_texture t, civue_rect r,
                    const f32 uv[4], f32 opacity, f32 radius, bool nearest) {
  civue_quad q = {.color = {opacity, opacity, opacity, opacity},
                  .radii = {radius, radius, radius, radius},
                  .params = {0, CIVUE_QUAD_IMAGE, 0, nearest}};
  set_rect(&q, r);
  memcpy(q.uv, uv, sizeof q.uv);
  push(d, q, t);
}

void civue_dl_image_tinted(civue_draw_list *d, civue_texture t, civue_rect r,
                           const f32 uv[4], const f32 tint[4], bool nearest) {
  civue_quad q = {.color = {tint[0], tint[1], tint[2], tint[3]},
                  .params = {0, CIVUE_QUAD_IMAGE, 0, nearest}};
  set_rect(&q, r);
  memcpy(q.uv, uv, sizeof q.uv);
  push(d, q, t);
}

// the kind of an image quad is 2 here, past the 1 of a pixel grid, and the
// progress is kept where a grid keeps its origin.
void civue_dl_image_reveal(civue_draw_list *d, civue_texture t, civue_rect r,
                           const f32 uv[4], const f32 color[4], f32 progress) {
  civue_quad q = {.color = {color[0], color[1], color[2], color[3]},
                  .border = {progress},
                  .params = {0, CIVUE_QUAD_IMAGE, 2, 0}};
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

void civue_dl_rect_grid(civue_draw_list *d, civue_rect r, civue_color fill,
                        const civue_pixel_grid *grid) {
  civue_quad q = {.params = {0, CIVUE_QUAD_RECT, grid != nullptr}};
  set_rect(&q, r);
  premul(fill, q.color);
  if (grid) {
    encode_grid(grid, q.border);
  }
  push(d, q, 0);
}

void civue_dl_image_grid(civue_draw_list *d, civue_texture t, civue_rect r,
                         const f32 uv[4], bool nearest,
                         const civue_pixel_grid *grid) {
  // hard edges: tiles meeting at fractional pixels leave no seam.
  civue_quad q = {.color = {1, 1, 1, 1},
                  .params = {1, CIVUE_QUAD_IMAGE, grid != nullptr, nearest}};
  set_rect(&q, r);
  memcpy(q.uv, uv, sizeof q.uv);
  if (grid) {
    encode_grid(grid, q.border);
  }
  push(d, q, t);
}

// ---- atlas ----

bool civue_atlas_insert(civue_atlas *a, u32 w, u32 h, const u8 *data,
                        f32 uv[4]) {
  if (!w || !h) {
    memset(uv, 0, 4 * sizeof *uv);
    return true;
  }
  // a texel of padding keeps neighbors out of filtered samples, shelves
  // come in heights rounded to 4 so similar glyphs share them.
  u32 pw = w + 1, ph = (h + 1 + 3) & ~3u;
  if (pw > CIVUE_ATLAS_SIZE || ph > CIVUE_ATLAS_SIZE) {
    return false;
  }
  civue_shelf *best = nullptr;
  vec_each(&a->shelves, s) {
    if (s->height < ph || s->height > ph * 3 / 2 + 4 ||
        s->x + pw > CIVUE_ATLAS_SIZE) {
      continue;
    }
    if (!best || s->height < best->height) {
      best = s;
    }
  }
  if (!best) {
    if (a->top + ph > CIVUE_ATLAS_SIZE) {
      return false;
    }
    vec_push(&a->shelves, ((civue_shelf){a->top, ph, 0}));
    a->top += ph;
    best = &vec_last(&a->shelves);
  }
  u32 x = best->x, y = best->y;
  best->x += pw;
  civue_atlas_upload up = {x, y, w, h};
  usize bpp = a->bytes_per_pixel;

  if (a->keeps) {
    // pages never written to take no memory.
    if (!a->pixels) {
      a->pixels = xcalloc((usize)CIVUE_ATLAS_SIZE * CIVUE_ATLAS_SIZE, bpp);
    }

    for (u32 row = 0; row < h; row++) {
      memcpy(a->pixels + ((usize)(y + row) * CIVUE_ATLAS_SIZE + x) * bpp,
             data + (usize)row * w * bpp, w * bpp);
    }

    // frames drawn on the cpu upload nothing, their glyphs add up.
    if (a->pending.len >= 64) {
      a->stale = true;
      a->pending.len = 0;
    }
  }
  else {
    buf_put(&up.data, data, (usize)w * h * bpp);
  }

  if (!a->stale) {
    vec_push(&a->pending, up);
  }
  f32 s = CIVUE_ATLAS_SIZE;
  uv[0] = x / s, uv[1] = y / s, uv[2] = (x + w) / s, uv[3] = (y + h) / s;
  return true;
}

void civue_atlas_clear(civue_atlas *a) {
  if (a->reusing) {
    a->reusing(a->reusing_ctx);
  }

  a->shelves.len = 0;
  a->top = 0;
  vec_each(&a->pending, p) {
    vec_free(&p->data);
  }
  a->pending.len = 0;
  a->generation++;
}

void civue_atlas_free(civue_atlas *a) {
  civue_atlas_clear(a);
  vec_free(&a->shelves);
  vec_free(&a->pending);
  free(a->pixels);
  a->pixels = nullptr;
}
