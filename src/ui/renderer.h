/*
 * OpenGL ES backend for the trimmed loom command stream.
 *
 * Every rectangle, border, image and glyph is one instance of the same unit quad, and
 * per-instance clipping keeps a virtual list inside the batch.
 *
 * There is no bindless texture support on this GPU, so the batch is bounded by its
 * *bindings*, not by the command count: instances accumulate until a command needs a
 * different texture or different uniforms, and only then does the batch flush. Untextured
 * commands - fills and borders, the bulk of a UI - join whichever batch is open instead of
 * breaking it, so alternating rect/glyph/rect costs one draw, not three.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "glyph_atlas.h"
#include "loom.h"

typedef struct jf_renderer jf_renderer;

/* `media` is an optional default texture for loom_image, RGB8 and width*height*3 bytes,
 * for an application whose artwork is known up front. An application that loads images at
 * runtime passes NULL and makes its own with jf_renderer_create_texture. */
jf_renderer *jf_renderer_create(const uint8_t *media, uint32_t media_width, uint32_t media_height);
void jf_renderer_destroy(jf_renderer *renderer);

void jf_renderer_draw(jf_renderer *renderer, const loom_command *commands, size_t count,
                      float width, float height);

float jf_renderer_measure(jf_renderer *renderer, const char *text, float size);
jf_atlas *jf_renderer_atlas(jf_renderer *renderer);

/* A texture of its own for one image.
 *
 * Deliberately not an atlas: artwork arrives at whatever size the server felt like
 * returning, an atlas would have to crop every image to a fixed tile, and recycling tiles
 * in a scrolling grid means uploading over a tile something else may still be drawing
 * from. The cost is one draw call per distinct texture on screen, which is what the
 * batcher already does for a binding change. */
uint32_t jf_renderer_create_texture(jf_renderer *renderer, uint32_t width, uint32_t height,
                                    const uint8_t *rgb);
/* The same, with an alpha channel, for an overlay that has to let the picture
 * behind it through - the subtitle image, which arrives already composited.
 * Storage is immutable, so a changed overlay is a new texture rather than a
 * subimage upload; one a second, at the rate dialogue changes, is not worth a
 * mutable-format special case. */
uint32_t jf_renderer_create_rgba_texture(jf_renderer *renderer, uint32_t width,
                                         uint32_t height, const uint8_t *rgba);
void jf_renderer_destroy_texture(jf_renderer *renderer, uint32_t id);

/* Draw calls issued by the last draw, which is the number worth watching. */
uint32_t jf_renderer_batches(const jf_renderer *renderer);
/* Instances the last draw submitted. */
size_t jf_renderer_instances(const jf_renderer *renderer);
/* Fragments the last frame actually rasterised, after the vertex shader's geometric
 * clipping. Divided by the screen area this is the overdraw factor. */
double jf_renderer_covered(const jf_renderer *renderer);
