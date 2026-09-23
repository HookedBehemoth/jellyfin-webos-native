/* INI files. The reader emits sections and values, skipping blank lines and
 * comments, and cuts the text in place with NUL bytes. The writer appends to an
 * arena buffer that cfg_flush writes out. */
#pragma once

#include "arena.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum { CFG_END, CFG_SECTION, CFG_VALUE } cfg_event;

typedef struct {
  char *cursor;
  const char *section; /* the latest header, "" before any */
  const char *key;
  const char *value;
} cfg_reader;

/* text is modified; it has to outlive every pointer the reader hands out. */
void cfg_init(cfg_reader *reader, char *text);
/* Reads the whole file into the arena. A missing file is an empty document. */
bool cfg_open(cfg_reader *reader, jf_arena *arena, const char *path);
cfg_event cfg_next(cfg_reader *reader);

/* The current value, or fallback when it does not parse as that type. */
const char *cfg_text(const cfg_reader *reader);
int cfg_int(const cfg_reader *reader, int fallback);
float cfg_float(const cfg_reader *reader, float fallback);
bool cfg_bool(const cfg_reader *reader, bool fallback);

typedef struct {
  jf_arena *arena;
  char *text;
  size_t length;
  size_t capacity;
  bool ok;
} cfg_writer;

void cfg_writer_init(cfg_writer *writer, jf_arena *arena);
void cfg_comment(cfg_writer *writer, const char *text);
void cfg_section(cfg_writer *writer, const char *name);
void cfg_write_text(cfg_writer *writer, const char *key, const char *value);
void cfg_write_int(cfg_writer *writer, const char *key, int value);
void cfg_write_float(cfg_writer *writer, const char *key, float value);
void cfg_write_bool(cfg_writer *writer, const char *key, bool value);
/* Atomic: writes a temporary and renames it over path. */
bool cfg_flush(const cfg_writer *writer, const char *path);
