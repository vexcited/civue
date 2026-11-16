#include "../components.h"

#include <civue/dsl.h>

// the knob follows the pointer, painted rather than laid out.
static void paint(civue_rect track, double x, Look k) {
  double mid = (track.y0 + track.y1) / 2;

  civue_rect bar = civue_rect_from(track.x0, mid - 2, track.x1, mid + 2);
  civue_rect filled = civue_rect_from(bar.x0, bar.y0, x, bar.y1);
  civue_rect knob = civue_rect_from(x - 7, mid - 7, x + 7, mid + 7);

  fill(bar, k.track, 2);
  fill(filled, k.accent, 2);

  fill(knob, CIVUE_WHITE, 7);
  stroke(knob, k.line, 1, 7);
}

void volume_slider(Hello *app, Look k) {
  div(key(id("volume")), w(px(200)), h(6), px(2)) {
    civue_slider_state volume = slider(app->volume, 0, 1, 0, 0.01);

    if (volume.changed) {
      app->volume = (float)volume.value;
    }

    // where the value is now: volume.x is where the one given was.
    civue_rect track = content();
    double x = track.x0 + volume.value * (track.x1 - track.x0);

    paint(track, x, k);
  }
}
