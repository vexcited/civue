#include "../components.h"

#include <civue/dsl.h>

static void knob(bool on, Look k) {
  civue_place side = on ? CIVUE_PLACE_END : CIVUE_PLACE_START;

  div(flex, items_center, w(7), h(4), p(0p5), rounded(full),
      bg(on ? k.accent : k.track), style(.justify = side)) {
    div(size(3), rounded(full), bg(CIVUE_WHITE)) {}
  }
}

void dark_switch(Hello *app, Look k) {
  div(key(id("dark")), flex, items_center, gap(2), self_start, cursor_pointer) {
    civue_toggle_state dark = toggle(app->dark);

    knob(app->dark, k);
    text("Dark theme");

    app->dark = dark.on;
  }
}
