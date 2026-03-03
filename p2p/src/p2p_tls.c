#include "p2p_tls.h"
#include "p2p_platform.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct p2p_tls_ctx_s {
    SSL_CTX *ctx;
};

struct p2p_tls_conn_s {
    SSL          *ssl;
    p2p_socket_t  fd;
};

/* ------------------------------------------------------------------ */
/*  Context                                                           */
/* ------------------------------------------------------------------ */

p2p_tls_ctx_t *p2p_tls_ctx_create(void)
{
    p2p_tls_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->ctx = SSL_CTX_new(TLS_client_method());
    if (!c->ctx) { free(c); return NULL; }

    SSL_CTX_set_min_proto_version(c->ctx, TLS1_2_VERSION);
    /* Skip server cert verification for self-signed dev certs */
    SSL_CTX_set_verify(c->ctx, SSL_VERIFY_NONE, NULL);
    return c;
}

void p2p_tls_ctx_destroy(p2p_tls_ctx_t *c)
{
    if (!c) return;
    SSL_CTX_free(c->ctx);
    free(c);
}

/* ------------------------------------------------------------------ */
/*  Connection                                                        */
/* ------------------------------------------------------------------ */

static p2p_socket_t tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (getaddrinfo(host, port_str, &hints, &res) != 0)
        return P2P_INVALID_SOCKET;

    p2p_socket_t fd = P2P_INVALID_SOCKET;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd == P2P_INVALID_SOCKET) continue;
        if (connect(fd, rp->ai_addr, (int)rp->ai_addrlen) == 0) break;
        p2p_socket_close(fd);
        fd = P2P_INVALID_SOCKET;
    }
    freeaddrinfo(res);
    return fd;
}

p2p_tls_conn_t *p2p_tls_connect(p2p_tls_ctx_t *ctx,
                                 const char *host, uint16_t port)
{
    if (!ctx || !host) return NULL;

    p2p_socket_t fd = tcp_connect(host, port);
    if (fd == P2P_INVALID_SOCKET) return NULL;

    SSL *ssl = SSL_new(ctx->ctx);
    if (!ssl) { p2p_socket_close(fd); return NULL; }

    SSL_set_fd(ssl, (int)fd);
    SSL_set_tlsext_host_name(ssl, host);

    if (SSL_connect(ssl) != 1) {
        fprintf(stderr, "[p2p_tls] SSL_connect failed: ");
        ERR_print_errors_fp(stderr);
        SSL_free(ssl);
        p2p_socket_close(fd);
        return NULL;
    }

    p2p_tls_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) { SSL_free(ssl); p2p_socket_close(fd); return NULL; }
    conn->ssl = ssl;
    conn->fd  = fd;
    return conn;
}

void p2p_tls_close(p2p_tls_conn_t *conn)
{
    if (!conn) return;
    SSL_shutdown(conn->ssl);
    SSL_free(conn->ssl);
    p2p_socket_close(conn->fd);
    free(conn);
}

int p2p_tls_write(p2p_tls_conn_t *conn, const void *buf, int len)
{
    if (!conn) return -1;
    return SSL_write(conn->ssl, buf, len);
}

int p2p_tls_read(p2p_tls_conn_t *conn, void *buf, int len)
{
    if (!conn) return -1;
    return SSL_read(conn->ssl, buf, len);
}

/* ------------------------------------------------------------------ */
/*  Internal: read exactly n bytes                                    */
/* ------------------------------------------------------------------ */

static int tls_read_full(p2p_tls_conn_t *conn, char *buf, int need)
{
    int got = 0;
    while (got < need) {
        int n = SSL_read(conn->ssl, buf + got, need - got);
        if (n <= 0) return -1;
        got += n;
    }
    return got;
}

/* Read one line (up to '\n') into buf. Returns length or -1. */
static int tls_read_line(p2p_tls_conn_t *conn, char *buf, int buf_sz)
{
    int i = 0;
    while (i < buf_sz - 1) {
        int n = SSL_read(conn->ssl, buf + i, 1);
        if (n <= 0) return -1;
        if (buf[i] == '\n') { buf[i + 1] = '\0'; return i + 1; }
        i++;
    }
    buf[i] = '\0';
    return i;
}

/* ------------------------------------------------------------------ */
/*  HTTP/1.1 POST                                                     */
/* ------------------------------------------------------------------ */

int p2p_https_post(p2p_tls_conn_t *conn, const char *host,
                   const char *path, const char *json_body,
                   char *resp_buf, size_t resp_buf_sz)
{
    if (!conn || !host || !path || !json_body || !resp_buf) return -1;

    size_t body_len = strlen(json_body);
    char req[2048];
    int rlen = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "%s",
        path, host, body_len, json_body);

    if (rlen <= 0 || (size_t)rlen >= sizeof(req)) return -1;
    if (SSL_write(conn->ssl, req, rlen) != rlen) return -1;

    /* Read response headers */
    char hdr_buf[4096];
    int hdr_len = 0;
    while (hdr_len < (int)sizeof(hdr_buf) - 1) {
        int n = SSL_read(conn->ssl, hdr_buf + hdr_len, 1);
        if (n <= 0) return -1;
        hdr_len++;
        if (hdr_len >= 4 &&
            hdr_buf[hdr_len-4] == '\r' && hdr_buf[hdr_len-3] == '\n' &&
            hdr_buf[hdr_len-2] == '\r' && hdr_buf[hdr_len-1] == '\n')
            break;
    }
    hdr_buf[hdr_len] = '\0';

    /* Parse status code */
    int status = 0;
    if (sscanf(hdr_buf, "HTTP/1.1 %d", &status) != 1 &&
        sscanf(hdr_buf, "HTTP/1.0 %d", &status) != 1)
        return -1;

    /* Parse Content-Length */
    int content_len = 0;
    const char *cl = strstr(hdr_buf, "Content-Length:");
    if (!cl) cl = strstr(hdr_buf, "content-length:");
    if (cl) content_len = atoi(cl + 15);

    if (content_len <= 0 || (size_t)content_len >= resp_buf_sz) {
        /* No body or too large -- drain what we can */
        if (content_len > 0 && (size_t)content_len < resp_buf_sz) {
            if (tls_read_full(conn, resp_buf, content_len) < 0) return -1;
            resp_buf[content_len] = '\0';
        } else {
            resp_buf[0] = '\0';
        }
        return status;
    }

    if (tls_read_full(conn, resp_buf, content_len) < 0) return -1;
    resp_buf[content_len] = '\0';
    return status;
}

/* ------------------------------------------------------------------ */
/*  HTTP/1.1 GET for SSE                                              */
/* ------------------------------------------------------------------ */

int p2p_https_get_sse(p2p_tls_conn_t *conn, const char *host,
                      const char *path)
{
    if (!conn || !host || !path) return -1;

    char req[2048];
    int rlen = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Accept: text/event-stream\r\n"
        "Connection: keep-alive\r\n"
        "\r\n",
        path, host);

    if (rlen <= 0 || (size_t)rlen >= sizeof(req)) return -1;
    if (SSL_write(conn->ssl, req, rlen) != rlen) return -1;

    /* Read and discard response headers until blank line */
    char hdr_buf[4096];
    int hdr_len = 0;
    while (hdr_len < (int)sizeof(hdr_buf) - 1) {
        int n = SSL_read(conn->ssl, hdr_buf + hdr_len, 1);
        if (n <= 0) return -1;
        hdr_len++;
        if (hdr_len >= 4 &&
            hdr_buf[hdr_len-4] == '\r' && hdr_buf[hdr_len-3] == '\n' &&
            hdr_buf[hdr_len-2] == '\r' && hdr_buf[hdr_len-1] == '\n')
            break;
    }
    hdr_buf[hdr_len] = '\0';

    int status = 0;
    if (sscanf(hdr_buf, "HTTP/1.1 %d", &status) != 1) return -1;
    if (status != 200) return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  SSE event reader                                                  */
/* ------------------------------------------------------------------ */

int p2p_sse_read_event(p2p_tls_conn_t *conn,
                       char *event_type, size_t et_sz,
                       char *event_data, size_t ed_sz)
{
    if (!conn || !event_type || !event_data) return -1;

    event_type[0] = '\0';
    event_data[0] = '\0';

    char line[8192];
    size_t data_off = 0;

    for (;;) {
        int n = tls_read_line(conn, line, sizeof(line));
        if (n < 0) return -1;

        /* Strip trailing \r\n */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            line[--n] = '\0';

        /* Blank line = end of event */
        if (n == 0) {
            if (event_type[0] != '\0' || data_off > 0)
                return 0;  /* event complete */
            continue;       /* spurious blank line, keep reading */
        }

        if (strncmp(line, "event:", 6) == 0) {
            const char *val = line + 6;
            while (*val == ' ') val++;
            snprintf(event_type, et_sz, "%s", val);
        } else if (strncmp(line, "data:", 5) == 0) {
            const char *val = line + 5;
            while (*val == ' ') val++;
            size_t vlen = strlen(val);
            if (data_off + vlen < ed_sz) {
                memcpy(event_data + data_off, val, vlen);
                data_off += vlen;
                event_data[data_off] = '\0';
            }
        }
        /* ignore id:, retry:, comments */
    }
}
