/**
 * @file video_decoder.h
 * @brief Video decoder module for PSP Moonlight
 * 
 * Interfaces with PSP Media Engine for hardware H.264 decoding.
 */

#ifndef VIDEO_DECODER_H
#define VIDEO_DECODER_H

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspmpeg.h> /* For MPEG/ME functions */

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct VideoDecoder VideoDecoder;

/* Create and destroy functions */
VideoDecoder* video_decoder_create(void);
void video_decoder_destroy(VideoDecoder* decoder);

/* Initialization and update */
int video_decoder_init(VideoDecoder* decoder);
void video_decoder_update(VideoDecoder* decoder);

/* Frame handling */
int video_decoder_submit_frame(VideoDecoder* decoder, const void* data, unsigned int size);
void* video_decoder_get_output_frame(VideoDecoder* decoder, unsigned int* width, unsigned int* height);
unsigned int video_decoder_get_frame_count(VideoDecoder* decoder);
unsigned int video_decoder_get_dropped_frames(VideoDecoder *decoder);
void video_decoder_refresh(VideoDecoder* decoder);
unsigned long long video_decoder_get_total_bytes(VideoDecoder* decoder);

/* Status */
int video_decoder_is_initialized(VideoDecoder* decoder);

/* Error query API */
int video_decoder_get_last_error(VideoDecoder* decoder);
const char* video_decoder_get_error_message(VideoDecoder* decoder);
void video_decoder_clear_error(VideoDecoder* decoder);

/* Configuration */
void video_decoder_set_stream_resolution(VideoDecoder* decoder, unsigned int width, unsigned int height);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_DECODER_H */