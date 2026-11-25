#include "lazy.h"
#include "wl.h"

#include <civue/gfx.h>
#include <civue/webview.h>

#include <civue_lazy_libraries.h>
#include <math.h>
#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>

// wpe webkit and its platform are taken by the first web view: every one
// of their functions called here.
#define WPE_FUNCTIONS(X)                                          \
  X(jsc_value_is_string)                                          \
  X(jsc_value_to_string)                                          \
  X(webkit_cookie_manager_set_persistent_storage)                 \
  X(webkit_navigation_action_get_navigation_type)                 \
  X(webkit_navigation_action_get_request)                         \
  X(webkit_navigation_policy_decision_get_navigation_action)      \
  X(webkit_network_error_quark)                                   \
  X(webkit_network_session_get_cookie_manager)                    \
  X(webkit_network_session_new)                                   \
  X(webkit_policy_decision_ignore)                                \
  X(webkit_policy_error_quark)                                    \
  X(webkit_settings_new)                                          \
  X(webkit_settings_set_user_agent)                               \
  X(webkit_uri_request_get_uri)                                   \
  X(webkit_user_content_manager_add_script)                       \
  X(webkit_user_content_manager_new)                              \
  X(webkit_user_content_manager_register_script_message_handler)  \
  X(webkit_user_script_new)                                       \
  X(webkit_user_script_unref)                                     \
  X(webkit_web_view_evaluate_javascript)                          \
  X(webkit_web_view_get_type)                                     \
  X(webkit_web_view_get_uri)                                      \
  X(webkit_web_view_get_user_content_manager)                     \
  X(webkit_web_view_get_wpe_view)                                 \
  X(webkit_web_view_load_html)                                    \
  X(webkit_web_view_load_uri)                                     \
  X(webkit_web_view_set_background_color)                         \
  X(webkit_web_view_stop_loading)                                 \
  X(wpe_buffer_get_height)                                        \
  X(wpe_buffer_get_width)                                         \
  X(wpe_buffer_import_to_pixels)                                  \
  X(wpe_buffer_shm_get_stride)                                    \
  X(wpe_buffer_shm_get_type)                                      \
  X(wpe_display_connect)                                          \
  X(wpe_display_get_type)                                         \
  X(wpe_event_keyboard_new)                                       \
  X(wpe_event_pointer_button_new)                                 \
  X(wpe_event_pointer_move_new)                                   \
  X(wpe_event_scroll_new)                                         \
  X(wpe_event_unref)                                              \
  X(wpe_keymap_get_entries_for_keyval)                            \
  X(wpe_keymap_xkb_new)                                           \
  X(wpe_toplevel_get_type)                                        \
  X(wpe_toplevel_resized)                                         \
  X(wpe_toplevel_scale_changed)                                   \
  X(wpe_toplevel_state_changed)                                   \
  X(wpe_unicode_to_keyval)                                        \
  X(wpe_view_buffer_released)                                     \
  X(wpe_view_buffer_rendered)                                     \
  X(wpe_view_compute_press_count)                                 \
  X(wpe_view_event)                                               \
  X(wpe_view_focus_in)                                            \
  X(wpe_view_focus_out)                                           \
  X(wpe_view_get_toplevel)                                        \
  X(wpe_view_get_type)                                            \
  X(wpe_view_map)                                                 \
  X(wpe_view_resized)                                             \
  X(wpe_view_set_toplevel)                                        \
  X(wpe_view_set_visible)

WPE_FUNCTIONS(CIVUE_LAZY_FUNCTION)

// whether wpe webkit is there, asked once.
static bool wpe_loaded() {
  static const char *const libraries[] = {CIVUE_WPE_LIBRARIES, nullptr};
  static const civue__lazy_symbol symbols[] = {
    WPE_FUNCTIONS(CIVUE_LAZY_SYMBOL)};
  static bool asked, loaded;

  if (!asked) {
    asked = true;
    loaded = civue__lazy_load(libraries, symbols, countof(symbols));
  }

  return loaded;
}

// how often the page is told its picture was shown, in milliseconds: it
// paints the next one no sooner.
constexpr u32 PACE = 16;
// a wheel's notch scrolls this many points in civue, one line in a page.
constexpr f32 NOTCH = 40;

struct civue_webview {
  WebKitWebView *view;
  WPEView *wpe;
  WPEToplevel *top;
  void *ctx;
  void (*message)(void *ctx, const char *text);
  void (*loaded)(void *ctx, const char *error);
  f32 radius;
  // the frame that last showed it.
  u64 shown;
  bool visible, focused, inside;
  bool down[3];
  // its size in points and the scale it paints at.
  i32 width, height;
  f32 scale;
  // the page's last picture, premultiplied rgba, until a frame uploads it.
  u8 *next;
  u32 next_w, next_h;
  civue_gfx *gfx;
  civue_texture texture;
  civue_cursor_kind cursor;
  // the picture the page waits to hear was shown, and the one it heard of
  // last, its memory the ui's until the next.
  WPEBuffer *painted, *held;
  guint paced;
  struct civue_webview *after;
};

static civue_webview *all;

/* ========================================================================== */
/* a display of civue's own for wpe: no screen and no gpu behind it, so the   */
/* page paints into memory, and a view that hands those pictures to the ui.   */
/* ========================================================================== */

#define CIVUE_TYPE_WPE_DISPLAY (civue_wpe_display_get_type())
G_DECLARE_FINAL_TYPE(CivueWpeDisplay, civue_wpe_display, CIVUE, WPE_DISPLAY,
                     WPEDisplay)

struct _CivueWpeDisplay {
  WPEDisplay parent;
};

G_DEFINE_FINAL_TYPE(CivueWpeDisplay, civue_wpe_display, WPE_TYPE_DISPLAY)

#define CIVUE_TYPE_WPE_VIEW (civue_wpe_view_get_type())
G_DECLARE_FINAL_TYPE(CivueWpeView, civue_wpe_view, CIVUE, WPE_VIEW, WPEView)

struct _CivueWpeView {
  WPEView parent;
  civue_webview *web;
};

G_DEFINE_FINAL_TYPE(CivueWpeView, civue_wpe_view, WPE_TYPE_VIEW)

#define CIVUE_TYPE_WPE_TOPLEVEL (civue_wpe_toplevel_get_type())
G_DECLARE_FINAL_TYPE(CivueWpeToplevel, civue_wpe_toplevel, CIVUE, WPE_TOPLEVEL,
                     WPEToplevel)

struct _CivueWpeToplevel {
  WPEToplevel parent;
};

G_DEFINE_FINAL_TYPE(CivueWpeToplevel, civue_wpe_toplevel, WPE_TYPE_TOPLEVEL)

static gboolean display_connect(WPEDisplay *, GError **) {
  return true;
}

static WPEView *display_view(WPEDisplay *display) {
  return g_object_new(CIVUE_TYPE_WPE_VIEW, "display", display, nullptr);
}

static WPEToplevel *display_toplevel(WPEDisplay *display, guint max_views) {
  return g_object_new(CIVUE_TYPE_WPE_TOPLEVEL, "display", display, "max-views",
                      max_views, nullptr);
}

// the keys' codes of a us layout, which webkit reads its editing keys by.
static WPEKeymap *display_keymap(WPEDisplay *) {
  static WPEKeymap *keymap;

  if (!keymap) {
    keymap = wpe_keymap_xkb_new();
  }

  return keymap;
}

static void civue_wpe_display_class_init(CivueWpeDisplayClass *c) {
  WPEDisplayClass *display = WPE_DISPLAY_CLASS(c);
  display->get_keymap = display_keymap;
  display->connect = display_connect;
  display->create_view = display_view;
  display->create_toplevel = display_toplevel;
}

static void civue_wpe_display_init(CivueWpeDisplay *) {
}

static gboolean toplevel_resize(WPEToplevel *top, int width, int height) {
  wpe_toplevel_resized(top, width, height);
  return true;
}

static void civue_wpe_toplevel_class_init(CivueWpeToplevelClass *c) {
  WPE_TOPLEVEL_CLASS(c)->resize = toplevel_resize;
}

static void civue_wpe_toplevel_init(CivueWpeToplevel *) {
}

// tells the page its picture was shown, and that the memory of the one
// before is free again.
static gboolean picture_shown(void *data) {
  civue_webview *w = data;

  w->paced = 0;

  if (!w->painted) {
    return G_SOURCE_REMOVE;
  }

  if (w->held) {
    wpe_view_buffer_released(w->wpe, w->held);
    g_object_unref(w->held);
  }

  w->held = w->painted;
  w->painted = nullptr;
  wpe_view_buffer_rendered(w->wpe, w->held);
  return G_SOURCE_REMOVE;
}

// the page painted: its pixels are copied for the frame the window is asked
// for, blue first as wpe has them to red first as images are.
static gboolean view_render(WPEView *view, WPEBuffer *buffer,
                            const WPERectangle *, guint, GError **error) {
  civue_webview *w = CIVUE_WPE_VIEW(view)->web;

  if (!w) {
    return true;
  }

  // the buffer's own bytes, not ours to unref.
  GBytes *bytes = wpe_buffer_import_to_pixels(buffer, error);

  if (!bytes) {
    return false;
  }

  gsize size = 0;
  const u8 *from = g_bytes_get_data(bytes, &size);
  u32 pw = (u32)MAX(wpe_buffer_get_width(buffer), 0);
  u32 ph = (u32)MAX(wpe_buffer_get_height(buffer), 0);
  usize stride = WPE_IS_BUFFER_SHM(buffer)
                   ? wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer))
                 : ph ? size / ph
                      : 0;

  if (pw && ph && stride >= (usize)pw * 4 &&
      size >= stride * (ph - 1) + pw * 4) {
    free(w->next);
    w->next = xmalloc((usize)pw * ph * 4);
    w->next_w = pw;
    w->next_h = ph;

    for (u32 y = 0; y < ph; y++) {
      const u8 *src = from + y * stride;
      u8 *dst = w->next + (usize)y * pw * 4;

      for (u32 x = 0; x < pw; x++) {
        dst[x * 4] = src[x * 4 + 2];
        dst[x * 4 + 1] = src[x * 4 + 1];
        dst[x * 4 + 2] = src[x * 4];
        dst[x * 4 + 3] = src[x * 4 + 3];
      }
    }
  }

  g_set_object(&w->painted, buffer);

  if (!w->paced) {
    w->paced = g_timeout_add(PACE, picture_shown, w);
  }

  civue_window_wake();
  return true;
}

static civue_cursor_kind cursor_of(const char *name) {
  static const struct {
    const char *name;
    civue_cursor_kind kind;
  } kinds[] = {
    {"pointer", CIVUE_CURSOR_POINTER},    {"text", CIVUE_CURSOR_TEXT},
    {"vertical-text", CIVUE_CURSOR_TEXT}, {"grab", CIVUE_CURSOR_HAND},
    {"grabbing", CIVUE_CURSOR_GRABBING},  {"crosshair", CIVUE_CURSOR_CROSSHAIR},
    {"copy", CIVUE_CURSOR_COPY},          {"zoom-in", CIVUE_CURSOR_ZOOM_IN},
    {"zoom-out", CIVUE_CURSOR_ZOOM_OUT},  {"col-resize", CIVUE_CURSOR_RESIZE_H},
    {"ew-resize", CIVUE_CURSOR_RESIZE_H}, {"e-resize", CIVUE_CURSOR_RESIZE_H},
    {"w-resize", CIVUE_CURSOR_RESIZE_H},  {"row-resize", CIVUE_CURSOR_RESIZE_V},
    {"ns-resize", CIVUE_CURSOR_RESIZE_V}, {"n-resize", CIVUE_CURSOR_RESIZE_V},
    {"s-resize", CIVUE_CURSOR_RESIZE_V},
  };

  for (u32 i = 0; name && i < countof(kinds); i++) {
    if (!strcmp(name, kinds[i].name)) {
      return kinds[i].kind;
    }
  }

  return CIVUE_CURSOR_DEFAULT;
}

static void view_cursor(WPEView *view, const char *name) {
  civue_webview *w = CIVUE_WPE_VIEW(view)->web;

  if (w && w->cursor != cursor_of(name)) {
    w->cursor = cursor_of(name);
    civue_window_wake();
  }
}

static gboolean view_mappable(WPEView *) {
  return true;
}

static void civue_wpe_view_class_init(CivueWpeViewClass *c) {
  WPEViewClass *view = WPE_VIEW_CLASS(c);
  view->render_buffer = view_render;
  view->set_cursor_from_name = view_cursor;
  view->can_be_mapped = view_mappable;
}

static void civue_wpe_view_init(CivueWpeView *) {
}

/* ========================================================================== */
/* the page.                                                                  */
/* ========================================================================== */

static void posted(WebKitUserContentManager *, JSCValue *value, void *data) {
  civue_webview *w = data;

  if (!w->message || !jsc_value_is_string(value)) {
    return;
  }

  char *text = jsc_value_to_string(value);
  w->message(w->ctx, text ? text : "");
  g_free(text);
}

static void load_changed(WebKitWebView *, WebKitLoadEvent event, void *data) {
  civue_webview *w = data;

  if (event == WEBKIT_LOAD_FINISHED && w->loaded) {
    w->loaded(w->ctx, nullptr);
  }
}

static gboolean load_failed(WebKitWebView *, WebKitLoadEvent, const char *,
                            GError *error, void *data) {
  civue_webview *w = data;
  // a load another one replaced, or a link sent to the browser, failed at
  // nothing.
  bool replaced = g_error_matches(error, WEBKIT_NETWORK_ERROR,
                                  WEBKIT_NETWORK_ERROR_CANCELLED);
  bool sent_away = g_error_matches(
    error, WEBKIT_POLICY_ERROR,
    WEBKIT_POLICY_ERROR_FRAME_LOAD_INTERRUPTED_BY_POLICY_CHANGE);

  if (w->loaded && !replaced && !sent_away) {
    w->loaded(w->ctx, error && error->message ? error->message : "failed");
  }

  // the finished that follows says nothing more.
  return true;
}

static void process_ended(WebKitWebView *, WebKitWebProcessTerminationReason,
                          void *data) {
  civue_webview *w = data;

  if (w->loaded) {
    w->loaded(w->ctx, "the page's process ended");
  }
}

static bool same_origin(const char *a, const char *b) {
  GUri *ua = a ? g_uri_parse(a, G_URI_FLAGS_NONE, nullptr) : nullptr;
  GUri *ub = b ? g_uri_parse(b, G_URI_FLAGS_NONE, nullptr) : nullptr;
  bool same =
    ua && ub &&
    !g_ascii_strcasecmp(g_uri_get_scheme(ua), g_uri_get_scheme(ub)) &&
    !g_ascii_strcasecmp(g_uri_get_host(ua) ? g_uri_get_host(ua) : "",
                        g_uri_get_host(ub) ? g_uri_get_host(ub) : "") &&
    g_uri_get_port(ua) == g_uri_get_port(ub);

  if (ua) {
    g_uri_unref(ua);
  }

  if (ub) {
    g_uri_unref(ub);
  }

  return same;
}

// a link to a new window, or one the user clicks to another site, opens in
// the system's browser. the page's own scripts still move it where they
// want.
static gboolean decide(WebKitWebView *view, WebKitPolicyDecision *decision,
                       WebKitPolicyDecisionType type, void *) {
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
    return false;
  }

  WebKitNavigationAction *action =
    webkit_navigation_policy_decision_get_navigation_action(
      WEBKIT_NAVIGATION_POLICY_DECISION(decision));
  const char *url =
    webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
  bool new_window = type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION;
  bool leaves = webkit_navigation_action_get_navigation_type(action) ==
                  WEBKIT_NAVIGATION_TYPE_LINK_CLICKED &&
                !same_origin(url, webkit_web_view_get_uri(view));

  if (!new_window && !leaves) {
    return false;
  }

  // web pages only: a page does not get to open files or other apps.
  if (url && (!g_ascii_strncasecmp(url, "https://", 8) ||
              !g_ascii_strncasecmp(url, "http://", 7))) {
    civue_open_url(url);
  }

  webkit_policy_decision_ignore(decision);
  return true;
}

// where cookies and storage are kept between runs, as a browser keeps them:
// one session for every webview of the app.
static WebKitNetworkSession *session() {
  static WebKitNetworkSession *s;

  if (s) {
    return s;
  }

  char *data = g_build_filename(g_get_user_data_dir(), civue__app_id(),
                                "webview", nullptr);
  char *cache = g_build_filename(g_get_user_cache_dir(), civue__app_id(),
                                 "webview", nullptr);
  char *cookies = g_build_filename(data, "cookies.sqlite", nullptr);

  s = webkit_network_session_new(data, cache);
  webkit_cookie_manager_set_persistent_storage(
    webkit_network_session_get_cookie_manager(s), cookies,
    WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  g_free(data);
  g_free(cache);
  g_free(cookies);
  return s;
}

static WPEDisplay *display() {
  static WPEDisplay *d;

  if (!d) {
    d = g_object_new(CIVUE_TYPE_WPE_DISPLAY, nullptr);
    wpe_display_connect(d, nullptr);
  }

  return d;
}

static void hide(civue_webview *w) {
  if (w->focused) {
    w->focused = false;
    wpe_view_focus_out(w->wpe);
  }

  if (w->visible) {
    w->visible = false;
    w->inside = false;
    memset(w->down, 0, sizeof w->down);
    wpe_view_set_visible(w->wpe, false);
  }
}

// hides the webviews the frame numbered `frame` did not show: their pages
// stop painting.
static void place(u64 frame) {
  for (civue_webview *w = all; w; w = w->after) {
    if (w->shown != frame) {
      hide(w);
    }
  }
}

civue_webview *civue_webview_new(const civue_webview_options *o) {
  // a system that lost wpe webkit since the build has no web views.
  if (!wpe_loaded()) {
    return nullptr;
  }

  civue_webview *w = own_new(sizeof(civue_webview));
  w->ctx = o->ctx;
  w->message = o->message;
  w->loaded = o->loaded;
  w->radius = o->radius;
  w->scale = 1;

  // glib's loop, which the page's processes talk through, runs in civue's.
  civue__glib_drive();
  civue__webviews_place = place;

  WebKitUserContentManager *content = webkit_user_content_manager_new();
  WebKitUserScript *bridge = webkit_user_script_new(
    "window.civue = { postMessage: (s) => "
    "window.webkit.messageHandlers.civue.postMessage(String(s)) };",
    WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(content, bridge);
  webkit_user_script_unref(bridge);
  webkit_user_content_manager_register_script_message_handler(content, "civue",
                                                              nullptr);
  g_signal_connect(content, "script-message-received::civue",
                   G_CALLBACK(posted), w);

  WebKitSettings *settings = webkit_settings_new();

  if (o->user_agent) {
    webkit_settings_set_user_agent(settings, o->user_agent);
  }

  w->view = g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display(),
                         "network-session", session(), "user-content-manager",
                         content, "settings", settings, nullptr);
  g_object_unref(content);
  g_object_unref(settings);

  if (o->transparent) {
    WebKitColor clear = {};
    webkit_web_view_set_background_color(w->view, &clear);
  }

  g_signal_connect(w->view, "load-changed", G_CALLBACK(load_changed), w);
  g_signal_connect(w->view, "load-failed", G_CALLBACK(load_failed), w);
  g_signal_connect(w->view, "decide-policy", G_CALLBACK(decide), w);
  g_signal_connect(w->view, "web-process-terminated", G_CALLBACK(process_ended),
                   w);

  w->wpe = webkit_web_view_get_wpe_view(w->view);
  CIVUE_WPE_VIEW(w->wpe)->web = w;
  w->top = wpe_view_get_toplevel(w->wpe);

  if (!w->top) {
    w->top =
      g_object_new(CIVUE_TYPE_WPE_TOPLEVEL, "display", display(), nullptr);
    wpe_view_set_toplevel(w->wpe, w->top);
    g_object_unref(w->top);
  }

  wpe_view_set_visible(w->wpe, false);
  wpe_view_map(w->wpe);
  w->after = all;
  all = w;
  return w;
}

void civue_webview_free(civue_webview *w) {
  if (!w) {
    return;
  }

  for (civue_webview **at = &all; *at; at = &(*at)->after) {
    if (*at == w) {
      *at = w->after;
      break;
    }
  }

  if (w->paced) {
    g_source_remove(w->paced);
  }

  g_clear_object(&w->painted);
  g_clear_object(&w->held);
  CIVUE_WPE_VIEW(w->wpe)->web = nullptr;
  g_signal_handlers_disconnect_by_data(w->view, w);
  g_signal_handlers_disconnect_by_data(
    webkit_web_view_get_user_content_manager(w->view), w);
  webkit_web_view_stop_loading(w->view);
  g_object_unref(w->view);

  if (w->texture != 0 && w->gfx) {
    civue_gfx_free_texture(w->gfx, w->texture);
  }

  free(w->next);
  own_free(w);
}

void civue_webview_load_html(civue_webview *w, const char *html,
                             const char *base_url) {
  if (w) {
    webkit_web_view_load_html(w->view, html, base_url);
  }
}

void civue_webview_load_url(civue_webview *w, const char *url) {
  if (w && url) {
    webkit_web_view_load_uri(w->view, url);
  }
}

void civue_webview_eval(civue_webview *w, const char *js) {
  if (w && js) {
    webkit_web_view_evaluate_javascript(w->view, js, -1, nullptr, nullptr,
                                        nullptr, nullptr, nullptr);
  }
}

/* ========================================================================== */
/* showing it, and giving it the pointer and the keys.                        */
/* ========================================================================== */

static WPEModifiers modifiers(const civue_webview *w, u8 mods) {
  WPEModifiers m = 0;

  if (mods & CIVUE_MOD_CTRL) {
    m |= WPE_MODIFIER_KEYBOARD_CONTROL;
  }

  if (mods & CIVUE_MOD_SHIFT) {
    m |= WPE_MODIFIER_KEYBOARD_SHIFT;
  }

  if (mods & CIVUE_MOD_ALT) {
    m |= WPE_MODIFIER_KEYBOARD_ALT;
  }

  if (mods & CIVUE_MOD_CMD) {
    m |= WPE_MODIFIER_KEYBOARD_META;
  }

  if (w->down[0]) {
    m |= WPE_MODIFIER_POINTER_BUTTON1;
  }

  if (w->down[2]) {
    m |= WPE_MODIFIER_POINTER_BUTTON2;
  }

  if (w->down[1]) {
    m |= WPE_MODIFIER_POINTER_BUTTON3;
  }

  return m;
}

static void send(civue_webview *w, WPEEvent *event) {
  wpe_view_event(w->wpe, event);
  wpe_event_unref(event);
}

// the key of a page for one of civue's own, 0 for the others.
static guint keyval_of(u32 key) {
  switch (key) {
    case CIVUE_KEY_ENTER:
      return WPE_KEY_Return;
    case CIVUE_KEY_ESCAPE:
      return WPE_KEY_Escape;
    case CIVUE_KEY_TAB:
      return WPE_KEY_Tab;
    case CIVUE_KEY_BACKSPACE:
      return WPE_KEY_BackSpace;
    case CIVUE_KEY_DELETE:
      return WPE_KEY_Delete;
    case CIVUE_KEY_LEFT:
      return WPE_KEY_Left;
    case CIVUE_KEY_RIGHT:
      return WPE_KEY_Right;
    case CIVUE_KEY_UP:
      return WPE_KEY_Up;
    case CIVUE_KEY_DOWN:
      return WPE_KEY_Down;
    case CIVUE_KEY_HOME:
      return WPE_KEY_Home;
    case CIVUE_KEY_END:
      return WPE_KEY_End;
    case CIVUE_KEY_PAGE_UP:
      return WPE_KEY_Page_Up;
    case CIVUE_KEY_PAGE_DOWN:
      return WPE_KEY_Page_Down;
    case CIVUE_KEY_SPACE:
      return WPE_KEY_space;
    default:
      break;
  }

  if (key >= CIVUE_KEY_F1 && key <= CIVUE_KEY_F12) {
    return WPE_KEY_F1 + (key - CIVUE_KEY_F1);
  }

  return 0;
}

static void type(civue_webview *w, guint keyval, WPEModifiers m, u32 time) {
  if (!keyval) {
    return;
  }

  // a key without its code is a character to webkit, never an edit.
  guint code = 0;
  WPEKeymapEntry *entries = nullptr;
  guint n = 0;

  if (wpe_keymap_get_entries_for_keyval(display_keymap(nullptr), keyval,
                                        &entries, &n) &&
      n) {
    code = entries[0].keycode;
  }

  g_free(entries);
  send(w, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_DOWN, w->wpe,
                                 WPE_INPUT_SOURCE_KEYBOARD, time, m, code,
                                 keyval));
  send(w, wpe_event_keyboard_new(WPE_EVENT_KEYBOARD_KEY_UP, w->wpe,
                                 WPE_INPUT_SOURCE_KEYBOARD, time, m, code,
                                 keyval));
}

// the keys the frame got, while the page has the focus: the ones that type
// nothing and the shortcuts as keys, the text a character at a time.
static void give_keys(civue_ui *ui, civue_webview *w, u32 time) {
  const civue_input *in = &ui->input;

  vec_each(&in->keys, k) {
    WPEModifiers m = modifiers(w, k->mods);
    guint keyval = keyval_of(k->key);

    if (keyval == WPE_KEY_space &&
        !(k->mods & (CIVUE_MOD_CTRL | CIVUE_MOD_CMD))) {
      continue;
    }

    if (!keyval && k->key < CIVUE_KEY_ENTER &&
        k->mods & (CIVUE_MOD_CTRL | CIVUE_MOD_CMD)) {
      keyval = wpe_unicode_to_keyval(k->key);
    }

    type(w, keyval, m, time);
  }

  const char *text = (const char *)in->text.v;
  usize n = in->text.len;

  for (usize i = 0; i < n;) {
    usize before = i;
    u32 c = utf8_next(text, n, &i);

    if (i == before) {
      break;
    }

    type(w, wpe_unicode_to_keyval(c), modifiers(w, in->mods & CIVUE_MOD_SHIFT),
         time);
  }
}

void civue_webview_show(civue_ui *ui, civue_webview *w, civue_rect r) {
  if (!w) {
    return;
  }

  // on whole pixels, like what the ui paints around it.
  f32 s = ui->scale > 0 ? ui->scale : 1;
  r = rect(roundf(r.x0 * s) / s, roundf(r.y0 * s) / s, roundf(r.x1 * s) / s,
           roundf(r.y1 * s) / s);

  i32 width = (i32)lroundf(rect_w(r)), height = (i32)lroundf(rect_h(r));

  if (width < 1 || height < 1) {
    return;
  }

  w->shown = ui->frames;

  if (ui->gfx) {
    w->gfx = ui->gfx;
  }

  if (s != w->scale) {
    w->scale = s;
    wpe_toplevel_scale_changed(w->top, s);
  }

  if (width != w->width || height != w->height) {
    w->width = width;
    w->height = height;
    wpe_toplevel_resized(w->top, width, height);
    wpe_view_resized(w->wpe, width, height);
  }

  if (!w->visible) {
    w->visible = true;
    wpe_toplevel_state_changed(w->top, WPE_TOPLEVEL_STATE_ACTIVE);
    wpe_view_set_visible(w->wpe, true);
  }

  if (w->next && w->gfx) {
    if (w->texture != 0) {
      civue_gfx_free_texture(w->gfx, w->texture);
    }

    w->texture = civue_gfx_upload_image(w->gfx, w->next_w, w->next_h, w->next);
    free(w->next);
    w->next = nullptr;
  }

  if (w->texture != 0) {
    civue_image(ui, w->texture, r, (f32[4]){0, 0, 1, 1}, 1, w->radius, false);
  }

  civue_id id = civue_idn(civue_id_of("civue_webview"), (uintptr_t)w);
  civue_response resp = civue_interact(ui, id, r, CIVUE_SENSE_DRAG);
  const civue_input *in = &ui->input;
  u32 time = (u32)(civue_now() * 1000);
  f64 x = in->mouse.x - r.x0, y = in->mouse.y - r.y0;
  bool held = w->down[0] || w->down[1] || w->down[2];
  bool inside = in->has_mouse && (resp.hovered || held);

  if (inside != w->inside) {
    w->inside = inside;
    send(w, wpe_event_pointer_move_new(inside ? WPE_EVENT_POINTER_ENTER
                                              : WPE_EVENT_POINTER_LEAVE,
                                       w->wpe, WPE_INPUT_SOURCE_MOUSE, time,
                                       modifiers(w, in->mods), x, y, 0, 0));
  }

  if (inside && civue_pointer_moved(ui)) {
    send(w, wpe_event_pointer_move_new(WPE_EVENT_POINTER_MOVE, w->wpe,
                                       WPE_INPUT_SOURCE_MOUSE, time,
                                       modifiers(w, in->mods), x, y, 0, 0));
  }

  // civue's buttons are the left, the right and the middle one.
  static constexpr guint buttons[3] = {WPE_BUTTON_PRIMARY, WPE_BUTTON_SECONDARY,
                                       WPE_BUTTON_MIDDLE};

  for (u32 b = 0; b < 3; b++) {
    if (in->pressed[b] && resp.hovered && !w->down[b]) {
      guint count =
        wpe_view_compute_press_count(w->wpe, x, y, buttons[b], time);

      send(w, wpe_event_pointer_button_new(
                WPE_EVENT_POINTER_DOWN, w->wpe, WPE_INPUT_SOURCE_MOUSE, time,
                modifiers(w, in->mods), buttons[b], x, y, count));
      w->down[b] = true;
      ui->focus = id;
    }

    if (in->released[b] && w->down[b]) {
      w->down[b] = false;
      send(w, wpe_event_pointer_button_new(
                WPE_EVENT_POINTER_UP, w->wpe, WPE_INPUT_SOURCE_MOUSE, time,
                modifiers(w, in->mods), buttons[b], x, y, 0));
    }
  }

  if (resp.hovered && (in->scroll.x != 0 || in->scroll.y != 0)) {
    f32 unit = in->precise_scroll ? 1 : NOTCH;

    send(w, wpe_event_scroll_new(w->wpe, WPE_INPUT_SOURCE_MOUSE, time,
                                 modifiers(w, in->mods), in->scroll.x / unit,
                                 in->scroll.y / unit, in->precise_scroll, false,
                                 x, y));
  }

  bool focused = ui->focus == id;

  if (focused != w->focused) {
    w->focused = focused;

    if (focused) {
      wpe_view_focus_in(w->wpe);
    }
    else {
      wpe_view_focus_out(w->wpe);
    }
  }

  if (focused) {
    civue_claim_focus(ui);
    give_keys(ui, w, time);
  }

  if (resp.hovered) {
    ui->cursor = (civue_cursor){.kind = w->cursor};
  }
}
