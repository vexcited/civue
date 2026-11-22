#ifndef CIVUE_WL_H
#define CIVUE_WL_H

#include "../internal.h"

#include <civue/window.h>
#include <wayland-client.h>

#include "cursor-shape-v1-client-protocol.h"
#include "tablet-v2-client-protocol.h"
#include "viewporter-client-protocol.h"

// the wayland connection the window, the clipboard and the cursors share, all
// of it used from the main thread.
typedef struct {
  struct wl_display *display;
  struct wl_compositor *compositor;
  struct wl_shm *shm;
  struct wl_seat *seat;
  struct wl_pointer *pointer;
  struct wl_data_device_manager *data_manager;
  struct wp_viewporter *viewporter;
  struct wp_cursor_shape_manager_v1 *cursor_shapes;
  // the window's surface.
  struct wl_surface *surface;
  // the last press or key, what the clipboard and the window's moves are asked
  // with.
  u32 serial;
  // the pointer's entry into the window, what its cursor is set with.
  u32 pointer_serial;
  // the stylus over the window and its entry, null when none is.
  struct zwp_tablet_tool_v2 *tool;
  u32 tool_serial;
} Wl;

extern Wl civue__wl;
#define WL civue__wl

// whether this is the process's first thread.
bool civue__main_thread();

// files dragged over the window, and dropped on it. window.c.
void civue__drag_moved(civue_vec2 at);
void civue__drag_left();
void civue__dropped(civue_vec2 at, civue_strs *paths);

// the data device's selection and drags, once the seat is known. clipboard.c.
void civue__clipboard_seat();
// the selection's bytes of a mime type, false when it holds none, and whether
// it holds anything. from any thread.
bool civue__clip_read(const char *mime, civue_bytes *out);
bool civue__clip_any();

// the app's cursor images and the scale they are made at, the window's display
// scale. cursors.c.
void civue__cursors_use(const civue_window_options *o);
void civue__cursors_set_scale(f32 scale);
// the pointer's or the stylus's cursor again, after either entered the window.
void civue__cursors_entered();
// sets the cursor the app asks for, its image or the system's.
void civue__cursors_set(civue_cursor c);
// the system's resize cursor of an edge of the window, xdg shell's number.
void civue__cursors_set_edge(u32 edge);

// a malloc'd path of a file:// url, percent escapes undone, nullptr for other
// urls. pasteboard.c.
char *civue__file_path(const char *url, usize n);

#endif
