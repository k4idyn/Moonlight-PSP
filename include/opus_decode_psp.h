/* opus_decode_psp.h - Opus multistream decoder interface (48 kHz stereo, fixed-point) */
#ifndef OPUS_DECODE_PSP_H
#define OPUS_DECODE_PSP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize Opus multistream decoder for Moonlight stereo audio.
 * sample_rate: 48000
 * channels: 2
 * streams: 1
 * coupled_streams: 1
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

/* Shutdown and free decoder */
void opus_psp_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif /* OPUS_DECODE_PSP_H */
