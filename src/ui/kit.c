#include "kit.h"

#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../platform/gl.h"
#include "../platform/window.h"
#include "theme.h"

uint64_t ui_now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Sized for a full grid page: every card formats a title and a subtitle to
 * fit its width. */
static char scratch[256][160];
static size_t scratch_used;

const char *ui_fmt(const char *pattern, ...) {
  if (scratch_used == sizeof(scratch) / sizeof(scratch[0]))
    return "...";
  char *out = scratch[scratch_used++];
  va_list args;
  va_start(args, pattern);
  vsnprintf(out, sizeof(scratch[0]), pattern, args);
  va_end(args);
  return out;
}

void ui_frame_reset(void) { scratch_used = 0; }

/* ------------------------------------------------------------- animation */

bool ui_anim_running(const ui_anim *value) { return value->started_at != 0; }

void ui_anim_snap(ui_anim *value, float target) {
  value->value = target;
  value->start = target;
  value->target = target;
  value->started_at = 0;
  value->duration = 0;
}

void ui_anim_update(ui_anim *value, uint64_t now) {
  if (!ui_anim_running(value))
    return;
  const float progress =
      value->duration == 0 ? 1.0f
                           : (float)(now - value->started_at) / value->duration;
  if (progress >= 1.0f) {
    ui_anim_snap(value, value->target);
    return;
  }
  const float eased = 1.0f - powf(2.0f, -10.0f * progress);
  value->value = value->start + (value->target - value->start) * eased;
}

void ui_anim_set(ui_anim *value, float target, uint64_t duration,
                 bool animate) {
  if (!animate || fabsf(value->target - target) < 0.01f) {
    if (!animate)
      ui_anim_snap(value, target);
    return;
  }
  ui_anim_update(value, ui_now_ns());
  value->start = value->value;
  value->target = target;
  value->started_at = ui_now_ns();
  value->duration = duration;
}

/* -------------------------------------------------------------- controls */

static void button_label(ui_kit *kit, loom_context *ctx, loom_rect rect,
                         const char *label, const loom_color color) {
  const float s = kit->scale;
  loom_label(ctx,
             (loom_rect){rect.x + 24 * s, rect.y + (rect.h - 30 * s) / 2,
                         rect.w - 48 * s, 38 * s},
             &rect, label, color, 25 * s);
}

unsigned ui_button(ui_kit *kit, loom_context *ctx, loom_rect rect,
                   const char *label, nav_id id) {
  const float s = kit->scale;
  const unsigned flags = nav_item(kit->nav, id, rect, ctx->viewport);
  const bool focused = flags & NAV_FOCUSED;
  loom_fill(ctx, rect, NULL, flags & NAV_HOVERED ? UI_HOT : UI_CARD, 12 * s);
  loom_stroke(ctx, rect, NULL, focused ? UI_ACCENT : UI_BORDER,
              focused ? 4 * s : 2 * s, 12 * s);
  button_label(kit, ctx, rect, label, UI_TEXT);
  return flags;
}

unsigned ui_action_button(ui_kit *kit, loom_context *ctx, loom_rect rect,
                          const char *label, nav_id id) {
  const float s = kit->scale;
  const unsigned flags = nav_item(kit->nav, id, rect, ctx->viewport);
  const bool focused = flags & NAV_FOCUSED;
  loom_fill(ctx, rect, NULL, flags & NAV_HOVERED ? UI_SELECTED : UI_ACCENT,
            12 * s);
  loom_stroke(ctx, rect, NULL, focused ? UI_WHITE : UI_ACCENT,
              focused ? 4 * s : 2 * s, 12 * s);
  button_label(kit, ctx, rect, label, UI_WHITE);
  return flags;
}

unsigned ui_value_row(ui_kit *kit, loom_context *ctx, loom_rect rect,
                      const loom_rect *clip, const char *label,
                      const char *value, nav_id id) {
  const float s = kit->scale;
  const loom_rect bounds = clip != NULL ? *clip : ctx->viewport;
  const unsigned flags = nav_item(kit->nav, id, rect, bounds);
  const bool focused = flags & NAV_FOCUSED;
  loom_fill(ctx, rect, &bounds, flags & NAV_HOVERED ? UI_HOT : UI_CARD, 12 * s);
  loom_stroke(ctx, rect, &bounds, focused ? UI_ACCENT : UI_BORDER,
              focused ? 4 * s : 2 * s, 12 * s);
  const loom_rect inside = loom_intersect(rect, bounds);
  const float size = 25 * s;
  const float y = rect.y + (rect.h - 30 * s) / 2;
  loom_label(ctx, (loom_rect){rect.x + 24 * s, y, rect.w * 0.6f, 38 * s},
             &inside, label, UI_TEXT, size);
  if (value != NULL && value[0] != '\0') {
    const float width = jf_renderer_measure(kit->renderer, value, size);
    loom_label(ctx,
               (loom_rect){rect.x + rect.w - 24 * s - width, y, width, 38 * s},
               &inside, value, focused ? UI_ACCENT : UI_DIM, size);
  }
  return flags;
}

void ui_panel(ui_kit *kit, loom_context *ctx, loom_rect rect) {
  loom_fill(ctx, rect, NULL, UI_PANEL, 18 * kit->scale);
  loom_stroke(ctx, rect, NULL, UI_BORDER, 2 * kit->scale, 18 * kit->scale);
}

void ui_spinner(ui_kit *kit, loom_context *ctx, float x, float y, float radius,
                const loom_rect *clip) {
  jf_window_frame_requested = true;
  const unsigned phase = (unsigned)(ui_now_ns() / 90000000ull) % 8;
  for (unsigned i = 0; i < 8; i++) {
    const float angle = ((float)i / 8.0f) * 6.2831853f - 1.5707963f;
    const float dot = 7.0f * kit->scale;
    loom_color color = {UI_DIM[0], UI_DIM[1], UI_DIM[2], i == phase ? 255 : 72};
    loom_fill(ctx,
              (loom_rect){x + cosf(angle) * radius - dot / 2,
                          y + sinf(angle) * radius - dot / 2, dot, dot},
              clip, color, dot / 2);
  }
}

const char *ui_ellipsize(ui_kit *kit, const char *text, float width,
                         float size) {
  if (jf_renderer_measure(kit->renderer, text, size) <= width)
    return text;
  const float dots = jf_renderer_measure(kit->renderer, "...", size);
  char probe[256];
  size_t take = strlen(text);
  if (take > sizeof(probe) - 1)
    take = sizeof(probe) - 1;
  while (take > 1) {
    memcpy(probe, text, take);
    probe[take] = '\0';
    if (jf_renderer_measure(kit->renderer, probe, size) <= width - dots)
      break;
    take--;
    while (take > 0 && ((unsigned char)text[take] & 0xc0) == 0x80)
      take--;
  }
  memcpy(probe, text, take);
  probe[take] = '\0';
  return ui_fmt("%s...", probe);
}

/* The renderer clips a label to its rect but does not break it, so a
 * paragraph needs splitting before it becomes draw commands. */
void ui_wrapped(ui_kit *kit, loom_context *ctx, loom_rect rect,
                const char *text, float size, const loom_color color) {
  float y = rect.y;
  const char *rest = text;
  char line[512];
  while (*rest != '\0' && y + size * 1.4f <= rect.y + rect.h) {
    const char *newline = strchr(rest, '\n');
    size_t limit = newline != NULL ? (size_t)(newline - rest) : strlen(rest);
    if (limit == 0) {
      rest++;
      y += size * 1.45f;
      continue;
    }
    if (limit > sizeof(line) - 1)
      limit = sizeof(line) - 1;
    size_t take = limit;
    for (;;) {
      memcpy(line, rest, take);
      line[take] = '\0';
      if (jf_renderer_measure(kit->renderer, line, size) <= rect.w)
        break;
      const char *space = NULL;
      for (size_t i = take; i > 0; i--)
        if (rest[i - 1] == ' ') {
          space = rest + i - 1;
          break;
        }
      if (space == NULL)
        break;
      take = (size_t)(space - rest);
    }
    if (take == 0)
      break;
    memcpy(line, rest, take);
    line[take] = '\0';
    loom_label(ctx, (loom_rect){rect.x, y, rect.w, size * 1.4f}, &rect,
               ui_fmt("%s", line), color, size);
    rest += take;
    if (take == limit && newline != NULL)
      rest++;
    while (*rest == ' ' || *rest == '\t' || *rest == '\r')
      rest++;
    y += size * 1.45f;
  }
}

/* ------------------------------------------------------------ development */

bool ui_capture(const char *path) {
  const size_t width = gl_width;
  const size_t height = gl_height;
  uint8_t *rgba = malloc(width * height * 4);
  uint8_t *row = malloc(width * 3);
  FILE *file = NULL;
  bool ok = false;
  if (rgba == NULL || row == NULL)
    goto done;
  glReadPixels(0, 0, (GLsizei)width, (GLsizei)height, GL_RGBA, GL_UNSIGNED_BYTE,
               rgba);
  file = fopen(path, "wb");
  if (file == NULL)
    goto done;
  fprintf(file, "P6\n%zu %zu\n255\n", width, height);
  for (size_t y = 0; y < height; y++) {
    const uint8_t *source = rgba + (height - 1 - y) * width * 4;
    for (size_t x = 0; x < width; x++) {
      row[x * 3 + 0] = source[x * 4 + 0];
      row[x * 3 + 1] = source[x * 4 + 1];
      row[x * 3 + 2] = source[x * 4 + 2];
    }
    fwrite(row, 1, width * 3, file);
  }
  fprintf(stderr, "captured OpenGL framebuffer to %s (%zux%zu)\n", path, width,
          height);
  ok = true;
done:
  if (file != NULL)
    fclose(file);
  free(rgba);
  free(row);
  return ok;
}

void ui_log_to_file(const char *path, const char *fallback) {
  if (isatty(2))
    return;
  int file = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file < 0 && fallback != NULL)
    file = open(fallback, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (file < 0)
    return;
  dup2(file, 1);
  dup2(file, 2);
  close(file);
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IOLBF, 0);
}

int ui_script_next(ui_script *script, bool busy) {
  if (script->keys == NULL || script->keys[script->at] == '\0')
    return busy ? 0 : -1;
  if (busy)
    return 0;
  if (script->wait != 0) {
    script->wait--;
    return 0;
  }
  script->wait = UI_SCRIPT_BEAT;
  return (unsigned char)script->keys[script->at++];
}

uint32_t ui_script_key(char command) {
  switch (command) {
  case 'u':
    return 103;
  case 'd':
    return 108;
  case 'l':
    return 105;
  case 'r':
    return 106;
  case 'o':
    return 28;
  case 'b':
    return jf_window_on_webos ? 412 : 158;
  default:
    return 0;
  }
}
