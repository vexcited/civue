#include "../internal.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

// an image no window could show whole, refused rather than decoded.
constexpr u64 DECODED_PIXELS_MAX = 64u << 20;

// what gdk-pixbuf has a loader for, webp being one to install on most
// systems.
u8 *civue__image_decode(const u8 *bytes, usize n, u32 max_pixels, u32 *width,
                        u32 *height) {
  GInputStream *in =
    g_memory_input_stream_new_from_data(bytes, (gssize)n, nullptr);
  GdkPixbuf *loaded = gdk_pixbuf_new_from_stream(in, nullptr, nullptr);

  g_object_unref(in);

  if (!loaded) {
    return nullptr;
  }

  GdkPixbuf *pix = gdk_pixbuf_apply_embedded_orientation(loaded);

  g_object_unref(loaded);

  if (!pix) {
    return nullptr;
  }

  u32 w = (u32)gdk_pixbuf_get_width(pix), h = (u32)gdk_pixbuf_get_height(pix);
  u32 longest = MAX(w, h);

  if (max_pixels != 0 && longest > max_pixels) {
    u32 sw = MAX((u32)((u64)w * max_pixels / longest), 1);
    u32 sh = MAX((u32)((u64)h * max_pixels / longest), 1);
    GdkPixbuf *scaled =
      gdk_pixbuf_scale_simple(pix, (int)sw, (int)sh, GDK_INTERP_BILINEAR);

    g_object_unref(pix);
    pix = scaled;
    w = sw;
    h = sh;
  }

  u8 *rgba = nullptr;

  if (pix && gdk_pixbuf_get_bits_per_sample(pix) == 8 &&
      (u64)w * h <= DECODED_PIXELS_MAX) {
    rgba = malloc((usize)w * h * 4);
  }

  if (rgba) {
    const u8 *pixels = gdk_pixbuf_read_pixels(pix);
    usize stride = (usize)gdk_pixbuf_get_rowstride(pix);
    u32 channels = (u32)gdk_pixbuf_get_n_channels(pix);

    for (u32 y = 0; y < h; y++) {
      const u8 *from = pixels + y * stride;
      u8 *to = rgba + (usize)y * w * 4;

      for (u32 x = 0; x < w; x++, from += channels, to += 4) {
        u32 a = channels == 4 ? from[3] : 255;

        to[0] = (u8)((from[0] * a + 127) / 255);
        to[1] = (u8)((from[1] * a + 127) / 255);
        to[2] = (u8)((from[2] * a + 127) / 255);
        to[3] = (u8)a;
      }
    }

    *width = w;
    *height = h;
  }

  if (pix) {
    g_object_unref(pix);
  }

  return rgba;
}
