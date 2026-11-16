#ifndef CIVUE_WIDGETS_H
#define CIVUE_WIDGETS_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* widgets without a look: buttons, toggles, sliders, groups of choices,      */
/* scroll areas, splitters, menus and tooltips. each is a call per frame that */
/* takes the pointer and the keys and answers with its state, and the app     */
/* paints it from that. the keys go to a widget while it has the focus,       */
/* ui->focus, which a press doesn't give it: the app does, as its keyboard    */
/* navigation moves.                                                          */
/* ========================================================================== */

typedef struct {
  civue_response response;
  // held down with the pointer over it.
  bool held;
  // the focus is on it, or the keyboard's ring.
  bool focused, ring;
  // clicked, pressed through civue_press_ring, or enter or space while focused.
  bool activated;
} civue_button_state;

civue_button_state civue_button(civue_ui *ui, civue_id id, civue_rect r);

// a checkbox or a switch: a button that flips `on` when activated.
typedef struct {
  civue_button_state button;
  // what it is after this frame.
  bool on, changed;
} civue_toggle_state;

civue_toggle_state civue_toggle(civue_ui *ui, civue_id id, civue_rect r,
                                bool on);

typedef struct {
  civue_response response;
  bool focused;
  // the value dragged or stepped to, `changed` when it differs from the one
  // given. `ended` when the drag ends or a key stepped it, the gesture done.
  double value;
  bool changed, ended;
  // where the value given and the anchor sit along the track.
  double x, anchor_x;
} civue_slider_state;

// a slider dragged anywhere in `r`, its values from min at the left of `track`
// to max at its right, snapped to `step`. while focused the arrows step it, ten
// steps with shift, home and end go to min and max.
civue_slider_state civue_slider(civue_ui *ui, civue_id id, civue_rect r,
                                civue_rect track, double value, double min,
                                double max, double anchor, double step);

typedef struct {
  // the item chosen this frame by a click or the keys, -1 for none.
  int32_t chosen;
  bool focused;
  // the item the keyboard is on while focused, -1 otherwise.
  int32_t current;
} civue_group_state;

// a row of `count` choices like a segmented control or radio buttons, item i
// over `cells[i]` taking the pointer as civue_idn(id, i), its response in
// `out[i]`. selection indices are -1 when absent. while the group has the focus
// the arrows choose the item after or before the selected one, home and end the
// first and the last.
civue_group_state civue_radio_group(civue_ui *ui, civue_id id,
                                    const civue_rect *cells,
                                    civue_response *out, uint32_t count,
                                    int32_t selected);

/* ========================================================================== */
/* scroll areas. the wheel and the trackpad move them, eased toward where the */
/* wheel sent them. like gpui's, they have no bars unless the app gives a     */
/* look for them: then bars along the right and the bottom edges take the     */
/* pointer, a thumb is dragged and a press beside it pages, and the look      */
/* paints them.                                                               */
/* ========================================================================== */

typedef struct {
  // the content overflows that way.
  bool shown;
  // `size` across along the edge, the thumb `inset` inside it and at least
  // `min_thumb` long.
  civue_rect track, thumb;
  // the pointer is over the track, or drags its thumb.
  bool lit;
} civue_scrollbar;

struct civue_scroll_state {
  civue_id id;
  civue_rect r;
  // how far the bars show, 1 while the pointer is over the area, it scrolled in
  // the last second or a thumb is dragged, eased to 0 after.
  float show;
  civue_scrollbar vertical, horizontal;
};

// how a scroll area's bars sit and look.
typedef struct civue_scrollbar_look {
  // across the edge, the thumb inside its track and its least length.
  float size, inset, min_thumb;
  // the bars stay this long after a scroll, fading over `fade`.
  float linger, fade;
  void *ctx;
  // paints the bars as civue_scroll_end ends the area.
  void (*paint)(civue_ui *ui, void *ctx, const civue_scroll_state *s);
} civue_scrollbar_look;

// each begin returns the shifted content rect and requires civue_scroll_end.
// the bars follow `look`, or ui->scrollbars for the ones without, and there are
// none when both are null.
civue_rect civue_scroll_area(civue_ui *ui, civue_id id, civue_rect r,
                             float content_height);
civue_rect civue_scroll_area_xy(civue_ui *ui, civue_id id, civue_rect r,
                                float content_width, float content_height);
civue_rect civue_scroll_area_look(civue_ui *ui, civue_id id, civue_rect r,
                                  float content_width, float content_height,
                                  const civue_scrollbar_look *look);
civue_scroll_state civue_scroll_end(civue_ui *ui);
float civue_scroll_x(civue_ui *ui, civue_id id);
float civue_scroll_y(civue_ui *ui, civue_id id);
void civue_set_scroll_y(civue_ui *ui, civue_id id, float y);
void civue_set_scroll_x(civue_ui *ui, civue_id id, float x);
void civue_scroll_to_center(civue_ui *ui, civue_id id, float y0, float y1,
                            float view, bool always);
void civue_scroll_to(civue_ui *ui, civue_id id, float y0, float y1, float view);
// a splitter 6 points wide at `x`, the distance it was dragged this frame.
float civue_vertical_splitter(civue_ui *ui, civue_id id, float x, float y0,
                              float y1);

typedef struct {
  civue_ui *ui;
  bool open;
  civue_rect inner;
} civue_scroll_scope;

static inline civue_scroll_scope
civue__scroll_begin(civue_ui *ui, civue_id id, civue_rect r, float height) {
  return (civue_scroll_scope){ui, true, civue_scroll_area(ui, id, r, height)};
}

static inline void civue__scroll_end(civue_scroll_scope *s) {
  civue_scroll_end(s->ui);
}

// a scroll area over `r` of `height` points of content, `content` the shifted
// rect its body lays out in, ended however the body is left.
#define CIVUE_SCROLLING(ui, id, r, height, content)                          \
  for (civue_scroll_scope CIVUE__SCOPE [[gnu::cleanup(civue__scroll_end)]] = \
         civue__scroll_begin(ui, id, r, height);                             \
       CIVUE__SCOPE.open; CIVUE__SCOPE.open = false)                         \
    for (civue_rect content = CIVUE__SCOPE.inner; CIVUE__SCOPE.open;         \
         CIVUE__SCOPE.open = false)

/* ========================================================================== */
/* menus: placed by their anchor, submenus opened by hovering their rows, the */
/* arrows and enter, a click outside or escape closing them. the app measures */
/* and paints them through a look.                                            */
/* ========================================================================== */

typedef struct civue_menu_item {
  // a line between groups, or a title over the rows after it: neither is chosen
  // nor lit.
  bool separator, header;
  bool disabled;
  // a row the app handles itself in its row callback, chosen when that returns
  // true.
  bool custom;
  float height;
  const struct civue_menu_item *submenu;
  uint32_t nsubmenu;
  // the app's, what it paints the row from.
  const void *data;
} civue_menu_item;

// a row as it is painted.
typedef struct {
  uint32_t index;
  // the menu's card, the row across it, and the part of a regular row that
  // takes the pointer, 8 points inside.
  civue_rect menu, row, hit;
  civue_response response;
  // lit by the pointer, its submenu showing or the keyboard on it, unless it is
  // disabled.
  bool lit, open, keyboard;
} civue_menu_row;

typedef struct {
  void *ctx;
  // the size of a submenu of `count` items.
  civue_vec2 (*measure)(civue_ui *ui, void *ctx, const civue_menu_item *items,
                        uint32_t count);
  // paints a card under a menu's rows.
  void (*surface)(civue_ui *ui, void *ctx, civue_rect r);
  // paints a row of `items`. true when a custom row is chosen.
  bool (*row)(civue_ui *ui, void *ctx, const civue_menu_item *items,
              uint32_t count, const civue_menu_row *row);
  // marks where a menu taller than the window is cut, 16 points tall, at its
  // top when `up`.
  void (*cut)(civue_ui *ui, void *ctx, civue_rect r, bool up);
} civue_menu_look;

typedef struct {
  // initialize with civue_menu_options_default before overriding fields.
  float gap;
  // placed under the pointer rather than its anchor, and above the anchor.
  bool at_pointer, above;
  // the lowest the menu goes, the window's bottom unless set.
  bool has_floor;
  float floor;
  // the row the keyboard starts on, the first one when `focus_first`, none for
  // -1.
  bool focus_first;
  int32_t focus_row;
  const civue_menu_look *look;
} civue_menu_options;

civue_menu_options civue_menu_options_default();
static constexpr uint32_t CIVUE_MENU_MAX_DEPTH = 32;
// a menu of `size` under `anchor`, above it where there is no room, its rows 8
// points in from its top and bottom. path must hold CIVUE_MENU_MAX_DEPTH
// entries, depth is zero when none is chosen.
bool civue_menu(civue_ui *ui, civue_id id, civue_rect anchor, civue_vec2 size,
                bool *open, const civue_menu_item *items, uint32_t count,
                const civue_menu_options *o, uint32_t *path, uint32_t *depth);
// a menu at `x` over a field, its row `selected` lined up with the field,
// scrolled when the window is too short for it. the item chosen, -1 for none.
int32_t civue_select_menu(civue_ui *ui, civue_id id, civue_rect field, float x,
                          civue_vec2 size, int32_t selected, bool *open,
                          const civue_menu_item *items, uint32_t count,
                          const civue_menu_options *o);
// the row of a menu whose submenu shows, as if the pointer were on it, until
// the pointer moves to another. call before the menu is drawn.
void civue_menu_open_row(civue_ui *ui, civue_id id, int32_t row);

/* ========================================================================== */
/* tooltips.                                                                  */
/* ========================================================================== */

typedef enum : uint8_t {
  CIVUE_TOOLTIP_BELOW,
  CIVUE_TOOLTIP_ABOVE,
  CIVUE_TOOLTIP_RIGHT
} civue_tooltip_side;

typedef struct {
  civue_rect rect;
  civue_tooltip_side side;
} civue_tooltip_place;

// whether the tooltip `id` of what answered `resp` shows: after half a second
// of hover without dragging.
bool civue_tooltip(civue_ui *ui, civue_id id, const civue_response *resp);
// where a tooltip of `size` goes by `anchor`, `gap` from it and from the
// window's edges: below it, above it when asked or when there is no room below,
// or to its right. its corner is rounded to whole points.
civue_tooltip_place civue_tooltip_at(civue_ui *ui, civue_rect anchor,
                                     civue_vec2 size, civue_tooltip_side side,
                                     float gap);

#ifdef __cplusplus
}
#endif

#endif
