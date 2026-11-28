#include "win.h"

#include <civue/gfx.h>
#include <civue/raster.h>

#include <dwmapi.h>
#include <imm.h>
#include <math.h>
#include <shellapi.h>
#include <stdatomic.h>
#include <windowsx.h>

// how near the window's top a press resizes it, in points. its other edges
// are past the window, where windows keeps a frame nobody sees.
constexpr f32 EDGE = 5;
// how wide each of the window's buttons is, in points, as windows 11 has
// them.
constexpr f32 BUTTON_W = 46;
// how far apart the frames asked by civue_window_wake are once one changed
// nothing, and how far at most when they keep changing nothing.
constexpr f64 WAKE_PACE = 1.0 / 60;
constexpr f64 WAKE_PACE_MAX = 0.1;
// a wheel's notch is 120, scrolling 40 points as it does on linux at the
// system's 3 lines.
constexpr f32 NOTCH_POINTS = 40;
// a touchpad that sent part of a notch keeps scrolling precisely this long.
constexpr f64 PRECISE_FOR = 0.3;

// what other threads post to the window, the display's refresh among them,
// and the timer standing in for the window's while a modal loop runs.
constexpr UINT WM_CIVUE_WAKE = WM_APP + 0x3c1;
constexpr UINT_PTR MODAL_TIMER = 0xc1e;

static constexpr wchar_t CLASS[] = L"civue";

// a call another thread waits on.
typedef struct {
  void (*fn)(void *arg);
  void *arg;
  bool done;
} Call;

// the one window and what drives it.
static struct {
  civue_ui *ui;
  civue_app app_cb;
  civue_window_options options;
  civue_input input;
  civue_gfx *gfx;
  _Atomic(HWND) hwnd;
  HINSTANCE instance;

  // a frame is wanted, drawn once the messages are in and the display took
  // the last one.
  bool wanted;
  // the last frame shown waits for the display's refresh.
  bool presenting;
  // how long a wake waits after the last frame began, 0 for not at all.
  f64 wake_pace;
  // the last frame showed nothing new.
  bool unchanged;
  // when the frame being built began.
  f64 began;
  // a frame asked for at this time, 0 for none.
  f64 timer_at;
  // a frame is being built: what a modal loop inside it dispatches waits.
  bool building;
  // the message being handled was dispatched by civue's own loop, which
  // serves the frames once the messages are in.
  bool from_loop;
  // the system's modal loops running: a move, a resize, a menu, a dialog.
  u32 modal;
  // the window was shown, and is minimized.
  bool visible, minimized;

  // when the last press happened, where and how many in a row.
  f64 click_at;
  civue_vec2 click_pos;
  u32 clicks;
  // the leave of the client or of the frame is asked for.
  bool tracking, tracking_frame;
  // a touchpad scrolls, until then.
  f64 precise_until;

  civue_cursor cursor;
  bool cursor_set;
  HCURSOR hcursor;
  // what windows last asked the cursor for.
  u16 cursor_hit;

  // the first half of a surrogate pair typed.
  wchar_t high;
  // an input method composes, and the focused widget takes a secret.
  bool composing, secure;

  // ---- the window's state ----
  u32 dpi;
  // the client's size in pixels.
  u32 pw, ph;
  bool maximized, focused, fullscreen;
  // the window as it was before full screen.
  WINDOWPLACEMENT placement;
  LONG style;
  // the app draws the title bar, the window's buttons in it, unless hidden.
  bool custom_frame, buttons_hidden;
  // where the maximize button was drawn, empty for nowhere.
  civue_rect max_button;
  // the look the frame was last given, -1 for none yet.
  int frame_dark;
  bool system_dark, reduce_motion;
  HICON icon_big, icon_small;
  // the display's refresh rate, for when dwm does not say.
  _Atomic u32 hz;

  // ---- drawing ----
  // what the cpu draws into, `cap_w` by `cap_h` pixels, the frame shown at
  // its top left: `drawn_w` by `drawn_h` once drawn.
  HDC dc;
  HBITMAP dib, was_selected;
  u8 *pixels;
  u32 cap_w, cap_h;
  bool drawn;
  u32 drawn_w, drawn_h;
  civue_color background;

  // ---- other threads ----
  SRWLOCK lock;
  CONDITION_VARIABLE called;
  Vec(Call *) calls;
  // a wake is posted and not handled yet.
  _Atomic bool posted;
  _Atomic bool woken;
  // the display refreshed since the last frame was shown.
  _Atomic bool ticked;
  HANDLE pace_go;
  // the high resolution timer of the window's own loop.
  HANDLE deadline;
} W = {
  .lock = SRWLOCK_INIT,
  .called = CONDITION_VARIABLE_INIT,
  .frame_dark = -1,
  .hz = 60,
  .cursor_hit = HTCLIENT,
};

void (*civue__webviews_place)(u64 frame);

const char *civue__app_id() {
  return W.options.app_id ? W.options.app_id : "civue";
}

static DWORD main_thread;

[[gnu::constructor]] static void note_main_thread() {
  main_thread = GetCurrentThreadId();
}

bool civue__main_thread() {
  return GetCurrentThreadId() == main_thread;
}

HWND civue__hwnd() {
  return W.hwnd;
}

void civue__com_init() {
  static thread_local bool done;

  if (done) {
    return;
  }

  done = true;
  CoInitializeEx(nullptr, civue__main_thread() ? COINIT_APARTMENTTHREADED
                                               : COINIT_MULTITHREADED);
}

void *civue_window_hwnd() {
  return W.hwnd;
}

static void want() {
  W.wanted = true;
}

static void schedule_timer(f64 at) {
  if (!W.timer_at || at < W.timer_at) {
    W.timer_at = at;
  }
}

// ---- other threads ----

// one wake message at a time is in the queue, every loop of the thread
// dispatches it: the system's while it moves the window too.
static void post_wake() {
  if (atomic_exchange(&W.posted, true)) {
    return;
  }

  HWND h = W.hwnd;

  if (!h || !PostMessageW(h, WM_CIVUE_WAKE, 0, 0)) {
    atomic_store(&W.posted, false);
  }
}

void civue_window_wake() {
  atomic_store(&W.woken, true);
  post_wake();
}

void civue_on_main(void (*fn)(void *arg), void *arg) {
  if (civue__main_thread() || !W.hwnd) {
    fn(arg);
    return;
  }

  Call c = {fn, arg};

  AcquireSRWLockExclusive(&W.lock);
  vec_push(&W.calls, &c);
  ReleaseSRWLockExclusive(&W.lock);

  post_wake();

  AcquireSRWLockExclusive(&W.lock);

  while (!c.done) {
    SleepConditionVariableSRW(&W.called, &W.lock, INFINITE, 0);
  }

  ReleaseSRWLockExclusive(&W.lock);
}

// runs what other threads asked of the main thread.
static void run_calls() {
  for (;;) {
    AcquireSRWLockExclusive(&W.lock);
    Call *c = W.calls.len ? W.calls.v[0] : nullptr;

    if (c) {
      memmove(W.calls.v, W.calls.v + 1, (W.calls.len - 1) * sizeof *W.calls.v);
      W.calls.len--;
    }

    ReleaseSRWLockExclusive(&W.lock);

    if (!c) {
      return;
    }

    c->fn(c->arg);

    AcquireSRWLockExclusive(&W.lock);
    c->done = true;
    WakeAllConditionVariable(&W.called);
    ReleaseSRWLockExclusive(&W.lock);
  }
}

// a frame asked from outside the window. the display's refresh paces the
// frames it is shown, but a frame that changed nothing waits for none: an
// app told a hundred times a second of what its window does not show would
// build as many frames. those come a display's frame apart, then further
// while they keep changing nothing.
static void woke() {
  f64 due = W.began + W.wake_pace;

  if (W.wake_pace > 0 && now_s() < due) {
    schedule_timer(due);
  }
  else {
    want();
  }
}

// what the wake message carries: calls, the display's refresh, wakes.
static void wake_up() {
  atomic_store(&W.posted, false);
  run_calls();

  if (atomic_exchange(&W.ticked, false)) {
    W.presenting = false;
  }

  if (atomic_exchange(&W.woken, false)) {
    woke();
  }
}

void civue_window_send_key(civue_key_press k) {
  vec_push(&W.input.keys, k);
  want();
}

void civue_window_send_command(u32 command) {
  vec_push(&W.input.commands, command);
  want();
}

// ---- the display's refresh ----

// how many qpc ticks the window's display refreshes in, and when it last did.
static void refresh_timing(i64 freq, i64 now, i64 *period, i64 *vblank) {
  u32 hz = atomic_load(&W.hz);
  i64 own = freq / (hz ? hz : 60);
  DWM_TIMING_INFO t = {.cbSize = sizeof t};

  *period = own;
  *vblank = now;

  if (FAILED(DwmGetCompositionTimingInfo(nullptr, &t)) ||
      t.qpcRefreshPeriod < (u64)freq / 500 ||
      t.qpcRefreshPeriod > (u64)freq / 20) {
    return;
  }

  // dwm composes at its fastest display's rate, the window's display may
  // be slower.
  *period = MAX((i64)t.qpcRefreshPeriod, own - own / 10);
  *vblank = (i64)t.qpcVBlank;
}

// how long until the display next refreshes, in 100 nanoseconds.
static i64 until_refresh() {
  LARGE_INTEGER freq, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&now);

  i64 period, vblank;
  refresh_timing(freq.QuadPart, now.QuadPart, &period, &vblank);

  i64 since = now.QuadPart - vblank;
  i64 next = since < 0 ? vblank : vblank + (since / period + 1) * period;
  return (next - now.QuadPart) * 10'000'000 / freq.QuadPart;
}

// waits for the refresh after each frame shown, then tells the window.
static DWORD WINAPI pace(void *arg) {
  HANDLE timer = CreateWaitableTimerExW(
    nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

  if (!timer) {
    timer = CreateWaitableTimerW(nullptr, false, nullptr);
  }

  for (;;) {
    WaitForSingleObject(W.pace_go, INFINITE);

    // half a millisecond at least, the frame landing in this refresh.
    LARGE_INTEGER due = {.QuadPart = -MAX(until_refresh(), 5'000)};

    if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, false)) {
      WaitForSingleObject(timer, INFINITE);
    }
    else {
      Sleep(1);
    }

    atomic_store(&W.ticked, true);
    post_wake();
  }

  return 0;
}

// the refresh rate of the display the window is on.
static void read_refresh() {
  MONITORINFOEXW mi = {.cbSize = sizeof mi};
  DEVMODEW mode = {.dmSize = sizeof mode};
  HMONITOR m = MonitorFromWindow(W.hwnd, MONITOR_DEFAULTTONEAREST);

  if (GetMonitorInfoW(m, (MONITORINFO *)&mi) &&
      EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &mode) &&
      mode.dmDisplayFrequency > 1) {
    atomic_store(&W.hz, mode.dmDisplayFrequency);
  }
}

// ---- scale and frame ----

static f32 scale_factor() {
  return (W.dpi ? W.dpi : 96) / 96.0f;
}

static u32 dpi_of(HWND h) {
  u32 dpi = h ? GetDpiForWindow(h) : 0;
  return dpi ? dpi : 96;
}

// the only children are webviews, which draw through directcomposition
// above the window. they are not clipped out of it, so what shows through a
// transparent page is the window's own pixels under it.
static DWORD style_of() {
  return WS_OVERLAPPEDWINDOW;
}

// how thick the frame windows resizes a window from is, in pixels.
static i32 frame_x(u32 dpi) {
  return GetSystemMetricsForDpi(SM_CXFRAME, dpi) +
         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

static i32 frame_y(u32 dpi) {
  return GetSystemMetricsForDpi(SM_CYFRAME, dpi) +
         GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
}

// the window's rect around a client of `w` by `h` pixels at `dpi`. the app's
// title bar takes the system's place, up to the window's top.
static RECT around_client(i32 w, i32 h, u32 dpi) {
  RECT r = {0, 0, w, h};
  AdjustWindowRectExForDpi(&r, style_of(), false, 0, dpi);

  if (W.custom_frame) {
    r.top = 0;
  }

  return r;
}

civue_vec2 civue__window_point(POINT screen) {
  ScreenToClient(W.hwnd, &screen);

  f32 s = scale_factor();
  return v2(screen.x / s, screen.y / s);
}

static civue_vec2 client_point(LPARAM lp) {
  f32 s = scale_factor();
  return v2(GET_X_LPARAM(lp) / s, GET_Y_LPARAM(lp) / s);
}

// ---- the system's look ----

// windows' "animation effects", off for less motion.
static bool read_reduce_motion() {
  BOOL on = TRUE;
  SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
  return !on;
}

static bool read_dark() {
  DWORD light = 1, n = sizeof light;
  LSTATUS r = RegGetValueW(
    HKEY_CURRENT_USER,
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
    L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &n);
  return r == ERROR_SUCCESS && !light;
}

// the frame, its border and the system's title bar, light or dark as the
// app is.
static void frame_look(bool dark) {
  if (W.frame_dark == (int)dark) {
    return;
  }

  W.frame_dark = dark;

  BOOL on = dark;
  DwmSetWindowAttribute(W.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &on, sizeof on);
}

// the first of the app's icons at `pixels`, null when it has none or it
// can't be drawn.
static HICON app_icon(u32 pixels) {
  if (!W.options.icons || !W.options.nicons || !pixels) {
    return nullptr;
  }

  const civue_icon_svg *svg = W.options.icons;
  civue_bytes rgba = {};
  HICON icon = nullptr;

  if (civue_svg_rasterize(svg->svg, svg->len, pixels, pixels, &rgba) &&
      rgba.len >= (usize)pixels * pixels * 4) {
    icon = civue__icon_of(rgba.v, pixels, pixels, false, 0, 0);
  }

  vec_free(&rgba);
  return icon;
}

// the window's icons at its display's sizes, the program's own when the
// app gives none.
static void set_icons() {
  HICON big = app_icon((u32)GetSystemMetricsForDpi(SM_CXICON, W.dpi));
  HICON small = app_icon((u32)GetSystemMetricsForDpi(SM_CXSMICON, W.dpi));

  if (!big || !small) {
    if (big) {
      DestroyIcon(big);
    }

    if (small) {
      DestroyIcon(small);
    }

    return;
  }

  SendMessageW(W.hwnd, WM_SETICON, ICON_BIG, (LPARAM)big);
  SendMessageW(W.hwnd, WM_SETICON, ICON_SMALL, (LPARAM)small);

  if (W.icon_big) {
    DestroyIcon(W.icon_big);
  }

  if (W.icon_small) {
    DestroyIcon(W.icon_small);
  }

  W.icon_big = big;
  W.icon_small = small;
}

static BOOL CALLBACK first_icon(HMODULE m, LPCWSTR type, LPWSTR name,
                                LONG_PTR out) {
  WNDCLASSEXW *wc = (WNDCLASSEXW *)out;
  wc->hIcon = LoadImageW(m, name, IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
  wc->hIconSm = LoadImageW(m, name, IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                           GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
  return false;
}

// ---- drawing ----

// what the cpu draws a `pw` by `ph` frame into, false when it was made anew
// and holds no frame. it grows by steps, as a resize goes.
static bool fit_dib(u32 pw, u32 ph) {
  bool fits = W.dib && pw <= W.cap_w && ph <= W.cap_h;

  // much larger than the window, it shrinks.
  if (fits && (u64)W.cap_w * W.cap_h <= 4 * (u64)MAX(pw, 64) * MAX(ph, 64)) {
    return true;
  }

  if (W.dib) {
    SelectObject(W.dc, W.was_selected);
    DeleteObject(W.dib);
    W.dib = nullptr;
  }

  if (!W.dc) {
    W.dc = CreateCompatibleDC(nullptr);
  }

  u32 cw = (pw + 127) & ~127u, ch = (ph + 127) & ~127u;

  // rows from the top, blue first, as civue_raster draws them.
  BITMAPINFO bi = {.bmiHeader = {
                     .biSize = sizeof bi.bmiHeader,
                     .biWidth = (LONG)cw,
                     .biHeight = -(LONG)ch,
                     .biPlanes = 1,
                     .biBitCount = 32,
                     .biCompression = BI_RGB,
                   }};

  void *bits = nullptr;
  W.dib = CreateDIBSection(W.dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);

  if (!W.dib) {
    die("no memory for %u by %u pixels", cw, ch);
  }

  W.was_selected = SelectObject(W.dc, W.dib);
  W.pixels = bits;
  W.cap_w = cw;
  W.cap_h = ch;
  W.drawn = false;
  return false;
}

static bool find_pixels(void *ctx, civue_texture t, civue_pixels *out) {
  return civue_gfx_pixels(ctx, t, out);
}

static void blit_box(void *ctx, civue_box b) {
  BitBlt(ctx, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0, W.dc, b.x0, b.y0, SRCCOPY);
}

// draws what changed of the frame the ui built on the cpu, then copies the
// boxes it drew to the window.
static void present(civue_color background, u32 pw, u32 ph) {
  civue_damage changes;
  const civue_draw_list *list =
    civue_frame_changes(W.ui, background, pw, ph, &changes);

  bool kept = fit_dib(pw, ph);

  // an image the app registered may have been drawn into since.
  if (!kept || !civue_gfx_static(W.gfx, list) || !W.drawn || W.drawn_w != pw ||
      W.drawn_h != ph) {
    changes = (civue_damage){.all = true};
  }

  W.unchanged = !changes.all && !changes.any;

  if (W.unchanged) {
    return;
  }

  civue_surface s = {W.pixels, pw, ph, (usize)W.cap_w * 4};
  civue_raster_images images = {find_pixels, W.gfx};
  civue_raster(&s, list, background, &changes, civue_gfx_atlas(W.gfx),
               civue_gfx_color_atlas(W.gfx), &images);

  W.drawn = true;
  W.drawn_w = pw;
  W.drawn_h = ph;
  W.background = background;

  HDC dc = GetDC(W.hwnd);

  if (changes.all) {
    BitBlt(dc, 0, 0, (int)pw, (int)ph, W.dc, 0, 0, SRCCOPY);
  }
  else {
    civue_damage_each(&changes, pw, ph, blit_box, dc);
  }

  ReleaseDC(W.hwnd, dc);

  W.presenting = true;
  SetEvent(W.pace_go);
}

// the window's paint from the frame drawn, what lies past it in the
// background until the next frame.
static void paint(HWND h) {
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(h, &ps);
  RECT r = ps.rcPaint;
  civue_color c = W.background;
  HBRUSH ground = CreateSolidBrush(RGB(
    (u8)lroundf(c.r * 255), (u8)lroundf(c.g * 255), (u8)lroundf(c.b * 255)));

  if (W.drawn) {
    i32 x1 = MIN(r.right, (i32)W.drawn_w), y1 = MIN(r.bottom, (i32)W.drawn_h);

    if (x1 > r.left && y1 > r.top) {
      BitBlt(dc, r.left, r.top, x1 - r.left, y1 - r.top, W.dc, r.left, r.top,
             SRCCOPY);
    }

    RECT right = {MAX(r.left, (LONG)W.drawn_w), r.top, r.right, r.bottom};
    RECT below = {r.left, MAX(r.top, (LONG)W.drawn_h), r.right, r.bottom};

    if (right.left < right.right) {
      FillRect(dc, &right, ground);
    }

    if (below.top < below.bottom) {
      FillRect(dc, &below, ground);
    }
  }
  else {
    FillRect(dc, &r, ground);
  }

  DeleteObject(ground);
  EndPaint(h, &ps);

  if (!W.drawn || W.drawn_w != W.pw || W.drawn_h != W.ph) {
    want();
  }
}

// ---- the window's buttons ----

static bool buttons_shown() {
  return W.custom_frame && !W.fullscreen && !W.buttons_hidden;
}

void civue_window_buttons_hidden(bool hidden) {
  if (W.buttons_hidden == hidden) {
    return;
  }

  W.buttons_hidden = hidden;
  W.max_button = (civue_rect){};
  want();
}

// a button's glyph, ten points square around `c`, its lines whole pixels.
static void glyph(civue_ui *ui, civue_title_button b, civue_vec2 c,
                  civue_color color) {
  f64 s = ui->scale;
  f32 w = (f32)(fmax(round(s), 1) / s);
  f64 x0 = round((c.x - 5) * s) / s, y0 = round((c.y - 5) * s) / s;
  f64 x1 = x0 + 10, y1 = y0 + 10;

  switch (b) {
    case CIVUE_TITLE_MINIMIZE: {
      f64 y = round(c.y * s) / s;
      civue_fill(ui, rect(x0, y, x1, y + w), color, 0);
      break;
    }
    case CIVUE_TITLE_MAXIMIZE:
      if (!W.maximized) {
        civue_stroke(ui, rect(x0, y0, x1, y1), color, w, 1);
        break;
      }

      // restoring: a square, the corner of another behind it.
      civue_stroke(ui, rect(x0, y0 + 2, x1 - 2, y1), color, w, 1);
      civue_fill(ui, rect(x0 + 2, y0, x1, y0 + w), color, 0);
      civue_fill(ui, rect(x1 - w, y0, x1, y1 - 2), color, 0);
      break;
    case CIVUE_TITLE_CLOSE:
      civue_line(ui, v2(x0, y0), v2(x1, y1), w, color);
      civue_line(ui, v2(x1, y0), v2(x0, y1), w, color);
      break;
    default:
      break;
  }
}

// minimize, maximize and close at the end of the app's title bar, drawn as
// windows 11 draws its own over everything but the overlays. the maximize
// button is the system's to the pointer, for its snap layouts: its input
// comes in through the frame's messages.
static void window_buttons(civue_ui *ui) {
  W.max_button = (civue_rect){};

  if (!buttons_shown()) {
    return;
  }

  static const civue_title_button order[] = {
    CIVUE_TITLE_MINIMIZE, CIVUE_TITLE_MAXIMIZE, CIVUE_TITLE_CLOSE};

  bool dark = ui->dark;
  f64 h = W.options.titlebar, end = ui->size.x;
  civue_color fg = dark ? CIVUE_WHITE : color_hex(0x1a1a1a);

  if (!W.focused) {
    fg = color_alpha(fg, dark ? 0.45f : 0.4f);
  }

  for (u32 i = 0; i < countof(order); i++) {
    civue_title_button b = order[i];
    civue_rect r =
      rect(end - (3 - i) * BUTTON_W, 0, end - (2 - i) * BUTTON_W, h);
    civue_id id = civue_idn(civue_id_of("civue-window-buttons"), b);
    civue_response res = civue_interact(ui, id, r, CIVUE_SENSE_CLICK);
    bool held = res.dragging && res.hovered;
    civue_color bg = CIVUE_CLEAR, ink = fg;

    if (b == CIVUE_TITLE_CLOSE && (res.hovered || held)) {
      bg = held ? color_alpha(color_hex(0xc42b1c), 0.9f) : color_hex(0xc42b1c);
      ink = held ? color_alpha(CIVUE_WHITE, 0.7f) : CIVUE_WHITE;
    }
    else if (held) {
      bg = dark ? color_alpha(CIVUE_WHITE, 0.04f)
                : color_alpha(CIVUE_BLACK, 0.024f);
    }
    else if (res.hovered) {
      bg = dark ? color_alpha(CIVUE_WHITE, 0.06f)
                : color_alpha(CIVUE_BLACK, 0.037f);
    }

    civue_fill(ui, r, bg, 0);
    glyph(ui, b, rect_center(r), ink);

    if (b == CIVUE_TITLE_MAXIMIZE) {
      W.max_button = r;
    }

    if (!res.clicked) {
      continue;
    }

    if (b == CIVUE_TITLE_MINIMIZE) {
      ui->minimize_window = true;
    }
    else if (b == CIVUE_TITLE_MAXIMIZE) {
      ui->toggle_maximize = true;
    }
    else {
      ui->close_window = true;
    }
  }
}

// ---- the window's frame ----

// where the app's title bar takes the pointer and nothing is over it, or
// its whole height when it names no region.
static bool on_title_bar(civue_vec2 at) {
  if (!W.custom_frame || W.fullscreen || !W.ui || at.y >= W.options.titlebar) {
    return false;
  }

  civue_id id = civue_region_at(W.ui, at);
  return W.options.titlebar_id ? id == W.options.titlebar_id : !id;
}

// what windows does with the pointer where the app draws the title bar: it
// resizes from the window's top, moves it from the title bar, maximizes it
// on a double click there and snaps it, and shows its layouts over the
// maximize button. the other edges are past the client, the system's.
static LRESULT hit_test(HWND h, WPARAM wp, LPARAM lp) {
  LRESULT hit = DefWindowProcW(h, WM_NCHITTEST, wp, lp);

  if (hit != HTCLIENT || !W.custom_frame || W.fullscreen) {
    return hit;
  }

  POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  ScreenToClient(h, &p);

  f32 s = scale_factor();
  civue_vec2 at = v2(p.x / s, p.y / s);

  if (!W.maximized && at.y < EDGE) {
    i32 corner = frame_x(W.dpi);
    return p.x < corner                ? HTTOPLEFT
           : p.x >= (i32)W.pw - corner ? HTTOPRIGHT
                                       : HTTOP;
  }

  if (rect_contains(W.max_button, at)) {
    return HTMAXBUTTON;
  }

  return on_title_bar(at) ? HTCAPTION : HTCLIENT;
}

// the client goes up to the window's top, the frame's sides and bottom stay
// for the system to resize from and to draw its round corners and shadow
// around. a maximized window goes past the screen by its frame.
static LRESULT calc_size(HWND h, WPARAM wp, LPARAM lp) {
  NCCALCSIZE_PARAMS *p = (NCCALCSIZE_PARAMS *)lp;
  LONG top = p->rgrc[0].top;
  LRESULT r = DefWindowProcW(h, WM_NCCALCSIZE, wp, lp);

  if (r) {
    return r;
  }

  p->rgrc[0].top = top;

  if (IsZoomed(h) && !W.fullscreen) {
    p->rgrc[0].top += frame_y(dpi_of(h));
  }

  return 0;
}

static void toggle_fullscreen() {
  HWND h = W.hwnd;

  if (!W.fullscreen) {
    W.placement = (WINDOWPLACEMENT){.length = sizeof W.placement};
    GetWindowPlacement(h, &W.placement);
    W.style = GetWindowLongW(h, GWL_STYLE);

    MONITORINFO mi = {.cbSize = sizeof mi};
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);

    RECT m = mi.rcMonitor;
    W.fullscreen = true;
    SetWindowLongW(h, GWL_STYLE, (W.style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
    SetWindowPos(h, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top,
                 SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
  }
  else {
    W.fullscreen = false;
    SetWindowLongW(h, GWL_STYLE, W.style);
    SetWindowPlacement(h, &W.placement);
    SetWindowPos(h, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER |
                   SWP_FRAMECHANGED);
  }

  want();
}

// ---- input ----

static u8 mods_now() {
  // altgr is a control and an alt windows makes up, it types.
  bool altgr = GetKeyState(VK_RMENU) < 0 && GetKeyState(VK_LCONTROL) < 0;
  u8 mods = 0;

  if (GetKeyState(VK_SHIFT) < 0) {
    mods |= CIVUE_MOD_SHIFT;
  }

  // like most apps on windows, control is the command key. the windows key
  // stands in for the mac's control.
  if (GetKeyState(VK_CONTROL) < 0 && !altgr) {
    mods |= CIVUE_MOD_CMD;
  }

  if (GetKeyState(VK_MENU) < 0 && !altgr) {
    mods |= CIVUE_MOD_ALT;
  }

  if (GetKeyState(VK_LWIN) < 0 || GetKeyState(VK_RWIN) < 0) {
    mods |= CIVUE_MOD_CTRL;
  }

  return mods;
}

static void moved(civue_vec2 at) {
  W.input.has_mouse = true;
  W.input.mouse = at;
  W.input.pressure = -1;
  vec_push(&W.input.trail, ((civue_trail_point){at, -1}));
  W.input.mods = mods_now();
  want();
}

static bool any_down() {
  return W.input.down[0] || W.input.down[1] || W.input.down[2];
}

static void released_all() {
  for (u32 i = 0; i < countof(W.input.down); i++) {
    if (W.input.down[i]) {
      W.input.down[i] = false;
      W.input.released[i] = true;
    }
  }

  if (GetCapture() == W.hwnd) {
    ReleaseCapture();
  }

  want();
}

// a press or release of a button at `at`, the mouse held by the window
// while one is down.
static void button(u32 i, civue_vec2 at, bool down) {
  moved(at);

  if (!down) {
    // a release the app never saw pressed is the frame's.
    if (!W.input.down[i]) {
      return;
    }

    W.input.down[i] = false;
    W.input.released[i] = true;

    if (!any_down() && GetCapture() == W.hwnd) {
      ReleaseCapture();
    }

    return;
  }

  if (i == 0) {
    f32 s = scale_factor();
    f64 near_x = GetSystemMetricsForDpi(SM_CXDOUBLECLK, W.dpi) / 2.0 / s;
    f64 near_y = GetSystemMetricsForDpi(SM_CYDOUBLECLK, W.dpi) / 2.0 / s;
    bool close_by = fabs(at.x - W.click_pos.x) <= near_x &&
                    fabs(at.y - W.click_pos.y) <= near_y;
    f64 t = now_s();

    W.clicks = close_by && t - W.click_at < GetDoubleClickTime() / 1000.0
                 ? W.clicks + 1
                 : 1;
    W.click_at = t;
    W.click_pos = at;
    W.input.clicks = W.clicks;
  }

  if (!any_down()) {
    SetCapture(W.hwnd);
  }

  W.input.down[i] = true;
  W.input.pressed[i] = true;
}

static void track(bool frame) {
  TRACKMOUSEEVENT t = {
    .cbSize = sizeof t,
    .dwFlags = TME_LEAVE | (frame ? TME_NONCLIENT : 0),
    .hwndTrack = W.hwnd,
  };

  TrackMouseEvent(&t);
  W.tracking = !frame;
  W.tracking_frame = frame;
}

// whether a point of the screen is over the window's client, not over what
// is above it: a web view's window, another app's.
static bool over_client(POINT p) {
  RECT r;
  GetClientRect(W.hwnd, &r);
  MapWindowPoints(W.hwnd, nullptr, (POINT *)&r, 2);
  return PtInRect(&r, p) && WindowFromPoint(p) == W.hwnd;
}

// the pointer left the client or the frame, maybe for the other one, the
// title bar or the maximize button, that are over the client still.
static void pointer_left() {
  POINT p;
  GetCursorPos(&p);

  if (over_client(p)) {
    return;
  }

  if (!any_down()) {
    W.input.has_mouse = false;
  }

  want();
}

// a move over the frame where it is over the client: the app's title bar,
// its top edge, the maximize button.
static void frame_moved(LPARAM lp) {
  POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};

  if (!over_client(p)) {
    return;
  }

  if (!W.tracking_frame) {
    track(true);
  }

  moved(civue__window_point(p));
}

static void wheel(i32 delta, bool across) {
  // a wheel sends whole notches, a precision touchpad parts of them as the
  // fingers move.
  f64 t = now_s();

  if (delta % WHEEL_DELTA) {
    W.precise_until = t + PRECISE_FOR;
  }

  bool precise = t < W.precise_until;
  UINT lines = 3;
  SystemParametersInfoW(
    across ? SPI_GETWHEELSCROLLCHARS : SPI_GETWHEELSCROLLLINES, 0, &lines, 0);

  if (!lines || lines == WHEEL_PAGESCROLL) {
    lines = 3;
  }

  u8 mods = mods_now();
  W.input.mods = mods;

  // a touchpad's pinch comes as control and the wheel.
  if (precise && !across && mods & CIVUE_MOD_CMD) {
    W.input.pinch += delta / (f32)WHEEL_DELTA * 0.2f;
    want();
    return;
  }

  f32 v = delta / (f32)WHEEL_DELTA * lines / 3 * NOTCH_POINTS;

  if (across) {
    W.input.scroll.x -= v;
  }
  else {
    W.input.scroll.y += v;
  }

  W.input.precise_scroll |= precise;
  want();
}

void civue__drag_moved(civue_vec2 at) {
  W.input.drop_at = at;
  W.input.drag_over = true;
  want();
}

void civue__drag_left() {
  W.input.drag_over = false;
  want();
}

void civue__dropped(civue_vec2 at, civue_strs *paths) {
  W.input.drop_at = at;
  W.input.drag_over = false;

  vec_each(paths, p) {
    vec_push(&W.input.drops, *p);
  }

  paths->len = 0;
  want();
}

// ---- the keyboard ----

static u32 special_key(WPARAM vk) {
  switch (vk) {
    case VK_RETURN:
      return CIVUE_KEY_ENTER;
    case VK_ESCAPE:
      return CIVUE_KEY_ESCAPE;
    case VK_TAB:
      return CIVUE_KEY_TAB;
    case VK_BACK:
      return CIVUE_KEY_BACKSPACE;
    case VK_DELETE:
      return CIVUE_KEY_DELETE;
    case VK_LEFT:
      return CIVUE_KEY_LEFT;
    case VK_RIGHT:
      return CIVUE_KEY_RIGHT;
    case VK_UP:
      return CIVUE_KEY_UP;
    case VK_DOWN:
      return CIVUE_KEY_DOWN;
    case VK_HOME:
      return CIVUE_KEY_HOME;
    case VK_END:
      return CIVUE_KEY_END;
    case VK_PRIOR:
      return CIVUE_KEY_PAGE_UP;
    case VK_NEXT:
      return CIVUE_KEY_PAGE_DOWN;
    case VK_SPACE:
      return CIVUE_KEY_SPACE;
  }

  if (vk >= VK_F1 && vk <= VK_F12) {
    return CIVUE_KEY_F1 + (u32)(vk - VK_F1);
  }

  return CIVUE_KEY_NONE;
}

// the key a message stands for, by the key itself rather than what shift
// or altgr make of it on the layout.
static u32 key_of(WPARAM vk, LPARAM lp) {
  u32 k = special_key(vk);

  if (k) {
    return k;
  }

  // the digit row gives its digit on any layout, shortcuts need them.
  u32 scan = (u32)(lp >> 16) & 0xff;
  bool extended = lp & (1 << 24);

  if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
    return '0' + (u32)(vk - VK_NUMPAD0);
  }

  if (!extended && scan >= 0x02 && scan <= 0x0b) {
    return scan == 0x0b ? '0' : '1' + (scan - 0x02);
  }

  // the layout's character for the key alone, a dead key's too.
  u32 c = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;

  if (c < 0x20 || c == 0x7f) {
    return CIVUE_KEY_NONE;
  }

  return (u32)(uintptr_t)CharLowerW((LPWSTR)(uintptr_t)c) & 0xffff;
}

static void key_down(WPARAM vk, LPARAM lp) {
  u8 mods = mods_now();
  W.input.mods = mods;
  want();

  // what the input method takes.
  if (vk == VK_PROCESSKEY) {
    return;
  }

  u32 key = key_of(vk, lp);

  if (!key) {
    return;
  }

  bool held = false;

  vec_each(&W.input.held, h) {
    held |= *h == key;
  }

  if (!held) {
    vec_push(&W.input.held, key);
  }

  // the menus' keys go to them first, like the system's menu bar takes its
  // key equivalents.
  civue_key_press k = {key, mods};

  if (!W.options.menu_key || !W.options.menu_key(W.ui, k)) {
    vec_push(&W.input.keys, k);
  }
}

static void key_up(WPARAM vk, LPARAM lp) {
  W.input.mods = mods_now();
  want();

  u32 key = key_of(vk, lp), k = 0;

  vec_each(&W.input.held, h) {
    if (*h != key) {
      W.input.held.v[k++] = *h;
    }
  }

  W.input.held.len = k;
}

static void put_text(const char *s) {
  for (const char *c = s; *c; c++) {
    if ((u8)*c >= 0x20 && *c != 0x7f) {
      vec_push(&W.input.text, (u8)*c);
    }
  }
}

// a code unit typed, the halves of a pair put together.
static void typed(wchar_t u) {
  if (IS_HIGH_SURROGATE(u)) {
    W.high = u;
    return;
  }

  u32 cp = u;

  if (IS_LOW_SURROGATE(u)) {
    if (!W.high) {
      return;
    }

    cp = 0x10000 + ((u32)(W.high - 0xd800) << 10) + (u - 0xdc00);
  }

  W.high = 0;

  // keys say what control characters do, and a shortcut takes its text.
  if (cp < 0x20 || cp == 0x7f ||
      W.input.mods & (CIVUE_MOD_CMD | CIVUE_MOD_CTRL)) {
    return;
  }

  char s[5] = {};
  utf8_put(s, cp);
  put_text(s);
  W.input.preedit.len = 0;
  want();
}

// ---- input methods ----

// the input method's candidates open under the caret, or the pointer
// without one.
static void place_candidates() {
  HIMC imc = ImmGetContext(W.hwnd);

  if (!imc) {
    return;
  }

  f32 s = scale_factor();
  civue_rect c = W.ui ? W.ui->caret : (civue_rect){};

  if (rect_empty(c)) {
    civue_vec2 m = W.input.mouse;
    c = rect_xywh(m.x, m.y, 1, 20);
  }

  RECT r = {lround(c.x0 * s), lround(c.y0 * s), lround(c.x1 * s),
            lround(c.y1 * s)};

  COMPOSITIONFORM cf = {.dwStyle = CFS_POINT, .ptCurrentPos = {r.left, r.top}};
  ImmSetCompositionWindow(imc, &cf);

  CANDIDATEFORM cand = {
    .dwIndex = 0,
    .dwStyle = CFS_EXCLUDE,
    .ptCurrentPos = {r.left, r.bottom},
    .rcArea = r,
  };

  ImmSetCandidateWindow(imc, &cand);
  ImmReleaseContext(W.hwnd, imc);
}

static char *composition(HIMC imc, DWORD kind) {
  LONG bytes = ImmGetCompositionStringW(imc, kind, nullptr, 0);

  if (bytes <= 0) {
    return nullptr;
  }

  wchar_t *w = xmalloc((usize)bytes + sizeof *w);
  ImmGetCompositionStringW(imc, kind, w, (DWORD)bytes);

  char *s = civue__utf8(w, bytes / (LONG)sizeof *w);
  free(w);
  return s;
}

// what the input method composed and committed, the composition shown as
// the preedit and the committed text typed.
static void composed(LPARAM lp) {
  HIMC imc = ImmGetContext(W.hwnd);

  if (!imc) {
    return;
  }

  if (lp & GCS_RESULTSTR) {
    char *s = composition(imc, GCS_RESULTSTR);

    if (s) {
      put_text(s);
      free(s);
    }
  }

  W.input.preedit.len = 0;

  if (lp & GCS_COMPSTR) {
    char *s = composition(imc, GCS_COMPSTR);

    if (s) {
      buf_puts(&W.input.preedit, s);
      free(s);
    }
  }

  ImmReleaseContext(W.hwnd, imc);
  place_candidates();
  want();
}

// a password's keys skip the input method, which would show and keep them.
static void set_secure(bool on) {
  if (W.secure == on) {
    return;
  }

  W.secure = on;
  W.input.preedit.len = 0;
  ImmAssociateContextEx(W.hwnd, nullptr, on ? 0 : IACE_DEFAULT);
}

// ---- frames ----

static void update_cursor() {
  civue_cursor c = W.ui->cursor;

  if (W.cursor_set && c.kind == W.cursor.kind && c.angle == W.cursor.angle) {
    return;
  }

  W.cursor = c;
  W.cursor_set = true;
  W.hcursor = civue__cursor_of(c);

  if (W.cursor_hit == HTCLIENT && W.input.has_mouse) {
    SetCursor(W.hcursor);
  }
}

// a move of the window from the press the app saw on its title bar, which
// windows goes on with and snaps to the screen's edges.
static void start_move() {
  if (!W.input.down[0]) {
    return;
  }

  POINT p;
  GetCursorPos(&p);
  released_all();
  SendMessageW(W.hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(p.x, p.y));
  released_all();
}

static void frame() {
  W.wanted = false;
  W.began = now_s();
  W.building = true;

  u32 pw = MAX(W.pw, 1), ph = MAX(W.ph, 1);
  f32 scale = scale_factor();
  civue_ui *ui = W.ui;
  civue_color background = W.app_cb.background
                             ? W.app_cb.background(W.app_cb.ctx)
                             : color_hex(0x1e1e1e);

  if (W.options.titlebar > 0) {
    bool bar = W.custom_frame && !W.fullscreen;
    ui->titlebar_inset = 0;
    // kept while the app hides the buttons, as macos keeps its own.
    ui->titlebar_end_inset = bar ? 3 * BUTTON_W : 0;
    ui->titlebar_height = bar ? W.options.titlebar : 0;
    ui->window_controls = false;
    ui->window_maximized = W.maximized;
  }

  ui->system_dark = W.system_dark;
  ui->reduce_motion = W.reduce_motion;
  ui->window_focused = W.focused;
  ui->screen_known = true;
  ui->screen = rect(0, 0, pw / scale, ph / scale);

  // what a pass asks of the window is asked once, the passes after it begin
  // without it and without the input that made it.
  bool drag = false, maximize = false, minimize = false, closing = false,
       repaint = false;
  f64 repaint_at = 0;

  for (u32 pass = 0; pass < 4; pass++) {
    civue_begin_frame(ui, v2(pw / scale, ph / scale), scale, &W.input);
    W.app_cb.frame(W.app_cb.ctx, ui);
    window_buttons(ui);

    bool settled = civue_end_frame(ui);
    drag |= ui->drag_window;
    maximize |= ui->toggle_maximize;
    minimize |= ui->minimize_window;
    closing |= ui->close_window;
    repaint |= ui->repaint;

    if (ui->repaint_at && (!repaint_at || ui->repaint_at < repaint_at)) {
      repaint_at = ui->repaint_at;
    }

    if (settled) {
      break;
    }
  }

  ui->drag_window = drag;
  ui->toggle_maximize = maximize;
  ui->minimize_window = minimize;
  ui->close_window = closing;
  ui->repaint = repaint;
  ui->repaint_at = repaint_at;
  // a frame that never settled is shown as it is, the next one is new.
  ui->settled = true;

  // by the last pass alone, the one the window shows.
  if (civue__webviews_place) {
    civue__webviews_place(ui->frames);
  }

  present(background, pw, ph);
  W.building = false;

  W.wake_pace =
    W.unchanged ? MIN(MAX(W.wake_pace * 2, WAKE_PACE), WAKE_PACE_MAX) : 0;

  frame_look(ui->dark);
  update_cursor();
  set_secure(ui->secure_input);

  if (W.composing) {
    place_candidates();
  }

  // a frame asked again after one that changed nothing waits a display
  // frame's time, no refresh comes for it.
  if (repaint && W.unchanged) {
    schedule_timer(now_s() + 1.0 / atomic_load(&W.hz));
  }
  else if (repaint) {
    want();
  }

  if (repaint_at) {
    schedule_timer(repaint_at);
  }

  // what the frame asks of the window, once it is done with it: a modal
  // loop may draw the next frames meanwhile.
  bool fullscreen = ui->toggle_fullscreen;
  ui->toggle_fullscreen = false;
  HWND h = W.hwnd;

  if (fullscreen) {
    toggle_fullscreen();
  }

  if (maximize && !W.fullscreen) {
    ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
  }

  if (minimize) {
    ShowWindow(h, SW_MINIMIZE);
  }

  if (closing) {
    PostMessageW(h, WM_CLOSE, 0, 0);
  }

  if (drag && !W.fullscreen) {
    start_move();
  }
}

// whether a frame can be shown now: the window is up, and the display took
// the last one.
static bool drawable() {
  return W.visible && !W.minimized && !W.presenting;
}

// while a modal loop runs, the timer is one of its messages.
static void arm_modal_timer() {
  if (!W.modal) {
    return;
  }

  if (!W.timer_at) {
    KillTimer(W.hwnd, MODAL_TIMER);
    return;
  }

  f64 ms = ceil((W.timer_at - now_s()) * 1000);
  SetTimer(W.hwnd, MODAL_TIMER, (UINT)CLAMP(ms, USER_TIMER_MINIMUM, 1e6),
           nullptr);
}

// what is due: the timer, a frame wanted.
static void serve() {
  if (W.building || !W.ui || !W.hwnd) {
    return;
  }

  if (W.timer_at && now_s() >= W.timer_at) {
    W.timer_at = 0;
    want();
  }

  if (W.wanted && drawable()) {
    frame();
  }

  arm_modal_timer();
}

void civue__modal(bool on) {
  if (on) {
    W.modal++;
    arm_modal_timer();
    return;
  }

  if (W.modal && !--W.modal && W.hwnd) {
    KillTimer(W.hwnd, MODAL_TIMER);
  }
}

static void scale_changed() {
  civue__cursors_set_scale(scale_factor());
  W.cursor_set = false;
  set_icons();
  want();
}

// ---- the window's messages ----

static void sized(HWND h, WPARAM wp, LPARAM lp) {
  W.minimized = wp == SIZE_MINIMIZED;

  if (W.minimized) {
    return;
  }

  W.maximized = wp == SIZE_MAXIMIZED;

  u32 pw = LOWORD(lp), ph = HIWORD(lp);

  if (pw == W.pw && ph == W.ph) {
    want();
    return;
  }

  W.pw = pw;
  W.ph = ph;
  want();
  // a resize draws each size as it comes, not a refresh late.
  W.presenting = false;
}

static LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp, bool from_loop) {
  switch (msg) {
    case WM_CIVUE_WAKE:
      wake_up();

      if (!from_loop) {
        serve();
      }

      return 0;

    case WM_TIMER:
      if (wp != MODAL_TIMER) {
        break;
      }

      KillTimer(h, MODAL_TIMER);

      if (!from_loop) {
        serve();
      }

      return 0;

    case WM_NCCALCSIZE:
      if (!wp || !W.custom_frame) {
        break;
      }

      return calc_size(h, wp, lp);

    case WM_NCHITTEST:
      return hit_test(h, wp, lp);

    // the frame is not drawn by the system where the app draws it.
    case WM_NCACTIVATE:
      if (!W.custom_frame) {
        break;
      }

      return DefWindowProcW(h, msg, wp, -1);

    case WM_ACTIVATE:
      W.focused = LOWORD(wp) != WA_INACTIVE;
      want();
      break;

    case WM_SETFOCUS:
      want();
      return 0;

    case WM_KILLFOCUS:
      W.input.held.len = 0;
      W.input.mods = 0;
      W.high = 0;
      want();
      return 0;

    case WM_SIZE:
      sized(h, wp, lp);

      if (!from_loop) {
        serve();
      }

      return 0;

    case WM_SHOWWINDOW:
      W.visible = wp;
      want();
      break;

    case WM_GETMINMAXINFO: {
      MINMAXINFO *m = (MINMAXINFO *)lp;
      u32 dpi = dpi_of(h);
      f32 s = dpi / 96.0f;
      RECT r = around_client((i32)lroundf(W.options.min_width * s),
                             (i32)lroundf(W.options.min_height * s), dpi);
      m->ptMinTrackSize = (POINT){r.right - r.left, r.bottom - r.top};
      return 0;
    }

    // the client keeps its size in points on the other display.
    case WM_GETDPISCALEDSIZE: {
      u32 dpi = (u32)wp, was = dpi_of(h);
      SIZE *size = (SIZE *)lp;
      RECT c;
      GetClientRect(h, &c);

      f64 k = (f64)dpi / was;
      RECT r =
        around_client((i32)lround(c.right * k), (i32)lround(c.bottom * k), dpi);
      *size = (SIZE){r.right - r.left, r.bottom - r.top};
      return true;
    }

    case WM_DPICHANGED: {
      W.dpi = HIWORD(wp);

      RECT *r = (RECT *)lp;
      SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left,
                   r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
      scale_changed();
      read_refresh();
      return 0;
    }

    case WM_DISPLAYCHANGE:
      read_refresh();
      want();
      break;

    case WM_SETTINGCHANGE:
      if (lp && !wcscmp((const wchar_t *)lp, L"ImmersiveColorSet")) {
        W.system_dark = read_dark();
        want();
      }

      if (wp == SPI_SETCLIENTAREAANIMATION) {
        W.reduce_motion = read_reduce_motion();
        want();
      }

      break;

    case WM_ENTERSIZEMOVE:
    case WM_ENTERMENULOOP:
      civue__modal(true);
      break;

    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
      civue__modal(false);
      released_all();
      read_refresh();
      break;

    case WM_ERASEBKGND:
      return 1;

    case WM_PAINT:
      paint(h);

      if (!from_loop) {
        serve();
      }

      return 0;

    case WM_SETCURSOR:
      W.cursor_hit = LOWORD(lp);

      if ((HWND)wp == h && LOWORD(lp) == HTCLIENT) {
        SetCursor(W.hcursor ? W.hcursor : LoadCursorW(nullptr, IDC_ARROW));
        return true;
      }

      break;

    case WM_MOUSEMOVE:
      if (!W.tracking) {
        track(false);
      }

      moved(client_point(lp));
      return 0;

    case WM_MOUSELEAVE:
      W.tracking = false;
      pointer_left();
      return 0;

    case WM_NCMOUSEMOVE:
      frame_moved(lp);

      if (wp == HTMAXBUTTON) {
        return 0;
      }

      break;

    case WM_NCMOUSELEAVE:
      W.tracking_frame = false;
      pointer_left();
      break;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
      button(0, client_point(lp), msg == WM_LBUTTONDOWN);
      return 0;

    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      button(1, client_point(lp), msg == WM_RBUTTONDOWN);
      return 0;

    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
      button(2, client_point(lp), msg == WM_MBUTTONDOWN);
      return 0;

    // the back and forward buttons go nowhere, as on linux.
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
      return true;

    // the maximize button is the app's to press.
    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK:
    case WM_NCLBUTTONUP:
      if (wp != HTMAXBUTTON) {
        break;
      }

      button(0,
             civue__window_point((POINT){GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}),
             msg != WM_NCLBUTTONUP);
      return 0;

    case WM_CAPTURECHANGED:
      if ((HWND)lp != h) {
        released_all();
      }

      return 0;

    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
      wheel(GET_WHEEL_DELTA_WPARAM(wp), msg == WM_MOUSEHWHEEL);
      return 0;

    case WM_KEYDOWN:
      key_down(wp, lp);
      return 0;

    case WM_KEYUP:
      key_up(wp, lp);
      return 0;

    // alt and f10 alone would take the keyboard into the window's menu,
    // which the loop would wait in. alt and f4 closes, alt and space opens
    // the window's menu.
    case WM_SYSKEYDOWN:
      key_down(wp, lp);

      if (wp == VK_F4 || wp == VK_SPACE) {
        break;
      }

      return 0;

    case WM_SYSKEYUP:
      key_up(wp, lp);
      return 0;

    case WM_SYSCHAR:
      if (wp == ' ') {
        break;
      }

      return 0;

    case WM_SYSCOMMAND:
      if ((wp & 0xfff0) == SC_KEYMENU && !lp) {
        return 0;
      }

      break;

    case WM_CHAR:
      typed((wchar_t)wp);
      return 0;

    case WM_UNICHAR:
      if (wp == UNICODE_NOCHAR) {
        return true;
      }

      if (wp > 0xffff) {
        typed((wchar_t)(0xd800 + ((wp - 0x10000) >> 10)));
        typed((wchar_t)(0xdc00 + ((wp - 0x10000) & 0x3ff)));
      }
      else {
        typed((wchar_t)wp);
      }

      return 0;

    // civue shows what is composed in the text, the system's window would
    // cover it.
    case WM_IME_SETCONTEXT:
      lp &= ~ISC_SHOWUICOMPOSITIONWINDOW;
      break;

    case WM_IME_STARTCOMPOSITION:
      W.composing = true;
      place_candidates();
      return 0;

    case WM_IME_COMPOSITION:
      composed(lp);
      return 0;

    case WM_IME_ENDCOMPOSITION:
      W.composing = false;
      W.input.preedit.len = 0;
      want();
      return 0;

    // the committed text came with the composition already.
    case WM_IME_CHAR:
      return 0;

    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }

  return DefWindowProcW(h, msg, wp, lp);
}

static LRESULT CALLBACK window_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
  // the messages a modal loop dispatches inside this one are not the loop's.
  bool from_loop = W.from_loop;
  W.from_loop = false;

  if (msg == WM_NCCREATE) {
    W.hwnd = h;
  }

  return handle(h, msg, wp, lp, from_loop);
}

// ---- running ----

// handles what is in the queue, every window's of the thread: the app's own
// and the web views' too. false once the app quits.
static bool drain(int *code) {
  MSG m;

  while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
    if (m.message == WM_QUIT) {
      *code = (int)m.wParam;
      return false;
    }

    TranslateMessage(&m);
    W.from_loop = true;
    DispatchMessageW(&m);
    W.from_loop = false;
  }

  return true;
}

void civue_wait_while(bool (*waiting)(void *arg), void *arg) {
  int code = 0;

  while (waiting(arg)) {
    MsgWaitForMultipleObjectsEx(0, nullptr, 20, QS_ALLINPUT,
                                MWMO_INPUTAVAILABLE);

    // the loop that waits on the app quits, once this wait is over.
    if (!drain(&code)) {
      PostQuitMessage(code);
      break;
    }
  }

  // what was pressed or typed while the dialog was up was the dialog's.
  memset(W.input.pressed, 0, sizeof W.input.pressed);
  memset(W.input.released, 0, sizeof W.input.released);
  memset(W.input.down, 0, sizeof W.input.down);
  W.input.keys.len = 0;
  W.input.text.len = 0;
}

static void open_window(const civue_window_options *o) {
  W.instance = GetModuleHandleW(nullptr);

  WNDCLASSEXW wc = {
    .cbSize = sizeof wc,
    .lpfnWndProc = window_proc,
    .hInstance = W.instance,
    .lpszClassName = CLASS,
  };

  EnumResourceNamesW(W.instance, RT_GROUP_ICON, first_icon, (LONG_PTR)&wc);

  if (!RegisterClassExW(&wc)) {
    die("windows: no window class");
  }

  // the window opens on the display the pointer is on, in its middle.
  POINT pointer;
  GetCursorPos(&pointer);

  MONITORINFO mi = {.cbSize = sizeof mi};
  GetMonitorInfoW(MonitorFromPoint(pointer, MONITOR_DEFAULTTOPRIMARY), &mi);

  RECT work = mi.rcWork;
  wchar_t *title = civue__wide(o->title ? o->title : civue__app_id(), -1);
  HWND h = CreateWindowExW(0, CLASS, title, style_of(), work.left, work.top,
                           100, 100, nullptr, nullptr, W.instance, nullptr);
  free(title);

  if (!h) {
    die("windows: no window");
  }

  W.hwnd = h;
  W.dpi = dpi_of(h);

  f32 s = scale_factor();
  RECT r = around_client((i32)lroundf(o->width * s),
                         (i32)lroundf(o->height * s), W.dpi);
  i32 work_w = work.right - work.left, work_h = work.bottom - work.top;
  i32 ww = MIN(r.right - r.left, work_w), wh = MIN(r.bottom - r.top, work_h);

  SetWindowPos(h, nullptr, work.left + (work_w - ww) / 2,
               work.top + (work_h - wh) / 2, ww, wh,
               SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);

  RECT c;
  GetClientRect(h, &c);
  W.pw = (u32)c.right;
  W.ph = (u32)c.bottom;
}

// shows the window with its first frame drawn, cloaked until it is so the
// system shows no empty window first.
static void show_window() {
  HWND h = W.hwnd;
  STARTUPINFOW si = {.cb = sizeof si};
  GetStartupInfoW(&si);

  int show = si.dwFlags & STARTF_USESHOWWINDOW ? si.wShowWindow : SW_SHOWNORMAL;

  if (show == SW_HIDE) {
    show = SW_SHOWNORMAL;
  }

  BOOL cloak = true;
  DwmSetWindowAttribute(h, DWMWA_CLOAK, &cloak, sizeof cloak);
  ShowWindow(h, show);
  UpdateWindow(h);

  cloak = false;
  DwmSetWindowAttribute(h, DWMWA_CLOAK, &cloak, sizeof cloak);
  W.visible = true;
}

void civue_window_run_sized(const civue_window_options *o,
                            civue_app (*make)(civue_ui *ui, void *arg),
                            void *arg, size_t ui_size) {
  W.options = *o;
  W.input.pressure = -1;
  W.custom_frame = o->titlebar > 0;

  // the program's manifest may say nothing of the displays' scales.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  OleInitialize(nullptr);
  civue__com_init();

  if (o->title) {
    civue__gpu_title(o->title);
  }

  W.system_dark = read_dark();
  W.reduce_motion = read_reduce_motion();
  W.pace_go = CreateEventW(nullptr, false, false, nullptr);
  W.deadline = CreateWaitableTimerExW(
    nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

  if (!W.deadline) {
    W.deadline = CreateWaitableTimerW(nullptr, false, nullptr);
  }

  open_window(o);
  read_refresh();
  set_icons();
  civue__drop_register(W.hwnd);

  if (o->install_menus) {
    o->install_menus();
  }

  W.gfx = civue_gfx_new(nullptr);
  W.ui = civue_ui_new_sized(W.gfx, scale_factor(), ui_size);

  if (o->icons) {
    civue_icons_use(W.ui->icons, o->icons, o->nicons);
  }

  civue__cursors_use(o);
  civue__cursors_set_scale(scale_factor());
  W.focused = true;
  W.app_cb = make(W.ui, arg);

  HANDLE pacer = CreateThread(nullptr, 0, pace, nullptr, 0, nullptr);

  if (pacer) {
    CloseHandle(pacer);
  }

  // the first frame is drawn before the window shows.
  W.visible = true;
  frame();
  W.visible = false;
  show_window();
  // what other threads asked while the window opened.
  run_calls();

  int code = 0;

  for (;;) {
    serve();

    if (W.timer_at) {
      i64 due = (i64)fmax((W.timer_at - now_s()) * 1e7, 0);
      SetWaitableTimer(W.deadline, &(LARGE_INTEGER){.QuadPart = -due}, 0,
                       nullptr, nullptr, false);
    }
    else {
      CancelWaitableTimer(W.deadline);
    }

    MsgWaitForMultipleObjectsEx(W.deadline ? 1 : 0, &W.deadline, INFINITE,
                                QS_ALLINPUT, MWMO_INPUTAVAILABLE);

    if (!drain(&code)) {
      exit(code);
    }
  }
}

// ---- what the app asks of the window ----

bool civue_window_maximized() {
  return W.hwnd && IsZoomed(W.hwnd);
}

static void raise_on_main(void *arg) {
  HWND h = W.hwnd;

  if (!h) {
    return;
  }

  ShowWindow(h, IsIconic(h) ? SW_RESTORE : SW_SHOW);

  // windows only lets the app the user works in, or the one it allowed,
  // take the foreground: the taskbar flashes the window otherwise.
  if (!SetForegroundWindow(h) && GetForegroundWindow() != h) {
    FLASHWINFO f = {
      .cbSize = sizeof f,
      .hwnd = h,
      .dwFlags = FLASHW_TRAY | FLASHW_TIMERNOFG,
    };

    FlashWindowEx(&f);
  }
}

void civue_window_raise() {
  civue_on_main(raise_on_main, nullptr);
}

void civue_open_url(const char *url) {
  if (!url || !*url) {
    return;
  }

  civue__com_init();

  wchar_t *w = civue__wide(url, -1);
  HINSTANCE r =
    ShellExecuteW(W.hwnd, L"open", w, nullptr, nullptr, SW_SHOWNORMAL);

  if ((INT_PTR)r <= 32) {
    LOG("could not open %s: %d", url, (int)(INT_PTR)r);
  }

  free(w);
}
