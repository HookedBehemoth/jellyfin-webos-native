// Container demux and audio decode for the Jellyfin player. The app links and ships one
// known FFmpeg build rather than trying to impersonate each TV's FFmpeg ABI.
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "demux.h"

#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libswresample/swresample.h>

#define PCM_SAMPLE_RATE 48000
#define PCM_CHANNELS 2
#define PCM_FRAME_BYTES 4 /* stereo, signed 16-bit */

struct jf_demux {
    AVFormatContext *format;
    AVPacket *packet;
    // The ALSA sink takes interleaved stereo S16LE PCM; see audio_alsa.c.
    AVCodecContext *audio;
    AVFrame *frame;
    SwrContext *swr;
    uint8_t *pcm;
    size_t pcm_cap, pcm_size;
    int audio_input_rate;
    int64_t audio_next_pts;
    // A file container stores H.264/H.265 length-prefixed with the parameter
    // sets off in extradata. The hardware decoder wants Annex-B start codes --
    // without them it reports "Sequence Init Fail" and never starts.
    AVBSFContext *bsf;
    AVPacket *filtered;
    int video_index;
    // Subtitle decoders by stream: the selected text track, and every bitmap
    // track, which are decoded all along so a switch has pictures to show.
    AVCodecContext *subs[JF_DEMUX_SUB_STREAMS];
};

static void audio_close(struct jf_demux *d) {
    swr_free(&d->swr);
    av_frame_free(&d->frame);
    avcodec_free_context(&d->audio);
    d->pcm_size = 0;
    d->audio_input_rate = 0;
    d->audio_next_pts = AV_NOPTS_VALUE;
}

static int resampler_open(struct jf_demux *d) {
    const AVChannelLayout *input_layout = &d->frame->ch_layout;
    AVChannelLayout fallback = {0};
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    if (!av_channel_layout_check(input_layout)) {
        if (av_channel_layout_check(&d->audio->ch_layout)) {
            input_layout = &d->audio->ch_layout;
        } else {
            int channels = input_layout->nb_channels > 0 ? input_layout->nb_channels :
                           d->audio->ch_layout.nb_channels > 0 ?
                           d->audio->ch_layout.nb_channels : PCM_CHANNELS;
            av_channel_layout_default(&fallback, channels);
            input_layout = &fallback;
        }
    }
    d->audio_input_rate = d->frame->sample_rate > 0 ? d->frame->sample_rate :
                          d->audio->sample_rate;
    int error = swr_alloc_set_opts2(&d->swr, &stereo, AV_SAMPLE_FMT_S16,
                                    PCM_SAMPLE_RATE, input_layout, d->frame->format,
                                    d->audio_input_rate, 0, NULL);
    av_channel_layout_uninit(&fallback);
    if (error >= 0) error = swr_init(d->swr);
    return error >= 0;
}

/// Open one audio stream for decode. Output is always 48 kHz interleaved stereo S16LE,
/// which is the one format the ALSA sink is opened with.
int jf_demux_audio_open(void *opaque, int index, int *rate) {
    struct jf_demux *d = opaque;
    if (index < 0 || index >= (int)d->format->nb_streams) return 0;
    audio_close(d);
    AVStream *stream = d->format->streams[index];
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    if (decoder == NULL || (d->audio = avcodec_alloc_context3(decoder)) == NULL)
        return 0;
    if (avcodec_parameters_to_context(d->audio, stream->codecpar) < 0) {
        audio_close(d);
        return 0;
    }
    // The decoder needs the packet timebase to adjust frame PTS when it
    // removes codec padding (e.g. Opus pre-skip). Otherwise the samples are
    // trimmed but still carry the timestamp of the discarded samples.
    d->audio->pkt_timebase = stream->time_base;
    d->audio_next_pts = AV_NOPTS_VALUE;
    if (avcodec_open2(d->audio, decoder, NULL) < 0 ||
        (d->frame = av_frame_alloc()) == NULL) {
        audio_close(d);
        return 0;
    }
    *rate = PCM_SAMPLE_RATE;
    return 1;
}

/// Decode the packet jf_demux_next last returned into interleaved S16LE stereo.
/// pts enters as the packet timestamp and leaves as the first output sample's
/// timestamp, in nanoseconds. The buffer stays valid until the next call.
int jf_demux_audio_decode(void *opaque, uint8_t **out, int *size, int64_t *pts) {
    struct jf_demux *d = opaque;
    if (d->audio == NULL || d->frame == NULL) return 0;
    d->pcm_size = 0;
    int64_t first_pts = AV_NOPTS_VALUE;
    int64_t packet_pts = *pts;
    int error = avcodec_send_packet(d->audio, d->packet);
    if (error < 0) return 0;
    for (;;) {
        error = avcodec_receive_frame(d->audio, d->frame);
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) break;
        if (error < 0) return 0;
        if (d->swr == NULL && !resampler_open(d)) {
            av_frame_unref(d->frame);
            return 0;
        }
        const AVRational ns = {1, 1000000000};
        // Decoded frames may belong to an earlier packet or have leading
        // samples removed. Resampler buffering also belongs in the timestamp.
        int64_t frame_pts = d->frame->best_effort_timestamp != AV_NOPTS_VALUE
            ? av_rescale_q(d->frame->best_effort_timestamp,
                           d->format->streams[d->packet->stream_index]->time_base, ns)
                - swr_get_delay(d->swr, 1000000000)
            : d->audio_next_pts != AV_NOPTS_VALUE ? d->audio_next_pts : packet_pts;
        int64_t delayed = swr_get_delay(d->swr, d->audio_input_rate);
        int64_t output_count = av_rescale_rnd(delayed + d->frame->nb_samples,
                                              PCM_SAMPLE_RATE, d->audio_input_rate,
                                              AV_ROUND_UP);
        if (output_count <= 0 || output_count > INT_MAX ||
            (size_t)output_count > (SIZE_MAX - d->pcm_size) / PCM_FRAME_BYTES) {
            av_frame_unref(d->frame);
            return 0;
        }
        size_t need = d->pcm_size + (size_t)output_count * PCM_FRAME_BYTES;
        if (need > d->pcm_cap) {
            uint8_t *grown = av_realloc(d->pcm, need);
            if (grown == NULL) {
                av_frame_unref(d->frame);
                return 0;
            }
            d->pcm = grown;
            d->pcm_cap = need;
        }
        uint8_t *dst[] = {d->pcm + d->pcm_size};
        int got = swr_convert(d->swr, dst, (int)output_count,
                              (const uint8_t **)d->frame->extended_data,
                              d->frame->nb_samples);
        if (got > 0) {
            if (d->pcm_size == 0) first_pts = frame_pts;
            d->pcm_size += (size_t)got * PCM_FRAME_BYTES;
            d->audio_next_pts = frame_pts +
                                av_rescale_q(got, (AVRational){1, PCM_SAMPLE_RATE}, ns);
        }
        av_frame_unref(d->frame);
    }
    if (d->pcm_size == 0 || d->pcm_size > INT_MAX) return 0;
    *out = d->pcm;
    *size = (int)d->pcm_size;
    *pts = first_pts;
    return 1;
}

static void subtitle_close(struct jf_demux *d) {
  for (int i = 0; i < JF_DEMUX_SUB_STREAMS; i++)
    avcodec_free_context(&d->subs[i]);
}

void jf_demux_subtitle_stop(void *opaque, int index) {
  struct jf_demux *d = opaque;
  if (index < 0)
    subtitle_close(d);
  else if (index < JF_DEMUX_SUB_STREAMS)
    avcodec_free_context(&d->subs[index]);
}

/// Open one subtitle stream for decode. Every text format FFmpeg knows decodes
/// to ASS dialogue lines, which is exactly what ass_process_chunk takes, so
/// SRT, WebVTT, mov_text and ASS itself all arrive here in one shape. Bitmap
/// formats - PGS, VobSub, DVB - decode to paletted pictures instead, on a
/// canvas of the stream's own size.
int jf_demux_subtitle_open(void *opaque, int index, const char **header,
                           int *header_size, int *canvas_w, int *canvas_h) {
  struct jf_demux *d = opaque;
  if (index < 0 || index >= (int)d->format->nb_streams ||
      index >= JF_DEMUX_SUB_STREAMS)
    return 0;
  AVCodecContext **subs = &d->subs[index];
  avcodec_free_context(subs);
  AVStream *stream = d->format->streams[index];
  if (stream->codecpar->codec_type != AVMEDIA_TYPE_SUBTITLE)
    return 0;
  const AVCodecDescriptor *about =
      avcodec_descriptor_get(stream->codecpar->codec_id);
  if (about == NULL ||
      (about->props & (AV_CODEC_PROP_TEXT_SUB | AV_CODEC_PROP_BITMAP_SUB)) == 0)
    return 0;
  const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
  if (decoder == NULL || (*subs = avcodec_alloc_context3(decoder)) == NULL)
    return 0;
  if (avcodec_parameters_to_context(*subs, stream->codecpar) < 0) {
    avcodec_free_context(subs);
    return 0;
  }
  (*subs)->pkt_timebase = stream->time_base;
  if (avcodec_open2(*subs, decoder, NULL) < 0) {
    avcodec_free_context(subs);
    return 0;
  }
  *header = (const char *)(*subs)->subtitle_header;
  *header_size = (*subs)->subtitle_header_size;
  *canvas_w = (*subs)->width;
  *canvas_h = (*subs)->height;
  return (about->props & AV_CODEC_PROP_TEXT_SUB) ? 1 : 2;
}

/// The bitmap rectangles of one subtitle, flattened into a single premultiplied
/// RGBA image over their union. NULL when there is nothing to show.
static uint8_t *flatten(const AVSubtitle *sub, int *x, int *y, int *w, int *h) {
  int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
  for (unsigned i = 0; i < sub->num_rects; i++) {
    const AVSubtitleRect *r = sub->rects[i];
    if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0)
      continue;
    if (r->x < x0)
      x0 = r->x;
    if (r->y < y0)
      y0 = r->y;
    if (r->x + r->w > x1)
      x1 = r->x + r->w;
    if (r->y + r->h > y1)
      y1 = r->y + r->h;
  }
  if (x1 <= x0 || y1 <= y0)
    return NULL;
  const int width = x1 - x0, height = y1 - y0;
  uint8_t *out = calloc((size_t)width * height, 4);
  if (out == NULL)
    return NULL;
  for (unsigned i = 0; i < sub->num_rects; i++) {
    const AVSubtitleRect *r = sub->rects[i];
    if (r->type != SUBTITLE_BITMAP || r->w <= 0 || r->h <= 0)
      continue;
    // data[1] is the palette, native-endian 0xAARRGGBB.
    const uint32_t *palette = (const uint32_t *)r->data[1];
    uint8_t colors[256][4];
    for (int c = 0; c < 256 && c < (r->nb_colors > 0 ? r->nb_colors : 256);
         c++) {
      const uint32_t argb = palette[c];
      const unsigned a = argb >> 24;
      colors[c][0] = (uint8_t)(((argb >> 16) & 0xff) * a / 255);
      colors[c][1] = (uint8_t)(((argb >> 8) & 0xff) * a / 255);
      colors[c][2] = (uint8_t)((argb & 0xff) * a / 255);
      colors[c][3] = (uint8_t)a;
    }
    const int colors_used =
        r->nb_colors > 0 && r->nb_colors < 256 ? r->nb_colors : 256;
    for (int row = 0; row < r->h; row++) {
      const uint8_t *src = r->data[0] + (size_t)row * r->linesize[0];
      uint8_t *dst =
          out + ((size_t)(r->y - y0 + row) * width + (r->x - x0)) * 4;
      for (int col = 0; col < r->w; col++) {
        const int c = src[col] < colors_used ? src[col] : 0;
        memcpy(dst + col * 4, colors[c], 4);
      }
    }
  }
  *x = x0;
  *y = y0;
  *w = width;
  *h = height;
  return out;
}

/// Title, else language, else the stream number; plus the codec, because a
/// library commonly carries the same language as both full subtitles and forced
/// signs.
int jf_demux_font(void *opaque, int index, const char **name,
                  const uint8_t **data, int *size) {
  struct jf_demux *d = opaque;
  if (index < 0 || index >= (int)d->format->nb_streams)
    return 0;
  const AVStream *stream = d->format->streams[index];
  const AVCodecParameters *p = stream->codecpar;
  if (p->codec_type != AVMEDIA_TYPE_ATTACHMENT || p->extradata == NULL ||
      p->extradata_size <= 0)
    return 0;
  /* Matroska keeps the MIME type; FFmpeg maps the font ones to these ids. */
  const AVDictionaryEntry *mime =
      av_dict_get(stream->metadata, "mimetype", NULL, 0);
  const bool font = p->codec_id == AV_CODEC_ID_TTF ||
                    p->codec_id == AV_CODEC_ID_OTF ||
                    (mime != NULL && (strstr(mime->value, "font") != NULL ||
                                      strstr(mime->value, "opentype") != NULL));
  if (!font)
    return 0;
  const AVDictionaryEntry *file =
      av_dict_get(stream->metadata, "filename", NULL, 0);
  *name = file != NULL ? file->value : "attachment";
  *data = p->extradata;
  *size = p->extradata_size;
  return 1;
}

int jf_demux_duration_ms(void *opaque) {
  struct jf_demux *d = opaque;
  return d->format->duration > 0 ? (int)(d->format->duration / 1000) : 0;
}

int jf_demux_chapters(void *opaque, int *starts_ms, int max) {
  struct jf_demux *d = opaque;
  int count = 0;
  for (unsigned i = 0; i < d->format->nb_chapters && count < max; i++) {
    const AVChapter *chapter = d->format->chapters[i];
    starts_ms[count++] = (int)av_rescale_q(chapter->start, chapter->time_base,
                                           (AVRational){1, 1000});
  }
  return count;
}

int jf_demux_stream_name(void *opaque, int index, char *out, int out_len) {
  struct jf_demux *d = opaque;
  if (index < 0 || index >= (int)d->format->nb_streams || out_len <= 0)
    return 0;
  AVStream *stream = d->format->streams[index];
  const AVDictionaryEntry *title =
      av_dict_get(stream->metadata, "title", NULL, 0);
  const AVDictionaryEntry *language =
      av_dict_get(stream->metadata, "language", NULL, 0);
  const AVCodecDescriptor *about =
      avcodec_descriptor_get(stream->codecpar->codec_id);
  char fallback[24];
  const char *label = title != NULL      ? title->value
                      : language != NULL ? language->value
                                         : NULL;
  if (label == NULL) {
    snprintf(fallback, sizeof(fallback), "Track %d", index);
    label = fallback;
  }
  snprintf(out, (size_t)out_len, "%s (%s)", label,
           about != NULL ? about->name : "?");
  return 1;
}

int jf_demux_subtitle_decode(void *opaque, jf_subtitle_sink sink,
                             jf_picture_sink pictures, void *user) {
  struct jf_demux *d = opaque;
  const int index = d->packet->stream_index;
  AVCodecContext *subs = index < JF_DEMUX_SUB_STREAMS ? d->subs[index] : NULL;
  if (subs == NULL)
    return 0;
  AVSubtitle sub;
  int got = 0;
  if (avcodec_decode_subtitle2(subs, &sub, &got, d->packet) < 0 || !got)
    return 0;
  const AVRational ms = {1, 1000};
  AVStream *stream = d->format->streams[d->packet->stream_index];
  const int64_t base = d->packet->pts != AV_NOPTS_VALUE
                           ? av_rescale_q(d->packet->pts, stream->time_base, ms)
                           : 0;
  const int64_t packet_ms =
      d->packet->duration > 0
          ? av_rescale_q(d->packet->duration, stream->time_base, ms)
          : 0;
  // The display window is the decoder's when it has one, the packet's
  // otherwise.
  int64_t duration =
      (int64_t)sub.end_display_time - (int64_t)sub.start_display_time;
  if (sub.end_display_time == UINT32_MAX || duration <= 0)
    duration = packet_ms;
  if (sub.format == 0) {
    // A picture with no end lasts until the next one, which is how PGS clears.
    int x = 0, y = 0, w = 0, h = 0;
    uint8_t *rgba = flatten(&sub, &x, &y, &w, &h);
    // The canvas, not known to PGS until its first display set.
    pictures(user, base + sub.start_display_time, duration > 0 ? duration : -1,
             x, y, w, h, index, subs->width, subs->height, rgba);
    avsubtitle_free(&sub);
    return 1;
  }
  // A line with no end would otherwise never come off.
  if (duration <= 0)
    duration = 5000;
  for (unsigned i = 0; i < sub.num_rects; i++) {
    const AVSubtitleRect *rect = sub.rects[i];
    if (rect->type != SUBTITLE_ASS || rect->ass == NULL)
      continue;
    sink(user, rect->ass, base + sub.start_display_time, duration);
  }
  avsubtitle_free(&sub);
  return 1;
}

/// The whole body at `url`, NUL-terminated, through FFmpeg's own HTTP. For a
/// subtitle file: the player has no other client, and this one already speaks
/// to the server.
int jf_demux_fetch(const char *url, char **data, size_t *size,
                   int (*cancel)(void *), void *opaque) {
  AVIOContext *io = NULL;
  const AVIOInterruptCB interrupt = {cancel, opaque};
  if (avio_open2(&io, url, AVIO_FLAG_READ, cancel != NULL ? &interrupt : NULL,
                 NULL) < 0)
    return 0;
  size_t used = 0, capacity = 1 << 16;
  char *buffer = malloc(capacity + 1);
  int ok = buffer != NULL;
  while (ok) {
    if (used == capacity) {
      char *grown = realloc(buffer, capacity * 2 + 1);
      if (grown == NULL) {
        ok = 0;
        break;
      }
      buffer = grown;
      capacity *= 2;
    }
    const int n =
        avio_read(io, (unsigned char *)buffer + used,
                  (int)(capacity - used < INT_MAX ? capacity - used : INT_MAX));
    if (n == AVERROR_EOF)
      break;
    if (n < 0)
      ok = 0;
    else
      used += (size_t)n;
  }
  avio_closep(&io);
  if (!ok) {
    free(buffer);
    return 0;
  }
  buffer[used] = '\0';
  *data = buffer;
  *size = used;
  return 1;
}

void *jf_demux_open(const char *url) {
    struct jf_demux *d = calloc(1, sizeof(*d));
    if (d == NULL) return NULL;
    d->audio_next_pts = AV_NOPTS_VALUE;
    d->packet = av_packet_alloc();
    if (d->packet == NULL || avformat_open_input(&d->format, url, NULL, NULL) < 0 ||
        avformat_find_stream_info(d->format, NULL) < 0) {
        av_packet_free(&d->packet);
        avformat_close_input(&d->format);
        free(d);
        return NULL;
    }
    return d;
}

void jf_demux_close(void *opaque) {
    struct jf_demux *d = opaque;
    if (d == NULL) return;
    av_bsf_free(&d->bsf);
    av_packet_free(&d->filtered);
    audio_close(d);
    subtitle_close(d);
    av_freep(&d->pcm);
    av_packet_free(&d->packet);
    avformat_close_input(&d->format);
    free(d);
}

int jf_demux_stream_count(void *opaque) { return ((struct jf_demux *)opaque)->format->nb_streams; }

/// Frame rate of a video stream as a rational. The pipeline wants to be told
/// this; left out, it assumes whatever the load payload's maxFrameRate says.
int jf_demux_video_fps(void *opaque, int index, int *num, int *den) {
    struct jf_demux *d = opaque;
    if (index < 0 || index >= (int)d->format->nb_streams) return 0;
    AVStream *st = d->format->streams[index];
    AVRational fps = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (fps.num <= 0 || fps.den <= 0) return 0;
    *num = fps.num;
    *den = fps.den;
    return 1;
}
// Keep FFmpeg's AVCodecID values on the C side. They are not an ABI: enum
// members move whenever FFmpeg inserts new codecs (H.264 is 27 in 4.4 and 172
// in the bundled build). Return the small, stable VideoType values understood
// by player.zig instead.
static int video_type(enum AVCodecID codec) {
    switch (codec) {
        case AV_CODEC_ID_H264: return 1;
        case AV_CODEC_ID_HEVC: return 2;
        case AV_CODEC_ID_VP9:  return 3;
        case AV_CODEC_ID_AV1:  return 4;
        default:               return 0;
    }
}

int jf_demux_stream(void *opaque, int index, int *kind, int *video, int *width, int *height) {
    struct jf_demux *d = opaque;
    if (index < 0 || index >= (int)d->format->nb_streams) return 0;
    AVCodecParameters *p = d->format->streams[index]->codecpar;
    *kind = p->codec_type;
    *video = video_type(p->codec_id);
    *width = p->width;
    *height = p->height;
    return 1;
}
/// 1 when the video in this container is beyond what the TV's decoder takes:
/// more than 8 bits per sample, or H.264 above High profile. Hardware decoders
/// answer either with "Sequence Init Fail" and no picture, so the caller has
/// to ask the server to transcode instead.
int jf_demux_video_unsupported(void *opaque) {
    struct jf_demux *d = opaque;
    for (unsigned i = 0; i < d->format->nb_streams; i++) {
        AVCodecParameters *p = d->format->streams[i]->codecpar;
        if (p->codec_type != AVMEDIA_TYPE_VIDEO) continue;
        if (p->bits_per_raw_sample > 8) return 1;
        return p->codec_id == AV_CODEC_ID_H264 && p->profile > AV_PROFILE_H264_HIGH;
    }
    return 0;
}

/// Route this video stream through a bitstream filter when the container
/// stores it length-prefixed (an AVCC/HVCC extradata block starts with 1).
/// Elementary-stream containers already carry start codes and need none.
int jf_demux_video_open(void *opaque, int index) {
    struct jf_demux *d = opaque;
    d->video_index = index;
    av_bsf_free(&d->bsf);
    if (index < 0 || index >= (int)d->format->nb_streams) return 0;
    AVStream *stream = d->format->streams[index];
    AVCodecParameters *par = stream->codecpar;
    if (par->extradata_size < 1 || par->extradata[0] != 1) return 0;
    const char *name = par->codec_id == AV_CODEC_ID_H264   ? "h264_mp4toannexb"
                       : par->codec_id == AV_CODEC_ID_HEVC ? "hevc_mp4toannexb"
                                                           : 0;
    const AVBitStreamFilter *filter = name ? av_bsf_get_by_name(name) : NULL;
    if (!filter || av_bsf_alloc(filter, &d->bsf) < 0) return 0;
    if (!d->filtered) d->filtered = av_packet_alloc();
    if (!d->filtered || avcodec_parameters_copy(d->bsf->par_in, par) < 0) {
        av_bsf_free(&d->bsf);
        return 0;
    }
    d->bsf->time_base_in = stream->time_base;
    if (av_bsf_init(d->bsf) < 0) {
        av_bsf_free(&d->bsf);
        return 0;
    }
    return 1;
}

/// Point the handle at a different URL, keeping the handle itself. A server
/// that transcodes on the fly serves no byte ranges, so seeking such a stream
/// means asking the server for it again from a different offset.
int jf_demux_reopen(void *opaque, const char *url) {
    struct jf_demux *d = opaque;
    // Before the context goes: the packet still references buffers it owns.
    av_packet_unref(d->packet);
    if (d->filtered != NULL) av_packet_unref(d->filtered);
    av_bsf_free(&d->bsf);
    audio_close(d);
    subtitle_close(d);
    avformat_close_input(&d->format);
    if (avformat_open_input(&d->format, url, NULL, NULL) < 0) return 0;
    return avformat_find_stream_info(d->format, NULL) >= 0;
}

/// Move the source to the keyframe at or before position_ns. Stream index -1
/// means the timestamp is in AV_TIME_BASE units, i.e. microseconds.
int jf_demux_seek(void *opaque, int64_t position_ns) {
    struct jf_demux *d = opaque;
    if (av_seek_frame(d->format, -1, position_ns / 1000, AVSEEK_FLAG_BACKWARD) < 0)
        return 0;
    if (d->audio) avcodec_flush_buffers(d->audio);
    for (int i = 0; i < JF_DEMUX_SUB_STREAMS; i++)
      if (d->subs[i])
        avcodec_flush_buffers(d->subs[i]);
    if (d->bsf) av_bsf_flush(d->bsf);
    // Discard any buffered conversion samples along with the decoder state.
    if (d->swr) {
        swr_close(d->swr);
        if (swr_init(d->swr) < 0) return 0;
    }
    d->pcm_size = 0;
    d->audio_next_pts = AV_NOPTS_VALUE;
    return 1;
}

int jf_demux_next(void *opaque, uint8_t **data, int *size, int *stream, int64_t *pts) {
    struct jf_demux *d = opaque;
    for (;;) {
        av_packet_unref(d->packet);
        if (av_read_frame(d->format, d->packet) < 0) return 0;
        AVPacket *out = d->packet;
        if (d->bsf && d->packet->stream_index == d->video_index) {
            // send_packet takes the input; receive can want another one first.
            if (av_bsf_send_packet(d->bsf, d->packet) < 0) continue;
            av_packet_unref(d->filtered);
            if (av_bsf_receive_packet(d->bsf, d->filtered) < 0) continue;
            d->filtered->stream_index = d->video_index;
            out = d->filtered;
        }
        *data = out->data; *size = out->size; *stream = out->stream_index;
        int64_t timestamp = out->pts != AV_NOPTS_VALUE ? out->pts : out->dts;
        *pts = timestamp == AV_NOPTS_VALUE ? 0 :
               av_rescale_q(timestamp, d->format->streams[*stream]->time_base,
                            (AVRational){1, 1000000000});
        return 1;
    }
}
