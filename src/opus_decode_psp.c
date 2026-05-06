/*
 * opus_decode_psp.c - Opus Audio Decoder for PSP
 *
 * Multistream Opus decoder wrapper (48 kHz stereo, fixed-point Silk+CELT).
 * Receives encrypted RTP audio packets, decodes via libopus, outputs signed
 * 16-bit PCM for the PSP hardware audio channel.
 */

#include "opus_decode_psp.h"
#include <string.h>
#include <stdio.h>
#include <pspiofilemgr.h>

#include "diag_log.h"

/* Opus multistream decoder header */
#include "opus_multistream.h"

static OpusMSDecoder *s_decoder = NULL;
static int s_channels = 0;
static int s_initialized = 0;
static int s_decode_error_logs = 0;
/* Frame size (samples/ch) of the most recently decoded Opus packet.
 * Used by the audio thread to invoke PLC with the correct frame duration. */
static int s_last_frame_size = 240; /* default: 5 ms @ 48 kHz */

#define OPUS_STATIC_DECODER_BYTES  (32 * 1024)
static unsigned char s_decoder_storage[OPUS_STATIC_DECODER_BYTES] __attribute__((aligned(16)));

#define opus_log(fmt, ...) diag_log_write("OPUS", fmt, ##__VA_ARGS__)

int opus_psp_init(int sample_rate, int channels, int streams, int coupled_streams)
{
    int err;
    /* Standard stereo mapping: channel 0 = left, channel 1 = right */
    unsigned char mapping[2] = { 0, 1 };

    if (s_initialized) {
        opus_psp_shutdown();
    }

    s_channels = channels;
    s_decode_error_logs = 0;

    {
        int need = opus_multistream_decoder_get_size(streams, coupled_streams);
        if (need <= 0 || need > OPUS_STATIC_DECODER_BYTES) {
            opus_log("[OPUS] static decoder buffer too small: need=%d have=%d\n",
                     need, OPUS_STATIC_DECODER_BYTES);
            s_decoder = NULL;
            return -1;
        }
    }

    s_decoder = (OpusMSDecoder *)s_decoder_storage;
    memset(s_decoder_storage, 0, sizeof(s_decoder_storage));
    err = opus_multistream_decoder_init(
        s_decoder,
        sample_rate, channels, streams, coupled_streams,
        mapping);

    if (s_decoder == NULL || err != 0) {
        opus_log("[OPUS] decoder create failed: err=%d\n", err);
        s_decoder = NULL;
        return -1;
    }

    s_initialized = 1;
    opus_log("[OPUS] decoder initialized: %dHz %dch %dstreams %dcoupled\n",
             sample_rate, channels, streams, coupled_streams);
    return 0;
}

int opus_psp_decode(const unsigned char *opus_data, int opus_len,
                    int16_t *pcm_out, int frame_size)
{
    int samples;

    if (!s_initialized || s_decoder == NULL) {
        return -1;
    }

    /* opus_data == NULL means packet loss — trigger PLC */
    samples = opus_multistream_decode(
        s_decoder,
        opus_data, (opus_int32)opus_len,
        pcm_out, frame_size,
        0  /* decode_fec = 0: normal decode */
    );

    if (samples < 0) {
        /* Log first 10 errors, then every 50th so audio health stays visible */
        if (s_decode_error_logs < 10 || (s_decode_error_logs % 50) == 0) {
            opus_log("[OPUS] decode error: %d (total=%d)\n", samples, s_decode_error_logs + 1);
        }
        s_decode_error_logs++;
        return -1;
    }

    s_last_frame_size = samples;
    return samples;
}

int opus_psp_decode_fec(const unsigned char *opus_data, int opus_len,
                        int16_t *pcm_out, int frame_size)
{
    int samples;

    if (!s_initialized || s_decoder == NULL) {
        return -1;
    }

    /* decode_fec = 1: extract the in-band FEC data from opus_data to
     * reconstruct the PREVIOUS lost frame.  The Opus encoder embeds a
     * low-bitrate copy of the prior frame in each packet specifically
     * for packet loss recovery.  Quality is better than PLC (NULL decode)
     * because it uses actual encoded audio data rather than synthesis. */
    samples = opus_multistream_decode(
        s_decoder,
        opus_data, (opus_int32)opus_len,
        pcm_out, frame_size,
        1  /* decode_fec = 1: use in-band FEC from this packet */
    );

    if (samples < 0) {
        if (s_decode_error_logs < 10 || (s_decode_error_logs % 50) == 0) {
            opus_log("[OPUS] FEC decode error: %d (total=%d)\n", samples, s_decode_error_logs + 1);
        }
        s_decode_error_logs++;
        return -1;
    }

    /* Don't update s_last_frame_size — the FEC decode produces the
     * previous frame's size, not the current packet's.  The caller
     * will do a normal decode of the current packet next. */
    return samples;
}

int opus_psp_last_frame_size(void)
{
    return s_last_frame_size;
}

void opus_psp_shutdown(void)
{
    if (s_decoder != NULL) {
        s_decoder = NULL;
    }
    s_initialized = 0;
    s_channels = 0;
    s_decode_error_logs = 0;
    s_last_frame_size = 240;
}
