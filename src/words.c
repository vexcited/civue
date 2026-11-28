#include "internal.h"

#include <civue/text.h>

/* ========================================================================== */
/* words, the segments of unicode's UAX #29 as ICU cuts them, which is what   */
/* chromium selects by: letters, digits and underscores run on, spaces run    */
/* on, and any other sign stands alone. the scripts written without spaces    */
/* are cut by the system's dictionaries, as ICU's own cut them.               */
/* ========================================================================== */

typedef enum : u8 {
  OTHER,
  LETTER,
  DIGIT,
  // joins letters and digits alike.
  LOW_LINE,
  SPACE,
  // an apostrophe or a full stop, between two letters or two digits.
  MID_BOTH,
  // a middle dot, between two letters.
  MID_LETTER,
  // a comma or a semicolon, between two digits.
  MID_DIGIT,
  // a mark that goes with what is before it.
  EXTEND,
  JOINER,
  PICTURE,
  FLAG,
  // a syllable of hangul, which joins no other letter.
  HANGUL,
  // han and kana, written without spaces: a dictionary cuts their words.
  KANA_KANJI,
  // thai, lao, burmese and khmer, cut by a dictionary too.
  COMPLEX,
} Kind;

static Kind kind_of(u32 c) {
  if (c < 0x80) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
      return LETTER;
    }

    if (c >= '0' && c <= '9') {
      return DIGIT;
    }

    switch (c) {
      case '_':
        return LOW_LINE;
      case ' ':
        return SPACE;
      case '\'':
      case '.':
        return MID_BOTH;
      case ',':
      case ';':
        return MID_DIGIT;
      default:
        return OTHER;
    }
  }

  if (c == 0x200d) {
    return JOINER;
  }

  // combining marks, the soft hyphen, variation selectors, skin tones, tags.
  if ((c >= 0x300 && c <= 0x36f) || c == 0xad || c == 0x200c ||
      (c >= 0x20d0 && c <= 0x20ff) || (c >= 0xfe00 && c <= 0xfe0f) ||
      (c >= 0x1f3fb && c <= 0x1f3ff) || (c >= 0xe0020 && c <= 0xe007f)) {
    return EXTEND;
  }

  if (c == 0x2018 || c == 0x2019 || c == 0x2024 || c == 0xff07 ||
      c == 0xff0e) {
    return MID_BOTH;
  }

  if (c == 0xb7 || c == 0x2027) {
    return MID_LETTER;
  }

  if (c == 0xaa || c == 0xb5 || c == 0xba) {
    return LETTER;
  }

  if (c == 0x1680 || (c >= 0x2000 && c <= 0x2006) ||
      (c >= 0x2008 && c <= 0x200a) || c == 0x205f || c == 0x3000) {
    return SPACE;
  }

  if (c >= 0x1f1e6 && c <= 0x1f1ff) {
    return FLAG;
  }

  if (c == 0xa9 || c == 0xae || c == 0x203c || c == 0x2049 || c == 0x2122 ||
      c == 0x2139 || (c >= 0x2194 && c <= 0x21aa) ||
      (c >= 0x231a && c <= 0x23ff) || (c >= 0x25aa && c <= 0x27bf) ||
      (c >= 0x2934 && c <= 0x2935) || (c >= 0x2b05 && c <= 0x2b55) ||
      c == 0x3030 || c == 0x303d || c == 0x3297 || c == 0x3299 ||
      (c >= 0x1f000 && c <= 0x1faff)) {
    return PICTURE;
  }

  if ((c >= 0xe00 && c <= 0xeff) || (c >= 0x1000 && c <= 0x109f) ||
      (c >= 0x1780 && c <= 0x17ff)) {
    return COMPLEX;
  }

  if (c >= 0xac00 && c <= 0xd7a3) {
    return HANGUL;
  }

  // the marks that repeat an ideograph, kana without the dot that parts two
  // names, then han.
  if ((c >= 0x3005 && c <= 0x3007) || (c >= 0x3021 && c <= 0x3029) ||
      (c >= 0x3038 && c <= 0x303b) ||
      (c >= 0x3041 && c <= 0x30ff && c != 0x30fb) ||
      (c >= 0x31f0 && c <= 0x31ff) || (c >= 0x3400 && c <= 0x4dbf) ||
      (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
      (c >= 0xff66 && c <= 0xff9f) || (c >= 0x20000 && c <= 0x3ffff)) {
    return KANA_KANJI;
  }

  // signs of latin-1, punctuation, symbols, arrows and shapes, then those
  // of the far east.
  if ((c >= 0xa0 && c <= 0xbf) || c == 0xd7 || c == 0xf7 ||
      (c >= 0x2000 && c <= 0x206f) || (c >= 0x20a0 && c <= 0x20cf) ||
      (c >= 0x2100 && c <= 0x2bff) || (c >= 0x2e00 && c <= 0x2e7f) ||
      (c >= 0x3000 && c <= 0x303f) || c == 0x30fb ||
      (c >= 0xfe10 && c <= 0xfe6f) || (c >= 0xff00 && c <= 0xff0f) ||
      (c >= 0xff1a && c <= 0xff20) || (c >= 0xff3b && c <= 0xff40) ||
      (c >= 0xff5b && c <= 0xff65)) {
    return OTHER;
  }

  return LETTER;
}

// the kind of the character at `*i`, which moves past it and its marks.
// `joined` when the last of them joins it to a picture.
static Kind take(const char *s, usize n, usize *i, bool *joined) {
  Kind k = kind_of(civue_utf8_next(s, n, i));

  *joined = false;

  while (*i < n) {
    usize next = *i;
    Kind mark = kind_of(civue_utf8_next(s, n, &next));

    if (mark != EXTEND && mark != JOINER) {
      break;
    }

    *joined = mark == JOINER;
    *i = next;
  }

  return k;
}

static bool wordy(Kind k) {
  return k == LETTER || k == DIGIT || k == LOW_LINE;
}

// the kinds that run on with their own and with nothing else.
static bool runs(Kind k) {
  return k == SPACE || k == HANGUL || k == KANA_KANJI || k == COMPLEX;
}

static bool joins(Kind before, Kind mid, Kind after) {
  bool letters = before == LETTER && after == LETTER;
  bool digits = before == DIGIT && after == DIGIT;

  switch (mid) {
    case MID_BOTH:
      return letters || digits;
    case MID_LETTER:
      return letters;
    case MID_DIGIT:
      return digits;
    default:
      return false;
  }
}

// where the segment that starts at `i` ends, before `n`.
static usize segment_end(const char *s, usize n, usize i) {
  bool joined;
  Kind k = take(s, n, &i, &joined);

  while (i < n) {
    usize next = i, after;
    bool joins_next, unused;
    Kind then = take(s, n, &next, &joins_next);

    if (wordy(k) && wordy(then)) {
      k = then;
    }
    else if (wordy(k) && next < n) {
      after = next;

      Kind last = take(s, n, &after, &unused);

      if (!joins(k, then, last)) {
        break;
      }

      k = last;
      next = after;
    }
    else if (k == PICTURE && joined && then == PICTURE) {
      joined = joins_next;
    }
    else if (k == FLAG && then == FLAG) {
      // a flag is two letters, a third starts the next one.
      k = OTHER;
    }
    else if (k != then || !runs(k)) {
      break;
    }

    i = next;
  }

  return i;
}

// how many characters of a run the dictionaries are given on each side of
// the caret. a word is far shorter, and a run of chinese can be a page.
constexpr u32 NEAR = 64;

// narrows `*from` to `*to`, a run of a script written without spaces, to
// the word of it the caret `at` is in, the one that ends there when
// `before`.
static void dictionary_word(const char *s, u32 at, bool before, u32 *from,
                            u32 *to) {
  usize i = *from, end = *to;
  bool unused;
  Kind k = take(s, end, &i, &unused);

  if (k != KANA_KANJI && k != COMPLEX) {
    return;
  }

  u32 lo = at;

  for (u32 back = 0; back < NEAR && lo > *from; back++) {
    do {
      lo--;
    } while (lo > *from && ((u8)s[lo] & 0xc0) == 0x80);
  }

  // utf-16 for the system, each unit's character found again by its byte.
  u16 units[NEAR * 4];
  u32 bytes[NEAR * 4 + 1];
  u32 n = 0, after = 0;

  for (i = lo; i < end && (i < at || after < NEAR);) {
    after += i >= at;
    bytes[n] = (u32)i;

    u32 c = civue_utf8_next(s, end, &i);

    if (c >= 0x10000) {
      units[n++] = (u16)(0xd7c0 + (c >> 10));
      bytes[n] = bytes[n - 1];
      c = 0xdc00 | (c & 0x3ff);
    }

    units[n++] = (u16)c;
  }

  bytes[n] = (u32)i;

  u32 caret = 0, a, b;

  while (caret < n && bytes[caret] < at) {
    caret++;
  }

  caret -= before;

  if (civue__word_at(units, n, caret, &a, &b) && a < b && b <= n) {
    *from = bytes[a];
    *to = bytes[b];
  }
  else if (k == KANA_KANJI) {
    // without a dictionary an ideograph is a word.
    i = bytes[caret];
    *from = (u32)i;
    take(s, end, &i, &unused);
    *to = (u32)i;
  }
}

civue_text_unit civue_text_unit_of(uint32_t clicks) {
  return clicks >= 3   ? CIVUE_UNIT_PARAGRAPH
         : clicks == 2 ? CIVUE_UNIT_WORD
                       : CIVUE_UNIT_CHAR;
}

void civue_text_unit_at(const char *s, size_t n, uint32_t at,
                        civue_text_unit unit, bool line_end, uint32_t *from,
                        uint32_t *to) {
  at = MIN(at, (u32)n);
  *from = *to = at;

  if (unit == CIVUE_UNIT_CHAR) {
    return;
  }

  u32 start = at, end = at;

  while (start > 0 && s[start - 1] != '\n') {
    start--;
  }

  while (end < n && s[end] != '\n') {
    end++;
  }

  // the break that ends a paragraph is selected with it, and alone from
  // past its last word.
  u32 past = end < n ? end + 1 : end;

  if (unit == CIVUE_UNIT_PARAGRAPH) {
    *from = start;
    *to = past;
    return;
  }

  if (at == end && end < n) {
    *from = end;
    *to = past;
    return;
  }

  bool before = line_end || at == end;

  // the spaces a wrapped line ends with hang past it, its last word is the
  // one before them.
  while (line_end && at > start && s[at - 1] == ' ') {
    at--;
  }

  for (usize i = start; i < end;) {
    usize e = segment_end(s, end, i);

    if (before ? at <= e : at < e) {
      *from = (u32)i;
      *to = (u32)e;
      dictionary_word(s, at, before && at > i, from, to);
      return;
    }

    i = e;
  }
}

void civue_text_press(civue_text_grip *g, const char *s, size_t n,
                      uint32_t at, bool line_end, uint32_t clicks, bool extend,
                      uint32_t *anchor, uint32_t *focus) {
  u32 from, to;

  at = MIN(at, (u32)n);

  if (!extend) {
    g->unit = civue_text_unit_of(clicks);
    civue_text_unit_at(s, n, at, g->unit, line_end, &from, &to);
    g->base = at;
    g->from = *anchor = from;
    g->to = *focus = to;
    return;
  }

  bool back = at < *anchor;
  u32 keep = *anchor;

  civue_text_unit_at(s, n, at, g->unit, line_end, &from, &to);

#ifdef __APPLE__
  // on a mac a selection has no direction, its nearer end moves.
  u32 lo = MIN(*anchor, *focus), hi = MAX(*anchor, *focus);

  back = at < lo || (at <= hi && at - lo <= hi - at);
  keep = back ? hi : lo;
#endif

  *anchor = keep;
  *focus = back ? from : to;
  g->base = g->from = g->to = keep;
}

void civue_text_drag(const civue_text_grip *g, const char *s, size_t n,
                     uint32_t at, bool line_end, uint32_t *anchor,
                     uint32_t *focus) {
  u32 from, to;

  at = MIN(at, (u32)n);
  civue_text_unit_at(s, n, at, g->unit, line_end, &from, &to);

  if (at < g->base) {
    *anchor = MIN(g->to, (u32)n);
    *focus = from;
  }
  else {
    *anchor = MIN(g->from, (u32)n);
    *focus = to;
  }
}
