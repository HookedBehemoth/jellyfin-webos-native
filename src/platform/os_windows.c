#include "os.h"

#include <winsock2.h>

#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

int jf_os_mkdir(const char *path) { return _mkdir(path); }

int jf_os_fsync(int file) { return _commit(file); }

/* Reference counted, so calling it per entry point without cleanup is fine. */
static bool net_init(void) {
  WSADATA wsa;
  return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

bool jf_os_hostname(char *name, size_t size) {
  char host[256];
  if (!net_init() || gethostname(host, sizeof(host)) != 0)
    return false;
  host[sizeof(host) - 1] = '\0';
  snprintf(name, size, "%s", host);
  return true;
}

jf_os_socket jf_os_udp_broadcast(unsigned short port, const void *data,
                                 size_t size, int timeout_ms) {
  if (!net_init())
    return -1;
  const SOCKET socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd == INVALID_SOCKET)
    return -1;
  const int yes = 1;
  const DWORD timeout = (DWORD)timeout_ms;
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = INADDR_BROADCAST;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, (const char *)&yes,
                 sizeof(yes)) != 0 ||
      setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                 sizeof(timeout)) != 0 ||
      sendto(socket_fd, data, (int)size, 0, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
    closesocket(socket_fd);
    return -1;
  }
  return (jf_os_socket)socket_fd;
}

ssize_t jf_os_udp_recv(jf_os_socket socket, void *buffer, size_t size) {
  return recv((SOCKET)socket, buffer, (int)size, 0);
}

void jf_os_udp_close(jf_os_socket socket) { closesocket((SOCKET)socket); }

/* Segoe UI is the system face every supported Windows carries; the rest are for when it
 * has been removed. */
static const char *const candidates[] = {
    "C:/Windows/Fonts/segoeui.ttf",
    "C:/Windows/Fonts/arial.ttf",
    "C:/Windows/Fonts/tahoma.ttf",
};

static const char *const fallbacks[] = {
    "C:/Windows/Fonts/seguisym.ttf",
    "C:/Windows/Fonts/arial.ttf",
};

const char *const *jf_os_font_candidates(size_t *count)
{
    *count = sizeof(candidates) / sizeof(*candidates);
    return candidates;
}

const char *const *jf_os_font_fallbacks(size_t *count)
{
    *count = sizeof(fallbacks) / sizeof(*fallbacks);
    return fallbacks;
}
