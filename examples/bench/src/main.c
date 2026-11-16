#include <stdio.h>
#include <string.h>

#include <civue/dsl.h>

#include "scenes.h"

typedef void (*Scene)(Bench *b, civue_ui *ui);

static Scene scene;

static Scene scene_of(const char *name) {
  if (!strcmp(name, "counter")) {
    return counter_scene;
  }

  if (!strcmp(name, "list")) {
    return list_scene;
  }

  if (!strcmp(name, "grid")) {
    return grid_scene;
  }

  if (!strcmp(name, "document")) {
    return document_scene;
  }

  return nullptr;
}

static void frame(void *ctx, civue_ui *ui) {
  ui->appearance = CIVUE_APPEARANCE_LIGHT;
  scene(ctx, ui);
}

static civue_color background([[maybe_unused]] void *ctx) {
  return CIVUE_WHITE;
}

static civue_app make([[maybe_unused]] civue_ui *ui, void *arg) {
  return (civue_app){arg, frame, background};
}

int main(int argc, char **argv) {
  const char *name = argc > 1 && argv[1][0] != '-' ? argv[1] : "counter";
  scene = scene_of(name);

  if (!scene) {
    fprintf(stderr, "no scene \"%s\"\n", name);
    return 1;
  }

  bool small = scene == counter_scene;
  Bench bench = {.recorder = recorder_new()};

  civue_window_options options = {
    .title = "Benchmark",
    .width = small ? 360 : 800,
    .height = small ? 260 : 600,
#ifdef __APPLE__
    .install_menus = install_menus,
#endif
  };

  civue_window_run(&options, make, &bench);

  return 0;
}
