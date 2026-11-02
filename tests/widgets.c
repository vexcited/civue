#include <civue/field.h>
#include <civue/gpu.h>
#include <civue/numfield.h>
#include <civue/widgets.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) ((x) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x), exit(1)))

// one frame with the pointer at `at`, the primary button `down`, and `text` typed.
static void frame(civue_ui *ui, civue_input *in, civue_vec2 at, bool down, const char *text, bool *checked,
                  char **value) {
  in->has_mouse = true;
  in->mouse = at;
  in->pressed[0] = down && !in->down[0];
  in->released[0] = !down && in->down[0];
  in->down[0] = down;
  for (const char *c = text; c && *c; c++) {
    if (in->text.len == in->text.cap) {
      in->text.cap = in->text.cap ? in->text.cap * 2 : 16;
      in->text.v = realloc(in->text.v, in->text.cap);
    }
    in->text.v[in->text.len++] = (uint8_t)*c;
  }
  civue_begin_frame(ui, civue_v2(300, 200), 2, in);
  if (civue_checkbox(ui, civue_id_of("check"), civue_v2(10, 10), *checked, "Snap")) *checked = !*checked;
  civue_text_field(ui, civue_id_of("name"), civue_rect_xywh(10, 40, 160, 24), value, "Name", CIVUE_BODY);
  civue_button(ui, civue_id_of("ok"), civue_rect_xywh(10, 80, 60, 24), "OK", CIVUE_BUTTON_PRIMARY);
  civue_end_frame(ui);
}

int main(void) {
  double v;
  CHECK(civue_eval_num("2*(3+4)", nullptr, &v) && v == 14);
  double now = 10;
  CHECK(civue_eval_num("*2", &now, &v) && v == 20);
  CHECK(!civue_eval_num("two", nullptr, &v));
  char shown[32];
  civue_num_show(1.5, shown, sizeof shown);
  CHECK(!strcmp(shown, "1.5"));
  CHECK(fabsf(civue_ease(0.5f) - 0.8024034f) < 1e-4f && fabsf(civue_ease(1) - 1) < 1e-3f);

  void *device = civue_gpu_device();
  if (!device) {
    puts("widgets: no gpu, skipped");
    return 0;
  }
  civue_gfx *g = civue_gfx_new(device);
  civue_ui *ui = civue_ui_new(g, 2);
  civue_input in = {};
  bool checked = false;
  char *value = nullptr;
  frame(ui, &in, civue_v2(15, 15), false, nullptr, &checked, &value);
  frame(ui, &in, civue_v2(15, 15), true, nullptr, &checked, &value);
  frame(ui, &in, civue_v2(15, 15), false, nullptr, &checked, &value);
  CHECK(checked);
  frame(ui, &in, civue_v2(40, 52), true, nullptr, &checked, &value);
  frame(ui, &in, civue_v2(40, 52), false, nullptr, &checked, &value);
  frame(ui, &in, civue_v2(40, 52), false, "ada", &checked, &value);
  CHECK(value && !strcmp(value, "ada"));

  civue_bytes px = {};
  CHECK(civue_gfx_snapshot(g, civue_frame_list(ui), 600, 400, CIVUE_WHITE, &px));
  uint32_t ink = 0;
  for (uint32_t i = 0; i < px.len; i += 4) ink += px.v[i] < 128;
  CHECK(ink > 200);
  free(px.v);
  free(value);
  civue_input_free(&in);
  civue_ui_free(ui);
  civue_gfx_free(g);
  puts("widgets: ok");
  return 0;
}
