#ifndef CIVUE_LAYOUT_H
#define CIVUE_LAYOUT_H

#include <civue/field.h>
#include <civue/widgets.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* nodes: boxes laid out like css blocks, flexbox and grid, painted like      */
/* gpui's divs. a node is painted and answers the pointer from where the last */
/* solve put it, the tree is solved again as the frame ends, and a frame      */
/* whose layout moved is run again before it shows. sizes are border boxes.   */
/* ========================================================================== */

typedef enum : uint8_t {
  CIVUE_UNIT_UNSET,
  CIVUE_UNIT_PX,
  CIVUE_UNIT_REM,
  // a fraction of what holds it.
  CIVUE_UNIT_RELATIVE,
  CIVUE_UNIT_AUTO,
} civue_unit;

typedef struct {
  float value;
  civue_unit unit;
} civue_length;

static inline civue_length civue_px(float v) {
  return (civue_length){v, CIVUE_UNIT_PX};
}

static inline civue_length civue_rems(float v) {
  return (civue_length){v, CIVUE_UNIT_REM};
}

static inline civue_length civue_relative(float v) {
  return (civue_length){v, CIVUE_UNIT_RELATIVE};
}

typedef enum : uint8_t {
  CIVUE_BLOCK,
  CIVUE_FLEX,
  CIVUE_GRID
} civue_display;

typedef enum : uint8_t {
  CIVUE_ROW,
  CIVUE_COL,
  CIVUE_ROW_REVERSE,
  CIVUE_COL_REVERSE,
} civue_direction;

// justify, align items, align self and align content.
typedef enum : uint8_t {
  CIVUE_PLACE_UNSET,
  CIVUE_PLACE_START,
  CIVUE_PLACE_END,
  CIVUE_PLACE_CENTER,
  CIVUE_PLACE_STRETCH,
  CIVUE_PLACE_BASELINE,
  CIVUE_PLACE_BETWEEN,
  CIVUE_PLACE_AROUND,
  CIVUE_PLACE_EVENLY,
} civue_place;

typedef enum : uint8_t {
  CIVUE_RELATIVE,
  CIVUE_ABSOLUTE
} civue_position;

typedef enum : uint8_t {
  CIVUE_OVERFLOW_VISIBLE,
  CIVUE_OVERFLOW_HIDDEN,
  CIVUE_OVERFLOW_SCROLL,
} civue_overflow;

typedef enum : uint8_t {
  CIVUE_SHADOW_NONE,
  CIVUE_SHADOW_SM,
  CIVUE_SHADOW_MD,
  CIVUE_SHADOW_LG,
  CIVUE_SHADOW_XL,
  CIVUE_SHADOW_2XL,
} civue_shadow_size;

// what text inherits unless a node sets it: 0, then on and off.
typedef enum : uint8_t {
  CIVUE_INHERIT,
  CIVUE_ON,
  CIVUE_OFF
} civue_switch;

typedef enum : uint8_t {
  CIVUE_TEXT_INHERIT,
  CIVUE_TEXT_LEFT,
  CIVUE_TEXT_CENTER,
  CIVUE_TEXT_RIGHT,
} civue_text_align;

// a grid item spanning every column.
constexpr uint16_t CIVUE_SPAN_FULL = UINT16_MAX;

// edges go top, right, bottom then left, corners top left then clockwise.
// grid lines count from 1 as in css, 0 for none.
#define CIVUE__NODE_LAYOUT                                     \
  civue_length w, h, min_w, min_h, max_w, max_h, basis;        \
  civue_length padding[4], margin[4], inset[4];                \
  civue_length gap_x, gap_y;                                   \
  float grow, shrink, aspect;                                  \
  float border_w[4];                                           \
  uint16_t cols, rows, col_span, row_span;                     \
  uint16_t col_start, col_end, row_start, row_end;             \
  civue_display display;                                       \
  civue_direction direction;                                   \
  bool wrap, has_shrink;                                       \
  civue_place justify, align_items, align_self, align_content; \
  civue_position position;                                     \
  civue_overflow overflow_x, overflow_y;

#define CIVUE__NODE_PAINT                   \
  civue_color bg, border_color, text_color; \
  bool has_bg, has_border_color, has_text_color;

// what civue keeps of a style until the frame's solve.
typedef struct {
  CIVUE__NODE_LAYOUT
} civue_node_layout;

typedef struct {
  CIVUE__NODE_PAINT
} civue_node_paint;

typedef struct {
  CIVUE__NODE_LAYOUT
  // who it is across frames, made from its parent and place when 0.
  civue_id key;
  civue_length text_size, line_height;
  float font_weight;
  civue_switch italic, mono, tnum, nowrap, truncate;
  civue_text_align text_align;
  CIVUE__NODE_PAINT
  civue_length radius[4];
  civue_shadow_size shadow;
  bool has_cursor;
  civue_cursor_kind cursor;
  // painted over the rest while the pointer is over it, or presses it.
  civue_node_paint hover, active;
  // the bars of a node that scrolls, ui->scrollbars when null.
  const civue_scrollbar_look *scrollbars;
} civue_node_style;

// opens a node in the one open, or a root over the window when none is or
// when an overlay starts. civue_node_close ends it.
void civue_node_open(civue_ui *ui, const civue_node_style *s);
void civue_node_close(civue_ui *ui);
// a node holding text, wrapped to its width unless told not to.
void civue_node_text_n(civue_ui *ui, const char *s, size_t n,
                       const civue_node_style *style);

static inline void civue_node_text(civue_ui *ui, const char *s,
                                   const civue_node_style *style) {
  civue_node_text_n(ui, s, strlen(s), style);
}

// a node showing an icon, as tall as its text unless sized, as wide as its
// view box makes it, in the text color unless it has colors of its own.
void civue_node_icon(civue_ui *ui, civue_icon icon,
                     const civue_node_style *style);

// the open node's border box, its content box inside the padding and its id.
civue_rect civue_node_rect(civue_ui *ui);
civue_rect civue_node_content(civue_ui *ui);
civue_id civue_node_id(civue_ui *ui);
// the text style and color the open node passes on.
civue_text_style civue_node_text_style(civue_ui *ui);
civue_color civue_node_color(civue_ui *ui);
// how big node `id` was when the last frame was solved, zero when it wasn't.
civue_vec2 civue_node_last_size(civue_ui *ui, civue_id id);
// the points in a rem, 16 unless set.
void civue_set_rem_size(civue_ui *ui, float size);

// the widgets over the open node, by its id.
civue_button_state civue_node_button(civue_ui *ui);
civue_toggle_state civue_node_toggle(civue_ui *ui, bool on);
// a slider over the node, its track the node's content box.
civue_slider_state civue_node_slider(civue_ui *ui, double value, double min,
                                     double max, double anchor, double step);
// a password field set in the text style the node passes on.
civue_field_response civue_node_secret_field(civue_ui *ui, char **value,
                                             const char *placeholder,
                                             civue_field_look look);
// a text field set in the text style the node passes on.
civue_field_response civue_node_text_field(civue_ui *ui, char **value,
                                           const char *placeholder,
                                           civue_field_look look);

// a style from designated initializers, a field set twice keeping the last
// as css does.
#define CIVUE_NODE_STYLE(...)                                                \
  _Pragma("GCC diagnostic push")                                             \
    _Pragma("GCC diagnostic ignored \"-Woverride-init\"")(civue_node_style){ \
      __VA_ARGS__} _Pragma("GCC diagnostic pop")

static inline civue_scope civue__node_begin(civue_ui *ui,
                                            const civue_node_style *s) {
  civue_node_open(ui, s);
  return (civue_scope){.ui = ui, .open = true};
}

static inline void civue__node_end(civue_scope *s) {
  civue_node_close(s->ui);
}

// a node around its body, closed however the body is left.
#define CIVUE_DIV(ui, ...)                                          \
  for (civue_scope CIVUE__SCOPE [[gnu::cleanup(civue__node_end)]] = \
         civue__node_begin(ui, &CIVUE_NODE_STYLE(__VA_ARGS__));     \
       CIVUE__SCOPE.open; CIVUE__SCOPE.open = false)

/* ========================================================================== */
/* rows. a list of `count` rows of one height, of which only the ones in view */
/* are built: a hundred thousand cost what a screen of them does. it goes in  */
/* a node that scrolls, and each row is a node `row_height` tall, known by    */
/* its index.                                                                 */
/* ========================================================================== */

typedef struct {
  civue_ui *ui;
  bool open;
  // the rows to build, `last` excluded.
  uint32_t first, last;
  civue_id id;
  float row_height;
} civue_rows_scope;

civue_rows_scope civue_rows_begin(civue_ui *ui, uint32_t count,
                                  float row_height);
void civue_rows_end(civue_rows_scope *s);
// opens the node of row `i`, closed by civue_node_close.
void civue_row_open(civue_rows_scope *s, uint32_t i);

static inline civue_scope civue__row_begin(civue_rows_scope *s, uint32_t i) {
  civue_row_open(s, i);
  return (civue_scope){.ui = s->ui, .open = true};
}

#define CIVUE__ROWS CIVUE__CAT(civue__rows_, __LINE__)

// the rows in view, `i` the index of each.
#define CIVUE_ROWS(ui, count, row_height, i)                            \
  for (civue_rows_scope CIVUE__ROWS [[gnu::cleanup(civue_rows_end)]] =  \
         civue_rows_begin(ui, count, row_height);                       \
       CIVUE__ROWS.open; CIVUE__ROWS.open = false)                      \
    for (uint32_t i = CIVUE__ROWS.first; i < CIVUE__ROWS.last; i++)     \
      for (civue_scope CIVUE__SCOPE [[gnu::cleanup(civue__node_end)]] = \
             civue__row_begin(&CIVUE__ROWS, i);                         \
           CIVUE__SCOPE.open; CIVUE__SCOPE.open = false)

/* ========================================================================== */
/* memos. the nodes a block builds and what they draw are kept, and put back  */
/* without running the block while `inputs` stays what it was: a hash of all  */
/* the block reads, as civue_hash makes one. the block runs again by itself   */
/* when the pointer is over it, when it holds the focus or what is pressed,   */
/* when a key or a button changed, when it asks for another frame, or when    */
/* what is around it changed its text style, its size or its place by other   */
/* than whole pixels.                                                         */
/* ========================================================================== */

typedef struct {
  civue_ui *ui;
  // whether the block runs.
  bool build;
  bool recording;
} civue_memo_scope;

civue_memo_scope civue_memo_begin(civue_ui *ui, civue_id id, uint64_t inputs);
void civue_memo_end(civue_memo_scope *s);

#define CIVUE__MEMO CIVUE__CAT(civue__memo_, __LINE__)

#define CIVUE_MEMO(ui, id, inputs)                                          \
  for (civue_memo_scope CIVUE__MEMO                                         \
       [[gnu::cleanup(civue_memo_end)]] = civue_memo_begin(ui, id, inputs); \
       CIVUE__MEMO.build; CIVUE__MEMO.build = false)

/* ========================================================================== */
/* gpui's style names, the ones <civue/dsl.h> takes in div and text. each     */
/* name is pasted after CIVUE__S_, so they exist nowhere else and no header   */
/* or variable of the app can clash with them.                                */
/* ========================================================================== */

#define CIVUE__P ()
#define CIVUE__X(...) CIVUE__X3(CIVUE__X3(CIVUE__X3(CIVUE__X3(__VA_ARGS__))))
#define CIVUE__X3(...) CIVUE__X2(CIVUE__X2(CIVUE__X2(CIVUE__X2(__VA_ARGS__))))
#define CIVUE__X2(...) CIVUE__X1(CIVUE__X1(CIVUE__X1(CIVUE__X1(__VA_ARGS__))))
#define CIVUE__X1(...) __VA_ARGS__
#define CIVUE__STYLES(...) __VA_OPT__(CIVUE__X(CIVUE__STYLES_(__VA_ARGS__)))
#define CIVUE__STYLES_(a, ...) \
  CIVUE__S_##a __VA_OPT__(, CIVUE__STYLES_AGAIN CIVUE__P(__VA_ARGS__))
#define CIVUE__STYLES_AGAIN() CIVUE__STYLES_

// the same again for hover and active, which sit inside a style.
#define CIVUE__Y(...) CIVUE__Y2(CIVUE__Y2(CIVUE__Y2(CIVUE__Y2(__VA_ARGS__))))
#define CIVUE__Y2(...) CIVUE__Y1(CIVUE__Y1(CIVUE__Y1(CIVUE__Y1(__VA_ARGS__))))
#define CIVUE__Y1(...) __VA_ARGS__
#define CIVUE__PAINTS(...) __VA_OPT__(CIVUE__Y(CIVUE__PAINTS_(__VA_ARGS__)))
#define CIVUE__PAINTS_(a, ...) \
  CIVUE__S_##a __VA_OPT__(, CIVUE__PAINTS_AGAIN CIVUE__P(__VA_ARGS__))
#define CIVUE__PAINTS_AGAIN() CIVUE__PAINTS_

// any field as it is, like style(.justify = on ? end : start).
#define CIVUE__S_style(...) __VA_ARGS__
#define CIVUE__S_key(v) .key = (v)

#define CIVUE__S_block .display = CIVUE_BLOCK
#define CIVUE__S_flex .display = CIVUE_FLEX
#define CIVUE__S_grid .display = CIVUE_GRID
#define CIVUE__S_flex_row .direction = CIVUE_ROW
#define CIVUE__S_flex_col .direction = CIVUE_COL
#define CIVUE__S_flex_row_reverse .direction = CIVUE_ROW_REVERSE
#define CIVUE__S_flex_col_reverse .direction = CIVUE_COL_REVERSE
#define CIVUE__S_flex_wrap .wrap = true
#define CIVUE__S_flex_nowrap .wrap = false
#define CIVUE__S_flex_grow .grow = 1
#define CIVUE__S_flex_grow_0 .grow = 0
#define CIVUE__S_flex_shrink .shrink = 1, .has_shrink = true
#define CIVUE__S_flex_shrink_0 .shrink = 0, .has_shrink = true
#define CIVUE__S_flex_1 \
  .grow = 1, .shrink = 1, .has_shrink = true, .basis = CIVUE__L_0
#define CIVUE__S_flex_auto \
  .grow = 1, .shrink = 1, .has_shrink = true, .basis = CIVUE__L_auto
#define CIVUE__S_flex_initial \
  .grow = 0, .shrink = 1, .has_shrink = true, .basis = CIVUE__L_auto
#define CIVUE__S_flex_none \
  .grow = 0, .shrink = 0, .has_shrink = true, .basis = CIVUE__L_auto
#define CIVUE__S_flex_basis(v) .basis = CIVUE__L_##v

#define CIVUE__S_grid_cols(n) .cols = (n)
#define CIVUE__S_grid_rows(n) .rows = (n)
#define CIVUE__S_col_span(n) .col_span = (n)
#define CIVUE__S_col_span_full .col_span = CIVUE_SPAN_FULL
#define CIVUE__S_row_span(n) .row_span = (n)
#define CIVUE__S_col_start(n) .col_start = (n)
#define CIVUE__S_col_end(n) .col_end = (n)
#define CIVUE__S_row_start(n) .row_start = (n)
#define CIVUE__S_row_end(n) .row_end = (n)

#define CIVUE__S_items_start .align_items = CIVUE_PLACE_START
#define CIVUE__S_items_end .align_items = CIVUE_PLACE_END
#define CIVUE__S_items_center .align_items = CIVUE_PLACE_CENTER
#define CIVUE__S_items_stretch .align_items = CIVUE_PLACE_STRETCH
#define CIVUE__S_items_baseline .align_items = CIVUE_PLACE_BASELINE
#define CIVUE__S_self_start .align_self = CIVUE_PLACE_START
#define CIVUE__S_self_end .align_self = CIVUE_PLACE_END
#define CIVUE__S_self_center .align_self = CIVUE_PLACE_CENTER
#define CIVUE__S_self_stretch .align_self = CIVUE_PLACE_STRETCH
#define CIVUE__S_self_baseline .align_self = CIVUE_PLACE_BASELINE
#define CIVUE__S_justify_start .justify = CIVUE_PLACE_START
#define CIVUE__S_justify_end .justify = CIVUE_PLACE_END
#define CIVUE__S_justify_center .justify = CIVUE_PLACE_CENTER
#define CIVUE__S_justify_between .justify = CIVUE_PLACE_BETWEEN
#define CIVUE__S_justify_around .justify = CIVUE_PLACE_AROUND
#define CIVUE__S_justify_evenly .justify = CIVUE_PLACE_EVENLY
#define CIVUE__S_content_start .align_content = CIVUE_PLACE_START
#define CIVUE__S_content_end .align_content = CIVUE_PLACE_END
#define CIVUE__S_content_center .align_content = CIVUE_PLACE_CENTER
#define CIVUE__S_content_stretch .align_content = CIVUE_PLACE_STRETCH
#define CIVUE__S_content_between .align_content = CIVUE_PLACE_BETWEEN
#define CIVUE__S_content_around .align_content = CIVUE_PLACE_AROUND
#define CIVUE__S_content_evenly .align_content = CIVUE_PLACE_EVENLY

#define CIVUE__S_relative .position = CIVUE_RELATIVE
#define CIVUE__S_absolute .position = CIVUE_ABSOLUTE
#define CIVUE__S_overflow_hidden \
  .overflow_x = CIVUE_OVERFLOW_HIDDEN, .overflow_y = CIVUE_OVERFLOW_HIDDEN
#define CIVUE__S_overflow_x_hidden .overflow_x = CIVUE_OVERFLOW_HIDDEN
#define CIVUE__S_overflow_y_hidden .overflow_y = CIVUE_OVERFLOW_HIDDEN
#define CIVUE__S_overflow_scroll \
  .overflow_x = CIVUE_OVERFLOW_SCROLL, .overflow_y = CIVUE_OVERFLOW_SCROLL
#define CIVUE__S_overflow_x_scroll .overflow_x = CIVUE_OVERFLOW_SCROLL
#define CIVUE__S_overflow_y_scroll .overflow_y = CIVUE_OVERFLOW_SCROLL
#define CIVUE__S_scrollbars(look) .scrollbars = (look)

#define CIVUE__S_w(v) .w = CIVUE__L_##v
#define CIVUE__S_h(v) .h = CIVUE__L_##v
#define CIVUE__S_size(v) .w = CIVUE__L_##v, .h = CIVUE__L_##v
#define CIVUE__S_min_w(v) .min_w = CIVUE__L_##v
#define CIVUE__S_min_h(v) .min_h = CIVUE__L_##v
#define CIVUE__S_min_size(v) .min_w = CIVUE__L_##v, .min_h = CIVUE__L_##v
#define CIVUE__S_max_w(v) .max_w = CIVUE__L_##v
#define CIVUE__S_max_h(v) .max_h = CIVUE__L_##v
#define CIVUE__S_max_size(v) .max_w = CIVUE__L_##v, .max_h = CIVUE__L_##v
#define CIVUE__S_aspect_ratio(v) .aspect = (v)
#define CIVUE__S_aspect_square .aspect = 1

#define CIVUE__S_p(v) \
  .padding = {CIVUE__L_##v, CIVUE__L_##v, CIVUE__L_##v, CIVUE__L_##v}
#define CIVUE__S_px(v) .padding[1] = CIVUE__L_##v, .padding[3] = CIVUE__L_##v
#define CIVUE__S_py(v) .padding[0] = CIVUE__L_##v, .padding[2] = CIVUE__L_##v
#define CIVUE__S_pt(v) .padding[0] = CIVUE__L_##v
#define CIVUE__S_pr(v) .padding[1] = CIVUE__L_##v
#define CIVUE__S_pb(v) .padding[2] = CIVUE__L_##v
#define CIVUE__S_pl(v) .padding[3] = CIVUE__L_##v

#define CIVUE__S_m(v) \
  .margin = {CIVUE__L_##v, CIVUE__L_##v, CIVUE__L_##v, CIVUE__L_##v}
#define CIVUE__S_mx(v) .margin[1] = CIVUE__L_##v, .margin[3] = CIVUE__L_##v
#define CIVUE__S_my(v) .margin[0] = CIVUE__L_##v, .margin[2] = CIVUE__L_##v
#define CIVUE__S_mt(v) .margin[0] = CIVUE__L_##v
#define CIVUE__S_mr(v) .margin[1] = CIVUE__L_##v
#define CIVUE__S_mb(v) .margin[2] = CIVUE__L_##v
#define CIVUE__S_ml(v) .margin[3] = CIVUE__L_##v

#define CIVUE__S_gap(v) .gap_x = CIVUE__L_##v, .gap_y = CIVUE__L_##v
#define CIVUE__S_gap_x(v) .gap_x = CIVUE__L_##v
#define CIVUE__S_gap_y(v) .gap_y = CIVUE__L_##v

#define CIVUE__S_inset(v) \
  .inset = {CIVUE__L_##v, CIVUE__L_##v, CIVUE__L_##v, CIVUE__L_##v}
#define CIVUE__S_top(v) .inset[0] = CIVUE__L_##v
#define CIVUE__S_right(v) .inset[1] = CIVUE__L_##v
#define CIVUE__S_bottom(v) .inset[2] = CIVUE__L_##v
#define CIVUE__S_left(v) .inset[3] = CIVUE__L_##v

#define CIVUE__S_text_size(v) .text_size = CIVUE__T_##v
#define CIVUE__S_line_height(v) .line_height = CIVUE__H_##v
#define CIVUE__S_font_weight(v) .font_weight = CIVUE__W_##v
#define CIVUE__S_font_mono .mono = CIVUE_ON
#define CIVUE__S_italic .italic = CIVUE_ON
#define CIVUE__S_not_italic .italic = CIVUE_OFF
#define CIVUE__S_tabular_nums .tnum = CIVUE_ON
#define CIVUE__S_text_left .text_align = CIVUE_TEXT_LEFT
#define CIVUE__S_text_center .text_align = CIVUE_TEXT_CENTER
#define CIVUE__S_text_right .text_align = CIVUE_TEXT_RIGHT
#define CIVUE__S_whitespace_normal .nowrap = CIVUE_OFF
#define CIVUE__S_whitespace_nowrap .nowrap = CIVUE_ON
#define CIVUE__S_truncate .truncate = CIVUE_ON

#define CIVUE__S_bg(c) .bg = (c), .has_bg = true
#define CIVUE__S_border_color(c) .border_color = (c), .has_border_color = true
#define CIVUE__S_text_color(c) .text_color = (c), .has_text_color = true
#define CIVUE__S_hover(...) .hover = {CIVUE__PAINTS(__VA_ARGS__)}
#define CIVUE__S_active(...) .active = {CIVUE__PAINTS(__VA_ARGS__)}

// border widths are points.
#define CIVUE__S_border(v) .border_w = {(v), (v), (v), (v)}
#define CIVUE__S_border_x(v) .border_w[1] = (v), .border_w[3] = (v)
#define CIVUE__S_border_y(v) .border_w[0] = (v), .border_w[2] = (v)
#define CIVUE__S_border_t(v) .border_w[0] = (v)
#define CIVUE__S_border_r(v) .border_w[1] = (v)
#define CIVUE__S_border_b(v) .border_w[2] = (v)
#define CIVUE__S_border_l(v) .border_w[3] = (v)

#define CIVUE__S_rounded(v) \
  .radius = {CIVUE__R_##v, CIVUE__R_##v, CIVUE__R_##v, CIVUE__R_##v}
#define CIVUE__S_rounded_t(v) \
  .radius[0] = CIVUE__R_##v, .radius[1] = CIVUE__R_##v
#define CIVUE__S_rounded_r(v) \
  .radius[1] = CIVUE__R_##v, .radius[2] = CIVUE__R_##v
#define CIVUE__S_rounded_b(v) \
  .radius[2] = CIVUE__R_##v, .radius[3] = CIVUE__R_##v
#define CIVUE__S_rounded_l(v) \
  .radius[3] = CIVUE__R_##v, .radius[0] = CIVUE__R_##v
#define CIVUE__S_rounded_tl(v) .radius[0] = CIVUE__R_##v
#define CIVUE__S_rounded_tr(v) .radius[1] = CIVUE__R_##v
#define CIVUE__S_rounded_br(v) .radius[2] = CIVUE__R_##v
#define CIVUE__S_rounded_bl(v) .radius[3] = CIVUE__R_##v
#define CIVUE__S_shadow(v) .shadow = CIVUE__SH_##v

#define CIVUE__S_cursor_default \
  .cursor = CIVUE_CURSOR_DEFAULT, .has_cursor = true
#define CIVUE__S_cursor_pointer \
  .cursor = CIVUE_CURSOR_POINTER, .has_cursor = true
#define CIVUE__S_cursor_text .cursor = CIVUE_CURSOR_TEXT, .has_cursor = true
#define CIVUE__S_cursor_crosshair \
  .cursor = CIVUE_CURSOR_CROSSHAIR, .has_cursor = true

/* ========================================================================== */
/* the values the style names take, gpui's: lengths on a scale of quarter     */
/* rems with fractions, full and auto, and px, rems and relative for the      */
/* rest.                                                                      */
/* ========================================================================== */

#define CIVUE__REM(v) ((civue_length){v, CIVUE_UNIT_REM})
#define CIVUE__REL(v) ((civue_length){v, CIVUE_UNIT_RELATIVE})

#define CIVUE__L_px(v) civue_px(v)
#define CIVUE__L_rems(v) civue_rems(v)
#define CIVUE__L_relative(v) civue_relative(v)

static constexpr civue_length CIVUE__L_px = {1, CIVUE_UNIT_PX};

#define CIVUE__L_0 ((civue_length){0, CIVUE_UNIT_PX})
#define CIVUE__L_0p5 CIVUE__REM(.125f)
#define CIVUE__L_1 CIVUE__REM(.25f)
#define CIVUE__L_1p5 CIVUE__REM(.375f)
#define CIVUE__L_2 CIVUE__REM(.5f)
#define CIVUE__L_2p5 CIVUE__REM(.625f)
#define CIVUE__L_3 CIVUE__REM(.75f)
#define CIVUE__L_3p5 CIVUE__REM(.875f)
#define CIVUE__L_4 CIVUE__REM(1)
#define CIVUE__L_5 CIVUE__REM(1.25f)
#define CIVUE__L_6 CIVUE__REM(1.5f)
#define CIVUE__L_7 CIVUE__REM(1.75f)
#define CIVUE__L_8 CIVUE__REM(2)
#define CIVUE__L_9 CIVUE__REM(2.25f)
#define CIVUE__L_10 CIVUE__REM(2.5f)
#define CIVUE__L_11 CIVUE__REM(2.75f)
#define CIVUE__L_12 CIVUE__REM(3)
#define CIVUE__L_16 CIVUE__REM(4)
#define CIVUE__L_20 CIVUE__REM(5)
#define CIVUE__L_24 CIVUE__REM(6)
#define CIVUE__L_32 CIVUE__REM(8)
#define CIVUE__L_40 CIVUE__REM(10)
#define CIVUE__L_48 CIVUE__REM(12)
#define CIVUE__L_56 CIVUE__REM(14)
#define CIVUE__L_64 CIVUE__REM(16)
#define CIVUE__L_72 CIVUE__REM(18)
#define CIVUE__L_80 CIVUE__REM(20)
#define CIVUE__L_96 CIVUE__REM(24)
#define CIVUE__L_112 CIVUE__REM(28)
#define CIVUE__L_128 CIVUE__REM(32)
#define CIVUE__L_auto ((civue_length){0, CIVUE_UNIT_AUTO})
#define CIVUE__L_full CIVUE__REL(1)
#define CIVUE__L_1_2 CIVUE__REL(1.f / 2)
#define CIVUE__L_1_3 CIVUE__REL(1.f / 3)
#define CIVUE__L_2_3 CIVUE__REL(2.f / 3)
#define CIVUE__L_1_4 CIVUE__REL(1.f / 4)
#define CIVUE__L_2_4 CIVUE__REL(2.f / 4)
#define CIVUE__L_3_4 CIVUE__REL(3.f / 4)
#define CIVUE__L_1_5 CIVUE__REL(1.f / 5)
#define CIVUE__L_2_5 CIVUE__REL(2.f / 5)
#define CIVUE__L_3_5 CIVUE__REL(3.f / 5)
#define CIVUE__L_4_5 CIVUE__REL(4.f / 5)
#define CIVUE__L_1_6 CIVUE__REL(1.f / 6)
#define CIVUE__L_5_6 CIVUE__REL(5.f / 6)
#define CIVUE__L_1_12 CIVUE__REL(1.f / 12)

#define CIVUE__R_px(v) civue_px(v)
#define CIVUE__R_rems(v) civue_rems(v)
#define CIVUE__R_none ((civue_length){0, CIVUE_UNIT_PX})
#define CIVUE__R_xs CIVUE__REM(.125f)
#define CIVUE__R_sm CIVUE__REM(.25f)
#define CIVUE__R_md CIVUE__REM(.375f)
#define CIVUE__R_lg CIVUE__REM(.5f)
#define CIVUE__R_xl CIVUE__REM(.75f)
#define CIVUE__R_2xl CIVUE__REM(1)
#define CIVUE__R_3xl CIVUE__REM(1.5f)
#define CIVUE__R_full ((civue_length){9999, CIVUE_UNIT_PX})

#define CIVUE__T_px(v) civue_px(v)
#define CIVUE__T_rems(v) civue_rems(v)
#define CIVUE__T_xs CIVUE__REM(.75f)
#define CIVUE__T_sm CIVUE__REM(.875f)
#define CIVUE__T_base CIVUE__REM(1)
#define CIVUE__T_lg CIVUE__REM(1.125f)
#define CIVUE__T_xl CIVUE__REM(1.25f)
#define CIVUE__T_2xl CIVUE__REM(1.5f)
#define CIVUE__T_3xl CIVUE__REM(1.875f)
#define CIVUE__T_4xl CIVUE__REM(2.25f)
#define CIVUE__T_5xl CIVUE__REM(3)
#define CIVUE__T_6xl CIVUE__REM(3.75f)

#define CIVUE__H_px(v) civue_px(v)
#define CIVUE__H_rems(v) civue_rems(v)
#define CIVUE__H_relative(v) civue_relative(v)
#define CIVUE__H_none CIVUE__REL(1)
#define CIVUE__H_tight CIVUE__REL(1.25f)
#define CIVUE__H_snug CIVUE__REL(1.375f)
#define CIVUE__H_normal CIVUE__REL(1.5f)
#define CIVUE__H_relaxed CIVUE__REL(1.625f)
#define CIVUE__H_loose CIVUE__REL(2)

#define CIVUE__W_thin 100.f
#define CIVUE__W_extralight 200.f
#define CIVUE__W_light 300.f
#define CIVUE__W_normal 400.f
#define CIVUE__W_medium 500.f
#define CIVUE__W_semibold 600.f
#define CIVUE__W_bold 700.f
#define CIVUE__W_extrabold 800.f
#define CIVUE__W_black 900.f

#define CIVUE__SH_none CIVUE_SHADOW_NONE
#define CIVUE__SH_sm CIVUE_SHADOW_SM
#define CIVUE__SH_md CIVUE_SHADOW_MD
#define CIVUE__SH_lg CIVUE_SHADOW_LG
#define CIVUE__SH_xl CIVUE_SHADOW_XL
#define CIVUE__SH_2xl CIVUE_SHADOW_2XL

#ifdef __cplusplus
}
#endif

#endif
