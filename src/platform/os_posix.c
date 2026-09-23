#include "os.h"

#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

int jf_os_mkdir(const char *path) { return mkdir(path, 0755); }

int jf_os_fsync(int file) { return fsync(file); }

bool jf_os_hostname(char *name, size_t size) {
  char host[256];
  if (gethostname(host, sizeof(host)) != 0)
    return false;
  host[sizeof(host) - 1] = '\0';
  snprintf(name, size, "%s", host);
  return true;
}

jf_os_socket jf_os_udp_broadcast(unsigned short port, const void *data,
                                 size_t size, int timeout_ms) {
  const int socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
  if (socket_fd < 0)
    return -1;
  const int yes = 1;
  const struct timeval timeout = {timeout_ms / 1000,
                                  (timeout_ms % 1000) * 1000};
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = INADDR_BROADCAST;
  if (setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) != 0 ||
      setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0 ||
      sendto(socket_fd, data, size, 0, (struct sockaddr *)&address,
             sizeof(address)) < 0) {
    close(socket_fd);
    return -1;
  }
  return socket_fd;
}

ssize_t jf_os_udp_recv(jf_os_socket socket, void *buffer, size_t size) {
  return recv((int)socket, buffer, size, 0);
}

void jf_os_udp_close(jf_os_socket socket) { close((int)socket); }

/* The TV's own face first, then what a desktop distribution is likely to carry. */
static const char *const candidates[] = {
    "/usr/share/fonts/LG_Smart_UI-Regular.ttf",
    "/usr/share/fonts/DroidSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/liberation2/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};

static const char *const fallbacks[] = {
    "/usr/share/fonts/DroidSansFallback.ttf",
    "/usr/share/fonts/DroidSans.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
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
