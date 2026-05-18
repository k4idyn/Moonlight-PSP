/*
 * stream_resolution.c - Unified Stream Resolution Configuration
 *
 * Computes all derived resolution parameters from base width/height.
 * Single source of truth; eliminates hardcoded 480/272/512 scattered
 * across decode, display, and buffer-allocation code.
 */

#include "stream_resolution.h"
#include "diag_log.h"
#include <pspkernel.h>
#include <pspdebug.h>

/* GU UI owns the framebuffer during normal runtime; avoid direct debug-screen writes. */
#define pspDebugScreenPrintf(...) ((void)0)

/* Global resolution table — initialized once per stream session */
StreamResolution g_stream_res = {0};

void stream_resolution_normalize(int *inout_width, int *inout_height)
{
    int width = (inout_width && *inout_width > 0) ? *inout_width : PSP_LCD_WIDTH;
    int height = (inout_height && *inout_height > 0) ? *inout_height : PSP_LCD_HEIGHT;
    int scale_from_w;
    int scale_from_h;
    int scale;
    const int scale_min = (STREAM_MIN_WIDTH + 29) / 30;
    const int scale_max = STREAM_MAX_WIDTH / 30;

    /* Snap every request to the PSP LCD's exact 30:17 aspect ratio. The scale
     * value must be even so the resulting YUV420 dimensions are even.
     * This avoids black bars without relying on renderer-side distortion. */
    scale_from_w = (width + 15) / 30;
    scale_from_h = (height + 8) / 17;
    scale = (scale_from_w + scale_from_h + 1) / 2;
    if (scale & 1) {
        scale++;
    }

    if (scale < scale_min) scale = scale_min;
    if (scale & 1) scale++;
    if (scale > scale_max) scale = scale_max & ~1;
    if (scale < 2) scale = 2;

    width = 30 * scale;
    height = 17 * scale;

    if (height > STREAM_MAX_HEIGHT) {
        scale = (STREAM_MAX_HEIGHT / 17) & ~1;
        width = 30 * scale;
        height = 17 * scale;
    }

    if (inout_width) {
        *inout_width = width;
    }
    if (inout_height) {
        *inout_height = height;
    }
}

void stream_resolution_init(int width, int height)
{
    /* Normalize to renderer-safe bounds before deriving allocations/strides. */
    stream_resolution_normalize(&width, &height);

    g_stream_res.width  = width;
    g_stream_res.height = height;

    /* GPU texture stride: must be power-of-2 and >= width */
    g_stream_res.stride = (width > 512) ? 1024 : 512;

    /* Buffer sizes */
    g_stream_res.rgba_size     = g_stream_res.stride * height * 4;
    g_stream_res.y_plane_size  = width * height;
    g_stream_res.uv_plane_size = (width / 2) * (height / 2);
    g_stream_res.yuv_total_size = g_stream_res.y_plane_size
                                + 2 * g_stream_res.uv_plane_size;

    /* H.264 coded macroblock grid. Display dimensions do not need to be
     * mod-16; encoders signal cropping for exact even sizes such as 300x170. */
    g_stream_res.mb_width  = (width + 15) / 16;
    g_stream_res.mb_height = (height + 15) / 16;
    g_stream_res.total_mbs = g_stream_res.mb_width * g_stream_res.mb_height;

    g_stream_res.initialized = 1;

    diag_log_write("RES", "Resolution table: %dx%d stride=%d rgba=%dKB "
                   "yuv=%dKB mbs=%dx%d=%d",
                   width, height, g_stream_res.stride,
                   g_stream_res.rgba_size / 1024,
                   g_stream_res.yuv_total_size / 1024,
                   g_stream_res.mb_width, g_stream_res.mb_height,
                   g_stream_res.total_mbs);
}

/*============================================================================
 * Phase 4: Smart Resolution Scaling
 *============================================================================*/

static const int s_res_steps[RES_STEP_COUNT][2] = {
    { RES_STEP_0_W, RES_STEP_0_H },
    { RES_STEP_1_W, RES_STEP_1_H },
    { RES_STEP_2_W, RES_STEP_2_H },
    { RES_STEP_3_W, RES_STEP_3_H },
};

ResolutionScaler g_res_scaler = {0};

void resolution_scaler_init(void)
{
    g_res_scaler.current_step = RES_STEP_COUNT - 1;
    g_res_scaler.target_width = s_res_steps[RES_STEP_COUNT - 1][0];
    g_res_scaler.target_height = s_res_steps[RES_STEP_COUNT - 1][1];
    g_res_scaler.last_change_time = sceKernelGetSystemTimeLow();
    g_res_scaler.recover_start_time = 0;
    g_res_scaler.decode_avg_ms = 0;
    g_res_scaler.loss_avg_pct = 0;
    g_res_scaler.initialized = 1;

    diag_log_write("RES", "[RES] scaler init: %dx%d step=%d\n",
                   g_res_scaler.target_width, g_res_scaler.target_height,
                   g_res_scaler.current_step);
}

int resolution_scaler_update(int decode_time_ms, int loss_rate_pct)
{
    int old_step;
    u32 now;

    if (!g_res_scaler.initialized)
        return 0;

    now = sceKernelGetSystemTimeLow();
    old_step = g_res_scaler.current_step;

    /* Exponential moving average (fixed-point, alpha=1/4) */
    g_res_scaler.decode_avg_ms = (g_res_scaler.decode_avg_ms * 3 + decode_time_ms) / 4;
    g_res_scaler.loss_avg_pct = (g_res_scaler.loss_avg_pct * 3 + loss_rate_pct) / 4;

    /* Step DOWN: decode too slow or loss too high */
    if ((g_res_scaler.decode_avg_ms > RES_DECODE_DROP_MS ||
         g_res_scaler.loss_avg_pct > RES_LOSS_DROP_PCT) &&
        g_res_scaler.current_step > 0) {
        g_res_scaler.current_step--;
        g_res_scaler.target_width = s_res_steps[g_res_scaler.current_step][0];
        g_res_scaler.target_height = s_res_steps[g_res_scaler.current_step][1];
        g_res_scaler.last_change_time = now;
        g_res_scaler.recover_start_time = 0;

        diag_log_write("RES", "[RES] step DOWN %d->%d: %dx%d (decode=%dms loss=%d%%)\n",
                       old_step, g_res_scaler.current_step,
                       g_res_scaler.target_width, g_res_scaler.target_height,
                       g_res_scaler.decode_avg_ms, g_res_scaler.loss_avg_pct);
        return 1;
    }

    /* Step UP: conditions good for sustained period */
    if (g_res_scaler.decode_avg_ms < RES_DECODE_RECOVER_MS &&
        g_res_scaler.loss_avg_pct < RES_LOSS_RECOVER_PCT &&
        g_res_scaler.current_step < RES_STEP_COUNT - 1) {
        if (g_res_scaler.recover_start_time == 0)
            g_res_scaler.recover_start_time = now;

        if ((now - g_res_scaler.recover_start_time) >= RES_RECOVER_HOLDOFF_US) {
            g_res_scaler.current_step++;
            g_res_scaler.target_width = s_res_steps[g_res_scaler.current_step][0];
            g_res_scaler.target_height = s_res_steps[g_res_scaler.current_step][1];
            g_res_scaler.last_change_time = now;
            g_res_scaler.recover_start_time = 0;

            diag_log_write("RES", "[RES] step UP %d->%d: %dx%d (decode=%dms loss=%d%%)\n",
                           old_step, g_res_scaler.current_step,
                           g_res_scaler.target_width, g_res_scaler.target_height,
                           g_res_scaler.decode_avg_ms, g_res_scaler.loss_avg_pct);
            return 1;
        }
    } else {
        g_res_scaler.recover_start_time = 0;
    }

    return 0;
}

void resolution_scaler_get_current(int *out_w, int *out_h)
{
    if (!g_res_scaler.initialized) {
        if (out_w) *out_w = PSP_LCD_WIDTH;
        if (out_h) *out_h = PSP_LCD_HEIGHT;
        return;
    }
    if (out_w) *out_w = g_res_scaler.target_width;
    if (out_h) *out_h = g_res_scaler.target_height;
}
