#ifndef CIVUE_DARWIN_WINDOW_H
#define CIVUE_DARWIN_WINDOW_H

#include <civue/window.h>

// sets the app's image of `c` as the pointer, false when it draws none.
bool civue__cursor_apply(const civue_window_options *o, civue_cursor c);

#endif
