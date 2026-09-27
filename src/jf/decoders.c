/* The TV's video decoders, once at startup. Its own file: it reads the TV,
 * not the server, and needs json-c, which jf-portable does without. */
#include "codecs.h"

#include <json.h>
#include <stdio.h>
#include <string.h>

void jf_decoders_probe(jf_decoders *out) {
  memset(out, 0, sizeof(*out));
#ifndef JF_WEBOS
  /* smp_host's FFmpeg decodes whatever the player feeds it, at any size. */
  for (jf_codec codec = JF_H264; codec <= JF_AV1; codec++)
    out->max_width[codec] = out->max_height[codec] = UINT32_MAX;
  out->deep = true;
#else
  /* uMediaServer's list names every codec the SoC decodes, some twice ("H.265"
   * and "HEVC"). Only the four the player feeds count. libpf's elementary-
   * stream path (CustomPipeline) has caps for H.264, HEVC, VP9, AV1, VP8 and
   * VC-1 alone; MPEG-4 given caps by hand still ends in lxvideodec's "Sequence
   * Init Fail", so MPEG-1/2/4 and the rest are the server's to transcode. */
  static const struct {
    const char *name;
    jf_codec codec;
  } fed[] = {{"H.264", JF_H264},
             {"H.265", JF_HEVC},
             {"HEVC", JF_HEVC},
             {"VP9", JF_VP9},
             {"AV1", JF_AV1}};
  json_object *root = json_object_from_file(
      "/etc/umediaserver/device_codec_capability_config.json");
  json_object *codecs = NULL;
  if (root == NULL ||
      !json_object_object_get_ex(root, "videoCodecs", &codecs) ||
      json_object_get_type(codecs) != json_type_array) {
    fprintf(stderr, "decoders: no capability list, assuming none\n");
    json_object_put(root);
    return;
  }
  for (size_t i = 0; i < json_object_array_length(codecs); i++) {
    json_object *entry = json_object_array_get_idx(codecs, i);
    char name[16];
    double width = 0, height = 0, rate = 0, mbps = 0;
    json_object *value = NULL;
    snprintf(name, sizeof(name), "%s",
             json_object_object_get_ex(entry, "name", &value)
                 ? json_object_get_string(value)
                 : "");
    if (json_object_object_get_ex(entry, "maxWidth", &value))
      width = json_object_get_double(value);
    if (json_object_object_get_ex(entry, "maxHeight", &value))
      height = json_object_get_double(value);
    if (json_object_object_get_ex(entry, "maxFrameRate", &value))
      rate = json_object_get_double(value);
    /* In Mbit/s. */
    if (json_object_object_get_ex(entry, "maxBitRate", &value))
      mbps = json_object_get_double(value);
    for (size_t f = 0; f < sizeof(fed) / sizeof(fed[0]); f++) {
      const jf_codec codec = fed[f].codec;
      if (strcmp(name, fed[f].name) != 0 || width <= out->max_width[codec])
        continue;
      out->max_width[codec] = (uint32_t)width;
      out->max_height[codec] = (uint32_t)height;
      out->max_frame_rate[codec] = (float)rate;
      out->max_bit_rate[codec] = (uint32_t)(mbps * 1000000);
    }
  }
  json_object_put(root);
#endif
  for (jf_codec codec = 0; codec < JF_CODECS; codec++)
    if (out->max_width[codec] != 0)
      fprintf(stderr, "decoders: %s up to %ux%u@%g, %u bit/s\n",
              jf_codec_label(codec), out->max_width[codec],
              out->max_height[codec], out->max_frame_rate[codec],
              out->max_bit_rate[codec]);
}
