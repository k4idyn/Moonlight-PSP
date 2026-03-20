/**
 * @file audio_decoder.h
 * @brief Audio decoder module for PSP Moonlight
 * 
 * Decodes Opus audio and outputs via sceAudio.
 */

#ifndef AUDIO_DECODER_H
#define AUDIO_DECODER_H

#include <pspkernel.h>
#include <opus/opus.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct AudioDecoder AudioDecoder;

/* Create and destroy functions */
AudioDecoder* audio_decoder_create(void);
void audio_decoder_destroy(AudioDecoder* decoder);

/* Initialization and update */
int audio_decoder_init(AudioDecoder* decoder);
void audio_decoder_update(AudioDecoder* decoder);

/* Audio data handling */
int audio_decoder_submit_packet(AudioDecoder* decoder, const unsigned char* data, unsigned int size);

/* Status */
int audio_decoder_is_initialized(AudioDecoder* decoder);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_DECODER_H */