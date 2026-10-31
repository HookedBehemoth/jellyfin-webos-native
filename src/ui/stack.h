/*
 * Where Back goes: the screens left to get here, each with the focus it had.
 *
 * Every forward step pushes the screen being left and Back pops it, so the
 * focus comes back to exactly the item it was on - no screen has to know
 * where it was opened from, or name the control to return to. An entry holds
 * enough to re-enter a screen, not a snapshot of it: what that is belongs to
 * the application, so entries are the application's own struct, whose first
 * member is a ui_view.
 *
 *     typedef struct { ui_view view; card item; } entry;
 *     static entry storage[12];
 *     ui_stack_init(&stack, storage, sizeof(entry), 12);
 *
 *     ui_stack_push(&stack, &ui, &(entry){{SCREEN_GRID}, library});
 *     const entry *back = ui_stack_pop(&stack, &ui);
 *
 * The screen on top is the application's live state, not an entry; the stack
 * holds only what is under it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "nav.h"

typedef struct {
  int screen;
  /* Filled in by ui_stack_push from where the focus is as the screen is left.
   */
  nav_id focus;
} ui_view;

typedef struct {
  unsigned char *storage;
  size_t entry_size, capacity, depth;
} ui_stack;

void ui_stack_init(ui_stack *stack, void *storage, size_t entry_size,
                   size_t capacity);
/* Leaving the screen `entry` re-enters for another, which starts on its own
 * default: the focus is recorded in the entry and cleared. Past capacity the
 * oldest entry goes, rather than refusing to navigate. */
void ui_stack_push(ui_stack *stack, nav *ui, const void *entry);
/* Back. The entry to re-enter, valid until the next push, with its focus
 * already handed back to nav; NULL at the root. */
const void *ui_stack_pop(ui_stack *stack, nav *ui);
/* The entry Back would return to, without going there. */
const void *ui_stack_peek(const ui_stack *stack);
/* A new root, such as signing in or out: nothing to go back to. */
void ui_stack_clear(ui_stack *stack);
static inline size_t ui_stack_depth(const ui_stack *stack) {
  return stack->depth;
}
