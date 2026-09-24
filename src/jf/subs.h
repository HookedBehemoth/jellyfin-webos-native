/*
 * Subtitle rendering with libass.
 *
 * The TV decodes and presents video on its own plane, so nothing can be
 * composited into the picture. Subtitles are drawn on the graphics plane
 * instead, as one RGBA image the UI uploads and draws over the video hole -
 * which is also why this renders at the window's size rather than the video's.
 *
 * Timing is the item's: lines are placed on the item's own timeline, which the
 * player derives from the pipeline's projected presentation clock, so they move
 * with the picture across pauses, seeks and buffering.
 *
 * Every call that changes the track carries the player's selection serial.
 * Anything older than the selection already held is ignored, so the demuxer
 * and the loader of the server's copy can both act on one selection without
 * racing each other or a newer one.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
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

/* The overlay is width x height; the script is laid out over the video's
 * picture fitted into it, as a player compositing into the video would. Frames
 * are rendered one per `frame_ms` of the video; 0 means 24 fps. Applies from
 * the next open. */
void jf_subs_geometry(int width, int height, int video_width, int video_height,
                      double frame_ms);

/* Starts rendering selection `serial` with `header`'s styles and the container
 * stream's lines fed so far, then takes that stream's feeds (-1: none). True
 * when that selection is rendering, whoever started it. */
bool jf_subs_open(uint64_t serial, int stream, const char *header,
                  int header_size);
/* The whole script for `serial`, as the server extracts it: every line, so
 * seeking and switching find the ones already running. Takes `data` (malloc'd,
 * NUL-terminated) either way. Replaces the demuxer's lines if it got there
 * first, and makes further feeds redundant. Parsing is slow for a typeset
 * track, so not on a thread that feeds playback. */
bool jf_subs_open_file(uint64_t serial, char *data, size_t size);
/* Stops rendering: subtitles off, or a track only the server can supply. */
void jf_subs_close(uint64_t serial);
/* Closes the track and drops the fonts: the end of a playback. */
void jf_subs_release(void);
bool jf_subs_ready(void);

/* One dialogue line of container stream `stream` (below 64) in the ASS event
 * format ("ReadOrder,Layer,Style,..."), on the item's timeline. Any text stream
 * can be fed, selected or not, and a window of each is kept, so a switch shows
 * the line already running. A line seen before - read again after a seek back
 * - is kept once. Called from the demux thread, ahead of playback. */
void jf_subs_feed(int stream, const char *line, int length, int64_t start_ms,
                  int64_t duration_ms);

/* The newest rendered frame at or before `media_ms`, which also moves the
 * rendering along. True when the image changed since the last call, which is
 * the only time the caller has to re-upload it. */
bool jf_subs_frame(int64_t media_ms, jf_subs_image *out);

/* Keeps dialogue above overlay row `y` - over the player's controls, say -
 * and INT_MAX lets it go back down. Only lines the script's styles place at
 * the bottom move; signs positioned over the picture stay where they are. */
void jf_subs_keep_above(int y);

/* What rendering the frame now shown cost its background thread, in CPU time.
 */
typedef struct {
  double render_ms, composite_ms;
  unsigned runs;
} jf_subs_cost;
jf_subs_cost jf_subs_last_cost(void);
