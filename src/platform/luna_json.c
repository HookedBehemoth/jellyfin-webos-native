/* One field out of a Luna payload, without a JSON parser for it.
 *
 * Kept apart from luna.c so the host test can link it without GLib or libhelpers - the
 * rule it encodes (a key matched inside some other value is not a match) is the part
 * worth testing. */
#include "luna.h"

#include <stdio.h>
#include <string.h>

bool jf_luna_json_string(const char *payload, const char *key, char *out, size_t out_len)
{
    char quoted[64];
    if ((size_t)snprintf(quoted, sizeof(quoted), "\"%s\"", key) >= sizeof(quoted))
        return false;
    const char *at = strstr(payload, quoted);
    if (at == NULL)
        return false;
    at += strlen(quoted);
    while (*at == ' ' || *at == ':')
        at++;
    if (*at != '"') /* not a string value, or the key was matched inside one */
        return false;
    at++;
    const char *end = strchr(at, '"');
    if (end == NULL || (size_t)(end - at) >= out_len)
        return false;
    memcpy(out, at, (size_t)(end - at));
    out[end - at] = '\0';
    return true;
}

bool jf_luna_json_bool(const char *payload, const char *key, bool *out) {
  char quoted[64];
  if ((size_t)snprintf(quoted, sizeof(quoted), "\"%s\"", key) >= sizeof(quoted))
    return false;
  const char *at = strstr(payload, quoted);
  if (at == NULL)
    return false;
  at += strlen(quoted);
  while (*at == ' ')
    at++;
  if (*at++ != ':') /* the key was matched as a value */
    return false;
  while (*at == ' ')
    at++;
  if (strncmp(at, "true", 4) == 0)
    *out = true;
  else if (strncmp(at, "false", 5) == 0)
    *out = false;
  else
    return false;
  return true;
}
