/* opus_decode_psp.h - Opus decoder interface (48 kHz mono PSP output, fixed-point) */
#ifndef OPUS_DECODE_PSP_H
#define OPUS_DECODE_PSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize Opus decoder for Moonlight audio.
 * sample_rate: 48000
 * channels: 1 for the low-work PSP decode/output path
 * streams: 1
 * coupled_streams: 0 for mono
 * Returns 0 on success, -1 on failure */
int opus_psp_init(int sample_rate, int channels, int streams, int coupled_streams);

/* Decode one Opus packet to PCM S16LE interleaved samples.
 * opus_data: encrypted opus frame (NULL = packet loss concealment)
 * opus_len: length in bytes
 * pcm_out: output buffer (must fit frame_size * channels samples)
 * frame_size: max samples per channel to decode
 * Returns number of decoded samples per channel, or negative on error */
int opus_psp_decode(const unsigned char *opus_data, int opus_len,
                    int16_t *pcm_out, int frame_size);

/* Decode using Opus in-band FEC.  Call with the CURRENT packet's data
 * to recover the PREVIOUS lost packet using embedded forward error
 * correction.  The Opus encoder embeds a low-bitrate copy of the
 * previous frame's audio in each packet.  When a gap is detected,
 * call this with the next-received packet to reconstruct the lost one.
 * Returns number of decoded samples per channel, or negative on error */
int opus_psp_decode_fec(const unsigned char *opus_data, int opus_len,
                        int16_t *pcm_out, int frame_size);

/* Shutdown and free decoder */
void opus_psp_shutdown(void);

/* Return the frame size (samples per channel) of the last successfully
 * decoded packet.  Used to invoke PLC with the correct frame duration.
 * Returns 240 (5 ms @ 48 kHz) until the first successful decode. */
int opus_psp_last_frame_size(void);

#ifdef __cplusplus
}
#endif

#endif /* OPUS_DECODE_PSP_H */
