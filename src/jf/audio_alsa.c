#include "audio_alsa.h"

#include "audio_sync.h"
#include "clock.h"
#include "player.h"
#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Enough to ride out demux jitter without the queue becoming the thing that decides
 * latency; the sync correction below keeps it honest either way. */
#define BUFFER_LATENCY_US 200000
#define SILENCE_FRAMES 1024

static snd_pcm_t *pcm;
static int pcm_rate = 48000;
static int pcm_channels = 2;
static size_t frame_bytes = 4;
static char error_text[160];
static atomic_bool interrupted;

static void set_error(const char *what)
{
    snprintf(error_text, sizeof(error_text), "%s", what);
    fprintf(stderr, "Jellyfin audio: %s\n", what);
}

const char *jf_audio_error(void) { return error_text; }

void jf_audio_interrupt(void) { atomic_store(&interrupted, true); }

bool jf_audio_open(int rate, int channels)
{
    jf_audio_close();
    /* webOS routes app audio through its own daemon, so which device that ends
     * up being is a property of the TV rather than of this code. `default` is
     * the right first guess; the audio_device preference exists so trying
     * another is not a rebuild. */
    const char *device = jf_player_audio_device;
    if (device == NULL || device[0] == '\0')
        device = "default";

    /* Non-blocking: a device that stops draining - paused, or left behind by a
     * video pipeline that failed - would otherwise hold a write, and every
     * thread joining this one, forever. write_frames waits in slices instead.
     */
    int error =
        snd_pcm_open(&pcm, device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK);
    if (error < 0) {
        char message[160];
        snprintf(message, sizeof(message), "cannot open %s: %s", device, snd_strerror(error));
        set_error(message);
        pcm = NULL;
        return false;
    }
    /* snd_pcm_set_params rather than the hw_params dance: one format, one rate, one
     * access mode, and no reason to negotiate any of them. */
    error = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                               (unsigned)channels, (unsigned)rate, 1, BUFFER_LATENCY_US);
    if (error < 0) {
        char message[160];
        snprintf(message, sizeof(message), "%d Hz %d ch refused: %s", rate, channels,
                 snd_strerror(error));
        set_error(message);
        snd_pcm_close(pcm);
        pcm = NULL;
        return false;
    }
    pcm_rate = rate;
    pcm_channels = channels;
    frame_bytes = (size_t)channels * 2;
    error_text[0] = '\0';
    atomic_store(&interrupted, false);
    fprintf(stderr, "Jellyfin audio: ALSA %s at %d Hz, %d ch\n", device, rate, channels);
    return true;
}

void jf_audio_close(void)
{
    if (pcm == NULL)
        return;
    snd_pcm_drop(pcm);
    snd_pcm_close(pcm);
    pcm = NULL;
}

void jf_audio_flush(void)
{
    atomic_store(&interrupted, false);
    if (pcm == NULL)
        return;
    snd_pcm_drop(pcm);
    if (snd_pcm_prepare(pcm) < 0)
        set_error("could not re-prepare the device after a flush");
}

void jf_audio_pause(bool paused)
{
    if (pcm == NULL)
        return;
    /* Not every device can pause. Dropping instead costs the queued tail, which the
     * resync on resume then covers - a shorter gap than the seek this usually precedes. */
    if (snd_pcm_pause(pcm, paused ? 1 : 0) < 0) {
        if (paused)
            snd_pcm_drop(pcm);
        else if (snd_pcm_prepare(pcm) < 0)
            set_error("could not re-prepare the device after a pause");
    }
}

/* When the next sample written would be heard. */
static int64_t queue_tail_ns(void)
{
    snd_pcm_sframes_t queued = 0;
    if (snd_pcm_delay(pcm, &queued) < 0 || queued < 0)
        queued = 0;
    return jf_now_ns() + (int64_t)queued * 1000000000LL / pcm_rate;
}

static bool recover(int error)
{
    if (snd_pcm_recover(pcm, error, 1) < 0) {
        set_error(snd_strerror(error));
        return false;
    }
    /* Whatever was queued is gone; the next placement measures the empty queue and puts
     * the track back where it belongs. */
    return true;
}

/* Writes whole frames, waiting for room at most 50 ms at a time so an interrupt
 * is seen even when the device has stopped taking any. */
static bool write_frames(const uint8_t *data, snd_pcm_uframes_t frames)
{
    while (frames > 0) {
        if (atomic_load(&interrupted))
            return false;
        snd_pcm_sframes_t written = snd_pcm_writei(pcm, data, frames);
        if (written == -EAGAIN) {
          snd_pcm_wait(pcm, 50);
          continue;
        }
        if (written < 0) {
            if (!recover((int)written))
                return false;
            continue;
        }
        data += (size_t)written * frame_bytes;
        frames -= (snd_pcm_uframes_t)written;
    }
    return true;
}

static bool write_silence(int64_t frames)
{
    static const uint8_t quiet[SILENCE_FRAMES * 8] = {0};
    while (frames > 0) {
        const int64_t run = frames < SILENCE_FRAMES ? frames : SILENCE_FRAMES;
        if (!write_frames(quiet, (snd_pcm_uframes_t)run))
            return false;
        frames -= run;
    }
    return true;
}

/* Blocks until the video clock has something to say, so the first chunk of a segment is
 * placed against a real anchor rather than against whatever the host clock reads. */
static bool wait_for_clock(void)
{
    while (jf_clock_pts() == JF_CLOCK_NONE) {
        if (atomic_load(&interrupted))
            return false;
        struct timespec nap = {0, 2 * 1000000};
        nanosleep(&nap, NULL);
    }
    return true;
}

bool jf_audio_write(const void *data, size_t bytes, int64_t pts_ns)
{
    if (pcm == NULL || bytes < frame_bytes)
        return pcm != NULL;
    if (atomic_load(&interrupted))
        return false;

    const uint8_t *samples = data;
    int64_t frames = (int64_t)(bytes / frame_bytes);

    if (!wait_for_clock())
        return false;
    const int64_t due = jf_clock_host_for(pts_ns);
    if (due == JF_CLOCK_NONE)
        return false;

    /* After a flush the queue is empty and `tail` is simply now, which is what makes the
     * same correction serve both "we drifted" and "we just restarted". */
    const int64_t placement = jf_audio_placement(due, queue_tail_ns(), pcm_rate);
    if (placement > 0) {
        if (!write_silence(placement))
            return false;
    } else if (placement < 0) {
        const int64_t drop = -placement;
        if (drop >= frames)
            return true; /* the whole chunk is already in the past */
        samples += (size_t)drop * frame_bytes;
        frames -= drop;
    }
    return write_frames(samples, (snd_pcm_uframes_t)frames);
}
