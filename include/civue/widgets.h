#ifndef CIVUE_WIDGETS_H
#define CIVUE_WIDGETS_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* widgets: buttons, sliders, segmented controls, checkboxes and switches, fields, dropdowns, scroll areas,   */
/* menus, tooltips, floating surfaces and progress indicators, drawn like figma's ui3. each is a call per     */
/* frame returning what the pointer and the keys did with it.                                                 */
/* ========================================================================================================== */

typedef enum : uint8_t {
  CIVUE_BUTTON_PRIMARY,
  CIVUE_BUTTON_SECONDARY,
  CIVUE_BUTTON_GHOST,
  // red, for what can't be undone.
  CIVUE_BUTTON_DANGER
} civue_button_kind;
civue_response civue_button(civue_ui *ui, civue_id id, civue_rect r, const char *label, civue_button_kind kind);
float civue_button_width(civue_ui *ui, const char *label, civue_text_style style);
// a dialog's title row over `r` and its close button, which sets `close`.
void civue_dialog_head(civue_ui *ui, civue_id id, civue_rect r, const char *title, bool *close);
typedef struct {
  double value;
  bool ended;
} civue_slider_change;
// returns true when the value changes or the drag ends.
bool civue_slider(civue_ui *ui, civue_id id, civue_rect r, double value, double min, double max, double anchor,
                  double step, civue_slider_change *out);
civue_response civue_icon_button(civue_ui *ui, civue_id id, civue_rect r, civue_icon icon, bool selected);
civue_response civue_panel_icon(civue_ui *ui, civue_id id, civue_rect r, civue_icon icon);

typedef enum : uint8_t {
  CIVUE_E100,
  CIVUE_E100_CANVAS,
  CIVUE_E200,
  CIVUE_E200_CANVAS,
  CIVUE_E300,
  CIVUE_E300_TOOLTIP,
  CIVUE_E400,
  CIVUE_E400_MENU,
  CIVUE_E500,
  CIVUE_E500_MODAL
} civue_elevation;
// shadows go before the fill, the dark theme's inner edge after it.
void civue_elevation_shadow(civue_ui *ui, civue_rect r, float radius, civue_elevation e);
void civue_elevation_edge(civue_ui *ui, civue_rect r, float radius, civue_elevation e);
void civue_floating(civue_ui *ui, civue_rect r, float radius, civue_color fill, civue_elevation e);

typedef enum : uint8_t { CIVUE_LABEL_NONE, CIVUE_LABEL_TEXT, CIVUE_LABEL_STRONG, CIVUE_LABEL_ICON } civue_label_kind;
typedef struct {
  civue_label_kind kind;
  const char *text;
  civue_icon icon;
} civue_label;
void civue_field(civue_ui *ui, civue_rect r, civue_label label, const char *value);
civue_response civue_dropdown_outlined(civue_ui *ui, civue_id id, civue_rect r, const char *value);
civue_response civue_dropdown(civue_ui *ui, civue_id id, civue_rect r, const char *value, bool placeholder);
// selection indices are -1 when absent.
int32_t civue_segmented(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, uint32_t count,
                        int32_t selected);
int32_t civue_segmented_dimmed(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, uint32_t count,
                               int32_t selected, bool dimmed);
int32_t civue_segmented_sized(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, uint32_t count,
                              int32_t selected, bool dimmed, float size);
int32_t civue_segmented_tips(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, uint32_t count,
                             const char *const *tips, uint32_t ntips, int32_t selected, bool dimmed, float size);
int32_t civue_button_group(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, uint32_t count,
                           bool enabled);
int32_t civue_button_group_dimmed(civue_ui *ui, civue_id id, civue_rect r, const civue_icon *icons, uint32_t count,
                                  bool enabled, bool dimmed);
bool civue_checkbox(civue_ui *ui, civue_id id, civue_vec2 at, bool checked, const char *label);
bool civue_checkbox_brand(civue_ui *ui, civue_id id, civue_vec2 at, bool checked, const char *label);
// paints a switch, the caller owns its interaction.
void civue_switch_draw(civue_ui *ui, civue_rect r, bool on);

// each begin returns the shifted content rect and requires civue_scroll_end.
civue_rect civue_scroll_area(civue_ui *ui, civue_id id, civue_rect r, float content_height);
civue_rect civue_scroll_area_xy(civue_ui *ui, civue_id id, civue_rect r, float content_width, float content_height);
void civue_scroll_end(civue_ui *ui);
float civue_scroll_x(civue_ui *ui, civue_id id);
void civue_set_scroll_y(civue_ui *ui, civue_id id, float y);
void civue_set_scroll_x(civue_ui *ui, civue_id id, float x);
void civue_scroll_to_center(civue_ui *ui, civue_id id, float y0, float y1, float view, bool always);
void civue_scroll_to(civue_ui *ui, civue_id id, float y0, float y1, float view);
float civue_vertical_splitter(civue_ui *ui, civue_id id, float x, float y0, float y1);

typedef enum : uint8_t {
  CIVUE_MENU_IMAGE_NONE,
  CIVUE_MENU_IMAGE_TEXTURE,
  CIVUE_MENU_IMAGE_INITIAL
} civue_menu_image_kind;
typedef struct {
  civue_menu_image_kind kind;
  civue_texture texture;
  const char *initial;
  civue_color color;
} civue_menu_image;
typedef struct civue_menu_item {
  const char *label, *shortcut, *badge;
  bool checkbox, checked, disabled, separator, header;
  bool has_icon;
  civue_icon icon;
  civue_menu_image image;
  const struct civue_menu_item *submenu;
  uint32_t nsubmenu;
  float height;
  bool (*draw)(civue_ui *ui, civue_rect r, void *ctx);
  void *ctx;
} civue_menu_item;
typedef struct {
  // initialize with civue_menu_style_default before overriding fields.
  float width, separator, gap;
  bool at_pointer, above, compact;
  bool has_floor, has_min_width;
  float floor, min_width, separator_inset;
  bool focus_first;
  int32_t focus_row;
  bool has_accent;
  civue_color accent;
  float label_lead;
  bool row_checks, has_label_style;
  civue_text_style label_style;
} civue_menu_style;
civue_menu_style civue_menu_style_default(void);
enum { CIVUE_MENU_MAX_DEPTH = 32 };
void civue_menu_image_draw(civue_ui *ui, civue_menu_image img, civue_rect r);
// path must hold CIVUE_MENU_MAX_DEPTH entries, depth is zero when none is chosen.
bool civue_menu_path(civue_ui *ui, civue_id id, civue_rect anchor, bool *open, const civue_menu_item *items,
                     uint32_t count, uint32_t *path, uint32_t *depth);
bool civue_menu_styled(civue_ui *ui, civue_id id, civue_rect anchor, bool *open, const civue_menu_item *items,
                       uint32_t count, const civue_menu_style *style, uint32_t *path, uint32_t *depth);
// the row of a menu whose submenu shows, as if the pointer were on it, until
// the pointer moves to another. call before the menu is drawn.
void civue_menu_open_row(civue_ui *ui, civue_id id, int32_t row);
int32_t civue_menu(civue_ui *ui, civue_id id, civue_rect anchor, bool *open, const civue_menu_item *items,
                   uint32_t count);
int32_t civue_select_menu(civue_ui *ui, civue_id id, civue_rect field, bool *open, const civue_menu_item *items,
                          uint32_t count);
int32_t civue_select_menu_wide(civue_ui *ui, civue_id id, civue_rect field, bool *open, const civue_menu_item *items,
                               uint32_t count);
int32_t civue_select_menu_start(civue_ui *ui, civue_id id, civue_rect field, bool *open, const civue_menu_item *items,
                                uint32_t count);

// tooltips appear after half a second of hover without dragging.
void civue_tooltip_shortcut(civue_ui *ui, civue_id id, const civue_response *resp, const char *text,
                            const char *shortcut);
void civue_tooltip_above(civue_ui *ui, civue_id id, const civue_response *resp, const char *text, const char *shortcut);
void civue_tooltip_right(civue_ui *ui, civue_id id, const civue_response *resp, const char *text);
void civue_tooltip_badge(civue_ui *ui, civue_id id, const civue_response *resp, const char *text, const char *badge);
void civue_tooltip(civue_ui *ui, civue_id id, const civue_response *resp, const char *text);
#define CIVUE_SPINNER civue_hex(0xaaaaaa)
void civue_spinner(civue_ui *ui, civue_rect r, civue_color color);
void civue_shimmer(civue_ui *ui, civue_rect r, float radius, float t, float opacity);
void civue_fade_out(civue_ui *ui, civue_rect r, civue_color color);
float civue_cubic_bezier(float x1, float y1, float x2, float y2, float t);
float civue_ease(float t);

// a scroll area over `r` of `height` points of content, `content` the shifted rect its body lays out in. a
// `break` or a `return` inside skips civue_scroll_end.
#define CIVUE_SCROLLING(ui, id, r, height, content)                                                     \
  for (civue_rect content = civue_scroll_area(ui, id, r, height), *CIVUE__ONCE = &content; CIVUE__ONCE; \
       CIVUE__ONCE = (civue_scroll_end(ui), nullptr))

#ifdef __cplusplus
}
#endif

#endif
