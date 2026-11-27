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
  // x, y, width and height of a rounded clip, and its radii from the top
  // left clockwise. none when every radius is zero.
  float round[4];
  float round_radii[4];
} civue_quad;

// a rounded clip, laid out as a quad's.
typedef struct {
  float rect[4];
  float radii[4];
} civue_round;

// where a glyph goes, x, y, width and height, and what of the atlas it
// shows.
typedef struct {
  float rect[4];
  float uv[4];
} civue_glyph_quad;

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
  // what it covers is taken away, alpha too, by its coverage.
  bool erase;
} civue_batch;

typedef struct {
  CIVUE_VEC(civue_quad) quads;
  CIVUE_VEC(civue_batch) batches;
  CIVUE_VEC(civue_rect) clips;
  CIVUE_VEC(civue_round) rounds;
  // what is pushed multiplies what is under it, like a highlighter.
  bool multiply;
  // what is pushed takes away what is under it, leaving it see through.
  bool erase;
  float turn[4];
  // quads the clip hides are kept, for what is recorded to be drawn again
  // somewhere else.
  bool keep_hidden;
  // clips pushed that the one before them cut.
  uint32_t cuts;
} civue_draw_list;

void civue_dl_clear(civue_draw_list *d);
void civue_dl_free(civue_draw_list *d);
// adds what `other` draws after what this one does.
void civue_dl_append(civue_draw_list *d, const civue_draw_list *other);
// draws `count` recorded quads again, moved by `dx` and `dy` pixels. a quad
// that `was`, the clip when it was recorded, clipped takes the clip of now,
// any other keeps its own, moved and cut by it. rounded clips go the same
// way from `round_was`. none may be an image or turned.
void civue_dl_replay(civue_draw_list *d, const civue_quad *quads,
                     uint32_t count, float dx, float dy, const float was[4],
                     const civue_round *round_was);
// moves everything drawn by `dx` and `dy` pixels, clips too.
void civue_dl_shift(civue_draw_list *d, float dx, float dy);
// restricts what follows to `r` until civue_dl_pop_clip.
void civue_dl_push_clip(civue_draw_list *d, civue_rect r);
void civue_dl_pop_clip(civue_draw_list *d);
[[gnu::pure]] civue_rect civue_dl_clip(const civue_draw_list *d);
// rounds the corners of what follows to those of `r` until
// civue_dl_pop_round. clips to `r` itself are pushed apart. only the last
// one pushed rounds.
void civue_dl_push_round(civue_draw_list *d, civue_rect r,
                         const float radii[4]);
void civue_dl_pop_round(civue_draw_list *d);
[[gnu::pure]] civue_round civue_dl_round(const civue_draw_list *d);
// turns what is pushed next by `angle` radians around `pivot`, a zero angle
// stops turning.
void civue_dl_set_turn(civue_draw_list *d, float angle, civue_vec2 pivot);
void civue_dl_set_multiply(civue_draw_list *d, bool on);
// what is pushed next takes away what it covers, like a mask cutting a hole
// in a window that blends with what is under it.
void civue_dl_set_erase(civue_draw_list *d, bool on);

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
// civue_dl_mask for each of `count` glyphs tinted alike, in one go.
void civue_dl_masks(civue_draw_list *d, const civue_glyph_quad *quads,
                    uint32_t count, civue_color color, float gamma);
// pixels from the color atlas, faded.
void civue_dl_color_glyph(civue_draw_list *d, civue_rect r, const float uv[4],
                          float opacity);
// a registered image stretched over `r`, uv picks the part shown.
void civue_dl_image(civue_draw_list *d, civue_texture t, civue_rect r,
                    const float uv[4], float opacity, float radius,
                    bool nearest);
// the same, each of its pixels times `tint`, premultiplied as they are: red,
// green, blue and alpha. an alpha of zero adds the image to what is under
// it.
void civue_dl_image_tinted(civue_draw_list *d, civue_texture t, civue_rect r,
                           const float uv[4], const float tint[4],
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
  // the atlas keeps its pixels for <civue/raster.h>, CIVUE_ATLAS_SIZE a
  // side, and uploads carry no copy of theirs. past a few uploads waiting
  // the atlas is `stale` instead: all of it is to upload.
  bool keeps, stale;
  uint8_t *pixels;
  // called before the atlas starts over or frees its pixels, when a renderer
  // whose gpu reads them in place has frames to wait for.
  void (*reusing)(void *ctx);
  void *reusing_ctx;
} civue_atlas;

// an image's pixels the cpu reads: premultiplied rgba8, rows from the top.
typedef struct {
  const uint8_t *pixels;
  uint32_t width, height;
  size_t stride;
} civue_pixels;

// room for a w by h bitmap, its uv returned, false when full. `data` is copied.
bool civue_atlas_insert(civue_atlas *a, uint32_t w, uint32_t h,
                        const uint8_t *data, float uv[4]);
void civue_atlas_clear(civue_atlas *a);
void civue_atlas_free(civue_atlas *a);

#ifdef __cplusplus
}
#endif

#endif
