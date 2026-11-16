#import <AppKit/AppKit.h>
#import <WebKit/WebKit.h>

#include "darwin.h"
#include "window.h"

#include <civue/webview.h>

// a civue_webview is one of these, kept by `all` until it is freed.
@interface CivueWebview
    : NSObject <WKScriptMessageHandler, WKNavigationDelegate>
@end

static bool same_origin(NSURL *a, NSURL *b) {
  NSString *as = a.scheme.lowercaseString, *bs = b.scheme.lowercaseString;
  NSString *ah = a.host.lowercaseString, *bh = b.host.lowercaseString;
  NSNumber *ap = a.port, *bp = b.port;

  return (as == bs || [as isEqualToString:bs]) &&
         (ah == bh || [ah isEqualToString:bh]) &&
         (ap == bp || [ap isEqualToNumber:bp]);
}

@implementation CivueWebview {
@public
  // made once the window shows, what was asked of it before waits.
  WKWebView *view;
  NSMutableArray<void (^)(WKWebView *)> *waiting;
  NSString *user_agent;
  bool transparent;
  f32 radius;
  void *ctx;
  void (*message)(void *ctx, const char *text);
  void (*loaded)(void *ctx, const char *error);
  // the frame that last showed it, and where.
  u64 shown;
  civue_rect rect;
}

- (void)report:(NSError *)error {
  // a load another one replaced, or a link sent to the browser, failed at
  // nothing.
  bool replaced = [error.domain isEqualToString:NSURLErrorDomain] &&
                  error.code == NSURLErrorCancelled;
  bool sent_away =
    [error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102;

  if (loaded && !replaced && !sent_away) {
    loaded(ctx, error.localizedDescription.UTF8String ?: "failed");
  }
}

- (void)userContentController:(WKUserContentController *)controller
      didReceiveScriptMessage:(WKScriptMessage *)posted {
  // the handler is in every frame of the page, window.civue in its main one.
  if (!message || !posted.frameInfo.mainFrame ||
      ![posted.body isKindOfClass:NSString.class]) {
    return;
  }

  message(ctx, ((NSString *)posted.body).UTF8String ?: "");
}

- (void)webView:(WKWebView *)web didFinishNavigation:(WKNavigation *)nav {
  if (loaded) {
    loaded(ctx, nullptr);
  }
}

- (void)webView:(WKWebView *)web
  didFailNavigation:(WKNavigation *)nav
          withError:(NSError *)error {
  [self report:error];
}

- (void)webView:(WKWebView *)web
  didFailProvisionalNavigation:(WKNavigation *)nav
                     withError:(NSError *)error {
  [self report:error];
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)web {
  if (loaded) {
    loaded(ctx, "the page's process ended");
  }
}

- (void)webView:(WKWebView *)web
  decidePolicyForNavigationAction:(WKNavigationAction *)action
                  decisionHandler:(void (^)(WKNavigationActionPolicy))decide {
  NSURL *url = action.request.URL;
  // a new window has no frame to go to. the page's own scripts still move it
  // where they want, only the user's clicks leave.
  bool new_window = !action.targetFrame;
  bool leaves = action.navigationType == WKNavigationTypeLinkActivated &&
                action.targetFrame.mainFrame && !same_origin(url, web.URL);

  if (!new_window && !leaves) {
    decide(WKNavigationActionPolicyAllow);
    return;
  }

  NSString *scheme = url.scheme.lowercaseString;

  // web pages only: a page does not get to open files or other apps.
  if ([scheme isEqualToString:@"https"] || [scheme isEqualToString:@"http"]) {
    civue_open_url(url.absoluteString.UTF8String);
  }

  decide(WKNavigationActionPolicyCancel);
}

@end

static NSMutableArray<CivueWebview *> *all;

static CivueWebview *of(civue_webview *w) {
  return (__bridge CivueWebview *)(void *)w;
}

// hides it, the keys going back to the ui when the page had them.
static void hide(WKWebView *view) {
  NSResponder *first = view.window.firstResponder;

  if ([first isKindOfClass:NSView.class] &&
      [(NSView *)first isDescendantOf:view]) {
    [view.window makeFirstResponder:view.superview];
  }

  view.hidden = YES;
}

// a WKWebView made before the app has its window on the screen runs its
// page's timers late or never, most of the time: it is made once the
// window shows, and loads then what it was asked to.
static void make(CivueWebview *w) {
  // the default data store, on disk: cookies last as in a browser.
  WKWebViewConfiguration *config = [WKWebViewConfiguration new];
  NSString *bridge = @"window.civue = { postMessage: (s) => "
                     @"window.webkit.messageHandlers.civue.postMessage("
                     @"String(s)) };";
  WKUserScript *script = [[WKUserScript alloc]
      initWithSource:bridge
       injectionTime:WKUserScriptInjectionTimeAtDocumentStart
    forMainFrameOnly:YES];
  [config.userContentController addUserScript:script];
  // the controller keeps `w`, which keeps the view: undone when freed.
  [config.userContentController addScriptMessageHandler:w name:@"civue"];

  WKWebView *view = [[WKWebView alloc] initWithFrame:NSZeroRect
                                       configuration:config];
  view.navigationDelegate = w;
  view.hidden = YES;
  view.customUserAgent = w->user_agent;

  if (w->transparent) {
    view.underPageBackgroundColor = NSColor.clearColor;

    // appkit's WKWebView has no public way not to paint its background,
    // this key is the one every embedder sets. should its setter ever go,
    // the page is only opaque.
    if ([view
          respondsToSelector:NSSelectorFromString(@"_setDrawsBackground:")]) {
      [view setValue:@NO forKey:@"drawsBackground"];
    }
  }

  if (w->radius > 0) {
    view.wantsLayer = YES;
    view.layer.cornerRadius = w->radius;
    view.layer.masksToBounds = YES;
  }

  w->view = view;

  for (void (^asked)(WKWebView *) in w->waiting) {
    asked(view);
  }

  w->waiting = nil;
}

static void ask(civue_webview *w, void (^what)(WKWebView *view)) {
  if (!w) {
    return;
  }

  CivueWebview *web = of(w);

  if (web->view) {
    what(web->view);
  }
  else {
    [web->waiting addObject:what];
  }
}

// puts the webviews the frame numbered `frame` showed where it showed them,
// in the ui's view, and hides the others.
static void place(u64 frame) {
  NSView *host = civue__window_view();

  for (CivueWebview *w in all) {
    if (!w->view && host) {
      make(w);
    }

    WKWebView *view = w->view;

    if (w->shown != frame || !host) {
      if (!view.hidden) {
        hide(view);
      }

      continue;
    }

    if (view.superview != host) {
      [host addSubview:view];
    }

    // on whole pixels, like what the ui paints around it.
    f64 s = host.window ? host.window.backingScaleFactor : 1;
    f64 x0 = round(w->rect.x0 * s) / s, y0 = round(w->rect.y0 * s) / s;
    f64 x1 = round(w->rect.x1 * s) / s, y1 = round(w->rect.y1 * s) / s;
    NSRect at = NSMakeRect(x0, y0, fmax(x1 - x0, 0), fmax(y1 - y0, 0));

    if (!NSEqualRects(view.frame, at)) {
      view.frame = at;
    }

    if (view.hidden) {
      view.hidden = NO;
    }
  }
}

civue_webview *civue_webview_new(const civue_webview_options *o) {
  @autoreleasepool {
    CivueWebview *w = [CivueWebview new];
    w->ctx = o->ctx;
    w->message = o->message;
    w->loaded = o->loaded;
    w->user_agent = o->user_agent ? ns_str(o->user_agent) : nil;
    w->transparent = o->transparent;
    w->radius = o->radius;
    w->waiting = [NSMutableArray array];

    if (!all) {
      all = [NSMutableArray array];
      civue__webviews_place = place;
    }

    [all addObject:w];

    if (civue__window_view().window.visible) {
      make(w);
    }

    return (civue_webview *)(__bridge void *)w;
  }
}

void civue_webview_free(civue_webview *w) {
  if (!w) {
    return;
  }

  @autoreleasepool {
    CivueWebview *web = of(w);
    WKWebView *view = web->view;
    web->message = nullptr;
    web->loaded = nullptr;

    if (view) {
      hide(view);
      [view stopLoading];
      view.navigationDelegate = nil;
      [view.configuration.userContentController
        removeScriptMessageHandlerForName:@"civue"];
      [view removeFromSuperview];
    }

    [all removeObject:web];
  }
}

void civue_webview_load_html(civue_webview *w, const char *html,
                             const char *base_url) {
  @autoreleasepool {
    NSString *page = ns_str(html);
    NSURL *base = base_url ? [NSURL URLWithString:ns_str(base_url)] : nil;

    ask(w, ^(WKWebView *view) { [view loadHTMLString:page baseURL:base]; });
  }
}

void civue_webview_load_url(civue_webview *w, const char *url) {
  @autoreleasepool {
    NSURL *u = [NSURL URLWithString:ns_str(url)];

    if (!u) {
      return;
    }

    ask(w, ^(WKWebView *view) {
      [view loadRequest:[NSURLRequest requestWithURL:u]];
    });
  }
}

void civue_webview_eval(civue_webview *w, const char *js) {
  @autoreleasepool {
    NSString *code = ns_str(js);

    ask(w, ^(WKWebView *view) {
      [view evaluateJavaScript:code completionHandler:nil];
    });
  }
}

void civue_webview_show(civue_ui *ui, civue_webview *w, civue_rect r) {
  if (!w) {
    return;
  }

  CivueWebview *web = of(w);
  web->shown = ui->frames;
  web->rect = r;

  // the view takes the clicks itself, the region keeps what is under it
  // from hovering.
  civue_id id = civue_idn(civue_id_of("civue_webview"), (uintptr_t)w);
  civue_interact(ui, id, r, CIVUE_SENSE_HOVER);
}
