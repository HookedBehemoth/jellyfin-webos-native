#include "api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/os.h"
#include "blurhash.h"

/* ------------------------------------------------------------------ model */

static bool same(const char *a, const char *b) {
  return a != NULL && strcmp(a, b) == 0;
}

bool jf_item_has_poster(const jf_item *item) {
  return item->primary_image_tag != NULL && item->primary_image_tag[0] != '\0';
}

static bool uses_series_poster(const jf_item *item) {
  return same(item->type, "Episode") ||
         (same(item->type, "Season") && !jf_item_has_poster(item));
}

const char *jf_item_poster_id(const jf_item *item) {
  if (uses_series_poster(item) && item->series_id != NULL)
    return item->series_id;
  return item->id != NULL ? item->id : "";
}

const char *jf_item_poster_tag(const jf_item *item) {
  if (uses_series_poster(item) && item->series_id != NULL)
    return item->series_primary_image_tag != NULL
               ? item->series_primary_image_tag
               : "";
  return item->primary_image_tag != NULL ? item->primary_image_tag : "";
}

const char *jf_item_poster_blurhash(const jf_item *item) {
  if (uses_series_poster(item) && item->series_id != NULL)
    return item->series_primary_blurhash;
  return item->primary_blurhash;
}

bool jf_item_is_folder(const jf_item *item) {
  return same(item->type, "Series") || same(item->type, "Season") ||
         same(item->type, "BoxSet") || same(item->type, "CollectionFolder");
}

uint32_t jf_item_minutes(const jf_item *item) {
  return (uint32_t)(item->run_time_ticks / (10000000ull * 60));
}

bool jf_item_finished(const jf_item *item) {
  if (item->played)
    return true;
  if (jf_item_is_folder(item) && item->has_unplayed_item_count &&
      item->unplayed_item_count == 0)
    return true;
  return same(item->type, "Season") && item->has_child_count &&
         item->child_count == 0;
}

float jf_item_progress(const jf_item *item) {
  if (item->has_played_percentage)
    return (float)item->played_percentage;
  if (item->playback_position_ticks == 0 || item->run_time_ticks == 0)
    return 0;
  return (float)item->playback_position_ticks / (float)item->run_time_ticks *
         100.0f;
}

/* ------------------------------------------------------------- credentials */

void jf_session_authorization(const jf_session *session, char *out,
                              size_t out_len) {
  snprintf(out, out_len,
           "MediaBrowser Client=\"%s\", Device=\"webOS TV\", DeviceId=\"%s\", "
           "Version=\"%s\", "
           "Token=\"%s\"",
           JF_CLIENT_NAME, session->device_id, JF_CLIENT_VERSION,
           session->token);
}

void jf_session_device_id(jf_session *session) {
  char host[64] = "webos";
  jf_os_hostname(host, sizeof(host));
  /* FNV-1a over the host name and the client name: stable across launches, and
   * not a value anyone needs to reverse. */
  uint64_t hash = 1469598103934665603ull;
  for (const char *p = host; *p != '\0'; p++)
    hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
  for (const char *p = JF_CLIENT_NAME; *p != '\0'; p++)
    hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
  snprintf(session->device_id, sizeof(session->device_id), "%016llx",
           (unsigned long long)hash);
}

void jf_session_save(const jf_session *session) {
  jf_credentials credentials;
  memset(&credentials, 0, sizeof(credentials));
  snprintf(credentials.server, sizeof(credentials.server), "%s", session->url);
  snprintf(credentials.token, sizeof(credentials.token), "%s", session->token);
  snprintf(credentials.user_id, sizeof(credentials.user_id), "%s",
           session->user_id);
  snprintf(credentials.user_name, sizeof(credentials.user_name), "%s",
           session->user_name);
  snprintf(credentials.password, sizeof(credentials.password), "%s",
           session->password);
  jf_store_save(&credentials);
}

bool jf_session_load(jf_session *session) {
  jf_credentials credentials;
  if (!jf_store_load(&credentials))
    return false;
  snprintf(session->url, sizeof(session->url), "%s", credentials.server);
  snprintf(session->token, sizeof(session->token), "%s", credentials.token);
  snprintf(session->user_id, sizeof(session->user_id), "%s",
           credentials.user_id);
  snprintf(session->user_name, sizeof(session->user_name), "%s",
           credentials.user_name);
  snprintf(session->password, sizeof(session->password), "%s",
           credentials.password);
  return session->token[0] != '\0';
}

void jf_api_init(void) {
  jf_image_report_version();
  jf_store_init();
  jf_store_prune();
}

/* `base` without a trailing slash, so every URL below is "{base}/Path". */
const char *jf_session_base(const jf_session *session, char *out,
                            size_t out_len) {
  snprintf(out, out_len, "%s", session->url);
  const size_t length = strlen(out);
  if (length > 0 && out[length - 1] == '/')
    out[length - 1] = '\0';
  return out;
}

/* Percent-encode a query value. Library names and search text reach the URL,
 * and a space or `&` in one would otherwise build a different request. */
void jf_url_escape(char *out, size_t out_len, const char *value) {
  size_t at = 0;
  for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
    const bool safe = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                      (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' ||
                      *p == '.' || *p == '~';
    if (safe) {
      if (at + 2 > out_len)
        break;
      out[at++] = (char)*p;
    } else {
      if (at + 4 > out_len)
        break;
      at += (size_t)snprintf(out + at, out_len - at, "%%%02X", *p);
    }
  }
  out[at < out_len ? at : out_len - 1] = '\0';
}

/* -------------------------------------------------------------------- urls */

void jf_transcode_url(const jf_session *session, const char *id, char *out,
                      size_t out_len) {
  /* MPEG-TS lets Jellyfin remux compatible video instead of re-encoding it; the
   * player demuxes its packets locally.
   *
   * Stream copy is deliberately disabled as well as the codecs named. This path
   * is selected precisely because the source is outside the hardware decoder's
   * limits, so a copied H.264 stream can still be High 10, 4:2:2, or above the
   * level the decoder accepts. The h264-* options are Jellyfin's codec profile
   * constraints; generic `profile` and `level` alone do not constrain all
   * server versions' encoder choice.
   *
   * The bitrate has to be stated. Left out, the server picks a default meant
   * for a client that declared no capabilities, and 1080p arrives visibly soft.
   */
  char base[512];
  snprintf(
      out, out_len,
      "%s/Videos/%s/stream.ts?videoCodec=h264&audioCodec=mp3"
      "&allowVideoStreamCopy=false&allowAudioStreamCopy=false"
      "&maxVideoBitDepth=8&profile=high&level=41"
      "&h264-profile=high&h264-level=41&h264-videobitdepth=8&h264-rangetype=SDR"
      "&requireAvc=true&maxAudioChannels=2&transcodingMaxAudioChannels=2"
      /* Some server releases only honour the copy-disabling flags after a
       * transcode reason is supplied. Without it, an unsupported H.264 source
       * may be remuxed again and its seek request accepted but ignored by the
       * live response. */
      "&enableAutoStreamCopy=false&transcodeReasons=VideoCodecNotSupported"
      "&videoBitRate=20000000&audioBitRate=192000&api_key=%s",
      jf_session_base(session, base, sizeof(base)), id, session->token);
}

void jf_stream_url(const jf_session *session, const char *id, char *out,
                   size_t out_len) {
  /* static=true is the original file, byte ranges and all, which is what makes
   * seeking work: a server-side remux is a live stream with no Content-Length,
   * so neither av_seek_frame nor StartTimeTicks moves it. */
  char base[512];
  snprintf(out, out_len, "%s/Videos/%s/stream?static=true&api_key=%s",
           jf_session_base(session, base, sizeof(base)), id, session->token);
}

void jf_subtitle_url(const jf_session *session, const char *id,
                     const char *media_source_id, int index, char *out,
                     size_t out_len) {
  char base[512];
  snprintf(out, out_len, "%s/Videos/%s/%s/Subtitles/%d/0/Stream.ass?api_key=%s",
           jf_session_base(session, base, sizeof(base)), id, media_source_id,
           index, session->token);
}

/* Decodes the page's blurhashes on this worker, into rows the UI uploads as one
 * piece. The episode list shows 16:9 stills; everything else is a 2:3 poster.
 */
static void decode_blurs(jf_task *task) {
  const bool still = task->job == JF_JOB_EPISODES || task->job == JF_JOB_VIEWS;
  const uint8_t width = still ? JF_BLUR_CELL : JF_BLUR_CELL * 2 / 3;
  const uint8_t height = still ? JF_BLUR_CELL * 9 / 16 : JF_BLUR_CELL;
  const size_t row_bytes =
      (size_t)JF_BLUR_COLUMNS * JF_BLUR_CELL * JF_BLUR_CELL * 3;
  const size_t stride = (size_t)JF_BLUR_COLUMNS * JF_BLUR_CELL * 3;
  uint8_t cell[JF_BLUR_CELL * JF_BLUR_CELL * 3];
  for (size_t i = 0; i < task->list.count; i++) {
    jf_item *item = &task->list.items[i];
    const char *hash =
        still ? item->primary_blurhash : jf_item_poster_blurhash(item);
    if (!jf_blurhash_decode(hash, width, height, cell))
      continue;
    if (task->blur == NULL) {
      task->blur_rows = (uint32_t)((task->list.count + JF_BLUR_COLUMNS - 1) /
                                   JF_BLUR_COLUMNS);
      task->blur = jf_arena_alloc(&task->arena, task->blur_rows * row_bytes);
      if (task->blur == NULL)
        return;
    }
    uint8_t *origin = task->blur + i / JF_BLUR_COLUMNS * row_bytes +
                      i % JF_BLUR_COLUMNS * JF_BLUR_CELL * 3;
    for (uint8_t y = 0; y < height; y++)
      memcpy(origin + y * stride, cell + (size_t)y * width * 3,
             (size_t)width * 3);
    item->blur_cell = (uint32_t)i;
    item->blur_width = width;
    item->blur_height = height;
  }
}

/* ----------------------------------------------------------------- fetcher */

static void *worker(void *user) {
  jf_fetcher *fetcher = user;
  void *connection = jf_api_connect();

  for (;;) {
    pthread_mutex_lock(&fetcher->mutex);
    while (fetcher->running && fetcher->queue_count == 0)
      pthread_cond_wait(&fetcher->work_ready, &fetcher->mutex);
    if (!fetcher->running) {
      pthread_mutex_unlock(&fetcher->mutex);
      break;
    }
    jf_task *task = fetcher->queue[fetcher->queue_head];
    fetcher->queue_head = (fetcher->queue_head + 1) % JF_TASK_SLOTS;
    fetcher->queue_count--;
    task->state = JF_TASK_RUNNING;
    const jf_session session = fetcher->session;
    pthread_mutex_unlock(&fetcher->mutex);

    const char *error = jf_api_execute(connection, &session, task);
    if (error == NULL)
      decode_blurs(task);

    pthread_mutex_lock(&fetcher->mutex);
    if (error == NULL) {
      task->state = JF_TASK_READY;
    } else {
      snprintf(task->error, sizeof(task->error), "%s", error);
      task->state = JF_TASK_FAILED;
    }
    pthread_mutex_unlock(&fetcher->mutex);
    if (fetcher->wake != NULL)
      fetcher->wake();
  }
  jf_api_disconnect(connection);
  return NULL;
}

bool jf_fetcher_init(jf_fetcher *fetcher, void (*wake)(void)) {
  memset(fetcher, 0, sizeof(*fetcher));
  pthread_mutex_init(&fetcher->mutex, NULL);
  pthread_cond_init(&fetcher->work_ready, NULL);
  fetcher->wake = wake;
  fetcher->running = true;
  for (size_t i = 0; i < JF_FETCH_WORKERS; i++) {
    if (pthread_create(&fetcher->threads[i], NULL, worker, fetcher) != 0)
      break;
    fetcher->thread_count++;
  }
  return fetcher->thread_count > 0;
}

void jf_fetcher_deinit(jf_fetcher *fetcher) {
  pthread_mutex_lock(&fetcher->mutex);
  fetcher->running = false;
  pthread_cond_broadcast(&fetcher->work_ready);
  pthread_mutex_unlock(&fetcher->mutex);
  for (size_t i = 0; i < fetcher->thread_count; i++)
    pthread_join(fetcher->threads[i], NULL);
  for (size_t i = 0; i < JF_TASK_SLOTS; i++) {
    if (fetcher->tasks[i].has_image)
      jf_image_free(&fetcher->tasks[i].image);
    jf_arena_destroy(&fetcher->tasks[i].arena);
  }
  pthread_mutex_destroy(&fetcher->mutex);
  pthread_cond_destroy(&fetcher->work_ready);
}

void jf_fetcher_set_session(jf_fetcher *fetcher, const jf_session *session) {
  pthread_mutex_lock(&fetcher->mutex);
  fetcher->session = *session;
  pthread_mutex_unlock(&fetcher->mutex);
}

jf_task *jf_fetcher_submit(jf_fetcher *fetcher, jf_job job, uint32_t tag) {
  pthread_mutex_lock(&fetcher->mutex);
  jf_task *found = NULL;
  for (size_t i = 0; i < JF_TASK_SLOTS && found == NULL; i++) {
    jf_task *task = &fetcher->tasks[i];
    if (task->state != JF_TASK_FREE)
      continue;
    /* The arena outlives the reset: it is the slot's storage, not the task's.
     */
    const jf_arena arena = task->arena;
    memset(task, 0, sizeof(*task));
    task->arena = arena;
    task->job = job;
    task->tag = tag;
    task->state = JF_TASK_RESERVED;
    found = task;
  }
  pthread_mutex_unlock(&fetcher->mutex);
  return found;
}

void jf_fetcher_start(jf_fetcher *fetcher, jf_task *task) {
  pthread_mutex_lock(&fetcher->mutex);
  task->state = JF_TASK_QUEUED;
  /* There are only JF_TASK_SLOTS tasks, so publishing a reserved one cannot
   * overfill a queue of the same capacity. */
  fetcher->queue[(fetcher->queue_head + fetcher->queue_count) % JF_TASK_SLOTS] =
      task;
  fetcher->queue_count++;
  pthread_cond_signal(&fetcher->work_ready);
  pthread_mutex_unlock(&fetcher->mutex);
}

jf_task *jf_fetcher_finished(jf_fetcher *fetcher) {
  pthread_mutex_lock(&fetcher->mutex);
  jf_task *found = NULL;
  for (size_t i = 0; i < JF_TASK_SLOTS && found == NULL; i++) {
    const jf_task_state state = fetcher->tasks[i].state;
    if (state == JF_TASK_READY || state == JF_TASK_FAILED)
      found = &fetcher->tasks[i];
  }
  pthread_mutex_unlock(&fetcher->mutex);
  return found;
}

void jf_fetcher_release(jf_fetcher *fetcher, jf_task *task) {
  /* The decoded image is the one result that is not in the arena: libpng wrote
   * it into a malloc'd buffer the UI has now copied to a texture. */
  if (task->has_image) {
    jf_image_free(&task->image);
    task->has_image = false;
  }
  jf_arena_reset(&task->arena);
  pthread_mutex_lock(&fetcher->mutex);
  task->state = JF_TASK_FREE;
  pthread_mutex_unlock(&fetcher->mutex);
}

size_t jf_fetcher_pending(jf_fetcher *fetcher) {
  pthread_mutex_lock(&fetcher->mutex);
  size_t count = 0;
  for (size_t i = 0; i < JF_TASK_SLOTS; i++)
    if (fetcher->tasks[i].state != JF_TASK_FREE)
      count++;
  pthread_mutex_unlock(&fetcher->mutex);
  return count;
}
