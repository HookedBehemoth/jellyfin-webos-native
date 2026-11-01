/* The TV's transport: libhelpers, through the platform layer's GLib thread. */
#include <stdint.h>

#include "../platform/luna.h"
#include "transport.h"

static void on_reply(const char *payload, void *user) {
  tv_deliver((uint32_t)(uintptr_t)user, payload);
}

bool tv_transport_send(const char *uri, const char *payload, uint32_t tag) {
  return jf_luna_call(uri, payload, on_reply, (void *)(uintptr_t)tag);
}

bool tv_transport_subscribe(const char *uri, const char *payload,
                            uint32_t tag) {
  return jf_luna_subscribe(uri, payload, on_reply, (void *)(uintptr_t)tag);
}
