#include "win.h"

#include <civue/icons.h>

#include <d2d1_1.h>
#include <limits.h>
#include <stddefer.h>
#include <wincodec.h>

/* ========================================================================== */
/* svgs are drawn by direct2d, into a bitmap of wic's. mingw declares its svg */
/* calls for c++ alone: the two used here are taken from the vtable of        */
/* ID2D1DeviceContext5 by hand, after the 92 calls of ID2D1DeviceContext and  */
/* the 23 its versions 1 to 4 add, which com keeps where they are for good.   */
/* ========================================================================== */

typedef struct {
  ID2D1DeviceContextVtbl base;
  void *added[23];
  HRESULT(STDMETHODCALLTYPE *CreateSvgDocument)(void *self, IStream *xml,
                                                D2D1_SIZE_F viewport,
                                                IUnknown **document);
  void(STDMETHODCALLTYPE *DrawSvgDocument)(void *self, IUnknown *document);
} Context5Vtbl;

typedef struct {
  const Context5Vtbl *lpVtbl;
} Context5;

static const IID IID_Context5 = {
  0x7836d248, 0x68cc, 0x4df6, {0xb9, 0xe8, 0xde, 0x99, 0x1b, 0xf6, 0x2e, 0xb7}};

static void release(void *p) {
  if (p) {
    IUnknown_Release((IUnknown *)p);
  }
}

// the factories of a thread that draws svgs, made by its first and kept:
// direct2d's takes milliseconds to make.
static thread_local IWICImagingFactory *wic;
static thread_local ID2D1Factory *d2d;

static bool factories() {
  D2D1_FACTORY_OPTIONS options = {D2D1_DEBUG_LEVEL_NONE};

  civue__com_init();

  if (!wic && FAILED(CoCreateInstance(
                &CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                &IID_IWICImagingFactory, (void **)&wic))) {
    wic = nullptr;
  }

  if (!d2d &&
      FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                               &IID_ID2D1Factory1, &options, (void **)&d2d))) {
    d2d = nullptr;
  }

  return wic && d2d;
}

// the length of the value of `name` in the tag from `tag` up to `end`, and
// where it starts and where its attribute does. -1 without it.
static i64 attribute(const char *tag, const char *end, const char *name,
                     const char **value, const char **from) {
  usize len = strlen(name);

  for (const char *p = tag; p + len + 2 < end; p++) {
    bool spaced = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';

    if (!spaced || memcmp(p + 1, name, len) || p[len + 1] != '=' ||
        (p[len + 2] != '"' && p[len + 2] != '\'')) {
      continue;
    }

    const char *at = p + len + 3, *close = memchr(at, p[len + 2], end - at);

    if (close) {
      *value = at, *from = p;
      return close - at;
    }
  }

  return -1;
}

// the svg with its root's width and height left out, so that it fills what
// it is drawn in as librsvg and appkit draw it: given in ems direct2d would
// refuse it, given in pixels it would draw at that size in a corner. a root
// without a view box is given one of that size.
static char *filling(const char *svg, usize n, usize *len) {
  const char *root = memmem(svg, n, "<svg", 4);
  const char *end = root ? memchr(root, '>', svg + n - root) : nullptr;

  if (!end) {
    return nullptr;
  }

  const char *value, *from, *cut[2][2] = {};
  const char *names[2] = {"width", "height"};
  f32 size[2] = {};

  for (u32 i = 0; i < 2; i++) {
    i64 got = attribute(root + 4, end, names[i], &value, &from);

    if (got >= 0) {
      cut[i][0] = from;
      cut[i][1] = value + got + 1;
      size[i] = strtof(value, nullptr);
    }
  }

  civue_bytes out = {};

  buf_put(&out, svg, root + 4 - svg);

  if (attribute(root + 4, end, "viewBox", &value, &from) < 0 && size[0] > 0 &&
      size[1] > 0) {
    buf_printf(&out, " viewBox=\"0 0 %g %g\"", size[0], size[1]);
  }

  // the two are cut in the order they come.
  u32 first = cut[1][0] && (!cut[0][0] || cut[1][0] < cut[0][0]);
  const char *at = root + 4;

  for (u32 k = 0; k < 2; k++) {
    const char **c = cut[k ? !first : first];

    if (c[0]) {
      buf_put(&out, at, c[0] - at);
      at = c[1];
    }
  }

  buf_put(&out, at, svg + n - at);
  *len = out.len;
  return (char *)out.v;
}

bool civue_svg_rasterize(const char *svg, usize n, u32 w, u32 h,
                         civue_bytes *rgba) {
  if (!svg || !rgba || !w || !h || w > INT_MAX || h > INT_MAX ||
      (u64)w * h > UINT32_MAX / 4) {
    return false;
  }

  usize len = 0;
  char *xml = filling(svg, n, &len);
  defer free(xml);

  IWICStream *stream = nullptr;
  IWICBitmap *bitmap = nullptr;
  ID2D1RenderTarget *target = nullptr;
  Context5 *context = nullptr;
  IUnknown *document = nullptr;
  defer release(stream);
  defer release(bitmap);
  defer release(target);
  defer release(context);
  defer release(document);

  D2D1_RENDER_TARGET_PROPERTIES props = {
    .pixelFormat = {DXGI_FORMAT_R8G8B8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED},
  };

  // a system before windows 10's of 2017 has no svg: no context of it.
  if (!xml || len > UINT32_MAX || !factories() ||
      FAILED(IWICImagingFactory_CreateStream(wic, &stream)) ||
      FAILED(
        IWICStream_InitializeFromMemory(stream, (BYTE *)xml, (DWORD)len)) ||
      FAILED(IWICImagingFactory_CreateBitmap(wic, w, h,
                                             &GUID_WICPixelFormat32bppPRGBA,
                                             WICBitmapCacheOnLoad, &bitmap)) ||
      FAILED(ID2D1Factory_CreateWicBitmapRenderTarget(d2d, bitmap, &props,
                                                      &target)) ||
      FAILED(ID2D1RenderTarget_QueryInterface(target, &IID_Context5,
                                              (void **)&context)) ||
      FAILED(context->lpVtbl->CreateSvgDocument(context, (IStream *)stream,
                                                (D2D1_SIZE_F){(f32)w, (f32)h},
                                                &document))) {
    return false;
  }

  ID2D1RenderTarget_BeginDraw(target);
  ID2D1RenderTarget_Clear(target, &(D2D1_COLOR_F){});
  context->lpVtbl->DrawSvgDocument(context, document);

  if (FAILED(ID2D1RenderTarget_EndDraw(target, nullptr, nullptr))) {
    return false;
  }

  usize size = (usize)w * h * 4;

  rgba->len = 0;
  vec_reserve(rgba, size);
  rgba->len = size;
  return SUCCEEDED(
    IWICBitmap_CopyPixels(bitmap, nullptr, w * 4, (UINT)size, rgba->v));
}
