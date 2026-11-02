#include <civue/gfx.h>
#include <civue/gpu.h>
#include <civue/text.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) ((x) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x), exit(1)))

int main(void) {
  civue_text_system *t = civue_text_new();
  civue_text_style body = civue_style(11, 450, 16, 0.055f);
  const char *s = "Layers  Assets été 👋";
  const civue_text_block *b = civue_text_layout(t, s, strlen(s), body, 0, 2);
  CHECK(b->width > 60 && b->width < 200 && b->height == 16 && b->nlines == 1);
  CHECK(b->ncarets > 0 && b->carets[b->ncarets - 1].byte == strlen(s));
  CHECK(civue_text_layout(t, s, strlen(s), body, 0, 2) == b);
  const civue_text_block *wrapped = civue_text_layout(t, s, strlen(s), body, 40, 2);
  CHECK(wrapped->nlines > 1 && wrapped->height == 16 * wrapped->nlines);
  CHECK(civue_text_layout(t, "abc", 3, civue_style_mono(body), 0, 2)->width > 0);

  void *device = civue_gpu_device();
  if (device) {
    civue_gfx *g = civue_gfx_new(device);
    civue_draw_list d = {};
    civue_text_draw(t, &d, civue_gfx_atlas(g), civue_gfx_color_atlas(g), b, civue_v2(0, 0), CIVUE_WHITE, nullptr, 0);
    CHECK(d.quads.len == b->nglyphs - 4);
    civue_bytes px = {};
    CHECK(civue_gfx_snapshot(g, &d, 400, 40, CIVUE_BLACK, &px));
    uint32_t lit = 0;
    for (uint32_t i = 0; i < px.len; i += 4) lit += px.v[i] > 128;
    CHECK(lit > 100);
    free(px.v);
    civue_dl_free(&d);
    civue_gfx_free(g);
  }
  civue_text_end_frame(t);
  civue_text_free(t);
  puts("text: ok");
  return 0;
}
