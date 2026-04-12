/*
 * stream_crypto.h - Per-packet stream decryption (video: AES-128-GCM, audio: AES-128-CBC)
 *
 * After pairing and launch negotiation, the per-session AES-128 key
 * is derived from Sunshine's challenge-response exchange.
 *
 * Video packets are encrypted with AES-128-GCM.  Each packet carries a
 * 32-byte ENC_VIDEO_HEADER (iv[12] + frameNumber[4] + tag[16]) before
 * the ciphertext.
 *
 * Audio packets are encrypted with AES-128-CBC.  The IV is constructed
 * per-packet as BE32(avRiKeyId + rtp_seq) in the first 4 bytes of a
 * 16-byte IV, with the remaining 12 bytes zeroed.
 *
 * Usage:
 *   stream_crypto_init(key)               — set session key once
 *   stream_crypto_decrypt_video(...)      — decrypt video payload (GCM)
 *   stream_crypto_decrypt_audio(...)      — decrypt audio payload (CBC)
 *   stream_crypto_shutdown()              — clear key material
 */

#ifndef STREAM_CRYPTO_H
#define STREAM_CRYPTO_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * stream_crypto_init - Set the per-session AES-128 key.
 *
 * @key: 16-byte AES key derived from pairing challenge-response (rikey)
 *
 * Must be called before any decrypt calls.
 * Returns 0 on success, -1 on error.
 */
int stream_crypto_init(const unsigned char *key);

/*
 * stream_crypto_decrypt_video - Decrypt a video RTP payload in-place (AES-128-GCM).
 *
 * @payload:     pointer to the raw UDP video payload.
 *               The first 32 bytes are the ENC_VIDEO_HEADER:
 *                 iv[12] + frameNumber[4] + tag[16]
 *               followed by the GCM ciphertext (the encrypted RTP packet).
 * @payload_len: total length INCLUDING the 32-byte ENC_VIDEO_HEADER.
 * @out_data:    pointer where the decrypted plaintext will be written.
 *               Must have room for (payload_len - 32) bytes.
 * @out_len:     receives the number of decrypted bytes.
 *
 * Returns 0 on success, -1 on error.
 */
int stream_crypto_decrypt_video(const unsigned char *payload, int payload_len,
                                unsigned char *out_data, int *out_len);

/*
 * stream_crypto_decrypt_audio - Decrypt an audio RTP payload in-place (AES-128-CBC).
 *
 * @payload:     pointer to ciphertext (overwritten with plaintext)
 * @payload_len: length in bytes (MUST be multiple of 16)
 * @rtp_seq:     RTP sequence number from the packet header (host byte order)
 * @ri_key_id:   avRiKeyId from the launch/resume request
 *
 * Returns 0 on success, -1 on error.
 */
int stream_crypto_decrypt_audio(unsigned char *payload, int payload_len,
                                unsigned short rtp_seq, unsigned int ri_key_id);

/*
 * stream_crypto_get_error_count - Return cumulative crypto failures.
 */
unsigned int stream_crypto_get_error_count(void);

/*
 * stream_crypto_shutdown - Wipe key material from memory.
 */
void stream_crypto_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_CRYPTO_H */
