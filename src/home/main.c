/*
 * A Home screen for the TV, in place of LG's: a bar of apps along the bottom
 * of the screen, as LG's Home drew it before webOS 6, over the app or input
 * in front, with the inputs under it.
 *
 * It is LG's Home only while tools/home-takeover.sh has it mounted over LG's
 * app; a reboot gives LG's Home its place back, unless it was asked to take
 * the place at every start. Opened from LG's menu under its own id, it runs
 * that script to take the place, and the bar opens as LG's Home.
 *
 * Everything it knows of the TV comes through src/tv, whose answers arrive
 * between frames; the screens draw whatever has arrived so far. The order of
 * the apps and which are hidden are its own (layout.h).
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/stat.h>
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
#include "layout.h"

#define APP_ID "dev.hookedbehemoth.home"
#define LG_HOME_ID "com.webos.app.home"
/* Where the TV installed it, with the scripts it carries. */
#define INSTALLED_DIR "/media/developer/apps/usr/palm/applications/" APP_ID

/* SCREEN_EDIT reorders and hides the apps; SCREEN_CONFIRM asks whether one is
 * hidden or uninstalled; SCREEN_OPTIONS is the launcher's own: whether it
 * takes LG's Home's place at every start, and giving it back. */
typedef enum {
  SCREEN_HOME,
  SCREEN_EDIT,
  SCREEN_CONFIRM,
  SCREEN_OPTIONS
} screen_id;

static jf_renderer *renderer;
static nav ui;
static ui_kit kit = {.nav = &ui, .scale = 1};
static bool animations_enabled = true;
/* Drawn without easing: from the launcher coming up until the first key or
 * pointer move, through the focus going to what is in front. Opening, it is
 * simply there. */
static bool instant = true;
static bool animating(void) { return animations_enabled && !instant; }
/* Over nothing: the app in front closed, and the input the compositor brings
 * up for it is not to be seen. Black behind the menu until it goes. */
static bool covering;

static screen_id screen = SCREEN_HOME;

static ui_view stack_entries[8];
static ui_stack stack = {(unsigned char *)stack_entries, sizeof(ui_view), 8, 0};

/* A status line over the bar: what went wrong, until the next key. */
static char status[160];
/* Restarting the TV, until it goes; the line says so. */
static bool restarting;

static float minf(float a, float b) { return a < b ? a : b; }
static float maxf(float a, float b) { return a > b ? a : b; }

/* ------------------------------------------------------------ navigation */

static void push(screen_id to) {
  const ui_view view = {(int)screen, 0};
  ui_stack_push(&stack, &ui, &view);
  screen = to;
}

/* Home is the root: Back there stays. */
static void go_back(void) {
  const ui_view *top = ui_stack_pop(&stack, &ui);
  if (top != NULL)
    screen = (screen_id)top->screen;
}

static void save_layout(void);
/* In SCREEN_EDIT: the focused app goes along with Left and Right. */
static bool moving;

/* Pressing Home with the launcher already up: back to its first screen, as
 * it would open, keeping what was being edited. */
static void go_home(void) {
  if (ui_stack_depth(&stack) > 0) {
    moving = false;
    save_layout();
  }
  while (ui_stack_depth(&stack) > 0)
    go_back();
}

/* ----------------------------------------------------------------- layout */

static home_layout layout;
static char layout_path[512];
/* A PNG shown behind the bar when nothing else is: "" for black. */
static char background_path[512];

/* The app SCREEN_CONFIRM asks about. */
static char confirming[128];
/* The app with the focus, as last drawn, for the keys that move it. */
static char focused_app[128];

static const tv_app *app_by_id(const char *id) {
  for (size_t i = 0; i < tv.app_count; i++)
    if (strcmp(tv.apps[i].id, id) == 0)
      return &tv.apps[i];
  return NULL;
}

static void save_layout(void) {
  if (!layout_save(&layout, layout_path))
    snprintf(status, sizeof(status),
             "The order of the apps could not be saved");
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

/* An image fitted into `area`, `fill` of its shorter side, centred. */
static void draw_fitted(loom_context *ctx, const icon *picture, loom_rect area,
                        loom_rect clip, float fill, uint8_t alpha) {
  const float room = minf(area.w, area.h) * fill;
  const float fit = room / maxf((float)picture->width, (float)picture->height);
  const float w = picture->width * fit, h = picture->height * fit;
  static const float uv[4] = {0, 0, 1, 1};
  const loom_color tint = {255, 255, 255, alpha};
  loom_textured(
      ctx,
      (loom_rect){area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h},
      &clip, picture->texture, uv, tint, 0);
}

/* An image filling `area`, cropped evenly on the sides that overflow. */
static void draw_cover(loom_context *ctx, const icon *picture, loom_rect area) {
  const float fit =
      maxf(area.w / (float)picture->width, area.h / (float)picture->height);
  const float u = area.w / (picture->width * fit),
              v = area.h / (picture->height * fit);
  const float uv[4] = {(1 - u) / 2, (1 - v) / 2, (1 + u) / 2, (1 + v) / 2};
  loom_textured(ctx, area, &area, picture->texture, uv, UI_WHITE, 0);
}

/* ------------------------------------------------------------------- bar
 *
 * The apps along the bottom edge, each a tile in the colour the app asks for
 * with its icon on it. The focused tile rises out of the bar and sinks back
 * when the focus moves on. Below the
 * apps, out of sight until Down, the inputs.
 *
 * The rest of the frame is transparent: declared as an overlay, the launcher
 * is laid over the app or input beneath, which keeps playing. */

/* At 1080 rows: 7.5% of the screen's width and 18.5% of its height. */
#define TILE_W 144.0f
#define TILE_H 200.0f
/* The tiles are parallelograms, leaning right by this much per pixel of
 * height, side by side with no gap. */
#define TILE_SLANT 0.17f
#define TILE_LIFT 52.0f
/* Between the shown apps and the hidden ones, in SCREEN_EDIT. */
#define HIDDEN_GAP 64.0f
#define INPUT_W 352.0f
#define INPUT_H 198.0f
/* How far the bar rises to bring the inputs into view. */
#define INPUTS_RISE 340.0f

/* Hidden for something just opened, whose answer has not come back. */
static bool hidden_for_launch;

/* How far each tile has risen, by its id: the one gaining the focus eases up
 * as the one losing it eases down. */
typedef struct {
  nav_id id;
  ui_anim lift;
  uint64_t frame;
} tile_motion;

static tile_motion motions[TV_APPS + 2];
static uint64_t frame_number;
static ui_anim rise;

static float lift_of(nav_id id, bool up) {
  tile_motion *motion = NULL, *oldest = &motions[0];
  for (size_t i = 0; i < sizeof(motions) / sizeof(motions[0]); i++) {
    if (motions[i].id == id) {
      motion = &motions[i];
      break;
    }
    if (motions[i].frame < oldest->frame)
      oldest = &motions[i];
  }
  if (motion == NULL) {
    motion = oldest;
    motion->id = id;
    ui_anim_snap(&motion->lift, 0);
  }
  motion->frame = frame_number;
  ui_anim_set(&motion->lift, up ? 1 : 0, UI_NAVIGATION_NS, animating());
  ui_anim_update(&motion->lift, ui_now_ns());
  if (ui_anim_running(&motion->lift))
    jf_window_frame_requested = true;
  return motion->lift.value;
}

/* Black or white, whichever contrasts more with `background`: WCAG's
 * relative luminance, against which black's contrast is (L + 0.05) / 0.05
 * and white's 1.05 / (L + 0.05). They are equal at L = 0.179. */
static float linear(uint8_t channel) {
  const float c = channel / 255.0f;
  return c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
}

static void ink_on(const loom_color background, loom_color out) {
  const float luminance = 0.2126f * linear(background[0]) +
                          0.7152f * linear(background[1]) +
                          0.0722f * linear(background[2]);
  const uint8_t level = luminance > 0.179f ? 0 : 255;
  out[0] = out[1] = out[2] = level;
  out[3] = 255;
}

/* A first letter for an app without an icon: a whole UTF-8 sequence, in frame
 * scratch, as the label borrows it until the frame is drawn. */
static const char *initial_of(const char *title) {
  int length = 1;
  while (title[0] != '\0' && title[length] != '\0' &&
         ((unsigned char)title[length] & 0xc0) == 0x80)
    length++;
  return ui_fmt("%.*s", length, title);
}

static void centred_label(loom_context *ctx, loom_rect area, loom_rect clip,
                          const char *text, const loom_color color,
                          float size) {
  const float w = jf_renderer_measure(renderer, text, size);
  loom_label(ctx,
             (loom_rect){area.x + (area.w - w) / 2,
                         area.y + (area.h - size * 1.2f) / 2, w + 2,
                         size * 1.4f},
             &clip, text, color, size);
}

/* `base` is where the tile sits in the bar; `lift`, from 0 to 1, how far it
 * has risen out of it. A tile without a picture shows `glyph`, or the first
 * letter of its title. */
/* Where a tile's picture is once it has risen `lift` (0 to 1) of the way out
 * of its place in the bar: up along its slanted sides, so it slides past its
 * neighbours' edges rather than away from them. */
static loom_rect risen_tile(loom_rect base, float lift) {
  const float risen = lift * TILE_LIFT * kit.scale;
  return (loom_rect){base.x + risen * TILE_SLANT, base.y - risen, base.w,
                     base.h};
}

/* The middle of a tile's slanted shape, at `from_top` (0 to 1) of its
 * height: the parallelogram leans right as it goes up. */
static float tile_middle(loom_rect tile, float from_top) {
  return tile.x + tile.w / 2 + tile.h * (1 - from_top) * TILE_SLANT;
}

/* `base` is the tile's place in the bar; `lift`, from 0 to 1, how far it has
 * risen out of it. Rising, it grows up along its slanted sides, its colour
 * still reaching down to the bar's foot. A tile without a picture shows
 * `glyph`, or the first letter of its title. */
static void draw_tile(loom_context *ctx, loom_rect base, loom_rect clip,
                      float lift, const loom_color background,
                      const icon *picture, const char *glyph, const char *title,
                      bool dim) {
  const float s = kit.scale;
  const float room = lift * TILE_LIFT * s;
  /* A pixel over the next tile's edge: neighbours' slanted edges, each worked
   * out on its own, would otherwise leave a hairline between them. */
  const loom_rect whole = {base.x, base.y - room, base.w + 1, base.h + room};
  loom_parallelogram(ctx, whole, &clip, background, TILE_SLANT);
  /* Upright, in the middle of the slanted shape's top. */
  const loom_rect top = risen_tile(base, lift);
  const loom_rect art = {tile_middle(top, 0.5f) - top.w / 2, top.y, top.w,
                         top.h};
  loom_color ink;
  ink_on(background, ink);
  if (picture != NULL)
    draw_fitted(ctx, picture, art, clip, 0.6f, 255);
  else
    centred_label(ctx, art, clip, glyph != NULL ? glyph : initial_of(title),
                  ink, 64 * s);
  if (dim) {
    static const loom_color shade = {0, 0, 0, 170};
    loom_parallelogram(ctx, whole, &clip, shade, TILE_SLANT);
  }
}

static void app_colour(const tv_app *app, loom_color out) {
  if (app->has_color) {
    out[0] = (uint8_t)(app->color >> 16);
    out[1] = (uint8_t)(app->color >> 8);
    out[2] = (uint8_t)app->color;
  } else {
    out[0] = 62, out[1] = 66, out[2] = 76;
  }
  out[3] = 255;
}

static nav_id app_id_of(const char *id) { return nav_str(id); }
static nav_id remove_id_of(const char *id) {
  return nav_child(nav_str(id), nav_str("remove"));
}
#define EDIT_TILE_ID nav_str("edit")
#define OPTIONS_TILE_ID nav_str("options")

static nav_id in_ribbon(nav_id local) {
  return nav_child(nav_str("ribbon"), local);
}
static nav_id in_inputs(nav_id local) {
  return nav_child(nav_str("inputs"), local);
}

/* Where Up and Down between the rows land: where the focus last was. */
static nav_id last_app_focus, last_input_focus;

static bool inputs_focused(void) {
  for (size_t i = 0; i < tv.input_count; i++)
    if (ui.focus == in_inputs(nav_str(tv.inputs[i].id)))
      return true;
  return false;
}

static void open_app(const tv_app *app) {
  if (!tv_launch(app->id)) {
    snprintf(status, sizeof(status), "%s could not be opened", app->title);
    return;
  }
  /* Out of the way of what it opened, which takes the remote - unless the TV
   * answers that it will not open it. */
  jf_window_minimize();
  hidden_for_launch = true;
}

static void open_input(const tv_input *input) {
  if (!tv_open_input(input)) {
    snprintf(status, sizeof(status), "%s could not be opened", input->label);
    return;
  }
  jf_window_minimize();
  hidden_for_launch = true;
}

/* The X over an app in SCREEN_EDIT, for hiding or removing it. */
static bool draw_remove_badge(loom_context *ctx, loom_rect tile, loom_rect clip,
                              const char *id) {
  const float s = kit.scale;
  const float d = 56 * s;
  const loom_rect badge = {tile_middle(tile, 0) - d / 2, tile.y - d - 18 * s, d,
                           d};
  const unsigned flags = nav_item(&ui, remove_id_of(id), badge, clip);
  if (flags & NAV_FOCUSED)
    nav_ring(&ui, badge, d / 2);
  loom_fill(ctx, badge, &clip, flags & NAV_FOCUSED ? UI_WHITE : UI_PANEL,
            d / 2);
  centred_label(ctx, badge, clip, "\xe2\x9c\x95",
                flags & NAV_FOCUSED ? UI_BG : UI_TEXT, 26 * s);
  return flags & NAV_ACTIVATED;
}

/* The arrows either side of an app being moved. */
static void draw_move_arrows(loom_context *ctx, loom_rect tile,
                             loom_rect clip) {
  const float s = kit.scale;
  const float d = 44 * s;
  const float y = tile.y + (tile.h - d) / 2;
  /* On its slanted sides, halfway up. */
  const float middle = tile_middle(tile, 0.5f);
  const loom_rect left = {middle - tile.w / 2 - d * 0.75f, y, d, d};
  const loom_rect right = {middle + tile.w / 2 - d * 0.25f, y, d, d};
  loom_fill(ctx, left, &clip, UI_WHITE, d / 2);
  loom_fill(ctx, right, &clip, UI_WHITE, d / 2);
  centred_label(ctx, left, clip, "\xe2\x97\x80", UI_BG, 22 * s);
  centred_label(ctx, right, clip, "\xe2\x96\xb6", UI_BG, 22 * s);
}

/* Resting the pointer over either side of the bar scrolls it that way, at a
 * steady speed that grows the nearer the pointer is to the screen's edge. */
#define EDGE_ZONE 120.0f
/* Pixels a second at the very edge, at 1080 rows. */
#define EDGE_SPEED 1400.0f
static uint64_t edge_scrolled_at;

static void edge_scroll(loom_rect strip, float width, float scroll,
                        float limit) {
  const float zone = EDGE_ZONE * kit.scale;
  /* How far into either zone, from 0 at its inner side to 1 at the edge;
   * negative for the left. */
  float depth = 0;
  if (ui.pointer_present && ui.pointer_y >= strip.y &&
      ui.pointer_y < strip.y + strip.h) {
    if (ui.pointer_x < zone)
      depth = -(zone - ui.pointer_x) / zone;
    else if (ui.pointer_x > width - zone)
      depth = (ui.pointer_x - (width - zone)) / zone;
  }
  /* Already at that end. */
  if ((depth < 0 && scroll <= 0) || (depth > 0 && scroll >= limit))
    depth = 0;
  const uint64_t now = ui_now_ns();
  if (depth == 0) {
    edge_scrolled_at = 0;
    return;
  }
  /* The first frame only starts the clock; a long gap between frames is
   * taken as one frame, not a jump. */
  const float seconds =
      edge_scrolled_at == 0
          ? 0
          : minf((float)(now - edge_scrolled_at) / 1e9f, 0.05f);
  edge_scrolled_at = now;
  const float to =
      scroll + maxf(-1, minf(depth, 1)) * EDGE_SPEED * kit.scale * seconds;
  nav_scroll_by(&ui, nav_str("ribbon"), maxf(0, minf(to, limit)) - scroll);
  jf_window_frame_requested = true;
}

/* The apps, in the layout's order, then the pencil for SCREEN_EDIT - or in
 * it, the hidden apps greyed out after the shown ones. `bottom` is where the
 * tiles' feet are. */
static void draw_apps(loom_context *ctx, float bottom) {
  const float s = kit.scale;
  const float width = ctx->viewport.w;
  /* The question is about what SCREEN_EDIT shows, which stays beneath. */
  const bool editing = screen == SCREEN_EDIT || screen == SCREEN_CONFIRM;
  const float tile_w = TILE_W * s, tile_h = TILE_H * s;
  const float step = TILE_W * s;
  const size_t tiles = editing ? layout.count : layout.shown + 2;
  const float extent =
      (float)tiles * step + TILE_H * TILE_SLANT * s +
      (editing && layout.count > layout.shown ? HIDDEN_GAP * s : 0);
  /* Tall enough for a risen tile and the X over it. */
  const float above = (TILE_LIFT + 100) * s;
  /* From the screen's side: the first tile's bottom left corner is on it. */
  const loom_rect strip = {0, bottom - tile_h - above, width - 48 * s,
                           tile_h + above};
  const float scroll = nav_scroll_begin(&ui, nav_str("ribbon"), strip,
                                        LOOM_HORIZONTAL, extent, step, step);
  const loom_rect clip = ctx->viewport;
  const char *chosen = NULL, *to_remove = NULL;
  bool chose_edit = false, chose_options = false;
  /* Over the neighbours, so drawn after them. */
  bool arrows = false;
  loom_rect carried = {0};
  focused_app[0] = '\0';
  float x = strip.x - scroll;
  for (size_t i = 0; i < tiles; i++) {
    if (editing && i == layout.shown)
      x += HIDDEN_GAP * s;
    const loom_rect base = {x, bottom - tile_h, tile_w, tile_h};
    x += step;
    if (i == 0)
      nav_default(&ui);
    /* The pencil, after the apps. */
    if (!editing && i == layout.shown) {
      const unsigned flags = nav_item(&ui, EDIT_TILE_ID, base, clip);
      if (flags & NAV_FOCUSED)
        last_app_focus = in_ribbon(EDIT_TILE_ID);
      chose_edit = flags & NAV_ACTIVATED;
      static const loom_color pencil = {44, 46, 52, 255};
      draw_tile(ctx, base, clip,
                lift_of(in_ribbon(EDIT_TILE_ID), flags & NAV_FOCUSED), pencil,
                NULL, "\xe2\x9c\x8e", "Edit", false);
      continue;
    }
    /* And the launcher's own options, last. */
    if (!editing && i == layout.shown + 1) {
      const unsigned flags = nav_item(&ui, OPTIONS_TILE_ID, base, clip);
      if (flags & NAV_FOCUSED)
        last_app_focus = in_ribbon(OPTIONS_TILE_ID);
      chose_options = flags & NAV_ACTIVATED;
      static const loom_color gear = {34, 36, 42, 255};
      draw_tile(ctx, base, clip,
                lift_of(in_ribbon(OPTIONS_TILE_ID), flags & NAV_FOCUSED), gear,
                NULL, "\xe2\x9a\x99", "Options", false);
      continue;
    }
    const layout_slot *slot = &layout.slots[i];
    const tv_app *app = app_by_id(slot->id);
    if (app == NULL)
      continue;
    const unsigned flags = nav_item(&ui, app_id_of(app->id), base, clip);
    const bool focused = flags & NAV_FOCUSED;
    const bool badge_focused =
        ui.focus == in_ribbon(remove_id_of(app->id)) && editing;
    if (focused) {
      snprintf(focused_app, sizeof(focused_app), "%s", app->id);
      last_app_focus = in_ribbon(app_id_of(app->id));
    }
    if (flags & NAV_ACTIVATED)
      chosen = app->id;
    if (base.x + base.w < 0 || base.x > width) {
      lift_of(in_ribbon(app_id_of(app->id)), focused || badge_focused);
      continue;
    }
    loom_color background;
    app_colour(app, background);
    const float lift =
        lift_of(in_ribbon(app_id_of(app->id)), focused || badge_focused);
    draw_tile(ctx, base, clip, lift, background, icon_for(app->icon), NULL,
              app->title, slot->hidden);
    const loom_rect risen = risen_tile(base, lift);
    if (editing && !slot->hidden && !moving && (focused || badge_focused) &&
        draw_remove_badge(ctx, risen, clip, app->id))
      to_remove = app->id;
    if (editing && moving && focused) {
      arrows = true;
      carried = risen;
    }
  }
  if (arrows)
    draw_move_arrows(ctx, carried, clip);
  if (editing && layout.count > layout.shown) {
    const float hidden_x =
        strip.x - scroll + (float)layout.shown * step + HIDDEN_GAP * s;
    loom_label(ctx,
               (loom_rect){hidden_x, bottom - tile_h - TILE_LIFT * s - 48 * s,
                           400 * s, 36 * s},
               &clip, "Hidden", UI_DIM, 24 * s);
  }
  nav_scroll_end(&ui);
  edge_scroll(strip, width, scroll, maxf(0, extent - strip.w));

  if (chose_options) {
    push(SCREEN_OPTIONS);
  } else if (chose_edit) {
    push(SCREEN_EDIT);
    moving = false;
    /* The pencil is not there to edit; the app beside it is. */
    if (layout.shown > 0)
      nav_focus(&ui, in_ribbon(app_id_of(layout.slots[layout.shown - 1].id)));
  } else if (chosen != NULL && !editing) {
    open_app(app_by_id(chosen));
  } else if (chosen != NULL) {
    /* Picked up, or put down where it is; a hidden one is shown again, at
     * the end of the shown ones, and picked up to be put in its place. */
    const int at = layout_find(&layout, chosen);
    if (at >= 0 && layout.slots[at].hidden) {
      layout_show(&layout, chosen);
      moving = true;
    } else {
      moving = !moving;
    }
  } else if (to_remove != NULL) {
    snprintf(confirming, sizeof(confirming), "%s", to_remove);
    push(SCREEN_CONFIRM);
  }
}

/* The inputs, a card each with the last frame the TV kept of it. */
static void draw_inputs(loom_context *ctx, float top) {
  const float s = kit.scale;
  const float width = ctx->viewport.w;
  /* Declared below the screen too: Down focuses one before the bar has risen
   * to show it, and an undeclared focus would fall back to the first app. */
  loom_label(ctx, (loom_rect){96 * s, top, 600 * s, 40 * s}, NULL, "Inputs",
             UI_TEXT, 28 * s);
  const float card_w = INPUT_W * s, card_h = INPUT_H * s;
  const float step = card_w + 32 * s;
  const loom_rect strip = {96 * s, top + 60 * s, width - 192 * s,
                           card_h + 90 * s};
  const float extent = (float)tv.input_count * step - 32 * s;
  const float scroll = nav_scroll_begin(&ui, nav_str("inputs"), strip,
                                        LOOM_HORIZONTAL, extent, step, step);
  const loom_rect clip = ctx->viewport;
  const tv_input *chosen = NULL;
  for (size_t i = 0; i < tv.input_count; i++) {
    const tv_input *input = &tv.inputs[i];
    const loom_rect card = {strip.x - scroll + (float)i * step, strip.y, card_w,
                            card_h};
    const unsigned flags = nav_item(&ui, nav_str(input->id), card, clip);
    if (flags & NAV_FOCUSED) {
      nav_ring(&ui, card, 8 * s);
      last_input_focus = in_inputs(nav_str(input->id));
    }
    if (flags & NAV_ACTIVATED)
      chosen = input;
    loom_fill(ctx, card, &clip, flags & NAV_HOVERED ? UI_HOT : UI_CARD, 8 * s);
    const icon *preview = icon_for(input->preview);
    if (preview != NULL) {
      static const float uv[4] = {0, 0, 1, 1};
      loom_textured(ctx, card, &clip, preview->texture, uv, UI_WHITE, 8 * s);
    } else {
      const loom_color glyph = {UI_DIM[0], UI_DIM[1], UI_DIM[2],
                                input->connected ? 255 : 110};
      centred_label(ctx, card, clip, "\xe2\x87\xa5", glyph, 64 * s);
    }
    loom_label(ctx,
               (loom_rect){card.x, card.y + card_h + 12 * s, card_w, 32 * s},
               &clip, ui_ellipsize(&kit, input->label, card_w, 22 * s),
               flags & NAV_FOCUSED ? UI_TEXT : UI_DIM, 22 * s);
    const char *detail = !input->connected          ? "Nothing connected"
                         : input->device[0] != '\0' ? input->device
                                                    : NULL;
    if (detail != NULL)
      loom_label(
          ctx, (loom_rect){card.x, card.y + card_h + 46 * s, card_w, 28 * s},
          &clip, ui_ellipsize(&kit, detail, card_w, 18 * s), UI_DIM, 18 * s);
  }
  nav_scroll_end(&ui);
  if (chosen != NULL)
    open_input(chosen);
}

static void draw_bar(loom_context *ctx) {
  const float s = kit.scale;
  const float height = ctx->viewport.h;
  /* Up by the inputs' height while one of them has the focus. */
  ui_anim_set(&rise, screen == SCREEN_HOME && inputs_focused() ? 1 : 0,
              UI_SLIDE_NS, animating());
  ui_anim_update(&rise, ui_now_ns());
  if (ui_anim_running(&rise))
    jf_window_frame_requested = true;
  const float bottom = height - rise.value * INPUTS_RISE * s;
  /* The top of a risen tile, which the lines of text keep above. */
  const float risen_top = bottom - (TILE_H + TILE_LIFT + 20) * s;
  if (!tv.apps_loaded) {
    ui_spinner(&kit, ctx, ctx->viewport.w / 2, bottom - TILE_H * s / 2, 22 * s,
               NULL);
    return;
  }
  /* Behind the inputs, solid up to the top of the apps: beside them, or past
   * the last of a few, it shows. Shown as far as the inputs are. */
  if (screen == SCREEN_HOME && rise.value > 0.01f) {
    const float apps_top = bottom - TILE_H * s;
    const loom_color shade = {10, 11, 14, (uint8_t)(255 * rise.value)};
    loom_fill(ctx, (loom_rect){0, apps_top, ctx->viewport.w, height - apps_top},
              NULL, shade, 0);
  }
  /* Over the bar, and while editing over the X too. */
  const loom_rect line_rect = {
      96 * s,
      risen_top -
          (screen == SCREEN_EDIT || screen == SCREEN_CONFIRM ? 140 : 56) * s,
      ctx->viewport.w - 192 * s, 34 * s};
  const char *line = status[0] != '\0' ? status : tv.error;
  if (line[0] != '\0')
    loom_label(ctx, line_rect, NULL, line, UI_RED, 22 * s);
  else if (restarting)
    loom_label(ctx, line_rect, NULL, "Restarting the TV...", UI_TEXT, 22 * s);
  else if (screen == SCREEN_EDIT)
    loom_label(ctx, line_rect, NULL,
               moving ? "Left and right move it; select puts it down"
                      : "Select an app to move it, or the X over it to hide "
                        "or remove it. Back saves.",
               UI_TEXT, 22 * s);
  draw_apps(ctx, bottom);
  if (screen == SCREEN_HOME)
    draw_inputs(ctx, bottom + 40 * s);
}

/* ---------------------------------------------------------------- confirm */

/* Where the focus goes once an app is hidden or removed: the app that takes
 * its place, or the one before when it was last. Zero for the screen's
 * default. */
static nav_id neighbour_of(const char *id) {
  const int at = layout_find(&layout, id);
  if (at < 0 || (size_t)at >= layout.shown || layout.shown < 2)
    return 0;
  const size_t next =
      (size_t)at + 1 < layout.shown ? (size_t)at + 1 : (size_t)at - 1;
  return in_ribbon(app_id_of(layout.slots[next].id));
}

static void draw_confirm(loom_context *ctx) {
  const float s = kit.scale;
  const tv_app *app = app_by_id(confirming);
  if (app == NULL) {
    go_back();
    return;
  }
  const float w = 720 * s, h = 300 * s;
  const loom_rect panel = {(ctx->viewport.w - w) / 2, 140 * s, w, h};
  ui_panel(&kit, ctx, panel);
  const float x = panel.x + 48 * s, inner = w - 96 * s;
  loom_label(ctx, (loom_rect){x, panel.y + 40 * s, inner, 44 * s}, &panel,
             ui_ellipsize(&kit, app->title, inner, 32 * s), UI_TEXT, 32 * s);
  ui_wrapped(&kit, ctx, (loom_rect){x, panel.y + 96 * s, inner, 70 * s},
             app->removable
                 ? "Hide it from the bar, or uninstall it from the TV."
                 : "It came with the TV and cannot be uninstalled, but it can "
                   "be hidden from the bar.",
             21 * s, UI_DIM);
  const float button_w = (inner - 24 * s) / 2;
  const loom_rect hide = {x, panel.y + h - 104 * s, button_w, 64 * s};
  nav_default(&ui);
  /* Back in SCREEN_EDIT, on the neighbour rather than the X that was. */
  const nav_id next = neighbour_of(app->id);
  if (ui_action_button(&kit, ctx, hide, "Hide", nav_str("confirm.hide")) &
      NAV_ACTIVATED) {
    layout_hide(&layout, app->id);
    save_layout();
    go_back();
    nav_focus(&ui, next);
    return;
  }
  if (app->removable && ui_button(&kit, ctx,
                                  (loom_rect){hide.x + button_w + 24 * s,
                                              hide.y, button_w, hide.h},
                                  "Uninstall", nav_str("confirm.remove")) &
                            NAV_ACTIVATED) {
    if (!tv_remove_app(app->id))
      snprintf(status, sizeof(status), "%s could not be removed", app->title);
    go_back();
    nav_focus(&ui, next);
  }
}

/* ------------------------------------------------------------------ frame */

/* ---------------------------------------------------------------- options
 *
 * Taking LG's Home's place at every start is a link in Homebrew Channel's
 * boot hooks to the hook the launcher carries (tools/home-boot-hook), which
 * runs home-takeover.sh. In LG's Home's place the launcher runs as root, so
 * it makes and removes the link itself.
 *
 * LG's Home comes back by restarting the TV with the link off: the takeover
 * is all in /tmp. Undoing it in place (home-takeover.sh restore) is for a
 * shell. */

/* The link; "" where there is none, off the TV. */
static char boot_hook_path[512];

static bool boot_enabled(void) {
  struct stat link;
  return boot_hook_path[0] != '\0' && lstat(boot_hook_path, &link) == 0;
}

static bool set_boot(bool on) {
  if (unlink(boot_hook_path) != 0 && errno != ENOENT)
    return false;
  return !on || symlink(INSTALLED_DIR "/home-boot-hook", boot_hook_path) == 0;
}

static void draw_options(loom_context *ctx) {
  const float s = kit.scale;
  const float w = 760 * s, h = 320 * s;
  const loom_rect panel = {(ctx->viewport.w - w) / 2, 140 * s, w, h};
  ui_panel(&kit, ctx, panel);
  const float x = panel.x + 48 * s, inner = w - 96 * s;
  const bool every_start = boot_enabled();
  loom_label(ctx, (loom_rect){x, panel.y + 40 * s, inner, 44 * s}, &panel,
             "Home", UI_TEXT, 32 * s);
  ui_wrapped(&kit, ctx, (loom_rect){x, panel.y + 96 * s, inner, 90 * s},
             every_start
                 ? "This Home takes LG's Home's place every time the TV "
                   "starts."
                 : "This Home has LG's Home's place until the TV restarts. "
                   "It can take it at every start instead.",
             21 * s, UI_DIM);
  const float button_w = (inner - 24 * s) / 2;
  const loom_rect toggle = {x, panel.y + h - 104 * s, button_w, 64 * s};
  nav_default(&ui);
  if (ui_action_button(&kit, ctx, toggle,
                       every_start ? "At every start: on"
                                   : "At every start: off",
                       nav_str("options.boot")) &
      NAV_ACTIVATED) {
    if (!set_boot(!every_start))
      snprintf(status, sizeof(status),
               "The TV's start-up could not be changed");
  }
  if (ui_button(&kit, ctx,
                (loom_rect){toggle.x + button_w + 24 * s, toggle.y, button_w,
                            toggle.h},
                "Restart the TV", nav_str("options.restart")) &
      NAV_ACTIVATED) {
    restarting = tv_restart();
    go_back();
  }
}

static void build_ui(loom_context *ctx) {
  const float width = (float)gl_width, height = (float)gl_height;
  kit.scale = minf(width / 1920.0f, height / 1080.0f);
  ui_frame_reset();
  icon_loads = 0;
  frame_number++;
  if (tv.apps_loaded)
    layout_sync(&layout, tv.apps, tv.app_count);
  loom_begin(ctx, width, height);
  ui.animate = animating();
  ui.duration = UI_NAVIGATION_NS;
  nav_begin(&ui, ctx, ui_now_ns());
  if (covering) {
    loom_fill(ctx, (loom_rect){0, 0, width, height}, NULL,
              (loom_color){0, 0, 0, 255}, 0);
    const icon *wallpaper = icon_for(background_path);
    if (wallpaper != NULL)
      draw_cover(ctx, wallpaper, (loom_rect){0, 0, width, height});
  }
  /* Behind the question, the bar cannot be reached. */
  nav_set_inert(&ui, screen == SCREEN_CONFIRM || screen == SCREEN_OPTIONS);
  draw_bar(ctx);
  nav_set_inert(&ui, false);
  if (screen == SCREEN_CONFIRM)
    draw_confirm(ctx);
  else if (screen == SCREEN_OPTIONS)
    draw_options(ctx);
  if (nav_end(&ui, UI_WHITE, 4 * kit.scale))
    jf_window_frame_requested = true;
}

/* ------------------------------------------------------------- the front */

/* Set as the launcher comes up, until the first key: the focus starts on the
 * app or input beneath it once the TV says which that is. */
static bool focus_foreground = true;
/* Until when a pause brings the launcher back; 0 for not. */
static uint64_t reshow_until;

static void focus_on_foreground(void) {
  fprintf(stderr, "home: in front is %s\n",
          tv.foreground[0] != '\0' ? tv.foreground : "nothing");
  if (screen != SCREEN_HOME || tv.foreground[0] == '\0')
    return;
  for (size_t i = 0; i < tv.input_count; i++)
    if (strcmp(tv.inputs[i].app_id, tv.foreground) == 0) {
      nav_focus(&ui, in_inputs(nav_str(tv.inputs[i].id)));
      return;
    }
  layout_sync(&layout, tv.apps, tv.app_count);
  const int at = layout_find(&layout, tv.foreground);
  if (at >= 0 && (size_t)at < layout.shown)
    nav_focus(&ui, in_ribbon(app_id_of(tv.foreground)));
}

/* ------------------------------------------------------------------ input */

static bool capture_requested;

/* Out of SCREEN_EDIT, keeping what was changed. */
static void finish_editing(void) {
  moving = false;
  save_layout();
  go_back();
}

static void on_key(uint32_t code, bool pressed, bool repeat) {
  (void)repeat;
  if (!pressed)
    return;
  jf_window_frame_requested = true;
  status[0] = '\0';
  tv.error[0] = '\0';
  /* Where the focus goes is the viewer's now. */
  focus_foreground = false;
  instant = false;
  reshow_until = 0;
  if (jf_window_is_back_key(code)) {
    if (screen == SCREEN_EDIT)
      finish_editing();
    /* On the first screen, out of the way of the app beneath. */
    else if (ui_stack_depth(&stack) == 0)
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
  if (screen == SCREEN_EDIT && moving) {
    /* The app goes along; the focus stays on it. */
    if ((dir == 2 || dir == 3) && focused_app[0] != '\0' &&
        layout_move(&layout, focused_app, dir == 3 ? 1 : -1))
      /* Scrolled along with it. */
      nav_focus(&ui, ui.focus);
    return;
  }
  /* Between the apps and the inputs, back to where each was left; Up from
   * the apps puts the launcher out of the way. */
  if (screen == SCREEN_HOME && (dir == 0 || dir == 1)) {
    const bool below = inputs_focused();
    if (dir == 0 && !below)
      jf_window_minimize();
    else if (dir == 1 && !below && tv.input_count > 0)
      nav_focus(&ui, last_input_focus != 0
                         ? last_input_focus
                         : in_inputs(nav_str(tv.inputs[0].id)));
    else if (dir == 0 && below)
      nav_focus(&ui, last_app_focus);
    return;
  }
  nav_move(&ui, dirs[dir]);
}

static void on_event(const jf_event *event) {
  switch (event->kind) {
  case JF_EVENT_KEY:
    on_key(event->key.code, event->key.pressed, event->key.repeat);
    break;
  case JF_EVENT_POINTER_ENTER:
    /* The window coming up under a pointer resting where it was, not the
     * viewer pointing: where it is, without the focus going to what is
     * under it. */
    ui.pointer_x = (float)jf_fixed_to_int(event->pointer.x);
    ui.pointer_y = (float)jf_fixed_to_int(event->pointer.y);
    ui.pointer_present = true;
    jf_window_frame_requested = true;
    break;
  case JF_EVENT_POINTER_MOTION:
    focus_foreground = false;
    instant = false;
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
 * nothing left in front once an app closes, or the TV coming on. */
enum { RELAUNCH_NONE, RELAUNCH_OTHER, RELAUNCH_HOME_KEY, RELAUNCH_BOOT };
static atomic_int relaunched;

static void relaunch(int why) {
  atomic_store(&relaunched, why);
  jf_window_wake();
}

static void on_relaunch(void) {
  const jf_relaunch_reason why = jf_luna_relaunch_reason();
  relaunch(why == JF_RELAUNCH_HOME_KEY ? RELAUNCH_HOME_KEY
           : why == JF_RELAUNCH_BOOT   ? RELAUNCH_BOOT
                                       : RELAUNCH_OTHER);
}

/* The TV coming on opens Home twice: for the start (reason "boot"), then for
 * the last input (sam's last_input_handler, reason "undefined"). LG's Home
 * opens the last input then and stays out of sight; so does the launcher, for
 * the second as for the first. */
#define WAKE_NS 15000000000ull
static uint64_t woke_at;

/* SAM brings Home up when the app in front closes, and the input it goes back
 * to comes up over it a moment later, hiding it: SAM says so with a pause.
 * Within this long of such a relaunch, and before any key, the launcher comes
 * back over that input, once. */
#define RESHOW_NS 3000000000ull
static atomic_bool paused;

static void on_pause(void) {
  atomic_store(&paused, true);
  jf_window_wake();
}

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
  jf_luna_set_pause_handler(on_pause);
  if (!jf_luna_register_lifecycle(jf_window_post_quit, on_relaunch))
    fprintf(stderr, "no webOS lifecycle\n");
  renderer = jf_renderer_create(NULL, 0, 0);
  if (renderer == NULL) {
    fprintf(stderr, "the UI renderer could not start\n");
    return false;
  }
  kit.renderer = renderer;
  loom_init(&ctx);
  /* Kept with the installed app, which a reinstall starts afresh. */
  const char *path = getenv("HOME_LAYOUT");
#ifdef JF_WEBOS
  if (path == NULL)
    path = INSTALLED_DIR "/layout.txt";
#else
  if (path == NULL)
    path = "/tmp/home-layout.txt";
#endif
  snprintf(layout_path, sizeof(layout_path), "%s", path);
  layout_load(&layout, layout_path);
  /* Outside the app's folder, so a reinstall keeps it. */
  const char *background = getenv("HOME_BACKGROUND");
#ifdef JF_WEBOS
  if (background == NULL)
    background = "/media/developer/home-background.png";
#endif
  snprintf(background_path, sizeof(background_path), "%s",
           background != NULL ? background : "");
  const char *hook = getenv("HOME_BOOT_HOOK");
#ifdef JF_WEBOS
  if (hook == NULL)
    hook = "/var/lib/webosbrew/init.d/60-home-takeover";
#endif
  snprintf(boot_hook_path, sizeof(boot_hook_path), "%s",
           hook != NULL ? hook : "");
  tv_init(jf_window_wake);
  tv_refresh();
  /* Started by the TV coming on, as against opened. */
  char reason[32] = "";
  bool boot = false;
  if (argc > 1 &&
      ((jf_luna_json_string(argv[1], "reason", reason, sizeof(reason)) &&
        strcmp(reason, "boot") == 0) ||
       (jf_luna_json_bool(argv[1], "boot", &boot) && boot)))
    relaunch(RELAUNCH_BOOT);
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
  const bool waking = relaunch == RELAUNCH_BOOT ||
                      (relaunch == RELAUNCH_OTHER && woke_at != 0 &&
                       ui_now_ns() - woke_at < WAKE_NS);
  if (relaunch == RELAUNCH_HOME_KEY)
    woke_at = 0;
  if (waking) {
    if (relaunch == RELAUNCH_BOOT) {
      woke_at = ui_now_ns();
      tv_open_last_input();
    }
    reshow_until = 0;
    covering = false;
    go_home();
    if (jf_window_drawable)
      jf_window_minimize();
  } else if (relaunch == RELAUNCH_HOME_KEY && jf_window_drawable &&
             jf_window_focused()) {
    reshow_until = 0;
    jf_window_minimize();
  } else if (relaunch != RELAUNCH_NONE) {
    go_home();
    tv_refresh();
    focus_foreground = true;
    instant = true;
    reshow_until = relaunch == RELAUNCH_OTHER ? ui_now_ns() + RESHOW_NS : 0;
    /* SAM opening Home with nothing in front: an app closed, and the
     * compositor may be bringing the last input up, which is not to be
     * seen. */
    covering = relaunch == RELAUNCH_OTHER;
    jf_window_post_raise();
    jf_window_frame_requested = true;
  }
  /* Back in front from an app: what is installed and plugged in may have
   * changed while it was. */
  if (atomic_exchange(&paused, false) && reshow_until != 0 &&
      ui_now_ns() < reshow_until) {
    reshow_until = 0;
    jf_window_post_raise();
    jf_window_frame_requested = true;
  }
  if (jf_window_drawable && !was_drawable) {
    tv_refresh();
    focus_foreground = true;
    instant = true;
    jf_window_frame_requested = true;
  }
  /* Put away, it is opened uncovered next time, unless SAM says otherwise. */
  if (!jf_window_drawable && was_drawable)
    covering = false;
  was_drawable = jf_window_drawable;
  if (tv_pump()) {
    jf_window_frame_requested = true;
    /* What it hid for was refused: back, saying so. */
    if (hidden_for_launch && tv.error[0] != '\0')
      jf_window_post_raise();
    if (tv_pending() == 0)
      hidden_for_launch = false;
    /* Once the lists it is looked up in are in too. */
    if (tv.foreground_fresh && tv.apps_loaded && tv.inputs_loaded) {
      tv.foreground_fresh = false;
      if (focus_foreground)
        focus_on_foreground();
      else
        fprintf(stderr, "home: a key came before what is in front\n");
      focus_foreground = false;
    }
  }
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
  /* The frame the focus went to what is in front was the last drawn so. */
  if (!focus_foreground)
    instant = false;
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
#ifdef JF_WEBOS
/* Opened from LG's menu, as the app it is installed as: it is not LG's Home
 * yet, and runs as a developer app without the rights to make itself that.
 * Homebrew Channel's service runs the script as root, which takes the place
 * and opens it; the script says on screen what went wrong, if anything. */
static atomic_int exec_answer; /* 0 waiting, 1 run, -1 not */

static void on_exec(const char *payload, void *user) {
  (void)user;
  bool ran = false;
  atomic_store(&exec_answer,
               payload != NULL &&
                       jf_luna_json_bool(payload, "returnValue", &ran) && ran
                   ? 1
                   : -1);
}

static int take_lg_homes_place(void) {
  if (!jf_luna_call("luna://org.webosbrew.hbchannel.service/exec",
                    "{\"command\":\"setsid sh " INSTALLED_DIR
                    "/home-takeover.sh take open </dev/null "
                    ">/tmp/home-takeover.log 2>&1 &\"}",
                    on_exec, NULL))
    atomic_store(&exec_answer, -1);
  for (int i = 0; i < 100 && atomic_load(&exec_answer) == 0; i++)
    usleep(100000);
  if (atomic_load(&exec_answer) == 1)
    return 0;
  fprintf(stderr, "home: Homebrew Channel did not run the takeover\n");
  atomic_store(&exec_answer, 0);
  jf_luna_call("luna://com.webos.notification/createToast",
               "{\"sourceId\":\"" APP_ID
               "\",\"message\":\"Taking LG's Home's place needs Homebrew "
               "Channel\"}",
               on_exec, NULL);
  for (int i = 0; i < 30 && atomic_load(&exec_answer) == 0; i++)
    usleep(100000);
  return 1;
}

/* Started as LG's Home: SAM names it so in the launch payload. */
static bool started_as_lg_home(int argc, char **argv) {
  char id[128] = "";
  const char *env = getenv("APPID");
  if (env != NULL)
    snprintf(id, sizeof(id), "%s", env);
  if (argc > 1)
    jf_luna_json_string(argv[1], "appId", id, sizeof(id));
  return strcmp(id, LG_HOME_ID) == 0;
}
#endif

int main(int argc, char **argv) {
#ifdef JF_WEBOS
  if (!started_as_lg_home(argc, argv))
    return take_lg_homes_place();
#endif
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
