#ifndef CIVUE_FIELD_H
#define CIVUE_FIELD_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* text fields: one line or several, edited with the keyboard, the pointer, the clipboard and input methods.  */
/* ========================================================================================================== */

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
civue_field_response civue_text_field(civue_ui *ui, civue_id id, civue_rect r, char **value, const char *placeholder,
                                      civue_text_style style, civue_field_look look);
// several lines wrapped to the field's width from its top: enter starts a
// line and the arrows move between them.
civue_field_response civue_text_area(civue_ui *ui, civue_id id, civue_rect r, char **value, const char *placeholder,
                                     civue_text_style style, civue_field_look look);
// puts `text` at the caret of the focused field `id`, as a button beside it
// types. its whole text after, malloc'd, nullptr when it isn't editing.
char *civue_field_insert(civue_ui *ui, civue_id id, const char *text);
// puts the caret of the editing field `id` after its text, false when it
// isn't editing yet.
bool civue_field_caret_to_end(civue_ui *ui, civue_id id);

#ifdef __cplusplus
}
#endif

#endif
