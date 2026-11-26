#ifndef CIVUE_FIELD_H
#define CIVUE_FIELD_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* text fields: one line or several, edited with the keyboard, the pointer,   */
/* the clipboard and input methods.                                           */
/* ========================================================================== */

// what a text field did this frame.
typedef struct {
  civue_response response;
  bool changed;
  // enter was pressed.
  bool submitted;
  bool focused;
} civue_field_response;

// a field's box and the text in it.
typedef struct {
  civue_color bg, border;
  float border_width, radius;
  civue_color text;
} civue_field_box;

// how a text field looks: while it edits, and while it doesn't, hovered or not.
typedef struct {
  // room between the box and the text on each side.
  float pad;
  civue_field_box editing, idle, hovered;
  civue_color placeholder, selection;
} civue_field_look;

// a single line text field editing `*value`, a malloc'd string replaced
// when it changes, nullptr for empty.
civue_field_response civue_text_field(civue_ui *ui, civue_id id, civue_rect r,
                                      char **value, const char *placeholder,
                                      civue_text_style style,
                                      civue_field_look look);
// a single line field for a password: it shows a bullet for each character,
// is never copied out, and the input method is told it takes a secret.
civue_field_response civue_secret_field(civue_ui *ui, civue_id id,
                                        civue_rect r, char **value,
                                        const char *placeholder,
                                        civue_text_style style,
                                        civue_field_look look);
// several lines wrapped to the field's width from its top: enter starts a
// line and the arrows move between them. what doesn't fit scrolls, keeping the
// caret in view.
civue_field_response civue_text_area(civue_ui *ui, civue_id id, civue_rect r,
                                     char **value, const char *placeholder,
                                     civue_text_style style,
                                     civue_field_look look);
// what a text area shows in place of some of its text, like the picture of
// an emoji for its name between colons. each is one character to the caret
// and to the keys, and its bytes stay in the text as they are.
typedef struct {
  void *ctx;
  // how many bytes of `text` from `at` are shown as one, 0 for none, and
  // how wide it is in points.
  uint32_t (*find)(void *ctx, const char *text, uint32_t len, uint32_t at,
                   float *width);
  // draws the one these bytes are over `r`, a line high and about as wide
  // as asked: the room is a whole number of spaces.
  void (*paint)(civue_ui *ui, void *ctx, const char *text, uint32_t len,
                civue_rect r);
} civue_field_inlays;

// a text area with inlays, none when `inlays` is null.
civue_field_response civue_text_area_inlaid(civue_ui *ui, civue_id id,
                                            civue_rect r, char **value,
                                            const char *placeholder,
                                            civue_text_style style,
                                            civue_field_look look,
                                            const civue_field_inlays *inlays);
// how high the text of a text area `width` points wide is, a line for the
// caret after a last newline included.
float civue_text_area_height(civue_ui *ui, const char *value,
                             civue_text_style style, float width,
                             const civue_field_inlays *inlays);
// puts `text` at the caret of the focused field `id`, as a button beside it
// types. its whole text after, malloc'd, nullptr when it isn't editing.
char *civue_field_insert(civue_ui *ui, civue_id id, const char *text);
// puts the caret of the editing field `id` after its text, false when it
// isn't editing yet.
bool civue_field_caret_to_end(civue_ui *ui, civue_id id);
// the caret of the editing field `id`: its byte offset and where it was drawn
// last frame. false when it isn't editing.
bool civue_field_caret(civue_ui *ui, civue_id id, uint32_t *at,
                       civue_rect *where);
// replaces the bytes `from` to `to` of the editing field `id` with `text`, the
// caret after it. its whole text after, malloc'd, nullptr when it isn't
// editing.
char *civue_field_replace(civue_ui *ui, civue_id id, uint32_t from, uint32_t to,
                          const char *text);

#ifdef __cplusplus
}
#endif

#endif
