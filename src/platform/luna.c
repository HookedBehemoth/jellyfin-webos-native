#include "luna.h"

bool jf_luna_log;

#ifndef JF_WEBOS

/* Off-device there is no Luna bus and nothing asking the app to close, which is not an
 * error - it is the whole reason registerLifecycle returning false is not fatal. */
bool jf_luna_register_lifecycle(void (*quit)(void), void (*relaunch)(void))
{
    (void)quit;
    (void)relaunch;
    return false;
}

void jf_luna_deinit(void) {}

#else

#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <webos-helpers/libhelpers.h>

static HContext context;
static GMainLoop *loop;
static pthread_t loop_thread;
static bool loop_running;
static void (*on_quit)(void);
static void (*on_relaunch)(void);

static bool on_message(LSHandle *handle, LSMessage *message, void *user)
{
    (void)handle;
    (void)user;
    const char *payload = HLunaServiceMessage(message);
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
    /* A minimised app is still running, so opening it from the launcher is a relaunch
     * rather than a start: nothing else will raise the window. */
    if (strcmp(event, "relaunch") == 0 && on_relaunch != NULL)
        on_relaunch();
    return true;
}

static void *run_loop(void *unused)
{
    (void)unused;
    g_main_loop_run(loop);
    return NULL;
}

bool jf_luna_register_lifecycle(void (*quit)(void), void (*relaunch)(void))
{
    if (loop != NULL)
        return true;
    on_quit = quit;
    on_relaunch = relaunch;

    /* A subscription, so the reply keeps arriving for the life of the app. */
    memset(&context, 0, sizeof(context));
    context.callback = on_message;
    context.multiple = 1;
    context.pub = 1;
    /* `registerApp`, because appinfo.json declares nativeLifeCycleInterfaceVersion 2. The
     * older `registerNativeApp` is version 1's method and SAM rejects the mismatch with
     * "trying to register via unmatched method with nativeLifeCycleInterfaceVersion". */
    if (HLunaServiceCall("luna://com.webos.service.applicationmanager/registerApp", "{}",
                         &context) != 0)
        return false;

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

void jf_luna_deinit(void)
{
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
}

#endif /* JF_WEBOS */
