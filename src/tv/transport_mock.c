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
static bool hc_root, hc_permitted, hc_chosen, subscribable;
static bool has_subscriber;
static uint32_t subscriber;

static const char *list_apps(bool subscribed);

static void tell_subscriber(void) {
  if (has_subscriber)
    tv_deliver(subscriber, list_apps(true));
}

void tv_mock_reset(void) {
  hc_root = hc_permitted = hc_chosen = subscribable = true;
  has_subscriber = false;
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

static const char *refusal_text;
void tv_mock_refuse_text(const char *text) { refusal_text = text; }

void tv_mock_foreground(const char *id) {
  snprintf(foreground, sizeof(foreground), "%s", id);
}

void tv_mock_install(const char *id, const char *title) {
  if (installed_count == sizeof(installed) / sizeof(installed[0]))
    return;
  snprintf(installed[installed_count][0], sizeof(installed[0][0]), "%s", id);
  snprintf(installed[installed_count][1], sizeof(installed[0][1]), "%s", title);
  installed_count++;
  tell_subscriber();
}

void tv_mock_subscribable(bool can) { subscribable = can; }

void tv_mock_lose_subscribers(void) {
  if (has_subscriber)
    tv_deliver(subscriber, "{\"returnValue\":false,\"errorCode\":-1,"
                           "\"errorText\":\"The service went away\"}");
  has_subscriber = false;
}

void tv_mock_home_chooser(bool root, bool permitted, bool chosen) {
  hc_root = root;
  hc_permitted = permitted;
  hc_chosen = chosen;
}

/* ------------------------------------------------------------ answers */

static bool is_removed(const char *id) {
  for (size_t i = 0; i < removed_count; i++)
    if (strcmp(removed[i], id) == 0)
      return true;
  return false;
}

static const char *list_apps(bool subscribed) {
  /* Netflix asks for a red tile and can be removed; the browser is LG's. */
  static const char *const apps[][2] = {
      {"netflix", "{\"id\":\"netflix\",\"title\":\"Netflix\","
                  "\"folderPath\":\"" JF_SOURCE_DIR "/assets\","
                  "\"version\":\"1.0\",\"icon\":\"icon.png\","
                  "\"largeIcon\":\"icon.png\","
                  "\"iconColor\":\"#e50914\",\"removable\":true,"
                  "\"visible\":true}"},
      {"youtube.leanback.v4",
       "{\"id\":\"youtube.leanback.v4\",\"title\":\"YouTube\","
       "\"folderPath\":\"/nonexistent\",\"version\":\"2.0\","
       "\"icon\":\"icon.png\",\"largeIcon\":\"large.png\","
       "\"iconColor\":\"#ffffff\",\"removable\":true,\"visible\":true}"},
      {"com.webos.app.hdmi1",
       "{\"id\":\"com.webos.app.hdmi1\",\"title\":\"HDMI 1\"}"},
      {"com.webos.app.browser",
       "{\"id\":\"com.webos.app.browser\",\"title\":\"Web Browser\","
       "\"icon\":\"gone.png\",\"iconColor\":\"\",\"visible\":true}"},
      {"com.webos.app.hidden", "{\"id\":\"com.webos.app.hidden\",\"title\":"
                               "\"Hidden\",\"visible\":false}"},
      {"com.webos.app.home",
       "{\"id\":\"com.webos.app.home\",\"title\":\"Home\"}"},
      {"dev.hookedbehemoth.home",
       "{\"id\":\"dev.hookedbehemoth.home\",\"title\":\"Home\","
       "\"removable\":true}"},
  };
  static char out[4096];
  int at = snprintf(out, sizeof(out),
                    "{\"returnValue\":true,\"apiVersion\":1,%s\"apps\":[",
                    subscribed ? "\"subscribed\":true," : "");
  const char *comma = "";
  for (size_t i = 0; i < sizeof(apps) / sizeof(apps[0]); i++) {
    if (is_removed(apps[i][0]) || at >= (int)sizeof(out))
      continue;
    at +=
        snprintf(out + at, sizeof(out) - (size_t)at, "%s%s", comma, apps[i][1]);
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

/* getAppIcon's answer, the repository's icon for every app that has one. */
static const char *app_icon(const char *id, const char *size) {
  const bool large = size != NULL && strcmp(size, "largeIcon") == 0;
  if (strcmp(id, "com.webos.app.browser") == 0 ||
      (strcmp(id, "youtube.leanback.v4") == 0 && large))
    return "{\"returnValue\":false,\"apiVersion\":1,\"errorCode\":\"noIcon\","
           "\"errorText\":\"The app has no readable icon\"}";
  static const char digits[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  static unsigned char bytes[16384];
  static char out[32768];
  FILE *file = fopen(ICON, "rb");
  const size_t read = file != NULL ? fread(bytes, 1, sizeof(bytes), file) : 0;
  if (file != NULL)
    fclose(file);
  int at = snprintf(out, sizeof(out),
                    "{\"returnValue\":true,\"apiVersion\":1,"
                    "\"mimeType\":\"image/png\",\"data\":\"");
  for (size_t i = 0; i < read; i += 3) {
    const unsigned word = (unsigned)bytes[i] << 16 |
                          (i + 1 < read ? (unsigned)bytes[i + 1] << 8 : 0) |
                          (i + 2 < read ? bytes[i + 2] : 0);
    out[at++] = digits[word >> 18 & 63];
    out[at++] = digits[word >> 12 & 63];
    out[at++] = i + 1 < read ? digits[word >> 6 & 63] : '=';
    out[at++] = i + 2 < read ? digits[word & 63] : '=';
  }
  snprintf(out + at, sizeof(out) - (size_t)at, "\",\"byteLength\":%zu}", read);
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

static const char *string_in(json_object *object, const char *key) {
  json_object *value;
  return object != NULL && json_object_object_get_ex(object, key, &value)
             ? json_object_get_string(value)
             : NULL;
}

bool tv_transport_subscribe(const char *uri, const char *payload,
                            uint32_t tag) {
  const char *method = strrchr(uri, '/') + 1;
  const int index = method_index(method, true);
  if (!subscribable || (index >= 0 && methods[index].refuse > 0))
    return tv_transport_send(uri, payload, tag);
  if (index >= 0)
    methods[index].calls++;
  has_subscriber = true;
  subscriber = tag;
  tell_subscriber();
  return true;
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
      char refusal[256];
      snprintf(refusal, sizeof(refusal),
               "{\"returnValue\":false,\"errorCode\":-1,\"errorText\":\"%s\"}",
               refusal_text != NULL ? refusal_text : "refused by the mock");
      tv_deliver(tag, refusal);
      return true;
    }
  }
  json_object *ask = json_tokener_parse(payload);
  const char *reply = "{\"returnValue\":true}";
  if (strcmp(method, "listApps") == 0) {
    reply = list_apps(false);
  } else if (strcmp(method, "getAllInputStatus") == 0) {
    reply = inputs;
  } else if (strcmp(method, "getInfo") == 0) {
    static char answer[256];
    snprintf(answer, sizeof(answer),
             "{\"returnValue\":true,\"apiVersion\":1,\"root\":%s,"
             "\"permitted\":%s,\"isSelectedHome\":%s}",
             hc_root ? "true" : "false", hc_permitted ? "true" : "false",
             hc_chosen ? "true" : "false");
    reply = answer;
  } else if (strcmp(method, "getAppIcon") == 0) {
    reply = app_icon(string_in(ask, "id"), string_in(ask, "size"));
  } else if (strcmp(method, "getForegroundApp") == 0) {
    static char answer[256];
    snprintf(answer, sizeof(answer),
             "{\"returnValue\":true,\"appId\":\"%s\",\"processId\":\"\"}",
             foreground);
    reply = answer;
  } else if (strcmp(method, "uninstallApp") == 0) {
    /* And the owner agreeing at once, which the real thing never does. */
    reply = "{\"returnValue\":true,\"apiVersion\":1,\"pending\":true,"
            "\"confirmation\":\"alert\"}";
    const char *id = string_in(ask, "id");
    if (id != NULL && removed_count < sizeof(removed) / sizeof(removed[0]))
      snprintf(removed[removed_count++], sizeof(removed[0]), "%s", id);
    tell_subscriber();
  }
  tv_deliver(tag, reply);
  json_object_put(ask);
  return true;
}
