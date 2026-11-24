#ifndef CIVUE_VIDEO_H
#define CIVUE_VIDEO_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* a video from an url, played by the system with its sound, its picture a    */
/* texture to draw. AVFoundation on macos, GStreamer on linux, where a build  */
/* without it has civue_video_new answer nullptr and the rest take it.        */
/* everything here runs on the main thread.                                   */
/*                                                                            */
/*   civue_video *v = civue_video_new(url, &(civue_video_options){.loop = 1});*/
/*   ... each frame it shows:                                                 */
/*   civue_video_info i = civue_video_frame(ui, v);                           */
/*   if (i.texture != 0) civue_image(ui, i.texture, rect, ...);               */
/* ========================================================================== */

typedef struct civue_video civue_video;

typedef struct {
  // starts playing as soon as it can.
  bool autoplay;
  // starts again from the start once it ends.
  bool loop;
  bool muted;
} civue_video_options;

typedef enum : uint8_t {
  // nothing is known of it yet.
  CIVUE_VIDEO_LOADING,
  CIVUE_VIDEO_READY,
  // the url gave nothing the system can play.
  CIVUE_VIDEO_FAILED,
} civue_video_state;

typedef struct {
  civue_video_state state;
  // the picture due now and its pixels, 0 before the first one.
  civue_texture texture;
  uint32_t width, height;
  // in seconds, `duration` 0 while unknown. `buffered` is how far from the
  // start it is loaded.
  double time, duration, buffered;
  // asked to play, and waiting for data to do so.
  bool playing, stalled;
  // it reached its end and stopped there, never true when it loops.
  bool ended;
  bool muted;
  float volume;
} civue_video_info;

// starts loading `url` at once, an http one or a file:// one.
civue_video *civue_video_new(const char *url, const civue_video_options *o);
void civue_video_free(civue_video *v);

// what it shows now, for a frame that draws it. while it plays or loads it
// asks for the frames it needs.
civue_video_info civue_video_frame(civue_ui *ui, civue_video *v);

// playing a video that ended starts it again.
void civue_video_play(civue_video *v);
void civue_video_pause(civue_video *v);
void civue_video_seek(civue_video *v, double seconds);
void civue_video_set_muted(civue_video *v, bool muted);
// from 0 to 1.
void civue_video_set_volume(civue_video *v, float volume);
void civue_video_set_loop(civue_video *v, bool loop);

#ifdef __cplusplus
}
#endif

#endif
