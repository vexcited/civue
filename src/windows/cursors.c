#include "win.h"

#include <math.h>

// a cursor made once for each look, by kind and angle, at the pixels the
// window's scale wants. null for the system's own.
typedef struct {
  civue_cursor_kind kind;
  i16 angle;
  u32 pixels;
  HCURSOR cursor;
} Made;

static struct {
  Vec(Made) made;
  // the cursors' scale, the window's, 1 until it says.
  f32 scale;
  // the app's images, their size in points and their most pixels.
  bool (*image)(civue_cursor c, u32 pixels, civue_cursor_image *out);
  f32 points;
  u32 most;
} C = {.scale = 1, .points = 32, .most = 128};

void civue__cursors_use(const civue_window_options *o) {
  C.image = o->cursor_image;

  if (o->cursor_points) {
    C.points = o->cursor_points;
  }

  if (o->cursor_pixels) {
    C.most = o->cursor_pixels;
  }
}

void civue__cursors_set_scale(f32 s) {
  C.scale = s > 0 ? s : 1;
}

HICON civue__icon_of(const u8 *rgba, u32 w, u32 h, bool cursor, u32 hot_x,
                     u32 hot_y) {
  BITMAPV5HEADER bi = {
    .bV5Size = sizeof bi,
    .bV5Width = (LONG)w,
    .bV5Height = -(LONG)h,
    .bV5Planes = 1,
    .bV5BitCount = 32,
    .bV5Compression = BI_BITFIELDS,
    .bV5RedMask = 0x00ff0000,
    .bV5GreenMask = 0x0000ff00,
    .bV5BlueMask = 0x000000ff,
    .bV5AlphaMask = 0xff000000,
  };

  void *bits = nullptr;
  HDC screen = GetDC(nullptr);
  HBITMAP color = CreateDIBSection(screen, (BITMAPINFO *)&bi, DIB_RGB_COLORS,
                                   &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);

  if (!color) {
    return nullptr;
  }

  // icons take their alpha straight, the system multiplies it in.
  u8 *out = bits;

  for (usize i = 0; i < (usize)w * h; i++) {
    const u8 *p = rgba + i * 4;
    u32 a = p[3];
    u8 *q = out + i * 4;

    for (u32 k = 0; k < 3; k++) {
      u32 c = a ? MIN((p[2 - k] * 255 + a / 2) / a, 255u) : 0;
      q[k] = (u8)c;
    }

    q[3] = (u8)a;
  }

  // the mask is all clear, the alpha says what shows. its rows are words.
  usize row = ((w + 15) / 16) * 2;
  void *zero = xcalloc(row * h ? row * h : 1, 1);
  HBITMAP mask = CreateBitmap((int)w, (int)h, 1, 1, zero);
  free(zero);

  ICONINFO ii = {
    .fIcon = !cursor,
    .xHotspot = hot_x,
    .yHotspot = hot_y,
    .hbmMask = mask,
    .hbmColor = color,
  };

  HICON icon = mask ? CreateIconIndirect(&ii) : nullptr;

  DeleteObject(color);

  if (mask) {
    DeleteObject(mask);
  }

  return icon;
}

// the app's image of `c` at `pixels`, null for the system's cursors.
static HCURSOR made(civue_cursor c, u32 pixels) {
  vec_each(&C.made, m) {
    if (m->kind == c.kind && m->angle == c.angle && m->pixels == pixels) {
      return m->cursor;
    }
  }

  Made m = {c.kind, c.angle, pixels};
  civue_cursor_image img = {};

  if (C.image && C.image(c, pixels, &img)) {
    f32 k = pixels / C.points;
    u32 hx = (u32)MIN(lroundf(img.hot_x * k), (long)pixels - 1);
    u32 hy = (u32)MIN(lroundf(img.hot_y * k), (long)pixels - 1);
    m.cursor = civue__icon_of(img.rgba, pixels, pixels, true, hx, hy);
    free(img.rgba);
  }

  // the system's are remembered too, as none.
  vec_push(&C.made, m);
  return m.cursor;
}

// a resize's cursor by its angle, the four lines windows has.
static const wchar_t *resize_of(i16 angle) {
  static const wchar_t *const around[4] = {IDC_SIZEWE, IDC_SIZENWSE, IDC_SIZENS,
                                           IDC_SIZENESW};
  i32 turn = ((angle % 360) + 360) % 360;
  return around[(turn + 22) / 45 % 4];
}

// windows has no grab, zoom or copy cursors of its own, the nearest stand
// in for them.
static const wchar_t *system_of(civue_cursor c) {
  switch (c.kind) {
    case CIVUE_CURSOR_POINTER:
    case CIVUE_CURSOR_HAND:
      return IDC_HAND;
    case CIVUE_CURSOR_TEXT:
      return IDC_IBEAM;
    case CIVUE_CURSOR_RESIZE_H:
    case CIVUE_CURSOR_RESIZE_E:
    case CIVUE_CURSOR_RESIZE_W:
      return IDC_SIZEWE;
    case CIVUE_CURSOR_RESIZE_V:
      return IDC_SIZENS;
    case CIVUE_CURSOR_GRABBING:
      return IDC_SIZEALL;
    case CIVUE_CURSOR_CROSSHAIR:
      return IDC_CROSS;
    case CIVUE_CURSOR_RESIZE:
      return resize_of(c.angle);
    default:
      return IDC_ARROW;
  }
}

HCURSOR civue__cursor_of(civue_cursor c) {
  // the app's images show at the window's scale, no more than their pixels.
  u32 pixels = (u32)MIN(roundf(C.points * C.scale), (f32)C.most);
  HCURSOR own = pixels ? made(c, pixels) : nullptr;

  if (own) {
    return own;
  }

  // the system scales its own to the display.
  return LoadCursorW(nullptr, system_of(c));
}
