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
 * A developer app may not list the apps, ask what is in front or uninstall, so
 * those go through Home Chooser's launcher API; the rest are public. See
 * docs/home.md for which is which.
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
  /* Its folder, and the icons it names in it, possibly as absolute paths; ""
   * for none. A jailed app can read some of these itself. */
  char folder[256];
  char icon[128], large_icon[128];
  /* As the app last said; what it says changes with an update. */
  char version[32];
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
  /* Home Chooser is ready and has another app as Home, as tv_check_home last
   * found. For the launcher to clear. */
  bool not_chosen;
  /* The last thing that went wrong, for a status line; "" for nothing. */
  char error[160];
} tv_state;

extern tv_state tv;

/* An icon asked for with tv_ask_icon, as the file's bytes, which are only valid
 * during the call; NULL for none. Called from tv_pump. */
typedef void tv_icon_fn(uint32_t slot, const uint8_t *data, size_t size);

/* `wake` is called, from any thread, when an answer is queued. */
void tv_init(void (*wake)(void), tv_icon_fn *on_icon);
void tv_deinit(void);
/* Applies queued answers. True when `tv` changed. */
bool tv_pump(void);
/* Requests still unanswered. */
size_t tv_pending(void);

/* Everything the launcher shows that can have changed unannounced: the apps
 * only until Home Chooser has said it will tell of changes. */
void tv_refresh(void);
bool tv_launch(const char *app_id);
bool tv_open_input(const tv_input *input);
/* Has Home Chooser ask the owner to uninstall it. That answers at once and the
 * owner later: the list changes if they agree. */
bool tv_remove_app(const char *app_id);
/* Whether Home Chooser is there and ready, and this app its choice: what it
 * finds goes to tv.error or tv.not_chosen. */
void tv_check_home(void);
/* Opens Home Chooser, asking the owner to make `app_id` Home. */
bool tv_ask_to_be_home(const char *app_id);
/* One app's icon, through Home Chooser, as a jailed app cannot read most of
 * them: `slot` is handed back with it. */
void tv_ask_icon(const char *app_id, bool large, uint32_t slot);
