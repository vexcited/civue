#include "recorder.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

// a count from the environment, at least `least`, or the fallback when it is
// not set. anything else ends the run before it measures.
static int env(const char *name, int fallback, int least) {
  const char *v = getenv(name);

  if (!v || !*v) {
    return fallback;
  }

  char *end;
  errno = 0;
  long n = strtol(v, &end, 10);

  if (errno || *end || n < least || n > INT_MAX) {
    fprintf(stderr, "bench: %s is \"%s\", a count of %d or more is needed\n",
            name, v, least);
    exit(2);
  }

  return (int)n;
}

Recorder recorder_new() {
  Recorder r = {
    .warmup = env("BENCH_WARMUP", 120, 0),
    .frames = env("BENCH_FRAMES", 600, 1),
  };

  // frames are counted in an int.
  if (r.warmup > INT_MAX - r.frames) {
    fprintf(stderr, "bench: %d frames after %d are too many to count\n",
            r.frames, r.warmup);
    exit(2);
  }

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
