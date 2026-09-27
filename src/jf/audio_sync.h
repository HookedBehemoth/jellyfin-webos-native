/*
 * Where a chunk of decoded audio belongs relative to what ALSA has already
 * queued.
 *
 * Split out of audio_alsa.c so the arithmetic - the part that is easy to get
 * subtly wrong and impossible to eyeball on a TV - can be tested on a machine
 * with no sound card.
 */
#pragma once

#include <stdint.h>

/* Inside this, the chunk is simply written: correcting for a couple of
 * milliseconds costs more in audible artefacts than the error is worth. */
#define JF_AUDIO_SYNC_THRESHOLD_NS (20 * 1000000LL)
/* Never insert more than this much silence at once. A larger gap converges over
 * the chunks that follow instead, which keeps one bad timestamp from stalling
 * the track. */
#define JF_AUDIO_MAX_PAD_NS (500 * 1000000LL)

/* `due_ns` is the host time the chunk's first sample should be heard (from the
 * video clock); `tail_ns` is when the next sample written would be heard (now +
 * what ALSA still has queued).
 *
 * Returns frames of silence to insert before the chunk (positive), frames to
 * drop from its head (negative - possibly more than the chunk holds, meaning
 * drop all of it), or zero to write it as it is. */
static inline int64_t jf_audio_placement(int64_t due_ns, int64_t tail_ns,
                                         int rate) {
  const int64_t error_ns = due_ns - tail_ns;
  if (error_ns > -JF_AUDIO_SYNC_THRESHOLD_NS &&
      error_ns < JF_AUDIO_SYNC_THRESHOLD_NS)
    return 0;
  const int64_t capped =
      error_ns > JF_AUDIO_MAX_PAD_NS ? JF_AUDIO_MAX_PAD_NS : error_ns;
  return capped * rate / 1000000000LL;
}
