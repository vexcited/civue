#include "../components.h"

#include <civue/dsl.h>

static void tile(Look k, const char *name, civue_color c) {
  civue_color tint = civue_alpha(c, .18f);
  civue_color edge = civue_alpha(c, .5f);

  div(flex, items_center, justify_center, min_h(16), p(3), rounded(lg),
      bg(tint), border(1), border_color(edge), text_center) {
    text(name, text_color(k.text));
  }
}

static void heading(Look k) {
  div(flex, items_center, gap(2)) {
    icon(lucide, layout_grid, text_color(k.faint));
    text("Grid", font_weight(semibold));
  }
}

static void tiles(Look k) {
  civue_color blue = rgb(0x0d99ff);
  civue_color green = rgb(0x14ae5c);
  civue_color orange = rgb(0xffa629);
  civue_color purple = rgb(0x9747ff);

  div(grid, grid_cols(3), gap(2)) {
    div(col_span(2)) {
      tile(k, "two columns", blue);
    }

    div(row_span(2), flex) {
      div(flex_1) {
        tile(k, "two rows, its text wrapping and centered line by line",
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

void grid_card(App *app) {
  Look k = app->look;

  div(flex, flex_col, gap(3), w(1_2), p(4), rounded(xl), bg(k.panel),
      shadow(sm)) {
    heading(k);
    tiles(k);
  }
}
