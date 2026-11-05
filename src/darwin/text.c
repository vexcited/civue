#include "darwin.h"

#include <civue/text.h>

#include <CoreText/CoreText.h>
#include <stdlib.h>

// horizontal positions a glyph is rasterized at within a pixel.
constexpr u32 SUBPIXELS = 4;
constexpr u32 WGHT = 0x77676874;

typedef struct {
  CTFontRef font;
  bool color; // its glyphs are pictures, sbix or colr.
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

typedef struct {
  f32 weight, size;
  u32 flags;
  CTFontRef font;
} MadeFont;

struct civue_text_system {
  // the ui and the code faces, the system's until the app sets its own.
  CTFontDescriptorRef ui, code, display[CIVUE_TEXT_FACES + 1];
  // fonts by index, keyed by their face, size and variation.
  Vec(Font) fonts;
  civue_map font_index; // CFHash of a font to index + 1, CFEqual checked.
  Vec(Layout) layouts;
  civue_map layout_index; // key to index + 1.
  Vec(GlyphEntry) glyphs;
  civue_map glyph_index;
  // fonts layouts are built with, by style and scale: making one costs more
  // than laying out a label.
  Vec(MadeFont) made;
  civue_map made_index;
  u64 frame;
};

static CTFontDescriptorRef system_face(CTFontUIFontType type) {
  CTFontRef font = CTFontCreateUIFontForLanguage(type, 0, nullptr);
  CTFontDescriptorRef d = CTFontCopyFontDescriptor(font);
  CFRelease(font);
  return d;
}

civue_text_system *civue_text_new(void) {
  civue_text_system *t = own_new(sizeof *t);
  t->ui = system_face(kCTFontUIFontSystem);
  t->code = system_face(kCTFontUIFontUserFixedPitch);
  return t;
}

static void forget_layouts(civue_text_system *t) {
  vec_each(&t->made, f) CFRelease(f->font);
  vec_clear(&t->made);
  map_clear(&t->made_index);
  vec_each(&t->layouts, l) {
    free(l->text);
    free(l->block->glyphs);
    free(l->block->carets);
    free(l->block->lines);
    own_free(l->block);
  }
  t->layouts.len = 0;
  map_clear(&t->layout_index);
}

void civue_text_free(civue_text_system *t) {
  if (!t) return;
  forget_layouts(t);
  vec_free(&t->layouts);
  map_free(&t->layout_index);
  vec_free(&t->made);
  map_free(&t->made_index);
  vec_each(&t->fonts, f) CFRelease(f->font);
  vec_free(&t->fonts);
  map_free(&t->font_index);
  vec_free(&t->glyphs);
  map_free(&t->glyph_index);
  CFRelease(t->ui);
  CFRelease(t->code);
  for (u32 i = 0; i <= CIVUE_TEXT_FACES; i++)
    if (t->display[i]) CFRelease(t->display[i]);
  own_free(t);
}

void civue_text_register_display(civue_text_system *t, u8 display, const u8 *data, usize n) {
  if (!display || display > CIVUE_TEXT_FACES) return;
  CFDataRef d = CFDataCreate(nullptr, data, n);
  CTFontDescriptorRef desc = CTFontManagerCreateFontDescriptorFromData(d);
  CFRelease(d);
  if (!desc) return;
  if (t->display[display]) CFRelease(t->display[display]);
  t->display[display] = desc;
  // text laid out before it is laid out again.
  forget_layouts(t);
}

void civue_text_set_face(civue_text_system *t, civue_face face, const u8 *data, usize n) {
  CFDataRef d = CFDataCreate(nullptr, data, n);
  CTFontDescriptorRef desc = CTFontManagerCreateFontDescriptorFromData(d);
  CFRelease(d);
  if (!desc) return;
  CTFontDescriptorRef *slot = face == CIVUE_FACE_CODE ? &t->code : &t->ui;
  CFRelease(*slot);
  *slot = desc;
  forget_layouts(t);
}

// the index of a font, kept for the glyphs drawn with it.
static u32 font_of(civue_text_system *t, CTFontRef font) {
  u64 h = (u64)CFHash(font) | 1;
  for (u64 k = h;; k = hash_u64(k) | 1) {
    u64 at;
    if (!map_get(&t->font_index, k, &at)) {
      CFRetain(font);
      bool color = CTFontGetSymbolicTraits(font) & kCTFontTraitColorGlyphs;
      vec_push(&t->fonts, ((Font){font, color}));
      map_put(&t->font_index, k, t->fonts.len);
      return t->fonts.len - 1;
    }
    if (CFEqual(t->fonts.v[at - 1].font, font)) return at - 1;
  }
}

static CTFontRef make_font(civue_text_system *t, civue_text_style s, f32 scale) {
  CTFontDescriptorRef base = s.mono ? t->code : t->ui;
  if (s.display && t->display[s.display]) base = t->display[s.display];
  CFMutableDictionaryRef attrs =
      CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  i32 tag = WGHT;
  f64 weight = s.weight;
  CFNumberRef tag_n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &tag);
  CFNumberRef weight_n = CFNumberCreate(nullptr, kCFNumberFloat64Type, &weight);
  const void *vk[] = {tag_n}, *vv[] = {weight_n};
  CFDictionaryRef variation =
      CFDictionaryCreate(nullptr, vk, vv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(attrs, kCTFontVariationAttribute, variation);
  if (s.tnum) {
    const void *fk[] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
    i32 one = 1;
    CFNumberRef one_n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &one);
    const void *fv[] = {CFSTR("tnum"), one_n};
    CFDictionaryRef feature =
        CFDictionaryCreate(nullptr, fk, fv, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef features = CFArrayCreate(nullptr, (const void **)&feature, 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(attrs, kCTFontFeatureSettingsAttribute, features);
    CFRelease(features);
    CFRelease(feature);
    CFRelease(one_n);
  }
  if (s.emoji) {
    // emoji first, the ui face for what it lacks.
    CTFontDescriptorRef emoji = CTFontDescriptorCreateWithNameAndSize(CFSTR("AppleColorEmoji"), 0);
    CFArrayRef cascade = CFArrayCreate(nullptr, (const void **)&base, 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(attrs, kCTFontCascadeListAttribute, cascade);
    CFRelease(cascade);
    base = emoji;
  } else {
    CFRetain(base);
  }
  CTFontDescriptorRef d = CTFontDescriptorCreateCopyWithAttributes(base, attrs);
  CTFontRef font = CTFontCreateWithFontDescriptor(d, s.size * scale, nullptr);
  CFRelease(d);
  CFRelease(base);
  CFRelease(attrs);
  CFRelease(variation);
  CFRelease(tag_n);
  CFRelease(weight_n);
  return font;
}

static CTFontRef font_for(civue_text_system *t, civue_text_style s, f32 scale) {
  MadeFont want = {s.weight, s.size * scale, (u32)(s.mono | s.tnum << 1 | s.emoji << 2) | (u32)s.display << 3};
  u32 bits[3] = {want.flags};
  memcpy(bits + 1, &want.weight, 8);
  u64 key = hash_bytes(bits, sizeof bits) | 1;
  for (;; key = hash_u64(key) | 1) {
    u64 at;
    if (!map_get(&t->made_index, key, &at)) break;
    const MadeFont *m = &t->made.v[at - 1];
    if (m->weight == want.weight && m->size == want.size && m->flags == want.flags) return m->font;
  }
  want.font = make_font(t, s, scale);
  vec_push(&t->made, want);
  map_put(&t->made_index, key, t->made.len);
  return want.font;
}

static u64 layout_key(const char *s, usize n, civue_text_style st, f32 max_width, f32 scale) {
  u64 h = hash_bytes(s, n);
  f32 v[] = {st.size, st.weight, st.line_height, st.letter_spacing, max_width, scale};
  h ^= hash_bytes(v, sizeof v) * 0x9e3779b97f4a7c15ull;
  h ^= hash_u64(st.mono | st.tnum << 1 | st.emoji << 2 | (u64)st.display << 3);
  return h | 1;
}

static civue_text_block *build(civue_text_system *t, const char *s, usize n, civue_text_style st, f32 max_width,
                               f32 scale) {
  civue_text_block *b = own_new(sizeof *b);
  b->scale = scale;
  // utf-16 indices to bytes, both halves of a pair at its character.
  civue_u32s bytes16 = {};
  for (usize i = 0; i < n;) {
    usize at = i;
    u32 cp = utf8_next(s, n, &i);
    vec_push(&bytes16, (u32)at);
    if (cp >= 0x10000) vec_push(&bytes16, (u32)at);
  }
  vec_push(&bytes16, (u32)n);
  CFStringRef str = CFStringCreateWithBytes(nullptr, (const u8 *)s, n, kCFStringEncodingUTF8, false);
  if (!str) str = CFSTR("");
  CTFontRef font = font_for(t, st, scale);
  f64 kern = st.letter_spacing * scale;
  CFNumberRef kern_n = CFNumberCreate(nullptr, kCFNumberFloat64Type, &kern);
  const void *ak[] = {kCTFontAttributeName, kCTKernAttributeName};
  const void *av[] = {font, kern_n};
  CFDictionaryRef attrs =
      CFDictionaryCreate(nullptr, ak, av, 2, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
  CFAttributedStringRef astr = CFAttributedStringCreate(nullptr, str, attrs);
  CTTypesetterRef ts = CTTypesetterCreateWithAttributedString(astr);
  CFIndex len16 = CFStringGetLength(str);
  f64 lh = st.line_height * scale;
  f64 ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font);
  // css places the font's box in the middle of the line.
  f64 first = (lh - (ascent + descent)) / 2 + ascent;
  b->baseline = (f32)(first / scale);
  Vec(civue_glyph) glyphs = {};
  Vec(civue_caret) carets = {};
  Vec(civue_text_line) tlines = {};
  f64 width = 0;
  u32 lines = 0;
  CFIndex start = 0;
  do {
    CFIndex count = max_width > 0 ? CTTypesetterSuggestLineBreak(ts, start, max_width * scale) : len16 - start;
    if (count <= 0) count = len16 - start;
    CTLineRef line = CTTypesetterCreateLine(ts, CFRangeMake(start, count));
    f64 y = first + lines * lh;
    f64 w = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr) - CTLineGetTrailingWhitespaceWidth(line);
    width = fmax(width, w);
    CFArrayRef runs = CTLineGetGlyphRuns(line);
    for (CFIndex r = 0; r < CFArrayGetCount(runs); r++) {
      CTRunRef run = CFArrayGetValueAtIndex(runs, r);
      CTFontRef rf = CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
      u32 fi = font_of(t, rf ? rf : font);
      CFIndex ng = CTRunGetGlyphCount(run);
      CGGlyph gs[ng ? ng : 1];
      CGPoint ps[ng ? ng : 1];
      CFIndex is[ng ? ng : 1];
      CTRunGetGlyphs(run, CFRangeMake(0, 0), gs);
      CTRunGetPositions(run, CFRangeMake(0, 0), ps);
      CTRunGetStringIndices(run, CFRangeMake(0, 0), is);
      for (CFIndex g = 0; g < ng; g++) {
        u32 byte = is[g] < bytes16.len ? bytes16.v[is[g]] : (u32)n;
        vec_push(&glyphs, ((civue_glyph){byte, fi, gs[g], (f32)ps[g].x, (f32)(y - ps[g].y)}));
      }
    }
    for (CFIndex k = start; k < start + count; k++) {
      // a stop at each character, not inside surrogate pairs.
      if (k > start && bytes16.v[k] == bytes16.v[k - 1]) continue;
      f64 x = CTLineGetOffsetForStringIndex(line, k, nullptr);
      vec_push(&carets, ((civue_caret){bytes16.v[k], (f32)(x / scale), lines}));
    }
    // the end of a wrapped line stops before its trailing space.
    bool last = start + count >= len16;
    CFIndex end = start + count;
    f64 end_x = CTLineGetOffsetForStringIndex(line, end, nullptr);
    if (last) vec_push(&carets, ((civue_caret){(u32)n, (f32)(end_x / scale), lines}));
    vec_push(&tlines, ((civue_text_line){bytes16.v[start], end < bytes16.len ? bytes16.v[end] : (u32)n,
                                         (f32)(lines * lh / scale), (f32)(y / scale)}));
    CFRelease(line);
    lines++;
    start += count;
  } while (start < len16);
  b->width = (f32)(width / scale);
  b->nlines = tlines.len;
  b->lines = tlines.v;
  b->height = (f32)(lines * lh / scale);
  b->nglyphs = glyphs.len;
  b->glyphs = glyphs.v;
  b->ncarets = carets.len;
  b->carets = carets.v;
  CFRelease(ts);
  CFRelease(astr);
  CFRelease(attrs);
  CFRelease(kern_n);
  CFRelease(str);
  vec_free(&bytes16);
  return b;
}

const civue_text_block *civue_text_layout(civue_text_system *t, const char *s, usize n, civue_text_style style,
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
    free(old->block->glyphs);
    free(old->block->carets);
    free(old->block->lines);
    own_free(old->block);
    *old = l;
  } else {
    vec_push(&t->layouts, l);
    map_put(&t->layout_index, key, t->layouts.len);
  }
  return b;
}

void civue_text_end_frame(civue_text_system *t) {
  t->frame++;
  if (t->frame % 120) return;
  // blocks live until the end of the frame after they were last asked.
  u32 k = 0;
  map_clear(&t->layout_index);
  vec_each(&t->layouts, l) {
    if (t->frame - l->used >= 600) {
      free(l->text);
      free(l->block->glyphs);
      free(l->block->carets);
      free(l->block->lines);
      own_free(l->block);
      continue;
    }
    t->layouts.v[k++] = *l;
    map_put(&t->layout_index, l->key, k);
  }
  t->layouts.len = k;
}

// a glyph's bitmap and where it goes from its pen position, coverage or
// premultiplied rgba for color glyphs.
static bool rasterize(const Font *f, u16 id, u32 subpixel, civue_bytes *out, i32 *left, i32 *top, u32 *w, u32 *h) {
  CGGlyph g = id;
  CGRect r;
  CTFontGetBoundingRectsForGlyphs(f->font, kCTFontOrientationHorizontal, &g, &r, 1);
  f64 dx = (f64)subpixel / SUBPIXELS;
  if (r.size.width <= 0 || r.size.height <= 0) {
    *w = *h = 0;
    return true;
  }
  f64 x0 = floor(r.origin.x + dx) - 1, x1 = ceil(r.origin.x + r.size.width + dx) + 1;
  f64 y0 = floor(r.origin.y) - 1, y1 = ceil(r.origin.y + r.size.height) + 1;
  *w = (u32)(x1 - x0);
  *h = (u32)(y1 - y0);
  *left = (i32)x0;
  *top = (i32)-y1;
  u32 bpp = f->color ? 4 : 1;
  out->len = 0;
  vec_reserve(out, (usize)*w * *h * bpp);
  memset(out->v, 0, (usize)*w * *h * bpp);
  out->len = (usize)*w * *h * bpp;
  CGColorSpaceRef space = f->color ? CGColorSpaceCreateWithName(kCGColorSpaceSRGB) : nullptr;
  CGContextRef ctx =
      CGBitmapContextCreate(out->v, *w, *h, 8, *w * bpp, space,
                            f->color ? (CGBitmapInfo)kCGImageAlphaPremultipliedLast : (CGBitmapInfo)kCGImageAlphaOnly);
  if (space) CGColorSpaceRelease(space);
  if (!ctx) return false;
  CGContextSetShouldAntialias(ctx, true);
  CGContextSetShouldSmoothFonts(ctx, false);
  CGContextSetShouldSubpixelPositionFonts(ctx, true);
  CGContextSetShouldSubpixelQuantizeFonts(ctx, false);
  CGContextSetGrayFillColor(ctx, 1, 1);
  CGPoint at = {dx - x0, -y0};
  CTFontDrawGlyphs(f->font, &g, &at, 1, ctx);
  CGContextRelease(ctx);
  return true;
}

static const GlyphEntry *glyph(civue_text_system *t, civue_atlas *mask, civue_atlas *color, const civue_glyph *g,
                               u32 subpixel) {
  const Font *f = &t->fonts.v[g->font];
  if (f->color) subpixel = 0;
  u64 key = hash_u64((u64)g->font << 24 ^ (u64)g->id << 4 ^ subpixel) | 1;
  u64 at;
  GlyphEntry *e = nullptr;
  if (map_get(&t->glyph_index, key, &at)) {
    e = &t->glyphs.v[at - 1];
    civue_atlas *a = e->color ? color : mask;
    if (e->generation == a->generation) return e;
  }
  civue_bytes px = {};
  i32 left = 0, top = 0;
  u32 w = 0, h = 0;
  if (!rasterize(f, g->id, subpixel, &px, &left, &top, &w, &h)) {
    vec_free(&px);
    return nullptr;
  }
  civue_atlas *a = f->color ? color : mask;
  f32 uv[4];
  if (!civue_atlas_insert(a, w, h, px.v, uv)) {
    // a full atlas starts over, glyphs come back as frames need them.
    civue_atlas_clear(a);
    if (!civue_atlas_insert(a, w, h, px.v, uv)) {
      vec_free(&px);
      return nullptr;
    }
  }
  vec_free(&px);
  GlyphEntry n = {key, {uv[0], uv[1], uv[2], uv[3]}, f->color, left, top, w, h, a->generation};
  if (map_get(&t->glyph_index, key, &at)) {
    t->glyphs.v[at - 1] = n;
    return &t->glyphs.v[at - 1];
  }
  vec_push(&t->glyphs, n);
  map_put(&t->glyph_index, key, t->glyphs.len);
  return &vec_last(&t->glyphs);
}

void civue_text_draw(civue_text_system *t, civue_draw_list *d, civue_atlas *mask, civue_atlas *color,
                     const civue_text_block *b, civue_vec2 origin, civue_color base, const civue_text_span *spans,
                     u32 nspans) {
  f64 s = b->scale;
  f64 ox = origin.x * s, oy = round(origin.y * s);
  for (u32 i = 0; i < b->nglyphs; i++) {
    const civue_glyph *g = &b->glyphs[i];
    civue_color c = base;
    for (u32 k = 0; k < nspans; k++)
      if (g->byte >= spans[k].start && g->byte < spans[k].end) {
        c = spans[k].color;
        break;
      }
    // light text on dark grounds looks thinner, browsers boost it.
    f32 gamma = 1 - 0.45f * (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b);
    f64 x = ox + g->x, px = floor(x);
    u32 sub = (u32)floor((x - px) * SUBPIXELS) % SUBPIXELS;
    f64 y = round(oy + g->y);
    const GlyphEntry *e = glyph(t, mask, color, g, sub);
    if (!e || !e->w || !e->h) continue;
    civue_rect r = rect_xywh(px + e->left, y + e->top, e->w, e->h);
    if (e->color)
      civue_dl_color_glyph(d, r, e->uv, c.a);
    else
      civue_dl_mask(d, r, e->uv, c, gamma);
  }
}
