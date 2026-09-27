#include "probe.h"

#include <EGL/egl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../ui/text.h"

/* Slang hands out binding points in declaration order and the `rect` uniform
 * block already took 0, so the text shader's sampler is binding 1 and the
 * texture has to go on unit 1. Check the generated GLSL if this ever moves. */
#define TEXT_ATLAS_UNIT 1

#define OVERLAY_W (PROBE_OVERLAY_COLS * TEXT_GLYPH_W)
#define OVERLAY_H (PROBE_OVERLAY_ROWS * TEXT_GLYPH_H)

static uint8_t overlay_pixels[OVERLAY_W * OVERLAY_H];

uint64_t probe_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* CPU time this thread actually burned, which is the honest "CPU time": wall
 * clock would just measure the wait for a free swapchain buffer and report the
 * vsync interval back. */
static uint64_t cpu_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Exponential moving average, so the numbers on screen stay readable instead of
 * flickering every frame. */
static double smooth(double previous, double sample) {
  return previous == 0 ? sample : previous * 0.9 + sample * 0.1;
}

static unsigned compile(GLenum type, const unsigned char *source) {
  const GLuint shader = glCreateShader(type);
  const char *text = (const char *)source;
  glShaderSource(shader, 1, &text, NULL);
  glCompileShader(shader);
  GLint ok = 0;
  glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    char log[4096] = {0};
    glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
    fprintf(stderr, "shader compile failed:\n%s\n", log);
    abort();
  }
  return shader;
}

unsigned probe_program(const unsigned char *vertex,
                       const unsigned char *fragment) {
  const GLuint program = glCreateProgram();
  glAttachShader(program, compile(GL_VERTEX_SHADER, vertex));
  glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment));
  glLinkProgram(program);
  GLint ok = 0;
  glGetProgramiv(program, GL_LINK_STATUS, &ok);
  if (!ok) {
    char log[4096] = {0};
    glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
    fprintf(stderr, "program link failed:\n%s\n", log);
    abort();
  }
  return program;
}

bool probe_overlay_init(probe_overlay *overlay, const unsigned char *text_vs,
                        const unsigned char *text_fs, int width, int height) {
  memset(overlay, 0, sizeof(*overlay));
  overlay->pixels = overlay_pixels;
  overlay->width = OVERLAY_W;
  overlay->height = OVERLAY_H;
  overlay->program = probe_program(text_vs, text_fs);

  /* Clip space, bottom-right, with a margin. */
  const float margin = 16.0f;
  const float dst_w = (float)(OVERLAY_W * PROBE_OVERLAY_ZOOM);
  const float dst_h = (float)(OVERLAY_H * PROBE_OVERLAY_ZOOM);
  const float fw = (float)width;
  const float fh = (float)height;
  const float x1 = 1.0f - 2.0f * margin / fw;
  const float y0 = -1.0f + 2.0f * margin / fh;
  const float rect[4] = {x1 - 2.0f * dst_w / fw, y0, x1,
                         y0 + 2.0f * dst_h / fh};

  glGenBuffers(1, &overlay->uniform_buffer);
  glBindBuffer(GL_UNIFORM_BUFFER, overlay->uniform_buffer);
  glBufferData(GL_UNIFORM_BUFFER, sizeof(rect), rect, GL_STATIC_DRAW);

  glGenTextures(1, &overlay->texture);
  glBindTexture(GL_TEXTURE_2D, overlay->texture);
  glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, OVERLAY_W, OVERLAY_H);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  return true;
}

void probe_overlay_clear(probe_overlay *overlay) {
  memset(overlay->pixels, 0, (size_t)overlay->width * overlay->height);
}

void probe_overlay_line(probe_overlay *overlay, int row, const char *text) {
  text_draw(overlay->pixels, (size_t)overlay->width, (size_t)overlay->height, 0,
            (size_t)row * TEXT_GLYPH_H, 1, 255, text);
}

void probe_overlay_draw(probe_overlay *overlay) {
  glUseProgram(overlay->program);
  glActiveTexture(GL_TEXTURE0 + TEXT_ATLAS_UNIT);
  glBindTexture(GL_TEXTURE_2D, overlay->texture);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, overlay->width, overlay->height,
                  GL_RED, GL_UNSIGNED_BYTE, overlay->pixels);
  glBindBufferBase(GL_UNIFORM_BUFFER, 0, overlay->uniform_buffer);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

/* ------------------------------------------------------------------- timing
 *
 * GL_EXT_disjoint_timer_query is an extension: libGLESv2 exports none of it, so
 * the five entry points come from eglGetProcAddress and stay optional. */
static void (*gen_queries)(GLsizei, GLuint *);
static void (*begin_query)(GLenum, GLuint);
static void (*end_query)(GLenum);
static void (*get_query_uiv)(GLuint, GLenum, GLuint *);
static void (*get_query_ui64v)(GLuint, GLenum, GLuint64 *);

static bool resolve_timer_query(void) {
  gen_queries =
      (void (*)(GLsizei, GLuint *))eglGetProcAddress("glGenQueriesEXT");
  begin_query = (void (*)(GLenum, GLuint))eglGetProcAddress("glBeginQueryEXT");
  end_query = (void (*)(GLenum))eglGetProcAddress("glEndQueryEXT");
  get_query_uiv = (void (*)(GLuint, GLenum, GLuint *))eglGetProcAddress(
      "glGetQueryObjectuivEXT");
  get_query_ui64v = (void (*)(GLuint, GLenum, GLuint64 *))eglGetProcAddress(
      "glGetQueryObjectui64vEXT");
  return gen_queries != NULL && begin_query != NULL && end_query != NULL &&
         get_query_uiv != NULL && get_query_ui64v != NULL;
}

void probe_timer_init(probe_timer *timer) {
  memset(timer, 0, sizeof(*timer));
  timer->mode = resolve_timer_query() ? PROBE_GPU_QUERY : PROBE_GPU_FINISH;
  if (timer->mode == PROBE_GPU_QUERY)
    gen_queries(2, timer->queries);
  timer->last_frame_ns = probe_now_ns();
}

const char *probe_gpu_mode_name(const probe_timer *timer) {
  return timer->mode == PROBE_GPU_QUERY ? "query" : "finish";
}

void probe_timer_begin(probe_timer *timer) {
  const uint64_t now = probe_now_ns();
  timer->frame_ms =
      smooth(timer->frame_ms, (double)(now - timer->last_frame_ns) / 1e6);
  timer->last_frame_ns = now;
  timer->cpu_start_ns = cpu_ns();

  /* The previous frame's GPU result is ready by now; reading the current one
   * here would stall the pipeline, which is what we are measuring. Asking an
   * unused query object for a result is GL_INVALID_OPERATION, so the first two
   * frames only write queries. */
  if (timer->mode == PROBE_GPU_QUERY && timer->frames > 1) {
    const GLuint previous = timer->queries[(timer->frames + 1) % 2];
    GLuint available = 0;
    get_query_uiv(previous, GL_QUERY_RESULT_AVAILABLE_EXT, &available);
    if (available != 0) {
      GLuint64 elapsed = 0;
      get_query_ui64v(previous, GL_QUERY_RESULT_EXT, &elapsed);
      if (elapsed != 0)
        timer->gpu_ms = smooth(timer->gpu_ms, (double)elapsed / 1e6);
    }
    /* Advertised but never answered: give up after a second and measure it the
     * blunt way. */
    if (timer->frames > 120 && timer->gpu_ms == 0)
      timer->mode = PROBE_GPU_FINISH;
  }
  if (timer->mode == PROBE_GPU_QUERY)
    begin_query(GL_TIME_ELAPSED_EXT, timer->queries[timer->frames % 2]);
}

void probe_timer_end(probe_timer *timer) {
  if (timer->mode == PROBE_GPU_QUERY) {
    end_query(GL_TIME_ELAPSED_EXT);
  } else {
    /* Costs the CPU/GPU overlap, but these probes are vsync-bound anyway. */
    const uint64_t before = probe_now_ns();
    glFinish();
    timer->gpu_ms =
        smooth(timer->gpu_ms, (double)(probe_now_ns() - before) / 1e6);
  }
  timer->frames++;
  timer->cpu_ms =
      smooth(timer->cpu_ms, (double)(cpu_ns() - timer->cpu_start_ns) / 1e6);
}

/* --------------------------------------------------------------------- dump */

void probe_dump_frame(void) {
  const size_t w = gl_width;
  const size_t h = gl_height;
  uint8_t *pixels = malloc(w * h * 4);
  if (pixels == NULL)
    return;
  glReadPixels(0, 0, (GLsizei)w, (GLsizei)h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

  /* 72x24 cells, each reporting the brightest pixel it covers. */
  size_t lit = 0;
  char line[73];
  for (size_t row = 0; row < 24; row++) {
    for (size_t col = 0; col < 72; col++) {
      uint8_t best = 0;
      for (size_t sy = 0; sy < 8; sy++) {
        const size_t y = (h - 1) - (row * h / 24 + sy * h / (24 * 8));
        for (size_t sx = 0; sx < 8; sx++) {
          const size_t x = col * w / 72 + sx * w / (72 * 8);
          const uint8_t *px = pixels + (y * w + x) * 4;
          const uint8_t value = px[0] > px[1] ? (px[0] > px[2] ? px[0] : px[2])
                                              : (px[1] > px[2] ? px[1] : px[2]);
          if (value > best)
            best = value;
        }
      }
      if (best > 24)
        lit++;
      line[col] = best <= 24 ? ' ' : best <= 80 ? '.' : best <= 160 ? '+' : '#';
    }
    line[72] = '\0';
    printf("%s\n", line);
  }
  printf("lit cells: %zu/1728\n", lit);

  /* The overlay is the only thing drawn in pure white, so counting white pixels
   * checks that the text program, the texture and the blend all work. */
  size_t white = 0;
  for (size_t i = 0; i < w * h; i++) {
    const uint8_t *px = pixels + i * 4;
    if (px[0] > 200 && px[0] == px[1] && px[1] == px[2])
      white++;
  }
  printf("overlay: %zu white pixels\n", white);
  free(pixels);
}
