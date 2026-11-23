#ifndef CIVUE_RASTER_H
#define CIVUE_RASTER_H

#include <civue/draw.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* drawing on the cpu. a frame that changes little is cheaper to draw here    */
/* than on the gpu, whose driver keeps over a hundred megabytes around for as */
/* long as it is given work. only what changed is drawn again.                */
/* ========================================================================== */

// whole pixels, x1 and y1 past the last ones.
typedef struct {
  int32_t x0, y0, x1, y1;
} civue_box;

constexpr uint32_t CIVUE_DAMAGE_ROWS = 256;

// what of a window a frame changed: all of it, or tiles of it. empty when
// zeroed.
typedef struct {
  bool all;
  // whether any tile is marked.
  bool any;
  // a tile is 1 << across by 1 << down pixels, 128 across and 256 down at
  // most.
  uint8_t across, down;
  // a bit a tile, a row of tiles after another.
  uint64_t rows[CIVUE_DAMAGE_ROWS][2];
} civue_damage;

// empties `d` for a window of `width` by `height` pixels.
void civue_damage_begin(civue_damage *d, uint32_t width, uint32_t height);
// marks the tiles `b` is over, on a damage begun.
void civue_damage_add(civue_damage *d, civue_box b);
// adds what `other` changed.
void civue_damage_join(civue_damage *d, const civue_damage *other);
// takes what `other` changed out of `d`.
void civue_damage_without(civue_damage *d, const civue_damage *other);
// how many pixels of a `width` by `height` window it covers, at most.
[[gnu::pure]] uint64_t civue_damage_area(const civue_damage *d, uint32_t width,
                                         uint32_t height);
// calls `box` with each box of pixels it covers in a `width` by `height`
// window, none over another.
void civue_damage_each(const civue_damage *d, uint32_t width, uint32_t height,
                       void (*box)(void *ctx, civue_box b), void *ctx);
// what `next` shows that `last` did not, both drawn at the same size over
// the same color.
void civue_damage_between(const civue_draw_list *last,
                          const civue_draw_list *next, uint32_t width,
                          uint32_t height, civue_damage *out);

// pixels drawn into: premultiplied bgra8, rows from the top.
typedef struct {
  uint8_t *pixels;
  uint32_t width, height;
  size_t stride;
} civue_surface;

// where the cpu finds the pixels of the images a list draws: `find` is false
// for a texture only the gpu has.
typedef struct {
  bool (*find)(void *ctx, civue_texture t, civue_pixels *out);
  void *ctx;
} civue_raster_images;

// how much drawing `damage` of `list` takes, in pixels filled, or false when
// something there is only drawn by the gpu. `images` may be null, images are
// then the gpu's.
bool civue_raster_cost(const civue_draw_list *list, const civue_damage *damage,
                       uint32_t width, uint32_t height,
                       const civue_raster_images *images, uint64_t *cost);
// draws `damage` of `list` over `clear`, as civue_gfx_encode does on the
// gpu. the atlases keep their pixels, and `images` finds the images'.
void civue_raster(const civue_surface *s, const civue_draw_list *list,
                  civue_color clear, const civue_damage *damage,
                  const civue_atlas *mask, const civue_atlas *color,
                  const civue_raster_images *images);

#ifdef __cplusplus
}
#endif

#endif
