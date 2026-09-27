/*
 * The JSON documents the pipeline consumes, written with snprintf.
 *
 * The exact bytes are the interesting part - every key here was read out of a
 * shipped webOS binary or copied from a known-working homebrew player - so they
 * are spelled out rather than assembled by a DOM builder. Kept in C, away from
 * the shim, so the host test binary can check them without libplayerAPIs.
 *
 * There is no audio in any of these payloads. Starfish's PCM sink is not used:
 * audio is decoded to PCM and written to ALSA directly (audio_alsa.h), which is
 * both simpler and the only way to keep a sample-accurate handle on where the
 * sound actually is. The pipeline therefore carries video alone and runs its
 * own clock off the system clock. Its buffering schema still declares both ES
 * lanes: libpf's Starfish load path initializes those controls as a pair even
 * when needAudio is false.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *app_id;
  const char *window_id;
  const char *codec; /* "H264", "H265", "VP9", "AV1" */
  int width;
  int height;
  /* Frame rate as value/scale, the pipeline's own convention
   * (PF_EXT_ES_VIDEO_FRAMERATE_VALUE / _SCALE). Zero leaves the pair out, and
   * the pipeline then believes adaptiveStreaming's maxFrameRate, which is not
   * the content's rate. */
  int fps_num;
  int fps_den;
} smp_video_params;

/* False when the document would not fit, in which case `out` must not be sent:
 * a truncated payload is not valid JSON. */
bool smp_payload_load(char *out, size_t out_len, const smp_video_params *video);
bool smp_payload_feed(char *out, size_t out_len, const void *buffer,
                      size_t size, int64_t pts_ns, int es_data);
/* Drop what the pipeline has buffered and re-anchor it at `offset_ms`.
 *
 * `audioFlush` and `offset` are the only keys StarfishMediaAPIs::flush(const
 * char *) parses. It hands them to CustomPipeline::flush(int, long long), which
 * pushes a real FLUSH_START/FLUSH_STOP pair to the appsrc - the no-argument
 * flush() sends neither and leaves the sink on the pre-seek segment. audioFlush
 * is false because there is no audio appsrc to flush. */
bool smp_payload_flush(char *out, size_t out_len, int64_t offset_ms);
bool smp_payload_play_rate(char *out, size_t out_len, int rate_millis);

/* A human-readable name for an event id, for the log. */
const char *smp_event_name(int type);
/* The callback's third argument is only a C string for string-valued events.
 * For integer events libpf may leave the slot unspecified, so dereferencing it
 * produces garbage or faults while trying to improve diagnostics. */
bool smp_event_has_text(int type);
