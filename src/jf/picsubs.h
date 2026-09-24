/*
 * Bitmap subtitles: PGS from Blu-rays and VobSub from DVDs.
 *
 * These arrive as pictures, not text, so there is nothing for libass to do.
 * The demuxer decodes each one to a premultiplied RGBA image at the track's own
 * resolution; this holds the few that are due and hands back the one on screen,
 * placed over the video's picture in the overlay. A picture shows until its end
 * time or until the next one replaces it, which is how PGS clears the screen.
 *
 * Every bitmap track in the container is decoded as it is read, and only the
 * selected one drawn, so a switch shows the picture already due rather than
 * waiting out the seconds the demuxer has read ahead. After a seek, a picture
 * that began before the point landed on is not known; the next one is.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
  float x, y, w, h;         /* where it goes in the overlay */
  int texture_w, texture_h; /* the pixels, `texture_w * texture_h * 4` bytes */
  const uint8_t *rgba;      /* premultiplied; NULL when nothing is on screen */
} jf_picsubs_image;

/* The overlay is width x height, with the video fitted into it. */
void jf_picsubs_open(int width, int height, int video_width, int video_height);
void jf_picsubs_close(void);
/* Which stream's pictures to draw, -1 for none. Takes effect at once. */
void jf_picsubs_select(int stream);
/* Open with a track selected. */
bool jf_picsubs_ready(void);

/* One picture of `stream` on the item's timeline, taking `rgba` (malloc'd),
 * placed on a canvas_w x canvas_h grid. A negative duration lasts until the
 * stream's next picture; NULL `rgba` is an empty one, which clears the
 * screen. */
void jf_picsubs_feed(int64_t start_ms, int64_t duration_ms, int x, int y, int w,
                     int h, int stream, int canvas_w, int canvas_h,
                     uint8_t *rgba);

/* The picture at `media_ms`; true when it differs from the last call's. The
 * image lives until the next call. */
bool jf_picsubs_frame(int64_t media_ms, jf_picsubs_image *out);
