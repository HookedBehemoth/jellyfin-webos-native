/*
 * Three thousand rotating triangles in one instanced draw call, with CPU and GPU frame
 * times in the bottom-right corner.
 *
 *   per-vertex attribute:    position
 *   per-instance attributes: offset, direction, speed, phase, size
 *   uniforms:                time, aspect
 *
 * Every instance scales with sin(time + phase) * size and spins at its own speed, so no
 * two triangles peak at the same moment or the same size.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/gl.h"
#include "../platform/luna.h"
#include "../platform/window.h"
#include "probe.h"

#include "tri_vs.h"
#include "tri_fs.h"
#include "text_vs.h"
#include "text_fs.h"

#define INSTANCES 3000
#define TRI_RADIUS 0.045f
/* offset.x, offset.y, direction, speed, phase, size */
#define INSTANCE_FLOATS 6
#define TAU 6.28318530717958647692f

static void on_event(const jf_event *event)
{
    if (event->kind == JF_EVENT_KEY && event->key.pressed &&
        jf_window_is_back_key(event->key.code))
        jf_window_running = false;
}

/* A fixed seed, so two runs on the same TV are comparable. */
static float next_unit(unsigned *state)
{
    *state = *state * 1103515245u + 12345u;
    return (float)((*state >> 8) & 0xffffff) / (float)0x1000000;
}

int main(void)
{
  const char *appid = getenv("APPID");
  jf_window_set_handler(on_event);
  if (!jf_window_init(appid != NULL ? appid : "dev.hookedbehemoth.gltri",
                      "3000 triangles", 0, 0))
    return 1;
  if (!jf_luna_register_lifecycle(jf_window_post_quit, jf_window_post_raise))
    fprintf(stderr, "no webOS lifecycle\n");

  const int w = (int)gl_width;
  const int h = (int)gl_height;
  glViewport(0, 0, w, h);
  glClearColor(0.04f, 0.04f, 0.06f, 1.0f);

  const GLuint tri_program = probe_program(tri_vs, tri_fs);

  /* One equilateral triangle, reused by every instance. */
  static const float verts[6] = {
      0.0f,
      TRI_RADIUS,
      -TRI_RADIUS * 0.866f,
      -TRI_RADIUS * 0.5f,
      TRI_RADIUS * 0.866f,
      -TRI_RADIUS * 0.5f,
  };

  float *instances =
      malloc((size_t)INSTANCES * INSTANCE_FLOATS * sizeof(float));
  if (instances == NULL) {
    fprintf(stderr, "failed to allocate triangle instances\n");
    return 1;
  }
    unsigned seed = 0x7A1B;
    for (size_t i = 0; i < INSTANCES; i++) {
        float *instance = instances + i * INSTANCE_FLOATS;
        instance[0] = next_unit(&seed) * 2.0f - 1.0f; /* offset.x */
        instance[1] = next_unit(&seed) * 2.0f - 1.0f; /* offset.y */
        instance[2] = next_unit(&seed) * TAU;         /* direction */
        instance[3] = 0.35f + next_unit(&seed) * 0.9f; /* speed */
        instance[4] = next_unit(&seed) * TAU;          /* phase: when it peaks */
        instance[5] = 0.3f + next_unit(&seed) * 1.2f;  /* size: how big it peaks */
    }

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    GLuint buffers[4] = {0, 0, 0, 0};
    glGenBuffers(4, buffers);
    const GLuint vbo = buffers[0];
    const GLuint ibo = buffers[1];
    const GLuint ubo = buffers[2];
    const GLuint quad_vbo = buffers[3];

    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * 4, (const void *)0);

    glBindBuffer(GL_ARRAY_BUFFER, ibo);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)INSTANCES * INSTANCE_FLOATS * sizeof(float)), instances,
                 GL_STATIC_DRAW);
    /* These advance once per instance, not per vertex: {location, floats, byte offset}. */
    static const struct { GLuint location; GLint floats; GLuint offset; } attributes[] = {
        {1, 2, 0}, {2, 1, 8}, {3, 1, 12}, {4, 1, 16}, {5, 1, 20},
    };
    for (size_t i = 0; i < sizeof(attributes) / sizeof(attributes[0]); i++) {
        glEnableVertexAttribArray(attributes[i].location);
        glVertexAttribPointer(attributes[i].location, attributes[i].floats, GL_FLOAT, GL_FALSE,
                              INSTANCE_FLOATS * 4, (const void *)(uintptr_t)attributes[i].offset);
        glVertexAttribDivisor(attributes[i].location, 1);
    }
    free(instances);

    /* std140: two floats, padded to a 16-byte block. The shader wants height/width. */
    float uniforms[4] = {0, (float)h / (float)w, 0, 0};
    glBindBuffer(GL_UNIFORM_BUFFER, ubo);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(uniforms), uniforms, GL_DYNAMIC_DRAW);

    /* The overlay gets a vertex array of its own: the triangle's per-instance attributes
     * stay bound in `vao`, and switching arrays is cheaper than re-pointing five of
     * them. */
    GLuint overlay_vao = 0;
    glGenVertexArrays(1, &overlay_vao);
    glBindVertexArray(overlay_vao);
    static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glBindBuffer(GL_ARRAY_BUFFER, quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * 4, (const void *)0);

    probe_overlay overlay;
    probe_overlay_init(&overlay, text_vs, text_fs, w, h);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    probe_timer timer;
    probe_timer_init(&timer);
    fprintf(stderr,
            "%d instances at %ux%u, output %u.%03u Hz, GPU timing: %s\n",
            INSTANCES, gl_width, gl_height, jf_window_refresh_mhz / 1000,
            jf_window_refresh_mhz % 1000, probe_gpu_mode_name(&timer));

    unsigned dump_after = getenv("GLTRI_DUMP") != NULL ? 3 : 0;
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
        glUseProgram(tri_program);
        glBindBuffer(GL_UNIFORM_BUFFER, ubo);
        glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(uniforms), uniforms);
        glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo);
        glBindVertexArray(vao);
        glDrawArraysInstanced(GL_TRIANGLES, 0, 3, INSTANCES);

        probe_overlay_clear(&overlay);
        snprintf(line, sizeof(line), "cpu  %6.2f ms", timer.cpu_ms);
        probe_overlay_line(&overlay, 0, line);
        snprintf(line, sizeof(line), "gpu %s%6.2f ms",
                 timer.mode == PROBE_GPU_FINISH ? "*" : " ", timer.gpu_ms);
        probe_overlay_line(&overlay, 1, line);
        snprintf(line, sizeof(line), "frame%6.2f ms", timer.frame_ms);
        probe_overlay_line(&overlay, 2, line);
        snprintf(line, sizeof(line), "%5.1f/%5.1f Hz",
                 timer.frame_ms > 0 ? 1000.0 / timer.frame_ms : 0.0,
                 (double)jf_window_refresh_mhz / 1000.0);
        probe_overlay_line(&overlay, 3, line);
        snprintf(line, sizeof(line), "%d triangles", INSTANCES);
        probe_overlay_line(&overlay, 4, line);
        if (dump_after > 0 || timer.frames % 60 == 0)
            fprintf(stderr, "cpu %.2f ms  gpu%s%.2f ms  frame %.2f ms  %.1f Hz\n", timer.cpu_ms,
                    timer.mode == PROBE_GPU_FINISH ? "* " : " ", timer.gpu_ms, timer.frame_ms,
                    timer.frame_ms > 0 ? 1000.0 / timer.frame_ms : 0.0);

        glBindVertexArray(overlay_vao);
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
