#include <stdio.h>

#include <civue/dsl.h>

typedef struct {
  civue_color ground, panel, raised, text, faint, accent, line, thumb;
} Look;

typedef struct {
  bool dark;
  Look look;
  civue_scrollbar_look bars;
} App;

static Look look_of(bool dark) {
  if (dark) {
    return (Look){rgb(0x1a1a1a),    rgb(0x242424),    rgb(0x2e2e2e),
                  rgb(0xf2f2f2),    rgba(0xffffff8c), rgb(0x0c8ce9),
                  rgba(0xffffff1f), rgba(0xffffff66)};
  }

  return (Look){rgb(0xf3f3f3),    rgb(0xffffff),    rgb(0xf7f7f7),
                rgb(0x1e1e1e),    rgba(0x0000008c), rgb(0x0d99ff),
                rgba(0x0000001a), rgba(0x00000059)};
}

// the bars civue has none of until the app draws them: a thumb that shows
// while the list scrolls or the pointer is over it.
static void paint_bars([[maybe_unused]] civue_ui *ui, void *ctx,
                       const civue_scroll_state *s) {
  const App *app = ctx;
  const civue_scrollbar *bars[] = {&s->vertical, &s->horizontal};

  for (int i = 0; i < 2; i++) {
    if (!bars[i]->shown) {
      continue;
    }

    float strength = bars[i]->lit ? 1 : .6f;
    civue_color c = civue_alpha(app->look.thumb, s->show * strength);
    fill(bars[i]->thumb, c, 3);
  }
}

static void header(App *app) {
  Look k = app->look;

  // the title and its caption sit on one baseline, whatever their sizes.
  div(flex, items_baseline, justify_between) {
    div(flex, items_baseline, gap(3)) {
      // iconify's c logo keeps its own colors, the others take the text's.
      icon(logos, c, size(6), self_center);
      text("Layout", text_size(2xl), line_height(px(32)), font_weight(bold));
      text("grid, baselines, collapsing margins and scrolling",
           text_color(k.faint));
    }

    div(key(id("theme")), flex, items_center, gap(2), px(3), py(1), rounded(md),
        border(1), border_color(k.line), hover(bg(k.raised)), cursor_pointer) {
      if (button().activated) {
        app->dark = !app->dark;
      }
      if (app->dark) {
        icon(lucide, sun);
      }
      else {
        icon(lucide, moon);
      }
      text(app->dark ? "Light" : "Dark");
    }
  }
}

static void tile(Look k, const char *name, civue_color c) {
  div(flex, items_center, justify_center, min_h(16), p(3), rounded(lg),
      bg(civue_alpha(c, .18f)), border(1), border_color(civue_alpha(c, .5f)),
      text_center) {
    text(name, text_color(k.text));
  }
}

static void grid_card(App *app) {
  Look k = app->look;
  civue_color blue = rgb(0x0d99ff), green = rgb(0x14ae5c);
  civue_color orange = rgb(0xffa629), purple = rgb(0x9747ff);

  div(flex, flex_col, gap(3), w(1_2), p(4), rounded(xl), bg(k.panel),
      shadow(sm)) {
    div(flex, items_center, gap(2)) {
      icon(lucide, layout_grid, text_color(k.faint));
      text("Grid", font_weight(semibold));
    }

    div(grid, grid_cols(3), gap(2)) {
      div(col_span(2)) {
        tile(k, "two columns", blue);
      }
      div(row_span(2), flex) {
        div(flex_1) {
          tile(k,
               "two rows, its text wrapping and "
               "centered line by line",
               purple);
        }
      }
      tile(k, "one", green);
      tile(k, "two", orange);
      div(col_span_full) {
        tile(k, "the whole row", green);
      }
      div(col_start(3)) {
        tile(k, "third column", blue);
      }
    }
  }
}

static void list_card(App *app) {
  Look k = app->look;

  div(flex, flex_col, flex_1, min_w(0), rounded(xl), bg(k.panel), shadow(sm),
      overflow_hidden) {
    div(px(4), pt(4), pb(2)) {
      text("Notes", font_weight(semibold));
      text("Twelve point margins on every note: between two they "
           "collapse to twelve, as in css.",
           text_color(k.faint));
    }

    // a block, its notes' vertical margins collapsing.
    div(flex_1, overflow_y_scroll, scrollbars(&app->bars), pb(2)) {
      for (int i = 0; i < 24; i++) {
        char title[32];
        snprintf(title, sizeof title, "Note %d", i + 1);

        div(key(id_at(id("note"), i)), mx(4), my(3), p(3), rounded(md),
            bg(k.raised), border_l(3),
            border_color(i % 3 ? k.line : k.accent)) {
          div(flex, items_center, gap(2)) {
            icon(lucide, sticky_note, text_color(k.faint));
            text(title, font_weight(medium));
          }
          text("A left border wider than the others, its inner "
               "corners rounded by what it leaves.",
               text_color(k.faint));
        }
      }
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

    div(flex, flex_1, min_h(0), gap(4), items_start) {
      grid_card(app);

      div(flex, self_stretch, flex_1, min_w(0)) {
        list_card(app);
      }
    }
  }
}

static civue_color background(void *ctx) {
  return look_of(((App *)ctx)->dark).ground;
}

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) {
  return (civue_app){arg, frame, background};
}

int main() {
  App app = {};
  app.bars = (civue_scrollbar_look){.size = 10,
                                    .inset = 2,
                                    .min_thumb = 24,
                                    .linger = 1,
                                    .fade = .2f,
                                    .ctx = &app,
                                    .paint = paint_bars};

  civue_window_options o = {.title = "Layout",
                            .width = 820,
                            .height = 560,
                            .min_width = 640,
                            .min_height = 420};
  civue_window_run(&o, make, &app);
  return 0;
}
