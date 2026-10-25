/*
 * Distance-field glyph atlas.
 *
 * Glyphs are MTSDF (msdf.h) generated from the TV's LG Smart UI font and
 * packed with skyline.h. ASCII is prewarmed; anything else is added on demand,
 * falling back through the other installed families for scripts the UI face
 * does not cover.
 *
 * A field rather than coverage because subtitles are drawn at any size, with
 * an outline and faux bold that are both just a different threshold of the
 * same texels. `JF_ATLAS_BASE_PX` is the size fields are generated at and
 * `JF_ATLAS_RANGE` how many of its pixels the field spans, which bounds how
 * wide an outline can get.
 *
 * FreeType reads the outlines, which every TV ships.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "skyline.h"

/* A .ttf to use instead of the system candidates; NULL for those. */
extern const char *jf_atlas_font;

#define JF_ATLAS_BASE_PX 48.0f
#define JF_ATLAS_RANGE 12.0f

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
/* A soft round falloff, for a glow behind something. Coverage, not a
 * distance: the renderer draws it as it is. */
#define JF_ICON_GLOW_TEXT "\xee\x80\x88"
/* A checkbox, ticked and not. */
#define JF_ICON_CHECKED "\xee\x80\x89"
#define JF_ICON_UNCHECKED "\xee\x80\x8a"
#define JF_ICON_LAST 0xe00au

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

/* GLES guarantees at least 2048; the renderer replaces this with the GPU's
 * limit. */
void jf_atlas_set_max_size(jf_atlas *atlas, uint16_t max_size);

uint16_t jf_atlas_size(const jf_atlas *atlas);
const uint8_t *jf_atlas_pixels(const jf_atlas *atlas);
bool jf_atlas_dirty(const jf_atlas *atlas);
/* Rows [top, bottom) changed since the last clear. */
void jf_atlas_dirty_rows(const jf_atlas *atlas, uint16_t *top,
                         uint16_t *bottom);
void jf_atlas_clear_dirty(jf_atlas *atlas);

/* Rasterise whatever `text` needs that is not cached yet. */
void jf_atlas_prepare(jf_atlas *atlas, const char *text);
/* NULL only if even '?' is missing. */
const jf_glyph *jf_atlas_glyph(jf_atlas *atlas, uint32_t codepoint);
float jf_atlas_measure(jf_atlas *atlas, const char *text, float size_px);

/* ------------------------------------------------------------- shaping
 *
 * For text HarfBuzz lays out (subtitles): scripts whose marks stack or whose
 * letters join need the font's own positioning, which a glyph per codepoint
 * cannot give. Faces are numbered in the order the atlas loaded them. */
typedef struct hb_font_t hb_font_t;

/* The first face with `codepoint`, loading an installed fallback that has it
 * when none does; -1 when no font has it. */
int jf_atlas_face_for(jf_atlas *atlas, uint32_t codepoint);
bool jf_atlas_face_has(jf_atlas *atlas, int face, uint32_t codepoint);
/* HarfBuzz's view of a face, in 26.6 pixels at JF_ATLAS_BASE_PX. */
hb_font_t *jf_atlas_shaper(jf_atlas *atlas, int face);
/* A glyph by the face's own index, as shaping hands them out. NULL when the
 * face cannot draw it. */
const jf_glyph *jf_atlas_glyph_index(jf_atlas *atlas, int face, uint32_t index);

/* Where one glyph's quad lands, and which part of the atlas it samples. */
void jf_atlas_quad(const jf_atlas *atlas, const jf_glyph *glyph, float pen_x,
                   float baseline, float size_px, float out_rect[4],
                   float out_uv[4]);

/* One codepoint at a time, including from invalid or truncated server strings:
 * measuring and rendering must agree on advances for UTF-8 UI symbols. Returns
 * '?' for anything malformed and advances past one byte, so it always
 * terminates. */
uint32_t jf_utf8_next(const char **text);
