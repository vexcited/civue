#ifndef CIVUE_VULKAN_H
#define CIVUE_VULKAN_H

#include <civue/gpu.h>

#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* the device. one per process, shared by the app's renderers, the ui and the */
/* window: queues behind locks, timelines counting submissions, memory carved */
/* out of large blocks, and textures that outlive their last use on the gpu.  */
/* linux only.                                                                */
/* ========================================================================== */

typedef struct {
  VkInstance instance;
  VkPhysicalDevice physical;
  VkDevice device;
  VkQueue queue;
  uint32_t family;
  VkPhysicalDeviceProperties props;
  VkPhysicalDeviceMemoryProperties memory;
  // the stencil attachments' format, S8 alone when the device has it.
  VkFormat stencil;
  VkImageAspectFlags stencil_aspects;
  // transient attachments can live in lazily allocated memory.
  bool lazy;
  bool anisotropy;
  bool swapchain;
  PFN_vkCmdPushDescriptorSetKHR push_descriptor;
  VkPipelineCache cache;
} civue_vk_device;

// the device, made on first use, nullptr when there is none fit to draw.
// civue_gpu_device returns it too.
const civue_vk_device *civue_vk();
// the instance extensions a window's surface needs were asked for.
bool civue_vk_has_instance_extension(const char *name);

/* ========================================================================== */
/* memory.                                                                    */
/* ========================================================================== */

typedef struct {
  VkDeviceMemory memory;
  VkDeviceSize offset, size;
  // the cpu address of host visible memory, mapped for good.
  uint8_t *map;
  // -1 for an allocation of its own.
  int32_t block;
  uint32_t type;
} civue_vk_mem;

// memory fit for `req`, of `want` and preferably `prefer` properties.
// `linear` resources, buffers, and images keep to blocks of their own.
bool civue_vk_alloc(const VkMemoryRequirements *req, VkMemoryPropertyFlags want,
                    VkMemoryPropertyFlags prefer, bool linear,
                    civue_vk_mem *out);
void civue_vk_free(civue_vk_mem *m);

/* ========================================================================== */
/* submissions.                                                               */
/* ========================================================================== */

// the queues work goes to: the app's renderers' and the ui's, the same one when
// the device has a single queue to give.
typedef enum : uint8_t {
  CIVUE_VK_LANE_RENDER,
  CIVUE_VK_LANE_UI,
  CIVUE_VK_LANES
} civue_vk_lane;

// submits `cb` under its lane's lock, waiting for `wait` at `stage` and
// signaling `signal` when given. returns the value it signals, which names
// its lane too.
uint64_t civue_vk_submit(civue_vk_lane lane, VkCommandBuffer cb,
                         VkSemaphore wait, VkPipelineStageFlags2 stage,
                         VkSemaphore signal);
// presents on the ui's queue.
VkResult civue_vk_present(const VkPresentInfoKHR *info);
// whether the gpu is done with a submission, and waiting for it.
bool civue_vk_done(uint64_t value);
void civue_vk_wait(uint64_t value);
// waits for everything submitted so far.
void civue_vk_wait_all();
// textures the render lane wrote up to `value` go to the ui, its next
// submissions see them.
void civue_vk_handoff(uint64_t value);
// runs `fn` once the gpu is done with every submission made so far, from
// whichever thread submits or collects next.
void civue_vk_defer(void (*fn)(void *), void *arg);
// runs what the finished submissions let go.
void civue_vk_collect();

/* ========================================================================== */
/* resources.                                                                 */
/* ========================================================================== */

typedef struct {
  VkBuffer buffer;
  civue_vk_mem mem;
  VkDeviceSize size;
} civue_vk_buf;

// a buffer, host visible and mapped when `host`.
bool civue_vk_buf_new(VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                      civue_vk_buf *out);
// frees it once the gpu is done with it.
void civue_vk_buf_free(civue_vk_buf *b);

// a texture, counted, what the renderers hand each other as a void *. it
// rests in SHADER_READ_ONLY_OPTIMAL between command buffers once written.
typedef struct civue_vk_tex {
  VkImage image;
  VkImageView view;
  civue_vk_mem mem;
  uint32_t width, height;
  VkFormat format;
  VkSampleCountFlagBits samples;
  VkImageAspectFlags aspects;
  // the layout the last recorded command left it in.
  VkImageLayout layout;
  _Atomic uint32_t refs;
} civue_vk_tex;

civue_vk_tex *civue_vk_tex_new(uint32_t width, uint32_t height, VkFormat format,
                               VkImageUsageFlags usage,
                               VkSampleCountFlagBits samples);
civue_vk_tex *civue_vk_tex_retain(civue_vk_tex *t);
// destroyed once the gpu is done with every submission made so far.
void civue_vk_tex_release(civue_vk_tex *t);
// moves `t` into `to`, every earlier access finished first.
void civue_vk_layout(VkCommandBuffer cb, civue_vk_tex *t, VkImageLayout to);
// the same even when it is in `to` already, for a pass writing what an
// earlier one wrote.
void civue_vk_layout_sync(VkCommandBuffer cb, civue_vk_tex *t,
                          VkImageLayout to);
// a full barrier between what came before in the queue and what follows.
void civue_vk_barrier_all(VkCommandBuffer cb);

// what copies into buffers wrote is seen by the cpu once the gpu is done.
void civue_vk_readback_barrier(VkCommandBuffer cb);

VkShaderModule civue_vk_shader(const uint8_t *spirv, size_t len);
VkSampler civue_vk_sampler(VkFilter filter, VkSamplerAddressMode mode,
                           bool mipmaps, float anisotropy);

// a command pool and buffers recorded from it, one thread at a time.
typedef struct {
  civue_vk_lane lane;
  VkCommandPool pool;
  CIVUE_VEC(VkCommandBuffer) free;
  // buffers submitted, by the timeline value that frees them.
  CIVUE_VEC(struct civue_vk_pending {
    VkCommandBuffer cb;
    uint64_t value;
  })
  pending;
} civue_vk_cmds;

void civue_vk_cmds_init(civue_vk_cmds *c, civue_vk_lane lane);
void civue_vk_cmds_free(civue_vk_cmds *c);
// a buffer in the recording state.
VkCommandBuffer civue_vk_cmds_begin(civue_vk_cmds *c);
// ends and submits it, returning its timeline value.
uint64_t civue_vk_cmds_submit(civue_vk_cmds *c, VkCommandBuffer cb,
                              VkSemaphore wait, VkPipelineStageFlags2 stage,
                              VkSemaphore signal);

#ifdef __cplusplus
}
#endif

#endif
