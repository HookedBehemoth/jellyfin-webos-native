/*
 * Rasterised glyph atlas.
 *
 * Glyphs are 8-bit coverage bitmaps rendered from the TV's LG Smart UI font and packed
 * with skyline.h. ASCII is prewarmed; anything else is added on demand, falling back
 * through the other installed families for scripts the UI face does not cover.
 *
 * Bitmaps, not MSDF: this UI draws text at a handful of fixed sizes, where a distance
 * field costs three channels and a median-of-three in the shader and buys nothing.
 * `JF_ATLAS_BASE_PX` is the size glyphs are rasterised at; drawing far above it softens,
 * which is the tradeoff.
 *
 * The rasteriser is FreeType, which every TV ships. The Zig version carried its own
 * TrueType reader, compiled as a separate hard-float module because that toolchain
 * lowered every float to a helper call on this ABI - about 2,900 lines and a second build
 * target to do what FT_Load_Char does.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "skyline.h"

/* A .ttf to use instead of the system candidates; NULL for those. */
extern const char *jf_atlas_font;

#define JF_ATLAS_BASE_PX 48.0f

/* Icons, which the atlas draws itself as private-use codepoints so a label can
 * show them: each is one em square with its top at the label's top, coverage
 * like any glyph and so tinted by the label's colour. */
#define JF_ICON_BASE 0xe000u
#define JF_ICON_GLOW 0xe008u
#define JF_ICON_PLAY "\xee\x80\x80"
#define JF_ICON_PAUSE "\xee\x80\x81"
#define JF_ICON_PREVIOUS "\xee\x80\x82"
#define JF_ICON_NEXT "\xee\x80\x83"
#define JF_ICON_BACK "\xee\x80\x84"
#define JF_ICON_FORWARD "\xee\x80\x85"
#define JF_ICON_SUBTITLES "\xee\x80\x86"
#define JF_ICON_AUDIO "\xee\x80\x87"
/* A soft round falloff, for a glow behind something. */
#define JF_ICON_GLOW_TEXT "\xee\x80\x88"

typedef struct {
    skyline_region region;
    /* All three in pixels at JF_ATLAS_BASE_PX; jf_atlas_quad scales them. */
    float advance;
    float off_x; /* pen position to the left edge of the bitmap */
    float off_y; /* baseline to the top edge; negative is above the baseline */
} jf_glyph;

typedef struct jf_atlas jf_atlas;

jf_atlas *jf_atlas_create(void);
void jf_atlas_destroy(jf_atlas *atlas);

/* GLES guarantees at least 2048; the renderer replaces this with the GPU's limit. */
void jf_atlas_set_max_size(jf_atlas *atlas, uint16_t max_size);

uint16_t jf_atlas_size(const jf_atlas *atlas);
const uint8_t *jf_atlas_pixels(const jf_atlas *atlas);
bool jf_atlas_dirty(const jf_atlas *atlas);
void jf_atlas_clear_dirty(jf_atlas *atlas);

/* Rasterise whatever `text` needs that is not cached yet. */
void jf_atlas_prepare(jf_atlas *atlas, const char *text);
/* NULL only if even '?' is missing. */
const jf_glyph *jf_atlas_glyph(jf_atlas *atlas, uint32_t codepoint);
float jf_atlas_measure(jf_atlas *atlas, const char *text, float size_px);

/* Where one glyph's quad lands, and which part of the atlas it samples. */
void jf_atlas_quad(const jf_atlas *atlas, const jf_glyph *glyph, float pen_x, float baseline,
                   float size_px, float out_rect[4], float out_uv[4]);

/* One codepoint at a time, including from invalid or truncated server strings: measuring
 * and rendering must agree on advances for UTF-8 UI symbols. Returns '?' for anything
 * malformed and advances past one byte, so it always terminates. */
uint32_t jf_utf8_next(const char **text);
