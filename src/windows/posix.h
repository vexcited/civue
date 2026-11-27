#ifndef CIVUE_WINDOWS_POSIX_H
#define CIVUE_WINDOWS_POSIX_H

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ========================================================================== */
/* the little of posix the shared files use that windows has under other     */
/* names, or not at all. included before anything else, so the headers that  */
/* declare these are read before the names are taken.                         */
/* ========================================================================== */

// the crt opens files as text unless told, and a descriptor is inherited.
#define O_CLOEXEC (O_BINARY | O_NOINHERIT)

#define mkdir(path, mode) _mkdir(path)

// what `n` bytes from `hay` hold of `needle`, the first.
static inline void *memmem(const void *hay, size_t n, const void *needle,
                           size_t m) {
  const char *h = (const char *)hay;

  if (m == 0) {
    return (void *)h;
  }

  for (size_t i = 0; m <= n && i <= n - m; i++) {
    if (h[i] == *(const char *)needle && !memcmp(h + i, needle, m)) {
      return (void *)(h + i);
    }
  }

  return nullptr;
}

// windows renames over a file only when asked.
int civue__rename(const char *from, const char *to);
#define rename civue__rename

#endif
