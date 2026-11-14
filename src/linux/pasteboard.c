#include "wl.h"

#include <civue/system.h>

bool civue_pasteboard_image(civue_bytes *out, char **mime,
                            civue_image_convert convert) {
  *out = (civue_bytes){};
  *mime = nullptr;

  // what apps read as it is, then what is converted.
  static const struct {
    const char *type;
    bool converted;
  } types[] = {{"image/png"},        {"image/jpeg"},       {"image/gif"},
               {"image/webp"},       {"image/heic", true}, {"image/heif", true},
               {"image/avif", true}, {"image/tiff", true}};

  for (u32 i = 0; i < countof(types) && !out->len; i++) {
    civue_bytes raw = {};
    if (!civue__clip_read(types[i].type, &raw)) {
      continue;
    }
    if (!types[i].converted || !convert) {
      *out = raw;
      *mime = xstrdup(types[i].type);
      continue;
    }
    const char *converted;
    if (convert(&raw, out, &converted)) {
      *mime = xstrdup(converted);
    }
    vec_free(&raw);
  }
  return out->len != 0;
}

static int hex(int c) {
  return c >= '0' && c <= '9'   ? c - '0'
         : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                : -1;
}

char *civue__file_path(const char *url, usize n) {
  if (n < 7 || strncmp(url, "file://", 7)) {
    return nullptr;
  }
  url += 7, n -= 7;
  // a host before the path, usually empty or localhost.
  const char *slash = memchr(url, '/', n);
  if (!slash) {
    return nullptr;
  }
  n -= slash - url;
  url = slash;
  char *out = xmalloc(n + 1);
  usize k = 0;
  for (usize i = 0; i < n; i++) {
    int a, b;
    if (url[i] == '%' && i + 2 < n && (a = hex(url[i + 1])) >= 0 &&
        (b = hex(url[i + 2])) >= 0) {
      out[k++] = (char)(a << 4 | b);
      i += 2;
    }
    else {
      out[k++] = url[i];
    }
  }
  out[k] = 0;
  return out;
}

civue_strs civue_pasteboard_files(void) {
  civue_strs out = {};
  // file managers copy a list of urls, gnome's with the operation first.
  civue_bytes list = {};
  if (!civue__clip_read("text/uri-list", &list) &&
      !civue__clip_read("x-special/gnome-copied-files", &list)) {
    return out;
  }
  usize i = 0;
  while (i < list.len) {
    const char *line = (const char *)list.v + i;
    usize n = 0;
    while (i + n < list.len && line[n] != '\n' && line[n] != '\r') {
      n++;
    }
    i += n + 1;
    if (!n || line[0] == '#') {
      continue;
    }
    char *path = civue__file_path(line, n);
    if (path) {
      vec_push(&out, path);
    }
  }
  vec_free(&list);
  return out;
}

bool civue_pasteboard_holds_other(void) {
  return civue__clip_any();
}
