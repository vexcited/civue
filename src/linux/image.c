#include "../internal.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <math.h>
#include <stddefer.h>

#ifdef CIVUE_LCMS
#include <lcms2.h>
#endif

#ifdef CIVUE_PNG
#include <png.h>
#include <setjmp.h>
#endif

#ifdef CIVUE_JPEG
#include <turbojpeg.h>
#endif

#ifdef CIVUE_WEBP
#include <webp/decode.h>
#include <webp/demux.h>
#endif

// an image no window could show whole, refused rather than decoded.
constexpr u64 DECODED_PIXELS_MAX = 64u << 20;

/* ========================================================================== */
/* sizes. png, jpeg, webp and gif are decoded here, by the libraries browsers */
/* read them with: gdk-pixbuf hands each image to a process of its own, which */
/* takes longer to start than an avatar takes to decode.                      */
/* ========================================================================== */

// the size an image is given: its longest side `max_pixels` at most.
static void fit(u32 w, u32 h, u32 max_pixels, u32 *tw, u32 *th) {
  u32 longest = MAX(w, h);

  *tw = w, *th = h;

  if (max_pixels != 0 && longest > max_pixels) {
    *tw = MAX((u32)((u64)w * max_pixels / longest), 1);
    *th = MAX((u32)((u64)h * max_pixels / longest), 1);
  }
}

// x over 255, rounded, for x up to 255 * 255.
static u32 div255(u32 x) {
  x += 128;
  return (x + (x >> 8)) >> 8;
}

static void premultiply(u8 *rgba, usize pixels) {
  for (usize i = 0; i < pixels; i++) {
    u8 *p = rgba + i * 4;
    u32 a = p[3];

    if (a == 255) {
      continue;
    }

    p[0] = (u8)div255(p[0] * a);
    p[1] = (u8)div255(p[1] * a);
    p[2] = (u8)div255(p[2] * a);
  }
}

// premultiplied pixels made half as wide and as tall where they are, each
// the mean of four, two channels to a word.
static void halve(u8 *px, u32 w, u32 h) {
  u32 hw = w / 2, hh = h / 2;
  constexpr u32 EVEN = 0x00ff00ff;

  for (u32 y = 0; y < hh; y++) {
    const u8 *top = px + (usize)(2 * y) * w * 4;
    const u8 *bottom = top + (usize)w * 4;
    u8 *to = px + (usize)y * hw * 4;

    for (u32 x = 0; x < hw; x++) {
      u32 p[4];

      memcpy(&p[0], top + x * 8, 4);
      memcpy(&p[1], top + x * 8 + 4, 4);
      memcpy(&p[2], bottom + x * 8, 4);
      memcpy(&p[3], bottom + x * 8 + 4, 4);

      u32 low = (p[0] & EVEN) + (p[1] & EVEN) + (p[2] & EVEN) + (p[3] & EVEN) +
                0x00020002;
      u32 high = ((p[0] >> 8) & EVEN) + ((p[1] >> 8) & EVEN) +
                 ((p[2] >> 8) & EVEN) + ((p[3] >> 8) & EVEN) + 0x00020002;
      u32 mean = ((low >> 2) & EVEN) | (((high >> 2) & EVEN) << 8);

      memcpy(to + x * 4, &mean, 4);
    }
  }
}

// `src` made `dw` by `dh` in `dst`, never larger than it was: halved while
// it is twice too large, then each pixel read between its four nearest.
// `src` is written over.
static bool scale_into(u8 *src, u32 sw, u32 sh, u8 *dst, u32 dw, u32 dh) {
  while (sw >= dw * 2 && sh >= dh * 2) {
    halve(src, sw, sh);
    sw /= 2, sh /= 2;
  }

  if (sw == dw && sh == dh) {
    memcpy(dst, src, (usize)dw * dh * 4);
    return true;
  }

  // where each column reads from, and how much of the texel after.
  struct {
    u32 at, next, weight;
  } *across = malloc(dw * sizeof *across);

  if (!across) {
    return false;
  }

  for (u32 x = 0; x < dw; x++) {
    i64 p = (((i64)(2 * x + 1) * sw) << 16) / (2 * (i64)dw) - 32768;

    p = MAX(p, 0);
    across[x].at = (u32)(p >> 16);
    across[x].next = MIN(across[x].at + 1, sw - 1);
    across[x].weight = (u32)(p & 0xffff) >> 8;
  }

  for (u32 y = 0; y < dh; y++) {
    i64 p = (((i64)(2 * y + 1) * sh) << 16) / (2 * (i64)dh) - 32768;

    p = MAX(p, 0);

    u32 y0 = (u32)(p >> 16), y1 = MIN(y0 + 1, sh - 1);
    u32 wy = (u32)(p & 0xffff) >> 8;
    const u8 *top = src + (usize)y0 * sw * 4, *bottom = src + (usize)y1 * sw * 4;
    u8 *to = dst + (usize)y * dw * 4;

    for (u32 x = 0; x < dw; x++, to += 4) {
      const u8 *a = top + across[x].at * 4, *b = top + across[x].next * 4;
      const u8 *c = bottom + across[x].at * 4, *d = bottom + across[x].next * 4;
      u32 wx = across[x].weight;

      for (u32 k = 0; k < 4; k++) {
        u32 over = a[k] * (256 - wx) + b[k] * wx;
        u32 under = c[k] * (256 - wx) + d[k] * wx;

        to[k] = (u8)((over * (256 - wy) + under * wy + 32768) >> 16);
      }
    }
  }

  free(across);
  return true;
}

// pixels of `w` by `h` handed over, given back `tw` by `th`.
static u8 *sized(u8 *rgba, u32 w, u32 h, u32 tw, u32 th) {
  if (!rgba || (w == tw && h == th)) {
    return rgba;
  }

  u8 *out = malloc((usize)tw * th * 4);

  if (!out || !scale_into(rgba, w, h, out, tw, th)) {
    free(out);
    out = nullptr;
  }

  free(rgba);
  return out;
}

static bool starts(const u8 *bytes, usize n, const char *magic, usize len) {
  return n >= len && !memcmp(bytes, magic, len);
}

static bool is_png(const u8 *b, usize n) {
  return starts(b, n, "\x89PNG\r\n\x1a\n", 8);
}

static bool is_jpeg(const u8 *b, usize n) {
  return starts(b, n, "\xff\xd8\xff", 3);
}

static bool is_gif(const u8 *b, usize n) {
  return starts(b, n, "GIF87a", 6) || starts(b, n, "GIF89a", 6);
}

static bool is_webp(const u8 *b, usize n) {
  return n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4);
}

/* ========================================================================== */
/* colors. an image that carries a profile is in that profile's colors, a     */
/* phone's photo in display p3 for one, and is shown in srgb.                 */
/* ========================================================================== */

// a profile larger than this is no profile.
constexpr usize PROFILE_MAX = 16u << 20;

// unpremultiplied pixels moved from `profile`'s colors to srgb, false when
// that cannot be done here and another decoder is to be asked.
static bool to_srgb(u8 *rgba, usize pixels, const u8 *profile, usize len) {
#ifdef CIVUE_LCMS
  cmsHPROFILE in = cmsOpenProfileFromMem(profile, (cmsUInt32Number)len);

  // a profile that cannot be read leaves the colors as they are.
  if (!in) {
    return true;
  }

  defer cmsCloseProfile(in);

  char name[128] = "";

  cmsGetProfileInfoASCII(in, cmsInfoDescription, "en", "US", name,
                         sizeof name);

  // most profiles say srgb, which the pixels are shown in already.
  if (cmsGetColorSpace(in) != cmsSigRgbData || strstr(name, "sRGB")) {
    return true;
  }

  cmsHPROFILE out = cmsCreate_sRGBProfile();

  if (!out) {
    return true;
  }

  defer cmsCloseProfile(out);

  cmsHTRANSFORM t = cmsCreateTransform(in, TYPE_RGBA_8, out, TYPE_RGBA_8,
                                       INTENT_PERCEPTUAL, cmsFLAGS_COPY_ALPHA);

  if (t) {
    cmsDoTransform(t, rgba, rgba, (cmsUInt32Number)pixels);
    cmsDeleteTransform(t);
  }

  return true;
#else
  (void)rgba, (void)pixels, (void)profile, (void)len;
  return false;
#endif
}

/* ========================================================================== */
/* png.                                                                       */
/* ========================================================================== */

#ifdef CIVUE_PNG
typedef struct {
  const u8 *b;
  usize n, at;
} PngBytes;

static void png_take(png_structp png, png_bytep out, png_size_t len) {
  PngBytes *from = png_get_io_ptr(png);

  if (len > from->n - from->at) {
    png_error(png, "the file ends early");
  }

  memcpy(out, from->b + from->at, len);
  from->at += len;
}

// libpng says what is wrong on stderr unless told otherwise.
static void png_failed(png_structp png, png_const_charp) {
  png_longjmp(png, 1);
}

static void png_warned(png_structp, png_const_charp) {}

// read row by row, the way every other reader of png goes: libpng's one call
// reader has given a 16 bit interlaced image the wrong pixels.
static u8 *decode_png(const u8 *bytes, usize n, u32 *w, u32 *h,
                      bool *other) {
  PngBytes from = {bytes, n, 0};
  // what a failure jumps back with is read after the jump.
  u8 *volatile rgba = nullptr;
  png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr,
                                           png_failed, png_warned);
  png_infop info = png ? png_create_info_struct(png) : nullptr;

  if (!info || setjmp(png_jmpbuf(png))) {
    png_destroy_read_struct(&png, &info, nullptr);
    free(rgba);
    return nullptr;
  }

  png_set_read_fn(png, &from, png_take);
  png_read_info(png, info);

  u32 width = png_get_image_width(png, info);
  u32 height = png_get_image_height(png, info);
  u32 type = png_get_color_type(png, info);
  bool alpha =
    type & PNG_COLOR_MASK_ALPHA || png_get_valid(png, info, PNG_INFO_tRNS);
  double gamma;

  if (!width || !height || (u64)width * height > DECODED_PIXELS_MAX) {
    png_error(png, "too large");
  }

  // eight bits of red, green, blue and alpha, in srgb. a file that says
  // nothing of its gamma is srgb already, as browsers read it.
  png_set_expand(png);
  png_set_scale_16(png);
  png_set_gray_to_rgb(png);
  png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);

  if (!png_get_valid(png, info, PNG_INFO_sRGB) &&
      png_get_gAMA(png, info, &gamma)) {
    png_set_gamma(png, PNG_DEFAULT_sRGB, gamma);
  }

  int passes = png_set_interlace_handling(png);

  png_read_update_info(png, info);

  if (png_get_rowbytes(png, info) != (usize)width * 4) {
    png_error(png, "rows of another size");
  }

  rgba = malloc((usize)width * height * 4);

  if (!rgba) {
    png_error(png, "no memory");
  }

  for (int pass = 0; pass < passes; pass++) {
    for (u32 y = 0; y < height; y++) {
      png_read_row(png, rgba + (usize)y * width * 4, nullptr);
    }
  }

  png_charp name;
  png_bytep profile;
  png_uint_32 len;
  int packed;
  u8 *px = rgba;

  if (png_get_iCCP(png, info, &name, &packed, &profile, &len) &&
      len <= PROFILE_MAX &&
      !to_srgb(px, (usize)width * height, profile, len)) {
    *other = true;
    png_error(png, "a profile for another reader");
  }

  png_destroy_read_struct(&png, &info, nullptr);

  if (alpha) {
    premultiply(px, (usize)width * height);
  }

  *w = width, *h = height;
  return px;
}
#endif

/* ========================================================================== */
/* jpeg.                                                                      */
/* ========================================================================== */

#ifdef CIVUE_JPEG
static u32 be16(const u8 *p) {
  return (u32)p[0] << 8 | p[1];
}

// how a jpeg's exif says it is turned, 1 for as it is.
static u32 orientation(const u8 *b, usize n) {
  usize at = 2;

  while (at + 4 <= n && b[at] == 0xff) {
    u32 marker = b[at + 1], len = be16(b + at + 2);

    // the pixels start: what follows is no header.
    if (marker == 0xda || len < 2 || at + 2 + len > n) {
      break;
    }

    const u8 *seg = b + at + 4;
    usize size = len - 2;

    at += 2 + len;

    if (marker != 0xe1 || size < 14 || memcmp(seg, "Exif\0\0", 6)) {
      continue;
    }

    const u8 *tiff = seg + 6;
    usize left = size - 6;
    bool little = tiff[0] == 'I';
#define U16(p) \
  (little ? (u32)(p)[0] | (u32)(p)[1] << 8 : (u32)(p)[0] << 8 | (p)[1])
#define U32(p) \
  (little ? U16(p) | U16((p) + 2) << 16 : U16(p) << 16 | U16((p) + 2))
    usize ifd = U32(tiff + 4);

    if (ifd + 2 > left) {
      return 1;
    }

    u32 count = U16(tiff + ifd);

    for (u32 i = 0; i < count && ifd + 2 + (i + 1) * 12 <= left; i++) {
      const u8 *e = tiff + ifd + 2 + i * 12;

      if (U16(e) == 0x0112) {
        u32 v = U16(e + 8);
        return v >= 1 && v <= 8 ? v : 1;
      }
    }
#undef U16
#undef U32

    return 1;
  }

  return 1;
}

// the profile a jpeg carries, in parts that say which they are. false
// without one.
static bool jpeg_profile(const u8 *b, usize n, civue_bytes *out) {
  out->len = 0;

  for (u32 want = 1; want < 256; want++) {
    usize at = 2;
    bool found = false;
    u32 parts = 0;

    while (at + 4 <= n && b[at] == 0xff && !found) {
      u32 marker = b[at + 1], len = be16(b + at + 2);

      if (marker == 0xda || len < 2 || at + 2 + len > n) {
        break;
      }

      const u8 *seg = b + at + 4;
      usize size = len - 2;

      at += 2 + len;

      if (marker == 0xe2 && size > 14 && !memcmp(seg, "ICC_PROFILE\0", 12) &&
          seg[12] == want) {
        parts = seg[13];
        buf_put(out, seg + 14, size - 14);
        found = true;
      }
    }

    if (!found) {
      return false;
    }

    if (want >= parts) {
      return out->len > 0 && out->len <= PROFILE_MAX;
    }
  }

  return false;
}

// pixels handed over, given back turned as `o` says.
static u8 *oriented(u8 *rgba, u32 *w, u32 *h, u32 o) {
  if (o <= 1 || !rgba) {
    return rgba;
  }

  u32 sw = *w, sh = *h;
  bool swap = o >= 5;
  u32 dw = swap ? sh : sw, dh = swap ? sw : sh;
  u8 *out = malloc((usize)dw * dh * 4);

  if (!out) {
    free(rgba);
    return nullptr;
  }

  for (u32 y = 0; y < dh; y++) {
    for (u32 x = 0; x < dw; x++) {
      u32 sx = x, sy = y;

      switch (o) {
        case 2:
          sx = sw - 1 - x;
          break;
        case 3:
          sx = sw - 1 - x, sy = sh - 1 - y;
          break;
        case 4:
          sy = sh - 1 - y;
          break;
        case 5:
          sx = y, sy = x;
          break;
        case 6:
          sx = y, sy = sh - 1 - x;
          break;
        case 7:
          sx = sw - 1 - y, sy = sh - 1 - x;
          break;
        default:
          sx = sw - 1 - y, sy = x;
          break;
      }

      memcpy(out + ((usize)y * dw + x) * 4, rgba + ((usize)sy * sw + sx) * 4,
             4);
    }
  }

  free(rgba);
  *w = dw, *h = dh;
  return out;
}

static u8 *decode_jpeg(const u8 *bytes, usize n, u32 max_pixels, u32 *w,
                       u32 *h, bool *other) {
  tjhandle tj = tj3Init(TJINIT_DECOMPRESS);

  if (!tj) {
    return nullptr;
  }

  defer tj3Destroy(tj);

  if (tj3DecompressHeader(tj, bytes, n) != 0) {
    return nullptr;
  }

  int jw = tj3Get(tj, TJPARAM_JPEGWIDTH), jh = tj3Get(tj, TJPARAM_JPEGHEIGHT);
  int space = tj3Get(tj, TJPARAM_COLORSPACE);

  if (jw <= 0 || jh <= 0 || (u64)jw * (u64)jh > DECODED_PIXELS_MAX) {
    return nullptr;
  }

  // a print's four inks are not made red, green and blue here.
  if (space == TJCS_CMYK || space == TJCS_YCCK) {
    *other = true;
    return nullptr;
  }

  // the decoder makes it an eighth of its size and up by itself, reading
  // that much less of the file: the smallest that is still large enough.
  tjscalingfactor best = TJUNSCALED;
  int longest = MAX(jw, jh), count = 0;
  const tjscalingfactor *factors = tj3GetScalingFactors(&count);

  for (int i = 0; max_pixels != 0 && factors && i < count; i++) {
    int made = TJSCALED(longest, factors[i]);

    if (made >= (int)max_pixels && made < TJSCALED(longest, best)) {
      best = factors[i];
    }
  }

  if (tj3SetScalingFactor(tj, best) != 0) {
    best = TJUNSCALED;
  }

  int sw = TJSCALED(jw, best), sh = TJSCALED(jh, best);
  u8 *rgba = malloc((usize)sw * (usize)sh * 4);

  if (!rgba) {
    return nullptr;
  }

  // a file cut short still shows what it has.
  if (tj3Decompress8(tj, bytes, n, rgba, sw * 4, TJPF_RGBA) != 0 &&
      tj3GetErrorCode(tj) != TJERR_WARNING) {
    free(rgba);
    return nullptr;
  }

  civue_bytes profile = {};
  defer vec_free(&profile);

  if (jpeg_profile(bytes, n, &profile) &&
      !to_srgb(rgba, (usize)sw * (usize)sh, profile.v, profile.len)) {
    *other = true;
    free(rgba);
    return nullptr;
  }

  *w = (u32)sw, *h = (u32)sh;
  return rgba;
}
#endif

/* ========================================================================== */
/* webp.                                                                      */
/* ========================================================================== */

#ifdef CIVUE_WEBP
// the first frame of an animated one, whole.
static u8 *webp_first(const u8 *bytes, usize n, u32 *w, u32 *h) {
  WebPAnimDecoderOptions options;

  if (!WebPAnimDecoderOptionsInit(&options)) {
    return nullptr;
  }

  options.color_mode = MODE_rgbA;

  WebPData data = {bytes, n};
  WebPAnimDecoder *dec = WebPAnimDecoderNew(&data, &options);

  if (!dec) {
    return nullptr;
  }

  defer WebPAnimDecoderDelete(dec);

  WebPAnimInfo info;
  u8 *canvas;
  int stamp;

  if (!WebPAnimDecoderGetInfo(dec, &info) ||
      (u64)info.canvas_width * info.canvas_height > DECODED_PIXELS_MAX ||
      !WebPAnimDecoderGetNext(dec, &canvas, &stamp)) {
    return nullptr;
  }

  usize size = (usize)info.canvas_width * info.canvas_height * 4;
  u8 *rgba = malloc(size);

  if (rgba) {
    memcpy(rgba, canvas, size);
    *w = info.canvas_width, *h = info.canvas_height;
  }

  return rgba;
}

// decoded at the size asked for, never at its own then scaled.
static u8 *decode_webp(const u8 *bytes, usize n, u32 max_pixels, u32 *w,
                       u32 *h, bool *other) {
  WebPDecoderConfig config;

  if (!WebPInitDecoderConfig(&config) ||
      WebPGetFeatures(bytes, n, &config.input) != VP8_STATUS_OK) {
    return nullptr;
  }

  if (config.input.has_animation) {
    return webp_first(bytes, n, w, h);
  }

  u32 iw = (u32)config.input.width, ih = (u32)config.input.height, tw, th;

  if (!iw || !ih || (u64)iw * ih > DECODED_PIXELS_MAX) {
    return nullptr;
  }

  fit(iw, ih, max_pixels, &tw, &th);

  u8 *rgba = malloc((usize)tw * th * 4);

  if (!rgba) {
    return nullptr;
  }

  // the extended header says whether a profile comes with it. its colors
  // are moved before they are multiplied by their alpha.
  bool profiled = n > 21 && !memcmp(bytes + 12, "VP8X", 4) && bytes[20] & 0x20;

  config.output.colorspace = profiled ? MODE_RGBA : MODE_rgbA;
  config.output.is_external_memory = 1;
  config.output.u.RGBA.rgba = rgba;
  config.output.u.RGBA.stride = (int)tw * 4;
  config.output.u.RGBA.size = (usize)tw * th * 4;
  config.options.use_scaling = tw != iw || th != ih;
  config.options.scaled_width = (int)tw;
  config.options.scaled_height = (int)th;

  if (WebPDecode(bytes, n, &config) != VP8_STATUS_OK) {
    free(rgba);
    return nullptr;
  }

  if (profiled) {
    WebPData data = {bytes, n};
    WebPDemuxer *demux = WebPDemux(&data);
    WebPChunkIterator chunk;
    bool ok = true;

    if (demux && WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
      ok = chunk.chunk.size > PROFILE_MAX ||
           to_srgb(rgba, (usize)tw * th, chunk.chunk.bytes, chunk.chunk.size);
      WebPDemuxReleaseChunkIterator(&chunk);
    }

    WebPDemuxDelete(demux);

    if (!ok) {
      *other = true;
      free(rgba);
      return nullptr;
    }

    premultiply(rgba, (usize)tw * th);
  }

  *w = tw, *h = th;
  return rgba;
}
#endif

/* ========================================================================== */
/* gif. its frames are drawn over one another on a canvas, each from a table  */
/* of colors.                                                                 */
/* ========================================================================== */

typedef struct {
  const u8 *b;
  usize n, at;
  u32 width, height;
  // the colors of every frame without its own, red first.
  u8 global[256 * 3];
  // premultiplied rgba, what is shown now.
  u8 *canvas;
  // the canvas before a frame that is taken away once shown.
  u8 *before;
  // what the frame read last asked for.
  u32 dispose, x, y, w, h;
  bool drawn;
  // its codes, out of their blocks, and its pixels as indexes.
  civue_bytes codes, indexes;
} Gif;

static bool gif_open(Gif *g, const u8 *b, usize n) {
  if (!is_gif(b, n) || n < 13) {
    return false;
  }

  *g = (Gif){.b = b, .n = n, .at = 13};
  g->width = (u32)b[6] | (u32)b[7] << 8;
  g->height = (u32)b[8] | (u32)b[9] << 8;

  if (!g->width || !g->height ||
      (u64)g->width * g->height > DECODED_PIXELS_MAX) {
    return false;
  }

  if (b[10] & 0x80) {
    usize size = (usize)3 << ((b[10] & 7) + 1);

    if (g->at + size > n) {
      return false;
    }

    memcpy(g->global, b + g->at, size);
    g->at += size;
  }

  return true;
}

static void gif_close(Gif *g) {
  free(g->canvas);
  free(g->before);
  vec_free(&g->codes);
  vec_free(&g->indexes);
}

// past the blocks of data at `at`, gathered in `out` when given. false when
// the file ends first.
static bool gif_blocks(Gif *g, civue_bytes *out) {
  for (;;) {
    if (g->at >= g->n) {
      return false;
    }

    usize size = g->b[g->at++];

    if (!size) {
      return true;
    }

    if (g->at + size > g->n) {
      return false;
    }

    if (out) {
      buf_put(out, g->b + g->at, size);
    }

    g->at += size;
  }
}

// lzw: codes of a growing width, each a string of indexes that is one
// before it and a last index.
static bool gif_unpack(const u8 *codes, usize n, u32 min, u8 *out,
                       usize pixels) {
  constexpr u32 CODES = 4096;
  // zeroed, so that a code the file never made ends at the first color.
  u16 prefix[CODES] = {};
  u8 suffix[CODES] = {}, stack[CODES + 1];

  if (min < 2 || min > 8) {
    return false;
  }

  u32 clear = 1u << min, end = clear + 1, avail = clear + 2;
  u32 width = min + 1, mask = (1u << width) - 1;
  u32 bits = 0, held = 0, first = 0, top = 0;
  i32 old = -1;
  usize at = 0, made = 0;

  for (u32 i = 0; i < clear; i++) {
    suffix[i] = (u8)i;
  }

  while (made < pixels) {
    while (held < width && at < n) {
      bits |= (u32)codes[at++] << held;
      held += 8;
    }

    if (held < width) {
      break;
    }

    u32 code = bits & mask;

    bits >>= width;
    held -= width;

    if (code == clear) {
      avail = clear + 2;
      width = min + 1;
      mask = (1u << width) - 1;
      old = -1;
      continue;
    }

    if (code == end) {
      break;
    }

    if (old < 0) {
      if (code >= clear) {
        break;
      }

      out[made++] = (u8)code;
      first = code;
      old = (i32)code;
      continue;
    }

    u32 in = code;

    if (code >= avail) {
      // the string being made is the one asked for.
      if (code > avail) {
        break;
      }

      stack[top++] = (u8)first;
      code = (u32)old;
    }

    while (code >= clear && top < CODES) {
      stack[top++] = suffix[code];
      code = prefix[code];
    }

    first = suffix[code];
    stack[top++] = (u8)first;

    if (avail < CODES) {
      prefix[avail] = (u16)old;
      suffix[avail] = (u8)first;
      avail++;

      if ((avail & mask) == 0 && avail < CODES) {
        width++;
        mask = (1u << width) - 1;
      }
    }

    old = (i32)in;

    while (top && made < pixels) {
      out[made++] = stack[--top];
    }

    top = 0;
  }

  // a file cut short leaves its first color.
  memset(out + made, 0, pixels - made);
  return true;
}

// the frame after the one drawn last, on the canvas. `delay` is how long it
// shows, in seconds. false when there is none, or when `draw` is false and
// the frames are only counted.
static bool gif_next(Gif *g, bool draw, f32 *delay) {
  u32 dispose = 0, hundredths = 0;
  i32 clear = -1;

  for (;;) {
    if (g->at >= g->n) {
      return false;
    }

    u8 kind = g->b[g->at++];

    if (kind == 0x3b) {
      return false;
    }

    if (kind == 0x21) {
      if (g->at >= g->n) {
        return false;
      }

      u8 label = g->b[g->at++];

      if (label == 0xf9 && g->at + 5 <= g->n && g->b[g->at] == 4) {
        const u8 *e = g->b + g->at + 1;

        dispose = (e[0] >> 2) & 7;
        hundredths = (u32)e[1] | (u32)e[2] << 8;
        clear = e[0] & 1 ? (i32)e[3] : -1;
      }

      if (!gif_blocks(g, nullptr)) {
        return false;
      }

      continue;
    }

    if (kind != 0x2c) {
      return false;
    }

    break;
  }

  if (g->at + 9 > g->n) {
    return false;
  }

  const u8 *d = g->b + g->at;
  u32 x = (u32)d[0] | (u32)d[1] << 8, y = (u32)d[2] | (u32)d[3] << 8;
  u32 w = (u32)d[4] | (u32)d[5] << 8, h = (u32)d[6] | (u32)d[7] << 8;
  u8 packed = d[8];
  const u8 *colors = g->global;

  g->at += 9;

  if (packed & 0x80) {
    usize size = (usize)3 << ((packed & 7) + 1);

    if (g->at + size > g->n) {
      return false;
    }

    colors = g->b + g->at;
    g->at += size;
  }

  if (g->at >= g->n) {
    return false;
  }

  u32 min = g->b[g->at++];

  // browsers show a frame that asks for 10 ms or less for 100 ms, and so do
  // the gifs made for them.
  *delay = hundredths <= 1 ? 0.1f : (f32)hundredths / 100.0f;

  if (!draw) {
    return gif_blocks(g, nullptr);
  }

  g->codes.len = 0;

  if (!gif_blocks(g, &g->codes)) {
    return false;
  }

  usize size = (usize)g->width * g->height * 4;

  if (!g->canvas) {
    g->canvas = calloc(1, size);

    if (!g->canvas) {
      return false;
    }
  }

  // what the frame before asked for once it was shown.
  if (g->drawn && g->dispose == 2) {
    for (u32 row = g->y; row < MIN(g->y + g->h, g->height); row++) {
      u32 from = MIN(g->x, g->width), to = MIN(g->x + g->w, g->width);

      memset(g->canvas + ((usize)row * g->width + from) * 4, 0,
             (usize)(to - from) * 4);
    }
  }
  else if (g->drawn && g->dispose == 3 && g->before) {
    memcpy(g->canvas, g->before, size);
  }

  if (dispose == 3) {
    if (!g->before) {
      g->before = malloc(size);
    }

    if (g->before) {
      memcpy(g->before, g->canvas, size);
    }
  }

  usize pixels = (usize)w * h;

  if (pixels > DECODED_PIXELS_MAX) {
    return false;
  }

  g->indexes.len = 0;
  vec_reserve(&g->indexes, pixels + 1);

  if (pixels &&
      !gif_unpack(g->codes.v, g->codes.len, min, g->indexes.v, pixels)) {
    return false;
  }

  // the colors of a table of its own may be fewer than its indexes go to.
  usize has = colors == g->global ? 256
                                  : (usize)1 << ((packed & 7) + 1);
  bool interlaced = packed & 0x40;
  const u32 starts[4] = {0, 4, 2, 1}, steps[4] = {8, 8, 4, 2};
  u32 pass = 0, row = 0;

  for (u32 i = 0; i < h; i++) {
    u32 line = i;

    if (interlaced) {
      while (pass < 4 && starts[pass] + row * steps[pass] >= h) {
        pass++;
        row = 0;
      }

      line = pass < 4 ? starts[pass] + row * steps[pass] : i;
      row++;
    }

    if (y + line >= g->height) {
      continue;
    }

    const u8 *from = g->indexes.v + (usize)i * w;
    u8 *to = g->canvas + ((usize)(y + line) * g->width + x) * 4;

    for (u32 k = 0; k < w && x + k < g->width; k++, to += 4) {
      u32 index = from[k];

      if ((i32)index == clear) {
        continue;
      }

      const u8 *c = colors + (index < has ? index : 0) * 3;

      to[0] = c[0], to[1] = c[1], to[2] = c[2], to[3] = 255;
    }
  }

  g->dispose = dispose;
  g->x = x, g->y = y, g->w = w, g->h = h;
  g->drawn = true;
  return true;
}

static u8 *decode_gif(const u8 *bytes, usize n, u32 *w, u32 *h) {
  Gif g;
  f32 delay;

  if (!gif_open(&g, bytes, n)) {
    return nullptr;
  }

  defer gif_close(&g);

  if (!gif_next(&g, true, &delay)) {
    return nullptr;
  }

  u8 *rgba = g.canvas;

  g.canvas = nullptr;
  *w = g.width, *h = g.height;
  return rgba;
}

/* ========================================================================== */
/* the rest, by gdk-pixbuf.                                                   */
/* ========================================================================== */

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

static u8 *decode_other(const u8 *bytes, usize n, u32 max_pixels, u32 *width,
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
  u8 *rgba = nullptr;

  if (gdk_pixbuf_get_bits_per_sample(pix) == 8 && w && h &&
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

        to[0] = (u8)div255(from[0] * a);
        to[1] = (u8)div255(from[1] * a);
        to[2] = (u8)div255(from[2] * a);
        to[3] = (u8)a;
      }
    }

    *width = w;
    *height = h;
  }

  g_object_unref(pix);
  return rgba;
}

u8 *civue__image_decode(const u8 *bytes, usize n, u32 max_pixels, u32 *width,
                        u32 *height) {
  u8 *rgba = nullptr;
  u32 w = 0, h = 0;
  [[maybe_unused]] u32 turn = 1;
  // whether a decoder here reads the format, and leaves this file of it to
  // gdk-pixbuf all the same. a file it could not read is no image: handing
  // gdk-pixbuf what is damaged has made it abort.
  bool mine = false, other = false;

#ifdef CIVUE_PNG
  if (is_png(bytes, n)) {
    mine = true;
    rgba = decode_png(bytes, n, &w, &h, &other);
  }
#endif

#ifdef CIVUE_JPEG
  if (is_jpeg(bytes, n)) {
    mine = true;
    rgba = decode_jpeg(bytes, n, max_pixels, &w, &h, &other);
    turn = rgba ? orientation(bytes, n) : 1;
  }
#endif

#ifdef CIVUE_WEBP
  if (is_webp(bytes, n)) {
    mine = true;
    rgba = decode_webp(bytes, n, max_pixels, &w, &h, &other);
  }
#endif

  if (is_gif(bytes, n)) {
    mine = true;
    rgba = decode_gif(bytes, n, &w, &h);
  }

  if (!rgba && (!mine || other)) {
    rgba = decode_other(bytes, n, max_pixels, &w, &h);
  }

  if (!rgba) {
    return nullptr;
  }

  u32 tw, th;

  fit(w, h, max_pixels, &tw, &th);
  rgba = sized(rgba, w, h, tw, th);
  w = tw, h = th;
#ifdef CIVUE_JPEG
  rgba = oriented(rgba, &w, &h, turn);
#endif

  if (rgba) {
    *width = w;
    *height = h;
  }

  return rgba;
}

/* ========================================================================== */
/* frames.                                                                    */
/* ========================================================================== */

// the size every frame of `count` is given, as the other platforms size
// them. false when each would be a smudge: the first one alone is better.
static bool frames_size(u32 width, u32 height, u32 count, u32 max_pixels,
                        usize max_bytes, u32 *w, u32 *h) {
  f64 longest = MAX(width, height);
  f64 scale = max_pixels > 0 ? MIN(1.0, max_pixels / longest) : 1.0;
  f64 need = (f64)width * height * scale * scale * 4.0 * (f64)count;

  if (need > (f64)max_bytes) {
    scale *= sqrt((f64)max_bytes / need);
  }

  *w = (u32)MAX(round(width * scale), 1.0);
  *h = (u32)MAX(round(height * scale), 1.0);
  return MAX(*w, *h) >= MIN(longest, 64.0);
}

static bool gif_frames(const u8 *bytes, usize n, u32 max_pixels,
                       usize max_bytes, civue__frames *out) {
  Gif g;
  f32 delay;
  u32 count = 0;

  if (!gif_open(&g, bytes, n)) {
    return false;
  }

  while (gif_next(&g, false, &delay)) {
    count++;
  }

  gif_close(&g);

  u32 w, h;

  if (count < 2 || !gif_open(&g, bytes, n)) {
    return false;
  }

  defer gif_close(&g);

  if (!frames_size(g.width, g.height, count, max_pixels, max_bytes, &w, &h)) {
    return false;
  }

  usize frame = (usize)w * h * 4, whole = (usize)g.width * g.height * 4;
  u8 *rgba = calloc(count, frame);
  f32 *delays = calloc(count, sizeof(f32));
  u8 *scratch = malloc(whole);
  u32 made = 0;
  bool ok = rgba && delays && scratch;

  while (ok && made < count && gif_next(&g, true, &delays[made])) {
    // scaling writes over what it reads, the canvas is the next frame's.
    memcpy(scratch, g.canvas, whole);
    ok = scale_into(scratch, g.width, g.height, rgba + made * frame, w, h);
    made++;
  }

  free(scratch);

  if (!ok || made < 2) {
    free(rgba);
    free(delays);
    return false;
  }

  *out = (civue__frames){rgba, delays, w, h, made};
  return true;
}

#ifdef CIVUE_WEBP
static bool webp_frames(const u8 *bytes, usize n, u32 max_pixels,
                        usize max_bytes, civue__frames *out) {
  WebPAnimDecoderOptions options;

  if (!WebPAnimDecoderOptionsInit(&options)) {
    return false;
  }

  options.color_mode = MODE_rgbA;

  WebPData data = {bytes, n};
  WebPAnimDecoder *dec = WebPAnimDecoderNew(&data, &options);

  if (!dec) {
    return false;
  }

  defer WebPAnimDecoderDelete(dec);

  WebPAnimInfo info;
  u32 w, h;

  if (!WebPAnimDecoderGetInfo(dec, &info) || info.frame_count < 2 ||
      !info.canvas_width || !info.canvas_height ||
      (u64)info.canvas_width * info.canvas_height > DECODED_PIXELS_MAX ||
      !frames_size(info.canvas_width, info.canvas_height, info.frame_count,
                   max_pixels, max_bytes, &w, &h)) {
    return false;
  }

  u32 count = info.frame_count, made = 0;
  usize frame = (usize)w * h * 4;
  usize whole = (usize)info.canvas_width * info.canvas_height * 4;
  u8 *rgba = calloc(count, frame);
  f32 *delays = calloc(count, sizeof(f32));
  u8 *scratch = malloc(whole);
  bool ok = rgba && delays && scratch;
  int last = 0;

  while (ok && made < count && WebPAnimDecoderHasMoreFrames(dec)) {
    u8 *canvas;
    int stamp;

    if (!WebPAnimDecoderGetNext(dec, &canvas, &stamp)) {
      break;
    }

    f32 delay = (f32)(stamp - last) / 1000.0f;

    last = stamp;
    delays[made] = delay <= 0.01f ? 0.1f : delay;
    memcpy(scratch, canvas, whole);
    ok = scale_into(scratch, info.canvas_width, info.canvas_height,
                    rgba + made * frame, w, h);
    made++;
  }

  free(scratch);

  if (!ok || made < 2) {
    free(rgba);
    free(delays);
    return false;
  }

  *out = (civue__frames){rgba, delays, w, h, made};
  return true;
}
#endif

// the frames of gifs and webps. an animated png shows its first one still.
bool civue__image_decode_frames(const u8 *bytes, usize n, u32 max_pixels,
                                usize max_bytes, civue__frames *out) {
  if (is_gif(bytes, n)) {
    return gif_frames(bytes, n, max_pixels, max_bytes, out);
  }

#ifdef CIVUE_WEBP
  if (is_webp(bytes, n)) {
    return webp_frames(bytes, n, max_pixels, max_bytes, out);
  }
#endif

  return false;
}
