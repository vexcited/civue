#include "darwin.h"
#include "window.h"

#include <civue/gpu.h>

#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdlib.h>

enum : NSUInteger {
  STYLE_TITLED = 1,
  STYLE_CLOSABLE = 2,
  STYLE_MINIATURIZABLE = 4,
  STYLE_RESIZABLE = 8,
  STYLE_FULL_SIZE_CONTENT = 1 << 15,
  BACKING_BUFFERED = 2,
  TRACK_ENTERED_EXITED = 1,
  TRACK_MOUSE_MOVED = 2,
  TRACK_ACTIVE_ALWAYS = 0x80,
  TRACK_IN_VISIBLE_RECT = 0x200,
  PIXEL_BGRA8 = 80,
  FLAG_SHIFT = 1 << 17,
  FLAG_CONTROL = 1 << 18,
  FLAG_OPTION = 1 << 19,
  FLAG_COMMAND = 1 << 20,
  EVENT_TABLET_POINT = 1,
  FULL_SCREEN = 1 << 14,
  RESIZE_WIDTH = 2,
  RESIZE_HEIGHT = 16,
  RESIZE_BOTTOM_MARGIN = 8,
  MATERIAL_TITLEBAR = 3,
  EFFECT_ACTIVE = 1,
};

typedef struct {
  float minimum, maximum, preferred;
} CAFrameRateRange;

extern id NSRunLoopCommonModes;
extern id NSAppearanceNameAqua, NSAppearanceNameDarkAqua;
constexpr NSUInteger NOT_FOUND = (NSUInteger)LONG_MAX;

// the one window and what drives it.
static struct {
  id app, window, view, layer, link;
  // the title bar's material under the app's title bar.
  id glass;
  // full screen hides the app's title bar, the strip brings it back inside
  // the system's one: its view, the ui's view in it and that one's layer,
  // and what follows the title bar's layout. the app's title bar is as tall
  // as that row there.
  bool full_screen, strip_shown;
  id strip, strip_view, strip_layer;
  // follows appkit's layouts of the title bar, to put it right at once.
  id titlebar_observer;
  f64 strip_height;
  civue_ui *ui;
  civue_app app_cb;
  civue_window_options options;
  civue_input input;
  civue_gfx *gfx;
  // a frame waits for the display link's next drawable.
  bool wanted;
  u32 idle;
  // when the last press happened, where and how many in a row.
  f64 click_at;
  civue_vec2 click_pos;
  u32 clicks;
  civue_cursor cursor;
  bool cursor_set;
  // without a display link a frame is queued for the main thread.
  bool queued;
  // the appearance last given to the window, -1 for none yet.
  int window_dark;
  // a frame asked for at this time, 0 for none.
  f64 timer_at;
  id marked; // the input method's composed text, an NSString.
  // a live resize draws each size at once, in the resize's transaction.
  bool resizing;
} W = {.window_dark = -1};

static void draw_without_link(void);

static void want(void) {
  W.wanted = true;
  if (W.link && msg0(BOOL, W.link, "isPaused")) {
    W.idle = 0;
    msg1(void, W.link, "setPaused:", (BOOL)NO);
  } else if (!W.link && !W.queued) {
    W.queued = true;
    dispatch_async(dispatch_get_main_queue(), ^{
      W.queued = false;
      draw_without_link();
    });
  }
}

void civue_window_wake(void) {
  dispatch_async(dispatch_get_main_queue(), ^{ want(); });
}

void civue_window_send_key(civue_key_press k) {
  vec_push(&W.input.keys, k);
  want();
}

void civue_window_send_command(u32 command) {
  vec_push(&W.input.commands, command);
  want();
}

static f32 scale_factor(void) { return W.window ? (f32)msg0(CGFloat, W.window, "backingScaleFactor") : 1; }

static bool system_dark(void) {
  id appearance = msg0(id, W.app, "effectiveAppearance");
  const id list[] = {NSAppearanceNameAqua, NSAppearanceNameDarkAqua};
  id names = msg2(id, CLS("NSArray"), "arrayWithObjects:count:", (const id *)list, (NSUInteger)2);
  id best = msg1(id, appearance, "bestMatchFromAppearancesWithNames:", names);
  return best && msg1(BOOL, best, "isEqualToString:", NSAppearanceNameDarkAqua);
}

static void resize_layer(void) {
  if (!W.layer) return;
  NSRect b = msg0(NSRect, W.view, "bounds");
  f32 s = scale_factor();
  msg1(void, W.layer, "setContentsScale:", (CGFloat)s);
  CGSize size = {fmax(round(b.size.width * s), 1), fmax(round(b.size.height * s), 1)};
  msg1(void, W.layer, "setDrawableSize:", size);
}

// places the traffic lights in the app's title bar: the close button 12 points from the left, and from the top
// since macos 26, 11 before. appkit lays them out again on resizes, so this runs every frame and only moves what
// moved.
static void place_window_buttons(void) {
  id first = msg1(id, W.window, "standardWindowButton:", (NSUInteger)0);
  if (!first) return;
  static f64 y;
  if (!y) {
    typedef struct {
      NSInteger major, minor, patch;
    } OsVersion;
    id info = msg0(id, CLS("NSProcessInfo"), "processInfo");
    y = msg0(OsVersion, info, "operatingSystemVersion").major >= 26 ? 12 : 11;
  }
  f64 x = 12;
  id container = msg0(id, msg0(id, first, "superview"), "superview");
  if (!container) return;
  NSRect ff = msg0(NSRect, first, "frame");
  f64 height = ff.size.height + 2 * y;
  NSRect frame = msg0(NSRect, container, "frame");
  NSRect wf = msg0(NSRect, W.window, "frame");
  f64 top = wf.size.height - height;
  if (frame.size.height != height || frame.origin.y != top) {
    frame.size.height = height;
    frame.origin.y = top;
    msg1(void, container, "setFrame:", frame);
  }
  // appkit's own spacing, read before the lights are first moved: once moved,
  // appkit shifts the others along and a spacing read then grows each time.
  // the lights stay where they are until appkit has laid them out.
  static f64 spacing;
  if (!spacing) {
    id second = msg1(id, W.window, "standardWindowButton:", (NSUInteger)1);
    f64 apart = second ? msg0(NSRect, second, "frame").origin.x - ff.origin.x : 0;
    if (apart <= 0) return;
    spacing = apart;
  }
  for (NSUInteger i = 0; i < 3; i++) {
    id b = msg1(id, W.window, "standardWindowButton:", i);
    if (!b) continue;
    NSPoint at = msg0(NSRect, b, "frame").origin;
    NSPoint want_at = {x + i * spacing, y};
    if (at.x != want_at.x || at.y != want_at.y) msg1(void, b, "setFrameOrigin:", want_at);
  }
}

// the material spans the app's title bar.
static void place_glass(void) {
  if (!W.glass) return;
  NSRect b = msg0(NSRect, msg0(id, W.window, "contentView"), "bounds");
  f64 h = W.options.titlebar;
  NSRect want_at = {{0, b.size.height - h}, {b.size.width, h}};
  NSRect at = msg0(NSRect, W.glass, "frame");
  if (memcmp(&at, &want_at, sizeof at)) msg1(void, W.glass, "setFrame:", want_at);
}

// the window, its buttons and title bar follow the app's theme.
static void set_window_dark(bool dark) {
  id appearance =
      msg1(id, CLS("NSAppearance"), "appearanceNamed:", dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua);
  msg1(void, W.window, "setAppearance:", appearance);
}

static void set_cursor(civue_cursor c) {
  if (civue__cursor_apply(&W.options, c)) return;
  const char *name;
  switch (c.kind) {
  case CIVUE_CURSOR_POINTER: name = "pointingHandCursor"; break;
  case CIVUE_CURSOR_TEXT: name = "IBeamCursor"; break;
  case CIVUE_CURSOR_RESIZE_H: name = "resizeLeftRightCursor"; break;
  case CIVUE_CURSOR_RESIZE_V: name = "resizeUpDownCursor"; break;
  case CIVUE_CURSOR_RESIZE_E: name = "resizeRightCursor"; break;
  case CIVUE_CURSOR_RESIZE_W: name = "resizeLeftCursor"; break;
  case CIVUE_CURSOR_HAND: name = "openHandCursor"; break;
  case CIVUE_CURSOR_GRABBING: name = "closedHandCursor"; break;
  case CIVUE_CURSOR_CROSSHAIR: name = "crosshairCursor"; break;
  default: name = "arrowCursor"; break;
  }
  id cursor = ((id (*)(id, SEL))objc_msgSend)(CLS("NSCursor"), sel_registerName(name));
  msg0(void, cursor, "set");
}

static void schedule_timer(f64 at);

static void draw_strip(id cb, civue_ui *ui);
static f64 bar_height(void);

// builds a frame into `texture` and shows `drawable`.
static void frame(id texture, id drawable) {
  AUTORELEASE({
    f32 scale = scale_factor();
    u32 pw = (u32)msg0(NSUInteger, texture, "width");
    u32 ph = (u32)msg0(NSUInteger, texture, "height");
    civue_ui *ui = W.ui;
    civue_color background = W.app_cb.background ? W.app_cb.background(W.app_cb.ctx) : color_hex(0x1e1e1e);
    if (W.options.titlebar > 0) {
      if (W.window_dark != (int)ui->theme.dark) {
        W.window_dark = ui->theme.dark;
        set_window_dark(ui->theme.dark);
      }
      // in full screen the strip has the traffic lights, without it there
      // are none in the title bar nor room kept for them.
      bool full = msg0(NSUInteger, W.window, "styleMask") & FULL_SCREEN;
      if (!W.full_screen) {
        place_window_buttons();
        place_glass();
      }
      ui->titlebar_inset = full && !W.strip_shown ? 0 : 80;
      ui->titlebar_height = bar_height();
    }
    ui->system_dark = system_dark();
    ui->window_focused = msg0(BOOL, W.window, "isKeyWindow");
    civue_begin_frame(ui, v2(pw / scale, ph / scale), scale, &W.input);
    W.app_cb.frame(W.app_cb.ctx, ui);
    civue_end_frame(ui);
    id cb = msg0(id, civue_gfx_queue(W.gfx), "commandBuffer");
    civue_gfx_encode(W.gfx, cb, texture, pw, ph, civue_frame_list(ui), &background);
    draw_strip(cb, ui);
    // presented with the transaction, the frame shows with the window's
    // new size instead of after it.
    bool synced = W.resizing;
    if (!synced) msg1(void, cb, "presentDrawable:", drawable);
    msg0(void, cb, "commit");
    if (synced) {
      msg0(void, cb, "waitUntilScheduled");
      msg0(void, drawable, "present");
    }
    if (!W.cursor_set || ui->cursor.kind != W.cursor.kind || ui->cursor.angle != W.cursor.angle) {
      W.cursor = ui->cursor;
      W.cursor_set = true;
      set_cursor(ui->cursor);
    }
    // a full screen window neither moves nor zooms.
    if (ui->drag_window && !W.full_screen) {
      id e = msg0(id, W.app, "currentEvent");
      if (e) msg1(void, W.window, "performWindowDragWithEvent:", e);
    }
    if (ui->toggle_maximize && !W.full_screen) msg1(void, W.window, "zoom:", (id) nullptr);
    if (ui->toggle_fullscreen) {
      ui->toggle_fullscreen = false;
      msg1(void, W.window, "toggleFullScreen:", (id) nullptr);
    }
    if (ui->repaint) want();
    if (ui->repaint_at) schedule_timer(ui->repaint_at);
  });
}

// ---- display link ----

// shows a frame in the next drawable, unless it is of the window's old size.
static void show(id drawable) {
  CGSize want_size = msg0(CGSize, W.layer, "drawableSize");
  id texture = msg0(id, drawable, "texture");
  // a drawable of the old size would be stretched.
  if (msg0(NSUInteger, texture, "width") != (NSUInteger)want_size.width ||
      msg0(NSUInteger, texture, "height") != (NSUInteger)want_size.height) {
    want();
    return;
  }
  frame(texture, drawable);
}

static void link_update(id self, SEL sel, id link, id update) {
  if (!W.wanted) {
    // nothing to show, the link sleeps until a frame is wanted, not at
    // once since the next frame is often about to be.
    if (++W.idle > 8) msg1(void, link, "setPaused:", (BOOL)YES);
    return;
  }
  W.idle = 0;
  W.wanted = false;
  id drawable = msg0(id, update, "drawable");
  if (!drawable) return;
  show(drawable);
}

// draws a frame wanted before the display link exists, waiting for a free
// drawable.
static void draw_without_link(void) {
  if (!W.wanted) return;
  // a frame queued before the link existed is the link's to draw now, the
  // layer refuses nextDrawable once a link drives it.
  if (W.link) {
    W.idle = 0;
    msg1(void, W.link, "setPaused:", (BOOL)NO);
    return;
  }
  W.wanted = false;
  AUTORELEASE({
    id drawable = msg0(id, W.layer, "nextDrawable");
    if (drawable) show(drawable);
  });
}

// drives frames as fast as the display goes, a frame ahead.
static void start_link(void) {
  static id delegate;
  if (!delegate) {
    Class cls = class_new("CivueLinkDelegate", "NSObject");
    class_method(cls, "metalDisplayLink:needsUpdate:", (void *)link_update, "v@:@@");
    Protocol *p = objc_getProtocol("CAMetalDisplayLinkDelegate");
    if (p) class_addProtocol(cls, p);
    class_done(cls);
    delegate = NEW("CivueLinkDelegate");
  }
  W.link = msg1(id, msg0(id, CLS("CAMetalDisplayLink"), "alloc"), "initWithMetalLayer:", W.layer);
  msg1(void, W.link, "setDelegate:", delegate);
  id screen = msg0(id, W.window, "screen");
  float hz = screen ? (float)msg0(NSInteger, screen, "maximumFramesPerSecond") : 120;
  if (hz <= 0) hz = 60;
  msg1(void, W.link, "setPreferredFrameRateRange:", ((CAFrameRateRange){fminf(hz, 60), hz, hz}));
  msg1(void, W.link, "setPreferredFrameLatency:", 1.0f);
  msg1(void, W.link, "setPaused:", (BOOL)YES);
  msg2(void, W.link, "addToRunLoop:forMode:", msg0(id, CLS("NSRunLoop"), "mainRunLoop"), NSRunLoopCommonModes);
}

static void timer_fired(id self, SEL sel, id timer) {
  W.timer_at = 0;
  want();
}

static void schedule_timer(f64 at) {
  if (W.timer_at && W.timer_at <= at) return;
  W.timer_at = at;
  static id target;
  if (!target) {
    Class cls = class_new("CivueTimerTarget", "NSObject");
    class_method(cls, "fire:", (void *)timer_fired, "v@:@");
    class_done(cls);
    target = NEW("CivueTimerTarget");
  }
  f64 delay = fmax(at - now_s(), 0);
  id timer = msg5(id, CLS("NSTimer"), "timerWithTimeInterval:target:selector:userInfo:repeats:", delay, target,
                  SEL_("fire:"), (id) nullptr, (BOOL)NO);
  msg2(void, msg0(id, CLS("NSRunLoop"), "mainRunLoop"), "addTimer:forMode:", timer, NSRunLoopCommonModes);
}

// ---- input ----

// the strip shows the top of the ui, so its points are the ui's as they are.
static civue_vec2 event_point(id e) {
  bool strip = W.strip_shown && msg0(id, e, "window") == msg0(id, W.strip, "window");
  NSPoint p = msg0(NSPoint, e, "locationInWindow");
  NSPoint v = msg2(NSPoint, strip ? W.strip_view : W.view, "convertPoint:fromView:", p, (id) nullptr);
  return v2(v.x, v.y);
}

static f32 event_pressure(id e) {
  NSInteger type = msg0(NSInteger, e, "type");
  // only mouse events carry a subtype, appkit throws on others.
  if (type < 1 || type > 7) return -1;
  if (msg0(NSInteger, e, "subtype") != EVENT_TABLET_POINT) return -1;
  return fminf(fmaxf(msg0(float, e, "pressure"), 0), 1);
}

static u8 mods_of(NSUInteger flags) {
  return (flags & FLAG_SHIFT ? CIVUE_MOD_SHIFT : 0) | (flags & FLAG_CONTROL ? CIVUE_MOD_CTRL : 0) |
         (flags & FLAG_OPTION ? CIVUE_MOD_ALT : 0) | (flags & FLAG_COMMAND ? CIVUE_MOD_CMD : 0);
}

static void moved(id self, SEL sel, id e) {
  civue_vec2 at = event_point(e);
  W.input.has_mouse = true;
  W.input.mouse = at;
  W.input.pressure = event_pressure(e);
  vec_push(&W.input.trail, ((civue_trail_point){at, W.input.pressure}));
  W.input.mods = mods_of(msg0(NSUInteger, e, "modifierFlags"));
  want();
}

static void exited(id self, SEL sel, id e) {
  if (!W.input.down[0] && !W.input.down[1] && !W.input.down[2]) W.input.has_mouse = false;
  want();
}

static void civue_button(id e, u32 i, bool down) {
  moved(nullptr, nullptr, e);
  W.input.down[i] = down;
  if (!down) {
    W.input.released[i] = true;
    return;
  }
  W.input.pressed[i] = true;
  if (i == 0) {
    civue_vec2 p = W.input.mouse;
    bool near = fabs(p.x - W.click_pos.x) < 4 && fabs(p.y - W.click_pos.y) < 4;
    f64 t = now_s();
    W.clicks = near && t - W.click_at < 0.4 ? W.clicks + 1 : 1;
    W.click_at = t;
    W.click_pos = p;
    W.input.clicks = W.clicks;
  }
}

static void mouse_down(id self, SEL sel, id e) { civue_button(e, 0, true); }
static void mouse_up(id self, SEL sel, id e) { civue_button(e, 0, false); }
static void right_down(id self, SEL sel, id e) { civue_button(e, 1, true); }
static void right_up(id self, SEL sel, id e) { civue_button(e, 1, false); }
static void other_down(id self, SEL sel, id e) { civue_button(e, 2, true); }
static void other_up(id self, SEL sel, id e) { civue_button(e, 2, false); }

static void scroll_wheel(id self, SEL sel, id e) {
  f64 dx = msg0(CGFloat, e, "scrollingDeltaX");
  f64 dy = msg0(CGFloat, e, "scrollingDeltaY");
  if (msg0(BOOL, e, "hasPreciseScrollingDeltas")) {
    W.input.precise_scroll = true;
  } else {
    dx *= 40, dy *= 40;
  }
  W.input.scroll = v2_add(W.input.scroll, v2(dx, dy));
  W.input.mods = mods_of(msg0(NSUInteger, e, "modifierFlags"));
  want();
}

static void magnify(id self, SEL sel, id e) {
  W.input.pinch += (f32)msg0(CGFloat, e, "magnification");
  want();
}

static void flags_changed(id self, SEL sel, id e) {
  W.input.mods = mods_of(msg0(NSUInteger, e, "modifierFlags"));
  want();
}

static u32 special_key(u16 code) {
  switch (code) {
  case 36:
  case 76: return CIVUE_KEY_ENTER;
  case 53: return CIVUE_KEY_ESCAPE;
  case 48: return CIVUE_KEY_TAB;
  case 51: return CIVUE_KEY_BACKSPACE;
  case 117: return CIVUE_KEY_DELETE;
  case 123: return CIVUE_KEY_LEFT;
  case 124: return CIVUE_KEY_RIGHT;
  case 125: return CIVUE_KEY_DOWN;
  case 126: return CIVUE_KEY_UP;
  case 115: return CIVUE_KEY_HOME;
  case 119: return CIVUE_KEY_END;
  case 116: return CIVUE_KEY_PAGE_UP;
  case 121: return CIVUE_KEY_PAGE_DOWN;
  case 49: return CIVUE_KEY_SPACE;
  }
  static const u16 fkeys[] = {122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111};
  for (u32 i = 0; i < countof(fkeys); i++)
    if (fkeys[i] == code) return CIVUE_KEY_F1 + i;
  return CIVUE_KEY_NONE;
}

// the digit row gives its digit on any layout, shortcuts need them.
static u32 digit_key(u16 code) {
  static const u16 row[] = {29, 18, 19, 20, 21, 23, 22, 26, 28, 25};
  static const u16 pad[] = {82, 83, 84, 85, 86, 87, 88, 89, 91, 92};
  for (u32 i = 0; i < 10; i++)
    if (row[i] == code || pad[i] == code) return '0' + i;
  return CIVUE_KEY_NONE;
}

// the key a shifted symbol of a us layout is typed with.
static u32 unshifted(u32 c) {
  static const char base[] = "`1234567890-=[]\\;',./";
  static const char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";
  for (u32 i = 0; shifted[i]; i++)
    if ((u32)shifted[i] == c) return (u32)base[i];
  return c;
}

static u32 lower(u32 c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

// the key an event stands for, by the key itself rather than what option
// makes of it.
static u32 key_of(id e, bool shift) {
  u16 code = msg0(unsigned short, e, "keyCode");
  u32 k = special_key(code);
  if (k) return k;
  k = digit_key(code);
  if (k) return k;
  const char *s = ns_cstr(msg0(id, e, "charactersIgnoringModifiers"));
  usize i = 0;
  u32 c = *s ? utf8_next(s, strlen(s), &i) : 0;
  if (!c || c < 0x20 || (c >= 0xf700 && c <= 0xf8ff)) return CIVUE_KEY_NONE;
  c = lower(c);
  return shift ? unshifted(c) : c;
}

static void key_down(id self, SEL sel, id e) {
  u8 mods = mods_of(msg0(NSUInteger, e, "modifierFlags"));
  W.input.mods = mods;
  u32 key = key_of(e, mods & CIVUE_MOD_SHIFT);
  if (key) {
    bool held = false;
    vec_each(&W.input.held, h) held |= *h == key;
    if (!held) vec_push(&W.input.held, key);
    vec_push(&W.input.keys, ((civue_key_press){key, mods}));
  }
  // typed text goes through the input method, unless a shortcut takes it.
  if (!(mods & (CIVUE_MOD_CMD | CIVUE_MOD_CTRL))) {
    const id list[] = {e};
    id events = msg2(id, CLS("NSArray"), "arrayWithObjects:count:", (const id *)list, (NSUInteger)1);
    msg1(void, self, "interpretKeyEvents:", events);
  }
  want();
}

static void key_up(id self, SEL sel, id e) {
  u32 key = key_of(e, msg0(NSUInteger, e, "modifierFlags") & FLAG_SHIFT);
  u32 k = 0;
  vec_each(&W.input.held, h) if (*h != key) W.input.held.v[k++] = *h;
  W.input.held.len = k;
  want();
}

// ---- text input ----

static const char *string_of(id s) {
  // attributed strings come too.
  if (msg1(BOOL, s, "isKindOfClass:", CLS("NSAttributedString"))) s = msg0(id, s, "string");
  return ns_cstr(s);
}

static void insert_text(id self, SEL sel, id s, NSRange r) {
  const char *t = string_of(s);
  for (const char *c = t; *c; c++)
    if ((u8)*c >= 0x20 && *c != 0x7f) vec_push(&W.input.text, (u8)*c);
  W.input.preedit.len = 0;
  release(W.marked);
  W.marked = nullptr;
  want();
}

static void set_marked(id self, SEL sel, id s, NSRange sr, NSRange rr) {
  const char *t = string_of(s);
  W.input.preedit.len = 0;
  buf_put(&W.input.preedit, t, strlen(t));
  release(W.marked);
  W.marked = retain(msg1(BOOL, s, "isKindOfClass:", CLS("NSAttributedString")) ? msg0(id, s, "string") : s);
  want();
}

static void unmark(id self, SEL sel) {
  W.input.preedit.len = 0;
  release(W.marked);
  W.marked = nullptr;
}

static BOOL has_marked(id self, SEL sel) { return W.marked != nullptr; }

static NSRange marked_range(id self, SEL sel) {
  return W.marked ? (NSRange){0, msg0(NSUInteger, W.marked, "length")} : (NSRange){NOT_FOUND, 0};
}

static NSRange selected_range(id self, SEL sel) { return (NSRange){0, 0}; }

static id valid_attributes(id self, SEL sel) { return msg0(id, CLS("NSArray"), "array"); }

static id substring(id self, SEL sel, NSRange r, NSRange *actual) { return nullptr; }

static NSUInteger char_index(id self, SEL sel, NSPoint p) { return NOT_FOUND; }

static NSRect first_rect(id self, SEL sel, NSRange r, NSRange *actual) {
  // the candidates window opens under the pointer.
  NSRect wf = msg0(NSRect, W.window, "frame");
  civue_vec2 m = W.input.mouse;
  NSRect b = msg0(NSRect, W.view, "bounds");
  return (NSRect){{wf.origin.x + m.x, wf.origin.y + b.size.height - m.y - 20}, {1, 20}};
}

static void do_command(id self, SEL sel, SEL command) {}

// ---- view ----

static BOOL yes(id self, SEL sel) { return YES; }

static id make_backing_layer(id self, SEL sel) { return retain(W.layer); }

static void set_frame_size(id self, SEL sel, NSSize size) {
  struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
  ((void (*)(struct objc_super *, SEL, NSSize))objc_msgSendSuper)(&sup, sel, size);
  resize_layer();
  want();
  if (W.resizing) draw_without_link();
}

static void backing_changed(id self, SEL sel) {
  resize_layer();
  want();
  if (W.resizing) draw_without_link();
}

// the display link would show a resized frame a refresh late, the old one
// stretched to the new size meanwhile, so a live resize or a move in and out
// of full screen draws without it. the layer gives no drawables while a link
// drives it, even paused.
static void sync_resizes(bool on) {
  if (W.resizing == on) return;
  W.resizing = on;
  if (on && W.link) {
    msg0(void, W.link, "invalidate");
    release(W.link);
    W.link = nullptr;
  }
  msg1(void, W.layer, "setPresentsWithTransaction:", (BOOL)on);
  if (!on) {
    start_link();
    want();
  }
}

static void live_resize_began(id self, SEL sel) {
  struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
  ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel);
  sync_resizes(true);
}

static void live_resize_ended(id self, SEL sel) {
  struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
  ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel);
  sync_resizes(false);
}

static void appearance_changed(id self, SEL sel) { want(); }

static void update_tracking(id self, SEL sel) {
  id areas = msg0(id, self, "trackingAreas");
  for (NSUInteger i = ns_count(areas); i-- > 0;) msg1(void, self, "removeTrackingArea:", ns_at(areas, i));
  id area = msg4(id, msg0(id, CLS("NSTrackingArea"), "alloc"), "initWithRect:options:owner:userInfo:", (NSRect){},
                 (NSUInteger)(TRACK_ENTERED_EXITED | TRACK_MOUSE_MOVED | TRACK_ACTIVE_ALWAYS | TRACK_IN_VISIBLE_RECT),
                 self, (id) nullptr);
  msg1(void, self, "addTrackingArea:", area);
  release(area);
  struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
  ((void (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, sel);
}

// files dragged onto the window: what a comment's upload button asks for.
static NSUInteger drag_entered(id self, SEL sel, id info) { return 1; }

static BOOL drag_performed(id self, SEL sel, id info) {
  id board = msg0(id, info, "draggingPasteboard");
  NSPoint where = msg0(NSPoint, info, "draggingLocation");
  NSPoint at = msg2(NSPoint, W.view, "convertPoint:fromView:", where, (id) nullptr);
  W.input.drop_at = v2(at.x, at.y);
  id paths = msg1(id, board, "propertyListForType:", ns_str("NSFilenamesPboardType"));
  for (NSUInteger i = 0; paths && i < ns_count(paths); i++) {
    const char *path = ns_cstr(ns_at(paths, i));
    if (path) vec_push(&W.input.drops, xstrdup(path));
  }
  want();
  return YES;
}

static Class view_class(void) {
  Class cls = class_new("CivueView", "NSView");
  class_method(cls, "acceptsFirstResponder", (void *)yes, "c@:");
  class_method(cls, "acceptsFirstMouse:", (void *)yes, "c@:@");
  class_method(cls, "isFlipped", (void *)yes, "c@:");
  class_method(cls, "wantsUpdateLayer", (void *)yes, "c@:");
  class_method(cls, "makeBackingLayer", (void *)make_backing_layer, "@@:");
  class_method(cls, "setFrameSize:", (void *)set_frame_size, "v@:{CGSize=dd}");
  class_method(cls, "viewDidChangeBackingProperties", (void *)backing_changed, "v@:");
  class_method(cls, "viewWillStartLiveResize", (void *)live_resize_began, "v@:");
  class_method(cls, "viewDidEndLiveResize", (void *)live_resize_ended, "v@:");
  class_method(cls, "viewDidChangeEffectiveAppearance", (void *)appearance_changed, "v@:");
  class_method(cls, "updateTrackingAreas", (void *)update_tracking, "v@:");
  class_method(cls, "mouseMoved:", (void *)moved, "v@:@");
  class_method(cls, "mouseDragged:", (void *)moved, "v@:@");
  class_method(cls, "rightMouseDragged:", (void *)moved, "v@:@");
  class_method(cls, "otherMouseDragged:", (void *)moved, "v@:@");
  class_method(cls, "mouseExited:", (void *)exited, "v@:@");
  class_method(cls, "mouseDown:", (void *)mouse_down, "v@:@");
  class_method(cls, "mouseUp:", (void *)mouse_up, "v@:@");
  class_method(cls, "rightMouseDown:", (void *)right_down, "v@:@");
  class_method(cls, "rightMouseUp:", (void *)right_up, "v@:@");
  class_method(cls, "otherMouseDown:", (void *)other_down, "v@:@");
  class_method(cls, "otherMouseUp:", (void *)other_up, "v@:@");
  class_method(cls, "scrollWheel:", (void *)scroll_wheel, "v@:@");
  class_method(cls, "magnifyWithEvent:", (void *)magnify, "v@:@");
  class_method(cls, "flagsChanged:", (void *)flags_changed, "v@:@");
  class_method(cls, "keyDown:", (void *)key_down, "v@:@");
  class_method(cls, "keyUp:", (void *)key_up, "v@:@");
  class_method(cls, "insertText:replacementRange:", (void *)insert_text, "v@:@{_NSRange=QQ}");
  class_method(cls, "setMarkedText:selectedRange:replacementRange:", (void *)set_marked,
               "v@:@{_NSRange=QQ}{_NSRange=QQ}");
  class_method(cls, "unmarkText", (void *)unmark, "v@:");
  class_method(cls, "hasMarkedText", (void *)has_marked, "c@:");
  class_method(cls, "markedRange", (void *)marked_range, "{_NSRange=QQ}@:");
  class_method(cls, "selectedRange", (void *)selected_range, "{_NSRange=QQ}@:");
  class_method(cls, "validAttributesForMarkedText", (void *)valid_attributes, "@@:");
  class_method(cls, "attributedSubstringForProposedRange:actualRange:", (void *)substring,
               "@@:{_NSRange=QQ}^{_NSRange=QQ}");
  class_method(cls, "characterIndexForPoint:", (void *)char_index, "Q@:{CGPoint=dd}");
  class_method(cls, "firstRectForCharacterRange:actualRange:", (void *)first_rect,
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^{_NSRange=QQ}");
  class_method(cls, "doCommandBySelector:", (void *)do_command, "v@::");
  class_method(cls, "draggingEntered:", (void *)drag_entered, "Q@:@");
  class_method(cls, "performDragOperation:", (void *)drag_performed, "c@:@");
  Protocol *p = objc_getProtocol("NSTextInputClient");
  if (p) class_addProtocol(cls, p);
  class_done(cls);
  return cls;
}

// ---- full screen ----

// how tall the app's title bar is drawn: the strip's in full screen, the
// window's title bar otherwise.
static f64 bar_height(void) { return W.strip_shown && W.strip_height > 0 ? W.strip_height : W.options.titlebar; }

// like safari's tabs: in full screen the content takes the whole screen,
// and the app's title bar comes back with the menu bar. appkit slides its
// title bar in with it, inside a window of its own: the strip goes in
// there, over the traffic lights' row, so it moves with it and the lights
// sit on it. that row is shorter than the window's title bar, 32, and an
// accessory to make it taller would be 36 at the least, so the app's title
// bar is drawn as tall as the row instead.

// the traffic lights' row, which the reveal moves.
static id titlebar_row(void) {
  id close = msg1(id, W.window, "standardWindowButton:", (NSUInteger)0);
  return close ? msg0(id, close, "superview") : nullptr;
}

static void resize_strip_layer(void) {
  NSRect b = msg0(NSRect, W.strip_view, "bounds");
  f32 s = scale_factor();
  msg1(void, W.strip_layer, "setContentsScale:", (CGFloat)s);
  CGSize size = {fmax(round(b.size.width * s), 1), fmax(round(b.size.height * s), 1)};
  msg1(void, W.strip_layer, "setDrawableSize:", size);
}

static id strip_backing_layer(id self, SEL sel) { return retain(W.strip_layer); }

static void strip_frame_size(id self, SEL sel, NSSize size) {
  struct objc_super sup = {self, class_getSuperclass(object_getClass(self))};
  ((void (*)(struct objc_super *, SEL, NSSize))objc_msgSendSuper)(&sup, sel, size);
  resize_strip_layer();
  want();
}

static void strip_backing_changed(id self, SEL sel) {
  resize_strip_layer();
  want();
}

static BOOL no(id self, SEL sel) { return NO; }

static void lift_view(bool lift);

// the strip over the row, where appkit centers the lights already: moving
// them would have appkit move the others back. appkit lays the title bar out
// again on its own, this follows each time, the app's title bar taking a new height
// with the row.
static void place_strip(void) {
  id row = titlebar_row();
  id container = row ? msg0(id, row, "superview") : nullptr;
  if (!container || msg0(id, W.strip, "superview") != container) return;
  NSRect rf = msg0(NSRect, row, "frame");
  NSRect cb = msg0(NSRect, container, "bounds");
  if (rf.size.height > 0 && rf.size.height != W.strip_height) {
    W.strip_height = rf.size.height;
    lift_view(true);
    want();
  }
  NSRect want_at = {{0, rf.origin.y}, {cb.size.width, rf.size.height}};
  NSRect at = msg0(NSRect, W.strip, "frame");
  if (memcmp(&at, &want_at, sizeof at)) msg1(void, W.strip, "setFrame:", want_at);
}

static void titlebar_relayout(id self, SEL sel, id note) {
  if (W.strip_shown)
    place_strip();
  else if (!W.full_screen)
    place_window_buttons();
}

// takes the pointer like the window's view, its points being the ui's.
static void pointer_methods(Class cls) {
  class_method(cls, "acceptsFirstMouse:", (void *)yes, "c@:@");
  class_method(cls, "mouseDownCanMoveWindow", (void *)no, "c@:");
  class_method(cls, "updateTrackingAreas", (void *)update_tracking, "v@:");
  class_method(cls, "mouseMoved:", (void *)moved, "v@:@");
  class_method(cls, "mouseDragged:", (void *)moved, "v@:@");
  class_method(cls, "rightMouseDragged:", (void *)moved, "v@:@");
  class_method(cls, "otherMouseDragged:", (void *)moved, "v@:@");
  class_method(cls, "mouseExited:", (void *)exited, "v@:@");
  class_method(cls, "mouseDown:", (void *)mouse_down, "v@:@");
  class_method(cls, "mouseUp:", (void *)mouse_up, "v@:@");
  class_method(cls, "rightMouseDown:", (void *)right_down, "v@:@");
  class_method(cls, "rightMouseUp:", (void *)right_up, "v@:@");
  class_method(cls, "otherMouseDown:", (void *)other_down, "v@:@");
  class_method(cls, "otherMouseUp:", (void *)other_up, "v@:@");
}

static void make_strip(void) {
  NSRect r = {{0, 0}, {640, W.options.titlebar}};
  Class view_cls = class_new("CivueStripView", "NSView");
  pointer_methods(view_cls);
  class_method(view_cls, "isFlipped", (void *)yes, "c@:");
  class_method(view_cls, "wantsUpdateLayer", (void *)yes, "c@:");
  class_method(view_cls, "makeBackingLayer", (void *)strip_backing_layer, "@@:");
  class_method(view_cls, "setFrameSize:", (void *)strip_frame_size, "v@:{CGSize=dd}");
  class_method(view_cls, "viewDidChangeBackingProperties", (void *)strip_backing_changed, "v@:");
  class_done(view_cls);
  W.strip = msg1(id, msg0(id, CLS("NSView"), "alloc"), "initWithFrame:", r);
  NSUInteger fill = RESIZE_WIDTH | RESIZE_HEIGHT;
  id glass = NEW("NSVisualEffectView");
  msg1(void, glass, "setFrame:", r);
  msg1(void, glass, "setMaterial:", (NSInteger)MATERIAL_TITLEBAR);
  msg1(void, glass, "setBlendingMode:", (NSInteger)0);
  msg1(void, glass, "setState:", (NSInteger)EFFECT_ACTIVE);
  msg1(void, glass, "setAutoresizingMask:", fill);
  msg1(void, W.strip, "addSubview:", glass);
  release(glass);

  W.strip_layer = NEW("CAMetalLayer");
  msg1(void, W.strip_layer, "setDevice:", (id)civue_gfx_device(W.gfx));
  msg1(void, W.strip_layer, "setPixelFormat:", (NSUInteger)PIXEL_BGRA8);
  msg1(void, W.strip_layer, "setFramebufferOnly:", (BOOL)YES);
  msg1(void, W.strip_layer, "setOpaque:", (BOOL)NO);
  W.strip_view = msg1(id, msg0(id, CLS("CivueStripView"), "alloc"), "initWithFrame:", r);
  msg1(void, W.strip_view, "setWantsLayer:", (BOOL)YES);
  msg1(void, W.strip_view, "setAutoresizingMask:", fill);
  msg1(void, W.strip, "addSubview:", W.strip_view);
  resize_strip_layer();
}

// the main view a title bar taller than the window, its top off the screen,
// or back to the window's size.
static void lift_view(bool lift) {
  id content = msg0(id, W.window, "contentView");
  NSRect b = msg0(NSRect, content, "bounds");
  if (lift) b.size.height += bar_height();
  msg1(void, W.view, "setFrame:", b);
  msg1(void, W.glass, "setHidden:", (BOOL)lift);
}

// the top of the ui into the strip.
static void draw_strip(id cb, civue_ui *ui) {
  if (!W.strip_shown) return;
  place_strip();
  id drawable = msg0(id, W.strip_layer, "nextDrawable");
  if (!drawable) return;
  id texture = msg0(id, drawable, "texture");
  civue_color clear = {};
  civue_gfx_encode(W.gfx, cb, texture, (u32)msg0(NSUInteger, texture, "width"),
                   (u32)msg0(NSUInteger, texture, "height"), civue_frame_list(ui), &clear);
  msg1(void, cb, "presentDrawable:", drawable);
}

static void will_enter_full_screen(id self, SEL sel, id note) {
  if (!W.glass) return;
  sync_resizes(true);
  W.full_screen = true;
  W.strip_height = 0;
  lift_view(true);
  // the animation starts from this frame.
  want();
  draw_without_link();
}

// the row and its container, followed again as they move between the
// window and appkit's full screen one.
static void follow_titlebar(void) {
  id center = msg0(id, CLS("NSNotificationCenter"), "defaultCenter");
  msg1(void, center, "removeObserver:", W.titlebar_observer);
  id row = titlebar_row();
  if (!row) return;
  id name = ns_str("NSViewFrameDidChangeNotification");
  id views[2] = {row, msg0(id, row, "superview")};
  for (u32 i = 0; i < 2; i++)
    msg4(void, center, "addObserver:selector:name:object:", W.titlebar_observer, SEL_("relayout:"), name, views[i]);
}

static void did_enter_full_screen(id self, SEL sel, id note) {
  if (!W.full_screen) return;
  id row = titlebar_row();
  id container = row ? msg0(id, row, "superview") : nullptr;
  if (!container || msg0(id, row, "window") == W.window) {
    // no title bar of its own to ride on, the app's stays on the screen.
    lift_view(false);
    sync_resizes(false);
    return;
  }
  if (!W.strip) make_strip();
  // under the row, the lights and its empty parts over the strip.
  msg3(void, container, "addSubview:positioned:relativeTo:", W.strip, (NSInteger)-1, row);
  W.strip_shown = true;
  follow_titlebar();
  place_strip();
  sync_resizes(false);
}

static void will_exit_full_screen(id self, SEL sel, id note) {
  if (!W.full_screen) return;
  sync_resizes(true);
  if (W.strip_shown) {
    msg0(void, W.strip, "removeFromSuperview");
    W.strip_shown = false;
  }
  W.full_screen = false;
  lift_view(false);
  place_window_buttons();
  place_glass();
  // the animation starts from this frame.
  want();
  draw_without_link();
}

static void did_exit_full_screen(id self, SEL sel, id note) {
  follow_titlebar();
  place_window_buttons();
  sync_resizes(false);
}

// a move that did not happen leaves things as they were before it.
static void enter_failed(id self, SEL sel, id window) {
  will_exit_full_screen(self, sel, nullptr);
  did_exit_full_screen(self, sel, nullptr);
}

static void exit_failed(id self, SEL sel, id window) {
  W.full_screen = true;
  lift_view(true);
  did_enter_full_screen(self, sel, nullptr);
}

// ---- window and app ----

static void will_close(id self, SEL sel, id note) { msg1(void, W.app, "terminate:", (id) nullptr); }

static void key_changed(id self, SEL sel, id note) {
  if (!msg0(BOOL, W.window, "isKeyWindow")) {
    W.input.held.len = 0;
    memset(W.input.down, 0, sizeof W.input.down);
  }
  want();
}

static BOOL should_terminate_after_close(id self, SEL sel, id app) { return YES; }

void civue_window_run_sized(const civue_window_options *o, civue_app (*make)(civue_ui *ui, void *arg), void *arg,
                            size_t ui_size) {
  void *pool = objc_autoreleasePoolPush();
  W.options = *o;
  W.input.pressure = -1;
  W.app = msg0(id, CLS("NSApplication"), "sharedApplication");
  msg1(void, W.app, "setActivationPolicy:", (NSInteger)0);
  Class delegate_cls = class_new("CivueAppDelegate", "NSObject");
  class_method(delegate_cls, "applicationShouldTerminateAfterLastWindowClosed:", (void *)should_terminate_after_close,
               "c@:@");
  class_method(delegate_cls, "windowWillClose:", (void *)will_close, "v@:@");
  class_method(delegate_cls, "windowDidBecomeKey:", (void *)key_changed, "v@:@");
  class_method(delegate_cls, "windowDidResignKey:", (void *)key_changed, "v@:@");
  class_method(delegate_cls, "windowWillEnterFullScreen:", (void *)will_enter_full_screen, "v@:@");
  class_method(delegate_cls, "windowDidEnterFullScreen:", (void *)did_enter_full_screen, "v@:@");
  class_method(delegate_cls, "windowWillExitFullScreen:", (void *)will_exit_full_screen, "v@:@");
  class_method(delegate_cls, "windowDidExitFullScreen:", (void *)did_exit_full_screen, "v@:@");
  class_method(delegate_cls, "windowDidFailToEnterFullScreen:", (void *)enter_failed, "v@:@");
  class_method(delegate_cls, "windowDidFailToExitFullScreen:", (void *)exit_failed, "v@:@");
  class_done(delegate_cls);
  id delegate = NEW("CivueAppDelegate");
  msg1(void, W.app, "setDelegate:", delegate);
  if (o->install_menus) o->install_menus();

  id device = civue_gpu_device();
  W.gfx = civue_gfx_new(device);
  W.layer = NEW("CAMetalLayer");
  msg1(void, W.layer, "setDevice:", device);
  // blending happens on srgb encoded values like browsers do, and the
  // layer shows them as srgb.
  msg1(void, W.layer, "setPixelFormat:", (NSUInteger)PIXEL_BGRA8);
  msg1(void, W.layer, "setFramebufferOnly:", (BOOL)YES);
  msg1(void, W.layer, "setOpaque:", (BOOL)YES);
  msg1(void, W.layer, "setMaximumDrawableCount:", (NSUInteger)3);
  msg1(void, W.layer, "setAllowsNextDrawableTimeout:", (BOOL)NO);

  NSUInteger style = STYLE_TITLED | STYLE_CLOSABLE | STYLE_MINIATURIZABLE | STYLE_RESIZABLE;
  if (o->titlebar > 0) style |= STYLE_FULL_SIZE_CONTENT;
  NSRect frame = {{0, 0}, {o->width, o->height}};
  W.window = msg4(id, msg0(id, CLS("NSWindow"), "alloc"), "initWithContentRect:styleMask:backing:defer:", frame, style,
                  (NSUInteger)BACKING_BUFFERED, (BOOL)NO);
  msg1(void, W.window, "setReleasedWhenClosed:", (BOOL)NO);
  msg1(void, W.window, "setTitle:", ns_str(o->title));
  msg1(void, W.window, "setContentMinSize:", ((NSSize){o->min_width, o->min_height}));
  if (o->titlebar > 0) {
    msg1(void, W.window, "setTitlebarAppearsTransparent:", (BOOL)YES);
    msg1(void, W.window, "setTitleVisibility:", (NSInteger)1);
  }
  msg1(void, W.window, "setDelegate:", delegate);
  view_class();
  W.view = msg1(id, msg0(id, CLS("CivueView"), "alloc"), "initWithFrame:", frame);
  msg1(void, W.view, "setWantsLayer:", (BOOL)YES);
  id dragged[] = {ns_str("NSFilenamesPboardType")};
  msg1(void, W.view, "registerForDraggedTypes:",
       msg2(id, CLS("NSArray"), "arrayWithObjects:count:", (const id *)dragged, (NSUInteger)1));
  // vibrancy: the title bar's material shows where the app's title bar draws
  // nothing, under a layer that is not opaque.
  id content = W.view;
  if (o->titlebar > 0) {
    content = msg1(id, msg0(id, CLS("NSView"), "alloc"), "initWithFrame:", frame);
    W.glass = NEW("NSVisualEffectView");
    msg1(void, W.glass, "setMaterial:", (NSInteger)MATERIAL_TITLEBAR);
    // behind the window, dimmed with it when it is not the key window.
    msg1(void, W.glass, "setBlendingMode:", (NSInteger)0);
    msg1(void, W.glass, "setState:", (NSInteger)0);
    msg1(void, W.glass, "setAutoresizingMask:", (NSUInteger)(RESIZE_WIDTH | RESIZE_BOTTOM_MARGIN));
    msg1(void, content, "addSubview:", W.glass);
    msg1(void, W.view, "setAutoresizingMask:", (NSUInteger)(RESIZE_WIDTH | RESIZE_HEIGHT));
    msg1(void, content, "addSubview:", W.view);
    msg1(void, W.layer, "setOpaque:", (BOOL)NO);
  }
  msg1(void, W.window, "setContentView:", content);
  if (content != W.view) release(content);
  msg1(void, W.window, "makeFirstResponder:", W.view);
  msg0(void, W.window, "center");
  resize_layer();

  W.ui = civue_ui_new_sized(W.gfx, scale_factor(), ui_size);
  if (o->icons) civue_icons_use(W.ui->icons, o->icons, o->nicons);
  W.ui->titlebar_glass = W.glass != nullptr;
  if (W.glass) {
    Class observer_cls = class_new("CivueTitlebarObserver", "NSObject");
    class_method(observer_cls, "relayout:", (void *)titlebar_relayout, "v@:@");
    class_done(observer_cls);
    W.titlebar_observer = NEW("CivueTitlebarObserver");
    follow_titlebar();
  }
  W.ui->system_dark = system_dark();
  W.app_cb = make(W.ui, arg);

  start_link();

  msg1(void, W.window, "makeKeyAndOrderFront:", (id) nullptr);
  msg1(void, W.app, "activateIgnoringOtherApps:", (BOOL)YES);
  want();
  objc_autoreleasePoolPop(pool);
  msg0(void, W.app, "run");
}

void civue_open_url(const char *url) {
  AUTORELEASE({
    id u = msg1(id, CLS("NSURL"), "URLWithString:", ns_str(url));
    id ws = msg0(id, CLS("NSWorkspace"), "sharedWorkspace");
    if (u) msg1(BOOL, ws, "openURL:", u);
  });
}

static void call(void *arg) {
  void **c = arg;
  ((void (*)(void *))c[0])(c[1]);
}

void civue_on_main(void (*fn)(void *arg), void *arg) {
  if (pthread_main_np()) {
    fn(arg);
    return;
  }
  void *c[] = {(void *)fn, arg};
  dispatch_sync_f(dispatch_get_main_queue(), c, call);
}
