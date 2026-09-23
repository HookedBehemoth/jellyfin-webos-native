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
/* Which subtitle stream the reader has open, and which the UI has asked for.
 * The reader owns the demuxer, so a selection made on the render thread is a
 * request it picks up between packets rather than a call into FFmpeg from
 * somewhere else. */
static int read_subs = -1;
static atomic_int wanted_subs = -1;
/* Filled once, before the session reports JF_PLAYING, and read by the UI from
 * then on. */
#define MAX_SUB_TRACKS 16
static struct {
  int stream;
  char name[96];
} sub_tracks[MAX_SUB_TRACKS];
static atomic_int sub_track_count;
static atomic_uint subtitle_lines_fed;
static uint32_t frame_width = 1920, frame_height = 1080;
/* The picture subtitles are laid out over, which the TV fits into the frame. */
static int video_width = 1920, video_height = 1080;
static int audio_rate = 48000;
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
int jf_player_position(void) { return atomic_load(&position_ms); }
bool jf_player_embedded(void) { return false; }
bool jf_player_needs_frame(void) { return false; } /* Starfish presents independently. */
void jf_player_render(uint32_t width, uint32_t height) { (void)width; (void)height; }

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

/* Hand one dialogue line to libass on the segment timeline the clock and both
 * feed threads use, so a subtitle lands with the frame and the sound it belongs
 * to. */
static void subtitle_line(void *user, const char *ass_line, int64_t start_ms,
                          int64_t duration_ms) {
  (void)user;
  int64_t start = start_ms - clock_pts_origin / 1000000LL;
  if (start < 0) {
    /* A line that straddles the start of a re-cut stream still has the rest of
     * its time to run. */
    duration_ms += start;
    start = 0;
  }
  if (duration_ms <= 0)
    return;
  const unsigned seen = atomic_fetch_add(&subtitle_lines_fed, 1);
  if (seen == 0 || seen % 100 == 0)
    fprintf(stderr, "Subtitle line #%u at %lldms for %lldms: %s\n", seen + 1,
            (long long)start, (long long)duration_ms, ass_line);
  jf_subs_feed(ass_line, (int)strlen(ass_line), start, duration_ms);
}

/* Act on a track change requested from the UI. Reader thread only. */
static void apply_subtitle_track(void *demux) {
  const int wanted = atomic_load(&wanted_subs);
  if (wanted == read_subs)
    return;
  read_subs = wanted;
  jf_subs_close();
  jf_demux_subtitle_stop(demux);
  if (wanted < 0) {
    fprintf(stderr, "Subtitles off\n");
    return;
  }
  const char *header = NULL;
  int header_size = 0;
  if (!jf_demux_subtitle_open(demux, wanted, &header, &header_size) ||
      !jf_subs_open(header, header_size, (int)frame_width, (int)frame_height,
                    video_width, video_height)) {
    fprintf(stderr, "Subtitles: stream %d could not be opened\n", wanted);
    jf_demux_subtitle_stop(demux);
    read_subs = -1;
    atomic_store(&wanted_subs, -1);
    return;
  }
  fprintf(stderr, "Subtitles on: stream %d, %d bytes of header\n", wanted,
          header_size);
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
      if (stream == read_subs && read_subs >= 0) {
        /* Straight to libass rather than through a queue: the events carry
         * their own timeline and the renderer picks from it by time.
         *
         * Not before the origin is known, though. It is set below, by the first
         * video or audio packet, and rebasing against a zero origin would place
         * the line at its absolute container time - half an hour out, on a
         * resume. A subtitle ahead of the segment's first frame is already
         * partly in the past; dropping it costs one line at the start of a
         * segment. */
        if (atomic_load(&clock_origin_ready))
          jf_demux_subtitle_decode(demux, subtitle_line, NULL);
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
            fprintf(stderr, "Starfish first video feed: raw=%.3fs pts=%.3fs bytes=%d\n",
                    (double)chunk.pts / 1e9, (double)pts / 1e9, chunk.size);
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
        written = snprintf(url, sizeof(url), "%s&PlaySessionId=%s&StartTimeTicks=%lld",
                           fallback, id, (long long)target_ms * 10000LL);
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
    read_video = -1;
    read_audio = -1;
    const int count = jf_demux_stream_count(demux);
    for (int i = 0; i < count; i++) {
        int kind = 0, codec = 0, w = 0, h = 0;
        if (!jf_demux_stream(demux, i, &kind, &codec, &w, &h))
            continue;
        if (read_video < 0 && kind == 0 && codec != 0)
            read_video = i;
        if (read_audio < 0 && kind == 1 && had_audio)
            read_audio = i;
    }
    if (read_video < 0)
        return false;
    /* The subtitle track is re-opened by the reader, which sees the request as
     * a change because the decoder went with the old source. */
    read_subs = -1;
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
    /* The source timestamps are rebased to zero for every segment. */
    pipeline_pause();
    if (!pipeline_flush(0))
        fprintf(stderr, "SMP flush refused\n");
    if (!pipeline_begin_segment(0))
        fprintf(stderr, "SMP segment restart refused: %s\n", jf_starfish_segment_error());
    jf_audio_flush();

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
    jf_subs_flush();
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

    if (!smp_open()) {
        set_error(smp_shim_error());
        goto done;
    }
    demux = jf_demux_open(uri);
    if (demux == NULL) {
        set_error("FFmpeg could not open the Jellyfin stream");
        goto done;
    }

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
    int subtitle_count = 0;
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
        /* AVMEDIA_TYPE_SUBTITLE. Bitmap tracks are filtered out by the open
         * below, but listing them and failing on selection is worse than not
         * offering them. */
        if (kind == 3 && subtitle_count < MAX_SUB_TRACKS) {
          const char *header = NULL;
          int header_size = 0;
          if (jf_demux_subtitle_open(demux, i, &header, &header_size)) {
            sub_tracks[subtitle_count].stream = i;
            jf_demux_stream_name(demux, i, sub_tracks[subtitle_count].name,
                                 (int)sizeof(sub_tracks[subtitle_count].name));
            subtitle_count++;
          }
        }
    }
    jf_demux_subtitle_stop(demux);
    atomic_store(&sub_track_count, subtitle_count);
    for (int i = 0; i < subtitle_count; i++)
      fprintf(stderr, "Jellyfin subtitle track %d: stream %d, %s\n", i,
              sub_tracks[i].stream, sub_tracks[i].name);
    /* Off keeps the reader off the subtitle path entirely, which is the A/B
     * test when playback itself misbehaves. */
    if (!jf_player_subtitles) {
      atomic_store(&sub_track_count, 0);
      subtitle_count = 0;
    }
    /* On by default when the container has one, which is what every other
     * player does; the Subtitles button cycles through the rest and back to
     * off. */
    if (subtitle_count > 0)
      atomic_store(&wanted_subs, sub_tracks[0].stream);
    /* Typeset tracks name the fonts the release attached; without them every
     * style falls back to the one default face, at its own metrics. */
    int font_count = 0;
    for (int i = 0; subtitle_count > 0 && i < count; i++) {
      const char *name = NULL;
      const uint8_t *data = NULL;
      int size = 0;
      if (jf_demux_font(demux, i, &name, &data, &size)) {
        jf_subs_add_font(name, data, size);
        font_count++;
      }
    }
    if (font_count > 0)
      fprintf(stderr, "Jellyfin subtitles: %d attached font(s)\n", font_count);
    fprintf(stderr, "Jellyfin subtitles: %d text track(s)\n", subtitle_count);
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
    fprintf(stderr, "Starfish segment established at 0ns\n");
    reset_segment_timeline();
    set_state(JF_PLAYING);

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
    jf_audio_close();
    jf_subs_release();
    atomic_store(&sub_track_count, 0);
    atomic_store(&wanted_subs, -1);
    read_subs = -1;
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
    atomic_store(&sub_track_count, 0);
    atomic_store(&wanted_subs, -1);
    atomic_store(&subtitle_lines_fed, 0);
    read_subs = -1;

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

void jf_player_seek(int delta_seconds)
{
    pthread_mutex_lock(&segment_mutex);
    if (atomic_load(&running)) {
        int target = atomic_load(&position_ms) + delta_seconds * 1000;
        seek_to_ms = target < 0 ? 0 : target;
        atomic_store(&seek_pending, true);
        interrupt_segment_locked();
    }
    pthread_mutex_unlock(&segment_mutex);
}

void jf_player_stop(void)
{
    atomic_store(&running, false);
    interrupt_segment();
    set_state(JF_IDLE);
}

int jf_player_subtitle_count(void) { return atomic_load(&sub_track_count); }

const char *jf_player_subtitle_name(int track) {
  if (track < 0 || track >= atomic_load(&sub_track_count))
    return "";
  return sub_tracks[track].name;
}

int jf_player_subtitle_current(void) {
  const int stream = atomic_load(&wanted_subs);
  const int count = atomic_load(&sub_track_count);
  for (int i = 0; i < count; i++)
    if (sub_tracks[i].stream == stream)
      return i;
  return -1;
}

void jf_player_subtitle_select(int track) {
  const int count = atomic_load(&sub_track_count);
  atomic_store(&wanted_subs,
               track >= 0 && track < count ? sub_tracks[track].stream : -1);
  /* Nothing else to do: the reader applies it between packets, and the overlay
   * is cleared by the close that follows. */
}

int jf_player_media_ms(void) {
  const int64_t pts = jf_clock_pts();
  return pts == JF_CLOCK_NONE ? -1 : (int)(pts / 1000000LL);
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
