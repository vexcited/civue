#include "wl.h"

#include <civue/gfx.h>
#include <civue/raster.h>

#include "fractional-scale-v1-client-protocol.h"
#include "pointer-gestures-unstable-v1-client-protocol.h"
#include "text-input-unstable-v3-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
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
// a frame the cpu would take this many pixels' fill to draw goes to the gpu.
constexpr u64 COSTLY = 32'000'000;
// so do the frames of an animation that take more than this much of a
// refresh, most of it drawing, from the third in a row.
constexpr f64 TOO_LONG = 0.75;
constexpr u32 IN_A_ROW = 3;
// frames this far apart are not in a row.
constexpr f64 APART = 0.05;
// how long the swapchain stays once no frame needs the gpu, in seconds.
constexpr f64 GPU_KEPT = 2.5;
// how long a still window keeps the buffers it does not show.
constexpr f64 KEPT_FOR = 2;
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

// an output's scale, for compositors that say nothing of the surface's, and
// how many times a thousand seconds it refreshes.
typedef struct {
  struct wl_output *output;
  i32 scale;
  bool on;
  i32 refresh;
} Output;

// shared memory the cpu draws a frame into. the compositor reads it while
// `busy`, and `missing` is what the frames since it was drawn changed.
typedef struct {
  struct wl_buffer *buffer;
  u8 *pixels;
  usize size;
  u32 pw, ph;
  // whether its alpha shows, for a window with round corners.
  bool alpha;
  bool busy;
  civue_damage missing;
} Buffer;

// the one window and what drives it.
static struct {
  civue_ui *ui;
  civue_app app_cb;
  civue_window_options options;
  civue_input input;
  civue_gfx *gfx;
  // a frame is wanted, drawn once the events are in.
  bool wanted;
  // the last frame showed nothing new, so no frame callback paces the next.
  bool unchanged;
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
  struct zwlr_layer_shell_v1 *layer_shell;
  struct zwlr_layer_surface_v1 *layer;
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
  // the focused widget takes a password, told to the input method.
  bool text_secure;
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
  // the cpu draws the frames it can into buffers of the window's own, the
  // one shown and the ones it draws the next into. -1 when none is shown.
  Buffer buffers[3];
  int shown;
  // which version of the compositor's interface was bound.
  u32 compositor_version;
  // when the frame being built began, and when one was last drawn.
  f64 began, frame_at;
  // the non shown buffers go at this time, 0 for never.
  f64 trim_at;
  // how many frames in a row the cpu took too long to draw, when the last
  // did, and when a frame last needed the gpu.
  u32 slow;
  f64 slow_at, gpu_at;
  // the vulkan surface and swapchain are there: the gpu draws the frames.
  // they are made by the first frame that needs them and go GPU_KEPT after
  // the last. `gpu_none` once vulkan could not be had.
  bool gpu_on, gpu_none;
  // whether the corners of the frame shown are cut.
  bool rounded;
  // how much of each pixel of the top left corner stays, `corner_cut` a
  // side.
  u8 *corner_keep;
  u32 corner_cut;
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

  // ---- the overlays past the window ----
  // the queue of the popups below, waited on in the middle of a frame.
  struct wl_event_queue *queue;
  struct wl_compositor *queued_compositor;
  struct xdg_wm_base *queued_wm;
  // the screen around the window in points, the window's top left at 0, 0.
  // stale once the window may have moved, as wayland never says.
  civue_rect screen;
  bool screen_known, screen_stale;
  // the last frame's overlays could go past the window.
  bool outside_before;
  // a popup over the screen the overlays are drawn in when they go past
  // the window, `popup_screen` being where it was made.
  struct wl_surface *popup_surface;
  struct xdg_surface *popup_xdg;
  struct xdg_popup *popup;
  struct wp_viewport *popup_viewport;
  civue_rect popup_screen;
  bool popup_configured;
  // the compositor took it away: the overlays stay in the window until they
  // close.
  bool popup_refused;
  Buffer popup_buffers[2];
  int popup_shown;
  // what the popup's shown buffer holds, and where it takes the pointer.
  civue_draw_list popup_list, popup_last;
  civue_box popup_input;
  bool pointer_on_popup, tool_on_popup;

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
       .buffer_scale = 1,
       .shown = -1,
       .popup_shown = -1};

// the app id the desktop file and the compositor know the window by.
static const char *app_id() {
  return W.options.app_id ? W.options.app_id : "civue";
}

const char *civue__app_id() {
  return app_id();
}

void (*civue__webviews_place)(u64 frame);

// glib's default loop is run by civue's, once something built on it asks.
static bool glib_driven;

void civue__glib_drive() {
  glib_driven = true;
}

bool civue__main_thread() {
  return gettid() == getpid();
}

// wakes the loop from any thread, -1 before the window runs.
static int wake_fd = -1;
static _Atomic bool woken;

static void want() {
  W.wanted = true;
}

void civue_window_buttons_hidden([[maybe_unused]] bool hidden) {
}

void civue_window_wake() {
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
static void run_calls() {
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

bool civue_window_maximized() {
  return W.maximized;
}

const char *civue_window_handle() {
  return W.handle ? W.handle : "";
}

// ---- scale ----

// physical pixels per point.
static f32 scale_factor() {
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

static void scale_changed() {
  W.stale = true;
  civue__cursors_set_scale(scale_factor());
  W.cursor_set = false;
  want();
}

// the largest scale of the outputs the window shows on.
static void outputs_scale() {
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

static void drop_swapchain() {
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
static void make_swapchain() {
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

// the vulkan surface of the window, made when a frame first needs the gpu.
// false when there is no device to draw with.
static bool gpu_start() {
  if (W.gpu_on) {
    return true;
  }
  if (W.gpu_none) {
    return false;
  }
  const civue_vk_device *v = civue_vk();
  if (!v || !v->swapchain ||
      !civue_vk_has_instance_extension("VK_KHR_wayland_surface")) {
    LOG("vulkan: no device to draw with, the cpu draws what it can");
    W.gpu_none = true;
    return false;
  }
  VkWaylandSurfaceCreateInfoKHR info = {
    .sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR,
    .display = WL.display,
    .surface = WL.surface};
  if (vkCreateWaylandSurfaceKHR(v->instance, &info, nullptr, &W.surface) !=
      VK_SUCCESS) {
    LOG("vulkan: no wayland surface, the cpu draws what it can");
    W.gpu_none = true;
    return false;
  }
  VkSemaphoreCreateInfo sem = {.sType =
                                 VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (u32 i = 0; i < FRAMES; i++) {
    if (!W.acquired[i]) {
      vkCreateSemaphore(v->device, &sem, nullptr, &W.acquired[i]);
    }
  }
  W.gpu_on = true;
  W.stale = true;
  return true;
}

// gives the surface back to the cpu's buffers: what vulkan put on it goes
// with its swapchain and its surface.
static void gpu_stop() {
  if (!W.gpu_on) {
    return;
  }
  const civue_vk_device *v = civue_vk();
  if (W.swapchain) {
    drop_swapchain();
    vkDestroySwapchainKHR(v->device, W.swapchain, nullptr);
    W.swapchain = VK_NULL_HANDLE;
  }
  vkDestroySurfaceKHR(v->instance, W.surface, nullptr);
  W.surface = VK_NULL_HANDLE;
  W.format = 0;
  W.gpu_on = false;
}

// ---- the cpu's buffers ----

static void buffer_release(void *data, struct wl_buffer *b) {
  ((Buffer *)data)->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {.release =
                                                            buffer_release};

static void free_buffer_of(Buffer *b) {
  if (b->buffer) {
    wl_buffer_destroy(b->buffer);
    munmap(b->pixels, b->size);
  }
  *b = (Buffer){};
}

static void drop_buffer(int i) {
  free_buffer_of(&W.buffers[i]);
  if (i == W.shown) {
    W.shown = -1;
  }
}

// frees the buffers but the one shown, or that one too.
static void drop_buffers(bool shown_too) {
  for (int i = 0; i < (int)countof(W.buffers); i++) {
    if (shown_too || i != W.shown) {
      drop_buffer(i);
    }
  }
}

static bool fits(const Buffer *b, u32 pw, u32 ph, bool alpha) {
  return b->buffer && b->pw == pw && b->ph == ph && b->alpha == alpha;
}

static void make_buffer_of(Buffer *b, u32 pw, u32 ph, bool alpha) {
  usize stride = (usize)pw * 4, size = stride * ph;
  int fd = memfd_create("civue-frame", MFD_CLOEXEC);
  if (fd < 0 || ftruncate(fd, (off_t)size) < 0) {
    die("no memory for %u by %u pixels", pw, ph);
  }
  u8 *pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (pixels == MAP_FAILED) {
    die("no memory for %u by %u pixels", pw, ph);
  }
  struct wl_shm_pool *pool = wl_shm_create_pool(WL.shm, fd, (i32)size);
  // the cpu draws blue first, premultiplied, as wayland's argb is in memory.
  *b = (Buffer){.buffer = wl_shm_pool_create_buffer(
                  pool, 0, (i32)pw, (i32)ph, (i32)stride,
                  alpha ? WL_SHM_FORMAT_ARGB8888 : WL_SHM_FORMAT_XRGB8888),
                .pixels = pixels,
                .size = size,
                .pw = pw,
                .ph = ph,
                .alpha = alpha,
                .missing = {.all = true}};
  wl_shm_pool_destroy(pool);
  close(fd);
  wl_buffer_add_listener(b->buffer, &buffer_listener, b);
}

static void make_buffer(int i, u32 pw, u32 ph, bool alpha) {
  make_buffer_of(&W.buffers[i], pw, ph, alpha);
}

// a buffer to draw the next frame into. the one shown when the compositor
// has copied it already, as most do: it is drawn over in place and one
// buffer is enough. else one the compositor is done with, which gives its
// place to a new one when there is no other.
static int free_buffer(u32 pw, u32 ph, bool alpha) {
  if (W.shown >= 0 && !W.buffers[W.shown].busy) {
    return W.shown;
  }
  int empty = -1, busy = -1, found = -1;
  for (int i = 0; i < (int)countof(W.buffers); i++) {
    if (i == W.shown) {
      continue;
    }
    if (!W.buffers[i].buffer) {
      empty = i;
    }
    else if (W.buffers[i].busy) {
      busy = i;
    }
    else if (found < 0) {
      found = i;
    }
    else {
      drop_buffer(i);
    }
  }
  if (found >= 0) {
    return found;
  }
  if (empty < 0) {
    drop_buffer(busy);
    empty = busy;
  }
  make_buffer(empty, pw, ph, alpha);
  return empty;
}

// ---- the system's look ----

static GDBusConnection *bus;

static void connect_bus() {
  bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
}

static GDBusConnection *session_bus() {
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
  for (u32 m = 0; m < countof(methods); m++) {
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
static char *gtk_layout() {
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
  for (u32 i = 0; i < countof(W.start); i++) {
    menu |= W.start[i] == CIVUE_TITLE_MENU || W.end[i] == CIVUE_TITLE_MENU;
  }
  if (!menu) {
    memmove(W.start + 1, W.start, 3 * sizeof *W.start);
    W.start[0] = CIVUE_TITLE_MENU;
  }
}

static char *read_layout() {
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
static void read_look() {
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

// how much of each pixel of the top left corner stays once it is cut round,
// `pixels` a side: all of it inside a circle of `pixels` around the corner's
// far end.
static void corner_table(u32 pixels) {
  if (W.corner_keep && W.corner_cut == pixels) {
    return;
  }
  free(W.corner_keep);
  W.corner_keep = xmalloc((usize)pixels * pixels);
  W.corner_cut = pixels;
  f32 r = (f32)pixels;
  for (u32 y = 0; y < pixels; y++) {
    for (u32 x = 0; x < pixels; x++) {
      f32 dx = r - (x + 0.5f), dy = r - (y + 0.5f);
      f32 outside = fminf(fmaxf(sqrtf(dx * dx + dy * dy) - r + 0.5f, 0), 1);
      W.corner_keep[(usize)y * pixels + x] = (u8)lroundf((1 - outside) * 255);
    }
  }
}

// the corners' mask for the gpu, black where the window is cut, its
// coverage what the table takes away, for the top left.
static void corner_mask(u32 pixels) {
  if (W.corner && W.corner_pixels == pixels) {
    return;
  }
  if (W.corner) {
    civue_gfx_free_texture(W.gfx, W.corner);
  }
  corner_table(pixels);
  u8 *px = xcalloc((usize)pixels * pixels, 4);
  for (usize i = 0; i < (usize)pixels * pixels; i++) {
    px[i * 4 + 3] = (u8)(255 - W.corner_keep[i]);
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

static void toggle_maximized() {
  if (!W.toplevel) {
    return;
  }
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

// whether the pointer, or the stylus when it is near, is over the popup of
// the overlays.
static bool over_popup() {
  return WL.tool ? W.tool_on_popup : W.pointer_on_popup;
}

// where a point of `s` is in the window's points.
static civue_vec2 in_window(struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
  civue_vec2 at = v2((f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
  if (s && s == W.popup_surface) {
    at = v2(at.x + (f32)W.popup_screen.x0, at.y + (f32)W.popup_screen.y0);
  }
  return at;
}

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
  // the window may have moved since the screen was found.
  if (down) {
    W.screen_stale = true;
  }
  if (down && !over_popup() && frame_press(code, p, i == 0 ? W.clicks : 1)) {
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

static void released_all() {
  for (u32 i = 0; i < countof(W.input.down); i++) {
    if (W.input.down[i]) {
      W.input.down[i] = false;
      W.input.released[i] = true;
    }
  }
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

// ---- the pointer ----

static void pointer_enter(void *data, struct wl_pointer *p, u32 serial,
                          struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
  WL.pointer_serial = serial;
  W.cursor_set = false;
  W.cursor_edge = 0;
  civue__cursors_entered();
  W.pointer_on_popup = s && s == W.popup_surface;
  W.screen_stale |= !W.pointer_on_popup;
  W.pointer = in_window(s, x, y);
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
  W.pointer = in_window(W.pointer_on_popup ? W.popup_surface : nullptr, x, y);
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
  W.tool_on_popup = s && s == W.popup_surface;
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
  W.tool_at = in_window(W.tool_on_popup ? W.popup_surface : nullptr, x, y);
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
static void repeat_keys() {
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

// the content type only changes with a committed enable, which also forgets
// the cursor rectangle.
static void text_enable(struct zwp_text_input_v3 *t) {
  zwp_text_input_v3_enable(t);

  if (W.text_secure) {
    zwp_text_input_v3_set_content_type(
      t,
      ZWP_TEXT_INPUT_V3_CONTENT_HINT_HIDDEN_TEXT |
        ZWP_TEXT_INPUT_V3_CONTENT_HINT_SENSITIVE_DATA,
      ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_PASSWORD);
  }
  else {
    zwp_text_input_v3_set_content_type(
      t, ZWP_TEXT_INPUT_V3_CONTENT_HINT_NONE,
      ZWP_TEXT_INPUT_V3_CONTENT_PURPOSE_NORMAL);
  }

  zwp_text_input_v3_commit(t);
  W.candidates = (civue_rect){};
}

static void text_enter(void *data, struct zwp_text_input_v3 *t,
                       struct wl_surface *s) {
  text_enable(t);
  W.text_on = true;
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

static void set_secure(bool on) {
  if (W.text_secure == on) {
    return;
  }

  W.text_secure = on;
  W.input.preedit.len = 0;

  if (W.text_on) {
    text_enable(W.text_input);
  }
}

// the input method's candidates open under the pointer.
static void place_candidates() {
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
  Output *out = output_of(o);
  if (out && flags & WL_OUTPUT_MODE_CURRENT) {
    out->refresh = refresh;
  }
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
  W.screen_stale = true;
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

// ---- the layer surface ----

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *s,
                            u32 serial, u32 w, u32 h) {
  i32 width = w ? (i32)w : (i32)W.options.width;
  i32 height = h ? (i32)h : (i32)W.options.height;
  width = MAX(width, 1);
  height = MAX(height, 1);
  if (width != W.width || height != W.height) {
    W.stale = true;
  }
  W.width = W.floating_w = width;
  W.height = W.floating_h = height;
  zwlr_layer_surface_v1_ack_configure(s, serial);
  W.configured = true;
  W.reconfigured = true;
  want();
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *s) {
  exit(0);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
  .configure = layer_configure, .closed = layer_closed};

static void layer_open(const civue_window_options *o) {
  W.layer = zwlr_layer_shell_v1_get_layer_surface(
    W.layer_shell, WL.surface, nullptr, (u32)o->layer - 1,
    o->title ? o->title : app_id());
  zwlr_layer_surface_v1_add_listener(W.layer, &layer_listener, nullptr);
  zwlr_layer_surface_v1_set_size(W.layer, (u32)o->width, (u32)o->height);
  zwlr_layer_surface_v1_set_anchor(W.layer, o->edges);
  zwlr_layer_surface_v1_set_exclusive_zone(W.layer, o->exclusive);
  zwlr_layer_surface_v1_set_keyboard_interactivity(
    W.layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
}

void civue_layer_keyboard(bool on, bool exclusive) {
  if (!W.layer) {
    return;
  }
  u32 mode = !on        ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE
             : exclusive ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                         : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND;
  // on demand came with the shell's fourth version.
  if (mode == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND &&
      zwlr_layer_surface_v1_get_version(W.layer) < 4) {
    mode = ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE;
  }
  zwlr_layer_surface_v1_set_keyboard_interactivity(W.layer, mode);
  wl_surface_commit(WL.surface);
}

void civue_glib_drive() {
  civue__glib_drive();
}

void civue_layer_resize(float width, float height) {
  if (!W.layer) {
    return;
  }
  zwlr_layer_surface_v1_set_size(W.layer, (u32)width, (u32)height);
  W.options.width = width;
  W.options.height = height;
  wl_surface_commit(WL.surface);
}

void civue_layer_input(const civue_rect *rects, int n) {
  if (!W.layer) {
    return;
  }
  struct wl_region *region = nullptr;
  if (n) {
    region = wl_compositor_create_region(WL.compositor);
    for (int i = 0; i < n; i++) {
      civue_rect r = rects[i];
      i32 x = (i32)floorf(r.x0), y = (i32)floorf(r.y0);
      wl_region_add(region, x, y, (i32)ceilf(r.x1) - x, (i32)ceilf(r.y1) - y);
    }
  }
  wl_surface_set_input_region(WL.surface, region);
  if (region) {
    wl_region_destroy(region);
  }
  wl_surface_commit(WL.surface);
}

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

void civue_window_raise() {
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
    W.compositor_version = MIN(version, 6u);
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
  else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
    W.layer_shell = bind(r, name, &zwlr_layer_shell_v1_interface, version, 4);
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
static bool drawable() {
  return W.configured && !W.suspended && (!W.frame_cb || W.reconfigured);
}

// the cursor the pointer shows: a resize at the window's edges, the app's
// elsewhere.
static void update_cursor() {
  u32 edge = W.input.has_mouse && !over_popup() ? edge_at(W.input.mouse) : 0;
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

// how long a frame may take: a part of the refresh of the fastest display
// the window is on.
static f64 time_allowed() {
  i32 refresh = 0;
  vec_each(&W.outputs, o) {
    if (o->on) {
      refresh = MAX(refresh, o->refresh);
    }
  }
  return TOO_LONG * 1000 / (refresh > 0 ? refresh : 60'000);
}

static bool find_pixels(void *ctx, civue_texture t, civue_pixels *out) {
  return civue_gfx_pixels(ctx, t, out);
}

// the images the cpu draws: the ones civue uploaded.
static civue_raster_images images_of() {
  return (civue_raster_images){find_pixels, W.gfx};
}

// what a commit of the surface carries besides its buffer: its scale, and
// the callback that paces the next frame.
static void surface_state() {
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
}

// frees the buffers the window does not show once it stopped drawing.
static void trim_buffers() {
  f64 due = W.frame_at + KEPT_FOR;
  if (now_s() < due) {
    W.trim_at = due;
    return;
  }
  W.trim_at = 0;
  drop_buffers(false);
}

typedef struct {
  Buffer *buffer;
  u32 cut;
} Corners;

// cuts the corners of a box the cpu just drew: its pixels there are whole
// again, and are taken away once.
static void cut_box(void *ctx, civue_box b) {
  Corners *c = ctx;
  Buffer *buf = c->buffer;
  i32 n = (i32)c->cut, pw = (i32)buf->pw, ph = (i32)buf->ph;
  for (u32 corner = 0; corner < 4; corner++) {
    bool right = corner & 1, bottom = corner & 2;
    i32 cx = right ? pw - n : 0, cy = bottom ? ph - n : 0;
    i32 x0 = MAX(b.x0, cx), x1 = MIN(b.x1, cx + n);
    i32 y0 = MAX(b.y0, cy), y1 = MIN(b.y1, cy + n);
    for (i32 y = y0; y < y1; y++) {
      i32 ty = bottom ? n - 1 - (y - cy) : y - cy;
      u8 *row = buf->pixels + (usize)y * buf->pw * 4;
      for (i32 x = x0; x < x1; x++) {
        i32 tx = right ? n - 1 - (x - cx) : x - cx;
        u32 keep = W.corner_keep[(usize)ty * n + tx];
        if (keep == 255) {
          continue;
        }
        u8 *px = row + (usize)x * 4;
        for (u32 k = 0; k < 4; k++) {
          px[k] = (u8)((px[k] * keep + 127) / 255);
        }
      }
    }
  }
}

static void damage_box(void *ctx, civue_box b) {
  wl_surface_damage_buffer(WL.surface, b.x0, b.y0, b.x1 - b.x0, b.y1 - b.y0);
}

// draws what changed on the cpu, into a buffer of the window's own.
static void draw_on_cpu(const civue_draw_list *list, civue_color background,
                        civue_damage changes, u32 pw, u32 ph, f32 scale,
                        bool round) {
  f64 drawing = now_s();
  bool resized = W.reconfigured;
  bool alpha = round || background.a < 1;
  for (int i = 0; i < (int)countof(W.buffers); i++) {
    if (W.buffers[i].buffer && !fits(&W.buffers[i], pw, ph, alpha)) {
      drop_buffer(i);
    }
  }
  int shown = W.shown;
  int at = free_buffer(pw, ph, alpha);
  Buffer *b = &W.buffers[at];

  // what the buffer misses of the frames since it was drawn is drawn again
  // with what changed, when that is not all of it.
  civue_damage drawn = {.all = true};
  if (shown >= 0 && !changes.all) {
    if (at == shown) {
      drawn = changes;
    }
    else if (b->missing.all) {
      // a buffer that misses everything starts as the one shown.
      memcpy(b->pixels, W.buffers[shown].pixels, b->size);
      drawn = changes;
    }
    else {
      drawn = b->missing;
      civue_damage_join(&drawn, &changes);
    }
  }
  else {
    changes = (civue_damage){.all = true};
  }

  civue_surface pixels = {b->pixels, pw, ph, (usize)pw * 4};
  civue_raster_images images = images_of();
  civue_raster(&pixels, list, background, &drawn, civue_gfx_atlas(W.gfx),
               civue_gfx_color_atlas(W.gfx), &images);
  u32 cut = round ? (u32)lroundf(W.radius * scale) : 0;
  if (cut && cut * 2 <= MIN(pw, ph)) {
    corner_table(cut);
    Corners corners = {b, cut};
    civue_damage_each(&drawn, pw, ph, cut_box, &corners);
  }

  for (int i = 0; i < (int)countof(W.buffers); i++) {
    if (W.buffers[i].buffer) {
      civue_damage_join(&W.buffers[i].missing, &changes);
    }
  }
  b->missing = (civue_damage){};
  b->busy = true;
  W.shown = at;

  wl_surface_attach(WL.surface, b->buffer, 0, 0);
  if (changes.all || W.compositor_version < 4) {
    wl_surface_damage(WL.surface, 0, 0, INT32_MAX, INT32_MAX);
  }
  else {
    civue_damage_each(&changes, pw, ph, damage_box, nullptr);
  }
  surface_state();
  wl_surface_commit(WL.surface);

  // an animation the cpu is too slow for is the gpu's: its frames come
  // late, and the drawing is what takes the time. a resize's frames are
  // drawn as its sizes come, late for none.
  f64 now = now_s();
  f64 whole = now - W.began;
  if (resized || whole <= time_allowed() || (now - drawing) * 2 < whole) {
    W.slow = 0;
  }
  else {
    W.slow = now - W.slow_at < APART ? W.slow + 1 : 1;
    W.slow_at = now;
  }
}

// draws the whole frame on the gpu. false when the swapchain gave no image,
// the frame is then built again.
static bool draw_on_gpu(const civue_draw_list *list, civue_color background,
                        u32 pw, u32 ph, f32 scale, bool round) {
  const civue_vk_device *v = civue_vk();
  // the frame that last used this slot's semaphore is done.
  civue_vk_wait(W.frame_value[W.frame]);
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
    return false;
  }
  if (r == VK_SUBOPTIMAL_KHR) {
    W.stale = true;
  }
  else if (r != VK_SUCCESS) {
    die("vulkan: acquire failed (%d)", r);
  }
  civue_vk_tex *target = W.images[image];
  VkSemaphore acquired = W.acquired[W.frame];
  // the mask uploads with the frame that first draws it.
  round = round && W.translucent;
  if (round) {
    round_corners(target->width, target->height, scale);
  }
  VkCommandBuffer cb = civue_gfx_vk_begin(W.gfx);
  civue_gfx_encode(W.gfx, cb, target, target->width, target->height, list,
                   &background);
  if (round) {
    civue_gfx_vk_erase(W.gfx, cb, target, target->width, target->height,
                       &W.corners);
  }
  civue_vk_layout(cb, target, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  W.frame_value[W.frame] =
    civue_gfx_vk_submit(W.gfx, cb, acquired, W.rendered[image]);
  // the surface's scale and the next frame's callback come with the commit
  // the present makes.
  surface_state();
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
  // the cpu's next frame is drawn anew.
  drop_buffers(true);
  return true;
}

// shows the frame the ui built: on the cpu when it can draw it and has the
// time to, in memory the compositor reads. vulkan is only there for the
// frames that need it, and for a while after.
static void present(civue_color background, u32 pw, u32 ph, f32 scale) {
  civue_damage changes;
  const civue_draw_list *list =
    civue_frame_changes(W.ui, background, pw, ph, &changes);
  bool round =
    W.custom_frame && W.radius > 0 && !W.maximized && !W.fullscreen && !W.tiled;
  if (round != W.rounded) {
    W.rounded = round;
    changes = (civue_damage){.all = true};
  }

  // an image the app registered is the gpu's, and may have been drawn into
  // since.
  bool fixed = civue_gfx_static(W.gfx, list);
  f64 now = now_s();
  bool anew = changes.all || W.shown < 0 ||
              !fits(&W.buffers[W.shown], pw, ph, round || background.a < 1);
  bool needed = !fixed;
  if (!needed) {
    civue_damage all = {.all = true};
    civue_raster_images images = images_of();
    u64 cost;
    // an animation the cpu was too slow for stays with the gpu until it
    // stops.
    needed =
      !civue_raster_cost(list, anew ? &all : &changes, pw, ph, &images,
                         &cost) ||
      cost > COSTLY ||
      (W.slow >= IN_A_ROW && now - (W.gpu_on ? W.gpu_at : W.slow_at) < APART);
  }

  // CIVUE_DRAW says who draws whatever the frame: gpu or cpu.
  static const char *forced;
  if (!forced) {
    forced = getenv("CIVUE_DRAW");
    forced = forced ? forced : "";
  }
  if (!strcmp(forced, "gpu")) {
    needed = true;
  }
  else if (!strcmp(forced, "cpu")) {
    needed = !fixed;
  }

  bool by_gpu = needed && gpu_start();
  if (by_gpu) {
    W.gpu_at = now;
  }
  else if (W.gpu_on) {
    // the swapchain stays for the frames that follow one it was needed
    // for, then the cpu takes the window back.
    if (now - W.gpu_at < GPU_KEPT) {
      by_gpu = true;
    }
    else {
      gpu_stop();
    }
  }

  // the window shows this frame already.
  bool same =
    by_gpu ? fixed && !changes.all && !changes.any : !anew && !changes.any;
  W.unchanged = same;
  if (same) {
    if (W.reconfigured) {
      W.reconfigured = false;
      wl_surface_commit(WL.surface);
    }
  }
  else if (by_gpu) {
    if (!draw_on_gpu(list, background, pw, ph, scale, round)) {
      civue_frame_forget(W.ui);
      want();
    }
  }
  else {
    draw_on_cpu(list, background, changes, pw, ph, scale, round);
    W.frame_at = now_s();
    if (!W.trim_at) {
      W.trim_at = W.frame_at + KEPT_FOR;
    }
  }

  if (W.gpu_on) {
    schedule_timer(W.gpu_at + GPU_KEPT);
  }
}

// ---- the overlays past the window ----

// a popup of the window, on the queue waited on while a frame is built.
typedef struct {
  struct wl_surface *surface;
  struct xdg_surface *xdg;
  struct xdg_popup *popup;
} Popup;

// a popup `w` by `h` points, its top left `x` and `y` from the window's.
static struct xdg_positioner *positioner(i32 x, i32 y, i32 w, i32 h,
                                         u32 adjust) {
  struct xdg_positioner *p = xdg_wm_base_create_positioner(W.queued_wm);
  xdg_positioner_set_size(p, w, h);
  xdg_positioner_set_anchor_rect(p, 0, 0, 1, 1);
  xdg_positioner_set_anchor(p, XDG_POSITIONER_ANCHOR_TOP_LEFT);
  xdg_positioner_set_gravity(p, XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT);
  xdg_positioner_set_offset(p, x, y);
  xdg_positioner_set_constraint_adjustment(p, adjust);
  return p;
}

static Popup popup_make(struct xdg_positioner *p,
                        const struct xdg_surface_listener *surface,
                        const struct xdg_popup_listener *popup, void *data) {
  Popup made = {};
  made.surface = wl_compositor_create_surface(W.queued_compositor);
  made.xdg = xdg_wm_base_get_xdg_surface(W.queued_wm, made.surface);
  xdg_surface_add_listener(made.xdg, surface, data);
  made.popup = xdg_surface_get_popup(made.xdg, W.xdg, p);
  xdg_popup_add_listener(made.popup, popup, data);
  xdg_positioner_destroy(p);
  return made;
}

static void popup_free(Popup *p) {
  if (p->popup) {
    xdg_popup_destroy(p->popup);
  }
  if (p->xdg) {
    xdg_surface_destroy(p->xdg);
  }
  if (p->surface) {
    wl_surface_destroy(p->surface);
  }
  *p = (Popup){};
}

typedef struct {
  i32 x, y;
  bool placed, configured;
} Probe;

static void probe_configure(void *data, struct xdg_surface *s, u32 serial) {
  ((Probe *)data)->configured = true;
}

static void probe_placed(void *data, struct xdg_popup *p, i32 x, i32 y, i32 w,
                         i32 h) {
  Probe *probe = data;
  probe->x = x;
  probe->y = y;
  probe->placed = true;
}

static void probe_done(void *data, struct xdg_popup *p) {
}

static void probe_repositioned(void *data, struct xdg_popup *p, u32 token) {
}

static const struct xdg_surface_listener probe_surface_listener = {
  .configure = probe_configure};

static const struct xdg_popup_listener probe_listener = {
  .configure = probe_placed,
  .popup_done = probe_done,
  .repositioned = probe_repositioned};

// finds where the screen is around the window. wayland never says, but a
// popup asked for far past a corner of the screen is slid back onto it by
// the compositor, to that very corner. they are never shown.
static void probe_screen() {
  constexpr i32 FAR = 1 << 15;
  constexpr u32 SLIDE = XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X |
                        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_Y;
  Probe probes[2] = {};
  Popup popups[2];
  W.screen_stale = false;
  for (u32 i = 0; i < 2; i++) {
    i32 far = i ? FAR : -FAR;
    popups[i] = popup_make(positioner(far, far, 1, 1, SLIDE),
                           &probe_surface_listener, &probe_listener,
                           &probes[i]);
    wl_surface_commit(popups[i].surface);
  }
  wl_display_roundtrip_queue(WL.display, W.queue);
  for (u32 i = 0; i < 2; i++) {
    popup_free(&popups[i]);
  }
  // a compositor that slides nothing leaves them out there.
  Probe a = probes[0], b = probes[1];
  W.screen_known = a.placed && b.placed && abs(a.x) < FAR / 2 &&
                   abs(a.y) < FAR / 2 && abs(b.x) < FAR / 2 &&
                   abs(b.y) < FAR / 2 && a.x <= b.x && a.y <= b.y;
  if (W.screen_known) {
    W.screen = rect(a.x, a.y, b.x + 1, b.y + 1);
  }
}

static void popup_configure(void *data, struct xdg_surface *s, u32 serial) {
  xdg_surface_ack_configure(s, serial);
  W.popup_configured = true;
}

static void popup_placed(void *data, struct xdg_popup *p, i32 x, i32 y, i32 w,
                         i32 h) {
}

static void popup_dismissed(void *data, struct xdg_popup *p) {
  W.popup_refused = true;
  want();
}

static const struct xdg_surface_listener popup_surface_listener = {
  .configure = popup_configure};

static const struct xdg_popup_listener popup_listener = {
  .configure = popup_placed,
  .popup_done = popup_dismissed,
  .repositioned = probe_repositioned};

static void popup_close() {
  if (!W.popup) {
    return;
  }
  if (W.popup_viewport) {
    wp_viewport_destroy(W.popup_viewport);
  }
  Popup p = {W.popup_surface, W.popup_xdg, W.popup};
  popup_free(&p);
  for (u32 i = 0; i < countof(W.popup_buffers); i++) {
    free_buffer_of(&W.popup_buffers[i]);
  }
  civue_dl_clear(&W.popup_last);
  W.popup_surface = nullptr;
  W.popup_xdg = nullptr;
  W.popup = nullptr;
  W.popup_viewport = nullptr;
  W.popup_configured = false;
  W.popup_shown = -1;
  W.popup_input = (civue_box){};
  W.pointer_on_popup = W.tool_on_popup = false;
}

// the popup over the screen, made once the compositor configured it. false
// when it would not.
static bool popup_ready() {
  civue_rect s = W.screen;
  if (W.popup && memcmp(&W.popup_screen, &s, sizeof s)) {
    popup_close();
  }
  if (W.popup) {
    return W.popup_configured;
  }
  Popup p =
    popup_make(positioner((i32)s.x0, (i32)s.y0, (i32)rect_w(s), (i32)rect_h(s),
                          XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_NONE),
               &popup_surface_listener, &popup_listener, nullptr);
  W.popup_surface = p.surface;
  W.popup_xdg = p.xdg;
  W.popup = p.popup;
  W.popup_screen = s;
  if (W.scale120) {
    W.popup_viewport = wp_viewporter_get_viewport(WL.viewporter, p.surface);
  }
  // it takes the pointer where the overlays are only, once drawn.
  struct wl_region *none = wl_compositor_create_region(WL.compositor);
  wl_surface_set_input_region(p.surface, none);
  wl_region_destroy(none);
  wl_surface_commit(p.surface);
  wl_display_roundtrip_queue(WL.display, W.queue);
  if (!W.popup_configured) {
    popup_close();
    W.popup_refused = true;
  }
  return W.popup_configured;
}

// the box of pixels around what `list` draws.
static civue_box drawn_box(const civue_draw_list *list) {
  f32 x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
  vec_each(&list->quads, q) {
    f32 l = fmaxf(q->rect[0], q->clip[0]), t = fmaxf(q->rect[1], q->clip[1]);
    f32 r = fminf(q->rect[0] + q->rect[2], q->clip[2]);
    f32 b = fminf(q->rect[1] + q->rect[3], q->clip[3]);
    if (l < r && t < b) {
      x0 = fminf(x0, l), y0 = fminf(y0, t);
      x1 = fmaxf(x1, r), y1 = fmaxf(y1, b);
    }
  }
  if (x0 >= x1) {
    return (civue_box){};
  }
  return (civue_box){(i32)floorf(x0), (i32)floorf(y0), (i32)ceilf(x1),
                     (i32)ceilf(y1)};
}

// whether the overlays draw past the window's `pw` by `ph` pixels.
static bool overlays_past(const civue_ui *ui, u32 pw, u32 ph) {
  civue_box b = drawn_box(&ui->overlay);
  return b.x0 < b.x1 &&
         (b.x0 < 0 || b.y0 < 0 || b.x1 > (i32)pw || b.y1 > (i32)ph);
}

static void popup_damage_box(void *ctx, civue_box b) {
  wl_surface_damage_buffer(W.popup_surface, b.x0, b.y0, b.x1 - b.x0,
                           b.y1 - b.y0);
}

// draws the overlays alone in the popup, on the cpu.
static void draw_popup(f32 scale) {
  civue_rect s = W.popup_screen;
  i32 w = (i32)rect_w(s), h = (i32)rect_h(s);
  u32 pw, ph;
  if (W.scale120) {
    pw = (u32)MAX(lround(w * W.scale120 / 120.0), 1);
    ph = (u32)MAX(lround(h * W.scale120 / 120.0), 1);
  }
  else {
    pw = (u32)MAX(w * W.buffer_scale, 1);
    ph = (u32)MAX(h * W.buffer_scale, 1);
  }
  civue_dl_clear(&W.popup_list);
  civue_dl_append(&W.popup_list, &W.ui->overlay);
  civue_dl_shift(&W.popup_list, -roundf((f32)s.x0 * scale),
                 -roundf((f32)s.y0 * scale));

  // the buffer shown is drawn over in place once the compositor is done with
  // it, the other one anew.
  int at = W.popup_shown;
  civue_damage drawn = {.all = true};
  if (at >= 0 && !W.popup_buffers[at].busy &&
      fits(&W.popup_buffers[at], pw, ph, true)) {
    civue_damage_between(&W.popup_last, &W.popup_list, pw, ph, &drawn);
  }
  else {
    at = at == 0 ? 1 : 0;
    Buffer *b = &W.popup_buffers[at];
    if (b->busy || !fits(b, pw, ph, true)) {
      free_buffer_of(b);
      make_buffer_of(b, pw, ph, true);
    }
  }

  // in points, as the input region goes.
  civue_box box = drawn_box(&W.popup_list);
  civue_box input = {(i32)floorf((f32)box.x0 / scale),
                     (i32)floorf((f32)box.y0 / scale),
                     (i32)ceilf((f32)box.x1 / scale),
                     (i32)ceilf((f32)box.y1 / scale)};
  bool reshaped = memcmp(&input, &W.popup_input, sizeof input);

  civue_draw_list swap = W.popup_last;
  W.popup_last = W.popup_list;
  W.popup_list = swap;
  if (at == W.popup_shown && !drawn.all && !drawn.any && !reshaped) {
    return;
  }

  Buffer *b = &W.popup_buffers[at];
  civue_surface pixels = {b->pixels, pw, ph, (usize)pw * 4};
  civue_raster_images images = images_of();
  civue_raster(&pixels, &W.popup_last, (civue_color){}, &drawn,
               civue_gfx_atlas(W.gfx), civue_gfx_color_atlas(W.gfx), &images);

  if (reshaped) {
    W.popup_input = input;
    struct wl_region *region = wl_compositor_create_region(WL.compositor);
    wl_region_add(region, input.x0, input.y0, input.x1 - input.x0,
                  input.y1 - input.y0);
    wl_surface_set_input_region(W.popup_surface, region);
    wl_region_destroy(region);
  }
  wl_surface_attach(W.popup_surface, b->buffer, 0, 0);
  if (drawn.all || at != W.popup_shown || W.compositor_version < 4) {
    wl_surface_damage(W.popup_surface, 0, 0, INT32_MAX, INT32_MAX);
  }
  else {
    civue_damage_each(&drawn, pw, ph, popup_damage_box, nullptr);
  }
  if (W.popup_viewport) {
    wp_viewport_set_destination(W.popup_viewport, w, h);
  }
  else {
    wl_surface_set_buffer_scale(W.popup_surface, W.buffer_scale);
  }
  wl_surface_commit(W.popup_surface);
  b->busy = true;
  W.popup_shown = at;
}

static void frame() {
  W.wanted = false;
  W.began = now_s();
  u32 pw, ph;
  pixel_size(&pw, &ph);
  f32 scale = scale_factor();
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
  // overlays may go as far as the screen around a toplevel.
  civue_rect window = rect(0, 0, pw / scale, ph / scale);
  bool probed = W.xdg && W.screen_stale;
  if (probed) {
    probe_screen();
  }
  ui->screen_known = true;
  ui->screen = W.xdg && W.screen_known ? W.screen : window;
  // what a pass asks of the window is asked once, the passes after it begin
  // without it and without the input that made it.
  bool maximize = false, minimize = false, closing = false, repaint = false;
  f64 repaint_at = 0;
  for (u32 pass = 0; pass < 4; pass++) {
    civue_begin_frame(ui, v2(pw / scale, ph / scale), scale, &W.input);
    W.app_cb.frame(W.app_cb.ctx, ui);
    bool settled = civue_end_frame(ui);
    maximize |= ui->toggle_maximize;
    minimize |= ui->minimize_window;
    closing |= ui->close_window;
    repaint |= ui->repaint;
    if (ui->repaint_at && (!repaint_at || ui->repaint_at < repaint_at)) {
      repaint_at = ui->repaint_at;
    }
    // overlays that just began to go past the window go by the screen found
    // anew, the window may have moved since: hyprland, for one, says where
    // it is in the middle of its opening animation.
    if (W.xdg && !probed && ui->overlay_outside && !W.outside_before) {
      civue_rect was = W.screen;
      bool knew = W.screen_known;
      probed = true;
      probe_screen();
      if (knew != W.screen_known || memcmp(&was, &W.screen, sizeof was)) {
        ui->screen = W.screen_known ? W.screen : window;
        continue;
      }
    }
    if (settled) {
      break;
    }
  }
  W.outside_before = ui->overlay_outside;

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
  // the frame is shown without its overlays when they go past it: they
  // have a popup of their own.
  bool past = W.xdg && W.screen_known && ui->overlay_outside &&
              overlays_past(ui, pw, ph);
  if (!past) {
    W.popup_refused = false;
  }
  ui->overlay_apart = past && !W.popup_refused && popup_ready();
  present(background, pw, ph, scale);
  if (ui->overlay_apart) {
    draw_popup(scale);
  }
  else {
    popup_close();
  }
  update_cursor();
  set_secure(ui->secure_input);
  if (ui->toggle_maximize && !W.fullscreen) {
    toggle_maximized();
  }
  if (ui->minimize_window && W.toplevel) {
    xdg_toplevel_set_minimized(W.toplevel);
  }
  if (ui->close_window) {
    exit(0);
  }
  if (ui->toggle_fullscreen && W.toplevel) {
    ui->toggle_fullscreen = false;
    if (W.fullscreen) {
      xdg_toplevel_unset_fullscreen(W.toplevel);
    }
    else {
      xdg_toplevel_set_fullscreen(W.toplevel, nullptr);
    }
  }
  // a frame asked again after one that changed nothing waits a display
  // frame's time, the compositor's callback that paces the others never
  // comes for it.
  if (ui->repaint && W.unchanged) {
    schedule_timer(now_s() + 1.0 / 60);
  }
  else if (ui->repaint) {
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
  bool came = false;
  while (wl_display_prepare_read(d) != 0) {
    wl_display_dispatch_pending(d);
    came = true;
  }
  // what was just handled may have made a frame drawable, which the wait
  // asked for before it would sleep through.
  if (came && W.wanted && drawable()) {
    ms = 0;
  }
  while (wl_display_flush(d) < 0 && errno == EAGAIN) {
    struct pollfd out = {wl_display_get_fd(d), POLLOUT, 0};
    poll(&out, 1, -1);
  }
  constexpr u32 MOST = 8, GLIB_MOST = 64;
  struct pollfd p[2 + MOST + GLIB_MOST] = {{wl_display_get_fd(d), POLLIN, 0},
                                           {wake_fd, POLLIN, 0}};
  u32 nwatch = MIN(W.watches.len, MOST);
  for (u32 i = 0; i < nwatch; i++) {
    p[2 + i] = (struct pollfd){W.watches.v[i].fd, POLLIN, 0};
  }
  // glib's sources wait with the rest: its descriptors are polled here, a
  // GPollFD being a pollfd, and the wait is no longer than its timers.
  GMainContext *glib = glib_driven ? g_main_context_default() : nullptr;
  GPollFD *gfds = (GPollFD *)(p + 2 + nwatch);
  int nglib = 0, priority = 0;
  if (glib && g_main_context_acquire(glib)) {
    int timeout = -1;
    g_main_context_prepare(glib, &priority);
    nglib = g_main_context_query(glib, priority, &timeout, gfds, GLIB_MOST);
    nglib = MIN(nglib, (int)GLIB_MOST);
    if (timeout >= 0 && (ms < 0 || timeout < ms)) {
      ms = timeout;
    }
  }
  else {
    glib = nullptr;
  }
  int n = poll(p, 2 + nwatch + (u32)nglib, ms);
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
  if (wl_display_dispatch_pending(d) < 0 ||
      (W.queue && wl_display_dispatch_queue_pending(d, W.queue) < 0)) {
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
  if (glib) {
    if (g_main_context_check(glib, priority, gfds, nglib)) {
      g_main_context_dispatch(glib);
    }
    g_main_context_release(glib);
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
static i32 next_wait() {
  f64 next = W.wanted && drawable() ? now_s() : W.timer_at;
  if (W.repeat_key && (!next || W.repeat_at < next)) {
    next = W.repeat_at;
  }
  if (W.trim_at && (!next || W.trim_at < next)) {
    next = W.trim_at;
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
  if (!WL.compositor || !WL.shm || (o->layer ? !W.layer_shell : !W.wm)) {
    die(o->layer ? "wayland: the compositor has no layer shell"
                 : "wayland: the compositor has no xdg shell");
  }
  // the seat's devices, the outputs' scales.
  wl_display_roundtrip(WL.display);
  if (o->title) {
    civue__gpu_title(o->title);
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
  if (o->layer) {
    layer_open(o);
  }
  else {
    W.xdg = xdg_wm_base_get_xdg_surface(W.wm, WL.surface);
    xdg_surface_add_listener(W.xdg, &xdg_listener, nullptr);
    W.toplevel = xdg_surface_get_toplevel(W.xdg);
    xdg_toplevel_add_listener(W.toplevel, &toplevel_listener, nullptr);
    xdg_toplevel_set_title(W.toplevel, o->title);
    xdg_toplevel_set_app_id(W.toplevel, app_id());
    xdg_toplevel_set_min_size(W.toplevel, (i32)o->min_width,
                              (i32)o->min_height);
  }
  if (W.xdg) {
    W.queue = wl_display_create_queue(WL.display);
    W.queued_compositor = wl_proxy_create_wrapper(WL.compositor);
    wl_proxy_set_queue((struct wl_proxy *)W.queued_compositor, W.queue);
    W.queued_wm = wl_proxy_create_wrapper(W.wm);
    wl_proxy_set_queue((struct wl_proxy *)W.queued_wm, W.queue);
  }
  if (W.decorations && W.toplevel) {
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
  if (W.exporter && W.toplevel) {
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
  // without vulkan yet: its device is made by the first frame the gpu draws.
  W.gfx = civue_gfx_new(nullptr);
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
    if (W.trim_at && now_s() >= W.trim_at) {
      trim_buffers();
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
