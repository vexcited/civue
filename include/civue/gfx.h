#ifndef CIVUE_GFX_H
#define CIVUE_GFX_H

#include <civue/draw.h>

#ifdef __linux__
#include <civue/vulkan.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================================================== */
/* the renderer. draws draw lists on the gpu in one render pass of instanced quads, its atlases and images    */
/* uploaded on the way.                                                                                       */
/* ========================================================================================================== */

typedef struct civue_gfx civue_gfx;

// a renderer on `device`, civue_gpu_device or a device the app made: id<MTLDevice> on macos, the
// const civue_vk_device * of civue_vk on linux.
civue_gfx *civue_gfx_new(void *device);
void civue_gfx_free(civue_gfx *g);
void *civue_gfx_device(civue_gfx *g);
// the command queue it encodes for on macos, id<MTLCommandQueue>.
void *civue_gfx_queue(civue_gfx *g);
// the coverage atlas of glyphs and icons, and the color glyph one.
civue_atlas *civue_gfx_atlas(civue_gfx *g);
civue_atlas *civue_gfx_color_atlas(civue_gfx *g);
// makes a texture drawable with civue_dl_image, retained: id<MTLTexture> on macos, a civue_vk_tex on linux,
// of srgb encoded premultiplied colors.
civue_texture civue_gfx_register_texture(civue_gfx *g, void *texture);
// points an id at another texture.
void civue_gfx_update_texture(civue_gfx *g, civue_texture id, void *texture);
void civue_gfx_free_texture(civue_gfx *g, civue_texture id);
// uploads premultiplied rgba8 pixels and registers them.
civue_texture civue_gfx_upload_image(civue_gfx *g, uint32_t width, uint32_t height, const uint8_t *rgba);
// encodes `list` over `target`, id<MTLTexture> or civue_vk_tex, in the command buffer `cb`, cleared first when
// `clear` is given.
void civue_gfx_encode(civue_gfx *g, void *cb, void *target, uint32_t width, uint32_t height,
                      const civue_draw_list *list, const civue_color *clear);
// draws a list offscreen over `clear` and reads it back as premultiplied rgba8, for tests and screenshots.
bool civue_gfx_snapshot(civue_gfx *g, const civue_draw_list *list, uint32_t width, uint32_t height, civue_color clear,
                        civue_bytes *rgba);

#ifdef __linux__
// on vulkan the renderer records into command buffers of its own, and knows when they are submitted so the
// buffers they read are kept until the gpu is done. a command buffer to encode frames into, recording.
VkCommandBuffer civue_gfx_vk_begin(civue_gfx *g);
// submits it, waiting for `wait` before writing colors and signaling `signal`, both optional. returns its
// timeline value.
uint64_t civue_gfx_vk_submit(civue_gfx *g, VkCommandBuffer cb, VkSemaphore wait, VkSemaphore signal);
// a texture wrapping an image someone else owns, like a swapchain's, freed with free rather than released.
civue_vk_tex *civue_gfx_vk_wrap(VkImage image, VkImageView view, VkFormat format, uint32_t width, uint32_t height);
// encodes `list` taking away what it covers, alpha too: the corners of a window with round ones.
void civue_gfx_vk_erase(civue_gfx *g, VkCommandBuffer cb, civue_vk_tex *target, uint32_t width, uint32_t height,
                        const civue_draw_list *list);
#endif

#ifdef __cplusplus
}
#endif

#endif
