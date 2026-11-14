#include "wl.h"

#include <math.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-cursor.h>

// a cursor made once for each look, by kind and angle, at the pixels the
// window's scale wants.
typedef struct {
  civue_cursor_kind kind;
  i16 angle;
  u32 pixels;
  struct wl_buffer *buffer;
  u32 hot_x, hot_y;
} Made;

static struct {
  Vec(Made) made;
  // the cursors' scale, the window's display scale, 1 until it says.
  f32 scale;
  // what the pointer and the stylus show, each on a surface of its own.
  struct wl_surface *surface, *tool_surface;
  struct wp_viewport *viewport, *tool_viewport;
  struct wp_cursor_shape_device_v1 *shapes, *tool_shapes;
  struct zwp_tablet_tool_v2 *shaped_tool;
  // the system's cursors when the compositor draws no shapes itself.
  struct wl_cursor_theme *theme;
  u32 theme_pixels;
  civue_cursor now;
  bool set;
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

// a buffer of `w` by `h` premultiplied pixels in wayland's argb order.
static struct wl_buffer *shm_buffer(const u8 *rgba, u32 w, u32 h) {
  usize size = (usize)w * h * 4;
  int fd = memfd_create("civue-cursor", MFD_CLOEXEC);
  if (fd < 0) {
    return nullptr;
  }
  if (ftruncate(fd, (off_t)size) < 0) {
    close(fd);
    return nullptr;
  }
  u8 *px = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (px == MAP_FAILED) {
    close(fd);
    return nullptr;
  }
  for (usize i = 0; i < size; i += 4) {
    px[i] = rgba[i + 2];
    px[i + 1] = rgba[i + 1];
    px[i + 2] = rgba[i];
    px[i + 3] = rgba[i + 3];
  }
  munmap(px, size);
  struct wl_shm_pool *pool = wl_shm_create_pool(WL.shm, fd, (i32)size);
  struct wl_buffer *b = wl_shm_pool_create_buffer(
    pool, 0, (i32)w, (i32)h, (i32)w * 4, WL_SHM_FORMAT_ARGB8888);
  wl_shm_pool_destroy(pool);
  close(fd);
  return b;
}

// the app's image of `c` at `pixels`, nullptr for the system's cursors.
static const Made *made(civue_cursor c, u32 pixels) {
  vec_each(&C.made, m) {
    if (m->kind == c.kind && m->angle == c.angle && m->pixels == pixels) {
      return m;
    }
  }
  Made m = {c.kind, c.angle, pixels};
  civue_cursor_image img = {};
  if (C.image && C.image(c, pixels, &img)) {
    m.buffer = shm_buffer(img.rgba, pixels, pixels);
    m.hot_x = img.hot_x;
    m.hot_y = img.hot_y;
    free(img.rgba);
  }
  // system cursors are remembered too, as none.
  vec_push(&C.made, m);
  return &C.made.v[C.made.len - 1];
}

// the system's cursors, by the compositor's shape and the cursor theme's
// names, the standard one first.
typedef struct {
  enum wp_cursor_shape_device_v1_shape shape;
  const char *names[2];
} System;

static System system_of(civue_cursor_kind k) {
  switch (k) {
    case CIVUE_CURSOR_POINTER:
      return (System){WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
                      {"pointer", "hand2"}};
    case CIVUE_CURSOR_TEXT:
      return (System){WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT, {"text", "xterm"}};
    case CIVUE_CURSOR_RESIZE_H:
      return (System){WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_EW_RESIZE,
                      {"ew-resize", "sb_h_double_arrow"}};
    case CIVUE_CURSOR_RESIZE_V:
      return (System){WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NS_RESIZE,
                      {"ns-resize", "sb_v_double_arrow"}};
    case CIVUE_CURSOR_RESIZE_E:
      return (System){WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,
                      {"e-resize", "right_side"}};
    case CIVUE_CURSOR_RESIZE_W:
      return (System){WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
                      {"w-resize", "left_side"}};
    default:
      return (System){WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,
                      {"default", "left_ptr"}};
  }
}

// the theme's image of a system cursor, loaded at the pixels it shows at.
static struct wl_cursor_image *theme_image(const System *sys, u32 pixels) {
  if (C.theme && C.theme_pixels != pixels) {
    wl_cursor_theme_destroy(C.theme);
    C.theme = nullptr;
  }
  if (!C.theme) {
    const char *name = getenv("XCURSOR_THEME");
    C.theme = wl_cursor_theme_load(name, (int)pixels, WL.shm);
    C.theme_pixels = pixels;
  }
  if (!C.theme) {
    return nullptr;
  }
  for (u32 i = 0; i < 2; i++) {
    struct wl_cursor *c = wl_cursor_theme_get_cursor(C.theme, sys->names[i]);
    if (c && c->image_count) {
      return c->images[0];
    }
  }
  return nullptr;
}

// shows `b`, `pixels` wide, on `surface` at `points` wide.
static void show(struct wl_surface *surface, struct wp_viewport *viewport,
                 struct wl_buffer *b, u32 pixels, f32 points) {
  wl_surface_attach(surface, b, 0, 0);
  if (viewport) {
    wp_viewport_set_destination(viewport, (i32)lroundf(points),
                                (i32)lroundf(points));
  }
  else {
    wl_surface_set_buffer_scale(surface, (i32)MAX(lroundf(pixels / points), 1));
  }
  wl_surface_damage_buffer(surface, 0, 0, INT32_MAX, INT32_MAX);
  wl_surface_commit(surface);
}

// a surface for a device's cursor, with its viewport when there are any.
static void cursor_surface(struct wl_surface **s, struct wp_viewport **v) {
  if (*s) {
    return;
  }
  *s = wl_compositor_create_surface(WL.compositor);
  if (WL.viewporter) {
    *v = wp_viewporter_get_viewport(WL.viewporter, *s);
  }
}

// shows `b` at `points` wide for the pointer, or the stylus when `tool`.
static void attach(bool tool, struct wl_buffer *b, u32 pixels, f32 points,
                   f32 hx, f32 hy) {
  struct wl_surface **surface = tool ? &C.tool_surface : &C.surface;
  struct wp_viewport **viewport = tool ? &C.tool_viewport : &C.viewport;
  cursor_surface(surface, viewport);
  show(*surface, *viewport, b, pixels, points);
  if (tool) {
    zwp_tablet_tool_v2_set_cursor(WL.tool, WL.tool_serial, *surface,
                                  (i32)lroundf(hx), (i32)lroundf(hy));
  }
  else {
    wl_pointer_set_cursor(WL.pointer, WL.pointer_serial, *surface,
                          (i32)lroundf(hx), (i32)lroundf(hy));
  }
}

// a system cursor for the pointer, or the stylus when `tool`: the
// compositor's shape, or the cursor theme's image.
static void set_system(bool tool, System sys) {
  if (WL.cursor_shapes) {
    struct wp_cursor_shape_device_v1 **d = tool ? &C.tool_shapes : &C.shapes;
    if (tool && C.shaped_tool != WL.tool && *d) {
      wp_cursor_shape_device_v1_destroy(*d);
      *d = nullptr;
    }
    if (!*d) {
      *d = tool ? wp_cursor_shape_manager_v1_get_tablet_tool_v2(
                    WL.cursor_shapes, WL.tool)
                : wp_cursor_shape_manager_v1_get_pointer(WL.cursor_shapes,
                                                         WL.pointer);
      if (tool) {
        C.shaped_tool = WL.tool;
      }
    }
    wp_cursor_shape_device_v1_set_shape(
      *d, tool ? WL.tool_serial : WL.pointer_serial, sys.shape);
    return;
  }
  // the theme's cursors are drawn at the size it says, 24 points as a rule.
  u32 size = 24;
  const char *env = getenv("XCURSOR_SIZE");
  if (env && atoi(env) > 0) {
    size = (u32)atoi(env);
  }
  struct wl_cursor_image *img = theme_image(&sys, (u32)lroundf(size * C.scale));
  if (!img) {
    return;
  }
  attach(tool, wl_cursor_image_get_buffer(img), img->width,
         img->width / C.scale, img->hotspot_x / C.scale,
         img->hotspot_y / C.scale);
}

// sets `c` for the pointer, or the stylus when `tool`.
static void apply(civue_cursor c, bool tool) {
  // the app's images show at the window's scale, no more than their pixels.
  u32 pixels = (u32)MIN(roundf(C.points * C.scale), (f32)C.most);
  const Made *m = made(c, pixels);
  if (m->buffer) {
    attach(tool, m->buffer, pixels, C.points, m->hot_x, m->hot_y);
  }
  else {
    set_system(tool, system_of(c.kind));
  }
}

void civue__cursors_set(civue_cursor c) {
  C.now = c;
  C.set = true;
  if (WL.pointer && WL.pointer_serial) {
    apply(c, false);
  }
  if (WL.tool) {
    apply(c, true);
  }
}

void civue__cursors_set_edge(u32 edge) {
  // xdg shell's edges: top 1, bottom 2, left 4, right 8 and their sums.
  static const System edges[] = {
    [1] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE, {"n-resize", "top_side"}},
    [2] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,
           {"s-resize", "bottom_side"}},
    [4] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE, {"w-resize", "left_side"}},
    [5] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
           {"nw-resize", "top_left_corner"}},
    [6] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE,
           {"sw-resize", "bottom_left_corner"}},
    [8] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,
           {"e-resize", "right_side"}},
    [9] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE,
           {"ne-resize", "top_right_corner"}},
    [10] = {WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE,
            {"se-resize", "bottom_right_corner"}}};
  if (edge >= countof(edges) || !edges[edge].names[0]) {
    return;
  }
  if (WL.pointer && WL.pointer_serial) {
    set_system(false, edges[edge]);
  }
}

void civue__cursors_entered(void) {
  if (C.set) {
    civue__cursors_set(C.now);
  }
}
