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
  ASK_RECENTS,
  ASK_FOREGROUND,
  ASK_LAUNCH,
  ASK_REMOVE,
  ASK_RESTART,
  ASK_LAST_INPUT,
} ask;

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

void tv_init(void (*wake)(void)) { wake_fn = wake; }

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

void tv_refresh(void) {
  send_json("luna://com.webos.applicationManager/listLaunchPoints",
            json_object_new_object(), tag(ASK_APPS, 0));
  send_json("luna://com.webos.service.eim/getAllInputStatus",
            json_object_new_object(), tag(ASK_INPUTS, 0));
  /* The compositor's list of what was in front lately, with the last frame
   * it kept of each: LG's Home shows an input's from there. */
  send_json("luna://com.webos.service.recent/listRecents",
            json_object_new_object(), tag(ASK_RECENTS, 0));
  /* The launcher, an overlay, is not the foreground app: the one under it
   * is. */
  send_json("luna://com.webos.applicationManager/getForegroundAppInfo",
            json_object_new_object(), tag(ASK_FOREGROUND, 0));
}

bool tv_open_last_input(void) {
  json_object *ask = json_object_new_object();
  json_object *keys = json_object_new_array();
  json_object_array_add(keys, json_object_new_string("lastInputApp"));
  json_object_object_add(ask, "category", json_object_new_string("general"));
  json_object_object_add(ask, "keys", keys);
  return send_json("luna://com.webos.settingsservice/getSystemSettings", ask,
                   tag(ASK_LAST_INPUT, 0));
}

bool tv_restart(void) {
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "reason", json_object_new_string("remoteKey"));
  return send_json("luna://com.webos.service.sleep/shutdown/machineReboot", ask,
                   tag(ASK_RESTART, 0));
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

bool tv_remove_app(const char *app_id) {
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "id", json_object_new_string(app_id));
  json_object_object_add(ask, "subscribe", json_object_new_boolean(false));
  return send_json("luna://com.webos.appInstallService/remove", ask,
                   tag(ASK_REMOVE, 0));
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
  json_object *points = member(reply, "launchPoints");
  if (!succeeded(reply) || !json_object_is_type(points, json_type_array)) {
    failed("The apps could not be listed", reply);
    return;
  }
  tv.app_count = 0;
  const size_t count = json_object_array_length(points);
  for (size_t i = 0; i < count && tv.app_count < TV_APPS; i++) {
    json_object *point = json_object_array_get_idx(points, i);
    const char *id = string_of(point, "id");
    const char *type = string_of(point, "lptype");
    /* Saved web pages and input tiles are bookmarks of something else. */
    if (id == NULL || !flag_of(point, "visible", true) ||
        (type != NULL && strcmp(type, "bookmark") == 0) || !shown_as_app(id))
      continue;
    tv_app *app = &tv.apps[tv.app_count++];
    copy(app->id, sizeof(app->id), id);
    const char *title = string_of(point, "title");
    copy(app->title, sizeof(app->title), title != NULL ? title : id);
    const char *icon = string_of(point, "largeIcon");
    if (icon == NULL || icon[0] != '/')
      icon = string_of(point, "icon");
    copy(app->icon, sizeof(app->icon),
         icon != NULL && icon[0] == '/' ? icon : "");
    /* LG's Home paints the tile in it. */
    const char *color = string_of(point, "iconColor");
    unsigned rgb = 0;
    app->has_color =
        color != NULL && sscanf(color, "#%6x", &rgb) == 1 && strlen(color) == 7;
    app->color = app->has_color ? rgb : 0;
    app->removable = flag_of(point, "removable", false);
  }
  tv.apps_loaded = true;
}

/* The last frame of each app the compositor kept, by app id, as it last
 * answered: an input's is shown on its card. */
#define PREVIEWS 32
static struct {
  char app_id[64];
  char path[384];
} previews[PREVIEWS];
static size_t preview_count;

static void assign_previews(void) {
  for (size_t i = 0; i < tv.input_count; i++) {
    tv_input *input = &tv.inputs[i];
    input->preview[0] = '\0';
    for (size_t p = 0; p < preview_count; p++)
      if (strcmp(previews[p].app_id, input->app_id) == 0)
        copy(input->preview, sizeof(input->preview), previews[p].path);
  }
}

static void recents_answered(json_object *reply) {
  json_object *list = member(reply, "recentsAppList");
  /* Not every TV has the service; the cards do without. */
  if (!succeeded(reply) || !json_object_is_type(list, json_type_array))
    return;
  preview_count = 0;
  for (size_t i = 0;
       i < json_object_array_length(list) && preview_count < PREVIEWS; i++) {
    json_object *recent = json_object_array_get_idx(list, i);
    const char *app = string_of(recent, "appId");
    const char *path = string_of(recent, "cardSnapShotFilePath");
    if (app == NULL || path == NULL || path[0] != '/')
      continue;
    copy(previews[preview_count].app_id, sizeof(previews[0].app_id), app);
    copy(previews[preview_count].path, sizeof(previews[0].path), path);
    preview_count++;
  }
  assign_previews();
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
  assign_previews();
  tv.inputs_loaded = true;
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
  case ASK_RECENTS:
    recents_answered(reply);
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
    /* Gone, or not: the list says which. */
    send_json("luna://com.webos.applicationManager/listLaunchPoints",
              json_object_new_object(), tag(ASK_APPS, 0));
    break;
  case ASK_LAST_INPUT: {
    json_object *settings = NULL;
    const char *app = NULL;
    if (succeeded(reply) &&
        json_object_object_get_ex(reply, "settings", &settings))
      app = string_of(settings, "lastInputApp");
    if (app != NULL && app[0] != '\0')
      launch(app, NULL);
    else
      failed("The TV did not say what it last showed", reply);
    break;
  }
  case ASK_RESTART:
    if (!succeeded(reply))
      failed("The TV would not restart", reply);
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
    if (in_flight > 0)
      in_flight--;
    apply(a);
    free(a->payload);
    free(a);
  }
  return changed;
}
