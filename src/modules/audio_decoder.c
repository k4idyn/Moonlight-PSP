/**
 * @file audio_decoder.c
 * @brief Audio decoder for PSP Moonlight — hardware-optimized
 *
 * Decodes Opus audio and outputs via sceAudio.
 * Fixed-point optimized for authentic PSP 1000 hardware.
 */

#include "audio_decoder.h"
#include <opus/opus.h>
#include <pspaudio.h>
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>

#include "logger.h"
#include "exception_handler.h"

#define OUTPUT_CHUNK_SAMPLES 1024  /* Stereo samples per sceAudio output */

struct AudioDecoder {
  int initialized;
  OpusDecoder *opus_decoder;
  int sample_rate;
  int channels;
  int frame_size;
  short *audio_buffer;
  int audio_channel;
  SceUID mutex;
  short *mix_buffer;
  int mix_buffer_capacity;
  int mix_buffer_size;
  int sample_count;
};

static int init_sceaudio(AudioDecoder *decoder);

AudioDecoder *audio_decoder_create(void) {
  AudioDecoder *decoder = (AudioDecoder *)calloc(1, sizeof(AudioDecoder));
  if (decoder) decoder->audio_channel = -1;
  return decoder;
}

void audio_decoder_destroy(AudioDecoder *decoder) {
  if (!decoder) return;
  if (decoder->opus_decoder) { opus_decoder_destroy(decoder->opus_decoder); }
  if (decoder->mutex > 0) { sceKernelDeleteSema(decoder->mutex); }
  if (decoder->audio_channel >= 0) { sceAudioChRelease(decoder->audio_channel); }
  free(decoder->audio_buffer);
  free(decoder->mix_buffer);
  free(decoder);
}

int audio_decoder_init(AudioDecoder *decoder) {
  if (!decoder) return -1;

  decoder->sample_rate = 48000;
  decoder->channels = 2;
  decoder->frame_size = 240;  /* Typical Opus frame at 48kHz/5ms */

  int error;
  decoder->opus_decoder = opus_decoder_create(decoder->sample_rate, decoder->channels, &error);
  if (!decoder->opus_decoder) {
    LOG_ERROR(COMPONENT_AUDIO, "Opus create failed: %s", opus_strerror(error));
    exception_handler_trigger_manually("Opus Creation Failure", error);
    return -1;
  }

  decoder->mutex = sceKernelCreateSema("audio_mtx", 0, 1, 1, NULL);
  if (decoder->mutex < 0) {
    LOG_ERROR(COMPONENT_AUDIO, "Sema create failed: 0x%08x", (unsigned int)decoder->mutex);
    exception_handler_trigger_manually("Audio Sema Creation Failure", decoder->mutex);
    opus_decoder_destroy(decoder->opus_decoder);
    decoder->opus_decoder = NULL;
    return -1;
  }

  if (init_sceaudio(decoder) < 0) {
    LOG_ERROR(COMPONENT_AUDIO, "sceAudio init failed");
    sceKernelDeleteSema(decoder->mutex);
    opus_decoder_destroy(decoder->opus_decoder);
    decoder->opus_decoder = NULL;
    return -1;
  }

  /* Allocate decode buffer: max Opus frame = 5760 samples/ch */
  decoder->audio_buffer = (short *)memalign(64, 5760 * decoder->channels * sizeof(short));
  if (!decoder->audio_buffer) { return -1; }

  /* Mix buffer = one output chunk, stereo interleaved */
  decoder->mix_buffer_capacity = OUTPUT_CHUNK_SAMPLES * 2;
  decoder->mix_buffer = (short *)memalign(64, decoder->mix_buffer_capacity * sizeof(short));
  if (decoder->mix_buffer) {
    memset(decoder->mix_buffer, 0, decoder->mix_buffer_capacity * sizeof(short));
  }
  if (!decoder->mix_buffer) { free(decoder->audio_buffer); return -1; }

  decoder->initialized = 1;
  LOG_INFO(COMPONENT_AUDIO, "Audio initialized: ch=%d rate=%d", decoder->audio_channel, decoder->sample_rate);
  return 0;
}

void audio_decoder_update(AudioDecoder *decoder) {
  (void)decoder; /* Audio output happens in submit_packet on the limelight thread */
}

int audio_decoder_submit_packet(AudioDecoder *decoder,
                                 const unsigned char *data, unsigned int size) {
  if (!decoder || !decoder->initialized || !data) return -1;

  if (sceKernelWaitSema(decoder->mutex, 1, NULL) < 0) return -1;

  /* Determine frame size from Opus header (no logging — hot path) */
  int spf = opus_packet_get_samples_per_frame(data, decoder->sample_rate);
  if (spf <= 0) spf = decoder->frame_size;
  decoder->frame_size = spf;

  /* Restore Opus decode with Complexity 0 for hardware perfection */
  opus_decoder_ctl(decoder->opus_decoder, OPUS_SET_COMPLEXITY(0));
  int nb_samples = opus_decode(decoder->opus_decoder, data, size, decoder->audio_buffer, spf, 0);
  if (nb_samples < 0) {
      static int error_count = 0;
      if (error_count++ % 100 == 0) {
          LOG_ERROR(COMPONENT_AUDIO, "Opus decode error: %d", nb_samples);
      }
      return -1;
  }

  /* Accumulate into mix buffer, flush when full */
  int total = nb_samples * decoder->channels;
  short *src = decoder->audio_buffer;
  while (total > 0) {
    int space = decoder->mix_buffer_capacity - decoder->mix_buffer_size;
    int copy = total < space ? total : space;
    memcpy(decoder->mix_buffer + decoder->mix_buffer_size, src, copy * sizeof(short));
    decoder->mix_buffer_size += copy;
    src += copy;
    total -= copy;

    if (decoder->mix_buffer_size >= decoder->mix_buffer_capacity) {
      /* Absolute Perfection: Check rest length to avoid blocking the network thread */
      if (sceAudioGetChannelRestLen(decoder->audio_channel) <= (decoder->sample_count / 2)) {
          int res = sceAudioOutputPanned(decoder->audio_channel, 0xFFFF, 0xFFFF, decoder->mix_buffer);
          if (res < 0) {
              static int output_error = 0;
              if (output_error++ % 100 == 0) {
                  LOG_INFO(COMPONENT_AUDIO, "Audio output busy (total skips: %d)", output_error);
              }
          }
      }
      decoder->mix_buffer_size = 0;
    }
  }

  sceKernelSignalSema(decoder->mutex, 1);
  return 0;
}

int audio_decoder_is_initialized(AudioDecoder *decoder) {
  return decoder ? decoder->initialized : 0;
}

static int init_sceaudio(AudioDecoder *decoder) {
  int sample_count = (OUTPUT_CHUNK_SAMPLES + 63) & ~63;
  decoder->sample_count = sample_count;
  decoder->audio_channel = sceAudioChReserve(-1, sample_count, PSP_AUDIO_FORMAT_STEREO);
  if (decoder->audio_channel < 0) {
      exception_handler_trigger_manually("sceAudioChReserve Failure", decoder->audio_channel);
      return decoder->audio_channel;
  }
  LOG_INFO(COMPONENT_AUDIO, "sceAudioChReserve OK: ch=%d samples=%d", decoder->audio_channel, sample_count);
  return 0;
}