#include "../internal.h"

#include <civue/system.h>
#include <civue/window.h>

#include <gio/gio.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

static constexpr char PORTAL[] = "org.freedesktop.portal.Desktop";

typedef struct {
  const char *const *extensions;
  u32 n;
  bool multiple;
  civue_strs paths;
  _Atomic bool done;
} Pick;

typedef struct {
  Pick *pick;
  bool answered;
} Wait;

// the dialog's answer: the chosen files' uris when the response is 0.
static void responded(GDBusConnection *conn, const char *sender,
                      const char *path, const char *iface, const char *name,
                      GVariant *params, void *data) {
  Wait *w = data;
  guint32 code = 1;
  GVariant *results = nullptr;
  g_variant_get(params, "(u@a{sv})", &code, &results);
  char **uris = nullptr;
  if (code == 0 && g_variant_lookup(results, "uris", "^as", &uris)) {
    for (u32 i = 0; uris[i]; i++) {
      char *file = g_filename_from_uri(uris[i], nullptr, nullptr);
      if (!file) {
        continue;
      }
      vec_push(&w->pick->paths, xstrdup(file));
      g_free(file);
    }
    g_strfreev(uris);
  }
  g_variant_unref(results);
  w->answered = true;
}

// the portal went away with the dialog open, which nobody will answer.
static void vanished(GDBusConnection *conn, const char *name, void *data) {
  Wait *w = data;
  w->answered = true;
}

static void ask_portal(GDBusConnection *conn, GMainContext *context,
                       Pick *pick) {
  char *sender = g_strdup(g_dbus_connection_get_unique_name(conn) + 1);
  for (char *c = sender; *c; c++) {
    if (*c == '.') {
      *c = '_';
    }
  }
  char *token = g_strdup_printf("civue_%d", (int)getpid());
  char *request = g_strdup_printf(
    "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
  Wait wait = {.pick = pick};
  // listens before asking, or a quick answer would be missed.
  guint sub = g_dbus_connection_signal_subscribe(
    conn, PORTAL, "org.freedesktop.portal.Request", "Response", request,
    nullptr, G_DBUS_SIGNAL_FLAGS_NONE, responded, &wait, nullptr);
  GVariantBuilder opts;
  g_variant_builder_init(&opts, G_VARIANT_TYPE_VARDICT);
  g_variant_builder_add(&opts, "{sv}", "handle_token",
                        g_variant_new_string(token));
  g_variant_builder_add(&opts, "{sv}", "modal", g_variant_new_boolean(true));
  g_variant_builder_add(&opts, "{sv}", "multiple",
                        g_variant_new_boolean(pick->multiple));
  if (pick->n) {
    GVariantBuilder globs;
    g_variant_builder_init(&globs, G_VARIANT_TYPE("a(us)"));
    for (u32 i = 0; i < pick->n; i++) {
      char *lower = g_ascii_strdown(pick->extensions[i], -1);
      char *upper = g_ascii_strup(pick->extensions[i], -1);
      char *low = g_strdup_printf("*.%s", lower);
      char *up = g_strdup_printf("*.%s", upper);
      g_variant_builder_add(&globs, "(us)", 0u, low);
      if (strcmp(low, up)) {
        g_variant_builder_add(&globs, "(us)", 0u, up);
      }
      g_free(up);
      g_free(low);
      g_free(upper);
      g_free(lower);
    }
    GVariant *filter = g_variant_new("(s@a(us))", "Supported files",
                                     g_variant_builder_end(&globs));
    g_variant_builder_add(&opts, "{sv}", "filters",
                          g_variant_new_array(nullptr, &filter, 1));
  }
  GVariant *reply = g_dbus_connection_call_sync(
    conn, PORTAL, "/org/freedesktop/portal/desktop",
    "org.freedesktop.portal.FileChooser", "OpenFile",
    g_variant_new("(ss@a{sv})", civue_window_handle(), "Open",
                  g_variant_builder_end(&opts)),
    G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, nullptr);
  if (reply) {
    g_variant_unref(reply);
    guint watch = g_bus_watch_name_on_connection(
      conn, PORTAL, G_BUS_NAME_WATCHER_FLAGS_NONE, nullptr, vanished, &wait,
      nullptr);
    while (!wait.answered) {
      g_main_context_iteration(context, true);
    }
    g_bus_unwatch_name(watch);
  }
  g_dbus_connection_signal_unsubscribe(conn, sub);
  g_free(request);
  g_free(token);
  g_free(sender);
}

// asks on a main context of this thread, which the caller's thread does not
// iterate.
static void *ask(void *arg) {
  Pick *pick = arg;
  GMainContext *context = g_main_context_new();
  g_main_context_push_thread_default(context);
  GDBusConnection *conn = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, nullptr);
  if (conn) {
    ask_portal(conn, context, pick);
    g_object_unref(conn);
  }
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
  atomic_store(&pick->done, true);
  return nullptr;
}

static bool waiting(void *arg) {
  return !atomic_load(&((Pick *)arg)->done);
}

static void wait_on_main(void *arg) {
  civue_wait_while(waiting, arg);
}

civue_strs civue_file_panel_open(const char *const *extensions, u32 n,
                                 bool multiple) {
  Pick pick = {.extensions = extensions, .n = n, .multiple = multiple};
  pthread_t thread;
  if (pthread_create(&thread, nullptr, ask, &pick)) {
    return (civue_strs){};
  }
  // like the system's panel, the app waits for it, its window still drawn
  // and handling events.
  civue_on_main(wait_on_main, &pick);
  pthread_join(thread, nullptr);
  return pick.paths;
}
