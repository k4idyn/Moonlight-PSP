/*
 * stream_resolution.c - Unified Stream Resolution Configuration
 *
 * Computes all derived resolution parameters from base width/height.
 * Single source of truth; eliminates hardcoded 480/272/512 scattered
 * across decode, display, and buffer-allocation code.
 */

#include "stream_resolution.h"
#include "diag_log.h"

/* Global resolution table — initialized once per stream session */
StreamResolution g_stream_res = {0};

void stream_resolution_init(int width, int height)
{
    /* Clamp to mod-16 (H.264 macroblock alignment) */
    width  = (width  > 0) ? (width  & ~15) : PSP_LCD_WIDTH;
    height = (height > 0) ? (height & ~15) : PSP_LCD_HEIGHT;

    /* Enforce sane bounds: minimum 128x80, maximum 1920x1088 */
    if (width  < 128)  width  = 128;
    if (height < 80)   height = 80;
    if (width  > 1920) width  = 1920;
    if (height > 1088) height = 1088;

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

    /* Macroblock grid */
    g_stream_res.mb_width  = width / 16;
    g_stream_res.mb_height = height / 16;
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
