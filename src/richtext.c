#include "internal.h"

#include <civue/rich.h>

// the doc's link number for `url`, 0 when it has too many.
static u8 link_of(civue_rich_doc *d, const char *url, usize n) {
  for (u32 i = 0; i < d->links.len; i++)
    if (strlen(d->links.v[i]) == n && !memcmp(d->links.v[i], url, n)) return (u8)(i + 1);
  if (d->links.len >= CIVUE_RICH_MAX_LINKS) return 0;
  vec_push(&d->links, xstrndup(url, n));
  return (u8)d->links.len;
}

static void open_block(civue_rich_doc *d, civue_rich_block kind, bool *started) {
  if (*started) {
    buf_putc(&d->text, '\n');
    buf_putc(&d->fmt, 0);
  }
  vec_push(&d->blocks, kind);
  *started = true;
}

// html entities figma's editor writes.
static void unescape_into(civue_rich_doc *d, const char *s, usize n, u8 f, civue_rich_block kind, bool *started) {
  static const struct {
    const char *from, *to;
  } entities[] = {{"&lt;", "<"},    {"&gt;", ">"},  {"&nbsp;", "\xc2\xa0"},
                  {"&quot;", "\""}, {"&#39;", "'"}, {"&amp;", "&"}};
  for (usize i = 0; i < n;) {
    const char *put = &s[i];
    usize len = 1, skip = 1;
    if (s[i] == '&')
      for (u32 k = 0; k < countof(entities); k++) {
        usize el = strlen(entities[k].from);
        if (i + el <= n && !memcmp(s + i, entities[k].from, el)) {
          put = entities[k].to, len = strlen(put), skip = el;
          break;
        }
      }
    if (!*started) open_block(d, kind, started);
    if (*put == '\n')
      open_block(d, kind, started);
    else {
      buf_put(&d->text, put, len);
      for (usize k = 0; k < len; k++) buf_putc(&d->fmt, (char)f);
    }
    i += skip;
  }
}

// the value of attribute `name` in the tag from `p` to `end`, its bytes
// unescaped for the doc's links, nullptr when it has none.
static char *attribute(const char *p, const char *end, const char *name) {
  usize nl = strlen(name);
  for (const char *q = p; q + nl + 2 < end; q++) {
    if (!memcmp(q, name, nl) && q[nl] == '=' && (q[-1] == ' ' || q[-1] == '\t') &&
        (q[nl + 1] == '"' || q[nl + 1] == '\'')) {
      char quote = q[nl + 1];
      const char *from = q + nl + 2, *to = memchr(from, quote, end - from);
      if (!to) return nullptr;
      civue_bytes b = {};
      for (const char *x = from; x < to;) {
        static const struct {
          const char *from, *to;
        } entities[] = {{"&amp;", "&"}, {"&quot;", "\""}, {"&#39;", "'"}, {"&lt;", "<"}, {"&gt;", ">"}};
        bool found = false;
        for (u32 k = 0; *x == '&' && k < countof(entities) && !found; k++) {
          usize el = strlen(entities[k].from);
          if (x + el <= to && !memcmp(x, entities[k].from, el)) {
            buf_puts(&b, entities[k].to);
            x += el;
            found = true;
          }
        }
        if (!found) buf_putc(&b, *x++);
      }
      return buf_take(&b);
    }
  }
  return nullptr;
}

civue_rich_doc civue_rich_from_html(const char *html) {
  civue_rich_doc d = {};
  i32 bold = 0, italic = 0, code = 0, strike = 0;
  u8 link = 0;
  bool started = false, unclosed = false;
  civue_rich_block kind = CIVUE_RICH_PARA, list = CIVUE_RICH_ORDERED;
  const char *p = html ? html : "";
  while (*p) {
    const char *end = *p == '<' && !unclosed ? strchr(p, '>') : nullptr;
    // no more tags after a bracket that never closes, found once.
    if (*p == '<' && !end) unclosed = true;
    if (end) {
      bool closing = p[1] == '/';
      const char *name = p + 1 + closing;
      usize nl = strcspn(name, " />\t\n");
      char tag[16] = {};
      for (usize k = 0; k < nl && k < sizeof tag - 1; k++) tag[k] = (char)(name[k] | 32);
      i32 step = closing ? -1 : 1;
      if (!strcmp(tag, "b") || !strcmp(tag, "strong"))
        bold += step;
      else if (!strcmp(tag, "i") || !strcmp(tag, "em"))
        italic += step;
      else if (!strcmp(tag, "code"))
        code += step;
      else if (!strcmp(tag, "s") || !strcmp(tag, "strike") || !strcmp(tag, "del"))
        strike += step;
      else if (!strcmp(tag, "a")) {
        link = 0;
        char *href = closing ? nullptr : attribute(p, end, "href");
        if (href) link = link_of(&d, href, strlen(href));
        free(href);
      } else if (!closing && (!strcmp(tag, "p") || !strcmp(tag, "div") || !strcmp(tag, "h1") || !strcmp(tag, "h2") ||
                              !strcmp(tag, "h3") || !strcmp(tag, "blockquote") || !strcmp(tag, "pre"))) {
        kind = !strcmp(tag, "h1") ? CIVUE_RICH_H1 : !strcmp(tag, "pre") ? CIVUE_RICH_CODE_BLOCK : CIVUE_RICH_PARA;
        open_block(&d, kind, &started);
      } else if (closing && (!strcmp(tag, "h1") || !strcmp(tag, "pre") || !strcmp(tag, "ol") || !strcmp(tag, "ul")))
        kind = CIVUE_RICH_PARA;
      else if (!closing && !strcmp(tag, "li")) {
        // quill's data-list says which, else the list around it does.
        bool bullet = memmem(p, end - p, "data-list=\"bullet\"", 18) || memmem(p, end - p, "data-list=\"check\"", 17);
        bool ordered = memmem(p, end - p, "data-list=\"ordered\"", 19);
        kind = bullet ? CIVUE_RICH_BULLET : ordered ? CIVUE_RICH_ORDERED : list;
        open_block(&d, kind, &started);
      } else if (!closing && (!strcmp(tag, "ul") || !strcmp(tag, "ol")))
        kind = list = !strcmp(tag, "ul") ? CIVUE_RICH_BULLET : CIVUE_RICH_ORDERED;
      else if (!strcmp(tag, "br")) {
        // an empty block's br holds its place, a later one breaks the line.
        usize last = d.text.len;
        while (last && d.text.v[last - 1] != '\n') last--;
        if (!started || d.text.len > last) open_block(&d, kind, &started);
      }
      p = end + 1;
      continue;
    }
    usize n = strcspn(p, "<");
    if (!n) n = 1;
    u8 f = (bold > 0 ? CIVUE_RICH_BOLD : 0) | (italic > 0 ? CIVUE_RICH_ITALIC : 0) | (code > 0 ? CIVUE_RICH_CODE : 0) |
           (strike > 0 ? CIVUE_RICH_STRIKE : 0) | (u8)(link << CIVUE_RICH_LINK_SHIFT);
    unescape_into(&d, p, n, f, kind, &started);
    p += n;
  }
  if (!d.blocks.len) vec_push(&d.blocks, CIVUE_RICH_PARA);
  return d;
}

void civue_rich_free(civue_rich_doc *d) {
  vec_free(&d->text);
  vec_free(&d->fmt);
  vec_free(&d->blocks);
  vec_each(&d->links, l) free(*l);
  vec_free(&d->links);
}

static civue_text_style style_of(civue_text_style s, u8 f) {
  if (f & CIVUE_RICH_BOLD) s.weight = 600;
  if (f & CIVUE_RICH_CODE) s.mono = true;
  return s;
}

// what a block's kind sets: a header is large and bold, a code block mono.
static civue_text_style block_style(civue_text_style s, civue_rich_block kind) {
  if (kind == CIVUE_RICH_H1) {
    s.size = roundf(s.size * 1.6f);
    s.line_height = roundf(s.line_height * 1.5f);
    s.weight = 650;
  } else if (kind == CIVUE_RICH_CODE_BLOCK)
    s.mono = true;
  return s;
}

// how far list items stand in from paragraphs, figma's, and the gray of
// inline code with the room on each side of its text.
constexpr f32 INDENT = 20;
constexpr f32 CODE_PAD = 4;
// the room a code block's gray takes around its lines.
constexpr f32 BLOCK_PAD = 4;
void civue_rich_lay_free(civue_rich_lay *l) {
  vec_free(&l->pieces);
  vec_free(&l->boxes);
}

// lays the doc out word by word, wrapping at `width`.
void civue_rich_layout(civue_ui *ui, const civue_rich_doc *d, civue_text_style style, f32 width, civue_rich_lay *out) {
  *out = (civue_rich_lay){.line_height = style.line_height, .width = width};
  const char *t = (const char *)d->text.v;
  usize n = d->text.len, at = 0;
  f32 y = 0;
  for (u32 b = 0; b < d->blocks.len; b++) {
    usize end = at;
    while (end < n && t[end] != '\n') end++;
    civue_rich_block kind = d->blocks.v[b];
    civue_text_style bs = block_style(style, kind);
    bool code_block = kind == CIVUE_RICH_CODE_BLOCK;
    f32 indent = kind == CIVUE_RICH_BULLET || kind == CIVUE_RICH_ORDERED ? INDENT : 0;
    // a code block's gray reaches past its lines by `BLOCK_PAD`, taking a
    // line of room above and below at its ends.
    bool first = code_block && (b == 0 || d->blocks.v[b - 1] != kind);
    bool last = code_block && (b + 1 == d->blocks.len || d->blocks.v[b + 1] != kind);
    if (first) y += BLOCK_PAD;
    f32 top = y, line_y = y, x = indent;
    // runs of one format, broken after spaces so lines wrap at words.
    usize i = at;
    while (i < end) {
      usize j = i;
      u8 f = d->fmt.v[i];
      while (j < end && d->fmt.v[j] == f && t[j] != ' ') j++;
      while (j < end && d->fmt.v[j] == f && t[j] == ' ') j++;
      if (j == i) j++;
      // the word, then its trailing spaces, which layouts leave out of
      // their width.
      civue_text_style ws = style_of(bs, f);
      usize k = j;
      while (k > i && t[k - 1] == ' ') k--;
      const civue_text_block *w = k > i ? civue_layout_n(ui, t + i, k - i, ws, 0) : nullptr;
      f32 ink = w ? w->width : 0;
      f32 space = civue_layout(ui, "x x", ws, 0)->width - civue_layout(ui, "xx", ws, 0)->width;
      bool code = (f & CIVUE_RICH_CODE) && !code_block;
      bool open = code && (i == at || !(d->fmt.v[i - 1] & CIVUE_RICH_CODE));
      bool close = code && (j == end || !(d->fmt.v[j] & CIVUE_RICH_CODE));
      f32 pad_l = open ? CODE_PAD : 0, pad_r = close ? CODE_PAD : 0;
      if (x > indent && x + pad_l + ink > width) {
        x = indent;
        line_y += bs.line_height;
      }
      civue_rich_piece piece = {.start = (u32)i,
                                .end = (u32)j,
                                .ink = (u32)k,
                                .block = b,
                                .x = x + pad_l,
                                .y = line_y,
                                .ink_w = ink,
                                .space = space,
                                .x0 = x,
                                .fmt = f,
                                .code_open = open,
                                .code_close = close,
                                .text = w};
      x += pad_l + ink + space * (f32)(j - k) + pad_r;
      piece.x1 = x;
      vec_push(&out->pieces, piece);
      out->widest = fmaxf(out->widest, piece.x + ink);
      i = j;
    }
    y = line_y + bs.line_height;
    if (last) y += BLOCK_PAD;
    vec_push(&out->boxes, ((civue_rich_box){top, y - top, indent, (u32)at, (u32)end, bs.line_height, kind}));
    at = end + 1;
  }
  out->height = y;
}

void civue_rich_paint(civue_ui *ui, const civue_rich_doc *d, const civue_rich_lay *l, civue_vec2 origin,
                      civue_text_style style, civue_rich_colors colors) {
  civue_color c = colors.text;
  u32 number = 0;
  for (u32 b = 0; b < d->blocks.len; b++) {
    const civue_rich_box *box = &l->boxes.v[b];
    civue_rich_block kind = d->blocks.v[b];
    number = kind == CIVUE_RICH_ORDERED ? number + 1 : 0;
    f32 top = (f32)origin.y + box->y;
    if (kind == CIVUE_RICH_BULLET) {
      // css's disc, a third of the font size across.
      f32 dia = style.size / 3, cy = top + style.line_height / 2 + .5f;
      civue_fill(ui, rect_xywh(origin.x + 7 - dia / 2, cy - dia / 2, dia, dia), c, dia / 2);
    } else if (kind == CIVUE_RICH_ORDERED) {
      char marker[16];
      snprintf(marker, sizeof marker, "%u.", number);
      f32 w = civue_layout(ui, marker, style, 0)->width;
      civue_text(ui, marker, style, v2(origin.x + 16.5f - w, top), c);
    } else if (kind == CIVUE_RICH_CODE_BLOCK && (b == 0 || d->blocks.v[b - 1] != kind)) {
      // one gray for the lines of a code block.
      u32 q = b;
      while (q + 1 < d->blocks.len && d->blocks.v[q + 1] == kind) q++;
      f32 bottom = (f32)origin.y + l->boxes.v[q].y + l->boxes.v[q].height;
      civue_fill(ui, rect(origin.x - CODE_PAD, top, origin.x + l->width + CODE_PAD, bottom), colors.code, 3);
    }
  }
  // inline code sits on a light gray, a little wider than its text, one
  // ground for the words of a run on a line.
  for (u32 p = 0; p < l->pieces.len; p++) {
    const civue_rich_piece *a = &l->pieces.v[p];
    if (!(a->fmt & CIVUE_RICH_CODE) || d->blocks.v[a->block] == CIVUE_RICH_CODE_BLOCK) continue;
    f32 x1 = a->x1;
    u32 q = p;
    while (q + 1 < l->pieces.len && !l->pieces.v[q].code_close && l->pieces.v[q + 1].fmt & CIVUE_RICH_CODE &&
           l->pieces.v[q + 1].y == a->y)
      x1 = l->pieces.v[++q].x1;
    f32 h = style.size + 4, cy = (f32)origin.y + a->y + style.line_height / 2;
    civue_fill(ui, rect(origin.x + a->x0, cy - h / 2, origin.x + x1, cy + h / 2), colors.code, 4);
    p = q;
  }
  for (u32 p = 0; p < l->pieces.len; p++) {
    const civue_rich_piece *a = &l->pieces.v[p];
    bool link = a->fmt & CIVUE_RICH_LINK_MASK;
    civue_color ink = link ? colors.link : c;
    if (a->text) civue_draw_text(ui, a->text, v2(origin.x + a->x, origin.y + a->y), ink);
    if (!a->ink_w) continue;
    const civue_rich_box *box = &l->boxes.v[a->block];
    // a link is underlined, struck text has a line through its middle.
    f32 x0 = origin.x + a->x, x1 = x0 + a->ink_w;
    f32 mid = roundf((f32)origin.y + a->y + box->line / 2);
    if (a->fmt & CIVUE_RICH_STRIKE) civue_fill(ui, rect(x0, mid, x1, mid + 1), ink, 0);
    if (link) civue_fill(ui, rect(x0, mid + style.size / 2, x1, mid + style.size / 2 + 1), ink, 0);
  }
}

civue_vec2 civue_rich_measure(civue_ui *ui, const civue_rich_doc *d, civue_text_style style, f32 width) {
  civue_rich_lay l;
  civue_rich_layout(ui, d, style, width, &l);
  civue_vec2 size = v2(l.widest, l.height);
  civue_rich_lay_free(&l);
  return size;
}

f32 civue_rich_draw(civue_ui *ui, const civue_rich_doc *d, civue_vec2 origin, civue_text_style style, f32 width,
                    civue_rich_colors colors) {
  civue_rich_lay l;
  civue_rich_layout(ui, d, style, width, &l);
  civue_rich_paint(ui, d, &l, origin, style, colors);
  f32 height = l.height;
  civue_rich_lay_free(&l);
  return height;
}
