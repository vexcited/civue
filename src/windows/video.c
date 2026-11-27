#include "win.h"

#include <civue/gfx.h>
#include <civue/video.h>
#include <civue/window.h>

#include <math.h>
#include <mfapi.h>
#include <mfmediaengine.h>
#include <oleauto.h>
#include <stdatomic.h>
#include <stddefer.h>
#include <wincodec.h>

// winbase's macro of windows 3, which takes the engine's call.
#undef GetCurrentTime

// while loading or seeking, how often to look again.
constexpr f64 POLL_SECONDS = 0.05;
// a frame is asked for this long after a change while paused, for the
// picture of a seek to arrive.
constexpr f64 SETTLE_SECONDS = 0.5;

/* ========================================================================== */
/* media foundation's engine plays the video and its sound, in the mode where */
/* the app asks it for each picture: drawn into a bitmap of wic's, on the     */
/* cpu like the rest of the window. what it has to say comes on its own       */
/* threads, which keep it for the main one and wake the window.               */
/* ========================================================================== */

typedef struct {
  // first, where com finds its calls.
  const IMFMediaEngineNotifyVtbl *calls;
  atomic_uint refs;
  // waits for data while it plays.
  atomic_bool waiting;
  atomic_bool failed;
  // why, an error of MF_MEDIA_ENGINE_ERR and its hresult.
  atomic_uint error, status;
} Notify;

static HRESULT STDMETHODCALLTYPE notify_query(IMFMediaEngineNotify *self,
                                              REFIID iid, void **out) {
  if (IsEqualIID(iid, &IID_IUnknown) ||
      IsEqualIID(iid, &IID_IMFMediaEngineNotify)) {
    IMFMediaEngineNotify_AddRef(self);
    *out = self;
    return S_OK;
  }

  *out = nullptr;
  return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE notify_keep(IMFMediaEngineNotify *self) {
  return atomic_fetch_add(&((Notify *)self)->refs, 1) + 1;
}

static ULONG STDMETHODCALLTYPE notify_drop(IMFMediaEngineNotify *self) {
  u32 left = atomic_fetch_sub(&((Notify *)self)->refs, 1) - 1;

  if (left == 0) {
    free(self);
  }

  return left;
}

static HRESULT STDMETHODCALLTYPE notify_event(IMFMediaEngineNotify *self,
                                              DWORD event, DWORD_PTR param,
                                              DWORD status) {
  Notify *n = (Notify *)self;

  switch (event) {
    case MF_MEDIA_ENGINE_EVENT_WAITING:
      atomic_store(&n->waiting, true);
      break;
    case MF_MEDIA_ENGINE_EVENT_PLAYING:
    case MF_MEDIA_ENGINE_EVENT_PAUSE:
    case MF_MEDIA_ENGINE_EVENT_SEEKED:
    case MF_MEDIA_ENGINE_EVENT_ENDED:
      atomic_store(&n->waiting, false);
      break;
    case MF_MEDIA_ENGINE_EVENT_ERROR:
      atomic_store(&n->error, (u32)param);
      atomic_store(&n->status, status);
      atomic_store(&n->failed, true);
      break;
    default:
      break;
  }

  civue_window_wake();
  return S_OK;
}

static const IMFMediaEngineNotifyVtbl notify_calls = {
  notify_query,
  notify_keep,
  notify_drop,
  notify_event,
};

struct civue_video {
  IMFMediaEngine *engine;
  Notify *notify;
  IWICImagingFactory *wic;
  // what the engine draws the picture into, of its size.
  IWICBitmap *bitmap;
  u32 width, height;
  u8 *pixels;
  bool loop, ended, wanted, logged;
  // the picture's frames a second, 0 until its stream says, -1 when it
  // says none.
  f64 fps;
  // until when a frame is asked for though it does not play.
  f64 settle_until;

  civue_gfx *gfx;
  civue_texture texture;
};

static void release(void *p) {
  if (p) {
    IUnknown_Release((IUnknown *)p);
  }
}

static void settle(civue_video *v) {
  v->settle_until = civue_now() + SETTLE_SECONDS;
  civue_window_wake();
}

civue_video *civue_video_new(const char *url, const civue_video_options *o) {
  if (!url) {
    return nullptr;
  }

  // media foundation is started by the first video and left on: stopping
  // it after the last one takes a fifth of a second.
  static bool started;

  civue__com_init();

  if (!started && FAILED(MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
    return nullptr;
  }

  started = true;

  IMFMediaEngineClassFactory *factory = nullptr;
  IMFAttributes *attributes = nullptr;
  IWICImagingFactory *wic = nullptr;
  Notify *notify = calloc(1, sizeof *notify);
  IMFMediaEngine *engine = nullptr;
  defer release(factory);
  defer release(attributes);

  if (notify) {
    notify->calls = &notify_calls;
    atomic_init(&notify->refs, 1);
  }

  if (!notify ||
      FAILED(CoCreateInstance(&CLSID_WICImagingFactory, nullptr,
                              CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                              (void **)&wic)) ||
      FAILED(CoCreateInstance(
        &CLSID_MFMediaEngineClassFactory, nullptr, CLSCTX_INPROC_SERVER,
        &IID_IMFMediaEngineClassFactory, (void **)&factory)) ||
      FAILED(MFCreateAttributes(&attributes, 2)) ||
      FAILED(IMFAttributes_SetUnknown(attributes, &MF_MEDIA_ENGINE_CALLBACK,
                                      (IUnknown *)notify)) ||
      FAILED(IMFAttributes_SetUINT32(attributes,
                                     &MF_MEDIA_ENGINE_VIDEO_OUTPUT_FORMAT,
                                     DXGI_FORMAT_B8G8R8A8_UNORM)) ||
      FAILED(IMFMediaEngineClassFactory_CreateInstance(factory, 0, attributes,
                                                       &engine))) {
    LOG("video: media foundation has no engine");
    release(notify);
    release(wic);
    return nullptr;
  }

  civue_video *v = own_new(sizeof(civue_video));

  v->engine = engine;
  v->notify = notify;
  v->wic = wic;
  v->loop = o && o->loop;
  v->wanted = o && o->autoplay;

  wchar_t *wide = civue__wide(url, -1);
  BSTR source = SysAllocString(wide);

  free(wide);
  IMFMediaEngine_SetPreload(engine, MF_MEDIA_ENGINE_PRELOAD_AUTOMATIC);
  IMFMediaEngine_SetLoop(engine, v->loop);
  IMFMediaEngine_SetMuted(engine, o && o->muted);
  IMFMediaEngine_SetAutoPlay(engine, v->wanted);

  // the engine takes its own copy of the url.
  if (!source || FAILED(IMFMediaEngine_SetSource(engine, source))) {
    atomic_store(&notify->failed, true);
  }

  SysFreeString(source);
  return v;
}

void civue_video_free(civue_video *v) {
  if (!v) {
    return;
  }

  // its threads are stopped before what they call goes.
  IMFMediaEngine_Shutdown(v->engine);
  release(v->engine);
  release(v->notify);
  release(v->bitmap);
  release(v->wic);
  free(v->pixels);

  if (v->texture != 0 && v->gfx) {
    civue_gfx_free_texture(v->gfx, v->texture);
  }

  own_free(v);
}

// the picture due now as the texture, when the engine has a new one: an
// image of civue's own, as linux's are.
static void take_picture(civue_video *v) {
  LONGLONG pts;
  DWORD w = 0, h = 0, ax = 0, ay = 0;

  if (IMFMediaEngine_OnVideoStreamTick(v->engine, &pts) != S_OK ||
      FAILED(IMFMediaEngine_GetNativeVideoSize(v->engine, &w, &h)) || !w ||
      !h) {
    return;
  }

  // the ratio is the picture's: pixels that are not square are made so, as
  // wide as they are shown.
  if (SUCCEEDED(IMFMediaEngine_GetVideoAspectRatio(v->engine, &ax, &ay)) &&
      ax && ay) {
    w = (DWORD)MAX(((u64)h * ax + ay / 2) / ay, 1);
  }

  if ((u64)w * h > 64u << 20) {
    return;
  }

  if (!v->bitmap || v->width != w || v->height != h) {
    release(v->bitmap);
    v->bitmap = nullptr;
    free(v->pixels);
    v->pixels = malloc((usize)w * h * 4);

    if (!v->pixels || FAILED(IWICImagingFactory_CreateBitmap(
                        v->wic, w, h, &GUID_WICPixelFormat32bppBGRA,
                        WICBitmapCacheOnLoad, &v->bitmap))) {
      return;
    }

    v->width = w, v->height = h;
  }

  RECT to = {0, 0, (LONG)w, (LONG)h};
  MFARGB black = {0, 0, 0, 255};
  usize size = (usize)w * h * 4;

  if (FAILED(IMFMediaEngine_TransferVideoFrame(v->engine, (IUnknown *)v->bitmap,
                                               nullptr, &to, &black)) ||
      FAILED(IWICBitmap_CopyPixels(v->bitmap, nullptr, w * 4, (UINT)size,
                                   v->pixels))) {
    return;
  }

  // blue first from the engine, red first for civue, and opaque.
  for (usize i = 0; i < size; i += 4) {
    u8 b = v->pixels[i];

    v->pixels[i] = v->pixels[i + 2];
    v->pixels[i + 2] = b;
    v->pixels[i + 3] = 255;
  }

  if (v->texture != 0) {
    civue_gfx_free_texture(v->gfx, v->texture);
  }

  v->texture = civue_gfx_upload_image(v->gfx, w, h, v->pixels);
}

// the frames a second of its picture, 30 where its stream does not say.
static f64 frame_rate(civue_video *v) {
  if (v->fps != 0) {
    return v->fps > 0 ? v->fps : 30;
  }

  IMFMediaEngineEx *ex = nullptr;
  DWORD count = 0;

  if (SUCCEEDED(IMFMediaEngine_QueryInterface(v->engine, &IID_IMFMediaEngineEx,
                                              (void **)&ex)) &&
      SUCCEEDED(IMFMediaEngineEx_GetNumberOfStreams(ex, &count))) {
    v->fps = -1;

    for (DWORD i = 0; i < count && v->fps < 0; i++) {
      PROPVARIANT kind, rate;

      PropVariantInit(&kind);
      PropVariantInit(&rate);

      if (SUCCEEDED(IMFMediaEngineEx_GetStreamAttribute(
            ex, i, &MF_MT_MAJOR_TYPE, &kind)) &&
          kind.vt == VT_CLSID && IsEqualGUID(kind.puuid, &MFMediaType_Video) &&
          SUCCEEDED(IMFMediaEngineEx_GetStreamAttribute(
            ex, i, &MF_MT_FRAME_RATE, &rate)) &&
          rate.vt == VT_UI8) {
        u32 over = (u32)(rate.uhVal.QuadPart >> 32);
        u32 under = (u32)rate.uhVal.QuadPart;

        v->fps = over && under ? MIN((f64)over / under, 120.0) : -1;
      }

      PropVariantClear(&kind);
      PropVariantClear(&rate);
    }
  }

  release(ex);
  return v->fps > 0 ? v->fps : 30;
}

static f64 seconds(f64 s) {
  return isfinite(s) && s > 0 ? s : 0;
}

civue_video_info civue_video_frame(civue_ui *ui, civue_video *v) {
  if (!v) {
    return (civue_video_info){.state = CIVUE_VIDEO_FAILED};
  }

  if (ui->gfx && v->gfx != ui->gfx) {
    v->gfx = ui->gfx;
  }

  bool failed = atomic_load(&v->notify->failed);

  if (failed && !v->logged) {
    v->logged = true;
    LOG("video: media foundation cannot play it, error %u, 0x%08x",
        atomic_load(&v->notify->error), atomic_load(&v->notify->status));
  }

  if (failed) {
    return (civue_video_info){.state = CIVUE_VIDEO_FAILED,
                              .texture = v->texture,
                              .width = v->width,
                              .height = v->height};
  }

  u16 ready = IMFMediaEngine_GetReadyState(v->engine);

  if (v->gfx && ready >= MF_MEDIA_ENGINE_READY_HAVE_METADATA) {
    take_picture(v);
  }

  // it stopped at its end, unless it loops.
  if (v->wanted && !v->loop && IMFMediaEngine_IsEnded(v->engine)) {
    v->ended = true;
    v->wanted = false;
  }

  f64 buffered = 0;
  IMFMediaTimeRange *ranges = nullptr;

  if (SUCCEEDED(IMFMediaEngine_GetBuffered(v->engine, &ranges))) {
    for (DWORD i = 0, n = IMFMediaTimeRange_GetLength(ranges); i < n; i++) {
      f64 end = 0;

      IMFMediaTimeRange_GetEnd(ranges, i, &end);
      buffered = MAX(buffered, seconds(end));
    }
  }

  release(ranges);

  civue_video_state state = ready >= MF_MEDIA_ENGINE_READY_HAVE_METADATA
                              ? CIVUE_VIDEO_READY
                              : CIVUE_VIDEO_LOADING;
  bool stalled = v->wanted && (atomic_load(&v->notify->waiting) ||
                               ready < MF_MEDIA_ENGINE_READY_HAVE_FUTURE_DATA ||
                               IMFMediaEngine_IsSeeking(v->engine));

  if (v->wanted && !stalled) {
    civue_request_repaint_after(ui, 1.0 / frame_rate(v));
  }
  else if (state == CIVUE_VIDEO_LOADING || stalled || v->texture == 0 ||
           civue_now() < v->settle_until) {
    civue_request_repaint_after(ui, POLL_SECONDS);
  }

  f64 duration = seconds(IMFMediaEngine_GetDuration(v->engine));
  f64 time = seconds(IMFMediaEngine_GetCurrentTime(v->engine));

  return (civue_video_info){
    .state = state,
    .texture = v->texture,
    .width = v->width,
    .height = v->height,
    // at its end the engine says a little past it.
    .time = duration > 0 ? MIN(time, duration) : time,
    .duration = duration,
    .buffered = buffered,
    .playing = v->wanted,
    .stalled = stalled,
    .ended = v->ended,
    .muted = IMFMediaEngine_GetMuted(v->engine),
    .volume = (f32)IMFMediaEngine_GetVolume(v->engine),
  };
}

void civue_video_play(civue_video *v) {
  if (!v) {
    return;
  }

  if (v->ended) {
    v->ended = false;
    IMFMediaEngine_SetCurrentTime(v->engine, 0);
  }

  v->wanted = true;
  IMFMediaEngine_Play(v->engine);
  settle(v);
}

void civue_video_pause(civue_video *v) {
  if (!v) {
    return;
  }

  v->wanted = false;
  IMFMediaEngine_Pause(v->engine);
  settle(v);
}

void civue_video_seek(civue_video *v, double to) {
  if (!v) {
    return;
  }

  v->ended = false;
  IMFMediaEngine_SetCurrentTime(v->engine, MAX(to, 0));
  settle(v);
}

void civue_video_set_muted(civue_video *v, bool muted) {
  if (v) {
    IMFMediaEngine_SetMuted(v->engine, muted);
  }
}

void civue_video_set_volume(civue_video *v, float volume) {
  if (v) {
    IMFMediaEngine_SetVolume(v->engine, CLAMP(volume, 0.0f, 1.0f));
  }
}

void civue_video_set_loop(civue_video *v, bool loop) {
  if (v) {
    v->loop = loop;
    IMFMediaEngine_SetLoop(v->engine, loop);
  }
}
