#include <civue/dsl.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  int count;
  bool dark;
  float volume;
  char *name;
} Hello;

static void frame(void *ctx, civue_ui *ui) {
  Hello *h = ctx;
  ui->theme_pref = h->dark ? CIVUE_THEME_DARK_PREF : CIVUE_THEME_LIGHT_PREF;
  civue_rect panel = xywh(24, 24, 320, 248);
  civue_floating(ui, panel, CIVUE_RADIUS_LARGE, ui->theme.bg, CIVUE_E200);
  clipped(ui, panel) {
    label(ui, "Hello from civue", CIVUE_HEADING_MEDIUM, pt(48, 44), ui->theme.text);

    char counted[32];
    snprintf(counted, sizeof counted, "Clicked %d times", h->count);
    civue_response add = button(ui, id("add"), xywh(48, 84, 96, 28), "Click me", CIVUE_BUTTON_PRIMARY);
    if (add.clicked) h->count++;
    tooltip(ui, id("add"), &add, "Counts the clicks");
    civue_text_in(ui, counted, CIVUE_BODY, xywh(156, 84, 160, 28), ui->theme.text_secondary, CIVUE_ALIGN_START);

    if (checkbox(ui, id("dark"), pt(48, 130), h->dark, "Dark theme")) h->dark = !h->dark;

    civue_slider_change change;
    if (slider(ui, id("volume"), xywh(48, 162, 200, 24), h->volume, 0, 1, 0, 0.01, &change))
      h->volume = (float)change.value;

    text_field(ui, id("name"), xywh(48, 200, 200, 28), &h->name, "Your name", CIVUE_BODY);
    if (h->name) {
      char hi[96];
      snprintf(hi, sizeof hi, "Hi %s.", h->name);
      label(ui, hi, CIVUE_BODY, pt(48, 236), ui->theme.text);
    }
  }
}

static civue_color background(void *ctx) { return ((Hello *)ctx)->dark ? rgb(0x1e1e1e) : rgb(0xf5f5f5); }

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) { return (civue_app){arg, frame, background}; }

int main(void) {
  Hello hello = {.volume = 0.5f};
  civue_window_options o = {.title = "Hello", .width = 368, .height = 296, .min_width = 368, .min_height = 296};
  civue_window_run(&o, make, &hello);
  free(hello.name);
  return 0;
}
