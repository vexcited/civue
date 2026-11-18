#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>

#include "darwin.h"

// an image no window could show whole, refused rather than decoded.
static const usize decoded_pixels_max = 64u << 20;

static CGImageRef create_image(CFDataRef data, u32 max_pixels) {
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);

  if (!source) {
    return nullptr;
  }

  CGImageRef image = nullptr;

  if (CGImageSourceGetCount(source) > 0) {
    if (max_pixels > 0) {
      // decoded at the size asked for, never at its own then scaled.
      NSDictionary *options = @{
        (__bridge id)kCGImageSourceCreateThumbnailFromImageAlways : @YES,
        (__bridge id)kCGImageSourceCreateThumbnailWithTransform : @YES,
        (__bridge id)kCGImageSourceThumbnailMaxPixelSize : @(max_pixels),
      };
      image = CGImageSourceCreateThumbnailAtIndex(
        source, 0, (__bridge CFDictionaryRef)options);
    }
    else {
      image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
    }
  }

  CFRelease(source);
  return image;
}

u8 *civue__image_decode(const u8 *bytes, usize n, u32 max_pixels, u32 *width,
                        u32 *height) {
  @autoreleasepool {
    CFDataRef data =
      CFDataCreateWithBytesNoCopy(nullptr, bytes, (CFIndex)n, kCFAllocatorNull);
    CGImageRef image = data ? create_image(data, max_pixels) : nullptr;

    if (data) {
      CFRelease(data);
    }

    if (!image) {
      return nullptr;
    }

    usize w = CGImageGetWidth(image), h = CGImageGetHeight(image);
    u8 *rgba = nullptr;

    if (w != 0 && h != 0 && w * h <= decoded_pixels_max) {
      rgba = calloc(w * h, 4);
    }

    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef ctx =
      rgba
        ? CGBitmapContextCreate(rgba, w, h, 8, w * 4, space,
                                (CGBitmapInfo)kCGImageAlphaPremultipliedLast |
                                  kCGBitmapByteOrder32Big)
        : nullptr;
    CGColorSpaceRelease(space);

    if (ctx) {
      CGContextSetBlendMode(ctx, kCGBlendModeCopy);
      CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
      CGContextRelease(ctx);
      *width = (u32)w;
      *height = (u32)h;
    }
    else {
      free(rgba);
      rgba = nullptr;
    }

    CGImageRelease(image);
    return rgba;
  }
}
