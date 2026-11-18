#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include "darwin.h"
#include "window.h"

#include <civue/gpu.h>

#include <pthread.h>

@interface CivuePointerView : NSView
@end

@interface CivueView : CivuePointerView <NSTextInputClient>
@end

@interface CivueOutsideView : CivuePointerView
@end

@interface CivueOutsideWindow : NSPanel
@end

@interface CivueStripView : CivuePointerView
@end

@interface CivueLink : NSObject
- (void)tick:(CADisplayLink *)link;
@end

@interface CivueAppDelegate : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@interface CivueTitlebarObserver : NSObject
- (void)relayout:(NSNotification *)note;
@end

// the one window and what drives it.
static struct {
  NSApplication *app;
  NSWindow *window;
  CivueView *view;
  // the gpu's layer, and over it the one showing the frames the cpu draws.
  CAMetalLayer *layer;
  CALayer *soft;
  CADisplayLink *link;
  CivueLink *link_target;
  CivueAppDelegate *delegate;
  // the title bar's material under the app's title bar.
  NSVisualEffectView *glass;
  // full screen hides the app's title bar, the strip brings it back inside
  // the system's one: its view, the ui's view in it and that one's layer,
  // and what follows the title bar's layout. the app's title bar is as tall
  // as that row there.
  bool full_screen, strip_shown;
  NSView *strip;
  CivueStripView *strip_view;
  CAMetalLayer *strip_layer;
  // follows appkit's layouts of the title bar, to put it right at once.
  CivueTitlebarObserver *titlebar_observer;
  f64 strip_height;
  // overlays that go past the window's edges are drawn in a clear window
  // of its own over the screen, which takes the pointer for the ui.
  CivueOutsideWindow *outside;
  CivueOutsideView *outside_view;
  CAMetalLayer *outside_layer;
  civue_draw_list outside_list;
  bool outside_shown;
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
  // the input method's composed text.
  NSString *marked;
  // a live resize draws each size at once, in the resize's transaction.
  bool resizing;

  // the surfaces the cpu draws frames into, the one shown and the ones it
  // draws the next into, each missing what the frames since it was drawn
  // changed. none is shown after a frame the gpu drew.
  struct {
    IOSurfaceRef io;
    civue_damage missing;
  } surfaces[3];

  int shown;
  // the surface of the gpu's last frame while it is the one shown, to read
  // back when the cpu draws the next.
  IOSurfaceRef gpu_frame;
  // left once the gpu has drawn it.
  dispatch_group_t gpu_drawing;
  f64 frame_at;
  bool frame_watched;
  // when the frame being built began.
  f64 began;
  // how many frames in a row the cpu took too long to draw, when the last
  // did, and when the gpu last drew.
  u32 slow;
  f64 slow_at, gpu_at;
} W = {.window_dark = -1, .shown = -1};

void (*civue__webviews_place)(u64 frame);

NSView *civue__window_view() {
  return W.view;
}

static void draw_now();
static void start_link();

static void want() {
  W.wanted = true;

  if (W.link && W.link.paused) {
    W.idle = 0;
    W.link.paused = NO;
  }
  else if (!W.link && !W.queued) {
    W.queued = true;
    dispatch_async(dispatch_get_main_queue(), ^{
      W.queued = false;

      if (!W.link) {
        draw_now();
      }
    });
  }
}

void civue_window_wake() {
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

static f32 scale_factor() {
  return W.window ? (f32)W.window.backingScaleFactor : 1;
}

static bool system_dark() {
  NSAppearanceName best =
    [W.app.effectiveAppearance bestMatchFromAppearancesWithNames:@[
      NSAppearanceNameAqua, NSAppearanceNameDarkAqua
    ]];
  return [best isEqualToString:NSAppearanceNameDarkAqua];
}

static void fit_layer(CAMetalLayer *layer, NSView *view) {
  NSRect b = view.bounds;
  f32 s = scale_factor();
  layer.contentsScale = s;
  layer.drawableSize = CGSizeMake(fmax(round(b.size.width * s), 1),
                                  fmax(round(b.size.height * s), 1));

  if (layer == W.layer) {
    W.soft.contentsScale = s;
    W.soft.frame = layer.bounds;
  }
}

// places the traffic lights in the app's title bar: the close button 12
// points from the left, and from the top since macos 26, 11 before. appkit
// lays them out again on resizes, so this runs every frame and only moves
// what moved.
static void place_window_buttons() {
  NSButton *first = [W.window standardWindowButton:NSWindowCloseButton];

  if (!first) {
    return;
  }

  static f64 y;

  if (!y) {
    NSOperatingSystemVersion v =
      NSProcessInfo.processInfo.operatingSystemVersion;
    y = v.majorVersion >= 26 ? 12 : 11;
  }

  f64 x = 12;
  NSView *container = first.superview.superview;

  if (!container) {
    return;
  }

  NSRect ff = first.frame;
  f64 height = ff.size.height + 2 * y;
  NSRect frame = container.frame;
  f64 top = W.window.frame.size.height - height;

  if (frame.size.height != height || frame.origin.y != top) {
    frame.size.height = height;
    frame.origin.y = top;
    container.frame = frame;
  }

  // appkit's own spacing, read before the lights are first moved: once moved,
  // appkit shifts the others along and a spacing read then grows each time.
  // the lights stay where they are until appkit has laid them out.
  static f64 spacing;

  if (!spacing) {
    NSButton *second =
      [W.window standardWindowButton:NSWindowMiniaturizeButton];
    f64 apart = second ? second.frame.origin.x - ff.origin.x : 0;

    if (apart <= 0) {
      return;
    }

    spacing = apart;
  }

  NSWindowButton kinds[] = {
    NSWindowCloseButton,
    NSWindowMiniaturizeButton,
    NSWindowZoomButton,
  };

  for (u32 i = 0; i < countof(kinds); i++) {
    NSButton *b = [W.window standardWindowButton:kinds[i]];
    NSPoint want_at = {x + i * spacing, y};

    if (b && !NSEqualPoints(b.frame.origin, want_at)) {
      [b setFrameOrigin:want_at];
    }
  }
}

void civue_window_buttons_hidden(bool hidden) {
  NSWindowButton kinds[] = {
    NSWindowCloseButton,
    NSWindowMiniaturizeButton,
    NSWindowZoomButton,
  };

  for (u32 i = 0; i < countof(kinds); i++) {
    [W.window standardWindowButton:kinds[i]].hidden = hidden;
  }

  // appkit lays its title bar out again for it, the lights back where it
  // wants them: done now and undone, or they show there until the next
  // frame.
  [W.window.contentView.superview layoutSubtreeIfNeeded];

  if (!W.full_screen) {
    place_window_buttons();
  }
}

// the material spans the app's title bar.
static void place_glass() {
  if (!W.glass) {
    return;
  }

  NSRect b = W.window.contentView.bounds;
  f64 h = W.options.titlebar;
  NSRect want_at = {{0, b.size.height - h}, {b.size.width, h}};

  if (!NSEqualRects(W.glass.frame, want_at)) {
    W.glass.frame = want_at;
  }
}

static NSCursor *system_cursor(civue_cursor_kind kind) {
  switch (kind) {
    case CIVUE_CURSOR_POINTER:
      return NSCursor.pointingHandCursor;
    case CIVUE_CURSOR_TEXT:
      return NSCursor.IBeamCursor;
    case CIVUE_CURSOR_RESIZE_H:
      return NSCursor.resizeLeftRightCursor;
    case CIVUE_CURSOR_RESIZE_V:
      return NSCursor.resizeUpDownCursor;
    case CIVUE_CURSOR_RESIZE_E:
      return NSCursor.resizeRightCursor;
    case CIVUE_CURSOR_RESIZE_W:
      return NSCursor.resizeLeftCursor;
    case CIVUE_CURSOR_HAND:
      return NSCursor.openHandCursor;
    case CIVUE_CURSOR_GRABBING:
      return NSCursor.closedHandCursor;
    case CIVUE_CURSOR_CROSSHAIR:
      return NSCursor.crosshairCursor;
    default:
      return NSCursor.arrowCursor;
  }
}

static void set_cursor(civue_cursor c) {
  if (!civue__cursor_apply(&W.options, c)) {
    [system_cursor(c.kind) set];
  }
}

static void schedule_timer(f64 at);
static void draw_strip(id<MTLCommandBuffer> cb, const civue_draw_list *list);
static f64 bar_height();
static void show_as_srgb(CAMetalLayer *layer);

// a frame the cpu would take this many pixels' fill to draw goes to the gpu.
constexpr u64 COSTLY = 32'000'000;
// so do the frames of an animation that take more than this much of a
// refresh, most of it drawing, from the third in a row.
constexpr f64 TOO_LONG = 0.75;
constexpr u32 IN_A_ROW = 3;
// frames this far apart are not in a row.
constexpr f64 APART = 0.05;

// who draws a frame: the gpu, or the cpu over the frame shown, over the
// gpu's last frame read back, or all of it anew.
typedef enum {
  BY_GPU,
  OVER_SHOWN,
  OVER_GPU,
  ANEW
} Drawer;

static bool sized(IOSurfaceRef io, u32 pw, u32 ph) {
  return io && IOSurfaceGetWidth(io) == pw && IOSurfaceGetHeight(io) == ph;
}

static void drop_gpu_frame() {
  if (W.gpu_frame) {
    CFRelease(W.gpu_frame);
    W.gpu_frame = nullptr;
  }

  W.gpu_drawing = nil;
}

// frees the surfaces but the one shown, or that one too.
static void drop_surfaces(bool shown_too) {
  for (int i = 0; i < (int)countof(W.surfaces); i++) {
    if (W.surfaces[i].io && (shown_too || i != W.shown)) {
      CFRelease(W.surfaces[i].io);
      W.surfaces[i].io = nullptr;
    }
  }

  if (shown_too) {
    W.shown = -1;
  }
}

// how long a frame may take: a part of the display's refresh.
static f64 time_allowed() {
  NSScreen *screen = W.window.screen;
  f64 hz = screen ? (f64)screen.maximumFramesPerSecond : 60;

  return TOO_LONG / (hz > 0 ? hz : 60);
}

// who draws what changed of `list`. the gpu's driver keeps over a hundred
// megabytes around while it is given work, and for seconds after, so the
// cpu draws every frame it can and has the time to.
static Drawer drawer(const civue_draw_list *list, const civue_damage *changes,
                     u32 pw, u32 ph) {
  if (W.strip_shown) {
    return BY_GPU;
  }

  f64 now = now_s();

  // an animation the cpu was too slow for stays with the gpu until it
  // stops.
  if (now - W.gpu_at < APART && W.slow >= IN_A_ROW) {
    return BY_GPU;
  }

  u64 cost;

  if (!civue_raster_cost(list, changes, pw, ph, &cost)) {
    return BY_GPU;
  }

  Drawer by = OVER_SHOWN;

  // with no frame of its own shown the cpu starts from the gpu's last one,
  // or anew.
  if (W.shown < 0 || !sized(W.surfaces[W.shown].io, pw, ph)) {
    civue_damage all = {.all = true};
    u64 anew;

    if (civue_raster_cost(list, &all, pw, ph, &anew)) {
      by = ANEW;
      cost = anew;
    }
    else if (sized(W.gpu_frame, pw, ph)) {
      by = OVER_GPU;
    }
    else {
      return BY_GPU;
    }
  }

  if (cost > COSTLY) {
    return BY_GPU;
  }

  if (W.slow >= IN_A_ROW && now - W.slow_at < APART) {
    return BY_GPU;
  }

  return by;
}

static IOSurfaceRef make_surface(u32 pw, u32 ph) {
  // the window server shows nothing of a surface whose rows are not a
  // multiple of 16 bytes long: with rows as long as the width, one in two
  // widths of a resize came out blank.
  usize stride = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, (usize)pw * 4);

  NSDictionary *properties = @{
    (id)kIOSurfaceWidth : @(pw),
    (id)kIOSurfaceHeight : @(ph),
    (id)kIOSurfaceBytesPerElement : @4,
    (id)kIOSurfaceBytesPerRow : @(stride),
    (id)kIOSurfaceAllocSize : @(stride * ph),
    (id)kIOSurfacePixelFormat : @((u32)'BGRA'),
  };
  IOSurfaceRef io = IOSurfaceCreate((__bridge CFDictionaryRef)properties);

  if (!io) {
    die("no memory for %u by %u pixels", pw, ph);
  }

  // srgb, as the gpu's layer shows its frames: an untagged surface would
  // be shown in the display's own colors.
  static CFTypeRef srgb;

  if (!srgb) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    srgb = CGColorSpaceCopyPropertyList(space);
    CGColorSpaceRelease(space);
  }

  IOSurfaceSetValue(io, kIOSurfaceColorSpace, srgb);
  return io;
}

// a surface to draw the next frame into: not the one shown, nor one the
// window server may still be reading, which gives its place to a new one
// when there is no other.
static int free_surface(u32 pw, u32 ph) {
  int empty = -1, busy = -1;

  for (int i = 0; i < (int)countof(W.surfaces); i++) {
    if (i == W.shown) {
      continue;
    }

    if (!W.surfaces[i].io) {
      empty = i;
    }
    else if (!IOSurfaceIsInUse(W.surfaces[i].io)) {
      return i;
    }
    else {
      busy = i;
    }
  }

  if (empty < 0) {
    CFRelease(W.surfaces[busy].io);
    empty = busy;
  }

  W.surfaces[empty].io = make_surface(pw, ph);
  W.surfaces[empty].missing = (civue_damage){.all = true};
  return empty;
}

// copies a surface to another of the same size.
static void copy_whole(IOSurfaceRef from, IOSurfaceRef to) {
  IOSurfaceLock(from, kIOSurfaceLockReadOnly, nullptr);

  const u8 *src = IOSurfaceGetBaseAddress(from);
  u8 *dst = IOSurfaceGetBaseAddress(to);
  usize src_stride = IOSurfaceGetBytesPerRow(from);
  usize dst_stride = IOSurfaceGetBytesPerRow(to);
  usize width = IOSurfaceGetWidth(to) * 4, height = IOSurfaceGetHeight(to);

  for (usize y = 0; y < height; y++) {
    memcpy(dst + y * dst_stride, src + y * src_stride, width);
  }

  IOSurfaceUnlock(from, kIOSurfaceLockReadOnly, nullptr);
}

// how long a still window keeps the surfaces it does not show.
constexpr f64 KEPT_FOR = 2;

// frees them once the window stopped drawing.
static void watch_frame() {
  if (W.frame_watched) {
    return;
  }

  W.frame_watched = true;

  NSTimer *timer =
    [NSTimer timerWithTimeInterval:KEPT_FOR
                           repeats:NO
                             block:^(NSTimer *) {
                               W.frame_watched = false;

                               if (now_s() - W.frame_at < KEPT_FOR - 0.1) {
                                 watch_frame();
                                 return;
                               }

                               drop_surfaces(false);
                             }];
  [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
}

// draws what changed on the cpu, into a surface of the window's own that
// the layer over the gpu's then shows.
static void draw_on_cpu(Drawer by, const civue_draw_list *list,
                        civue_color background, civue_damage changes, u32 pw,
                        u32 ph) {
  f64 drawing = now_s();

  if (W.shown >= 0 && !sized(W.surfaces[W.shown].io, pw, ph)) {
    drop_surfaces(true);
  }

  int at = free_surface(pw, ph);
  IOSurfaceRef io = W.surfaces[at].io;

  IOSurfaceLock(io, 0, nullptr);

  // what the surface misses of the frames since it was drawn is drawn
  // again with what changed, when that is not all of it.
  civue_damage drawn = {.all = true};

  if (by == OVER_GPU) {
    // the gpu may still be drawing the frame read back.
    dispatch_group_wait(W.gpu_drawing, DISPATCH_TIME_FOREVER);
    copy_whole(W.gpu_frame, io);
    drawn = changes;
  }
  else if (by == OVER_SHOWN && !changes.all) {
    drawn = W.surfaces[at].missing;

    // a surface that misses everything starts as the one shown.
    if (drawn.all) {
      copy_whole(W.surfaces[W.shown].io, io);
      drawn = changes;
    }
    else {
      civue_damage_join(&drawn, &changes);
    }
  }

  civue_surface pixels = {IOSurfaceGetBaseAddress(io), pw, ph,
                          IOSurfaceGetBytesPerRow(io)};
  civue_raster(&pixels, list, background, &drawn, civue_gfx_atlas(W.gfx),
               civue_gfx_color_atlas(W.gfx));
  IOSurfaceUnlock(io, 0, nullptr);

  for (int i = 0; i < (int)countof(W.surfaces); i++) {
    civue_damage_join(&W.surfaces[i].missing, &changes);
  }

  W.surfaces[at].missing = (civue_damage){};
  W.shown = at;
  drop_gpu_frame();

  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  W.soft.contents = (__bridge id)io;
  W.soft.hidden = NO;
  [CATransaction commit];

  // an animation the cpu is too slow for is the gpu's: its frames come
  // late, and the drawing is what takes the time. a resize's frames are
  // drawn as its sizes come, late for none.
  f64 now = now_s();
  f64 whole = now - W.began;

  if (W.resizing || whole <= time_allowed() || (now - drawing) * 2 < whole) {
    W.slow = 0;
  }
  else {
    W.slow = now - W.slow_at < APART ? W.slow + 1 : 1;
    W.slow_at = now;
  }
}

// draws the whole frame on the gpu.
static void draw_on_gpu(const civue_draw_list *list, civue_color background,
                        u32 pw, u32 ph) {
  if (!W.layer.device) {
    W.layer.device = (__bridge id<MTLDevice>)civue_gfx_device(W.gfx);
  }

  bool synced = W.resizing || !W.soft.hidden;

  if (W.layer.presentsWithTransaction != synced) {
    W.layer.presentsWithTransaction = synced;
  }

  id<CAMetalDrawable> drawable = [W.layer nextDrawable];
  id<MTLTexture> texture = drawable.texture;

  // none, or one of the window's old size that would be stretched: the
  // frame is drawn again.
  if (!drawable || texture.width != pw || texture.height != ph) {
    civue_frame_forget(W.ui);
    want();
    return;
  }

  id<MTLCommandQueue> queue = (__bridge id)civue_gfx_queue(W.gfx);
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  civue_gfx_encode(W.gfx, (__bridge void *)cb, (__bridge void *)texture, pw, ph,
                   list, &background);
  draw_strip(cb, list);

  dispatch_group_t drawing = dispatch_group_create();
  dispatch_group_enter(drawing);
  [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
    dispatch_group_leave(drawing);
  }];

  if (synced) {
    // shown with the transaction: the layer of the cpu's frames goes away
    // with it, and a resized frame comes with the window's new size.
    [cb commit];
    [cb waitUntilScheduled];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    [drawable present];
    W.soft.hidden = YES;
    W.soft.contents = nil;
    [CATransaction commit];
  }
  else {
    [cb presentDrawable:drawable];
    [cb commit];
  }

  drop_surfaces(true);
  drop_gpu_frame();

  IOSurfaceRef io = texture.iosurface;

  if (io) {
    W.gpu_frame = (IOSurfaceRef)CFRetain(io);
    W.gpu_drawing = drawing;
  }

  W.gpu_at = now_s();
}

// where the screen is around the ui, in its points.
static civue_rect screen_around(NSRect of) {
  NSRect v = [W.window convertRectToScreen:[W.view convertRect:W.view.bounds
                                                        toView:nil]];

  return rect(of.origin.x - v.origin.x, NSMaxY(v) - NSMaxY(of),
              NSMaxX(of) - v.origin.x, NSMaxY(v) - of.origin.y);
}

static bool may_go_outside() {
  return W.window.screen && !W.full_screen && !W.resizing &&
         !(W.window.styleMask & NSWindowStyleMaskFullScreen);
}

static void hide_outside() {
  if (!W.outside_shown) {
    return;
  }

  W.outside_shown = false;
  [W.window removeChildWindow:W.outside];
  [W.outside orderOut:nil];
}

// draws the overlays alone in the window over the screen.
static void draw_outside(f32 scale) {
  if (!W.outside) {
    W.outside_layer = [CAMetalLayer layer];
    W.outside_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    show_as_srgb(W.outside_layer);
    W.outside_layer.opaque = NO;
    W.outside_layer.presentsWithTransaction = YES;
    W.outside_layer.allowsNextDrawableTimeout = NO;

    W.outside = [[CivueOutsideWindow alloc]
      initWithContentRect:NSZeroRect
                styleMask:NSWindowStyleMaskBorderless |
                          NSWindowStyleMaskNonactivatingPanel
                  backing:NSBackingStoreBuffered
                    defer:NO];
    W.outside.releasedWhenClosed = NO;
    W.outside.opaque = NO;
    W.outside.hasShadow = NO;
    W.outside.backgroundColor = NSColor.clearColor;
    W.outside.acceptsMouseMovedEvents = YES;
    W.outside.hidesOnDeactivate = NO;
    W.outside.animationBehavior = NSWindowAnimationBehaviorNone;
    W.outside_view = [[CivueOutsideView alloc] initWithFrame:NSZeroRect];
    W.outside_view.wantsLayer = YES;
    W.outside.contentView = W.outside_view;
  }

  NSRect screen = W.window.screen.frame;

  if (!NSEqualRects(W.outside.frame, screen)) {
    [W.outside setFrame:screen display:NO];
  }

  W.outside_layer.contentsScale = scale;
  W.outside_layer.drawableSize =
    CGSizeMake(fmax(round(screen.size.width * scale), 1),
               fmax(round(screen.size.height * scale), 1));

  if (!W.outside_layer.device) {
    W.outside_layer.device = (__bridge id<MTLDevice>)civue_gfx_device(W.gfx);
  }

  civue_rect around = screen_around(screen);

  civue_dl_clear(&W.outside_list);
  civue_dl_append(&W.outside_list, &W.ui->overlay);
  civue_dl_shift(&W.outside_list, (f32)round(-around.x0 * scale),
                 (f32)round(-around.y0 * scale));

  id<CAMetalDrawable> drawable = [W.outside_layer nextDrawable];

  if (!drawable) {
    want();
    return;
  }

  id<MTLTexture> texture = drawable.texture;
  id<MTLCommandQueue> queue = (__bridge id)civue_gfx_queue(W.gfx);
  id<MTLCommandBuffer> cb = [queue commandBuffer];
  civue_color clear = {};

  civue_gfx_encode(W.gfx, (__bridge void *)cb, (__bridge void *)texture,
                   (u32)texture.width, (u32)texture.height, &W.outside_list,
                   &clear);
  [cb commit];
  [cb waitUntilScheduled];
  [CATransaction begin];
  [CATransaction setDisableActions:YES];
  [drawable present];
  [CATransaction commit];

  if (!W.outside_shown) {
    W.outside_shown = true;
    [W.window addChildWindow:W.outside ordered:NSWindowAbove];
  }
}

static void present(civue_color background, u32 pw, u32 ph) {
  civue_damage changes;
  const civue_draw_list *list =
    civue_frame_changes(W.ui, background, pw, ph, &changes);

  // an image the app registered may have been drawn into since.
  if (W.strip_shown || !civue_gfx_static(W.gfx, list)) {
    changes = (civue_damage){.all = true};
  }

  // the window shows this frame already.
  if (!changes.all && !changes.any) {
    return;
  }

  W.frame_at = now_s();
  watch_frame();

  Drawer by = drawer(list, &changes, pw, ph);

  if (by == BY_GPU) {
    draw_on_gpu(list, background, pw, ph);
  }
  else {
    draw_on_cpu(by, list, background, changes, pw, ph);
  }
}

// builds a frame and shows it.
static void frame() {
  @autoreleasepool {
    W.began = now_s();

    f32 scale = scale_factor();
    CGSize size = W.layer.drawableSize;
    u32 pw = (u32)size.width;
    u32 ph = (u32)size.height;
    civue_ui *ui = W.ui;
    civue_color background = W.app_cb.background
                               ? W.app_cb.background(W.app_cb.ctx)
                               : color_hex(0x1e1e1e);

    if (W.options.titlebar > 0) {
      // the window, its buttons and title bar follow the app's appearance.
      if (W.window_dark != (int)ui->dark) {
        W.window_dark = ui->dark;
        W.window.appearance =
          [NSAppearance appearanceNamed:ui->dark ? NSAppearanceNameDarkAqua
                                                 : NSAppearanceNameAqua];
      }

      // in full screen the strip has the traffic lights, without it there
      // are none in the title bar nor room kept for them.
      bool full = W.window.styleMask & NSWindowStyleMaskFullScreen;

      if (!W.full_screen) {
        place_window_buttons();
        place_glass();
      }

      ui->titlebar_inset = full && !W.strip_shown ? 0 : 80;
      ui->titlebar_height = bar_height();
    }

    ui->system_dark = system_dark();
    ui->window_focused = W.window.keyWindow;
    ui->screen_known = true;
    ui->screen = may_go_outside() ? screen_around(W.window.screen.visibleFrame)
                                  : rect(0, 0, pw / scale, ph / scale);

    // what a pass asks of the window is asked once, the passes after it
    // begin without it and without the input that made it.
    bool drag = false, maximize = false, repaint = false;
    f64 repaint_at = 0;

    for (u32 pass = 0; pass < 4; pass++) {
      civue_begin_frame(ui, v2(pw / scale, ph / scale), scale, &W.input);
      W.app_cb.frame(W.app_cb.ctx, ui);

      bool settled = civue_end_frame(ui);
      drag |= ui->drag_window;
      maximize |= ui->toggle_maximize;
      repaint |= ui->repaint;

      if (ui->repaint_at && (!repaint_at || ui->repaint_at < repaint_at)) {
        repaint_at = ui->repaint_at;
      }

      if (settled) {
        break;
      }
    }

    ui->drag_window = drag;
    ui->toggle_maximize = maximize;
    ui->repaint = repaint;
    ui->repaint_at = repaint_at;
    // a frame that never settled is shown as it is, the next one is new.
    ui->settled = true;

    // by the last pass alone, the one the window shows.
    if (civue__webviews_place) {
      civue__webviews_place(ui->frames);
    }

    // the frame is shown without its overlays when they have a window.
    ui->overlay_apart =
      ui->overlay_outside && ui->overlay.quads.len != 0 && may_go_outside();
    present(background, pw, ph);

    if (ui->overlay_apart) {
      draw_outside(scale);
    }
    else {
      hide_outside();
    }

    if (!W.cursor_set || ui->cursor.kind != W.cursor.kind ||
        ui->cursor.angle != W.cursor.angle) {
      W.cursor = ui->cursor;
      W.cursor_set = true;
      set_cursor(ui->cursor);
    }

    // a full screen window neither moves nor zooms.
    if (ui->drag_window && !W.full_screen) {
      NSEvent *e = W.app.currentEvent;

      if (e) {
        [W.window performWindowDragWithEvent:e];
      }
    }

    if (ui->toggle_maximize && !W.full_screen) {
      [W.window zoom:nil];
    }

    if (ui->toggle_fullscreen) {
      ui->toggle_fullscreen = false;
      [W.window toggleFullScreen:nil];
    }

    if (ui->repaint) {
      want();
    }

    if (ui->repaint_at) {
      schedule_timer(ui->repaint_at);
    }
  }
}

// ---- display link ----

@implementation CivueLink

- (void)tick:(CADisplayLink *)link {
  // a live resize draws its frames itself, with the window's new sizes.
  if (W.resizing) {
    return;
  }

  if (!W.wanted) {
    // nothing to show, the link sleeps until a frame is wanted, not at
    // once since the next frame is often about to be.
    if (++W.idle > 8) {
      link.paused = YES;
    }

    return;
  }

  W.idle = 0;
  W.wanted = false;
  frame();
}

@end

// draws a wanted frame at once, without waiting for the display.
static void draw_now() {
  if (!W.wanted) {
    return;
  }

  W.wanted = false;
  frame();
}

// asks for frames as fast as the window's display goes.
static void pace_link() {
  NSScreen *screen = W.window.screen;
  float hz = screen ? (float)screen.maximumFramesPerSecond : 120;

  if (hz <= 0) {
    hz = 60;
  }

  W.link.preferredFrameRateRange = CAFrameRateRangeMake(fminf(hz, 60), hz, hz);
}

static void start_link() {
  W.link_target = [CivueLink new];
  W.link = [W.view displayLinkWithTarget:W.link_target
                                selector:@selector(tick:)];
  pace_link();
  W.link.paused = !W.wanted;
  [W.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

static void schedule_timer(f64 at) {
  if (W.timer_at && W.timer_at <= at) {
    return;
  }

  W.timer_at = at;

  NSTimer *timer = [NSTimer timerWithTimeInterval:fmax(at - now_s(), 0)
                                          repeats:NO
                                            block:^(NSTimer *) {
                                              W.timer_at = 0;
                                              want();
                                            }];
  [NSRunLoop.mainRunLoop addTimer:timer forMode:NSRunLoopCommonModes];
}

// ---- input ----

// the strip shows the top of the ui, so its points are the ui's as they are.
static civue_vec2 event_point(NSEvent *e) {
  bool strip = W.strip_shown && e.window == W.strip.window;
  NSView *view = strip ? W.strip_view : W.view;
  NSPoint at = e.locationInWindow;

  // the window over the screen says where on the screen.
  if (W.outside && e.window == W.outside) {
    at = [W.window convertPointFromScreen:[W.outside convertPointToScreen:at]];
  }

  NSPoint v = [view convertPoint:at fromView:nil];
  return v2(v.x, v.y);
}

static f32 event_pressure(NSEvent *e) {
  // only mouse events carry a subtype, appkit throws on others.
  if (e.type < NSEventTypeLeftMouseDown ||
      e.type > NSEventTypeRightMouseDragged) {
    return -1;
  }

  if (e.subtype != NSEventSubtypeTabletPoint) {
    return -1;
  }

  return fminf(fmaxf(e.pressure, 0), 1);
}

static u8 mods_of(NSEventModifierFlags flags) {
  return (flags & NSEventModifierFlagShift ? CIVUE_MOD_SHIFT : 0) |
         (flags & NSEventModifierFlagControl ? CIVUE_MOD_CTRL : 0) |
         (flags & NSEventModifierFlagOption ? CIVUE_MOD_ALT : 0) |
         (flags & NSEventModifierFlagCommand ? CIVUE_MOD_CMD : 0);
}

static void moved(NSEvent *e) {
  civue_vec2 at = event_point(e);
  W.input.has_mouse = true;
  W.input.mouse = at;
  W.input.pressure = event_pressure(e);
  vec_push(&W.input.trail, ((civue_trail_point){at, W.input.pressure}));
  W.input.mods = mods_of(e.modifierFlags);
  want();
}

static void button(NSEvent *e, u32 i, bool down) {
  moved(e);
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

static u32 special_key(u16 code) {
  switch (code) {
    case 36:
    case 76:
      return CIVUE_KEY_ENTER;
    case 53:
      return CIVUE_KEY_ESCAPE;
    case 48:
      return CIVUE_KEY_TAB;
    case 51:
      return CIVUE_KEY_BACKSPACE;
    case 117:
      return CIVUE_KEY_DELETE;
    case 123:
      return CIVUE_KEY_LEFT;
    case 124:
      return CIVUE_KEY_RIGHT;
    case 125:
      return CIVUE_KEY_DOWN;
    case 126:
      return CIVUE_KEY_UP;
    case 115:
      return CIVUE_KEY_HOME;
    case 119:
      return CIVUE_KEY_END;
    case 116:
      return CIVUE_KEY_PAGE_UP;
    case 121:
      return CIVUE_KEY_PAGE_DOWN;
    case 49:
      return CIVUE_KEY_SPACE;
  }

  static constexpr u16 fkeys[] = {
    122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111,
  };

  for (u32 i = 0; i < countof(fkeys); i++) {
    if (fkeys[i] == code) {
      return CIVUE_KEY_F1 + i;
    }
  }

  return CIVUE_KEY_NONE;
}

// the digit row gives its digit on any layout, shortcuts need them.
static u32 digit_key(u16 code) {
  static constexpr u16 row[] = {29, 18, 19, 20, 21, 23, 22, 26, 28, 25};
  static constexpr u16 pad[] = {82, 83, 84, 85, 86, 87, 88, 89, 91, 92};

  for (u32 i = 0; i < 10; i++) {
    if (row[i] == code || pad[i] == code) {
      return '0' + i;
    }
  }

  return CIVUE_KEY_NONE;
}

// the key a shifted symbol of a us layout is typed with.
static u32 unshifted(u32 c) {
  static constexpr char base[] = "`1234567890-=[]\\;',./";
  static constexpr char shifted[] = "~!@#$%^&*()_+{}|:\"<>?";

  for (u32 i = 0; shifted[i]; i++) {
    if ((u32)shifted[i] == c) {
      return (u32)base[i];
    }
  }

  return c;
}

static u32 lower(u32 c) {
  return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

// the key an event stands for, by the key itself rather than what option
// makes of it.
static u32 key_of(NSEvent *e, bool shift) {
  u32 k = special_key(e.keyCode);

  if (k) {
    return k;
  }

  k = digit_key(e.keyCode);

  if (k) {
    return k;
  }

  const char *s = e.charactersIgnoringModifiers.UTF8String ?: "";
  usize i = 0;
  u32 c = *s ? utf8_next(s, strlen(s), &i) : 0;

  if (!c || c < 0x20 || (c >= 0xf700 && c <= 0xf8ff)) {
    return CIVUE_KEY_NONE;
  }

  c = lower(c);
  return shift ? unshifted(c) : c;
}

// attributed strings come too.
static NSString *string_of(id s) {
  return [s isKindOfClass:NSAttributedString.class]
           ? ((NSAttributedString *)s).string
           : s;
}

// takes the pointer, its points being the ui's.
@implementation CivuePointerView

- (BOOL)isFlipped {
  return YES;
}

- (BOOL)wantsUpdateLayer {
  return YES;
}

- (BOOL)acceptsFirstMouse:(NSEvent *)e {
  return YES;
}

- (void)updateTrackingAreas {
  for (NSTrackingArea *area in self.trackingAreas.copy) {
    [self removeTrackingArea:area];
  }

  NSTrackingAreaOptions options =
    NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved |
    NSTrackingActiveAlways | NSTrackingInVisibleRect;
  [self addTrackingArea:[[NSTrackingArea alloc] initWithRect:NSZeroRect
                                                     options:options
                                                       owner:self
                                                    userInfo:nil]];
  [super updateTrackingAreas];
}

- (void)mouseMoved:(NSEvent *)e {
  moved(e);
}

- (void)mouseDragged:(NSEvent *)e {
  moved(e);
}

- (void)rightMouseDragged:(NSEvent *)e {
  moved(e);
}

- (void)otherMouseDragged:(NSEvent *)e {
  moved(e);
}

- (void)mouseExited:(NSEvent *)e {
  // the pointer leaves the window for the one over it, or that one as it
  // goes away: it is still over the ui.
  if ((self == (NSView *)W.outside_view) != W.outside_shown) {
    return;
  }

  if (!W.input.down[0] && !W.input.down[1] && !W.input.down[2]) {
    W.input.has_mouse = false;
  }

  want();
}

- (void)mouseDown:(NSEvent *)e {
  button(e, 0, true);
}

- (void)mouseUp:(NSEvent *)e {
  button(e, 0, false);
}

- (void)rightMouseDown:(NSEvent *)e {
  button(e, 1, true);
}

- (void)rightMouseUp:(NSEvent *)e {
  button(e, 1, false);
}

- (void)otherMouseDown:(NSEvent *)e {
  button(e, 2, true);
}

- (void)otherMouseUp:(NSEvent *)e {
  button(e, 2, false);
}

@end

// the display link would show a resized frame a refresh late, the old one
// stretched to the new size meanwhile, so a live resize or a move in and out
// of full screen draws each size at once, in the resize's transaction.
static void sync_resizes(bool on) {
  if (W.resizing == on) {
    return;
  }

  W.resizing = on;

  if (!on) {
    want();
  }
}

@implementation CivueView

- (BOOL)acceptsFirstResponder {
  return YES;
}

// under a transparent title bar appkit moves the window on any press there.
// the app says when a press moves it, see drag_window.
- (BOOL)mouseDownCanMoveWindow {
  return NO;
}

- (CALayer *)makeBackingLayer {
  return W.layer;
}

- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  fit_layer(W.layer, self);
  want();

  if (W.resizing) {
    draw_now();
  }
}

- (void)viewDidChangeBackingProperties {
  fit_layer(W.layer, self);
  want();

  if (W.resizing) {
    draw_now();
  }
}

- (void)viewWillStartLiveResize {
  [super viewWillStartLiveResize];
  sync_resizes(true);
}

- (void)viewDidEndLiveResize {
  [super viewDidEndLiveResize];
  sync_resizes(false);
}

- (void)viewDidChangeEffectiveAppearance {
  want();
}

- (void)scrollWheel:(NSEvent *)e {
  f64 dx = e.scrollingDeltaX;
  f64 dy = e.scrollingDeltaY;

  if (e.hasPreciseScrollingDeltas) {
    W.input.precise_scroll = true;
  }
  else {
    dx *= 40;
    dy *= 40;
  }

  W.input.scroll = v2_add(W.input.scroll, v2(dx, dy));
  W.input.mods = mods_of(e.modifierFlags);
  want();
}

- (void)magnifyWithEvent:(NSEvent *)e {
  W.input.pinch += (f32)e.magnification;
  want();
}

- (void)flagsChanged:(NSEvent *)e {
  W.input.mods = mods_of(e.modifierFlags);
  want();
}

- (void)keyDown:(NSEvent *)e {
  u8 mods = mods_of(e.modifierFlags);
  W.input.mods = mods;
  u32 key = key_of(e, mods & CIVUE_MOD_SHIFT);

  if (key) {
    bool held = false;

    vec_each(&W.input.held, h) {
      held |= *h == key;
    }

    if (!held) {
      vec_push(&W.input.held, key);
    }

    vec_push(&W.input.keys, ((civue_key_press){key, mods}));
  }

  // typed text goes through the input method, unless a shortcut takes it.
  if (!(mods & (CIVUE_MOD_CMD | CIVUE_MOD_CTRL))) {
    [self interpretKeyEvents:@[ e ]];
  }

  want();
}

- (void)keyUp:(NSEvent *)e {
  u32 key = key_of(e, e.modifierFlags & NSEventModifierFlagShift);
  u32 k = 0;

  vec_each(&W.input.held, h) {
    if (*h != key) {
      W.input.held.v[k++] = *h;
    }
  }

  W.input.held.len = k;
  want();
}

// ---- text input ----

- (void)insertText:(id)s replacementRange:(NSRange)r {
  const char *t = string_of(s).UTF8String ?: "";

  for (const char *c = t; *c; c++) {
    if ((u8)*c >= 0x20 && *c != 0x7f) {
      vec_push(&W.input.text, (u8)*c);
    }
  }

  W.input.preedit.len = 0;
  W.marked = nil;
  want();
}

- (void)setMarkedText:(id)s
        selectedRange:(NSRange)sr
     replacementRange:(NSRange)rr {
  W.marked = string_of(s);
  const char *t = W.marked.UTF8String ?: "";
  W.input.preedit.len = 0;
  buf_put(&W.input.preedit, t, strlen(t));
  want();
}

- (void)unmarkText {
  W.input.preedit.len = 0;
  W.marked = nil;
}

- (BOOL)hasMarkedText {
  return W.marked != nil;
}

- (NSRange)markedRange {
  return W.marked ? NSMakeRange(0, W.marked.length)
                  : NSMakeRange(NSNotFound, 0);
}

- (NSRange)selectedRange {
  return NSMakeRange(0, 0);
}

- (NSArray<NSAttributedStringKey> *)validAttributesForMarkedText {
  return @[];
}

- (NSAttributedString *)attributedSubstringForProposedRange:(NSRange)r
                                                actualRange:(NSRange *)actual {
  return nil;
}

- (NSUInteger)characterIndexForPoint:(NSPoint)p {
  return NSNotFound;
}

- (NSRect)firstRectForCharacterRange:(NSRange)r actualRange:(NSRange *)actual {
  // the candidates window opens under the pointer.
  NSRect wf = W.window.frame;
  civue_vec2 m = W.input.mouse;
  NSRect b = self.bounds;
  return NSMakeRect(wf.origin.x + m.x, wf.origin.y + b.size.height - m.y - 20,
                    1, 20);
}

- (void)doCommandBySelector:(SEL)command {
}

// ---- dropped files ----

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)info {
  return NSDragOperationCopy;
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)info {
  NSPoint at = [self convertPoint:info.draggingLocation fromView:nil];
  W.input.drop_at = v2(at.x, at.y);

  NSArray<NSURL *> *urls = [info.draggingPasteboard
    readObjectsForClasses:@[ NSURL.class ]
                  options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];

  for (NSURL *url in urls) {
    if (url.path) {
      vec_push(&W.input.drops, xstrdup(url.path.UTF8String));
    }
  }

  want();
  return YES;
}

@end

// ---- full screen ----

// how tall the app's title bar is drawn: the strip's in full screen, the
// window's title bar otherwise.
static f64 bar_height() {
  return W.strip_shown && W.strip_height > 0 ? W.strip_height
                                             : W.options.titlebar;
}

// like safari's tabs: in full screen the content takes the whole screen,
// and the app's title bar comes back with the menu bar. appkit slides its
// title bar in with it, inside a window of its own: the strip goes in
// there, over the traffic lights' row, so it moves with it and the lights
// sit on it. that row is shorter than the window's title bar, 32, and an
// accessory to make it taller would be 36 at the least, so the app's title
// bar is drawn as tall as the row instead.

// the traffic lights' row, which the reveal moves.
static NSView *titlebar_row() {
  return [W.window standardWindowButton:NSWindowCloseButton].superview;
}

@implementation CivueOutsideView

- (BOOL)mouseDownCanMoveWindow {
  return NO;
}

- (CALayer *)makeBackingLayer {
  return W.outside_layer;
}

- (void)scrollWheel:(NSEvent *)e {
  [W.view scrollWheel:e];
}

- (void)magnifyWithEvent:(NSEvent *)e {
  [W.view magnifyWithEvent:e];
}

@end

// the keys stay with the app's window.
@implementation CivueOutsideWindow

- (BOOL)canBecomeKeyWindow {
  return NO;
}

- (BOOL)canBecomeMainWindow {
  return NO;
}

@end

@implementation CivueStripView

- (BOOL)mouseDownCanMoveWindow {
  return NO;
}

- (CALayer *)makeBackingLayer {
  return W.strip_layer;
}

- (void)setFrameSize:(NSSize)size {
  [super setFrameSize:size];
  fit_layer(W.strip_layer, self);
  want();
}

- (void)viewDidChangeBackingProperties {
  fit_layer(W.strip_layer, self);
  want();
}

@end

// matched to the display as the surfaces the cpu draws are: a layer without
// a color space shows its values as the display's own.
static void show_as_srgb(CAMetalLayer *layer) {
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  layer.colorspace = space;
  CGColorSpaceRelease(space);
}

// the main view a title bar taller than the window, its top off the screen,
// or back to the window's size.
static void lift_view(bool lift) {
  NSRect b = W.window.contentView.bounds;

  if (lift) {
    b.size.height += bar_height();
  }

  W.view.frame = b;
  W.glass.hidden = lift;
}

// the strip over the row, where appkit centers the lights already: moving
// them would have appkit move the others back. appkit lays the title bar out
// again on its own, this follows each time, the app's title bar taking a new
// height with the row.
static void place_strip() {
  NSView *row = titlebar_row();
  NSView *container = row.superview;

  if (!container || W.strip.superview != container) {
    return;
  }

  NSRect rf = row.frame;

  if (rf.size.height > 0 && rf.size.height != W.strip_height) {
    W.strip_height = rf.size.height;
    lift_view(true);
    want();
  }

  NSRect want_at = {{0, rf.origin.y},
                    {container.bounds.size.width, rf.size.height}};

  if (!NSEqualRects(W.strip.frame, want_at)) {
    W.strip.frame = want_at;
  }
}

@implementation CivueTitlebarObserver

- (void)relayout:(NSNotification *)note {
  if (W.strip_shown) {
    place_strip();
  }
  else if (!W.full_screen) {
    place_window_buttons();
  }
}

@end

static void make_strip() {
  NSRect r = {{0, 0}, {640, W.options.titlebar}};
  NSAutoresizingMaskOptions fill = NSViewWidthSizable | NSViewHeightSizable;
  W.strip = [[NSView alloc] initWithFrame:r];

  NSVisualEffectView *glass = [[NSVisualEffectView alloc] initWithFrame:r];
  glass.material = NSVisualEffectMaterialTitlebar;
  glass.blendingMode = NSVisualEffectBlendingModeBehindWindow;
  glass.state = NSVisualEffectStateActive;
  glass.autoresizingMask = fill;
  [W.strip addSubview:glass];

  W.strip_layer = [CAMetalLayer layer];
  W.strip_layer.device = (__bridge id<MTLDevice>)civue_gfx_device(W.gfx);
  W.strip_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
  show_as_srgb(W.strip_layer);
  W.strip_layer.framebufferOnly = YES;
  W.strip_layer.opaque = NO;

  W.strip_view = [[CivueStripView alloc] initWithFrame:r];
  W.strip_view.wantsLayer = YES;
  W.strip_view.autoresizingMask = fill;
  [W.strip addSubview:W.strip_view];
  fit_layer(W.strip_layer, W.strip_view);
}

// the top of the ui into the strip.
static void draw_strip(id<MTLCommandBuffer> cb, const civue_draw_list *list) {
  if (!W.strip_shown) {
    return;
  }

  place_strip();

  id<CAMetalDrawable> drawable = [W.strip_layer nextDrawable];

  if (!drawable) {
    return;
  }

  id<MTLTexture> texture = drawable.texture;
  civue_color clear = {};
  civue_gfx_encode(W.gfx, (__bridge void *)cb, (__bridge void *)texture,
                   (u32)texture.width, (u32)texture.height, list, &clear);
  [cb presentDrawable:drawable];
}

// the row and its container, followed again as they move between the
// window and appkit's full screen one.
static void follow_titlebar() {
  NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
  [center removeObserver:W.titlebar_observer];

  NSView *row = titlebar_row();

  if (!row) {
    return;
  }

  for (NSView *view in @[ row, row.superview ]) {
    [center addObserver:W.titlebar_observer
               selector:@selector(relayout:)
                   name:NSViewFrameDidChangeNotification
                 object:view];
  }
}

static void entered_full_screen() {
  if (!W.full_screen) {
    return;
  }

  NSView *row = titlebar_row();
  NSView *container = row.superview;

  if (!container || row.window == W.window) {
    // no title bar of its own to ride on, the app's stays on the screen.
    lift_view(false);
    sync_resizes(false);
    return;
  }

  if (!W.strip) {
    make_strip();
  }

  // under the row, the lights and its empty parts over the strip.
  [container addSubview:W.strip positioned:NSWindowBelow relativeTo:row];
  W.strip_shown = true;
  follow_titlebar();
  place_strip();
  sync_resizes(false);
}

static void exiting_full_screen() {
  if (!W.full_screen) {
    return;
  }

  sync_resizes(true);

  if (W.strip_shown) {
    [W.strip removeFromSuperview];
    W.strip_shown = false;
  }

  W.full_screen = false;
  lift_view(false);
  place_window_buttons();
  place_glass();
  // the animation starts from this frame.
  want();
  draw_now();
}

static void exited_full_screen() {
  follow_titlebar();
  place_window_buttons();
  sync_resizes(false);
}

// ---- window and app ----

@implementation CivueAppDelegate

- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
  return YES;
}

- (void)windowWillClose:(NSNotification *)note {
  [W.app terminate:nil];
}

- (void)windowDidBecomeKey:(NSNotification *)note {
  want();
}

// another display may go faster or slower.
- (void)windowDidChangeScreen:(NSNotification *)note {
  pace_link();
}

- (void)windowDidResignKey:(NSNotification *)note {
  W.input.held.len = 0;
  memset(W.input.down, 0, sizeof W.input.down);
  want();
}

- (void)windowWillEnterFullScreen:(NSNotification *)note {
  if (!W.glass) {
    return;
  }

  sync_resizes(true);
  W.full_screen = true;
  W.strip_height = 0;
  lift_view(true);
  // the animation starts from this frame.
  want();
  draw_now();
}

- (void)windowDidEnterFullScreen:(NSNotification *)note {
  entered_full_screen();
}

- (void)windowWillExitFullScreen:(NSNotification *)note {
  exiting_full_screen();
}

- (void)windowDidExitFullScreen:(NSNotification *)note {
  exited_full_screen();
}

// a move that did not happen leaves things as they were before it.
- (void)windowDidFailToEnterFullScreen:(NSWindow *)window {
  exiting_full_screen();
  exited_full_screen();
}

- (void)windowDidFailToExitFullScreen:(NSWindow *)window {
  W.full_screen = true;
  lift_view(true);
  entered_full_screen();
}

@end

void civue_window_run_sized(const civue_window_options *o,
                            civue_app (*make)(civue_ui *ui, void *arg),
                            void *arg, size_t ui_size) {
  @autoreleasepool {
    W.options = *o;
    W.input.pressure = -1;
    W.app = NSApplication.sharedApplication;
    W.app.activationPolicy = NSApplicationActivationPolicyRegular;
    W.delegate = [CivueAppDelegate new];
    W.app.delegate = W.delegate;

    if (o->install_menus) {
      o->install_menus();
    }

    // the gpu's device is made when a frame first needs it.
    W.gfx = civue_gfx_new(nullptr);
    W.layer = [CAMetalLayer layer];
    // blending happens on srgb encoded values like browsers do, and the
    // layer shows them as srgb.
    W.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    show_as_srgb(W.layer);
    // the cpu reads the gpu's frames back.
    W.layer.framebufferOnly = NO;
    W.layer.opaque = YES;
    W.layer.maximumDrawableCount = 2;
    W.layer.allowsNextDrawableTimeout = NO;

    W.soft = [CALayer layer];
    W.soft.opaque = YES;
    W.soft.hidden = YES;
    W.soft.actions = @{
      @"contents" : NSNull.null,
      @"hidden" : NSNull.null,
      @"bounds" : NSNull.null,
      @"position" : NSNull.null,
    };
    [W.layer addSublayer:W.soft];

    NSWindowStyleMask style =
      NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
      NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;

    if (o->titlebar > 0) {
      style |= NSWindowStyleMaskFullSizeContentView;
    }

    NSRect frame = NSMakeRect(0, 0, o->width, o->height);
    W.window = [[NSWindow alloc] initWithContentRect:frame
                                           styleMask:style
                                             backing:NSBackingStoreBuffered
                                               defer:NO];
    W.window.releasedWhenClosed = NO;
    W.window.title = ns_str(o->title);
    W.window.contentMinSize = NSMakeSize(o->min_width, o->min_height);

    if (o->titlebar > 0) {
      W.window.titlebarAppearsTransparent = YES;
      W.window.titleVisibility = NSWindowTitleHidden;
    }

    W.window.delegate = W.delegate;
    W.view = [[CivueView alloc] initWithFrame:frame];
    W.view.wantsLayer = YES;
    [W.view registerForDraggedTypes:@[ NSPasteboardTypeFileURL ]];

    // vibrancy: the title bar's material shows where the app's title bar
    // draws nothing, under a layer that is not opaque.
    NSView *content = W.view;

    if (o->titlebar > 0) {
      content = [[NSView alloc] initWithFrame:frame];
      W.glass = [NSVisualEffectView new];
      W.glass.material = NSVisualEffectMaterialTitlebar;
      // behind the window, dimmed with it when it is not the key window.
      W.glass.blendingMode = NSVisualEffectBlendingModeBehindWindow;
      W.glass.state = NSVisualEffectStateFollowsWindowActiveState;
      W.glass.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
      [content addSubview:W.glass];
      W.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      [content addSubview:W.view];
      W.layer.opaque = NO;
      W.soft.opaque = NO;
    }

    W.window.contentView = content;
    [W.window makeFirstResponder:W.view];
    [W.window center];
    fit_layer(W.layer, W.view);

    W.ui = civue_ui_new_sized(W.gfx, scale_factor(), ui_size);

    if (o->icons) {
      civue_icons_use(W.ui->icons, o->icons, o->nicons);
    }

    W.ui->titlebar_glass = W.glass != nil;

    if (W.glass) {
      W.titlebar_observer = [CivueTitlebarObserver new];
      follow_titlebar();
    }

    W.ui->system_dark = system_dark();
    W.app_cb = make(W.ui, arg);

    [W.window makeKeyAndOrderFront:nil];

    // the first frame is drawn before the app runs and the window is sent
    // to the window server with it, instead of once the app has finished
    // launching, tens of milliseconds later. a second frame the first asks
    // for follows at once.
    W.wanted = true;
    draw_now();
    [CATransaction flush];
    draw_now();
    start_link();

    [W.app activate];
  }

  [W.app run];
}

void civue_open_url(const char *url) {
  @autoreleasepool {
    NSURL *u = [NSURL URLWithString:ns_str(url)];

    if (u) {
      [NSWorkspace.sharedWorkspace openURL:u];
    }
  }
}

void civue_on_main(void (*fn)(void *arg), void *arg) {
  if (pthread_main_np()) {
    fn(arg);
    return;
  }

  dispatch_sync(dispatch_get_main_queue(), ^{ fn(arg); });
}
