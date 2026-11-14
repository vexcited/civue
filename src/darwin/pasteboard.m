#import <AppKit/AppKit.h>

#include "darwin.h"

#include <civue/system.h>

bool civue_pasteboard_image(civue_bytes *out, char **mime,
                            civue_image_convert convert) {
  *out = (civue_bytes){};
  *mime = nullptr;

  // what apps read as it is, then what is converted.
  static const struct {
    const char *type, *mime;
    bool converted;
  } types[] = {
    {"public.png", "image/png"},         {"public.jpeg", "image/jpeg"},
    {"com.compuserve.gif", "image/gif"}, {"org.webmproject.webp", "image/webp"},
    {"public.heic", "image/heic", true}, {"public.avif", "image/avif", true},
    {"public.tiff", "image/tiff", true},
  };

  @autoreleasepool {
    NSPasteboard *board = NSPasteboard.generalPasteboard;

    for (u32 i = 0; i < countof(types) && !out->len; i++) {
      NSData *data = [board dataForType:ns_str(types[i].type)];

      if (!data.length) {
        continue;
      }

      civue_bytes raw = {};
      buf_put(&raw, data.bytes, data.length);

      if (!types[i].converted || !convert) {
        *out = raw;
        *mime = xstrdup(types[i].mime);
        continue;
      }

      const char *converted;

      if (convert(&raw, out, &converted)) {
        *mime = xstrdup(converted);
      }

      vec_free(&raw);
    }
  }

  return out->len != 0;
}

civue_strs civue_pasteboard_files() {
  civue_strs out = {};

  @autoreleasepool {
    NSPasteboard *board = NSPasteboard.generalPasteboard;

    for (NSPasteboardItem *item in board.pasteboardItems) {
      NSString *url = [item stringForType:NSPasteboardTypeFileURL];
      NSURL *file = url ? [NSURL URLWithString:url] : nil;

      if (file.isFileURL && file.path) {
        vec_push(&out, ns_dup(file.path));
      }
    }
  }

  return out;
}

bool civue_pasteboard_holds_other() {
  @autoreleasepool {
    return NSPasteboard.generalPasteboard.types.count > 0;
  }
}
