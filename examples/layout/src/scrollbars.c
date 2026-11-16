#include "scrollbars.h"

#include <civue/dsl.h>

static void paint_bar(const civue_scrollbar *bar, civue_color thumb,
                      float show) {
  if (!bar->shown) {
    return;
  }

  float strength = bar->lit ? 1 : .6f;

  fill(bar->thumb, civue_alpha(thumb, show * strength), 3);
}

static void paint([[maybe_unused]] civue_ui *ui, void *ctx,
                  const civue_scroll_state *s) {
  const App *app = ctx;

  paint_bar(&s->vertical, app->look.thumb, s->show);
  paint_bar(&s->horizontal, app->look.thumb, s->show);
}

civue_scrollbar_look scrollbars_of(App *app) {
  return (civue_scrollbar_look){
    .size = 10,
    .inset = 2,
    .min_thumb = 24,
    .linger = 1,
    .fade = .2f,
    .ctx = app,
    .paint = paint,
  };
}
