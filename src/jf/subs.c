#include "subs.h"

#include <ass/ass.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* The same faces the UI rasterises with, in the same order; see
 * ui/glyph_atlas.c and docs/fonts.md. With no font provider compiled in, this
 * is what every style resolves to, so it has to be a face that exists. */
static const char *const font_candidates[] = {
    "/usr/share/fonts/LG_Smart_UI-Regular.ttf",
    "/usr/share/fonts/DroidSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};

static void ass_log(int level, const char *format, va_list args, void *unused) {
  (void)unused;
  if (level > 4) /* libass levels above 4 are per-glyph chatter */
    return;
  fprintf(stderr, "libass: ");
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
}

const char *jf_subs_font;

static const char *pick_font(void) {
  if (jf_subs_font != NULL)
    return jf_subs_font;
  for (size_t i = 0; i < sizeof(font_candidates) / sizeof(*font_candidates);
       i++) {
    FILE *probe = fopen(font_candidates[i], "rb");
    if (probe != NULL) {
      fclose(probe);
      return font_candidates[i];
    }
  }
  return NULL;
}

/* libass takes 30-90 ms a frame on the TV for a typeset sign, so rendering runs
 * on one worker per core but one, each ahead of the playback position on its
 * own frame of the video's frame grid. They share the library - the attached
 * fonts, read-only once they run - and each has its own renderer and its own
 * copy of the track, since rendering writes into both.
 *
 * Finished frames wait in a few slots; the caller takes the newest one that is
 * not in the future. A hash of what libass drew says whether it differs from
 * the frame on screen, so a line that holds still costs no upload. */

#define MAX_WORKERS 8

typedef struct line {
  struct line *next;
  int64_t start_ms, duration_ms;
  int length;
  char text[];
} line;

typedef struct {
  pthread_t thread;
  ASS_Renderer *renderer;
  ASS_Track *track;
  /* Lines and flushes this worker's track has yet to see, so feeding never
   * waits for a render. */
  pthread_mutex_t feed_lock;
  line *pending, **pending_tail;
  bool flush_pending;
} worker;

typedef enum { SLOT_FREE, SLOT_BUSY, SLOT_READY } slot_state;

typedef struct {
  slot_state state;
  int64_t frame;
  uint32_t generation;
  uint64_t hash;
  jf_subs_image image;
  /* Never freed: the caller uploads from it after jf_subs_frame returns, and a
   * track change on another thread must not pull it away. */
  uint8_t *canvas;
  size_t capacity;
  jf_subs_cost cost;
} slot;

/* Guards everything below except each worker's renderer, track and pending
 * lines. Held only between renders, never across one. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work = PTHREAD_COND_INITIALIZER;

static ASS_Library *library;
static worker workers[MAX_WORKERS];
static size_t worker_count;
static slot slots[MAX_WORKERS + 2];
static size_t slot_count;
static bool running;
static int frame_width, frame_height;
static double frame_ms;
/* A flush or a jump back makes every slot in flight stale. */
static uint32_t generation;
static int64_t playhead, next_frame;

/* The slot whose image the caller holds, until the next jf_subs_frame. */
static slot *shown;
static uint64_t shown_hash;
static bool shown_valid;
static jf_subs_image current;
static bool dirty;
static jf_subs_cost cost;

/* The library holds the attached fonts, so it outlives a track. Called with the
 * lock held. */
static bool have_library(void) {
  if (library == NULL) {
    library = ass_library_init();
    if (library != NULL)
      ass_set_message_cb(library, ass_log, NULL);
  }
  return library != NULL;
}

void jf_subs_add_font(const char *name, const uint8_t *data, int size) {
  if (data == NULL || size <= 0)
    return;
  pthread_mutex_lock(&lock);
  if (have_library())
    ass_add_font(library, name != NULL ? name : "attachment",
                 (const char *)data, size);
  pthread_mutex_unlock(&lock);
}

/* ------------------------------------------------------------------
 * compositing */

/* libass hands back one 8-bit coverage bitmap per colour run. Blending them
 * into a single image costs one upload instead of one draw call each, and the
 * bound is the union of their rectangles rather than the screen: a line of
 * dialogue is a strip near the bottom, not a frame. */

static void bounds(const ASS_Image *image, int *x0, int *y0, int *x1, int *y1) {
  *x0 = frame_width;
  *y0 = frame_height;
  *x1 = 0;
  *y1 = 0;
  for (const ASS_Image *it = image; it != NULL; it = it->next) {
    if (it->w <= 0 || it->h <= 0)
      continue;
    if (it->dst_x < *x0)
      *x0 = it->dst_x;
    if (it->dst_y < *y0)
      *y0 = it->dst_y;
    if (it->dst_x + it->w > *x1)
      *x1 = it->dst_x + it->w;
    if (it->dst_y + it->h > *y1)
      *y1 = it->dst_y + it->h;
  }
}

/* x / 255, rounded, exact for every x up to 255 * 255. Only adds and shifts, so
 * the loops using it vectorise. */
static inline unsigned div255(unsigned x) {
  return (x + 128 + ((x + 128) >> 8)) >> 8;
}

/* `over`, with the destination held premultiplied so overlapping runs - a glyph
 * on its own outline and shadow - accumulate correctly. It stays premultiplied:
 * the renderer's shader for it divides the alpha back out.
 *
 * Written for the vectoriser: no branch in the pixel loop (a zero coverage
 * blends to the same pixel), every product fits 16 bits, and everything read
 * inside the loop is a local, since a store through a uint8_t pointer could
 * otherwise alias `it` and force it to be reloaded per pixel. */
static void blend(uint8_t *dst, int pitch, int origin_x, int origin_y,
                  const ASS_Image *it) {
  const unsigned r = (uint8_t)(it->color >> 24);
  const unsigned g = (uint8_t)(it->color >> 16);
  const unsigned b = (uint8_t)(it->color >> 8);
  const unsigned opacity = 255u - (it->color & 0xff);
  if (opacity == 0)
    return;
  const int w = it->w, h = it->h, stride = it->stride;
  const uint8_t *bitmap = it->bitmap;
  uint8_t *origin = dst + (size_t)(it->dst_y - origin_y) * pitch +
                    (size_t)(it->dst_x - origin_x) * 4;
  for (int y = 0; y < h; y++) {
    const uint8_t *restrict source = bitmap + (size_t)y * stride;
    uint8_t *restrict row = origin + (size_t)y * pitch;
    for (int x = 0; x < w; x++) {
      const unsigned k = div255(source[x] * opacity);
      const unsigned keep = 255u - k;
      row[x * 4 + 0] = (uint8_t)div255(r * k + row[x * 4 + 0] * keep);
      row[x * 4 + 1] = (uint8_t)div255(g * k + row[x * 4 + 1] * keep);
      row[x * 4 + 2] = (uint8_t)div255(b * k + row[x * 4 + 2] * keep);
      row[x * 4 + 3] = (uint8_t)(k + div255(row[x * 4 + 3] * keep));
    }
  }
}

static double cpu_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static uint64_t mix(uint64_t hash, uint32_t value) {
  return (hash ^ value) * 0x100000001b3ull;
}

/* What libass drew, as a number: placement, colour and every coverage byte. */
static uint64_t hash_image(uint64_t hash, const ASS_Image *it) {
  hash = mix(hash, (uint32_t)it->dst_x);
  hash = mix(hash, (uint32_t)it->dst_y);
  hash = mix(hash, (uint32_t)it->w);
  hash = mix(hash, (uint32_t)it->h);
  hash = mix(hash, it->color);
  for (int y = 0; y < it->h; y++) {
    const uint8_t *row = it->bitmap + (size_t)y * it->stride;
    int x = 0;
    for (; x + 4 <= it->w; x += 4) {
      uint32_t word;
      memcpy(&word, row + x, sizeof(word));
      hash = mix(hash, word);
    }
    for (; x < it->w; x++)
      hash = mix(hash, row[x]);
  }
  return hash;
}

/* Renders `media_ms` with this worker's renderer into the slot, which the
 * worker owns while it is busy. */
static void compose(worker *w, slot *s, int64_t media_ms) {
  const double started = cpu_ms();
  ASS_Image *image = ass_render_frame(w->renderer, w->track, media_ms, NULL);
  const double rendered = cpu_ms();
  uint64_t hash = 0xcbf29ce484222325ull;
  unsigned runs = 0;
  s->image = (jf_subs_image){0};

  int x0, y0, x1, y1;
  bounds(image, &x0, &y0, &x1, &y1);
  const int width = x1 - x0, height = y1 - y0;
  const size_t need =
      (size_t)(width > 0 ? width : 0) * (size_t)(height > 0 ? height : 0) * 4;
  if (need > s->capacity) {
    uint8_t *grown = realloc(s->canvas, need);
    if (grown != NULL) {
      s->canvas = grown;
      s->capacity = need;
    }
  }
  if (need > 0 && need <= s->capacity) {
    memset(s->canvas, 0, need);
    for (const ASS_Image *it = image; it != NULL; it = it->next) {
      if (it->w <= 0 || it->h <= 0)
        continue;
      runs++;
      blend(s->canvas, width * 4, x0, y0, it);
      hash = hash_image(hash, it);
    }
    s->image = (jf_subs_image){x0, y0, width, height, s->canvas};
  }
  s->hash = hash;
  s->cost = (jf_subs_cost){rendered - started, cpu_ms() - rendered, runs};
}

/* Everything fed or flushed since this worker last looked, in order. */
static void catch_up(worker *w) {
  pthread_mutex_lock(&w->feed_lock);
  line *list = w->pending;
  const bool flush = w->flush_pending;
  w->pending = NULL;
  w->pending_tail = &w->pending;
  w->flush_pending = false;
  pthread_mutex_unlock(&w->feed_lock);
  if (flush)
    ass_flush_events(w->track);
  while (list != NULL) {
    line *next = list->next;
    ass_process_chunk(w->track, list->text, list->length, list->start_ms,
                      list->duration_ms);
    free(list);
    list = next;
  }
}

/* A slot nothing needs: free, or finished for a frame already behind, and not
 * on screen. Lock held. */
static slot *claim(void) {
  for (size_t i = 0; i < slot_count; i++) {
    slot *s = &slots[i];
    if (s == shown || s->state == SLOT_BUSY)
      continue;
    if (s->state == SLOT_FREE || s->generation != generation ||
        s->frame < playhead)
      return s;
  }
  return NULL;
}

static void *render_loop(void *arg) {
  worker *w = arg;
  pthread_mutex_lock(&lock);
  while (running) {
    if (next_frame < playhead)
      next_frame = playhead;
    /* No further ahead than the slots can hold. */
    slot *s = next_frame < playhead + (int64_t)slot_count - 1 ? claim() : NULL;
    if (s == NULL) {
      pthread_cond_wait(&work, &lock);
      continue;
    }
    s->state = SLOT_BUSY;
    s->frame = next_frame++;
    s->generation = generation;
    pthread_mutex_unlock(&lock);

    catch_up(w);
    compose(w, s, (int64_t)((double)s->frame * frame_ms));

    pthread_mutex_lock(&lock);
    s->state = s->generation == generation ? SLOT_READY : SLOT_FREE;
  }
  pthread_mutex_unlock(&lock);
  return NULL;
}

static void drop_lines(line *list) {
  while (list != NULL) {
    line *next = list->next;
    free(list);
    list = next;
  }
}

bool jf_subs_open(const char *header, int header_size, int width, int height,
                  int video_width, int video_height, double video_frame_ms) {
  if (width <= 0 || height <= 0 || video_width <= 0 || video_height <= 0)
    return false;
  jf_subs_close();
  pthread_mutex_lock(&lock);
  bool ok = false;
  if (!have_library())
    goto done;

  /* The frame is the whole overlay and the margins are the letterbox around the
   * picture, which subtitles stay out of, as mpv's sub-ass-use-margins=no. The
   * storage size is the video's own, which is what ScaledBorderAndShadow=no and
   * blur scale against. */
  const double fit = (double)width / video_width < (double)height / video_height
                         ? (double)width / video_width
                         : (double)height / video_height;
  const int picture_w = (int)(video_width * fit + 0.5);
  const int picture_h = (int)(video_height * fit + 0.5);
  const int side = (width - picture_w) / 2, top = (height - picture_h) / 2;

  /* Configured, not online: the TV parks idle cores and brings them back under
   * load, so the online count is low exactly before rendering starts. */
  const long cores = sysconf(_SC_NPROCESSORS_CONF);
  size_t count = cores > 1 ? (size_t)cores - 1 : 1;
  if (count > MAX_WORKERS)
    count = MAX_WORKERS;
  for (worker_count = 0; worker_count < count; worker_count++) {
    worker *w = &workers[worker_count];
    *w = (worker){0};
    w->pending_tail = &w->pending;
    pthread_mutex_init(&w->feed_lock, NULL);
    w->renderer = ass_renderer_init(library);
    if (w->renderer == NULL)
      goto done;
    ass_set_frame_size(w->renderer, width, height);
    ass_set_margins(w->renderer, top, height - picture_h - top, side,
                    width - picture_w - side);
    ass_set_use_margins(w->renderer, 0);
    ass_set_storage_size(w->renderer, video_width, video_height);
    /* No system provider is compiled in (see tools/build-libass.sh): a style
     * resolves to a font the container attached, or else to this default face.
     * `update` has to be 1 for libass to load either. */
    ass_set_fonts(w->renderer, pick_font(), "Sans", ASS_FONTPROVIDER_NONE, NULL,
                  1);
    /* HarfBuzz is linked in, so the shaper that uses it is the one to ask for.
     */
    ass_set_shaper(w->renderer, ASS_SHAPING_COMPLEX);
    if (header != NULL && header_size > 0) {
      /* ass_read_memory parses in place, so each track gets its own copy. */
      char *copy = malloc((size_t)header_size);
      if (copy == NULL)
        goto done;
      memcpy(copy, header, (size_t)header_size);
      w->track = ass_read_memory(library, copy, (size_t)header_size, NULL);
      free(copy);
    } else {
      w->track = ass_new_track(library);
    }
    if (w->track == NULL) {
      ass_renderer_done(w->renderer);
      goto done;
    }
  }

  frame_width = width;
  frame_height = height;
  frame_ms = video_frame_ms > 0 ? video_frame_ms : 1000.0 / 24;
  slot_count = worker_count + 2;
  for (size_t i = 0; i < slot_count; i++)
    slots[i].state = SLOT_FREE;
  generation++;
  playhead = 0;
  next_frame = 0;
  shown = NULL;
  shown_valid = false;
  current = (jf_subs_image){0};
  dirty = true;
  running = true;
  ok = true;
  for (size_t i = 0; i < worker_count; i++)
    if (pthread_create(&workers[i].thread, NULL, render_loop, &workers[i]) !=
        0) {
      /* Fewer workers still render; none would be a closed track. */
      for (size_t j = i; j < worker_count; j++) {
        ass_free_track(workers[j].track);
        ass_renderer_done(workers[j].renderer);
        pthread_mutex_destroy(&workers[j].feed_lock);
      }
      worker_count = i;
      ok = worker_count > 0;
      break;
    }

done:
  if (!ok) {
    for (size_t i = 0; i < worker_count; i++) {
      ass_free_track(workers[i].track);
      ass_renderer_done(workers[i].renderer);
      pthread_mutex_destroy(&workers[i].feed_lock);
    }
    worker_count = 0;
    running = false;
  }
  pthread_mutex_unlock(&lock);
  return ok;
}

void jf_subs_close(void) {
  pthread_mutex_lock(&lock);
  running = false;
  pthread_cond_broadcast(&work);
  const size_t count = worker_count;
  pthread_mutex_unlock(&lock);
  for (size_t i = 0; i < count; i++)
    pthread_join(workers[i].thread, NULL);

  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < count; i++) {
    worker *w = &workers[i];
    ass_free_track(w->track);
    ass_renderer_done(w->renderer);
    drop_lines(w->pending);
    pthread_mutex_destroy(&w->feed_lock);
  }
  worker_count = 0;
  for (size_t i = 0; i < slot_count; i++)
    slots[i].state = SLOT_FREE;
  shown = NULL;
  shown_valid = false;
  current = (jf_subs_image){0};
  dirty = true;
  pthread_mutex_unlock(&lock);
}

void jf_subs_release(void) {
  jf_subs_close();
  pthread_mutex_lock(&lock);
  if (library != NULL)
    ass_library_done(library);
  library = NULL;
  pthread_mutex_unlock(&lock);
}

bool jf_subs_ready(void) {
  pthread_mutex_lock(&lock);
  const bool ready = worker_count > 0;
  pthread_mutex_unlock(&lock);
  return ready;
}

void jf_subs_feed(const char *text, int length, int64_t start_ms,
                  int64_t duration_ms) {
  if (text == NULL || length <= 0)
    return;
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < worker_count; i++) {
    line *l = malloc(sizeof(*l) + (size_t)length);
    if (l == NULL)
      break;
    *l = (line){NULL, start_ms, duration_ms, length};
    memcpy(l->text, text, (size_t)length);
    worker *w = &workers[i];
    pthread_mutex_lock(&w->feed_lock);
    *w->pending_tail = l;
    w->pending_tail = &l->next;
    pthread_mutex_unlock(&w->feed_lock);
  }
  pthread_mutex_unlock(&lock);
}

void jf_subs_flush(void) {
  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < worker_count; i++) {
    worker *w = &workers[i];
    pthread_mutex_lock(&w->feed_lock);
    drop_lines(w->pending);
    w->pending = NULL;
    w->pending_tail = &w->pending;
    w->flush_pending = true;
    pthread_mutex_unlock(&w->feed_lock);
  }
  generation++;
  next_frame = playhead;
  shown_valid = false;
  current = (jf_subs_image){0};
  dirty = true;
  pthread_cond_broadcast(&work);
  pthread_mutex_unlock(&lock);
}

bool jf_subs_frame(int64_t media_ms, jf_subs_image *out) {
  pthread_mutex_lock(&lock);
  if (worker_count > 0) {
    const int64_t target = (int64_t)((double)media_ms / frame_ms);
    if (target != playhead) {
      if (target < playhead) {
        generation++;
        next_frame = target;
      }
      playhead = target;
      pthread_cond_broadcast(&work);
    }
    /* The caller is done with the last image by now. */
    if (shown != NULL && shown->generation != generation) {
      shown->state = SLOT_FREE;
      shown = NULL;
    }
    slot *best = shown;
    for (size_t i = 0; i < slot_count; i++) {
      slot *s = &slots[i];
      if (s->state == SLOT_READY && s->generation == generation &&
          s->frame <= target && (best == NULL || s->frame > best->frame))
        best = s;
    }
    if (best != shown) {
      if (shown != NULL)
        shown->state = SLOT_FREE;
      shown = best;
      if (!shown_valid || best->hash != shown_hash) {
        dirty = true;
        cost = best->cost;
      }
      shown_hash = best->hash;
      shown_valid = true;
      current = best->image;
      pthread_cond_broadcast(&work);
    }
  }
  const bool changed = dirty;
  dirty = false;
  *out = current;
  pthread_mutex_unlock(&lock);
  return changed;
}

jf_subs_cost jf_subs_last_cost(void) {
  pthread_mutex_lock(&lock);
  const jf_subs_cost out = cost;
  pthread_mutex_unlock(&lock);
  return out;
}
