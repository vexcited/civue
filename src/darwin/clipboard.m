#import <AppKit/AppKit.h>

#include "darwin.h"

#include <civue/system.h>

char *civue_clipboard_text() {
  @autoreleasepool {
    NSPasteboard *board = NSPasteboard.generalPasteboard;
    return ns_dup([board stringForType:NSPasteboardTypeString]);
  }
}

char *civue_clipboard_html() {
  @autoreleasepool {
    NSPasteboard *board = NSPasteboard.generalPasteboard;
    return ns_dup([board stringForType:NSPasteboardTypeHTML]);
  }
}

void civue_clipboard_set_text(const char *s, usize n) {
  @autoreleasepool {
    NSPasteboard *board = NSPasteboard.generalPasteboard;
    [board clearContents];
    [board setString:ns_strn(s, n) forType:NSPasteboardTypeString];
  }
}

void civue_clipboard_set_png(const u8 *png, usize n) {
  @autoreleasepool {
    NSPasteboard *board = NSPasteboard.generalPasteboard;
    [board clearContents];
    [board setData:[NSData dataWithBytes:png length:n]
           forType:NSPasteboardTypePNG];
  }
}

void civue_clipboard_set_html(const char *html, const char *text) {
  @autoreleasepool {
    NSPasteboard *board = NSPasteboard.generalPasteboard;
    [board clearContents];
    [board setString:ns_str(html) forType:NSPasteboardTypeHTML];
    [board setString:ns_str(text) forType:NSPasteboardTypeString];
  }
}
