/*
 * A Jellyfin client for the TV: discovery, sign-in, home rows, a virtual
 * library grid and the path down to a single episode.
 *
 * The UI is one instanced batch with rasterised glyphs, remote/pointer
 * handling, and virtual-list geometry. Every screen is backed by a real server,
 * so this file is mostly about keeping the render thread free of that: the
 * fetcher runs the requests and image decoding on worker threads, results wake
 * the event loop, and screen state is plain fixed-size storage that a task
 * result is copied into. Nothing the renderer touches is owned by a worker.
 *
 * Video decoding and presentation belong to LG's Starfish pipeline, fed from an
 * FFmpeg demuxer; audio is decoded here and written to ALSA. See jf/player.c.
 */
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "../platform/os.h"

#include "../apps/probe.h"
#include "../jf/api.h"
#include "../jf/cfg.h"
#include "../jf/picsubs.h"
#include "../jf/player.h"
#include "../jf/subs.h"
#include "../platform/gl.h"
#include "../platform/luna.h"
#include "../platform/window.h"
#include "../ui/font_index.h"
#include "../ui/glyph_atlas.h"
#include "../ui/loom.h"
#include "../ui/nav.h"
#include "../ui/renderer.h"
#include "licenses.h"
#include "text_fs.h"
#include "text_vs.h"

#define APP_ID "dev.hookedbehemoth.jellyfin"

static const loom_color BG = {9, 13, 22, 255};
static const loom_color PANEL = {20, 27, 40, 255};
static const loom_color CARD = {27, 36, 52, 255};
static const loom_color HOT = {38, 51, 70, 255};
static const loom_color SELECTED = {27, 69, 91, 255};
static const loom_color ACCENT = {0, 164, 220, 255};
static const loom_color RED = {228, 96, 96, 255};
static const loom_color TEXT = {239, 244, 250, 255};
static const loom_color DIM = {148, 163, 182, 255};
static const loom_color BORDER = {55, 70, 91, 255};
static const loom_color WHITE = {255, 255, 255, 255};
static const loom_color YELLOW = {255, 206, 64, 255};

static jf_renderer *renderer;
static jf_fetcher fetcher;
static jf_session session;

static uint64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Where the render thread's time goes, for the stats overlay: the worst frame
 * of each phase over the last second, in thread CPU time. The subtitle render
 * and composite are the background thread's cost for the frame shown. */
typedef enum {
  PHASE_UI,
  PHASE_ASS,
  PHASE_COMPOSITE,
  PHASE_UPLOAD,
  PHASE_DRAW,
  PHASE_COUNT
} phase;
static double phase_frame[PHASE_COUNT], phase_peak[PHASE_COUNT],
    phase_shown[PHASE_COUNT];
static unsigned runs_peak, runs_shown;

static double thread_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

typedef struct {
  float value, start, target;
  uint64_t started_at, duration;
} animated_float;

#define NAVIGATION_ANIMATION_NS 180000000ull
#define SIDEBAR_ANIMATION_NS 280000000ull

static bool animations_enabled = true;
static bool stats_overlay;
static char preferences_path[576];
static char ui_font[512];
static char audio_device[64] = "default";

/* How subtitles look. The plain-text renderer takes all of it; libass takes
 * the size, margin and side (see jf_subs_style). Colours are indices into
 * `swatches`, sizes in pixels at 1080 rows. */
typedef struct {
  const char *name;
  loom_color color;
} swatch;
static const swatch swatches[] = {
    {"White", {255, 255, 255, 255}},   {"Yellow", {255, 226, 64, 255}},
    {"Cyan", {96, 220, 255, 255}},     {"Green", {120, 232, 120, 255}},
    {"Magenta", {240, 112, 232, 255}}, {"Red", {240, 92, 92, 255}},
    {"Grey", {176, 176, 176, 255}},    {"Black", {0, 0, 0, 255}},
};
#define SWATCH_COUNT (int)(sizeof(swatches) / sizeof(swatches[0]))

static struct {
  int color, outline_color;
  bool outline;
  int outline_width;
  bool ignore_inline;
  int size_percent; /* added to 100 */
  int margin_percent;
  bool top;
  bool plain_ass;
  int search_language; /* index into search_languages */
  bool bold;           /* every line, whatever the script marks */
} sub_style = {0, 7, true, 3, false, 0, 5, false, false, 0, false};

static int clampi(int value, int lo, int hi) {
  return value < lo ? lo : value > hi ? hi : value;
}

static void apply_subtitle_style(void) {
  sub_style.color = clampi(sub_style.color, 0, SWATCH_COUNT - 1);
  sub_style.outline_color =
      clampi(sub_style.outline_color, 0, SWATCH_COUNT - 1);
  sub_style.outline_width = clampi(sub_style.outline_width, 1, 6);
  sub_style.size_percent = clampi(sub_style.size_percent, -50, 150);
  sub_style.margin_percent = clampi(sub_style.margin_percent, 0, 40);
  jf_subs_force_plain(sub_style.plain_ass);
  jf_subs_style(1 + sub_style.size_percent / 100.0,
                sub_style.margin_percent / 100.0, sub_style.top);
}

static bool is(const cfg_reader *reader, const char *section, const char *key) {
  return strcmp(reader->section, section) == 0 && strcmp(reader->key, key) == 0;
}

static void load_preferences(void) {
  jf_arena arena = {0};
  cfg_reader reader;
  if (!cfg_open(&reader, &arena, preferences_path))
    fprintf(stderr, "could not load preferences: %s\n", preferences_path);
  for (cfg_event event; (event = cfg_next(&reader)) != CFG_END;) {
    if (event != CFG_VALUE)
      continue;
    if (is(&reader, "ui", "animations"))
      animations_enabled = cfg_bool(&reader, animations_enabled);
    else if (is(&reader, "ui", "overlay"))
      stats_overlay = cfg_bool(&reader, stats_overlay);
    else if (is(&reader, "ui", "font"))
      snprintf(ui_font, sizeof(ui_font), "%s", cfg_text(&reader));
    else if (is(&reader, "playback", "audio"))
      jf_player_audio = cfg_bool(&reader, jf_player_audio);
    else if (is(&reader, "playback", "subtitles"))
      jf_player_subtitles = cfg_bool(&reader, jf_player_subtitles);
    else if (is(&reader, "playback", "audio_device"))
      snprintf(audio_device, sizeof(audio_device), "%s", cfg_text(&reader));
    else if (is(&reader, "subtitles", "color"))
      sub_style.color = cfg_int(&reader, sub_style.color);
    else if (is(&reader, "subtitles", "outline"))
      sub_style.outline = cfg_bool(&reader, sub_style.outline);
    else if (is(&reader, "subtitles", "outline_width"))
      sub_style.outline_width = cfg_int(&reader, sub_style.outline_width);
    else if (is(&reader, "subtitles", "outline_color"))
      sub_style.outline_color = cfg_int(&reader, sub_style.outline_color);
    else if (is(&reader, "subtitles", "ignore_inline_styles"))
      sub_style.ignore_inline = cfg_bool(&reader, sub_style.ignore_inline);
    else if (is(&reader, "subtitles", "bold"))
      sub_style.bold = cfg_bool(&reader, sub_style.bold);
    else if (is(&reader, "subtitles", "size_percent"))
      sub_style.size_percent = cfg_int(&reader, sub_style.size_percent);
    else if (is(&reader, "subtitles", "margin_percent"))
      sub_style.margin_percent = cfg_int(&reader, sub_style.margin_percent);
    else if (is(&reader, "subtitles", "top"))
      sub_style.top = cfg_bool(&reader, sub_style.top);
    else if (is(&reader, "subtitles", "search_language"))
      sub_style.search_language = cfg_int(&reader, 0);
    else if (is(&reader, "subtitles", "plain_ass"))
      sub_style.plain_ass = cfg_bool(&reader, sub_style.plain_ass);
    else if (is(&reader, "log", "keys"))
      jf_window_log_keys = cfg_bool(&reader, jf_window_log_keys);
    else if (is(&reader, "log", "window"))
      jf_window_log_events = cfg_bool(&reader, jf_window_log_events);
    else if (is(&reader, "log", "luna"))
      jf_luna_log = cfg_bool(&reader, jf_luna_log);
  }
  jf_arena_destroy(&arena);
  jf_atlas_font = ui_font[0] != '\0' ? ui_font : NULL;
  jf_subs_font = jf_atlas_font;
  jf_player_audio_device = audio_device;
  apply_subtitle_style();
}

static void save_preferences(void) {
  jf_arena arena = {0};
  cfg_writer writer;
  cfg_writer_init(&writer, &arena);
  cfg_section(&writer, "ui");
  cfg_write_bool(&writer, "animations", animations_enabled);
  cfg_comment(&writer,
              "frame times and instance counts, redrawing every frame");
  cfg_write_bool(&writer, "overlay", stats_overlay);
  cfg_comment(&writer, "a .ttf to draw the UI with; empty picks a system face");
  cfg_write_text(&writer, "font", ui_font);
  cfg_section(&writer, "playback");
  cfg_write_bool(&writer, "audio", jf_player_audio);
  cfg_write_bool(&writer, "subtitles", jf_player_subtitles);
  cfg_write_text(&writer, "audio_device", audio_device);
  cfg_section(&writer, "subtitles");
  cfg_comment(&writer,
              "colours: 0 white, 1 yellow, 2 cyan, 3 green, 4 magenta, "
              "5 red, 6 grey, 7 black");
  cfg_write_int(&writer, "color", sub_style.color);
  cfg_write_bool(&writer, "outline", sub_style.outline);
  cfg_write_int(&writer, "outline_width", sub_style.outline_width);
  cfg_write_int(&writer, "outline_color", sub_style.outline_color);
  cfg_write_bool(&writer, "ignore_inline_styles", sub_style.ignore_inline);
  cfg_write_bool(&writer, "bold", sub_style.bold);
  cfg_write_int(&writer, "size_percent", sub_style.size_percent);
  cfg_write_int(&writer, "margin_percent", sub_style.margin_percent);
  cfg_write_bool(&writer, "top", sub_style.top);
  cfg_comment(&writer, "draw styled ASS tracks as plain text too");
  cfg_write_bool(&writer, "plain_ass", sub_style.plain_ass);
  cfg_write_int(&writer, "search_language", sub_style.search_language);
  cfg_section(&writer, "log");
  cfg_write_bool(&writer, "keys", jf_window_log_keys);
  cfg_write_bool(&writer, "window", jf_window_log_events);
  cfg_write_bool(&writer, "luna", jf_luna_log);
  if (!cfg_flush(&writer, preferences_path))
    fprintf(stderr, "could not save preferences: %s\n", preferences_path);
  jf_arena_destroy(&arena);
}

static bool animated_float_running(const animated_float *value) {
  return value->started_at != 0;
}

static void animated_float_snap(animated_float *value, float target) {
  value->value = target;
  value->start = target;
  value->target = target;
  value->started_at = 0;
  value->duration = 0;
}

static void animated_float_update(animated_float *value, uint64_t now) {
  if (!animated_float_running(value))
    return;
  const float progress =
      value->duration == 0 ? 1.0f
                           : (float)(now - value->started_at) / value->duration;
  if (progress >= 1.0f) {
    animated_float_snap(value, value->target);
    return;
  }
  /* Ease-out exponential: quick feedback, with the last few pixels settling
   * softly. */
  const float eased = 1.0f - powf(2.0f, -10.0f * progress);
  value->value = value->start + (value->target - value->start) * eased;
}

static void animated_float_set(animated_float *value, float target,
                               uint64_t duration) {
  if (!animations_enabled || fabsf(value->target - target) < 0.01f) {
    if (!animations_enabled)
      animated_float_snap(value, target);
    return;
  }
  animated_float_update(value, now_ns());
  value->start = value->value;
  value->target = target;
  value->started_at = now_ns();
  value->duration = duration;
}

static uint64_t frame_index;

static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
/* Saturating decrement, which is what every "move focus back" does. */
static size_t dec(size_t value) { return value > 0 ? value - 1 : 0; }

static void set_text(char *out, size_t out_len, const char *value) {
  snprintf(out, out_len, "%s", value != NULL ? value : "");
}

/* Per-frame scratch for formatted labels. A draw command borrows the string
 * until the renderer has consumed it, so these must outlive the layout pass -
 * but only that, hence a ring reset every frame. Sized for a full grid page:
 * every card formats a title and a subtitle to fit its width. */
static char scratch[256][160];
static size_t scratch_used;

static const char *fmt(const char *pattern, ...) {
  if (scratch_used == sizeof(scratch) / sizeof(scratch[0]))
    return "...";
  char *out = scratch[scratch_used++];
  va_list args;
  va_start(args, pattern);
  vsnprintf(out, sizeof(scratch[0]), pattern, args);
  va_end(args);
  return out;
}

/* Formatting for text that is copied into fixed storage immediately, which is
 * every field of a card. Deliberately not the frame scratch: building cards
 * happens while draining tasks, before the frame resets, and one page of sixty
 * items would eat the ring the labels then need. */
static char build_buffer[256];

static const char *build(const char *pattern, ...) {
  va_list args;
  va_start(args, pattern);
  vsnprintf(build_buffer, sizeof(build_buffer), pattern, args);
  va_end(args);
  return build_buffer;
}

/* ------------------------------------------------------------- poster cache
 *
 * One GL texture per image, not an atlas. Artwork comes back at whatever size
 * the server chose (200x300, 204x300, 534x300 for a library tile), so an atlas
 * would mean cropping every image to a fixed tile and then re-uploading over
 * tiles that a scrolling grid may still be drawing from. A texture per poster
 * keeps each image at its own size, and the batcher already handles the cost:
 * one draw call per distinct texture on screen. */

#define POSTER_W 240
#define POSTER_H 360
/* Enough for a full grid page plus the rows behind it. Each is ~260 KB. */
#define CACHE_SIZE 48

typedef struct {
  char id[40];
  char tag[40];
  jf_image_kind kind;
  uint32_t requested_width, requested_height;
  uint32_t texture;
  uint32_t width, height;
  /* Frame this slot was last asked for. 0 means never. */
  uint64_t used;
  bool loading;
} poster_slot;

static poster_slot slots[CACHE_SIZE];
/* Poster requests started this frame. Scrolling a grid can name thirty new
 * posters in one frame; the fetcher has 32 slots shared with page requests, so
 * let the visible ones trickle in over a few frames instead of starving it. */
static unsigned poster_requests;

static bool artwork_loading(const char *id, const char *tag, jf_image_kind kind,
                            uint32_t width, uint32_t height) {
  for (size_t i = 0; i < CACHE_SIZE; i++) {
    const poster_slot *slot = &slots[i];
    if (strcmp(slot->id, id != NULL ? id : "") == 0 &&
        strcmp(slot->tag, tag != NULL ? tag : "") == 0 && slot->kind == kind &&
        slot->requested_width == width && slot->requested_height == height)
      return slot->loading;
  }
  return false;
}

/* The cached artwork for `id`, or NULL while it is still being fetched. Calling
 * this is also what keeps a slot alive, so it must be called every frame for
 * every visible card. */
static const poster_slot *artwork(const char *id, const char *tag,
                                  jf_image_kind kind, uint32_t width,
                                  uint32_t height) {
  if (id == NULL || id[0] == '\0')
    return NULL;
  if (tag == NULL)
    tag = "";
  for (size_t i = 0; i < CACHE_SIZE; i++) {
    poster_slot *slot = &slots[i];
    if (strcmp(slot->id, id) != 0 || strcmp(slot->tag, tag) != 0)
      continue;
    if (slot->kind != kind || slot->requested_width != width ||
        slot->requested_height != height)
      continue;
    slot->used = frame_index;
    return slot->texture != 0 ? slot : NULL;
  }
  if (poster_requests >= 4 || jf_fetcher_pending(&fetcher) >= 24)
    return NULL;

  poster_slot *victim = NULL;
  uint64_t oldest = UINT64_MAX;
  for (size_t i = 0; i < CACHE_SIZE; i++) {
    poster_slot *slot = &slots[i];
    /* Never evict something drawn this frame or the one before: that is a slot
     * the screen is using right now, and recycling it would thrash. */
    if (slot->loading || (slot->used != 0 && slot->used + 1 >= frame_index))
      continue;
    if (slot->used < oldest) {
      oldest = slot->used;
      victim = slot;
    }
  }
  if (victim == NULL) {
    /* A navigation frame may still protect the previous screen's slots. Advance
     * that grace period once, even with no requests in flight. */
    for (size_t i = 0; i < CACHE_SIZE; i++)
      if (!slots[i].loading && slots[i].used < frame_index) {
        jf_window_frame_requested = true;
        break;
      }
    return NULL;
  }

  const uint32_t index = (uint32_t)(victim - slots);
  jf_task *task = jf_fetcher_submit(&fetcher, JF_JOB_POSTER, index);
  if (task == NULL)
    return NULL;
  set_text(task->a, sizeof(task->a), id);
  set_text(task->b, sizeof(task->b), tag);
  task->start = width;
  task->limit = height;
  task->image_kind = kind;

  jf_renderer_destroy_texture(renderer, victim->texture);
  memset(victim, 0, sizeof(*victim));
  victim->used = frame_index;
  victim->loading = true;
  victim->kind = kind;
  victim->requested_width = width;
  victim->requested_height = height;
  set_text(victim->id, sizeof(victim->id), id);
  set_text(victim->tag, sizeof(victim->tag), tag);
  jf_fetcher_start(&fetcher, task);
  poster_requests++;
  return NULL;
}

static const poster_slot *poster(const char *id, const char *tag) {
  return artwork(id, tag, JF_IMAGE_PRIMARY, POSTER_W, POSTER_H);
}

/* The renderer's primitive is a rounded rectangle, so an eight-dot spinner is
 * cheaper than a separate texture or shader program. Its highlighted dot
 * advances with time. */
static void draw_spinner(loom_context *ctx, float x, float y, float radius,
                         const loom_rect *clip, float scale) {
  jf_window_frame_requested = true;
  const unsigned phase = (unsigned)(now_ns() / 90000000ull) % 8;
  for (unsigned i = 0; i < 8; i++) {
    const float angle = ((float)i / 8.0f) * 6.2831853f - 1.5707963f;
    const float dot = 7.0f * scale;
    loom_color color = {DIM[0], DIM[1], DIM[2], i == phase ? 255 : 72};
    loom_fill(ctx,
              (loom_rect){x + cosf(angle) * radius - dot / 2,
                          y + sinf(angle) * radius - dot / 2, dot, dot},
              clip, color, dot / 2);
  }
}

/* Texture coordinates that fill `rect` with the image, cropping the long axis
 * instead of stretching it. A 534x300 library banner and a 200x300 poster then
 * look right in the same portrait card. */
static void cover_uv(const poster_slot *slot, loom_rect rect, float out[4]) {
  out[0] = 0;
  out[1] = 0;
  out[2] = 1;
  out[3] = 1;
  if (slot->width == 0 || slot->height == 0 || rect.w <= 0 || rect.h <= 0)
    return;
  const float want = rect.w / rect.h;
  const float have = (float)slot->width / (float)slot->height;
  if (have > want) {
    const float keep = want / have;
    out[0] = (1 - keep) / 2;
    out[2] = (1 + keep) / 2;
  } else {
    const float keep = have / want;
    out[1] = (1 - keep) / 2;
    out[3] = (1 + keep) / 2;
  }
}

/* --------------------------------------------------------------- card model
 *
 * A task's result lives in that task's arena and dies when the UI releases it,
 * so screens keep their own copy. Fixed-size text means the copy is a memcpy
 * into storage that never moves - which is also what the renderer needs, since
 * a draw command holds a pointer, not a string. */

typedef struct {
  char id[40];
  char poster_id[40];
  char poster_tag[40];
  char series_id[40];
  char thumbnail_tag[40];
  char backdrop_id[40];
  char backdrop_tag[40];
  /* A cell of the blurhash atlas, valid while blur_rows_stamp[blur_row]
   * matches. */
  uint32_t blur_stamp;
  uint16_t blur_row, blur_column;
  uint8_t blur_width, blur_height;
  char title[96];
  char episode_title[128];
  char overview[1024];
  char rating[32];
  char official_rating[16];
  char subtitle[72];
  char kind[16];
  char runtime[24];
  float progress;
  uint64_t playback_position_ticks;
  bool played;
  uint32_t remaining;
  bool has_remaining;
  bool present;
} card;

static bool card_is(const card *c, const char *kind) {
  return strcmp(c->kind, kind) == 0;
}

static uint32_t date_year(const char *date) {
  if (date == NULL || strlen(date) < 4)
    return 0;
  char digits[5] = {date[0], date[1], date[2], date[3], '\0'};
  for (int i = 0; i < 4; i++)
    if (digits[i] < '0' || digits[i] > '9')
      return 0;
  return (uint32_t)atoi(digits);
}

/* One line under a card: what distinguishes this item from its neighbours. */
static const char *subtitle_for(const jf_item *item) {
  const char *type = item->type != NULL ? item->type : "";
  if (strcmp(type, "Episode") == 0)
    return build("S%uE%u  %s", item->parent_index_number, item->index_number,
                 item->name != NULL ? item->name : "");
  if (strcmp(type, "CollectionFolder") == 0)
    return item->collection_type != NULL ? item->collection_type : "Library";
  if (strcmp(type, "Series") == 0) {
    uint32_t start = item->production_year;
    if (start == 0)
      start = date_year(item->premiere_date);
    if (start == 0)
      return "";
    const uint32_t end = date_year(item->end_date);
    if (end > start)
      return build("%u-%u", start, end);
    if (end == 0 && item->status != NULL &&
        strcmp(item->status, "Continuing") == 0)
      return build("%u-Present", start);
    return build("%u", start);
  }
  if (strcmp(type, "Season") == 0)
    return build("%u episodes", item->child_count);
  const uint32_t minutes = jf_item_minutes(item);
  if (item->run_time_ticks != 0 && minutes != 0) {
    if (item->production_year != 0)
      return build("%u  -  %uh %02um", item->production_year, minutes / 60,
                   minutes % 60);
    return build("%uh %02um", minutes / 60, minutes % 60);
  }
  return item->collection_type != NULL ? item->collection_type : type;
}

/* Jellyfin overviews carry a little HTML. Only <br> means anything to a
 * renderer that cannot lay out markup, so it becomes a newline and every other
 * tag is left alone. */
static void set_overview(char *out, size_t out_len, const char *text) {
  size_t len = 0;
  for (size_t at = 0; text[at] != '\0' && len + 1 < out_len;) {
    if (text[at] == '<') {
      const char *close = strchr(text + at, '>');
      if (close != NULL) {
        char tag[16] = {0};
        size_t n = 0;
        for (const char *p = text + at + 1; p < close && n + 1 < sizeof(tag);
             p++)
          if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '/')
            tag[n++] = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        if (strcmp(tag, "br") == 0) {
          out[len++] = '\n';
          at = (size_t)(close - text) + 1;
          continue;
        }
      }
    }
    out[len++] = text[at++];
  }
  out[len] = '\0';
}

/* Blurhashes stand in for artwork that is still loading. Each result page's
 * arrive decoded as whole atlas rows; pages take the next free rows and
 * overwrite the oldest once the atlas is full, which a card notices by its
 * stamp. */
#define BLUR_ATLAS_ROWS 32
static uint32_t blur_atlas;
static uint32_t blur_rows_stamp[BLUR_ATLAS_ROWS];
static uint32_t blur_next_row, blur_stamps;
/* Where the page consume() is making cards from went. */
static struct {
  uint32_t stamp, row;
} incoming_blur;

static void upload_blurs(const jf_task *task) {
  incoming_blur.stamp = 0;
  if (task->blur == NULL || task->blur_rows > BLUR_ATLAS_ROWS)
    return;
  const uint32_t width = JF_BLUR_COLUMNS * JF_BLUR_CELL;
  if (blur_atlas == 0) {
    uint8_t *black = calloc((size_t)width * BLUR_ATLAS_ROWS * JF_BLUR_CELL, 3);
    if (black == NULL)
      return;
    blur_atlas = jf_renderer_create_texture(
        renderer, width, BLUR_ATLAS_ROWS * JF_BLUR_CELL, black);
    free(black);
  }
  if (blur_next_row + task->blur_rows > BLUR_ATLAS_ROWS)
    blur_next_row = 0;
  incoming_blur.stamp = ++blur_stamps;
  incoming_blur.row = blur_next_row;
  for (uint32_t i = 0; i < task->blur_rows; i++)
    blur_rows_stamp[blur_next_row + i] = incoming_blur.stamp;
  jf_renderer_update_texture(renderer, blur_atlas, 0,
                             blur_next_row * JF_BLUR_CELL, width,
                             task->blur_rows * JF_BLUR_CELL, task->blur);
  blur_next_row += task->blur_rows;
}

static card card_from(const jf_item *item) {
  card out;
  memset(&out, 0, sizeof(out));
  out.present = true;
  out.progress = jf_item_progress(item);
  out.playback_position_ticks = item->playback_position_ticks;
  out.played = jf_item_finished(item);
  const char *type = item->type != NULL ? item->type : "";

  if (strcmp(type, "Season") == 0 || strcmp(type, "Series") == 0) {
    out.has_remaining = item->has_unplayed_item_count;
    out.remaining = out.played ? 0 : item->unplayed_item_count;
  }
  set_text(out.id, sizeof(out.id), item->id);
  set_text(out.poster_id, sizeof(out.poster_id), jf_item_poster_id(item));
  set_text(out.poster_tag, sizeof(out.poster_tag), jf_item_poster_tag(item));
  set_text(out.series_id, sizeof(out.series_id), item->series_id);
  set_text(out.thumbnail_tag, sizeof(out.thumbnail_tag),
           item->primary_image_tag);
  if (incoming_blur.stamp != 0 && item->blur_width != 0) {
    out.blur_stamp = incoming_blur.stamp;
    out.blur_row =
        (uint16_t)(incoming_blur.row + item->blur_cell / JF_BLUR_COLUMNS);
    out.blur_column = (uint16_t)(item->blur_cell % JF_BLUR_COLUMNS);
    out.blur_width = item->blur_width;
    out.blur_height = item->blur_height;
  }
  if (item->backdrop_image_tag != NULL) {
    set_text(out.backdrop_id, sizeof(out.backdrop_id), item->id);
    set_text(out.backdrop_tag, sizeof(out.backdrop_tag),
             item->backdrop_image_tag);
  } else if (item->parent_backdrop_image_tag != NULL) {
    set_text(out.backdrop_id, sizeof(out.backdrop_id),
             item->parent_backdrop_item_id);
    set_text(out.backdrop_tag, sizeof(out.backdrop_tag),
             item->parent_backdrop_image_tag);
  }
  set_overview(out.overview, sizeof(out.overview),
               item->overview != NULL ? item->overview : "");

  const char *name = item->name != NULL ? item->name : "";
  if (item->index_number != 0)
    set_text(out.episode_title, sizeof(out.episode_title),
             build("%u. %s", item->index_number, name));
  else
    set_text(out.episode_title, sizeof(out.episode_title), name);
  if (item->community_rating != 0)
    set_text(out.rating, sizeof(out.rating),
             build("%.1f", item->community_rating));
  set_text(out.official_rating, sizeof(out.official_rating),
           item->official_rating);
  set_text(out.kind, sizeof(out.kind), type);
  set_text(out.title, sizeof(out.title),
           strcmp(type, "Episode") == 0 && item->series_name != NULL
               ? item->series_name
               : name);
  /* Each build() reuses one buffer, so every result is copied into the card
   * before the next call. */
  set_text(out.subtitle, sizeof(out.subtitle), subtitle_for(item));
  if (item->run_time_ticks != 0) {
    const uint32_t minutes = jf_item_minutes(item);
    const char *text;
    if (minutes >= 60)
      text = minutes % 60 == 0
                 ? build("%u hr", minutes / 60)
                 : build("%u hr %u min", minutes / 60, minutes % 60);
    else if (minutes > 0)
      text = build("%u min", minutes);
    else
      text = build("%llu sec",
                   (unsigned long long)(item->run_time_ticks / 10000000ull));
    set_text(out.runtime, sizeof(out.runtime), text);
  }
  return out;
}

static size_t first_unfinished(const card *cards, size_t count) {
  for (size_t i = 0; i < count; i++)
    if (!cards[i].played)
      return i;
  return 0;
}

/* ---------------------------------------------------------------- app state */

typedef enum {
  SCREEN_SERVER,
  SCREEN_AUTH,
  SCREEN_QUICK,
  SCREEN_HOME,
  SCREEN_GRID,
  SCREEN_DETAILS,
  SCREEN_SEASON,
  SCREEN_PLAYBACK,
  SCREEN_SETTINGS,
  SCREEN_LICENSES,
  SCREEN_LICENSE,
} screen_id;

typedef enum { EDIT_NONE, EDIT_URL, EDIT_USERNAME, EDIT_PASSWORD } edit_field;

#define ROW_CAPACITY 24
#define CATEGORY_CAPACITY 256
#define SEASONS_CAPACITY 128
#define EPISODES_CAPACITY 512
/* Libraries, Continue Watching, Next Up, and Recently Added for each library.
 */
#define LATEST_ROWS 8
#define ROW_COUNT (3 + LATEST_ROWS)

typedef struct {
  const char *title;
  /* Libraries are 16:9 banners on the server, not posters. Cropping one to a
   * portrait card cuts the library's name out of the middle of it. */
  bool wide;
  card *cards;
  size_t capacity;
  size_t count;
  bool loading;
} item_row;

/* Every card showing the item, as the server now has it. */
static void apply_user_data(const jf_user_data *item);

static void row_fill(item_row *row, const jf_item *items, size_t count) {
  row->count = min_size(count, row->capacity);
  for (size_t i = 0; i < row->count; i++)
    row->cards[i] = card_from(&items[i]);
  row->loading = false;
}

/* The server's own home screen, top to bottom; the focus still starts on what
 * a returning user wants, the first row to continue from. */
enum { ROW_LIBRARIES, ROW_RESUME, ROW_NEXT_UP, ROW_LATEST };
static card home_cards[ROW_COUNT][ROW_CAPACITY];
static item_row rows[ROW_COUNT] = {
    {"Libraries", true, NULL, ROW_CAPACITY, 0, false},
    {"Continue Watching", false, NULL, ROW_CAPACITY, 0, false},
    {"Next Up", false, NULL, ROW_CAPACITY, 0, false},
};
/* Each Recently Added row's title, and the library it is for. */
static char latest_titles[LATEST_ROWS][96];
static char latest_parents[LATEST_ROWS][40];
static card category_cards[CATEGORY_CAPACITY];
static item_row categories_row = {"All categories",  true, NULL,
                                  CATEGORY_CAPACITY, 0,    false};

/* Back cancels the UI's interest in pending sign-in / Quick Connect replies. */
static uint32_t auth_generation;

static screen_id screen = SCREEN_SERVER;
/* Focus and scrolling, for every screen and overlay. */
static nav ui;

/* Where Back goes, as a stack rather than a rule per screen.
 *
 * The rule version was wrong in the ordinary case: reaching a show from a home
 * row and pressing Back returned to whichever library grid happened to be
 * loaded, because "is a grid loaded" is not the same question as "is that where
 * I came from". An entry carries enough to re-enter a screen, not a snapshot of
 * it - re-entering refetches, which is what keeps a details screen current
 * after playback marks something watched. */
typedef struct {
  screen_id screen;
  /* The grid's library, the details item, or the season. */
  card card;
  /* The series a season belongs to, so a season can be re-entered. */
  card series;
  nav_id nav_focus;
} stack_entry;

static stack_entry stack[12];
static size_t depth;

/* Third-party licences. The list is static; the text of one is split into lines
 * when it is opened, so only the licence being read costs anything, and the
 * virtual list draws the handful of lines that are on screen. */
static size_t license_open = JF_LICENSE_COUNT;
static char *license_body;
static const char **license_lines;
static size_t license_line_count;
/* Width of the longest line at a font size of one, measured once when the
 * licence is opened. The panel is then sized to the text rather than to the
 * screen. */
static float license_text_width;
/* Set by the draw, used by the key handler: one press moves one line of
 * whatever size that screen turned out to be. */

/* The composited subtitle overlay, uploaded only when libass says it changed.
 */
static uint32_t subtitle_texture;
static loom_rect subtitle_rect;
static uint64_t subtitle_tick;

/* Discovery / manual server entry. */
#define DISCOVERED_CAPACITY 8
static struct {
  char name[64];
  char address[128];
} discovered[DISCOVERED_CAPACITY];
static size_t discovered_count;
static bool discovering;
static uint64_t discovery_until;
static bool connecting;
static char server_name[512];

/* Quick Connect. */
static char quick_code[16];
static char quick_secret[80];
static uint64_t quick_poll_at;

/* Library grid: a window of a much longer server-side list. */
#define PAGE_SIZE 60
#define WINDOW_PAGES 4
#define WINDOW_SIZE (PAGE_SIZE * WINDOW_PAGES)
static char grid_parent[40];
static char grid_title[96];
static uint32_t grid_total;
static bool grid_loading;
static card grid_cards[WINDOW_SIZE];
/* Absolute item index currently stored in each window entry. Without it a stale
 * card from a page that scrolled away would be drawn as if it were the item
 * that now occupies the same ring slot. */
static uint32_t grid_at[WINDOW_SIZE];
static uint32_t grid_requested[WINDOW_PAGES];

/* Details and season screens. */
static card detail;
/* The details item's video, and who would decode it. */
static jf_video detail_video;
static bool detail_has_video;
static jf_decoders decoders;
static jf_server_transcode server_transcode;

/* The probe, and 10-bit once the TV has said it shows HDR. */
static const jf_decoders *tv_decoders(void) {
  if (jf_luna_hdr() == 1)
    decoders.deep = true;
  return &decoders;
}
/* Backdrops live longer than a details screen. Track their transition here
 * rather than on the cache slot, so opening another item also fades an
 * already-resident texture in, while moving between a series and its seasons
 * keeps the backdrop that is already showing. */
static uint32_t backdrop_texture;
static uint64_t backdrop_fade_started_at;
static card seasons_cards[SEASONS_CAPACITY];
static card episodes_cards[EPISODES_CAPACITY];
static item_row seasons_row = {"Seasons",        false, NULL,
                               SEASONS_CAPACITY, 0,     false};
static item_row episodes_row = {"Episodes",        false, NULL,
                                EPISODES_CAPACITY, 0,     false};
static card season_detail;
static char playback_title[160];
static char playback_item_id[40];
static char playback_source_id[40];
/* The episode's series, "" for anything else. */
static char playback_series_id[48];
/* The episodes either side of the one playing, from the server; `present`
 * false when there is none, or the item is not an episode. */
static card previous_episode, next_episode;
/* The subtitle picker, over the video so a choice shows on the frame behind
 * it. Row 0 is off, row i + 1 the player's track i. */
static bool subtitle_picker;
/* Where the focus was when the picker opened. */
static nav_id picker_return_focus;
/* The picker's first frame puts the focus on the current track: its rows do
 * not exist until then, and nav falls back off a focus nothing declares. */
static bool picker_focus_pending;
/* The appearance panel, which right on a plain-text track in the picker
 * opens. It stands in for the picker, over the picture, so every change shows
 * on the line it changes. */
static bool subtitle_settings;
/* Per player track: whether it is text, and ASS the script styles; and what
 * its delay is saved under. */
#define KNOWN_TRACKS 32
static bool track_text[KNOWN_TRACKS], track_styled[KNOWN_TRACKS],
    track_external[KNOWN_TRACKS];
static char track_key[KNOWN_TRACKS][80];
/* And what choosing it is remembered as for the series (see series_choices):
 * language and title, kept to what a JSON string takes as it is. */
static char track_choice[KNOWN_TRACKS][48];
/* The same for the audio, with the server's index for each; and the index
 * still to select, once the player has opened the source. */
static char audio_choice[KNOWN_TRACKS][48];
static int audio_index[KNOWN_TRACKS], audio_known, audio_wanted = -1;
/* Milliseconds a track is shown late by: tracks cut for another release are
 * off by their own amount. Kept on the server as this client's display
 * preferences (JF_JOB_DELAYS), so they follow the user from TV to TV; read
 * with the home rows, written when the panel closes on a change. */
#define SAVED_DELAYS 256
static struct {
  char key[80];
  int ms;
} saved_delays[SAVED_DELAYS];
static size_t saved_delay_count;
static bool delays_asked, delays_changed;
/* The subtitle and audio last chosen in each series, "-" for no subtitle. The
 * server remembers a choice for that one episode, and otherwise goes by the
 * release's default flags, so a show switches language from one episode to the
 * next. Saved with the delays, under "sub:<series id>" and "audio:<series
 * id>". */
#define SERIES_CHOICES 128
static struct {
  char key[56];
  char choice[48];
} series_choices[SERIES_CHOICES];
static size_t series_choice_count;
/* Deleting takes a second OK on the row, which this remembers. */
static bool delete_armed;
/* The list refreshed after a delete, which shows the track gone and none
 * selected. */
static bool awaiting_delete;
/* The subtitle search panel (see open_subtitle_search). */
#define SEARCH_RESULTS 48

/* A download, from the request to a track the player can read. The server
 * saves the file at once but lists it only once a background refresh of the
 * item gets there, so the list is asked for again until it has it; then the
 * file is fetched as the player would, and only then is it selected. */
typedef enum {
  DOWNLOAD_NONE,
  DOWNLOAD_FETCHING, /* the server getting it from the provider */
  DOWNLOAD_LISTING,  /* waiting for the server to list it */
  DOWNLOAD_CHECKING, /* reading it back */
  DOWNLOAD_SELECTING /* the list that selects it, on its way */
} download_phase;
#define DOWNLOAD_POLL_NS 1000000000ull
#define DOWNLOAD_POLLS 30
/* PLAYBACK_INFO's tag for a look at the list that changes nothing. */
#define DOWNLOAD_POLL_TAG 1

static struct {
  bool open, searching;
  download_phase phase;
  uint64_t poll_at;
  unsigned polls;
  bool poll_in_flight;
  /* The tracks before the download, so the new one can be told apart. */
  char before[KNOWN_TRACKS][80];
  int before_count;
  char error[160];
  bool error_is_warning; /* yellow, a thing to do, rather than red */
  size_t count;
  struct {
    char id[256], name[160], detail[96];
  } results[SEARCH_RESULTS];
} search;

/* The subtitle sources panel: every provider the server's repositories
 * offer, installed or not, each installable and switchable. Changes take
 * effect when the server restarts, which a row offers once one is pending. */
#define SOURCES 16
static struct {
  bool open, loading, loaded, restart_armed;
  /* Switched this session: turning one off waits for a restart too, though
   * the server calls it plain Disabled. */
  bool changed;
  bool adding; /* the repository being added */
  char note[160];
  bool note_is_error;
  /* A package being installed, looked for in the list until it shows. */
  char installing[96]; /* a name, as items' */
  uint64_t poll_at;
  unsigned polls;
  size_t count;
  struct {
    char name[96], guid[64], version[32], status[24];
    bool busy;
  } items[SOURCES];
} sources;
static void search_failed(const jf_task *task);
static void sources_failed(const jf_task *task);
static void sources_done(const jf_task *task);
static void poll_sources(void);
static void search_done(const jf_task *task);
static void search_listed(const jf_playback_info *info);
static void poll_download(void);
static nav_id picker_row_id(size_t row);
static size_t track_row(int track);
static bool playback_paused;
static screen_id playback_return_screen;
static nav_id playback_return_focus;
static uint64_t playback_started_at;
static uint64_t playback_progress_at;
/* The player chrome is deliberately transient: it never covers a scene for more
 * than three seconds unless playback is paused. */
static uint64_t playback_controls_until;
#define PLAYBACK_CONTROLS_NS (3ull * 1000000000ull)
/* Hidden with Up from the seek bar, until the next key or pointer movement. */
static bool chrome_dismissed;
/* How far the chrome is in, 0 to 1, and where the focus glow is. */
static animated_float chrome_motion, glow_x, glow_y;
static bool glow_placed;

static bool playback_chrome_wanted(uint64_t now) {
  return !chrome_dismissed &&
         (playback_paused || now < playback_controls_until);
}

static void show_playback_chrome(void) {
  chrome_dismissed = false;
  playback_controls_until = now_ns() + PLAYBACK_CONTROLS_NS;
}
/* evdev's KEY_SUBTITLE: F10 on a keyboard, Yellow on the remote. */
#define KEY_SUBTITLE 370

/* Text entry, remote, and pointer input. */
static char server_url[512];
static char username[256];
static char password[256];
static char password_mask[257];
static edit_field active_field = EDIT_NONE;
static bool capture_requested;

/* The sidebar is an overlay containing every library category and the one
 * settings row. */
static bool sidebar_open;
static bool sidebar_closing;
static animated_float sidebar_slide;
static screen_id sidebar_return_screen;
/* Where the focus was on the screen the sidebar opened over. */
static nav_id sidebar_return_focus;

static char status_text[200];
static bool status_error;

static void set_status(const char *pattern, ...) {
  jf_window_frame_requested = true;
  va_list args;
  va_start(args, pattern);
  vsnprintf(status_text, sizeof(status_text), pattern, args);
  va_end(args);
  status_error = false;
}

static void set_error(const char *pattern, ...) {
  jf_window_frame_requested = true;
  va_list args;
  va_start(args, pattern);
  vsnprintf(status_text, sizeof(status_text), pattern, args);
  va_end(args);
  status_error = true;
}

/* --------------------------------------------------------------- requests */

static bool is_auth_job(jf_job job) {
  return job == JF_JOB_PROBE || job == JF_JOB_LOGIN ||
         job == JF_JOB_QUICK_INITIATE || job == JF_JOB_QUICK_POLL ||
         job == JF_JOB_QUICK_AUTHENTICATE;
}

static jf_task *request(jf_job job, uint32_t tag) {
  return jf_fetcher_submit(&fetcher, job,
                           is_auth_job(job) ? auth_generation : tag);
}

static void simple(jf_job job) {
  jf_task *task = request(job, 0);
  if (task != NULL)
    jf_fetcher_start(&fetcher, task);
}

/* Also a refresh: rows keep what they show until the answer replaces it. */
static void load_home(void) {
  for (size_t i = 0; i < ROW_LATEST; i++)
    rows[i].loading = true;
  categories_row.loading = true;
  simple(JF_JOB_RESUME);
  simple(JF_JOB_NEXT_UP);
  simple(JF_JOB_VIEWS);
  simple(JF_JOB_ENCODING);
  /* Before any playback: the series' subtitle choices pick its first track. */
  if (!delays_asked)
    delays_asked = (simple(JF_JOB_DELAYS), true);
}

static void restart_discovery(void) {
  discovered_count = 0;
  discovering = true;
  discovery_until = now_ns() + 30000000000ull;
  simple(JF_JOB_DISCOVER);
}

/* Ask for the page containing `index`, unless that page is already in the
 * window or already in flight. */
static void request_page(uint32_t index) {
  const uint32_t page = index / PAGE_SIZE;
  const uint32_t ring = page % WINDOW_PAGES;
  if (grid_requested[ring] == page)
    return;
  jf_task *task = request(JF_JOB_CHILDREN, page);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), grid_parent);
  task->start = page * PAGE_SIZE;
  task->limit = PAGE_SIZE;
  grid_requested[ring] = page;
  jf_fetcher_start(&fetcher, task);
}

/* Opening a list afresh starts it at the top; Back into one keeps its place. */
static void forget_scroll(const char *id) { nav_forget(&ui, nav_str(id)); }

static void open_grid(const card *source) {
  set_text(grid_parent, sizeof(grid_parent), source->id);
  set_text(grid_title, sizeof(grid_title), source->title);
  grid_total = 0;
  grid_loading = true;
  memset(grid_cards, 0, sizeof(grid_cards));
  for (size_t i = 0; i < WINDOW_SIZE; i++)
    grid_at[i] = UINT32_MAX;
  for (size_t i = 0; i < WINDOW_PAGES; i++)
    grid_requested[i] = UINT32_MAX;
  screen = SCREEN_GRID;
  request_page(0);
}

static void open_details(const card *source) {
  if (strcmp(detail.backdrop_id, source->backdrop_id) != 0) {
    backdrop_texture = 0;
    backdrop_fade_started_at = 0;
  }
  detail = *source;
  detail_has_video = false;
  seasons_row.count = 0;
  seasons_row.loading = false;
  screen = SCREEN_DETAILS;

  /* A series card from a list already says everything this screen shows, and
   * its seasons bring the backdrop the list left out. An episode or movie needs
   * the item itself for its video stream. */
  const bool series = card_is(&detail, "Series");
  seasons_row.loading = series;
  jf_task *task = request(series ? JF_JOB_SEASONS : JF_JOB_ITEM, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), detail.id);
  jf_fetcher_start(&fetcher, task);
}

static void open_season(const card *source) {
  season_detail = *source;
  episodes_row.count = 0;
  episodes_row.loading = true;
  screen = SCREEN_SEASON;
  jf_task *task = request(JF_JOB_EPISODES, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a),
           season_detail.series_id[0] != '\0' ? season_detail.series_id
                                              : detail.id);
  set_text(task->b, sizeof(task->b), season_detail.id);
  jf_fetcher_start(&fetcher, task);
}

/* The library a grid belongs to, rebuilt from what the grid kept. */
static card library_card(void) {
  card out;
  memset(&out, 0, sizeof(out));
  out.present = true;
  set_text(out.id, sizeof(out.id), grid_parent);
  set_text(out.title, sizeof(out.title), grid_title);
  return out;
}

static stack_entry here(void) {
  stack_entry entry;
  memset(&entry, 0, sizeof(entry));
  entry.screen = screen;
  entry.card = screen == SCREEN_GRID     ? library_card()
               : screen == SCREEN_SEASON ? season_detail
                                         : detail;
  entry.series = detail;
  entry.nav_focus = ui.focus;
  return entry;
}

static void push(void) {
  if (depth == sizeof(stack) / sizeof(stack[0])) {
    /* Deeper than any real path through this app; drop the oldest rather than
     * refuse to navigate. */
    memmove(&stack[0], &stack[1], (depth - 1) * sizeof(stack[0]));
    depth--;
  }
  stack[depth++] = here();
}

static void pop(void) {
  if (depth == 0) {
    jf_window_running = false;
    return;
  }
  const stack_entry entry = stack[--depth];
  nav_focus(&ui, entry.nav_focus);
  switch (entry.screen) {
  case SCREEN_HOME:
    screen = SCREEN_HOME;
    break;
  case SCREEN_GRID:
    /* Returning to the grid we are still holding pages for is free; a different
     * library has to be fetched again. */
    if (strcmp(grid_parent, entry.card.id) == 0 && grid_total != 0)
      screen = SCREEN_GRID;
    else
      open_grid(&entry.card);
    break;
  case SCREEN_DETAILS:
    open_details(&entry.card);
    break;
  case SCREEN_SEASON:
    detail = entry.series;
    open_season(&entry.card);
    break;
  default:
    /* Nothing above the sign-in screens is ever pushed. */
    screen = entry.screen;
    break;
  }
}

/* ------------------------------------------------------------ sign-in flow */

/* The server's event socket said the user's items changed (see
 * jf_events_start), and the home rows have not been asked again since: they
 * also change which items they hold. Set from its thread, with the items' new
 * state queued for the next frame. */
static atomic_bool items_changed;
static bool home_stale;
#define CHANGES_QUEUED 64
static pthread_mutex_t changes_lock = PTHREAD_MUTEX_INITIALIZER;
static jf_user_data changes[CHANGES_QUEUED];
static size_t change_count;

static void on_items_changed(const jf_user_data *items, size_t count) {
  pthread_mutex_lock(&changes_lock);
  for (size_t i = 0; i < count && change_count < CHANGES_QUEUED; i++)
    changes[change_count++] = items[i];
  pthread_mutex_unlock(&changes_lock);
  atomic_store(&items_changed, true);
  jf_window_wake();
}

/* Re-authentications attempted since the last success. Bounded, or a server
 * that rejects a valid-looking password turns into a login loop. */
static unsigned relogin_attempts;
static void use_server(const char *address, const char *name);

static void signed_in(const jf_auth *auth, const char *used_password) {
  auth_generation++;
  const bool silent = screen == SCREEN_HOME && session.token[0] != '\0';
  set_text(session.token, sizeof(session.token), auth->access_token);
  set_text(session.user_id, sizeof(session.user_id), auth->user_id);
  set_text(session.user_name, sizeof(session.user_name), auth->user_name);
  set_text(session.password, sizeof(session.password), used_password);
  jf_fetcher_set_session(&fetcher, &session);
  jf_session_save(&session);
  jf_events_start(&session, on_items_changed);
  relogin_attempts = 0;
  set_status("Signed in as %s", session.user_name);
  if (silent) {
    /* A token replaced underneath a screen that is already up: reload what
     * failed, do not throw the user back to the top. */
    load_home();
    return;
  }
  screen = SCREEN_HOME;
  nav_focus(&ui, 0);
  depth = 0;
  load_home();
}

/* Replace a rejected token without involving the user.
 *
 * Jellyfin invalidates a device's previous token when that device signs in
 * again, so a stored token going stale is routine rather than exceptional -
 * signing the user out and asking for a password on a TV remote is the wrong
 * response when the password is right there. */
static bool reauthenticate(void) {
  if (session.password[0] == '\0' || session.user_name[0] == '\0')
    return false;
  if (relogin_attempts >= 2)
    return false;
  jf_task *task = request(JF_JOB_LOGIN, 0);
  if (task == NULL)
    return false;
  set_text(task->a, sizeof(task->a), session.user_name);
  set_text(task->b, sizeof(task->b), session.password);
  relogin_attempts++;
  jf_fetcher_start(&fetcher, task);
  set_status("Session expired; signing in again");
  return true;
}

static void sign_out(void) {
  auth_generation++;
  jf_events_stop();
  /* The next user's delays are their own. */
  saved_delay_count = 0;
  series_choice_count = 0;
  delays_asked = false;
  session.token[0] = '\0';
  session.user_id[0] = '\0';
  session.user_name[0] = '\0';
  session.password[0] = '\0';
  username[0] = '\0';
  password[0] = '\0';
  relogin_attempts = 0;
  jf_fetcher_set_session(&fetcher, &session);
  jf_session_save(&session);
  for (size_t i = 0; i < ROW_COUNT; i++)
    rows[i].count = 0;
  categories_row.count = 0;
  depth = 0;
  screen = SCREEN_AUTH;
  nav_focus(&ui, 0);
  status_text[0] = '\0';
  status_error = false;
}

/* ------------------------------------------------------------ task results */

static const char *job_name(jf_job job) {
  switch (job) {
  case JF_JOB_DISCOVER:
    return "discover";
  case JF_JOB_PROBE:
    return "probe";
  case JF_JOB_LOGIN:
    return "login";
  case JF_JOB_QUICK_INITIATE:
    return "quick_initiate";
  case JF_JOB_QUICK_POLL:
    return "quick_poll";
  case JF_JOB_QUICK_AUTHENTICATE:
    return "quick_authenticate";
  case JF_JOB_VIEWS:
    return "views";
  case JF_JOB_RESUME:
    return "resume";
  case JF_JOB_NEXT_UP:
    return "next_up";
  case JF_JOB_CHILDREN:
    return "children";
  case JF_JOB_ITEM:
    return "item";
  case JF_JOB_SEASONS:
    return "seasons";
  case JF_JOB_EPISODES:
    return "episodes";
  case JF_JOB_POSTER:
    return "poster";
  case JF_JOB_PLAYBACK_STARTED:
    return "playback_started";
  case JF_JOB_PLAYBACK_PROGRESS:
    return "playback_progress";
  case JF_JOB_PLAYBACK_INFO:
    return "playback_info";
  case JF_JOB_ADJACENT:
    return "adjacent";
  case JF_JOB_ENCODING:
    return "encoding";
  case JF_JOB_DELAYS:
    return "delays";
  case JF_JOB_SAVE_DELAYS:
    return "save_delays";
  case JF_JOB_SUBTITLE_SEARCH:
    return "subtitle_search";
  case JF_JOB_SUBTITLE_DOWNLOAD:
    return "subtitle_download";
  case JF_JOB_SUBTITLE_DELETE:
    return "subtitle_delete";
  case JF_JOB_SUBTITLE_CHECK:
    return "subtitle_check";
  case JF_JOB_SUBTITLE_SOURCES:
    return "subtitle_sources";
  case JF_JOB_SOURCE_INSTALL:
    return "source_install";
  case JF_JOB_SOURCE_TOGGLE:
    return "source_toggle";
  case JF_JOB_RESTART:
    return "restart";
  case JF_JOB_ADD_REPOSITORY:
    return "add_repository";
  case JF_JOB_MARK_PLAYED:
    return "mark_played";
  case JF_JOB_LATEST:
    return "latest";
  }
  return "?";
}

static void on_failure(jf_task *task) {
  fprintf(stderr, "%s failed: %s\n", job_name(task->job), task->error);
  /* A look at the list for a download: that flow's own to retry. */
  if (task->job == JF_JOB_PLAYBACK_INFO && task->tag == DOWNLOAD_POLL_TAG) {
    search_failed(task);
    return;
  }
  switch (task->job) {
  /* A missing poster is normal - plenty of items have none - so it marks the
   * slot done and leaves the placeholder card showing. The id stays, so the
   * same item is not asked for again every frame. */
  case JF_JOB_POSTER:
    slots[task->tag].loading = false;
    break;
  case JF_JOB_DISCOVER:
    set_error("Discovery failed: %s", task->error);
    discovering = false;
    break;
  case JF_JOB_PROBE:
    connecting = false;
    if (strcmp(task->error, "ServerError") == 0)
      set_error("Internal server error at %s", server_url);
    else if (strcmp(task->error, "ClientError") == 0 ||
             strcmp(task->error, "Unauthorized") == 0)
      set_error("Failed to connect to %s", server_url);
    else
      set_error("Failed to connect: %s", task->error);
    break;
  case JF_JOB_CHILDREN:
    grid_requested[task->tag % WINDOW_PAGES] = UINT32_MAX;
    grid_loading = false;
    if (strcmp(task->error, "ClientError") == 0 ||
        strcmp(task->error, "Unauthorized") == 0 ||
        strcmp(task->error, "ServerError") == 0) {
      screen = SCREEN_AUTH;
      nav_focus(&ui, 0);
      set_error("%s for %s at %s",
                strcmp(task->error, "ServerError") == 0
                    ? "Internal server error"
                    : "Request failed",
                session.user_name, session.url);
    }
    break;
  /* A rejected token is the one failure with a recovery: the stored credentials
   * are stale, so drop them rather than loop on 401s. Anything else - a server
   * restarting, a dropped Wi-Fi association - keeps them, because deleting a
   * good token over a transient error means typing a password back in on a TV
   * remote. */
  case JF_JOB_VIEWS:
  case JF_JOB_RESUME:
  case JF_JOB_NEXT_UP:
    if (strcmp(task->error, "Unauthorized") == 0) {
      if (!reauthenticate())
        sign_out();
      return;
    }
    if (strcmp(task->error, "ClientError") == 0 ||
        strcmp(task->error, "ServerError") == 0) {
      screen = SCREEN_AUTH;
      nav_focus(&ui, 0);
      set_error("%s for %s at %s",
                strcmp(task->error, "ServerError") == 0
                    ? "Internal server error"
                    : "Request failed",
                session.user_name, session.url);
      return;
    }
    set_error("%s failed: %s", job_name(task->job), task->error);
    break;
  case JF_JOB_QUICK_POLL:
    break;
  case JF_JOB_LOGIN:
    if (strcmp(task->error, "ServerError") == 0)
      set_error("Internal server error");
    else if (strcmp(task->error, "ClientError") == 0 ||
             strcmp(task->error, "Unauthorized") == 0)
      set_error("Failed for %s at %s", username, session.url);
    else
      set_error("Sign-in failed: %s", task->error);
    /* A stored password the server no longer accepts: forget it rather than
     * retry it on every screen. */
    if (relogin_attempts != 0)
      sign_out();
    break;
  case JF_JOB_ITEM:
  case JF_JOB_SEASONS:
  case JF_JOB_EPISODES:
    if (strcmp(task->error, "ClientError") == 0 ||
        strcmp(task->error, "Unauthorized") == 0 ||
        strcmp(task->error, "ServerError") == 0) {
      screen = SCREEN_AUTH;
      nav_focus(&ui, 0);
      set_error("%s for %s at %s",
                strcmp(task->error, "ServerError") == 0
                    ? "Internal server error"
                    : "Request failed",
                session.user_name, session.url);
      return;
    }
    set_error("%s failed: %s", job_name(task->job), task->error);
    break;
  /* Not an administrator: the details screen cannot tell a hardware
   * transcode from a software one, and says less. */
  case JF_JOB_ENCODING:
    server_transcode.known = false;
    break;
  case JF_JOB_DELAYS:
    /* None saved yet is a 404 on some servers: nothing to restore. */
    break;
  case JF_JOB_SAVE_DELAYS:
    delays_changed = true; /* again at the next close */
    set_error("Could not save the subtitle delay: %s", task->error);
    break;
  case JF_JOB_SUBTITLE_SEARCH:
  case JF_JOB_SUBTITLE_DOWNLOAD:
  case JF_JOB_SUBTITLE_CHECK:
    search_failed(task);
    break;
  case JF_JOB_SUBTITLE_SOURCES:
  case JF_JOB_SOURCE_INSTALL:
  case JF_JOB_SOURCE_TOGGLE:
  case JF_JOB_RESTART:
  case JF_JOB_ADD_REPOSITORY:
    sources_failed(task);
    break;
  case JF_JOB_SUBTITLE_DELETE:
    set_error("Could not delete the subtitle: %s", task->error);
    break;
  case JF_JOB_MARK_PLAYED:
    set_error("Could not mark it %s: %s", task->start ? "played" : "unplayed",
              task->error);
    break;
  default:
    set_error("%s failed: %s", job_name(task->job), task->error);
    break;
  }
}

static void use_playback_info(const jf_playback_info *info);

static void consume(jf_task *task) {
  if (is_auth_job(task->job) && task->tag != auth_generation)
    return;
  if (task->state == JF_TASK_FAILED) {
    on_failure(task);
    return;
  }
  upload_blurs(task);
  switch (task->job) {
  case JF_JOB_DISCOVER:
    for (size_t incoming = 0; incoming < task->server_count &&
                              discovered_count < DISCOVERED_CAPACITY;
         incoming++) {
      bool seen = false;
      for (size_t index = 0; index < discovered_count; index++)
        seen = seen || strcmp(discovered[index].address,
                              task->servers[incoming].address) == 0;
      if (seen)
        continue;
      set_text(discovered[discovered_count].name, sizeof(discovered[0].name),
               task->servers[incoming].name);
      set_text(discovered[discovered_count].address,
               sizeof(discovered[0].address), task->servers[incoming].address);
      discovered_count++;
    }
    if (now_ns() < discovery_until) {
      simple(JF_JOB_DISCOVER);
    } else {
      discovering = false;
    }
    break;

  case JF_JOB_PROBE:
    connecting = false;
    use_server(task->server.address, task->server.name);
    break;

  case JF_JOB_LOGIN:
    signed_in(&task->auth, task->b);
    break;
  /* Quick Connect never sees a password, so there is nothing to keep. */
  case JF_JOB_QUICK_AUTHENTICATE:
    signed_in(&task->auth, "");
    break;
  case JF_JOB_QUICK_INITIATE:
    set_text(quick_code, sizeof(quick_code), task->quick.code);
    set_text(quick_secret, sizeof(quick_secret), task->quick.secret);
    screen = SCREEN_QUICK;
    set_status("Waiting for approval");
    /* Also on stdout: on a TV the log is often easier to reach than the screen,
     * and this is the number the user has to read out. */
    fprintf(stderr, "quick connect code: %s\n", quick_code);
    break;
  case JF_JOB_QUICK_POLL:
    if (task->quick.authenticated) {
      jf_task *auth = request(JF_JOB_QUICK_AUTHENTICATE, 0);
      if (auth != NULL) {
        set_text(auth->a, sizeof(auth->a), quick_secret);
        jf_fetcher_start(&fetcher, auth);
      }
    }
    break;

  case JF_JOB_RESUME:
    row_fill(&rows[ROW_RESUME], task->list.items, task->list.count);
    break;
  case JF_JOB_NEXT_UP:
    row_fill(&rows[ROW_NEXT_UP], task->list.items, task->list.count);
    break;
  case JF_JOB_VIEWS: {
    /* Music and playlists have no poster grid worth opening from here, and the
     * scope for this client is video. */
    jf_item keep[CATEGORY_CAPACITY];
    size_t count = 0;
    for (size_t i = 0; i < task->list.count && count < CATEGORY_CAPACITY; i++) {
      const char *kind = task->list.items[i].collection_type;
      if (kind != NULL &&
          (strcmp(kind, "music") == 0 || strcmp(kind, "playlists") == 0))
        continue;
      keep[count++] = task->list.items[i];
    }
    row_fill(&rows[ROW_LIBRARIES], keep, count);
    row_fill(&categories_row, keep, count);
    /* Not for collections, which are made by hand rather than added to. */
    size_t latest = 0;
    for (size_t i = 0; i < count && latest < LATEST_ROWS; i++) {
      const char *kind = keep[i].collection_type;
      jf_task *fetch = kind != NULL && strcmp(kind, "boxsets") == 0
                           ? NULL
                           : request(JF_JOB_LATEST, (uint32_t)latest);
      if (fetch == NULL)
        continue;
      snprintf(latest_titles[latest], sizeof(latest_titles[0]),
               "Recently Added in %s",
               keep[i].name != NULL ? keep[i].name : "");
      set_text(latest_parents[latest], sizeof(latest_parents[0]), keep[i].id);
      rows[ROW_LATEST + latest].title = latest_titles[latest];
      set_text(fetch->a, sizeof(fetch->a), keep[i].id);
      jf_fetcher_start(&fetcher, fetch);
      latest++;
    }
    for (; latest < LATEST_ROWS; latest++)
      rows[ROW_LATEST + latest].count = 0;
    break;
  }
  case JF_JOB_LATEST:
    if (task->tag < LATEST_ROWS &&
        strcmp(task->a, latest_parents[task->tag]) == 0)
      row_fill(&rows[ROW_LATEST + task->tag], task->list.items,
               task->list.count);
    break;

  case JF_JOB_CHILDREN: {
    grid_loading = false;
    grid_total = task->list.total_record_count;
    const uint32_t start = task->tag * PAGE_SIZE;
    for (size_t offset = 0; offset < task->list.count; offset++) {
      const uint32_t absolute = start + (uint32_t)offset;
      const uint32_t ring = absolute % WINDOW_SIZE;
      grid_cards[ring] = card_from(&task->list.items[offset]);
      grid_at[ring] = absolute;
    }
    break;
  }

  case JF_JOB_ITEM:
    if (strcmp(task->a, detail.id) != 0)
      return;
    {
      /* A single item comes without a page to decode; the card it opened
       * from has the same poster's blurhash. */
      const card opened = detail;
      detail = card_from(&task->one);
      detail.blur_stamp = opened.blur_stamp;
      detail.blur_row = opened.blur_row;
      detail.blur_column = opened.blur_column;
      detail.blur_width = opened.blur_width;
      detail.blur_height = opened.blur_height;
    }
    detail_video = task->one.video;
    detail_has_video = task->one.has_video;
    break;

  case JF_JOB_ENCODING:
    server_transcode = task->encoding;
    break;

  case JF_JOB_DELAYS:
    /* What this playback already changed wins over what was saved. */
    for (size_t i = 0; i < task->pair_count; i++) {
      if (strncmp(task->keys[i], "sub:", 4) == 0 ||
          strncmp(task->keys[i], "audio:", 6) == 0) {
        bool known = false;
        for (size_t j = 0; j < series_choice_count && !known; j++)
          known = strcmp(series_choices[j].key, task->keys[i]) == 0;
        if (known || series_choice_count == SERIES_CHOICES)
          continue;
        set_text(series_choices[series_choice_count].key,
                 sizeof(series_choices[0].key), task->keys[i]);
        set_text(series_choices[series_choice_count++].choice,
                 sizeof(series_choices[0].choice), task->values[i]);
        continue;
      }
      bool known = false;
      for (size_t j = 0; j < saved_delay_count && !known; j++)
        known = strcmp(saved_delays[j].key, task->keys[i]) == 0;
      if (known || saved_delay_count == SAVED_DELAYS)
        continue;
      set_text(saved_delays[saved_delay_count].key, sizeof(saved_delays[0].key),
               task->keys[i]);
      saved_delays[saved_delay_count++].ms = atoi(task->values[i]);
    }
    break;
  case JF_JOB_SAVE_DELAYS:
    break;
  case JF_JOB_SUBTITLE_SEARCH:
  case JF_JOB_SUBTITLE_DOWNLOAD:
  case JF_JOB_SUBTITLE_CHECK:
    search_done(task);
    break;
  case JF_JOB_SUBTITLE_SOURCES:
  case JF_JOB_SOURCE_INSTALL:
  case JF_JOB_SOURCE_TOGGLE:
  case JF_JOB_RESTART:
  case JF_JOB_ADD_REPOSITORY:
    sources_done(task);
    break;
  case JF_JOB_MARK_PLAYED: {
    /* Either way, no position to resume from. */
    jf_user_data item = {.played = task->start != 0};
    snprintf(item.item_id, sizeof(item.item_id), "%.39s", task->a);
    apply_user_data(&item);
    break;
  }
  case JF_JOB_SUBTITLE_DELETE: {
    /* Gone from the server: its list, asked again, no longer has it. */
    awaiting_delete = true;
    jf_task *info = request(JF_JOB_PLAYBACK_INFO, 0);
    if (info != NULL) {
      set_text(info->a, sizeof(info->a), playback_item_id);
      jf_fetcher_start(&fetcher, info);
    }
    break;
  }

  case JF_JOB_SEASONS:
    if (strcmp(task->a, detail.id) != 0)
      return;
    row_fill(&seasons_row, task->list.items, task->list.count);
    /* The card may predate what was just watched; its seasons do not. */
    detail.remaining = 0;
    for (size_t i = 0; i < seasons_row.count; i++)
      detail.remaining += seasons_cards[i].remaining;
    detail.has_remaining = seasons_row.count != 0;
    detail.played = detail.has_remaining && detail.remaining == 0;
    /* Seasons inherit the series' backdrop. */
    if (detail.backdrop_id[0] == '\0' && seasons_row.count != 0) {
      set_text(detail.backdrop_id, sizeof(detail.backdrop_id),
               seasons_cards[0].backdrop_id);
      set_text(detail.backdrop_tag, sizeof(detail.backdrop_tag),
               seasons_cards[0].backdrop_tag);
    }
    break;

  case JF_JOB_EPISODES:
    if (strcmp(task->b, season_detail.id) != 0)
      return;
    row_fill(&episodes_row, task->list.items, task->list.count);
    break;

  case JF_JOB_POSTER:
    if (task->has_image) {
      poster_slot *slot = &slots[task->tag];
      slot->texture = jf_renderer_create_texture(
          renderer, task->image.width, task->image.height, task->image.rgb);
      slot->width = task->image.width;
      slot->height = task->image.height;
      slot->loading = false;
    }
    break;
  case JF_JOB_PLAYBACK_STARTED:
  case JF_JOB_PLAYBACK_PROGRESS:
    break;
  case JF_JOB_PLAYBACK_INFO:
    if (strcmp(task->a, playback_item_id) != 0)
      break;
    if (task->tag == DOWNLOAD_POLL_TAG)
      search_listed(&task->playback);
    else
      use_playback_info(&task->playback);
    break;
  case JF_JOB_ADJACENT:
    if (strcmp(task->a, playback_item_id) != 0)
      break;
    /* Started without its series, which the lookup found: the choice made
     * now is still the series'. */
    if (playback_series_id[0] == '\0')
      snprintf(playback_series_id, sizeof(playback_series_id), "%.47s",
               task->b);
    for (size_t i = 0; i < task->list.count; i++) {
      if (strcmp(task->list.items[i].id, playback_item_id) != 0)
        continue;
      if (i > 0)
        previous_episode = card_from(&task->list.items[i - 1]);
      if (i + 1 < task->list.count)
        next_episode = card_from(&task->list.items[i + 1]);
    }
    break;
  }
}

static void report_playback(jf_job job);
static void report_leaving(void);
static void remember_choice(void);

static void pump(void) {
  /* Changes show on every card at once, and reach the home rows when they are
   * next in view. */
  if (atomic_exchange(&items_changed, false)) {
    static jf_user_data incoming[CHANGES_QUEUED];
    pthread_mutex_lock(&changes_lock);
    const size_t count = change_count;
    memcpy(incoming, changes, count * sizeof(*changes));
    change_count = 0;
    pthread_mutex_unlock(&changes_lock);
    for (size_t i = 0; i < count; i++)
      apply_user_data(&incoming[i]);
    home_stale = true;
    jf_window_frame_requested = true;
  }
  if (home_stale && screen == SCREEN_HOME) {
    home_stale = false;
    load_home();
  }
  for (jf_task *task = jf_fetcher_finished(&fetcher); task != NULL;
       task = jf_fetcher_finished(&fetcher)) {
    if (task->job != JF_JOB_QUICK_POLL || task->state != JF_TASK_READY ||
        task->quick.authenticated)
      jf_window_frame_requested = true;
    consume(task);
    jf_fetcher_release(&fetcher, task);
  }
  if (screen == SCREEN_QUICK && quick_secret[0] != '\0' &&
      now_ns() > quick_poll_at) {
    quick_poll_at = now_ns() + 2000000000ull;
    jf_task *task = request(JF_JOB_QUICK_POLL, 0);
    if (task != NULL) {
      set_text(task->a, sizeof(task->a), quick_secret);
      jf_fetcher_start(&fetcher, task);
    }
  }
  poll_download();
  poll_sources();
  if (screen == SCREEN_PLAYBACK && !playback_paused &&
      playback_item_id[0] != '\0' && jf_player_state_get() == JF_PLAYING &&
      now_ns() >= playback_progress_at) {
    report_playback(JF_JOB_PLAYBACK_PROGRESS);
    playback_progress_at = now_ns() + 15000000000ull;
  }
}

/* --------------------------------------------------------------- text entry */

static char *field_buffer(edit_field which, size_t *out_len) {
  switch (which) {
  case EDIT_URL:
    *out_len = sizeof(server_url);
    return server_url;
  case EDIT_USERNAME:
    *out_len = sizeof(username);
    return username;
  case EDIT_PASSWORD:
    *out_len = sizeof(password);
    return password;
  default:
    *out_len = 0;
    return NULL;
  }
}

static void rect_ints(loom_rect rect, int out[4]) {
  out[0] = (int)rect.x;
  out[1] = (int)rect.y;
  out[2] = (int)rect.w;
  out[3] = (int)rect.h;
}

static void end_edit(void) {
  if (active_field == EDIT_NONE)
    return;
  jf_window_end_text_input();
  active_field = EDIT_NONE;
}

static void begin_edit(edit_field which, loom_rect rect) {
  end_edit();
  active_field = which;
  const jf_text_purpose purpose = which == EDIT_URL        ? JF_TEXT_URL
                                  : which == EDIT_PASSWORD ? JF_TEXT_PASSWORD
                                                           : JF_TEXT_NORMAL;
  int box[4];
  rect_ints(rect, box);
  if (jf_window_begin_text_input(box, purpose))
    set_status("webOS keyboard opened");
  else
    set_status("Type with the keyboard; Enter finishes");
}

static void append_text(const char *text) {
  size_t capacity = 0;
  char *target = field_buffer(active_field, &capacity);
  if (target == NULL)
    return;
  const size_t length = strlen(target);
  snprintf(target + length, capacity - length, "%s", text);
}

static void erase_text(size_t count) {
  size_t capacity = 0;
  char *target = field_buffer(active_field, &capacity);
  if (target == NULL)
    return;
  size_t length = strlen(target);
  while (count > 0 && length > 0) {
    length--;
    /* Back over a whole UTF-8 sequence, not a byte of one. */
    while (length > 0 && (target[length] & 0xc0) == 0x80)
      length--;
    count--;
  }
  target[length] = '\0';
}

/* --------------------------------------------------------------- navigation */

/* A slot that has not been paged in yet. Returning a pointer to this rather
 * than a temporary keeps every card the renderer sees alive past the frame. */
static card blank_card;

static const card *grid_card(size_t index) {
  if (index >= grid_total)
    return NULL;
  const size_t ring = index % WINDOW_SIZE;
  if (grid_at[ring] != index)
    return NULL;
  return grid_cards[ring].present ? &grid_cards[ring] : NULL;
}

static void use_server(const char *address, const char *name) {
  auth_generation++;
  set_text(server_url, sizeof(server_url), address);
  set_text(session.url, sizeof(session.url), address);
  set_text(server_name, sizeof(server_name),
           name != NULL && name[0] != '\0' ? name : address);
  jf_fetcher_set_session(&fetcher, &session);
  screen = SCREEN_AUTH;
  nav_focus(&ui, 0);
  status_text[0] = '\0';
  status_error = false;
}

/* Split one licence into lines once, at open, rather than measuring the whole
 * text every frame. The copy is mutable so the newlines become terminators in
 * place and each line is a pointer into it; the virtual list then draws only
 * what is on screen. */
static void open_license(size_t index) {
  free(license_body);
  free(license_lines);
  license_body = NULL;
  license_lines = NULL;
  license_line_count = 0;
  nav_forget(&ui, nav_str("license"));
  license_open = index;
  if (index >= JF_LICENSE_COUNT)
    return;

  const char *text = (const char *)jf_licenses[index].text;
  const size_t length = strlen(text);
  license_body = malloc(length + 1);
  if (license_body == NULL) {
    set_error("Not enough memory to open that licence");
    return;
  }
  memcpy(license_body, text, length + 1);

  size_t lines = 1;
  for (size_t i = 0; i < length; i++)
    if (license_body[i] == '\n')
      lines++;
  license_lines = malloc(lines * sizeof(*license_lines));
  if (license_lines == NULL) {
    free(license_body);
    license_body = NULL;
    set_error("Not enough memory to open that licence");
    return;
  }
  char *cursor = license_body;
  license_lines[license_line_count++] = cursor;
  for (size_t i = 0; i < length; i++) {
    if (license_body[i] != '\n')
      continue;
    license_body[i] = '\0';
    /* A trailing newline would otherwise add an empty last line. */
    if (i + 1 < length)
      license_lines[license_line_count++] = &license_body[i + 1];
  }
  /* Also the only walk over every glyph in the text, which warms the atlas here
   * rather than on the first frame that scrolls into a character it has not
   * seen. */
  license_text_width = 0;
  for (size_t i = 0; i < license_line_count; i++)
    license_text_width =
        maxf(license_text_width,
             jf_renderer_measure(renderer, license_lines[i], 1.0f));
  screen = SCREEN_LICENSE;
}

static void open_sidebar(void) {
  end_edit();
  sidebar_open = true;
  sidebar_closing = false;
  sidebar_return_screen = screen;
  sidebar_return_focus = ui.focus;
  animated_float_set(&sidebar_slide, 1.0f, SIDEBAR_ANIMATION_NS);
}

static void close_sidebar(void) {
  if (!sidebar_open || sidebar_closing)
    return;
  sidebar_closing = true;
  nav_focus(&ui, sidebar_return_focus);
  animated_float_set(&sidebar_slide, 0.0f, NAVIGATION_ANIMATION_NS);
}

/* Row `index` of the sidebar: a category, or past the last one, Settings. */
static void activate_sidebar(size_t index) {
  close_sidebar();
  if (index < categories_row.count) {
    const card chosen = categories_row.cards[index];
    push();
    forget_scroll(chosen.id);
    open_grid(&chosen);
    return;
  }
  screen = SCREEN_SETTINGS;
}

static void go_back(void) {
  if (sidebar_open) {
    close_sidebar();
    return;
  }
  if (screen == SCREEN_PLAYBACK) {
    report_leaving();
    jf_player_stop();
    playback_paused = false;
    playback_item_id[0] = '\0';
    screen = playback_return_screen;
    nav_focus(&ui, playback_return_focus);
    set_status("Stopped playback");
    return;
  }
  if (screen == SCREEN_SERVER && connecting) {
    auth_generation++;
    connecting = false;
    return;
  }
  if (active_field != EDIT_NONE) {
    end_edit();
    return;
  }
  switch (screen) {
  case SCREEN_SERVER:
    jf_window_running = false;
    break;
  case SCREEN_AUTH:
    auth_generation++;
    screen = SCREEN_SERVER;
    nav_focus(&ui, 0);
    break;
  case SCREEN_QUICK:
    auth_generation++;
    quick_secret[0] = '\0';
    screen = SCREEN_AUTH;
    nav_focus(&ui, 0);
    break;
  case SCREEN_SETTINGS:
    screen = sidebar_return_screen;
    nav_focus(&ui, sidebar_return_focus);
    break;
  case SCREEN_LICENSES:
    screen = SCREEN_SETTINGS;
    nav_focus(&ui, nav_str("settings.licenses"));
    break;
  case SCREEN_LICENSE:
    nav_focus(&ui, nav_child(nav_str("licenses"), nav_int(license_open)));
    open_license(JF_LICENSE_COUNT); /* frees the text */
    screen = SCREEN_LICENSES;
    break;
  default:
    pop();
    break;
  }
}

/* The transport row. The play/pause button is the one that toggles; the rest
 * seek by their own number of seconds. */
typedef enum {
  ACTION_PREVIOUS,
  ACTION_CHAPTER_BACK,
  ACTION_BACK_30,
  ACTION_BACK_10,
  ACTION_PAUSE,
  ACTION_FORWARD_10,
  ACTION_FORWARD_30,
  ACTION_CHAPTER_NEXT,
  ACTION_NEXT,
  ACTION_SUBTITLES,
  ACTION_AUDIO,
} playback_action;

/* In display order; the first TRANSPORT_COUNT are the transport, centred on
 * play/pause, and the rest sit at the right. `number` is drawn inside the
 * circular-arrow icons, `tag` small under the skip ones: they skip episodes
 * or chapters, and each shows only when there is one to skip to. */
static const struct {
  const char *icon, *number, *tag, *hint;
  playback_action action;
} playback_buttons[] = {
    {JF_ICON_PREVIOUS, "", "EP", "", ACTION_PREVIOUS},
    {JF_ICON_PREVIOUS, "", "CH", "Previous chapter", ACTION_CHAPTER_BACK},
    {JF_ICON_BACK, "30", "", "Back 30 seconds", ACTION_BACK_30},
    {JF_ICON_BACK, "10", "", "Back 10 seconds", ACTION_BACK_10},
    {JF_ICON_PAUSE, "", "", "", ACTION_PAUSE},
    {JF_ICON_FORWARD, "10", "", "Forward 10 seconds", ACTION_FORWARD_10},
    {JF_ICON_FORWARD, "30", "", "Forward 30 seconds", ACTION_FORWARD_30},
    {JF_ICON_NEXT, "", "CH", "Next chapter", ACTION_CHAPTER_NEXT},
    {JF_ICON_NEXT, "", "EP", "", ACTION_NEXT},
    {JF_ICON_SUBTITLES, "", "", "", ACTION_SUBTITLES},
    {JF_ICON_AUDIO, "", "", "", ACTION_AUDIO},
};
#define PLAYBACK_BUTTON_COUNT                                                  \
  (sizeof(playback_buttons) / sizeof(playback_buttons[0]))
#define PLAY_PAUSE_INDEX 4
#define TRANSPORT_COUNT 9

static nav_id seek_bar_id(void) { return nav_str("playback.seek"); }
static nav_id button_id(size_t index) {
  return nav_child(nav_str("playback.buttons"), nav_int(index));
}

static bool button_shown(size_t index) {
  const int *starts = NULL;
  switch (playback_buttons[index].action) {
  case ACTION_PREVIOUS:
    return previous_episode.present;
  case ACTION_NEXT:
    return next_episode.present;
  case ACTION_CHAPTER_BACK:
  case ACTION_CHAPTER_NEXT:
    return jf_player_chapters(&starts) > 0;
  default:
    return true;
  }
}

static void report_position(jf_job job, int position_ms) {
  if (playback_item_id[0] == '\0')
    return;
  jf_task *task = request(job, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), playback_item_id);
  set_text(task->b, sizeof(task->b), playback_source_id);
  task->subtitle_stream =
      jf_player_subtitle_stream(jf_player_subtitle_current());
  task->position_ticks =
      (uint64_t)(position_ms > 0 ? position_ms : 0) * 10000ull;
  jf_fetcher_start(&fetcher, task);
}

static void report_playback(jf_job job) {
  report_position(job, jf_player_position());
}

/* The last report before going: inside a closing chapter it says the end, as
 * leaving during the credits is having watched it. The server counts 90%,
 * which an hour's six minutes of credits start before. "Closing" is a last
 * chapter in the final 15%. */
static void report_leaving(void) {
  /* The subtitle too, which a panel still open has not remembered yet. */
  remember_choice();
  const int *starts = NULL;
  const int chapters = jf_player_chapters(&starts);
  const int duration = jf_player_duration();
  const int last = chapters > 1 ? starts[chapters - 1] : INT_MAX;
  const int position = jf_player_position();
  report_position(JF_JOB_PLAYBACK_PROGRESS,
                  last >= duration * 0.85 && position >= last ? duration
                                                              : position);
}

/* The server's subtitle streams, with its pick for the user's preferences
 * selected. */
/* `text` appended at `at`, kept to characters a JSON string takes as they
 * are; returns the new end. */
static int append_plain(char *out, int at, int size, const char *text) {
  for (const char *c = text; c != NULL && *c != '\0' && at + 1 < size; c++)
    out[at++] =
        (*c >= '0' && *c <= '9') || ((*c | 0x20) >= 'a' && (*c | 0x20) <= 'z')
            ? *c
            : '_';
  out[at] = '\0';
  return at;
}

/* What a track's delay is saved under, and how a new one is told from the
 * ones before it. Not the server's index: files beside the video are numbered
 * first, so a download would shift every embedded track's. `embedded` counts
 * the embedded tracks so far. */
static void stream_key(const jf_media_stream *stream, int *embedded,
                       char out[80]) {
  if (!stream->external) {
    snprintf(out, 80, "%.40s:e%d", playback_item_id, (*embedded)++);
    return;
  }
  /* Files beside the video come in whatever order the server lists them,
   * which a download changes: by name. */
  append_plain(out, snprintf(out, 80, "%.40s:x:", playback_item_id), 80,
               stream->title);
}

/* Language, then title: "eng_Full", "_" for neither. */
static void stream_choice(const jf_media_stream *stream, char out[48]) {
  const int at = append_plain(out, 0, 48, stream->language);
  out[at] = '_';
  append_plain(out, at + 1, 48, stream->name);
}

/* The current series' remembered choice of `kind`, "sub" or "audio", made
 * empty on `create`; NULL outside a series. */
static char *series_choice(const char *kind, bool create) {
  if (playback_series_id[0] == '\0')
    return NULL;
  char key[56];
  snprintf(key, sizeof(key), "%s:%.47s", kind, playback_series_id);
  for (size_t i = 0; i < series_choice_count; i++)
    if (strcmp(series_choices[i].key, key) == 0)
      return series_choices[i].choice;
  if (!create || series_choice_count == SERIES_CHOICES)
    return NULL;
  set_text(series_choices[series_choice_count].key,
           sizeof(series_choices[0].key), key);
  series_choices[series_choice_count].choice[0] = '\0';
  return series_choices[series_choice_count++].choice;
}

/* The track among `choices` a remembered choice names: the same language and
 * title, else the first in the language; `fallback` when the episode has
 * neither, or nothing is remembered. */
static int chosen_track(const char *wanted, char choices[][48], int count,
                        int fallback) {
  if (wanted == NULL || wanted[0] == '\0')
    return fallback;
  if (strcmp(wanted, "-") == 0)
    return -1;
  const size_t language = strcspn(wanted, "_") + 1;
  int same_language = -1;
  for (int i = 0; i < count; i++) {
    if (strcmp(choices[i], wanted) == 0)
      return i;
    if (same_language < 0 && strncmp(choices[i], wanted, language) == 0)
      same_language = i;
  }
  return same_language >= 0 ? same_language : fallback;
}

/* The audio track wanted, once the player lists the source's: matched by the
 * server's index. */
static void select_wanted_audio(void) {
  for (int i = 0; audio_wanted >= 0 && i < jf_player_audio_count(); i++)
    if (jf_player_audio_stream(i) == audio_wanted) {
      jf_player_audio_select(i);
      audio_wanted = -1;
    }
}

static void use_playback_info(const jf_playback_info *info) {
  set_text(playback_source_id, sizeof(playback_source_id),
           info->media_source_id);
  static jf_player_track tracks[KNOWN_TRACKS];
  int count = 0, selected = -1, embedded = 0;
  memset(track_key, 0, sizeof(track_key));
  memset(track_choice, 0, sizeof(track_choice));
  for (size_t i = 0; i < info->subtitle_count && count < KNOWN_TRACKS; i++) {
    const jf_media_stream *stream = &info->subtitles[i];
    /* A bitmap file beside the video has no source here: the server
     * converts only text, and it is not in the container. */
    if (!stream->text && stream->external)
      continue;
    jf_player_track *track = &tracks[count];
    track_text[count] = stream->text;
    track_styled[count] = stream->styled;
    track_external[count] = stream->external;
    stream_key(stream, &embedded, track_key[count]);
    stream_choice(stream, track_choice[count]);
    *track = (jf_player_track){
        stream->index, stream->external, stream->text, "", "", -1};
    set_text(track->name, sizeof(track->name), stream->title);
    if (stream->text)
      jf_subtitle_url(&session, playback_item_id, info->media_source_id,
                      stream->index, track->url, sizeof(track->url));
    if (stream->index == info->default_subtitle)
      selected = count;
    count++;
  }
  selected =
      chosen_track(series_choice("sub", false), track_choice, count, selected);
  /* A track just deleted: none selected, and back in the picker on "off". */
  if (awaiting_delete) {
    awaiting_delete = false;
    selected = -1;
    set_status("Subtitle deleted");
    if (subtitle_settings) {
      subtitle_settings = false;
      subtitle_picker = true;
      nav_focus(&ui, picker_row_id(track_row(-1)));
    }
  }
  /* A download just landed: the track that was not there before. */
  if (search.phase == DOWNLOAD_SELECTING) {
    search.phase = DOWNLOAD_NONE;
    for (int i = 0; i < count; i++) {
      bool known = false;
      for (int j = 0; j < search.before_count && !known; j++)
        known = strcmp(search.before[j], track_key[i]) == 0;
      if (!known)
        selected = i;
    }
    /* Back in the picker on it: focus is the selection there. */
    if (search.open) {
      search.open = false;
      subtitle_picker = true;
      nav_focus(&ui, picker_row_id(track_row(selected)));
    }
  }
  jf_player_subtitle_tracks(tracks, count, selected);
  int audio = -1;
  audio_known = 0;
  for (size_t i = 0; i < info->audio_count && audio_known < KNOWN_TRACKS; i++) {
    audio_index[audio_known] = info->audio[i].index;
    stream_choice(&info->audio[i], audio_choice[audio_known]);
    if (info->audio[i].index == info->default_audio)
      audio = audio_known;
    audio_known++;
  }
  audio = chosen_track(series_choice("audio", false), audio_choice, audio_known,
                       audio);
  audio_wanted = audio >= 0 ? audio_index[audio] : -1;
  select_wanted_audio();
  fprintf(stderr, "Playback info: %d subtitle track(s), default stream %d\n",
          count, info->default_subtitle);
}

static void start_playback(const char *id, const char *title,
                           const char *series_id, uint64_t resume_ticks) {
  char stream[1024];
  jf_stream_url(&session, id, stream, sizeof(stream));
  if (stream[0] == '\0') {
    set_error("Could not build a stream URL");
    return;
  }
  /* The transcode request carries the hardware capability constraints. Keep
   * this comfortably above a long reverse-proxy address plus access token. */
  char transcode[2048];
  jf_transcode_url(&session, id, transcode, sizeof(transcode));
  const uint64_t resume_ms = resume_ticks / 10000ull;
  const int start_ms = resume_ms > INT32_MAX ? INT32_MAX : (int)resume_ms;
  jf_player_decoders = *tv_decoders();
  if (!jf_player_play(stream, transcode, gl_width, gl_height, start_ms)) {
    set_error("Playback failed: %s", jf_player_error());
    return;
  }
  if (subtitle_texture != 0) {
    /* Whatever the last item left on screen is not this one's. */
    jf_renderer_destroy_texture(renderer, subtitle_texture);
    subtitle_texture = 0;
  }
  set_text(playback_title, sizeof(playback_title), title);
  set_text(playback_item_id, sizeof(playback_item_id), id);
  set_text(playback_source_id, sizeof(playback_source_id), id);
  set_text(playback_series_id, sizeof(playback_series_id), series_id);
  audio_known = 0;
  audio_wanted = -1;
  subtitle_picker = false;
  subtitle_settings = false;
  search.open = false;
  sources.open = false;
  jf_task *info = request(JF_JOB_PLAYBACK_INFO, 0);
  if (info != NULL) {
    set_text(info->a, sizeof(info->a), id);
    jf_fetcher_start(&fetcher, info);
  }
  previous_episode.present = next_episode.present = false;
  jf_task *adjacent = request(JF_JOB_ADJACENT, 0);
  if (adjacent != NULL) {
    set_text(adjacent->a, sizeof(adjacent->a), id);
    set_text(adjacent->b, sizeof(adjacent->b), series_id);
    jf_fetcher_start(&fetcher, adjacent);
  }
  playback_paused = false;
  playback_started_at = now_ns();
  playback_progress_at = playback_started_at + 15000000000ull;
  playback_return_screen = screen;
  playback_return_focus = ui.focus;
  nav_focus(&ui, button_id(PLAY_PAUSE_INDEX));
  glow_placed = false;
  animated_float_snap(&chrome_motion, 0);
  show_playback_chrome();
  screen = SCREEN_PLAYBACK;
  report_playback(JF_JOB_PLAYBACK_STARTED);
}

static void toggle_playback(void) {
  if (playback_paused) {
    jf_player_resume();
    playback_paused = false;
    set_status("Playing");
  } else {
    jf_player_pause();
    playback_paused = true;
    set_status("Paused");
  }
}

static nav_id picker_row_id(size_t row) {
  return nav_child(nav_str("subtitles"), nav_int(row));
}

/* Row 0 downloads more, row 1 is off, row t + 2 the player's track t. */
static size_t track_row(int track) { return (size_t)(track + 2); }

static bool track_is_text(int track) {
  return track >= 0 && track < KNOWN_TRACKS && track_text[track];
}
static bool track_is_ass(int track) {
  return track_is_text(track) && track_styled[track];
}
/* Drawn by the app as plain text: text, and not ASS the script styles unless
 * those are plain too. */
static bool track_plain(int track) {
  return track_is_text(track) && (!track_styled[track] || sub_style.plain_ass);
}

static void open_subtitle_picker(void) {
  subtitle_picker = true;
  jf_player_subtitle_preview(true);
  picker_return_focus = ui.focus;
  picker_focus_pending = true;
}

static void save_delays(void);

/* `now` as the series' choice of `kind`; whether that changed it. */
static bool remember(const char *kind, const char *now) {
  char *choice = now[0] != '\0' ? series_choice(kind, true) : NULL;
  if (choice == NULL || strcmp(choice, now) == 0)
    return false;
  snprintf(choice, sizeof(series_choices[0].choice), "%.47s", now);
  return true;
}

/* The tracks playing, for the series' next episode. */
static void remember_choice(void) {
  const int track = jf_player_subtitle_current();
  bool changed = remember("sub", track < 0              ? "-"
                                 : track < KNOWN_TRACKS ? track_choice[track]
                                                        : "");
  /* Not before the wanted one is in: what plays is only the first. */
  const int audio = jf_player_audio_stream(jf_player_audio_current());
  for (int i = 0; audio_wanted < 0 && audio >= 0 && i < audio_known; i++)
    if (audio_index[i] == audio)
      changed |= remember("audio", audio_choice[i]);
  if (changed)
    save_delays();
}

static void close_subtitle_picker(void) {
  subtitle_picker = false;
  jf_player_subtitle_preview(false);
  remember_choice();
  report_playback(JF_JOB_PLAYBACK_PROGRESS);
  nav_focus(&ui, picker_return_focus);
}

typedef enum {
  SETTING_PLAIN_ASS,
  SETTING_COLOR,
  SETTING_OUTLINE,
  SETTING_OUTLINE_WIDTH,
  SETTING_OUTLINE_COLOR,
  SETTING_INLINE,
  SETTING_BOLD,
  SETTING_SIZE,
  SETTING_MARGIN,
  SETTING_SIDE,
  SETTING_DELAY,
  SETTING_SUGGEST,
  SETTING_DELETE,
  SETTING_COUNT
} subtitle_setting;

static nav_id setting_row_id(subtitle_setting row) {
  return nav_child(nav_str("subtitle-settings"), nav_int(row));
}

/* The outline's own settings, drawn under it and only while it is on. */
static bool setting_child(subtitle_setting row) {
  return row == SETTING_OUTLINE_WIDTH || row == SETTING_OUTLINE_COLOR;
}

/* How the text looks, as opposed to when it shows. */
static bool setting_is_style(subtitle_setting row) {
  return row < SETTING_DELAY;
}

/* Whether a row is in the panel for the current track at all. */
static bool setting_shown(subtitle_setting row) {
  const int track = jf_player_subtitle_current();
  if (row == SETTING_PLAIN_ASS)
    return track_is_ass(track);
  /* Only a file beside the video is the server's to delete. */
  if (row == SETTING_DELETE)
    return track >= 0 && track < KNOWN_TRACKS && track_external[track];
  return !setting_child(row) || sub_style.outline;
}

/* Whether it does anything for the current track, else greyed out: a bitmap
 * track is pictures, so no style applies; libass takes only the placement of
 * styled ASS; and only lines can be matched to another track's. */
static bool setting_enabled(subtitle_setting row) {
  const int track = jf_player_subtitle_current();
  if (row == SETTING_PLAIN_ASS || row == SETTING_DELAY || row == SETTING_DELETE)
    return true;
  if (row == SETTING_SUGGEST)
    return track_is_text(track);
  if (row == SETTING_SIZE || row == SETTING_MARGIN || row == SETTING_SIDE)
    return track_is_text(track);
  return track_plain(track);
}

/* The current track's delay, made on first use; NULL for a track with no
 * key - off, or one only the container lists. */
static int *current_delay(void) {
  const int track = jf_player_subtitle_current();
  if (track < 0 || track >= KNOWN_TRACKS || track_key[track][0] == '\0')
    return NULL;
  for (size_t i = 0; i < saved_delay_count; i++)
    if (strcmp(saved_delays[i].key, track_key[track]) == 0)
      return &saved_delays[i].ms;
  if (saved_delay_count == SAVED_DELAYS)
    return NULL;
  snprintf(saved_delays[saved_delay_count].key, sizeof(saved_delays[0].key),
           "%.79s", track_key[track]);
  saved_delays[saved_delay_count].ms = 0;
  return &saved_delays[saved_delay_count++].ms;
}

/* Every delay but zero, as the display preferences' custom settings. */
static void save_delays(void) {
  jf_task *task = request(JF_JOB_SAVE_DELAYS, 0);
  if (task == NULL)
    return;
  size_t length = 0,
         capacity = 128 + (saved_delay_count + series_choice_count) * 100;
  char *body = jf_arena_alloc(&task->arena, capacity);
  if (body == NULL) {
    jf_fetcher_release(&fetcher, task);
    return;
  }
  length += (size_t)snprintf(body, capacity,
                             "{\"Id\":\"subtitle-delays\",\"Client\":\"%s\","
                             "\"CustomPrefs\":{",
                             JF_CLIENT_NAME);
  bool first = true;
  for (size_t i = 0; i < saved_delay_count; i++) {
    if (saved_delays[i].ms == 0)
      continue;
    /* Keys are ids and digits: nothing in them needs escaping. */
    length += (size_t)snprintf(body + length, capacity - length,
                               "%s\"%s\":\"%d\"", first ? "" : ",",
                               saved_delays[i].key, saved_delays[i].ms);
    first = false;
  }
  for (size_t i = 0; i < series_choice_count; i++) {
    length += (size_t)snprintf(body + length, capacity - length,
                               "%s\"%s\":\"%s\"", first ? "" : ",",
                               series_choices[i].key, series_choices[i].choice);
    first = false;
  }
  snprintf(body + length, capacity - length, "}}");
  task->body = body;
  jf_fetcher_start(&fetcher, task);
  delays_changed = false;
}

/* The panel keeps the picker's preview on: it is what gives the delay
 * suggestion the other tracks' lines to compare. */
static void open_subtitle_settings(void) {
  subtitle_picker = false;
  subtitle_settings = true;
  for (int i = 0; i < SETTING_COUNT; i++)
    if (setting_shown((subtitle_setting)i) &&
        setting_enabled((subtitle_setting)i)) {
      nav_focus(&ui, setting_row_id((subtitle_setting)i));
      break;
    }
}

static void close_subtitle_settings(void) {
  subtitle_settings = false;
  save_preferences();
  if (delays_changed)
    save_delays();
  subtitle_picker = true;
  nav_focus(&ui, picker_row_id(track_row(jf_player_subtitle_current())));
}

static void suggest_delay(void) {
  int *delay = current_delay();
  int64_t suggested = 0;
  if (delay == NULL) {
    set_status("Choose a track first");
  } else if (jf_subs_suggest_delay(&suggested)) {
    *delay = (int)suggested;
    delays_changed = true;
    set_status("Delay set to %+.1f s, matched to another track",
               *delay / 1000.0);
  } else {
    set_status("Not enough lines in common with another track yet");
  }
}

/* Rows OK (or a click) acts on: switches and actions. A value that steps is
 * left and right's alone, so a stray click never nudges it. */
static bool setting_is_toggle(subtitle_setting row) {
  return row == SETTING_PLAIN_ASS || row == SETTING_OUTLINE ||
         row == SETTING_INLINE || row == SETTING_BOLD || row == SETTING_SIDE ||
         row == SETTING_SUGGEST || row == SETTING_DELETE;
}

static void delete_track(void) {
  const int track = jf_player_subtitle_current();
  jf_task *task = request(JF_JOB_SUBTITLE_DELETE, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), playback_item_id);
  snprintf(task->b, sizeof(task->b), "%d", jf_player_subtitle_stream(track));
  jf_fetcher_start(&fetcher, task);
  delete_armed = false;
  set_status("Deleting %s", jf_player_subtitle_name(track));
}

/* Left and right change the row's value, OK toggles or acts. */
static void change_setting(subtitle_setting row, int step) {
  int *delay = current_delay();
  switch (row) {
  case SETTING_PLAIN_ASS:
    sub_style.plain_ass = !sub_style.plain_ass;
    break;
  case SETTING_COLOR:
    sub_style.color = (sub_style.color + step + SWATCH_COUNT) % SWATCH_COUNT;
    break;
  case SETTING_OUTLINE:
    sub_style.outline = !sub_style.outline;
    break;
  case SETTING_OUTLINE_WIDTH:
    sub_style.outline_width += step;
    break;
  case SETTING_OUTLINE_COLOR:
    sub_style.outline_color =
        (sub_style.outline_color + step + SWATCH_COUNT) % SWATCH_COUNT;
    break;
  case SETTING_INLINE:
    sub_style.ignore_inline = !sub_style.ignore_inline;
    break;
  case SETTING_BOLD:
    sub_style.bold = !sub_style.bold;
    break;
  case SETTING_SIZE:
    sub_style.size_percent += 10 * step;
    break;
  case SETTING_MARGIN:
    sub_style.margin_percent += step;
    break;
  case SETTING_SIDE:
    sub_style.top = !sub_style.top;
    break;
  case SETTING_DELAY:
    if (delay != NULL) {
      *delay = clampi(*delay + 100 * step, -60000, 60000);
      delays_changed = true;
    }
    break;
  case SETTING_SUGGEST:
    suggest_delay();
    break;
  case SETTING_DELETE:
    if (delete_armed)
      delete_track();
    else
      delete_armed = true;
    break;
  case SETTING_COUNT:
    break;
  }
  apply_subtitle_style();
}

static subtitle_setting focused_setting(void) {
  for (int i = 0; i < SETTING_COUNT; i++)
    if (ui.focus == setting_row_id((subtitle_setting)i))
      return (subtitle_setting)i;
  return SETTING_COUNT;
}

static void settings_key(uint32_t code) {
  const subtitle_setting row = focused_setting();
  if (jf_window_is_back_key(code) || code == KEY_SUBTITLE)
    close_subtitle_settings();
  else if (code == 103)
    nav_move(&ui, NAV_UP);
  else if (code == 108)
    nav_move(&ui, NAV_DOWN);
  else if ((code == 105 || code == 106) && !setting_is_toggle(row))
    change_setting(row, code == 105 ? -1 : 1);
  else if ((code == 105 || code == 106) && row != SETTING_SUGGEST &&
           row != SETTING_DELETE)
    change_setting(row, 1); /* a switch flips either way */
  else if ((code == 28 || code == 96 || code == 352) && setting_is_toggle(row))
    change_setting(row, 1);
}

/* ------------------------------------------------------ subtitle search
 *
 * The server's subtitle providers (OpenSubtitles and the like, set up by an
 * administrator): results for a language, and one of them downloaded beside
 * the video, after which the refreshed track list selects it. */

static const struct {
  const char *code, *name;
} search_languages[] = {
    {"eng", "English"},    {"ger", "German"},    {"fre", "French"},
    {"spa", "Spanish"},    {"ita", "Italian"},   {"por", "Portuguese"},
    {"dut", "Dutch"},      {"swe", "Swedish"},   {"nor", "Norwegian"},
    {"dan", "Danish"},     {"fin", "Finnish"},   {"pol", "Polish"},
    {"cze", "Czech"},      {"hun", "Hungarian"}, {"rum", "Romanian"},
    {"gre", "Greek"},      {"rus", "Russian"},   {"ukr", "Ukrainian"},
    {"tur", "Turkish"},    {"ara", "Arabic"},    {"heb", "Hebrew"},
    {"hin", "Hindi"},      {"tha", "Thai"},      {"vie", "Vietnamese"},
    {"ind", "Indonesian"}, {"jpn", "Japanese"},  {"kor", "Korean"},
    {"chi", "Chinese"},
};
#define SEARCH_LANGUAGE_COUNT                                                  \
  (int)(sizeof(search_languages) / sizeof(search_languages[0]))

static nav_id search_row_id(size_t row) {
  return nav_child(nav_str("subtitle-search"), nav_int(row));
}

static void start_search(void) {
  sub_style.search_language =
      clampi(sub_style.search_language, 0, SEARCH_LANGUAGE_COUNT - 1);
  jf_task *task = request(JF_JOB_SUBTITLE_SEARCH, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), playback_item_id);
  set_text(task->b, sizeof(task->b),
           search_languages[sub_style.search_language].code);
  jf_fetcher_start(&fetcher, task);
  search.searching = true;
  search.count = 0;
  search.error[0] = '\0';
}

/* The download panel's rows: its sources, the language - where the focus
 * starts - and then the results. */
#define SEARCH_ROW_SOURCES 0
#define SEARCH_ROW_LANGUAGE 1
#define SEARCH_ROW_RESULTS 2

static void refresh_sources(void) {
  jf_task *task = request(JF_JOB_SUBTITLE_SOURCES, 0);
  if (task == NULL)
    return;
  jf_fetcher_start(&fetcher, task);
  sources.loading = true;
}

static int sources_on(void) {
  int on = 0;
  for (size_t i = 0; i < sources.count; i++)
    on += strcmp(sources.items[i].status, "Active") == 0;
  return on;
}

static bool restart_pending(void) {
  for (size_t i = 0; i < sources.count; i++)
    if (strcmp(sources.items[i].status, "Restart") == 0)
      return true;
  return sources.changed;
}

/* The sources panel's rows: the sources, then the offer of more while
 * subbuzz is not among them, then the restart while one is pending. */
static bool offer_repository(void) {
  if (!sources.loaded)
    return false;
  for (size_t i = 0; i < sources.count; i++)
    if (strcasecmp(sources.items[i].name, "subbuzz") == 0)
      return false;
  return true;
}
static size_t repository_row(void) { return sources.count; }
static size_t restart_row(void) {
  return sources.count + (offer_repository() ? 1 : 0);
}
static size_t source_rows(void) {
  return restart_row() + (restart_pending() ? 1 : 0);
}

static void add_repository(void) {
  if (sources.adding)
    return;
  jf_task *task = request(JF_JOB_ADD_REPOSITORY, 0);
  if (task == NULL)
    return;
  jf_fetcher_start(&fetcher, task);
  sources.adding = true;
  sources.note[0] = '\0';
}

static void open_subtitle_search(void) {
  subtitle_picker = false;
  search.open = true;
  nav_focus(&ui, search_row_id(SEARCH_ROW_LANGUAGE));
  refresh_sources();
  start_search();
}

static void open_sources(void) {
  search.open = false;
  sources.open = true;
  sources.note[0] = '\0';
  sources.restart_armed = false;
  nav_focus(&ui, nav_child(nav_str("subtitle-sources"), nav_int(0)));
  refresh_sources();
}

static void close_sources(void) {
  sources.open = false;
  search.open = true;
  nav_focus(&ui, search_row_id(SEARCH_ROW_SOURCES));
  /* What was switched on may find what nothing did before. */
  start_search();
}

static void sources_warn(bool error, const char *pattern, ...) {
  va_list args;
  va_start(args, pattern);
  vsnprintf(sources.note, sizeof(sources.note), pattern, args);
  va_end(args);
  sources.note_is_error = error;
}

/* Install what is not there, switch what is. */
static void activate_source(size_t index) {
  if (index >= sources.count || sources.items[index].busy)
    return;
  const char *status = sources.items[index].status;
  if (strcmp(status, "Malfunctioned") == 0 ||
      strcmp(status, "NotSupported") == 0) {
    sources_warn(true, "%s does not work on this server version",
                 sources.items[index].name);
    return;
  }
  const bool install = status[0] == '\0';
  jf_task *task =
      request(install ? JF_JOB_SOURCE_INSTALL : JF_JOB_SOURCE_TOGGLE, 0);
  if (task == NULL)
    return;
  if (install) {
    set_text(task->a, sizeof(task->a), sources.items[index].name);
    set_text(task->b, sizeof(task->b), sources.items[index].guid);
  } else {
    set_text(task->a, sizeof(task->a), sources.items[index].guid);
    set_text(task->b, sizeof(task->b), sources.items[index].version);
    /* On, or on once the server restarts, switches off; anything else on. */
    task->start =
        strcmp(status, "Active") != 0 && strcmp(status, "Restart") != 0;
  }
  jf_fetcher_start(&fetcher, task);
  sources.items[index].busy = true;
  sources.note[0] = '\0';
}

static void restart_server(void) {
  if (!sources.restart_armed) {
    sources.restart_armed = true;
    return;
  }
  sources.restart_armed = false;
  simple(JF_JOB_RESTART);
}

static void sources_failed(const jf_task *task) {
  if (task->job == JF_JOB_SUBTITLE_SOURCES) {
    sources.loading = false;
    sources_warn(true, "The server did not list its plugins: %s", task->error);
    return;
  }
  for (size_t i = 0; i < sources.count; i++)
    sources.items[i].busy = false;
  sources.adding = false;
  if (strcmp(task->error, "Unauthorized") == 0)
    sources_warn(true, "Only an administrator can change subtitle sources");
  else
    sources_warn(true, "The server refused: %s", task->error);
}

static void sources_done(const jf_task *task) {
  switch (task->job) {
  case JF_JOB_SUBTITLE_SOURCES:
    sources.loading = false;
    sources.loaded = true;
    sources.count = 0;
    for (size_t i = 0; i < task->source_count && sources.count < SOURCES; i++) {
      const jf_subtitle_source *from = &task->sources[i];
      if (from->name == NULL || from->guid == NULL)
        continue;
      set_text(sources.items[sources.count].name, sizeof(sources.items[0].name),
               from->name);
      set_text(sources.items[sources.count].guid, sizeof(sources.items[0].guid),
               from->guid);
      set_text(sources.items[sources.count].version,
               sizeof(sources.items[0].version),
               from->version != NULL ? from->version : "");
      set_text(sources.items[sources.count].status,
               sizeof(sources.items[0].status),
               from->status != NULL ? from->status : "");
      sources.items[sources.count].busy =
          sources.installing[0] != '\0' &&
          strcmp(sources.installing, from->name) == 0 && from->status == NULL;
      sources.count++;
    }
    if (sources.installing[0] != '\0') {
      bool waiting = false;
      for (size_t i = 0; i < sources.count; i++)
        waiting |= sources.items[i].busy;
      if (!waiting) {
        sources.installing[0] = '\0';
        sources_warn(false, "Installed: it works once the server restarts");
      } else if (++sources.polls >= 30) {
        sources.installing[0] = '\0';
        sources_warn(true, "The server has not finished installing it");
      }
    }
    return;
  case JF_JOB_SOURCE_INSTALL:
    /* Installing happens in the background: the list says when it is done. */
    snprintf(sources.installing, sizeof(sources.installing), "%.95s", task->a);
    sources.polls = 0;
    sources.poll_at = now_ns();
    return;
  case JF_JOB_SOURCE_TOGGLE:
    sources.changed = true;
    sources_warn(false, "Changed: it takes effect once the server restarts");
    refresh_sources();
    return;
  case JF_JOB_ADD_REPOSITORY:
    sources.adding = false;
    sources_warn(false, "Added: subbuzz can be installed now");
    refresh_sources();
    return;
  case JF_JOB_RESTART:
    sources_warn(false, "The server is restarting; playback stops");
    return;
  default:
    return;
  }
}

/* Looks at the list while an install finishes. From pump. */
static void poll_sources(void) {
  if (sources.installing[0] == '\0' || sources.loading ||
      now_ns() < sources.poll_at)
    return;
  sources.poll_at = now_ns() + 1500000000ull;
  refresh_sources();
}

static void activate_source_row(size_t row) {
  if (row < sources.count)
    activate_source(row);
  else if (offer_repository() && row == repository_row())
    add_repository();
  else if (restart_pending() && row == restart_row())
    restart_server();
}

static void sources_key(uint32_t code) {
  size_t row = SIZE_MAX;
  for (size_t i = 0; i < source_rows(); i++)
    if (ui.focus == nav_child(nav_str("subtitle-sources"), nav_int(i)))
      row = i;
  if (jf_window_is_back_key(code))
    close_sources();
  else if (code == 103)
    nav_move(&ui, NAV_UP);
  else if (code == 108)
    nav_move(&ui, NAV_DOWN);
  else if (code == 28 || code == 96 || code == 352)
    activate_source_row(row);
}

static void close_subtitle_search(void) {
  search.open = false;
  subtitle_picker = true;
  nav_focus(&ui, picker_row_id(0));
}

static void download_result(size_t index) {
  if (search.phase != DOWNLOAD_NONE || index >= search.count)
    return;
  jf_task *task = request(JF_JOB_SUBTITLE_DOWNLOAD, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), playback_item_id);
  set_text(task->b, sizeof(task->b), search.results[index].id);
  jf_fetcher_start(&fetcher, task);
  search.phase = DOWNLOAD_FETCHING;
  search.error[0] = '\0';
  search.before_count = 0;
  for (int i = 0; i < KNOWN_TRACKS && track_key[i][0] != '\0'; i++)
    snprintf(search.before[search.before_count++], sizeof(search.before[0]),
             "%.79s", track_key[i]);
}

static void search_failed(const jf_task *task) {
  if (task->job == JF_JOB_SUBTITLE_SEARCH) {
    search.searching = false;
    /* The server answers 404 for a provider that is not there, 403 for a
     * user not allowed to manage subtitles. */
    search.error_is_warning = sources.loaded && sources_on() == 0;
    snprintf(search.error, sizeof(search.error), "%s",
             strcmp(task->error, "Unauthorized") == 0
                 ? "This user may not download subtitles"
             : search.error_is_warning
                 ? "No subtitle source is on: turn one on under Sources"
                 : "No subtitle source answered");
    return;
  }
  const bool poll = task->job == JF_JOB_PLAYBACK_INFO;
  if (poll) {
    /* One missed look is not the end: ask again at the next beat. */
    search.poll_in_flight = false;
    return;
  }
  search.phase = DOWNLOAD_NONE;
  search.error_is_warning = false;
  /* The failure's name is a word (see jf_api_execute). */
  snprintf(search.error, sizeof(search.error), "%s: %.32s",
           task->job == JF_JOB_SUBTITLE_CHECK
               ? "The download is listed but cannot be read"
           : strcmp(task->error, "Unauthorized") == 0
               ? "This user may not download subtitles"
               : "The download failed",
           task->error);
}

/* A look at the list while waiting for the download: the new track, once
 * there, is read back before anything changes. */
static void search_listed(const jf_playback_info *info) {
  search.poll_in_flight = false;
  if (search.phase != DOWNLOAD_LISTING)
    return;
  int embedded = 0;
  for (size_t i = 0; i < info->subtitle_count; i++) {
    const jf_media_stream *stream = &info->subtitles[i];
    char key[80];
    stream_key(stream, &embedded, key);
    bool known = false;
    for (int j = 0; j < search.before_count && !known; j++)
      known = strcmp(search.before[j], key) == 0;
    if (known || !stream->text)
      continue;
    jf_task *task = request(JF_JOB_SUBTITLE_CHECK, 0);
    if (task == NULL)
      return; /* the next look tries again */
    set_text(task->a, sizeof(task->a), playback_item_id);
    set_text(task->b, sizeof(task->b), info->media_source_id);
    task->start = (uint32_t)stream->index;
    jf_fetcher_start(&fetcher, task);
    search.phase = DOWNLOAD_CHECKING;
    return;
  }
  if (++search.polls >= DOWNLOAD_POLLS) {
    search.phase = DOWNLOAD_NONE;
    search.error_is_warning = false;
    snprintf(search.error, sizeof(search.error),
             "Downloaded, but the server has not listed it yet");
  }
}

/* Asks for the list while a download waits to be listed. From pump. */
static void poll_download(void) {
  if (search.phase != DOWNLOAD_LISTING || search.poll_in_flight ||
      now_ns() < search.poll_at)
    return;
  jf_task *info = request(JF_JOB_PLAYBACK_INFO, DOWNLOAD_POLL_TAG);
  if (info == NULL)
    return;
  set_text(info->a, sizeof(info->a), playback_item_id);
  jf_fetcher_start(&fetcher, info);
  search.poll_in_flight = true;
  search.poll_at = now_ns() + DOWNLOAD_POLL_NS;
}

static void search_done(const jf_task *task) {
  if (task->job == JF_JOB_SUBTITLE_CHECK) {
    /* Readable: the list that selects it, where use_playback_info finishes. */
    search.phase = DOWNLOAD_SELECTING;
    jf_task *info = request(JF_JOB_PLAYBACK_INFO, 0);
    if (info != NULL) {
      set_text(info->a, sizeof(info->a), playback_item_id);
      jf_fetcher_start(&fetcher, info);
    }
    return;
  }
  if (task->job == JF_JOB_SUBTITLE_DOWNLOAD) {
    search.phase = DOWNLOAD_LISTING;
    search.polls = 0;
    search.poll_at = now_ns();
    poll_download();
    return;
  }
  if (!search.searching || strcmp(task->a, playback_item_id) != 0 ||
      strcmp(task->b, search_languages[sub_style.search_language].code) != 0)
    return; /* an older language's */
  search.searching = false;
  search.count = 0;
  for (size_t i = 0; i < task->remote_count && search.count < SEARCH_RESULTS;
       i++) {
    const jf_remote_subtitle *r = &task->remote[i];
    set_text(search.results[search.count].id, sizeof(search.results[0].id),
             r->id);
    set_text(search.results[search.count].name, sizeof(search.results[0].name),
             r->name != NULL ? r->name : r->id);
    snprintf(search.results[search.count].detail,
             sizeof(search.results[0].detail), "%s  %s  %d downloads",
             r->provider != NULL ? r->provider : "",
             r->format != NULL ? r->format : "", r->downloads);
    search.count++;
  }
  search.error_is_warning = true;
  if (search.count == 0)
    snprintf(search.error, sizeof(search.error), "Nothing found in %s",
             search_languages[sub_style.search_language].name);
}

static void search_key(uint32_t code) {
  size_t row = SIZE_MAX;
  for (size_t i = 0; i < SEARCH_ROW_RESULTS + search.count; i++)
    if (ui.focus == search_row_id(i))
      row = i;
  const bool select = code == 28 || code == 96 || code == 352;
  if (jf_window_is_back_key(code))
    close_subtitle_search();
  else if (code == 103)
    nav_move(&ui, NAV_UP);
  else if (code == 108)
    nav_move(&ui, NAV_DOWN);
  else if ((select || code == 106) && row == SEARCH_ROW_SOURCES)
    open_sources();
  else if ((code == 105 || code == 106) && row == SEARCH_ROW_LANGUAGE) {
    sub_style.search_language =
        (sub_style.search_language + (code == 105 ? -1 : 1) +
         SEARCH_LANGUAGE_COUNT) %
        SEARCH_LANGUAGE_COUNT;
    save_preferences();
    start_search();
  } else if (select && row != SIZE_MAX && row >= SEARCH_ROW_RESULTS)
    download_result(row - SEARCH_ROW_RESULTS);
}

/* The picker takes every key while it is up. The subtitle key goes round the
 * tracks, and right on a track - the arrow - opens its appearance and timing.
 * Focus is the selection, so that is the track showing. OK on the first row
 * searches for more. */
static void picker_key(uint32_t code) {
  const bool on_search = ui.focus == picker_row_id(0);
  if ((code == 28 || code == 96 || code == 352) && on_search)
    open_subtitle_search();
  else if (jf_window_is_back_key(code) || code == 28 || code == 96 ||
           code == 352)
    close_subtitle_picker();
  else if (code == 103)
    nav_move(&ui, NAV_UP);
  else if (code == 108)
    nav_move(&ui, NAV_DOWN);
  else if (code == 106 && !on_search && jf_player_subtitle_current() >= 0)
    open_subtitle_settings();
  else if (code == KEY_SUBTITLE && !nav_move(&ui, NAV_DOWN))
    nav_focus(&ui, picker_row_id(track_row(-1)));
}

/* Next is the first chapter starting after here; back is the start of this
 * one, or of the one before when this one has only just begun. */
static void seek_chapter(int direction) {
  const int *starts = NULL;
  const int count = jf_player_chapters(&starts);
  const int position = jf_player_position();
  int target = direction > 0 ? -1 : 0;
  for (int i = 0; i < count; i++) {
    if (direction > 0 && starts[i] > position + 1000) {
      target = starts[i];
      break;
    }
    if (direction < 0 && starts[i] < position - 3000)
      target = starts[i];
  }
  if (target >= 0)
    jf_player_seek_to(target);
}

/* Straight from one episode into the next, with Back still going where it
 * went before. */
static void play_adjacent(const card *episode) {
  if (!episode->present)
    return;
  const card chosen = *episode;
  const screen_id back = playback_return_screen;
  const nav_id back_focus = playback_return_focus;
  report_leaving();
  jf_player_stop();
  start_playback(chosen.id, chosen.episode_title, chosen.series_id,
                 chosen.playback_position_ticks);
  playback_return_screen = back;
  playback_return_focus = back_focus;
}

static void activate_playback(size_t index) {
  playback_controls_until = now_ns() + PLAYBACK_CONTROLS_NS;
  switch (playback_buttons[index].action) {
  case ACTION_BACK_30:
    jf_player_seek(-30);
    break;
  case ACTION_BACK_10:
    jf_player_seek(-10);
    break;
  case ACTION_PAUSE:
    toggle_playback();
    break;
  case ACTION_FORWARD_10:
    jf_player_seek(10);
    break;
  case ACTION_FORWARD_30:
    jf_player_seek(30);
    break;
  case ACTION_PREVIOUS:
    play_adjacent(&previous_episode);
    break;
  case ACTION_NEXT:
    play_adjacent(&next_episode);
    break;
  case ACTION_CHAPTER_BACK:
    seek_chapter(-1);
    break;
  case ACTION_CHAPTER_NEXT:
    seek_chapter(1);
    break;
  case ACTION_SUBTITLES:
    open_subtitle_picker();
    break;
  case ACTION_AUDIO:
    if (jf_player_audio_count() > 1)
      jf_player_audio_select((jf_player_audio_current() + 1) %
                             jf_player_audio_count());
    break;
  }
}

/* The chrome is two rows: the buttons, and above them the seek bar. Up from the
 * bar hides it all, even paused; with it hidden, a key only brings it back,
 * except OK, which pauses, and Left and Right, which seek. */
/* Holding Left or Right on the seek bar scrubs: the bar moves, faster the
 * longer the key is held, and the seek happens once, on release - a seek per
 * key repeat would have the TV reopening the stream faster than it can. */
static bool scrubbing;
static int scrub_ms;
static uint64_t scrub_started_at, scrub_moved_at;

static void end_scrub(void) {
  if (!scrubbing)
    return;
  scrubbing = false;
  jf_player_seek_to(scrub_ms);
}

static void scrub(int direction, bool repeat) {
  const uint64_t now = now_ns();
  if (!repeat) {
    end_scrub();
    jf_player_seek(direction * 10);
    return;
  }
  if (!scrubbing) {
    scrubbing = true;
    scrub_ms = jf_player_position();
    scrub_started_at = scrub_moved_at = now;
  }
  /* Media seconds per second held: 20 at first, doubling every second held,
   * up to ten minutes a second. */
  const float held = (float)(now - scrub_started_at) / 1e9f;
  const float rate = minf(20.0f * powf(2.0f, held), 600.0f);
  scrub_ms += direction * (int)(rate * (float)(now - scrub_moved_at) / 1e6f);
  scrub_moved_at = now;
  const int duration = jf_player_duration();
  scrub_ms = scrub_ms < 0                          ? 0
             : duration > 0 && scrub_ms > duration ? duration
                                                   : scrub_ms;
}

static void playback_key(uint32_t code, bool repeat) {
  const bool shown = playback_chrome_wanted(now_ns());
  show_playback_chrome();
  const bool ok = code == 28 || code == 96 || code == 352;
  if (!shown) {
    if (ok)
      toggle_playback();
    if (code == 105 || code == 106) {
      nav_focus(&ui, seek_bar_id());
      scrub(code == 105 ? -1 : 1, repeat);
    }
    return;
  }
  switch (code) {
  case 103:
    /* Up past the seek bar hides the chrome; it comes back on play/pause. */
    if (!nav_move(&ui, NAV_UP)) {
      chrome_dismissed = true;
      nav_focus(&ui, 0);
    }
    break;
  case 108:
    nav_move(&ui, NAV_DOWN);
    break;
  case 105:
  case 106:
    if (ui.focus == seek_bar_id())
      scrub(code == 105 ? -1 : 1, repeat);
    else
      nav_move(&ui, code == 105 ? NAV_LEFT : NAV_RIGHT);
    break;
  default:
    if (ok)
      nav_activate(&ui);
    break;
  }
}

static void connect_server(void) {
  if (server_url[0] == '\0') {
    set_error("Enter a server URL first");
    return;
  }
  jf_task *task = request(JF_JOB_PROBE, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), server_url);
  connecting = true;
  jf_fetcher_start(&fetcher, task);
}

static void sign_in(void) {
  if (username[0] == '\0') {
    set_error("Enter a username");
    return;
  }
  jf_task *task = request(JF_JOB_LOGIN, 0);
  if (task == NULL)
    return;
  set_text(task->a, sizeof(task->a), username);
  set_text(task->b, sizeof(task->b), password);
  jf_fetcher_start(&fetcher, task);
  set_status("Signing in...");
}

static void toggle_animations(void) {
  animations_enabled = !animations_enabled;
  save_preferences();
  if (!animations_enabled) {
    animated_float_snap(&sidebar_slide, sidebar_open ? 1.0f : 0.0f);
  }
}

typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } direction;

/* Moves the focus; where there is nothing to move to, what that means on this
 * screen. */
static void move(direction where, bool repeat) {
  /* direction and nav_dir list the same four in the same order. */
  if (nav_move(&ui, (nav_dir)where))
    return;
  if (where == DIR_UP || where == DIR_DOWN)
    nav_step(&ui, where == DIR_DOWN ? 1 : -1);
  else if (sidebar_open)
    close_sidebar();
  /* A held Left stops at the edge rather than opening the sidebar. */
  else if (where == DIR_LEFT && !repeat && screen != SCREEN_LICENSES &&
           screen != SCREEN_LICENSE)
    open_sidebar();
}

static void navigate(uint32_t code, bool repeat) {
  if (jf_window_is_back_key(code)) {
    go_back();
    return;
  }
  switch (code) {
  case 103:
    move(DIR_UP, repeat);
    break;
  case 108:
    move(DIR_DOWN, repeat);
    break;
  case 105:
    move(DIR_LEFT, repeat);
    break;
  case 106:
    move(DIR_RIGHT, repeat);
    break;
  case 28:
  case 96:
  case 352:
    nav_activate(&ui);
    break;
  default:
    break;
  }
}

/* ------------------------------------------------------------------- input */

static void on_key(uint32_t code, bool pressed, bool repeat) {
  if (!pressed) {
    if (screen == SCREEN_PLAYBACK && (code == 105 || code == 106) &&
        scrubbing) {
      end_scrub();
      jf_window_frame_requested = true;
    }
    return;
  }
  jf_window_frame_requested = true;
  if (screen == SCREEN_PLAYBACK && subtitle_settings) {
    settings_key(code);
    return;
  }
  if (screen == SCREEN_PLAYBACK && search.open) {
    search_key(code);
    return;
  }
  if (screen == SCREEN_PLAYBACK && sources.open) {
    sources_key(code);
    return;
  }
  if (screen == SCREEN_PLAYBACK && subtitle_picker) {
    picker_key(code);
    return;
  }
  if (screen == SCREEN_PLAYBACK && code == KEY_SUBTITLE) {
    open_subtitle_picker();
    return;
  }
  if (jf_window_is_back_key(code)) {
    go_back();
    return;
  }
  if (code == 88) { /* F12 */
    capture_requested = true;
    return;
  }
  if (screen == SCREEN_PLAYBACK) {
    playback_key(code, repeat);
    return;
  }
  if (active_field != EDIT_NONE) {
    if (code == 1 || code == 158 || code == 28 || code == 96 || code == 352)
      end_edit();
    else if (code == 14)
      erase_text(1);
    return;
  }
  /* Blue button / F9: sign out, the only way back to the server screen once
   * credentials are stored. */
  if (code == 67 && screen != SCREEN_SERVER && screen != SCREEN_AUTH) {
    sign_out();
    return;
  }
  navigate(code, repeat);
}

static void move_cursor(jf_fixed x, jf_fixed y) {
  nav_pointer(&ui, (float)jf_fixed_to_int(x), (float)jf_fixed_to_int(y));
  if (screen == SCREEN_PLAYBACK)
    show_playback_chrome();
}

static void on_event(const jf_event *event) {
  /* Key releases do not change the UI, and neither does a pointer button going
   * up. */
  if (event->kind == JF_EVENT_POINTER_BUTTON) {
    if (event->button.pressed)
      jf_window_frame_requested = true;
  } else if (event->kind != JF_EVENT_KEY && event->kind != JF_EVENT_CLOSE) {
    jf_window_frame_requested = true;
  }

  switch (event->kind) {
  case JF_EVENT_KEY:
    on_key(event->key.code, event->key.pressed, event->key.repeat);
    break;
  case JF_EVENT_TEXT_COMMIT:
    append_text(event->text);
    break;
  case JF_EVENT_POINTER_ENTER:
  case JF_EVENT_POINTER_MOTION:
    move_cursor(event->pointer.x, event->pointer.y);
    break;
  case JF_EVENT_POINTER_LEAVE:
    nav_pointer_leave(&ui);
    break;
  case JF_EVENT_POINTER_BUTTON:
    if (event->button.pressed && event->button.button == 0x110)
      nav_click(&ui);
    break;
  case JF_EVENT_POINTER_AXIS: {
    if (event->axis.axis != 0)
      break;
    const float delta = (float)event->axis.value / 256.0f * 5.0f;
    nav_wheel(&ui, delta);
    break;
  }
  case JF_EVENT_CLOSE:
    jf_window_running = false;
    break;
  case JF_EVENT_RESIZED:
    jf_window_frame_requested = true;
    break;
  }
}

/* ------------------------------------------------------------------- views */

static void draw_heading_and_status(loom_context *ctx, float width,
                                    float height, float scale) {
  const float margin = 64 * scale;
  const char *heading = screen == SCREEN_QUICK ? "Quick Connect" : "";
  if (heading[0] != '\0')
    loom_label(ctx,
               (loom_rect){margin, 40 * scale, width - margin * 2, 50 * scale},
               NULL, heading, TEXT, 32 * scale);
  if (status_text[0] != '\0' && status_error)
    loom_label(ctx,
               (loom_rect){margin, height - 46 * scale, width - margin * 2,
                           32 * scale},
               NULL, status_text, status_error ? RED : DIM, 19 * scale);
}

/* Controls declare themselves to nav and draw; the flags they return say what
 * happened to them this frame. */
static unsigned draw_button(loom_context *ctx, loom_rect rect,
                            const char *label, nav_id id, float scale) {
  const unsigned flags = nav_item(&ui, id, rect, ctx->viewport);
  const bool focused = flags & NAV_FOCUSED;
  loom_fill(ctx, rect, NULL, flags & NAV_HOVERED ? HOT : CARD, 12 * scale);
  loom_stroke(ctx, rect, NULL, focused ? ACCENT : BORDER,
              focused ? 4 * scale : 2 * scale, 12 * scale);
  loom_label(ctx,
             (loom_rect){rect.x + 24 * scale,
                         rect.y + (rect.h - 30 * scale) / 2,
                         rect.w - 48 * scale, 38 * scale},
             &rect, label, TEXT, 25 * scale);
  return flags;
}

static unsigned draw_action_button(loom_context *ctx, loom_rect rect,
                                   const char *label, nav_id id, float scale) {
  const unsigned flags = nav_item(&ui, id, rect, ctx->viewport);
  const bool focused = flags & NAV_FOCUSED;
  loom_fill(ctx, rect, NULL, flags & NAV_HOVERED ? SELECTED : ACCENT,
            12 * scale);
  loom_stroke(ctx, rect, NULL, focused ? WHITE : ACCENT,
              focused ? 4 * scale : 2 * scale, 12 * scale);
  loom_label(ctx,
             (loom_rect){rect.x + 24 * scale,
                         rect.y + (rect.h - 30 * scale) / 2,
                         rect.w - 48 * scale, 38 * scale},
             &rect, label, WHITE, 25 * scale);
  return flags;
}

/* Activating a field starts editing it. */
static void draw_field(loom_context *ctx, loom_rect rect, const char *label,
                       edit_field which, nav_id id, float scale) {
  const unsigned flags = nav_item(&ui, id, rect, ctx->viewport);
  const bool editing = active_field == which;
  const bool lit = (flags & NAV_FOCUSED) || editing;
  loom_fill(ctx, rect, NULL,
            editing               ? SELECTED
            : flags & NAV_HOVERED ? HOT
                                  : CARD,
            10 * scale);
  loom_stroke(ctx, rect, NULL, lit ? ACCENT : BORDER,
              lit ? 4 * scale : 2 * scale, 10 * scale);

  size_t capacity = 0;
  const char *target = field_buffer(which, &capacity);
  const char *contents = target;
  if (which == EDIT_PASSWORD) {
    const size_t length = min_size(strlen(password), sizeof(password_mask) - 1);
    memset(password_mask, '*', length);
    password_mask[length] = '\0';
    contents = password_mask;
  }
  const bool empty = contents == NULL || contents[0] == '\0';
  loom_label(ctx,
             (loom_rect){rect.x + 22 * scale, rect.y + 17 * scale,
                         rect.w - 44 * scale, 38 * scale},
             &rect, empty ? label : contents, empty ? DIM : TEXT, 24 * scale);
  if (flags & NAV_ACTIVATED)
    begin_edit(which, rect);
}

/* Trim `text` to `width`, adding an ellipsis. Measured against the real glyph
 * advances, and backing up over whole UTF-8 sequences so a cut never lands
 * inside one. */
static const char *ellipsize(const char *text, float width, float size) {
  if (jf_renderer_measure(renderer, text, size) <= width)
    return text;
  const float dots = jf_renderer_measure(renderer, "...", size);
  char probe[256];
  size_t take = min_size(strlen(text), sizeof(probe) - 1);
  while (take > 1) {
    memcpy(probe, text, take);
    probe[take] = '\0';
    if (jf_renderer_measure(renderer, probe, size) <= width - dots)
      break;
    take--;
    while (take > 0 && ((unsigned char)text[take] & 0xc0) == 0x80)
      take--;
  }
  memcpy(probe, text, take);
  probe[take] = '\0';
  return fmt("%s...", probe);
}

/* The card whose poster stands for this one. Seasons may omit both SeriesId and
 * the inherited image tag; the show details carry the fallback and stay loaded
 * on the season page. */
static const card *poster_card(const card *source) {
  return card_is(source, "Season") && source->thumbnail_tag[0] == '\0' ? &detail
                                                                       : source;
}

static const poster_slot *card_poster(const card *source) {
  const card *owner = poster_card(source);
  return poster(owner->poster_id, owner->poster_tag);
}

/* The card's blurhash, if its atlas row still holds it. */
static bool draw_blur(loom_context *ctx, loom_rect rect, loom_rect clip,
                      const card *source, float radius) {
  if (source->blur_stamp == 0 ||
      blur_rows_stamp[source->blur_row] != source->blur_stamp)
    return false;
  const float width = JF_BLUR_COLUMNS * JF_BLUR_CELL,
              height = BLUR_ATLAS_ROWS * JF_BLUR_CELL;
  const float x = (float)(source->blur_column * JF_BLUR_CELL);
  const float y = (float)(source->blur_row * JF_BLUR_CELL);
  /* Half a texel in, so linear filtering never reaches the neighbouring cell.
   */
  const float uv[4] = {(x + 0.5f) / width, (y + 0.5f) / height,
                       (x + source->blur_width - 0.5f) / width,
                       (y + source->blur_height - 0.5f) / height};
  loom_textured(ctx, rect, &clip, blur_atlas, uv, WHITE, radius);
  return true;
}

static void draw_watch_badge(loom_context *ctx, loom_rect art, loom_rect clip,
                             const card *source, float scale) {
  const bool episode = card_is(source, "Episode");
  if (!episode && !card_is(source, "Season") && !card_is(source, "Series"))
    return;
  if (episode && !source->played)
    return;
  uint32_t count = 0;
  if (!episode) {
    count = source->has_remaining ? source->remaining : 0;
    if (count == 0)
      return;
  }
  const char *label =
      episode ? "\xe2\x9c\x93" : fmt("%u", count); /* U+2713 CHECK MARK */
  const float size = 22 * scale;
  const float measured = jf_renderer_measure(renderer, label, size);
  const float w = maxf(36 * scale, measured + 16 * scale);
  const loom_rect badge = {art.x + art.w - w - 8 * scale, art.y + 8 * scale, w,
                           36 * scale};
  loom_fill(ctx, badge, &clip, ACCENT, 6 * scale);
  loom_label(ctx,
             (loom_rect){badge.x + (badge.w - measured) / 2,
                         badge.y + 3 * scale, badge.w, badge.h},
             &clip, label, WHITE, size);
}

/* One poster card. Returns true when a pointer click landed on it, so the
 * caller decides what a click means: a card opens a grid on one screen and
 * details on another. */
/* `flags` are the card's from nav_item. */
static void draw_card(loom_context *ctx, loom_rect rect, loom_rect clip,
                      const card *source, unsigned flags, float scale) {
  const loom_rect visible = loom_intersect(rect, clip);
  if (visible.w <= 0 || visible.h <= 0)
    return;
  const bool hot = flags & NAV_HOVERED;
  const bool focused = flags & NAV_FOCUSED;
  const loom_rect art = {rect.x, rect.y, rect.w, rect.h - 78 * scale};

  const poster_slot *slot = card_poster(source);
  if (slot != NULL) {
    float uv[4];
    cover_uv(slot, art, uv);
    loom_textured(ctx, art, &clip, slot->texture, uv, WHITE, 10 * scale);
  } else if (!draw_blur(ctx, art, clip, poster_card(source), 10 * scale)) {
    loom_fill(ctx, art, &clip, hot ? HOT : CARD, 10 * scale);
    if (artwork_loading(source->poster_id, source->poster_tag, JF_IMAGE_PRIMARY,
                        POSTER_W, POSTER_H))
      draw_spinner(ctx, art.x + art.w / 2, art.y + art.h / 2, 18 * scale, &clip,
                   scale);
  }
  draw_watch_badge(ctx, art, clip, source, scale);

  if (source->progress > 1) {
    const loom_rect bar = {art.x + 5 * scale, art.y + art.h - 10 * scale,
                           art.w - 10 * scale, 6 * scale};
    loom_fill(ctx, bar, &clip, BORDER, 3 * scale);
    loom_fill(ctx,
              (loom_rect){bar.x, bar.y,
                          bar.w * minf(source->progress, 100) / 100, bar.h},
              &clip, ACCENT, 3 * scale);
  }
  const loom_rect text_clip = loom_intersect(rect, clip);
  loom_label(
      ctx, (loom_rect){rect.x, art.y + art.h + 12 * scale, rect.w, 32 * scale},
      &text_clip, ellipsize(source->title, rect.w, 21 * scale),
      focused ? TEXT : DIM, 21 * scale);
  loom_label(
      ctx, (loom_rect){rect.x, art.y + art.h + 44 * scale, rect.w, 28 * scale},
      &text_clip, ellipsize(source->subtitle, rect.w, 17 * scale), DIM,
      17 * scale);
}

static void draw_metadata(loom_context *ctx, loom_rect rect,
                          const loom_rect *clip, const char *prefix,
                          const char *rating, float size,
                          const loom_color color) {
  float x = rect.x;
  if (prefix != NULL && prefix[0] != '\0') {
    loom_label(ctx, rect, clip, prefix, color, size);
    x += jf_renderer_measure(renderer, prefix, size) + size;
  }
  if (rating != NULL && rating[0] != '\0') {
    static const char star[] = "\xe2\x98\x85"; /* U+2605 BLACK STAR */
    loom_label(ctx, (loom_rect){x, rect.y, size * 1.5f, rect.h}, clip, star,
               YELLOW, size);
    x += jf_renderer_measure(renderer, star, size) + size * 0.3f;
    loom_label(ctx,
               (loom_rect){x, rect.y, maxf(0, rect.x + rect.w - x), rect.h},
               clip, rating, TEXT, size);
  }
}

/* Greedy word wrap against the real glyph advances. The renderer clips a label
 * to its rect but does not break it, so an overview needs splitting before it
 * becomes draw commands. */
static void draw_wrapped(loom_context *ctx, loom_rect rect, const char *text,
                         float size) {
  float y = rect.y;
  const char *rest = text;
  char line[512];
  while (*rest != '\0' && y + size * 1.4f <= rect.y + rect.h) {
    const char *newline = strchr(rest, '\n');
    size_t limit = newline != NULL ? (size_t)(newline - rest) : strlen(rest);
    if (limit == 0) {
      rest++;
      y += size * 1.45f;
      continue;
    }
    if (limit > sizeof(line) - 1)
      limit = sizeof(line) - 1;
    size_t take = limit;
    for (;;) {
      memcpy(line, rest, take);
      line[take] = '\0';
      if (jf_renderer_measure(renderer, line, size) <= rect.w)
        break;
      const char *space = NULL;
      for (size_t i = take; i > 0; i--)
        if (rest[i - 1] == ' ') {
          space = rest + i - 1;
          break;
        }
      if (space == NULL)
        break;
      take = (size_t)(space - rest);
    }
    if (take == 0)
      break;
    memcpy(line, rest, take);
    line[take] = '\0';
    loom_label(ctx, (loom_rect){rect.x, y, rect.w, size * 1.4f}, &rect,
               fmt("%s", line), TEXT, size);
    rest += take;
    if (take == limit && newline != NULL)
      rest++;
    while (*rest == ' ' || *rest == '\t' || *rest == '\r')
      rest++;
    y += size * 1.45f;
  }
}

static void draw_server(loom_context *ctx, float width, float scale) {
  const loom_rect panel = {390 * scale, 80 * scale, width - 780 * scale,
                           920 * scale};
  loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
  const float x = panel.x + 54 * scale;
  const float w = panel.w - 108 * scale;
  draw_field(ctx, (loom_rect){x, panel.y + 48 * scale, w, 70 * scale},
             "Server URL", EDIT_URL, nav_str("server.url"), scale);
  if (draw_action_button(ctx,
                         (loom_rect){x, panel.y + 142 * scale, w, 70 * scale},
                         "Connect", nav_str("server.connect"), scale) &
      NAV_ACTIVATED)
    connect_server();

  float y = panel.y + 236 * scale;
  size_t chosen = discovered_count;
  for (size_t index = 0; index < discovered_count; index++) {
    const loom_rect rect = {x, y, w, 70 * scale};
    const unsigned flags =
        nav_item(&ui, nav_str(discovered[index].address), rect, panel);
    const bool focused = flags & NAV_FOCUSED;
    if (flags & NAV_ACTIVATED)
      chosen = index;
    loom_fill(ctx, rect, &panel, flags & NAV_HOVERED ? HOT : CARD, 13 * scale);
    loom_stroke(ctx, rect, &panel, focused ? ACCENT : BORDER,
                focused ? 4 * scale : 2 * scale, 13 * scale);
    loom_fill(ctx,
              (loom_rect){rect.x + 18 * scale, rect.y + 14 * scale, 40 * scale,
                          40 * scale},
              &rect, ACCENT, 22 * scale);
    loom_label(ctx,
               (loom_rect){rect.x + 78 * scale, rect.y + 7 * scale,
                           w - 96 * scale, 30 * scale},
               &rect, discovered[index].name, TEXT, 26 * scale);
    loom_label(ctx,
               (loom_rect){rect.x + 78 * scale, rect.y + 38 * scale,
                           w - 96 * scale, 24 * scale},
               &rect, discovered[index].address, DIM, 20 * scale);
    y += 80 * scale;
  }
  if (draw_button(ctx, (loom_rect){x, y + 8 * scale, w, 62 * scale},
                  "Search again", nav_str("server.search"), scale) &
      NAV_ACTIVATED)
    restart_discovery();
  else if (chosen < discovered_count)
    use_server(discovered[chosen].address, discovered[chosen].name);
  if (discovering || connecting)
    draw_spinner(ctx, panel.x + panel.w - 46 * scale,
                 panel.y + panel.h - 46 * scale, 18 * scale, &panel, scale);
  if (connecting) {
    const loom_rect modal = {width / 2 - 190 * scale, 440 * scale, 380 * scale,
                             150 * scale};
    loom_fill(ctx, modal, NULL, CARD, 16 * scale);
    loom_stroke(ctx, modal, NULL, ACCENT, 2 * scale, 16 * scale);
    draw_spinner(ctx, modal.x + 48 * scale, modal.y + modal.h / 2, 16 * scale,
                 &modal, scale);
    loom_label(ctx,
               (loom_rect){modal.x + 90 * scale, modal.y + 54 * scale,
                           modal.w - 116 * scale, 42 * scale},
               &modal, "Connecting...", TEXT, 26 * scale);
  }
}

static void draw_auth(loom_context *ctx, float width, float scale) {
  const loom_rect panel = {430 * scale, 180 * scale, width - 860 * scale,
                           680 * scale};
  loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
  const float x = panel.x + 64 * scale;
  const float w = panel.w - 128 * scale;
  loom_label(ctx, (loom_rect){x, panel.y + 38 * scale, w, 50 * scale}, &panel,
             server_name, TEXT, 34 * scale);
  loom_label(ctx, (loom_rect){x, panel.y + 92 * scale, w, 32 * scale}, &panel,
             session.url, DIM, 20 * scale);
  draw_field(ctx, (loom_rect){x, panel.y + 180 * scale, w, 70 * scale},
             "Username", EDIT_USERNAME, nav_str("auth.username"), scale);
  draw_field(ctx, (loom_rect){x, panel.y + 320 * scale, w, 70 * scale},
             "Password", EDIT_PASSWORD, nav_str("auth.password"), scale);
  if (draw_action_button(
          ctx, (loom_rect){x, panel.y + 450 * scale, 280 * scale, 72 * scale},
          "Sign in", nav_str("auth.sign_in"), scale) &
      NAV_ACTIVATED)
    sign_in();
  if (draw_action_button(ctx,
                         (loom_rect){x + 310 * scale, panel.y + 450 * scale,
                                     330 * scale, 72 * scale},
                         "Quick Connect", nav_str("auth.quick"), scale) &
      NAV_ACTIVATED) {
    simple(JF_JOB_QUICK_INITIATE);
    set_status("Requesting a Quick Connect code...");
  }
}

static void draw_quick(loom_context *ctx, float width, float scale) {
  const loom_rect panel = {470 * scale, 200 * scale, width - 940 * scale,
                           640 * scale};
  loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 70 * scale, panel.y + 50 * scale,
                         panel.w - 140 * scale, 42 * scale},
             &panel, "Enter this code in a signed-in Jellyfin client", TEXT,
             25 * scale);
  const loom_rect code = {panel.x + 150 * scale, panel.y + 140 * scale,
                          panel.w - 300 * scale, 160 * scale};
  loom_fill(ctx, code, &panel, CARD, 15 * scale);
  loom_stroke(ctx, code, &panel, ACCENT, 3 * scale, 15 * scale);
  const char *text = quick_code[0] != '\0' ? quick_code : "------";
  /* Centred by measuring, because the code is six digits in a proportional font
   * and eyeballing an offset is wrong at every scale. */
  const float size = 64 * scale;
  const float half = jf_renderer_measure(renderer, text, size) / 2;
  loom_label(ctx,
             (loom_rect){code.x + code.w / 2 - half, code.y + 46 * scale,
                         code.w, 80 * scale},
             &code, text, ACCENT, size);
  loom_label(ctx,
             (loom_rect){panel.x + 70 * scale, panel.y + 340 * scale,
                         panel.w - 140 * scale, 34 * scale},
             &panel, "Approval is polled every two seconds.", DIM, 20 * scale);
  if (draw_button(ctx,
                  (loom_rect){panel.x + 150 * scale, panel.y + 440 * scale,
                              260 * scale, 72 * scale},
                  "Cancel", nav_str("quick.cancel"), scale) &
      NAV_ACTIVATED)
    go_back();
}

/* A horizontal row of poster cards. Only the focused row scrolls; a TV row
 * holds few enough items that the whole row is one strip with an offset. */
/* Returns the card activated in this row, if any. A screen with no focus yet
 * starts on its first card when `starts_here`. */
static const card *draw_row(loom_context *ctx, const item_row *row, size_t id,
                            float top, float width, loom_rect clip,
                            bool starts_here, float scale) {
  const float margin = 64 * scale;
  const float card_w = (row->wide ? 356.0f : 200.0f) * scale;
  const float card_h = (row->wide ? 278.0f : 378.0f) * scale;
  const float gap = 26 * scale;
  const loom_rect strip = {margin, top + 46 * scale, width - margin * 2,
                           card_h};
  const loom_rect title = {margin, top, 600 * scale, 36 * scale};

  if (row->count == 0) {
    loom_label(ctx, title, &clip, row->title, DIM, 25 * scale);
    if (row->loading)
      draw_spinner(ctx, strip.x + 30 * scale, strip.y + 30 * scale, 14 * scale,
                   &clip, scale);
    else
      loom_label(
          ctx,
          (loom_rect){margin, strip.y + 30 * scale, 700 * scale, 32 * scale},
          &clip, "Nothing here", DIM, 20 * scale);
    return NULL;
  }

  /* The strip keeps the cards either side of the focused one fully in view,
   * moving a whole card at a time, so one predecessor starts beyond the left
   * edge and exits through the viewport fade. The last card stops at the
   * strip's right edge rather than a whole slot in. */
  const float step = card_w + gap;
  const float extent = (float)row->count * step - gap;
  const float strip_scroll = nav_scroll_begin(
      &ui, nav_int(id), strip, LOOM_HORIZONTAL, extent, step, step);
  const size_t first = dec((size_t)(strip_scroll / step));
  const loom_mask outer = ctx->mask;
  loom_fade(ctx, LOOM_HORIZONTAL, ctx->viewport.x,
            ctx->viewport.x + ctx->viewport.w, strip_scroll,
            maxf(0, extent - strip.w), 86 * scale);
  const card *chosen = NULL;
  bool row_focused = false;
  for (size_t index = first; index < row->count; index++) {
    const float x = strip.x - strip_scroll + (float)index * step;
    if (x >= width)
      break;
    const loom_rect rect = {x, strip.y, card_w, card_h};
    if (index == 0 && starts_here)
      nav_default(&ui);
    const unsigned flags =
        nav_item(&ui, nav_str(row->cards[index].id), rect, clip);
    if (flags & NAV_FOCUSED) {
      row_focused = true;
      nav_ring(&ui, (loom_rect){rect.x, rect.y, rect.w, rect.h - 78 * scale},
               12 * scale);
    }
    if (flags & NAV_ACTIVATED)
      chosen = &row->cards[index];
    draw_card(ctx, rect, clip, &row->cards[index], flags, scale);
  }
  nav_scroll_end(&ui);
  ctx->mask = outer;
  loom_label(ctx, title, &clip, row->title, row_focused ? TEXT : DIM,
             25 * scale);
  return chosen;
}

/* As the server's home screen: a row with nothing in it is left out, and a
 * Recently Added row shows once it has something. */
static bool row_shown(size_t id) {
  return id == ROW_LIBRARIES || rows[id].count > 0 ||
         (id < ROW_LATEST && rows[id].loading);
}

/* Still on its first load: a refresh keeps the cards it had. */
static bool row_arriving(size_t id) {
  return rows[id].loading && rows[id].count == 0;
}

static void draw_home(loom_context *ctx, float width, float height,
                      float scale) {
  if (row_arriving(ROW_LIBRARIES)) {
    draw_spinner(ctx, width / 2, height / 2, 26 * scale, NULL, scale);
    return;
  }
  /* Title, cards, and a gap: the libraries' banners are shorter. */
  float row_height[ROW_COUNT], extent = 0;
  for (size_t id = 0; id < ROW_COUNT; id++) {
    row_height[id] = row_shown(id) ? (rows[id].wide ? 370 : 470) * scale : 0;
    extent += row_height[id];
  }
  const float fade_height = 78 * scale;
  /* A card is revealed with its row's title clear of the fade above it. */
  const float scroll =
      nav_scroll_begin(&ui, nav_str("home"), (loom_rect){0, 0, width, height},
                       LOOM_VERTICAL, extent, fade_height + 46 * scale, 0);
  /* Nothing takes focus until the rows to continue from are in, or it would
   * start on a library. */
  const bool inert = ui.inert;
  nav_set_inert(&ui,
                inert || row_arriving(ROW_RESUME) || row_arriving(ROW_NEXT_UP));
  const size_t start = rows[ROW_RESUME].count > 0    ? ROW_RESUME
                       : rows[ROW_NEXT_UP].count > 0 ? ROW_NEXT_UP
                                                     : ROW_LIBRARIES;
  const loom_mask outer = ctx->mask;
  loom_fade(ctx, LOOM_VERTICAL, 0, height, scroll, maxf(0, extent - height),
            fade_height);
  const card *chosen = NULL;
  size_t chosen_row = 0;
  float top = -scroll;
  for (size_t id = 0; id < ROW_COUNT; id++) {
    if (!row_shown(id))
      continue;
    const card *activated = draw_row(ctx, &rows[id], id, top, width,
                                     ctx->viewport, id == start, scale);
    top += row_height[id];
    if (activated != NULL) {
      chosen = activated;
      chosen_row = id;
    }
  }
  ctx->mask = outer;
  nav_set_inert(&ui, inert);
  nav_scroll_end(&ui);
  if (chosen != NULL) {
    const card copy = *chosen;
    push();
    forget_scroll(copy.id);
    if (chosen_row == ROW_LIBRARIES)
      open_grid(&copy);
    else
      open_details(&copy);
  }
}

static void draw_grid(loom_context *ctx, float width, float height,
                      float scale) {
  const float margin = 64 * scale;
  const float card_w = 200 * scale;
  const float gap = 26 * scale;
  const loom_rect grid_rect = {margin, 120 * scale, width - margin * 2,
                               height - 190 * scale};
  size_t columns = (size_t)((grid_rect.w + gap) / (card_w + gap));
  if (columns < 1)
    columns = 1;
  const float row_height = 404 * scale;

  if (grid_total == 0) {
    loom_label(ctx,
               (loom_rect){margin, 40 * scale, width - margin * 2, 50 * scale},
               NULL, grid_title, TEXT, 32 * scale);
    if (grid_loading)
      draw_spinner(ctx, width / 2, height / 2, 24 * scale, NULL, scale);
    else
      loom_label(ctx,
                 (loom_rect){margin, grid_rect.y + 40 * scale, 800 * scale,
                             36 * scale},
                 NULL, "Nothing here", DIM, 24 * scale);
    return;
  }

  const size_t row_count = (grid_total + columns - 1) / columns;
  const float extent = (float)row_count * row_height;
  const float max_scroll = maxf(0, extent - grid_rect.h);
  /* Keyed by the library, so Back into it finds its place. */
  const float scroll = nav_scroll_begin(&ui, nav_str(grid_parent), grid_rect,
                                        LOOM_VERTICAL, extent, 0, 0);
  const loom_virtual_list list =
      loom_virtual_list_init(grid_rect, row_count, row_height, scroll);
  const loom_mask outer = ctx->mask;
  loom_fade(ctx, LOOM_VERTICAL, 0, height, scroll, max_scroll, 72 * scale);
  loom_label(
      ctx,
      (loom_rect){margin, 40 * scale - scroll, width - margin * 2, 50 * scale},
      NULL, grid_title, TEXT, 32 * scale);
  /* Grid padding reserves room for the first and last fully visible rows. It is
   * not a scissor: neighbouring rows extend into that margin and disappear at
   * the viewport fade. */
  const loom_rect content = ctx->viewport;

  const card *chosen = NULL;
  /* A row either side of the view, so there is always a row to move into. */
  const size_t first = dec(list.first);
  const size_t last = min_size(list.last + 1, row_count);
  for (size_t row_index = first; row_index < last; row_index++) {
    const loom_rect row_rect = loom_virtual_list_item(&list, row_index);
    for (size_t column = 0; column < columns; column++) {
      const size_t index = row_index * columns + column;
      if (index >= grid_total)
        break;
      const loom_rect rect = {row_rect.x + (float)column * (card_w + gap),
                              row_rect.y, card_w, row_height - 26 * scale};
      const card *source = grid_card(index);
      const unsigned flags = nav_item(&ui, nav_int(index), rect, content);
      if (flags & NAV_FOCUSED)
        nav_ring(&ui, (loom_rect){rect.x, rect.y, rect.w, rect.h - 78 * scale},
                 12 * scale);
      if ((flags & NAV_ACTIVATED) && source != NULL)
        chosen = source;
      draw_card(ctx, rect, content, source != NULL ? source : &blank_card,
                flags, scale);
    }
    /* One request per visible row is enough to walk the window forward
     * while scrolling with the pointer. */
    request_page((uint32_t)min_size(row_index * columns, dec(grid_total)));
  }
  nav_scroll_end(&ui);
  ctx->mask = outer;

  const loom_rect track = {grid_rect.x + grid_rect.w, grid_rect.y, 4 * scale,
                           grid_rect.h};
  if (max_scroll > 0) {
    const float thumb_h = maxf(40 * scale, track.h * grid_rect.h / extent);
    loom_fill(ctx, track, NULL, BORDER, track.w / 2);
    loom_fill(ctx,
              (loom_rect){track.x,
                          track.y + (track.h - thumb_h) * (scroll / max_scroll),
                          track.w, thumb_h},
              NULL, ACCENT, track.w / 2);
  }
  if (chosen != NULL) {
    const card copy = *chosen;
    push();
    forget_scroll(copy.id);
    open_details(&copy);
  }
}

/* The codec, and a warning when the TV cannot decode it: the server has to
 * transcode, which delays the start and every seek, and without a hardware
 * decoder for it may not keep up at all. */
static void draw_video_path(loom_context *ctx, loom_rect rect, float scale) {
  if (!detail_has_video)
    return;
  /* loom keeps the pointer until the frame is drawn. */
  static char line[192];
  const char *label = jf_codec_label(jf_codec_from_name(detail_video.codec));
  int n = snprintf(
      line, sizeof(line), "%s%s%s", label != NULL ? label : detail_video.codec,
      detail_video.profile[0] != '\0' ? " " : "", detail_video.profile);
  if (detail_video.height != 0 && n > 0 && (size_t)n < sizeof(line))
    n += snprintf(line + n, sizeof(line) - (size_t)n, " \xc2\xb7 %up",
                  detail_video.height);
  const jf_video_path path =
      jf_video_path_for(&detail_video, tv_decoders(), &server_transcode);
  const char *warning = path == JF_SERVER_HARDWARE
                            ? " \xc2\xb7 Server sided hardware transcode"
                            : " \xc2\xb7 Server sided software transcode";
  if (path != JF_DIRECT && n > 0 && (size_t)n < sizeof(line))
    snprintf(line + n, sizeof(line) - (size_t)n, "%s", warning);
  loom_label(ctx, rect, NULL, line,
             path == JF_DIRECT            ? DIM
             : path == JF_SERVER_HARDWARE ? YELLOW
                                          : RED,
             22 * scale);
}

static void draw_details(loom_context *ctx, float width, float scale) {
  const float margin = 64 * scale;
  const loom_rect art = {margin, margin, 320 * scale, 480 * scale};
  const poster_slot *slot = poster(detail.poster_id, detail.poster_tag);
  if (slot != NULL) {
    float uv[4];
    cover_uv(slot, art, uv);
    loom_textured(ctx, art, NULL, slot->texture, uv, WHITE, 16 * scale);
  } else if (!draw_blur(ctx, art, ctx->viewport, &detail, 16 * scale)) {
    loom_fill(ctx, art, NULL, CARD, 16 * scale);
    if (artwork_loading(detail.poster_id, detail.poster_tag, JF_IMAGE_PRIMARY,
                        POSTER_W, POSTER_H))
      draw_spinner(ctx, art.x + art.w / 2, art.y + art.h / 2, 20 * scale, &art,
                   scale);
  }
  draw_watch_badge(ctx, art, ctx->viewport, &detail, scale);

  const float x = art.x + art.w + 60 * scale;
  const float w = width - x - margin;
  loom_label(ctx, (loom_rect){x, margin, w, 72 * scale}, NULL,
             ellipsize(detail.title, w, 52 * scale), TEXT, 52 * scale);
  loom_label(ctx, (loom_rect){x, 150 * scale, w, 34 * scale}, NULL,
             detail.subtitle, TEXT, 24 * scale);
  draw_metadata(ctx, (loom_rect){x, 195 * scale, w, 34 * scale}, NULL,
                detail.official_rating, detail.rating, 20 * scale, DIM);
  draw_wrapped(ctx, (loom_rect){x, 255 * scale, w, 180 * scale},
               detail.overview, 22 * scale);

  if (card_is(&detail, "Series")) {
    if (seasons_row.count == 0) {
      loom_label(ctx, (loom_rect){x, 480 * scale, w, 34 * scale}, NULL,
                 seasons_row.loading ? "Loading seasons..." : "No seasons", DIM,
                 22 * scale);
      return;
    }
    loom_label(ctx, (loom_rect){x, 470 * scale, w, 34 * scale}, NULL, "Seasons",
               TEXT, 26 * scale);
    /* A carousel like the home rows, entering on the first season not yet
     * watched. */
    const float step = 226 * scale;
    const float gap = 26 * scale;
    const float extent = (float)seasons_row.count * step - gap;
    const loom_rect strip = {x, 524 * scale, w, 378 * scale};
    const float scroll = nav_scroll_begin(&ui, nav_str(detail.id), strip,
                                          LOOM_HORIZONTAL, extent, step, step);
    /* The poster ends 60 px left of the strip; a season leaving the strip
     * fades out in that gap rather than over it. */
    const loom_rect clip = {x - 60 * scale, 0, width - x + 60 * scale,
                            ctx->viewport.h};
    const loom_mask outer = ctx->mask;
    loom_fade(ctx, LOOM_HORIZONTAL, clip.x, width, scroll,
              maxf(0, extent - strip.w), 60 * scale);
    const size_t unfinished =
        first_unfinished(seasons_row.cards, seasons_row.count);
    const card *chosen = NULL;
    for (size_t index = 0; index < seasons_row.count; index++) {
      const loom_rect rect = {x - scroll + (float)index * step, strip.y,
                              200 * scale, strip.h};
      if (index == unfinished)
        nav_default(&ui);
      const unsigned flags =
          nav_item(&ui, nav_str(seasons_row.cards[index].id), rect, clip);
      if (flags & NAV_FOCUSED)
        nav_ring(&ui, (loom_rect){rect.x, rect.y, rect.w, rect.h - 78 * scale},
                 12 * scale);
      if (flags & NAV_ACTIVATED)
        chosen = &seasons_row.cards[index];
      draw_card(ctx, rect, clip, &seasons_row.cards[index], flags, scale);
    }
    nav_scroll_end(&ui);
    ctx->mask = outer;
    if (chosen != NULL) {
      const card copy = *chosen;
      push();
      forget_scroll(copy.id);
      open_season(&copy);
    }
    return;
  }

  draw_video_path(ctx, (loom_rect){x, 480 * scale, w, 34 * scale}, scale);
  if (draw_button(ctx, (loom_rect){x, 570 * scale, 250 * scale, 74 * scale},
                  "Play", nav_str("details.play"), scale) &
      NAV_ACTIVATED)
    start_playback(detail.id, detail.title, detail.series_id,
                   detail.playback_position_ticks);
  if (draw_button(
          ctx,
          (loom_rect){x + 280 * scale, 570 * scale, 250 * scale, 74 * scale},
          detail.played ? JF_ICON_CHECKED "  Played"
                        : JF_ICON_UNCHECKED "  Played",
          nav_str("details.played"), scale) &
      NAV_ACTIVATED) {
    jf_task *task = request(JF_JOB_MARK_PLAYED, 0);
    if (task != NULL) {
      set_text(task->a, sizeof(task->a), detail.id);
      task->start = !detail.played;
      jf_fetcher_start(&fetcher, task);
    }
  }
}

/* As card_from would make it: a show or season with nothing left is watched. */
static void update_card(card *c, const jf_user_data *item) {
  if (strcmp(c->id, item->item_id) != 0)
    return;
  const bool folder = card_is(c, "Series") || card_is(c, "Season");
  c->played = item->played || (folder && item->has_unplayed_item_count &&
                               item->unplayed_item_count == 0);
  c->playback_position_ticks = item->position_ticks;
  c->progress =
      item->has_played_percentage ? (float)item->played_percentage : 0;
  if (folder && item->has_unplayed_item_count) {
    c->has_remaining = true;
    c->remaining = c->played ? 0 : item->unplayed_item_count;
  }
}

static void apply_user_data(const jf_user_data *item) {
  card *const one[] = {&detail, &season_detail, &previous_episode,
                       &next_episode};
  for (size_t i = 0; i < sizeof(one) / sizeof(one[0]); i++)
    update_card(one[i], item);
  for (size_t i = 0; i < WINDOW_SIZE; i++)
    update_card(&grid_cards[i], item);
  for (size_t r = 0; r < ROW_COUNT; r++)
    for (size_t i = 0; i < rows[r].count; i++)
      update_card(&rows[r].cards[i], item);
  item_row *const lists[] = {&seasons_row, &episodes_row};
  for (size_t l = 0; l < sizeof(lists) / sizeof(lists[0]); l++)
    for (size_t i = 0; i < lists[l]->count; i++)
      update_card(&lists[l]->cards[i], item);
}

static void draw_season(loom_context *ctx, float width, float height,
                        float scale) {
  const float margin = 64 * scale;
  const loom_rect art = {margin, margin, 320 * scale, 480 * scale};
  const poster_slot *slot = card_poster(&season_detail);
  if (slot != NULL) {
    float uv[4];
    cover_uv(slot, art, uv);
    loom_textured(ctx, art, NULL, slot->texture, uv, WHITE, 12 * scale);
  } else if (!draw_blur(ctx, art, ctx->viewport, poster_card(&season_detail),
                        12 * scale)) {
    loom_fill(ctx, art, NULL, CARD, 12 * scale);
    if (artwork_loading(season_detail.poster_id, season_detail.poster_tag,
                        JF_IMAGE_PRIMARY, POSTER_W, POSTER_H))
      draw_spinner(ctx, art.x + art.w / 2, art.y + art.h / 2, 20 * scale, &art,
                   scale);
  }
  draw_watch_badge(ctx, art, ctx->viewport, &season_detail, scale);

  const float x = art.x + art.w + 60 * scale;
  const float w = width - x - margin;
  loom_label(ctx, (loom_rect){x, margin, w, 72 * scale}, NULL,
             ellipsize(detail.title, w, 52 * scale), TEXT, 52 * scale);
  loom_label(ctx, (loom_rect){x, 152 * scale, w, 48 * scale}, NULL,
             season_detail.title, TEXT, 32 * scale);
  draw_wrapped(ctx, (loom_rect){x, 220 * scale, w, 145 * scale},
               season_detail.overview[0] != '\0' ? season_detail.overview
                                                 : detail.overview,
               22 * scale);

  const loom_rect list_rect = {x, 400 * scale, w, height - 464 * scale};
  const float row_height = 170 * scale;
  if (episodes_row.count == 0) {
    loom_label(ctx, (loom_rect){x, list_rect.y + 20 * scale, w, 34 * scale},
               &list_rect,
               episodes_row.loading ? "Loading episodes..." : "No episodes",
               DIM, 23 * scale);
    return;
  }

  /* Like the home carousels: the selection walks down the fully visible
   * slots, then stays in the last one while the list moves a whole episode at
   * a time. The slot above it is then the one leaving through the fade. It
   * opens on the first episode not yet watched. */
  const float fade_height = 64 * scale;
  const float extent = (float)episodes_row.count * row_height;
  const float scroll =
      nav_scroll_begin(&ui, nav_str(season_detail.id), list_rect, LOOM_VERTICAL,
                       extent, 0, row_height);
  const loom_virtual_list list =
      loom_virtual_list_init(list_rect, episodes_row.count, row_height, scroll);
  /* Keep episodes below their heading, but let them reach the screen bottom. */
  const loom_rect content = {x, list_rect.y, width - x, height - list_rect.y};
  const loom_mask outer = ctx->mask;
  loom_fade(ctx, LOOM_VERTICAL, content.y, content.y + content.h, scroll,
            maxf(0, extent - list_rect.h), fade_height);
  const size_t unfinished =
      first_unfinished(episodes_row.cards, episodes_row.count);
  const card *chosen = NULL;
  /* Every episode is declared, drawn or not, so the one to open on can be
   * anywhere in the season. */
  for (size_t index = 0; index < episodes_row.count; index++) {
    const loom_rect raw = loom_virtual_list_item(&list, index);
    const loom_rect row = {raw.x + 10 * scale, raw.y + 6 * scale,
                           raw.w - 20 * scale, raw.h - 12 * scale};
    const card *source = &episodes_row.cards[index];
    if (index == unfinished)
      nav_default(&ui);
    const unsigned flags = nav_item(&ui, nav_str(source->id), row, content);
    if (flags & NAV_FOCUSED)
      nav_ring(&ui, row, 11 * scale);
    if (flags & NAV_ACTIVATED)
      chosen = source;
    if (index < list.first || index >= list.last)
      continue;
    const bool focused = flags & NAV_FOCUSED;
    if (focused || (flags & NAV_HOVERED)) {
      static const loom_color focused_bg = {20, 42, 60, 210};
      static const loom_color hot_bg = {30, 40, 55, 180};
      loom_fill(ctx, row, &content, focused ? focused_bg : hot_bg, 11 * scale);
    }
    const loom_rect clip = loom_intersect(row, content);
    const loom_rect thumb = {row.x + 12 * scale, row.y + 12 * scale,
                             232 * scale, 130.5f * scale};
    const poster_slot *still =
        artwork(source->id, source->thumbnail_tag, JF_IMAGE_PRIMARY, 384, 216);
    if (still != NULL) {
      float uv[4];
      cover_uv(still, thumb, uv);
      loom_textured(ctx, thumb, &clip, still->texture, uv, WHITE, 7 * scale);
    } else if (!draw_blur(ctx, thumb, clip, source, 7 * scale)) {
      loom_fill(ctx, thumb, &clip, CARD, 7 * scale);
      if (artwork_loading(source->id, source->thumbnail_tag, JF_IMAGE_PRIMARY,
                          384, 216))
        draw_spinner(ctx, thumb.x + thumb.w / 2, thumb.y + thumb.h / 2,
                     13 * scale, &clip, scale);
    }
    draw_watch_badge(ctx, thumb, clip, source, scale);

    const float text_x = thumb.x + thumb.w + 28 * scale;
    const float text_w = row.x + row.w - text_x - 24 * scale;
    loom_label(ctx, (loom_rect){text_x, row.y + 34 * scale, text_w, 44 * scale},
               &clip, ellipsize(source->episode_title, text_w, 28 * scale),
               TEXT, 28 * scale);
    draw_metadata(ctx,
                  (loom_rect){text_x, row.y + 90 * scale, text_w, 32 * scale},
                  &clip, source->runtime, source->rating, 21 * scale, TEXT);
    if (source->progress > 1) {
      const loom_rect bar = {thumb.x + 4 * scale, thumb.y + thumb.h - 7 * scale,
                             thumb.w - 8 * scale, 5 * scale};
      loom_fill(ctx,
                (loom_rect){bar.x, bar.y,
                            bar.w * minf(source->progress, 100) / 100, bar.h},
                &clip, ACCENT, 2 * scale);
    }
  }
  nav_scroll_end(&ui);
  ctx->mask = outer;
  if (chosen != NULL) {
    const card copy = *chosen;
    push();
    open_details(&copy);
  }
}

/* A column at the right, clear of where dialogue sits, so the line a choice
 * brings up is in view. */
static void draw_subtitle_picker(loom_context *ctx, float width, float height,
                                 float scale) {
  static const loom_color chrome = {8, 12, 20, 215};
  const size_t rows = track_row(jf_player_subtitle_count() - 1) + 1;
  const float row = 58 * scale, head = 64 * scale;
  const size_t visible = min_size(rows, 11);
  const loom_rect panel = {width - 620 * scale,
                           (height - head - visible * row) / 2 - 40 * scale,
                           560 * scale, head + visible * row + 16 * scale};
  loom_fill(ctx, panel, NULL, chrome, 16 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 28 * scale, panel.y + 16 * scale, panel.w,
                         36 * scale},
             &panel, "Subtitles", DIM, 22 * scale);
  const loom_rect list = {panel.x, panel.y + head, panel.w, visible * row};
  /* Scrolled only once the focus reaches the last row in view. */
  const float scroll =
      nav_scroll_begin(&ui, nav_str("subtitles"), list, LOOM_VERTICAL,
                       (float)rows * row, row, 0);
  if (picker_focus_pending) {
    picker_focus_pending = false;
    nav_focus(&ui, picker_row_id(track_row(jf_player_subtitle_current())));
  }
  bool chosen_any = false;
  for (size_t i = 0; i < rows; i++) {
    const loom_rect rect = {panel.x + 12 * scale,
                            list.y + (float)i * row - scroll,
                            panel.w - 24 * scale, row - 6 * scale};
    const unsigned flags = nav_item(&ui, nav_int(i), rect, list);
    const bool chosen = flags & NAV_FOCUSED;
    const int track = (int)i - 2;
    /* Moving is choosing: the frame behind the picker shows the track at
     * once. A click closes it, as OK does. */
    if (i > 0 && chosen && track != jf_player_subtitle_current())
      jf_player_subtitle_select(track);
    if (i == 0 && (flags & NAV_ACTIVATED))
      open_subtitle_search();
    else
      chosen_any |= (flags & NAV_ACTIVATED) != 0;
    if (rect.y + rect.h <= list.y || rect.y >= list.y + list.h)
      continue;
    if (chosen)
      loom_fill(ctx, rect, &list, SELECTED, 10 * scale);
    const char *name = i == 0   ? "Download subtitles..."
                       : i == 1 ? "Off"
                                : jf_player_subtitle_name(track);
    /* Every track has its appearance and timing to the right. */
    const float arrow = track >= 0 ? 36 * scale : 0;
    const float text_y = rect.y + (rect.h - 30 * scale) / 2;
    loom_label(ctx,
               (loom_rect){rect.x + 16 * scale, text_y,
                           rect.w - 32 * scale - arrow, 36 * scale},
               &list, ellipsize(name, rect.w - 32 * scale - arrow, 22 * scale),
               chosen   ? WHITE
               : i == 0 ? ACCENT
                        : TEXT,
               22 * scale);
    if (track >= 0)
      loom_label(ctx,
                 (loom_rect){rect.x + rect.w - 34 * scale, text_y, 18 * scale,
                             36 * scale},
                 &list, ">", chosen ? WHITE : DIM, 22 * scale);
  }
  nav_scroll_end(&ui);
  if (chosen_any && subtitle_picker)
    close_subtitle_picker();
}

/* A row's label at the left and its value at the right, with arrows round a
 * value left and right step, or an arrow to a submenu. */
static void draw_value_row(loom_context *ctx, loom_rect rect,
                           const loom_rect *clip, const char *label,
                           const char *value, const loom_color value_ink,
                           bool focused, bool steps, bool submenu,
                           float scale) {
  const float text_y = rect.y + (rect.h - 28 * scale) / 2;
  loom_label(ctx, (loom_rect){rect.x + 16 * scale, text_y, rect.w, 32 * scale},
             clip, label, focused ? WHITE : TEXT, 20 * scale);
  const char *shown = focused && steps ? fmt("<  %s  >", value)
                      : submenu        ? fmt("%s   >", value)
                                       : value;
  const float value_w = jf_renderer_measure(renderer, shown, 20 * scale);
  loom_label(ctx,
             (loom_rect){rect.x + rect.w - 16 * scale - value_w, text_y,
                         value_w, 32 * scale},
             clip, shown, focused ? WHITE : value_ink, 20 * scale);
}

/* A panel's note under its list: yellow for something to do, red for
 * something that went wrong. */
static void draw_note(loom_context *ctx, loom_rect panel, float y,
                      const char *note, bool error, float scale) {
  if (note[0] == '\0')
    return;
  loom_label(
      ctx,
      (loom_rect){panel.x + 28 * scale, y, panel.w - 56 * scale, 32 * scale},
      &panel, ellipsize(note, panel.w - 56 * scale, 20 * scale),
      error ? RED : YELLOW, 20 * scale);
}

static void draw_subtitle_search(loom_context *ctx, float width, float height,
                                 float scale) {
  static const loom_color chrome = {8, 12, 20, 225};
  const size_t rows = SEARCH_ROW_RESULTS + search.count;
  const float row = 64 * scale, head = 64 * scale;
  const size_t visible = min_size(rows, 10);
  const loom_rect panel = {width - 820 * scale,
                           (height - head - 10 * row) / 2 - 40 * scale,
                           760 * scale, head + 10 * row + 56 * scale};
  loom_fill(ctx, panel, NULL, chrome, 16 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 28 * scale, panel.y + 16 * scale, panel.w,
                         36 * scale},
             &panel, "Download subtitles", DIM, 22 * scale);
  const loom_rect list = {panel.x, panel.y + head, panel.w, visible * row};
  const float scroll =
      nav_scroll_begin(&ui, nav_str("subtitle-search"), list, LOOM_VERTICAL,
                       (float)rows * row, row, 0);
  for (size_t i = 0; i < rows; i++) {
    const loom_rect rect = {panel.x + 12 * scale,
                            list.y + (float)i * row - scroll,
                            panel.w - 24 * scale, row - 6 * scale};
    const unsigned flags = nav_item(&ui, nav_int(i), rect, list);
    if (flags & NAV_ACTIVATED) {
      if (i == SEARCH_ROW_SOURCES)
        open_sources();
      else if (i >= SEARCH_ROW_RESULTS)
        download_result(i - SEARCH_ROW_RESULTS);
    }
    if (rect.y + rect.h <= list.y || rect.y >= list.y + list.h)
      continue;
    const bool focused = flags & NAV_FOCUSED;
    if (focused)
      loom_fill(ctx, rect, &list, SELECTED, 10 * scale);
    if (i == SEARCH_ROW_SOURCES) {
      const int on = sources_on();
      const bool none = sources.loaded && on == 0;
      draw_value_row(ctx, rect, &list, "Sources",
                     !sources.loaded ? "..."
                     : none          ? "None on"
                                     : fmt("%d on", on),
                     none ? YELLOW : DIM, focused, false, true, scale);
      continue;
    }
    if (i == SEARCH_ROW_LANGUAGE) {
      draw_value_row(ctx, rect, &list, "Language",
                     search_languages[sub_style.search_language].name, DIM,
                     focused, true, false, scale);
      continue;
    }
    const size_t r = i - SEARCH_ROW_RESULTS;
    const float text_w = rect.w - 32 * scale;
    loom_label(ctx,
               (loom_rect){rect.x + 16 * scale, rect.y + 6 * scale, text_w,
                           32 * scale},
               &list, ellipsize(search.results[r].name, text_w, 20 * scale),
               focused ? WHITE : TEXT, 20 * scale);
    loom_label(ctx,
               (loom_rect){rect.x + 16 * scale, rect.y + 34 * scale, text_w,
                           28 * scale},
               &list, ellipsize(search.results[r].detail, text_w, 16 * scale),
               DIM, 16 * scale);
  }
  nav_scroll_end(&ui);
  static const char *const phases[] = {
      [DOWNLOAD_FETCHING] = "Downloading...",
      [DOWNLOAD_LISTING] = "Waiting for the server to list it...",
      [DOWNLOAD_CHECKING] = "Checking it can be read...",
      [DOWNLOAD_SELECTING] = "Selecting it...",
  };
  const float note_y = panel.y + panel.h - 44 * scale;
  if (search.phase != DOWNLOAD_NONE || search.searching)
    loom_label(ctx,
               (loom_rect){panel.x + 28 * scale, note_y, panel.w - 56 * scale,
                           32 * scale},
               &panel,
               search.phase != DOWNLOAD_NONE ? phases[search.phase]
                                             : "Searching...",
               DIM, 20 * scale);
  else
    draw_note(ctx, panel, note_y, search.error, !search.error_is_warning,
              scale);
}

static void draw_subtitle_sources(loom_context *ctx, float width, float height,
                                  float scale) {
  static const loom_color chrome = {8, 12, 20, 225};
  static const loom_color armed = {150, 40, 40, 255};
  const size_t rows = source_rows();
  /* Moving off the restart row takes back its first press. */
  if (ui.focus !=
      nav_child(nav_str("subtitle-sources"), nav_int(restart_row())))
    sources.restart_armed = false;
  const float row = 58 * scale, head = 64 * scale;
  const size_t visible = min_size(rows > 0 ? rows : 1, 9);
  const loom_rect panel = {width - 720 * scale,
                           (height - head - 9 * row) / 2 - 40 * scale,
                           660 * scale, head + 9 * row + 56 * scale};
  loom_fill(ctx, panel, NULL, chrome, 16 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 28 * scale, panel.y + 16 * scale, panel.w,
                         36 * scale},
             &panel, "Subtitle sources", DIM, 22 * scale);
  const loom_rect list = {panel.x, panel.y + head, panel.w, visible * row};
  const float scroll =
      nav_scroll_begin(&ui, nav_str("subtitle-sources"), list, LOOM_VERTICAL,
                       (float)rows * row, row, 0);
  for (size_t i = 0; i < rows; i++) {
    const loom_rect rect = {panel.x + 12 * scale,
                            list.y + (float)i * row - scroll,
                            panel.w - 24 * scale, row - 6 * scale};
    const unsigned flags = nav_item(&ui, nav_int(i), rect, list);
    if (flags & NAV_ACTIVATED)
      activate_source_row(i);
    if (rect.y + rect.h <= list.y || rect.y >= list.y + list.h)
      continue;
    const bool focused = flags & NAV_FOCUSED;
    if (i >= sources.count && restart_pending() && i == restart_row()) {
      /* Restarting stops playback: red, and a second press, as deleting. */
      if (focused)
        loom_fill(ctx, rect, &list, sources.restart_armed ? armed : SELECTED,
                  10 * scale);
      draw_value_row(ctx, rect, &list, "Restart the server to apply",
                     sources.restart_armed ? "Press again: playback stops" : "",
                     WHITE, focused, false, false, scale);
      continue;
    }
    if (focused)
      loom_fill(ctx, rect, &list, SELECTED, 10 * scale);
    if (i >= sources.count) {
      draw_value_row(ctx, rect, &list, "Add more sources",
                     sources.adding ? "Working..." : "subbuzz repository",
                     ACCENT, focused, false, false, scale);
      continue;
    }
    const char *status = sources.items[i].status;
    const char *value;
    loom_color ink;
    if (sources.items[i].busy)
      value = "Working...", memcpy(ink, DIM, sizeof(ink));
    else if (status[0] == '\0')
      value = "Install", memcpy(ink, ACCENT, sizeof(ink));
    else if (strcmp(status, "Active") == 0)
      value = "On", memcpy(ink, ACCENT, sizeof(ink));
    else if (strcmp(status, "Disabled") == 0)
      value = "Off", memcpy(ink, DIM, sizeof(ink));
    else if (strcmp(status, "Restart") == 0)
      value = "On after restart", memcpy(ink, YELLOW, sizeof(ink));
    else
      value = "Broken", memcpy(ink, RED, sizeof(ink));
    draw_value_row(ctx, rect, &list, sources.items[i].name, value, ink, focused,
                   false, false, scale);
  }
  nav_scroll_end(&ui);
  const float note_y = panel.y + panel.h - 44 * scale;
  if (sources.loading && sources.count == 0)
    loom_label(ctx,
               (loom_rect){panel.x + 28 * scale, note_y, panel.w, 32 * scale},
               &panel, "Asking the server...", DIM, 20 * scale);
  else if (sources.loaded && sources.count == 0 && sources.note[0] == '\0')
    draw_note(ctx, panel, note_y,
              "The server's plugin repositories offer no subtitle source",
              false, scale);
  else
    draw_note(ctx, panel, note_y, sources.note, sources.note_is_error, scale);
}

static const char *setting_value(subtitle_setting row) {
  const int *delay = current_delay();
  switch (row) {
  case SETTING_PLAIN_ASS:
    return sub_style.plain_ass ? "Plain" : "Styled";
  case SETTING_COLOR:
    return swatches[sub_style.color].name;
  case SETTING_OUTLINE:
    return sub_style.outline ? "On" : "Off";
  case SETTING_OUTLINE_WIDTH:
    return fmt("%d px", sub_style.outline_width);
  case SETTING_OUTLINE_COLOR:
    return swatches[sub_style.outline_color].name;
  case SETTING_INLINE:
    return sub_style.ignore_inline ? "Ignore" : "Keep";
  case SETTING_BOLD:
    return sub_style.bold ? "On" : "Off";
  case SETTING_SIZE:
    return fmt("%d%%", 100 + sub_style.size_percent);
  case SETTING_MARGIN:
    return fmt("%d%%", sub_style.margin_percent);
  case SETTING_SIDE:
    return sub_style.top ? "Top" : "Bottom";
  case SETTING_DELAY:
    return delay != NULL ? fmt("%+.1f s", *delay / 1000.0) : "-";
  case SETTING_DELETE:
    return delete_armed ? "Press again to delete" : "";
  default:
    return "";
  }
}

static void draw_subtitle_settings(loom_context *ctx, float width, float height,
                                   float scale) {
  static const char *const labels[SETTING_COUNT] = {
      [SETTING_PLAIN_ASS] = "Draw as plain text",
      [SETTING_COLOR] = "Text colour",
      [SETTING_OUTLINE] = "Outline",
      [SETTING_OUTLINE_WIDTH] = "Width",
      [SETTING_OUTLINE_COLOR] = "Colour",
      [SETTING_INLINE] = "Bold and italic",
      [SETTING_BOLD] = "Always bold",
      [SETTING_SIZE] = "Size",
      [SETTING_MARGIN] = "Edge margin",
      [SETTING_SIDE] = "Position",
      [SETTING_DELAY] = "Delay",
      [SETTING_SUGGEST] = "Match delay to another track",
      [SETTING_DELETE] = "Delete subtitle file",
  };
  /* Moving off the delete row takes back the first OK. */
  if (focused_setting() != SETTING_DELETE)
    delete_armed = false;
  static const loom_color chrome = {8, 12, 20, 215};
  static const loom_color greyed = {110, 122, 138, 255};
  subtitle_setting shown[SETTING_COUNT];
  size_t rows = 0;
  for (int i = 0; i < SETTING_COUNT; i++)
    if (setting_shown((subtitle_setting)i))
      shown[rows++] = (subtitle_setting)i;
  const float row = 50 * scale, head = 64 * scale;
  const size_t visible = min_size(rows, 13);
  const loom_rect panel = {width - 660 * scale, 40 * scale, 600 * scale,
                           head + visible * row + 16 * scale};
  loom_fill(ctx, panel, NULL, chrome, 16 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 28 * scale, panel.y + 16 * scale, panel.w,
                         36 * scale},
             &panel, "Subtitle appearance and timing", DIM, 22 * scale);
  const loom_rect list = {panel.x, panel.y + head, panel.w, visible * row};
  /* Scrolled only once the focus reaches the last row in view. */
  const float scroll =
      nav_scroll_begin(&ui, nav_str("subtitle-settings"), list, LOOM_VERTICAL,
                       (float)rows * row, row, 0);
  for (size_t r = 0; r < rows; r++) {
    const subtitle_setting i = shown[r];
    const bool child = setting_child(i);
    const float indent = child ? 34 * scale : 0;
    const loom_rect rect = {panel.x + 12 * scale,
                            list.y + (float)r * row - scroll,
                            panel.w - 24 * scale, row - 6 * scale};
    const bool enabled = setting_enabled(i);
    /* Greyed rows are not stops for the focus. */
    const unsigned flags = enabled ? nav_item(&ui, nav_int(i), rect, list) : 0;
    if ((flags & NAV_ACTIVATED) && setting_is_toggle(i))
      change_setting(i, 1);
    if (rect.y + rect.h <= list.y || rect.y >= list.y + list.h)
      continue;
    const bool focused = flags & NAV_FOCUSED;
    const float mid = rect.y + rect.h / 2;
    if (child) {
      /* A tree line from the parent: down through every child and across to
       * each, stopping at the last one's middle. */
      const float x = rect.x + 26 * scale;
      const bool last = r + 1 == rows || !setting_child(shown[r + 1]);
      const float top = rect.y - 6 * scale;
      const float bottom = last ? mid : rect.y + rect.h + 6 * scale;
      loom_fill(ctx, (loom_rect){x, top, 2 * scale, bottom - top}, &list,
                BORDER, 0);
      loom_fill(ctx, (loom_rect){x, mid - scale, 16 * scale, 2 * scale}, &list,
                BORDER, 0);
    }
    const loom_rect body = {rect.x + indent, rect.y, rect.w - indent, rect.h};
    /* Deleting is red, and its row turns red while it waits for the OK that
     * confirms it. */
    static const loom_color armed = {150, 40, 40, 255};
    const bool deleting = i == SETTING_DELETE;
    if (focused)
      loom_fill(ctx, body, &list, deleting && delete_armed ? armed : SELECTED,
                10 * scale);
    const float text_y = rect.y + (rect.h - 28 * scale) / 2;
    const loom_color *ink = !enabled                    ? &greyed
                            : deleting && !delete_armed ? &RED
                            : focused                   ? &WHITE
                                                        : &TEXT;
    loom_label(ctx,
               (loom_rect){body.x + 16 * scale, text_y, body.w, 32 * scale},
               &list, labels[i], *ink, 20 * scale);
    const char *value = setting_value(i);
    if (value[0] == '\0')
      continue;
    /* Arrows on the focused row say left and right change it. */
    const bool action = i == SETTING_SUGGEST || i == SETTING_DELETE;
    const char *text = focused && !action ? fmt("<  %s  >", value) : value;
    const float value_w = jf_renderer_measure(renderer, text, 20 * scale);
    float value_x = rect.x + rect.w - 16 * scale - value_w;
    loom_label(ctx, (loom_rect){value_x, text_y, value_w, 32 * scale}, &list,
               text,
               !enabled  ? greyed
               : focused ? WHITE
                         : DIM,
               20 * scale);
    if (i == SETTING_COLOR || i == SETTING_OUTLINE_COLOR) {
      const int index =
          i == SETTING_COLOR ? sub_style.color : sub_style.outline_color;
      const float side = 22 * scale;
      const loom_rect chip = {value_x - side - 10 * scale, mid - side / 2, side,
                              side};
      loom_fill(ctx, chip, &list, swatches[index].color, 5 * scale);
      loom_stroke(ctx, chip, &list, BORDER, 1.5f * scale, 5 * scale);
    }
  }
  nav_scroll_end(&ui);
  (void)height;
}

static const char *clock_text(int ms) {
  const int seconds = ms > 0 ? ms / 1000 : 0;
  return seconds >= 3600 ? fmt("%d:%02d:%02d", seconds / 3600,
                               seconds / 60 % 60, seconds % 60)
                         : fmt("%d:%02d", seconds / 60, seconds % 60);
}

/* Glides the focus glow to (x, y), or puts it there when it was not showing. */
static void move_glow(float x, float y) {
  if (!glow_placed) {
    animated_float_snap(&glow_x, x);
    animated_float_snap(&glow_y, y);
    glow_placed = true;
    return;
  }
  animated_float_set(&glow_x, x, NAVIGATION_ANIMATION_NS);
  animated_float_set(&glow_y, y, NAVIGATION_ANIMATION_NS);
}

static const char *playback_hint(size_t index) {
  switch (playback_buttons[index].action) {
  case ACTION_PAUSE:
    return playback_paused ? "Play" : "Pause";
  case ACTION_SUBTITLES: {
    const int track = jf_player_subtitle_current();
    return fmt("Subtitles: %s",
               track >= 0 ? jf_player_subtitle_name(track) : "Off");
  }
  case ACTION_AUDIO: {
    const int count = jf_player_audio_count();
    if (count == 0)
      return "Audio: none";
    return fmt("Audio: %s  (%d of %d)",
               jf_player_audio_name(jf_player_audio_current()),
               jf_player_audio_current() + 1, count);
  }
  case ACTION_PREVIOUS:
    return fmt("Previous episode: %s", previous_episode.episode_title);
  case ACTION_NEXT:
    return fmt("Next episode: %s", next_episode.episode_title);
  default:
    return playback_buttons[index].hint;
  }
}

/* Over the video: a gradient rising from the bottom rather than a box, the
 * title, the seek bar and a row of icon buttons. It slides and fades in and out
 * as a whole; the focus is a blue glow that glides between whatever it is on.
 */
static void draw_playback(loom_context *ctx, float width, float height,
                          float scale) {
  if (subtitle_settings) {
    draw_subtitle_settings(ctx, width, height, scale);
    return;
  }
  if (search.open) {
    draw_subtitle_search(ctx, width, height, scale);
    return;
  }
  if (sources.open) {
    draw_subtitle_sources(ctx, width, height, scale);
    return;
  }
  if (subtitle_picker) {
    draw_subtitle_picker(ctx, width, height, scale);
    return;
  }
  const bool wanted = playback_chrome_wanted(now_ns());
  animated_float_set(&chrome_motion, wanted ? 1 : 0,
                     2 * NAVIGATION_ANIMATION_NS);
  const float shown = chrome_motion.value;
  if (shown <= 0.001f) {
    glow_placed = false;
    return;
  }
  const size_t first = ctx->count;

  static const loom_color shade = {0, 0, 0, 215};
  static const loom_color track = {255, 255, 255, 64};
  static const loom_color mark = {255, 255, 255, 170};
  static const loom_color glow = {20, 130, 255, 235};
  const loom_rect scrim = {0, height - 440 * scale, width, 440 * scale};
  const loom_mask outer = ctx->mask;
  ctx->mask.edge[LOOM_FADE_TOP] = scrim.y;
  ctx->mask.width[LOOM_FADE_TOP] = scrim.h;
  loom_fill(ctx, scrim, NULL, shade, 0);
  ctx->mask = outer;

  const float margin = 40 * scale;
  const float row_y = height - 74 * scale;  /* the buttons' centre line */
  const float bar_y = height - 136 * scale; /* the seek bar's */
  /* What has the focus, for the hint and the glow; nav says which. Until it
   * lands on something, play/pause, the default. */
  const bool bar_focused = ui.focus == seek_bar_id();
  size_t focused = PLAY_PAUSE_INDEX;
  for (size_t index = 0; index < PLAYBACK_BUTTON_COUNT; index++)
    if (button_shown(index) && ui.focus == button_id(index))
      focused = index;

  /* Title, and what the focused control is or does. */
  const jf_player_state state = jf_player_state_get();
  const char *hint = state == JF_LOADING  ? "Loading..."
                     : state == JF_FAILED ? jf_player_error()
                     : bar_focused        ? (playback_paused ? "Paused" : "")
                                          : playback_hint(focused);
  const float hint_size = 22 * scale;
  const float hint_w = jf_renderer_measure(renderer, hint, hint_size);
  loom_label(ctx,
             (loom_rect){width - margin - hint_w, bar_y - 74 * scale,
                         hint_w + 4, 30 * scale},
             NULL, hint, state == JF_FAILED ? RED : DIM, hint_size);
  loom_label(ctx,
             (loom_rect){margin, bar_y - 84 * scale,
                         width - margin * 2 - hint_w - 40 * scale, 44 * scale},
             NULL,
             ellipsize(playback_title, width - margin * 2 - hint_w - 40 * scale,
                       34 * scale),
             TEXT, 34 * scale);

  /* The seek bar, between the elapsed time and the length. */
  const int duration = jf_player_duration();
  /* A release that never arrived ends a scrub all the same. */
  if (scrubbing && now_ns() - scrub_moved_at > 600000000ull)
    end_scrub();
  const int position = scrubbing ? scrub_ms : jf_player_position();
  const float time_size = 22 * scale, time_w = 74 * scale;
  const char *elapsed = clock_text(position);
  loom_label(ctx,
             (loom_rect){margin + time_w -
                             jf_renderer_measure(renderer, elapsed, time_size),
                         bar_y - 14 * scale, time_w, 30 * scale},
             NULL, elapsed, TEXT, time_size);
  loom_label(ctx,
             (loom_rect){width - margin - time_w, bar_y - 14 * scale, time_w,
                         30 * scale},
             NULL, duration > 0 ? clock_text(duration) : "--:--", DIM,
             time_size);
  const float bar_h = (bar_focused ? 8 : 5) * scale;
  const loom_rect bar = {margin + time_w + 28 * scale, bar_y - bar_h / 2,
                         width - 2 * (margin + time_w + 28 * scale), bar_h};
  const float seen =
      duration > 0 ? minf(maxf((float)position / duration, 0), 1) : 0;
  const float head_x = bar.x + bar.w * seen;
  if (bar_focused)
    move_glow(head_x, bar_y);
  /* OK on the bar pauses; a click on it seeks to there. */
  const loom_rect bar_hit = {bar.x, bar_y - 24 * scale, bar.w, 48 * scale};
  const unsigned bar_flags =
      nav_item(&ui, seek_bar_id(), bar_hit, ctx->viewport);
  if (bar_flags & NAV_ACTIVATED) {
    if (bar_flags & NAV_CLICKED) {
      if (duration > 0)
        jf_player_seek_to((int)((ui.pointer_x - bar.x) / bar.w * duration));
    } else {
      toggle_playback();
    }
  }

  /* Everything that glows is drawn after the glow. A button that goes away
   * (chapters or episodes) hands the focus back to play/pause, the default. */
  float centres[PLAYBACK_BUTTON_COUNT];
  const float step = 96 * scale;
  /* Play/pause holds the middle; the shown buttons pack outwards from it. */
  centres[PLAY_PAUSE_INDEX] = width / 2;
  float left = width / 2, right = width / 2;
  for (size_t index = PLAY_PAUSE_INDEX; index-- > 0;)
    if (button_shown(index))
      centres[index] = left -= step;
  for (size_t index = PLAY_PAUSE_INDEX + 1; index < TRANSPORT_COUNT; index++)
    if (button_shown(index))
      centres[index] = right += step;
  centres[TRANSPORT_COUNT] = width - margin - 24 * scale - step;
  centres[TRANSPORT_COUNT + 1] = width - margin - 24 * scale;
  if (!bar_focused)
    move_glow(centres[focused], row_y);
  const float glow_size = 180 * scale;
  loom_label(ctx,
             (loom_rect){glow_x.value - glow_size / 2,
                         glow_y.value - glow_size / 2, glow_size, glow_size},
             NULL, JF_ICON_GLOW_TEXT, glow, glow_size);

  loom_fill(ctx, bar, NULL, track, bar_h / 2);
  loom_fill(ctx, (loom_rect){bar.x, bar.y, bar.w * seen, bar.h}, NULL, ACCENT,
            bar_h / 2);
  const int *chapters = NULL;
  const int chapter_count = duration > 0 ? jf_player_chapters(&chapters) : 0;
  for (int i = 0; i < chapter_count; i++) {
    if (chapters[i] <= 0 || chapters[i] >= duration)
      continue;
    const float x = bar.x + bar.w * chapters[i] / duration;
    loom_fill(
        ctx,
        (loom_rect){x - 1.5f * scale, bar_y - 9 * scale, 3 * scale, 18 * scale},
        NULL, position < chapters[i] ? mark : ACCENT, scale);
  }
  if (bar_focused) {
    const float knob = 22 * scale;
    loom_fill(ctx, (loom_rect){head_x - knob / 2, bar_y - knob / 2, knob, knob},
              NULL, WHITE, knob / 2);
  }

  for (size_t index = 0; index < PLAYBACK_BUTTON_COUNT; index++) {
    if (!button_shown(index))
      continue;
    const bool pause = playback_buttons[index].action == ACTION_PAUSE;
    const float icon = (pause ? 64 : 46) * scale;
    const loom_rect hit = {centres[index] - step / 2, row_y - step / 2, step,
                           step};
    const loom_rect at = {centres[index] - icon / 2, row_y - icon / 2, icon,
                          icon};
    if (index == PLAY_PAUSE_INDEX)
      nav_default(&ui);
    const unsigned flags = nav_item(&ui, button_id(index), hit, ctx->viewport);
    if (flags & NAV_ACTIVATED)
      activate_playback(index);
    const bool lit = flags & NAV_FOCUSED;
    loom_label(ctx, at, NULL,
               pause && playback_paused ? JF_ICON_PLAY
                                        : playback_buttons[index].icon,
               lit ? WHITE : TEXT, icon);
    const char *number = playback_buttons[index].number;
    if (number[0] != '\0') {
      const float size = 15 * scale;
      const float w = jf_renderer_measure(renderer, number, size);
      loom_label(ctx,
                 (loom_rect){centres[index] - w / 2,
                             at.y + icon * 0.555f - size * 0.72f, w + 2,
                             size * 1.4f},
                 NULL, number, lit ? WHITE : TEXT, size);
    }
    const char *tag = playback_buttons[index].tag;
    if (tag[0] != '\0') {
      const float size = 16 * scale;
      const float w = jf_renderer_measure(renderer, tag, size);
      loom_label(
          ctx,
          (loom_rect){centres[index] - w / 2, at.y + icon, w + 2, size * 1.4f},
          NULL, tag, lit ? WHITE : DIM, size);
    }
  }

  /* Fade and slide the whole of it with the one animation. */
  const float drop = (1 - shown) * 60 * scale;
  for (size_t i = first; i < ctx->count; i++) {
    loom_command *c = &ctx->commands[i];
    c->rect.y += drop;
    uint8_t *alpha = c->kind == LOOM_RECTANGLE ? &c->rectangle.color[3]
                     : c->kind == LOOM_BORDER  ? &c->border.color[3]
                     : c->kind == LOOM_TEXT    ? &c->text.color[3]
                                               : &c->image.tint[3];
    *alpha = (uint8_t)(*alpha * shown);
  }
}

/* The subtitle overlay sits under the transport chrome and over the video hole.
 * libass composes it; this only notices when it changed and re-uploads. */
/* Asks libass for the picture at the playback position, uploading it when it
 * changed. Runs on the subtitle tick, outside building the UI, so an unchanged
 * subtitle costs no frame. */
/* The plain-text track's lines at the playback position, as jf_subs_text
 * gives them. */
static char plain_text[2048];

static bool update_subtitles(void) {
  const int *delay = current_delay();
  const int played_ms = jf_player_subtitle_ms();
  if (played_ms < 0)
    return false;
  /* A track shown late is one whose times are behind the picture's. */
  const int media_ms = played_ms - (delay != NULL ? *delay : 0);
  if (jf_picsubs_ready()) {
    /* Bitmap tracks: the picture is already pixels, and changes a few times a
     * minute, so it is uploaded as it is and scaled by the draw. */
    jf_picsubs_image picture;
    if (!jf_picsubs_frame(media_ms, &picture))
      return false;
    const double started = thread_ms();
    if (subtitle_texture != 0)
      jf_renderer_destroy_texture(renderer, subtitle_texture);
    subtitle_texture = picture.rgba != NULL
                           ? jf_renderer_create_rgba_texture(
                                 renderer, (uint32_t)picture.texture_w,
                                 (uint32_t)picture.texture_h, picture.rgba)
                           : 0;
    phase_frame[PHASE_UPLOAD] = thread_ms() - started;
    subtitle_rect = (loom_rect){picture.x, picture.y, picture.w, picture.h};
    return true;
  }
  jf_subs_image image;
  bool changed = jf_subs_frame(media_ms, &image);
  if (jf_subs_plain()) {
    char text[sizeof(plain_text)];
    jf_subs_text(media_ms, text, sizeof(text), !sub_style.ignore_inline);
    if (strcmp(text, plain_text) != 0) {
      memcpy(plain_text, text, sizeof(text));
      jf_window_frame_requested = true;
    }
  } else {
    plain_text[0] = '\0';
  }
  const jf_subs_cost cost = jf_subs_last_cost();
  phase_frame[PHASE_ASS] = cost.render_ms;
  phase_frame[PHASE_COMPOSITE] = cost.composite_ms;
  if (cost.runs > runs_peak)
    runs_peak = cost.runs;
  if (changed) {
    const double started = thread_ms();
    if (subtitle_texture != 0)
      jf_renderer_destroy_texture(renderer, subtitle_texture);
    subtitle_texture =
        image.w > 0
            ? jf_renderer_create_rgba_texture(renderer, (uint32_t)image.w,
                                              (uint32_t)image.h, image.rgba)
            : 0;
    phase_frame[PHASE_UPLOAD] = thread_ms() - started;
    subtitle_rect = (loom_rect){(float)image.x, (float)image.y, (float)image.w,
                                (float)image.h};
  }
  return changed;
}

static bool subtitles_active(void) {
  return jf_subs_ready() || jf_picsubs_ready();
}

/* A run of text in one style, laid out on one row of the plain renderer. */
typedef struct {
  const char *text;
  float x, width;
  int row;
  uint8_t style; /* bit 0 bold, bit 1 italic */
} text_run;

/* Plain text, centred and wrapped over rows from the chosen edge. Runs split
 * at spaces and style markers, and a row breaks between words, never inside
 * one. */
static void draw_plain_subtitles(loom_context *ctx, const char *text,
                                 float bottom_limit) {
  static char pool[4096];
  static text_run runs[256];
  /* Outlines, every one before any fill, and then the fills. */
  static loom_text_style styles[4], fills[4];
  const float unit = (float)gl_height / 1080;
  const float size = 58 * unit * (1 + sub_style.size_percent / 100.0f);
  const float line = size * 1.25f;
  const float max_width = (float)gl_width * 0.84f;
  for (int i = 0; i < 4; i++) {
    const float weight = (i & 1) ? size * 0.035f : 0;
    /* No wider than the field reaches past the glyph (see
     * JF_ATLAS_RANGE), less the pixel the edge needs. */
    const float reach =
        JF_ATLAS_RANGE / 2 * size / JF_ATLAS_BASE_PX - 1 - weight;
    const float outline =
        sub_style.outline ? sub_style.outline_width * unit : 0;
    styles[i] = (loom_text_style){{0, 0, 0, 0},
                                  outline < reach ? outline : reach,
                                  weight,
                                  (i & 2) ? 0.2f : 0};
    memcpy(styles[i].outline, swatches[sub_style.outline_color].color,
           sizeof(loom_color));
    fills[i] = (loom_text_style){{0, 0, 0, 0}, 0, weight, styles[i].slant};
  }

  /* Cut into runs, measuring each. */
  size_t count = 0, used = 0;
  uint8_t style = 0;
  int row = 0;
  float x = 0;
  size_t word_start = 0; /* first run of the word being laid out */
  float word_x = 0;
  bool space_pending = false;
  const float space =
      jf_renderer_measure_styled(renderer, " ", size, &styles[0]);
  for (const char *p = text;;) {
    const char c = *p;
    if (c == JF_SUBS_BOLD_ON || c == JF_SUBS_BOLD_OFF ||
        c == JF_SUBS_ITALIC_ON || c == JF_SUBS_ITALIC_OFF) {
      if (c == JF_SUBS_BOLD_ON)
        style |= 1;
      else if (c == JF_SUBS_BOLD_OFF)
        style &= (uint8_t)~1u;
      else if (c == JF_SUBS_ITALIC_ON)
        style |= 2;
      else
        style &= (uint8_t)~2u;
      p++;
      continue;
    }
    if (c == '\0' || c == '\n') {
      if (c == '\0')
        break;
      row++, x = 0, word_start = count, space_pending = false, p++;
      continue;
    }
    if (c == ' ') {
      space_pending = x > 0;
      word_start = count;
      p++;
      continue;
    }
    /* One run: up to the next space, newline or marker. */
    size_t length = 0;
    while (p[length] != '\0' && p[length] != ' ' && p[length] != '\n' &&
           (unsigned char)p[length] > JF_SUBS_ITALIC_OFF)
      length++;
    if (count == sizeof(runs) / sizeof(*runs) ||
        used + length + 1 > sizeof(pool))
      break;
    char *copy = pool + used;
    memcpy(copy, p, length);
    copy[length] = '\0';
    used += length + 1;
    p += length;
    const uint8_t look = style | (sub_style.bold ? 1 : 0);
    const float width =
        jf_renderer_measure_styled(renderer, copy, size, &styles[look]);
    if (word_start == count) { /* a word starts here */
      if (space_pending)
        x += space;
      space_pending = false;
      word_x = x;
    }
    runs[count] = (text_run){copy, x, width, row, look};
    x += width;
    count++;
    /* Too wide: the whole word moves down a row, unless it is alone there. */
    if (x > max_width && word_x > 0) {
      row++;
      for (size_t i = word_start; i < count; i++) {
        runs[i].x -= word_x;
        runs[i].row = row;
      }
      x -= word_x;
      word_x = 0;
    }
  }
  if (count == 0)
    return;

  const int rows = runs[count - 1].row + 1;
  const float margin = (float)gl_height * sub_style.margin_percent / 100.0f;
  float top =
      sub_style.top ? margin : (float)gl_height - margin - (float)rows * line;
  if (!sub_style.top && top + (float)rows * line > bottom_limit)
    top = bottom_limit - (float)rows * line;
  /* Centre each row on its own width. */
  static const loom_color clear = {0, 0, 0, 0};
  for (int pass = sub_style.outline ? 0 : 1; pass < 2; pass++)
    for (size_t first = 0; first < count;) {
      size_t last = first;
      while (last + 1 < count && runs[last + 1].row == runs[first].row)
        last++;
      const float row_width = runs[last].x + runs[last].width;
      const float left = ((float)gl_width - row_width) / 2;
      for (size_t i = first; i <= last; i++) {
        const text_run *r = &runs[i];
        loom_label_styled(
            ctx,
            (loom_rect){left + r->x, top + (float)r->row * line, r->width + 1,
                        line},
            NULL, r->text, pass == 0 ? clear : swatches[sub_style.color].color,
            size, pass == 0 ? &styles[r->style] : &fills[r->style]);
      }
      first = last + 1;
    }
}

static void draw_subtitles(loom_context *ctx) {
  /* Dialogue rises clear of the playback chrome while it is in. libass can
   * tell dialogue from a sign placed over the picture; for a bitmap track,
   * only a picture wholly in the bottom third is taken for dialogue. */
  const float chrome_top = (float)gl_height * (1 - 200.0f / 1080);
  const bool lift = !subtitle_picker && !subtitle_settings && !search.open &&
                    !sources.open && playback_chrome_wanted(now_ns());
  jf_subs_keep_above(lift ? (int)chrome_top : INT_MAX);
  if (!subtitles_active() && subtitle_texture != 0) {
    jf_renderer_destroy_texture(renderer, subtitle_texture);
    subtitle_texture = 0;
  }
  const float bottom_limit =
      lift ? chrome_top + (gl_height - chrome_top) * (1 - chrome_motion.value)
           : (float)gl_height;
  const bool plain = jf_subs_plain();
  if (plain && plain_text[0] != '\0') {
    draw_plain_subtitles(ctx, plain_text, bottom_limit);
  } else if (subtitle_settings && setting_is_style(focused_setting()) &&
             (plain || !subtitles_active())) {
    /* Nothing said right now: a stand-in, so a change still shows. */
    static const char sample[] = "Subtitles look like this,\n"
                                 "\x01"
                                 "bold"
                                 "\x02"
                                 " and "
                                 "\x03"
                                 "italic"
                                 "\x04"
                                 " too.";
    draw_plain_subtitles(ctx, sample, bottom_limit);
  }
  if (subtitle_texture == 0)
    return;
  static const float whole[4] = {0, 0, 1, 1};
  loom_rect rect = subtitle_rect;
  if (lift && jf_picsubs_ready() && rect.y > (float)gl_height * 2 / 3 &&
      rect.y + rect.h > chrome_top)
    rect.y -= (rect.y + rect.h - chrome_top) * chrome_motion.value;
  loom_premultiplied(ctx, rect, NULL, subtitle_texture, whole, WHITE);
}

static void draw_licenses(loom_context *ctx, float width, float height,
                          float scale) {
  /* Tall enough for the list, or for the screen, whichever runs out first. */
  const float row_height = 78 * scale;
  const float chrome = 164 * scale; /* heading above, hint below */
  const float panel_height =
      minf(height - 240 * scale, chrome + JF_LICENSE_COUNT * row_height);
  const loom_rect panel = {360 * scale, (height - panel_height) / 2,
                           width - 720 * scale, panel_height};
  loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 48 * scale, panel.y + 36 * scale,
                         panel.w - 96 * scale, 44 * scale},
             &panel, "Open source licenses", TEXT, 32 * scale);

  const loom_rect list_rect = {panel.x + 40 * scale, panel.y + 104 * scale,
                               panel.w - 80 * scale, panel.h - 144 * scale};
  const float scroll =
      nav_scroll_begin(&ui, nav_str("licenses"), list_rect, LOOM_VERTICAL,
                       JF_LICENSE_COUNT * row_height, 0, 0);
  const loom_virtual_list list =
      loom_virtual_list_init(list_rect, JF_LICENSE_COUNT, row_height, scroll);
  /* Rows of its own rather than draw_button, for the same reason the sidebar
   * has them: a scrolled list needs every part of a row clipped to the view,
   * not just its text. */
  const loom_rect clip = {list_rect.x - 4 * scale, list_rect.y,
                          list_rect.w + 8 * scale, list_rect.h};
  size_t chosen = JF_LICENSE_COUNT;
  /* A row either side of the view, so there is always a row to move into. */
  const size_t last = min_size(list.last + 1, JF_LICENSE_COUNT);
  for (size_t index = dec(list.first); index < last; index++) {
    const loom_rect raw = loom_virtual_list_item(&list, index);
    const loom_rect row = {raw.x, raw.y + 6 * scale, raw.w, raw.h - 12 * scale};
    const unsigned flags = nav_item(&ui, nav_int(index), row, clip);
    const bool focused = flags & NAV_FOCUSED;
    if (flags & NAV_ACTIVATED)
      chosen = index;
    loom_fill(ctx, row, &clip, flags & NAV_HOVERED ? HOT : CARD, 12 * scale);
    loom_stroke(ctx, row, &clip, focused ? ACCENT : BORDER,
                focused ? 4 * scale : 2 * scale, 12 * scale);
    loom_label(
        ctx,
        (loom_rect){row.x + 24 * scale, row.y + (row.h - 30 * scale) / 2,
                    row.w - 48 * scale, 38 * scale},
        &clip,
        fmt("%s - %s", jf_licenses[index].library, jf_licenses[index].license),
        TEXT, 25 * scale);
  }
  nav_scroll_end(&ui);
  if (chosen < JF_LICENSE_COUNT)
    open_license(chosen);
}

static void draw_license(loom_context *ctx, float width, float height,
                         float scale) {
  if (license_open >= JF_LICENSE_COUNT)
    return;
  /* The licence files are hard-wrapped to 96 columns in the tree, so opening
   * one is a split on newlines and nothing else - no measuring, no re-flow, and
   * the same line breaks the licensor published. Which also means the box can
   * be cut to the text: the ISC licence is fifteen lines and has no business
   * filling a screen. */
  const float size = 17 * scale;
  const float line_height = 24 * scale;
  const float padding = 40 * scale;
  const float heading = 86 * scale;
  const float panel_width =
      minf(width - 240 * scale,
           maxf(620 * scale, license_text_width * size + 2 * padding));
  const float panel_height =
      minf(height - 180 * scale,
           heading + license_line_count * line_height + padding);
  const loom_rect panel = {(width - panel_width) / 2,
                           (height - panel_height) / 2, panel_width,
                           panel_height};
  loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + padding, panel.y + 30 * scale,
                         panel.w - 2 * padding, 40 * scale},
             &panel,
             fmt("%s - %s", jf_licenses[license_open].library,
                 jf_licenses[license_open].license),
             TEXT, 28 * scale);

  const loom_rect view = {panel.x + padding, panel.y + heading,
                          panel.w - 2 * padding, panel.h - heading - padding};
  /* Up and Down, with nothing to focus here, move the text a line at a time. */
  const float extent = (float)license_line_count * line_height;
  const float end = maxf(0, extent - view.h);
  const float scroll = nav_scroll_begin(&ui, nav_str("license"), view,
                                        LOOM_VERTICAL, extent, 0, line_height);
  const loom_virtual_list list =
      loom_virtual_list_init(view, license_line_count, line_height, scroll);
  /* Deep enough to cover the line being cut in half. It grows in with the
   * scroll, so a licence at its top keeps its title readable. */
  const loom_mask outer = ctx->mask;
  loom_fade(ctx, LOOM_VERTICAL, view.y, view.y + view.h, scroll, end,
            line_height * 2);
  for (size_t index = list.first; index < list.last; index++) {
    const loom_rect row = loom_virtual_list_item(&list, index);
    loom_label(ctx, (loom_rect){row.x, row.y, row.w, row.h}, &view,
               license_lines[index], DIM, size);
  }
  nav_scroll_end(&ui);
  ctx->mask = outer;
}

static void draw_settings(loom_context *ctx, float width, float scale) {
  const loom_rect panel = {560 * scale, 230 * scale, width - 1120 * scale,
                           484 * scale};
  loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 48 * scale, panel.y + 42 * scale,
                         panel.w - 96 * scale, 44 * scale},
             &panel, "Settings", TEXT, 32 * scale);
  if (draw_button(ctx,
                  (loom_rect){panel.x + 48 * scale, panel.y + 116 * scale,
                              panel.w - 96 * scale, 72 * scale},
                  animations_enabled ? "Animations: On" : "Animations: Off",
                  nav_str("settings.animations"), scale) &
      NAV_ACTIVATED)
    toggle_animations();
  if (draw_button(ctx,
                  (loom_rect){panel.x + 48 * scale, panel.y + 210 * scale,
                              panel.w - 96 * scale, 72 * scale},
                  "Sign out", nav_str("settings.sign_out"), scale) &
      NAV_ACTIVATED)
    sign_out();
  if (draw_button(ctx,
                  (loom_rect){panel.x + 48 * scale, panel.y + 304 * scale,
                              panel.w - 96 * scale, 72 * scale},
                  "Open source licenses", nav_str("settings.licenses"), scale) &
      NAV_ACTIVATED) {
    screen = SCREEN_LICENSES;
    nav_focus(&ui, nav_child(nav_str("licenses"), nav_int(0)));
  }
}

static void draw_sidebar(loom_context *ctx, float height, float scale) {
  const float panel_width = 354 * scale;
  const loom_rect panel = {-panel_width * (1.0f - sidebar_slide.value), 0,
                           panel_width, height};
  loom_fill(ctx, panel, NULL, PANEL, 0);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 0);
  loom_label(ctx,
             (loom_rect){panel.x + 26 * scale, 28 * scale, panel.w - 52 * scale,
                         34 * scale},
             &panel, "Categories", DIM, 20 * scale);
  const loom_rect list_rect = {panel.x + 18 * scale, 78 * scale,
                               panel.w - 36 * scale, panel.h - 96 * scale};
  const size_t count = categories_row.count + 1; /* the final row is Settings */
  const float row_height = 76 * scale;
  const loom_rect button_clip = {
      list_rect.x - 4 * scale, list_rect.y - 4 * scale, list_rect.w + 8 * scale,
      list_rect.h + 8 * scale};
  /* Closing, it hands the focus back to what it covers. */
  const bool inert = ui.inert;
  nav_set_inert(&ui, sidebar_closing);
  const float scroll =
      nav_scroll_begin(&ui, nav_str("sidebar"), list_rect, LOOM_VERTICAL,
                       (float)count * row_height, 0, 0);
  const loom_virtual_list list =
      loom_virtual_list_init(list_rect, count, row_height, scroll);
  size_t chosen = count;
  /* Every row is declared, so it can open on the library already showing. */
  for (size_t index = 0; index < count; index++) {
    const loom_rect raw = loom_virtual_list_item(&list, index);
    const loom_rect row = {raw.x, raw.y + 5 * scale, raw.w, raw.h - 10 * scale};
    if (screen == SCREEN_GRID && index < categories_row.count &&
        strcmp(categories_row.cards[index].id, grid_parent) == 0)
      nav_default(&ui);
    const unsigned flags = nav_item(&ui, nav_int(index), row, button_clip);
    if (flags & NAV_FOCUSED)
      nav_ring(&ui, row, 10 * scale);
    if (flags & NAV_ACTIVATED)
      chosen = index;
    if (index < list.first || index >= list.last)
      continue;
    loom_fill(ctx, row, &button_clip, flags & NAV_HOVERED ? HOT : CARD,
              10 * scale);
    loom_stroke(ctx, row, &button_clip, BORDER, 1 * scale, 10 * scale);
    loom_label(ctx,
               (loom_rect){row.x + 22 * scale, row.y + 16 * scale,
                           row.w - 44 * scale, 32 * scale},
               &button_clip,
               index < categories_row.count ? categories_row.cards[index].title
                                            : "Settings",
               TEXT, 22 * scale);
  }
  nav_scroll_end(&ui);
  nav_set_inert(&ui, inert);
  if (chosen < count)
    activate_sidebar(chosen);
}

static bool advance_animations(uint64_t now) {
  animated_float_update(&sidebar_slide, now);
  animated_float_update(&chrome_motion, now);
  animated_float_update(&glow_x, now);
  animated_float_update(&glow_y, now);
  if (animated_float_running(&chrome_motion) ||
      animated_float_running(&glow_x) || animated_float_running(&glow_y))
    return true;

  if (sidebar_closing && !animated_float_running(&sidebar_slide) &&
      sidebar_slide.value == 0) {
    sidebar_open = false;
    sidebar_closing = false;
    /* What it covered takes the focus again, which only a frame without it
     * declares; otherwise the next key press finds nothing to move from. */
    return true;
  }
  return animated_float_running(&sidebar_slide);
}

static void build_ui(loom_context *ctx) {
  const float width = (float)gl_width;
  const float height = (float)gl_height;
  const float scale = minf(width / 1920.0f, height / 1080.0f);
  scratch_used = 0;
  poster_requests = 0;
  frame_index++;
  advance_animations(now_ns());

  loom_begin(ctx, width, height);
  ui.animate = animations_enabled;
  ui.duration = NAVIGATION_ANIMATION_NS;
  nav_begin(&ui, ctx, now_ns());
  /* What the sidebar covers cannot take the focus from under it. */
  nav_set_inert(&ui, sidebar_open);
  /* The video plane sits behind this EGL surface. During playback every
   * untouched pixel must remain transparent, otherwise the UI obscures it. */
  if (screen != SCREEN_PLAYBACK)
    loom_fill(ctx, (loom_rect){0, 0, width, height}, NULL, BG, 0);
  if (screen == SCREEN_DETAILS || screen == SCREEN_SEASON) {
    const loom_rect background = {0, 0, width, height};
    const poster_slot *slot = artwork(detail.backdrop_id, detail.backdrop_tag,
                                      JF_IMAGE_BACKDROP, 1920, 1080);
    if (slot != NULL) {
      static const loom_color scrim = {5, 9, 16, 185};
      float uv[4];
      cover_uv(slot, background, uv);
      loom_color tint = {255, 255, 255, 255};
      if (backdrop_texture != slot->texture) {
        backdrop_texture = slot->texture;
        backdrop_fade_started_at = now_ns();
      }
      if (backdrop_fade_started_at != 0) {
        const uint64_t elapsed = now_ns() - backdrop_fade_started_at;
        tint[3] = (uint8_t)min_size(255, elapsed * 255 / 250000000ull);
        if (tint[3] != 255)
          jf_window_frame_requested = true;
      }
      loom_textured(ctx, background, NULL, slot->texture, uv, tint, 0);
      loom_fill(ctx, background, NULL, scrim, 0);
    }
  }
  draw_heading_and_status(ctx, width, height, scale);
  switch (screen) {
  case SCREEN_SERVER:
    draw_server(ctx, width, scale);
    break;
  case SCREEN_AUTH:
    draw_auth(ctx, width, scale);
    break;
  case SCREEN_QUICK:
    draw_quick(ctx, width, scale);
    break;
  case SCREEN_HOME:
    draw_home(ctx, width, height, scale);
    break;
  case SCREEN_GRID:
    draw_grid(ctx, width, height, scale);
    break;
  case SCREEN_DETAILS:
    draw_details(ctx, width, scale);
    break;
  case SCREEN_SEASON:
    draw_season(ctx, width, height, scale);
    break;
  case SCREEN_PLAYBACK:
    draw_subtitles(ctx);
    draw_playback(ctx, width, height, scale);
    break;
  case SCREEN_SETTINGS:
    draw_settings(ctx, width, scale);
    break;
  case SCREEN_LICENSES:
    draw_licenses(ctx, width, height, scale);
    break;
  case SCREEN_LICENSE:
    draw_license(ctx, width, height, scale);
    break;
  }
  if (sidebar_open)
    draw_sidebar(ctx, height, scale);
  if (nav_end(&ui, ACCENT, 4 * scale))
    jf_window_frame_requested = true;
  if (advance_animations(now_ns()))
    jf_window_frame_requested = true;
}

/* -------------------------------------------------------------------- main */

/* How often the subtitle overlay is asked whether it has changed; only a change
 * draws a frame. */
#define SUBTITLE_TICK_NS (33 * 1000000ull)

/* Only these UI features have time-dependent work in normal operation. */
static bool next_deadline(uint64_t now, bool controls_visible, uint64_t *out) {
  bool have = false;
  uint64_t deadline = UINT64_MAX;
  if (screen == SCREEN_QUICK && quick_secret[0] != '\0') {
    deadline = quick_poll_at > now ? quick_poll_at : now;
    have = true;
  }
  if (screen == SCREEN_PLAYBACK && !playback_paused && controls_visible) {
    if (!have || playback_controls_until < deadline)
      deadline = playback_controls_until;
    have = true;
  }
  if (sources.installing[0] != '\0') {
    const uint64_t at = sources.poll_at > now ? sources.poll_at : now;
    if (!have || at < deadline)
      deadline = at;
    have = true;
  }
  if (search.phase == DOWNLOAD_LISTING) {
    const uint64_t at = search.poll_at > now ? search.poll_at : now;
    if (!have || at < deadline)
      deadline = at;
    have = true;
  }
  if (screen == SCREEN_PLAYBACK && subtitles_active()) {
    if (!have || subtitle_tick < deadline)
      deadline = subtitle_tick;
    have = true;
  }
  *out = deadline;
  return have;
}

static int wait_milliseconds(uint64_t now, bool have_deadline,
                             uint64_t deadline) {
  if (!have_deadline)
    return -1;
  const uint64_t remaining = deadline > now ? deadline - now : 0;
  const uint64_t ms =
      remaining / 1000000ull + (remaining % 1000000ull != 0 ? 1 : 0);
  return (int)min_size((size_t)ms, INT32_MAX);
}

/* This application's own backbuffer as a PPM: the only way to check on-device
 * rendering without the VNC grab. */
static void capture_frame(const char *path) {
  const size_t width = gl_width;
  const size_t height = gl_height;
  uint8_t *rgba = malloc(width * height * 4);
  uint8_t *row = malloc(width * 3);
  FILE *file = NULL;
  if (rgba == NULL || row == NULL)
    goto done;
  glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE,
               rgba);
  file = fopen(path, "wb");
  if (file == NULL)
    goto done;
  fprintf(file, "P6\n%zu %zu\n255\n", width, height);
  for (size_t y = 0; y < height; y++) {
    const uint8_t *source = rgba + (height - 1 - y) * width * 4;
    for (size_t x = 0; x < width; x++) {
      row[x * 3 + 0] = source[x * 4 + 0];
      row[x * 3 + 1] = source[x * 4 + 1];
      row[x * 3 + 2] = source[x * 4 + 2];
    }
    fwrite(row, 1, width * 3, file);
  }
  fprintf(stderr, "captured OpenGL framebuffer to %s (%zux%zu)\n", path, width,
          height);
done:
  if (file != NULL)
    fclose(file);
  free(rgba);
  free(row);
}

/* Launched from the TV's app list there is no terminal, so an installed app's
 * output goes nowhere and a failure is invisible. The fallback is to send
 * stdout and stderr to a file next to everything else this app writes - SDL and
 * the Luna bridge report on stdout, and those lines are the only diagnosis when
 * a backend refuses. */
static void log_to_file(void) {
  /* The UI tests report on stderr, wherever it goes. */
#ifdef JF_APP_TEST
  return;
#endif
  if (isatty(2))
    return;
  char path[576];
  /* conf/ rather than the app directory itself: the package can only make the
   * subdirectories world-writable, and the app runs as a jail uid that owns
   * none of them. /tmp is the fallback when even that fails. */
  snprintf(path, sizeof(path), "%s/conf/jellyfin.log", jf_store_root());
  int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file < 0)
    file = open("/tmp/jellyfin.log", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file < 0)
    return;
  dup2(file, 1);
  dup2(file, 2);
  close(file);
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
}

/* A remote, replayed. There is no way to click through this app without a TV or
 * a person, so UI_SCRIPT=drrob presses those buttons a few frames apart and
 * UI_CAPTURE saves the screen it ends on. Letters are the four arrows, `o` for
 * OK and `b` for Back; `.` waits one more beat, which is what a screen that is
 * still fetching needs. */
static const char *script = "";
static size_t script_at;
static unsigned script_wait;

/* Frames between scripted presses. Long enough that a request started by one
 * press has landed before the next, on a LAN. */
#define SCRIPT_BEAT 45

static bool step_script(void) {
  if (script[script_at] == '\0')
    return jf_fetcher_pending(&fetcher) == 0;
  /* Never press a button while a request is outstanding: whether discovery has
   * answered decides what the focused control even is. */
  if (jf_fetcher_pending(&fetcher) != 0)
    return false;
  if (script_wait != 0) {
    script_wait--;
    return false;
  }
  const char command = script[script_at++];
  script_wait = SCRIPT_BEAT;
  uint32_t key = 0;
  switch (command) {
  case 'u':
    key = 103;
    break;
  case 'd':
    key = 108;
    break;
  case 'l':
    key = 105;
    break;
  case 'r':
    key = 106;
    break;
  case 'o':
    key = 28;
    break;
  case 'b':
    key = jf_window_on_webos ? 412 : 158;
    break;
  case 's':
    key = KEY_SUBTITLE;
    break;
  default:
    break;
  }
  if (key != 0)
    on_key(key, true, false);
  if (command == '[' || command == ']') {
    jf_event wheel;
    memset(&wheel, 0, sizeof(wheel));
    wheel.kind = JF_EVENT_POINTER_AXIS;
    wheel.axis.axis = 0;
    wheel.axis.value = (command == '[' ? -180 : 180) * 256;
    on_event(&wheel);
  }
  fprintf(stderr, "script: '%c' -> screen=%d nav=%08x servers=%zu depth=%zu\n",
          command, (int)screen, (unsigned)ui.focus, discovered_count, depth);
  return false;
}

/* What the frame loop keeps between iterations. */
static loom_context ctx;
static probe_overlay overlay;
static probe_timer timer;
static const char *capture_path;
static unsigned capture_after;
static bool controls_visible;
static jf_player_state last_state;
static const char *autoplay;
static char launch_play[64];

static bool app_init(int argc, char **argv) {
  for (size_t i = 0; i < ROW_COUNT; i++) {
    rows[i].cards = home_cards[i];
    rows[i].capacity = ROW_CAPACITY;
  }
  categories_row.cards = category_cards;
  seasons_row.cards = seasons_cards;
  episodes_row.cards = episodes_cards;

  jf_session_device_id(&session);
  jf_api_init();
  snprintf(preferences_path, sizeof(preferences_path),
           "%s/conf/preferences.ini", jf_store_root());
  load_preferences();
  /* Read before a subtitle needs a script the UI face lacks. */
  jf_font_index_warm();
  log_to_file();
  jf_decoders_probe(&decoders);
  const bool restored = jf_session_load(&session);
  if (session.url[0] != '\0') {
    set_text(server_url, sizeof(server_url), session.url);
    set_text(server_name, sizeof(server_name), session.url);
  }

  jf_window_set_handler(on_event);
  const char *appid = getenv("APPID");
  if (!jf_window_init(appid != NULL ? appid : APP_ID, "Jellyfin", 0, 0))
    return false;
  /* How the TV asks the app to close. Absent off-device, where nothing asks. */
  if (!jf_luna_register_lifecycle(jf_window_post_quit, jf_window_post_raise))
    fprintf(stderr, "no webOS lifecycle\n");
  else
    jf_luna_query_hdr();

  glViewport(0, 0, (GLsizei)gl_width, (GLsizei)gl_height);
  glClearColor(9.0f / 255.0f, 13.0f / 255.0f, 22.0f / 255.0f, 1);

  renderer = jf_renderer_create(NULL, 0, 0);
  if (renderer == NULL) {
    fprintf(stderr, "the UI renderer could not start\n");
    return false;
  }
  loom_init(&ctx);
  if (stats_overlay) {
    probe_overlay_init(&overlay, text_vs, text_fs, (int)gl_width,
                       (int)gl_height);
    probe_timer_init(&timer);
  }

  if (!jf_fetcher_init(&fetcher, jf_window_wake)) {
    fprintf(stderr, "no fetcher worker threads\n");
    return false;
  }
  jf_fetcher_set_session(&fetcher, &session);

  /* A stored token skips straight to the home rows; JELLYFIN_ADDRESS makes a
   * fresh install land on the sign-in screen without typing a URL on a TV. */
  if (restored) {
    depth = 0;
    screen = SCREEN_HOME;
    set_status("Signed in as %s", session.user_name);
    load_home();
    jf_events_start(&session, on_items_changed);
  } else {
    /* Development convenience, and the only way a script can sign in: the
     * fields start filled from the environment. Nothing is read from there once
     * a token is stored. */
    const char *address = getenv("JELLYFIN_ADDRESS");
    if (address != NULL) {
      set_text(server_url, sizeof(server_url), address);
      set_text(session.url, sizeof(session.url), address);
      set_text(server_name, sizeof(server_name), address);
      jf_fetcher_set_session(&fetcher, &session);
    }
    const char *user = getenv("JELLYFIN_USER");
    if (user != NULL)
      set_text(username, sizeof(username), user);
    const char *secret = getenv("JELLYFIN_PASSWORD");
    if (secret != NULL)
      set_text(password, sizeof(password), secret);
    if (session.url[0] != '\0') {
      screen = SCREEN_AUTH;
    } else {
      restart_discovery();
    }
  }

  capture_path = getenv("UI_CAPTURE");
  const char *requested_script = getenv("UI_SCRIPT");
  script = requested_script != NULL ? requested_script : "";
  /* Without a script, hold long enough for discovery's three timeouts. */
  capture_after = (capture_path != NULL && script[0] == '\0') ? 240 : 0;
  last_state = jf_player_state_get();
  /* JF_PLAY=<item id>[@ms], or a "play" launch parameter of the same form
   * (`ares-launch -p '{"play":"..."}'`), starts that item once signed in:
   * timing a start on the TV without driving the UI there. */
  autoplay = getenv("JF_PLAY");
  const char *param = argc > 1 ? strstr(argv[1], "\"play\":\"") : NULL;
  if (autoplay == NULL && param != NULL &&
      sscanf(param + 8, "%63[^\"]", launch_play) == 1)
    autoplay = launch_play;
  return true;
}

/* One pass of the event loop after the window's events: results, then a frame
 * if anything asked for one. False when there was nothing to draw, with how
 * long to sleep before the next deadline (-1 for none). */
static bool app_step(int *wait_ms) {
  pump();
  if (autoplay != NULL && screen == SCREEN_HOME) {
    char id[40];
    long long at_ms = 0;
    sscanf(autoplay, "%39[^@]@%lld", id, &at_ms);
    start_playback(id, id, "", (uint64_t)at_ms * 10000ull);
    autoplay = NULL;
  }
  const jf_player_state state = jf_player_state_get();
  if (state != last_state) {
    /* The stream ran out by itself, as a stop would have left the screen: on
     * into the next episode, or back to where playback started. */
    const bool ended = last_state == JF_PLAYING && state == JF_IDLE &&
                       screen == SCREEN_PLAYBACK;
    last_state = state;
    select_wanted_audio();
    if (ended && next_episode.present)
      play_adjacent(&next_episode);
    else if (ended)
      go_back();
    if (screen == SCREEN_PLAYBACK)
      jf_window_frame_requested = true;
  }
  const uint64_t now = now_ns();
  const bool visible = screen == SCREEN_PLAYBACK && playback_chrome_wanted(now);
  if (visible != controls_visible) {
    controls_visible = visible;
    jf_window_frame_requested = true;
  }
  if (screen == SCREEN_PLAYBACK && subtitles_active() && now >= subtitle_tick) {
    subtitle_tick = now + SUBTITLE_TICK_NS;
    if (update_subtitles())
      jf_window_frame_requested = true;
  }
  /* Turned off: the tick has stopped, and the last line is still up. */
  if (subtitle_texture != 0 && !subtitles_active())
    jf_window_frame_requested = true;
  /* A finished script still has to reach its capture. */
  const bool scripted_frame = script[script_at] != '\0' || capture_after > 0 ||
                              (script[0] != '\0' && capture_path != NULL);
  if (!jf_window_drawable ||
      (!jf_window_frame_requested && !jf_player_needs_frame() &&
       !scripted_frame && !capture_requested)) {
    uint64_t deadline = 0;
    const bool have = next_deadline(now, controls_visible, &deadline);
    *wait_ms = wait_milliseconds(now_ns(), have, deadline);
    return false;
  }

  if (stats_overlay) {
    probe_timer_begin(&timer);
    jf_window_frame_requested = true;
  }
  glViewport(0, 0, (GLsizei)gl_width, (GLsizei)gl_height);
  if (screen == SCREEN_PLAYBACK && !jf_player_embedded())
    glClearColor(0, 0, 0, 0); /* Starfish owns the webOS video plane. */
  else
    glClearColor(9.0f / 255.0f, 13.0f / 255.0f, 22.0f / 255.0f, 1);
  glClear(GL_COLOR_BUFFER_BIT);
  if (screen == SCREEN_PLAYBACK)
    jf_player_render(gl_width, gl_height);
  if (jf_window_frame_requested || scripted_frame) {
    jf_window_frame_requested = false;
    const double started = thread_ms();
    build_ui(&ctx);
    phase_frame[PHASE_UI] = thread_ms() - started;
  }
  const double draw_started = thread_ms();
  jf_renderer_draw(renderer, ctx.commands, ctx.count, (float)gl_width,
                   (float)gl_height);
  phase_frame[PHASE_DRAW] = thread_ms() - draw_started;
  if (stats_overlay) {
    char line[PROBE_OVERLAY_COLS + 1];
    probe_overlay_clear(&overlay);
    snprintf(line, sizeof(line), "cpu  %6.2f ms", timer.cpu_ms);
    probe_overlay_line(&overlay, 0, line);
    snprintf(line, sizeof(line), "gpu %s%6.2f ms",
             timer.mode == PROBE_GPU_FINISH ? "*" : " ", timer.gpu_ms);
    probe_overlay_line(&overlay, 1, line);
    snprintf(line, sizeof(line), "%5.1f fps %zu inst",
             timer.frame_ms > 0 ? 1000.0 / timer.frame_ms : 0.0,
             jf_renderer_instances(renderer));
    probe_overlay_line(&overlay, 2, line);
    snprintf(line, sizeof(line), "%u draws %.1fx",
             jf_renderer_batches(renderer),
             jf_renderer_covered(renderer) / ((double)gl_width * gl_height));
    probe_overlay_line(&overlay, 3, line);
    /* The rest are the worst frame of the last second, which is what a
     * stutter is. */
    for (int i = 0; i < PHASE_COUNT; i++)
      if (phase_frame[i] > phase_peak[i])
        phase_peak[i] = phase_frame[i];
    static uint64_t window_started;
    if (now_ns() - window_started >= 1000000000ull) {
      window_started = now_ns();
      memcpy(phase_shown, phase_peak, sizeof(phase_peak));
      memset(phase_peak, 0, sizeof(phase_peak));
      runs_shown = runs_peak;
      runs_peak = 0;
      fprintf(stderr,
              "peak ms: ui %.2f  ass %.2f  composite %.2f (%u runs)  "
              "upload %.2f  "
              "draw %.2f\n",
              phase_shown[PHASE_UI], phase_shown[PHASE_ASS],
              phase_shown[PHASE_COMPOSITE], runs_shown,
              phase_shown[PHASE_UPLOAD], phase_shown[PHASE_DRAW]);
    }
    snprintf(line, sizeof(line), "ui   %6.2f ms", phase_shown[PHASE_UI]);
    probe_overlay_line(&overlay, 4, line);
    snprintf(line, sizeof(line), "ass  %6.2f ms", phase_shown[PHASE_ASS]);
    probe_overlay_line(&overlay, 5, line);
    snprintf(line, sizeof(line), "comp %6.2f ms %4u",
             phase_shown[PHASE_COMPOSITE], runs_shown);
    probe_overlay_line(&overlay, 6, line);
    snprintf(line, sizeof(line), "up %5.2f gl %5.2f", phase_shown[PHASE_UPLOAD],
             phase_shown[PHASE_DRAW]);
    probe_overlay_line(&overlay, 7, line);
    memset(phase_frame, 0, sizeof(phase_frame));
    glEnable(GL_BLEND);
    probe_overlay_draw(&overlay);
    probe_timer_end(&timer);
  }

  if (script[0] != '\0' && step_script() && capture_path != NULL &&
      capture_after == 0)
    capture_after = SCRIPT_BEAT;
  if (capture_after > 0 && --capture_after == 0)
    capture_requested = true;
  if (capture_requested) {
    capture_frame(capture_path != NULL ? capture_path : "jellyfin-capture.ppm");
    capture_requested = false;
    if (capture_path != NULL)
      jf_window_running = false;
  }
  jf_window_swap();
  return true;
}

static void app_deinit(void) {
  jf_events_stop();
  jf_player_deinit();
  jf_fetcher_deinit(&fetcher);
  loom_destroy(&ctx);
  jf_renderer_destroy(renderer);
  jf_luna_deinit();
  jf_window_deinit();
}

#ifndef JF_APP_TEST
int main(int argc, char **argv) {
  if (!app_init(argc, argv))
    return 1;
  while (jf_window_poll()) {
    int wait_ms;
    if (!app_step(&wait_ms))
      jf_window_wait_timeout(wait_ms);
  }
  app_deinit();
  return 0;
}
#endif
