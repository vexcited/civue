#include "../components.h"

#include <civue/dsl.h>

static void title(Look k) {
  // the title and its caption sit on one baseline, whatever their sizes.
  div(flex, items_baseline, gap(3)) {
    // iconify's c logo keeps its own colors, the others take the text's.
    icon(logos, c, size(6), self_center);

    text("Layout", text_size(2xl), line_height(px(32)), font_weight(bold));
    text("grid, baselines, collapsing margins and scrolling",
         text_color(k.faint));
  }
}

static void theme_button(App *app) {
  Look k = app->look;

  div(key(id("theme")), flex, items_center, gap(2), px(3), py(1), rounded(md),
      border(1), border_color(k.line), hover(bg(k.raised)), cursor_pointer) {
    if (button().activated) {
      app->dark = !app->dark;
    }

    if (app->dark) {
      icon(lucide, sun);
      text("Light");
    }
    else {
      icon(lucide, moon);
      text("Dark");
    }
  }
}

void header(App *app) {
  div(flex, items_baseline, justify_between) {
    title(app->look);
    theme_button(app);
  }
}
