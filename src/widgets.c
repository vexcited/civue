#include "internal.h"

#include <assert.h>
#include <civue/widgets.h>

static f64 clamp(f64 v, f64 lo, f64 hi) { return fmin(fmax(v, lo), hi); }

// rust's with_alpha replaces alpha rather than multiplying it.
static civue_color alpha(civue_color c, f32 a) {
  c.a = a;
  return c;
}

civue_response civue_button(civue_ui *ui, civue_id id, civue_rect r, const char *label, civue_button_kind kind) {
  civue_theme th = ui->theme;
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);
  bool pressed = resp.dragging && resp.hovered;
  civue_color bg, border = CIVUE_CLEAR, fg = th.text;
  if (kind == CIVUE_BUTTON_PRIMARY) {
    bg = pressed ? th.bg_brand_pressed : resp.hovered ? th.bg_brand_hover : th.bg_brand;
    fg = th.text_onbrand;
  } else if (kind == CIVUE_BUTTON_DANGER) {
    bg = th.bg_danger;
    if (pressed || resp.hovered) bg.a *= pressed ? .8f : .9f;
    fg = th.text_onbrand;
  } else {
    bg = pressed ? th.bg_pressed : resp.hovered ? th.bg_hover : CIVUE_CLEAR;
    if (kind == CIVUE_BUTTON_SECONDARY) border = th.border;
  }
  if (border.a > 0)
    civue_fill_stroke(ui, r, bg, border, 1, CIVUE_RADIUS_MEDIUM);
  else
    civue_fill(ui, r, bg, CIVUE_RADIUS_MEDIUM);
  bool strong = kind == CIVUE_BUTTON_PRIMARY || kind == CIVUE_BUTTON_DANGER;
  civue_text_in(ui, label, strong ? CIVUE_BODY_STRONG : CIVUE_BODY, rect_inflate(r, -8, 0), fg, CIVUE_ALIGN_CENTER);
  return resp;
}

f32 civue_button_width(civue_ui *ui, const char *label, civue_text_style style) {
  return ceilf(civue_layout(ui, label, style, 0)->width) + 24;
}

void civue_dialog_head(civue_ui *ui, civue_id id, civue_rect r, const char *title, bool *close) {
  const civue_theme *th = &ui->theme;
  civue_text_in(ui, title, CIVUE_BODY_STRONG, rect(r.x0 + 16, r.y0 + 12, r.x1 - 40, r.y0 + 28), th->text,
                CIVUE_ALIGN_START);
  if (civue_icon_button(ui, civue_idn(id, 1), rect_xywh(r.x1 - 32, r.y0 + 8, 24, 24), CIVUE_ICON_CLOSE, false).clicked)
    *close = true;
  civue_hline(ui, r.x0, r.x1, r.y0 + 39, th->border);
}

bool civue_slider(civue_ui *ui, civue_id id, civue_rect r, f64 value, f64 min, f64 max, f64 anchor, f64 step,
                  civue_slider_change *out) {
  civue_theme th = ui->theme;
  f64 cy = rect_center(r).y, span = fmax(max - min, 1e-9);
  civue_rect track = rect(r.x0 + 8, cy - 3, r.x1 - 8, cy + 3);
  f64 a = track.x0 + (clamp(anchor, min, max) - min) / span * rect_w(track);
  f64 b = track.x0 + (clamp(value, min, max) - min) / span * rect_w(track);
  civue_response resp = civue_interact(ui, id, rect_inflate(r, 0, 10), CIVUE_SENSE_DRAG);
  civue_fill(ui, track, th.bg_tertiary, 3);
  civue_fill(ui, rect(fmin(a, b), track.y0, fmax(a, b), track.y1), th.bg_brand, 3);
  civue_rect knob = rect(b - 6, cy - 6, b + 6, cy + 6);
  civue_elevation_shadow(ui, knob, 6, CIVUE_E100_CANVAS);
  civue_fill(ui, knob, CIVUE_WHITE, 6);
  if (resp.dragging && ui->input.has_mouse) {
    f64 k = clamp((ui->input.mouse.x - track.x0) / fmax(rect_w(track), 1e-6), 0, 1);
    f64 v = min + round(k * span / step) * step;
    if (fabs(v - value) > 1e-9) {
      *out = (civue_slider_change){v, false};
      return true;
    }
  }
  if (resp.released) {
    *out = (civue_slider_change){value, true};
    return true;
  }
  return false;
}

civue_response civue_icon_button(civue_ui *ui, civue_id id, civue_rect r, civue_icon icon, bool selected) {
  civue_theme th = ui->theme;
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);
  civue_color bg = selected                        ? th.bg_brand
                   : resp.dragging && resp.hovered ? th.bg_pressed
                   : resp.hovered                  ? th.bg_hover
                                                   : CIVUE_CLEAR;
  civue_fill(ui, r, bg, CIVUE_RADIUS_MEDIUM);
  civue_icon_centered(ui, icon, r, rect_w(r) >= 32 ? 24 : 16, selected ? th.icon_onbrand : th.icon);
  return resp;
}

civue_response civue_panel_icon(civue_ui *ui, civue_id id, civue_rect r, civue_icon icon) {
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);
  if (resp.hovered) civue_fill(ui, r, ui->theme.bg_hover, CIVUE_RADIUS_MEDIUM);
  civue_icon_centered(ui, icon, r, 16, ui->theme.icon);
  return resp;
}

static f32 elevation_hair(civue_ui *ui, civue_elevation e) { return e % 2 && ui->scale <= 1.5 ? 1 : 0.5; }

void civue_elevation_shadow(civue_ui *ui, civue_rect r, f32 radius, civue_elevation e) {
  f32 h = elevation_hair(ui, e);
  bool dark = ui->theme.dark;
  typedef struct {
    f32 y, blur, a;
  } Shadow;
  Shadow s[3] = {};
  u32 n = 0;
  switch (e / 2) {
  case 0:
    s[0] = (Shadow){1, 3, dark ? .4 : .15};
    s[1] = (Shadow){0, h, dark ? .5 : .3};
    n = 2;
    break;
  case 1:
    s[0] = (Shadow){1, 3, dark ? .5 : .1};
    s[1] = (Shadow){3, 8, dark ? .35 : .1};
    s[2] = (Shadow){0, h, .18};
    n = dark ? 2 : 3;
    break;
  case 2:
    s[0] = (Shadow){1, 3, dark ? .5 : .1};
    s[1] = (Shadow){5, 12, dark ? .35 : .13};
    s[2] = (Shadow){0, h, .15};
    n = dark ? 2 : 3;
    break;
  case 3:
    s[0] = (Shadow){2, 5, dark ? .35 : .15};
    s[1] = (Shadow){10, 16, dark ? .35 : .12};
    s[2] = (Shadow){0, h, .12};
    n = dark ? 2 : 3;
    break;
  case 4:
    s[0] = (Shadow){dark ? 3 : 2, 5, dark ? .35 : .15};
    s[1] = (Shadow){10, 24, dark ? .45 : .18};
    s[2] = (Shadow){0, h, .08};
    n = dark ? 2 : 3;
    break;
  }
  for (u32 j = 0; j < n; ++j) {
    Shadow v = s[e % 2 ? n - j - 1 : j];
    civue_shadow(ui, r, radius, v.blur, v2(0, v.y), alpha(CIVUE_BLACK, v.a));
  }
}

void civue_elevation_edge(civue_ui *ui, civue_rect r, f32 radius, civue_elevation e) {
  if (!ui->theme.dark) return;
  f32 h = elevation_hair(ui, e);
  f32 ring = e < CIVUE_E400 ? .3 : .35, top = e < CIVUE_E200 ? .1 : .08;
  civue_stroke(ui, r, alpha(CIVUE_WHITE, ring * .11), h, radius);
  f32 inset = fmin(radius * .6, rect_w(r) / 2);
  civue_fill(ui, rect(r.x0 + inset, r.y0, r.x1 - inset, r.y0 + h), alpha(CIVUE_WHITE, top), 0);
}

void civue_floating(civue_ui *ui, civue_rect r, f32 radius, civue_color fill, civue_elevation e) {
  civue_elevation_shadow(ui, r, radius, e);
  civue_fill(ui, r, fill, radius);
  civue_elevation_edge(ui, r, radius, e);
}

void civue_field(civue_ui *ui, civue_rect r, civue_label label, const char *value) {
  civue_theme th = ui->theme;
  civue_fill(ui, r, th.bg_secondary, CIVUE_RADIUS_MEDIUM);
  civue_rect lr = rect(r.x0, r.y0, r.x0 + 24, r.y1);
  if (label.kind == CIVUE_LABEL_TEXT || label.kind == CIVUE_LABEL_STRONG)
    civue_text_in(ui, label.text, CIVUE_BODY_REGULAR, lr,
                  label.kind == CIVUE_LABEL_STRONG ? th.text : th.text_secondary, CIVUE_ALIGN_CENTER);
  else if (label.kind == CIVUE_LABEL_ICON)
    civue_icon_centered(ui, label.icon, lr, 16, th.icon_secondary);
  civue_text_in(ui, value, CIVUE_BODY, rect(r.x0 + (label.kind == CIVUE_LABEL_NONE ? 8 : 24), r.y0, r.x1 - 8, r.y1),
                th.text, CIVUE_ALIGN_START);
}

civue_response civue_dropdown_outlined(civue_ui *ui, civue_id id, civue_rect r, const char *value) {
  civue_theme th = ui->theme;
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);
  civue_fill_stroke(ui, r, resp.hovered ? th.bg_hover : th.bg, th.border, 1, CIVUE_RADIUS_MEDIUM);
  civue_text_in(ui, value, CIVUE_BODY, rect(r.x0 + 9, r.y0, r.x1 - 24, r.y1), th.text, CIVUE_ALIGN_START);
  civue_icon_centered(ui, CIVUE_ICON_CARET_DOWN, rect(r.x1 - 25, r.y0, r.x1 - 1, r.y1), 12, th.icon);
  return resp;
}

civue_response civue_dropdown(civue_ui *ui, civue_id id, civue_rect r, const char *value, bool placeholder) {
  civue_theme th = ui->theme;
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);
  civue_fill_stroke(ui, r, th.bg_secondary, resp.hovered ? th.border : th.bg_secondary, 1, CIVUE_RADIUS_MEDIUM);
  civue_text_in(ui, value, CIVUE_BODY, rect(r.x0 + 8, r.y0, r.x1 - 24, r.y1), placeholder ? th.text_tertiary : th.text,
                CIVUE_ALIGN_START);
  civue_icon_centered(ui, CIVUE_ICON_CARET_DOWN, rect(r.x1 - 24, r.y0, r.x1, r.y1), 12, th.icon_secondary);
  return resp;
}

i32 civue_segmented(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, u32 count, i32 selected) {
  return civue_segmented_dimmed(ui, id, r, icons, count, selected, false);
}
i32 civue_segmented_dimmed(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, u32 count, i32 selected,
                           bool dimmed) {
  return civue_segmented_sized(ui, id, r, icons, count, selected, dimmed, 16);
}
i32 civue_segmented_sized(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, u32 count, i32 selected,
                          bool dimmed, f32 size) {
  return civue_segmented_tips(ui, id, r, icons, count, nullptr, 0, selected, dimmed, size);
}
i32 civue_segmented_tips(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, u32 count,
                         const char *const *tips, u32 ntips, i32 selected, bool dimmed, f32 size) {
  civue_theme th = ui->theme;
  civue_fill(ui, r, th.bg_secondary, CIVUE_RADIUS_MEDIUM);
  if (!count) return -1;
  f64 w = rect_w(r) / count;
  i32 chosen = -1;
  for (u32 i = 0; i < count; ++i) {
    civue_rect cell = rect_xywh(r.x0 + i * w, r.y0, w, rect_h(r));
    civue_response resp = civue_interact(ui, civue_idn(id, i), cell, CIVUE_SENSE_CLICK);
    if (selected == (i32)i) {
      civue_rect raised = rect_inflate(cell, -1, -1);
      if (!th.dark) civue_elevation_shadow(ui, raised, 3, CIVUE_E100_CANVAS);
      civue_fill(ui, raised, th.dark ? th.bg_tertiary : th.bg, 3);
    }
    civue_color c = dimmed ? th.icon_tertiary : selected == (i32)i || resp.hovered ? th.icon : th.icon_secondary;
    civue_icon_centered(ui, icons[i], cell, size, c);
    if (i < ntips && tips[i]) civue_tooltip(ui, civue_idn(civue_ids(id, "tip"), i), &resp, tips[i]);
    if (resp.clicked && !dimmed) chosen = i;
  }
  return chosen;
}

i32 civue_button_group(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, u32 count, bool enabled) {
  return civue_button_group_dimmed(ui, id, r, icons, count, enabled, false);
}
i32 civue_button_group_dimmed(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, u32 count, bool enabled,
                              bool dimmed) {
  if (!count) return -1;
  civue_theme th = ui->theme;
  enabled = enabled && !dimmed;
  f64 w = rect_w(r) / count;
  i32 chosen = -1;
  for (u32 i = 0; i < count; ++i) {
    civue_rect cell = rect_xywh(r.x0 + i * w, r.y0, w - (i + 1 < count ? 1 : 0), rect_h(r));
    civue_response resp = civue_interact(ui, civue_idn(id, i), cell, CIVUE_SENSE_CLICK);
    f32 first = i == 0 ? CIVUE_RADIUS_MEDIUM : 0, last = i + 1 == count ? CIVUE_RADIUS_MEDIUM : 0;
    civue_fill_corners(ui, cell, resp.hovered && enabled ? th.bg_pressed : th.bg_secondary,
                       (f32[4]){first, last, last, first});
    civue_icon_centered(ui, icons[i], cell, 16, dimmed ? th.icon_tertiary : th.icon);
    if (resp.clicked && enabled) chosen = i;
  }
  return chosen;
}

static bool checkbox_at(civue_ui *ui, civue_id id, civue_vec2 at, bool checked, const char *label, bool brand) {
  civue_theme th = ui->theme;
  f32 w = civue_layout(ui, label, CIVUE_BODY, 0)->width + 24;
  civue_rect r = rect_xywh(at.x, at.y, w, 16), b = rect_xywh(at.x, at.y, 16, 16);
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);
  if (checked) {
    if (brand)
      civue_fill_stroke(ui, b, th.bg_brand, color_hex(0x0c86e1), 1, CIVUE_RADIUS_SMALL);
    else
      civue_stroke(ui, b, th.icon, 1, CIVUE_RADIUS_SMALL);
    civue_icon_centered(ui, CIVUE_ICON_CHECK, b, 12, brand ? CIVUE_WHITE : th.icon);
  } else {
    civue_color edge = alpha(th.dark ? CIVUE_WHITE : CIVUE_BLACK, .2);
    civue_fill_stroke(ui, b, th.bg_secondary, edge, 1, CIVUE_RADIUS_SMALL);
  }
  civue_text_in(ui, label, CIVUE_BODY, rect(at.x + 24, at.y, r.x1, at.y + 16), th.text, CIVUE_ALIGN_START);
  return resp.clicked;
}
bool civue_checkbox(civue_ui *ui, civue_id id, civue_vec2 at, bool checked, const char *label) {
  return checkbox_at(ui, id, at, checked, label, false);
}
bool civue_checkbox_brand(civue_ui *ui, civue_id id, civue_vec2 at, bool checked, const char *label) {
  return checkbox_at(ui, id, at, checked, label, true);
}
void civue_switch_draw(civue_ui *ui, civue_rect r, bool on) {
  civue_fill(ui, r, on ? ui->theme.bg_brand : ui->theme.bg_tertiary, rect_h(r) / 2);
  f64 k = rect_h(r) - 4, x = on ? r.x1 - 2 - k : r.x0 + 2;
  civue_fill(ui, rect_xywh(x, r.y0 + 2, k, k), CIVUE_WHITE, k / 2);
}

typedef struct {
  f32 offset, target, x;
  bool has_scroll, dragging, vertical;
  f64 last_scroll;
  f32 from, start;
} Scroll;

typedef struct {
  civue_id id;
  civue_rect r, vt, ht;
  f32 max, max_x, vl, hl, offset, x;
  bool hovered, recently, dragging, vertical, on_bars;
} ScrollFrame;

typedef struct {
  Vec(ScrollFrame) frames;
} ScrollStack;

static void scroll_stack_free(void *p) {
  ScrollStack *s = p;
  vec_free(&s->frames);
}
static ScrollStack *scroll_stack(civue_ui *ui) {
  return civue_memory(ui, civue_id_of("widgets-scroll-stack"), sizeof(ScrollStack), scroll_stack_free);
}
static Scroll *scroll_state(civue_ui *ui, civue_id id) { return civue_memory(ui, id, sizeof(Scroll), nullptr); }

f32 civue_scroll_x(civue_ui *ui, civue_id id) { return scroll_state(ui, id)->x; }
void civue_set_scroll_y(civue_ui *ui, civue_id id, f32 y) {
  Scroll *s = scroll_state(ui, id);
  s->offset = s->target = fmaxf(y, 0);
}
void civue_set_scroll_x(civue_ui *ui, civue_id id, f32 x) { scroll_state(ui, id)->x = fmaxf(x, 0); }
void civue_scroll_to_center(civue_ui *ui, civue_id id, f32 y0, f32 y1, f32 view, bool always) {
  Scroll *s = scroll_state(ui, id);
  if (always || y0 < s->target || y1 > s->target + view) s->target = fmaxf((y0 + y1) / 2 - view / 2, 0);
}
void civue_scroll_to(civue_ui *ui, civue_id id, f32 y0, f32 y1, f32 view) {
  Scroll *s = scroll_state(ui, id);
  if (y0 < s->target)
    s->target = y0;
  else if (y1 > s->target + view)
    s->target = y1 - view;
}

civue_rect civue_scroll_area(civue_ui *ui, civue_id id, civue_rect r, f32 content_height) {
  return civue_scroll_area_xy(ui, id, r, rect_w(r), content_height);
}

static f32 thumb_len(f32 track, f32 view, f32 content) {
  return fminf(fmaxf((track - 4) * view / fmaxf(content, 1), 24), track - 4);
}

civue_rect civue_scroll_area_xy(civue_ui *ui, civue_id id, civue_rect r, f32 cw, f32 ch) {
  f32 max = fmax(ch - rect_h(r), 0), mx = fmax(cw - rect_w(r), 0);
  civue_vec2 m = ui->input.mouse;
  bool pointer = ui->input.has_mouse;
  bool hovered = pointer && rect_contains(r, m) && !civue_under_overlay(ui, m) &&
                 (ui->on_overlay || rect_contains(civue_clip_rect(ui), m));
  f64 wx = ui->input.scroll.x, wy = ui->input.scroll.y;
  if ((ui->input.mods & CIVUE_MOD_SHIFT) && wx == 0) {
    wx = wy;
    wy = 0;
  }
  civue_rect vt = rect(r.x1 - 10, r.y0, r.x1, r.y1 - 8);
  civue_rect ht = rect(r.x0, r.y1 - 10, r.x1 - 8, r.y1);
  f32 vl = thumb_len(rect_h(vt), rect_h(r), ch);
  f32 hl = thumb_len(rect_w(ht), rect_w(r), cw);
  Scroll *s = scroll_state(ui, id);
  bool scrolled = false;
  if (hovered && wy != 0 && max > 0) {
    s->target = clamp(s->target - wy, 0, max);
    if (ui->input.precise_scroll) s->offset = s->target;
    scrolled = true;
  }
  if (hovered && wx != 0 && mx > 0) {
    s->x = clamp(s->x - wx, 0, mx);
    scrolled = true;
  }
  if (pointer && ui->input.pressed[0]) {
    if (max > 0 && rect_contains(vt, m)) {
      f32 y = vt.y0 + 2 + (rect_h(vt) - 4 - vl) * s->offset / max;
      if (m.y >= y && m.y < y + vl) {
        s->dragging = true;
        s->vertical = true;
        s->from = m.y;
        s->start = s->offset;
      } else
        s->target = clamp(s->target + (m.y < y ? -rect_h(r) : rect_h(r)), 0, max);
      scrolled = true;
    } else if (mx > 0 && rect_contains(ht, m)) {
      f32 x = ht.x0 + 2 + (rect_w(ht) - 4 - hl) * s->x / mx;
      if (m.x >= x && m.x < x + hl) {
        s->dragging = true;
        s->vertical = false;
        s->from = m.x;
        s->start = s->x;
      } else
        s->x = clamp(s->x + (m.x < x ? -rect_w(r) : rect_w(r)), 0, mx);
      scrolled = true;
    }
  }
  if (s->dragging && pointer) {
    if (ui->input.down[0]) {
      if (s->vertical) {
        f32 k = max / fmax(rect_h(vt) - 4 - vl, 1);
        s->offset = s->target = clamp(s->start + (m.y - s->from) * k, 0, max);
      } else {
        f32 k = mx / fmax(rect_w(ht) - 4 - hl, 1);
        s->x = clamp(s->start + (m.x - s->from) * k, 0, mx);
      }
      scrolled = true;
    } else
      s->dragging = false;
  }
  if (scrolled) {
    s->has_scroll = true;
    s->last_scroll = ui->now;
  }
  s->target = clamp(s->target, 0, max);
  s->offset = clamp(s->offset, 0, max);
  s->x = clamp(s->x, 0, mx);
  bool moving = fabsf(s->target - s->offset) > .5;
  if (moving) {
    s->offset += (s->target - s->offset) * .35;
    civue_request_repaint(ui);
  } else
    s->offset = s->target;
  if (hovered) ui->input.scroll = v2(0, 0);
  ScrollFrame f = {.id = id,
                   .r = r,
                   .vt = vt,
                   .ht = ht,
                   .max = max,
                   .max_x = mx,
                   .vl = vl,
                   .hl = hl,
                   .offset = s->offset,
                   .x = s->x,
                   .hovered = hovered,
                   .recently = s->has_scroll && ui->now - s->last_scroll < 1,
                   .dragging = s->dragging,
                   .vertical = s->vertical,
                   .on_bars = pointer && ((max > 0 && rect_contains(vt, m)) || (mx > 0 && rect_contains(ht, m)))};
  vec_push(&scroll_stack(ui)->frames, f);
  civue_rect content = rect_xywh(r.x0 - s->x, r.y0 - s->offset, fmax(cw, rect_w(r)), fmax(ch, rect_h(r)));
  civue_push_clip(ui, r);
  return content;
}

void civue_scroll_end(civue_ui *ui) {
  ScrollStack *stack = scroll_stack(ui);
  assert(stack->frames.len);
  ScrollFrame f = stack->frames.v[--stack->frames.len];
  civue_pop_clip(ui);
  if (f.on_bars || f.dragging) {
    civue_interact(ui, civue_ids(f.id, "bars-over"), f.max > 0 ? f.vt : f.ht, CIVUE_SENSE_DRAG);
    if (f.max_x > 0) civue_interact(ui, civue_ids(f.id, "bars-over-x"), f.ht, CIVUE_SENSE_DRAG);
  }
  if (f.max <= 0 && f.max_x <= 0) return;
  f32 show = civue_animate(ui, civue_ids(f.id, "bar"), f.hovered || f.recently || f.dragging ? 1 : 0, .2);
  if (f.recently) civue_request_repaint_after(ui, 1.05);
  if (show <= .01) return;
  civue_theme th = ui->theme;
  civue_color thumb = alpha(color_hex(0xb3b3b3), .5 * show);
  f32 gray = th.dark ? .22 : .96;
  civue_color track = {gray, gray, gray, .5 * show};
  bool pointer = ui->input.has_mouse;
  civue_vec2 m = ui->input.mouse;
  civue_push_clip(ui, f.r);
  if (f.max > 0) {
    civue_rect t = f.vt;
    if ((pointer && rect_contains(t, m)) || (f.dragging && f.vertical)) {
      civue_fill(ui, t, track, 0);
      civue_fill(ui, rect(t.x0, t.y0, t.x0 + 1, t.y1), alpha(th.border, show), 0);
    }
    f32 y = t.y0 + 2 + (rect_h(t) - 4 - f.vl) * f.offset / f.max;
    civue_fill(ui, rect(t.x0 + 2, y, t.x1 - 2, y + f.vl), thumb, 3);
  }
  if (f.max_x > 0) {
    civue_rect t = f.ht;
    if ((pointer && rect_contains(t, m)) || (f.dragging && !f.vertical)) {
      civue_fill(ui, t, track, 0);
      civue_fill(ui, rect(t.x0, t.y0, t.x1, t.y0 + 1), alpha(th.border, show), 0);
    }
    f32 x = t.x0 + 2 + (rect_w(t) - 4 - f.hl) * f.x / f.max_x;
    civue_fill(ui, rect(x, t.y0 + 2, x + f.hl, t.y1 - 2), thumb, 3);
  }
  civue_pop_clip(ui);
}

f32 civue_vertical_splitter(civue_ui *ui, civue_id id, f32 x, f32 y0, f32 y1) {
  civue_response resp = civue_interact(ui, id, rect(x - 3, y0, x + 3, y1), CIVUE_SENSE_DRAG);
  if (resp.hovered || resp.dragging) civue_set_cursor(ui, CIVUE_CURSOR_RESIZE_H);
  return resp.dragging ? resp.drag_delta.x : 0;
}

civue_menu_style civue_menu_style_default(void) {
  return (civue_menu_style){.separator = 17, .gap = 4, .focus_row = -1};
}

static bool menu_regular(const civue_menu_item *item) { return !item->header && !item->separator && !item->draw; }

static civue_vec2 menu_lead(const civue_menu_item *items, u32 count) {
  bool checks = false, pictures = false;
  for (u32 i = 0; i < count; ++i) {
    const civue_menu_item *it = &items[i];
    if (!menu_regular(it)) continue;
    checks |= it->checked || it->checkbox;
    pictures |= it->has_icon || it->image.kind != CIVUE_MENU_IMAGE_NONE;
  }
  return v2(pictures ? checks ? 52 : 36 : checks ? 24 : 8, checks ? 28 : 8);
}

static civue_vec2 item_lead(const civue_menu_item *items, u32 count, const civue_menu_item *item,
                            const civue_menu_style *style) {
  if (!style->compact) {
    civue_vec2 lead = menu_lead(items, count);
    if (lead.x == 8 && style->label_lead > 0) lead.x = style->label_lead;
    return lead;
  }
  bool checks = item->checked || item->checkbox;
  if (!style->row_checks) {
    checks = false;
    for (u32 i = 0; i < count; ++i) checks |= menu_regular(&items[i]) && (items[i].checked || items[i].checkbox);
  }
  bool picture = item->has_icon || item->image.kind != CIVUE_MENU_IMAGE_NONE;
  f32 px = checks ? 24 : 8;
  return v2(picture ? px + 28 : checks ? 24 : 8, px);
}

static f32 item_height(const civue_menu_item *item, const civue_menu_style *style) {
  return item->separator ? style->separator : item->draw ? item->height : 24;
}

static civue_vec2 menu_size_min(civue_ui *ui, const civue_menu_item *items, u32 count, const civue_menu_style *style,
                                f32 min) {
  f32 w = style->compact ? 0 : min, h = 16;
  for (u32 i = 0; i < count; ++i) {
    const civue_menu_item *it = &items[i];
    h += item_height(it, style);
    if (it->separator || it->draw) continue;
    f32 l = civue_layout(ui, it->label ? it->label : "", CIVUE_BODY, 0)->width;
    if (it->header) {
      w = fmaxf(w, l + 32);
      continue;
    }
    f32 lead = item_lead(items, count, it, style).x;
    f32 b = it->badge ? civue_layout(ui, it->badge, CIVUE_BODY, 0)->width + 24 : 0;
    f32 sc = it->shortcut ? civue_layout(ui, civue_shortcut_label(it->shortcut), CIVUE_BODY, 0)->width : 0;
    f32 row;
    if (style->compact) {
      f32 s = it->shortcut ? 20 + sc + 17.5 : 25.5;
      row = l + fmaxf(s, it->submenu ? 40 : 0);
    } else if (it->submenu)
      row = l + 44;
    else
      row = fmaxf(l, 112) + 8 + sc + 16;
    w = fmaxf(w, 8 + lead + fmaxf(row, l + b));
  }
  if (style->width > 0)
    w = style->width;
  else if (style->compact)
    w = ceilf(w);
  return v2(w, h);
}

static void menu_surface(civue_ui *ui, civue_rect r) {
  civue_elevation_shadow(ui, r, CIVUE_RADIUS_LARGE, CIVUE_E400);
  civue_fill(ui, r, CIVUE_MENU_BG, CIVUE_RADIUS_LARGE);
}

void civue_menu_image_draw(civue_ui *ui, civue_menu_image img, civue_rect r) {
  if (img.kind == CIVUE_MENU_IMAGE_TEXTURE) {
    civue_image(ui, img.texture, r, (f32[4]){0, 0, 1, 1}, 1, rect_w(r) / 2, false);
  } else if (img.kind == CIVUE_MENU_IMAGE_INITIAL) {
    civue_color c = img.color;
    civue_fill(ui, r, c, rect_w(r) / 2);
    civue_text_style style = CIVUE_BODY_MEDIUM;
    style.size = round(rect_h(r) * .5);
    civue_color ink = c.r > .9 && c.g > .7 && c.b < .3 ? alpha(CIVUE_BLACK, .9 * c.a) : alpha(CIVUE_WHITE, c.a);
    civue_text_in(ui, img.initial ? img.initial : "", style, r, ink, CIVUE_ALIGN_CENTER);
  }
}

typedef struct {
  bool initialized;
  i32 row;
} MenuOpen;
typedef struct {
  u64 seen;
  i32 row;
} MenuKeys;

static bool menu_at(civue_ui *ui, civue_id id, civue_rect r, const civue_menu_item *items, u32 count,
                    const civue_menu_style *style, u32 *path, u32 level, u32 *depth) {
  if (level >= CIVUE_MENU_MAX_DEPTH) return false;
  menu_surface(ui, r);
  civue_interact(ui, civue_ids(id, "bg"), r, CIVUE_SENSE_CLICK);
  civue_vec2 lead = menu_lead(items, count);
  MenuOpen *os = civue_memory(ui, civue_ids(id, "open"), sizeof(MenuOpen), nullptr);
  if (!os->initialized) {
    os->initialized = true;
    os->row = -1;
  }
  i32 open = os->row;
  MenuKeys *ks = civue_memory(ui, civue_ids(id, "keys"), sizeof(MenuKeys), nullptr);
  i32 kbd = ks->row;
  civue_u32s rows = {};
  for (u32 i = 0; i < count; ++i)
    if (menu_regular(&items[i]) && !items[i].disabled) vec_push(&rows, i);
  if (!ks->seen || ks->seen + 1 != ui->frames) {
    kbd = -1;
    for (u32 i = 0; i < rows.len; ++i)
      if ((i32)rows.v[i] == style->focus_row) kbd = style->focus_row;
    if (kbd < 0 && style->focus_first && rows.len) kbd = rows.v[0];
  }
  bool chosen = false;
  vec_each(&ui->input.keys, key) {
    i32 at = -1;
    for (u32 i = 0; i < rows.len; ++i)
      if ((i32)rows.v[i] == kbd) at = i;
    if (key->key == CIVUE_KEY_DOWN && rows.len)
      kbd = rows.v[at < 0 ? 0 : ((u32)at + 1) % rows.len];
    else if (key->key == CIVUE_KEY_UP && rows.len)
      kbd = rows.v[at < 0 ? rows.len - 1 : ((u32)at + rows.len - 1) % rows.len];
    else if (key->key == CIVUE_KEY_ENTER && kbd >= 0 && (u32)kbd < count && !items[kbd].submenu) {
      path[level] = kbd;
      *depth = level + 1;
      chosen = true;
    }
  }
  vec_free(&rows);
  if (civue_pointer_moved(ui) && ui->input.has_mouse && rect_contains(r, ui->input.mouse)) kbd = -1;
  *ks = (MenuKeys){ui->frames, kbd};
  f32 y = r.y0 + 8;
  i32 sub = -1;
  civue_rect subrow = {};
  for (u32 i = 0; i < count; ++i) {
    const civue_menu_item *it = &items[i];
    f32 h = item_height(it, style);
    if (it->separator) {
      civue_hline(ui, r.x0 + style->separator_inset, r.x1 - style->separator_inset, y + floorf(h / 2),
                  CIVUE_MENU_BORDER);
    } else if (it->header) {
      f32 x = r.x0 + 8 + (lead.y > 8 && !style->row_checks ? 24 : 8);
      civue_text_in(ui, it->label ? it->label : "", CIVUE_BODY, rect(x, y, r.x1 - 16, y + h), CIVUE_MENU_HEADER,
                    CIVUE_ALIGN_START);
    } else if (it->draw) {
      if (it->draw(ui, rect(r.x0, y, r.x1, y + h), it->ctx)) {
        path[level] = i;
        *depth = level + 1;
        chosen = true;
      }
    } else {
      civue_vec2 il = item_lead(items, count, it, style);
      civue_rect row = rect(r.x0 + 8, y, r.x1 - 8, y + 24);
      civue_response resp = civue_interact(ui, civue_idn(id, i), row, CIVUE_SENSE_CLICK);
      if (resp.hovered && !it->disabled) open = it->submenu ? (i32)i : -1;
      bool lit = (resp.hovered || open == (i32)i || kbd == (i32)i) && !it->disabled;
      if (lit) civue_fill(ui, row, style->has_accent ? style->accent : CIVUE_MENU_HOVER, CIVUE_RADIUS_MEDIUM);
      civue_color c = it->disabled ? CIVUE_MENU_TEXT_DISABLED : CIVUE_MENU_TEXT;
      if (it->checked) civue_icon_centered(ui, CIVUE_ICON_CHECK, rect(row.x0, row.y0, row.x0 + 24, row.y1), 16, c);
      if (it->has_icon) civue_icon_centered(ui, it->icon, rect_xywh(row.x0 + il.y, row.y0, 24, 24), 16, c);
      if (it->image.kind != CIVUE_MENU_IMAGE_NONE)
        civue_menu_image_draw(ui, it->image, rect_xywh(row.x0 + il.y + 4, row.y0 + 4, 16, 16));
      civue_text_in(ui, it->label ? it->label : "", style->has_label_style ? style->label_style : CIVUE_BODY,
                    rect(row.x0 + il.x, row.y0, row.x1 - 8, row.y1), c, CIVUE_ALIGN_START);
      if (it->shortcut) {
        civue_color sc = it->disabled ? CIVUE_MENU_TEXT_DISABLED : lit ? CIVUE_MENU_TEXT : CIVUE_MENU_TEXT_SECONDARY;
        civue_text_in(ui, civue_shortcut_label(it->shortcut), CIVUE_BODY, rect(row.x0, row.y0, row.x1 - 9.5, row.y1),
                      sc, CIVUE_ALIGN_END);
      }
      if (it->badge) {
        f32 w = ceilf(civue_layout(ui, it->badge, CIVUE_BODY, 0)->width) + 8;
        civue_rect br = rect(row.x1 - 8 - w, row.y0 + 4, row.x1 - 8, row.y0 + 20);
        civue_fill(ui, br, lit ? alpha(CIVUE_WHITE, .2) : CIVUE_MENU_BADGE, CIVUE_RADIUS_MEDIUM);
        civue_text_in(ui, it->badge, CIVUE_BODY, br, CIVUE_MENU_TEXT, CIVUE_ALIGN_CENTER);
      }
      if (it->submenu)
        civue_icon_centered(ui, CIVUE_ICON_CARET_RIGHT, rect(row.x1 - 24, row.y0, row.x1 - 4, row.y1), 12, c);
      if (open == (i32)i) {
        sub = i;
        subrow = row;
      }
      if (resp.clicked && !it->disabled && !it->submenu) {
        path[level] = i;
        *depth = level + 1;
        chosen = true;
      }
    }
    y += h;
  }
  os->row = open;
  if (sub >= 0 && items[sub].submenu && level + 1 < CIVUE_MENU_MAX_DEPTH) {
    civue_menu_style ss = *style;
    ss.width = 0;
    const civue_menu_item *it = &items[sub];
    civue_vec2 size = menu_size_min(ui, it->submenu, it->nsubmenu, &ss, 152);
    f32 x = r.x1 - 4;
    if (x + size.x > ui->size.x - 8) x = r.x0 - size.x + 4;
    f32 sy = fmax(fmin(subrow.y0 - 8, ui->size.y - 8 - size.y), 8);
    u32 child_path[CIVUE_MENU_MAX_DEPTH];
    memcpy(child_path, path, level * sizeof(u32));
    child_path[level] = sub;
    u32 child_depth = 0;
    if (menu_at(ui, civue_idn(civue_ids(id, "sub"), sub), rect_xywh(x, sy, size.x, size.y), it->submenu, it->nsubmenu,
                &ss, child_path, level + 1, &child_depth)) {
      memcpy(path, child_path, child_depth * sizeof(u32));
      *depth = child_depth;
      chosen = true;
    }
  }
  return chosen;
}

static bool scrolled_menu(civue_ui *ui, civue_id id, civue_rect content, f32 lo, f32 hi, bool *open,
                          const civue_menu_item *items, u32 count, const civue_menu_style *style, u32 *path,
                          u32 *depth) {
  *depth = 0;
  civue_id sid = civue_ids(id, "scroll");
  if (!*open) {
    civue_forget(ui, sid);
    return false;
  }
  f32 x = content.x0, w = rect_w(content), top = content.y0, h = rect_h(content);
  civue_rect vis = rect(x, fmax(top, lo), x + w, fmin(top + h, hi));
  f32 *off = civue_memory(ui, sid, sizeof(f32), nullptr);
  if (ui->input.has_mouse && rect_contains(vis, ui->input.mouse)) *off -= ui->input.scroll.y;
  *off = clamp(*off, top - vis.y0, top + h - vis.y1);
  content = rect_xywh(x, top - *off, w, h);
  bool cut_top = content.y0<vis.y0 - .5, cut_bottom = content.y1> vis.y1 + .5;
  civue_rect inner = rect(vis.x0, vis.y0 + (cut_top ? 16 : 0), vis.x1, vis.y1 - (cut_bottom ? 16 : 0));
  bool was = civue_begin_overlay(ui);
  civue_id card = civue_ids(id, "menu-card");
  bool shown = civue_drawn_last_frame(ui, card);
  civue_interact(ui, card, vis, CIVUE_SENSE_HOVER);
  menu_surface(ui, vis);
  civue_push_clip(ui, inner);
  bool chosen = menu_at(ui, id, content, items, count, style, path, 0, depth);
  civue_pop_clip(ui);
  if (cut_top)
    civue_icon_centered(ui, CIVUE_ICON_CARET_UP, rect(vis.x0, vis.y0, vis.x1, vis.y0 + 16), 16, CIVUE_MENU_TEXT);
  if (cut_bottom)
    civue_icon_centered(ui, CIVUE_ICON_CARET_DOWN, rect(vis.x0, vis.y1 - 16, vis.x1, vis.y1), 16, CIVUE_MENU_TEXT);
  bool outside = shown && ui->input.pressed[0] && !(ui->input.has_mouse && rect_contains(vis, ui->input.mouse));
  if (chosen || civue_key_pressed(ui, CIVUE_KEY_ESCAPE) || outside) *open = false;
  civue_end_overlay(ui, was);
  if (!*open) civue_forget(ui, sid);
  return chosen;
}

bool civue_menu_styled(civue_ui *ui, civue_id id, civue_rect anchor, bool *open, const civue_menu_item *items,
                       u32 count, const civue_menu_style *style, u32 *path, u32 *depth) {
  *depth = 0;
  if (!*open) {
    civue_forget(ui, civue_ids(id, "open"));
    return false;
  }
  civue_vec2 size = menu_size_min(ui, items, count, style, style->has_min_width ? style->min_width : 194);
  f32 w = size.x, h = size.y, x = anchor.x0, y = anchor.y1 + style->gap;
  if (x + w > ui->size.x - 8) x = fmax(anchor.x1 - w, 8);
  if (h > ui->size.y - 16)
    return scrolled_menu(ui, id, rect_xywh(x, 8, w, h), 8, ui->size.y - 8, open, items, count, style, path, depth);
  f32 floor = style->has_floor ? style->floor : ui->size.y - 8;
  if (style->above || y + h > floor)
    y = style->at_pointer && !style->above ? fmaxf(floor - h, 8) : fmax(anchor.y0 - style->gap - h, 8);
  civue_rect r = rect_xywh(x, y, w, h);
  bool was = civue_begin_overlay(ui);
  civue_id card = civue_ids(id, "menu-card");
  bool shown = civue_drawn_last_frame(ui, card);
  civue_interact(ui, card, r, CIVUE_SENSE_HOVER);
  bool chosen = menu_at(ui, id, r, items, count, style, path, 0, depth);
  bool inside = ui->input.has_mouse && (rect_contains(r, ui->input.mouse) || rect_contains(anchor, ui->input.mouse));
  if (ui->input.has_mouse) {
    vec_each(&ui->regions, reg) if (reg->overlay && rect_contains(reg->rect, ui->input.mouse)) inside = true;
  }
  if (chosen || civue_key_pressed(ui, CIVUE_KEY_ESCAPE) || (shown && ui->input.pressed[0] && !inside)) *open = false;
  civue_end_overlay(ui, was);
  return chosen;
}

void civue_menu_open_row(civue_ui *ui, civue_id id, i32 row) {
  MenuOpen *os = civue_memory(ui, civue_ids(id, "open"), sizeof(MenuOpen), nullptr);
  os->initialized = true;
  os->row = row;
}

bool civue_menu_path(civue_ui *ui, civue_id id, civue_rect anchor, bool *open, const civue_menu_item *items, u32 count,
                     u32 *path, u32 *depth) {
  civue_menu_style style = civue_menu_style_default();
  return civue_menu_styled(ui, id, anchor, open, items, count, &style, path, depth);
}
i32 civue_menu(civue_ui *ui, civue_id id, civue_rect anchor, bool *open, const civue_menu_item *items, u32 count) {
  u32 path[CIVUE_MENU_MAX_DEPTH] = {}, depth;
  return civue_menu_path(ui, id, anchor, open, items, count, path, &depth) ? (i32)path[0] : -1;
}

static i32 select_menu_at(civue_ui *ui, civue_id id, civue_rect field, bool *open, const civue_menu_item *items,
                          u32 count, bool wide) {
  civue_menu_style style = civue_menu_style_default();
  civue_vec2 size = menu_size_min(ui, items, count, &style, wide ? rect_w(field) + 16 : 0);
  u32 checked = 0;
  for (u32 i = 0; i < count; ++i)
    if (menu_regular(&items[i]) && items[i].checked) {
      checked = i;
      break;
    }
  f32 above = 8;
  for (u32 i = 0; i < checked; ++i) above += item_height(&items[i], &style);
  f32 x = wide ? field.x0 - 8 : fmax(field.x1 - size.x, 8);
  style.width = size.x;
  f32 top = field.y0 - above, lo = 8, hi = ui->size.y - 8;
  u32 path[CIVUE_MENU_MAX_DEPTH] = {}, depth;
  bool chosen;
  if (top >= lo && top + size.y <= hi) {
    civue_rect anchor = rect(x, top - style.gap, x + size.x, top - style.gap);
    chosen = civue_menu_styled(ui, id, anchor, open, items, count, &style, path, &depth);
  } else
    chosen = scrolled_menu(ui, id, rect_xywh(x, top, size.x, size.y), lo, hi, open, items, count, &style, path, &depth);
  return chosen ? (i32)path[0] : -1;
}
i32 civue_select_menu(civue_ui *ui, civue_id id, civue_rect field, bool *open, const civue_menu_item *items,
                      u32 count) {
  return select_menu_at(ui, id, field, open, items, count, false);
}
i32 civue_select_menu_wide(civue_ui *ui, civue_id id, civue_rect field, bool *open, const civue_menu_item *items,
                           u32 count) {
  return select_menu_at(ui, id, field, open, items, count, true);
}
i32 civue_select_menu_start(civue_ui *ui, civue_id id, civue_rect field, bool *open, const civue_menu_item *items,
                            u32 count) {
  civue_menu_style style = civue_menu_style_default();
  civue_vec2 size = menu_size_min(ui, items, count, &style, 0);
  field.x1 = field.x0 + size.x;
  return select_menu_at(ui, id, field, open, items, count, false);
}

typedef struct {
  bool active;
  f64 since;
} TooltipState;
static bool tooltip_ready(civue_ui *ui, civue_id id, const civue_response *resp) {
  TooltipState *s = civue_memory(ui, civue_ids(id, "tooltip"), sizeof(TooltipState), nullptr);
  if (!resp->hovered || resp->dragging) {
    s->active = false;
    return false;
  }
  if (!s->active) {
    s->active = true;
    s->since = ui->now;
  }
  f64 shown = ui->now - s->since;
  if (shown < .5) {
    civue_request_repaint_after(ui, .5 - shown);
    return false;
  }
  return true;
}

static void tooltip_placed(civue_ui *ui, civue_id id, const civue_response *resp, const char *text,
                           const char *shortcut, bool above, bool right, const char *badge) {
  if (!tooltip_ready(ui, id, resp)) return;
  const civue_text_block *block = civue_layout(ui, text, CIVUE_BODY, badge ? 0 : 240);
  shortcut = civue_shortcut_label(shortcut);
  const civue_text_block *sc = shortcut ? civue_layout(ui, shortcut, CIVUE_BODY, 0) : nullptr;
  const civue_text_block *label = badge ? civue_layout(ui, badge, CIVUE_BODY, 0) : nullptr;
  f32 bw = label ? ceilf(label->width) + 9 : 0;
  f32 w = ceilf(block->width) + 16 + (sc ? ceilf(sc->width) + 12 : 0) + (label ? 6 + bw : 0);
  f32 h = ceilf(block->height) + 8;
  civue_vec2 center = rect_center(resp->rect);
  bool below = !above && resp->rect.y1 + 8 + h <= ui->size.y - 8;
  f32 x = clamp(center.x - w / 2, 8, fmax(ui->size.x - w - 8, 8));
  f32 y = below ? resp->rect.y1 + 8 : resp->rect.y0 - 8 - h;
  if (right) {
    x = fmin(resp->rect.x1 + 8, ui->size.x - w - 8);
    y = center.y - h / 2;
  }
  civue_rect r = rect_xywh(roundf(x), roundf(y), w, h);
  bool was = civue_begin_overlay(ui);
  civue_floating(ui, r, CIVUE_RADIUS_MEDIUM, CIVUE_TOOLTIP_BG, CIVUE_E300_TOOLTIP);
  if (right)
    civue_icon_at(ui, CIVUE_ICON_TOOLTIP_ARROW_LEFT, rect_xywh(r.x0 - 6, round(center.y - 6), 12, 12),
                  CIVUE_TOOLTIP_BG);
  else
    civue_icon_at(ui, below ? CIVUE_ICON_TOOLTIP_ARROW_UP : CIVUE_ICON_TOOLTIP_ARROW,
                  rect_xywh(round(center.x - 6), below ? r.y0 - 6 : r.y1, 12, 12), CIVUE_TOOLTIP_BG);
  civue_draw_text(ui, block, v2(r.x0 + 8, r.y0 + 4), CIVUE_TOOLTIP_TEXT);
  if (sc) civue_draw_text(ui, sc, v2(r.x1 - 8 - sc->width, r.y0 + 4), CIVUE_TOOLTIP_TEXT_SECONDARY);
  if (label) {
    civue_rect b = rect_xywh(r.x0 + 8 + ceilf(block->width) + 6, r.y0 + 4, bw, 16);
    civue_stroke(ui, rect_inflate(b, -.5, -.5), color_hex(0x444444), 1, 4);
    civue_draw_text(ui, label, v2(b.x0 + 4, r.y0 + 4), CIVUE_TOOLTIP_TEXT);
  }
  civue_end_overlay(ui, was);
}

void civue_tooltip_shortcut(civue_ui *ui, civue_id id, const civue_response *resp, const char *text,
                            const char *shortcut) {
  tooltip_placed(ui, id, resp, text, shortcut, false, false, nullptr);
}
void civue_tooltip_above(civue_ui *ui, civue_id id, const civue_response *resp, const char *text,
                         const char *shortcut) {
  tooltip_placed(ui, id, resp, text, shortcut, true, false, nullptr);
}
void civue_tooltip_right(civue_ui *ui, civue_id id, const civue_response *resp, const char *text) {
  tooltip_placed(ui, id, resp, text, nullptr, false, true, nullptr);
}
void civue_tooltip_badge(civue_ui *ui, civue_id id, const civue_response *resp, const char *text, const char *badge) {
  tooltip_placed(ui, id, resp, text, nullptr, false, false, badge);
}
void civue_tooltip(civue_ui *ui, civue_id id, const civue_response *resp, const char *text) {
  civue_tooltip_shortcut(ui, id, resp, text, nullptr);
}

void civue_spinner(civue_ui *ui, civue_rect r, civue_color color) {
  civue_draw_list *list = civue_list(ui);
  f32 old[4];
  memcpy(old, list->turn, sizeof old);
  f32 angle = fmod(ui->now, 1) * 6.283185307179586;
  civue_dl_set_turn(list, angle, v2_scale(rect_center(r), ui->scale));
  civue_icon_centered(ui, CIVUE_ICON_SPINNER, r, 16, color);
  memcpy(list->turn, old, sizeof old);
  civue_request_repaint_after(ui, .016);
}

static f32 rem_euclid(f32 v, f32 d) {
  f32 r = fmodf(v, d);
  return r < 0 ? r + d : r;
}
void civue_shimmer(civue_ui *ui, civue_rect r, f32 radius, f32 t, f32 opacity) {
  civue_color ink = ui->theme.dark ? CIVUE_WHITE : CIVUE_BLACK;
  civue_fill(ui, r, alpha(ink, .05 * opacity), radius);
  f32 offset = -1000 + 2000 * (rem_euclid(t, 6) / 6);
  for (f64 x = r.x0 + radius; x < r.x1 - radius; x += 2) {
    f32 p = rem_euclid(x - r.x0 - offset, 1000) / 1000;
    f32 extra = p >= .04 && p < .25 ? (p - .04) / .21 : p >= .25 && p < .36 ? 1 - (p - .25) / .11 : 0;
    if (extra > .01) {
      f32 w = fmin(2, r.x1 - radius - x);
      civue_fill(ui, rect(x, r.y0, x + w, r.y1), alpha(ink, .05 * extra * opacity), 0);
    }
  }
  civue_request_repaint_after(ui, .033);
}
void civue_fade_out(civue_ui *ui, civue_rect r, civue_color color) {
  for (f64 y = r.y0; y < r.y1; y += 2) {
    f32 a = clamp((y + 1 - r.y0) / rect_h(r), 0, 1);
    civue_fill(ui, rect(r.x0, y, r.x1, fmin(y + 2, r.y1)), alpha(color, a), 0);
  }
}
static f32 bezier_at(f32 a, f32 b, f32 s) {
  return 3 * a * s * (1 - s) * (1 - s) + 3 * b * s * s * (1 - s) + s * s * s;
}
f32 civue_cubic_bezier(f32 x1, f32 y1, f32 x2, f32 y2, f32 t) {
  t = clamp(t, 0, 1);
  f32 lo = 0, hi = 1;
  for (u32 i = 0; i < 24; ++i) {
    f32 mid = (lo + hi) / 2;
    if (bezier_at(x1, x2, mid) < t)
      lo = mid;
    else
      hi = mid;
  }
  return bezier_at(y1, y2, (lo + hi) / 2);
}
f32 civue_ease(f32 t) { return civue_cubic_bezier(.25, .1, .25, 1, t); }
