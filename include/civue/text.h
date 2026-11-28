#ifndef CIVUE_TEXT_H
#define CIVUE_TEXT_H

#include <civue/draw.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* text. laid out once per text, style and scale with core text on macos and  */
/* harfbuzz on linux, its glyphs rasterized into the renderer's atlases as    */
/* they are first drawn. the ui and the code faces are the system's ui font   */
/* and its monospace one until the app sets its own with civue_text_set_face. */
/* ========================================================================== */

// faces an app registers while running, chosen by civue_text_style.display from
// 1 up to this.
constexpr uint8_t CIVUE_TEXT_FACES = 2;

// the faces text is set in when its style names none of the app's: the ui's,
// and the code one for `mono`.
typedef enum : uint8_t {
  CIVUE_FACE_UI,
  CIVUE_FACE_CODE
} civue_face;

// how a run of text looks, sizes in logical points.
typedef struct {
  float size, weight, line_height, letter_spacing;
  // in the code face rather than the ui one.
  bool mono;
  // its italic, the system's when the face is the system's.
  bool italic;
  // tabular digits, css font-variant tabular-nums.
  bool tnum;
  // emoji only, its face first.
  bool emoji;
  // a face the app registered with civue_text_register_display, 0 for the ui
  // face.
  uint8_t display;
} civue_text_style;

static inline civue_text_style
civue_style(float size, float weight, float line_height, float letter_spacing) {
  return (civue_text_style){.size = size,
                            .weight = weight,
                            .line_height = line_height,
                            .letter_spacing = letter_spacing};
}

static inline civue_text_style civue_style_weight(civue_text_style s,
                                                  float weight) {
  s.weight = weight;
  return s;
}

static inline civue_text_style civue_style_mono(civue_text_style s) {
  s.mono = true;
  return s;
}

static inline civue_text_style civue_style_italic(civue_text_style s) {
  s.italic = true;
  return s;
}

static inline civue_text_style civue_style_tnum(civue_text_style s) {
  s.tnum = true;
  return s;
}

typedef struct {
  // where its character starts in the text.
  uint32_t byte;
  // its index in the text system's fonts.
  uint32_t font;
  uint16_t id;
  // physical pixels from the block's origin.
  float x, y;
} civue_glyph;

typedef struct {
  uint32_t byte;
  float x;
  uint32_t line;
} civue_caret;

typedef struct {
  // bytes, a wrapped line's trailing space included.
  uint32_t start, end;
  float top, baseline;
} civue_text_line;

// laid out text, measured in logical points.
typedef struct {
  float width, height;
  // from the top to the first baseline.
  float baseline;
  uint32_t nglyphs;
  civue_glyph *glyphs;
  // x of every character boundary of each line, line ends included, in line
  // order.
  uint32_t ncarets;
  civue_caret *carets;
  uint32_t nlines;
  civue_text_line *lines;
  float scale;
  // the text system's own, what it found of the glyphs to draw them again.
  void *cache;
} civue_text_block;

typedef struct {
  // bytes.
  uint32_t start, end;
  civue_color color;
} civue_text_span;

// the x of the caret before `byte` in a block, its line in `line`. at a wrap
// the caret goes to the start of the next line.
float civue_text_caret_x(const civue_text_block *b, uint32_t byte,
                         uint32_t *line);
// the byte whose caret is nearest to `p`, from the block's top left.
uint32_t civue_text_hit(const civue_text_block *b, civue_vec2 p);

// what a press selects and its drag grows by, as chromium does: letters,
// words after a double press, paragraphs after a triple one.
typedef enum : uint8_t {
  CIVUE_UNIT_CHAR,
  CIVUE_UNIT_WORD,
  CIVUE_UNIT_PARAGRAPH
} civue_text_unit;

civue_text_unit civue_text_unit_of(uint32_t clicks);
// the bytes of the unit of `s` the caret before byte `at` is in. a word is
// a segment of unicode's UAX #29 as ICU cuts them: letters and digits run
// on, spaces too, any other sign stands alone, and chinese, japanese, thai,
// lao, burmese and khmer are cut by the system's dictionaries where it has
// them. the caret between two words is in the one after it. it is in the
// one before it at the end of the text, and when `line_end`, at the end of
// a wrapped line, whose last spaces are no word. at the end of a paragraph
// it is in the break that ends it. a paragraph is a line of the text, with
// that break.
void civue_text_unit_at(const char *s, size_t n, uint32_t at,
                        civue_text_unit unit, bool line_end, uint32_t *from,
                        uint32_t *to);

// the press a selection of one text grows from: its unit, where it went
// down and the bounds of the unit there.
typedef struct {
  civue_text_unit unit;
  uint32_t base, from, to;
} civue_text_grip;

// a press at byte `at`, the `clicks`th in a row, selects the unit there
// from `anchor` to `focus`. with `extend`, as shift held does, it moves an
// end of what they hold instead, by the unit it was made with: the nearer
// one on a mac, the focus elsewhere. `line_end` as civue_text_unit_at's.
void civue_text_press(civue_text_grip *g, const char *s, size_t n,
                      uint32_t at, bool line_end, uint32_t clicks, bool extend,
                      uint32_t *anchor, uint32_t *focus);
// the pointer went to byte `at` with the press held: the selection goes
// from the unit pressed to the one there, both whole.
void civue_text_drag(const civue_text_grip *g, const char *s, size_t n,
                     uint32_t at, bool line_end, uint32_t *anchor,
                     uint32_t *focus);

typedef struct civue_text_system civue_text_system;

civue_text_system *civue_text_new();
void civue_text_free(civue_text_system *t);
// lays text out, wrapped at max_width points when positive, at `scale` physical
// pixels per point. the block lives at least until the end of the next frame.
const civue_text_block *civue_text_layout(civue_text_system *t, const char *s,
                                          size_t n, civue_text_style style,
                                          float max_width, float scale);
// draws a block with its top left at `origin` in points, the bytes of each span
// in its color.
void civue_text_draw(civue_text_system *t, civue_draw_list *d,
                     civue_atlas *mask, civue_atlas *color,
                     const civue_text_block *b, civue_vec2 origin,
                     civue_color base, const civue_text_span *spans,
                     uint32_t nspans);
// forgets layouts no frame used for a while.
void civue_text_end_frame(civue_text_system *t);
// registers the font file in `data` as face `display`, from 1 to
// CIVUE_TEXT_FACES. what was laid out is laid out again.
void civue_text_register_display(civue_text_system *t, uint8_t display,
                                 const uint8_t *data, size_t n);
// replaces the ui or the code face with the font file in `data`, copied. what
// was laid out is laid out again.
void civue_text_set_face(civue_text_system *t, civue_face face,
                         const uint8_t *data, size_t n);
// sets the ui or the code face in a family the system has, like "Hack Nerd
// Font", each weight its own. what was laid out is laid out again.
void civue_text_set_family(civue_text_system *t, civue_face face,
                           const char *family);

#ifdef __cplusplus
}
#endif

#endif
