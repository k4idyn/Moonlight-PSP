/*
 * stream_crypto.c - Per-packet stream decryption
 *
 * Video: AES-128-GCM.  Each encrypted video packet starts with a 32-byte
 *        ENC_VIDEO_HEADER: iv[12] + frameNumber[4] + tag[16], followed by
 *        the GCM ciphertext.
 *
 * Audio: AES-128-CBC.  The IV is constructed per-packet as:
 *        BE32(avRiKeyId + rtp_seq) in bytes [0..3], bytes [4..15] = 0.
 *
 * Uses mbedTLS AES-CBC and AES-GCM (already linked in the Makefile).
 */

#include "stream_crypto.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#include <pspkernel.h>
#include <pspiofilemgr.h>

#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"

/* Size of the ENC_VIDEO_HEADER prepended to each GCM video payload */
#define ENC_VIDEO_HDR_SIZE  32  /* iv[12] + frameNumber[4] + tag[16] */

/*--------------------------------------------------------------------------
 * Module state
 *--------------------------------------------------------------------------*/
static mbedtls_aes_context s_aes_ctx;       /* for audio CBC decrypt */
static mbedtls_gcm_context s_gcm_ctx;       /* for video GCM decrypt */
static unsigned char       s_session_key[16];
static int                 s_initialized = 0;
static volatile unsigned int s_error_count = 0;

/* Crypto fatal flag — set after too many consecutive decrypt failures */
volatile int g_crypto_fatal = 0;

/*--------------------------------------------------------------------------
 * Debug log helper
 *--------------------------------------------------------------------------*/
#include "diag_log.h"
#include "decode_flags.h"
#define crypto_log(fmt, ...) diag_log_write("CRYPTO", fmt, ##__VA_ARGS__)

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

int stream_crypto_init(const unsigned char *key)
{
    int ret;

    if (!key) {
        crypto_log("[CRYPTO] init failed: NULL key\n");
        return -1;
    }

    memcpy(s_session_key, key, 16);

    /* Audio: AES-CBC decrypt context */
    mbedtls_aes_init(&s_aes_ctx);
    ret = mbedtls_aes_setkey_dec(&s_aes_ctx, s_session_key, 128);
    if (ret != 0) {
        crypto_log("[CRYPTO] aes_setkey_dec failed: -0x%04X\n", -ret);
        return -1;
    }

    /* Video: AES-GCM context */
    mbedtls_gcm_init(&s_gcm_ctx);
    ret = mbedtls_gcm_setkey(&s_gcm_ctx, MBEDTLS_CIPHER_ID_AES,
                             s_session_key, 128);
    if (ret != 0) {
        crypto_log("[CRYPTO] gcm_setkey failed: -0x%04X\n", -ret);
        mbedtls_aes_free(&s_aes_ctx);
        return -1;
    }

    s_initialized = 1;
    s_error_count = 0;
    crypto_log("[CRYPTO] session key initialized OK (GCM+CBC)\n");
    return 0;
}

int stream_crypto_decrypt_video(const unsigned char *payload, int payload_len,
                                unsigned char *out_data, int *out_len)
{
    const unsigned char *iv;
    const unsigned char *tag;
    const unsigned char *ciphertext;
    int ct_len;
    int ret;

    if (!s_initialized) {
        return -1;
    }

    if (payload_len <= ENC_VIDEO_HDR_SIZE) {
        crypto_log("[CRYPTO] video payload too short: %d\n", payload_len);
        s_error_count++;
        return -1;
    }

    /* Parse ENC_VIDEO_HEADER: iv[12] + frameNumber[4] + tag[16] */
    iv         = payload;                   /* 12 bytes */
    /* frameNumber at payload+12, 4 bytes — not needed for decrypt */
    tag        = payload + 16;              /* 16 bytes */
    ciphertext = payload + ENC_VIDEO_HDR_SIZE;
    ct_len     = payload_len - ENC_VIDEO_HDR_SIZE;

    ret = mbedtls_gcm_auth_decrypt(&s_gcm_ctx,
                                   (size_t)ct_len,
                                   iv, 12,
                                   NULL, 0,    /* no AAD */
                                   tag, 16,
                                   ciphertext,
                                   out_data);
    if (ret != 0) {
        s_error_count++;
        if (s_error_count <= 5) {
            crypto_log("[CRYPTO] video GCM decrypt failed: -0x%04X (ct_len=%d)\n",
                       -ret, ct_len);
        }
        /* After 30 consecutive decryption failures, signal session reset (C-4) */
        if (s_error_count >= 30) {
            crypto_log("[CRYPTO] FATAL: %u consecutive decrypt failures — session key invalid\n",
                       s_error_count);
            g_crypto_fatal = 1;
        }
        return -1;
    }
    s_error_count = 0; /* Reset on success */

    if (out_len) {
        *out_len = ct_len;
    }
    return 0;
}

int stream_crypto_decrypt_audio(unsigned char *payload, int payload_len,
                                unsigned short rtp_seq, unsigned int ri_key_id)
{
    unsigned char iv[16];
    unsigned int iv_val;
    int ret;

    if (!s_initialized) {
        return -1;
    }

    if (payload_len <= 0) {
        return -1;
    }

    if ((payload_len & 0x0F) != 0) {
        crypto_log("[CRYPTO] invalid audio block size %d (not multiple of 16)\n",
                   payload_len);
        s_error_count++;
        return -1;
    }

    /* Construct per-packet IV: BE32(ri_key_id + rtp_seq) in bytes [0..3],
     * bytes [4..15] = 0.  This matches moonlight-common-c AudioStream.c. */
    iv_val = ri_key_id + (unsigned int)rtp_seq;
    iv[0] = (unsigned char)(iv_val >> 24);
    iv[1] = (unsigned char)(iv_val >> 16);
    iv[2] = (unsigned char)(iv_val >>  8);
    iv[3] = (unsigned char)(iv_val);
    memset(iv + 4, 0, 12);

    ret = mbedtls_aes_crypt_cbc(&s_aes_ctx, MBEDTLS_AES_DECRYPT,
                                 (size_t)payload_len,
                                 iv,
                                 payload, payload);
    if (ret != 0) {
        crypto_log("[CRYPTO] audio decrypt failed: -0x%04X\n", -ret);
        s_error_count++;
        return -1;
    }

    return 0;
}

unsigned int stream_crypto_get_error_count(void)
{
    return s_error_count;
}

void stream_crypto_shutdown(void)
{
    if (s_initialized) {
        mbedtls_aes_free(&s_aes_ctx);
        mbedtls_gcm_free(&s_gcm_ctx);
        /* Wipe key material from memory */
        memset(s_session_key, 0, sizeof(s_session_key));
        if (s_error_count > 0) {
            crypto_log("[CRYPTO] shutdown with %u total errors\n", s_error_count);
        }
        s_initialized = 0;
    }
}
