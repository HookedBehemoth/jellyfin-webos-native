#include "luna.h"

bool jf_luna_log;

#ifndef JF_WEBOS

/* Off-device there is no Luna bus and nothing asking the app to close, which is
 * not an error - it is the whole reason registerLifecycle returning false is
 * not fatal. */
bool jf_luna_register_lifecycle(void (*quit)(void), void (*relaunch)(void)) {
  (void)quit;
  (void)relaunch;
  return false;
}

void jf_luna_deinit(void) {}
void jf_luna_query_hdr(void) {}
int jf_luna_hdr(void) { return -1; }
bool jf_luna_relaunch_home_key(void) { return false; }

bool jf_luna_call(const char *uri, const char *payload, jf_luna_reply_fn reply,
                  void *user) {
  (void)uri, (void)payload, (void)reply, (void)user;
  return false;
}

void jf_luna_set_name(const char *service) { (void)service; }

#else

#include <glib.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luna-service2/lunaservice.h>
#include <webos-helpers/libhelpers.h>

/* Exported by every libhelpers, declared by none of their headers: what the
 * last failed call said, such as the bus refusing the app's name. */
const char *HGetError(void);

static const char *helpers_error(void) {
  const char *error = HGetError();
  return error != NULL ? error : "no reason given";
}

static HContext context;
static GMainLoop *loop;

/* The bus connection of a process that names itself (jf_luna_set_name), or
 * NULL for libhelpers' own. */
static LSHandle *named;
static char name[128];

void jf_luna_set_name(const char *service) {
  snprintf(name, sizeof(name), "%s", service);
}

static bool start_loop(void);

/* Registers `name` on first use; false, with the bus's reason, when the bus
 * refuses it. */
static bool named_handle(void) {
  if (named != NULL)
    return true;
  if (!start_loop())
    return false;
  LSError error;
  LSErrorInit(&error);
  LSHandle *handle = NULL;
  if (!LSRegister(name, &handle, &error) ||
      !LSGmainAttach(handle, loop, &error)) {
    fprintf(stderr, "luna: %s cannot register: %s\n", name,
            error.message != NULL ? error.message : "no reason given");
    LSErrorFree(&error);
    if (handle != NULL)
      LSUnregister(handle, NULL);
    return false;
  }
  named = handle;
  fprintf(stderr, "luna: registered as %s\n", name);
  return true;
}

/* One call through whichever connection this process has: `helpers` carries
 * the callback for libhelpers, which wants a context that outlives the call.
 * `multiple` keeps answers coming, for a subscription. */
static bool send_call(const char *uri, const char *payload, bool multiple,
                      HContext *helpers) {
  if (name[0] == '\0') {
    helpers->multiple = multiple;
    helpers->pub = 1;
    if (HLunaServiceCall(uri, payload, helpers) == 0)
      return true;
    fprintf(stderr, "luna: %s failed: %s\n", uri, helpers_error());
    return false;
  }
  if (!named_handle())
    return false;
  LSError error;
  LSErrorInit(&error);
  const bool sent = multiple
                        ? LSCall(named, uri, payload, helpers->callback,
                                 helpers->userdata, NULL, &error)
                        : LSCallOneReply(named, uri, payload, helpers->callback,
                                         helpers->userdata, NULL, &error);
  if (!sent) {
    fprintf(stderr, "luna: %s failed: %s\n", uri,
            error.message != NULL ? error.message : "no reason given");
    LSErrorFree(&error);
  }
  return sent;
}
static pthread_t loop_thread;
static bool loop_running;
static void (*on_quit)(void);
static void (*on_relaunch)(void);
static atomic_bool relaunch_home_key;

static bool on_message(LSHandle *handle, LSMessage *message, void *user) {
  (void)handle;
  (void)user;
  const char *payload = LSMessageGetPayload(message);
  if (payload == NULL)
    return true;
  if (jf_luna_log)
    fprintf(stderr, "luna: %s\n", payload);
  char event[32];
  if (!jf_luna_json_string(payload, "event", event, sizeof(event)))
    return true;
  fprintf(stderr, "luna lifecycle: %s\n", event);
  if (strcmp(event, "close") == 0 && on_quit != NULL)
    on_quit();
  /* A minimised app is still running, so opening it from the launcher is a
   * relaunch rather than a start: nothing else will raise the window. */
  if (strcmp(event, "relaunch") == 0 && on_relaunch != NULL) {
    /* The Home key, as against SAM opening Home because nothing else is
     * left in front. */
    char reason[32];
    atomic_store(
        &relaunch_home_key,
        jf_luna_json_string(payload, "reason", reason, sizeof(reason)) &&
            strcmp(reason, "hotKey") == 0);
    on_relaunch();
  }
  return true;
}

static HContext hdr_context;
static atomic_int hdr = -1;

static bool on_hdr(LSHandle *handle, LSMessage *message, void *user) {
  (void)handle;
  (void)user;
  bool value = false;
  const char *payload = LSMessageGetPayload(message);
  if (payload != NULL &&
      jf_luna_json_bool(payload, "tv.model.supportHDR", &value))
    atomic_store(&hdr, value);
  else
    fprintf(stderr, "supportHDR: %s\n",
            payload != NULL ? payload : "(no reply)");
  return true;
}

void jf_luna_query_hdr(void) {
  memset(&hdr_context, 0, sizeof(hdr_context));
  hdr_context.callback = on_hdr;
  if (!send_call("luna://com.webos.service.config/getConfigs",
                 "{\"configNames\":[\"tv.model.supportHDR\"]}", false,
                 &hdr_context))
    fprintf(stderr, "supportHDR: call failed\n");
}

int jf_luna_hdr(void) { return atomic_load(&hdr); }

bool jf_luna_relaunch_home_key(void) { return atomic_load(&relaunch_home_key); }

static void *run_loop(void *unused) {
  (void)unused;
  g_main_loop_run(loop);
  return NULL;
}

static bool start_loop(void) {
  if (loop != NULL)
    return true;
  loop = g_main_loop_new(NULL, FALSE);
  if (loop == NULL)
    return false;
  if (pthread_create(&loop_thread, NULL, run_loop, NULL) != 0) {
    g_main_loop_unref(loop);
    loop = NULL;
    return false;
  }
  loop_running = true;
  return true;
}

/* One call in flight. The context is the first member, so the callback finds
 * the call whether libhelpers hands it the context or its userdata. A call is
 * not freed from its own callback - libhelpers may still hold the context as
 * it returns - but marked done and reclaimed by a later call. */
typedef struct pending_call {
  HContext context;
  jf_luna_reply_fn reply;
  void *user;
  atomic_bool done;
  struct pending_call *next;
} pending_call;

static pthread_mutex_t calls_lock = PTHREAD_MUTEX_INITIALIZER;
static pending_call *calls;

static bool on_reply(LSHandle *handle, LSMessage *message, void *user) {
  (void)handle;
  pending_call *call = user;
  const char *payload = message != NULL ? LSMessageGetPayload(message) : NULL;
  if (jf_luna_log)
    fprintf(stderr, "luna reply: %s\n", payload != NULL ? payload : "(none)");
  if (call->reply != NULL)
    call->reply(payload, call->user);
  atomic_store(&call->done, true);
  return true;
}

static void reclaim_calls(void) {
  pthread_mutex_lock(&calls_lock);
  for (pending_call **at = &calls; *at != NULL;) {
    pending_call *call = *at;
    if (atomic_load(&call->done)) {
      *at = call->next;
      free(call);
    } else {
      at = &call->next;
    }
  }
  pthread_mutex_unlock(&calls_lock);
}

bool jf_luna_call(const char *uri, const char *payload, jf_luna_reply_fn reply,
                  void *user) {
  if (!start_loop())
    return false;
  reclaim_calls();
  pending_call *call = calloc(1, sizeof(*call));
  if (call == NULL)
    return false;
  call->context.callback = on_reply;
  call->context.userdata = call;
  call->reply = reply;
  call->user = user;
  pthread_mutex_lock(&calls_lock);
  call->next = calls;
  calls = call;
  pthread_mutex_unlock(&calls_lock);
  if (jf_luna_log)
    fprintf(stderr, "luna call: %s %s\n", uri, payload);
  if (!send_call(uri, payload, false, &call->context)) {
    atomic_store(&call->done, true);
    return false;
  }
  return true;
}

bool jf_luna_register_lifecycle(void (*quit)(void), void (*relaunch)(void)) {
  if (on_quit != NULL || on_relaunch != NULL)
    return true;
  on_quit = quit;
  on_relaunch = relaunch;

  /* A subscription, so the reply keeps arriving for the life of the app. */
  memset(&context, 0, sizeof(context));
  context.callback = on_message;
  /* `registerApp`, because appinfo.json declares
   * nativeLifeCycleInterfaceVersion 2. The older `registerNativeApp` is version
   * 1's method and SAM rejects the mismatch with "trying to register via
   * unmatched method with nativeLifeCycleInterfaceVersion". */
  if (!send_call("luna://com.webos.service.applicationmanager/registerApp",
                 "{}", true, &context))
    return false;
  return start_loop();
}

void jf_luna_deinit(void) {
  if (context.callback != NULL && named == NULL) {
    HUnregisterServiceCallback(&context);
    context.callback = NULL;
  }
  if (loop != NULL)
    g_main_loop_quit(loop);
  if (loop_running) {
    pthread_join(loop_thread, NULL);
    loop_running = false;
  }
  if (named != NULL) {
    LSUnregister(named, NULL);
    named = NULL;
  }
  if (loop != NULL) {
    g_main_loop_unref(loop);
    loop = NULL;
  }
  /* The loop is gone, so no answer can still arrive. */
  pthread_mutex_lock(&calls_lock);
  while (calls != NULL) {
    pending_call *call = calls;
    calls = call->next;
    free(call);
  }
  pthread_mutex_unlock(&calls_lock);
}

#endif /* JF_WEBOS */
