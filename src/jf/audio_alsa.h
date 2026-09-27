/*
 * Decoded PCM straight to ALSA, instead of through Starfish's PCM sink.
 *
 * The pipeline's audio path wanted the sound cut into 1024-sample access units,
 * each with a timestamp derived from a sample cursor we maintained ourselves,
 * with silence written into every gap so a missing packet did not pull the rest
 * of the track early - and it still only accepted a short list of sample rates,
 * treating anything else as "bypass". All of that existed to compensate for a
 * sink that would not say where it was.
 *
 * ALSA will: snd_pcm_delay() reports exactly how much sound is still queued, so
 * the presentation time of the next sample written is known rather than
 * tracked. Video stays the master clock (clock.h); this module pulls audio onto
 * it by padding silence when audio would otherwise arrive early and dropping
 * samples when it would arrive late.
 *
 * Format is fixed by the decoder: interleaved signed 16-bit stereo. See
 * demux.c.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool jf_audio_open(int rate, int channels);
void jf_audio_close(void);

/* Writes one decoded chunk, placing it against the video clock. Blocks while
 * ALSA has no room. False when the device failed or jf_audio_interrupt() was
 * called. */
bool jf_audio_write(const void *pcm, size_t bytes, int64_t pts_ns);

/* Wake a blocked writer so a segment can end. Cleared by jf_audio_flush(). */
void jf_audio_interrupt(void);

void jf_audio_pause(bool paused);

/* Seek or discontinuity: drop everything queued and re-anchor on the next
 * chunk. */
void jf_audio_flush(void);

/* What the last failure was, for the log. */
const char *jf_audio_error(void);
