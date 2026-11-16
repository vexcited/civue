#import <Metal/Metal.h>

#include <civue/gpu.h>

static id<MTLDevice> device;

void *civue_gpu_device() {
  static dispatch_once_t once;

  dispatch_once(&once, ^{ device = MTLCreateSystemDefaultDevice(); });

  return (__bridge void *)device;
}

void civue_gpu_set_name(const char *name) {
}

void civue__gpu_title(const char *title) {
}
