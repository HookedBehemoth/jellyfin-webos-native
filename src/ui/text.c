#include "text.h"

#include "font8x16.h"

static const uint8_t *glyph(unsigned char ch) {
  if (ch < 0x20 || ch >= 0x7f)
    return NULL;
  return font8x16 + ((size_t)ch - 0x20) * TEXT_GLYPH_H;
}

void text_draw(uint8_t *dst, size_t w, size_t h, size_t x, size_t y,
               size_t scale, uint8_t value, const char *text) {
  size_t cx = x;
  for (const unsigned char *p = (const unsigned char *)text; *p != '\0';
       p++, cx += TEXT_GLYPH_W * scale) {
    if (cx + TEXT_GLYPH_W * scale > w)
      return;
    const uint8_t *bitmap = glyph(*p);
    if (bitmap == NULL)
      continue;
    for (size_t gy = 0; gy < TEXT_GLYPH_H; gy++) {
      const uint8_t bits = bitmap[gy];
      if (bits == 0)
        continue;
      for (size_t gx = 0; gx < TEXT_GLYPH_W; gx++) {
        if (((bits >> (7 - gx)) & 1) == 0)
          continue;
        for (size_t sy = 0; sy < scale; sy++) {
          const size_t py = y + gy * scale + sy;
          if (py >= h)
            continue;
          for (size_t sx = 0; sx < scale; sx++)
            dst[py * w + cx + gx * scale + sx] = value;
        }
      }
    }
  }
}
