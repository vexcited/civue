#include "internal.h"

#include <civue/ui.h>

typedef struct {
  void (*drop)(void *);
  alignas(16) u8 data[];
} Memory;

thread_local civue_ui *civue__ui;

civue_ui *civue_ui_new_sized(civue_gfx *gfx, f32 scale, usize size) {
  civue_ui *ui = own_new(MAX(size, sizeof *ui));
  ui->gfx = gfx;
  ui->text = civue_text_new();
  ui->icons = civue_icons_new();
  ui->scale = scale;
  ui->window_focused = true;
  // the macos buttons' end and their edge margin, until measured.
  ui->titlebar_inset = 80;
  return ui;
}

void civue_input_free(civue_input *in) {
  vec_free(&in->keys);
  vec_free(&in->commands);
  vec_free(&in->held);
  vec_free(&in->text);
  vec_free(&in->preedit);
  vec_free(&in->trail);
  vec_each(&in->drops, d) {
    free(*d);
  }
  vec_free(&in->drops);
}

void civue_ui_free(civue_ui *ui) {
  if (!ui) {
    return;
  }
  if (civue__ui == ui) {
    civue__ui = nullptr;
  }
  CILO_MAP_EACH(e, &ui->memory) {
    Memory *m = (Memory *)e->val;
    if (m->drop) {
      m->drop(m->data);
    }
    own_free(m);
  }
  map_free(&ui->memory);
  civue__tree_free(ui->tree);
  civue_text_free(ui->text);
  civue_icons_free(ui->icons);
  civue_dl_free(&ui->base);
  civue_dl_free(&ui->overlay);
  civue_dl_free(&ui->combined);
  civue_dl_free(&ui->last);
  vec_free(&ui->regions);
  vec_free(&ui->prev_regions);
  vec_free(&ui->anims);
  civue_input_free(&ui->input);
  own_free(ui);
}

static civue_id topmost(const civue_ui *ui, civue_vec2 m, bool overlay) {
  for (u32 i = ui->prev_regions.len; i-- > 0;) {
    const civue_region *r = &ui->prev_regions.v[i];
    if (r->overlay == overlay && rect_contains(r->rect, m)) {
      return r->id;
    }
  }
  return 0;
}

civue_id civue_region_at(const civue_ui *ui, civue_vec2 p) {
  for (u32 k = 0; k < 2; k++) {
    for (u32 i = ui->regions.len; i-- > 0;) {
      const civue_region *r = &ui->regions.v[i];
      if (r->overlay == !k && rect_contains(r->rect, p)) {
        return r->id;
      }
    }
  }
  return 0;
}

void civue_begin_frame(civue_ui *ui, civue_vec2 size, f32 scale,
                       civue_input *input) {
  civue__ui = ui;
  ui->size = size;
  ui->scale = scale;
  // the input moves in, what carries over goes back.
  civue_input taken = *input;
  civue_input old = ui->input;
  ui->input = taken;
  *input = old;
  input->keys.len = 0;
  input->commands.len = 0;
  vec_each(&input->drops, d) {
    free(*d);
  }
  input->drops.len = 0;
  input->text.len = 0;
  input->trail.len = 0;
  input->held.len = 0;
  vec_each(&taken.held, k) {
    vec_push(&input->held, *k);
  }
  input->preedit.len = 0;
  buf_put(&input->preedit, taken.preedit.v, taken.preedit.len);
  input->has_mouse = taken.has_mouse;
  input->mouse = taken.mouse;
  memcpy(input->down, taken.down, sizeof input->down);
  memset(input->pressed, 0, sizeof input->pressed);
  memset(input->released, 0, sizeof input->released);
  input->clicks = 0;
  input->scroll = v2(0, 0);
  input->precise_scroll = false;
  input->pinch = 0;
  input->mods = taken.mods;
  input->pressure = taken.pressure;

  ui->dark = ui->appearance == CIVUE_APPEARANCE_SYSTEM
               ? ui->system_dark
               : ui->appearance == CIVUE_APPEARANCE_DARK;
  ui->now = now_s();
  civue_dl_clear(&ui->base);
  civue_dl_clear(&ui->overlay);
  ui->on_overlay = false;
  ui->repaint = false;
  ui->repaint_at = 0;
  ui->cursor = (civue_cursor){};
  ui->drag_window = false;
  ui->toggle_maximize = false;
  ui->minimize_window = false;
  ui->close_window = false;
  ui->focus_alive = false;
  ui->focus_kept = false;
  ui->frames++;
  ui->rerun = ui->frames > 1 && !ui->settled;
  ui->scroll_areas = 0;
  ui->has_ring_rect = false;
  if (ui->ring_press) {
    ui->ring_press--;
  }
  typeof(ui->regions) swap = ui->prev_regions;
  ui->prev_regions = ui->regions;
  ui->regions = swap;
  ui->regions.len = 0;
  // the topmost region of the last frame under the pointer is hovered,
  // menus and popovers stay above what was drawn after them.
  ui->hovered = 0;
  if (ui->input.has_mouse) {
    ui->hovered = topmost(ui, ui->input.mouse, true);
    if (!ui->hovered) {
      ui->hovered = topmost(ui, ui->input.mouse, false);
    }
  }
  // a release is cleared after this frame, so widgets still see it.
  if (!ui->input.released[0] && !ui->input.down[0]) {
    ui->active = 0;
  }
}

const civue_draw_list *civue_frame_list(civue_ui *ui) {
  // most frames draw nothing over the rest.
  if (!ui->overlay.quads.len) {
    return &ui->base;
  }

  civue_dl_clear(&ui->combined);
  civue_dl_append(&ui->combined, &ui->base);
  civue_dl_append(&ui->combined, &ui->overlay);
  return &ui->combined;
}

void civue__generations(const civue_ui *ui, u64 out[2]) {
  out[0] = ui->gfx ? civue_gfx_atlas(ui->gfx)->generation : 0;
  out[1] = ui->gfx ? civue_gfx_color_atlas(ui->gfx)->generation : 0;
}

const civue_draw_list *civue_frame_changes(civue_ui *ui, civue_color clear,
                                           u32 width, u32 height,
                                           civue_damage *damage) {
  civue_draw_list *list = &ui->base;

  // an atlas that started over shows other glyphs where the last frame's
  // were: quads alike may not draw alike.
  u64 generations[2];
  civue__generations(ui, generations);

  if (memcmp(generations, ui->last_generations, sizeof generations)) {
    civue_frame_forget(ui);
    memcpy(ui->last_generations, generations, sizeof generations);
  }

  if (ui->overlay.quads.len) {
    civue_dl_clear(&ui->combined);
    civue_dl_append(&ui->combined, &ui->base);
    civue_dl_append(&ui->combined, &ui->overlay);
    list = &ui->combined;
  }

  bool comparable = ui->has_last && ui->last_width == width &&
                    ui->last_height == height &&
                    !memcmp(&ui->last_clear, &clear, sizeof clear);

  if (comparable) {
    civue_damage_between(&ui->last, list, width, height, damage);
  }
  else {
    *damage = (civue_damage){.all = true};
  }

  // the frame's quads are kept as they are, the list takes the last
  // frame's room for the next one.
  typeof(list->quads) quads = ui->last.quads;
  typeof(list->batches) batches = ui->last.batches;
  ui->last.quads = list->quads, ui->last.batches = list->batches;
  list->quads = quads, list->batches = batches;
  list->quads.len = 0, list->batches.len = 0;

  ui->last_clear = clear;
  ui->last_width = width, ui->last_height = height;
  ui->has_last = true;
  return &ui->last;
}

void civue_frame_forget(civue_ui *ui) {
  ui->has_last = false;
}

bool civue_end_frame(civue_ui *ui) {
  bool settled = civue__tree_end(ui);
  ui->settled = settled;
  // a press elsewhere, or the focused widget gone, ends the focus.
  if (!ui->focus_alive ||
      (ui->input.pressed[0] && ui->active != ui->focus && !ui->focus_kept)) {
    ui->focus = 0;
  }
  if (ui->input.released[0]) {
    ui->active = 0;
  }
  // what an event changed may be drawn by widgets laid out before it was seen.
  const civue_input *in = &ui->input;
  for (u32 i = 0; i < 3; i++) {
    if (in->pressed[i] || in->released[i]) {
      ui->repaint = true;
    }
  }
  if (in->keys.len || in->text.len || in->drops.len) {
    ui->repaint = true;
  }
  ui->had_mouse = ui->input.has_mouse;
  ui->last_mouse = ui->input.mouse;
  civue_text_end_frame(ui->text);
  // values not asked for this frame are forgotten.
  u32 k = 0;
  vec_each(&ui->anims, a) {
    if (a->used == ui->now) {
      ui->anims.v[k++] = *a;
    }
  }
  ui->anims.len = k;
  return settled;
}

void civue_request_repaint(civue_ui *ui) {
  ui->repaint = true;
}

void civue_request_repaint_after(civue_ui *ui, f64 seconds) {
  f64 t = ui->now + seconds;
  if (!ui->repaint_at || t < ui->repaint_at) {
    ui->repaint_at = t;
  }
}

// ---- layers ----

civue_draw_list *civue_list(civue_ui *ui) {
  return ui->on_overlay ? &ui->overlay : &ui->base;
}

bool civue_begin_overlay(civue_ui *ui) {
  bool was = ui->on_overlay;
  ui->on_overlay = true;
  return was;
}

void civue_end_overlay(civue_ui *ui, bool was) {
  ui->on_overlay = was;
}

static civue_rect px(const civue_ui *ui, civue_rect r) {
  f64 s = ui->scale;
  return rect(round(r.x0 * s), round(r.y0 * s), round(r.x1 * s),
              round(r.y1 * s));
}

void civue_push_clip(civue_ui *ui, civue_rect r) {
  civue_dl_push_clip(civue_list(ui), px(ui, r));
}

void civue_pop_clip(civue_ui *ui) {
  civue_dl_pop_clip(civue_list(ui));
}

civue_rect civue_clip_rect(civue_ui *ui) {
  civue_rect c = civue_dl_clip(civue_list(ui));
  f64 s = ui->scale;
  return rect(c.x0 / s, c.y0 / s, c.x1 / s, c.y1 / s);
}

// ---- interaction ----

bool civue_owns_pointer(civue_ui *ui, civue_id id) {
  return ui->active == id || (!ui->active && ui->hovered == id);
}

bool civue_drawn_last_frame(civue_ui *ui, civue_id id) {
  vec_each(&ui->prev_regions, r) {
    if (r->id == id) {
      return true;
    }
  }
  return false;
}

civue_response civue_interact(civue_ui *ui, civue_id id, civue_rect rect,
                              civue_sense sense) {
  civue_rect visible = rect_intersect(rect, civue_clip_rect(ui));
  if (!rect_empty(visible)) {
    vec_push(&ui->regions, ((civue_region){id, visible, ui->on_overlay}));
  }
  civue_response r = {.rect = rect};
  if (ui->ring == id) {
    ui->has_ring_rect = true;
    ui->ring_rect = rect;
    if (ui->ring_press && sense != CIVUE_SENSE_HOVER) {
      ui->ring_press = 0;
      r.clicked = true;
      return r;
    }
  }
  bool over = ui->input.has_mouse && !rect_empty(visible) &&
              rect_contains(visible, ui->input.mouse);
  // a region new this frame can't be known topmost, its rect is trusted.
  bool known = civue_drawn_last_frame(ui, id);
  r.hovered =
    over && (ui->hovered == id || !known) && (!ui->active || ui->active == id);
  if (sense == CIVUE_SENSE_HOVER) {
    return r;
  }
  if (r.hovered && ui->input.pressed[0]) {
    ui->active = id;
    ui->press_origin = ui->input.mouse;
    r.pressed = true;
    r.double_clicked = ui->input.clicks >= 2;
  }
  if (r.hovered && ui->input.pressed[1]) {
    r.right_clicked = true;
  }
  if (ui->active == id) {
    if (ui->input.down[0]) {
      r.dragging = true;
      if (ui->input.has_mouse && ui->had_mouse) {
        r.drag_delta = v2_sub(ui->input.mouse, ui->last_mouse);
      }
    }
    if (ui->input.released[0]) {
      r.released = true;
      r.clicked = over;
    }
  }
  return r;
}

bool civue_under_overlay(civue_ui *ui, civue_vec2 p) {
  if (ui->on_overlay) {
    return false;
  }
  vec_each(&ui->prev_regions, r) {
    if (r->overlay && rect_contains(r->rect, p)) {
      return true;
    }
  }
  return false;
}

bool civue_pointer_moved(civue_ui *ui) {
  return ui->input.has_mouse != ui->had_mouse ||
         ui->input.mouse.x != ui->last_mouse.x ||
         ui->input.mouse.y != ui->last_mouse.y;
}

void civue_claim_focus(civue_ui *ui) {
  ui->focus_alive = true;
}

void civue_keep_focus(civue_ui *ui) {
  ui->focus_kept = true;
}

void civue_press_ring(civue_ui *ui) {
  if (!ui->ring) {
    return;
  }
  ui->ring_press = 2;
  ui->repaint = true;
}

bool civue_key_pressed(civue_ui *ui, u32 key) {
  vec_each(&ui->input.keys, k) {
    if (k->key == key) {
      return true;
    }
  }
  return false;
}

bool civue_key_held(civue_ui *ui, u32 key) {
  vec_each(&ui->input.held, k) {
    if (*k == key) {
      return true;
    }
  }
  return false;
}

bool civue_shortcut(civue_ui *ui, u32 key, bool shift) {
  vec_each(&ui->input.keys, k) {
    if (k->key == key && (k->mods & CIVUE_MOD_CMD) &&
        !!(k->mods & CIVUE_MOD_SHIFT) == shift) {
      return true;
    }
  }
  return false;
}

// ---- state ----

// ease out cubic, how the ui settles.
static f32 settle(f32 t) {
  f32 u = 1 - t;
  return 1 - u * u * u;
}

static f32 anim_at(const civue_anim *a, f64 now) {
  f32 t = (f32)fmin((now - a->start) / fmax(a->duration, 1e-3), 1);
  return a->from + (a->to - a->from) * settle(t);
}

static f32 animate(civue_ui *ui, civue_id id, bool has_from, f32 from,
                   f32 target, f32 seconds) {
  f64 now = ui->now;
  civue_anim *a = nullptr;
  vec_each(&ui->anims, x) {
    if (x->id == id) {
      a = x;
    }
  }
  if (!a) {
    vec_push(&ui->anims,
             ((civue_anim){id, has_from ? from : target, target, seconds,
                           has_from ? now : now - seconds, now}));
    a = &vec_last(&ui->anims);
  }
  a->used = now;
  f32 current = anim_at(a, now);
  if (a->to != target) {
    *a = (civue_anim){id, current, target, seconds, now, now};
  }
  f32 v = anim_at(a, now);
  if (now - a->start < a->duration) {
    ui->repaint = true;
  }
  return v;
}

f32 civue_animate(civue_ui *ui, civue_id id, f32 target, f32 seconds) {
  return animate(ui, id, false, 0, target, seconds);
}

f32 civue_animate_from(civue_ui *ui, civue_id id, f32 from, f32 target,
                       f32 seconds) {
  return animate(ui, id, true, from, target, seconds);
}

void *civue_memory(civue_ui *ui, civue_id id, usize size,
                   void (*drop)(void *)) {
  u64 v;
  if (map_get(&ui->memory, id, &v)) {
    return ((Memory *)v)->data;
  }
  Memory *m = own_new(sizeof *m + size);
  m->drop = drop;
  map_put(&ui->memory, id, (u64)m);
  return m->data;
}

void civue_forget(civue_ui *ui, civue_id id) {
  u64 v;
  if (!map_get(&ui->memory, id, &v)) {
    return;
  }
  Memory *m = (Memory *)v;
  if (m->drop) {
    m->drop(m->data);
  }
  own_free(m);
  map_del(&ui->memory, id);
}

static f32 bezier_at(f32 a, f32 b, f32 s) {
  return 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s;
}

f32 civue_cubic_bezier(f32 x1, f32 y1, f32 x2, f32 y2, f32 t) {
  t = fminf(fmaxf(t, 0), 1);
  f32 lo = 0, hi = 1;
  for (u32 i = 0; i < 24; ++i) {
    f32 mid = (lo + hi) / 2;
    if (bezier_at(x1, x2, mid) < t) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }
  return bezier_at(y1, y2, (lo + hi) / 2);
}

f32 civue_ease(f32 t) {
  return civue_cubic_bezier(.25, .1, .25, 1, t);
}

// ---- painting ----

static void radii4(f32 r, f32 s, f32 out[4]) {
  out[0] = out[1] = out[2] = out[3] = r * s;
}

void civue_fill(civue_ui *ui, civue_rect r, civue_color c, f32 radius) {
  if (c.a <= 0) {
    return;
  }
  f32 rr[4];
  radii4(radius, ui->scale, rr);
  civue_dl_rect(civue_list(ui), px(ui, r), rr, c, CIVUE_CLEAR, 0);
}

void civue_fill_corners(civue_ui *ui, civue_rect r, civue_color c,
                        const f32 radii[4]) {
  f32 rr[4];
  for (u32 i = 0; i < 4; i++) {
    rr[i] = radii[i] * ui->scale;
  }
  civue_dl_rect(civue_list(ui), px(ui, r), rr, c, CIVUE_CLEAR, 0);
}

void civue_stroke_corners(civue_ui *ui, civue_rect r, civue_color c, f32 width,
                          const f32 radii[4]) {
  f32 rr[4];
  for (u32 i = 0; i < 4; i++) {
    rr[i] = radii[i] * ui->scale;
  }
  f32 w = fmaxf(roundf(width * ui->scale), 1);
  civue_dl_rect(civue_list(ui), px(ui, r), rr, CIVUE_CLEAR, c, w);
}

void civue_stroke_sides(civue_ui *ui, civue_rect r, civue_color c,
                        const f32 widths[4], const f32 radii[4]) {
  f32 rr[4], ww[4];
  for (u32 i = 0; i < 4; i++) {
    rr[i] = radii[i] * ui->scale;
    ww[i] = widths[i] > 0 ? fmaxf(roundf(widths[i] * ui->scale), 1) : 0;
  }

  civue_dl_rect_sides(civue_list(ui), px(ui, r), rr, CIVUE_CLEAR, c, ww);
}

void civue_stroke(civue_ui *ui, civue_rect r, civue_color c, f32 width,
                  f32 radius) {
  civue_fill_stroke(ui, r, CIVUE_CLEAR, c, width, radius);
}

void civue_fill_stroke(civue_ui *ui, civue_rect r, civue_color fill,
                       civue_color stroke, f32 width, f32 radius) {
  f32 rr[4];
  radii4(radius, ui->scale, rr);
  f32 w = fmaxf(roundf(width * ui->scale), 1);
  civue_dl_rect(civue_list(ui), px(ui, r), rr, fill, stroke, w);
}

void civue_shadow(civue_ui *ui, civue_rect r, f32 radius, f32 blur,
                  civue_vec2 offset, civue_color c) {
  f32 s = ui->scale;
  civue_rect moved =
    rect(r.x0 + offset.x, r.y0 + offset.y, r.x1 + offset.x, r.y1 + offset.y);
  civue_dl_shadow(civue_list(ui), px(ui, moved), radius * s, blur * s / 2, c);
}

void civue_hline(civue_ui *ui, f64 x0, f64 x1, f64 y, civue_color c) {
  f64 s = ui->scale, y0 = round(y * s);
  civue_rect r = rect(round(x0 * s), y0, round(x1 * s), y0 + fmax(round(s), 1));
  civue_dl_rect(civue_list(ui), r, (f32[4]){}, c, CIVUE_CLEAR, 0);
}

void civue_vline(civue_ui *ui, f64 x, f64 y0, f64 y1, civue_color c) {
  f64 s = ui->scale, x0 = round(x * s);
  civue_rect r = rect(x0, round(y0 * s), x0 + fmax(round(s), 1), round(y1 * s));
  civue_dl_rect(civue_list(ui), r, (f32[4]){}, c, CIVUE_CLEAR, 0);
}

void civue_line(civue_ui *ui, civue_vec2 a, civue_vec2 b, f32 width,
                civue_color c) {
  f64 s = ui->scale;
  civue_dl_line(civue_list(ui), v2_scale(a, s), v2_scale(b, s),
                fmaxf(width * s, 1), c, false);
}

void civue_band(civue_ui *ui, civue_vec2 a, civue_vec2 b, f32 width,
                civue_color c) {
  f64 s = ui->scale;
  civue_dl_line(civue_list(ui), v2_scale(a, s), v2_scale(b, s),
                fmaxf(width * s, 1), c, true);
}

void civue_image(civue_ui *ui, civue_texture t, civue_rect r, const f32 uv[4],
                 f32 opacity, f32 radius, bool nearest) {
  f64 s = ui->scale;
  // images at fractional positions keep them, the canvas moves smoothly.
  civue_rect p = rect(r.x0 * s, r.y0 * s, r.x1 * s, r.y1 * s);
  civue_dl_image(civue_list(ui), t, p, uv, opacity, radius * s, nearest);
}

void civue_image_px(civue_ui *ui, civue_texture t, civue_rect r,
                    const f32 uv[4], bool nearest) {
  civue_dl_image(civue_list(ui), t, r, uv, 1, 0, nearest);
}

void civue_fade_out(civue_ui *ui, civue_rect r, civue_color color) {
  for (f64 y = r.y0; y < r.y1; y += 2) {
    color.a = (f32)fmin(fmax((y + 1 - r.y0) / rect_h(r), 0), 1);
    civue_fill(ui, rect(r.x0, y, r.x1, fmin(y + 2, r.y1)), color, 0);
  }
}

static void draw_icon(civue_ui *ui, civue_icon icon, civue_rect r,
                      civue_color c, bool mirror) {
  civue_atlas *mask = civue_gfx_atlas(ui->gfx),
              *color = civue_gfx_color_atlas(ui->gfx);
  civue_icons_draw(ui->icons, civue_list(ui), mask, color, icon, px(ui, r), c,
                   mirror);
}

void civue_icon_at(civue_ui *ui, civue_icon icon, civue_rect r, civue_color c) {
  draw_icon(ui, icon, r, c, false);
}

void civue_icon_mirrored(civue_ui *ui, civue_icon icon, civue_rect r, f32 size,
                         civue_color c) {
  f32 w = size, h = size;
  civue_icon_natural_size(ui->icons, icon, &w, &h);
  f64 x = r.x0 + (rect_w(r) - w) / 2, y = r.y0 + (rect_h(r) - h) / 2;
  draw_icon(ui, icon, rect_xywh(x, y, w, h), c, true);
}

void civue_icon_centered(civue_ui *ui, civue_icon icon, civue_rect r, f32 size,
                         civue_color c) {
  f32 w = size, h = size;
  civue_icon_natural_size(ui->icons, icon, &w, &h);
  f64 x = r.x0 + (rect_w(r) - w) / 2, y = r.y0 + (rect_h(r) - h) / 2;
  civue_icon_at(ui, icon, rect_xywh(x, y, w, h), c);
}

// ---- text ----

const civue_text_block *civue_layout_n(civue_ui *ui, const char *s, usize n,
                                       civue_text_style style, f32 max_width) {
  return civue_text_layout(ui->text, s, n, style, max_width, ui->scale);
}

const civue_text_block *civue_layout(civue_ui *ui, const char *s,
                                     civue_text_style style, f32 max_width) {
  return civue_layout_n(ui, s, strlen(s), style, max_width);
}

void civue_draw_text(civue_ui *ui, const civue_text_block *b, civue_vec2 origin,
                     civue_color c) {
  civue_text_draw(ui->text, civue_list(ui), civue_gfx_atlas(ui->gfx),
                  civue_gfx_color_atlas(ui->gfx), b, origin, c, nullptr, 0);
}

f32 civue_text(civue_ui *ui, const char *s, civue_text_style style,
               civue_vec2 origin, civue_color c) {
  const civue_text_block *b = civue_layout(ui, s, style, 0);
  civue_draw_text(ui, b, origin, c);
  return b->width;
}

const civue_text_block *civue_fit(civue_ui *ui, const char *s,
                                  civue_text_style style, f32 max) {
  usize n = strlen(s);
  const civue_text_block *b = civue_layout_n(ui, s, n, style, 0);
  if (b->width <= max + 0.5f || !n) {
    return b;
  }
  f32 dots = civue_layout(ui, "...", style, 0)->width;
  f32 room = max - dots;
  u32 cut = 0;
  for (u32 i = b->ncarets; i-- > 0;) {
    if (b->carets[i].x <= room) {
      cut = b->carets[i].byte;
      break;
    }
  }
  while (cut > 0 && s[cut - 1] == ' ') {
    cut--;
  }
  char *short_s = xfmt("%.*s...", (int)cut, s);
  const civue_text_block *out = civue_layout(ui, short_s, style, 0);
  free(short_s);
  return out;
}

f32 civue_text_in(civue_ui *ui, const char *s, civue_text_style style,
                  civue_rect r, civue_color c, civue_align align) {
  const civue_text_block *b = civue_fit(ui, s, style, (f32)rect_w(r));
  f64 x = align == CIVUE_ALIGN_START    ? r.x0
          : align == CIVUE_ALIGN_CENTER ? r.x0 + (rect_w(r) - b->width) / 2
                                        : r.x1 - b->width;
  f64 y = r.y0 + (rect_h(r) - style.line_height) / 2;
  civue_draw_text(ui, b, v2(x, y), c);
  return b->width;
}

#ifdef __APPLE__
const char *civue_shortcut_label(const char *mac) {
  return mac;
}
#else
// one combination of glyphs, like "⌥⌘B", written as windows and linux do,
// the modifiers first in their order.
static void label_combo(civue_bytes *out, const char *s, usize n) {
  static const struct {
    u32 cp;
    const char *name;
  } keys[] = {{0x232b, "Backspace"}, {0x2326, "Delete"}, {0x21a9, "Enter"},
              {0x23ce, "Enter"},     {0x21b5, "Enter"},  {0x238b, "Esc"},
              {0x21e5, "Tab"},       {0x2423, "Space"},  {0x21de, "PageUp"},
              {0x21df, "PageDown"},  {0x2196, "Home"},   {0x2198, "End"}};

  bool cmd = false, ctrl = false, alt = false, shift = false;
  usize i = 0;
  civue_bytes key = {};
  while (i < n) {
    usize at = i;
    u32 c = utf8_next(s, n, &i);
    if (c == 0x2318) {
      cmd = true;
    }
    else if (c == 0x2303) {
      ctrl = true;
    }
    else if (c == 0x2325) {
      alt = true;
    }
    else if (c == 0x21e7) {
      shift = true;
    }
    else {
      const char *name = nullptr;
      for (u32 k = 0; k < countof(keys); k++) {
        if (keys[k].cp == c) {
          name = keys[k].name;
        }
      }
      if (name) {
        buf_puts(&key, name);
      }
      else {
        buf_put(&key, s + at, i - at);
      }
    }
  }
  // control is the command key there, the super key the mac's control.
  if (cmd) {
    buf_puts(out, "Ctrl+");
  }
  if (ctrl) {
    buf_puts(out, "Super+");
  }
  if (alt) {
    buf_puts(out, "Alt+");
  }
  if (shift) {
    buf_puts(out, "Shift+");
  }
  // a modifier alone, like a hint to hold shift.
  if (!key.len && out->len && out->v[out->len - 1] == '+') {
    out->len--;
  }
  buf_put(out, key.v, key.len);
  vec_free(&key);
}

const char *civue_shortcut_label(const char *mac) {
  if (!mac) {
    return nullptr;
  }
  static civue_map made;
  u64 key = hash_str(mac) | 1, value;
  if (map_get(&made, key, &value)) {
    return (const char *)(uintptr_t)value;
  }
  civue_bytes out = {};
  usize n = strlen(mac), i = 0;
  // combinations apart by spaces, like "⌘K ⌘S", one at a time.
  while (i < n) {
    usize end = i;
    while (end < n && mac[end] != ' ') {
      end++;
    }
    label_combo(&out, mac + i, end - i);
    while (end < n && mac[end] == ' ') {
      buf_putc(&out, mac[end++]);
    }
    i = end;
  }
  char *label = buf_take(&out);
  map_put(&made, key, (u64)(uintptr_t)label);
  return label;
}
#endif
