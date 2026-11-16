#include "../components.h"

#include <stdio.h>

#include <civue/dsl.h>

constexpr int NOTES = 24;

static void heading(Look k) {
  div(px(4), pt(4), pb(2)) {
    text("Notes", font_weight(semibold));
    text("Twelve point margins on every note: between two they collapse to "
         "twelve, as in css.",
         text_color(k.faint));
  }
}

static void note(Look k, int index) {
  char title[32];
  snprintf(title, sizeof title, "Note %d", index + 1);

  civue_color edge = index % 3 ? k.line : k.accent;

  div(key(id_at(id("note"), index)), mx(4), my(3), p(3), rounded(md),
      bg(k.raised), border_l(3), border_color(edge)) {
    div(flex, items_center, gap(2)) {
      icon(lucide, sticky_note, text_color(k.faint));
      text(title, font_weight(medium));
    }

    text("A left border wider than the others, its inner corners rounded by "
         "what it leaves.",
         text_color(k.faint));
  }
}

void list_card(App *app) {
  Look k = app->look;

  div(flex, flex_col, flex_1, min_w(0), rounded(xl), bg(k.panel), shadow(sm),
      overflow_hidden) {
    heading(k);

    // a block, its notes' vertical margins collapsing.
    div(flex_1, overflow_y_scroll, scrollbars(&app->bars), pb(2)) {
      for (int i = 0; i < NOTES; i++) {
        // a note reads the look alone: built again when the theme changes.
        memo(id_at(id("note-memo"), i), app->dark) {
          note(k, i);
        }
      }
    }
  }
}
