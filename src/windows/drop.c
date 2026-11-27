#include "win.h"

#include <shellapi.h>
#include <shlobj.h>

// what takes the files dragged over the window, ole's drop target written
// out in C, and the shell's helper drawing what is dragged over it.
typedef struct {
  IDropTarget target;
  LONG refs;
  HWND hwnd;
  IDropTargetHelper *helper;
  // what is dragged holds files.
  bool files;
} Target;

static const FORMATETC HDROP_FORMAT = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1,
                                       TYMED_HGLOBAL};

static Target *target_of(IDropTarget *t) {
  return (Target *)t;
}

static HRESULT STDMETHODCALLTYPE query(IDropTarget *t, REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IDropTarget)) {
    *out = t;
    t->lpVtbl->AddRef(t);
    return S_OK;
  }

  *out = nullptr;
  return E_NOINTERFACE;
}

// it lives as long as the window, its counts are ole's alone.
static ULONG STDMETHODCALLTYPE add_ref(IDropTarget *t) {
  return (ULONG)InterlockedIncrement(&target_of(t)->refs);
}

static ULONG STDMETHODCALLTYPE release(IDropTarget *t) {
  return (ULONG)InterlockedDecrement(&target_of(t)->refs);
}

static DWORD effect_of(const Target *t) {
  return t->files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

static HRESULT STDMETHODCALLTYPE enter(IDropTarget *t, IDataObject *data,
                                       DWORD keys, POINTL at, DWORD *effect) {
  Target *d = target_of(t);
  FORMATETC f = HDROP_FORMAT;
  d->files = IDataObject_QueryGetData(data, &f) == S_OK;
  *effect = effect_of(d);

  POINT p = {at.x, at.y};

  if (d->helper) {
    IDropTargetHelper_DragEnter(d->helper, d->hwnd, data, &p, *effect);
  }

  if (d->files) {
    civue__drag_moved(civue__window_point(p));
  }

  return S_OK;
}

static HRESULT STDMETHODCALLTYPE over(IDropTarget *t, DWORD keys, POINTL at,
                                      DWORD *effect) {
  Target *d = target_of(t);
  *effect = effect_of(d);

  POINT p = {at.x, at.y};

  if (d->helper) {
    IDropTargetHelper_DragOver(d->helper, &p, *effect);
  }

  if (d->files) {
    civue__drag_moved(civue__window_point(p));
  }

  return S_OK;
}

static HRESULT STDMETHODCALLTYPE leave(IDropTarget *t) {
  Target *d = target_of(t);

  if (d->helper) {
    IDropTargetHelper_DragLeave(d->helper);
  }

  if (d->files) {
    civue__drag_left();
  }

  d->files = false;
  return S_OK;
}

// the paths of the files a drop holds, utf-8.
static civue_strs paths_of(IDataObject *data) {
  civue_strs out = {};
  FORMATETC f = HDROP_FORMAT;
  STGMEDIUM m = {};

  if (IDataObject_GetData(data, &f, &m) != S_OK) {
    return out;
  }

  HDROP drop = (HDROP)m.hGlobal;

  if (drop) {
    UINT n = DragQueryFileW(drop, 0xffffffff, nullptr, 0);

    for (UINT i = 0; i < n; i++) {
      UINT len = DragQueryFileW(drop, i, nullptr, 0);
      wchar_t *w = xmalloc(((usize)len + 1) * sizeof *w);

      if (DragQueryFileW(drop, i, w, len + 1)) {
        vec_push(&out, civue__utf8(w, -1));
      }

      free(w);
    }
  }

  ReleaseStgMedium(&m);
  return out;
}

static HRESULT STDMETHODCALLTYPE drop(IDropTarget *t, IDataObject *data,
                                      DWORD keys, POINTL at, DWORD *effect) {
  Target *d = target_of(t);
  *effect = effect_of(d);

  POINT p = {at.x, at.y};

  if (d->helper) {
    IDropTargetHelper_Drop(d->helper, data, &p, *effect);
  }

  if (d->files) {
    civue_strs paths = paths_of(data);
    civue__dropped(civue__window_point(p), &paths);

    vec_each(&paths, s) {
      free(*s);
    }

    vec_free(&paths);
  }

  d->files = false;
  return S_OK;
}

static IDropTargetVtbl vtable = {
  .QueryInterface = query,
  .AddRef = add_ref,
  .Release = release,
  .DragEnter = enter,
  .DragOver = over,
  .DragLeave = leave,
  .Drop = drop,
};

static Target T = {.target = {.lpVtbl = &vtable}, .refs = 1};

void civue__drop_register(HWND hwnd) {
  T.hwnd = hwnd;

  if (!T.helper) {
    CoCreateInstance(&CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER,
                     &IID_IDropTargetHelper, (void **)&T.helper);
  }

  RegisterDragDrop(hwnd, &T.target);
}
