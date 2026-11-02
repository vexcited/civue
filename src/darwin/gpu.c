#include "darwin.h"

#include <civue/gpu.h>

#include <pthread.h>

static id device;

static void make(void) { device = MTLCreateSystemDefaultDevice(); }

void *civue_gpu_device(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, make);
  return device;
}

void civue_gpu_set_name(const char *name) {}
