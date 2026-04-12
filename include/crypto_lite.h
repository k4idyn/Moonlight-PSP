/*
 * crypto_lite.h - Minimal AES-128-ECB and SHA-256 for PSP Moonlight pairing
 *
 * Pure C implementations that don't depend on mbedTLS or OpenSSL.
 * Only what's needed for the Moonlight pairing protocol.
 */

#ifndef CRYPTO_LITE_H
#define CRYPTO_LITE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * SHA-256
 *--------------------------------------------------------------------------*/

/* Compute SHA-256 hash of input data.
 * out must be at least 32 bytes. */
void sha256_hash(const unsigned char *data, size_t len, unsigned char *out);

/*--------------------------------------------------------------------------
 * AES-128-ECB
 *--------------------------------------------------------------------------*/

/* Encrypt 'len' bytes (must be multiple of 16) with AES-128-ECB.
 * key must be 16 bytes. */
void aes128_ecb_encrypt(const unsigned char *plaintext, int len,
                        const unsigned char *key, unsigned char *ciphertext);

/* Decrypt 'len' bytes (must be multiple of 16) with AES-128-ECB.
 * key must be 16 bytes. */
void aes128_ecb_decrypt(const unsigned char *ciphertext, int len,
                        const unsigned char *key, unsigned char *plaintext);

/*--------------------------------------------------------------------------
 * Hex conversion
 *--------------------------------------------------------------------------*/
void bytes_to_hex_lite(const unsigned char *in, char *out, size_t len);
void hex_to_bytes_lite(const char *in, unsigned char *out, size_t hex_len);

#ifdef __cplusplus
}
#endif

#endif /* CRYPTO_LITE_H */
