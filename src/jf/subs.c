#include "subs.h"

#include <ass/ass.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* ponytail: one lock for the library, the renderer and the track. The feed is a
 * handful of short calls a minute and compositing is a few a second, so there
 * is nothing here to contend for; per-object locks if that ever stops being
 * true. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static ASS_Library *library;
static ASS_Renderer *renderer;
static ASS_Track *track;
static int frame_width, frame_height;

static uint8_t *canvas;
static size_t canvas_capacity;
static jf_subs_image composited;
/* False until a frame has been composed, so the first call after an open or a
 * flush always produces one even though libass reports no change from nothing
 * to nothing. */
static bool composed;
/* One burst of run geometry per track, no more: this is a diagnostic, not a
 * trace. */
static bool logged_runs;

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

bool jf_subs_open(const char *header, int header_size, int width, int height) {
  if (width <= 0 || height <= 0)
    return false;
  jf_subs_close();
  pthread_mutex_lock(&lock);
  bool ok = false;

  library = ass_library_init();
  if (library == NULL)
    goto done;
  ass_set_message_cb(library, ass_log, NULL);
  renderer = ass_renderer_init(library);
  if (renderer == NULL)
    goto done;
  ass_set_frame_size(renderer, width, height);
  ass_set_storage_size(renderer, width, height);
  /* No provider is compiled in (see tools/build-libass.sh), so the default face
   * is the only face; `update` still has to be 1 for libass to load it. */
  ass_set_fonts(renderer, pick_font(), "Sans", ASS_FONTPROVIDER_NONE, NULL, 1);
  /* HarfBuzz is linked in, so the shaper that uses it is the one to ask for. */
  ass_set_shaper(renderer, ASS_SHAPING_COMPLEX);

  track = header != NULL && header_size > 0
              ? ass_read_memory(library, (char *)(uintptr_t)header,
                                (size_t)header_size, NULL)
              : ass_new_track(library);
  if (track == NULL)
    goto done;
  frame_width = width;
  frame_height = height;
  composed = false;
  logged_runs = false;
  composited.w = 0;
  composited.h = 0;
  ok = true;

done:
  pthread_mutex_unlock(&lock);
  if (!ok)
    jf_subs_close();
  return ok;
}

void jf_subs_close(void) {
  pthread_mutex_lock(&lock);
  if (track != NULL)
    ass_free_track(track);
  if (renderer != NULL)
    ass_renderer_done(renderer);
  if (library != NULL)
    ass_library_done(library);
  track = NULL;
  renderer = NULL;
  library = NULL;
  /* The canvas outlives the track deliberately. The caller uploads
   * `composited.rgba` after jf_subs_frame has returned and the lock is gone, so
   * freeing it here - from the demux thread, on a track change - would pull the
   * buffer out from under a texture upload in progress. Nothing else writes it:
   * composing happens only inside jf_subs_frame, on the thread doing that
   * upload. */
  composited.w = 0;
  composited.h = 0;
  composed = false;
  pthread_mutex_unlock(&lock);
}

bool jf_subs_ready(void) {
  pthread_mutex_lock(&lock);
  const bool ready = track != NULL;
  pthread_mutex_unlock(&lock);
  return ready;
}

void jf_subs_feed(const char *line, int length, int64_t start_ms,
                  int64_t duration_ms) {
  if (line == NULL || length <= 0)
    return;
  pthread_mutex_lock(&lock);
  if (track != NULL)
    ass_process_chunk(track, (char *)(uintptr_t)line, length, start_ms,
                      duration_ms);
  pthread_mutex_unlock(&lock);
}

void jf_subs_flush(void) {
  pthread_mutex_lock(&lock);
  if (track != NULL)
    ass_flush_events(track);
  composited.w = 0;
  composited.h = 0;
  composed = false;
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

/* `over`, with the destination held premultiplied so overlapping runs - a glyph
 * on its own outline and shadow - accumulate correctly. Undone again at the
 * end, because the renderer's blend function is the straight-alpha one. */
static void blend(uint8_t *dst, int pitch, int origin_x, int origin_y,
                  const ASS_Image *it) {
  const uint8_t r = (uint8_t)(it->color >> 24);
  const uint8_t g = (uint8_t)(it->color >> 16);
  const uint8_t b = (uint8_t)(it->color >> 8);
  const unsigned opacity = 255u - (it->color & 0xff);
  if (opacity == 0)
    return;
  for (int y = 0; y < it->h; y++) {
    const uint8_t *source = it->bitmap + (size_t)y * it->stride;
    uint8_t *row = dst + (size_t)(it->dst_y - origin_y + y) * pitch +
                   (size_t)(it->dst_x - origin_x) * 4;
    for (int x = 0; x < it->w; x++, row += 4) {
      const unsigned k = source[x] * opacity / 255u;
      if (k == 0)
        continue;
      const unsigned keep = 255u - k;
      row[0] = (uint8_t)((r * k + row[0] * keep) / 255u);
      row[1] = (uint8_t)((g * k + row[1] * keep) / 255u);
      row[2] = (uint8_t)((b * k + row[2] * keep) / 255u);
      row[3] = (uint8_t)(k + row[3] * keep / 255u);
    }
  }
}

static void unpremultiply(uint8_t *pixels, size_t count) {
  for (size_t i = 0; i < count; i++, pixels += 4) {
    const unsigned a = pixels[3];
    if (a == 0 || a == 255)
      continue;
    for (int c = 0; c < 3; c++) {
      const unsigned value = pixels[c] * 255u / a;
      pixels[c] = (uint8_t)(value > 255u ? 255u : value);
    }
  }
}

bool jf_subs_frame(int64_t media_ms, jf_subs_image *out) {
  pthread_mutex_lock(&lock);
  bool changed = false;
  if (track == NULL || renderer == NULL)
    goto done;

  int detect = 0;
  ASS_Image *image = ass_render_frame(renderer, track, media_ms, &detect);
  if (detect == 0 && composed) {
    /* Nothing moved since the last call; whatever is on screen still stands. */
    goto done;
  }
  composed = true;
  changed = true;

  int x0, y0, x1, y1;
  bounds(image, &x0, &y0, &x1, &y1);
  if (x1 <= x0 || y1 <= y0) {
    composited.w = 0;
    composited.h = 0;
    goto done;
  }
  const int w = x1 - x0;
  const int h = y1 - y0;
  const size_t need = (size_t)w * (size_t)h * 4;
  if (need > canvas_capacity) {
    uint8_t *grown = realloc(canvas, need);
    if (grown == NULL) {
      composited.w = 0;
      composited.h = 0;
      goto done;
    }
    canvas = grown;
    canvas_capacity = need;
  }
  memset(canvas, 0, need);
  unsigned runs = 0;
  for (const ASS_Image *it = image; it != NULL; it = it->next) {
    if (it->w <= 0 || it->h <= 0)
      continue;
    /* One line per run the first time a track draws anything. Where a
     * multi-segment line runs its words together, this says which half is at
     * fault: libass laying the runs out butted up against each other, or the
     * blend below placing them that way. */
    if (!logged_runs && runs < 16)
      fprintf(stderr, "libass run %u: x=%d..%d y=%d colour=%08x\n", runs,
              it->dst_x, it->dst_x + it->w, it->dst_y, it->color);
    runs++;
    blend(canvas, w * 4, x0, y0, it);
  }
  if (!logged_runs) {
    logged_runs = true;
    fprintf(stderr, "libass: %u run(s) into %dx%d at %d,%d\n", runs, w, h, x0,
            y0);
  }
  unpremultiply(canvas, (size_t)w * (size_t)h);
  composited.x = x0;
  composited.y = y0;
  composited.w = w;
  composited.h = h;
  composited.rgba = canvas;

done:
  *out = composited;
  pthread_mutex_unlock(&lock);
  return changed;
}
