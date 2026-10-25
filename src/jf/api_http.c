/*
 * The HTTP transport behind the fetcher: libcurl for the requests, json-c for
 * the answers, and the artwork cache in front of the image endpoint. One
 * connection per worker thread. The tests link api_mock.c in its place.
 */
#include "api.h"

#include <ctype.h>
#include <curl/curl.h>
#include <json.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "../platform/os.h"

static bool same(const char *a, const char *b) {
  return a != NULL && strcmp(a, b) == 0;
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

static size_t collect(char *data, size_t size, size_t count, void *user) {
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

static const char *json_string(jf_arena *arena, json_object *object,
                               const char *key);
static http_result parse_playback_info(jf_arena *arena, const char *body,
                                       jf_playback_info *out);

/* One request. The body is copied into `arena`; `payload` carries the JSON for
 * a POST (an empty string still means POST, as Quick Connect's Initiate
 * requires). */
static http_result send_request(CURL *curl, jf_arena *arena,
                                const jf_session *session, const char *method,
                                const char *url, const char *payload,
                                uint8_t **out_body, size_t *out_size) {
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
  if (payload == NULL && strcmp(method, "GET") != 0)
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
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

static http_result probe_server(CURL *curl, jf_arena *arena,
                                const jf_session *session, const char *address,
                                jf_discovered *out) {
  jf_session probe = *session;
  snprintf(probe.url, sizeof(probe.url), "%s", address);
  char base[512];
  char url[1024];
  snprintf(url, sizeof(url), "%s/System/Info/Public",
           jf_session_base(&probe, base, sizeof(base)));
  uint8_t *body = NULL;
  size_t size = 0;
  const http_result result =
      send_request(curl, arena, &probe, "GET", url, NULL, &body, &size);
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

static const char *json_string(jf_arena *arena, json_object *object,
                               const char *key) {
  json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value) ||
      json_object_get_type(value) != json_type_string)
    return NULL;
  return jf_arena_strdup(arena, json_object_get_string(value));
}

static bool json_number(json_object *object, const char *key, double *out) {
  json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value))
    return false;
  const json_type type = json_object_get_type(value);
  if (type != json_type_int && type != json_type_double)
    return false;
  *out = json_object_get_double(value);
  return true;
}

static bool json_flag(json_object *object, const char *key) {
  json_object *value = NULL;
  return json_object_object_get_ex(object, key, &value) &&
         json_object_get_boolean(value);
}

/* A string into a fixed buffer, empty when missing. */
static void json_copy(json_object *object, const char *key, char *out,
                      size_t out_len) {
  json_object *value = NULL;
  const char *text = json_object_object_get_ex(object, key, &value) &&
                             json_object_get_type(value) == json_type_string
                         ? json_object_get_string(value)
                         : "";
  snprintf(out, out_len, "%s", text);
}

/* BackdropImageTags[0], or the parent's - the only backdrop this UI draws. */
static const char *json_first_tag(jf_arena *arena, json_object *object,
                                  const char *key) {
  json_object *array = NULL;
  if (!json_object_object_get_ex(object, key, &array) ||
      json_object_get_type(array) != json_type_array ||
      json_object_array_length(array) == 0)
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

static void parse_item(jf_arena *arena, json_object *object, jf_item *out) {
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
  out->series_primary_image_tag =
      json_string(arena, object, "SeriesPrimaryImageTag");
  out->parent_backdrop_item_id =
      json_string(arena, object, "ParentBackdropItemId");
  out->backdrop_image_tag = json_first_tag(arena, object, "BackdropImageTags");
  out->parent_backdrop_image_tag =
      json_first_tag(arena, object, "ParentBackdropImageTags");

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
    if ((out->has_played_percentage =
             json_number(user_data, "PlayedPercentage", &number)))
      out->played_percentage = number;
    if (json_number(user_data, "PlaybackPositionTicks", &number))
      out->playback_position_ticks = (uint64_t)number;
    out->played = json_flag(user_data, "Played");
    if ((out->has_unplayed_item_count =
             json_number(user_data, "UnplayedItemCount", &number)))
      out->unplayed_item_count = (uint32_t)number;
  }

  json_object *streams = NULL;
  if (json_object_object_get_ex(object, "MediaStreams", &streams) &&
      json_object_get_type(streams) == json_type_array)
    for (size_t i = 0; i < json_object_array_length(streams) && !out->has_video;
         i++) {
      json_object *stream = json_object_array_get_idx(streams, i);
      char type[16];
      json_copy(stream, "Type", type, sizeof(type));
      if (strcmp(type, "Video") != 0)
        continue;
      out->has_video = true;
      json_copy(stream, "Codec", out->video.codec, sizeof(out->video.codec));
      json_copy(stream, "Profile", out->video.profile,
                sizeof(out->video.profile));
      if (json_number(stream, "BitDepth", &number))
        out->video.bit_depth = (uint32_t)number;
      if (json_number(stream, "Width", &number))
        out->video.width = (uint32_t)number;
      if (json_number(stream, "Height", &number))
        out->video.height = (uint32_t)number;
      if (json_number(stream, "RealFrameRate", &number) ||
          json_number(stream, "AverageFrameRate", &number))
        out->video.frame_rate = (float)number;
      if (json_number(stream, "BitRate", &number))
        out->video.bit_rate = (uint32_t)number;
    }
}

static bool parse_item_list(jf_arena *arena, const char *body,
                            jf_item_list *out) {
  json_object *root = json_tokener_parse(body);
  if (root == NULL)
    return false;
  memset(out, 0, sizeof(*out));

  /* A page of items, or - /Items/Latest - the items alone. */
  json_object *items = root;
  if ((json_object_get_type(root) == json_type_array ||
       json_object_object_get_ex(root, "Items", &items)) &&
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

/* --------------------------------------------------------------- discovery */

/* Jellyfin answers a UDP broadcast on 7359 with one JSON datagram per server.
 */
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

static http_result get_list(CURL *curl, jf_arena *arena,
                            const jf_session *session, const char *url,
                            jf_item_list *out) {
  uint8_t *body = NULL;
  size_t size = 0;
  const http_result result =
      send_request(curl, arena, session, "GET", url, NULL, &body, &size);
  if (result != HTTP_OK)
    return result;
  return parse_item_list(arena, (const char *)body, out) ? HTTP_OK
                                                         : HTTP_STATUS;
}

/* Jellyfin sends the authentication payload as JSON, so the two values have to
 * be escaped rather than pasted: a password may legitimately contain a quote or
 * a backslash. */
static void json_quote(char *out, size_t out_len, const char *value) {
  size_t at = 0;
  if (out_len == 0)
    return;
  out[at++] = '"';
  for (const unsigned char *p = (const unsigned char *)value;
       *p != '\0' && at + 8 < out_len; p++) {
    switch (*p) {
    case '"':
      out[at++] = '\\';
      out[at++] = '"';
      break;
    case '\\':
      out[at++] = '\\';
      out[at++] = '\\';
      break;
    case '\n':
      out[at++] = '\\';
      out[at++] = 'n';
      break;
    case '\r':
      out[at++] = '\\';
      out[at++] = 'r';
      break;
    case '\t':
      out[at++] = '\\';
      out[at++] = 't';
      break;
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

static http_result parse_auth(jf_arena *arena, const char *body, jf_auth *out) {
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

static http_result parse_quick(jf_arena *arena, const char *body,
                               jf_quick_connect *out) {
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
 * Three things happen here, in order: the cache is consulted, the request is
 * made only on a miss, and the encoded bytes are cached before decoding.
 *
 * The cache key is Jellyfin's image tag, a hash of the image, so a hit needs no
 * revalidation round trip - changed artwork has a different tag and therefore a
 * different file. (The server offers no ETag on images and ignores
 * If-None-Match; it does honour If-Modified-Since, which is the fallback if a
 * tagless image ever needs caching. Today an untagged image is simply not
 * cached.)
 *
 * fillWidth/fillHeight is the server's sizing hint, not a contract - it answers
 * 204x300 or 534x300 depending on the source art - so the caller must cope with
 * whatever comes back. format=Png is asked for because that is the one decoder
 * with the same ABI on the TV and on a development machine; see image.h. */
static http_result artwork(CURL *curl, jf_arena *arena,
                           const jf_session *session, const char *id,
                           const char *tag, uint32_t width, uint32_t height,
                           jf_image_kind kind, jf_image *out) {
  char cache_id[560];
  if (kind == JF_IMAGE_PRIMARY)
    snprintf(cache_id, sizeof(cache_id), "%s", id);
  else
    snprintf(cache_id, sizeof(cache_id), "%s-backdrop", id);

  char path[768];
  const bool cacheable =
      jf_store_image_path(path, sizeof(path), cache_id, tag, width, height);
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
  int n = snprintf(
      url, sizeof(url),
      "%s/Items/%s/Images/%s?fillWidth=%u&fillHeight=%u&format=Png",
      jf_session_base(session, base, sizeof(base)), escaped_id,
      kind == JF_IMAGE_PRIMARY ? "Primary" : "Backdrop/0", width, height);
  /* The tag makes the URL change when the image does, which is what lets any
   * cache in between - ours, or a proxy - treat it as immutable. */
  if (n > 0 && escaped_tag[0] != '\0')
    snprintf(url + n, sizeof(url) - (size_t)n, "&tag=%s", escaped_tag);

  uint8_t *body = NULL;
  size_t size = 0;
  const http_result result =
      send_request(curl, arena, session, "GET", url, NULL, &body, &size);
  if (result != HTTP_OK)
    return result;
  if (cacheable)
    jf_store_write_image(path, body, size);
  return jf_image_decode(body, size, out) ? HTTP_OK : HTTP_STATUS;
}

static http_result execute(CURL *curl, const jf_session *session,
                           jf_task *task) {
  jf_arena *arena = &task->arena;
  char base[512];
  char url[1024];
  char payload[1200];
  uint8_t *body = NULL;
  size_t size = 0;
  http_result result;

  switch (task->job) {
  case JF_JOB_DISCOVER:
    return discover(arena, &task->servers, &task->server_count)
               ? HTTP_OK
               : HTTP_REQUEST_FAILED;

  case JF_JOB_PROBE:
    return probe_server(curl, arena, session, task->a, &task->server);

  case JF_JOB_LOGIN: {
    char user[520], password[520];
    json_quote(user, sizeof(user), task->a);
    json_quote(password, sizeof(password), task->b);
    snprintf(payload, sizeof(payload), "{\"Username\":%s,\"Pw\":%s}", user,
             password);
    snprintf(url, sizeof(url), "%s/Users/AuthenticateByName",
             jf_session_base(session, base, sizeof(base)));
    result =
        send_request(curl, arena, session, "POST", url, payload, &body, &size);
    return result != HTTP_OK
               ? result
               : parse_auth(arena, (const char *)body, &task->auth);
  }

  case JF_JOB_QUICK_INITIATE:
    snprintf(url, sizeof(url), "%s/QuickConnect/Initiate",
             jf_session_base(session, base, sizeof(base)));
    result = send_request(curl, arena, session, "POST", url, "", &body, &size);
    return result != HTTP_OK
               ? result
               : parse_quick(arena, (const char *)body, &task->quick);

  case JF_JOB_QUICK_POLL: {
    char secret[256];
    jf_url_escape(secret, sizeof(secret), task->a);
    snprintf(url, sizeof(url), "%s/QuickConnect/Connect?secret=%s",
             jf_session_base(session, base, sizeof(base)), secret);
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    return result != HTTP_OK
               ? result
               : parse_quick(arena, (const char *)body, &task->quick);
  }

  case JF_JOB_QUICK_AUTHENTICATE: {
    char secret[520];
    json_quote(secret, sizeof(secret), task->a);
    snprintf(payload, sizeof(payload), "{\"Secret\":%s}", secret);
    snprintf(url, sizeof(url), "%s/Users/AuthenticateWithQuickConnect",
             jf_session_base(session, base, sizeof(base)));
    result =
        send_request(curl, arena, session, "POST", url, payload, &body, &size);
    return result != HTTP_OK
               ? result
               : parse_auth(arena, (const char *)body, &task->auth);
  }

  case JF_JOB_VIEWS:
    snprintf(url, sizeof(url), "%s/UserViews?userId=%s&limit=256",
             jf_session_base(session, base, sizeof(base)), session->user_id);
    return get_list(curl, arena, session, url, &task->list);

  case JF_JOB_RESUME:
    snprintf(url, sizeof(url),
             "%s/UserItems/Resume?userId=%s&limit=24&fields=Overview",
             jf_session_base(session, base, sizeof(base)), session->user_id);
    return get_list(curl, arena, session, url, &task->list);

  case JF_JOB_NEXT_UP:
    snprintf(url, sizeof(url),
             "%s/Shows/NextUp?userId=%s&limit=24&fields=Overview",
             jf_session_base(session, base, sizeof(base)), session->user_id);
    return get_list(curl, arena, session, url, &task->list);

  case JF_JOB_LATEST: {
    /* Episodes of one series come as the series, as on the server's own
     * home screen. */
    char parent[256];
    jf_url_escape(parent, sizeof(parent), task->a);
    snprintf(url, sizeof(url),
             "%s/Items/Latest?userId=%s&parentId=%s&limit=24&fields=Overview",
             jf_session_base(session, base, sizeof(base)), session->user_id,
             parent);
    return get_list(curl, arena, session, url, &task->list);
  }

  case JF_JOB_CHILDREN: {
    /* sortBy=SortName keeps paging stable, which is the whole reason a virtual
     * grid can ask for the window [start, start+limit). */
    char parent[256];
    jf_url_escape(parent, sizeof(parent), task->a);
    snprintf(
        url, sizeof(url),
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
             jf_session_base(session, base, sizeof(base)), id,
             session->user_id);
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

  case JF_JOB_ENCODING: {
    snprintf(url, sizeof(url), "%s/System/Configuration/encoding",
             jf_session_base(session, base, sizeof(base)));
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
      return result;
    json_object *root = json_tokener_parse((const char *)body);
    if (root == NULL)
      return HTTP_STATUS;
    jf_server_transcode *out = &task->encoding;
    memset(out, 0, sizeof(*out));
    out->known = true;
    char type[32];
    json_copy(root, "HardwareAccelerationType", type, sizeof(type));
    out->hardware = type[0] != '\0' && strcmp(type, "none") != 0 &&
                    json_flag(root, "EnableHardwareEncoding");
    json_object *codecs = NULL;
    size_t used = (size_t)snprintf(out->codecs, sizeof(out->codecs), ",");
    if (json_object_object_get_ex(root, "HardwareDecodingCodecs", &codecs) &&
        json_object_get_type(codecs) == json_type_array)
      for (size_t i = 0;
           i < json_object_array_length(codecs) && used < sizeof(out->codecs);
           i++)
        used += (size_t)snprintf(
            out->codecs + used, sizeof(out->codecs) - used, "%s,",
            json_object_get_string(json_object_array_get_idx(codecs, i)));
    out->hevc_10 = json_flag(root, "EnableDecodingColorDepth10Hevc");
    out->vp9_10 = json_flag(root, "EnableDecodingColorDepth10Vp9");
    out->hevc_rext_10 = json_flag(root, "EnableDecodingColorDepth10HevcRext");
    out->hevc_rext_12 = json_flag(root, "EnableDecodingColorDepth12HevcRext");
    json_object_put(root);
    return HTTP_OK;
  }

  case JF_JOB_SEASONS: {
    char series[256];
    jf_url_escape(series, sizeof(series), task->a);
    snprintf(url, sizeof(url),
             "%s/Shows/%s/"
             "Seasons?userId=%s&fields=ChildCount,Overview&imageTypeLimit=1"
             "&enableUserData=true",
             jf_session_base(session, base, sizeof(base)), series,
             session->user_id);
    return get_list(curl, arena, session, url, &task->list);
  }

  case JF_JOB_EPISODES: {
    char series[256], season[256];
    jf_url_escape(series, sizeof(series), task->a);
    jf_url_escape(season, sizeof(season), task->b);
    snprintf(
        url, sizeof(url),
        "%s/Shows/%s/"
        "Episodes?userId=%s&seasonId=%s&fields=Overview&enableUserData=true",
        jf_session_base(session, base, sizeof(base)), series, session->user_id,
        season);
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
    snprintf(url, sizeof(url),
             "%s/Shows/%s/Episodes?userId=%s&adjacentTo=%s&enableUserData=true",
             jf_session_base(session, base, sizeof(base)), series,
             session->user_id, id);
    return get_list(curl, arena, session, url, &task->list);
  }

  case JF_JOB_POSTER:
    result = artwork(curl, arena, session, task->a, task->b, task->start,
                     task->limit, task->image_kind, &task->image);
    task->has_image = result == HTTP_OK;
    return result;

  case JF_JOB_PLAYBACK_STARTED:
  case JF_JOB_PLAYBACK_PROGRESS: {
    char id[520];
    json_quote(id, sizeof(id), task->a);
    /* The subtitle stream is what lets the server remember the choice for
     * the next episode, when the user has it set to. */
    char source[520];
    json_quote(source, sizeof(source), task->b[0] != '\0' ? task->b : task->a);
    snprintf(payload, sizeof(payload),
             "{\"ItemId\":%s,\"MediaSourceId\":%s,\"PositionTicks\":%llu,"
             "\"SubtitleStreamIndex\":%d}",
             id, source, (unsigned long long)task->position_ticks,
             task->subtitle_stream);
    snprintf(url, sizeof(url), "%s/Sessions/Playing%s",
             jf_session_base(session, base, sizeof(base)),
             task->job == JF_JOB_PLAYBACK_PROGRESS ? "/Progress" : "");
    return send_request(curl, arena, session, "POST", url, payload, &body,
                        &size);
  }

  case JF_JOB_DELAYS:
  case JF_JOB_SAVE_DELAYS: {
    char user[128];
    jf_url_escape(user, sizeof(user), session->user_id);
    snprintf(url, sizeof(url),
             "%s/DisplayPreferences/subtitle-delays?userId=%s&client=%s",
             jf_session_base(session, base, sizeof(base)), user,
             JF_CLIENT_NAME);
    if (task->job == JF_JOB_SAVE_DELAYS)
      return send_request(curl, arena, session, "POST", url,
                          task->body != NULL ? task->body : "{}", &body, &size);
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
      return result;
    json_object *root = json_tokener_parse((const char *)body), *prefs = NULL;
    if (root != NULL &&
        json_object_object_get_ex(root, "CustomPrefs", &prefs) &&
        json_object_is_type(prefs, json_type_object)) {
      const size_t count = (size_t)json_object_object_length(prefs);
      task->keys = jf_arena_alloc(arena, (count + 1) * sizeof(*task->keys));
      task->values = jf_arena_alloc(arena, (count + 1) * sizeof(*task->values));
      json_object_object_foreach(prefs, key, value) {
        if (task->keys == NULL || task->values == NULL ||
            !json_object_is_type(value, json_type_string))
          continue;
        task->keys[task->pair_count] = jf_arena_strdup(arena, key);
        task->values[task->pair_count++] =
            jf_arena_strdup(arena, json_object_get_string(value));
      }
    }
    json_object_put(root);
    return HTTP_OK;
  }

  case JF_JOB_SUBTITLE_SEARCH: {
    char id[256], language[64];
    jf_url_escape(id, sizeof(id), task->a);
    jf_url_escape(language, sizeof(language), task->b);
    snprintf(url, sizeof(url), "%s/Items/%s/RemoteSearch/Subtitles/%s",
             jf_session_base(session, base, sizeof(base)), id, language);
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
      return result;
    json_object *root = json_tokener_parse((const char *)body);
    if (root == NULL || !json_object_is_type(root, json_type_array)) {
      json_object_put(root);
      return HTTP_STATUS;
    }
    const size_t count = json_object_array_length(root);
    task->remote = jf_arena_alloc(arena, (count + 1) * sizeof(*task->remote));
    for (size_t i = 0; task->remote != NULL && i < count; i++) {
      json_object *item = json_object_array_get_idx(root, i);
      jf_remote_subtitle *out = &task->remote[task->remote_count];
      out->id = json_string(arena, item, "Id");
      out->name = json_string(arena, item, "Name");
      out->provider = json_string(arena, item, "ProviderName");
      out->format = json_string(arena, item, "Format");
      double downloads = 0;
      json_number(item, "DownloadCount", &downloads);
      out->downloads = (int)downloads;
      if (out->id != NULL)
        task->remote_count++;
    }
    json_object_put(root);
    return task->remote != NULL ? HTTP_OK : HTTP_REQUEST_FAILED;
  }

  case JF_JOB_SUBTITLE_DOWNLOAD: {
    char id[256], subtitle[512];
    jf_url_escape(id, sizeof(id), task->a);
    jf_url_escape(subtitle, sizeof(subtitle), task->b);
    snprintf(url, sizeof(url), "%s/Items/%s/RemoteSearch/Subtitles/%s",
             jf_session_base(session, base, sizeof(base)), id, subtitle);
    return send_request(curl, arena, session, "POST", url, "", &body, &size);
  }

  case JF_JOB_SUBTITLE_SOURCES: {
    snprintf(url, sizeof(url), "%s/Packages",
             jf_session_base(session, base, sizeof(base)));
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
      return result;
    json_object *packages = json_tokener_parse((const char *)body);
    snprintf(url, sizeof(url), "%s/Plugins",
             jf_session_base(session, base, sizeof(base)));
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    json_object *plugins =
        result == HTTP_OK ? json_tokener_parse((const char *)body) : NULL;
    if (!json_object_is_type(packages, json_type_array) ||
        !json_object_is_type(plugins, json_type_array)) {
      json_object_put(packages);
      json_object_put(plugins);
      return result != HTTP_OK ? result : HTTP_STATUS;
    }
    const size_t count = json_object_array_length(packages);
    task->sources = jf_arena_alloc(arena, (count + 1) * sizeof(*task->sources));
    for (size_t i = 0; task->sources != NULL && i < count; i++) {
      json_object *package = json_object_array_get_idx(packages, i);
      const char *category = json_string(arena, package, "category");
      const char *guid = json_string(arena, package, "guid");
      if (category == NULL || strcasecmp(category, "Subtitles") != 0)
        continue;
      /* Subtitle Extract is in the category but only copies embedded
       * subtitles out to files: nothing to download from. */
      if (guid != NULL && strncasecmp(guid, "cd893c24", 8) == 0)
        continue;
      jf_subtitle_source *out = &task->sources[task->source_count++];
      *out =
          (jf_subtitle_source){json_string(arena, package, "name"),
                               json_string(arena, package, "guid"), NULL, NULL};
      /* Installed plugins carry the package's guid as their id, which the
       * server writes without dashes or with them. */
      for (size_t j = 0;
           out->guid != NULL && j < json_object_array_length(plugins); j++) {
        json_object *plugin = json_object_array_get_idx(plugins, j);
        const char *id = json_string(arena, plugin, "Id");
        bool same = id != NULL;
        for (const char *a = id, *b = out->guid; same && (*a || *b);) {
          if (*a == '-') {
            a++;
            continue;
          }
          if (*b == '-') {
            b++;
            continue;
          }
          same = (*a | 0x20) == (*b | 0x20);
          a++, b++;
        }
        if (!same)
          continue;
        out->version = json_string(arena, plugin, "Version");
        out->status = json_string(arena, plugin, "Status");
        out->guid = id; /* the form /Plugins/{id} takes */
        break;
      }
    }
    json_object_put(packages);
    json_object_put(plugins);
    return task->sources != NULL ? HTTP_OK : HTTP_REQUEST_FAILED;
  }

  case JF_JOB_SOURCE_INSTALL: {
    char name[256], guid[128];
    jf_url_escape(name, sizeof(name), task->a);
    jf_url_escape(guid, sizeof(guid), task->b);
    snprintf(url, sizeof(url), "%s/Packages/Installed/%s?assemblyGuid=%s",
             jf_session_base(session, base, sizeof(base)), name, guid);
    return send_request(curl, arena, session, "POST", url, "", &body, &size);
  }

  case JF_JOB_SOURCE_TOGGLE: {
    char id[128], version[64];
    jf_url_escape(id, sizeof(id), task->a);
    jf_url_escape(version, sizeof(version), task->b);
    snprintf(url, sizeof(url), "%s/Plugins/%s/%s/%s",
             jf_session_base(session, base, sizeof(base)), id, version,
             task->start ? "Enable" : "Disable");
    return send_request(curl, arena, session, "POST", url, "", &body, &size);
  }

  case JF_JOB_ADD_REPOSITORY: {
    /* The manifest has to match the server's version, or what it offers
     * will not load. */
    snprintf(url, sizeof(url), "%s/System/Info/Public",
             jf_session_base(session, base, sizeof(base)));
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
      return result;
    json_object *info = json_tokener_parse((const char *)body);
    const char *version = json_string(arena, info, "Version");
    json_object_put(info);
    unsigned major = 0, minor = 0;
    if (version == NULL || sscanf(version, "%u.%u", &major, &minor) != 2)
      return HTTP_STATUS;
    char manifest[256];
    snprintf(manifest, sizeof(manifest),
             "https://raw.githubusercontent.com/josdion/subbuzz/master/repo/"
             "jellyfin_%u.%u.json",
             major, minor);

    snprintf(url, sizeof(url), "%s/Repositories",
             jf_session_base(session, base, sizeof(base)));
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    if (result != HTTP_OK)
      return result;
    json_object *repositories = json_tokener_parse((const char *)body);
    if (!json_object_is_type(repositories, json_type_array)) {
      json_object_put(repositories);
      return HTTP_STATUS;
    }
    /* Written back whole: the endpoint replaces the list. */
    for (size_t i = 0; i < json_object_array_length(repositories); i++) {
      const char *known =
          json_string(arena, json_object_array_get_idx(repositories, i), "Url");
      if (known != NULL && strstr(known, "josdion/subbuzz") != NULL) {
        json_object_put(repositories);
        return HTTP_OK;
      }
    }
    json_object *entry = json_object_new_object();
    json_object_object_add(entry, "Name", json_object_new_string("subbuzz"));
    json_object_object_add(entry, "Url", json_object_new_string(manifest));
    json_object_object_add(entry, "Enabled", json_object_new_boolean(1));
    json_object_array_add(repositories, entry);
    const char *payload_json = jf_arena_strdup(
        arena,
        json_object_to_json_string_ext(repositories, JSON_C_TO_STRING_PLAIN));
    json_object_put(repositories);
    return send_request(curl, arena, session, "POST", url, payload_json, &body,
                        &size);
  }

  case JF_JOB_RESTART:
    snprintf(url, sizeof(url), "%s/System/Restart",
             jf_session_base(session, base, sizeof(base)));
    return send_request(curl, arena, session, "POST", url, "", &body, &size);

  case JF_JOB_SUBTITLE_CHECK:
    jf_subtitle_url(session, task->a, task->b, (int)task->start, url,
                    sizeof(url));
    result = send_request(curl, arena, session, "GET", url, NULL, &body, &size);
    return result != HTTP_OK ? result : size > 0 ? HTTP_OK : HTTP_STATUS;

  case JF_JOB_SUBTITLE_DELETE: {
    char id[256], index[32];
    jf_url_escape(id, sizeof(id), task->a);
    jf_url_escape(index, sizeof(index), task->b);
    snprintf(url, sizeof(url), "%s/Videos/%s/Subtitles/%s",
             jf_session_base(session, base, sizeof(base)), id, index);
    return send_request(curl, arena, session, "DELETE", url, NULL, &body,
                        &size);
  }

  case JF_JOB_MARK_PLAYED: {
    char id[256];
    jf_url_escape(id, sizeof(id), task->a);
    snprintf(url, sizeof(url), "%s/UserPlayedItems/%s?userId=%s",
             jf_session_base(session, base, sizeof(base)), id,
             session->user_id);
    return send_request(curl, arena, session, task->start ? "POST" : "DELETE",
                        url, NULL, &body, &size);
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

/* The first media source's audio and subtitle streams. The server applies the
 * user's language preferences to pick the defaults, which is the point of
 * asking it rather than reading the container. */
static http_result parse_playback_info(jf_arena *arena, const char *body,
                                       jf_playback_info *out) {
  json_object *root = json_tokener_parse(body);
  if (root == NULL)
    return HTTP_STATUS;
  *out = (jf_playback_info){NULL, -1, NULL, 0, -1, NULL, 0};
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
    if (json_number(source, "DefaultAudioStreamIndex", &number))
      out->default_audio = (int)number;
    if (json_object_object_get_ex(source, "MediaStreams", &streams) &&
        json_object_get_type(streams) == json_type_array) {
      const size_t length = json_object_array_length(streams);
      out->subtitles = jf_arena_alloc(arena, (length ? length : 1) *
                                                 sizeof(*out->subtitles));
      out->audio =
          jf_arena_alloc(arena, (length ? length : 1) * sizeof(*out->audio));
      for (size_t i = 0;
           out->subtitles != NULL && out->audio != NULL && i < length; i++) {
        json_object *stream = json_object_array_get_idx(streams, i);
        const char *type = json_string(arena, stream, "Type");
        const bool audio = type != NULL && strcmp(type, "Audio") == 0;
        if (type == NULL || (!audio && strcmp(type, "Subtitle") != 0) ||
            !json_number(stream, "Index", &number))
          continue;
        jf_media_stream *sub = audio ? &out->audio[out->audio_count++]
                                     : &out->subtitles[out->subtitle_count++];
        sub->index = (int)number;
        sub->title = json_string(arena, stream, "DisplayTitle");
        if (sub->title == NULL)
          sub->title = "Subtitles";
        sub->language = json_string(arena, stream, "Language");
        sub->name = json_string(arena, stream, "Title");
        sub->external = json_flag(stream, "IsExternal");
        sub->text = json_flag(stream, "IsTextSubtitleStream");
        const char *codec = json_string(arena, stream, "Codec");
        sub->styled = codec != NULL && (strcasecmp(codec, "ass") == 0 ||
                                        strcasecmp(codec, "ssa") == 0);
      }
    }
  }
  json_object_put(root);
  return out->media_source_id != NULL ? HTTP_OK : HTTP_STATUS;
}

static const char *result_name(http_result result) {
  switch (result) {
  case HTTP_UNAUTHORIZED:
    return "Unauthorized";
  case HTTP_CLIENT_ERROR:
    return "ClientError";
  case HTTP_SERVER_ERROR:
    return "ServerError";
  case HTTP_REQUEST_FAILED:
    return "RequestFailed";
  default:
    return "HttpStatus";
  }
}

static pthread_once_t curl_once = PTHREAD_ONCE_INIT;
/* Without it every request fails before it is made, so it says why. */
static void curl_init(void) {
  const CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
  fprintf(stderr, "%s: %s\n", curl_version(), curl_easy_strerror(result));
}

void *jf_api_connect(void) {
  pthread_once(&curl_once, curl_init);
  return curl_easy_init();
}

void jf_api_disconnect(void *connection) {
  if (connection != NULL)
    curl_easy_cleanup(connection);
}

const char *jf_api_execute(void *connection, const jf_session *session,
                           jf_task *task) {
  const http_result result = connection != NULL
                                 ? execute(connection, session, task)
                                 : HTTP_REQUEST_FAILED;
  return result == HTTP_OK ? NULL : result_name(result);
}

/* ------------------------------------------------------------------ events
 *
 * curl's WebSocket in its callback mode: the transfer is the connection, the
 * write callback gets the server's messages, and the progress callback - about
 * once a second, idle or not - sends the keep-alive and notices a stop. */

static struct {
  pthread_t thread;
  bool running; /* the UI thread's */
  atomic_bool stop;
  jf_session session;
  void (*changed)(const jf_user_data *items, size_t count);
  response message;
  bool overflowed;
  bool connected, ever_connected;
  time_t keep_alive_at;
} events;

/* Bigger is a long LibraryChanged, which means a change all the same. */
#define EVENT_MESSAGE_MAX (4u << 20)
/* A UserDataChanged naming more is passed on as a change with none: the UI
 * fetches what it shows again. */
#define EVENT_ITEMS 64

/* Its UserDataList, in `out`; false when there is more than fits. */
static bool event_user_data(json_object *root, jf_user_data *out,
                            size_t *count) {
  json_object *data = NULL, *list = NULL;
  *count = 0;
  if (!json_object_object_get_ex(root, "Data", &data) ||
      !json_object_object_get_ex(data, "UserDataList", &list) ||
      json_object_get_type(list) != json_type_array)
    return false;
  const size_t length = json_object_array_length(list);
  if (length > EVENT_ITEMS)
    return false;
  for (size_t i = 0; i < length; i++) {
    json_object *entry = json_object_array_get_idx(list, i), *value = NULL;
    jf_user_data *item = &out[*count];
    *item = (jf_user_data){0};
    if (!json_object_object_get_ex(entry, "ItemId", &value))
      continue;
    /* The API's own ids: hex, without the dashes a Guid may be sent with. */
    size_t at = 0;
    for (const char *c = json_object_get_string(value);
         c != NULL && *c != '\0' && at + 1 < sizeof(item->item_id); c++)
      if (*c != '-')
        item->item_id[at++] = (char)tolower((unsigned char)*c);
    double number = 0;
    item->played = json_flag(entry, "Played");
    if (json_number(entry, "PlaybackPositionTicks", &number) && number > 0)
      item->position_ticks = (uint64_t)number;
    item->has_played_percentage =
        json_number(entry, "PlayedPercentage", &item->played_percentage);
    item->has_unplayed_item_count =
        json_number(entry, "UnplayedItemCount", &number);
    item->unplayed_item_count = (uint32_t)number;
    (*count)++;
  }
  return true;
}

static void event_message(void) {
  static jf_user_data items[EVENT_ITEMS];
  const char *type = NULL;
  json_object *root =
      events.overflowed
          ? NULL
          : json_tokener_parse((const char *)events.message.bytes);
  json_object *value = NULL;
  if (root != NULL && json_object_object_get_ex(root, "MessageType", &value))
    type = json_object_get_string(value);
  size_t count = 0;
  if (same(type, "UserDataChanged") && event_user_data(root, items, &count))
    events.changed(items, count);
  else if (events.overflowed || same(type, "UserDataChanged") ||
           same(type, "LibraryChanged"))
    events.changed(NULL, 0);
  json_object_put(root);
}

static size_t event_data(char *data, size_t size, size_t count, void *user) {
  const size_t length = size * count;
  const struct curl_ws_frame *frame = curl_ws_meta(user);
  if (frame == NULL || !(frame->flags & CURLWS_TEXT))
    return length;
  /* The first word from the server is its keep-alive interval: connected. */
  if (!events.connected) {
    events.connected = true;
    fprintf(stderr, "events: connected\n");
    if (events.ever_connected)
      events.changed(NULL, 0);
    events.ever_connected = true;
  }
  if (events.message.size + length > EVENT_MESSAGE_MAX)
    events.overflowed = true;
  else if (collect(data, 1, length, &events.message) != length)
    events.overflowed = true;
  if (frame->bytesleft == 0 && !(frame->flags & CURLWS_CONT)) {
    event_message();
    events.message.size = 0;
    events.overflowed = false;
  }
  return length;
}

static int event_tick(void *user, curl_off_t a, curl_off_t b, curl_off_t c,
                      curl_off_t d) {
  (void)a, (void)b, (void)c, (void)d;
  if (atomic_load(&events.stop))
    return 1;
  /* The server drops a socket silent for its interval, a minute by default. */
  const time_t now = time(NULL);
  if (events.connected && now >= events.keep_alive_at) {
    static const char keep_alive[] = "{\"MessageType\":\"KeepAlive\"}";
    size_t sent = 0;
    curl_ws_send(user, keep_alive, sizeof(keep_alive) - 1, &sent, 0,
                 CURLWS_TEXT);
    events.keep_alive_at = now + 30;
  }
  return 0;
}

static void *events_thread(void *unused) {
  (void)unused;
  pthread_once(&curl_once, curl_init);
  CURL *curl = curl_easy_init();
  if (curl == NULL)
    fprintf(stderr, "events: no curl\n");
  char authorization[768], header[832], base[512], url[600];
  jf_session_authorization(&events.session, authorization,
                           sizeof(authorization));
  snprintf(header, sizeof(header), "Authorization: %s", authorization);
  struct curl_slist *headers = curl_slist_append(NULL, header);
  /* ws:// for http://, wss:// for https://. */
  const char *address = jf_session_base(&events.session, base, sizeof(base));
  snprintf(url, sizeof(url), "ws%s/socket", address + 4);
  unsigned backoff = 1;
  if (strncmp(address, "http", 4) != 0)
    atomic_store(&events.stop, true);
  while (curl != NULL && !atomic_load(&events.stop)) {
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, event_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, curl);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, event_tick);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, curl);
    events.connected = false;
    events.keep_alive_at = time(NULL) + 30;
    events.message.size = 0;
    events.overflowed = false;
    const CURLcode result = curl_easy_perform(curl);
    if (atomic_load(&events.stop))
      break;
    fprintf(stderr, "events: %s, again in %u s\n", curl_easy_strerror(result),
            backoff);
    backoff = events.connected ? 1 : backoff;
    for (unsigned i = 0; i < backoff && !atomic_load(&events.stop); i++)
      sleep(1);
    backoff = backoff < 60 ? backoff * 2 : 60;
  }
  curl_slist_free_all(headers);
  if (curl != NULL)
    curl_easy_cleanup(curl);
  free(events.message.bytes);
  events.message = (response){NULL, 0, 0};
  return NULL;
}

void jf_events_stop(void) {
  if (!events.running)
    return;
  atomic_store(&events.stop, true);
  pthread_join(events.thread, NULL);
  events.running = false;
}

void jf_events_start(const jf_session *session,
                     void (*changed)(const jf_user_data *items, size_t count)) {
  jf_events_stop();
  events.session = *session;
  events.changed = changed;
  events.ever_connected = false;
  atomic_store(&events.stop, false);
  events.running =
      pthread_create(&events.thread, NULL, events_thread, NULL) == 0;
}
