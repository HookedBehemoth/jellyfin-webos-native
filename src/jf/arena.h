/*
 * A bump allocator with a per-task lifetime.
 *
 * Every fetcher task parses a server response into one of these and the UI
 * copies what it needs out before handing the slot back; then the whole arena
 * is reset in one call. That is the reason a task's results can be plain
 * pointers with no ownership rules attached.
 */
#pragma once

#include <stddef.h>

typedef struct jf_arena_block jf_arena_block;

typedef struct {
  jf_arena_block *head;
} jf_arena;

void *jf_arena_alloc(jf_arena *arena, size_t size);
char *jf_arena_strdup(jf_arena *arena, const char *text);
/* Keeps the first block, so a slot reused every frame stops calling malloc. */
void jf_arena_reset(jf_arena *arena);
void jf_arena_destroy(jf_arena *arena);
