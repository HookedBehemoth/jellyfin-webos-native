/*
 * A Jellyfin client for the TV: discovery, sign-in, home rows, a virtual library grid and
 * the path down to a single episode.
 *
 * The UI is one instanced batch with rasterised glyphs, remote/pointer handling, and
 * virtual-list geometry. Every screen is backed by a real server, so this file is mostly
 * about keeping the render thread free of that: the fetcher runs the requests and image
 * decoding on worker threads, results wake the event loop, and screen state is plain
 * fixed-size storage that a task result is copied into. Nothing the renderer touches is
 * owned by a worker.
 *
 * Video decoding and presentation belong to LG's Starfish pipeline, fed from an FFmpeg
 * demuxer; audio is decoded here and written to ALSA. See jf/player.c.
 */
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../platform/os.h"

#include "../apps/probe.h"
#include "../jf/api.h"
#include "../jf/cfg.h"
#include "../jf/player.h"
#include "../jf/subs.h"
#include "../platform/gl.h"
#include "../platform/luna.h"
#include "../platform/window.h"
#include "../ui/glyph_atlas.h"
#include "../ui/loom.h"
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

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

/* A list that was not on screen last frame appears where it belongs rather than
 * scrolling there. */
static void animate_list(animated_float *motion, uint64_t *drawn_frame,
                         float target) {
  if (*drawn_frame + 1 == frame_index)
    animated_float_set(motion, target, NAVIGATION_ANIMATION_NS);
  else
    animated_float_snap(motion, target);
  *drawn_frame = frame_index;
}

static size_t min_size(size_t a, size_t b) { return a < b ? a : b; }
static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }
/* Saturating decrement, which is what every "move focus back" does. */
static size_t dec(size_t value) { return value > 0 ? value - 1 : 0; }

static void set_text(char *out, size_t out_len, const char *value)
{
    snprintf(out, out_len, "%s", value != NULL ? value : "");
}

/* Per-frame scratch for formatted labels. A draw command borrows the string until the
 * renderer has consumed it, so these must outlive the layout pass - but only that, hence
 * a ring reset every frame. Sized for a full grid page: every card formats a title and a
 * subtitle to fit its width. */
static char scratch[256][160];
static size_t scratch_used;

static const char *fmt(const char *pattern, ...)
{
    if (scratch_used == sizeof(scratch) / sizeof(scratch[0]))
        return "...";
    char *out = scratch[scratch_used++];
    va_list args;
    va_start(args, pattern);
    vsnprintf(out, sizeof(scratch[0]), pattern, args);
    va_end(args);
    return out;
}

/* Formatting for text that is copied into fixed storage immediately, which is every field
 * of a card. Deliberately not the frame scratch: building cards happens while draining
 * tasks, before the frame resets, and one page of sixty items would eat the ring the
 * labels then need. */
static char build_buffer[256];

static const char *build(const char *pattern, ...)
{
    va_list args;
    va_start(args, pattern);
    vsnprintf(build_buffer, sizeof(build_buffer), pattern, args);
    va_end(args);
    return build_buffer;
}

/* ------------------------------------------------------------- poster cache
 *
 * One GL texture per image, not an atlas. Artwork comes back at whatever size the server
 * chose (200x300, 204x300, 534x300 for a library tile), so an atlas would mean cropping
 * every image to a fixed tile and then re-uploading over tiles that a scrolling grid may
 * still be drawing from. A texture per poster keeps each image at its own size, and the
 * batcher already handles the cost: one draw call per distinct texture on screen. */

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
/* Poster requests started this frame. Scrolling a grid can name thirty new posters in one
 * frame; the fetcher has 32 slots shared with page requests, so let the visible ones
 * trickle in over a few frames instead of starving it. */
static unsigned poster_requests;

static bool artwork_loading(const char *id, const char *tag, jf_image_kind kind,
                            uint32_t width, uint32_t height)
{
    for (size_t i = 0; i < CACHE_SIZE; i++) {
        const poster_slot *slot = &slots[i];
        if (strcmp(slot->id, id != NULL ? id : "") == 0 &&
            strcmp(slot->tag, tag != NULL ? tag : "") == 0 && slot->kind == kind &&
            slot->requested_width == width && slot->requested_height == height)
            return slot->loading;
    }
    return false;
}

/* The cached artwork for `id`, or NULL while it is still being fetched. Calling this is
 * also what keeps a slot alive, so it must be called every frame for every visible card. */
static const poster_slot *artwork(const char *id, const char *tag, jf_image_kind kind,
                                  uint32_t width, uint32_t height)
{
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
        /* Never evict something drawn this frame or the one before: that is a slot the
         * screen is using right now, and recycling it would thrash. */
        if (slot->loading || (slot->used != 0 && slot->used + 1 >= frame_index))
            continue;
        if (slot->used < oldest) {
            oldest = slot->used;
            victim = slot;
        }
    }
    if (victim == NULL) {
        /* A navigation frame may still protect the previous screen's slots. Advance that
         * grace period once, even with no requests in flight. */
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

static const poster_slot *poster(const char *id, const char *tag)
{
    return artwork(id, tag, JF_IMAGE_PRIMARY, POSTER_W, POSTER_H);
}

/* The renderer's primitive is a rounded rectangle, so an eight-dot spinner is cheaper
 * than a separate texture or shader program. Its highlighted dot advances with time. */
static void draw_spinner(loom_context *ctx, float x, float y, float radius, const loom_rect *clip,
                         float scale)
{
    jf_window_frame_requested = true;
    const unsigned phase = (unsigned)(now_ns() / 90000000ull) % 8;
    for (unsigned i = 0; i < 8; i++) {
        const float angle = ((float)i / 8.0f) * 6.2831853f - 1.5707963f;
        const float dot = 7.0f * scale;
        loom_color color = {DIM[0], DIM[1], DIM[2], i == phase ? 255 : 72};
        loom_fill(ctx, (loom_rect){x + cosf(angle) * radius - dot / 2,
                                   y + sinf(angle) * radius - dot / 2, dot, dot},
                  clip, color, dot / 2);
    }
}

/* Texture coordinates that fill `rect` with the image, cropping the long axis instead of
 * stretching it. A 534x300 library banner and a 200x300 poster then look right in the same
 * portrait card. */
static void cover_uv(const poster_slot *slot, loom_rect rect, float out[4])
{
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
 * A task's result lives in that task's arena and dies when the UI releases it, so screens
 * keep their own copy. Fixed-size text means the copy is a memcpy into storage that never
 * moves - which is also what the renderer needs, since a draw command holds a pointer, not
 * a string. */

typedef struct {
    char id[40];
    char poster_id[40];
    char poster_tag[40];
    char series_id[40];
    char thumbnail_tag[40];
    char backdrop_id[40];
    char backdrop_tag[40];
    char title[96];
    char episode_title[128];
    char overview[1024];
    char rating[32];
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

static bool card_is(const card *c, const char *kind) { return strcmp(c->kind, kind) == 0; }

static uint32_t date_year(const char *date)
{
    if (date == NULL || strlen(date) < 4)
        return 0;
    char digits[5] = {date[0], date[1], date[2], date[3], '\0'};
    for (int i = 0; i < 4; i++)
        if (digits[i] < '0' || digits[i] > '9')
            return 0;
    return (uint32_t)atoi(digits);
}

/* One line under a card: what distinguishes this item from its neighbours. */
static const char *subtitle_for(const jf_item *item)
{
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
        if (end == 0 && item->status != NULL && strcmp(item->status, "Continuing") == 0)
            return build("%u-Present", start);
        return build("%u", start);
    }
    if (strcmp(type, "Season") == 0)
        return build("%u episodes", item->child_count);
    const uint32_t minutes = jf_item_minutes(item);
    if (item->run_time_ticks != 0 && minutes != 0) {
        if (item->production_year != 0)
            return build("%u  -  %uh %02um", item->production_year, minutes / 60, minutes % 60);
        return build("%uh %02um", minutes / 60, minutes % 60);
    }
    return item->collection_type != NULL ? item->collection_type : type;
}

/* Jellyfin overviews carry a little HTML. Only <br> means anything to a renderer that
 * cannot lay out markup, so it becomes a newline and every other tag is left alone. */
static void set_overview(char *out, size_t out_len, const char *text)
{
    size_t len = 0;
    for (size_t at = 0; text[at] != '\0' && len + 1 < out_len;) {
        if (text[at] == '<') {
            const char *close = strchr(text + at, '>');
            if (close != NULL) {
                char tag[16] = {0};
                size_t n = 0;
                for (const char *p = text + at + 1; p < close && n + 1 < sizeof(tag); p++)
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

static card card_from(const jf_item *item)
{
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
    set_text(out.thumbnail_tag, sizeof(out.thumbnail_tag), item->primary_image_tag);
    if (item->backdrop_image_tag != NULL) {
        set_text(out.backdrop_id, sizeof(out.backdrop_id), item->id);
        set_text(out.backdrop_tag, sizeof(out.backdrop_tag), item->backdrop_image_tag);
    } else if (item->parent_backdrop_image_tag != NULL) {
        set_text(out.backdrop_id, sizeof(out.backdrop_id), item->parent_backdrop_item_id);
        set_text(out.backdrop_tag, sizeof(out.backdrop_tag), item->parent_backdrop_image_tag);
    }
    set_overview(out.overview, sizeof(out.overview), item->overview != NULL ? item->overview : "");

    const char *name = item->name != NULL ? item->name : "";
    if (item->index_number != 0)
        set_text(out.episode_title, sizeof(out.episode_title),
                 build("%u. %s", item->index_number, name));
    else
        set_text(out.episode_title, sizeof(out.episode_title), name);
    if (item->community_rating != 0)
        set_text(out.rating, sizeof(out.rating), build("%.1f", item->community_rating));
    set_text(out.kind, sizeof(out.kind), type);
    set_text(out.title, sizeof(out.title),
             strcmp(type, "Episode") == 0 && item->series_name != NULL ? item->series_name : name);
    /* Each build() reuses one buffer, so every result is copied into the card before the
     * next call. */
    set_text(out.subtitle, sizeof(out.subtitle), subtitle_for(item));
    if (item->run_time_ticks != 0) {
        const uint32_t minutes = jf_item_minutes(item);
        const char *text;
        if (minutes >= 60)
            text = minutes % 60 == 0 ? build("%u hr", minutes / 60)
                                     : build("%u hr %u min", minutes / 60, minutes % 60);
        else if (minutes > 0)
            text = build("%u min", minutes);
        else
            text = build("%llu sec", (unsigned long long)(item->run_time_ticks / 10000000ull));
        set_text(out.runtime, sizeof(out.runtime), text);
    }
    return out;
}

static size_t first_unfinished(const card *cards, size_t count)
{
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
#define ROW_COUNT 3

typedef struct {
    const char *title;
    /* Libraries are 16:9 banners on the server, not posters. Cropping one to a portrait
     * card cuts the library's name out of the middle of it. */
    bool wide;
    card *cards;
    size_t capacity;
    size_t count;
    bool loading;
} item_row;

static void row_fill(item_row *row, const jf_item *items, size_t count)
{
    row->count = min_size(count, row->capacity);
    for (size_t i = 0; i < row->count; i++)
        row->cards[i] = card_from(&items[i]);
    row->loading = false;
}

/* Fixed home layout, top to bottom. Libraries last because the first two are what a
 * returning user actually wants. */
enum { ROW_RESUME, ROW_NEXT_UP, ROW_LIBRARIES };
static card home_cards[ROW_COUNT][ROW_CAPACITY];
static item_row rows[ROW_COUNT] = {
    {"Continue Watching", false, NULL, ROW_CAPACITY, 0, false},
    {"Up Next", false, NULL, ROW_CAPACITY, 0, false},
    {"Libraries", true, NULL, ROW_CAPACITY, 0, false},
};
static card category_cards[CATEGORY_CAPACITY];
static item_row categories_row = {"All categories", true, NULL, CATEGORY_CAPACITY, 0, false};

/* Back cancels the UI's interest in pending sign-in / Quick Connect replies. */
static uint32_t auth_generation;

static screen_id screen = SCREEN_SERVER;
static size_t focus;
static size_t row_focus;
static size_t col_focus[ROW_COUNT];
static float home_scroll;
static animated_float home_scroll_motion;
static animated_float row_offset_motion[ROW_COUNT];
static loom_rect home_rect;
static float home_row_height = 470;
static bool home_reveal;

/* Where Back goes, as a stack rather than a rule per screen.
 *
 * The rule version was wrong in the ordinary case: reaching a show from a home row and
 * pressing Back returned to whichever library grid happened to be loaded, because "is a
 * grid loaded" is not the same question as "is that where I came from". An entry carries
 * enough to re-enter a screen, not a snapshot of it - re-entering refetches, which is what
 * keeps a details screen current after playback marks something watched. */
typedef struct {
    screen_id screen;
    /* The grid's library, the details item, or the season. */
    card card;
    /* The series a season belongs to, so a season can be re-entered. */
    card series;
    size_t selected;
    float scroll;
    size_t focus;
    size_t row_focus;
    size_t col_focus[ROW_COUNT];
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
static float license_scroll;
/* Width of the longest line at a font size of one, measured once when the
 * licence is opened. The panel is then sized to the text rather than to the
 * screen. */
static float license_text_width;
/* Set by the draw, used by the key handler: one press moves one line of
 * whatever size that screen turned out to be. */
static float license_line_height = 26;
static float licenses_scroll;
static bool licenses_reveal;

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
/* Absolute item index currently stored in each window entry. Without it a stale card from
 * a page that scrolled away would be drawn as if it were the item that now occupies the
 * same ring slot. */
static uint32_t grid_at[WINDOW_SIZE];
static uint32_t grid_requested[WINDOW_PAGES];
static size_t grid_selected;
static float grid_scroll;
static animated_float grid_scroll_motion;
static loom_rect grid_rect;
static size_t grid_columns = 6;
static float grid_row_height = 360;

/* Details and season screens. */
static card detail;
static char detail_extra[96];
/* Backdrops live longer than a details screen. Track their transition here
 * rather than on the cache slot, so opening another item also fades an
 * already-resident texture in, while moving between a series and its seasons
 * keeps the backdrop that is already showing. */
static uint32_t backdrop_texture;
static uint64_t backdrop_fade_started_at;
static card seasons_cards[SEASONS_CAPACITY];
static card episodes_cards[EPISODES_CAPACITY];
static item_row seasons_row = {"Seasons", false, NULL, SEASONS_CAPACITY, 0, false};
static item_row episodes_row = {"Episodes", false, NULL, EPISODES_CAPACITY, 0, false};
static size_t episode_selected;
static card season_detail;
static float episode_scroll;
static animated_float episode_scroll_motion;
static loom_rect episode_rect;
static float episode_row_height = 170;
static bool episode_reveal;
static bool select_unfinished_season;
static bool select_unfinished_episode;
static char playback_title[160];
static char playback_item_id[40];
static bool playback_paused;
static screen_id playback_return_screen;
static size_t playback_return_focus;
static uint64_t playback_started_at;
static uint64_t playback_progress_at;
/* The player chrome is deliberately transient: it never covers a scene for more than
 * three seconds unless playback is paused. */
static uint64_t playback_controls_until;
#define PLAYBACK_CONTROLS_NS (3ull * 1000000000ull)

/* Text entry, remote, and pointer input. */
static char server_url[512];
static char username[256];
static char password[256];
static char password_mask[257];
static loom_rect url_rect, username_rect, password_rect;
static edit_field active_field = EDIT_NONE;
static bool capture_requested;

static float cursor_x = -1;
static float cursor_y = -1;
static bool cursor_present;
static bool pointer_press;

/* The sidebar is an overlay containing every library category and the one settings row. */
static bool sidebar_open;
static bool sidebar_closing;
static size_t sidebar_focus;
static float sidebar_scroll;
static animated_float sidebar_slide;
static animated_float sidebar_scroll_motion;
static bool sidebar_reveal;
static screen_id sidebar_return_screen;

typedef struct {
  animated_float x, y, w, h;
  loom_rect rendered;
  unsigned owner;
  bool known;
} navigation_cursor;
static navigation_cursor focus_cursor;
static bool focus_cursor_move_requested;

static char status_text[200];
static bool status_error;

static void set_status(const char *pattern, ...)
{
    jf_window_frame_requested = true;
    va_list args;
    va_start(args, pattern);
    vsnprintf(status_text, sizeof(status_text), pattern, args);
    va_end(args);
    status_error = false;
}

static void set_error(const char *pattern, ...)
{
    jf_window_frame_requested = true;
    va_list args;
    va_start(args, pattern);
    vsnprintf(status_text, sizeof(status_text), pattern, args);
    va_end(args);
    status_error = true;
}

/* --------------------------------------------------------------- requests */

static bool is_auth_job(jf_job job)
{
    return job == JF_JOB_PROBE || job == JF_JOB_LOGIN || job == JF_JOB_QUICK_INITIATE ||
           job == JF_JOB_QUICK_POLL || job == JF_JOB_QUICK_AUTHENTICATE;
}

static jf_task *request(jf_job job, uint32_t tag)
{
    return jf_fetcher_submit(&fetcher, job, is_auth_job(job) ? auth_generation : tag);
}

static void simple(jf_job job)
{
    jf_task *task = request(job, 0);
    if (task != NULL)
        jf_fetcher_start(&fetcher, task);
}

static void load_home(void)
{
    for (size_t i = 0; i < ROW_COUNT; i++)
        rows[i].loading = true;
    categories_row.loading = true;
    simple(JF_JOB_RESUME);
    simple(JF_JOB_NEXT_UP);
    simple(JF_JOB_VIEWS);
}

static void restart_discovery(void)
{
    discovered_count = 0;
    discovering = true;
    discovery_until = now_ns() + 30000000000ull;
    simple(JF_JOB_DISCOVER);
}

/* Ask for the page containing `index`, unless that page is already in the window or
 * already in flight. */
static void request_page(uint32_t index)
{
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

static void open_grid(const card *source)
{
    set_text(grid_parent, sizeof(grid_parent), source->id);
    set_text(grid_title, sizeof(grid_title), source->title);
    grid_total = 0;
    grid_loading = true;
    grid_selected = 0;
    grid_scroll = 0;
    memset(grid_cards, 0, sizeof(grid_cards));
    for (size_t i = 0; i < WINDOW_SIZE; i++)
        grid_at[i] = UINT32_MAX;
    for (size_t i = 0; i < WINDOW_PAGES; i++)
        grid_requested[i] = UINT32_MAX;
    screen = SCREEN_GRID;
    request_page(0);
}

static void open_details(const card *source)
{
    select_unfinished_season = true;
    if (strcmp(detail.backdrop_id, source->backdrop_id) != 0) {
      backdrop_texture = 0;
      backdrop_fade_started_at = 0;
    }
    detail = *source;
    detail_extra[0] = '\0';
    seasons_row.count = 0;
    seasons_row.loading = false;
    focus = 0;
    screen = SCREEN_DETAILS;

    /* An episode plays as itself; a series needs its seasons before anything on this
     * screen can be selected. */
    jf_task *task = request(JF_JOB_ITEM, 0);
    if (task == NULL)
        return;
    set_text(task->a, sizeof(task->a), detail.id);
    jf_fetcher_start(&fetcher, task);

    if (card_is(&detail, "Series")) {
        seasons_row.loading = true;
        jf_task *seasons = request(JF_JOB_SEASONS, 0);
        if (seasons == NULL)
            return;
        set_text(seasons->a, sizeof(seasons->a), detail.id);
        jf_fetcher_start(&fetcher, seasons);
    }
}

static void open_season(const card *source)
{
  select_unfinished_episode = true;
  season_detail = *source;
  episode_scroll = 0;
  episode_reveal = true;
  episodes_row.count = 0;
  episodes_row.loading = true;
  episode_selected = 0;
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
static card library_card(void)
{
    card out;
    memset(&out, 0, sizeof(out));
    out.present = true;
    set_text(out.id, sizeof(out.id), grid_parent);
    set_text(out.title, sizeof(out.title), grid_title);
    return out;
}

static stack_entry here(void)
{
    stack_entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.screen = screen;
    entry.card = screen == SCREEN_GRID ? library_card()
                 : screen == SCREEN_SEASON ? season_detail
                                           : detail;
    entry.series = detail;
    entry.selected = screen == SCREEN_GRID     ? grid_selected
                     : screen == SCREEN_SEASON ? episode_selected
                                               : 0;
    entry.scroll = screen == SCREEN_HOME       ? home_scroll
                   : screen == SCREEN_SEASON   ? episode_scroll
                                                : grid_scroll;
    entry.focus = focus;
    entry.row_focus = row_focus;
    memcpy(entry.col_focus, col_focus, sizeof(col_focus));
    return entry;
}

static void push(void)
{
    if (depth == sizeof(stack) / sizeof(stack[0])) {
        /* Deeper than any real path through this app; drop the oldest rather than refuse
         * to navigate. */
        memmove(&stack[0], &stack[1], (depth - 1) * sizeof(stack[0]));
        depth--;
    }
    stack[depth++] = here();
}

static void pop(void)
{
    if (depth == 0) {
        jf_window_running = false;
        return;
    }
    const stack_entry entry = stack[--depth];
    row_focus = entry.row_focus;
    memcpy(col_focus, entry.col_focus, sizeof(col_focus));
    switch (entry.screen) {
    case SCREEN_HOME:
        screen = SCREEN_HOME;
        home_scroll = entry.scroll;
        break;
    case SCREEN_GRID:
        /* Returning to the grid we are still holding pages for is free; a different
         * library has to be fetched again. */
        if (strcmp(grid_parent, entry.card.id) == 0 && grid_total != 0)
            screen = SCREEN_GRID;
        else
            open_grid(&entry.card);
        grid_selected = entry.selected;
        grid_scroll = entry.scroll;
        break;
    case SCREEN_DETAILS:
        open_details(&entry.card);
        focus = entry.focus;
        select_unfinished_season = false;
        break;
    case SCREEN_SEASON:
        detail = entry.series;
        open_season(&entry.card);
        episode_selected = entry.selected;
        episode_scroll = entry.scroll;
        select_unfinished_episode = false;
        break;
    default:
        /* Nothing above the sign-in screens is ever pushed. */
        screen = entry.screen;
        focus = entry.focus;
        break;
    }
}

/* ------------------------------------------------------------ sign-in flow */

/* Re-authentications attempted since the last success. Bounded, or a server that rejects
 * a valid-looking password turns into a login loop. */
static unsigned relogin_attempts;
static void use_server(const char *address, const char *name);

static void signed_in(const jf_auth *auth, const char *used_password)
{
    auth_generation++;
    const bool silent = screen == SCREEN_HOME && session.token[0] != '\0';
    set_text(session.token, sizeof(session.token), auth->access_token);
    set_text(session.user_id, sizeof(session.user_id), auth->user_id);
    set_text(session.user_name, sizeof(session.user_name), auth->user_name);
    set_text(session.password, sizeof(session.password), used_password);
    jf_fetcher_set_session(&fetcher, &session);
    jf_session_save(&session);
    relogin_attempts = 0;
    set_status("Signed in as %s", session.user_name);
    if (silent) {
        /* A token replaced underneath a screen that is already up: reload what failed, do
         * not throw the user back to the top. */
        load_home();
        return;
    }
    screen = SCREEN_HOME;
    home_scroll = 0;
    row_focus = 0;
    memset(col_focus, 0, sizeof(col_focus));
    depth = 0;
    load_home();
}

/* Replace a rejected token without involving the user.
 *
 * Jellyfin invalidates a device's previous token when that device signs in again, so a
 * stored token going stale is routine rather than exceptional - signing the user out and
 * asking for a password on a TV remote is the wrong response when the password is right
 * there. */
static bool reauthenticate(void)
{
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

static void sign_out(void)
{
    auth_generation++;
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
    focus = 0;
    status_text[0] = '\0';
    status_error = false;
}

/* ------------------------------------------------------------ task results */

static const char *job_name(jf_job job)
{
    switch (job) {
    case JF_JOB_DISCOVER: return "discover";
    case JF_JOB_PROBE: return "probe";
    case JF_JOB_LOGIN: return "login";
    case JF_JOB_QUICK_INITIATE: return "quick_initiate";
    case JF_JOB_QUICK_POLL: return "quick_poll";
    case JF_JOB_QUICK_AUTHENTICATE: return "quick_authenticate";
    case JF_JOB_VIEWS: return "views";
    case JF_JOB_RESUME: return "resume";
    case JF_JOB_NEXT_UP: return "next_up";
    case JF_JOB_CHILDREN: return "children";
    case JF_JOB_ITEM: return "item";
    case JF_JOB_SEASONS: return "seasons";
    case JF_JOB_EPISODES: return "episodes";
    case JF_JOB_POSTER: return "poster";
    case JF_JOB_PLAYBACK_STARTED: return "playback_started";
    case JF_JOB_PLAYBACK_PROGRESS: return "playback_progress";
    }
    return "?";
}

static void on_failure(jf_task *task)
{
    fprintf(stderr, "%s failed: %s\n", job_name(task->job), task->error);
    switch (task->job) {
    /* A missing poster is normal - plenty of items have none - so it marks the slot done
     * and leaves the placeholder card showing. The id stays, so the same item is not asked
     * for again every frame. */
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
        if (strcmp(task->error, "ClientError") == 0 || strcmp(task->error, "Unauthorized") == 0 ||
            strcmp(task->error, "ServerError") == 0) {
            screen = SCREEN_AUTH;
            focus = 0;
            set_error("%s for %s at %s", strcmp(task->error, "ServerError") == 0
                                           ? "Internal server error"
                                           : "Request failed",
                      session.user_name, session.url);
        }
        break;
    /* A rejected token is the one failure with a recovery: the stored credentials are
     * stale, so drop them rather than loop on 401s. Anything else - a server restarting, a
     * dropped Wi-Fi association - keeps them, because deleting a good token over a
     * transient error means typing a password back in on a TV remote. */
    case JF_JOB_VIEWS:
    case JF_JOB_RESUME:
    case JF_JOB_NEXT_UP:
        if (strcmp(task->error, "Unauthorized") == 0) {
            if (!reauthenticate())
                sign_out();
            return;
        }
        if (strcmp(task->error, "ClientError") == 0 || strcmp(task->error, "ServerError") == 0) {
            screen = SCREEN_AUTH;
            focus = 0;
            set_error("%s for %s at %s", strcmp(task->error, "ServerError") == 0
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
        /* A stored password the server no longer accepts: forget it rather than retry it
         * on every screen. */
        if (relogin_attempts != 0)
            sign_out();
        break;
    case JF_JOB_ITEM:
    case JF_JOB_SEASONS:
    case JF_JOB_EPISODES:
        if (strcmp(task->error, "ClientError") == 0 || strcmp(task->error, "Unauthorized") == 0 ||
            strcmp(task->error, "ServerError") == 0) {
            screen = SCREEN_AUTH;
            focus = 0;
            set_error("%s for %s at %s", strcmp(task->error, "ServerError") == 0
                                           ? "Internal server error"
                                           : "Request failed",
                      session.user_name, session.url);
            return;
        }
        set_error("%s failed: %s", job_name(task->job), task->error);
        break;
    default:
        set_error("%s failed: %s", job_name(task->job), task->error);
        break;
    }
}

static void consume(jf_task *task)
{
    if (is_auth_job(task->job) && task->tag != auth_generation)
        return;
    if (task->state == JF_TASK_FAILED) {
        on_failure(task);
        return;
    }
    switch (task->job) {
    case JF_JOB_DISCOVER:
        for (size_t incoming = 0;
             incoming < task->server_count && discovered_count < DISCOVERED_CAPACITY; incoming++) {
            bool seen = false;
            for (size_t index = 0; index < discovered_count; index++)
                seen = seen || strcmp(discovered[index].address, task->servers[incoming].address) == 0;
            if (seen)
                continue;
            set_text(discovered[discovered_count].name, sizeof(discovered[0].name),
                     task->servers[incoming].name);
            set_text(discovered[discovered_count].address, sizeof(discovered[0].address),
                     task->servers[incoming].address);
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
        /* Also on stdout: on a TV the log is often easier to reach than the screen, and
         * this is the number the user has to read out. */
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
        /* Music and playlists have no poster grid worth opening from here, and the scope
         * for this client is video. */
        jf_item keep[CATEGORY_CAPACITY];
        size_t count = 0;
        for (size_t i = 0; i < task->list.count && count < CATEGORY_CAPACITY; i++) {
            const char *kind = task->list.items[i].collection_type;
            if (kind != NULL && (strcmp(kind, "music") == 0 || strcmp(kind, "playlists") == 0))
                continue;
            keep[count++] = task->list.items[i];
        }
        row_fill(&rows[ROW_LIBRARIES], keep, count);
        row_fill(&categories_row, keep, count);
        break;
    }

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
        detail = card_from(&task->one);
        set_text(detail_extra, sizeof(detail_extra), task->one.official_rating);
        break;

    case JF_JOB_SEASONS:
        if (strcmp(task->a, detail.id) != 0)
            return;
        row_fill(&seasons_row, task->list.items, task->list.count);
        if (select_unfinished_season && screen == SCREEN_DETAILS) {
            focus = first_unfinished(seasons_row.cards, seasons_row.count);
            select_unfinished_season = false;
        }
        break;

    case JF_JOB_EPISODES:
        if (strcmp(task->b, season_detail.id) != 0)
            return;
        row_fill(&episodes_row, task->list.items, task->list.count);
        if (select_unfinished_episode && screen == SCREEN_SEASON) {
            episode_selected = first_unfinished(episodes_row.cards, episodes_row.count);
            episode_reveal = true;
            select_unfinished_episode = false;
        }
        break;

    case JF_JOB_POSTER:
        if (task->has_image) {
            poster_slot *slot = &slots[task->tag];
            slot->texture = jf_renderer_create_texture(renderer, task->image.width,
                                                       task->image.height, task->image.rgb);
            slot->width = task->image.width;
            slot->height = task->image.height;
            slot->loading = false;
        }
        break;
    case JF_JOB_PLAYBACK_STARTED:
    case JF_JOB_PLAYBACK_PROGRESS:
        break;
    }
}

static void report_playback(jf_job job);

static void pump(void)
{
    for (jf_task *task = jf_fetcher_finished(&fetcher); task != NULL;
         task = jf_fetcher_finished(&fetcher)) {
        if (task->job != JF_JOB_QUICK_POLL || task->state != JF_TASK_READY ||
            task->quick.authenticated)
            jf_window_frame_requested = true;
        consume(task);
        jf_fetcher_release(&fetcher, task);
    }
    if (screen == SCREEN_QUICK && quick_secret[0] != '\0' && now_ns() > quick_poll_at) {
        quick_poll_at = now_ns() + 2000000000ull;
        jf_task *task = request(JF_JOB_QUICK_POLL, 0);
        if (task != NULL) {
            set_text(task->a, sizeof(task->a), quick_secret);
            jf_fetcher_start(&fetcher, task);
        }
    }
    if (screen == SCREEN_PLAYBACK && !playback_paused && playback_item_id[0] != '\0' &&
        jf_player_state_get() == JF_PLAYING && now_ns() >= playback_progress_at) {
        report_playback(JF_JOB_PLAYBACK_PROGRESS);
        playback_progress_at = now_ns() + 15000000000ull;
    }
}

/* --------------------------------------------------------------- text entry */

static char *field_buffer(edit_field which, size_t *out_len)
{
    switch (which) {
    case EDIT_URL: *out_len = sizeof(server_url); return server_url;
    case EDIT_USERNAME: *out_len = sizeof(username); return username;
    case EDIT_PASSWORD: *out_len = sizeof(password); return password;
    default: *out_len = 0; return NULL;
    }
}

static void rect_ints(loom_rect rect, int out[4])
{
    out[0] = (int)rect.x;
    out[1] = (int)rect.y;
    out[2] = (int)rect.w;
    out[3] = (int)rect.h;
}

static void end_edit(void)
{
    if (active_field == EDIT_NONE)
        return;
    jf_window_end_text_input();
    active_field = EDIT_NONE;
}

static void begin_edit(edit_field which, loom_rect rect)
{
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

static void append_text(const char *text)
{
    size_t capacity = 0;
    char *target = field_buffer(active_field, &capacity);
    if (target == NULL)
        return;
    const size_t length = strlen(target);
    snprintf(target + length, capacity - length, "%s", text);
}

static void erase_text(size_t count)
{
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

/* A slot that has not been paged in yet. Returning a pointer to this rather than a
 * temporary keeps every card the renderer sees alive past the frame. */
static card blank_card;

static const card *grid_card(size_t index)
{
    if (index >= grid_total)
        return NULL;
    const size_t ring = index % WINDOW_SIZE;
    if (grid_at[ring] != index)
        return NULL;
    return grid_cards[ring].present ? &grid_cards[ring] : NULL;
}

static void use_server(const char *address, const char *name)
{
    auth_generation++;
    set_text(server_url, sizeof(server_url), address);
    set_text(session.url, sizeof(session.url), address);
    set_text(server_name, sizeof(server_name), name != NULL && name[0] != '\0' ? name : address);
    jf_fetcher_set_session(&fetcher, &session);
    screen = SCREEN_AUTH;
    focus = 0;
    status_text[0] = '\0';
    status_error = false;
}

static void activate(void);

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
  license_scroll = 0;
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

static void open_sidebar(void)
{
    end_edit();
    sidebar_open = true;
    sidebar_closing = false;
    sidebar_focus = min_size(sidebar_focus, categories_row.count);
    sidebar_reveal = true;
    sidebar_return_screen = screen;
    animated_float_set(&sidebar_slide, 1.0f, SIDEBAR_ANIMATION_NS);
}

static void close_sidebar(void) {
  if (!sidebar_open || sidebar_closing)
    return;
  sidebar_closing = true;
  animated_float_set(&sidebar_slide, 0.0f, NAVIGATION_ANIMATION_NS);
}

static void activate_sidebar(void)
{
  close_sidebar();
  if (sidebar_focus < categories_row.count) {
    const card chosen = categories_row.cards[sidebar_focus];
    push();
    open_grid(&chosen);
    return;
  }
    screen = SCREEN_SETTINGS;
    focus = 0;
}

static void go_back(void)
{
    if (sidebar_open) {
      close_sidebar();
      return;
    }
    if (screen == SCREEN_PLAYBACK) {
        report_playback(JF_JOB_PLAYBACK_PROGRESS);
        jf_player_stop();
        playback_paused = false;
        playback_item_id[0] = '\0';
        screen = playback_return_screen;
        focus = playback_return_focus;
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
        focus = 0;
        break;
    case SCREEN_QUICK:
        auth_generation++;
        quick_secret[0] = '\0';
        screen = SCREEN_AUTH;
        focus = 0;
        break;
    case SCREEN_SETTINGS:
        screen = sidebar_return_screen;
        focus = 0;
        break;
    case SCREEN_LICENSES:
      screen = SCREEN_SETTINGS;
      focus = 2;
      break;
    case SCREEN_LICENSE:
      focus = min_size(license_open, JF_LICENSE_COUNT - 1);
      open_license(JF_LICENSE_COUNT); /* frees the text */
      screen = SCREEN_LICENSES;
      licenses_reveal = true;
      break;
    default:
        pop();
        break;
    }
}

/* The transport row. The play/pause button is the one that toggles; the rest seek by
 * their own number of seconds. */
typedef enum {
    ACTION_PREVIOUS,
    ACTION_BACK_30,
    ACTION_BACK_10,
    ACTION_PAUSE,
    ACTION_FORWARD_10,
    ACTION_FORWARD_30,
    ACTION_NEXT,
    ACTION_SUBTITLES,
    ACTION_AUDIO,
} playback_action;

static const struct {
    const char *label;
    playback_action action;
} playback_buttons[] = {
    {"|<", ACTION_PREVIOUS},   {"-30", ACTION_BACK_30},   {"-10", ACTION_BACK_10},
    {"Pause", ACTION_PAUSE},   {"+10", ACTION_FORWARD_10}, {"+30", ACTION_FORWARD_30},
    {">|", ACTION_NEXT},       {"Subtitles", ACTION_SUBTITLES}, {"Audio", ACTION_AUDIO},
};
#define PLAYBACK_BUTTON_COUNT (sizeof(playback_buttons) / sizeof(playback_buttons[0]))
#define PLAY_PAUSE_INDEX 3

static void report_playback(jf_job job)
{
    if (playback_item_id[0] == '\0')
        return;
    jf_task *task = request(job, 0);
    if (task == NULL)
        return;
    set_text(task->a, sizeof(task->a), playback_item_id);
    const int position_ms = jf_player_position();
    task->position_ticks = (uint64_t)(position_ms > 0 ? position_ms : 0) * 10000ull;
    jf_fetcher_start(&fetcher, task);
}

static void start_playback(const char *id, const char *title, uint64_t resume_ticks)
{
    char stream[1024];
    jf_stream_url(&session, id, stream, sizeof(stream));
    if (stream[0] == '\0') {
        set_error("Could not build a stream URL");
        return;
    }
    /* The transcode request carries the hardware capability constraints. Keep this
     * comfortably above a long reverse-proxy address plus access token. */
    char transcode[2048];
    jf_transcode_url(&session, id, transcode, sizeof(transcode));
    const uint64_t resume_ms = resume_ticks / 10000ull;
    const int start_ms = resume_ms > INT32_MAX ? INT32_MAX : (int)resume_ms;
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
    playback_paused = false;
    playback_started_at = now_ns();
    playback_progress_at = playback_started_at + 15000000000ull;
    playback_return_screen = screen;
    playback_return_focus = focus;
    focus = PLAY_PAUSE_INDEX;
    playback_controls_until = now_ns() + PLAYBACK_CONTROLS_NS;
    screen = SCREEN_PLAYBACK;
    report_playback(JF_JOB_PLAYBACK_STARTED);
}

static void toggle_playback(void)
{
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

static void activate_playback(void)
{
    playback_controls_until = now_ns() + PLAYBACK_CONTROLS_NS;
    switch (playback_buttons[min_size(focus, PLAYBACK_BUTTON_COUNT - 1)].action) {
    case ACTION_BACK_30: jf_player_seek(-30); break;
    case ACTION_BACK_10: jf_player_seek(-10); break;
    case ACTION_PAUSE: toggle_playback(); break;
    case ACTION_FORWARD_10: jf_player_seek(10); break;
    case ACTION_FORWARD_30: jf_player_seek(30); break;
    case ACTION_PREVIOUS:
    case ACTION_NEXT:
        set_status("Episode navigation is not available for this item");
        break;
    case ACTION_SUBTITLES: {
      const int count = jf_player_subtitle_count();
      if (count == 0) {
        set_status("No subtitle tracks are available");
        break;
      }
      /* Off, then each track in turn, then off again. A picker is a screen;
       * this is a button that already exists. */
      const int next = jf_player_subtitle_current() + 1;
      if (next >= count) {
        jf_player_subtitle_select(-1);
        set_status("Subtitles off");
      } else {
        jf_player_subtitle_select(next);
        set_status("Subtitles: %s", jf_player_subtitle_name(next));
      }
      break;
    }
    case ACTION_AUDIO: set_status("This stream has one audio track"); break;
    }
}

static void activate_server(void)
{
    if (focus == 0) {
        begin_edit(EDIT_URL, url_rect);
        return;
    }
    if (focus == 1) {
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
        return;
    }
    const size_t index = focus - 2;
    if (index < discovered_count) {
        use_server(discovered[index].address, discovered[index].name);
    } else if (index == discovered_count) {
        restart_discovery();
    }
}

static void activate_details(void)
{
    if (card_is(&detail, "Series")) {
        if (focus < seasons_row.count) {
            push();
            open_season(&seasons_row.cards[focus]);
        }
        return;
    }
    if (focus == 0)
        start_playback(detail.id, detail.title, detail.playback_position_ticks);
}

static void activate(void)
{
    switch (screen) {
    case SCREEN_SERVER:
        activate_server();
        break;
    case SCREEN_AUTH:
        switch (focus) {
        case 0: begin_edit(EDIT_USERNAME, username_rect); break;
        case 1: begin_edit(EDIT_PASSWORD, password_rect); break;
        case 2: {
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
            break;
        }
        default:
            simple(JF_JOB_QUICK_INITIATE);
            set_status("Requesting a Quick Connect code...");
            break;
        }
        break;
    case SCREEN_QUICK:
        go_back();
        break;
    case SCREEN_HOME: {
        const item_row *row = &rows[row_focus];
        const size_t index = col_focus[row_focus];
        if (index >= row->count)
            return;
        const card chosen = row->cards[index];
        push();
        if (row_focus == ROW_LIBRARIES)
            open_grid(&chosen);
        else
            open_details(&chosen);
        break;
    }
    case SCREEN_GRID: {
        const card *chosen = grid_card(grid_selected);
        if (chosen != NULL) {
            const card copy = *chosen;
            push();
            open_details(&copy);
        }
        break;
    }
    case SCREEN_DETAILS:
        activate_details();
        break;
    case SCREEN_SEASON:
        if (episode_selected < episodes_row.count)
            start_playback(episodes_row.cards[episode_selected].id,
                           episodes_row.cards[episode_selected].episode_title,
                           episodes_row.cards[episode_selected].playback_position_ticks);
        break;
    case SCREEN_PLAYBACK:
        activate_playback();
        break;
    case SCREEN_SETTINGS:
      if (focus == 0) {
        animations_enabled = !animations_enabled;
        save_preferences();
        if (!animations_enabled) {
          animated_float_snap(&sidebar_slide, sidebar_open ? 1.0f : 0.0f);
          animated_float_snap(&home_scroll_motion, home_scroll);
          animated_float_snap(&grid_scroll_motion, grid_scroll);
          animated_float_snap(&episode_scroll_motion, episode_scroll);
          animated_float_snap(&sidebar_scroll_motion, sidebar_scroll);
          for (size_t index = 0; index < ROW_COUNT; index++)
            animated_float_snap(&row_offset_motion[index], 0);
          focus_cursor.known = false;
        }
      } else if (focus == 1) {
        sign_out();
      } else {
        screen = SCREEN_LICENSES;
        focus = 0;
        licenses_reveal = true;
      }
        break;
    case SCREEN_LICENSES:
      open_license(min_size(focus, JF_LICENSE_COUNT - 1));
      break;
    case SCREEN_LICENSE:
      break;
    }
}

/* How many focusable things the current screen has, so navigation clamps without every
 * caller knowing the layout. */
static size_t focus_count(void)
{
    switch (screen) {
    case SCREEN_SERVER: return discovered_count + 3;
    case SCREEN_AUTH: return 4;
    case SCREEN_QUICK: return 1;
    case SCREEN_PLAYBACK: return PLAYBACK_BUTTON_COUNT;
    case SCREEN_SETTINGS:
      return 3;
    case SCREEN_LICENSES:
      return JF_LICENSE_COUNT;
    case SCREEN_LICENSE:
      return 1;
    case SCREEN_DETAILS:
        return card_is(&detail, "Series") ? (seasons_row.count > 0 ? seasons_row.count : 1) : 1;
    default: return 1;
    }
}

typedef enum { DIR_UP, DIR_DOWN, DIR_LEFT, DIR_RIGHT } direction;

static size_t grid_rows_count(void)
{
    return (grid_total + grid_columns - 1) / grid_columns;
}

static size_t grid_move(size_t selected, size_t total, size_t columns, direction where)
{
    if (total == 0)
        return selected;
    switch (where) {
    case DIR_LEFT: return dec(selected);
    case DIR_RIGHT: return min_size(selected + 1, total - 1);
    case DIR_UP: return selected >= columns ? selected - columns : selected;
    case DIR_DOWN: return selected + columns < total ? selected + columns : selected;
    }
    return selected;
}

static void move_grid(direction where)
{
    const size_t next = grid_move(grid_selected, grid_total, grid_columns, where);
    if (next == grid_selected)
        return;
    grid_selected = next;
    focus_cursor_move_requested = true;
    const loom_virtual_list list =
        loom_virtual_list_init(grid_rect, grid_rows_count(), grid_row_height, grid_scroll);
    grid_scroll =
        loom_virtual_list_reveal(&list, grid_selected / grid_columns, 0);
    request_page((uint32_t)grid_selected);
}

static void move_home(direction where)
{
  const size_t old_row = row_focus;
  const size_t old_column = col_focus[row_focus];
  home_reveal = true;
  switch (where) {
  case DIR_UP:
    row_focus = dec(row_focus);
    break;
  case DIR_DOWN:
    row_focus = min_size(row_focus + 1, ROW_COUNT - 1);
    break;
  case DIR_LEFT:
    col_focus[row_focus] = dec(col_focus[row_focus]);
    break;
  case DIR_RIGHT:
    col_focus[row_focus] =
        min_size(col_focus[row_focus] + 1, dec(rows[row_focus].count));
    break;
  }
    col_focus[row_focus] = min_size(col_focus[row_focus], dec(rows[row_focus].count));
    if (row_focus != old_row || col_focus[row_focus] != old_column)
      focus_cursor_move_requested = true;
}

static void move(direction where, bool repeat) {
  if (sidebar_open) {
    if (where == DIR_UP) {
      const size_t next = dec(sidebar_focus);
      if (next != sidebar_focus) {
        sidebar_focus = next;
        sidebar_reveal = true;
        focus_cursor_move_requested = true;
      }
    } else if (where == DIR_DOWN) {
      const size_t next = min_size(sidebar_focus + 1, categories_row.count);
      if (next != sidebar_focus) {
        sidebar_focus = next;
        sidebar_reveal = true;
        focus_cursor_move_requested = true;
      }
    } else if (where == DIR_RIGHT) {
      close_sidebar();
    }
    return;
  }
  if (where == DIR_LEFT) {
    bool at_left =
        screen == SCREEN_SERVER || (screen == SCREEN_AUTH && focus != 3) ||
        screen == SCREEN_QUICK || (screen == SCREEN_DETAILS && focus == 0) ||
        screen == SCREEN_SETTINGS ||
        (screen == SCREEN_HOME && col_focus[row_focus] == 0) ||
        (screen == SCREEN_GRID && grid_columns != 0 &&
         grid_selected % grid_columns == 0) ||
        screen == SCREEN_SEASON || (screen == SCREEN_PLAYBACK && focus == 0);
    if (at_left) {
      if (!repeat)
        open_sidebar();
      return;
    }
  }
  switch (screen) {
  case SCREEN_HOME:
    move_home(where);
    break;
  case SCREEN_AUTH:
    if (where == DIR_UP)
      focus = dec(focus);
    else if (where == DIR_DOWN)
      focus = min_size(focus + 1, focus_count() - 1);
    else if (where == DIR_LEFT && focus == 3)
      focus = 2;
    else if (where == DIR_RIGHT && focus == 2)
      focus = 3;
    break;
  case SCREEN_GRID:
    move_grid(where);
    break;
  case SCREEN_LICENSES:
    if (where == DIR_UP)
      focus = dec(focus);
    else if (where == DIR_DOWN)
      focus = min_size(focus + 1, focus_count() - 1);
    licenses_reveal = true;
    break;
  case SCREEN_LICENSE:
    if (where == DIR_UP)
      license_scroll = maxf(0, license_scroll - license_line_height);
    else if (where == DIR_DOWN)
      license_scroll += license_line_height;
    break;
  case SCREEN_SEASON:
    if (where == DIR_UP) {
      const size_t next = dec(episode_selected);
      focus_cursor_move_requested |= next != episode_selected;
      episode_selected = next;
    } else if (where == DIR_DOWN) {
      const size_t next =
          min_size(episode_selected + 1, dec(episodes_row.count));
      focus_cursor_move_requested |= next != episode_selected;
      episode_selected = next;
    }
    episode_reveal = true;
    break;
  /* The details screen's seasons and the playback transport read as a row;
   * every other screen is a single column of controls. */
  case SCREEN_DETAILS:
  case SCREEN_PLAYBACK:
    if (where == DIR_LEFT) {
      const size_t next = dec(focus);
      focus_cursor_move_requested |= screen == SCREEN_DETAILS && next != focus;
      focus = next;
    } else if (where == DIR_RIGHT) {
      const size_t next = min_size(focus + 1, focus_count() - 1);
      focus_cursor_move_requested |= screen == SCREEN_DETAILS && next != focus;
      focus = next;
    }
    break;
  default:
    if (where == DIR_UP)
      focus = dec(focus);
    else if (where == DIR_DOWN)
      focus = min_size(focus + 1, focus_count() - 1);
    break;
  }
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
    if (sidebar_open)
      activate_sidebar();
    else
      activate();
    break;
  default:
    break;
  }
}

/* ------------------------------------------------------------------- input */

static void on_key(uint32_t code, bool pressed, bool repeat) {
  if (!pressed)
    return;
  jf_window_frame_requested = true;
  if (jf_window_is_back_key(code)) {
    go_back();
    return;
  }
  if (screen == SCREEN_PLAYBACK) {
    if (code == 103) { /* Up dismisses player chrome immediately. */
      playback_controls_until = 0;
      return;
    }
    playback_controls_until = now_ns() + PLAYBACK_CONTROLS_NS;
  }
  if (code == 88) { /* F12 */
    capture_requested = true;
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

static void move_cursor(jf_fixed x, jf_fixed y)
{
    cursor_x = (float)jf_fixed_to_int(x);
    cursor_y = (float)jf_fixed_to_int(y);
    cursor_present = true;
}

static void on_event(const jf_event *event)
{
    /* Key releases do not change the UI, and neither does a pointer button going up. */
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
        cursor_present = false;
        cursor_x = -1;
        break;
    case JF_EVENT_POINTER_BUTTON:
        if (event->button.pressed && event->button.button == 0x110)
            pointer_press = true;
        break;
    case JF_EVENT_POINTER_AXIS: {
        if (event->axis.axis != 0)
            break;
        const float delta = (float)event->axis.value / 256.0f * 5.0f;
        if (sidebar_open) {
            sidebar_scroll += delta;
            break;
        }
        switch (screen) {
        case SCREEN_HOME:
            home_scroll += delta;
            home_reveal = false;
            break;
        case SCREEN_GRID:
            grid_scroll += delta;
            break;
        case SCREEN_SEASON:
            episode_scroll += delta;
            episode_reveal = false;
            break;
        case SCREEN_DETAILS:
            if (card_is(&detail, "Series") && delta != 0)
                focus = delta > 0 ? min_size(focus + 1, dec(seasons_row.count)) : dec(focus);
            break;
        case SCREEN_LICENSES:
          licenses_scroll += delta;
          licenses_reveal = false;
          break;
        case SCREEN_LICENSE:
          license_scroll = maxf(0, license_scroll + delta);
          break;
        default:
            break;
        }
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

static bool hovered(loom_rect rect)
{
    return cursor_present && loom_contains(rect, cursor_x, cursor_y);
}

/* ------------------------------------------------------------------- views */

static void draw_navigation_cursor(loom_context *ctx, loom_rect target,
                                   const loom_rect *clip, float radius,
                                   float scale, unsigned owner) {
  if (!focus_cursor.known || focus_cursor.owner != owner) {
    animated_float_snap(&focus_cursor.x, 0);
    animated_float_snap(&focus_cursor.y, 0);
    animated_float_snap(&focus_cursor.w, 0);
    animated_float_snap(&focus_cursor.h, 0);
    focus_cursor.rendered = target;
    focus_cursor.owner = owner;
    focus_cursor.known = true;
    focus_cursor_move_requested = false;
  } else if (focus_cursor_move_requested) {
    /* The list's target can move while this is settling. Animate a delta from
     * the last visible cursor to that live target so list scrolling carries the
     * cursor with it. */
    animated_float_snap(&focus_cursor.x, focus_cursor.rendered.x - target.x);
    animated_float_snap(&focus_cursor.y, focus_cursor.rendered.y - target.y);
    animated_float_snap(&focus_cursor.w, focus_cursor.rendered.w - target.w);
    animated_float_snap(&focus_cursor.h, focus_cursor.rendered.h - target.h);
    animated_float_set(&focus_cursor.x, 0, NAVIGATION_ANIMATION_NS);
    animated_float_set(&focus_cursor.y, 0, NAVIGATION_ANIMATION_NS);
    animated_float_set(&focus_cursor.w, 0, NAVIGATION_ANIMATION_NS);
    animated_float_set(&focus_cursor.h, 0, NAVIGATION_ANIMATION_NS);
    focus_cursor_move_requested = false;
  } else if (!animated_float_running(&focus_cursor.x) &&
             !animated_float_running(&focus_cursor.y)) {
    animated_float_snap(&focus_cursor.x, 0);
    animated_float_snap(&focus_cursor.y, 0);
    animated_float_snap(&focus_cursor.w, 0);
    animated_float_snap(&focus_cursor.h, 0);
  }
  const loom_rect cursor = {
      target.x + focus_cursor.x.value, target.y + focus_cursor.y.value,
      target.w + focus_cursor.w.value, target.h + focus_cursor.h.value};
  focus_cursor.rendered = cursor;
  loom_stroke(ctx, loom_inset(cursor, -4 * scale), clip, ACCENT, 4 * scale,
              radius);
}

static void draw_heading_and_status(loom_context *ctx, float width, float height, float scale)
{
    const float margin = 64 * scale;
    const char *heading = screen == SCREEN_QUICK ? "Quick Connect" : "";
    if (heading[0] != '\0')
        loom_label(ctx, (loom_rect){margin, 40 * scale, width - margin * 2, 50 * scale}, NULL,
                   heading, TEXT, 32 * scale);
    if (status_text[0] != '\0' && status_error)
        loom_label(ctx, (loom_rect){margin, height - 46 * scale, width - margin * 2, 32 * scale},
                   NULL, status_text, status_error ? RED : DIM, 19 * scale);
}

static void draw_button(loom_context *ctx, loom_rect rect, const char *label, size_t index,
                        float scale)
{
    const bool hot = hovered(rect);
    loom_fill(ctx, rect, NULL, hot ? HOT : CARD, 12 * scale);
    loom_stroke(ctx, rect, NULL, focus == index ? ACCENT : BORDER,
                focus == index ? 4 * scale : 2 * scale, 12 * scale);
    loom_label(ctx,
               (loom_rect){rect.x + 24 * scale, rect.y + (rect.h - 30 * scale) / 2,
                           rect.w - 48 * scale, 38 * scale},
               &rect, label, TEXT, 25 * scale);
    if (hot && pointer_press) {
        focus = index;
        activate();
    }
}

static void draw_action_button(loom_context *ctx, loom_rect rect, const char *label, size_t index,
                               float scale)
{
    const bool hot = hovered(rect);
    loom_fill(ctx, rect, NULL, hot ? SELECTED : ACCENT, 12 * scale);
    loom_stroke(ctx, rect, NULL, focus == index ? WHITE : ACCENT,
                focus == index ? 4 * scale : 2 * scale, 12 * scale);
    loom_label(ctx,
               (loom_rect){rect.x + 24 * scale, rect.y + (rect.h - 30 * scale) / 2,
                           rect.w - 48 * scale, 38 * scale},
               &rect, label, WHITE, 25 * scale);
    if (hot && pointer_press) {
        focus = index;
        activate();
    }
}

static void draw_field(loom_context *ctx, loom_rect rect, const char *label, edit_field which,
                       size_t index, float scale)
{
    const bool hot = hovered(rect);
    const bool editing = active_field == which;
    const bool lit = focus == index || editing;
    loom_fill(ctx, rect, NULL, editing ? SELECTED : hot ? HOT : CARD, 10 * scale);
    loom_stroke(ctx, rect, NULL, lit ? ACCENT : BORDER, lit ? 4 * scale : 2 * scale, 10 * scale);

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
               (loom_rect){rect.x + 22 * scale, rect.y + 17 * scale, rect.w - 44 * scale,
                           38 * scale},
               &rect, empty ? label : contents, empty ? DIM : TEXT, 24 * scale);
    if (hot && pointer_press) {
        focus = index;
        begin_edit(which, rect);
    }
}

/* Trim `text` to `width`, adding an ellipsis. Measured against the real glyph advances,
 * and backing up over whole UTF-8 sequences so a cut never lands inside one. */
static const char *ellipsize(const char *text, float width, float size)
{
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

static const poster_slot *card_poster(const card *source)
{
    /* Seasons may omit both SeriesId and the inherited image tag. The show details already
     * carry the fallback and remain loaded on the season page. */
    if (card_is(source, "Season") && source->thumbnail_tag[0] == '\0')
        return poster(detail.poster_id, detail.poster_tag);
    return poster(source->poster_id, source->poster_tag);
}

static void draw_watch_badge(loom_context *ctx, loom_rect art, loom_rect clip, const card *source,
                             float scale)
{
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
    const char *label = episode ? "\xe2\x9c\x93" : fmt("%u", count); /* U+2713 CHECK MARK */
    const float size = 22 * scale;
    const float measured = jf_renderer_measure(renderer, label, size);
    const float w = maxf(36 * scale, measured + 16 * scale);
    const loom_rect badge = {art.x + art.w - w - 8 * scale, art.y + 8 * scale, w, 36 * scale};
    loom_fill(ctx, badge, &clip, ACCENT, 6 * scale);
    loom_label(ctx,
               (loom_rect){badge.x + (badge.w - measured) / 2, badge.y + 3 * scale, badge.w,
                           badge.h},
               &clip, label, WHITE, size);
}

/* One poster card. Returns true when a pointer click landed on it, so the caller decides
 * what a click means: a card opens a grid on one screen and details on another. */
static bool draw_card(loom_context *ctx, loom_rect rect, loom_rect clip, const card *source,
                      bool focused, float scale)
{
    const loom_rect visible = loom_intersect(rect, clip);
    if (visible.w <= 0 || visible.h <= 0)
        return false;
    const bool hot = hovered(rect) && loom_contains(clip, cursor_x, cursor_y);
    const loom_rect art = {rect.x, rect.y, rect.w, rect.h - 78 * scale};

    const poster_slot *slot = card_poster(source);
    if (slot != NULL) {
        float uv[4];
        cover_uv(slot, art, uv);
        loom_textured(ctx, art, &clip, slot->texture, uv, WHITE, 10 * scale);
    } else {
        loom_fill(ctx, art, &clip, hot ? HOT : CARD, 10 * scale);
        if (artwork_loading(source->poster_id, source->poster_tag, JF_IMAGE_PRIMARY, POSTER_W,
                            POSTER_H))
            draw_spinner(ctx, art.x + art.w / 2, art.y + art.h / 2, 18 * scale, &clip, scale);
    }
    draw_watch_badge(ctx, art, clip, source, scale);

    if (source->progress > 1) {
        const loom_rect bar = {art.x + 5 * scale, art.y + art.h - 10 * scale,
                               art.w - 10 * scale, 6 * scale};
        loom_fill(ctx, bar, &clip, BORDER, 3 * scale);
        loom_fill(ctx,
                  (loom_rect){bar.x, bar.y, bar.w * minf(source->progress, 100) / 100, bar.h},
                  &clip, ACCENT, 3 * scale);
    }
    const loom_rect text_clip = loom_intersect(rect, clip);
    loom_label(ctx, (loom_rect){rect.x, art.y + art.h + 12 * scale, rect.w, 32 * scale},
               &text_clip, ellipsize(source->title, rect.w, 21 * scale), focused ? TEXT : DIM,
               21 * scale);
    loom_label(ctx, (loom_rect){rect.x, art.y + art.h + 44 * scale, rect.w, 28 * scale},
               &text_clip, ellipsize(source->subtitle, rect.w, 17 * scale), DIM, 17 * scale);
    return hot && pointer_press;
}

static void draw_metadata(loom_context *ctx, loom_rect rect, const loom_rect *clip,
                          const char *prefix, const char *rating, float size, const loom_color color)
{
    float x = rect.x;
    if (prefix != NULL && prefix[0] != '\0') {
        loom_label(ctx, rect, clip, prefix, color, size);
        x += jf_renderer_measure(renderer, prefix, size) + size;
    }
    if (rating != NULL && rating[0] != '\0') {
        static const char star[] = "\xe2\x98\x85"; /* U+2605 BLACK STAR */
        loom_label(ctx, (loom_rect){x, rect.y, size * 1.5f, rect.h}, clip, star, YELLOW, size);
        x += jf_renderer_measure(renderer, star, size) + size * 0.3f;
        loom_label(ctx, (loom_rect){x, rect.y, maxf(0, rect.x + rect.w - x), rect.h}, clip,
                   rating, TEXT, size);
    }
}

/* Greedy word wrap against the real glyph advances. The renderer clips a label to its rect
 * but does not break it, so an overview needs splitting before it becomes draw commands. */
static void draw_wrapped(loom_context *ctx, loom_rect rect, const char *text, float size)
{
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

static void draw_server(loom_context *ctx, float width, float scale)
{
    const loom_rect panel = {390 * scale, 80 * scale, width - 780 * scale, 920 * scale};
    loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
    loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
    const float x = panel.x + 54 * scale;
    const float w = panel.w - 108 * scale;
    url_rect = (loom_rect){x, panel.y + 48 * scale, w, 70 * scale};
    draw_field(ctx, url_rect, "Server URL", EDIT_URL, 0, scale);
    draw_action_button(ctx, (loom_rect){x, panel.y + 142 * scale, w, 70 * scale}, "Connect",
                       1, scale);

    float y = panel.y + 236 * scale;
    for (size_t index = 0; index < discovered_count; index++) {
        const loom_rect rect = {x, y, w, 70 * scale};
        const bool hot = hovered(rect);
        loom_fill(ctx, rect, &panel, hot ? HOT : CARD, 13 * scale);
        loom_stroke(ctx, rect, &panel, focus == index + 2 ? ACCENT : BORDER,
                    focus == index + 2 ? 4 * scale : 2 * scale, 13 * scale);
        loom_fill(ctx,
                  (loom_rect){rect.x + 18 * scale, rect.y + 14 * scale, 40 * scale, 40 * scale},
                  &rect, ACCENT, 22 * scale);
        loom_label(ctx,
                   (loom_rect){rect.x + 78 * scale, rect.y + 7 * scale, w - 96 * scale,
                               30 * scale},
                   &rect, discovered[index].name, TEXT, 26 * scale);
        loom_label(ctx,
                   (loom_rect){rect.x + 78 * scale, rect.y + 38 * scale, w - 96 * scale,
                               24 * scale},
                   &rect, discovered[index].address, DIM, 20 * scale);
        if (hot && pointer_press) {
            focus = index + 2;
            activate();
        }
        y += 80 * scale;
    }
    draw_button(ctx, (loom_rect){x, y + 8 * scale, w, 62 * scale}, "Search again",
                discovered_count + 2, scale);
    if (discovering || connecting)
        draw_spinner(ctx, panel.x + panel.w - 46 * scale, panel.y + panel.h - 46 * scale,
                     18 * scale, &panel, scale);
    if (connecting) {
        const loom_rect modal = {width / 2 - 190 * scale, 440 * scale, 380 * scale,
                                 150 * scale};
        loom_fill(ctx, modal, NULL, CARD, 16 * scale);
        loom_stroke(ctx, modal, NULL, ACCENT, 2 * scale, 16 * scale);
        draw_spinner(ctx, modal.x + 48 * scale, modal.y + modal.h / 2, 16 * scale, &modal,
                     scale);
        loom_label(ctx, (loom_rect){modal.x + 90 * scale, modal.y + 54 * scale,
                                    modal.w - 116 * scale, 42 * scale},
                   &modal, "Connecting...", TEXT, 26 * scale);
    }
}

static void draw_auth(loom_context *ctx, float width, float scale)
{
    const loom_rect panel = {430 * scale, 180 * scale, width - 860 * scale, 680 * scale};
    loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
    loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
    const float x = panel.x + 64 * scale;
    const float w = panel.w - 128 * scale;
    loom_label(ctx, (loom_rect){x, panel.y + 38 * scale, w, 50 * scale}, &panel, server_name,
               TEXT, 34 * scale);
    loom_label(ctx, (loom_rect){x, panel.y + 92 * scale, w, 32 * scale}, &panel, session.url,
               DIM, 20 * scale);
    username_rect = (loom_rect){x, panel.y + 180 * scale, w, 70 * scale};
    password_rect = (loom_rect){x, panel.y + 320 * scale, w, 70 * scale};
    draw_field(ctx, username_rect, "Username", EDIT_USERNAME, 0, scale);
    draw_field(ctx, password_rect, "Password", EDIT_PASSWORD, 1, scale);
    draw_action_button(ctx, (loom_rect){x, panel.y + 450 * scale, 280 * scale, 72 * scale},
                       "Sign in", 2, scale);
    draw_action_button(ctx,
                       (loom_rect){x + 310 * scale, panel.y + 450 * scale, 330 * scale,
                                   72 * scale},
                       "Quick Connect", 3, scale);
}

static void draw_quick(loom_context *ctx, float width, float scale)
{
    const loom_rect panel = {470 * scale, 200 * scale, width - 940 * scale, 640 * scale};
    loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
    loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
    loom_label(ctx,
               (loom_rect){panel.x + 70 * scale, panel.y + 50 * scale, panel.w - 140 * scale,
                           42 * scale},
               &panel, "Enter this code in a signed-in Jellyfin client", TEXT, 25 * scale);
    const loom_rect code = {panel.x + 150 * scale, panel.y + 140 * scale, panel.w - 300 * scale,
                            160 * scale};
    loom_fill(ctx, code, &panel, CARD, 15 * scale);
    loom_stroke(ctx, code, &panel, ACCENT, 3 * scale, 15 * scale);
    const char *text = quick_code[0] != '\0' ? quick_code : "------";
    /* Centred by measuring, because the code is six digits in a proportional font and
     * eyeballing an offset is wrong at every scale. */
    const float size = 64 * scale;
    const float half = jf_renderer_measure(renderer, text, size) / 2;
    loom_label(ctx, (loom_rect){code.x + code.w / 2 - half, code.y + 46 * scale, code.w,
                                80 * scale},
               &code, text, ACCENT, size);
    loom_label(ctx,
               (loom_rect){panel.x + 70 * scale, panel.y + 340 * scale, panel.w - 140 * scale,
                           34 * scale},
               &panel, "Approval is polled every two seconds.", DIM, 20 * scale);
    draw_button(ctx, (loom_rect){panel.x + 150 * scale, panel.y + 440 * scale, 260 * scale,
                                 72 * scale},
                "Cancel", 0, scale);
}

/* A horizontal row of poster cards. Only the focused row scrolls; a TV row holds few
 * enough items that the whole row is one strip with an offset. */
static void draw_row(loom_context *ctx, const item_row *row, size_t id, float top, float width,
                     loom_rect clip, float scale)
{
    const float margin = 64 * scale;
    const float card_w = (row->wide ? 356.0f : 200.0f) * scale;
    const float card_h = (row->wide ? 278.0f : 378.0f) * scale;
    const float gap = 26 * scale;
    loom_label(ctx, (loom_rect){margin, top, 600 * scale, 36 * scale}, &clip, row->title,
               row_focus == id ? TEXT : DIM, 25 * scale);
    const loom_rect strip = {margin, top + 46 * scale, width - margin * 2, card_h};

    if (row->count == 0) {
        if (row->loading)
            draw_spinner(ctx, strip.x + 30 * scale, strip.y + 30 * scale, 14 * scale, &clip,
                         scale);
        else
            loom_label(ctx, (loom_rect){margin, strip.y + 30 * scale, 700 * scale, 32 * scale},
                       &clip, "Nothing here", DIM, 20 * scale);
        return;
    }

    /* The cursor moves across the fully visible cards first. Only when it reaches the last
     * full slot does the strip translate: one predecessor then starts beyond the left edge
     * and exits through the viewport fade. */
    /* The final fully visible card has no following gap. Counting that otherwise treats
     * a fitting final slot as overflow and scrolls the carousel one item too early. */
    size_t visible = (size_t)((strip.w + gap) / (card_w + gap));
    if (visible < 1)
        visible = 1;
    const float step = card_w + gap;
    /* Keep the strip's position cumulative. Advancing the virtual first index
     * while resetting this to one step made every movement after the first one
     * jump. */
    const size_t scroll_steps =
        col_focus[id] >= visible ? col_focus[id] - visible + 1 : 0;
    /* The last card stops at the strip's right edge rather than a whole slot
     * in. */
    const float end_scroll = maxf(0, (float)row->count * step - gap - strip.w);
    static uint64_t drawn[ROW_COUNT];
    animate_list(&row_offset_motion[id], &drawn[id],
                 minf((float)scroll_steps * step, end_scroll));
    const float strip_scroll = row_offset_motion[id].value;
    const size_t first = dec((size_t)(strip_scroll / step));
    const loom_mask outer = ctx->mask;
    loom_fade(ctx, LOOM_HORIZONTAL, ctx->viewport.x,
              ctx->viewport.x + ctx->viewport.w, strip_scroll, end_scroll,
              86 * scale);
    loom_rect selected_art = {0};
    bool selected_drawn = false;
    for (size_t index = first; index < row->count; index++) {
      const float x = strip.x - strip_scroll + (float)index * step;
      if (x >= width)
        break;
      const loom_rect rect = {x, strip.y, card_w, card_h};
      const bool focused = row_focus == id && col_focus[id] == index;
      if (draw_card(ctx, rect, clip, &row->cards[index], focused, scale)) {
        row_focus = id;
        col_focus[id] = index;
        activate();
        ctx->mask = outer;
        return;
      }
        if (focused) {
          selected_art =
              (loom_rect){rect.x, rect.y, rect.w, rect.h - 78 * scale};
          selected_drawn = true;
        }
    }
    if (selected_drawn)
      draw_navigation_cursor(ctx, selected_art, &clip, 12 * scale, scale,
                             100 + (unsigned)id);
    ctx->mask = outer;
}

static void draw_home(loom_context *ctx, float width, float height, float scale)
{
    if (rows[ROW_LIBRARIES].loading) {
        draw_spinner(ctx, width / 2, height / 2, 26 * scale, NULL, scale);
        return;
    }
    home_row_height = 470 * scale;
    home_rect = (loom_rect){0, 0, width, height};
    loom_virtual_list target = loom_virtual_list_init(
        home_rect, ROW_COUNT, home_row_height, home_scroll);
    const float fade_height = 78 * scale;
    if (home_reveal) {
      home_scroll = loom_virtual_list_reveal(&target, row_focus, fade_height);
      home_reveal = false;
    }
    target = loom_virtual_list_init(home_rect, ROW_COUNT, home_row_height,
                                    home_scroll);
    home_scroll = target.scroll;
    static uint64_t drawn;
    animate_list(&home_scroll_motion, &drawn, home_scroll);
    const loom_virtual_list list = loom_virtual_list_init(
        home_rect, ROW_COUNT, home_row_height, home_scroll_motion.value);
    const loom_mask outer = ctx->mask;
    loom_fade(ctx, LOOM_VERTICAL, 0, height, list.scroll,
              loom_virtual_list_max_scroll(&list), fade_height);
    for (size_t id = 0; id < ROW_COUNT; id++)
        draw_row(ctx, &rows[id], id, loom_virtual_list_item(&list, id).y, width, ctx->viewport,
                 scale);
    ctx->mask = outer;
}

static void draw_grid(loom_context *ctx, float width, float height, float scale)
{
    const float margin = 64 * scale;
    const float card_w = 200 * scale;
    const float gap = 26 * scale;
    grid_rect = (loom_rect){margin, 120 * scale, width - margin * 2, height - 190 * scale};
    grid_columns = (size_t)((grid_rect.w + gap) / (card_w + gap));
    if (grid_columns < 1)
        grid_columns = 1;
    grid_row_height = 404 * scale;

    if (grid_total == 0) {
        loom_label(ctx, (loom_rect){margin, 40 * scale, width - margin * 2, 50 * scale}, NULL,
                   grid_title, TEXT, 32 * scale);
        if (grid_loading)
            draw_spinner(ctx, width / 2, height / 2, 24 * scale, NULL, scale);
        else
            loom_label(ctx, (loom_rect){margin, grid_rect.y + 40 * scale, 800 * scale, 36 * scale},
                       NULL, "Nothing here", DIM, 24 * scale);
        return;
    }

    const size_t row_count = grid_rows_count();
    const loom_virtual_list target = loom_virtual_list_init(
        grid_rect, row_count, grid_row_height, grid_scroll);
    grid_scroll = target.scroll;
    static uint64_t drawn;
    animate_list(&grid_scroll_motion, &drawn, grid_scroll);
    const loom_virtual_list list = loom_virtual_list_init(
        grid_rect, row_count, grid_row_height, grid_scroll_motion.value);
    const loom_mask outer = ctx->mask;
    loom_fade(ctx, LOOM_VERTICAL, 0, height, list.scroll,
              loom_virtual_list_max_scroll(&list), 72 * scale);
    loom_label(ctx, (loom_rect){margin, 40 * scale - grid_scroll, width - margin * 2, 50 * scale},
               NULL, grid_title, TEXT, 32 * scale);
    /* Grid padding reserves room for the first and last fully visible rows. It is not a
     * scissor: neighbouring rows extend into that margin and disappear at the viewport fade. */
    const loom_rect content = ctx->viewport;

    /* Selection padding is not a scissor: include the rows that extend into it. */
    loom_rect selected_art = {0};
    bool selected_drawn = false;
    const size_t first = dec(list.first);
    const size_t last = min_size(list.last + 1, row_count);
    for (size_t row_index = first; row_index < last; row_index++) {
        const loom_rect row_rect = loom_virtual_list_item(&list, row_index);
        for (size_t column = 0; column < grid_columns; column++) {
            const size_t index = row_index * grid_columns + column;
            if (index >= grid_total)
                break;
            const loom_rect rect = {row_rect.x + (float)column * (card_w + gap), row_rect.y,
                                    card_w, grid_row_height - 26 * scale};
            const card *source = grid_card(index);
            if (source == NULL)
                source = &blank_card;
            if (draw_card(ctx, rect, content, source, index == grid_selected, scale)) {
                grid_selected = index;
                activate();
                ctx->mask = outer;
                return;
            }
            if (index == grid_selected) {
              selected_art =
                  (loom_rect){rect.x, rect.y, rect.w, rect.h - 78 * scale};
              selected_drawn = true;
            }
        }
        /* One request per visible row is enough to walk the window forward while scrolling
         * with the pointer, which never calls move_grid. */
        request_page((uint32_t)min_size(row_index * grid_columns, dec(grid_total)));
    }
    if (selected_drawn)
      draw_navigation_cursor(ctx, selected_art, &content, 12 * scale, scale,
                             200);
    ctx->mask = outer;

    const loom_rect track = {grid_rect.x + grid_rect.w, grid_rect.y, 4 * scale, grid_rect.h};
    const float max_scroll = loom_virtual_list_max_scroll(&list);
    if (max_scroll > 0) {
        const float thumb_h =
            maxf(40 * scale, track.h * grid_rect.h / ((float)row_count * grid_row_height));
        loom_fill(ctx, track, NULL, BORDER, track.w / 2);
        loom_fill(ctx,
                  (loom_rect){track.x, track.y + (track.h - thumb_h) * (grid_scroll / max_scroll),
                              track.w, thumb_h},
                  NULL, ACCENT, track.w / 2);
    }
}

static void draw_details(loom_context *ctx, float width, float scale)
{
    const float margin = 64 * scale;
    const loom_rect art = {margin, margin, 320 * scale, 480 * scale};
    const poster_slot *slot = poster(detail.poster_id, detail.poster_tag);
    if (slot != NULL) {
        float uv[4];
        cover_uv(slot, art, uv);
        loom_textured(ctx, art, NULL, slot->texture, uv, WHITE, 16 * scale);
    } else {
        loom_fill(ctx, art, NULL, CARD, 16 * scale);
        if (artwork_loading(detail.poster_id, detail.poster_tag, JF_IMAGE_PRIMARY, POSTER_W,
                            POSTER_H))
            draw_spinner(ctx, art.x + art.w / 2, art.y + art.h / 2, 20 * scale, &art, scale);
    }
    draw_watch_badge(ctx, art, ctx->viewport, &detail, scale);

    const float x = art.x + art.w + 60 * scale;
    const float w = width - x - margin;
    loom_label(ctx, (loom_rect){x, margin, w, 72 * scale}, NULL,
               ellipsize(detail.title, w, 52 * scale), TEXT, 52 * scale);
    loom_label(ctx, (loom_rect){x, 150 * scale, w, 34 * scale}, NULL, detail.subtitle, TEXT,
               24 * scale);
    draw_metadata(ctx, (loom_rect){x, 195 * scale, w, 34 * scale}, NULL, detail_extra,
                  detail.rating, 20 * scale, DIM);
    draw_wrapped(ctx, (loom_rect){x, 255 * scale, w, 180 * scale}, detail.overview, 22 * scale);

    if (card_is(&detail, "Series")) {
        if (seasons_row.count == 0) {
            loom_label(ctx, (loom_rect){x, 480 * scale, w, 34 * scale}, NULL,
                       seasons_row.loading ? "Loading seasons..." : "No seasons", DIM,
                       22 * scale);
            return;
        }
        loom_label(ctx, (loom_rect){x, 470 * scale, w, 34 * scale}, NULL, "Seasons", TEXT,
                   26 * scale);
        const loom_rect clip = ctx->viewport;
        size_t visible = (size_t)((w + 26 * scale) / (226 * scale));
        if (visible < 1)
            visible = 1;
        const size_t first = (focus + 1) > visible ? focus + 1 - visible : 0;
        loom_rect selected_art = {0};
        bool selected_drawn = false;
        for (size_t index = first; index < seasons_row.count; index++) {
            const loom_rect rect = {x + (float)(index - first) * 226 * scale, 524 * scale,
                                    200 * scale, 378 * scale};
            if (rect.x >= width)
                break;
            if (draw_card(ctx, rect, clip, &seasons_row.cards[index], focus == index, scale)) {
                focus = index;
                activate();
                break;
            }
            if (focus == index) {
              selected_art =
                  (loom_rect){rect.x, rect.y, rect.w, rect.h - 78 * scale};
              selected_drawn = true;
            }
        }
        if (selected_drawn)
          draw_navigation_cursor(ctx, selected_art, &clip, 12 * scale, scale,
                                 300);
        return;
    }

    draw_button(ctx, (loom_rect){x, 570 * scale, 250 * scale, 74 * scale}, "Play", 0, scale);
}

static void draw_season(loom_context *ctx, float width, float height, float scale)
{
    const float margin = 64 * scale;
    const loom_rect art = {margin, margin, 320 * scale, 480 * scale};
    const poster_slot *slot = card_poster(&season_detail);
    if (slot != NULL) {
        float uv[4];
        cover_uv(slot, art, uv);
        loom_textured(ctx, art, NULL, slot->texture, uv, WHITE, 12 * scale);
    } else {
        loom_fill(ctx, art, NULL, CARD, 12 * scale);
        if (artwork_loading(season_detail.poster_id, season_detail.poster_tag, JF_IMAGE_PRIMARY,
                            POSTER_W, POSTER_H))
            draw_spinner(ctx, art.x + art.w / 2, art.y + art.h / 2, 20 * scale, &art, scale);
    }
    draw_watch_badge(ctx, art, ctx->viewport, &season_detail, scale);

    const float x = art.x + art.w + 60 * scale;
    const float w = width - x - margin;
    loom_label(ctx, (loom_rect){x, margin, w, 72 * scale}, NULL,
               ellipsize(detail.title, w, 52 * scale), TEXT, 52 * scale);
    loom_label(ctx, (loom_rect){x, 152 * scale, w, 48 * scale}, NULL, season_detail.title, TEXT,
               32 * scale);
    draw_wrapped(ctx, (loom_rect){x, 220 * scale, w, 145 * scale},
                 season_detail.overview[0] != '\0' ? season_detail.overview : detail.overview,
                 22 * scale);

    episode_rect = (loom_rect){x, 400 * scale, w, height - 464 * scale};
    episode_row_height = 170 * scale;
    if (episodes_row.count == 0) {
        loom_label(ctx, (loom_rect){x, episode_rect.y + 20 * scale, w, 34 * scale},
                   &episode_rect, episodes_row.loading ? "Loading episodes..." : "No episodes",
                   DIM, 23 * scale);
        return;
    }

    /* Like the home carousels: the selection walks down the fully visible
     * slots, then stays in the last one while the list moves a whole episode at
     * a time. The slot above it is then the one leaving through the fade. */
    if (episode_reveal) {
      size_t visible = (size_t)(episode_rect.h / episode_row_height);
      if (visible < 1)
        visible = 1;
      episode_scroll =
          episode_selected >= visible
              ? (float)(episode_selected - visible + 1) * episode_row_height
              : 0;
      episode_reveal = false;
    }
    const float fade_height = 64 * scale;
    const loom_virtual_list target = loom_virtual_list_init(
        episode_rect, episodes_row.count, episode_row_height, episode_scroll);
    episode_scroll = target.scroll;
    static uint64_t drawn;
    animate_list(&episode_scroll_motion, &drawn, episode_scroll);
    const loom_virtual_list list =
        loom_virtual_list_init(episode_rect, episodes_row.count,
                               episode_row_height, episode_scroll_motion.value);
    /* Keep episodes below their heading, but let them reach the screen bottom. */
    const loom_rect content = {x, episode_rect.y, width - x, height - episode_rect.y};
    const loom_mask outer = ctx->mask;
    loom_fade(ctx, LOOM_VERTICAL, content.y, content.y + content.h, list.scroll,
              loom_virtual_list_max_scroll(&list), fade_height);
    loom_rect selected_row = {0};
    bool selected_drawn = false;
    for (size_t index = list.first; index < list.last; index++) {
        const loom_rect raw = loom_virtual_list_item(&list, index);
        const loom_rect row = {raw.x + 10 * scale, raw.y + 6 * scale, raw.w - 20 * scale,
                               raw.h - 12 * scale};
        const bool hot = hovered(row) && loom_contains(content, cursor_x, cursor_y);
        const bool focused = index == episode_selected;
        const card *source = &episodes_row.cards[index];
        if (focused || hot) {
            static const loom_color focused_bg = {20, 42, 60, 210};
            static const loom_color hot_bg = {30, 40, 55, 180};
            loom_fill(ctx, row, &content, focused ? focused_bg : hot_bg, 11 * scale);
        }
        if (focused) {
          selected_row = row;
          selected_drawn = true;
        }
        const loom_rect clip = loom_intersect(row, content);
        const loom_rect thumb = {row.x + 12 * scale, row.y + 12 * scale, 232 * scale,
                                 130.5f * scale};
        const poster_slot *still =
            artwork(source->id, source->thumbnail_tag, JF_IMAGE_PRIMARY, 384, 216);
        if (still != NULL) {
            float uv[4];
            cover_uv(still, thumb, uv);
            loom_textured(ctx, thumb, &clip, still->texture, uv, WHITE, 7 * scale);
        } else {
            loom_fill(ctx, thumb, &clip, CARD, 7 * scale);
            if (artwork_loading(source->id, source->thumbnail_tag, JF_IMAGE_PRIMARY, 384,
                                216))
                draw_spinner(ctx, thumb.x + thumb.w / 2, thumb.y + thumb.h / 2, 13 * scale,
                             &clip, scale);
        }
        draw_watch_badge(ctx, thumb, clip, source, scale);

        const float text_x = thumb.x + thumb.w + 28 * scale;
        const float text_w = row.x + row.w - text_x - 24 * scale;
        loom_label(ctx, (loom_rect){text_x, row.y + 34 * scale, text_w, 44 * scale}, &clip,
                   ellipsize(source->episode_title, text_w, 28 * scale), TEXT, 28 * scale);
        draw_metadata(ctx, (loom_rect){text_x, row.y + 90 * scale, text_w, 32 * scale}, &clip,
                      source->runtime, source->rating, 21 * scale, TEXT);
        if (source->progress > 1) {
            const loom_rect bar = {thumb.x + 4 * scale, thumb.y + thumb.h - 7 * scale,
                                   thumb.w - 8 * scale, 5 * scale};
            loom_fill(ctx,
                      (loom_rect){bar.x, bar.y, bar.w * minf(source->progress, 100) / 100, bar.h},
                      &clip, ACCENT, 2 * scale);
        }
        if (hot && pointer_press) {
            episode_selected = index;
            activate();
            ctx->mask = outer;
            return;
        }
    }
    if (selected_drawn)
      draw_navigation_cursor(ctx, selected_row, &content, 11 * scale, scale,
                             400);
    ctx->mask = outer;
}

static void draw_playback(loom_context *ctx, float width, float height, float scale)
{
    if (!playback_paused && now_ns() >= playback_controls_until)
        return;
    /* The video itself is on the TV's own plane. This graphics-plane strip is the only
     * part of playback this process draws. */
    static const loom_color chrome = {8, 12, 20, 205};
    const loom_rect panel = {0, height - 166 * scale, width, 166 * scale};
    loom_fill(ctx, panel, NULL, chrome, 0);
    loom_label(ctx, (loom_rect){64 * scale, panel.y + 20 * scale, width - 128 * scale,
                                34 * scale},
               &panel, playback_title, TEXT, 27 * scale);

    const jf_player_state state = jf_player_state_get();
    const char *message = state == JF_LOADING ? "Loading stream..."
                          : state == JF_PLAYING
                              ? (playback_paused ? "Paused - OK resumes, Back stops"
                                                 : "OK pauses, Back stops")
                          : state == JF_FAILED ? jf_player_error()
                                               : "Stopped";
    loom_label(ctx, (loom_rect){64 * scale, panel.y + 58 * scale, width - 128 * scale,
                                26 * scale},
               &panel, message, state == JF_FAILED ? RED : DIM, 19 * scale);

    /* Centre the transport, with the track controls held at the right edge. */
    float x = (width - 830 * scale) / 2;
    for (size_t index = 0; index < 7; index++) {
        const float button_width = playback_buttons[index].action == ACTION_PAUSE ? 130.0f : 88.0f;
        const loom_rect rect = {x, panel.y + 96 * scale, button_width * scale, 48 * scale};
        draw_button(ctx, rect, playback_buttons[index].label, index, scale);
        x += rect.w + 10 * scale;
    }
    x = width - 64 * scale - 210 * scale;
    for (size_t index = 7; index < PLAYBACK_BUTTON_COUNT; index++) {
        const loom_rect rect = {x, panel.y + 96 * scale, 100 * scale, 48 * scale};
        draw_button(ctx, rect, playback_buttons[index].label, index, scale);
        x += 110 * scale;
    }
}

/* The subtitle overlay sits under the transport chrome and over the video hole.
 * libass composes it; this only notices when it changed and re-uploads. */
static void draw_subtitles(loom_context *ctx) {
  if (!jf_subs_ready()) {
    if (subtitle_texture != 0) {
      jf_renderer_destroy_texture(renderer, subtitle_texture);
      subtitle_texture = 0;
    }
    return;
  }
  const int media_ms = jf_player_media_ms();
  if (media_ms < 0)
    return;
  jf_subs_image image;
  if (jf_subs_frame(media_ms, &image)) {
    if (subtitle_texture != 0)
      jf_renderer_destroy_texture(renderer, subtitle_texture);
    subtitle_texture =
        image.w > 0
            ? jf_renderer_create_rgba_texture(renderer, (uint32_t)image.w,
                                              (uint32_t)image.h, image.rgba)
            : 0;
    subtitle_rect = (loom_rect){(float)image.x, (float)image.y, (float)image.w,
                                (float)image.h};
  }
  if (subtitle_texture == 0)
    return;
  static const float whole[4] = {0, 0, 1, 1};
  loom_textured(ctx, subtitle_rect, NULL, subtitle_texture, whole, WHITE, 0);
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
  loom_virtual_list target = loom_virtual_list_init(
      list_rect, JF_LICENSE_COUNT, row_height, licenses_scroll);
  if (licenses_reveal) {
    licenses_scroll = loom_virtual_list_reveal(&target, focus, 0);
    licenses_reveal = false;
    target = loom_virtual_list_init(list_rect, JF_LICENSE_COUNT, row_height,
                                    licenses_scroll);
  }
  const loom_virtual_list list = target;
  /* Rows of its own rather than draw_button, for the same reason the sidebar
   * has them: a scrolled list needs every part of a row clipped to the view,
   * not just its text. */
  const loom_rect clip = {list_rect.x - 4 * scale, list_rect.y,
                          list_rect.w + 8 * scale, list_rect.h};
  for (size_t index = list.first; index < list.last; index++) {
    const loom_rect raw = loom_virtual_list_item(&list, index);
    const loom_rect row = {raw.x, raw.y + 6 * scale, raw.w, raw.h - 12 * scale};
    const bool hot = hovered(row);
    loom_fill(ctx, row, &clip, hot ? HOT : CARD, 12 * scale);
    loom_stroke(ctx, row, &clip, focus == index ? ACCENT : BORDER,
                focus == index ? 4 * scale : 2 * scale, 12 * scale);
    loom_label(
        ctx,
        (loom_rect){row.x + 24 * scale, row.y + (row.h - 30 * scale) / 2,
                    row.w - 48 * scale, 38 * scale},
        &clip,
        fmt("%s - %s", jf_licenses[index].library, jf_licenses[index].license),
        TEXT, 25 * scale);
    if (hot && pointer_press) {
      focus = index;
      activate();
    }
  }
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
  license_line_height = 24 * scale;
  const float padding = 40 * scale;
  const float heading = 86 * scale;
  const float panel_width =
      minf(width - 240 * scale,
           maxf(620 * scale, license_text_width * size + 2 * padding));
  const float panel_height =
      minf(height - 180 * scale,
           heading + license_line_count * license_line_height + padding);
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
  loom_virtual_list list = loom_virtual_list_init(
      view, license_line_count, license_line_height, license_scroll);
  const float end = loom_virtual_list_max_scroll(&list);
  license_scroll = minf(license_scroll, end);
  list = loom_virtual_list_init(view, license_line_count, license_line_height,
                                license_scroll);
  /* Deep enough to cover the line being cut in half. It grows in with the
   * scroll, so a licence at its top keeps its title readable. */
  const loom_mask outer = ctx->mask;
  loom_fade(ctx, LOOM_VERTICAL, view.y, view.y + view.h, license_scroll, end,
            license_line_height * 2);
  for (size_t index = list.first; index < list.last; index++) {
    const loom_rect row = loom_virtual_list_item(&list, index);
    loom_label(ctx, (loom_rect){row.x, row.y, row.w, row.h}, &view,
               license_lines[index], DIM, size);
  }
  ctx->mask = outer;
}

static void draw_settings(loom_context *ctx, float width, float scale)
{
  const loom_rect panel = {560 * scale, 230 * scale, width - 1120 * scale,
                           484 * scale};
  loom_fill(ctx, panel, NULL, PANEL, 18 * scale);
  loom_stroke(ctx, panel, NULL, BORDER, 2 * scale, 18 * scale);
  loom_label(ctx,
             (loom_rect){panel.x + 48 * scale, panel.y + 42 * scale,
                         panel.w - 96 * scale, 44 * scale},
             &panel, "Settings", TEXT, 32 * scale);
  draw_button(ctx,
              (loom_rect){panel.x + 48 * scale, panel.y + 116 * scale,
                          panel.w - 96 * scale, 72 * scale},
              animations_enabled ? "Animations: On" : "Animations: Off", 0,
              scale);
  draw_button(ctx,
              (loom_rect){panel.x + 48 * scale, panel.y + 210 * scale,
                          panel.w - 96 * scale, 72 * scale},
              "Sign out", 1, scale);
  draw_button(ctx,
              (loom_rect){panel.x + 48 * scale, panel.y + 304 * scale,
                          panel.w - 96 * scale, 72 * scale},
              "Open source licenses", 2, scale);
}

static void draw_sidebar(loom_context *ctx, float height, float scale)
{
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
  loom_virtual_list target =
      loom_virtual_list_init(list_rect, count, row_height, sidebar_scroll);
  if (sidebar_reveal) {
    sidebar_scroll = loom_virtual_list_reveal(&target, sidebar_focus, 0);
    sidebar_reveal = false;
  }
  target = loom_virtual_list_init(list_rect, count, row_height, sidebar_scroll);
  static uint64_t drawn;
  animate_list(&sidebar_scroll_motion, &drawn, target.scroll);
  const loom_virtual_list list = loom_virtual_list_init(
      list_rect, count, row_height, sidebar_scroll_motion.value);
  const loom_rect selected_raw = loom_virtual_list_item(&list, sidebar_focus);
  const loom_rect selected_row = {selected_raw.x, selected_raw.y + 5 * scale,
                                  selected_raw.w, selected_raw.h - 10 * scale};
  const loom_rect button_clip = {
      list_rect.x - 4 * scale, list_rect.y - 4 * scale, list_rect.w + 8 * scale,
      list_rect.h + 8 * scale};
  for (size_t index = list.first; index < list.last; index++) {
    const loom_rect raw = loom_virtual_list_item(&list, index);
    const loom_rect row = {raw.x, raw.y + 5 * scale, raw.w, raw.h - 10 * scale};
    const bool hot = hovered(row);
    loom_fill(ctx, row, &button_clip, hot ? HOT : CARD, 10 * scale);
    loom_stroke(ctx, row, &button_clip, BORDER, 1 * scale, 10 * scale);
    loom_label(ctx,
               (loom_rect){row.x + 22 * scale, row.y + 16 * scale,
                           row.w - 44 * scale, 32 * scale},
               &button_clip,
               index < categories_row.count ? categories_row.cards[index].title
                                            : "Settings",
               TEXT, 22 * scale);
    if (hot && pointer_press) {
      sidebar_focus = index;
      activate_sidebar();
    }
  }
  draw_navigation_cursor(ctx, selected_row, &button_clip, 10 * scale, scale,
                         500);
}

static bool advance_animations(uint64_t now) {
  animated_float_update(&home_scroll_motion, now);
  animated_float_update(&grid_scroll_motion, now);
  animated_float_update(&episode_scroll_motion, now);
  animated_float_update(&sidebar_slide, now);
  animated_float_update(&sidebar_scroll_motion, now);
  animated_float_update(&focus_cursor.x, now);
  animated_float_update(&focus_cursor.y, now);
  animated_float_update(&focus_cursor.w, now);
  animated_float_update(&focus_cursor.h, now);
  for (size_t index = 0; index < ROW_COUNT; index++)
    animated_float_update(&row_offset_motion[index], now);

  if (sidebar_closing && !animated_float_running(&sidebar_slide) &&
      sidebar_slide.value == 0) {
    sidebar_open = false;
    sidebar_closing = false;
    focus_cursor.known = false;
  }
  if (animated_float_running(&home_scroll_motion) ||
      animated_float_running(&grid_scroll_motion) ||
      animated_float_running(&episode_scroll_motion) ||
      animated_float_running(&sidebar_slide) ||
      animated_float_running(&sidebar_scroll_motion) ||
      animated_float_running(&focus_cursor.x) ||
      animated_float_running(&focus_cursor.y) ||
      animated_float_running(&focus_cursor.w) ||
      animated_float_running(&focus_cursor.h))
    return true;
  for (size_t index = 0; index < ROW_COUNT; index++)
    if (animated_float_running(&row_offset_motion[index]))
      return true;
  return false;
}

static void build_ui(loom_context *ctx)
{
    const float width = (float)gl_width;
    const float height = (float)gl_height;
    const float scale = minf(width / 1920.0f, height / 1080.0f);
    scratch_used = 0;
    poster_requests = 0;
    frame_index++;
    advance_animations(now_ns());

    loom_begin(ctx, width, height);
    /* The video plane sits behind this EGL surface. During playback every untouched pixel
     * must remain transparent, otherwise the UI obscures it. */
    if (screen != SCREEN_PLAYBACK)
        loom_fill(ctx, (loom_rect){0, 0, width, height}, NULL, BG, 0);
    if (screen == SCREEN_DETAILS || screen == SCREEN_SEASON) {
        const loom_rect background = {0, 0, width, height};
        const poster_slot *slot =
            artwork(detail.backdrop_id, detail.backdrop_tag, JF_IMAGE_BACKDROP, 1920, 1080);
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
    case SCREEN_SERVER: draw_server(ctx, width, scale); break;
    case SCREEN_AUTH: draw_auth(ctx, width, scale); break;
    case SCREEN_QUICK: draw_quick(ctx, width, scale); break;
    case SCREEN_HOME: draw_home(ctx, width, height, scale); break;
    case SCREEN_GRID: draw_grid(ctx, width, height, scale); break;
    case SCREEN_DETAILS: draw_details(ctx, width, scale); break;
    case SCREEN_SEASON: draw_season(ctx, width, height, scale); break;
    case SCREEN_PLAYBACK:
      draw_subtitles(ctx);
      draw_playback(ctx, width, height, scale);
      break;
    case SCREEN_SETTINGS: draw_settings(ctx, width, scale); break;
    case SCREEN_LICENSES:
      draw_licenses(ctx, width, height, scale);
      break;
    case SCREEN_LICENSE:
      draw_license(ctx, width, height, scale);
      break;
    }
    if (sidebar_open)
        draw_sidebar(ctx, height, scale);
    /* Pointer activation happens during layout; rebuild its resulting screen. */
    if (pointer_press)
        jf_window_frame_requested = true;
    if (advance_animations(now_ns()))
      jf_window_frame_requested = true;
    pointer_press = false;
}

/* -------------------------------------------------------------------- main */

/* How often the subtitle overlay is asked whether it has changed.
 *
 * ponytail: a poll, not a schedule. libass knows when the next event starts but
 * not when the current one ends, so there is no single wake-up to ask it for;
 * at this period a line appears within one frame of its time at 30 Hz and the
 * check costs one ass_render_frame against an unchanged time. A karaoke or
 * sign-heavy script animates at this rate rather than the display's - drive it
 * from jf_player_needs_frame if that ever matters. */
#define SUBTITLE_TICK_NS (33 * 1000000ull)

/* Only these UI features have time-dependent work in normal operation. */
static bool next_deadline(uint64_t now, bool controls_visible, uint64_t *out)
{
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
    if (screen == SCREEN_PLAYBACK && jf_subs_ready()) {
      if (!have || subtitle_tick < deadline)
        deadline = subtitle_tick;
      have = true;
    }
    *out = deadline;
    return have;
}

static int wait_milliseconds(uint64_t now, bool have_deadline, uint64_t deadline)
{
    if (!have_deadline)
        return -1;
    const uint64_t remaining = deadline > now ? deadline - now : 0;
    const uint64_t ms = remaining / 1000000ull + (remaining % 1000000ull != 0 ? 1 : 0);
    return (int)min_size((size_t)ms, INT32_MAX);
}

/* This application's own backbuffer as a PPM: the only way to check on-device rendering
 * without the VNC grab. */
static void capture_frame(const char *path)
{
    const size_t width = gl_width;
    const size_t height = gl_height;
    uint8_t *rgba = malloc(width * height * 4);
    uint8_t *row = malloc(width * 3);
    FILE *file = NULL;
    if (rgba == NULL || row == NULL)
        goto done;
    glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
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
    fprintf(stderr, "captured OpenGL framebuffer to %s (%zux%zu)\n", path, width, height);
done:
    if (file != NULL)
        fclose(file);
    free(rgba);
    free(row);
}

/* Launched from the TV's app list there is no terminal, so an installed app's output goes
 * nowhere and a failure is invisible. The fallback is to send stdout and stderr to a file
 * next to everything else this app writes - SDL and the Luna bridge report on stdout, and
 * those lines are the only diagnosis when a backend refuses. */
static void log_to_file(void)
{
    if (isatty(2))
        return;
    char path[576];
    /* conf/ rather than the app directory itself: the package can only make the
     * subdirectories world-writable, and the app runs as a jail uid that owns none of
     * them. /tmp is the fallback when even that fails. */
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

/* A remote, replayed. There is no way to click through this app without a TV or a person,
 * so UI_SCRIPT=drrob presses those buttons a few frames apart and UI_CAPTURE saves the
 * screen it ends on. Letters are the four arrows, `o` for OK and `b` for Back; `.` waits
 * one more beat, which is what a screen that is still fetching needs. */
static const char *script = "";
static size_t script_at;
static unsigned script_wait;

/* Frames between scripted presses. Long enough that a request started by one press has
 * landed before the next, on a LAN. */
#define SCRIPT_BEAT 45

static bool step_script(void)
{
    if (script[script_at] == '\0')
        return jf_fetcher_pending(&fetcher) == 0;
    /* Never press a button while a request is outstanding: whether discovery has answered
     * decides what the focused control even is. */
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
    case 'u': key = 103; break;
    case 'd': key = 108; break;
    case 'l': key = 105; break;
    case 'r': key = 106; break;
    case 'o': key = 28; break;
    case 'b': key = jf_window_on_webos ? 412 : 158; break;
    default: break;
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
    fprintf(stderr, "script: '%c' -> screen=%d focus=%zu row=%zu servers=%zu depth=%zu\n",
            command, (int)screen, focus, row_focus, discovered_count, depth);
    return false;
}

int main(void)
{
    for (size_t i = 0; i < ROW_COUNT; i++)
        rows[i].cards = home_cards[i];
    categories_row.cards = category_cards;
    seasons_row.cards = seasons_cards;
    episodes_row.cards = episodes_cards;

    jf_session_device_id(&session);
    jf_api_init();
    snprintf(preferences_path, sizeof(preferences_path),
             "%s/conf/preferences.ini", jf_store_root());
    load_preferences();
    log_to_file();
    const bool restored = jf_session_load(&session);
    if (session.url[0] != '\0') {
        set_text(server_url, sizeof(server_url), session.url);
        set_text(server_name, sizeof(server_name), session.url);
    }

    jf_window_set_handler(on_event);
    const char *appid = getenv("APPID");
    if (!jf_window_init(appid != NULL ? appid : APP_ID, "Jellyfin", 0, 0))
        return 1;
    /* How the TV asks the app to close. Absent off-device, where nothing asks. */
    if (!jf_luna_register_lifecycle(jf_window_post_quit, jf_window_post_raise))
        fprintf(stderr, "no webOS lifecycle\n");

    glViewport(0, 0, (GLsizei)gl_width, (GLsizei)gl_height);
    glClearColor(9.0f / 255.0f, 13.0f / 255.0f, 22.0f / 255.0f, 1);

    renderer = jf_renderer_create(NULL, 0, 0);
    if (renderer == NULL) {
        fprintf(stderr, "the UI renderer could not start\n");
        return 1;
    }
    loom_context ctx;
    loom_init(&ctx);
    probe_overlay overlay;
    probe_timer timer;
    if (stats_overlay) {
      probe_overlay_init(&overlay, text_vs, text_fs, (int)gl_width,
                         (int)gl_height);
      probe_timer_init(&timer);
    }

    if (!jf_fetcher_init(&fetcher, jf_window_wake)) {
        fprintf(stderr, "no fetcher worker threads\n");
        return 1;
    }
    jf_fetcher_set_session(&fetcher, &session);

    /* A stored token skips straight to the home rows; JELLYFIN_ADDRESS makes a fresh
     * install land on the sign-in screen without typing a URL on a TV. */
    if (restored) {
        depth = 0;
        screen = SCREEN_HOME;
        set_status("Signed in as %s", session.user_name);
        load_home();
    } else {
        /* Development convenience, and the only way a script can sign in: the fields start
         * filled from the environment. Nothing is read from there once a token is
         * stored. */
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

    const char *capture_path = getenv("UI_CAPTURE");
    const char *requested_script = getenv("UI_SCRIPT");
    script = requested_script != NULL ? requested_script : "";
    /* Without a script, hold long enough for discovery's three timeouts. */
    unsigned capture_after = (capture_path != NULL && script[0] == '\0') ? 240 : 0;
    bool controls_visible = false;
    jf_player_state last_state = jf_player_state_get();

    while (jf_window_poll()) {
        pump();
        const jf_player_state state = jf_player_state_get();
        if (state != last_state) {
            last_state = state;
            if (screen == SCREEN_PLAYBACK)
                jf_window_frame_requested = true;
        }
        const uint64_t now = now_ns();
        const bool visible =
            screen == SCREEN_PLAYBACK && (playback_paused || now < playback_controls_until);
        if (visible != controls_visible) {
            controls_visible = visible;
            jf_window_frame_requested = true;
        }
        if (screen == SCREEN_PLAYBACK && jf_subs_ready() &&
            now >= subtitle_tick) {
          subtitle_tick = now + SUBTITLE_TICK_NS;
          jf_window_frame_requested = true;
        }
        /* A finished script still has to reach its capture. */
        const bool scripted_frame = script[script_at] != '\0' ||
                                    capture_after > 0 ||
                                    (script[0] != '\0' && capture_path != NULL);
        if (!jf_window_drawable ||
            (!jf_window_frame_requested && !jf_player_needs_frame() && !scripted_frame &&
             !capture_requested)) {
            uint64_t deadline = 0;
            const bool have = next_deadline(now, controls_visible, &deadline);
            jf_window_wait_timeout(wait_milliseconds(now_ns(), have, deadline));
            continue;
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
            build_ui(&ctx);
        }
        jf_renderer_draw(renderer, ctx.commands, ctx.count, (float)gl_width, (float)gl_height);
        if (stats_overlay) {
          char line[PROBE_OVERLAY_COLS + 1];
          probe_overlay_clear(&overlay);
          snprintf(line, sizeof(line), "cpu  %6.2f ms", timer.cpu_ms);
          probe_overlay_line(&overlay, 0, line);
          snprintf(line, sizeof(line), "gpu %s%6.2f ms",
                   timer.mode == PROBE_GPU_FINISH ? "*" : " ", timer.gpu_ms);
          probe_overlay_line(&overlay, 1, line);
          snprintf(line, sizeof(line), "%5.1f fps",
                   timer.frame_ms > 0 ? 1000.0 / timer.frame_ms : 0.0);
          probe_overlay_line(&overlay, 2, line);
          snprintf(line, sizeof(line), "%zu instances",
                   jf_renderer_instances(renderer));
          probe_overlay_line(&overlay, 3, line);
          snprintf(line, sizeof(line), "%u draws %.1fx",
                   jf_renderer_batches(renderer),
                   jf_renderer_covered(renderer) /
                       ((double)gl_width * gl_height));
          probe_overlay_line(&overlay, 4, line);
          glEnable(GL_BLEND);
          probe_overlay_draw(&overlay);
          probe_timer_end(&timer);
        }

        if (script[0] != '\0' && step_script() && capture_path != NULL && capture_after == 0)
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
    }

    jf_player_deinit();
    jf_fetcher_deinit(&fetcher);
    loom_destroy(&ctx);
    jf_renderer_destroy(renderer);
    jf_luna_deinit();
    jf_window_deinit();
    return 0;
}
