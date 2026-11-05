#include <civue/dsl.h>

#include <stdio.h>
#include <stdlib.h>

typedef struct {
  int count;
  bool dark;
  float volume;
  char *name;
} Hello;

// the example's own look: civue paints nothing of its own.
typedef struct {
  civue_color ground, panel, text, faint, accent, track, line;
} Look;

static Look look_of(bool dark) {
  if (dark)
    return (Look){rgb(0x1e1e1e), rgb(0x2c2c2c), rgb(0xffffff),   rgba(0xffffff80),
                  rgb(0x0c8ce9), rgb(0x444444), rgba(0xffffff33)};
  return (Look){rgb(0xf5f5f5), rgb(0xffffff), rgb(0x1e1e1e),   rgba(0x00000080),
                rgb(0x0d99ff), rgb(0xe6e6e6), rgba(0x0000001a)};
}

static const civue_text_style BODY = {.size = 13, .weight = 400, .line_height = 20};

static void frame(void *ctx, civue_ui *ui) {
  Hello *h = ctx;
  Look k = look_of(h->dark);
  ui->theme_pref = h->dark ? CIVUE_THEME_DARK_PREF : CIVUE_THEME_LIGHT_PREF;
  civue_rect panel = xywh(24, 24, 320, 248);
  fill(ui, panel, k.panel, 12);
  clipped(ui, panel) {
    label(ui, "Hello from civue", civue_style(17, 600, 24, 0), pt(48, 44), k.text);

    civue_button_state add = button(ui, id("add"), xywh(48, 84, 96, 28));
    fill(ui, add.response.rect, civue_alpha(k.accent, add.held ? .8f : 1), 6);
    civue_text_in(ui, "Click me", BODY, add.response.rect, CIVUE_WHITE, CIVUE_ALIGN_CENTER);
    if (add.activated) h->count++;
    if (tooltip(ui, id("add"), &add.response)) {
      civue_tooltip_place at = civue_tooltip_at(ui, add.response.rect, pt(132, 28), CIVUE_TOOLTIP_BELOW, 6);
      overlay(ui) {
        fill(ui, at.rect, rgb(0x1e1e1e), 6);
        civue_text_in(ui, "Counts the clicks", BODY, at.rect, CIVUE_WHITE, CIVUE_ALIGN_CENTER);
      }
    }
    char counted[32];
    snprintf(counted, sizeof counted, "Clicked %d times", h->count);
    civue_text_in(ui, counted, BODY, xywh(156, 84, 160, 28), k.faint, CIVUE_ALIGN_START);

    civue_toggle_state dark = toggle(ui, id("dark"), xywh(48, 130, 140, 20), h->dark);
    civue_rect track = xywh(48, 132, 28, 16);
    fill(ui, track, dark.on ? k.accent : k.track, 8);
    fill(ui, xywh(dark.on ? 62 : 50, 134, 12, 12), CIVUE_WHITE, 6);
    civue_text_in(ui, "Dark theme", BODY, xywh(84, 130, 104, 20), k.text, CIVUE_ALIGN_START);
    h->dark = dark.on;

    civue_rect bar = xywh(56, 172, 184, 4);
    civue_slider_state volume = slider(ui, id("volume"), xywh(48, 162, 200, 24), bar, h->volume, 0, 1, 0, 0.01);
    fill(ui, bar, k.track, 2);
    fill(ui, civue_rect_from(bar.x0, bar.y0, volume.x, bar.y1), k.accent, 2);
    fill(ui, xywh(volume.x - 7, 167, 14, 14), CIVUE_WHITE, 7);
    civue_stroke(ui, xywh(volume.x - 7, 167, 14, 14), k.line, 1, 7);
    if (volume.changed) h->volume = (float)volume.value;

    civue_field_box box = {k.ground, k.accent, 1, 6, k.text};
    civue_field_look field = {.pad = 8,
                              .editing = box,
                              .idle = box,
                              .hovered = box,
                              .placeholder = k.faint,
                              .selection = civue_alpha(k.accent, .3f)};
    field.idle.border = field.hovered.border = k.track;
    civue_text_field_with(ui, id("name"), xywh(48, 200, 200, 28), &h->name, "Your name", BODY, field);
    if (h->name) {
      char hi[96];
      snprintf(hi, sizeof hi, "Hi %s.", h->name);
      label(ui, hi, BODY, pt(48, 236), k.text);
    }
  }
}

static civue_color background(void *ctx) { return look_of(((Hello *)ctx)->dark).ground; }

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) { return (civue_app){arg, frame, background}; }

int main(void) {
  Hello hello = {.volume = 0.5f};
  civue_window_options o = {.title = "Hello", .width = 368, .height = 296, .min_width = 368, .min_height = 296};
  civue_window_run(&o, make, &hello);
  free(hello.name);
  return 0;
}
