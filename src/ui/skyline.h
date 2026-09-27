/*
 * Skyline binary packing, after
 * http://clb.demon.fi/projects/even-more-rectangle-bin-packing
 *
 * One 8-bit channel, because the only thing packed here is glyph coverage. The
 * Zig original carried a depth parameter for RGBA atlases that this program
 * never had.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint16_t x, y, w, h;
} skyline_region;

typedef struct {
  struct skyline_node *nodes;
  size_t node_count, node_capacity;
  uint16_t size;
  size_t used;
  uint8_t *data;
  /* The renderer clears this once it has uploaded the texture. */
  bool dirty;
} skyline;

bool skyline_init(skyline *packer, uint16_t size);
void skyline_destroy(skyline *packer);

/* False when nothing fits; the caller then enlarges and tries again. Regions
 * carry an implicit one-pixel spacing, so two allocations never touch. */
bool skyline_alloc(skyline *packer, uint16_t w, uint16_t h,
                   skyline_region *out);
bool skyline_blit(skyline *packer, skyline_region region, const uint8_t *data,
                  size_t stride);
/* Power of two, larger than the current size. */
bool skyline_enlarge(skyline *packer, uint16_t size);
