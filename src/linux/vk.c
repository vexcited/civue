#include "../internal.h"

#include <civue/gpu.h>
#include <civue/vulkan.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>

// memory blocks carved into allocations, a request past half of one gets
// memory of its own.
constexpr VkDeviceSize BLOCK = 64ull << 20;

typedef struct {
  VkDeviceSize offset, size;
} Range;

typedef struct {
  VkDeviceMemory memory;
  VkDeviceSize size, used;
  u8 *map;
  u32 type;
  bool linear;
  Vec(Range) free;
} Block;

typedef struct {
  void (*fn)(void *);
  void *arg;
  // what each lane had submitted when it was asked.
  u64 values[CIVUE_VK_LANES];
} Deferred;

// a queue, its lock and the timeline counting what it was given.
typedef struct {
  VkQueue queue;
  pthread_mutex_t lock;
  VkSemaphore timeline;
  _Atomic u64 submitted;
} Lane;

// a timeline value and the lane it counts, in one word: lanes signal
// timelines of their own, values must grow on each.
constexpr u32 LANE_SHIFT = 60;

static struct {
  civue_vk_device vk;
  bool ok;
  civue_strs instance_exts;
  pthread_mutex_t alloc_lock, defer_lock;
  Lane lanes[CIVUE_VK_LANES];
  // the ui lane's work is the render lane's when the queue family has one
  // queue only.
  u32 nlanes;
  // the render lane's value the ui last took textures of, waited for by the
  // ui's submissions so they see what it wrote.
  _Atomic u64 handoff;
  Vec(Block *) blocks;
  Vec(Deferred) deferred;
} V = {
    .lanes = {{.lock = PTHREAD_MUTEX_INITIALIZER}, {.lock = PTHREAD_MUTEX_INITIALIZER}},
    .alloc_lock = PTHREAD_MUTEX_INITIALIZER,
    .defer_lock = PTHREAD_MUTEX_INITIALIZER,
};

// what the drivers are told the app is.
static const char *app_name = "civue";

static Lane *lane_of(civue_vk_lane lane) { return &V.lanes[lane < V.nlanes ? lane : 0]; }

static void check(VkResult r, const char *what) {
  if (r != VK_SUCCESS) die("vulkan: %s failed (%d)", what, r);
}

bool civue_vk_has_instance_extension(const char *name) {
  civue_vk();
  vec_each(&V.instance_exts, e) if (!strcmp(*e, name)) return true;
  return false;
}

static bool has_extension(VkPhysicalDevice p, const char *name) {
  u32 n = 0;
  vkEnumerateDeviceExtensionProperties(p, nullptr, &n, nullptr);
  VkExtensionProperties *e = xcalloc(n ? n : 1, sizeof *e);
  vkEnumerateDeviceExtensionProperties(p, nullptr, &n, e);
  bool found = false;
  for (u32 i = 0; i < n && !found; i++) found = !strcmp(e[i].extensionName, name);
  free(e);
  return found;
}

static bool make_instance(void) {
  u32 n = 0;
  vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
  VkExtensionProperties *e = xcalloc(n ? n : 1, sizeof *e);
  vkEnumerateInstanceExtensionProperties(nullptr, &n, e);
  // the window's wayland surface, when the loader has one.
  static const char *const wanted[] = {
      VK_KHR_SURFACE_EXTENSION_NAME,
      "VK_KHR_wayland_surface",
      "VK_EXT_swapchain_colorspace",
      "VK_KHR_get_surface_capabilities2",
  };
  const char *names[countof(wanted)];
  u32 count = 0;
  for (u32 w = 0; w < countof(wanted); w++)
    for (u32 i = 0; i < n; i++)
      if (!strcmp(e[i].extensionName, wanted[w])) {
        names[count++] = wanted[w];
        vec_push(&V.instance_exts, xstrdup(wanted[w]));
        break;
      }
  free(e);
  VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = app_name,
      .pEngineName = "civue",
      .apiVersion = VK_API_VERSION_1_3,
  };
  VkInstanceCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
      .enabledExtensionCount = count,
      .ppEnabledExtensionNames = names,
  };
  VkResult r = vkCreateInstance(&info, nullptr, &V.vk.instance);
  if (r != VK_SUCCESS) LOG("vulkan: no instance (%d)", r);
  return r == VK_SUCCESS;
}

// how much a device is preferred, -1 when it can't draw.
static i32 score(VkPhysicalDevice p, u32 *family) {
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(p, &props);
  if (props.apiVersion < VK_API_VERSION_1_3) return -1;
  if (!has_extension(p, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) return -1;
  VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
  VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                          .pNext = &f13};
  VkPhysicalDeviceFeatures2 f = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f12};
  vkGetPhysicalDeviceFeatures2(p, &f);
  if (!f13.dynamicRendering || !f13.synchronization2 || !f12.timelineSemaphore) return -1;
  u32 n = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(p, &n, nullptr);
  VkQueueFamilyProperties *q = xcalloc(n ? n : 1, sizeof *q);
  vkGetPhysicalDeviceQueueFamilyProperties(p, &n, q);
  i32 found = -1;
  for (u32 i = 0; i < n && found < 0; i++)
    if (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) found = (i32)i;
  free(q);
  if (found < 0) return -1;
  *family = (u32)found;
  switch (props.deviceType) {
  case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 4;
  case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 3;
  case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
  case VK_PHYSICAL_DEVICE_TYPE_CPU: return 1;
  default: return 0;
  }
}

static bool pick_device(void) {
  u32 n = 0;
  vkEnumeratePhysicalDevices(V.vk.instance, &n, nullptr);
  VkPhysicalDevice *list = xcalloc(n ? n : 1, sizeof *list);
  vkEnumeratePhysicalDevices(V.vk.instance, &n, list);
  i32 best = -1;
  for (u32 i = 0; i < n; i++) {
    u32 family;
    i32 s = score(list[i], &family);
    if (s < 0) continue;
    if (s <= best) continue;
    best = s;
    V.vk.physical = list[i];
    V.vk.family = family;
    vkGetPhysicalDeviceProperties(list[i], &V.vk.props);
  }
  free(list);
  if (best < 0) LOG("vulkan: no device with vulkan 1.3 and push descriptors");
  return best >= 0;
}

static bool stencil_attachment(VkFormat f) {
  VkFormatProperties p;
  vkGetPhysicalDeviceFormatProperties(V.vk.physical, f, &p);
  return p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
}

static bool make_device(void) {
  civue_vk_device *v = &V.vk;
  vkGetPhysicalDeviceMemoryProperties(v->physical, &v->memory);
  for (u32 i = 0; i < v->memory.memoryTypeCount; i++)
    if (v->memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) v->lazy = true;
  static const VkFormat stencils[] = {VK_FORMAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT};
  for (u32 i = 0; i < countof(stencils) && !v->stencil; i++)
    if (stencil_attachment(stencils[i])) v->stencil = stencils[i];
  if (!v->stencil) {
    LOG("vulkan: no stencil format");
    return false;
  }
  v->stencil_aspects = v->stencil == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
                                                       : VK_IMAGE_ASPECT_STENCIL_BIT | VK_IMAGE_ASPECT_DEPTH_BIT;
  VkPhysicalDeviceFeatures base;
  vkGetPhysicalDeviceFeatures(v->physical, &base);
  v->anisotropy = base.samplerAnisotropy;
  v->swapchain = has_extension(v->physical, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  const char *exts[2] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
  u32 nexts = 1;
  if (v->swapchain) exts[nexts++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
  VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
                                          .dynamicRendering = true,
                                          .synchronization2 = true};
  VkPhysicalDeviceVulkan12Features f12 = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13, .timelineSemaphore = true};
  VkPhysicalDeviceFeatures2 features = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                                        .pNext = &f12,
                                        .features = {.samplerAnisotropy = v->anisotropy}};
  // the ui gets a queue of its own when the family has two, so its frames
  // never wait behind a renderer's or an export's.
  u32 nfamilies = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(v->physical, &nfamilies, nullptr);
  VkQueueFamilyProperties *families = xcalloc(nfamilies ? nfamilies : 1, sizeof *families);
  vkGetPhysicalDeviceQueueFamilyProperties(v->physical, &nfamilies, families);
  V.nlanes = MIN(families[v->family].queueCount, (u32)CIVUE_VK_LANES);
  free(families);
  f32 priority[CIVUE_VK_LANES] = {1, 1};
  VkDeviceQueueCreateInfo queue = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = v->family,
                                   .queueCount = V.nlanes,
                                   .pQueuePriorities = priority};
  VkDeviceCreateInfo info = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .pNext = &features,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &queue,
                             .enabledExtensionCount = nexts,
                             .ppEnabledExtensionNames = exts};
  VkResult r = vkCreateDevice(v->physical, &info, nullptr, &v->device);
  if (r != VK_SUCCESS) {
    LOG("vulkan: no device (%d)", r);
    return false;
  }
  for (u32 i = 0; i < V.nlanes; i++) vkGetDeviceQueue(v->device, v->family, i, &V.lanes[i].queue);
  v->queue = V.lanes[0].queue;
  v->push_descriptor = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(v->device, "vkCmdPushDescriptorSetKHR");
  VkSemaphoreTypeCreateInfo type = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
                                    .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
  VkSemaphoreCreateInfo sem = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &type};
  for (u32 i = 0; i < V.nlanes; i++)
    check(vkCreateSemaphore(v->device, &sem, nullptr, &V.lanes[i].timeline), "timeline");
  VkPipelineCacheCreateInfo cache = {.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  vkCreatePipelineCache(v->device, &cache, nullptr, &v->cache);
  return true;
}

static void make(void) { V.ok = make_instance() && pick_device() && make_device(); }

const civue_vk_device *civue_vk(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, make);
  return V.ok ? &V.vk : nullptr;
}

void *civue_gpu_device(void) { return (void *)civue_vk(); }

static bool named;

void civue_gpu_set_name(const char *name) {
  app_name = name;
  named = true;
}

void civue__gpu_title(const char *title) {
  if (!named) app_name = title;
}

// ---- memory ----

static i32 memory_type(u32 bits, VkMemoryPropertyFlags want) {
  const VkPhysicalDeviceMemoryProperties *m = &V.vk.memory;
  for (u32 i = 0; i < m->memoryTypeCount; i++)
    if (bits & 1u << i && (m->memoryTypes[i].propertyFlags & want) == want) return (i32)i;
  return -1;
}

static VkDeviceSize align_up(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

static bool block_take(Block *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *at) {
  for (u32 i = 0; i < b->free.len; i++) {
    Range r = b->free.v[i];
    VkDeviceSize start = align_up(r.offset, align);
    if (start + size > r.offset + r.size) continue;
    VkDeviceSize end = r.offset + r.size;
    vec_remove(&b->free, i);
    // the parts before and after stay free, in order.
    if (end > start + size) vec_insert(&b->free, i, ((Range){start + size, end - start - size}));
    if (start > r.offset) vec_insert(&b->free, i, ((Range){r.offset, start - r.offset}));
    b->used += size;
    *at = start;
    return true;
  }
  return false;
}

static void block_give(Block *b, VkDeviceSize offset, VkDeviceSize size) {
  u32 i = 0;
  while (i < b->free.len && b->free.v[i].offset < offset) i++;
  vec_insert(&b->free, i, ((Range){offset, size}));
  // joined with its neighbors.
  if (i + 1 < b->free.len && b->free.v[i].offset + b->free.v[i].size == b->free.v[i + 1].offset) {
    b->free.v[i].size += b->free.v[i + 1].size;
    vec_remove(&b->free, i + 1);
  }
  if (i > 0 && b->free.v[i - 1].offset + b->free.v[i - 1].size == b->free.v[i].offset) {
    b->free.v[i - 1].size += b->free.v[i].size;
    vec_remove(&b->free, i);
  }
  b->used -= size;
}

static bool host_visible(u32 type) {
  return V.vk.memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
}

static bool allocate(VkDeviceSize size, u32 type, VkDeviceMemory *memory, u8 **map) {
  VkMemoryAllocateInfo info = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = size, .memoryTypeIndex = type};
  if (vkAllocateMemory(V.vk.device, &info, nullptr, memory) != VK_SUCCESS) return false;
  *map = nullptr;
  if (host_visible(type) && vkMapMemory(V.vk.device, *memory, 0, VK_WHOLE_SIZE, 0, (void **)map) != VK_SUCCESS) {
    vkFreeMemory(V.vk.device, *memory, nullptr);
    return false;
  }
  return true;
}

bool civue_vk_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer,
                    bool linear, civue_vk_mem *out) {
  i32 type = memory_type(req->memoryTypeBits, want | prefer);
  if (type < 0) type = memory_type(req->memoryTypeBits, want);
  if (type < 0) return false;
  *out = (civue_vk_mem){.size = req->size, .type = (u32)type, .block = -1};
  if (req->size > BLOCK / 2) return allocate(req->size, (u32)type, &out->memory, &out->map);
  pthread_mutex_lock(&V.alloc_lock);
  bool ok = false;
  for (u32 i = 0; i < V.blocks.len && !ok; i++) {
    Block *b = V.blocks.v[i];
    if (!b || b->type != (u32)type || b->linear != linear) continue;
    if (block_take(b, req->size, req->alignment, &out->offset)) {
      out->block = (i32)i;
      ok = true;
    }
  }
  if (!ok) {
    Block *b = xcalloc(1, sizeof *b);
    b->size = BLOCK;
    b->type = (u32)type;
    b->linear = linear;
    if (allocate(BLOCK, (u32)type, &b->memory, &b->map)) {
      vec_push(&b->free, ((Range){0, BLOCK}));
      u32 slot = 0;
      while (slot < V.blocks.len && V.blocks.v[slot]) slot++;
      if (slot == V.blocks.len) vec_push(&V.blocks, nullptr);
      V.blocks.v[slot] = b;
      ok = block_take(b, req->size, req->alignment, &out->offset);
      out->block = (i32)slot;
    } else
      free(b);
  }
  if (ok) {
    Block *b = V.blocks.v[out->block];
    out->memory = b->memory;
    out->map = b->map ? b->map + out->offset : nullptr;
  }
  pthread_mutex_unlock(&V.alloc_lock);
  return ok;
}

void civue_vk_free(civue_vk_mem *m) {
  if (!m->memory) return;
  if (m->block < 0) {
    vkFreeMemory(V.vk.device, m->memory, nullptr);
    *m = (civue_vk_mem){};
    return;
  }
  pthread_mutex_lock(&V.alloc_lock);
  Block *b = V.blocks.v[m->block];
  block_give(b, m->offset, m->size);
  // an empty block goes back to the driver.
  if (!b->used) {
    vkFreeMemory(V.vk.device, b->memory, nullptr);
    vec_free(&b->free);
    free(b);
    V.blocks.v[m->block] = nullptr;
  }
  pthread_mutex_unlock(&V.alloc_lock);
  *m = (civue_vk_mem){};
}

// ---- submissions ----

u64 civue_vk_submit(civue_vk_lane lane, VkCommandBuffer cb, VkSemaphore wait, VkPipelineStageFlags2 stage,
                    VkSemaphore signal) {
  u32 index = lane < V.nlanes ? lane : 0;
  Lane *l = &V.lanes[index];
  pthread_mutex_lock(&l->lock);
  u64 value = atomic_load(&l->submitted) + 1;
  VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
  VkSemaphoreSubmitInfo waits[2];
  u32 nwaits = 0;
  if (wait)
    waits[nwaits++] = (VkSemaphoreSubmitInfo){
        .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = wait, .stageMask = stage};
  // what a renderer wrote into textures the ui took is visible to it.
  u64 handoff = atomic_load(&V.handoff);
  if (index && handoff)
    waits[nwaits++] = (VkSemaphoreSubmitInfo){.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
                                              .semaphore = V.lanes[0].timeline,
                                              .value = handoff,
                                              .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
  VkSemaphoreSubmitInfo signals[2] = {
      {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
       .semaphore = l->timeline,
       .value = value,
       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
      {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
       .semaphore = signal,
       .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT},
  };
  VkSubmitInfo2 info = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .waitSemaphoreInfoCount = nwaits,
                        .pWaitSemaphoreInfos = waits,
                        .commandBufferInfoCount = cb ? 1 : 0,
                        .pCommandBufferInfos = &cbi,
                        .signalSemaphoreInfoCount = signal ? 2 : 1,
                        .pSignalSemaphoreInfos = signals};
  VkResult r = vkQueueSubmit2(l->queue, 1, &info, VK_NULL_HANDLE);
  if (r == VK_ERROR_DEVICE_LOST) LOG("GPU device lost");
  check(r, "queue submit");
  atomic_store(&l->submitted, value);
  pthread_mutex_unlock(&l->lock);
  civue_vk_collect();
  return (u64)index << LANE_SHIFT | value;
}

VkResult civue_vk_present(const VkPresentInfoKHR *info) {
  // the ui lane's lock alone: a present that blocks holds up the ui, not
  // the renderers, unless the two share the one queue.
  Lane *l = lane_of(CIVUE_VK_LANE_UI);
  pthread_mutex_lock(&l->lock);
  VkResult r = vkQueuePresentKHR(l->queue, info);
  pthread_mutex_unlock(&l->lock);
  return r;
}

void civue_vk_handoff(u64 value) {
  u64 lane = value >> LANE_SHIFT, v = value & ((1ull << LANE_SHIFT) - 1);
  if (lane) return;
  u64 old = atomic_load(&V.handoff);
  while (v > old && !atomic_compare_exchange_weak(&V.handoff, &old, v)) {}
}

static u64 lane_now(u32 lane) {
  u64 now = 0;
  vkGetSemaphoreCounterValue(V.vk.device, V.lanes[lane].timeline, &now);
  return now;
}

bool civue_vk_done(u64 value) {
  u64 v = value & ((1ull << LANE_SHIFT) - 1);
  if (!v) return true;
  return lane_now((u32)(value >> LANE_SHIFT)) >= v;
}

void civue_vk_wait(u64 value) {
  u64 v = value & ((1ull << LANE_SHIFT) - 1);
  if (!v) return;
  VkSemaphoreWaitInfo info = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                              .semaphoreCount = 1,
                              .pSemaphores = &V.lanes[value >> LANE_SHIFT].timeline,
                              .pValues = &v};
  VkResult r = vkWaitSemaphores(V.vk.device, &info, UINT64_MAX);
  if (r == VK_ERROR_DEVICE_LOST) LOG("GPU device lost");
  check(r, "wait");
  civue_vk_collect();
}

void civue_vk_wait_all(void) {
  for (u32 i = 0; i < V.nlanes; i++) civue_vk_wait((u64)i << LANE_SHIFT | atomic_load(&V.lanes[i].submitted));
}

void civue_vk_defer(void (*fn)(void *), void *arg) {
  Deferred d = {fn, arg};
  for (u32 i = 0; i < V.nlanes; i++) d.values[i] = atomic_load(&V.lanes[i].submitted);
  pthread_mutex_lock(&V.defer_lock);
  vec_push(&V.deferred, d);
  pthread_mutex_unlock(&V.defer_lock);
}

void civue_vk_collect(void) {
  u64 now[CIVUE_VK_LANES] = {};
  for (u32 i = 0; i < V.nlanes; i++) now[i] = lane_now(i);
  Vec(Deferred) ready = {};
  pthread_mutex_lock(&V.defer_lock);
  u32 k = 0;
  vec_each(&V.deferred, d) {
    bool done = true;
    for (u32 i = 0; i < V.nlanes; i++) done &= d->values[i] <= now[i];
    if (done)
      vec_push(&ready, *d);
    else
      V.deferred.v[k++] = *d;
  }
  V.deferred.len = k;
  pthread_mutex_unlock(&V.defer_lock);
  vec_each(&ready, d) d->fn(d->arg);
  vec_free(&ready);
}

// ---- resources ----

bool civue_vk_buf_new(VkDeviceSize size, VkBufferUsageFlags usage, bool host, civue_vk_buf *out) {
  *out = (civue_vk_buf){.size = size};
  VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MAX(size, 16), .usage = usage};
  if (vkCreateBuffer(V.vk.device, &info, nullptr, &out->buffer) != VK_SUCCESS) return false;
  VkMemoryRequirements req;
  vkGetBufferMemoryRequirements(V.vk.device, out->buffer, &req);
  // what the cpu reads back is cached for it.
  VkMemoryPropertyFlags cached = usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0;
  bool ok = host ? civue_vk_alloc(&req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                  cached, true, &out->mem)
                 : civue_vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, true, &out->mem);
  if (ok) ok = vkBindBufferMemory(V.vk.device, out->buffer, out->mem.memory, out->mem.offset) == VK_SUCCESS;
  if (!ok) {
    vkDestroyBuffer(V.vk.device, out->buffer, nullptr);
    civue_vk_free(&out->mem);
    *out = (civue_vk_buf){};
  }
  return ok;
}

static void buf_destroy(void *arg) {
  civue_vk_buf *b = arg;
  vkDestroyBuffer(V.vk.device, b->buffer, nullptr);
  civue_vk_free(&b->mem);
  free(b);
}

void civue_vk_buf_free(civue_vk_buf *b) {
  if (!b->buffer) return;
  civue_vk_buf *copy = xmalloc(sizeof *copy);
  *copy = *b;
  civue_vk_defer(buf_destroy, copy);
  *b = (civue_vk_buf){};
}

civue_vk_tex *civue_vk_tex_new(u32 width, u32 height, VkFormat format, VkImageUsageFlags usage,
                               VkSampleCountFlagBits samples) {
  const civue_vk_device *v = civue_vk();
  bool depth = format == v->stencil;
  bool transient = usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  civue_vk_tex *t = xcalloc(1, sizeof *t);
  t->width = MAX(width, 1u);
  t->height = MAX(height, 1u);
  t->format = format;
  t->samples = samples ? samples : VK_SAMPLE_COUNT_1_BIT;
  t->aspects = depth ? v->stencil_aspects : VK_IMAGE_ASPECT_COLOR_BIT;
  t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
  t->refs = 1;
  VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = format,
                            .extent = {t->width, t->height, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = t->samples,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
  check(vkCreateImage(v->device, &info, nullptr, &t->image), "image");
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(v->device, t->image, &req);
  bool ok = transient && v->lazy && civue_vk_alloc(&req, VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT, 0, false, &t->mem);
  if (!ok) ok = civue_vk_alloc(&req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, &t->mem);
  if (!ok) ok = civue_vk_alloc(&req, 0, 0, false, &t->mem);
  if (!ok) die("vulkan: out of memory for a %ux%u texture", width, height);
  check(vkBindImageMemory(v->device, t->image, t->mem.memory, t->mem.offset), "bind image");
  VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = t->image,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = format,
                                .subresourceRange = {t->aspects, 0, 1, 0, 1}};
  // a stencil read as such, never sampled, views its stencil alone.
  if (depth) view.subresourceRange.aspectMask = v->stencil_aspects;
  check(vkCreateImageView(v->device, &view, nullptr, &t->view), "image view");
  return t;
}

civue_vk_tex *civue_vk_tex_retain(civue_vk_tex *t) {
  if (t) atomic_fetch_add(&t->refs, 1);
  return t;
}

static void tex_destroy(void *arg) {
  civue_vk_tex *t = arg;
  vkDestroyImageView(V.vk.device, t->view, nullptr);
  vkDestroyImage(V.vk.device, t->image, nullptr);
  civue_vk_free(&t->mem);
  free(t);
}

void civue_vk_tex_release(civue_vk_tex *t) {
  if (!t || atomic_fetch_sub(&t->refs, 1) != 1) return;
  civue_vk_defer(tex_destroy, t);
}

void civue_vk_layout(VkCommandBuffer cb, civue_vk_tex *t, VkImageLayout to) {
  if (t->layout != to) civue_vk_layout_sync(cb, t, to);
}

void civue_vk_layout_sync(VkCommandBuffer cb, civue_vk_tex *t, VkImageLayout to) {
  VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                             .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                             .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                             .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                             .oldLayout = t->layout,
                             .newLayout = to,
                             .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                             .image = t->image,
                             .subresourceRange = {t->aspects, 0, 1, 0, 1}};
  VkDependencyInfo dep = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
  vkCmdPipelineBarrier2(cb, &dep);
  t->layout = to;
}

void civue_vk_barrier_all(VkCommandBuffer cb) {
  VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
  VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
  vkCmdPipelineBarrier2(cb, &dep);
}

void civue_vk_readback_barrier(VkCommandBuffer cb) {
  VkMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
                        .dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT,
                        .dstAccessMask = VK_ACCESS_2_HOST_READ_BIT};
  VkDependencyInfo dep = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &b};
  vkCmdPipelineBarrier2(cb, &dep);
}

VkShaderModule civue_vk_shader(const u8 *spirv, usize len) {
  VkShaderModuleCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = len, .pCode = (const u32 *)spirv};
  VkShaderModule m;
  check(vkCreateShaderModule(civue_vk()->device, &info, nullptr, &m), "shader");
  return m;
}

VkSampler civue_vk_sampler(VkFilter filter, VkSamplerAddressMode mode, bool mipmaps, f32 anisotropy) {
  const civue_vk_device *v = civue_vk();
  VkSamplerCreateInfo info = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = filter,
                              .minFilter = filter,
                              .mipmapMode = mipmaps ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = mode,
                              .addressModeV = mode,
                              .addressModeW = mode,
                              .anisotropyEnable = v->anisotropy && anisotropy > 1,
                              .maxAnisotropy = MIN(anisotropy, v->props.limits.maxSamplerAnisotropy),
                              .maxLod = mipmaps ? VK_LOD_CLAMP_NONE : 0.25f};
  VkSampler s;
  check(vkCreateSampler(v->device, &info, nullptr, &s), "sampler");
  return s;
}

// ---- command buffers ----

void civue_vk_cmds_init(civue_vk_cmds *c, civue_vk_lane lane) {
  *c = (civue_vk_cmds){.lane = lane};
  VkCommandPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = civue_vk()->family};
  check(vkCreateCommandPool(civue_vk()->device, &info, nullptr, &c->pool), "command pool");
}

void civue_vk_cmds_free(civue_vk_cmds *c) {
  if (!c->pool) return;
  vec_each(&c->pending, p) civue_vk_wait(p->value);
  vkDestroyCommandPool(civue_vk()->device, c->pool, nullptr);
  vec_free(&c->free);
  vec_free(&c->pending);
  *c = (civue_vk_cmds){};
}

VkCommandBuffer civue_vk_cmds_begin(civue_vk_cmds *c) {
  u32 k = 0;
  vec_each(&c->pending, p) {
    if (civue_vk_done(p->value)) {
      vkResetCommandBuffer(p->cb, 0);
      vec_push(&c->free, p->cb);
    } else
      c->pending.v[k++] = *p;
  }
  c->pending.len = k;
  VkCommandBuffer cb;
  if (c->free.len)
    cb = vec_pop(&c->free);
  else {
    VkCommandBufferAllocateInfo info = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = c->pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1};
    check(vkAllocateCommandBuffers(civue_vk()->device, &info, &cb), "command buffer");
  }
  VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
  check(vkBeginCommandBuffer(cb, &begin), "begin commands");
  // what earlier buffers wrote is seen by this one.
  civue_vk_barrier_all(cb);
  return cb;
}

u64 civue_vk_cmds_submit(civue_vk_cmds *c, VkCommandBuffer cb, VkSemaphore wait, VkPipelineStageFlags2 stage,
                         VkSemaphore signal) {
  check(vkEndCommandBuffer(cb), "end commands");
  u64 value = civue_vk_submit(c->lane, cb, wait, stage, signal);
  vec_push(&c->pending, ((struct civue_vk_pending){cb, value}));
  return value;
}
