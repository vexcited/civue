#ifndef CIVUE_WINDOWS_WIN_H
#define CIVUE_WINDOWS_WIN_H

#include "../internal.h"

#include <civue/window.h>

#include <objbase.h>
#include <windows.h>

/* ========================================================================== */
/* what the windows backend shares between its files. the build defines      */
/* UNICODE, COBJMACROS, WIN32_LEAN_AND_MEAN and NOMINMAX, so the system's     */
/* calls take utf-16 and com is called through its C macros.                  */
/* ========================================================================== */

// `n` bytes of utf-8, or up to its nul when `n` is -1, as nul terminated
// utf-16 from malloc.
[[gnu::malloc, gnu::returns_nonnull]]
static inline wchar_t *civue__wide(const char *s, int n) {
  int len = s ? MultiByteToWideChar(CP_UTF8, 0, s, n, nullptr, 0) : 0;
  wchar_t *w = xmalloc(((usize)len + 1) * sizeof *w);
  len = s ? MultiByteToWideChar(CP_UTF8, 0, s, n, w, len) : 0;
  w[n < 0 && len > 0 ? len - 1 : len] = 0;
  return w;
}

// `n` code units of utf-16, or up to its nul when `n` is -1, as nul
// terminated utf-8 from malloc.
[[gnu::malloc, gnu::returns_nonnull]]
static inline char *civue__utf8(const wchar_t *w, int n) {
  int len =
    w ? WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr) : 0;
  char *s = xmalloc((usize)len + 1);
  len = w ? WideCharToMultiByte(CP_UTF8, 0, w, n, s, len, nullptr, nullptr) : 0;
  s[n < 0 && len > 0 ? len - 1 : len] = 0;
  return s;
}

// the window civue opened, null before it opens and after it closes.
HWND civue__hwnd();

// com on the calling thread, apartment threaded on the main one. once per
// thread, from any.
void civue__com_init();

// what webview.c gives the window to call once a frame is shown, with the
// frame's number: the web views no call showed that frame hide.
extern void (*civue__webviews_place)(u64 frame);

// whether this is the process's first thread.
bool civue__main_thread();

// the id the desktop knows the app by, the window's options' or "civue".
// window.c.
const char *civue__app_id();

// where a point of the screen, in pixels, is in the window's points.
// window.c.
civue_vec2 civue__window_point(POINT screen);

// a modal loop of the system's begins or ends, like a dialog's: the window's
// timers go through the messages it dispatches meanwhile. window.c.
void civue__modal(bool on);

// files dragged over the window, and dropped on it. window.c.
void civue__drag_moved(civue_vec2 at);
void civue__drag_left();
void civue__dropped(civue_vec2 at, civue_strs *paths);

// takes the files dragged from other apps, once ole is up. drop.c.
void civue__drop_register(HWND hwnd);

// the app's cursor images and the scale they are made at, the window's.
// cursors.c.
void civue__cursors_use(const civue_window_options *o);
void civue__cursors_set_scale(f32 scale);
// the cursor the app asks for, its image or the system's.
HCURSOR civue__cursor_of(civue_cursor c);
// an icon, or a cursor with its hotspot, of `w` by `h` premultiplied rgba8
// pixels, rows from the top. null when the system makes none.
HICON civue__icon_of(const u8 *rgba, u32 w, u32 h, bool cursor, u32 hot_x,
                     u32 hot_y);

// opens the clipboard for the window, which another app may hold a moment.
// clipboard.c.
bool civue__clipboard_open();
// png bytes as straight bgra8, rows from the top, through wic. malloc'd, null
// when they are no image. pasteboard.c.
u8 *civue__png_pixels(const u8 *png, usize n, u32 *width, u32 *height);

#endif
