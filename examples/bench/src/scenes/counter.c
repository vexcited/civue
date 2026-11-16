#include "../scenes.h"

#include <stdio.h>

#include <civue/dsl.h>

static bool step(const char *name, const char *label, civue_length width) {
  bool clicked = false;

  div(key(id(name)), flex, items_center, justify_center, h(7), px(3),
      style(.min_w = width), rounded(md), bg(rgb(0xe6e6e6)),
      hover(bg(rgb(0xdadada))), active(bg(rgb(0xcccccc)))) {
    clicked = button().activated;
    text(label);
  }

  return clicked;
}

void counter_scene(Bench *b, civue_ui *ui) {
  // the first frame is on screen once a second one starts, as the other
  // frameworks' apps tell it.
  if (!ui->rerun) {
    b->frame++;
  }

  if (b->frame == 1) {
    civue_request_repaint(ui);
  }
  else if (b->frame == 2 && !ui->rerun) {
    puts("ready");
    fflush(stdout);
  }

  char number[16];
  snprintf(number, sizeof number, "%d", b->n);

  div(size(full), flex, flex_col, items_center, justify_center, gap(4),
      text_size(px(13)), text_color(rgb(0x1e1e1e))) {
    text(number, text_size(px(56)), font_weight(bold));

    div(flex, gap(2)) {
      if (step("decrement", "−", civue_px(44))) {
        b->n--;
      }

      if (step("reset", "Reset", civue_px(0))) {
        b->n = 0;
      }

      if (step("increment", "+", civue_px(44))) {
        b->n++;
      }
    }
  }
}
