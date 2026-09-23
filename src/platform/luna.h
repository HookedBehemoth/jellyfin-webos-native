/*
 * The webOS application lifecycle, over the Luna bus.
 *
 * SAM expects a native app to register and then answer what it is told: `relaunch` when
 * the user opens an app that is already running, `close` when the system wants it gone -
 * switching apps, reclaiming memory, powering off. An app SAM has no handle on is
 * signalled instead, which is why this has to work before SDL's signal handling can be
 * left alone.
 *
 * The call goes through libhelpers, the library webOS's own native apps use, because it
 * owns the LS2 handle. What it does *not* own is a main loop: it attaches the subscription
 * to GLib's default context and expects the app to be iterating one. This app never
 * touches GLib otherwise, so it runs a loop of its own on a thread here - without it the
 * registration succeeds and not one callback ever arrives.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* Both callbacks run on the GLib thread, so they must be things a foreign thread may do -
 * jf_window_post_quit and jf_window_post_raise are, because SDL_PushEvent is thread-safe.
 *
 * False on anything that is not a webOS device, which is not an error there: nothing is
 * asking the app to close. */
bool jf_luna_register_lifecycle(void (*on_quit)(void), void (*on_relaunch)(void));
void jf_luna_deinit(void);

/* Print every lifecycle payload. */
extern bool jf_luna_log;

/* The value of a top-level string key, copied into `out`. Exposed for its test: a
 * lifecycle payload arrives as a flat C string and one field of it is not worth a JSON
 * parser. */
bool jf_luna_json_string(const char *payload, const char *key, char *out, size_t out_len);
