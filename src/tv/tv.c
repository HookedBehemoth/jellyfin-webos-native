#include "tv.h"

#include <json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"

tv_state tv;

/* ------------------------------------------------------------ the queue */

/* What an answer is to, and which of several: the high half and the low. */
typedef enum {
  ASK_APPS = 1,
  ASK_INPUTS,
  ASK_FOREGROUND,
  ASK_LAUNCH,
  ASK_REMOVE,
  ASK_HOME,
  ASK_ICON,
} ask;

#define HOME_CHOOSER_APP "org.webosbrew.homechooser"
#define HOME_CHOOSER "luna://org.webosbrew.homechooser.service/"
/* The launcher API this was written for. */
#define HOME_CHOOSER_API 1

static uint32_t tag(ask kind, uint32_t index) {
  return (uint32_t)kind << 16 | (index & 0xffff);
}

typedef struct answer {
  uint32_t tag;
  char *payload; /* NULL: no answer */
  struct answer *next;
} answer;

static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static answer *queue_head, **queue_tail = &queue_head;
static void (*wake_fn)(void);
static tv_icon_fn *icon_fn;
static size_t in_flight;

void tv_deliver(uint32_t tag, const char *payload) {
  answer *a = calloc(1, sizeof(*a));
  if (a == NULL)
    return;
  a->tag = tag;
  a->payload = payload != NULL ? strdup(payload) : NULL;
  pthread_mutex_lock(&queue_lock);
  *queue_tail = a;
  queue_tail = &a->next;
  pthread_mutex_unlock(&queue_lock);
  if (wake_fn != NULL)
    wake_fn();
}

static bool send_json(const char *uri, json_object *payload, uint32_t t) {
  const char *text =
      json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN);
  const bool sent = tv_transport_send(uri, text, t);
  json_object_put(payload);
  if (sent)
    in_flight++;
  return sent;
}

size_t tv_pending(void) { return in_flight; }

void tv_init(void (*wake)(void), tv_icon_fn *on_icon) {
  wake_fn = wake;
  icon_fn = on_icon;
}

void tv_deinit(void) {
  pthread_mutex_lock(&queue_lock);
  while (queue_head != NULL) {
    answer *a = queue_head;
    queue_head = a->next;
    free(a->payload);
    free(a);
  }
  queue_tail = &queue_head;
  pthread_mutex_unlock(&queue_lock);
  in_flight = 0;
}

/* ---------------------------------------------------------- the requests */

/* Whether Home Chooser tells of changes to the apps by itself, as far as its
 * last answer said, and whether the subscription's first answer is yet to come:
 * only that one counts as a request in flight. */
static bool apps_live, apps_awaited;

static void ask_apps(void) {
  if (apps_live || apps_awaited)
    return;
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "subscribe", json_object_new_boolean(true));
  const char *text =
      json_object_to_json_string_ext(ask, JSON_C_TO_STRING_PLAIN);
  if (tv_transport_subscribe(HOME_CHOOSER "listApps", text, tag(ASK_APPS, 0))) {
    in_flight++;
    apps_awaited = true;
  }
  json_object_put(ask);
}

void tv_refresh(void) {
  ask_apps();
  send_json("luna://com.webos.service.eim/getAllInputStatus",
            json_object_new_object(), tag(ASK_INPUTS, 0));
  /* The launcher, an overlay, is not the foreground app: the one under it
   * is. */
  send_json(HOME_CHOOSER "getForegroundApp", json_object_new_object(),
            tag(ASK_FOREGROUND, 0));
}

void tv_check_home(void) {
  send_json(HOME_CHOOSER "getInfo", json_object_new_object(), tag(ASK_HOME, 0));
}

void tv_ask_icon(const char *app_id, bool large, uint32_t slot) {
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "id", json_object_new_string(app_id));
  json_object_object_add(ask, "size",
                         json_object_new_string(large ? "largeIcon" : "icon"));
  send_json(HOME_CHOOSER "getAppIcon", ask, tag(ASK_ICON, slot));
}

static bool launch(const char *app_id, json_object *params) {
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "id", json_object_new_string(app_id));
  if (params != NULL)
    json_object_object_add(ask, "params", params);
  return send_json("luna://com.webos.applicationManager/launch", ask,
                   tag(ASK_LAUNCH, 0));
}

bool tv_launch(const char *app_id) { return launch(app_id, NULL); }

bool tv_open_input(const tv_input *input) {
  return launch(input->app_id, NULL);
}

bool tv_ask_to_be_home(const char *app_id) {
  json_object *params = json_object_new_object();
  json_object_object_add(params, "request", json_object_new_string(app_id));
  return launch(HOME_CHOOSER_APP, params);
}

bool tv_remove_app(const char *app_id) {
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "id", json_object_new_string(app_id));
  return send_json(HOME_CHOOSER "uninstallApp", ask, tag(ASK_REMOVE, 0));
}

/* ------------------------------------------------------------ the answers */

static const char *string_of(json_object *object, const char *key) {
  json_object *value;
  if (object == NULL || !json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string))
    return NULL;
  return json_object_get_string(value);
}

/* Missing means true: older firmware leaves the flags out. */
static bool flag_of(json_object *object, const char *key, bool missing) {
  json_object *value;
  if (object == NULL || !json_object_object_get_ex(object, key, &value))
    return missing;
  return json_object_get_boolean(value);
}

static json_object *member(json_object *object, const char *key) {
  json_object *value;
  return object != NULL && json_object_object_get_ex(object, key, &value)
             ? value
             : NULL;
}

static void copy(char *out, size_t size, const char *value) {
  snprintf(out, size, "%s", value != NULL ? value : "");
}

static bool succeeded(json_object *reply) {
  return reply != NULL && flag_of(reply, "returnValue", true);
}

static void failed(const char *what, json_object *reply) {
  const char *why = string_of(reply, "errorText");
  snprintf(tv.error, sizeof(tv.error), "%s: %s", what,
           why != NULL     ? why
           : reply == NULL ? "no answer"
                           : "refused");
  fprintf(stderr, "tv: %s\n", tv.error);
}

/* The input apps are shown as the Inputs row, and the launcher is not one of
 * its own tiles: neither LG's Home nor this one, installed for development. */
static bool shown_as_app(const char *id) {
  static const char *const hidden[] = {"com.webos.app.home",
                                       "dev.hookedbehemoth.home",
                                       "com.webos.app.livetv-hidden"};
  for (size_t i = 0; i < sizeof(hidden) / sizeof(hidden[0]); i++)
    if (strcmp(id, hidden[i]) == 0)
      return false;
  return strncmp(id, "com.webos.app.hdmi", 18) != 0;
}

static void apps_answered(json_object *reply) {
  json_object *listed = member(reply, "apps");
  apps_live = succeeded(reply) && flag_of(reply, "subscribed", false);
  if (!succeeded(reply) || !json_object_is_type(listed, json_type_array)) {
    failed("The apps could not be listed", reply);
    return;
  }
  tv.app_count = 0;
  const size_t count = json_object_array_length(listed);
  for (size_t i = 0; i < count && tv.app_count < TV_APPS; i++) {
    json_object *listing = json_object_array_get_idx(listed, i);
    const char *id = string_of(listing, "id");
    if (id == NULL || !flag_of(listing, "visible", true) || !shown_as_app(id))
      continue;
    tv_app *app = &tv.apps[tv.app_count++];
    copy(app->id, sizeof(app->id), id);
    const char *title = string_of(listing, "title");
    copy(app->title, sizeof(app->title), title != NULL ? title : id);
    copy(app->folder, sizeof(app->folder), string_of(listing, "folderPath"));
    copy(app->icon, sizeof(app->icon), string_of(listing, "icon"));
    copy(app->large_icon, sizeof(app->large_icon),
         string_of(listing, "largeIcon"));
    copy(app->version, sizeof(app->version), string_of(listing, "version"));
    /* LG's Home paints the tile in it. */
    const char *color = string_of(listing, "iconColor");
    unsigned rgb = 0;
    app->has_color =
        color != NULL && sscanf(color, "#%6x", &rgb) == 1 && strlen(color) == 7;
    app->color = app->has_color ? rgb : 0;
    app->removable = flag_of(listing, "removable", false);
  }
  tv.apps_loaded = true;
}

static void inputs_answered(json_object *reply) {
  json_object *devices = member(reply, "devices");
  if (!succeeded(reply) || !json_object_is_type(devices, json_type_array)) {
    failed("The inputs could not be listed", reply);
    return;
  }
  tv.input_count = 0;
  const size_t count = json_object_array_length(devices);
  for (size_t i = 0; i < count && tv.input_count < TV_INPUTS; i++) {
    json_object *device = json_object_array_get_idx(devices, i);
    const char *id = string_of(device, "id");
    int port = 0;
    if (id == NULL || sscanf(id, "HDMI_%d", &port) != 1)
      continue;
    tv_input *input = &tv.inputs[tv.input_count++];
    memset(input, 0, sizeof(*input));
    copy(input->id, sizeof(input->id), id);
    const char *app = string_of(device, "appId");
    if (app != NULL)
      copy(input->app_id, sizeof(input->app_id), app);
    else
      snprintf(input->app_id, sizeof(input->app_id), "com.webos.app.hdmi%d",
               port);
    const char *label = string_of(device, "label");
    if (label != NULL && label[0] != '\0')
      copy(input->label, sizeof(input->label), label);
    else
      snprintf(input->label, sizeof(input->label), "HDMI %d", port);
    /* connected is true for every port on some TVs; the plug is what says. */
    input->connected =
        flag_of(device, "hdmiPlugIn", flag_of(device, "connected", true));
    json_object *subs = member(device, "subList");
    const size_t sub_count = json_object_is_type(subs, json_type_array)
                                 ? json_object_array_length(subs)
                                 : 0;
    for (size_t s = 0; s < sub_count; s++) {
      json_object *sub = json_object_array_get_idx(subs, s);
      const char *name = string_of(sub, "osdName");
      const char *kind = string_of(sub, "id");
      if (name != NULL && name[0] != '\0' && kind != NULL &&
          strcmp(kind, "SIMPLINK") == 0) {
        copy(input->device, sizeof(input->device), name);
        break;
      }
    }
  }
  tv.inputs_loaded = true;
}

/* What getInfo says, in the order the launcher API's guide has it. */
static void home_answered(json_object *reply) {
  json_object *version = member(reply, "apiVersion");
  const char *why = string_of(reply, "errorText");
  /* The bus, not Home Chooser: it is there, but not set up, as after being
   * installed again. */
  if (!succeeded(reply) && why != NULL && strstr(why, "Denied") != NULL)
    snprintf(tv.error, sizeof(tv.error),
             "Open Home Chooser once to finish setting it up");
  else if (!succeeded(reply))
    failed("Home Chooser is needed, and did not answer", reply);
  else if (version == NULL || json_object_get_int(version) != HOME_CHOOSER_API)
    snprintf(tv.error, sizeof(tv.error),
             "This version of Home Chooser is not the one this Home knows");
  else if (!flag_of(reply, "root", false))
    snprintf(tv.error, sizeof(tv.error),
             "Open Home Chooser once to finish setting it up");
  else if (!flag_of(reply, "permitted", false))
    snprintf(tv.error, sizeof(tv.error),
             "Install this Home again, with Home Chooser already set up");
  else
    tv.not_chosen = !flag_of(reply, "isSelectedHome", false);
}

static int base64_digit(char c) {
  if (c >= 'A' && c <= 'Z')
    return c - 'A';
  if (c >= 'a' && c <= 'z')
    return c - 'a' + 26;
  if (c >= '0' && c <= '9')
    return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

/* Up to the padding, or the first thing that is not base64. Its length in
 * `size`; NULL when out of memory. */
static uint8_t *base64_decode(const char *text, size_t *size) {
  uint8_t *out = malloc(strlen(text) / 4 * 3 + 3);
  if (out == NULL)
    return NULL;
  size_t at = 0;
  uint32_t bits = 0;
  int count = 0;
  for (; base64_digit(*text) >= 0; text++) {
    bits = bits << 6 | (uint32_t)base64_digit(*text);
    if ((count += 6) >= 8) {
      count -= 8;
      out[at++] = (uint8_t)(bits >> count);
    }
  }
  *size = at;
  return out;
}

static void icon_answered(uint32_t slot, json_object *reply) {
  const char *data = succeeded(reply) ? string_of(reply, "data") : NULL;
  size_t size = 0;
  uint8_t *bytes = data != NULL ? base64_decode(data, &size) : NULL;
  if (icon_fn != NULL)
    icon_fn(slot, bytes, size);
  free(bytes);
}

static void apply(const answer *a) {
  json_object *reply =
      a->payload != NULL ? json_tokener_parse(a->payload) : NULL;
  switch ((ask)(a->tag >> 16)) {
  case ASK_APPS:
    apps_answered(reply);
    break;
  case ASK_INPUTS:
    inputs_answered(reply);
    break;
  case ASK_FOREGROUND:
    copy(tv.foreground, sizeof(tv.foreground),
         succeeded(reply) ? string_of(reply, "appId") : NULL);
    tv.foreground_fresh = true;
    break;
  case ASK_LAUNCH:
    if (!succeeded(reply))
      failed("The TV would not open it", reply);
    break;
  case ASK_REMOVE:
    if (!succeeded(reply))
      failed("The app could not be removed", reply);
    /* Gone once the owner agrees: said by itself, or else seen next time. */
    ask_apps();
    break;
  case ASK_HOME:
    home_answered(reply);
    break;
  case ASK_ICON:
    icon_answered(a->tag & 0xffff, reply);
    break;
  }
  json_object_put(reply);
}

bool tv_pump(void) {
  pthread_mutex_lock(&queue_lock);
  answer *list = queue_head;
  queue_head = NULL;
  queue_tail = &queue_head;
  pthread_mutex_unlock(&queue_lock);
  const bool changed = list != NULL;
  while (list != NULL) {
    answer *a = list;
    list = a->next;
    /* A subscription's later answers were never asked for. */
    const bool later = a->tag >> 16 == ASK_APPS && !apps_awaited;
    if (a->tag >> 16 == ASK_APPS)
      apps_awaited = false;
    if (in_flight > 0 && !later)
      in_flight--;
    apply(a);
    free(a->payload);
    free(a);
  }
  return changed;
}
