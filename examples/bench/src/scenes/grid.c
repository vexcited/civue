#include "../scenes.h"

#include <stdio.h>

#include <civue/dsl.h>

#include "../words.h"

constexpr int COLS = 20;
constexpr int LINES = 30;

static civue_color cell_color(double v) {
  if (v < 333) {
    return rgb(0xf6c9c4);
  }

  if (v < 666) {
    return rgb(0xf4e8b8);
  }

  return rgb(0xc6e9c8);
}

static void cell(double v) {
  char value[16];
  snprintf(value, sizeof value, "%.2f", v);

  div(flex, items_center, justify_center, w(px(40)), h(px(20)),
      bg(cell_color(v))) {
    text(value, whitespace_nowrap);
  }
}

void grid_scene(Bench *b, civue_ui *ui) {
  if (recorder_frame(&b->recorder, ui)) {
    b->frame++;
  }

  div(size(full), flex, flex_col, text_size(px(11)), line_height(px(14)),
      text_color(rgb(0x101010))) {
    for (int line = 0; line < LINES; line++) {
      div(flex) {
        for (int col = 0; col < COLS; col++) {
          cell(cell_value(line * COLS + col, b->frame));
        }
      }
    }
  }
}
