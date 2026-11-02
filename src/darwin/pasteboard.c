#include "darwin.h"

#include <civue/system.h>

bool civue_pasteboard_image(civue_bytes *out, char **mime, civue_image_convert convert) {
  *out = (civue_bytes){};
  *mime = nullptr;
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    // what apps read as it is, then what is converted.
    static const struct {
      const char *type, *mime;
      bool converted;
    } types[] = {{"public.png", "image/png"},         {"public.jpeg", "image/jpeg"},
                 {"com.compuserve.gif", "image/gif"}, {"org.webmproject.webp", "image/webp"},
                 {"public.heic", "image/heic", true}, {"public.avif", "image/avif", true},
                 {"public.tiff", "image/tiff", true}};
    for (u32 i = 0; i < countof(types) && !out->len; i++) {
      id data = msg1(id, board, "dataForType:", ns_str(types[i].type));
      NSUInteger n = data ? msg0(NSUInteger, data, "length") : 0;
      if (!n) continue;
      civue_bytes raw = {};
      buf_put(&raw, msg0(const void *, data, "bytes"), n);
      if (!types[i].converted || !convert) {
        *out = raw;
        *mime = xstrdup(types[i].mime);
        continue;
      }
      const char *converted;
      if (convert(&raw, out, &converted)) *mime = xstrdup(converted);
      vec_free(&raw);
    }
  });
  return out->len != 0;
}

civue_strs civue_pasteboard_files(void) {
  civue_strs out = {};
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    id items = msg0(id, board, "pasteboardItems");
    for (NSUInteger i = 0; i < ns_count(items); i++) {
      id url = msg1(id, ns_at(items, i), "stringForType:", ns_str("public.file-url"));
      id file = url ? msg1(id, CLS("NSURL"), "URLWithString:", url) : nullptr;
      id path = file && msg0(bool, file, "isFileURL") ? msg0(id, file, "path") : nullptr;
      if (path) vec_push(&out, ns_dup(path));
    }
  });
  return out;
}

bool civue_pasteboard_holds_other(void) {
  bool any = false;
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    any = ns_count(msg0(id, board, "types")) > 0;
  });
  return any;
}
