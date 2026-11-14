#import <Metal/Metal.h>

#include "darwin.h"

#include <civue/gfx.h>

#include <stdatomic.h>

extern const unsigned char civue_ui_metallib[];
extern const size_t civue_ui_metallib_len;

// the buffer slot of the quads, past the globals in slot 0.
constexpr NSUInteger QUAD_SLOT = 1;

// a buffer for a frame's quads, kept for the next frames instead of made and
// freed each time. it is busy from encoding until the gpu is done with it.
typedef struct {
  id<MTLBuffer> buffer;
  NSUInteger capacity;
  _Atomic bool busy;
} QuadBuffer;

constexpr u32 QUAD_BUFFERS = 4;

struct civue_gfx {
  id<MTLDevice> device;
  id<MTLCommandQueue> queue;
  QuadBuffer quad_buffers[QUAD_BUFFERS];
  id<MTLFunction> vs, fs;
  // the pipelines by their pixel format, then whether they multiply.
  NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pipelines;
  id<MTLSamplerState> linear, nearest;
  civue_atlas atlas, color_atlas;
  id<MTLTexture> atlas_texture;
  // made when the first color glyph arrives, most screens have none.
  id<MTLTexture> color_atlas_texture;
  id<MTLTexture> dummy;
  NSMutableDictionary<NSNumber *, id<MTLTexture>> *images;
  civue_texture next_id;
};

static id<MTLTexture> texture_stored(id<MTLDevice> device,
                                     MTLPixelFormat format, u32 w, u32 h,
                                     MTLTextureUsage usage,
                                     MTLStorageMode storage) {
  MTLTextureDescriptor *d =
    [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                       width:MAX(w, 1u)
                                                      height:MAX(h, 1u)
                                                   mipmapped:NO];
  d.usage = usage;
  d.storageMode = storage;
  return [device newTextureWithDescriptor:d];
}

static id<MTLTexture> texture(id<MTLDevice> device, MTLPixelFormat format,
                              u32 w, u32 h, MTLTextureUsage usage) {
  return texture_stored(device, format, w, h, usage, MTLStorageModePrivate);
}

// a texture the cpu writes its pixels into. the driver keeps about 90 mb
// around for as long as blits are encoded, however small they are, so
// uploads go through replaceRegion instead.
static id<MTLTexture> written_texture(id<MTLDevice> device,
                                      MTLPixelFormat format, u32 w, u32 h) {
  return texture_stored(device, format, w, h, MTLTextureUsageShaderRead,
                        MTLStorageModeShared);
}

static id<MTLSamplerState> sampler(id<MTLDevice> device,
                                   MTLSamplerMinMagFilter filter) {
  MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
  d.minFilter = filter;
  d.magFilter = filter;
  d.sAddressMode = MTLSamplerAddressModeClampToEdge;
  d.tAddressMode = MTLSamplerAddressModeClampToEdge;
  return [device newSamplerStateWithDescriptor:d];
}

civue_gfx *civue_gfx_new(void *device) {
  civue_gfx *g = xcalloc(1, sizeof *g);
  g->device = (__bridge id<MTLDevice>)device;
  g->queue = [g->device newCommandQueue];

  dispatch_data_t data =
    dispatch_data_create(civue_ui_metallib, civue_ui_metallib_len, nullptr,
                         DISPATCH_DATA_DESTRUCTOR_DEFAULT);
  NSError *error = nil;
  id<MTLLibrary> lib = [g->device newLibraryWithData:data error:&error];

  if (!lib) {
    die("ui shader: %s", error.localizedDescription.UTF8String);
  }

  g->vs = [lib newFunctionWithName:@"ui_vs"];
  g->fs = [lib newFunctionWithName:@"ui_fs"];
  g->pipelines = [NSMutableDictionary dictionary];
  g->images = [NSMutableDictionary dictionary];
  g->linear = sampler(g->device, MTLSamplerMinMagFilterLinear);
  g->nearest = sampler(g->device, MTLSamplerMinMagFilterNearest);
  g->atlas.bytes_per_pixel = 1;
  g->color_atlas.bytes_per_pixel = 4;
  g->atlas_texture = written_texture(g->device, MTLPixelFormatR8Unorm,
                                     CIVUE_ATLAS_SIZE, CIVUE_ATLAS_SIZE);
  g->dummy = texture(g->device, MTLPixelFormatRGBA8Unorm, 1, 1,
                     MTLTextureUsageShaderRead);
  g->next_id = 1;
  return g;
}

void civue_gfx_free(civue_gfx *g) {
  if (!g) {
    return;
  }

  civue_atlas_free(&g->atlas);
  civue_atlas_free(&g->color_atlas);
  // assigning releases the objects it held.
  *g = (civue_gfx){};
  free(g);
}

void *civue_gfx_device(civue_gfx *g) {
  return (__bridge void *)g->device;
}

void *civue_gfx_queue(civue_gfx *g) {
  return (__bridge void *)g->queue;
}

civue_atlas *civue_gfx_atlas(civue_gfx *g) {
  return &g->atlas;
}

civue_atlas *civue_gfx_color_atlas(civue_gfx *g) {
  return &g->color_atlas;
}

static id<MTLRenderPipelineState> pipeline(civue_gfx *g, MTLPixelFormat format,
                                           bool multiply) {
  NSNumber *key = @((NSUInteger)format << 1 | multiply);
  id<MTLRenderPipelineState> state = g->pipelines[key];

  if (state) {
    return state;
  }

  MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
  d.vertexFunction = g->vs;
  d.fragmentFunction = g->fs;

  MTLVertexDescriptor *v = [MTLVertexDescriptor vertexDescriptor];

  for (NSUInteger i = 0; i < 8; i++) {
    v.attributes[i].format = MTLVertexFormatFloat4;
    v.attributes[i].offset = i * 16;
    v.attributes[i].bufferIndex = QUAD_SLOT;
  }

  v.layouts[QUAD_SLOT].stride = sizeof(civue_quad);
  v.layouts[QUAD_SLOT].stepFunction = MTLVertexStepFunctionPerInstance;
  d.vertexDescriptor = v;

  // premultiplied colors. multiplying is the destination times the source,
  // plus what the source's coverage leaves of it.
  MTLRenderPipelineColorAttachmentDescriptor *att = d.colorAttachments[0];
  att.pixelFormat = format;
  att.blendingEnabled = YES;
  att.sourceRGBBlendFactor =
    multiply ? MTLBlendFactorDestinationColor : MTLBlendFactorOne;
  att.sourceAlphaBlendFactor = MTLBlendFactorOne;
  att.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  att.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

  NSError *error = nil;
  state = [g->device newRenderPipelineStateWithDescriptor:d error:&error];

  if (!state) {
    die("ui pipeline: %s", error.localizedDescription.UTF8String);
  }

  g->pipelines[key] = state;
  return state;
}

civue_texture civue_gfx_register_texture(civue_gfx *g, void *t) {
  civue_texture tid = g->next_id++;
  g->images[@(tid)] = (__bridge id<MTLTexture>)t;
  return tid;
}

void civue_gfx_update_texture(civue_gfx *g, civue_texture tid, void *t) {
  if (g->images[@(tid)]) {
    g->images[@(tid)] = (__bridge id<MTLTexture>)t;
  }
}

void civue_gfx_free_texture(civue_gfx *g, civue_texture tid) {
  [g->images removeObjectForKey:@(tid)];
}

civue_texture civue_gfx_upload_image(civue_gfx *g, u32 w, u32 h,
                                     const u8 *rgba) {
  id<MTLTexture> t =
    written_texture(g->device, MTLPixelFormatRGBA8Unorm, w, h);

  if (w && h) {
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h)
         mipmapLevel:0
           withBytes:rgba
         bytesPerRow:(NSUInteger)w * 4];
  }

  return civue_gfx_register_texture(g, (__bridge void *)t);
}

// a free buffer of at least `len` bytes, marked busy, or nullptr when the
// gpu still holds them all.
static QuadBuffer *quad_buffer(civue_gfx *g, NSUInteger len) {
  for (u32 i = 0; i < QUAD_BUFFERS; i++) {
    QuadBuffer *q = &g->quad_buffers[i];

    if (atomic_load(&q->busy)) {
      continue;
    }

    if (q->capacity < len) {
      NSUInteger cap = MAX(len + len / 2, (NSUInteger)64 * 1024);
      q->buffer = [g->device newBufferWithLength:cap
                                         options:MTLResourceStorageModeShared];
      q->capacity = q->buffer ? cap : 0;

      if (!q->buffer) {
        return nullptr;
      }
    }

    atomic_store(&q->busy, true);
    return q;
  }

  return nullptr;
}

// the glyphs and icons new this frame.
static void upload_atlases(civue_gfx *g) {
  civue_atlas *atlases[2] = {&g->atlas, &g->color_atlas};
  id<MTLTexture> textures[2] = {g->atlas_texture, g->color_atlas_texture};

  for (u32 k = 0; k < 2; k++) {
    civue_atlas *a = atlases[k];

    vec_each(&a->pending, p) {
      if (p->w && p->h) {
        [textures[k] replaceRegion:MTLRegionMake2D(p->x, p->y, p->w, p->h)
                       mipmapLevel:0
                         withBytes:p->data.v
                       bytesPerRow:(NSUInteger)p->w * a->bytes_per_pixel];
      }

      vec_free(&p->data);
    }

    a->pending.len = 0;
  }
}

void civue_gfx_encode(civue_gfx *g, void *command_buffer, void *target_texture,
                      u32 width, u32 height, const civue_draw_list *list,
                      const civue_color *clear) {
  id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)command_buffer;
  id<MTLTexture> target = (__bridge id<MTLTexture>)target_texture;

  if (g->color_atlas.pending.len && !g->color_atlas_texture) {
    g->color_atlas_texture =
      written_texture(g->device, MTLPixelFormatRGBA8Unorm, CIVUE_ATLAS_SIZE,
                      CIVUE_ATLAS_SIZE);
  }

  upload_atlases(g);

  MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
  MTLRenderPassColorAttachmentDescriptor *att = rp.colorAttachments[0];
  att.texture = target;
  att.storeAction = MTLStoreActionStore;

  if (clear) {
    civue_color c = *clear;
    att.loadAction = MTLLoadActionClear;
    att.clearColor = MTLClearColorMake(c.r * c.a, c.g * c.a, c.b * c.a, c.a);
  }
  else {
    att.loadAction = MTLLoadActionLoad;
  }

  MTLPixelFormat format = target.pixelFormat;
  id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];

  if (!enc) {
    return;
  }

  if (list->quads.len) {
    NSUInteger len = list->quads.len * sizeof(civue_quad);
    QuadBuffer *slot = quad_buffer(g, len);
    id<MTLBuffer> quads;

    if (slot) {
      quads = slot->buffer;
      memcpy(quads.contents, list->quads.v, len);
      [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
        atomic_store(&slot->busy, false);
      }];
    }
    else {
      quads = [g->device newBufferWithBytes:list->quads.v
                                     length:len
                                    options:MTLResourceStorageModeShared];
    }

    f32 globals[4] = {(f32)width, (f32)height, 0, 0};
    [enc setRenderPipelineState:pipeline(g, format, false)];
    [enc setVertexBytes:globals length:sizeof globals atIndex:0];
    [enc setFragmentBytes:globals length:sizeof globals atIndex:0];
    [enc setVertexBuffer:quads offset:0 atIndex:QUAD_SLOT];
    [enc setFragmentTexture:g->atlas_texture atIndex:0];
    [enc setFragmentTexture:g->color_atlas_texture ?: g->dummy atIndex:2];
    [enc setFragmentSamplerState:g->linear atIndex:0];
    [enc setFragmentSamplerState:g->nearest atIndex:1];

    bool multiplying = false;

    for (u32 b = 0; b < list->batches.len; b++) {
      const civue_batch *batch = &list->batches.v[b];
      u32 end = b + 1 < list->batches.len ? list->batches.v[b + 1].start
                                          : list->quads.len;

      if (batch->multiply != multiplying) {
        multiplying = batch->multiply;
        [enc setRenderPipelineState:pipeline(g, format, multiplying)];
      }

      id<MTLTexture> image =
        batch->texture ? g->images[@(batch->texture)] : nil;
      [enc setFragmentTexture:image ?: g->dummy atIndex:1];
      [enc drawPrimitives:MTLPrimitiveTypeTriangleStrip
              vertexStart:0
              vertexCount:4
            instanceCount:end - batch->start
             baseInstance:batch->start];
    }
  }

  [enc endEncoding];
}

bool civue_gfx_snapshot(civue_gfx *g, const civue_draw_list *list, u32 width,
                        u32 height, civue_color clear, civue_bytes *rgba) {
  @autoreleasepool {
    id<MTLTexture> target =
      texture(g->device, MTLPixelFormatRGBA8Unorm, width, height,
              MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
    NSUInteger row = (NSUInteger)width * 4, len = row * height;
    id<MTLBuffer> buf =
      [g->device newBufferWithLength:len options:MTLResourceStorageModeShared];

    if (!target || !buf) {
      return false;
    }

    id<MTLCommandBuffer> cb = [g->queue commandBuffer];
    civue_gfx_encode(g, (__bridge void *)cb, (__bridge void *)target, width,
                     height, list, &clear);

    id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];
    [blit copyFromTexture:target
                   sourceSlice:0
                   sourceLevel:0
                  sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake(width, height, 1)
                      toBuffer:buf
             destinationOffset:0
        destinationBytesPerRow:row
      destinationBytesPerImage:len];
    [blit endEncoding];
    [cb commit];
    [cb waitUntilCompleted];

    rgba->len = 0;
    buf_put(rgba, buf.contents, len);
    return true;
  }
}
