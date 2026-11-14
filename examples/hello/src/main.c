#include <stdio.h>
#include <stdlib.h>

#include <civue/dsl.h>

typedef struct {
  int count;
  bool dark;
  float volume;
  char *name;
} Hello;

// the example's own look: civue paints nothing of its own.
typedef struct {
  civue_color ground, panel, text, faint, accent, track, line;
} Look;

static Look look_of(bool dark) {
  if (dark) {
    return (Look){rgb(0x1e1e1e),    rgb(0x2c2c2c), rgb(0xffffff),
                  rgba(0xffffff80), rgb(0x0c8ce9), rgb(0x444444),
                  rgba(0xffffff33)};
  }

  return (Look){rgb(0xf5f5f5), rgb(0xffffff), rgb(0x1e1e1e),   rgba(0x00000080),
                rgb(0x0d99ff), rgb(0xe6e6e6), rgba(0x0000001a)};
}

static void add_button(Hello *app, Look k) {
  div(key(id("add")), flex, items_center, justify_center, gap(1p5), w(24), h(7),
      rounded(md), bg(k.accent), active(bg(civue_alpha(k.accent, .8f))),
      text_color(CIVUE_WHITE), cursor_pointer) {
    civue_button_state add = button();
    if (add.activated) {
      app->count++;
    }
    icon(lucide, plus);
    text("Click me");

    if (tooltip(id("add"), &add.response)) {
      civue_id tip = id("add-tip");
      civue_vec2 size = last_size(tip);
      civue_tooltip_place at =
        tooltip_at(add.response.rect, size, CIVUE_TOOLTIP_BELOW, 6);
      overlay() {
        div(key(tip), left(px(at.rect.x0)), top(px(at.rect.y0)), px(2), py(1),
            rounded(md), bg(rgb(0x1e1e1e)), text_color(CIVUE_WHITE),
            whitespace_nowrap) {
          text("Counts the clicks");
        }
      }
    }
  }
}

static void dark_switch(Hello *app, Look k) {
  div(key(id("dark")), flex, items_center, gap(2), self_start, cursor_pointer) {
    civue_toggle_state dark = toggle(app->dark);

    div(flex, items_center, w(7), h(4), p(0p5), rounded(full),
        bg(app->dark ? k.accent : k.track),
        style(.justify = app->dark ? CIVUE_PLACE_END : CIVUE_PLACE_START)) {
      div(size(3), rounded(full), bg(CIVUE_WHITE)) {}
    }

    text("Dark theme");
    app->dark = dark.on;
  }
}

static void volume_slider(Hello *app, Look k) {
  div(key(id("volume")), w(px(200)), h(6), px(2)) {
    civue_slider_state volume = slider(app->volume, 0, 1, 0, 0.01);
    if (volume.changed) {
      app->volume = (float)volume.value;
    }

    // the knob follows the pointer, painted rather than laid out.
    civue_rect track = content();
    double mid = (track.y0 + track.y1) / 2;
    civue_rect bar = civue_rect_from(track.x0, mid - 2, track.x1, mid + 2);
    civue_rect knob =
      civue_rect_from(volume.x - 7, mid - 7, volume.x + 7, mid + 7);
    fill(bar, k.track, 2);
    fill(civue_rect_from(bar.x0, bar.y0, volume.x, bar.y1), k.accent, 2);
    fill(knob, CIVUE_WHITE, 7);
    stroke(knob, k.line, 1, 7);
  }
}

static void name_field(Hello *app, Look k) {
  civue_field_box box = {k.ground, k.accent, 1, 6, k.text};
  civue_field_look look = {.pad = 8,
                           .editing = box,
                           .idle = box,
                           .hovered = box,
                           .placeholder = k.faint,
                           .selection = civue_alpha(k.accent, .3f)};
  look.idle.border = look.hovered.border = k.track;

  div(key(id("name")), w(px(200)), h(7)) {
    text_field(&app->name, "Your name", look);
  }

  if (app->name) {
    char hi[96];
    snprintf(hi, sizeof hi, "Hi %s.", app->name);
    text(hi);
  }
}

static void frame(void *ctx, civue_ui *ui) {
  Hello *app = ctx;
  Look k = look_of(app->dark);
  ui->appearance = app->dark ? CIVUE_APPEARANCE_DARK : CIVUE_APPEARANCE_LIGHT;

  div(size(full), p(6), text_size(px(13)), line_height(px(20)),
      text_color(k.text)) {
    div(flex, flex_col, gap(3), w(80), p(6), rounded(xl), bg(k.panel)) {
      text("Hello from civue", text_size(px(17)), line_height(px(24)),
           font_weight(semibold));

      div(flex, items_center, gap(3)) {
        add_button(app, k);

        char counted[32];
        snprintf(counted, sizeof counted, "Clicked %d times", app->count);
        text(counted, text_color(k.faint));
      }

      dark_switch(app, k);
      volume_slider(app, k);
      name_field(app, k);
    }
  }
}

static civue_color background(void *ctx) {
  return look_of(((Hello *)ctx)->dark).ground;
}

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) {
  return (civue_app){arg, frame, background};
}

int main() {
  Hello hello = {.volume = 0.5f};
  civue_window_options o = {.title = "Hello",
                            .width = 368,
                            .height = 304,
                            .min_width = 368,
                            .min_height = 304};
  civue_window_run(&o, make, &hello);
  free(hello.name);
  return 0;
}
