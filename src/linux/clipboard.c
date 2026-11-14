#include "wl.h"

#include <civue/system.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>

// what another app offers, the types as they are announced.
typedef struct {
  struct wl_data_offer *offer;
  civue_strs mimes;
} Offer;

static struct {
  struct wl_data_device *device;
  // the selection another app holds, nullptr when none or the app's own.
  Offer *selection;
  // files dragged over the window.
  Offer *drag;
  civue_vec2 drag_at;
  // what the app offers while the selection is its own.
  struct wl_data_source *source;
  char *text, *html;
  civue_bytes png;
} D;

static const char *TEXT_MIMES[] = {"text/plain;charset=utf-8", "text/plain",
                                   "UTF8_STRING", "STRING", "TEXT"};

// ---- offers ----

static void offer_mime(void *data, struct wl_data_offer *o, const char *mime) {
  Offer *offer = data;
  vec_push(&offer->mimes, xstrdup(mime));
}

static void offer_actions(void *data, struct wl_data_offer *o, u32 actions) {
}

static void offer_action(void *data, struct wl_data_offer *o, u32 action) {
}

static const struct wl_data_offer_listener offer_listener = {
  .offer = offer_mime, .source_actions = offer_actions, .action = offer_action};

static void offer_free(Offer *o) {
  if (!o) {
    return;
  }
  wl_data_offer_destroy(o->offer);
  vec_each(&o->mimes, m) {
    free(*m);
  }
  vec_free(&o->mimes);
  free(o);
}

static bool offers(const Offer *o, const char *mime) {
  if (o) {
    vec_each(&o->mimes, m) {
      if (!strcmp(*m, mime)) {
        return true;
      }
    }
  }
  return false;
}

// the bytes another app sends for `mime`, false when it sends none in time.
static bool receive(Offer *o, const char *mime, civue_bytes *out) {
  int fds[2];
  if (pipe2(fds, O_CLOEXEC) < 0) {
    return false;
  }
  wl_data_offer_receive(o->offer, mime, fds[1]);
  close(fds[1]);
  wl_display_flush(WL.display);
  usize before = out->len;
  for (;;) {
    struct pollfd p = {fds[0], POLLIN, 0};
    // an app that sends nothing for a second is given up on.
    if (poll(&p, 1, 1000) <= 0) {
      break;
    }
    u8 chunk[16384];
    ssize_t n = read(fds[0], chunk, sizeof chunk);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    buf_put(out, chunk, (usize)n);
  }
  close(fds[0]);
  return out->len > before;
}

// ---- the data device ----

static void device_offer(void *data, struct wl_data_device *d,
                         struct wl_data_offer *o) {
  Offer *offer = xcalloc(1, sizeof *offer);
  offer->offer = o;
  wl_data_offer_add_listener(o, &offer_listener, offer);
}

static void device_selection(void *data, struct wl_data_device *d,
                             struct wl_data_offer *o) {
  offer_free(D.selection);
  D.selection = o ? wl_data_offer_get_user_data(o) : nullptr;
}

static void device_enter(void *data, struct wl_data_device *d, u32 serial,
                         struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y,
                         struct wl_data_offer *o) {
  offer_free(D.drag);
  D.drag = o ? wl_data_offer_get_user_data(o) : nullptr;
  if (!D.drag) {
    return;
  }
  // files are taken, anything else is refused.
  bool files = offers(D.drag, "text/uri-list");
  wl_data_offer_accept(o, serial, files ? "text/uri-list" : nullptr);
  if (wl_data_offer_get_version(o) >= 3) {
    wl_data_offer_set_actions(
      o, files ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0,
      files ? WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY : 0);
  }
  D.drag_at = v2((f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
  civue__drag_moved(D.drag_at);
}

static void device_leave(void *data, struct wl_data_device *d) {
  offer_free(D.drag);
  D.drag = nullptr;
}

static void device_motion(void *data, struct wl_data_device *d, u32 time,
                          wl_fixed_t x, wl_fixed_t y) {
  D.drag_at = v2((f32)wl_fixed_to_double(x), (f32)wl_fixed_to_double(y));
  civue__drag_moved(D.drag_at);
}

static void device_drop(void *data, struct wl_data_device *d) {
  Offer *o = D.drag;
  D.drag = nullptr;
  if (!o) {
    return;
  }
  civue_bytes list = {};
  civue_strs paths = {};
  if (offers(o, "text/uri-list") && receive(o, "text/uri-list", &list)) {
    usize i = 0;
    while (i < list.len) {
      const char *line = (const char *)list.v + i;
      usize n = 0;
      while (i + n < list.len && line[n] != '\n' && line[n] != '\r') {
        n++;
      }
      i += n + 1;
      if (!n || line[0] == '#') {
        continue;
      }
      char *path = civue__file_path(line, n);
      if (path) {
        vec_push(&paths, path);
      }
    }
  }
  vec_free(&list);
  if (wl_data_offer_get_version(o->offer) >= 3) {
    wl_data_offer_finish(o->offer);
  }
  offer_free(o);
  if (paths.len) {
    civue__dropped(D.drag_at, &paths);
  }
  vec_each(&paths, p) {
    free(*p);
  }
  vec_free(&paths);
}

static const struct wl_data_device_listener device_listener = {
  .data_offer = device_offer,
  .enter = device_enter,
  .leave = device_leave,
  .motion = device_motion,
  .drop = device_drop,
  .selection = device_selection};

void civue__clipboard_seat(void) {
  if (D.device || !WL.data_manager || !WL.seat) {
    return;
  }
  D.device = wl_data_device_manager_get_data_device(WL.data_manager, WL.seat);
  wl_data_device_add_listener(D.device, &device_listener, nullptr);
}

// ---- reading ----

// the app's own copy of `mime`, while the selection is its own.
static bool own(const char *mime, civue_bytes *out) {
  if (!D.source) {
    return false;
  }
  if (!strcmp(mime, "text/html") && D.html) {
    buf_puts(out, D.html);
    return true;
  }
  if (!strcmp(mime, "image/png") && D.png.len) {
    buf_put(out, D.png.v, D.png.len);
    return true;
  }
  if (D.text && strncmp(mime, "image/", 6) && strcmp(mime, "text/html") &&
      strcmp(mime, "text/uri-list")) {
    buf_puts(out, D.text);
    return true;
  }
  return false;
}

typedef struct {
  const char *mime;
  civue_bytes *out;
  bool any;
} Read;

static void read_on_main(void *arg) {
  Read *r = arg;
  if (D.source) {
    r->any = own(r->mime, r->out);
  }
  else if (offers(D.selection, r->mime)) {
    r->any = receive(D.selection, r->mime, r->out);
  }
}

bool civue__clip_read(const char *mime, civue_bytes *out) {
  Read r = {mime, out};
  civue_on_main(read_on_main, &r);
  return r.any;
}

static void any_on_main(void *arg) {
  *(bool *)arg = D.source || (D.selection && D.selection->mimes.len);
}

bool civue__clip_any(void) {
  bool any = false;
  civue_on_main(any_on_main, &any);
  return any;
}

// the clipboard's text in the first type it has, as a malloc'd string.
static char *read_string(const char *const *mimes, u32 n) {
  for (u32 i = 0; i < n; i++) {
    civue_bytes b = {};
    if (!civue__clip_read(mimes[i], &b)) {
      continue;
    }
    char *out = xstrndup((char *)b.v, b.len);
    vec_free(&b);
    if (*out) {
      return out;
    }
    free(out);
  }
  return nullptr;
}

char *civue_clipboard_text(void) {
  return read_string(TEXT_MIMES, countof(TEXT_MIMES));
}

char *civue_clipboard_html(void) {
  static const char *mimes[] = {"text/html"};
  return read_string(mimes, 1);
}

// ---- writing ----

static void source_target(void *data, struct wl_data_source *s,
                          const char *mime) {
}

typedef struct {
  int fd;
  civue_bytes bytes;
} Send;

static void *send_bytes(void *arg) {
  Send *s = arg;
  usize at = 0;
  while (at < s->bytes.len) {
    ssize_t n = write(s->fd, s->bytes.v + at, s->bytes.len - at);
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      break;
    }
    at += (usize)n;
  }
  close(s->fd);
  vec_free(&s->bytes);
  free(s);
  return nullptr;
}

// writes what is asked off the main thread, an app reading slowly never
// holds up the window.
static void source_send(void *data, struct wl_data_source *s, const char *mime,
                        i32 fd) {
  civue_bytes b = {};
  if (!own(mime, &b)) {
    close(fd);
    return;
  }
  Send *send = xmalloc(sizeof *send);
  *send = (Send){fd, b};
  pthread_t thread;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&thread, &attr, send_bytes, send)) {
    send_bytes(send);
  }
  pthread_attr_destroy(&attr);
}

static void own_clear(void) {
  free(D.text);
  free(D.html);
  D.text = D.html = nullptr;
  D.png.len = 0;
}

static void source_cancelled(void *data, struct wl_data_source *s) {
  wl_data_source_destroy(s);
  if (s != D.source) {
    return;
  }
  D.source = nullptr;
  own_clear();
}

static void source_dropped(void *data, struct wl_data_source *s) {
}

static void source_finished(void *data, struct wl_data_source *s) {
}

static void source_action(void *data, struct wl_data_source *s, u32 a) {
}

static const struct wl_data_source_listener source_listener = {
  .target = source_target,
  .send = source_send,
  .cancelled = source_cancelled,
  .dnd_drop_performed = source_dropped,
  .dnd_finished = source_finished,
  .action = source_action};

// what the app copies: its text, html or png.
typedef struct {
  const char *const *mimes;
  u32 n;
  char *text;
  const char *html, *html_text;
  const u8 *png;
  usize png_len;
} Copy;

// makes the app's copy the selection under its mime types.
static void publish_on_main(void *arg) {
  Copy *c = arg;
  if (!D.device) {
    free(c->text);
    return;
  }
  if (D.source) {
    wl_data_source_destroy(D.source);
    D.source = nullptr;
  }
  own_clear();
  if (c->png) {
    buf_put(&D.png, c->png, c->png_len);
  }
  else if (c->text) {
    D.text = c->text;
  }
  else {
    D.html = c->html ? xstrdup(c->html) : nullptr;
    D.text = c->html_text ? xstrdup(c->html_text) : nullptr;
  }
  D.source = wl_data_device_manager_create_data_source(WL.data_manager);
  wl_data_source_add_listener(D.source, &source_listener, nullptr);
  for (u32 i = 0; i < c->n; i++) {
    wl_data_source_offer(D.source, c->mimes[i]);
  }
  wl_data_device_set_selection(D.device, D.source, WL.serial);
  wl_display_flush(WL.display);
}

void civue_clipboard_set_text(const char *s, usize n) {
  Copy c = {
    .mimes = TEXT_MIMES, .n = countof(TEXT_MIMES), .text = xstrndup(s, n)};
  civue_on_main(publish_on_main, &c);
}

void civue_clipboard_set_png(const u8 *png, usize n) {
  static const char *mimes[] = {"image/png"};
  Copy c = {.mimes = mimes, .n = 1, .png = png, .png_len = n};
  civue_on_main(publish_on_main, &c);
}

void civue_clipboard_set_html(const char *html, const char *text) {
  static const char *mimes[] = {"text/html",  "text/plain;charset=utf-8",
                                "text/plain", "UTF8_STRING",
                                "STRING",     "TEXT"};
  Copy c = {
    .mimes = mimes, .n = countof(mimes), .html = html, .html_text = text};
  civue_on_main(publish_on_main, &c);
}
