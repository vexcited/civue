#ifndef BENCH_RECORDER_H
#define BENCH_RECORDER_H

#include <civue/civue.h>

// times frames and says on standard output how the run goes, as the runner
// of the comparison reads it.
typedef struct {
  int warmup, frames;
  int seen;
  double last, started;
  double *intervals;
  int count;
  bool more;
} Recorder;

Recorder recorder_new();
// counts a frame, and returns whether the scene advances: once a frame,
// however many passes build it, while the run wants more.
bool recorder_frame(Recorder *r, civue_ui *ui);

#endif
