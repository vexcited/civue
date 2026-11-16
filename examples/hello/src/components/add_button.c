#include "../components.h"

#include <civue/dsl.h>

static void add_tooltip(civue_rect anchor) {
  civue_id tip = id("add-tip");
  civue_tooltip_place at =
    tooltip_at(anchor, last_size(tip), CIVUE_TOOLTIP_BELOW, 6);

  overlay() {
    div(key(tip), left(px(at.rect.x0)), top(px(at.rect.y0)), px(2), py(1),
        rounded(md), bg(rgb(0x1e1e1e)), text_color(CIVUE_WHITE),
        whitespace_nowrap) {
      text("Counts the clicks");
    }
  }
}

void add_button(Hello *app, Look k) {
  civue_color pressed = civue_alpha(k.accent, .8f);

  div(key(id("add")), flex, items_center, justify_center, gap(1p5), w(24), h(7),
      rounded(md), bg(k.accent), active(bg(pressed)), text_color(CIVUE_WHITE),
      cursor_pointer) {
    civue_button_state add = button();

    if (add.activated) {
      app->count++;
    }

    icon(lucide, plus);
    text("Click me");

    if (tooltip(id("add"), &add.response)) {
      add_tooltip(add.response.rect);
    }
  }
}
