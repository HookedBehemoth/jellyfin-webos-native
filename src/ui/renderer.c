#include "renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/gl.h"

#include "ui_border.h"
#include "ui_fill.h"
#include "ui_glyph.h"
#include "ui_image.h"
#include "ui_image_premultiplied.h"
#include "ui_round.h"
#include "ui_vs.h"

/* One program per kind of instance, so no fragment ever executes another kind's code.
 * The kind is also part of the batch state, so switching program costs a flush and
 * nothing else. */
typedef enum {
  KIND_FILL,
  KIND_ROUND,
  KIND_BORDER,
  KIND_GLYPH,
  KIND_IMAGE,
  KIND_IMAGE_PREMULTIPLIED,
  KIND_COUNT
} kind;

/* Slang hands out bindings in declaration order and the uniform block takes 0, so the
 * shader's single sampler is binding 1. Check the generated GLSL if the shader's
 * declarations are ever reordered. */
#define ATLAS_UNIT GL_TEXTURE1

typedef struct {
    float rect[4];
    float uv[4];
    float color[4];
    float clip[4];
    float shape[4];
    float mask_scale[4];
    float mask_offset[4];
} instance;

/* std140 pads a float2 block to 16 bytes, so the tail is explicit. */
typedef struct { float viewport[2]; float padding[2]; } uniforms;

/* Everything a draw call needs bound. Two batches merge iff these match. */
typedef struct {
    kind kind;
    GLuint texture;
    uniforms uniforms;
    /* Off for square opaque fills. Measured at 1080p: alpha blending over this scene's
     * 3.1x overdraw costs 1.6 ms, and a fill with no soft edge and no alpha does not need
     * any of it. It also lets the driver treat the full-screen background as a tile clear
     * rather than a blend over whatever was in the framebuffer. */
    bool blend;
} batch_state;

struct jf_renderer {
    jf_atlas *atlas;
    instance *instances;
    size_t instance_count, instance_capacity;
    GLuint programs[KIND_COUNT];
    GLuint vao, instance_buffer, uniform_buffer;
    GLuint texture;
    uint16_t atlas_texture_size;
    GLuint media_texture;

    bool have_state;
    batch_state state;
    bool blend_enabled;
    size_t batch_start;
    uint32_t batches;
    double covered;
};

/* ----------------------------------------------------------------- shaders */

static GLuint compile(GLenum type, const unsigned char *source)
{
    const GLuint shader = glCreateShader(type);
    const char *text = (const char *)source;
    glShaderSource(shader, 1, &text, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096] = {0};
        glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "UI shader compile failed:\n%s\n", log);
        abort();
    }
    return shader;
}

static GLuint make_program(const unsigned char *fragment)
{
    const GLuint program = glCreateProgram();
    glAttachShader(program, compile(GL_VERTEX_SHADER, ui_vs));
    glAttachShader(program, compile(GL_FRAGMENT_SHADER, fragment));
    glLinkProgram(program);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096] = {0};
        glGetProgramInfoLog(program, sizeof(log) - 1, NULL, log);
        fprintf(stderr, "UI program link failed:\n%s\n", log);
        abort();
    }
    return program;
}

/* Into a fresh immutable-storage texture, clamped and linear - the settings
 * every texture here wants. */
static GLuint make_texture(uint32_t width, uint32_t height,
                           const uint8_t *pixels, bool alpha) {
  GLuint id = 0;
  glGenTextures(1, &id);
  glActiveTexture(ATLAS_UNIT);
  glBindTexture(GL_TEXTURE_2D, id);
  glTexStorage2D(GL_TEXTURE_2D, 1, alpha ? GL_RGBA8 : GL_RGB8, (GLsizei)width,
                 (GLsizei)height);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)width, (GLsizei)height,
                  alpha ? GL_RGBA : GL_RGB, GL_UNSIGNED_BYTE, pixels);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  return id;
}

static void atlas_texture_params(void)
{
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

/* ------------------------------------------------------------------- setup */

jf_renderer *jf_renderer_create(const uint8_t *media, uint32_t media_width, uint32_t media_height)
{
    jf_renderer *r = calloc(1, sizeof(*r));
    if (r == NULL)
        return NULL;
    r->atlas = jf_atlas_create();
    if (r->atlas == NULL) {
        free(r);
        return NULL;
    }
    GLint max_texture_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    jf_atlas_set_max_size(r->atlas, (uint16_t)(max_texture_size < 16384 ? max_texture_size : 16384));

    glGenVertexArrays(1, &r->vao);
    glBindVertexArray(r->vao);
    GLuint buffers[3] = {0, 0, 0};
    glGenBuffers(3, buffers);

    static const float quad[8] = {0, 0, 1, 0, 0, 1, 1, 1};
    glBindBuffer(GL_ARRAY_BUFFER, buffers[0]);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (const void *)0);

    glBindBuffer(GL_ARRAY_BUFFER, buffers[1]);
    glBufferData(GL_ARRAY_BUFFER, 1, NULL, GL_DYNAMIC_DRAW);
    for (GLuint i = 0; i < 7; i++) {
      glEnableVertexAttribArray(i + 1);
      glVertexAttribPointer(i + 1, 4, GL_FLOAT, GL_FALSE, sizeof(instance),
                            (const void *)(uintptr_t)(i * 16));
      glVertexAttribDivisor(i + 1, 1);
    }

    glBindBuffer(GL_UNIFORM_BUFFER, buffers[2]);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(uniforms), NULL, GL_DYNAMIC_DRAW);

    r->instance_buffer = buffers[1];
    r->uniform_buffer = buffers[2];

    glGenTextures(1, &r->texture);
    glActiveTexture(ATLAS_UNIT);
    glBindTexture(GL_TEXTURE_2D, r->texture);
    const GLsizei atlas_size = (GLsizei)jf_atlas_size(r->atlas);
    /* One channel: rasterised coverage, not a three-channel distance field. */
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, atlas_size, atlas_size);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, atlas_size, atlas_size, GL_RED, GL_UNSIGNED_BYTE,
                    jf_atlas_pixels(r->atlas));
    atlas_texture_params();
    jf_atlas_clear_dirty(r->atlas);
    r->atlas_texture_size = jf_atlas_size(r->atlas);

    /* Same unit as the glyph atlas: only one texture is bound at a time. */
    r->media_texture =
        media != NULL ? make_texture(media_width, media_height, media, false)
                      : r->texture;

    const unsigned char *const fragments[KIND_COUNT] = {
        ui_fill,  ui_round, ui_border,
        ui_glyph, ui_image, ui_image_premultiplied};
    for (int i = 0; i < KIND_COUNT; i++)
        r->programs[i] = make_program(fragments[i]);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    r->blend_enabled = true;
    return r;
}

void jf_renderer_destroy(jf_renderer *r)
{
    if (r == NULL)
        return;
    free(r->instances);
    jf_atlas_destroy(r->atlas);
    free(r);
}

jf_atlas *jf_renderer_atlas(jf_renderer *r) { return r->atlas; }
uint32_t jf_renderer_batches(const jf_renderer *r) { return r->batches; }
size_t jf_renderer_instances(const jf_renderer *r) { return r->instance_count; }
double jf_renderer_covered(const jf_renderer *r) { return r->covered; }

float jf_renderer_measure(jf_renderer *r, const char *text, float size)
{
    return jf_atlas_measure(r->atlas, text, size);
}

uint32_t jf_renderer_create_texture(jf_renderer *r, uint32_t width, uint32_t height,
                                    const uint8_t *rgb)
{
  const GLuint id = make_texture(width, height, rgb, false);
  r->have_state =
      false; /* the batcher tracks what it bound last; this bypassed it */
  return id;
}

uint32_t jf_renderer_create_rgba_texture(jf_renderer *r, uint32_t width,
                                         uint32_t height, const uint8_t *rgba) {
  const GLuint id = make_texture(width, height, rgba, true);
  r->have_state = false;
  return id;
}

void jf_renderer_update_texture(jf_renderer *r, uint32_t id, uint32_t x,
                                uint32_t y, uint32_t width, uint32_t height,
                                const uint8_t *rgb) {
  glActiveTexture(ATLAS_UNIT);
  glBindTexture(GL_TEXTURE_2D, id);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexSubImage2D(GL_TEXTURE_2D, 0, (GLint)x, (GLint)y, (GLsizei)width,
                  (GLsizei)height, GL_RGB, GL_UNSIGNED_BYTE, rgb);
  r->have_state = false;
}

void jf_renderer_destroy_texture(jf_renderer *r, uint32_t id)
{
    if (id == 0)
        return;
    glDeleteTextures(1, &id);
    r->have_state = false;
}

/* ----------------------------------------------------------------- batching */

static void flush(jf_renderer *r)
{
    if (!r->have_state)
        return;
    const size_t pending = r->instance_count - r->batch_start;
    if (pending == 0)
        return;

    if (r->state.blend != r->blend_enabled) {
        if (r->state.blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        r->blend_enabled = r->state.blend;
    }
    glUseProgram(r->programs[r->state.kind]);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(uniforms), &r->state.uniforms);
    glBindTexture(GL_TEXTURE_2D, r->state.texture);
    glBindBuffer(GL_ARRAY_BUFFER, r->instance_buffer);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(pending * sizeof(instance)),
                 &r->instances[r->batch_start], GL_DYNAMIC_DRAW);
    glDrawArraysInstanced(GL_TRIANGLE_STRIP, 0, 4, (GLsizei)pending);

    r->batch_start = r->instance_count;
    r->batches++;
}

/* Declare what the next instances need. `texture` of 0 means "whatever is already bound"
 * - an untextured instance never forces a flush. */
static void want(jf_renderer *r, kind which, GLuint texture, uniforms u, bool blend)
{
    if (!r->have_state) {
        r->have_state = true;
        r->state = (batch_state){which, texture != 0 ? texture : r->texture, u, blend};
        return;
    }
    const batch_state next = {which, texture != 0 ? texture : r->state.texture, u, blend};
    if (next.kind == r->state.kind && next.texture == r->state.texture &&
        next.blend == r->state.blend &&
        memcmp(&next.uniforms, &r->state.uniforms, sizeof(uniforms)) == 0)
        return;
    flush(r);
    r->state = next;
}

static void push(jf_renderer *r, loom_rect rect, loom_rect clip,
                 const loom_mask *mask, const float uv[4],
                 const loom_color color, float radius, float border) {
  if (!r->have_state)
    return;
  if (r->instance_count == r->instance_capacity) {
    const size_t capacity =
        r->instance_capacity ? r->instance_capacity * 2 : 1024;
    instance *grown = realloc(r->instances, capacity * sizeof(*grown));
    if (grown == NULL)
      return;
    r->instances = grown;
    r->instance_capacity = capacity;
  }
  /* Match the vertex shader: the quad is shrunk to its clip rectangle, so a
   * fully clipped instance rasterises nothing. */
  const loom_rect visible = loom_intersect(rect, clip);
  r->covered += (double)visible.w * (double)visible.h;

  instance *out = &r->instances[r->instance_count++];
  out->rect[0] = rect.x;
  out->rect[1] = rect.y;
  out->rect[2] = rect.w;
  out->rect[3] = rect.h;
  memcpy(out->uv, uv, sizeof(out->uv));
  for (int i = 0; i < 4; i++)
    out->color[i] = (float)color[i] / 255.0f;
  out->clip[0] = clip.x;
  out->clip[1] = clip.y;
  out->clip[2] = clip.x + clip.w;
  out->clip[3] = clip.y + clip.h;
  out->shape[0] = radius;
  out->shape[1] = border;
  out->shape[2] = 0;
  out->shape[3] = 0;
  /* Bottom and right measure inwards from the other side, hence the mirrored
   * edge. An edge without a fade is a constant fully opaque 1. */
  static const float sign[4] = {1, -1, 1, -1};
  for (int i = 0; i < 4; i++) {
    const float width = mask->width[i];
    out->mask_scale[i] = width > 0 ? 1 / width : 0;
    out->mask_offset[i] = width > 0 ? sign[i] * mask->edge[i] / width : -1;
  }
}

/* Whether any faded edge reaches into the visible part of the command. */
static bool faded(const loom_command *c) {
  const loom_rect v = loom_intersect(c->rect, c->clip);
  const float *edge = c->mask.edge, *width = c->mask.width;
  return (width[LOOM_FADE_TOP] > 0 &&
          v.y < edge[LOOM_FADE_TOP] + width[LOOM_FADE_TOP]) ||
         (width[LOOM_FADE_BOTTOM] > 0 &&
          v.y + v.h > edge[LOOM_FADE_BOTTOM] - width[LOOM_FADE_BOTTOM]) ||
         (width[LOOM_FADE_LEFT] > 0 &&
          v.x < edge[LOOM_FADE_LEFT] + width[LOOM_FADE_LEFT]) ||
         (width[LOOM_FADE_RIGHT] > 0 &&
          v.x + v.w > edge[LOOM_FADE_RIGHT] - width[LOOM_FADE_RIGHT]);
}

static void append_text(jf_renderer *r, loom_rect rect, loom_rect clip, const loom_command *command)
{
    const float size = command->text.size;
    const float scale = size / JF_ATLAS_BASE_PX;
    float pen_x = rect.x;
    const float baseline = rect.y + size;
    const char *text = command->text.contents;
    for (uint32_t cp = jf_utf8_next(&text); cp != 0; cp = jf_utf8_next(&text)) {
        const jf_glyph *glyph = jf_atlas_glyph(r->atlas, cp);
        if (glyph == NULL)
            continue;
        if (glyph->region.w != 0 && glyph->region.h != 0) {
            float placed[4], uv[4];
            jf_atlas_quad(r->atlas, glyph, pen_x, baseline, size, placed, uv);
            push(r, (loom_rect){placed[0], placed[1], placed[2], placed[3]},
                 clip, &command->mask, uv, command->text.color, 0, 0);
        }
        pen_x += glyph->advance * scale;
    }
}

static void sync_atlas(jf_renderer *r)
{
    if (!jf_atlas_dirty(r->atlas))
        return;
    const GLsizei size = (GLsizei)jf_atlas_size(r->atlas);
    glActiveTexture(ATLAS_UNIT);
    if (r->atlas_texture_size != jf_atlas_size(r->atlas)) {
        const GLuint previous = r->texture;
        glGenTextures(1, &r->texture);
        glBindTexture(GL_TEXTURE_2D, r->texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_R8, size, size);
        atlas_texture_params();
        if (r->media_texture == previous)
            r->media_texture = r->texture;
        glDeleteTextures(1, &previous);
        r->atlas_texture_size = jf_atlas_size(r->atlas);
    } else {
        glBindTexture(GL_TEXTURE_2D, r->texture);
    }
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, size, size, GL_RED, GL_UNSIGNED_BYTE,
                    jf_atlas_pixels(r->atlas));
    jf_atlas_clear_dirty(r->atlas);
    r->have_state = false;
}

void jf_renderer_draw(jf_renderer *r, const loom_command *commands, size_t count, float width,
                      float height)
{
    /* Finish all atlas growth before emitting UVs or submitting any batch. */
    for (size_t i = 0; i < count; i++)
        if (commands[i].kind == LOOM_TEXT)
            jf_atlas_prepare(r->atlas, commands[i].text.contents);
    sync_atlas(r);

    r->instance_count = 0;
    r->have_state = false;
    r->batch_start = 0;
    r->batches = 0;
    r->covered = 0;

    /* Anything else drawing into this context is not ours to assume about, so the blend
     * state is re-established once a frame; the per-batch cache is only valid from here. */
    glDisable(GL_BLEND);
    r->blend_enabled = false;

    const uniforms u = {{width, height}, {0, 0}};
    glBindVertexArray(r->vao);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, r->uniform_buffer);
    glActiveTexture(ATLAS_UNIT);

    static const float no_uv[4] = {0, 0, 0, 0};
    for (size_t i = 0; i < count; i++) {
        const loom_command *c = &commands[i];
        switch (c->kind) {
        case LOOM_RECTANGLE: {
            /* A square-cornered fill needs no corner code at all, and the background
             * alone is a full screen of them. */
            const bool square = c->rectangle.radius <= 0;
            want(r, square ? KIND_FILL : KIND_ROUND, 0, u,
                 !(square && c->rectangle.color[3] == 255) || faded(c));
            push(r, c->rect, c->clip, &c->mask, no_uv, c->rectangle.color,
                 c->rectangle.radius, 0);
            break;
        }
        case LOOM_BORDER:
            want(r, KIND_BORDER, 0, u, true);
            push(r, c->rect, c->clip, &c->mask, no_uv, c->border.color,
                 c->border.radius, c->border.width);
            break;
        case LOOM_TEXT:
            want(r, KIND_GLYPH, r->texture, u, true);
            append_text(r, c->rect, c->clip, c);
            break;
        case LOOM_IMAGE:
          want(r,
               c->image.premultiplied ? KIND_IMAGE_PREMULTIPLIED : KIND_IMAGE,
               c->image.texture != 0 ? c->image.texture : r->media_texture, u,
               true);
          push(r, c->rect, c->clip, &c->mask, c->image.uv, c->image.tint,
               c->image.radius, 0);
          break;
        }
    }
    flush(r);
}
