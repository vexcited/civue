#include "internal.h"

#include <civue/rich.h>
#include <civue/system.h>
#include <ctype.h>
#include <stddefer.h>

// ---- the doc ----

civue_rich_doc civue_rich_new() {
  civue_rich_doc d = {};
  vec_push(&d.blocks, CIVUE_RICH_PARA);
  return d;
}

civue_rich_doc civue_rich_clone(const civue_rich_doc *d) {
  civue_rich_doc c = {};
  vec_extend(&c.text, d->text.v, d->text.len);
  vec_extend(&c.fmt, d->fmt.v, d->fmt.len);
  vec_extend(&c.blocks, d->blocks.v, d->blocks.len);
  vec_each(&d->links, l) {
    vec_push(&c.links, xstrdup(*l));
  }
  return c;
}

bool civue_rich_is_empty(const civue_rich_doc *d) {
  if (d->text.len) {
    return false;
  }
  vec_each(&d->blocks, b) {
    if (*b != CIVUE_RICH_PARA) {
      return false;
    }
  }
  return true;
}

bool civue_rich_equal(const civue_rich_doc *a, const civue_rich_doc *b) {
  if (a->links.len != b->links.len) {
    return false;
  }
  for (u32 i = 0; i < a->links.len; i++) {
    if (strcmp(a->links.v[i], b->links.v[i])) {
      return false;
    }
  }
  return a->text.len == b->text.len && a->blocks.len == b->blocks.len &&
         (!a->text.len || (!memcmp(a->text.v, b->text.v, a->text.len) &&
                           !memcmp(a->fmt.v, b->fmt.v, a->fmt.len))) &&
         !memcmp(a->blocks.v, b->blocks.v, a->blocks.len);
}

static u32 count_lines(const u8 *p, usize n) {
  u32 lines = 0;
  for (usize i = 0; i < n; i++) {
    lines += p[i] == '\n';
  }
  return lines;
}

// the block holding byte `at`.
static u32 block_at(const civue_rich_doc *d, u32 at) {
  return count_lines(d->text.v, at);
}

// the bytes of block `k`, its newline left out.
static void block_span(const civue_rich_doc *d, u32 k, u32 *start, u32 *end) {
  u32 at = 0;
  for (u32 i = 0; i < k; i++) {
    while (at < d->text.len && d->text.v[at] != '\n') {
      at++;
    }
    at++;
  }
  *start = at;
  while (at < d->text.len && d->text.v[at] != '\n') {
    at++;
  }
  *end = at;
}

void civue_rich_insert(civue_rich_doc *d, u32 at, const char *s, usize n,
                       u8 f) {
  u32 k = block_at(d, at);
  u32 lines = count_lines((const u8 *)s, n);
  for (u32 i = 0; i < lines; i++) {
    vec_insert(&d->blocks, k + 1, CIVUE_RICH_PARA);
  }
  vec_reserve(&d->text, (u32)n);
  vec_reserve(&d->fmt, (u32)n);
  memmove(d->text.v + at + n, d->text.v + at, d->text.len - at);
  memmove(d->fmt.v + at + n, d->fmt.v + at, d->fmt.len - at);
  memcpy(d->text.v + at, s, n);
  for (usize i = 0; i < n; i++) {
    d->fmt.v[at + i] = s[i] == '\n' ? 0 : f;
  }
  d->text.len += (u32)n;
  d->fmt.len += (u32)n;
}

void civue_rich_delete(civue_rich_doc *d, u32 from, u32 to) {
  if (from >= to) {
    return;
  }
  u32 k = block_at(d, from);
  u32 lines = count_lines(d->text.v + from, to - from);
  for (u32 i = 0; i < lines; i++) {
    vec_remove(&d->blocks, k + 1);
  }
  memmove(d->text.v + from, d->text.v + to, d->text.len - to);
  memmove(d->fmt.v + from, d->fmt.v + to, d->fmt.len - to);
  d->text.len -= to - from;
  d->fmt.len -= to - from;
}

void civue_rich_toggle(civue_rich_doc *d, u32 from, u32 to, u8 f) {
  bool on = false;
  for (u32 i = from; i < to; i++) {
    if (d->text.v[i] != '\n' && !(d->fmt.v[i] & f)) {
      on = true;
    }
  }
  for (u32 i = from; i < to; i++) {
    if (d->text.v[i] != '\n') {
      d->fmt.v[i] = on ? d->fmt.v[i] | f : d->fmt.v[i] & (u8)~f;
    }
  }
}

// ---- html ----

static void escape_into(civue_bytes *out, const u8 *s, usize n) {
  for (usize i = 0; i < n; i++) {
    switch (s[i]) {
      case '&':
        buf_puts(out, "&amp;");
        break;
      case '<':
        buf_puts(out, "&lt;");
        break;
      case '>':
        buf_puts(out, "&gt;");
        break;
      default:
        buf_putc(out, (char)s[i]);
    }
  }
}

static constexpr char WRAP[] = " style=\"white-space: pre-wrap;\"";

static void escape_attribute(civue_bytes *out, const char *s) {
  for (; *s; s++) {
    switch (*s) {
      case '&':
        buf_puts(out, "&amp;");
        break;
      case '<':
        buf_puts(out, "&lt;");
        break;
      case '>':
        buf_puts(out, "&gt;");
        break;
      case '"':
        buf_puts(out, "&quot;");
        break;
      default:
        buf_putc(out, *s);
    }
  }
}

// a run of one format. the annotations' html keeps plain text bare, the
// lexical html of descriptions wraps it in a span, bold in b, italic in i
// and struck text in s, and puts a link around the run.
static void run_html(const civue_rich_doc *d, u32 at, u32 end, civue_bytes *out,
                     bool lexical) {
  u8 f = d->fmt.v[at];
  bool b = f & CIVUE_RICH_BOLD, i = f & CIVUE_RICH_ITALIC,
       s = f & CIVUE_RICH_STRIKE;
  u32 link = f >> CIVUE_RICH_LINK_SHIFT;
  if (link && link <= d->links.len) {
    buf_puts(out, "<a href=\"");
    escape_attribute(out, d->links.v[link - 1]);
    buf_puts(out,
             "\" target=\"_blank\" rel=\"noreferrer noopener nofollow ugc\">");
  }
  else {
    link = 0;
  }
  if (!lexical) {
    if (f & CIVUE_RICH_CODE) {
      buf_printf(out, "<code spellcheck=\"false\"%s>", WRAP);
      if (b) {
        buf_puts(out, "<strong>");
      }
      if (i) {
        buf_puts(out, "<em>");
      }
      escape_into(out, d->text.v + at, end - at);
      if (i) {
        buf_puts(out, "</em>");
      }
      if (b) {
        buf_puts(out, "</strong>");
      }
      buf_puts(out, "</code>");
    }
    else if (b || i) {
      if (i) {
        buf_puts(out, "<i>");
      }
      buf_puts(out, b ? "<strong" : "<em");
      buf_puts(out, WRAP);
      buf_putc(out, '>');
      escape_into(out, d->text.v + at, end - at);
      buf_puts(out, b ? "</strong>" : "</em>");
      if (i) {
        buf_puts(out, "</i>");
      }
    }
    else {
      escape_into(out, d->text.v + at, end - at);
    }
  }
  else {
    // the wrappers go on innermost bold, then italic, then struck.
    if (s) {
      buf_puts(out, "<s>");
    }
    if (i) {
      buf_puts(out, "<i>");
    }
    if (b) {
      buf_puts(out, "<b>");
    }
    const char *tag = f & CIVUE_RICH_CODE ? "code"
                      : b                 ? "strong"
                      : i                 ? "em"
                                          : "span";
    buf_printf(out, "<%s%s%s>", tag,
               f & CIVUE_RICH_CODE ? " spellcheck=\"false\"" : "", WRAP);
    escape_into(out, d->text.v + at, end - at);
    buf_printf(out, "</%s>", tag);
    if (b) {
      buf_puts(out, "</b>");
    }
    if (i) {
      buf_puts(out, "</i>");
    }
    if (s) {
      buf_puts(out, "</s>");
    }
  }
  if (link) {
    buf_puts(out, "</a>");
  }
}

static void runs_html(const civue_rich_doc *d, u32 from, u32 to,
                      civue_bytes *out, bool lexical) {
  if (from == to) {
    buf_puts(out, "<br>");
    return;
  }
  for (u32 at = from; at < to;) {
    u8 f = d->fmt.v[at];
    u32 end = at;
    while (end < to && d->fmt.v[end] == f) {
      end++;
    }
    run_html(d, at, end, out, lexical);
    at = end;
  }
}

static char *to_html(const civue_rich_doc *d, bool lexical) {
  civue_bytes out = {};
  u32 start = 0, end = 0;
  for (u32 k = 0; k < d->blocks.len;) {
    block_span(d, k, &start, &end);
    civue_rich_block kind = d->blocks.v[k];
    if (kind == CIVUE_RICH_PARA || kind == CIVUE_RICH_H1) {
      const char *tag = kind == CIVUE_RICH_H1 ? "h1" : "p";
      buf_printf(&out, "<%s>", tag);
      runs_html(d, start, end, &out, lexical);
      buf_printf(&out, "</%s>", tag);
      k++;
    }
    else if (kind == CIVUE_RICH_CODE_BLOCK) {
      // the lines of one block, a break between them.
      buf_puts(&out, "<pre spellcheck=\"false\">");
      for (u32 n = 0; k < d->blocks.len && d->blocks.v[k] == kind; n++, k++) {
        block_span(d, k, &start, &end);
        if (n) {
          buf_puts(&out, "<br>");
        }
        if (start < end) {
          buf_printf(&out, "<span%s>", WRAP);
          escape_into(&out, d->text.v + start, end - start);
          buf_puts(&out, "</span>");
        }
      }
      buf_puts(&out, "</pre>");
    }
    else if (lexical) {
      const char *tag = kind == CIVUE_RICH_BULLET ? "ul" : "ol";
      buf_printf(&out, "<%s>", tag);
      for (u32 n = 1; k < d->blocks.len && d->blocks.v[k] == kind; n++, k++) {
        block_span(d, k, &start, &end);
        buf_printf(&out, "<li value=\"%u\">", n);
        runs_html(d, start, end, &out, lexical);
        buf_puts(&out, "</li>");
      }
      buf_printf(&out, "</%s>", tag);
    }
    else {
      buf_puts(&out, "<ol>");
      for (u32 n = 1; k < d->blocks.len && d->blocks.v[k] == kind; n++, k++) {
        block_span(d, k, &start, &end);
        buf_printf(&out, "<li value=\"%u\" data-list=\"%s\">", n,
                   kind == CIVUE_RICH_BULLET ? "bullet" : "ordered");
        runs_html(d, start, end, &out, lexical);
        buf_puts(&out, "</li>");
      }
      buf_puts(&out, "</ol>");
    }
  }
  return buf_take(&out);
}

char *civue_rich_to_html(const civue_rich_doc *d) {
  return to_html(d, false);
}

char *civue_rich_to_lexical_html(const civue_rich_doc *d) {
  return to_html(d, true);
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
  return c >= 0x80 || isalnum(c);
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

// ---- where bytes and points are ----

static f32 text_caret_x(const civue_text_block *b, u32 byte) {
  f32 x = 0;
  for (u32 i = 0; i < b->ncarets; i++) {
    if (b->carets[i].byte > byte) {
      break;
    }
    x = b->carets[i].x;
  }
  return x;
}

// the x of byte `at` of a piece, its trailing spaces counted.
static f32 piece_x(const civue_rich_piece *p, u32 at) {
  if (at <= p->ink) {
    return p->text && at > p->start
             ? p->x + text_caret_x(p->text, at - p->start)
             : p->x;
  }
  return p->x + p->ink_w + p->space * (f32)(at - p->ink);
}

// where the caret of byte `at` is, from the origin: its x and its line's top.
// at a wrap it goes to the start of the next line.
static civue_vec2 caret_pos(const civue_rich_doc *d, const civue_rich_lay *l,
                            u32 at) {
  u32 k = block_at(d, at);
  const civue_rich_piece *last = nullptr;
  vec_each(&l->pieces, p) {
    if (p->block != k) {
      continue;
    }
    if (at >= p->start && at < p->end) {
      return v2(piece_x(p, at), p->y);
    }
    last = p;
  }
  if (last) {
    return v2(piece_x(last, at), last->y);
  }
  return v2(l->boxes.v[k].indent, l->boxes.v[k].y);
}

// whether the caret of byte `at` ends a wrapped line: the spaces after it
// hang past the line, and what follows them is on the next one.
static bool line_ends_at(const civue_rich_doc *d, const civue_rich_lay *l,
                         u32 at) {
  u32 next = at;
  while (next < d->text.len && d->text.v[next] == ' ') {
    next++;
  }
  return next < d->text.len && d->text.v[next] != '\n' &&
         caret_pos(d, l, next).y != caret_pos(d, l, at).y;
}

// the byte nearest to point `p` from the origin.
static u32 hit(const civue_rich_lay *l, civue_vec2 p) {
  u32 k = l->boxes.len - 1;
  for (u32 i = 0; i < l->boxes.len; i++) {
    if (p.y < l->boxes.v[i].y + l->boxes.v[i].height) {
      k = i;
      break;
    }
  }
  const civue_rich_box *box = &l->boxes.v[k];
  // a code block's padding under its last line is not a line.
  f32 lines = fmaxf(floorf(box->height / box->line + .01f), 1);
  f32 line =
    fminf(fmaxf(floorf((f32)(p.y - box->y) / box->line), 0), lines - 1);
  f32 y = box->y + line * box->line;
  u32 best = box->start;
  f32 dist = INFINITY;
  vec_each(&l->pieces, pc) {
    if (pc->block != k || fabsf(pc->y - y) > .5f) {
      continue;
    }
    // the end of a piece is the start of the next one's, but a block's last
    // piece and a wrapped line's spaces have none to give it.
    bool last = pc->end == box->end;
    f32 x = pc->x;
    if (!pc->text || pc->ink == pc->start) {
      f32 d = fabsf(x - (f32)p.x);
      if (d < dist) {
        dist = d, best = pc->start;
      }
    }
    for (u32 i = 0; pc->text && i < pc->text->ncarets; i++) {
      u32 byte = pc->start + pc->text->carets[i].byte;
      if (byte > pc->ink || (byte == pc->end && !last)) {
        continue;
      }
      f32 d = fabsf(pc->x + pc->text->carets[i].x - (f32)p.x);
      if (d < dist) {
        dist = d, best = byte;
      }
    }
    for (u32 byte = pc->ink + 1; byte <= pc->end; byte++) {
      if (byte == pc->end && !last) {
        continue;
      }
      f32 d = fabsf(piece_x(pc, byte) - (f32)p.x);
      if (d < dist) {
        dist = d, best = byte;
      }
    }
  }
  return best;
}

// ---- the editor ----

typedef struct {
  bool live;
  u32 anchor, focus;
  // the press the pointer's selection grows from.
  civue_text_grip grip;
  // the format of what is typed next after toggling one with no selection,
  // negative for none.
  i32 pending;
  // the x up and down keep to, negative for none.
  f32 goal;
  // when the caret last moved, it stays lit while typing.
  f64 blink;
  // enter breaks the block, and the link shortcut was pressed.
  bool multiline, link_asked;
} Editing;

static void move_to(Editing *st, u32 at, bool select) {
  st->focus = at;
  if (!select) {
    st->anchor = at;
  }
}

// what is typed at `at` takes: the format before it in its block, else the
// one after.
static u8 typed_fmt(const civue_rich_doc *d, u32 at) {
  bool before = at > 0 && d->text.v[at - 1] != '\n';
  bool after = at < d->text.len && d->text.v[at] != '\n';
  u8 f = before ? d->fmt.v[prev_char(&d->text, at)] : after ? d->fmt.v[at] : 0;
  // text typed at a link's ends is not part of it, only inside it.
  u8 link = f & CIVUE_RICH_LINK_MASK;
  if (link &&
      !(before && after && (d->fmt.v[at] & CIVUE_RICH_LINK_MASK) == link)) {
    f &= (u8)~CIVUE_RICH_LINK_MASK;
  }
  return f;
}

// the last place `needle` starts in the `n` bytes of `s`, -1 for none.
static i32 rfind(const u8 *s, u32 n, const char *needle) {
  u32 m = (u32)strlen(needle);
  for (i32 i = (i32)n - (i32)m; i >= 0; i--) {
    if (!memcmp(s + i, needle, m)) {
      return i;
    }
  }
  return -1;
}

// text that can be bold, italic or code between markers: some of it, not
// started or ended by a space.
static bool valid(const u8 *s, u32 n) {
  return n && s[0] != ' ' && s[n - 1] != ' ';
}

// the markers around `content` come out, the content takes the format.
static void close_markers(civue_rich_doc *d, Editing *st, u32 at, u32 f,
                          u32 open, u32 marker, u32 from, u32 to, u8 add) {
  // the closing marker never went in, the opening one and any typed half of
  // the closing one come out.
  for (u32 i = from; i < to; i++) {
    d->fmt.v[i] |= add;
  }
  civue_rich_delete(d, to, at);
  civue_rich_delete(d, open, open + marker);
  st->anchor = st->focus = to - marker;
  // what follows is typed as before the markers, figma's.
  st->pending = (i32)(f & (u8)~add);
}

// one typed character with figma's markdown shortcuts: a list started by
// "- " or "1. ", and `code`, **bold** and *italic* or _italic_ closed.
static void type_char(civue_rich_doc *d, Editing *st, u32 cp) {
  u32 lo = MIN(st->anchor, st->focus), hi = MAX(st->anchor, st->focus);
  u8 f = st->pending >= 0 ? (u8)st->pending : typed_fmt(d, lo);
  st->pending = -1;
  civue_rich_delete(d, lo, hi);
  u32 at = lo, b = block_at(d, at), start, end;
  block_span(d, b, &start, &end);
  const u8 *before = d->text.v + start;
  u32 n = at - start;
  if (cp == ' ' && d->blocks.v[b] == CIVUE_RICH_PARA) {
    civue_rich_block kind = CIVUE_RICH_PARA;
    if (n == 1 && (before[0] == '-' || before[0] == '*' || before[0] == '+')) {
      kind = CIVUE_RICH_BULLET;
    }
    else if (n > 1 && before[n - 1] == '.') {
      bool digits = true;
      for (u32 i = 0; i + 1 < n; i++) {
        digits &= isdigit(before[i]) != 0;
      }
      if (digits) {
        kind = CIVUE_RICH_ORDERED;
      }
    }
    if (kind != CIVUE_RICH_PARA) {
      civue_rich_delete(d, start, at);
      d->blocks.v[b] = kind;
      st->anchor = st->focus = start;
      return;
    }
  }
  // a marker opens after a space or at the start of the block.
#define OPENS(i) ((i) == start || isspace(d->text.v[(i) - 1]))
  if (cp == '`') {
    i32 j = rfind(before, n, "`");
    if (j >= 0 && valid(before + j + 1, n - (u32)j - 1)) {
      u32 open = start + (u32)j;
      close_markers(d, st, at, f, open, 1, open + 1, at, CIVUE_RICH_CODE);
      return;
    }
  }
  if (cp == '*' || cp == '_') {
    char m[3] = {(char)cp, (char)cp, 0};
    if (n && before[n - 1] == cp) {
      u32 body = n - 1;
      i32 j = rfind(before, body, m);
      if (j >= 0 && valid(before + j + 2, body - (u32)j - 2) &&
          OPENS(start + (u32)j)) {
        u32 open = start + (u32)j;
        close_markers(d, st, at, f, open, 2, open + 2, at - 1, CIVUE_RICH_BOLD);
        return;
      }
    }
    i32 j = rfind(before, n, m + 1);
    if (j >= 0 && valid(before + j + 1, n - (u32)j - 1) &&
        before[j + 1] != cp && OPENS(start + (u32)j)) {
      u32 open = start + (u32)j;
      close_markers(d, st, at, f, open, 1, open + 1, at, CIVUE_RICH_ITALIC);
      return;
    }
  }
#undef OPENS
  char utf[4];
  usize len = utf8_put(utf, cp);
  civue_rich_insert(d, at, utf, len, f);
  st->anchor = st->focus = at + (u32)len;
}

// the format of what is typed toggled: over the selection, or for what is
// typed next when it is none. true when the doc changed.
static bool act_format(civue_rich_doc *d, Editing *st, u8 f) {
  u32 lo = MIN(st->anchor, st->focus), hi = MAX(st->anchor, st->focus);
  if (lo == hi) {
    u8 base = st->pending >= 0 ? (u8)st->pending : typed_fmt(d, lo);
    st->pending = base ^ f;
    return false;
  }
  civue_rich_toggle(d, lo, hi, f);
  return true;
}

// the blocks the selection touches made `kind`, or paragraphs when all of
// them were.
static bool act_block(civue_rich_doc *d, Editing *st, civue_rich_block kind) {
  u32 lo = MIN(st->anchor, st->focus), hi = MAX(st->anchor, st->focus);
  u32 first = block_at(d, lo), last = block_at(d, hi);
  bool all = true;
  for (u32 b = first; b <= last; b++) {
    all &= d->blocks.v[b] == kind;
  }
  for (u32 b = first; b <= last; b++) {
    d->blocks.v[b] = all ? CIVUE_RICH_PARA : kind;
  }
  return true;
}

// a key of the editor, true when it changed the doc.
static bool edit_key(civue_rich_doc *d, Editing *st, const civue_rich_lay *l,
                     const civue_key_press *k, bool *touched) {
  bool shift = k->mods & CIVUE_MOD_SHIFT, word = k->mods & CIVUE_MOD_ALT,
       line = k->mods & CIVUE_MOD_CMD, command = k->mods & CIVUE_MOD_CMD;
  const civue_bytes *t = &d->text;
  u32 lo = MIN(st->anchor, st->focus), hi = MAX(st->anchor, st->focus);
  bool changed = false, vertical = false;
  u32 bs, be;
  block_span(d, block_at(d, st->focus), &bs, &be);
  *touched = true;
  switch (k->key) {
    case CIVUE_KEY_LEFT:
      st->pending = -1;
      move_to(st,
              line                 ? bs
              : word               ? word_left(t, st->focus)
              : !shift && lo != hi ? lo
                                   : prev_char(t, st->focus),
              shift);
      break;
    case CIVUE_KEY_RIGHT:
      st->pending = -1;
      move_to(st,
              line                 ? be
              : word               ? word_right(t, st->focus)
              : !shift && lo != hi ? hi
                                   : next_char(t, st->focus),
              shift);
      break;
    case CIVUE_KEY_UP:
    case CIVUE_KEY_DOWN: {
      st->pending = -1;
      vertical = true;
      civue_vec2 c = caret_pos(d, l, st->focus);
      if (st->goal < 0) {
        st->goal = (f32)c.x;
      }
      f32 line = l->boxes.v[block_at(d, st->focus)].line;
      f32 y = (f32)c.y + (k->key == CIVUE_KEY_UP ? -1 : line + 1);
      move_to(st,
              y < 0           ? 0
              : y > l->height ? t->len
                              : hit(l, v2(st->goal, y)),
              shift);
      break;
    }
    case CIVUE_KEY_HOME:
      move_to(st, 0, shift);
      break;
    case CIVUE_KEY_END:
      move_to(st, t->len, shift);
      break;
    case 'a':
      if (command) {
        st->anchor = 0, st->focus = t->len;
      }
      break;
    case 'b':
    case 'i':
      if (!command) {
        break;
      }
      changed =
        act_format(d, st, k->key == 'b' ? CIVUE_RICH_BOLD : CIVUE_RICH_ITALIC);
      break;
    case 'x':
    case 'c':
    case 'u':
      // shift turns them to format shortcuts, with alt a code block.
      if (command && shift) {
        u8 f = k->key == 'x' ? CIVUE_RICH_STRIKE : CIVUE_RICH_CODE;
        if (k->key == 'u') {
          st->link_asked = true;
        }
        else if (k->key == 'c' && word) {
          changed = act_block(d, st, CIVUE_RICH_CODE_BLOCK);
        }
        else {
          changed = act_format(d, st, f);
        }
        break;
      }
      if (!command || k->key == 'u') {
        break;
      }
      if (lo != hi) {
        civue_clipboard_set_text((const char *)t->v + lo, hi - lo);
      }
      if (k->key == 'x' && lo != hi) {
        civue_rich_delete(d, lo, hi);
        move_to(st, lo, false);
        changed = true;
      }
      break;
    case 'v':
      if (!command) {
        break;
      }
      {
        char *s = civue_clipboard_text();
        if (!s) {
          break;
        }
        // control characters are dropped, newlines kept for blocks.
        civue_bytes clean = {};
        for (const char *c = s; *c; c++) {
          if (*c == '\r' && c[1] == '\n') {
            continue;
          }
          if ((u8)*c >= 0x20 || *c == '\n') {
            vec_push(&clean, (u8)*c);
          }
        }
        u8 f = typed_fmt(d, lo);
        civue_rich_delete(d, lo, hi);
        civue_rich_insert(d, lo, (const char *)clean.v, clean.len, f);
        move_to(st, lo + clean.len, false);
        changed = true;
        vec_free(&clean);
        free(s);
      }
      break;
    case CIVUE_KEY_ENTER:
      if (!shift && !st->multiline) {
        break;
      }
      civue_rich_delete(d, lo, hi);
      {
        u32 at = lo, b = block_at(d, at), s, e;
        block_span(d, b, &s, &e);
        if (d->blocks.v[b] != CIVUE_RICH_PARA && s == e) {
          // an empty list item leaves the list, figma's.
          d->blocks.v[b] = CIVUE_RICH_PARA;
        }
        else {
          civue_rich_block kind = d->blocks.v[b];
          civue_rich_insert(d, at, "\n", 1, 0);
          // a header's end is followed by a paragraph.
          d->blocks.v[b + 1] =
            kind == CIVUE_RICH_H1 && at == e ? CIVUE_RICH_PARA : kind;
          move_to(st, at + 1, false);
        }
      }
      changed = true;
      break;
    case CIVUE_KEY_BACKSPACE:
      if (lo != hi) {
        civue_rich_delete(d, lo, hi);
        move_to(st, lo, false);
      }
      else {
        u32 at = st->focus, b = block_at(d, at);
        if (at == bs && d->blocks.v[b] != CIVUE_RICH_PARA) {
          d->blocks.v[b] = CIVUE_RICH_PARA;
        }
        else if (at > 0) {
          u32 from = line   ? (at == bs ? at - 1 : bs)
                     : word ? word_left(t, at)
                            : prev_char(t, at);
          civue_rich_delete(d, from, at);
          move_to(st, from, false);
        }
      }
      changed = true;
      break;
    case CIVUE_KEY_DELETE: {
      u32 end = lo != hi ? hi : word ? word_right(t, lo) : next_char(t, lo);
      civue_rich_delete(d, lo, end);
      move_to(st, lo, false);
      changed = true;
      break;
    }
    default:
      break;
  }
  if (!vertical) {
    st->goal = -1;
  }
  return changed;
}

static Editing *editing(civue_ui *ui, civue_id id) {
  return civue_memory(ui, id, sizeof(Editing), nullptr);
}

bool civue_rich_caret_to_end(civue_ui *ui, civue_id id,
                             const civue_rich_doc *d) {
  if (ui->focus != id) {
    return false;
  }
  Editing *st = editing(ui, id);
  if (!st->live) {
    return false;
  }
  st->anchor = st->focus = d->text.len;
  return true;
}

// the selected bytes' grounds, and the newlines inside them.
static void selection(civue_ui *ui, const civue_rich_doc *d,
                      const civue_rich_lay *l, civue_vec2 origin, u32 lo,
                      u32 hi, civue_color color) {
  vec_each(&l->pieces, p) {
    u32 s = MAX(lo, p->start), e = MIN(hi, p->end);
    if (s >= e) {
      continue;
    }
    f32 x0 = piece_x(p, s), x1 = piece_x(p, e);
    civue_fill(ui,
               rect(origin.x + x0, origin.y + p->y, origin.x + x1,
                    origin.y + p->y + l->boxes.v[p->block].line),
               color, 0);
  }
  for (u32 k = 0; k < l->boxes.len; k++) {
    u32 end = l->boxes.v[k].end;
    if (end < lo || end >= hi) {
      continue;
    }
    civue_vec2 c = caret_pos(d, l, end);
    civue_fill(ui,
               rect_xywh(origin.x + c.x, origin.y + c.y, 4, l->boxes.v[k].line),
               color, 0);
  }
}

static civue_rich_response
rich_edit_at(civue_ui *ui, civue_id id, civue_vec2 origin, f32 width,
             civue_rich_doc *d, civue_text_style style,
             civue_rich_colors colors, bool multiline);

civue_rich_response civue_rich_edit(civue_ui *ui, civue_id id,
                                    civue_vec2 origin, f32 width,
                                    civue_rich_doc *d, civue_text_style style,
                                    civue_rich_colors colors) {
  return rich_edit_at(ui, id, origin, width, d, style, colors, false);
}

civue_rich_response civue_rich_edit_multiline(civue_ui *ui, civue_id id,
                                              civue_vec2 origin, f32 width,
                                              civue_rich_doc *d,
                                              civue_text_style style,
                                              civue_rich_colors colors) {
  return rich_edit_at(ui, id, origin, width, d, style, colors, true);
}

static civue_rich_response
rich_edit_at(civue_ui *ui, civue_id id, civue_vec2 origin, f32 width,
             civue_rich_doc *d, civue_text_style style,
             civue_rich_colors colors, bool multiline) {
  civue_rich_lay l;
  civue_rich_layout(ui, d, style, width, &l);
  defer civue_rich_lay_free(&l);
  civue_rect area = rect_xywh(origin.x, origin.y, width, l.height);
  civue_response resp = civue_interact(ui, id, area, CIVUE_SENSE_DRAG);
  if (resp.hovered) {
    civue_set_cursor(ui, CIVUE_CURSOR_TEXT);
  }
  civue_rich_response out = {.height = l.height};
  if (ui->focus != id) {
    civue_forget(ui, id);
    civue_rich_paint(ui, d, &l, origin, style, colors);
    return out;
  }
  civue_claim_focus(ui);
  Editing *st = editing(ui, id);
  bool fresh = !st->live;
  if (fresh) {
    *st = (Editing){.live = true, .pending = -1, .goal = -1, .blink = ui->now};
    st->anchor = st->focus = d->text.len;
    st->grip.base = st->grip.from = st->grip.to = d->text.len;
  }
  st->multiline = multiline;
  // the text may have changed under the caret since the last frame.
  st->focus = MIN(st->focus, d->text.len);
  st->anchor = MIN(st->anchor, d->text.len);
  civue_input *in = &ui->input;
  bool touched = false, changed = false;
  if (in->has_mouse && !fresh) {
    civue_vec2 p = v2_sub(in->mouse, origin);
    const char *text = (const char *)d->text.v;
    u32 at = hit(&l, p);
    bool line_end = line_ends_at(d, &l, at);
    if (resp.pressed) {
      civue_text_press(&st->grip, text, d->text.len, at, line_end, in->clicks,
                       in->mods & CIVUE_MOD_SHIFT, &st->anchor, &st->focus);
      st->goal = -1;
      st->pending = -1;
      touched = true;
    }
    else if (resp.dragging) {
      civue_text_drag(&st->grip, text, d->text.len, at, line_end, &st->anchor,
                      &st->focus);
      touched = true;
    }
  }
  bool stale = false;
  vec_each(&in->keys, k) {
    if (stale) {
      civue_rich_lay_free(&l);
      civue_rich_layout(ui, d, style, width, &l);
      stale = false;
    }
    bool t = false;
    if (edit_key(d, st, &l, k, &t)) {
      changed = stale = true;
    }
    touched |= t;
    st->focus = MIN(st->focus, d->text.len);
    st->anchor = MIN(st->anchor, d->text.len);
  }
  for (usize i = 0; i < in->text.len;) {
    u32 cp = utf8_next((const char *)in->text.v, in->text.len, &i);
    if (cp < 0x20 || cp == 0x7f) {
      continue;
    }
    type_char(d, st, cp);
    changed = stale = touched = true;
  }
  if (stale) {
    civue_rich_lay_free(&l);
    civue_rich_layout(ui, d, style, width, &l);
  }
  out.changed = changed;
  out.height = l.height;
  out.link_request = st->link_asked;
  st->link_asked = false;
  {
    civue_vec2 c = caret_pos(d, &l, st->focus);
    out.has_caret = true;
    out.caret = c;
    out.caret_line = l.boxes.v[block_at(d, st->focus)].line;
    ui->caret = rect_xywh(origin.x + c.x, origin.y + c.y, 1, out.caret_line);
  }
  if (touched) {
    st->blink = ui->now;
  }
  // the selection behind, then the text and the blinking caret.
  u32 lo = MIN(st->anchor, st->focus), hi = MAX(st->anchor, st->focus);
  if (lo != hi) {
    selection(ui, d, &l, origin, lo, hi, colors.selection);
  }
  civue_rich_paint(ui, d, &l, origin, style, colors);
  f64 since = ui->now - st->blink;
  if (lo == hi && (u64)(since / 0.53) % 2 == 0) {
    civue_vec2 c = caret_pos(d, &l, st->focus);
    f32 line = l.boxes.v[block_at(d, st->focus)].line;
    civue_fill(ui, rect_xywh(origin.x + c.x, origin.y + c.y, 1, line),
               colors.text, 0);
  }
  civue_request_repaint_after(ui, 0.53 - fmod(since, 0.53));
  return out;
}

// ---- the toolbar ----

static Editing *live(civue_ui *ui, civue_id id) {
  if (ui->focus != id) {
    return nullptr;
  }
  Editing *st = editing(ui, id);
  return st->live ? st : nullptr;
}

bool civue_rich_selection(civue_ui *ui, civue_id id, u32 *lo, u32 *hi) {
  Editing *st = live(ui, id);
  if (!st) {
    return false;
  }
  *lo = MIN(st->anchor, st->focus);
  *hi = MAX(st->anchor, st->focus);
  return true;
}

void civue_rich_select(civue_ui *ui, civue_id id, u32 lo, u32 hi) {
  Editing *st = live(ui, id);
  if (!st) {
    return;
  }
  st->anchor = lo;
  st->focus = hi;
}

static u8 action_flag(civue_rich_action a) {
  return a == CIVUE_RICH_ACT_BOLD     ? CIVUE_RICH_BOLD
         : a == CIVUE_RICH_ACT_ITALIC ? CIVUE_RICH_ITALIC
         : a == CIVUE_RICH_ACT_STRIKE ? CIVUE_RICH_STRIKE
         : a == CIVUE_RICH_ACT_CODE   ? CIVUE_RICH_CODE
                                      : 0;
}

static civue_rich_block action_block(civue_rich_action a) {
  return a == CIVUE_RICH_ACT_H1        ? CIVUE_RICH_H1
         : a == CIVUE_RICH_ACT_BULLETS ? CIVUE_RICH_BULLET
         : a == CIVUE_RICH_ACT_NUMBERS ? CIVUE_RICH_ORDERED
                                       : CIVUE_RICH_CODE_BLOCK;
}

bool civue_rich_active(civue_ui *ui, civue_id id, const civue_rich_doc *d,
                       civue_rich_action a) {
  Editing *st = live(ui, id);
  if (!st) {
    return false;
  }
  u32 lo = MIN(MIN(st->anchor, st->focus), d->text.len);
  u32 hi = MIN(MAX(st->anchor, st->focus), d->text.len);
  u8 f = action_flag(a);
  if (f) {
    if (lo == hi) {
      u8 now = st->pending >= 0 ? (u8)st->pending : typed_fmt(d, lo);
      return now & f;
    }
    bool any = false;
    for (u32 i = lo; i < hi; i++) {
      if (d->text.v[i] == '\n') {
        continue;
      }
      if (!(d->fmt.v[i] & f)) {
        return false;
      }
      any = true;
    }
    return any;
  }
  civue_rich_block kind = action_block(a);
  for (u32 b = block_at(d, lo); b <= block_at(d, hi); b++) {
    if (d->blocks.v[b] != kind) {
      return false;
    }
  }
  return true;
}

bool civue_rich_act(civue_ui *ui, civue_id id, civue_rich_doc *d,
                    civue_rich_action a) {
  Editing *st = live(ui, id);
  if (!st) {
    return false;
  }
  st->anchor = MIN(st->anchor, d->text.len);
  st->focus = MIN(st->focus, d->text.len);
  u8 f = action_flag(a);
  st->blink = ui->now;
  return f ? act_format(d, st, f) : act_block(d, st, action_block(a));
}

void civue_rich_set_link(civue_rich_doc *d, u32 lo, u32 hi, const char *url) {
  u8 index = 0;
  if (url && *url) {
    for (u32 i = 0; i < d->links.len && !index; i++) {
      if (!strcmp(d->links.v[i], url)) {
        index = (u8)(i + 1);
      }
    }
    if (!index && d->links.len < CIVUE_RICH_MAX_LINKS) {
      vec_push(&d->links, xstrdup(url));
      index = (u8)d->links.len;
    }
  }
  for (u32 i = lo; i < hi && i < d->text.len; i++) {
    if (d->text.v[i] != '\n') {
      d->fmt.v[i] =
        (d->fmt.v[i] & CIVUE_RICH_FLAGS) | (u8)(index << CIVUE_RICH_LINK_SHIFT);
    }
  }
}

// the link number of byte `i`, 0 for none.
static u32 link_number(const civue_rich_doc *d, u32 i) {
  return i < d->text.len && d->text.v[i] != '\n'
           ? d->fmt.v[i] >> CIVUE_RICH_LINK_SHIFT
           : 0;
}

const char *civue_rich_link_at(const civue_rich_doc *d, u32 at, u32 *from,
                               u32 *to) {
  u32 pos = link_number(d, at)                 ? at
            : at > 0 && link_number(d, at - 1) ? at - 1
                                               : at;
  u32 link = link_number(d, pos);
  if (!link || link > d->links.len) {
    return nullptr;
  }
  u32 a = pos, b = pos + 1;
  while (a > 0 && link_number(d, a - 1) == link) {
    a--;
  }
  while (link_number(d, b) == link) {
    b++;
  }
  if (from) {
    *from = a;
  }
  if (to) {
    *to = b;
  }
  return d->links.v[link - 1];
}
