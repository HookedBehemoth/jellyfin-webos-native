/*
 * Who can decode an item's video: the TV itself, the server's hardware, or
 * only the server's CPU. For the details screen to say so before Play does.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The video codecs either side may have hardware for, by Jellyfin's names. */
typedef enum {
  JF_H264,
  JF_HEVC,
  JF_VP9,
  JF_AV1,
  JF_MPEG2,
  JF_VC1,
  JF_VP8,
  JF_MPEG4,
  JF_CODECS,
} jf_codec;

/* JF_CODECS for a name that is none of them. */
jf_codec jf_codec_from_name(const char *name);
/* How a person writes it: "H.264", "HEVC". NULL for JF_CODECS. */
const char *jf_codec_label(jf_codec codec);

/* An item's first video stream, as the server describes it. */
typedef struct {
  char codec[24];
  char profile[40];
  uint32_t bit_depth, width, height;
  /* Zero when the source does not say. */
  float frame_rate;
  uint32_t bit_rate;
} jf_video;

/* What the TV's decoders take, per codec; a zero width where it has none, and
 * zero for no limit on the rate and bitrate. */
typedef struct {
  uint32_t max_width[JF_CODECS], max_height[JF_CODECS];
  float max_frame_rate[JF_CODECS];
  uint32_t max_bit_rate[JF_CODECS];
  /* 10-bit HEVC, VP9 and AV1. The list has no bit depths; a TV that shows HDR
   * decodes them, since that is what HDR arrives in. */
  bool deep;
} jf_decoders;

/* From the capability list the TV's media server keeps; off the TV, what the
 * host player's FFmpeg stands in for. In decoders.c. */
void jf_decoders_probe(jf_decoders *out);

/* The server's hardware transcoding, from its encoding configuration. */
typedef struct {
  /* False when the server would not say: only administrators may read it. */
  bool known;
  /* Hardware acceleration on, for encoding as well as decoding. */
  bool hardware;
  /* The codecs it decodes, as the server names them: ",h264,hevc,". Names,
   * not jf_codec, so one this client has never heard of still counts. */
  char codecs[256];
  bool hevc_10, vp9_10, hevc_rext_10, hevc_rext_12;
} jf_server_transcode;

/* Whether the TV decodes it as it is, both for the details screen and for the
 * player, which otherwise asks the server for a transcode. */
bool jf_tv_decodes(const jf_video *video, const jf_decoders *tv);

typedef enum {
  JF_DIRECT,          /* the TV decodes it */
  JF_SERVER_HARDWARE, /* the server transcodes it, or might: unknown */
  JF_SERVER_SOFTWARE, /* the server transcodes it on its CPU */
} jf_video_path;

jf_video_path jf_video_path_for(const jf_video *video, const jf_decoders *tv,
                                const jf_server_transcode *server);
