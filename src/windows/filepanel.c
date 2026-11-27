#include "win.h"

#include <civue/system.h>

#include <shobjidl.h>

// the filter of the panel, "*.png;*.jpg" for its extensions.
static wchar_t *pattern_of(const char *const *extensions, uint32_t n) {
  civue_bytes b = {};

  for (u32 i = 0; i < n; i++) {
    buf_printf(&b, "%s*.%s", i ? ";" : "", extensions[i]);
  }

  wchar_t *w = civue__wide((const char *)b.v, (int)b.len);
  vec_free(&b);
  return w;
}

// the picked file's path, utf-8 from malloc, null when it has none.
static char *path_of(IShellItem *item) {
  wchar_t *w = nullptr;

  if (FAILED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &w)) || !w) {
    return nullptr;
  }

  char *s = civue__utf8(w, -1);
  CoTaskMemFree(w);
  return s;
}

civue_strs civue_file_panel_open(const char *const *extensions, uint32_t n,
                                 bool multiple) {
  civue_strs out = {};
  civue__com_init();

  IFileOpenDialog *d = nullptr;

  if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog,
                              (void **)&d))) {
    return out;
  }

  FILEOPENDIALOGOPTIONS o = 0;
  IFileOpenDialog_GetOptions(d, &o);
  o |= FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;

  if (multiple) {
    o |= FOS_ALLOWMULTISELECT;
  }

  IFileOpenDialog_SetOptions(d, o);

  wchar_t *pattern = n ? pattern_of(extensions, n) : nullptr;
  wchar_t *label = nullptr;

  if (pattern) {
    char *s = civue__utf8(pattern, -1);
    civue_bytes b = {};
    buf_printf(&b, "Supported files (%s)", s);
    label = civue__wide((const char *)b.v, (int)b.len);
    vec_free(&b);
    free(s);

    COMDLG_FILTERSPEC spec[] = {{label, pattern}, {L"All files", L"*.*"}};
    IFileOpenDialog_SetFileTypes(d, countof(spec), spec);
    IFileOpenDialog_SetFileTypeIndex(d, 1);
  }

  // the panel's loop dispatches the window's messages meanwhile, which
  // keep drawing it.
  civue__modal(true);
  HRESULT shown = IFileOpenDialog_Show(d, civue__hwnd());
  civue__modal(false);

  IShellItemArray *items = nullptr;

  if (SUCCEEDED(shown) && SUCCEEDED(IFileOpenDialog_GetResults(d, &items))) {
    DWORD count = 0;
    IShellItemArray_GetCount(items, &count);

    for (DWORD i = 0; i < count; i++) {
      IShellItem *item = nullptr;

      if (FAILED(IShellItemArray_GetItemAt(items, i, &item))) {
        continue;
      }

      char *path = path_of(item);

      if (path) {
        vec_push(&out, path);
      }

      IShellItem_Release(item);
    }

    IShellItemArray_Release(items);
  }

  free(pattern);
  free(label);
  IFileOpenDialog_Release(d);
  return out;
}
