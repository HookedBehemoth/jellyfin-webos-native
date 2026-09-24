#include "api.h"

#include <curl/curl.h>
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../platform/os.h"
#include "blurhash.h"

/* ------------------------------------------------------------------ model */

static bool same(const char *a, const char *b) { return a != NULL && strcmp(a, b) == 0; }

bool jf_item_has_poster(const jf_item *item)
{
    return item->primary_image_tag != NULL && item->primary_image_tag[0] != '\0';
}

static bool uses_series_poster(const jf_item *item)
{
    return same(item->type, "Episode") || (same(item->type, "Season") && !jf_item_has_poster(item));
}

const char *jf_item_poster_id(const jf_item *item)
{
    if (uses_series_poster(item) && item->series_id != NULL)
        return item->series_id;
    return item->id != NULL ? item->id : "";
}

const char *jf_item_poster_tag(const jf_item *item)
{
    if (uses_series_poster(item) && item->series_id != NULL)
        return item->series_primary_image_tag != NULL ? item->series_primary_image_tag : "";
    return item->primary_image_tag != NULL ? item->primary_image_tag : "";
}

const char *jf_item_poster_blurhash(const jf_item *item) {
  if (uses_series_poster(item) && item->series_id != NULL)
    return item->series_primary_blurhash;
  return item->primary_blurhash;
}

bool jf_item_is_folder(const jf_item *item)
{
    return same(item->type, "Series") || same(item->type, "Season") ||
           same(item->type, "BoxSet") || same(item->type, "CollectionFolder");
}

uint32_t jf_item_minutes(const jf_item *item)
{
    return (uint32_t)(item->run_time_ticks / (10000000ull * 60));
}

bool jf_item_finished(const jf_item *item)
{
    if (item->played)
        return true;
    if (jf_item_is_folder(item) && item->has_unplayed_item_count && item->unplayed_item_count == 0)
        return true;
    return same(item->type, "Season") && item->has_child_count && item->child_count == 0;
}

float jf_item_progress(const jf_item *item)
{
    if (item->has_played_percentage)
        return (float)item->played_percentage;
    if (item->playback_position_ticks == 0 || item->run_time_ticks == 0)
        return 0;
    return (float)item->playback_position_ticks / (float)item->run_time_ticks * 100.0f;
}

/* ------------------------------------------------------------- credentials */

void jf_session_authorization(const jf_session *session, char *out, size_t out_len)
{
    snprintf(out, out_len,
             "MediaBrowser Client=\"%s\", Device=\"webOS TV\", DeviceId=\"%s\", Version=\"%s\", "
             "Token=\"%s\"",
             JF_CLIENT_NAME, session->device_id, JF_CLIENT_VERSION, session->token);
}

void jf_session_device_id(jf_session *session)
{
    char host[64] = "webos";
    jf_os_hostname(host, sizeof(host));
    /* FNV-1a over the host name and the client name: stable across launches, and not a
     * value anyone needs to reverse. */
    uint64_t hash = 1469598103934665603ull;
    for (const char *p = host; *p != '\0'; p++)
        hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
    for (const char *p = JF_CLIENT_NAME; *p != '\0'; p++)
        hash = (hash ^ (unsigned char)*p) * 1099511628211ull;
    snprintf(session->device_id, sizeof(session->device_id), "%016llx",
             (unsigned long long)hash);
}

void jf_session_save(const jf_session *session)
{
    jf_credentials credentials;
    memset(&credentials, 0, sizeof(credentials));
    snprintf(credentials.server, sizeof(credentials.server), "%s", session->url);
    snprintf(credentials.token, sizeof(credentials.token), "%s", session->token);
    snprintf(credentials.user_id, sizeof(credentials.user_id), "%s", session->user_id);
    snprintf(credentials.user_name, sizeof(credentials.user_name), "%s", session->user_name);
    snprintf(credentials.password, sizeof(credentials.password), "%s", session->password);
    jf_store_save(&credentials);
}

bool jf_session_load(jf_session *session)
{
    jf_credentials credentials;
    if (!jf_store_load(&credentials))
        return false;
    snprintf(session->url, sizeof(session->url), "%s", credentials.server);
    snprintf(session->token, sizeof(session->token), "%s", credentials.token);
    snprintf(session->user_id, sizeof(session->user_id), "%s", credentials.user_id);
    snprintf(session->user_name, sizeof(session->user_name), "%s", credentials.user_name);
    snprintf(session->password, sizeof(session->password), "%s", credentials.password);
    return session->token[0] != '\0';
}

void jf_api_init(void) {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  jf_image_report_version();
  jf_store_init();
  jf_store_prune();
}

/* `base` without a trailing slash, so every URL below is "{base}/Path". */
const char *jf_session_base(const jf_session *session, char *out, size_t out_len)
{
    snprintf(out, out_len, "%s", session->url);
    const size_t length = strlen(out);
    if (length > 0 && out[length - 1] == '/')
        out[length - 1] = '\0';
    return out;
}

/* Percent-encode a query value. Library names and search text reach the URL, and a space
 * or `&` in one would otherwise build a different request. */
void jf_url_escape(char *out, size_t out_len, const char *value)
{
    size_t at = 0;
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0'; p++) {
        const bool safe = (*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
                          (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.' ||
                          *p == '~';
        if (safe) {
            if (at + 2 > out_len)
                break;
            out[at++] = (char)*p;
        } else {
            if (at + 4 > out_len)
                break;
            at += (size_t)snprintf(out + at, out_len - at, "%%%02X", *p);
        }
    }
    out[at < out_len ? at : out_len - 1] = '\0';
}

/* -------------------------------------------------------------------- http */

typedef enum {
    HTTP_OK,
    HTTP_REQUEST_FAILED,
    HTTP_UNAUTHORIZED,
    HTTP_CLIENT_ERROR,
    HTTP_SERVER_ERROR,
    HTTP_STATUS,
} http_result;

typedef struct {
    uint8_t *bytes;
    size_t size;
    size_t capacity;
} response;

static size_t collect(char *data, size_t size, size_t count, void *user)
{
    response *out = user;
    const size_t chunk = size * count;
    if (out->size + chunk > out->capacity) {
        size_t capacity = out->capacity ? out->capacity * 2 : 16384;
        while (capacity < out->size + chunk)
            capacity *= 2;
        uint8_t *grown = realloc(out->bytes, capacity + 1);
        if (grown == NULL)
            return 0;
        out->bytes = grown;
        out->capacity = capacity;
    }
    memcpy(out->bytes + out->size, data, chunk);
    out->size += chunk;
    out->bytes[out->size] = '\0';
    return chunk;
}

static const char *json_string(jf_arena *arena, json_object *object, const char *key);
static http_result parse_playback_info(jf_arena *arena, const char *body,
                                       jf_playback_info *out);

/* One request. The body is copied into `arena`; `payload` carries the JSON for a POST
 * (an empty string still means POST, as Quick Connect's Initiate requires). */
static http_result send_request(CURL *curl, jf_arena *arena, const jf_session *session,
                                const char *method, const char *url, const char *payload,
                                uint8_t **out_body, size_t *out_size)
{
    char authorization[768];
    jf_session_authorization(session, authorization, sizeof(authorization));
    char header[832];
    snprintf(header, sizeof(header), "Authorization: %s", authorization);

    struct curl_slist *headers = curl_slist_append(NULL, header);
    if (payload != NULL)
        headers = curl_slist_append(headers, "Content-Type: application/json");

    response body = {NULL, 0, 0};
    fprintf(stderr, "HTTP %s %s\n", method, url);
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, collect);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 8L);
    curl_easy_setopt(curl, CURLOPT_POSTREDIR, CURL_REDIR_POST_ALL);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (payload != NULL) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    }

    const CURLcode code = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);

    if (code != CURLE_OK) {
        fprintf(stderr, "HTTP %s %s: %s\n", method, url, curl_easy_strerror(code));
        free(body.bytes);
        return HTTP_REQUEST_FAILED;
    }
    if (status < 200 || status >= 300) {
        fprintf(stderr, "HTTP %s %s -> %ld\n", method, url, status);
        free(body.bytes);
        if (status == 401 || status == 403)
            return HTTP_UNAUTHORIZED;
        if (status >= 400 && status < 500)
            return HTTP_CLIENT_ERROR;
        if (status >= 500 && status < 600)
            return HTTP_SERVER_ERROR;
        return HTTP_STATUS;
    }

    /* Into the arena, so the caller frees nothing. */
    uint8_t *copy = jf_arena_alloc(arena, body.size + 1);
    if (copy == NULL) {
        free(body.bytes);
        return HTTP_REQUEST_FAILED;
    }
    if (body.size > 0)
        memcpy(copy, body.bytes, body.size);
    copy[body.size] = '\0';
    free(body.bytes);
    *out_body = copy;
    *out_size = body.size;
    return HTTP_OK;
}

static http_result probe_server(CURL *curl, jf_arena *arena, const jf_session *session,
                                const char *address, jf_discovered *out)
{
    jf_session probe = *session;
    snprintf(probe.url, sizeof(probe.url), "%s", address);
    char base[512];
    char url[1024];
    snprintf(url, sizeof(url), "%s/System/Info/Public",
             jf_session_base(&probe, base, sizeof(base)));
    uint8_t *body = NULL;
    size_t size = 0;
    const http_result result = send_request(curl, arena, &probe, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
        return result;
    json_object *root = json_tokener_parse((const char *)body);
    if (root == NULL)
        return HTTP_STATUS;
    out->address = jf_arena_strdup(arena, probe.url);
    out->name = json_string(arena, root, "ServerName");
    out->id = json_string(arena, root, "Id");
    if (out->name == NULL || out->name[0] == '\0')
        out->name = out->address;
    json_object_put(root);
    return HTTP_OK;
}

/* ------------------------------------------------------------------- json */

static const char *json_string(jf_arena *arena, json_object *object, const char *key)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        json_object_get_type(value) != json_type_string)
        return NULL;
    return jf_arena_strdup(arena, json_object_get_string(value));
}

static bool json_number(json_object *object, const char *key, double *out)
{
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value))
        return false;
    const json_type type = json_object_get_type(value);
    if (type != json_type_int && type != json_type_double)
        return false;
    *out = json_object_get_double(value);
    return true;
}

static bool json_flag(json_object *object, const char *key)
{
    json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value) && json_object_get_boolean(value);
}

/* BackdropImageTags[0], or the parent's - the only backdrop this UI draws. */
static const char *json_first_tag(jf_arena *arena, json_object *object, const char *key)
{
    json_object *array = NULL;
    if (!json_object_object_get_ex(object, key, &array) ||
        json_object_get_type(array) != json_type_array || json_object_array_length(array) == 0)
        return NULL;
    json_object *first = json_object_array_get_idx(array, 0);
    if (json_object_get_type(first) != json_type_string)
        return NULL;
    return jf_arena_strdup(arena, json_object_get_string(first));
}

/* ImageBlurHashes is {type: {tag: hash}}, and carries the series' and parent's
 * tags too. */
static const char *json_blurhash(jf_arena *arena, json_object *object,
                                 const char *type, const char *tag) {
  json_object *hashes = NULL, *of_type = NULL;
  if (tag == NULL ||
      !json_object_object_get_ex(object, "ImageBlurHashes", &hashes) ||
      !json_object_object_get_ex(hashes, type, &of_type))
    return NULL;
  return json_string(arena, of_type, tag);
}

static void parse_item(jf_arena *arena, json_object *object, jf_item *out)
{
    memset(out, 0, sizeof(*out));
    out->id = json_string(arena, object, "Id");
    out->name = json_string(arena, object, "Name");
    out->type = json_string(arena, object, "Type");
    out->collection_type = json_string(arena, object, "CollectionType");
    out->overview = json_string(arena, object, "Overview");
    out->premiere_date = json_string(arena, object, "PremiereDate");
    out->end_date = json_string(arena, object, "EndDate");
    out->status = json_string(arena, object, "Status");
    out->official_rating = json_string(arena, object, "OfficialRating");
    out->series_name = json_string(arena, object, "SeriesName");
    out->series_id = json_string(arena, object, "SeriesId");
    out->series_primary_image_tag = json_string(arena, object, "SeriesPrimaryImageTag");
    out->parent_backdrop_item_id = json_string(arena, object, "ParentBackdropItemId");
    out->backdrop_image_tag = json_first_tag(arena, object, "BackdropImageTags");
    out->parent_backdrop_image_tag = json_first_tag(arena, object, "ParentBackdropImageTags");

    double number = 0;
    if (json_number(object, "ProductionYear", &number))
        out->production_year = (uint32_t)number;
    if (json_number(object, "CommunityRating", &number))
        out->community_rating = number;
    if (json_number(object, "RunTimeTicks", &number))
        out->run_time_ticks = (uint64_t)number;
    if (json_number(object, "IndexNumber", &number))
        out->index_number = (uint32_t)number;
    if (json_number(object, "ParentIndexNumber", &number))
        out->parent_index_number = (uint32_t)number;
    if ((out->has_child_count = json_number(object, "ChildCount", &number)))
        out->child_count = (uint32_t)number;

    json_object *tags = NULL;
    if (json_object_object_get_ex(object, "ImageTags", &tags))
        out->primary_image_tag = json_string(arena, tags, "Primary");
    out->primary_blurhash =
        json_blurhash(arena, object, "Primary", out->primary_image_tag);
    out->series_primary_blurhash =
        json_blurhash(arena, object, "Primary", out->series_primary_image_tag);

    json_object *user_data = NULL;
    if (json_object_object_get_ex(object, "UserData", &user_data)) {
        if ((out->has_played_percentage = json_number(user_data, "PlayedPercentage", &number)))
            out->played_percentage = number;
        if (json_number(user_data, "PlaybackPositionTicks", &number))
            out->playback_position_ticks = (uint64_t)number;
        out->played = json_flag(user_data, "Played");
        if ((out->has_unplayed_item_count = json_number(user_data, "UnplayedItemCount", &number)))
            out->unplayed_item_count = (uint32_t)number;
    }
}

static bool parse_item_list(jf_arena *arena, const char *body, jf_item_list *out)
{
    json_object *root = json_tokener_parse(body);
    if (root == NULL)
        return false;
    memset(out, 0, sizeof(*out));

    json_object *items = NULL;
    if (json_object_object_get_ex(root, "Items", &items) &&
        json_object_get_type(items) == json_type_array) {
        const size_t count = json_object_array_length(items);
        out->items = jf_arena_alloc(arena, count * sizeof(jf_item));
        if (out->items == NULL) {
            json_object_put(root);
            return false;
        }
        for (size_t i = 0; i < count; i++)
            parse_item(arena, json_object_array_get_idx(items, i), &out->items[i]);
        out->count = count;
    }
    double number = 0;
    if (json_number(root, "TotalRecordCount", &number))
        out->total_record_count = (uint32_t)number;
    if (json_number(root, "StartIndex", &number))
        out->start_index = (uint32_t)number;
    json_object_put(root);
    return true;
}

/* -------------------------------------------------------------------- urls */

void jf_transcode_url(const jf_session *session, const char *id, char *out, size_t out_len)
{
    /* MPEG-TS lets Jellyfin remux compatible video instead of re-encoding it; the player
     * demuxes its packets locally.
     *
     * Stream copy is deliberately disabled as well as the codecs named. This path is
     * selected precisely because the source is outside the hardware decoder's limits, so a
     * copied H.264 stream can still be High 10, 4:2:2, or above the level the decoder
     * accepts. The h264-* options are Jellyfin's codec profile constraints; generic
     * `profile` and `level` alone do not constrain all server versions' encoder choice.
     *
     * The bitrate has to be stated. Left out, the server picks a default meant for a
     * client that declared no capabilities, and 1080p arrives visibly soft. */
    char base[512];
    snprintf(out, out_len,
             "%s/Videos/%s/stream.ts?videoCodec=h264&audioCodec=mp3"
             "&allowVideoStreamCopy=false&allowAudioStreamCopy=false"
             "&maxVideoBitDepth=8&profile=high&level=41"
             "&h264-profile=high&h264-level=41&h264-videobitdepth=8&h264-rangetype=SDR"
             "&requireAvc=true&maxAudioChannels=2&transcodingMaxAudioChannels=2"
             /* Some server releases only honour the copy-disabling flags after a transcode
              * reason is supplied. Without it, an unsupported H.264 source may be remuxed
              * again and its seek request accepted but ignored by the live response. */
             "&enableAutoStreamCopy=false&transcodeReasons=VideoCodecNotSupported"
             "&videoBitRate=20000000&audioBitRate=192000&api_key=%s",
             jf_session_base(session, base, sizeof(base)), id, session->token);
}

void jf_stream_url(const jf_session *session, const char *id, char *out, size_t out_len)
{
    /* static=true is the original file, byte ranges and all, which is what makes seeking
     * work: a server-side remux is a live stream with no Content-Length, so neither
     * av_seek_frame nor StartTimeTicks moves it. */
    char base[512];
    snprintf(out, out_len, "%s/Videos/%s/stream?static=true&api_key=%s",
             jf_session_base(session, base, sizeof(base)), id, session->token);
}

void jf_subtitle_url(const jf_session *session, const char *id,
                     const char *media_source_id, int index, char *out,
                     size_t out_len) {
  char base[512];
  snprintf(out, out_len, "%s/Videos/%s/%s/Subtitles/%d/0/Stream.ass?api_key=%s",
           jf_session_base(session, base, sizeof(base)), id, media_source_id,
           index, session->token);
}

/* --------------------------------------------------------------- discovery */

/* Jellyfin answers a UDP broadcast on 7359 with one JSON datagram per server. */
static bool discover(jf_arena *arena, jf_discovered **out, size_t *out_count) {
  static const char probe[] = "who is JellyfinServer?";
  const jf_os_socket socket_fd =
      jf_os_udp_broadcast(7359, probe, sizeof(probe) - 1, 1000);
  if (socket_fd < 0)
    return false;

  /* Three timeouts, not one: a second server answering late is worth three
   * seconds of a worker thread, and a lone reply usually lands in the first 20
   * ms. */
  jf_discovered *found = jf_arena_alloc(arena, 16 * sizeof(*found));
  size_t count = 0;
  char datagram[2048];
  for (int quiet = 0; quiet < 3 && count < 16;) {
    const ssize_t n = jf_os_udp_recv(socket_fd, datagram, sizeof(datagram) - 1);
    if (n <= 0) {
      quiet++;
      continue;
    }
    datagram[n] = '\0';
    json_object *root = json_tokener_parse(datagram);
    if (root == NULL)
      continue;
    jf_discovered server = {json_string(arena, root, "Address"),
                            json_string(arena, root, "Name"),
                            json_string(arena, root, "Id")};
    json_object_put(root);
    bool seen = false;
    for (size_t i = 0; i < count && !seen; i++)
      seen = same(found[i].id, server.id != NULL ? server.id : "");
    if (!seen && found != NULL)
      found[count++] = server;
  }
  jf_os_udp_close(socket_fd);
  *out = found;
  *out_count = count;
  return true;
}

/* ------------------------------------------------------------------- calls */

static http_result get_list(CURL *curl, jf_arena *arena, const jf_session *session,
                            const char *url, jf_item_list *out)
{
    uint8_t *body = NULL;
    size_t size = 0;
    const http_result result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
        return result;
    return parse_item_list(arena, (const char *)body, out) ? HTTP_OK : HTTP_STATUS;
}

/* Jellyfin sends the authentication payload as JSON, so the two values have to be escaped
 * rather than pasted: a password may legitimately contain a quote or a backslash. */
static void json_quote(char *out, size_t out_len, const char *value)
{
    size_t at = 0;
    if (out_len == 0)
        return;
    out[at++] = '"';
    for (const unsigned char *p = (const unsigned char *)value; *p != '\0' && at + 8 < out_len; p++) {
        switch (*p) {
        case '"': out[at++] = '\\'; out[at++] = '"'; break;
        case '\\': out[at++] = '\\'; out[at++] = '\\'; break;
        case '\n': out[at++] = '\\'; out[at++] = 'n'; break;
        case '\r': out[at++] = '\\'; out[at++] = 'r'; break;
        case '\t': out[at++] = '\\'; out[at++] = 't'; break;
        default:
            if (*p < 0x20)
                at += (size_t)snprintf(out + at, out_len - at, "\\u%04x", *p);
            else
                out[at++] = (char)*p;
        }
    }
    out[at++] = '"';
    out[at] = '\0';
}

static http_result parse_auth(jf_arena *arena, const char *body, jf_auth *out)
{
    json_object *root = json_tokener_parse(body);
    if (root == NULL)
        return HTTP_STATUS;
    memset(out, 0, sizeof(*out));
    out->access_token = json_string(arena, root, "AccessToken");
    json_object *user = NULL;
    if (json_object_object_get_ex(root, "User", &user)) {
        out->user_id = json_string(arena, user, "Id");
        out->user_name = json_string(arena, user, "Name");
    }
    json_object_put(root);
    return HTTP_OK;
}

static http_result parse_quick(jf_arena *arena, const char *body, jf_quick_connect *out)
{
    json_object *root = json_tokener_parse(body);
    if (root == NULL)
        return HTTP_STATUS;
    memset(out, 0, sizeof(*out));
    out->secret = json_string(arena, root, "Secret");
    out->code = json_string(arena, root, "Code");
    out->authenticated = json_flag(root, "Authenticated");
    json_object_put(root);
    return HTTP_OK;
}

/* Artwork, decoded to RGB8.
 *
 * Three things happen here, in order: the cache is consulted, the request is made only on
 * a miss, and the encoded bytes are cached before decoding.
 *
 * The cache key is Jellyfin's image tag, a hash of the image, so a hit needs no
 * revalidation round trip - changed artwork has a different tag and therefore a different
 * file. (The server offers no ETag on images and ignores If-None-Match; it does honour
 * If-Modified-Since, which is the fallback if a tagless image ever needs caching. Today an
 * untagged image is simply not cached.)
 *
 * fillWidth/fillHeight is the server's sizing hint, not a contract - it answers 204x300 or
 * 534x300 depending on the source art - so the caller must cope with whatever comes back.
 * format=Png is asked for because that is the one decoder with the same ABI on the TV and
 * on a development machine; see image.h. */
static http_result artwork(CURL *curl, jf_arena *arena, const jf_session *session,
                           const char *id, const char *tag, uint32_t width, uint32_t height,
                           jf_image_kind kind, jf_image *out)
{
    char cache_id[560];
    if (kind == JF_IMAGE_PRIMARY)
        snprintf(cache_id, sizeof(cache_id), "%s", id);
    else
        snprintf(cache_id, sizeof(cache_id), "%s-backdrop", id);

    char path[768];
    const bool cacheable = jf_store_image_path(path, sizeof(path), cache_id, tag, width, height);
    if (cacheable) {
        size_t size = 0;
        uint8_t *bytes = jf_store_read_image(path, &size);
        if (bytes != NULL) {
            const bool decoded = jf_image_decode(bytes, size, out);
            free(bytes);
            if (decoded)
                return HTTP_OK;
        }
    }

    char base[512];
    char escaped_id[256];
    char escaped_tag[256];
    jf_url_escape(escaped_id, sizeof(escaped_id), id);
    jf_url_escape(escaped_tag, sizeof(escaped_tag), tag != NULL ? tag : "");
    char url[1024];
    int n = snprintf(url, sizeof(url), "%s/Items/%s/Images/%s?fillWidth=%u&fillHeight=%u&format=Png",
                     jf_session_base(session, base, sizeof(base)), escaped_id,
                     kind == JF_IMAGE_PRIMARY ? "Primary" : "Backdrop/0", width, height);
    /* The tag makes the URL change when the image does, which is what lets any cache in
     * between - ours, or a proxy - treat it as immutable. */
    if (n > 0 && escaped_tag[0] != '\0')
        snprintf(url + n, sizeof(url) - (size_t)n, "&tag=%s", escaped_tag);

    uint8_t *body = NULL;
    size_t size = 0;
    const http_result result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
        return result;
    if (cacheable)
        jf_store_write_image(path, body, size);
    return jf_image_decode(body, size, out) ? HTTP_OK : HTTP_STATUS;
}

/* Decodes the page's blurhashes on this worker, into rows the UI uploads as one
 * piece. The episode list shows 16:9 stills; everything else is a 2:3 poster.
 */
static void decode_blurs(jf_task *task) {
  const bool still = task->job == JF_JOB_EPISODES || task->job == JF_JOB_VIEWS;
  const uint8_t width = still ? JF_BLUR_CELL : JF_BLUR_CELL * 2 / 3;
  const uint8_t height = still ? JF_BLUR_CELL * 9 / 16 : JF_BLUR_CELL;
  const size_t row_bytes =
      (size_t)JF_BLUR_COLUMNS * JF_BLUR_CELL * JF_BLUR_CELL * 3;
  const size_t stride = (size_t)JF_BLUR_COLUMNS * JF_BLUR_CELL * 3;
  uint8_t cell[JF_BLUR_CELL * JF_BLUR_CELL * 3];
  for (size_t i = 0; i < task->list.count; i++) {
    jf_item *item = &task->list.items[i];
    const char *hash =
        still ? item->primary_blurhash : jf_item_poster_blurhash(item);
    if (!jf_blurhash_decode(hash, width, height, cell))
      continue;
    if (task->blur == NULL) {
      task->blur_rows = (uint32_t)((task->list.count + JF_BLUR_COLUMNS - 1) /
                                   JF_BLUR_COLUMNS);
      task->blur = jf_arena_alloc(&task->arena, task->blur_rows * row_bytes);
      if (task->blur == NULL)
        return;
    }
    uint8_t *origin = task->blur + i / JF_BLUR_COLUMNS * row_bytes +
                      i % JF_BLUR_COLUMNS * JF_BLUR_CELL * 3;
    for (uint8_t y = 0; y < height; y++)
      memcpy(origin + y * stride, cell + (size_t)y * width * 3,
             (size_t)width * 3);
    item->blur_cell = (uint32_t)i;
    item->blur_width = width;
    item->blur_height = height;
  }
}

static http_result execute(CURL *curl, const jf_session *session, jf_task *task)
{
    jf_arena *arena = &task->arena;
    char base[512];
    char url[1024];
    char payload[1200];
    uint8_t *body = NULL;
    size_t size = 0;
    http_result result;

    switch (task->job) {
    case JF_JOB_DISCOVER:
        return discover(arena, &task->servers, &task->server_count) ? HTTP_OK : HTTP_REQUEST_FAILED;

    case JF_JOB_PROBE:
        return probe_server(curl, arena, session, task->a, &task->server);

    case JF_JOB_LOGIN: {
        char user[520], password[520];
        json_quote(user, sizeof(user), task->a);
        json_quote(password, sizeof(password), task->b);
        snprintf(payload, sizeof(payload), "{\"Username\":%s,\"Pw\":%s}", user, password);
        snprintf(url, sizeof(url), "%s/Users/AuthenticateByName",
                 jf_session_base(session, base, sizeof(base)));
        result = send_request(curl, arena, session, "POST", url, payload, &body, &size);
        return result != HTTP_OK ? result : parse_auth(arena, (const char *)body, &task->auth);
    }

    case JF_JOB_QUICK_INITIATE:
        snprintf(url, sizeof(url), "%s/QuickConnect/Initiate",
                 jf_session_base(session, base, sizeof(base)));
        result = send_request(curl, arena, session, "POST", url, "", &body, &size);
        return result != HTTP_OK ? result : parse_quick(arena, (const char *)body, &task->quick);

    case JF_JOB_QUICK_POLL: {
        char secret[256];
        jf_url_escape(secret, sizeof(secret), task->a);
        snprintf(url, sizeof(url), "%s/QuickConnect/Connect?secret=%s",
                 jf_session_base(session, base, sizeof(base)), secret);
        result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
        return result != HTTP_OK ? result : parse_quick(arena, (const char *)body, &task->quick);
    }

    case JF_JOB_QUICK_AUTHENTICATE: {
        char secret[520];
        json_quote(secret, sizeof(secret), task->a);
        snprintf(payload, sizeof(payload), "{\"Secret\":%s}", secret);
        snprintf(url, sizeof(url), "%s/Users/AuthenticateWithQuickConnect",
                 jf_session_base(session, base, sizeof(base)));
        result = send_request(curl, arena, session, "POST", url, payload, &body, &size);
        return result != HTTP_OK ? result : parse_auth(arena, (const char *)body, &task->auth);
    }

    case JF_JOB_VIEWS:
        snprintf(url, sizeof(url), "%s/UserViews?userId=%s&limit=256",
                 jf_session_base(session, base, sizeof(base)), session->user_id);
        return get_list(curl, arena, session, url, &task->list);

    case JF_JOB_RESUME:
        snprintf(url, sizeof(url), "%s/UserItems/Resume?userId=%s&limit=24&fields=Overview",
                 jf_session_base(session, base, sizeof(base)), session->user_id);
        return get_list(curl, arena, session, url, &task->list);

    case JF_JOB_NEXT_UP:
        snprintf(url, sizeof(url), "%s/Shows/NextUp?userId=%s&limit=24&fields=Overview",
                 jf_session_base(session, base, sizeof(base)), session->user_id);
        return get_list(curl, arena, session, url, &task->list);

    case JF_JOB_CHILDREN: {
        /* sortBy=SortName keeps paging stable, which is the whole reason a virtual grid
         * can ask for the window [start, start+limit). */
        char parent[256];
        jf_url_escape(parent, sizeof(parent), task->a);
        snprintf(url, sizeof(url),
                 "%s/Users/%s/Items?parentId=%s&startIndex=%u&limit=%u&recursive=true"
                 "&sortBy=SortName&sortOrder=Ascending&includeItemTypes=Movie,Series"
                 "&fields=Overview,ChildCount&imageTypeLimit=1&enableImageTypes=Primary",
                 jf_session_base(session, base, sizeof(base)), session->user_id, parent,
                 task->start, task->limit);
        return get_list(curl, arena, session, url, &task->list);
    }

    case JF_JOB_ITEM: {
        char id[256];
        jf_url_escape(id, sizeof(id), task->a);
        snprintf(url, sizeof(url), "%s/Items/%s?userId=%s",
                 jf_session_base(session, base, sizeof(base)), id, session->user_id);
        result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
        if (result != HTTP_OK)
            return result;
        json_object *root = json_tokener_parse((const char *)body);
        if (root == NULL)
            return HTTP_STATUS;
        parse_item(arena, root, &task->one);
        json_object_put(root);
        return HTTP_OK;
    }

    case JF_JOB_SEASONS: {
        char series[256];
        jf_url_escape(series, sizeof(series), task->a);
        snprintf(url, sizeof(url),
                 "%s/Shows/%s/Seasons?userId=%s&fields=ChildCount,Overview&enableImages=true"
                 "&enableUserData=true",
                 jf_session_base(session, base, sizeof(base)), series, session->user_id);
        return get_list(curl, arena, session, url, &task->list);
    }

    case JF_JOB_EPISODES: {
        char series[256], season[256];
        jf_url_escape(series, sizeof(series), task->a);
        jf_url_escape(season, sizeof(season), task->b);
        snprintf(url, sizeof(url),
                 "%s/Shows/%s/Episodes?userId=%s&seasonId=%s&fields=Overview&enableUserData=true",
                 jf_session_base(session, base, sizeof(base)), series, session->user_id, season);
        return get_list(curl, arena, session, url, &task->list);
    }

    case JF_JOB_ADJACENT: {
      char id[256], series[256];
      jf_url_escape(id, sizeof(id), task->a);
      if (task->b[0] == '\0') {
        snprintf(url, sizeof(url), "%s/Items/%s?userId=%s",
                 jf_session_base(session, base, sizeof(base)), id,
                 session->user_id);
        result =
            send_request(curl, arena, session, "GET", url, NULL, &body, &size);
        if (result != HTTP_OK)
          return result;
        json_object *root = json_tokener_parse((const char *)body);
        if (root == NULL)
          return HTTP_STATUS;
        parse_item(arena, root, &task->one);
        json_object_put(root);
        if (task->one.series_id == NULL) {
          task->list = (jf_item_list){0};
          return HTTP_OK;
        }
        snprintf(task->b, sizeof(task->b), "%s", task->one.series_id);
      }
      jf_url_escape(series, sizeof(series), task->b);
      /* Previous, this one and next, in the series' order: seasons do not
       * get in the way. */
      snprintf(
          url, sizeof(url),
          "%s/Shows/%s/Episodes?userId=%s&adjacentTo=%s&enableUserData=true",
          jf_session_base(session, base, sizeof(base)), series,
          session->user_id, id);
      return get_list(curl, arena, session, url, &task->list);
    }

    case JF_JOB_POSTER:
        result = artwork(curl, arena, session, task->a, task->b, task->start, task->limit,
                         task->image_kind, &task->image);
        task->has_image = result == HTTP_OK;
        return result;

    case JF_JOB_PLAYBACK_STARTED:
    case JF_JOB_PLAYBACK_PROGRESS: {
        char id[520];
        json_quote(id, sizeof(id), task->a);
        /* The subtitle stream is what lets the server remember the choice for
         * the next episode, when the user has it set to. */
        char source[520];
        json_quote(source, sizeof(source),
                   task->b[0] != '\0' ? task->b : task->a);
        snprintf(payload, sizeof(payload),
                 "{\"ItemId\":%s,\"MediaSourceId\":%s,\"PositionTicks\":%llu,"
                 "\"SubtitleStreamIndex\":%d}",
                 id, source, (unsigned long long)task->position_ticks,
                 task->subtitle_stream);
        snprintf(url, sizeof(url), "%s/Sessions/Playing%s",
                 jf_session_base(session, base, sizeof(base)),
                 task->job == JF_JOB_PLAYBACK_PROGRESS ? "/Progress" : "");
        return send_request(curl, arena, session, "POST", url, payload, &body, &size);
    }

    case JF_JOB_PLAYBACK_INFO: {
      char id[256];
      jf_url_escape(id, sizeof(id), task->a);
      snprintf(url, sizeof(url), "%s/Items/%s/PlaybackInfo?userId=%s",
               jf_session_base(session, base, sizeof(base)), id,
               session->user_id);
      result =
          send_request(curl, arena, session, "POST", url, "{}", &body, &size);
      return result != HTTP_OK ? result
                               : parse_playback_info(arena, (const char *)body,
                                                     &task->playback);
    }
    }
    return HTTP_STATUS;
}

/* The first media source's subtitle streams. The server applies the user's
 * subtitle mode and language to pick the default, which is the point of asking
 * it rather than reading the container. */
static http_result parse_playback_info(jf_arena *arena, const char *body,
                                       jf_playback_info *out) {
  json_object *root = json_tokener_parse(body);
  if (root == NULL)
    return HTTP_STATUS;
  *out = (jf_playback_info){NULL, -1, NULL, 0};
  json_object *sources = NULL, *source = NULL, *streams = NULL;
  if (json_object_object_get_ex(root, "MediaSources", &sources) &&
      json_object_get_type(sources) == json_type_array &&
      json_object_array_length(sources) > 0)
    source = json_object_array_get_idx(sources, 0);
  if (source != NULL) {
    out->media_source_id = json_string(arena, source, "Id");
    double number = 0;
    if (json_number(source, "DefaultSubtitleStreamIndex", &number))
      out->default_subtitle = (int)number;
    if (json_object_object_get_ex(source, "MediaStreams", &streams) &&
        json_object_get_type(streams) == json_type_array) {
      const size_t length = json_object_array_length(streams);
      out->subtitles = jf_arena_alloc(arena, (length ? length : 1) *
                                                 sizeof(*out->subtitles));
      for (size_t i = 0; out->subtitles != NULL && i < length; i++) {
        json_object *stream = json_object_array_get_idx(streams, i);
        const char *type = json_string(arena, stream, "Type");
        if (type == NULL || strcmp(type, "Subtitle") != 0 ||
            !json_number(stream, "Index", &number))
          continue;
        jf_subtitle_stream *sub = &out->subtitles[out->subtitle_count++];
        sub->index = (int)number;
        sub->title = json_string(arena, stream, "DisplayTitle");
        if (sub->title == NULL)
          sub->title = "Subtitles";
        sub->external = json_flag(stream, "IsExternal");
        sub->text = json_flag(stream, "IsTextSubtitleStream");
      }
    }
  }
  json_object_put(root);
  return out->media_source_id != NULL ? HTTP_OK : HTTP_STATUS;
}

/* ----------------------------------------------------------------- fetcher */

static const char *result_name(http_result result)
{
    switch (result) {
    case HTTP_UNAUTHORIZED: return "Unauthorized";
    case HTTP_CLIENT_ERROR: return "ClientError";
    case HTTP_SERVER_ERROR: return "ServerError";
    case HTTP_REQUEST_FAILED: return "RequestFailed";
    default: return "HttpStatus";
    }
}

static void *worker(void *user)
{
    jf_fetcher *fetcher = user;
    CURL *curl = curl_easy_init();

    for (;;) {
        pthread_mutex_lock(&fetcher->mutex);
        while (fetcher->running && fetcher->queue_count == 0)
            pthread_cond_wait(&fetcher->work_ready, &fetcher->mutex);
        if (!fetcher->running) {
            pthread_mutex_unlock(&fetcher->mutex);
            break;
        }
        jf_task *task = fetcher->queue[fetcher->queue_head];
        fetcher->queue_head = (fetcher->queue_head + 1) % JF_TASK_SLOTS;
        fetcher->queue_count--;
        task->state = JF_TASK_RUNNING;
        const jf_session session = fetcher->session;
        pthread_mutex_unlock(&fetcher->mutex);

        const http_result result = curl != NULL ? execute(curl, &session, task) : HTTP_REQUEST_FAILED;
        if (result == HTTP_OK)
          decode_blurs(task);

        pthread_mutex_lock(&fetcher->mutex);
        if (result == HTTP_OK) {
            task->state = JF_TASK_READY;
        } else {
            snprintf(task->error, sizeof(task->error), "%s", result_name(result));
            task->state = JF_TASK_FAILED;
        }
        pthread_mutex_unlock(&fetcher->mutex);
        if (fetcher->wake != NULL)
            fetcher->wake();
    }
    if (curl != NULL)
        curl_easy_cleanup(curl);
    return NULL;
}

bool jf_fetcher_init(jf_fetcher *fetcher, void (*wake)(void))
{
    memset(fetcher, 0, sizeof(*fetcher));
    pthread_mutex_init(&fetcher->mutex, NULL);
    pthread_cond_init(&fetcher->work_ready, NULL);
    fetcher->wake = wake;
    fetcher->running = true;
    for (size_t i = 0; i < JF_FETCH_WORKERS; i++) {
        if (pthread_create(&fetcher->threads[i], NULL, worker, fetcher) != 0)
            break;
        fetcher->thread_count++;
    }
    return fetcher->thread_count > 0;
}

void jf_fetcher_deinit(jf_fetcher *fetcher)
{
    pthread_mutex_lock(&fetcher->mutex);
    fetcher->running = false;
    pthread_cond_broadcast(&fetcher->work_ready);
    pthread_mutex_unlock(&fetcher->mutex);
    for (size_t i = 0; i < fetcher->thread_count; i++)
        pthread_join(fetcher->threads[i], NULL);
    for (size_t i = 0; i < JF_TASK_SLOTS; i++) {
        if (fetcher->tasks[i].has_image)
            jf_image_free(&fetcher->tasks[i].image);
        jf_arena_destroy(&fetcher->tasks[i].arena);
    }
    pthread_mutex_destroy(&fetcher->mutex);
    pthread_cond_destroy(&fetcher->work_ready);
}

void jf_fetcher_set_session(jf_fetcher *fetcher, const jf_session *session)
{
    pthread_mutex_lock(&fetcher->mutex);
    fetcher->session = *session;
    pthread_mutex_unlock(&fetcher->mutex);
}

jf_task *jf_fetcher_submit(jf_fetcher *fetcher, jf_job job, uint32_t tag)
{
    pthread_mutex_lock(&fetcher->mutex);
    jf_task *found = NULL;
    for (size_t i = 0; i < JF_TASK_SLOTS && found == NULL; i++) {
        jf_task *task = &fetcher->tasks[i];
        if (task->state != JF_TASK_FREE)
            continue;
        /* The arena outlives the reset: it is the slot's storage, not the task's. */
        const jf_arena arena = task->arena;
        memset(task, 0, sizeof(*task));
        task->arena = arena;
        task->job = job;
        task->tag = tag;
        task->state = JF_TASK_RESERVED;
        found = task;
    }
    pthread_mutex_unlock(&fetcher->mutex);
    return found;
}

void jf_fetcher_start(jf_fetcher *fetcher, jf_task *task)
{
    pthread_mutex_lock(&fetcher->mutex);
    task->state = JF_TASK_QUEUED;
    /* There are only JF_TASK_SLOTS tasks, so publishing a reserved one cannot overfill a
     * queue of the same capacity. */
    fetcher->queue[(fetcher->queue_head + fetcher->queue_count) % JF_TASK_SLOTS] = task;
    fetcher->queue_count++;
    pthread_cond_signal(&fetcher->work_ready);
    pthread_mutex_unlock(&fetcher->mutex);
}

jf_task *jf_fetcher_finished(jf_fetcher *fetcher)
{
    pthread_mutex_lock(&fetcher->mutex);
    jf_task *found = NULL;
    for (size_t i = 0; i < JF_TASK_SLOTS && found == NULL; i++) {
        const jf_task_state state = fetcher->tasks[i].state;
        if (state == JF_TASK_READY || state == JF_TASK_FAILED)
            found = &fetcher->tasks[i];
    }
    pthread_mutex_unlock(&fetcher->mutex);
    return found;
}

void jf_fetcher_release(jf_fetcher *fetcher, jf_task *task)
{
    /* The decoded image is the one result that is not in the arena: libpng wrote it into
     * a malloc'd buffer the UI has now copied to a texture. */
    if (task->has_image) {
        jf_image_free(&task->image);
        task->has_image = false;
    }
    jf_arena_reset(&task->arena);
    pthread_mutex_lock(&fetcher->mutex);
    task->state = JF_TASK_FREE;
    pthread_mutex_unlock(&fetcher->mutex);
}

size_t jf_fetcher_pending(jf_fetcher *fetcher)
{
    pthread_mutex_lock(&fetcher->mutex);
    size_t count = 0;
    for (size_t i = 0; i < JF_TASK_SLOTS; i++)
        if (fetcher->tasks[i].state != JF_TASK_FREE)
            count++;
    pthread_mutex_unlock(&fetcher->mutex);
    return count;
}
