#import <AppKit/AppKit.h>

#include "darwin.h"

#include <civue/system.h>

#include <string.h>

// each utf-16 offset of `text` as a code point index, one past its end too.
static civue_u32s code_points(const char *text) {
  civue_u32s at = {};
  usize n = strlen(text), i = 0;
  u32 cp = 0;

  while (i < n) {
    u32 c = utf8_next(text, n, &i);
    vec_push(&at, cp);

    if (c > 0xffff) {
      vec_push(&at, cp);
    }

    cp++;
  }

  vec_push(&at, cp);
  return at;
}

// the utf-16 offset of code point `cp`.
static u32 unit_of(const civue_u32s *at, u32 cp) {
  for (u32 u = 0; u < at->len; u++) {
    if (at->v[u] == cp) {
      return u;
    }
  }

  return at->len - 1;
}

civue_u32s civue_spell_misspelled(const char *text) {
  civue_u32s out = {};

  if (!text || !*text) {
    return out;
  }

  civue_u32s at = code_points(text);

  @autoreleasepool {
    NSSpellChecker *checker = NSSpellChecker.sharedSpellChecker;
    NSString *s = ns_str(text);
    NSInteger from = 0, len = (NSInteger)s.length;

    while (from < len) {
      NSRange r = [checker checkSpellingOfString:s startingAt:from];

      if (r.location == NSNotFound || !r.length ||
          (NSInteger)r.location < from) {
        break;
      }

      u32 end = (u32)(r.location + r.length);

      if (end >= at.len) {
        break;
      }

      vec_push(&out, at.v[r.location]);
      vec_push(&out, at.v[end]);
      from = end;
    }
  }

  vec_free(&at);
  return out;
}

civue_strs civue_spell_guesses(const char *text, u32 a, u32 b) {
  civue_strs out = {};

  if (!text || b <= a) {
    return out;
  }

  civue_u32s at = code_points(text);
  u32 ua = unit_of(&at, a), ub = unit_of(&at, b);

  @autoreleasepool {
    NSSpellChecker *checker = NSSpellChecker.sharedSpellChecker;
    NSArray<NSString *> *guesses =
      [checker guessesForWordRange:NSMakeRange(ua, ub - ua)
                          inString:ns_str(text)
                          language:checker.language
            inSpellDocumentWithTag:0];

    for (NSString *guess in guesses) {
      vec_push(&out, xstrdup(guess.UTF8String));
    }
  }

  vec_free(&at);
  return out;
}

char *civue_spell_language() {
  @autoreleasepool {
    return xstrdup(NSSpellChecker.sharedSpellChecker.language.UTF8String ?: "");
  }
}

void civue_spell_set_language(const char *code) {
  @autoreleasepool {
    [NSSpellChecker.sharedSpellChecker setLanguage:ns_str(code)];
  }
}
