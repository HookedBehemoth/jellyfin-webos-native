#include "picsubs.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  int64_t start, end;
  float x, y, w, h; /* in the overlay */
  int texture_w, texture_h;
  int stream;
  uint8_t *rgba;
} picture;

/* Every bitmap track's, for the several seconds the demuxer reads ahead: a
 * handful each. A burst beyond this drops the latest. */
#define MAX_PICTURES 256

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool open;
static int selected = -1;
static picture pictures[MAX_PICTURES];
static size_t count;
/* Out of the list but still what the caller is drawing from. */
static uint8_t *shown_rgba;
static bool shown_valid;
static int frame_width, frame_height, picture_width, picture_height;

static void drop_all(void) {
  for (size_t i = 0; i < count; i++)
    if (pictures[i].rgba != shown_rgba)
      free(pictures[i].rgba);
  count = 0;
}

void jf_picsubs_open(int width, int height, int video_width, int video_height) {
  jf_picsubs_close();
  pthread_mutex_lock(&lock);
  frame_width = width;
  frame_height = height;
  picture_width = video_width > 0 ? video_width : width;
  picture_height = video_height > 0 ? video_height : height;
  open = true;
  pthread_mutex_unlock(&lock);
}

/* Where a canvas_w x canvas_h grid lands in the overlay. A canvas shaped like
 * the video covers its picture - a DVD's 720x480 is anamorphic, so the axes
 * scale apart. One shaped otherwise is the frame the disc was authored for
 * around a video since cropped (1920x1080 over a 2.39:1 encode), so it keeps
 * its own shape, centred as the crop was. Lock held. */
static void place(int canvas_w, int canvas_h, float *sx, float *sy, float *ox,
                  float *oy) {
  if (canvas_w <= 0 || canvas_h <= 0)
    canvas_w = picture_width, canvas_h = picture_height;
  const float canvas_aspect = (float)canvas_w / canvas_h;
  const float video_aspect = (float)picture_width / picture_height;
  const bool same = canvas_aspect > video_aspect * 0.97f &&
                    canvas_aspect < video_aspect * 1.03f;
  const float shape_w = same ? (float)picture_width : (float)canvas_w;
  const float shape_h = same ? (float)picture_height : (float)canvas_h;
  const float fit = (float)frame_width / shape_w < (float)frame_height / shape_h
                        ? (float)frame_width / shape_w
                        : (float)frame_height / shape_h;
  *sx = shape_w * fit / canvas_w;
  *sy = shape_h * fit / canvas_h;
  *ox = (frame_width - shape_w * fit) / 2;
  *oy = (frame_height - shape_h * fit) / 2;
}

void jf_picsubs_close(void) {
  pthread_mutex_lock(&lock);
  drop_all();
  free(shown_rgba);
  shown_rgba = NULL;
  shown_valid = false;
  open = false;
  selected = -1;
  pthread_mutex_unlock(&lock);
}

void jf_picsubs_select(int stream) {
  pthread_mutex_lock(&lock);
  selected = stream;
  pthread_mutex_unlock(&lock);
}

bool jf_picsubs_ready(void) {
  pthread_mutex_lock(&lock);
  const bool ready = open && selected >= 0;
  pthread_mutex_unlock(&lock);
  return ready;
}

void jf_picsubs_feed(int64_t start_ms, int64_t duration_ms, int x, int y, int w,
                     int h, int stream, int canvas_w, int canvas_h,
                     uint8_t *rgba) {
  pthread_mutex_lock(&lock);
  if (!open) {
    pthread_mutex_unlock(&lock);
    free(rgba);
    return;
  }
  /* Long over by the demuxer's clock: nothing draws them while subtitles are
   * off, so nothing else would let them go. */
  size_t kept = 0;
  for (size_t i = 0; i < count; i++) {
    if (pictures[i].end < start_ms - 30000 && pictures[i].rgba != shown_rgba) {
      free(pictures[i].rgba);
      continue;
    }
    pictures[kept++] = pictures[i];
  }
  count = kept;
  size_t at = 0;
  while (at < count && pictures[at].start <= start_ms) {
    if (pictures[at].start == start_ms && pictures[at].stream == stream) {
      /* Read again after a seek back. */
      pthread_mutex_unlock(&lock);
      free(rgba);
      return;
    }
    at++;
  }
  if (count == MAX_PICTURES) {
    pthread_mutex_unlock(&lock);
    free(rgba);
    return;
  }
  memmove(pictures + at + 1, pictures + at, (count - at) * sizeof(*pictures));
  count++;
  const int64_t end = duration_ms < 0 ? INT64_MAX : start_ms + duration_ms;
  float sx, sy, ox, oy;
  place(canvas_w, canvas_h, &sx, &sy, &ox, &oy);
  pictures[at] = (picture){start_ms, end, ox + x * sx, oy + y * sy, w * sx,
                           h * sy,   w,   h,           stream,      rgba};
  /* The track's next picture ends this one, whatever it said. */
  for (size_t i = at; i-- > 0;)
    if (pictures[i].stream == stream) {
      if (pictures[i].end > start_ms)
        pictures[i].end = start_ms;
      break;
    }
  for (size_t i = at + 1; i < count; i++)
    if (pictures[i].stream == stream) {
      if (pictures[at].end > pictures[i].start)
        pictures[at].end = pictures[i].start;
      break;
    }
  pthread_mutex_unlock(&lock);
}

bool jf_picsubs_frame(int64_t media_ms, jf_picsubs_image *out) {
  pthread_mutex_lock(&lock);
  /* Behind the playhead: gone. The one on screen is kept until replaced. */
  size_t kept = 0;
  const picture *now = NULL;
  for (size_t i = 0; i < count; i++) {
    if (pictures[i].end <= media_ms &&
        (pictures[i].rgba == NULL || pictures[i].rgba != shown_rgba)) {
      free(pictures[i].rgba);
      continue;
    }
    pictures[kept++] = pictures[i];
  }
  count = kept;
  for (size_t i = 0; i < count; i++)
    if (pictures[i].stream == selected && pictures[i].start <= media_ms &&
        media_ms < pictures[i].end)
      now = &pictures[i];
  uint8_t *rgba = now != NULL ? now->rgba : NULL;
  const bool changed = !shown_valid || rgba != shown_rgba;
  if (changed) {
    /* The last image is the caller's until now; free it if it left the list. */
    bool listed = false;
    for (size_t i = 0; i < count && !listed; i++)
      listed = pictures[i].rgba == shown_rgba;
    if (!listed)
      free(shown_rgba);
    shown_rgba = rgba;
    shown_valid = true;
  }
  *out = (jf_picsubs_image){0};
  if (now != NULL && now->rgba != NULL)
    *out = (jf_picsubs_image){now->x,         now->y,         now->w,   now->h,
                              now->texture_w, now->texture_h, now->rgba};
  pthread_mutex_unlock(&lock);
  return changed;
}
