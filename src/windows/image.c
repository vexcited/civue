#include "win.h"

#include <math.h>
#include <stddefer.h>
#include <wincodec.h>

#ifdef CIVUE_WEBP
#include <webp/decode.h>
#include <webp/demux.h>
#endif

// an image no window could show whole, refused rather than decoded.
constexpr u64 DECODED_PIXELS_MAX = 64u << 20;
// a profile larger than this is no profile.
constexpr usize PROFILE_MAX = 16u << 20;
constexpr u32 PNG_FRAMES_MAX = 4096;

/* ========================================================================== */
/* wic decodes what windows has a codec for: png, jpeg, gif, bmp, tiff, ico   */
/* and the extensions' heif, avif and webp. it shows the frames of a gif one  */
/* by one and an animated png's first alone, so both are drawn on a canvas   */
/* here, and webp is libwebp's where the build has it.                        */
/* ========================================================================== */

static void release(void *p) {
  if (p) {
    IUnknown_Release((IUnknown *)p);
  }
}

// wic's factory, com started on the calling thread for it.
static IWICImagingFactory *wic() {
  IWICImagingFactory *f = nullptr;

  civue__com_init();
  CoCreateInstance(&CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                   &IID_IWICImagingFactory, (void **)&f);
  return f;
}

static IWICBitmapDecoder *open_bytes(IWICImagingFactory *f, const u8 *bytes,
                                     usize n) {
  IWICStream *stream = nullptr;
  IWICBitmapDecoder *d = nullptr;

  if (n > UINT32_MAX || FAILED(IWICImagingFactory_CreateStream(f, &stream))) {
    return nullptr;
  }

  if (SUCCEEDED(
        IWICStream_InitializeFromMemory(stream, (BYTE *)bytes, (DWORD)n))) {
    IWICImagingFactory_CreateDecoderFromStream(
      f, (IStream *)stream, nullptr, WICDecodeMetadataCacheOnDemand, &d);
  }

  release(stream);
  return d;
}

// a number the frame's metadata holds at `name`, `otherwise` without it.
static u32 meta(IWICMetadataQueryReader *q, const wchar_t *name,
                u32 otherwise) {
  PROPVARIANT v;
  u32 got = otherwise;

  PropVariantInit(&v);

  if (q && SUCCEEDED(IWICMetadataQueryReader_GetMetadataByName(q, name, &v))) {
    switch (v.vt) {
      case VT_UI1:
        got = v.bVal;
        break;
      case VT_UI2:
        got = v.uiVal;
        break;
      case VT_UI4:
        got = v.ulVal;
        break;
      case VT_BOOL:
        got = v.boolVal != 0;
        break;
      default:
        break;
    }
  }

  PropVariantClear(&v);
  return got;
}

/* ========================================================================== */
/* sizes.                                                                     */
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

// premultiplied `src` of `sw` by `sh` made `dw` by `dh` in `dst`. wic's
// scaler reads premultiplied bgra as it is and gives other formats back with
// red and blue swapped, while it treats every channel alike: rgba is handed
// to it as bgra.
static bool scale(IWICImagingFactory *f, const u8 *src, u32 sw, u32 sh, u8 *dst,
                  u32 dw, u32 dh) {
  if (sw == dw && sh == dh) {
    memcpy(dst, src, (usize)dw * dh * 4);
    return true;
  }

  IWICBitmap *b = nullptr;
  IWICBitmapScaler *s = nullptr;
  defer release(b);
  defer release(s);

  return SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(
           f, sw, sh, &GUID_WICPixelFormat32bppPBGRA, sw * 4, sw * sh * 4,
           (BYTE *)src, &b)) &&
         SUCCEEDED(IWICImagingFactory_CreateBitmapScaler(f, &s)) &&
         SUCCEEDED(IWICBitmapScaler_Initialize(
           s, (IWICBitmapSource *)b, dw, dh, WICBitmapInterpolationModeFant)) &&
         SUCCEEDED(
           IWICBitmapScaler_CopyPixels(s, nullptr, dw * 4, dw * dh * 4, dst));
}

// pixels of `w` by `h` handed over, given back `tw` by `th`.
static u8 *sized(IWICImagingFactory *f, u8 *rgba, u32 w, u32 h, u32 tw,
                 u32 th) {
  if (!rgba || (w == tw && h == th)) {
    return rgba;
  }

  u8 *out = malloc((usize)tw * th * 4);

  if (out && !scale(f, rgba, w, h, out, tw, th)) {
    free(out);
    out = nullptr;
  }

  free(rgba);
  return out;
}

// mingw's wincodec.h leaves out how a codec decodes smaller by itself.
typedef struct SourceTransform SourceTransform;

typedef struct {
  HRESULT(STDMETHODCALLTYPE *QueryInterface)(SourceTransform *, REFIID,
                                             void **);
  ULONG(STDMETHODCALLTYPE *AddRef)(SourceTransform *);
  ULONG(STDMETHODCALLTYPE *Release)(SourceTransform *);
  HRESULT(STDMETHODCALLTYPE *CopyPixels)(SourceTransform *, const WICRect *,
                                         UINT, UINT, WICPixelFormatGUID *,
                                         WICBitmapTransformOptions, UINT, UINT,
                                         BYTE *);
  HRESULT(STDMETHODCALLTYPE *GetClosestSize)(SourceTransform *, UINT *, UINT *);
  HRESULT(STDMETHODCALLTYPE *GetClosestPixelFormat)(SourceTransform *,
                                                    WICPixelFormatGUID *);
} SourceTransformVtbl;

struct SourceTransform {
  const SourceTransformVtbl *lpVtbl;
};

static const IID IID_SourceTransform = {
  0x3b16811b, 0x6a43, 0x4ec9, {0xb7, 0x13, 0x3d, 0x5a, 0x0c, 0x13, 0xb9, 0x40}};

static u32 bits_per_pixel(IWICImagingFactory *f, const GUID *format) {
  IWICComponentInfo *info = nullptr;
  IWICPixelFormatInfo *pixel = nullptr;
  UINT bits = 0;

  if (SUCCEEDED(IWICImagingFactory_CreateComponentInfo(f, format, &info)) &&
      SUCCEEDED(IWICComponentInfo_QueryInterface(info, &IID_IWICPixelFormatInfo,
                                                 (void **)&pixel))) {
    IWICPixelFormatInfo_GetBitsPerPixel(pixel, &bits);
  }

  release(pixel);
  release(info);
  return bits;
}

// the frame decoded smaller by its codec, still `w` by `h` or more: a jpeg
// is by an eighth of its size and up, reading that much less of it. null
// where the codec cannot.
static IWICBitmap *reduced(IWICImagingFactory *f, IWICBitmapFrameDecode *frame,
                           u32 fw, u32 fh, u32 w, u32 h) {
  SourceTransform *t = nullptr;

  if (FAILED(IWICBitmapFrameDecode_QueryInterface(frame, &IID_SourceTransform,
                                                  (void **)&t))) {
    return nullptr;
  }

  defer release(t);

  u32 by = 8;

  while (by > 1 && ((fw + by - 1) / by < w || (fh + by - 1) / by < h)) {
    by /= 2;
  }

  // in the frame's own format, which the codec makes as it is.
  UINT sw = (fw + by - 1) / by, sh = (fh + by - 1) / by;
  WICPixelFormatGUID format;

  if (by == 1 || FAILED(t->lpVtbl->GetClosestSize(t, &sw, &sh)) || sw < w ||
      sh < h || sw >= fw || sh >= fh ||
      FAILED(IWICBitmapFrameDecode_GetPixelFormat(frame, &format)) ||
      FAILED(t->lpVtbl->GetClosestPixelFormat(t, &format))) {
    return nullptr;
  }

  u32 bits = bits_per_pixel(f, &format);
  usize stride = ((usize)sw * bits + 31) / 32 * 4, size = stride * sh;
  u8 *pixels = bits && size <= UINT32_MAX ? malloc(size) : nullptr;
  IWICBitmap *b = nullptr;

  if (pixels && SUCCEEDED(t->lpVtbl->CopyPixels(
                  t, nullptr, sw, sh, &format, WICBitmapTransformRotate0,
                  (UINT)stride, (UINT)size, pixels))) {
    IWICImagingFactory_CreateBitmapFromMemory(f, sw, sh, &format, (UINT)stride,
                                              (UINT)size, pixels, &b);
  }

  free(pixels);
  return b;
}

/* ========================================================================== */
/* colors. an image that carries a profile is in that profile's colors, a     */
/* phone's photo in display p3 for one, and is shown in srgb.                 */
/* ========================================================================== */

static u32 be32(const u8 *p) {
  return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
}

// whether an icc profile's description says srgb, which the pixels are shown
// in already. a version 4 profile says it in utf-16.
static bool says_srgb(const u8 *p, usize n) {
  u32 tags = n >= 132 ? be32(p + 128) : 0;

  for (u32 i = 0; i < tags && 132 + (usize)(i + 1) * 12 <= n; i++) {
    const u8 *t = p + 132 + (usize)i * 12;
    usize at = be32(t + 4), len = be32(t + 8);

    if (!memcmp(t, "desc", 4) && at <= n && len <= n - at) {
      return memmem(p + at, len, "sRGB", 4) ||
             memmem(p + at, len, "\0s\0R\0G\0B", 8);
    }
  }

  return false;
}

// a profile of `n` bytes as wic's, null for srgb's or one it cannot read.
static IWICColorContext *profile(IWICImagingFactory *f, const u8 *p, usize n) {
  IWICColorContext *c = nullptr;

  if (n == 0 || n > PROFILE_MAX || says_srgb(p, n) ||
      FAILED(IWICImagingFactory_CreateColorContext(f, &c))) {
    return nullptr;
  }

  if (FAILED(IWICColorContext_InitializeFromMemory(c, p, (UINT)n))) {
    release(c);
    return nullptr;
  }

  return c;
}

// what the frame's colors are in when that is not srgb, null otherwise: its
// profile, or adobe rgb as its exif says.
static IWICColorContext *colors_of(IWICImagingFactory *f,
                                   IWICBitmapFrameDecode *frame) {
  IWICColorContext *all[4] = {}, *found = nullptr;
  UINT count = 0;

  if (FAILED(
        IWICBitmapFrameDecode_GetColorContexts(frame, 0, nullptr, &count)) ||
      count == 0) {
    return nullptr;
  }

  count = MIN(count, countof(all));

  for (UINT i = 0; i < count; i++) {
    if (FAILED(IWICImagingFactory_CreateColorContext(f, &all[i]))) {
      count = i;
    }
  }

  if (count == 0 || FAILED(IWICBitmapFrameDecode_GetColorContexts(
                      frame, count, all, &count))) {
    count = 0;
  }

  for (UINT i = 0; i < count && !found; i++) {
    WICColorContextType type;
    UINT len = 0, space = 0;

    if (FAILED(IWICColorContext_GetType(all[i], &type))) {
      continue;
    }

    if (type == WICColorContextProfile &&
        SUCCEEDED(IWICColorContext_GetProfileBytes(all[i], 0, nullptr, &len)) &&
        len > 0 && len <= PROFILE_MAX) {
      u8 *bytes = malloc(len);

      if (bytes &&
          SUCCEEDED(
            IWICColorContext_GetProfileBytes(all[i], len, bytes, &len)) &&
          !says_srgb(bytes, len)) {
        found = all[i];
      }

      free(bytes);
    }
    else if (type == WICColorContextExifColorSpace &&
             SUCCEEDED(IWICColorContext_GetExifColorSpace(all[i], &space)) &&
             space == 2) {
      found = all[i];
    }
  }

  for (UINT i = 0; i < countof(all); i++) {
    if (all[i] != found) {
      release(all[i]);
    }
  }

  return found;
}

// `src` as premultiplied rgba of `w` by `h`, moved from `colors` to srgb
// when given. malloc'd, null when wic cannot.
static u8 *pixels_of(IWICImagingFactory *f, IWICBitmapSource *src,
                     IWICColorContext *colors, u32 w, u32 h) {
  IWICBitmapSource *given = src;
  IWICColorContext *srgb = nullptr;
  IWICColorTransform *moved = nullptr;
  IWICFormatConverter *bgra = nullptr, *premultiplied = nullptr;
  IWICFormatConverter *rgba = nullptr;
  IWICBitmapScaler *scaler = nullptr;
  defer release(srgb);
  defer release(moved);
  defer release(bgra);
  defer release(premultiplied);
  defer release(rgba);
  defer release(scaler);

  // a source the transform cannot read is given to it as bgra. a profile
  // it cannot use leaves the colors as they are.
  if (colors && SUCCEEDED(IWICImagingFactory_CreateColorContext(f, &srgb)) &&
      SUCCEEDED(IWICColorContext_InitializeFromExifColorSpace(srgb, 1)) &&
      SUCCEEDED(IWICImagingFactory_CreateColorTransformer(f, &moved))) {
    const GUID *to = &GUID_WICPixelFormat32bppBGRA;

    if (SUCCEEDED(
          IWICColorTransform_Initialize(moved, src, colors, srgb, to))) {
      src = (IWICBitmapSource *)moved;
    }
    else if (SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &bgra)) &&
             SUCCEEDED(IWICFormatConverter_Initialize(
               bgra, src, to, WICBitmapDitherTypeNone, nullptr, 0,
               WICBitmapPaletteTypeCustom)) &&
             SUCCEEDED(IWICColorTransform_Initialize(
               moved, (IWICBitmapSource *)bgra, colors, srgb, to))) {
      src = (IWICBitmapSource *)moved;
    }
  }

  UINT sw = 0, sh = 0;

  if (FAILED(IWICBitmapSource_GetSize(src, &sw, &sh))) {
    return nullptr;
  }

  // scaled as premultiplied bgra, see scale.
  if (sw != w || sh != h) {
    if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &premultiplied)) ||
        FAILED(IWICFormatConverter_Initialize(
          premultiplied, src, &GUID_WICPixelFormat32bppPBGRA,
          WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(IWICImagingFactory_CreateBitmapScaler(f, &scaler)) ||
        FAILED(
          IWICBitmapScaler_Initialize(scaler, (IWICBitmapSource *)premultiplied,
                                      w, h, WICBitmapInterpolationModeFant))) {
      return nullptr;
    }

    src = (IWICBitmapSource *)scaler;
  }

  if (FAILED(IWICImagingFactory_CreateFormatConverter(f, &rgba)) ||
      FAILED(IWICFormatConverter_Initialize(
        rgba, src, &GUID_WICPixelFormat32bppPRGBA, WICBitmapDitherTypeNone,
        nullptr, 0, WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }

  src = (IWICBitmapSource *)rgba;

  u8 *px = malloc((usize)w * h * 4);

  if (px &&
      FAILED(IWICBitmapSource_CopyPixels(src, nullptr, w * 4, w * h * 4, px))) {
    free(px);
    px = nullptr;
  }

  // a transform that took the profile may still fail to read through it.
  if (!px && colors) {
    return pixels_of(f, given, nullptr, w, h);
  }

  return px;
}

// a frame whole, as premultiplied rgba in srgb.
static u8 *frame_pixels(IWICImagingFactory *f, IWICBitmapFrameDecode *frame,
                        u32 *w, u32 *h) {
  UINT fw = 0, fh = 0;

  if (FAILED(IWICBitmapFrameDecode_GetSize(frame, &fw, &fh)) || !fw || !fh ||
      (u64)fw * fh > DECODED_PIXELS_MAX) {
    return nullptr;
  }

  IWICColorContext *colors = colors_of(f, frame);
  u8 *px = pixels_of(f, (IWICBitmapSource *)frame, colors, fw, fh);

  release(colors);
  *w = fw, *h = fh;
  return px;
}

/* ========================================================================== */
/* turns. a photo says in its exif how the camera was held.                   */
/* ========================================================================== */

static u32 orientation_of(IWICBitmapFrameDecode *frame) {
  IWICMetadataQueryReader *q = nullptr;

  if (FAILED(IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &q))) {
    return 1;
  }

  u32 o = meta(q, L"/app1/ifd/{ushort=274}", 0);

  o = o ? o : meta(q, L"/ifd/{ushort=274}", 1);
  release(q);
  return o >= 1 && o <= 8 ? o : 1;
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

/* ========================================================================== */
/* a still image, by wic.                                                     */
/* ========================================================================== */

// the frame shown: the first, or an icon's largest.
static IWICBitmapFrameDecode *shown_frame(IWICBitmapDecoder *d) {
  GUID kind;
  UINT count = 1, best = 0;
  u64 largest = 0;

  if (SUCCEEDED(IWICBitmapDecoder_GetContainerFormat(d, &kind)) &&
      IsEqualGUID(&kind, &GUID_ContainerFormatIco) &&
      SUCCEEDED(IWICBitmapDecoder_GetFrameCount(d, &count))) {
    for (UINT i = 0; i < count; i++) {
      IWICBitmapFrameDecode *frame = nullptr;
      UINT w = 0, h = 0;

      if (SUCCEEDED(IWICBitmapDecoder_GetFrame(d, i, &frame)) &&
          SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &w, &h)) &&
          (u64)w * h > largest) {
        largest = (u64)w * h;
        best = i;
      }

      release(frame);
    }
  }

  IWICBitmapFrameDecode *frame = nullptr;
  IWICBitmapDecoder_GetFrame(d, best, &frame);
  return frame;
}

static u8 *decode_wic(IWICImagingFactory *f, const u8 *bytes, usize n,
                      u32 max_pixels, u32 *w, u32 *h) {
  IWICBitmapDecoder *d = open_bytes(f, bytes, n);
  IWICBitmapFrameDecode *frame = d ? shown_frame(d) : nullptr;
  defer release(d);
  defer release(frame);

  UINT fw = 0, fh = 0;

  if (!frame || FAILED(IWICBitmapFrameDecode_GetSize(frame, &fw, &fh)) || !fw ||
      !fh || (u64)fw * fh > DECODED_PIXELS_MAX) {
    return nullptr;
  }

  u32 tw, th;

  fit(fw, fh, max_pixels, &tw, &th);

  IWICBitmap *smaller = tw < fw ? reduced(f, frame, fw, fh, tw, th) : nullptr;
  IWICColorContext *colors = colors_of(f, frame);
  IWICBitmapSource *src =
    smaller ? (IWICBitmapSource *)smaller : (IWICBitmapSource *)frame;
  u8 *px = pixels_of(f, src, colors, tw, th);

  release(colors);
  release(smaller);
  px = oriented(px, &tw, &th, orientation_of(frame));

  if (px) {
    *w = tw, *h = th;
  }

  return px;
}

/* ========================================================================== */
/* animated pngs, whose frames are pngs of their own in chunks of the file.   */
/* ========================================================================== */

static void put_be32(u8 *p, u32 v) {
  p[0] = (u8)(v >> 24), p[1] = (u8)(v >> 16), p[2] = (u8)(v >> 8), p[3] = (u8)v;
}

// what a chunk of png ends with, over its type and its bytes.
static u32 png_crc(const u8 *b, usize n, u32 crc) {
  static u32 table[256];

  if (!table[1]) {
    for (u32 i = 0; i < 256; i++) {
      u32 c = i;

      for (u32 k = 0; k < 8; k++) {
        c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
      }

      table[i] = c;
    }
  }

  for (usize i = 0; i < n; i++) {
    crc = table[(crc ^ b[i]) & 0xff] ^ (crc >> 8);
  }

  return crc;
}

static usize put_chunk(u8 *to, const char *type, const u8 *data, u32 len) {
  put_be32(to, len);
  memcpy(to + 4, type, 4);

  if (len) {
    memcpy(to + 8, data, len);
  }

  put_be32(to + 8 + len, ~png_crc(to + 4, len + 4, ~0u));
  return (usize)len + 12;
}

// what becomes of the canvas after a frame, the dispose_op of the apng
// specification, and how it goes over it, its blend_op.
enum : u8 {
  DISPOSE_NONE,
  DISPOSE_BACKGROUND,
  DISPOSE_PREVIOUS
};

enum : u8 {
  BLEND_SOURCE,
  BLEND_OVER
};

typedef struct {
  u32 width, height, x, y;
  f32 delay;
  u8 dispose, blend;
  // the chunks of its pixels are from `from` up to `end` in the file.
  usize from, end;
} Place;

typedef struct {
  // the 13 bytes of IHDR.
  const u8 *header;
  // the chunks before the pixels every frame is read with: the palette,
  // what is transparent, the gamma and the profile.
  usize kept[16][2];
  u32 nkept;
  Place *frames;
  u32 count;
} Apng;

// the frames an animated png says it has. false for a still one.
static bool apng_open(Apng *a, const u8 *b, usize n) {
  usize at = 8;
  bool told = false, pixels = false;
  u32 cap = 0;

  *a = (Apng){};

  if (n < 8 || memcmp(b, "\x89PNG\r\n\x1a\n", 8)) {
    return false;
  }

  while (at + 12 <= n) {
    u32 len = be32(b + at);
    const char *type = (const char *)b + at + 4;
    const u8 *data = b + at + 8;

    if (len > n - at - 12) {
      break;
    }

    usize next = at + 12 + len;

    if (!memcmp(type, "IHDR", 4) && len == 13) {
      a->header = data;
    }
    else if (!memcmp(type, "acTL", 4)) {
      told = true;
    }
    else if (!memcmp(type, "fcTL", 4) && len == 26) {
      if (a->count == cap) {
        cap = cap ? cap * 2 : 16;

        Place *grown = cap <= PNG_FRAMES_MAX
                         ? realloc(a->frames, cap * sizeof *grown)
                         : nullptr;

        if (!grown) {
          break;
        }

        a->frames = grown;
      }

      if (a->count) {
        a->frames[a->count - 1].end = at;
      }

      u32 over = (u32)data[20] << 8 | data[21];
      u32 under = (u32)data[22] << 8 | data[23];

      a->frames[a->count++] = (Place){
        .width = be32(data + 4),
        .height = be32(data + 8),
        .x = be32(data + 12),
        .y = be32(data + 16),
        .delay = (f32)over / (f32)(under ? under : 100),
        .dispose = data[24],
        .blend = data[25] == 1 ? BLEND_OVER : BLEND_SOURCE,
        .from = next,
        .end = next,
      };
    }
    else if (!memcmp(type, "IDAT", 4)) {
      pixels = true;
    }
    else if (!memcmp(type, "IEND", 4)) {
      if (a->count) {
        a->frames[a->count - 1].end = at;
      }

      break;
    }
    else if (!pixels && memcmp(type, "fdAT", 4) &&
             a->nkept < countof(a->kept)) {
      a->kept[a->nkept][0] = at;
      a->kept[a->nkept++][1] = next;
    }

    at = next;
  }

  if (!told || !a->header || a->count < 2) {
    free(a->frames);
    *a = (Apng){};
    return false;
  }

  return true;
}

// a frame as a png of its own in `to`, which wic reads: the header with its
// size, what was kept, then its pixels as IDAT.
static usize apng_frame(const Apng *a, const Place *p, const u8 *b, u8 *to) {
  static const u8 SIGNATURE[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  u8 header[13];
  usize len = sizeof SIGNATURE;

  memcpy(to, SIGNATURE, sizeof SIGNATURE);
  memcpy(header, a->header, sizeof header);
  put_be32(header, p->width);
  put_be32(header + 4, p->height);
  len += put_chunk(to + len, "IHDR", header, sizeof header);

  for (u32 i = 0; i < a->nkept; i++) {
    memcpy(to + len, b + a->kept[i][0], a->kept[i][1] - a->kept[i][0]);
    len += a->kept[i][1] - a->kept[i][0];
  }

  for (usize at = p->from; at < p->end;) {
    u32 size = be32(b + at);
    const char *type = (const char *)b + at + 4;

    if (!memcmp(type, "IDAT", 4)) {
      len += put_chunk(to + len, "IDAT", b + at + 8, size);
    }
    else if (!memcmp(type, "fdAT", 4) && size >= 4) {
      len += put_chunk(to + len, "IDAT", b + at + 12, size - 4);
    }

    at += (usize)size + 12;
  }

  return len + put_chunk(to + len, "IEND", nullptr, 0);
}

/* ========================================================================== */
/* the frames of a gif or an animated png, drawn over one another on a canvas */
/* of the image's size.                                                       */
/* ========================================================================== */

typedef struct {
  IWICImagingFactory *f;
  const u8 *bytes;
  usize n;
  // a gif's frames are wic's, an animated png's are cut out of its chunks
  // into `file`.
  IWICBitmapDecoder *gif;
  Apng png;
  u8 *file;
  u32 width, height, count, at;
  // premultiplied rgba, what is shown now, and what was before the frame
  // drawn last when it is to come back.
  u8 *canvas, *before;
  Place last;
} Anim;

static void anim_close(Anim *a) {
  release(a->gif);
  free(a->png.frames);
  free(a->file);
  free(a->canvas);
  free(a->before);
}

static bool anim_open(Anim *a, IWICImagingFactory *f, const u8 *bytes,
                      usize n) {
  *a = (Anim){.f = f, .bytes = bytes, .n = n};

  if (n >= 6 && (!memcmp(bytes, "GIF87a", 6) || !memcmp(bytes, "GIF89a", 6))) {
    IWICMetadataQueryReader *q = nullptr;
    UINT count = 0;

    a->gif = open_bytes(f, bytes, n);

    if (!a->gif || FAILED(IWICBitmapDecoder_GetFrameCount(a->gif, &count)) ||
        !count) {
      anim_close(a);
      return false;
    }

    IWICBitmapDecoder_GetMetadataQueryReader(a->gif, &q);
    a->width = meta(q, L"/logscrdesc/Width", 0);
    a->height = meta(q, L"/logscrdesc/Height", 0);
    a->count = count;
    release(q);

    // a screen of no size is the first frame's.
    if (!a->width || !a->height) {
      IWICBitmapFrameDecode *first = nullptr;
      UINT w = 0, h = 0;

      if (SUCCEEDED(IWICBitmapDecoder_GetFrame(a->gif, 0, &first))) {
        IWICBitmapFrameDecode_GetSize(first, &w, &h);
      }

      release(first);
      a->width = w, a->height = h;
    }
  }
  else if (apng_open(&a->png, bytes, n)) {
    a->width = be32(a->png.header);
    a->height = be32(a->png.header + 4);
    a->count = a->png.count;
    a->file = malloc(n + 64);
  }
  else {
    return false;
  }

  usize size = (usize)a->width * a->height * 4;

  if (!a->width || !a->height ||
      (u64)a->width * a->height > DECODED_PIXELS_MAX || (!a->gif && !a->file) ||
      !(a->canvas = calloc(1, size))) {
    anim_close(a);
    return false;
  }

  return true;
}

// the next frame's pixels, its size, and where and how it goes.
static u8 *anim_frame(Anim *a, Place *p) {
  u32 w = 0, h = 0;
  u8 *px = nullptr;

  if (a->gif) {
    IWICBitmapFrameDecode *frame = nullptr;
    IWICMetadataQueryReader *q = nullptr;

    if (FAILED(IWICBitmapDecoder_GetFrame(a->gif, a->at, &frame))) {
      return nullptr;
    }

    IWICBitmapFrameDecode_GetMetadataQueryReader(frame, &q);

    // browsers show a frame that asks for 10 ms or less for 100 ms, and so
    // do the gifs made for them. the third way of the gif is the previous.
    u32 hundredths = meta(q, L"/grctlext/Delay", 0);
    u32 dispose = meta(q, L"/grctlext/Disposal", 0);

    *p = (Place){
      .x = meta(q, L"/imgdesc/Left", 0),
      .y = meta(q, L"/imgdesc/Top", 0),
      .delay = hundredths <= 1 ? 0.1f : (f32)hundredths / 100.0f,
      .dispose = dispose == 2   ? DISPOSE_BACKGROUND
                 : dispose == 3 ? DISPOSE_PREVIOUS
                                : DISPOSE_NONE,
      .blend = BLEND_OVER,
    };
    px = frame_pixels(a->f, frame, &w, &h);
    release(q);
    release(frame);
  }
  else {
    IWICBitmapDecoder *d = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;

    *p = a->png.frames[a->at];

    usize len = apng_frame(&a->png, p, a->bytes, a->file);

    if ((d = open_bytes(a->f, a->file, len)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(d, 0, &frame))) {
      px = frame_pixels(a->f, frame, &w, &h);
    }

    release(frame);
    release(d);

    if (px && (w != p->width || h != p->height)) {
      free(px);
      px = nullptr;
    }

    p->delay = p->delay <= 0.01f ? 0.1f : p->delay;
  }

  p->width = w, p->height = h;
  return px;
}

// `p`'s rows of the canvas, cut to it: where they start, how many and how
// long.
static bool region(const Anim *a, const Place *p, usize *start, u32 *rows,
                   u32 *cols) {
  if (p->x >= a->width || p->y >= a->height) {
    return false;
  }

  *start = ((usize)p->y * a->width + p->x) * 4;
  *rows = MIN(p->height, a->height - p->y);
  *cols = MIN(p->width, a->width - p->x);
  return *rows && *cols;
}

// x over 255, rounded, for x up to 255 * 255.
static u32 div255(u32 x) {
  x += 128;
  return (x + (x >> 8)) >> 8;
}

// the next frame on the canvas, after what the one before asked for once it
// was shown. false when there is none.
static bool anim_next(Anim *a, f32 *delay) {
  usize size = (usize)a->width * a->height * 4, start;
  usize row = (usize)a->width * 4;
  u32 rows, cols;

  if (a->at >= a->count) {
    return false;
  }

  if (a->at > 0 && region(a, &a->last, &start, &rows, &cols)) {
    for (u32 y = 0; y < rows; y++) {
      u8 *to = a->canvas + start + y * row;

      if (a->last.dispose == DISPOSE_BACKGROUND) {
        memset(to, 0, (usize)cols * 4);
      }
      else if (a->last.dispose == DISPOSE_PREVIOUS && a->before) {
        memcpy(to, a->before + start + y * row, (usize)cols * 4);
      }
    }
  }

  Place p;
  u8 *px = anim_frame(a, &p);

  if (!px) {
    return false;
  }

  // the first frame has nothing before it to go back to.
  if (a->at == 0 && p.dispose == DISPOSE_PREVIOUS) {
    p.dispose = DISPOSE_BACKGROUND;
  }

  if (p.dispose == DISPOSE_PREVIOUS) {
    a->before = a->before ? a->before : malloc(size);

    if (a->before) {
      memcpy(a->before, a->canvas, size);
    }
  }

  if (region(a, &p, &start, &rows, &cols)) {
    for (u32 y = 0; y < rows; y++) {
      u8 *to = a->canvas + start + y * row;
      const u8 *from = px + (usize)y * p.width * 4;

      if (p.blend == BLEND_SOURCE) {
        memcpy(to, from, (usize)cols * 4);
        continue;
      }

      for (u32 x = 0; x < cols; x++, to += 4, from += 4) {
        u32 left = 255 - from[3];

        for (u32 k = 0; k < 4; k++) {
          to[k] = (u8)(from[k] + div255(to[k] * left));
        }
      }
    }
  }

  free(px);
  a->last = p;
  a->at++;
  *delay = p.delay;
  return true;
}

/* ========================================================================== */
/* webp.                                                                      */
/* ========================================================================== */

#ifdef CIVUE_WEBP
static bool is_webp(const u8 *b, usize n) {
  return n >= 12 && !memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WEBP", 4);
}

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
static u8 *decode_webp(IWICImagingFactory *f, const u8 *bytes, usize n,
                       u32 max_pixels, u32 *w, u32 *h) {
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

  usize size = (usize)tw * th * 4;
  u8 *rgba = malloc(size);

  if (!rgba) {
    return nullptr;
  }

  // the extended header says whether a profile comes with it. its colors
  // are moved before they are multiplied by their alpha.
  bool profiled = n > 21 && !memcmp(bytes + 12, "VP8X", 4) && bytes[20] & 0x20;
  IWICColorContext *colors = nullptr;

  if (profiled) {
    WebPData data = {bytes, n};
    WebPDemuxer *demux = WebPDemux(&data);
    WebPChunkIterator chunk;

    if (demux && WebPDemuxGetChunk(demux, "ICCP", 1, &chunk)) {
      colors = profile(f, chunk.chunk.bytes, chunk.chunk.size);
      WebPDemuxReleaseChunkIterator(&chunk);
    }

    WebPDemuxDelete(demux);
  }

  config.output.colorspace = colors ? MODE_BGRA : MODE_rgbA;
  config.output.is_external_memory = 1;
  config.output.u.RGBA.rgba = rgba;
  config.output.u.RGBA.stride = (int)tw * 4;
  config.output.u.RGBA.size = size;
  config.options.use_scaling = tw != iw || th != ih;
  config.options.scaled_width = (int)tw;
  config.options.scaled_height = (int)th;

  if (WebPDecode(bytes, n, &config) != VP8_STATUS_OK) {
    release(colors);
    free(rgba);
    return nullptr;
  }

  if (colors) {
    IWICBitmap *b = nullptr;
    u8 *moved = nullptr;

    if (SUCCEEDED(IWICImagingFactory_CreateBitmapFromMemory(
          f, tw, th, &GUID_WICPixelFormat32bppBGRA, tw * 4, (UINT)size, rgba,
          &b))) {
      moved = pixels_of(f, (IWICBitmapSource *)b, colors, tw, th);
    }

    release(b);
    release(colors);
    free(rgba);
    rgba = moved;
  }

  if (rgba) {
    *w = tw, *h = th;
  }

  return rgba;
}
#endif

static bool is_gif(const u8 *b, usize n) {
  return n >= 6 && (!memcmp(b, "GIF87a", 6) || !memcmp(b, "GIF89a", 6));
}

u8 *civue__image_decode(const u8 *bytes, usize n, u32 max_pixels, u32 *width,
                        u32 *height) {
  IWICImagingFactory *f = wic();
  defer release(f);

  if (!f || !bytes || n == 0) {
    return nullptr;
  }

  u8 *rgba = nullptr;
  u32 w = 0, h = 0, tw, th;

#ifdef CIVUE_WEBP
  if (is_webp(bytes, n)) {
    rgba = decode_webp(f, bytes, n, max_pixels, &w, &h);
  }
#endif

  // a gif's first frame goes on its canvas, which may be larger.
  if (is_gif(bytes, n)) {
    Anim a;
    f32 delay;

    if (anim_open(&a, f, bytes, n)) {
      if (anim_next(&a, &delay)) {
        rgba = a.canvas;
        a.canvas = nullptr;
        w = a.width, h = a.height;
      }

      anim_close(&a);
    }
  }

  if (!rgba) {
    rgba = decode_wic(f, bytes, n, max_pixels, &w, &h);
  }

  if (!rgba) {
    return nullptr;
  }

  fit(w, h, max_pixels, &tw, &th);
  rgba = sized(f, rgba, w, h, tw, th);

  if (rgba) {
    *width = tw;
    *height = th;
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

static bool anim_frames(IWICImagingFactory *f, const u8 *bytes, usize n,
                        u32 max_pixels, usize max_bytes, civue__frames *out) {
  Anim a;
  u32 w, h;

  if (!anim_open(&a, f, bytes, n)) {
    return false;
  }

  defer anim_close(&a);

  if (a.count < 2 ||
      !frames_size(a.width, a.height, a.count, max_pixels, max_bytes, &w, &h)) {
    return false;
  }

  usize frame = (usize)w * h * 4;
  u8 *rgba = calloc(a.count, frame);
  f32 *delays = calloc(a.count, sizeof(f32));
  u32 made = 0;
  bool ok = rgba && delays;

  while (ok && made < a.count && anim_next(&a, &delays[made])) {
    ok = scale(f, a.canvas, a.width, a.height, rgba + made * frame, w, h);
    made++;
  }

  if (!ok || made < 2) {
    free(rgba);
    free(delays);
    return false;
  }

  *out = (civue__frames){rgba, delays, w, h, made};
  return true;
}

#ifdef CIVUE_WEBP
static bool webp_frames(IWICImagingFactory *f, const u8 *bytes, usize n,
                        u32 max_pixels, usize max_bytes, civue__frames *out) {
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
  u8 *rgba = calloc(count, frame);
  f32 *delays = calloc(count, sizeof(f32));
  bool ok = rgba && delays;
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
    ok = scale(f, canvas, info.canvas_width, info.canvas_height,
               rgba + made * frame, w, h);
    made++;
  }

  if (!ok || made < 2) {
    free(rgba);
    free(delays);
    return false;
  }

  *out = (civue__frames){rgba, delays, w, h, made};
  return true;
}
#endif

bool civue__image_decode_frames(const u8 *bytes, usize n, u32 max_pixels,
                                usize max_bytes, civue__frames *out) {
  IWICImagingFactory *f = wic();
  defer release(f);

  if (!f || !bytes || n == 0) {
    return false;
  }

#ifdef CIVUE_WEBP
  if (is_webp(bytes, n)) {
    return webp_frames(f, bytes, n, max_pixels, max_bytes, out);
  }
#endif

  return anim_frames(f, bytes, n, max_pixels, max_bytes, out);
}
