#include "tv.h"

#include <json.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "transport.h"

tv_state tv = {
    .settings =
        {
            [TV_PICTURE_MODE] = {"picture", "pictureMode", "Picture mode"},
            [TV_SOUND_OUTPUT] = {"sound", "soundOutput", "Sound out"},
            [TV_ENERGY_SAVING] = {"picture", "energySaving", "Energy saving"},
        },
};

/* The candidates for each overlay, first found wins: LG renamed some between
 * releases. Each is asked for with getAppInfo, so only the ones this TV has
 * are offered. */
static const struct {
  const char *title;
  const char *apps[3];
} overlays[TV_OVERLAY_COUNT] = {
    [TV_OVERLAY_INPUTS] = {"Inputs",
                           {"com.webos.app.quickinputpicker",
                            "com.webos.app.inputcommon"}},
    [TV_OVERLAY_QUICK_SETTINGS] = {"Quick settings",
                                   {"com.webos.app.quicksettings"}},
    [TV_OVERLAY_SETTINGS] = {"All settings", {"com.palm.app.settings"}},
    [TV_OVERLAY_NOTIFICATIONS] = {"Notifications",
                                  {"com.webos.app.notificationcenter"}},
    [TV_OVERLAY_SEARCH] = {"Search", {"com.webos.app.voice"}},
};
#define OVERLAY_CANDIDATES                                                     \
  (sizeof(overlays[0].apps) / sizeof(overlays[0].apps[0]))

/* Renamed between webOS generations: the newer name, and on a TV that says it
 * has no such service, the older one from then on. */
static const char *settings_service = "com.webos.service.settings";
#define SETTINGS_LEGACY "com.webos.settingsservice"

/* ------------------------------------------------------------ the queue */

/* What an answer is to, and which of several: the high half and the low. */
typedef enum {
  ASK_APPS = 1,
  ASK_INPUTS,
  ASK_SETTING,
  ASK_DESCRIPTION,
  ASK_WRITE,
  ASK_OVERLAY,
  ASK_LAUNCH,
  ASK_SCREEN_OFF,
} ask;

/* A settings request sent to the older service name, which is not asked again
 * under it. */
#define LEGACY_BIT 0x8000u

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

static char uri_buffer[160];
static const char *settings_uri(const char *method) {
  snprintf(uri_buffer, sizeof(uri_buffer), "luna://%s/%s", settings_service,
           method);
  return uri_buffer;
}

static uint32_t settings_tag(ask kind, tv_setting_id id) {
  return tag(
      kind,
      (uint32_t)id |
          (strcmp(settings_service, SETTINGS_LEGACY) == 0 ? LEGACY_BIT : 0));
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

/* getSystemSettings for its value, or getSystemSettingDesc for the values
 * it takes: the same question of both. */
static void ask_setting(tv_setting_id id, ask kind) {
  const tv_setting *s = &tv.settings[id];
  json_object *keys = json_object_new_array();
  json_object_array_add(keys, json_object_new_string(s->key));
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "category", json_object_new_string(s->category));
  json_object_object_add(ask, "keys", keys);
  send_json(settings_uri(kind == ASK_SETTING ? "getSystemSettings"
                                             : "getSystemSettingDesc"),
            ask, settings_tag(kind, id));
}

static void ask_overlay(tv_overlay overlay, size_t candidate) {
  json_object *ask = json_object_new_object();
  json_object_object_add(
      ask, "id", json_object_new_string(overlays[overlay].apps[candidate]));
  send_json("luna://com.webos.applicationManager/getAppInfo", ask,
            tag(ASK_OVERLAY, (uint32_t)(overlay << 4 | candidate)));
}

void tv_refresh(void) {
  send_json("luna://com.webos.applicationManager/listLaunchPoints",
            json_object_new_object(), tag(ASK_APPS, 0));
  send_json("luna://com.webos.service.eim/getAllInputStatus",
            json_object_new_object(), tag(ASK_INPUTS, 0));
  for (int i = 0; i < TV_SETTING_COUNT; i++) {
    ask_setting((tv_setting_id)i, ASK_SETTING);
    ask_setting((tv_setting_id)i, ASK_DESCRIPTION);
  }
  for (int i = 0; i < TV_OVERLAY_COUNT; i++)
    if (tv.overlay_app[i][0] == '\0')
      ask_overlay((tv_overlay)i, 0);
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

bool tv_open_overlay(tv_overlay overlay) {
  if (overlay >= TV_OVERLAY_COUNT || tv.overlay_app[overlay][0] == '\0')
    return false;
  return launch(tv.overlay_app[overlay], NULL);
}

const char *tv_overlay_title(tv_overlay overlay) {
  return overlay < TV_OVERLAY_COUNT ? overlays[overlay].title : "";
}

/* What a write replaces, for putting back when the TV refuses it. */
static char before_write[TV_SETTING_COUNT][48];

void tv_setting_set(tv_setting_id id, const char *value) {
  tv_setting *s = &tv.settings[id];
  if (!s->writing)
    snprintf(before_write[id], sizeof(before_write[id]), "%s", s->value);
  snprintf(s->value, sizeof(s->value), "%s", value);
  json_object *settings = json_object_new_object();
  json_object_object_add(settings, s->key, json_object_new_string(value));
  json_object *ask = json_object_new_object();
  json_object_object_add(ask, "category", json_object_new_string(s->category));
  json_object_object_add(ask, "settings", settings);
  s->writing = send_json(settings_uri("setSystemSettings"), ask,
                         settings_tag(ASK_WRITE, id)) ||
               s->writing;
}

bool tv_screen_off(void) {
  return send_json("luna://com.webos.service.tvpower/power/turnOffScreen",
                   json_object_new_object(), tag(ASK_SCREEN_OFF, 0));
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
 * its own tiles. */
static bool shown_as_app(const char *id) {
  static const char *const hidden[] = {"com.webos.app.home",
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

/* "Service does not exist", from a TV that has the older name. */
static bool wrong_service(json_object *reply) {
  const char *why = string_of(reply, "errorText");
  return reply != NULL && !succeeded(reply) && why != NULL &&
         strstr(why, "does not exist") != NULL;
}

static void setting_answered(tv_setting_id id, json_object *reply) {
  tv_setting *s = &tv.settings[id];
  const char *value = string_of(member(reply, "settings"), s->key);
  /* A write already on its way says what the value will be. */
  if (value == NULL || s->writing)
    return;
  copy(s->value, sizeof(s->value), value);
  s->known = true;
}

static void description_answered(tv_setting_id id, json_object *reply) {
  tv_setting *s = &tv.settings[id];
  json_object *results = member(reply, "results");
  if (!json_object_is_type(results, json_type_array))
    return;
  for (size_t i = 0; i < json_object_array_length(results); i++) {
    json_object *result = json_object_array_get_idx(results, i);
    const char *key = string_of(result, "key");
    if (key == NULL || strcmp(key, s->key) != 0)
      continue;
    json_object *ui = member(result, "ui");
    s->visible = flag_of(ui, "visible", true);
    s->active = flag_of(ui, "active", true);
    json_object *values = member(result, "values");
    json_object *list = member(values, "arrayExt");
    const bool extended = json_object_is_type(list, json_type_array);
    if (!extended)
      list = member(values, "array");
    if (!json_object_is_type(list, json_type_array))
      return;
    s->choice_count = 0;
    for (size_t v = 0; v < json_object_array_length(list); v++) {
      json_object *entry = json_object_array_get_idx(list, v);
      const char *choice =
          extended ? string_of(entry, "value") : json_object_get_string(entry);
      if (choice == NULL || (extended && !flag_of(entry, "visible", true)) ||
          s->choice_count == TV_CHOICES)
        continue;
      copy(s->choices[s->choice_count++], sizeof(s->choices[0]), choice);
    }
    return;
  }
}

static void write_answered(tv_setting_id id, json_object *reply) {
  tv_setting *s = &tv.settings[id];
  s->writing = false;
  if (succeeded(reply))
    return;
  failed(s->title, reply);
  copy(s->value, sizeof(s->value), before_write[id]);
}

static void overlay_answered(uint32_t index, json_object *reply) {
  const tv_overlay overlay = (tv_overlay)(index >> 4);
  const size_t candidate = index & 15;
  if (overlay >= TV_OVERLAY_COUNT)
    return;
  json_object *info = member(reply, "appInfo");
  if (succeeded(reply) && info != NULL) {
    copy(tv.overlay_app[overlay], sizeof(tv.overlay_app[0]),
         overlays[overlay].apps[candidate]);
    return;
  }
  if (candidate + 1 < OVERLAY_CANDIDATES &&
      overlays[overlay].apps[candidate + 1] != NULL)
    ask_overlay(overlay, candidate + 1);
}

static void apply(const answer *a) {
  json_object *reply =
      a->payload != NULL ? json_tokener_parse(a->payload) : NULL;
  const ask kind = (ask)(a->tag >> 16);
  const bool settings =
      kind == ASK_SETTING || kind == ASK_DESCRIPTION || kind == ASK_WRITE;
  const bool legacy = settings && (a->tag & LEGACY_BIT) != 0;
  const uint32_t index = a->tag & 0xffff & ~(settings ? LEGACY_BIT : 0);
  if (settings && !legacy && index < TV_SETTING_COUNT && wrong_service(reply)) {
    settings_service = SETTINGS_LEGACY;
    if (kind == ASK_WRITE) {
      /* Still writing, so the value to put back on a refusal is kept. */
      char wanted[48];
      snprintf(wanted, sizeof(wanted), "%s", tv.settings[index].value);
      tv_setting_set((tv_setting_id)index, wanted);
    } else {
      ask_setting((tv_setting_id)index, kind);
    }
    json_object_put(reply);
    return;
  }
  switch (kind) {
  case ASK_APPS:
    apps_answered(reply);
    break;
  case ASK_INPUTS:
    inputs_answered(reply);
    break;
  case ASK_SETTING:
    if (index < TV_SETTING_COUNT)
      setting_answered((tv_setting_id)index, reply);
    break;
  case ASK_DESCRIPTION:
    if (index < TV_SETTING_COUNT)
      description_answered((tv_setting_id)index, reply);
    break;
  case ASK_WRITE:
    if (index < TV_SETTING_COUNT)
      write_answered((tv_setting_id)index, reply);
    break;
  case ASK_OVERLAY:
    overlay_answered(index, reply);
    break;
  case ASK_LAUNCH:
    if (!succeeded(reply))
      failed("The TV would not open it", reply);
    break;
  case ASK_SCREEN_OFF:
    if (!succeeded(reply))
      failed("The screen could not be switched off", reply);
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

/* --------------------------------------------------------------- titles */

const char *tv_value_title(const char *value) {
  static const struct {
    const char *value, *title;
  } titles[] = {
      {"vivid", "Vivid"},
      {"normal", "Standard"},
      {"eco", "Eco"},
      {"cinema", "Cinema"},
      {"sports", "Sports"},
      {"game", "Game"},
      {"filmMaker", "Filmmaker"},
      {"expert1", "Expert (bright room)"},
      {"expert2", "Expert (dark room)"},
      {"photo", "Photo"},
      {"hdrVivid", "Vivid"},
      {"hdrStandard", "Standard"},
      {"hdrCinema", "Cinema"},
      {"hdrFilmMaker", "Filmmaker"},
      {"dolbyHdrVivid", "Vivid"},
      {"dolbyHdrCinema", "Cinema"},
      {"dolbyHdrCinemaBright", "Cinema Home"},
      {"dolbyHdrGame", "Game"},
      {"tv_speaker", "TV speaker"},
      {"external_arc", "HDMI (ARC)"},
      {"external_optical", "Optical"},
      {"bt_soundbar", "Bluetooth"},
      {"headphone", "Wired headphones"},
      {"tv_speaker_headphone", "TV speaker and headphones"},
      {"tv_external_speaker", "TV speaker and optical"},
      {"lineout", "Line out"},
      {"auto", "Automatic"},
      {"off", "Off"},
      {"min", "Minimum"},
      {"med", "Medium"},
      {"max", "Maximum"},
      {"screen_off", "Screen off"},
  };
  for (size_t i = 0; i < sizeof(titles) / sizeof(titles[0]); i++)
    if (strcmp(value, titles[i].value) == 0)
      return titles[i].title;
  return value;
}
