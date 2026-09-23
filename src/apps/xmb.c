/*
 * Full-screen XMB wave and starfield, with CPU and GPU frame times in the bottom-right
 * corner.
 *
 * Shaders are written in Slang, under src/shaders, and compiled to GLSL ES by the
 * build - see cmake/Assets.cmake and docs/opengl.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/gl.h"
#include "../platform/luna.h"
#include "../platform/window.h"
#include "probe.h"

#include "xmb_vs.h"
#include "xmb_fs.h"
#include "text_vs.h"
#include "text_fs.h"

/* The shader demo has no navigation state: Back is simply its close key, matching the
 * lifecycle close request from SAM. */
static void on_event(const jf_event *event)
{
    if (event->kind == JF_EVENT_KEY && event->key.pressed &&
        jf_window_is_back_key(event->key.code))
        jf_window_running = false;
}

int main(void)
{
  const char *appid = getenv("APPID");
  jf_window_set_handler(on_event);
  if (!jf_window_init(appid != NULL ? appid : "dev.hookedbehemoth.xmb", "XMB",
                      0, 0))
    return 1;
  /* SAM lifecycle messages arrive on Luna's worker thread; SDL's posted events
   * bring them safely back to this render thread. */
  if (!jf_luna_register_lifecycle(jf_window_post_quit, jf_window_post_raise))
    fprintf(stderr, "no webOS lifecycle\n");

  const int w = (int)gl_width;
  const int h = (int)gl_height;
  glViewport(0, 0, w, h);
  glClearColor(0, 0, 0, 1);

  const GLuint xmb_program = probe_program(xmb_vs, xmb_fs);

  /* Both the XMB and text shaders consume the same (0,0)-(1,1) strip. */
  GLuint vao = 0;
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  GLuint quad_vbo = 0;
  glGenBuffers(1, &quad_vbo);
  static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
  glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * 4, (const void *)0);

  /* std140 layout: time at byte 0, float2 resolution at byte 8. */
  float uniforms[4] = {0, 0, (float)w, (float)h};
  GLuint xmb_ubo = 0;
  glGenBuffers(1, &xmb_ubo);
  glBindBuffer(GL_UNIFORM_BUFFER, xmb_ubo);
  glBufferData(GL_UNIFORM_BUFFER, sizeof(uniforms), uniforms, GL_DYNAMIC_DRAW);

  probe_overlay overlay;
  probe_overlay_init(&overlay, text_vs, text_fs, w, h);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  const GLenum setup_error = glGetError();
  if (setup_error != GL_NO_ERROR) {
    fprintf(stderr, "GL error after setup: 0x%x\n", setup_error);
    return 1;
  }

    probe_timer timer;
    probe_timer_init(&timer);
    fprintf(stderr, "XMB at %ux%u, output %u.%03u Hz, GPU timing: %s\n",
            gl_width, gl_height, jf_window_refresh_mhz / 1000,
            jf_window_refresh_mhz % 1000, probe_gpu_mode_name(&timer));

    /* A couple of frames first, so the timer query has a result to show. */
    unsigned dump_after = getenv("XMB_DUMP") != NULL ? 3 : 0;
    const uint64_t start = probe_now_ns();
    char line[PROBE_OVERLAY_COLS + 1];

    while (jf_window_poll()) {
        if (!jf_window_drawable) {
            jf_window_wait();
            continue;
        }
        probe_timer_begin(&timer);
        uniforms[0] = (float)(probe_now_ns() - start) / 1e9f;
        if (dump_after > 0)
            uniforms[0] = 1.5f;

        glClear(GL_COLOR_BUFFER_BIT);
        glUseProgram(xmb_program);
        glBindBuffer(GL_UNIFORM_BUFFER, xmb_ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(uniforms), uniforms);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, xmb_ubo);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

        probe_overlay_clear(&overlay);
        snprintf(line, sizeof(line), "cpu  %6.2f ms", timer.cpu_ms);
        probe_overlay_line(&overlay, 0, line);
        /* The star marks the glFinish fallback: a real number, but measured by stalling
         * rather than by asking the GPU. */
        snprintf(line, sizeof(line), "gpu %s%6.2f ms",
                 timer.mode == PROBE_GPU_FINISH ? "*" : " ", timer.gpu_ms);
        probe_overlay_line(&overlay, 1, line);
        snprintf(line, sizeof(line), "frame%6.2f ms", timer.frame_ms);
        probe_overlay_line(&overlay, 2, line);
        /* Both numbers on purpose: what the output says it runs at, and what we are
         * actually presenting. On a variable-refresh output they differ. */
        snprintf(line, sizeof(line), "%5.1f/%5.1f Hz",
                 timer.frame_ms > 0 ? 1000.0 / timer.frame_ms : 0.0,
                 (double)jf_window_refresh_mhz / 1000.0);
        probe_overlay_line(&overlay, 3, line);
        probe_overlay_line(&overlay, 4, "xmb shader");
        /* Same numbers as the overlay, once a second, so a run over ssh shows them
         * without a camera pointed at the TV. */
        if (dump_after > 0 || timer.frames % 60 == 0)
            fprintf(stderr, "cpu %.2f ms  gpu%s%.2f ms  frame %.2f ms  %.1f Hz\n", timer.cpu_ms,
                    timer.mode == PROBE_GPU_FINISH ? "* " : " ", timer.gpu_ms, timer.frame_ms,
                    timer.frame_ms > 0 ? 1000.0 / timer.frame_ms : 0.0);
        probe_overlay_draw(&overlay);

        probe_timer_end(&timer);
        if (dump_after > 0 && --dump_after == 0) {
            probe_dump_frame();
            break;
        }
        jf_window_swap();
    }

    jf_luna_deinit();
    jf_window_deinit();
    return 0;
}
