#include <civue/gfx.h>
#include <civue/gpu.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) ((x) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x), exit(1)))

static const uint8_t *at(const civue_bytes *px, uint32_t w, uint32_t x, uint32_t y) { return px->v + (y * w + x) * 4; }

int main(void) {
  void *device = civue_gpu_device();
  if (!device) {
    puts("gfx: no gpu, skipped");
    return 0;
  }
  civue_gfx *g = civue_gfx_new(device);
  civue_draw_list d = {};
  civue_dl_fill(&d, civue_rect_xywh(10, 10, 40, 20), 0, civue_hex(0xff0000));
  civue_dl_push_clip(&d, civue_rect_xywh(60, 0, 10, 64));
  civue_dl_fill(&d, civue_rect_xywh(50, 10, 40, 20), 4, civue_hex(0x00ff00));
  civue_dl_pop_clip(&d);
  uint8_t checker[4 * 4];
  for (int i = 0; i < 16; i++) checker[i] = i % 2 ? 255 : 0;
  float uv[4];
  CHECK(civue_atlas_insert(civue_gfx_atlas(g), 4, 4, checker, uv));
  civue_dl_mask(&d, civue_rect_xywh(10, 40, 4, 4), uv, CIVUE_WHITE, 1);

  civue_bytes px = {};
  CHECK(civue_gfx_snapshot(g, &d, 96, 64, CIVUE_BLACK, &px));
  CHECK(px.len == 96 * 64 * 4);
  const uint8_t *red = at(&px, 96, 30, 20), *green = at(&px, 96, 65, 20), *outside = at(&px, 96, 55, 20);
  CHECK(red[0] == 255 && red[1] == 0 && red[2] == 0 && red[3] == 255);
  CHECK(green[0] == 0 && green[1] == 255 && green[2] == 0);
  CHECK(outside[0] == 0 && outside[1] == 0 && outside[2] == 0);
  CHECK(at(&px, 96, 11, 40)[0] == 255 && at(&px, 96, 10, 40)[0] == 0);
  free(px.v);
  civue_dl_free(&d);
  civue_gfx_free(g);
  puts("gfx: ok");
  return 0;
}
