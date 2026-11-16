#include "internal.h"

#include <civue/layout.h>

constexpr u32 NIL = UINT32_MAX;
constexpr f32 NONE = NAN;
// an available width that wraps text at every break, for min-content sizes.
constexpr f32 TINY = 1e-3f;
// how far a solved rect may move from the painted one and still settle.
constexpr f32 SETTLED = .01f;

enum {
  // lays the children out too, rather than only measuring.
  PERFORM = 1,
  // a block in a block, whose first and last children's margins may pass
  // through its edges.
  COLLAPSE = 2,
};

typedef struct {
  f32 w, h;
  // the first baseline from the top, NONE for none.
  f32 base;
  // the margins of its children that passed through its top and bottom.
  f32 mt, mb;
} Size;

typedef struct {
  f32 t, r, b, l;
} Edges;

typedef struct {
  civue_node_layout l;
  civue_id id;
  u32 parent, first, last, next;
  u32 text_at, text_len;
  civue_text_style ts;
  bool is_text, nowrap, truncate;
  // a length of its own is a fraction of what holds it.
  bool relative;
  // its text laid out on one line, and wrapped to `wrap_width`, this frame.
  const civue_text_block *whole, *wrapped;
  f32 wrap_width;
  // out of view, where it settles without the frame running again.
  bool quiet;
  // where it was painted, relative to its parent, and the content scrolled.
  f32 ux, uy, uw, uh, ucw, uch;
  f32 x, y, w, h, cw, ch;
  u8 ncache, next_cache;
  f32 keys[2][7];
  Size sizes[2];
  // how it was last laid out, and whether that still stands: a node a memo
  // put back is not laid out again when asked the same.
  bool solved, kept;
  f32 solved_key[7];
  Size solved_size;
} Node;

typedef struct {
  civue_id id;
  f32 x, y, w, h, cw, ch;
} Placed;

typedef struct {
  u32 node, children;
  civue_rect rect;
  // where its children's positions start, scrolled.
  civue_vec2 origin;
  civue_text_style ts;
  // the line height as a fraction of the size, 0 when set in points.
  f32 lh_rel;
  civue_color color;
  civue_text_align align;
  bool nowrap, truncate, clip, scroll, overlay, quiet;
} Open;

typedef struct {
  u32 node;
  // main then cross margins, NONE where auto.
  f32 m0, m1, c0, c1;
  f32 basis, hyp, min, max, main, cross, cmin, cmax;
  f32 factor, base, pos;
  u32 row, col, rspan, cspan;
  bool frozen, stretch, auto_min;
  civue_place align;
} Item;

typedef struct {
  u32 from, to;
  f32 cross, base;
} Line;

// a node as its parent sizes it.
typedef struct {
  const civue_node_layout *s;
  Edges pad, bor;
  f32 ph, pv;
  // the border box, NONE where it follows the content.
  f32 w, h;
  f32 minw, maxw, minh, maxh;
  // the room inside it.
  f32 aiw, aih;
  u32 flags;
} Box;

// what a memo's block built, as its frame solved it.
typedef struct {
  civue_id id;
  u64 inputs, around;
  // the frame that last asked for it.
  u64 used;
  bool valid;
  // an inner clip was cut by the one around: it only goes back where it was.
  bool exact;
  // the first of its nodes and of their text when recorded.
  u32 start, text_start;
  Vec(Node) nodes;
  civue_bytes text;
  Vec(civue_quad) quads;
  Vec(civue_region) regions;
  // where its parent's children started, in points, and the clip and what
  // it drew, in pixels.
  civue_vec2 origin;
  f32 clip[4], box[4];
} Memo;

// a memo being recorded.
typedef struct {
  civue_id id;
  u32 node, node_end, text, text_end, quad, region, batch, other;
  civue_texture texture;
  bool overlay, multiply, kept_hidden, repaint, focus_alive, failed;
  f64 repaint_at;
  u32 scroll_areas, cuts, open;
} Take;

struct civue_tree {
  Vec(Node) nodes;
  Vec(Placed) prev;
  civue_map index;
  bool indexed;
  Vec(Open) open;
  Vec(Item) items;
  Vec(Line) lines;
  Vec(f32) tracks;
  civue_bytes cells;
  civue_bytes text;
  u32 roots, first_root, last_root;
  f32 rem;
  bool dirty;
  // where the next node goes when the last frame had none of it.
  bool has_hint, hint_quiet;
  f32 hint[4];
  Vec(Memo) memos;
  civue_map memo_index;
  // the memos being recorded, then the ones the frame's solve confirms.
  Vec(Take) takes, taken;
};

static civue_tree *tree(civue_ui *ui) {
  if (!ui->tree) {
    ui->tree = own_new(sizeof *ui->tree);
    ui->tree->rem = 16;
    ui->tree->first_root = ui->tree->last_root = NIL;
  }

  return ui->tree;
}

static void memo_free(Memo *m) {
  vec_free(&m->nodes);
  vec_free(&m->text);
  vec_free(&m->quads);
  vec_free(&m->regions);
}

void civue__tree_free(civue_tree *t) {
  if (!t) {
    return;
  }

  vec_free(&t->nodes);
  vec_free(&t->prev);
  map_free(&t->index);
  vec_free(&t->open);
  vec_free(&t->items);
  vec_free(&t->lines);
  vec_free(&t->tracks);
  vec_free(&t->cells);
  vec_free(&t->text);
  vec_each(&t->memos, m) {
    memo_free(m);
  }
  vec_free(&t->memos);
  map_free(&t->memo_index);
  vec_free(&t->takes);
  vec_free(&t->taken);
  own_free(t);
}

void civue_set_rem_size(civue_ui *ui, f32 size) {
  tree(ui)->rem = size;
}

// ---- lengths ----

static f32 resolve(const civue_tree *t, civue_length l, f32 base) {
  switch (l.unit) {
    case CIVUE_UNIT_PX:
      return l.value;
    case CIVUE_UNIT_REM:
      return l.value * t->rem;
    case CIVUE_UNIT_RELATIVE:
      return isnan(base) ? NONE : l.value * base;
    default:
      return NONE;
  }
}

static f32 resolve0(const civue_tree *t, civue_length l, f32 base) {
  f32 v = resolve(t, l, base);
  return isnan(v) ? 0 : v;
}

// the minimum wins over the maximum, as in css.
static f32 clampf(f32 v, f32 lo, f32 hi) {
  return fmaxf(fminf(v, hi), lo);
}

static f32 or0(f32 v) {
  return isnan(v) ? 0 : v;
}

static Edges edges(const civue_tree *t, const civue_length e[4], f32 base) {
  return (Edges){resolve0(t, e[0], base), resolve0(t, e[1], base),
                 resolve0(t, e[2], base), resolve0(t, e[3], base)};
}

// margins with auto ones as NONE.
static Edges margins(const civue_tree *t, const civue_length e[4], f32 base) {
  Edges m = edges(t, e, base);
  if (e[0].unit == CIVUE_UNIT_AUTO) {
    m.t = NONE;
  }
  if (e[1].unit == CIVUE_UNIT_AUTO) {
    m.r = NONE;
  }
  if (e[2].unit == CIVUE_UNIT_AUTO) {
    m.b = NONE;
  }
  if (e[3].unit == CIVUE_UNIT_AUTO) {
    m.l = NONE;
  }
  return m;
}

static Edges border_of(const civue_node_layout *l) {
  return (Edges){l->border_w[0], l->border_w[1], l->border_w[2],
                 l->border_w[3]};
}

// two margins meeting, as css collapses them.
static f32 collapse(f32 a, f32 b) {
  if (a >= 0 && b >= 0) {
    return fmaxf(a, b);
  }

  if (a <= 0 && b <= 0) {
    return fminf(a, b);
  }

  return a + b;
}

static bool clips(const civue_node_layout *s) {
  return s->overflow_x || s->overflow_y;
}

static bool auto_height(const civue_node_layout *s) {
  return (!s->h.unit || s->h.unit == CIVUE_UNIT_AUTO) && !s->aspect;
}

// ---- the previous frame ----

static const Placed *placed(civue_tree *t, u32 at, civue_id id) {
  if (at < t->prev.len && t->prev.v[at].id == id) {
    return &t->prev.v[at];
  }

  if (!t->indexed) {
    map_clear(&t->index);
    for (u32 i = 0; i < t->prev.len; i++) {
      map_put(&t->index, t->prev.v[i].id, i);
    }
    t->indexed = true;
  }

  u64 i;
  return map_get(&t->index, id, &i) ? &t->prev.v[i] : nullptr;
}

civue_vec2 civue_node_last_size(civue_ui *ui, civue_id id) {
  civue_tree *t = tree(ui);
  const Placed *p = placed(t, NIL, id);
  return p ? v2(p->w, p->h) : v2(0, 0);
}

// ---- building the tree ----

static Open *top(civue_tree *t) {
  if (!t->open.len) {
    die("civue: no node is open");
  }

  return &vec_last(&t->open);
}

static civue_rect inset(civue_rect r, Edges e) {
  return rect(r.x0 + e.l, r.y0 + e.t, r.x1 - e.r, r.y1 - e.b);
}

static void inherit(civue_ui *ui, civue_tree *t, Open *o, const Open *parent,
                    const civue_node_style *s) {
  if (parent) {
    o->ts = parent->ts;
    o->lh_rel = parent->lh_rel;
    o->color = parent->color;
    o->align = parent->align;
    o->nowrap = parent->nowrap;
    o->truncate = parent->truncate;
  }
  else {
    // gpui's: a rem, on a golden line.
    o->lh_rel = 1.618f;
    o->ts = (civue_text_style){
      .size = t->rem, .weight = 400, .line_height = t->rem * o->lh_rel};
    o->color = ui->dark ? CIVUE_WHITE : CIVUE_BLACK;
    o->align = CIVUE_TEXT_LEFT;
  }

  if (s->text_size.unit) {
    o->ts.size = resolve0(t, s->text_size, o->ts.size);
    if (o->lh_rel) {
      o->ts.line_height = o->ts.size * o->lh_rel;
    }
  }

  if (s->line_height.unit == CIVUE_UNIT_RELATIVE) {
    o->lh_rel = s->line_height.value;
    o->ts.line_height = o->ts.size * o->lh_rel;
  }
  else if (s->line_height.unit) {
    o->lh_rel = 0;
    o->ts.line_height = resolve0(t, s->line_height, 0);
  }

  if (s->font_weight > 0) {
    o->ts.weight = s->font_weight;
  }
  if (s->italic) {
    o->ts.italic = s->italic == CIVUE_ON;
  }
  if (s->mono) {
    o->ts.mono = s->mono == CIVUE_ON;
  }
  if (s->tnum) {
    o->ts.tnum = s->tnum == CIVUE_ON;
  }
  if (s->nowrap) {
    o->nowrap = s->nowrap == CIVUE_ON;
  }
  if (s->truncate) {
    o->truncate = s->truncate == CIVUE_ON;
  }
  if (s->text_align) {
    o->align = s->text_align;
  }
  if (s->has_text_color) {
    o->color = s->text_color;
  }
}

static void layer(civue_node_paint *to, const civue_node_paint *p) {
  if (p->has_bg) {
    to->bg = p->bg;
  }
  if (p->has_border_color) {
    to->border_color = p->border_color;
  }
  if (p->has_text_color) {
    to->text_color = p->text_color;
  }
  to->has_text_color |= p->has_text_color;
}

static bool lit(const civue_node_paint *p) {
  return p->has_bg || p->has_border_color || p->has_text_color;
}

typedef struct {
  civue_vec2 offset;
  f32 blur, spread;
  civue_color color;
} Shadow;

// gpui's shadows, their first layer.
static constexpr Shadow SHADOWS[] = {
  [CIVUE_SHADOW_SM] = {{0, 1}, 2, 0, {0, 0, 0, .05f}},
  [CIVUE_SHADOW_MD] = {{0, 4}, 6, -1, {0, 0, 0, .1f}},
  [CIVUE_SHADOW_LG] = {{0, 10}, 15, -3, {0, 0, 0, .1f}},
  [CIVUE_SHADOW_XL] = {{0, 20}, 25, -5, {0, 0, 0, .1f}},
  [CIVUE_SHADOW_2XL] = {{0, 25}, 50, -12, {0, 0, 0, .25f}},
};

static void paint(civue_ui *ui, civue_tree *t, const civue_node_style *s,
                  civue_rect r, Open *o) {
  civue_node_paint p = {.bg = s->bg, .border_color = s->border_color};

  if (lit(&s->hover) || lit(&s->active) || s->has_cursor) {
    // css's hover holds over what is above it too, so no region is taken.
    civue_id id = t->nodes.v[o->node].id;
    civue_vec2 m = ui->input.mouse;
    civue_rect seen = rect_intersect(r, civue_clip_rect(ui));
    bool over = ui->input.has_mouse && rect_contains(seen, m) &&
                (ui->on_overlay || !civue_under_overlay(ui, m)) &&
                (!ui->active || ui->active == id);

    if (over) {
      layer(&p, &s->hover);
      if (s->has_cursor) {
        civue_set_cursor(ui, s->cursor);
      }
    }

    if (ui->active == id) {
      layer(&p, &s->active);
    }

    if (p.has_text_color) {
      o->color = p.text_color;
    }
  }

  f32 w = (f32)rect_w(r), h = (f32)rect_h(r);
  f32 most = fminf(w, h) / 2, radii[4];
  for (u32 i = 0; i < 4; i++) {
    radii[i] = fminf(resolve0(t, s->radius[i], fminf(w, h)), most);
  }

  if (s->shadow) {
    const Shadow *sh = &SHADOWS[s->shadow];
    civue_rect at = rect_inflate(r, sh->spread, sh->spread);
    civue_shadow(ui, at, radii[0], sh->blur, sh->offset, sh->color);
  }

  if (p.bg.a > 0) {
    civue_fill_corners(ui, r, p.bg, radii);
  }

  const f32 *bw = s->border_w;
  if (p.border_color.a <= 0 || !(bw[0] || bw[1] || bw[2] || bw[3])) {
    return;
  }

  if (bw[0] == bw[1] && bw[1] == bw[2] && bw[2] == bw[3]) {
    civue_stroke_corners(ui, r, p.border_color, bw[0], radii);
  }
  else {
    civue_stroke_sides(ui, r, p.border_color, bw, radii);
  }
}

// whether any length of a node is a fraction of what holds it: one with
// none is laid out the same in any containing block.
static bool relative(const civue_node_layout *l) {
  const civue_length *sizes[] = {&l->w,     &l->h,     &l->min_w,
                                 &l->min_h, &l->max_w, &l->max_h,
                                 &l->basis, &l->gap_x, &l->gap_y};
  for (u32 i = 0; i < sizeof sizes / sizeof *sizes; i++) {
    if (sizes[i]->unit == CIVUE_UNIT_RELATIVE) {
      return true;
    }
  }

  for (u32 i = 0; i < 4; i++) {
    if (l->padding[i].unit == CIVUE_UNIT_RELATIVE ||
        l->inset[i].unit == CIVUE_UNIT_RELATIVE) {
      return true;
    }
  }

  return false;
}

// the node new ones go in: the one open, unless an overlay started since.
static Open *parent_of(civue_ui *ui, civue_tree *t) {
  return t->open.len && vec_last(&t->open).overlay == ui->on_overlay
           ? &vec_last(&t->open)
           : nullptr;
}

// who the next child of `parent` is when it has no key, a root when null.
static civue_id next_id(const civue_tree *t, const Open *parent) {
  if (parent) {
    return civue_idn(t->nodes.v[parent->node].id, parent->children);
  }

  return civue_idn(0x9e3779b97f4a7c15ull, t->roots);
}

// makes node `i` the last child of `parent`, or the last root.
static void link(civue_tree *t, Open *parent, u32 i) {
  Node *n = &t->nodes.v[i];
  n->next = NIL;

  if (parent) {
    Node *p = &t->nodes.v[parent->node];
    n->parent = parent->node;
    parent->children++;
    if (p->last == NIL) {
      p->first = i;
    }
    else {
      t->nodes.v[p->last].next = i;
    }
    p->last = i;
    return;
  }

  n->parent = NIL;
  t->roots++;
  if (t->last_root == NIL) {
    t->first_root = i;
  }
  else {
    t->nodes.v[t->last_root].next = i;
  }
  t->last_root = i;
}

static u32 add(civue_ui *ui, civue_tree *t, const civue_node_style *s,
               bool is_text, Open *o) {
  Open *parent = parent_of(ui, t);
  u32 i = t->nodes.len;
  Node *n = vec_add(&t->nodes);
  memcpy(&n->l, s, sizeof n->l);
  n->is_text = is_text;
  n->first = n->last = n->next = NIL;

  n->id = s->key ? s->key : next_id(t, parent);
  n->relative = relative(&n->l);
  link(t, parent, i);

  const Placed *was = placed(t, i, n->id);
  if (was) {
    n->ux = was->x, n->uy = was->y, n->uw = was->w, n->uh = was->h;
    n->ucw = was->cw, n->uch = was->ch;
  }
  else if (t->has_hint) {
    n->ux = t->hint[0], n->uy = t->hint[1];
    n->uw = t->hint[2], n->uh = t->hint[3];
  }

  n->quiet = (parent && parent->quiet) || (t->has_hint && t->hint_quiet);
  t->has_hint = false;

  civue_vec2 at = parent ? parent->origin : v2(0, 0);
  *o = (Open){.node = i, .overlay = ui->on_overlay, .quiet = n->quiet};
  o->rect = rect_xywh(at.x + n->ux, at.y + n->uy, n->uw, n->uh);
  o->origin = v2(o->rect.x0, o->rect.y0);
  inherit(ui, t, o, parent, s);
  paint(ui, t, s, o->rect, o);
  return i;
}

void civue_node_open(civue_ui *ui, const civue_node_style *s) {
  civue_tree *t = tree(ui);
  Open o;
  u32 i = add(ui, t, s, false, &o);
  Node *n = &t->nodes.v[i];

  bool scroll_x = s->overflow_x == CIVUE_OVERFLOW_SCROLL;
  bool scroll_y = s->overflow_y == CIVUE_OVERFLOW_SCROLL;
  civue_rect pad = inset(o.rect, border_of(&n->l));

  if (scroll_x || scroll_y) {
    f32 cw = scroll_x ? n->ucw : (f32)rect_w(pad);
    f32 ch = scroll_y ? n->uch : (f32)rect_h(pad);
    civue_rect c =
      civue_scroll_area_look(ui, n->id, pad, cw, ch, s->scrollbars);
    o.origin = v2(o.rect.x0 + c.x0 - pad.x0, o.rect.y0 + c.y0 - pad.y0);
    o.scroll = true;
  }
  else if (clips(&n->l)) {
    civue_push_clip(ui, pad);
    o.clip = true;
  }

  vec_push(&t->open, o);
}

void civue_node_close(civue_ui *ui) {
  civue_tree *t = tree(ui);
  Open o = *top(t);
  t->open.len--;

  if (o.scroll) {
    civue_scroll_end(ui);
  }
  else if (o.clip) {
    civue_pop_clip(ui);
  }
}

// a text node's text on one line, asked once a frame.
static const civue_text_block *whole(civue_ui *ui, const civue_tree *t,
                                     Node *n) {
  if (!n->whole) {
    const char *s = (const char *)t->text.v + n->text_at;
    n->whole = civue_layout_n(ui, s, n->text_len, n->ts, 0);
  }

  return n->whole;
}

static const civue_text_block *shape(civue_ui *ui, const civue_tree *t, Node *n,
                                     f32 width) {
  const civue_text_block *full = whole(ui, t, n);
  if (n->nowrap || n->truncate || full->width <= width + SETTLED) {
    return full;
  }

  f32 w = fmaxf(width, TINY);
  if (!n->wrapped || n->wrap_width != w) {
    const char *s = (const char *)t->text.v + n->text_at;
    n->wrapped = civue_layout_n(ui, s, n->text_len, n->ts, w);
    n->wrap_width = w;
  }

  return n->wrapped;
}

// each line of a wrapped block moved within `w` on its own, as css aligns
// lines rather than paragraphs.
static void draw_lines(civue_ui *ui, const civue_text_block *b, const Node *n,
                       const char *s, civue_rect box, civue_text_align align,
                       civue_color c) {
  f32 w = (f32)rect_w(box);
  f32 k = align == CIVUE_TEXT_CENTER ? .5f : 1;

  for (u32 l = 0; l < b->nlines; l++) {
    const civue_text_line *line = &b->lines[l];
    u32 end = line->end;
    while (end > line->start && (s[end - 1] == ' ' || s[end - 1] == '\n')) {
      end--;
    }

    const civue_text_block *one =
      civue_layout_n(ui, s + line->start, end - line->start, n->ts, 0);
    civue_vec2 at = v2(box.x0 + (w - one->width) * k, box.y0 + line->top);
    civue_draw_text(ui, one, at, c);
  }
}

void civue_node_text_n(civue_ui *ui, const char *s, usize len,
                       const civue_node_style *style) {
  civue_tree *t = tree(ui);
  Open o;
  u32 i = add(ui, t, style, true, &o);
  Node *n = &t->nodes.v[i];
  n->ts = o.ts;
  n->nowrap = o.nowrap;
  n->truncate = o.truncate;
  n->text_at = t->text.len;
  n->text_len = (u32)len;
  buf_put(&t->text, s, len);
  buf_putc(&t->text, 0);

  Edges pad = edges(t, n->l.padding, NONE);
  civue_rect box = inset(inset(o.rect, border_of(&n->l)), pad);
  f32 w = (f32)rect_w(box);
  const char *str = (const char *)t->text.v + n->text_at;
  const civue_text_block *b =
    n->truncate ? civue_fit(ui, str, n->ts, w) : shape(ui, t, n, w);

  if (o.align != CIVUE_TEXT_LEFT && b->nlines > 1 && !n->truncate) {
    draw_lines(ui, b, n, str, box, o.align, o.color);
    return;
  }

  f64 x = box.x0;
  if (o.align == CIVUE_TEXT_CENTER) {
    x += (w - b->width) / 2;
  }
  if (o.align == CIVUE_TEXT_RIGHT) {
    x += w - b->width;
  }
  civue_draw_text(ui, b, v2(x, box.y0), o.color);
}

void civue_node_icon(civue_ui *ui, civue_icon icon,
                     const civue_node_style *style) {
  civue_tree *t = tree(ui);
  Open o;
  u32 i = add(ui, t, style, false, &o);
  civue_node_layout *l = &t->nodes.v[i].l;

  if (!l->aspect) {
    l->aspect = civue_icon_aspect(ui->icons, icon);
  }

  bool w = l->w.unit && l->w.unit != CIVUE_UNIT_AUTO;
  bool h = l->h.unit && l->h.unit != CIVUE_UNIT_AUTO;
  if (!w && !h) {
    l->h = civue_px(o.ts.size);
  }

  Edges pad = edges(t, l->padding, NONE);
  civue_rect box = inset(inset(o.rect, border_of(l)), pad);
  if (!rect_empty(box)) {
    civue_icon_at(ui, icon, box, o.color);
  }
}

civue_rect civue_node_rect(civue_ui *ui) {
  return top(tree(ui))->rect;
}

civue_rect civue_node_content(civue_ui *ui) {
  civue_tree *t = tree(ui);
  const Open *o = top(t);
  const Node *n = &t->nodes.v[o->node];
  Edges pad = edges(t, n->l.padding, (f32)rect_w(o->rect));
  return inset(inset(o->rect, border_of(&n->l)), pad);
}

civue_id civue_node_id(civue_ui *ui) {
  civue_tree *t = tree(ui);
  return t->nodes.v[top(t)->node].id;
}

civue_text_style civue_node_text_style(civue_ui *ui) {
  return top(tree(ui))->ts;
}

civue_color civue_node_color(civue_ui *ui) {
  return top(tree(ui))->color;
}

// ---- solving ----

static Size compute(civue_ui *ui, civue_tree *t, u32 i, f32 kw, f32 kh, f32 aw,
                    f32 ah, f32 cbw, f32 cbh, u32 flags);

static void place(civue_tree *t, u32 i, f32 x, f32 y, f32 w, f32 h) {
  Node *n = &t->nodes.v[i];
  n->x = x, n->y = y, n->w = w, n->h = h;
  if (n->quiet) {
    return;
  }

  if (fabsf(x - n->ux) > SETTLED || fabsf(y - n->uy) > SETTLED ||
      fabsf(w - n->uw) > SETTLED || fabsf(h - n->uh) > SETTLED) {
    t->dirty = true;
  }
}

// a child's baseline at a size, its bottom edge when it has none.
static f32 baseline_at(civue_ui *ui, civue_tree *t, u32 c, f32 w, f32 h,
                       f32 cbw, f32 cbh) {
  f32 b = compute(ui, t, c, w, h, w, h, cbw, cbh, 0).base;
  return isnan(b) ? h : b;
}

// lays out an absolute node in a containing block at `ox`, `oy` of `cw` by
// `ch`, at `sx`, `sy` when no inset places it.
static void absolute(civue_ui *ui, civue_tree *t, u32 i, f32 ox, f32 oy, f32 cw,
                     f32 ch, f32 sx, f32 sy) {
  const civue_node_layout *s = &t->nodes.v[i].l;
  f32 top = resolve(t, s->inset[0], ch), right = resolve(t, s->inset[1], cw);
  f32 bottom = resolve(t, s->inset[2], ch), left = resolve(t, s->inset[3], cw);
  Edges m = edges(t, s->margin, cw);

  f32 kw = resolve(t, s->w, cw), kh = resolve(t, s->h, ch);
  if (isnan(kw) && !isnan(left) && !isnan(right)) {
    kw = cw - left - right - m.l - m.r;
  }
  if (isnan(kh) && !isnan(top) && !isnan(bottom)) {
    kh = ch - top - bottom - m.t - m.b;
  }

  f32 aw = cw - or0(left) - or0(right) - m.l - m.r;
  f32 ah = ch - or0(top) - or0(bottom) - m.t - m.b;
  Size z = compute(ui, t, i, kw, kh, aw, ah, cw, ch, PERFORM);

  f32 x = !isnan(left)    ? ox + left + m.l
          : !isnan(right) ? ox + cw - right - m.r - z.w
                          : sx + m.l;
  f32 y = !isnan(top)      ? oy + top + m.t
          : !isnan(bottom) ? oy + ch - bottom - m.b - z.h
                           : sy + m.t;
  place(t, i, x, y, z.w, z.h);
}

static Size measure_text(civue_ui *ui, civue_tree *t, Node *n, const Box *b,
                         f32 aw) {
  f32 avail = (isnan(b->w) ? fminf(aw, b->maxw) : b->w) - b->ph;
  const civue_text_block *full = whole(ui, t, n);
  f32 tw = full->width, th = full->height;

  if (n->truncate) {
    tw = fminf(tw, fmaxf(avail, 0));
  }
  else if (!n->nowrap && tw > avail + SETTLED) {
    const civue_text_block *wrapped = shape(ui, t, n, avail);
    tw = fmaxf(wrapped->width, avail);
    th = wrapped->height;
  }

  return (Size){
    isnan(b->w) ? clampf(tw + b->ph, b->minw, b->maxw) : b->w,
    isnan(b->h) ? clampf(th + b->pv, b->minh, b->maxh) : b->h,
    b->bor.t + b->pad.t + full->baseline,
    0,
    0,
  };
}

static bool cached(Node *n, const f32 key[7], Size *out) {
  for (u32 k = 0; k < n->ncache; k++) {
    if (!memcmp(n->keys[k], key, sizeof n->keys[k])) {
      *out = n->sizes[k];
      return true;
    }
  }

  return false;
}

static void remember(Node *n, const f32 key[7], Size z) {
  u32 k = n->next_cache;
  memcpy(n->keys[k], key, sizeof n->keys[k]);
  n->sizes[k] = z;
  n->next_cache = !k;
  if (n->ncache < 2) {
    n->ncache++;
  }
}

// ---- blocks ----

// children stacked from the top, their vertical margins collapsing between
// them and through the box's edges where nothing holds them apart.
static Size layout_block(civue_ui *ui, civue_tree *t, u32 i, const Box *b) {
  const civue_node_layout *s = b->s;
  f32 iw = isnan(b->w) ? NONE : b->w - b->ph;
  f32 ih = isnan(b->h) ? NONE : b->h - b->pv;
  f32 gap = resolve0(t, s->gap_y, ih);
  bool through = (b->flags & COLLAPSE) && !clips(s);
  bool through_top = through && b->pad.t + b->bor.t == 0;
  bool through_bottom = through && b->pad.b + b->bor.b == 0 && auto_height(s);

  u32 base = t->items.len;
  f32 widest = 0;
  for (u32 c = t->nodes.v[i].first; c != NIL; c = t->nodes.v[c].next) {
    const civue_node_layout *cs = &t->nodes.v[c].l;
    if (cs->position == CIVUE_ABSOLUTE) {
      continue;
    }

    Edges m = margins(t, cs->margin, iw);
    f32 side = or0(m.l) + or0(m.r);
    f32 cw = resolve(t, cs->w, iw);
    if (isnan(cw) && !isnan(iw)) {
      cw = iw - side;
    }

    f32 aw = (isnan(iw) ? b->aiw : iw) - side;
    Size z = compute(ui, t, c, cw, resolve(t, cs->h, ih), aw, INFINITY, iw, ih,
                     COLLAPSE);
    Item it = {.node = c, .c0 = m.l, .c1 = m.r};
    it.m0 = collapse(or0(m.t), z.mt);
    it.m1 = collapse(or0(m.b), z.mb);
    it.cross = z.w;
    it.main = z.h;
    it.base = z.base;
    vec_push(&t->items, it);
    widest = fmaxf(widest, z.w + side);
  }

  u32 count = t->items.len - base;
  f32 y = 0, pending = 0, top_out = 0, bottom_out = 0;
  for (u32 k = 0; k < count; k++) {
    Item *it = &t->items.v[base + k];
    if (k == 0 && through_top) {
      top_out = it->m0;
    }
    else if (k == 0) {
      y = it->m0;
    }
    else {
      y += collapse(pending, it->m0) + gap;
    }

    it->pos = y;
    y += it->main;
    pending = it->m1;
  }

  if (count && through_bottom) {
    bottom_out = pending;
  }
  else if (count) {
    y += pending;
  }

  f32 w = isnan(b->w) ? clampf(widest + b->ph, b->minw, b->maxw) : b->w;
  f32 h = isnan(b->h) ? clampf(y + b->pv, b->minh, b->maxh) : b->h;
  Size out = {w, h, NONE, top_out, bottom_out};
  for (u32 k = 0; k < count; k++) {
    const Item *it = &t->items.v[base + k];
    if (!isnan(it->base)) {
      out.base = b->bor.t + b->pad.t + it->pos + it->base;
      break;
    }
  }

  if (b->flags & PERFORM) {
    iw = w - b->ph;
    ih = h - b->pv;
    for (u32 k = 0; k < count; k++) {
      Item it = t->items.v[base + k];
      f32 x = or0(it.c0);
      if (isnan(it.c0) && isnan(it.c1)) {
        x = (iw - it.cross) / 2;
      }
      else if (isnan(it.c0)) {
        x = iw - it.cross - or0(it.c1);
      }

      compute(ui, t, it.node, it.cross, it.main, it.cross, it.main, iw, ih,
              PERFORM | COLLAPSE);
      place(t, it.node, b->bor.l + b->pad.l + x, b->bor.t + b->pad.t + it.pos,
            it.cross, it.main);
    }
  }

  t->items.len = base;
  return out;
}

// ---- flexbox ----

// grows or shrinks the items of a line to fill `target`, css's resolving of
// flexible lengths.
static void flexible(civue_ui *ui, civue_tree *t, u32 from, u32 to, f32 target,
                     f32 gaps, bool row, f32 aic, f32 iw, f32 ih) {
  Item *v = t->items.v;
  f32 used = gaps;
  for (u32 k = from; k < to; k++) {
    used += v[k].hyp + or0(v[k].m0) + or0(v[k].m1);
  }

  bool grow = target - used > 0;
  for (u32 k = from; k < to; k++) {
    const civue_node_layout *s = &t->nodes.v[v[k].node].l;
    v[k].factor = grow ? s->grow : s->has_shrink ? s->shrink : 1;
    v[k].main = v[k].hyp;
    v[k].frozen = v[k].factor == 0 || (grow && v[k].basis > v[k].hyp) ||
                  (!grow && v[k].basis < v[k].hyp);
  }

  // what an item holds at least when nothing says, its min-content size,
  // which only matters to items that shrink.
  if (used > target) {
    for (u32 k = from; k < to; k++) {
      if (v[k].frozen || !v[k].auto_min) {
        continue;
      }

      u32 c = v[k].node;
      Size z = row ? compute(ui, t, c, NONE, NONE, TINY, aic, iw, ih, 0)
                   : compute(ui, t, c, NONE, NONE, aic, INFINITY, iw, ih, 0);
      v = t->items.v;
      const civue_node_layout *s = &t->nodes.v[c].l;
      f32 least = row ? z.w : z.h;
      f32 set = resolve(t, row ? s->w : s->h, row ? iw : ih);
      if (!isnan(set)) {
        least = fminf(least, set);
      }
      v[k].min = fmaxf(v[k].min, fminf(least, v[k].hyp));
    }
  }

  for (u32 round = 0; round <= to - from; round++) {
    f32 free = target - gaps, factors = 0, scaled = 0;
    for (u32 k = from; k < to; k++) {
      free -= or0(v[k].m0) + or0(v[k].m1);
      free -= v[k].frozen ? v[k].main : v[k].basis;
      if (!v[k].frozen) {
        factors += v[k].factor;
        scaled += v[k].factor * v[k].basis;
      }
    }

    if (factors == 0) {
      break;
    }

    if (grow && factors < 1) {
      free *= factors;
    }

    f32 violation = 0;
    for (u32 k = from; k < to; k++) {
      if (v[k].frozen) {
        continue;
      }

      f32 share = grow         ? free * v[k].factor / factors
                  : scaled > 0 ? free * v[k].factor * v[k].basis / scaled
                               : 0;
      f32 want = v[k].basis + share;
      v[k].main = clampf(want, v[k].min, v[k].max);
      violation += v[k].main - want;
    }

    bool done = true;
    for (u32 k = from; k < to; k++) {
      if (v[k].frozen) {
        continue;
      }

      bool low = v[k].main == v[k].min && violation > 0;
      bool high = v[k].main == v[k].max && violation < 0;
      if (violation == 0 || low || high) {
        v[k].frozen = true;
      }
      else {
        done = false;
      }
    }

    if (done) {
      break;
    }
  }
}

static f32 lead(civue_place p, f32 free, u32 count, f32 *between) {
  *between = 0;
  if (!count || (free < 0 && p >= CIVUE_PLACE_BETWEEN)) {
    return 0;
  }

  switch (p) {
    case CIVUE_PLACE_END:
      return free;
    case CIVUE_PLACE_CENTER:
      return free / 2;
    case CIVUE_PLACE_BETWEEN:
      *between = count > 1 ? free / (count - 1) : 0;
      return 0;
    case CIVUE_PLACE_AROUND:
      *between = free / count;
      return *between / 2;
    case CIVUE_PLACE_EVENLY:
      *between = free / (count + 1);
      return *between;
    default:
      return 0;
  }
}

// where an item sits across its line or cell of `room`.
static f32 across(const Item *it, f32 room, f32 size, f32 line_base) {
  if (isnan(it->c0) && isnan(it->c1)) {
    return (room - size) / 2;
  }

  if (isnan(it->c0)) {
    return room - size - it->c1;
  }

  if (isnan(it->c1)) {
    return it->c0;
  }

  switch (it->align) {
    case CIVUE_PLACE_END:
      return room - size - it->c1;
    case CIVUE_PLACE_CENTER:
      return it->c0 + (room - size - it->c0 - it->c1) / 2;
    case CIVUE_PLACE_BASELINE:
      return isnan(line_base) ? it->c0 : line_base - it->base;
    default:
      return it->c0;
  }
}

static Size layout_flex(civue_ui *ui, civue_tree *t, u32 i, const Box *b) {
  const civue_node_layout *s = b->s;
  civue_direction dir = s->direction;
  bool row = dir == CIVUE_ROW || dir == CIVUE_ROW_REVERSE;
  bool rev = dir == CIVUE_ROW_REVERSE || dir == CIVUE_COL_REVERSE;

  f32 iw = isnan(b->w) ? NONE : b->w - b->ph;
  f32 ih = isnan(b->h) ? NONE : b->h - b->pv;
  f32 im = row ? iw : ih, ic = row ? ih : iw;
  f32 aim = row ? b->aiw : b->aih, aic = row ? b->aih : b->aiw;
  f32 gm = resolve0(t, row ? s->gap_x : s->gap_y, im);
  f32 gc = resolve0(t, row ? s->gap_y : s->gap_x, ic);
  civue_place items = s->align_items ? s->align_items : CIVUE_PLACE_STRETCH;

  u32 base = t->items.len, lbase = t->lines.len;
  for (u32 c = t->nodes.v[i].first; c != NIL; c = t->nodes.v[c].next) {
    const civue_node_layout *cs = &t->nodes.v[c].l;
    if (cs->position == CIVUE_ABSOLUTE) {
      continue;
    }

    Item it = {.node = c, .max = INFINITY, .base = NONE};
    Edges m = margins(t, cs->margin, iw);
    it.m0 = row ? m.l : m.t;
    it.m1 = row ? m.r : m.b;
    it.c0 = row ? m.t : m.l;
    it.c1 = row ? m.b : m.r;
    it.align = cs->align_self ? cs->align_self : items;
    if (!row && it.align == CIVUE_PLACE_BASELINE) {
      it.align = CIVUE_PLACE_START;
    }

    f32 cross_set = resolve(t, row ? cs->h : cs->w, ic);
    it.stretch = it.align == CIVUE_PLACE_STRETCH && isnan(cross_set) &&
                 !isnan(it.c0) && !isnan(it.c1);
    it.min = resolve0(t, row ? cs->min_w : cs->min_h, im);
    f32 most = resolve(t, row ? cs->max_w : cs->max_h, im);
    if (!isnan(most)) {
      it.max = most;
    }
    it.cmin = resolve0(t, row ? cs->min_h : cs->min_w, ic);
    it.cmax = resolve(t, row ? cs->max_h : cs->max_w, ic);
    if (isnan(it.cmax)) {
      it.cmax = INFINITY;
    }

    civue_overflow over = row ? cs->overflow_x : cs->overflow_y;
    civue_length least = row ? cs->min_w : cs->min_h;
    it.auto_min = !least.unit && over == CIVUE_OVERFLOW_VISIBLE;

    f32 basis = resolve(t, cs->basis, im);
    if (isnan(basis)) {
      basis = resolve(t, row ? cs->w : cs->h, im);
    }
    if (isnan(basis)) {
      f32 kc = it.stretch && !isnan(ic) ? ic - it.c0 - it.c1 : cross_set;
      Size z = row
                 ? compute(ui, t, c, NONE, kc, INFINITY, b->aih, iw, ih, 0)
                 : compute(ui, t, c, kc, NONE, b->aiw - or0(it.c0) - or0(it.c1),
                           INFINITY, iw, ih, 0);
      basis = row ? z.w : z.h;
    }

    it.basis = basis;
    it.hyp = clampf(basis, it.min, it.max);
    vec_push(&t->items, it);
  }

  u32 count = t->items.len - base;
  f32 limit = !isnan(im) ? im : aim;
  f32 widest = 0;

  {
    Line line = {base, base, 0, NONE};
    f32 sum = 0;
    for (u32 k = base; k < base + count; k++) {
      const Item *it = &t->items.v[k];
      f32 outer = it->hyp + or0(it->m0) + or0(it->m1);
      if (s->wrap && k > line.from && sum + gm + outer > limit + SETTLED) {
        line.to = k;
        vec_push(&t->lines, line);
        widest = fmaxf(widest, sum);
        line = (Line){k, k, 0, NONE};
        sum = 0;
      }
      sum += (k > line.from ? gm : 0) + outer;
    }
    line.to = base + count;
    vec_push(&t->lines, line);
    widest = fmaxf(widest, sum);
  }

  u32 nlines = t->lines.len - lbase;
  f32 target = im;
  if (isnan(target)) {
    target = row ? fminf(widest, aim) : widest;
    f32 lo = row ? b->minw - b->ph : b->minh - b->pv;
    f32 hi = row ? b->maxw - b->ph : b->maxh - b->pv;
    target = clampf(target, lo, hi);
  }

  for (u32 l = lbase; l < lbase + nlines; l++) {
    Line line = t->lines.v[l];
    f32 kc = !isnan(ic) && nlines == 1 ? ic : NONE;
    f32 gaps = line.to > line.from ? gm * (line.to - line.from - 1) : 0;
    flexible(ui, t, line.from, line.to, target, gaps, row, aic, iw, ih);

    f32 cross = 0, above = NONE, below = 0;
    for (u32 k = line.from; k < line.to; k++) {
      Item it = t->items.v[k];
      const civue_node_layout *cs = &t->nodes.v[it.node].l;
      f32 set = resolve(t, row ? cs->h : cs->w, ic);
      f32 cr;
      if (!isnan(set)) {
        cr = clampf(set, it.cmin, it.cmax);
      }
      else if (it.stretch && !isnan(kc)) {
        cr = clampf(kc - it.c0 - it.c1, it.cmin, it.cmax);
      }
      else {
        f32 room = aic - or0(it.c0) - or0(it.c1);
        Size z =
          row
            ? compute(ui, t, it.node, it.main, NONE, it.main, room, iw, ih, 0)
            : compute(ui, t, it.node, NONE, it.main, room, it.main, iw, ih, 0);
        cr = row ? z.h : z.w;
      }

      if (it.align == CIVUE_PLACE_BASELINE) {
        it.base = baseline_at(ui, t, it.node, it.main, cr, iw, ih);
        f32 up = or0(it.c0) + it.base;
        above = isnan(above) ? up : fmaxf(above, up);
        below = fmaxf(below, cr - it.base + or0(it.c1));
      }
      else {
        cross = fmaxf(cross, cr + or0(it.c0) + or0(it.c1));
      }

      t->items.v[k].cross = cr;
      t->items.v[k].base = it.base;
    }

    if (!isnan(above)) {
      cross = fmaxf(cross, above + below);
    }
    t->lines.v[l].cross = cross;
    t->lines.v[l].base = above;
  }

  f32 content_cross = gc * (nlines - 1);
  for (u32 l = lbase; l < lbase + nlines; l++) {
    content_cross += t->lines.v[l].cross;
  }

  f32 w = b->w, h = b->h;
  if (isnan(w)) {
    w = clampf((row ? target : content_cross) + b->ph, b->minw, b->maxw);
  }
  if (isnan(h)) {
    h = clampf((row ? content_cross : target) + b->pv, b->minh, b->maxh);
  }

  iw = w - b->ph, ih = h - b->pv;
  im = row ? iw : ih, ic = row ? ih : iw;
  if (nlines == 1) {
    t->lines.v[lbase].cross = ic;
  }

  f32 cfree = ic - content_cross, cpos = 0, cbetween = 0;
  civue_place content =
    s->align_content ? s->align_content : CIVUE_PLACE_STRETCH;
  if (nlines > 1 && content == CIVUE_PLACE_STRETCH && cfree > 0) {
    for (u32 l = lbase; l < lbase + nlines; l++) {
      t->lines.v[l].cross += cfree / nlines;
    }
  }
  else if (nlines > 1) {
    cpos = lead(content, cfree, nlines, &cbetween);
  }

  f32 x0 = b->bor.l + b->pad.l, y0 = b->bor.t + b->pad.t;
  Size out = {w, h, NONE, 0, 0};

  for (u32 l = lbase; l < lbase + nlines; l++) {
    Line line = t->lines.v[l];
    u32 n = line.to - line.from;
    f32 free = im - (n ? gm * (n - 1) : 0);
    u32 autos = 0;
    for (u32 k = line.from; k < line.to; k++) {
      const Item *it = &t->items.v[k];
      free -= it->main + or0(it->m0) + or0(it->m1);
      autos += isnan(it->m0) + isnan(it->m1);
    }

    f32 each = autos && free > 0 ? free / autos : 0;
    f32 between = 0, pos = 0;
    if (!(autos && free > 0)) {
      pos = lead(s->justify, free, n, &between);
    }

    for (u32 k = line.from; k < line.to; k++) {
      Item it = t->items.v[k];
      pos += isnan(it.m0) ? each : it.m0;

      f32 cross = it.cross;
      if (it.stretch) {
        cross = clampf(line.cross - it.c0 - it.c1, it.cmin, it.cmax);
      }

      f32 at = across(&it, line.cross, cross, line.base);
      f32 main_at = rev ? im - pos - it.main : pos;
      f32 cw = row ? it.main : cross, ch = row ? cross : it.main;
      f32 x = x0 + (row ? main_at : cpos + at);
      f32 y = y0 + (row ? cpos + at : main_at);

      // the first item that has a baseline gives the box its own.
      if (isnan(out.base) && l == lbase) {
        f32 ib = isnan(it.base) ? baseline_at(ui, t, it.node, cw, ch, iw, ih)
                                : it.base;
        if (!row || it.align == CIVUE_PLACE_BASELINE || k == line.from) {
          out.base = y + ib;
        }
      }

      if (b->flags & PERFORM) {
        compute(ui, t, it.node, cw, ch, cw, ch, iw, ih, PERFORM);
        place(t, it.node, x, y, cw, ch);
      }

      pos += it.main + (isnan(it.m1) ? each : it.m1) + gm + between;
    }

    cpos += line.cross + gc + cbetween;
  }

  t->items.len = base;
  t->lines.len = lbase;
  return out;
}

// ---- grid ----

static bool taken(const civue_tree *t, u32 at, u32 ncols, u32 nrows, u32 r,
                  u32 c, u32 rs, u32 cs) {
  for (u32 y = r; y < r + rs; y++) {
    if (y >= nrows) {
      return false;
    }

    for (u32 x = c; x < c + cs; x++) {
      if (t->cells.v[at + y * ncols + x]) {
        return true;
      }
    }
  }

  return false;
}

static void take(civue_tree *t, u32 at, u32 ncols, u32 *nrows, u32 r, u32 c,
                 u32 rs, u32 cs) {
  if (r + rs > *nrows) {
    u32 more = (r + rs - *nrows) * ncols;
    vec_reserve(&t->cells, more);
    memset(t->cells.v + t->cells.len, 0, more);
    t->cells.len += more;
    *nrows = r + rs;
  }

  for (u32 y = r; y < r + rs; y++) {
    memset(t->cells.v + at + y * ncols + c, 1, cs);
  }
}

// css's sparse auto placement, row by row, of items spanning cells.
static u32 place_cells(civue_tree *t, u32 base, u32 count, u32 ncols) {
  u32 at = t->cells.len, nrows = 0, cur_r = 0, cur_c = 0, used = 0;

  for (u32 k = base; k < base + count; k++) {
    Item *it = &t->items.v[k];
    const civue_node_layout *s = &t->nodes.v[it->node].l;
    u32 cs = s->col_span == CIVUE_SPAN_FULL ? ncols : MAX(s->col_span, 1);
    u32 rs = MAX(s->row_span, 1);
    if (s->col_start && s->col_end > s->col_start) {
      cs = s->col_end - s->col_start;
    }
    if (s->row_start && s->row_end > s->row_start) {
      rs = s->row_end - s->row_start;
    }
    cs = MIN(cs, ncols);

    i32 fc = s->col_start ? s->col_start - 1
             : s->col_end ? (i32)s->col_end - 1 - (i32)cs
                          : -1;
    i32 fr = s->row_start ? s->row_start - 1
             : s->row_end ? (i32)s->row_end - 1 - (i32)rs
                          : -1;
    if (s->col_start || s->col_end) {
      fc = CLAMP(fc, 0, (i32)(ncols - cs));
    }
    if (s->row_start || s->row_end) {
      fr = MAX(fr, 0);
    }

    u32 r = 0, c = 0;
    if (fr >= 0 && fc >= 0) {
      r = fr, c = fc;
    }
    else if (fr >= 0) {
      r = fr;
      while (c + cs < ncols && taken(t, at, ncols, nrows, r, c, rs, cs)) {
        c++;
      }
    }
    else if (fc >= 0) {
      c = fc;
      r = cur_r;
      while (taken(t, at, ncols, nrows, r, c, rs, cs)) {
        r++;
      }
    }
    else {
      r = cur_r, c = cur_c;
      for (;;) {
        if (c + cs > ncols) {
          r++, c = 0;
          continue;
        }
        if (!taken(t, at, ncols, nrows, r, c, rs, cs)) {
          break;
        }
        c++;
      }
      cur_r = r, cur_c = c + cs;
    }

    take(t, at, ncols, &nrows, r, c, rs, cs);
    it->row = r, it->col = c, it->rspan = rs, it->cspan = cs;
    used = MAX(used, r + rs);
  }

  t->cells.len = at;
  return used;
}

static Size layout_grid(civue_ui *ui, civue_tree *t, u32 i, const Box *b) {
  const civue_node_layout *s = b->s;
  u32 ncols = MAX(s->cols, 1);
  f32 iw = isnan(b->w) ? NONE : b->w - b->ph;
  f32 ih = isnan(b->h) ? NONE : b->h - b->pv;
  f32 gx = resolve0(t, s->gap_x, iw), gy = resolve0(t, s->gap_y, ih);
  civue_place items = s->align_items ? s->align_items : CIVUE_PLACE_STRETCH;

  u32 base = t->items.len;
  for (u32 c = t->nodes.v[i].first; c != NIL; c = t->nodes.v[c].next) {
    const civue_node_layout *cs = &t->nodes.v[c].l;
    if (cs->position == CIVUE_ABSOLUTE) {
      continue;
    }

    Edges m = edges(t, cs->margin, iw);
    Item it = {.node = c, .m0 = m.t, .m1 = m.b, .c0 = m.l, .c1 = m.r};
    it.align = cs->align_self ? cs->align_self : items;
    vec_push(&t->items, it);
  }

  u32 count = t->items.len - base;
  u32 nrows = MAX(place_cells(t, base, count, ncols), s->rows);
  u32 explicit_rows = MIN(s->rows, nrows);
  f32 gaps_x = gx * (ncols - 1), gaps_y = nrows ? gy * (nrows - 1) : 0;

  // every column is 1fr, minmax(0, 1fr) as gpui's grid_cols.
  f32 col;
  if (!isnan(iw)) {
    col = fmaxf((iw - gaps_x) / ncols, 0);
  }
  else {
    col = 0;
    for (u32 k = base; k < base + count; k++) {
      Item it = t->items.v[k];
      const civue_node_layout *cs = &t->nodes.v[it.node].l;
      Size z = compute(ui, t, it.node, resolve(t, cs->w, NONE), NONE, INFINITY,
                       INFINITY, NONE, NONE, 0);
      f32 want = z.w + it.c0 + it.c1 - gx * (it.cspan - 1);
      col = fmaxf(col, want / it.cspan);
    }

    f32 w =
      clampf(fminf(col * ncols + gaps_x, b->aiw) + b->ph, b->minw, b->maxw);
    col = fmaxf((w - b->ph - gaps_x) / ncols, 0);
  }

  f32 w = isnan(b->w) ? col * ncols + gaps_x + b->ph : b->w;
  iw = w - b->ph;

  u32 rows = t->tracks.len;
  vec_reserve(&t->tracks, nrows * 2);
  memset(t->tracks.v + rows, 0, sizeof(f32) * nrows * 2);
  t->tracks.len += nrows * 2;
  u32 bases = rows + nrows;
  for (u32 r = 0; r < nrows; r++) {
    t->tracks.v[bases + r] = NONE;
  }

  for (u32 pass = 0; pass < 2; pass++) {
    for (u32 k = base; k < base + count; k++) {
      Item it = t->items.v[k];
      if ((pass == 0) != (it.rspan == 1)) {
        continue;
      }

      const civue_node_layout *cs = &t->nodes.v[it.node].l;
      f32 cell = col * it.cspan + gx * (it.cspan - 1);
      f32 cw = resolve(t, cs->w, cell);
      if (isnan(cw)) {
        cw = cell - it.c0 - it.c1;
      }
      Size z = compute(ui, t, it.node, cw, resolve(t, cs->h, NONE), cw,
                       INFINITY, cell, NONE, 0);
      t->items.v[k].cross = cw;
      t->items.v[k].main = z.h;
      t->items.v[k].base = isnan(z.base) ? z.h : z.base;

      f32 *track = t->tracks.v + rows + it.row;
      f32 need = z.h + it.m0 + it.m1;
      if (it.rspan == 1) {
        track[0] = fmaxf(track[0], need);
        if (it.align == CIVUE_PLACE_BASELINE) {
          f32 *line = t->tracks.v + bases + it.row;
          f32 up = it.m0 + t->items.v[k].base;
          *line = isnan(*line) ? up : fmaxf(*line, up);
        }
        continue;
      }

      f32 have = gy * (it.rspan - 1);
      for (u32 r = 0; r < it.rspan; r++) {
        have += track[r];
      }
      if (need > have) {
        for (u32 r = 0; r < it.rspan; r++) {
          track[r] += (need - have) / it.rspan;
        }
      }
    }
  }

  // a baseline row holds what hangs under its baselines too.
  for (u32 k = base; k < base + count; k++) {
    Item it = t->items.v[k];
    f32 line = t->tracks.v[bases + it.row];
    if (it.rspan == 1 && it.align == CIVUE_PLACE_BASELINE) {
      f32 need = line + it.main - it.base + it.m1;
      f32 *track = t->tracks.v + rows + it.row;
      *track = fmaxf(*track, need);
    }
  }

  // the explicit rows are 1fr too: they share what the box leaves them, or
  // all take the tallest when it follows its content.
  if (explicit_rows) {
    f32 *track = t->tracks.v + rows;
    f32 share = 0;
    if (!isnan(ih)) {
      share = ih - gaps_y;
      for (u32 r = explicit_rows; r < nrows; r++) {
        share -= track[r];
      }
      share = fmaxf(share / explicit_rows, 0);
    }
    else {
      for (u32 r = 0; r < explicit_rows; r++) {
        share = fmaxf(share, track[r]);
      }
    }

    for (u32 r = 0; r < explicit_rows; r++) {
      track[r] = share;
    }
  }

  f32 content = gaps_y;
  for (u32 r = 0; r < nrows; r++) {
    content += t->tracks.v[rows + r];
  }
  f32 h = isnan(b->h) ? clampf(content + b->pv, b->minh, b->maxh) : b->h;
  ih = h - b->pv;

  Size out = {w, h, NONE, 0, 0};
  f32 x0 = b->bor.l + b->pad.l, y0 = b->bor.t + b->pad.t;
  for (u32 k = base; k < base + count; k++) {
    Item it = t->items.v[k];
    const f32 *track = t->tracks.v + rows;
    f32 y = y0;
    for (u32 r = 0; r < it.row; r++) {
      y += track[r] + gy;
    }
    f32 area = gy * (it.rspan - 1);
    for (u32 r = 0; r < it.rspan; r++) {
      area += track[it.row + r];
    }

    const civue_node_layout *cs = &t->nodes.v[it.node].l;
    f32 ch = it.main;
    if (it.align == CIVUE_PLACE_STRETCH && auto_height(cs)) {
      f32 lo = resolve0(t, cs->min_h, ih), hi = resolve(t, cs->max_h, ih);
      ch = clampf(area - it.m0 - it.m1, lo, isnan(hi) ? INFINITY : hi);
    }

    Item a = it;
    a.c0 = it.m0, a.c1 = it.m1;
    f32 at = across(&a, area, ch, t->tracks.v[bases + it.row]);
    f32 x = x0 + it.col * (col + gx) + it.c0;
    if (isnan(out.base) && it.row == 0) {
      out.base = y + at +
                 (it.align == CIVUE_PLACE_BASELINE
                    ? it.base
                    : baseline_at(ui, t, it.node, it.cross, ch, iw, ih));
    }

    if (b->flags & PERFORM) {
      compute(ui, t, it.node, it.cross, ch, it.cross, ch, iw, ih, PERFORM);
      place(t, it.node, x, y + at, it.cross, ch);
    }
  }

  t->tracks.len = rows;
  t->items.len = base;
  return out;
}

// ---- every node ----

// places the absolute children and measures what the node scrolls over.
static void finish(civue_ui *ui, civue_tree *t, u32 i, const Box *b, Size z) {
  f32 pw = z.w - b->bor.l - b->bor.r, ph = z.h - b->bor.t - b->bor.b;
  f32 x0 = b->bor.l + b->pad.l, y0 = b->bor.t + b->pad.t;
  f32 right = z.w - b->bor.r, bottom = z.h - b->bor.b;

  for (u32 c = t->nodes.v[i].first; c != NIL; c = t->nodes.v[c].next) {
    const Node *cn = &t->nodes.v[c];
    if (cn->l.position == CIVUE_ABSOLUTE) {
      absolute(ui, t, c, b->bor.l, b->bor.t, pw, ph, x0, y0);
    }

    Edges m = edges(t, cn->l.margin, pw);
    right = fmaxf(right, cn->x + cn->w + m.r);
    bottom = fmaxf(bottom, cn->y + cn->h + m.b);
  }

  Node *n = &t->nodes.v[i];
  n->cw = right - b->bor.l + b->pad.r;
  n->ch = bottom - b->bor.t + b->pad.b;
  bool scrolls = b->s->overflow_x == CIVUE_OVERFLOW_SCROLL ||
                 b->s->overflow_y == CIVUE_OVERFLOW_SCROLL;
  if (scrolls && !n->quiet &&
      (fabsf(n->cw - n->ucw) > SETTLED || fabsf(n->ch - n->uch) > SETTLED)) {
    t->dirty = true;
  }
}

static Size compute(civue_ui *ui, civue_tree *t, u32 i, f32 kw, f32 kh, f32 aw,
                    f32 ah, f32 cbw, f32 cbh, u32 flags) {
  const civue_node_layout *s = &t->nodes.v[i].l;
  Box b = {.s = s, .flags = flags};
  b.pad = edges(t, s->padding, cbw);
  b.bor = border_of(s);
  b.ph = b.pad.l + b.pad.r + b.bor.l + b.bor.r;
  b.pv = b.pad.t + b.pad.b + b.bor.t + b.bor.b;

  b.minw = fmaxf(resolve0(t, s->min_w, cbw), b.ph);
  b.minh = fmaxf(resolve0(t, s->min_h, cbh), b.pv);
  b.maxw = resolve(t, s->max_w, cbw), b.maxh = resolve(t, s->max_h, cbh);
  if (isnan(b.maxw)) {
    b.maxw = INFINITY;
  }
  if (isnan(b.maxh)) {
    b.maxh = INFINITY;
  }

  b.w = isnan(kw) ? resolve(t, s->w, cbw) : kw;
  b.h = isnan(kh) ? resolve(t, s->h, cbh) : kh;
  if (s->aspect > 0 && isnan(b.w) && !isnan(b.h)) {
    b.w = b.h * s->aspect;
  }
  if (s->aspect > 0 && isnan(b.h) && !isnan(b.w)) {
    b.h = b.w / s->aspect;
  }
  if (!isnan(b.w)) {
    b.w = clampf(b.w, b.minw, b.maxw);
  }
  if (!isnan(b.h)) {
    b.h = clampf(b.h, b.minh, b.maxh);
  }
  b.aiw = (isnan(b.w) ? fminf(aw, b.maxw) : b.w) - b.ph;
  b.aih = (isnan(b.h) ? fminf(ah, b.maxh) : b.h) - b.pv;

  // asked the same when what differs can't change the answer: by its size
  // however it came to it, the room around it when that size is set, the
  // containing block when it has no fraction of it.
  Node *n = &t->nodes.v[i];
  f32 key[7] = {b.w,
                b.h,
                isnan(b.w) ? aw : 0,
                isnan(b.h) ? ah : 0,
                n->relative ? cbw : 0,
                n->relative ? cbh : 0,
                (f32)(flags & COLLAPSE)};
  Size out;
  if (!(flags & PERFORM) && cached(n, key, &out)) {
    return out;
  }

  // what a memo put back is where its frame left it when asked the same.
  if ((flags & PERFORM) && n->kept && n->solved &&
      !memcmp(n->solved_key, key, sizeof key)) {
    return n->solved_size;
  }

  if (t->nodes.v[i].is_text) {
    out = measure_text(ui, t, &t->nodes.v[i], &b, aw);
  }
  else if (s->display == CIVUE_FLEX) {
    out = layout_flex(ui, t, i, &b);
  }
  else if (s->display == CIVUE_GRID) {
    out = layout_grid(ui, t, i, &b);
  }
  else {
    out = layout_block(ui, t, i, &b);
  }

  if (flags & PERFORM) {
    finish(ui, t, i, &b, out);
    Node *n = &t->nodes.v[i];
    n->solved = true;
    n->solved_size = out;
    memcpy(n->solved_key, key, sizeof key);
  }
  else {
    remember(&t->nodes.v[i], key, out);
  }

  return out;
}

// ---- memos ----

// memos not asked for in this many frames are let go.
constexpr u64 MEMO_FRAMES = 600;

static bool near(f32 a, f32 b) {
  return fabsf(a - b) <= SETTLED;
}

static Memo *memo_find(civue_tree *t, civue_id id) {
  u64 at;
  return map_get(&t->memo_index, id, &at) ? &t->memos.v[at - 1] : nullptr;
}

static Memo *memo_of(civue_tree *t, civue_id id) {
  Memo *m = memo_find(t, id);
  if (!m) {
    m = vec_add(&t->memos);
    m->id = id;
    map_put(&t->memo_index, id, t->memos.len);
  }

  return m;
}

static void forget_memos(civue_ui *ui, civue_tree *t) {
  if (ui->frames % 256 || !t->memos.len) {
    return;
  }

  u32 k = 0;
  map_clear(&t->memo_index);
  vec_each(&t->memos, m) {
    if (ui->frames - m->used > MEMO_FRAMES) {
      memo_free(m);
      continue;
    }

    t->memos.v[k++] = *m;
    map_put(&t->memo_index, m->id, k);
  }
  t->memos.len = k;
}

// all a block's nodes take from what is around them.
static u64 around(civue_ui *ui, const civue_tree *t, const Open *parent) {
  struct {
    civue_id next;
    civue_text_style ts;
    civue_color color;
    f32 lh_rel, scale, rem;
    u32 flags;
  } a = {};

  a.next = next_id(t, parent);
  a.scale = ui->scale;
  a.rem = t->rem;
  a.flags = ui->dark | ui->on_overlay << 1;

  if (parent) {
    a.ts = parent->ts;
    a.color = parent->color;
    a.lh_rel = parent->lh_rel;
    a.flags |=
      (u32)parent->align << 2 | parent->nowrap << 5 | parent->truncate << 6;
  }

  return hash_bytes(&a, sizeof a);
}

static bool holds(const Memo *m, civue_id id) {
  if (!id) {
    return false;
  }

  vec_each(&m->regions, r) {
    if (r->id == id) {
      return true;
    }
  }

  vec_each(&m->nodes, n) {
    if (n->id == id) {
      return true;
    }
  }

  return false;
}

// whether what the memo drew, moved by `dx` and `dy` pixels, is under `p`.
static bool under(const civue_ui *ui, const Memo *m, f32 dx, f32 dy,
                  civue_vec2 p) {
  f64 x = p.x * ui->scale - dx, y = p.y * ui->scale - dy;
  return x >= m->box[0] - 1 && y >= m->box[1] - 1 && x <= m->box[2] + 1 &&
         y <= m->box[3] + 1;
}

// whether the memo's block would build what it built: nothing it answers to
// happened, and its nodes are where they were, all moved by whole pixels.
static bool stands(civue_ui *ui, civue_tree *t, const Memo *m,
                   const Open *parent, f32 *dx, f32 *dy) {
  const civue_input *in = &ui->input;
  if (in->keys.len || in->text.len || in->drops.len || in->commands.len) {
    return false;
  }

  for (u32 i = 0; i < 3; i++) {
    if (in->pressed[i] || in->released[i]) {
      return false;
    }
  }

  if (holds(m, ui->active) || holds(m, ui->focus) || holds(m, ui->ring)) {
    return false;
  }

  civue_vec2 at = parent ? parent->origin : v2(0, 0);
  f64 mx = (at.x - m->origin.x) * ui->scale;
  f64 my = (at.y - m->origin.y) * ui->scale;
  if (fabs(mx - round(mx)) > 1e-3 || fabs(my - round(my)) > 1e-3) {
    return false;
  }

  *dx = (f32)round(mx), *dy = (f32)round(my);
  civue_rect c = civue_dl_clip(civue_list(ui));
  f32 clip[4] = {(f32)c.x0, (f32)c.y0, (f32)c.x1, (f32)c.y1};
  if (m->exact && (*dx || *dy || memcmp(clip, m->clip, sizeof clip))) {
    return false;
  }

  if ((in->has_mouse && under(ui, m, *dx, *dy, in->mouse)) ||
      (ui->had_mouse && under(ui, m, *dx, *dy, ui->last_mouse))) {
    return false;
  }

  u32 base = t->nodes.len;
  for (u32 i = 0; i < m->nodes.len; i++) {
    const Node *n = &m->nodes.v[i];
    const Placed *was = placed(t, base + i, n->id);
    if (was &&
        !(near(was->x, n->x) && near(was->y, n->y) && near(was->w, n->w) &&
          near(was->h, n->h) && near(was->cw, n->cw) && near(was->ch, n->ch))) {
      return false;
    }
  }

  return true;
}

// puts back the nodes the memo's block built, and what they drew.
static void put_back(civue_ui *ui, civue_tree *t, const Memo *m, Open *parent,
                     f32 dx, f32 dy) {
  u32 base = t->nodes.len, text = t->text.len;
  bool quiet = parent && parent->quiet;

  vec_extend(&t->nodes, m->nodes.v, m->nodes.len);
  vec_extend(&t->text, m->text.v, m->text.len);

  for (u32 i = 0; i < m->nodes.len; i++) {
    Node *n = &t->nodes.v[base + i];
    bool top = n->parent == NIL || n->parent < m->start;

    n->first = n->first == NIL ? NIL : n->first - m->start + base;
    n->last = n->last == NIL ? NIL : n->last - m->start + base;
    n->text_at = n->text_at - m->text_start + text;
    n->ux = n->x, n->uy = n->y, n->uw = n->w, n->uh = n->h;
    n->ucw = n->cw, n->uch = n->ch;
    n->quiet = quiet;
    n->kept = true;
    // the blocks of the frame it was recorded in may be gone.
    n->whole = n->wrapped = nullptr;

    if (top) {
      link(t, parent, base + i);
    }
    else {
      n->parent = n->parent - m->start + base;
      n->next = n->next == NIL ? NIL : n->next - m->start + base;
    }
  }

  civue_dl_replay(civue_list(ui), m->quads.v, m->quads.len, dx, dy, m->clip);

  civue_vec2 by = v2(dx / ui->scale, dy / ui->scale);
  civue_rect seen = civue_clip_rect(ui);
  vec_each(&m->regions, r) {
    civue_region moved = *r;
    moved.rect = rect_intersect(rect(r->rect.x0 + by.x, r->rect.y0 + by.y,
                                     r->rect.x1 + by.x, r->rect.y1 + by.y),
                                seen);
    if (!rect_empty(moved.rect)) {
      vec_push(&ui->regions, moved);
    }
  }
}

civue_memo_scope civue_memo_begin(civue_ui *ui, civue_id id, u64 inputs) {
  civue_tree *t = tree(ui);
  Open *parent = parent_of(ui, t);
  u64 a = around(ui, t, parent);
  Memo *m = memo_of(t, id);
  m->used = ui->frames;

  f32 dx = 0, dy = 0;
  if (m->valid && m->inputs == inputs && m->around == a &&
      stands(ui, t, m, parent, &dx, &dy)) {
    put_back(ui, t, m, parent, dx, dy);
    return (civue_memo_scope){.ui = ui};
  }

  m->valid = false;
  m->inputs = inputs;
  m->around = a;
  m->origin = parent ? parent->origin : v2(0, 0);

  civue_draw_list *d = civue_list(ui);
  civue_rect c = civue_dl_clip(d);
  m->clip[0] = (f32)c.x0, m->clip[1] = (f32)c.y0;
  m->clip[2] = (f32)c.x1, m->clip[3] = (f32)c.y1;

  const civue_draw_list *other = ui->on_overlay ? &ui->base : &ui->overlay;
  Take take = {
    .id = id,
    .node = t->nodes.len,
    .text = t->text.len,
    .quad = d->quads.len,
    .region = ui->regions.len,
    .batch = d->batches.len,
    .other = other->quads.len,
    .texture = d->batches.len ? vec_last(&d->batches).texture : 0,
    .overlay = ui->on_overlay,
    .multiply = d->multiply,
    .kept_hidden = d->keep_hidden,
    .repaint = ui->repaint,
    .focus_alive = ui->focus_alive,
    .repaint_at = ui->repaint_at,
    .scroll_areas = ui->scroll_areas,
    .cuts = d->cuts,
    .open = t->open.len,
  };
  vec_push(&t->takes, take);
  d->keep_hidden = true;

  return (civue_memo_scope){.ui = ui, .build = true, .recording = true};
}

// whether what was recorded can be drawn again as it is: nothing in it
// moves by itself, scrolls, holds the focus or draws an image, and all of it
// went to one list.
static bool lasting(const civue_ui *ui, const civue_tree *t, const Take *k,
                    const civue_draw_list *d) {
  const civue_draw_list *other = k->overlay ? &ui->base : &ui->overlay;
  if (k->failed || ui->on_overlay != k->overlay ||
      other->quads.len != k->other || t->open.len != k->open) {
    return false;
  }

  if ((ui->repaint && !k->repaint) || ui->repaint_at != k->repaint_at ||
      (ui->focus_alive && !k->focus_alive) ||
      ui->scroll_areas != k->scroll_areas) {
    return false;
  }

  // no image: the batch it was drawn in before, and any it started.
  if (d->multiply || k->multiply ||
      (k->batch && d->batches.v[k->batch - 1].texture != k->texture)) {
    return false;
  }

  for (u32 i = k->batch; i < d->batches.len; i++) {
    if (d->batches.v[i].texture || d->batches.v[i].multiply) {
      return false;
    }
  }

  for (u32 i = k->quad; i < d->quads.len; i++) {
    const f32 *turn = d->quads.v[i].turn;
    if (turn[0] != 0 || turn[1] != 0) {
      return false;
    }
  }

  return true;
}

void civue_memo_end(civue_memo_scope *s) {
  if (!s->recording) {
    return;
  }

  civue_ui *ui = s->ui;
  civue_tree *t = tree(ui);
  Take k = vec_pop(&t->takes);
  civue_draw_list *d = k.overlay ? &ui->overlay : &ui->base;
  d->keep_hidden = k.kept_hidden;

  Memo *m = memo_find(t, k.id);
  if (!m || !lasting(ui, t, &k, d)) {
    return;
  }

  m->exact = d->cuts != k.cuts;
  m->quads.len = 0;
  vec_extend(&m->quads, d->quads.v + k.quad, d->quads.len - k.quad);
  m->regions.len = 0;
  vec_extend(&m->regions, ui->regions.v + k.region, ui->regions.len - k.region);

  m->box[0] = m->box[1] = INFINITY;
  m->box[2] = m->box[3] = -INFINITY;
  vec_each(&m->quads, q) {
    m->box[0] = fminf(m->box[0], q->rect[0]);
    m->box[1] = fminf(m->box[1], q->rect[1]);
    m->box[2] = fmaxf(m->box[2], q->rect[0] + q->rect[2]);
    m->box[3] = fmaxf(m->box[3], q->rect[1] + q->rect[3]);
  }

  k.node_end = t->nodes.len;
  k.text_end = t->text.len;
  vec_push(&t->taken, k);
}

// the memos recorded this frame are kept when the solve left their nodes
// where they were painted.
static void keep_memos(civue_ui *ui, civue_tree *t) {
  vec_each(&t->taken, k) {
    Memo *m = memo_find(t, k->id);
    if (!m) {
      continue;
    }

    bool settled = true;
    for (u32 i = k->node; i < k->node_end && settled; i++) {
      const Node *n = &t->nodes.v[i];
      settled = near(n->x, n->ux) && near(n->y, n->uy) && near(n->w, n->uw) &&
                near(n->h, n->uh) && near(n->cw, n->ucw) && near(n->ch, n->uch);
    }

    if (!settled) {
      continue;
    }

    m->start = k->node;
    m->text_start = k->text;
    m->nodes.len = 0;
    vec_extend(&m->nodes, t->nodes.v + k->node, k->node_end - k->node);
    m->text.len = 0;
    vec_extend(&m->text, t->text.v + k->text, k->text_end - k->text);
    m->valid = true;
  }

  t->taken.len = 0;
  forget_memos(ui, t);
}

bool civue__tree_end(civue_ui *ui) {
  civue_tree *t = ui->tree;
  if (!t) {
    return true;
  }

  if (t->open.len) {
    die("civue: %u nodes were left open", t->open.len);
  }

  if (t->takes.len) {
    die("civue: %u memos were left open", t->takes.len);
  }

  t->dirty = false;
  f32 w = (f32)ui->size.x, h = (f32)ui->size.y;
  for (u32 r = t->first_root; r != NIL; r = t->nodes.v[r].next) {
    absolute(ui, t, r, 0, 0, w, h, 0, 0);
  }

  keep_memos(ui, t);

  t->prev.len = 0;
  vec_reserve(&t->prev, t->nodes.len);
  vec_each(&t->nodes, n) {
    t->prev.v[t->prev.len++] =
      (Placed){n->id, n->x, n->y, n->w, n->h, n->cw, n->ch};
  }

  t->indexed = false;
  t->nodes.len = 0;
  t->text.len = 0;
  t->roots = 0;
  t->first_root = t->last_root = NIL;
  return !t->dirty;
}

// ---- rows ----

// rows built past each end of the view, so one scrolled into view was laid
// out the frame before.
constexpr u32 OVERSCAN = 2;

civue_rows_scope civue_rows_begin(civue_ui *ui, u32 count, f32 row_height) {
  civue_tree *t = tree(ui);
  f32 rh = fmaxf(row_height, 1);

  // which rows are built follows the scroll, that no memo around them sees.
  vec_each(&t->takes, k) {
    k->failed = true;
  }

  civue_node_open(
    ui, &(civue_node_style){.h = civue_px(count * rh), .has_shrink = true});

  const Open *o = top(t);
  civue_rows_scope s = {
    .ui = ui, .open = true, .id = t->nodes.v[o->node].id, .row_height = rh};
  civue_rect seen = civue_clip_rect(ui);
  f64 y0 = fmax(seen.y0, 0) - o->rect.y0;
  f64 y1 = fmin(seen.y1, ui->size.y) - o->rect.y0;

  if (rect_empty(o->rect) || y1 <= y0) {
    // not laid out yet, or out of view: enough rows to have a size.
    s.last = MIN(count, OVERSCAN);
  }
  else {
    f64 first = floor(y0 / rh) - OVERSCAN, last = ceil(y1 / rh) + OVERSCAN;
    s.first = (u32)fmin(fmax(first, 0), count);
    s.last = (u32)fmin(fmax(last, s.first), count);
  }

  if (s.first) {
    civue_node_open(ui, &(civue_node_style){.key = civue_ids(s.id, "before"),
                                            .h = civue_px(s.first * rh),
                                            .has_shrink = true});
    civue_node_close(ui);
  }

  return s;
}

void civue_rows_end(civue_rows_scope *s) {
  civue_node_close(s->ui);
}

void civue_row_open(civue_rows_scope *s, u32 i) {
  civue_ui *ui = s->ui;
  civue_tree *t = tree(ui);
  const Open *o = top(t);
  f32 rh = s->row_height, y = i * rh;
  civue_rect seen = civue_clip_rect(ui);

  // a row the last frame had not is where its index puts it, and settles
  // out of view without the frame running again.
  t->has_hint = true;
  t->hint[0] = 0, t->hint[1] = y;
  t->hint[2] = (f32)rect_w(o->rect), t->hint[3] = rh;
  t->hint_quiet = o->rect.y0 + y + rh <= seen.y0 || o->rect.y0 + y >= seen.y1;

  civue_node_open(ui, &(civue_node_style){.key = civue_idn(s->id, i),
                                          .h = civue_px(rh),
                                          .has_shrink = true});

  // what it holds has no place yet either, and must not show past it.
  Open *row = top(t);
  if (row->quiet && !row->clip && !row->scroll) {
    civue_push_clip(ui, row->rect);
    row->clip = true;
  }
}

// ---- widgets ----

civue_button_state civue_node_button(civue_ui *ui) {
  return civue_button(ui, civue_node_id(ui), civue_node_rect(ui));
}

civue_toggle_state civue_node_toggle(civue_ui *ui, bool on) {
  return civue_toggle(ui, civue_node_id(ui), civue_node_rect(ui), on);
}

civue_slider_state civue_node_slider(civue_ui *ui, f64 value, f64 min, f64 max,
                                     f64 anchor, f64 step) {
  return civue_slider(ui, civue_node_id(ui), civue_node_rect(ui),
                      civue_node_content(ui), value, min, max, anchor, step);
}

civue_field_response civue_node_text_field(civue_ui *ui, char **value,
                                           const char *placeholder,
                                           civue_field_look look) {
  return civue_text_field(ui, civue_node_id(ui), civue_node_rect(ui), value,
                          placeholder, civue_node_text_style(ui), look);
}
