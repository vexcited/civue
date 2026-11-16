#include <civue/dsl.h>

#include "components.h"
#include "scrollbars.h"

static void cards(App *app) {
  div(flex, flex_1, min_h(0), gap(4), items_start) {
    grid_card(app);

    div(flex, self_stretch, flex_1, min_w(0)) {
      list_card(app);
    }
  }
}

static void frame(void *ctx, civue_ui *ui) {
  App *app = ctx;

  app->look = look_of(app->dark);
  ui->appearance = app->dark ? CIVUE_APPEARANCE_DARK : CIVUE_APPEARANCE_LIGHT;

  div(size(full), flex, flex_col, gap(4), p(5), text_size(px(13)),
      line_height(px(20)), text_color(app->look.text)) {
    header(app);
    cards(app);
  }
}

static civue_color background(void *ctx) {
  App *app = ctx;

  return look_of(app->dark).ground;
}

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) {
  return (civue_app){arg, frame, background};
}

int main() {
  App app = {};
  app.bars = scrollbars_of(&app);

  civue_window_options options = {
    .title = "Layout",
    .width = 820,
    .height = 560,
    .min_width = 640,
    .min_height = 420,
  };

  civue_window_run(&options, make, &app);

  return 0;
}
