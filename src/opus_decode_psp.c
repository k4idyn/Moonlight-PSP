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

    s_decoder = opus_multistream_decoder_create(
        sample_rate, channels, streams, coupled_streams,
        mapping, &err);

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
        0  /* decode_fec = 0 */
    );

    if (samples < 0) {
        if (s_decode_error_logs < 10) {
            opus_log("[OPUS] decode error: %d\n", samples);
            s_decode_error_logs++;
        }
        return -1;
    }

    return samples;
}

void opus_psp_shutdown(void)
{
    if (s_decoder != NULL) {
        opus_multistream_decoder_destroy(s_decoder);
        s_decoder = NULL;
    }
    s_initialized = 0;
    s_channels = 0;
    s_decode_error_logs = 0;
}
