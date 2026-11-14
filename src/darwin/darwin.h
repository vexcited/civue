#ifndef CIVUE_DARWIN_H
#define CIVUE_DARWIN_H

#import <Foundation/Foundation.h>

#include "../internal.h"

// utf-8 as a string, latin-1 when it is not valid utf-8 so the text shows.
static inline NSString *ns_strn(const char *s, usize n) {
  NSString *str = [[NSString alloc] initWithBytes:s
                                           length:n
                                         encoding:NSUTF8StringEncoding];

  if (!str) {
    str = [[NSString alloc] initWithBytes:s
                                   length:n
                                 encoding:NSISOLatin1StringEncoding];
  }

  return str;
}

static inline NSString *ns_str(const char *s) {
  return s ? ns_strn(s, strlen(s)) : @"";
}

// a copy of a string's utf-8 from malloc, nullptr for nil.
static inline char *ns_dup(NSString *s) {
  return s ? xstrdup(s.UTF8String) : nullptr;
}

#endif
