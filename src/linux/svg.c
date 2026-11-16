#include "../internal.h"

#include <civue/icons.h>

#include <cairo.h>
#include <librsvg/rsvg.h>
#include <limits.h>

bool civue_svg_rasterize(const char *svg, usize n, u32 w, u32 h,
                         civue_bytes *rgba) {
  if (!svg || !rgba || !w || !h || w > INT_MAX || h > INT_MAX ||
      (u64)w * h > UINT32_MAX / 4) {
    return false;
  }
  RsvgHandle *handle = rsvg_handle_new_from_data((const u8 *)svg, n, nullptr);
  if (!handle) {
    return false;
  }
  cairo_surface_t *surface =
    cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
  cairo_t *cr = cairo_create(surface);
  RsvgRectangle viewport = {0, 0, w, h};
  bool ok = rsvg_handle_render_document(handle, cr, &viewport, nullptr) &&
            cairo_status(cr) == CAIRO_STATUS_SUCCESS &&
            cairo_surface_status(surface) == CAIRO_STATUS_SUCCESS;
  if (ok) {
    cairo_surface_flush(surface);
    rgba->len = 0;
    vec_reserve(rgba, w * h * 4);
    rgba->len = w * h * 4;
    const u8 *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (u32 y = 0; y < h; y++) {
      const u32 *row = (const u32 *)(data + (usize)y * stride);
      for (u32 x = 0; x < w; x++) {
        u32 v = row[x];
        u8 *p = rgba->v + ((usize)y * w + x) * 4;
        p[0] = v >> 16;
        p[1] = v >> 8;
        p[2] = v;
        p[3] = v >> 24;
      }
    }
  }
  cairo_destroy(cr);
  cairo_surface_destroy(surface);
  g_object_unref(handle);
  return ok;
}
