#include "stack.h"

#include <string.h>

void ui_stack_init(ui_stack *stack, void *storage, size_t entry_size,
                   size_t capacity) {
  stack->storage = storage;
  stack->entry_size = entry_size;
  stack->capacity = capacity;
  stack->depth = 0;
}

static unsigned char *at(const ui_stack *stack, size_t index) {
  return stack->storage + index * stack->entry_size;
}

void ui_stack_push(ui_stack *stack, nav *ui, const void *entry) {
  if (stack->capacity == 0)
    return;
  if (stack->depth == stack->capacity) {
    memmove(at(stack, 0), at(stack, 1), (stack->depth - 1) * stack->entry_size);
    stack->depth--;
  }
  unsigned char *slot = at(stack, stack->depth++);
  memmove(slot, entry, stack->entry_size);
  ((ui_view *)slot)->focus = ui->focus;
  nav_focus(ui, 0);
}

const void *ui_stack_pop(ui_stack *stack, nav *ui) {
  if (stack->depth == 0)
    return NULL;
  const unsigned char *entry = at(stack, --stack->depth);
  nav_focus(ui, ((const ui_view *)entry)->focus);
  return entry;
}

const void *ui_stack_peek(const ui_stack *stack) {
  return stack->depth == 0 ? NULL : at(stack, stack->depth - 1);
}

void ui_stack_clear(ui_stack *stack) { stack->depth = 0; }
