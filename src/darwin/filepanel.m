#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "darwin.h"

#include <civue/system.h>

civue_strs civue_file_panel_open(const char *const *extensions, u32 n,
                                 bool multiple) {
  civue_strs out = {};

  @autoreleasepool {
    NSOpenPanel *panel = NSOpenPanel.openPanel;
    panel.canChooseDirectories = NO;
    panel.canChooseFiles = YES;
    panel.allowsMultipleSelection = multiple;

    NSMutableArray<UTType *> *types = [NSMutableArray array];

    for (u32 i = 0; i < n; i++) {
      UTType *type = [UTType typeWithFilenameExtension:ns_str(extensions[i])];

      if (type) {
        [types addObject:type];
      }
    }

    if (types.count) {
      panel.allowedContentTypes = types;
    }

    if ([panel runModal] == NSModalResponseOK) {
      for (NSURL *url in panel.URLs) {
        char *path = ns_dup(url.path);

        if (path) {
          vec_push(&out, path);
        }
      }
    }
  }

  return out;
}
