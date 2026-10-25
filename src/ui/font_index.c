#include "font_index.h"

#include <ft2build.h>
#include FT_FREETYPE_H

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../jf/arena.h"
#include "../platform/os.h"

typedef struct {
  const char *path;
  uint32_t (*ranges)[2]; /* [first, last], ascending */
  size_t count;
  int rank; /* lower first */
} font;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t scanned = PTHREAD_COND_INITIALIZER;
static enum { IDLE, SCANNING, READY } state;
/* Paths and ranges, for the life of the process. */
static jf_arena arena;
static font *fonts;
static size_t font_count, font_capacity;

static bool contains_nocase(const char *text, const char *word) {
  for (size_t n = strlen(word); *text != '\0'; text++)
    if (strncasecmp(text, word, n) == 0)
      return true;
  return false;
}

/* A regular weight before the family's other cuts, which cover the same
 * characters less legibly. */
static int rank_of(const char *path) {
  static const char *const cuts[] = {
      "bold", "italic", "oblique", "light", "thin",   "black", "condensed",
      "mono", "semi",   "medium",  "heavy", "narrow", "extra"};
  const char *name = strrchr(path, '/');
  name = name != NULL ? name + 1 : path;
  int rank = strncmp(name, "LG_Smart_UI", 11) == 0 ? 1 : 2;
  /* Sans before serif, as the UI face is. */
  if (contains_nocase(name, "serif") && !contains_nocase(name, "sans"))
    rank += 5;
  for (size_t i = 0; i < sizeof(cuts) / sizeof(*cuts); i++)
    if (contains_nocase(name, cuts[i]))
      return rank + 10;
  return rank;
}

/* The font's character map as ranges, or false for a face this cannot use:
 * not scalable, or colour bitmaps (emoji), which the field generator has no
 * outline for. Lock not held. */
static bool read_font(FT_Library library, const char *path, font *out) {
  FT_Face face;
  if (FT_New_Face(library, path, 0, &face) != 0)
    return false;
  bool ok = FT_IS_SCALABLE(face) && !FT_HAS_COLOR(face) &&
            FT_Select_Charmap(face, FT_ENCODING_UNICODE) == 0;
  uint32_t (*ranges)[2] = NULL;
  size_t count = 0, capacity = 0;
  FT_UInt index;
  for (FT_ULong c = ok ? FT_Get_First_Char(face, &index) : 0; ok && index != 0;
       c = FT_Get_Next_Char(face, c, &index)) {
    if (count > 0 && ranges[count - 1][1] + 1 == c) {
      ranges[count - 1][1] = (uint32_t)c;
      continue;
    }
    if (count == capacity) {
      capacity = capacity ? capacity * 2 : 64;
      uint32_t (*grown)[2] = realloc(ranges, capacity * sizeof(*ranges));
      if (grown == NULL) {
        ok = false;
        break;
      }
      ranges = grown;
    }
    ranges[count][0] = ranges[count][1] = (uint32_t)c;
    count++;
  }
  FT_Done_Face(face);
  ok = ok && count > 0;
  if (ok) {
    pthread_mutex_lock(&lock);
    out->path = jf_arena_strdup(&arena, path);
    out->ranges = jf_arena_alloc(&arena, count * sizeof(*ranges));
    pthread_mutex_unlock(&lock);
    ok = out->path != NULL && out->ranges != NULL;
    if (ok) {
      memcpy(out->ranges, ranges, count * sizeof(*ranges));
      out->count = count;
      out->rank = rank_of(path);
    }
  }
  free(ranges);
  return ok;
}

/* Keeps the list sorted by rank, a new font after its equals. Lock held. */
static void insert(const font *f) {
  for (size_t i = 0; i < font_count; i++)
    if (strcmp(fonts[i].path, f->path) == 0)
      return;
  if (font_count == font_capacity) {
    const size_t capacity = font_capacity ? font_capacity * 2 : 64;
    font *grown = realloc(fonts, capacity * sizeof(*grown));
    if (grown == NULL)
      return;
    fonts = grown;
    font_capacity = capacity;
  }
  size_t at = font_count;
  while (at > 0 && fonts[at - 1].rank > f->rank)
    at--;
  memmove(fonts + at + 1, fonts + at, (font_count - at) * sizeof(*fonts));
  fonts[at] = *f;
  font_count++;
}

static bool is_font_file(const char *name) {
  const char *dot = strrchr(name, '.');
  return dot != NULL &&
         (strcasecmp(dot, ".ttf") == 0 || strcasecmp(dot, ".otf") == 0 ||
          strcasecmp(dot, ".ttc") == 0);
}

static void scan_dir(FT_Library library, const char *dir, int depth) {
  DIR *d = opendir(dir);
  if (d == NULL)
    return;
  for (struct dirent *e; (e = readdir(d)) != NULL;) {
    if (e->d_name[0] == '.')
      continue;
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s", dir, e->d_name) >=
        (int)sizeof(path))
      continue; /* cut short, it would name some other file */
    if (is_font_file(e->d_name)) {
      font f;
      if (read_font(library, path, &f)) {
        pthread_mutex_lock(&lock);
        insert(&f);
        pthread_mutex_unlock(&lock);
      }
    } else if (depth < 4) {
      scan_dir(library, path, depth + 1); /* fails quietly on a file */
    }
  }
  closedir(d);
}

static void *scan(void *unused) {
  (void)unused;
  FT_Library library;
  if (FT_Init_FreeType(&library) == 0) {
    size_t count = 0;
    const char *const *dirs = jf_os_font_dirs(&count);
    for (size_t i = 0; i < count; i++)
      scan_dir(library, dirs[i], 0);
    FT_Done_FreeType(library);
  }
  pthread_mutex_lock(&lock);
  state = READY;
  pthread_cond_broadcast(&scanned);
  fprintf(stderr, "fonts: %zu indexed\n", font_count);
  pthread_mutex_unlock(&lock);
  return NULL;
}

/* Claims the scan when nobody has started it. Lock held. */
static bool claim_scan(void) {
  if (state != IDLE)
    return false;
  state = SCANNING;
  return true;
}

void jf_font_index_warm(void) {
  pthread_mutex_lock(&lock);
  const bool mine = claim_scan();
  pthread_mutex_unlock(&lock);
  pthread_t thread;
  if (mine) {
    if (pthread_create(&thread, NULL, scan, NULL) == 0)
      pthread_detach(thread);
    else
      scan(NULL);
  }
}

/* Waits for the index, scanning here if nothing has started it. Returns
 * with the lock held. */
static void ready(void) {
  pthread_mutex_lock(&lock);
  if (claim_scan()) {
    pthread_mutex_unlock(&lock);
    scan(NULL);
    pthread_mutex_lock(&lock);
  }
  while (state != READY)
    pthread_cond_wait(&scanned, &lock);
}

static bool has(const font *f, uint32_t codepoint) {
  size_t low = 0, high = f->count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (f->ranges[mid][1] < codepoint)
      low = mid + 1;
    else
      high = mid;
  }
  return low < f->count && f->ranges[low][0] <= codepoint;
}

const char *jf_font_for(uint32_t codepoint) {
  ready();
  const char *path = NULL;
  for (size_t i = 0; i < font_count && path == NULL; i++)
    if (has(&fonts[i], codepoint))
      path = fonts[i].path;
  pthread_mutex_unlock(&lock);
  return path;
}

bool jf_font_covers(const char *path, uint32_t codepoint) {
  ready();
  bool covered = false;
  for (size_t i = 0; i < font_count; i++)
    if (strcmp(fonts[i].path, path) == 0) {
      covered = has(&fonts[i], codepoint);
      break;
    }
  pthread_mutex_unlock(&lock);
  return covered;
}
