#include <civue/gpu.h>
#include <civue/ui.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) ((x) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x), exit(1)))

static civue_response button(civue_ui *ui, civue_input *in, bool down) {
  in->has_mouse = true;
  in->mouse = civue_v2(15, 15);
  in->pressed[0] = down && !in->down[0];
  in->released[0] = !down && in->down[0];
  in->down[0] = down;
  civue_begin_frame(ui, civue_v2(100, 100), 2, in);
  civue_response r = civue_interact(ui, civue_id_of("button"), civue_rect_xywh(10, 10, 20, 20), CIVUE_SENSE_CLICK);
  civue_fill(ui, r.rect, ui->theme.bg_brand, CIVUE_RADIUS_MEDIUM);
  civue_icon_centered(ui, CIVUE_ICON_CHECK, r.rect, 16, ui->theme.icon_onbrand);
  civue_text(ui, "ok", CIVUE_BODY, civue_v2(40, 10), ui->theme.text);
  civue_end_frame(ui);
  return r;
}

int main(void) {
  void *device = civue_gpu_device();
  if (!device) {
    puts("ui: no gpu, skipped");
    return 0;
  }
  civue_gfx *g = civue_gfx_new(device);
  civue_ui *ui = civue_ui_new(g, 2);
  civue_input in = {};
  button(ui, &in, false);
  CHECK(button(ui, &in, false).hovered);
  CHECK(button(ui, &in, true).pressed);
  civue_response up = button(ui, &in, false);
  CHECK(up.clicked && ui->repaint);
  CHECK(civue_frame_list(ui)->quads.len >= 3);

  civue_bytes px = {};
  CHECK(civue_gfx_snapshot(g, civue_frame_list(ui), 200, 200, CIVUE_WHITE, &px));
  const uint8_t *edge = px.v + (24 * 200 + 22) * 4;
  CHECK(edge[2] > 200 && edge[0] < 100);

  CHECK(civue_id_of("a") != civue_id_of("b") && civue_ids(civue_id_of("a"), "b") != civue_id_of("b"));
  int *count = civue_memory(ui, civue_id_of("count"), sizeof(int), nullptr);
  *count = 3;
  CHECK(*(int *)civue_memory(ui, civue_id_of("count"), sizeof(int), nullptr) == 3);
  free(px.v);
  civue_input_free(&in);
  civue_ui_free(ui);
  civue_gfx_free(g);
  puts("ui: ok");
  return 0;
}
