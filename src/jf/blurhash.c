#include "blurhash.h"

#include <math.h>
#include <string.h>

static const char digits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklm"
                             "nopqrstuvwxyz#$%*+,-.:;=?@[]^_{|}~";

static bool decode83(const char *text, size_t length, int *out) {
  int value = 0;
  for (size_t i = 0; i < length; i++) {
    const char *digit = text[i] != '\0' ? strchr(digits, text[i]) : NULL;
    if (digit == NULL)
      return false;
    value = value * 83 + (int)(digit - digits);
  }
  *out = value;
  return true;
}

static float to_linear(int value) {
  const float v = (float)value / 255.0f;
  return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

static uint8_t to_srgb(float value) {
  const float v = value < 0 ? 0 : value > 1 ? 1 : value;
  const float s =
      v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
  return (uint8_t)(s * 255.0f + 0.5f);
}

static float signed_square(float value) {
  return value < 0 ? -value * value : value * value;
}

bool jf_blurhash_decode(const char *hash, uint32_t width, uint32_t height,
                        uint8_t *rgb) {
  int size = 0, quantised = 0;
  if (hash == NULL || width == 0 || height == 0 || strlen(hash) < 6 ||
      !decode83(hash, 1, &size) || !decode83(hash + 1, 1, &quantised))
    return false;
  const int nx = size % 9 + 1, ny = size / 9 + 1;
  if (strlen(hash) != (size_t)(4 + 2 * nx * ny))
    return false;

  float colors[81][3];
  int value = 0;
  if (!decode83(hash + 2, 4, &value))
    return false;
  colors[0][0] = to_linear(value >> 16);
  colors[0][1] = to_linear((value >> 8) & 255);
  colors[0][2] = to_linear(value & 255);
  const float max = (float)(quantised + 1) / 166.0f;
  for (int i = 1; i < nx * ny; i++) {
    if (!decode83(hash + 4 + i * 2, 2, &value))
      return false;
    colors[i][0] = signed_square((float)(value / (19 * 19) - 9) / 9.0f) * max;
    colors[i][1] = signed_square((float)(value / 19 % 19 - 9) / 9.0f) * max;
    colors[i][2] = signed_square((float)(value % 19 - 9) / 9.0f) * max;
  }

  const float pi = 3.14159265f;
  for (uint32_t y = 0; y < height; y++) {
    float cos_y[9];
    for (int j = 0; j < ny; j++)
      cos_y[j] = cosf(pi * (float)y * (float)j / (float)height);
    for (uint32_t x = 0; x < width; x++) {
      float pixel[3] = {0, 0, 0};
      for (int i = 0; i < nx; i++) {
        const float cos_x = cosf(pi * (float)x * (float)i / (float)width);
        for (int j = 0; j < ny; j++) {
          const float basis = cos_x * cos_y[j];
          const float *c = colors[j * nx + i];
          pixel[0] += c[0] * basis;
          pixel[1] += c[1] * basis;
          pixel[2] += c[2] * basis;
        }
      }
      uint8_t *out = rgb + ((size_t)y * width + x) * 3;
      out[0] = to_srgb(pixel[0]);
      out[1] = to_srgb(pixel[1]);
      out[2] = to_srgb(pixel[2]);
    }
  }
  return true;
}
