/*
 * Cross-compilation compatibility wrapper.
 * arpa/inet.h functions are provided by Winsock2 on Windows.
 */
#ifndef _MINGW_COMPAT_ARPA_INET_H
#define _MINGW_COMPAT_ARPA_INET_H

#include <winsock2.h>
#include <ws2tcpip.h>

#endif
