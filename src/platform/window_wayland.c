/*
 * The platform layer of window.h on the TV, talking to the compositor itself
 * rather than through SDL. What it has that SDL cannot give: the surface's
 * properties, the window type above all - a launcher has to be an overlay,
 * laid over the app in front, which keeps playing, where every SDL window is
 * a card that sends it to the background (docs/home.md). What it does not
 * have yet: the on-screen keyboard and the exported video window, which is
 * why jellyfin stays on window.c.
 *
 * libwayland-client is opened at runtime, as the TV's SDL does, and only the
 * calls that SDL looks up are used. wl_proxy_marshal_flags is 1.20's, and
 * older TVs ship an older libwayland (the reason this layer once moved to
 * SDL), so it is used only where it exists, wl_proxy_marshal_constructor and
 * its _versioned form elsewhere. Requests are found by name in the
 * interfaces' own method tables, so no opcode is written down here; the webOS
 * interfaces come from libwayland-webos-client.
 *
 * Keys arrive as evdev codes already, which is what the app speaks. A wheel
 * notch is reported as SDL's path reports one, and keys repeat here, as the
 * protocol leaves that to the client.
 */
#include "window.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "gl.h"

uint32_t gl_width;
uint32_t gl_height;

bool jf_window_log_keys;
bool jf_window_log_events;
bool jf_window_running = true;
bool jf_window_drawable = true;
bool jf_window_frame_requested = true;
bool jf_window_on_webos = true;
uint32_t jf_window_refresh_mhz = 60000;

/* ------------------------------------------------------ libwayland's ABI */

struct wl_message {
  const char *name, *signature;
  const void **types;
};
struct wl_interface {
  const char *name;
  int version;
  int method_count;
  const struct wl_message *methods;
  int event_count;
  const struct wl_message *events;
};
typedef struct wl_proxy wl_proxy;
typedef void (*listener_fn)(void);

static struct {
  void *(*connect)(const char *);
  void (*disconnect)(void *);
  int (*dispatch_pending)(void *);
  int (*flush)(void *);
  int (*get_fd)(void *);
  int (*roundtrip)(void *);
  int (*prepare_read)(void *);
  int (*read_events)(void *);
  void (*cancel_read)(void *);
  int (*add_listener)(wl_proxy *, listener_fn *, void *);
  uint32_t (*get_version)(wl_proxy *);
  void (*destroy)(wl_proxy *);
  wl_proxy *(*marshal_flags)(wl_proxy *, uint32_t, const struct wl_interface *,
                             uint32_t, uint32_t, ...);
  wl_proxy *(*marshal_versioned)(wl_proxy *, uint32_t,
                                 const struct wl_interface *, uint32_t, ...);
  wl_proxy *(*marshal_constructor)(wl_proxy *, uint32_t,
                                   const struct wl_interface *, ...);
  void *(*egl_window_create)(wl_proxy *, int, int);
  void (*egl_window_destroy)(void *);
  const struct wl_interface *display, *registry, *compositor, *surface, *seat,
      *keyboard, *pointer, *output, *shell, *shell_surface;
  /* Not on every TV: without it the pointer is the compositor's to hide. */
  const struct wl_interface *input_manager;
} wl;

static bool load_symbol(void *library, const char *name, void *out) {
  void *found = library != NULL ? dlsym(library, name) : NULL;
  memcpy(out, &found, sizeof(found));
  if (found == NULL)
    fprintf(stderr, "wayland: no %s\n", name);
  return found != NULL;
}

static bool load(void) {
  void *client = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_GLOBAL);
  void *webos = dlopen("libwayland-webos-client.so.1", RTLD_NOW);
  void *egl = dlopen("libwayland-egl.so.1", RTLD_NOW);
  if (client == NULL || webos == NULL || egl == NULL) {
    fprintf(stderr, "wayland: libraries client=%p webos=%p egl=%p\n", client,
            webos, egl);
    return false;
  }
  bool ok = true;
  ok &= load_symbol(client, "wl_display_connect", &wl.connect);
  ok &= load_symbol(client, "wl_display_disconnect", &wl.disconnect);
  ok &=
      load_symbol(client, "wl_display_dispatch_pending", &wl.dispatch_pending);
  ok &= load_symbol(client, "wl_display_flush", &wl.flush);
  ok &= load_symbol(client, "wl_display_get_fd", &wl.get_fd);
  ok &= load_symbol(client, "wl_display_roundtrip", &wl.roundtrip);
  ok &= load_symbol(client, "wl_display_prepare_read", &wl.prepare_read);
  ok &= load_symbol(client, "wl_display_read_events", &wl.read_events);
  ok &= load_symbol(client, "wl_display_cancel_read", &wl.cancel_read);
  ok &= load_symbol(client, "wl_proxy_add_listener", &wl.add_listener);
  ok &= load_symbol(client, "wl_proxy_get_version", &wl.get_version);
  ok &= load_symbol(client, "wl_proxy_destroy", &wl.destroy);
  ok &= load_symbol(client, "wl_proxy_marshal_constructor",
                    &wl.marshal_constructor);
  /* Newer than some TVs' libwayland: used where they are there. */
  wl.marshal_flags = dlsym(client, "wl_proxy_marshal_flags");
  wl.marshal_versioned =
      dlsym(client, "wl_proxy_marshal_constructor_versioned");
  ok &= load_symbol(egl, "wl_egl_window_create", &wl.egl_window_create);
  ok &= load_symbol(egl, "wl_egl_window_destroy", &wl.egl_window_destroy);
  ok &= load_symbol(client, "wl_display_interface", &wl.display);
  ok &= load_symbol(client, "wl_registry_interface", &wl.registry);
  ok &= load_symbol(client, "wl_compositor_interface", &wl.compositor);
  ok &= load_symbol(client, "wl_surface_interface", &wl.surface);
  ok &= load_symbol(client, "wl_seat_interface", &wl.seat);
  ok &= load_symbol(client, "wl_keyboard_interface", &wl.keyboard);
  ok &= load_symbol(client, "wl_pointer_interface", &wl.pointer);
  ok &= load_symbol(client, "wl_output_interface", &wl.output);
  ok &= load_symbol(webos, "wl_webos_shell_interface", &wl.shell);
  ok &=
      load_symbol(webos, "wl_webos_shell_surface_interface", &wl.shell_surface);
  wl.input_manager = dlsym(webos, "wl_webos_input_manager_interface");
  return ok;
}

static bool opcode(const struct wl_interface *i, const char *name,
                   uint32_t *out) {
  for (int n = 0; n < i->method_count; n++)
    if (strcmp(i->methods[n].name, name) == 0) {
      *out = (uint32_t)n;
      return true;
    }
  fprintf(stderr, "wayland: %s has no request %s\n", i->name, name);
  return false;
}

/* Every request this sends: up to four arguments, a new object's NULL
 * placeholder among them where its signature puts it. The new object gets
 * `made_version`, or the sender's version for 0. */
static wl_proxy *send_request(wl_proxy *p, const struct wl_interface *i,
                              const char *name, const struct wl_interface *made,
                              uint32_t made_version, uintptr_t a, uintptr_t b,
                              uintptr_t c, uintptr_t d) {
  uint32_t op;
  if (p == NULL || !opcode(i, name, &op))
    return NULL;
  const uint32_t version = made_version != 0 ? made_version : wl.get_version(p);
  if (wl.marshal_flags != NULL)
    return wl.marshal_flags(p, op, made, version, 0, a, b, c, d);
  if (made != NULL && wl.marshal_versioned != NULL)
    return wl.marshal_versioned(p, op, made, version, a, b, c, d);
  return wl.marshal_constructor(p, op, made, a, b, c, d);
}

/* ------------------------------------------------------------ the window */

static void *display;
static wl_proxy *registry, *compositor, *shell, *output, *input_manager;
static wl_proxy *surface, *shell_surface;
static void *egl_window;
static EGLDisplay egl_display = EGL_NO_DISPLAY;
static EGLContext egl_context = EGL_NO_CONTEXT;
static EGLSurface egl_surface = EGL_NO_SURFACE;
static EGLConfig egl_config;
/* Unmapped by jf_window_minimize, until the next raise. */
static bool hidden;
static jf_event_fn handler;
static char window_type[48];

/* The remote's keys arrive on one seat and the others mirror its modifiers
 * (docs/input.md), so every seat is bound. */
#define SEATS 4
static struct {
  wl_proxy *seat, *keyboard, *pointer;
  /* The keyboard's focus is on our surface. */
  bool focused;
} seats[SEATS];
static size_t seat_count;

static int output_width, output_height;

/* Another thread's requests, carried out on the main thread by the pump. */
static int wake_pipe[2] = {-1, -1};
static atomic_bool quit_asked, raise_asked;

void jf_window_set_handler(jf_event_fn value) { handler = value; }
void jf_window_set_type(const char *type) {
  snprintf(window_type, sizeof(window_type), "%s", type);
}

static void emit(const jf_event *event) {
  if (handler != NULL)
    handler(event);
}

bool jf_window_is_back_key(uint32_t code) {
  return code == 1 || code == 158 || code == 412;
}

static void set_property(const char *name, const char *value) {
  send_request(shell_surface, wl.shell_surface, "set_property", NULL, 0,
               (uintptr_t)name, (uintptr_t)value, 0, 0);
}

/* wl_webos_shell_surface's state enum. */
enum { STATE_DEFAULT, STATE_MINIMIZED, STATE_MAXIMIZED, STATE_FULLSCREEN };

static void set_state(uint32_t state) {
  send_request(shell_surface, wl.shell_surface, "set_state", NULL, 0, state, 0,
               0, 0);
}

/* ------------------------------------------------------------ key repeat */

static uint64_t now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* wl_keyboard.repeat_info's, until a seat sends its own. */
static int32_t repeat_rate = 25, repeat_delay = 500;
static uint32_t repeat_key;
static uint64_t repeat_at;

/* Milliseconds until the held key repeats, -1 for none. */
static int repeat_wait(void) {
  if (repeat_key == 0 || repeat_rate <= 0)
    return -1;
  const uint64_t now = now_ms();
  return repeat_at > now ? (int)(repeat_at - now) : 0;
}

/* The remote's colour buttons, as the SDL backend's keymap gives them: the
 * TV's only spare inputs, so they mean the same in either. */
static uint32_t app_key(uint32_t code) {
  switch (code) {
  case 398: /* KEY_RED */
    return 64;
  case 399: /* KEY_GREEN */
    return 65;
  case 400: /* KEY_YELLOW */
    return 370;
  case 401: /* KEY_BLUE */
    return 67;
  default:
    return code;
  }
}

static void key(uint32_t code, bool pressed, bool repeat) {
  if (jf_window_log_keys)
    fprintf(stderr, "wayland: key %u %s%s\n", code, pressed ? "down" : "up",
            repeat ? " (repeat)" : "");
  jf_event out = {.kind = JF_EVENT_KEY};
  out.key.code = app_key(code);
  out.key.pressed = pressed;
  out.key.repeat = repeat;
  emit(&out);
}

static void repeat_keys(void) {
  if (repeat_wait() != 0)
    return;
  key(repeat_key, true, true);
  repeat_at = now_ms() + (uint64_t)(1000 / repeat_rate);
}

/* ---------------------------------------------------------------- events */

static void on_keymap(void *d, wl_proxy *k, uint32_t format, int32_t fd,
                      uint32_t size) {
  (void)d, (void)k, (void)format, (void)size;
  close(fd);
}
/* Where the remote's keys go is the compositor's to say, and it says it per
 * seat: anything brought up over the window - an app, an input chosen in
 * LG's picker - takes the focus from it, with no other word to the window. */
static void on_key_enter(void *d, wl_proxy *k, uint32_t serial, wl_proxy *s,
                         void *keys) {
  (void)k, (void)serial, (void)keys;
  seats[(size_t)(uintptr_t)d].focused = s == surface;
  if (jf_window_log_events)
    fprintf(stderr, "wayland: focus in\n");
}
static void on_key_leave(void *d, wl_proxy *k, uint32_t serial, wl_proxy *s) {
  (void)k, (void)serial, (void)s;
  seats[(size_t)(uintptr_t)d].focused = false;
  repeat_key = 0;
  if (jf_window_log_events)
    fprintf(stderr, "wayland: focus out\n");
}
/* The magic remote's pointer, as the compositor last said. Hidden here on
 * every key, as LG's SDL does; moving the remote brings it back. */
static bool cursor_shown;

static void hide_cursor(void) {
  if (!cursor_shown || input_manager == NULL)
    return;
  send_request(input_manager, wl.input_manager, "set_cursor_visibility", NULL,
               0, 0, 0, 0, 0);
  cursor_shown = false;
  emit(&(jf_event){.kind = JF_EVENT_POINTER_LEAVE});
}

static void on_cursor_visibility(void *d, wl_proxy *m, uint32_t visibility,
                                 wl_proxy *webos_seat) {
  (void)d, (void)m, (void)webos_seat;
  if (jf_window_log_events)
    fprintf(stderr, "wayland: pointer %s\n", visibility ? "shown" : "hidden");
  cursor_shown = visibility != 0;
  if (!cursor_shown)
    emit(&(jf_event){.kind = JF_EVENT_POINTER_LEAVE});
}
static void ignore_event(void) {}
/* Filled by name from the interface's own event table, the rest ignored. */
static listener_fn input_manager_listener[8];

static void on_key(void *d, wl_proxy *k, uint32_t serial, uint32_t time,
                   uint32_t code, uint32_t state) {
  (void)d, (void)k, (void)serial, (void)time;
  const bool pressed = state == 1;
  if (pressed)
    hide_cursor();
  if (pressed) {
    repeat_key = code;
    repeat_at = now_ms() + (uint64_t)repeat_delay;
  } else if (code == repeat_key) {
    repeat_key = 0;
  }
  key(code, pressed, false);
}
static void on_modifiers(void *d, wl_proxy *k, uint32_t serial, uint32_t a,
                         uint32_t b, uint32_t c, uint32_t e) {
  (void)d, (void)k, (void)serial, (void)a, (void)b, (void)c, (void)e;
}
static void on_repeat_info(void *d, wl_proxy *k, int32_t rate, int32_t delay) {
  (void)d, (void)k;
  repeat_rate = rate;
  repeat_delay = delay;
}
static listener_fn keyboard_listener[] = {
    (listener_fn)on_keymap,    (listener_fn)on_key_enter,
    (listener_fn)on_key_leave, (listener_fn)on_key,
    (listener_fn)on_modifiers, (listener_fn)on_repeat_info};

/* Wayland's fixed point is 24.8, as jf_fixed is. */
static void pointer_at(jf_event_kind kind, int32_t x, int32_t y) {
  jf_event out = {.kind = kind};
  out.pointer.x = x;
  out.pointer.y = y;
  emit(&out);
}
static void on_pointer_enter(void *d, wl_proxy *p, uint32_t serial, wl_proxy *s,
                             int32_t x, int32_t y) {
  (void)d, (void)p, (void)serial, (void)s;
  pointer_at(JF_EVENT_POINTER_ENTER, x, y);
}
static void on_pointer_leave(void *d, wl_proxy *p, uint32_t serial,
                             wl_proxy *s) {
  (void)d, (void)p, (void)serial, (void)s;
  emit(&(jf_event){.kind = JF_EVENT_POINTER_LEAVE});
}
static void on_motion(void *d, wl_proxy *p, uint32_t time, int32_t x,
                      int32_t y) {
  (void)d, (void)p, (void)time;
  cursor_shown = true;
  pointer_at(JF_EVENT_POINTER_MOTION, x, y);
}
static void on_button(void *d, wl_proxy *p, uint32_t serial, uint32_t time,
                      uint32_t button, uint32_t state) {
  (void)d, (void)p, (void)serial, (void)time;
  jf_event out = {.kind = JF_EVENT_POINTER_BUTTON};
  out.button.button = button;
  out.button.pressed = state == 1;
  emit(&out);
}
static void on_axis(void *d, wl_proxy *p, uint32_t time, uint32_t axis,
                    int32_t value) {
  (void)d, (void)p, (void)time;
  jf_event out = {.kind = JF_EVENT_POINTER_AXIS};
  out.axis.axis = axis;
  out.axis.value = value;
  emit(&out);
}
static listener_fn pointer_listener[] = {
    (listener_fn)on_pointer_enter, (listener_fn)on_pointer_leave,
    (listener_fn)on_motion, (listener_fn)on_button, (listener_fn)on_axis};

static void on_capabilities(void *data, wl_proxy *seat, uint32_t caps) {
  const size_t i = (size_t)(uintptr_t)data;
  if ((caps & 2) != 0 && seats[i].keyboard == NULL) {
    seats[i].keyboard =
        send_request(seat, wl.seat, "get_keyboard", wl.keyboard, 0, 0, 0, 0, 0);
    if (seats[i].keyboard != NULL)
      wl.add_listener(seats[i].keyboard, keyboard_listener, data);
  }
  if ((caps & 1) != 0 && seats[i].pointer == NULL) {
    seats[i].pointer =
        send_request(seat, wl.seat, "get_pointer", wl.pointer, 0, 0, 0, 0, 0);
    if (seats[i].pointer != NULL)
      wl.add_listener(seats[i].pointer, pointer_listener, NULL);
  }
}
static void on_seat_name(void *d, wl_proxy *s, const char *name) {
  (void)d, (void)s, (void)name;
}
static listener_fn seat_listener[] = {(listener_fn)on_capabilities,
                                      (listener_fn)on_seat_name};

static void on_geometry(void *d, wl_proxy *o, int32_t x, int32_t y, int32_t pw,
                        int32_t ph, int32_t subpixel, const char *make,
                        const char *model, int32_t transform) {
  (void)d, (void)o, (void)x, (void)y, (void)pw, (void)ph, (void)subpixel,
      (void)make, (void)model, (void)transform;
}
static void on_mode(void *d, wl_proxy *o, uint32_t flags, int32_t width,
                    int32_t height, int32_t refresh) {
  (void)d, (void)o;
  if ((flags & 1) == 0) /* current */
    return;
  output_width = width;
  output_height = height;
  if (refresh > 0)
    jf_window_refresh_mhz = (uint32_t)refresh;
}
static void on_output_done(void *d, wl_proxy *o) { (void)d, (void)o; }
static void on_scale(void *d, wl_proxy *o, int32_t factor) {
  (void)d, (void)o, (void)factor;
}
static listener_fn output_listener[] = {
    (listener_fn)on_geometry, (listener_fn)on_mode, (listener_fn)on_output_done,
    (listener_fn)on_scale};

static void on_global(void *data, wl_proxy *r, uint32_t name,
                      const char *interface, uint32_t version) {
  (void)data;
  const struct wl_interface *want = NULL;
  uint32_t at = 1;
  if (strcmp(interface, "wl_compositor") == 0 && compositor == NULL)
    want = wl.compositor, at = version < 3 ? version : 3;
  else if (strcmp(interface, "wl_webos_shell") == 0 && shell == NULL)
    want = wl.shell;
  else if (strcmp(interface, "wl_output") == 0 && output == NULL)
    want = wl.output, at = version < 2 ? version : 2;
  else if (strcmp(interface, "wl_seat") == 0 && seat_count < SEATS)
    want = wl.seat, at = version < 4 ? version : 4;
  else if (strcmp(interface, "wl_webos_input_manager") == 0 &&
           wl.input_manager != NULL && input_manager == NULL &&
           (size_t)wl.input_manager->event_count <=
               sizeof(input_manager_listener) /
                   sizeof(input_manager_listener[0]))
    want = wl.input_manager;
  if (want == NULL)
    return;
  /* wl_registry.bind is "usun": the name, then the interface's name and
   * version for the new id. */
  wl_proxy *bound = send_request(r, wl.registry, "bind", want, at, name,
                                 (uintptr_t)want->name, at, 0);
  if (bound == NULL)
    return;
  if (want == wl.compositor) {
    compositor = bound;
  } else if (want == wl.shell) {
    shell = bound;
  } else if (want == wl.output) {
    output = bound;
    wl.add_listener(bound, output_listener, NULL);
  } else if (want == wl.input_manager) {
    input_manager = bound;
    for (int i = 0; i < want->event_count; i++) {
      const bool visibility =
          strcmp(want->events[i].name, "cursor_visibility") == 0;
      input_manager_listener[i] = visibility ? (listener_fn)on_cursor_visibility
                                             : (listener_fn)ignore_event;
    }
    wl.add_listener(bound, input_manager_listener, NULL);
  } else {
    seats[seat_count].seat = bound;
    wl.add_listener(bound, seat_listener, (void *)(uintptr_t)seat_count);
    seat_count++;
  }
}
static void on_global_remove(void *d, wl_proxy *r, uint32_t name) {
  (void)d, (void)r, (void)name;
}
static listener_fn registry_listener[] = {(listener_fn)on_global,
                                          (listener_fn)on_global_remove};

static void on_state_changed(void *d, wl_proxy *s, uint32_t state) {
  (void)d, (void)s;
  if (jf_window_log_events)
    fprintf(stderr, "wayland: state %u\n", state);
  jf_window_drawable = state != STATE_MINIMIZED;
  if (jf_window_drawable)
    jf_window_frame_requested = true;
  if (!jf_window_drawable)
    repeat_key = 0;
}
static void on_position(void *d, wl_proxy *s, int32_t x, int32_t y) {
  (void)d, (void)s, (void)x, (void)y;
}
static void on_close(void *d, wl_proxy *s) {
  (void)d, (void)s;
  jf_window_running = false;
  emit(&(jf_event){.kind = JF_EVENT_CLOSE});
}
static void on_exposed(void *d, wl_proxy *s, void *rectangles) {
  (void)d, (void)s, (void)rectangles;
  jf_window_frame_requested = true;
}
static void on_about_to_change(void *d, wl_proxy *s, uint32_t state) {
  (void)d, (void)s, (void)state;
}
static listener_fn shell_surface_listener[] = {
    (listener_fn)on_state_changed, (listener_fn)on_position,
    (listener_fn)on_close, (listener_fn)on_exposed,
    (listener_fn)on_about_to_change};

/* ------------------------------------------------------------------ init */

/* The EGL surface over the Wayland surface, current: the first buffer swapped
 * into it maps the window. */
static bool map_surface(void) {
  egl_window = wl.egl_window_create(surface, (int)gl_width, (int)gl_height);
  egl_surface =
      egl_window != NULL
          ? eglCreateWindowSurface(egl_display, egl_config,
                                   (EGLNativeWindowType)egl_window, NULL)
          : EGL_NO_SURFACE;
  return egl_surface != EGL_NO_SURFACE &&
         eglMakeCurrent(egl_display, egl_surface, egl_surface, egl_context);
}

/* No buffer, so nothing to show: the ordinary Wayland way of hiding, which
 * hands the remote to what is beneath. The context stays, and with it every
 * texture. The EGL surface goes first, or a swap could wait for a buffer the
 * compositor will never release. */
static void unmap_surface(void) {
  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context);
  if (egl_surface != EGL_NO_SURFACE)
    eglDestroySurface(egl_display, egl_surface);
  egl_surface = EGL_NO_SURFACE;
  if (egl_window != NULL)
    wl.egl_window_destroy(egl_window);
  egl_window = NULL;
  send_request(surface, wl.surface, "attach", NULL, 0, 0, 0, 0, 0);
  send_request(surface, wl.surface, "commit", NULL, 0, 0, 0, 0, 0);
}

static bool init_egl(void) {
  egl_display = eglGetDisplay((EGLNativeDisplayType)display);
  EGLint major = 0, minor = 0;
  if (egl_display == EGL_NO_DISPLAY ||
      !eglInitialize(egl_display, &major, &minor)) {
    fprintf(stderr, "wayland: EGL did not initialise\n");
    return false;
  }
  eglBindAPI(EGL_OPENGL_ES_API);
  /* Alpha, because the TV's video plane - and, for an overlay, the app
   * beneath - shows through wherever the UI writes 0. */
  static const EGLint config_attributes[] = {EGL_SURFACE_TYPE,
                                             EGL_WINDOW_BIT,
                                             EGL_RED_SIZE,
                                             8,
                                             EGL_GREEN_SIZE,
                                             8,
                                             EGL_BLUE_SIZE,
                                             8,
                                             EGL_ALPHA_SIZE,
                                             8,
                                             EGL_DEPTH_SIZE,
                                             16,
                                             EGL_RENDERABLE_TYPE,
                                             EGL_OPENGL_ES3_BIT_KHR,
                                             EGL_NONE};
  EGLConfig config;
  EGLint count = 0;
  if (!eglChooseConfig(egl_display, config_attributes, &egl_config, 1,
                       &count) ||
      count == 0) {
    fprintf(stderr, "wayland: no EGL config\n");
    return false;
  }
  static const EGLint context_attributes[] = {EGL_CONTEXT_MAJOR_VERSION_KHR, 3,
                                              EGL_CONTEXT_MINOR_VERSION_KHR, 0,
                                              EGL_NONE};
  config = egl_config;
  egl_context =
      eglCreateContext(egl_display, config, EGL_NO_CONTEXT, context_attributes);
  if (egl_context == EGL_NO_CONTEXT || !map_surface()) {
    fprintf(stderr, "wayland: EGL context %p surface %p\n", egl_context,
            egl_surface);
    return false;
  }
  eglSwapInterval(egl_display, 1);
  fprintf(stderr, "wayland: EGL %d.%d, %s, window %ux%u at %u mHz, type %s\n",
          major, minor, (const char *)glGetString(GL_VERSION), gl_width,
          gl_height, jf_window_refresh_mhz,
          window_type[0] != '\0' ? window_type : "(the compositor's)");
  return true;
}

bool jf_window_init(const char *app_id, const char *title, uint32_t width,
                    uint32_t height) {
  if (!load())
    return false;
  if (pipe(wake_pipe) != 0)
    return false;
  fcntl(wake_pipe[0], F_SETFL, O_NONBLOCK);
  fcntl(wake_pipe[1], F_SETFL, O_NONBLOCK);
  display = wl.connect(NULL);
  if (display == NULL) {
    fprintf(stderr, "wayland: no compositor\n");
    return false;
  }
  registry = send_request(display, wl.display, "get_registry", wl.registry, 0,
                          0, 0, 0, 0);
  if (registry == NULL)
    return false;
  wl.add_listener(registry, registry_listener, NULL);
  /* The globals, then what binding them sent: the output's mode and the
   * seats' capabilities. */
  wl.roundtrip(display);
  wl.roundtrip(display);
  if (compositor == NULL || shell == NULL) {
    fprintf(stderr, "wayland: compositor %p, webOS shell %p\n",
            (void *)compositor, (void *)shell);
    return false;
  }
  gl_width = output_width > 0 ? (uint32_t)output_width
             : width > 0      ? width
                              : 1920;
  gl_height = output_height > 0 ? (uint32_t)output_height
              : height > 0      ? height
                                : 1080;

  surface = send_request(compositor, wl.compositor, "create_surface",
                         wl.surface, 0, 0, 0, 0, 0);
  /* "no": the new id first, then the surface. */
  shell_surface =
      send_request(shell, wl.shell, "get_shell_surface", wl.shell_surface, 0, 0,
                   (uintptr_t)surface, 0, 0);
  if (surface == NULL || shell_surface == NULL)
    return false;
  wl.add_listener(shell_surface, shell_surface_listener, NULL);
  /* All before the first frame maps the surface: the compositor places it by
   * what it says then. LSM composites only a surface it can put to an app. */
  set_property("appId", app_id);
  set_property("title", title);
  if (window_type[0] != '\0')
    set_property("_WEBOS_WINDOW_TYPE", window_type);
  set_property("_WEBOS_ACCESS_POLICY_KEYS_BACK", "true"); /* no exit dialog */
  set_state(STATE_FULLSCREEN);
  return init_egl();
}

void jf_window_deinit(void) {
  if (egl_display != EGL_NO_DISPLAY) {
    eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (egl_surface != EGL_NO_SURFACE)
      eglDestroySurface(egl_display, egl_surface);
    if (egl_context != EGL_NO_CONTEXT)
      eglDestroyContext(egl_display, egl_context);
    eglTerminate(egl_display);
  }
  if (egl_window != NULL)
    wl.egl_window_destroy(egl_window);
  if (display != NULL)
    wl.disconnect(display);
  display = NULL;
  for (int i = 0; i < 2; i++)
    if (wake_pipe[i] >= 0)
      close(wake_pipe[i]);
  wake_pipe[0] = wake_pipe[1] = -1;
}

void jf_window_swap(void) {
  if (egl_surface != EGL_NO_SURFACE)
    eglSwapBuffers(egl_display, egl_surface);
}

/* ------------------------------------------------------------- the pump */

static void poke(void) {
  const char byte = 1;
  ssize_t ignored = write(wake_pipe[1], &byte, 1);
  (void)ignored;
}

/* Not wl_webos_shell_surface's minimized state: the compositor never brings
 * an overlay back from it, whatever the app asks (webOS 9.2.2), so a launcher
 * minimised that way cannot be opened again. The surface is unmapped
 * instead, and the next raise maps it again. */
void jf_window_minimize(void) {
  if (hidden || display == NULL)
    return;
  if (jf_window_log_events)
    fprintf(stderr, "wayland: hide\n");
  unmap_surface();
  wl.flush(display);
  hidden = true;
  jf_window_drawable = false;
  repeat_key = 0;
}

bool jf_window_focused(void) {
  if (hidden)
    return false;
  for (size_t i = 0; i < seat_count; i++)
    if (seats[i].focused)
      return true;
  return false;
}

void jf_window_post_quit(void) {
  atomic_store(&quit_asked, true);
  poke();
}
void jf_window_post_raise(void) {
  atomic_store(&raise_asked, true);
  poke();
}
void jf_window_wake(void) { poke(); }

/* What other threads asked for, now on the main thread. */
static void carry_out(void) {
  char drain[64];
  while (read(wake_pipe[0], drain, sizeof(drain)) > 0)
    continue;
  if (atomic_exchange(&raise_asked, false)) {
    if (jf_window_log_events)
      fprintf(stderr, "wayland: raise\n");
    /* Mapped afresh unless it is up and has the remote: the compositor hides
     * an overlay itself when an app comes up over it - an input chosen in
     * LG's input picker - and tells it nothing (SAM only sends a pause), and
     * a window it hid that way comes back only as one hidden here does. Up
     * and focused, it is in front already, and a remap would only flicker. */
    if (!jf_window_focused()) {
      if (!hidden)
        unmap_surface();
      if (!map_surface())
        fprintf(stderr, "wayland: the window could not be mapped again\n");
      hidden = false;
      set_state(STATE_FULLSCREEN);
    }
    jf_window_drawable = true;
    jf_window_frame_requested = true;
  }
  if (atomic_exchange(&quit_asked, false)) {
    jf_window_running = false;
    emit(&(jf_event){.kind = JF_EVENT_CLOSE});
  }
}

/* Reads and dispatches whatever arrives within `timeout` milliseconds (-1 for
 * as long as it takes), stopping early for another thread or a key repeat. */
static void pump(int timeout) {
  if (display == NULL)
    return;
  while (wl.prepare_read(display) != 0)
    wl.dispatch_pending(display);
  wl.flush(display);
  const int repeat = repeat_wait();
  if (repeat >= 0 && (timeout < 0 || repeat < timeout))
    timeout = repeat;
  struct pollfd fds[2] = {{.fd = wl.get_fd(display), .events = POLLIN},
                          {.fd = wake_pipe[0], .events = POLLIN}};
  const int ready = poll(fds, 2, timeout);
  if (ready > 0 && (fds[0].revents & POLLIN) != 0)
    wl.read_events(display);
  else
    wl.cancel_read(display);
  if (wl.dispatch_pending(display) < 0) {
    fprintf(stderr, "wayland: the connection is gone (%s)\n", strerror(errno));
    jf_window_running = false;
    emit(&(jf_event){.kind = JF_EVENT_CLOSE});
    return;
  }
  repeat_keys();
  carry_out();
}

bool jf_window_poll(void) {
  pump(0);
  return jf_window_running;
}

bool jf_window_wait(void) {
  pump(-1);
  return jf_window_running;
}

bool jf_window_wait_timeout(int milliseconds) {
  pump(milliseconds);
  return jf_window_running;
}

/* ----------------------------------------- what only window.c has yet */

bool jf_window_begin_text_input(const int rect[4], jf_text_purpose purpose) {
  (void)rect, (void)purpose;
  return false;
}
void jf_window_end_text_input(void) {}

const char *jf_window_export_video(const int src[4], const int dst[4]) {
  (void)src, (void)dst;
  return NULL;
}
