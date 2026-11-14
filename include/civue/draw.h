#ifndef CIVUE_DRAW_H
#define CIVUE_DRAW_H

#include <civue/base.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* draw lists. what a frame draws, platform neutral: instanced quads in       */
/* physical pixels, in order, batched by the image they sample. <civue/gfx.h> */
/* draws them on the gpu.                                                     */
/* ========================================================================== */

// side of the glyph and icon atlases.
constexpr uint32_t CIVUE_ATLAS_SIZE = 2048;

enum {
  CIVUE_QUAD_RECT = 0,
  CIVUE_QUAD_SHADOW = 1,
  CIVUE_QUAD_MASK = 2,
  CIVUE_QUAD_IMAGE = 3,
  CIVUE_QUAD_LINE = 4,
  CIVUE_QUAD_COLOR_GLYPH = 5,
};

// one quad, the layout the ui shaders read.
typedef struct {
  // x, y, width and height.
  float rect[4];
  // x0, y0, x1 and y1.
  float clip[4];
  // premultiplied.
  float color[4];
  // premultiplied, or a pixel grid.
  float border[4];
  // top left then clockwise, or a line's ends.
  float radii[4];
  // border width, kind, sigma or gamma, nearest.
  float params[4];
  float uv[4];
  // cos, sin and pivot of a turn, none when both are zero.
  float turn[4];
} civue_quad;

// an image the ui can draw, see civue_gfx_register_texture.
typedef uint64_t civue_texture;

// a pixel grid over a zoomed in canvas: where its units are in physical pixels.
// the first pixel after each unit's edge is recolored, like design tools show
// pixels.
typedef struct {
  civue_vec2 origin;
  // pixels per canvas unit.
  double scale;
} civue_pixel_grid;

typedef struct {
  uint32_t start;
  // 0 for none.
  civue_texture texture;
  bool multiply;
} civue_batch;

typedef struct {
  CIVUE_VEC(civue_quad) quads;
  CIVUE_VEC(civue_batch) batches;
  CIVUE_VEC(civue_rect) clips;
  // what is pushed multiplies what is under it, like a highlighter.
  bool multiply;
  float turn[4];
} civue_draw_list;

void civue_dl_clear(civue_draw_list *d);
void civue_dl_free(civue_draw_list *d);
// adds what `other` draws after what this one does.
void civue_dl_append(civue_draw_list *d, const civue_draw_list *other);
// restricts what follows to `r` until civue_dl_pop_clip.
void civue_dl_push_clip(civue_draw_list *d, civue_rect r);
void civue_dl_pop_clip(civue_draw_list *d);
civue_rect civue_dl_clip(const civue_draw_list *d);
// turns what is pushed next by `angle` radians around `pivot`, a zero angle
// stops turning.
void civue_dl_set_turn(civue_draw_list *d, float angle, civue_vec2 pivot);
void civue_dl_set_multiply(civue_draw_list *d, bool on);

// a rounded rect, with an inside border when border_width is positive. radii go
// from the top left clockwise.
void civue_dl_rect(civue_draw_list *d, civue_rect r, const float radii[4],
                   civue_color fill, civue_color border, float border_width);

static inline void civue_dl_fill(civue_draw_list *d, civue_rect r, float radius,
                                 civue_color fill) {
  civue_dl_rect(d, r, (float[4]){radius, radius, radius, radius}, fill,
                CIVUE_CLEAR, 0);
}

// a rounded rect with a border of its own width on each side, from the top
// clockwise.
void civue_dl_rect_sides(civue_draw_list *d, civue_rect r, const float radii[4],
                         civue_color fill, civue_color border,
                         const float widths[4]);
// a gaussian blurred rounded rect.
void civue_dl_shadow(civue_draw_list *d, civue_rect r, float radius,
                     float sigma, civue_color color);
// an antialiased segment, square ended when `square`.
void civue_dl_line(civue_draw_list *d, civue_vec2 a, civue_vec2 b, float width,
                   civue_color color, bool square);
// coverage from the mask atlas at `uv`, tinted. a gamma under 1 thickens it.
void civue_dl_mask(civue_draw_list *d, civue_rect r, const float uv[4],
                   civue_color color, float gamma);
// pixels from the color atlas, faded.
void civue_dl_color_glyph(civue_draw_list *d, civue_rect r, const float uv[4],
                          float opacity);
// a registered image stretched over `r`, uv picks the part shown.
void civue_dl_image(civue_draw_list *d, civue_texture t, civue_rect r,
                    const float uv[4], float opacity, float radius,
                    bool nearest);
// an opaque rect of a canvas, with its pixel grid when given.
void civue_dl_rect_grid(civue_draw_list *d, civue_rect r, civue_color fill,
                        const civue_pixel_grid *grid);
// an opaque image of a canvas, with its pixel grid when given.
void civue_dl_image_grid(civue_draw_list *d, civue_texture t, civue_rect r,
                         const float uv[4], bool nearest,
                         const civue_pixel_grid *grid);

/* ========================================================================== */
/* atlases. glyph and icon bitmaps packed in shelves, uploaded by the         */
/* renderer before it draws.                                                  */
/* ========================================================================== */

typedef struct {
  uint32_t x, y, w, h;
  civue_bytes data;
} civue_atlas_upload;

typedef struct {
  uint32_t y, height, x;
} civue_shelf;

typedef struct {
  // 1 for coverage, 4 for premultiplied rgba.
  uint32_t bytes_per_pixel;
  CIVUE_VEC(civue_shelf) shelves;
  // where the next shelf starts.
  uint32_t top;
  CIVUE_VEC(civue_atlas_upload) pending;
  // bumped when cleared, so cached entries know they are gone.
  uint64_t generation;
} civue_atlas;

// room for a w by h bitmap, its uv returned, false when full. `data` is copied.
bool civue_atlas_insert(civue_atlas *a, uint32_t w, uint32_t h,
                        const uint8_t *data, float uv[4]);
void civue_atlas_clear(civue_atlas *a);
void civue_atlas_free(civue_atlas *a);

#ifdef __cplusplus
}
#endif

#endif
