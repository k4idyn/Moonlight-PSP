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
 * The active OpenH264 path uses g_stream_res exclusively.
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

/* Active renderer path supports up to PSP LCD max resolution (480x272). */
#define STREAM_MIN_WIDTH     300
#define STREAM_MIN_HEIGHT    170
#define STREAM_MAX_WIDTH     480
#define STREAM_MAX_HEIGHT    272

typedef struct {
    /* Base dimensions (from config) */
    int width;              /* Stream width  (e.g. 300, 360, 480) */
    int height;             /* Stream height (e.g. 170, 204, 272) */

    /* GPU-compatible stride (power-of-2, >= width) */
    int stride;             /* 512 for width<=512, 1024 for width>512 */

    /* Derived buffer sizes */
    int rgba_size;          /* stride * height * 4 (bytes per RGBA buffer) */
    int y_plane_size;       /* width * height */
    int uv_plane_size;      /* (width/2) * (height/2) */
    int yuv_total_size;     /* y + 2*uv */

    /* Macroblock grid (H.264 coded size, rounded up from display size) */
    int mb_width;           /* ceil(width / 16) */
    int mb_height;          /* ceil(height / 16) */
    int total_mbs;          /* mb_width * mb_height */

    /* Initialization flag */
    int initialized;
} StreamResolution;

extern StreamResolution g_stream_res;

/*
 * stream_resolution_init - Compute all derived resolution parameters
 *
 * @width:  Stream width from config
 * @height: Stream height from config
 *
 * Must be called before oh264_pipeline_init() and sw_pipeline_init().
 */
void stream_resolution_init(int width, int height);

/* Clamp and align a resolution to the renderer-safe runtime contract.
 * Stream requests are snapped to the PSP LCD's 30:17 aspect ratio so the
 * renderer can fill 480x272 without black bars or aspect distortion. */
void stream_resolution_normalize(int *inout_width, int *inout_height);

/*============================================================================
 * Phase 4: Smart Resolution Scaling
 *============================================================================*/

/* Resolution steps (all exact 30:17 PSP-panel aspect; no black bars) */
#define RES_STEP_COUNT  4
#define RES_STEP_0_W    300
#define RES_STEP_0_H    170
#define RES_STEP_1_W    360
#define RES_STEP_1_H    204
#define RES_STEP_2_W    420
#define RES_STEP_2_H    238
#define RES_STEP_3_W    480
#define RES_STEP_3_H    272

/* Thresholds for resolution scaling decisions */
#define RES_DECODE_DROP_MS     200  /* avg decode > this -> step down */
#define RES_DECODE_RECOVER_MS  50   /* avg decode < this -> step up */
#define RES_LOSS_DROP_PCT      15   /* sustained loss > this -> step down */
#define RES_LOSS_RECOVER_PCT   2    /* sustained loss < this -> step up */
#define RES_RECOVER_HOLDOFF_US (30 * 1000 * 1000) /* 30s stable before step up */

typedef struct {
    int current_step;       /* 0..RES_STEP_COUNT-1 */
    int target_width;       /* current target width */
    int target_height;      /* current target height */
    u32 last_change_time;   /* timestamp of last resolution change */
    u32 recover_start_time; /* timestamp when recovery conditions first met */
    int decode_avg_ms;      /* running average decode time */
    int loss_avg_pct;       /* running average loss percentage */
    int initialized;
} ResolutionScaler;

extern ResolutionScaler g_res_scaler;

/* Initialize resolution scaler (starts at max step) */
void resolution_scaler_init(void);

/* Update scaler with current metrics. Returns 1 if resolution changed. */
int resolution_scaler_update(int decode_time_ms, int loss_rate_pct);

/* Get current target resolution */
void resolution_scaler_get_current(int *out_w, int *out_h);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_RESOLUTION_H */
