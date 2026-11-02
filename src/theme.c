#include "internal.h"

#include <civue/theme.h>

#define RGB(c) RGBA((u32)(c) << 8 | 255)
#define RGBA(c) {((c) >> 24) / 255.0f, ((c) >> 16 & 255) / 255.0f, ((c) >> 8 & 255) / 255.0f, ((c) & 255) / 255.0f}

const civue_theme civue_theme_light = {
    .dark = false,
    .bg = RGB(0xffffff),
    .bg_secondary = RGB(0xf5f5f5),
    .bg_tertiary = RGB(0xe6e6e6),
    .bg_hover = RGB(0xf5f5f5),
    .bg_pressed = RGB(0xe6e6e6),
    .bg_selected = RGB(0xe5f4ff),
    .bg_selected_secondary = RGB(0xf2f9ff),
    .bg_selected_hover = RGB(0xbde3ff),
    .bg_selected_strong = RGB(0x0d99ff),
    .bg_brand = RGB(0x0d99ff),
    .bg_brand_hover = RGB(0x007be5),
    .bg_brand_pressed = RGB(0x0768cf),
    .bg_transparent_hover = RGBA(0x0000000d),
    .bg_transparent_pressed = RGBA(0x0000001a),
    .bg_transparent_secondary_hover = RGBA(0x0000001a),
    .bg_transparent_secondary_pressed = RGBA(0x00000026),
    .bg_component = RGB(0x9747ff),
    .bg_component_tertiary = RGB(0xf1e5ff),
    .bg_danger = RGB(0xf24822),
    .bg_disabled = RGB(0xd9d9d9),
    .text = RGBA(0x000000e5),
    .text_secondary = RGBA(0x00000080),
    .text_tertiary = RGBA(0x0000004d),
    .text_onselected = RGBA(0x000000e5),
    .text_selected = RGB(0x007be5),
    .text_component = RGB(0x8638e5),
    .text_component_tertiary = RGB(0xc5b2dc),
    .text_danger = RGB(0xdc3412),
    .text_brand = RGB(0x007be5),
    .text_onbrand = RGB(0xffffff),
    .icon = RGBA(0x000000e5),
    .icon_secondary = RGBA(0x00000080),
    .icon_tertiary = RGBA(0x0000004d),
    .icon_selected = RGB(0x007be5),
    .icon_component = RGB(0x8638e5),
    .icon_brand = RGB(0x007be5),
    .icon_onbrand = RGB(0xffffff),
    .border = RGB(0xe6e6e6),
    .border_strong = RGB(0x2c2c2c),
    .border_selected = RGB(0x0d99ff),
    .border_component = RGB(0xe4ccff),
    .border_translucent = RGBA(0x0000001a),
    .panel_border = RGBA(0x0000001a),
    .canvas = RGB(0xf5f5f5),
};

const civue_theme civue_theme_dark = {
    .dark = true,
    .bg = RGB(0x2c2c2c),
    .bg_secondary = RGB(0x383838),
    .bg_tertiary = RGB(0x444444),
    .bg_hover = RGB(0x383838),
    .bg_pressed = RGB(0x444444),
    .bg_selected = RGB(0x394360),
    .bg_selected_secondary = RGB(0x32394d),
    .bg_selected_hover = RGB(0x4a5878),
    .bg_selected_strong = RGB(0x0c8ce9),
    .bg_brand = RGB(0x0c8ce9),
    .bg_brand_hover = RGB(0x0a6dc2),
    .bg_brand_pressed = RGB(0x105cad),
    .bg_transparent_hover = RGBA(0xffffff0d),
    .bg_transparent_pressed = RGBA(0xffffff1a),
    .bg_transparent_secondary_hover = RGBA(0xffffff1a),
    .bg_transparent_secondary_pressed = RGBA(0xffffff26),
    .bg_component = RGB(0x8a38f5),
    .bg_component_tertiary = RGB(0x473956),
    .bg_danger = RGB(0xe03e1a),
    .bg_disabled = RGB(0x757575),
    .text = RGB(0xffffff),
    .text_secondary = RGBA(0xffffffb2),
    .text_tertiary = RGBA(0xffffff66),
    .text_onselected = RGBA(0xffffffe5),
    .text_selected = RGB(0x7cc4f8),
    .text_component = RGB(0xd1a8ff),
    .text_component_tertiary = RGB(0x7f699b),
    .text_danger = RGB(0xfca397),
    .text_brand = RGB(0x7cc4f8),
    .text_onbrand = RGB(0xffffff),
    .icon = RGB(0xffffff),
    .icon_secondary = RGBA(0xffffffb2),
    .icon_tertiary = RGBA(0xffffff66),
    .icon_selected = RGB(0x7cc4f8),
    .icon_component = RGB(0xd1a8ff),
    .icon_brand = RGB(0x7cc4f8),
    .icon_onbrand = RGB(0xffffff),
    .border = RGB(0x444444),
    .border_strong = RGBA(0xffffffe5),
    .border_selected = RGB(0x0c8ce9),
    .border_component = RGB(0x604d75),
    .border_translucent = RGBA(0xffffff1a),
    .panel_border = RGB(0x444444),
    .canvas = RGB(0x1e1e1e),
};

civue_theme civue_theme_enhanced(civue_theme t) {
  if (t.dark) {
    t.bg_selected = color_hex(0x394360);
    t.bg_selected_secondary = color_hex(0x282e3e);
    t.bg_selected_hover = color_hex(0x536383);
    t.bg_selected_strong = color_hex(0x80caff);
    t.bg_brand = color_hex(0x80caff);
    t.bg_brand_hover = color_hex(0xbde3ff);
    t.bg_brand_pressed = color_hex(0xe5f4ff);
    t.bg_component = color_hex(0xf1e5ff);
    t.bg_danger = color_hex(0xffe2e0);
    t.text_secondary = color_hex(0xffffff);
    t.text_tertiary = civue_hexa(0xffffffcc);
    t.text_onselected = color_hex(0xffffff);
    t.text_component = color_hex(0xe4ccff);
    t.text_component_tertiary = color_hex(0xe4ccff);
    t.text_onbrand = color_hex(0x2c2c2c);
    t.icon_tertiary = civue_hexa(0xffffffb2);
    t.icon_component = color_hex(0xe4ccff);
    t.icon_onbrand = color_hex(0x2c2c2c);
    t.border = color_hex(0x757575);
    t.border_selected = color_hex(0x80caff);
    t.border_component = color_hex(0xe4ccff);
  } else {
    t.bg_selected_strong = color_hex(0x80caff);
    t.bg_brand = color_hex(0x034ac1);
    t.bg_brand_hover = color_hex(0x093077);
    t.bg_brand_pressed = color_hex(0x0d193f);
    t.bg_component = color_hex(0x681abb);
    t.bg_danger = color_hex(0x9f1f18);
    t.text_secondary = civue_hexa(0x000000cc);
    t.text_tertiary = color_hex(0x444444);
    t.text_onselected = color_hex(0x000000);
    t.text_selected = color_hex(0x034ac1);
    t.text_component = color_hex(0x681abb);
    t.text_component_tertiary = color_hex(0x681abb);
    t.text_danger = color_hex(0x9f1f18);
    t.text_brand = color_hex(0x034ac1);
    t.icon_secondary = civue_hexa(0x000000cc);
    t.icon_tertiary = civue_hexa(0x000000cc);
    t.icon_selected = color_hex(0x034ac1);
    t.icon_component = color_hex(0x681abb);
    t.icon_brand = color_hex(0x034ac1);
    t.border = color_hex(0x757575);
    t.border_selected = color_hex(0x034ac1);
    t.border_component = color_hex(0x681abb);
  }
  return t;
}
