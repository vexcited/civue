#include "internal.h"

#include <civue/field.h>
#include <civue/system.h>

typedef struct {
  bool live;
  civue_bytes text;
  // byte offsets, the selection runs between them.
  u32 caret, anchor;
  f32 scroll;
  // when the caret last moved, it stays lit while typing.
  f64 blink;
  // the x arrows up and down keep to, negative for none.
  f32 goal;
} Field;

static void drop_field(void *p) {
  Field *f = p;
  vec_free(&f->text);
}

static Field *field_of(civue_ui *ui, civue_id id) {
  return civue_memory(ui, id, sizeof(Field), drop_field);
}

// ---- moving through text ----

static u32 prev_char(const civue_bytes *t, u32 i) {
  if (!i) {
    return 0;
  }
  i--;
  while (i > 0 && (t->v[i] & 0xc0) == 0x80) {
    i--;
  }
  return i;
}

static u32 next_char(const civue_bytes *t, u32 i) {
  if (i >= t->len) {
    return t->len;
  }
  i++;
  while (i < t->len && (t->v[i] & 0xc0) == 0x80) {
    i++;
  }
  return i;
}

static bool word_byte(u8 c) {
  return c >= 0x80 || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || c == '_';
}

static u32 word_left(const civue_bytes *t, u32 i) {
  while (i > 0 && !word_byte(t->v[prev_char(t, i)])) {
    i = prev_char(t, i);
  }
  while (i > 0 && word_byte(t->v[prev_char(t, i)])) {
    i = prev_char(t, i);
  }
  return i;
}

static u32 word_right(const civue_bytes *t, u32 i) {
  while (i < t->len && !word_byte(t->v[i])) {
    i = next_char(t, i);
  }
  while (i < t->len && word_byte(t->v[i])) {
    i = next_char(t, i);
  }
  return i;
}

static u32 sel_lo(const Field *f) {
  return MIN(f->caret, f->anchor);
}

static u32 sel_hi(const Field *f) {
  return MAX(f->caret, f->anchor);
}

// replaces the selection with `s`, the caret after it.
static void replace(Field *f, const char *s, usize n) {
  u32 lo = sel_lo(f), hi = sel_hi(f);
  civue_bytes out = {};
  buf_put(&out, f->text.v, lo);
  buf_put(&out, s, n);
  buf_put(&out, f->text.v + hi, f->text.len - hi);
  vec_free(&f->text);
  f->text = out;
  f->caret = f->anchor = lo + (u32)n;
}

static void move(Field *f, u32 to, bool select) {
  f->caret = to;
  if (!select) {
    f->anchor = to;
  }
}

// ---- layout queries ----

// where a byte's caret is, its line returned in `line`. at a wrap the caret
// goes to the start of the next line.
static f32 caret_x(const civue_text_block *b, u32 byte, u32 *line) {
  f32 x = 0;
  *line = 0;
  for (u32 i = 0; i < b->ncarets; i++) {
    if (b->carets[i].byte > byte) {
      break;
    }
    x = b->carets[i].x;
    *line = b->carets[i].line;
  }
  return x;
}

static u32 nearest_on_line(const civue_text_block *b, u32 line, f32 x) {
  u32 best = 0;
  f32 dist = INFINITY;
  for (u32 i = 0; i < b->ncarets; i++) {
    if (b->carets[i].line != line) {
      continue;
    }
    f32 d = fabsf(b->carets[i].x - x);
    if (d < dist) {
      dist = d, best = b->carets[i].byte;
    }
  }
  return best;
}

static u32 line_at(const civue_text_block *b, f32 y) {
  u32 line = 0;
  for (u32 i = 0; i < b->nlines; i++) {
    if (y >= b->lines[i].top) {
      line = i;
    }
  }
  return line;
}

static u32 hit(const civue_text_block *b, civue_vec2 p) {
  return nearest_on_line(b, line_at(b, (f32)p.y), (f32)p.x);
}

static f32 line_height(const civue_text_block *b) {
  return b->nlines ? b->height / b->nlines : b->height;
}

// ---- keys ----

typedef struct {
  bool changed, submitted, touched;
} Edit;

static void key(Field *f, const civue_key_press *k, const civue_text_block *b,
                bool multiline, Edit *e) {
  bool shift = k->mods & CIVUE_MOD_SHIFT, word = k->mods & CIVUE_MOD_ALT,
       line = k->mods & CIVUE_MOD_CMD, command = k->mods & CIVUE_MOD_CMD;
  const civue_bytes *t = &f->text;
  e->touched = true;
  u32 lo = sel_lo(f), hi = sel_hi(f);
  bool vertical = false;
  u32 cl;
  f32 cx = caret_x(b, f->caret, &cl);
  switch (k->key) {
    case CIVUE_KEY_LEFT:
      if (line) {
        move(f, b->nlines ? b->lines[cl].start : 0, shift);
      }
      else if (word) {
        move(f, word_left(t, f->caret), shift);
      }
      else if (!shift && lo != hi) {
        move(f, lo, false);
      }
      else {
        move(f, prev_char(t, f->caret), shift);
      }
      break;
    case CIVUE_KEY_RIGHT:
      if (line) {
        u32 end = b->nlines ? b->lines[cl].end : t->len;
        // a wrapped line ends before its trailing space.
        if (cl + 1 < b->nlines && end > b->lines[cl].start &&
            t->v[end - 1] == ' ') {
          end--;
        }
        move(f, end, shift);
      }
      else if (word) {
        move(f, word_right(t, f->caret), shift);
      }
      else if (!shift && lo != hi) {
        move(f, hi, false);
      }
      else {
        move(f, next_char(t, f->caret), shift);
      }
      break;
    case CIVUE_KEY_UP:
    case CIVUE_KEY_DOWN:
      if (multiline) {
        vertical = true;
        if (f->goal < 0) {
          f->goal = cx;
        }
        bool up = k->key == CIVUE_KEY_UP;
        if (up && cl == 0) {
          move(f, 0, shift);
        }
        else if (!up && cl + 1 >= b->nlines) {
          move(f, t->len, shift);
        }
        else {
          move(f, nearest_on_line(b, up ? cl - 1 : cl + 1, f->goal), shift);
        }
      }
      else {
        move(f, k->key == CIVUE_KEY_UP ? 0 : t->len, shift);
      }
      break;
    case CIVUE_KEY_HOME:
      move(f, 0, shift);
      break;
    case CIVUE_KEY_END:
      move(f, t->len, shift);
      break;
    case CIVUE_KEY_BACKSPACE:
      if (lo == hi) {
        if (line) {
          f->anchor = b->nlines ? b->lines[cl].start : 0;
        }
        else if (word) {
          f->anchor = word_left(t, f->caret);
        }
        else {
          f->anchor = prev_char(t, f->caret);
        }
      }
      replace(f, "", 0);
      e->changed = true;
      break;
    case CIVUE_KEY_DELETE:
      if (lo == hi) {
        f->anchor = word ? word_right(t, f->caret) : next_char(t, f->caret);
      }
      replace(f, "", 0);
      e->changed = true;
      break;
    case CIVUE_KEY_ENTER:
      if (multiline) {
        replace(f, "\n", 1);
        e->changed = true;
      }
      else {
        e->submitted = true;
      }
      break;
    case 'a':
      if (command) {
        f->anchor = 0, f->caret = t->len;
      }
      break;
    case 'c':
    case 'x':
      if (command && lo != hi) {
        civue_clipboard_set_text((const char *)t->v + lo, hi - lo);
        if (k->key == 'x') {
          replace(f, "", 0);
          e->changed = true;
        }
      }
      break;
    case 'v':
      if (command) {
        char *s = civue_clipboard_text();
        if (s) {
          // control characters are dropped, newlines kept by text areas.
          civue_bytes clean = {};
          for (char *c = s; *c; c++) {
            if ((u8)*c >= 0x20 || (multiline && *c == '\n')) {
              vec_push(&clean, (u8)*c);
            }
          }
          replace(f, (const char *)clean.v, clean.len);
          vec_free(&clean);
          free(s);
          e->changed = true;
        }
      }
      break;
  }
  if (!vertical) {
    f->goal = -1;
  }
}

// ---- the field ----

static civue_field_response edit_field(civue_ui *ui, civue_id id, civue_rect r,
                                       char **value, const char *placeholder,
                                       civue_text_style style,
                                       civue_field_look look, bool multiline) {
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_DRAG);
  if (resp.hovered) {
    civue_set_cursor(ui, CIVUE_CURSOR_TEXT);
  }
  bool focused_before = ui->focus == id;
  if (resp.pressed) {
    ui->focus = id;
  }
  bool focused = ui->focus == id;
  civue_field_response out = {.response = resp, .focused = focused};
  civue_rect area = rect(r.x0 + look.pad, r.y0, r.x1 - look.pad, r.y1);
  civue_vec2 origin =
    multiline ? v2(area.x0, r.y0 + 4)
              : v2(area.x0, r.y0 + (rect_h(r) - style.line_height) / 2);
  f32 wrap = multiline ? (f32)rect_w(area) : 0;
  const char *v = *value ? *value : "";

  // the editor only exists while the field is focused.
  if (!focused) {
    civue_forget(ui, id);
    civue_field_box box = resp.hovered ? look.hovered : look.idle;
    civue_fill_stroke(ui, r, box.bg, box.border, box.border_width, box.radius);
    civue_push_clip(ui, area);
    bool empty = !*v;
    const civue_text_block *b =
      civue_layout(ui, empty ? placeholder : v, style, wrap);
    civue_draw_text(ui, b, origin, empty ? look.placeholder : box.text);
    civue_pop_clip(ui);
    return out;
  }
  civue_claim_focus(ui);
  Field *f = field_of(ui, id);
  // the editor is made when focus starts, all of its text selected.
  bool fresh = !f->live;
  if (fresh) {
    f->live = true;
    f->text.len = 0;
    buf_put(&f->text, v, strlen(v));
    f->blink = ui->now;
    f->goal = -1;
  }
  if (fresh || !focused_before) {
    f->anchor = 0;
    f->caret = f->text.len;
  }
  Edit e = {};
  const civue_text_block *b =
    civue_layout_n(ui, (const char *)f->text.v, f->text.len, style, wrap);
  civue_input *in = &ui->input;
  if (in->has_mouse && focused_before) {
    civue_vec2 p =
      v2(in->mouse.x - origin.x + f->scroll, in->mouse.y - origin.y);
    if (resp.pressed) {
      u32 at = hit(b, p);
      if (in->clicks >= 2) {
        f->anchor =
          word_left(&f->text, at < f->text.len && word_byte(f->text.v[at])
                                ? next_char(&f->text, at)
                                : at);
        f->caret = word_right(&f->text, f->anchor);
      }
      else {
        move(f, at, in->mods & CIVUE_MOD_SHIFT);
      }
      e.touched = true;
    }
    else if (resp.dragging) {
      f->caret = hit(b, p);
      e.touched = true;
    }
  }
  vec_each(&in->keys, k) {
    key(f, k, b, multiline, &e);
    b = civue_layout_n(ui, (const char *)f->text.v, f->text.len, style, wrap);
  }
  if (in->text.len) {
    replace(f, (const char *)in->text.v, in->text.len);
    e.changed = e.touched = true;
  }
  if (e.touched) {
    f->blink = ui->now;
  }
  out.submitted = e.submitted;
  if (civue_key_pressed(ui, CIVUE_KEY_ESCAPE) || e.submitted) {
    ui->focus = 0;
  }
  if (e.changed &&
      (f->text.len != strlen(v) || memcmp(f->text.v, v, f->text.len))) {
    free(*value);
    *value = xstrndup((const char *)f->text.v, f->text.len);
    out.changed = true;
  }

  civue_field_box box = look.editing;
  civue_fill_stroke(ui, r, box.bg, box.border, box.border_width, box.radius);
  // what an input method composes shows at the caret, underlined.
  civue_bytes shown = {};
  buf_put(&shown, f->text.v, f->caret);
  buf_put(&shown, in->preedit.v, in->preedit.len);
  buf_put(&shown, f->text.v + f->caret, f->text.len - f->caret);
  b = civue_layout_n(ui, (const char *)shown.v, shown.len, style, wrap);
  u32 caret = f->caret + (u32)in->preedit.len, cl;
  f32 cx = caret_x(b, caret, &cl);
  // keeps the caret in view as it moves past the edges, and all of the
  // text once it fits.
  f32 w = (f32)rect_w(area);
  if (multiline || b->width <= w - 2) {
    f->scroll = 0;
  }
  else if (cx - f->scroll > w - 2) {
    f->scroll = cx - w + 2;
  }
  else if (cx - f->scroll < 0) {
    f->scroll = fmaxf(cx, 0);
  }
  civue_vec2 at = v2(origin.x - f->scroll, origin.y);
  f32 lh = line_height(b);
  civue_push_clip(ui, area);
  u32 lo = sel_lo(f), hi = sel_hi(f);
  if (lo != hi && !in->preedit.len) {
    for (u32 l = 0; l < b->nlines; l++) {
      u32 s = MAX(lo, b->lines[l].start), t = MIN(hi, b->lines[l].end);
      if (s >= t && !(s == t && hi > b->lines[l].end)) {
        continue;
      }
      u32 sl, tl;
      f32 x0 = caret_x(b, s, &sl), x1 = caret_x(b, t, &tl);
      if (tl != l) {
        x1 = b->width;
      }
      if (sl != l) {
        x0 = 0;
      }
      civue_rect sel = rect(at.x + x0, at.y + b->lines[l].top, at.x + x1,
                            at.y + b->lines[l].top + lh);
      civue_fill(ui, sel, look.selection, 0);
    }
  }
  // an empty field keeps its placeholder while focused, a browser's.
  if (!shown.len && placeholder && *placeholder) {
    civue_text(ui, placeholder, style, origin, look.placeholder);
  }
  civue_draw_text(ui, b, at, box.text);
  if (in->preedit.len) {
    u32 pl;
    f32 p0 = caret_x(b, f->caret, &pl);
    f32 y = at.y + (b->nlines ? b->lines[cl].baseline : b->baseline) + 2;
    civue_hline(ui, at.x + p0, at.x + cx, y, box.text);
  }
  f64 since = ui->now - f->blink;
  bool lit = (u64)(since / 0.53) % 2 == 0;
  if (lo == hi && lit) {
    f64 x = at.x + cx;
    f64 y0 =
      multiline ? at.y + (b->nlines ? b->lines[cl].top : 0) : origin.y + 1;
    f64 y1 = multiline ? y0 + lh : origin.y + style.line_height - 1;
    civue_fill(ui, rect(x, y0, x + 1, y1), box.text, 0);
  }
  civue_pop_clip(ui);
  vec_free(&shown);
  if (ui->focus == id) {
    civue_request_repaint_after(ui, 0.53 - fmod(since, 0.53));
  }
  else {
    civue_forget(ui, id);
  }
  return out;
}

civue_field_response civue_text_field(civue_ui *ui, civue_id id, civue_rect r,
                                      char **value, const char *placeholder,
                                      civue_text_style style,
                                      civue_field_look look) {
  return edit_field(ui, id, r, value, placeholder, style, look, false);
}

civue_field_response civue_text_area(civue_ui *ui, civue_id id, civue_rect r,
                                     char **value, const char *placeholder,
                                     civue_text_style style,
                                     civue_field_look look) {
  return edit_field(ui, id, r, value, placeholder, style, look, true);
}

char *civue_field_insert(civue_ui *ui, civue_id id, const char *text) {
  if (ui->focus != id) {
    return nullptr;
  }
  Field *f = field_of(ui, id);
  if (!f->live) {
    return nullptr;
  }
  replace(f, text, strlen(text));
  return xstrndup((const char *)f->text.v, f->text.len);
}

bool civue_field_caret_to_end(civue_ui *ui, civue_id id) {
  if (ui->focus != id) {
    return false;
  }
  Field *f = field_of(ui, id);
  if (!f->live) {
    return false;
  }
  f->caret = f->anchor = f->text.len;
  return true;
}
