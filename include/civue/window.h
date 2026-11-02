#ifndef CIVUE_WINDOW_H
#define CIVUE_WINDOW_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* the window and the main loop. frames are only built when something changed and are presented in step with */
/* the display: a display link on macos, the compositor's frame callbacks on wayland.                          */
/* ========================================================================================================== */

typedef struct {
  void *ctx;
  // builds a frame.
  void (*frame)(void *ctx, civue_ui *ui);
  // the color under the ui, a dark gray when null.
  civue_color (*background)(void *ctx);
} civue_app;

// a cursor's image the app draws, `pixels` wide and square, premultiplied rgba from malloc that civue frees, its
// hotspot in points.
typedef struct {
  uint8_t *rgba;
  uint32_t hot_x, hot_y;
} civue_cursor_image;

typedef struct {
  const char *title;
  float width, height, min_width, min_height;
  // the height of the app's own title bar, drawn in the window under a transparent one, the window's buttons
  // centered in it. 0 keeps the system's.
  float titlebar;
  // the id of the region the app's title bar registers with civue_interact: a press where it is topmost moves the
  // window on linux, a double click maximizes it. 0 for the whole height of `titlebar`.
  civue_id titlebar_id;
  // the id the desktop knows the app by, its desktop file's name on linux. "civue" when null.
  const char *app_id;
  // the app's icons, numbered from 0, see civue_icons_use.
  const civue_icon_svg *icons;
  uint32_t nicons;
  // builds the system's menu bar, once before the window opens. its items call civue_window_send_key or
  // civue_window_send_command.
  void (*install_menus)(void);
  // where the system has no menu bar, keys the app's menus take before the frame sees them, true when taken.
  bool (*menu_key)(civue_ui *ui, civue_key_press k);
  // the image of a cursor at `pixels` wide, false for the system's own. `points` wide on the screen, `pixels` at
  // most, 32 and 128 when 0.
  bool (*cursor_image)(civue_cursor c, uint32_t pixels, civue_cursor_image *out);
  float cursor_points;
  uint32_t cursor_pixels;
} civue_window_options;

// opens a window and runs the app `make` returns in it until it closes.
void civue_window_run(const civue_window_options *o, civue_app (*make)(civue_ui *ui, void *arg), void *arg);
// asks the window for a frame, from any thread.
void civue_window_wake(void);
// what an item of the system's menu bar asks for, taken by the next frame from civue_input: a key pressed as if
// typed, or a command of the app's own. from the main thread.
void civue_window_send_key(civue_key_press k);
void civue_window_send_command(uint32_t command);
// opens a web page in the system's browser.
void civue_open_url(const char *url);
// runs `fn` on the main thread and waits for it, from any thread.
void civue_on_main(void (*fn)(void *arg), void *arg);

#ifdef __linux__
// the window's handle for desktop portals, "wayland:" and an exported handle, or "" when the compositor cannot
// export it.
const char *civue_window_handle(void);
// handles the window's events while `waiting` says so, from the main thread, running what other threads ask of it
// meanwhile: a modal dialog's wait. what was pressed or typed meanwhile is dropped.
void civue_wait_while(bool (*waiting)(void *arg), void *arg);
// whether the window fills the screen's work area.
bool civue_window_maximized(void);
// brings the window to the front, a second launch's link opened in it.
void civue_window_raise(void);
// calls `ready` on the main thread when `fd` can be read, to drive another event loop from civue's, like the main
// queue of libdispatch. before civue_window_run.
void civue_watch_fd(int fd, void (*ready)(void *arg), void *arg);
#endif

#ifdef __cplusplus
}
#endif

#endif
