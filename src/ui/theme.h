/* The palette every application here draws with. */
#pragma once

#include "loom.h"

static const loom_color UI_BG = {9, 13, 22, 255};
static const loom_color UI_PANEL = {20, 27, 40, 255};
static const loom_color UI_CARD = {27, 36, 52, 255};
static const loom_color UI_HOT = {38, 51, 70, 255};
static const loom_color UI_SELECTED = {27, 69, 91, 255};
static const loom_color UI_ACCENT = {0, 164, 220, 255};
static const loom_color UI_RED = {228, 96, 96, 255};
static const loom_color UI_TEXT = {239, 244, 250, 255};
static const loom_color UI_DIM = {148, 163, 182, 255};
static const loom_color UI_BORDER = {55, 70, 91, 255};
static const loom_color UI_WHITE = {255, 255, 255, 255};
static const loom_color UI_YELLOW = {255, 206, 64, 255};

/* How long a focus or scroll move eases for, and an overlay sliding in. */
#define UI_NAVIGATION_NS 180000000ull
#define UI_SLIDE_NS 280000000ull
