#include "internal.h"

#include <civue/gfx.h>
#include <civue/icons.h>
#include <civue/images.h>
#include <civue/window.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

constexpr usize MEMORY_BYTES = 256u << 20;
constexpr usize DISK_BYTES = 512u << 20;
constexpr usize FILE_BYTES_MAX = 64u << 20;
constexpr u32 PIXELS_MAX = 4096;
// the side an svg is drawn at when none is asked for.
constexpr u32 SVG_SIDE = 128;
// an image asked for this recently may still be in a frame being shown.
constexpr f64 KEEP_SECONDS = 2;
constexpr f64 RETRY_SECONDS = 60;
constexpr u32 FETCHES = 6;
constexpr u32 WORKERS = 3;
constexpr u32 READS = 2;
// urls waiting to be read or fetched past this many are forgotten, the ones
// asked for longest ago first: a list scrolled through fast.
constexpr u32 PENDING_MAX = 128;

/* ========================================================================== */
/* the bytes of an url, shared between the main thread and the workers.       */
/* ========================================================================== */

typedef struct {
  atomic_uint refs;
  usize len;
  u8 data[];
} Blob;

static Blob *blob_new(const void *bytes, usize len) {
  Blob *b = xmalloc(sizeof(Blob) + len);

  atomic_init(&b->refs, 1);
  b->len = len;

  if (bytes) {
    memcpy(b->data, bytes, len);
  }

  return b;
}

static Blob *blob_keep(Blob *b) {
  atomic_fetch_add(&b->refs, 1);
  return b;
}

static void blob_drop(Blob *b) {
  if (b && atomic_fetch_sub(&b->refs, 1) == 1) {
    free(b);
  }
}

/* ========================================================================== */
/* what the workers do: files and decoding.                                   */
/* ========================================================================== */

typedef enum : u8 {
  JOB_READ,
  JOB_WRITE,
  JOB_REMOVE,
  JOB_DECODE,
  JOB_SWEEP,
} JobKind;

typedef struct Job Job;

struct Job {
  Job *next;
  JobKind kind;
  // the source or the entry it is for, and which of its loads.
  u64 key, serial;
  char *path;
  Blob *blob;
  u32 max_pixels;
  u64 limit;

  // what a read or a decode found, null when nothing.
  Blob *read;
  u8 *rgba;
  u32 width, height;
};

static void job_free(Job *job) {
  blob_drop(job->blob);
  blob_drop(job->read);
  free(job->rgba);
  free(job->path);
  free(job);
}

static Blob *read_file(const char *path) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);

  if (fd < 0) {
    return nullptr;
  }

  struct stat st;
  Blob *b = nullptr;

  if (fstat(fd, &st) == 0 && st.st_size > 0 &&
      (u64)st.st_size <= FILE_BYTES_MAX) {
    usize got = 0;

    b = blob_new(nullptr, (usize)st.st_size);

    while (got < b->len) {
      ssize_t n = read(fd, b->data + got, b->len - got);

      if (n < 0 && errno == EINTR) {
        continue;
      }

      if (n <= 0) {
        break;
      }

      got += (usize)n;
    }

    if (got == b->len) {
      // the sweep removes the files read longest ago.
      futimens(fd, nullptr);
    }
    else {
      blob_drop(b);
      b = nullptr;
    }
  }

  close(fd);
  return b;
}

// written beside then renamed, so that a file is whole or is not there. the
// file beside is this write's alone, another app's with the same cache has
// its own.
static void write_file(const char *path, const Blob *b, u64 serial) {
  char *part = xfmt("%s.%lx.%llx.part", path, (unsigned long)getpid(),
                    (unsigned long long)serial);
  int fd = open(part, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

  if (fd < 0) {
    free(part);
    return;
  }

  bool ok = true;
  usize put = 0;

  while (ok && put < b->len) {
    ssize_t n = write(fd, b->data + put, b->len - put);

    if (n < 0 && errno == EINTR) {
      continue;
    }

    ok = n > 0;
    put += ok ? (usize)n : 0;
  }

  ok = close(fd) == 0 && ok;

  if (!ok || rename(part, path) != 0) {
    unlink(part);
  }

  free(part);
}

typedef struct {
  char *path;
  time_t used;
  u64 size;
} Kept;

static int by_age(const void *a, const void *b) {
  const Kept *x = a, *y = b;
  return (x->used > y->used) - (x->used < y->used);
}

static void sweep(const char *dir, u64 limit) {
  DIR *d = opendir(dir);

  if (!d) {
    return;
  }

  Vec(Kept) files = {};
  u64 total = 0;
  time_t now = time(nullptr);
  struct dirent *it;

  while ((it = readdir(d))) {
    if (it->d_name[0] == '.') {
      continue;
    }

    char *path = xfmt("%s/%s", dir, it->d_name);
    struct stat st;

    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
      free(path);
      continue;
    }

    // a write the app quit in the middle of.
    if (strstr(it->d_name, ".part") && now - st.st_mtime > 3600) {
      unlink(path);
      free(path);
      continue;
    }

    total += (u64)st.st_size;
    vec_push(&files, ((Kept){path, st.st_mtime, (u64)st.st_size}));
  }

  closedir(d);

  if (total > limit) {
    qsort(files.v, files.len, sizeof(Kept), by_age);

    // down to four fifths, so that it is not swept at every write.
    vec_each(&files, f) {
      if (total <= limit - limit / 5) {
        break;
      }

      if (unlink(f->path) == 0) {
        total -= f->size;
      }
    }
  }

  vec_each(&files, f) {
    free(f->path);
  }

  vec_free(&files);
}

static bool is_svg(const u8 *bytes, usize len) {
  usize head = MIN(len, 256);

  for (usize i = 0; i + 4 <= head; i++) {
    if (!memcmp(bytes + i, "<svg", 4)) {
      return true;
    }
  }

  return false;
}

static void decode(Job *job) {
  const Blob *b = job->blob;

  if (!is_svg(b->data, b->len)) {
    job->rgba = civue__image_decode(b->data, b->len, job->max_pixels,
                                    &job->width, &job->height);
    return;
  }

  // PIXELS_MAX is what no size asked for becomes.
  u32 side = job->max_pixels < PIXELS_MAX ? job->max_pixels : SVG_SIDE;
  civue_bytes rgba = {};

  if (civue_svg_rasterize((const char *)b->data, b->len, side, side, &rgba)) {
    job->rgba = rgba.v;
    job->width = job->height = side;
  }
  else {
    free(rgba.v);
  }
}

static void run(Job *job) {
  switch (job->kind) {
    case JOB_READ:
      job->read = read_file(job->path);
      break;
    case JOB_WRITE:
      write_file(job->path, job->blob, job->serial);
      break;
    case JOB_REMOVE:
      unlink(job->path);
      break;
    case JOB_DECODE:
      decode(job);
      break;
    case JOB_SWEEP:
      sweep(job->path, job->limit);
      break;
  }
}

/* ========================================================================== */
/* the cache. a source is the bytes of an url, an entry one size of it.       */
/* ========================================================================== */

typedef enum : u8 {
  // no bytes held: never loaded, or every size of it is decoded.
  SOURCE_IDLE,
  // waits for a worker to look on disk.
  SOURCE_QUEUED,
  SOURCE_READING,
  // not on disk, waits for a fetch.
  SOURCE_WANTED,
  SOURCE_FETCHING,
  SOURCE_LOADED,
  SOURCE_FAILED,
} SourceState;

typedef enum : u8 {
  // for its source's bytes, then for a worker.
  STAGE_WAITING,
  STAGE_DECODING,
  STAGE_DONE,
} Stage;

typedef struct Source Source;
typedef struct Entry Entry;

struct Source {
  u64 key;
  char *url;
  // its file in the disk cache, null without one.
  char *path;
  // a file:// url, read from `path` and never fetched, written or removed.
  bool local;
  SourceState state;
  // the bytes came from disk, where a file may be damaged.
  bool from_disk;
  Blob *bytes;
  // which read or fetch is running, and what cancels the fetch.
  u64 load, fetch;
  // when a failed fetch is worth another try, 0 for never.
  f64 retry_at;
  // the count of the last civue_images_get of any of its sizes.
  u64 asked;
  Entry *entries;
  // how many of them are not done.
  u32 unfinished;
};

struct Entry {
  u64 key, serial;
  Source *source;
  Entry *sibling;
  u32 max_pixels;
  Stage stage;
  civue_image_state state;
  // failed because its source did, and comes back when it does.
  bool with_source;
  civue_texture texture;
  u32 width, height;
  usize bytes;
  f64 used_at;
  u64 asked;
  // most recently asked first.
  Entry *newer, *older;
};

struct civue_images {
  civue_images_options o;
  civue_gfx *gfx;
  char *dir;

  civue_map sources, entries;
  Entry *newest, *oldest;
  // sources queued or wanted, entries waiting, sources fetching.
  Vec(Source *) pending;
  Vec(Entry *) waiting;
  Vec(Source *) fetching;
  u32 reading, decoding;
  bool pumping;

  usize bytes;
  u64 version, serial, asks;
  // bytes put on disk since it was last swept.
  u64 written;
  civue_images_stats stats;

  pthread_t workers[WORKERS];
  u32 nworkers;
  pthread_mutex_t lock;
  pthread_cond_t has_jobs;
  Job *jobs, *last_job, *done;
  bool stop;
};

static void *work(void *arg) {
  civue_images *im = arg;

  pthread_mutex_lock(&im->lock);

  for (;;) {
    while (!im->stop && !im->jobs) {
      pthread_cond_wait(&im->has_jobs, &im->lock);
    }

    if (im->stop) {
      break;
    }

    Job *job = im->jobs;

    im->jobs = job->next;

    if (!im->jobs) {
      im->last_job = nullptr;
    }

    pthread_mutex_unlock(&im->lock);
    run(job);

    bool answers = job->kind == JOB_READ || job->kind == JOB_DECODE;

    if (!answers) {
      job_free(job);
    }

    pthread_mutex_lock(&im->lock);

    if (answers) {
      job->next = im->done;
      im->done = job;
      im->o.wake();
    }
  }

  pthread_mutex_unlock(&im->lock);
  return nullptr;
}

static void submit(civue_images *im, Job job) {
  Job *j = xmalloc(sizeof(Job));

  *j = job;
  j->next = nullptr;

  pthread_mutex_lock(&im->lock);

  if (im->last_job) {
    im->last_job->next = j;
  }
  else {
    im->jobs = j;
  }

  im->last_job = j;
  pthread_cond_signal(&im->has_jobs);
  pthread_mutex_unlock(&im->lock);
}

static void forget_source(civue_images *im, const Source *s) {
  for (u32 i = 0; i < im->pending.len; i++) {
    if (im->pending.v[i] == s) {
      im->pending.v[i] = vec_pop(&im->pending);
      return;
    }
  }
}

static void forget_waiting(civue_images *im, const Entry *e) {
  for (u32 i = 0; i < im->waiting.len; i++) {
    if (im->waiting.v[i] == e) {
      im->waiting.v[i] = vec_pop(&im->waiting);
      return;
    }
  }
}

static bool forget_fetch(civue_images *im, const Source *s) {
  for (u32 i = 0; i < im->fetching.len; i++) {
    if (im->fetching.v[i] == s) {
      im->fetching.v[i] = vec_pop(&im->fetching);
      return true;
    }
  }

  return false;
}

static void unlink_entry(civue_images *im, Entry *e) {
  if (e->newer) {
    e->newer->older = e->older;
  }
  else if (im->newest == e) {
    im->newest = e->older;
  }

  if (e->older) {
    e->older->newer = e->newer;
  }
  else if (im->oldest == e) {
    im->oldest = e->newer;
  }

  e->newer = e->older = nullptr;
}

static void link_newest(civue_images *im, Entry *e) {
  e->older = im->newest;

  if (im->newest) {
    im->newest->newer = e;
  }

  im->newest = e;

  if (!im->oldest) {
    im->oldest = e;
  }
}

static void queue_source(civue_images *im, Source *s) {
  s->state = s->path ? SOURCE_QUEUED : SOURCE_WANTED;
  vec_push(&im->pending, s);
}

// the bytes are only held while a size of them is still to decode: the disk
// has them for the next one.
static void settle(Source *s) {
  if (s->unfinished == 0 && s->state == SOURCE_LOADED) {
    blob_drop(s->bytes);
    s->bytes = nullptr;
    s->state = SOURCE_IDLE;
  }
}

static void finish(civue_images *im, Entry *e, civue_image_state state) {
  e->stage = STAGE_DONE;
  e->state = state;
  e->source->unfinished--;
  im->stats.loading--;
  im->version++;
}

static void fail_source(civue_images *im, Source *s, bool retry) {
  s->state = SOURCE_FAILED;
  s->retry_at = retry ? now_s() + RETRY_SECONDS : 0;
  blob_drop(s->bytes);
  s->bytes = nullptr;

  for (Entry *e = s->entries; e; e = e->sibling) {
    if (e->stage == STAGE_WAITING) {
      forget_waiting(im, e);
      e->with_source = true;
      finish(im, e, CIVUE_IMAGE_FAILED);
    }
  }
}

static void wait_again(civue_images *im, Entry *e) {
  e->stage = STAGE_WAITING;
  e->state = CIVUE_IMAGE_LOADING;
  e->with_source = false;
  vec_push(&im->waiting, e);
}

static void retry_source(civue_images *im, Source *s) {
  for (Entry *e = s->entries; e; e = e->sibling) {
    if (e->state == CIVUE_IMAGE_FAILED && e->with_source) {
      wait_again(im, e);
      s->unfinished++;
      im->stats.loading++;
    }
  }

  queue_source(im, s);
}

static void drop_source(civue_images *im, Source *s) {
  forget_source(im, s);

  if (forget_fetch(im, s) && s->fetch != 0 && im->o.cancel) {
    im->o.cancel(im->o.ctx, s->fetch);
  }

  map_del(&im->sources, s->key);
  blob_drop(s->bytes);
  free(s->path);
  free(s->url);
  own_free(s);
}

// a source lives as long as one size of it does.
static void drop_entry(civue_images *im, Entry *e) {
  Source *s = e->source;

  if (e->stage == STAGE_WAITING) {
    forget_waiting(im, e);
  }

  if (e->stage != STAGE_DONE) {
    s->unfinished--;
    im->stats.loading--;
  }

  if (e->texture != 0 && im->gfx) {
    civue_gfx_free_texture(im->gfx, e->texture);
  }

  im->bytes -= e->bytes;
  unlink_entry(im, e);
  map_del(&im->entries, e->key);

  for (Entry **at = &s->entries; *at; at = &(*at)->sibling) {
    if (*at == e) {
      *at = e->sibling;
      break;
    }
  }

  if (s->entries) {
    settle(s);
  }
  else {
    drop_source(im, s);
  }

  own_free(e);
}

// frees the images asked for longest ago until the budget holds.
static void evict(civue_images *im) {
  if (im->bytes <= im->o.memory_bytes) {
    return;
  }

  f64 now = now_s();
  Entry *e = im->oldest;

  while (e && im->bytes > im->o.memory_bytes) {
    Entry *next = e->newer;

    if (now - e->used_at < KEEP_SECONDS) {
      break;
    }

    if (e->state == CIVUE_IMAGE_READY) {
      drop_entry(im, e);
      im->version++;
    }

    e = next;
  }
}

static void start_fetch(civue_images *im, Source *s) {
  u64 load = s->load = ++im->serial;

  s->state = SOURCE_FETCHING;
  s->fetch = 0;
  vec_push(&im->fetching, s);
  im->stats.fetched++;

  u64 fetch = im->o.fetch ? im->o.fetch(im->o.ctx, load, s->url) : 0;

  // the fetch may have answered before returning.
  if (s->state != SOURCE_FETCHING || s->load != load) {
    return;
  }

  if (fetch != 0) {
    s->fetch = fetch;
  }
  else {
    forget_fetch(im, s);
    fail_source(im, s, im->o.fetch != nullptr);
  }
}

static Source *newest_pending(civue_images *im, SourceState state) {
  Source *best = nullptr;

  vec_each(&im->pending, it) {
    if ((*it)->state == state && (!best || (*it)->asked > best->asked)) {
      best = *it;
    }
  }

  return best;
}

static void trim(civue_images *im) {
  while (im->pending.len > PENDING_MAX) {
    Source *last = im->pending.v[0];

    vec_each(&im->pending, it) {
      if ((*it)->asked < last->asked) {
        last = *it;
      }
    }

    bool kept = false;

    // the sizes of it already drawn stay. its last entry takes the source
    // with it.
    for (Entry *e = last->entries, *next; e; e = next) {
      next = e->sibling;

      if (e->stage == STAGE_DONE) {
        kept = true;
      }
      else {
        drop_entry(im, e);
      }
    }

    if (kept) {
      forget_source(im, last);
      last->state = SOURCE_IDLE;
    }
  }
}

// hands the workers and the app what was asked for last, a few at a time, so
// that what is asked for next goes before what was queued earlier.
static void pump(civue_images *im) {
  if (im->pumping) {
    return;
  }

  im->pumping = true;
  trim(im);

  while (im->reading < READS) {
    Source *s = newest_pending(im, SOURCE_QUEUED);

    if (!s) {
      break;
    }

    forget_source(im, s);
    s->state = SOURCE_READING;
    s->load = ++im->serial;
    im->reading++;
    submit(im, (Job){.kind = JOB_READ,
                     .key = s->key,
                     .serial = s->load,
                     .path = xstrdup(s->path)});
  }

  while (im->fetching.len < im->o.fetches) {
    Source *s = newest_pending(im, SOURCE_WANTED);

    if (!s) {
      break;
    }

    forget_source(im, s);
    start_fetch(im, s);
  }

  while (im->decoding < im->nworkers) {
    Entry *best = nullptr;

    vec_each(&im->waiting, it) {
      if ((*it)->source->state == SOURCE_LOADED &&
          (!best || (*it)->asked > best->asked)) {
        best = *it;
      }
    }

    if (!best) {
      break;
    }

    forget_waiting(im, best);
    best->stage = STAGE_DECODING;
    im->decoding++;
    submit(im, (Job){.kind = JOB_DECODE,
                     .key = best->key,
                     .serial = best->serial,
                     .blob = blob_keep(best->source->bytes),
                     .max_pixels = best->max_pixels});
  }

  im->pumping = false;
}

static void on_read(civue_images *im, Job *job) {
  u64 at = 0;

  im->reading--;

  if (!map_get(&im->sources, job->key, &at)) {
    return;
  }

  Source *s = (Source *)(uintptr_t)at;

  if (s->state != SOURCE_READING || s->load != job->serial) {
    return;
  }

  if (job->read) {
    s->bytes = job->read;
    job->read = nullptr;
    s->from_disk = !s->local;
    s->state = SOURCE_LOADED;
    im->stats.from_disk++;
  }
  else if (s->local) {
    fail_source(im, s, false);
  }
  else {
    s->state = SOURCE_WANTED;
    vec_push(&im->pending, s);
  }
}

static void on_decoded(civue_images *im, Job *job) {
  u64 at = 0;

  im->decoding--;

  if (!map_get(&im->entries, job->key, &at)) {
    return;
  }

  Entry *e = (Entry *)(uintptr_t)at;
  Source *s = e->source;

  if (e->stage != STAGE_DECODING || e->serial != job->serial) {
    return;
  }

  civue_texture texture = 0;

  if (job->rgba && im->gfx) {
    texture =
      civue_gfx_upload_image(im->gfx, job->width, job->height, job->rgba);
  }

  if (texture != 0) {
    e->texture = texture;
    e->width = job->width;
    e->height = job->height;
    e->bytes = (usize)job->width * job->height * 4;
    im->bytes += e->bytes;
    im->stats.decoded++;
    finish(im, e, CIVUE_IMAGE_READY);
    settle(s);
    evict(im);
    return;
  }

  if (s->state == SOURCE_FAILED) {
    e->with_source = true;
    finish(im, e, CIVUE_IMAGE_FAILED);
    return;
  }

  // the source has other bytes by now, these were bad ones from disk.
  if (job->blob != s->bytes) {
    wait_again(im, e);
    return;
  }

  if (s->path && !s->local && !job->rgba) {
    submit(im, (Job){.kind = JOB_REMOVE, .path = xstrdup(s->path)});
  }

  // a damaged file is fetched again.
  if (s->from_disk && !job->rgba) {
    blob_drop(s->bytes);
    s->bytes = nullptr;
    s->from_disk = false;
    s->state = SOURCE_WANTED;
    vec_push(&im->pending, s);
    wait_again(im, e);
    return;
  }

  finish(im, e, CIVUE_IMAGE_FAILED);
  settle(s);
}

static void drain(civue_images *im) {
  pthread_mutex_lock(&im->lock);
  Job *job = im->done;
  im->done = nullptr;
  pthread_mutex_unlock(&im->lock);

  bool worked = job != nullptr;

  // an image kept for the frame that showed it is evicted by a later one.
  evict(im);

  while (job) {
    Job *next = job->next;

    if (job->kind == JOB_READ) {
      on_read(im, job);
    }
    else {
      on_decoded(im, job);
    }

    job_free(job);
    job = next;
  }

  if (worked) {
    pump(im);
  }
}

/* ========================================================================== */
/* the disk cache: one file an url, named by a hash of it.                    */
/* ========================================================================== */

static u64 fnv(const char *s, usize len, u64 h) {
  for (usize i = 0; i < len; i++) {
    h = (h ^ (u8)s[i]) * 0x100000001b3ull;
  }

  return h;
}

// two hashes, the names staying the same from a run to the next.
static char *path_of(civue_images *im, const char *url) {
  if (!im->dir) {
    return nullptr;
  }

  usize len = strlen(url);

  if (im->o.key_len) {
    usize named = im->o.key_len(im->o.ctx, url);
    len = named != 0 && named < len ? named : len;
  }

  return xfmt("%s/%016llx%016llx", im->dir,
              (unsigned long long)fnv(url, len, 0xcbf29ce484222325ull),
              (unsigned long long)fnv(url, len, 0x84222325cbf29ce4ull));
}

static char *caches_dir(const char *name) {
  const char *home = getenv("HOME");

#ifdef __APPLE__
  return home ? xfmt("%s/Library/Caches/%s/images", home, name) : nullptr;
#else
  const char *xdg = getenv("XDG_CACHE_HOME");

  if (xdg && xdg[0] == '/') {
    return xfmt("%s/%s/images", xdg, name);
  }

  return home ? xfmt("%s/.cache/%s/images", home, name) : nullptr;
#endif
}

static bool make_dirs(char *path) {
  for (char *p = path + 1; *p; p++) {
    if (*p == '/') {
      *p = '\0';
      mkdir(path, 0700);
      *p = '/';
    }
  }

  struct stat st;

  mkdir(path, 0700);
  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void sweep_disk(civue_images *im) {
  im->written = 0;
  submit(im, (Job){.kind = JOB_SWEEP,
                   .path = xstrdup(im->dir),
                   .limit = im->o.disk_bytes});
}

/* ========================================================================== */
/* the public side.                                                           */
/* ========================================================================== */

civue_images *civue_images_new(const civue_images_options *o) {
  civue_images *im = own_new(sizeof(civue_images));

  if (o) {
    im->o = *o;
  }

  im->o.memory_bytes = im->o.memory_bytes ? im->o.memory_bytes : MEMORY_BYTES;
  im->o.disk_bytes = im->o.disk_bytes ? im->o.disk_bytes : DISK_BYTES;
  im->o.fetches = im->o.fetches ? im->o.fetches : FETCHES;
  im->o.wake = im->o.wake ? im->o.wake : civue_window_wake;

  if (im->o.disk_name) {
    im->dir = caches_dir(im->o.disk_name);

    if (im->dir && !make_dirs(im->dir)) {
      free(im->dir);
      im->dir = nullptr;
    }
  }

  im->o.disk_name = nullptr;
  pthread_mutex_init(&im->lock, nullptr);
  pthread_cond_init(&im->has_jobs, nullptr);

  while (im->nworkers < WORKERS &&
         pthread_create(&im->workers[im->nworkers], nullptr, work, im) == 0) {
    im->nworkers++;
  }

  if (im->nworkers == 0) {
    die("civue: no thread for images");
  }

  if (im->dir) {
    sweep_disk(im);
  }

  return im;
}

void civue_images_free(civue_images *im) {
  if (!im) {
    return;
  }

  pthread_mutex_lock(&im->lock);
  im->stop = true;
  pthread_cond_broadcast(&im->has_jobs);
  pthread_mutex_unlock(&im->lock);

  for (u32 i = 0; i < im->nworkers; i++) {
    pthread_join(im->workers[i], nullptr);
  }

  for (Job *job = im->jobs, *next; job; job = next) {
    next = job->next;
    job_free(job);
  }

  for (Job *job = im->done, *next; job; job = next) {
    next = job->next;
    job_free(job);
  }

  while (im->newest) {
    drop_entry(im, im->newest);
  }

  pthread_cond_destroy(&im->has_jobs);
  pthread_mutex_destroy(&im->lock);
  map_free(&im->sources);
  map_free(&im->entries);
  vec_free(&im->pending);
  vec_free(&im->waiting);
  vec_free(&im->fetching);
  free(im->dir);
  own_free(im);
}

static Source *source_of(civue_images *im, const char *url) {
  u64 key = cilo_hash_str(url), at = 0;

  if (map_get(&im->sources, key, &at)) {
    return (Source *)(uintptr_t)at;
  }

  Source *s = own_new(sizeof(Source));

  s->key = key;
  s->url = xstrdup(url);
  s->local = !strncmp(url, "file://", 7);
  s->path = s->local ? xstrdup(url + 7) : path_of(im, url);
  map_put(&im->sources, key, (u64)(uintptr_t)s);
  return s;
}

static Entry *entry_new(civue_images *im, u64 key, const char *url,
                        u32 max_pixels) {
  Entry *e = own_new(sizeof(Entry));
  Source *s = source_of(im, url);

  e->key = key;
  e->serial = ++im->serial;
  e->source = s;
  e->max_pixels = max_pixels;
  e->sibling = s->entries;
  s->entries = e;
  map_put(&im->entries, key, (u64)(uintptr_t)e);
  link_newest(im, e);

  if (s->state == SOURCE_FAILED) {
    e->stage = STAGE_DONE;
    e->state = CIVUE_IMAGE_FAILED;
    e->with_source = true;
    return e;
  }

  wait_again(im, e);
  s->unfinished++;
  im->stats.loading++;

  if (s->state == SOURCE_IDLE) {
    queue_source(im, s);
  }

  return e;
}

civue_image_info civue_images_get(civue_images *im, civue_ui *ui,
                                  const char *url, uint32_t max_pixels) {
  if (!im || !url || !url[0]) {
    return (civue_image_info){.state = CIVUE_IMAGE_FAILED};
  }

  if (ui && ui->gfx) {
    im->gfx = ui->gfx;
  }

  drain(im);

  if (max_pixels == 0 || max_pixels > PIXELS_MAX) {
    max_pixels = PIXELS_MAX;
  }

  u64 key = cilo_hash_str(url) ^ cilo_hash_u64(max_pixels), at = 0;
  bool fresh = !map_get(&im->entries, key, &at);
  Entry *e =
    fresh ? entry_new(im, key, url, max_pixels) : (Entry *)(uintptr_t)at;
  Source *s = e->source;
  f64 now = now_s();

  if (im->newest != e) {
    unlink_entry(im, e);
    link_newest(im, e);
  }

  e->used_at = now;
  e->asked = s->asked = ++im->asks;

  if (e->state == CIVUE_IMAGE_FAILED && e->with_source &&
      s->state == SOURCE_FAILED && s->retry_at != 0 && now >= s->retry_at) {
    retry_source(im, s);
    fresh = true;
  }

  if (fresh) {
    pump(im);
  }

  return (civue_image_info){e->state, e->texture, e->width, e->height};
}

void civue_images_fetched(civue_images *im, uint64_t load, const void *bytes,
                          size_t len, civue_fetch_result result) {
  Source *s = nullptr;

  vec_each(&im->fetching, it) {
    if ((*it)->load == load) {
      s = *it;
      break;
    }
  }

  if (!s) {
    return;
  }

  forget_fetch(im, s);
  s->fetch = 0;

  if (result != CIVUE_FETCH_OK || !bytes || len == 0) {
    fail_source(im, s, result == CIVUE_FETCH_LATER);
    im->o.wake();
  }
  else {
    s->bytes = blob_new(bytes, len);
    s->from_disk = false;
    s->state = SOURCE_LOADED;

    if (s->path && len <= FILE_BYTES_MAX) {
      submit(im, (Job){.kind = JOB_WRITE,
                       .serial = load,
                       .path = xstrdup(s->path),
                       .blob = blob_keep(s->bytes)});
      im->written += len;

      if (im->written > im->o.disk_bytes / 8) {
        sweep_disk(im);
      }
    }

    settle(s);
  }

  pump(im);
}

uint64_t civue_images_version(civue_images *im) {
  if (!im) {
    return 0;
  }

  drain(im);
  return im->version;
}

civue_images_stats civue_images_stat(civue_images *im) {
  if (!im) {
    return (civue_images_stats){};
  }

  drain(im);
  im->stats.memory_bytes = im->bytes;
  return im->stats;
}
