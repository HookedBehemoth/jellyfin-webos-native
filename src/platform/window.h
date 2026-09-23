/*
 * The platform layer: window, GL context, input and the webOS on-screen keyboard, all
 * through SDL2.
 *
 * SDL is what makes one binary work across webOS releases: talking Wayland directly means
 * binding wl_proxy_marshal_flags, which libwayland-client only grew in 1.20, and the older
 * TVs ship 0.3.0. SDL also carries the webOS-specific pieces - the exported video window,
 * the Back-key access policy and the on-screen keyboard.
 *
 * Events reach the app as raw evdev keycodes; the keymap in window.c is the whole
 * translation from SDL's scancodes.
 *
 * The TV has SDL 2.0.14, so nothing newer than that API may be used.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* 24.8 fixed point, as the Wayland shim this replaced reported pointer positions. */
typedef int32_t jf_fixed;
static inline int jf_fixed_to_int(jf_fixed value) { return value >> 8; }

typedef enum {
    JF_EVENT_KEY,
    JF_EVENT_POINTER_ENTER,
    JF_EVENT_POINTER_LEAVE,
    JF_EVENT_POINTER_MOTION,
    JF_EVENT_POINTER_BUTTON,
    JF_EVENT_POINTER_AXIS,
    JF_EVENT_TEXT_COMMIT,
    JF_EVENT_RESIZED,
    JF_EVENT_CLOSE,
} jf_event_kind;

typedef struct {
    jf_event_kind kind;
    union {
      struct {
        uint32_t code;
        bool pressed, repeat;
      } key; /* raw evdev, no XKB +8 */
        struct { jf_fixed x, y; } pointer;
        struct { uint32_t button; bool pressed; } button;
        struct { uint32_t axis; jf_fixed value; } axis;
        const char *text;                                   /* valid for the call only */
        struct { uint32_t width, height; } resized;
    };
} jf_event;

typedef void (*jf_event_fn)(const jf_event *event);

bool jf_window_init(const char *app_id, const char *title, uint32_t width, uint32_t height);
void jf_window_deinit(void);
void jf_window_set_handler(jf_event_fn handler);

/* Drain SDL's queue into the handler. False once the app should stop. */
bool jf_window_poll(void);
/* Block until SDL delivers one event. A negative timeout waits indefinitely; zero only
 * checks pending events. */
bool jf_window_wait(void);
bool jf_window_wait_timeout(int milliseconds);

void jf_window_swap(void);

/* Safe from any thread: SDL_PushEvent is the one part of SDL that is. */
void jf_window_post_quit(void);
/* webOS hands a minimised app a `relaunch` and expects it to raise itself; one that
 * ignores it stays in the background and cannot be reopened at all. */
void jf_window_post_raise(void);
/* Wake the event loop without input, window activation or forced rendering. Worker
 * completions coalesce until the main thread consumes the empty event. */
void jf_window_wake(void);

typedef enum { JF_TEXT_NORMAL = 0, JF_TEXT_URL = 5, JF_TEXT_PASSWORD = 8 } jf_text_purpose;
bool jf_window_begin_text_input(const int rect[4], jf_text_purpose purpose);
void jf_window_end_text_input(void);

/* A webOS window id for the hardware video plane, positioned by `src` and `dst` as
 * {x, y, w, h}. This is the whole reason the video never passes through this process.
 * The returned string stays valid until the window is destroyed. NULL off the TV. */
const char *jf_window_export_video(const int src[4], const int dst[4]);

/* LG's keycodes map IR_KEY_BACK to XKB 420, i.e. Wayland key 412. On desktops 412 is
 * KEY_PREVIOUS, so only treat it as Back on the TV. */
bool jf_window_is_back_key(uint32_t code);

extern bool jf_window_running;
extern bool jf_window_log_keys;
extern bool jf_window_log_events;
/* False while SDL reports the window minimized. Render loops wait for an event in this
 * state instead of swapping invisible frames in a tight loop. */
extern bool jf_window_drawable;
/* Main-thread invalidation. Worker wakeups alone do not request a redraw. */
extern bool jf_window_frame_requested;
/* True on the TV, where SDL drives its own webOS video backend. */
extern bool jf_window_on_webos;
/* Display refresh rate in millihertz; a few desktop drivers omit it, hence the fallback. */
extern uint32_t jf_window_refresh_mhz;
