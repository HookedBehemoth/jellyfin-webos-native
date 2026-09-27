#include "smp_payload.h"

#include <inttypes.h>
#include <stdio.h>

static bool fits(int written, size_t out_len) {
  return written > 0 && (size_t)written < out_len;
}

bool smp_payload_load(char *out, size_t out_len,
                      const smp_video_params *video) {
  char fps[64] = "";
  if (video->fps_num > 0 && video->fps_den > 0) {
    int n =
        snprintf(fps, sizeof(fps), ",\"videoFpsValue\":%d,\"videoFpsScale\":%d",
                 video->fps_num, video->fps_den);
    if (!fits(n, sizeof(fps)))
      return false;
  }

  int written =
      snprintf(out, out_len,
               "{\"args\":["
               "{"
               "\"mediaTransportType\":\"BUFFERSTREAM\","
               "\"option\":{"
               "\"appId\":\"%s\","
               "\"needAudio\":false,"
               "\"seekMode\":\"keep-rate\","
               "\"queryPosition\":true,"
               "\"useDroppedFrameEvent\":true,"
               /* No audio track means nothing else establishes a running clock,
                * so the video sink takes the system clock as its own. */
               "\"useCurrentTimeWithSystemClock\":true,"
               "\"windowId\":\"%s\","
               "\"transmission\":{"
               "\"contentsType\":\"LIVE\","
               "\"trickType\":\"client-side\""
               "},"
               "\"externalStreamingInfo\":{"
               "\"audioSync\":false,"
               "\"streamQualityInfo\":true,"
               "\"streamQualityInfoNonFlushable\":true,"
               "\"streamQualityInfoCorruptedFrame\":true,"
               "\"contents\":{\"format\":\"RAW\","
               "\"provider\":\"%s\","
               "\"codec\":{\"video\":\"%s\"},"
               "\"esInfo\":{"
               "\"pauseAtDecodeTime\":true,"
               "\"seperatedPTS\":true,"
               "\"ptsToDecode\":0,"
               "\"videoWidth\":%d,"
               "\"videoHeight\":%d%s}},"
               "\"bufferingCtrInfo\":{\"preBufferByte\":0,"
               "\"bufferMinLevel\":0,"
               "\"bufferMaxLevel\":0,"
               "\"qBufferLevelVideo\":0,"
               "\"srcBufferLevelVideo\":{\"minimum\":1048576,"
               "\"maximum\":8388608},"
               "\"qBufferLevelAudio\":0,"
               "\"srcBufferLevelAudio\":{\"minimum\":1048576,"
               "\"maximum\":2097152}}}}}]}",
               video->app_id, video->window_id, video->app_id, video->codec,
               video->width, video->height, fps);
  return fits(written, out_len);
}

bool smp_payload_feed(char *out, size_t out_len, const void *buffer,
                      size_t size, int64_t pts_ns, int es_data) {
  int written =
      snprintf(out, out_len,
               "{\"bufferAddr\":\"0x%" PRIxPTR "\",\"bufferSize\":%zu,"
               "\"pts\":%" PRId64 ",\"esData\":%d}",
               (uintptr_t)buffer, size, pts_ns, es_data);
  return fits(written, out_len);
}

bool smp_payload_flush(char *out, size_t out_len, int64_t offset_ms) {
  int written = snprintf(
      out, out_len, "{\"audioFlush\":false,\"offset\":%" PRId64 "}", offset_ms);
  return fits(written, out_len);
}

bool smp_payload_play_rate(char *out, size_t out_len, int rate_millis) {
  /* audioOutput is false: the pipeline has no audio sink to rate-adjust. */
  int written =
      snprintf(out, out_len, "{\"audioOutput\":false,\"playRate\":%d.%03d}",
               rate_millis / 1000, rate_millis % 1000);
  return fits(written, out_len);
}

const char *smp_event_name(int type) {
  switch (type) {
  case 0x0:
    return "TYPE_FRAMEREADY";
  case 0x1:
    return "TYPE_STR_STREAMING_INFO_PERI";
  case 0x2:
    return "TYPE_INT_BUFFER_RANGE_INFO";
  case 0x3:
    return "TYPE_INT_DURATION";
  case 0x4:
    return "TYPE_STR_VIDEO_INFO";
  case 0x5:
    return "TYPE_STR_VIDEO_TRACK_INFO";
  case 0x7:
    return "TYPE_STR_AUDIO_INFO";
  case 0x8:
    return "TYPE_STR_AUDIO_TRACK_INFO";
  case 0x9:
    return "TYPE_STR_SUBT_TRACK_INFO";
  case 0xa:
    return "TYPE_STR_BUFF_EVENT";
  case 0xb:
    return "TYPE_STR_SOURCE_INFO";
  case 0xd:
    return "TYPE_INT_NUM_PROGRAM";
  case 0xe:
    return "TYPE_INT_NUM_VIDEO_TRACK";
  case 0xf:
    return "TYPE_INT_NUM_AUDIO_TRACK";
  case 0x11:
    return "TYPE_STR_RESOURCE_INFO";
  case 0x12:
    return "TYPE_INT_ERROR";
  case 0x13:
    return "TYPE_STR_ERROR";
  case 0x15:
    return "TYPE_STR_STATE_UPDATE__PRELOADCOMPLETED";
  case 0x16:
    return "TYPE_STR_STATE_UPDATE__LOADCOMPLETED";
  case 0x17:
    return "TYPE_STR_STATE_UPDATE__UNLOADCOMPLETED";
  case 0x18:
    return "TYPE_STR_STATE_UPDATE__TRACKSELECTED";
  case 0x19:
    return "TYPE_STR_STATE_UPDATE__SEEKDONE";
  case 0x1a:
    return "TYPE_STR_STATE_UPDATE__PLAYING";
  case 0x1b:
    return "TYPE_STR_STATE_UPDATE__PAUSED";
  case 0x1c:
    return "TYPE_STR_STATE_UPDATE__ENDOFSTREAM";
  case 0x1d:
    return "TYPE_STR_CUSTOM";
  case 0x26:
    return "TYPE_INT_NEED_DATA";
  case 0x27:
    return "TYPE_INT_ENOUGH_DATA";
  case 0x2b:
    return "TYPE_INT_SVP_VDEC_READY";
  case 0x2c:
    return "TYPE_INT_BUFFERLOW";
  case 0x2d:
    return "TYPE_STR_BUFFERFULL";
  case 0x2e:
    return "TYPE_STR_BUFFERLOW";
  case 0x30:
    return "TYPE_DROPPED_FRAME";
  case 0x270:
    return "USER_DEFINED";
  default:
    return "(unknown)";
  }
}

bool smp_event_has_text(int type) {
  switch (type) {
  case 0x1:
  case 0x4:
  case 0x5:
  case 0x7:
  case 0x8:
  case 0x9:
  case 0xa:
  case 0xb:
  case 0x11:
  case 0x13:
  case 0x15:
  case 0x16:
  case 0x17:
  case 0x18:
  case 0x19:
  case 0x1a:
  case 0x1b:
  case 0x1c:
  case 0x1d:
  case 0x2d:
  case 0x2e:
  case 10001:
  case 10002:
    return true;
  default:
    return false;
  }
}
