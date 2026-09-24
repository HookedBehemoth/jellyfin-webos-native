/*
 * Jellyfin playback.
 *
 * Three threads move one segment of playback:
 *
 *   reader   demuxes ahead, decodes audio to PCM, fills two bounded queues
 *   video    feeds access units to Starfish, paced against the pipeline's own clock
 *   audio    writes PCM to ALSA, placed against that same clock
 *
 * Where playback is comes from the pipeline: `option.queryPosition` in the load payload
 * keeps `getCurrentPlaytime()` maintained, and the clock thread below samples it. The
 * same option is documented as also reporting each displayed frame as a FRAMEREADY
 * event, and on some firmware it does - this TV's does not emit one at all - so the event
 * feeds the same anchor when it arrives and nothing depends on it arriving.
 *
 * Audio does not go through Starfish. It used to, as PCM through the pipeline's own sink,
 * and most of what that needed - a sample cursor, silence padding, 1024-sample access
 * units, a preroll gate before Play, a restricted list of sample rates - existed only to
 * work around a sink that never said where it was. ALSA does say, so all of it is gone;
 * see audio_alsa.h. What is left is one rule: the video clock is the master, and audio is
 * placed against it.
 */
#include "player.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../platform/window.h"
#include "audio_alsa.h"
#include "clock.h"
#include "demux.h"
#include "packet_queue.h"
#include "picsubs.h"
#include "smp.h"
#include "smp_payload.h"
#include "smp_segment.h"
#include "subs.h"

#define APP_ID "dev.hookedbehemoth.jellyfin"
#define MAX_URI 2047

/* Queue bounds, not sync corrections: enough video for decoder continuity, and a shorter
 * audio lead so stale sound cannot accumulate across a pause or a seek. */
#define VIDEO_FEED_AHEAD_NS (1600 * 1000000LL)
#define AUDIO_FEED_AHEAD_NS (400 * 1000000LL)
#define PLAYBACK_RATE_MILLIS 1000

bool jf_player_audio = true;
bool jf_player_subtitles = true;
const char *jf_player_audio_device = "default";

static atomic_int playback_state = JF_IDLE;
static char error_text[160];

static atomic_bool running;
/* True while a stretch of playback is flowing. A seek clears it to park the reader and
 * both feed threads without unloading the pipeline; `running` stays set, so the session
 * survives and only the segment restarts. */
static atomic_bool segment_flowing;
static atomic_bool load_complete;
static atomic_bool pipeline_playing;
static atomic_bool paused;
static atomic_bool play_requested;
static atomic_uint frame_ready_count;
static atomic_int fed_video_ms = INT32_MIN;
static atomic_bool first_video_feed = true;
static atomic_bool warned_no_clock;

static atomic_int position_ms;
/* Where in the item the current source starts: zero for a stream opened from the
 * beginning, the seek target for one the server re-cut at an offset. Read from the
 * pipeline's thread, so it is atomic like the position it is added to. */
static atomic_int stream_base_ms;
static atomic_bool seek_pending;
static int seek_to_ms;
/* What the UI shows from a seek's request until playback reaches its target,
 * -1 when none: a static seek resumes at the keyframe before the target, and
 * the old position lingers until the new segment has a clock. */
static atomic_int held_position = -1;

static pthread_mutex_t segment_mutex = PTHREAD_MUTEX_INITIALIZER;
/* StarfishMediaAPIs is one C++ object; feeds and state changes must not enter it
 * concurrently from the reader, audio and UI threads. */
static pthread_mutex_t pipeline_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Cuts a pacing sleep short so teardown does not wait out a second of cushion. */
static pthread_mutex_t interrupt_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t interrupt_cond = PTHREAD_COND_INITIALIZER;

static jf_queue video_queue;
static jf_queue audio_queue;
static bool queues_ready;

static char uri[MAX_URI + 1];
/* What to ask for instead when the original is beyond the decoder. Empty means there is
 * nothing else to try. */
static char fallback[MAX_URI + 1];
static int read_video = -1;
static int read_audio = -1;
/* Subtitle tracks, from the server's playback info. The UI selects; the
 * reader decodes the selected track from the container when it can, and the
 * loader thread fetches the server's whole copy of a text track. Each
 * selection gets a serial, so either can tell when it has been overtaken. */
#define MAX_SUB_TRACKS 32
static pthread_mutex_t tracks_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tracks_changed = PTHREAD_COND_INITIALIZER;
static jf_player_track sub_tracks[MAX_SUB_TRACKS];
static int sub_track_count;
/* The two sources of the list: the server's, and the container's own streams,
 * which catch a track the server's scan predates. */
static jf_player_track server_tracks[MAX_SUB_TRACKS],
    container_tracks[MAX_SUB_TRACKS];
static int server_track_count, container_track_count;
static int selected_track = -1;
static uint64_t selection;
/* The selection the reader last acted on. */
static uint64_t read_selection;
/* Every bitmap track, decoded all along; see picsubs.h. Set before the reader
 * starts. */
static bool bitmap_streams[JF_DEMUX_SUB_STREAMS];
/* Every text track's decoder is open too, but only the selected one is
 * decoded - except while the picker is up, when all are, so a switch finds the
 * lines already running; see jf_subs_feed. */
static bool text_streams[JF_DEMUX_SUB_STREAMS];
static atomic_int read_text = -1;
static atomic_bool previewing;
static atomic_uint subtitle_lines_fed;
/* Whether the source is the server's transcode, which carries no subtitles. */
static atomic_bool source_transcoded;
/* Where on the item's timeline the segment's zero is. */
static atomic_llong timeline_origin_ms;
static uint32_t frame_width = 1920, frame_height = 1080;
/* The picture subtitles are laid out over, which the TV fits into the frame. */
static int video_width = 1920, video_height = 1080;
static double video_frame_ms;
static int audio_rate = 48000;
/* The original's audio streams, listed as the session opens it. A switch is a
 * seek to where playback is with the other stream's decoder: the reader runs
 * seconds ahead, so anything gentler plays the old language for that long. */
#define MAX_AUDIO_TRACKS 16
static struct {
  int stream;
  char name[96];
} audio_tracks[MAX_AUDIO_TRACKS];
static atomic_int audio_track_count;
static atomic_int audio_track = -1;
/* The one the session decodes; the session thread's own. */
static int played_audio_track = -1;
static atomic_int duration_ms;
#define MAX_CHAPTERS 64
static int chapter_ms[MAX_CHAPTERS];
static atomic_int chapter_count;
static const char *window_id = "";
static atomic_uint transcode_sequence;

static pthread_t session_thread;
static bool session_running;

/* ------------------------------------------------------------------ state */

static void set_state(jf_player_state value)
{
    atomic_store(&playback_state, (int)value);
    jf_window_wake();
}

static void set_error(const char *message)
{
    snprintf(error_text, sizeof(error_text), "%s", message);
    fprintf(stderr, "Jellyfin player error: %s\n", message);
    set_state(JF_FAILED);
}

jf_player_state jf_player_state_get(void) { return (jf_player_state)atomic_load(&playback_state); }
const char *jf_player_error(void) { return error_text; }
/* The item's own clock, but a seek's target from its request until playback
 * gets there, so the bar neither jumps back while the seek lands nor while the
 * keyframe before the target plays. */
int jf_player_position(void) {
  if (atomic_load(&seek_pending))
    return seek_to_ms;
  int held = atomic_load(&held_position);
  const int now = jf_player_subtitle_ms();
  if (held >= 0) {
    if (now < held)
      return held;
    atomic_compare_exchange_strong(&held_position, &held, -1);
  }
  return now >= 0 ? now : atomic_load(&position_ms);
}
bool jf_player_embedded(void) { return false; }
#ifdef JF_HOST_VIDEO
bool jf_player_needs_frame(void) { return smp_host_frame_due(); }
void jf_player_render(uint32_t width, uint32_t height) {
  smp_host_render(width, height);
}
#else
bool jf_player_needs_frame(void) { return false; } /* Starfish presents independently. */
void jf_player_render(uint32_t width, uint32_t height) { (void)width; (void)height; }
#endif

/* Monotonic milliseconds, matching the subtitle log's `t=`. */
static long long timing_ms(void) { return (long long)(jf_now_ns() / 1000000); }

static bool flowing(void)
{
    return atomic_load(&running) && atomic_load(&segment_flowing);
}

static int ns_to_ms(int64_t ns) { return (int)(ns / 1000000LL); }

/* A pacing sleep a stop can cut short. */
static void sleep_paced(int64_t ns)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(ns / 1000000000LL);
    deadline.tv_nsec += (long)(ns % 1000000000LL);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&interrupt_mutex);
    while (flowing()) {
        if (pthread_cond_timedwait(&interrupt_cond, &interrupt_mutex, &deadline) != 0)
            break;
    }
    pthread_mutex_unlock(&interrupt_mutex);
}

static void sleep_ns(int64_t ns)
{
    struct timespec nap = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    nanosleep(&nap, NULL);
}

/* ------------------------------------------------- pipeline, under one lock */

static bool pipeline_play(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    bool ok = smp_play();
    if (ok) {
        char rate[64];
        if (smp_payload_play_rate(rate, sizeof(rate), PLAYBACK_RATE_MILLIS) &&
            !smp_set_play_rate(rate))
            fprintf(stderr, "SMP SetPlayRate failed\n");
    }
    pthread_mutex_unlock(&pipeline_mutex);
    return ok;
}

static bool pipeline_pause(void)
{
    pthread_mutex_lock(&pipeline_mutex);
    bool ok = smp_pause();
    pthread_mutex_unlock(&pipeline_mutex);
    return ok;
}

static bool pipeline_flush(int64_t offset_ms)
{
    char payload[64];
    if (!smp_payload_flush(payload, sizeof(payload), offset_ms))
        return false;
    pthread_mutex_lock(&pipeline_mutex);
    bool ok = smp_flush(payload);
    pthread_mutex_unlock(&pipeline_mutex);
    return ok;
}

static bool pipeline_begin_segment(int64_t pts_ns)
{
    pthread_mutex_lock(&pipeline_mutex);
    bool ok = jf_starfish_begin_segment(pts_ns);
    pthread_mutex_unlock(&pipeline_mutex);
    return ok;
}

typedef enum { FEED_OK, FEED_BUFFER_FULL, FEED_FAILED } feed_status;

static char feed_error[160];

static feed_status pipeline_feed_video(const void *bytes, size_t size, int64_t pts)
{
    char payload[160];
    char status[128];
    if (!smp_payload_feed(payload, sizeof(payload), bytes, size, pts, SMP_ES_VIDEO))
        return FEED_FAILED;
    pthread_mutex_lock(&pipeline_mutex);
    bool called = smp_feed(payload, status, sizeof(status));
    pthread_mutex_unlock(&pipeline_mutex);
    if (!called) {
        snprintf(feed_error, sizeof(feed_error), "%s", smp_shim_error());
        return FEED_FAILED;
    }
    if (strstr(status, "Ok") != NULL)
        return FEED_OK;
    if (strstr(status, "BufferFull") != NULL)
        return FEED_BUFFER_FULL;
    snprintf(feed_error, sizeof(feed_error), "%s", status);
    return FEED_FAILED;
}

/* Feed one access unit, waiting out backpressure. A full pipeline answers BufferFull and
 * keeps nothing, so the same buffer has to be offered again. */
static bool feed_retrying(const void *bytes, size_t size, int64_t pts)
{
    unsigned stalled = 0;
    while (flowing()) {
        switch (pipeline_feed_video(bytes, size, pts)) {
        case FEED_OK:
            return true;
        case FEED_FAILED:
            fprintf(stderr, "video feed rejected at pts=%dms: %s\n", ns_to_ms(pts), feed_error);
            return false;
        case FEED_BUFFER_FULL:
            /* While paused the pipeline consumes nothing, so waiting it out is the whole
             * point; while playing, backpressure this long is something wedged. */
            stalled = atomic_load(&paused) ? 0 : stalled + 1;
            if (stalled > 2000) /* 10s */
                return false;
            sleep_paced(5 * 1000000LL);
            break;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ events */

static void on_event(int type, int64_t num, const char *str)
{
    /* Function-scope, because an integer error event has no text of its own and the
     * message built for it has to outlive the case that builds it. */
    char detail[128];
    const char *text = (smp_event_has_text(type) && str != NULL) ? str : "";
    switch (type) {
    case SMP_FRAMEREADY:
        /* queryPosition in the load payload makes this the presentation timestamp of the
         * frame just displayed - the app's entire notion of where playback is. */
        jf_clock_sample(num, jf_now_ns());
        atomic_store(&position_ms, atomic_load(&stream_base_ms) + ns_to_ms(num));
        {
            const unsigned seen = atomic_fetch_add(&frame_ready_count, 1);
            if (seen == 0 || seen % 300 == 0)
                fprintf(stderr, "Starfish FRAME_READY #%u pts=%.3fs\n", seen + 1,
                        (double)num / 1e9);
        }
        return;
    case SMP_LOADCOMPLETED:
        atomic_store(&load_complete, true);
        break;
    case SMP_UNLOADCOMPLETED:
        atomic_store(&load_complete, false);
        atomic_store(&pipeline_playing, false);
        jf_clock_set_running(false);
        break;
    case SMP_PLAYING:
        atomic_store(&pipeline_playing, true);
        jf_clock_set_running(true);
        break;
    case SMP_PAUSED:
        atomic_store(&pipeline_playing, false);
        jf_clock_set_running(false);
        break;
    case SMP_INT_ERROR:
    case SMP_STR_ERROR:
        if (text[0] == '\0') {
            snprintf(detail, sizeof(detail), "Starfish pipeline error %lld", (long long)num);
            text = detail;
        }
        set_error(text);
        atomic_store(&running, false);
        break;
    default:
        break;
    }
    fprintf(stderr, "SMP event: type=%d desc=%s value=%lld text=%s\n", type,
            smp_event_name(type), (long long)num, text);
}

/* ------------------------------------------------------------- segment life */

static void interrupt_segment_locked(void)
{
    atomic_store(&segment_flowing, false);
    jf_queue_close(&video_queue);
    jf_queue_close(&audio_queue);
    jf_audio_interrupt();
    pthread_mutex_lock(&interrupt_mutex);
    pthread_cond_broadcast(&interrupt_cond);
    pthread_mutex_unlock(&interrupt_mutex);
}

static void interrupt_segment(void)
{
    pthread_mutex_lock(&segment_mutex);
    interrupt_segment_locked();
    pthread_mutex_unlock(&segment_mutex);
}

static void reset_segment_timeline(void)
{
    atomic_store(&play_requested, false);
    atomic_store(&pipeline_playing, false);
    atomic_store(&fed_video_ms, INT32_MIN);
    atomic_store(&first_video_feed, true);
    atomic_store(&warned_no_clock, false);
    jf_clock_reset();
}

/* Play once video has been fed past the anchor. Audio no longer gates this: it is not in
 * the pipeline, and its own writer waits for the first frame report before placing
 * anything, so there is nothing to preroll here. */
static void maybe_start_pipeline(void)
{
    if (atomic_load(&paused) || atomic_load(&play_requested))
        return;
    if (atomic_load(&fed_video_ms) < 0)
        return;
    bool expected = false;
    if (!atomic_compare_exchange_strong(&play_requested, &expected, true))
        return;
    fprintf(stderr, "Starfish preroll ready: video=%dms; Play\n", atomic_load(&fed_video_ms));
    if (!pipeline_play()) {
        atomic_store(&play_requested, false);
        set_error("Starfish Play failed after preroll");
        atomic_store(&running, false);
    }
}

/* Container PTS are rebased to one segment timeline before either feed thread sees them.
 * The reader sets the origin because it sees packets in container order, so the first one
 * is the earliest; two feed threads racing for it would send the loser negative, and libpf
 * reads a negative pts as an enormous unsigned one. */
static int64_t clock_pts_origin;
static atomic_bool clock_origin_ready;

/* Pace one lane against the pipeline's clock. Before the first frame report, zero is the
 * segment anchor and no host-time guess becomes part of the media timeline. */
static int64_t pace(int64_t packet_pts, int64_t feed_ahead_ns)
{
    int64_t pts = packet_pts - clock_pts_origin;
    if (pts < 0)
        pts = 0;
    if (!atomic_load(&play_requested))
        return pts;
    while (flowing()) {
        const int64_t clock = jf_clock_pts();
        if (clock == JF_CLOCK_NONE) {
            /* No usable clock: feed on the pipeline's own backpressure instead. Feed
             * answers BufferFull and feed_retrying waits that out, which bounds the
             * read-ahead without needing to know where playback is. Pacing against a
             * clock that is merely absent would stall the stream outright - which is
             * exactly what a missing FRAMEREADY used to do here. */
            if (!atomic_exchange(&warned_no_clock, true))
                fprintf(stderr, "Starfish: no pipeline clock yet; feeding on backpressure\n");
            break;
        }
        const int64_t limit = clock + feed_ahead_ns;
        if (pts <= limit)
            break;
        const int64_t wait = pts - limit;
        sleep_paced(wait < 20 * 1000000LL ? wait : 20 * 1000000LL);
    }
    return pts;
}

/* ------------------------------------------------------------------ threads */

#define CLOCK_POLL_PERIOD_NS (20 * 1000000LL)
/* A query this slow says nothing useful about *when* the reported frame was on screen,
 * and a wrong host anchor goes straight into the audio placement. */
#define CLOCK_SLOW_QUERY_NS (50 * 1000000LL)
#define CLOCK_REPORT_PERIOD_NS (1000 * 1000000LL)

/* Samples the pipeline's presentation clock, which is the whole of what the rest of
 * playback synchronises against: the video feed paces from it and every chunk of audio is
 * placed on it. It is also where the periodic diagnostic line comes from - a segment that
 * stalls should say so rather than going quiet. */
static void *clock_thread(void *unused)
{
    (void)unused;
    int64_t last_report = 0;
    while (flowing()) {
        if (atomic_load(&pipeline_playing)) {
            pthread_mutex_lock(&pipeline_mutex);
            const int64_t before = jf_now_ns();
            const int64_t pts = smp_get_current_playtime();
            const int64_t after = jf_now_ns();
            pthread_mutex_unlock(&pipeline_mutex);

            if (pts >= 0 && after - before <= CLOCK_SLOW_QUERY_NS) {
                /* Halfway between the two reads: the frame was on screen somewhere in
                 * there, and the midpoint is the least wrong guess available. */
                jf_clock_sample(pts, before + (after - before) / 2);
                atomic_store(&position_ms, atomic_load(&stream_base_ms) + ns_to_ms(pts));
            } else if (pts < 0 && after - last_report >= CLOCK_REPORT_PERIOD_NS) {
                fprintf(stderr, "Starfish clock: getCurrentPlaytime unavailable\n");
            }

            if (after - last_report >= CLOCK_REPORT_PERIOD_NS) {
                last_report = after;
                const int64_t projected = jf_clock_pts();
                const int fed = atomic_load(&fed_video_ms);
                fprintf(stderr,
                        "Starfish clock: pts=%.3fs projected=%.3fs query=%.1fms fed_video=%dms "
                        "lead=%dms frames=%u\n",
                        (double)pts / 1e9,
                        projected == JF_CLOCK_NONE ? -1.0 : (double)projected / 1e9,
                        (double)(after - before) / 1e6, fed,
                        (projected == JF_CLOCK_NONE || fed < 0) ? -1 : fed - ns_to_ms(projected),
                        atomic_load(&frame_ready_count));
            }
        }
        sleep_paced(CLOCK_POLL_PERIOD_NS);
    }
    return NULL;
}

static void *audio_thread(void *unused)
{
    (void)unused;
    jf_chunk chunk;
    while (jf_queue_pop(&audio_queue, &chunk)) {
        if (flowing()) {
            const int64_t pts = pace(chunk.pts, AUDIO_FEED_AHEAD_NS);
            if (!jf_audio_write(chunk.bytes, (size_t)chunk.size, pts)) {
                free(chunk.bytes);
                break;
            }
        }
        free(chunk.bytes);
        if (!flowing())
            break;
    }
    return NULL;
}

/* The server's tracks, then any in the container it does not list; appended,
 * so an index the UI already holds stays put. The server's embedded tracks are
 * the container's in the same order, which is how they pair up: its indices
 * count files beside the video first.
 * ponytail: pairs with the container's decodable subtitle streams only, so an
 * undecodable one ahead of the rest would shift them. tracks_lock held. */
static void merge_tracks(void) {
  sub_track_count = !jf_player_subtitles ? 0 : server_track_count;
  memcpy(sub_tracks, server_tracks,
         (size_t)sub_track_count * sizeof(*sub_tracks));
  int paired = 0;
  for (int j = 0; j < sub_track_count; j++) {
    sub_tracks[j].stream = -1;
    if (!sub_tracks[j].external && paired < container_track_count)
      sub_tracks[j].stream = container_tracks[paired++].stream;
  }
  for (int i = paired; jf_player_subtitles && i < container_track_count &&
                       sub_track_count < MAX_SUB_TRACKS;
       i++)
    sub_tracks[sub_track_count++] = container_tracks[i];
  if (selected_track >= sub_track_count)
    selected_track = -1;
}

/* Hand one dialogue line to libass on the item's timeline, which for the
 * container's own tracks is the container's. `user` is the stream. */
static void subtitle_line(void *user, const char *ass_line, int64_t start_ms,
                          int64_t duration_ms) {
  if (atomic_fetch_add(&subtitle_lines_fed, 1) == 0)
    fprintf(stderr, "t=%lld Subtitle line #1 at %lldms for %lldms: %s\n",
            (long long)(jf_now_ns() / 1000000), (long long)start_ms,
            (long long)duration_ms, ass_line);
  jf_subs_feed(*(const int *)user, ass_line, (int)strlen(ass_line), start_ms,
               duration_ms);
}

static void subtitle_picture(void *user, int64_t start_ms, int64_t duration_ms,
                             int x, int y, int w, int h, int stream,
                             int canvas_w, int canvas_h, uint8_t *rgba) {
  (void)user;
  jf_picsubs_feed(start_ms, duration_ms, x, y, w, h, stream, canvas_w, canvas_h,
                  rgba);
}

/* The bitmap track to draw, straight from whoever changed the selection: its
 * pictures are already being decoded, so this needs neither the reader - which
 * sits blocked while paused - nor the loader. tracks_lock held. */
static void select_pictures(void) {
  const jf_player_track *track =
      selected_track >= 0 ? &sub_tracks[selected_track] : NULL;
  jf_picsubs_select(track != NULL && !track->text && track->stream >= 0 &&
                            !atomic_load(&source_transcoded)
                        ? track->stream
                        : -1);
}

/* Act on a track change requested from the UI: decode a text track from the
 * container if it is in there, as the immediate source until the loader has
 * the server's copy. Reader thread only. */
static void apply_subtitle_track(void *demux) {
  pthread_mutex_lock(&tracks_lock);
  const uint64_t wanted = selection;
  const int track = selected_track;
  const jf_player_track chosen =
      track >= 0 ? sub_tracks[track] : (jf_player_track){0};
  pthread_mutex_unlock(&tracks_lock);
  if (wanted == read_selection)
    return;
  read_selection = wanted;
  atomic_store(&read_text, -1);
  if (track < 0 || !chosen.text || chosen.stream < 0 ||
      chosen.stream >= JF_DEMUX_SUB_STREAMS || !text_streams[chosen.stream] ||
      atomic_load(&source_transcoded))
    return;
  /* Reopening only restarts a text decoder, which keeps no state between
   * packets, and is what hands back its header. */
  const char *header = NULL;
  int header_size = 0, canvas_w = 0, canvas_h = 0;
  if (jf_demux_subtitle_open(demux, chosen.stream, &header, &header_size,
                             &canvas_w, &canvas_h) != 1 ||
      !jf_subs_open(wanted, chosen.stream, header, header_size)) {
    fprintf(stderr, "Subtitles: stream %d is not a text track\n",
            chosen.stream);
    return;
  }
  atomic_store(&read_text, chosen.stream);
  /* The UI only polls subtitles while some are open, and may be asleep until
   * its next deadline - seconds off when the controls are up. */
  jf_window_wake();
  fprintf(stderr, "t=%lld Subtitles: decoding stream %d (server's %d)\n",
          (long long)(jf_now_ns() / 1000000), chosen.stream, chosen.index);
}

/* Fetches the server's copy of each selected text track: every line, so a seek
 * or a switch finds the ones already running, and the only source there is
 * for a file beside the video or a transcode. */
static int fetch_cancelled(void *serial) {
  pthread_mutex_lock(&tracks_lock);
  const bool stale = selection != *(const uint64_t *)serial;
  pthread_mutex_unlock(&tracks_lock);
  return stale || !atomic_load(&running);
}

static void *subtitle_loader(void *unused) {
  (void)unused;
  uint64_t done = 0;
  pthread_mutex_lock(&tracks_lock);
  while (atomic_load(&running)) {
    if (selection == done) {
      pthread_cond_wait(&tracks_changed, &tracks_lock);
      continue;
    }
    uint64_t serial = done = selection;
    const int track = selected_track;
    const jf_player_track chosen =
        track >= 0 ? sub_tracks[track] : (jf_player_track){0};
    pthread_mutex_unlock(&tracks_lock);

    if (track < 0 || !chosen.text || chosen.url[0] == '\0') {
      jf_subs_close(serial);
    } else {
      char *data = NULL;
      size_t size = 0;
      const int64_t started = jf_now_ns();
      if (jf_demux_fetch(chosen.url, &data, &size, fetch_cancelled, &serial)) {
        const int64_t fetched = jf_now_ns();
        const bool ok = jf_subs_open_file(serial, data, size);
        jf_window_wake();
        fprintf(
            stderr,
            "t=%lld Subtitles: stream %d from the server, %zu bytes in %d ms, "
            "parsed in %d ms%s\n",
            (long long)(jf_now_ns() / 1000000), chosen.index, size,
            ns_to_ms(fetched - started), ns_to_ms(jf_now_ns() - fetched),
            ok ? "" : " - not used");
      } else if (!fetch_cancelled(&serial)) {
        fprintf(stderr, "Subtitles: the server has no copy of stream %d\n",
                chosen.index);
      }
    }
    pthread_mutex_lock(&tracks_lock);
  }
  pthread_mutex_unlock(&tracks_lock);
  return NULL;
}

/* Demux ahead of playback, decoding audio on the way, until a queue is full. */
static void *reader_thread(void *demux)
{
    uint8_t *packet = NULL;
    int size = 0;
    int stream = 0;
    int64_t pts = 0;
    while (flowing() && jf_demux_next(demux, &packet, &size, &stream, &pts)) {
      apply_subtitle_track(demux);
      if (stream >= 0 && stream < JF_DEMUX_SUB_STREAMS &&
          (bitmap_streams[stream] ||
           (text_streams[stream] &&
            (stream == atomic_load(&read_text) || atomic_load(&previewing))))) {
        /* Straight to the renderer rather than through a queue: the events
         * carry their own times, on the container's timeline. */
        jf_demux_subtitle_decode(demux, subtitle_line, subtitle_picture,
                                 &stream);
        continue;
      }
        if ((stream != read_video && stream != read_audio) || size <= 0)
            continue;
        const uint8_t *bytes = packet;
        const bool is_audio = stream == read_audio;
        if (is_audio) {
            uint8_t *pcm = NULL;
            int pcm_size = 0;
            if (!jf_demux_audio_decode(demux, &pcm, &pcm_size, &pts) || pcm == NULL)
                continue;
            bytes = pcm;
            size = pcm_size;
        }
        if (!atomic_load(&clock_origin_ready)) {
            clock_pts_origin = pts;
            /* A transcode is numbered from where it was cut; the container's
             * own timeline is the item's. */
            atomic_store(&timeline_origin_ms,
                         atomic_load(&source_transcoded)
                             ? (long long)atomic_load(&stream_base_ms)
                             : (long long)(pts / 1000000LL));
            atomic_store(&clock_origin_ready, true);
        }
        /* Both buffers belong to the demuxer and die on the next read. */
        uint8_t *copy = malloc((size_t)size);
        if (copy == NULL)
            break;
        memcpy(copy, bytes, (size_t)size);
        jf_chunk chunk = {copy, size, pts};
        if (!jf_queue_push(is_audio ? &audio_queue : &video_queue, &chunk))
            break;
    }
    jf_queue_close(&video_queue);
    jf_queue_close(&audio_queue);
    return NULL;
}

/* One stretch of uninterrupted playback: demux ahead, feed both lanes, and return when
 * the stream ends, playback stops, or a seek parks the segment. */
static void run_segment(void *demux)
{
    pthread_t reader;
    if (pthread_create(&reader, NULL, reader_thread, demux) != 0) {
        set_error("could not start the demux thread");
        atomic_store(&running, false);
        return;
    }
    /* Audio gets its own thread: on a shared one, a chunk of audio due later holds up
     * every video frame queued behind it. */
    pthread_t audio;
    bool have_audio = read_audio >= 0 && pthread_create(&audio, NULL, audio_thread, NULL) == 0;
    pthread_t clock;
    const bool have_clock = pthread_create(&clock, NULL, clock_thread, NULL) == 0;

    jf_chunk chunk;
    while (jf_queue_pop(&video_queue, &chunk)) {
        if (!flowing()) {
            free(chunk.bytes);
            break;
        }
        const int64_t pts = pace(chunk.pts, VIDEO_FEED_AHEAD_NS);
        const bool fed = feed_retrying(chunk.bytes, (size_t)chunk.size, pts);
        if (atomic_exchange(&first_video_feed, false))
          fprintf(stderr,
                  "t=%lld Starfish first video feed: raw=%.3fs pts=%.3fs "
                  "bytes=%d\n",
                  timing_ms(), (double)chunk.pts / 1e9, (double)pts / 1e9,
                  chunk.size);
        free(chunk.bytes);
        if (!fed) {
            if (flowing())
                set_error(feed_error);
            break;
        }
        const int ms = ns_to_ms(pts);
        int seen = atomic_load(&fed_video_ms);
        while (ms > seen && !atomic_compare_exchange_weak(&fed_video_ms, &seen, ms))
            ;
        maybe_start_pipeline();
    }

    /* Clearing this first is what lets both threads fall out of their queues; joining
     * before it would wait forever. */
    interrupt_segment();
    pthread_join(reader, NULL);
    if (have_audio)
        pthread_join(audio, NULL);
    if (have_clock)
        pthread_join(clock, NULL);
}

/* ------------------------------------------------------------------- source */

/* Jellyfin keys a running transcode by PlaySessionId. Seeking needs a new job; reusing
 * the id simply reconnects to the first one, whose output starts at zero regardless of
 * StartTimeTicks. */
static void play_session_id(char *out, size_t out_len)
{
    const uint64_t stamp = (uint64_t)jf_now_ns();
    const unsigned sequence = atomic_fetch_add(&transcode_sequence, 1);
    snprintf(out, out_len, "%08x-%04x-4%03x-8%03x-%012llx",
             (unsigned)(stamp & 0xffffffffu), (unsigned)((stamp >> 32) & 0xffffu),
             (unsigned)((stamp >> 48) & 0x0fffu), sequence & 0x0fffu,
             (unsigned long long)((stamp ^ ((uint64_t)sequence << 32)) & 0x0000ffffffffffffULL));
}

/* The transcode's audio, by the server's number for the chosen stream. The
 * server numbers files beside the video first; the subtitle list says how many.
 * ponytail: an audio file beside the video would shift it too, and is not
 * counted - PlaybackInfo's audio streams would say. */
static const char *audio_parameter(void) {
  static char parameter[40];
  parameter[0] = '\0';
  if (played_audio_track < 0)
    return parameter;
  int external = 0;
  pthread_mutex_lock(&tracks_lock);
  for (int i = 0; i < server_track_count; i++)
    external += server_tracks[i].external;
  pthread_mutex_unlock(&tracks_lock);
  snprintf(parameter, sizeof(parameter), "&AudioStreamIndex=%d",
           audio_tracks[played_audio_track].stream + external);
  return parameter;
}

static bool open_audio_stream(void *demux, int index)
{
    int rate = 0;
    if (index < 0 || !jf_demux_audio_open(demux, index, &rate) || rate <= 0)
        return false;
    audio_rate = rate;
    return jf_audio_open(rate, 2);
}

/* Ask the server for the same stream from `target_ms` on. Jellyfin's transcoding endpoint
 * takes the offset as StartTimeTicks - a tick is 100 ns, so a millisecond is ten thousand
 * of them - and what comes back is a fresh stream numbered from zero. */
static bool reopen_at(void *demux, int target_ms, bool transcoded)
{
    char url[MAX_URI + 96];
    char id[40];
    play_session_id(id, sizeof(id));
    int written;
    if (transcoded)
        /* Use the route's canonical Pascal-case spelling. ASP.NET's current binder is
         * case-insensitive, but older Jellyfin servers route the lower-case spelling
         * through the opaque stream-options map instead of binding it to
         * VideoRequestDto.StartTimeTicks. */
        written = snprintf(
            url, sizeof(url), "%s&PlaySessionId=%s&StartTimeTicks=%lld%s",
            fallback, id, (long long)target_ms * 10000LL, audio_parameter());
    else
        written = snprintf(url, sizeof(url), "%s&StartTimeTicks=%lld", uri,
                           (long long)target_ms * 10000LL);
    if (written <= 0 || (size_t)written >= sizeof(url))
        return false;
    if (strlen(url) >= sizeof(uri))
        return false; /* the next seek would have to rebuild it from a truncated copy */
    if (!jf_demux_reopen(demux, url))
        return false;
    if (transcoded)
        memcpy(uri, url, strlen(url) + 1);

    /* A re-cut stream is a new stream, and nothing promises it numbers its tracks the way
     * the last one did. */
    const bool had_audio = read_audio >= 0;
    const int wanted_audio = !transcoded && played_audio_track >= 0
                                 ? audio_tracks[played_audio_track].stream
                                 : -1;
    read_video = -1;
    read_audio = -1;
    const int count = jf_demux_stream_count(demux);
    for (int i = 0; i < count; i++) {
        int kind = 0, codec = 0, w = 0, h = 0;
        if (!jf_demux_stream(demux, i, &kind, &codec, &w, &h))
            continue;
        if (read_video < 0 && kind == 0 && codec != 0)
            read_video = i;
        if (kind == 1 && had_audio && (read_audio < 0 || i == wanted_audio))
          read_audio = i;
    }
    if (read_video < 0)
        return false;
    /* The subtitle decoders went with the old source. */
    for (int i = 0; i < JF_DEMUX_SUB_STREAMS; i++) {
      const char *header = NULL;
      int header_size = 0, canvas_w = 0, canvas_h = 0;
      if (bitmap_streams[i] || text_streams[i])
        jf_demux_subtitle_open(demux, i, &header, &header_size, &canvas_w,
                               &canvas_h);
    }
    jf_demux_video_open(demux, read_video);
    /* The decoder went with the old source. */
    if (read_audio >= 0 && !open_audio_stream(demux, read_audio))
        read_audio = -1;
    return true;
}

/* In-place seek: keep the pipeline loaded and re-anchor it. Runs between segments, so the
 * reader and both feed threads are already joined and the queues and clock are ours. */
static void seek_to(void *demux, int target_ms, bool transcoded)
{
  /* The source timestamps are rebased to zero for every segment. Nothing fed
   * since the segment began - a resume, straight after the load - means
   * nothing to flush, and a flush that early is worse than none: an H.265
   * pipeline still connecting its sink refuses it and then rejects every
   * feed. */
  if (atomic_load(&fed_video_ms) >= 0) {
    pipeline_pause();
    if (!pipeline_flush(0))
      fprintf(stderr, "SMP flush refused\n");
    if (!pipeline_begin_segment(0))
      fprintf(stderr, "SMP segment restart refused: %s\n",
              jf_starfish_segment_error());
  }
    jf_audio_flush();
    const int wanted_audio = atomic_load(&audio_track);
    const bool switch_audio = wanted_audio >= 0 &&
                              wanted_audio != played_audio_track &&
                              read_audio >= 0;
    if (switch_audio)
      played_audio_track = wanted_audio;

    /* A live transcode has no byte ranges. av_seek_frame nevertheless reports success for
     * it, but positions FFmpeg at byte zero; always ask Jellyfin to create a new segment
     * at the desired time instead. Static originals seek locally, falling back to a
     * re-open only when that fails. */
    const bool moved = transcoded
                           ? reopen_at(demux, target_ms, true)
                           : (jf_demux_seek(demux, (int64_t)target_ms * 1000000LL) != 0 ||
                              reopen_at(demux, target_ms, false));
    fprintf(stderr, "Jellyfin seek: target=%dms source=%s moved=%d\n", target_ms,
            transcoded ? "transcode" : "static", moved);
    if (!moved) {
        set_error("Could not seek this Jellyfin stream");
        atomic_store(&running, false);
        return;
    }
    /* A reopen chose the stream itself; a seek in place keeps the decoder. The
     * output format is fixed, so ALSA stays as it is. */
    int rate = 0;
    if (switch_audio && !transcoded &&
        read_audio != audio_tracks[wanted_audio].stream &&
        jf_demux_audio_open(demux, audio_tracks[wanted_audio].stream, &rate))
      read_audio = audio_tracks[wanted_audio].stream;
    atomic_store(&stream_base_ms, target_ms);
    /* The first packet after the seek re-anchors the timeline. */
    atomic_store(&clock_origin_ready, false);
    reset_segment_timeline();
    atomic_store(&position_ms, target_ms);
}

/* --------------------------------------------------------------- the session */

static void *session(void *unused)
{
    (void)unused;
    void *demux = NULL;
    pthread_t loader;
    bool have_loader = false;

    if (!smp_open()) {
        set_error(smp_shim_error());
        goto done;
    }
    fprintf(stderr, "t=%lld session: opening the source\n", timing_ms());
    demux = jf_demux_open(uri);
    fprintf(stderr, "t=%lld session: source open\n", timing_ms());
    if (demux == NULL) {
        set_error("FFmpeg could not open the Jellyfin stream");
        goto done;
    }

    /* From the original, which a transcode would hide: its length, chapters and
     * every audio stream. */
    atomic_store(&duration_ms, jf_demux_duration_ms(demux));
    atomic_store(&chapter_count,
                 jf_demux_chapters(demux, chapter_ms, MAX_CHAPTERS));
    int audio_count = 0;
    for (int i = 0;
         i < jf_demux_stream_count(demux) && audio_count < MAX_AUDIO_TRACKS;
         i++) {
      int kind = 0, candidate = 0, w = 0, h = 0;
      if (!jf_demux_stream(demux, i, &kind, &candidate, &w, &h) || kind != 1)
        continue;
      audio_tracks[audio_count].stream = i;
      jf_demux_stream_name(demux, i, audio_tracks[audio_count].name,
                           (int)sizeof(audio_tracks[audio_count].name));
      audio_count++;
    }
    played_audio_track = audio_count > 0 && jf_player_audio ? 0 : -1;
    atomic_store(&audio_track, played_audio_track);
    atomic_store(&audio_track_count, played_audio_track >= 0 ? audio_count : 0);

    /* Before anything is read: a 10-bit or above-High source will never decode, so swap to
     * the transcode URL while the demuxer is still fresh and let the discovery below run
     * against what the server sends instead. */
    bool transcoded = false;
    if (jf_demux_video_unsupported(demux) && fallback[0] != '\0') {
        fprintf(stderr, "Jellyfin: source is beyond the decoder, transcoding\n");
        char id[40];
        play_session_id(id, sizeof(id));
        /* Straight into `uri`: it is where the transcode source lives from here on, and
         * a URL that does not fit must not be half-written into it. */
        const int written = snprintf(uri, sizeof(uri), "%s&PlaySessionId=%s&StartTimeTicks=0",
                                     fallback, id);
        if (written <= 0 || (size_t)written >= sizeof(uri)) {
            set_error("The server transcode URL is too long");
            goto done;
        }
        if (!jf_demux_reopen(demux, uri)) {
            set_error("The server would not transcode this item");
            goto done;
        }
        transcoded = true;
    }

    int video_stream = -1, audio_stream = -1, codec = 0, width = 0, height = 0;
    int subtitle_streams = 0;
    const int count = jf_demux_stream_count(demux);
    for (int i = 0; i < count; i++) {
        int kind = 0, candidate = 0, w = 0, h = 0;
        if (!jf_demux_stream(demux, i, &kind, &candidate, &w, &h))
            continue;
        fprintf(stderr, "Jellyfin stream %d: type=%d codec=%d %dx%d\n", i, kind, candidate, w, h);
        if (video_stream < 0 && kind == 0 && candidate != 0) {
            video_stream = i;
            codec = candidate;
            width = w;
            height = h;
        }
        if (audio_stream < 0 && kind == 1)
            audio_stream = i;
        if (kind != 3) /* AVMEDIA_TYPE_SUBTITLE */
          continue;
        subtitle_streams++;
        const char *header = NULL;
        int header_size = 0, canvas_w = 0, canvas_h = 0;
        const int decodable = jf_demux_subtitle_open(
            demux, i, &header, &header_size, &canvas_w, &canvas_h);
        /* Every decoder stays open for the whole session. */
        if (decodable == 2 && i < JF_DEMUX_SUB_STREAMS)
          bitmap_streams[i] = true;
        else if (decodable == 1 && i < JF_DEMUX_SUB_STREAMS)
          text_streams[i] = true;
        else
          jf_demux_subtitle_stop(demux, i);
        if (decodable != 0 && container_track_count < MAX_SUB_TRACKS) {
          jf_player_track *track = &container_tracks[container_track_count++];
          *track = (jf_player_track){i, false, decodable == 1, "", "", i};
          jf_demux_stream_name(demux, i, track->name, (int)sizeof(track->name));
        }
    }
    atomic_store(&source_transcoded, transcoded);
    pthread_mutex_lock(&tracks_lock);
    merge_tracks();
    pthread_mutex_unlock(&tracks_lock);
    /* Typeset tracks name the fonts the release attached; without them every
     * style falls back to the one default face, at its own metrics. */
    const long long fonts_from = timing_ms();
    int font_count = 0;
    for (int i = 0; subtitle_streams > 0 && jf_player_subtitles && i < count;
         i++) {
      const char *name = NULL;
      const uint8_t *data = NULL;
      int size = 0;
      if (jf_demux_font(demux, i, &name, &data, &size)) {
        jf_subs_add_font(name, data, size);
        font_count++;
      }
    }
    if (font_count > 0)
      fprintf(
          stderr,
          "t=%lld Jellyfin subtitles: %d attached font(s) added in %lld ms\n",
          timing_ms(), font_count, timing_ms() - fonts_from);
    if (!jf_player_audio)
      audio_stream = -1;
    if (video_stream < 0 || width <= 0 || height <= 0) {
        set_error("No DirectMedia-compatible video stream");
        goto done;
    }
    video_width = width;
    video_height = height;
    if (audio_stream >= 0 && !open_audio_stream(demux, audio_stream)) {
        fprintf(stderr, "Jellyfin: no usable audio (%s), playing video only\n", jf_audio_error());
        audio_stream = -1;
    }
    read_video = video_stream;
    read_audio = audio_stream;
    jf_demux_video_open(demux, video_stream);

    int fps_num = 0, fps_den = 0;
    jf_demux_video_fps(demux, video_stream, &fps_num, &fps_den);
    video_frame_ms =
        fps_num > 0 && fps_den > 0 ? 1000.0 * fps_den / fps_num : 0;
    jf_subs_geometry((int)frame_width, (int)frame_height, video_width,
                     video_height, video_frame_ms);
    jf_picsubs_open((int)frame_width, (int)frame_height, video_width,
                    video_height);
    pthread_mutex_lock(&tracks_lock);
    select_pictures();
    pthread_mutex_unlock(&tracks_lock);

    static const char *const codec_names[] = {"", "H264", "H265", "VP9", "AV1"};
    smp_video_params params = {APP_ID, window_id, codec_names[codec], width, height,
                               fps_num, fps_den};
    char payload[4096];
    if (!smp_payload_load(payload, sizeof(payload), &params)) {
        set_error("The Starfish load payload does not fit");
        goto done;
    }
    fprintf(stderr, "SMP Load payload: %s\n", payload);

    atomic_store(&load_complete, false);
    atomic_store(&pipeline_playing, false);
    atomic_store(&frame_ready_count, 0);
    if (!smp_notify_foreground()) {
        fprintf(stderr, "Starfish notifyForeground failed: %s\n", smp_shim_error());
    }
    if (!smp_load(payload, on_event)) {
        set_error("Starfish rejected the load payload");
        goto done;
    }

    /* Load returning only means the request was accepted. Feeding and Play belong after
     * the asynchronous LOADCOMPLETED transition. */
    while (atomic_load(&running) && !atomic_load(&load_complete))
        sleep_ns(10 * 1000000LL);
    if (!atomic_load(&running))
        goto done;

    /* The load payload describes the timeline, but libpf does not activate it until
     * CustomPipeline receives a segment event. */
    if (!pipeline_begin_segment(0)) {
        fprintf(stderr, "Starfish segment setup failed: %s\n", jf_starfish_segment_error());
        set_error("Starfish could not establish the initial media segment");
        goto done;
    }
    fprintf(stderr, "t=%lld Starfish segment established at 0ns\n",
            timing_ms());
    reset_segment_timeline();
    set_state(JF_PLAYING);
    have_loader = pthread_create(&loader, NULL, subtitle_loader, NULL) == 0;

    while (atomic_load(&running)) {
        pthread_mutex_lock(&segment_mutex);
        if (!atomic_load(&running)) {
            pthread_mutex_unlock(&segment_mutex);
            break;
        }
        /* Serialize new seek requests with segment startup. A request arriving during a
         * slow reopen stays pending for the next iteration. */
        if (atomic_exchange(&seek_pending, false)) {
            const int target = seek_to_ms;
            /* The reader is joined, so nothing reads the old segment's clock
             * after this: the position holds at the target until the new one
             * has its own. */
            atomic_store(&clock_origin_ready, false);
            pthread_mutex_unlock(&segment_mutex);
            seek_to(demux, target, transcoded);
            continue;
        }
        jf_queue_reset(&video_queue);
        jf_queue_reset(&audio_queue);
        jf_audio_flush();
        atomic_store(&segment_flowing, true);
        pthread_mutex_unlock(&segment_mutex);

        run_segment(demux);
        if (!atomic_load(&seek_pending))
            break;
    }

done:
    atomic_store(&running, false);
    atomic_store(&segment_flowing, false);
    if (have_loader) {
      pthread_mutex_lock(&tracks_lock);
      pthread_cond_broadcast(&tracks_changed);
      pthread_mutex_unlock(&tracks_lock);
      pthread_join(loader, NULL);
    }
    jf_audio_close();
    jf_subs_release();
    jf_picsubs_close();
    if (demux != NULL)
        jf_demux_close(demux);
    smp_unload();
    smp_close();
    if (jf_player_state_get() != JF_FAILED)
        set_state(JF_IDLE);
    return NULL;
}

/* -------------------------------------------------------------------- public */

bool jf_player_play(const char *stream_uri, const char *transcode_uri,
                    uint32_t width, uint32_t height, int start_position_ms)
{
    if (atomic_load(&running))
        return false;
    /* The previous session unloads the pipeline on its way out, and that must land before
     * the next Load. */
    if (session_running) {
        pthread_join(session_thread, NULL);
        session_running = false;
    }
    if (strlen(stream_uri) > MAX_URI || strlen(transcode_uri) > MAX_URI) {
        set_error("The Jellyfin stream URL is too long");
        return false;
    }
    error_text[0] = '\0';
    set_state(JF_LOADING);

    frame_width = width;
    frame_height = height;
    pthread_mutex_lock(&tracks_lock);
    sub_track_count = server_track_count = container_track_count = 0;
    selected_track = -1;
    selection++;
    pthread_mutex_unlock(&tracks_lock);
    atomic_store(&subtitle_lines_fed, 0);
    read_selection = 0;
    memset(bitmap_streams, 0, sizeof(bitmap_streams));
    memset(text_streams, 0, sizeof(text_streams));
    atomic_store(&read_text, -1);
    atomic_store(&previewing, false);
    atomic_store(&audio_track_count, 0);
    atomic_store(&audio_track, -1);
    atomic_store(&duration_ms, 0);
    atomic_store(&chapter_count, 0);

    const int rect[4] = {0, 0, (int)width, (int)height};
    window_id = jf_window_export_video(rect, rect);
    if (window_id == NULL) {
        set_error("The compositor would not create a video window");
        return false;
    }

    snprintf(uri, sizeof(uri), "%s", stream_uri);
    snprintf(fallback, sizeof(fallback), "%s", transcode_uri);
    if (!queues_ready) {
        jf_queue_init(&video_queue);
        jf_queue_init(&audio_queue);
        queues_ready = true;
    }
    atomic_store(&clock_origin_ready, false);
    atomic_store(&paused, false);
    seek_to_ms = start_position_ms > 0 ? start_position_ms : 0;
    atomic_store(&seek_pending, seek_to_ms > 0);
    atomic_store(&held_position, seek_to_ms > 0 ? seek_to_ms : -1);
    atomic_store(&position_ms, seek_to_ms);
    atomic_store(&stream_base_ms, seek_to_ms);
    atomic_store(&running, true);
    if (pthread_create(&session_thread, NULL, session, NULL) != 0) {
        atomic_store(&running, false);
        set_error("could not start the playback thread");
        return false;
    }
    session_running = true;
    return true;
}

void jf_player_pause(void)
{
    if (atomic_exchange(&paused, true))
        return;
    pipeline_pause();
    jf_audio_pause(true);
}

void jf_player_resume(void)
{
    atomic_store(&paused, false);
    jf_audio_pause(false);
    if (atomic_load(&play_requested))
        pipeline_play();
    else
        maybe_start_pipeline();
}

static void request_seek_locked(int target) {
  const int duration = atomic_load(&duration_ms);
  if (duration > 0 && target > duration - 1000)
    target = duration - 1000;
  seek_to_ms = target < 0 ? 0 : target;
  atomic_store(&held_position, seek_to_ms);
  atomic_store(&seek_pending, true);
  interrupt_segment_locked();
}

void jf_player_seek(int delta_seconds)
{
    pthread_mutex_lock(&segment_mutex);
    /* Presses faster than the seek lands add up rather than repeat. */
    if (atomic_load(&running))
      request_seek_locked(jf_player_position() + delta_seconds * 1000);
    pthread_mutex_unlock(&segment_mutex);
}

void jf_player_seek_to(int target_ms) {
  pthread_mutex_lock(&segment_mutex);
  if (atomic_load(&running))
    request_seek_locked(target_ms);
  pthread_mutex_unlock(&segment_mutex);
}

int jf_player_duration(void) { return atomic_load(&duration_ms); }

int jf_player_chapters(const int **starts_ms) {
  *starts_ms = chapter_ms;
  return atomic_load(&chapter_count);
}

int jf_player_audio_count(void) { return atomic_load(&audio_track_count); }

const char *jf_player_audio_name(int track) {
  return track >= 0 && track < atomic_load(&audio_track_count)
             ? audio_tracks[track].name
             : "";
}

int jf_player_audio_current(void) { return atomic_load(&audio_track); }

void jf_player_audio_select(int track) {
  if (track < 0 || track >= atomic_load(&audio_track_count) ||
      atomic_exchange(&audio_track, track) == track)
    return;
  pthread_mutex_lock(&segment_mutex);
  if (atomic_load(&running))
    request_seek_locked(jf_player_position());
  pthread_mutex_unlock(&segment_mutex);
}

void jf_player_stop(void)
{
    atomic_store(&running, false);
    interrupt_segment();
    set_state(JF_IDLE);
}

void jf_player_subtitle_tracks(const jf_player_track *tracks, int count,
                               int selected) {
  pthread_mutex_lock(&tracks_lock);
  server_track_count = count < MAX_SUB_TRACKS ? count : MAX_SUB_TRACKS;
  memcpy(server_tracks, tracks, (size_t)server_track_count * sizeof(*tracks));
  /* Empty when subtitles are turned off in the settings: the A/B test for
   * playback itself misbehaving. */
  selected_track = selected;
  merge_tracks();
  select_pictures();
  selection++;
  pthread_cond_broadcast(&tracks_changed);
  pthread_mutex_unlock(&tracks_lock);
}

int jf_player_subtitle_count(void) {
  pthread_mutex_lock(&tracks_lock);
  const int count = sub_track_count;
  pthread_mutex_unlock(&tracks_lock);
  return count;
}

/* Unlocked, for the UI to draw from: the server's list is set on the UI
 * thread, and the container's merge only rewrites an entry with the same bytes
 * or appends past the count the UI last saw. */
const char *jf_player_subtitle_name(int track) {
  return track >= 0 && track < sub_track_count ? sub_tracks[track].name : "";
}

int jf_player_subtitle_stream(int track) {
  return track >= 0 && track < sub_track_count ? sub_tracks[track].index : -1;
}

int jf_player_subtitle_current(void) {
  pthread_mutex_lock(&tracks_lock);
  const int track = selected_track;
  pthread_mutex_unlock(&tracks_lock);
  return track;
}

void jf_player_subtitle_select(int track) {
  pthread_mutex_lock(&tracks_lock);
  const int chosen = track >= 0 && track < sub_track_count ? track : -1;
  if (chosen != selected_track) {
    selected_track = chosen;
    select_pictures();
    selection++;
    pthread_cond_broadcast(&tracks_changed);
  }
  pthread_mutex_unlock(&tracks_lock);
  /* Nothing else to do: the reader and the loader pick it up. */
}

void jf_player_subtitle_preview(bool on) { atomic_store(&previewing, on); }

int jf_player_subtitle_ms(void) {
  const int64_t pts = jf_clock_pts();
  if (pts == JF_CLOCK_NONE || !atomic_load(&clock_origin_ready))
    return -1;
  return (int)(atomic_load(&timeline_origin_ms) + pts / 1000000LL);
}

void jf_player_deinit(void)
{
    jf_player_stop();
    if (session_running) {
        pthread_join(session_thread, NULL);
        session_running = false;
    }
    if (queues_ready) {
        jf_queue_destroy(&video_queue);
        jf_queue_destroy(&audio_queue);
        queues_ready = false;
    }
}
