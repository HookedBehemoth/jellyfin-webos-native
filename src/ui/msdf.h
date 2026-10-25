/*
 * Multi-channel signed distance fields from FreeType outlines, after Viktor
 * Chlumsky's msdfgen (and the gallery's msdf-zig port of it).
 *
 * The output is MTSDF: the three-channel field in RGB, whose median keeps
 * corners sharp at any scale, and the true distance in alpha, which is round
 * everywhere and so what an outline or a glow widens.
 *
 * Each texel is distance / range + 0.5, so 0.5 is the edge, above is inside,
 * and `range` pixels of distance span the whole byte.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "../jf/arena.h"

#include <ft2build.h>
#include FT_FREETYPE_H

typedef struct {
  int width, height;
  /* Pen position to the bitmap's left edge, and baseline to its top edge
   * (negative above), in pixels. */
  int left, top;
  uint8_t *rgba; /* width * height * 4; NULL for an empty outline */
} jf_msdf;

/* The outline in pixels, as FT_Load_Glyph leaves it at the face's size.
 * Everything, the output included, comes out of `scratch`, which the caller
 * rewinds once it has copied the field out: one glyph after another then
 * allocates nothing. Modifies the outline (it is translated to rasterise the
 * sign check). False only when out of memory. */
bool jf_msdf_generate(FT_Library library, FT_Outline *outline, float range,
                      jf_arena *scratch, jf_msdf *out);
