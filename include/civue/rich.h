#ifndef CIVUE_RICH_H
#define CIVUE_RICH_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ==========================================================================================================
 */
/* rich text as figma keeps annotation labels and component descriptions:
 * paragraphs, headers, code blocks    */
/* and list items of runs bold, italic, struck, code or linked, laid out
 * wrapped.                             */
/* ==========================================================================================================
 */

// the format of a byte: its flags, and in the high bits which of the doc's
// links it belongs to, none for 0.
enum {
  CIVUE_RICH_BOLD = 1,
  CIVUE_RICH_ITALIC = 2,
  CIVUE_RICH_CODE = 4,
  CIVUE_RICH_STRIKE = 8,
  CIVUE_RICH_LINK_SHIFT = 4,
  CIVUE_RICH_LINK_MASK = 0xf0,
  CIVUE_RICH_FLAGS = 0x0f,
  CIVUE_RICH_MAX_LINKS = 15
};

typedef enum : uint8_t {
  CIVUE_RICH_PARA,
  CIVUE_RICH_BULLET,
  CIVUE_RICH_ORDERED,
  CIVUE_RICH_H1,
  CIVUE_RICH_CODE_BLOCK
} civue_rich_block;

// the text, a line per block, each byte's format, each block's kind, and
// the urls the links of the formats name, the first being link 1.
typedef struct {
  civue_bytes text;
  civue_bytes fmt;
  CIVUE_VEC(civue_rich_block) blocks;
  civue_strs links;
} civue_rich_doc;

// where one word of one format sits once laid out, relative to the origin.
// a word takes its trailing spaces with it, they add to its advance only.
typedef struct {
  uint32_t start, end, ink; // bytes, `ink` where the trailing spaces start.
  uint32_t block;
  float x, y; // the ink's left and its line's top.
  float ink_w, space;
  // the room it takes, inline code's padding and gray included.
  float x0, x1;
  uint8_t fmt;
  bool code_open, code_close;
  const civue_text_block *text;
} civue_rich_piece;

typedef struct {
  float y, height, indent;
  uint32_t start, end; // bytes, the newline left out.
  // the height of one of its lines, a header's higher than a paragraph's.
  float line;
  civue_rich_block kind;
} civue_rich_box;

// the colors a doc is drawn in: its text, its links, the ground under its code,
// and its selection's while edited.
typedef struct {
  civue_color text, link, code, selection;
} civue_rich_colors;

// a doc laid out, for drawing and for finding where bytes and points are.
typedef struct {
  CIVUE_VEC(civue_rich_piece) pieces;
  CIVUE_VEC(civue_rich_box) boxes;
  float widest, height;
  // the paragraph's line height, and the width laid out at.
  float line_height, width;
} civue_rich_lay;

civue_rich_doc civue_rich_from_html(const char *html);
void civue_rich_free(civue_rich_doc *d);
// lays it out wrapped at `width`, the blocks' list indents inside it.
void civue_rich_layout(civue_ui *ui, const civue_rich_doc *d,
                       civue_text_style style, float width,
                       civue_rich_lay *out);
void civue_rich_lay_free(civue_rich_lay *l);
// draws a doc laid out at `origin`: list markers, code grounds, the text.
void civue_rich_paint(civue_ui *ui, const civue_rich_doc *d,
                      const civue_rich_lay *l, civue_vec2 origin,
                      civue_text_style style, civue_rich_colors colors);
// its size wrapped at `width`, the widest line and the height.
civue_vec2 civue_rich_measure(civue_ui *ui, const civue_rich_doc *d,
                              civue_text_style style, float width);
// draws it from `origin`, returning its height.
float civue_rich_draw(civue_ui *ui, const civue_rich_doc *d, civue_vec2 origin,
                      civue_text_style style, float width,
                      civue_rich_colors colors);

/* ==========================================================================================================
 */
/* editing: the doc's own edits, its html, and an editor with bold and italic on
 * cmd+b and cmd+i, lists and   */
/* markdown shortcuts as they are typed. */
/* ==========================================================================================================
 */

// a doc of one empty paragraph.
civue_rich_doc civue_rich_new(void);
civue_rich_doc civue_rich_clone(const civue_rich_doc *d);
// no text and no list.
bool civue_rich_is_empty(const civue_rich_doc *d);
// the same text, formats and blocks.
bool civue_rich_equal(const civue_rich_doc *a, const civue_rich_doc *b);
// `n` bytes of `s` put in at `at` in format `f`, a newline starting a
// paragraph.
void civue_rich_insert(civue_rich_doc *d, uint32_t at, const char *s, size_t n,
                       uint8_t f);
// the bytes from `from` to `to` taken out, blocks it joins keeping the
// first's kind.
void civue_rich_delete(civue_rich_doc *d, uint32_t from, uint32_t to);
// `f` toggled over the bytes: on everywhere unless every byte has it.
void civue_rich_toggle(civue_rich_doc *d, uint32_t from, uint32_t to,
                       uint8_t f);
// figma's saved label, its html, malloc'd.
char *civue_rich_to_html(const civue_rich_doc *d);
// the html of figma's lexical editor as component descriptions keep it:
// spans around the text, b, i and s around the formats, ul and ol for lists,
// pre for code blocks and a for links.
char *civue_rich_to_lexical_html(const civue_rich_doc *d);

typedef struct {
  bool changed;
  float height;
  // the link shortcut was pressed.
  bool link_request;
  // where the caret is from the origin, and the height of its line, while
  // the doc is edited.
  bool has_caret;
  civue_vec2 caret;
  float caret_line;
} civue_rich_response;

// an editable doc at `origin`, `width` wide, its keys and the pointer
// handled while it has the focus. enter is the caller's, shift enter starts
// a new block like figma's.
civue_rich_response civue_rich_edit(civue_ui *ui, civue_id id,
                                    civue_vec2 origin, float width,
                                    civue_rich_doc *d, civue_text_style style,
                                    civue_rich_colors colors);
// the same where enter starts a new block, for a description.
civue_rich_response civue_rich_edit_multiline(civue_ui *ui, civue_id id,
                                              civue_vec2 origin, float width,
                                              civue_rich_doc *d,
                                              civue_text_style style,
                                              civue_rich_colors colors);
// puts the caret of the editing doc `id` after its text, false while it
// isn't editing yet.
bool civue_rich_caret_to_end(civue_ui *ui, civue_id id,
                             const civue_rich_doc *d);

// what a toolbar over the doc being edited does.

typedef enum : uint8_t {
  CIVUE_RICH_ACT_BOLD,
  CIVUE_RICH_ACT_ITALIC,
  CIVUE_RICH_ACT_STRIKE,
  CIVUE_RICH_ACT_CODE,
  CIVUE_RICH_ACT_H1,
  CIVUE_RICH_ACT_BULLETS,
  CIVUE_RICH_ACT_NUMBERS,
  CIVUE_RICH_ACT_CODE_BLOCK
} civue_rich_action;

// the selection of the doc `id` being edited, false while it is not.
bool civue_rich_selection(civue_ui *ui, civue_id id, uint32_t *lo,
                          uint32_t *hi);
void civue_rich_select(civue_ui *ui, civue_id id, uint32_t lo, uint32_t hi);
// whether the action is on where the caret or the selection is.
bool civue_rich_active(civue_ui *ui, civue_id id, const civue_rich_doc *d,
                       civue_rich_action a);
// toggles it there. a format with no selection applies to what is typed
// next. true when the doc changed.
bool civue_rich_act(civue_ui *ui, civue_id id, civue_rich_doc *d,
                    civue_rich_action a);
// links the bytes from `lo` to `hi` to `url`, or takes their link off when
// it is none.
void civue_rich_set_link(civue_rich_doc *d, uint32_t lo, uint32_t hi,
                         const char *url);
// the url of the link the byte at `at`, or just before it, is in, and the
// bytes it covers, none when it is in no link.
const char *civue_rich_link_at(const civue_rich_doc *d, uint32_t at,
                               uint32_t *from, uint32_t *to);

#ifdef __cplusplus
}
#endif

#endif
