#include "layout.h"

#include <stdio.h>
#include <string.h>

/* One line per app, in order: "show <id>" or "hide <id>". */
void layout_load(home_layout *layout, const char *path) {
  layout->count = layout->shown = 0;
  FILE *file = fopen(path, "r");
  if (file == NULL)
    return;
  char line[160];
  home_layout read = {0};
  while (fgets(line, sizeof(line), file) != NULL && read.count < TV_APPS) {
    line[strcspn(line, "\r\n")] = '\0';
    const bool hidden = strncmp(line, "hide ", 5) == 0;
    if (!hidden && strncmp(line, "show ", 5) != 0)
      continue;
    const size_t length = strlen(line + 5);
    if (length == 0 || length >= sizeof(read.slots[0].id) ||
        layout_find(&read, line + 5) >= 0)
      continue;
    layout_slot *slot = &read.slots[read.count++];
    memcpy(slot->id, line + 5, length + 1);
    slot->hidden = hidden;
  }
  fclose(file);
  /* The shown ones first, whatever order the file had them in. */
  for (int pass = 0; pass < 2; pass++)
    for (size_t i = 0; i < read.count; i++)
      if (read.slots[i].hidden == (pass == 1)) {
        layout->slots[layout->count++] = read.slots[i];
        layout->shown += pass == 0;
      }
}

bool layout_save(const home_layout *layout, const char *path) {
  char temporary[512];
  snprintf(temporary, sizeof(temporary), "%s.new", path);
  FILE *file = fopen(temporary, "w");
  if (file == NULL)
    return false;
  for (size_t i = 0; i < layout->count; i++)
    fprintf(file, "%s %s\n", layout->slots[i].hidden ? "hide" : "show",
            layout->slots[i].id);
  const bool written = fclose(file) == 0;
  return written && rename(temporary, path) == 0;
}

int layout_find(const home_layout *layout, const char *id) {
  for (size_t i = 0; i < layout->count; i++)
    if (strcmp(layout->slots[i].id, id) == 0)
      return (int)i;
  return -1;
}

static bool installed(const tv_app *apps, size_t count, const char *id) {
  for (size_t i = 0; i < count; i++)
    if (strcmp(apps[i].id, id) == 0)
      return true;
  return false;
}

/* `slot` out of the list, the rest closing up behind it. */
static layout_slot take(home_layout *layout, size_t at) {
  const layout_slot slot = layout->slots[at];
  memmove(&layout->slots[at], &layout->slots[at + 1],
          (layout->count - at - 1) * sizeof(layout_slot));
  layout->count--;
  if (at < layout->shown)
    layout->shown--;
  return slot;
}

static void put(home_layout *layout, size_t at, layout_slot slot) {
  memmove(&layout->slots[at + 1], &layout->slots[at],
          (layout->count - at) * sizeof(layout_slot));
  layout->slots[at] = slot;
  layout->count++;
}

void layout_sync(home_layout *layout, const tv_app *apps, size_t count) {
  for (size_t i = layout->count; i-- > 0;)
    if (!installed(apps, count, layout->slots[i].id))
      take(layout, i);
  for (size_t i = 0; i < count && layout->count < TV_APPS; i++)
    if (layout_find(layout, apps[i].id) < 0) {
      layout_slot slot = {.hidden = false};
      snprintf(slot.id, sizeof(slot.id), "%s", apps[i].id);
      put(layout, layout->shown++, slot);
    }
}

bool layout_move(home_layout *layout, const char *id, int direction) {
  const int at = layout_find(layout, id);
  const int to = at + (direction > 0 ? 1 : -1);
  if (at < 0 || (size_t)at >= layout->shown || to < 0 ||
      (size_t)to >= layout->shown)
    return false;
  const layout_slot slot = layout->slots[at];
  layout->slots[at] = layout->slots[to];
  layout->slots[to] = slot;
  return true;
}

void layout_hide(home_layout *layout, const char *id) {
  const int at = layout_find(layout, id);
  if (at < 0 || (size_t)at >= layout->shown)
    return;
  layout_slot slot = take(layout, (size_t)at);
  slot.hidden = true;
  put(layout, layout->shown, slot);
}

void layout_show(home_layout *layout, const char *id) {
  const int at = layout_find(layout, id);
  if (at < 0 || (size_t)at < layout->shown)
    return;
  layout_slot slot = take(layout, (size_t)at);
  slot.hidden = false;
  put(layout, layout->shown++, slot);
}
