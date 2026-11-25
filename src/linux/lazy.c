#include "lazy.h"

#include <dlfcn.h>

bool civue__lazy_load(const char *const *libraries,
                      const civue__lazy_symbol *symbols, usize count) {
  void *opened[8];
  u32 n = 0;

  for (; libraries[n] && n < countof(opened); n++) {
    // for everyone, as when the app is linked with it: what the library
    // loads later finds it.
    opened[n] = dlopen(libraries[n], RTLD_NOW | RTLD_GLOBAL);

    if (!opened[n]) {
      LOG("%s", dlerror());
      return false;
    }
  }

  for (usize i = 0; i < count; i++) {
    void *found = nullptr;

    for (u32 k = 0; k < n && !found; k++) {
      found = dlsym(opened[k], symbols[i].name);
    }

    if (!found) {
      LOG("no %s in %s", symbols[i].name, libraries[0]);
      return false;
    }

    *symbols[i].slot = found;
  }

  return true;
}
