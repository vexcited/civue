#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include "darwin.h"

#include <civue/gfx.h>
#include <civue/video.h>
#include <civue/window.h>

// the pictures the gpu may still read from, a frame in flight or two.
constexpr u32 KEPT = 3;
// while loading or seeking, how often to look again.
constexpr f64 POLL_SECONDS = 0.05;
// a frame is asked for this long after a change while paused, for the
// picture of a seek to arrive.
constexpr f64 SETTLE_SECONDS = 0.5;

// what the system's callbacks reach the video through, emptied as it is
// freed: they may come after.
@interface CivueVideoBox : NSObject {
@public
  civue_video *video;
}
@end

@implementation CivueVideoBox
@end

struct civue_video {
  CivueVideoBox *box;
  AVPlayer *player;
  AVPlayerItem *item;
  AVPlayerItemVideoOutput *output;
  id ended_observer;
  bool loop, ended, wanted;

  civue_gfx *gfx;
  CVMetalTextureCacheRef cache;
  CVMetalTextureRef kept[KEPT];
  u32 next_kept;
  civue_texture texture;
  u32 width, height;
  f64 fps;
  // until when a frame is asked for though it does not play.
  f64 settle_until;
};

static void settle(civue_video *v) {
  v->settle_until = civue_now() + SETTLE_SECONDS;
  civue_window_wake();
}

civue_video *civue_video_new(const char *url, const civue_video_options *o) {
  NSURL *at = url ? [NSURL URLWithString:ns_str(url)] : nil;

  if (!at) {
    return nullptr;
  }

  civue_video *v = own_new(sizeof(civue_video));
  NSDictionary *pixels = @{
    (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA),
    (id)kCVPixelBufferMetalCompatibilityKey : @YES,
  };

  v->item = [AVPlayerItem playerItemWithURL:at];
  v->output =
    [[AVPlayerItemVideoOutput alloc] initWithPixelBufferAttributes:pixels];
  [v->item addOutput:v->output];
  v->player = [AVPlayer playerWithPlayerItem:v->item];
  v->player.actionAtItemEnd = AVPlayerActionAtItemEndPause;
  v->loop = o && o->loop;
  v->player.muted = o && o->muted;
  // a gif looping in a corner keeps no screen awake.
  v->player.preventsDisplaySleepDuringVideoPlayback = !(o && o->muted);

  v->box = [CivueVideoBox new];
  v->box->video = v;

  __weak CivueVideoBox *box = v->box;

  v->ended_observer = [NSNotificationCenter.defaultCenter
    addObserverForName:AVPlayerItemDidPlayToEndTimeNotification
                object:v->item
                 queue:NSOperationQueue.mainQueue
            usingBlock:^(NSNotification *) {
              CivueVideoBox *held = box;
              civue_video *self = held ? held->video : nullptr;

              if (!self) {
                return;
              }

              if (self->loop) {
                [self->player seekToTime:kCMTimeZero];
                [self->player play];
              }
              else {
                self->ended = true;
                self->wanted = false;
              }

              settle(self);
            }];

  if (o && o->autoplay) {
    v->wanted = true;
    [v->player play];
  }

  return v;
}

void civue_video_free(civue_video *v) {
  if (!v) {
    return;
  }

  v->box->video = nullptr;
  v->box = nil;
  [NSNotificationCenter.defaultCenter removeObserver:v->ended_observer];
  [v->player pause];
  [v->player replaceCurrentItemWithPlayerItem:nil];
  v->ended_observer = nil;
  v->player = nil;
  v->output = nil;
  v->item = nil;

  for (u32 i = 0; i < KEPT; i++) {
    if (v->kept[i]) {
      CFRelease(v->kept[i]);
    }
  }

  if (v->cache) {
    CFRelease(v->cache);
  }

  if (v->texture != 0 && v->gfx) {
    civue_gfx_free_texture(v->gfx, v->texture);
  }

  own_free(v);
}

// the newest picture as the texture, kept until the gpu is surely done with
// the one it replaces.
static void take_picture(civue_video *v, CVPixelBufferRef pixels) {
  if (!v->cache) {
    id<MTLDevice> device = (__bridge id<MTLDevice>)civue_gfx_device(v->gfx);

    if (CVMetalTextureCacheCreate(nullptr, nullptr, device, nullptr,
                                  &v->cache) != kCVReturnSuccess) {
      return;
    }
  }

  usize w = CVPixelBufferGetWidth(pixels), h = CVPixelBufferGetHeight(pixels);
  CVMetalTextureRef made = nullptr;

  if (CVMetalTextureCacheCreateTextureFromImage(
        nullptr, v->cache, pixels, nullptr, MTLPixelFormatBGRA8Unorm, w, h, 0,
        &made) != kCVReturnSuccess) {
    return;
  }

  id<MTLTexture> texture = CVMetalTextureGetTexture(made);

  if (!texture) {
    CFRelease(made);
    return;
  }

  if (v->texture == 0) {
    v->texture = civue_gfx_register_texture(v->gfx, (__bridge void *)texture);
  }
  else {
    civue_gfx_update_texture(v->gfx, v->texture, (__bridge void *)texture);
  }

  if (v->kept[v->next_kept]) {
    CFRelease(v->kept[v->next_kept]);
  }

  v->kept[v->next_kept] = made;
  v->next_kept = (v->next_kept + 1) % KEPT;
  v->width = (u32)w;
  v->height = (u32)h;
  CVMetalTextureCacheFlush(v->cache, 0);
}

static f64 frame_rate(civue_video *v) {
  if (v->fps > 0) {
    return v->fps;
  }

  for (AVPlayerItemTrack *track in v->item.tracks) {
    if ([track.assetTrack.mediaType isEqualToString:AVMediaTypeVideo]) {
      f64 fps = track.currentVideoFrameRate;

      fps = fps > 0 ? fps : track.assetTrack.nominalFrameRate;
      v->fps = fps > 0 ? MIN(fps, 120.0) : 0;
    }
  }

  return v->fps > 0 ? v->fps : 30;
}

static f64 seconds(CMTime t) {
  f64 s = CMTIME_IS_NUMERIC(t) ? CMTimeGetSeconds(t) : 0;

  return isfinite(s) && s > 0 ? s : 0;
}

civue_video_info civue_video_frame(civue_ui *ui, civue_video *v) {
  if (!v) {
    return (civue_video_info){.state = CIVUE_VIDEO_FAILED};
  }

  if (ui->gfx && v->gfx != ui->gfx) {
    v->gfx = ui->gfx;
  }

  AVPlayerItemStatus status = v->item.status;
  CMTime now = [v->output itemTimeForHostTime:CACurrentMediaTime()];

  if (v->gfx && [v->output hasNewPixelBufferForItemTime:now]) {
    CVPixelBufferRef pixels = [v->output copyPixelBufferForItemTime:now
                                                 itemTimeForDisplay:nullptr];

    if (pixels) {
      take_picture(v, pixels);
      CVPixelBufferRelease(pixels);
    }
  }

  f64 buffered = 0;

  for (NSValue *range in v->item.loadedTimeRanges) {
    CMTimeRange r = range.CMTimeRangeValue;

    buffered = MAX(buffered, seconds(CMTimeRangeGetEnd(r)));
  }

  bool stalled = v->player.timeControlStatus ==
                 AVPlayerTimeControlStatusWaitingToPlayAtSpecifiedRate;
  bool failed = status == AVPlayerItemStatusFailed;
  civue_video_state state = failed ? CIVUE_VIDEO_FAILED
                            : status == AVPlayerItemStatusReadyToPlay
                              ? CIVUE_VIDEO_READY
                              : CIVUE_VIDEO_LOADING;

  if (!failed) {
    bool moving = v->wanted && !stalled && state == CIVUE_VIDEO_READY;

    if (moving) {
      civue_request_repaint_after(ui, 1.0 / frame_rate(v));
    }
    else if (state == CIVUE_VIDEO_LOADING || stalled || v->texture == 0 ||
             civue_now() < v->settle_until) {
      civue_request_repaint_after(ui, POLL_SECONDS);
    }
  }

  return (civue_video_info){
    .state = state,
    .texture = v->texture,
    .width = v->width,
    .height = v->height,
    .time = seconds(v->player.currentTime),
    .duration = seconds(v->item.duration),
    .buffered = buffered,
    .playing = v->wanted,
    .stalled = stalled,
    .ended = v->ended,
    .muted = v->player.muted,
    .volume = v->player.volume,
  };
}

void civue_video_play(civue_video *v) {
  if (!v) {
    return;
  }

  if (v->ended) {
    v->ended = false;
    [v->player seekToTime:kCMTimeZero];
  }

  v->wanted = true;
  [v->player play];
  settle(v);
}

void civue_video_pause(civue_video *v) {
  if (!v) {
    return;
  }

  v->wanted = false;
  [v->player pause];
  settle(v);
}

void civue_video_seek(civue_video *v, double to) {
  if (!v) {
    return;
  }

  __weak CivueVideoBox *box = v->box;

  v->ended = false;
  [v->player seekToTime:CMTimeMakeWithSeconds(MAX(to, 0), 600)
        toleranceBefore:kCMTimeZero
         toleranceAfter:kCMTimeZero
      completionHandler:^(BOOL) {
        dispatch_async(dispatch_get_main_queue(), ^{
          CivueVideoBox *held = box;

          if (held && held->video) {
            settle(held->video);
          }
        });
      }];
  settle(v);
}

void civue_video_set_muted(civue_video *v, bool muted) {
  if (v) {
    v->player.muted = muted;
    v->player.preventsDisplaySleepDuringVideoPlayback = !muted;
  }
}

void civue_video_set_volume(civue_video *v, float volume) {
  if (v) {
    v->player.volume = CLAMP(volume, 0.0f, 1.0f);
  }
}

void civue_video_set_loop(civue_video *v, bool loop) {
  if (v) {
    v->loop = loop;
  }
}
