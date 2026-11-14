#import <AppKit/AppKit.h>

#include "darwin.h"

#include <civue/icons.h>

bool civue_svg_rasterize(const char *svg, usize n, u32 w, u32 h,
                         civue_bytes *rgba) {
  @autoreleasepool {
    NSData *data = [NSData dataWithBytesNoCopy:(void *)svg
                                        length:n
                                  freeWhenDone:NO];
    NSImage *img = [[NSImage alloc] initWithData:data];

    if (!img) {
      return false;
    }

    usize size = (usize)w * h * 4;
    rgba->len = 0;
    vec_reserve(rgba, size);
    memset(rgba->v, 0, size);
    rgba->len = size;

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx =
      CGBitmapContextCreate(rgba->v, w, h, 8, w * 4, cs,
                            (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(cs);

    if (!ctx) {
      return false;
    }

    NSGraphicsContext *saved = NSGraphicsContext.currentContext;
    NSGraphicsContext.currentContext =
      [NSGraphicsContext graphicsContextWithCGContext:ctx flipped:NO];
    [img drawInRect:NSMakeRect(0, 0, w, h)
           fromRect:NSZeroRect
          operation:NSCompositingOperationSourceOver
           fraction:1];
    NSGraphicsContext.currentContext = saved;
    CGContextRelease(ctx);
    return true;
  }
}
