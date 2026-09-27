#include "image.h"

#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jf_image_report_version(void) {
  const png_uint_32 version = png_access_version_number();
  fprintf(stderr, "libpng %u.%u.%u\n", version / 10000, version / 100 % 100,
          version % 100);
}

bool jf_image_decode(const uint8_t *data, size_t size, jf_image *out) {
  png_image png;
  memset(&png, 0, sizeof(png));
  png.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_memory(&png, data, size))
    return false;
  /* From here libpng holds an allocation inside `png`, released either by
   * png_image_finish_read or explicitly. */
  png.format = PNG_FORMAT_RGB;
  const size_t bytes = (size_t)png.width * png.height * 3;
  if (bytes == 0) {
    png_image_free(&png);
    return false;
  }
  uint8_t *rgb = malloc(bytes);
  if (rgb == NULL) {
    png_image_free(&png);
    return false;
  }
  /* row_stride 0 means "tightly packed", which is what the texture upload
   * wants; no colormap and no background, since the format has no alpha. */
  if (!png_image_finish_read(&png, NULL, rgb, 0, NULL)) {
    free(rgb);
    png_image_free(&png);
    return false;
  }
  out->width = png.width;
  out->height = png.height;
  out->rgb = rgb;
  return true;
}

void jf_image_free(jf_image *image) {
  free(image->rgb);
  image->rgb = NULL;
  image->width = 0;
  image->height = 0;
}
