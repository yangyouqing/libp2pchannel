/**
 * xqc_tls_ctx_mbedtls.c -- TLS context management for mbedTLS backend.
 * Replaces xqc_tls_ctx.c when SSL_TYPE=mbedtls.
 */

#include "src/tls/xqc_tls_ctx.h"
#include "src/tls/xqc_tls_defs.h"
#include "src/common/xqc_malloc.h"

#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <string.h>

typedef struct xqc_tls_ctx_s {
    xqc_tls_type_t              type;

    xqc_engine_ssl_config_t     cfg;

    xqc_tls_callbacks_t         tls_cbs;

    xqc_ssl_session_ticket_key_t session_ticket_key;

    xqc_log_t                  *log;

    unsigned char              *alpn_list;
    size_t                      alpn_list_sz;
    size_t                      alpn_list_len;

    /* mbedTLS objects for server cert/key */
    mbedtls_x509_crt            cert;
    mbedtls_pk_context          pkey;
    int                         cert_loaded;

    /* RNG */
    mbedtls_entropy_context     entropy;
    mbedtls_ctr_drbg_context    ctr_drbg;
} xqc_tls_ctx_t;


static xqc_int_t
xqc_tls_ctx_set_config(xqc_tls_ctx_t *ctx, const xqc_engine_ssl_config_t *src)
{
    xqc_engine_ssl_config_t *dst = &ctx->cfg;

    dst->session_timeout = src->session_timeout;

    if (src->ciphers && *src->ciphers) {
        int len = strlen(src->ciphers) + 1;
        dst->ciphers = (char *)xqc_malloc(len);
        if (!dst->ciphers) return -XQC_EMALLOC;
        memcpy(dst->ciphers, src->ciphers, len);
    } else {
        int len = sizeof(XQC_TLS_CIPHERS);
        dst->ciphers = (char *)xqc_malloc(len);
        if (!dst->ciphers) return -XQC_EMALLOC;
        memcpy(dst->ciphers, XQC_TLS_CIPHERS, len);
    }

    if (src->groups && *src->groups) {
        int len = strlen(src->groups) + 1;
        dst->groups = (char *)xqc_malloc(len);
        if (!dst->groups) return -XQC_EMALLOC;
        memcpy(dst->groups, src->groups, len);
    } else {
        int len = sizeof(XQC_TLS_GROUPS);
        dst->groups = (char *)xqc_malloc(len);
        if (!dst->groups) return -XQC_EMALLOC;
        memcpy(dst->groups, XQC_TLS_GROUPS, len);
    }

    if (ctx->type == XQC_TLS_TYPE_SERVER) {
        if (src->private_key_file && *src->private_key_file) {
            int len = strlen(src->private_key_file) + 1;
            dst->private_key_file = (char *)xqc_malloc(len);
            if (!dst->private_key_file) return -XQC_EMALLOC;
            memcpy(dst->private_key_file, src->private_key_file, len);
        } else {
            xqc_log(ctx->log, XQC_LOG_ERROR, "|no private key file|");
            return -XQC_TLS_INVALID_ARGUMENT;
        }

        if (src->cert_file && *src->cert_file) {
            int len = strlen(src->cert_file) + 1;
            dst->cert_file = (char *)xqc_malloc(len);
            if (!dst->cert_file) return -XQC_EMALLOC;
            memcpy(dst->cert_file, src->cert_file, len);
        } else {
            xqc_log(ctx->log, XQC_LOG_ERROR, "|no cert file|");
            return -XQC_TLS_INVALID_ARGUMENT;
        }

        if (src->session_ticket_key_len > 0 && src->session_ticket_key_data) {
            dst->session_ticket_key_len = src->session_ticket_key_len;
            dst->session_ticket_key_data = (char *)xqc_malloc(src->session_ticket_key_len);
            if (!dst->session_ticket_key_data) return -XQC_EMALLOC;
            memcpy(dst->session_ticket_key_data, src->session_ticket_key_data,
                src->session_ticket_key_len);
        }
    }

    return XQC_OK;
}

xqc_tls_ctx_t *
xqc_tls_ctx_create(xqc_tls_type_t type, const xqc_engine_ssl_config_t *cfg,
    const xqc_tls_callbacks_t *cbs, xqc_log_t *log)
{
    xqc_tls_ctx_t *ctx = xqc_calloc(1, sizeof(xqc_tls_ctx_t));
    if (!ctx) return NULL;

    ctx->type = type;
    ctx->tls_cbs = *cbs;
    ctx->log = log;

    /* RNG init */
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);
    if (mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, mbedtls_entropy_func,
                               &ctx->entropy, (const unsigned char *)"xquic", 5) != 0)
    {
        xqc_log(log, XQC_LOG_ERROR, "|ctr_drbg_seed failed|");
        goto fail;
    }

    mbedtls_x509_crt_init(&ctx->cert);
    mbedtls_pk_init(&ctx->pkey);

    xqc_int_t ret = xqc_tls_ctx_set_config(ctx, cfg);
    if (ret != XQC_OK) goto fail;

    if (type == XQC_TLS_TYPE_SERVER) {
        int r = mbedtls_x509_crt_parse_file(&ctx->cert, ctx->cfg.cert_file);
        if (r != 0) {
            xqc_log(log, XQC_LOG_ERROR, "|parse cert file failed|ret:%d", r);
            goto fail;
        }
        r = mbedtls_pk_parse_keyfile(&ctx->pkey, ctx->cfg.private_key_file,
                                      NULL, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);
        if (r != 0) {
            xqc_log(log, XQC_LOG_ERROR, "|parse key file failed|ret:%d", r);
            goto fail;
        }
        ctx->cert_loaded = 1;
    }

    return ctx;

fail:
    xqc_tls_ctx_destroy(ctx);
    return NULL;
}


static void
xqc_tls_ctx_free_cfg(xqc_tls_ctx_t *ctx)
{
    xqc_engine_ssl_config_t *cfg = &ctx->cfg;
    if (cfg->ciphers)                xqc_free(cfg->ciphers);
    if (cfg->groups)                 xqc_free(cfg->groups);
    if (cfg->private_key_file)       xqc_free(cfg->private_key_file);
    if (cfg->cert_file)              xqc_free(cfg->cert_file);
    if (cfg->session_ticket_key_data) xqc_free(cfg->session_ticket_key_data);
}


void
xqc_tls_ctx_destroy(xqc_tls_ctx_t *ctx)
{
    if (!ctx) return;

    xqc_tls_ctx_free_cfg(ctx);

    mbedtls_x509_crt_free(&ctx->cert);
    mbedtls_pk_free(&ctx->pkey);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);

    if (ctx->alpn_list) xqc_free(ctx->alpn_list);

    xqc_free(ctx);
}


void *
xqc_tls_ctx_get_ssl_ctx(xqc_tls_ctx_t *ctx)
{
    return NULL;
}


xqc_tls_type_t
xqc_tls_ctx_get_type(xqc_tls_ctx_t *ctx)
{
    return ctx->type;
}


void
xqc_tls_ctx_get_tls_callbacks(xqc_tls_ctx_t *ctx, xqc_tls_callbacks_t **tls_cbs)
{
    *tls_cbs = &ctx->tls_cbs;
}


void
xqc_tls_ctx_get_session_ticket_key(xqc_tls_ctx_t *ctx, xqc_ssl_session_ticket_key_t **stk)
{
    *stk = &ctx->session_ticket_key;
}


void
xqc_tls_ctx_get_cfg(xqc_tls_ctx_t *ctx, xqc_engine_ssl_config_t **cfg)
{
    *cfg = &ctx->cfg;
}


void
xqc_tls_ctx_get_alpn_list(xqc_tls_ctx_t *ctx, unsigned char **alpn_list, size_t *alpn_list_len)
{
    *alpn_list = ctx->alpn_list;
    *alpn_list_len = ctx->alpn_list_len;
}


/* access helpers used by xqc_tls_mbedtls.c */
mbedtls_x509_crt *xqc_tls_ctx_get_cert(xqc_tls_ctx_t *ctx) { return &ctx->cert; }
mbedtls_pk_context *xqc_tls_ctx_get_pkey(xqc_tls_ctx_t *ctx) { return &ctx->pkey; }
mbedtls_ctr_drbg_context *xqc_tls_ctx_get_rng(xqc_tls_ctx_t *ctx) { return &ctx->ctr_drbg; }
int xqc_tls_ctx_is_cert_loaded(xqc_tls_ctx_t *ctx) { return ctx->cert_loaded; }


xqc_int_t
xqc_tls_ctx_register_alpn(xqc_tls_ctx_t *ctx, const char *alpn, size_t alpn_len)
{
    if (!alpn || !alpn_len) return -XQC_EPARAM;

    if (alpn_len + 1 > ctx->alpn_list_sz - ctx->alpn_list_len) {
        size_t new_sz = 2 * (ctx->alpn_list_sz + alpn_len) + 1;
        char *new_list = xqc_malloc(new_sz);
        if (!new_list) return -XQC_EMALLOC;
        ctx->alpn_list_sz = new_sz;
        if (ctx->alpn_list) {
            xqc_memcpy(new_list, ctx->alpn_list, ctx->alpn_list_len);
            xqc_free(ctx->alpn_list);
        }
        ctx->alpn_list = (unsigned char *)new_list;
    }

    snprintf((char *)ctx->alpn_list + ctx->alpn_list_len,
             ctx->alpn_list_sz - ctx->alpn_list_len, "%c%s", (uint8_t)alpn_len, alpn);
    ctx->alpn_list_len = strlen((char *)ctx->alpn_list);

    return XQC_OK;
}


xqc_int_t
xqc_tls_ctx_unregister_alpn(xqc_tls_ctx_t *ctx, const char *alpn, size_t alpn_len)
{
    if (!alpn || !alpn_len) return -XQC_EPARAM;

    unsigned char *pos = ctx->alpn_list;
    unsigned char *end = ctx->alpn_list + ctx->alpn_list_len;
    while (pos < end) {
        size_t node_len = *pos;
        unsigned char *next = pos + node_len + 1;
        if (node_len == alpn_len && memcmp(pos + 1, alpn, alpn_len) == 0) {
            size_t remain = end - next;
            memmove(pos, next, remain);
            ctx->alpn_list_len -= alpn_len + 1;
            return XQC_OK;
        }
        pos = next;
    }

    return -XQC_EALPN_NOT_REGISTERED;
}
