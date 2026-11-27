#include "win.h"

#include <civue/system.h>

#include <shellapi.h>
#include <wincodec.h>

static IWICImagingFactory *wic() {
  static thread_local IWICImagingFactory *f;

  if (!f) {
    civue__com_init();
    CoCreateInstance(&CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                     &IID_IWICImagingFactory, (void **)&f);
  }

  return f;
}

// the pixels of the image in `bytes`, of any format wic reads, as straight
// bgra8, rows from the top. malloc'd, null when it reads none.
static u8 *pixels_of(const u8 *bytes, usize n, u32 *width, u32 *height) {
  IWICImagingFactory *f = wic();
  IWICStream *stream = nullptr;
  IWICBitmapDecoder *decoder = nullptr;
  IWICBitmapFrameDecode *frame = nullptr;
  IWICBitmapSource *bgra = nullptr;
  u8 *out = nullptr;

  if (!f || n > UINT32_MAX ||
      FAILED(IWICImagingFactory_CreateStream(f, &stream)) ||
      FAILED(IWICStream_InitializeFromMemory(stream, (u8 *)bytes, (DWORD)n)) ||
      FAILED(IWICImagingFactory_CreateDecoderFromStream(
        f, (IStream *)stream, nullptr, WICDecodeMetadataCacheOnDemand,
        &decoder)) ||
      FAILED(IWICBitmapDecoder_GetFrame(decoder, 0, &frame)) ||
      FAILED(WICConvertBitmapSource(&GUID_WICPixelFormat32bppBGRA,
                                    (IWICBitmapSource *)frame, &bgra))) {
    goto done;
  }

  UINT w = 0, h = 0;
  IWICBitmapSource_GetSize(bgra, &w, &h);

  if (!w || !h || (u64)w * h > 1 << 28) {
    goto done;
  }

  out = xmalloc((usize)w * h * 4);

  if (FAILED(
        IWICBitmapSource_CopyPixels(bgra, nullptr, w * 4, w * h * 4, out))) {
    free(out);
    out = nullptr;
    goto done;
  }

  *width = w;
  *height = h;

done:
  if (bgra) {
    IWICBitmapSource_Release(bgra);
  }

  if (frame) {
    IWICBitmapFrameDecode_Release(frame);
  }

  if (decoder) {
    IWICBitmapDecoder_Release(decoder);
  }

  if (stream) {
    IWICStream_Release(stream);
  }

  return out;
}

u8 *civue__png_pixels(const u8 *png, usize n, u32 *width, u32 *height) {
  return pixels_of(png, n, width, height);
}

// `w` by `h` straight bgra8 pixels as png bytes, false when wic makes none.
static bool png_of(const u8 *bgra, u32 w, u32 h, civue_bytes *out) {
  IWICImagingFactory *f = wic();
  IStream *stream = nullptr;
  IWICBitmapEncoder *encoder = nullptr;
  IWICBitmapFrameEncode *frame = nullptr;
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  bool ok = false;

  if (!f || FAILED(CreateStreamOnHGlobal(nullptr, true, &stream)) ||
      FAILED(IWICImagingFactory_CreateEncoder(f, &GUID_ContainerFormatPng,
                                              nullptr, &encoder)) ||
      FAILED(IWICBitmapEncoder_Initialize(encoder, stream,
                                          WICBitmapEncoderNoCache)) ||
      FAILED(IWICBitmapEncoder_CreateNewFrame(encoder, &frame, nullptr)) ||
      FAILED(IWICBitmapFrameEncode_Initialize(frame, nullptr)) ||
      FAILED(IWICBitmapFrameEncode_SetSize(frame, w, h)) ||
      FAILED(IWICBitmapFrameEncode_SetPixelFormat(frame, &format)) ||
      !IsEqualGUID(&format, &GUID_WICPixelFormat32bppBGRA) ||
      FAILED(IWICBitmapFrameEncode_WritePixels(frame, h, w * 4, w * h * 4,
                                               (u8 *)bgra)) ||
      FAILED(IWICBitmapFrameEncode_Commit(frame)) ||
      FAILED(IWICBitmapEncoder_Commit(encoder))) {
    goto done;
  }

  HGLOBAL g = nullptr;
  STATSTG st = {};

  if (FAILED(GetHGlobalFromStream(stream, &g)) ||
      FAILED(IStream_Stat(stream, &st, STATFLAG_NONAME))) {
    goto done;
  }

  const u8 *p = GlobalLock(g);

  if (p) {
    out->len = 0;
    buf_put(out, p, (usize)st.cbSize.QuadPart);
    GlobalUnlock(g);
    ok = true;
  }

done:
  if (frame) {
    IWICBitmapFrameEncode_Release(frame);
  }

  if (encoder) {
    IWICBitmapEncoder_Release(encoder);
  }

  if (stream) {
    IStream_Release(stream);
  }

  return ok;
}

// a bitmap of the clipboard as png bytes: wic reads it as a .bmp file, the
// file's header put before it.
static bool png_of_dib(const u8 *dib, usize n, civue_bytes *out) {
  if (n < sizeof(BITMAPINFOHEADER)) {
    return false;
  }

  BITMAPINFOHEADER bi;
  memcpy(&bi, dib, sizeof bi);

  if (bi.biSize < sizeof bi || bi.biSize > n) {
    return false;
  }

  // the palette of a bitmap of few bits follows the header, and the masks
  // its fields' when it has them: after an info header, and after the
  // version 5 header windows makes of a copied bitmap too, which the size
  // tells apart from one that keeps them in the header alone.
  usize colors = bi.biClrUsed         ? bi.biClrUsed
                 : bi.biBitCount <= 8 ? 1u << bi.biBitCount
                                      : 0;
  usize stride = ((usize)bi.biWidth * bi.biBitCount + 31) / 32 * 4;
  usize rows = (usize)(bi.biHeight < 0 ? -(i64)bi.biHeight : bi.biHeight);
  usize pixels = stride * rows;
  bool fields = bi.biCompression == BI_BITFIELDS || bi.biCompression == 6;
  usize most = bi.biCompression == 6 ? 16 : 12;
  usize masks = !fields                                       ? 0
                : bi.biSize == sizeof bi                      ? most
                : n >= bi.biSize + colors * 4 + most + pixels ? most
                                                              : 0;
  usize at = sizeof(BITMAPFILEHEADER) + bi.biSize + masks + colors * 4;

  civue_bytes file = {};
  BITMAPFILEHEADER fh = {
    .bfType = 'B' | 'M' << 8,
    .bfSize = (DWORD)(sizeof fh + n),
    .bfOffBits = (DWORD)at,
  };

  buf_put(&file, &fh, sizeof fh);
  buf_put(&file, dib, n);

  u32 w, h;
  u8 *bgra = pixels_of(file.v, file.len, &w, &h);
  vec_free(&file);

  if (!bgra) {
    return false;
  }

  // a bitmap of 32 bits often keeps nothing in its alpha, it is opaque.
  bool clear = true;

  for (usize i = 3; i < (usize)w * h * 4 && clear; i += 4) {
    clear = !bgra[i];
  }

  if (clear) {
    for (usize i = 3; i < (usize)w * h * 4; i += 4) {
      bgra[i] = 0xff;
    }
  }

  bool ok = png_of(bgra, w, h, out);
  free(bgra);
  return ok;
}

// what the clipboard holds of `format`, false for none.
static bool read_format(UINT format, civue_bytes *out) {
  HANDLE h = format ? GetClipboardData(format) : nullptr;
  const u8 *p = h ? GlobalLock(h) : nullptr;

  if (!p) {
    return false;
  }

  out->len = 0;
  buf_put(out, p, GlobalSize(h));
  GlobalUnlock(h);
  return out->len > 0;
}

// the formats images are copied as by the names apps register them with:
// browsers, office, paint. what apps read as it is, then what is converted.
static const struct {
  const wchar_t *name;
  const char *mime;
  bool converted;
} FORMATS[] = {
  {L"PNG", "image/png"},
  {L"image/png", "image/png"},
  {L"JFIF", "image/jpeg"},
  {L"image/jpeg", "image/jpeg"},
  {L"GIF", "image/gif"},
  {L"image/gif", "image/gif"},
  {L"image/webp", "image/webp"},
  {L"image/heic", "image/heic", true},
  {L"image/heif", "image/heif", true},
  {L"image/avif", "image/avif", true},
  {L"image/tiff", "image/tiff", true},
};

bool civue_pasteboard_image(civue_bytes *out, char **mime,
                            civue_image_convert convert) {
  *out = (civue_bytes){};
  *mime = nullptr;

  if (!civue__clipboard_open()) {
    return false;
  }

  civue_bytes raw = {};

  for (u32 i = 0; i < countof(FORMATS) && !out->len; i++) {
    UINT f = RegisterClipboardFormatW(FORMATS[i].name);

    if (!IsClipboardFormatAvailable(f) || !read_format(f, &raw)) {
      continue;
    }

    if (!FORMATS[i].converted || !convert) {
      *out = raw;
      raw = (civue_bytes){};
      *mime = xstrdup(FORMATS[i].mime);
      continue;
    }

    const char *converted;

    if (convert(&raw, out, &converted)) {
      *mime = xstrdup(converted);
    }
  }

  // a bitmap otherwise, a screenshot's, made png. its fifth version keeps
  // its alpha.
  if (!out->len && (read_format(CF_DIBV5, &raw) || read_format(CF_DIB, &raw)) &&
      png_of_dib(raw.v, raw.len, out)) {
    *mime = xstrdup("image/png");
  }

  vec_free(&raw);
  CloseClipboard();
  return out->len != 0;
}

civue_strs civue_pasteboard_files() {
  civue_strs out = {};

  if (!civue__clipboard_open()) {
    return out;
  }

  HDROP drop = GetClipboardData(CF_HDROP);
  UINT n = drop ? DragQueryFileW(drop, 0xffffffff, nullptr, 0) : 0;

  for (UINT i = 0; i < n; i++) {
    UINT len = DragQueryFileW(drop, i, nullptr, 0);
    wchar_t *w = xmalloc(((usize)len + 1) * sizeof *w);

    if (DragQueryFileW(drop, i, w, len + 1)) {
      vec_push(&out, civue__utf8(w, -1));
    }

    free(w);
  }

  CloseClipboard();
  return out;
}

bool civue_pasteboard_holds_other() {
  if (!civue__clipboard_open()) {
    return false;
  }

  bool any = CountClipboardFormats() > 0;
  CloseClipboard();
  return any;
}
