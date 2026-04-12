/*
 * stream_resolution.h - Unified Stream Resolution Configuration
 *
 * Single source of truth for ALL resolution-dependent parameters.
 * Eliminates "resolution drift" where different components use
 * different hardcoded values.
 *
 * Usage:
 *   1. Call stream_resolution_init(width, height) once at stream start
 *   2. All components read from g_stream_res instead of local defines
 *   3. PSP LCD constants (480x272) remain fixed (hardware limitation)
 *
 * NOTE: sw_decode_pipeline.h retains compile-time SW_FRAME_* defines
 * for the decommissioned CAVLC pipeline's static struct sizing.
 * The active FFmpeg path uses g_stream_res exclusively.
 */

#ifndef STREAM_RESOLUTION_H
#define STREAM_RESOLUTION_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSP LCD hardware constants — immutable, used only by display/UI */
#define PSP_LCD_WIDTH       480
#define PSP_LCD_HEIGHT      272
#define PSP_LCD_STRIDE      512     /* VRAM pitch (power-of-2) */

typedef struct {
    /* Base dimensions (from config) */
    int width;              /* Stream width  (e.g. 368, 480, 640) */
    int height;             /* Stream height (e.g. 208, 272, 360) */

    /* GPU-compatible stride (power-of-2, >= width) */
    int stride;             /* 512 for width<=512, 1024 for width>512 */

    /* Derived buffer sizes */
    int rgba_size;          /* stride * height * 4 (bytes per RGBA buffer) */
    int y_plane_size;       /* width * height */
    int uv_plane_size;      /* (width/2) * (height/2) */
    int yuv_total_size;     /* y + 2*uv */

    /* Macroblock grid (H.264: width/16 x height/16) */
    int mb_width;           /* width / 16 */
    int mb_height;          /* height / 16 */
    int total_mbs;          /* mb_width * mb_height */

    /* Initialization flag */
    int initialized;
} StreamResolution;

extern StreamResolution g_stream_res;

/*
 * stream_resolution_init - Compute all derived resolution parameters
 *
 * @width:  Stream width from config (will be clamped to mod-16)
 * @height: Stream height from config (will be clamped to mod-16)
 *
 * Must be called before ffmpeg_pipeline_init() and sw_pipeline_init().
 */
void stream_resolution_init(int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_RESOLUTION_H */
