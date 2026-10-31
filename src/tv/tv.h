/*
 * What the launcher asks of the TV: the apps and inputs to show, opening one,
 * LG's own screens that open over the launcher, and the few settings it
 * changes itself.
 *
 * Every request goes out over the Luna bus and comes back later, on whatever
 * thread the bus answers on. Answers are queued and applied to `tv` by
 * tv_pump on the main thread, so the screens read plain state and never wait.
 * The transport is the only part that differs between the TV and a desktop
 * (transport.h): the payloads are parsed here either way, so the host tests
 * run the same parsing against the mock's canned answers.
 *
 * Payload shapes are as glasshouse documents them for webOS 5 to 11; see
 * docs/home.md for which were checked on which TV.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TV_APPS 96
#define TV_INPUTS 8
#define TV_CHOICES 24

typedef struct {
  char id[128];
  char title[96];
  /* An absolute path to a PNG, or "" for none. */
  char icon[384];
} tv_app;

typedef struct {
  /* EIM's id, HDMI_1 .. HDMI_4, and the app that shows it. */
  char id[16];
  char app_id[64];
  char label[64];
  /* What is plugged in, by its HDMI-CEC name, or "". */
  char device[64];
  bool connected;
} tv_input;

/* LG's screens that open over whatever is in front, the launcher included. */
typedef enum {
  TV_OVERLAY_INPUTS,
  TV_OVERLAY_QUICK_SETTINGS,
  TV_OVERLAY_SETTINGS,
  TV_OVERLAY_NOTIFICATIONS,
  TV_OVERLAY_SEARCH,
  TV_OVERLAY_COUNT
} tv_overlay;

typedef enum {
  TV_PICTURE_MODE,
  TV_SOUND_OUTPUT,
  TV_ENERGY_SAVING,
  TV_SETTING_COUNT
} tv_setting_id;

typedef struct {
  const char *category, *key, *title;
  char value[48];
  bool known;
  /* The values it takes now, as the TV describes them. A setting the TV says
   * is not visible here is left out of the screen; one not active is shown
   * but cannot change. */
  char choices[TV_CHOICES][48];
  size_t choice_count;
  bool visible, active;
  /* A write on its way: the value shown is the one asked for. */
  bool writing;
} tv_setting;

typedef struct {
  tv_app apps[TV_APPS];
  size_t app_count;
  bool apps_loaded;
  tv_input inputs[TV_INPUTS];
  size_t input_count;
  bool inputs_loaded;
  tv_setting settings[TV_SETTING_COUNT];
  /* Which app each overlay is on this TV, or "" where it has none. */
  char overlay_app[TV_OVERLAY_COUNT][64];
  /* The last thing that went wrong, for a status line; "" for nothing. */
  char error[160];
} tv_state;

extern tv_state tv;

/* `wake` is called, from any thread, when an answer is queued. */
void tv_init(void (*wake)(void));
void tv_deinit(void);
/* Applies queued answers. True when `tv` changed. */
bool tv_pump(void);
/* Requests still unanswered. */
size_t tv_pending(void);

/* Everything the launcher shows, asked again. */
void tv_refresh(void);
bool tv_launch(const char *app_id);
bool tv_open_input(const tv_input *input);
bool tv_open_overlay(tv_overlay overlay);
const char *tv_overlay_title(tv_overlay overlay);
/* Shown at once, written behind it; a refusal puts the old value back. */
void tv_setting_set(tv_setting_id setting, const char *value);
/* The panel off and the sound on, for music. */
bool tv_screen_off(void);
/* LG's own name for a setting's value, "expert1" as "Expert (Dark room)". */
const char *tv_value_title(const char *value);
