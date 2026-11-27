#include "win.h"

#include <civue/system.h>

// a window of its own owns what civue copies before the app's opens, or
// with no window at all.
static HWND owner() {
  static HWND own;
  HWND h = civue__hwnd();

  if (h) {
    return h;
  }

  if (!own) {
    own = CreateWindowExW(0, L"STATIC", L"civue clipboard", 0, 0, 0, 0, 0,
                          HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr),
                          nullptr);
  }

  return own;
}

bool civue__clipboard_open() {
  // another app may hold it a moment, as it reads or writes.
  for (u32 i = 0; i < 10; i++) {
    if (OpenClipboard(owner())) {
      return true;
    }

    Sleep(5);
  }

  return false;
}

static UINT html_format() {
  static UINT f;

  if (!f) {
    f = RegisterClipboardFormatW(L"HTML Format");
  }

  return f;
}

// what the clipboard holds of `format`, a copy from malloc and its size.
static u8 *read_format(UINT format, usize *n) {
  HANDLE h = GetClipboardData(format);
  u8 *p = h ? GlobalLock(h) : nullptr;

  if (!p) {
    return nullptr;
  }

  usize size = GlobalSize(h);
  u8 *out = xmalloc(size + 2);
  memcpy(out, p, size);
  out[size] = out[size + 1] = 0;
  GlobalUnlock(h);

  *n = size;
  return out;
}

// `n` bytes for the clipboard in `format`, once it is opened and emptied.
static bool put_format(UINT format, const void *p, usize n) {
  HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
  void *to = h ? GlobalLock(h) : nullptr;

  if (!to) {
    if (h) {
      GlobalFree(h);
    }

    return false;
  }

  memcpy(to, p, n);
  GlobalUnlock(h);

  if (!SetClipboardData(format, h)) {
    GlobalFree(h);
    return false;
  }

  return true;
}

// windows' lines end in a carriage return too, civue's do not.
static char *without_returns(char *s) {
  char *to = s;

  for (const char *c = s; *c; c++) {
    if (!(c[0] == '\r' && c[1] == '\n')) {
      *to++ = *c;
    }
  }

  *to = 0;
  return s;
}

static wchar_t *with_returns(const char *s, usize n) {
  civue_bytes b = {};

  for (usize i = 0; i < n; i++) {
    if (s[i] == '\n' && (!i || s[i - 1] != '\r')) {
      buf_putc(&b, '\r');
    }

    buf_putc(&b, s[i]);
  }

  wchar_t *w = civue__wide(b.v ? (const char *)b.v : "", (int)b.len);
  vec_free(&b);
  return w;
}

char *civue_clipboard_text() {
  if (!civue__clipboard_open()) {
    return nullptr;
  }

  usize n = 0;
  wchar_t *w = (wchar_t *)read_format(CF_UNICODETEXT, &n);
  CloseClipboard();

  if (!w) {
    return nullptr;
  }

  char *s = civue__utf8(w, -1);
  free(w);
  return without_returns(s);
}

// the fragment the html of the clipboard holds, between its offsets: the
// header says where in bytes from its start.
static char *fragment_of(const char *cf, usize n) {
  const char *start = strstr(cf, "StartFragment:");
  const char *end = strstr(cf, "EndFragment:");

  if (!start || !end) {
    return nullptr;
  }

  long a = strtol(start + 14, nullptr, 10), b = strtol(end + 12, nullptr, 10);

  if (a < 0 || b < a || (usize)b > n) {
    return nullptr;
  }

  char *s = xmalloc((usize)(b - a) + 1);
  memcpy(s, cf + a, (usize)(b - a));
  s[b - a] = 0;
  return s;
}

char *civue_clipboard_html() {
  if (!civue__clipboard_open()) {
    return nullptr;
  }

  usize n = 0;
  char *cf = (char *)read_format(html_format(), &n);
  CloseClipboard();

  if (!cf) {
    return nullptr;
  }

  n = strnlen(cf, n);

  char *s = fragment_of(cf, n);
  free(cf);
  return s;
}

static bool open_empty() {
  if (!civue__clipboard_open()) {
    return false;
  }

  EmptyClipboard();
  return true;
}

static void put_text(const char *s, usize n) {
  wchar_t *w = with_returns(s, n);
  put_format(CF_UNICODETEXT, w, (wcslen(w) + 1) * sizeof *w);
  free(w);
}

void civue_clipboard_set_text(const char *s, size_t n) {
  if (!open_empty()) {
    return;
  }

  put_text(s ? s : "", s ? n : 0);
  CloseClipboard();
}

// windows' html: a header of the offsets in bytes of the html and of the
// fragment copied in it, each written in ten digits so the header's length
// is known before them.
static civue_bytes cf_html(const char *html) {
  static const char HEADER[] = "Version:0.9\r\n"
                               "StartHTML:%010zu\r\n"
                               "EndHTML:%010zu\r\n"
                               "StartFragment:%010zu\r\n"
                               "EndFragment:%010zu\r\n";
  static const char BEFORE[] = "<html><body>\r\n<!--StartFragment-->";
  static const char AFTER[] = "<!--EndFragment-->\r\n</body></html>";

  usize header =
    (usize)snprintf(nullptr, 0, HEADER, (usize)0, (usize)0, (usize)0, (usize)0);
  usize start = header + strlen(BEFORE), end = start + strlen(html);
  usize total = end + strlen(AFTER);

  civue_bytes b = {};
  buf_printf(&b, HEADER, header, total, start, end);
  buf_puts(&b, BEFORE);
  buf_puts(&b, html);
  buf_puts(&b, AFTER);
  buf_putc(&b, 0);
  return b;
}

void civue_clipboard_set_html(const char *html, const char *text) {
  if (!open_empty()) {
    return;
  }

  civue_bytes cf = cf_html(html ? html : "");
  put_format(html_format(), cf.v, cf.len);
  vec_free(&cf);

  if (text) {
    put_text(text, strlen(text));
  }

  CloseClipboard();
}

// png bytes as a bitmap of their pixels, which most apps paste: a header,
// then rows from the bottom.
static HGLOBAL dib_of(const u8 *png, usize n) {
  u32 w, h;
  u8 *bgra = civue__png_pixels(png, n, &w, &h);

  if (!bgra) {
    return nullptr;
  }

  usize size = (usize)w * h * 4;
  HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPV5HEADER) + size);
  u8 *to = g ? GlobalLock(g) : nullptr;

  if (!to) {
    if (g) {
      GlobalFree(g);
    }

    free(bgra);
    return nullptr;
  }

  *(BITMAPV5HEADER *)to = (BITMAPV5HEADER){
    .bV5Size = sizeof(BITMAPV5HEADER),
    .bV5Width = (LONG)w,
    .bV5Height = (LONG)h,
    .bV5Planes = 1,
    .bV5BitCount = 32,
    .bV5Compression = BI_BITFIELDS,
    .bV5SizeImage = (DWORD)size,
    .bV5RedMask = 0x00ff0000,
    .bV5GreenMask = 0x0000ff00,
    .bV5BlueMask = 0x000000ff,
    .bV5AlphaMask = 0xff000000,
    .bV5CSType = LCS_sRGB,
    .bV5Intent = LCS_GM_IMAGES,
  };

  u8 *rows = to + sizeof(BITMAPV5HEADER);

  for (u32 y = 0; y < h; y++) {
    memcpy(rows + (usize)(h - 1 - y) * w * 4, bgra + (usize)y * w * 4,
           (usize)w * 4);
  }

  GlobalUnlock(g);
  free(bgra);
  return g;
}

void civue_clipboard_set_png(const uint8_t *png, size_t n) {
  if (!png || !n) {
    return;
  }

  HGLOBAL dib = dib_of(png, n);

  if (!open_empty()) {
    if (dib) {
      GlobalFree(dib);
    }

    return;
  }

  // the png as it is for the apps that read it, its pixels for the rest.
  put_format(RegisterClipboardFormatW(L"PNG"), png, n);

  if (dib && !SetClipboardData(CF_DIBV5, dib)) {
    GlobalFree(dib);
  }

  CloseClipboard();
}
