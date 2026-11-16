#ifndef BENCH_SCENES_H
#define BENCH_SCENES_H

#include "recorder.h"

typedef struct {
  Recorder recorder;
  // what the scene is at: the counter's number, or the frames advanced.
  int n;
  int frame;
  char *paragraphs[40];
} Bench;

// a number and three buttons, then nothing.
void counter_scene(Bench *b, civue_ui *ui);
// a hundred thousand rows of three texts, scrolling 8 points a frame.
void list_scene(Bench *b, civue_ui *ui);
// 600 cells, each a background and a number changing every frame.
void grid_scene(Bench *b, civue_ui *ui);
// 40 paragraphs wrapped to a width changing every frame.
void document_scene(Bench *b, civue_ui *ui);

void install_menus();

#endif
