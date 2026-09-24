#include "window.h"

#include <SDL.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gl.h"

#ifdef JF_WEBOS
#include <SDL_webOS.h>
#endif

uint32_t gl_width;
uint32_t gl_height;

bool jf_window_log_keys;
bool jf_window_log_events;
bool jf_window_running = true;
bool jf_window_drawable = true;
bool jf_window_frame_requested = true;
bool jf_window_on_webos;
uint32_t jf_window_refresh_mhz = 60000;

static SDL_Window *window;
static SDL_GLContext context;
static jf_event_fn handler;
static atomic_bool wake_pending;
/* Window units to framebuffer pixels; 1.0 unless SDL and GL disagree. */
static float pointer_scale_x = 1.0f;
static float pointer_scale_y = 1.0f;
/* Frames still owed to a size change; see jf_window_swap. */
static int resize_redraws;
static char video_window_id[64];

/* Our own event, so a foreign thread can ask the main thread to do something only the
 * main thread may do. */
#define EV_RAISE (SDL_USEREVENT)
#define EV_WAKE (SDL_USEREVENT + 1)

static void emit(const jf_event *event)
{
    if (handler != NULL)
        handler(event);
}

void jf_window_set_handler(jf_event_fn value) { handler = value; }

static jf_fixed to_fixed(int value) { return value << 8; }
static jf_fixed pointer_x(int value) { return to_fixed((int)((float)value * pointer_scale_x)); }
static jf_fixed pointer_y(int value) { return to_fixed((int)((float)value * pointer_scale_y)); }

bool jf_window_is_back_key(uint32_t code)
{
    return code == 1 || code == 158 || (jf_window_on_webos && code == 412);
}

/* ---------------------------------------------------------------- key map
 *
 * SDL reports USB-HID scancodes; the app speaks evdev. Only the keys it acts on are here -
 * letters and digits are deliberately absent, because their text arrives as
 * SDL_TEXTINPUT instead, already shifted, capsed and in the user's own layout. */
static const struct {
  int scancode;
  uint32_t evdev;
} keymap[] = {
    {40, 28}, /* Return */
    {41, 1},  /* Escape */
    {42, 14}, /* Backspace */
    {43, 15}, /* Tab */
    {44, 57}, /* Space */
    {58, 59}, /* F1 */
    {59, 60}, /* F2 */
    {60, 61}, /* F3 */
    {61, 62}, /* F4 */
    {66, 67}, /* F9  - sign out */
    {67,
     370}, /* F10 - KEY_SUBTITLE, which the webOS keymap has no scancode for */
    {69, 88},   /* F12 - screenshot */
    {79, 106},  /* Right */
    {80, 105},  /* Left */
    {81, 108},  /* Down */
    {82, 103},  /* Up */
    {88, 96},   /* Keypad Enter */
    {270, 158}, /* AC_BACK */
    /* The webOS remote, from SDL's own scancode block. The colour buttons are
     * the TV's only spare inputs, so Blue keeps doing what F9 does. */
    {482, 158}, /* WEBOS_BACK */
    {486, 64},  /* WEBOS_RED    -> F6 */
    {487, 65},  /* WEBOS_GREEN  -> F7 */
    {488, 370}, /* WEBOS_YELLOW -> KEY_SUBTITLE */
    {489, 67},  /* WEBOS_BLUE   -> F9 */
};
/* Sent as keys by the webOS backend when the magic remote's pointer appears and
 * disappears. */
#define SCANCODE_CURSOR_SHOW 484
#define SCANCODE_CURSOR_HIDE 485

static bool evdev_for(int scancode, uint32_t *out)
{
    for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        if (keymap[i].scancode == scancode) {
            *out = keymap[i].evdev;
            return true;
        }
    }
    return false;
}

static bool cursor_visible = true;

static void hide_cursor(void)
{
    if (!cursor_visible)
        return;
#ifdef JF_WEBOS
    SDL_webOSCursorVisibility(SDL_FALSE);
#endif
    cursor_visible = false;
}

/* ------------------------------------------------------------------- init */

bool jf_window_init(const char *app_id, const char *title, uint32_t want_width,
                    uint32_t want_height)
{
    /* The webOS backend registers the app on the Luna bus as part of deciding whether it
     * is available at all, and it takes the id from the environment rather than from a
     * hint. Without it the registration fails with "Invalid appId specified", SDL reports
     * the backend as unavailable and falls back to plain wayland - where the remote has no
     * keymap and every button arrives as scancode 1. Whatever SAM set wins. */
    SDL_setenv("APPID", app_id, 0);
    SDL_SetHint("SDL_WEBOS_REGISTER_APP", "true");
    SDL_SetHint("SDL_WEBOS_ACCESS_POLICY_KEYS_BACK", "true"); /* no exit dialog */
    /* How long the magic-remote pointer stays on screen once it stops moving. SAM starts
     * an app with SDL_MRCU_TIMER=300000 in its environment - five minutes, which is never
     * - and this hint is what sets that timer. */
    SDL_SetHint("SDL_WEBOS_CURSOR_SLEEP_TIME", "1000");
#ifdef JF_WEBOS
    jf_window_on_webos = true;
#endif

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
#ifdef _WIN32
    /* GLES here is ANGLE, which gives exactly the version asked for, and the UI shaders
     * need 3.1. Without the hint SDL returns a desktop WGL context instead, whose entry
     * points are not the ones this program links. */
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#else
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    /* The TV's video plane shows through wherever the UI writes alpha 0. */
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);

    /* The TV is always fullscreen and managed by LSM; a desktop gets a normal resizable
     * window. Fullscreen-desktop resizes the window but not the GL surface underneath it,
     * so a window asked for at some convenient default keeps that surface for its whole
     * life while SDL reports the display's size - a viewport 1.5x too large and pointer
     * coordinates in a space of their own. Ask for the display's size up front instead. */
    SDL_DisplayMode mode;
    const bool display_ok = SDL_GetCurrentDisplayMode(0, &mode) == 0 && mode.w > 0 && mode.h > 0;
    if (display_ok && mode.refresh_rate > 0)
        jf_window_refresh_mhz = (uint32_t)mode.refresh_rate * 1000;
    const bool panel_size = jf_window_on_webos && display_ok;
    const int width = panel_size ? mode.w : 1280;
    const int height = panel_size ? mode.h : 720;

    const Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN |
                         (jf_window_on_webos ? SDL_WINDOW_FULLSCREEN_DESKTOP
                                             : SDL_WINDOW_RESIZABLE);
    window = SDL_CreateWindow(title, SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                              width, height, flags);
    if (window == NULL) {
        fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    context = SDL_GL_CreateContext(window);
    if (context == NULL) {
        fprintf(stderr, "SDL_GL_CreateContext: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_SetSwapInterval(1);

    /* Everything above the platform layer works in framebuffer pixels, so the drawable
     * size is the one that matters - the window size is in logical units and the two
     * differ wherever the compositor applies a scale. */
    int logical_w = 0, logical_h = 0, pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    SDL_GL_GetDrawableSize(window, &pixel_w, &pixel_h);
    gl_width = (uint32_t)(pixel_w > 0 ? pixel_w : width);
    gl_height = (uint32_t)(pixel_h > 0 ? pixel_h : height);
    /* ...and if SDL still disagrees with GL, GL wins: it is the buffer the pixels land
     * in. Pointer coordinates arrive in window space, so remember the ratio. */
    GLint viewport[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] > 0 && viewport[3] > 0) {
        gl_width = (uint32_t)viewport[2];
        gl_height = (uint32_t)viewport[3];
    }
    pointer_scale_x = logical_w > 0 ? (float)gl_width / (float)logical_w : 1.0f;
    pointer_scale_y = logical_h > 0 ? (float)gl_height / (float)logical_h : 1.0f;

    int panel_w = 0, panel_h = 0;
#ifdef JF_WEBOS
    SDL_webOSGetPanelResolution(&panel_w, &panel_h);
#endif
    const char *driver = SDL_GetCurrentVideoDriver();
    fprintf(stderr,
            "SDL video driver=%s window=%dx%d drawable=%dx%d gl_viewport=%ux%u panel=%dx%d\n",
            driver != NULL ? driver : "?", logical_w, logical_h, pixel_w, pixel_h,
            gl_width, gl_height, panel_w, panel_h);
    return true;
}

void jf_window_deinit(void)
{
    if (context != NULL)
        SDL_GL_DeleteContext(context);
    if (window != NULL)
        SDL_DestroyWindow(window);
    context = NULL;
    window = NULL;
    SDL_Quit();
}

void jf_window_swap(void)
{
    if (window == NULL)
        return;
    SDL_GL_SwapWindow(window);
    /* ANGLE resizes its window surface on the swap, not when the window changes, so the
     * first frame after a resize lands in a buffer of the old size and stays there until
     * something asks for another. Only ever non-zero on a desktop. */
    if (resize_redraws > 0) {
        resize_redraws--;
        jf_window_frame_requested = true;
    }
}

/* ------------------------------------------------------------- event pump */

void jf_window_post_quit(void)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = SDL_QUIT;
    SDL_PushEvent(&event);
}

void jf_window_post_raise(void)
{
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = EV_RAISE;
    SDL_PushEvent(&event);
}

void jf_window_wake(void)
{
    if (atomic_exchange(&wake_pending, true))
        return;
    SDL_Event event;
    memset(&event, 0, sizeof(event));
    event.type = EV_WAKE;
    if (SDL_PushEvent(&event) <= 0)
        atomic_store(&wake_pending, false);
}

/* Called by anything that can change the window's size, before it asks for a frame. */
static void refresh_drawable_size(void)
{
    if (window == NULL)
        return;
    resize_redraws = 2; /* see jf_window_swap */
    int pixel_w = 0, pixel_h = 0;
    int logical_w = 0, logical_h = 0;
    SDL_GL_GetDrawableSize(window, &pixel_w, &pixel_h);
    SDL_GetWindowSize(window, &logical_w, &logical_h);
    if (pixel_w <= 0 || pixel_h <= 0)
        return;
    gl_width = (uint32_t)pixel_w;
    gl_height = (uint32_t)pixel_h;
    /* Pointer coordinates are scaled by this ratio, and a resize - or a move to a display
     * of a different scale - changes it. Stale, and the cursor drifts. */
    if (logical_w > 0)
        pointer_scale_x = (float)gl_width / (float)logical_w;
    if (logical_h > 0)
        pointer_scale_y = (float)gl_height / (float)logical_h;
}

static void translate(const SDL_Event *event)
{
    jf_event out;
    switch (event->type) {
    case SDL_QUIT:
        jf_window_running = false;
        out.kind = JF_EVENT_CLOSE;
        emit(&out);
        break;

    /* Only the main thread may touch the window, so post_raise comes back through the
     * queue to be acted on here. */
    case EV_RAISE:
        if (window != NULL)
            SDL_RaiseWindow(window);
        jf_window_frame_requested = true;
        break;

    case EV_WAKE:
        atomic_store(&wake_pending, false);
        break;

    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const bool down = event->type == SDL_KEYDOWN;
        const int scancode = (int)event->key.keysym.scancode;
        if (scancode == SCANCODE_CURSOR_HIDE) {
            cursor_visible = false;
            out.kind = JF_EVENT_POINTER_LEAVE;
            emit(&out);
            break;
        }
        if (scancode == SCANCODE_CURSOR_SHOW) {
            cursor_visible = true;
            break;
        }
        uint32_t code = 0;
        const bool known = evdev_for(scancode, &code);
        /* Logging every key is the only way to learn what a TV remote actually
         * sends. */
        if (!known || jf_window_log_keys)
          fprintf(stderr, "sdl: key scancode=%d sym=0x%x down=%d repeat=%d\n",
                  scancode, (unsigned)event->key.keysym.sym, down,
                  event->key.repeat);
        if (!known)
            break;
        if (down)
            hide_cursor();
        /* Auto-repeat drives held-down navigation, so it is not filtered. */
        out.kind = JF_EVENT_KEY;
        out.key.code = code;
        out.key.pressed = down;
        out.key.repeat = down && event->key.repeat != 0;
        emit(&out);
        break;
    }

    case SDL_TEXTINPUT:
        out.kind = JF_EVENT_TEXT_COMMIT;
        out.text = event->text.text;
        emit(&out);
        break;

    case SDL_MOUSEMOTION:
        cursor_visible = true;
        out.kind = JF_EVENT_POINTER_MOTION;
        out.pointer.x = pointer_x(event->motion.x);
        out.pointer.y = pointer_y(event->motion.y);
        emit(&out);
        break;

    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        /* SDL numbers buttons from 1; the app speaks evdev BTN_LEFT. */
        if (event->button.button != SDL_BUTTON_LEFT)
            break;
        out.kind = JF_EVENT_POINTER_BUTTON;
        out.button.button = 0x110;
        out.button.pressed = event->type == SDL_MOUSEBUTTONDOWN;
        emit(&out);
        break;

    case SDL_MOUSEWHEEL:
        /* SDL counts notches and points up; Wayland reports a length and points down.
         * Ten surface units per notch is what the compositors this was written against
         * send, so the app's feel is unchanged. */
        if (event->wheel.y == 0)
            break;
        out.kind = JF_EVENT_POINTER_AXIS;
        out.axis.axis = 0;
        out.axis.value = to_fixed(-event->wheel.y * 10);
        emit(&out);
        break;

    case SDL_WINDOWEVENT:
        /* Which events carry a size, and whether it has landed by the time one arrives,
         * differs between backends - the first thing to reach for when a window is drawn
         * at the wrong dimensions. */
        if (jf_window_log_events) {
          int drawable_w = 0, drawable_h = 0, logical_w = 0, logical_h = 0;
          if (window != NULL) {
            SDL_GL_GetDrawableSize(window, &drawable_w, &drawable_h);
            SDL_GetWindowSize(window, &logical_w, &logical_h);
          }
          fprintf(stderr,
                  "winevent=%u data=%dx%d drawable=%dx%d windowsize=%dx%d "
                  "gl=%ux%u\n",
                  event->window.event, event->window.data1, event->window.data2,
                  drawable_w, drawable_h, logical_w, logical_h, gl_width,
                  gl_height);
        }
        switch (event->window.event) {
        case SDL_WINDOWEVENT_HIDDEN:
        case SDL_WINDOWEVENT_MINIMIZED:
            jf_window_drawable = false;
            break;
        case SDL_WINDOWEVENT_SHOWN:
        case SDL_WINDOWEVENT_EXPOSED:
        case SDL_WINDOWEVENT_MAXIMIZED:
        case SDL_WINDOWEVENT_RESTORED:
            /* Maximising changes the drawable without always sending RESIZED first, so
             * the size is taken here too or the frame is drawn from the old one. */
            refresh_drawable_size();
            jf_window_drawable = true;
            jf_window_frame_requested = true;
            break;
        case SDL_WINDOWEVENT_RESIZED:
        case SDL_WINDOWEVENT_SIZE_CHANGED: {
            refresh_drawable_size();
            out.kind = JF_EVENT_RESIZED;
            out.resized.width = gl_width;
            out.resized.height = gl_height;
            emit(&out);
            break;
        }
        case SDL_WINDOWEVENT_LEAVE:
            out.kind = JF_EVENT_POINTER_LEAVE;
            emit(&out);
            break;
        case SDL_WINDOWEVENT_CLOSE:
            jf_window_running = false;
            out.kind = JF_EVENT_CLOSE;
            emit(&out);
            break;
        default:
            break;
        }
        break;

    default:
        break;
    }
}

bool jf_window_poll(void)
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
        translate(&event);
    return jf_window_running;
}

bool jf_window_wait(void)
{
    SDL_Event event;
    if (SDL_WaitEvent(&event))
        translate(&event);
    return jf_window_running;
}

bool jf_window_wait_timeout(int milliseconds)
{
    SDL_Event event;
    if (milliseconds < 0)
        return jf_window_wait();
    if (SDL_WaitEventTimeout(&event, milliseconds))
        translate(&event);
    return jf_window_running;
}

/* -------------------------------------------------------------- text input */

/* Show the TV's on-screen keyboard, or just enable SDL_TEXTINPUT on a desktop. SDL 2.0.14
 * has no way to seed the field with existing text, so editing starts from what the app
 * already holds and appends. */
bool jf_window_begin_text_input(const int rect[4], jf_text_purpose purpose)
{
    (void)purpose;
    SDL_Rect box = {rect[0], rect[1], rect[2], rect[3]};
    SDL_SetTextInputRect(&box);
    SDL_StartTextInput();
    return true;
}

void jf_window_end_text_input(void) { SDL_StopTextInput(); }

/* ----------------------------------------------------- exported video plane */

const char *jf_window_export_video(const int src[4], const int dst[4])
{
#ifdef JF_WEBOS
    if (video_window_id[0] == '\0') {
        /* Type 0 is the video plane. The string SDL returns is owned by SDL and only
         * guaranteed until its next call, so it is copied. */
        const char *created = SDL_webOSCreateExportedWindow(SDL_WEBOS_EXPORED_WINDOW_TYPE_VIDEO);
        if (created == NULL) {
            fprintf(stderr, "SDL_webOSCreateExportedWindow: %s\n", SDL_GetError());
            return NULL;
        }
        snprintf(video_window_id, sizeof(video_window_id), "%s", created);
    }
    SDL_Rect src_rect = {src[0], src[1], src[2], src[3]};
    SDL_Rect dst_rect = {dst[0], dst[1], dst[2], dst[3]};
    SDL_webOSSetExportedWindow(video_window_id, &src_rect, &dst_rect);
    return video_window_id;
#else
    (void)src;
    (void)dst;
    (void)video_window_id;
    /* No video plane; the host pipeline (jf/smp_host.c) shows nothing anyway.
     */
    return "";
#endif
}
