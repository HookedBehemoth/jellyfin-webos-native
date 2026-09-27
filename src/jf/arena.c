#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE (64 * 1024)

struct jf_arena_block {
  jf_arena_block *next;
  size_t used, capacity;
  /* Flexible array, so a block is one allocation. */
  unsigned char data[];
};

static jf_arena_block *new_block(size_t capacity) {
  if (capacity < BLOCK_SIZE)
    capacity = BLOCK_SIZE;
  jf_arena_block *block = malloc(sizeof(*block) + capacity);
  if (block == NULL)
    return NULL;
  block->next = NULL;
  block->used = 0;
  block->capacity = capacity;
  return block;
}

void *jf_arena_alloc(jf_arena *arena, size_t size) {
  size = (size + 7u) & ~(size_t)7; /* keep every allocation 8-aligned */
  if (arena->head == NULL || arena->head->used + size > arena->head->capacity) {
    jf_arena_block *block = new_block(size);
    if (block == NULL)
      return NULL;
    block->next = arena->head;
    arena->head = block;
  }
  void *out = arena->head->data + arena->head->used;
  arena->head->used += size;
  return out;
}

char *jf_arena_strdup(jf_arena *arena, const char *text) {
  if (text == NULL)
    text = "";
  const size_t size = strlen(text) + 1;
  char *out = jf_arena_alloc(arena, size);
  if (out != NULL)
    memcpy(out, text, size);
  return out;
}

void jf_arena_reset(jf_arena *arena) {
  if (arena->head == NULL)
    return;
  for (jf_arena_block *block = arena->head->next; block != NULL;) {
    jf_arena_block *next = block->next;
    free(block);
    block = next;
  }
  arena->head->next = NULL;
  arena->head->used = 0;
}

void jf_arena_destroy(jf_arena *arena) {
  for (jf_arena_block *block = arena->head; block != NULL;) {
    jf_arena_block *next = block->next;
    free(block);
    block = next;
  }
  arena->head = NULL;
}
