/*
 * Jellyfin server API, plus the background fetcher that keeps it off the render thread.
 *
 * Everything here is blocking and allocates into a per-task arena; the UI never calls it
 * directly. The fetcher owns a small fixed pool of task slots and a few worker threads,
 * the UI submits a task and polls it once a frame, and the arena is released when the UI
 * is done reading the result. That is the whole concurrency model - no futures, no
 * callbacks, nothing shared but the slot array behind one mutex.
 *
 * Verified against Jellyfin 10.11.8.
 */
#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "arena.h"
#include "image.h"
#include "store.h"

#define JF_CLIENT_NAME "webos-native"
#define JF_CLIENT_VERSION "0.1"

/* ------------------------------------------------------------------ model
 *
 * The fields this UI reads; the rest of a very wide server DTO is ignored, so adding a
 * screen means adding a field here only. Strings point into the owning task's arena. */
typedef struct {
    const char *id;
    const char *name;
    const char *type;
    const char *collection_type;
    const char *overview;
    const char *premiere_date;
    const char *end_date;
    const char *status;
    const char *official_rating;
    const char *series_name;
    const char *series_id;
    /* The series' own Primary tag, carried on every episode - so an episode can name the
     * artwork jf_item_poster_tag falls back to without fetching the series first. */
    const char *series_primary_image_tag;
    const char *primary_image_tag;
    const char *backdrop_image_tag;      /* BackdropImageTags[0] */
    const char *parent_backdrop_item_id;
    const char *parent_backdrop_image_tag;
    /* ImageBlurHashes for the two Primary tags above, or NULL. */
    const char *series_primary_blurhash;
    const char *primary_blurhash;
    /* Its blurhash decoded into cell blur_cell of the task's blur rows; width 0
     * if none. */
    uint32_t blur_cell;
    uint8_t blur_width, blur_height;

    uint32_t production_year;
    double community_rating;
    uint64_t run_time_ticks;
    uint32_t index_number;
    uint32_t parent_index_number;
    uint32_t child_count;
    bool has_child_count;

    /* UserData */
    double played_percentage;
    bool has_played_percentage;
    uint64_t playback_position_ticks;
    bool played;
    uint32_t unplayed_item_count;
    bool has_unplayed_item_count;
} jf_item;

bool jf_item_has_poster(const jf_item *item);
/* The id whose Primary image represents this item on a portrait tile. An episode's own
 * Primary is a 16:9 still, which looks wrong stretched into a poster and makes a
 * "continue watching" row look like a different kind of list, so an episode always shows
 * its series' poster. */
const char *jf_item_poster_id(const jf_item *item);
/* The image tag for whatever poster_id points at. Jellyfin's tags are content hashes, so
 * this doubles as the cache key - see store.h. */
const char *jf_item_poster_tag(const jf_item *item);
const char *jf_item_poster_blurhash(const jf_item *item);
bool jf_item_is_folder(const jf_item *item);
uint32_t jf_item_minutes(const jf_item *item);
bool jf_item_finished(const jf_item *item);
/* 0..100, for the resume bar. The server only fills PlayedPercentage on some endpoints,
 * so it is derived from the position when it is missing. */
float jf_item_progress(const jf_item *item);

typedef struct {
    jf_item *items;
    size_t count;
    uint32_t total_record_count;
    uint32_t start_index;
} jf_item_list;

typedef struct {
    const char *access_token;
    const char *user_id;
    const char *user_name;
} jf_auth;

typedef struct {
    const char *secret;
    const char *code;
    bool authenticated;
} jf_quick_connect;

typedef struct {
    const char *address;
    const char *name;
    const char *id;
} jf_discovered;

/* ------------------------------------------------------------- credentials
 *
 * Everything needed to talk to a server, and the whole of what is persisted. Inline
 * strings: the session is global state read by worker threads, so no allocator and no
 * lifetime to get wrong. */
typedef struct {
    char url[512];
    char token[256];
    char user_id[64];
    char user_name[128];
    /* Kept so an invalidated token can be replaced without the user. Empty after a Quick
     * Connect sign-in, which never sees one. */
    char password[128];
    char device_id[64];
} jf_session;

/* A stable per-installation id, so the server's device list does not grow a new entry
 * every launch and Quick Connect approvals stick. */
void jf_session_device_id(jf_session *session);
void jf_session_authorization(const jf_session *session, char *out, size_t out_len);
void jf_session_save(const jf_session *session);
bool jf_session_load(jf_session *session);

/* Resolve where this app writes, sweep the artwork cache back under budget and report the
 * libpng version. All before the fetcher's workers start. */
void jf_api_init(void);

/* What the server should send when the original is beyond the TV's decoder, and the
 * direct stream URL for when it is not. Both write into `out`. */
void jf_transcode_url(const jf_session *session, const char *id, char *out, size_t out_len);
void jf_stream_url(const jf_session *session, const char *id, char *out, size_t out_len);

/* ---------------------------------------------------------------- fetcher */

typedef enum {
    JF_JOB_DISCOVER,
    JF_JOB_PROBE,
    JF_JOB_LOGIN,
    JF_JOB_QUICK_INITIATE,
    JF_JOB_QUICK_POLL,
    JF_JOB_QUICK_AUTHENTICATE,
    JF_JOB_VIEWS,
    JF_JOB_RESUME,
    JF_JOB_NEXT_UP,
    JF_JOB_CHILDREN,
    JF_JOB_ITEM,
    JF_JOB_SEASONS,
    JF_JOB_EPISODES,
    JF_JOB_POSTER,
    JF_JOB_PLAYBACK_STARTED,
    JF_JOB_PLAYBACK_PROGRESS,
} jf_job;

typedef enum { JF_IMAGE_PRIMARY, JF_IMAGE_BACKDROP } jf_image_kind;

typedef enum {
    JF_TASK_FREE,
    JF_TASK_RESERVED,
    JF_TASK_QUEUED,
    JF_TASK_RUNNING,
    JF_TASK_READY,
    JF_TASK_FAILED,
} jf_task_state;

/* One unit of work and its result, in the same object. The UI owns a task from submit
 * until release, a worker owns it in between, and `state` is the handover - so nothing
 * inside needs its own lock. */
typedef struct {
    jf_job job;
    /* Parent/series id, username, or Quick Connect secret, per job. */
    char a[512];
    /* Season id or password, per job. */
    char b[512];
    uint32_t start, limit;
    uint64_t position_ticks;
    jf_image_kind image_kind;
    /* Opaque to the fetcher: the UI uses it to match a result to the row, grid slot or
     * poster tile that asked for it. */
    uint32_t tag;

    jf_arena arena;
    jf_task_state state;

    jf_item_list list;
    jf_item one;
    jf_auth auth;
    jf_quick_connect quick;
    jf_discovered *servers;
    size_t server_count;
    jf_discovered server;
    jf_image image;
    bool has_image;
    /* Every item's blurhash, JF_BLUR_COLUMNS cells to a row, ready to go into
     * the UI's atlas in one upload. NULL when no item has one. */
    uint8_t *blur;
    uint32_t blur_rows;
    char error[128];
} jf_task;

/* Blurhash cells, in texels. A blurhash is smooth, so this is plenty when
 * upscaled. */
#define JF_BLUR_CELL 32
#define JF_BLUR_COLUMNS 32

/* Deep enough for a screen of posters plus the page request that named them; a full pool
 * makes submit return NULL and the UI retry next frame. */
#define JF_TASK_SLOTS 32
#define JF_FETCH_WORKERS 4

typedef struct {
    pthread_mutex_t mutex;
    pthread_cond_t work_ready;
    jf_task tasks[JF_TASK_SLOTS];
    jf_task *queue[JF_TASK_SLOTS];
    size_t queue_head, queue_count;
    pthread_t threads[JF_FETCH_WORKERS];
    size_t thread_count;
    void (*wake)(void);
    /* The session workers use. Copied under the lock at the start of every task, so the
     * UI can replace it between frames without racing. */
    jf_session session;
    bool running;
} jf_fetcher;

bool jf_fetcher_init(jf_fetcher *fetcher, void (*wake)(void));
void jf_fetcher_deinit(jf_fetcher *fetcher);
void jf_fetcher_set_session(jf_fetcher *fetcher, const jf_session *session);

/* Reserve a slot; only jf_fetcher_start publishes its inputs to workers. Between the two
 * the slot is reserved but not yet visible to a worker, so the caller can fill the inputs
 * in without a lock. */
jf_task *jf_fetcher_submit(jf_fetcher *fetcher, jf_job job, uint32_t tag);
void jf_fetcher_start(jf_fetcher *fetcher, jf_task *task);
/* The next finished task, or NULL. Call until it returns NULL each frame. */
jf_task *jf_fetcher_finished(jf_fetcher *fetcher);
void jf_fetcher_release(jf_fetcher *fetcher, jf_task *task);
/* Tasks not yet handed back by the UI - in flight *or* finished and still waiting to be
 * consumed. The finished-but-unconsumed case matters: a caller that treats "nothing in
 * flight" as "the screen is up to date" races the frame between a worker finishing and
 * jf_fetcher_finished being drained. */
size_t jf_fetcher_pending(jf_fetcher *fetcher);

/* Exposed for the host tests. */
void jf_url_escape(char *out, size_t out_len, const char *value);
const char *jf_session_base(const jf_session *session, char *out, size_t out_len);
