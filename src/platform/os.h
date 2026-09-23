/* Operating-system calls that differ per platform, defined in os_posix.c or
 * os_windows.c. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

int jf_os_mkdir(const char *path);
int jf_os_fsync(int file);
bool jf_os_hostname(char *name, size_t size);

/* Broadcasts data to port; each receive then times out after timeout_ms.
 * Negative on failure. */
typedef intptr_t jf_os_socket;
jf_os_socket jf_os_udp_broadcast(unsigned short port, const void *data,
                                 size_t size, int timeout_ms);
ssize_t jf_os_udp_recv(jf_os_socket socket, void *buffer, size_t size);
void jf_os_udp_close(jf_os_socket socket);

const char *const *jf_os_font_candidates(size_t *count);
const char *const *jf_os_font_fallbacks(size_t *count);
