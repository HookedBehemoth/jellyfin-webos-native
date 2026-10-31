/*
 * What every screen of every application here is drawn with, above loom and
 * nav: the frame's scratch strings, eased values, the common controls, and the
 * two development aids - a replayed remote and a capture of the frame.
 *
 * A control declares itself to nav and draws; the flags it returns say what
 * happened to it this frame, as nav_item's do. Sizes are given at 1080 rows
 * and multiplied by the kit's scale, which the application sets each frame.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "loom.h"
#include "nav.h"
#include "renderer.h"

typedef struct {
  jf_renderer *renderer;
  nav *nav;
  /* Screen pixels per pixel at 1080 rows. */
  float scale;
} ui_kit;

uint64_t ui_now_ns(void);

/* Per-frame scratch for formatted labels. A draw command borrows its string
 * until the renderer has consumed it, so these outlive the layout pass - but
 * only that: ui_frame_reset at the top of every frame recycles them. */
const char *ui_fmt(const char *pattern, ...)
    __attribute__((format(printf, 1, 2)));
void ui_frame_reset(void);

/* A value easing towards a target, ease-out exponential: quick feedback, with
 * the last few pixels settling softly. */
typedef struct {
  float value, start, target;
  uint64_t started_at, duration;
} ui_anim;

bool ui_anim_running(const ui_anim *value);
void ui_anim_snap(ui_anim *value, float target);
void ui_anim_update(ui_anim *value, uint64_t now);
/* Without `animate` it snaps there. */
void ui_anim_set(ui_anim *value, float target, uint64_t duration, bool animate);

unsigned ui_button(ui_kit *kit, loom_context *ctx, loom_rect rect,
                   const char *label, nav_id id);
/* The one control a screen is for, filled with the accent. */
unsigned ui_action_button(ui_kit *kit, loom_context *ctx, loom_rect rect,
                          const char *label, nav_id id);
/* A setting's row: its name on the left, its value on the right. */
unsigned ui_value_row(ui_kit *kit, loom_context *ctx, loom_rect rect,
                      const loom_rect *clip, const char *label,
                      const char *value, nav_id id);
void ui_panel(ui_kit *kit, loom_context *ctx, loom_rect rect);
/* Asks for the next frame itself, for as long as it is drawn. */
void ui_spinner(ui_kit *kit, loom_context *ctx, float x, float y, float radius,
                const loom_rect *clip);
/* `text` trimmed to `width` with an ellipsis, measured against the real glyph
 * advances and never cut inside a UTF-8 sequence. Frame scratch. */
const char *ui_ellipsize(ui_kit *kit, const char *text, float width,
                         float size);
/* Greedy word wrap into `rect`; whatever does not fit is left out. */
void ui_wrapped(ui_kit *kit, loom_context *ctx, loom_rect rect,
                const char *text, float size, const loom_color color);

/* The current GL framebuffer as a PPM. */
bool ui_capture(const char *path);
/* An installed app has no terminal: stdout and stderr go to `path`, or to
 * `fallback` when that cannot be opened. Nothing changes on a terminal. */
void ui_log_to_file(const char *path, const char *fallback);

/* A remote, replayed: UI_SCRIPT=drrob presses those buttons a few frames
 * apart, so a screen can be reached without a TV or a person. Letters are the
 * four arrows, `o` for select and `b` for Back; `.` waits one more beat, and
 * anything else is the application's to interpret. */
typedef struct {
  const char *keys;
  size_t at;
  unsigned wait;
} ui_script;

/* Frames between presses: long enough that a request one press started has
 * landed before the next. */
#define UI_SCRIPT_BEAT 45

/* The command to perform this frame, 0 while waiting, -1 once the script is
 * done and nothing is `busy`. Never presses while `busy`: what is still loading
 * decides what the focused control even is. */
int ui_script_next(ui_script *script, bool busy);
/* The evdev key for an arrow, select or Back command; 0 for anything else. */
uint32_t ui_script_key(char command);
