/*
 * openh264_decode.cpp — OpenH264-based H.264 Software Decoder for PSP
 *
 * Native OpenH264 decoder pipeline, purpose-built for low-latency streaming.
 * Exposes the public pipeline API consumed by sw_decoder_thread.c:
 *
 *   oh264_pipeline_init()
 *   oh264_pipeline_shutdown()
 *   oh264_pipeline_decode_frame()
 *   oh264_pipeline_invalidate_refs()
 *   oh264_pipeline_flush_buffers()
 *   oh264_pipeline_abandon()
 *
 * Architecture (dual-core async):
 *   1. Submit Annex-B NAL unit to OpenH264 DecodeFrameNoDelay()
 *   2. Receive decoded YUV420P planes
 *   3. Dispatch YUV420P → RGBA8888 to Media Engine (async, dual-core)
 *   4. Return PREVIOUS frame's RGBA while ME converts current frame
 *
 * Low-latency optimizations (requires our custom OpenH264 PSP port):
 *   - Single-threaded decode (DECODER_OPTION_NUM_OF_THREADS = 0)
 *   - Slice-based output via DecodeFrameNoDelay (immediate per-slice)
 *   - Bounded frame-copy concealment for first-frame/startup continuity
 *   - No internal frame reordering (B-frames disabled at encoder)
 *   - Direct I420 plane access — zero memcpy to ME dispatch
 *
 * OpenH264 supports both CAVLC and CABAC. CABAC detection is telemetry-only;
 * playback quality is handled by the pacing, transport, and decode path.
 */

extern "C" {
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspsdk.h>
#include <psprtc.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include "sw_decode_pipeline.h"
#include "stream_resolution.h"
#include "shared.h"
#include "diag_log.h"
#include "me.h"
#include "settings_menu.h"  /* PspConfig */
#include "decode_flags.h"
#include "runtime_telemetry.h"

extern PspConfig g_psp_config;
extern int control_stream_request_idr(void);
} // extern "C"

/* OpenH264 C++ API header — must be outside extern "C" block */
#include "codec/api/wels/codec_api.h"

/* ============================================================================
 * Module-level state
 * ============================================================================*/

static ISVCDecoder *g_decoder = NULL;

/* RGBA output ring. Use 3 slots for CABAC Performance zero-copy presentation
 * pacing to prevent frame-id gap pacing stalls, otherwise use 2 slots. */
#define OH264_RGBA_BUFFER_COUNT 3
static u8 *g_rgba_buf[OH264_RGBA_BUFFER_COUNT] = {NULL, NULL, NULL};
static int  g_rgba_idx   = 0;
static int  g_rgba_count = 2;
static int  g_rgba_me_clean[OH264_RGBA_BUFFER_COUNT] = {0, 0, 0};
#define OH264_MAX_RGBA_SIZE (FRAME_STRIDE * FRAME_HEIGHT * PIXEL_SIZE)
static u8 g_rgba_static[OH264_RGBA_BUFFER_COUNT][OH264_MAX_RGBA_SIZE] __attribute__((aligned(64)));
#define OH264_MAX_Y_SIZE    (FRAME_WIDTH * FRAME_HEIGHT)
#define OH264_MAX_UV_SIZE   ((FRAME_WIDTH / 2) * (FRAME_HEIGHT / 2))
static u8 g_y_stage[2][OH264_MAX_Y_SIZE] __attribute__((aligned(64)));
static u8 g_u_stage[2][OH264_MAX_UV_SIZE] __attribute__((aligned(64)));
static u8 g_v_stage[2][OH264_MAX_UV_SIZE] __attribute__((aligned(64)));
static int g_yuv_stage_idx = 0;

/* Statistics */
static int g_frames_decoded = 0;
static int g_frames_dropped = 0;

/* Flags read by sw_decoder_thread.c via extern */
int              g_saw_first_idr           = 0;
volatile int     g_idr_fully_decoded       = 0;
volatile int     g_refs_corrupted          = 0;
volatile int     g_current_frame_is_corrupt = 0;
volatile int     g_decode_counters_reset_pending = 0;

extern "C" {
volatile u32     g_oh264_me_wait_us        = 0;
}

/* ============================================================================
 * BT.601 YUV420P → RGBA8888 — optimized low-latency conversion
 *
 * Runs on ME core (dual-core) or main CPU (fallback).
 *
 * Optimization #1: 2×2 block processing — each chroma sample maps to a
 * 2×2 luma block. Computing UV contributions ONCE per block and applying
 * to all 4 pixels eliminates redundant multiplications (saves ~25% ops).
 *
 * Optimization #2: Row-pair processing — adjacent rows share identical
 * UV pointers, so we process two rows per outer iteration, cutting loop
 * overhead in half.
 *
 * Optimization #3: Shift-only clamp — replaces LUT with arithmetic
 * clamp (avoids memory loads from g_clamp_lut[], better on PSP's
 * small L1 cache since it frees cache for the actual pixel data).
 *
 * Note: VFPU-accelerated conversion was tested but produces black output
 * on ME core due to dcache coherence issues with static staging buffers.
 * The integer 2×2 block approach is used instead — it's proven reliable
 * and avoids VFPU↔CPU data marshaling overhead that the legacy code
 * measured at ~31ms (worse than the ~5ms scalar path).
 * ============================================================================*/

/* Arithmetic clamp: no LUT, no memory load, pure ALU.
 * On MIPS Allegrex, SLT+MOVN are 1-cycle each — faster than an
 * L1 miss on the 896-byte LUT when interleaved with pixel data. */
static inline u8 clamp8_fast(int v)
{
    if (v < 0)   return 0;
    if (v > 255) return 255;
    return (u8)v;
}

static inline int graded_luma_term(int y, int row, int col)
{
    static const signed char dither4x4[16] = {
        -3,  1, -2,  2,
         3, -1,  4,  0,
        -2,  2, -3,  1,
         4,  0,  3, -1
    };

    /* Renderer-only grade: PSP LCD plus low-bitrate streams look washed out
     * with the standard 16 luma offset. A slightly deeper black point and a
     * 76/64 luma gain preserve legal-range white without the old 82/64
     * highlight clipping. UI/HUD never pass through this converter. */
    int yy = y - 20 + dither4x4[((row & 3) << 2) | (col & 3)];
    if (yy < 0) yy = 0;
    if (yy > 235) yy = 235;
    return 76 * yy;
}

static inline int plain_luma_term(int y)
{
    int yy = y - 16;
    if (yy < 0) yy = 0;
    return 76 * yy;
}

static inline u32 pack_video_pixel(int c, int rv, int guv, int bu)
{
    return (u32)clamp8_fast((c + rv) >> 6)
         | ((u32)clamp8_fast((c + guv) >> 6) << 8)
         | ((u32)clamp8_fast((c + bu) >> 6) << 16)
         | 0xFF000000u;
}

static inline int decode_state_is_pending_only(DECODING_STATE ds)
{
    return ds == dsErrorFree || ds == dsFramePending;
}

/* 2×2 block YUV420→RGBA with row-pair processing.
 * Processes 4 pixels per inner loop body (2 wide × 2 tall),
 * computing UV contribution once and reusing for all 4 pixels. */
static void yuv420_to_rgba_2x2(const u8 *y_plane, int y_stride,
                                const u8 *u_plane, int u_stride,
                                const u8 *v_plane, int v_stride,
                                u8 *rgba_out, int rgba_stride_pixels,
                                int width, int height,
                                int plain_luma)
{
    const int w2 = width & ~1;
    const int h2 = height & ~1;
    const int rgba_row_bytes = rgba_stride_pixels * 4;

    for (int row = 0; row < h2; row += 2) {
        const u8 *yp0 = y_plane + row * y_stride;
        const u8 *yp1 = yp0 + y_stride;
        const u8 *up  = u_plane + (row >> 1) * u_stride;
        const u8 *vp  = v_plane + (row >> 1) * v_stride;
        u32 *dst0 = (u32 *)(rgba_out + row * rgba_row_bytes);
        u32 *dst1 = (u32 *)((u8 *)dst0 + rgba_row_bytes);

        for (int col = 0; col < w2; col += 2) {
            /* UV contribution — computed ONCE for 4 pixels */
            int uu  = (int)up[col >> 1] - 128;
            int vv  = (int)vp[col >> 1] - 128;
            int rv  =  102 * vv;
            int guv = -25 * uu - 52 * vv;
            int bu  =  129 * uu;

            if (plain_luma) {
                int c = plain_luma_term((int)yp0[col]);
                dst0[col] = pack_video_pixel(c, rv, guv, bu);

                c = plain_luma_term((int)yp0[col + 1]);
                dst0[col + 1] = pack_video_pixel(c, rv, guv, bu);

                c = plain_luma_term((int)yp1[col]);
                dst1[col] = pack_video_pixel(c, rv, guv, bu);

                c = plain_luma_term((int)yp1[col + 1]);
                dst1[col + 1] = pack_video_pixel(c, rv, guv, bu);
            } else {
                int c = graded_luma_term((int)yp0[col], row, col);
                dst0[col] = pack_video_pixel(c, rv, guv, bu);

                c = graded_luma_term((int)yp0[col + 1], row, col + 1);
                dst0[col + 1] = pack_video_pixel(c, rv, guv, bu);

                c = graded_luma_term((int)yp1[col], row + 1, col);
                dst1[col] = pack_video_pixel(c, rv, guv, bu);

                c = graded_luma_term((int)yp1[col + 1], row + 1, col + 1);
                dst1[col + 1] = pack_video_pixel(c, rv, guv, bu);
            }
        }
    }
}

/* ============================================================================
 * ME dual-core dispatch — async YUV→RGBA on Media Engine core
 * ============================================================================*/

typedef struct {
    const u8 *y_plane;
    int y_stride;
    const u8 *u_plane;
    int u_stride;
    const u8 *v_plane;
    int v_stride;
    u8 *rgba_out;
    int rgba_stride_pixels;
    int width;
    int height;
    int plain_luma;
} __attribute__((aligned(64))) MeYuv2RgbaParams;

static volatile struct me_struct *g_me_ctrl        = NULL;
static volatile struct me_struct *g_me_ctrl_cached = NULL;
static MeYuv2RgbaParams          *g_me_params      = NULL;
static struct me_struct           g_me_ctrl_storage __attribute__((aligned(64)));
static MeYuv2RgbaParams           g_me_params_storage __attribute__((aligned(64)));
static int  g_me_available = 0;
static int  g_me_pending   = 0;
static int  g_me_pending_clean = 0;
static int  g_me_pending_staged = 0;
static u8  *g_me_rgba_out  = NULL;
static u8  *g_last_rgba    = NULL;
static u32  g_me_dispatch_us = 0;
static int  g_plain_luma_mode = 0;

/* Zero-delay mode waits for ME conversion before returning a frame. That saves
 * one frame of latency, but serializes CPU decode with ME color conversion.
 * Release playback favors async ME pipelining, keeping zero-delay only for
 * tiny low-FPS streams where latency matters more than overlap. */
static int  g_zero_delay_mode = 0;
#define ZERO_DELAY_PIXEL_THRESHOLD  (256 * 144)
#define ZERO_DELAY_MAX_FPS          15

static int rgba_slot_for_pointer(const void *frame)
{
    uintptr_t frame_addr;

    if (!frame) {
        return -1;
    }

    frame_addr = ((uintptr_t)frame) & 0x1FFFFFFFu;
    for (int i = 0; i < g_rgba_count && i < OH264_RGBA_BUFFER_COUNT; i++) {
        if (g_rgba_buf[i] &&
            ((((uintptr_t)g_rgba_buf[i]) & 0x1FFFFFFFu) == frame_addr)) {
            return i;
        }
    }

    return -1;
}

static void mark_rgba_me_clean(u8 *frame, int clean)
{
    int slot = rgba_slot_for_pointer(frame);

    if (slot >= 0) {
        g_rgba_me_clean[slot] = clean ? 1 : 0;
    }
}

static void clear_rgba_me_clean_tags(void)
{
    for (int i = 0; i < OH264_RGBA_BUFFER_COUNT; i++) {
        g_rgba_me_clean[i] = 0;
    }
}

extern "C" int oh264_frame_is_me_clean(const void *frame)
{
    int slot = rgba_slot_for_pointer(frame);

    return (slot >= 0 && g_rgba_me_clean[slot]) ? 1 : 0;
}

static int me_yuv420_to_rgba_entry(int param)
{
    MeYuv2RgbaParams *p = (MeYuv2RgbaParams *)(unsigned int)param;
    yuv420_to_rgba_2x2(p->y_plane, p->y_stride,
                       p->u_plane, p->u_stride,
                       p->v_plane, p->v_stride,
                       p->rgba_out, p->rgba_stride_pixels,
                       p->width, p->height, p->plain_luma);
    return 0;
}

static int stage_yuv_for_me(unsigned char *pData[3],
                            int y_stride, int uv_stride,
                            int width, int height,
                            const u8 **out_y, const u8 **out_u, const u8 **out_v,
                            int *out_y_stride, int *out_uv_stride)
{
    int idx;
    int uv_w;
    int uv_h;

    if (!pData || !pData[0] || !pData[1] || !pData[2] ||
        width <= 0 || height <= 0 ||
        y_stride <= 0 || uv_stride <= 0 ||
        width > FRAME_WIDTH || height > FRAME_HEIGHT) {
        return 0;
    }

    uv_w = width >> 1;
    uv_h = height >> 1;
    if (uv_w <= 0 || uv_h <= 0 ||
        (width * height) > OH264_MAX_Y_SIZE ||
        (uv_w * uv_h) > OH264_MAX_UV_SIZE) {
        return 0;
    }

    idx = g_yuv_stage_idx;
    for (int row = 0; row < height; row++) {
        memcpy(g_y_stage[idx] + row * width,
               pData[0] + row * y_stride,
               (size_t)width);
    }
    for (int row = 0; row < uv_h; row++) {
        memcpy(g_u_stage[idx] + row * uv_w,
               pData[1] + row * uv_stride,
               (size_t)uv_w);
        memcpy(g_v_stage[idx] + row * uv_w,
               pData[2] + row * uv_stride,
               (size_t)uv_w);
    }

    g_yuv_stage_idx = 1 - g_yuv_stage_idx;
    *out_y = g_y_stage[idx];
    *out_u = g_u_stage[idx];
    *out_v = g_v_stage[idx];
    *out_y_stride = width;
    *out_uv_stride = uv_w;
    return 1;
}

static void collect_pending_me_frame(int block, int *collected_me_frame)
{
    int loops = 0;
    u32 me_wait_start_us;

    if (!g_me_available || !g_me_pending || !g_me_ctrl) {
        return;
    }

    if (!block && !CheckME(g_me_ctrl)) {
        return;
    }

    me_wait_start_us = sceKernelGetSystemTimeLow();
    while (!CheckME(g_me_ctrl) && loops < 5000000) {
        loops++;
        if ((loops & 63) == 0) {
            sceKernelDelayThread(0);
        }
    }
    g_oh264_me_wait_us += sceKernelGetSystemTimeLow() - me_wait_start_us;

    if (loops >= 5000000) {
        if (g_me_dispatch_us != 0) {
            telemetry_accum_me(sceKernelGetSystemTimeLow() - g_me_dispatch_us);
            g_me_dispatch_us = 0;
        }
        diag_log_write("OH264", "ME TIMEOUT -- resetting");
        KillME(g_me_ctrl);
        memset((void *)g_me_ctrl_cached, 0, sizeof(struct me_struct));
        sceKernelDcacheWritebackInvalidateAll();
        if (InitME(g_me_ctrl) == 0) {
            int rl = 0;
            sceKernelDcacheWritebackInvalidateAll();
            while (!g_me_ctrl->done && rl < 2000000) rl++;
            sceKernelDcacheWritebackInvalidateAll();
            if (!g_me_ctrl->done) {
                g_me_available = 0;
            }
        } else {
            g_me_available = 0;
        }
    } else {
        if (g_me_dispatch_us != 0) {
            telemetry_accum_me(sceKernelGetSystemTimeLow() - g_me_dispatch_us);
            g_me_dispatch_us = 0;
        }
        sceKernelDcacheInvalidateRange(g_me_rgba_out, g_stream_res.rgba_size);
        if (g_me_pending_clean) {
            g_last_rgba = g_me_rgba_out;
            mark_rgba_me_clean(g_last_rgba, 1);
            if (collected_me_frame) {
                *collected_me_frame = 1;
            }
        }
    }

    g_me_pending = 0;
    g_me_pending_clean = 0;
    g_me_pending_staged = 0;
}

/* ============================================================================
 * oh264_pipeline_init — Init OpenH264 decoder + ME (low-latency config)
 * ============================================================================*/

extern "C" int oh264_pipeline_init(void)
{
    diag_log_write("OH264", "Initializing OpenH264 decoder (low-latency)...");

    /* clamp LUT removed — using arithmetic clamp (clamp8_fast) */

    /* Create decoder via OpenH264 C API */
    long ret = WelsCreateDecoder(&g_decoder);
    if (ret != 0 || !g_decoder) {
        diag_log_write("OH264", "WelsCreateDecoder failed: %ld", ret);
        return -1;
    }

    /* Resolution must be initialised before buffer allocation */
    if (!g_stream_res.initialized) {
        stream_resolution_init(g_psp_config.width, g_psp_config.height);
    }

    /* Configure decoder: low-latency streaming profile.
     * Since we build OpenH264 from source for PSP (DISABLE_DECODER_MT,
     * single-threaded), these settings maximize decode-to-display speed:
     *   - VIDEO_BITSTREAM_AVC: standard Annex-B NAL input (no container overhead)
     *   - ERROR_CON_DISABLE: avoid spending CPU on concealed slices that still
     *     look blocky on PSP. The caller holds the last clean frame instead.
     *   - bParseOnly=false: full decode, immediate output
     *   - NUM_OF_THREADS=0: explicit single-thread (matches PSP hardware)
     *   - TRACE_LEVEL=0: disable all internal logging
     */
    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.bParseOnly   = false;

    long iret = g_decoder->Initialize(&param);
    if (iret != 0) {
        diag_log_write("OH264", "ISVCDecoder::Initialize failed: %ld", iret);
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
        return -2;
    }

    /* Post-init low-latency tuning via DECODER_OPTION API.
     * These require an initialized decoder instance. */
    {
        /* Single-threaded decode — eliminates thread sync overhead.
         * Our PSP port is compiled with DISABLE_DECODER_MT but setting
         * this explicitly ensures no fallback path activates. */
        int num_threads = 0;
        g_decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS, &num_threads);

        /* Disable internal trace logging — saves ~200µs per frame on PSP
         * by avoiding snprintf + buffer formatting inside the codec. */
        int trace_level = 0;  /* WELS_LOG_QUIET */
        g_decoder->SetOption(DECODER_OPTION_TRACE_LEVEL, &trace_level);
    }
    diag_log_write("OH264", "OpenH264 decoder initialized (threads=0, trace=off, ec=off)");

    /* Fixed static RGBA storage keeps the stream path out of the heap;
     * g_stream_res.rgba_size is validated against the PSP LCD max. */
    if (g_stream_res.rgba_size > OH264_MAX_RGBA_SIZE) {
        diag_log_write("OH264", "RGBA static buffer too small (%d > %d)",
                       g_stream_res.rgba_size, OH264_MAX_RGBA_SIZE);
        g_decoder->Uninitialize();
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
        return -3;
    }
    g_rgba_buf[0] = g_rgba_static[0];
    g_rgba_buf[1] = g_rgba_static[1];
    g_rgba_buf[2] = g_rgba_static[2];
    if (!g_rgba_buf[0] || !g_rgba_buf[1] || !g_rgba_buf[2]) {
        diag_log_write("OH264", "RGBA static buffer unavailable (%d bytes each)",
                       g_stream_res.rgba_size);
        g_decoder->Uninitialize();
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
        return -3;
    }
    for (int i = 0; i < OH264_RGBA_BUFFER_COUNT; i++) {
        memset(g_rgba_buf[i], 0, g_stream_res.rgba_size);
    }
    g_rgba_idx = 0;
    g_rgba_count = 2;
    clear_rgba_me_clean_tags();
    g_yuv_stage_idx = 0;

    /* Release presets pipeline ME work so CPU decode can overlap colorspace.
     * Keep synchronous zero-delay only for tiny low-FPS streams. */
    {
        int pixels = g_stream_res.width * g_stream_res.height;
        int target_fps = (g_psp_config.fps > 0) ? g_psp_config.fps : 20;
        g_zero_delay_mode =
            (target_fps <= ZERO_DELAY_MAX_FPS &&
             pixels <= ZERO_DELAY_PIXEL_THRESHOLD) ? 1 : 0;
        int cabac_perf_video_only =
            (g_psp_config.cabacTestMode &&
             !g_psp_config.audioEnabled &&
             target_fps >= 30 &&
             g_stream_res.width <= 320 &&
             g_stream_res.height <= 180) ? 1 : 0;
        int cabac_quality_audio =
            (g_psp_config.cabacTestMode &&
             g_psp_config.audioEnabled &&
             g_stream_res.width == 480 &&
             g_stream_res.height == 272 &&
             target_fps <= 10) ? 1 : 0;
        g_plain_luma_mode = (cabac_perf_video_only || cabac_quality_audio) ? 1 : 0;
        g_rgba_count = cabac_perf_video_only ? 3 : 2;
        diag_log_write("OH264",
                       "ME pipeline mode: %s (res=%dx%d fps=%d zero_limit=%dpx@%dfps plain_luma=%d rgba_slots=%d)",
                       g_zero_delay_mode ? "zero-delay" : "async",
                       g_stream_res.width, g_stream_res.height, target_fps,
                       ZERO_DELAY_PIXEL_THRESHOLD, ZERO_DELAY_MAX_FPS,
                       g_plain_luma_mode, g_rgba_count);
    }

    /* Initialize Media Engine */
    g_me_available = 0;
    g_me_pending   = 0;
    g_me_pending_clean = 0;
    g_me_pending_staged = 0;
    g_last_rgba    = NULL;
    g_me_rgba_out  = NULL;

    /* Load ME helper PRX if not already resident */
    int me_helper_loaded = 0;
    {
        extern char g_app_dir[512];
        char dynamic_helper_path[512];
        snprintf(dynamic_helper_path, sizeof(dynamic_helper_path), "%s/moonlight_me_helper.prx", g_app_dir);

        const char *me_paths[] = {
            dynamic_helper_path,
            "ms0:/PSP/GAME/Moonlight/moonlight_me_helper.prx",
            "moonlight_me_helper.prx",
            NULL
        };
        SceUID me_prx_id = -1;
        for (int pi = 0; me_paths[pi]; pi++) {
            me_prx_id = sceKernelLoadModule(me_paths[pi], 0, NULL);
            if (me_prx_id >= 0) {
                diag_log_write("OH264", "ME helper loaded from %s", me_paths[pi]);
                me_helper_loaded = 1;
                break;
            }
            if (me_prx_id == (SceUID)0x80020139 ||
                me_prx_id == (SceUID)0x8002032C) {
                me_helper_loaded = 1;
                break;
            }
        }
        if (me_prx_id >= 0) {
            int status = 0;
#ifdef RETAIL_BUILD
            sceKernelStartModule(me_prx_id, 0, NULL, &status, NULL);
#else
            int res = sceKernelStartModule(me_prx_id, 0, NULL, &status, NULL);
            diag_log_write("OH264", "ME helper start=0x%08X", (unsigned)res);
#endif
        } else if (me_prx_id == (SceUID)0x80020139 ||
                   me_prx_id == (SceUID)0x8002032C) {
            diag_log_write("OH264", "ME helper already resident");
        } else {
            diag_log_write("OH264", "ME helper load failed: 0x%08X", (unsigned)me_prx_id);
        }
#ifdef RETAIL_BUILD
        DisableMsLED();
#endif
    }

    g_me_ctrl_cached = (volatile struct me_struct *)&g_me_ctrl_storage;
    g_me_params      = &g_me_params_storage;

    if (me_helper_loaded && g_me_ctrl_cached && g_me_params) {
        g_me_ctrl = (volatile struct me_struct *)((u32)g_me_ctrl_cached | 0x40000000u);
        memset((void *)g_me_ctrl_cached, 0, sizeof(struct me_struct));
        memset(g_me_params, 0, sizeof(MeYuv2RgbaParams));
        sceKernelDcacheWritebackInvalidateAll();

        int me_ret = InitME(g_me_ctrl);
        if (me_ret == 0) {
            sceKernelDcacheWritebackInvalidateAll();
            int loops = 0;
            while (!g_me_ctrl->done && loops < 2000000) {
                loops++;
                if ((loops % 100000) == 0)
                    sceKernelDcacheWritebackInvalidateAll();
            }
            sceKernelDcacheWritebackInvalidateAll();
            if (g_me_ctrl->done) {
                g_me_available = 1;
                diag_log_write("OH264", "ME ready (%d loops) — dual-core active", loops);
            } else {
                diag_log_write("OH264", "ME not ready after %d loops — CPU fallback", loops);
            }
        } else {
            diag_log_write("OH264", "ME InitME failed %d — CPU fallback", me_ret);
        }
    } else {
        diag_log_write("OH264", "ME helper not loaded or alloc failed — CPU fallback");
        g_me_ctrl = NULL;
    }

    g_frames_decoded = 0;
    g_frames_dropped = 0;
    g_saw_first_idr  = 0;
    g_idr_fully_decoded = 0;
    g_refs_corrupted    = 0;

    diag_log_write("OH264", "OpenH264 pipeline ready (RGBA=%dKB each slots=%d) ME=%d ZeroDL=%d",
                   g_stream_res.rgba_size / 1024, g_rgba_count,
                   g_me_available, g_zero_delay_mode);
    return 0;
}

/* ============================================================================
 * oh264_pipeline_shutdown
 * ============================================================================*/

extern "C" void oh264_pipeline_shutdown(void)
{
    diag_log_write("OH264", "Shutting down OpenH264 pipeline (decoded=%d dropped=%d)",
                   g_frames_decoded, g_frames_dropped);

    if (g_me_available && g_me_pending && g_me_ctrl) {
        sceKernelDcacheWritebackInvalidateAll();
        WaitME(g_me_ctrl);
        g_me_pending = 0;
    }
    if (g_me_available && g_me_ctrl) {
        KillME(g_me_ctrl);
        g_me_available = 0;
        diag_log_write("OH264", "Media Engine shut down");
    }
    g_me_ctrl_cached = NULL;
    g_me_ctrl = NULL;
    g_me_params = NULL;
    g_me_pending  = 0;
    g_me_pending_clean = 0;
    g_me_pending_staged = 0;
    g_me_dispatch_us = 0;
    g_last_rgba   = NULL;
    g_me_rgba_out = NULL;
    clear_rgba_me_clean_tags();

    if (g_decoder) {
        g_decoder->Uninitialize();
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
    }

    for (int i = 0; i < OH264_RGBA_BUFFER_COUNT; i++) {
        g_rgba_buf[i] = NULL;
    }
    g_rgba_count = 2;
    clear_rgba_me_clean_tags();

    diag_log_write("OH264", "OpenH264 pipeline shutdown complete");
}

/* ============================================================================
 * oh264_pipeline_abandon — Emergency null-all (watchdog recovery)
 * ============================================================================*/

extern "C" void oh264_pipeline_abandon(void)
{
    diag_log_write("OH264", "ABANDON: resetting old pipeline (decoded=%d dropped=%d)",
                   g_frames_decoded, g_frames_dropped);

    if (g_me_ctrl) { KillME(g_me_ctrl); }

    /* Static RGBA buffers are retained; display thread sees g_last_rgba go
     * NULL below so it won't dereference stale contents during reinit. */
    for (int i = 0; i < OH264_RGBA_BUFFER_COUNT; i++) {
        g_rgba_buf[i] = NULL;
    }
    g_rgba_idx    = 0;
    g_rgba_count  = 2;
    clear_rgba_me_clean_tags();

    /* Decoder thread is terminated before abandon; release decoder resources
     * to prevent watchdog-restart heap growth. */
    if (g_decoder) {
        g_decoder->Uninitialize();
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
    }

    g_me_ctrl_cached = NULL;
    g_me_params = NULL;

    g_me_ctrl        = NULL;
    g_me_available   = 0;
    g_me_pending     = 0;
    g_me_pending_clean = 0;
    g_me_pending_staged = 0;
    g_me_dispatch_us = 0;
    g_last_rgba      = NULL;
    g_me_rgba_out    = NULL;

    g_frames_decoded    = 0;
    g_frames_dropped    = 0;
    g_saw_first_idr     = 0;
    g_idr_fully_decoded = 0;
    g_refs_corrupted    = 1;

    diag_log_write("OH264", "ABANDON: resources released, ready for full reinit");
}

/* ============================================================================
 * oh264_pipeline_flush_buffers — Flush decoder state (post-ring-overrun)
 * ============================================================================*/

extern "C" void oh264_pipeline_flush_buffers(void)
{
    if (!g_decoder) return;
    /* Flush: feed NULL to drain any buffered frames */
    unsigned char *pData[3] = {NULL, NULL, NULL};
    SBufferInfo info;
    memset(&info, 0, sizeof(info));
    g_decoder->DecodeFrameNoDelay(NULL, 0, pData, &info);
    diag_log_write("OH264", "flush_buffers: decoder state flushed");
}

/* ============================================================================
 * oh264_pipeline_invalidate_refs — Force wait for next IDR
 * ============================================================================*/

extern "C" int oh264_pipeline_reset_codec(void)
{
    diag_log_write("OH264", "codec reset: begin");

    if (g_me_available && g_me_pending && g_me_ctrl) {
        int loops = 0;
        while (!CheckME(g_me_ctrl) && loops < 500000) {
            loops++;
            if ((loops & 63) == 0)
                sceKernelDelayThread(0);
        }
        if (loops >= 500000) {
            KillME(g_me_ctrl);
            memset((void *)g_me_ctrl_cached, 0, sizeof(struct me_struct));
            sceKernelDcacheWritebackInvalidateAll();
            if (InitME(g_me_ctrl) != 0) {
                g_me_available = 0;
            }
        }
        g_me_pending = 0;
        g_me_pending_clean = 0;
        g_me_pending_staged = 0;
    }

    if (g_decoder) {
        g_decoder->Uninitialize();
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
    }

    long ret = WelsCreateDecoder(&g_decoder);
    if (ret != 0 || !g_decoder) {
        diag_log_write("OH264", "codec reset: WelsCreateDecoder failed %ld", ret);
        return -1;
    }

    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_DISABLE;
    param.bParseOnly   = false;

    long iret = g_decoder->Initialize(&param);
    if (iret != 0) {
        diag_log_write("OH264", "codec reset: Initialize failed %ld", iret);
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
        return -2;
    }

    int num_threads = 0;
    g_decoder->SetOption(DECODER_OPTION_NUM_OF_THREADS, &num_threads);
    int trace_level = 0;
    g_decoder->SetOption(DECODER_OPTION_TRACE_LEVEL, &trace_level);

    g_last_rgba = NULL;
    g_me_rgba_out = NULL;
    g_me_pending_clean = 0;
    g_me_pending_staged = 0;
    g_saw_first_idr = 0;
    g_idr_fully_decoded = 0;
    g_refs_corrupted = 1;
    g_current_frame_is_corrupt = 0;

    diag_log_write("OH264", "codec reset: complete");
    return 0;
}

extern "C" void oh264_pipeline_invalidate_refs(void)
{
    g_refs_corrupted = 1;
    g_saw_first_idr  = 0;
    diag_log_write("OH264", "refs invalidated (waiting for IDR)");
}

/* ============================================================================
 * oh264_pipeline_decode_frame — Decode one Annex-B access unit (low-latency)
 *
 * Uses DecodeFrameNoDelay() for immediate per-slice output — no internal
 * frame buffering.  Combined with single-threaded decode and disabled
 * trace logging, this is the fastest path through OpenH264 on PSP.
 *
 * Returns 0 + *out_rgba set on success.
 * Returns -5 if waiting for first IDR.
 * Returns negative on decode failure.
 * ============================================================================*/

extern "C" int oh264_pipeline_decode_frame(const u8 *nal_data, int nal_len, u8 **out_rgba)
{
    if (!g_decoder || !nal_data || nal_len <= 0 || !out_rgba) return -1;

    *out_rgba = NULL;
    int collected_me_frame = 0;
    int output_concealed = 0;
    g_oh264_me_wait_us = 0;

    /* Clear per-frame corruption flag (snapshot not needed here) */
    g_current_frame_is_corrupt = 0;

    if (!g_zero_delay_mode && g_me_available && g_me_pending && g_me_ctrl &&
        g_me_pending_staged) {
        collect_pending_me_frame(0, &collected_me_frame);
    }

    /* --- Collect previous ME frame (only in pipelined async mode) --------- */
    if (!g_zero_delay_mode && g_me_available && g_me_pending && g_me_ctrl &&
        !g_me_pending_staged) {
        int loops = 0;
        u32 me_wait_start_us = sceKernelGetSystemTimeLow();
        while (!CheckME(g_me_ctrl) && loops < 5000000) {
            loops++;
            if ((loops & 63) == 0)
                sceKernelDelayThread(0);
        }
        g_oh264_me_wait_us += sceKernelGetSystemTimeLow() - me_wait_start_us;
        if (loops >= 5000000) {
            if (g_me_dispatch_us != 0) {
                telemetry_accum_me(sceKernelGetSystemTimeLow() - g_me_dispatch_us);
                g_me_dispatch_us = 0;
            }
            diag_log_write("OH264", "ME TIMEOUT — resetting");
            KillME(g_me_ctrl);
            memset((void *)g_me_ctrl_cached, 0, sizeof(struct me_struct));
            sceKernelDcacheWritebackInvalidateAll();
            if (InitME(g_me_ctrl) == 0) {
                sceKernelDcacheWritebackInvalidateAll();
                int rl = 0;
                while (!g_me_ctrl->done && rl < 2000000) rl++;
                sceKernelDcacheWritebackInvalidateAll();
                if (!g_me_ctrl->done) g_me_available = 0;
            } else {
                g_me_available = 0;
            }
        } else {
            if (g_me_dispatch_us != 0) {
                telemetry_accum_me(sceKernelGetSystemTimeLow() - g_me_dispatch_us);
                g_me_dispatch_us = 0;
            }
            sceKernelDcacheInvalidateRange(g_me_rgba_out, g_stream_res.rgba_size);
            if (g_me_pending_clean) {
                g_last_rgba = g_me_rgba_out;
                mark_rgba_me_clean(g_last_rgba, 1);
                collected_me_frame = 1;
            }
        }
        g_me_pending = 0;
        g_me_pending_clean = 0;
    }

    /* --- Decode via OpenH264 ---------------------------------------------- */
    unsigned char *pData[3] = {NULL, NULL, NULL};
    SBufferInfo    info;
    memset(&info, 0, sizeof(info));

    /* CRITICAL: Do NOT use DecodeFrameNoDelay() — it calls DecodeFrame2
     * twice (data + NULL flush) and ORs the return codes together. After
     * pipeline reinit the NULL flush can mask a valid IDR decode. We do the
     * two calls manually so first-call output always wins. */
    DECODING_STATE ds1 = g_decoder->DecodeFrame2(
        (const unsigned char *)nal_data, nal_len, pData, &info);

    DECODING_STATE ds;
    if (info.iBufferStatus == 1 && pData[0]) {
        ds = ds1;
    } else {
        unsigned char *pData1[3] = { pData[0], pData[1], pData[2] };
        SBufferInfo    info1;
        memcpy(&info1, &info, sizeof(SBufferInfo));

        unsigned char *pData2[3] = {NULL, NULL, NULL};
        SBufferInfo    info2;
        memset(&info2, 0, sizeof(info2));
        DECODING_STATE ds2 = g_decoder->DecodeFrame2(NULL, 0, pData2, &info2);

        if (info1.iBufferStatus == 1 && pData1[0]) {
            ds = ds1;
            pData[0] = pData1[0]; pData[1] = pData1[1]; pData[2] = pData1[2];
            memcpy(&info, &info1, sizeof(SBufferInfo));
        } else if (info2.iBufferStatus == 1 && pData2[0]) {
            ds = ds2;
            pData[0] = pData2[0]; pData[1] = pData2[1]; pData[2] = pData2[2];
            memcpy(&info, &info2, sizeof(SBufferInfo));
        } else {
            ds = (DECODING_STATE)(ds1 | ds2);
        }
    }

    if (!g_zero_delay_mode && g_me_available && g_me_pending && g_me_ctrl &&
        g_me_pending_staged) {
        collect_pending_me_frame(0, &collected_me_frame);
    }

    /* OpenH264 DECODING_STATE is a bitmask:
     *   dsErrorFree          (0x00)  = clean decode
     *   dsFramePending       (0x01)  = need more data
     *   dsRefLost            (0x02)  = reference frame missing
     *   dsBitstreamError     (0x04)  = broken bitstream
     *   dsDepLayerLost       (0x08)  = dependency layer lost
     *   dsNoParamSets        (0x10)  = no SPS/PPS
     *   dsDataErrorConcealed (0x20)  = error concealed
     *   dsInitialOptExpected (0x2000)= decoder needs initialization
     *   dsInvalidArgument    (0x1000)= bad input
     *
     * Concealed output is not release-safe on PSP. It often appears as
     * macroblocking after Wi-Fi loss, so keep the last clean frame on-screen
     * and force the RTP path into IDR recovery instead of queueing concealed
     * pixels for ME conversion. */

    if (info.iBufferStatus == 1 && pData[0]) {
        static int s_concealed_streak = 0;
        if (ds != dsErrorFree) {
            static int s_concealed_count = 0;
            static u32 s_last_conceal_idr_us = 0;
            s_concealed_count++;
            s_concealed_streak++;
            if (ds == dsDataErrorConcealed) {
                u32 now_us = sceKernelGetSystemTimeLow();
                g_current_frame_is_corrupt = 1;
                g_refs_corrupted = 1;
                g_idr_fully_decoded = 0;
                output_concealed = 1;
                if (s_concealed_count <= 3 || (s_concealed_count % 60) == 0) {
                    diag_log_write("OH264", "DecodeFrame2 fid=%u ds=0x%X len=%d (concealed drop #%d streak=%d)",
                                   (unsigned)g_decode_current_frame_id, (int)ds, nal_len,
                                   s_concealed_count, s_concealed_streak);
                }
                if (now_us - s_last_conceal_idr_us > 1000000) {
                    control_stream_request_idr();
                    s_last_conceal_idr_us = now_us;
                }
                if (collected_me_frame && g_last_rgba) {
                    *out_rgba = g_last_rgba;
                    g_frames_decoded++;
                    return 0;
                }
                return -6;
            } else {
                if (s_concealed_count <= 3 || (s_concealed_count % 60) == 0) {
                    diag_log_write("OH264", "DecodeFrame2 fid=%u ds=0x%X len=%d (corrupt/conceal drop #%d streak=%d)",
                                   (unsigned)g_decode_current_frame_id, (int)ds, nal_len,
                                   s_concealed_count, s_concealed_streak);
                }
                {
                    u32 now_us = sceKernelGetSystemTimeLow();
                    g_frames_dropped++;
                    g_current_frame_is_corrupt = 1;
                    g_refs_corrupted = 1;
                    g_idr_fully_decoded = 0;
                    if (now_us - s_last_conceal_idr_us > 500000) {
                        control_stream_request_idr();
                        s_last_conceal_idr_us = now_us;
                    }
                    if (collected_me_frame && g_last_rgba) {
                        *out_rgba = g_last_rgba;
                        g_frames_decoded++;
                        return 0;
                    }
                    return -6;
                }
            }
        } else {
            s_concealed_streak = 0;
        }
    } else if (!decode_state_is_pending_only(ds)) {
        /* No usable output AND error state — true failure */
        {
            int ref_lost_only =
                ((ds & dsRefLost) != 0) &&
                ((ds & ~(dsRefLost | dsFramePending)) == 0);
            g_frames_dropped++;
            g_current_frame_is_corrupt = 1;
            if (ref_lost_only) {
                if (g_frames_dropped <= 3 || (g_frames_dropped % 60) == 0) {
                    diag_log_write("OH264", "DecodeFrame2 fid=%u returned 0x%X len=%d (ref-lost, no output, dropped=%d)",
                                   (unsigned)g_decode_current_frame_id, (int)ds, nal_len,
                                   g_frames_dropped);
                }
                (void)collected_me_frame;
                return -7;
            }
        }
        g_refs_corrupted = 1;
        g_idr_fully_decoded = 0;
        if (g_frames_dropped <= 3 || (g_frames_dropped % 60) == 0) {
            diag_log_write("OH264", "DecodeFrame2 fid=%u returned 0x%X len=%d (dropped=%d, no output)",
                           (unsigned)g_decode_current_frame_id, (int)ds, nal_len,
                           g_frames_dropped);
        }
        /* Decoder-driven IDR feedback: when the decoder signals it has no
         * SPS/PPS (0x10) or needs reinit (0x2000), request IDR immediately
         * rather than waiting 5s for the watchdog.  This leverages our
         * recompilable port to create a tight encoder/decoder feedback loop. */
        if ((ds & 0x2010) != 0) {
            static u32 s_last_idr_req_us = 0;
            u32 now_us = sceKernelGetSystemTimeLow();
            if (now_us - s_last_idr_req_us > 500000) { /* max 2/sec */
                control_stream_request_idr();
                s_last_idr_req_us = now_us;
            }
        }
        if (collected_me_frame && g_last_rgba) {
            *out_rgba = g_last_rgba;
            g_frames_decoded++;
            return 0;
        }
        return -4;
    } else {
        /* ds == dsErrorFree but no output (frame buffered internally).
         * Only report success if async ME completed a fresh frame at entry.
         * Re-emitting stale RGBA hides frozen playback from telemetry. */
        if (collected_me_frame && g_last_rgba) {
            *out_rgba = g_last_rgba;
            g_frames_decoded++;
            return 0;
        }
        /* No displayable frame was produced for this access unit. */
        return -5;
    }

    /* We have a decoded I420 frame. */
    g_saw_first_idr = 1;
    if (!output_concealed) {
        g_idr_fully_decoded = 1;
        g_refs_corrupted    = 0;
    } else {
        g_current_frame_is_corrupt = 1;
    }

    int src_w      = info.UsrData.sSystemBuffer.iWidth;
    int src_h      = info.UsrData.sSystemBuffer.iHeight;
    int y_stride   = info.UsrData.sSystemBuffer.iStride[0];
    int uv_stride  = info.UsrData.sSystemBuffer.iStride[1];
    const u8 *me_y_plane;
    const u8 *me_u_plane;
    const u8 *me_v_plane;
    int me_y_stride;
    int me_uv_stride;
    int me_uses_staged_yuv = 0;

    /* Clamp to stream_res in case OpenH264 reports slightly different dimensions */
    if (src_w <= 0 || src_h <= 0) {
        src_w = g_stream_res.width;
        src_h = g_stream_res.height;
    }
    if (y_stride <= 0)  y_stride  = src_w;
    if (uv_stride <= 0) uv_stride = src_w / 2;

    me_y_plane = pData[0];
    me_u_plane = pData[1];
    me_v_plane = pData[2];
    me_y_stride = y_stride;
    me_uv_stride = uv_stride;

    /* Choose output RGBA buffer */
    u8 *rgba_out = g_rgba_buf[g_rgba_idx];
    g_rgba_idx   = (g_rgba_idx + 1) % g_rgba_count;
    mark_rgba_me_clean(rgba_out, 0);

    /* --- Dispatch YUV→RGBA to ME (or CPU fallback) ------------------------
     *
     * Two modes, chosen at init based on resolution and target fps:
     *
     * ZERO-DELAY MODE (tiny low-FPS res):
     *   Synchronous ME dispatch waits for ME to finish and returns the current
     *   frame immediately. This is reserved for <=15fps tiny streams.
     *
     * ASYNC PIPELINED MODE (release/stress presets):
     *   Returns the previous frame while ME processes the current frame. This
     *   costs one frame of latency, but keeps CPU and ME overlapped.
     * ----------------------------------------------------------------------- */
    if (g_me_available && g_me_ctrl && g_me_params) {
        if (!g_zero_delay_mode) {
            me_uses_staged_yuv = stage_yuv_for_me(pData, y_stride, uv_stride,
                                                  src_w, src_h,
                                                  &me_y_plane, &me_u_plane, &me_v_plane,
                                                  &me_y_stride, &me_uv_stride);
            if (g_me_pending) {
                collect_pending_me_frame(1, &collected_me_frame);
            }
        }

        g_me_params->y_plane            = me_y_plane;
        g_me_params->y_stride           = me_y_stride;
        g_me_params->u_plane            = me_u_plane;
        g_me_params->u_stride           = me_uv_stride;
        g_me_params->v_plane            = me_v_plane;
        g_me_params->v_stride           = me_uv_stride;
        g_me_params->rgba_out           = rgba_out;
        g_me_params->rgba_stride_pixels = g_stream_res.stride;
        g_me_params->width              = src_w;
        g_me_params->height             = src_h;
        g_me_params->plain_luma         = g_plain_luma_mode;

        /* Targeted dcache flush: only flush the YUV input planes and ME params,
         * NOT the entire cache.  At 256x144, Y=36KB + UV=18KB + params=64B.
         * vs sceKernelDcacheWritebackInvalidateAll() which flushes all 16KB L1
         * + any L2 state — evicting hot decode data and ring buffer metadata. */
        sceKernelDcacheWritebackRange((const void *)me_y_plane,
                                      me_y_stride * src_h);
        sceKernelDcacheWritebackRange((const void *)me_u_plane,
                                      me_uv_stride * (src_h >> 1));
        sceKernelDcacheWritebackRange((const void *)me_v_plane,
                                      me_uv_stride * (src_h >> 1));
        sceKernelDcacheWritebackRange((const void *)g_me_params,
                                      sizeof(MeYuv2RgbaParams));

        BeginME(g_me_ctrl, (int)(unsigned int)me_yuv420_to_rgba_entry,
                (int)(unsigned int)g_me_params,
                -1, NULL, -1, NULL);
        g_me_dispatch_us = sceKernelGetSystemTimeLow();
        g_me_rgba_out = rgba_out;
        g_me_pending  = 1;
        g_me_pending_clean = 1;
        g_me_pending_staged = me_uses_staged_yuv;

        if (g_zero_delay_mode) {
            /* ZERO-DELAY: wait for ME synchronously, return current frame.
             * ME at 222MHz finishes YUV→RGBA for 256x144 in ~3ms.
             * Yield every 4 iterations to keep audio/network responsive.
             * 500K iters ≈ 5ms ceiling — generous for ~3ms actual. */
            int w = 0;
            u32 me_wait_start_us = sceKernelGetSystemTimeLow();
            while (!CheckME(g_me_ctrl) && w < 500000) {
                w++;
                if ((w & 3) == 0)
                    sceKernelDelayThread(0);
            }
            g_oh264_me_wait_us += sceKernelGetSystemTimeLow() - me_wait_start_us;
            if (g_me_dispatch_us != 0) {
                telemetry_accum_me(sceKernelGetSystemTimeLow() - g_me_dispatch_us);
                g_me_dispatch_us = 0;
            }
            sceKernelDcacheInvalidateRange(rgba_out, g_stream_res.rgba_size);
            g_last_rgba  = rgba_out;
            mark_rgba_me_clean(g_last_rgba, 1);
            g_me_pending = 0;
            g_me_pending_clean = 0;
            g_me_pending_staged = 0;
            *out_rgba = rgba_out;
            g_frames_decoded++;
            return 0;
        }

        /* ASYNC PIPELINED: return previous completed frame */
        if (g_last_rgba) {
            *out_rgba = g_last_rgba;
            g_frames_decoded++;
            return 0;
        }
        /* First frame: must wait for ME synchronously */
        int w = 0;
        u32 me_wait_start_us = sceKernelGetSystemTimeLow();
        while (!CheckME(g_me_ctrl) && w < 500000) {
            w++;
            if ((w & 3) == 0)
                sceKernelDelayThread(0);
        }
        g_oh264_me_wait_us += sceKernelGetSystemTimeLow() - me_wait_start_us;
        if (g_me_dispatch_us != 0) {
            telemetry_accum_me(sceKernelGetSystemTimeLow() - g_me_dispatch_us);
            g_me_dispatch_us = 0;
        }
        sceKernelDcacheInvalidateRange(rgba_out, g_stream_res.rgba_size);
        g_last_rgba  = rgba_out;
        mark_rgba_me_clean(g_last_rgba, 1);
        g_me_pending = 0;
        g_me_pending_clean = 0;
        g_me_pending_staged = 0;
        *out_rgba = g_last_rgba;
        g_frames_decoded++;
        return 0;
    }

    /* CPU fallback (no ME) — use optimized 2×2 block converter */
    yuv420_to_rgba_2x2(pData[0], y_stride,
                       pData[1], uv_stride,
                       pData[2], uv_stride,
                       rgba_out, g_stream_res.stride,
                       src_w, src_h, g_plain_luma_mode);

    g_last_rgba = rgba_out;
    mark_rgba_me_clean(g_last_rgba, 0);
    *out_rgba   = rgba_out;
    g_frames_decoded++;
    return 0;
}
