/*
 * Cross-compilation compatibility wrapper.
 * Redirects sys/socket.h to Winsock2 when cross-compiling with MinGW-w64.
 */
#ifndef _MINGW_COMPAT_SYS_SOCKET_H
#define _MINGW_COMPAT_SYS_SOCKET_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif
