/* The desktop's transport: the TV of tv_mock.h, answering at once. */
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"
#include "tv_mock.h"

#ifndef JF_SOURCE_DIR
#define JF_SOURCE_DIR "."
#endif
#define ICON JF_SOURCE_DIR "/assets/icon.png"

#define METHODS 24
static struct {
  char method[48];
  unsigned calls, refuse;
  char last[512];
} methods[METHODS];
static size_t method_count;

static char foreground[128];
static char installed[12][2][64];
static size_t installed_count;
static char removed[8][128];
static size_t removed_count;

void tv_mock_reset(void) {
  memset(methods, 0, sizeof(methods));
  method_count = 0;
  installed_count = 0;
  removed_count = 0;
  foreground[0] = '\0';
}

static int method_index(const char *method, bool create) {
  for (size_t i = 0; i < method_count; i++)
    if (strcmp(methods[i].method, method) == 0)
      return (int)i;
  if (!create || method_count == METHODS)
    return -1;
  snprintf(methods[method_count].method, sizeof(methods[0].method), "%s",
           method);
  return (int)method_count++;
}

unsigned tv_mock_calls(const char *method) {
  const int i = method_index(method, false);
  return i < 0 ? 0 : methods[i].calls;
}

const char *tv_mock_last(const char *method) {
  const int i = method_index(method, false);
  return i < 0 ? "" : methods[i].last;
}

void tv_mock_refuse(const char *method, unsigned count) {
  const int i = method_index(method, true);
  if (i >= 0)
    methods[i].refuse = count;
}

void tv_mock_foreground(const char *id) {
  snprintf(foreground, sizeof(foreground), "%s", id);
}

void tv_mock_install(const char *id, const char *title) {
  if (installed_count == sizeof(installed) / sizeof(installed[0]))
    return;
  snprintf(installed[installed_count][0], sizeof(installed[0][0]), "%s", id);
  snprintf(installed[installed_count][1], sizeof(installed[0][1]), "%s", title);
  installed_count++;
}

/* ------------------------------------------------------------ answers */

static bool is_removed(const char *id) {
  for (size_t i = 0; i < removed_count; i++)
    if (strcmp(removed[i], id) == 0)
      return true;
  return false;
}

static const char *launch_points(void) {
  /* Netflix asks for a red tile and can be removed; the browser is LG's. */
  static const char *const points[][2] = {
      {"netflix", "{\"id\":\"netflix\",\"title\":\"Netflix\",\"icon\":\"" ICON
                  "\",\"largeIcon\":\"" ICON "\",\"lptype\":\"default\","
                  "\"iconColor\":\"#e50914\",\"removable\":true}"},
      {"youtube.leanback.v4",
       "{\"id\":\"youtube.leanback.v4\",\"title\":\"YouTube\","
       "\"icon\":\"" ICON "\",\"iconColor\":\"#ffffff\",\"removable\":true}"},
      {"com.webos.app.hdmi1",
       "{\"id\":\"com.webos.app.hdmi1\",\"title\":\"HDMI 1\"}"},
      {"com.webos.app.browser",
       "{\"id\":\"com.webos.app.browser\",\"title\":\"Web Browser\","
       "\"icon\":\"assets/relative.png\",\"iconColor\":\"\"}"},
      {"bookmark.page", "{\"id\":\"bookmark.page\",\"title\":\"A saved page\","
                        "\"lptype\":\"bookmark\"}"},
      {"com.webos.app.hidden", "{\"id\":\"com.webos.app.hidden\",\"title\":"
                               "\"Hidden\",\"visible\":false}"},
      {"com.webos.app.home",
       "{\"id\":\"com.webos.app.home\",\"title\":\"Home\"}"},
      {"dev.hookedbehemoth.home",
       "{\"id\":\"dev.hookedbehemoth.home\",\"title\":\"Home\","
       "\"removable\":true}"},
  };
  static char out[4096];
  int at =
      snprintf(out, sizeof(out), "{\"returnValue\":true,\"launchPoints\":[");
  const char *comma = "";
  for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
    if (is_removed(points[i][0]) || at >= (int)sizeof(out))
      continue;
    at += snprintf(out + at, sizeof(out) - (size_t)at, "%s%s", comma,
                   points[i][1]);
    comma = ",";
  }
  for (size_t i = 0; i < installed_count && at < (int)sizeof(out); i++)
    if (!is_removed(installed[i][0])) {
      at += snprintf(out + at, sizeof(out) - (size_t)at,
                     "%s{\"id\":\"%s\",\"title\":\"%s\",\"removable\":true}",
                     comma, installed[i][0], installed[i][1]);
      comma = ",";
    }
  if (at < (int)sizeof(out))
    snprintf(out + at, sizeof(out) - (size_t)at, "]}");
  return out;
}

static const char *const inputs =
    "{\"returnValue\":true,\"devices\":["
    "{\"id\":\"HDMI_1\",\"label\":\"HDMI 1\",\"appId\":\"com.webos.app.hdmi1\","
    "\"connected\":true,\"hdmiPlugIn\":true,\"subList\":["
    "{\"id\":\"SIMPLINK\",\"osdName\":\"PlayStation 5\",\"depth\":1}]},"
    "{\"id\":\"HDMI_2\",\"label\":\"HDMI 2\",\"appId\":\"com.webos.app.hdmi2\","
    "\"connected\":true,\"hdmiPlugIn\":false},"
    "{\"id\":\"HDMI_3\",\"label\":\"\",\"connected\":true,\"hdmiPlugIn\":true},"
    "{\"id\":\"HDMI_4\",\"label\":\"HDMI 4\",\"connected\":false},"
    "{\"id\":\"AV_1\",\"label\":\"AV\"}]}";

/* The console's last frame was kept; the other inputs have none. */
static const char *const recents =
    "{\"returnValue\":true,\"recentsAppList\":["
    "{\"appId\":\"com.webos.app.hdmi1\",\"cardSnapShotFilePath\":\"" ICON "\"},"
    "{\"appId\":\"com.webos.app.hdmi2\",\"cardSnapShotFilePath\":\"\"},"
    "{\"appId\":\"netflix\"}]}";

static const char *string_in(json_object *object, const char *key) {
  json_object *value;
  return object != NULL && json_object_object_get_ex(object, key, &value)
             ? json_object_get_string(value)
             : NULL;
}

bool tv_transport_send(const char *uri, const char *payload, uint32_t tag) {
  const char *slash = strrchr(uri, '/');
  const char *method = slash != NULL ? slash + 1 : uri;
  const int index = method_index(method, true);
  if (index >= 0) {
    methods[index].calls++;
    snprintf(methods[index].last, sizeof(methods[0].last), "%s", payload);
    if (methods[index].refuse > 0) {
      methods[index].refuse--;
      tv_deliver(tag, "{\"returnValue\":false,\"errorCode\":-1,"
                      "\"errorText\":\"refused by the mock\"}");
      return true;
    }
  }
  json_object *ask = json_tokener_parse(payload);
  const char *reply = "{\"returnValue\":true}";
  if (strcmp(method, "listLaunchPoints") == 0) {
    reply = launch_points();
  } else if (strcmp(method, "getAllInputStatus") == 0) {
    reply = inputs;
  } else if (strcmp(method, "listRecents") == 0) {
    reply = recents;
  } else if (strcmp(method, "getForegroundAppInfo") == 0) {
    static char answer[256];
    snprintf(answer, sizeof(answer),
             "{\"returnValue\":true,\"appId\":\"%s\",\"processId\":\"\"}",
             foreground);
    reply = answer;
  } else if (strcmp(method, "getSystemSettings") == 0) {
    reply = "{\"returnValue\":true,\"category\":\"general\","
            "\"settings\":{\"lastInputApp\":\"com.webos.app.hdmi1\"}}";
  } else if (strcmp(method, "remove") == 0) {
    const char *id = string_in(ask, "id");
    if (id != NULL && removed_count < sizeof(removed) / sizeof(removed[0]))
      snprintf(removed[removed_count++], sizeof(removed[0]), "%s", id);
  }
  tv_deliver(tag, reply);
  json_object_put(ask);
  return true;
}
