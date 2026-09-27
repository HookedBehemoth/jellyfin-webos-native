/*
 * PNG decoding through libpng.
 *
 * Why PNG and not the JPEG the server would rather send: the TV ships
 * libjpeg.so.62 and a development machine ships libjpeg.so.8. Those are
 * different ABIs for the same name. libpng16 is on both (1.6.39 on the
 * TV, 1.6.58 here) and Jellyfin re-encodes to PNG on request, so one decoder
 * covers both.
 *
 * libpng's *simplified* API is used: png_image is a fixed, version-checked
 * struct and errors come back as a zero return rather than through setjmp.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
  uint32_t width, height;
  uint8_t *rgb; /* width * height * 3 bytes, malloc'd; free() it */
} jf_image;

bool jf_image_decode(const uint8_t *data, size_t size, jf_image *out);
void jf_image_free(jf_image *image);
/* Logs the libpng version once, so a decode failure on device can be placed. */
void jf_image_report_version(void);
