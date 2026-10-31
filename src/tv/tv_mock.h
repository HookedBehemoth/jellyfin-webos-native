/*
 * A TV in the process, behind transport_mock.c: what the desktop build shows,
 * and what the launcher's tests run against. It answers as the TV's services
 * do, in their payloads, so tv.c parses what it would on the TV.
 *
 * Its TV: six apps on the launch points, among them a bookmark, a hidden app,
 * an input app and LG's Home, none of which the launcher shows; four HDMI
 * ports - a console over CEC on 1, nothing plugged into 2, an unnamed device
 * on 3, and 4 from firmware that leaves hdmiPlugIn out; the picture mode,
 * energy saving and sound output; and LG's input picker under its older id,
 * Settings and the notification list, but no quick settings and no search.
 */
#pragma once

#include <stdbool.h>

#define TV_MOCK_APPS 3

/* Back to the TV above, with nothing counted. */
void tv_mock_reset(void);
/* Calls of `method` ("launch", "setSystemSettings", ...) since reset. */
unsigned tv_mock_calls(const char *method);
/* The payload the last call of `method` sent; "" for none. */
const char *tv_mock_last(const char *method);
/* The value the TV holds for a setting's key. */
const char *tv_mock_setting(const char *key);
/* The next `count` calls of `method` are refused, as the TV refuses a call it
 * will not do: returnValue false, with an errorText. */
void tv_mock_refuse(const char *method, unsigned count);
/* The settings service under its older name only, as on webOS 4 and earlier;
 * the newer name answers that it does not exist. */
void tv_mock_legacy_settings(bool legacy);
/* Another app installed, at the end of the launch points. */
void tv_mock_install(const char *id, const char *title);
