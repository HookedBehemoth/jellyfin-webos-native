/*
 * An 8x16 bitmap font and a blitter, for the two rendering probes' overlays.
 *
 * assets/font8x16.bin is the ASCII range of Terminus (OFL-1.1), extracted from
 * Lat2-Terminus16.psfu as 95 glyphs of 16 bytes, one bit per pixel, MSB
 * leftmost.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define TEXT_GLYPH_W 8
#define TEXT_GLYPH_H 16

/* Blit `text` at (x, y) into a `w` x `h` buffer of 8-bit coverage, `scale`x
 * magnified. Anything off the right or bottom edge is clipped. */
void text_draw(uint8_t *dst, size_t w, size_t h, size_t x, size_t y,
               size_t scale, uint8_t value, const char *text);
