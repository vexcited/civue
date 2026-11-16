#include "../internal.h"

#include <civue/webview.h>

// no webview on linux yet: none is made, and the rest takes none.
civue_webview *civue_webview_new(const civue_webview_options *o) {
  return nullptr;
}

void civue_webview_free(civue_webview *w) {
}

void civue_webview_load_html(civue_webview *w, const char *html,
                             const char *base_url) {
}

void civue_webview_load_url(civue_webview *w, const char *url) {
}

void civue_webview_eval(civue_webview *w, const char *js) {
}

void civue_webview_show(civue_ui *ui, civue_webview *w, civue_rect r) {
}
