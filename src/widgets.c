#include "internal.h"

#include <assert.h>
#include <civue/widgets.h>

static f64 clamp(f64 v, f64 lo, f64 hi) {
  return fmin(fmax(v, lo), hi);
}

// takes the first press of `a` or `b` from the keys, true when there was one.
static bool take_key(civue_ui *ui, u32 a, u32 b) {
  civue_input *in = &ui->input;
  for (u32 i = 0; i < in->keys.len; i++) {
    u32 k = in->keys.v[i].key;
    if (k != a && k != b) {
      continue;
    }
    vec_remove(&in->keys, i);
    return true;
  }
  return false;
}

civue_button_state civue_button(civue_ui *ui, civue_id id, civue_rect r) {
  civue_button_state s = {.response =
                            civue_interact(ui, id, r, CIVUE_SENSE_CLICK)};
  s.held = s.response.dragging && s.response.hovered;
  s.ring = ui->ring == id;
  s.focused = ui->focus == id;
  if (s.focused) {
    civue_claim_focus(ui);
    s.activated = take_key(ui, CIVUE_KEY_ENTER, CIVUE_KEY_SPACE);
  }
  s.activated |= s.response.clicked;
  return s;
}

civue_toggle_state civue_toggle(civue_ui *ui, civue_id id, civue_rect r,
                                bool on) {
  civue_toggle_state s = {.button = civue_button(ui, id, r), .on = on};
  if (s.button.activated) {
    s.on = !on;
    s.changed = true;
  }
  return s;
}

civue_slider_state civue_slider(civue_ui *ui, civue_id id, civue_rect r,
                                civue_rect track, f64 value, f64 min, f64 max,
                                f64 anchor, f64 step) {
  f64 span = fmax(max - min, 1e-9);
  civue_slider_state s = {.value = value};
  s.anchor_x =
    track.x0 + (clamp(anchor, min, max) - min) / span * rect_w(track);
  s.x = track.x0 + (clamp(value, min, max) - min) / span * rect_w(track);
  s.response = civue_interact(ui, id, r, CIVUE_SENSE_DRAG);
  s.focused = ui->focus == id;
  if (s.focused) {
    civue_claim_focus(ui);
    f64 to = value;
    for (u32 i = 0; i < ui->input.keys.len;) {
      civue_key_press k = ui->input.keys.v[i];
      f64 n = k.mods & CIVUE_MOD_SHIFT ? 10 : 1;
      if (k.key == CIVUE_KEY_RIGHT || k.key == CIVUE_KEY_UP) {
        to += step * n;
      }
      else if (k.key == CIVUE_KEY_LEFT || k.key == CIVUE_KEY_DOWN) {
        to -= step * n;
      }
      else if (k.key == CIVUE_KEY_HOME) {
        to = min;
      }
      else if (k.key == CIVUE_KEY_END) {
        to = max;
      }
      else {
        i++;
        continue;
      }
      vec_remove(&ui->input.keys, i);
      s.ended = true;
    }
    to = clamp(to, min, max);
    if (s.ended && fabs(to - value) > 1e-9) {
      s.value = to;
      s.changed = true;
    }
  }
  if (s.response.dragging && ui->input.has_mouse) {
    f64 k =
      clamp((ui->input.mouse.x - track.x0) / fmax(rect_w(track), 1e-6), 0, 1);
    f64 v = min + round(k * span / step) * step;
    if (fabs(v - value) > 1e-9) {
      s.value = v;
      s.changed = true;
    }
  }
  if (s.response.released) {
    s.ended = true;
  }
  return s;
}

civue_group_state civue_radio_group(civue_ui *ui, civue_id id,
                                    const civue_rect *cells,
                                    civue_response *out, u32 count,
                                    i32 selected) {
  civue_group_state s = {.chosen = -1, .current = -1};
  for (u32 i = 0; i < count; ++i) {
    out[i] = civue_interact(ui, civue_idn(id, i), cells[i], CIVUE_SENSE_CLICK);
    if (out[i].clicked) {
      s.chosen = (i32)i;
    }
  }
  s.focused = ui->focus == id;
  if (!s.focused || !count) {
    return s;
  }
  civue_claim_focus(ui);
  i32 at = s.chosen >= 0 ? s.chosen : selected;
  for (u32 i = 0; i < ui->input.keys.len;) {
    u32 k = ui->input.keys.v[i].key;
    if (k == CIVUE_KEY_RIGHT || k == CIVUE_KEY_DOWN) {
      at = at < 0 ? 0 : (at + 1) % (i32)count;
    }
    else if (k == CIVUE_KEY_LEFT || k == CIVUE_KEY_UP) {
      at = at < 0 ? (i32)count - 1 : (at + (i32)count - 1) % (i32)count;
    }
    else if (k == CIVUE_KEY_HOME) {
      at = 0;
    }
    else if (k == CIVUE_KEY_END) {
      at = (i32)count - 1;
    }
    else {
      i++;
      continue;
    }
    vec_remove(&ui->input.keys, i);
    s.chosen = at;
  }
  s.current = at;
  return s;
}

// ---- scrolling ----

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
  return civue_memory(ui, civue_id_of("widgets-scroll-stack"),
                      sizeof(ScrollStack), scroll_stack_free);
}

static Scroll *scroll_state(civue_ui *ui, civue_id id) {
  return civue_memory(ui, id, sizeof(Scroll), nullptr);
}

f32 civue_scroll_x(civue_ui *ui, civue_id id) {
  return scroll_state(ui, id)->x;
}

void civue_set_scroll_y(civue_ui *ui, civue_id id, f32 y) {
  Scroll *s = scroll_state(ui, id);
  s->offset = s->target = fmaxf(y, 0);
}

void civue_set_scroll_x(civue_ui *ui, civue_id id, f32 x) {
  scroll_state(ui, id)->x = fmaxf(x, 0);
}

void civue_scroll_to_center(civue_ui *ui, civue_id id, f32 y0, f32 y1, f32 view,
                            bool always) {
  Scroll *s = scroll_state(ui, id);
  if (always || y0 < s->target || y1 > s->target + view) {
    s->target = fmaxf((y0 + y1) / 2 - view / 2, 0);
  }
}

void civue_scroll_to(civue_ui *ui, civue_id id, f32 y0, f32 y1, f32 view) {
  Scroll *s = scroll_state(ui, id);
  if (y0 < s->target) {
    s->target = y0;
  }
  else if (y1 > s->target + view) {
    s->target = y1 - view;
  }
}

civue_rect civue_scroll_area(civue_ui *ui, civue_id id, civue_rect r,
                             f32 content_height) {
  return civue_scroll_area_xy(ui, id, r, rect_w(r), content_height);
}

static f32 thumb_len(f32 track, f32 view, f32 content) {
  return fminf(fmaxf((track - 4) * view / fmaxf(content, 1), 24), track - 4);
}

civue_rect civue_scroll_area_xy(civue_ui *ui, civue_id id, civue_rect r, f32 cw,
                                f32 ch) {
  f32 max = fmax(ch - rect_h(r), 0), mx = fmax(cw - rect_w(r), 0);
  civue_vec2 m = ui->input.mouse;
  bool pointer = ui->input.has_mouse;
  bool hovered = pointer && rect_contains(r, m) &&
                 !civue_under_overlay(ui, m) &&
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
    if (ui->input.precise_scroll) {
      s->offset = s->target;
    }
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
      }
      else {
        s->target =
          clamp(s->target + (m.y < y ? -rect_h(r) : rect_h(r)), 0, max);
      }
      scrolled = true;
    }
    else if (mx > 0 && rect_contains(ht, m)) {
      f32 x = ht.x0 + 2 + (rect_w(ht) - 4 - hl) * s->x / mx;
      if (m.x >= x && m.x < x + hl) {
        s->dragging = true;
        s->vertical = false;
        s->from = m.x;
        s->start = s->x;
      }
      else {
        s->x = clamp(s->x + (m.x < x ? -rect_w(r) : rect_w(r)), 0, mx);
      }
      scrolled = true;
    }
  }
  if (s->dragging && pointer) {
    if (ui->input.down[0]) {
      if (s->vertical) {
        f32 k = max / fmax(rect_h(vt) - 4 - vl, 1);
        s->offset = s->target = clamp(s->start + (m.y - s->from) * k, 0, max);
      }
      else {
        f32 k = mx / fmax(rect_w(ht) - 4 - hl, 1);
        s->x = clamp(s->start + (m.x - s->from) * k, 0, mx);
      }
      scrolled = true;
    }
    else {
      s->dragging = false;
    }
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
  }
  else {
    s->offset = s->target;
  }
  if (hovered) {
    ui->input.scroll = v2(0, 0);
  }
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
                   .on_bars = pointer && ((max > 0 && rect_contains(vt, m)) ||
                                          (mx > 0 && rect_contains(ht, m)))};
  vec_push(&scroll_stack(ui)->frames, f);
  civue_rect content = rect_xywh(r.x0 - s->x, r.y0 - s->offset,
                                 fmax(cw, rect_w(r)), fmax(ch, rect_h(r)));
  civue_push_clip(ui, r);
  return content;
}

civue_scroll_state civue_scroll_end(civue_ui *ui) {
  ScrollStack *stack = scroll_stack(ui);
  assert(stack->frames.len);
  ScrollFrame f = stack->frames.v[--stack->frames.len];
  civue_pop_clip(ui);
  civue_scroll_state s = {.id = f.id, .r = f.r};
  if (f.on_bars || f.dragging) {
    civue_interact(ui, civue_ids(f.id, "bars-over"), f.max > 0 ? f.vt : f.ht,
                   CIVUE_SENSE_DRAG);
    if (f.max_x > 0) {
      civue_interact(ui, civue_ids(f.id, "bars-over-x"), f.ht,
                     CIVUE_SENSE_DRAG);
    }
  }
  if (f.max <= 0 && f.max_x <= 0) {
    return s;
  }
  s.show = civue_animate(ui, civue_ids(f.id, "bar"),
                         f.hovered || f.recently || f.dragging ? 1 : 0, .2);
  if (f.recently) {
    civue_request_repaint_after(ui, 1.05);
  }
  bool pointer = ui->input.has_mouse;
  civue_vec2 m = ui->input.mouse;
  if (f.max > 0) {
    civue_rect t = f.vt;
    f32 y = t.y0 + 2 + (rect_h(t) - 4 - f.vl) * f.offset / f.max;
    s.vertical = (civue_scrollbar){
      true, t, rect(t.x0 + 2, y, t.x1 - 2, y + f.vl),
      (pointer && rect_contains(t, m)) || (f.dragging && f.vertical)};
  }
  if (f.max_x > 0) {
    civue_rect t = f.ht;
    f32 x = t.x0 + 2 + (rect_w(t) - 4 - f.hl) * f.x / f.max_x;
    s.horizontal = (civue_scrollbar){
      true, t, rect(x, t.y0 + 2, x + f.hl, t.y1 - 2),
      (pointer && rect_contains(t, m)) || (f.dragging && !f.vertical)};
  }
  if (ui->scrollbars) {
    ui->scrollbars(ui, &s);
  }
  return s;
}

f32 civue_vertical_splitter(civue_ui *ui, civue_id id, f32 x, f32 y0, f32 y1) {
  civue_response resp =
    civue_interact(ui, id, rect(x - 3, y0, x + 3, y1), CIVUE_SENSE_DRAG);
  if (resp.hovered || resp.dragging) {
    civue_set_cursor(ui, CIVUE_CURSOR_RESIZE_H);
  }
  return resp.dragging ? resp.drag_delta.x : 0;
}

// ---- menus ----

// the room above and below a menu's rows and beside a row's hit, and the room
// kept to the window's edges.
constexpr f32 MENU_PAD = 8;
// how far a submenu goes over its menu, and how tall the marks of a cut menu
// are.
constexpr f32 MENU_OVERLAP = 4;
constexpr f32 MENU_CUT = 16;

civue_menu_options civue_menu_options_default(void) {
  return (civue_menu_options){.gap = 4, .focus_row = -1};
}

static bool menu_regular(const civue_menu_item *item) {
  return !item->header && !item->separator && !item->custom;
}

static const civue_menu_look *look_of(const civue_menu_options *o) {
  static const civue_menu_look none = {};
  return o->look ? o->look : &none;
}

static void menu_surface(civue_ui *ui, const civue_menu_options *o,
                         civue_rect r) {
  const civue_menu_look *l = look_of(o);
  if (l->surface) {
    l->surface(ui, l->ctx, r);
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

static bool menu_at(civue_ui *ui, civue_id id, civue_rect r,
                    const civue_menu_item *items, u32 count,
                    const civue_menu_options *o, u32 *path, u32 level,
                    u32 *depth) {
  if (level >= CIVUE_MENU_MAX_DEPTH) {
    return false;
  }
  const civue_menu_look *look = look_of(o);
  menu_surface(ui, o, r);
  civue_interact(ui, civue_ids(id, "bg"), r, CIVUE_SENSE_CLICK);
  MenuOpen *os =
    civue_memory(ui, civue_ids(id, "open"), sizeof(MenuOpen), nullptr);
  if (!os->initialized) {
    os->initialized = true;
    os->row = -1;
  }
  i32 open = os->row;
  MenuKeys *ks =
    civue_memory(ui, civue_ids(id, "keys"), sizeof(MenuKeys), nullptr);
  i32 kbd = ks->row;
  civue_u32s rows = {};
  for (u32 i = 0; i < count; ++i) {
    if (menu_regular(&items[i]) && !items[i].disabled) {
      vec_push(&rows, i);
    }
  }
  if (!ks->seen || ks->seen + 1 != ui->frames) {
    kbd = -1;
    for (u32 i = 0; i < rows.len; ++i) {
      if ((i32)rows.v[i] == o->focus_row) {
        kbd = o->focus_row;
      }
    }
    if (kbd < 0 && o->focus_first && rows.len) {
      kbd = rows.v[0];
    }
  }
  bool chosen = false;
  vec_each(&ui->input.keys, key) {
    i32 at = -1;
    for (u32 i = 0; i < rows.len; ++i) {
      if ((i32)rows.v[i] == kbd) {
        at = i;
      }
    }
    if (key->key == CIVUE_KEY_DOWN && rows.len) {
      kbd = rows.v[at < 0 ? 0 : ((u32)at + 1) % rows.len];
    }
    else if (key->key == CIVUE_KEY_UP && rows.len) {
      kbd = rows.v[at < 0 ? rows.len - 1 : ((u32)at + rows.len - 1) % rows.len];
    }
    else if (key->key == CIVUE_KEY_ENTER && kbd >= 0 && (u32)kbd < count &&
             !items[kbd].submenu) {
      path[level] = kbd;
      *depth = level + 1;
      chosen = true;
    }
  }
  vec_free(&rows);
  if (civue_pointer_moved(ui) && ui->input.has_mouse &&
      rect_contains(r, ui->input.mouse)) {
    kbd = -1;
  }
  *ks = (MenuKeys){ui->frames, kbd};
  f32 y = r.y0 + MENU_PAD;
  i32 sub = -1;
  civue_rect subrow = {};
  for (u32 i = 0; i < count; ++i) {
    const civue_menu_item *it = &items[i];
    f32 h = it->height;
    civue_menu_row row = {
      .index = i, .menu = r, .row = rect(r.x0, y, r.x1, y + h)};
    if (menu_regular(it)) {
      row.hit = rect(r.x0 + MENU_PAD, y, r.x1 - MENU_PAD, y + h);
      row.response =
        civue_interact(ui, civue_idn(id, i), row.hit, CIVUE_SENSE_CLICK);
      if (row.response.hovered && !it->disabled) {
        open = it->submenu ? (i32)i : -1;
      }
      row.open = open == (i32)i;
      row.keyboard = kbd == (i32)i;
      row.lit =
        (row.response.hovered || row.open || row.keyboard) && !it->disabled;
    }
    bool took = look->row && look->row(ui, look->ctx, items, count, &row);
    if (it->custom && took) {
      path[level] = i;
      *depth = level + 1;
      chosen = true;
    }
    else if (menu_regular(it)) {
      if (row.open) {
        sub = i;
        subrow = row.hit;
      }
      if (row.response.clicked && !it->disabled && !it->submenu) {
        path[level] = i;
        *depth = level + 1;
        chosen = true;
      }
    }
    y += h;
  }
  os->row = open;
  if (sub >= 0 && items[sub].submenu && level + 1 < CIVUE_MENU_MAX_DEPTH) {
    const civue_menu_item *it = &items[sub];
    civue_vec2 size =
      look->measure ? look->measure(ui, look->ctx, it->submenu, it->nsubmenu)
                    : v2(0, 0);
    f32 x = r.x1 - MENU_OVERLAP;
    if (x + size.x > ui->size.x - MENU_PAD) {
      x = r.x0 - size.x + MENU_OVERLAP;
    }
    f32 sy = fmax(fmin(subrow.y0 - MENU_PAD, ui->size.y - MENU_PAD - size.y),
                  MENU_PAD);
    u32 child_path[CIVUE_MENU_MAX_DEPTH];
    memcpy(child_path, path, level * sizeof(u32));
    child_path[level] = sub;
    u32 child_depth = 0;
    if (menu_at(ui, civue_idn(civue_ids(id, "sub"), sub),
                rect_xywh(x, sy, size.x, size.y), it->submenu, it->nsubmenu, o,
                child_path, level + 1, &child_depth)) {
      memcpy(path, child_path, child_depth * sizeof(u32));
      *depth = child_depth;
      chosen = true;
    }
  }
  return chosen;
}

static bool scrolled_menu(civue_ui *ui, civue_id id, civue_rect content, f32 lo,
                          f32 hi, bool *open, const civue_menu_item *items,
                          u32 count, const civue_menu_options *o, u32 *path,
                          u32 *depth) {
  *depth = 0;
  civue_id sid = civue_ids(id, "scroll");
  if (!*open) {
    civue_forget(ui, sid);
    return false;
  }
  const civue_menu_look *look = look_of(o);
  f32 x = content.x0, w = rect_w(content), top = content.y0,
      h = rect_h(content);
  civue_rect vis = rect(x, fmax(top, lo), x + w, fmin(top + h, hi));
  f32 *off = civue_memory(ui, sid, sizeof(f32), nullptr);
  if (ui->input.has_mouse && rect_contains(vis, ui->input.mouse)) {
    *off -= ui->input.scroll.y;
  }
  *off = clamp(*off, top - vis.y0, top + h - vis.y1);
  content = rect_xywh(x, top - *off, w, h);
  bool cut_top = content.y0<vis.y0 - .5, cut_bottom = content.y1> vis.y1 + .5;
  civue_rect inner = rect(vis.x0, vis.y0 + (cut_top ? MENU_CUT : 0), vis.x1,
                          vis.y1 - (cut_bottom ? MENU_CUT : 0));
  bool was = civue_begin_overlay(ui);
  civue_id card = civue_ids(id, "menu-card");
  bool shown = civue_drawn_last_frame(ui, card);
  civue_interact(ui, card, vis, CIVUE_SENSE_HOVER);
  menu_surface(ui, o, vis);
  civue_push_clip(ui, inner);
  bool chosen = menu_at(ui, id, content, items, count, o, path, 0, depth);
  civue_pop_clip(ui);
  if (cut_top && look->cut) {
    look->cut(ui, look->ctx, rect(vis.x0, vis.y0, vis.x1, vis.y0 + MENU_CUT),
              true);
  }
  if (cut_bottom && look->cut) {
    look->cut(ui, look->ctx, rect(vis.x0, vis.y1 - MENU_CUT, vis.x1, vis.y1),
              false);
  }
  bool outside = shown && ui->input.pressed[0] &&
                 !(ui->input.has_mouse && rect_contains(vis, ui->input.mouse));
  if (chosen || civue_key_pressed(ui, CIVUE_KEY_ESCAPE) || outside) {
    *open = false;
  }
  civue_end_overlay(ui, was);
  if (!*open) {
    civue_forget(ui, sid);
  }
  return chosen;
}

bool civue_menu(civue_ui *ui, civue_id id, civue_rect anchor, civue_vec2 size,
                bool *open, const civue_menu_item *items, u32 count,
                const civue_menu_options *o, u32 *path, u32 *depth) {
  *depth = 0;
  if (!*open) {
    civue_forget(ui, civue_ids(id, "open"));
    return false;
  }
  f32 w = size.x, h = size.y, x = anchor.x0, y = anchor.y1 + o->gap;
  if (x + w > ui->size.x - MENU_PAD) {
    x = fmax(anchor.x1 - w, MENU_PAD);
  }
  if (h > ui->size.y - 2 * MENU_PAD) {
    return scrolled_menu(ui, id, rect_xywh(x, MENU_PAD, w, h), MENU_PAD,
                         ui->size.y - MENU_PAD, open, items, count, o, path,
                         depth);
  }
  f32 floor = o->has_floor ? o->floor : ui->size.y - MENU_PAD;
  if (o->above || y + h > floor) {
    y = o->at_pointer && !o->above ? fmaxf(floor - h, MENU_PAD)
                                   : fmax(anchor.y0 - o->gap - h, MENU_PAD);
  }
  civue_rect r = rect_xywh(x, y, w, h);
  bool was = civue_begin_overlay(ui);
  civue_id card = civue_ids(id, "menu-card");
  bool shown = civue_drawn_last_frame(ui, card);
  civue_interact(ui, card, r, CIVUE_SENSE_HOVER);
  bool chosen = menu_at(ui, id, r, items, count, o, path, 0, depth);
  bool inside = ui->input.has_mouse && (rect_contains(r, ui->input.mouse) ||
                                        rect_contains(anchor, ui->input.mouse));
  if (ui->input.has_mouse) {
    vec_each(&ui->regions, reg) {
      if (reg->overlay && rect_contains(reg->rect, ui->input.mouse)) {
        inside = true;
      }
    }
  }
  if (chosen || civue_key_pressed(ui, CIVUE_KEY_ESCAPE) ||
      (shown && ui->input.pressed[0] && !inside)) {
    *open = false;
  }
  civue_end_overlay(ui, was);
  return chosen;
}

void civue_menu_open_row(civue_ui *ui, civue_id id, i32 row) {
  MenuOpen *os =
    civue_memory(ui, civue_ids(id, "open"), sizeof(MenuOpen), nullptr);
  os->initialized = true;
  os->row = row;
}

i32 civue_select_menu(civue_ui *ui, civue_id id, civue_rect field, f32 x,
                      civue_vec2 size, i32 selected, bool *open,
                      const civue_menu_item *items, u32 count,
                      const civue_menu_options *o) {
  f32 above = MENU_PAD;
  for (i32 i = 0; i < selected && (u32)i < count; ++i) {
    above += items[i].height;
  }
  f32 top = field.y0 - above, lo = MENU_PAD, hi = ui->size.y - MENU_PAD;
  u32 path[CIVUE_MENU_MAX_DEPTH] = {}, depth;
  bool chosen;
  if (top >= lo && top + size.y <= hi) {
    civue_rect anchor = rect(x, top - o->gap, x + size.x, top - o->gap);
    chosen =
      civue_menu(ui, id, anchor, size, open, items, count, o, path, &depth);
  }
  else {
    chosen = scrolled_menu(ui, id, rect_xywh(x, top, size.x, size.y), lo, hi,
                           open, items, count, o, path, &depth);
  }
  return chosen ? (i32)path[0] : -1;
}

// ---- tooltips ----

typedef struct {
  bool active;
  f64 since;
} TooltipState;

bool civue_tooltip(civue_ui *ui, civue_id id, const civue_response *resp) {
  TooltipState *s =
    civue_memory(ui, civue_ids(id, "tooltip"), sizeof(TooltipState), nullptr);
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

civue_tooltip_place civue_tooltip_at(civue_ui *ui, civue_rect anchor,
                                     civue_vec2 size, civue_tooltip_side side,
                                     f32 gap) {
  f32 w = size.x, h = size.y;
  civue_vec2 center = rect_center(anchor);
  bool below =
    side == CIVUE_TOOLTIP_BELOW && anchor.y1 + gap + h <= ui->size.y - gap;
  f32 x = clamp(center.x - w / 2, gap, fmax(ui->size.x - w - gap, gap));
  f32 y = below ? anchor.y1 + gap : anchor.y0 - gap - h;
  if (side == CIVUE_TOOLTIP_RIGHT) {
    x = fmin(anchor.x1 + gap, ui->size.x - w - gap);
    y = center.y - h / 2;
  }
  civue_tooltip_side placed = side == CIVUE_TOOLTIP_RIGHT ? side
                              : below                     ? CIVUE_TOOLTIP_BELOW
                                                          : CIVUE_TOOLTIP_ABOVE;
  return (civue_tooltip_place){rect_xywh(roundf(x), roundf(y), w, h), placed};
}
