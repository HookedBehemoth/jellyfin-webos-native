/*
 * Subtitle rendering with libass.
 *
 * The TV decodes and presents video on its own plane, so nothing can be
 * composited into the picture. Subtitles are drawn on the graphics plane
 * instead, as one RGBA image the UI uploads and draws over the video hole -
 * which is also why this renders at the window's size rather than the video's.
 *
 * Timing is the pipeline's, not the wall clock's: events are fed on the same
 * rebased segment timeline the video feed and the ALSA writer are paced against
 * (see clock.h), and the frame asked for is the projected presentation
 * timestamp. Subtitles therefore move with the picture across pauses, seeks and
 * buffering, including the audio they belong to.
 *
 * libass keeps mutable state in the track and the renderer, and the feed comes
 * from the demux thread while the compositing comes from the render thread, so
 * one lock covers the lot.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A .ttf to use instead of the system candidates; NULL for those. */
extern const char *jf_subs_font;

/* The composited overlay: `rgba` is premultiplied alpha, `w * h * 4` bytes, and
 * lives until the next jf_subs_frame. An empty image has w == 0. */
typedef struct {
  int x, y, w, h;
  const uint8_t *rgba;
} jf_subs_image;

/* A font the container carries, which is what the styles of a typeset ASS track
 * name. Copied; they stay loaded for every track until jf_subs_release. */
void jf_subs_add_font(const char *name, const uint8_t *data, int size);

/* `header` is the decoder's ASS script header - the styles the events refer to.
 * The script is laid out over the video's picture, fitted into the width x
 * height overlay, as a player compositing into the video would. */
bool jf_subs_open(const char *header, int header_size, int width, int height,
                  int video_width, int video_height);
void jf_subs_close(void);
/* Closes the track and drops the fonts: the end of a playback. */
void jf_subs_release(void);
bool jf_subs_ready(void);

/* One dialogue line in the ASS event format ("ReadOrder,Layer,Style,..."), on
 * the segment timeline. Called from the demux thread, ahead of playback. */
void jf_subs_feed(const char *line, int length, int64_t start_ms,
                  int64_t duration_ms);
/* A seek invalidates every event still held. */
void jf_subs_flush(void);

/* Compose what is on screen at `media_ms`. True when the image changed since
 * the last call, which is the only time the caller has to re-upload it. */
bool jf_subs_frame(int64_t media_ms, jf_subs_image *out);

/* What the last jf_subs_frame cost, in thread CPU time. */
typedef struct {
  double render_ms, composite_ms;
  unsigned runs;
} jf_subs_cost;
jf_subs_cost jf_subs_last_cost(void);
