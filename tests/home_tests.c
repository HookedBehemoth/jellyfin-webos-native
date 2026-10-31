/* The launcher against the TV of src/tv/tv_mock.h: key presses in, focus,
 * screens and Luna calls checked.
 *
 * main.c is included rather than linked, so its state is in reach, and the
 * frames are real ones on SDL's offscreen driver. Like tests/ui_tests.c this
 * is one journey in order. UI_TEST_CAPTURE=<dir> saves the checked frames.
 */
#define HOME_APP_TEST
#include "home/main.c"

#include <unistd.h>

#include "tv/tv_mock.h"

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "FAIL %s:%d: %s (screen %d, focus %08x)\n", __FILE__,    \
              __LINE__, #cond, (int)screen, (unsigned)ui.focus);               \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static int failures;

/* Frames until every answer is in and nothing asks for another frame. */
static void settle(void) {
  const uint64_t deadline = ui_now_ns() + 10000000000ull;
  for (;;) {
    jf_window_poll();
    int wait_ms;
    const bool drew = app_step(&wait_ms);
    if (!drew && tv_pending() == 0)
      return;
    if (ui_now_ns() > deadline) {
      fprintf(stderr, "FAIL: the launcher did not settle\n");
      failures++;
      return;
    }
    if (!drew)
      usleep(1000);
  }
}

enum { UP = 103, DOWN = 108, LEFT = 105, RIGHT = 106, OK = 28, BACK = 158 };

static void press(uint32_t code) {
  on_key(code, true, false);
  settle();
}

static void presses(uint32_t code, unsigned count) {
  while (count-- > 0)
    press(code);
}

static void capture(const char *name) {
  const char *dir = getenv("UI_TEST_CAPTURE");
  if (dir == NULL)
    return;
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
  ui_capture(path);
}

/* Ids as the screens declare them. */
static nav_id tile(const char *id) {
  return nav_child(nav_str("ribbon"), nav_str(id));
}
static nav_id tv_tile(int overlay) {
  return nav_child(nav_str("ribbon"), nav_int((uint64_t)(overlay + 1)));
}
static nav_id choice(size_t index) {
  return nav_child(nav_str("choices"), nav_int(index));
}

static void focus(nav_id id) {
  nav_focus(&ui, id);
  jf_window_frame_requested = true;
  settle();
}

/* ------------------------------------------------------------------ tests */

/* What the TV lists, less what is not an app to open: the bookmark, the
 * hidden app, the input app and LG's Home. */
static void test_catalogue(void) {
  settle();
  CHECK(screen == SCREEN_HOME);
  CHECK(tv.app_count == TV_MOCK_APPS);
  CHECK(strcmp(tv.apps[0].id, "netflix") == 0);
  CHECK(tv.apps[0].icon[0] == '/');
  /* A relative icon is no icon. */
  CHECK(strcmp(tv.apps[2].id, "com.webos.app.browser") == 0 &&
        tv.apps[2].icon[0] == '\0');
  CHECK(tv.input_count == 4);
  CHECK(strcmp(tv.inputs[0].device, "PlayStation 5") == 0);
  CHECK(!tv.inputs[1].connected && tv.inputs[2].connected);
  CHECK(strcmp(tv.inputs[2].label, "HDMI 3") == 0);
  CHECK(strcmp(tv.inputs[2].app_id, "com.webos.app.hdmi3") == 0);
  CHECK(!tv.inputs[3].connected);
  /* The input picker under its older id; no quick settings, no search. */
  CHECK(strcmp(tv.overlay_app[TV_OVERLAY_INPUTS],
               "com.webos.app.inputcommon") == 0);
  CHECK(tv.overlay_app[TV_OVERLAY_QUICK_SETTINGS][0] == '\0');
  CHECK(tv.overlay_app[TV_OVERLAY_SEARCH][0] == '\0');
  CHECK(ui.focus == tile("netflix"));
  capture("ribbon");
}

static void test_open(void) {
  press(OK);
  CHECK(tv_mock_calls("launch") == 1);
  CHECK(strstr(tv_mock_last("launch"), "\"netflix\"") != NULL);
  /* Opening something leaves the ribbon where it was. */
  CHECK(screen == SCREEN_HOME && ui.focus == tile("netflix"));

  presses(RIGHT, 5);
  CHECK(ui.focus == tile("HDMI_3"));
  press(OK);
  CHECK(strstr(tv_mock_last("launch"), "com.webos.app.hdmi3") != NULL);

  /* The settings tile, then LG's input picker after it. */
  presses(RIGHT, 3);
  CHECK(ui.focus == tv_tile(TV_OVERLAY_INPUTS));
  press(OK);
  CHECK(strstr(tv_mock_last("launch"), "com.webos.app.inputcommon") != NULL);
  press(RIGHT);
  CHECK(ui.focus == tv_tile(TV_OVERLAY_NOTIFICATIONS));
  /* The end of the ribbon stays there. */
  press(RIGHT);
  CHECK(ui.focus == tv_tile(TV_OVERLAY_NOTIFICATIONS));
  /* A refusal says so. */
  tv_mock_refuse("launch", 1);
  press(OK);
  CHECK(tv.error[0] != '\0');
  press(LEFT);
  CHECK(tv.error[0] == '\0');
}

/* Into the settings and a value list and back out, landing each time on the
 * item that was left. */
static void test_stack(void) {
  focus(tv_tile(TV_ENTRY_SETTINGS));
  press(OK);
  CHECK(screen == SCREEN_SETTINGS);
  CHECK(ui.focus == setting_row_id(TV_PICTURE_MODE));
  press(DOWN);
  CHECK(ui.focus == setting_row_id(TV_SOUND_OUTPUT));
  capture("settings");
  press(OK);
  CHECK(screen == SCREEN_CHOICES && choosing == TV_SOUND_OUTPUT);
  /* On the value it has now. */
  CHECK(ui.focus == choice(0));
  capture("choices");
  press(DOWN);
  press(OK);
  CHECK(strcmp(tv_mock_setting("soundOutput"), "external_arc") == 0);
  CHECK(strcmp(tv.settings[TV_SOUND_OUTPUT].value, "external_arc") == 0);
  CHECK(screen == SCREEN_SETTINGS);
  CHECK(ui.focus == setting_row_id(TV_SOUND_OUTPUT));
  /* Back into the list starts on the new value. */
  press(OK);
  CHECK(ui.focus == choice(1));
  press(BACK);
  CHECK(screen == SCREEN_SETTINGS);
  CHECK(ui.focus == setting_row_id(TV_SOUND_OUTPUT));
  press(BACK);
  CHECK(screen == SCREEN_HOME);
  CHECK(ui.focus == tv_tile(TV_ENTRY_SETTINGS));
  /* Back on the first screen puts the launcher out of the way of the app
   * beneath; Home brings it back as it was. */
  press(BACK);
  CHECK(screen == SCREEN_HOME);
  relaunch(true);
  settle();
  CHECK(jf_window_drawable);
  CHECK(screen == SCREEN_HOME && ui.focus == tv_tile(TV_ENTRY_SETTINGS));
}

static void test_settings(void) {
  focus(tv_tile(TV_ENTRY_SETTINGS));
  press(OK);
  /* Left and right step through the values the TV offers, eco left out as
   * not visible, and wrap. */
  CHECK(tv.settings[TV_PICTURE_MODE].choice_count == 5);
  press(RIGHT);
  CHECK(strcmp(tv_mock_setting("pictureMode"), "expert2") == 0);
  press(RIGHT);
  CHECK(strcmp(tv_mock_setting("pictureMode"), "vivid") == 0);
  press(LEFT);
  CHECK(strcmp(tv_mock_setting("pictureMode"), "expert2") == 0);

  /* A refusal puts the value back and says so. */
  tv_mock_refuse("setSystemSettings", 1);
  press(RIGHT);
  CHECK(strcmp(tv.settings[TV_PICTURE_MODE].value, "expert2") == 0);
  CHECK(strcmp(tv_mock_setting("pictureMode"), "expert2") == 0);
  CHECK(tv.error[0] != '\0');

  /* Greyed out in this mode: nothing is written. */
  CHECK(tv.settings[TV_ENERGY_SAVING].choice_count == 6 &&
        !tv.settings[TV_ENERGY_SAVING].active);
  focus(setting_row_id(TV_ENERGY_SAVING));
  const unsigned writes = tv_mock_calls("setSystemSettings");
  press(RIGHT);
  CHECK(tv_mock_calls("setSystemSettings") == writes);
  CHECK(status[0] != '\0');

  press(DOWN);
  CHECK(ui.focus == nav_str("settings.screen_off"));
  press(OK);
  CHECK(tv_mock_calls("turnOffScreen") == 1);
  press(BACK);
  CHECK(screen == SCREEN_HOME);
}

/* webOS 4 and earlier answer only to the older name of the settings service:
 * one refusal, and every request goes there from then on. */
static void test_legacy_settings(void) {
  tv_mock_legacy_settings(true);
  tv.settings[TV_PICTURE_MODE].known = false;
  tv_refresh();
  settle();
  CHECK(tv.settings[TV_PICTURE_MODE].known);
  CHECK(strcmp(tv.settings[TV_PICTURE_MODE].value, "expert2") == 0);
  focus(tv_tile(TV_ENTRY_SETTINGS));
  press(OK);
  press(RIGHT);
  CHECK(strcmp(tv_mock_setting("pictureMode"), "vivid") == 0);
  CHECK(tv.error[0] == '\0');
  press(BACK);
}

/* The Home key with the launcher already up: back to the ribbon, with what
 * changed meanwhile, the focus still on the app it was on. */
static void test_relaunch(void) {
  focus(tile("youtube.leanback.v4"));
  focus(tv_tile(TV_ENTRY_SETTINGS));
  press(OK);
  press(OK);
  CHECK(screen == SCREEN_CHOICES);
  tv_mock_install("org.example.new", "New");
  /* SAM bringing Home up, nothing else being left: in front whatever it
   * had. */
  relaunch(false);
  settle();
  CHECK(screen == SCREEN_HOME && ui_stack_depth(&stack) == 0);
  CHECK(tv.app_count == TV_MOCK_APPS + 1);
  CHECK(ui.focus == tv_tile(TV_ENTRY_SETTINGS));
  focus(tile("youtube.leanback.v4"));
  CHECK(ui.focus == tile("youtube.leanback.v4"));
}

/* The Home key toggles: with the launcher in front and holding the remote it
 * goes out of the way, and the next press brings it back on its first
 * screen. */
static void test_home_key(void) {
  focus(tv_tile(TV_ENTRY_SETTINGS));
  press(OK);
  CHECK(screen == SCREEN_SETTINGS);
  CHECK(jf_window_drawable && jf_window_focused());
  relaunch(true);
  settle();
  CHECK(!jf_window_drawable);
  relaunch(true);
  settle();
  CHECK(jf_window_drawable);
  CHECK(screen == SCREEN_HOME && ui.focus == tv_tile(TV_ENTRY_SETTINGS));
}

int main(void) {
  setenv("SDL_VIDEODRIVER", "offscreen", 0);
  unsetenv("UI_SCRIPT");
  unsetenv("UI_CAPTURE");
  tv_mock_reset();
  char *argv[] = {"home-tests", NULL};
  if (!app_init(1, argv))
    return 1;
  animations_enabled = false;

  test_catalogue();
  test_open();
  test_stack();
  test_settings();
  test_legacy_settings();
  test_relaunch();
  test_home_key();

  app_deinit();
  if (failures == 0)
    printf("home tests passed\n");
  return failures == 0 ? 0 : 1;
}
