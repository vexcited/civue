#include "darwin.h"

#include <civue/system.h>

extern id NSPasteboardTypeString, NSPasteboardTypeHTML, NSPasteboardTypePNG;

char *civue_clipboard_text(void) {
  char *out = nullptr;
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    id s = msg1(id, board, "stringForType:", NSPasteboardTypeString);
    out = ns_dup(s);
  });
  return out;
}

char *civue_clipboard_html(void) {
  char *out = nullptr;
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    id s = msg1(id, board, "stringForType:", NSPasteboardTypeHTML);
    out = ns_dup(s);
  });
  return out;
}

void civue_clipboard_set_text(const char *s, usize n) {
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    msg0(NSInteger, board, "clearContents");
    msg2(BOOL, board, "setString:forType:", ns_strn(s, n), NSPasteboardTypeString);
  });
}

void civue_clipboard_set_png(const u8 *png, usize n) {
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    msg0(NSInteger, board, "clearContents");
    msg2(BOOL, board, "setData:forType:", ns_data(png, n), NSPasteboardTypePNG);
  });
}

void civue_clipboard_set_html(const char *html, const char *text) {
  AUTORELEASE({
    id board = msg0(id, CLS("NSPasteboard"), "generalPasteboard");
    msg0(NSInteger, board, "clearContents");
    msg2(BOOL, board, "setString:forType:", ns_str(html), NSPasteboardTypeHTML);
    msg2(BOOL, board, "setString:forType:", ns_str(text), NSPasteboardTypeString);
  });
}
