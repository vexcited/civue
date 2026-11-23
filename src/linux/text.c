#include "../internal.h"

#include <civue/text.h>

#include <fontconfig/fontconfig.h>
#include <fribidi.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_MULTIPLE_MASTERS_H
#include <hb-ot.h>
#include <hb.h>
#include <math.h>
#include <stddefer.h>
#include <stdlib.h>

// horizontal positions a glyph is rasterized at within a pixel.
constexpr u32 SUBPIXELS = 4;
constexpr u32 WGHT = 0x77676874;

// faces of the ui and of the system, by where their bytes are. the ui and the
// code ones are empty until the app sets them, the system's faces standing in.
enum {
  SRC_UI,
  SRC_CODE,
  SRC_DISPLAY,
  SRC_EMOJI = SRC_DISPLAY + CIVUE_TEXT_FACES + 1
};

typedef struct {
  // built in or registered bytes, else a file and its index.
  const u8 *data;
  usize len;
  char *path;
  u32 index;
  hb_face_t *hb;
  bool color, variable;
} Source;

// a source at a size and weight, what glyphs are drawn with.
typedef struct {
  u32 source;
  f32 px, weight;
  hb_font_t *hb;
  FT_Face ft;
  bool color;
  // pixels of a bitmap glyph per pixel of its strike.
  f32 strike_scale;
} Font;

typedef struct {
  u64 key;
  char *text;
  usize len;
  civue_text_block *block;
  u64 used;
} Layout;

typedef struct {
  u64 key;
  f32 uv[4];
  bool color;
  i32 left, top;
  u32 w, h;
  u64 generation;
} GlyphEntry;

struct civue_text_system {
  FT_Library ft;
  Vec(Source) sources;
  // the system's faces for code points the ui faces lack, by code point.
  civue_map fallback;
  // the system's sans-serif and monospace faces by weight, while the app sets
  // none.
  civue_map system;
  bool emoji_looked;
  Vec(Font) fonts;
  civue_map font_index;
  Vec(Layout) layouts;
  civue_map layout_index; // key to index + 1.
  Vec(GlyphEntry) glyphs;
  civue_map glyph_index;
  u64 frame;
};

static void source_set(Source *s, const u8 *data, usize len) {
  if (s->hb) {
    hb_face_destroy(s->hb);
  }
  s->data = data;
  s->len = len;
  hb_blob_t *blob = hb_blob_create((const char *)data, (u32)len,
                                   HB_MEMORY_MODE_READONLY, nullptr, nullptr);
  s->hb = hb_face_create(blob, s->index);
  hb_blob_destroy(blob);
  s->variable = hb_ot_var_has_data(s->hb);
}

civue_text_system *civue_text_new() {
  civue_text_system *t = own_new(sizeof *t);
  if (FT_Init_FreeType(&t->ft)) {
    die("freetype: no library");
  }
  FcInit();
  for (u32 i = 0; i < SRC_EMOJI + 1; i++) {
    vec_push(&t->sources, (Source){});
  }
  return t;
}

static void block_free(civue_text_block *b) {
  free(b->glyphs);
  free(b->carets);
  free(b->lines);
  own_free(b);
}

static void forget_layouts(civue_text_system *t) {
  vec_each(&t->layouts, l) {
    free(l->text);
    block_free(l->block);
  }
  t->layouts.len = 0;
  map_clear(&t->layout_index);
}

void civue_text_free(civue_text_system *t) {
  if (!t) {
    return;
  }
  forget_layouts(t);
  vec_free(&t->layouts);
  map_free(&t->layout_index);
  vec_each(&t->fonts, f) {
    hb_font_destroy(f->hb);
    if (f->ft) {
      FT_Done_Face(f->ft);
    }
  }
  vec_free(&t->fonts);
  map_free(&t->font_index);
  vec_each(&t->sources, s) {
    if (s->hb) {
      hb_face_destroy(s->hb);
    }
    if (s->path) {
      free((void *)s->data);
    }
    free(s->path);
  }
  vec_free(&t->sources);
  map_free(&t->fallback);
  map_free(&t->system);
  vec_free(&t->glyphs);
  map_free(&t->glyph_index);
  FT_Done_FreeType(t->ft);
  own_free(t);
}

// puts a copy of `data` in a source, forgetting the fonts and the layouts made
// from what it had.
static void source_replace(civue_text_system *t, u32 src, const u8 *data,
                           usize n) {
  Source *s = &t->sources.v[src];
  if (s->path) {
    free((void *)s->data);
  }
  u8 *copy = xmalloc(n);
  memcpy(copy, data, n);
  // owned like a file's bytes.
  free(s->path);
  s->path = xstrdup("");
  s->index = 0;
  source_set(s, copy, n);
  vec_each(&t->fonts, f) {
    if (f->source == src) {
      hb_font_destroy(f->hb);
      if (f->ft) {
        FT_Done_Face(f->ft);
      }
      f->hb = nullptr;
      f->ft = nullptr;
      f->source = UINT32_MAX;
    }
  }
  map_clear(&t->font_index);
  for (u32 i = 0; i < t->fonts.len; i++) {
    const Font *f = &t->fonts.v[i];
    if (f->source == UINT32_MAX) {
      continue;
    }
    u32 bits[3] = {f->source};
    memcpy(bits + 1, &f->px, 4);
    memcpy(bits + 2, &f->weight, 4);
    map_put(&t->font_index, hash_bytes(bits, sizeof bits) | 1, i + 1);
  }
  forget_layouts(t);
}

void civue_text_register_display(civue_text_system *t, u8 display,
                                 const u8 *data, usize n) {
  if (!display || display > CIVUE_TEXT_FACES) {
    return;
  }
  source_replace(t, SRC_DISPLAY + display, data, n);
}

void civue_text_set_face(civue_text_system *t, civue_face face, const u8 *data,
                         usize n) {
  source_replace(t, face == CIVUE_FACE_CODE ? SRC_CODE : SRC_UI, data, n);
}

// a face the system has, its bytes read once, nullptr when unreadable.
static u32 file_source(civue_text_system *t, const char *path, u32 index,
                       bool color) {
  for (u32 i = SRC_EMOJI + 1; i < t->sources.len; i++) {
    if (!strcmp(t->sources.v[i].path, path) && t->sources.v[i].index == index) {
      return i;
    }
  }
  civue_bytes b = {};
  if (!file_read(path, &b)) {
    return UINT32_MAX;
  }
  Source s = {.path = xstrdup(path), .index = index, .color = color};
  source_set(&s, b.v, b.len);
  vec_push(&t->sources, s);
  return t->sources.len - 1;
}

// the system's color emoji face, looked up once.
static void find_emoji(civue_text_system *t) {
  if (t->emoji_looked) {
    return;
  }
  t->emoji_looked = true;
  FcPattern *p = FcPatternCreate();
  FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"emoji");
  FcPatternAddBool(p, FC_COLOR, FcTrue);
  FcConfigSubstitute(nullptr, p, FcMatchPattern);
  FcDefaultSubstitute(p);
  FcResult r;
  FcPattern *m = FcFontMatch(nullptr, p, &r);
  FcChar8 *file;
  int index = 0;
  FcBool color = FcFalse;
  if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
    FcPatternGetInteger(m, FC_INDEX, 0, &index);
    FcPatternGetBool(m, FC_COLOR, 0, &color);
    if (color) {
      civue_bytes b = {};
      if (file_read((const char *)file, &b)) {
        Source *s = &t->sources.v[SRC_EMOJI];
        s->path = xstrdup((const char *)file);
        s->index = (u32)index;
        s->color = true;
        source_set(s, b.v, b.len);
      }
    }
  }
  if (m) {
    FcPatternDestroy(m);
  }
  FcPatternDestroy(p);
}

static bool covers(const Source *s, u32 cp) {
  hb_font_t *f = nullptr;
  if (!s->hb) {
    return false;
  }
  // a face's cmap answers without a size.
  f = hb_font_create(s->hb);
  hb_codepoint_t g;
  bool has = hb_font_get_nominal_glyph(f, cp, &g);
  hb_font_destroy(f);
  return has;
}

// the face text in the ui or the code face is set in at `weight`: the app's, or
// the system's sans-serif or monospace nearest the weight, upright or italic.
static u32 face_source(civue_text_system *t, bool code, f32 weight,
                       bool italic) {
  u32 src = code ? SRC_CODE : SRC_UI;
  if (t->sources.v[src].hb) {
    return src;
  }
  u64 key = ((u64)italic << 33 | (u64)code << 32 | (u32)lroundf(weight)) + 1, v;
  if (map_get(&t->system, key, &v)) {
    return (u32)v;
  }
  FcPattern *p = FcPatternCreate();
  FcPatternAddString(p, FC_FAMILY,
                     (const FcChar8 *)(code ? "monospace" : "sans-serif"));
  FcPatternAddDouble(p, FC_WEIGHT, FcWeightFromOpenTypeDouble(weight));
  FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
  FcConfigSubstitute(nullptr, p, FcMatchPattern);
  FcDefaultSubstitute(p);
  FcResult r;
  FcPattern *m = FcFontMatch(nullptr, p, &r);
  FcChar8 *file;
  u32 found = src;
  if (m && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
    int index = 0;
    FcPatternGetInteger(m, FC_INDEX, 0, &index);
    u32 at = file_source(t, (const char *)file, (u32)index, false);
    if (at != UINT32_MAX) {
      found = at;
    }
  }
  if (m) {
    FcPatternDestroy(m);
  }
  FcPatternDestroy(p);
  map_put(&t->system, key, found);
  return found;
}

// a system face for a code point no ui face has, UINT32_MAX for none.
static u32 fallback_for(civue_text_system *t, u32 cp) {
  u64 v;
  if (map_get(&t->fallback, (u64)cp + 1, &v)) {
    return (u32)v;
  }
  u32 found = UINT32_MAX;
  FcPattern *p = FcPatternCreate();
  FcCharSet *cs = FcCharSetCreate();
  FcCharSetAddChar(cs, cp);
  FcPatternAddCharSet(p, FC_CHARSET, cs);
  FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"sans-serif");
  FcConfigSubstitute(nullptr, p, FcMatchPattern);
  FcDefaultSubstitute(p);
  FcResult r;
  FcPattern *m = FcFontMatch(nullptr, p, &r);
  FcCharSet *has;
  FcChar8 *file;
  if (m && FcPatternGetCharSet(m, FC_CHARSET, 0, &has) == FcResultMatch &&
      FcCharSetHasChar(has, cp) &&
      FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
    int index = 0;
    FcBool color = FcFalse;
    FcPatternGetInteger(m, FC_INDEX, 0, &index);
    FcPatternGetBool(m, FC_COLOR, 0, &color);
    found = file_source(t, (const char *)file, (u32)index, color);
  }
  if (m) {
    FcPatternDestroy(m);
  }
  FcCharSetDestroy(cs);
  FcPatternDestroy(p);
  map_put(&t->fallback, (u64)cp + 1, found);
  return found;
}

// a source at a size and weight, made once.
static u32 font_of(civue_text_system *t, u32 source, f32 px, f32 weight) {
  u32 bits[3] = {source};
  memcpy(bits + 1, &px, 4);
  memcpy(bits + 2, &weight, 4);
  u64 key = hash_bytes(bits, sizeof bits) | 1, at;
  if (map_get(&t->font_index, key, &at)) {
    return (u32)at - 1;
  }
  const Source *s = &t->sources.v[source];
  Font f = {.source = source, .px = px, .weight = weight, .color = s->color};
  f.hb = hb_font_create(s->hb);
  i32 scale = (i32)lroundf(px * 64);
  hb_font_set_scale(f.hb, scale, scale);
  if (s->variable) {
    hb_variation_t wght = {WGHT, weight};
    hb_font_set_variations(f.hb, &wght, 1);
  }
  if (!FT_New_Memory_Face(t->ft, s->data, (FT_Long)s->len, s->index, &f.ft)) {
    f.color = f.color || FT_HAS_COLOR(f.ft);
    if (FT_IS_SCALABLE(f.ft)) {
      FT_Set_Char_Size(f.ft, 0, (FT_F26Dot6)lroundf(px * 64), 72, 72);
    }
    else if (f.ft->num_fixed_sizes) {
      // a bitmap face draws its nearest strike, scaled.
      i32 best = 0;
      for (i32 i = 1; i < f.ft->num_fixed_sizes; i++) {
        f32 a = f.ft->available_sizes[i].y_ppem / 64.f,
            b = f.ft->available_sizes[best].y_ppem / 64.f;
        if ((a >= px && (b < px || a < b)) || (b < px && a > b)) {
          best = i;
        }
      }
      FT_Select_Size(f.ft, best);
      f.strike_scale = px / (f.ft->available_sizes[best].y_ppem / 64.f);
    }
    if (s->variable && FT_HAS_MULTIPLE_MASTERS(f.ft)) {
      FT_MM_Var *mm;
      if (!FT_Get_MM_Var(f.ft, &mm)) {
        FT_Fixed coords[16];
        u32 n = MIN(mm->num_axis, 16u);
        for (u32 i = 0; i < n; i++) {
          coords[i] = mm->axis[i].tag == WGHT
                        ? (FT_Fixed)lroundf(weight * 65536)
                        : mm->axis[i].def;
        }
        FT_Set_Var_Design_Coordinates(f.ft, n, coords);
        FT_Done_MM_Var(t->ft, mm);
      }
    }
  }
  else {
    f.ft = nullptr;
  }
  vec_push(&t->fonts, f);
  map_put(&t->font_index, key, t->fonts.len);
  return t->fonts.len - 1;
}

static u64 layout_key(const char *s, usize n, civue_text_style st,
                      f32 max_width, f32 scale) {
  u64 h = hash_bytes(s, n);
  f32 v[] = {st.size,           st.weight, st.line_height,
             st.letter_spacing, max_width, scale};
  h ^= hash_bytes(v, sizeof v) * 0x9e3779b97f4a7c15ull;
  h ^= hash_u64(st.mono | st.tnum << 1 | st.emoji << 2 | (u64)st.italic << 8 |
                (u64)st.display << 3);
  return h | 1;
}

// where a line may break after code point `c` followed by `next`.
static bool break_after(u32 c, u32 next) {
  if (c == ' ' || c == '\t' || c == 0x3000) {
    return next != ' ';
  }
  if ((c == '-' || (c >= 0x2010 && c <= 0x2014)) && next != ' ') {
    return true;
  }
  bool wide = (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af) ||
              (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef);
  bool wide_next =
    (next >= 0x2e80 && next <= 0x9fff) || (next >= 0xac00 && next <= 0xd7af) ||
    (next >= 0xf900 && next <= 0xfaff) || (next >= 0xff00 && next <= 0xffef);
  return wide || wide_next;
}

static bool space(u32 c) {
  return c == ' ' || c == '\t' || c == 0x3000 || c == '\n' || c == 0x2028;
}

// a combining mark or joiner, drawn with what it follows.
static bool joins(u32 c) {
  hb_unicode_funcs_t *u = hb_unicode_funcs_get_default();
  hb_unicode_general_category_t g = hb_unicode_general_category(u, c);
  return g == HB_UNICODE_GENERAL_CATEGORY_NON_SPACING_MARK ||
         g == HB_UNICODE_GENERAL_CATEGORY_ENCLOSING_MARK ||
         g == HB_UNICODE_GENERAL_CATEGORY_SPACING_MARK || c == 0x200d ||
         (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0xe0100 && c <= 0xe01ef) ||
         (c >= 0x1f3fb && c <= 0x1f3ff);
}

typedef struct {
  u32 glyph, cluster; // the code point it starts at.
  f32 advance, x_offset, y_offset;
} Shaped;

typedef struct {
  u32 start, end; // code points.
  u32 font;
  u8 level;
  u32 g0, g1; // its glyphs, in visual order.
} Run;

static civue_text_block *build(civue_text_system *t, const char *s, usize n,
                               civue_text_style st, f32 max_width, f32 scale) {
  civue_text_block *b = own_new(sizeof *b);
  b->scale = scale;
  // code points and where each starts.
  civue_u32s cps = {}, at = {};
  for (usize i = 0; i < n;) {
    vec_push(&at, (u32)i);
    vec_push(&cps, utf8_next(s, n, &i));
  }
  vec_push(&at, (u32)n);
  u32 count = cps.len;
  f32 px = st.size * scale;
  u32 base = face_source(t, st.mono, st.weight, st.italic);
  if (st.display && t->sources.v[SRC_DISPLAY + st.display].hb) {
    base = SRC_DISPLAY + st.display;
  }
  if (st.emoji) {
    find_emoji(t);
  }
  u32 base_font = font_of(t, base, px, st.weight);
  // each code point's face: emoji first when asked, the ui face, then the
  // system's.
  civue_u32s fonts = {};
  for (u32 i = 0; i < count; i++) {
    u32 c = cps.v[i];
    u32 src = UINT32_MAX;
    if (i && joins(c)) {
      src = t->fonts.v[fonts.v[i - 1]].source;
    }
    else if (st.emoji && t->sources.v[SRC_EMOJI].hb &&
             covers(&t->sources.v[SRC_EMOJI], c) && c > 0x7f) {
      src = SRC_EMOJI;
    }
    else if (c < 0x20 || covers(&t->sources.v[base], c)) {
      src = base;
    }
    else {
      find_emoji(t);
      if (t->sources.v[SRC_EMOJI].hb && covers(&t->sources.v[SRC_EMOJI], c)) {
        src = SRC_EMOJI;
      }
      else {
        src = fallback_for(t, c);
      }
      if (src == UINT32_MAX) {
        src = base;
      }
    }
    vec_push(&fonts, src == base ? base_font : font_of(t, src, px, st.weight));
  }
  // bidi levels, the paragraph's direction from its first strong letter.
  u8 *levels = xcalloc(count + 1, 1);
  u8 base_level = 0;
  if (count) {
    FriBidiCharType *types = xcalloc(count, sizeof *types);
    FriBidiBracketType *brackets = xcalloc(count, sizeof *brackets);
    FriBidiLevel *lv = xcalloc(count, sizeof *lv);
    fribidi_get_bidi_types(cps.v, count, types);
    fribidi_get_bracket_types(cps.v, count, types, brackets);
    FriBidiParType dir = FRIBIDI_PAR_ON;
    if (fribidi_get_par_embedding_levels_ex(types, brackets, count, &dir, lv)) {
      for (u32 i = 0; i < count; i++) {
        levels[i] = lv[i];
      }
    }
    base_level = FRIBIDI_IS_RTL(dir) ? 1 : 0;
    free(types);
    free(brackets);
    free(lv);
  }
  // runs of one face and level, shaped.
  Vec(Run) runs = {};
  Vec(Shaped) shaped = {};
  hb_buffer_t *buf = hb_buffer_create();
  hb_feature_t features[2];
  u32 nfeatures = 0;
  if (st.tnum) {
    features[nfeatures++] =
      (hb_feature_t){HB_TAG('t', 'n', 'u', 'm'), 1, 0, (u32)-1};
  }
  for (u32 i = 0; i < count;) {
    u32 j = i + 1;
    while (j < count && fonts.v[j] == fonts.v[i] && levels[j] == levels[i]) {
      j++;
    }
    Run r = {i, j, fonts.v[i], levels[i], shaped.len, 0};
    hb_buffer_clear_contents(buf);
    hb_buffer_add_codepoints(buf, cps.v, (i32)count, i, (i32)(j - i));
    hb_buffer_set_direction(buf, levels[i] % 2 ? HB_DIRECTION_RTL
                                               : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(t->fonts.v[r.font].hb, buf, features, nfeatures);
    u32 ng;
    hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &ng);
    hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &ng);
    for (u32 g = 0; g < ng; g++) {
      vec_push(
        &shaped,
        ((Shaped){info[g].codepoint, info[g].cluster, pos[g].x_advance / 64.f,
                  pos[g].x_offset / 64.f, pos[g].y_offset / 64.f}));
    }
    r.g1 = shaped.len;
    vec_push(&runs, r);
    i = j;
  }
  hb_buffer_destroy(buf);
  // each code point's share of its cluster's advance: a ligature's
  // characters share its width for their carets. letter spacing goes after
  // every character, like core text's kern.
  f32 *adv = xcalloc(count + 1, sizeof *adv);
  vec_each(&shaped, g) {
    if (g->cluster < count) {
      adv[g->cluster] += g->advance;
    }
  }
  f32 kern = st.letter_spacing * scale;
  for (u32 i = 0; i < count;) {
    u32 j = i + 1;
    while (j < count && adv[j] == 0 && !space(cps.v[j]) && !joins(cps.v[j]) &&
           fonts.v[j] == fonts.v[i]) {
      j++;
    }
    f32 w = adv[i] / (f32)(j - i);
    for (u32 k = i; k < j; k++) {
      adv[k] = w + kern;
    }
    i = j;
  }
  f32 lh = st.line_height * scale;
  hb_font_extents_t ext = {};
  hb_font_get_h_extents(t->fonts.v[base_font].hb, &ext);
  f32 ascent = ext.ascender / 64.f, descent = -ext.descender / 64.f;
  // css places the font's box in the middle of the line.
  f32 first = (lh - (ascent + descent)) / 2 + ascent;
  b->baseline = first / scale;
  // lines, greedy, broken at the last chance before the width.
  civue_u32s breaks = {};
  if (max_width > 0) {
    f32 limit = max_width * scale, w = 0;
    u32 start = 0, chance = UINT32_MAX;
    for (u32 i = 0; i < count; i++) {
      u32 c = cps.v[i];
      if (c == '\n' || c == 0x2028) {
        vec_push(&breaks, i + 1);
        start = i + 1, w = 0, chance = UINT32_MAX;
        continue;
      }
      if (!space(c) && w + adv[i] > limit && i > start) {
        u32 at_ = chance != UINT32_MAX ? chance : i;
        vec_push(&breaks, at_);
        start = at_, chance = UINT32_MAX, w = 0;
        for (u32 k = start; k < i; k++) {
          w += adv[k];
        }
      }
      w += adv[i];
      if (i + 1 < count && break_after(c, cps.v[i + 1])) {
        chance = i + 1;
      }
    }
  }
  if (!breaks.len || vec_last(&breaks) != count) {
    vec_push(&breaks, count);
  }
  Vec(civue_glyph) glyphs = {};
  Vec(civue_caret) carets = {};
  Vec(civue_text_line) tlines = {};
  f32 width = 0;
  f32 *caret_x = xcalloc(count + 1, sizeof *caret_x);
  u32 start = 0;
  for (u32 line = 0; line < breaks.len; line++) {
    u32 end = breaks.v[line];
    f32 y = first + line * lh;
    // the line's runs in visual order: each level from the highest down to
    // the lowest odd one reverses the sequences at least that high.
    Vec(Run) lr = {};
    vec_each(&runs, r) {
      if (r->end <= start || r->start >= end) {
        continue;
      }
      Run part = *r;
      part.start = MAX(r->start, start);
      part.end = MIN(r->end, end);
      vec_push(&lr, part);
    }
    u8 high = 0, low_odd = 255;
    vec_each(&lr, r) {
      high = MAX(high, r->level);
      if (r->level % 2) {
        low_odd = MIN(low_odd, r->level);
      }
    }
    for (i32 lv = high; lv >= (i32)low_odd && lv > 0; lv--) {
      for (u32 i = 0; i < lr.len;) {
        if (lr.v[i].level < lv) {
          i++;
          continue;
        }
        u32 j = i;
        while (j < lr.len && lr.v[j].level >= lv) {
          j++;
        }
        for (u32 a = i, z = j - 1; a < z; a++, z--) {
          Run tmp = lr.v[a];
          lr.v[a] = lr.v[z];
          lr.v[z] = tmp;
        }
        i = j;
      }
    }
    f32 x = 0;
    vec_each(&lr, r) {
      bool rtl = r->level % 2;
      // the run's characters' leading edges.
      f32 run_w = 0;
      for (u32 k = r->start; k < r->end; k++) {
        run_w += adv[k];
      }
      f32 edge = rtl ? x + run_w : x;
      for (u32 k = r->start; k < r->end; k++) {
        caret_x[k] = edge;
        edge += rtl ? -adv[k] : adv[k];
      }
      // its glyphs of this line, already in visual order.
      f32 gx = x;
      for (u32 g = r->g0; g < r->g1; g++) {
        const Shaped *sh = &shaped.v[g];
        if (sh->cluster < r->start || sh->cluster >= r->end) {
          continue;
        }
        vec_push(&glyphs,
                 ((civue_glyph){at.v[sh->cluster], r->font, (u16)sh->glyph,
                                gx + sh->x_offset, y - sh->y_offset}));
        // the letter spacing after the character's last glyph.
        bool last =
          rtl ? g == r->g0 || shaped.v[g - 1].cluster != sh->cluster
              : g + 1 == r->g1 || shaped.v[g + 1].cluster != sh->cluster;
        gx += sh->advance + (last ? kern : 0);
      }
      x += run_w;
    }
    f32 trailing = 0;
    for (u32 k = end; k > start && space(cps.v[k - 1]); k--) {
      trailing += adv[k - 1];
    }
    width = fmaxf(width, x - trailing);
    for (u32 k = start; k < end; k++) {
      vec_push(&carets, ((civue_caret){at.v[k], caret_x[k] / scale, line}));
    }
    // the end of the last line, after its trailing space.
    bool last = line + 1 == breaks.len;
    if (last) {
      vec_push(&carets,
               ((civue_caret){(u32)n, (base_level ? 0 : x) / scale, line}));
    }
    vec_push(&tlines, ((civue_text_line){at.v[start], at.v[end],
                                         line * lh / scale, y / scale}));
    vec_free(&lr);
    start = end;
  }
  b->width = width / scale;
  b->nlines = tlines.len;
  b->lines = tlines.v;
  b->height = breaks.len * lh / scale;
  b->nglyphs = glyphs.len;
  b->glyphs = glyphs.v;
  b->ncarets = carets.len;
  b->carets = carets.v;
  free(caret_x);
  free(adv);
  free(levels);
  vec_free(&breaks);
  vec_free(&runs);
  vec_free(&shaped);
  vec_free(&fonts);
  vec_free(&cps);
  vec_free(&at);
  return b;
}

const civue_text_block *civue_text_layout(civue_text_system *t, const char *s,
                                          usize n, civue_text_style style,
                                          f32 max_width, f32 scale) {
  u64 key = layout_key(s, n, style, max_width, scale);
  u64 at;
  if (map_get(&t->layout_index, key, &at)) {
    Layout *l = &t->layouts.v[at - 1];
    if (l->len == n && !memcmp(l->text, s, n)) {
      l->used = t->frame;
      return l->block;
    }
  }
  civue_text_block *b = build(t, s, n, style, max_width, scale);
  Layout l = {key, xstrndup(s, n), n, b, t->frame};
  if (map_get(&t->layout_index, key, &at)) {
    Layout *old = &t->layouts.v[at - 1];
    free(old->text);
    block_free(old->block);
    *old = l;
  }
  else {
    vec_push(&t->layouts, l);
    map_put(&t->layout_index, key, t->layouts.len);
  }
  return b;
}

void civue_text_end_frame(civue_text_system *t) {
  t->frame++;
  if (t->frame % 30) {
    return;
  }

  // text wrapped to a width that keeps changing leaves a block a frame: past
  // a few thousand, the ones a few frames old go.
  u64 kept = t->layouts.len > 2048 ? 30 : 600;
  // blocks live until the end of the frame after they were last asked.
  u32 k = 0;
  map_clear(&t->layout_index);
  vec_each(&t->layouts, l) {
    if (t->frame - l->used >= kept) {
      free(l->text);
      block_free(l->block);
      continue;
    }
    t->layouts.v[k++] = *l;
    map_put(&t->layout_index, l->key, k);
  }
  t->layouts.len = k;
}

// a bitmap scaled by `k` with a box filter, premultiplied rgba.
static void scale_rgba(const u8 *src, u32 sw, u32 sh, u32 pitch, f32 k,
                       civue_bytes *out, u32 *w, u32 *h) {
  *w = MAX((u32)ceilf(sw * k), 1u);
  *h = MAX((u32)ceilf(sh * k), 1u);
  out->len = 0;
  vec_reserve(out, (usize)*w * *h * 4);
  out->len = (usize)*w * *h * 4;
  for (u32 y = 0; y < *h; y++) {
    for (u32 x = 0; x < *w; x++) {
      f32 x0 = x / k, x1 = (x + 1) / k, y0 = y / k, y1 = (y + 1) / k;
      f32 acc[4] = {}, area = 0;
      for (u32 sy = (u32)y0; sy < MIN((u32)ceilf(y1), sh); sy++) {
        for (u32 sx = (u32)x0; sx < MIN((u32)ceilf(x1), sw); sx++) {
          f32 a = (fminf(x1, sx + 1) - fmaxf(x0, sx)) *
                  (fminf(y1, sy + 1) - fmaxf(y0, sy));
          // freetype's color bitmaps are premultiplied bgra.
          const u8 *p = src + (usize)sy * pitch + sx * 4;
          acc[0] += p[2] * a, acc[1] += p[1] * a, acc[2] += p[0] * a;
          acc[3] += p[3] * a;
          area += a;
        }
      }
      u8 *o = out->v + ((usize)y * *w + x) * 4;
      for (u32 c = 0; c < 4; c++) {
        o[c] = area > 0 ? (u8)fminf(acc[c] / area + 0.5f, 255) : 0;
      }
    }
  }
}

// a glyph's bitmap and where it goes from its pen position, coverage or
// premultiplied rgba for color glyphs, a pixel of room around it.
static bool rasterize(const Font *f, u16 id, u32 subpixel, civue_bytes *out,
                      i32 *left, i32 *top, u32 *w, u32 *h) {
  *w = *h = 0;
  if (!f->ft) {
    return false;
  }
  FT_Vector delta = {(FT_Pos)(subpixel * 64 / SUBPIXELS), 0};
  FT_Set_Transform(f->ft, nullptr, &delta);
  FT_Int32 flags = FT_LOAD_NO_HINTING | FT_LOAD_RENDER;
  if (f->color) {
    flags |= FT_LOAD_COLOR;
  }
  if (FT_Load_Glyph(f->ft, id, flags)) {
    return false;
  }
  FT_GlyphSlot slot = f->ft->glyph;
  FT_Bitmap *bm = &slot->bitmap;
  if (!bm->width || !bm->rows) {
    return true;
  }
  if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
    if (!f->color) {
      return false;
    }
    f32 k = f->strike_scale > 0 ? f->strike_scale : 1;
    civue_bytes scaled = {};
    u32 sw, sh;
    scale_rgba(bm->buffer, bm->width, bm->rows, (u32)abs(bm->pitch), k, &scaled,
               &sw, &sh);
    *w = sw + 2, *h = sh + 2;
    *left = (i32)floorf(slot->bitmap_left * k) - 1;
    *top = -(i32)ceilf(slot->bitmap_top * k) - 1;
    out->len = 0;
    vec_reserve(out, (usize)*w * *h * 4);
    memset(out->v, 0, (usize)*w * *h * 4);
    out->len = (usize)*w * *h * 4;
    for (u32 y = 0; y < sh; y++) {
      memcpy(out->v + ((usize)(y + 1) * *w + 1) * 4,
             scaled.v + (usize)y * sw * 4, (usize)sw * 4);
    }
    vec_free(&scaled);
    return true;
  }
  if (bm->pixel_mode != FT_PIXEL_MODE_GRAY) {
    return false;
  }
  if (f->color) {
    // a plain glyph of a color face, drawn white so the tint is its color.
    *w = bm->width + 2, *h = bm->rows + 2;
    out->len = 0;
    vec_reserve(out, (usize)*w * *h * 4);
    memset(out->v, 0, (usize)*w * *h * 4);
    out->len = (usize)*w * *h * 4;
    for (u32 y = 0; y < bm->rows; y++) {
      for (u32 x = 0; x < bm->width; x++) {
        u8 a = bm->buffer[y * bm->pitch + x];
        memset(out->v + ((usize)(y + 1) * *w + x + 1) * 4, a, 4);
      }
    }
  }
  else {
    *w = bm->width + 2, *h = bm->rows + 2;
    out->len = 0;
    vec_reserve(out, (usize)*w * *h);
    memset(out->v, 0, (usize)*w * *h);
    out->len = (usize)*w * *h;
    for (u32 y = 0; y < bm->rows; y++) {
      memcpy(out->v + (usize)(y + 1) * *w + 1, bm->buffer + y * bm->pitch,
             bm->width);
    }
  }
  *left = slot->bitmap_left - 1;
  *top = -slot->bitmap_top - 1;
  return true;
}

static const GlyphEntry *glyph(civue_text_system *t, civue_atlas *mask,
                               civue_atlas *color, const civue_glyph *g,
                               u32 subpixel) {
  const Font *f = &t->fonts.v[g->font];
  if (f->color) {
    subpixel = 0;
  }
  u64 key = hash_u64((u64)g->font << 24 ^ (u64)g->id << 4 ^ subpixel) | 1;
  u64 at;
  GlyphEntry *e = nullptr;
  if (map_get(&t->glyph_index, key, &at)) {
    e = &t->glyphs.v[at - 1];
    civue_atlas *a = e->color ? color : mask;
    if (e->generation == a->generation) [[clang::likely]] {
      return e;
    }
  }
  civue_bytes px = {};
  defer vec_free(&px);
  i32 left = 0, top = 0;
  u32 w = 0, h = 0;
  if (!rasterize(f, g->id, subpixel, &px, &left, &top, &w, &h)) {
    return nullptr;
  }
  civue_atlas *a = f->color ? color : mask;
  f32 uv[4] = {};
  if (w && h && !civue_atlas_insert(a, w, h, px.v, uv)) {
    // a full atlas starts over, glyphs come back as frames need them.
    civue_atlas_clear(a);
    if (!civue_atlas_insert(a, w, h, px.v, uv)) {
      return nullptr;
    }
  }
  GlyphEntry n = {
    key,          {uv[0], uv[1], uv[2], uv[3]}, f->color, left, top, w, h,
    a->generation};
  if (map_get(&t->glyph_index, key, &at)) {
    t->glyphs.v[at - 1] = n;
    return &t->glyphs.v[at - 1];
  }
  vec_push(&t->glyphs, n);
  map_put(&t->glyph_index, key, t->glyphs.len);
  return &vec_last(&t->glyphs);
}

void civue_text_draw(civue_text_system *t, civue_draw_list *d,
                     civue_atlas *mask, civue_atlas *color,
                     const civue_text_block *b, civue_vec2 origin,
                     civue_color base, const civue_text_span *spans,
                     u32 nspans) {
  f64 s = b->scale;
  f64 ox = origin.x * s, oy = round(origin.y * s);
  for (u32 i = 0; i < b->nglyphs; i++) {
    const civue_glyph *g = &b->glyphs[i];
    civue_color c = base;
    for (u32 k = 0; k < nspans; k++) {
      if (g->byte >= spans[k].start && g->byte < spans[k].end) {
        c = spans[k].color;
        break;
      }
    }
    // light text on dark grounds looks thinner, browsers boost it.
    f32 gamma = 1 - 0.45f * (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b);
    f64 x = ox + g->x, px = floor(x);
    u32 sub = (u32)floor((x - px) * SUBPIXELS) % SUBPIXELS;
    f64 y = round(oy + g->y);
    const GlyphEntry *e = glyph(t, mask, color, g, sub);
    if (!e || !e->w || !e->h) {
      continue;
    }
    civue_rect r = rect_xywh(px + e->left, y + e->top, e->w, e->h);
    if (e->color) {
      civue_dl_color_glyph(d, r, e->uv, c.a);
    }
    else {
      civue_dl_mask(d, r, e->uv, c, gamma);
    }
  }
}
