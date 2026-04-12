/*
 * sw_decode_orchestrator.c - Software Decode Pipeline Orchestrator
 *
 * Ties together all stages of the asymmetric dual-core pipeline:
 *
 *   1. Receives raw Annex-B H.264 NAL data from RTP reassembly
 *   2. Runs CAVLC entropy decode on Main CPU  → SwMacroblockData[]
 *   3. Flushes dcache  → physical RAM coherent
 *   4. Dispatches VFPU reconstruction to Media Engine
 *   5. ME: dequant → IDCT → prediction → motcomp → YUV→RGBA
 *   6. Waits for ME completion, invalidates dcache
 *   7. Returns RGBA8888 frame pointer
 *
 * Memory Budget (PSP-1000, 32MB total):
 *   - SwPipelineState (mbs[510]):  ~510 * 900 ≈ 450KB
 *   - Reference frames (2 × YUV):  2 × 163KB = 326KB
 *   - Current frame YUV planes:    163KB
 *   - RGBA output double-buffer:   2 × 522KB = 1044KB
 *   Total: ~2MB  (vs existing sceMpeg: ~1.5MB for ringbuffers)
 *
 * This module owns all allocation and is the single entry point
 * for the rest of the codebase to use software decode.
 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <psprtc.h>
#include <string.h>
#include <malloc.h>

#include "sw_decode_pipeline.h"
#include "shared.h"
#include "diag_log.h"
#include "me.h"

/* ============================================================================
 * External APIs from pipeline components
 * ============================================================================*/

/* From sw_cavlc.c */
extern int cavlc_decode_frame(const u8 *annexb, int annexb_len,
                               SwPipelineState *state);

/* From sw_me_worker.c */
extern int sw_me_worker_init(void);
extern void sw_me_worker_shutdown(void);
extern int sw_me_worker_dispatch(SwPipelineState *state, u8 *rgba_output);
extern int sw_me_worker_wait(void);
extern int sw_me_worker_cpu_fallback(SwPipelineState *state, u8 *rgba_output);
extern int sw_me_worker_is_available(void);

/* Track whether ME dispatch is active (set after first successful init) */
static int g_use_me = 0;

/* ============================================================================
 * Pipeline State — All 64-byte aligned for ME DMA
 * ============================================================================*/

static SwPipelineState *g_pipeline = NULL;

/* YUV plane buffers for current frame */
static u8 *g_y_plane = NULL;
static u8 *g_u_plane = NULL;
static u8 *g_v_plane = NULL;

/* Reference frame YUV buffers (2 references for Baseline) */
static u8 *g_ref_y[SW_MAX_REFS] = {NULL, NULL};
static u8 *g_ref_u[SW_MAX_REFS] = {NULL, NULL};
static u8 *g_ref_v[SW_MAX_REFS] = {NULL, NULL};

/* RGBA output double-buffer (so display can read while we decode next) */
static u8 *g_rgba_buf[2] = {NULL, NULL};
static int g_rgba_idx = 0;

/* Track whether we've received SPS/PPS */
static int g_got_sps = 0;
static int g_got_pps = 0;

/* Performance tracking done via sceRtcGetCurrentTick in decode function */

/* ============================================================================
 * YUV Plane Sizes
 * ============================================================================*/

#define Y_PLANE_SIZE    (SW_FRAME_WIDTH * SW_FRAME_HEIGHT)         /* 130,560 */
#define UV_PLANE_SIZE   ((SW_FRAME_WIDTH/2) * (SW_FRAME_HEIGHT/2)) /*  32,640 */
#define YUV_TOTAL_SIZE  (Y_PLANE_SIZE + 2 * UV_PLANE_SIZE)         /* 195,840 */
#define RGBA_SIZE       (SW_FRAME_STRIDE * SW_FRAME_HEIGHT * 4)    /* 557,056 — 512-pixel stride for GU */

/* ============================================================================
 * Allocation Helpers — All memalign(64, ...) for ME coherence
 * ============================================================================*/

static u8 *alloc_aligned(int size, const char *label)
{
    u8 *p = (u8 *)memalign(64, size);
    if (!p) {
        diag_log_write("SW_ORCH",
                 "Failed to allocate %d bytes for %s", size, label);
    } else {
        memset(p, 0, size);
    }
    return p;
}

/* ============================================================================
 * sw_pipeline_init — Top-level initialization
 * ============================================================================*/

int sw_pipeline_init(void)
{
    if (g_pipeline) {
        diag_log_write("SW_ORCH",
                 "Pipeline already initialized");
        return 0;
    }

    diag_log_write("SW_ORCH",
             "Initializing software decode pipeline...");

    /* 1. Allocate pipeline state struct */
    g_pipeline = (SwPipelineState *)memalign(64, sizeof(SwPipelineState));
    if (!g_pipeline) {
        diag_log_write("SW_ORCH",
                 "Failed to allocate pipeline state (%d bytes)",
                 (int)sizeof(SwPipelineState));
        return -1;
    }
    memset(g_pipeline, 0, sizeof(SwPipelineState));

    /* 2. Allocate current frame YUV planes */
    g_y_plane = alloc_aligned(Y_PLANE_SIZE, "Y plane");
    g_u_plane = alloc_aligned(UV_PLANE_SIZE, "U plane");
    g_v_plane = alloc_aligned(UV_PLANE_SIZE, "V plane");
    if (!g_y_plane || !g_u_plane || !g_v_plane) {
        sw_pipeline_shutdown();
        return -2;
    }

    g_pipeline->current.y_plane = g_y_plane;
    g_pipeline->current.u_plane = g_u_plane;
    g_pipeline->current.v_plane = g_v_plane;

    /* 3. Allocate reference frame buffers */
    for (int i = 0; i < SW_MAX_REFS; i++) {
        g_ref_y[i] = alloc_aligned(Y_PLANE_SIZE, "ref Y");
        g_ref_u[i] = alloc_aligned(UV_PLANE_SIZE, "ref U");
        g_ref_v[i] = alloc_aligned(UV_PLANE_SIZE, "ref V");
        if (!g_ref_y[i] || !g_ref_u[i] || !g_ref_v[i]) {
            sw_pipeline_shutdown();
            return -3;
        }
        g_pipeline->ref_frames[i].y_plane = g_ref_y[i];
        g_pipeline->ref_frames[i].u_plane = g_ref_u[i];
        g_pipeline->ref_frames[i].v_plane = g_ref_v[i];
        g_pipeline->ref_frames[i].used = 0;
    }
    g_pipeline->active_ref = 0;

    /* Pre-fill ref 0 with neutral gray so P-frames can decode even if the
     * initial IDR is lost.  Y=128 Cb=128 Cr=128 = mid-gray in BT.601.
     * Error propagation from a wrong reference converges quickly — P-frames
     * carry enough DCT coefficients to correct within ~5 frames. */
    memset(g_ref_y[0], 128, Y_PLANE_SIZE);
    memset(g_ref_u[0], 128, UV_PLANE_SIZE);
    memset(g_ref_v[0], 128, UV_PLANE_SIZE);
    g_pipeline->ref_frames[0].used = 1;
    g_pipeline->ref_frames[0].frame_num = 0;

    /* 4. Allocate RGBA output double-buffer */
    g_rgba_buf[0] = alloc_aligned(RGBA_SIZE, "RGBA buf 0");
    g_rgba_buf[1] = alloc_aligned(RGBA_SIZE, "RGBA buf 1");
    if (!g_rgba_buf[0] || !g_rgba_buf[1]) {
        sw_pipeline_shutdown();
        return -4;
    }
    g_rgba_idx = 0;

    /* 5. Initialize ME worker */
    int ret = sw_me_worker_init();
    if (ret < 0) {
        diag_log_write("SW_ORCH",
                 "ME init failed (%d), will use CPU fallback", ret);
        g_use_me = 0;
    } else {
        g_use_me = sw_me_worker_is_available();
        diag_log_write("SW_ORCH",
                 "ME dispatch %s", g_use_me ? "ENABLED (dual-core)" : "DISABLED (CPU only)");
    }

    /* TEMPORARY: Force CPU-only mode.  The ME crashes on first dispatch
     * (timeout after 2s) because user-mode pointers in SwPipelineState
     * are not accessible from the MediaEngine core.  Until proper
     * uncached/physical memory allocation is implemented, skip ME
     * entirely to avoid wasting 2 seconds per session. */
    if (g_use_me) {
        diag_log_write("SW_ORCH", "ME FORCE-DISABLED: CPU-only mode (ME memory sharing TBD)");
        g_use_me = 0;
    }

    /* 6. Mark pipeline active */
    g_pipeline->pipeline_active = 1;
    g_pipeline->frame_ready = 0;
    g_pipeline->me_done = 1;

    g_got_sps = 0;
    g_got_pps = 0;

    diag_log_write("SW_ORCH",
             "Software decode pipeline ready "
             "(state=%d bytes, %d MB total YUV)",
             (int)sizeof(SwPipelineState),
             (int)(sizeof(SwPipelineState) + YUV_TOTAL_SIZE * 3 + RGBA_SIZE * 2) / (1024*1024));

    return 0;
}

/* ============================================================================
 * sw_pipeline_shutdown — Clean teardown
 * ============================================================================*/

void sw_pipeline_shutdown(void)
{
    diag_log_write("SW_ORCH",
             "Shutting down software decode pipeline...");

    if (g_pipeline) {
        g_pipeline->pipeline_active = 0;
    }

    sw_me_worker_shutdown();

    /* Free RGBA buffers */
    for (int i = 0; i < 2; i++) {
        if (g_rgba_buf[i]) { free(g_rgba_buf[i]); g_rgba_buf[i] = NULL; }
    }

    /* Free reference frames */
    for (int i = 0; i < SW_MAX_REFS; i++) {
        if (g_ref_y[i]) { free(g_ref_y[i]); g_ref_y[i] = NULL; }
        if (g_ref_u[i]) { free(g_ref_u[i]); g_ref_u[i] = NULL; }
        if (g_ref_v[i]) { free(g_ref_v[i]); g_ref_v[i] = NULL; }
    }

    /* Free current frame planes */
    if (g_y_plane) { free(g_y_plane); g_y_plane = NULL; }
    if (g_u_plane) { free(g_u_plane); g_u_plane = NULL; }
    if (g_v_plane) { free(g_v_plane); g_v_plane = NULL; }

    /* Free pipeline state */
    if (g_pipeline) { free(g_pipeline); g_pipeline = NULL; }

    g_got_sps = 0;
    g_got_pps = 0;

    diag_log_write("SW_ORCH",
             "Pipeline shutdown complete");
}

/* ============================================================================
 * Reference Frame Management — Swap current → reference after decode
 * ============================================================================*/

static void swap_ref_frames(void)
{
    /* Rotate: current decoded frame becomes the new reference.
     * Pointer swap instead of 191KB memcpy — all planes are
     * separately allocated with identical size/alignment. */
    int next_ref = (g_pipeline->active_ref + 1) % SW_MAX_REFS;
    u8 *tmp;

    tmp = g_pipeline->ref_frames[next_ref].y_plane;
    g_pipeline->ref_frames[next_ref].y_plane = g_pipeline->current.y_plane;
    g_pipeline->current.y_plane = tmp;

    tmp = g_pipeline->ref_frames[next_ref].u_plane;
    g_pipeline->ref_frames[next_ref].u_plane = g_pipeline->current.u_plane;
    g_pipeline->current.u_plane = tmp;

    tmp = g_pipeline->ref_frames[next_ref].v_plane;
    g_pipeline->ref_frames[next_ref].v_plane = g_pipeline->current.v_plane;
    g_pipeline->current.v_plane = tmp;

    g_pipeline->ref_frames[next_ref].used = 1;
    g_pipeline->ref_frames[next_ref].frame_num = g_pipeline->slice.frame_num;
    g_pipeline->active_ref = next_ref;
}

/* ============================================================================
 * sw_pipeline_invalidate_refs — Mark all reference frames as invalid
 *
 * Called after a queue flush to force the decoder to wait for a fresh IDR
 * before accepting P-frames.  Without this, P-frames decoded after a gap
 * use stale reference data and produce extremely blocky/corrupt output.
 * ============================================================================*/

void sw_pipeline_invalidate_refs(void)
{
    if (!g_pipeline) return;
    for (int i = 0; i < SW_MAX_REFS; i++) {
        g_pipeline->ref_frames[i].used = 0;
    }
    diag_log_write("SW_ORCH", "Reference frames invalidated (awaiting IDR)");
}

/* ============================================================================
 * sw_pipeline_decode_frame — Main decode entry point
 *
 * Called from the decoder thread when a complete access unit arrives.
 * Returns pointer to RGBA8888 pixel data on success.
 * ============================================================================*/

int sw_pipeline_decode_frame(const u8 *nal_data, int nal_len, u8 **out_rgba)
{
    static int s_frame_seq = 0;
    s_frame_seq++;

    if (!g_pipeline || !g_pipeline->pipeline_active) {
        return -1;
    }
    if (!nal_data || nal_len <= 0 || nal_len > SW_MAX_BITSTREAM) {
        return -2;
    }
    if (!out_rgba) {
        return -3;
    }

    *out_rgba = NULL;
    u64 t_start, t_cavlc, t_recon;
    sceRtcGetCurrentTick(&t_start);

    diag_log_write("SW_ORCH", "DECODE#%d len=%d entry\n", s_frame_seq, nal_len);

    /* ---- Stage 1: CAVLC Entropy Decode (Main CPU) ---- */
    g_pipeline->frame_ready = 0;
    g_pipeline->me_done = 0;

    int ret = cavlc_decode_frame(nal_data, nal_len, g_pipeline);

    diag_log_write("SW_ORCH", "DECODE#%d cavlc=%d mb=%d/%d slice=%s\n",
                   s_frame_seq, ret,
                   g_pipeline->mb_count, g_pipeline->total_mbs_expected,
                   g_pipeline->slice.slice_type == SW_SLICE_P ? "P" :
                   g_pipeline->slice.slice_type == SW_SLICE_I ? "I" : "?");

    if (ret < 0) {
        /* If CAVLC failed and we haven't received SPS/PPS yet, this is
         * expected for P-frames arriving before the first IDR.  Return -5
         * so the decoder thread uses structured IDR retry logic instead
         * of the generic -4 drop handler. */
        if (!g_got_sps || !g_got_pps) {
            static int s_no_sps_log_count = 0;
            if (s_no_sps_log_count < 3) {
                diag_log_write("SW_ORCH", "No SPS/PPS yet — waiting for IDR");
                s_no_sps_log_count++;
            }
            return -5;
        }
        if (ret == -2) {
            diag_log_write("SW_ORCH",
                     "CAVLC partial frame: %d/%d MBs (dropping)",
                     g_pipeline->mb_count, g_pipeline->total_mbs_expected);
        } else {
            diag_log_write("SW_ORCH",
                     "CAVLC decode failed: %d", ret);
        }
        g_pipeline->frames_dropped++;
        return -4;
    }

    /* Validate mb_count — partial frames are now accepted thanks to
     * error concealment in sw_cavlc.c filling undecoded MBs with defaults.
     * Only warn; don't drop. A partial IDR is infinitely better than
     * no reference at all (which blocks ALL P-frames). */
    if (g_pipeline->mb_count < g_pipeline->total_mbs_expected) {
        diag_log_write("SW_ORCH",
                 "Short frame: %d/%d MBs (concealment filled rest)",
                 g_pipeline->mb_count, g_pipeline->total_mbs_expected);
    }

    /* Track SPS/PPS reception */
    if (g_pipeline->sps.valid) g_got_sps = 1;
    if (g_pipeline->pps.valid) g_got_pps = 1;

    /* Need both SPS and PPS before we can reconstruct */
    if (!g_got_sps || !g_got_pps) {
        diag_log_write("SW_ORCH",
                 "Waiting for SPS/PPS (sps=%d pps=%d)",
                 g_got_sps, g_got_pps);
        return -5;
    }

    /* P-frame safety: refuse to reconstruct if no valid reference exists. */
    if (g_pipeline->slice.slice_type == SW_SLICE_P &&
        !g_pipeline->ref_frames[g_pipeline->active_ref].used) {
        diag_log_write("SW_ORCH",
                 "P-frame without valid reference (ref[%d].used=0) — dropping",
                 g_pipeline->active_ref);
        g_pipeline->frames_dropped++;
        return -6;
    }

    /* ---- Stage 1b: Initialize current frame planes ---- */
    if (g_pipeline->slice.slice_type == SW_SLICE_P) {
        /* P-frames: skip full-frame memcpy (was ~4ms for 195KB).
         * P_SKIP zero-MV MBs now copy per-MB in the recon loop
         * (~1ms scattered) instead of a single contiguous memcpy. */
    } else {
        memset(g_pipeline->current.y_plane, 128, Y_PLANE_SIZE);
        memset(g_pipeline->current.u_plane, 128, UV_PLANE_SIZE);
        memset(g_pipeline->current.v_plane, 128, UV_PLANE_SIZE);
    }

    sceRtcGetCurrentTick(&t_cavlc);

    /* ---- Stage 1c: All-skip P-frame fast path ---- */
    if (g_pipeline->slice.slice_type == SW_SLICE_P) {
        int all_skip = 1;
        for (int i = 0; i < g_pipeline->mb_count; i++) {
            if (!g_pipeline->mbs[i].skip_flag ||
                g_pipeline->mbs[i].mv[0].dx != 0 ||
                g_pipeline->mbs[i].mv[0].dy != 0) {
                all_skip = 0;
                break;
            }
        }
        if (all_skip) {
            u8 *rgba_out = g_rgba_buf[1 - g_rgba_idx];

            sceRtcGetCurrentTick(&t_recon);
            /* Don't swap — current planes don't have the memcpy'd ref data.
             * Since all MBs are P_SKIP zero-MV, the frame is identical to the
             * reference. Keep active_ref pointing at the same valid reference
             * for subsequent P-frames. */
            g_pipeline->frames_decoded++;
            g_pipeline->frame_ready = 1;
            g_pipeline->me_done = 1;
            /* NOTE: Do NOT set g_saw_first_idr or g_idr_fully_decoded here.
             * This is a P-frame fast-path — only IDR frames should set those
             * flags.  Setting them from P-frames caused ALL subsequent
             * error-concealed IDRs to be skipped before reaching the
             * Stage 5 accumulation code (run054 corruption bug). */
            g_pipeline->cavlc_time_us = (u32)(t_cavlc - t_start);
            g_pipeline->recon_time_us = (u32)(t_recon - t_cavlc);

            *out_rgba = rgba_out;
            return 0;
        }
    }

    /* ---- Stage 2: Reconstruction (ME dual-core or CPU fallback) ---- */
    u8 *rgba_out = g_rgba_buf[g_rgba_idx];

    /* Early skip: once IDR accumulation is complete, error-concealed IDRs
     * are useless for display AND accumulation.  Skip the expensive RGBA
     * conversion and show the previous clean frame instead.  We still
     * set frame_ready so the display thread isn't deadlocked, but DON'T
     * toggle rgba_idx so it re-displays the previous clean buffer. */
    {
        extern volatile int g_idr_fully_decoded;
        if (g_idr_fully_decoded && g_pipeline->slice.idr_flag
            && g_pipeline->error_concealed) {
            diag_log_write("SW_ORCH", "DECODE#%d skip: error-concealed IDR after accumulation complete",
                           s_frame_seq);
            sceRtcGetCurrentTick(&t_recon);
            g_pipeline->frames_decoded++;
            g_pipeline->frame_ready = 1;
            g_pipeline->me_done = 1;
            g_pipeline->cavlc_time_us = (u32)(t_cavlc - t_start);
            g_pipeline->recon_time_us = (u32)(t_recon - t_cavlc);
            *out_rgba = g_rgba_buf[1 - g_rgba_idx]; /* Previous clean buffer */
            return 0;
        }
    }

    diag_log_write("SW_ORCH", "DECODE#%d recon_start skip_me=%d use_me=%d\n",
                   s_frame_seq, g_pipeline->error_concealed, g_use_me);

    /* Provide previous RGBA buffer for incremental YUV→RGBA conversion.
     * For IDR (I-slice) frames, all MBs are intra-coded so there are no
     * P_SKIP blocks — set NULL to force full-frame YUV→RGBA. */
    g_pipeline->prev_rgba = g_pipeline->slice.idr_flag
                          ? NULL
                          : g_rgba_buf[1 - g_rgba_idx];

    /* Skip ME for error-concealed frames: the ME may crash on irregular
     * macroblock data (partial decode + concealment defaults).  Using
     * CPU fallback avoids the 2-second ME timeout and keeps the decoder
     * responsive so it can process the replacement IDR quickly. */
    int skip_me = g_pipeline->error_concealed;

    if (g_use_me && !skip_me) {
        /* Dual-core path: flush dcache → dispatch to ME → wait for result */
        sceKernelDcacheWritebackInvalidateAll();

        int me_ret = sw_me_worker_dispatch(g_pipeline, rgba_out);
        if (me_ret < 0) {
            /* ME dispatch failed — fall back to CPU this frame */
            diag_log_write("SW_ORCH", "ME dispatch failed (%d), CPU fallback", me_ret);
            sw_me_worker_cpu_fallback(g_pipeline, rgba_out);
        } else {
            int wait_ret = sw_me_worker_wait();
            if (wait_ret < 0) {
                /* ME crashed/timed out — disable ME, fall back for this + future.
                 * CRITICAL: The ME may have partially written to Y/U/V planes
                 * before crashing. We must reinitialize them so the CPU fallback
                 * starts from a clean slate (intra prediction reads neighbor
                 * pixels from the planes — corrupted neighbors cascade errors). */
                diag_log_write("SW_ORCH", "ME wait failed (%d), disabling ME", wait_ret);
                g_use_me = 0;
                sceKernelDcacheWritebackInvalidateAll();
                if (g_pipeline->slice.slice_type != SW_SLICE_P) {
                    memset(g_pipeline->current.y_plane, 128, Y_PLANE_SIZE);
                    memset(g_pipeline->current.u_plane, 128, UV_PLANE_SIZE);
                    memset(g_pipeline->current.v_plane, 128, UV_PLANE_SIZE);
                }
                sw_me_worker_cpu_fallback(g_pipeline, rgba_out);
            }
            /* Invalidate CPU dcache to see ME's output */
            sceKernelDcacheWritebackInvalidateAll();
        }
    } else {
        /* CPU-only path (Main CPU VFPU with timing instrumentation) */
        if (skip_me && g_use_me) {
            diag_log_write("SW_ORCH", "Error-concealed frame: skipping ME, using CPU fallback");
        }
        sw_me_worker_cpu_fallback(g_pipeline, rgba_out);
    }

    /* Note: We do NOT request IDR for error-concealed frames here.
     * Recovery is handled by RFI at the FEC layer (rtp_fec.c sends
     * 0x0301 with lost frame range).  Requesting IDR creates massive
     * 65KB frames that WiFi 802.11b can't deliver intact, triggering
     * a vicious cycle of corrupt IDR → request → corrupt again.
     * RFI tells Sunshine to send a small recovery P-frame instead. */

    sceRtcGetCurrentTick(&t_recon);

    diag_log_write("SW_ORCH", "DECODE#%d recon_done us=%u\n",
                   s_frame_seq, (unsigned)(t_recon - t_cavlc));

    /* ---- Pixel Diagnostic Dump (once per clean IDR) ---- */
    {
        static int s_pixel_dump_done = 0;
        if (!s_pixel_dump_done && g_pipeline->slice.idr_flag &&
            !g_pipeline->error_concealed) {
            s_pixel_dump_done = 1;
            /* Sample 5 pixels at known positions and dump Y/U/V + RGBA */
            const int sample_x[] = {  0,  120, 240, 360, 479 };
            const int sample_y[] = {  0,   68, 136, 204, 271 };
            for (int sp = 0; sp < 5; sp++) {
                int sx = sample_x[sp], sy = sample_y[sp];
                u8 yv = g_pipeline->current.y_plane[sy * SW_FRAME_WIDTH + sx];
                u8 uv = g_pipeline->current.u_plane[(sy/2) * (SW_FRAME_WIDTH/2) + sx/2];
                u8 vv = g_pipeline->current.v_plane[(sy/2) * (SW_FRAME_WIDTH/2) + sx/2];
                u32 rgba = ((u32 *)rgba_out)[sy * SW_FRAME_STRIDE + sx];
                diag_log_write("PIXDUMP",
                    "IDR px(%d,%d) Y=%u U=%u V=%u RGBA=0x%08X (R=%u G=%u B=%u)",
                    sx, sy, yv, uv, vv, rgba,
                    rgba & 0xFF, (rgba >> 8) & 0xFF, (rgba >> 16) & 0xFF);
            }
            /* Also dump MB#0 raw coefficients for verification */
            diag_log_write("PIXDUMP", "MB0 type=%d qp=%d cbp=0x%02X i16mode=%d",
                g_pipeline->mbs[0].mb_type, g_pipeline->mbs[0].qp_y,
                g_pipeline->mbs[0].coded_block_pattern,
                g_pipeline->mbs[0].intra16x16_mode);
            diag_log_write("PIXDUMP", "MB0 luma_dc: %d %d %d %d  %d %d %d %d",
                g_pipeline->mbs[0].luma_dc[0], g_pipeline->mbs[0].luma_dc[1],
                g_pipeline->mbs[0].luma_dc[2], g_pipeline->mbs[0].luma_dc[3],
                g_pipeline->mbs[0].luma_dc[4], g_pipeline->mbs[0].luma_dc[5],
                g_pipeline->mbs[0].luma_dc[6], g_pipeline->mbs[0].luma_dc[7]);
            diag_log_write("PIXDUMP", "MB0 chroma_dc_cb: %d %d %d %d  cr: %d %d %d %d",
                g_pipeline->mbs[0].chroma_dc_cb[0], g_pipeline->mbs[0].chroma_dc_cb[1],
                g_pipeline->mbs[0].chroma_dc_cb[2], g_pipeline->mbs[0].chroma_dc_cb[3],
                g_pipeline->mbs[0].chroma_dc_cr[0], g_pipeline->mbs[0].chroma_dc_cr[1],
                g_pipeline->mbs[0].chroma_dc_cr[2], g_pipeline->mbs[0].chroma_dc_cr[3]);
        }
    }

    /* ---- Stage 5: Post-processing ---- */
    /* Progressive IDR Accumulation:
     * WiFi 802.11b can't deliver full IDR frames intact (65KB = 63 packets,
     * 20-50% loss).  Instead of waiting for one perfect IDR, we accumulate
     * successfully-decoded MBs from multiple partial IDRs into the reference.
     *
     * Each partial IDR decodes some MBs (e.g., 27/510) before hitting corrupt
     * data.  We selectively copy ONLY those good MBs from current→reference,
     * leaving the rest of the reference unchanged.  After 3-5 partial IDRs,
     * the reference is complete and P-frames decode correctly. */
    if (g_pipeline->slice.idr_flag && g_pipeline->error_concealed) {
        static u8 s_accum_bitmap[(SW_TOTAL_MBS + 7) / 8]; /* 64 bytes */
        static int s_accum_count = 0;
        static int s_accum_complete = 0;

        int ref_idx = g_pipeline->active_ref;
        int have_valid_ref = g_pipeline->ref_frames[ref_idx].used;
        /* Use real_mb_count (pre-error count), NOT mb_count which includes
         * concealment-filled MBs.  Copying concealed MBs into the reference
         * would overwrite accumulated good data with garbage (run054 bug). */
        int good_mbs = g_pipeline->real_mb_count;

        if (!have_valid_ref) {
            /* First IDR ever — accept even partial as bootstrap reference */
            swap_ref_frames();
            /* Initialize accumulation bitmap with decoded MBs */
            memset(s_accum_bitmap, 0, sizeof(s_accum_bitmap));
            s_accum_count = 0;
            for (int i = 0; i < good_mbs && i < SW_TOTAL_MBS; i++) {
                s_accum_bitmap[i / 8] |= (1 << (i % 8));
                s_accum_count++;
            }
            diag_log_write("SW_ORCH",
                     "Bootstrap IDR: %d/%d MBs (first reference, accum=%d)",
                     good_mbs, g_pipeline->total_mbs_expected, s_accum_count);
        } else if (good_mbs > 0 && !s_accum_complete) {
            /* Accumulate: copy only NEW decoded MBs from current into reference */
            u8 *ref_y = g_pipeline->ref_frames[ref_idx].y_plane;
            u8 *ref_u = g_pipeline->ref_frames[ref_idx].u_plane;
            u8 *ref_v = g_pipeline->ref_frames[ref_idx].v_plane;
            u8 *cur_y = g_pipeline->current.y_plane;
            u8 *cur_u = g_pipeline->current.u_plane;
            u8 *cur_v = g_pipeline->current.v_plane;
            int newly_merged = 0;

            for (int i = 0; i < good_mbs && i < SW_TOTAL_MBS; i++) {
                if (s_accum_bitmap[i / 8] & (1 << (i % 8)))
                    continue; /* Already have this MB */

                int mb_row = i / SW_MB_WIDTH;
                int mb_col = i % SW_MB_WIDTH;

                /* Copy 16x16 Y block */
                int y_off = mb_row * 16 * 480 + mb_col * 16;
                for (int r = 0; r < 16; r++) {
                    memcpy(ref_y + y_off + r * 480,
                           cur_y + y_off + r * 480, 16);
                }

                /* Copy 8x8 U/V blocks */
                int uv_off = mb_row * 8 * 240 + mb_col * 8;
                for (int r = 0; r < 8; r++) {
                    memcpy(ref_u + uv_off + r * 240,
                           cur_u + uv_off + r * 240, 8);
                    memcpy(ref_v + uv_off + r * 240,
                           cur_v + uv_off + r * 240, 8);
                }

                s_accum_bitmap[i / 8] |= (1 << (i % 8));
                s_accum_count++;
                newly_merged++;
            }

            diag_log_write("SW_ORCH",
                     "IDR accumulate: +%d new MBs (total %d/%d covered)",
                     newly_merged, s_accum_count, SW_TOTAL_MBS);

            if (s_accum_count >= SW_TOTAL_MBS) {
                s_accum_complete = 1;
                extern volatile int g_idr_fully_decoded;
                g_idr_fully_decoded = 1;
                diag_log_write("SW_ORCH",
                         "IDR ACCUMULATION COMPLETE: all %d MBs covered!",
                         SW_TOTAL_MBS);
            }
        } else {
            diag_log_write("SW_ORCH",
                     "Skipping IDR: %s",
                     s_accum_complete ? "accumulation already complete" :
                                       "0 MBs decoded");
        }
    } else if (g_pipeline->slice.idr_flag && !g_pipeline->error_concealed) {
        /* Clean IDR (rare on lossy WiFi): full reference swap — the entire
         * frame was decoded without concealment, so it's a perfect reference */
        swap_ref_frames();
    } else {
        /* P-frame (clean or error-concealed): SELECTIVE reference update.
         * Copy only the genuinely decoded MBs (0..real_mb_count-1) from
         * current into the reference.  Leave concealed MBs untouched so the
         * reference retains previously accumulated IDR data and prior good
         * P-frame predictions.
         *
         * run058 finding: ZERO fully clean frames in 60s of streaming.
         * The old all-or-nothing swap (requiring error_concealed==0) meant
         * the reference was NEVER updated by P-frames.  With selective update,
         * a frame with 503/510 real MBs updates 503 MBs — the reference
         * improves incrementally from every frame. */
        int good_mbs = g_pipeline->real_mb_count;
        if (good_mbs > 0) {
            int ref_idx = g_pipeline->active_ref;
            u8 *ref_y = g_pipeline->ref_frames[ref_idx].y_plane;
            u8 *ref_u = g_pipeline->ref_frames[ref_idx].u_plane;
            u8 *ref_v = g_pipeline->ref_frames[ref_idx].v_plane;
            u8 *cur_y = g_pipeline->current.y_plane;
            u8 *cur_u = g_pipeline->current.u_plane;
            u8 *cur_v = g_pipeline->current.v_plane;

            /* Bulk copy complete MB-rows (fast: single memcpy per plane) */
            int complete_rows = good_mbs / SW_MB_WIDTH;
            if (complete_rows > 0) {
                memcpy(ref_y, cur_y, complete_rows * 16 * SW_FRAME_WIDTH);
                memcpy(ref_u, cur_u, complete_rows * 8 * (SW_FRAME_WIDTH / 2));
                memcpy(ref_v, cur_v, complete_rows * 8 * (SW_FRAME_WIDTH / 2));
            }

            /* Copy remaining partial-row MBs individually */
            int partial_start = complete_rows * SW_MB_WIDTH;
            for (int i = partial_start; i < good_mbs && i < SW_TOTAL_MBS; i++) {
                int mb_row = i / SW_MB_WIDTH;
                int mb_col = i % SW_MB_WIDTH;
                int y_off = mb_row * 16 * SW_FRAME_WIDTH + mb_col * 16;
                for (int r = 0; r < 16; r++)
                    memcpy(ref_y + y_off + r * SW_FRAME_WIDTH,
                           cur_y + y_off + r * SW_FRAME_WIDTH, 16);
                int uv_off = mb_row * 8 * (SW_FRAME_WIDTH / 2) + mb_col * 8;
                for (int r = 0; r < 8; r++) {
                    memcpy(ref_u + uv_off + r * (SW_FRAME_WIDTH / 2),
                           cur_u + uv_off + r * (SW_FRAME_WIDTH / 2), 8);
                    memcpy(ref_v + uv_off + r * (SW_FRAME_WIDTH / 2),
                           cur_v + uv_off + r * (SW_FRAME_WIDTH / 2), 8);
                }
            }
        }
    }

    /* Display update logic:
     * - Error-concealed IDRs (accumulation path): skip display — raw decode
     *   artifacts and concealment gray would flash on screen.  Keep showing
     *   the previous buffer.
     * - All P-frames (clean or concealed) + clean IDRs: display normally.
     *   P-frames show the best available prediction from the progressively
     *   improving reference.  Even with some concealment, the decoded MBs
     *   show real content.
     * (run056 fix: error-concealed IDRs don't toggle display)
     * (run059 fix: error-concealed P-frames DO display — they have useful content) */
    if (g_pipeline->slice.idr_flag && g_pipeline->error_concealed) {
        /* IDR accumulation path — don't display */
        *out_rgba = g_rgba_buf[1 - g_rgba_idx];
    } else {
        /* All P-frames + clean IDRs — display normally */
        g_rgba_idx = 1 - g_rgba_idx;
        *out_rgba = rgba_out;
    }

    g_pipeline->frames_decoded++;
    g_pipeline->frame_ready = 1;
    g_pipeline->me_done = 1;

    /* Only IDR frames update IDR-tracking flags.  P-frames must NEVER
     * set g_saw_first_idr or g_idr_fully_decoded — doing so causes the
     * early-return at Stage 2 to skip ALL error-concealed IDRs before
     * they reach Stage 5 accumulation (run054 corruption bug). */
    if (g_pipeline->slice.idr_flag) {
        extern int g_saw_first_idr;
        extern volatile int g_idr_fully_decoded;
        g_saw_first_idr = 1;
        /* Only mark IDR fully decoded if it was clean (no error concealment).
         * For partial/concealed IDRs, the frame IS usable as a reference
         * (far better than frozen screen), but we leave g_idr_fully_decoded=0
         * so the ping thread keeps requesting a clean replacement IDR. */
        if (!g_pipeline->error_concealed) {
            g_idr_fully_decoded = 1;
        }
    }

    /* Timing in microseconds (PSP tick = 1MHz) */
    g_pipeline->cavlc_time_us = (u32)(t_cavlc - t_start);
    g_pipeline->recon_time_us = (u32)(t_recon - t_cavlc);

    /* Log timing every 120 frames — now includes per-phase breakdown */
    if ((g_pipeline->frames_decoded % 120) == 0) {
        diag_log_write("SW_ORCH",
                 "PERF n=%u: CAVLC=%uus MB=%uus DEBLK=%uus YUV=%uus tot=%uus skip=%u/%u",
                 g_pipeline->frames_decoded,
                 g_pipeline->cavlc_time_us,
                 g_pipeline->mb_loop_us,
                 g_pipeline->deblock_us,
                 g_pipeline->yuv_us,
                 g_pipeline->cavlc_time_us + g_pipeline->recon_time_us,
                 g_pipeline->skip_mb_count,
                 g_pipeline->mb_count);
    }

    /* Warn on slow frames — periodic to avoid log flood at 60fps.
     * First 3 slow frames logged immediately, then every 120th. */
    {
        static u32 slow_count = 0;
        u32 total_us = g_pipeline->cavlc_time_us + g_pipeline->recon_time_us;
        if (total_us > 33333) {
            slow_count++;
            if (slow_count <= 3 || (slow_count % 120) == 0) {
                diag_log_write("SW_ORCH",
                         "SLOW #%u Frame %u: %uus CAVLC=%uus MB=%uus DEBLK=%uus YUV=%uus skip=%u",
                         slow_count, g_pipeline->frames_decoded, total_us,
                         g_pipeline->cavlc_time_us,
                         g_pipeline->mb_loop_us,
                         g_pipeline->deblock_us,
                         g_pipeline->yuv_us,
                         g_pipeline->skip_mb_count);
            }
        }
    }

    /* out_rgba already set above in the IDR/P-frame branch */
    return 0;
}

/* ============================================================================
 * Stats & Status
 * ============================================================================*/

void sw_pipeline_get_stats(u32 *decoded, u32 *dropped,
                           u32 *cpu_us, u32 *me_us)
{
    if (!g_pipeline) {
        if (decoded) *decoded = 0;
        if (dropped) *dropped = 0;
        if (cpu_us) *cpu_us = 0;
        if (me_us) *me_us = 0;
        return;
    }

    if (decoded) *decoded = g_pipeline->frames_decoded;
    if (dropped) *dropped = g_pipeline->frames_dropped;
    if (cpu_us)  *cpu_us  = g_pipeline->cavlc_time_us;
    if (me_us)   *me_us   = g_pipeline->recon_time_us;
}

int sw_pipeline_is_ready(void)
{
    return (g_pipeline != NULL && g_pipeline->pipeline_active &&
            g_got_sps && g_got_pps);
}
