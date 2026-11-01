/* The launcher against the TV of src/tv/tv_mock.h: key presses in, focus,
 * screens, Luna calls and the saved layout checked.
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

/* Frames until every answer is in and nothing asks for another frame. Two
 * idle passes in a row: a pass can post to the window's own queue (a raise),
 * which only the next one reads. */
static void settle(void) {
  const uint64_t deadline = ui_now_ns() + 10000000000ull;
  for (unsigned idle = 0;;) {
    jf_window_poll();
    int wait_ms;
    const bool drew = app_step(&wait_ms);
    idle = !drew && tv_pending() == 0 ? idle + 1 : 0;
    if (idle == 2)
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
static nav_id remove_badge(const char *id) {
  return nav_child(nav_str("ribbon"),
                   nav_child(nav_str(id), nav_str("remove")));
}
static nav_id input_card(const char *id) {
  return nav_child(nav_str("inputs"), nav_str(id));
}
#define PENCIL nav_child(nav_str("ribbon"), nav_str("edit"))

static void focus(nav_id id) {
  nav_focus(&ui, id);
  jf_window_frame_requested = true;
  settle();
}

/* The shown apps in order, comma separated. */
static const char *shown_order(void) {
  static char out[512];
  out[0] = '\0';
  for (size_t i = 0; i < layout.shown; i++)
    snprintf(out + strlen(out), sizeof(out) - strlen(out), "%s%s",
             i > 0 ? "," : "", layout.slots[i].id);
  return out;
}

/* The item nav last saw for `id`, or NULL. */
static const nav_item_record *declared(nav_id id) {
  for (size_t i = 0; i < ui.count[ui.current]; i++)
    if (ui.items[ui.current][i].id == id)
      return &ui.items[ui.current][i];
  return NULL;
}

/* ------------------------------------------------------------------ tests */

/* What Home Chooser lists, less what is not an app to open: the hidden app, the
 * input app, and Home - LG's, and this one. */
static void test_catalogue(void) {
  settle();
  CHECK(screen == SCREEN_HOME);
  CHECK(tv.app_count == TV_MOCK_APPS);
  CHECK(strcmp(tv.apps[0].id, "netflix") == 0);
  /* Each icon: from the app's folder where that can be read, and otherwise
   * through Home Chooser, large, then small; for neither, its letter. */
  CHECK(icon_for(&tv.apps[0]) != NULL);
  CHECK(strstr(tv_mock_last("getAppIcon"), "netflix") == NULL);
  CHECK(icon_for(&tv.apps[1]) != NULL);
  CHECK(icon_for(&tv.apps[2]) == NULL);
  CHECK(tv_mock_calls("getAppIcon") == 3);
  /* What Home Chooser gave is kept, and asked for no more. */
  for (size_t i = 0; i < icon_count; i++)
    free(icons[i].data);
  icon_count = 0;
  find_icons();
  settle();
  CHECK(icon_for(&tv.apps[1]) != NULL);
  CHECK(tv_mock_calls("getAppIcon") == 4);
  /* The tile colour each asks for, and which can be uninstalled. */
  CHECK(tv.apps[0].has_color && tv.apps[0].color == 0xe50914);
  CHECK(tv.apps[0].removable);
  /* LG's browser names no colour and stays. */
  CHECK(strcmp(tv.apps[2].id, "com.webos.app.browser") == 0);
  CHECK(!tv.apps[2].has_color && !tv.apps[2].removable);
  CHECK(tv.input_count == 4);
  CHECK(strcmp(tv.inputs[0].device, "PlayStation 5") == 0);
  CHECK(!tv.inputs[1].connected && tv.inputs[2].connected);
  CHECK(strcmp(tv.inputs[2].label, "HDMI 3") == 0);
  CHECK(strcmp(tv.inputs[2].app_id, "com.webos.app.hdmi3") == 0);
  CHECK(!tv.inputs[3].connected);
  /* No saved layout: the TV's order. */
  CHECK(strcmp(shown_order(),
               "netflix,youtube.leanback.v4,com.webos.app.browser") == 0);
  CHECK(ui.focus == tile("netflix"));
  capture("bar");
}

/* What was opened takes the screen and the remote; the Home key brings the
 * launcher back. */
static void home_again(void) {
  CHECK(!jf_window_drawable);
  const unsigned lists = tv_mock_calls("listApps");
  const unsigned fronts = tv_mock_calls("getForegroundApp");
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(jf_window_drawable);
  /* Once for each press, and the apps are not asked for at all. */
  CHECK(tv_mock_calls("getForegroundApp") == fronts + 1);
  CHECK(tv_mock_calls("listApps") == lists);
}

static void test_open(void) {
  press(OK);
  CHECK(tv_mock_calls("launch") == 1);
  CHECK(strstr(tv_mock_last("launch"), "\"netflix\"") != NULL);
  home_again();
  /* Back with the bar where it was. */
  CHECK(screen == SCREEN_HOME && ui.focus == tile("netflix"));
  /* The pencil after the apps, and the end of the bar stays there. */
  presses(RIGHT, 3);
  CHECK(ui.focus == PENCIL);
  press(RIGHT);
  CHECK(ui.focus == PENCIL);
  /* Up from the apps puts the launcher out of the way. */
  press(UP);
  CHECK(!jf_window_drawable);
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(jf_window_drawable && ui.focus == PENCIL);
  /* A refusal says so, and stays in front to. */
  focus(tile("youtube.leanback.v4"));
  tv_mock_refuse("launch", 1);
  press(OK);
  CHECK(tv.error[0] != '\0' && jf_window_drawable);
  press(LEFT);
  CHECK(tv.error[0] == '\0');
}

/* Down brings the inputs up under the apps; Up goes back to the app it left. */
static void test_inputs(void) {
  focus(tile("youtube.leanback.v4"));
  press(DOWN);
  CHECK(ui.focus == input_card("HDMI_1"));
  CHECK(rise.target == 1);
  capture("inputs");
  presses(RIGHT, 2);
  CHECK(ui.focus == input_card("HDMI_3"));
  press(UP);
  CHECK(ui.focus == tile("youtube.leanback.v4"));
  CHECK(rise.target == 0);
  /* And back down to the input it left. */
  press(DOWN);
  CHECK(ui.focus == input_card("HDMI_3"));
  press(OK);
  CHECK(strcmp(tv_mock_last("launch"), "") != 0 &&
        strstr(tv_mock_last("launch"), "com.webos.app.hdmi3") != NULL);
  home_again();
  press(UP);

  /* Eased, the inputs are still below the screen when Down focuses one: the
   * focus stays there while the bar rises to it. */
  animations_enabled = true;
  press(DOWN);
  CHECK(ui.focus == input_card("HDMI_3"));
  CHECK(rise.value > 0.99f);
  press(UP);
  CHECK(ui.focus == tile("youtube.leanback.v4"));
  animations_enabled = false;
}

/* The background of the inputs as last drawn: the one fill that fades in from
 * its top. NULL for none. */
static const loom_command *inputs_background(void) {
  for (size_t i = 0; i < ctx.count; i++)
    if (ctx.commands[i].kind == LOOM_RECTANGLE &&
        ctx.commands[i].mask.width[LOOM_FADE_TOP] > 0)
      return &ctx.commands[i];
  return NULL;
}

/* From nothing at the top of the apps to solid at their foot, as drawn: the one
 * fade, its quad ending with it. */
static void check_inputs_background(void) {
  const loom_command *shade = inputs_background();
  CHECK(shade != NULL);
  if (shade == NULL)
    return;
  const float bottom = (float)gl_height - rise.value * INPUTS_RISE * kit.scale;
  const float tile = TILE_H * kit.scale;
  CHECK(shade->rectangle.color[3] == 255);
  CHECK(fabsf(shade->rect.y - (bottom - tile)) < 1);
  CHECK(fabsf(shade->mask.edge[LOOM_FADE_TOP] - (bottom - tile)) < 1);
  CHECK(fabsf(shade->mask.width[LOOM_FADE_TOP] - tile) < 1);
  CHECK(fabsf(shade->rect.y + shade->rect.h - bottom) < 1);
}

/* The blue of a pixel of the frame as drawn. */
static int frame_blue(const char *path, int x, int y) {
  FILE *file = fopen(path, "rb");
  int width = 0, height = 0, blue = -1;
  if (file != NULL && fscanf(file, "P6 %d %d 255", &width, &height) == 2) {
    fgetc(file);
    fseek(file, (long)(y * width + x) * 3 + 2, SEEK_CUR);
    blue = fgetc(file);
  }
  if (file != NULL)
    fclose(file);
  return blue;
}

/* One frame, read back, with the background's blue checked past the last tile:
 * the frame's clear is black, and the background is 14. Solid from the apps'
 * foot down, half way half way up them, and nothing above. */
static void check_background_in_frame(void) {
  char path[64];
  snprintf(path, sizeof(path), "/tmp/home-tests-%d.ppm", (int)getpid());
  jf_window_poll();
  glViewport(0, 0, (GLsizei)gl_width, (GLsizei)gl_height);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  build_ui(&ctx);
  jf_renderer_draw(renderer, ctx.commands, ctx.count, (float)gl_width,
                   (float)gl_height);
  ui_capture(path);
  jf_window_swap();
  check_inputs_background();
  const int x = (int)gl_width - 8;
  const float bottom = (float)gl_height - rise.value * INPUTS_RISE * kit.scale;
  const float tile = TILE_H * kit.scale;
  /* The row the foot falls in is only part covered. */
  if ((int)bottom + 1 < (int)gl_height)
    CHECK(frame_blue(path, x, (int)bottom + 1) == 14);
  if ((int)bottom + 2 < (int)gl_height)
    CHECK(frame_blue(path, x, (int)gl_height - 1) == 14);
  const int half = frame_blue(path, x, (int)(bottom - tile / 2));
  CHECK(half >= 5 && half <= 9);
  CHECK(frame_blue(path, x, (int)(bottom - tile) + 1) <= 1);
  CHECK(frame_blue(path, x, (int)(bottom - tile) - 4) == 0);
  unlink(path);
}

static void check_background_while(uint32_t key, float until) {
  animations_enabled = true;
  on_key(key, true, false);
  bool mid_way = false;
  for (int i = 0; i < 400 && (i < 2 || rise.value != until); i++) {
    check_background_in_frame();
    mid_way = mid_way || (rise.value > 0.05f && rise.value < 0.95f);
    usleep(1000);
  }
  CHECK(mid_way && rise.value == until);
  animations_enabled = false;
}

/* Up and down with the inputs and at rest, in the pixels of every frame:
 * nothing about the background depends on how far the bar has risen. */
static void test_inputs_background(void) {
  focus(tile("youtube.leanback.v4"));
  CHECK(rise.value == 0);
  check_background_in_frame();
  check_background_while(DOWN, 1);
  check_background_while(UP, 0);
  check_background_in_frame();
  settle();
}

/* Reordering: select picks the app up, Left and Right carry it, select puts
 * it down. */
static void test_move(void) {
  focus(PENCIL);
  press(OK);
  CHECK(screen == SCREEN_EDIT);
  /* On the app beside where the pencil was, which is not there to edit. */
  CHECK(ui.focus == tile("com.webos.app.browser"));
  press(RIGHT);
  CHECK(ui.focus == tile("com.webos.app.browser"));
  focus(tile("netflix"));
  press(OK);
  CHECK(moving);
  capture("moving");
  press(RIGHT);
  CHECK(strcmp(shown_order(),
               "youtube.leanback.v4,netflix,com.webos.app.browser") == 0);
  CHECK(ui.focus == tile("netflix"));
  /* Not past the end. */
  presses(RIGHT, 2);
  CHECK(strcmp(shown_order(),
               "youtube.leanback.v4,com.webos.app.browser,netflix") == 0);
  press(LEFT);
  press(OK);
  CHECK(!moving);
  CHECK(strcmp(shown_order(),
               "youtube.leanback.v4,netflix,com.webos.app.browser") == 0);
  /* Left and Right move the focus again. */
  press(RIGHT);
  CHECK(ui.focus == tile("com.webos.app.browser"));
}

static void point(jf_event_kind kind, float x, float y) {
  jf_event pointer = {.kind = kind};
  pointer.pointer.x = (jf_fixed)x << 8;
  pointer.pointer.y = (jf_fixed)y << 8;
  on_event(&pointer);
  settle();
}

static void point_at(nav_id id) {
  const nav_item_record *item = declared(id);
  CHECK(item != NULL);
  if (item != NULL)
    point(JF_EVENT_POINTER_MOTION, item->rect.x + item->rect.w / 2,
          item->rect.y + item->rect.h / 2);
}

static void button(bool pressed) {
  jf_event click = {.kind = JF_EVENT_POINTER_BUTTON};
  click.button.button = 0x110;
  click.button.pressed = pressed;
  on_event(&click);
  settle();
}

/* The pointer does what the keys do: a click picks an app up, it goes with the
 * pointer, the focus staying on it, and another click puts it down. Or it is
 * dragged, and letting go puts it down. */
static void test_move_with_pointer(void) {
  CHECK(screen == SCREEN_EDIT);
  point_at(tile("netflix"));
  CHECK(ui.focus == tile("netflix"));
  button(true);
  button(false);
  CHECK(moving);
  point_at(tile("com.webos.app.browser"));
  CHECK(strcmp(shown_order(),
               "youtube.leanback.v4,com.webos.app.browser,netflix") == 0);
  CHECK(ui.focus == tile("netflix"));
  /* Across the bar, and put down well above it. */
  point_at(tile("youtube.leanback.v4"));
  CHECK(strcmp(shown_order(),
               "netflix,youtube.leanback.v4,com.webos.app.browser") == 0);
  point(JF_EVENT_POINTER_MOTION, declared(tile("netflix"))->rect.x + 8, 10);
  CHECK(moving);
  button(true);
  CHECK(!moving);
  button(false);
  CHECK(!moving);
  /* Hovering only moves the focus again. */
  point_at(tile("com.webos.app.browser"));
  CHECK(ui.focus == tile("com.webos.app.browser"));
  CHECK(strcmp(shown_order(),
               "netflix,youtube.leanback.v4,com.webos.app.browser") == 0);
  /* Dragged. */
  button(true);
  CHECK(moving);
  point_at(tile("netflix"));
  CHECK(strcmp(shown_order(),
               "com.webos.app.browser,netflix,youtube.leanback.v4") == 0);
  button(false);
  CHECK(!moving);
  /* Back as it was, by the keys, which hand over from the pointer. */
  point_at(tile("com.webos.app.browser"));
  press(OK);
  CHECK(moving && !pointer_carrying);
  presses(RIGHT, 2);
  press(OK);
  CHECK(!moving);
  CHECK(strcmp(shown_order(),
               "netflix,youtube.leanback.v4,com.webos.app.browser") == 0);
  nav_pointer_leave(&ui);
  /* The order the keys' journey goes on from. */
  focus(tile("netflix"));
  press(OK);
  press(RIGHT);
  press(OK);
  CHECK(strcmp(shown_order(),
               "youtube.leanback.v4,netflix,com.webos.app.browser") == 0);
}

/* The X over an app: hide it, or uninstall it where the TV lets. */
static void test_hide(void) {
  focus(tile("netflix"));
  press(UP);
  CHECK(ui.focus == remove_badge("netflix"));
  capture("edit");
  press(OK);
  CHECK(screen == SCREEN_CONFIRM && ui.focus == nav_str("confirm.hide"));
  capture("confirm");
  press(OK);
  CHECK(screen == SCREEN_EDIT);
  CHECK(strcmp(shown_order(), "youtube.leanback.v4,com.webos.app.browser") ==
        0);
  CHECK(layout.count == 3 && layout.slots[2].hidden);
  /* On the app that took its place. */
  CHECK(ui.focus == tile("com.webos.app.browser"));
  /* The hidden ones after the shown ones, still in reach. */
  press(RIGHT);
  CHECK(ui.focus == tile("netflix"));
  capture("hidden");

  /* LG's own browser can only be hidden: there is nothing beside Hide. */
  focus(remove_badge("com.webos.app.browser"));
  press(OK);
  CHECK(screen == SCREEN_CONFIRM);
  press(RIGHT);
  CHECK(ui.focus == nav_str("confirm.hide"));
  press(BACK);
  CHECK(screen == SCREEN_EDIT &&
        ui.focus == remove_badge("com.webos.app.browser"));

  /* YouTube can go altogether. */
  const unsigned list_calls = tv_mock_calls("listApps");
  focus(remove_badge("youtube.leanback.v4"));
  press(OK);
  press(RIGHT);
  CHECK(ui.focus == nav_str("confirm.remove"));
  press(OK);
  CHECK(tv_mock_calls("uninstallApp") == 1);
  /* Said by Home Chooser, not asked for. */
  CHECK(tv_mock_calls("listApps") == list_calls);
  CHECK(strstr(tv_mock_last("uninstallApp"), "youtube.leanback.v4") != NULL);
  CHECK(screen == SCREEN_EDIT);
  CHECK(tv.app_count == TV_MOCK_APPS - 1);
  CHECK(layout_find(&layout, "youtube.leanback.v4") < 0);
  CHECK(strcmp(shown_order(), "com.webos.app.browser") == 0);
  CHECK(ui.focus == tile("com.webos.app.browser"));
}

/* A hidden app picked up is shown again, and put where it is wanted. Back
 * saves the lot. */
static void test_show_and_save(void) {
  focus(tile("netflix"));
  press(OK);
  CHECK(moving);
  CHECK(strcmp(shown_order(), "com.webos.app.browser,netflix") == 0);
  press(LEFT);
  press(OK);
  CHECK(strcmp(shown_order(), "netflix,com.webos.app.browser") == 0);
  focus(remove_badge("com.webos.app.browser"));
  press(OK);
  press(OK);
  CHECK(layout.shown == 1);
  press(BACK);
  CHECK(screen == SCREEN_HOME && ui.focus == PENCIL);
  home_layout saved;
  layout_load(&saved, layout_path);
  CHECK(saved.count == 2 && saved.shown == 1);
  CHECK(strcmp(saved.slots[0].id, "netflix") == 0 && !saved.slots[0].hidden);
  CHECK(strcmp(saved.slots[1].id, "com.webos.app.browser") == 0 &&
        saved.slots[1].hidden);
  /* The bar shows only what is shown. */
  press(LEFT);
  CHECK(ui.focus == tile("netflix"));
  press(LEFT);
  CHECK(ui.focus == tile("netflix"));
}

/* Something installed meanwhile goes after the shown apps, before the hidden
 * ones; the Home key with the launcher up goes back to its first screen. */
static void test_relaunch(void) {
  focus(PENCIL);
  press(OK);
  CHECK(screen == SCREEN_EDIT);
  tv_mock_install("org.example.new", "New");
  relaunch(RELAUNCH_OTHER);
  settle();
  CHECK(screen == SCREEN_HOME && ui_stack_depth(&stack) == 0);
  CHECK(tv.app_count == TV_MOCK_APPS);
  CHECK(strcmp(shown_order(), "netflix,org.example.new") == 0);
  CHECK(layout.count == 3 && layout.slots[2].hidden);
  CHECK(ui.focus == PENCIL);
}

/* The Home key toggles: with the launcher in front and holding the remote it
 * goes out of the way, and the next press brings it back on its first
 * screen. */
static void test_home_key(void) {
  focus(PENCIL);
  press(OK);
  CHECK(screen == SCREEN_EDIT);
  CHECK(jf_window_drawable && jf_window_focused());
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(!jf_window_drawable);
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(jf_window_drawable);
  CHECK(screen == SCREEN_HOME && ui.focus == PENCIL);
}

/* Coming up, the launcher starts on what is in front beneath it - unless a
 * key got there first. */
static void test_foreground(void) {
  tv_mock_foreground("netflix");
  press(BACK);
  CHECK(!jf_window_drawable);
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(ui.focus == tile("netflix"));
  /* An input is its card, with the bar risen to it. */
  tv_mock_foreground("com.webos.app.hdmi3");
  press(BACK);
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(ui.focus == input_card("HDMI_3"));
  CHECK(rise.target == 1);
  /* The window coming up under the pointer, resting over a tile, does not
   * take the focus there; moving it does. */
  const nav_item_record *under = declared(tile("netflix"));
  jf_event pointer = {.kind = JF_EVENT_POINTER_ENTER};
  pointer.pointer.x = (jf_fixed)(under->rect.x + under->rect.w / 2) << 8;
  pointer.pointer.y = (jf_fixed)(under->rect.y + under->rect.h / 2) << 8;
  on_event(&pointer);
  settle();
  CHECK(ui.focus == input_card("HDMI_3"));
  pointer.kind = JF_EVENT_POINTER_MOTION;
  on_event(&pointer);
  CHECK(ui.focus == tile("netflix"));
  nav_pointer_leave(&ui);
  focus(input_card("HDMI_3"));
  press(UP);
  CHECK(ui.focus == tile("netflix"));
  tv_mock_foreground("");
}

/* How far a tile has risen, as last drawn. */
static float lift_now(nav_id id) {
  for (size_t i = 0; i < sizeof(motions) / sizeof(motions[0]); i++)
    if (motions[i].id == id)
      return motions[i].lift.value;
  return -1;
}

/* Opening, nothing eases: the tile in front is up, the bar where it belongs,
 * from the first frame that shows them. Keys after that ease again. */
static void test_opens_instantly(void) {
  animations_enabled = true;
  const char *front = layout.slots[2].id;
  focus(tile(layout.slots[0].id));
  tv_mock_foreground(front);
  press(BACK);
  relaunch(RELAUNCH_HOME_KEY);
  for (int i = 0; i < 200 && ui.focus != tile(front); i++) {
    jf_window_poll();
    int wait_ms;
    app_step(&wait_ms);
  }
  /* The frame that moved the focus there. */
  jf_window_frame_requested = true;
  jf_window_poll();
  int wait_ms;
  app_step(&wait_ms);
  CHECK(ui.focus == tile(front));
  CHECK(lift_now(tile(front)) == 1.0f);
  CHECK(lift_now(tile(layout.slots[0].id)) == 0.0f);
  CHECK(!instant);
  /* A key eases again. */
  on_key(LEFT, true, false);
  jf_window_poll();
  app_step(&wait_ms);
  CHECK(lift_now(tile(front)) < 1.0f);
  settle();
  animations_enabled = false;
  tv_mock_foreground("");
}

/* A bar longer than the screen scrolls along with the app being carried. */
static void test_carried_out_of_view(void) {
  char id[16];
  /* More than the screen holds. */
  for (int i = 0; i < 11; i++) {
    snprintf(id, sizeof(id), "org.more.%d", i);
    tv_mock_install(id, id);
  }
  tv_refresh();
  settle();
  focus(PENCIL);
  press(OK);
  focus(tile("netflix"));
  press(OK);
  presses(RIGHT, 14);
  CHECK(layout_find(&layout, "netflix") == (int)layout.shown - 1);
  CHECK(ui.focus == tile("netflix"));
  const nav_item_record *carried = declared(tile("netflix"));
  CHECK(carried != NULL &&
        carried->rect.x + carried->rect.w <= (float)gl_width + 1);
  press(OK);
  press(BACK);

  /* The pointer resting at either side scrolls the bar that way, to its
   * end. */
  const nav_id first = tile(layout.slots[0].id);
  focus(first);
  const float first_x = declared(first)->rect.x;
  nav_pointer(&ui, (float)gl_width - 4, (float)gl_height - 40);
  jf_window_frame_requested = true;
  settle();
  const nav_item_record *pencil = declared(PENCIL);
  CHECK(pencil != NULL && pencil->rect.x + pencil->rect.w <= (float)gl_width);
  CHECK(declared(first)->rect.x < first_x);
  nav_pointer(&ui, 2, (float)gl_height - 40);
  jf_window_frame_requested = true;
  settle();
  CHECK(declared(first)->rect.x == first_x);
  /* Not above the bar. */
  nav_pointer(&ui, (float)gl_width - 4, 10);
  jf_window_frame_requested = true;
  settle();
  CHECK(declared(first)->rect.x == first_x);
  nav_pointer_leave(&ui);
}

/* Another app as Home, the owner is asked in Home Chooser and the launcher goes
 * out of its way; what Home Chooser still needs is said on the line. */
static void test_home_chooser(void) {
  const unsigned launches = tv_mock_calls("launch");
  tv_mock_home_chooser(true, true, false);
  tv_check_home();
  settle();
  CHECK(!jf_window_drawable && !tv.not_chosen);
  CHECK(tv_mock_calls("launch") == launches + 1);
  CHECK(strstr(tv_mock_last("launch"), "\"org.webosbrew.homechooser\"") !=
        NULL);
  CHECK(strstr(tv_mock_last("launch"), "\"request\":\"" APP_ID "\"") != NULL);
  tv_mock_home_chooser(true, true, true);
  home_again();

  tv_mock_home_chooser(false, true, true);
  tv_check_home();
  settle();
  CHECK(strstr(tv.error, "Open Home Chooser") != NULL);
  press(LEFT);
  tv_mock_home_chooser(true, false, true);
  tv_check_home();
  settle();
  CHECK(strstr(tv.error, "Install this Home again") != NULL);
  press(LEFT);
  tv_mock_refuse("getInfo", 1);
  tv_check_home();
  settle();
  CHECK(strstr(tv.error, "Home Chooser is needed") != NULL);
  press(LEFT);
  tv_mock_home_chooser(true, true, true);
  CHECK(tv_mock_calls("launch") == launches + 1 && jf_window_drawable);
}

/* Home Chooser going away, as when it is installed again: the list asked for
 * again next time, and until then the line says why and the inputs still work.
 * One that cannot subscribe is asked at every press. */
static void test_apps_refused(void) {
  tv_mock_lose_subscribers();
  settle();
  CHECK(strstr(tv.error, "could not be listed") != NULL);
  press(LEFT);
  tv.apps_loaded = false;
  tv_mock_refuse("listApps", 1);
  tv_refresh();
  settle();
  CHECK(!tv.apps_loaded && strstr(tv.error, "could not be listed") != NULL);
  CHECK(jf_window_drawable && declared(input_card("HDMI_1")) != NULL);
  capture("refused");
  press(DOWN);
  CHECK(ui.focus == input_card("HDMI_1"));
  press(UP);
  CHECK(!jf_window_drawable);
  tv_mock_subscribable(false);
  const unsigned lists = tv_mock_calls("listApps");
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(jf_window_drawable && tv.apps_loaded);
  CHECK(tv_mock_calls("listApps") == lists + 1);
  /* And what went wrong before is no longer said. */
  CHECK(tv.error[0] == '\0');
  press(BACK);
  relaunch(RELAUNCH_HOME_KEY);
  settle();
  CHECK(tv_mock_calls("listApps") == lists + 2);
  tv_mock_subscribable(true);
  tv_refresh();
  settle();
  /* The bus refusing Home Chooser is it not being set up. */
  tv_mock_refuse("getInfo", 1);
  tv_mock_refuse_text("Denied method call");
  tv_check_home();
  settle();
  CHECK(strstr(tv.error, "Open Home Chooser") != NULL);
  tv_mock_refuse_text(NULL);
  press(LEFT);
}

/* The file itself: a layout written and read back, and one the TV's apps
 * have moved on from. */
static void test_layout_file(void) {
  home_layout written = {0};
  const tv_app apps[] = {{.id = "a"}, {.id = "b"}, {.id = "c"}};
  layout_sync(&written, apps, 3);
  layout_hide(&written, "a");
  layout_move(&written, "c", -1);
  CHECK(layout_save(&written, layout_path));
  home_layout read;
  layout_load(&read, layout_path);
  CHECK(read.count == 3 && read.shown == 2);
  CHECK(strcmp(read.slots[0].id, "c") == 0 &&
        strcmp(read.slots[1].id, "b") == 0);
  CHECK(strcmp(read.slots[2].id, "a") == 0 && read.slots[2].hidden);
  /* b uninstalled, d new: d after the shown, before the hidden. */
  const tv_app now[] = {{.id = "a"}, {.id = "c"}, {.id = "d"}};
  layout_sync(&read, now, 3);
  CHECK(read.count == 3 && read.shown == 2);
  CHECK(strcmp(read.slots[0].id, "c") == 0 &&
        strcmp(read.slots[1].id, "d") == 0);
  CHECK(read.slots[2].hidden);
}

int main(void) {
  setenv("SDL_VIDEODRIVER", "offscreen", 0);
  unsetenv("UI_SCRIPT");
  unsetenv("UI_CAPTURE");
  char path[64];
  snprintf(path, sizeof(path), "/tmp/home-tests-%d.txt", (int)getpid());
  setenv("HOME_LAYOUT", path, 1);
  unlink(path);
  char cache[64];
  snprintf(cache, sizeof(cache), "/tmp/home-tests-%d-icons", (int)getpid());
  setenv("HOME_ICON_CACHE", cache, 1);
  tv_mock_reset();
  if (!app_init())
    return 1;
  animations_enabled = false;

  test_catalogue();
  test_open();
  test_inputs();
  test_inputs_background();
  test_move();
  test_move_with_pointer();
  test_hide();
  test_show_and_save();
  test_relaunch();
  test_home_key();
  test_foreground();
  test_carried_out_of_view();
  test_opens_instantly();
  test_home_chooser();
  test_apps_refused();
  test_layout_file();

  app_deinit();
  unlink(path);
  char remove_cache[96];
  snprintf(remove_cache, sizeof(remove_cache), "rm -rf %s", cache);
  if (system(remove_cache) != 0)
    failures++;
  if (failures == 0)
    printf("home tests passed\n");
  return failures == 0 ? 0 : 1;
}
