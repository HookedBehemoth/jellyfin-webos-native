/*
 * The launcher's own order of the apps, and which are hidden: LG's own apps
 * cannot be uninstalled, so hiding is what the launcher offers for every app.
 *
 * It is the launcher's, kept in a file of its own, and changes nothing on the
 * TV: LG's Home keeps its order when it has its place back. Apps the TV lists
 * that the layout does not know are shown after the rest, in the TV's order.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "../tv/tv.h"

typedef struct {
  char id[128];
  bool hidden;
} layout_slot;

/* The shown apps first, in order, then the hidden ones. */
typedef struct {
  layout_slot slots[TV_APPS];
  size_t count;
  /* Where the shown ones end. */
  size_t shown;
} home_layout;

/* Missing or unreadable is an empty layout: every app shown, as the TV lists
 * them. */
void layout_load(home_layout *layout, const char *path);
bool layout_save(const home_layout *layout, const char *path);

/* Up to date with what is installed: apps gone are dropped, new ones shown at
 * the end of the shown ones. */
void layout_sync(home_layout *layout, const tv_app *apps, size_t count);
/* The slot an app is in, or -1. */
int layout_find(const home_layout *layout, const char *id);
/* One place along the shown apps, towards `direction`. False at an end, or for
 * one that is not shown. */
bool layout_move(home_layout *layout, const char *id, int direction);
/* To the front of the hidden apps. */
void layout_hide(home_layout *layout, const char *id);
/* Shown again, at the end of the shown apps. */
void layout_show(home_layout *layout, const char *id);
