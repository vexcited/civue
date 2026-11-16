#include "recorder.h"

#include <stdio.h>
#include <stdlib.h>

static int env(const char *name, int fallback) {
  const char *v = getenv(name);

  return v && *v ? atoi(v) : fallback;
}

Recorder recorder_new() {
  Recorder r = {
    .warmup = env("BENCH_WARMUP", 120),
    .frames = env("BENCH_FRAMES", 600),
  };

  r.intervals = calloc((size_t)r.frames + 1, sizeof *r.intervals);
  r.last = r.started = civue_now();

  return r;
}

static int by_length(const void *a, const void *b) {
  double x = *(const double *)a, y = *(const double *)b;

  return (x > y) - (x < y);
}

static long us(double seconds) {
  return (long)(seconds * 1e6);
}

static void done(Recorder *r, double now) {
  qsort(r->intervals, (size_t)r->count, sizeof *r->intervals, by_length);

  printf("done {\"frames\":%d,\"wallUs\":%ld,\"p50Us\":%ld,\"p99Us\":%ld,"
         "\"maxUs\":%ld}\n",
         r->frames, us(now - r->started), us(r->intervals[r->frames / 2]),
         us(r->intervals[r->frames * 99 / 100]),
         us(r->intervals[r->frames - 1]));
  fflush(stdout);
}

static bool tick(Recorder *r) {
  double now = civue_now();
  r->seen++;

  if (r->seen == r->warmup) {
    puts("start");
    fflush(stdout);
    r->started = now;
  }
  else if (r->seen > r->warmup && r->seen <= r->warmup + r->frames) {
    r->intervals[r->count++] = now - r->last;

    if (r->count == r->frames) {
      done(r, now);
    }
  }

  r->last = now;

  return r->seen <= r->warmup + r->frames;
}

bool recorder_frame(Recorder *r, civue_ui *ui) {
  bool started = !ui->rerun;

  if (started) {
    r->more = tick(r);
  }

  if (r->more) {
    civue_request_repaint(ui);
  }

  return started && r->more;
}
