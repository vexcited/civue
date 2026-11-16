#include <stdio.h>
#include <stdlib.h>

#include <civue/dsl.h>

#include "components.h"

static void counter(Hello *app, Look k) {
  char counted[32];
  snprintf(counted, sizeof counted, "Clicked %d times", app->count);

  div(flex, items_center, gap(3)) {
    add_button(app, k);
    text(counted, text_color(k.faint));
  }
}

static void card(Hello *app, Look k) {
  div(flex, flex_col, gap(3), w(80), p(6), rounded(xl), bg(k.panel)) {
    text("Hello from civue", text_size(px(17)), line_height(px(24)),
         font_weight(semibold));

    counter(app, k);
    dark_switch(app, k);
    volume_slider(app, k);
    name_field(app, k);
  }
}

static void frame(void *ctx, civue_ui *ui) {
  Hello *app = ctx;
  Look k = look_of(app->dark);

  ui->appearance = app->dark ? CIVUE_APPEARANCE_DARK : CIVUE_APPEARANCE_LIGHT;

  div(size(full), p(6), text_size(px(13)), line_height(px(20)),
      text_color(k.text)) {
    card(app, k);
  }
}

static civue_color background(void *ctx) {
  Hello *app = ctx;

  return look_of(app->dark).ground;
}

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) {
  return (civue_app){arg, frame, background};
}

int main() {
  Hello hello = {.volume = 0.5f};

  civue_window_options options = {
    .title = "Hello",
    .width = 368,
    .height = 304,
    .min_width = 368,
    .min_height = 304,
  };

  civue_window_run(&options, make, &hello);
  free(hello.name);

  return 0;
}
