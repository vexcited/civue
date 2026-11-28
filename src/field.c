#include "internal.h"

#include <civue/field.h>
#include <civue/system.h>
#include <civue/widgets.h>

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
  // where the caret was drawn last, its line's height.
  civue_rect caret_rect;
  // the caret was moved from outside, the next frame scrolls to it.
  bool follow;
  // the press the pointer's selection grows from.
  civue_text_grip grip;
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
f32 civue_text_caret_x(const civue_text_block *b, u32 byte, u32 *line) {
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

u32 civue_text_hit(const civue_text_block *b, civue_vec2 p) {
  return nearest_on_line(b, line_at(b, (f32)p.y), (f32)p.x);
}

static f32 line_height(const civue_text_block *b) {
  return b->nlines ? b->height / b->nlines : b->height;
}

// lays out a text area's text. a last newline makes no line of its own in a
// layout, while the caret after it goes on one: a space stands for that line.
static const civue_text_block *area_layout(civue_ui *ui, const char *s, usize n,
                                           civue_text_style style, f32 wrap) {
  if (wrap <= 0 || !n || s[n - 1] != '\n') {
    return civue_layout_n(ui, s, n, style, wrap);
  }
  civue_bytes t = {};
  buf_put(&t, s, n);
  buf_put(&t, " ", 1);
  const civue_text_block *b =
    civue_layout_n(ui, (const char *)t.v, t.len, style, wrap);
  vec_free(&t);
  return b;
}

// ---- what is shown ----

// what a secret field shows for each character.
static const char BULLET[] = "\u2022";
constexpr u32 BULLET_LEN = sizeof BULLET - 1;
// what holds the room of an inlay: spaces no line breaks at, as many as it
// is wide.
static const char NBSP[] = "\u00a0";
constexpr u32 NBSP_LEN = sizeof NBSP - 1;
constexpr u32 INLAY_SPACES = 64;

// bytes of the text the field shows as something else: a bullet for a
// character of a secret, the room of an inlay.
typedef struct {
  u32 from, to;
  // where it is in what is shown.
  u32 at, len;
  bool inlay;
  f32 width;
} Swap;

// what the field lays out for its text, the text itself when nothing of it
// is swapped.
typedef struct {
  const char *s;
  u32 n;
  civue_bytes shown;
  Vec(Swap) swaps;
} View;

static void view_free(View *v) {
  vec_free(&v->shown);
  vec_free(&v->swaps);
}

static f32 nbsp_width(civue_ui *ui, civue_text_style style) {
  static const char between[] = "n\u00a0n";
  f32 w = civue_layout_n(ui, between, sizeof between - 1, style, 0)->width -
          civue_layout_n(ui, "nn", 2, style, 0)->width;
  return fmaxf(w, 1);
}

static void view_of(civue_ui *ui, View *v, const u8 *text, u32 n,
                    civue_text_style style, bool secret,
                    const civue_field_inlays *inlays) {
  v->shown.len = 0;
  v->swaps.len = 0;
  v->s = text ? (const char *)text : "", v->n = n;
  if (!secret && !(inlays && inlays->find)) {
    return;
  }
  f32 space = 0;
  for (u32 i = 0; i < n;) {
    u32 next = i + 1;
    while (next < n && (text[next] & 0xc0) == 0x80) {
      next++;
    }
    Swap sw = {.from = i, .to = next, .at = (u32)v->shown.len};
    if (secret) {
      buf_put(&v->shown, BULLET, BULLET_LEN);
    }
    else {
      f32 width = 0;
      u32 taken = inlays->find(inlays->ctx, (const char *)text, n, i, &width);
      if (!taken || taken > n - i) {
        buf_put(&v->shown, text + i, next - i);
        i = next;
        continue;
      }
      if (space <= 0) {
        space = nbsp_width(ui, style);
      }
      u32 count = (u32)fminf(fmaxf(roundf(width / space), 1), INLAY_SPACES);
      for (u32 k = 0; k < count; k++) {
        buf_put(&v->shown, NBSP, NBSP_LEN);
      }
      sw.to = i + taken;
      sw.inlay = true;
      sw.width = (f32)count * space;
    }
    sw.len = (u32)v->shown.len - sw.at;
    vec_push(&v->swaps, sw);
    i = sw.to;
  }
  v->s = v->shown.len ? (const char *)v->shown.v : "", v->n = (u32)v->shown.len;
}

// where a byte of the text is in what the field shows.
static u32 shown_at(const View *v, u32 byte) {
  i32 shift = 0;
  vec_each(&v->swaps, s) {
    if (byte <= s->from) {
      break;
    }
    if (byte < s->to) {
      return s->at;
    }
    shift = (i32)(s->at + s->len) - (i32)s->to;
  }
  return (u32)((i32)byte + shift);
}

// the byte of the text where a byte of what the field shows is, the nearer
// end of what was swapped for one inside it.
static u32 text_at(const View *v, u32 shown) {
  i32 shift = 0;
  vec_each(&v->swaps, s) {
    if (shown <= s->at) {
      break;
    }
    if (shown < s->at + s->len) {
      return (shown - s->at) * 2 < s->len ? s->from : s->to;
    }
    shift = (i32)s->to - (i32)(s->at + s->len);
  }
  return (u32)((i32)shown + shift);
}

// the caret is never inside an inlay: it goes to the end it was moving to.
static u32 snap(const View *v, u32 byte, bool forward) {
  vec_each(&v->swaps, s) {
    if (s->inlay && byte > s->from && byte < s->to) {
      return forward ? s->to : s->from;
    }
  }
  return byte;
}

// puts what an input method composes in what is shown, where the byte
// `caret` of the text is. how many bytes it shows as.
static u32 view_compose(View *v, u32 caret, const civue_bytes *preedit,
                        bool secret) {
  if (!preedit->len) {
    return 0;
  }
  u32 at = shown_at(v, caret);
  civue_bytes out = {};
  buf_put(&out, v->s, at);
  if (secret) {
    for (u32 i = 0; i < preedit->len; i++) {
      if ((preedit->v[i] & 0xc0) != 0x80) {
        buf_put(&out, BULLET, BULLET_LEN);
      }
    }
  }
  else {
    buf_put(&out, preedit->v, preedit->len);
  }
  u32 added = (u32)out.len - at;
  buf_put(&out, v->s + at, v->n - at);
  vec_each(&v->swaps, s) {
    if (s->from >= caret) {
      s->at += added;
    }
  }
  vec_free(&v->shown);
  v->shown = out;
  v->s = (const char *)out.v, v->n = (u32)out.len;
  return added;
}

// draws the inlays over the room what is shown keeps for them.
static void paint_inlays(civue_ui *ui, const View *v, const u8 *text,
                         const civue_text_block *b, civue_vec2 at, f32 lh,
                         const civue_field_inlays *inlays) {
  if (!inlays || !inlays->paint) {
    return;
  }
  vec_each(&v->swaps, s) {
    if (!s->inlay) {
      continue;
    }
    u32 line;
    f32 x = civue_text_caret_x(b, s->at, &line);
    f32 top = b->nlines ? b->lines[line].top : 0;
    inlays->paint(ui, inlays->ctx, (const char *)text + s->from,
                  s->to - s->from,
                  rect(at.x + x, at.y + top, at.x + x + s->width,
                       at.y + top + lh));
  }
}

// ---- keys ----

typedef struct {
  bool changed, submitted, touched;
} Edit;

static void key(Field *f, const View *v, const civue_key_press *k,
                const civue_text_block *b, bool multiline, bool secret,
                Edit *e) {
  bool shift = k->mods & CIVUE_MOD_SHIFT, word = k->mods & CIVUE_MOD_ALT,
       line = k->mods & CIVUE_MOD_CMD, command = k->mods & CIVUE_MOD_CMD;
  const civue_bytes *t = &f->text;
  e->touched = true;
  u32 lo = sel_lo(f), hi = sel_hi(f);
  // a secret has no words to move by, and is never copied out.
  if (secret && (word || line)) {
    switch (k->key) {
      case CIVUE_KEY_LEFT:
        move(f, 0, shift);
        return;
      case CIVUE_KEY_RIGHT:
        move(f, t->len, shift);
        return;
      case CIVUE_KEY_BACKSPACE:
      case CIVUE_KEY_DELETE:
        if (lo == hi) {
          f->anchor = k->key == CIVUE_KEY_BACKSPACE ? 0 : t->len;
        }
        replace(f, "", 0);
        e->changed = true;
        return;
    }
  }
  if (secret && command && (k->key == 'c' || k->key == 'x')) {
    return;
  }
  // what this key changes, apart from what the ones before it did.
  bool changed = e->changed;
  e->changed = false;
  bool vertical = false;
  u32 cl;
  f32 cx = civue_text_caret_x(b, shown_at(v, f->caret), &cl);
  u32 line_start = b->nlines ? text_at(v, b->lines[cl].start) : 0;
  u32 was = f->caret;
  switch (k->key) {
    case CIVUE_KEY_LEFT:
      if (line) {
        move(f, line_start, shift);
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
        u32 end = b->nlines ? b->lines[cl].end : v->n;
        // a wrapped line ends before its trailing space.
        if (cl + 1 < b->nlines && end > b->lines[cl].start &&
            (v->s[end - 1] == ' ' || v->s[end - 1] == '\n')) {
          end--;
        }
        move(f, b->nlines ? text_at(v, end) : t->len, shift);
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
          move(f, text_at(v, nearest_on_line(b, up ? cl - 1 : cl + 1, f->goal)),
               shift);
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
          f->anchor = line_start;
        }
        else if (word) {
          f->anchor = word_left(t, f->caret);
        }
        else {
          f->anchor = prev_char(t, f->caret);
        }
        f->anchor = snap(v, f->anchor, false);
      }
      replace(f, "", 0);
      e->changed = true;
      break;
    case CIVUE_KEY_DELETE:
      if (lo == hi) {
        f->anchor = word ? word_right(t, f->caret) : next_char(t, f->caret);
        f->anchor = snap(v, f->anchor, true);
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
  // the line a last newline makes reaches past the text.
  f->caret = MIN(f->caret, t->len);
  f->anchor = MIN(f->anchor, t->len);
  // an edit leaves a text the view was not made of.
  if (!e->changed) {
    f->caret = snap(v, f->caret, f->caret > was);
    f->anchor = snap(v, f->anchor, f->anchor > f->caret);
  }
  e->changed |= changed;
}

// ---- the field ----

static civue_field_response edit_field(civue_ui *ui, civue_id id, civue_rect r,
                                       char **value, const char *placeholder,
                                       civue_text_style style,
                                       civue_field_look look, bool multiline,
                                       bool secret,
                                       const civue_field_inlays *inlays) {
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
  civue_id sid = civue_ids(id, "scroll");
  View view = {};

  // the editor only exists while the field is focused.
  if (!focused) {
    civue_forget(ui, id);
    civue_field_box box = resp.hovered ? look.hovered : look.idle;
    civue_fill_stroke(ui, r, box.bg, box.border, box.border_width, box.radius);
    bool empty = !*v;
    if (empty) {
      view.s = placeholder ? placeholder : "";
      view.n = (u32)strlen(view.s);
    }
    else {
      view_of(ui, &view, (const u8 *)v, (u32)strlen(v), style, secret, inlays);
    }
    const civue_text_block *b = area_layout(ui, view.s, view.n, style, wrap);
    if (multiline) {
      origin.y = civue_scroll_area(ui, sid, r, b->height + 8).y0 + 4;
    }
    civue_push_clip(ui, area);
    civue_draw_text(ui, b, origin, empty ? look.placeholder : box.text);
    paint_inlays(ui, &view, (const u8 *)v, b, origin, line_height(b), inlays);
    civue_pop_clip(ui);
    if (multiline) {
      civue_scroll_end(ui);
    }
    view_free(&view);
    return out;
  }
  civue_claim_focus(ui);
  if (secret) {
    civue_claim_secure_input(ui);
  }
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
    f->follow = true;
    f->grip = (civue_text_grip){};
  }
  Edit e = {};
  const civue_text_block *b = nullptr;
#define LAYOUT_TEXT()                                                        \
  do {                                                                       \
    view_of(ui, &view, f->text.v, (u32)f->text.len, style, secret, inlays);  \
    b = area_layout(ui, view.s, view.n, style, wrap);                        \
  } while (0)
  LAYOUT_TEXT();
  civue_input *in = &ui->input;
  civue_field_box box = look.editing;
  civue_fill_stroke(ui, r, box.bg, box.border, box.border_width, box.radius);
  // where the pointer is in the text goes by the scroll the last frame showed.
  if (multiline) {
    origin.y -= civue_scroll_y(ui, sid);
  }
  // a press on the scroll bar leaves the caret where it is.
  if (multiline && in->pressed[0] && !resp.pressed &&
      rect_contains(r, in->mouse)) {
    civue_keep_focus(ui);
  }
  if (in->has_mouse && focused_before) {
    civue_vec2 p =
      v2(in->mouse.x - origin.x + f->scroll, in->mouse.y - origin.y);
    const char *text = (const char *)f->text.v;
    u32 shown = civue_text_hit(b, p), past = shown;
    u32 at = MIN(text_at(&view, shown), f->text.len);
    // past the last word of a wrapped line: only the spaces that hang at its
    // end are left of it.
    while (past < view.n && view.s[past] == ' ') {
      past++;
    }
    u32 line = line_at(b, (f32)p.y);
    bool line_end = line + 1 < b->nlines && past >= b->lines[line].end &&
                    past < view.n && view.s[past] != '\n';
    if (resp.pressed) {
      // a secret has no words to pick.
      civue_text_press(&f->grip, text, f->text.len, at, line_end,
                       secret ? 1 : in->clicks, in->mods & CIVUE_MOD_SHIFT,
                       &f->anchor, &f->caret);
      e.touched = true;
    }
    else if (resp.dragging) {
      civue_text_drag(&f->grip, text, f->text.len, at, line_end, &f->anchor,
                      &f->caret);
      // past the edges it scrolls along.
      if (multiline && !rect_contains(r, in->mouse)) {
        civue_request_repaint(ui);
      }
      e.touched = true;
    }
    if (e.touched && f->grip.unit != CIVUE_UNIT_CHAR) {
      bool back = f->caret < f->anchor;
      f->anchor = snap(&view, f->anchor, back);
      f->caret = snap(&view, f->caret, !back);
    }
  }
  vec_each(&in->keys, k) {
    key(f, &view, k, b, multiline, secret, &e);
    LAYOUT_TEXT();
  }
  if (in->text.len) {
    replace(f, (const char *)in->text.v, in->text.len);
    e.changed = e.touched = true;
    LAYOUT_TEXT();
  }
#undef LAYOUT_TEXT
  // what was typed may have closed an inlay around the caret.
  f->caret = snap(&view, f->caret, true);
  f->anchor = snap(&view, f->anchor, true);
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

  // what an input method composes shows at the caret, underlined.
  u32 preedit = view_compose(&view, f->caret, &in->preedit, secret);
  b = area_layout(ui, view.s, view.n, style, wrap);
  u32 caret = shown_at(&view, f->caret) + preedit, cl;
  f32 cx = civue_text_caret_x(b, caret, &cl);
  // keeps the caret in view as it moves past the edges, and all of the
  // text once it fits.
  f32 w = (f32)rect_w(area);
  // the scroll comes after the keys, so that what they did shows in view in
  // the frame they were pressed, a drag past an edge easing along.
  if (multiline) {
    f32 top = b->nlines ? b->lines[cl].top : 0, lh = line_height(b),
        view_h = (f32)rect_h(r);
    f32 y = civue_scroll_y(ui, sid);
    // the wheel alone leaves the caret out of view, but not what is being
    // composed.
    bool moved = e.touched || f->follow || in->preedit.len;
    if (e.touched && resp.dragging) {
      civue_scroll_to(ui, sid, top, top + lh + 8, view_h);
      civue_request_repaint(ui);
    }
    else if (moved && top < y) {
      civue_set_scroll_y(ui, sid, top);
    }
    else if (moved && top + lh + 8 > y + view_h) {
      civue_set_scroll_y(ui, sid, top + lh + 8 - view_h);
    }
    origin.y = civue_scroll_area(ui, sid, r, b->height + 8).y0 + 4;
  }
  f->follow = false;
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
  u32 lo = shown_at(&view, sel_lo(f)), hi = shown_at(&view, sel_hi(f));
  if (lo != hi && !in->preedit.len) {
    for (u32 l = 0; l < b->nlines; l++) {
      u32 s = MAX(lo, b->lines[l].start), t = MIN(hi, b->lines[l].end);
      // an empty line inside the selection is lit, not the line before the
      // one it starts on.
      bool empty =
        s == t && lo <= b->lines[l].start && hi > b->lines[l].end;
      if (s >= t && !empty) {
        continue;
      }
      u32 sl, tl;
      f32 x0 = civue_text_caret_x(b, s, &sl);
      f32 x1 = civue_text_caret_x(b, t, &tl);
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
  if (!view.n && placeholder && *placeholder) {
    civue_text(ui, placeholder, style, origin, look.placeholder);
  }
  civue_draw_text(ui, b, at, box.text);
  paint_inlays(ui, &view, f->text.v, b, at, lh, inlays);
  if (in->preedit.len) {
    u32 pl;
    f32 p0 = civue_text_caret_x(b, caret - preedit, &pl);
    f32 y = at.y + (b->nlines ? b->lines[cl].baseline : b->baseline) + 2;
    civue_hline(ui, at.x + p0, at.x + cx, y, box.text);
  }
  {
    f64 x = at.x + cx;
    f64 y0 =
      multiline ? at.y + (b->nlines ? b->lines[cl].top : 0) : origin.y + 1;
    f->caret_rect =
      rect(x, y0, x + 1, y0 + (multiline ? lh : style.line_height - 2));
    ui->caret = f->caret_rect;
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
  view_free(&view);
  if (multiline) {
    civue_scroll_end(ui);
  }
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
  return edit_field(ui, id, r, value, placeholder, style, look, false, false,
                    nullptr);
}

civue_field_response civue_secret_field(civue_ui *ui, civue_id id,
                                        civue_rect r, char **value,
                                        const char *placeholder,
                                        civue_text_style style,
                                        civue_field_look look) {
  return edit_field(ui, id, r, value, placeholder, style, look, false, true,
                    nullptr);
}

civue_field_response civue_text_area(civue_ui *ui, civue_id id, civue_rect r,
                                     char **value, const char *placeholder,
                                     civue_text_style style,
                                     civue_field_look look) {
  return edit_field(ui, id, r, value, placeholder, style, look, true, false,
                    nullptr);
}

civue_field_response civue_text_area_inlaid(civue_ui *ui, civue_id id,
                                            civue_rect r, char **value,
                                            const char *placeholder,
                                            civue_text_style style,
                                            civue_field_look look,
                                            const civue_field_inlays *inlays) {
  return edit_field(ui, id, r, value, placeholder, style, look, true, false,
                    inlays);
}

float civue_text_area_height(civue_ui *ui, const char *value,
                             civue_text_style style, float width,
                             const civue_field_inlays *inlays) {
  View view = {};
  const char *v = value ? value : "";
  view_of(ui, &view, (const u8 *)v, (u32)strlen(v), style, false, inlays);
  f32 height = area_layout(ui, view.s, view.n, style, width)->height;
  view_free(&view);
  return height;
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
  f->follow = true;
  return xstrndup((const char *)f->text.v, f->text.len);
}

bool civue_field_caret(civue_ui *ui, civue_id id, uint32_t *at,
                       civue_rect *where) {
  if (ui->focus != id) {
    return false;
  }
  Field *f = field_of(ui, id);
  if (!f->live) {
    return false;
  }
  if (at) {
    *at = f->caret;
  }
  if (where) {
    *where = f->caret_rect;
  }
  return true;
}

char *civue_field_replace(civue_ui *ui, civue_id id, uint32_t from, uint32_t to,
                          const char *text) {
  if (ui->focus != id) {
    return nullptr;
  }
  Field *f = field_of(ui, id);
  if (!f->live || from > to || to > f->text.len) {
    return nullptr;
  }
  f->anchor = from;
  f->caret = to;
  replace(f, text, strlen(text));
  f->blink = ui->now;
  f->follow = true;
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
  f->follow = true;
  return true;
}
