#include "win.h"

#include <civue/text.h>

#include <initguid.h>

#include <d2d1.h>
#include <dwrite_3.h>
#include <math.h>
#include <stddefer.h>
#include <stdlib.h>

// horizontal positions a glyph is rasterized at within a pixel.
constexpr u32 SUBPIXELS = 4;

// where the faces text is set in come from: the app's own from memory, else
// the system's families.
enum {
  SRC_UI,
  SRC_CODE,
  SRC_DISPLAY,
  SRC_COUNT = SRC_DISPLAY + CIVUE_TEXT_FACES + 1
};

// the image formats of glyphs drawn in colors.
constexpr DWRITE_GLYPH_IMAGE_FORMATS PICTURES =
  DWRITE_GLYPH_IMAGE_FORMATS_COLR | DWRITE_GLYPH_IMAGE_FORMATS_PNG |
  DWRITE_GLYPH_IMAGE_FORMATS_JPEG | DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
  DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8 |
  DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE;

/* ========================================================================== */
/* icu. windows has its own since 10 1903, and directwrite tells neither      */
/* where graphemes end nor which characters show as emoji pictures.           */
/* ========================================================================== */

constexpr i32 UBRK_CHARACTER = 0;
constexpr i32 UBRK_WORD = 1;
constexpr i32 UCHAR_EMOJI_PRESENTATION = 58;

typedef struct {
  void *(*open)(i32 type, const char *locale, const u16 *text, i32 len,
                i32 *status);
  void (*set_text)(void *brk, const u16 *text, i32 len, i32 *status);
  i32 (*next)(void *brk);
  // the first boundary past a unit and the last one before it, -1 for none.
  i32 (*following)(void *brk, i32 at);
  i32 (*preceding)(void *brk, i32 at);
  void (*close)(void *brk);
  i8 (*has)(i32 c, i32 property);
  // a break iterator over graphemes, given each text in turn.
  void *graphemes;
} Icu;

static void icu_load(Icu *icu) {
  HMODULE m =
    LoadLibraryExW(L"icu.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);

  if (!m) {
    return;
  }

  icu->open = (typeof(icu->open))(void *)GetProcAddress(m, "ubrk_open");
  icu->set_text =
    (typeof(icu->set_text))(void *)GetProcAddress(m, "ubrk_setText");
  icu->next = (typeof(icu->next))(void *)GetProcAddress(m, "ubrk_next");
  icu->following =
    (typeof(icu->following))(void *)GetProcAddress(m, "ubrk_following");
  icu->preceding =
    (typeof(icu->preceding))(void *)GetProcAddress(m, "ubrk_preceding");
  icu->close = (typeof(icu->close))(void *)GetProcAddress(m, "ubrk_close");
  icu->has =
    (typeof(icu->has))(void *)GetProcAddress(m, "u_hasBinaryProperty");

  if (!icu->open || !icu->set_text || !icu->next || !icu->following ||
      !icu->preceding || !icu->close || !icu->has) {
    *icu = (Icu){};
    return;
  }

  i32 status = 0;
  icu->graphemes = icu->open(UBRK_CHARACTER, "", nullptr, 0, &status);

  if (status > 0) {
    icu->graphemes = nullptr;
  }
}

// icu's dictionaries, the ones chromium cuts these words with too.
bool civue__word_at(const u16 *s, u32 n, u32 at, u32 *from, u32 *to) {
  static Icu icu;
  static bool asked;

  if (!asked) {
    asked = true;
    icu_load(&icu);
  }

  if (!icu.open) {
    return false;
  }

  i32 status = 0;
  void *words = icu.open(UBRK_WORD, "", s, (i32)n, &status);

  if (status > 0 || !words) {
    return false;
  }

  i32 end = icu.following(words, (i32)at);
  i32 start = end < 0 ? end : icu.preceding(words, end);

  icu.close(words);

  if (start < 0) {
    return false;
  }

  *from = (u32)start;
  *to = (u32)end;
  return true;
}

/* ========================================================================== */
/* what directwrite analyzes: the text it reads and what it says of it, its   */
/* source and its sink in one.                                                */
/* ========================================================================== */

typedef struct {
  u32 at, len;
  DWRITE_SCRIPT_ANALYSIS script;
} Script;

typedef struct {
  IDWriteTextAnalysisSource source;
  IDWriteTextAnalysisSink sink;
  const wchar_t *text;
  u32 len;
  const wchar_t *locale;
  DWRITE_READING_DIRECTION direction;
  Vec(Script) scripts;
  // a breakpoint and a bidi level for each utf-16 unit.
  Vec(DWRITE_LINE_BREAKPOINT) breaks;
  Vec(u8) levels;
} Analysis;

static Analysis *of_source(IDWriteTextAnalysisSource *s) {
  return (Analysis *)((char *)s - offsetof(Analysis, source));
}

static Analysis *of_sink(IDWriteTextAnalysisSink *s) {
  return (Analysis *)((char *)s - offsetof(Analysis, sink));
}

// directwrite asks for newer interfaces it then calls past this vtable.
static HRESULT STDMETHODCALLTYPE source_query(IDWriteTextAnalysisSource *s,
                                              REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) ||
      IsEqualIID(iid, &IID_IDWriteTextAnalysisSource)) {
    *out = s;
    return S_OK;
  }

  *out = nullptr;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE source_ref(IDWriteTextAnalysisSource *) {
  return 1;
}

static HRESULT STDMETHODCALLTYPE source_at(IDWriteTextAnalysisSource *s,
                                           UINT32 at, const WCHAR **text,
                                           UINT32 *len) {
  Analysis *a = of_source(s);
  *text = at < a->len ? a->text + at : nullptr;
  *len = at < a->len ? a->len - at : 0;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE source_before(IDWriteTextAnalysisSource *s,
                                               UINT32 at, const WCHAR **text,
                                               UINT32 *len) {
  Analysis *a = of_source(s);
  bool some = at > 0 && at <= a->len;
  *text = some ? a->text : nullptr;
  *len = some ? at : 0;
  return S_OK;
}

static DWRITE_READING_DIRECTION STDMETHODCALLTYPE
source_direction(IDWriteTextAnalysisSource *s) {
  return of_source(s)->direction;
}

static HRESULT STDMETHODCALLTYPE source_locale(IDWriteTextAnalysisSource *s,
                                               UINT32 at, UINT32 *len,
                                               const WCHAR **locale) {
  Analysis *a = of_source(s);
  *len = at < a->len ? a->len - at : 0;
  *locale = a->locale;
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE source_numbers(
  IDWriteTextAnalysisSource *s, UINT32 at, UINT32 *len,
  IDWriteNumberSubstitution **numbers) {
  Analysis *a = of_source(s);
  *len = at < a->len ? a->len - at : 0;
  *numbers = nullptr;
  return S_OK;
}

static IDWriteTextAnalysisSourceVtbl source_vtbl = {
  source_query,  source_ref,       source_ref,    source_at,
  source_before, source_direction, source_locale, source_numbers};

static HRESULT STDMETHODCALLTYPE sink_query(IDWriteTextAnalysisSink *s,
                                            REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) ||
      IsEqualIID(iid, &IID_IDWriteTextAnalysisSink)) {
    *out = s;
    return S_OK;
  }

  *out = nullptr;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE sink_ref(IDWriteTextAnalysisSink *) {
  return 1;
}

static HRESULT STDMETHODCALLTYPE sink_script(
  IDWriteTextAnalysisSink *s, UINT32 at, UINT32 len,
  const DWRITE_SCRIPT_ANALYSIS *script) {
  vec_push(&of_sink(s)->scripts, ((Script){at, len, *script}));
  return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_breaks(
  IDWriteTextAnalysisSink *s, UINT32 at, UINT32 len,
  const DWRITE_LINE_BREAKPOINT *breaks) {
  Analysis *a = of_sink(s);

  if (at + len <= a->breaks.len) {
    memcpy(a->breaks.v + at, breaks, len * sizeof *breaks);
  }

  return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_bidi(IDWriteTextAnalysisSink *s,
                                           UINT32 at, UINT32 len, UINT8,
                                           UINT8 level) {
  Analysis *a = of_sink(s);

  if (at + len <= a->levels.len) {
    memset(a->levels.v + at, level, len);
  }

  return S_OK;
}

static HRESULT STDMETHODCALLTYPE sink_numbers(IDWriteTextAnalysisSink *,
                                              UINT32, UINT32,
                                              IDWriteNumberSubstitution *) {
  return S_OK;
}

static IDWriteTextAnalysisSinkVtbl sink_vtbl = {
  sink_query,  sink_ref,  sink_ref,    sink_script,
  sink_breaks, sink_bidi, sink_numbers};

/* ========================================================================== */
/* the text system.                                                           */
/* ========================================================================== */

typedef struct {
  IDWriteFontFace *face;
  // some of its glyphs are pictures, colr or bitmaps.
  bool color;
  // it has a glyph for each printable ascii character.
  bool ascii;
  // above and below the baseline, in ems.
  f32 ascent, descent;
} Face;

// a face at a size, what glyphs are drawn with.
typedef struct {
  u32 face;
  f32 px;
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

// a glyph as shaped, in its run's logical order.
typedef struct {
  u16 id;
  // a control's, drawn as nothing.
  bool hidden;
  // the first code point of its cluster.
  u32 cluster;
  // pixels: its advance, and how far it is moved along it and up.
  f32 advance, dx, dy;
  // the letter spacing after it, on the last glyph of a cluster.
  f32 kern;
} Shaped;

typedef struct {
  u32 start, end; // code points.
  u32 font;
  u32 script;
  u8 level;
  u32 g0, g1; // its glyphs in `shaped`.
} Run;

struct civue_text_system {
  IDWriteFactory2 *dw;
  // newer factories where windows has them, null else: color glyph runs,
  // fonts from memory and colr v1 paint trees.
  IDWriteFactory4 *dw4;
  IDWriteFactory5 *dw5;
  IDWriteFactory8 *dw8;
  IDWriteFontCollection *system;
  IDWriteFontFallback *fallback;
  IDWriteTextAnalyzer *analyzer;
  IDWriteInMemoryFontFileLoader *memory;
  wchar_t locale[LOCALE_NAME_MAX_LENGTH];
  // what latin is analyzed as, given to ascii without asking.
  DWRITE_SCRIPT_ANALYSIS latin;
  Icu icu;
  Analysis an;
  // the faces the app set from memory, and the families it named.
  IDWriteFontFace *own[SRC_COUNT];
  wchar_t *family[2];
  Vec(Face) faces;
  // what a face is, its files, index, simulations and axes, to index + 1.
  civue_map face_index;
  // the face of a source at a weight, slant and optical size, index + 1.
  civue_map base_index;
  // segoe ui emoji's index + 1, 0 until looked up, UINT32_MAX when missing.
  u32 emoji;
  Vec(Font) fonts;
  civue_map font_index;
  Vec(Layout) layouts;
  civue_map layout_index; // key to index + 1.
  Vec(GlyphEntry) glyphs;
  civue_map glyph_index;
  // what a block is built from, kept from a text to the next: its code
  // points, where each starts in bytes and in utf-16, the utf-16 and the
  // code point of each of its units.
  civue_u32s cps, at, at16, of16;
  Vec(wchar_t) w16;
  // whether each code point starts a grapheme, its bidi level and font.
  Vec(u8) starts, levels;
  civue_u32s font_of;
  Vec(u16) indices, types;
  Vec(Run) runs, parts;
  Vec(Shaped) shaped;
  // what each grapheme takes on its line, on its first code point.
  Vec(f32) adv, caret_x;
  civue_u32s breaks;
  // what shaping a run gives.
  Vec(u16) clusters, gids;
  Vec(DWRITE_SHAPING_TEXT_PROPERTIES) tprops;
  Vec(DWRITE_SHAPING_GLYPH_PROPERTIES) gprops;
  Vec(f32) gadv;
  Vec(DWRITE_GLYPH_OFFSET) goff;
  Vec(civue_glyph) glyphs_made;
  Vec(civue_caret) carets_made;
  Vec(civue_text_line) lines_made;
  // the glyphs of the block being drawn, gathered to go in one run.
  Vec(civue_glyph_quad) quads;
  // coverage drawn larger, and its sums over each pixel.
  civue_bytes fine;
  Vec(u16) sums;
  u64 frame;
};

civue_text_system *civue_text_new() {
  civue_text_system *t = own_new(sizeof *t);

  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                 &IID_IDWriteFactory2, (IUnknown **)&t->dw))) {
    die("directwrite: no factory");
  }

  IDWriteFactory2_QueryInterface(t->dw, &IID_IDWriteFactory4,
                                 (void **)&t->dw4);
  IDWriteFactory2_QueryInterface(t->dw, &IID_IDWriteFactory5,
                                 (void **)&t->dw5);
  IDWriteFactory2_QueryInterface(t->dw, &IID_IDWriteFactory8,
                                 (void **)&t->dw8);

  if (FAILED(IDWriteFactory2_GetSystemFontCollection(t->dw, &t->system,
                                                     FALSE)) ||
      FAILED(IDWriteFactory2_GetSystemFontFallback(t->dw, &t->fallback)) ||
      FAILED(IDWriteFactory2_CreateTextAnalyzer(t->dw, &t->analyzer))) {
    die("directwrite: no fonts");
  }

  if (t->dw5 &&
      SUCCEEDED(IDWriteFactory5_CreateInMemoryFontFileLoader(t->dw5,
                                                             &t->memory))) {
    IDWriteFactory5_RegisterFontFileLoader(t->dw5,
                                           (IDWriteFontFileLoader *)t->memory);
  }

  if (!GetUserDefaultLocaleName(t->locale, countof(t->locale))) {
    wcscpy(t->locale, L"en-US");
  }

  t->an.source.lpVtbl = &source_vtbl;
  t->an.sink.lpVtbl = &sink_vtbl;
  t->an.locale = t->locale;
  icu_load(&t->icu);

  t->an.text = L"a";
  t->an.len = 1;
  IDWriteTextAnalyzer_AnalyzeScript(t->analyzer, &t->an.source, 0, 1,
                                    &t->an.sink);
  if (t->an.scripts.len) {
    t->latin = t->an.scripts.v[0].script;
  }

  return t;
}

static void forget_layouts(civue_text_system *t) {
  vec_each(&t->layouts, l) {
    free(l->text);
    free(l->block->cache);
    free(l->block);
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

  vec_each(&t->faces, f) {
    IDWriteFontFace_Release(f->face);
  }

  for (u32 i = 0; i < SRC_COUNT; i++) {
    if (t->own[i]) {
      IDWriteFontFace_Release(t->own[i]);
    }
  }

  free(t->family[0]);
  free(t->family[1]);
  vec_free(&t->faces);
  map_free(&t->face_index);
  map_free(&t->base_index);
  vec_free(&t->fonts);
  map_free(&t->font_index);
  vec_free(&t->glyphs);
  map_free(&t->glyph_index);

  vec_free(&t->an.scripts);
  vec_free(&t->an.breaks);
  vec_free(&t->an.levels);
  vec_free(&t->cps);
  vec_free(&t->at);
  vec_free(&t->at16);
  vec_free(&t->of16);
  vec_free(&t->w16);
  vec_free(&t->starts);
  vec_free(&t->levels);
  vec_free(&t->font_of);
  vec_free(&t->indices);
  vec_free(&t->types);
  vec_free(&t->runs);
  vec_free(&t->parts);
  vec_free(&t->shaped);
  vec_free(&t->adv);
  vec_free(&t->caret_x);
  vec_free(&t->breaks);
  vec_free(&t->clusters);
  vec_free(&t->gids);
  vec_free(&t->tprops);
  vec_free(&t->gprops);
  vec_free(&t->gadv);
  vec_free(&t->goff);
  vec_free(&t->glyphs_made);
  vec_free(&t->carets_made);
  vec_free(&t->lines_made);
  vec_free(&t->quads);
  vec_free(&t->fine);
  vec_free(&t->sums);

  if (t->icu.graphemes) {
    t->icu.close(t->icu.graphemes);
  }

  if (t->memory) {
    IDWriteFactory5_UnregisterFontFileLoader(
      t->dw5, (IDWriteFontFileLoader *)t->memory);
    IDWriteInMemoryFontFileLoader_Release(t->memory);
  }

  IDWriteTextAnalyzer_Release(t->analyzer);
  IDWriteFontFallback_Release(t->fallback);
  IDWriteFontCollection_Release(t->system);

  if (t->dw8) {
    IDWriteFactory8_Release(t->dw8);
  }

  if (t->dw5) {
    IDWriteFactory5_Release(t->dw5);
  }

  if (t->dw4) {
    IDWriteFactory4_Release(t->dw4);
  }

  IDWriteFactory2_Release(t->dw);
  own_free(t);
}

/* ========================================================================== */
/* faces.                                                                     */
/* ========================================================================== */

// what a face is, the same for two objects of one font: the files it reads,
// its index in them, its simulations and where it is on its axes.
static u64 identity_of(IDWriteFontFace *face) {
  u64 h = hash_u64((u64)IDWriteFontFace_GetIndex(face) << 8 |
                   IDWriteFontFace_GetSimulations(face));
  UINT32 n = 0;
  IDWriteFontFace_GetFiles(face, &n, nullptr);
  IDWriteFontFile *files[4] = {};

  if (n > countof(files) ||
      FAILED(IDWriteFontFace_GetFiles(face, &n, files))) {
    return hash_u64((u64)(uintptr_t)face) | 1;
  }

  for (u32 i = 0; i < n; i++) {
    const void *key = nullptr;
    UINT32 size = 0;
    IDWriteFontFileLoader *loader = nullptr;

    if (SUCCEEDED(IDWriteFontFile_GetReferenceKey(files[i], &key, &size))) {
      h = hash_u64(h ^ hash_bytes(key, size));
    }

    if (SUCCEEDED(IDWriteFontFile_GetLoader(files[i], &loader))) {
      h = hash_u64(h ^ (u64)(uintptr_t)loader);
      IDWriteFontFileLoader_Release(loader);
    }

    IDWriteFontFile_Release(files[i]);
  }

  IDWriteFontFace5 *f5 = nullptr;

  if (SUCCEEDED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace5,
                                               (void **)&f5))) {
    DWRITE_FONT_AXIS_VALUE values[16];
    u32 count = IDWriteFontFace5_GetFontAxisValueCount(f5);

    if (count <= countof(values) &&
        SUCCEEDED(IDWriteFontFace5_GetFontAxisValues(f5, values, count))) {
      h = hash_u64(h ^ hash_bytes(values, count * sizeof *values));
    }

    IDWriteFontFace5_Release(f5);
  }

  return h | 1;
}

// keeps `face` among the faces, or finds the same font there already, and
// says its index. takes `face`.
static u32 face_add(civue_text_system *t, IDWriteFontFace *face) {
  u64 id = identity_of(face), at;

  if (map_get(&t->face_index, id, &at)) {
    IDWriteFontFace_Release(face);
    return (u32)at - 1;
  }

  Face f = {.face = face};
  DWRITE_FONT_METRICS m;
  IDWriteFontFace_GetMetrics(face, &m);
  f32 em = m.designUnitsPerEm ? m.designUnitsPerEm : 2048;
  f.ascent = m.ascent / em;
  f.descent = m.descent / em;

  IDWriteFontFace4 *f4 = nullptr;
  IDWriteFontFace2 *f2 = nullptr;

  if (SUCCEEDED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace4,
                                               (void **)&f4))) {
    f.color = IDWriteFontFace4_GetGlyphImageFormats(f4) & PICTURES;
    IDWriteFontFace4_Release(f4);
  }
  else if (SUCCEEDED(IDWriteFontFace_QueryInterface(
             face, &IID_IDWriteFontFace2, (void **)&f2))) {
    f.color = IDWriteFontFace2_IsColorFont(f2);
    IDWriteFontFace2_Release(f2);
  }

  constexpr u32 PRINTABLE = 0x7f - 0x20;
  UINT32 cps[PRINTABLE];
  UINT16 ids[PRINTABLE] = {};

  for (u32 i = 0; i < PRINTABLE; i++) {
    cps[i] = 0x20 + i;
  }

  IDWriteFontFace_GetGlyphIndices(face, cps, PRINTABLE, ids);
  f.ascii = true;

  for (u32 i = 0; i < PRINTABLE; i++) {
    f.ascii &= ids[i] != 0;
  }

  vec_push(&t->faces, f);
  map_put(&t->face_index, id, t->faces.len);
  return t->faces.len - 1;
}

// `face` at `weight` when it varies. takes `face`.
static IDWriteFontFace *varied(IDWriteFontFace *face, f32 weight) {
  IDWriteFontFace5 *f5 = nullptr;

  if (FAILED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace5,
                                            (void **)&f5))) {
    return face;
  }

  defer IDWriteFontFace5_Release(f5);

  if (!IDWriteFontFace5_HasVariations(f5)) {
    return face;
  }

  IDWriteFontResource *res = nullptr;

  if (FAILED(IDWriteFontFace5_GetFontResource(f5, &res))) {
    return face;
  }

  defer IDWriteFontResource_Release(res);

  DWRITE_FONT_AXIS_VALUE values[16];
  DWRITE_FONT_AXIS_RANGE ranges[16];
  u32 n = IDWriteFontFace5_GetFontAxisValueCount(f5);
  u32 nr = IDWriteFontResource_GetFontAxisCount(res);

  if (!n || n > countof(values) || nr > countof(ranges) ||
      FAILED(IDWriteFontFace5_GetFontAxisValues(f5, values, n)) ||
      FAILED(IDWriteFontResource_GetFontAxisRanges(res, ranges, nr))) {
    return face;
  }

  bool weighs = false;

  for (u32 i = 0; i < n; i++) {
    if (values[i].axisTag != DWRITE_FONT_AXIS_TAG_WEIGHT) {
      continue;
    }

    for (u32 k = 0; k < nr; k++) {
      if (ranges[k].axisTag == DWRITE_FONT_AXIS_TAG_WEIGHT) {
        values[i].value = CLAMP(weight, ranges[k].minValue,
                                ranges[k].maxValue);
        weighs = true;
      }
    }
  }

  IDWriteFontFace5 *made = nullptr;

  if (!weighs ||
      FAILED(IDWriteFontResource_CreateFontFace(
        res, IDWriteFontFace_GetSimulations(face), values, n, &made))) {
    return face;
  }

  IDWriteFontFace_Release(face);
  return (IDWriteFontFace *)made;
}

static DWRITE_FONT_WEIGHT weight_of(f32 weight) {
  return (DWRITE_FONT_WEIGHT)CLAMP(lroundf(weight), 1, 999);
}

// the face of a family the system has nearest `weight`, upright or italic,
// null when it has no such family.
static IDWriteFontFace *family_face(civue_text_system *t, const wchar_t *name,
                                    f32 weight, bool italic) {
  UINT32 i = 0;
  BOOL exists = FALSE;

  if (!name ||
      FAILED(IDWriteFontCollection_FindFamilyName(t->system, name, &i,
                                                  &exists)) ||
      !exists) {
    return nullptr;
  }

  IDWriteFontFamily *family = nullptr;

  if (FAILED(IDWriteFontCollection_GetFontFamily(t->system, i, &family))) {
    return nullptr;
  }

  defer IDWriteFontFamily_Release(family);

  IDWriteFont *font = nullptr;

  if (FAILED(IDWriteFontFamily_GetFirstMatchingFont(
        family, weight_of(weight), DWRITE_FONT_STRETCH_NORMAL,
        italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        &font))) {
    return nullptr;
  }

  defer IDWriteFont_Release(font);

  IDWriteFontFace *face = nullptr;
  return SUCCEEDED(IDWriteFont_CreateFontFace(font, &face)) ? face : nullptr;
}

// the face text of a source is set in at `weight`, upright or italic, `size`
// points tall: the app's, else the system's, segoe ui variable for the ui
// in its optical size for text or for display as windows sets them, its
// italic segoe ui's, and cascadia mono or consolas for code.
static u32 base_face(civue_text_system *t, u32 src, f32 weight, bool italic,
                     f32 size) {
  bool display = size >= 20;
  u64 key = hash_u64((u64)src << 40 | (u64)lroundf(weight * 4) << 8 |
                     (u64)italic << 1 | display) |
            1;
  u64 at;

  if (map_get(&t->base_index, key, &at)) {
    return (u32)at - 1;
  }

  IDWriteFontFace *face = t->own[src];

  if (face) {
    IDWriteFontFace_AddRef(face);
  }
  else {
    bool code = src == SRC_CODE;
    face = family_face(t, t->family[code], weight, italic);

    if (!face && code) {
      face = family_face(t, L"Cascadia Mono", weight, italic);
    }

    if (!face && code) {
      face = family_face(t, L"Consolas", weight, italic);
    }

    if (!face && !italic) {
      face = family_face(t,
                         display ? L"Segoe UI Variable Display"
                                 : L"Segoe UI Variable Text",
                         weight, false);
    }

    if (!face) {
      face = family_face(t, L"Segoe UI", weight, italic);
    }

    if (!face) {
      face = family_face(t, L"Arial", weight, italic);
    }

    if (!face) {
      die("directwrite: no face to set text in");
    }
  }

  u32 index = face_add(t, varied(face, weight));
  map_put(&t->base_index, key, index + 1);
  return index;
}

static u32 emoji_face(civue_text_system *t) {
  if (!t->emoji) {
    IDWriteFontFace *face = family_face(t, L"Segoe UI Emoji", 400, false);
    t->emoji = face ? face_add(t, face) + 1 : UINT32_MAX;
  }

  return t->emoji == UINT32_MAX ? UINT32_MAX : t->emoji - 1;
}

static bool covers(const civue_text_system *t, u32 face, u32 c) {
  UINT32 cp = c;
  UINT16 id = 0;
  IDWriteFontFace_GetGlyphIndices(t->faces.v[face].face, &cp, 1, &id);
  return id != 0;
}

// a face at a size, made once.
static u32 font_of(civue_text_system *t, u32 face, f32 px) {
  u32 bits[2] = {face};
  memcpy(bits + 1, &px, 4);
  u64 key = hash_bytes(bits, sizeof bits) | 1, at;

  if (map_get(&t->font_index, key, &at)) {
    return (u32)at - 1;
  }

  vec_push(&t->fonts, ((Font){face, px}));
  map_put(&t->font_index, key, t->fonts.len);
  return t->fonts.len - 1;
}

// a face of the font file in `data`, which directwrite copies.
static IDWriteFontFace *memory_face(civue_text_system *t, const u8 *data,
                                    usize n) {
  if (!t->memory || !n) {
    return nullptr;
  }

  IDWriteFontFile *file = nullptr;

  if (FAILED(IDWriteInMemoryFontFileLoader_CreateInMemoryFontFileReference(
        t->memory, (IDWriteFactory *)t->dw5, data, (UINT32)n, nullptr,
        &file))) {
    return nullptr;
  }

  defer IDWriteFontFile_Release(file);

  BOOL supported = FALSE;
  DWRITE_FONT_FILE_TYPE type;
  DWRITE_FONT_FACE_TYPE face_type;
  UINT32 count = 0;
  IDWriteFontFace *face = nullptr;

  if (FAILED(IDWriteFontFile_Analyze(file, &supported, &type, &face_type,
                                     &count)) ||
      !supported ||
      FAILED(IDWriteFactory2_CreateFontFace(t->dw, face_type, 1, &file, 0,
                                            DWRITE_FONT_SIMULATIONS_NONE,
                                            &face))) {
    return nullptr;
  }

  return face;
}

static void faces_changed(civue_text_system *t) {
  map_clear(&t->base_index);
  forget_layouts(t);
}

static void own_set(civue_text_system *t, u32 src, IDWriteFontFace *face) {
  if (t->own[src]) {
    IDWriteFontFace_Release(t->own[src]);
  }

  t->own[src] = face;
  faces_changed(t);
}

void civue_text_register_display(civue_text_system *t, u8 display,
                                 const u8 *data, usize n) {
  if (!display || display > CIVUE_TEXT_FACES) {
    return;
  }

  IDWriteFontFace *face = memory_face(t, data, n);

  if (face) {
    own_set(t, SRC_DISPLAY + display, face);
  }
}

void civue_text_set_face(civue_text_system *t, civue_face face, const u8 *data,
                         usize n) {
  bool code = face == CIVUE_FACE_CODE;
  IDWriteFontFace *made = memory_face(t, data, n);

  if (!made) {
    return;
  }

  free(t->family[code]);
  t->family[code] = nullptr;
  own_set(t, code ? SRC_CODE : SRC_UI, made);
}

void civue_text_set_family(civue_text_system *t, civue_face face,
                           const char *family) {
  bool code = face == CIVUE_FACE_CODE;
  free(t->family[code]);
  t->family[code] = family && *family ? civue__wide(family, -1) : nullptr;
  own_set(t, code ? SRC_CODE : SRC_UI, nullptr);
}

/* ========================================================================== */
/* layout.                                                                    */
/* ========================================================================== */

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

static bool space(u32 c) {
  return c == ' ' || c == '\t' || c == 0x3000 || c == '\n' || c == '\r' ||
         c == 0x2028;
}

// a control, drawn as nothing and taking no room. a tab is set as a space.
static bool control(u32 c) {
  return (c < 0x20 && c != '\t') || (c >= 0x7f && c < 0xa0);
}

// a character that draws nothing of its own, which a face need not have.
static bool invisible(u32 c) {
  return control(c) || c == 0x200b || c == 0x200c || c == 0x200d ||
         (c >= 0x200e && c <= 0x200f) || (c >= 0x202a && c <= 0x202e) ||
         (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x2069) ||
         (c >= 0xfe00 && c <= 0xfe0f) || c == 0xfeff ||
         (c >= 0xe0100 && c <= 0xe01ef);
}

static bool regional(u32 c) {
  return c >= 0x1f1e6 && c <= 0x1f1ff;
}

// whether `c` goes on the grapheme `prev` is in, where windows has no icu.
static bool extends(u32 prev, u32 c, u32 regionals) {
  if (prev == '\r' && c == '\n') {
    return true;
  }

  if (prev == 0x200d) {
    return c >= 0x2000;
  }

  if (regional(prev) && regional(c)) {
    return regionals % 2;
  }

  if (c == 0x200c || c == 0x200d || c == 0x20e3 ||
      (c >= 0xfe00 && c <= 0xfe0f) || (c >= 0x1f3fb && c <= 0x1f3ff) ||
      (c >= 0xe0020 && c <= 0xe007f) || (c >= 0xe0100 && c <= 0xe01ef)) {
    return true;
  }

  if (c < 0x300 || c > 0xffff) {
    return false;
  }

  wchar_t w = (wchar_t)c;
  WORD type = 0;
  GetStringTypeW(CT_CTYPE3, &w, 1, &type);
  return type & (C3_NONSPACING | C3_VOWELMARK);
}

// marks the code points graphemes start at, the end too.
static void graphemes(civue_text_system *t, bool ascii) {
  u32 count = t->cps.len;
  const u32 *cps = t->cps.v;
  t->starts.len = 0;
  vec_reserve(&t->starts, count + 1);
  t->starts.len = count + 1;
  u8 *starts = t->starts.v;
  Icu *icu = &t->icu;

  if (!ascii && icu->graphemes) {
    i32 status = 0;
    icu->set_text(icu->graphemes, (const u16 *)t->w16.v, (i32)t->w16.len,
                  &status);

    if (status <= 0) {
      memset(starts, 0, count + 1);

      for (i32 b = 0; b >= 0 && b <= (i32)t->w16.len;
           b = icu->next(icu->graphemes)) {
        starts[t->of16.v[b]] = 1;
      }

      starts[0] = starts[count] = 1;
      return;
    }
  }

  u32 regionals = 0;

  for (u32 i = 0; i < count; i++) {
    starts[i] = !i || !extends(cps[i - 1], cps[i], regionals);
    regionals = regional(cps[i]) ? regionals + 1 : 0;
  }

  starts[count] = 1;
}

// whether a grapheme of these code points shows as an emoji's picture: one
// asked to, a keycap, one with a skin tone or a tag, a sequence of joined
// emoji, or an emoji that shows as a picture unless asked not to.
static bool pictured(const Icu *icu, const u32 *c, u32 n) {
  bool asked = false;

  for (u32 i = 0; i < n; i++) {
    if (c[i] == 0xfe0e) {
      return false;
    }

    asked |= c[i] == 0xfe0f || c[i] == 0x20e3 ||
             (c[i] >= 0x1f3fb && c[i] <= 0x1f3ff) ||
             (c[i] >= 0xe0020 && c[i] <= 0xe007f) ||
             (i && c[i - 1] == 0x200d);
  }

  if (asked) {
    return true;
  }

  if (c[0] < 0x80) {
    return false;
  }

  return icu->has ? icu->has((i32)c[0], UCHAR_EMOJI_PRESENTATION)
                  : c[0] >= 0x1f000 && c[0] <= 0x1faff;
}

// the faces of code points from `a` to `b` the text's own face lacks, a run
// at a time from the system's fallback, each grapheme in the face of its
// first unit.
static void fall_back(civue_text_system *t, civue_text_style st,
                      u32 base_font, f32 px, u32 a, u32 b) {
  u32 *fonts = t->font_of.v;
  const u32 *at16 = t->at16.v, *of16 = t->of16.v;
  u32 end = at16[b];

  for (u32 p = at16[a]; p < end;) {
    UINT32 mapped = 0;
    IDWriteFont *font = nullptr;
    FLOAT k = 1;
    HRESULT hr = IDWriteFontFallback_MapCharacters(
      t->fallback, &t->an.source, p, end - p, t->system, nullptr,
      weight_of(st.weight),
      st.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &k);

    if (FAILED(hr) || !mapped || mapped > end - p) {
      mapped = end - p;
    }

    u32 f = base_font;
    IDWriteFontFace *face = nullptr;

    if (font && SUCCEEDED(IDWriteFont_CreateFontFace(font, &face))) {
      f = font_of(t, face_add(t, face), px * (k > 0 ? k : 1));
    }

    if (font) {
      IDWriteFont_Release(font);
    }

    for (u32 q = p; q < p + mapped; q++) {
      if (at16[of16[q]] == q) {
        fonts[of16[q]] = f;
      }
    }

    p += mapped;
  }

  for (u32 i = a + 1; i < b; i++) {
    if (!t->starts.v[i]) {
      fonts[i] = fonts[i - 1];
    }
  }
}

// the font of each code point: emoji's for a grapheme that shows as a
// picture or for anything of an emoji style, the text's own face where it
// has every character of a grapheme, else the system's fallback.
static void choose_fonts(civue_text_system *t, civue_text_style st, u32 base,
                         f32 px, bool ascii) {
  u32 count = t->cps.len;
  const u32 *cps = t->cps.v;
  const u8 *starts = t->starts.v;
  u32 base_font = font_of(t, base, px);
  t->font_of.len = 0;
  vec_reserve(&t->font_of, count);
  t->font_of.len = count;
  u32 *fonts = t->font_of.v;

  if (ascii && t->faces.v[base].ascii) {
    for (u32 i = 0; i < count; i++) {
      fonts[i] = base_font;
    }

    return;
  }

  t->indices.len = 0;
  vec_reserve(&t->indices, count);
  IDWriteFontFace_GetGlyphIndices(t->faces.v[base].face, cps, count,
                                  t->indices.v);

  for (u32 a = 0; a < count;) {
    u32 b = a + 1;

    while (b < count && !starts[b]) {
      b++;
    }

    u32 c = cps[a], font = UINT32_MAX;

    if ((st.emoji && c > 0x7f) || pictured(&t->icu, cps + a, b - a)) {
      u32 emoji = emoji_face(t);

      if (emoji != UINT32_MAX && covers(t, emoji, c)) {
        font = font_of(t, emoji, px);
      }
    }

    bool has = true;

    for (u32 k = a; k < b && has; k++) {
      has = t->indices.v[k] || invisible(cps[k]);
    }

    if (font == UINT32_MAX && has) {
      font = base_font;
    }

    for (u32 k = a; k < b; k++) {
      fonts[k] = font;
    }

    a = b;
  }

  for (u32 a = 0; a < count;) {
    if (fonts[a] != UINT32_MAX) {
      a++;
      continue;
    }

    u32 b = a;

    while (b < count && fonts[b] == UINT32_MAX) {
      b++;
    }

    fall_back(t, st, base_font, px, a, b);
    a = b;
  }
}

// the bidi level of each code point, the paragraph's level returned: its
// direction is its first strong character's.
static u8 bidi(civue_text_system *t, bool ltr) {
  u32 count = t->cps.len, len16 = t->w16.len;
  t->levels.len = 0;
  vec_reserve(&t->levels, count);
  t->levels.len = count;
  memset(t->levels.v, 0, count);

  // nothing under hebrew goes from right to left or says so, which is most
  // text.
  if (ltr || !count) {
    return 0;
  }

  t->types.len = 0;
  vec_reserve(&t->types, len16);
  GetStringTypeW(CT_CTYPE2, t->w16.v, (int)len16, t->types.v);
  bool rtl = false, strong = false, any = false;

  for (u32 i = 0; i < len16; i++) {
    WORD type = t->types.v[i];
    wchar_t c = t->w16.v[i];

    if (!strong && (type == C2_LEFTTORIGHT || type == C2_RIGHTTOLEFT)) {
      rtl = type == C2_RIGHTTOLEFT;
      strong = true;
    }

    any |= type == C2_RIGHTTOLEFT || type == C2_ARABICNUMBER || c == 0x200f ||
           c == 0x61c || (c >= 0x202a && c <= 0x202e) ||
           (c >= 0x2066 && c <= 0x2069);
  }

  // text with nothing that goes right to left, like cjk or emoji.
  if (!any) {
    return 0;
  }

  t->an.direction = rtl ? DWRITE_READING_DIRECTION_RIGHT_TO_LEFT
                        : DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
  t->an.levels.len = 0;
  vec_reserve(&t->an.levels, len16);
  t->an.levels.len = len16;
  memset(t->an.levels.v, rtl, len16);
  IDWriteTextAnalyzer_AnalyzeBidi(t->analyzer, &t->an.source, 0, len16,
                                  &t->an.sink);
  t->an.direction = DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;

  for (u32 i = 0; i < count; i++) {
    t->levels.v[i] = t->an.levels.v[t->at16.v[i]];
  }

  return rtl;
}

static int script_order(const void *a, const void *b) {
  u32 x = ((const Script *)a)->at, y = ((const Script *)b)->at;
  return (x > y) - (x < y);
}

// the scripts of the text, in order, latin for ascii without asking.
static void scripts(civue_text_system *t, bool ascii) {
  u32 len16 = t->w16.len;
  t->an.scripts.len = 0;

  if (!ascii) {
    IDWriteTextAnalyzer_AnalyzeScript(t->analyzer, &t->an.source, 0, len16,
                                      &t->an.sink);
    qsort(t->an.scripts.v, t->an.scripts.len, sizeof *t->an.scripts.v,
          script_order);
  }

  if (!t->an.scripts.len) {
    vec_push(&t->an.scripts, ((Script){0, len16, t->latin}));
  }
}

// runs of one font, bidi level and script, cut at a grapheme past a few
// thousand code points: a cluster's glyph is a 16 bit index.
static void itemize(civue_text_system *t) {
  constexpr u32 LONGEST = 4096;
  u32 count = t->cps.len, si = 0;
  const u32 *fonts = t->font_of.v, *at16 = t->at16.v;
  const u8 *levels = t->levels.v, *starts = t->starts.v;
  const Analysis *an = &t->an;
  t->runs.len = 0;

  for (u32 i = 0; i < count;) {
    while (si + 1 < an->scripts.len &&
           at16[i] >= an->scripts.v[si].at + an->scripts.v[si].len) {
      si++;
    }

    u32 end16 = an->scripts.v[si].at + an->scripts.v[si].len;
    u32 j = i + 1;

    while (j < count && fonts[j] == fonts[i] && levels[j] == levels[i] &&
           at16[j] < end16 && (j - i < LONGEST || !starts[j])) {
      j++;
    }

    vec_push(&t->runs, ((Run){i, j, fonts[i], si, levels[i]}));
    i = j;
  }
}

// shapes a run, adding what each of its clusters takes to the graphemes in
// it.
static void shape(civue_text_system *t, Run *r, civue_text_style st,
                  f32 kern) {
  const Font *f = &t->fonts.v[r->font];
  IDWriteFontFace *face = t->faces.v[f->face].face;
  const u32 *cps = t->cps.v, *of16 = t->of16.v;
  const u8 *starts = t->starts.v;
  f32 *adv = t->adv.v;
  u32 p0 = t->at16.v[r->start], len = t->at16.v[r->end] - p0;
  const wchar_t *text = t->w16.v + p0;
  BOOL rtl = r->level & 1;
  DWRITE_SCRIPT_ANALYSIS sa = t->an.scripts.v[r->script].script;
  DWRITE_FONT_FEATURE tnum = {DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
  DWRITE_TYPOGRAPHIC_FEATURES typo = {&tnum, 1};
  const DWRITE_TYPOGRAPHIC_FEATURES *features = &typo;
  UINT32 feature_len = len;
  u32 nfeatures = st.tnum ? 1 : 0;
  u32 max = len * 3 / 2 + 16;
  UINT32 ng = 0;
  HRESULT hr = E_FAIL;
  r->g0 = r->g1 = t->shaped.len;

  vec_reserve(&t->clusters, len);
  vec_reserve(&t->tprops, len);

  for (u32 tries = 0; tries < 6; tries++) {
    t->gids.len = t->gprops.len = 0;
    vec_reserve(&t->gids, max);
    vec_reserve(&t->gprops, max);
    hr = IDWriteTextAnalyzer_GetGlyphs(
      t->analyzer, text, len, face, FALSE, rtl, &sa, t->locale, nullptr,
      nfeatures ? &features : nullptr, nfeatures ? &feature_len : nullptr,
      nfeatures, max, t->clusters.v, t->tprops.v, t->gids.v, t->gprops.v,
      &ng);

    if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER)) {
      max *= 2;
      continue;
    }

    // a script the face has no shaping for is set as no script at all.
    if (FAILED(hr) && sa.script) {
      sa = (DWRITE_SCRIPT_ANALYSIS){};
      continue;
    }

    break;
  }

  if (FAILED(hr) || !ng) {
    return;
  }

  t->gadv.len = t->goff.len = 0;
  vec_reserve(&t->gadv, ng);
  vec_reserve(&t->goff, ng);

  if (FAILED(IDWriteTextAnalyzer_GetGlyphPlacements(
        t->analyzer, text, t->clusters.v, t->tprops.v, len, t->gids.v,
        t->gprops.v, ng, face, f->px, FALSE, rtl, &sa, t->locale,
        nfeatures ? &features : nullptr, nfeatures ? &feature_len : nullptr,
        nfeatures, t->gadv.v, t->goff.v))) {
    return;
  }

  vec_reserve(&t->shaped, ng);
  Shaped *out = t->shaped.v + t->shaped.len;
  const u16 *clusters = t->clusters.v;

  for (u32 i = 0; i < len;) {
    u32 j = i + 1;

    while (j < len && clusters[j] == clusters[i]) {
      j++;
    }

    u32 g0 = clusters[i], g1 = j < len ? clusters[j] : ng;
    u32 cp0 = of16[p0 + i], cp1 = j < len ? of16[p0 + j] : r->end;
    bool hide = control(cps[cp0]);
    g1 = CLAMP(g1, g0, ng);
    f32 a = 0;

    for (u32 g = g0; g < g1 && !hide; g++) {
      a += t->gadv.v[g];
    }

    // a cluster of several graphemes, a ligature, shares its width among
    // them. one begun inside a grapheme widens it.
    u32 n = 0;

    for (u32 k = cp0; k < cp1; k++) {
      n += starts[k];
    }

    if (n) {
      for (u32 k = cp0; k < cp1; k++) {
        adv[k] += starts[k] ? a / n + (hide ? 0 : kern) : 0;
      }
    }
    else {
      u32 k = cp0;

      while (k > r->start && !starts[k]) {
        k--;
      }

      adv[k] += a;
    }

    for (u32 g = g0; g < g1; g++) {
      out[g] = (Shaped){t->gids.v[g],
                        hide,
                        cp0,
                        hide ? 0 : t->gadv.v[g],
                        t->goff.v[g].advanceOffset,
                        t->goff.v[g].ascenderOffset,
                        g + 1 == g1 && !hide ? kern * n : 0};
    }

    i = j;
  }

  t->shaped.len += ng;
  r->g1 = t->shaped.len;
}

// whether a line breaks before code point `i`, and must.
static u32 break_before(const civue_text_system *t, u32 i) {
  const DWRITE_LINE_BREAKPOINT *b = t->an.breaks.v;
  u32 u = t->at16.v[i];
  u32 after = b[u - 1].breakConditionAfter, before = b[u].breakConditionBefore;

  if (after == DWRITE_BREAK_CONDITION_MUST_BREAK ||
      before == DWRITE_BREAK_CONDITION_MUST_BREAK) {
    return 2;
  }

  return (after == DWRITE_BREAK_CONDITION_CAN_BREAK ||
          before == DWRITE_BREAK_CONDITION_CAN_BREAK) &&
         after != DWRITE_BREAK_CONDITION_MAY_NOT_BREAK &&
         before != DWRITE_BREAK_CONDITION_MAY_NOT_BREAK;
}

// lines, greedy, broken at the last chance before the width, at a grapheme
// when a word alone is wider. spaces hang past it.
static void break_lines(civue_text_system *t, f32 limit) {
  u32 count = t->cps.len, len16 = t->w16.len;
  const u32 *cps = t->cps.v;
  const u8 *starts = t->starts.v;
  const f32 *adv = t->adv.v;
  t->breaks.len = 0;

  if (limit > 0 && count) {
    t->an.breaks.len = 0;
    vec_reserve(&t->an.breaks, len16);
    t->an.breaks.len = len16;
    memset(t->an.breaks.v, 0, len16 * sizeof *t->an.breaks.v);
    IDWriteTextAnalyzer_AnalyzeLineBreakpoints(t->analyzer, &t->an.source, 0,
                                               len16, &t->an.sink);
    f32 w = 0;
    u32 start = 0, chance = UINT32_MAX;

    for (u32 i = 0; i < count; i++) {
      if (!starts[i]) {
        continue;
      }

      u32 can = i > start ? break_before(t, i) : 0;

      if (can == 2) {
        vec_push(&t->breaks, i);
        start = i, w = 0, chance = UINT32_MAX;
      }
      else {
        if (can) {
          chance = i;
        }

        if (!space(cps[i]) && w + adv[i] > limit && i > start) {
          u32 at = chance != UINT32_MAX ? chance : i;
          vec_push(&t->breaks, at);
          start = at, chance = UINT32_MAX, w = 0;

          for (u32 k = start; k < i; k++) {
            w += adv[k];
          }
        }
      }

      w += adv[i];
    }
  }

  if (!t->breaks.len || vec_last(&t->breaks) != count) {
    vec_push(&t->breaks, count);
  }
}

// a block with its glyphs, carets and lines after it, in one allocation.
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

// a line's runs in visual order: each level from the highest down to the
// lowest odd one reverses the sequences at least that high.
static void reorder(civue_text_system *t, u32 start, u32 end) {
  t->parts.len = 0;

  vec_each(&t->runs, r) {
    if (r->end <= start || r->start >= end) {
      continue;
    }

    Run part = *r;
    part.start = MAX(r->start, start);
    part.end = MIN(r->end, end);
    vec_push(&t->parts, part);
  }

  Run *lr = t->parts.v;
  u32 n = t->parts.len;
  u8 high = 0, low_odd = 255;

  for (u32 i = 0; i < n; i++) {
    high = MAX(high, lr[i].level);

    if (lr[i].level % 2) {
      low_odd = MIN(low_odd, lr[i].level);
    }
  }

  for (i32 lv = high; lv >= (i32)low_odd && lv > 0; lv--) {
    for (u32 i = 0; i < n;) {
      if (lr[i].level < lv) {
        i++;
        continue;
      }

      u32 j = i;

      while (j < n && lr[j].level >= lv) {
        j++;
      }

      for (u32 a = i, z = j - 1; a < z; a++, z--) {
        Run tmp = lr[a];
        lr[a] = lr[z];
        lr[z] = tmp;
      }

      i = j;
    }
  }
}

static civue_text_block *build(civue_text_system *t, const char *s, usize n,
                               civue_text_style st, f32 max_width, f32 scale) {
  t->cps.len = t->at.len = t->at16.len = t->of16.len = t->w16.len = 0;
  vec_reserve(&t->cps, n + 1);
  vec_reserve(&t->at, n + 1);
  vec_reserve(&t->at16, n + 1);
  vec_reserve(&t->of16, n + 1);
  vec_reserve(&t->w16, n + 1);
  bool ascii = true, ltr = true;

  for (usize i = 0; i < n;) {
    u32 k = t->cps.len;
    t->at.v[t->at.len++] = (u32)i;
    t->at16.v[t->at16.len++] = t->w16.len;
    u32 c = utf8_next(s, n, &i);
    t->cps.v[t->cps.len++] = c;
    ascii &= c < 0x80;
    ltr &= c < 0x590;

    if (c >= 0x10000) {
      t->w16.v[t->w16.len++] = (wchar_t)(0xd800 + ((c - 0x10000) >> 10));
      t->w16.v[t->w16.len++] = (wchar_t)(0xdc00 + ((c - 0x10000) & 0x3ff));
      t->of16.v[t->of16.len++] = k;
      t->of16.v[t->of16.len++] = k;
    }
    else {
      t->w16.v[t->w16.len++] = c == '\t' ? L' ' : (wchar_t)c;
      t->of16.v[t->of16.len++] = k;
    }
  }

  u32 count = t->cps.len;
  t->at.v[t->at.len++] = (u32)n;
  t->at16.v[t->at16.len++] = t->w16.len;
  t->of16.v[t->of16.len++] = count;
  t->an.text = t->w16.v;
  t->an.len = t->w16.len;
  const u32 *cps = t->cps.v, *at = t->at.v;

  f32 px = st.size * scale;
  u32 src = st.mono ? SRC_CODE : SRC_UI;

  if (st.display && st.display <= CIVUE_TEXT_FACES &&
      t->own[SRC_DISPLAY + st.display]) {
    src = SRC_DISPLAY + st.display;
  }

  u32 base = base_face(t, src, st.weight, st.italic, st.size);
  graphemes(t, ascii);
  choose_fonts(t, st, base, px, ascii);
  u8 base_level = bidi(t, ltr);
  scripts(t, ascii);
  itemize(t);

  t->adv.len = 0;
  vec_reserve(&t->adv, count + 1);
  t->adv.len = count + 1;
  memset(t->adv.v, 0, (count + 1) * sizeof *t->adv.v);
  t->shaped.len = 0;
  f32 kern = st.letter_spacing * scale;

  vec_each(&t->runs, r) {
    shape(t, r, st, kern);
  }

  break_lines(t, max_width > 0 ? max_width * scale : 0);

  f32 lh = st.line_height * scale;
  const Face *bf = &t->faces.v[base];
  f32 ascent = bf->ascent * px, descent = bf->descent * px;
  // css places the font's box in the middle of the line.
  f32 first = (lh - (ascent + descent)) / 2 + ascent;
  const f32 *adv = t->adv.v;
  const u8 *starts = t->starts.v;
  t->glyphs_made.len = t->carets_made.len = t->lines_made.len = 0;
  t->caret_x.len = 0;
  vec_reserve(&t->caret_x, count + 1);
  f32 *caret_x = t->caret_x.v;
  f32 width = 0;
  u32 start = 0, nlines = t->breaks.len;

  for (u32 line = 0; line < nlines; line++) {
    u32 end = t->breaks.v[line];
    f32 y = first + line * lh, x = 0;
    reorder(t, start, end);

    vec_each(&t->parts, p) {
      bool rtl = p->level % 2;
      f32 run_w = 0;

      for (u32 k = p->start; k < p->end; k++) {
        run_w += adv[k];
      }

      // the leading edge of each grapheme.
      f32 edge = rtl ? x + run_w : x;

      for (u32 k = p->start; k < p->end; k++) {
        caret_x[k] = edge;
        edge += rtl ? -adv[k] : adv[k];
      }

      // its glyphs of this line from the left, the spacing after a cluster
      // to its left when it goes from right to left.
      vec_reserve(&t->glyphs_made, p->g1 - p->g0);
      f32 pen = x;

      for (u32 i = 0; i < p->g1 - p->g0; i++) {
        const Shaped *g = &t->shaped.v[rtl ? p->g1 - 1 - i : p->g0 + i];

        if (g->cluster < p->start || g->cluster >= p->end) {
          continue;
        }

        pen += rtl ? g->kern : 0;

        if (!g->hidden) {
          t->glyphs_made.v[t->glyphs_made.len++] =
            (civue_glyph){at[g->cluster], p->font, g->id,
                          rtl ? pen - g->dx : pen + g->dx, y - g->dy};
        }

        pen += g->advance + (rtl ? 0 : g->kern);
      }

      x += run_w;
    }

    f32 trailing = 0;

    for (u32 k = end; k > start && space(cps[k - 1]); k--) {
      trailing += adv[k - 1];
    }

    width = fmaxf(width, x - trailing);
    vec_reserve(&t->carets_made, end - start + 1);

    for (u32 k = start; k < end; k++) {
      if (starts[k]) {
        t->carets_made.v[t->carets_made.len++] =
          (civue_caret){at[k], caret_x[k] / scale, line};
      }
    }

    // the end of the last line, after its trailing space.
    if (line + 1 == nlines) {
      t->carets_made.v[t->carets_made.len++] =
        (civue_caret){(u32)n, (base_level ? 0 : x) / scale, line};
    }

    vec_push(&t->lines_made, ((civue_text_line){at[start], at[end],
                                                line * lh / scale,
                                                y / scale}));
    start = end;
  }

  civue_text_block *b = block_of(t);
  b->scale = scale;
  b->baseline = first / scale;
  b->width = width / scale;
  b->height = nlines * lh / scale;
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
    free(old->block->cache);
    free(old->block);
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
      free(l->block->cache);
      free(l->block);
      continue;
    }

    t->layouts.v[k++] = *l;
    map_put(&t->layout_index, l->key, k);
  }

  t->layouts.len = k;
}

/* ========================================================================== */
/* rasterizing. coverage is directwrite's, antialiased in gray without        */
/* hinting as freetype and core text draw it. color glyphs are drawn here     */
/* from their colr v1 paint trees, their layers or their pictures,            */
/* premultiplied rgba.                                                        */
/* ========================================================================== */

static i32 floor_div(i32 v, i32 k) {
  return v >= 0 ? v / k : -((-v + k - 1) / k);
}

// the coverage of a glyph run under `m`, moved by `x` and `y` pixels, from
// malloc, and its box in pixels. an empty box has no pixels. directwrite
// antialiases in 17 levels of gray: the run is drawn larger and brought down
// to as many as freetype's and core text's.
static bool coverage(civue_text_system *t, const DWRITE_GLYPH_RUN *run,
                     const DWRITE_MATRIX *m, f32 x, f32 y, RECT *box,
                     u8 **px) {
  DWRITE_MATRIX at = m ? *m : (DWRITE_MATRIX){1, 0, 0, 1, 0, 0};
  f32 size = run->fontEmSize * fmaxf(hypotf(at.m11, at.m12),
                                     hypotf(at.m21, at.m22));
  i32 k = size <= 128 ? 4 : size <= 256 ? 2 : 1;
  DWRITE_MATRIX big = {at.m11 * k,     at.m12 * k,     at.m21 * k,
                       at.m22 * k,     (at.dx + x) * k, (at.dy + y) * k};
  IDWriteGlyphRunAnalysis *a = nullptr;
  RECT r;
  *px = nullptr;
  *box = (RECT){};

  if (FAILED(IDWriteFactory2_CreateGlyphRunAnalysis(
        t->dw, run, &big, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
        DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DISABLED,
        DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, 0, 0, &a))) {
    return false;
  }

  defer IDWriteGlyphRunAnalysis_Release(a);

  if (FAILED(IDWriteGlyphRunAnalysis_GetAlphaTextureBounds(
        a, DWRITE_TEXTURE_ALIASED_1x1, &r))) {
    return false;
  }

  if (r.right <= r.left || r.bottom <= r.top) {
    return true;
  }

  u32 fw = (u32)(r.right - r.left), fh = (u32)(r.bottom - r.top);
  t->fine.len = 0;
  vec_reserve(&t->fine, fw * fh);

  if (FAILED(IDWriteGlyphRunAnalysis_CreateAlphaTexture(
        a, DWRITE_TEXTURE_ALIASED_1x1, &r, t->fine.v, fw * fh))) {
    return false;
  }

  *box = (RECT){floor_div(r.left, k), floor_div(r.top, k),
                floor_div(r.right + k - 1, k), floor_div(r.bottom + k - 1, k)};
  u32 w = (u32)(box->right - box->left), h = (u32)(box->bottom - box->top);
  t->sums.len = 0;
  vec_reserve(&t->sums, w * h);
  memset(t->sums.v, 0, (usize)w * h * sizeof *t->sums.v);

  for (u32 fy = 0; fy < fh; fy++) {
    i32 y0 = floor_div(r.top + (i32)fy, k) - box->top;
    u16 *sums = t->sums.v + (usize)y0 * w;
    const u8 *src = t->fine.v + (usize)fy * fw;

    for (u32 fx = 0; fx < fw; fx++) {
      sums[floor_div(r.left + (i32)fx, k) - box->left] += src[fx];
    }
  }

  *px = xmalloc((usize)w * h);

  for (usize i = 0; i < (usize)w * h; i++) {
    (*px)[i] = (u8)((t->sums.v[i] + k * k / 2) / (k * k));
  }

  return true;
}

static DWRITE_GLYPH_RUN glyph_run(IDWriteFontFace *face, f32 px,
                                  const u16 *id) {
  static const FLOAT advance = 0;
  static const DWRITE_GLYPH_OFFSET offset = {};
  return (DWRITE_GLYPH_RUN){face, px, 1, id, &advance, &offset, FALSE, 0};
}

// a glyph's coverage and where it goes from its pen position, a pixel of
// room around it.
static bool rasterize_mask(civue_text_system *t, const Font *f, u16 id,
                           u32 subpixel, civue_bytes *out, i32 *left,
                           i32 *top, u32 *w, u32 *h) {
  DWRITE_GLYPH_RUN run = glyph_run(t->faces.v[f->face].face, f->px, &id);
  RECT r;
  u8 *px;
  *w = *h = 0;

  if (!coverage(t, &run, nullptr, (f32)subpixel / SUBPIXELS, 0, &r, &px)) {
    return false;
  }

  if (!px) {
    return true;
  }

  u32 bw = (u32)(r.right - r.left), bh = (u32)(r.bottom - r.top);
  *w = bw + 2, *h = bh + 2;
  out->len = 0;
  vec_reserve(out, (usize)*w * *h);
  memset(out->v, 0, (usize)*w * *h);
  out->len = (usize)*w * *h;

  for (u32 y = 0; y < bh; y++) {
    memcpy(out->v + (usize)(y + 1) * *w + 1, px + (usize)y * bw, bw);
  }

  free(px);
  *left = r.left - 1;
  *top = r.top - 1;
  return true;
}

// pixels of premultiplied rgba as floats, over a box of the glyph's pixels
// from its pen position.
typedef struct {
  i32 x0, y0;
  u32 w, h;
  f32 *px;
} Canvas;

static void over(f32 *d, const f32 *s) {
  f32 k = 1 - s[3];
  d[0] = s[0] + d[0] * k;
  d[1] = s[1] + d[1] * k;
  d[2] = s[2] + d[2] * k;
  d[3] = s[3] + d[3] * k;
}

// coverage `cov` over the box `r` of the canvas, in `color`.
static void fill_coverage(Canvas *c, const RECT *r, const u8 *cov,
                          const f32 color[4]) {
  u32 bw = (u32)(r->right - r->left);

  for (i32 y = MAX(r->top, c->y0); y < MIN(r->bottom, c->y0 + (i32)c->h);
       y++) {
    for (i32 x = MAX(r->left, c->x0); x < MIN(r->right, c->x0 + (i32)c->w);
         x++) {
      f32 a = cov[(usize)(y - r->top) * bw + (x - r->left)] / 255.f;

      if (a > 0) {
        f32 s[4] = {color[0] * a, color[1] * a, color[2] * a, color[3] * a};
        over(&c->px[((usize)(y - c->y0) * c->w + (x - c->x0)) * 4], s);
      }
    }
  }
}

// ---- colr v1 paint trees ----

// a paint tree drawn on a canvas, its coordinates ems going down from the
// glyph's origin.
typedef struct {
  civue_text_system *t;
  IDWritePaintReader *reader;
  IDWriteFontFace *face;
  f32 px;
  Canvas *canvas;
  // canvases of their own for what is composited, taken as deep as the tree
  // goes.
  Vec(f32 *) spare;
  u32 used;
} Painter;

static f32 *take(Painter *p) {
  usize size = (usize)p->canvas->w * p->canvas->h * 4;

  if (p->used == p->spare.len) {
    vec_push(&p->spare, xmalloc(size * sizeof(f32)));
  }

  f32 *c = p->spare.v[p->used++];
  memset(c, 0, size * sizeof(f32));
  return c;
}

// `a` then `b`.
static DWRITE_MATRIX then(DWRITE_MATRIX a, DWRITE_MATRIX b) {
  return (DWRITE_MATRIX){a.m11 * b.m11 + a.m12 * b.m21,
                         a.m11 * b.m12 + a.m12 * b.m22,
                         a.m21 * b.m11 + a.m22 * b.m21,
                         a.m21 * b.m12 + a.m22 * b.m22,
                         a.dx * b.m11 + a.dy * b.m21 + b.dx,
                         a.dx * b.m12 + a.dy * b.m22 + b.dy};
}

static bool fill_kind(DWRITE_PAINT_TYPE type) {
  return type == DWRITE_PAINT_TYPE_SOLID ||
         type == DWRITE_PAINT_TYPE_LINEAR_GRADIENT ||
         type == DWRITE_PAINT_TYPE_RADIAL_GRADIENT ||
         type == DWRITE_PAINT_TYPE_SWEEP_GRADIENT;
}

constexpr u32 RAMP = 256;

// a solid color or a gradient, found at a canvas pixel.
typedef struct {
  DWRITE_PAINT_TYPE type;
  f32 color[4];
  // canvas pixels to paint space.
  DWRITE_MATRIX inverse;
  // a linear gradient's start and its vector over its squared length, a
  // radial's circles, a sweep's center and angles.
  f32 x0, y0, vx, vy;
  f32 r0, x1, y1, r1;
  f32 start, end;
  u32 extend;
  // the first and last stops, and the colors between them.
  f32 t0, t1;
  f32 ramp[RAMP][4];
} Fill;

static void premultiply(f32 out[4], D2D1_COLOR_F c) {
  out[0] = c.r * c.a;
  out[1] = c.g * c.a;
  out[2] = c.b * c.a;
  out[3] = c.a;
}

static int stop_order(const void *a, const void *b) {
  f32 x = ((const D2D1_GRADIENT_STOP *)a)->position,
      y = ((const D2D1_GRADIENT_STOP *)b)->position;
  return (x > y) - (x < y);
}

// the stops of the gradient the reader is at, interpolated premultiplied.
static void ramp(Painter *p, Fill *f, u32 count) {
  D2D1_GRADIENT_STOP stops[64];
  count = MIN(count, (u32)countof(stops));

  if (!count ||
      FAILED(IDWritePaintReader_GetGradientStops(p->reader, 0, count, stops))) {
    f->t0 = 0, f->t1 = 1;
    memset(f->ramp, 0, sizeof f->ramp);
    return;
  }

  qsort(stops, count, sizeof *stops, stop_order);
  f->t0 = stops[0].position;
  f->t1 = stops[count - 1].position;
  u32 k = 0;

  for (u32 i = 0; i < RAMP; i++) {
    f32 at = f->t0 + (f->t1 - f->t0) * i / (RAMP - 1);

    while (k + 1 < count - 1 && stops[k + 1].position < at) {
      k++;
    }

    f32 a[4], b[4];
    const D2D1_GRADIENT_STOP *s0 = &stops[k],
                             *s1 = &stops[MIN(k + 1, count - 1)];
    premultiply(a, s0->color);
    premultiply(b, s1->color);
    f32 span = s1->position - s0->position;
    f32 u = span > 1e-6f ? CLAMP((at - s0->position) / span, 0, 1) : 1;

    for (u32 c = 0; c < 4; c++) {
      f->ramp[i][c] = a[c] + (b[c] - a[c]) * u;
    }
  }
}

static void fill_of(Painter *p, const DWRITE_PAINT_ELEMENT *e,
                    DWRITE_MATRIX m, Fill *f) {
  *f = (Fill){.type = e->paintType};
  f32 det = m.m11 * m.m22 - m.m12 * m.m21;

  if (fabsf(det) < 1e-12f) {
    f->type = DWRITE_PAINT_TYPE_NONE;
    return;
  }

  f->inverse = (DWRITE_MATRIX){m.m22 / det,
                               -m.m12 / det,
                               -m.m21 / det,
                               m.m11 / det,
                               (m.m21 * m.dy - m.m22 * m.dx) / det,
                               (m.m12 * m.dx - m.m11 * m.dy) / det};

  switch (e->paintType) {
  case DWRITE_PAINT_TYPE_SOLID:
    premultiply(f->color, *(const D2D1_COLOR_F *)&e->paint.solid.value);
    break;

  case DWRITE_PAINT_TYPE_LINEAR_GRADIENT: {
    const struct PAINT_LINEAR_GRADIENT *g = &e->paint.linearGradient;
    // colors are alike along lines parallel to p0 p2: the gradient goes
    // from p0 to where p1 projects on the normal of those lines.
    f32 nx = g->y2 - g->y0, ny = -(g->x2 - g->x0);
    f32 nn = nx * nx + ny * ny;
    f32 dot = (g->x1 - g->x0) * nx + (g->y1 - g->y0) * ny;
    f32 vx = nn > 0 ? nx * dot / nn : g->x1 - g->x0;
    f32 vy = nn > 0 ? ny * dot / nn : g->y1 - g->y0;
    f32 vv = vx * vx + vy * vy;

    if (vv < 1e-12f) {
      f->type = DWRITE_PAINT_TYPE_NONE;
      return;
    }

    f->x0 = g->x0, f->y0 = g->y0, f->vx = vx / vv, f->vy = vy / vv;
    f->extend = g->extendMode;
    ramp(p, f, g->gradientStopCount);
    break;
  }

  case DWRITE_PAINT_TYPE_RADIAL_GRADIENT: {
    const struct PAINT_RADIAL_GRADIENT *g = &e->paint.radialGradient;
    f->x0 = g->x0, f->y0 = g->y0, f->r0 = g->radius0;
    f->x1 = g->x1, f->y1 = g->y1, f->r1 = g->radius1;
    f->extend = g->extendMode;
    ramp(p, f, g->gradientStopCount);
    break;
  }

  case DWRITE_PAINT_TYPE_SWEEP_GRADIENT: {
    const struct PAINT_SWEEP_GRADIENT *g = &e->paint.sweepGradient;
    f->x0 = g->centerX, f->y0 = g->centerY;
    f->start = g->startAngle, f->end = g->endAngle;
    f->extend = g->extendMode;
    ramp(p, f, g->gradientStopCount);
    break;
  }

  default:
    f->type = DWRITE_PAINT_TYPE_NONE;
    break;
  }
}

// the color of a gradient at `u`, its stops' positions, extended past them
// as it says.
static const f32 *ramp_at(const Fill *f, f32 u) {
  f32 span = f->t1 - f->t0;

  if (span < 1e-6f) {
    return f->ramp[u < f->t0 ? 0 : RAMP - 1];
  }

  f32 v = (u - f->t0) / span;

  if (f->extend == D2D1_EXTEND_MODE_WRAP) {
    v -= floorf(v);
  }
  else if (f->extend == D2D1_EXTEND_MODE_MIRROR) {
    v = fabsf(v - 2 * floorf(v / 2));
    v = v > 1 ? 2 - v : v;
  }

  return f->ramp[(u32)lroundf(CLAMP(v, 0, 1) * (RAMP - 1))];
}

// the color at canvas pixel x, y, false where the gradient has none.
static bool fill_at(const Fill *f, f32 x, f32 y, f32 out[4]) {
  static const f32 clear[4] = {};

  if (f->type == DWRITE_PAINT_TYPE_SOLID) {
    memcpy(out, f->color, sizeof f->color);
    return true;
  }

  const DWRITE_MATRIX *m = &f->inverse;
  f32 px = x * m->m11 + y * m->m21 + m->dx;
  f32 py = x * m->m12 + y * m->m22 + m->dy;
  const f32 *c = clear;

  switch (f->type) {
  case DWRITE_PAINT_TYPE_LINEAR_GRADIENT:
    c = ramp_at(f, (px - f->x0) * f->vx + (py - f->y0) * f->vy);
    break;

  case DWRITE_PAINT_TYPE_RADIAL_GRADIENT: {
    // the largest u of the circle between the two that passes through the
    // point with a radius not below zero.
    f32 cdx = f->x1 - f->x0, cdy = f->y1 - f->y0, dr = f->r1 - f->r0;
    f32 pdx = px - f->x0, pdy = py - f->y0;
    f32 a = cdx * cdx + cdy * cdy - dr * dr;
    f32 b = pdx * cdx + pdy * cdy + f->r0 * dr;
    f32 cc = pdx * pdx + pdy * pdy - f->r0 * f->r0;
    f32 u;

    if (fabsf(a) < 1e-9f) {
      if (fabsf(b) < 1e-9f) {
        return false;
      }

      u = cc / (2 * b);
    }
    else {
      f32 disc = b * b - a * cc;

      if (disc < 0) {
        return false;
      }

      f32 root = sqrtf(disc);
      u = (b + root) / a;

      if (f->r0 + u * dr < 0) {
        u = (b - root) / a;
      }
    }

    if (f->r0 + u * dr < 0) {
      return false;
    }

    c = ramp_at(f, u);
    break;
  }

  case DWRITE_PAINT_TYPE_SWEEP_GRADIENT: {
    // directwrite keeps the font's angles, counterclockwise with y up.
    f32 deg = atan2f(-(py - f->y0), px - f->x0) * (180 / (f32)M_PI);
    deg = deg < 0 ? deg + 360 : deg;
    f32 span = f->end - f->start;

    if (fabsf(span) < 1e-6f) {
      return false;
    }

    c = ramp_at(f, (deg - f->start) / span);
    break;
  }

  default:
    return false;
  }

  memcpy(out, c, 4 * sizeof *c);
  return true;
}

// paints a fill over `dst`, through `cov` over the box `r` when given.
static void fill_paint(Painter *p, const Fill *f, f32 *dst, const RECT *r,
                       const u8 *cov) {
  const Canvas *c = p->canvas;
  i32 x0 = 0, y0 = 0, x1 = (i32)c->w, y1 = (i32)c->h;

  if (f->type == DWRITE_PAINT_TYPE_NONE) {
    return;
  }

  if (r) {
    x0 = MAX(x0, r->left - c->x0), y0 = MAX(y0, r->top - c->y0);
    x1 = MIN(x1, r->right - c->x0), y1 = MIN(y1, r->bottom - c->y0);
  }

  for (i32 y = y0; y < y1; y++) {
    for (i32 x = x0; x < x1; x++) {
      f32 a = 1;

      if (r) {
        a = cov[(usize)(y + c->y0 - r->top) * (r->right - r->left) +
                (x + c->x0 - r->left)] /
            255.f;

        if (a <= 0) {
          continue;
        }
      }

      f32 s[4];

      if (!fill_at(f, x + .5f, y + .5f, s)) {
        continue;
      }

      for (u32 k = 0; k < 4; k++) {
        s[k] *= a;
      }

      over(&dst[((usize)y * c->w + x) * 4], s);
    }
  }
}

// a glyph's outline in canvas pixels, from ems under `m`.
static bool glyph_coverage(Painter *p, u32 glyph, DWRITE_MATRIX m, RECT *r,
                           u8 **cov) {
  u16 id = (u16)glyph;
  DWRITE_GLYPH_RUN run = glyph_run(p->face, p->px, &id);
  // the run is set in pixels, the paint in ems.
  DWRITE_MATRIX a = {m.m11 / p->px, m.m12 / p->px, m.m21 / p->px,
                     m.m22 / p->px, m.dx + p->canvas->x0,
                     m.dy + p->canvas->y0};
  return coverage(p->t, &run, &a, 0, 0, r, cov) && *cov;
}

static f32 blend(DWRITE_COLOR_COMPOSITE_MODE mode, f32 s, f32 d) {
  switch (mode) {
  case DWRITE_COLOR_COMPOSITE_SCREEN:
    return s + d - s * d;
  case DWRITE_COLOR_COMPOSITE_OVERLAY:
    return d <= .5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
  case DWRITE_COLOR_COMPOSITE_DARKEN:
    return fminf(s, d);
  case DWRITE_COLOR_COMPOSITE_LIGHTEN:
    return fmaxf(s, d);
  case DWRITE_COLOR_COMPOSITE_COLOR_DODGE:
    return d <= 0 ? 0 : s >= 1 ? 1 : fminf(1, d / (1 - s));
  case DWRITE_COLOR_COMPOSITE_COLOR_BURN:
    return d >= 1 ? 1 : s <= 0 ? 0 : 1 - fminf(1, (1 - d) / s);
  case DWRITE_COLOR_COMPOSITE_HARD_LIGHT:
    return s <= .5f ? 2 * s * d : 1 - 2 * (1 - s) * (1 - d);
  case DWRITE_COLOR_COMPOSITE_SOFT_LIGHT: {
    if (s <= .5f) {
      return d - (1 - 2 * s) * d * (1 - d);
    }

    f32 g = d <= .25f ? ((16 * d - 12) * d + 4) * d : sqrtf(d);
    return d + (2 * s - 1) * (g - d);
  }
  case DWRITE_COLOR_COMPOSITE_DIFFERENCE:
    return fabsf(s - d);
  case DWRITE_COLOR_COMPOSITE_EXCLUSION:
    return s + d - 2 * s * d;
  case DWRITE_COLOR_COMPOSITE_MULTIPLY:
    return s * d;
  default:
    return s;
  }
}

// `src` composited onto `dst` in `mode`, into `dst`. the hue, saturation,
// color and luminosity modes go over it.
static void composite(f32 *dst, const f32 *src, usize n,
                      DWRITE_COLOR_COMPOSITE_MODE mode) {
  for (usize i = 0; i < n; i++, src += 4, dst += 4) {
    f32 sa = src[3], da = dst[3], fs = 0, fd = 0;
    bool porter = true;

    switch (mode) {
    case DWRITE_COLOR_COMPOSITE_CLEAR:
      break;
    case DWRITE_COLOR_COMPOSITE_SRC:
      fs = 1;
      break;
    case DWRITE_COLOR_COMPOSITE_DEST:
      fd = 1;
      break;
    case DWRITE_COLOR_COMPOSITE_DEST_OVER:
      fs = 1 - da, fd = 1;
      break;
    case DWRITE_COLOR_COMPOSITE_SRC_IN:
      fs = da;
      break;
    case DWRITE_COLOR_COMPOSITE_DEST_IN:
      fd = sa;
      break;
    case DWRITE_COLOR_COMPOSITE_SRC_OUT:
      fs = 1 - da;
      break;
    case DWRITE_COLOR_COMPOSITE_DEST_OUT:
      fd = 1 - sa;
      break;
    case DWRITE_COLOR_COMPOSITE_SRC_ATOP:
      fs = da, fd = 1 - sa;
      break;
    case DWRITE_COLOR_COMPOSITE_DEST_ATOP:
      fs = 1 - da, fd = sa;
      break;
    case DWRITE_COLOR_COMPOSITE_XOR:
      fs = 1 - da, fd = 1 - sa;
      break;
    case DWRITE_COLOR_COMPOSITE_PLUS:
      for (u32 k = 0; k < 4; k++) {
        dst[k] = fminf(1, src[k] + dst[k]);
      }
      continue;
    case DWRITE_COLOR_COMPOSITE_SRC_OVER:
    case DWRITE_COLOR_COMPOSITE_HSL_HUE:
    case DWRITE_COLOR_COMPOSITE_HSL_SATURATION:
    case DWRITE_COLOR_COMPOSITE_HSL_COLOR:
    case DWRITE_COLOR_COMPOSITE_HSL_LUMINOSITY:
      fs = 1, fd = 1 - sa;
      break;
    default:
      porter = false;
      break;
    }

    if (porter) {
      for (u32 k = 0; k < 4; k++) {
        dst[k] = src[k] * fs + dst[k] * fd;
      }

      continue;
    }

    f32 out[4];

    for (u32 k = 0; k < 3; k++) {
      f32 s = sa > 0 ? src[k] / sa : 0, d = da > 0 ? dst[k] / da : 0;
      out[k] =
        src[k] * (1 - da) + dst[k] * (1 - sa) + sa * da * blend(mode, s, d);
    }

    out[3] = sa + da - sa * da;
    memcpy(dst, out, sizeof out);
  }
}

static void paint(Painter *p, const DWRITE_PAINT_ELEMENT *e, DWRITE_MATRIX m,
                  f32 *dst);

// paints the first `count` children of the element the reader is at, each
// over the last.
static void paint_children(Painter *p, u32 count, DWRITE_MATRIX m, f32 *dst) {
  DWRITE_PAINT_ELEMENT c;

  if (!count ||
      FAILED(IDWritePaintReader_MoveToFirstChild(p->reader, &c, sizeof c))) {
    return;
  }

  for (u32 i = 0; i < count; i++) {
    if (i &&
        FAILED(IDWritePaintReader_MoveToNextSibling(p->reader, &c, sizeof c))) {
      break;
    }

    paint(p, &c, m, dst);
  }

  IDWritePaintReader_MoveToParent(p->reader);
}

// the element the reader is at, painted over `dst` under `m`, ems to canvas
// pixels.
static void paint(Painter *p, const DWRITE_PAINT_ELEMENT *e, DWRITE_MATRIX m,
                  f32 *dst) {
  usize n = (usize)p->canvas->w * p->canvas->h;
  Fill *fill = nullptr;
  defer free(fill);

  switch (e->paintType) {
  case DWRITE_PAINT_TYPE_LAYERS:
    paint_children(p, e->paint.layers.childCount, m, dst);
    break;

  case DWRITE_PAINT_TYPE_SOLID_GLYPH: {
    RECT r;
    u8 *cov;

    if (glyph_coverage(p, e->paint.solidGlyph.glyphIndex, m, &r, &cov)) {
      fill = xmalloc(sizeof *fill);
      *fill = (Fill){.type = DWRITE_PAINT_TYPE_SOLID};
      premultiply(fill->color,
                  *(const D2D1_COLOR_F *)&e->paint.solidGlyph.color.value);
      fill_paint(p, fill, dst, &r, cov);
      free(cov);
    }

    break;
  }

  case DWRITE_PAINT_TYPE_SOLID:
  case DWRITE_PAINT_TYPE_LINEAR_GRADIENT:
  case DWRITE_PAINT_TYPE_RADIAL_GRADIENT:
  case DWRITE_PAINT_TYPE_SWEEP_GRADIENT:
    fill = xmalloc(sizeof *fill);
    fill_of(p, e, m, fill);
    fill_paint(p, fill, dst, nullptr, nullptr);
    break;

  case DWRITE_PAINT_TYPE_GLYPH: {
    RECT r;
    u8 *cov;
    DWRITE_PAINT_ELEMENT c;

    if (!glyph_coverage(p, e->paint.glyph.glyphIndex, m, &r, &cov)) {
      break;
    }

    defer free(cov);

    if (FAILED(IDWritePaintReader_MoveToFirstChild(p->reader, &c, sizeof c))) {
      break;
    }

    // a fill is found only where the glyph is, anything else is painted
    // whole and then cut.
    if (fill_kind(c.paintType)) {
      fill = xmalloc(sizeof *fill);
      fill_of(p, &c, m, fill);
      fill_paint(p, fill, dst, &r, cov);
    }
    else {
      f32 *layer = take(p);
      paint(p, &c, m, layer);
      const Canvas *cv = p->canvas;
      u32 bw = (u32)(r.right - r.left);

      for (i32 y = MAX(r.top, cv->y0); y < MIN(r.bottom, cv->y0 + (i32)cv->h);
           y++) {
        for (i32 x = MAX(r.left, cv->x0);
             x < MIN(r.right, cv->x0 + (i32)cv->w); x++) {
          f32 a = cov[(usize)(y - r.top) * bw + (x - r.left)] / 255.f;
          usize at = ((usize)(y - cv->y0) * cv->w + (x - cv->x0)) * 4;
          f32 s[4] = {layer[at] * a, layer[at + 1] * a, layer[at + 2] * a,
                      layer[at + 3] * a};
          over(&dst[at], s);
        }
      }

      p->used--;
    }

    IDWritePaintReader_MoveToParent(p->reader);
    break;
  }

  case DWRITE_PAINT_TYPE_COLOR_GLYPH:
    paint_children(p, 1, m, dst);
    break;

  case DWRITE_PAINT_TYPE_TRANSFORM:
    paint_children(p, 1, then(e->paint.transform, m), dst);
    break;

  case DWRITE_PAINT_TYPE_COMPOSITE: {
    DWRITE_PAINT_ELEMENT c;

    if (FAILED(IDWritePaintReader_MoveToFirstChild(p->reader, &c, sizeof c))) {
      break;
    }

    // the source, then the backdrop it is composited onto.
    f32 *src = take(p), *back = take(p);
    paint(p, &c, m, src);

    if (SUCCEEDED(
          IDWritePaintReader_MoveToNextSibling(p->reader, &c, sizeof c))) {
      paint(p, &c, m, back);
    }

    IDWritePaintReader_MoveToParent(p->reader);
    composite(back, src, n, e->paint.composite.mode);

    for (usize i = 0; i < n; i++) {
      over(&dst[i * 4], &back[i * 4]);
    }

    p->used -= 2;
    break;
  }

  default:
    break;
  }
}

// ---- color glyphs ----

typedef enum : u8 { PIECE_COVERAGE, PIECE_PAINT, PIECE_IMAGE } PieceKind;

// what a color glyph is drawn from, in order: coverage in a color, a glyph
// of the paint tree, or a picture.
typedef struct {
  PieceKind kind;
  RECT box;
  u8 *cov;
  f32 color[4];
  u16 glyph;
  f32 x, y;
  // a picture's premultiplied rgba, and its pixels per glyph pixel.
  u8 *rgba;
  u32 w, h;
  f32 scale;
} Piece;

// a picture glyph's image, `formats` the ones it has, as premultiplied
// rgba.
static bool picture(IDWriteFontFace *face, u16 id, f32 px,
                    DWRITE_GLYPH_IMAGE_FORMATS formats, f32 x, f32 y,
                    Piece *out) {
  IDWriteFontFace4 *f4 = nullptr;

  if (FAILED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace4,
                                            (void **)&f4))) {
    return false;
  }

  defer IDWriteFontFace4_Release(f4);

  static const DWRITE_GLYPH_IMAGE_FORMATS read[] = {
    DWRITE_GLYPH_IMAGE_FORMATS_PNG, DWRITE_GLYPH_IMAGE_FORMATS_JPEG,
    DWRITE_GLYPH_IMAGE_FORMATS_TIFF,
    DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8};
  DWRITE_GLYPH_IMAGE_FORMATS format = DWRITE_GLYPH_IMAGE_FORMATS_NONE;

  for (u32 i = 0; i < countof(read) && !format; i++) {
    format = formats & read[i];
  }

  if (!format) {
    return false;
  }

  DWRITE_GLYPH_IMAGE_DATA data;
  void *context = nullptr;

  if (FAILED(IDWriteFontFace4_GetGlyphImageData(
        f4, id, (UINT32)MAX(lroundf(px), 1), format, &data, &context))) {
    return false;
  }

  defer IDWriteFontFace4_ReleaseGlyphImageData(f4, context);

  u32 w = 0, h = 0;
  u8 *rgba = nullptr;

  if (format == DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8) {
    w = data.pixelSize.width, h = data.pixelSize.height;

    if (!w || !h || data.imageDataSize < (usize)w * h * 4) {
      return false;
    }

    rgba = xmalloc((usize)w * h * 4);
    const u8 *src = data.imageData;

    for (usize i = 0; i < (usize)w * h; i++) {
      rgba[i * 4] = src[i * 4 + 2];
      rgba[i * 4 + 1] = src[i * 4 + 1];
      rgba[i * 4 + 2] = src[i * 4];
      rgba[i * 4 + 3] = src[i * 4 + 3];
    }
  }
  else {
    rgba = civue__image_decode(data.imageData, data.imageDataSize, 0, &w, &h);
  }

  if (!rgba || !w || !h) {
    free(rgba);
    return false;
  }

  f32 k = px / (f32)MAX(data.pixelsPerEm, 1u);
  f32 left = x - data.horizontalLeftOrigin.x * k;
  f32 top = y - data.horizontalLeftOrigin.y * k;
  *out = (Piece){.kind = PIECE_IMAGE,
                 .box = {(LONG)floorf(left), (LONG)floorf(top),
                         (LONG)ceilf(left + w * k), (LONG)ceilf(top + h * k)},
                 .x = left,
                 .y = top,
                 .rgba = rgba,
                 .w = w,
                 .h = h,
                 .scale = k};
  return true;
}

// a picture over the canvas, sampled between its pixels.
static void draw_picture(Canvas *c, const Piece *pc) {
  for (u32 y = 0; y < c->h; y++) {
    for (u32 x = 0; x < c->w; x++) {
      f32 u = (c->x0 + x + .5f - pc->x) / pc->scale - .5f;
      f32 v = (c->y0 + y + .5f - pc->y) / pc->scale - .5f;

      if (u < -1 || v < -1 || u > pc->w || v > pc->h) {
        continue;
      }

      i32 u0 = (i32)floorf(u), v0 = (i32)floorf(v);
      f32 fu = u - u0, fv = v - v0, s[4] = {};

      for (u32 k = 0; k < 4; k++) {
        i32 su = u0 + (i32)(k & 1), sv = v0 + (i32)(k >> 1);

        if (su < 0 || sv < 0 || su >= (i32)pc->w || sv >= (i32)pc->h) {
          continue;
        }

        f32 wgt = (k & 1 ? fu : 1 - fu) * (k >> 1 ? fv : 1 - fv);
        const u8 *src = &pc->rgba[((usize)sv * pc->w + su) * 4];

        for (u32 ch = 0; ch < 4; ch++) {
          s[ch] += src[ch] / 255.f * wgt;
        }
      }

      over(&c->px[((usize)y * c->w + x) * 4], s);
    }
  }
}

// a color glyph's premultiplied rgba and where it goes from its pen
// position, false when the glyph has no colors.
static bool rasterize_color(civue_text_system *t, const Font *f, u16 id,
                            civue_bytes *out, i32 *left, i32 *top, u32 *w,
                            u32 *h) {
  IDWriteFontFace *face = t->faces.v[f->face].face;
  DWRITE_GLYPH_RUN run = glyph_run(face, f->px, &id);
  D2D1_POINT_2F origin = {0, 0};
  DWRITE_GLYPH_IMAGE_FORMATS formats = DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE |
                                       DWRITE_GLYPH_IMAGE_FORMATS_CFF |
                                       PICTURES;
  IDWriteColorGlyphRunEnumerator1 *runs = nullptr;
  HRESULT hr = E_FAIL;

  if (t->dw8) {
    hr = IDWriteFactory8_TranslateColorGlyphRun(
      t->dw8, origin, &run, nullptr, formats,
      DWRITE_PAINT_FEATURE_LEVEL_COLR_V1, DWRITE_MEASURING_MODE_NATURAL,
      nullptr, 0, &runs);
  }
  else if (t->dw4) {
    hr = IDWriteFactory4_TranslateColorGlyphRun(
      t->dw4, origin, &run, nullptr,
      formats & ~DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,
      DWRITE_MEASURING_MODE_NATURAL, nullptr, 0, &runs);
  }

  if (FAILED(hr) || !runs) {
    return false;
  }

  defer IDWriteColorGlyphRunEnumerator1_Release(runs);

  Vec(Piece) pieces = {};
  defer {
    vec_each(&pieces, pc) {
      free(pc->cov);
      free(pc->rgba);
    }

    vec_free(&pieces);
  }

  // the color of text, for layers drawn in it, is white: a color glyph is
  // drawn untinted.
  static const f32 white[4] = {1, 1, 1, 1};
  BOOL more = FALSE;

  while (SUCCEEDED(IDWriteColorGlyphRunEnumerator1_MoveNext(runs, &more)) &&
         more) {
    const DWRITE_COLOR_GLYPH_RUN1 *r = nullptr;

    if (FAILED(IDWriteColorGlyphRunEnumerator1_GetCurrentRun(runs, &r))) {
      break;
    }

    const DWRITE_GLYPH_RUN *g = &r->glyphRun;
    f32 x = r->baselineOriginX, y = r->baselineOriginY;

    switch (r->glyphImageFormat) {
    case DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE:
      for (u32 i = 0; i < g->glyphCount; i++) {
        vec_push(&pieces,
                 ((Piece){.kind = PIECE_PAINT,
                          .glyph = g->glyphIndices[i],
                          .x = x,
                          .y = y}));
        x += g->glyphAdvances ? g->glyphAdvances[i] : 0;
      }
      break;

    case DWRITE_GLYPH_IMAGE_FORMATS_PNG:
    case DWRITE_GLYPH_IMAGE_FORMATS_JPEG:
    case DWRITE_GLYPH_IMAGE_FORMATS_TIFF:
    case DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8:
      for (u32 i = 0; i < g->glyphCount; i++) {
        Piece pc;

        if (picture(face, g->glyphIndices[i], g->fontEmSize,
                    r->glyphImageFormat, x, y, &pc)) {
          vec_push(&pieces, pc);
        }

        x += g->glyphAdvances ? g->glyphAdvances[i] : 0;
      }
      break;

    default: {
      Piece pc = {.kind = PIECE_COVERAGE};

      if (!coverage(t, g, nullptr, x, y, &pc.box, &pc.cov) || !pc.cov) {
        break;
      }

      if (r->paletteIndex == 0xffff) {
        memcpy(pc.color, white, sizeof white);
      }
      else {
        premultiply(pc.color, *(const D2D1_COLOR_F *)&r->runColor);
      }

      vec_push(&pieces, pc);
      break;
    }
    }
  }

  IDWriteFontFace7 *f7 = nullptr;
  IDWritePaintReader *reader = nullptr;
  defer {
    if (reader) {
      IDWritePaintReader_Release(reader);
    }

    if (f7) {
      IDWriteFontFace7_Release(f7);
    }
  }

  // a paint tree's glyph is as large as its clip box says, or its em.
  vec_each(&pieces, pc) {
    if (pc->kind != PIECE_PAINT) {
      continue;
    }

    if (!reader &&
        (FAILED(IDWriteFontFace_QueryInterface(face, &IID_IDWriteFontFace7,
                                               (void **)&f7)) ||
         FAILED(IDWriteFontFace7_CreatePaintReader(
           f7, DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE,
           DWRITE_PAINT_FEATURE_LEVEL_COLR_V1, &reader)))) {
      return false;
    }

    DWRITE_PAINT_ELEMENT e;
    D2D_RECT_F clip = {};
    DWRITE_PAINT_ATTRIBUTES attrs;

    if (FAILED(IDWritePaintReader_SetCurrentGlyph(reader, pc->glyph, &e,
                                                  sizeof e, &clip, &attrs))) {
      pc->kind = PIECE_COVERAGE;
      continue;
    }

    if (clip.right <= clip.left || clip.bottom <= clip.top) {
      const Face *fc = &t->faces.v[f->face];
      clip = (D2D_RECT_F){-.25f, -fc->ascent - .25f, 1.25f,
                          fc->descent + .25f};
    }

    pc->box = (RECT){(LONG)floorf(pc->x + clip.left * f->px),
                     (LONG)floorf(pc->y + clip.top * f->px),
                     (LONG)ceilf(pc->x + clip.right * f->px),
                     (LONG)ceilf(pc->y + clip.bottom * f->px)};
  }

  RECT box = {INT32_MAX, INT32_MAX, INT32_MIN, INT32_MIN};

  vec_each(&pieces, pc) {
    if (pc->box.right > pc->box.left && pc->box.bottom > pc->box.top) {
      box.left = MIN(box.left, pc->box.left);
      box.top = MIN(box.top, pc->box.top);
      box.right = MAX(box.right, pc->box.right);
      box.bottom = MAX(box.bottom, pc->box.bottom);
    }
  }

  *w = *h = 0;

  if (box.right <= box.left) {
    return true;
  }

  Canvas c = {box.left - 1, box.top - 1, (u32)(box.right - box.left) + 2,
              (u32)(box.bottom - box.top) + 2};

  if (c.w > CIVUE_ATLAS_SIZE || c.h > CIVUE_ATLAS_SIZE) {
    return false;
  }

  c.px = xcalloc((usize)c.w * c.h * 4, sizeof(f32));
  defer free(c.px);

  Painter p = {t, reader, face, f->px, &c};
  defer {
    vec_each(&p.spare, s) {
      free(*s);
    }

    vec_free(&p.spare);
  }

  if (reader) {
    IDWritePaintReader_SetTextColor(reader, (const DWRITE_COLOR_F *)white);
  }

  vec_each(&pieces, pc) {
    switch (pc->kind) {
    case PIECE_COVERAGE:
      if (pc->cov) {
        fill_coverage(&c, &pc->box, pc->cov, pc->color);
      }
      break;

    case PIECE_IMAGE:
      draw_picture(&c, pc);
      break;

    case PIECE_PAINT: {
      DWRITE_PAINT_ELEMENT e;
      D2D_RECT_F clip;
      DWRITE_PAINT_ATTRIBUTES attrs;

      if (SUCCEEDED(IDWritePaintReader_SetCurrentGlyph(
            reader, pc->glyph, &e, sizeof e, &clip, &attrs))) {
        DWRITE_MATRIX m = {f->px, 0, 0, f->px, pc->x - c.x0, pc->y - c.y0};
        paint(&p, &e, m, c.px);
      }
      break;
    }
    }
  }

  *w = c.w, *h = c.h;
  *left = c.x0, *top = c.y0;
  usize n = (usize)c.w * c.h * 4;
  out->len = 0;
  vec_reserve(out, n);
  out->len = n;

  for (usize i = 0; i < n; i++) {
    out->v[i] = (u8)lroundf(CLAMP(c.px[i], 0, 1) * 255);
  }

  return true;
}

static const GlyphEntry *glyph(civue_text_system *t, civue_atlas *mask,
                               civue_atlas *color, const civue_glyph *g,
                               u32 subpixel) {
  const Font *f = &t->fonts.v[g->font];
  bool color_face = t->faces.v[f->face].color;

  if (color_face) {
    subpixel = 0;
  }

  u64 key = hash_u64((u64)g->font << 24 ^ (u64)g->id << 4 ^ subpixel) | 1;
  u64 at;

  if (map_get(&t->glyph_index, key, &at)) {
    GlyphEntry *e = &t->glyphs.v[at - 1];
    civue_atlas *a = e->color ? color : mask;

    if (e->generation == a->generation) [[clang::likely]] {
      return e;
    }
  }

  civue_bytes px = {};
  defer vec_free(&px);
  i32 left = 0, top = 0;
  u32 w = 0, h = 0;
  bool pictured =
    color_face && rasterize_color(t, f, g->id, &px, &left, &top, &w, &h);

  if (!pictured &&
      !rasterize_mask(t, f, g->id, subpixel, &px, &left, &top, &w, &h)) {
    return nullptr;
  }

  civue_atlas *a = pictured ? color : mask;
  f32 uv[4] = {};

  if (w && h && !civue_atlas_insert(a, w, h, px.v, uv)) {
    // a full atlas starts over, glyphs come back as frames need them.
    civue_atlas_clear(a);

    if (!civue_atlas_insert(a, w, h, px.v, uv)) {
      return nullptr;
    }
  }

  GlyphEntry n = {
    key,          {uv[0], uv[1], uv[2], uv[3]}, pictured, left, top, w, h,
    a->generation};

  if (map_get(&t->glyph_index, key, &at)) {
    t->glyphs.v[at - 1] = n;
    return &t->glyphs.v[at - 1];
  }

  vec_push(&t->glyphs, n);
  map_put(&t->glyph_index, key, t->glyphs.len);
  return &vec_last(&t->glyphs);
}

/* ========================================================================== */
/* drawing.                                                                   */
/* ========================================================================== */

// a glyph that draws nothing, a space for one.
constexpr u32 EMPTY = UINT32_MAX;
static_assert(SUBPIXELS == 4);

typedef struct {
  // the glyph's entry + 1, EMPTY, or 0 when it could not be made: it is
  // asked for again the next time.
  u32 entry;
  // whole pixels from the block's origin, floored, over the two bits of
  // the part of a pixel it is drawn at.
  i32 placed;
} Found;

// where a block's glyphs are in the atlas, found once for the fraction of a
// pixel it is drawn at.
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
    d->v[i] = (Found){find(t, mask, color, g, sub), (i32)px * 4 + (i32)sub};
  }

  return d;
}

// light text on dark grounds looks thinner, browsers boost it.
static f32 gamma_of(civue_color c) {
  return 1 - 0.45f * (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b);
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
    if (!e || e->generation != (e->color ? color : mask)->generation)
      [[clang::unlikely]] {
      f->entry = find(t, mask, color, g, (u32)f->placed & 3);

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

    f64 x = whole + (f->placed >> 2) + e->left, y = round(oy + g->y) + e->top;
    bool same = c.r == run.r && c.g == run.g && c.b == run.b && c.a == run.a;

    if (e->color || !same) {
      civue_dl_masks(d, t->quads.v, t->quads.len, run, gamma_of(run));
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

  civue_dl_masks(d, t->quads.v, t->quads.len, run, gamma_of(run));
}
