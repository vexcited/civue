#include "../internal.h"

#include <civue/gfx.h>
#include <civue/raster.h>

#include <stdatomic.h>
#include <stddefer.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char civue_ui_vert[], civue_ui_frag[];
extern const size_t civue_ui_vert_len, civue_ui_frag_len;

// an image the list draws. one civue uploaded keeps its pixels for the cpu,
// and its texture is made by the first frame the gpu draws it in.
typedef struct {
  civue_texture id;
  civue_vk_tex *texture;
  u8 *pixels;
  u32 width, height;
} Image;

typedef struct {
  VkFormat format;
  // 0 over, 1 multiplied, 2 erasing what the coverage covers.
  u8 mode;
  VkPipeline state;
} Pipeline;

// a buffer for a frame's quads, kept for the next frames instead of made and
// freed each time. it is busy until the gpu is done with the submission
// that read it, `value` 0 while that one is not submitted yet.
typedef struct {
  civue_vk_buf buf;
  bool busy;
  u64 value;
} QuadBuffer;

constexpr u32 QUAD_BUFFERS = 4;

struct civue_gfx {
  // null until the gpu first draws: a window the cpu draws alone never
  // makes a device.
  const civue_vk_device *vk;
  bool tried;
  civue_vk_cmds cmds;
  QuadBuffer quad_buffers[QUAD_BUFFERS];
  VkShaderModule vs, fs;
  VkDescriptorSetLayout set_layout;
  VkPipelineLayout layout;
  Vec(Pipeline) pipelines;
  VkSampler linear, nearest;
  civue_atlas atlas, color_atlas;
  civue_vk_tex *atlas_texture;
  // made when the first color glyph arrives, most screens have none.
  civue_vk_tex *color_atlas_texture;
  civue_vk_tex *dummy;
  Vec(Image) images;
  civue_texture next_id;
  // what the buffer being recorded uses: staging buffers freed and quad
  // buffers marked once it is submitted.
  Vec(civue_vk_buf) staging;
  Vec(QuadBuffer *) quads_used;
  // gfx_vk_erase is encoding.
  bool erasing;
};

static civue_vk_tex *texture(VkFormat format, u32 w, u32 h) {
  return civue_vk_tex_new(
    w, h, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
    VK_SAMPLE_COUNT_1_BIT);
}

// copies `data`, rows of `row` bytes, into a region of a texture, in `cb`.
static void upload(civue_gfx *g, VkCommandBuffer cb, civue_vk_tex *dst, u32 x,
                   u32 y, u32 w, u32 h, usize row, const u8 *data) {
  civue_vk_buf staging;
  if (!civue_vk_buf_new((VkDeviceSize)row * h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                        true, &staging)) {
    return;
  }
  memcpy(staging.mem.map, data, row * h);
  civue_vk_layout(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  u32 bpp = dst->format == VK_FORMAT_R8_UNORM ? 1 : 4;
  VkBufferImageCopy region = {
    .bufferRowLength = (u32)(row / bpp),
    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
    .imageOffset = {(i32)x, (i32)y, 0},
    .imageExtent = {w, h, 1}};
  vkCmdCopyBufferToImage(cb, staging.buffer, dst->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  vec_push(&g->staging, staging);
}

static void ready_to_sample(VkCommandBuffer cb, civue_vk_tex *t) {
  civue_vk_layout(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

static bool ready(civue_gfx *g);

VkCommandBuffer civue_gfx_vk_begin(civue_gfx *g) {
  if (!ready(g)) {
    die("vulkan: no device to draw the ui with");
  }
  return civue_vk_cmds_begin(&g->cmds);
}

u64 civue_gfx_vk_submit(civue_gfx *g, VkCommandBuffer cb, VkSemaphore wait,
                        VkSemaphore signal) {
  u64 value = civue_vk_cmds_submit(
    &g->cmds, cb, wait, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
    signal);
  vec_each(&g->staging, s) {
    civue_vk_buf_free(s);
  }
  vec_clear(&g->staging);
  vec_each(&g->quads_used, q) {
    (*q)->value = value;
  }
  vec_clear(&g->quads_used);
  return value;
}

civue_vk_tex *civue_gfx_vk_wrap(VkImage image, VkImageView view,
                                VkFormat format, u32 width, u32 height) {
  civue_vk_tex *t = xcalloc(1, sizeof *t);
  *t = (civue_vk_tex){.image = image,
                      .view = view,
                      .width = width,
                      .height = height,
                      .format = format,
                      .samples = VK_SAMPLE_COUNT_1_BIT,
                      .aspects = VK_IMAGE_ASPECT_COLOR_BIT,
                      .layout = VK_IMAGE_LAYOUT_UNDEFINED,
                      .refs = 1};
  return t;
}

civue_gfx *civue_gfx_new(void *device) {
  civue_gfx *g = xcalloc(1, sizeof *g);
  g->atlas.bytes_per_pixel = 1;
  g->color_atlas.bytes_per_pixel = 4;
  g->atlas.keeps = g->color_atlas.keeps = true;
  g->next_id = 1;
  return g;
}

// what the gpu draws with, made once. false when there is no device.
static bool ready(civue_gfx *g) {
  if (g->tried) {
    return g->vk;
  }
  g->tried = true;
  const civue_vk_device *v = civue_vk();
  if (!v) {
    return false;
  }
  g->vk = v;
  civue_vk_cmds_init(&g->cmds, CIVUE_VK_LANE_UI);
  g->vs = civue_vk_shader(civue_ui_vert, civue_ui_vert_len);
  g->fs = civue_vk_shader(civue_ui_frag, civue_ui_frag_len);
  VkDescriptorSetLayoutBinding binds[5];
  for (u32 i = 0; i < countof(binds); i++) {
    binds[i] = (VkDescriptorSetLayoutBinding){
      .binding = i,
      .descriptorType =
        i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLER,
      .descriptorCount = 1,
      .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
  }
  VkDescriptorSetLayoutCreateInfo set = {
    .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
    .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
    .bindingCount = countof(binds),
    .pBindings = binds};
  vkCreateDescriptorSetLayout(v->device, &set, nullptr, &g->set_layout);
  VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
  VkPipelineLayoutCreateInfo layout = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    .setLayoutCount = 1,
    .pSetLayouts = &g->set_layout,
    .pushConstantRangeCount = 1,
    .pPushConstantRanges = &push};
  vkCreatePipelineLayout(v->device, &layout, nullptr, &g->layout);
  g->linear = civue_vk_sampler(VK_FILTER_LINEAR,
                               VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, 1);
  g->nearest = civue_vk_sampler(
    VK_FILTER_NEAREST, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, false, 1);
  g->atlas_texture =
    texture(VK_FORMAT_R8_UNORM, CIVUE_ATLAS_SIZE, CIVUE_ATLAS_SIZE);
  g->dummy = texture(VK_FORMAT_R8G8B8A8_UNORM, 1, 1);
  // both start cleared, ready to sample.
  VkCommandBuffer cb = civue_vk_cmds_begin(&g->cmds);
  VkClearColorValue zero = {};
  VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  civue_vk_tex *fresh[] = {g->atlas_texture, g->dummy};
  for (u32 i = 0; i < countof(fresh); i++) {
    civue_vk_layout(cb, fresh[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cb, fresh[i]->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    ready_to_sample(cb, fresh[i]);
  }
  civue_gfx_vk_submit(g, cb, VK_NULL_HANDLE, VK_NULL_HANDLE);
  // the glyphs the cpu's frames made are all to upload.
  g->atlas.stale = g->color_atlas.stale = true;
  return true;
}

void civue_gfx_free(civue_gfx *g) {
  if (!g) {
    return;
  }
  vec_each(&g->images, i) {
    civue_vk_tex_release(i->texture);
    free(i->pixels);
  }
  vec_free(&g->images);
  civue_atlas_free(&g->atlas);
  civue_atlas_free(&g->color_atlas);
  if (!g->vk) {
    free(g);
    return;
  }
  VkDevice d = g->vk->device;
  civue_vk_wait_all();
  vec_each(&g->pipelines, p) {
    vkDestroyPipeline(d, p->state, nullptr);
  }
  vec_free(&g->pipelines);
  for (u32 i = 0; i < QUAD_BUFFERS; i++) {
    civue_vk_buf_free(&g->quad_buffers[i].buf);
  }
  civue_vk_tex_release(g->atlas_texture);
  civue_vk_tex_release(g->color_atlas_texture);
  civue_vk_tex_release(g->dummy);
  vkDestroySampler(d, g->linear, nullptr);
  vkDestroySampler(d, g->nearest, nullptr);
  vkDestroyShaderModule(d, g->vs, nullptr);
  vkDestroyShaderModule(d, g->fs, nullptr);
  vkDestroyPipelineLayout(d, g->layout, nullptr);
  vkDestroyDescriptorSetLayout(d, g->set_layout, nullptr);
  civue_vk_cmds_free(&g->cmds);
  vec_free(&g->staging);
  vec_free(&g->quads_used);
  civue_vk_collect();
  free(g);
}

void *civue_gfx_device(civue_gfx *g) {
  return ready(g) ? (void *)g->vk : nullptr;
}

void *civue_gfx_queue(civue_gfx *g) {
  return ready(g) ? g->vk->queue : nullptr;
}

civue_atlas *civue_gfx_atlas(civue_gfx *g) {
  return &g->atlas;
}

civue_atlas *civue_gfx_color_atlas(civue_gfx *g) {
  return &g->color_atlas;
}

static VkPipeline pipeline(civue_gfx *g, VkFormat format, u8 mode) {
  vec_each(&g->pipelines, p) {
    if (p->format == format && p->mode == mode) {
      return p->state;
    }
  }
  VkPipelineShaderStageCreateInfo stages[2] = {
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
     .stage = VK_SHADER_STAGE_VERTEX_BIT,
     .module = g->vs,
     .pName = "main"},
    {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
     .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
     .module = g->fs,
     .pName = "main"},
  };
  VkVertexInputBindingDescription binding = {0, sizeof(civue_quad),
                                             VK_VERTEX_INPUT_RATE_INSTANCE};
  VkVertexInputAttributeDescription attrs[10];
  for (u32 i = 0; i < countof(attrs); i++) {
    attrs[i] = (VkVertexInputAttributeDescription){
      i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
  }
  VkPipelineVertexInputStateCreateInfo input = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    .vertexBindingDescriptionCount = 1,
    .pVertexBindingDescriptions = &binding,
    .vertexAttributeDescriptionCount = countof(attrs),
    .pVertexAttributeDescriptions = attrs};
  VkPipelineInputAssemblyStateCreateInfo assembly = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
    .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP};
  VkPipelineViewportStateCreateInfo viewport = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
    .viewportCount = 1,
    .scissorCount = 1};
  VkPipelineRasterizationStateCreateInfo raster = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
    .polygonMode = VK_POLYGON_MODE_FILL,
    .cullMode = VK_CULL_MODE_NONE,
    .lineWidth = 1};
  VkPipelineMultisampleStateCreateInfo ms = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
    .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  // premultiplied colors. multiplying is the destination times the source,
  // plus what the source's coverage leaves of it. erasing keeps only what
  // the coverage leaves, alpha too.
  VkPipelineColorBlendAttachmentState att = {
    .blendEnable = true,
    .srcColorBlendFactor = mode == 1   ? VK_BLEND_FACTOR_DST_COLOR
                           : mode == 2 ? VK_BLEND_FACTOR_ZERO
                                       : VK_BLEND_FACTOR_ONE,
    .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .colorBlendOp = VK_BLEND_OP_ADD,
    .srcAlphaBlendFactor =
      mode == 2 ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_ONE,
    .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
    .alphaBlendOp = VK_BLEND_OP_ADD,
    .colorWriteMask = 0xf};
  VkPipelineColorBlendStateCreateInfo blend = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
    .attachmentCount = 1,
    .pAttachments = &att};
  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dynamic = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
    .dynamicStateCount = 2,
    .pDynamicStates = dyn};
  VkPipelineRenderingCreateInfo rendering = {
    .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
    .colorAttachmentCount = 1,
    .pColorAttachmentFormats = &format};
  VkGraphicsPipelineCreateInfo info = {
    .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
    .pNext = &rendering,
    .stageCount = 2,
    .pStages = stages,
    .pVertexInputState = &input,
    .pInputAssemblyState = &assembly,
    .pViewportState = &viewport,
    .pRasterizationState = &raster,
    .pMultisampleState = &ms,
    .pColorBlendState = &blend,
    .pDynamicState = &dynamic,
    .layout = g->layout};
  VkPipeline state;
  if (vkCreateGraphicsPipelines(g->vk->device, g->vk->cache, 1, &info, nullptr,
                                &state) != VK_SUCCESS) {
    die("vulkan: no ui pipeline");
  }
  vec_push(&g->pipelines, ((Pipeline){format, mode, state}));
  return state;
}

civue_texture civue_gfx_register_texture(civue_gfx *g, void *t) {
  civue_texture tid = g->next_id++;
  vec_push(&g->images, ((Image){tid, civue_vk_tex_retain(t)}));
  return tid;
}

static Image *image(civue_gfx *g, civue_texture tid) {
  vec_each(&g->images, i) {
    if (i->id == tid) {
      return i;
    }
  }
  return nullptr;
}

void civue_gfx_update_texture(civue_gfx *g, civue_texture tid, void *t) {
  Image *i = image(g, tid);
  if (!i) {
    return;
  }
  civue_vk_tex_retain(t);
  civue_vk_tex_release(i->texture);
  i->texture = t;
  // no longer the pixels civue uploaded.
  free(i->pixels);
  i->pixels = nullptr;
}

void civue_gfx_free_texture(civue_gfx *g, civue_texture tid) {
  Image *i = image(g, tid);
  if (!i) {
    return;
  }
  civue_vk_tex_release(i->texture);
  free(i->pixels);
  *i = vec_last(&g->images);
  g->images.len--;
}

civue_texture civue_gfx_upload_image(civue_gfx *g, u32 w, u32 h,
                                     const u8 *rgba) {
  civue_texture tid = g->next_id++;
  usize len = (usize)w * h * 4;
  u8 *pixels = xmalloc(len ? len : 1);
  memcpy(pixels, rgba, len);
  vec_push(&g->images, ((Image){tid, nullptr, pixels, w, h}));
  return tid;
}

bool civue_gfx_pixels(civue_gfx *g, civue_texture tid, civue_pixels *out) {
  Image *i = image(g, tid);
  if (!i || !i->pixels) {
    return false;
  }
  *out = (civue_pixels){i->pixels, i->width, i->height, (usize)i->width * 4};
  return true;
}

bool civue_gfx_static(civue_gfx *g, const civue_draw_list *list) {
  vec_each(&list->batches, b) {
    Image *i = b->texture ? image(g, b->texture) : nullptr;
    if (i && !i->pixels) {
      return false;
    }
  }
  return true;
}

// a free buffer of at least `len` bytes, marked busy, or nullptr when the
// gpu still holds them all.
static QuadBuffer *quad_buffer(civue_gfx *g, usize len) {
  for (u32 i = 0; i < QUAD_BUFFERS; i++) {
    QuadBuffer *q = &g->quad_buffers[i];
    if (q->busy && (!q->value || !civue_vk_done(q->value))) {
      continue;
    }
    q->busy = false;
    if (q->buf.size < len) {
      civue_vk_buf_free(&q->buf);
      usize cap = MAX(len + len / 2, (usize)64 * 1024);
      if (!civue_vk_buf_new(cap, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true,
                            &q->buf)) {
        return nullptr;
      }
    }
    q->busy = true;
    q->value = 0;
    vec_push(&g->quads_used, q);
    return q;
  }
  return nullptr;
}

void civue_gfx_encode(civue_gfx *g, void *cbp, void *target, u32 width,
                      u32 height, const civue_draw_list *list,
                      const civue_color *clear) {
  VkCommandBuffer cb = cbp;
  civue_vk_tex *dst = target;
  if (g->color_atlas.pixels && !g->color_atlas_texture) {
    g->color_atlas_texture =
      texture(VK_FORMAT_R8G8B8A8_UNORM, CIVUE_ATLAS_SIZE, CIVUE_ATLAS_SIZE);
    VkClearColorValue zero = {};
    VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    civue_vk_layout(cb, g->color_atlas_texture,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdClearColorImage(cb, g->color_atlas_texture->image,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &all);
    g->color_atlas.stale = true;
  }
  // the glyphs and icons the textures do not have yet, from the pixels the
  // atlases keep: the rows in use of a stale one, else what was added.
  civue_atlas *atlases[2] = {&g->atlas, &g->color_atlas};
  civue_vk_tex *textures[2] = {g->atlas_texture, g->color_atlas_texture};
  for (u32 k = 0; k < countof(atlases); k++) {
    civue_atlas *a = atlases[k];
    usize bpp = a->bytes_per_pixel;
    if (a->pixels && textures[k]) {
      if (a->stale && a->top) {
        upload(g, cb, textures[k], 0, 0, CIVUE_ATLAS_SIZE, a->top,
               (usize)CIVUE_ATLAS_SIZE * bpp, a->pixels);
      }
      else if (!a->stale) {
        vec_each(&a->pending, p) {
          if (!p->w || !p->h) {
            continue;
          }
          // its rows are the atlas's, as long as the atlas is wide.
          civue_vk_buf staging;
          usize row = (usize)p->w * bpp;
          if (!civue_vk_buf_new(row * p->h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                true, &staging)) {
            continue;
          }
          for (u32 y = 0; y < p->h; y++) {
            memcpy(staging.mem.map + y * row,
                   a->pixels +
                     ((usize)(p->y + y) * CIVUE_ATLAS_SIZE + p->x) * bpp,
                   row);
          }
          civue_vk_layout(cb, textures[k],
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
          VkBufferImageCopy region = {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageOffset = {(i32)p->x, (i32)p->y, 0},
            .imageExtent = {p->w, p->h, 1}};
          vkCmdCopyBufferToImage(cb, staging.buffer, textures[k]->image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                 &region);
          vec_push(&g->staging, staging);
        }
      }
    }
    a->stale = false;
    a->pending.len = 0;
  }
  ready_to_sample(cb, g->atlas_texture);
  if (g->color_atlas_texture) {
    ready_to_sample(cb, g->color_atlas_texture);
  }
  vec_each(&list->batches, b) {
    Image *img = b->texture ? image(g, b->texture) : nullptr;
    if (!img) {
      continue;
    }
    // an image the cpu alone drew so far.
    if (!img->texture && img->pixels && img->width && img->height) {
      img->texture = texture(VK_FORMAT_R8G8B8A8_UNORM, img->width, img->height);
      upload(g, cb, img->texture, 0, 0, img->width, img->height,
             (usize)img->width * 4, img->pixels);
    }
    if (img->texture) {
      ready_to_sample(cb, img->texture);
    }
  }

  civue_vk_layout(cb, dst, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  VkRenderingAttachmentInfo color = {
    .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
    .imageView = dst->view,
    .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
    .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
  if (clear) {
    civue_color c = *clear;
    color.clearValue.color =
      (VkClearColorValue){.float32 = {c.r * c.a, c.g * c.a, c.b * c.a, c.a}};
  }
  VkRenderingInfo rendering = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                               .renderArea = {{0, 0}, {width, height}},
                               .layerCount = 1,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &color};
  vkCmdBeginRendering(cb, &rendering);
  if (list->quads.len) {
    usize len = list->quads.len * sizeof(civue_quad);
    QuadBuffer *slot = quad_buffer(g, len);
    civue_vk_buf quads;
    if (slot) {
      quads = slot->buf;
    }
    else if (civue_vk_buf_new(len, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true,
                              &quads)) {
      vec_push(&g->staging, quads);
    }
    else {
      vkCmdEndRendering(cb);
      return;
    }
    memcpy(quads.mem.map, list->quads.v, len);
    // flipped like metal's, y up in clip space.
    VkViewport vp = {0, (f32)height, (f32)width, -(f32)height, 0, 1};
    VkRect2D scissor = {{0, 0}, {width, height}};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &scissor);
    f32 globals[4] = {(f32)width, (f32)height, 0, 0};
    vkCmdPushConstants(cb, g->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16,
                       globals);
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 0, 1, &quads.buffer, &offset);
    u8 mode = g->erasing ? 2 : 0;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline(g, dst->format, mode));
    civue_vk_tex *color_atlas =
      g->color_atlas_texture ? g->color_atlas_texture : g->dummy;
    for (u32 b = 0; b < list->batches.len; b++) {
      const civue_batch *batch = &list->batches.v[b];
      u32 end = b + 1 < list->batches.len ? list->batches.v[b + 1].start
                                          : list->quads.len;
      u8 want = g->erasing || batch->erase ? 2 : batch->multiply ? 1 : 0;
      if (want != mode) {
        mode = want;
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipeline(g, dst->format, mode));
      }
      Image *img = batch->texture ? image(g, batch->texture) : nullptr;
      VkDescriptorImageInfo infos[5] = {
        {.imageView = g->atlas_texture->view,
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.imageView = img && img->texture ? img->texture->view : g->dummy->view,
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.imageView = color_atlas->view,
         .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {.sampler = g->linear},
        {.sampler = g->nearest},
      };
      VkWriteDescriptorSet writes[5];
      for (u32 i = 0; i < countof(writes); i++) {
        writes[i] = (VkWriteDescriptorSet){
          .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
          .dstBinding = i,
          .descriptorCount = 1,
          .descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                                  : VK_DESCRIPTOR_TYPE_SAMPLER,
          .pImageInfo = &infos[i]};
      }
      g->vk->push_descriptor(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g->layout, 0,
                             countof(writes), writes);
      vkCmdDraw(cb, 4, end - batch->start, 0, batch->start);
    }
  }
  vkCmdEndRendering(cb);
}

static bool find_pixels(void *ctx, civue_texture t, civue_pixels *out) {
  return civue_gfx_pixels(ctx, t, out);
}

// without a gpu the cpu draws the list, when it draws nothing the gpu alone
// has.
static bool snapshot_on_cpu(civue_gfx *g, const civue_draw_list *list,
                            u32 width, u32 height, civue_color clear,
                            civue_bytes *rgba) {
  civue_raster_images images = {find_pixels, g};
  civue_damage all = {.all = true};
  u64 cost;
  if (!civue_raster_cost(list, &all, width, height, &images, &cost)) {
    return false;
  }
  usize len = (usize)width * height * 4;
  rgba->len = 0;
  vec_reserve(rgba, len);
  rgba->len = len;
  civue_surface s = {rgba->v, width, height, (usize)width * 4};
  civue_raster(&s, list, clear, &all, &g->atlas, &g->color_atlas, &images);
  // the cpu draws blue first.
  for (usize i = 0; i < len; i += 4) {
    u8 b = rgba->v[i];
    rgba->v[i] = rgba->v[i + 2];
    rgba->v[i + 2] = b;
  }
  return true;
}

bool civue_gfx_snapshot(civue_gfx *g, const civue_draw_list *list, u32 width,
                        u32 height, civue_color clear, civue_bytes *rgba) {
  if (!ready(g)) {
    return snapshot_on_cpu(g, list, width, height, clear, rgba);
  }
  civue_vk_tex *target = civue_vk_tex_new(
    width, height, VK_FORMAT_R8G8B8A8_UNORM,
    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
      VK_IMAGE_USAGE_SAMPLED_BIT,
    VK_SAMPLE_COUNT_1_BIT);
  defer civue_vk_tex_release(target);
  usize row = (usize)width * 4, len = row * height;
  civue_vk_buf buf;
  if (!civue_vk_buf_new(len, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, &buf)) {
    return false;
  }
  VkCommandBuffer cb = civue_gfx_vk_begin(g);
  civue_gfx_encode(g, cb, target, width, height, list, &clear);
  civue_vk_layout(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
  VkBufferImageCopy region = {
    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
    .imageExtent = {width, height, 1}};
  vkCmdCopyImageToBuffer(cb, target->image,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf.buffer, 1,
                         &region);
  civue_vk_readback_barrier(cb);
  civue_vk_wait(civue_gfx_vk_submit(g, cb, VK_NULL_HANDLE, VK_NULL_HANDLE));
  rgba->len = 0;
  buf_put(rgba, buf.mem.map, len);
  civue_vk_buf_free(&buf);
  return true;
}

void civue_gfx_vk_erase(civue_gfx *g, VkCommandBuffer cb, civue_vk_tex *target,
                        u32 width, u32 height, const civue_draw_list *list) {
  g->erasing = true;
  civue_gfx_encode(g, cb, target, width, height, list, nullptr);
  g->erasing = false;
}
