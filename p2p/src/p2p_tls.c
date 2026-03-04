/**
 * p2p_tls.c -- TLS client for signaling (HTTPS) using mbedTLS.
 *
 * Implements the same p2p_tls.h interface but with mbedTLS instead of OpenSSL.
 */

#include "p2p_tls.h"
#include "p2p_platform.h"

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct p2p_tls_ctx_s {
    mbedtls_ssl_config      conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
};

struct p2p_tls_conn_s {
    mbedtls_ssl_context     ssl;
    mbedtls_net_context     net;
    p2p_tls_ctx_t          *ctx;
};


/* ------------------------------------------------------------------ */
/*  Context                                                           */
/* ------------------------------------------------------------------ */

p2p_tls_ctx_t *p2p_tls_ctx_create(void)
{
    p2p_tls_ctx_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    mbedtls_ssl_config_init(&c->conf);
    mbedtls_entropy_init(&c->entropy);
    mbedtls_ctr_drbg_init(&c->ctr_drbg);

    if (mbedtls_ctr_drbg_seed(&c->ctr_drbg, mbedtls_entropy_func,
                               &c->entropy, (const unsigned char *)"p2p_tls", 7) != 0)
    {
        goto fail;
    }

    if (mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT,
                                     MBEDTLS_SSL_TRANSPORT_STREAM,
                                     MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        goto fail;
    }

    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &c->ctr_drbg);
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_2);
    /* Disable SSL debug callback to avoid crashes with concurrent TLS connections. */
    mbedtls_ssl_conf_dbg(&c->conf, NULL, NULL);

    return c;

fail:
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
    mbedtls_ssl_config_free(&c->conf);
    free(c);
    return NULL;
}

void p2p_tls_ctx_destroy(p2p_tls_ctx_t *c)
{
    if (!c) return;
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_ctr_drbg_free(&c->ctr_drbg);
    mbedtls_entropy_free(&c->entropy);
    free(c);
}


/* ------------------------------------------------------------------ */
/*  Connection                                                        */
/* ------------------------------------------------------------------ */

p2p_tls_conn_t *p2p_tls_connect(p2p_tls_ctx_t *ctx,
                                 const char *host, uint16_t port)
{
    if (!ctx || !host) return NULL;

    p2p_tls_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) return NULL;
    conn->ctx = ctx;

    mbedtls_ssl_init(&conn->ssl);
    mbedtls_net_init(&conn->net);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    if (mbedtls_net_connect(&conn->net, host, port_str, MBEDTLS_NET_PROTO_TCP) != 0) {
        goto fail;
    }

    if (mbedtls_ssl_setup(&conn->ssl, &ctx->conf) != 0) {
        goto fail;
    }

    mbedtls_ssl_set_hostname(&conn->ssl, host);
    mbedtls_ssl_set_bio(&conn->ssl, &conn->net,
                         mbedtls_net_send, mbedtls_net_recv, NULL);

    int ret;
    while ((ret = mbedtls_ssl_handshake(&conn->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            char errbuf[128];
            mbedtls_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[p2p_tls] handshake failed: %s\n", errbuf);
            goto fail;
        }
    }

    return conn;

fail:
    mbedtls_ssl_free(&conn->ssl);
    mbedtls_net_free(&conn->net);
    free(conn);
    return NULL;
}

void p2p_tls_shutdown(p2p_tls_conn_t *conn)
{
    if (!conn) return;
    mbedtls_net_free(&conn->net);
}

void p2p_tls_close(p2p_tls_conn_t *conn)
{
    if (!conn) return;
    mbedtls_ssl_close_notify(&conn->ssl);
    mbedtls_ssl_free(&conn->ssl);
    mbedtls_net_free(&conn->net);
    free(conn);
}

int p2p_tls_write(p2p_tls_conn_t *conn, const void *buf, int len)
{
    if (!conn) return -1;
    int ret;
    while ((ret = mbedtls_ssl_write(&conn->ssl, buf, len)) <= 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return -1;
    }
    return ret;
}

int p2p_tls_read(p2p_tls_conn_t *conn, void *buf, int len)
{
    if (!conn) return -1;
    int ret;
    while ((ret = mbedtls_ssl_read(&conn->ssl, buf, len)) < 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
            return -1;
    }
    return ret;
}


/* ------------------------------------------------------------------ */
/*  Internal: read exactly n bytes                                    */
/* ------------------------------------------------------------------ */

static int tls_read_full(p2p_tls_conn_t *conn, char *buf, int need)
{
    int got = 0;
    while (got < need) {
        int n = p2p_tls_read(conn, buf + got, need - got);
        if (n <= 0) return -1;
        got += n;
    }
    return got;
}

static int tls_read_line(p2p_tls_conn_t *conn, char *buf, int buf_sz)
{
    int i = 0;
    while (i < buf_sz - 1) {
        int n = p2p_tls_read(conn, buf + i, 1);
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
    if (p2p_tls_write(conn, req, rlen) != rlen) return -1;

    /* Read response headers */
    char hdr_buf[4096];
    int hdr_len = 0;
    while (hdr_len < (int)sizeof(hdr_buf) - 1) {
        int n = p2p_tls_read(conn, hdr_buf + hdr_len, 1);
        if (n <= 0) return -1;
        hdr_len++;
        if (hdr_len >= 4 &&
            hdr_buf[hdr_len-4] == '\r' && hdr_buf[hdr_len-3] == '\n' &&
            hdr_buf[hdr_len-2] == '\r' && hdr_buf[hdr_len-1] == '\n')
            break;
    }
    hdr_buf[hdr_len] = '\0';

    int status = 0;
    if (sscanf(hdr_buf, "HTTP/1.1 %d", &status) != 1 &&
        sscanf(hdr_buf, "HTTP/1.0 %d", &status) != 1)
        return -1;

    int content_len = 0;
    const char *cl = strstr(hdr_buf, "Content-Length:");
    if (!cl) cl = strstr(hdr_buf, "content-length:");
    if (cl) content_len = atoi(cl + 15);

    if (content_len <= 0 || (size_t)content_len >= resp_buf_sz) {
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

    if (rlen <= 0 || (size_t)rlen >= sizeof(req)) {
        fprintf(stderr, "[sig] SSE request too long (%d bytes)\n", rlen);
        return -1;
    }
    if (p2p_tls_write(conn, req, rlen) != rlen) {
        fprintf(stderr, "[sig] SSE write failed\n");
        return -1;
    }

    char hdr_buf[4096];
    int hdr_len = 0;
    while (hdr_len < (int)sizeof(hdr_buf) - 1) {
        int n = p2p_tls_read(conn, hdr_buf + hdr_len, 1);
        if (n <= 0) {
            fprintf(stderr, "[sig] SSE read failed at byte %d (n=%d)\n", hdr_len, n);
            return -1;
        }
        hdr_len++;
        if (hdr_len >= 4 &&
            hdr_buf[hdr_len-4] == '\r' && hdr_buf[hdr_len-3] == '\n' &&
            hdr_buf[hdr_len-2] == '\r' && hdr_buf[hdr_len-1] == '\n')
            break;
    }
    hdr_buf[hdr_len] = '\0';

    int status = 0;
    if (sscanf(hdr_buf, "HTTP/1.1 %d", &status) != 1) {
        fprintf(stderr, "[sig] SSE: cannot parse HTTP status from: %.80s\n", hdr_buf);
        return -1;
    }
    if (status != 200) {
        fprintf(stderr, "[sig] SSE: HTTP %d\n%.200s\n", status, hdr_buf);
        return -1;
    }

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

        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r'))
            line[--n] = '\0';

        if (n == 0) {
            if (event_type[0] != '\0' || data_off > 0)
                return 0;
            continue;
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
    }
}
