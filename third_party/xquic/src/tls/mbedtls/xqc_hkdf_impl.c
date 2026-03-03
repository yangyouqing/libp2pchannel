/**
 * xqc_hkdf_impl.c -- HKDF extract/expand using mbedTLS
 */

#include "src/tls/xqc_hkdf.h"

#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

xqc_int_t
xqc_hkdf_extract(uint8_t *dest, size_t destlen,
    const uint8_t *secret, size_t secretlen,
    const uint8_t *salt, size_t saltlen,
    const xqc_digest_t *md)
{
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md->md_type);
    if (md_info == NULL) {
        return -XQC_TLS_DERIVE_KEY_ERROR;
    }

    int ret = mbedtls_hkdf_extract(md_info, salt, saltlen, secret, secretlen, dest);
    if (ret != 0) {
        return -XQC_TLS_DERIVE_KEY_ERROR;
    }

    return XQC_OK;
}

xqc_int_t
xqc_hkdf_expand(uint8_t *dest, size_t destlen,
    const uint8_t *secret, size_t secretlen,
    const uint8_t *info, size_t infolen,
    const xqc_digest_t *md)
{
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(md->md_type);
    if (md_info == NULL) {
        return -XQC_TLS_DERIVE_KEY_ERROR;
    }

    int ret = mbedtls_hkdf_expand(md_info, secret, secretlen, info, infolen, dest, destlen);
    if (ret != 0) {
        return -XQC_TLS_DERIVE_KEY_ERROR;
    }

    return XQC_OK;
}
