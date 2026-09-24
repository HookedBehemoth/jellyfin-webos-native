/*
 * A C façade over StarfishMediaAPIs, the C++ class exported by libplayerAPIs.
 *
 * Everything else in this program is C. The shim behind this header (smp_shim.cpp) is the
 * only C++ translation unit, and it contains no logic: each function is a try/catch around
 * one call into the library. The JSON payloads it takes are built in C, in smp_payload.c,
 * so they can be unit-tested on the host without libplayerAPIs.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Event ids, from PF_EVENT_T in starfish-media-pipeline/StarfishMediaAPIs.h. */
enum {
    SMP_FRAMEREADY = 0x00,
    SMP_STR_VIDEO_INFO = 0x04,
    SMP_STR_AUDIO_INFO = 0x07,
    SMP_STR_SOURCE_INFO = 0x0b,
    SMP_INT_ERROR = 0x12,
    SMP_STR_ERROR = 0x13,
    SMP_LOADCOMPLETED = 0x16,
    SMP_UNLOADCOMPLETED = 0x17,
    SMP_SEEKDONE = 0x19,
    SMP_PLAYING = 0x1a,
    SMP_PAUSED = 0x1b,
    SMP_ENDOFSTREAM = 0x1c,
    SMP_INT_BUFFERLOW = 0x2c,
    SMP_STR_BUFFERFULL = 0x2d,
    SMP_STR_BUFFERLOW = 0x2e,
    SMP_DROPPED_FRAME = 0x30,
};

/* Which elementary stream a feed carries. Audio is no longer one of them - it leaves the
 * pipeline entirely and goes to ALSA; see audio_alsa.h. */
enum { SMP_ES_VIDEO = 1, SMP_ES_AUDIO = 2 };

/* Invoked on the pipeline's own thread, not the caller's. */
typedef void (smp_event_fn)(int type, int64_t num_value, const char *str_value);

/* One session's worth of pipeline. The object is *not* reusable across load/unload: its
 * uMediaServer context is fixed at construction, and a second load comes up with its
 * resources granted but its sinks never registered - no sourceInfo, no picture. So
 * smp_close() really destroys it and smp_open() builds a new one. */
bool smp_open(void);
void smp_close(void);
bool smp_is_open(void);

bool smp_load(const char *payload, smp_event_fn *callback);
bool smp_unload(void);
bool smp_play(void);
bool smp_pause(void);
bool smp_push_eos(void);
bool smp_notify_foreground(void);

/* Seek takes milliseconds as a decimal string; flush and set_play_rate take JSON. Build
 * all three with the smp_payload_* helpers. */
bool smp_seek(const char *millis);
bool smp_flush(const char *payload);
bool smp_set_play_rate(const char *payload);

/* Feed answers with a status *string*, not a boolean. `status` receives it so the caller
 * can classify it (and report it) without the shim knowing what the words mean. */
bool smp_feed(const char *payload, char *status, size_t status_len);

/* The pipeline's own presentation clock in nanoseconds, negative when unavailable.
 * `option.queryPosition` in the load payload is what keeps it maintained. */
int64_t smp_get_current_playtime(void);

/* The public `player` member of the instance, for the libpf segment bridge. NULL before
 * a successful load. */
void *smp_player(void);

/* Set when a shim call threw; empty otherwise. */
const char *smp_shim_error(void);

#ifdef JF_HOST_VIDEO
/* The desktop stand-in (smp_host.c) decodes the picture itself: whether a
 * frame is waiting to be shown, and drawing the one the clock has reached into
 * the current GL framebuffer. */
bool smp_host_frame_due(void);
void smp_host_render(uint32_t width, uint32_t height);
#endif

#ifdef __cplusplus
}
#endif
