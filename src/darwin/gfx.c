#include "darwin.h"

#include <civue/gfx.h>

#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

extern const unsigned char civue_ui_metallib[];
extern const size_t civue_ui_metallib_len;

// the buffer slot of the quads, past the globals in slot 0.
constexpr NSUInteger QUAD_SLOT = 1;

enum : NSUInteger {
  UI_FORMAT_R8 = 10,
  UI_VERTEX_FLOAT4 = 31,
  UI_STEP_PER_INSTANCE = 2,
  UI_TRIANGLE_STRIP = 4,
  UI_FILTER_NEAREST = 0,
  UI_BLEND_DESTINATION_COLOR = 6,
};

typedef struct {
  civue_texture id;
  id texture;
} Image;

typedef struct {
  NSUInteger format;
  bool multiply;
  id state;
} Pipeline;

// a buffer for a frame's quads, kept for the next frames instead of made and
// freed each time. it is busy from encoding until the gpu is done with it.
typedef struct {
  id buffer;
  NSUInteger capacity;
  _Atomic bool busy;
} QuadBuffer;

constexpr u32 QUAD_BUFFERS = 4;

struct civue_gfx {
  id device, queue;
  QuadBuffer quad_buffers[QUAD_BUFFERS];
  id vs, fs;
  Vec(Pipeline) pipelines;
  id linear, nearest;
  civue_atlas atlas, color_atlas;
  id atlas_texture;
  // made when the first color glyph arrives, most screens have none.
  id color_atlas_texture;
  id dummy;
  Vec(Image) images;
  civue_texture next_id;
};

static id texture(id device, NSUInteger format, u32 w, u32 h, NSUInteger usage) {
  id d = msg4(id, CLS("MTLTextureDescriptor"), "texture2DDescriptorWithPixelFormat:width:height:mipmapped:", format,
              (NSUInteger)MAX(w, 1u), (NSUInteger)MAX(h, 1u), (BOOL)NO);
  msg1(void, d, "setUsage:", usage);
  msg1(void, d, "setStorageMode:", (NSUInteger)MTLStorageModePrivate);
  return msg1(id, device, "newTextureWithDescriptor:", d);
}

// copies `data` into a region of a texture through a buffer, in `cb`.
static void upload(civue_gfx *g, id cb, id dst, u32 x, u32 y, u32 w, u32 h, NSUInteger row, const u8 *data) {
  NSUInteger len = row * h;
  id staging = msg3(id, g->device, "newBufferWithBytes:length:options:", (const void *)data, len,
                    (NSUInteger)MTLResourceStorageModeShared);
  if (!staging) return;
  id blit = msg0(id, cb, "blitCommandEncoder");
  MTLSize size = {w, h, 1};
  MTLOrigin origin = {x, y, 0};
  ((void (*)(id, SEL, id, NSUInteger, NSUInteger, NSUInteger, MTLSize, id, NSUInteger, NSUInteger,
             MTLOrigin))objc_msgSend)(blit,
                                      SEL_("copyFromBuffer:sourceOffset:sourceBytesPerRow:sourceBytesPerImage:"
                                           "sourceSize:toTexture:destinationSlice:destinationLevel:"
                                           "destinationOrigin:"),
                                      staging, (NSUInteger)0, row, len, size, dst, (NSUInteger)0, (NSUInteger)0,
                                      origin);
  msg0(void, blit, "endEncoding");
  release(staging);
}

static id sampler(id device, NSUInteger filter) {
  id d = NEW("MTLSamplerDescriptor");
  msg1(void, d, "setMinFilter:", filter);
  msg1(void, d, "setMagFilter:", filter);
  msg1(void, d, "setSAddressMode:", (NSUInteger)MTLSamplerAddressModeClampToEdge);
  msg1(void, d, "setTAddressMode:", (NSUInteger)MTLSamplerAddressModeClampToEdge);
  id s = msg1(id, device, "newSamplerStateWithDescriptor:", d);
  release(d);
  return s;
}

civue_gfx *civue_gfx_new(void *device) {
  civue_gfx *g = xcalloc(1, sizeof *g);
  g->device = retain(device);
  g->queue = msg0(id, g->device, "newCommandQueue");
  dispatch_data_t data =
      dispatch_data_create(civue_ui_metallib, civue_ui_metallib_len, nullptr, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  id error = nullptr;
  id lib = msg2(id, g->device, "newLibraryWithData:error:", (id)data, &error);
  dispatch_release(data);
  if (!lib) die("ui shader: %s", ns_cstr(msg0(id, error, "localizedDescription")));
  g->vs = msg1(id, lib, "newFunctionWithName:", ns_str("ui_vs"));
  g->fs = msg1(id, lib, "newFunctionWithName:", ns_str("ui_fs"));
  release(lib);
  g->linear = sampler(g->device, MTLSamplerMinMagFilterLinear);
  g->nearest = sampler(g->device, UI_FILTER_NEAREST);
  g->atlas.bytes_per_pixel = 1;
  g->color_atlas.bytes_per_pixel = 4;
  g->atlas_texture = texture(g->device, UI_FORMAT_R8, CIVUE_ATLAS_SIZE, CIVUE_ATLAS_SIZE, MTLTextureUsageShaderRead);
  g->dummy = texture(g->device, MTLPixelFormatRGBA8Unorm, 1, 1, MTLTextureUsageShaderRead);
  g->next_id = 1;
  return g;
}

void civue_gfx_free(civue_gfx *g) {
  if (!g) return;
  vec_each(&g->pipelines, p) release(p->state);
  vec_free(&g->pipelines);
  vec_each(&g->images, i) release(i->texture);
  vec_free(&g->images);
  civue_atlas_free(&g->atlas);
  civue_atlas_free(&g->color_atlas);
  release(g->atlas_texture);
  release(g->color_atlas_texture);
  release(g->dummy);
  release(g->linear);
  release(g->nearest);
  release(g->vs);
  release(g->fs);
  release(g->queue);
  release(g->device);
  free(g);
}

void *civue_gfx_device(civue_gfx *g) { return g->device; }

void *civue_gfx_queue(civue_gfx *g) { return g->queue; }

civue_atlas *civue_gfx_atlas(civue_gfx *g) { return &g->atlas; }

civue_atlas *civue_gfx_color_atlas(civue_gfx *g) { return &g->color_atlas; }

static id pipeline(civue_gfx *g, NSUInteger format, bool multiply) {
  vec_each(&g->pipelines, p) if (p->format == format && p->multiply == multiply) return p->state;
  id d = NEW("MTLRenderPipelineDescriptor");
  msg1(void, d, "setVertexFunction:", g->vs);
  msg1(void, d, "setFragmentFunction:", g->fs);
  id v = msg0(id, CLS("MTLVertexDescriptor"), "vertexDescriptor");
  id attrs = msg0(id, v, "attributes");
  for (NSUInteger i = 0; i < 8; i++) {
    id a = msg1(id, attrs, "objectAtIndexedSubscript:", i);
    msg1(void, a, "setFormat:", (NSUInteger)UI_VERTEX_FLOAT4);
    msg1(void, a, "setOffset:", i * 16);
    msg1(void, a, "setBufferIndex:", QUAD_SLOT);
  }
  id layout = msg1(id, msg0(id, v, "layouts"), "objectAtIndexedSubscript:", QUAD_SLOT);
  msg1(void, layout, "setStride:", (NSUInteger)sizeof(civue_quad));
  msg1(void, layout, "setStepFunction:", (NSUInteger)UI_STEP_PER_INSTANCE);
  msg1(void, d, "setVertexDescriptor:", v);
  id att = msg1(id, msg0(id, d, "colorAttachments"), "objectAtIndexedSubscript:", (NSUInteger)0);
  msg1(void, att, "setPixelFormat:", format);
  // premultiplied colors. multiplying is the destination times the source,
  // plus what the source's coverage leaves of it.
  msg1(void, att, "setBlendingEnabled:", (BOOL)YES);
  msg1(void, att,
       "setSourceRGBBlendFactor:", multiply ? (NSUInteger)UI_BLEND_DESTINATION_COLOR : (NSUInteger)MTLBlendFactorOne);
  msg1(void, att, "setSourceAlphaBlendFactor:", (NSUInteger)MTLBlendFactorOne);
  msg1(void, att, "setDestinationRGBBlendFactor:", (NSUInteger)MTLBlendFactorOneMinusSourceAlpha);
  msg1(void, att, "setDestinationAlphaBlendFactor:", (NSUInteger)MTLBlendFactorOneMinusSourceAlpha);
  id error = nullptr;
  id state = msg2(id, g->device, "newRenderPipelineStateWithDescriptor:error:", d, &error);
  release(d);
  if (!state) die("ui pipeline: %s", ns_cstr(msg0(id, error, "localizedDescription")));
  vec_push(&g->pipelines, ((Pipeline){format, multiply, state}));
  return state;
}

civue_texture civue_gfx_register_texture(civue_gfx *g, void *t) {
  civue_texture tid = g->next_id++;
  vec_push(&g->images, ((Image){tid, retain(t)}));
  return tid;
}

static Image *image(civue_gfx *g, civue_texture tid) {
  vec_each(&g->images, i) if (i->id == tid) return i;
  return nullptr;
}

void civue_gfx_update_texture(civue_gfx *g, civue_texture tid, void *t) {
  Image *i = image(g, tid);
  if (!i) return;
  retain(t);
  release(i->texture);
  i->texture = t;
}

void civue_gfx_free_texture(civue_gfx *g, civue_texture tid) {
  Image *i = image(g, tid);
  if (!i) return;
  release(i->texture);
  *i = vec_last(&g->images);
  g->images.len--;
}

civue_texture civue_gfx_upload_image(civue_gfx *g, u32 w, u32 h, const u8 *rgba) {
  id t = texture(g->device, MTLPixelFormatRGBA8Unorm, w, h, MTLTextureUsageShaderRead);
  AUTORELEASE({
    id cb = msg0(id, g->queue, "commandBuffer");
    upload(g, cb, t, 0, 0, w, h, (NSUInteger)w * 4, rgba);
    msg0(void, cb, "commit");
  });
  civue_texture tid = civue_gfx_register_texture(g, t);
  release(t);
  return tid;
}

// a free buffer of at least `len` bytes, marked busy, or nullptr when the
// gpu still holds them all.
static QuadBuffer *quad_buffer(civue_gfx *g, NSUInteger len) {
  for (u32 i = 0; i < QUAD_BUFFERS; i++) {
    QuadBuffer *q = &g->quad_buffers[i];
    if (atomic_load(&q->busy)) continue;
    if (q->capacity < len) {
      release(q->buffer);
      NSUInteger cap = MAX(len + len / 2, (NSUInteger)64 * 1024);
      q->buffer = msg2(id, g->device, "newBufferWithLength:options:", cap, (NSUInteger)MTLResourceStorageModeShared);
      q->capacity = q->buffer ? cap : 0;
      if (!q->buffer) return nullptr;
    }
    atomic_store(&q->busy, true);
    return q;
  }
  return nullptr;
}

void civue_gfx_encode(civue_gfx *g, void *cb, void *target, u32 width, u32 height, const civue_draw_list *list,
                      const civue_color *clear) {
  if (g->color_atlas.pending.len && !g->color_atlas_texture)
    g->color_atlas_texture =
        texture(g->device, MTLPixelFormatRGBA8Unorm, CIVUE_ATLAS_SIZE, CIVUE_ATLAS_SIZE, MTLTextureUsageShaderRead);
  // the glyphs and icons new this frame, through one buffer and one encoder.
  civue_atlas *atlases[2] = {&g->atlas, &g->color_atlas};
  id textures[2] = {g->atlas_texture, g->color_atlas_texture};
  NSUInteger total = 0;
  for (u32 k = 0; k < 2; k++)
    vec_each(&atlases[k]->pending, p) total += ((NSUInteger)p->w * p->h * atlases[k]->bytes_per_pixel + 15) & ~15ul;
  id staging =
      total ? msg2(id, g->device, "newBufferWithLength:options:", total, (NSUInteger)MTLResourceStorageModeShared)
            : nullptr;
  id blit = staging ? msg0(id, cb, "blitCommandEncoder") : nullptr;
  u8 *bytes = staging ? msg0(u8 *, staging, "contents") : nullptr;
  NSUInteger at = 0;
  for (u32 k = 0; k < 2; k++) {
    civue_atlas *a = atlases[k];
    vec_each(&a->pending, p) {
      NSUInteger row = (NSUInteger)p->w * a->bytes_per_pixel, len = row * p->h;
      if (blit) {
        memcpy(bytes + at, p->data.v, len);
        ((void (*)(id, SEL, id, NSUInteger, NSUInteger, NSUInteger, MTLSize, id, NSUInteger, NSUInteger,
                   MTLOrigin))objc_msgSend)(blit,
                                            SEL_("copyFromBuffer:sourceOffset:sourceBytesPerRow:"
                                                 "sourceBytesPerImage:sourceSize:toTexture:destinationSlice:"
                                                 "destinationLevel:destinationOrigin:"),
                                            staging, at, row, len, (MTLSize){p->w, p->h, 1}, textures[k], (NSUInteger)0,
                                            (NSUInteger)0, (MTLOrigin){p->x, p->y, 0});
      }
      at += (len + 15) & ~15ul;
      vec_free(&p->data);
    }
    a->pending.len = 0;
  }
  if (blit) msg0(void, blit, "endEncoding");
  release(staging);

  id rp = msg0(id, CLS("MTLRenderPassDescriptor"), "renderPassDescriptor");
  id att = msg1(id, msg0(id, rp, "colorAttachments"), "objectAtIndexedSubscript:", (NSUInteger)0);
  msg1(void, att, "setTexture:", (id)target);
  if (clear) {
    civue_color c = *clear;
    msg1(void, att, "setLoadAction:", (NSUInteger)MTLLoadActionClear);
    msg1(void, att, "setClearColor:", ((MTLClearColor){c.r * c.a, c.g * c.a, c.b * c.a, c.a}));
  } else {
    msg1(void, att, "setLoadAction:", (NSUInteger)MTLLoadActionLoad);
  }
  msg1(void, att, "setStoreAction:", (NSUInteger)MTLStoreActionStore);
  NSUInteger format = msg0(NSUInteger, target, "pixelFormat");
  id enc = msg1(id, cb, "renderCommandEncoderWithDescriptor:", rp);
  if (!enc) return;
  if (list->quads.len) {
    NSUInteger len = (NSUInteger)(list->quads.len * sizeof(civue_quad));
    QuadBuffer *slot = quad_buffer(g, len);
    id quads;
    if (slot) {
      quads = slot->buffer;
      memcpy(msg0(void *, quads, "contents"), list->quads.v, len);
      void (^done)(id) = ^(id) { atomic_store(&slot->busy, false); };
      msg1(void, cb, "addCompletedHandler:", done);
    } else
      quads = msg3(id, g->device, "newBufferWithBytes:length:options:", (const void *)list->quads.v, len,
                   (NSUInteger)MTLResourceStorageModeShared);
    f32 globals[4] = {(f32)width, (f32)height, 0, 0};
    msg1(void, enc, "setRenderPipelineState:", pipeline(g, format, false));
    msg3(void, enc, "setVertexBytes:length:atIndex:", (const void *)globals, (NSUInteger)16, (NSUInteger)0);
    msg3(void, enc, "setFragmentBytes:length:atIndex:", (const void *)globals, (NSUInteger)16, (NSUInteger)0);
    msg3(void, enc, "setVertexBuffer:offset:atIndex:", quads, (NSUInteger)0, QUAD_SLOT);
    msg2(void, enc, "setFragmentTexture:atIndex:", g->atlas_texture, (NSUInteger)0);
    msg2(void, enc, "setFragmentTexture:atIndex:", g->color_atlas_texture ? g->color_atlas_texture : g->dummy,
         (NSUInteger)2);
    msg2(void, enc, "setFragmentSamplerState:atIndex:", g->linear, (NSUInteger)0);
    msg2(void, enc, "setFragmentSamplerState:atIndex:", g->nearest, (NSUInteger)1);
    bool multiplying = false;
    for (u32 b = 0; b < list->batches.len; b++) {
      const civue_batch *batch = &list->batches.v[b];
      u32 end = b + 1 < list->batches.len ? list->batches.v[b + 1].start : list->quads.len;
      if (batch->multiply != multiplying) {
        multiplying = batch->multiply;
        msg1(void, enc, "setRenderPipelineState:", pipeline(g, format, multiplying));
      }
      Image *img = batch->texture ? image(g, batch->texture) : nullptr;
      msg2(void, enc, "setFragmentTexture:atIndex:", img ? img->texture : g->dummy, (NSUInteger)1);
      msg5(void, enc,
           "drawPrimitives:vertexStart:vertexCount:instanceCount:baseInstance:", (NSUInteger)UI_TRIANGLE_STRIP,
           (NSUInteger)0, (NSUInteger)4, (NSUInteger)(end - batch->start), (NSUInteger)batch->start);
    }
    if (!slot) release(quads);
  }
  msg0(void, enc, "endEncoding");
}

bool civue_gfx_snapshot(civue_gfx *g, const civue_draw_list *list, u32 width, u32 height, civue_color clear,
                        civue_bytes *rgba) {
  id target = texture(g->device, MTLPixelFormatRGBA8Unorm, width, height,
                      MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
  NSUInteger row = (NSUInteger)width * 4, len = row * height;
  id buf = msg2(id, g->device, "newBufferWithLength:options:", len, (NSUInteger)MTLResourceStorageModeShared);
  bool ok = target && buf;
  if (ok)
    AUTORELEASE({
      id cb = msg0(id, g->queue, "commandBuffer");
      civue_gfx_encode(g, cb, target, width, height, list, &clear);
      id blit = msg0(id, cb, "blitCommandEncoder");
      ((void (*)(id, SEL, id, NSUInteger, NSUInteger, MTLOrigin, MTLSize, id, NSUInteger, NSUInteger,
                 NSUInteger))objc_msgSend)(blit,
                                           SEL_("copyFromTexture:sourceSlice:sourceLevel:sourceOrigin:sourceSize:"
                                                "toBuffer:destinationOffset:destinationBytesPerRow:"
                                                "destinationBytesPerImage:"),
                                           target, (NSUInteger)0, (NSUInteger)0, (MTLOrigin){0, 0, 0},
                                           (MTLSize){width, height, 1}, buf, (NSUInteger)0, row, len);
      msg0(void, blit, "endEncoding");
      msg0(void, cb, "commit");
      msg0(void, cb, "waitUntilCompleted");
      rgba->len = 0;
      buf_put(rgba, msg0(const u8 *, buf, "contents"), len);
    });
  release(buf);
  release(target);
  return ok;
}
