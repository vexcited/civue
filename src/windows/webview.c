#include "win.h"

#include <civue/webview.h>
#include <civue/window.h>

#include <math.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <stddefer.h>

#include <WebView2.h>

/* ========================================================================== */
/* webview2, the edge runtime windows ships, found when the first web view   */
/* is made: through WebView2Loader.dll when the app ships it next to its exe, */
/* otherwise as that loader finds the installed runtime.                      */
/* ========================================================================== */

typedef ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler Made;

typedef HRESULT(STDMETHODCALLTYPE *Loader)(PCWSTR browser, PCWSTR data,
                                           ICoreWebView2EnvironmentOptions *,
                                           Made *made);

// what the loader calls in the runtime's own dll. microsoft does not
// document it, but every app's loader, linked statically years ago, calls it
// in today's runtimes. 0 is an installed runtime, 1 one the app carries.
typedef HRESULT(STDMETHODCALLTYPE *Internal)(bool check_running, int kind,
                                             PCWSTR data, IUnknown *options,
                                             Made *made);

static Loader loader;
static Internal internal;

#if defined(__aarch64__) || defined(_M_ARM64)
#define RUNTIME_ARCH L"arm64"
#elif defined(__x86_64__) || defined(_M_X64)
#define RUNTIME_ARCH L"x64"
#else
#define RUNTIME_ARCH L"x86"
#endif

// the evergreen runtime of the stable channel, for the machine or for the
// user: edge update keeps its folder, its dll is under it for this cpu.
static Internal installed_runtime() {
  static const HKEY roots[] = {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER};
  static const wchar_t state[] =
    L"SOFTWARE\\Microsoft\\EdgeUpdate\\ClientState\\"
    L"{F3017226-FE2A-4295-8BDF-00C3A9A7E4C5}";

  for (u32 i = 0; i < countof(roots); i++) {
    HKEY key;

    if (RegOpenKeyExW(roots[i], state, 0, KEY_READ | KEY_WOW64_32KEY, &key)) {
      continue;
    }

    wchar_t dir[MAX_PATH];
    DWORD size = sizeof dir;
    LSTATUS read = RegGetValueW(key, nullptr, L"EBWebView", RRF_RT_REG_SZ,
                                nullptr, dir, &size);
    RegCloseKey(key);

    wchar_t path[MAX_PATH + 64];

    if (read ||
        swprintf(
          path, countof(path),
          L"%ls\\EBWebView\\" RUNTIME_ARCH L"\\EmbeddedBrowserWebView.dll",
          dir) < 0) {
      continue;
    }

    HMODULE dll = LoadLibraryExW(path, nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                   LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    Internal make = dll ? (Internal)(void *)GetProcAddress(
                            dll, "CreateWebViewEnvironmentWithOptionsInternal")
                        : nullptr;

    if (make) {
      return make;
    }
  }

  return nullptr;
}

// whether webview2 is there, asked once.
static bool runtime_found() {
  static bool asked;

  if (!asked) {
    asked = true;

    HMODULE dll = LoadLibraryExW(L"WebView2Loader.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_APPLICATION_DIR);

    if (dll) {
      loader = (Loader)(void *)GetProcAddress(
        dll, "CreateCoreWebView2EnvironmentWithOptions");
    }

    if (!loader) {
      internal = installed_runtime();
    }
  }

  return loader || internal;
}

/* ========================================================================== */
/* the objects webview2 calls back, each of one interface. what they are for  */
/* is kept until webview2 lets go of them, and told apart once freed.         */
/* ========================================================================== */

typedef enum : u8 {
  ASK_HTML,
  ASK_URL,
  ASK_EVAL,
} Ask_kind;

// what the app asked of a page that is not there yet.
typedef struct {
  Ask_kind kind;
  // the url, the base url or the script.
  wchar_t *text;
  char *html;
} Ask;

struct civue_webview {
  // the app's reference and one for each handler webview2 holds.
  LONG refs;
  bool freed;
  void *ctx;
  void (*message)(void *ctx, const char *text);
  void (*loaded)(void *ctx, const char *error);
  wchar_t *user_agent;
  bool transparent;
  f32 radius;
  // the child of the ui's window it sits in, rounded by its region.
  HWND host;
  ICoreWebView2Controller *controller;
  ICoreWebView2 *view;
  bool making, broken;
  Vec(Ask) waiting;
  // the page of load_html, answered when the document at `html_url` is
  // asked for.
  char *html;
  wchar_t *html_url;
  // the navigations of our own that have not started.
  u32 ours;

  struct {
    EventRegistrationToken message, starting, completed, window, resource,
      failed, got, lost;
  } on;

  // the page has the keys.
  bool focused;
  // the frame that last showed it, where, and where its window is now in
  // pixels.
  u64 shown;
  civue_rect rect;
  bool visible;
  RECT at;
  i32 corner;
  struct civue_webview *after;
};

static civue_webview *all;

static void unref(civue_webview *w) {
  if (InterlockedDecrement(&w->refs)) {
    return;
  }

  vec_each(&w->waiting, a) {
    free(a->text);
    free(a->html);
  }

  vec_free(&w->waiting);
  free(w->user_agent);
  free(w->html);
  free(w->html_url);
  free(w);
}

typedef struct {
  const void *vtbl;
  const IID *iid;
  LONG refs;
  // none for the environment's.
  civue_webview *w;
} Handler;

static HRESULT STDMETHODCALLTYPE query(IUnknown *self, REFIID iid, void **out) {
  Handler *h = (Handler *)self;

  if (!IsEqualIID(iid, &IID_IUnknown) && !IsEqualIID(iid, h->iid)) {
    *out = nullptr;
    return E_NOINTERFACE;
  }

  InterlockedIncrement(&h->refs);
  *out = h;
  return S_OK;
}

static ULONG STDMETHODCALLTYPE add_ref(IUnknown *self) {
  return (ULONG)InterlockedIncrement(&((Handler *)self)->refs);
}

static ULONG STDMETHODCALLTYPE release(IUnknown *self) {
  Handler *h = (Handler *)self;
  LONG left = InterlockedDecrement(&h->refs);

  if (!left) {
    if (h->w) {
      unref(h->w);
    }

    free(h);
  }

  return (ULONG)left;
}

#define UNKNOWN(T)                                                             \
  .QueryInterface = (HRESULT(STDMETHODCALLTYPE *)(T *, REFIID, void **))query, \
  .AddRef = (ULONG(STDMETHODCALLTYPE *)(T *))add_ref,                          \
  .Release = (ULONG(STDMETHODCALLTYPE *)(T *))release

static Handler *handler_new(const void *vtbl, const IID *iid,
                            civue_webview *w) {
  Handler *h = xmalloc(sizeof *h);
  *h = (Handler){vtbl, iid, 1, w};

  if (w) {
    InterlockedIncrement(&w->refs);
  }

  return h;
}

// `w` of the handler webview2 called, null once the app freed it.
static civue_webview *of(void *self) {
  civue_webview *w = ((Handler *)self)->w;

  return w && !w->freed ? w : nullptr;
}

// adds a handler of `vtbl` to the event of `add`, which keeps it.
#define LISTEN(add, on, vtbl, iid, w, token)       \
  do {                                             \
    Handler *h_ = handler_new(&(vtbl), &(iid), w); \
    add(on, (void *)h_, token);                    \
    release((IUnknown *)h_);                       \
  } while (0)

static void report(civue_webview *w, const char *error) {
  if (w->loaded) {
    w->loaded(w->ctx, error);
  }
}

/* ========================================================================== */
/* the environment, one for the process: the browser's processes and where   */
/* they keep cookies and storage between runs.                                */
/* ========================================================================== */

static enum : u8 {
  ENV_NONE,
  ENV_MAKING,
  ENV_READY,
  ENV_FAILED,
} env_state;

static ICoreWebView2Environment *env;

static void make_controller(civue_webview *w);

static HRESULT STDMETHODCALLTYPE env_made(Made *self, HRESULT hr,
                                          ICoreWebView2Environment *made) {
  if (FAILED(hr) || !made) {
    env_state = ENV_FAILED;

    for (civue_webview *w = all; w; w = w->after) {
      report(w, "WebView2 did not start.");
    }

    return S_OK;
  }

  ICoreWebView2Environment_AddRef(made);
  env = made;
  env_state = ENV_READY;

  for (civue_webview *w = all; w; w = w->after) {
    if (w->host && !w->controller && !w->making) {
      make_controller(w);
    }
  }

  return S_OK;
}

static const ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl
  env_made_vtbl = {
    UNKNOWN(Made),
    .Invoke = env_made,
};

// %LOCALAPPDATA%\<app id>\webview, from malloc.
static wchar_t *data_folder() {
  wchar_t *local = nullptr;

  if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                  nullptr, &local))) {
    CoTaskMemFree(local);
    return nullptr;
  }

  char *base = civue__utf8(local, -1);
  char *path = xfmt("%s\\%s\\webview", base, civue__app_id());
  wchar_t *folder = civue__wide(path, -1);

  CoTaskMemFree(local);
  free(base);
  free(path);
  SHCreateDirectoryExW(nullptr, folder, nullptr);
  return folder;
}

static void make_environment() {
  if (env_state != ENV_NONE) {
    return;
  }

  civue__com_init();
  env_state = ENV_MAKING;

  wchar_t *folder = data_folder();
  Handler *h = handler_new(
    &env_made_vtbl,
    &IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, nullptr);
  HRESULT hr = loader ? loader(nullptr, folder, nullptr, (void *)h)
                      : internal(true, 0, folder, nullptr, (void *)h);

  if (FAILED(hr) && env_state == ENV_MAKING) {
    env_made((void *)h, hr, nullptr);
  }

  release((IUnknown *)h);
  free(folder);
}

/* ========================================================================== */
/* urls, as the page's requests have them.                                    */
/* ========================================================================== */

// how long its scheme and host are, with the port, 0 for no such url.
static usize origin_len(const wchar_t *url) {
  const wchar_t *sep = url ? wcsstr(url, L"://") : nullptr;

  return sep ? (usize)(sep + 3 - url) + wcscspn(sep + 3, L"/?#") : 0;
}

static bool same_origin(const wchar_t *a, const wchar_t *b) {
  usize n = origin_len(a);

  return n && n == origin_len(b) && !_wcsnicmp(a, b, n);
}

// whether `a` and `b` are the same document, as a server sees it: an empty
// path is the root, the fragment stays in the page.
static bool same_document(const wchar_t *a, const wchar_t *b) {
  if (!same_origin(a, b)) {
    return false;
  }

  a += origin_len(a);
  b += origin_len(b);
  a += *a == L'/' && (!a[1] || a[1] == L'?' || a[1] == L'#');
  b += *b == L'/' && (!b[1] || b[1] == L'?' || b[1] == L'#');

  usize na = wcscspn(a, L"#"), nb = wcscspn(b, L"#");

  return na == nb && !wcsncmp(a, b, na);
}

// web pages only: a page does not get to open files or other apps.
static void open_in_browser(const wchar_t *url) {
  if (url &&
      (!_wcsnicmp(url, L"https://", 8) || !_wcsnicmp(url, L"http://", 7))) {
    char *page = civue__utf8(url, -1);

    civue_open_url(page);
    free(page);
  }
}

/* ========================================================================== */
/* the page's events.                                                         */
/* ========================================================================== */

static HRESULT STDMETHODCALLTYPE
posted(ICoreWebView2WebMessageReceivedEventHandler *self, ICoreWebView2 *,
       ICoreWebView2WebMessageReceivedEventArgs *args) {
  civue_webview *w = of(self);
  wchar_t *text = nullptr;

  if (!w || !w->message ||
      FAILED(ICoreWebView2WebMessageReceivedEventArgs_TryGetWebMessageAsString(
        args, &text))) {
    return S_OK;
  }

  defer CoTaskMemFree(text);
  char *said = civue__utf8(text, -1);
  defer free(said);
  w->message(w->ctx, said);
  return S_OK;
}

static const ICoreWebView2WebMessageReceivedEventHandlerVtbl posted_vtbl = {
  UNKNOWN(ICoreWebView2WebMessageReceivedEventHandler),
  .Invoke = posted,
};

// a link the user follows to another site opens in the system's browser.
// the page's own scripts still move it where they want, and so does the
// app.
static HRESULT STDMETHODCALLTYPE
starting(ICoreWebView2NavigationStartingEventHandler *self, ICoreWebView2 *view,
         ICoreWebView2NavigationStartingEventArgs *args) {
  civue_webview *w = of(self);
  BOOL user = false, redirected = false;

  ICoreWebView2NavigationStartingEventArgs_get_IsRedirected(args, &redirected);

  if (!w || redirected) {
    return S_OK;
  }

  // the app's own navigations count as the user's.
  if (w->ours) {
    w->ours--;
    return S_OK;
  }

  ICoreWebView2NavigationStartingEventArgs_get_IsUserInitiated(args, &user);

  ICoreWebView2NavigationStartingEventArgs3 *args3 = nullptr;
  COREWEBVIEW2_NAVIGATION_KIND kind = COREWEBVIEW2_NAVIGATION_KIND_NEW_DOCUMENT;

  if (SUCCEEDED(ICoreWebView2NavigationStartingEventArgs_QueryInterface(
        args, &IID_ICoreWebView2NavigationStartingEventArgs3,
        (void **)&args3))) {
    ICoreWebView2NavigationStartingEventArgs3_get_NavigationKind(args3, &kind);
    ICoreWebView2NavigationStartingEventArgs3_Release(args3);
  }

  if (!user || kind != COREWEBVIEW2_NAVIGATION_KIND_NEW_DOCUMENT) {
    return S_OK;
  }

  wchar_t *url = nullptr, *here = nullptr;

  ICoreWebView2NavigationStartingEventArgs_get_Uri(args, &url);
  ICoreWebView2_get_Source(view, &here);
  defer {
    CoTaskMemFree(url);
    CoTaskMemFree(here);
  }

  if (!same_origin(url, here)) {
    ICoreWebView2NavigationStartingEventArgs_put_Cancel(args, true);
    open_in_browser(url);
  }

  return S_OK;
}

static const ICoreWebView2NavigationStartingEventHandlerVtbl starting_vtbl = {
  UNKNOWN(ICoreWebView2NavigationStartingEventHandler),
  .Invoke = starting,
};

#define STATUS(name) COREWEBVIEW2_WEB_ERROR_STATUS_##name

static const char *const errors[] = {
  [STATUS(CERTIFICATE_COMMON_NAME_IS_INCORRECT)] =
    "The server's certificate is for another name.",
  [STATUS(CERTIFICATE_EXPIRED)] = "The server's certificate has expired.",
  [STATUS(CLIENT_CERTIFICATE_CONTAINS_ERRORS)] =
    "The client certificate has errors.",
  [STATUS(CERTIFICATE_REVOKED)] = "The server's certificate was revoked.",
  [STATUS(CERTIFICATE_IS_INVALID)] = "The server's certificate is not valid.",
  [STATUS(SERVER_UNREACHABLE)] = "The server cannot be reached.",
  [STATUS(TIMEOUT)] = "The request timed out.",
  [STATUS(ERROR_HTTP_INVALID_SERVER_RESPONSE)] =
    "The server's response is not valid.",
  [STATUS(CONNECTION_ABORTED)] = "The connection was aborted.",
  [STATUS(CONNECTION_RESET)] = "The connection was reset.",
  [STATUS(DISCONNECTED)] = "The Internet connection appears to be offline.",
  [STATUS(CANNOT_CONNECT)] = "Could not connect to the server.",
  [STATUS(HOST_NAME_NOT_RESOLVED)] =
    "A server with the specified hostname could not be found.",
  [STATUS(REDIRECT_FAILED)] = "A redirect failed.",
  [STATUS(UNEXPECTED_ERROR)] = "An unexpected error occurred.",
  [STATUS(VALID_AUTHENTICATION_CREDENTIALS_REQUIRED)] =
    "The server asks for credentials.",
  [STATUS(VALID_PROXY_AUTHENTICATION_REQUIRED)] =
    "The proxy asks for credentials.",
};

static HRESULT STDMETHODCALLTYPE
completed(ICoreWebView2NavigationCompletedEventHandler *self, ICoreWebView2 *,
          ICoreWebView2NavigationCompletedEventArgs *args) {
  civue_webview *w = of(self);
  BOOL ok = false;
  COREWEBVIEW2_WEB_ERROR_STATUS status = STATUS(UNKNOWN);

  if (!w) {
    return S_OK;
  }

  ICoreWebView2NavigationCompletedEventArgs_get_IsSuccess(args, &ok);
  ICoreWebView2NavigationCompletedEventArgs_get_WebErrorStatus(args, &status);

  // a page the server answered with an error loaded all the same, as it
  // does on the other systems.
  ICoreWebView2NavigationCompletedEventArgs2 *args2 = nullptr;
  int http = 0;

  if (!ok && status == STATUS(UNKNOWN) &&
      SUCCEEDED(ICoreWebView2NavigationCompletedEventArgs_QueryInterface(
        args, &IID_ICoreWebView2NavigationCompletedEventArgs2,
        (void **)&args2))) {
    ICoreWebView2NavigationCompletedEventArgs2_get_HttpStatusCode(args2, &http);
    ICoreWebView2NavigationCompletedEventArgs2_Release(args2);
  }

  // a load another one replaced, or a link sent to the browser, failed at
  // nothing.
  if (ok || http > 0) {
    report(w, nullptr);
  }
  else if (status != STATUS(OPERATION_CANCELED)) {
    report(w, (u32)status < countof(errors) && errors[status] ? errors[status]
                                                              : "failed");
  }

  return S_OK;
}

static const ICoreWebView2NavigationCompletedEventHandlerVtbl completed_vtbl = {
  UNKNOWN(ICoreWebView2NavigationCompletedEventHandler),
  .Invoke = completed,
};

// a page's popups, of the user's clicks alone, open in the system's
// browser.
static HRESULT STDMETHODCALLTYPE
new_window(ICoreWebView2NewWindowRequestedEventHandler *self, ICoreWebView2 *,
           ICoreWebView2NewWindowRequestedEventArgs *args) {
  BOOL user = false;
  wchar_t *url = nullptr;

  ICoreWebView2NewWindowRequestedEventArgs_put_Handled(args, true);
  ICoreWebView2NewWindowRequestedEventArgs_get_IsUserInitiated(args, &user);
  ICoreWebView2NewWindowRequestedEventArgs_get_Uri(args, &url);

  if (of(self) && user) {
    open_in_browser(url);
  }

  CoTaskMemFree(url);
  return S_OK;
}

static const ICoreWebView2NewWindowRequestedEventHandlerVtbl new_window_vtbl = {
  UNKNOWN(ICoreWebView2NewWindowRequestedEventHandler),
  .Invoke = new_window,
};

// the document of load_html, from memory at its base url: the page runs on
// that origin, the rest it asks for comes from the network.
static HRESULT STDMETHODCALLTYPE
requested(ICoreWebView2WebResourceRequestedEventHandler *self, ICoreWebView2 *,
          ICoreWebView2WebResourceRequestedEventArgs *args) {
  civue_webview *w = of(self);
  ICoreWebView2WebResourceRequest *request = nullptr;
  wchar_t *url = nullptr;

  if (!w || !w->html || !env ||
      FAILED(ICoreWebView2WebResourceRequestedEventArgs_get_Request(
        args, &request))) {
    return S_OK;
  }

  defer ICoreWebView2WebResourceRequest_Release(request);
  ICoreWebView2WebResourceRequest_get_Uri(request, &url);
  defer CoTaskMemFree(url);

  if (!same_document(url, w->html_url)) {
    return S_OK;
  }

  IStream *page =
    SHCreateMemStream((const BYTE *)w->html, (UINT)strlen(w->html));
  ICoreWebView2WebResourceResponse *response = nullptr;

  if (page && SUCCEEDED(ICoreWebView2Environment_CreateWebResourceResponse(
                env, page, 200, L"OK",
                L"Content-Type: text/html; charset=utf-8", &response))) {
    ICoreWebView2WebResourceRequestedEventArgs_put_Response(args, response);
    ICoreWebView2WebResourceResponse_Release(response);
  }

  if (page) {
    IStream_Release(page);
  }

  return S_OK;
}

static const ICoreWebView2WebResourceRequestedEventHandlerVtbl requested_vtbl =
  {
    UNKNOWN(ICoreWebView2WebResourceRequestedEventHandler),
    .Invoke = requested,
};

static void drop(civue_webview *w);

static HRESULT STDMETHODCALLTYPE
failed(ICoreWebView2ProcessFailedEventHandler *self, ICoreWebView2 *,
       ICoreWebView2ProcessFailedEventArgs *args) {
  civue_webview *w = of(self);
  COREWEBVIEW2_PROCESS_FAILED_KIND kind =
    COREWEBVIEW2_PROCESS_FAILED_KIND_UNKNOWN_PROCESS_EXITED;

  ICoreWebView2ProcessFailedEventArgs_get_ProcessFailedKind(args, &kind);

  if (!w) {
    return S_OK;
  }

  // the browser's end takes the environment and every page with it: they
  // are made again when next shown, and load what is asked of them then.
  if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED) {
    drop(w);

    if (env) {
      ICoreWebView2Environment_Release(env);
      env = nullptr;
      env_state = ENV_NONE;
    }
  }

  if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED ||
      kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
    report(w, "the page's process ended");
  }

  return S_OK;
}

static const ICoreWebView2ProcessFailedEventHandlerVtbl failed_vtbl = {
  UNKNOWN(ICoreWebView2ProcessFailedEventHandler),
  .Invoke = failed,
};

static HRESULT STDMETHODCALLTYPE
got_focus(ICoreWebView2FocusChangedEventHandler *self,
          ICoreWebView2Controller *, IUnknown *) {
  civue_webview *w = of(self);

  if (w) {
    w->focused = true;
    civue_window_wake();
  }

  return S_OK;
}

static HRESULT STDMETHODCALLTYPE
lost_focus(ICoreWebView2FocusChangedEventHandler *self,
           ICoreWebView2Controller *, IUnknown *) {
  civue_webview *w = of(self);

  if (w) {
    w->focused = false;
    civue_window_wake();
  }

  return S_OK;
}

static const ICoreWebView2FocusChangedEventHandlerVtbl got_focus_vtbl = {
  UNKNOWN(ICoreWebView2FocusChangedEventHandler),
  .Invoke = got_focus,
};

static const ICoreWebView2FocusChangedEventHandlerVtbl lost_focus_vtbl = {
  UNKNOWN(ICoreWebView2FocusChangedEventHandler),
  .Invoke = lost_focus,
};

/* ========================================================================== */
/* the page.                                                                  */
/* ========================================================================== */

static void navigate(civue_webview *w, const wchar_t *url) {
  w->ours++;

  if (FAILED(ICoreWebView2_Navigate(w->view, url))) {
    w->ours--;
    report(w, "The URL is not valid.");
  }
}

// does what `a` asks, and frees it.
static void run(civue_webview *w, Ask *a) {
  defer {
    free(a->text);
    free(a->html);
  }

  // freed by a callback of the app's, asks and all.
  if (!w->view) {
    return;
  }

  if (a->kind != ASK_EVAL) {
    free(w->html);
    free(w->html_url);
    w->html = nullptr;
    w->html_url = nullptr;
  }

  switch (a->kind) {
    case ASK_HTML:
      // no origin to give it: the page is about:blank's.
      if (!a->text) {
        wchar_t *page = civue__wide(a->html, -1);

        w->ours++;
        ICoreWebView2_NavigateToString(w->view, page);
        free(page);
        break;
      }

      w->html = a->html;
      w->html_url = a->text;
      a->html = nullptr;
      a->text = nullptr;
      navigate(w, w->html_url);
      break;
    case ASK_URL:
      navigate(w, a->text);
      break;
    case ASK_EVAL:
      ICoreWebView2_ExecuteScript(w->view, a->text, nullptr);
      break;
  }
}

static void ask(civue_webview *w, Ask a) {
  if (w->view) {
    run(w, &a);
  }
  else {
    vec_push(&w->waiting, a);
  }
}

// window.civue in the page's main frame, its frames left without.
static const wchar_t bridge[] =
  L"if (window === window.top) {\n"
  L"  window.civue = { postMessage: (s) => "
  L"window.chrome.webview.postMessage(String(s)) };\n"
  L"}\n";

static void fit(civue_webview *w) {
  RECT inside = {0, 0, w->at.right - w->at.left, w->at.bottom - w->at.top};

  ICoreWebView2Controller_put_Bounds(w->controller, inside);
  ICoreWebView2Controller_NotifyParentWindowPositionChanged(w->controller);
}

static void settle(civue_webview *w) {
  ICoreWebView2Settings *settings = nullptr;

  if (FAILED(ICoreWebView2_get_Settings(w->view, &settings))) {
    return;
  }

  defer ICoreWebView2Settings_Release(settings);
  // a part of the app's window, not a browser: no bar of links, no zoom,
  // no browser's shortcuts.
  ICoreWebView2Settings_put_IsStatusBarEnabled(settings, false);
  ICoreWebView2Settings_put_IsZoomControlEnabled(settings, false);

  ICoreWebView2Settings2 *settings2 = nullptr;
  ICoreWebView2Settings3 *settings3 = nullptr;

  if (w->user_agent &&
      SUCCEEDED(ICoreWebView2Settings_QueryInterface(
        settings, &IID_ICoreWebView2Settings2, (void **)&settings2))) {
    ICoreWebView2Settings2_put_UserAgent(settings2, w->user_agent);
    ICoreWebView2Settings2_Release(settings2);
  }

  if (SUCCEEDED(ICoreWebView2Settings_QueryInterface(
        settings, &IID_ICoreWebView2Settings3, (void **)&settings3))) {
    ICoreWebView2Settings3_put_AreBrowserAcceleratorKeysEnabled(settings3,
                                                                false);
    ICoreWebView2Settings3_Release(settings3);
  }
}

static HRESULT STDMETHODCALLTYPE
controller_made(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *self,
                HRESULT hr, ICoreWebView2Controller *made) {
  civue_webview *w = ((Handler *)self)->w;

  w->making = false;

  if (w->freed || !w->host) {
    if (made) {
      ICoreWebView2Controller_Close(made);
    }

    return S_OK;
  }

  if (FAILED(hr) || !made ||
      FAILED(ICoreWebView2Controller_get_CoreWebView2(made, &w->view))) {
    w->broken = true;
    report(w, "The page could not be made.");
    return S_OK;
  }

  ICoreWebView2Controller_AddRef(made);
  w->controller = made;

  if (w->transparent) {
    ICoreWebView2Controller2 *c2 = nullptr;

    if (SUCCEEDED(ICoreWebView2Controller_QueryInterface(
          made, &IID_ICoreWebView2Controller2, (void **)&c2))) {
      ICoreWebView2Controller2_put_DefaultBackgroundColor(
        c2, (COREWEBVIEW2_COLOR){});
      ICoreWebView2Controller2_Release(c2);
    }
  }

  settle(w);
  ICoreWebView2_AddScriptToExecuteOnDocumentCreated(w->view, bridge, nullptr);
  ICoreWebView2_AddWebResourceRequestedFilter(
    w->view, L"*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_DOCUMENT);

  LISTEN(ICoreWebView2_add_WebMessageReceived, w->view, posted_vtbl,
         IID_ICoreWebView2WebMessageReceivedEventHandler, w, &w->on.message);
  LISTEN(ICoreWebView2_add_NavigationStarting, w->view, starting_vtbl,
         IID_ICoreWebView2NavigationStartingEventHandler, w, &w->on.starting);
  LISTEN(ICoreWebView2_add_NavigationCompleted, w->view, completed_vtbl,
         IID_ICoreWebView2NavigationCompletedEventHandler, w, &w->on.completed);
  LISTEN(ICoreWebView2_add_NewWindowRequested, w->view, new_window_vtbl,
         IID_ICoreWebView2NewWindowRequestedEventHandler, w, &w->on.window);
  LISTEN(ICoreWebView2_add_WebResourceRequested, w->view, requested_vtbl,
         IID_ICoreWebView2WebResourceRequestedEventHandler, w, &w->on.resource);
  LISTEN(ICoreWebView2_add_ProcessFailed, w->view, failed_vtbl,
         IID_ICoreWebView2ProcessFailedEventHandler, w, &w->on.failed);
  LISTEN(ICoreWebView2Controller_add_GotFocus, made, got_focus_vtbl,
         IID_ICoreWebView2FocusChangedEventHandler, w, &w->on.got);
  LISTEN(ICoreWebView2Controller_add_LostFocus, made, lost_focus_vtbl,
         IID_ICoreWebView2FocusChangedEventHandler, w, &w->on.lost);

  fit(w);
  ICoreWebView2Controller_put_IsVisible(made, w->visible);

  vec_each(&w->waiting, a) {
    run(w, a);
  }

  vec_clear(&w->waiting);
  return S_OK;
}

static const ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl
  controller_made_vtbl = {
    UNKNOWN(ICoreWebView2CreateCoreWebView2ControllerCompletedHandler),
    .Invoke = controller_made,
};

static void make_controller(civue_webview *w) {
  Handler *h = handler_new(
    &controller_made_vtbl,
    &IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, w);

  w->making = true;

  if (FAILED(ICoreWebView2Environment_CreateCoreWebView2Controller(
        env, w->host, (void *)h))) {
    w->making = false;
    w->broken = true;
    report(w, "The page could not be made.");
  }

  release((IUnknown *)h);
}

// lets go of the page, its window kept.
static void drop(civue_webview *w) {
  if (w->view) {
    ICoreWebView2_remove_WebMessageReceived(w->view, w->on.message);
    ICoreWebView2_remove_NavigationStarting(w->view, w->on.starting);
    ICoreWebView2_remove_NavigationCompleted(w->view, w->on.completed);
    ICoreWebView2_remove_NewWindowRequested(w->view, w->on.window);
    ICoreWebView2_remove_WebResourceRequested(w->view, w->on.resource);
    ICoreWebView2_remove_ProcessFailed(w->view, w->on.failed);
    ICoreWebView2_Release(w->view);
    w->view = nullptr;
  }

  if (w->controller) {
    ICoreWebView2Controller_remove_GotFocus(w->controller, w->on.got);
    ICoreWebView2Controller_remove_LostFocus(w->controller, w->on.lost);
    ICoreWebView2Controller_Close(w->controller);
    ICoreWebView2Controller_Release(w->controller);
    w->controller = nullptr;
  }

  w->focused = false;
  w->ours = 0;
}

/* ========================================================================== */
/* its window, a child of the ui's that paints nothing of its own: what the   */
/* page leaves clear shows the ui's frame.                                    */
/* ========================================================================== */

static LRESULT CALLBACK host_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint;
      BeginPaint(hwnd, &paint);
      EndPaint(hwnd, &paint);
      return 0;
    }
    // the ui's window closed, and this one with it.
    case WM_NCDESTROY: {
      civue_webview *w = (void *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

      if (w) {
        w->host = nullptr;
        w->visible = false;
        drop(w);
      }

      break;
    }
    default:
      break;
  }

  return DefWindowProcW(hwnd, msg, wp, lp);
}

static HWND make_host(civue_webview *w, HWND window) {
  static ATOM kind;

  if (!kind) {
    kind = RegisterClassExW(&(WNDCLASSEXW){
      .cbSize = sizeof(WNDCLASSEXW),
      .lpfnWndProc = host_proc,
      .hInstance = GetModuleHandleW(nullptr),
      .hCursor = LoadCursorW(nullptr, IDC_ARROW),
      .lpszClassName = L"civue_webview",
    });
  }

  HWND host = CreateWindowExW(
    0, L"civue_webview", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0,
    0, 0, 0, window, nullptr, GetModuleHandleW(nullptr), nullptr);

  if (host) {
    SetWindowLongPtrW(host, GWLP_USERDATA, (LONG_PTR)w);
  }

  return host;
}

// the keys go back to the ui's window when the page had them.
static void unfocus(civue_webview *w) {
  HWND focus = GetFocus();
  HWND window = w->host ? GetParent(w->host) : nullptr;

  if (window && (w->focused || focus == w->host || IsChild(w->host, focus))) {
    SetFocus(window);
  }

  w->focused = false;
}

static void hide(civue_webview *w) {
  if (!w->visible) {
    return;
  }

  unfocus(w);
  w->visible = false;

  if (w->controller) {
    ICoreWebView2Controller_put_IsVisible(w->controller, false);
  }

  ShowWindow(w->host, SW_HIDE);
}

// over the rect the frame showed it in, on whole pixels like what the ui
// paints around it.
static void put(civue_webview *w, HWND window) {
  UINT dpi = GetDpiForWindow(window);
  f32 s = (dpi ? dpi : USER_DEFAULT_SCREEN_DPI) / 96.0f;
  civue_rect r = w->rect;
  RECT at = {lroundf(r.x0 * s), lroundf(r.y0 * s), lroundf(r.x1 * s),
             lroundf(r.y1 * s)};
  i32 corner = (i32)lroundf(w->radius * s);
  i32 width = MAX(at.right - at.left, 0), height = MAX(at.bottom - at.top, 0);
  bool moved = !EqualRect(&at, &w->at);
  bool resized =
    width != w->at.right - w->at.left || height != w->at.bottom - w->at.top;

  if (moved) {
    SetWindowPos(w->host, nullptr, at.left, at.top, width, height,
                 SWP_NOZORDER | SWP_NOACTIVATE);
  }

  if (resized || corner != w->corner) {
    HRGN round = corner > 0 ? CreateRoundRectRgn(0, 0, width + 1, height + 1,
                                                 corner * 2, corner * 2)
                            : nullptr;

    // the window owns the region from here.
    SetWindowRgn(w->host, round, true);
  }

  w->at = at;
  w->corner = corner;

  if (moved && w->controller) {
    fit(w);
  }

  if (!w->visible) {
    w->visible = true;
    ShowWindow(w->host, SW_SHOWNA);

    if (w->controller) {
      ICoreWebView2Controller_put_IsVisible(w->controller, true);
    }
  }
}

// puts the webviews the frame numbered `frame` showed where it showed them,
// and hides the others.
static void place(u64 frame) {
  HWND window = civue__hwnd();
  // the page's popups are placed on the screen: they are told the window
  // moved.
  static POINT origin;
  POINT now = {};
  bool moved = window && ClientToScreen(window, &now) &&
               (now.x != origin.x || now.y != origin.y);

  if (moved) {
    origin = now;
  }

  for (civue_webview *w = all; w; w = w->after) {
    if (!w->host && window) {
      w->host = make_host(w, window);
    }

    if (!w->host) {
      continue;
    }

    if (!w->controller && !w->making && !w->broken) {
      if (env_state == ENV_READY) {
        make_controller(w);
      }
      else {
        make_environment();
      }
    }

    if (w->shown != frame || !window) {
      hide(w);
      continue;
    }

    put(w, window);

    if (moved && w->controller) {
      ICoreWebView2Controller_NotifyParentWindowPositionChanged(w->controller);
    }
  }
}

civue_webview *civue_webview_new(const civue_webview_options *o) {
  // a system without webview2 has no web views.
  if (!runtime_found() || env_state == ENV_FAILED) {
    return nullptr;
  }

  civue_webview *w = xcalloc(1, sizeof *w);
  w->refs = 1;
  w->ctx = o->ctx;
  w->message = o->message;
  w->loaded = o->loaded;
  w->user_agent = o->user_agent ? civue__wide(o->user_agent, -1) : nullptr;
  w->transparent = o->transparent;
  w->radius = o->radius;
  w->corner = -1;
  w->after = all;
  all = w;
  civue__webviews_place = place;

  // the browser starts with the window, whose options name the app's
  // folder.
  HWND window = civue__hwnd();

  if (window) {
    w->host = make_host(w, window);
  }

  if (w->host && env_state == ENV_READY) {
    make_controller(w);
  }
  else if (w->host) {
    make_environment();
  }

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

  w->freed = true;
  w->message = nullptr;
  w->loaded = nullptr;

  if (w->host) {
    unfocus(w);
    SetWindowLongPtrW(w->host, GWLP_USERDATA, 0);
    DestroyWindow(w->host);
    w->host = nullptr;
  }

  drop(w);
  unref(w);
}

void civue_webview_load_html(civue_webview *w, const char *html,
                             const char *base_url) {
  if (w && html) {
    ask(w, (Ask){ASK_HTML, base_url ? civue__wide(base_url, -1) : nullptr,
                 xstrdup(html)});
  }
}

void civue_webview_load_url(civue_webview *w, const char *url) {
  if (w && url) {
    ask(w, (Ask){ASK_URL, civue__wide(url, -1)});
  }
}

void civue_webview_eval(civue_webview *w, const char *js) {
  if (w && js) {
    ask(w, (Ask){ASK_EVAL, civue__wide(js, -1)});
  }
}

void civue_webview_show(civue_ui *ui, civue_webview *w, civue_rect r) {
  if (!w) {
    return;
  }

  w->shown = ui->frames;
  w->rect = r;

  // its window takes the clicks itself, the region keeps what is under it
  // from hovering.
  civue_id id = civue_idn(civue_id_of("civue_webview"), (uintptr_t)w);
  civue_interact(ui, id, r, CIVUE_SENSE_HOVER);

  // the ui only hears the presses out of the page: one takes the keys back.
  const civue_input *in = &ui->input;

  if (w->focused && (in->pressed[0] || in->pressed[1] || in->pressed[2])) {
    unfocus(w);
  }

  // while the page has the keys, no widget of the ui does.
  if (w->focused) {
    ui->focus = id;
    civue_claim_focus(ui);
  }
}
