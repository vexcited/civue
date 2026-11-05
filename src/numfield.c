#include "internal.h"

#include <civue/numfield.h>

// what a number field keeps between frames.
typedef struct {
  // the text being typed, while focused.
  char *text;
  // the value when the label drag started, and how far it went.
  bool scrubbing;
  f64 scrub_from, scrub_dx;
} NumState;

static void num_state_drop(void *p) { free(((NumState *)p)->text); }

static NumState *num_state(civue_ui *ui, civue_id id) {
  return civue_memory(ui, civue_ids(id, "num"), sizeof(NumState), num_state_drop);
}

static bool *flag(civue_ui *ui, const char *name) { return civue_memory(ui, civue_id_of(name), sizeof(bool), nullptr); }

void civue_num_field_variable(civue_ui *ui) { *flag(ui, "num-field-variable") = true; }

void civue_num_show(f64 v, char *out, usize n) {
  f64 r = round(v * 100) / 100;
  if (r == 0) r = 0; // no negative zero.
  if (r == trunc(r))
    snprintf(out, n, "%.0f", r);
  else if (r * 10 == trunc(r * 10))
    snprintf(out, n, "%.1f", r);
  else
    snprintf(out, n, "%.2f", r);
}

// the typed text a field was left with, kept for num_field_typed.
static void drop_typed(void *p) { free(*(char **)p); }

static char **typed_slot(civue_ui *ui, civue_id id) {
  return civue_memory(ui, civue_ids(id, "typed"), sizeof(char *), drop_typed);
}

civue_num_change civue_num_field(civue_ui *ui, civue_id id, civue_rect r, f32 label_w, f64 value, bool mixed,
                                 const char *shown, civue_num_format show, const civue_num_look *look) {
  bool *variable_flag = flag(ui, "num-field-variable");
  bool variable = *variable_flag;
  *variable_flag = false;
  civue_num_change out = {};
  civue_rect lr = rect(r.x0, r.y0, r.x0 + label_w, r.y1);
  bool focused = ui->focus == id;
  // the label scrubs.
  if (label_w > 0 && !focused) {
    civue_response lresp = civue_interact(ui, civue_ids(id, "label"), lr, CIVUE_SENSE_DRAG);
    if (lresp.hovered || lresp.dragging) civue_set_cursor(ui, CIVUE_CURSOR_RESIZE_H);
    NumState *st = num_state(ui, id);
    if (lresp.pressed) {
      st->scrubbing = !mixed;
      st->scrub_from = value;
      st->scrub_dx = 0;
    }
    if (lresp.dragging && !lresp.pressed && st->scrubbing) {
      st->scrub_dx += lresp.drag_delta.x * (ui->input.mods & CIVUE_MOD_SHIFT ? 10.0 : 1.0);
      f64 v = st->scrub_from + round(st->scrub_dx);
      if (mixed || v != value) out = (civue_num_change){CIVUE_NUM_SCRUB, v};
    }
    if (lresp.released && st->scrubbing) {
      st->scrubbing = false;
      out = (civue_num_change){CIVUE_NUM_SCRUB_END, 0};
    }
  }
  // the value types.
  civue_rect vr = rect(r.x0 + label_w, r.y0, r.x1, r.y1);
  NumState *st = num_state(ui, id);
  if (!focused && !st->text) {
    civue_response resp = civue_interact(ui, id, vr, CIVUE_SENSE_CLICK);
    if (look->idle) look->idle(ui, look->ctx, r, lr, vr, shown, resp.hovered);
    if (resp.pressed) {
      ui->focus = id;
      civue_claim_focus(ui);
      st->text = xstrdup(shown);
    }
    return out;
  }
  // arrows step the value without leaving the field.
  f64 step = 0;
  if (focused) {
    u32 kept = 0;
    vec_each(&ui->input.keys, k) {
      if (k->key == CIVUE_KEY_UP || k->key == CIVUE_KEY_DOWN) {
        f64 s = k->mods & CIVUE_MOD_SHIFT ? 10 : 1;
        step += k->key == CIVUE_KEY_UP ? s : -s;
        continue;
      }
      ui->input.keys.v[kept++] = *k;
    }
    ui->input.keys.len = kept;
  }
  // = asks for a variable and is not typed.
  bool wants_variable = false;
  if (focused && variable) {
    u32 kept = 0;
    vec_each(&ui->input.text, c) {
      if (*c == '=')
        wants_variable = true;
      else
        ui->input.text.v[kept++] = *c;
    }
    ui->input.text.len = kept;
    kept = 0;
    vec_each(&ui->input.keys, k) {
      if (k->key == '=' && !(k->mods & ~CIVUE_MOD_SHIFT)) {
        wants_variable = true;
        continue;
      }
      ui->input.keys.v[kept++] = *k;
    }
    ui->input.keys.len = kept;
  }
  bool escaped = civue_key_pressed(ui, CIVUE_KEY_ESCAPE);
  char *text = st->text ? st->text : xstrdup("");
  st->text = nullptr;
  civue_field_response fr = civue_text_field(ui, id, r, &text, "", look->style, look->field);
  const f64 *current = mixed ? nullptr : &value;
  if (step != 0) {
    f64 base = 0;
    if (!civue_eval_num(text ? text : "", current, &base)) base = mixed ? 0 : value;
    f64 v = base + step;
    char buf[64];
    show(v, buf, sizeof buf);
    free(text);
    text = xstrdup(buf);
    out = (civue_num_change){CIVUE_NUM_SET, v};
    // the field shows the new value, selected.
    civue_forget(ui, id);
  }
  bool left = !fr.focused || ui->focus != id;
  if (left) {
    const char *t = text ? text : "";
    if (!escaped && step == 0 && strcmp(t, shown)) {
      char **typed = typed_slot(ui, id);
      free(*typed);
      *typed = xstrdup(t);
    }
    f64 v;
    if (!escaped && step == 0 && civue_eval_num(t, current, &v) && (mixed || v != value))
      out = (civue_num_change){CIVUE_NUM_SET, v};
    free(text);
  } else {
    st = num_state(ui, id);
    st->text = text ? text : xstrdup("");
  }
  if (wants_variable) out = (civue_num_change){CIVUE_NUM_VARIABLE, 0};
  return out;
}

void civue_focus_num(civue_ui *ui, civue_id id, const char *text) {
  ui->focus = id;
  civue_claim_focus(ui);
  NumState *st = num_state(ui, id);
  free(st->text);
  st->text = xstrdup(text);
}

char *civue_num_field_typed(civue_ui *ui, civue_id id) {
  char **typed = typed_slot(ui, id);
  char *t = *typed;
  *typed = nullptr;
  return t;
}

// ---- arithmetic ----

typedef struct {
  const char *s;
  usize i, n;
} Parser;

static int peek(const Parser *p) { return p->i < p->n ? (u8)p->s[p->i] : -1; }

static bool sum(Parser *p, f64 *out);

static bool atom(Parser *p, f64 *out) {
  int c = peek(p);
  if (c < 0) return false;
  if (c == '-') {
    p->i++;
    if (!atom(p, out)) return false;
    *out = -*out;
    return true;
  }
  if (c == '(') {
    p->i++;
    if (!sum(p, out) || peek(p) != ')') return false;
    p->i++;
    return true;
  }
  usize start = p->i;
  while (peek(p) >= '0' && peek(p) <= '9') p->i++;
  if (peek(p) == '.') {
    p->i++;
    while (peek(p) >= '0' && peek(p) <= '9') p->i++;
  }
  // a number like rust's parse: digits with at most one point.
  if (p->i == start || (p->i == start + 1 && p->s[start] == '.')) return false;
  if (peek(p) == '.') return false;
  char buf[64];
  usize len = MIN(p->i - start, sizeof buf - 1);
  memcpy(buf, p->s + start, len);
  buf[len] = 0;
  *out = strtod(buf, nullptr);
  return true;
}

static bool product(Parser *p, f64 *out) {
  if (!atom(p, out)) return false;
  for (int c; (c = peek(p)) == '*' || c == '/';) {
    p->i++;
    f64 r;
    if (!atom(p, &r)) return false;
    *out = c == '*' ? *out * r : *out / r;
  }
  return true;
}

static bool sum(Parser *p, f64 *out) {
  if (!product(p, out)) return false;
  for (int c; (c = peek(p)) == '+' || c == '-';) {
    p->i++;
    f64 r;
    if (!product(p, &r)) return false;
    *out = c == '+' ? *out + r : *out - r;
  }
  return true;
}

bool civue_eval_num(const char *text, const f64 *current, f64 *out) {
  // spaces, percents and degrees go, commas are points.
  civue_bytes s = {};
  if (current) {
    usize i = 0;
    while (text[i] == ' ' || text[i] == '\t') i++;
    char c = text[i];
    if (c == '+' || c == '*' || c == '/') {
      char buf[64];
      snprintf(buf, sizeof buf, "%.17g", *current);
      buf_put(&s, buf, strlen(buf));
    }
  }
  for (const char *p = text; *p; p++) {
    if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '%') continue;
    // the degree sign, two bytes in utf-8.
    if ((u8)p[0] == 0xc2 && (u8)p[1] == 0xb0) {
      p++;
      continue;
    }
    vec_push(&s, *p == ',' ? '.' : *p);
  }
  bool ok = false;
  if (s.len) {
    Parser p = {(const char *)s.v, 0, s.len};
    f64 v;
    ok = sum(&p, &v) && p.i == p.n && isfinite(v);
    if (ok) *out = v;
  }
  vec_free(&s);
  return ok;
}

// ---- steppers ----

typedef struct {
  bool held;
  f64 from;
  u32 n;
} Held;

civue_steppers_state civue_steppers(civue_ui *ui, civue_id id, civue_rect up, civue_rect down, f64 value, f64 step,
                                    f64 big, f64 min, f64 max) {
  civue_steppers_state s = {.at_max = value >= max, .at_min = value <= min, .value = value};
  civue_rect halves[2] = {up, down};
  for (u32 k = 0; k < 2; k++) {
    f64 dir = k ? -1 : 1;
    civue_response resp = civue_interact(ui, civue_idn(civue_ids(id, "stepper"), k), halves[k], CIVUE_SENSE_DRAG);
    *(k ? &s.down : &s.up) = resp;
    f64 now = ui->now;
    Held *held = civue_memory(ui, civue_idn(civue_ids(id, "stepper-held"), k), sizeof(Held), nullptr);
    bool fire = resp.pressed;
    if (resp.pressed) {
      // figma's steppers leave the caret in their field.
      civue_keep_focus(ui);
      *held = (Held){true, now, 0};
    } else if (resp.dragging && held->held) {
      // figma's: the first repeat after 500ms, then every 100ms.
      if (now >= held->from + 0.5 + 0.1 * held->n) {
        fire = true;
        held->n++;
      }
      f64 next = held->from + 0.5 + 0.1 * held->n;
      civue_request_repaint_after(ui, fmax(next - now, 0));
    } else if (!resp.dragging) {
      held->held = false;
    }
    if (fire) {
      f64 by = ui->input.mods & CIVUE_MOD_SHIFT ? big : step;
      s.value = fmax(fmin((round(s.value / by) + dir) * by, max), min);
      s.stepped = true;
    }
  }
  return s;
}
