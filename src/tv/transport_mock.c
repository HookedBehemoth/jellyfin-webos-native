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

static struct {
  const char *key;
  char value[48];
} settings[] = {
    {"pictureMode", "expert1"},
    {"energySaving", "auto"},
    {"soundOutput", "tv_speaker"},
};
#define SETTING_COUNT (sizeof(settings) / sizeof(settings[0]))

static bool legacy_only;
static char installed[4][2][64];
static size_t installed_count;

void tv_mock_reset(void) {
  memset(methods, 0, sizeof(methods));
  method_count = 0;
  snprintf(settings[0].value, sizeof(settings[0].value), "expert1");
  snprintf(settings[1].value, sizeof(settings[1].value), "auto");
  snprintf(settings[2].value, sizeof(settings[2].value), "tv_speaker");
  legacy_only = false;
  installed_count = 0;
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

const char *tv_mock_setting(const char *key) {
  for (size_t i = 0; i < SETTING_COUNT; i++)
    if (strcmp(settings[i].key, key) == 0)
      return settings[i].value;
  return "";
}

void tv_mock_refuse(const char *method, unsigned count) {
  const int i = method_index(method, true);
  if (i >= 0)
    methods[i].refuse = count;
}

void tv_mock_legacy_settings(bool legacy) { legacy_only = legacy; }

void tv_mock_install(const char *id, const char *title) {
  if (installed_count == sizeof(installed) / sizeof(installed[0]))
    return;
  snprintf(installed[installed_count][0], sizeof(installed[0][0]), "%s", id);
  snprintf(installed[installed_count][1], sizeof(installed[0][1]), "%s", title);
  installed_count++;
}

/* ------------------------------------------------------------ answers */

static const char *launch_points(void) {
  static char out[4096];
  int at = snprintf(
      out, sizeof(out),
      "{\"returnValue\":true,\"launchPoints\":["
      "{\"id\":\"netflix\",\"title\":\"Netflix\",\"icon\":\"" ICON "\","
      "\"largeIcon\":\"" ICON "\",\"lptype\":\"default\"},"
      "{\"id\":\"youtube.leanback.v4\",\"title\":\"YouTube\","
      "\"icon\":\"" ICON "\"},"
      "{\"id\":\"com.webos.app.hdmi1\",\"title\":\"HDMI 1\"},"
      "{\"id\":\"com.webos.app.browser\",\"title\":\"Web Browser\","
      "\"icon\":\"assets/relative.png\"},"
      "{\"id\":\"bookmark.page\",\"title\":\"A saved page\","
      "\"lptype\":\"bookmark\"},"
      "{\"id\":\"com.webos.app.hidden\",\"title\":\"Hidden\","
      "\"visible\":false},"
      "{\"id\":\"com.webos.app.home\",\"title\":\"Home\"}");
  for (size_t i = 0; i < installed_count && at < (int)sizeof(out); i++)
    at += snprintf(out + at, sizeof(out) - (size_t)at,
                   ",{\"id\":\"%s\",\"title\":\"%s\"}", installed[i][0],
                   installed[i][1]);
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

static const char *description(const char *key) {
  if (strcmp(key, "pictureMode") == 0)
    return "{\"returnValue\":true,\"results\":[{\"key\":\"pictureMode\","
           "\"ui\":{\"visible\":true,\"active\":true},\"values\":{\"arrayExt\":"
           "["
           "{\"value\":\"vivid\",\"visible\":true},"
           "{\"value\":\"normal\",\"visible\":true},"
           "{\"value\":\"eco\",\"visible\":false},"
           "{\"value\":\"cinema\",\"visible\":true},"
           "{\"value\":\"expert1\",\"visible\":true},"
           "{\"value\":\"expert2\",\"visible\":true}]}}]}";
  if (strcmp(key, "energySaving") == 0)
    /* The older shape, a plain array, greyed out in this picture mode. */
    return "{\"returnValue\":true,\"results\":[{\"key\":\"energySaving\","
           "\"ui\":{\"active\":false},\"values\":{\"array\":["
           "\"auto\",\"off\",\"min\",\"med\",\"max\",\"screen_off\"]}}]}";
  if (strcmp(key, "soundOutput") == 0)
    return "{\"returnValue\":true,\"results\":[{\"key\":\"soundOutput\","
           "\"values\":{\"arrayExt\":["
           "{\"value\":\"tv_speaker\"},{\"value\":\"external_arc\"},"
           "{\"value\":\"bt_soundbar\"}]}}]}";
  return "{\"returnValue\":false,\"errorText\":\"no result\"}";
}

static const char *string_in(json_object *object, const char *key) {
  json_object *value;
  return object != NULL && json_object_object_get_ex(object, key, &value)
             ? json_object_get_string(value)
             : NULL;
}

/* The first key a get or a describe names. */
static const char *first_key(json_object *ask) {
  json_object *keys;
  if (!json_object_object_get_ex(ask, "keys", &keys) ||
      json_object_array_length(keys) == 0)
    return "";
  return json_object_get_string(json_object_array_get_idx(keys, 0));
}

static char answer[1024];

static const char *answer_settings(const char *method, json_object *ask) {
  if (strcmp(method, "getSystemSettingDesc") == 0)
    return description(first_key(ask));
  if (strcmp(method, "getSystemSettings") == 0) {
    const char *key = first_key(ask);
    snprintf(answer, sizeof(answer),
             "{\"returnValue\":true,\"settings\":{\"%s\":\"%s\"}}", key,
             tv_mock_setting(key));
    return answer;
  }
  json_object *written;
  if (!json_object_object_get_ex(ask, "settings", &written))
    return "{\"returnValue\":false,\"errorText\":\"no settings\"}";
  json_object_object_foreach(written, key, value) {
    for (size_t i = 0; i < SETTING_COUNT; i++)
      if (strcmp(settings[i].key, key) == 0)
        snprintf(settings[i].value, sizeof(settings[i].value), "%s",
                 json_object_get_string(value));
  }
  return "{\"returnValue\":true}";
}

static const char *answer_app_info(json_object *ask) {
  static const char *const present[] = {"com.webos.app.inputcommon",
                                        "com.palm.app.settings",
                                        "com.webos.app.notificationcenter"};
  const char *id = string_in(ask, "id");
  for (size_t i = 0; id != NULL && i < sizeof(present) / sizeof(present[0]);
       i++)
    if (strcmp(id, present[i]) == 0) {
      snprintf(answer, sizeof(answer),
               "{\"returnValue\":true,\"appInfo\":{\"id\":\"%s\","
               "\"visible\":false}}",
               id);
      return answer;
    }
  return "{\"returnValue\":false,\"errorText\":\"Cannot find proper "
         "launchPoint\"}";
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
  if (strstr(uri, "settings") != NULL) {
    const bool legacy = strstr(uri, "com.webos.settingsservice") != NULL;
    reply = legacy_only != legacy
                ? "{\"returnValue\":false,\"errorCode\":-1,\"errorText\":"
                  "\"Service does not exist: com.webos.service.settings.\"}"
                : answer_settings(method, ask);
  } else if (strcmp(method, "listLaunchPoints") == 0) {
    reply = launch_points();
  } else if (strcmp(method, "getAllInputStatus") == 0) {
    reply = inputs;
  } else if (strcmp(method, "getAppInfo") == 0) {
    reply = answer_app_info(ask);
  }
  tv_deliver(tag, reply);
  json_object_put(ask);
  return true;
}
