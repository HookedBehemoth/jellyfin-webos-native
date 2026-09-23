#include "store.h"

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "../platform/os.h"
#include "cfg.h"

/* Artwork budget. The cache is pruned to this at startup, oldest first. Two hundred-odd
 * posters at 240x360 PNG; a library of any size settles here. */
#define CACHE_BUDGET (48u * 1024 * 1024)
#define DEV_ROOT "/tmp/jellyfin-native"

static char root[512] = DEV_ROOT;
static bool installed_app;

const char *jf_store_root(void) { return root; }
bool jf_store_installed(void) { return installed_app; }

static void make_directory(const char *path) { jf_os_mkdir(path); }

static void make_subdirectories(void)
{
    char path[576];
    make_directory(root);
    snprintf(path, sizeof(path), "%s/conf", root);
    make_directory(path);
    snprintf(path, sizeof(path), "%s/cache", root);
    make_directory(path);
}

/* The directory an installed app runs in, or false when this is not one.
 *
 * `/usr/palm/applications/` is the marker rather than the leading component, because a
 * developer-mode install lives under /media/developer/apps and a retail one under
 * /media/cryptofs/apps, both ending in that path. */
#ifdef JF_WEBOS
static bool app_directory(char *out, size_t out_len)
{
    const ssize_t n = readlink("/proc/self/cwd", out, out_len - 1);
    if (n <= 0)
        return false;
    out[n] = '\0';
    if (strstr(out, "/usr/palm/applications/") == NULL)
        return false;
    /* The last component is the app id, which always has a dot in it. */
    const char *slash = strrchr(out, '/');
    return slash != NULL && strchr(slash + 1, '.') != NULL;
}
#else
static bool app_directory(char *out, size_t out_len)
{
    (void)out;
    (void)out_len;
    return false;
}
#endif

static bool writable(void)
{
    char probe[576];
    snprintf(probe, sizeof(probe), "%s/conf/.probe", root);
    const int file = open(probe, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (file < 0)
        return false;
    close(file);
    unlink(probe);
    return true;
}

void jf_store_init(void)
{
    char cwd[512];
    const char *override = getenv("JELLYFIN_STORE");
    if (override != NULL && override[0] != '\0') {
        snprintf(root, sizeof(root), "%s", override);
    } else if (app_directory(cwd, sizeof(cwd))) {
        installed_app = true;
        snprintf(root, sizeof(root), "%s", cwd);
    } else {
        snprintf(root, sizeof(root), "%s", DEV_ROOT);
    }
    make_subdirectories();

    /* An installed app runs as a jail uid that owns none of its files, so its directory is
     * only writable if the package shipped it that way. If an older package did not, fall
     * back rather than fail every write silently - /tmp at least keeps the app working
     * until the next reinstall. */
    if (!writable()) {
        fprintf(stderr, "store: %s is not writable; falling back to /tmp\n", root);
        snprintf(root, sizeof(root), "%s", DEV_ROOT);
        installed_app = false;
        make_subdirectories();
    }
    fprintf(stderr, "store: %s (%s)\n", root, installed_app ? "installed" : "development");
}

/* ------------------------------------------------------------- credentials */

static void credentials_path(char *out, size_t out_len)
{
    snprintf(out, out_len, "%s/conf/credentials", root);
}

void jf_store_save(const jf_credentials *credentials)
{
    char path[576];
    credentials_path(path, sizeof(path));
    jf_arena arena = {0};
    cfg_writer writer;
    cfg_writer_init(&writer, &arena);
    cfg_write_text(&writer, "server", credentials->server);
    cfg_write_text(&writer, "token", credentials->token);
    cfg_write_text(&writer, "user_id", credentials->user_id);
    cfg_write_text(&writer, "user_name", credentials->user_name);
    cfg_write_text(&writer, "password", credentials->password);
    if (!cfg_flush(&writer, path))
      fprintf(stderr, "store: could not save %s\n", path);
    jf_arena_destroy(&arena);
}

bool jf_store_load(jf_credentials *out)
{
    char path[576];
    credentials_path(path, sizeof(path));
    memset(out, 0, sizeof(*out));
    jf_arena arena = {0};
    cfg_reader reader;
    (void)cfg_open(&reader, &arena, path);
    const struct {
      const char *key;
      char *field;
      size_t size;
    } fields[] = {
        {"server", out->server, sizeof(out->server)},
        {"token", out->token, sizeof(out->token)},
        {"user_id", out->user_id, sizeof(out->user_id)},
        {"user_name", out->user_name, sizeof(out->user_name)},
        {"password", out->password, sizeof(out->password)},
    };
    for (cfg_event event; (event = cfg_next(&reader)) != CFG_END;) {
      for (size_t i = 0;
           event == CFG_VALUE && i < sizeof(fields) / sizeof(*fields); i++)
        if (strcmp(reader.key, fields[i].key) == 0)
          snprintf(fields[i].field, fields[i].size, "%s", cfg_text(&reader));
    }
    jf_arena_destroy(&arena);
    return out->server[0] != '\0';
}

void jf_store_forget(void)
{
    char path[576];
    credentials_path(path, sizeof(path));
    unlink(path);
}

/* ------------------------------------------------------------- image cache */

bool jf_store_image_path(char *out, size_t out_len, const char *item, const char *tag,
                         uint32_t width, uint32_t height)
{
    if (tag == NULL || tag[0] == '\0')
        return false;
    const int n = snprintf(out, out_len, "%s/cache/%s-%s-%ux%u.img", root, item, tag, width, height);
    return n > 0 && (size_t)n < out_len;
}

uint8_t *jf_store_read_image(const char *path, size_t *out_size)
{
    const int file = open(path, O_RDONLY);
    if (file < 0)
        return NULL;
    struct stat info;
    if (fstat(file, &info) != 0 || info.st_size <= 0 || info.st_size > 16 * 1024 * 1024) {
        close(file);
        return NULL;
    }
    uint8_t *bytes = malloc((size_t)info.st_size);
    if (bytes == NULL) {
        close(file);
        return NULL;
    }
    size_t filled = 0;
    while (filled < (size_t)info.st_size) {
        const ssize_t n = read(file, bytes + filled, (size_t)info.st_size - filled);
        if (n <= 0)
            break;
        filled += (size_t)n;
    }
    close(file);
    if (filled != (size_t)info.st_size) {
        free(bytes);
        return NULL;
    }
    *out_size = filled;
    return bytes;
}

/* Write via a temporary and rename, because four workers share this directory and a
 * half-written file must never be readable as a whole one. The temporary is named after
 * the final path plus the thread id, so two workers racing on the same image cannot
 * collide either. */
void jf_store_write_image(const char *path, const uint8_t *bytes, size_t size)
{
    char partial[700];
    const int n = snprintf(partial, sizeof(partial), "%s.%lx", path,
                           (unsigned long)pthread_self());
    if (n <= 0 || (size_t)n >= sizeof(partial))
        return;
    const int file = open(partial, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (file < 0)
        return;
    size_t written = 0;
    while (written < size) {
        const ssize_t chunk = write(file, bytes + written, size - written);
        if (chunk <= 0)
            break;
        written += (size_t)chunk;
    }
    close(file);
    if (written != size || rename(partial, path) != 0)
        unlink(partial);
}

typedef struct {
    char name[96];
    off_t size;
    time_t mtime;
} cache_entry;

static int by_mtime(const void *a, const void *b)
{
    const time_t left = ((const cache_entry *)a)->mtime;
    const time_t right = ((const cache_entry *)b)->mtime;
    return left < right ? -1 : left > right ? 1 : 0;
}

void jf_store_prune(void)
{
    char cache[576];
    snprintf(cache, sizeof(cache), "%s/cache", root);
    DIR *dir = opendir(cache);
    if (dir == NULL)
        return;

    cache_entry *entries = NULL;
    size_t count = 0, capacity = 0;
    uint64_t total = 0;
    for (const struct dirent *item = readdir(dir); item != NULL; item = readdir(dir)) {
        const size_t name_len = strlen(item->d_name);
        if (name_len >= sizeof(entries->name))
            continue;
        char path[700];
        if ((size_t)snprintf(path, sizeof(path), "%s/%s", cache, item->d_name) >= sizeof(path))
            continue;
        struct stat info;
        if (stat(path, &info) != 0 || !S_ISREG(info.st_mode))
            continue;
        if (count == capacity) {
            const size_t grown_capacity = capacity ? capacity * 2 : 256;
            cache_entry *grown = realloc(entries, grown_capacity * sizeof(*grown));
            if (grown == NULL)
                break;
            entries = grown;
            capacity = grown_capacity;
        }
        memcpy(entries[count].name, item->d_name, name_len + 1);
        entries[count].size = info.st_size;
        entries[count].mtime = info.st_mtime;
        count++;
        total += (uint64_t)info.st_size;
    }
    closedir(dir);

    if (total > CACHE_BUDGET) {
        qsort(entries, count, sizeof(*entries), by_mtime);
        uint64_t freed = 0;
        for (size_t i = 0; i < count && total - freed > CACHE_BUDGET; i++) {
            char path[700];
            snprintf(path, sizeof(path), "%s/%s", cache, entries[i].name);
            if (unlink(path) == 0)
                freed += (uint64_t)entries[i].size;
        }
        fprintf(stderr, "store: pruned %llu KiB of artwork cache\n",
                (unsigned long long)(freed / 1024));
    }
    free(entries);
}
