#include "subs.h"

#include <ass/ass.h>
#include <limits.h>
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
  if (level > 2) /* past warnings: a line per font lookup and per track */
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
 * track, since rendering writes into both.
 *
 * The lines themselves live once, here, sorted by start on the item's timeline,
 * and outlive seeks: a jump back or a paused preview finds the line that was
 * already running. Each worker's track only ever holds the lines active at the
 * frame it renders, fed from that list and pruned behind it, so three tracks
 * cost what one would.
 *
 * Finished frames wait in a few slots; the caller takes the newest one that is
 * not in the future. A hash of what libass drew says whether it differs from
 * the frame on screen, so a line that holds still costs no upload. */

#define MAX_WORKERS 8

/* One dialogue line: `text` is the event after its ReadOrder ("Layer,Style,
 * Name,MarginL,MarginR,MarginV,Effect,Text"), and `id` stands in for the
 * ReadOrder, unique for as long as the list lives. Immutable once listed, and
 * freed only when no worker can hold it. */
typedef struct {
  int64_t start, end;
  uint32_t id;
  int length;
  char text[];
} entry;

typedef struct {
  entry **items;
  size_t count, capacity;
  int64_t longest; /* the longest line, bounding how far back one can start */
} entry_list;

typedef struct {
  pthread_t thread;
  ASS_Renderer *renderer;
  ASS_Track *track;
  /* What the track has seen: every line starting up to `fed_until` that was
   * listed before id `seen`, from list `epoch`. */
  int64_t fed_until;
  uint32_t seen, epoch;
  entry **batch;
  size_t batch_capacity;
  char *chunk;
  size_t chunk_capacity;
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

/* Serialises opening and closing, which join the workers, so it is held across
 * a render; `lock` never is. */
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
/* Guards everything below except each worker's renderer, track and buffers. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t work = PTHREAD_COND_INITIALIZER;

static ASS_Library *library;
/* Kept from one track to the next: a renderer caches the fonts it has looked
 * up, and a typeset track's attachments are megabytes of CJK face to load
 * again on every switch. */
static ASS_Renderer *renderers[MAX_WORKERS];
static worker workers[MAX_WORKERS];
static size_t worker_count;
static slot slots[MAX_WORKERS + 2];
static size_t slot_count;
static bool running;
static int frame_width = 1920, frame_height = 1080;
static int video_width = 1920, video_height = 1080;
static double frame_ms = 1000.0 / 24;
/* The picture in the overlay, and the row dialogue stays above. */
static int picture_top, picture_height = 1080;
static int keep_above = INT_MAX;
/* A jump back makes every slot in flight stale. */
static uint32_t generation;
static int64_t playhead, next_frame;

/* The selection the lines belong to, and whether they are the whole track. */
static uint64_t serial;
static bool complete;
static entry_list lines;
/* Lists replaced while workers could still be reading them. */
static entry_list retired;
/* Every container text track's recent lines, fed all along, so a switch starts
 * with the line already on screen rather than only what the demuxer reads
 * next. A window either side of the newest is kept. */
#define MAX_STREAMS 64
#define BACKLOG_MS 60000
static entry_list backlog[MAX_STREAMS];
/* The stream `lines` takes feeds from, -1 for none. */
static int open_stream = -1;
static uint32_t next_id, epoch;

/* The slot whose image the caller holds, until the next jf_subs_frame. */
static slot *shown;
static uint64_t shown_hash;
static bool shown_valid;
static jf_subs_image current;
static bool dirty;
static jf_subs_cost cost;

/* Monotonic milliseconds, for the timing lines in the log. */
static long long now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
/* When the track now rendering was opened, until its first picture shows. */
static long long opened_at;
static bool first_shown;

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
 * the line list */

static bool list_push(entry_list *list, entry *e) {
  if (list->count == list->capacity) {
    const size_t capacity = list->capacity ? list->capacity * 2 : 256;
    entry **grown = realloc(list->items, capacity * sizeof(*grown));
    if (grown == NULL)
      return false;
    list->items = grown;
    list->capacity = capacity;
  }
  list->items[list->count++] = e;
  if (e->end - e->start > list->longest)
    list->longest = e->end - e->start;
  return true;
}

static void list_free(entry_list *list) {
  for (size_t i = 0; i < list->count; i++)
    free(list->items[i]);
  free(list->items);
  *list = (entry_list){0};
}

/* The first line starting at or after `ms`. */
static size_t lower_bound(const entry_list *list, int64_t ms) {
  size_t low = 0, high = list->count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (list->items[mid]->start < ms)
      low = mid + 1;
    else
      high = mid;
  }
  return low;
}

static entry *make_entry(const char *text, int length, int64_t start,
                         int64_t end) {
  entry *e = malloc(sizeof(*e) + (size_t)length + 1);
  if (e == NULL)
    return NULL;
  *e = (entry){start, end, 0, length};
  memcpy(e->text, text, (size_t)length);
  e->text[length] = '\0';
  return e;
}

/* A worker feeds every line active at `ms` that its track has not seen: a
 * replaced list or a step back starts it over, and a line that arrived late
 * (listed after its last look) still gets in. Only pointers are taken under
 * the lock; the parsing is libass's, outside it. */
static void feed_window(worker *w, int64_t ms) {
  pthread_mutex_lock(&lock);
  const bool restart = w->epoch != epoch || ms < w->fed_until;
  if (restart) {
    w->epoch = epoch;
    w->fed_until = INT64_MIN;
    w->seen = 0;
  }
  size_t count = 0;
  for (size_t i = lower_bound(&lines, ms - lines.longest);
       i < lines.count && lines.items[i]->start <= ms; i++) {
    entry *e = lines.items[i];
    if (e->end <= ms || (e->start <= w->fed_until && e->id < w->seen))
      continue;
    if (count == w->batch_capacity) {
      const size_t capacity = count ? count * 2 : 64;
      entry **grown = realloc(w->batch, capacity * sizeof(*grown));
      if (grown == NULL)
        break;
      w->batch = grown;
      w->batch_capacity = capacity;
    }
    w->batch[count++] = e;
  }
  w->fed_until = ms;
  w->seen = next_id;
  pthread_mutex_unlock(&lock);

  if (restart)
    ass_flush_events(w->track);
  for (size_t i = 0; i < count; i++) {
    const entry *e = w->batch[i];
    const size_t need = (size_t)e->length + 16;
    if (need > w->chunk_capacity) {
      char *grown = realloc(w->chunk, need);
      if (grown == NULL)
        continue;
      w->chunk = grown;
      w->chunk_capacity = need;
    }
    const int length = snprintf(w->chunk, need, "%u,%s", e->id, e->text);
    ass_process_chunk(w->track, w->chunk, length, e->start, e->end - e->start);
  }
  ass_prune_events(w->track, ms);
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
    slot *s = playhead >= 0 && next_frame < playhead + (int64_t)slot_count - 1
                  ? claim()
                  : NULL;
    if (s == NULL) {
      pthread_cond_wait(&work, &lock);
      continue;
    }
    s->state = SLOT_BUSY;
    s->frame = next_frame++;
    s->generation = generation;
    /* libass moves only lines placed by their style at the bottom, not ones
     * with \pos or \move, which is how typesetting pins a sign to the
     * picture. A percentage of the way from the bottom to the top. */
    const int bottom = picture_top + picture_height;
    const double line_position =
        keep_above < bottom ? (bottom - keep_above) * 100.0 / picture_height
                            : 0;
    pthread_mutex_unlock(&lock);
    ass_set_line_position(w->renderer, line_position);

    const int64_t ms = (int64_t)((double)s->frame * frame_ms);
    feed_window(w, ms);
    compose(w, s, ms);

    pthread_mutex_lock(&lock);
    s->state = s->generation == generation ? SLOT_READY : SLOT_FREE;
  }
  pthread_mutex_unlock(&lock);
  return NULL;
}

static void free_worker(worker *w) {
  ass_free_track(w->track);
  free(w->batch);
  free(w->chunk);
  *w = (worker){0};
}

/* Joins the workers and drops the lines. Control held, lock not. */
static void stop(void) {
  pthread_mutex_lock(&lock);
  running = false;
  pthread_cond_broadcast(&work);
  const size_t count = worker_count;
  pthread_mutex_unlock(&lock);
  for (size_t i = 0; i < count; i++)
    pthread_join(workers[i].thread, NULL);

  pthread_mutex_lock(&lock);
  for (size_t i = 0; i < count; i++)
    free_worker(&workers[i]);
  worker_count = 0;
  list_free(&lines);
  list_free(&retired);
  complete = false;
  open_stream = -1;
  epoch++;
  for (size_t i = 0; i < slot_count; i++)
    slots[i].state = SLOT_FREE;
  shown = NULL;
  shown_valid = false;
  current = (jf_subs_image){0};
  dirty = true;
  pthread_mutex_unlock(&lock);
}

/* Styles from `header`, with the event format replaced by the one every line
 * here is written in: the Matroska order, which is also what FFmpeg decodes
 * every text format to. A script's own Format line may list fewer fields (an
 * SRT converted by the server does). */
static char *events_header(const char *header, int size, int *out_size) {
  static const char events[] =
      "\n[Events]\nFormat: Layer, Start, End, Style, Name, MarginL, MarginR, "
      "MarginV, Effect, Text\n";
  int keep = 0;
  while (keep < size && !(header[keep] == '[' && size - keep >= 8 &&
                          memcmp(header + keep, "[Events]", 8) == 0))
    keep++;
  char *out = malloc((size_t)keep + sizeof(events));
  if (out == NULL)
    return NULL;
  memcpy(out, header, (size_t)keep);
  memcpy(out + keep, events, sizeof(events));
  *out_size = keep + (int)sizeof(events) - 1;
  return out;
}

/* Starts the workers for `id`'s selection. Control held, workers stopped. */
static bool start(uint64_t id, const char *header, int header_size) {
  const long long started_at = now_ms();
  pthread_mutex_lock(&lock);
  bool ok = false;
  int script_size = 0;
  char *script = NULL;
  if (!have_library() ||
      (script = events_header(header != NULL ? header : "",
                              header != NULL ? header_size : 0,
                              &script_size)) == NULL)
    goto done;

  /* The frame is the whole overlay and the margins are the letterbox around the
   * picture, which subtitles stay out of, as mpv's sub-ass-use-margins=no. The
   * storage size is the video's own, which is what ScaledBorderAndShadow=no and
   * blur scale against. */
  const double fit =
      (double)frame_width / video_width < (double)frame_height / video_height
          ? (double)frame_width / video_width
          : (double)frame_height / video_height;
  const int picture_w = (int)(video_width * fit + 0.5);
  const int picture_h = (int)(video_height * fit + 0.5);
  const int side = (frame_width - picture_w) / 2;
  const int top = (frame_height - picture_h) / 2;
  picture_top = top;
  picture_height = picture_h;

  /* Configured, not online: the TV parks idle cores and brings them back under
   * load, so the online count is low exactly before rendering starts. */
  const long cores = sysconf(_SC_NPROCESSORS_CONF);
  size_t count = cores > 1 ? (size_t)cores - 1 : 1;
  if (count > MAX_WORKERS)
    count = MAX_WORKERS;
  for (worker_count = 0; worker_count < count; worker_count++) {
    worker *w = &workers[worker_count];
    *w = (worker){0};
    if (renderers[worker_count] == NULL) {
      renderers[worker_count] = ass_renderer_init(library);
      if (renderers[worker_count] == NULL)
        goto done;
      /* No system provider is compiled in (see tools/build-libass.sh): a
       * style resolves to a font the container attached, or else to this
       * default face. `update` has to be 1 for libass to load either. */
      ass_set_fonts(renderers[worker_count], pick_font(), "Sans",
                    ASS_FONTPROVIDER_NONE, NULL, 1);
      /* HarfBuzz is linked in, so the shaper that uses it is the one to ask
       * for. */
      ass_set_shaper(renderers[worker_count], ASS_SHAPING_COMPLEX);
    }
    w->renderer = renderers[worker_count];
    ass_set_frame_size(w->renderer, frame_width, frame_height);
    ass_set_margins(w->renderer, top, frame_height - picture_h - top, side,
                    frame_width - picture_w - side);
    ass_set_use_margins(w->renderer, 0);
    ass_set_storage_size(w->renderer, video_width, video_height);
    /* ass_read_memory parses in place, so each track gets its own copy. */
    char *copy = malloc((size_t)script_size);
    if (copy == NULL)
      goto done;
    memcpy(copy, script, (size_t)script_size);
    w->track = ass_read_memory(library, copy, (size_t)script_size, NULL);
    free(copy);
    if (w->track == NULL)
      goto done;
  }

  opened_at = now_ms();
  first_shown = false;
  slot_count = worker_count + 2;
  for (size_t i = 0; i < slot_count; i++)
    slots[i].state = SLOT_FREE;
  serial = id;
  generation++;
  epoch++;
  /* Unknown until the first jf_subs_frame: nothing renders before then, or
   * the item's first frame would be drawn in the meantime. */
  playhead = -1;
  next_frame = -1;
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
      for (size_t j = i; j < worker_count; j++)
        free_worker(&workers[j]);
      worker_count = i;
      ok = worker_count > 0;
      break;
    }

  fprintf(stderr, "t=%lld subtitles: %zu renderer(s) up in %lld ms\n", now_ms(),
          worker_count, now_ms() - started_at);
done:
  free(script);
  if (!ok) {
    for (size_t i = 0; i < worker_count; i++)
      free_worker(&workers[i]);
    worker_count = 0;
    running = false;
  }
  pthread_mutex_unlock(&lock);
  return ok;
}

void jf_subs_geometry(int width, int height, int video_w, int video_h,
                      double video_frame_ms) {
  if (width <= 0 || height <= 0 || video_w <= 0 || video_h <= 0)
    return;
  pthread_mutex_lock(&lock);
  frame_width = width;
  frame_height = height;
  video_width = video_w;
  video_height = video_h;
  frame_ms = video_frame_ms > 0 ? video_frame_ms : 1000.0 / 24;
  pthread_mutex_unlock(&lock);
}

/* True when `id` is older than the selection already held, or is it and has
 * workers. Lock held. */
static bool superseded(uint64_t id) {
  return id < serial || (id == serial && worker_count > 0);
}

/* A copy of the line into `list` unless it is there already; NULL then. Lock
 * held. */
static entry *insert_line(entry_list *list, const char *text, int length,
                          int64_t start_ms, int64_t end_ms) {
  /* After everything starting no later, so a line fed twice - read again after
   * a seek back - is found right before it. */
  const size_t at = lower_bound(list, start_ms + 1);
  for (size_t i = at; i > 0 && list->items[i - 1]->start == start_ms; i--) {
    const entry *e = list->items[i - 1];
    if (e->end == end_ms && e->length == length &&
        memcmp(e->text, text, (size_t)length) == 0)
      return NULL;
  }
  entry *e = make_entry(text, length, start_ms, end_ms);
  if (e == NULL)
    return NULL;
  if (!list_push(list, e)) {
    free(e);
    return NULL;
  }
  memmove(list->items + at + 1, list->items + at,
          (list->count - 1 - at) * sizeof(*list->items));
  list->items[at] = e;
  return e;
}

bool jf_subs_open(uint64_t id, int stream, const char *header,
                  int header_size) {
  pthread_mutex_lock(&control);
  pthread_mutex_lock(&lock);
  const bool skip = superseded(id);
  const bool held = id == serial && worker_count > 0;
  pthread_mutex_unlock(&lock);
  bool ok = held;
  if (!skip) {
    stop();
    ok = start(id, header, header_size);
    if (ok && stream >= 0 && stream < MAX_STREAMS) {
      pthread_mutex_lock(&lock);
      open_stream = stream;
      const entry_list *known = &backlog[stream];
      for (size_t i = 0; i < known->count; i++) {
        const entry *from = known->items[i];
        entry *e = insert_line(&lines, from->text, from->length, from->start,
                               from->end);
        if (e != NULL)
          e->id = next_id++;
      }
      pthread_mutex_unlock(&lock);
    }
  }
  pthread_mutex_unlock(&control);
  return ok;
}

/* The events of a whole script, in the list's shape and sorted by start. */
static bool parse_script(char *data, size_t size, entry_list *out) {
  pthread_mutex_lock(&lock);
  const bool have = have_library();
  pthread_mutex_unlock(&lock);
  if (!have)
    return false;
  /* The library is only read for its message callback while parsing. */
  ASS_Track *track = ass_read_memory(library, data, size, NULL);
  if (track == NULL)
    return false;
  bool ok = true;
  for (int i = 0; i < track->n_events && ok; i++) {
    const ASS_Event *ev = &track->events[i];
    const char *style = ev->Style >= 0 && ev->Style < track->n_styles
                            ? track->styles[ev->Style].Name
                            : "Default";
    char head[64];
    snprintf(head, sizeof(head), "%d,", ev->Layer);
    const char *name = ev->Name ? ev->Name : "";
    const char *effect = ev->Effect ? ev->Effect : "";
    const char *text = ev->Text ? ev->Text : "";
    const int length =
        snprintf(NULL, 0, "%s%s,%s,%d,%d,%d,%s,%s", head, style, name,
                 ev->MarginL, ev->MarginR, ev->MarginV, effect, text);
    entry *e = malloc(sizeof(*e) + (size_t)length + 1);
    if (e == NULL) {
      ok = false;
      break;
    }
    *e = (entry){ev->Start, ev->Start + ev->Duration, 0, length};
    snprintf(e->text, (size_t)length + 1, "%s%s,%s,%d,%d,%d,%s,%s", head, style,
             name, ev->MarginL, ev->MarginR, ev->MarginV, effect, text);
    ok = list_push(out, e);
    if (!ok)
      free(e);
  }
  ass_free_track(track);
  if (!ok) {
    list_free(out);
    return false;
  }
  /* Scripts are mostly in order already; insertion sort is linear then. */
  for (size_t i = 1; i < out->count; i++) {
    entry *e = out->items[i];
    size_t j = i;
    while (j > 0 && out->items[j - 1]->start > e->start) {
      out->items[j] = out->items[j - 1];
      j--;
    }
    out->items[j] = e;
  }
  return true;
}

/* Case-insensitive prefix test. */
static bool starts(const char *at, const char *end, const char *word) {
  for (; *word != '\0'; at++, word++)
    if (at == end || (*at | 0x20) != *word)
      return false;
  return true;
}

/* The server converts SRT and friends to ASS by wrapping them, not
 * translating them: HTML-ish tags stay as they were, and a line break is
 * written `\n`, which ASS reads as a space unless WrapStyle is 2. This turns
 * both into ASS, as FFmpeg's own decoders do for the container's copy. A
 * converted script is known by its short event format, which lacks the Name
 * field every real ASS script has. Takes `data`, returns the result (`data`
 * itself when there is nothing to convert), NUL-terminated, or NULL. */
static char *from_markup(char *data, size_t *size) {
  const char *events = strstr(data, "[Events]");
  const char *format = events != NULL ? strstr(events, "Format:") : NULL;
  const char *eol = format != NULL ? strchr(format, '\n') : NULL;
  bool converted = false;
  if (format != NULL) {
    const size_t length = eol != NULL ? (size_t)(eol - format) : strlen(format);
    converted = true;
    for (size_t i = 0; i + 4 <= length && converted; i++)
      converted = !starts(format + i, format + length, "name");
  }
  /* A real ASS script is left alone: markup there would be a typo, and a
   * typeset track's copy is a hundred megabytes the TV cannot spare twice. */
  if (!converted)
    return data;
  /* Worst case, `<b>` (3) becomes `{\b1}` (5); a colour tag only shrinks. */
  char *out = malloc(*size * 2 + 1);
  if (out == NULL) {
    free(data);
    return NULL;
  }
  const char *at = data, *end = data + *size;
  char *to = out;
  while (at < end) {
    if (at[0] == '\\' && at + 1 < end && at[1] == 'n') {
      memcpy(to, "\\N", 2), to += 2, at += 2;
      continue;
    }
    if (at[0] == '<') {
      const bool closing = at + 1 < end && at[1] == '/';
      const char *name = at + 1 + closing;
      const char *close = memchr(at, '>', (size_t)(end - at));
      const char *line_end = memchr(at, '\n', (size_t)(end - at));
      if (close != NULL && (line_end == NULL || close < line_end)) {
        char tag = 0;
        if (close - name == 1 && strchr("ibus", *name | 0x20) != NULL)
          tag = (char)(*name | 0x20);
        if (tag != 0) {
          to += sprintf(to, "{\\%c%c}", tag, closing ? '0' : '1');
          at = close + 1;
          continue;
        }
        if (starts(name, close, "font")) {
          /* Only the colour survives; ASS writes it blue-green-red. */
          const char *hash = memchr(name, '#', (size_t)(close - name));
          unsigned rgb = 0;
          if (closing)
            to += sprintf(to, "{\\c}");
          else if (hash != NULL && close - hash >= 7 &&
                   sscanf(hash + 1, "%6x", &rgb) == 1)
            to += sprintf(to, "{\\c&H%02X%02X%02X&}", rgb & 0xff,
                          (rgb >> 8) & 0xff, rgb >> 16);
          at = close + 1;
          continue;
        }
      }
    }
    *to++ = *at++;
  }
  *to = '\0';
  *size = (size_t)(to - out);
  free(data);
  return out;
}

bool jf_subs_open_file(uint64_t id, char *data, size_t size) {
  data = from_markup(data, &size);
  if (data == NULL)
    return false;
  /* The header before ass_read_memory, which parses in place. */
  int header_size = 0;
  char *header = events_header(data, (int)(size < INT32_MAX ? size : INT32_MAX),
                               &header_size);
  entry_list parsed = {0};
  const bool parsed_ok = header != NULL && parse_script(data, size, &parsed);
  free(data);
  if (!parsed_ok) {
    free(header);
    return false;
  }

  pthread_mutex_lock(&control);
  pthread_mutex_lock(&lock);
  const bool stale = id < serial;
  const bool held = id == serial && worker_count > 0;
  pthread_mutex_unlock(&lock);
  bool ok = false;
  if (!stale) {
    /* Already rendering this selection from the demuxer: its header has the
     * same styles, so only the lines change hands. */
    ok = held || (stop(), start(id, header, header_size));
    if (ok) {
      pthread_mutex_lock(&lock);
      for (size_t i = 0; i < lines.count; i++)
        list_push(&retired, lines.items[i]);
      free(lines.items);
      lines = parsed;
      parsed = (entry_list){0};
      for (size_t i = 0; i < lines.count; i++)
        lines.items[i]->id = next_id++;
      complete = true;
      epoch++;
      pthread_cond_broadcast(&work);
      pthread_mutex_unlock(&lock);
    }
  }
  pthread_mutex_unlock(&control);
  list_free(&parsed);
  free(header);
  return ok;
}

void jf_subs_close(uint64_t id) {
  pthread_mutex_lock(&control);
  pthread_mutex_lock(&lock);
  const bool act = id >= serial;
  if (act)
    serial = id;
  pthread_mutex_unlock(&lock);
  if (act)
    stop();
  pthread_mutex_unlock(&control);
}

void jf_subs_release(void) {
  pthread_mutex_lock(&control);
  stop();
  pthread_mutex_lock(&lock);
  serial = 0;
  for (size_t i = 0; i < MAX_STREAMS; i++)
    list_free(&backlog[i]);
  for (size_t i = 0; i < MAX_WORKERS; i++) {
    if (renderers[i] != NULL)
      ass_renderer_done(renderers[i]);
    renderers[i] = NULL;
  }
  if (library != NULL)
    ass_library_done(library);
  library = NULL;
  pthread_mutex_unlock(&lock);
  pthread_mutex_unlock(&control);
}

bool jf_subs_ready(void) {
  pthread_mutex_lock(&lock);
  const bool ready = worker_count > 0;
  pthread_mutex_unlock(&lock);
  return ready;
}

void jf_subs_feed(int stream, const char *text, int length, int64_t start_ms,
                  int64_t duration_ms) {
  /* Past the ReadOrder, which a decoder restarts after every seek and so says
   * nothing about whether a line is new. */
  const char *comma =
      text != NULL && length > 0 ? memchr(text, ',', (size_t)length) : NULL;
  if (comma == NULL || duration_ms <= 0 || stream < 0 || stream >= MAX_STREAMS)
    return;
  const int rest = length - (int)(comma + 1 - text);
  const int64_t end_ms = start_ms + duration_ms;
  pthread_mutex_lock(&lock);
  entry_list *known = &backlog[stream];
  if (insert_line(known, comma + 1, rest, start_ms, end_ms) != NULL &&
      known->count % 64 == 0) {
    size_t kept = 0;
    for (size_t i = 0; i < known->count; i++) {
      entry *e = known->items[i];
      if (e->end < start_ms - BACKLOG_MS || e->start > start_ms + BACKLOG_MS)
        free(e);
      else
        known->items[kept++] = e;
    }
    known->count = kept;
  }
  if (stream == open_stream && worker_count > 0 && !complete) {
    entry *e = insert_line(&lines, comma + 1, rest, start_ms, end_ms);
    if (e != NULL)
      e->id = next_id++;
  }
  pthread_mutex_unlock(&lock);
}

bool jf_subs_frame(int64_t media_ms, jf_subs_image *out) {
  pthread_mutex_lock(&lock);
  if (worker_count > 0) {
    const int64_t target = (int64_t)((double)media_ms / frame_ms);
    if (target != playhead) {
      /* A jump either way leaves nothing rendered worth showing: frames from
       * before a forward one are still "not in the future", and the one on
       * screen is far better than those. */
      if (target < playhead || target >= playhead + (int64_t)slot_count) {
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
      if (!first_shown && best->image.w > 0) {
        first_shown = true;
        fprintf(stderr,
                "t=%lld subtitles: first picture on screen %lld ms after open, "
                "frame %lld rendered in %.1f ms\n",
                now_ms(), now_ms() - opened_at, (long long)best->frame,
                best->cost.render_ms);
      }
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

void jf_subs_keep_above(int y) {
  pthread_mutex_lock(&lock);
  if (y != keep_above) {
    keep_above = y;
    /* Everything rendered ahead is in the old place: again from here. */
    generation++;
    next_frame = playhead;
    pthread_cond_broadcast(&work);
  }
  pthread_mutex_unlock(&lock);
}

jf_subs_cost jf_subs_last_cost(void) {
  pthread_mutex_lock(&lock);
  const jf_subs_cost out = cost;
  pthread_mutex_unlock(&lock);
  return out;
}
