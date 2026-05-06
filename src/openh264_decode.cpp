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
 *   - ERROR_CON_SLICE_COPY for fast error concealment without full re-decode
 *   - No internal frame reordering (B-frames disabled at encoder)
 *   - Direct I420 plane access — zero memcpy to ME dispatch
 *
 * OpenH264 supports BOTH CAVLC and CABAC — CABAC detection is now
 * informational only (no hard block needed, but the warning screen
 * still fires from sw_decoder_thread.c's check_nal_for_cabac()).
 */

extern "C" {
#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspsdk.h>
#include <psprtc.h>
#include <string.h>

#include "sw_decode_pipeline.h"
#include "stream_resolution.h"
#include "shared.h"
#include "diag_log.h"
#include "me.h"
#include "settings_menu.h"  /* PspConfig */
#include "decode_flags.h"

extern PspConfig g_psp_config;
extern int control_stream_request_idr(void);
} // extern "C"

/* OpenH264 C++ API header — must be outside extern "C" block */
#include "codec/api/wels/codec_api.h"

/* ============================================================================
 * Module-level state
 * ============================================================================*/

static ISVCDecoder *g_decoder = NULL;

/* RGBA double-buffer — sized from g_stream_res at init */
static u8 *g_rgba_buf[2] = {NULL, NULL};
static int  g_rgba_idx   = 0;
#define OH264_MAX_RGBA_SIZE (FRAME_STRIDE * FRAME_HEIGHT * PIXEL_SIZE)
static u8 g_rgba_static[2][OH264_MAX_RGBA_SIZE] __attribute__((aligned(64)));

/* Statistics */
static int g_frames_decoded = 0;
static int g_frames_dropped = 0;

/* Flags read by sw_decoder_thread.c via extern */
int              g_saw_first_idr           = 0;
volatile int     g_idr_fully_decoded       = 0;
volatile int     g_refs_corrupted          = 0;
volatile int     g_current_frame_is_corrupt = 0;
volatile int     g_decode_counters_reset_pending = 0;

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

/* 2×2 block YUV420→RGBA with row-pair processing.
 * Processes 4 pixels per inner loop body (2 wide × 2 tall),
 * computing UV contribution once and reusing for all 4 pixels. */
static void yuv420_to_rgba_2x2(const u8 *y_plane, int y_stride,
                                const u8 *u_plane, int u_stride,
                                const u8 *v_plane, int v_stride,
                                u8 *rgba_out, int rgba_stride_pixels,
                                int width, int height)
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

            /* Top-left pixel (row, col) */
            int c = 74 * ((int)yp0[col] - 16);
            dst0[col] = (u32)clamp8_fast((c + rv)  >> 6)
                      | ((u32)clamp8_fast((c + guv) >> 6) << 8)
                      | ((u32)clamp8_fast((c + bu)  >> 6) << 16)
                      | 0xFF000000u;

            /* Top-right pixel (row, col+1) */
            c = 74 * ((int)yp0[col + 1] - 16);
            dst0[col + 1] = (u32)clamp8_fast((c + rv)  >> 6)
                          | ((u32)clamp8_fast((c + guv) >> 6) << 8)
                          | ((u32)clamp8_fast((c + bu)  >> 6) << 16)
                          | 0xFF000000u;

            /* Bottom-left pixel (row+1, col) */
            c = 74 * ((int)yp1[col] - 16);
            dst1[col] = (u32)clamp8_fast((c + rv)  >> 6)
                      | ((u32)clamp8_fast((c + guv) >> 6) << 8)
                      | ((u32)clamp8_fast((c + bu)  >> 6) << 16)
                      | 0xFF000000u;

            /* Bottom-right pixel (row+1, col+1) */
            c = 74 * ((int)yp1[col + 1] - 16);
            dst1[col + 1] = (u32)clamp8_fast((c + rv)  >> 6)
                          | ((u32)clamp8_fast((c + guv) >> 6) << 8)
                          | ((u32)clamp8_fast((c + bu)  >> 6) << 16)
                          | 0xFF000000u;
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
} __attribute__((aligned(64))) MeYuv2RgbaParams;

static volatile struct me_struct *g_me_ctrl        = NULL;
static volatile struct me_struct *g_me_ctrl_cached = NULL;
static MeYuv2RgbaParams          *g_me_params      = NULL;
static struct me_struct           g_me_ctrl_storage __attribute__((aligned(64)));
static MeYuv2RgbaParams           g_me_params_storage __attribute__((aligned(64)));
static int  g_me_available = 0;
static int  g_me_pending   = 0;
static u8  *g_me_rgba_out  = NULL;
static u8  *g_last_rgba    = NULL;

/* Zero-pipeline-delay mode: when enabled, ME dispatch is synchronous.
 * Eliminates the 1-frame pipeline latency (16-33ms at 30-60fps) at the
 * cost of serializing decode + colorspace.  For sub-native resolutions
 * (256x144, 368x208) where ME YUV→RGBA takes <5ms, this is a net win:
 *   Async:  decode=11ms + pipeline_latency=33ms = 44ms glass-to-glass
 *   ZeroDL: decode=11ms + ME_wait=3ms            = 14ms glass-to-glass
 * Threshold: width*height <= 480*272 (native PSP LCD) → zero-delay. */
static int  g_zero_delay_mode = 0;
#define ZERO_DELAY_PIXEL_THRESHOLD  (480 * 272)

static int me_yuv420_to_rgba_entry(int param)
{
    MeYuv2RgbaParams *p = (MeYuv2RgbaParams *)(unsigned int)param;
    yuv420_to_rgba_2x2(p->y_plane, p->y_stride,
                       p->u_plane, p->u_stride,
                       p->v_plane, p->v_stride,
                       p->rgba_out, p->rgba_stride_pixels,
                       p->width, p->height);
    return 0;
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

    /* Configure decoder — low-latency streaming profile.
     * Since we build OpenH264 from source for PSP (DISABLE_DECODER_MT,
     * single-threaded), these settings maximize decode-to-display speed:
     *   - VIDEO_BITSTREAM_AVC: standard Annex-B NAL input (no container overhead)
     *   - ERROR_CON_FRAME_COPY_CROSS_IDR: on error, copy entire previous frame
     *     even across IDR boundaries — keeps picture stable on packet loss
     *   - bParseOnly=false: full decode, immediate output
     *   - NUM_OF_THREADS=0: explicit single-thread (matches PSP hardware)
     *   - TRACE_LEVEL=0: disable all internal logging (saves ~200µs/frame)
     */
    SDecodingParam param;
    memset(&param, 0, sizeof(param));
    param.sVideoProperty.eVideoBsType = VIDEO_BITSTREAM_AVC;
    param.eEcActiveIdc = ERROR_CON_FRAME_COPY_CROSS_IDR;
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
    diag_log_write("OH264", "OpenH264 decoder initialized (threads=0, trace=off)");

    /* RGBA double-buffer. Fixed static storage keeps the stream path out of
     * the heap; g_stream_res.rgba_size is validated against the PSP LCD max. */
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
    if (!g_rgba_buf[0] || !g_rgba_buf[1]) {
        diag_log_write("OH264", "RGBA static buffer unavailable (%d bytes each)",
                       g_stream_res.rgba_size);
        g_decoder->Uninitialize();
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
        return -3;
    }
    memset(g_rgba_buf[0], 0, g_stream_res.rgba_size);
    memset(g_rgba_buf[1], 0, g_stream_res.rgba_size);
    g_rgba_idx = 0;

    /* Determine zero-delay mode based on resolution.
     * Sub-native resolutions (256x144, 368x208) benefit from synchronous
     * ME dispatch: ME YUV→RGBA takes ~3ms at these sizes, which is far
     * less than the 16-33ms pipeline latency saved by not double-buffering. */
    g_zero_delay_mode = (g_stream_res.width * g_stream_res.height
                         <= ZERO_DELAY_PIXEL_THRESHOLD) ? 1 : 0;
    diag_log_write("OH264", "Zero-delay mode: %s (res=%dx%d, threshold=%d)",
                   g_zero_delay_mode ? "ON" : "OFF",
                   g_stream_res.width, g_stream_res.height,
                   ZERO_DELAY_PIXEL_THRESHOLD);

    /* Initialize Media Engine */
    g_me_available = 0;
    g_me_pending   = 0;
    g_last_rgba    = NULL;
    g_me_rgba_out  = NULL;

    /* Load ME helper PRX if not already resident */
    {
        static const char *me_paths[] = {
            "ms0:/PSP/GAME/Moonlight/moonlight_me_helper.prx",
            "moonlight_me_helper.prx",
            NULL
        };
        SceUID me_prx_id = -1;
        for (int pi = 0; me_paths[pi]; pi++) {
            me_prx_id = sceKernelLoadModule(me_paths[pi], 0, NULL);
            if (me_prx_id >= 0) {
                diag_log_write("OH264", "ME helper loaded from %s", me_paths[pi]);
                break;
            }
            if (me_prx_id == (SceUID)0x80020139 ||
                me_prx_id == (SceUID)0x8002032C) break;
        }
        if (me_prx_id >= 0) {
            int status = 0;
            int res = sceKernelStartModule(me_prx_id, 0, NULL, &status, NULL);
            diag_log_write("OH264", "ME helper start=0x%08X", (unsigned)res);
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

    if (g_me_ctrl_cached && g_me_params) {
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
        diag_log_write("OH264", "ME alloc failed — CPU fallback");
        g_me_ctrl = NULL;
    }

    g_frames_decoded = 0;
    g_frames_dropped = 0;
    g_saw_first_idr  = 0;
    g_idr_fully_decoded = 0;
    g_refs_corrupted    = 0;

    diag_log_write("OH264", "OpenH264 pipeline ready (RGBA=%dKB each) ME=%d ZeroDL=%d",
                   g_stream_res.rgba_size / 1024, g_me_available, g_zero_delay_mode);
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
    g_last_rgba   = NULL;
    g_me_rgba_out = NULL;

    if (g_decoder) {
        g_decoder->Uninitialize();
        WelsDestroyDecoder(g_decoder);
        g_decoder = NULL;
    }

    for (int i = 0; i < 2; i++) {
        g_rgba_buf[i] = NULL;
    }
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
    g_rgba_buf[0] = NULL;
    g_rgba_buf[1] = NULL;
    g_rgba_idx    = 0;

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
    param.eEcActiveIdc = ERROR_CON_FRAME_COPY_CROSS_IDR;
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

    /* Clear per-frame corruption flag (snapshot not needed here) */
    g_current_frame_is_corrupt = 0;

    /* --- Collect previous ME frame (only in pipelined async mode) --------- */
    if (!g_zero_delay_mode && g_me_available && g_me_pending && g_me_ctrl) {
        int loops = 0;
        while (!CheckME(g_me_ctrl) && loops < 5000000) {
            loops++;
            if ((loops & 63) == 0)
                sceKernelDelayThread(0);
        }
        if (loops >= 5000000) {
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
            sceKernelDcacheInvalidateRange(g_me_rgba_out, g_stream_res.rgba_size);
            g_last_rgba = g_me_rgba_out;
            collected_me_frame = 1;
        }
        g_me_pending = 0;
    }

    /* --- Decode via OpenH264 ---------------------------------------------- */
    unsigned char *pData[3] = {NULL, NULL, NULL};
    SBufferInfo    info;
    memset(&info, 0, sizeof(info));

    /* CRITICAL: Do NOT use DecodeFrameNoDelay() — it calls DecodeFrame2
     * twice (data + NULL flush) and ORs the return codes together.  After
     * a pipeline reinit the NULL flush returns dsInitialOptExpected (0x2000)
     * which masks the valid IDR decode from the first call, permanently
     * stalling video recovery.
     *
     * Instead, call DecodeFrame2 twice manually and preserve the first
     * call's output if it produced a valid frame. */
    DECODING_STATE ds1 = g_decoder->DecodeFrame2(
        (const unsigned char *)nal_data, nal_len, pData, &info);

    /* Fast path: if first call produced a frame, skip the NULL flush entirely.
     * This saves ~3-8% CPU on clean streams where every frame decodes on
     * the first call. The flush is only needed to drain buffered frames
     * from OpenH264's internal pipeline (rare at low latency). */
    DECODING_STATE ds;
    if (info.iBufferStatus == 1 && pData[0]) {
        ds = ds1;
    } else {
        /* Save first call's results before the flush call may overwrite them */
        unsigned char *pData1[3] = { pData[0], pData[1], pData[2] };
        SBufferInfo    info1;
        memcpy(&info1, &info, sizeof(SBufferInfo));

        /* Flush call: feed NULL to drain any internally buffered frame */
        unsigned char *pData2[3] = {NULL, NULL, NULL};
        SBufferInfo    info2;
        memset(&info2, 0, sizeof(info2));
        DECODING_STATE ds2 = g_decoder->DecodeFrame2(NULL, 0, pData2, &info2);

        /* Prefer the first call's output if it produced a frame.
         * Fall back to the flush call's output if the first had nothing. */
        if (info1.iBufferStatus == 1 && pData1[0]) {
            ds = ds1;
            pData[0] = pData1[0]; pData[1] = pData1[1]; pData[2] = pData1[2];
            memcpy(&info, &info1, sizeof(SBufferInfo));
        } else if (info2.iBufferStatus == 1 && pData2[0]) {
            ds = ds2;
            pData[0] = pData2[0]; pData[1] = pData2[1]; pData[2] = pData2[2];
            memcpy(&info, &info2, sizeof(SBufferInfo));
        } else {
            /* Neither call produced output — merge error codes */
            ds = (DECODING_STATE)(ds1 | ds2);
        }
    }

    /* OpenH264 DECODING_STATE is a bitmask:
     *   dsErrorFree          (0x00)  = clean decode
     *   dsFramePending       (0x01)  = need more data
     *   dsRefLost            (0x02)  = reference frame missing
     *   dsBitstreamError     (0x04)  = broken bitstream
     *   dsDepLayerLost       (0x08)  = dependency layer lost
     *   dsNoParamSets        (0x10)  = no SPS/PPS
     *   dsDataErrorConcealed (0x20)  = error concealed, output MAY be valid
     *   dsInitialOptExpected (0x2000)= decoder needs initialization
     *   dsInvalidArgument    (0x1000)= bad input
     *
     * CRITICAL: dsDataErrorConcealed (0x20) means OpenH264 substituted
     * missing data via ERROR_CON_SLICE_COPY — the frame IS decoded and
     * output may be available in pData with iBufferStatus==1.
     *
     * Fix: check iBufferStatus==1 for usable output regardless of ds. */

    if (info.iBufferStatus == 1 && pData[0]) {
        /* Frame produced — use it even if ds has error bits set.
         * dsDataErrorConcealed frames have minor visual artifacts
         * (macroblocking) but are far better than stale frames. */
        if (ds != dsErrorFree) {
            static int s_concealed_count = 0;
            s_concealed_count++;
            if (s_concealed_count <= 3 || (s_concealed_count % 120) == 0) {
                diag_log_write("OH264", "DecodeFrame2 ds=0x%X (concealed #%d, using output)",
                               (int)ds, s_concealed_count);
            }
        }
    } else if (ds != dsErrorFree) {
        /* No usable output AND error state — true failure */
        g_frames_dropped++;
        if (g_frames_dropped <= 3 || (g_frames_dropped % 60) == 0) {
            diag_log_write("OH264", "DecodeFrame2 returned 0x%X (dropped=%d, no output)",
                           (int)ds, g_frames_dropped);
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
        /* No frame yet — still waiting for first IDR */
        return -5;
    }

    /* We have a valid decoded I420 frame (possibly with error concealment) */
    g_saw_first_idr = 1;
    if (ds == dsErrorFree) {
        /* Clean decode — fully trust this frame */
        g_idr_fully_decoded = 1;
        g_refs_corrupted    = 0;
    } else {
        /* Concealed frame — still usable but refs may be imperfect.
         * Set g_idr_fully_decoded so the dedup/lgf logic works normally.
         * Don't clear g_refs_corrupted since concealment means data was lost. */
        g_idr_fully_decoded = 1;
    }

    int src_w      = info.UsrData.sSystemBuffer.iWidth;
    int src_h      = info.UsrData.sSystemBuffer.iHeight;
    int y_stride   = info.UsrData.sSystemBuffer.iStride[0];
    int uv_stride  = info.UsrData.sSystemBuffer.iStride[1];

    /* Clamp to stream_res in case OpenH264 reports slightly different dimensions */
    if (src_w <= 0 || src_h <= 0) {
        src_w = g_stream_res.width;
        src_h = g_stream_res.height;
    }
    if (y_stride <= 0)  y_stride  = src_w;
    if (uv_stride <= 0) uv_stride = src_w / 2;

    /* Choose output RGBA buffer */
    u8 *rgba_out = g_rgba_buf[g_rgba_idx];
    g_rgba_idx   = 1 - g_rgba_idx;

    /* --- Dispatch YUV→RGBA to ME (or CPU fallback) ------------------------
     *
     * Two modes, chosen at init based on resolution:
     *
     * ZERO-DELAY MODE (sub-native res):
     *   Synchronous ME dispatch — waits for ME to finish, returns CURRENT
     *   frame immediately.  Eliminates 1-frame pipeline latency (16-33ms).
     *   At 256x144 (36K pixels), ME takes ~3ms — far less than a frame.
     *
     * ASYNC PIPELINED MODE (native/super-native res):
     *   Returns PREVIOUS frame while ME processes current frame async.
     *   Higher throughput but adds 1-frame pipeline latency.
     * ----------------------------------------------------------------------- */
    if (g_me_available && g_me_ctrl && g_me_params) {
        g_me_params->y_plane            = pData[0];
        g_me_params->y_stride           = y_stride;
        g_me_params->u_plane            = pData[1];
        g_me_params->u_stride           = uv_stride;
        g_me_params->v_plane            = pData[2];
        g_me_params->v_stride           = uv_stride;
        g_me_params->rgba_out           = rgba_out;
        g_me_params->rgba_stride_pixels = g_stream_res.stride;
        g_me_params->width              = src_w;
        g_me_params->height             = src_h;

        /* Targeted dcache flush: only flush the YUV input planes and ME params,
         * NOT the entire cache.  At 256x144, Y=36KB + UV=18KB + params=64B.
         * vs sceKernelDcacheWritebackInvalidateAll() which flushes all 16KB L1
         * + any L2 state — evicting hot decode data and ring buffer metadata. */
        sceKernelDcacheWritebackRange((const void *)pData[0],
                                      y_stride * src_h);
        sceKernelDcacheWritebackRange((const void *)pData[1],
                                      uv_stride * (src_h >> 1));
        sceKernelDcacheWritebackRange((const void *)pData[2],
                                      uv_stride * (src_h >> 1));
        sceKernelDcacheWritebackRange((const void *)g_me_params,
                                      sizeof(MeYuv2RgbaParams));

        BeginME(g_me_ctrl, (int)(unsigned int)me_yuv420_to_rgba_entry,
                (int)(unsigned int)g_me_params,
                -1, NULL, -1, NULL);
        g_me_rgba_out = rgba_out;
        g_me_pending  = 1;

        if (g_zero_delay_mode) {
            /* ZERO-DELAY: wait for ME synchronously, return current frame.
             * ME at 222MHz finishes YUV→RGBA for 256x144 in ~3ms.
             * Yield every 4 iterations to keep audio/network responsive.
             * 500K iters ≈ 5ms ceiling — generous for ~3ms actual. */
            int w = 0;
            while (!CheckME(g_me_ctrl) && w < 500000) {
                w++;
                if ((w & 3) == 0)
                    sceKernelDelayThread(0);
            }
            sceKernelDcacheInvalidateRange(rgba_out, g_stream_res.rgba_size);
            g_last_rgba  = rgba_out;
            g_me_pending = 0;
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
        while (!CheckME(g_me_ctrl) && w < 500000) {
            w++;
            if ((w & 3) == 0)
                sceKernelDelayThread(0);
        }
        sceKernelDcacheInvalidateRange(rgba_out, g_stream_res.rgba_size);
        g_last_rgba  = rgba_out;
        g_me_pending = 0;
        *out_rgba = g_last_rgba;
        g_frames_decoded++;
        return 0;
    }

    /* CPU fallback (no ME) — use optimized 2×2 block converter */
    yuv420_to_rgba_2x2(pData[0], y_stride,
                       pData[1], uv_stride,
                       pData[2], uv_stride,
                       rgba_out, g_stream_res.stride,
                       src_w, src_h);

    g_last_rgba = rgba_out;
    *out_rgba   = rgba_out;
    g_frames_decoded++;
    return 0;
}
