/*
 * ffmpeg_decode.c - FFmpeg-based H.264 Software Decoder for PSP
 *
 * Replaces the hand-written CAVLC + VFPU reconstruction pipeline with
 * FFmpeg's proven libavcodec H.264 decoder.  Modeled after the 3DS
 * moonlight implementation (src/video/ffmpeg.c).
 *
 * Architecture (Dual-Core Pipeline):
 *   1. Receives raw Annex-B H.264 NAL data from RTP reassembly
 *   2. Feeds to FFmpeg via avcodec_send_packet() (Main CPU)
 *   3. Retrieves decoded YUV420P frame via avcodec_receive_frame() (Main CPU)
 *   4. Dispatches YUV420P → RGBA8888 conversion to Media Engine (async)
 *   5. Returns PREVIOUS frame's RGBA while ME converts current frame
 *   6. Throughput = max(decode_time, convert_time) instead of sum
 *
 * Fallback: If ME initialization fails, runs YUV→RGBA on main CPU (sequential).
 */

/* Diagnostic: define to test ME dispatch with no-op function */
/* #define ME_NOOP_TEST 1 */

#include <pspsdk.h>
#include <pspkernel.h>
#include <pspthreadman.h>
#include <psprtc.h>
#include <string.h>
#include <malloc.h>

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include "sw_decode_pipeline.h"
#include "stream_resolution.h"
#include "shared.h"
#include "diag_log.h"
#include "me.h"
#include "settings_menu.h"  /* PspConfig */
#include "decode_flags.h"

extern PspConfig g_psp_config;

/* ============================================================================
 * FFmpeg Decoder State
 * ============================================================================*/

static AVCodecContext *g_codec_ctx = NULL;
static AVFrame       *g_avframe[2] = {NULL, NULL};  /* double-buffered for zero-copy ME */
static int            g_avframe_idx = 0;             /* which AVFrame to decode into */
static AVPacket      *g_avpkt     = NULL;

/* RGBA output double-buffer — sized from g_stream_res at init.
 * All resolution-dependent values come from the unified resolution table. */

static u8 *g_rgba_buf[2] = {NULL, NULL};
static int  g_rgba_idx = 0;

/* Statistics */
static int g_frames_decoded = 0;
static int g_frames_dropped = 0;
static int g_got_first_frame = 0;

/* External flags used by decoder thread */
int g_saw_first_idr = 0;
volatile int g_idr_fully_decoded = 0;

/* Reference corruption guard: set on ANY decode error OR when the RTP
 * layer drops a frame (FEC unrecoverable, mid-assembly drop).  Cleared
 * only when a key frame (IDR) is successfully decoded.  While set,
 * P-frames are skipped (last good frame repeated) to prevent displaying
 * artifacts from broken reference chains.
 * MIPS Allegrex: volatile aligned word = sufficient memory barrier. */
volatile int g_refs_corrupted = 0;

/* SPS/PPS cache for recovery after force_restart.
 * When the decoder thread force-restarts, the new AVCodecContext has no
 * SPS/PPS parameter sets.  Without them, avcodec_send_packet() hangs on
 * PSP's FFmpeg build.  Step 1b blocks all frames without SPS (returns -5).
 * This cache stores raw Annex-B SPS+PPS NALs from the first IDR and
 * re-injects them into fresh codec contexts after restart. */
static uint8_t s_cached_sps_pps[256];
static int     s_cached_sps_pps_len = 0;

/* ============================================================================
 * Synthetic SPS/PPS Generator
 *
 * When no IDR with SPS/PPS arrives (WiFi packet loss), generate valid
 * SPS+PPS NALs from the known stream parameters (resolution, profile).
 * This allows FFmpeg to accept and decode P-frames without waiting for
 * an IDR that may never arrive intact over lossy 802.11b.
 *
 * H.264 Baseline Profile, CAVLC, 1 reference frame.
 * ============================================================================*/

/* Exp-Golomb bitstream writer */
typedef struct {
    uint8_t *buf;
    int byte_pos;
    int bit_pos;  /* bits written in current byte (0-7) */
    int max_len;
} BitWriter;

static void bw_init(BitWriter *bw, uint8_t *buf, int max_len) {
    bw->buf = buf;
    bw->byte_pos = 0;
    bw->bit_pos = 0;
    bw->max_len = max_len;
    memset(buf, 0, max_len);
}

static void bw_put_bit(BitWriter *bw, int bit) {
    if (bw->byte_pos >= bw->max_len) return;
    if (bit) bw->buf[bw->byte_pos] |= (0x80 >> bw->bit_pos);
    bw->bit_pos++;
    if (bw->bit_pos == 8) { bw->bit_pos = 0; bw->byte_pos++; }
}

static void bw_put_bits(BitWriter *bw, int val, int n) {
    for (int i = n - 1; i >= 0; i--)
        bw_put_bit(bw, (val >> i) & 1);
}

static void bw_put_ue(BitWriter *bw, int val) {
    /* Exp-Golomb unsigned: encode val as (zeros)(val+1 in binary) */
    int v = val + 1;
    int n = 0, tmp = v;
    while (tmp > 1) { tmp >>= 1; n++; }
    for (int i = 0; i < n; i++) bw_put_bit(bw, 0);
    bw_put_bits(bw, v, n + 1);
}

static void bw_put_se(BitWriter *bw, int val) {
    /* Exp-Golomb signed: 0→0, 1→1, -1→2, 2→3, -2→4, ... */
    int mapped = (val > 0) ? (2 * val - 1) : (-2 * val);
    bw_put_ue(bw, mapped);
}

static int bw_rbsp_trailing(BitWriter *bw) {
    bw_put_bit(bw, 1);
    while (bw->bit_pos != 0) bw_put_bit(bw, 0);
    return bw->byte_pos;
}

static int generate_synthetic_sps_pps(uint8_t *out, int max_len)
{
    int pos = 0;
    int w_mbs = g_stream_res.mb_width;   /* width / 16 */
    int h_mbs = g_stream_res.mb_height;  /* height / 16 */

    if (w_mbs <= 0 || h_mbs <= 0 || max_len < 32) return 0;

    /* Determine level_idc from total MBs (conservative) */
    int total_mbs = w_mbs * h_mbs;
    int level_idc;
    if      (total_mbs <= 99)   level_idc = 10;  /* 176x144 */
    else if (total_mbs <= 396)  level_idc = 20;  /* 352x288 */
    else if (total_mbs <= 792)  level_idc = 21;  /* 480x360 */
    else                        level_idc = 30;  /* 720x480 */

    /* Check if frame cropping is needed */
    int crop_right = (w_mbs * 16 - g_stream_res.width) / 2;
    int crop_bottom = (h_mbs * 16 - g_stream_res.height) / 2;
    int need_crop = (crop_right > 0 || crop_bottom > 0);

    /* --- SPS (NAL type 7) --- */
    out[pos++] = 0x00; out[pos++] = 0x00; out[pos++] = 0x00; out[pos++] = 0x01;
    out[pos++] = 0x67;  /* nal_ref_idc=3, type=7 */
    out[pos++] = 66;    /* profile_idc = Baseline */
    out[pos++] = 0xC0;  /* constraint_set0=1, constraint_set1=1 */
    out[pos++] = (uint8_t)level_idc;

    {
        BitWriter bw;
        uint8_t sps_body[32];
        bw_init(&bw, sps_body, 32);

        bw_put_ue(&bw, 0);                  /* seq_parameter_set_id */
        bw_put_ue(&bw, 0);                  /* log2_max_frame_num_minus4 */
        bw_put_ue(&bw, 2);                  /* pic_order_cnt_type */
        bw_put_ue(&bw, 1);                  /* max_num_ref_frames */
        bw_put_bit(&bw, 0);                 /* gaps_in_frame_num_allowed */
        bw_put_ue(&bw, w_mbs - 1);          /* pic_width_in_mbs_minus1 */
        bw_put_ue(&bw, h_mbs - 1);          /* pic_height_in_map_units_minus1 */
        bw_put_bit(&bw, 1);                 /* frame_mbs_only_flag */
        bw_put_bit(&bw, 0);                 /* direct_8x8_inference_flag */

        if (need_crop) {
            bw_put_bit(&bw, 1);             /* frame_cropping_flag */
            bw_put_ue(&bw, 0);              /* crop_left */
            bw_put_ue(&bw, crop_right);     /* crop_right */
            bw_put_ue(&bw, 0);              /* crop_top */
            bw_put_ue(&bw, crop_bottom);    /* crop_bottom */
        } else {
            bw_put_bit(&bw, 0);             /* frame_cropping_flag */
        }
        bw_put_bit(&bw, 0);                 /* vui_parameters_present_flag */

        int sps_len = bw_rbsp_trailing(&bw);
        memcpy(out + pos, sps_body, sps_len);
        pos += sps_len;
    }

    /* --- PPS (NAL type 8) --- */
    out[pos++] = 0x00; out[pos++] = 0x00; out[pos++] = 0x00; out[pos++] = 0x01;
    out[pos++] = 0x68;  /* nal_ref_idc=3, type=8 */

    {
        BitWriter bw;
        uint8_t pps_body[16];
        bw_init(&bw, pps_body, 16);

        bw_put_ue(&bw, 0);    /* pic_parameter_set_id */
        bw_put_ue(&bw, 0);    /* seq_parameter_set_id */
        bw_put_bit(&bw, 0);   /* entropy_coding_mode = CAVLC */
        bw_put_bit(&bw, 0);   /* bottom_field_pic_order */
        bw_put_ue(&bw, 0);    /* num_slice_groups_minus1 */
        bw_put_ue(&bw, 0);    /* num_ref_idx_l0_default_active_minus1 */
        bw_put_ue(&bw, 0);    /* num_ref_idx_l1_default_active_minus1 */
        bw_put_bit(&bw, 0);   /* weighted_pred_flag */
        bw_put_bits(&bw, 0, 2); /* weighted_bipred_idc */
        bw_put_se(&bw, 0);    /* pic_init_qp_minus26 */
        bw_put_se(&bw, 0);    /* pic_init_qs_minus26 */
        bw_put_se(&bw, 0);    /* chroma_qp_index_offset */
        bw_put_bit(&bw, 1);   /* deblocking_filter_control_present */
        bw_put_bit(&bw, 0);   /* constrained_intra_pred */
        bw_put_bit(&bw, 0);   /* redundant_pic_cnt_present */

        int pps_len = bw_rbsp_trailing(&bw);
        memcpy(out + pos, pps_body, pps_len);
        pos += pps_len;
    }

    diag_log_write("FFMPEG", "Generated synthetic SPS/PPS: %d bytes for %dx%d (%dx%d MBs, level %d)",
                   pos, g_stream_res.width, g_stream_res.height, w_mbs, h_mbs, level_idc);
    return pos;
}

/* Per-frame corruption flag: set by RTP layer (rtp_reassembly, rtp_fec)
 * when the frame about to be submitted has known data loss (seq gaps,
 * RS failure).  Prevents a corrupted IDR from clearing g_refs_corrupted
 * and being displayed with partial macroblock corruption.
 * MIPS Allegrex: volatile aligned word = sufficient memory barrier. */
volatile int g_current_frame_is_corrupt = 0;

/* Desperate recovery: consecutive REF-SKIP count.  File-scoped so both
 * the early-skip gate and the post-decode guard can reference it.
 * After DESPERATE_THRESHOLD consecutive skips (~2s blackout at 30fps),
 * the decoder bypasses REF-SKIP and accepts any frame FFmpeg produces.
 * This prevents permanent black screen on high-loss 802.11b WiFi where
 * multi-packet IDR frames never arrive intact. */
#define DESPERATE_THRESHOLD 60
static int s_ref_skip_count = 0;

/* Watchdog restart signal: set by sw_decoder_thread_force_restart() to
 * tell ffmpeg_pipeline_decode_frame()/rtp_frame_complete_callback() to
 * reset their function-level static counters on next entry.  Avoids
 * stale me_stressed / consecutive_corrupt / s_wait_count after restart. */
volatile int g_decode_counters_reset_pending = 0;

/* ============================================================================
 * Media Engine Dual-Core State
 *
 * YUV→RGBA runs on the ME while main CPU decodes next frame.
 * The pipeline returns the PREVIOUS frame's RGBA to get throughput =
 * max(decode_time, convert_time) instead of sum.
 * ============================================================================*/

/* ME dispatch parameter block — passed via int pointer to ME function */
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

static volatile struct me_struct *g_me_ctrl = NULL;       /* uncached address for ME ops */
static volatile struct me_struct *g_me_ctrl_cached = NULL; /* cached address for alloc/free */
static MeYuv2RgbaParams         *g_me_params = NULL;
static int  g_me_available = 0;   /* 1 if ME initialized OK */
static int  g_me_pending   = 0;   /* 1 if ME is converting */
static u8  *g_me_rgba_out  = NULL; /* which buffer ME writes to */
static u8  *g_last_rgba    = NULL; /* last completed RGBA frame */

/* YUV shadow buffer removed — double-buffered AVFrames allow zero-copy
 * ME dispatch by passing FFmpeg's output pointers directly to ME.
 * Saves ~208KB memcpy per frame (devastating on 16KB dcache). */

/* ============================================================================
 * BT.601 YUV420P → RGBA8888 Conversion (Optimized Fixed-Point)
 *
 * Uses 16-bit fixed-point arithmetic (shift 6) to avoid floating point.
 * Processes 2 pixels at a time sharing the same chroma sample.
 * Uses a lookup table for clamping to eliminate all branches in inner loop.
 * Runs on ME core (dual-core) or main CPU (fallback).
 * ============================================================================*/

/* Clamping LUT: clamp_lut[v + 320] = clamp(v, 0, 255) for v in [-320..575] */
#define CLAMP_OFF 320
#define CLAMP_SIZE 896
static u8 g_clamp_lut[CLAMP_SIZE];
static int g_clamp_ready = 0;

static void init_clamp_lut(void)
{
    for (int i = 0; i < CLAMP_SIZE; i++) {
        int v = i - CLAMP_OFF;
        if (v < 0) g_clamp_lut[i] = 0;
        else if (v > 255) g_clamp_lut[i] = 255;
        else g_clamp_lut[i] = (u8)v;
    }
    g_clamp_ready = 1;
}

#define CLAMP8(v) g_clamp_lut[(v) + CLAMP_OFF]

static void yuv420_to_rgba(const u8 *y_plane, int y_stride,
                           const u8 *u_plane, int u_stride,
                           const u8 *v_plane, int v_stride,
                           u8 *rgba_out, int rgba_stride_pixels,
                           int width, int height)
{
    /* BT.601 conversion (standard for H.264 Baseline at SD resolution):
     * R = 1.164*(Y-16) + 1.596*(V-128)
     * G = 1.164*(Y-16) - 0.391*(U-128) - 0.813*(V-128)
     * B = 1.164*(Y-16) + 2.018*(U-128)
     *
     * Fixed-point (<<6): multiply by 64
     * R = (74*(Y-16) + 102*(V-128)) >> 6
     * G = (74*(Y-16) -  25*(U-128) -  52*(V-128)) >> 6
     * B = (74*(Y-16) + 129*(U-128)) >> 6
     */

    const int w2 = width & ~1; /* round down to even */

    for (int row = 0; row < height; row++) {
        const u8 *yp = y_plane + row * y_stride;
        const u8 *up = u_plane + (row >> 1) * u_stride;
        const u8 *vp = v_plane + (row >> 1) * v_stride;
        u32 *dst = (u32 *)(rgba_out + row * rgba_stride_pixels * 4);

        for (int col = 0; col < w2; col += 2) {
            /* Pre-compute chroma contributions (shared by 2 pixels) */
            int uu = (int)up[col >> 1] - 128;
            int vv = (int)vp[col >> 1] - 128;
            int rv = 102 * vv;
            int guv = -25 * uu - 52 * vv;
            int bu = 129 * uu;

            /* Pixel 0 */
            int c0 = 74 * ((int)yp[col] - 16);
            dst[col] = (u32)CLAMP8((c0 + rv) >> 6)
                     | ((u32)CLAMP8((c0 + guv) >> 6) << 8)
                     | ((u32)CLAMP8((c0 + bu) >> 6) << 16)
                     | 0xFF000000u;

            /* Pixel 1 */
            int c1 = 74 * ((int)yp[col + 1] - 16);
            dst[col + 1] = (u32)CLAMP8((c1 + rv) >> 6)
                         | ((u32)CLAMP8((c1 + guv) >> 6) << 8)
                         | ((u32)CLAMP8((c1 + bu) >> 6) << 16)
                         | 0xFF000000u;
        }
    }
}


/* NOTE: VFPU on ME is non-functional for data extraction (mfv returns 0,
 * sv.s/sv.q crash ME). The C fixed-point yuv420_to_rgba() above achieves
 * ~31µs on ME which is sufficient (1500x speedup from CPU-only 47ms). */

/* ============================================================================
 * ME Entry Point — Runs on Media Engine core (no syscalls!)
 *
 * Receives MeYuv2RgbaParams* cast as int.  Only uses array accesses and
 * fixed-point integer pixel conversion, which is safe for ME execution.
 *
 * NOTE: VFPU on ME is NON-FUNCTIONAL for data extraction:
 *   - mfv (VFPU→GP register): Always returns 0
 *   - sv.s/sv.q (VFPU→memory): Crashes ME
 *   VFPU instructions execute (CU2 enable works) but results cannot be
 *   retrieved.  Using proven C fixed-point code which achieves ~31µs.
 * ============================================================================*/

static int me_yuv420_to_rgba_entry(int param)
{
    MeYuv2RgbaParams *p = (MeYuv2RgbaParams *)(unsigned int)param;

#ifdef ME_NOOP_TEST
    (void)p;
    return 0;
#else
    yuv420_to_rgba(p->y_plane, p->y_stride,
                   p->u_plane, p->u_stride,
                   p->v_plane, p->v_stride,
                   p->rgba_out, p->rgba_stride_pixels,
                   p->width, p->height);

    return 0;
#endif
}

/* ============================================================================
 * ffmpeg_pipeline_init — Initialize FFmpeg H.264 decoder + ME
 * ============================================================================*/

int ffmpeg_pipeline_init(void)
{
    diag_log_write("FFMPEG", "Initializing FFmpeg H.264 decoder...");

    /* Initialize clamping LUT for YUV→RGBA conversion */
    if (!g_clamp_ready) {
        init_clamp_lut();
    }

    /* Find the H.264 decoder */
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!codec) {
        diag_log_write("FFMPEG", "FATAL: H.264 decoder not found in libavcodec!");
        return -1;
    }
    diag_log_write("FFMPEG", "Found codec: %s", codec->name);

    /* Allocate codec context */
    g_codec_ctx = avcodec_alloc_context3(codec);
    if (!g_codec_ctx) {
        diag_log_write("FFMPEG", "Failed to allocate codec context");
        return -2;
    }

    /* Configure for low-latency streaming (matching 3DS implementation) */
    g_codec_ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    g_codec_ctx->flags |= AV_CODEC_FLAG_OUTPUT_CORRUPT;
    g_codec_ctx->flags2 |= AV_CODEC_FLAG2_SHOW_ALL;
    g_codec_ctx->flags2 |= AV_CODEC_FLAG2_FAST;  /* Enable speed-optimized decode paths (D-1) */

    /* Deblocking filter DISABLED — saves ~3-5ms per frame (~20-30% of budget)
     * on PSP-1000's 333MHz MIPS.  At 480x272 @ 500kbps over WiFi 802.11b,
     * blocking artifacts from skip_loop_filter are imperceptible.
     * This is THE single biggest performance win for stable FPS. */
    g_codec_ctx->skip_loop_filter = AVDISCARD_ALL;
    g_codec_ctx->skip_frame = AVDISCARD_NONREF;   /* Skip non-reference frames */
    g_codec_ctx->skip_idct = AVDISCARD_NONREF;     /* Skip IDCT for non-ref frames */

    /* Single-threaded — PSP has no pthreads, and we manage dual-core ourselves */
    g_codec_ctx->thread_count = 1;

    /* Resolution — from unified global table (initialized by caller) */
    {
        if (!g_stream_res.initialized) {
            stream_resolution_init(g_psp_config.width, g_psp_config.height);
        }
        g_codec_ctx->width  = g_stream_res.width;
        g_codec_ctx->height = g_stream_res.height;
        diag_log_write("FFMPEG", "RGBA config: %dx%d stride=%d size=%d KB (from g_stream_res)",
                       g_stream_res.width, g_stream_res.height,
                       g_stream_res.stride, g_stream_res.rgba_size / 1024);
    }
    g_codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    /* Open the decoder */
    int ret = avcodec_open2(g_codec_ctx, codec, NULL);
    if (ret < 0) {
        diag_log_write("FFMPEG", "avcodec_open2 failed: %d", ret);
        avcodec_free_context(&g_codec_ctx);
        g_codec_ctx = NULL;
        return -3;
    }
    diag_log_write("FFMPEG", "Decoder opened successfully");

    /* Allocate double-buffered frames (zero-copy ME pipeline) */
    g_avframe[0] = av_frame_alloc();
    g_avframe[1] = av_frame_alloc();
    if (!g_avframe[0] || !g_avframe[1]) {
        diag_log_write("FFMPEG", "av_frame_alloc failed");
        avcodec_free_context(&g_codec_ctx);
        g_codec_ctx = NULL;
        return -4;
    }
    g_avframe_idx = 0;

    g_avpkt = av_packet_alloc();
    if (!g_avpkt) {
        diag_log_write("FFMPEG", "av_packet_alloc failed");
        av_frame_free(&g_avframe[0]);
        av_frame_free(&g_avframe[1]);
        avcodec_free_context(&g_codec_ctx);
        g_codec_ctx = NULL;
        return -5;
    }

    /* Allocate RGBA double-buffer (64-byte aligned for GU DMA) */
    g_rgba_buf[0] = (u8 *)memalign(64, g_stream_res.rgba_size);
    g_rgba_buf[1] = (u8 *)memalign(64, g_stream_res.rgba_size);
    if (!g_rgba_buf[0] || !g_rgba_buf[1]) {
        diag_log_write("FFMPEG", "RGBA buffer allocation failed (%d bytes each)", g_stream_res.rgba_size);
        av_packet_free(&g_avpkt);
        av_frame_free(&g_avframe[0]);
        av_frame_free(&g_avframe[1]);
        avcodec_free_context(&g_codec_ctx);
        g_codec_ctx = NULL;
        return -6;
    }
    memset(g_rgba_buf[0], 0, g_stream_res.rgba_size);
    memset(g_rgba_buf[1], 0, g_stream_res.rgba_size);
    g_rgba_idx = 0;

    /* Initialize Media Engine for dual-core YUV→RGBA */
    g_me_available = 0;
    g_me_pending = 0;
    g_last_rgba = NULL;
    g_me_rgba_out = NULL;

    /* ── Load the ME helper PRX (provides InitME/KillME exports) ──
     *
     * CRITICAL: The ME helper kernel PRX must be loaded AND started
     * BEFORE calling InitME(). Without this, the import stubs in
     * MediaEngine.o are unresolved and InitME returns 0x8002013A
     * (SCE_KERNEL_ERROR_NOT_STARTED). */
    {
        /* Try multiple paths for XMB (ms0:/) and PSPLink (host0:/ CWD) */
        static const char *me_paths[] = {
            "ms0:/PSP/GAME/Moonlight/moonlight_me_helper.prx",
            "moonlight_me_helper.prx",
            NULL
        };
        SceUID me_prx_id = -1;
        for (int pi = 0; me_paths[pi]; pi++) {
            me_prx_id = sceKernelLoadModule(me_paths[pi], 0, NULL);
            if (me_prx_id >= 0) {
                diag_log_write("FFMPEG", "ME helper loaded from %s", me_paths[pi]);
                break;
            }
            if (me_prx_id == (SceUID)0x80020139 ||
                me_prx_id == (SceUID)0x8002032C) break;
        }
        if (me_prx_id >= 0) {
            int status = 0;
            int res = sceKernelStartModule(me_prx_id, 0, NULL, &status, NULL);
            if (res < 0 && res != (int)0x80020139 && res != (int)0x8002032C) {
                diag_log_write("FFMPEG", "ME helper StartModule failed: 0x%08X", (unsigned)res);
            } else {
                diag_log_write("FFMPEG", "ME helper loaded uid=0x%08X start=0x%08X",
                               (unsigned)me_prx_id, (unsigned)res);
            }
        } else if (me_prx_id == (SceUID)0x80020139 ||
                   me_prx_id == (SceUID)0x8002032C) {
            /* Already loaded from prior session or pre-loaded by test script */
            diag_log_write("FFMPEG", "ME helper already resident (0x%08X)", (unsigned)me_prx_id);
        } else {
            diag_log_write("FFMPEG", "ME helper LoadModule failed: 0x%08X — CPU-only mode",
                           (unsigned)me_prx_id);
        }
    }

    diag_log_write("FFMPEG", "ME init: allocating control structures...");

    g_me_ctrl_cached = (volatile struct me_struct *)memalign(64, sizeof(struct me_struct));
    g_me_params = (MeYuv2RgbaParams *)memalign(64, sizeof(MeYuv2RgbaParams));

    if (!g_me_ctrl_cached || !g_me_params) {
        diag_log_write("FFMPEG", "ME alloc FAILED: ctrl=%p params=%p — CPU-only mode",
                       g_me_ctrl_cached, g_me_params);
        g_me_ctrl = NULL;
        g_me_ctrl_cached = NULL;
        g_me_params = NULL;
    }

    /* Convert to uncached address (OR with 0x40000000) so both CPUs bypass dcache */
    if (g_me_ctrl_cached) {
        g_me_ctrl = (volatile struct me_struct *)((u32)g_me_ctrl_cached | 0x40000000);
    }

    diag_log_write("FFMPEG", "ME alloc: ctrl=%p (uncached=%p) params=%p",
                   g_me_ctrl_cached, g_me_ctrl, g_me_params);

    if (g_me_ctrl_cached && g_me_params) {
        memset((void *)g_me_ctrl_cached, 0, sizeof(struct me_struct));
        sceKernelDcacheWritebackInvalidateAll();
        memset(g_me_params, 0, sizeof(MeYuv2RgbaParams));

        diag_log_write("FFMPEG", "ME calling InitME...");
        int me_ret = InitME(g_me_ctrl);
        diag_log_write("FFMPEG", "ME InitME returned %d", me_ret);

        if (me_ret == 0) {
            /* Wait for ME kernel thread to become ready (sets done=1) */
            sceKernelDcacheWritebackInvalidateAll();
            int ready_loops = 0;
            while (!g_me_ctrl->done && ready_loops < 2000000) {
                ready_loops++;
                if ((ready_loops % 100000) == 0)
                    sceKernelDcacheWritebackInvalidateAll();
            }
            sceKernelDcacheWritebackInvalidateAll();

            if (g_me_ctrl->done) {
                g_me_available = 1;
                diag_log_write("FFMPEG", "ME ready after %d loops -- dual-core active",
                               ready_loops);
            } else {
                diag_log_write("FFMPEG", "ME thread not ready after %d loops -- CPU fallback",
                               ready_loops);
            }
        } else {
            diag_log_write("FFMPEG", "ME InitME FAILED (%d) -- CPU fallback", me_ret);
        }
    } else {
        diag_log_write("FFMPEG", "ME alloc FAILED -- CPU fallback");
    }

    g_frames_decoded = 0;
    g_frames_dropped = 0;
    g_got_first_frame = 0;
    g_saw_first_idr = 0;
    g_idr_fully_decoded = 0;
    g_refs_corrupted = 0;
    s_ref_skip_count = 0;

    diag_log_write("FFMPEG", "FFmpeg pipeline ready (RGBA bufs = %d KB each) ME=%d BUILD_V3_ZEROCOPY",
                   g_stream_res.rgba_size / 1024, g_me_available);

    return 0;
}

/* ============================================================================
 * ffmpeg_pipeline_shutdown — Clean teardown
 * ============================================================================*/

void ffmpeg_pipeline_shutdown(void)
{
    diag_log_write("FFMPEG", "Shutting down FFmpeg pipeline (decoded=%d dropped=%d)",
                   g_frames_decoded, g_frames_dropped);

    /* Wait for any pending ME conversion before cleanup */
    if (g_me_available && g_me_pending && g_me_ctrl) {
        sceKernelDcacheWritebackInvalidateAll();
        WaitME(g_me_ctrl);
        g_me_pending = 0;
    }

    /* Tear down Media Engine */
    if (g_me_available && g_me_ctrl) {
        KillME(g_me_ctrl);
        g_me_available = 0;
        diag_log_write("FFMPEG", "Media Engine shut down");
    }

    if (g_me_ctrl_cached) { free((void *)g_me_ctrl_cached); g_me_ctrl_cached = NULL; g_me_ctrl = NULL; }
    if (g_me_params) { free(g_me_params); g_me_params = NULL; }
    g_me_pending = 0;
    g_last_rgba = NULL;
    g_me_rgba_out = NULL;

    if (g_avpkt) { av_packet_free(&g_avpkt); g_avpkt = NULL; }
    if (g_avframe[0]) { av_frame_free(&g_avframe[0]); g_avframe[0] = NULL; }
    if (g_avframe[1]) { av_frame_free(&g_avframe[1]); g_avframe[1] = NULL; }
    if (g_codec_ctx) { avcodec_free_context(&g_codec_ctx); g_codec_ctx = NULL; }

    for (int i = 0; i < 2; i++) {
        if (g_rgba_buf[i]) { free(g_rgba_buf[i]); g_rgba_buf[i] = NULL; }
    }
}

/* ============================================================================
 * ffmpeg_pipeline_abandon — Emergency cleanup after forced thread termination
 *
 * Nulls all global pointers WITHOUT freeing (leaked memory is acceptable
 * for a one-time watchdog recovery).  The old codec context, frames, and
 * RGBA buffers are abandoned in heap — typically ~2MB on PSP-1000.
 *
 * Caller must call ffmpeg_pipeline_init() after this to allocate fresh state.
 * The ME is killed and reinited so it's safe for the new pipeline.
 * ============================================================================*/

void ffmpeg_pipeline_abandon(void)
{
    diag_log_write("FFMPEG", "ABANDON: leaking old pipeline (decoded=%d dropped=%d)",
                   g_frames_decoded, g_frames_dropped);

    /* Kill ME (it might be mid-conversion from the terminated thread) */
    if (g_me_ctrl) {
        KillME(g_me_ctrl);
        diag_log_write("FFMPEG", "ABANDON: ME killed");
    }

    /* NULL out ALL pointers WITHOUT freeing (memory is leaked).
     * Total leak: ~2MB codec ctx + ~1MB RGBA bufs + ~256 bytes ME structs.
     * Acceptable for a one-time watchdog recovery on PSP-1000. */
    g_codec_ctx = NULL;
    g_avpkt = NULL;
    g_avframe[0] = NULL;
    g_avframe[1] = NULL;
    g_avframe_idx = 0;
    g_rgba_buf[0] = NULL;
    g_rgba_buf[1] = NULL;
    g_rgba_idx = 0;

    g_me_ctrl_cached = NULL;
    g_me_ctrl = NULL;
    g_me_params = NULL;
    g_me_available = 0;
    g_me_pending = 0;
    g_last_rgba = NULL;
    g_me_rgba_out = NULL;

    /* Reset decode state */
    g_frames_decoded = 0;
    g_frames_dropped = 0;
    g_got_first_frame = 0;
    g_saw_first_idr = 0;
    g_idr_fully_decoded = 0;
    g_refs_corrupted = 1;

    diag_log_write("FFMPEG", "ABANDON: all state nulled, ready for full reinit");
}

/* ============================================================================
 * ffmpeg_pipeline_decode_frame — Decode one Annex-B access unit
 *
 * Returns 0 on success, negative on error.
 * *out_rgba is set to the RGBA8888 output buffer on success.
 * ============================================================================*/

int ffmpeg_pipeline_decode_frame(const u8 *nal_data, int nal_len, u8 **out_rgba)
{
    static int s_frame_seq = 0;
    s_frame_seq++;

    if (!g_codec_ctx || !nal_data || nal_len <= 0 || !out_rgba) {
        return -1;
    }

    /* Snapshot and clear the per-frame corruption flag.  The RTP layer sets
     * this before invoking the decode callback (synchronous, same thread),
     * so the value is already stable by the time we read it here.  We must
     * clear it now so a corrupt P-frame's flag doesn't linger and cause the
     * next clean IDR to be wrongly rejected. */
    int this_frame_corrupt = g_current_frame_is_corrupt;
    g_current_frame_is_corrupt = 0;

    *out_rgba = NULL;
    u64 t_start, t_decode, t_convert;
    sceRtcGetCurrentTick(&t_start);

    /* Verbose diagnostics for first 5 frames after recovery */
    int diag_verbose = (g_frames_decoded >= 0 && g_frames_decoded <= 5);
    if (diag_verbose) {
        diag_log_write("FFMPEG", "DIAG F#%d enter me_pend=%d me_avail=%d",
                       g_frames_decoded, g_me_pending, g_me_available);
    }

    /* --- Step 1: Collect PREVIOUS frame from ME (if pipelined) ------------ */
    if (g_me_available && g_me_pending) {
        /* g_me_ctrl is uncached — CheckME reads directly from RAM, no flush needed.
         * ME postcache=-1 ensures dcache is flushed, so RGBA is in RAM when done=1.
         *
         * Optimized polling: tight spin for ~2000 iterations (~31µs typical ME time),
         * then yield 1µs per check to free CPU for other threads. */
        int pipe_loops = 0;
        while (!CheckME(g_me_ctrl) && pipe_loops < 5000000) {
            pipe_loops++;
            if (pipe_loops > 2000 && (pipe_loops % 500) == 0) {
                sceKernelDelayThread(1);  /* 1µs yield — frees CPU for network/USB */
            }
        }
        /* Invalidate CPU dcache for RGBA region — ME wrote via cached+flushed,
         * but CPU may have stale cache lines from previous display or memset. */
        sceKernelDcacheInvalidateRange(g_me_rgba_out, g_stream_res.rgba_size);
        if (pipe_loops >= 5000000) {
            /* ME timed out — likely crashed.  Attempt recovery. */
            diag_log_write("FFMPEG", "ME TIMEOUT pipeline (%d loops) -- resetting ME",
                           pipe_loops);
            KillME(g_me_ctrl);
            memset((void *)g_me_ctrl_cached, 0, sizeof(struct me_struct));
            sceKernelDcacheWritebackInvalidateAll();
            int reinit = InitME(g_me_ctrl);
            if (reinit == 0) {
                sceKernelDcacheWritebackInvalidateAll();
                int rl = 0;
                while (!g_me_ctrl->done && rl < 2000000) rl++;
                sceKernelDcacheWritebackInvalidateAll();
                if (g_me_ctrl->done) {
                    diag_log_write("FFMPEG", "ME recovered after pipeline timeout");
                } else {
                    diag_log_write("FFMPEG", "ME recovery failed -- disabling");
                    g_me_available = 0;
                }
            } else {
                diag_log_write("FFMPEG", "ME reinit after timeout failed: %d -- disabling", reinit);
                g_me_available = 0;
            }
        }
        g_last_rgba = g_me_rgba_out;
        g_me_pending = 0;
    }

    if (diag_verbose) {
        diag_log_write("FFMPEG", "DIAG F#%d step1-done, sending %d bytes",
                       g_frames_decoded, nal_len);
    }

    /* --- Step 1b: Skip non-IDR NALs before first SPS/PPS received ---------
     *
     * CRITICAL FIX: avcodec_send_packet() hangs on PSP's FFmpeg build when
     * given a non-IDR slice (NAL type 1/2/3/4) without prior SPS/PPS context.
     * The h264 decoder tries to parse slice headers referencing non-existent
     * parameter sets and enters an infinite loop.
     *
     * Scan for SPS (NAL type 7) start code in the frame data.  If no SPS
     * found and we haven't decoded any frames yet, skip this frame entirely
     * and return -5 (waiting for SPS/PPS).  The caller will request an IDR. */
    if (!g_got_first_frame) {
        int has_sps = 0;
        for (int i = 0; i < nal_len - 4; i++) {
            if (nal_data[i] == 0 && nal_data[i+1] == 0 &&
                (nal_data[i+2] == 1 || (nal_data[i+2] == 0 && nal_data[i+3] == 1))) {
                int nal_offset = (nal_data[i+2] == 1) ? i + 3 : i + 4;
                if (nal_offset < nal_len) {
                    int nal_type = nal_data[nal_offset] & 0x1F;
                    if (nal_type == 7) { /* SPS */
                        has_sps = 1;
                        break;
                    }
                }
            }
        }
        if (!has_sps) {
            /* No SPS in this frame.  If we have a cached SPS/PPS from a
             * prior decode session, inject it into the fresh codec context
             * so the decoder can resume without waiting for a new IDR. */
            if (s_cached_sps_pps_len > 0) {
                AVPacket sps_pkt;
                memset(&sps_pkt, 0, sizeof(sps_pkt));
                sps_pkt.data = s_cached_sps_pps;
                sps_pkt.size = s_cached_sps_pps_len;
                int sps_ret = avcodec_send_packet(g_codec_ctx, &sps_pkt);
                if (sps_ret >= 0) {
                    /* Drain any buffered frame (unlikely, SPS-only produces none) */
                    AVFrame *drain = g_avframe[g_avframe_idx];
                    avcodec_receive_frame(g_codec_ctx, drain);
                    g_got_first_frame = 1;
                    diag_log_write("FFMPEG", "Injected cached SPS/PPS (%d bytes) after restart -- codec ready",
                                   s_cached_sps_pps_len);
                    /* Fall through to decode the current frame normally */
                } else {
                    diag_log_write("FFMPEG", "WARN: cached SPS/PPS inject failed: %d (cache %d bytes)",
                                   sps_ret, s_cached_sps_pps_len);
                    return -5;
                }
            } else {
                /* No cached SPS/PPS either.  After 30 frames, generate synthetic
                 * SPS/PPS from stream parameters to break the deadlock. */
                static int s_no_sps_count = 0;
                s_no_sps_count++;
                if (s_no_sps_count >= 30 && g_stream_res.initialized) {
                    int synth_len = generate_synthetic_sps_pps(s_cached_sps_pps,
                                                                (int)sizeof(s_cached_sps_pps));
                    if (synth_len > 0) {
                        s_cached_sps_pps_len = synth_len;
                        AVPacket sps_pkt;
                        memset(&sps_pkt, 0, sizeof(sps_pkt));
                        sps_pkt.data = s_cached_sps_pps;
                        sps_pkt.size = s_cached_sps_pps_len;
                        int sps_ret = avcodec_send_packet(g_codec_ctx, &sps_pkt);
                        if (sps_ret >= 0) {
                            AVFrame *drain = g_avframe[g_avframe_idx];
                            avcodec_receive_frame(g_codec_ctx, drain);
                            g_got_first_frame = 1;
                            s_no_sps_count = 0;
                            diag_log_write("FFMPEG", "Injected SYNTHETIC SPS/PPS (%d bytes) -- codec ready",
                                           synth_len);
                            /* Fall through to decode current frame */
                        } else {
                            diag_log_write("FFMPEG", "Synthetic SPS/PPS inject failed: %d", sps_ret);
                            s_cached_sps_pps_len = 0;  /* Don't reuse bad synthetic */
                            return -5;
                        }
                    } else {
                        return -5;
                    }
                } else {
                    if (diag_verbose || s_frame_seq <= 5 || (s_no_sps_count % 30) == 0) {
                        diag_log_write("FFMPEG", "SKIP F#%d: no SPS in %d bytes (pre-IDR, no cache, wait=%d)",
                                       g_frames_decoded, nal_len, s_no_sps_count);
                    }
                    return -5;
                }
            }
        } else {
            diag_log_write("FFMPEG", "SPS found in F#%d (%d bytes) -- starting decode",
                           g_frames_decoded, nal_len);

            /* Cache raw SPS+PPS NALs for recovery after future force_restart.
             * Extract NAL type 7 (SPS) and type 8 (PPS) with start codes. */
            if (s_cached_sps_pps_len == 0) {
                int cache_pos = 0;
                int i = 0;
                while (i < nal_len - 4 && cache_pos < 200) {
                    int sc_len = 0;
                    if (nal_data[i] == 0 && nal_data[i+1] == 0) {
                        if (nal_data[i+2] == 1)
                            sc_len = 3;
                        else if (nal_data[i+2] == 0 && i + 3 < nal_len && nal_data[i+3] == 1)
                            sc_len = 4;
                    }
                    if (sc_len > 0) {
                        int ntype = nal_data[i + sc_len] & 0x1F;
                        if (ntype == 7 || ntype == 8) {
                            /* Find end of this NAL (next start code or EOF) */
                            int j = i + sc_len + 1;
                            while (j < nal_len - 3) {
                                if (nal_data[j] == 0 && nal_data[j+1] == 0 &&
                                    (nal_data[j+2] == 1 ||
                                     (nal_data[j+2] == 0 && j + 3 < nal_len && nal_data[j+3] == 1)))
                                    break;
                                j++;
                            }
                            int copy_len = j - i;
                            if (cache_pos + copy_len <= (int)sizeof(s_cached_sps_pps)) {
                                memcpy(s_cached_sps_pps + cache_pos, nal_data + i, copy_len);
                                cache_pos += copy_len;
                            }
                            i = j;
                            continue;
                        }
                        i += sc_len + 1;
                        continue;
                    }
                    i++;
                }
                if (cache_pos > 0) {
                    s_cached_sps_pps_len = cache_pos;
                    diag_log_write("FFMPEG", "SPS/PPS cached: %d bytes for restart recovery", cache_pos);
                }
            }
        }
    }

    /* --- Early skip: save CPU by not decoding P-frames during ref corruption --
     *
     * When refs are corrupted, ALL P-frames produce garbage regardless of
     * whether the frame itself is corrupt or clean.  Each avcodec decode
     * wastes ~20ms of ME time.  VQ#10 showed 1686/1720 frames returned -4
     * with 73% drop rate because the early-skip v3 only targeted clean
     * P-frames — but on lossy 802.11b, virtually ALL frames during ref
     * corruption also have seq gaps (this_frame_corrupt=1), making v3 a no-op.
     *
     * Strategy: detect frame type using the Access Unit Delimiter (AUD) at
     * the very start of the assembled frame buffer.  The AUD is always in
     * the FIRST RTP packet of each frame:
     *   - If first packet present: bytes 0-5 = 00 00 00 01 09 XX
     *     where (XX >> 5) == 0 → I-only (IDR) → let through
     *     where (XX >> 5) > 0  → I+P / I+P+B (P-frame) → SKIP
     *   - If first packet missing: AUD not at offset 0 → unknown type
     *     Use frame size heuristic: < 4000 bytes → likely P → skip
     *     >= 4000 bytes → might be IDR → let through
     *
     * This is safe because:
     *   - AUD detection only reads 6 bytes at a KNOWN offset (not deep scan)
     *   - VQ#9v2 freeze was caused by scanning 256 bytes of corrupt data for
     *     NAL type 5 — the IDR start codes were in missing packet sections
     *   - Here, if the first packet is missing, we KNOW and use safe fallback
     *   - IDRs at 480x272@500kbps are typically 2000-40KB
     *   - P-frames are typically 200-1500 bytes (1-2 packets)
     */
    /* Zero-artifact early-skip: skip ALL non-IDR frames during ref corruption.
     *
     * When references are corrupted, ALL P-frames produce visual garbage
     * (green/magenta banding, ghosting) because they motion-compensate from
     * corrupted DPB pictures — even "clean" P-frames with no seq gaps.
     * Decoding them wastes ~20ms ME time AND produces artifacts.
     *
     * Strategy: when g_refs_corrupted, only let IDR frames through.
     * Display holds the last clean frame (g_last_rgba) automatically.
     * Clean IDR decode resets DPB and clears g_refs_corrupted.
     *
     * History: VQ#14 (always-skip at 300kbps) was 1.8fps because IDRs were
     * rare.  At 500kbps with improved FEC, IDRs arrive every 5-15s which
     * produces acceptable freeze-then-refresh behavior vs constant artifacts.
     *
     * IDR detection via AUD at byte 0:
     *   - Bytes 0-5 = 00 00 00 01 09 XX where (XX>>5)==0 → IDR → let through
     *   - First packet missing + frame >= 4000 bytes → might be IDR → let through
     *   - Everything else → non-IDR → skip */
    static int me_stressed = 0;
    static int s_consec_decode_errs = 0;

    /* Watchdog restart: reset stale static counters from previous pipeline. */
    if (g_decode_counters_reset_pending) {
        me_stressed = 0;
        s_frame_seq = 0;
        s_ref_skip_count = 0;
        s_consec_decode_errs = 0;
        g_decode_counters_reset_pending = 0;
        diag_log_write("FFMPEG", "decode counters reset (watchdog restart)");
    }

    if (g_refs_corrupted) {
        int is_idr = 0;
        int desperate = (s_ref_skip_count >= DESPERATE_THRESHOLD);

        /* Check for AUD start code at byte offset 0 (first RTP packet present) */
        if (nal_len >= 6 &&
            nal_data[0] == 0x00 && nal_data[1] == 0x00 &&
            nal_data[2] == 0x00 && nal_data[3] == 0x01 &&
            (nal_data[4] & 0x1F) == 9) {
            int primary_pic_type = (nal_data[5] >> 5) & 0x07;
            if (primary_pic_type == 0) {
                is_idr = 1;  /* I-only (IDR) — let through to restore refs */
            }
        } else if (nal_len >= 2000) {
            /* No AUD at offset 0 — first packet likely missing.
             * Large frame might be an IDR — let through conservatively. */
            is_idr = 1;
        }

        if (!is_idr) {
            s_ref_skip_count++;

            /* Desperate recovery: after DESPERATE_THRESHOLD consecutive skips
             * (~2s blackout), stop waiting for a clean IDR that will never
             * arrive on high-loss 802.11b.  Let P-frames through to FFmpeg —
             * OUTPUT_CORRUPT flag produces best-effort decode with error
             * concealment.  Glitchy video beats permanent black screen. */
            if (desperate) {
                if (s_ref_skip_count == DESPERATE_THRESHOLD ||
                    (s_ref_skip_count % 300) == 0) {
                    diag_log_write("FFMPEG", "DESPERATE #%d: bypassing REF-SKIP, decoding %d bytes (corrupt=%d)",
                                   s_ref_skip_count, nal_len, this_frame_corrupt);
                }
                /* Fall through to Step 2 (decode) instead of returning -4 */
            } else {
                if (s_ref_skip_count <= 3 || (s_ref_skip_count % 100) == 0) {
                    diag_log_write("FFMPEG", "REF-SKIP #%d: non-IDR %d bytes (corrupt=%d) -- holding last clean frame",
                                   s_ref_skip_count, nal_len, this_frame_corrupt);
                }
                return -4;
            }
        }
    }

    /* --- Step 2: FFmpeg decode current frame ------------------------------- */

    /* Skip deblocking and IDCT aggressively for ALL frames during ref
     * corruption — these frames are garbage anyway.  For clean streams,
     * deblocking is already disabled globally (codec init) for max perf. */
    if (g_refs_corrupted) {
        /* During ref corruption, skip everything aggressively — decoded
         * output is garbage anyway.  Only IDRs matter (they clear refs). */
        g_codec_ctx->skip_loop_filter = AVDISCARD_ALL;
        g_codec_ctx->skip_idct = AVDISCARD_ALL;
        g_codec_ctx->skip_frame = AVDISCARD_DEFAULT;  /* Still decode IDRs */
    } else {
        /* Normal path: deblocking disabled for max perf (set at init),
         * skip non-reference frames to reduce CPU load. */
        g_codec_ctx->skip_loop_filter = AVDISCARD_ALL;
        g_codec_ctx->skip_idct = AVDISCARD_NONREF;
        g_codec_ctx->skip_frame = AVDISCARD_NONREF;
    }

    /* Feed Annex-B data to FFmpeg */
    g_avpkt->data = (uint8_t *)nal_data;
    g_avpkt->size = nal_len;

    int ret = avcodec_send_packet(g_codec_ctx, g_avpkt);
    if (ret < 0) {
        if (ret == AVERROR(EAGAIN)) {
            /* Codec internal buffer full — drain one frame then retry send */
            diag_log_write("FFMPEG", "DECODE#%d send EAGAIN, draining...", s_frame_seq);
            AVFrame *drain_frame = g_avframe[g_avframe_idx];
            avcodec_receive_frame(g_codec_ctx, drain_frame);
            ret = avcodec_send_packet(g_codec_ctx, g_avpkt);
            if (ret < 0 && ret != AVERROR(EAGAIN)) {
                g_frames_dropped++;
                s_consec_decode_errs++;
                if (s_consec_decode_errs >= 3) {
                    g_refs_corrupted = 1;
                    g_idr_fully_decoded = 0;
                }
                diag_log_write("FFMPEG", "DECODE#%d send retry failed: %d (consec=%d)", s_frame_seq, ret, s_consec_decode_errs);
                return -4;
            }
        } else if (ret == AVERROR_INVALIDDATA) {
            g_frames_dropped++;
            s_consec_decode_errs++;
            if (s_consec_decode_errs >= 3) {
                g_refs_corrupted = 1;
                g_idr_fully_decoded = 0;
            }
            if ((g_frames_dropped % 30) == 1 || s_consec_decode_errs <= 3) {
                diag_log_write("FFMPEG", "DECODE#%d invalid data (dropped=%d consec=%d)",
                               s_frame_seq, g_frames_dropped, s_consec_decode_errs);
            }
            return -4;
        } else {
            g_frames_dropped++;
            s_consec_decode_errs++;
            if (s_consec_decode_errs >= 3) {
                g_refs_corrupted = 1;
                g_idr_fully_decoded = 0;
            }
            diag_log_write("FFMPEG", "DECODE#%d send_packet error: %d (consec=%d)", s_frame_seq, ret, s_consec_decode_errs);
            return -4;
        }
    }

    /* Retrieve decoded YUV frame into current double-buffer slot */
    AVFrame *cur_frame = g_avframe[g_avframe_idx];
    u64 t_before_receive;
    sceRtcGetCurrentTick(&t_before_receive);
    ret = avcodec_receive_frame(g_codec_ctx, cur_frame);

    /* Timing: t_decode measures actual FFmpeg decode (send + receive) */
    sceRtcGetCurrentTick(&t_decode);

    /* ME stress detection: if receive_frame took >500ms the ME hardware
     * is struggling with corrupt NAL data.  Fires regardless of ret —
     * the ME can spend 5s on corrupt data and still 'succeed' (ret>=0)
     * with garbage output.  Skip subsequent corrupt P-frames until a
     * clean IDR resets things. */
    {
        u64 receive_us = t_decode - t_before_receive;
        if (receive_us > 500000) {
            if (!me_stressed) {
                diag_log_write("FFMPEG", "ME STRESS (reactive): receive took %u us (ret=%d) -- skipping corrupt frames",
                               (unsigned)(receive_us & 0xFFFFFFFF), ret);
            }
            me_stressed = 1;
        }
    }

    if (ret == AVERROR(EAGAIN)) {
        /* No frame ready yet — FFmpeg is buffering */
        if (g_last_rgba) { *out_rgba = g_last_rgba; return 0; }
        return -5;
    } else if (ret < 0) {
        g_frames_dropped++;
        s_consec_decode_errs++;
        if (s_consec_decode_errs >= 3) {
            g_refs_corrupted = 1;
            g_idr_fully_decoded = 0;
        }
        diag_log_write("FFMPEG", "DECODE#%d receive_frame error: %d (consec=%d)", s_frame_seq, ret, s_consec_decode_errs);
        return -4;
    }

    /* Frame decoded! Select RGBA output buffer */
    u8 *rgba_out = g_rgba_buf[g_rgba_idx];
    int width  = cur_frame->width  ? cur_frame->width  : 480;
    int height = cur_frame->height ? cur_frame->height : 272;

    /* Successful decode — reset consecutive-error counter */
    g_frames_dropped = 0;
    s_consec_decode_errs = 0;

    /* Capture desperate state BEFORE resetting skip count.
     * The post-decode guard needs to know if we entered via desperate mode. */
    int desperate_at_entry = (s_ref_skip_count >= DESPERATE_THRESHOLD);

    /* Reference corruption guard: after a decode error, P-frames decoded
     * with wrong references look visually corrupt (banding, ghosting).
     * Hold the last good frame on screen until a clean IDR resets refs.
     *
     * Desperate recovery: when desperate_at_entry is true, accept ANY decoded
     * output to break permanent black screen on lossy 802.11b WiFi where
     * clean IDRs never arrive. */
    if (g_refs_corrupted) {
        if (cur_frame->key_frame) {
            if (this_frame_corrupt && !desperate_at_entry) {
                /* This IDR itself had packet loss (seq gaps / RS failure).
                 * Its macroblocks may be partially garbage.  Do NOT clear
                 * g_refs_corrupted — wait for a clean IDR instead.
                 * In desperate mode, accept it anyway (glitchy > black). */
                g_idr_fully_decoded = 0;
                diag_log_write("FFMPEG", "Corrupt IDR F#%d -- skipping, refs still bad",
                               g_frames_decoded);
                return -4;
            }
            /* Clean IDR or desperate-mode corrupt IDR — accept and clear refs */
            g_refs_corrupted = 0;
            g_idr_fully_decoded = 1;
            me_stressed = 0;
            s_ref_skip_count = 0;
            if (this_frame_corrupt) {
                diag_log_write("FFMPEG", "IDR DESPERATE-ACCEPT F#%d %dx%d (corrupt but displayed)",
                               g_frames_decoded, width, height);
            } else {
                diag_log_write("FFMPEG", "IDR decoded -- refs restored (F#%d %dx%d)",
                               g_frames_decoded, width, height);
            }
        } else if (desperate_at_entry) {
            /* Desperate mode: P-frame during ref corruption.  Display it —
             * FFmpeg with OUTPUT_CORRUPT produces best-effort decode.
             * Artifacts are acceptable vs permanent black screen.
             * Reset skip count so we don't stay in desperate mode forever
             * if clean IDRs eventually arrive. */
            s_ref_skip_count = 0;
            diag_log_write("FFMPEG", "P-FRAME DESPERATE-ACCEPT F#%d %dx%d (lossy recovery)",
                           g_frames_decoded, width, height);
            /* Fall through to YUV→RGBA conversion */
        } else {
            /* P-frame reached decode despite ref corruption (should be rare
             * with the zero-artifact early-skip).  Do NOT display — hold the
             * last clean frame on screen to avoid visual artifacts.  The frame
             * was decoded to keep FFmpeg's DPB consistent. */
            g_idr_fully_decoded = 0;
            g_frames_decoded++;
            if (g_last_rgba) {
                *out_rgba = g_last_rgba;
                return 0;
            }
            return -4;
        }
    }

    /* Normal path: clean frame decoded, reset skip count */
    s_ref_skip_count = 0;

    if (diag_verbose) {
        diag_log_write("FFMPEG", "DIAG F#%d step2-done %dx%d idx=%d/%d rgba=%p",
                       g_frames_decoded, width, height, g_avframe_idx, g_rgba_idx, rgba_out);
    }

    /* --- Step 3: Dispatch YUV→RGBA conversion ----------------------------- */

    if (g_me_available) {
        /* Dual-core path: zero-copy — pass FFmpeg frame pointers directly to ME.
         * Double-buffered AVFrames ensure ME reads stable data: ME reads from
         * cur_frame while next decode writes to g_avframe[1-g_avframe_idx].
         * Reference counting in FFmpeg's buffer pool prevents buffer reuse. */

        /* Fill ME dispatch parameters with FFmpeg's native strides */
        g_me_params->y_plane = cur_frame->data[0];
        g_me_params->y_stride = cur_frame->linesize[0];
        g_me_params->u_plane = cur_frame->data[1];
        g_me_params->u_stride = cur_frame->linesize[1];
        g_me_params->v_plane = cur_frame->data[2];
        g_me_params->v_stride = cur_frame->linesize[2];
        g_me_params->rgba_out = rgba_out;
        g_me_params->rgba_stride_pixels = g_stream_res.stride;
        g_me_params->width = width;
        g_me_params->height = height;

        /* Log strides once to verify */
        if (g_frames_decoded == 1) {
            diag_log_write("FFMPEG", "STRIDE y=%d u=%d v=%d (native, zero-copy)",
                           cur_frame->linesize[0], cur_frame->linesize[1],
                           cur_frame->linesize[2]);
        }

        /* Flush main CPU dcache so ME sees YUV data + params in RAM.
         * Targeted flush: only writeback the specific buffers ME will read.
         * Saves ~15µs vs WritebackInvalidateAll + avoids evicting decoder
         * working set (~20-40µs of compulsory misses on next frame). */
        sceKernelDcacheWritebackRange(g_me_params, sizeof(MeYuv2RgbaParams));
        sceKernelDcacheWritebackRange((void *)cur_frame->data[0],
            cur_frame->linesize[0] * height);
        sceKernelDcacheWritebackRange((void *)cur_frame->data[1],
            cur_frame->linesize[1] * ((height + 1) / 2));
        sceKernelDcacheWritebackRange((void *)cur_frame->data[2],
            cur_frame->linesize[2] * ((height + 1) / 2));

        /* ME failure/recovery counters (persist across frames) */
        static int s_me_fail_count = 0;
        static int s_me_reinit_count = 0;

        /* Dispatch to Media Engine with full dcache coherence:
         *   precache_len=-1  → ME invalidates all dcache before function
         *   postcache_len=-1 → ME flushes+invalidates after function
         * This matches sw_me_worker's proven pattern. */
        int me_begin_ret = BeginME(g_me_ctrl,
                (int)(unsigned int)me_yuv420_to_rgba_entry,
                (int)(unsigned int)g_me_params,
                -1, NULL,    /* precache: invalidate all ME dcache */
                -1, NULL);   /* postcache: flush+invalidate all ME dcache */

        if (me_begin_ret < 0) {
            /* ME still busy — spin-wait up to ~1ms for it to finish */
            int spin = 0;
            while (!g_me_ctrl->done && spin < 100000) {
                spin++;
            }
            if (g_me_ctrl->done) {
                /* ME finished during spin — retry dispatch */
                sceKernelDcacheWritebackInvalidateAll();
                me_begin_ret = BeginME(g_me_ctrl,
                        (int)(unsigned int)me_yuv420_to_rgba_entry,
                        (int)(unsigned int)g_me_params,
                        -1, NULL, -1, NULL);
            }
        }

        if (me_begin_ret < 0) {
            /* BeginME failed: ME likely crashed (done never set to 1).
             * Recovery: KillME + reinitialize, matching sw_me_worker pattern. */
            s_me_fail_count++;

            if (s_me_fail_count <= 3 && s_me_reinit_count < 10) {
                /* Attempt ME recovery */
                diag_log_write("FFMPEG", "BeginME failed (%d) done=%d cnt=%d -- resetting ME",
                               me_begin_ret, (int)g_me_ctrl->done, s_me_fail_count);
                KillME(g_me_ctrl);
                memset((void *)g_me_ctrl_cached, 0, sizeof(struct me_struct));
                sceKernelDcacheWritebackInvalidateAll();
                int reinit = InitME(g_me_ctrl);
                if (reinit == 0) {
                    sceKernelDcacheWritebackInvalidateAll();
                    int rl = 0;
                    while (!g_me_ctrl->done && rl < 2000000) rl++;
                    sceKernelDcacheWritebackInvalidateAll();
                    if (g_me_ctrl->done) {
                        /* ME recovered — retry dispatch */
                        s_me_reinit_count++;
                        me_begin_ret = BeginME(g_me_ctrl,
                                (int)(unsigned int)me_yuv420_to_rgba_entry,
                                (int)(unsigned int)g_me_params,
                                -1, NULL, -1, NULL);
                        if (me_begin_ret == 0) {
                            s_me_fail_count = 0; /* reset fail streak */
                            diag_log_write("FFMPEG", "ME recovered (reinit=%d)", s_me_reinit_count);
                        }
                    } else {
                        diag_log_write("FFMPEG", "ME reinit: thread not ready");
                    }
                } else {
                    diag_log_write("FFMPEG", "ME reinit failed: %d", reinit);
                }
            }
        }

        if (me_begin_ret < 0) {
            if (s_me_fail_count <= 5 || (s_me_fail_count % 60) == 0) {
                diag_log_write("FFMPEG", "BeginME failed (%d) done=%d cnt=%d -- CPU fallback",
                               me_begin_ret, (int)g_me_ctrl->done, s_me_fail_count);
            }
            yuv420_to_rgba(cur_frame->data[0], cur_frame->linesize[0],
                           cur_frame->data[1], cur_frame->linesize[1],
                           cur_frame->data[2], cur_frame->linesize[2],
                           rgba_out, g_stream_res.stride, width, height);
            sceRtcGetCurrentTick(&t_convert);
            g_rgba_idx = 1 - g_rgba_idx;
            g_avframe_idx = 1 - g_avframe_idx;
            g_frames_decoded++;
            g_last_rgba = rgba_out;  /* Keep pipeline consistent on re-entry */
            if (!g_got_first_frame) {
                g_got_first_frame = 1;
                g_saw_first_idr = 1;
                g_idr_fully_decoded = 1;
                diag_log_write("FFMPEG", "FIRST FRAME OK %dx%d fmt=%d [CPU-beginFail]",
                               cur_frame->width, cur_frame->height, cur_frame->format);
            }
            *out_rgba = rgba_out;
        } else {
        /* BeginME succeeded — pipeline mode */

        g_me_pending = 1;
        g_me_rgba_out = rgba_out;

        /* E-3: Reset ME reinit counter periodically on success.
         * Prevents permanent CPU fallback after 10 reinits across a long session. */
        s_me_fail_count = 0;
        if (g_frames_decoded % 1000 == 0) s_me_reinit_count = 0;

        /* Toggle double-buffer for next frame (both AVFrame and RGBA) */
        g_rgba_idx = 1 - g_rgba_idx;
        g_avframe_idx = 1 - g_avframe_idx;

        sceRtcGetCurrentTick(&t_convert);
        g_frames_decoded++;

        /* First frame: wait synchronously (no previous to return) */
        if (!g_got_first_frame) {
            /* Need to invalidate dcache to read ME's RGBA output */
            sceKernelDcacheInvalidateRange(rgba_out, g_stream_res.rgba_size);

            /* Timeout-protected WaitME with CPU yield for efficiency.
             * ME typically completes in ~31µs (~1000 loops). After initial
             * spin, yield CPU to avoid burning cycles. */
            int me_wait_loops = 0;
            while (!CheckME(g_me_ctrl) && me_wait_loops < 5000000) {
                me_wait_loops++;
                if (me_wait_loops > 2000 && (me_wait_loops % 1000) == 0) {
                    sceKernelDelayThread(1);  /* 1µs yield after initial spin */
                }
            }
            /* Invalidate only the RGBA buffer region to read ME output */
            sceKernelDcacheInvalidateRange(rgba_out, g_stream_res.rgba_size);

            if (me_wait_loops >= 5000000) {
                /* ME timed out — disable and fall back to CPU */
                diag_log_write("FFMPEG", "ME TIMEOUT on first frame (%d loops) -- disabling ME",
                               me_wait_loops);
                g_me_available = 0;
                g_me_pending = 0;
                /* CPU fallback for this frame */
                yuv420_to_rgba(cur_frame->data[0], cur_frame->linesize[0],
                               cur_frame->data[1], cur_frame->linesize[1],
                               cur_frame->data[2], cur_frame->linesize[2],
                               rgba_out, g_stream_res.stride, width, height);
            } else {
                diag_log_write("FFMPEG", "ME first frame completed in %d loops", me_wait_loops);
            }
            g_last_rgba = rgba_out;
            g_me_pending = 0;
            g_got_first_frame = 1;
            g_saw_first_idr = 1;
            g_idr_fully_decoded = 1;
            diag_log_write("FFMPEG", "FIRST FRAME OK %dx%d fmt=%d [%s]",
                           cur_frame->width, cur_frame->height, cur_frame->format,
                           g_me_available ? "ME" : "CPU-timeout");
            *out_rgba = rgba_out;
        } else {
            /* Pipelined: return PREVIOUS frame while ME converts current */
            *out_rgba = g_last_rgba;
        }
        } /* end BeginME success */
    } else {
        /* Fallback: single-core on main CPU */
        yuv420_to_rgba(cur_frame->data[0], cur_frame->linesize[0],
                       cur_frame->data[1], cur_frame->linesize[1],
                       cur_frame->data[2], cur_frame->linesize[2],
                       rgba_out, g_stream_res.stride, width, height);

        sceRtcGetCurrentTick(&t_convert);
        g_rgba_idx = 1 - g_rgba_idx;
        g_avframe_idx = 1 - g_avframe_idx;
        g_frames_decoded++;

        if (!g_got_first_frame) {
            g_got_first_frame = 1;
            g_saw_first_idr = 1;
            g_idr_fully_decoded = 1;
            diag_log_write("FFMPEG", "FIRST FRAME OK %dx%d fmt=%d [CPU]",
                           cur_frame->width, cur_frame->height, cur_frame->format);
        }

        *out_rgba = rgba_out;
    }

    /* Log performance every 30 frames.
     * Track actual ME vs CPU path usage (not just g_me_available flag). */
    {
        static int s_me_frames = 0;
        static int s_cpu_frames = 0;
        if (g_me_pending) s_me_frames++; else s_cpu_frames++;

        if ((g_frames_decoded % 30) == 0) {
            u32 decode_us = (u32)(t_decode - t_start);
            u32 convert_us = (u32)(t_convert - t_decode);
            u32 total_us = (u32)(t_convert - t_start);
            float fps = 1000000.0f / (float)(total_us ? total_us : 1);
            diag_log_write("FFMPEG", "PERF#%d %dx%d decode=%uus yuv2rgba=%uus total=%uus fps=%.1f [%s] me=%d cpu=%d",
                           g_frames_decoded, width, height, decode_us, convert_us, total_us, fps,
                           g_me_pending ? "ME" : "CPU", s_me_frames, s_cpu_frames);
            s_me_frames = 0;
            s_cpu_frames = 0;
        }
    }

    return (*out_rgba != NULL) ? 0 : -5;
}

/* ============================================================================
 * ffmpeg_pipeline_invalidate_refs — Request IDR internally
 *
 * FFmpeg manages its own reference frames, so we just flush the decoder.
 * ============================================================================*/

void ffmpeg_pipeline_invalidate_refs(void)
{
    if (g_codec_ctx) {
        avcodec_flush_buffers(g_codec_ctx);
        diag_log_write("FFMPEG", "Decoder flushed (references invalidated)");
    }
}

/* ============================================================================
 * ffmpeg_pipeline_flush_buffers — Pipeline reset for queue overrun recovery
 *
 * Called when the decoder falls behind and the packet queue overruns.
 * Resets FFmpeg's internal decode state so the next IDR frame can be
 * decoded cleanly.  KEEPS g_got_first_frame = 1 so the ME pipeline
 * stays in efficient pipelined (parallel) mode.  The first frame after
 * flush returns out_rgba = NULL (g_last_rgba cleared), which the caller
 * treats as a skipped frame.  The second frame returns the first frame's
 * completed RGBA via the normal pipeline path.
 *
 * Previously, resetting g_got_first_frame = 0 here caused every
 * post-flush frame to enter the synchronous first-frame ME wait
 * (~20ms polling per frame), degrading throughput from 34fps to 20fps
 * and making the overrun cascade unrecoverable.
 * ============================================================================*/

void ffmpeg_pipeline_flush_buffers(void)
{
    /* Wait for any pending ME conversion before resetting */
    if (g_me_available && g_me_pending && g_me_ctrl) {
        int wait_loops = 0;
        while (!CheckME(g_me_ctrl) && wait_loops < 500000) {
            wait_loops++;
        }
        sceKernelDcacheInvalidateRange(g_me_rgba_out, g_stream_res.rgba_size);
    }

    if (g_codec_ctx) {
        avcodec_flush_buffers(g_codec_ctx);
    }

    g_last_rgba = NULL;
    g_me_pending = 0;
    g_me_rgba_out = NULL;
    g_refs_corrupted = 0;
    s_ref_skip_count = 0;
    g_avframe_idx = 0; /* Reset double-buffer indices for clean state (D-3) */
    g_rgba_idx = 0;
    /* NOTE: g_got_first_frame intentionally NOT reset.
     * Keeps ME pipeline in efficient parallel mode. First post-flush
     * frame returns NULL (skipped), second frame returns valid RGBA. */

    diag_log_write("FFMPEG", "Pipeline flushed (pipelined mode preserved)");
}
