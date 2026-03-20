/**
 * @file render_pipeline.c
 * @brief GU render pipeline for PSP Moonlight — hardware-optimized
 *
 * Presents decoded video frames via GU textured quads with double buffering.
 */

#include "render_pipeline.h"
#include <pspdisplay.h>
#include <pspge.h>
#include <pspgu.h>
#include <pspkernel.h>
#include <stdlib.h>
#include <string.h>

#define BUF_WIDTH  512
#define SCR_WIDTH  480
#define SCR_HEIGHT 272

/** Shared display list for all GU modules, defined in main.c */
extern unsigned int __attribute__((aligned(16))) g_gu_display_list[16384];

struct Vertex {
  float u, v;
  float x, y, z;
};

struct VertexColor {
  unsigned int color;
  float x, y, z;
};


struct RenderPipeline {
  int initialized;
  int gu_ready;
  VideoDecoder *video_decoder;
};

RenderPipeline *render_pipeline_create(void) {
  return (RenderPipeline *)calloc(1, sizeof(RenderPipeline));
}

void render_pipeline_destroy(RenderPipeline *pipeline) {
  free(pipeline);
}

/* GU is now initialized centrally by ui_renderer */
int render_pipeline_init(RenderPipeline *p) {
  if (!p) return -1;
  p->initialized = 1;
  p->gu_ready = 1; /* Assume GU is ready from ui_renderer_init */
  return 0;
}

void render_pipeline_start(RenderPipeline *p) { (void)p; }

void render_pipeline_stop(RenderPipeline *p) {
  if (p && p->gu_ready) {
    p->gu_ready = 0;
  }
}

/* Draws the latest video frame into the CURRENT GU display list.
   Expects sceGuStart to have been called by the caller (ui_renderer_begin_frame). */
void render_pipeline_draw_video(RenderPipeline *p) {
  if (!p || !p->initialized || !p->video_decoder) return;

  unsigned int width = 0, height = 0;
  void *frame = video_decoder_get_output_frame(p->video_decoder, &width, &height);
  
  /* Absolute Perfection: If no new frame is ready, we MUST NOT return early.
     Returning early causes ui_renderer to clear the buffer to black,
     creating the "flickering on and off" effect if stream FPS < UI FPS.
     We reuse the last frame already uploaded to the GU texture. */
  if (!frame) {
      /* If we have an initialized video decoder, we assume there's a valid 
         texture already bound from the last successful draw. */
      sceGuEnable(GU_TEXTURE_2D);
      struct Vertex *v = (struct Vertex *)sceGuGetMemory(2 * sizeof(struct Vertex));
      v[0].u = 0.0f;          v[0].v = 0.0f;
      v[0].x = 0.0f;          v[0].y = 0.0f;           v[0].z = 0.0f;
      v[1].u = (float)SCR_WIDTH;  v[1].v = (float)SCR_HEIGHT; /* Use screen dims for safety */
      v[1].x = (float)SCR_WIDTH; v[1].y = (float)SCR_HEIGHT; v[1].z = 0.0f;
      // Re-draw with existing texture state
      sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
      return;
  }

  /* 2 bytes per pixel for 565 format */
  sceKernelDcacheWritebackInvalidateRange(frame, BUF_WIDTH * height * 2);

  struct Vertex *v = (struct Vertex *)sceGuGetMemory(2 * sizeof(struct Vertex));
  v[0].u = 0.0f;          v[0].v = 0.0f;
  v[0].x = 0.0f;          v[0].y = 0.0f;           v[0].z = 0.0f;
  v[1].u = (float)width;  v[1].v = (float)height;
  v[1].x = (float)SCR_WIDTH; v[1].y = (float)SCR_HEIGHT; v[1].z = 0.0f;

  sceGuEnable(GU_TEXTURE_2D);
  sceGuTexMode(GU_PSM_5650, 0, 0, 0);
  sceGuTexImage(0, 512, 512, BUF_WIDTH, frame);
  sceGuTexFunc(GU_TFX_REPLACE, GU_TCC_RGBA);
  sceGuTexFilter(GU_LINEAR, GU_LINEAR);

  sceGuDrawArray(GU_SPRITES, GU_TEXTURE_32BITF | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);
}

void render_pipeline_update(RenderPipeline *p) {
    /* Legacy wrapper for compatibility - if called alone, it must start/finish its own list */
    sceGuStart(GU_DIRECT, g_gu_display_list);
    sceGuClear(GU_COLOR_BUFFER_BIT);
    render_pipeline_draw_video(p);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuSwapBuffers();
}

void render_pipeline_set_video_decoder(RenderPipeline *p, VideoDecoder *decoder) {
  if (p) p->video_decoder = decoder;
}

void render_pipeline_draw_rect(RenderPipeline* p, int x, int y, int w, int h, unsigned int color) {
  if (!p || !p->gu_ready) return;

  sceGuStart(GU_DIRECT, g_gu_display_list);
  
  sceGuDisable(GU_TEXTURE_2D);
  sceGuEnable(GU_BLEND);
  sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);

  struct VertexColor *v = (struct VertexColor *)sceGuGetMemory(2 * sizeof(struct VertexColor));
  v[0].color = color;
  v[0].x = (float)x;      v[0].y = (float)y;      v[0].z = 0.0f;
  v[1].color = color;
  v[1].x = (float)(x+w);  v[1].y = (float)(y+h);  v[1].z = 0.0f;

  sceGuDrawArray(GU_SPRITES, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 2, 0, v);

  sceGuDisable(GU_BLEND);
  sceGuEnable(GU_TEXTURE_2D);
  
  sceGuFinish();
  sceGuSync(0, 0);
}

int render_pipeline_is_ready(RenderPipeline *p) {
    return (p && p->gu_ready);
}