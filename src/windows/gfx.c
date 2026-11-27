#include "../internal.h"

#include <civue/gfx.h>
#include <civue/gpu.h>
#include <civue/raster.h>

#include <string.h>

// windows draws on the cpu alone: an image is its pixels, kept for
// civue_raster.
typedef struct {
  civue_texture id;
  u8 *pixels;
  u32 width, height;
  usize stride;
  // the app's own pixels, from civue_gfx_register_texture, not freed here.
  bool borrowed;
} Image;

struct civue_gfx {
  civue_atlas atlas, color_atlas;
  Vec(Image) images;
  civue_texture next_id;
};

void *civue_gpu_device() {
  return nullptr;
}

void civue_gpu_set_name(const char *) {
}

void civue__gpu_title(const char *) {
}

civue_gfx *civue_gfx_new(void *) {
  civue_gfx *g = xcalloc(1, sizeof *g);
  g->atlas.bytes_per_pixel = 1;
  g->color_atlas.bytes_per_pixel = 4;
  g->atlas.keeps = g->color_atlas.keeps = true;
  g->next_id = 1;
  return g;
}

void civue_gfx_free(civue_gfx *g) {
  if (!g) {
    return;
  }

  vec_each(&g->images, i) {
    if (!i->borrowed) {
      free(i->pixels);
    }
  }

  vec_free(&g->images);
  civue_atlas_free(&g->atlas);
  civue_atlas_free(&g->color_atlas);
  free(g);
}

void *civue_gfx_device(civue_gfx *) {
  return nullptr;
}

void *civue_gfx_queue(civue_gfx *) {
  return nullptr;
}

civue_atlas *civue_gfx_atlas(civue_gfx *g) {
  return &g->atlas;
}

civue_atlas *civue_gfx_color_atlas(civue_gfx *g) {
  return &g->color_atlas;
}

static Image *image(civue_gfx *g, civue_texture tid) {
  vec_each(&g->images, i) {
    if (i->id == tid) {
      return i;
    }
  }

  return nullptr;
}

// `texture` is a civue_pixels the app keeps alive and may draw into.
civue_texture civue_gfx_register_texture(civue_gfx *g, void *texture) {
  const civue_pixels *p = texture;
  civue_texture tid = g->next_id++;
  vec_push(&g->images, ((Image){tid, (u8 *)p->pixels, p->width, p->height,
                                p->stride, true}));
  return tid;
}

void civue_gfx_update_texture(civue_gfx *g, civue_texture tid, void *texture) {
  Image *i = image(g, tid);

  if (!i) {
    return;
  }

  if (!i->borrowed) {
    free(i->pixels);
  }

  const civue_pixels *p = texture;
  *i = (Image){tid, (u8 *)p->pixels, p->width, p->height, p->stride, true};
}

void civue_gfx_free_texture(civue_gfx *g, civue_texture tid) {
  Image *i = image(g, tid);

  if (!i) {
    return;
  }

  if (!i->borrowed) {
    free(i->pixels);
  }

  *i = vec_last(&g->images);
  g->images.len--;
}

civue_texture civue_gfx_upload_image(civue_gfx *g, u32 w, u32 h,
                                     const u8 *rgba) {
  civue_texture tid = g->next_id++;
  usize len = (usize)w * h * 4;
  u8 *pixels = xmalloc(len ? len : 1);
  memcpy(pixels, rgba, len);
  vec_push(&g->images, ((Image){tid, pixels, w, h, (usize)w * 4, false}));
  return tid;
}

bool civue_gfx_pixels(civue_gfx *g, civue_texture tid, civue_pixels *out) {
  Image *i = image(g, tid);

  if (!i || !i->pixels) {
    return false;
  }

  *out = (civue_pixels){i->pixels, i->width, i->height, i->stride};
  return true;
}

bool civue_gfx_static(civue_gfx *g, const civue_draw_list *list) {
  vec_each(&list->batches, b) {
    Image *i = b->texture ? image(g, b->texture) : nullptr;

    if (i && i->borrowed) {
      return false;
    }
  }

  return true;
}

static bool find_pixels(void *ctx, civue_texture t, civue_pixels *out) {
  return civue_gfx_pixels(ctx, t, out);
}

// `target` is a civue_surface, drawn whole.
void civue_gfx_encode(civue_gfx *g, void *, void *target, u32 width,
                      u32 height, const civue_draw_list *list,
                      const civue_color *clear) {
  civue_raster_images images = {find_pixels, g};
  civue_damage all = {.all = true};
  civue_raster(target, list, clear ? *clear : (civue_color){}, &all,
               &g->atlas, &g->color_atlas, &images);
  (void)width;
  (void)height;
}

bool civue_gfx_snapshot(civue_gfx *g, const civue_draw_list *list, u32 width,
                        u32 height, civue_color clear, civue_bytes *rgba) {
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
