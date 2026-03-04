#ifndef P2P_TLS_H
#define P2P_TLS_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct p2p_tls_ctx_s  p2p_tls_ctx_t;
typedef struct p2p_tls_conn_s p2p_tls_conn_t;

/* TLS context (wraps SSL_CTX, one per process) */
p2p_tls_ctx_t  *p2p_tls_ctx_create(void);
void            p2p_tls_ctx_destroy(p2p_tls_ctx_t *ctx);

/* TLS connection lifecycle */
p2p_tls_conn_t *p2p_tls_connect(p2p_tls_ctx_t *ctx, const char *host, uint16_t port);
void            p2p_tls_shutdown(p2p_tls_conn_t *conn);
void            p2p_tls_close(p2p_tls_conn_t *conn);
int             p2p_tls_write(p2p_tls_conn_t *conn, const void *buf, int len);
int             p2p_tls_read(p2p_tls_conn_t *conn, void *buf, int len);

/*
 * HTTP/1.1 helpers built on top of TLS.
 *
 * p2p_https_post  -- POST JSON, read full response body into resp_buf.
 *                    Returns HTTP status code (e.g. 200) or -1 on error.
 *
 * p2p_https_get_sse -- Send GET for text/event-stream.
 *                      Returns 0 on success (headers consumed), -1 on error.
 *
 * p2p_sse_read_event -- Block until the next SSE event arrives.
 *                       Fills event_type ("ping", "ice_offer", ...) and
 *                       event_data (JSON string).
 *                       Returns 0 on success, -1 on connection closed/error.
 */
int  p2p_https_post(p2p_tls_conn_t *conn, const char *host,
                    const char *path, const char *json_body,
                    char *resp_buf, size_t resp_buf_sz);

int  p2p_https_get_sse(p2p_tls_conn_t *conn, const char *host,
                       const char *path);

int  p2p_sse_read_event(p2p_tls_conn_t *conn,
                        char *event_type, size_t et_sz,
                        char *event_data, size_t ed_sz);

#ifdef __cplusplus
}
#endif

#endif /* P2P_TLS_H */
