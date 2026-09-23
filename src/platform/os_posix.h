/* The POSIX side of os.h. Include os.h, not this. */
#pragma once

#include <arpa/inet.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

static inline int jf_os_mkdir(const char *path) { return mkdir(path, 0755); }

static inline int jf_os_fsync(int file) { return fsync(file); }

typedef int jf_os_socket;

static inline bool jf_os_net_init(void) { return true; }

static inline bool jf_os_socket_valid(jf_os_socket socket_fd) { return socket_fd >= 0; }

static inline void jf_os_socket_close(jf_os_socket socket_fd) { close(socket_fd); }

static inline int jf_os_socket_recv_timeout(jf_os_socket socket_fd, int milliseconds)
{
    const struct timeval timeout = {milliseconds / 1000, (milliseconds % 1000) * 1000};
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
}

static inline int jf_os_socket_broadcast(jf_os_socket socket_fd)
{
    const int yes = 1;
    return setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
}

