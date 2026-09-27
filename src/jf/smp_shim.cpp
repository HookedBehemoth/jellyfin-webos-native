/*
 * The only C++ in this program.
 *
 * Every function here is a try/catch around one call into libplayerAPIs and
 * nothing more. No JSON is built here, no status string is interpreted here, no
 * state is tracked here beyond the instance itself - all of that is C, in
 * smp_payload.c and player.c, where it can be read and tested without a C++
 * toolchain in the way.
 *
 * The one thing that genuinely needs C++ is the ABI: the constructor and the
 * `std::string` that Feed() returns by value. Confining those to this file is
 * the whole point of it existing.
 */
#include <starfish-media-pipeline/StarfishMediaAPIs.h>

#include <cstring>
#include <new>
#include <string>

#include "smp.h"

namespace {

StarfishMediaAPIs *g_api = nullptr;
char g_error[160] = {0};

void note(const char *what)
{
    std::strncpy(g_error, what, sizeof(g_error) - 1);
    g_error[sizeof(g_error) - 1] = '\0';
}

} // namespace

#define SMP_TRY(expr, fallback)          \
    do {                                 \
        if (g_api == nullptr)            \
            return (fallback);           \
        try {                            \
            g_error[0] = '\0';           \
            return (expr);               \
        } catch (const std::exception &e) { \
            note(e.what());              \
            return (fallback);           \
        } catch (...) {                  \
            note("StarfishMediaAPIs threw"); \
            return (fallback);           \
        }                                \
    } while (0)

extern "C" {

bool smp_open(void)
{
    if (g_api != nullptr)
        return true;
    try {
        g_error[0] = '\0';
        g_api = new StarfishMediaAPIs();
    } catch (const std::exception &e) {
        note(e.what());
        return false;
    } catch (...) {
        note("StarfishMediaAPIs constructor threw");
        return false;
    }
    return true;
}

void smp_close(void)
{
    if (g_api == nullptr)
        return;
    try {
        delete g_api;
    } catch (...) {
        note("StarfishMediaAPIs destructor threw");
    }
    g_api = nullptr;
}

bool smp_is_open(void) { return g_api != nullptr; }

bool smp_load(const char *payload, smp_event_fn *callback)
{
    SMP_TRY(g_api->Load(payload, callback), false);
}
bool smp_unload(void) { SMP_TRY(g_api->Unload(), false); }
bool smp_play(void) { SMP_TRY(g_api->Play(), false); }
bool smp_pause(void) { SMP_TRY(g_api->Pause(), false); }
bool smp_push_eos(void) { SMP_TRY(g_api->pushEOS(), false); }
bool smp_notify_foreground(void) { SMP_TRY(g_api->notifyForeground(), false); }
bool smp_seek(const char *millis) { SMP_TRY(g_api->Seek(millis), false); }
bool smp_flush(const char *payload) { SMP_TRY(g_api->flush(payload), false); }
bool smp_set_play_rate(const char *payload) { SMP_TRY(g_api->SetPlayRate(payload), false); }

int64_t smp_get_current_playtime(void)
{
    SMP_TRY(g_api->getCurrentPlaytime(), -1);
}

bool smp_feed(const char *payload, char *status, size_t status_len)
{
    if (g_api == nullptr)
        return false;
    try {
        g_error[0] = '\0';
        const std::string result = g_api->Feed(payload);
        if (status_len > 0) {
            const size_t n = result.size() < status_len - 1 ? result.size() : status_len - 1;
            std::memcpy(status, result.data(), n);
            status[n] = '\0';
        }
        return true;
    } catch (const std::exception &e) {
        note(e.what());
    } catch (...) {
        note("StarfishMediaAPIs::Feed threw");
    }
    if (status_len > 0)
        status[0] = '\0';
    return false;
}

const char *smp_shim_error(void) { return g_error; }

} // extern "C"
