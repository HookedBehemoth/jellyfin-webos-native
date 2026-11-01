/* The whole app against api_mock.c: key presses in, focus and screens checked.
 *
 * main.c is included rather than linked, so every screen's state is in reach,
 * and the frames are the real ones - window, renderer and all - on SDL's
 * offscreen driver. The tests are one journey through the app in order, as its
 * state is the app's.
 *
 * Run with `ctest --preset host`, or ./build-host/tests/jf-ui-tests [animated]:
 * the journey runs with animations off, or with the argument on. The home-row
 * walk tries both either way. UI_TEST_CAPTURE=<dir> saves every checked frame
 * there as a PPM.
 */
#define JF_APP_TEST
#include "app/main.c"

#include "api_mock.h"

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr,                                                          \
              "FAIL %s:%d: %s (screen %d, focus %08x, status \"%s\")\n",       \
              __FILE__, __LINE__, #cond, (int)screen, (unsigned)ui.focus,      \
              status_text);                                                    \
      failures++;                                                              \
    }                                                                          \
  } while (0)

static int failures;

/* Frames until nothing is left to do: every request not held by the mock
 * answered and consumed, and no animation asking for another frame. */
static void test_ui_settle(void) {
  const uint64_t deadline = ui_now_ns() + 10000000000ull;
  for (;;) {
    jf_window_poll();
    int wait_ms;
    const bool drew = app_step(&wait_ms);
    const bool busy = jf_fetcher_pending(&fetcher) > mock_held();
    if (!drew && !busy)
      return;
    if (ui_now_ns() > deadline) {
      fprintf(stderr, "FAIL: the UI did not settle (screen %d, %zu pending)\n",
              (int)screen, jf_fetcher_pending(&fetcher));
      failures++;
      return;
    }
    if (!drew)
      usleep(1000);
  }
}

static void press(uint32_t code) {
  on_key(code, true, false);
  on_key(code, false, false);
  test_ui_settle();
}

enum { UP = 103, DOWN = 108, LEFT = 105, RIGHT = 106, OK = 28, BACK = 158 };

static void presses(uint32_t code, unsigned count) {
  while (count-- > 0)
    press(code);
}

/* The last frame's record of the focused item, or NULL when it drew none. */
static const nav_item_record *focused(void) {
  for (size_t i = 0; i < ui.count[ui.current]; i++)
    if (ui.items[ui.current][i].id == ui.focus)
      return &ui.items[ui.current][i];
  return NULL;
}

/* Drawn whole, inside its clip and on screen: what the viewer sees selected. */
static bool focus_in_view(void) {
  const nav_item_record *item = focused();
  if (item == NULL)
    return false;
  const loom_rect r = item->rect, c = item->clip;
  return r.x >= c.x - 0.5f && r.y >= c.y - 0.5f &&
         r.x + r.w <= c.x + c.w + 0.5f && r.y + r.h <= c.y + c.h + 0.5f &&
         r.x >= -0.5f && r.y >= -0.5f && r.x + r.w <= (float)gl_width + 0.5f &&
         r.y + r.h <= (float)gl_height + 0.5f;
}

static void capture(const char *name) {
  const char *dir = getenv("UI_TEST_CAPTURE");
  if (dir == NULL)
    return;
  char path[512];
  snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
  ui_capture(path);
}

/* Ids as the screens declare them, scopes and all. */
static nav_id home_card(size_t row, const char *id) {
  return nav_child(nav_child(nav_str("home"), nav_int(row)), nav_str(id));
}
static nav_id grid_card_id(const char *library, size_t index) {
  return nav_child(nav_str(library), nav_int(index));
}
static nav_id in_list(const char *list, const char *id) {
  return nav_child(nav_str(list), nav_str(id));
}

/* Where the grid's focus is, as a card index. */
static size_t grid_focus(void) {
  for (size_t index = 0; index < grid_total; index++)
    if (ui.focus == grid_card_id(grid_parent, index))
      return index;
  return SIZE_MAX;
}

static void go_home(void) {
  if (sidebar_open)
    press(BACK);
  while (screen != SCREEN_HOME && ui_stack_depth(&stack) > 0)
    press(BACK);
  CHECK(screen == SCREEN_HOME);
}

/* ------------------------------------------------------------------ tests */

static void test_sign_in(void) {
  /* Discovery asks again for 30 s; the mock's servers are all there the first
   * time. */
  discovery_until = 0;
  test_ui_settle();
  CHECK(screen == SCREEN_SERVER);
  CHECK(discovered_count == 1);
  CHECK(ui.focus == nav_str("server.url"));

  /* Typed in, a server is probed first; one that does not answer says so. */
  set_text(server_url, sizeof(server_url), "http://mock.test");
  press(DOWN);
  CHECK(ui.focus == nav_str("server.connect"));
  mock_fail(JF_JOB_PROBE, 0, 1);
  press(OK);
  CHECK(screen == SCREEN_SERVER && status_error && !connecting);
  CHECK(ui.focus == nav_str("server.connect"));

  press(DOWN);
  CHECK(ui.focus == nav_str("http://mock.test"));
  press(OK);
  CHECK(screen == SCREEN_AUTH);
  CHECK(ui.focus == nav_str("auth.username"));
  /* Back to the list lands on the server it was left from, not the top. */
  press(BACK);
  CHECK(screen == SCREEN_SERVER);
  CHECK(ui.focus == nav_str("http://mock.test"));
  press(OK);
  CHECK(screen == SCREEN_AUTH);
  /* And out of Quick Connect, on the button that opened it. */
  mock_hold(JF_JOB_QUICK_POLL);
  nav_focus(&ui, nav_str("auth.quick"));
  test_ui_settle();
  press(OK);
  CHECK(screen == SCREEN_QUICK);
  press(BACK);
  CHECK(screen == SCREEN_AUTH);
  CHECK(ui.focus == nav_str("auth.quick"));
  mock_release(JF_JOB_QUICK_POLL);
  test_ui_settle();
  CHECK(screen == SCREEN_AUTH);
  nav_focus(&ui, nav_str("auth.username"));
  test_ui_settle();

  set_text(username, sizeof(username), "tester");
  set_text(password, sizeof(password), "secret");
  /* Down from the full-width password field goes to the button more nearly
   * under its middle, which is Quick Connect. */
  presses(DOWN, 2);
  CHECK(ui.focus == nav_str("auth.quick"));
  press(LEFT);
  CHECK(ui.focus == nav_str("auth.sign_in"));

  mock_fail(JF_JOB_LOGIN, 401, 1);
  press(OK);
  CHECK(screen == SCREEN_AUTH && status_error);
  CHECK(ui.focus == nav_str("auth.sign_in"));
  press(OK);
  CHECK(screen == SCREEN_HOME);
  CHECK(rows[ROW_RESUME].count == MOCK_RESUME);
  CHECK(ui.focus == home_card(ROW_RESUME, "movie-0"));
  capture("home");
}

/* Right to the end of a row and back: the focused card is in view after every
 * press, however the row scrolls, and Left off the first card opens the
 * sidebar. */
/* Whether this run has animations on, which every test but the row walk uses.
 */
static bool animated;

static void walk_row(bool animate) {
  animations_enabled = animate;
  go_home();
  nav_focus(&ui, home_card(ROW_RESUME, "movie-0"));
  test_ui_settle();
  for (unsigned i = 1; i < MOCK_RESUME; i++) {
    press(RIGHT);
    CHECK(ui.focus == home_card(ROW_RESUME, mock_id("movie-%u", i)));
    CHECK(focus_in_view());
  }
  capture(animate ? "row-end-animated" : "row-end");
  press(RIGHT);
  CHECK(ui.focus ==
        home_card(ROW_RESUME, mock_id("movie-%u", MOCK_RESUME - 1)));
  for (unsigned i = MOCK_RESUME - 1; i-- > 0;) {
    press(LEFT);
    CHECK(ui.focus == home_card(ROW_RESUME, mock_id("movie-%u", i)));
    CHECK(focus_in_view());
  }
  press(LEFT);
  CHECK(sidebar_open);
  press(RIGHT);
  CHECK(!sidebar_open);
  CHECK(ui.focus == home_card(ROW_RESUME, "movie-0"));
}

static void test_rows(void) {
  /* Animated first: that walk loads the posters, whose arrival draws frames of
   * its own and would hide a scroll that asks for none. */
  walk_row(true);
  walk_row(false);
  animations_enabled = animated;

  /* The server's order: libraries above, what was added below. */
  press(DOWN);
  CHECK(ui.focus == home_card(ROW_NEXT_UP, "episode-0-2-1"));
  press(DOWN);
  CHECK(ui.focus == home_card(ROW_LATEST, "movie-100"));
  CHECK(strcmp(rows[ROW_LATEST].title, "Recently Added in Movies") == 0);
  CHECK(strcmp(rows[ROW_LATEST + 1].title, "Recently Added in Shows") == 0);
  CHECK(rows[ROW_LATEST + 1].count == MOCK_LATEST &&
        !row_shown(ROW_LATEST + 2));
  press(DOWN);
  CHECK(ui.focus == home_card(ROW_LATEST + 1, "series-0"));
  CHECK(focus_in_view());
  capture("home-latest");
  presses(UP, 3);
  CHECK(ui.focus == home_card(ROW_RESUME, "movie-0"));
  press(UP);
  CHECK(ui.focus == home_card(ROW_LIBRARIES, "library-movies"));
  CHECK(focus_in_view());
  press(DOWN);
  CHECK(ui.focus == home_card(ROW_RESUME, "movie-0"));

  /* The server says something changed: the rows are asked again at once on
   * this screen, and on coming back to it from another. */
  const unsigned asked = mock_calls(JF_JOB_RESUME);
  mock_event(NULL, 0);
  test_ui_settle();
  CHECK(mock_calls(JF_JOB_RESUME) == asked + 1);
  CHECK(ui.focus == home_card(ROW_RESUME, "movie-0"));
  press(OK);
  CHECK(screen == SCREEN_DETAILS);
  mock_event(NULL, 0);
  test_ui_settle();
  CHECK(mock_calls(JF_JOB_RESUME) == asked + 1);
  press(BACK);
  CHECK(screen == SCREEN_HOME && mock_calls(JF_JOB_RESUME) == asked + 2);
}

/* A token the server stopped accepting mid-session: the stored password signs
 * in again and the rows come back. */
static void test_expired_token(void) {
  go_home();
  mock_fail(JF_JOB_RESUME, 401, 1);
  load_home();
  test_ui_settle();
  CHECK(screen == SCREEN_HOME);
  CHECK(rows[ROW_RESUME].count == MOCK_RESUME);
  CHECK(mock_calls(JF_JOB_LOGIN) >= 3);
}

static void open_library(const char *id) {
  go_home();
  nav_focus(&ui, home_card(ROW_LIBRARIES, id));
  test_ui_settle();
  press(OK);
  CHECK(screen == SCREEN_GRID);
  CHECK(strcmp(grid_parent, id) == 0);
}

static void test_grid(void) {
  open_library("library-movies");
  CHECK(grid_total == MOCK_MOVIES);
  CHECK(ui.focus == grid_card_id("library-movies", 0));
  press(RIGHT);
  CHECK(ui.focus == grid_card_id("library-movies", 1));
  press(LEFT);

  /* Down past the first page, with the page it needs timing out on the way: the
   * cards there stay blank, and the next frame asks again. */
  mock_hold(JF_JOB_CHILDREN);
  mock_fail(JF_JOB_CHILDREN, 0, 1);
  for (unsigned i = 0; i < 30 && grid_card(grid_focus()) != NULL; i++) {
    press(DOWN);
    CHECK(focus_in_view());
  }
  CHECK(grid_card(grid_focus()) == NULL);
  CHECK(mock_held() > 0);
  mock_release(JF_JOB_CHILDREN);
  test_ui_settle();
  CHECK(screen == SCREEN_GRID);
  CHECK(!status_error);
  CHECK(grid_card(grid_focus()) != NULL);
  CHECK(focus_in_view());
  capture("grid");

  /* Back into a grid keeps its place. */
  const nav_id place = ui.focus;
  press(OK);
  CHECK(screen == SCREEN_DETAILS);
  press(BACK);
  CHECK(screen == SCREEN_GRID);
  CHECK(ui.focus == place);
  CHECK(focus_in_view());
}

/* A library that answers 404 part-way: the app gives up on the session. */
static void test_grid_not_found(void) {
  open_library("library-movies");
  mock_fail(JF_JOB_CHILDREN, 404, 0);
  presses(DOWN, 12);
  CHECK(screen == SCREEN_AUTH && status_error);
  mock_reset();
  nav_focus(&ui, nav_str("auth.sign_in"));
  test_ui_settle();
  press(OK);
  CHECK(screen == SCREEN_HOME);
}

static void test_series(void) {
  open_library("library-shows");
  const unsigned items = mock_calls(JF_JOB_ITEM);
  press(OK);
  CHECK(screen == SCREEN_DETAILS);
  CHECK(strcmp(detail.id, "series-0") == 0);
  /* One request: the grid card and the seasons carry the show. */
  CHECK(mock_calls(JF_JOB_ITEM) == items);
  CHECK(strcmp(detail.backdrop_id, "series-0") == 0);
  CHECK(detail.remaining == (MOCK_SEASONS - 1) * MOCK_EPISODES);
  /* The first season is watched, so it opens on the second. */
  CHECK(ui.focus == in_list("series-0", "season-0-2"));
  press(LEFT);
  CHECK(ui.focus == in_list("series-0", "season-0-1"));
  press(RIGHT);
  press(OK);
  CHECK(screen == SCREEN_SEASON);
  CHECK(ui.focus == in_list("season-0-2", "episode-0-2-1"));
  for (unsigned e = 2; e <= MOCK_EPISODES; e++) {
    press(DOWN);
    CHECK(ui.focus == in_list("season-0-2", mock_id("episode-0-2-%u", e)));
    CHECK(focus_in_view());
  }
  capture("season");
  press(BACK);
  CHECK(screen == SCREEN_DETAILS);
  CHECK(ui.focus == in_list("series-0", "season-0-2"));
  press(BACK);
  CHECK(screen == SCREEN_GRID);
  CHECK(ui.focus == grid_card_id("library-shows", 0));
}

/* Back out of a details screen whose item is still loading: when it lands late,
 * it does not take the screen back. */
static void test_slow_details(void) {
  open_library("library-movies");
  mock_hold(JF_JOB_ITEM);
  press(OK);
  CHECK(screen == SCREEN_DETAILS);
  press(BACK);
  CHECK(screen == SCREEN_GRID);
  mock_release(JF_JOB_ITEM);
  test_ui_settle();
  CHECK(screen == SCREEN_GRID);
  CHECK(ui.focus == grid_card_id("library-movies", 0));
}

static void test_playback(void) {
  open_library("library-shows");
  press(OK);
  press(OK);
  CHECK(screen == SCREEN_SEASON);
  const nav_id episode = ui.focus;
  press(OK);
  CHECK(screen == SCREEN_DETAILS);
  CHECK(ui.focus == nav_str("details.play"));
  press(OK);
  CHECK(screen == SCREEN_PLAYBACK);
  CHECK(ui.focus == button_id(PLAY_PAUSE_INDEX));
  capture("playback");
  press(RIGHT);
  CHECK(ui.focus == button_id(PLAY_PAUSE_INDEX + 1));
  press(UP);
  CHECK(ui.focus == seek_bar_id());
  press(DOWN);

  /* Another app opened over it minimises the window: playback pauses, and is
   * still paused when the window comes back. */
  CHECK(!playback_paused);
  jf_window_minimize();
  test_ui_settle();
  CHECK(playback_paused);
  jf_window_post_raise();
  test_ui_settle();
  CHECK(jf_window_drawable && playback_paused);
  toggle_playback();
  test_ui_settle();

  press(KEY_SUBTITLE);
  CHECK(subtitle_picker);
  CHECK(focus_in_view());
  CHECK(ui.focus == picker_row_id(track_row(-1))); /* off, under download */
  press(DOWN);
  CHECK(focus_in_view());
  capture("subtitles");

  /* Right on a track opens its appearance and timing. An SRT has no ASS row,
   * so the text colour comes first; left and right change it at once. */
  const int track = jf_player_subtitle_current();
  CHECK(track == 0);
  press(RIGHT);
  CHECK(subtitle_settings && !subtitle_picker);
  CHECK(jf_player_subtitle_current() == track);
  CHECK(ui.focus == setting_row_id(SETTING_COLOR));
  const int color = sub_style.color;
  press(RIGHT);
  CHECK(sub_style.color == (color + 1) % SWATCH_COUNT);
  press(LEFT);
  CHECK(sub_style.color == color);
  /* The outline's own rows go with it. */
  press(DOWN);
  CHECK(ui.focus == setting_row_id(SETTING_OUTLINE));
  press(OK);
  CHECK(!setting_shown(SETTING_OUTLINE_WIDTH));
  press(DOWN);
  CHECK(ui.focus == setting_row_id(SETTING_INLINE));
  press(UP);
  press(OK);
  CHECK(setting_shown(SETTING_OUTLINE_WIDTH));
  /* The delay is the track's, kept on the server. */
  nav_focus(&ui, setting_row_id(SETTING_DELAY));
  presses(RIGHT, 3);
  CHECK(*current_delay() == 300);
  presses(DOWN, SETTING_COUNT);
  CHECK(focus_in_view());
  /* An outline that shows against the black of the video hole, and the
   * stand-in line, which only a style row brings up. */
  const int outline_color = sub_style.outline_color;
  sub_style.outline_color = 1;
  sub_style.outline_width = 5;
  nav_focus(&ui, setting_row_id(SETTING_OUTLINE_WIDTH));
  jf_window_frame_requested = true;
  test_ui_settle();
  capture("subtitle-settings");
  sub_style.outline_color = outline_color;
  sub_style.outline_width = 3;
  press(BACK);
  CHECK(subtitle_picker && !subtitle_settings);
  CHECK(ui.focus == picker_row_id(track_row(track)));
  CHECK(strstr(mock_saved_delays(), "\"300\"") != NULL);

  /* A bitmap track has no style to change, only its timing. */
  presses(DOWN, 2);
  CHECK(jf_player_subtitle_current() == 2);
  press(RIGHT);
  CHECK(ui.focus == setting_row_id(SETTING_DELAY));
  CHECK(!setting_enabled(SETTING_COLOR) && !setting_enabled(SETTING_SUGGEST));
  press(BACK);

  /* The first row searches the server's providers; a result downloads and
   * comes back as a track of its own, selected. */
  presses(UP, 4);
  CHECK(ui.focus == picker_row_id(0));
  press(OK);
  CHECK(search.open && !subtitle_picker);
  CHECK(ui.focus == search_row_id(SEARCH_ROW_LANGUAGE));
  CHECK(search.count == 2);
  CHECK(sources.loaded && sources_on() == 0); /* "None on", in yellow */
  capture("subtitle-search");
  /* Sources, above the language: OpenSubtitles offered for installing; once
   * installed, on after a restart, which a mistaken press can still switch
   * off and on again; more sources a repository away; and a restart offered
   * to apply it all. */
  press(UP);
  press(RIGHT);
  CHECK(sources.open && !search.open);
  CHECK(sources.count == 1 && sources.items[0].status[0] == '\0');
  CHECK(offer_repository());
  press(OK);
  for (int i = 0; i < 100 && sources.items[0].status[0] == '\0'; i++) {
    usleep(20000);
    jf_window_frame_requested = true;
    test_ui_settle();
  }
  CHECK(strcmp(sources.items[0].status, "Restart") == 0);
  press(OK);
  test_ui_settle();
  CHECK(strcmp(sources.items[0].status, "Disabled") == 0);
  press(OK);
  test_ui_settle();
  CHECK(strcmp(sources.items[0].status, "Restart") == 0);
  press(DOWN);
  press(OK);
  test_ui_settle();
  CHECK(sources.count == 2 && strcmp(sources.items[1].name, "subbuzz") == 0);
  CHECK(!offer_repository());
  CHECK(ui.focus ==
            nav_child(nav_str("subtitle-sources"), nav_int(restart_row())) ||
        focus_in_view());
  nav_focus(&ui,
            nav_child(nav_str("subtitle-sources"), nav_int(restart_row())));
  press(OK);
  CHECK(sources.restart_armed && mock_calls(JF_JOB_RESTART) == 0);
  capture("subtitle-sources");
  press(UP);
  CHECK(!sources.restart_armed);
  press(BACK);
  CHECK(search.open && ui.focus == search_row_id(SEARCH_ROW_SOURCES));
  test_ui_settle();
  press(DOWN);
  press(DOWN);
  press(OK);
  /* The server lists it only after a few looks (see the mock), and it is
   * read back before the picker returns on it. */
  for (int i = 0; i < 200 && search.open; i++) {
    usleep(20000);
    jf_window_frame_requested = true;
    test_ui_settle();
  }
  CHECK(mock_calls(JF_JOB_SUBTITLE_CHECK) == 1);
  CHECK(!search.open && subtitle_picker);
  CHECK(jf_player_subtitle_count() == 4);
  CHECK(strcmp(jf_player_subtitle_name(jf_player_subtitle_current()),
               "Thai - Downloaded") == 0);
  CHECK(ui.focus == picker_row_id(track_row(jf_player_subtitle_current())));
  /* A click or OK on a value never steps it; only left and right do. */
  press(RIGHT);
  CHECK(subtitle_settings);
  nav_focus(&ui, setting_row_id(SETTING_SIZE));
  const int size = sub_style.size_percent;
  press(OK);
  CHECK(sub_style.size_percent == size);
  /* The downloaded file can go again, on a second OK. */
  CHECK(setting_shown(SETTING_DELETE));
  nav_focus(&ui, setting_row_id(SETTING_DELETE));
  press(OK);
  CHECK(delete_armed && jf_player_subtitle_count() == 4);
  capture("subtitle-delete");
  press(OK);
  test_ui_settle();
  CHECK(jf_player_subtitle_count() == 3);
  CHECK(jf_player_subtitle_current() == -1);
  CHECK(subtitle_picker && ui.focus == picker_row_id(track_row(-1)));
  press(BACK);
  CHECK(!subtitle_picker);

  press(BACK);
  CHECK(screen == SCREEN_DETAILS);
  CHECK(ui.focus == nav_str("details.play"));
  press(BACK);
  CHECK(screen == SCREEN_SEASON);
  CHECK(ui.focus == episode);
}

void player_mock_end(void);

/* A subtitle and audio chosen in one episode are the next one's too, whatever
 * the server picks; and an episode that plays out goes on into the next. */
static void test_next_episode(void) {
  open_library("library-shows");
  press(OK);
  press(OK);
  press(OK);
  press(OK);
  CHECK(screen == SCREEN_PLAYBACK && next_episode.present);
  press(KEY_SUBTITLE);
  presses(DOWN, 2);
  CHECK(strcmp(jf_player_subtitle_name(jf_player_subtitle_current()),
               "German - ASS") == 0);
  press(BACK);
  CHECK(strstr(mock_saved_delays(), "\"sub:series-0\":\"ger_\"") != NULL);
  jf_player_audio_select(1);

  char next[sizeof(next_episode.id)];
  set_text(next, sizeof(next), next_episode.id);
  /* Before the credits, where it was is what is reported. */
  jf_player_seek_to(35 * 60000);
  player_mock_end();
  test_ui_settle();
  CHECK(screen == SCREEN_PLAYBACK);
  CHECK(strcmp(playback_item_id, next) == 0);
  CHECK(strcmp(jf_player_subtitle_name(jf_player_subtitle_current()),
               "German - ASS") == 0);
  CHECK(jf_player_audio_current() == 1);
  CHECK(mock_progress_ms() == 35 * 60000);
  /* Leaving during them is the end, which the server counts as watched. */
  jf_player_seek_to(41 * 60000);
  press(BACK);
  test_ui_settle();
  CHECK(mock_progress_ms() == (uint64_t)jf_player_duration());
  CHECK(screen == SCREEN_DETAILS);

  /* Beside Play, a checkbox that marks it played on the server, and back. */
  press(RIGHT);
  CHECK(ui.focus == nav_str("details.played"));
  const bool played = detail.played;
  press(OK);
  CHECK(mock_calls(JF_JOB_MARK_PLAYED) == 1 && detail.played == !played);
  capture("details-played");
  press(OK);
  CHECK(mock_calls(JF_JOB_MARK_PLAYED) == 2 && detail.played == played);

  /* Another device's progress shows on every card of the item at once, and a
   * season's count with it - nothing asked again. */
  const unsigned episodes = mock_calls(JF_JOB_EPISODES);
  jf_user_data elsewhere[2] = {
      {.position_ticks = 600000000,
       .played_percentage = 25,
       .has_played_percentage = true},
      {.unplayed_item_count = 0, .has_unplayed_item_count = true}};
  set_text(elsewhere[0].item_id, sizeof(elsewhere[0].item_id), detail.id);
  set_text(elsewhere[1].item_id, sizeof(elsewhere[1].item_id),
           season_detail.id);
  mock_event(elsewhere, 2);
  test_ui_settle();
  CHECK(!detail.played && detail.progress == 25 &&
        detail.playback_position_ticks == 600000000);
  CHECK(season_detail.played && season_detail.remaining == 0);
  bool listed = false;
  for (size_t i = 0; i < episodes_row.count; i++)
    if (strcmp(episodes_row.cards[i].id, detail.id) == 0)
      listed = episodes_row.cards[i].progress == 25;
  CHECK(listed && mock_calls(JF_JOB_EPISODES) == episodes);
  go_home();
}

static void test_settings(void) {
  go_home();
  nav_focus(&ui, home_card(ROW_RESUME, "movie-0"));
  test_ui_settle();
  press(LEFT);
  CHECK(sidebar_open);
  CHECK(ui.focus == nav_child(nav_str("sidebar"), nav_int(0)));
  presses(DOWN, 2);
  CHECK(ui.focus == nav_child(nav_str("sidebar"), nav_int(2)));
  press(OK);
  CHECK(screen == SCREEN_SETTINGS);
  CHECK(ui.focus == nav_str("settings.animations"));
  presses(DOWN, 2);
  CHECK(ui.focus == nav_str("settings.licenses"));
  press(OK);
  CHECK(screen == SCREEN_LICENSES);
  press(OK);
  CHECK(screen == SCREEN_LICENSE);
  presses(DOWN, 5);
  press(BACK);
  CHECK(screen == SCREEN_LICENSES);
  CHECK(ui.focus == nav_child(nav_str("licenses"), nav_int(0)));
  press(BACK);
  CHECK(screen == SCREEN_SETTINGS);
  CHECK(ui.focus == nav_str("settings.licenses"));
  press(BACK);
  CHECK(screen == SCREEN_HOME);
  CHECK(ui.focus == home_card(ROW_RESUME, "movie-0"));

  /* A library opened from the sidebar comes back to the card the sidebar
   * opened over. */
  press(LEFT);
  press(OK);
  CHECK(screen == SCREEN_GRID);
  press(BACK);
  CHECK(screen == SCREEN_HOME);
  CHECK(ui.focus == home_card(ROW_RESUME, "movie-0"));
}

int main(int argc, char **argv) {
  animated = argc > 1 && strcmp(argv[1], "animated") == 0;
  char store[] = "/tmp/jf-ui-tests-XXXXXX";
  if (mkdtemp(store) == NULL)
    return 1;
  setenv("JELLYFIN_STORE", store, 1);
  setenv("SDL_VIDEODRIVER", "offscreen", 0);
  unsetenv("JELLYFIN_ADDRESS");
  unsetenv("JELLYFIN_USER");
  unsetenv("JELLYFIN_PASSWORD");
  unsetenv("UI_SCRIPT");
  unsetenv("UI_CAPTURE");
  unsetenv("JF_PLAY");
  char *app_argv[] = {argv[0], NULL};
  if (!app_init(1, app_argv))
    return 1;
  animations_enabled = animated;

  test_sign_in();
  test_rows();
  test_expired_token();
  test_grid();
  test_grid_not_found();
  test_series();
  test_slow_details();
  test_playback();
  test_next_episode();
  test_settings();

  app_deinit();
  if (failures == 0)
    printf("ui tests passed\n");
  return failures == 0 ? 0 : 1;
}
