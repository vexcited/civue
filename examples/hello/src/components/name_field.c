#include "../components.h"

#include <stdio.h>

#include <civue/dsl.h>

static civue_field_look field_look(Look k) {
  civue_field_box focused = {
    .bg = k.ground,
    .border = k.accent,
    .border_width = 1,
    .radius = 6,
    .text = k.text,
  };

  civue_field_box resting = focused;
  resting.border = k.track;

  return (civue_field_look){
    .pad = 8,
    .editing = focused,
    .idle = resting,
    .hovered = resting,
    .placeholder = k.faint,
    .selection = civue_alpha(k.accent, .3f),
  };
}

static void greeting(const char *name) {
  char hi[96];
  snprintf(hi, sizeof hi, "Hi %s.", name);

  text(hi);
}

void name_field(Hello *app, Look k) {
  div(key(id("name")), w(px(200)), h(7)) {
    text_field(&app->name, "Your name", field_look(k));
  }

  if (app->name) {
    greeting(app->name);
  }
}
