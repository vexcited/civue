#include "words.h"

#include <stdint.h>
#include <string.h>

static const char *const WORDS[32] = {
  "window", "button",  "layout",  "render",  "scroll", "update", "shadow",
  "cursor", "memory",  "thread",  "signal",  "bitmap", "stroke", "column",
  "editor", "kernel",  "packet",  "vector",  "syntax", "buffer", "format",
  "cipher", "socket",  "module",  "branch",  "commit", "server", "client",
  "screen", "pointer", "display", "palette",
};

void words(char *out, size_t size, unsigned seed, int n) {
  uint32_t x = seed;
  size_t at = 0;

  for (int i = 0; i < n; i++) {
    x = x * 1103515245u + 12345u;

    const char *word = WORDS[(x >> 16) % 32];
    size_t len = strlen(word);

    if (at + len + 2 > size) {
      break;
    }

    if (i > 0) {
      out[at++] = ' ';
    }

    memcpy(out + at, word, len);
    at += len;
  }

  out[at] = 0;
}

double cell_value(int cell, int frame) {
  return (double)((cell * 7919 + frame * 31) % 100000) / 100;
}
