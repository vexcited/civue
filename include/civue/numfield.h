#ifndef CIVUE_NUMFIELD_H
#define CIVUE_NUMFIELD_H

#include <civue/widgets.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* number fields: typed values and arithmetic, arrow steps, scrubbing the label, and stepper buttons.         */
/* ========================================================================================================== */

typedef enum : uint8_t {
  CIVUE_NUM_NONE,
  // a value typed or stepped with the arrows.
  CIVUE_NUM_SET,
  // the label dragged, a gesture of several values.
  CIVUE_NUM_SCRUB,
  // the drag on the label ended.
  CIVUE_NUM_SCRUB_END,
  // = typed in a field marked with civue_num_field_variable, which asks
  // for a variable rather than typing it.
  CIVUE_NUM_VARIABLE,
} civue_num_change_kind;

typedef struct {
  civue_num_change_kind kind;
  double value;
} civue_num_change;

// formats a value for a field into `out`.
typedef void (*civue_num_format)(double v, char *out, size_t n);

// a design panel field holding a number, like figma's: click to type a value
// or arithmetic, arrows step it by 1 or 10 with shift, dragging the label
// scrubs it. `mixed` when the selection mixes values, `show` formats it.
civue_num_change civue_num_field(civue_ui *ui, civue_id id, civue_rect r, civue_label label, double value, bool mixed,
                                 civue_num_format show);
// the next number field shows its text faint, or blank rather than mixed.
void civue_num_field_faint(civue_ui *ui);
void civue_num_field_blank(civue_ui *ui);
// the next number field takes = for a variable, answering it with
// NUM_VARIABLE. the field takes the character out of what is typed.
void civue_num_field_variable(civue_ui *ui);
// starts editing the number field `id` with `text`, as if it was clicked.
void civue_focus_num(civue_ui *ui, civue_id id, const char *text);
// the text the number field `id` was just left with, for fields whose units
// are typed too, like "150%" or "auto". malloc'd, nullptr when none.
char *civue_num_field_typed(civue_ui *ui, civue_id id);
// the number a field's text says: a number, arithmetic, or arithmetic on the
// current value when it starts with an operator, like "*2".
bool civue_eval_num(const char *text, const double *current, double *out);
// figma's stepper buttons over a number field's right 24 points: a step up
// or down, a big one with shift, the value rounded to the step and kept in
// min to max. held, they step again after half a second, then every tenth.
// true with the new value when it steps.
bool civue_steppers(civue_ui *ui, civue_id id, civue_rect field, double value, double step, double big, double min,
                    double max, double *out);
// the usual display of a number: whole, or at most two decimals.
void civue_num_show(double v, char *out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
