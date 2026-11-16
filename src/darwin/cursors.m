#import <AppKit/AppKit.h>

#include "darwin.h"
#include "window.h"

// a cursor made once for each look, by kind and angle, NSNull for the
// system's own.
static NSMutableDictionary<NSNumber *, id> *made;

// an NSCursor holding the app's image at 1x, 2x and 4x, so it stays sharp on
// every display. turned cursors skip the 4x one, there are many of them.
static NSCursor *make(const civue_window_options *o, civue_cursor c) {
  if (!o->cursor_image) {
    return nil;
  }

  f32 points = o->cursor_points ? o->cursor_points : 32;
  u32 most = o->cursor_pixels ? o->cursor_pixels : 128;
  bool turned = c.kind == CIVUE_CURSOR_RESIZE || c.kind == CIVUE_CURSOR_ROTATE;
  u32 sizes[] = {(u32)points, (u32)points * 2, most};
  NSSize size = {points, points};
  NSImage *image = nil;
  NSPoint hot = {};

  for (u32 k = 0; k < (turned ? 2u : 3u); k++) {
    u32 pixels = sizes[k];
    civue_cursor_image img = {};

    if (!o->cursor_image(c, pixels, &img)) {
      break;
    }

    if (!image) {
      image = [[NSImage alloc] initWithSize:size];
      hot = (NSPoint){img.hot_x, img.hot_y};
    }

    NSBitmapImageRep *rep =
      [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                              pixelsWide:pixels
                                              pixelsHigh:pixels
                                           bitsPerSample:8
                                         samplesPerPixel:4
                                                hasAlpha:YES
                                                isPlanar:NO
                                          colorSpaceName:NSDeviceRGBColorSpace
                                             bytesPerRow:pixels * 4
                                            bitsPerPixel:32];

    if (rep) {
      // premultiplied rows, as the rep expects by default.
      memcpy(rep.bitmapData, img.rgba, (usize)pixels * pixels * 4);
      rep.size = size;
      [image addRepresentation:rep];
    }

    free(img.rgba);
  }

  return image ? [[NSCursor alloc] initWithImage:image hotSpot:hot] : nil;
}

bool civue__cursor_apply(const civue_window_options *o, civue_cursor c) {
  if (!made) {
    made = [NSMutableDictionary dictionary];
  }

  NSNumber *key = @((i64)c.kind << 16 | (u16)c.angle);
  id cursor = made[key];

  if (!cursor) {
    @autoreleasepool {
      cursor = make(o, c) ?: NSNull.null;
    }

    made[key] = cursor;
  }

  if (cursor == NSNull.null) {
    return false;
  }

  [(NSCursor *)cursor set];
  return true;
}
