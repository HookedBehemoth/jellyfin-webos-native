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
jf_relaunch_reason jf_luna_relaunch_reason(void) { return JF_RELAUNCH_OTHER; }

bool jf_luna_call(const char *uri, const char *payload, jf_luna_reply_fn reply,
                  void *user) {
  (void)uri, (void)payload, (void)reply, (void)user;
  return false;
}

bool jf_luna_subscribe(const char *uri, const char *payload,
                       jf_luna_reply_fn reply, void *user) {
  (void)uri, (void)payload, (void)reply, (void)user;
  return false;
}

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

static bool start_loop(void);

/* One call: `helpers` carries the callback for libhelpers, which wants a
 * context that outlives the call. `multiple` keeps answers coming, for a
 * subscription. */
static bool send_call(const char *uri, const char *payload, bool multiple,
                      HContext *helpers) {
  helpers->multiple = multiple;
  helpers->pub = 1;
  if (HLunaServiceCall(uri, payload, helpers) == 0)
    return true;
  fprintf(stderr, "luna: %s failed: %s\n", uri, helpers_error());
  return false;
}
static pthread_t loop_thread;
static bool loop_running;
static void (*on_quit)(void);
static void (*on_relaunch)(void);
static atomic_int relaunch_reason;

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
    char reason[32] = "";
    jf_luna_json_string(payload, "reason", reason, sizeof(reason));
    atomic_store(&relaunch_reason, strcmp(reason, "hotKey") == 0
                                       ? JF_RELAUNCH_HOME_KEY
                                       : JF_RELAUNCH_OTHER);
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

jf_relaunch_reason jf_luna_relaunch_reason(void) {
  return (jf_relaunch_reason)atomic_load(&relaunch_reason);
}

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
  /* Answered for as long as the app runs, so never reclaimed. */
  bool lasting;
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
    fprintf(stderr, "luna reply: %.300s\n",
            payload != NULL ? payload : "(none)");
  if (call->reply != NULL)
    call->reply(payload, call->user);
  if (!call->lasting)
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

static bool start_call(const char *uri, const char *payload,
                       jf_luna_reply_fn reply, void *user, bool lasting) {
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
  call->lasting = lasting;
  pthread_mutex_lock(&calls_lock);
  call->next = calls;
  calls = call;
  pthread_mutex_unlock(&calls_lock);
  if (jf_luna_log)
    fprintf(stderr, "luna call: %s %s\n", uri, payload);
  if (!send_call(uri, payload, lasting, &call->context)) {
    atomic_store(&call->done, true);
    return false;
  }
  return true;
}

bool jf_luna_call(const char *uri, const char *payload, jf_luna_reply_fn reply,
                  void *user) {
  return start_call(uri, payload, reply, user, false);
}

bool jf_luna_subscribe(const char *uri, const char *payload,
                       jf_luna_reply_fn reply, void *user) {
  return start_call(uri, payload, reply, user, true);
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
  if (context.callback != NULL) {
    HUnregisterServiceCallback(&context);
    context.callback = NULL;
  }
  if (loop != NULL)
    g_main_loop_quit(loop);
  if (loop_running) {
    pthread_join(loop_thread, NULL);
    loop_running = false;
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
