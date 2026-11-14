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
} civue_text_block;

typedef struct {
  // bytes.
  uint32_t start, end;
  civue_color color;
} civue_text_span;

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

#ifdef __cplusplus
}
#endif

#endif
