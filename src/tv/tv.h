/*
 * What the launcher asks of the TV: the apps and inputs to show, opening one,
 * and removing an app.
 *
 * Every request goes out over the Luna bus and comes back later, on whatever
 * thread the bus answers on. Answers are queued and applied to `tv` by
 * tv_pump on the main thread, so the screens read plain state and never wait.
 * The transport is the only part that differs between the TV and a desktop
 * (transport.h): the payloads are parsed here either way, so the host tests
 * run the same parsing against the mock's canned answers.
 *
 * Payload shapes are as glasshouse and LG's own Home use them; see
 * docs/home.md for which were checked on which TV.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TV_APPS 96
#define TV_INPUTS 8

typedef struct {
  char id[128];
  char title[96];
  /* An absolute path to a PNG, or "" for none. */
  char icon[384];
  /* The colour the app asks its tile to be, 0xRRGGBB, when it names one. */
  uint32_t color;
  bool has_color;
  /* Can be uninstalled; LG's own apps cannot. */
  bool removable;
} tv_app;

typedef struct {
  /* EIM's id, HDMI_1 .. HDMI_4, and the app that shows it. */
  char id[16];
  char app_id[64];
  char label[64];
  /* What is plugged in, by its HDMI-CEC name, or "". */
  char device[64];
  bool connected;
  /* The last frame the TV kept of it, an absolute path, or "". */
  char preview[384];
} tv_input;

typedef struct {
  tv_app apps[TV_APPS];
  size_t app_count;
  bool apps_loaded;
  tv_input inputs[TV_INPUTS];
  size_t input_count;
  bool inputs_loaded;
  /* The app in front, beneath the launcher, as tv_refresh last found it:
   * an input is its app. "" for none. `foreground_fresh` is set with every
   * answer, for the launcher to clear. */
  char foreground[128];
  bool foreground_fresh;
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
/* Uninstalls it; the apps are asked again once the TV has. */
bool tv_remove_app(const char *app_id);
/* Restarts the TV, as its own power menu does. */
bool tv_restart(void);
/* Opens the input or app the TV was last showing (general.lastInputApp), as
 * LG's Home does when the TV comes on. */
bool tv_open_last_input(void);
