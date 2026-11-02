#ifndef CIVUE_THEME_H
#define CIVUE_THEME_H

#include <civue/text.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* themes. the colors, type scale and radii the widgets draw with, by role: backgrounds, text, icons and      */
/* borders. the built in light and dark ones follow figma's ui3. menus and tooltips are dark in both.         */
/* ========================================================================================================== */

typedef struct {
  bool dark;
  civue_color bg;
  civue_color bg_secondary;
  civue_color bg_tertiary;
  civue_color bg_hover;
  civue_color bg_pressed;
  civue_color bg_selected;
  civue_color bg_selected_secondary;
  civue_color bg_selected_hover;
  civue_color bg_selected_strong;
  civue_color bg_brand;
  civue_color bg_brand_hover;
  civue_color bg_brand_pressed;
  civue_color bg_transparent_hover;
  civue_color bg_transparent_pressed;
  civue_color bg_transparent_secondary_hover;
  civue_color bg_transparent_secondary_pressed;
  civue_color bg_component;
  civue_color bg_component_tertiary;
  civue_color bg_danger;
  civue_color bg_disabled;
  civue_color text;
  civue_color text_secondary;
  civue_color text_tertiary;
  civue_color text_onselected;
  civue_color text_selected;
  civue_color text_component;
  civue_color text_component_tertiary;
  civue_color text_danger;
  civue_color text_brand;
  civue_color text_onbrand;
  civue_color icon;
  civue_color icon_secondary;
  civue_color icon_tertiary;
  civue_color icon_selected;
  civue_color icon_component;
  civue_color icon_brand;
  civue_color icon_onbrand;
  civue_color border;
  civue_color border_strong;
  civue_color border_selected;
  civue_color border_component;
  civue_color border_translucent;
  civue_color panel_border;
  civue_color canvas;
} civue_theme;

extern const civue_theme civue_theme_light, civue_theme_dark;
// the same theme in more contrast, like figma's enhanced contrast mode.
civue_theme civue_theme_enhanced(civue_theme t);

#define CIVUE_MENU_BG civue_hex(0x1e1e1e)
#define CIVUE_MENU_HOVER civue_hex(0x0c8ce9)
#define CIVUE_MENU_TEXT civue_hex(0xffffff)
#define CIVUE_MENU_TEXT_SECONDARY civue_hexa(0xffffffcc)
#define CIVUE_MENU_TEXT_DISABLED civue_hexa(0xffffff66)
#define CIVUE_MENU_BORDER civue_hex(0x383838)
#define CIVUE_MENU_HEADER civue_hexa(0xffffffb2)
#define CIVUE_MENU_BADGE civue_hex(0x444444)
#define CIVUE_TOOLTIP_BG civue_hex(0x1e1e1e)
#define CIVUE_TOOLTIP_TEXT civue_hex(0xffffff)
#define CIVUE_TOOLTIP_TEXT_SECONDARY civue_hexa(0xffffffb2)

#define CIVUE_BODY civue_style(11.0, 450.0, 16.0, 0.055)
#define CIVUE_BODY_REGULAR civue_style(11.0, 400.0, 16.0, 0.055)
#define CIVUE_BODY_MEDIUM civue_style(11.0, 500.0, 16.0, 0.055)
#define CIVUE_BODY_STRONG civue_style(11.0, 550.0, 16.0, 0.055)
#define CIVUE_BODY_BOLD civue_style(11.0, 600.0, 16.0, 0.055)
#define CIVUE_BODY_LARGE civue_style(13.0, 450.0, 22.0, -0.032)
#define CIVUE_BODY_LARGE_STRONG civue_style(13.0, 550.0, 22.0, -0.032)
#define CIVUE_HEADING_SMALL civue_style(13.0, 550.0, 22.0, -0.032)
#define CIVUE_HEADING_MEDIUM civue_style(15.0, 550.0, 25.0, -0.075)
#define CIVUE_HEADING_LARGE civue_style(24.0, 550.0, 32.0, -0.408)

constexpr float CIVUE_RADIUS_SMALL = 2.0;
constexpr float CIVUE_RADIUS_MEDIUM = 5.0;
constexpr float CIVUE_RADIUS_LARGE = 13.0;

#ifdef __cplusplus
}
#endif

#endif
