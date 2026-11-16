#ifndef CIVUE_DARWIN_WINDOW_H
#define CIVUE_DARWIN_WINDOW_H

#include <civue/window.h>

// sets the app's image of `c` as the pointer, false when it draws none.
bool civue__cursor_apply(const civue_window_options *o, civue_cursor c);

@class NSView;

// the ui's view, its points the ui's, nil before the window opens.
NSView *civue__window_view();
// places the webviews the frame numbered `frame` showed and hides the others,
// set by the first webview made: an app without one links none of it.
extern void (*civue__webviews_place)(uint64_t frame);

#endif
