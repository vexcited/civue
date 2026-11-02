#include "darwin.h"
#include "window.h"

extern id NSDeviceRGBColorSpace;

// a cursor made once for each look, by kind and angle, the system's remembered as none.
typedef struct {
  civue_cursor_kind kind;
  i16 angle;
  id cursor;
} Made;

static Vec(Made) made;

// an NSCursor holding the app's image at 1x, 2x and 4x, so it stays sharp on every display. turned cursors skip
// the 4x one, there are many of them.
static id make(const civue_window_options *o, civue_cursor c) {
  if (!o->cursor_image) return nullptr;
  f32 points = o->cursor_points ? o->cursor_points : 32;
  u32 most = o->cursor_pixels ? o->cursor_pixels : 128;
  bool turned = c.kind == CIVUE_CURSOR_RESIZE || c.kind == CIVUE_CURSOR_ROTATE;
  u32 sizes[] = {(u32)points, (u32)points * 2, most};
  NSSize size = {points, points};
  id image = nullptr;
  NSPoint hot = {};
  for (u32 k = 0; k < (turned ? 2u : 3u); k++) {
    u32 pixels = sizes[k];
    civue_cursor_image img = {};
    if (!o->cursor_image(c, pixels, &img)) break;
    if (!image) {
      image = msg1(id, msg0(id, CLS("NSImage"), "alloc"), "initWithSize:", size);
      hot = (NSPoint){img.hot_x, img.hot_y};
    }
    id rep = msg0(id, CLS("NSBitmapImageRep"), "alloc");
    rep = ((id (*)(id, SEL, unsigned char **, NSInteger, NSInteger, NSInteger, NSInteger, BOOL, BOOL, id, NSInteger,
                   NSInteger))objc_msgSend)(rep,
                                            SEL_("initWithBitmapDataPlanes:pixelsWide:pixelsHigh:bitsPerSample:"
                                                 "samplesPerPixel:hasAlpha:isPlanar:colorSpaceName:bytesPerRow:"
                                                 "bitsPerPixel:"),
                                            nullptr, pixels, pixels, 8, 4, YES, NO, NSDeviceRGBColorSpace, pixels * 4,
                                            32);
    if (rep) {
      // premultiplied rows, as the rep expects by default.
      memcpy(msg0(void *, rep, "bitmapData"), img.rgba, (usize)pixels * pixels * 4);
      msg1(void, rep, "setSize:", size);
      msg1(void, image, "addRepresentation:", rep);
      release(rep);
    }
    free(img.rgba);
  }
  if (!image) return nullptr;
  id cursor = msg2(id, msg0(id, CLS("NSCursor"), "alloc"), "initWithImage:hotSpot:", image, hot);
  release(image);
  return cursor;
}

bool civue__cursor_apply(const civue_window_options *o, civue_cursor c) {
  id cursor = nullptr;
  bool known = false;
  for (u32 i = 0; i < made.len && !known; i++)
    if (made.v[i].kind == c.kind && made.v[i].angle == c.angle) {
      cursor = made.v[i].cursor;
      known = true;
    }
  if (!known) {
    AUTORELEASE(cursor = make(o, c));
    vec_push(&made, ((Made){c.kind, c.angle, cursor}));
  }
  if (!cursor) return false;
  msg0(void, cursor, "set");
  return true;
}
