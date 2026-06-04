/*
 * xbl_crypto.c - SHA-256 and HMAC-SHA256 helpers (mbed TLS), used to sign score
 * submissions with the game secret. mbed TLS is already linked for the HTTPS
 * client, so this adds no new dependency.
 */
#include "xblsdk.h"

#include <string.h>

#include <mbedtls/md.h>

static void to_hex(const unsigned char *in, size_t n, char *out)
{
    static const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[(in[i] >> 4) & 0xF];
        out[i * 2 + 1] = hex[in[i] & 0xF];
    }
    out[n * 2] = '\0';
}

int xbl_sha256_hex(const unsigned char *msg, size_t len, char *out)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info) {
        return XBL_ERR_CONFIG;
    }
    unsigned char digest[32];
    if (mbedtls_md(info, msg, len, digest) != 0) {
        return XBL_ERR_CONFIG;
    }
    to_hex(digest, sizeof(digest), out);
    return XBL_OK;
}

int xbl_hmac_sha256_hex(const char *key, const unsigned char *msg, size_t len, char *out)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (!info || !key) {
        return XBL_ERR_CONFIG;
    }
    unsigned char digest[32];
    if (mbedtls_md_hmac(info, (const unsigned char *)key, strlen(key), msg, len, digest) != 0) {
        return XBL_ERR_CONFIG;
    }
    to_hex(digest, sizeof(digest), out);
    return XBL_OK;
}
