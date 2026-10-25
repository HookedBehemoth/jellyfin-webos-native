/* The transport api.h asks for, answering from a made-up library. See
 * api_mock.h. */
#include "api_mock.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOBS (JF_JOB_LATEST + 1)

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
/* What the server keeps: the delays last saved, and whether a subtitle was
 * downloaded, which lists one more file beside the video. */
static char saved_delays[1024];
static uint64_t progress_ticks;
static bool downloaded;
static unsigned listed_after;
/* The subtitle sources, as the official repository has them: OpenSubtitles,
 * not installed until asked to be; and subbuzz once its repository is added.
 * NULL is not installed. */
static const char *opensubtitles_status;
static const char *subbuzz_status;
static bool subbuzz_listed;
static pthread_cond_t released = PTHREAD_COND_INITIALIZER,
                      left = PTHREAD_COND_INITIALIZER;
static struct {
  int status;
  unsigned remaining; /* ~0u: until reset */
  bool failing, held;
  unsigned calls, waiting;
} rules[JOBS];

/* Returns with none of `job`'s requests still waiting, so a test that releases
 * them and then settles sees them in flight rather than a moment too early. */
static void drain(jf_job job) {
  pthread_cond_broadcast(&released);
  while (rules[job].waiting != 0)
    pthread_cond_wait(&left, &mutex);
}

void mock_reset(void) {
  pthread_mutex_lock(&mutex);
  for (int job = 0; job < JOBS; job++) {
    rules[job].held = false;
    drain((jf_job)job);
  }
  memset(rules, 0, sizeof(rules));
  saved_delays[0] = '\0';
  downloaded = false;
  listed_after = 0;
  opensubtitles_status = NULL;
  subbuzz_status = NULL;
  subbuzz_listed = false;
  pthread_mutex_unlock(&mutex);
}

const char *mock_saved_delays(void) { return saved_delays; }
uint64_t mock_progress_ms(void) { return progress_ticks / 10000; }

void mock_fail(jf_job job, int status, unsigned count) {
  pthread_mutex_lock(&mutex);
  rules[job].failing = true;
  rules[job].status = status;
  rules[job].remaining = count != 0 ? count : ~0u;
  pthread_mutex_unlock(&mutex);
}

void mock_hold(jf_job job) {
  pthread_mutex_lock(&mutex);
  rules[job].held = true;
  pthread_mutex_unlock(&mutex);
}

void mock_release(jf_job job) {
  pthread_mutex_lock(&mutex);
  rules[job].held = false;
  drain(job);
  pthread_mutex_unlock(&mutex);
}

unsigned mock_held(void) {
  pthread_mutex_lock(&mutex);
  unsigned count = 0;
  for (int job = 0; job < JOBS; job++)
    count += rules[job].waiting;
  pthread_mutex_unlock(&mutex);
  return count;
}

unsigned mock_calls(jf_job job) {
  pthread_mutex_lock(&mutex);
  const unsigned count = rules[job].calls;
  pthread_mutex_unlock(&mutex);
  return count;
}

const char *mock_id(const char *pattern, ...) {
  static char out[64];
  va_list args;
  va_start(args, pattern);
  vsnprintf(out, sizeof(out), pattern, args);
  va_end(args);
  return out;
}

/* ------------------------------------------------------------------ library */

static const char *text(jf_arena *arena, const char *pattern, ...) {
  char buffer[256];
  va_list args;
  va_start(args, pattern);
  vsnprintf(buffer, sizeof(buffer), pattern, args);
  va_end(args);
  return jf_arena_strdup(arena, buffer);
}

static void movie(jf_arena *arena, unsigned n, jf_item *out) {
  *out = (jf_item){0};
  out->id = text(arena, "movie-%u", n);
  out->name = text(arena, "Movie %03u", n);
  out->type = "Movie";
  out->overview = "A film.";
  out->primary_image_tag = text(arena, "tag-movie-%u", n);
  out->production_year = 1990 + n % 30;
  out->run_time_ticks = 90ull * 60 * 10000000;
  out->community_rating = 7.5;
}

static void series(jf_arena *arena, unsigned n, jf_item *out) {
  *out = (jf_item){0};
  out->id = text(arena, "series-%u", n);
  out->name = text(arena, "Series %02u", n);
  out->type = "Series";
  out->overview = "A show.";
  out->primary_image_tag = text(arena, "tag-series-%u", n);
  out->production_year = 2000 + n % 20;
  out->child_count = MOCK_SEASONS;
  out->has_child_count = true;
}

static void season(jf_arena *arena, unsigned s, unsigned n, jf_item *out) {
  *out = (jf_item){0};
  out->id = text(arena, "season-%u-%u", s, n);
  out->name = text(arena, "Season %u", n);
  out->type = "Season";
  out->series_id = text(arena, "series-%u", s);
  out->series_name = text(arena, "Series %02u", s);
  out->series_primary_image_tag = text(arena, "tag-series-%u", s);
  out->parent_backdrop_item_id = out->series_id;
  out->parent_backdrop_image_tag = text(arena, "tag-backdrop-%u", s);
  out->index_number = n;
  out->child_count = MOCK_EPISODES;
  out->has_child_count = true;
  /* The first season is watched, so a series opens on its second. */
  out->unplayed_item_count = n == 1 ? 0 : MOCK_EPISODES;
  out->has_unplayed_item_count = true;
}

static void episode(jf_arena *arena, unsigned s, unsigned n, unsigned e,
                    jf_item *out) {
  *out = (jf_item){0};
  out->id = text(arena, "episode-%u-%u-%u", s, n, e);
  out->name = text(arena, "Episode %u", e);
  out->type = "Episode";
  out->overview = "An episode.";
  out->series_id = text(arena, "series-%u", s);
  out->series_name = text(arena, "Series %02u", s);
  out->series_primary_image_tag = text(arena, "tag-series-%u", s);
  out->primary_image_tag = text(arena, "tag-episode-%u-%u-%u", s, n, e);
  out->index_number = e;
  out->parent_index_number = n;
  out->run_time_ticks = 24ull * 60 * 10000000;
  out->played = n == 1;
}

static void library(jf_arena *arena, const char *id, jf_item *out) {
  const bool movies = strcmp(id, "library-movies") == 0;
  *out = (jf_item){0};
  out->id = jf_arena_strdup(arena, id);
  out->name = movies ? "Movies" : "Shows";
  out->type = "CollectionFolder";
  out->collection_type = movies ? "movies" : "tvshows";
  out->primary_image_tag = movies ? "tag-movies" : "tag-shows";
}

/* Whatever the id names; false for an id that is none of the above. */
static bool item(jf_arena *arena, const char *id, jf_item *out) {
  unsigned s, n, e;
  if (sscanf(id, "episode-%u-%u-%u", &s, &n, &e) == 3)
    episode(arena, s, n, e, out);
  else if (sscanf(id, "season-%u-%u", &s, &n) == 2)
    season(arena, s, n, out);
  else if (sscanf(id, "series-%u", &s) == 1 && s < MOCK_SERIES)
    series(arena, s, out);
  else if (sscanf(id, "movie-%u", &n) == 1 && n < MOCK_MOVIES)
    movie(arena, n, out);
  else if (strncmp(id, "library-", 8) == 0)
    library(arena, id, out);
  else
    return false;
  return true;
}

static jf_item *items(jf_arena *arena, size_t count) {
  return jf_arena_alloc(arena, (count ? count : 1) * sizeof(jf_item));
}

static void list(jf_task *task, size_t count, uint32_t total) {
  task->list.count = count;
  task->list.total_record_count = total;
  task->list.start_index = task->start;
}

/* A flat colour, different per item, so a test capture shows which is which. */
static bool poster(const char *id, jf_image *out) {
  uint32_t hash = 2166136261u;
  for (const char *p = id; *p != '\0'; p++)
    hash = (hash ^ (uint8_t)*p) * 16777619u;
  out->width = 4;
  out->height = 6;
  out->rgb = malloc(4 * 6 * 3);
  if (out->rgb == NULL)
    return false;
  for (size_t i = 0; i < 4 * 6; i++)
    memcpy(out->rgb + i * 3, &hash, 3);
  return true;
}

static const char *answer(const jf_session *session, jf_task *task) {
  jf_arena *arena = &task->arena;
  unsigned s, n, e;
  switch (task->job) {
  case JF_JOB_DISCOVER:
    task->servers = jf_arena_alloc(arena, sizeof(*task->servers));
    task->servers[0] =
        (jf_discovered){"http://mock.test", "Mock Server", "mock"};
    task->server_count = 1;
    return NULL;
  case JF_JOB_PROBE:
    task->server =
        (jf_discovered){jf_arena_strdup(arena, task->a), "Mock Server", "mock"};
    return NULL;
  case JF_JOB_LOGIN:
  case JF_JOB_QUICK_AUTHENTICATE:
    task->auth = (jf_auth){"mock-token", "mock-user",
                           task->a[0] != '\0' && task->job == JF_JOB_LOGIN
                               ? jf_arena_strdup(arena, task->a)
                               : "tester"};
    return NULL;
  case JF_JOB_QUICK_INITIATE:
  case JF_JOB_QUICK_POLL:
    task->quick = (jf_quick_connect){"mock-secret", "123456",
                                     task->job == JF_JOB_QUICK_POLL};
    return NULL;
  case JF_JOB_VIEWS:
    task->list.items = items(arena, 2);
    library(arena, "library-movies", &task->list.items[0]);
    library(arena, "library-shows", &task->list.items[1]);
    list(task, 2, 2);
    return NULL;
  case JF_JOB_RESUME:
    task->list.items = items(arena, MOCK_RESUME);
    for (unsigned i = 0; i < MOCK_RESUME; i++) {
      movie(arena, i, &task->list.items[i]);
      task->list.items[i].playback_position_ticks = 30ull * 60 * 10000000;
    }
    list(task, MOCK_RESUME, MOCK_RESUME);
    return NULL;
  case JF_JOB_LATEST: {
    const bool movies = strcmp(task->a, "library-movies") == 0;
    task->list.items = items(arena, MOCK_LATEST);
    for (unsigned i = 0; i < MOCK_LATEST; i++)
      if (movies)
        movie(arena, 100 + i, &task->list.items[i]);
      else
        series(arena, i, &task->list.items[i]);
    list(task, MOCK_LATEST, MOCK_LATEST);
    return NULL;
  }
  case JF_JOB_NEXT_UP:
    task->list.items = items(arena, MOCK_NEXT_UP);
    for (unsigned i = 0; i < MOCK_NEXT_UP; i++)
      episode(arena, i, 2, 1, &task->list.items[i]);
    list(task, MOCK_NEXT_UP, MOCK_NEXT_UP);
    return NULL;
  case JF_JOB_CHILDREN: {
    const bool movies = strcmp(task->a, "library-movies") == 0;
    if (!movies && strcmp(task->a, "library-shows") != 0)
      return "ClientError";
    const uint32_t total = movies ? MOCK_MOVIES : MOCK_SERIES;
    const uint32_t start = task->start < total ? task->start : total;
    const uint32_t count =
        task->limit < total - start ? task->limit : total - start;
    task->list.items = items(arena, count);
    for (uint32_t i = 0; i < count; i++)
      (movies ? movie : series)(arena, start + i, &task->list.items[i]);
    list(task, count, total);
    return NULL;
  }
  case JF_JOB_ITEM:
    return item(arena, task->a, &task->one) ? NULL : "ClientError";
  case JF_JOB_SEASONS:
    if (sscanf(task->a, "series-%u", &s) != 1)
      return "ClientError";
    task->list.items = items(arena, MOCK_SEASONS);
    for (unsigned i = 0; i < MOCK_SEASONS; i++)
      season(arena, s, i + 1, &task->list.items[i]);
    list(task, MOCK_SEASONS, MOCK_SEASONS);
    return NULL;
  case JF_JOB_EPISODES:
    if (sscanf(task->b, "season-%u-%u", &s, &n) != 2)
      return "ClientError";
    task->list.items = items(arena, MOCK_EPISODES);
    for (unsigned i = 0; i < MOCK_EPISODES; i++)
      episode(arena, s, n, i + 1, &task->list.items[i]);
    list(task, MOCK_EPISODES, MOCK_EPISODES);
    return NULL;
  case JF_JOB_ADJACENT: {
    if (!item(arena, task->a, &task->one))
      return "ClientError";
    if (sscanf(task->a, "episode-%u-%u-%u", &s, &n, &e) != 3) {
      task->list = (jf_item_list){0};
      return NULL;
    }
    task->list.items = items(arena, 3);
    size_t count = 0;
    for (unsigned at = e > 1 ? e - 1 : e; at <= e + 1 && at <= MOCK_EPISODES;
         at++)
      episode(arena, s, n, at, &task->list.items[count++]);
    list(task, count, (uint32_t)count);
    return NULL;
  }
  case JF_JOB_POSTER:
    task->has_image = poster(task->a, &task->image);
    return task->has_image ? NULL : "HttpStatus";
  case JF_JOB_PLAYBACK_STARTED:
    return NULL;
  case JF_JOB_PLAYBACK_PROGRESS:
    progress_ticks = task->position_ticks;
    return NULL;
  case JF_JOB_PLAYBACK_INFO: {
    static const char *const titles[] = {"English - SRT", "German - ASS",
                                         "Japanese - PGS"};
    static const char *const languages[] = {"eng", "ger", "jpn"};
    jf_playback_info *info = &task->playback;
    info->media_source_id = jf_arena_strdup(arena, task->a);
    info->default_subtitle = -1;
    info->subtitles = jf_arena_alloc(arena, 4 * sizeof(*info->subtitles));
    /* A download sorts first among the files, as a server's might. */
    if (listed_after > 0)
      listed_after--;
    else if (downloaded)
      info->subtitles[info->subtitle_count++] = (jf_media_stream){
          1, "Thai - Downloaded", true, true, false, "tha", NULL};
    for (int i = 0; i < 3; i++)
      info->subtitles[info->subtitle_count++] = (jf_media_stream){
          i + 2, titles[i], i == 0, i < 2, i == 1, languages[i], NULL};
    /* The player mock's two audio tracks, numbered as it numbers them. */
    info->default_audio = 0;
    info->audio = jf_arena_alloc(arena, 2 * sizeof(*info->audio));
    info->audio[info->audio_count++] =
        (jf_media_stream){0, "English", false, false, false, "eng", NULL};
    info->audio[info->audio_count++] =
        (jf_media_stream){1, "Commentary", false, false, false, "eng", "Comm"};
    return NULL;
  }
  case JF_JOB_DELAYS: {
    /* The one pair a save leaves behind, as `"key":"ms"`. */
    char key[128];
    int ms = 0;
    if (sscanf(saved_delays, "%*[^{]{%*[^{]{\"%127[^\"]\":\"%d", key, &ms) != 2)
      return NULL;
    task->keys = jf_arena_alloc(arena, sizeof(*task->keys));
    task->values = jf_arena_alloc(arena, sizeof(*task->values));
    task->keys[0] = jf_arena_strdup(arena, key);
    char value[16];
    snprintf(value, sizeof(value), "%d", ms);
    task->values[0] = jf_arena_strdup(arena, value);
    task->pair_count = 1;
    return NULL;
  }
  case JF_JOB_SAVE_DELAYS:
    snprintf(saved_delays, sizeof(saved_delays), "%s",
             task->body != NULL ? task->body : "");
    return NULL;
  case JF_JOB_SUBTITLE_SEARCH: {
    task->remote = jf_arena_alloc(arena, 2 * sizeof(*task->remote));
    task->remote[0] = (jf_remote_subtitle){"os-1", "Show.S01E01.WEB.srt",
                                           "Open Subtitles", "srt", 1200};
    task->remote[1] = (jf_remote_subtitle){"os-2", "Show.S01E01.HDTV.srt",
                                           "Open Subtitles", "srt", 30};
    task->remote_count = 2;
    return NULL;
  }
  case JF_JOB_SUBTITLE_DOWNLOAD:
    /* Listed only after a few asks, as a server refreshing the item in the
     * background lists it. */
    downloaded = true;
    listed_after = 3;
    return NULL;
  case JF_JOB_SUBTITLE_SOURCES:
    task->sources = jf_arena_alloc(arena, 2 * sizeof(*task->sources));
    task->sources[task->source_count++] = (jf_subtitle_source){
        "Open Subtitles", "4b9ed42f518548b598036ff2989014c4",
        opensubtitles_status != NULL ? "1.0.0.0" : NULL, opensubtitles_status};
    if (subbuzz_listed)
      task->sources[task->source_count++] = (jf_subtitle_source){
          "subbuzz", "5aeab01b2ef845c6bb6b16ce9cb268d4",
          subbuzz_status != NULL ? "10.11.0.0" : NULL, subbuzz_status};
    return NULL;
  case JF_JOB_SOURCE_INSTALL:
    /* As the server: installed, loaded at the next restart. */
    *(strcmp(task->a, "subbuzz") == 0 ? &subbuzz_status
                                      : &opensubtitles_status) = "Restart";
    return NULL;
  case JF_JOB_SOURCE_TOGGLE:
    *(strncmp(task->a, "5aeab01b", 8) == 0 ? &subbuzz_status
                                           : &opensubtitles_status) =
        task->start ? "Restart" : "Disabled";
    return NULL;
  case JF_JOB_ADD_REPOSITORY:
    subbuzz_listed = true;
    return NULL;
  case JF_JOB_RESTART:
  case JF_JOB_MARK_PLAYED:
    return NULL;
  case JF_JOB_SUBTITLE_CHECK:
    return downloaded ? NULL : "ClientError";
  case JF_JOB_SUBTITLE_DELETE:
    /* Only the download is a file the mock can delete. */
    if (!downloaded || strcmp(task->b, "1") != 0)
      return "ClientError";
    downloaded = false;
    return NULL;
  case JF_JOB_ENCODING:
    task->encoding = (jf_server_transcode){.known = true, .codecs = ","};
    return NULL;
  }
  (void)session;
  return "HttpStatus";
}

static const char *failure(int status) {
  if (status == 0)
    return "RequestFailed";
  if (status == 401 || status == 403)
    return "Unauthorized";
  if (status >= 400 && status < 500)
    return "ClientError";
  if (status >= 500 && status < 600)
    return "ServerError";
  return "HttpStatus";
}

/* --------------------------------------------------------------- transport */

/* The event socket: nothing connects, and mock_event says what one would. */
static void (*event_changed)(const jf_user_data *items, size_t count);
void jf_events_start(const jf_session *session,
                     void (*changed)(const jf_user_data *items, size_t count)) {
  (void)session;
  event_changed = changed;
}
void jf_events_stop(void) { event_changed = NULL; }
void mock_event(const jf_user_data *items, size_t count) {
  if (event_changed != NULL)
    event_changed(items, count);
}

void *jf_api_connect(void) { return &rules; }
void jf_api_disconnect(void *connection) { (void)connection; }

const char *jf_api_execute(void *connection, const jf_session *session,
                           jf_task *task) {
  (void)connection;
  pthread_mutex_lock(&mutex);
  if (rules[task->job].held) {
    rules[task->job].waiting++;
    while (rules[task->job].held)
      pthread_cond_wait(&released, &mutex);
    rules[task->job].waiting--;
    pthread_cond_broadcast(&left);
  }
  rules[task->job].calls++;
  const char *error = NULL;
  if (rules[task->job].failing && rules[task->job].remaining != 0) {
    error = failure(rules[task->job].status);
    if (rules[task->job].remaining != ~0u)
      rules[task->job].remaining--;
  }
  pthread_mutex_unlock(&mutex);
  return error != NULL ? error : answer(session, task);
}
