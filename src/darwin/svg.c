#include "darwin.h"

#include <civue/icons.h>

bool civue_svg_rasterize(const char *svg, usize n, u32 w, u32 h, civue_bytes *rgba) {
  bool ok = false;
  AUTORELEASE({
    id data = ns_data_nocopy(svg, n);
    id img = msg1(id, msg0(id, CLS("NSImage"), "alloc"), "initWithData:", data);
    if (img) {
      rgba->len = 0;
      vec_reserve(rgba, (usize)w * h * 4);
      memset(rgba->v, 0, (usize)w * h * 4);
      rgba->len = (usize)w * h * 4;
      CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
      CGContextRef ctx = CGBitmapContextCreate(rgba->v, w, h, 8, w * 4, cs, kCGImageAlphaPremultipliedLast);
      CGColorSpaceRelease(cs);
      if (ctx) {
        id gc = msg2(id, CLS("NSGraphicsContext"), "graphicsContextWithCGContext:flipped:", (void *)ctx, (BOOL)NO);
        id saved = msg0(id, CLS("NSGraphicsContext"), "currentContext");
        msg1(void, CLS("NSGraphicsContext"), "setCurrentContext:", gc);
        msg4(void, img, "drawInRect:fromRect:operation:fraction:", ((CGRect){{0, 0}, {w, h}}), ((CGRect){}),
             (NSUInteger)2, (CGFloat)1);
        msg1(void, CLS("NSGraphicsContext"), "setCurrentContext:", saved);
        CGContextRelease(ctx);
        ok = true;
      }
      release(img);
    }
  });
  return ok;
}
