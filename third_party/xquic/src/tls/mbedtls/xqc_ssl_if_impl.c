/**
 * xqc_ssl_if_impl.c -- mbedTLS stubs for SSL interface functions.
 * These functions are not used with the mbedTLS backend since the handshake
 * is driven directly, but they must exist to satisfy the linker.
 */

#include "src/tls/xqc_tls_defs.h"
#include "src/tls/xqc_tls_common.h"
