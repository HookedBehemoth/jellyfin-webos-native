/*
 * Which installed font has a character.
 *
 * The UI face covers Latin; the TV carries separate faces for Thai, Arabic,
 * the Indic scripts, CJK and more (docs/fonts.md), and a subtitle track needs
 * whichever of them its language does. Every font under jf_os_font_dirs is
 * read once - its character map, as ranges - and after that finding a face
 * for a character is a binary search per font.
 *
 * Faces that look like a family's regular weight come before its bold,
 * italic and condensed cuts.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Starts reading the fonts on a thread of its own, so the first lookup does
 * not wait for it. Lookups before then wait. */
void jf_font_index_warm(void);

/* A font with `codepoint`, or NULL. The path stays valid for the process. */
const char *jf_font_for(uint32_t codepoint);
/* Whether the font at `path` has `codepoint`; false for a font not indexed. */
bool jf_font_covers(const char *path, uint32_t codepoint);
