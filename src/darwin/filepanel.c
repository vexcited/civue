#include "darwin.h"

#include <civue/system.h>

civue_strs civue_file_panel_open(const char *const *extensions, u32 n, bool multiple) {
  civue_strs out = {};
  AUTORELEASE({
    id panel = msg0(id, CLS("NSOpenPanel"), "openPanel");
    msg1(void, panel, "setCanChooseDirectories:", (BOOL)NO);
    msg1(void, panel, "setCanChooseFiles:", (BOOL)YES);
    msg1(void, panel, "setAllowsMultipleSelection:", (BOOL)multiple);
    id types = msg0(id, CLS("NSMutableArray"), "array");
    for (u32 i = 0; i < n; i++) {
      id type = msg1(id, CLS("UTType"), "typeWithFilenameExtension:", ns_str(extensions[i]));
      if (type) msg1(void, types, "addObject:", type);
    }
    if (ns_count(types)) msg1(void, panel, "setAllowedContentTypes:", types);
    if (msg0(NSInteger, panel, "runModal") == 1) {
      id urls = msg0(id, panel, "URLs");
      for (NSUInteger i = 0; i < ns_count(urls); i++) {
        char *path = ns_dup(msg0(id, ns_at(urls, i), "path"));
        if (path) vec_push(&out, path);
      }
    }
  });
  return out;
}
