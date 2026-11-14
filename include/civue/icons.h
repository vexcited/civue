#ifndef CIVUE_ICONS_H
#define CIVUE_ICONS_H

#include <civue/draw.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* icons. svgs rasterized at device size, numbered from 0: one color ones     */
/* into the coverage atlas and tinted when drawn, like css's currentColor,    */
/* the others into the color atlas as they are.                               */
/* ========================================================================== */

typedef uint16_t civue_icon;

typedef struct {
  const char *svg;
  size_t len;
  // its own colors rather than the one it is drawn in.
  bool color;
} civue_icon_svg;

// an icon of any iconify set, `CIVUE_ICON(mdi, account)` for mdi:account,
// dashes written as underscores. CMake's civue_icons(target) finds the ones a
// target's sources name, fetches them at build time and makes this header,
// whose icons every new ui starts with.
#define CIVUE_ICON(set, name) CIVUE_ICON_##set##__##name
#if __has_include(<civue_icons.h>)
#include <civue_icons.h>
#endif

typedef struct civue_icons civue_icons;

// the icons every registry made after starts with, the build's own.
void civue_icons_preset(const civue_icon_svg *svgs, uint32_t count);
civue_icons *civue_icons_new();
// the app's icons, `svgs` numbered from 0. it stays the app's and must outlive
// the registry.
void civue_icons_use(civue_icons *ic, const civue_icon_svg *svgs,
                     uint32_t count);
void civue_icons_free(civue_icons *ic);
// the size an icon is drawn at in points, for icons made for one size. false
// for scalable ones, they fill what they are given.
bool civue_icon_natural_size(civue_icons *ic, civue_icon icon, float *w,
                             float *h);
// its width over its height, from its view box, 1 when it has none.
float civue_icon_aspect(civue_icons *ic, civue_icon icon);
// draws an icon filling `r` in physical pixels, turned left to right when
// `mirror`, into the coverage atlas or the color one for an icon of its own
// colors, faded by the color's alpha.
void civue_icons_draw(civue_icons *ic, civue_draw_list *d, civue_atlas *mask,
                      civue_atlas *color, civue_icon icon, civue_rect r,
                      civue_color c, bool mirror);
// rasterizes an svg at w by h pixels into premultiplied rgba8, false when it
// can't be read.
bool civue_svg_rasterize(const char *svg, size_t n, uint32_t w, uint32_t h,
                         civue_bytes *rgba);

#ifdef __cplusplus
}
#endif

#endif
