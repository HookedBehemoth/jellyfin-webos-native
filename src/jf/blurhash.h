/* Decodes the blurhash placeholders Jellyfin sends in ImageBlurHashes. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Fills width x height RGB pixels. False, with rgb untouched, for a malformed
 * hash. */
bool jf_blurhash_decode(const char *hash, uint32_t width, uint32_t height,
                        uint8_t *rgb);
