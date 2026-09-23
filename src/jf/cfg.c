#include "cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "../platform/os.h"

static char *trim(char *text) {
  while (*text == ' ' || *text == '\t')
    text++;
  char *end = text + strlen(text);
  while (end > text && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r'))
    *--end = '\0';
  return text;
}

void cfg_init(cfg_reader *reader, char *text) {
  reader->cursor = text;
  reader->section = "";
  reader->key = NULL;
  reader->value = NULL;
}

bool cfg_open(cfg_reader *reader, jf_arena *arena, const char *path) {
  cfg_init(reader, NULL);
  FILE *file = fopen(path, "rb");
  if (file == NULL)
    return true;
  char *text = NULL;
  long size = -1;
  if (fseek(file, 0, SEEK_END) == 0)
    size = ftell(file);
  if (size >= 0 && size <= 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0)
    text = jf_arena_alloc(arena, (size_t)size + 1);
  if (text != NULL)
    text[fread(text, 1, (size_t)size, file)] = '\0';
  fclose(file);
  reader->cursor = text;
  return text != NULL;
}

cfg_event cfg_next(cfg_reader *reader) {
  while (reader->cursor != NULL && *reader->cursor != '\0') {
    char *line = reader->cursor;
    char *next = strchr(line, '\n');
    if (next != NULL)
      *next++ = '\0';
    reader->cursor = next;
    char *text = trim(line);
    if (text[0] == '[') {
      char *end = strchr(text + 1, ']');
      if (end == NULL)
        continue;
      *end = '\0';
      reader->section = trim(text + 1);
      reader->key = NULL;
      reader->value = NULL;
      return CFG_SECTION;
    }
    if (text[0] == ';' || text[0] == '#')
      continue;
    char *equals = strchr(text, '=');
    if (equals == NULL)
      continue;
    *equals = '\0';
    reader->key = trim(text);
    reader->value = trim(equals + 1);
    return CFG_VALUE;
  }
  reader->cursor = NULL;
  return CFG_END;
}

const char *cfg_text(const cfg_reader *reader) { return reader->value; }

int cfg_int(const cfg_reader *reader, int fallback) {
  char *end;
  const long value = strtol(reader->value, &end, 10);
  return *reader->value != '\0' && *end == '\0' ? (int)value : fallback;
}

float cfg_float(const cfg_reader *reader, float fallback) {
  char *end;
  const float value = strtof(reader->value, &end);
  return *reader->value != '\0' && *end == '\0' ? value : fallback;
}

bool cfg_bool(const cfg_reader *reader, bool fallback) {
  static const char *const yes[] = {"true", "yes", "on", "1"};
  static const char *const no[] = {"false", "no", "off", "0"};
  for (size_t i = 0; i < sizeof(yes) / sizeof(*yes); i++) {
    if (strcasecmp(reader->value, yes[i]) == 0)
      return true;
    if (strcasecmp(reader->value, no[i]) == 0)
      return false;
  }
  return fallback;
}

void cfg_writer_init(cfg_writer *writer, jf_arena *arena) {
  *writer = (cfg_writer){.arena = arena, .ok = true};
}

static void emit(cfg_writer *writer, const char *format, const char *first,
                 const char *second) {
  const int needed = snprintf(NULL, 0, format, first, second);
  if (!writer->ok || needed < 0)
    return;
  if (writer->length + (size_t)needed + 1 > writer->capacity) {
    size_t capacity = writer->capacity != 0 ? writer->capacity * 2 : 256;
    while (capacity < writer->length + (size_t)needed + 1)
      capacity *= 2;
    char *text = jf_arena_alloc(writer->arena, capacity);
    if (text == NULL) {
      writer->ok = false;
      return;
    }
    if (writer->length != 0)
      memcpy(text, writer->text, writer->length);
    writer->text = text;
    writer->capacity = capacity;
  }
  snprintf(writer->text + writer->length, (size_t)needed + 1, format, first,
           second);
  writer->length += (size_t)needed;
}

void cfg_comment(cfg_writer *writer, const char *text) {
  emit(writer, "%s%s\n", "# ", text);
}

void cfg_section(cfg_writer *writer, const char *name) {
  emit(writer, "%s[%s]\n", writer->length != 0 ? "\n" : "", name);
}

void cfg_write_text(cfg_writer *writer, const char *key, const char *value) {
  emit(writer, "%s=%s\n", key, value);
}

void cfg_write_int(cfg_writer *writer, const char *key, int value) {
  char text[16];
  snprintf(text, sizeof(text), "%d", value);
  cfg_write_text(writer, key, text);
}

void cfg_write_float(cfg_writer *writer, const char *key, float value) {
  char text[32];
  snprintf(text, sizeof(text), "%.9g", value);
  cfg_write_text(writer, key, text);
}

void cfg_write_bool(cfg_writer *writer, const char *key, bool value) {
  cfg_write_text(writer, key, value ? "true" : "false");
}

bool cfg_flush(const cfg_writer *writer, const char *path) {
  char temporary[600];
  const int length = snprintf(temporary, sizeof(temporary), "%s.tmp", path);
  if (!writer->ok || length <= 0 || (size_t)length >= sizeof(temporary))
    return false;
  FILE *file = fopen(temporary, "wb");
  if (file == NULL)
    return false;
  bool ok = fwrite(writer->text, 1, writer->length, file) == writer->length &&
            fflush(file) == 0 && jf_os_fsync(fileno(file)) == 0;
  ok = fclose(file) == 0 && ok;
  if (ok && rename(temporary, path) == 0)
    return true;
  remove(temporary);
  return false;
}
