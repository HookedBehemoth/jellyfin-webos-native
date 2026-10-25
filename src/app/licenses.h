/*
 * What this app ships, and under what.
 *
 * Only what this package actually distributes. The ipk carries FFmpeg, Mbed
 * TLS, libass, FriBidi and HarfBuzz as shared objects of its own, and links
 * json-c and libcurl statically, so those licences travel with it - the LGPL
 * and the Apache licence both require the text, not a reference to it.
 * FreeType, libpng, SDL2, zlib and GLib are the TV's own libraries: linked
 * against, never shipped, and so not listed.
 *
 * The files are embedded rather than staged beside the binary: an installed app
 * runs as a jail uid and reads nothing it does not own, and a licence that
 * fails to open is the one case where "missing" is not an acceptable outcome. A
 * text two libraries share is embedded once and pointed at twice.
 */
#pragma once

#include <stddef.h>

#define JF_LICENSE_TEXT(symbol)                                                \
  extern const unsigned char symbol[];                                         \
  extern const size_t symbol##_len;

JF_LICENSE_TEXT(license_agpl_3_0)
JF_LICENSE_TEXT(license_lgpl_3_0)
JF_LICENSE_TEXT(license_lgpl_2_1)
JF_LICENSE_TEXT(license_apache_2_0)
JF_LICENSE_TEXT(license_libass)
JF_LICENSE_TEXT(license_harfbuzz)
JF_LICENSE_TEXT(license_mit)
JF_LICENSE_TEXT(license_curl)

typedef struct {
  const char *library;
  const char *license;
  const unsigned char *text;
} jf_license;

static const jf_license jf_licenses[] = {
    {"Jellyfin for webOS", "GNU AGPL v3", license_agpl_3_0},
    {"FFmpeg", "GNU LGPL v3", license_lgpl_3_0},
    {"Mbed TLS", "Apache 2.0", license_apache_2_0},
    {"libass", "ISC", license_libass},
    {"FriBidi", "GNU LGPL v2.1", license_lgpl_2_1},
    {"HarfBuzz", "Old MIT", license_harfbuzz},
    {"json-c", "MIT", license_mit},
    {"libcurl", "curl", license_curl},
};

#define JF_LICENSE_COUNT (sizeof(jf_licenses) / sizeof(jf_licenses[0]))
