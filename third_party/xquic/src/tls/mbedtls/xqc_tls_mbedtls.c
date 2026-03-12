/**
 * xqc_tls_mbedtls.c -- QUIC-TLS 1.3 handshake adapter using mbedTLS primitives.
 *
 * Implements the same public interface as xqc_tls.c, driving a TLS 1.3
 * handshake directly with mbedTLS's crypto primitives (HKDF, ECDH, X.509,
 * AES-GCM, SHA-256/384) without going through mbedTLS's TLS record layer.
 *
 * Replaces xqc_tls.c when SSL_TYPE=mbedtls.
 */

#include "src/tls/xqc_tls.h"
#include "src/tls/xqc_tls_ctx.h"
#include "src/tls/xqc_tls_defs.h"
#include "src/tls/xqc_tls_common.h"
#include "src/tls/xqc_crypto.h"
#include "src/tls/xqc_hkdf.h"
#include "src/common/xqc_common.h"
#include "src/common/xqc_malloc.h"
#include "src/transport/xqc_conn.h"

#include <mbedtls/ecdh.h>
#include <mbedtls/md.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>

#include <string.h>
#include <stdlib.h>

/* ---- extern helpers from xqc_tls_ctx_mbedtls.c ---- */
mbedtls_x509_crt      *xqc_tls_ctx_get_cert(xqc_tls_ctx_t *ctx);
mbedtls_pk_context     *xqc_tls_ctx_get_pkey(xqc_tls_ctx_t *ctx);
mbedtls_ctr_drbg_context *xqc_tls_ctx_get_rng(xqc_tls_ctx_t *ctx);
int                     xqc_tls_ctx_is_cert_loaded(xqc_tls_ctx_t *ctx);


/* ---- Constants ---- */
#define TLS_HS_CLIENT_HELLO         1
#define TLS_HS_SERVER_HELLO         2
#define TLS_HS_ENCRYPTED_EXTENSIONS 8
#define TLS_HS_CERTIFICATE          11
#define TLS_HS_CERTIFICATE_VERIFY   15
#define TLS_HS_FINISHED             20

#define TLS_EXT_SERVER_NAME         0x0000
#define TLS_EXT_SUPPORTED_GROUPS    0x000a
#define TLS_EXT_SIGNATURE_ALGORITHMS 0x000d
#define TLS_EXT_ALPN                0x0010
#define TLS_EXT_SUPPORTED_VERSIONS  0x002b
#define TLS_EXT_KEY_SHARE           0x0033

/* QUIC transport parameters extension */
#define TLS_EXT_QUIC_TP_V1         0x0039
#define TLS_EXT_QUIC_TP_DRAFT      0xffa5

/* TLS 1.3 version */
#define TLS13_VERSION              0x0304

/* X25519 named group */
#define TLS_GROUP_X25519           0x001d

/* Signature algorithms */
#define TLS_SIG_RSA_PSS_RSAE_SHA256  0x0804
#define TLS_SIG_ECDSA_SECP256R1_SHA256 0x0403

/* Cipher suites */
#define TLS_CS_AES_128_GCM_SHA256   0x1301
#define TLS_CS_AES_256_GCM_SHA384   0x1302

#define REASM_BUF_SIZE 16384
#define MAX_HS_MSG_SIZE 16384

/* ---- Handshake state ---- */
typedef enum {
    MTLS_STATE_INIT,
    MTLS_CLI_HELLO_SENT,
    MTLS_CLI_WAIT_SH,
    MTLS_CLI_WAIT_EE,
    MTLS_CLI_WAIT_CERT_OR_FIN,
    MTLS_CLI_WAIT_CV,
    MTLS_CLI_WAIT_FIN,
    MTLS_SVR_WAIT_CH,
    MTLS_SVR_WAIT_FIN,
    MTLS_HANDSHAKE_DONE,
} xqc_mtls_state_t;


/* ---- TLS instance ---- */
struct xqc_tls_s {
    xqc_tls_ctx_t         *ctx;
    xqc_tls_type_t         type;
    xqc_log_t             *log;
    xqc_tls_callbacks_t   *cbs;
    void                  *user_data;

    xqc_crypto_t          *crypto[XQC_ENC_LEV_MAX];
    xqc_mtls_state_t       state;

    xqc_bool_t             no_crypto;
    xqc_bool_t             resumption;
    xqc_bool_t             key_update_confirmed;
    xqc_bool_t             hsk_completed;
    xqc_bool_t             tp_received;
    uint8_t                cert_verify_flag;
    xqc_proto_version_t    version;

    /* X25519 ECDH */
    mbedtls_ecdh_context   ecdh;
    uint8_t                my_x25519_pub[32];
    uint8_t                peer_x25519_pub[32];
    uint8_t                shared_secret[32];
    int                    ecdh_done;

    /* Cipher suite selected */
    uint16_t               cipher_suite;
    uint32_t               cipher_id;
    mbedtls_md_type_t      md_type;
    int                    hash_len;

    /* Transcript hash */
    mbedtls_md_context_t   transcript;
    int                    transcript_inited;

    /* Key schedule secrets */
    uint8_t                early_secret[48];
    uint8_t                hs_secret[48];
    uint8_t                master_secret[48];
    uint8_t                c_hs_secret[48];
    uint8_t                s_hs_secret[48];
    uint8_t                c_ap_secret[48];
    uint8_t                s_ap_secret[48];

    /* ALPN */
    char                   alpn[128];
    size_t                 alpn_len;
    char                   selected_alpn[128];
    size_t                 selected_alpn_len;

    /* Transport parameters */
    uint8_t               *local_tp;
    size_t                 local_tp_len;

    /* SNI hostname */
    char                   hostname[256];

    /* Server certificate (for verification) */
    mbedtls_x509_crt       peer_cert;
    int                    peer_cert_inited;

    /* Reassembly buffer for handshake messages per level */
    uint8_t                reasm[XQC_ENC_LEV_MAX][REASM_BUF_SIZE];
    size_t                 reasm_len[XQC_ENC_LEV_MAX];

    /* Saved ClientHello for transcript (client only) */
    uint8_t                saved_ch[2048];
    size_t                 saved_ch_len;

    /* Random values */
    uint8_t                client_random[32];
    uint8_t                server_random[32];

    /* Server's Finished verify_data (for client to check) */
    uint8_t                svr_finished_key[48];
};


/* ---- Helpers ---- */

static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v & 0xff; }
static void put24(uint8_t *p, uint32_t v) { p[0] = (v >> 16) & 0xff; p[1] = (v >> 8) & 0xff; p[2] = v & 0xff; }
static uint16_t get16(const uint8_t *p) { return (p[0] << 8) | p[1]; }
static uint32_t get24(const uint8_t *p) { return (p[0] << 16) | (p[1] << 8) | p[2]; }


/* ---- Transcript hash ---- */

static xqc_int_t
transcript_init(xqc_tls_t *tls, mbedtls_md_type_t md_type)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(md_type);
    if (!info) return -XQC_TLS_INTERNAL;

    mbedtls_md_init(&tls->transcript);
    if (mbedtls_md_setup(&tls->transcript, info, 0) != 0) return -XQC_TLS_INTERNAL;
    if (mbedtls_md_starts(&tls->transcript) != 0) return -XQC_TLS_INTERNAL;
    tls->transcript_inited = 1;
    return XQC_OK;
}

static void
transcript_update(xqc_tls_t *tls, const uint8_t *data, size_t len)
{
    if (tls->transcript_inited) {
        mbedtls_md_update(&tls->transcript, data, len);
    }
}

static xqc_int_t
transcript_hash(xqc_tls_t *tls, uint8_t *out)
{
    mbedtls_md_context_t clone;
    mbedtls_md_init(&clone);
    mbedtls_md_setup(&clone, mbedtls_md_info_from_type(tls->md_type), 0);
    mbedtls_md_clone(&clone, &tls->transcript);
    mbedtls_md_finish(&clone, out);
    mbedtls_md_free(&clone);
    return XQC_OK;
}


/* ---- TLS 1.3 Key Schedule ---- */

static xqc_int_t
hkdf_expand_label(xqc_tls_t *tls, uint8_t *out, size_t out_len,
    const uint8_t *secret, size_t secret_len,
    const char *label, const uint8_t *ctx_hash, size_t ctx_len)
{
    uint8_t info[256];
    const char *prefix = "tls13 ";
    size_t label_len = strlen(label);
    size_t prefix_len = strlen(prefix);
    size_t total_label_len = prefix_len + label_len;

    size_t info_len = 0;
    info[info_len++] = (out_len >> 8) & 0xff;
    info[info_len++] = out_len & 0xff;
    info[info_len++] = (uint8_t)total_label_len;
    memcpy(info + info_len, prefix, prefix_len); info_len += prefix_len;
    memcpy(info + info_len, label, label_len); info_len += label_len;
    info[info_len++] = (uint8_t)ctx_len;
    if (ctx_len > 0) {
        memcpy(info + info_len, ctx_hash, ctx_len);
        info_len += ctx_len;
    }

    xqc_digest_t md;
    md.md_type = tls->md_type;
    return xqc_hkdf_expand(out, out_len, secret, secret_len, info, info_len, &md);
}

static xqc_int_t
derive_secret(xqc_tls_t *tls, uint8_t *out,
    const uint8_t *secret, const char *label)
{
    uint8_t th[48];
    transcript_hash(tls, th);
    return hkdf_expand_label(tls, out, tls->hash_len, secret, tls->hash_len, label, th, tls->hash_len);
}

static xqc_int_t
compute_finished_key(xqc_tls_t *tls, const uint8_t *base_secret, uint8_t *finished_key)
{
    return hkdf_expand_label(tls, finished_key, tls->hash_len, base_secret, tls->hash_len,
        "finished", NULL, 0);
}

static xqc_int_t
compute_finished_verify(xqc_tls_t *tls, const uint8_t *finished_key, uint8_t *verify_data)
{
    uint8_t th[48];
    transcript_hash(tls, th);

    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(tls->md_type);
    if (mbedtls_md_hmac(md_info, finished_key, tls->hash_len, th, tls->hash_len, verify_data) != 0)
        return -XQC_TLS_INTERNAL;
    return XQC_OK;
}

/* Derive handshake secrets from shared_secret */
static xqc_int_t
derive_handshake_secrets(xqc_tls_t *tls)
{
    xqc_int_t ret;
    xqc_digest_t md;
    md.md_type = tls->md_type;

    /* Early Secret = HKDF-Extract(salt=0, IKM=0...) */
    uint8_t zero[48] = {0};
    uint8_t zero_salt[48] = {0};
    ret = xqc_hkdf_extract(tls->early_secret, tls->hash_len, zero, tls->hash_len,
                            zero_salt, tls->hash_len, &md);
    if (ret != XQC_OK) return ret;

    /* derived_secret = Derive-Secret(early_secret, "derived", empty_hash) */
    uint8_t empty_hash[48];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(tls->md_type);
    if (mbedtls_md(md_info, NULL, 0, empty_hash) != 0)
        return -XQC_TLS_INTERNAL;

    uint8_t derived[48];
    ret = hkdf_expand_label(tls, derived, tls->hash_len, tls->early_secret, tls->hash_len,
        "derived", empty_hash, tls->hash_len);
    if (ret != XQC_OK) return ret;

    /* Handshake Secret = HKDF-Extract(salt=derived, IKM=shared_secret) */
    ret = xqc_hkdf_extract(tls->hs_secret, tls->hash_len, tls->shared_secret, 32,
                            derived, tls->hash_len, &md);
    if (ret != XQC_OK) return ret;

    /* Client/Server handshake traffic secrets */
    ret = derive_secret(tls, tls->c_hs_secret, tls->hs_secret, "c hs traffic");
    if (ret != XQC_OK) return ret;
    ret = derive_secret(tls, tls->s_hs_secret, tls->hs_secret, "s hs traffic");
    return ret;
}

/* Derive application secrets (call after server Finished is in transcript) */
static xqc_int_t
derive_application_secrets(xqc_tls_t *tls)
{
    xqc_int_t ret;
    xqc_digest_t md;
    md.md_type = tls->md_type;

    uint8_t empty_hash[48];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(tls->md_type);
    if (mbedtls_md(md_info, NULL, 0, empty_hash) != 0)
        return -XQC_TLS_INTERNAL;

    uint8_t derived2[48];
    ret = hkdf_expand_label(tls, derived2, tls->hash_len, tls->hs_secret, tls->hash_len,
        "derived", empty_hash, tls->hash_len);
    if (ret != XQC_OK) return ret;

    uint8_t zero[48] = {0};
    ret = xqc_hkdf_extract(tls->master_secret, tls->hash_len, zero, tls->hash_len,
                            derived2, tls->hash_len, &md);
    if (ret != XQC_OK) return ret;

    ret = derive_secret(tls, tls->c_ap_secret, tls->master_secret, "c ap traffic");
    if (ret != XQC_OK) return ret;
    ret = derive_secret(tls, tls->s_ap_secret, tls->master_secret, "s ap traffic");
    return ret;
}


/* ---- ECDH X25519 ---- */

static xqc_int_t
ecdh_generate_keypair(xqc_tls_t *tls)
{
    mbedtls_ecdh_init(&tls->ecdh);
    mbedtls_ctr_drbg_context *rng = xqc_tls_ctx_get_rng(tls->ctx);

    if (mbedtls_ecdh_setup(&tls->ecdh, MBEDTLS_ECP_DP_CURVE25519) != 0)
        return -XQC_TLS_INTERNAL;

    /* Generate keypair and export public key.
     * mbedtls_ecdh_make_public outputs 1-byte length prefix + 32-byte key for X25519. */
    uint8_t pub_buf[64];
    size_t pub_len = 0;
    if (mbedtls_ecdh_make_public(&tls->ecdh, &pub_len, pub_buf, sizeof(pub_buf),
                                  mbedtls_ctr_drbg_random, rng) != 0)
        return -XQC_TLS_INTERNAL;

    /* For Curve25519, mbedtls_ecdh_make_public writes: [0x20] [32 bytes of key] */
    if (pub_len == 33 && pub_buf[0] == 32) {
        memcpy(tls->my_x25519_pub, pub_buf + 1, 32);
    } else if (pub_len == 32) {
        memcpy(tls->my_x25519_pub, pub_buf, 32);
    } else {
        return -XQC_TLS_INTERNAL;
    }

    return XQC_OK;
}

static xqc_int_t
ecdh_compute_shared(xqc_tls_t *tls)
{
    mbedtls_ctr_drbg_context *rng = xqc_tls_ctx_get_rng(tls->ctx);

    /* Import peer's public key: prepend 1-byte length for mbedTLS */
    uint8_t peer_buf[33];
    peer_buf[0] = 32;
    memcpy(peer_buf + 1, tls->peer_x25519_pub, 32);
    if (mbedtls_ecdh_read_public(&tls->ecdh, peer_buf, 33) != 0)
        return -XQC_TLS_INTERNAL;

    size_t olen = 0;
    uint8_t shared[32];
    if (mbedtls_ecdh_calc_secret(&tls->ecdh, &olen, shared, 32,
                                  mbedtls_ctr_drbg_random, rng) != 0)
        return -XQC_TLS_INTERNAL;

    memcpy(tls->shared_secret, shared, 32);
    tls->ecdh_done = 1;
    return XQC_OK;
}


/* ---- Handshake message construction ---- */

/* Write a handshake message header (4 bytes: type + 3-byte length) */
static uint8_t *
hs_msg_start(uint8_t *p, uint8_t type)
{
    *p = type;
    return p + 4;  /* leave 3 bytes for length, fill in later */
}

static void
hs_msg_finish(uint8_t *start, uint8_t *end)
{
    uint32_t len = (uint32_t)(end - start - 4);
    put24(start + 1, len);
}


/* Build ClientHello */
static xqc_int_t
build_client_hello(xqc_tls_t *tls, uint8_t *buf, size_t cap, size_t *outlen)
{
    uint8_t *p = buf;
    uint8_t *msg_start = hs_msg_start(p, TLS_HS_CLIENT_HELLO);
    p = msg_start;

    /* Legacy version */
    put16(p, 0x0303); p += 2;

    /* Client random */
    mbedtls_ctr_drbg_context *rng = xqc_tls_ctx_get_rng(tls->ctx);
    mbedtls_ctr_drbg_random(rng, tls->client_random, 32);
    memcpy(p, tls->client_random, 32); p += 32;

    /* Session ID (empty for QUIC) */
    *p++ = 0;

    /* Cipher suites */
    put16(p, 4); p += 2; /* 2 suites × 2 bytes */
    put16(p, TLS_CS_AES_128_GCM_SHA256); p += 2;
    put16(p, TLS_CS_AES_256_GCM_SHA384); p += 2;

    /* Compression methods: null only */
    *p++ = 1; *p++ = 0;

    /* Extensions */
    uint8_t *ext_len_pos = p; p += 2;

    /* supported_versions */
    put16(p, TLS_EXT_SUPPORTED_VERSIONS); p += 2;
    put16(p, 3); p += 2;
    *p++ = 2;
    put16(p, TLS13_VERSION); p += 2;

    /* supported_groups */
    put16(p, TLS_EXT_SUPPORTED_GROUPS); p += 2;
    put16(p, 4); p += 2;
    put16(p, 2); p += 2;
    put16(p, TLS_GROUP_X25519); p += 2;

    /* key_share */
    put16(p, TLS_EXT_KEY_SHARE); p += 2;
    put16(p, 2 + 2 + 2 + 32); p += 2; /* ext data len */
    put16(p, 2 + 2 + 32); p += 2;     /* client_shares len */
    put16(p, TLS_GROUP_X25519); p += 2;
    put16(p, 32); p += 2;
    memcpy(p, tls->my_x25519_pub, 32); p += 32;

    /* signature_algorithms */
    put16(p, TLS_EXT_SIGNATURE_ALGORITHMS); p += 2;
    put16(p, 6); p += 2;
    put16(p, 4); p += 2;
    put16(p, TLS_SIG_RSA_PSS_RSAE_SHA256); p += 2;
    put16(p, TLS_SIG_ECDSA_SECP256R1_SHA256); p += 2;

    /* server_name (SNI) */
    if (tls->hostname[0]) {
        size_t hlen = strlen(tls->hostname);
        put16(p, TLS_EXT_SERVER_NAME); p += 2;
        put16(p, hlen + 5); p += 2;
        put16(p, hlen + 3); p += 2;
        *p++ = 0; /* host_name type */
        put16(p, hlen); p += 2;
        memcpy(p, tls->hostname, hlen); p += hlen;
    }

    /* ALPN */
    if (tls->alpn_len > 0) {
        put16(p, TLS_EXT_ALPN); p += 2;
        put16(p, 2 + 1 + tls->alpn_len); p += 2;
        put16(p, 1 + tls->alpn_len); p += 2;
        *p++ = (uint8_t)tls->alpn_len;
        memcpy(p, tls->alpn, tls->alpn_len); p += tls->alpn_len;
    }

    /* QUIC transport parameters */
    uint16_t tp_ext_type = (tls->version == XQC_VERSION_V1) ? TLS_EXT_QUIC_TP_V1 : TLS_EXT_QUIC_TP_DRAFT;
    if (tls->local_tp && tls->local_tp_len > 0) {
        put16(p, tp_ext_type); p += 2;
        put16(p, tls->local_tp_len); p += 2;
        memcpy(p, tls->local_tp, tls->local_tp_len); p += tls->local_tp_len;
    }

    /* Fill extensions length */
    put16(ext_len_pos, (uint16_t)(p - ext_len_pos - 2));

    hs_msg_finish(buf, p);
    *outlen = p - buf;
    return XQC_OK;
}


/* Build ServerHello */
static xqc_int_t
build_server_hello(xqc_tls_t *tls, uint8_t *buf, size_t cap, size_t *outlen)
{
    uint8_t *p = buf;
    uint8_t *msg_start = hs_msg_start(p, TLS_HS_SERVER_HELLO);
    p = msg_start;

    put16(p, 0x0303); p += 2;

    mbedtls_ctr_drbg_context *rng = xqc_tls_ctx_get_rng(tls->ctx);
    mbedtls_ctr_drbg_random(rng, tls->server_random, 32);
    memcpy(p, tls->server_random, 32); p += 32;

    /* Session ID: echo empty */
    *p++ = 0;

    /* Cipher suite */
    put16(p, tls->cipher_suite); p += 2;

    /* Compression: null */
    *p++ = 0;

    /* Extensions */
    uint8_t *ext_len_pos = p; p += 2;

    /* supported_versions */
    put16(p, TLS_EXT_SUPPORTED_VERSIONS); p += 2;
    put16(p, 2); p += 2;
    put16(p, TLS13_VERSION); p += 2;

    /* key_share */
    put16(p, TLS_EXT_KEY_SHARE); p += 2;
    put16(p, 2 + 2 + 32); p += 2;
    put16(p, TLS_GROUP_X25519); p += 2;
    put16(p, 32); p += 2;
    memcpy(p, tls->my_x25519_pub, 32); p += 32;

    put16(ext_len_pos, (uint16_t)(p - ext_len_pos - 2));
    hs_msg_finish(buf, p);
    *outlen = p - buf;
    return XQC_OK;
}


/* Build EncryptedExtensions */
static xqc_int_t
build_encrypted_extensions(xqc_tls_t *tls, uint8_t *buf, size_t cap, size_t *outlen)
{
    uint8_t *p = buf;
    uint8_t *msg_start = hs_msg_start(p, TLS_HS_ENCRYPTED_EXTENSIONS);
    p = msg_start;

    uint8_t *ext_len_pos = p; p += 2;

    /* ALPN */
    if (tls->selected_alpn_len > 0) {
        put16(p, TLS_EXT_ALPN); p += 2;
        put16(p, 2 + 1 + tls->selected_alpn_len); p += 2;
        put16(p, 1 + tls->selected_alpn_len); p += 2;
        *p++ = (uint8_t)tls->selected_alpn_len;
        memcpy(p, tls->selected_alpn, tls->selected_alpn_len); p += tls->selected_alpn_len;
    }

    /* QUIC transport parameters */
    uint16_t tp_ext_type = (tls->version == XQC_VERSION_V1) ? TLS_EXT_QUIC_TP_V1 : TLS_EXT_QUIC_TP_DRAFT;
    if (tls->local_tp && tls->local_tp_len > 0) {
        put16(p, tp_ext_type); p += 2;
        put16(p, tls->local_tp_len); p += 2;
        memcpy(p, tls->local_tp, tls->local_tp_len); p += tls->local_tp_len;
    }

    put16(ext_len_pos, (uint16_t)(p - ext_len_pos - 2));
    hs_msg_finish(buf, p);
    *outlen = p - buf;
    return XQC_OK;
}


/* Build Certificate message */
static xqc_int_t
build_certificate(xqc_tls_t *tls, uint8_t *buf, size_t cap, size_t *outlen)
{
    uint8_t *p = buf;
    uint8_t *msg_start = hs_msg_start(p, TLS_HS_CERTIFICATE);
    p = msg_start;

    /* Certificate request context (empty for server) */
    *p++ = 0;

    /* Certificate list */
    uint8_t *list_len_pos = p; p += 3;

    mbedtls_x509_crt *crt = xqc_tls_ctx_get_cert(tls->ctx);
    while (crt && crt->raw.len > 0) {
        put24(p, (uint32_t)crt->raw.len); p += 3;
        memcpy(p, crt->raw.p, crt->raw.len); p += crt->raw.len;
        /* Extensions per cert entry (empty) */
        put16(p, 0); p += 2;
        crt = crt->next;
    }

    put24(list_len_pos, (uint32_t)(p - list_len_pos - 3));
    hs_msg_finish(buf, p);
    *outlen = p - buf;
    return XQC_OK;
}


/* Build CertificateVerify */
static xqc_int_t
build_certificate_verify(xqc_tls_t *tls, uint8_t *buf, size_t cap, size_t *outlen)
{
    /* Compute the signature input */
    uint8_t th[48];
    transcript_hash(tls, th);

    /* Construct content to sign:
     * 64 spaces + "TLS 1.3, server CertificateVerify" + 0x00 + transcript_hash */
    uint8_t content[200];
    memset(content, 0x20, 64);
    const char *label = "TLS 1.3, server CertificateVerify";
    size_t label_len = strlen(label);
    memcpy(content + 64, label, label_len);
    content[64 + label_len] = 0;
    memcpy(content + 64 + label_len + 1, th, tls->hash_len);
    size_t content_len = 64 + label_len + 1 + tls->hash_len;

    /* Hash the content for signing */
    uint8_t hash[48];
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    mbedtls_md(md_info, content, content_len, hash);

    /* Sign */
    mbedtls_pk_context *pk = xqc_tls_ctx_get_pkey(tls->ctx);
    mbedtls_ctr_drbg_context *rng = xqc_tls_ctx_get_rng(tls->ctx);
    uint8_t sig[512];
    size_t sig_len = 0;

    /* Determine signature algorithm */
    uint16_t sig_alg;
    mbedtls_md_type_t sig_md = MBEDTLS_MD_SHA256;
    if (mbedtls_pk_get_type(pk) == MBEDTLS_PK_RSA) {
        sig_alg = TLS_SIG_RSA_PSS_RSAE_SHA256;
        mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, sig_md);
        if (mbedtls_rsa_rsassa_pss_sign(rsa, mbedtls_ctr_drbg_random, rng,
                sig_md, mbedtls_md_get_size(md_info), hash, sig) != 0)
            return -XQC_TLS_INTERNAL;
        sig_len = mbedtls_pk_get_len(pk);
    } else {
        sig_alg = TLS_SIG_ECDSA_SECP256R1_SHA256;
        if (mbedtls_pk_sign(pk, sig_md, hash, mbedtls_md_get_size(md_info),
                sig, sizeof(sig), &sig_len, mbedtls_ctr_drbg_random, rng) != 0)
            return -XQC_TLS_INTERNAL;
    }

    uint8_t *p = buf;
    uint8_t *msg_start = hs_msg_start(p, TLS_HS_CERTIFICATE_VERIFY);
    p = msg_start;

    put16(p, sig_alg); p += 2;
    put16(p, sig_len); p += 2;
    memcpy(p, sig, sig_len); p += sig_len;

    hs_msg_finish(buf, p);
    *outlen = p - buf;
    return XQC_OK;
}


/* Build Finished */
static xqc_int_t
build_finished(xqc_tls_t *tls, const uint8_t *base_secret, uint8_t *buf, size_t cap, size_t *outlen)
{
    uint8_t finished_key[48];
    xqc_int_t ret = compute_finished_key(tls, base_secret, finished_key);
    if (ret != XQC_OK) return ret;

    uint8_t verify_data[48];
    ret = compute_finished_verify(tls, finished_key, verify_data);
    if (ret != XQC_OK) return ret;

    uint8_t *p = buf;
    uint8_t *msg_start = hs_msg_start(p, TLS_HS_FINISHED);
    p = msg_start;
    memcpy(p, verify_data, tls->hash_len); p += tls->hash_len;
    hs_msg_finish(buf, p);
    *outlen = p - buf;
    return XQC_OK;
}


/* ---- Handshake message parsing ---- */

static xqc_int_t
parse_server_hello(xqc_tls_t *tls, const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    if (len < 38) return -XQC_TLS_INTERNAL;

    /* Skip legacy version */
    p += 2;

    /* Server random */
    memcpy(tls->server_random, p, 32); p += 32;

    /* Session ID */
    if (p >= end) return -XQC_TLS_INTERNAL;
    uint8_t sid_len = *p++;
    p += sid_len;

    /* Cipher suite */
    if (p + 2 > end) return -XQC_TLS_INTERNAL;
    tls->cipher_suite = get16(p); p += 2;

    /* Compression */
    p += 1;

    /* Set crypto parameters based on cipher suite */
    if (tls->cipher_suite == TLS_CS_AES_128_GCM_SHA256) {
        tls->cipher_id = XQC_TLS13_AES_128_GCM_SHA256;
        tls->md_type = MBEDTLS_MD_SHA256;
        tls->hash_len = 32;
    } else if (tls->cipher_suite == TLS_CS_AES_256_GCM_SHA384) {
        tls->cipher_id = XQC_TLS13_AES_256_GCM_SHA384;
        tls->md_type = MBEDTLS_MD_SHA384;
        tls->hash_len = 48;
    } else {
        return -XQC_TLS_INTERNAL;
    }

    /* Parse extensions */
    if (p + 2 > end) return XQC_OK;
    uint16_t ext_total = get16(p); p += 2;
    const uint8_t *ext_end = p + ext_total;

    while (p + 4 <= ext_end) {
        uint16_t ext_type = get16(p); p += 2;
        uint16_t ext_len  = get16(p); p += 2;
        const uint8_t *ext_data = p;
        p += ext_len;
        if (p > ext_end) break;

        if (ext_type == TLS_EXT_KEY_SHARE) {
            if (ext_len < 36) continue;
            uint16_t group = get16(ext_data);
            uint16_t klen  = get16(ext_data + 2);
            if (group == TLS_GROUP_X25519 && klen == 32) {
                memcpy(tls->peer_x25519_pub, ext_data + 4, 32);
            }
        }
    }

    return XQC_OK;
}


static xqc_int_t
parse_client_hello(xqc_tls_t *tls, const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    if (len < 38) return -XQC_TLS_INTERNAL;

    p += 2; /* legacy version */
    memcpy(tls->client_random, p, 32); p += 32;

    /* Session ID */
    uint8_t sid_len = *p++;
    p += sid_len;

    /* Cipher suites */
    if (p + 2 > end) return -XQC_TLS_INTERNAL;
    uint16_t cs_len = get16(p); p += 2;
    tls->cipher_suite = 0;
    for (int i = 0; i < cs_len / 2; i++) {
        uint16_t cs = get16(p + i * 2);
        if (cs == TLS_CS_AES_128_GCM_SHA256 || cs == TLS_CS_AES_256_GCM_SHA384) {
            if (tls->cipher_suite == 0) tls->cipher_suite = cs;
        }
    }
    p += cs_len;

    if (!tls->cipher_suite) {
        tls->cipher_suite = TLS_CS_AES_128_GCM_SHA256;
    }

    if (tls->cipher_suite == TLS_CS_AES_128_GCM_SHA256) {
        tls->cipher_id = XQC_TLS13_AES_128_GCM_SHA256;
        tls->md_type = MBEDTLS_MD_SHA256;
        tls->hash_len = 32;
    } else {
        tls->cipher_id = XQC_TLS13_AES_256_GCM_SHA384;
        tls->md_type = MBEDTLS_MD_SHA384;
        tls->hash_len = 48;
    }

    /* Compression */
    if (p >= end) return -XQC_TLS_INTERNAL;
    uint8_t comp_len = *p++;
    p += comp_len;

    /* Extensions */
    if (p + 2 > end) return XQC_OK;
    uint16_t ext_total = get16(p); p += 2;
    const uint8_t *ext_end = p + ext_total;

    while (p + 4 <= ext_end) {
        uint16_t ext_type = get16(p); p += 2;
        uint16_t ext_len  = get16(p); p += 2;
        const uint8_t *ext_data = p;
        p += ext_len;
        if (p > ext_end) break;

        if (ext_type == TLS_EXT_KEY_SHARE) {
            /* Client key_share list */
            if (ext_len < 2) continue;
            uint16_t list_len = get16(ext_data);
            const uint8_t *kp = ext_data + 2;
            const uint8_t *kend = ext_data + 2 + list_len;
            while (kp + 4 <= kend) {
                uint16_t group = get16(kp); kp += 2;
                uint16_t klen  = get16(kp); kp += 2;
                if (group == TLS_GROUP_X25519 && klen == 32 && kp + 32 <= kend) {
                    memcpy(tls->peer_x25519_pub, kp, 32);
                }
                kp += klen;
            }

        } else if (ext_type == TLS_EXT_ALPN) {
            if (ext_len < 2) continue;
            uint16_t alpn_list_len = get16(ext_data);
            const uint8_t *ap = ext_data + 2;
            const uint8_t *aend = ext_data + 2 + alpn_list_len;

            /* Select matching ALPN from registered list */
            unsigned char *reg_alpn = NULL;
            size_t reg_len = 0;
            xqc_tls_ctx_get_alpn_list(tls->ctx, &reg_alpn, &reg_len);

            while (ap < aend) {
                uint8_t alen = *ap++;
                if (ap + alen > aend) break;

                /* Try to match against registered ALPNs */
                const unsigned char *rp = reg_alpn;
                const unsigned char *rend = reg_alpn + reg_len;
                while (rp < rend) {
                    uint8_t rlen = *rp++;
                    if (rlen == alen && memcmp(rp, ap, alen) == 0) {
                        tls->selected_alpn_len = alen;
                        memcpy(tls->selected_alpn, ap, alen);
                        tls->selected_alpn[alen] = '\0';
                        goto alpn_done;
                    }
                    rp += rlen;
                }
                ap += alen;
            }
            alpn_done:;

        } else if (ext_type == TLS_EXT_SERVER_NAME) {
            if (ext_len < 5) continue;
            uint16_t sni_list_len = get16(ext_data);
            (void)sni_list_len;
            uint8_t name_type = ext_data[2];
            if (name_type == 0) {
                uint16_t name_len = get16(ext_data + 3);
                if (name_len < sizeof(tls->hostname)) {
                    memcpy(tls->hostname, ext_data + 5, name_len);
                    tls->hostname[name_len] = '\0';
                }
            }

        } else if (ext_type == TLS_EXT_QUIC_TP_V1 || ext_type == TLS_EXT_QUIC_TP_DRAFT) {
            if (tls->cbs->tp_cb && !tls->tp_received) {
                tls->cbs->tp_cb(ext_data, ext_len, tls->user_data);
                tls->tp_received = 1;
            }
        }
    }

    return XQC_OK;
}


static xqc_int_t
parse_encrypted_extensions(xqc_tls_t *tls, const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;

    if (len < 2) return XQC_OK;
    uint16_t ext_total = get16(p); p += 2;
    const uint8_t *ext_end = p + ext_total;
    if (ext_end > end) ext_end = end;

    while (p + 4 <= ext_end) {
        uint16_t ext_type = get16(p); p += 2;
        uint16_t ext_len  = get16(p); p += 2;
        const uint8_t *ext_data = p;
        p += ext_len;

        if (ext_type == TLS_EXT_ALPN) {
            if (ext_len < 4) continue;
            uint16_t list_len = get16(ext_data);
            (void)list_len;
            uint8_t alen = ext_data[2];
            if (alen + 3 <= ext_len) {
                tls->selected_alpn_len = alen;
                memcpy(tls->selected_alpn, ext_data + 3, alen);
                tls->selected_alpn[alen] = '\0';

                if (tls->cbs->alpn_select_cb) {
                    tls->cbs->alpn_select_cb(tls->selected_alpn, tls->selected_alpn_len, tls->user_data);
                }
            }

        } else if (ext_type == TLS_EXT_QUIC_TP_V1 || ext_type == TLS_EXT_QUIC_TP_DRAFT) {
            if (tls->cbs->tp_cb && !tls->tp_received) {
                tls->cbs->tp_cb(ext_data, ext_len, tls->user_data);
                tls->tp_received = 1;
            }
        }
    }

    return XQC_OK;
}


static xqc_int_t
parse_certificate(xqc_tls_t *tls, const uint8_t *data, size_t len)
{
    const uint8_t *p = data;
    if (len < 4) return -XQC_TLS_INTERNAL;

    uint8_t ctx_len = *p++;
    p += ctx_len;

    uint32_t list_len = get24(p); p += 3;
    (void)list_len;

    /* Parse first cert (skip chain validation for self-signed) */
    if (p + 3 > data + len) return -XQC_TLS_INTERNAL;
    uint32_t cert_len = get24(p); p += 3;
    if (p + cert_len > data + len) return -XQC_TLS_INTERNAL;

    mbedtls_x509_crt_init(&tls->peer_cert);
    tls->peer_cert_inited = 1;
    if (mbedtls_x509_crt_parse_der(&tls->peer_cert, p, cert_len) != 0) {
        xqc_log(tls->log, XQC_LOG_WARN, "|parse peer cert failed|");
    }
    p += cert_len;

    /* Skip cert extensions */
    if (p + 2 <= data + len) {
        uint16_t ce_len = get16(p);
        p += 2 + ce_len;
    }

    return XQC_OK;
}


static xqc_int_t
parse_certificate_verify(xqc_tls_t *tls, const uint8_t *data, size_t len)
{
    if (len < 4) return -XQC_TLS_INTERNAL;

    uint16_t sig_alg = get16(data);
    uint16_t sig_len = get16(data + 2);
    const uint8_t *sig = data + 4;

    if (4 + sig_len > len) return -XQC_TLS_INTERNAL;

    /* Build the signed content */
    uint8_t th[48];
    transcript_hash(tls, th);

    uint8_t content[200];
    memset(content, 0x20, 64);
    const char *label = "TLS 1.3, server CertificateVerify";
    size_t label_len = strlen(label);
    memcpy(content + 64, label, label_len);
    content[64 + label_len] = 0;
    memcpy(content + 64 + label_len + 1, th, tls->hash_len);
    size_t content_len = 64 + label_len + 1 + tls->hash_len;

    uint8_t hash[48];
    mbedtls_md_type_t md_t = MBEDTLS_MD_SHA256;
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md_t);
    size_t hash_size = mbedtls_md_get_size(md_info);
    mbedtls_md(md_info, content, content_len, hash);

    /* Verify signature using peer certificate */
    if (!tls->peer_cert_inited) {
        if (tls->cert_verify_flag & XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED) {
            return XQC_OK;
        }
        xqc_log(tls->log, XQC_LOG_WARN, "|no peer cert for verify|");
        return XQC_OK;
    }

    int ret;
    (void)sig_alg;
    ret = mbedtls_pk_verify(&tls->peer_cert.pk, md_t, hash, hash_size, sig, sig_len);
    if (ret != 0) {
        char errbuf[128];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        xqc_log(tls->log, XQC_LOG_WARN, "|cert verify sig failed|%s|", errbuf);
        if (!(tls->cert_verify_flag & XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED)) {
            return -XQC_TLS_INTERNAL;
        }
    }

    return XQC_OK;
}


static xqc_int_t
verify_finished(xqc_tls_t *tls, const uint8_t *data, size_t len,
    const uint8_t *base_secret)
{
    if ((int)len != tls->hash_len) return -XQC_TLS_INTERNAL;

    uint8_t finished_key[48];
    xqc_int_t ret = compute_finished_key(tls, base_secret, finished_key);
    if (ret != XQC_OK) return ret;

    uint8_t expected[48];
    ret = compute_finished_verify(tls, finished_key, expected);
    if (ret != XQC_OK) return ret;

    if (memcmp(data, expected, tls->hash_len) != 0) {
        xqc_log(tls->log, XQC_LOG_ERROR, "|finished verify mismatch|");
        return -XQC_TLS_INTERNAL;
    }

    return XQC_OK;
}


/* ---- Install keys into xquic crypto layer ---- */

static xqc_int_t
install_keys(xqc_tls_t *tls, xqc_encrypt_level_t level, uint32_t cid,
    const uint8_t *read_secret, const uint8_t *write_secret, size_t secret_len)
{
    if (!tls->crypto[level]) {
        tls->crypto[level] = xqc_crypto_create(cid, tls->log);
        if (!tls->crypto[level]) return -XQC_TLS_NOMEM;
    }

    xqc_int_t ret;
    if (read_secret) {
        ret = xqc_crypto_derive_keys(tls->crypto[level], read_secret, secret_len, XQC_KEY_TYPE_RX_READ);
        if (ret != XQC_OK) return ret;

        if (level == XQC_ENC_LEV_1RTT) {
            xqc_crypto_save_application_traffic_secret_0(tls->crypto[level], read_secret, secret_len, XQC_KEY_TYPE_RX_READ);
        }
    }

    if (write_secret) {
        ret = xqc_crypto_derive_keys(tls->crypto[level], write_secret, secret_len, XQC_KEY_TYPE_TX_WRITE);
        if (ret != XQC_OK) return ret;

        if (level == XQC_ENC_LEV_1RTT) {
            xqc_crypto_save_application_traffic_secret_0(tls->crypto[level], write_secret, secret_len, XQC_KEY_TYPE_TX_WRITE);
        }
    }

    return XQC_OK;
}


/* ---- Main handshake processing ---- */

static xqc_int_t
process_hs_msg(xqc_tls_t *tls, xqc_encrypt_level_t level,
    uint8_t msg_type, const uint8_t *msg_data, size_t msg_len);


static xqc_int_t
client_process_server_hello(xqc_tls_t *tls, const uint8_t *msg, size_t msg_len,
    const uint8_t *raw_msg, size_t raw_msg_len)
{
    xqc_int_t ret;

    ret = parse_server_hello(tls, msg, msg_len);
    if (ret != XQC_OK) return ret;

    /* Initialize transcript with the cipher suite's hash */
    ret = transcript_init(tls, tls->md_type);
    if (ret != XQC_OK) return ret;

    /* Add saved ClientHello to transcript before ServerHello */
    if (tls->saved_ch_len > 0) {
        transcript_update(tls, tls->saved_ch, tls->saved_ch_len);
        tls->saved_ch_len = 0;
    }

    /* Update transcript with ServerHello */
    transcript_update(tls, raw_msg, raw_msg_len);

    /* Compute shared secret via ECDH */
    ret = ecdh_compute_shared(tls);
    if (ret != XQC_OK) return ret;

    /* Derive handshake secrets */
    ret = derive_handshake_secrets(tls);
    if (ret != XQC_OK) return ret;

    /* Install handshake keys */
    ret = install_keys(tls, XQC_ENC_LEV_HSK, tls->cipher_id,
        tls->s_hs_secret, tls->c_hs_secret, tls->hash_len);
    if (ret != XQC_OK) return ret;

    tls->state = MTLS_CLI_WAIT_EE;
    return XQC_OK;
}


static xqc_int_t
server_process_client_hello(xqc_tls_t *tls, const uint8_t *msg, size_t msg_len,
    const uint8_t *raw_msg, size_t raw_msg_len)
{
    xqc_int_t ret;

    ret = parse_client_hello(tls, msg, msg_len);
    if (ret != XQC_OK) return ret;

    /* Initialize transcript */
    ret = transcript_init(tls, tls->md_type);
    if (ret != XQC_OK) return ret;

    /* Add ClientHello to transcript */
    transcript_update(tls, raw_msg, raw_msg_len);

    /* Generate ECDH keypair and compute shared secret */
    ret = ecdh_generate_keypair(tls);
    if (ret != XQC_OK) return ret;
    ret = ecdh_compute_shared(tls);
    if (ret != XQC_OK) return ret;

    /* ALPN selection notification */
    if (tls->selected_alpn_len > 0 && tls->cbs->alpn_select_cb) {
        tls->cbs->alpn_select_cb(tls->selected_alpn, tls->selected_alpn_len, tls->user_data);
    }

    /* Build ServerHello */
    uint8_t sh_buf[512];
    size_t sh_len = 0;
    ret = build_server_hello(tls, sh_buf, sizeof(sh_buf), &sh_len);
    if (ret != XQC_OK) return ret;

    /* Add ServerHello to transcript */
    transcript_update(tls, sh_buf, sh_len);

    /* Derive handshake secrets */
    ret = derive_handshake_secrets(tls);
    if (ret != XQC_OK) return ret;

    /* Install handshake keys */
    ret = install_keys(tls, XQC_ENC_LEV_HSK, tls->cipher_id,
        tls->c_hs_secret, tls->s_hs_secret, tls->hash_len);
    if (ret != XQC_OK) return ret;

    /* Send ServerHello at INITIAL level */
    if (tls->cbs->crypto_data_cb) {
        ret = tls->cbs->crypto_data_cb(XQC_ENC_LEV_INIT, sh_buf, sh_len, tls->user_data);
        if (ret != XQC_OK) return ret;
    }

    /* Build and send EncryptedExtensions, Certificate, CertificateVerify, Finished at HANDSHAKE level */
    uint8_t hs_buf[8192];
    size_t total_len = 0;

    /* EncryptedExtensions */
    size_t ee_len = 0;
    ret = build_encrypted_extensions(tls, hs_buf + total_len, sizeof(hs_buf) - total_len, &ee_len);
    if (ret != XQC_OK) return ret;
    transcript_update(tls, hs_buf + total_len, ee_len);
    total_len += ee_len;

    /* Certificate */
    if (xqc_tls_ctx_is_cert_loaded(tls->ctx)) {
        size_t cert_len = 0;
        ret = build_certificate(tls, hs_buf + total_len, sizeof(hs_buf) - total_len, &cert_len);
        if (ret != XQC_OK) return ret;
        transcript_update(tls, hs_buf + total_len, cert_len);
        total_len += cert_len;

        /* CertificateVerify */
        size_t cv_len = 0;
        ret = build_certificate_verify(tls, hs_buf + total_len, sizeof(hs_buf) - total_len, &cv_len);
        if (ret != XQC_OK) return ret;
        transcript_update(tls, hs_buf + total_len, cv_len);
        total_len += cv_len;
    }

    /* Finished */
    size_t fin_len = 0;
    ret = build_finished(tls, tls->s_hs_secret, hs_buf + total_len, sizeof(hs_buf) - total_len, &fin_len);
    if (ret != XQC_OK) return ret;
    transcript_update(tls, hs_buf + total_len, fin_len);
    total_len += fin_len;

    /* Send all at HANDSHAKE level */
    if (tls->cbs->crypto_data_cb) {
        ret = tls->cbs->crypto_data_cb(XQC_ENC_LEV_HSK, hs_buf, total_len, tls->user_data);
        if (ret != XQC_OK) return ret;
    }

    /* Derive application secrets now (server Finished is in transcript) */
    ret = derive_application_secrets(tls);
    if (ret != XQC_OK) return ret;

    /* Install 1-RTT write key (server can start sending app data) */
    ret = install_keys(tls, XQC_ENC_LEV_1RTT, tls->cipher_id,
        NULL, tls->s_ap_secret, tls->hash_len);
    if (ret != XQC_OK) return ret;

    tls->state = MTLS_SVR_WAIT_FIN;
    return XQC_OK;
}


static xqc_int_t
process_hs_msg(xqc_tls_t *tls, xqc_encrypt_level_t level,
    uint8_t msg_type, const uint8_t *msg_data, size_t msg_len)
{
    xqc_int_t ret;
    /* raw_msg includes the 4-byte header */
    const uint8_t *raw_msg = msg_data - 4;
    size_t raw_msg_len = msg_len + 4;

    switch (tls->state) {
    case MTLS_CLI_WAIT_SH:
        if (msg_type != TLS_HS_SERVER_HELLO) {
            xqc_log(tls->log, XQC_LOG_ERROR, "|expected ServerHello, got %d|", msg_type);
            return -XQC_TLS_INTERNAL;
        }
        return client_process_server_hello(tls, msg_data, msg_len, raw_msg, raw_msg_len);

    case MTLS_CLI_WAIT_EE:
        if (msg_type != TLS_HS_ENCRYPTED_EXTENSIONS) {
            xqc_log(tls->log, XQC_LOG_ERROR, "|expected EE, got %d|", msg_type);
            return -XQC_TLS_INTERNAL;
        }
        ret = parse_encrypted_extensions(tls, msg_data, msg_len);
        if (ret != XQC_OK) return ret;
        transcript_update(tls, raw_msg, raw_msg_len);
        tls->state = MTLS_CLI_WAIT_CERT_OR_FIN;
        return XQC_OK;

    case MTLS_CLI_WAIT_CERT_OR_FIN:
        if (msg_type == TLS_HS_CERTIFICATE) {
            ret = parse_certificate(tls, msg_data, msg_len);
            if (ret != XQC_OK) return ret;
            transcript_update(tls, raw_msg, raw_msg_len);
            tls->state = MTLS_CLI_WAIT_CV;
            return XQC_OK;
        } else if (msg_type == TLS_HS_FINISHED) {
            goto handle_server_finished;
        }
        return -XQC_TLS_INTERNAL;

    case MTLS_CLI_WAIT_CV:
        if (msg_type != TLS_HS_CERTIFICATE_VERIFY) {
            return -XQC_TLS_INTERNAL;
        }
        ret = parse_certificate_verify(tls, msg_data, msg_len);
        if (ret != XQC_OK) return ret;
        transcript_update(tls, raw_msg, raw_msg_len);
        tls->state = MTLS_CLI_WAIT_FIN;
        return XQC_OK;

    case MTLS_CLI_WAIT_FIN:
        if (msg_type != TLS_HS_FINISHED) {
            return -XQC_TLS_INTERNAL;
        }
    handle_server_finished:
        /* Verify server Finished */
        ret = verify_finished(tls, msg_data, msg_len, tls->s_hs_secret);
        if (ret != XQC_OK) return ret;

        /* Add server Finished to transcript */
        transcript_update(tls, raw_msg, raw_msg_len);

        /* Derive application secrets */
        ret = derive_application_secrets(tls);
        if (ret != XQC_OK) return ret;

        /* Build and send client Finished at HANDSHAKE level */
        {
            uint8_t fin_buf[256];
            size_t fin_len = 0;
            ret = build_finished(tls, tls->c_hs_secret, fin_buf, sizeof(fin_buf), &fin_len);
            if (ret != XQC_OK) return ret;

            if (tls->cbs->crypto_data_cb) {
                ret = tls->cbs->crypto_data_cb(XQC_ENC_LEV_HSK, fin_buf, fin_len, tls->user_data);
                if (ret != XQC_OK) return ret;
            }
        }

        /* Install 1-RTT keys */
        ret = install_keys(tls, XQC_ENC_LEV_1RTT, tls->cipher_id,
            tls->s_ap_secret, tls->c_ap_secret, tls->hash_len);
        if (ret != XQC_OK) return ret;

        tls->state = MTLS_HANDSHAKE_DONE;
        tls->hsk_completed = 1;

        if (tls->cbs->hsk_completed_cb) {
            tls->cbs->hsk_completed_cb(tls->user_data);
        }
        return XQC_OK;

    case MTLS_SVR_WAIT_CH:
        if (msg_type != TLS_HS_CLIENT_HELLO) {
            return -XQC_TLS_INTERNAL;
        }
        return server_process_client_hello(tls, msg_data, msg_len, raw_msg, raw_msg_len);

    case MTLS_SVR_WAIT_FIN:
        if (msg_type != TLS_HS_FINISHED) {
            return -XQC_TLS_INTERNAL;
        }
        /* Verify client Finished */
        ret = verify_finished(tls, msg_data, msg_len, tls->c_hs_secret);
        if (ret != XQC_OK) return ret;

        /* Install 1-RTT read key */
        ret = install_keys(tls, XQC_ENC_LEV_1RTT, tls->cipher_id,
            tls->c_ap_secret, NULL, tls->hash_len);
        if (ret != XQC_OK) return ret;

        tls->state = MTLS_HANDSHAKE_DONE;
        tls->hsk_completed = 1;

        if (tls->cbs->hsk_completed_cb) {
            tls->cbs->hsk_completed_cb(tls->user_data);
        }
        return XQC_OK;

    default:
        xqc_log(tls->log, XQC_LOG_WARN, "|unexpected hs msg in state %d|type:%d|", tls->state, msg_type);
        return XQC_OK;
    }
}


/* ---- Public interface implementation ---- */

xqc_tls_t *
xqc_tls_create(xqc_tls_ctx_t *ctx, xqc_tls_config_t *cfg, xqc_log_t *log, void *user_data)
{
    xqc_tls_t *tls = xqc_calloc(1, sizeof(xqc_tls_t));
    if (!tls) return NULL;

    xqc_tls_ctx_get_tls_callbacks(ctx, &tls->cbs);
    tls->type = xqc_tls_ctx_get_type(ctx);
    tls->ctx = ctx;
    tls->log = log;
    tls->user_data = user_data;
    tls->cert_verify_flag = cfg->cert_verify_flag;
    tls->no_crypto = cfg->no_crypto_flag;
    tls->key_update_confirmed = XQC_TRUE;
    tls->state = MTLS_STATE_INIT;

    /* ALPN */
    if (cfg->alpn) {
        tls->alpn_len = strlen(cfg->alpn);
        if (tls->alpn_len >= sizeof(tls->alpn)) tls->alpn_len = sizeof(tls->alpn) - 1;
        memcpy(tls->alpn, cfg->alpn, tls->alpn_len);
    }

    /* Hostname */
    if (cfg->hostname && strlen(cfg->hostname) > 0) {
        strncpy(tls->hostname, cfg->hostname, sizeof(tls->hostname) - 1);
    } else {
        strncpy(tls->hostname, "localhost", sizeof(tls->hostname) - 1);
    }

    /* Transport parameters */
    if (cfg->trans_params && cfg->trans_params_len > 0) {
        tls->local_tp = xqc_malloc(cfg->trans_params_len);
        if (tls->local_tp) {
            memcpy(tls->local_tp, cfg->trans_params, cfg->trans_params_len);
            tls->local_tp_len = cfg->trans_params_len;
        }
    }

    return tls;
}


xqc_int_t
xqc_tls_init(xqc_tls_t *tls, xqc_proto_version_t version, const xqc_cid_t *odcid)
{
    tls->version = version;

    /* Create initial crypto */
    tls->crypto[XQC_ENC_LEV_INIT] = xqc_crypto_create(XQC_TLS13_AES_128_GCM_SHA256, tls->log);
    if (!tls->crypto[XQC_ENC_LEV_INIT]) {
        xqc_log(tls->log, XQC_LOG_ERROR, "|create init crypto error|");
        return -XQC_TLS_NOMEM;
    }

    /* Derive initial keys */
    uint8_t cli_init[INITIAL_SECRET_MAX_LEN] = {0};
    uint8_t svr_init[INITIAL_SECRET_MAX_LEN] = {0};
    xqc_int_t ret = xqc_crypto_derive_initial_secret(
        cli_init, INITIAL_SECRET_MAX_LEN,
        svr_init, INITIAL_SECRET_MAX_LEN,
        odcid, xqc_crypto_initial_salt[version], strlen(xqc_crypto_initial_salt[version]));
    if (ret != XQC_OK) return ret;

    if (tls->type == XQC_TLS_TYPE_CLIENT) {
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], cli_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_TX_WRITE);
        if (ret != XQC_OK) return ret;
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], svr_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_RX_READ);
        if (ret != XQC_OK) return ret;

        /* Generate ECDH keypair */
        ret = ecdh_generate_keypair(tls);
        if (ret != XQC_OK) return ret;

        /* Build ClientHello */
        uint8_t ch_buf[2048];
        size_t ch_len = 0;
        ret = build_client_hello(tls, ch_buf, sizeof(ch_buf), &ch_len);
        if (ret != XQC_OK) return ret;

        /* Save CH bytes for transcript (we'll add them when we know the hash) */
        memcpy(tls->saved_ch, ch_buf, ch_len);
        tls->saved_ch_len = ch_len;

        /* Send ClientHello */
        if (tls->cbs->crypto_data_cb) {
            ret = tls->cbs->crypto_data_cb(XQC_ENC_LEV_INIT, ch_buf, ch_len, tls->user_data);
            if (ret != XQC_OK) return ret;
        }

        tls->state = MTLS_CLI_WAIT_SH;
    } else {
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], svr_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_TX_WRITE);
        if (ret != XQC_OK) return ret;
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], cli_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_RX_READ);
        if (ret != XQC_OK) return ret;

        tls->state = MTLS_SVR_WAIT_CH;
    }

    return XQC_OK;
}


xqc_int_t
xqc_tls_reset_initial(xqc_tls_t *tls, xqc_proto_version_t version, const xqc_cid_t *odcid)
{
    tls->version = version;

    if (!tls->crypto[XQC_ENC_LEV_INIT]) {
        return -XQC_TLS_INVALID_STATE;
    }

    uint8_t cli_init[INITIAL_SECRET_MAX_LEN] = {0};
    uint8_t svr_init[INITIAL_SECRET_MAX_LEN] = {0};
    xqc_int_t ret = xqc_crypto_derive_initial_secret(
        cli_init, INITIAL_SECRET_MAX_LEN,
        svr_init, INITIAL_SECRET_MAX_LEN,
        odcid, xqc_crypto_initial_salt[version], strlen(xqc_crypto_initial_salt[version]));
    if (ret != XQC_OK) return ret;

    if (tls->type == XQC_TLS_TYPE_CLIENT) {
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], cli_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_TX_WRITE);
        if (ret != XQC_OK) return ret;
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], svr_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_RX_READ);
    } else {
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], svr_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_TX_WRITE);
        if (ret != XQC_OK) return ret;
        ret = xqc_crypto_derive_keys(tls->crypto[XQC_ENC_LEV_INIT], cli_init, INITIAL_SECRET_MAX_LEN, XQC_KEY_TYPE_RX_READ);
    }
    return ret;
}


void
xqc_tls_destroy(xqc_tls_t *tls)
{
    if (!tls) return;

    for (int lv = 0; lv < XQC_ENC_LEV_MAX; lv++) {
        xqc_crypto_destroy(tls->crypto[lv]);
    }

    if (tls->transcript_inited) {
        mbedtls_md_free(&tls->transcript);
    }

    mbedtls_ecdh_free(&tls->ecdh);

    if (tls->peer_cert_inited) {
        mbedtls_x509_crt_free(&tls->peer_cert);
    }

    if (tls->local_tp) {
        xqc_free(tls->local_tp);
    }

    xqc_free(tls);
}


xqc_int_t
xqc_tls_process_crypto_data(xqc_tls_t *tls, xqc_encrypt_level_t level,
    const uint8_t *crypto_data, size_t data_len)
{
    /* Buffer incoming bytes */
    if (tls->reasm_len[level] + data_len > REASM_BUF_SIZE) {
        xqc_log(tls->log, XQC_LOG_ERROR, "|reasm overflow|level:%d|", level);
        return -XQC_TLS_INTERNAL;
    }

    memcpy(tls->reasm[level] + tls->reasm_len[level], crypto_data, data_len);
    tls->reasm_len[level] += data_len;

    /* Process complete handshake messages */
    while (tls->reasm_len[level] >= 4) {
        uint8_t msg_type = tls->reasm[level][0];
        uint32_t msg_len = get24(tls->reasm[level] + 1);
        size_t total = 4 + msg_len;

        if (tls->reasm_len[level] < total) break;

        /* Set flag BEFORE processing so xqc_crypto_stream_on_write can flush the
         * ServerHello that server_process_client_hello will generate. */
        if (msg_type == TLS_HS_CLIENT_HELLO && tls->user_data) {
            xqc_connection_t *conn = (xqc_connection_t *)tls->user_data;
            conn->conn_flag |= XQC_CONN_FLAG_TLS_CH_RECVD;
        }

        xqc_int_t ret = process_hs_msg(tls, level, msg_type, tls->reasm[level] + 4, msg_len);
        if (ret != XQC_OK) return ret;

        /* Consume the processed message */
        size_t remaining = tls->reasm_len[level] - total;
        if (remaining > 0) {
            memmove(tls->reasm[level], tls->reasm[level] + total, remaining);
        }
        tls->reasm_len[level] = remaining;
    }

    return XQC_OK;
}


/* ---- Delegating functions (identical to original xqc_tls.c) ---- */

xqc_int_t
xqc_tls_encrypt_header(xqc_tls_t *tls, xqc_encrypt_level_t level,
    xqc_pkt_type_t pkt_type, uint8_t *header, uint8_t *pktno, uint8_t *end)
{
    return xqc_crypto_encrypt_header(tls->crypto[level], pkt_type, header, pktno, end);
}

xqc_int_t
xqc_tls_decrypt_header(xqc_tls_t *tls, xqc_encrypt_level_t level,
    xqc_pkt_type_t pkt_type, uint8_t *header, uint8_t *pktno, uint8_t *end)
{
    xqc_crypto_t *crypto = tls->crypto[level];
    if (!crypto) {
        xqc_log(tls->log, XQC_LOG_ERROR, "|crypto not initialized|level:%d|", level);
        return -XQC_TLS_INVALID_STATE;
    }
    return xqc_crypto_decrypt_header(crypto, pkt_type, header, pktno, end);
}

xqc_int_t
xqc_tls_encrypt_payload(xqc_tls_t *tls, xqc_encrypt_level_t level,
    uint64_t pktno, uint32_t path_id,
    uint8_t *header, size_t header_len, uint8_t *payload, size_t payload_len,
    uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    xqc_uint_t key_phase = 0;
    if (level == XQC_ENC_LEV_1RTT) {
        key_phase = XQC_PACKET_SHORT_HEADER_KEY_PHASE(header);
        if (key_phase >= XQC_KEY_PHASE_CNT) return -XQC_TLS_INVALID_STATE;
    }
    return xqc_crypto_encrypt_payload(tls->crypto[level], pktno, key_phase, path_id,
        header, header_len, payload, payload_len, dst, dst_cap, dst_len);
}

xqc_int_t
xqc_tls_decrypt_payload(xqc_tls_t *tls, xqc_encrypt_level_t level,
    uint64_t pktno, uint32_t path_id,
    uint8_t *header, size_t header_len, uint8_t *payload, size_t payload_len,
    uint8_t *dst, size_t dst_cap, size_t *dst_len)
{
    xqc_crypto_t *crypto = tls->crypto[level];
    if (!crypto) {
        xqc_log(tls->log, XQC_LOG_ERROR, "|crypto not initialized|level:%d|", level);
        return -XQC_TLS_INVALID_STATE;
    }
    xqc_uint_t key_phase = 0;
    if (level == XQC_ENC_LEV_1RTT) {
        key_phase = XQC_PACKET_SHORT_HEADER_KEY_PHASE(header);
        if (key_phase >= XQC_KEY_PHASE_CNT) return -XQC_TLS_INVALID_STATE;
    }
    return xqc_crypto_decrypt_payload(crypto, pktno, key_phase, path_id,
        header, header_len, payload, payload_len, dst, dst_cap, dst_len);
}

xqc_bool_t
xqc_tls_is_key_ready(xqc_tls_t *tls, xqc_encrypt_level_t level, xqc_key_type_t key_type)
{
    if (!tls || !tls->crypto[level]) return XQC_FALSE;
    return xqc_crypto_is_key_ready(tls->crypto[level], key_type);
}

xqc_bool_t
xqc_tls_is_ready_to_send_early_data(xqc_tls_t *tls)
{
    return XQC_FALSE; /* 0-RTT not supported in mbedTLS backend */
}

xqc_tls_early_data_accept_t
xqc_tls_is_early_data_accepted(xqc_tls_t *tls)
{
    return XQC_TLS_NO_EARLY_DATA;
}

ssize_t
xqc_tls_aead_tag_len(xqc_tls_t *tls, xqc_encrypt_level_t level)
{
    xqc_crypto_t *crypto = tls->crypto[level];
    if (!crypto) return -XQC_TLS_INVALID_STATE;
    return xqc_crypto_aead_tag_len(crypto);
}

void
xqc_tls_set_no_crypto(xqc_tls_t *tls)
{
    tls->no_crypto = XQC_TRUE;
}

void
xqc_tls_set_1rtt_key_phase(xqc_tls_t *tls, xqc_uint_t key_phase)
{
    tls->crypto[XQC_ENC_LEV_1RTT]->key_phase = key_phase;
}

xqc_bool_t
xqc_tls_is_key_update_confirmed(xqc_tls_t *tls)
{
    return tls->key_update_confirmed;
}

xqc_int_t
xqc_tls_update_1rtt_keys(xqc_tls_t *tls, xqc_key_type_t type)
{
    xqc_crypto_t *crypto = tls->crypto[XQC_ENC_LEV_1RTT];
    if (!crypto) return -XQC_TLS_UPDATE_KEY_ERROR;

    xqc_int_t ret = xqc_crypto_derive_updated_keys(crypto, type);
    if (ret != XQC_OK) return -XQC_TLS_UPDATE_KEY_ERROR;

    if (type == XQC_KEY_TYPE_RX_READ) {
        tls->key_update_confirmed = XQC_FALSE;
    } else {
        tls->key_update_confirmed = XQC_TRUE;
    }
    return XQC_OK;
}

void
xqc_tls_discard_old_1rtt_keys(xqc_tls_t *tls)
{
    xqc_crypto_discard_old_keys(tls->crypto[XQC_ENC_LEV_1RTT]);
}

xqc_int_t
xqc_tls_cal_retry_integrity_tag(xqc_log_t *log,
    uint8_t *retry_pseudo_packet, size_t retry_pseudo_packet_len,
    uint8_t *dst, size_t dst_cap, size_t *dst_len, xqc_proto_version_t ver)
{
    xqc_crypto_t *crypto = xqc_crypto_create(XQC_TLS13_AES_128_GCM_SHA256, log);
    if (!crypto) return -XQC_TLS_NOMEM;

    xqc_int_t ret = xqc_crypto_aead_encrypt(crypto, (const uint8_t *)"", 0,
        (const uint8_t *)xqc_crypto_retry_key[ver], strlen(xqc_crypto_retry_key[ver]),
        (const uint8_t *)xqc_crypto_retry_nonce[ver], strlen(xqc_crypto_retry_nonce[ver]),
        retry_pseudo_packet, retry_pseudo_packet_len,
        dst, dst_cap, dst_len);

    xqc_crypto_destroy(crypto);
    return ret;
}

void
xqc_tls_get_selected_alpn(xqc_tls_t *tls, const char **out_alpn, size_t *out_len)
{
    if (!tls) return;
    *out_alpn = tls->selected_alpn;
    *out_len = tls->selected_alpn_len;
}

xqc_int_t
xqc_tls_update_tp(xqc_tls_t *tls, uint8_t *tp_buf, size_t tp_len)
{
    if (tls->local_tp) xqc_free(tls->local_tp);
    tls->local_tp = xqc_malloc(tp_len);
    if (!tls->local_tp) return -XQC_TLS_NOBUF;
    memcpy(tls->local_tp, tp_buf, tp_len);
    tls->local_tp_len = tp_len;
    return XQC_OK;
}

void *
xqc_tls_get_ssl(xqc_tls_t *tls)
{
    return NULL;
}
