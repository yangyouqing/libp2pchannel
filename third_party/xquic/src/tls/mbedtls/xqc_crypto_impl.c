/**
 * xqc_crypto_impl.c -- AEAD encrypt/decrypt and HP mask using mbedTLS
 */

#include "src/tls/xqc_crypto.h"

#include <mbedtls/gcm.h>
#include <mbedtls/chachapoly.h>
#include <mbedtls/chacha20.h>
#include <mbedtls/aes.h>
#include <string.h>
#include <stdlib.h>


/* ---- AEAD context ---- */

typedef struct {
    int algo; /* 0=AES-128-GCM, 1=AES-256-GCM, 2=ChaCha20-Poly1305 */
    union {
        mbedtls_gcm_context     gcm;
        mbedtls_chachapoly_context cp;
    } u;
} xqc_mbedtls_aead_ctx_t;

void *
xqc_aead_ctx_new(const xqc_pkt_protect_aead_t *pp_aead, xqc_key_type_t type,
                 const uint8_t *key, size_t noncelen)
{
    (void)noncelen;
    (void)type;

    xqc_mbedtls_aead_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->algo = pp_aead->aead.algo;

    if (ctx->algo <= 1) {
        mbedtls_gcm_init(&ctx->u.gcm);
        int bits = pp_aead->aead.key_bytes * 8;
        if (mbedtls_gcm_setkey(&ctx->u.gcm, MBEDTLS_CIPHER_ID_AES, key, bits) != 0) {
            free(ctx);
            return NULL;
        }
    } else {
        mbedtls_chachapoly_init(&ctx->u.cp);
        if (mbedtls_chachapoly_setkey(&ctx->u.cp, key) != 0) {
            free(ctx);
            return NULL;
        }
    }
    return ctx;
}

void
xqc_aead_ctx_free(void *aead_ctx)
{
    if (!aead_ctx) return;
    xqc_mbedtls_aead_ctx_t *ctx = aead_ctx;
    if (ctx->algo <= 1) {
        mbedtls_gcm_free(&ctx->u.gcm);
    } else {
        mbedtls_chachapoly_free(&ctx->u.cp);
    }
    free(ctx);
}

xqc_int_t
xqc_mbedtls_aead_encrypt(const xqc_pkt_protect_aead_t *pp_aead, void *aead_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *plaintext, size_t plaintextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *nonce, size_t noncelen,
    const uint8_t *ad, size_t adlen)
{
    (void)pp_aead;
    (void)key;
    (void)keylen;

    xqc_mbedtls_aead_ctx_t *ctx = aead_ctx;
    if (!ctx) return -XQC_TLS_INVALID_ARGUMENT;

    size_t taglen = 16;
    if (destcap < plaintextlen + taglen) {
        return -XQC_TLS_ENCRYPT_DATA_ERROR;
    }

    int ret;
    if (ctx->algo <= 1) {
        uint8_t tag[16];
        ret = mbedtls_gcm_crypt_and_tag(&ctx->u.gcm, MBEDTLS_GCM_ENCRYPT,
            plaintextlen, nonce, noncelen, ad, adlen,
            plaintext, dest, taglen, tag);
        if (ret != 0) return -XQC_TLS_ENCRYPT_DATA_ERROR;
        memcpy(dest + plaintextlen, tag, taglen);
    } else {
        ret = mbedtls_chachapoly_encrypt_and_tag(&ctx->u.cp,
            plaintextlen, nonce, ad, adlen,
            plaintext, dest, dest + plaintextlen);
        if (ret != 0) return -XQC_TLS_ENCRYPT_DATA_ERROR;
    }

    *destlen = plaintextlen + taglen;
    return XQC_OK;
}

xqc_int_t
xqc_mbedtls_aead_decrypt(const xqc_pkt_protect_aead_t *pp_aead, void *aead_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *ciphertext, size_t ciphertextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *nonce, size_t noncelen,
    const uint8_t *ad, size_t adlen)
{
    (void)pp_aead;
    (void)key;
    (void)keylen;

    xqc_mbedtls_aead_ctx_t *ctx = aead_ctx;
    if (!ctx) return -XQC_TLS_INVALID_ARGUMENT;

    size_t taglen = 16;
    if (ciphertextlen < taglen) return -XQC_TLS_DECRYPT_DATA_ERROR;
    size_t ptlen = ciphertextlen - taglen;

    int ret;
    if (ctx->algo <= 1) {
        ret = mbedtls_gcm_auth_decrypt(&ctx->u.gcm, ptlen,
            nonce, noncelen, ad, adlen,
            ciphertext + ptlen, taglen,
            ciphertext, dest);
        if (ret != 0) return -XQC_TLS_DECRYPT_DATA_ERROR;
    } else {
        ret = mbedtls_chachapoly_auth_decrypt(&ctx->u.cp,
            ptlen, nonce, ad, adlen,
            ciphertext + ptlen, ciphertext, dest);
        if (ret != 0) return -XQC_TLS_DECRYPT_DATA_ERROR;
    }

    *destlen = ptlen;
    return XQC_OK;
}


/* ---- Header protection context ---- */

typedef struct {
    int algo;
    union {
        mbedtls_aes_context aes;
    } u;
    uint8_t key[32];
} xqc_mbedtls_hp_ctx_t;

void *
xqc_hp_ctx_new(const xqc_hdr_protect_cipher_t *hp_cipher, const uint8_t *key)
{
    xqc_mbedtls_hp_ctx_t *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) return NULL;

    ctx->algo = hp_cipher->cipher.algo;
    memcpy(ctx->key, key, hp_cipher->keylen);

    if (ctx->algo <= 1) {
        int bits = hp_cipher->cipher.key_bytes * 8;
        mbedtls_aes_init(&ctx->u.aes);
        if (mbedtls_aes_setkey_enc(&ctx->u.aes, key, bits) != 0) {
            free(ctx);
            return NULL;
        }
    }
    return ctx;
}

void
xqc_hp_ctx_free(void *hp_ctx)
{
    if (!hp_ctx) return;
    xqc_mbedtls_hp_ctx_t *ctx = hp_ctx;
    if (ctx->algo <= 1) {
        mbedtls_aes_free(&ctx->u.aes);
    }
    free(ctx);
}

xqc_int_t
xqc_mbedtls_hp_mask(const xqc_hdr_protect_cipher_t *hp_cipher, void *hp_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *plaintext, size_t plaintextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *sample, size_t samplelen)
{
    (void)hp_cipher;
    (void)key;
    (void)keylen;
    (void)destcap;

    xqc_mbedtls_hp_ctx_t *ctx = hp_ctx;
    if (!ctx) return -XQC_TLS_INVALID_ARGUMENT;

    /*
     * AES-ECB encrypt the sample to produce the HP mask.
     * The sample is 16 bytes, we encrypt it to get the mask,
     * then XOR the mask with plaintext (done by caller's xqc_crypto layer).
     *
     * Actually xquic HP uses AES-CTR with sample as IV, encrypting plaintext.
     * We replicate the BoringSSL behavior: EVP_EncryptInit(CTR, key, sample)
     * then EVP_EncryptUpdate(plaintext → dest).
     */
    uint8_t iv[16];
    memcpy(iv, sample, 16);

    size_t nc_off = 0;
    uint8_t stream_block[16] = {0};
    if (mbedtls_aes_crypt_ctr(&ctx->u.aes, plaintextlen, &nc_off, iv,
                               stream_block, plaintext, dest) != 0)
    {
        return -XQC_TLS_ENCRYPT_DATA_ERROR;
    }
    *destlen = plaintextlen;
    return XQC_OK;
}

xqc_int_t
xqc_mbedtls_hp_mask_chacha20(const xqc_hdr_protect_cipher_t *hp_cipher, void *hp_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *plaintext, size_t plaintextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *sample, size_t samplelen)
{
    (void)hp_cipher;
    (void)hp_ctx;
    (void)destcap;

    if (keylen != 32 || samplelen != 16) {
        return -XQC_TLS_INVALID_ARGUMENT;
    }

    uint32_t counter;
    memcpy(&counter, sample, sizeof(counter));

    if (mbedtls_chacha20_crypt(key, sample + 4, counter,
                                plaintextlen, plaintext, dest) != 0)
    {
        return -XQC_TLS_ENCRYPT_DATA_ERROR;
    }

    *destlen = plaintextlen;
    return XQC_OK;
}
