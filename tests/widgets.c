#include <civue/field.h>
#include <civue/gpu.h>
#include <civue/numfield.h>
#include <civue/widgets.h>

#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) ((x) ? (void)0 : (fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x), exit(1)))

static civue_ui *ui;
static civue_input in;
static double clock_now = 100;

static void at(double x, double y, bool down) {
  in.has_mouse = true;
  in.mouse = civue_v2(x, y);
  in.pressed[0] = down && !in.down[0];
  in.released[0] = !down && in.down[0];
  in.down[0] = down;
}

static void key(uint32_t k, uint8_t mods) {
  if (in.keys.len == in.keys.cap) {
    in.keys.cap = in.keys.cap ? in.keys.cap * 2 : 8;
    in.keys.v = realloc(in.keys.v, in.keys.cap * sizeof *in.keys.v);
  }
  in.keys.v[in.keys.len++] = (civue_key_press){k, mods};
}

static void type(const char *s) {
  for (const char *c = s; *c; c++) {
    if (in.text.len == in.text.cap) {
      in.text.cap = in.text.cap ? in.text.cap * 2 : 16;
      in.text.v = realloc(in.text.v, in.text.cap);
    }
    in.text.v[in.text.len++] = (uint8_t)*c;
  }
}

static void begin(void) {
  civue_begin_frame(ui, civue_v2(300, 200), 2, &in);
  ui->now = clock_now;
  clock_now += .125;
}

static void buttons(void) {
  civue_rect r = civue_rect_xywh(10, 10, 60, 24);
  civue_id id = civue_id_of("ok");
  civue_button_state b = {};
  // a click activates it, held with the pointer over it on the way.
  for (int f = 0; f < 3; f++) {
    at(20, 20, f == 1);
    begin();
    b = civue_button(ui, id, r);
    CHECK(b.response.hovered && b.held == (f == 1) && b.activated == (f == 2));
    civue_end_frame(ui);
  }
  // a press doesn't take the focus, enter and space activate it once the app gives it.
  CHECK(ui->focus != id);
  ui->focus = id;
  key(CIVUE_KEY_SPACE, 0);
  begin();
  b = civue_button(ui, id, r);
  CHECK(b.focused && b.activated && !in.keys.len && !ui->input.keys.len);
  civue_end_frame(ui);
  begin();
  CHECK(!civue_button(ui, id, r).activated);
  civue_end_frame(ui);
  // the ring presses it too.
  ui->focus = 0;
  ui->ring = id;
  civue_press_ring(ui);
  bool pressed = false;
  for (int f = 0; f < 3; f++) {
    begin();
    b = civue_button(ui, id, r);
    pressed |= b.activated;
    CHECK(b.ring);
    civue_end_frame(ui);
  }
  CHECK(pressed && ui->has_ring_rect);
  ui->ring = 0;

  bool on = false;
  for (int f = 0; f < 3; f++) {
    at(20, 50, f == 1);
    begin();
    civue_toggle_state t = civue_toggle(ui, civue_id_of("snap"), civue_rect_xywh(10, 40, 80, 20), on);
    CHECK(t.changed == (f == 2));
    on = t.on;
    civue_end_frame(ui);
  }
  CHECK(on);
}

static void sliders(void) {
  civue_rect r = civue_rect_xywh(10, 0, 200, 40), track = civue_rect_xywh(20, 18, 180, 4);
  civue_id id = civue_id_of("volume");
  at(110, 20, true);
  begin();
  civue_slider_state s = civue_slider(ui, id, r, track, 0, 0, 100, 25, 5);
  CHECK(s.changed && s.value == 50 && !s.ended && s.x == 20 && s.anchor_x == 65);
  civue_end_frame(ui);
  at(110, 20, false);
  begin();
  s = civue_slider(ui, id, r, track, 50, 0, 100, 25, 5);
  CHECK(!s.changed && s.ended && s.x == 110);
  civue_end_frame(ui);
  ui->focus = id;
  key(CIVUE_KEY_RIGHT, 0);
  key(CIVUE_KEY_RIGHT, CIVUE_MOD_SHIFT);
  begin();
  s = civue_slider(ui, id, r, track, 50, 0, 100, 25, 5);
  CHECK(s.focused && s.changed && s.ended && s.value == 105 - 5 && !ui->input.keys.len);
  civue_end_frame(ui);
  key(CIVUE_KEY_HOME, 0);
  begin();
  CHECK(civue_slider(ui, id, r, track, 50, 0, 100, 25, 5).value == 0);
  civue_end_frame(ui);
  ui->focus = 0;
}

static void groups(void) {
  civue_rect cells[3];
  civue_response resp[3];
  for (int i = 0; i < 3; i++) cells[i] = civue_rect_xywh(10 + i * 30, 100, 30, 24);
  civue_id id = civue_id_of("align");
  int32_t chosen = -1;
  for (int f = 0; f < 3; f++) {
    at(75, 110, f == 1);
    begin();
    civue_group_state g = civue_radio_group(ui, id, cells, resp, 3, 0);
    if (g.chosen >= 0) chosen = g.chosen;
    CHECK(resp[2].hovered && !resp[0].hovered);
    civue_end_frame(ui);
  }
  CHECK(chosen == 2);
  // the arrows move the choice around, wrapping.
  ui->focus = id;
  key(CIVUE_KEY_RIGHT, 0);
  begin();
  civue_group_state g = civue_radio_group(ui, id, cells, resp, 3, 2);
  CHECK(g.focused && g.chosen == 0 && g.current == 0);
  civue_end_frame(ui);
  key(CIVUE_KEY_LEFT, 0);
  key(CIVUE_KEY_LEFT, 0);
  begin();
  CHECK(civue_radio_group(ui, id, cells, resp, 3, 0).chosen == 1);
  civue_end_frame(ui);
  ui->focus = 0;
}

static civue_scroll_state painted;
static void bars([[maybe_unused]] civue_ui *u, const civue_scroll_state *s) { painted = *s; }

static void scrolling(void) {
  civue_id id = civue_id_of("list");
  civue_rect r = civue_rect_xywh(10, 10, 200, 100);
  at(50, 50, false);
  in.scroll = civue_v2(0, -40);
  in.precise_scroll = true;
  begin();
  civue_rect c = civue_scroll_area(ui, id, r, 400);
  CHECK(c.y0 == 10 - 40);
  civue_scroll_state s = civue_scroll_end(ui);
  CHECK(s.vertical.shown && !s.horizontal.shown && s.show == 1 && !s.vertical.lit);
  CHECK(s.vertical.track.x0 == 200 && s.vertical.thumb.x0 == 202 && s.vertical.thumb.y1 - s.vertical.thumb.y0 == 24);
  civue_end_frame(ui);
  // over the bar, it lights, and ui->scrollbars paints it.
  ui->scrollbars = bars;
  at(205, 60, false);
  begin();
  CIVUE_SCROLLING(ui, id, r, 400, content) CHECK(content.y0 == 10 - 40);
  CHECK(painted.id == id && painted.vertical.lit);
  civue_end_frame(ui);
  ui->scrollbars = nullptr;
  // the thumb dragged scrolls by the content's height over the track's.
  at(205, 26, true);
  begin();
  civue_scroll_area(ui, id, r, 400);
  civue_scroll_end(ui);
  civue_end_frame(ui);
  at(205, 26 + 8, true);
  begin();
  c = civue_scroll_area(ui, id, r, 400);
  s = civue_scroll_end(ui);
  CHECK(c.y0 == 10 - 40 - 37.5);
  civue_end_frame(ui);
  at(205, 34, false);
  begin();
  civue_scroll_area(ui, id, r, 400);
  civue_scroll_end(ui);
  civue_end_frame(ui);
}

typedef struct {
  int rows;
  bool surface;
} Painted;

static civue_vec2 measure([[maybe_unused]] civue_ui *u, [[maybe_unused]] void *ctx, const civue_menu_item *items,
                          uint32_t count) {
  float h = 16;
  for (uint32_t i = 0; i < count; i++) h += items[i].height;
  return civue_v2(120, h);
}
static void surface([[maybe_unused]] civue_ui *u, void *ctx, [[maybe_unused]] civue_rect r) {
  ((Painted *)ctx)->surface = true;
}
static bool row([[maybe_unused]] civue_ui *u, void *ctx, [[maybe_unused]] const civue_menu_item *items,
                [[maybe_unused]] uint32_t count, [[maybe_unused]] const civue_menu_row *r) {
  ((Painted *)ctx)->rows++;
  return false;
}

static void menus(void) {
  static const civue_menu_item children[] = {{.height = 24}, {.height = 24}};
  static const civue_menu_item items[] = {{.height = 24, .disabled = true},
                                          {.header = true, .height = 24},
                                          {.height = 24},
                                          {.separator = true, .height = 17},
                                          {.height = 24, .submenu = children, .nsubmenu = 2}};
  Painted p = {};
  civue_menu_look look = {&p, measure, surface, row, nullptr};
  civue_menu_options o = civue_menu_options_default();
  o.look = &look;
  o.focus_first = true;
  civue_vec2 size = measure(ui, nullptr, items, 5);
  civue_id id = civue_id_of("menu");
  civue_rect anchor = civue_rect_xywh(10, 10, 100, 0);
  uint32_t path[CIVUE_MENU_MAX_DEPTH], depth;
  bool open = true;
  in.has_mouse = false;
  begin();
  CHECK(!civue_menu(ui, id, anchor, size, &open, items, 5, &o, path, &depth));
  CHECK(p.surface && p.rows == 5 && open);
  civue_end_frame(ui);
  // the keys skip what can't be chosen, enter chooses.
  key(CIVUE_KEY_DOWN, 0);
  key(CIVUE_KEY_ENTER, 0);
  begin();
  CHECK(!civue_menu(ui, id, anchor, size, &open, items, 5, &o, path, &depth));
  civue_end_frame(ui);
  key(CIVUE_KEY_UP, 0);
  key(CIVUE_KEY_ENTER, 0);
  begin();
  CHECK(civue_menu(ui, id, anchor, size, &open, items, 5, &o, path, &depth));
  CHECK(depth == 1 && path[0] == 2 && !open);
  civue_end_frame(ui);
  // hovering the row of a submenu opens it, a click in it chooses down the path.
  open = true;
  o.focus_first = false;
  for (int f = 0; f < 3; f++) {
    at(40, 10 + 4 + 8 + 24 * 3 + 17 + 12, false);
    begin();
    civue_menu(ui, id, anchor, size, &open, items, 5, &o, path, &depth);
    civue_end_frame(ui);
  }
  double sub_y = 10 + 4 + 8 + 24 * 3 + 17 + 24 + 12;
  for (int f = 0; f < 4; f++) {
    at(10 + 120 + 20, sub_y, f == 2);
    begin();
    bool chose = civue_menu(ui, id, anchor, size, &open, items, 5, &o, path, &depth);
    CHECK(chose == (f == 3));
    civue_end_frame(ui);
  }
  CHECK(depth == 2 && path[0] == 4 && path[1] == 1 && !open);
  // a select menu lines its selected row up with the field.
  open = true;
  at(40, 160, false);
  begin();
  civue_select_menu(ui, civue_id_of("select"), civue_rect_xywh(30, 150, 100, 24), 30, size, 2, &open, items, 5, &o);
  CHECK(civue_region_at(ui, civue_v2(40, 160)) == civue_idn(civue_id_of("select"), 2));
  civue_end_frame(ui);
  key(CIVUE_KEY_ESCAPE, 0);
  begin();
  civue_select_menu(ui, civue_id_of("select"), civue_rect_xywh(30, 150, 100, 24), 30, size, 2, &open, items, 5, &o);
  CHECK(!open);
  civue_end_frame(ui);
}

static void tooltips(void) {
  civue_response resp = {.hovered = true, .rect = civue_rect_xywh(100, 20, 40, 20)};
  for (int f = 0; f < 7; f++) {
    begin();
    CHECK(civue_tooltip(ui, civue_id_of("tip"), &resp) == (f >= 4));
    civue_end_frame(ui);
  }
  begin();
  civue_tooltip_place p = civue_tooltip_at(ui, resp.rect, civue_v2(60.5, 24), CIVUE_TOOLTIP_BELOW, 8);
  CHECK(p.side == CIVUE_TOOLTIP_BELOW && p.rect.x0 == 90 && p.rect.y0 == 48);
  p = civue_tooltip_at(ui, civue_rect_xywh(100, 170, 40, 20), civue_v2(60, 24), CIVUE_TOOLTIP_BELOW, 8);
  CHECK(p.side == CIVUE_TOOLTIP_ABOVE && p.rect.y1 == 162);
  p = civue_tooltip_at(ui, resp.rect, civue_v2(60, 24), CIVUE_TOOLTIP_RIGHT, 8);
  CHECK(p.side == CIVUE_TOOLTIP_RIGHT && p.rect.x0 == 148 && p.rect.y0 == 18);
  resp.dragging = true;
  CHECK(!civue_tooltip(ui, civue_id_of("tip"), &resp));
  civue_end_frame(ui);
}

static void fields(civue_gfx *g) {
  civue_field_box box = {CIVUE_WHITE, civue_hex(0x0d99ff), 1, 5, CIVUE_BLACK};
  civue_field_look look = {.pad = 8,
                           .editing = box,
                           .idle = box,
                           .hovered = box,
                           .placeholder = civue_hex(0x999999),
                           .selection = civue_hex(0xbde3ff)};
  char *value = nullptr;
  civue_rect r = civue_rect_xywh(10, 40, 160, 24);
  const char *typed[] = {nullptr, nullptr, nullptr, "ada"};
  for (int f = 0; f < 4; f++) {
    at(40, 52, f == 1);
    type(typed[f] ? typed[f] : "");
    begin();
    civue_text_field_with(ui, civue_id_of("name"), r, &value, "Name", civue_style(11, 450, 16, 0), look);
    civue_end_frame(ui);
  }
  CHECK(value && !strcmp(value, "ada"));
  // the test paints it, its text dark on white.
  civue_bytes px = {};
  CHECK(civue_gfx_snapshot(g, civue_frame_list(ui), 600, 400, CIVUE_WHITE, &px));
  uint32_t ink = 0;
  for (uint32_t i = 0; i < px.len; i += 4) ink += px.v[i] < 128;
  CHECK(ink > 100);
  free(px.v);
  free(value);

  // a number field steps with the arrows while it is typed into.
  civue_num_look nl = {.style = civue_style(11, 450, 16, 0), .field = look};
  civue_id id = civue_id_of("width");
  civue_num_change c = {};
  for (int f = 0; f < 4; f++) {
    at(60, 110, f == 1);
    if (f == 3) key(CIVUE_KEY_UP, CIVUE_MOD_SHIFT);
    begin();
    c = civue_num_field(ui, id, civue_rect_xywh(10, 100, 100, 24), 24, 40, false, "40", civue_num_show, &nl);
    civue_end_frame(ui);
  }
  CHECK(c.kind == CIVUE_NUM_SET && c.value == 50);
  at(250, 190, true);
  begin();
  civue_end_frame(ui);
  at(250, 190, false);
  begin();
  civue_end_frame(ui);
  // its label scrubs.
  for (int f = 0; f < 3; f++) {
    at(15 + f * 4, 110, f < 2);
    begin();
    c = civue_num_field(ui, id, civue_rect_xywh(10, 100, 100, 24), 24, 40, false, "40", civue_num_show, &nl);
    CHECK(f != 1 || (c.kind == CIVUE_NUM_SCRUB && c.value == 44));
    CHECK(f != 2 || c.kind == CIVUE_NUM_SCRUB_END);
    civue_end_frame(ui);
  }
  // steppers round to the step and stop at the ends.
  civue_rect up = civue_rect_xywh(200, 100, 24, 12), down = civue_rect_xywh(200, 112, 24, 12);
  civue_steppers_state s = {};
  for (int f = 0; f < 2; f++) {
    at(210, 104, f == 1);
    begin();
    s = civue_steppers(ui, civue_id_of("steps"), up, down, 9.5, 1, 10, 0, 10);
    civue_end_frame(ui);
  }
  CHECK(s.stepped && s.value == 10 && s.up.dragging && !s.at_max);
  at(210, 104, false);
  begin();
  s = civue_steppers(ui, civue_id_of("steps"), up, down, 10, 1, 10, 0, 10);
  CHECK(!s.stepped && s.at_max);
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
  CHECK(fabsf(civue_cubic_bezier(0, 0, 1, 1, .25f) - .25f) < 1e-5f);

  void *device = civue_gpu_device();
  if (!device) {
    puts("widgets: no gpu, skipped");
    return 0;
  }
  civue_gfx *g = civue_gfx_new(device);
  ui = civue_ui_new(g, 2);
  buttons();
  sliders();
  groups();
  scrolling();
  menus();
  tooltips();
  fields(g);
  civue_input_free(&in);
  civue_ui_free(ui);
  civue_gfx_free(g);
  puts("widgets: ok");
  return 0;
}
