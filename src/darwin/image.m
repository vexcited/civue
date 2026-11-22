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

typedef struct {
  CFStringRef dictionary, unclamped, clamped;
} DelayKeys;

// browsers show a frame that asks for 10 ms or less for 100 ms, and so do
// the gifs made for them.
static f32 frame_delay(CGImageSourceRef source, usize index) {
  const DelayKeys kinds[] = {
    {kCGImagePropertyGIFDictionary, kCGImagePropertyGIFUnclampedDelayTime,
     kCGImagePropertyGIFDelayTime},
    {kCGImagePropertyWebPDictionary, kCGImagePropertyWebPUnclampedDelayTime,
     kCGImagePropertyWebPDelayTime},
    {kCGImagePropertyPNGDictionary, kCGImagePropertyAPNGUnclampedDelayTime,
     kCGImagePropertyAPNGDelayTime},
  };
  NSDictionary *props = CFBridgingRelease(
    CGImageSourceCopyPropertiesAtIndex(source, index, nullptr));
  f64 delay = 0;

  for (usize i = 0; i < countof(kinds) && delay <= 0; i++) {
    NSDictionary *kind = props[(__bridge id)kinds[i].dictionary];
    NSNumber *got = kind[(__bridge id)kinds[i].unclamped]
                      ?: kind[(__bridge id)kinds[i].clamped];

    delay = got.doubleValue;
  }

  return delay <= 0.01 ? 0.1f : (f32)delay;
}

static bool draw_frame(CGImageRef image, u8 *rgba, u32 w, u32 h) {
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef ctx = CGBitmapContextCreate(
    rgba, w, h, 8, (usize)w * 4, space,
    (CGBitmapInfo)kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);

  CGColorSpaceRelease(space);

  if (!ctx) {
    return false;
  }

  CGContextSetBlendMode(ctx, kCGBlendModeCopy);
  CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
  CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), image);
  CGContextRelease(ctx);
  return true;
}

bool civue__image_decode_frames(const u8 *bytes, usize n, u32 max_pixels,
                                usize max_bytes, civue__frames *out) {
  @autoreleasepool {
    CFDataRef data =
      CFDataCreateWithBytesNoCopy(nullptr, bytes, (CFIndex)n, kCFAllocatorNull);
    CGImageSourceRef source =
      data ? CGImageSourceCreateWithData(data, nullptr) : nullptr;

    if (data) {
      CFRelease(data);
    }

    if (!source) {
      return false;
    }

    usize count = CGImageSourceGetCount(source);
    NSDictionary *first = CFBridgingRelease(
      count > 1 ? CGImageSourceCopyPropertiesAtIndex(source, 0, nullptr)
                : nullptr);
    f64 width = [first[(__bridge id)kCGImagePropertyPixelWidth] doubleValue];
    f64 height = [first[(__bridge id)kCGImagePropertyPixelHeight] doubleValue];

    if (count < 2 || width < 1 || height < 1 ||
        width * height > decoded_pixels_max) {
      CFRelease(source);
      return false;
    }

    f64 longest = MAX(width, height);
    f64 scale = max_pixels > 0 ? MIN(1.0, max_pixels / longest) : 1.0;
    f64 need = width * height * scale * scale * 4.0 * (f64)count;

    if (need > (f64)max_bytes) {
      scale *= sqrt((f64)max_bytes / need);
    }

    u32 w = (u32)MAX(round(width * scale), 1.0);
    u32 h = (u32)MAX(round(height * scale), 1.0);
    usize frame = (usize)w * h * 4;

    // so many frames that each would be a smudge: the first one is better.
    if (MAX(w, h) < MIN(longest, 64.0)) {
      CFRelease(source);
      return false;
    }

    u8 *rgba = calloc(count, frame);
    f32 *delays = calloc(count, sizeof(f32));
    bool ok = rgba && delays;

    for (usize i = 0; ok && i < count; i++) {
      CGImageRef image = CGImageSourceCreateImageAtIndex(source, i, nullptr);

      ok = image && draw_frame(image, rgba + i * frame, w, h);
      delays[i] = frame_delay(source, i);

      if (image) {
        CGImageRelease(image);
      }
    }

    CFRelease(source);

    if (!ok) {
      free(rgba);
      free(delays);
      return false;
    }

    *out = (civue__frames){rgba, delays, w, h, (u32)count};
    return true;
  }
}
