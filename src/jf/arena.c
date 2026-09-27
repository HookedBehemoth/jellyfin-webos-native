#include "arena.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define BLOCK_SIZE (64 * 1024)

/* Debug builds fill fresh and rewound memory with a pattern, so reading
 * something never written - or written before the last reset - shows up as
 * garbage instead of as the zeros or the stale values that happened to be
 * there. Nothing is promised zeroed: a caller that wants zeros writes them. */
#ifdef NDEBUG
#define POISON(data, size) ((void)0)
#else
#define POISON(data, size) memset(data, 0xa5, size)
#endif

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
  POISON(block->data, capacity);
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
  POISON(arena->head->data, arena->head->used);
  arena->head->used = 0;
}

void jf_arena_rewind(jf_arena *arena) {
  if (arena->head == NULL || arena->head->next == NULL) {
    jf_arena_reset(arena);
    return;
  }
  size_t total = 0;
  for (jf_arena_block *block = arena->head; block != NULL; block = block->next)
    total += block->capacity;
  jf_arena_destroy(arena);
  arena->head = new_block(total);
}

void jf_arena_destroy(jf_arena *arena) {
  for (jf_arena_block *block = arena->head; block != NULL;) {
    jf_arena_block *next = block->next;
    free(block);
    block = next;
  }
  arena->head = NULL;
}
