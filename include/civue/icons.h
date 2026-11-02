#ifndef CIVUE_ICONS_H
#define CIVUE_ICONS_H

#include <civue/draw.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* icons. svgs rasterized at device size into the coverage atlas and tinted when drawn. the app numbers its   */
/* own from 0, civue's widgets draw a few built in ones numbered from CIVUE_ICON_BUILTIN.                     */
/* ========================================================================================================== */

typedef uint16_t civue_icon;

typedef struct {
  const char *svg;
  size_t len;
} civue_icon_svg;

enum : civue_icon {
  CIVUE_ICON_BUILTIN = 0xff00,
  CIVUE_ICON_CARET_DOWN = CIVUE_ICON_BUILTIN,
  CIVUE_ICON_CARET_UP,
  CIVUE_ICON_CARET_RIGHT,
  CIVUE_ICON_CHECK,
  CIVUE_ICON_CLOSE,
  CIVUE_ICON_SPINNER,
  CIVUE_ICON_TOOLTIP_ARROW,
  CIVUE_ICON_TOOLTIP_ARROW_LEFT,
  CIVUE_ICON_TOOLTIP_ARROW_UP,
};

typedef struct civue_icons civue_icons;

civue_icons *civue_icons_new(void);
// the app's icons, `svgs` numbered from 0. it stays the app's and must outlive the registry.
void civue_icons_use(civue_icons *ic, const civue_icon_svg *svgs, uint32_t count);
void civue_icons_free(civue_icons *ic);
// the size an icon is drawn at in points, for icons made for one size. false for scalable ones, they fill what
// they are given.
bool civue_icon_natural_size(civue_icons *ic, civue_icon icon, float *w, float *h);
// draws an icon filling `r` in physical pixels, turned left to right when `mirror`.
void civue_icons_draw(civue_icons *ic, civue_draw_list *d, civue_atlas *atlas, civue_icon icon, civue_rect r,
                      civue_color c, bool mirror);
// rasterizes an svg at w by h pixels into premultiplied rgba8, false when it can't be read.
bool civue_svg_rasterize(const char *svg, size_t n, uint32_t w, uint32_t h, civue_bytes *rgba);

#ifdef __cplusplus
}
#endif

#endif
