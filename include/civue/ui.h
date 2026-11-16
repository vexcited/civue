#ifndef CIVUE_UI_H
#define CIVUE_UI_H

#include <civue/gfx.h>
#include <civue/icons.h>
#include <civue/raster.h>
#include <civue/text.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* the immediate mode ui: input, interaction, animation and painting helpers. */
/* a frame only runs when input, an animation or the app asks. coordinates    */
/* are logical points unless said otherwise.                                  */
/* ========================================================================== */

// whether the app looks light or dark: as the system does, or one of them
// whatever the system does.
typedef enum : uint8_t {
  CIVUE_APPEARANCE_SYSTEM,
  CIVUE_APPEARANCE_LIGHT,
  CIVUE_APPEARANCE_DARK
} civue_appearance;

// identifies a widget across frames, made from a name and the ids of what holds
// it.
typedef uint64_t civue_id;

static inline civue_id civue_id_of(const char *s) {
  return civue_hash_str(s) | 1;
}

static inline civue_id civue_idn(civue_id parent, uint64_t v) {
  return civue_hash_u64(parent * 0x9e3779b97f4a7c15ull ^ civue_hash_u64(v)) | 1;
}

static inline civue_id civue_ids(civue_id parent, const char *s) {
  return civue_idn(parent, civue_hash_str(s));
}

enum {
  CIVUE_MOD_SHIFT = 1,
  CIVUE_MOD_CTRL = 2,
  CIVUE_MOD_ALT = 4,
  CIVUE_MOD_CMD = 8
};

// keys are characters by their lowercase code point, the others past unicode.
// digits come from the digit row and the keypad whatever the layout.
enum : uint32_t {
  CIVUE_KEY_NONE = 0,
  CIVUE_KEY_ENTER = 0x110000,
  CIVUE_KEY_ESCAPE,
  CIVUE_KEY_TAB,
  CIVUE_KEY_BACKSPACE,
  CIVUE_KEY_DELETE,
  CIVUE_KEY_LEFT,
  CIVUE_KEY_RIGHT,
  CIVUE_KEY_UP,
  CIVUE_KEY_DOWN,
  CIVUE_KEY_HOME,
  CIVUE_KEY_END,
  CIVUE_KEY_PAGE_UP,
  CIVUE_KEY_PAGE_DOWN,
  CIVUE_KEY_SPACE,
  CIVUE_KEY_F1,
  CIVUE_KEY_F12 = CIVUE_KEY_F1 + 11,
};

typedef struct {
  uint32_t key;
  uint8_t mods;
} civue_key_press;

// the pointer's look. an app draws cursors of its own through
// civue_window_options.cursor_image, by kind and angle, for these or for kinds
// of its own from CIVUE_CURSOR_CUSTOM. the system's show where it draws none.
typedef uint8_t civue_cursor_kind;

enum : civue_cursor_kind {
  CIVUE_CURSOR_DEFAULT,
  CIVUE_CURSOR_POINTER,
  CIVUE_CURSOR_TEXT,
  CIVUE_CURSOR_RESIZE_H,
  CIVUE_CURSOR_RESIZE_V,
  // a resize that can only go right, or left.
  CIVUE_CURSOR_RESIZE_E,
  CIVUE_CURSOR_RESIZE_W,
  CIVUE_CURSOR_ARROW,
  CIVUE_CURSOR_HAND,
  CIVUE_CURSOR_GRABBING,
  CIVUE_CURSOR_ZOOM_IN,
  CIVUE_CURSOR_ZOOM_OUT,
  CIVUE_CURSOR_COPY,
  CIVUE_CURSOR_CROSSHAIR,
  // a resize handle or beyond a corner, at an angle in degrees clockwise from
  // the right as seen from the center of what is resized.
  CIVUE_CURSOR_RESIZE,
  CIVUE_CURSOR_ROTATE,
  CIVUE_CURSOR_CUSTOM = 32,
};

typedef struct {
  civue_cursor_kind kind;
  int16_t angle;
} civue_cursor;

typedef struct {
  civue_vec2 at;
  // a stylus's, negative for a mouse or trackpad.
  float pressure;
} civue_trail_point;

// what happened since the last frame.
typedef struct {
  bool has_mouse;
  civue_vec2 mouse;
  bool down[3], pressed[3], released[3];
  // clicks in a row of the last press, 2 for a double click.
  uint32_t clicks;
  // positive when content should move down or right.
  civue_vec2 scroll;
  // from a trackpad rather than a wheel.
  bool precise_scroll;
  // trackpad pinch, positive when zooming in.
  float pinch;
  uint8_t mods;
  CIVUE_VEC(civue_key_press) keys;
  // what the system's menu bar asked the app for, its own numbers, see
  // civue_window_send_command.
  civue_u32s commands;
  // keys held down.
  civue_u32s held;
  // text typed, utf-8.
  civue_bytes text;
  // text being composed by an input method, empty when none.
  civue_bytes preedit;
  // paths of files dropped on the window, owned. whoever takes one frees it
  // and removes it, the rest are freed at the next frame.
  civue_strs drops;
  // where the files in `drops` landed.
  civue_vec2 drop_at;
  // every place the pointer moved to, in order.
  CIVUE_VEC(civue_trail_point) trail;
  float pressure;
} civue_input;

// what a widget did with the pointer this frame.
typedef struct {
  civue_rect rect;
  bool hovered;
  // the primary button went down on it this frame.
  bool pressed;
  // pressed and released over it.
  bool clicked;
  bool double_clicked;
  bool right_clicked;
  // held since a press on it.
  bool dragging;
  civue_vec2 drag_delta;
  bool released;
} civue_response;

typedef enum : uint8_t {
  CIVUE_SENSE_HOVER,
  CIVUE_SENSE_CLICK,
  CIVUE_SENSE_DRAG
} civue_sense;

typedef enum : uint8_t {
  CIVUE_ALIGN_START,
  CIVUE_ALIGN_CENTER,
  CIVUE_ALIGN_END
} civue_align;

typedef struct {
  civue_id id;
  civue_rect rect;
  bool overlay;
} civue_region;

typedef struct {
  civue_id id;
  float from, to, duration;
  double start, used;
} civue_anim;

// a button of the title bar the app draws: the menus' or the window's.
typedef enum : uint8_t {
  CIVUE_TITLE_NONE,
  CIVUE_TITLE_MENU,
  CIVUE_TITLE_MINIMIZE,
  CIVUE_TITLE_MAXIMIZE,
  CIVUE_TITLE_CLOSE
} civue_title_button;

typedef struct civue_ui civue_ui;
typedef struct civue_scroll_state civue_scroll_state;
typedef struct civue_tree civue_tree;
typedef struct civue_scrollbar_look civue_scrollbar_look;

struct civue_ui {
  civue_gfx *gfx;
  civue_appearance appearance;
  // whether the system looks dark, and this frame does, by `appearance`. the
  // window follows `dark`.
  bool system_dark, dark;
  civue_text_system *text;
  civue_icons *icons;
  civue_draw_list base, overlay, combined;
  bool on_overlay;
  float scale;
  civue_vec2 size;
  civue_input input;
  bool had_mouse;
  civue_vec2 last_mouse;
  // frames begun, to tell a fresh showing from a kept one.
  uint64_t frames;
  // this pass builds again a frame whose layout moved: what an app does once
  // a frame, it does when this is false.
  bool rerun;
  bool settled;
  // the scroll areas begun this frame.
  uint32_t scroll_areas;
  // what the last frame shown drew, over which color and at which size.
  civue_draw_list last;
  civue_color last_clear;
  uint32_t last_width, last_height;
  bool has_last;
  // what took the pointer this frame and the last.
  CIVUE_VEC(civue_region) regions, prev_regions;
  civue_id hovered, active;
  civue_vec2 press_origin;
  civue_id focus;
  bool focus_alive;
  // a press this frame leaves the focus where it is.
  bool focus_kept;
  // the control the keyboard moved to, a focus ring around it.
  civue_id ring;
  bool has_ring_rect;
  civue_rect ring_rect;
  uint8_t ring_press;
  CIVUE_VEC(civue_anim) anims;
  // id to a block made by civue_memory.
  civue_map memory;
  // the nodes of <civue/layout.h>, made when the first opens.
  civue_tree *tree;
  bool repaint;
  // when a frame is wanted, 0 for none, in civue_now seconds.
  double repaint_at;
  civue_cursor cursor;
  double now;
  // the window should start a move from the current press.
  bool drag_window;
  bool toggle_maximize, toggle_fullscreen;
  // the room kept for the window's buttons at the start of the title bar.
  float titlebar_inset;
  // how tall the window's title bar is, the app's tab bar going in it. 0
  // for a window with the system's own.
  float titlebar_height;
  bool window_focused;
  // the system's glass shows through the title bar where nothing is drawn.
  bool titlebar_glass;
  // the app draws the window's buttons in the title bar, where the system
  // draws none, and what they ask for goes to the window: at its start then
  // at its end, in the system's order.
  bool window_controls, window_maximized;
  civue_title_button titlebar_start[4], titlebar_end[4];
  bool minimize_window, close_window;
  // the bars of the scroll areas without a look of their own, none when null.
  const civue_scrollbar_look *scrollbars;
#ifdef CIVUE_UI_FIELDS
  CIVUE_UI_FIELDS
#endif
};

// the ui whose frame this thread builds, from civue_begin_frame on.
extern thread_local civue_ui *civue__ui;

static inline civue_ui *civue_ui_current() {
  return civue__ui;
}

// a ui of `size` bytes, zeroed: sizeof(civue_ui) where the app includes civue.
// an app keeps fields of its own at the end of civue_ui by defining
// CIVUE_UI_FIELDS as their declarations before including civue, the same in
// every file that does, and civue never reads them.
civue_ui *civue_ui_new_sized(civue_gfx *gfx, float scale, size_t size);
#define civue_ui_new(gfx, scale) \
  civue_ui_new_sized(gfx, scale, sizeof(civue_ui))
void civue_ui_free(civue_ui *ui);
// starts a frame of `size` points. the input is taken, `input` left with
// what carries over: buttons held, the pointer and the modifiers.
void civue_begin_frame(civue_ui *ui, civue_vec2 size, float scale,
                       civue_input *input);
// solves the frame's nodes, false when they moved from where they were painted
// and the frame should run again, without the input, before it shows.
bool civue_end_frame(civue_ui *ui);
// both draw lists of the frame as one.
const civue_draw_list *civue_frame_list(civue_ui *ui);
// the frame's list, and what it changes of the one the last call returned for
// the same `clear`, `width` and `height` in pixels: nothing for a frame the
// window shows already, that needs no drawing. the list is kept until the
// next call. civue_frame_forget makes the next one change everything.
const civue_draw_list *civue_frame_changes(civue_ui *ui, civue_color clear,
                                           uint32_t width, uint32_t height,
                                           civue_damage *damage);
void civue_frame_forget(civue_ui *ui);
void civue_input_free(civue_input *in);

// what took the pointer at `p` in the last frame, topmost first, from
// between frames: a window asks whether a press there moves it.
civue_id civue_region_at(const civue_ui *ui, civue_vec2 p);
void civue_request_repaint(civue_ui *ui);
void civue_request_repaint_after(civue_ui *ui, double seconds);

/* ========================================================================== */
/* layers.                                                                    */
/* ========================================================================== */

// what draws until the matching end goes above everything, for menus.
bool civue_begin_overlay(civue_ui *ui);
void civue_end_overlay(civue_ui *ui, bool was);
civue_draw_list *civue_list(civue_ui *ui);
// restricts drawing to `r` until civue_pop_clip.
void civue_push_clip(civue_ui *ui, civue_rect r);
void civue_pop_clip(civue_ui *ui);
civue_rect civue_clip_rect(civue_ui *ui);

/* ========================================================================== */
/* interaction.                                                               */
/* ========================================================================== */

// registers `rect` for the pointer and reports what it did.
civue_response civue_interact(civue_ui *ui, civue_id id, civue_rect rect,
                              civue_sense sense);
// whether `id` was topmost under the pointer last frame, or holds it.
bool civue_owns_pointer(civue_ui *ui, civue_id id);
bool civue_drawn_last_frame(civue_ui *ui, civue_id id);
bool civue_under_overlay(civue_ui *ui, civue_vec2 p);
bool civue_pointer_moved(civue_ui *ui);
void civue_claim_focus(civue_ui *ui);
void civue_keep_focus(civue_ui *ui);
void civue_press_ring(civue_ui *ui);
bool civue_key_pressed(civue_ui *ui, uint32_t key);
bool civue_key_held(civue_ui *ui, uint32_t key);
// a shortcut's label as the platform writes it, from the mac's glyphs: "⇧⌘K"
// on macos, "Ctrl+Shift+K" elsewhere. never freed.
const char *civue_shortcut_label(const char *mac);
// a shortcut with the platform command modifier.
bool civue_shortcut(civue_ui *ui, uint32_t key, bool shift);

/* ========================================================================== */
/* state.                                                                     */
/* ========================================================================== */

// eases a value toward `target` over `seconds`, asking frames while it moves.
float civue_animate(civue_ui *ui, civue_id id, float target, float seconds);
// like civue_animate, a new value starting from `from`.
float civue_animate_from(civue_ui *ui, civue_id id, float from, float target,
                         float seconds);
// widget state kept across frames: `size` zeroed bytes on first use, the
// same block after. `drop` frees what it holds when forgotten.
void *civue_memory(civue_ui *ui, civue_id id, size_t size,
                   void (*drop)(void *));
void civue_forget(civue_ui *ui, civue_id id);
// css's cubic-bezier timing function at `t`, and its `ease`.
float civue_cubic_bezier(float x1, float y1, float x2, float y2, float t);
float civue_ease(float t);

/* ========================================================================== */
/* painting, snapped to pixels.                                               */
/* ========================================================================== */

void civue_fill(civue_ui *ui, civue_rect r, civue_color c, float radius);
void civue_fill_corners(civue_ui *ui, civue_rect r, civue_color c,
                        const float radii[4]);
// radii top left first and clockwise.
void civue_stroke_corners(civue_ui *ui, civue_rect r, civue_color c,
                          float width, const float radii[4]);
// an inside border of its own width on each side, from the top clockwise,
// each at least a device pixel wide unless 0.
void civue_stroke_sides(civue_ui *ui, civue_rect r, civue_color c,
                        const float widths[4], const float radii[4]);
// an inside border, at least a device pixel wide.
void civue_stroke(civue_ui *ui, civue_rect r, civue_color c, float width,
                  float radius);
void civue_fill_stroke(civue_ui *ui, civue_rect r, civue_color fill,
                       civue_color stroke, float width, float radius);
void civue_shadow(civue_ui *ui, civue_rect r, float radius, float blur,
                  civue_vec2 offset, civue_color c);
void civue_hline(civue_ui *ui, double x0, double x1, double y, civue_color c);
void civue_vline(civue_ui *ui, double x, double y0, double y1, civue_color c);
// a segment, not snapped.
void civue_line(civue_ui *ui, civue_vec2 a, civue_vec2 b, float width,
                civue_color c);
// a line with square ends, a turned rectangle `width` across.
void civue_band(civue_ui *ui, civue_vec2 a, civue_vec2 b, float width,
                civue_color c);
void civue_image(civue_ui *ui, civue_texture t, civue_rect r, const float uv[4],
                 float opacity, float radius, bool nearest);
// an image placed in physical pixels.
void civue_image_px(civue_ui *ui, civue_texture t, civue_rect r,
                    const float uv[4], bool nearest);
// `color` over `r` from clear at its top to opaque at its bottom, in bands two
// points tall.
void civue_fade_out(civue_ui *ui, civue_rect r, civue_color color);

// an icon filling `r`.
void civue_icon_at(civue_ui *ui, civue_icon icon, civue_rect r, civue_color c);
// an icon centered in `r`, `size` points square unless made for a size of
// its own.
void civue_icon_centered(civue_ui *ui, civue_icon icon, civue_rect r,
                         float size, civue_color c);
// the same turned left to right.
void civue_icon_mirrored(civue_ui *ui, civue_icon icon, civue_rect r,
                         float size, civue_color c);

/* ========================================================================== */
/* text.                                                                      */
/* ========================================================================== */

const civue_text_block *civue_layout(civue_ui *ui, const char *s,
                                     civue_text_style style, float max_width);
const civue_text_block *civue_layout_n(civue_ui *ui, const char *s, size_t n,
                                       civue_text_style style, float max_width);
void civue_draw_text(civue_ui *ui, const civue_text_block *b, civue_vec2 origin,
                     civue_color c);
// one line at `origin`, its width returned.
float civue_text(civue_ui *ui, const char *s, civue_text_style style,
                 civue_vec2 origin, civue_color c);
// one line centered vertically in `r`, cut with an ellipsis when too long.
float civue_text_in(civue_ui *ui, const char *s, civue_text_style style,
                    civue_rect r, civue_color c, civue_align align);
// `s` on one line, shortened with an ellipsis to fit `max`.
const civue_text_block *civue_fit(civue_ui *ui, const char *s,
                                  civue_text_style style, float max);

static inline void civue_set_cursor(civue_ui *ui, civue_cursor_kind k) {
  ui->cursor = (civue_cursor){k, 0};
}

/* ========================================================================== */
/* blocks. each runs its body once between a begin and an end, the end        */
/* running however the body is left: at its close, a `break`, a `continue` or */
/* a `return`.                                                                */
/* ========================================================================== */

#define CIVUE__CAT_(a, b) a##b
#define CIVUE__CAT(a, b) CIVUE__CAT_(a, b)
#define CIVUE__SCOPE CIVUE__CAT(civue__scope_, __LINE__)

typedef struct {
  civue_ui *ui;
  bool open, was;
} civue_scope;

static inline civue_scope civue__clip_begin(civue_ui *ui, civue_rect r) {
  civue_push_clip(ui, r);
  return (civue_scope){.ui = ui, .open = true};
}

static inline void civue__clip_end(civue_scope *s) {
  civue_pop_clip(s->ui);
}

static inline civue_scope civue__overlay_begin(civue_ui *ui) {
  return (civue_scope){ui, true, civue_begin_overlay(ui)};
}

static inline void civue__overlay_end(civue_scope *s) {
  civue_end_overlay(s->ui, s->was);
}

// draws its body clipped to `r`.
#define CIVUE_CLIPPED(ui, r)                                         \
  for (civue_scope CIVUE__SCOPE                                      \
       [[gnu::cleanup(civue__clip_end)]] = civue__clip_begin(ui, r); \
       CIVUE__SCOPE.open; CIVUE__SCOPE.open = false)
// draws its body above everything else, for menus and popovers.
#define CIVUE_OVERLAY(ui)                                               \
  for (civue_scope CIVUE__SCOPE                                         \
       [[gnu::cleanup(civue__overlay_end)]] = civue__overlay_begin(ui); \
       CIVUE__SCOPE.open; CIVUE__SCOPE.open = false)

#ifdef __cplusplus
}
#endif

#endif
