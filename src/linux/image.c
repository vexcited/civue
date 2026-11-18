#include "../internal.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

// an image no window could show whole, refused rather than decoded.
constexpr u64 DECODED_PIXELS_MAX = 64u << 20;

typedef struct {
  u32 max_pixels;
  bool refused;
} Sizing;

// the size is known before the pixels are made. an image too large is
// refused there, a size of zero stops a loader before it makes them. one
// larger than asked for is decoded smaller where the loader can, and
// turning it later keeps its longest side.
static void on_size(GdkPixbufLoader *loader, int width, int height,
                    gpointer arg) {
  Sizing *s = arg;
  u64 w = (u64)MAX(width, 1), h = (u64)MAX(height, 1);
  u64 longest = MAX(w, h);

  if (w * h > DECODED_PIXELS_MAX) {
    s->refused = true;
    gdk_pixbuf_loader_set_size(loader, 0, 0);
  }
  else if (s->max_pixels != 0 && longest > s->max_pixels) {
    gdk_pixbuf_loader_set_size(loader,
                               (int)MAX(w * s->max_pixels / longest, 1),
                               (int)MAX(h * s->max_pixels / longest, 1));
  }
}

// the bytes go in by parts, so that a refused image stops being read.
constexpr usize PART = 64u << 10;

// what gdk-pixbuf has a loader for, webp being one to install on most
// systems.
u8 *civue__image_decode(const u8 *bytes, usize n, u32 max_pixels, u32 *width,
                        u32 *height) {
  GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
  Sizing sizing = {.max_pixels = max_pixels};
  bool ok = true;

  g_signal_connect(loader, "size-prepared", G_CALLBACK(on_size), &sizing);

  for (usize at = 0; ok && at < n; at += PART) {
    ok = gdk_pixbuf_loader_write(loader, bytes + at, MIN(n - at, PART),
                                 nullptr) &&
         !sizing.refused;
  }

  // closed even when it failed, a loader left open warns when freed.
  ok = gdk_pixbuf_loader_close(loader, nullptr) && ok && !sizing.refused;

  GdkPixbuf *loaded = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;

  if (loaded) {
    g_object_ref(loaded);
  }

  g_object_unref(loader);

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
