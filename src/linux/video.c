#include "../internal.h"

#include <civue/gfx.h>
#include <civue/video.h>
#include <civue/window.h>

#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/video/video.h>
#include <math.h>
#include <pthread.h>

// playbin's flags, which its header keeps to itself: the picture, the sound,
// the volume done in software, and a download kept on disk as it plays.
enum : u32 {
  PLAY_VIDEO = 1 << 0,
  PLAY_AUDIO = 1 << 1,
  PLAY_SOFT_VOLUME = 1 << 4,
  PLAY_DOWNLOAD = 1 << 7,
};

struct civue_video {
  GstElement *player;
  GstElement *sink;
  GstBus *bus;
  bool loop, ended, wanted, failed, ready;
  // a stream waits for data, paused meanwhile.
  bool buffering;
  bool muted;
  f32 volume;

  // the picture the streaming thread last made, taken by the next frame.
  pthread_mutex_t lock;
  GstSample *next;

  civue_gfx *gfx;
  civue_texture texture;
  u32 width, height;
};

static void start_gst() {
  gst_init_check(nullptr, nullptr, nullptr);
}

// a picture is due: kept for the frame the window is asked for. on a
// streaming thread.
static GstFlowReturn keep(civue_video *v, GstSample *sample) {
  if (!sample) {
    return GST_FLOW_OK;
  }

  pthread_mutex_lock(&v->lock);
  GstSample *old = v->next;
  v->next = sample;
  pthread_mutex_unlock(&v->lock);

  if (old) {
    gst_sample_unref(old);
  }

  civue_window_wake();
  return GST_FLOW_OK;
}

static GstFlowReturn new_sample(GstAppSink *sink, void *data) {
  return keep(data, gst_app_sink_pull_sample(sink));
}

// the picture a paused video shows, the first one or a seek's.
static GstFlowReturn new_preroll(GstAppSink *sink, void *data) {
  return keep(data, gst_app_sink_pull_preroll(sink));
}

// what the player has to say is read by the next frame. on any thread.
static GstBusSyncReply posted(GstBus *, GstMessage *, void *) {
  civue_window_wake();
  return GST_BUS_PASS;
}

static void set_state(civue_video *v) {
  gst_element_set_state(v->player, v->wanted && !v->buffering
                                     ? GST_STATE_PLAYING
                                     : GST_STATE_PAUSED);
}

civue_video *civue_video_new(const char *url, const civue_video_options *o) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  pthread_once(&once, start_gst);

  if (!url || !gst_is_initialized()) {
    return nullptr;
  }

  GstElement *player = gst_element_factory_make("playbin", nullptr);
  GstElement *sink = gst_element_factory_make("appsink", nullptr);

  if (!player || !sink) {
    LOG("video: gstreamer has no playbin or appsink");

    if (player) {
      gst_object_unref(player);
    }

    if (sink) {
      gst_object_unref(sink);
    }

    return nullptr;
  }

  civue_video *v = own_new(sizeof(civue_video));
  v->player = player;
  v->sink = sink;
  v->loop = o && o->loop;
  v->muted = o && o->muted;
  v->volume = 1;
  v->wanted = o && o->autoplay;
  pthread_mutex_init(&v->lock, nullptr);

  // pictures come as rgba at the times they are due, the late ones dropped.
  GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING,
                                      "RGBA", nullptr);
  g_object_set(sink, "caps", caps, "max-buffers", 1, "drop", true, "sync", true,
               nullptr);
  gst_caps_unref(caps);

  GstAppSinkCallbacks calls = {.new_preroll = new_preroll,
                               .new_sample = new_sample};
  gst_app_sink_set_callbacks(GST_APP_SINK(sink), &calls, v, nullptr);

  g_object_set(
    player, "uri", url, "video-sink", sink, "mute", v->muted, "flags",
    PLAY_VIDEO | PLAY_AUDIO | PLAY_SOFT_VOLUME | PLAY_DOWNLOAD, nullptr);
  v->bus = gst_element_get_bus(player);
  gst_bus_set_sync_handler(v->bus, posted, nullptr, nullptr);
  set_state(v);
  return v;
}

void civue_video_free(civue_video *v) {
  if (!v) {
    return;
  }

  // the streaming threads are stopped before what they write to goes.
  gst_element_set_state(v->player, GST_STATE_NULL);
  gst_bus_set_sync_handler(v->bus, nullptr, nullptr, nullptr);
  gst_object_unref(v->bus);
  gst_object_unref(v->player);

  if (v->next) {
    gst_sample_unref(v->next);
  }

  if (v->texture != 0 && v->gfx) {
    civue_gfx_free_texture(v->gfx, v->texture);
  }

  pthread_mutex_destroy(&v->lock);
  own_free(v);
}

static void seek_to(civue_video *v, f64 seconds) {
  gst_element_seek_simple(v->player, GST_FORMAT_TIME,
                          GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE,
                          (gint64)(MAX(seconds, 0) * GST_SECOND));
}

// what the player posted since the last frame.
static void read_bus(civue_video *v) {
  for (GstMessage *m; (m = gst_bus_pop(v->bus)); gst_message_unref(m)) {
    switch (GST_MESSAGE_TYPE(m)) {
      case GST_MESSAGE_ERROR: {
        GError *err = nullptr;
        gst_message_parse_error(m, &err, nullptr);
        LOG("video: %s", err ? err->message : "failed");
        g_clear_error(&err);
        v->failed = true;
        gst_element_set_state(v->player, GST_STATE_NULL);
        break;
      }

      case GST_MESSAGE_EOS:
        if (v->loop) {
          seek_to(v, 0);
        }
        else {
          v->ended = true;
          v->wanted = false;
          set_state(v);
        }

        break;

      case GST_MESSAGE_ASYNC_DONE:
        v->ready = true;
        break;

      case GST_MESSAGE_BUFFERING: {
        // a stream plays once enough of it came. a live one never pauses.
        gint percent = 0;
        gst_message_parse_buffering(m, &percent);

        bool buffering = percent < 100;

        if (buffering != v->buffering) {
          v->buffering = buffering;
          set_state(v);
        }

        break;
      }

      default:
        break;
    }
  }
}

// the newest picture as the texture: an image of civue's own, which the cpu
// draws when it has the time and the gpu otherwise.
static void take_picture(civue_video *v) {
  pthread_mutex_lock(&v->lock);
  GstSample *sample = v->next;
  v->next = nullptr;
  pthread_mutex_unlock(&v->lock);

  if (!sample) {
    return;
  }

  GstVideoInfo info;
  GstVideoFrame frame;
  GstCaps *caps = gst_sample_get_caps(sample);
  GstBuffer *buffer = gst_sample_get_buffer(sample);

  if (caps && buffer && gst_video_info_from_caps(&info, caps) &&
      gst_video_frame_map(&frame, &info, buffer, GST_MAP_READ)) {
    u32 w = (u32)GST_VIDEO_FRAME_WIDTH(&frame);
    u32 h = (u32)GST_VIDEO_FRAME_HEIGHT(&frame);
    const u8 *pixels = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
    usize stride = (usize)GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
    u8 *packed = nullptr;

    // rows longer than their pixels are packed first.
    if (stride != (usize)w * 4) {
      packed = xmalloc((usize)w * h * 4);

      for (u32 y = 0; y < h; y++) {
        memcpy(packed + (usize)y * w * 4, pixels + y * stride, (usize)w * 4);
      }

      pixels = packed;
    }

    if (v->texture != 0) {
      civue_gfx_free_texture(v->gfx, v->texture);
    }

    v->texture = civue_gfx_upload_image(v->gfx, w, h, pixels);
    v->width = w;
    v->height = h;
    free(packed);
    gst_video_frame_unmap(&frame);
  }

  gst_sample_unref(sample);
}

static f64 seconds_of(gint64 t) {
  return t > 0 ? (f64)t / GST_SECOND : 0;
}

civue_video_info civue_video_frame(civue_ui *ui, civue_video *v) {
  if (!v) {
    return (civue_video_info){.state = CIVUE_VIDEO_FAILED};
  }

  if (ui->gfx && v->gfx != ui->gfx) {
    v->gfx = ui->gfx;
  }

  read_bus(v);

  if (v->failed) {
    return (civue_video_info){.state = CIVUE_VIDEO_FAILED,
                              .texture = v->texture,
                              .width = v->width,
                              .height = v->height};
  }

  if (v->gfx) {
    take_picture(v);
  }

  gint64 at = 0, length = 0;
  gst_element_query_position(v->player, GST_FORMAT_TIME, &at);
  gst_element_query_duration(v->player, GST_FORMAT_TIME, &length);

  f64 duration = seconds_of(length);
  // a file on disk is all there, a download says how far it got.
  f64 buffered = duration;
  GstQuery *q = gst_query_new_buffering(GST_FORMAT_PERCENT);

  if (gst_element_query(v->player, q)) {
    gint64 stop = 0;
    gst_query_parse_buffering_range(q, nullptr, nullptr, &stop, nullptr);

    if (stop >= 0) {
      buffered = duration * (f64)stop / GST_FORMAT_PERCENT_MAX;
    }
  }

  gst_query_unref(q);

  return (civue_video_info){
    .state = v->ready ? CIVUE_VIDEO_READY : CIVUE_VIDEO_LOADING,
    .texture = v->texture,
    .width = v->width,
    .height = v->height,
    .time = seconds_of(at),
    .duration = duration,
    .buffered = buffered,
    .playing = v->wanted,
    .stalled = v->wanted && (v->buffering || !v->ready),
    .ended = v->ended,
    .muted = v->muted,
    .volume = v->volume,
  };
}

void civue_video_play(civue_video *v) {
  if (!v || v->failed) {
    return;
  }

  if (v->ended) {
    v->ended = false;
    seek_to(v, 0);
  }

  v->wanted = true;
  set_state(v);
}

void civue_video_pause(civue_video *v) {
  if (!v || v->failed) {
    return;
  }

  v->wanted = false;
  set_state(v);
}

void civue_video_seek(civue_video *v, double to) {
  if (!v || v->failed) {
    return;
  }

  v->ended = false;
  seek_to(v, to);
}

void civue_video_set_muted(civue_video *v, bool muted) {
  if (v) {
    v->muted = muted;
    g_object_set(v->player, "mute", muted, nullptr);
  }
}

void civue_video_set_volume(civue_video *v, float volume) {
  if (v) {
    v->volume = CLAMP(volume, 0.0f, 1.0f);
    g_object_set(v->player, "volume", (gdouble)v->volume, nullptr);
  }
}

void civue_video_set_loop(civue_video *v, bool loop) {
  if (v) {
    v->loop = loop;
  }
}
