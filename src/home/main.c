/*
 * A Home screen for the TV, in place of LG's: the apps and the inputs, LG's
 * own screens that open over it, and the picture and sound settings changed
 * without leaving it.
 *
 * It is LG's Home only while tools/home-takeover.sh has it mounted over LG's
 * app; a reboot gives LG's Home its place back. Installed as an ordinary app
 * it is just an app, which is how it is developed.
 *
 * Everything it knows of the TV comes through src/tv, whose answers arrive
 * between frames; the screens draw whatever has arrived so far.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdatomic.h>
#include <ucontext.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../jf/image.h"
#include "../platform/gl.h"
#include "../platform/luna.h"
#include "../platform/window.h"
#include "../tv/tv.h"
#include "../ui/kit.h"
#include "../ui/loom.h"
#include "../ui/nav.h"
#include "../ui/renderer.h"
#include "../ui/stack.h"
#include "../ui/theme.h"

#define APP_ID "dev.hookedbehemoth.home"

typedef enum { SCREEN_HOME, SCREEN_SETTINGS, SCREEN_CHOICES } screen_id;

static jf_renderer *renderer;
static nav ui;
static ui_kit kit = {.nav = &ui, .scale = 1};
static bool animations_enabled = true;

static screen_id screen = SCREEN_HOME;
/* The setting SCREEN_CHOICES lists the values of. */
static tv_setting_id choosing;

typedef struct {
  ui_view view;
  tv_setting_id setting;
} stack_entry;
static stack_entry stack_entries[8];
static ui_stack stack = {(unsigned char *)stack_entries, sizeof(stack_entry), 8,
                         0};

/* A status line under everything: what went wrong, until the next key. */
static char status[160];

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

/* ------------------------------------------------------------ navigation */

static void push(void) {
  const stack_entry entry = {{(int)screen, 0}, choosing};
  ui_stack_push(&stack, &ui, &entry);
}

/* Home is the root: Back there stays. */
static void go_back(void) {
  const stack_entry *top = ui_stack_pop(&stack, &ui);
  if (top == NULL)
    return;
  screen = (screen_id)top->view.screen;
  choosing = top->setting;
}

static void open_settings(void) {
  push();
  screen = SCREEN_SETTINGS;
}

static void open_choices(tv_setting_id setting) {
  push();
  screen = SCREEN_CHOICES;
  choosing = setting;
}

/* Pressing Home with the launcher already up: back to its first screen, as
 * it would open. */
static void go_home(void) {
  while (ui_stack_depth(&stack) > 0)
    go_back();
}

/* ----------------------------------------------------------------- icons
 *
 * One texture per app's icon, read from the path its launch point gives. The
 * icons are the TV's own files, so they are decoded on the main thread, a
 * couple a frame, rather than kept anywhere. */

#define ICONS 128
#define ICON_LOADS_PER_FRAME 2

typedef struct {
  char path[384];
  uint32_t texture, width, height;
} icon;

static icon icons[ICONS];
static size_t icon_count;
static unsigned icon_loads;

static uint8_t *read_file(const char *path, size_t *out_size) {
  FILE *file = fopen(path, "rb");
  if (file == NULL)
    return NULL;
  uint8_t *data = NULL;
  if (fseek(file, 0, SEEK_END) == 0) {
    const long size = ftell(file);
    if (size > 0 && size < 8 * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0) {
      data = malloc((size_t)size);
      if (data != NULL && fread(data, 1, (size_t)size, file) != (size_t)size) {
        free(data);
        data = NULL;
      }
      *out_size = (size_t)size;
    }
  }
  fclose(file);
  return data;
}

/* NULL for none, for one that would not load, or for one still to come. */
static const icon *icon_for(const char *path) {
  if (path[0] == '\0')
    return NULL;
  for (size_t i = 0; i < icon_count; i++)
    if (strcmp(icons[i].path, path) == 0)
      return icons[i].texture != 0 ? &icons[i] : NULL;
  if (icon_count == ICONS)
    return NULL;
  if (icon_loads == ICON_LOADS_PER_FRAME) {
    jf_window_frame_requested = true;
    return NULL;
  }
  icon_loads++;
  icon *slot = &icons[icon_count++];
  memset(slot, 0, sizeof(*slot));
  snprintf(slot->path, sizeof(slot->path), "%s", path);
  size_t size = 0;
  uint8_t *data = read_file(path, &size);
  jf_image image;
  if (data != NULL && jf_image_decode_rgba(data, size, &image)) {
    slot->texture = jf_renderer_create_rgba_texture(renderer, image.width,
                                                    image.height, image.rgb);
    slot->width = image.width;
    slot->height = image.height;
    jf_image_free(&image);
  } else {
    fprintf(stderr, "home: no icon at %s\n", path);
  }
  free(data);
  return slot->texture != 0 ? slot : NULL;
}

/* ---------------------------------------------------------------- ribbon
 *
 * One horizontal list along the bottom of the screen over whatever is in front
 * - the apps, then the inputs, then LG's screens and this launcher's settings -
 * as LG's Home drew it before webOS 6. The rest of the frame is transparent:
 * declared as a Home window (see tools/home-takeover.sh), the launcher is laid
 * over the app or input beneath, which keeps playing. */

typedef enum { ENTRY_APP, ENTRY_INPUT, ENTRY_TV } entry_kind;

/* The TV entries: LG's screens this TV has, and this launcher's own settings
 * first. */
#define TV_ENTRY_SETTINGS (-1)

typedef struct {
  entry_kind kind;
  /* Into tv.apps or tv.inputs, or an overlay or TV_ENTRY_SETTINGS. */
  int index;
} entry;

#define ENTRIES (TV_APPS + TV_INPUTS + TV_OVERLAY_COUNT + 1)
static entry entries[ENTRIES];
static size_t entry_count;

static void collect_entries(void) {
  entry_count = 0;
  for (size_t i = 0; i < tv.app_count; i++)
    entries[entry_count++] = (entry){ENTRY_APP, (int)i};
  for (size_t i = 0; i < tv.input_count; i++)
    entries[entry_count++] = (entry){ENTRY_INPUT, (int)i};
  entries[entry_count++] = (entry){ENTRY_TV, TV_ENTRY_SETTINGS};
  for (int i = 0; i < TV_OVERLAY_COUNT; i++)
    if (tv.overlay_app[i][0] != '\0' && i != TV_OVERLAY_SETTINGS)
      entries[entry_count++] = (entry){ENTRY_TV, i};
}

/* What a tile is to nav: what it shows, not where, so the focus stays on an
 * app when one before it is installed. */
static nav_id entry_id(const entry *e) {
  switch (e->kind) {
  case ENTRY_APP:
    return nav_str(tv.apps[e->index].id);
  case ENTRY_INPUT:
    return nav_str(tv.inputs[e->index].id);
  case ENTRY_TV:
    break;
  }
  return nav_int((uint64_t)(e->index + 1));
}

static const char *entry_title(const entry *e) {
  switch (e->kind) {
  case ENTRY_APP:
    return tv.apps[e->index].title;
  case ENTRY_INPUT:
    return tv.inputs[e->index].label;
  case ENTRY_TV:
    break;
  }
  return e->index == TV_ENTRY_SETTINGS ? "Picture & sound"
                                       : tv_overlay_title((tv_overlay)e->index);
}

/* The line under a tile: what is plugged into an input. */
static const char *entry_detail(const entry *e) {
  if (e->kind != ENTRY_INPUT)
    return NULL;
  const tv_input *input = &tv.inputs[e->index];
  return !input->connected          ? "Nothing connected"
         : input->device[0] != '\0' ? input->device
                                    : NULL;
}

static void open_entry(const entry *e) {
  bool opened = true;
  switch (e->kind) {
  case ENTRY_APP:
    opened = tv_launch(tv.apps[e->index].id);
    break;
  case ENTRY_INPUT:
    opened = tv_open_input(&tv.inputs[e->index]);
    break;
  case ENTRY_TV:
    if (e->index == TV_ENTRY_SETTINGS)
      open_settings();
    else
      opened = tv_open_overlay((tv_overlay)e->index);
    break;
  }
  if (!opened)
    snprintf(status, sizeof(status), "%s could not be opened", entry_title(e));
  /* Out of the way of what it opened, which takes the remote. */
  else if (!(e->kind == ENTRY_TV && e->index == TV_ENTRY_SETTINGS))
    jf_window_minimize();
}

/* An icon fitted into the middle of the tile, as LG's Home draws them, or
 * the first letter of what has none. */
static void draw_picture(loom_context *ctx, const entry *e, loom_rect art,
                         loom_rect clip, bool dim) {
  const float s = kit.scale;
  const icon *picture =
      e->kind == ENTRY_APP ? icon_for(tv.apps[e->index].icon) : NULL;
  if (picture != NULL) {
    const float room = minf(art.w, art.h) * 0.62f;
    const float fit =
        room / maxf((float)picture->width, (float)picture->height);
    const float w = picture->width * fit, h = picture->height * fit;
    static const float uv[4] = {0, 0, 1, 1};
    loom_textured(
        ctx,
        (loom_rect){art.x + (art.w - w) / 2, art.y + (art.h - h) / 2, w, h},
        &clip, picture->texture, uv, UI_WHITE, 0);
    return;
  }
  /* A whole UTF-8 sequence, in frame scratch: the label borrows it until the
   * frame is drawn. */
  const char *title = entry_title(e);
  int length = 1;
  while (title[0] != '\0' && title[length] != '\0' &&
         ((unsigned char)title[length] & 0xc0) == 0x80)
    length++;
  const char *initial = e->kind == ENTRY_INPUT ? ui_fmt("%s", "\xe2\x87\xa5")
                                               : ui_fmt("%.*s", length, title);
  const float size = 64 * s;
  const float w = jf_renderer_measure(renderer, initial, size);
  loom_color color = {UI_DIM[0], UI_DIM[1], UI_DIM[2], dim ? 110 : 255};
  loom_label(ctx,
             (loom_rect){art.x + (art.w - w) / 2,
                         art.y + (art.h - size * 1.2f) / 2, w + 2, size * 1.4f},
             &clip, initial, color, size);
}

static void draw_tile(loom_context *ctx, const entry *e, loom_rect rect,
                      loom_rect clip, unsigned flags) {
  const float s = kit.scale;
  const loom_rect art = {rect.x, rect.y, rect.w, rect.w};
  const bool dim = e->kind == ENTRY_INPUT && !tv.inputs[e->index].connected;
  static const loom_color tile = {27, 36, 52, 225}, hot = {38, 51, 70, 235};
  loom_fill(ctx, art, &clip, flags & NAV_HOVERED ? hot : tile, 16 * s);
  draw_picture(ctx, e, art, clip, dim);
  const bool focused = flags & NAV_FOCUSED;
  loom_label(ctx, (loom_rect){rect.x, art.y + art.h + 12 * s, rect.w, 32 * s},
             &clip, ui_ellipsize(&kit, entry_title(e), rect.w, 21 * s),
             focused && !dim ? UI_TEXT : UI_DIM, 21 * s);
  const char *detail = entry_detail(e);
  if (detail != NULL)
    loom_label(ctx, (loom_rect){rect.x, art.y + art.h + 42 * s, rect.w, 28 * s},
               &clip, ui_ellipsize(&kit, detail, rect.w, 17 * s), UI_DIM,
               17 * s);
}

/* Dark towards the bottom of the screen and clear above, so the ribbon reads
 * over a bright picture. */
static void draw_scrim(loom_context *ctx, float top) {
  const loom_mask outer = ctx->mask;
  const float fade = 220 * kit.scale;
  ctx->mask.edge[LOOM_FADE_TOP] = top;
  ctx->mask.width[LOOM_FADE_TOP] = fade;
  static const loom_color scrim = {5, 8, 14, 215};
  loom_fill(ctx, (loom_rect){0, top, ctx->viewport.w, ctx->viewport.h - top},
            NULL, scrim, 0);
  ctx->mask = outer;
}

/* The ribbon, scrolling a whole tile at a time and keeping the focused one's
 * neighbours in view. Every tile is declared, so the focus can come back to
 * one scrolled away. */
static void draw_ribbon(loom_context *ctx) {
  const float s = kit.scale;
  const float width = ctx->viewport.w, height = ctx->viewport.h;
  const float tile_w = 168 * s, caption_h = 76 * s, gap = 24 * s;
  const float margin = 96 * s;
  const loom_rect strip = {margin, height - 96 * s - tile_w - caption_h,
                           width - margin * 2, tile_w + caption_h};
  draw_scrim(ctx, strip.y - 260 * s);
  if (!tv.apps_loaded) {
    ui_spinner(&kit, ctx, width / 2, strip.y + tile_w / 2, 22 * s, NULL);
    return;
  }
  collect_entries();
  const float step = tile_w + gap;
  const float extent = (float)entry_count * step - gap;
  const float scroll = nav_scroll_begin(&ui, nav_str("ribbon"), strip,
                                        LOOM_HORIZONTAL, extent, step, step);
  const loom_mask outer = ctx->mask;
  loom_fade(ctx, LOOM_HORIZONTAL, 0, width, scroll, maxf(0, extent - strip.w),
            86 * s);
  const entry *chosen = NULL;
  for (size_t i = 0; i < entry_count; i++) {
    const loom_rect rect = {strip.x - scroll + (float)i * step, strip.y, tile_w,
                            strip.h};
    if (i == 0)
      nav_default(&ui);
    const unsigned flags =
        nav_item(&ui, entry_id(&entries[i]), rect, ctx->viewport);
    if (flags & NAV_FOCUSED)
      nav_ring(&ui, (loom_rect){rect.x, rect.y, tile_w, tile_w}, 16 * s);
    if (flags & NAV_ACTIVATED)
      chosen = &entries[i];
    if (rect.x + rect.w >= 0 && rect.x < width)
      draw_tile(ctx, &entries[i], rect, ctx->viewport, flags);
  }
  nav_scroll_end(&ui);
  ctx->mask = outer;
  if (chosen != NULL)
    open_entry(chosen);
}

/* -------------------------------------------------------------- settings */

static nav_id setting_row_id(tv_setting_id setting) {
  return nav_child(nav_str("settings"), nav_int((uint64_t)setting));
}

static bool setting_shown(const tv_setting *setting) {
  return setting->known && setting->visible;
}

/* One step through the values it takes, wrapping at either end. */
static void step_setting(tv_setting_id id, int direction) {
  const tv_setting *s = &tv.settings[id];
  if (!s->active) {
    snprintf(status, sizeof(status), "%s cannot be changed right now",
             s->title);
    return;
  }
  if (s->choice_count == 0)
    return;
  size_t at = 0;
  for (size_t i = 0; i < s->choice_count; i++)
    if (strcmp(s->choices[i], s->value) == 0)
      at = i;
  const size_t n = s->choice_count;
  at = direction > 0 ? (at + 1) % n : (at + n - 1) % n;
  tv_setting_set(id, s->choices[at]);
}

static void draw_settings(loom_context *ctx) {
  const float s = kit.scale;
  const loom_rect panel = {ctx->viewport.w - 96 * s - 820 * s, 120 * s, 820 * s,
                           620 * s};
  ui_panel(&kit, ctx, panel);
  loom_label(
      ctx,
      (loom_rect){panel.x + 48 * s, panel.y + 40 * s, panel.w - 96 * s, 44 * s},
      &panel, "Picture & sound", UI_TEXT, 32 * s);
  const float x = panel.x + 48 * s, w = panel.w - 96 * s;
  float y = panel.y + 116 * s;
  bool first = true;
  for (int i = 0; i < TV_SETTING_COUNT; i++) {
    const tv_setting *setting = &tv.settings[i];
    if (!setting_shown(setting))
      continue;
    if (first)
      nav_default(&ui);
    first = false;
    const unsigned flags = ui_value_row(
        &kit, ctx, (loom_rect){x, y, w, 76 * s}, &panel, setting->title,
        tv_value_title(setting->value), setting_row_id((tv_setting_id)i));
    if ((flags & NAV_ACTIVATED) && setting->active)
      open_choices((tv_setting_id)i);
    else if (flags & NAV_ACTIVATED)
      step_setting((tv_setting_id)i, 1);
    y += 96 * s;
  }
  if (!tv.settings[TV_PICTURE_MODE].known)
    ui_spinner(&kit, ctx, panel.x + panel.w / 2, y + 40 * s, 18 * s, &panel);
  y += 24 * s;
  if (ui_button(&kit, ctx, (loom_rect){x, y, w, 72 * s},
                "Switch the screen off", nav_str("settings.screen_off")) &
          NAV_ACTIVATED &&
      !tv_screen_off())
    snprintf(status, sizeof(status), "The screen could not be switched off");
  y += 92 * s;
  if (tv.overlay_app[TV_OVERLAY_SETTINGS][0] != '\0' &&
      ui_button(&kit, ctx, (loom_rect){x, y, w, 72 * s}, "All settings",
                nav_str("settings.all")) &
          NAV_ACTIVATED)
    tv_open_overlay(TV_OVERLAY_SETTINGS);
  loom_label(ctx, (loom_rect){x, panel.y + panel.h - 60 * s, w, 30 * s}, &panel,
             "Left and right change a value", UI_DIM, 20 * s);
}

static void draw_choices(loom_context *ctx) {
  const float s = kit.scale;
  const tv_setting *setting = &tv.settings[choosing];
  const float row_h = 84 * s;
  const float list_h = minf(470 * s, (float)setting->choice_count * row_h);
  const loom_rect panel = {ctx->viewport.w - 96 * s - 680 * s, 120 * s, 680 * s,
                           list_h + 150 * s};
  ui_panel(&kit, ctx, panel);
  loom_label(
      ctx,
      (loom_rect){panel.x + 48 * s, panel.y + 36 * s, panel.w - 96 * s, 44 * s},
      &panel, setting->title, UI_TEXT, 32 * s);
  const loom_rect list = {panel.x + 40 * s, panel.y + 110 * s, panel.w - 80 * s,
                          list_h};
  const float scroll =
      nav_scroll_begin(&ui, nav_str("choices"), list, LOOM_VERTICAL,
                       (float)setting->choice_count * row_h, 0, 0);
  long chosen = -1;
  for (size_t i = 0; i < setting->choice_count; i++) {
    const loom_rect row = {list.x, list.y - scroll + (float)i * row_h + 6 * s,
                           list.w, row_h - 12 * s};
    const bool current = strcmp(setting->choices[i], setting->value) == 0;
    if (current)
      nav_default(&ui);
    const unsigned flags =
        ui_value_row(&kit, ctx, row, &list, tv_value_title(setting->choices[i]),
                     current ? "\xe2\x9c\x93" : "", nav_int(i));
    if (flags & NAV_ACTIVATED)
      chosen = (long)i;
  }
  nav_scroll_end(&ui);
  if (chosen >= 0) {
    tv_setting_set(choosing, setting->choices[chosen]);
    go_back();
  }
}

/* ------------------------------------------------------------------ frame */

static void build_ui(loom_context *ctx) {
  const float width = (float)gl_width, height = (float)gl_height;
  kit.scale = minf(width / 1920.0f, height / 1080.0f);
  ui_frame_reset();
  icon_loads = 0;
  loom_begin(ctx, width, height);
  ui.animate = animations_enabled;
  ui.duration = UI_NAVIGATION_NS;
  nav_begin(&ui, ctx, ui_now_ns());
  /* Nothing over the picture beneath but the ribbon and what opens from it,
   * which leaves it inert behind them. */
  nav_set_inert(&ui, screen != SCREEN_HOME);
  draw_ribbon(ctx);
  nav_set_inert(&ui, false);
  switch (screen) {
  case SCREEN_HOME:
    break;
  case SCREEN_SETTINGS:
    draw_settings(ctx);
    break;
  case SCREEN_CHOICES:
    draw_choices(ctx);
    break;
  }
  const char *line = status[0] != '\0' ? status : tv.error;
  if (line[0] != '\0')
    loom_label(ctx,
               (loom_rect){96 * kit.scale, height - 64 * kit.scale,
                           width - 192 * kit.scale, 34 * kit.scale},
               NULL, line, UI_RED, 22 * kit.scale);
  if (nav_end(&ui, UI_ACCENT, 4 * kit.scale))
    jf_window_frame_requested = true;
}

/* ------------------------------------------------------------------ input */

static bool capture_requested;

static void on_key(uint32_t code, bool pressed, bool repeat) {
  (void)repeat;
  if (!pressed)
    return;
  jf_window_frame_requested = true;
  status[0] = '\0';
  tv.error[0] = '\0';
  if (jf_window_is_back_key(code)) {
    /* On the first screen, out of the way of the app beneath. */
    if (ui_stack_depth(&stack) == 0)
      jf_window_minimize();
    else
      go_back();
    return;
  }
  const nav_dir dirs[] = {NAV_UP, NAV_DOWN, NAV_LEFT, NAV_RIGHT};
  int dir = -1;
  switch (code) {
  case 103:
    dir = 0;
    break;
  case 108:
    dir = 1;
    break;
  case 105:
    dir = 2;
    break;
  case 106:
    dir = 3;
    break;
  case 28:
  case 96:
  case 352:
    nav_activate(&ui);
    return;
  case 88: /* F12 */
    capture_requested = true;
    return;
  default:
    return;
  }
  /* Across a setting's row, its value. */
  if (screen == SCREEN_SETTINGS && (dir == 2 || dir == 3))
    for (int i = 0; i < TV_SETTING_COUNT; i++)
      if (ui.focus == setting_row_id((tv_setting_id)i)) {
        step_setting((tv_setting_id)i, dir == 3 ? 1 : -1);
        return;
      }
  if (!nav_move(&ui, dirs[dir]) && (dir == 0 || dir == 1))
    nav_step(&ui, dir == 1 ? 1 : -1);
}

static void on_event(const jf_event *event) {
  switch (event->kind) {
  case JF_EVENT_KEY:
    on_key(event->key.code, event->key.pressed, event->key.repeat);
    break;
  case JF_EVENT_POINTER_ENTER:
  case JF_EVENT_POINTER_MOTION:
    jf_window_frame_requested = true;
    nav_pointer(&ui, (float)jf_fixed_to_int(event->pointer.x),
                (float)jf_fixed_to_int(event->pointer.y));
    break;
  case JF_EVENT_POINTER_LEAVE:
    nav_pointer_leave(&ui);
    jf_window_frame_requested = true;
    break;
  case JF_EVENT_POINTER_BUTTON:
    if (event->button.pressed && event->button.button == 0x110) {
      nav_click(&ui);
      jf_window_frame_requested = true;
    }
    break;
  case JF_EVENT_POINTER_AXIS:
    if (event->axis.axis == 0) {
      nav_wheel(&ui, (float)event->axis.value / 256.0f * 5.0f);
      jf_window_frame_requested = true;
    }
    break;
  case JF_EVENT_CLOSE:
    fprintf(stderr, "home: the window was closed\n");
    jf_window_running = false;
    break;
  case JF_EVENT_RESIZED:
  case JF_EVENT_TEXT_COMMIT:
    jf_window_frame_requested = true;
    break;
  }
}

/* -------------------------------------------------------------- lifecycle */

/* Set from the GLib thread when SAM opens the launcher again: the Home key,
 * or nothing left in front once an app closes. */
enum { RELAUNCH_NONE, RELAUNCH_OTHER, RELAUNCH_HOME_KEY };
static atomic_int relaunched;

static void relaunch(bool home_key) {
  atomic_store(&relaunched, home_key ? RELAUNCH_HOME_KEY : RELAUNCH_OTHER);
  jf_window_wake();
}

static void on_relaunch(void) { relaunch(jf_luna_relaunch_home_key()); }

/* A crash as Home leaves nothing behind: SAM starts it again and no log on
 * the TV says why. This writes the signal, the address and the mappings the
 * fault and the instruction are in to the log, with nothing a signal handler
 * may not call. */
static void write_text(const char *text) {
  ssize_t ignored = write(2, text, strlen(text));
  (void)ignored;
}

static void write_hex(uintptr_t value) {
  char out[2 + 2 * sizeof(value) + 1];
  out[0] = '0', out[1] = 'x';
  for (size_t i = 0; i < 2 * sizeof(value); i++)
    out[2 + i] =
        "0123456789abcdef"[(value >> (4 * (2 * sizeof(value) - 1 - i))) & 15];
  out[sizeof(out) - 1] = '\0';
  write_text(out);
}

/* The line of /proc/self/maps that holds `address`. */
static void write_mapping(uintptr_t address) {
  const int maps = open("/proc/self/maps", O_RDONLY);
  if (maps < 0)
    return;
  char line[256];
  size_t used = 0;
  char c;
  while (read(maps, &c, 1) == 1) {
    if (c != '\n') {
      if (used < sizeof(line) - 1)
        line[used++] = c;
      continue;
    }
    line[used] = '\0';
    used = 0;
    const uintptr_t from = (uintptr_t)strtoul(line, NULL, 16);
    const char *dash = strchr(line, '-');
    const uintptr_t to =
        dash != NULL ? (uintptr_t)strtoul(dash + 1, NULL, 16) : 0;
    if (address >= from && address < to) {
      write_text("  ");
      write_text(line);
      write_text("\n");
      break;
    }
  }
  close(maps);
}

static void on_fatal(int signal, siginfo_t *info, void *context) {
  write_text("home: fatal signal ");
  write_hex((uintptr_t)signal);
  write_text(" at ");
  write_hex((uintptr_t)info->si_addr);
  write_text("\n");
  write_mapping((uintptr_t)info->si_addr);
#ifdef __arm__
  const ucontext_t *uc = context;
  write_text("  pc ");
  write_hex(uc->uc_mcontext.arm_pc);
  write_text(" lr ");
  write_hex(uc->uc_mcontext.arm_lr);
  write_text("\n");
  write_mapping(uc->uc_mcontext.arm_pc);
  write_mapping(uc->uc_mcontext.arm_lr);
#else
  (void)context;
#endif
  /* As it would have ended. */
  struct sigaction fallback = {.sa_handler = SIG_DFL};
  sigaction(signal, &fallback, NULL);
  raise(signal);
}

/* And an exit no code here asked for: LG's SDL ends the process itself
 * on some of the compositor's requests. */
static bool exiting;
static void on_exit_unasked(void) {
  if (!exiting)
    write_text("home: exit() from somewhere else\n");
}

static void report_crashes(void) {
  atexit(on_exit_unasked);
  struct sigaction action = {.sa_sigaction = on_fatal, .sa_flags = SA_SIGINFO};
  sigemptyset(&action.sa_mask);
  const int signals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTERM};
  for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++)
    sigaction(signals[i], &action, NULL);
}

static loom_context ctx;
static ui_script script;
static const char *capture_path;
static bool was_drawable = true;

static bool app_init(int argc, char **argv) {
  /* Launched by SAM there is no terminal; the tests report on stderr. */
  report_crashes();
#ifndef HOME_APP_TEST
  ui_log_to_file("/tmp/home.log", NULL);
  /* Every call and answer: which ones the bus refuses this app is the first
   * thing to read there (docs/home.md). */
  jf_luna_log = true;
  jf_window_log_events = true;
#endif
  /* Who this is to the bus and the compositor. SAM starts a system app, which
   * the takeover makes this, without APPID in the environment, but names it
   * in the launch payload; with neither, it is the development install. The
   * name decides everything: the bus gives com.webos.app.home LG's Home's
   * role, and sam waits for a window of that app before taking its spinner
   * down. */
  static char launched_as[128];
  const char *appid = getenv("APPID");
  if (appid == NULL && argc > 1 &&
      jf_luna_json_string(argv[1], "appId", launched_as, sizeof(launched_as))) {
    appid = launched_as;
    jf_luna_set_name(appid);
  }
  fprintf(stderr, "home: running as %s\n", appid != NULL ? appid : APP_ID);
  jf_window_set_handler(on_event);
  /* Over the app or input in front, as LG's Settings and Search are. */
  jf_window_set_type("_WEBOS_WINDOW_TYPE_OVERLAY");
  if (!jf_window_init(appid != NULL ? appid : APP_ID, "Home", 0, 0))
    return false;
  if (!jf_luna_register_lifecycle(jf_window_post_quit, on_relaunch))
    fprintf(stderr, "no webOS lifecycle\n");
  renderer = jf_renderer_create(NULL, 0, 0);
  if (renderer == NULL) {
    fprintf(stderr, "the UI renderer could not start\n");
    return false;
  }
  kit.renderer = renderer;
  loom_init(&ctx);
  tv_init(jf_window_wake);
  tv_refresh();
  capture_path = getenv("UI_CAPTURE");
  script.keys = getenv("UI_SCRIPT");
  if (script.keys == NULL)
    script.keys = "";
  return true;
}

static bool app_step(int *wait_ms) {
  /* The Home key toggles: the launcher out of the way when it has the
   * remote, and otherwise in front on its first screen - whether it was
   * hidden here or something came up over it. */
  const int relaunch = atomic_exchange(&relaunched, RELAUNCH_NONE);
  if (relaunch == RELAUNCH_HOME_KEY && jf_window_drawable &&
      jf_window_focused()) {
    jf_window_minimize();
  } else if (relaunch != RELAUNCH_NONE) {
    go_home();
    tv_refresh();
    jf_window_post_raise();
    jf_window_frame_requested = true;
  }
  /* Back in front from an app: what is installed and plugged in may have
   * changed while it was. */
  if (jf_window_drawable && !was_drawable) {
    tv_refresh();
    jf_window_frame_requested = true;
  }
  was_drawable = jf_window_drawable;
  if (tv_pump())
    jf_window_frame_requested = true;
  const bool scripted = script.keys[0] != '\0' || capture_path != NULL;
  if (!jf_window_drawable ||
      (!jf_window_frame_requested && !scripted && !capture_requested)) {
    *wait_ms = -1;
    return false;
  }
  jf_window_frame_requested = false;
  glViewport(0, 0, (GLsizei)gl_width, (GLsizei)gl_height);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  build_ui(&ctx);
  jf_renderer_draw(renderer, ctx.commands, ctx.count, (float)gl_width,
                   (float)gl_height);
  if (scripted) {
    const int command = ui_script_next(&script, tv_pending() != 0);
    const uint32_t key = command > 0 ? ui_script_key((char)command) : 0;
    if (key != 0)
      on_key(key, true, false);
    /* The screen the script ends on, once nothing on it is still arriving. */
    if (command < 0 && capture_path != NULL && !jf_window_frame_requested)
      capture_requested = true;
  }
  if (capture_requested) {
    ui_capture(capture_path != NULL ? capture_path : "home-capture.ppm");
    capture_requested = false;
    if (capture_path != NULL)
      jf_window_running = false;
  }
  jf_window_swap();
  return true;
}

static void app_deinit(void) {
  exiting = true;
  tv_deinit();
  loom_destroy(&ctx);
  jf_renderer_destroy(renderer);
  jf_luna_deinit();
  jf_window_deinit();
}

#ifndef HOME_APP_TEST
int main(int argc, char **argv) {
  if (!app_init(argc, argv))
    return 1;
  while (jf_window_poll()) {
    int wait_ms;
    if (!app_step(&wait_ms))
      jf_window_wait_timeout(wait_ms);
  }
  app_deinit();
  return 0;
}
#endif
