#include "wl.h"

#include <civue/gfx.h>

#include "fractional-scale-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-foreign-unstable-v2-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <gio/gio.h>
#include <math.h>
#include <poll.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <unistd.h>
#include <vulkan/vulkan_wayland.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

// frames worked on at once: the cpu builds one while the gpu draws the last.
constexpr u32 FRAMES = 2;
// how near the edge of the window a press resizes it, in points.
constexpr f32 EDGE = 5;

// linux's codes of the buttons and keys the window reads, apart from its
// header whose names are the app's own keys'.
enum : u32 {
  EV_1 = 2,
  EV_0 = 11,
  EV_KP7 = 71,
  EV_KP8,
  EV_KP9,
  EV_KP4 = 75,
  EV_KP5,
  EV_KP6,
  EV_KP1 = 79,
  EV_KP2,
  EV_KP3,
  EV_KP0,
  BTN_LEFT = 0x110,
  BTN_RIGHT,
  BTN_MIDDLE,
  BTN_STYLUS = 0x14b,
};

Wl WL;

// a call another thread waits on.
typedef struct {
  void (*fn)(void *arg);
  void *arg;
  bool done;
} Call;

// what another loop asks to be called with.
typedef struct {
  int fd;
  void (*ready)(void *arg);
  void *arg;
} Watch;

// an output's scale, for compositors that say nothing of the surface's.
typedef struct {
  struct wl_output *output;
  i32 scale;
  bool on;
} Output;

// the one window and what drives it.
static struct {
  civue_ui *ui;
  civue_app app_cb;
  civue_window_options options;
  civue_input input;
  civue_gfx *gfx;
  // a frame is wanted, drawn once the events are in.
  bool wanted;
  // a frame asked for at this time, 0 for none.
  f64 timer_at;
  // when the last press happened, where and how many in a row.
  f64 click_at;
  civue_vec2 click_pos;
  u32 clicks;
  civue_cursor cursor;
  bool cursor_set;
  // the resize edge the cursor shows, 0 for none.
  u32 cursor_edge;

  // ---- wayland ----
  struct xdg_wm_base *wm;
  struct xdg_surface *xdg;
  struct xdg_toplevel *toplevel;
  struct zxdg_decoration_manager_v1 *decorations;
  struct zxdg_toplevel_decoration_v1 *decoration;
  struct wp_fractional_scale_manager_v1 *fractions;
  struct wp_fractional_scale_v1 *fraction;
  struct wp_viewport *viewport;
  struct zwp_pointer_gestures_v1 *gestures;
  struct zwp_pointer_gesture_pinch_v1 *pinch;
  f64 pinch_scale;
  struct zwp_tablet_manager_v2 *tablets;
  struct zwp_tablet_seat_v2 *tablet_seat;
  struct zwp_text_input_manager_v3 *text_inputs;
  struct zwp_text_input_v3 *text_input;
  bool text_on;
  // what the input method sends, applied at its done.
  char *preedit_next, *commit_next;
  civue_rect candidates;
  struct xdg_activation_v1 *activation;
  struct zxdg_exporter_v2 *exporter;
  struct zxdg_exported_v2 *exported;
  char *handle;
  struct wl_keyboard *keyboard;
  Vec(Output) outputs;
  // the last frame's callback, nullptr once the compositor wants another.
  struct wl_callback *frame_cb;

  // ---- the keyboard ----
  struct xkb_context *xkb;
  struct xkb_keymap *keymap;
  struct xkb_state *state;
  struct xkb_compose_state *compose;
  xkb_mod_index_t mod_shift, mod_ctrl, mod_alt, mod_logo;
  u8 mods;
  i32 repeat_rate, repeat_delay;
  // the key repeating and when it next does, 0 for none.
  u32 repeat_key;
  f64 repeat_at;

  // ---- the window's state ----
  // its size in points, and the size it had when last floating.
  i32 width, height, floating_w, floating_h;
  // what the compositor configured, applied at the configure's end.
  i32 next_w, next_h;
  bool next_maximized, next_fullscreen, next_activated, next_tiled,
    next_suspended;
  bool configured, maximized, fullscreen, activated, tiled, suspended;
  // a configure was acked and waits for a frame of its size.
  bool reconfigured;
  // the largest size the compositor would like, 0 for no limit.
  i32 bounds_w, bounds_h;
  // what the window manager can do, all of it until it says.
  bool caps_known, can_minimize, can_maximize, can_menu;
  // 120ths of the scale the compositor prefers, or its whole scale.
  u32 scale120;
  i32 buffer_scale;
  bool preferred_scale;
  // the pointer's position and its scroll since the last frame.
  civue_vec2 pointer;
  f64 axis[2], axis120[2];
  i32 discrete[2];
  bool has_120, has_discrete;
  u32 axis_source;
  // the stylus's position, pressure, and its tip on the surface.
  civue_vec2 tool_at;
  f32 tool_pressure;
  bool tool_moved, tool_down, tool_up;

  // ---- drawing ----
  // the swapchain and its images, remade when `stale`.
  VkSurfaceKHR surface;
  VkSwapchainKHR swapchain;
  VkFormat format;
  VkExtent2D extent;
  u32 nimages;
  civue_vk_tex **images;
  VkSemaphore *rendered;
  VkSemaphore acquired[FRAMES];
  u64 frame_value[FRAMES];
  u32 frame;
  bool stale;
  // the window's own title bar, drawn by the app: the frame is the app's.
  bool custom_frame;
  // the compositor draws the frame, it would not leave it to the app.
  bool server_frame;
  // the system's buttons and where they go.
  civue_title_button start[4], end[4];
  // the swapchain blends with what is under the window, so its corners can
  // be round like the system's windows, `radius` points.
  bool translucent;
  f32 radius;
  // the corners' mask, made at the scale it was asked for.
  civue_texture corner;
  u32 corner_pixels;
  civue_draw_list corners;

  // ---- other threads ----
  pthread_mutex_t lock;
  Vec(Call *) calls;
  pthread_cond_t called;
  Vec(Watch) watches;
  // the desktop's look changed: its dark mode, and its buttons' layout.
  _Atomic bool dark;
  char *layout_next;
} W = {.lock = PTHREAD_MUTEX_INITIALIZER,
       .called = PTHREAD_COND_INITIALIZER,
       .buffer_scale = 1};

// the app id the desktop file and the compositor know the window by.
static const char *app_id(void) {
  return W.options.app_id ? W.options.app_id : "civue";
}

bool civue__main_thread(void) {
  return gettid() == getpid();
}

// wakes the loop from any thread, -1 before the window runs.
static int wake_fd = -1;
static _Atomic bool woken;

static void want(void) {
  W.wanted = true;
}

void civue_window_wake(void) {
  woken = true;
  if (wake_fd >= 0) {
    eventfd_write(wake_fd, 1);
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

void civue_on_main(void (*fn)(void *arg), void *arg) {
  if (civue__main_thread() || wake_fd < 0) {
    fn(arg);
    return;
  }
  Call c = {fn, arg};
  pthread_mutex_lock(&W.lock);
  vec_push(&W.calls, &c);
  pthread_mutex_unlock(&W.lock);
  eventfd_write(wake_fd, 1);
  pthread_mutex_lock(&W.lock);
  while (!c.done) {
    pthread_cond_wait(&W.called, &W.lock);
  }
  pthread_mutex_unlock(&W.lock);
}

// runs what other threads asked of the main thread.
static void run_calls(void) {
  for (;;) {
    pthread_mutex_lock(&W.lock);
    Call *c = W.calls.len ? W.calls.v[0] : nullptr;
    if (c) {
      memmove(W.calls.v, W.calls.v + 1, (W.calls.len - 1) * sizeof *W.calls.v);
      W.calls.len--;
    }
    pthread_mutex_unlock(&W.lock);
    if (!c) {
      return;
    }
    c->fn(c->arg);
    pthread_mutex_lock(&W.lock);
    c->done = true;
    pthread_cond_broadcast(&W.called);
    pthread_mutex_unlock(&W.lock);
  }
}

bool civue_window_maximized(void) {
  return W.maximized;
}

const char *civue_window_handle(void) {
  return W.handle ? W.handle : "";
}

// ---- scale ----

// physical pixels per point.
static f32 scale_factor(void) {
  if (W.scale120) {
    return W.scale120 / 120.0f;
  }
  return (f32)MAX(W.buffer_scale, 1);
}

// the pixels the window's buffers have, its points at its scale.
static void pixel_size(u32 *pw, u32 *ph) {
  if (W.scale120) {
    *pw = (u32)MAX(lround(W.width * W.scale120 / 120.0), 1);
    *ph = (u32)MAX(lround(W.height * W.scale120 / 120.0), 1);
  }
  else {
    *pw = (u32)MAX(W.width * W.buffer_scale, 1);
    *ph = (u32)MAX(W.height * W.buffer_scale, 1);
  }
}

static void scale_changed(void) {
  W.stale = true;
  civue__cursors_set_scale(scale_factor());
  W.cursor_set = false;
  want();
}

// the largest scale of the outputs the window shows on.
static void outputs_scale(void) {
  if (W.preferred_scale || W.scale120) {
    return;
  }
  i32 s = 0;
  vec_each(&W.outputs, o) {
    if (o->on) {
      s = MAX(s, o->scale);
    }
  }
  if (s < 1) {
    s = W.buffer_scale;
  }
  if (s == W.buffer_scale) {
    return;
  }
  W.buffer_scale = s;
  scale_changed();
}

// ---- the swapchain ----

static void drop_swapchain(void) {
  const civue_vk_device *v = civue_vk();
  // the frames that drew into the images are done, the canvas's work goes
  // on.
  for (u32 i = 0; i < FRAMES; i++) {
    civue_vk_wait(W.frame_value[i]);
  }
  for (u32 i = 0; i < W.nimages; i++) {
    vkDestroyImageView(v->device, W.images[i]->view, nullptr);
    vkDestroySemaphore(v->device, W.rendered[i], nullptr);
    free(W.images[i]);
  }
  free(W.images);
  free(W.rendered);
  W.images = nullptr;
  W.rendered = nullptr;
  W.nimages = 0;
}

// makes the swapchain at the window's pixel size.
static void make_swapchain(void) {
  const civue_vk_device *v = civue_vk();
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v->physical, W.surface, &caps);
  u32 pw, ph;
  pixel_size(&pw, &ph);
  VkExtent2D extent = caps.currentExtent;
  if (extent.width == UINT32_MAX) {
    extent = (VkExtent2D){pw, ph};
  }
  extent.width =
    CLAMP(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
  extent.height = CLAMP(extent.height, caps.minImageExtent.height,
                        caps.maxImageExtent.height);
  if (!W.format) {
    u32 n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v->physical, W.surface, &n, nullptr);
    VkSurfaceFormatKHR *formats = xcalloc(n ? n : 1, sizeof *formats);
    vkGetPhysicalDeviceSurfaceFormatsKHR(v->physical, W.surface, &n, formats);
    // blending happens on srgb encoded values like browsers do, and the
    // surface shows them as srgb.
    for (u32 i = 0; i < n && !W.format; i++) {
      if ((formats[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
           formats[i].format == VK_FORMAT_R8G8B8A8_UNORM) &&
          formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
        W.format = formats[i].format;
      }
    }
    if (!W.format) {
      W.format = n ? formats[0].format : VK_FORMAT_B8G8R8A8_UNORM;
    }
    free(formats);
  }
  // frames are paced by the compositor's frame callbacks, mailbox never
  // blocks on a window the compositor stopped showing.
  VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
  u32 n = 0;
  vkGetPhysicalDeviceSurfacePresentModesKHR(v->physical, W.surface, &n,
                                            nullptr);
  VkPresentModeKHR *modes = xcalloc(n ? n : 1, sizeof *modes);
  vkGetPhysicalDeviceSurfacePresentModesKHR(v->physical, W.surface, &n, modes);
  for (u32 i = 0; i < n; i++) {
    if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
      mode = modes[i];
    }
  }
  free(modes);
  u32 count = MAX(caps.minImageCount + 1, 3u);
  if (caps.maxImageCount) {
    count = MIN(count, caps.maxImageCount);
  }
  // a window with round corners shows what is under them.
  VkCompositeAlphaFlagsKHR has = caps.supportedCompositeAlpha;
  VkCompositeAlphaFlagBitsKHR alpha =
    W.radius > 0 && has & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
      ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
    : has & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
      ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
      : (VkCompositeAlphaFlagBitsKHR)(has & -has);
  W.translucent = alpha == VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
  VkSwapchainKHR old = W.swapchain;
  if (old) {
    drop_swapchain();
  }
  VkSwapchainCreateInfoKHR info = {
    .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
    .surface = W.surface,
    .minImageCount = count,
    .imageFormat = W.format,
    .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
    .imageExtent = extent,
    .imageArrayLayers = 1,
    .imageUsage =
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
    .preTransform = caps.currentTransform,
    .compositeAlpha = alpha,
    .presentMode = mode,
    .clipped = true,
    .oldSwapchain = old};
  if (vkCreateSwapchainKHR(v->device, &info, nullptr, &W.swapchain) !=
      VK_SUCCESS) {
    die("vulkan: no swapchain");
  }
  if (old) {
    vkDestroySwapchainKHR(v->device, old, nullptr);
  }
  W.extent = extent;
  vkGetSwapchainImagesKHR(v->device, W.swapchain, &W.nimages, nullptr);
  VkImage *images = xcalloc(W.nimages, sizeof *images);
  vkGetSwapchainImagesKHR(v->device, W.swapchain, &W.nimages, images);
  W.images = xcalloc(W.nimages, sizeof *W.images);
  W.rendered = xcalloc(W.nimages, sizeof *W.rendered);
  VkSemaphoreCreateInfo sem = {.sType =
                                 VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (u32 i = 0; i < W.nimages; i++) {
    VkImageViewCreateInfo view = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = images[i],
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = W.format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkImageView iv;
    vkCreateImageView(v->device, &view, nullptr, &iv);
    W.images[i] =
      civue_gfx_vk_wrap(images[i], iv, W.format, extent.width, extent.height);
    vkCreateSemaphore(v->device, &sem, nullptr, &W.rendered[i]);
  }
  free(images);
  W.stale = false;
}

// ---- the system's look ----

static GDBusConnection *bus;

static void connect_bus(void) {
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

static GDBusConnection *session_bus(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, connect_bus);
  return bus;
}

// the value inside however many variants hold it.
static GVariant *unwrap(GVariant *v) {
  while (v && g_variant_is_of_type(v, G_VARIANT_TYPE_VARIANT)) {
    GVariant *inner = g_variant_get_variant(v);
    g_variant_unref(v);
    v = inner;
  }
  return v;
}

// a setting of the desktop portal, nullptr when there is none: gnome's, and
// those other desktops give the same.
static GVariant *portal_setting(const char *ns, const char *key) {
  GDBusConnection *bus = session_bus();
  if (!bus) {
    return nullptr;
  }
  // ReadOne answers the value, Read of older portals a variant of it.
  const char *methods[] = {"ReadOne", "Read"};
  for (u32 m = 0; m < 2; m++) {
    GVariant *r = g_dbus_connection_call_sync(
      bus, "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.Settings", methods[m],
      g_variant_new("(ss)", ns, key), G_VARIANT_TYPE("(v)"),
      G_DBUS_CALL_FLAGS_NONE, 500, nullptr, nullptr);
    if (!r) {
      continue;
    }
    GVariant *v = nullptr;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    if ((v = unwrap(v))) {
      return v;
    }
  }
  return nullptr;
}

// a string of `v` as a malloc'd copy, nullptr for other values. frees `v`.
static char *take_string(GVariant *v) {
  char *out = v && g_variant_is_of_type(v, G_VARIANT_TYPE_STRING)
                ? xstrdup(g_variant_get_string(v, nullptr))
                : nullptr;
  if (v) {
    g_variant_unref(v);
  }
  return out;
}

// whether the desktop prefers dark windows. frees `v`.
static bool take_dark(GVariant *v) {
  bool dark = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) &&
              g_variant_get_uint32(v) == 1;
  if (v) {
    g_variant_unref(v);
  }
  return dark;
}

// gtk's decoration layout from its settings files, malloc'd, nullptr when
// they say none.
static char *gtk_layout(void) {
  const char *files[] = {".config/gtk-4.0/settings.ini",
                         ".config/gtk-3.0/settings.ini"};
  for (u32 i = 0; i < countof(files); i++) {
    char *path = path_join(home_dir(), files[i]);
    civue_bytes b = {};
    bool read = file_read(path, &b);
    free(path);
    if (!read) {
      continue;
    }
    buf_putc(&b, 0);
    const char *at = strstr((char *)b.v, "gtk-decoration-layout");
    char *out = nullptr;
    if (at && (at = strchr(at, '='))) {
      at++;
      while (*at == ' ') {
        at++;
      }
      out = xstrndup(at, strcspn(at, "\r\n"));
    }
    vec_free(&b);
    if (out) {
      return out;
    }
  }
  return nullptr;
}

// the buttons of a side of "appmenu:minimize,maximize,close" the window
// manager can act on.
static void layout_side(const char *s, usize n, civue_title_button out[4]) {
  memset(out, 0, 4 * sizeof *out);
  u32 k = 0;
  for (usize i = 0; i < n && k < 4;) {
    usize len = strcspn(s + i, ",:");
    if (i + len > n) {
      len = n - i;
    }
    const char *t = s + i;
    civue_title_button b = CIVUE_TITLE_NONE;
    if ((len == 7 && !memcmp(t, "appmenu", 7)) ||
        (len == 4 && !memcmp(t, "menu", 4))) {
      b = CIVUE_TITLE_MENU;
    }
    else if (len == 8 && !memcmp(t, "minimize", 8) && W.can_minimize) {
      b = CIVUE_TITLE_MINIMIZE;
    }
    else if (len == 8 && !memcmp(t, "maximize", 8) && W.can_maximize) {
      b = CIVUE_TITLE_MAXIMIZE;
    }
    else if (len == 5 && !memcmp(t, "close", 5)) {
      b = CIVUE_TITLE_CLOSE;
    }
    if (b) {
      out[k++] = b;
    }
    i += len + 1;
  }
}

// where the window's buttons go from the desktop's layout, nullptr for
// windows' and kde's: the menus first, the buttons at the end.
static void set_layout(const char *layout) {
  if (!layout) {
    layout = "menu:minimize,maximize,close";
  }
  const char *colon = strchr(layout, ':');
  usize n = strlen(layout), left = colon ? (usize)(colon - layout) : n;
  layout_side(layout, left, W.start);
  if (colon) {
    layout_side(colon + 1, n - left - 1, W.end);
  }
  else {
    memset(W.end, 0, sizeof W.end);
  }
  // the menus show somewhere: at the start when the layout leaves them out.
  bool menu = false;
  for (u32 i = 0; i < 4; i++) {
    menu |= W.start[i] == CIVUE_TITLE_MENU || W.end[i] == CIVUE_TITLE_MENU;
  }
  if (!menu) {
    memmove(W.start + 1, W.start, 3 * sizeof *W.start);
    W.start[0] = CIVUE_TITLE_MENU;
  }
}

static char *read_layout(void) {
  char *layout = take_string(
    portal_setting("org.gnome.desktop.wm.preferences", "button-layout"));
  return layout ? layout : gtk_layout();
}

// the desktop's settings as they change, on a thread of its own.
static void setting_changed(GDBusConnection *bus, const char *sender,
                            const char *path, const char *iface,
                            const char *signal, GVariant *params, void *data) {
  const char *ns, *key;
  GVariant *value;
  g_variant_get(params, "(&s&sv)", &ns, &key, &value);
  value = unwrap(value);
  if (!strcmp(ns, "org.freedesktop.appearance") &&
      !strcmp(key, "color-scheme")) {
    W.dark = take_dark(value);
    civue_window_wake();
  }
  else if (!strcmp(ns, "org.gnome.desktop.wm.preferences") &&
           !strcmp(key, "button-layout")) {
    char *layout = take_string(value);
    pthread_mutex_lock(&W.lock);
    free(W.layout_next);
    W.layout_next = layout ? layout : xstrdup("");
    pthread_mutex_unlock(&W.lock);
    civue_window_wake();
  }
  else if (value) {
    g_variant_unref(value);
  }
}

static void *watch_settings(void *arg) {
  GDBusConnection *bus = session_bus();
  if (!bus) {
    return nullptr;
  }
  GMainContext *ctx = g_main_context_new();
  g_main_context_push_thread_default(ctx);
  g_dbus_connection_signal_subscribe(
    bus, "org.freedesktop.portal.Desktop", "org.freedesktop.portal.Settings",
    "SettingChanged", "/org/freedesktop/portal/desktop", nullptr,
    G_DBUS_SIGNAL_FLAGS_NONE, setting_changed, nullptr, nullptr);
  GMainLoop *loop = g_main_loop_new(ctx, false);
  g_main_loop_run(loop);
  return nullptr;
}

// how the desktop's windows look: dark or light, their buttons, how round.
static void read_look(void) {
  W.dark =
    take_dark(portal_setting("org.freedesktop.appearance", "color-scheme"));
  char *layout = read_layout();
  set_layout(layout);
  free(layout);
  // gnome's adwaita windows are rounder than the others'.
  const char *desktop = getenv("XDG_CURRENT_DESKTOP");
  W.radius = desktop && strstr(desktop, "GNOME") ? 12 : 8;
  pthread_t watcher;
  pthread_create(&watcher, nullptr, watch_settings, nullptr);
  pthread_detach(watcher);
}

// the corners' mask, black where the window is cut, its coverage outside a
// circle of `pixels` around the corner, for the top left.
static void corner_mask(u32 pixels) {
  if (W.corner && W.corner_pixels == pixels) {
    return;
  }
  if (W.corner) {
    civue_gfx_free_texture(W.gfx, W.corner);
  }
  u8 *px = xcalloc((usize)pixels * pixels, 4);
  f32 r = (f32)pixels;
  for (u32 y = 0; y < pixels; y++) {
    for (u32 x = 0; x < pixels; x++) {
      f32 dx = r - (x + 0.5f), dy = r - (y + 0.5f);
      f32 outside = fminf(fmaxf(sqrtf(dx * dx + dy * dy) - r + 0.5f, 0), 1);
      px[((usize)y * pixels + x) * 4 + 3] = (u8)lroundf(outside * 255);
    }
  }
  W.corner = civue_gfx_upload_image(W.gfx, pixels, pixels, px);
  W.corner_pixels = pixels;
  free(px);
}

// cuts the window's corners round where the mask covers them.
static void round_corners(u32 pw, u32 ph, f32 scale) {
  u32 n = (u32)lroundf(W.radius * scale);
  if (!n) {
    return;
  }
  corner_mask(n);
  civue_dl_clear(&W.corners);
  f32 s = (f32)n;

  const struct {
    f32 x, y, uv[4];
  } at[] = {{0, 0, {0, 0, 1, 1}},
            {pw - s, 0, {1, 0, 0, 1}},
            {0, ph - s, {0, 1, 1, 0}},
            {pw - s, ph - s, {1, 1, 0, 0}}};

  for (u32 i = 0; i < countof(at); i++) {
    civue_dl_image(&W.corners, W.corner, rect_xywh(at[i].x, at[i].y, s, s),
                   at[i].uv, 1, 0, true);
  }
}

// ---- the window's frame ----

// the edge a press at `at` resizes the window from, 0 for none.
static u32 edge_at(civue_vec2 at) {
  if (!W.custom_frame || W.maximized || W.fullscreen) {
    return 0;
  }
  bool l = at.x < EDGE, r = at.x >= W.width - EDGE, t = at.y < EDGE,
       b = at.y >= W.height - EDGE;
  return t && l   ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT
         : t && r ? XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT
         : b && l ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT
         : b && r ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT
         : t      ? XDG_TOPLEVEL_RESIZE_EDGE_TOP
         : b      ? XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM
         : l      ? XDG_TOPLEVEL_RESIZE_EDGE_LEFT
         : r      ? XDG_TOPLEVEL_RESIZE_EDGE_RIGHT
                  : 0;
}

// where the app's title bar takes the pointer, or its whole height when it
// names no region.
static bool on_title_bar(civue_vec2 at) {
  return W.custom_frame && !W.fullscreen && at.y < W.options.titlebar &&
         (!W.options.titlebar_id ||
          civue_region_at(W.ui, at) == W.options.titlebar_id);
}

static void toggle_maximized(void) {
  if (W.maximized) {
    xdg_toplevel_unset_maximized(W.toplevel);
  }
  else {
    xdg_toplevel_set_maximized(W.toplevel);
  }
}

// a press the window's frame takes: resizing from its edges, moving it from
// its title bar, maximizing it with a double click there, and the window
// manager's menu. true when the app never sees it.
static bool frame_press(u32 button, civue_vec2 at, u32 clicks) {
  u32 edge = edge_at(at);
  if (edge && button == BTN_LEFT) {
    xdg_toplevel_resize(W.toplevel, WL.seat, WL.serial, edge);
    return true;
  }
  if (!on_title_bar(at)) {
    return false;
  }
  if (button == BTN_RIGHT && W.can_menu) {
    xdg_toplevel_show_window_menu(W.toplevel, WL.seat, WL.serial, (i32)at.x,
                                  (i32)at.y);
    return true;
  }
  if (button != BTN_LEFT) {
    return false;
  }
  if (clicks == 2 && W.can_maximize) {
    toggle_maximized();
  }
  else {
    xdg_toplevel_move(W.toplevel, WL.seat, WL.serial);
  }
  return true;
}

// ---- input ----

static void moved(civue_vec2 at, f32 pressure) {
  W.input.has_mouse = true;
  W.input.mouse = at;
  W.input.pressure = pressure;
  vec_push(&W.input.trail, ((civue_trail_point){at, pressure}));
  W.input.mods = W.mods;
  want();
}

// a press or release of a mouse button or the stylus's tip, at the pointer.
static void civue_button(u32 code, bool down) {
  u32 i = code == BTN_LEFT     ? 0
          : code == BTN_RIGHT  ? 1
          : code == BTN_MIDDLE ? 2
                               : 3;
  if (i > 2) {
    return;
  }
  civue_vec2 p = W.input.mouse;
  if (down && i == 0) {
    bool near = fabs(p.x - W.click_pos.x) < 4 && fabs(p.y - W.click_pos.y) < 4;
    f64 t = now_s();
    W.clicks = near && t - W.click_at < 0.4 ? W.clicks + 1 : 1;
    W.click_at = t;
    W.click_pos = p;
  }
  if (down && frame_press(code, p, i == 0 ? W.clicks : 1)) {
    return;
  }
  // a release the app never saw pressed is the frame's.
  if (!down && !W.input.down[i]) {
    return;
  }
  W.input.down[i] = down;
  if (!down) {
    W.input.released[i] = true;
  }
  else {
    W.input.pressed[i] = true;
    if (i == 0) {
      W.input.clicks = W.clicks;
    }
  }
  want();
}

static void released_all(void) {
  for (u32 i = 0; i < 3; i++) {
    if (W.input.down[i]) {
      W.input.down[i] = false;
      W.input.released[i] = true;
    }
  }
}

void civue__drag_moved(civue_vec2 at) {
  W.input.drop_at = at;
}

void civue__dropped(civue_vec2 at, civue_strs *paths) {
  W.input.drop_at = at;
  vec_each(paths, p) {
    vec_push(&W.input.drops, *p);
  }
  paths->len = 0;
  want();
}

// ---- the pointer ----

static void pointer_enter(void *data, struct wl_pointer *p, u32 serial,
                          struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
  WL.pointer_serial = serial;
  W.cursor_set = false;
  W.cursor_edge = 0;
  civue__cursors_entered();
  W.pointer = v2((f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
  moved(W.pointer, -1);
}

static void pointer_leave(void *data, struct wl_pointer *p, u32 serial,
                          struct wl_surface *s) {
  // the compositor moving or resizing the window takes the pointer, its
  // release never comes.
  released_all();
  W.input.has_mouse = false;
  want();
}

static void pointer_motion(void *data, struct wl_pointer *p, u32 time,
                           wl_fixed_t x, wl_fixed_t y) {
  W.pointer = v2((f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
  moved(W.pointer, -1);
}

static void pointer_button(void *data, struct wl_pointer *p, u32 serial,
                           u32 time, u32 code, u32 state) {
  WL.serial = serial;
  W.input.mouse = W.pointer;
  civue_button(code, state == WL_POINTER_BUTTON_STATE_PRESSED);
}

static void pointer_axis(void *data, struct wl_pointer *p, u32 time, u32 axis,
                         wl_fixed_t value) {
  if (axis < 2) {
    W.axis[axis] += wl_fixed_to_double(value);
  }
}

static void pointer_source(void *data, struct wl_pointer *p, u32 source) {
  W.axis_source = source;
}

static void pointer_stop(void *data, struct wl_pointer *p, u32 time, u32 axis) {
}

static void pointer_discrete(void *data, struct wl_pointer *p, u32 axis,
                             i32 discrete) {
  if (axis >= 2) {
    return;
  }
  W.discrete[axis] += discrete;
  W.has_discrete = true;
}

static void pointer_120(void *data, struct wl_pointer *p, u32 axis, i32 v) {
  if (axis >= 2) {
    return;
  }
  W.axis120[axis] += v;
  W.has_120 = true;
}

static void pointer_direction(void *data, struct wl_pointer *p, u32 axis,
                              u32 direction) {
}

// the scroll of a pointer frame: a wheel's notches scroll 40 points each,
// a touchpad's fingers move the content as far as they go.
static void pointer_frame(void *data, struct wl_pointer *p) {
  bool precise = W.axis_source == WL_POINTER_AXIS_SOURCE_FINGER ||
                 W.axis_source == WL_POINTER_AXIS_SOURCE_CONTINUOUS;
  civue_vec2 d = {};
  for (u32 a = 0; a < 2; a++) {
    f64 v = precise          ? W.axis[a]
            : W.has_120      ? W.axis120[a] / 120 * 40
            : W.has_discrete ? W.discrete[a] * 40.0
                             : W.axis[a] * 4;
    if (a == WL_POINTER_AXIS_VERTICAL_SCROLL) {
      d.y = (f32)-v;
    }
    else {
      d.x = (f32)-v;
    }
  }
  if (d.x || d.y) {
    if (precise) {
      W.input.precise_scroll = true;
    }
    W.input.scroll = v2_add(W.input.scroll, d);
    W.input.mods = W.mods;
    want();
  }
  W.axis[0] = W.axis[1] = W.axis120[0] = W.axis120[1] = 0;
  W.discrete[0] = W.discrete[1] = 0;
  W.has_120 = W.has_discrete = false;
  W.axis_source = WL_POINTER_AXIS_SOURCE_WHEEL;
}

static const struct wl_pointer_listener pointer_listener = {
  .enter = pointer_enter,
  .leave = pointer_leave,
  .motion = pointer_motion,
  .button = pointer_button,
  .axis = pointer_axis,
  .frame = pointer_frame,
  .axis_source = pointer_source,
  .axis_stop = pointer_stop,
  .axis_discrete = pointer_discrete,
  .axis_value120 = pointer_120,
  .axis_relative_direction = pointer_direction};

// ---- pinches ----

static void pinch_begin(void *data, struct zwp_pointer_gesture_pinch_v1 *g,
                        u32 serial, u32 time, struct wl_surface *s,
                        u32 fingers) {
  W.pinch_scale = 1;
}

static void pinch_update(void *data, struct zwp_pointer_gesture_pinch_v1 *g,
                         u32 time, wl_fixed_t dx, wl_fixed_t dy,
                         wl_fixed_t scale, wl_fixed_t rotation) {
  f64 s = wl_fixed_to_double(scale);
  if (s <= 0 || W.pinch_scale <= 0) {
    return;
  }
  W.input.pinch += (f32)(s / W.pinch_scale - 1);
  W.pinch_scale = s;
  want();
}

static void pinch_end(void *data, struct zwp_pointer_gesture_pinch_v1 *g,
                      u32 serial, u32 time, i32 cancelled) {
}

static const struct zwp_pointer_gesture_pinch_v1_listener pinch_listener = {
  .begin = pinch_begin, .update = pinch_update, .end = pinch_end};

// ---- the stylus ----

static void tool_type(void *d, struct zwp_tablet_tool_v2 *t, u32 type) {
}

static void tool_serial(void *d, struct zwp_tablet_tool_v2 *t, u32 hi, u32 lo) {
}

static void tool_wacom(void *d, struct zwp_tablet_tool_v2 *t, u32 hi, u32 lo) {
}

static void tool_capability(void *d, struct zwp_tablet_tool_v2 *t, u32 c) {
}

static void tool_done(void *d, struct zwp_tablet_tool_v2 *t) {
}

static void tool_removed(void *d, struct zwp_tablet_tool_v2 *t) {
  if (WL.tool == t) {
    WL.tool = nullptr;
  }
  zwp_tablet_tool_v2_destroy(t);
}

static void tool_in(void *d, struct zwp_tablet_tool_v2 *t, u32 serial,
                    struct zwp_tablet_v2 *tablet, struct wl_surface *s) {
  WL.tool = t;
  WL.tool_serial = serial;
  W.tool_pressure = 0;
  W.cursor_set = false;
  civue__cursors_entered();
}

static void tool_out(void *d, struct zwp_tablet_tool_v2 *t) {
  WL.tool = nullptr;
  released_all();
  W.input.pressure = -1;
  W.input.has_mouse = false;
  want();
}

static void tool_down(void *d, struct zwp_tablet_tool_v2 *t, u32 serial) {
  WL.serial = serial;
  W.tool_down = true;
}

static void tool_up(void *d, struct zwp_tablet_tool_v2 *t) {
  W.tool_up = true;
}

static void tool_motion(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x,
                        wl_fixed_t y) {
  W.tool_at = v2((f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
  W.tool_moved = true;
}

static void tool_pressure(void *d, struct zwp_tablet_tool_v2 *t, u32 p) {
  W.tool_pressure = p / 65535.0f;
  W.tool_moved = true;
}

static void tool_distance(void *d, struct zwp_tablet_tool_v2 *t, u32 v) {
}

static void tool_tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x,
                      wl_fixed_t y) {
}

static void tool_rotation(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t r) {
}

static void tool_slider(void *d, struct zwp_tablet_tool_v2 *t, i32 v) {
}

static void tool_wheel(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t deg,
                       i32 clicks) {
}

// the stylus's buttons are the mouse's right and middle buttons.
static void tool_button(void *d, struct zwp_tablet_tool_v2 *t, u32 serial,
                        u32 code, u32 state) {
  WL.serial = serial;
  W.input.mouse = W.tool_at;
  civue_button(code == BTN_STYLUS ? BTN_RIGHT : BTN_MIDDLE,
               state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED);
}

static void tool_frame(void *d, struct zwp_tablet_tool_v2 *t, u32 time) {
  if (W.tool_moved) {
    moved(W.tool_at, W.tool_pressure);
  }
  if (W.tool_down) {
    civue_button(BTN_LEFT, true);
  }
  if (W.tool_up) {
    civue_button(BTN_LEFT, false);
  }
  W.tool_moved = W.tool_down = W.tool_up = false;
}

static const struct zwp_tablet_tool_v2_listener tool_listener = {
  .type = tool_type,
  .hardware_serial = tool_serial,
  .hardware_id_wacom = tool_wacom,
  .capability = tool_capability,
  .done = tool_done,
  .removed = tool_removed,
  .proximity_in = tool_in,
  .proximity_out = tool_out,
  .down = tool_down,
  .up = tool_up,
  .motion = tool_motion,
  .pressure = tool_pressure,
  .distance = tool_distance,
  .tilt = tool_tilt,
  .rotation = tool_rotation,
  .slider = tool_slider,
  .wheel = tool_wheel,
  .button = tool_button,
  .frame = tool_frame};

// tablets and their pads say nothing the app uses, only their tools do.
static void tablet_added(void *d, struct zwp_tablet_seat_v2 *s,
                         struct zwp_tablet_v2 *t) {
  zwp_tablet_v2_destroy(t);
}

static void tool_added(void *d, struct zwp_tablet_seat_v2 *s,
                       struct zwp_tablet_tool_v2 *t) {
  zwp_tablet_tool_v2_add_listener(t, &tool_listener, nullptr);
}

static void pad_added(void *d, struct zwp_tablet_seat_v2 *s,
                      struct zwp_tablet_pad_v2 *p) {
  zwp_tablet_pad_v2_destroy(p);
}

static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {
  .tablet_added = tablet_added,
  .tool_added = tool_added,
  .pad_added = pad_added};

// ---- the keyboard ----

static void keymap(void *data, struct wl_keyboard *k, u32 format, i32 fd,
                   u32 size) {
  if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
    close(fd);
    return;
  }
  char *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
  close(fd);
  if (map == MAP_FAILED) {
    return;
  }
  struct xkb_keymap *km = xkb_keymap_new_from_string(
    W.xkb, map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
  munmap(map, size);
  if (!km) {
    return;
  }
  if (W.state) {
    xkb_state_unref(W.state);
  }
  if (W.keymap) {
    xkb_keymap_unref(W.keymap);
  }
  W.keymap = km;
  W.state = xkb_state_new(km);
  W.mod_shift = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_SHIFT);
  W.mod_ctrl = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_CTRL);
  W.mod_alt = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_ALT);
  W.mod_logo = xkb_keymap_mod_get_index(km, XKB_MOD_NAME_LOGO);
}

static void keyboard_enter(void *data, struct wl_keyboard *k, u32 serial,
                           struct wl_surface *s, struct wl_array *keys) {
  WL.serial = serial;
  want();
}

static void keyboard_leave(void *data, struct wl_keyboard *k, u32 serial,
                           struct wl_surface *s) {
  W.input.held.len = 0;
  W.repeat_key = 0;
  W.mods = 0;
  W.input.mods = 0;
  want();
}

static u32 special_key(xkb_keysym_t s) {
  switch (s) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
      return CIVUE_KEY_ENTER;
    case XKB_KEY_Escape:
      return CIVUE_KEY_ESCAPE;
    case XKB_KEY_Tab:
    case XKB_KEY_ISO_Left_Tab:
      return CIVUE_KEY_TAB;
    case XKB_KEY_BackSpace:
      return CIVUE_KEY_BACKSPACE;
    case XKB_KEY_Delete:
    case XKB_KEY_KP_Delete:
      return CIVUE_KEY_DELETE;
    case XKB_KEY_Left:
    case XKB_KEY_KP_Left:
      return CIVUE_KEY_LEFT;
    case XKB_KEY_Right:
    case XKB_KEY_KP_Right:
      return CIVUE_KEY_RIGHT;
    case XKB_KEY_Down:
    case XKB_KEY_KP_Down:
      return CIVUE_KEY_DOWN;
    case XKB_KEY_Up:
    case XKB_KEY_KP_Up:
      return CIVUE_KEY_UP;
    case XKB_KEY_Home:
    case XKB_KEY_KP_Home:
      return CIVUE_KEY_HOME;
    case XKB_KEY_End:
    case XKB_KEY_KP_End:
      return CIVUE_KEY_END;
    case XKB_KEY_Page_Up:
    case XKB_KEY_KP_Page_Up:
      return CIVUE_KEY_PAGE_UP;
    case XKB_KEY_Page_Down:
    case XKB_KEY_KP_Page_Down:
      return CIVUE_KEY_PAGE_DOWN;
    case XKB_KEY_space:
      return CIVUE_KEY_SPACE;
  }
  if (s >= XKB_KEY_F1 && s <= XKB_KEY_F12) {
    return CIVUE_KEY_F1 + (s - XKB_KEY_F1);
  }
  return CIVUE_KEY_NONE;
}

// the digit row gives its digit on any layout, shortcuts need them.
static u32 digit_key(u32 code) {
  if (code >= EV_1 && code < EV_0) {
    return '1' + (code - EV_1);
  }
  if (code == EV_0) {
    return '0';
  }

  static const struct {
    u32 code;
    char digit;
  } pad[] = {{EV_KP0, '0'}, {EV_KP1, '1'}, {EV_KP2, '2'}, {EV_KP3, '3'},
             {EV_KP4, '4'}, {EV_KP5, '5'}, {EV_KP6, '6'}, {EV_KP7, '7'},
             {EV_KP8, '8'}, {EV_KP9, '9'}};

  for (u32 i = 0; i < countof(pad); i++) {
    if (pad[i].code == code) {
      return (u32)pad[i].digit;
    }
  }
  return CIVUE_KEY_NONE;
}

// the key an event stands for, by the key itself rather than what shift or
// alt make of it on the layout.
static u32 key_of(u32 code) {
  xkb_keycode_t kc = code + 8;
  u32 k = special_key(xkb_state_key_get_one_sym(W.state, kc));
  if (k) {
    return k;
  }
  k = digit_key(code);
  if (k) {
    return k;
  }
  const xkb_keysym_t *syms;
  xkb_layout_index_t layout = xkb_state_key_get_layout(W.state, kc);
  if (layout == XKB_LAYOUT_INVALID) {
    layout = 0;
  }
  if (xkb_keymap_key_get_syms_by_level(W.keymap, kc, layout, 0, &syms) < 1) {
    return CIVUE_KEY_NONE;
  }
  u32 c = xkb_keysym_to_utf32(syms[0]);
  if (c < 0x20 || c == 0x7f) {
    return CIVUE_KEY_NONE;
  }
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

static void typed(const char *t) {
  // typed text goes through the input method, unless a shortcut takes it.
  if (W.input.mods & (CIVUE_MOD_CMD | CIVUE_MOD_CTRL)) {
    return;
  }
  for (const char *c = t; *c; c++) {
    if ((u8)*c >= 0x20 && *c != 0x7f) {
      vec_push(&W.input.text, (u8)*c);
    }
  }
  W.input.preedit.len = 0;
}

// a key going down, or repeating while held.
static void key_pressed(u32 code, bool repeat) {
  xkb_keycode_t kc = code + 8;
  W.input.mods = W.mods;
  u32 key = key_of(code);
  if (key) {
    bool held = false;
    vec_each(&W.input.held, h) {
      held |= *h == key;
    }
    if (!held) {
      vec_push(&W.input.held, key);
    }
    // the menus' keys go to them first, like the system's menu bar takes
    // its key equivalents.
    civue_key_press k = {key, W.mods};
    if (!W.options.menu_key || !W.options.menu_key(W.ui, k)) {
      vec_push(&W.input.keys, k);
    }
  }
  char text[64] = {};
  xkb_keysym_t sym = xkb_state_key_get_one_sym(W.state, kc);
  enum xkb_compose_status status = XKB_COMPOSE_NOTHING;
  if (W.compose && !repeat &&
      xkb_compose_state_feed(W.compose, sym) == XKB_COMPOSE_FEED_ACCEPTED) {
    status = xkb_compose_state_get_status(W.compose);
  }
  if (status == XKB_COMPOSE_COMPOSED) {
    xkb_compose_state_get_utf8(W.compose, text, sizeof text);
    xkb_compose_state_reset(W.compose);
  }
  else if (status == XKB_COMPOSE_CANCELLED) {
    xkb_compose_state_reset(W.compose);
  }
  else if (status == XKB_COMPOSE_NOTHING) {
    xkb_state_key_get_utf8(W.state, kc, text, sizeof text);
  }
  typed(text);
  want();
}

static void key_released(u32 code) {
  W.input.mods = W.mods;
  u32 key = key_of(code);
  u32 k = 0;
  vec_each(&W.input.held, h) {
    if (*h != key) {
      W.input.held.v[k++] = *h;
    }
  }
  W.input.held.len = k;
  want();
}

static void keyboard_key(void *data, struct wl_keyboard *k, u32 serial,
                         u32 time, u32 code, u32 state) {
  WL.serial = serial;
  if (!W.state) {
    return;
  }
  if (state == WL_KEYBOARD_KEY_STATE_PRESSED) {
    key_pressed(code, false);
    bool repeats =
      W.repeat_rate > 0 && xkb_keymap_key_repeats(W.keymap, code + 8);
    W.repeat_key = repeats ? code : 0;
    W.repeat_at = now_s() + W.repeat_delay / 1000.0;
  }
  else {
    key_released(code);
    if (W.repeat_key == code) {
      W.repeat_key = 0;
    }
  }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *k, u32 serial,
                               u32 depressed, u32 latched, u32 locked,
                               u32 group) {
  if (!W.state) {
    return;
  }
  xkb_state_update_mask(W.state, depressed, latched, locked, 0, 0, group);

  // like most apps on linux, control is the command key. the super key
  // stands in for the mac's control.
  struct {
    xkb_mod_index_t index;
    u8 mod;
  } map[] = {{W.mod_shift, CIVUE_MOD_SHIFT},
             {W.mod_ctrl, CIVUE_MOD_CMD},
             {W.mod_alt, CIVUE_MOD_ALT},
             {W.mod_logo, CIVUE_MOD_CTRL}};

  u8 mods = 0;
  for (u32 i = 0; i < countof(map); i++) {
    if (map[i].index != XKB_MOD_INVALID &&
        xkb_state_mod_index_is_active(W.state, map[i].index,
                                      XKB_STATE_MODS_EFFECTIVE) > 0) {
      mods |= map[i].mod;
    }
  }
  W.mods = mods;
  W.input.mods = mods;
  want();
}

static void keyboard_repeat(void *data, struct wl_keyboard *k, i32 rate,
                            i32 delay) {
  W.repeat_rate = rate;
  W.repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
  .keymap = keymap,
  .enter = keyboard_enter,
  .leave = keyboard_leave,
  .key = keyboard_key,
  .modifiers = keyboard_modifiers,
  .repeat_info = keyboard_repeat};

// repeats the key held, as often as the compositor says.
static void repeat_keys(void) {
  if (!W.repeat_key || W.repeat_rate <= 0) {
    return;
  }
  f64 t = now_s();
  while (W.repeat_key && t >= W.repeat_at) {
    key_pressed(W.repeat_key, true);
    W.repeat_at += 1.0 / W.repeat_rate;
  }
}

// ---- input methods ----

static void text_set(char **slot, const char *text) {
  free(*slot);
  *slot = text ? xstrdup(text) : nullptr;
}

static void text_enter(void *data, struct zwp_text_input_v3 *t,
                       struct wl_surface *s) {
  zwp_text_input_v3_enable(t);
  zwp_text_input_v3_set_content_type(t, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
                                     ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
  zwp_text_input_v3_commit(t);
  W.text_on = true;
  W.candidates = (civue_rect){};
}

static void text_leave(void *data, struct zwp_text_input_v3 *t,
                       struct wl_surface *s) {
  zwp_text_input_v3_disable(t);
  zwp_text_input_v3_commit(t);
  W.text_on = false;
  W.input.preedit.len = 0;
  want();
}

static void text_preedit(void *data, struct zwp_text_input_v3 *t,
                         const char *text, i32 begin, i32 end) {
  text_set(&W.preedit_next, text);
}

static void text_commit(void *data, struct zwp_text_input_v3 *t,
                        const char *text) {
  text_set(&W.commit_next, text);
}

static void text_delete(void *data, struct zwp_text_input_v3 *t, u32 before,
                        u32 after) {
}

static void text_done(void *data, struct zwp_text_input_v3 *t, u32 serial) {
  if (W.commit_next) {
    for (const char *c = W.commit_next; *c; c++) {
      if ((u8)*c >= 0x20 && *c != 0x7f) {
        vec_push(&W.input.text, (u8)*c);
      }
    }
  }
  W.input.preedit.len = 0;
  if (W.preedit_next) {
    buf_put(&W.input.preedit, W.preedit_next, strlen(W.preedit_next));
  }
  text_set(&W.commit_next, nullptr);
  text_set(&W.preedit_next, nullptr);
  want();
}

static const struct zwp_text_input_v3_listener text_listener = {
  .enter = text_enter,
  .leave = text_leave,
  .preedit_string = text_preedit,
  .commit_string = text_commit,
  .delete_surrounding_text = text_delete,
  .done = text_done};

// the input method's candidates open under the pointer.
static void place_candidates(void) {
  if (!W.text_on) {
    return;
  }
  civue_rect r =
    rect_xywh(roundf(W.input.mouse.x), roundf(W.input.mouse.y), 1, 20);
  if (!memcmp(&r, &W.candidates, sizeof r)) {
    return;
  }
  W.candidates = r;
  zwp_text_input_v3_set_cursor_rectangle(W.text_input, (i32)r.x0, (i32)r.y0, 1,
                                         20);
  zwp_text_input_v3_commit(W.text_input);
}

// ---- the seat ----

static void seat_capabilities(void *data, struct wl_seat *s, u32 caps) {
  bool pointer = caps & WL_SEAT_CAPABILITY_POINTER;
  bool keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;
  if (pointer && !WL.pointer) {
    WL.pointer = wl_seat_get_pointer(s);
    wl_pointer_add_listener(WL.pointer, &pointer_listener, nullptr);
    if (W.gestures) {
      W.pinch =
        zwp_pointer_gestures_v1_get_pinch_gesture(W.gestures, WL.pointer);
      zwp_pointer_gesture_pinch_v1_add_listener(W.pinch, &pinch_listener,
                                                nullptr);
    }
  }
  else if (!pointer && WL.pointer) {
    if (W.pinch) {
      zwp_pointer_gesture_pinch_v1_destroy(W.pinch);
    }
    W.pinch = nullptr;
    wl_pointer_release(WL.pointer);
    WL.pointer = nullptr;
  }
  if (keyboard && !W.keyboard) {
    W.keyboard = wl_seat_get_keyboard(s);
    wl_keyboard_add_listener(W.keyboard, &keyboard_listener, nullptr);
  }
  else if (!keyboard && W.keyboard) {
    wl_keyboard_release(W.keyboard);
    W.keyboard = nullptr;
  }
}

static void seat_name(void *data, struct wl_seat *s, const char *name) {
}

static const struct wl_seat_listener seat_listener = {
  .capabilities = seat_capabilities, .name = seat_name};

// ---- outputs ----

static Output *output_of(struct wl_output *o) {
  vec_each(&W.outputs, out) {
    if (out->output == o) {
      return out;
    }
  }
  return nullptr;
}

static void output_geometry(void *d, struct wl_output *o, i32 x, i32 y, i32 w,
                            i32 h, i32 subpixel, const char *make,
                            const char *model, i32 transform) {
}

static void output_mode(void *d, struct wl_output *o, u32 flags, i32 w, i32 h,
                        i32 refresh) {
}

static void output_done(void *d, struct wl_output *o) {
  outputs_scale();
}

static void output_scale(void *d, struct wl_output *o, i32 scale) {
  Output *out = output_of(o);
  if (out) {
    out->scale = scale;
  }
}

static void output_name(void *d, struct wl_output *o, const char *name) {
}

static void output_description(void *d, struct wl_output *o, const char *s) {
}

static const struct wl_output_listener output_listener = {
  .geometry = output_geometry,
  .mode = output_mode,
  .done = output_done,
  .scale = output_scale,
  .name = output_name,
  .description = output_description};

// ---- the surface ----

static void surface_enter(void *data, struct wl_surface *s,
                          struct wl_output *o) {
  Output *out = output_of(o);
  if (out) {
    out->on = true;
  }
  outputs_scale();
}

static void surface_leave(void *data, struct wl_surface *s,
                          struct wl_output *o) {
  Output *out = output_of(o);
  if (out) {
    out->on = false;
  }
  outputs_scale();
}

static void surface_scale(void *data, struct wl_surface *s, i32 scale) {
  W.preferred_scale = true;
  if (W.scale120 || scale == W.buffer_scale || scale < 1) {
    return;
  }
  W.buffer_scale = scale;
  scale_changed();
}

static void surface_transform(void *data, struct wl_surface *s, u32 t) {
}

static const struct wl_surface_listener surface_listener = {
  .enter = surface_enter,
  .leave = surface_leave,
  .preferred_buffer_scale = surface_scale,
  .preferred_buffer_transform = surface_transform};

static void fraction_scale(void *data, struct wp_fractional_scale_v1 *f,
                           u32 scale) {
  if (scale == W.scale120) {
    return;
  }
  W.scale120 = scale;
  scale_changed();
}

static const struct wp_fractional_scale_v1_listener fraction_listener = {
  .preferred_scale = fraction_scale};

// ---- the toplevel ----

static void wm_ping(void *data, struct xdg_wm_base *wm, u32 serial) {
  xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_listener = {.ping = wm_ping};

static void toplevel_configure(void *data, struct xdg_toplevel *t, i32 w, i32 h,
                               struct wl_array *states) {
  W.next_w = w;
  W.next_h = h;
  W.next_maximized = W.next_fullscreen = W.next_activated = W.next_tiled =
    W.next_suspended = false;
  u32 *s;
  wl_array_for_each(s, states) {
    switch (*s) {
      case XDG_TOPLEVEL_STATE_MAXIMIZED:
        W.next_maximized = true;
        break;
      case XDG_TOPLEVEL_STATE_FULLSCREEN:
        W.next_fullscreen = true;
        break;
      case XDG_TOPLEVEL_STATE_ACTIVATED:
        W.next_activated = true;
        break;
      case XDG_TOPLEVEL_STATE_TILED_LEFT:
      case XDG_TOPLEVEL_STATE_TILED_RIGHT:
      case XDG_TOPLEVEL_STATE_TILED_TOP:
      case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
        W.next_tiled = true;
        break;
      case XDG_TOPLEVEL_STATE_SUSPENDED:
        W.next_suspended = true;
        break;
    }
  }
}

static void toplevel_close(void *data, struct xdg_toplevel *t) {
  exit(0);
}

static void toplevel_bounds(void *data, struct xdg_toplevel *t, i32 w, i32 h) {
  W.bounds_w = w;
  W.bounds_h = h;
}

static void toplevel_caps(void *data, struct xdg_toplevel *t,
                          struct wl_array *caps) {
  W.caps_known = true;
  W.can_minimize = W.can_maximize = W.can_menu = false;
  u32 *c;
  wl_array_for_each(c, caps) {
    switch (*c) {
      case XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE:
        W.can_minimize = true;
        break;
      case XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE:
        W.can_maximize = true;
        break;
      case XDG_TOPLEVEL_WM_CAPABILITIES_WINDOW_MENU:
        W.can_menu = true;
        break;
    }
  }
}

static const struct xdg_toplevel_listener toplevel_listener = {
  .configure = toplevel_configure,
  .close = toplevel_close,
  .configure_bounds = toplevel_bounds,
  .wm_capabilities = toplevel_caps};

static void xdg_configure(void *data, struct xdg_surface *s, u32 serial) {
  bool floating = !W.next_maximized && !W.next_fullscreen && !W.next_tiled;
  i32 w = W.next_w, h = W.next_h;
  // a floating window the compositor leaves the size of goes back to its
  // own.
  if (w <= 0 || h <= 0) {
    w = W.floating_w;
    h = W.floating_h;
  }
  w = MAX(w, (i32)W.options.min_width);
  h = MAX(h, (i32)W.options.min_height);
  if (floating) {
    W.floating_w = w;
    W.floating_h = h;
  }
  if (w != W.width || h != W.height) {
    W.stale = true;
  }
  W.width = w;
  W.height = h;
  W.maximized = W.next_maximized;
  W.fullscreen = W.next_fullscreen;
  W.tiled = W.next_tiled;
  W.suspended = W.next_suspended;
  if (W.activated != W.next_activated) {
    W.activated = W.next_activated;
    if (!W.activated) {
      W.input.held.len = 0;
      released_all();
    }
  }
  xdg_surface_ack_configure(s, serial);
  W.configured = true;
  W.reconfigured = true;
  want();
}

static const struct xdg_surface_listener xdg_listener = {.configure =
                                                           xdg_configure};

static void decoration_mode(void *data, struct zxdg_toplevel_decoration_v1 *d,
                            u32 mode) {
  W.server_frame = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
  W.custom_frame = W.options.titlebar > 0 && !W.server_frame;
  want();
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
  .configure = decoration_mode};

static void exported_handle(void *data, struct zxdg_exported_v2 *e,
                            const char *handle) {
  free(W.handle);
  W.handle = xfmt("wayland:%s", handle);
}

static const struct zxdg_exported_v2_listener exported_listener = {
  .handle = exported_handle};

// ---- activation ----

static void activation_done(void *data, struct xdg_activation_token_v1 *t,
                            const char *token) {
  xdg_activation_v1_activate(W.activation, token, WL.surface);
  xdg_activation_token_v1_destroy(t);
}

static const struct xdg_activation_token_v1_listener token_listener = {
  .done = activation_done};

static void raise_on_main(void *arg) {
  if (!W.activation || !WL.surface) {
    return;
  }
  struct xdg_activation_token_v1 *t =
    xdg_activation_v1_get_activation_token(W.activation);
  xdg_activation_token_v1_add_listener(t, &token_listener, nullptr);
  xdg_activation_token_v1_set_surface(t, WL.surface);
  if (WL.serial) {
    xdg_activation_token_v1_set_serial(t, WL.serial, WL.seat);
  }
  xdg_activation_token_v1_set_app_id(t, app_id());
  xdg_activation_token_v1_commit(t);
  wl_display_flush(WL.display);
}

void civue_window_raise(void) {
  civue_on_main(raise_on_main, nullptr);
}

// ---- globals ----

static void *bind(struct wl_registry *r, u32 name,
                  const struct wl_interface *iface, u32 has, u32 want) {
  return wl_registry_bind(r, name, iface, MIN(has, want));
}

static void global(void *data, struct wl_registry *r, u32 name,
                   const char *iface, u32 version) {
  if (!strcmp(iface, wl_compositor_interface.name)) {
    WL.compositor = bind(r, name, &wl_compositor_interface, version, 6);
  }
  else if (!strcmp(iface, wl_shm_interface.name)) {
    WL.shm = bind(r, name, &wl_shm_interface, version, 1);
  }
  else if (!strcmp(iface, wl_seat_interface.name) && !WL.seat) {
    WL.seat = bind(r, name, &wl_seat_interface, version, 9);
    wl_seat_add_listener(WL.seat, &seat_listener, nullptr);
  }
  else if (!strcmp(iface, wl_output_interface.name)) {
    Output o = {bind(r, name, &wl_output_interface, version, 4), 1};
    wl_output_add_listener(o.output, &output_listener, nullptr);
    vec_push(&W.outputs, o);
  }
  else if (!strcmp(iface, wl_data_device_manager_interface.name)) {
    WL.data_manager =
      bind(r, name, &wl_data_device_manager_interface, version, 3);
  }
  else if (!strcmp(iface, xdg_wm_base_interface.name)) {
    W.wm = bind(r, name, &xdg_wm_base_interface, version, 6);
    xdg_wm_base_add_listener(W.wm, &wm_listener, nullptr);
  }
  else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
    W.decorations =
      bind(r, name, &zxdg_decoration_manager_v1_interface, version, 1);
  }
  else if (!strcmp(iface, wp_viewporter_interface.name)) {
    WL.viewporter = bind(r, name, &wp_viewporter_interface, version, 1);
  }
  else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
    W.fractions =
      bind(r, name, &wp_fractional_scale_manager_v1_interface, version, 1);
  }
  else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name)) {
    WL.cursor_shapes =
      bind(r, name, &wp_cursor_shape_manager_v1_interface, version, 1);
  }
  else if (!strcmp(iface, zwp_pointer_gestures_v1_interface.name)) {
    W.gestures = bind(r, name, &zwp_pointer_gestures_v1_interface, version, 1);
  }
  else if (!strcmp(iface, zwp_tablet_manager_v2_interface.name)) {
    W.tablets = bind(r, name, &zwp_tablet_manager_v2_interface, version, 1);
  }
  else if (!strcmp(iface, zwp_text_input_manager_v3_interface.name)) {
    W.text_inputs =
      bind(r, name, &zwp_text_input_manager_v3_interface, version, 1);
  }
  else if (!strcmp(iface, xdg_activation_v1_interface.name)) {
    W.activation = bind(r, name, &xdg_activation_v1_interface, version, 1);
  }
  else if (!strcmp(iface, zxdg_exporter_v2_interface.name)) {
    W.exporter = bind(r, name, &zxdg_exporter_v2_interface, version, 1);
  }
}

static void global_remove(void *data, struct wl_registry *r, u32 name) {
}

static const struct wl_registry_listener registry_listener = {
  .global = global, .global_remove = global_remove};

// ---- frames ----

static void schedule_timer(f64 at) {
  if (!W.timer_at || at < W.timer_at) {
    W.timer_at = at;
  }
}

static void frame_done(void *data, struct wl_callback *cb, u32 time) {
  wl_callback_destroy(cb);
  if (W.frame_cb == cb) {
    W.frame_cb = nullptr;
  }
}

static const struct wl_callback_listener frame_listener = {.done = frame_done};

// whether a frame can be drawn now: the window is shown, and the compositor
// took the last one or wants a new size.
static bool drawable(void) {
  return W.configured && !W.suspended && (!W.frame_cb || W.reconfigured);
}

// the cursor the pointer shows: a resize at the window's edges, the app's
// elsewhere.
static void update_cursor(void) {
  u32 edge = W.input.has_mouse ? edge_at(W.input.mouse) : 0;
  if (edge) {
    if (W.cursor_edge != edge) {
      civue__cursors_set_edge(edge);
    }
    W.cursor_edge = edge;
    W.cursor_set = false;
    return;
  }
  civue_ui *ui = W.ui;
  if (W.cursor_set && !W.cursor_edge && ui->cursor.kind == W.cursor.kind &&
      ui->cursor.angle == W.cursor.angle) {
    return;
  }
  W.cursor = ui->cursor;
  W.cursor_set = true;
  W.cursor_edge = 0;
  civue__cursors_set(ui->cursor);
}

static void frame(void) {
  W.wanted = false;
  const civue_vk_device *v = civue_vk();
  // the frame that last used this slot's semaphore is done.
  civue_vk_wait(W.frame_value[W.frame]);
  u32 pw, ph;
  pixel_size(&pw, &ph);
  if (W.stale || !W.swapchain || W.extent.width != pw ||
      W.extent.height != ph) {
    make_swapchain();
  }
  u32 image = 0;
  VkResult r =
    vkAcquireNextImageKHR(v->device, W.swapchain, UINT64_MAX,
                          W.acquired[W.frame], VK_NULL_HANDLE, &image);
  if (r == VK_ERROR_OUT_OF_DATE_KHR) {
    W.stale = true;
    want();
    return;
  }
  if (r == VK_SUBOPTIMAL_KHR) {
    W.stale = true;
  }
  else if (r != VK_SUCCESS) {
    die("vulkan: acquire failed (%d)", r);
  }
  civue_vk_tex *target = W.images[image];
  VkSemaphore acquired = W.acquired[W.frame];
  f32 scale = scale_factor();
  pw = target->width, ph = target->height;
  civue_ui *ui = W.ui;
  civue_color background = W.app_cb.background
                             ? W.app_cb.background(W.app_cb.ctx)
                             : color_hex(0x1e1e1e);
  if (W.options.titlebar > 0) {
    ui->titlebar_inset = 0;
    ui->titlebar_height = W.custom_frame ? W.options.titlebar : 0;
    ui->window_controls = W.custom_frame && !W.fullscreen;
    ui->window_maximized = W.maximized;
    memcpy(ui->titlebar_start, W.start, sizeof W.start);
    memcpy(ui->titlebar_end, W.end, sizeof W.end);
  }
  ui->system_dark = W.dark;
  ui->window_focused = W.activated;
  civue_begin_frame(ui, v2(pw / scale, ph / scale), scale, &W.input);
  W.app_cb.frame(W.app_cb.ctx, ui);
  civue_end_frame(ui);
  // the mask uploads with a submission of its own, before the frame's.
  bool round = W.custom_frame && W.translucent && !W.maximized &&
               !W.fullscreen && !W.tiled;
  if (round) {
    round_corners(pw, ph, scale);
  }
  VkCommandBuffer cb = civue_gfx_vk_begin(W.gfx);
  civue_gfx_encode(W.gfx, cb, target, pw, ph, civue_frame_list(ui),
                   &background);
  if (round) {
    civue_gfx_vk_erase(W.gfx, cb, target, pw, ph, &W.corners);
  }
  civue_vk_layout(cb, target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  W.frame_value[W.frame] =
    civue_gfx_vk_submit(W.gfx, cb, acquired, W.rendered[image]);
  // the surface's scale and the next frame's callback come with the commit
  // the present makes.
  if (W.viewport) {
    wp_viewport_set_destination(W.viewport, W.width, W.height);
  }
  else {
    wl_surface_set_buffer_scale(WL.surface, W.buffer_scale);
  }
  if (!W.frame_cb) {
    W.frame_cb = wl_surface_frame(WL.surface);
    wl_callback_add_listener(W.frame_cb, &frame_listener, nullptr);
  }
  W.reconfigured = false;
  VkPresentInfoKHR present = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                              .waitSemaphoreCount = 1,
                              .pWaitSemaphores = &W.rendered[image],
                              .swapchainCount = 1,
                              .pSwapchains = &W.swapchain,
                              .pImageIndices = &image};
  r = civue_vk_present(&present);
  if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
    W.stale = true;
  }
  // the image is the presentation engine's until it is acquired again.
  target->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  W.frame = (W.frame + 1) % FRAMES;
  update_cursor();
  if (ui->toggle_maximize && !W.fullscreen) {
    toggle_maximized();
  }
  if (ui->minimize_window) {
    xdg_toplevel_set_minimized(W.toplevel);
  }
  if (ui->close_window) {
    exit(0);
  }
  if (ui->toggle_fullscreen) {
    ui->toggle_fullscreen = false;
    if (W.fullscreen) {
      xdg_toplevel_unset_fullscreen(W.toplevel);
    }
    else {
      xdg_toplevel_set_fullscreen(W.toplevel, nullptr);
    }
  }
  if (ui->repaint) {
    want();
  }
  if (ui->repaint_at) {
    schedule_timer(ui->repaint_at);
  }
}

// ---- running ----

// waits for the compositor, other threads and the watched loops for at most
// `ms`, -1 for as long as it takes, then handles what came.
static void pump(i32 ms) {
  struct wl_display *d = WL.display;
  while (wl_display_prepare_read(d) != 0) {
    wl_display_dispatch_pending(d);
  }
  while (wl_display_flush(d) < 0 && errno == EAGAIN) {
    struct pollfd out = {wl_display_get_fd(d), POLLOUT, 0};
    poll(&out, 1, -1);
  }
  constexpr u32 MOST = 8;
  struct pollfd p[2 + MOST] = {{wl_display_get_fd(d), POLLIN, 0},
                               {wake_fd, POLLIN, 0}};
  u32 nwatch = MIN(W.watches.len, MOST);
  for (u32 i = 0; i < nwatch; i++) {
    p[2 + i] = (struct pollfd){W.watches.v[i].fd, POLLIN, 0};
  }
  int n = poll(p, 2 + nwatch, ms);
  if (n > 0 && p[0].revents & POLLIN) {
    if (wl_display_read_events(d) < 0) {
      die("wayland: the compositor left");
    }
  }
  else {
    wl_display_cancel_read(d);
  }
  if (p[0].revents & (POLLERR | POLLHUP)) {
    die("wayland: the compositor left");
  }
  if (wl_display_dispatch_pending(d) < 0) {
    die("wayland: the compositor left");
  }
  if (n > 0 && p[1].revents & POLLIN) {
    eventfd_t value;
    eventfd_read(wake_fd, &value);
    run_calls();
  }
  if (atomic_exchange(&woken, false)) {
    want();
  }
  for (u32 i = 0; n > 0 && i < nwatch; i++) {
    if (p[2 + i].revents & POLLIN) {
      W.watches.v[i].ready(W.watches.v[i].arg);
    }
  }
}

void civue_watch_fd(int fd, void (*ready)(void *arg), void *arg) {
  vec_push(&W.watches, ((Watch){fd, ready, arg}));
}

void civue_wait_while(bool (*waiting)(void *arg), void *arg) {
  while (waiting(arg)) {
    pump(20);
  }
  // what was pressed or typed while the dialog was up was the dialog's.
  memset(W.input.pressed, 0, sizeof W.input.pressed);
  memset(W.input.released, 0, sizeof W.input.released);
  memset(W.input.down, 0, sizeof W.input.down);
  W.input.keys.len = 0;
  W.input.text.len = 0;
}

// the timeout of the next wait: now for a frame, else the first of the
// timer and the key repeat, -1 for none.
static i32 next_wait(void) {
  f64 next = W.wanted && drawable() ? now_s() : W.timer_at;
  if (W.repeat_key && (!next || W.repeat_at < next)) {
    next = W.repeat_at;
  }
  if (!next) {
    return -1;
  }
  return (i32)CLAMP(ceil((next - now_s()) * 1000), 0.0, 1e6);
}

void civue_window_run_sized(const civue_window_options *o,
                            civue_app (*make)(civue_ui *ui, void *arg),
                            void *arg, size_t ui_size) {
  W.options = *o;
  W.input.pressure = -1;
  wake_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  W.can_minimize = W.can_maximize = W.can_menu = true;
  WL.display = wl_display_connect(nullptr);
  if (!WL.display) {
    die("a wayland session is needed");
  }
  W.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
  const char *locale = getenv("LC_ALL");
  if (!locale || !*locale) {
    locale = getenv("LC_CTYPE");
  }
  if (!locale || !*locale) {
    locale = getenv("LANG");
  }
  if (!locale || !*locale) {
    locale = "C";
  }
  struct xkb_compose_table *table = xkb_compose_table_new_from_locale(
    W.xkb, locale, XKB_COMPOSE_COMPILE_NO_FLAGS);
  if (table) {
    W.compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
    xkb_compose_table_unref(table);
  }
  struct wl_registry *registry = wl_display_get_registry(WL.display);
  wl_registry_add_listener(registry, &registry_listener, nullptr);
  wl_display_roundtrip(WL.display);
  if (!WL.compositor || !W.wm || !WL.shm) {
    die("wayland: the compositor has no xdg shell");
  }
  // the seat's devices, the outputs' scales.
  wl_display_roundtrip(WL.display);
  if (o->title) {
    civue__gpu_title(o->title);
  }
  const civue_vk_device *v = civue_vk();
  if (!v || !civue_vk_has_instance_extension("VK_KHR_wayland_surface")) {
    die("vulkan: no device to draw with");
  }
  W.custom_frame = o->titlebar > 0;
  read_look();

  WL.surface = wl_compositor_create_surface(WL.compositor);
  wl_surface_add_listener(WL.surface, &surface_listener, nullptr);
  if (W.fractions && WL.viewporter) {
    W.fraction = wp_fractional_scale_manager_v1_get_fractional_scale(
      W.fractions, WL.surface);
    wp_fractional_scale_v1_add_listener(W.fraction, &fraction_listener,
                                        nullptr);
    W.viewport = wp_viewporter_get_viewport(WL.viewporter, WL.surface);
  }
  W.xdg = xdg_wm_base_get_xdg_surface(W.wm, WL.surface);
  xdg_surface_add_listener(W.xdg, &xdg_listener, nullptr);
  W.toplevel = xdg_surface_get_toplevel(W.xdg);
  xdg_toplevel_add_listener(W.toplevel, &toplevel_listener, nullptr);
  xdg_toplevel_set_title(W.toplevel, o->title);
  xdg_toplevel_set_app_id(W.toplevel, app_id());
  xdg_toplevel_set_min_size(W.toplevel, (i32)o->min_width, (i32)o->min_height);
  if (W.decorations) {
    W.decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(
      W.decorations, W.toplevel);
    zxdg_toplevel_decoration_v1_add_listener(W.decoration, &decoration_listener,
                                             nullptr);
    zxdg_toplevel_decoration_v1_set_mode(
      W.decoration, W.custom_frame
                      ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE
                      : ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
  }
  W.floating_w = (i32)o->width;
  W.floating_h = (i32)o->height;
  wl_surface_commit(WL.surface);
  while (!W.configured) {
    if (wl_display_dispatch(WL.display) < 0) {
      die("wayland: no configure");
    }
  }
  // a window larger than the screen starts as large as the screen allows.
  if (W.bounds_w > 0 && W.bounds_h > 0 && !W.maximized && !W.fullscreen &&
      (W.width > W.bounds_w || W.height > W.bounds_h)) {
    W.width = W.floating_w = MIN(W.width, W.bounds_w);
    W.height = W.floating_h = MIN(W.height, W.bounds_h);
  }
  // the buttons the window manager has no use for are left out.
  if (W.caps_known) {
    char *layout = read_layout();
    set_layout(layout);
    free(layout);
  }

  VkWaylandSurfaceCreateInfoKHR info = {
    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
    .display = WL.display,
    .surface = WL.surface};
  if (vkCreateWaylandSurfaceKHR(v->instance, &info, nullptr, &W.surface) !=
      VK_SUCCESS) {
    die("vulkan: no wayland surface");
  }
  VkSemaphoreCreateInfo sem = {.sType =
                                 VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (u32 i = 0; i < FRAMES; i++) {
    vkCreateSemaphore(v->device, &sem, nullptr, &W.acquired[i]);
  }

  if (WL.seat) {
    civue__clipboard_seat();
    if (W.tablets) {
      W.tablet_seat = zwp_tablet_manager_v2_get_tablet_seat(W.tablets, WL.seat);
      zwp_tablet_seat_v2_add_listener(W.tablet_seat, &tablet_seat_listener,
                                      nullptr);
    }
    if (W.text_inputs) {
      W.text_input =
        zwp_text_input_manager_v3_get_text_input(W.text_inputs, WL.seat);
      zwp_text_input_v3_add_listener(W.text_input, &text_listener, nullptr);
    }
  }
  if (W.exporter) {
    W.exported = zxdg_exporter_v2_export_toplevel(W.exporter, WL.surface);
    zxdg_exported_v2_add_listener(W.exported, &exported_listener, nullptr);
  }
  // a launcher's token lets the window take the focus it was opened for.
  const char *token = getenv("XDG_ACTIVATION_TOKEN");
  if (token && *token && W.activation) {
    xdg_activation_v1_activate(W.activation, token, WL.surface);
  }
  unsetenv("XDG_ACTIVATION_TOKEN");

  if (o->install_menus) {
    o->install_menus();
  }
  W.gfx = civue_gfx_new((void *)v);
  W.ui = civue_ui_new_sized(W.gfx, scale_factor(), ui_size);
  if (o->icons) {
    civue_icons_use(W.ui->icons, o->icons, o->nicons);
  }
  civue__cursors_use(o);
  civue__cursors_set_scale(scale_factor());
  W.app_cb = make(W.ui, arg);
  want();
  for (;;) {
    pump(next_wait());
    repeat_keys();
    pthread_mutex_lock(&W.lock);
    char *layout = W.layout_next;
    W.layout_next = nullptr;
    pthread_mutex_unlock(&W.lock);
    if (layout) {
      char *gtk = *layout ? nullptr : gtk_layout();
      set_layout(*layout ? layout : gtk);
      free(gtk);
      free(layout);
      want();
    }
    if (W.timer_at && now_s() >= W.timer_at) {
      W.timer_at = 0;
      want();
    }
    if (W.wanted && drawable()) {
      place_candidates();
      frame();
    }
  }
}

void civue_open_url(const char *url) {
  GError *err = nullptr;
  if (!g_app_info_launch_default_for_uri(url, nullptr, &err)) {
    LOG("could not open %s: %s", url, err ? err->message : "");
    if (err) {
      g_error_free(err);
    }
  }
}
