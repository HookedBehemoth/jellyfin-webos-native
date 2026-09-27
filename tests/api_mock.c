/* The transport api.h asks for, answering from a made-up library. See
 * api_mock.h. */
#include "api_mock.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOBS (JF_JOB_ENCODING + 1)

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
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
  pthread_mutex_unlock(&mutex);
}

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
  case JF_JOB_PLAYBACK_PROGRESS:
    return NULL;
  case JF_JOB_PLAYBACK_INFO: {
    static const char *const titles[] = {"English - SRT", "German - ASS",
                                         "Japanese - PGS"};
    jf_playback_info *info = &task->playback;
    info->media_source_id = jf_arena_strdup(arena, task->a);
    info->default_subtitle = -1;
    info->subtitles = jf_arena_alloc(arena, 3 * sizeof(*info->subtitles));
    for (int i = 0; i < 3; i++)
      info->subtitles[i] =
          (jf_subtitle_stream){i + 2, titles[i], i == 0, i < 2};
    info->subtitle_count = 3;
    return NULL;
  }
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
