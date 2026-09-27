#include "glyph_atlas.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <hb-ft.h>

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/os.h"
#include "font_index.h"
#include "msdf.h"

#define MAX_FACES 32
#define TABLE_CAPACITY                                                         \
  16384 /* power of two; ~8k glyphs, a CJK track, at half full */

/* Open addressing keyed by codepoint, or for shaped text by face and glyph
 * index (GLYPH_KEY). A miss is cached too - an unsupported character must not
 * be re-rasterised every frame - so entries carry `found`. */
#define GLYPH_KEY(face, index) (0x80000000u | (uint32_t)(face) << 20 | (index))
typedef struct {
  uint32_t codepoint;
  bool occupied;
  bool found;
  jf_glyph glyph;
} entry;

struct jf_atlas {
  FT_Library library;
  FT_Face faces[MAX_FACES];
  const char *face_paths[MAX_FACES];
  hb_font_t *shapers[MAX_FACES]; /* made on first use */
  size_t face_count;
  /* Distance-field scratch, rewound after every glyph. */
  jf_arena scratch;
  const char *primary_path;
  skyline packer;
  uint16_t max_size;
  entry table[TABLE_CAPACITY];
};

uint32_t jf_utf8_next(const char **text) {
  const unsigned char *s = (const unsigned char *)*text;
  if (s[0] == 0)
    return 0;
  unsigned length;
  uint32_t value;
  if (s[0] < 0x80) {
    *text += 1;
    return s[0];
  } else if ((s[0] & 0xe0) == 0xc0) {
    length = 2;
    value = s[0] & 0x1fu;
  } else if ((s[0] & 0xf0) == 0xe0) {
    length = 3;
    value = s[0] & 0x0fu;
  } else if ((s[0] & 0xf8) == 0xf0) {
    length = 4;
    value = s[0] & 0x07u;
  } else {
    *text += 1;
    return '?';
  }
  for (unsigned i = 1; i < length; i++) {
    if ((s[i] & 0xc0) != 0x80) { /* truncated or malformed */
      *text += 1;
      return '?';
    }
    value = (value << 6) | (s[i] & 0x3fu);
  }
  *text += length;
  return value;
}

/* ------------------------------------------------------------------- faces */

const char *jf_atlas_font;

static bool add_face(jf_atlas *atlas, const char *path) {
  if (atlas->face_count == MAX_FACES)
    return false;
  FT_Face face;
  if (FT_New_Face(atlas->library, path, 0, &face) != 0)
    return false;
  if (FT_Set_Pixel_Sizes(face, 0, (FT_UInt)JF_ATLAS_BASE_PX) != 0 ||
      !FT_IS_SCALABLE(face)) {
    FT_Done_Face(face);
    return false;
  }
  atlas->face_paths[atlas->face_count] = path;
  atlas->faces[atlas->face_count++] = face;
  return true;
}

/* -------------------------------------------------------------- glyph cache */

static entry *slot_for(jf_atlas *atlas, uint32_t codepoint) {
  /* Knuth multiplicative; codepoints are dense and this table is generous. */
  size_t index = (codepoint * 2654435761u) & (TABLE_CAPACITY - 1);
  for (size_t probe = 0; probe < TABLE_CAPACITY; probe++) {
    entry *e = &atlas->table[(index + probe) & (TABLE_CAPACITY - 1)];
    if (!e->occupied || e->codepoint == codepoint)
      return e;
  }
  return NULL;
}

/* Pack one RGBA field into the atlas. */
static bool pack(jf_atlas *atlas, const uint8_t *rgba, unsigned width,
                 unsigned height, float advance, float off_x, float off_y,
                 jf_glyph *out) {
  if (width == 0 ||
      height == 0) { /* space and friends: real advance, no pixels */
    *out = (jf_glyph){{0, 0, 0, 0}, advance, 0, 0};
    return true;
  }

  /* The field already fades to "far outside" at its border, so the gutter
   * the packer leaves - zero, the same thing - is all the padding it needs. */
  skyline_region region;
  while (!skyline_alloc(&atlas->packer, (uint16_t)width, (uint16_t)height,
                        &region)) {
    if (atlas->packer.size >= atlas->max_size)
      return false;
    const uint32_t doubled = (uint32_t)atlas->packer.size * 2;
    const uint16_t next =
        doubled < atlas->max_size ? (uint16_t)doubled : atlas->max_size;
    if (!skyline_enlarge(&atlas->packer, next))
      return false;
  }
  skyline_blit(&atlas->packer, region, rgba, (size_t)width * SKYLINE_BPP);
  *out = (jf_glyph){region, advance, off_x, off_y};
  return true;
}

/* One codepoint's field from one face into the atlas. Unhinted, since it is
 * drawn at every size but this one. */
static bool rasterise_index(jf_atlas *atlas, FT_Face face, FT_UInt index,
                            jf_glyph *out) {
  if (FT_Load_Glyph(face, index, FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
    return false;
  const FT_GlyphSlot slot = face->glyph;
  /* A colour-bitmap face (emoji) has no outline to measure. */
  if (slot->format != FT_GLYPH_FORMAT_OUTLINE)
    return false;
  jf_msdf field;
  const bool ok = jf_msdf_generate(atlas->library, &slot->outline,
                                   JF_ATLAS_RANGE, &atlas->scratch, &field) &&
                  pack(atlas, field.rgba, (unsigned)field.width,
                       (unsigned)field.height, (float)slot->advance.x / 64.0f,
                       (float)field.left, (float)field.top, out);
  jf_arena_rewind(&atlas->scratch);
  return ok;
}

static bool rasterise(jf_atlas *atlas, FT_Face face, uint32_t codepoint,
                      jf_glyph *out) {
  const FT_UInt index = FT_Get_Char_Index(face, codepoint);
  return index != 0 && rasterise_index(atlas, face, index, out);
}

/* ------------------------------------------------------------------- icons */

/* Distances on a 24-unit grid, Material's; negative inside. */
static float box(float px, float py, float x, float y, float w, float h,
                 float r) {
  const float qx = fabsf(px - x - w / 2) - (w / 2 - r);
  const float qy = fabsf(py - y - h / 2) - (h / 2 - r);
  return hypotf(fmaxf(qx, 0), fmaxf(qy, 0)) + fminf(fmaxf(qx, qy), 0) - r;
}

/* A convex polygon, either winding. Not exact past a corner, which only rounds
 * the antialiasing there. */
static float polygon(float px, float py, const float *xy, int n) {
  float area = 0;
  for (int i = 0; i < n; i++) {
    const int j = (i + 1) % n;
    area += xy[2 * i] * xy[2 * j + 1] - xy[2 * j] * xy[2 * i + 1];
  }
  float d = -INFINITY;
  for (int i = 0; i < n; i++) {
    const int j = (i + 1) % n;
    const float ex = xy[2 * j] - xy[2 * i], ey = xy[2 * j + 1] - xy[2 * i + 1];
    const float length = hypotf(ex, ey);
    const float side =
        ((px - xy[2 * i]) * ey - (py - xy[2 * i + 1]) * ex) / length;
    d = fmaxf(d, area > 0 ? side : -side);
  }
  return d;
}

/* A stroke of `width` along a circle, from angle `from` sweeping `sweep`
 * radians clockwise on screen, with round ends. */
static float arc(float px, float py, float cx, float cy, float r, float width,
                 float from, float sweep) {
  const float dx = px - cx, dy = py - cy;
  const float into =
      fmodf(atan2f(dy, dx) - from + 4 * (float)M_PI, 2 * (float)M_PI);
  if (into <= sweep)
    return fabsf(hypotf(dx, dy) - r) - width / 2;
  const float ax = cx + r * cosf(from), ay = cy + r * sinf(from);
  const float bx = cx + r * cosf(from + sweep),
              by = cy + r * sinf(from + sweep);
  return fminf(hypotf(px - ax, py - ay), hypotf(px - bx, py - by)) - width / 2;
}

static float icon_distance(uint32_t codepoint, float x, float y) {
  const float pi = (float)M_PI;
  switch (codepoint) {
  case JF_ICON_BASE + 0: { /* play */
    static const float shape[] = {8, 5, 19, 12, 8, 19};
    return polygon(x, y, shape, 3);
  }
  case JF_ICON_BASE + 1: /* pause */
    return fminf(box(x, y, 6, 5, 4, 14, 1), box(x, y, 14, 5, 4, 14, 1));
  case JF_ICON_BASE + 2: { /* previous */
    static const float shape[] = {18, 6, 18, 18, 9.5f, 12};
    return fminf(box(x, y, 6, 6, 2, 12, 0.5f), polygon(x, y, shape, 3));
  }
  case JF_ICON_BASE + 3: { /* next */
    static const float shape[] = {6, 6, 14.5f, 12, 6, 18};
    return fminf(box(x, y, 16, 6, 2, 12, 0.5f), polygon(x, y, shape, 3));
  }
  case JF_ICON_BASE + 4:   /* back, a number drawn inside */
  case JF_ICON_BASE + 5: { /* forward, its mirror */
    if (codepoint == JF_ICON_BASE + 5)
      x = 24 - x;
    static const float head[] = {12.5f, 1.5f, 12.5f, 10.5f, 7.5f, 6};
    return fminf(arc(x, y, 12, 13.5f, 8, 2, -pi / 2 + 0.05f, 1.5f * pi),
                 polygon(x, y, head, 3));
  }
  case JF_ICON_BASE + 6: /* subtitles: a card with lines cut out */
    return fmaxf(box(x, y, 2, 4, 20, 16, 2),
                 -fminf(fminf(box(x, y, 5, 11.5f, 4, 2, 0.5f),
                              box(x, y, 11, 11.5f, 8, 2, 0.5f)),
                        fminf(box(x, y, 5, 15, 9, 2, 0.5f),
                              box(x, y, 15, 15, 4, 2, 0.5f))));
  case JF_ICON_BASE + 7: { /* audio: a speaker */
    static const float cone[] = {6.5f, 9, 12, 4, 12, 20, 6.5f, 15};
    return fminf(fminf(box(x, y, 3, 9, 4, 6, 0.5f), polygon(x, y, cone, 4)),
                 fminf(arc(x, y, 12, 12, 4.5f, 2, -0.85f, 1.7f),
                       arc(x, y, 12, 12, 8, 2, -0.9f, 1.8f)));
  }
  }
  return INFINITY;
}

/* Icons are distances already, so they go into all four channels as they are.
 * The glow is coverage instead, drawn as it is (see JF_ICON_GLOW). */
static bool rasterise_icon(jf_atlas *atlas, uint32_t codepoint, jf_glyph *out) {
  enum {
    PAD = (int)(JF_ATLAS_RANGE / 2),
    SIZE = (int)JF_ATLAS_BASE_PX + 2 * PAD
  };
  static uint8_t bitmap[SIZE * SIZE * 4];
  const float unit = 24.0f / JF_ATLAS_BASE_PX;
  for (int row = 0; row < SIZE; row++)
    for (int column = 0; column < SIZE; column++) {
      const float x = (column - PAD + 0.5f) * unit;
      const float y = (row - PAD + 0.5f) * unit;
      float value;
      if (codepoint == JF_ICON_GLOW) {
        /* A soft disc falling to nothing at the edge of the square. */
        const float r = fminf(hypotf(x - 12, y - 12) / 12, 1);
        value = powf(1 - r * r, 2.5f);
      } else {
        value = -icon_distance(codepoint, x, y) / unit / JF_ATLAS_RANGE + 0.5f;
        value = fminf(fmaxf(value, 0), 1);
      }
      memset(&bitmap[(row * SIZE + column) * 4], (int)lrintf(value * 255), 4);
    }
  /* One em square with its top at the label's top. */
  return pack(atlas, bitmap, SIZE, SIZE, JF_ATLAS_BASE_PX, -PAD,
              -JF_ATLAS_BASE_PX - PAD, out);
}

int jf_atlas_face_for(jf_atlas *atlas, uint32_t codepoint);

static void ensure(jf_atlas *atlas, uint32_t codepoint) {
  entry *slot = slot_for(atlas, codepoint);
  if (slot == NULL || slot->occupied)
    return;
  slot->occupied = true;
  slot->codepoint = codepoint;
  if (codepoint >= JF_ICON_BASE && codepoint <= JF_ICON_GLOW) {
    slot->found = rasterise_icon(atlas, codepoint, &slot->glyph);
    return;
  }
  if ((codepoint & 0x80000000u) != 0) {
    const size_t face = (codepoint >> 20) & 0x7ff;
    slot->found = face < atlas->face_count &&
                  rasterise_index(atlas, atlas->faces[face],
                                  codepoint & 0xfffff, &slot->glyph);
    return;
  }
  for (size_t i = 0; i < atlas->face_count; i++)
    if (rasterise(atlas, atlas->faces[i], codepoint, &slot->glyph)) {
      slot->found = true;
      return;
    }
  const int face = jf_atlas_face_for(atlas, codepoint);
  if (face >= 0)
    slot->found = rasterise(atlas, atlas->faces[face], codepoint, &slot->glyph);
}

int jf_atlas_face_for(jf_atlas *atlas, uint32_t codepoint) {
  for (size_t i = 0; i < atlas->face_count; i++)
    if (FT_Get_Char_Index(atlas->faces[i], codepoint) != 0)
      return (int)i;
  /* A script none of the faces so far has: whichever installed font does
   * joins them. Controls and the like are in no font; not worth a lookup. */
  if (codepoint < 0x20)
    return -1;
  const char *path = jf_font_for(codepoint);
  if (path == NULL)
    return -1;
  for (size_t i = 0; i < atlas->face_count; i++)
    if (strcmp(atlas->face_paths[i], path) == 0)
      return -1; /* has it by its map, and still not by its face */
  return add_face(atlas, path) ? (int)atlas->face_count - 1 : -1;
}

bool jf_atlas_face_has(jf_atlas *atlas, int face, uint32_t codepoint) {
  return face >= 0 && (size_t)face < atlas->face_count &&
         FT_Get_Char_Index(atlas->faces[face], codepoint) != 0;
}

hb_font_t *jf_atlas_shaper(jf_atlas *atlas, int face) {
  if (face < 0 || (size_t)face >= atlas->face_count)
    return NULL;
  if (atlas->shapers[face] == NULL) {
    atlas->shapers[face] = hb_ft_font_create_referenced(atlas->faces[face]);
    /* The same unhinted outlines the fields are made from. */
    hb_ft_font_set_load_flags(atlas->shapers[face],
                              FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP);
  }
  return atlas->shapers[face];
}

const jf_glyph *jf_atlas_glyph_index(jf_atlas *atlas, int face,
                                     uint32_t index) {
  if (face < 0 || index > 0xfffff)
    return NULL;
  const uint32_t key = GLYPH_KEY(face, index);
  ensure(atlas, key);
  const entry *slot = slot_for(atlas, key);
  return slot != NULL && slot->occupied && slot->found ? &slot->glyph : NULL;
}

/* ------------------------------------------------------------------ public */

jf_atlas *jf_atlas_create(void) {
  jf_atlas *atlas = calloc(1, sizeof(*atlas));
  if (atlas == NULL)
    return NULL;
  atlas->max_size = 2048;
  if (FT_Init_FreeType(&atlas->library) != 0) {
    free(atlas);
    return NULL;
  }

  if (jf_atlas_font != NULL && add_face(atlas, jf_atlas_font)) {
    atlas->primary_path = jf_atlas_font;
  } else {
    size_t count = 0;
    const char *const *paths = jf_os_font_candidates(&count);
    for (size_t i = 0; i < count; i++) {
      if (add_face(atlas, paths[i])) {
        atlas->primary_path = paths[i];
        break;
      }
    }
  }
  if (atlas->face_count == 0) {
    fprintf(stderr, "No UI font found; set font in the [ui] section of "
                    "preferences.ini\n");
    FT_Done_FreeType(atlas->library);
    free(atlas);
    return NULL;
  }
  if (!skyline_init(&atlas->packer, 1024)) {
    jf_atlas_destroy(atlas);
    return NULL;
  }
  for (uint32_t cp = 32; cp < 127; cp++)
    ensure(atlas, cp);
  skyline_touch(&atlas->packer, 0, atlas->packer.size);
  return atlas;
}

void jf_atlas_destroy(jf_atlas *atlas) {
  if (atlas == NULL)
    return;
  for (size_t i = 0; i < atlas->face_count; i++) {
    if (atlas->shapers[i] != NULL)
      hb_font_destroy(atlas->shapers[i]);
    FT_Done_Face(atlas->faces[i]);
  }
  FT_Done_FreeType(atlas->library);
  skyline_destroy(&atlas->packer);
  jf_arena_destroy(&atlas->scratch);
  free(atlas);
}

void jf_atlas_set_max_size(jf_atlas *atlas, uint16_t max_size) {
  if (max_size >= atlas->packer.size)
    atlas->max_size = max_size;
}

uint16_t jf_atlas_size(const jf_atlas *atlas) { return atlas->packer.size; }
const uint8_t *jf_atlas_pixels(const jf_atlas *atlas) {
  return atlas->packer.data;
}
bool jf_atlas_dirty(const jf_atlas *atlas) { return atlas->packer.dirty; }
void jf_atlas_dirty_rows(const jf_atlas *atlas, uint16_t *top,
                         uint16_t *bottom) {
  *top = atlas->packer.dirty_top;
  *bottom = atlas->packer.dirty_bottom;
}
void jf_atlas_clear_dirty(jf_atlas *atlas) { atlas->packer.dirty = false; }

void jf_atlas_prepare(jf_atlas *atlas, const char *text) {
  for (uint32_t cp = jf_utf8_next(&text); cp != 0; cp = jf_utf8_next(&text))
    ensure(atlas, cp);
}

const jf_glyph *jf_atlas_glyph(jf_atlas *atlas, uint32_t codepoint) {
  ensure(atlas, codepoint);
  const entry *slot = slot_for(atlas, codepoint);
  if (slot != NULL && slot->occupied && slot->found)
    return &slot->glyph;
  if (codepoint == '?')
    return NULL;
  return jf_atlas_glyph(atlas, '?');
}

float jf_atlas_measure(jf_atlas *atlas, const char *text, float size_px) {
  const float scale = size_px / JF_ATLAS_BASE_PX;
  float width = 0;
  for (uint32_t cp = jf_utf8_next(&text); cp != 0; cp = jf_utf8_next(&text)) {
    const jf_glyph *glyph = jf_atlas_glyph(atlas, cp);
    if (glyph != NULL)
      width += glyph->advance * scale;
  }
  return width;
}

void jf_atlas_quad(const jf_atlas *atlas, const jf_glyph *glyph, float pen_x,
                   float baseline, float size_px, float out_rect[4],
                   float out_uv[4]) {
  const float scale = size_px / JF_ATLAS_BASE_PX;
  const float extent = (float)atlas->packer.size;
  out_rect[0] = pen_x + glyph->off_x * scale;
  out_rect[1] = baseline + glyph->off_y * scale;
  out_rect[2] = (float)glyph->region.w * scale;
  out_rect[3] = (float)glyph->region.h * scale;
  out_uv[0] = (float)glyph->region.x / extent;
  out_uv[1] = (float)glyph->region.y / extent;
  out_uv[2] = (float)(glyph->region.x + glyph->region.w) / extent;
  out_uv[3] = (float)(glyph->region.y + glyph->region.h) / extent;
}
