/*
 * How a request reaches the TV, which is the one part of src/tv that differs
 * between the TV (transport_luna.c, the Luna bus) and a desktop
 * (transport_mock.c, a TV in the process).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Sends `payload` to `uri` ("luna://service/method"). The answer, or NULL for
 * none, goes to tv_deliver with `tag`, on any thread, at any later time. False
 * when it could not be sent; then nothing is delivered. */
bool tv_transport_send(const char *uri, const char *payload, uint32_t tag);
/* Thread-safe; copies `payload`. */
void tv_deliver(uint32_t tag, const char *payload);
