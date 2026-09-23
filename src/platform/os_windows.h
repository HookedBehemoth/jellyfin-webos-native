/* The Windows side of os.h. Include os.h, not this. */
#pragma once

/* winsock2.h first, or an indirect windows.h pulls in the older winsock instead. */
#include <winsock2.h>
#include <ws2tcpip.h>

#include <direct.h>
#include <io.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

static inline int jf_os_mkdir(const char *path) { return _mkdir(path); }

static inline int jf_os_fsync(int file) { return _commit(file); }

/* Unsigned, so the usual `fd < 0` test would never fire. */
typedef SOCKET jf_os_socket;

/* Winsock counts its own callers, so starting it more than once is safe. */
static inline bool jf_os_net_init(void)
{
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
}

static inline bool jf_os_socket_valid(jf_os_socket socket_fd)
{
    return socket_fd != INVALID_SOCKET;
}

static inline void jf_os_socket_close(jf_os_socket socket_fd) { closesocket(socket_fd); }

/* Milliseconds here, where POSIX takes a struct timeval. */
static inline int jf_os_socket_recv_timeout(jf_os_socket socket_fd, int milliseconds)
{
    const DWORD timeout = (DWORD)milliseconds;
    return setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
                      sizeof(timeout));
}

static inline int jf_os_socket_broadcast(jf_os_socket socket_fd)
{
    const int yes = 1;
    return setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));
}
