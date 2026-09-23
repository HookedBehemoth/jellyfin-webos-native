/*
 * Where this app keeps things on disk: the access token, and the artwork cache.
 *
 * An installed webOS app writes *inside its own installed directory* - the native
 * homebrew apps on this TV all do, and the packaged directory is world-writable for
 * exactly that reason (see WRITABLE_DIRS in cmake/WebOSPackage.cmake). So `conf/` holds
 * the token and `cache/` the artwork, matching Moonlight; `.cache/` is left to whatever
 * libraries want it.
 *
 * Finding that directory needs no environment, which matters because SAM provides none.
 * An installed app runs with its own directory as the working directory, so
 * /proc/self/cwd is the answer - and it is also how "installed" is told from
 * "development". Run any other way the root is /tmp/jellyfin-native, where a reboot
 * clears it and nothing accumulates in the source tree. $JELLYFIN_STORE overrides both.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Resolve the root and make sure conf/ and cache/ exist. Call once, before any worker
 * thread runs. */
void jf_store_init(void);
const char *jf_store_root(void);
bool jf_store_installed(void);

/* Kept as key=value lines in conf/credentials.
 *
 * The password is stored because Jellyfin invalidates a device's previous token
 * whenever that device signs in again, so a stored token alone eventually stops
 * working and leaves the user typing on a remote. With it, a 401 is recoverable
 * in the background.
 *
 * It is therefore a plaintext password in the app's own storage, which other
 * apps on the TV cannot read. Quick Connect stores no password and simply signs
 * out on a 401. */
typedef struct {
    char server[512];
    char token[256];
    char user_id[64];
    char user_name[128];
    char password[128];
} jf_credentials;

void jf_store_save(const jf_credentials *credentials);
/* False when no server has been selected. The access token may be empty after sign-out;
 * callers can retain the server and ask for credentials again. */
bool jf_store_load(jf_credentials *out);
void jf_store_forget(void);

/* Cache file name for one image.
 *
 * The tag is Jellyfin's own image tag, a hash of the image content: when the artwork
 * changes the tag changes, so a changed image is a different file and a stale one can
 * never be served. That is what makes this cache need no revalidation request at all - a
 * hit costs no network. The size is in the key because the same image is fetched at
 * whatever size the screen asked for, and the server does the scaling.
 *
 * False for an untagged image: not cacheable, always fetch. */
bool jf_store_image_path(char *out, size_t out_len, const char *item, const char *tag,
                         uint32_t width, uint32_t height);

/* NULL on any failure; the caller free()s a successful read. */
uint8_t *jf_store_read_image(const char *path, size_t *out_size);
void jf_store_write_image(const char *path, const uint8_t *bytes, size_t size);

/* Keep the cache under budget, deleting least-recently-modified first. Runs at startup,
 * on the main thread, before anything reads it.
 *
 * Deliberately not an eviction policy with bookkeeping: the cache is disposable, a sweep
 * at launch is cheap against a few hundred files, and the alternative - an index to keep
 * consistent across four writer threads - is a lot of machinery to avoid re-downloading a
 * poster. */
void jf_store_prune(void);
