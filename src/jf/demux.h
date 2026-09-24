/* Container demux and audio decode. Implemented in demux.c against the bundled FFmpeg. */
#pragma once

#include <stddef.h>
#include <stdint.h>

void *jf_demux_open(const char *url);
void jf_demux_close(void *demux);
int jf_demux_stream_count(void *demux);
int jf_demux_stream(void *demux, int index, int *kind, int *codec, int *width, int *height);
int jf_demux_next(void *demux, uint8_t **data, int *size, int *stream, int64_t *pts);
int jf_demux_video_fps(void *demux, int index, int *num, int *den);
int jf_demux_audio_open(void *demux, int index, int *rate);
int jf_demux_video_open(void *demux, int index);
int jf_demux_video_unsupported(void *demux);
int jf_demux_audio_decode(void *demux, uint8_t **out, int *size, int64_t *pts);
int jf_demux_seek(void *demux, int64_t position_ns);
/* The container's length in milliseconds, 0 when it does not say. */
int jf_demux_duration_ms(void *demux);
/* Up to `max` chapter start times in milliseconds; returns how many. */
int jf_demux_chapters(void *demux, int *starts_ms, int max);

/* Subtitles: 1 for a text track, 2 for a bitmap one (PGS, VobSub), 0 when the
 * stream is neither. `header` is a text track's ASS script header - the real
 * one for an ASS track, a synthesised default for SRT and the rest - valid
 * until the track is reopened or the handle closed. `canvas` is a bitmap
 * track's picture grid, 0 when the stream does not say yet. */
int jf_demux_subtitle_open(void *demux, int index, const char **header,
                           int *header_size, int *canvas_w, int *canvas_h);
/* One stream's decoder, or every one for -1. Several can be open at once. */
void jf_demux_subtitle_stop(void *demux, int index);
/* Streams past this have no subtitle decoder. */
#define JF_DEMUX_SUB_STREAMS 64
/* A font attachment's file name and bytes, valid until the handle is closed or
 * reopened. 0 for any other stream. */
int jf_demux_font(void *demux, int index, const char **name,
                  const uint8_t **data, int *size);
/* Human-readable name for a stream: its title or language, and the codec. */
int jf_demux_stream_name(void *demux, int index, char *out, int out_len);

/* Decode the subtitle packet jf_demux_next last returned, if its stream has a
 * decoder open; 0 otherwise. A text packet can
 * carry several lines, each handed to `sink` with its own start and duration
 * in milliseconds on the container's timeline. A bitmap packet is one picture,
 * premultiplied RGBA handed over to `pictures` (NULL for an empty one), with a
 * negative duration when it lasts until the next. */
typedef void (*jf_subtitle_sink)(void *user, const char *ass_line,
                                 int64_t start_ms, int64_t duration_ms);
typedef void (*jf_picture_sink)(void *user, int64_t start_ms,
                                int64_t duration_ms, int x, int y, int w, int h,
                                int stream, int canvas_w, int canvas_h,
                                uint8_t *rgba);
int jf_demux_subtitle_decode(void *demux, jf_subtitle_sink sink,
                             jf_picture_sink pictures, void *user);
/* Download a whole file, NUL-terminated and malloc'd. `cancel` returning
 * nonzero abandons it, as FFmpeg's interrupt callback. */
int jf_demux_fetch(const char *url, char **data, size_t *size,
                   int (*cancel)(void *), void *opaque);
int jf_demux_reopen(void *demux, const char *url);
