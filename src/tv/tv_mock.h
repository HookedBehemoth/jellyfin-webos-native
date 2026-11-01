/*
 * A TV in the process, behind transport_mock.c: what the desktop build shows,
 * and what the launcher's tests run against. It answers as the TV's services
 * do, in their payloads, so tv.c parses what it would on the TV.
 *
 * Home Chooser is there, ready, with this launcher as its choice. Seven apps,
 * among them a hidden one, an input app, LG's Home and this launcher, none of
 * which the launcher shows; of the three apps, Netflix and YouTube can be
 * removed and name a tile colour. Netflix's icons can be read from its folder;
 * the others' only through Home Chooser, where YouTube's large one is not there
 * but its small one is, and the browser's is not there. Four HDMI ports - a
 * console over CEC on 1, nothing plugged into 2, an unnamed device on 3, and 4
 * from firmware that leaves hdmiPlugIn out.
 */
#pragma once

#include <stdbool.h>

#define TV_MOCK_APPS 3

/* Back to the TV above, with nothing counted. */
void tv_mock_reset(void);
/* Calls of `method` ("launch", "uninstallApp", ...) since reset. */
unsigned tv_mock_calls(const char *method);
/* The payload the last call of `method` sent; "" for none. */
const char *tv_mock_last(const char *method);
/* The next `count` calls of `method` are refused, as the TV refuses a call it
 * will not do: returnValue false, with an errorText. */
void tv_mock_refuse(const char *method, unsigned count);
/* What the next refusals say; NULL for the mock's own words. */
void tv_mock_refuse_text(const char *text);
/* The app in front, beneath the launcher; "" (as after reset) for none. */
void tv_mock_foreground(const char *id);
/* Another app installed, at the end of the apps; subscribers hear. */
void tv_mock_install(const char *id, const char *title);
/* What Home Chooser's getInfo says: root, permitted, and whether this launcher
 * is the selected Home. All true after reset. */
void tv_mock_home_chooser(bool root, bool permitted, bool chosen);
/* Whether listApps takes a subscription, as a Home Chooser without it does not.
 * True after reset. */
void tv_mock_subscribable(bool can);
/* Home Chooser going away: subscribers get the failed answer the bus sends. */
void tv_mock_lose_subscribers(void);
