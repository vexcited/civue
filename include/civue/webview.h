#ifndef CIVUE_WEBVIEW_H
#define CIVUE_WEBVIEW_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* a web page of the system's over a rect of the frame, a WKWebView on macos  */
/* and WPE WebKit painting into the frame on linux, where a build without it  */
/* has civue_webview_new answer nullptr and the rest take it. shown like a    */
/* widget, by a call each frame it is wanted. everything here runs on the     */
/* main thread.                                                               */
/* ========================================================================== */

typedef struct civue_webview civue_webview;

typedef struct {
  void *ctx;
  // what the page posted with window.civue.postMessage(text), on the main
  // thread. only its main frame is heard, not the frames it embeds.
  void (*message)(void *ctx, const char *text);
  // the page finished loading, or failed to with `error`.
  void (*loaded)(void *ctx, const char *error);
  // the system's when null.
  const char *user_agent;
  // the ui shows through where the page paints nothing.
  bool transparent;
  // rounded corners.
  float radius;
} civue_webview_options;

// a webview showing nothing yet, at any time: one made before the window
// opens loads its page once it has. cookies and storage are kept between
// runs, as a browser keeps them.
civue_webview *civue_webview_new(const civue_webview_options *o);
void civue_webview_free(civue_webview *w);
// `base_url` is the origin the page believes it is on.
void civue_webview_load_html(civue_webview *w, const char *html,
                             const char *base_url);
void civue_webview_load_url(civue_webview *w, const char *url);
// runs javascript in the page, its result dropped: the page posts what it
// has to say.
void civue_webview_eval(civue_webview *w, const char *js);
// shows it over `r` for this frame, hidden again by a frame that does not
// call this. it takes the pointer there, and sits over everything the frame
// draws, overlays included. a link to a new window, or one the user clicks
// to another site, opens in the system's browser.
void civue_webview_show(civue_ui *ui, civue_webview *w, civue_rect r);

#ifdef __cplusplus
}
#endif

#endif
