#include "../internal.h"

#include <civue/text.h>

#include <CoreText/CoreText.h>
#include <dlfcn.h>
#include <stddefer.h>
#include <stdlib.h>

// horizontal positions a glyph is rasterized at within a pixel.
constexpr u32 SUBPIXELS = 4;
constexpr u32 WGHT = 0x77676874;

constexpr u32 ASCII_FIRST = 0x20;
constexpr u32 ASCII_COUNT = 0x7f - ASCII_FIRST;

// what a font gives printable ascii before shaping, asked once.
typedef struct {
  CGGlyph glyph[ASCII_COUNT];
  CGSize advance[ASCII_COUNT];
  // every character has a glyph of the font's own.
  bool whole;
  // shaping its glyphs apart from a line was seen to give something else
  // than the line: lines shape them from then on.
  bool differs;
  f64 ascent, descent;
  // how many texts were shaped apart from a line.
  u32 shaped;
} Ascii;

// the glyphs with the lowest ids of a font are the ones most text is set
// in: their entries are found by id, without hashing.
constexpr u32 NEAR_GLYPHS = 2048;

typedef struct {
  CTFontRef font;
  bool color; // its glyphs are pictures, sbix or colr.
  Ascii *ascii;
  // a glyph's entry + 1 at each subpixel position, 0 for none yet.
  u32 *near;
} Font;

// a text's typesetter and how many utf-16 units it set.
typedef struct {
  CTTypesetterRef ts;
  CFIndex len;
} Shaped;

typedef struct {
  u64 key;
  char *text;
  usize len;
  civue_text_block *block;
  u64 used;
  // the text shaped, kept once it is wrapped: wrapping it to another width
  // only breaks its lines again.
  Shaped shaped;
  // ascii on one line, a stop for each character: its lines when wrapped
  // are cut from this one.
  bool plain;
  bool wrapped;
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
  // the attributes text was last set with, the same from a label to the
  // next.
  CTFontRef attrs_font;
  f64 attrs_kern;
  CFDictionaryRef attrs;
  // what a block is gathered in before it is copied into one allocation.
  Vec(civue_glyph) glyphs_made;
  Vec(civue_caret) carets_made;
  Vec(civue_text_line) lines_made;
  Vec(f64) stops;
  civue_u32s bytes16;
  Vec(civue_glyph_quad) quads;
  // what core text shapes apart from a line.
  Vec(UniChar) shape_chars;
  Vec(CGGlyph) shape_glyphs;
  Vec(CGSize) shape_advances;
  Vec(CGPoint) shape_origins;
  Vec(CFIndex) shape_indices;
  // bytes of wrapped blocks made since the last sweep.
  usize churn;
  civue_map made_index;
  u64 frame;
};

static CTFontDescriptorRef system_face(CTFontUIFontType type) {
  CTFontRef font = CTFontCreateUIFontForLanguage(type, 0, nullptr);
  CTFontDescriptorRef d = CTFontCopyFontDescriptor(font);
  CFRelease(font);
  return d;
}

civue_text_system *civue_text_new() {
  civue_text_system *t = own_new(sizeof *t);
  t->ui = system_face(kCTFontUIFontSystem);
  t->code = system_face(kCTFontUIFontUserFixedPitch);
  return t;
}

static void layout_free(Layout *l) {
  // its typesetter reads the text in place.
  if (l->shaped.ts) {
    CFRelease(l->shaped.ts);
  }
  free(l->text);
  free(l->block->cache);
  free(l->block);
}

// a block with its glyphs, stops and lines after it, in one allocation.
static civue_text_block *block_of(civue_text_system *t) {
  usize ng = t->glyphs_made.len * sizeof(civue_glyph);
  usize nc = t->carets_made.len * sizeof(civue_caret);
  usize nl = t->lines_made.len * sizeof(civue_text_line);
  civue_text_block *b = xmalloc(sizeof *b + ng + nc + nl);
  *b = (civue_text_block){};

  u8 *at = (u8 *)(b + 1);
  b->glyphs = memcpy(at, t->glyphs_made.v, ng);
  b->nglyphs = t->glyphs_made.len;
  at += ng;
  b->carets = memcpy(at, t->carets_made.v, nc);
  b->ncarets = t->carets_made.len;
  at += nc;
  b->lines = memcpy(at, t->lines_made.v, nl);
  b->nlines = t->lines_made.len;
  return b;
}

static void forget_layouts(civue_text_system *t) {
  if (t->attrs) {
    CFRelease(t->attrs);
    t->attrs = nullptr;
    t->attrs_font = nullptr;
  }

  vec_each(&t->made, f) {
    CFRelease(f->font);
  }
  vec_clear(&t->made);
  map_clear(&t->made_index);
  vec_each(&t->layouts, l) {
    layout_free(l);
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
  vec_free(&t->made);
  map_free(&t->made_index);
  vec_each(&t->fonts, f) {
    CFRelease(f->font);
    free(f->ascii);
    free(f->near);
  }
  vec_free(&t->fonts);
  vec_free(&t->shape_chars);
  vec_free(&t->shape_glyphs);
  vec_free(&t->shape_advances);
  vec_free(&t->shape_origins);
  vec_free(&t->shape_indices);
  map_free(&t->font_index);
  vec_free(&t->glyphs);
  map_free(&t->glyph_index);
  vec_free(&t->glyphs_made);
  vec_free(&t->carets_made);
  vec_free(&t->lines_made);
  vec_free(&t->stops);
  vec_free(&t->bytes16);
  vec_free(&t->quads);
  CFRelease(t->ui);
  CFRelease(t->code);
  for (u32 i = 0; i <= CIVUE_TEXT_FACES; i++) {
    if (t->display[i]) {
      CFRelease(t->display[i]);
    }
  }
  own_free(t);
}

void civue_text_register_display(civue_text_system *t, u8 display,
                                 const u8 *data, usize n) {
  if (!display || display > CIVUE_TEXT_FACES) {
    return;
  }
  CFDataRef d = CFDataCreate(nullptr, data, n);
  CTFontDescriptorRef desc = CTFontManagerCreateFontDescriptorFromData(d);
  CFRelease(d);
  if (!desc) {
    return;
  }
  if (t->display[display]) {
    CFRelease(t->display[display]);
  }
  t->display[display] = desc;
  // text laid out before it is laid out again.
  forget_layouts(t);
}

void civue_text_set_face(civue_text_system *t, civue_face face, const u8 *data,
                         usize n) {
  CFDataRef d = CFDataCreate(nullptr, data, n);
  CTFontDescriptorRef desc = CTFontManagerCreateFontDescriptorFromData(d);
  CFRelease(d);
  if (!desc) {
    return;
  }
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
    if (CFEqual(t->fonts.v[at - 1].font, font)) {
      return at - 1;
    }
  }
}

static CTFontRef make_font(civue_text_system *t, civue_text_style s,
                           f32 scale) {
  CTFontDescriptorRef base = s.mono ? t->code : t->ui;
  if (s.display && t->display[s.display]) {
    base = t->display[s.display];
  }
  CFMutableDictionaryRef attrs =
    CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                              &kCFTypeDictionaryValueCallBacks);
  i32 tag = WGHT;
  f64 weight = s.weight;
  CFNumberRef tag_n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &tag);
  CFNumberRef weight_n = CFNumberCreate(nullptr, kCFNumberFloat64Type, &weight);
  const void *vk[] = {tag_n}, *vv[] = {weight_n};
  CFDictionaryRef variation =
    CFDictionaryCreate(nullptr, vk, vv, 1, &kCFTypeDictionaryKeyCallBacks,
                       &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(attrs, kCTFontVariationAttribute, variation);
  if (s.tnum) {
    const void *fk[] = {kCTFontOpenTypeFeatureTag, kCTFontOpenTypeFeatureValue};
    i32 one = 1;
    CFNumberRef one_n = CFNumberCreate(nullptr, kCFNumberSInt32Type, &one);
    const void *fv[] = {CFSTR("tnum"), one_n};
    CFDictionaryRef feature =
      CFDictionaryCreate(nullptr, fk, fv, 2, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
    CFArrayRef features = CFArrayCreate(nullptr, (const void **)&feature, 1,
                                        &kCFTypeArrayCallBacks);
    CFDictionarySetValue(attrs, kCTFontFeatureSettingsAttribute, features);
    CFRelease(features);
    CFRelease(feature);
    CFRelease(one_n);
  }
  if (s.emoji) {
    // emoji first, the ui face for what it lacks.
    CTFontDescriptorRef emoji =
      CTFontDescriptorCreateWithNameAndSize(CFSTR("AppleColorEmoji"), 0);
    CFArrayRef cascade =
      CFArrayCreate(nullptr, (const void **)&base, 1, &kCFTypeArrayCallBacks);
    CFDictionarySetValue(attrs, kCTFontCascadeListAttribute, cascade);
    CFRelease(cascade);
    base = emoji;
  }
  else {
    CFRetain(base);
  }
  CTFontDescriptorRef d = CTFontDescriptorCreateCopyWithAttributes(base, attrs);
  if (s.italic) {
    CTFontDescriptorRef slanted = CTFontDescriptorCreateCopyWithSymbolicTraits(
      d, kCTFontItalicTrait, kCTFontItalicTrait);
    if (slanted) {
      CFRelease(d);
      d = slanted;
    }
  }
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
  MadeFont want = {s.weight, s.size * scale,
                   (u32)(s.mono | s.tnum << 1 | s.emoji << 2 | s.italic << 8) |
                     (u32)s.display << 3};
  u32 bits[3] = {want.flags};
  memcpy(bits + 1, &want.weight, 8);
  u64 key = hash_bytes(bits, sizeof bits) | 1;
  for (;; key = hash_u64(key) | 1) {
    u64 at;
    if (!map_get(&t->made_index, key, &at)) {
      break;
    }
    const MadeFont *m = &t->made.v[at - 1];
    if (m->weight == want.weight && m->size == want.size &&
        m->flags == want.flags) {
      return m->font;
    }
  }
  want.font = make_font(t, s, scale);
  vec_push(&t->made, want);
  map_put(&t->made_index, key, t->made.len);
  return want.font;
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

// whether the character at utf-16 index `k` is ascii and so is the next: one
// glyph of its own, with nothing joining it.
static bool plain(const char *s, usize n, const civue_u32s *bytes16,
                  CFIndex k) {
  u32 at = bytes16->v[k], next = bytes16->v[k + 1];
  return (u8)s[at] < 0x80 && (next >= n || (u8)s[next] < 0x80);
}

// `s` with the attributes of its style, read in place: it must outlive the
// string and what is made of it.
static CFAttributedStringRef attributed(civue_text_system *t, const char *s,
                                        usize n, civue_text_style st, f32 scale,
                                        CFIndex *len16) {
  CTFontRef font = font_for(t, st, scale);
  f64 kern = st.letter_spacing * scale;
  if (!t->attrs || t->attrs_font != font || t->attrs_kern != kern) {
    if (t->attrs) {
      CFRelease(t->attrs);
    }
    CFNumberRef kern_n = CFNumberCreate(nullptr, kCFNumberFloat64Type, &kern);
    const void *ak[] = {kCTFontAttributeName, kCTKernAttributeName};
    const void *av[] = {font, kern_n};
    t->attrs =
      CFDictionaryCreate(nullptr, ak, av, 2, &kCFTypeDictionaryKeyCallBacks,
                         &kCFTypeDictionaryValueCallBacks);
    t->attrs_font = font;
    t->attrs_kern = kern;
    CFRelease(kern_n);
  }

  CFStringRef str = CFStringCreateWithBytesNoCopy(
    nullptr, (const u8 *)s, n, kCFStringEncodingUTF8, false, kCFAllocatorNull);
  if (!str) {
    str = CFSTR("");
  }
  *len16 = CFStringGetLength(str);
  CFAttributedStringRef astr = CFAttributedStringCreate(nullptr, str, t->attrs);
  CFRelease(str);
  return astr;
}

// shapes `s`, which the typesetter reads in place.
static Shaped typeset(civue_text_system *t, const char *s, usize n,
                      civue_text_style st, f32 scale) {
  Shaped shaped = {};
  CFAttributedStringRef astr = attributed(t, s, n, st, scale, &shaped.len);
  shaped.ts = CTTypesetterCreateWithAttributedString(astr);
  CFRelease(astr);
  return shaped;
}

// lays `s` out from its typesetter, or on one line when it has none, and
// says whether it is ascii that goes from left to right on lines it breaks by
// itself.
static civue_text_block *build(civue_text_system *t, const char *s, usize n,
                               civue_text_style st, f32 max_width, f32 scale,
                               const Shaped *shaped, bool *is_latin) {
  // utf-16 indices to bytes, both halves of a pair at its character.
  civue_u32s *bytes16 = &t->bytes16;
  bytes16->len = 0;
  vec_reserve(bytes16, (u32)n + 1);
  bool latin = true;
  for (usize i = 0; i < n;) {
    usize at = i;
    u32 cp = utf8_next(s, n, &i);
    latin &= cp >= 0x20 && cp < 0x7f;
    vec_push(bytes16, (u32)at);
    if (cp >= 0x10000) {
      vec_push(bytes16, (u32)at);
    }
  }
  vec_push(bytes16, (u32)n);
  *is_latin = latin;

  CTFontRef font = font_for(t, st, scale);
  CTTypesetterRef ts = shaped ? shaped->ts : nullptr;
  CTLineRef only = nullptr;
  CFIndex len16 = shaped ? shaped->len : 0;
  if (!ts) {
    CFAttributedStringRef astr = attributed(t, s, n, st, scale, &len16);
    only = CTLineCreateWithAttributedString(astr);
    CFRelease(astr);
  }

  f64 lh = st.line_height * scale;
  f64 ascent = CTFontGetAscent(font), descent = CTFontGetDescent(font);
  // css places the font's box in the middle of the line.
  f64 first = (lh - (ascent + descent)) / 2 + ascent;
  t->glyphs_made.len = t->carets_made.len = t->lines_made.len = 0;
  f64 width = 0;
  u32 lines = 0;
  CFIndex start = 0;
  do {
    CFIndex count =
      ts && max_width > 0
        ? CTTypesetterSuggestLineBreak(ts, start, max_width * scale)
        : len16 - start;
    if (count <= 0) {
      count = len16 - start;
    }
    CTLineRef line =
      only ? only : CTTypesetterCreateLine(ts, CFRangeMake(start, count));
    f64 y = first + lines * lh;
    f64 whole = CTLineGetTypographicBounds(line, nullptr, nullptr, nullptr);
    f64 w = whole - CTLineGetTrailingWhitespaceWidth(line);
    width = fmax(width, w);
    // a stop for each character of the line, taken from where its glyph is
    // when the text is plain: asking the line walks it again each time.
    t->stops.len = 0;
    vec_reserve(&t->stops, (u32)count + 1);
    f64 *xs = t->stops.v;
    for (CFIndex k = 0; k < count; k++) {
      xs[k] = NAN;
    }

    CFArrayRef runs = CTLineGetGlyphRuns(line);
    CFIndex nruns = CFArrayGetCount(runs);
    for (CFIndex r = 0; r < nruns; r++) {
      CTRunRef run = CFArrayGetValueAtIndex(runs, r);
      CTFontRef rf =
        CFDictionaryGetValue(CTRunGetAttributes(run), kCTFontAttributeName);
      u32 fi = font_of(t, rf ? rf : font);
      CFIndex ng = CTRunGetGlyphCount(run);
      CGGlyph gs[ng ? ng : 1];
      CGPoint ps[ng ? ng : 1];
      CFIndex is[ng ? ng : 1];
      CTRunGetGlyphs(run, CFRangeMake(0, 0), gs);
      CTRunGetPositions(run, CFRangeMake(0, 0), ps);
      CTRunGetStringIndices(run, CFRangeMake(0, 0), is);
      bool forward = !(CTRunGetStatus(run) & kCTRunStatusRightToLeft);
      vec_reserve(&t->glyphs_made, (u32)ng);
      for (CFIndex g = 0; g < ng; g++) {
        u32 byte = is[g] < bytes16->len ? bytes16->v[is[g]] : (u32)n;
        t->glyphs_made.v[t->glyphs_made.len++] =
          (civue_glyph){byte, fi, gs[g], (f32)ps[g].x, (f32)(y - ps[g].y)};

        CFIndex k = is[g] - start;
        if (forward && k >= 0 && k < count &&
            (latin || plain(s, n, bytes16, is[g])) && !(ps[g].x >= xs[k])) {
          xs[k] = ps[g].x;
        }
      }
    }

    u32 unknown = 0;
    for (CFIndex k = 0; k < count; k++) {
      unknown += isnan(xs[k]);
    }

    // a line of another script is asked for all of its stops in one pass.
    if (unknown > 8) {
      CTLineEnumerateCaretOffsets(
        line, ^(double offset, CFIndex at, bool leading, bool *) {
          if (leading && at >= start && at < start + count) {
            xs[at - start] = offset;
          }
        });
    }

    vec_reserve(&t->carets_made, (u32)count + 1);
    for (CFIndex k = start; k < start + count; k++) {
      // a stop at each character, not inside surrogate pairs.
      if (k > start && bytes16->v[k] == bytes16->v[k - 1]) {
        continue;
      }
      f64 x = xs[k - start];
      if (isnan(x)) {
        x = CTLineGetOffsetForStringIndex(line, k, nullptr);
      }
      t->carets_made.v[t->carets_made.len++] =
        (civue_caret){bytes16->v[k], (f32)(x / scale), lines};
    }
    // the end of a wrapped line stops before its trailing space.
    bool last = start + count >= len16;
    CFIndex end = start + count;
    if (last) {
      // a line of ascii alone ends where it is wide.
      f64 end_x =
        latin ? whole : CTLineGetOffsetForStringIndex(line, end, nullptr);
      t->carets_made.v[t->carets_made.len++] =
        (civue_caret){(u32)n, (f32)(end_x / scale), lines};
    }
    vec_push(&t->lines_made,
             ((civue_text_line){bytes16->v[start],
                                end < bytes16->len ? bytes16->v[end] : (u32)n,
                                (f32)(lines * lh / scale), (f32)(y / scale)}));
    CFRelease(line);
    lines++;
    start += count;
  } while (start < len16);

  civue_text_block *b = block_of(t);
  b->scale = scale;
  b->baseline = (f32)(first / scale);
  b->width = (f32)(width / scale);
  b->height = (f32)(lines * lh / scale);
  return b;
}

// core text's shaping of glyphs it is given, the call its lines go through.
// private, so it is looked up, and lines shape everything when it is gone.
typedef CGSize (*ShapeGlyphs)(
  CTFontRef font, CGGlyph glyphs[], CGSize advances[], CGPoint origins[],
  CFIndex indices[], const UniChar chars[], CFIndex count,
  CFOptionFlags options, CFStringRef language,
  void (^moved)(CFRange range, CGGlyph **glyphs, CGSize **advances,
                CGPoint **origins, CFIndex **indices));

static ShapeGlyphs shape_glyphs() {
  static ShapeGlyphs shape;
  static bool looked;

  if (!looked) {
    looked = true;
    shape = (ShapeGlyphs)dlsym(RTLD_DEFAULT, "CTFontShapeGlyphs");
  }

  return shape;
}

static Ascii *ascii_of(civue_text_system *t, u32 index) {
  Font *f = &t->fonts.v[index];

  if (f->ascii) {
    return f->ascii;
  }

  Ascii *a = xcalloc(1, sizeof *a);
  UniChar chars[ASCII_COUNT];

  for (u32 i = 0; i < ASCII_COUNT; i++) {
    chars[i] = (UniChar)(ASCII_FIRST + i);
  }

  a->whole =
    CTFontGetGlyphsForCharacters(f->font, chars, a->glyph, ASCII_COUNT);
  CTFontGetAdvancesForGlyphs(f->font, kCTFontOrientationHorizontal, a->glyph,
                             a->advance, ASCII_COUNT);
  a->ascent = CTFontGetAscent(f->font);
  a->descent = CTFontGetDescent(f->font);
  f->ascii = a;
  return a;
}

// room for `n` more in each of what is shaped apart, at `at`, or less room
// when `n` is negative.
static void shape_move(civue_text_system *t, CFIndex at, CFIndex n) {
  u32 len = t->shape_glyphs.len;
  u32 from = (u32)CLAMP(at, 0, (CFIndex)len);

  if (n < 0) {
    u32 gone = (u32)MIN((CFIndex)from, -n);
    from -= gone;

#define SHAPE_CUT(a)                            \
  memmove(&(a)->v[from], &(a)->v[from + gone],  \
          sizeof *(a)->v *(len - from - gone)), \
    (a)->len = len - gone

    SHAPE_CUT(&t->shape_glyphs);
    SHAPE_CUT(&t->shape_advances);
    SHAPE_CUT(&t->shape_origins);
    SHAPE_CUT(&t->shape_indices);
#undef SHAPE_CUT
    return;
  }

  u32 more = (u32)n;

#define SHAPE_HOLE(a)                            \
  vec_reserve(a, more),                          \
    memmove(&(a)->v[from + more], &(a)->v[from], \
            sizeof *(a)->v *(len - from)),       \
    memset(&(a)->v[from], 0, sizeof *(a)->v *more), (a)->len = len + more

  SHAPE_HOLE(&t->shape_glyphs);
  SHAPE_HOLE(&t->shape_advances);
  SHAPE_HOLE(&t->shape_origins);
  SHAPE_HOLE(&t->shape_indices);
#undef SHAPE_HOLE
}

// a line of printable ascii without making a line: the font's glyphs and
// advances, kept, then shaped by core text as a line's are. nullptr when
// that cannot tell what a line would.
static civue_text_block *shape_ascii(civue_text_system *t, const char *s,
                                     usize n, civue_text_style st, f32 scale) {
  ShapeGlyphs shape = shape_glyphs();

  // spacing between letters is the line's to add.
  if (!shape || !n || st.letter_spacing != 0) {
    return nullptr;
  }

  for (usize i = 0; i < n; i++) {
    if ((u8)s[i] < ASCII_FIRST || (u8)s[i] >= ASCII_FIRST + ASCII_COUNT) {
      return nullptr;
    }
  }

  CTFontRef font = font_for(t, st, scale);
  u32 fi = font_of(t, font);
  Ascii *a = ascii_of(t, fi);

  if (!a->whole || a->differs) {
    return nullptr;
  }

  u32 count = (u32)n;
  t->shape_chars.len = t->shape_glyphs.len = t->shape_advances.len = 0;
  t->shape_origins.len = t->shape_indices.len = 0;
  vec_reserve(&t->shape_chars, count);
  vec_reserve(&t->shape_glyphs, count);
  vec_reserve(&t->shape_advances, count);
  vec_reserve(&t->shape_origins, count);
  vec_reserve(&t->shape_indices, count);

  for (u32 i = 0; i < count; i++) {
    u32 c = (u8)s[i] - ASCII_FIRST;

    t->shape_chars.v[i] = (UniChar)(u8)s[i];
    t->shape_glyphs.v[i] = a->glyph[c];
    t->shape_advances.v[i] = a->advance[c];
    t->shape_origins.v[i] = CGPointZero;
    t->shape_indices.v[i] = i;
  }

  t->shape_chars.len = t->shape_glyphs.len = t->shape_advances.len = count;
  t->shape_origins.len = t->shape_indices.len = count;

  // glyphs joined or split are given room for, and left to a line.
  __block bool moved = false;
  // without kerning, as a line set with a kern of zero is.
  CGSize initial =
    shape(font, t->shape_glyphs.v, t->shape_advances.v, t->shape_origins.v,
          t->shape_indices.v, t->shape_chars.v, count, 0, nullptr,
          ^(CFRange range, CGGlyph **glyphs, CGSize **advances,
            CGPoint **origins, CFIndex **indices) {
            moved = true;
            shape_move(t, range.location, range.length);
            *glyphs = t->shape_glyphs.v;
            *advances = t->shape_advances.v;
            *origins = t->shape_origins.v;
            *indices = t->shape_indices.v;
          });

  if (moved || t->shape_glyphs.len != count) {
    return nullptr;
  }

  f64 lh = st.line_height * scale;
  f64 first = (lh - (a->ascent + a->descent)) / 2 + a->ascent;
  t->glyphs_made.len = t->carets_made.len = t->lines_made.len = 0;
  vec_reserve(&t->glyphs_made, count);
  vec_reserve(&t->carets_made, count + 1);
  t->stops.len = 0;
  vec_reserve(&t->stops, count);

  f64 *xs = t->stops.v;
  f64 x = initial.width;
  // what the spaces the line ends with take.
  f64 trailing = 0;

  for (u32 i = 0; i < count; i++) {
    xs[i] = NAN;
  }

  for (u32 g = 0; g < count; g++) {
    CFIndex k = t->shape_indices.v[g];
    f64 gx = x + t->shape_origins.v[g].x;

    if (k < 0 || k >= (CFIndex)count) {
      return nullptr;
    }

    t->glyphs_made.v[t->glyphs_made.len++] =
      (civue_glyph){(u32)k, fi, t->shape_glyphs.v[g], (f32)gx,
                    (f32)(first - t->shape_origins.v[g].y)};

    if (!(gx >= xs[k])) {
      xs[k] = gx;
    }

    f64 advance = t->shape_advances.v[g].width;
    trailing = s[k] == ' ' ? trailing + advance : 0;
    x += advance;
  }

  for (u32 k = 0; k < count; k++) {
    // a character without a glyph of its own stops where a line says.
    if (isnan(xs[k])) {
      return nullptr;
    }

    t->carets_made.v[t->carets_made.len++] =
      (civue_caret){k, (f32)(xs[k] / scale), 0};
  }

  t->carets_made.v[t->carets_made.len++] =
    (civue_caret){count, (f32)(x / scale), 0};
  vec_push(&t->lines_made,
           ((civue_text_line){0, count, 0, (f32)(first / scale)}));

  civue_text_block *b = block_of(t);
  b->scale = scale;
  b->baseline = (f32)(first / scale);
  b->width = (f32)((x - trailing) / scale);
  b->height = (f32)(lh / scale);
  a->shaped++;
  return b;
}

static bool same_block(const civue_text_block *a, const civue_text_block *b) {
  if (a->nglyphs != b->nglyphs || a->ncarets != b->ncarets ||
      a->nlines != b->nlines || a->width != b->width ||
      a->height != b->height || a->baseline != b->baseline ||
      a->scale != b->scale) {
    return false;
  }

  for (u32 i = 0; i < a->nglyphs; i++) {
    const civue_glyph *x = &a->glyphs[i], *y = &b->glyphs[i];

    if (x->byte != y->byte || x->font != y->font || x->id != y->id ||
        x->x != y->x || x->y != y->y) {
      return false;
    }
  }

  for (u32 i = 0; i < a->ncarets; i++) {
    const civue_caret *x = &a->carets[i], *y = &b->carets[i];

    if (x->byte != y->byte || x->x != y->x || x->line != y->line) {
      return false;
    }
  }

  for (u32 i = 0; i < a->nlines; i++) {
    const civue_text_line *x = &a->lines[i], *y = &b->lines[i];

    if (x->start != y->start || x->end != y->end || x->top != y->top ||
        x->baseline != y->baseline) {
      return false;
    }
  }

  return true;
}

// how often what is shaped apart from a line is held against the line: the
// first texts of a font, then one in this many.
constexpr u32 CHECKED_FIRST = 32;
constexpr u32 CHECKED_EVERY = 256;

// one line of text, shaped apart from a line when that is known to give
// the same.
static civue_text_block *one_line(civue_text_system *t, const char *s, usize n,
                                  civue_text_style st, f32 scale,
                                  bool *is_latin) {
  civue_text_block *b = shape_ascii(t, s, n, st, scale);

  if (!b) {
    return build(t, s, n, st, 0, scale, nullptr, is_latin);
  }

  *is_latin = true;

  Ascii *a = ascii_of(t, font_of(t, font_for(t, st, scale)));

  if (a->shaped > CHECKED_FIRST && a->shaped % CHECKED_EVERY) {
    return b;
  }

  // a private call may change with the system: the line has the last
  // word, and takes over for the font once they differ.
  bool latin;
  civue_text_block *line = build(t, s, n, st, 0, scale, nullptr, &latin);

  if (same_block(b, line)) {
    free(line);
    return b;
  }

  a->differs = true;
  LOG("civue: shaping apart from lines differs, lines shape this font");
  free(b);
  return line;
}

// whether a word of these alone has nowhere to break but at its spaces.
static bool unbroken(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '.' || c == ',' || c == ';' ||
         c == ':' || c == '\'' || c == '"';
}

// how many characters from `start` go on a line `limit` points wide, as the
// typesetter breaks it, found from the stops of the text on one line when
// the word that does not fit breaks nowhere but at its spaces. 0 when the
// typesetter must be asked.
static CFIndex fitting(const civue_text_block *full, const char *s, CFIndex len,
                       CFIndex start, f32 limit) {
  // near the limit the typesetter's own sum decides.
  constexpr f32 MARGIN = .02f;
  f32 left = full->carets[start].x;
  CFIndex fit = 0;

  for (CFIndex at = start; at < len;) {
    CFIndex word = at;
    while (word < len && s[word] != ' ') {
      if (!unbroken(s[word])) {
        return 0;
      }
      word++;
    }

    f32 right = (word < len ? full->carets[word].x : full->width) - left;
    if (fabsf(right - limit) < MARGIN) {
      return 0;
    }

    if (right > limit) {
      return fit;
    }

    // the spaces after a word stay on its line.
    while (word < len && s[word] == ' ') {
      word++;
    }
    fit = word - start;
    at = word;
  }

  return len - start;
}

// wraps ascii set on one line by cutting that line where the typesetter
// breaks it: no line is set again. null when a line breaks inside a word.
static civue_text_block *rewrap(civue_text_system *t, const Layout *w,
                                civue_text_style st, f32 max_width, f32 scale) {
  const civue_text_block *full = w->block;
  const char *s = w->text;
  CFIndex len = (CFIndex)w->len;
  f64 lh = st.line_height * scale;
  t->glyphs_made.len = t->carets_made.len = t->lines_made.len = 0;
  vec_reserve(&t->glyphs_made, full->nglyphs);
  vec_reserve(&t->carets_made, full->ncarets);
  f32 width = 0;
  u32 lines = 0, g = 0;

  for (CFIndex start = 0; start < len; lines++) {
    CFIndex count = fitting(full, s, len, start, max_width);
    if (count <= 0) {
      count =
        CTTypesetterSuggestLineBreak(w->shaped.ts, start, max_width * scale);
    }
    if (count <= 0) {
      count = len - start;
    }

    // a line broken inside a word may be set with other glyphs than the
    // word whole, a colon between digits for one.
    CFIndex end = start + count;
    bool inside = end < len && s[end - 1] != ' ';
    if (inside || g >= full->nglyphs || full->glyphs[g].byte != (u32)start) {
      return nullptr;
    }

    f32 x0 = full->glyphs[g].x, dy = (f32)(lines * lh);
    for (; g < full->nglyphs && full->glyphs[g].byte < (u32)end; g++) {
      civue_glyph moved = full->glyphs[g];
      moved.x -= x0;
      moved.y += dy;
      t->glyphs_made.v[t->glyphs_made.len++] = moved;
    }

    // as wide as what comes before its trailing spaces.
    CFIndex shown = end;
    while (shown > start && s[shown - 1] == ' ') {
      shown--;
    }

    f32 left = full->carets[start].x;
    f32 right = shown < len ? full->carets[shown].x : full->width;
    width = fmaxf(width, right - left);

    for (CFIndex k = start; k < end; k++) {
      t->carets_made.v[t->carets_made.len++] =
        (civue_caret){(u32)k, full->carets[k].x - left, lines};
    }

    if (end >= len) {
      t->carets_made.v[t->carets_made.len++] =
        (civue_caret){(u32)len, full->carets[len].x - left, lines};
    }

    f32 top = (f32)(lines * lh / scale);
    vec_push(&t->lines_made,
             ((civue_text_line){(u32)start, (u32)end, top,
                                full->lines[0].baseline + top}));
    start = end;
  }

  civue_text_block *b = block_of(t);
  b->scale = scale;
  b->baseline = full->baseline;
  b->width = width;
  b->height = (f32)(lines * lh / scale);
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
  // text being wrapped keeps what shaped it with its unwrapped block, the
  // one asked first for how wide it is.
  u64 whole = 0;
  if (max_width > 0 &&
      map_get(&t->layout_index, layout_key(s, n, style, 0, scale), &whole)) {
    const Layout *w = &t->layouts.v[whole - 1];
    if (w->len != n || memcmp(w->text, s, n)) {
      whole = 0;
    }
  }

  civue_text_block *b = nullptr;
  bool latin = false;
  if (whole) {
    Layout *w = &t->layouts.v[whole - 1];
    if (!w->shaped.ts) {
      w->shaped = typeset(t, w->text, n, style, scale);
    }

    if (w->plain) {
      b = rewrap(t, w, style, max_width, scale);
    }

    if (!b) {
      b = build(t, s, n, style, max_width, scale, &w->shaped, &latin);
    }
  }
  else if (max_width > 0) {
    Shaped shaped = typeset(t, s, n, style, scale);
    b = build(t, s, n, style, max_width, scale, &shaped, &latin);
    CFRelease(shaped.ts);
  }
  else {
    b = one_line(t, s, n, style, scale, &latin);
  }

  Layout l = {key, xstrndup(s, n), n, b, t->frame};
  l.wrapped = max_width > 0;
  if (l.wrapped) {
    t->churn += b->nglyphs * (sizeof(civue_glyph) + 12) +
                b->ncarets * sizeof(civue_caret) + n;
  }
  l.plain = !whole && max_width <= 0 && latin && n > 0 && b->nlines == 1 &&
            b->ncarets == n + 1;
  if (map_get(&t->layout_index, key, &at)) {
    Layout *old = &t->layouts.v[at - 1];
    layout_free(old);
    *old = l;
  }
  else {
    vec_push(&t->layouts, l);
    map_put(&t->layout_index, key, t->layouts.len);
  }
  return b;
}

void civue_text_end_frame(civue_text_system *t) {
  // text wrapped to a width that keeps changing leaves a block a frame, of
  // kilobytes each: megabytes of them are swept without waiting.
  constexpr usize CHURN = 1 << 20;
  t->frame++;
  if (t->frame % 30 && t->churn < CHURN) {
    return;
  }

  // text changing every frame leaves a block a frame too: past a few
  // thousand, the ones not asked for four frames go. a wrapped block is cut
  // from text set once and costs little to make again, so it goes first.
  u64 kept = t->layouts.len > 2048 ? 4 : 600;
  t->churn = 0;
  // blocks live until the end of the frame after they were last asked.
  u32 k = 0;
  map_clear(&t->layout_index);
  vec_each(&t->layouts, l) {
    if (t->frame - l->used >= (l->wrapped ? 2 : kept)) {
      layout_free(l);
      continue;
    }
    t->layouts.v[k++] = *l;
    map_put(&t->layout_index, l->key, k);
  }
  t->layouts.len = k;
}

// a glyph's bitmap and where it goes from its pen position, coverage or
// premultiplied rgba for color glyphs.
static bool rasterize(const Font *f, u16 id, u32 subpixel, civue_bytes *out,
                      i32 *left, i32 *top, u32 *w, u32 *h) {
  CGGlyph g = id;
  CGRect r;
  CTFontGetBoundingRectsForGlyphs(f->font, kCTFontOrientationHorizontal, &g, &r,
                                  1);
  f64 dx = (f64)subpixel / SUBPIXELS;
  if (r.size.width <= 0 || r.size.height <= 0) {
    *w = *h = 0;
    return true;
  }
  f64 x0 = floor(r.origin.x + dx) - 1,
      x1 = ceil(r.origin.x + r.size.width + dx) + 1;
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
  CGColorSpaceRef space =
    f->color ? CGColorSpaceCreateWithName(kCGColorSpaceSRGB) : nullptr;
  CGContextRef ctx = CGBitmapContextCreate(
    out->v, *w, *h, 8, *w * bpp, space,
    f->color ? (CGBitmapInfo)kCGImageAlphaPremultipliedLast
             : (CGBitmapInfo)kCGImageAlphaOnly);
  if (space) {
    CGColorSpaceRelease(space);
  }
  if (!ctx) {
    return false;
  }
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

static const GlyphEntry *glyph(civue_text_system *t, civue_atlas *mask,
                               civue_atlas *color, const civue_glyph *g,
                               u32 subpixel) {
  Font *f = &t->fonts.v[g->font];
  if (f->color) {
    subpixel = 0;
  }

  u32 *near = nullptr;
  if (g->id < NEAR_GLYPHS) {
    if (!f->near) {
      f->near = xcalloc(NEAR_GLYPHS * SUBPIXELS, sizeof *f->near);
    }

    near = &f->near[g->id * SUBPIXELS + subpixel];
    if (*near) {
      GlyphEntry *e = &t->glyphs.v[*near - 1];
      if (e->generation == (e->color ? color : mask)->generation) {
        return e;
      }
    }
  }

  u64 key = hash_u64((u64)g->font << 24 ^ (u64)g->id << 4 ^ subpixel) | 1;
  u64 at;
  GlyphEntry *e = nullptr;
  if (map_get(&t->glyph_index, key, &at)) {
    e = &t->glyphs.v[at - 1];
    civue_atlas *a = e->color ? color : mask;
    if (e->generation == a->generation) {
      if (near) {
        *near = (u32)at;
      }

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
  f32 uv[4];
  if (!civue_atlas_insert(a, w, h, px.v, uv)) {
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
  }
  else {
    vec_push(&t->glyphs, n);
    at = t->glyphs.len;
    map_put(&t->glyph_index, key, at);
  }

  if (near) {
    *near = (u32)at;
  }

  return &t->glyphs.v[at - 1];
}

// where a block's glyphs are in the atlas, found once for the fraction of a
// pixel it is drawn at.
// a glyph that draws nothing, a space for one.
constexpr u32 EMPTY = UINT32_MAX;

typedef struct {
  // the glyph's entry + 1, EMPTY, or 0 when it could not be made: it is
  // asked for again the next time.
  u32 entry;
  u32 sub;
  // whole pixels from the block's origin, floored.
  i32 dx;
} Found;

typedef struct {
  f64 fraction;
  Found v[];
} Drawn;

static u32 find(civue_text_system *t, civue_atlas *mask, civue_atlas *color,
                const civue_glyph *g, u32 sub) {
  const GlyphEntry *e = glyph(t, mask, color, g, sub);
  if (!e) {
    return 0;
  }

  return e->w && e->h ? (u32)(e - t->glyphs.v) + 1 : EMPTY;
}

static Drawn *drawn(civue_text_system *t, civue_atlas *mask, civue_atlas *color,
                    const civue_text_block *b, f64 fraction) {
  Drawn *d = b->cache;
  if (d && d->fraction == fraction) {
    return d;
  }

  if (!d) {
    d = xmalloc(sizeof *d + b->nglyphs * sizeof *d->v);
    ((civue_text_block *)b)->cache = d;
  }

  d->fraction = fraction;
  for (u32 i = 0; i < b->nglyphs; i++) {
    const civue_glyph *g = &b->glyphs[i];
    f64 x = fraction + g->x, px = floor(x);
    u32 sub = (u32)floor((x - px) * SUBPIXELS) % SUBPIXELS;
    d->v[i] = (Found){find(t, mask, color, g, sub), sub, (i32)px};
  }

  return d;
}

void civue_text_draw(civue_text_system *t, civue_draw_list *d,
                     civue_atlas *mask, civue_atlas *color,
                     const civue_text_block *b, civue_vec2 origin,
                     civue_color base, const civue_text_span *spans,
                     u32 nspans) {
  if (!b->nglyphs) {
    return;
  }

  f64 s = b->scale;
  f64 ox = origin.x * s, oy = round(origin.y * s);
  f64 whole = floor(ox);
  Drawn *found = drawn(t, mask, color, b, ox - whole);

  // glyphs tinted alike go to the list in one run.
  t->quads.len = 0;
  vec_reserve(&t->quads, b->nglyphs);
  civue_color run = base;

  for (u32 i = 0; i < b->nglyphs; i++) {
    Found *f = &found->v[i];
    if (f->entry == EMPTY) {
      continue;
    }

    const civue_glyph *g = &b->glyphs[i];
    const GlyphEntry *e = f->entry ? &t->glyphs.v[f->entry - 1] : nullptr;
    // an atlas that started over since has it somewhere else.
    if (!e || e->generation != (e->color ? color : mask)->generation) {
      f->entry = find(t, mask, color, g, f->sub);
      if (!f->entry || f->entry == EMPTY) {
        continue;
      }
      e = &t->glyphs.v[f->entry - 1];
    }

    civue_color c = base;
    for (u32 k = 0; k < nspans; k++) {
      if (g->byte >= spans[k].start && g->byte < spans[k].end) {
        c = spans[k].color;
        break;
      }
    }

    f64 x = whole + f->dx + e->left, y = round(oy + g->y) + e->top;
    bool same = c.r == run.r && c.g == run.g && c.b == run.b && c.a == run.a;
    if (e->color || !same) {
      // light text on dark grounds looks thinner, browsers boost it.
      f32 gamma =
        1 - 0.45f * (0.2126f * run.r + 0.7152f * run.g + 0.0722f * run.b);
      civue_dl_masks(d, t->quads.v, t->quads.len, run, gamma);
      t->quads.len = 0;
      run = c;
    }

    if (e->color) {
      civue_dl_color_glyph(d, rect_xywh(x, y, e->w, e->h), e->uv, c.a);
      continue;
    }

    t->quads.v[t->quads.len++] =
      (civue_glyph_quad){{(f32)x, (f32)y, (f32)e->w, (f32)e->h},
                         {e->uv[0], e->uv[1], e->uv[2], e->uv[3]}};
  }

  f32 gamma = 1 - 0.45f * (0.2126f * run.r + 0.7152f * run.g + 0.0722f * run.b);
  civue_dl_masks(d, t->quads.v, t->quads.len, run, gamma);
}
