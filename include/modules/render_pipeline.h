/**
 * @file render_pipeline.h
 * @brief Render pipeline module for PSP Moonlight
 * 
 * Handles double buffering and VBlank synchronization for presenting video frames.
 */

#ifndef RENDER_PIPELINE_H
#define RENDER_PIPELINE_H

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include "video_decoder.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declaration */
typedef struct RenderPipeline RenderPipeline;

/* Create and destroy functions */
RenderPipeline* render_pipeline_create(void);
void render_pipeline_destroy(RenderPipeline* pipeline);

/* Initialization and update */
int render_pipeline_init(RenderPipeline* pipeline);
void render_pipeline_start(RenderPipeline* pipeline);
void render_pipeline_stop(RenderPipeline* pipeline);
void render_pipeline_draw_video(RenderPipeline* pipeline);
void render_pipeline_update(RenderPipeline* pipeline);
void render_pipeline_draw_rect(RenderPipeline* pipeline, int x, int y, int w, int h, unsigned int color);
int render_pipeline_is_ready(RenderPipeline* pipeline);

/* Setter for video decoder */
void render_pipeline_set_video_decoder(RenderPipeline* pipeline, VideoDecoder* decoder);

#ifdef __cplusplus
}
#endif

#endif /* RENDER_PIPELINE_H */