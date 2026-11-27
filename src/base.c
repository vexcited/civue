#include "internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdckdint.h>
#include <stddefer.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef _WIN32
#include <pwd.h>
#endif

#ifdef __APPLE__
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

static void (*log_hook)(const char *message);

void civue_set_log(void (*log)(const char *message)) {
  log_hook = log;
}

static void say(const char *fmt, va_list ap) {
  if (!log_hook) {
    fputs("civue: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    return;
  }
  char line[1024];
  vsnprintf(line, sizeof line, fmt, ap);
  log_hook(line);
}

void civue__die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  say(fmt, ap);
  va_end(ap);
  abort();
}

void civue__log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  say(fmt, ap);
  va_end(ap);
}

double civue_now() {
#ifdef __APPLE__
  static mach_timebase_info_data_t tb;
  if (!tb.denom) {
    mach_timebase_info(&tb);
  }
  return (f64)mach_absolute_time() * tb.numer / tb.denom / 1e9;
#else
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (f64)t.tv_sec + t.tv_nsec / 1e9;
#endif
}

/* ========================================================================== */
/* memory.                                                                    */
/* ========================================================================== */

void *civue__xmalloc(usize n) {
  void *p = malloc(n ? n : 1);
  if (!p) {
    die("out of memory allocating %zu bytes", n);
  }
  return p;
}

void *civue__xcalloc(usize n, usize size) {
  void *p = calloc(n ? n : 1, size ? size : 1);
  if (!p) {
    die("out of memory allocating %zu items", n);
  }
  return p;
}

void *civue__xrealloc(void *p, usize n) {
  p = realloc(p, n ? n : 1);
  if (!p) {
    die("out of memory reallocating %zu bytes", n);
  }
  return p;
}

char *civue__xstrndup(const char *s, usize n) {
  char *d = xmalloc(n + 1);
  memcpy(d, s, n);
  d[n] = 0;
  return d;
}

char *civue__xfmt(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list again;
  va_copy(again, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    die("out of memory formatting");
  }
  char *out = xmalloc((usize)n + 1);
  vsnprintf(out, (usize)n + 1, fmt, again);
  va_end(again);
  return out;
}

void civue__vec_grow(void **v, u32 *cap, u32 need, usize item) {
  u32 n = *cap ? *cap : 4;
  while (n < need) {
    n = n < 0x40000000 ? n * 2 : need;
  }
  usize size;
  if (ckd_mul(&size, (usize)n, item)) {
    die("%u items of %zu bytes overflow", n, item);
  }
  *v = xrealloc(*v, size);
  *cap = n;
}

void civue__buf_put(civue_bytes *b, const void *p, usize n) {
  if (!n) {
    return;
  }
  vec_reserve(b, n);
  memcpy(b->v + b->len, p, n);
  b->len += n;
}

void civue__buf_printf(civue_bytes *b, const char *fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(nullptr, 0, fmt, ap);
  vec_reserve(b, (u32)n + 1);
  vsnprintf((char *)b->v + b->len, n + 1, fmt, ap2);
  b->len += n;
  va_end(ap2);
  va_end(ap);
}

char *civue__buf_take(civue_bytes *b) {
  vec_reserve(b, 1);
  b->v[b->len] = 0;
  char *s = (char *)b->v;
  *b = (civue_bytes){};
  return s ? s : xstrdup("");
}

/* ========================================================================== */
/* text.                                                                      */
/* ========================================================================== */

uint32_t civue_utf8_next(const char *s, size_t n, size_t *i) {
  const u8 *p = (const u8 *)s;
  u8 c = p[*i];
  u32 cp;
  usize len;
  if (c < 0x80) {
    *i += 1;
    return c;
  }
  else if ((c & 0xe0) == 0xc0) {
    cp = c & 0x1f, len = 2;
  }
  else if ((c & 0xf0) == 0xe0) {
    cp = c & 0x0f, len = 3;
  }
  else if ((c & 0xf8) == 0xf0) {
    cp = c & 0x07, len = 4;
  }
  else {
    *i += 1;
    return 0xfffd;
  }
  if (*i + len > n) {
    *i += 1;
    return 0xfffd;
  }
  for (usize k = 1; k < len; k++) {
    if ((p[*i + k] & 0xc0) != 0x80) {
      *i += 1;
      return 0xfffd;
    }
    cp = cp << 6 | (p[*i + k] & 0x3f);
  }
  *i += len;
  return cp;
}

size_t civue_utf8_put(char out[4], uint32_t cp) {
  if (cp < 0x80) {
    out[0] = cp;
    return 1;
  }
  else if (cp < 0x800) {
    out[0] = 0xc0 | cp >> 6;
    out[1] = 0x80 | (cp & 0x3f);
    return 2;
  }
  else if (cp < 0x10000) {
    out[0] = 0xe0 | cp >> 12;
    out[1] = 0x80 | ((cp >> 6) & 0x3f);
    out[2] = 0x80 | (cp & 0x3f);
    return 3;
  }
  out[0] = 0xf0 | cp >> 18;
  out[1] = 0x80 | ((cp >> 12) & 0x3f);
  out[2] = 0x80 | ((cp >> 6) & 0x3f);
  out[3] = 0x80 | (cp & 0x3f);
  return 4;
}

/* ========================================================================== */
/* files.                                                                     */
/* ========================================================================== */

bool civue__file_read(const char *path, civue_bytes *out) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return false;
  }
  defer close(fd);
  struct stat st;
  if (fstat(fd, &st) < 0) {
    return false;
  }
  out->len = 0;
  vec_reserve(out, (u32)st.st_size + 1);
  for (;;) {
    vec_reserve(out, 65536);
    ssize_t r = read(fd, out->v + out->len, out->cap - out->len);
    if (r < 0 && errno == EINTR) {
      continue;
    }
    if (r < 0) {
      return false;
    }
    if (r == 0) {
      break;
    }
    out->len += r;
  }
  return true;
}

const char *civue__home_dir() {
  static const char *home;
  if (!home) {
    const char *h = getenv("HOME");
#ifdef _WIN32
    if (!h || !*h) {
      h = getenv("USERPROFILE");
    }
#else
    if (!h || !*h) {
      h = getpwuid(getuid())->pw_dir;
    }
#endif
    home = xstrdup(h);
  }
  return home;
}
