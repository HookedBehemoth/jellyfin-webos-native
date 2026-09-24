/*
 * Starfish, faked for a desktop: player.c runs unchanged - demux, ALSA audio,
 * subtitles, seeking - against a pipeline that takes the same access units the
 * TV's does and reports a wall clock as its presentation time.
 *
 * The picture is decoded here with FFmpeg - VAAPI when the GPU offers it, in
 * software otherwise - converted to RGBA, and drawn behind the UI by the render
 * thread when the clock reaches it. A few decoded frames are all it holds, and
 * a full queue answers BufferFull as the TV's decoder does, which is what paces
 * the feed. Nothing fancy: no tone mapping, so HDR looks washed out.
 */
#include <GLES3/gl3.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "smp.h"
#include "smp_segment.h"

static smp_event_fn *events;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static bool playing;
/* Presentation time when last paused or re-anchored, and the wall clock since
 * then while playing. */
static int64_t base_ns, since_ns;

static int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void emit(int type) {
  if (events != NULL)
    events(type, 0, "");
}

/* ------------------------------------------------------------ decoded frames
 */

/* Enough to cover B-frame reordering; more is memory for nothing. */
#define QUEUE 8
#define QUEUE_FULL 5

typedef struct {
  int64_t pts;
  uint8_t *rgba;
} frame;

/* Guarded by `lock`: the feed thread fills, the render thread drains. */
static frame queue[QUEUE];
static int queued;
static int out_w, out_h;
static bool fresh; /* a frame newer than the one on screen is queued */

/* Feed thread only. */
static AVCodecContext *decoder;
static AVBufferRef *device;
static AVPacket *packet;
static AVFrame *decoded, *transferred;
static struct SwsContext *scaler;
static bool reported;
/* A line a second, to tell a slow decoder from a slow clock. Under `lock`. */
static struct {
  int64_t since;
  int decoded, shown, skipped;
  double decode_ms, convert_ms;
} stats;

static enum AVPixelFormat pick_format(AVCodecContext *ctx,
                                      const enum AVPixelFormat *offered) {
  (void)ctx;
  for (const enum AVPixelFormat *f = offered; *f != AV_PIX_FMT_NONE; f++)
    if (*f == AV_PIX_FMT_VAAPI)
      return *f;
  /* The hardware cannot take this stream: the first software format. */
  for (const enum AVPixelFormat *f = offered; *f != AV_PIX_FMT_NONE; f++)
    if (!(av_pix_fmt_desc_get(*f)->flags & AV_PIX_FMT_FLAG_HWACCEL))
      return *f;
  return AV_PIX_FMT_NONE;
}

static void drop_queue(void) {
  for (int i = 0; i < queued; i++)
    free(queue[i].rgba);
  queued = 0;
}

static void decoder_close(void) {
  avcodec_free_context(&decoder);
  av_buffer_unref(&device);
  av_packet_free(&packet);
  av_frame_free(&decoded);
  av_frame_free(&transferred);
  sws_freeContext(scaler);
  scaler = NULL;
  pthread_mutex_lock(&lock);
  drop_queue();
  pthread_mutex_unlock(&lock);
}

/* The codec named in the load payload, as smp_payload_load writes it. */
static bool decoder_open(const char *payload) {
  const char *at = strstr(payload, "\"video\":\"");
  const char *name = at != NULL ? at + 9 : "";
  const enum AVCodecID id = strncmp(name, "H264", 4) == 0   ? AV_CODEC_ID_H264
                            : strncmp(name, "H265", 4) == 0 ? AV_CODEC_ID_HEVC
                            : strncmp(name, "VP9", 3) == 0  ? AV_CODEC_ID_VP9
                            : strncmp(name, "AV1", 3) == 0  ? AV_CODEC_ID_AV1
                                                            : AV_CODEC_ID_NONE;
  /* FFmpeg's own AV1 decoder only drives hardware; dav1d is the fallback. */
  const AVCodec *codec = avcodec_find_decoder(id);
  if (codec == NULL || (decoder = avcodec_alloc_context3(codec)) == NULL)
    return false;
  decoder->pkt_timebase = (AVRational){1, 1000000000};
  decoder->thread_count = 0;
  if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, NULL, NULL, 0) ==
      0) {
    decoder->hw_device_ctx = av_buffer_ref(device);
    decoder->get_format = pick_format;
  }
  if (avcodec_open2(decoder, codec, NULL) < 0 && id == AV_CODEC_ID_AV1) {
    avcodec_free_context(&decoder);
    codec = avcodec_find_decoder_by_name("libdav1d");
    if (codec == NULL || (decoder = avcodec_alloc_context3(codec)) == NULL ||
        avcodec_open2(decoder, codec, NULL) < 0)
      return false;
  } else if (!avcodec_is_open(decoder)) {
    return false;
  }
  reported = false;
  packet = av_packet_alloc();
  decoded = av_frame_alloc();
  transferred = av_frame_alloc();
  fprintf(stderr, "host video: %s decoder%s\n", codec->name,
          device != NULL ? ", VAAPI offered" : ", no VAAPI device");
  return packet != NULL && decoded != NULL && transferred != NULL;
}

/* One decoded frame into the queue, as RGBA no larger than 1080p. */
static double ms_since(int64_t start) {
  return (double)(now_ns() - start) / 1e6;
}

static void keep(AVFrame *f) {
  const int64_t started = now_ns();
  const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE
                          ? f->best_effort_timestamp
                          : f->pts;
  if (!reported) {
    reported = true;
    fprintf(stderr, "host video: %dx%d %s\n", f->width, f->height,
            f->format == AV_PIX_FMT_VAAPI ? "decoded by VAAPI"
                                          : av_get_pix_fmt_name(f->format));
  }
  if (f->format == AV_PIX_FMT_VAAPI) {
    av_frame_unref(transferred);
    if (av_hwframe_transfer_data(transferred, f, 0) < 0)
      return;
    f = transferred;
  }
  const double fit =
      f->width > 1920 || f->height > 1080
          ? (1920.0 / f->width < 1080.0 / f->height ? 1920.0 / f->width
                                                    : 1080.0 / f->height)
          : 1.0;
  const int w = (int)(f->width * fit) & ~1, h = (int)(f->height * fit) & ~1;
  scaler =
      sws_getCachedContext(scaler, f->width, f->height, f->format, w, h,
                           AV_PIX_FMT_RGBA, SWS_BILINEAR, NULL, NULL, NULL);
  uint8_t *rgba = malloc((size_t)w * h * 4);
  if (scaler == NULL || rgba == NULL) {
    free(rgba);
    return;
  }
  uint8_t *planes[4] = {rgba};
  const int strides[4] = {w * 4};
  sws_scale(scaler, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
            planes, strides);
  pthread_mutex_lock(&lock);
  stats.decoded++;
  stats.convert_ms += ms_since(started);
  if (queued == QUEUE) {
    free(queue[0].rgba);
    memmove(queue, queue + 1, sizeof(*queue) * (QUEUE - 1));
    queued--;
  }
  /* In presentation order: the decoder already reorders, this only guards. */
  int at = queued;
  while (at > 0 && queue[at - 1].pts > pts)
    at--;
  memmove(queue + at + 1, queue + at, sizeof(*queue) * (size_t)(queued - at));
  queue[at] = (frame){pts, rgba};
  queued++;
  out_w = w;
  out_h = h;
  fresh = true;
  pthread_mutex_unlock(&lock);
}

static void decode(const uint8_t *bytes, size_t size, int64_t pts) {
  if (decoder == NULL || av_new_packet(packet, (int)size) < 0)
    return;
  memcpy(packet->data, bytes, size);
  packet->pts = pts;
  const int64_t started = now_ns();
  const int sent = avcodec_send_packet(decoder, packet);
  av_packet_unref(packet);
  if (sent < 0 && sent != AVERROR(EAGAIN))
    return;
  while (avcodec_receive_frame(decoder, decoded) == 0) {
    keep(decoded);
    av_frame_unref(decoded);
  }
  pthread_mutex_lock(&lock);
  stats.decode_ms += ms_since(started);
  pthread_mutex_unlock(&lock);
}

/* ---------------------------------------------------------------- the clock */

static void anchor(int64_t pts_ns) {
  pthread_mutex_lock(&lock);
  base_ns = pts_ns;
  since_ns = now_ns();
  drop_queue();
  pthread_mutex_unlock(&lock);
  if (decoder != NULL)
    avcodec_flush_buffers(decoder);
}

bool smp_open(void) { return true; }
void smp_close(void) { events = NULL; }
bool smp_is_open(void) { return true; }

bool smp_load(const char *payload, smp_event_fn *callback) {
  events = callback;
  playing = false;
  decoder_close();
  if (!decoder_open(payload))
    fprintf(stderr, "host video: no decoder, playing without a picture\n");
  anchor(0);
  emit(SMP_LOADCOMPLETED);
  return true;
}

bool smp_unload(void) {
  decoder_close();
  emit(SMP_UNLOADCOMPLETED);
  return true;
}

bool smp_play(void) {
  pthread_mutex_lock(&lock);
  since_ns = now_ns();
  playing = true;
  fresh = true;
  pthread_mutex_unlock(&lock);
  emit(SMP_PLAYING);
  return true;
}

bool smp_pause(void) {
  pthread_mutex_lock(&lock);
  if (playing)
    base_ns += now_ns() - since_ns;
  playing = false;
  fresh = false;
  pthread_mutex_unlock(&lock);
  emit(SMP_PAUSED);
  return true;
}

bool smp_push_eos(void) { return true; }
bool smp_notify_foreground(void) { return true; }
bool smp_seek(const char *millis) {
  (void)millis;
  return true;
}
bool smp_flush(const char *payload) {
  (void)payload;
  anchor(0);
  return true;
}
bool smp_set_play_rate(const char *payload) {
  (void)payload;
  return true;
}

int64_t smp_get_current_playtime(void) {
  pthread_mutex_lock(&lock);
  const int64_t pts = base_ns + (playing ? now_ns() - since_ns : 0);
  pthread_mutex_unlock(&lock);
  return pts;
}

/* A full queue of pictures is a full decoder, and so is being two seconds
 * ahead without one - the pacing the TV's decoder buffer gives. */
bool smp_feed(const char *payload, char *status, size_t status_len) {
  const char *address = strstr(payload, "\"bufferAddr\":\"");
  const char *size = strstr(payload, "\"bufferSize\":");
  const char *pts = strstr(payload, "\"pts\":");
  const int64_t at = pts != NULL ? strtoll(pts + 6, NULL, 10) : 0;
  pthread_mutex_lock(&lock);
  const bool full =
      decoder != NULL ? queued >= QUEUE_FULL : at > base_ns + 2000000000LL;
  pthread_mutex_unlock(&lock);
  if (!full && address != NULL && size != NULL)
    decode((const uint8_t *)(uintptr_t)strtoull(address + 14, NULL, 16),
           (size_t)strtoull(size + 13, NULL, 10), at);
  snprintf(status, status_len, full ? "BufferFull" : "Ok");
  return true;
}

void *smp_player(void) { return NULL; }
const char *smp_shim_error(void) { return ""; }

bool jf_starfish_begin_segment(int64_t pts_ns) {
  pthread_mutex_lock(&lock);
  playing = false;
  pthread_mutex_unlock(&lock);
  anchor(pts_ns);
  return true;
}

const char *jf_starfish_segment_error(void) { return ""; }

/* ------------------------------------------------------------- presentation */

static GLuint program, texture;
static int texture_w, texture_h;

static GLuint compile(GLenum kind, const char *source) {
  const GLuint shader = glCreateShader(kind);
  glShaderSource(shader, 1, &source, NULL);
  glCompileShader(shader);
  return shader;
}

/* A full-screen triangle pair from the vertex id, textured with the frame. */
static bool gl_ready(void) {
  if (program != 0)
    return true;
  static const char vs[] =
      "#version 300 es\n"
      "out vec2 uv;\n"
      "void main() {\n"
      "  vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);\n"
      "  uv = vec2(p.x, 1.0 - p.y);\n"
      "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
      "}\n";
  static const char fs[] =
      "#version 300 es\n"
      "precision mediump float;\n"
      "uniform sampler2D picture;\n"
      "in vec2 uv;\n"
      "out vec4 color;\n"
      "void main() { color = vec4(texture(picture, uv).rgb, 1.0); }\n";
  program = glCreateProgram();
  glAttachShader(program, compile(GL_VERTEX_SHADER, vs));
  glAttachShader(program, compile(GL_FRAGMENT_SHADER, fs));
  glLinkProgram(program);
  GLint linked = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &linked);
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return linked != 0;
}

bool smp_host_frame_due(void) {
  pthread_mutex_lock(&lock);
  const bool due = fresh;
  pthread_mutex_unlock(&lock);
  return due;
}

/* The newest frame the clock has reached goes up; older ones go. Then the
 * last one uploaded is drawn, fitted to the window. */
void smp_host_render(uint32_t width, uint32_t height) {
  if (!gl_ready())
    return;
  const int64_t clock = smp_get_current_playtime();
  pthread_mutex_lock(&lock);
  int due = -1;
  for (int i = 0; i < queued && queue[i].pts <= clock; i++)
    due = i;
  if (due >= 0) {
    glBindTexture(GL_TEXTURE_2D, texture);
    if (out_w != texture_w || out_h != texture_h) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, out_w, out_h, 0, GL_RGBA,
                   GL_UNSIGNED_BYTE, queue[due].rgba);
      texture_w = out_w;
      texture_h = out_h;
    } else {
      glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, out_w, out_h, GL_RGBA,
                      GL_UNSIGNED_BYTE, queue[due].rgba);
    }
    stats.shown++;
    stats.skipped += due;
    for (int i = 0; i <= due; i++)
      free(queue[i].rgba);
    memmove(queue, queue + due + 1,
            sizeof(*queue) * (size_t)(queued - due - 1));
    queued -= due + 1;
  }
  if (now_ns() - stats.since >= 1000000000LL) {
    if (stats.decoded > 0)
      fprintf(stderr,
              "host video: clock %.3fs, decoded %d (%.1f ms each, %.1f of it "
              "converting), "
              "shown %d, late %d, queued %d\n",
              (double)clock / 1e9, stats.decoded,
              stats.decode_ms / stats.decoded, stats.convert_ms / stats.decoded,
              stats.shown, stats.skipped, queued);
    stats = (typeof(stats)){now_ns(), 0, 0, 0, 0, 0};
  }
  /* Keep the loop turning while a frame is still to come; paused, none is. */
  fresh = queued > 0 && playing;
  pthread_mutex_unlock(&lock);
  if (texture_w == 0)
    return;

  const float fit = (float)width / texture_w < (float)height / texture_h
                        ? (float)width / texture_w
                        : (float)height / texture_h;
  const int w = (int)(texture_w * fit), h = (int)(texture_h * fit);
  glViewport(((int)width - w) / 2, ((int)height - h) / 2, w, h);
  glDisable(GL_BLEND);
  glUseProgram(program);
  glBindVertexArray(0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture);
  glUniform1i(glGetUniformLocation(program, "picture"), 0);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glViewport(0, 0, (int)width, (int)height);
}
