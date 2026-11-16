#include "../scenes.h"

#include <stdio.h>

#include <civue/dsl.h>

#include "../words.h"

constexpr uint32_t ROWS = 100000;
constexpr float ROW_HEIGHT = 40;

static void row(uint32_t i) {
  char title[32], summary[64], value[16];
  snprintf(title, sizeof title, "Item %u", i);
  words(summary, sizeof summary, i + 1, 6);
  snprintf(value, sizeof value, "%.2f", cell_value((int)i, 0));

  div(size(full), flex, items_center, gap(3), px(3)) {
    div(flex, flex_col, flex_1) {
      text(title, whitespace_nowrap);
      text(summary, text_size(px(11)), text_color(rgb(0x6b6b6b)),
           whitespace_nowrap);
    }

    text(value, whitespace_nowrap);
  }
}

void list_scene(Bench *b, civue_ui *ui) {
  civue_id list = id("list");

  if (recorder_frame(&b->recorder, ui)) {
    b->frame++;
    civue_set_scroll_y(ui, list, (float)b->frame * 8);
  }

  div(key(list), size(full), overflow_y_scroll, text_size(px(13)),
      line_height(px(18)), text_color(rgb(0x1e1e1e))) {
    rows(ROWS, ROW_HEIGHT, i) {
      // a row only reads its index: built once, then put back.
      memo(id_at(list, i), i) {
        row(i);
      }
    }
  }
}
