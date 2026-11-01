/*
 * A TV in the process, behind transport_mock.c: what the desktop build shows,
 * and what the launcher's tests run against. It answers as the TV's services
 * do, in their payloads, so tv.c parses what it would on the TV.
 *
 * Its TV last showed HDMI 1. Eight launch points, among them a bookmark, a
 * hidden app, an input app, LG's Home and this launcher's development install,
 * none of which the launcher shows; of the three apps, Netflix and YouTube can
 * be removed and name a tile colour. Four HDMI ports - a console over CEC on 1,
 * whose last frame the TV kept, nothing plugged into 2, an unnamed device on 3,
 * and 4 from firmware that leaves hdmiPlugIn out.
 */
#pragma once

#include <stdbool.h>

#define TV_MOCK_APPS 3

/* Back to the TV above, with nothing counted. */
void tv_mock_reset(void);
/* Calls of `method` ("launch", "remove", ...) since reset. */
unsigned tv_mock_calls(const char *method);
/* The payload the last call of `method` sent; "" for none. */
const char *tv_mock_last(const char *method);
/* The next `count` calls of `method` are refused, as the TV refuses a call it
 * will not do: returnValue false, with an errorText. */
void tv_mock_refuse(const char *method, unsigned count);
/* The app in front, beneath the launcher; "" (as after reset) for none. */
void tv_mock_foreground(const char *id);
/* Another app installed, at the end of the launch points. */
void tv_mock_install(const char *id, const char *title);
