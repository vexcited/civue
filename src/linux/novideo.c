#include "../internal.h"

#include <civue/video.h>

// built without gstreamer: no video is made, and the rest takes none.
civue_video *civue_video_new(const char *, const civue_video_options *) {
  return nullptr;
}

void civue_video_free(civue_video *) {
}

civue_video_info civue_video_frame(civue_ui *, civue_video *) {
  return (civue_video_info){.state = CIVUE_VIDEO_FAILED};
}

void civue_video_play(civue_video *) {
}

void civue_video_pause(civue_video *) {
}

void civue_video_seek(civue_video *, double) {
}

void civue_video_set_muted(civue_video *, bool) {
}

void civue_video_set_volume(civue_video *, float) {
}

void civue_video_set_loop(civue_video *, bool) {
}
