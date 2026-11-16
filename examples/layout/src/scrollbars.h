#ifndef LAYOUT_SCROLLBARS_H
#define LAYOUT_SCROLLBARS_H

#include "app.h"

// the bars civue has none of until the app draws them: a thumb that shows
// while the list scrolls or the pointer is over it.
civue_scrollbar_look scrollbars_of(App *app);

#endif
