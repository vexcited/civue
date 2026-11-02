#ifndef CIVUE_GPU_H
#define CIVUE_GPU_H

#include <civue/base.h>

#ifdef __cplusplus
extern "C" {
#endif

// the gpu civue draws with, made on first use and shared by the whole process: id<MTLDevice> on macos, a
// const civue_vk_device * of <civue/vulkan.h> on linux. nullptr when there is none fit to draw.
void *civue_gpu_device(void);
// the app's name the drivers are told, before the device is first made. vulkan drivers tune for some apps.
void civue_gpu_set_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
