#include "codecs.h"

#include <stdio.h>
#include <string.h>

static const char *const names[JF_CODECS] = {
    "h264", "hevc", "vp9", "av1", "mpeg2video", "vc1", "vp8", "mpeg4",
};
static const char *const labels[JF_CODECS] = {
    "H.264", "HEVC", "VP9", "AV1", "MPEG-2", "VC-1", "VP8", "MPEG-4",
};

jf_codec jf_codec_from_name(const char *name) {
  for (int i = 0; i < JF_CODECS; i++)
    if (strcmp(name, names[i]) == 0)
      return (jf_codec)i;
  return JF_CODECS;
}

const char *jf_codec_label(jf_codec codec) {
  return codec < JF_CODECS ? labels[codec] : NULL;
}

/* HEVC's range extensions: 4:2:2 and 4:4:4, which no TV decodes. */
static bool rext(const char *profile) {
  return strstr(profile, "Rext") != NULL || strstr(profile, "4:2:2") != NULL ||
         strstr(profile, "4:4:4") != NULL;
}

/* H.264 beyond High (High 10, 4:2:2, 4:4:4) is beyond every TV's decoder,
 * which answers "Sequence Init Fail" and shows nothing. `video` comes from the
 * server for the details screen and from the container for the player. */
bool jf_tv_decodes(const jf_video *video, const jf_decoders *tv) {
  const jf_codec codec = jf_codec_from_name(video->codec);
  if (codec == JF_CODECS || tv->max_width[codec] == 0)
    return false;
  if (video->bit_depth > (tv->deep && codec != JF_H264 ? 10u : 8u))
    return false;
  if (codec == JF_H264 && strncmp(video->profile, "High ", 5) == 0)
    return false;
  if (codec == JF_HEVC && rext(video->profile))
    return false;
  if (tv->max_frame_rate[codec] != 0 &&
      video->frame_rate > tv->max_frame_rate[codec] + 0.5f)
    return false;
  if (tv->max_bit_rate[codec] != 0 && video->bit_rate > tv->max_bit_rate[codec])
    return false;
  return video->width <= tv->max_width[codec] &&
         video->height <= tv->max_height[codec];
}

static bool server_decodes(jf_codec codec, const jf_video *video,
                           const jf_server_transcode *server) {
  char name[sizeof(video->codec) + 2];
  snprintf(name, sizeof(name), ",%s,", video->codec);
  if (!server->hardware || video->codec[0] == '\0' ||
      strstr(server->codecs, name) == NULL)
    return false;
  if (codec == JF_HEVC && rext(video->profile))
    return video->bit_depth > 10 ? server->hevc_rext_12 : server->hevc_rext_10;
  if (video->bit_depth <= 8)
    return true;
  if (video->bit_depth > 10)
    return false;
  switch (codec) {
  case JF_HEVC:
    return server->hevc_10;
  case JF_VP9:
    return server->vp9_10;
  case JF_AV1:
    return true;
  default:
    return false;
  }
}

jf_video_path jf_video_path_for(const jf_video *video, const jf_decoders *tv,
                                const jf_server_transcode *server) {
  if (jf_tv_decodes(video, tv))
    return JF_DIRECT;
  if (!server->known ||
      server_decodes(jf_codec_from_name(video->codec), video, server))
    return JF_SERVER_HARDWARE;
  return JF_SERVER_SOFTWARE;
}
