/**
 * xqc_aead_impl.h -- mbedTLS AEAD / cipher / digest macro wiring for xquic
 */

#ifndef XQC_AEAD_IMPL_H_
#define XQC_AEAD_IMPL_H_

#ifndef XQC_CRYPTO_PRIVATE
#error "Do not include this file directly, include xqc_crypto.h"
#endif

/*
 * Cipher IDs -- same numeric values as OpenSSL TLS 1.3 cipher suite IDs
 * so that the rest of xquic can use them unchanged.
 */
#define XQC_TLS13_AES_128_GCM_SHA256       0x03001301u
#define XQC_TLS13_AES_256_GCM_SHA384       0x03001302u
#define XQC_TLS13_CHACHA20_POLY1305_SHA256 0x03001303u

/*
 * Opaque handles stored inside xqc_pkt_protect_aead_t / xqc_hdr_protect_cipher_t.
 * mbedTLS does not have a single "cipher object" like EVP_AEAD / EVP_CIPHER;
 * we store the algorithm enum and key length instead.
 */
typedef struct {
    int   algo;       /* 0=AES-128-GCM, 1=AES-256-GCM, 2=ChaCha20-Poly1305 */
    int   key_bytes;
} xqc_mbedtls_aead_t;

typedef struct {
    int   algo;       /* 0=AES-128-CTR, 1=AES-256-CTR, 2=ChaCha20 */
    int   key_bytes;
} xqc_mbedtls_cipher_t;

#define XQC_AEAD_SUITES_IMPL    xqc_mbedtls_aead_t
#define XQC_CIPHER_SUITES_IMPL  xqc_mbedtls_cipher_t

#define XQC_AEAD_OVERHEAD_IMPL(obj, cln)  (0) + (obj)->taglen

/* NID_undef equivalent for no-crypto mode */
#ifndef NID_undef
#define NID_undef 0
#endif

/* forward declarations */
xqc_int_t xqc_mbedtls_aead_encrypt(const xqc_pkt_protect_aead_t *pp_aead, void *aead_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *plaintext, size_t plaintextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *nonce, size_t noncelen,
    const uint8_t *ad, size_t adlen);

xqc_int_t xqc_mbedtls_aead_decrypt(const xqc_pkt_protect_aead_t *pp_aead, void *aead_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *ciphertext, size_t ciphertextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *nonce, size_t noncelen,
    const uint8_t *ad, size_t adlen);

xqc_int_t xqc_mbedtls_hp_mask(const xqc_hdr_protect_cipher_t *hp_cipher, void *hp_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *plaintext, size_t plaintextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *sample, size_t samplelen);

xqc_int_t xqc_mbedtls_hp_mask_chacha20(const xqc_hdr_protect_cipher_t *hp_cipher, void *hp_ctx,
    uint8_t *dest, size_t destcap, size_t *destlen,
    const uint8_t *plaintext, size_t plaintextlen,
    const uint8_t *key, size_t keylen,
    const uint8_t *sample, size_t samplelen);

/* ---- AEAD init macros ---- */

#define XQC_AEAD_INIT_AES_GCM_IMPL(obj, d) do {                            \
    xqc_pkt_protect_aead_t *___a = (obj);                                   \
    ___a->aead.algo      = ((d) == 128) ? 0 : 1;                           \
    ___a->aead.key_bytes = (d) / 8;                                         \
    ___a->keylen         = (d) / 8;                                         \
    ___a->noncelen       = 12;                                              \
    ___a->taglen         = 16;                                              \
    ___a->encrypt        = xqc_mbedtls_aead_encrypt;                        \
    ___a->decrypt        = xqc_mbedtls_aead_decrypt;                        \
} while (0)

#define XQC_AEAD_INIT_CHACHA20_POLY1305_IMPL(obj) do {                      \
    xqc_pkt_protect_aead_t *___a = (obj);                                   \
    ___a->aead.algo      = 2;                                               \
    ___a->aead.key_bytes = 32;                                              \
    ___a->keylen         = 32;                                              \
    ___a->noncelen       = 12;                                              \
    ___a->taglen         = 16;                                              \
    ___a->encrypt        = xqc_mbedtls_aead_encrypt;                        \
    ___a->decrypt        = xqc_mbedtls_aead_decrypt;                        \
} while (0)

/* ---- Cipher (HP) init macros ---- */

#define XQC_CIPHER_INIT_AES_CTR_IMPL(obj, d) do {                          \
    xqc_hdr_protect_cipher_t *___c = (obj);                                 \
    ___c->cipher.algo      = ((d) == 128) ? 0 : 1;                         \
    ___c->cipher.key_bytes = (d) / 8;                                       \
    ___c->keylen           = (d) / 8;                                       \
    ___c->noncelen         = 16;                                            \
    ___c->hp_mask          = xqc_mbedtls_hp_mask;                           \
} while (0)

#define XQC_CIPHER_INIT_CHACHA20_IMPL(obj) do {                             \
    xqc_hdr_protect_cipher_t *___c = (obj);                                 \
    ___c->cipher.algo      = 2;                                             \
    ___c->cipher.key_bytes = 32;                                            \
    ___c->keylen           = 32;                                            \
    ___c->noncelen         = 16;                                            \
    ___c->hp_mask          = xqc_mbedtls_hp_mask_chacha20;                  \
} while (0)

#endif /* XQC_AEAD_IMPL_H_ */
