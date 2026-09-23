/* Container demux and audio decode. Implemented in demux.c against the bundled FFmpeg. */
#pragma once

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

/* Subtitles. Text tracks only: a bitmap track (PGS, VobSub) has no ASS form and
 * is reported as unusable by jf_demux_subtitle_open. `header` is the decoder's
 * generated ASS script header - the real one for an ASS track, a synthesised
 * default for SRT and the rest - and stays valid until the track is reopened or
 * the handle closed. */
int jf_demux_subtitle_open(void *demux, int index, const char **header,
                           int *header_size);
void jf_demux_subtitle_stop(void *demux);
/* A font attachment's file name and bytes, valid until the handle is closed or
 * reopened. 0 for any other stream. */
int jf_demux_font(void *demux, int index, const char **name,
                  const uint8_t **data, int *size);
/* Human-readable name for a stream: its title or language, and the codec. */
int jf_demux_stream_name(void *demux, int index, char *out, int out_len);

/* Decode the subtitle packet jf_demux_next last returned. One packet can carry
 * several lines, so each is handed to `sink` with its own start and duration in
 * milliseconds on the container's timeline. */
typedef void (*jf_subtitle_sink)(void *user, const char *ass_line,
                                 int64_t start_ms, int64_t duration_ms);
int jf_demux_subtitle_decode(void *demux, jf_subtitle_sink sink, void *user);
int jf_demux_reopen(void *demux, const char *url);
