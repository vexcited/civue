#ifndef CIVUE_IMAGES_H
#define CIVUE_IMAGES_H

#include <civue/ui.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================== */
/* images from urls: avatars, emoji, attachments. main thread only.           */
/*                                                                            */
/*   civue_image_info avatar = civue_images_get(images, ui, url, 80);         */
/*   if (avatar.texture != 0) civue_image(ui, avatar.texture, rect, ...);     */
/*                                                                            */
/* the first call for an url answers a texture of 0 and starts loading it:    */
/* from the disk cache when it was fetched before, else through the app's     */
/* `fetch`, civue having no network of its own. the bytes are decoded on      */
/* worker threads, uploaded on the main one, and the window is woken so the   */
/* next frame gets the texture. ask again every frame: an image nobody asked  */
/* for in a while may be evicted to stay under the budget, and comes back     */
/* the same way. the image at an url is taken to never change.                */
/*                                                                            */
/* an url is fetched once however many sizes of it are asked for, what was    */
/* asked for last is loaded first, and what waits behind too many others is   */
/* forgotten until it is asked for again.                                     */
/* ========================================================================== */

typedef enum : uint8_t {
  CIVUE_IMAGE_LOADING,
  CIVUE_IMAGE_READY,
  // the url gave no image. it is not asked again, or only after a minute
  // when the network was what failed.
  CIVUE_IMAGE_FAILED,
} civue_image_state;

typedef struct {
  civue_image_state state;
  // 0 unless ready.
  civue_texture texture;
  // pixels of the texture, which may be smaller than the image at the url.
  uint32_t width, height;
} civue_image_info;

typedef enum : uint8_t {
  CIVUE_FETCH_OK,
  // the url has no image and never will, a 404.
  CIVUE_FETCH_NEVER,
  // no answer or a server in trouble, worth another try later.
  CIVUE_FETCH_LATER,
} civue_fetch_result;

typedef struct {
  void *ctx;
  // starts fetching `url` and answers what `cancel` takes, 0 when it could
  // not start. civue_images_fetched is called with `load` once the bytes are
  // there or once it failed, from the main thread. null loads from the disk
  // cache only.
  uint64_t (*fetch)(void *ctx, uint64_t load, const char *url);
  // stops a fetch nobody waits for anymore. civue_images_fetched is not
  // called for it after. may be null.
  void (*cancel)(void *ctx, uint64_t fetch);
  // how many bytes of `url` name its image, for urls ending in a signature
  // that changes while the image does not. the whole url when null.
  size_t (*key_len)(void *ctx, const char *url);
  // the name the disk cache is filed under in the user's caches, the app's.
  // no disk cache when null.
  const char *disk_name;
  // pixels kept in memory at most, 256 MiB when 0.
  size_t memory_bytes;
  // fetched bytes kept on disk at most, 512 MiB when 0. the files read
  // longest ago go first.
  size_t disk_bytes;
  // fetches running at once, 6 when 0.
  uint32_t fetches;
  // called from any thread when an image is ready to be drawn,
  // civue_window_wake when null.
  void (*wake)(void);
} civue_images_options;

typedef struct civue_images civue_images;

civue_images *civue_images_new(const civue_images_options *o);
// frees every texture, cancels the fetches and waits for the workers.
void civue_images_free(civue_images *im);

// the image at `url`, decoded so that its longest side is at most
// `max_pixels`, 0 for 4096. an svg is drawn as a square of that side. the
// same url at another size is another image from the same bytes. a null or
// empty url is CIVUE_IMAGE_FAILED.
civue_image_info civue_images_get(civue_images *im, civue_ui *ui,
                                  const char *url, uint32_t max_pixels);

// what the app's `fetch` got for `load`. `bytes` are copied.
void civue_images_fetched(civue_images *im, uint64_t load, const void *bytes,
                          size_t len, civue_fetch_result result);

// bumped whenever an image finishes loading, fails or is evicted. a memo
// that draws images has it among its inputs, since the texture it drew may
// be gone.
uint64_t civue_images_version(civue_images *im);

typedef struct {
  // pixels held in textures.
  size_t memory_bytes;
  // since civue_images_new: urls read from the disk cache, urls handed to
  // `fetch`, and images decoded.
  uint64_t from_disk, fetched, decoded;
  // images not yet ready or failed.
  uint32_t loading;
} civue_images_stats;

civue_images_stats civue_images_stat(civue_images *im);

#ifdef __cplusplus
}
#endif

#endif
