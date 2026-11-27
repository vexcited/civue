#include "win.h"

int civue__rename(const char *from, const char *to) {
  wchar_t *a = civue__wide(from, -1), *b = civue__wide(to, -1);
  bool ok = MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
  free(a);
  free(b);
  return ok ? 0 : -1;
}
