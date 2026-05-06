/*
 * sw_decoder_thread.c - Software H.264 Decoder Thread (Dual-Core Pipeline)
 *
 * This is the ONLY decoder thread. No sceMpeg, no JPEG fallback, no MPEG-PS
 * wrapping. Pure software H.264 Baseline decode through the asymmetric
 * dual-core pipeline:
 *
 *   Core 1 (Main CPU) — "Front End":
 *     Drains packets from PacketRingBuffer → RTP reassembly →
 *     Annex-B NAL extraction → CAVLC entropy decode (sequential) →
 *     Writes coefficients + motion vectors to shared SwPipelineState
 *
 *   Core 2 (Media Engine) — "Heavy Lifter":
 *     Reads SwPipelineState → VFPU dequant+IDCT → intra/inter prediction →
 *     motion compensation → YUV420→RGBA8888 → FrameRingBuffer
 *
 * No hardware decode libraries are loaded. Memory saved:
 *   - No sceMpeg context (~512KB ringbuffer + control structs)
 *   - No JPEG decoder buffers
 *   - No MPEG-PS staging buffer
 *   - No AV codec PRX modules loaded
 */

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspsdk.h>
#include <psprtc.h>
#include <string.h>
#include <stdio.h>

#include "sw_decode_pipeline.h"
#include "stream_resolution.h"
#include "rtp_reassembly.h"
#include "rtp_fec.h"
#include "shared.h"
#include "diag_log.h"
#include "control_stream.h"
#include "settings_menu.h"  /* PspConfig */
#include "decode_flags.h"
#include "safety_buffer.h"

extern PspConfig g_psp_config;

/* OpenH264-based decode pipeline (native low-latency H.264 decoder) */
extern int  oh264_pipeline_init(void);
extern void oh264_pipeline_shutdown(void);
extern int  oh264_pipeline_decode_frame(const u8 *nal_data, int nal_len, u8 **out_rgba);
extern void oh264_pipeline_invalidate_refs(void);
extern void oh264_pipeline_flush_buffers(void);
extern int  oh264_pipeline_reset_codec(void);
extern void oh264_pipeline_abandon(void);

/* Forward declaration — lightweight CABAC detection */
static void check_nal_for_cabac(const u8 *nal_data, int nal_len);
static int s_pps_checked = 0;  /* PPS scan state — reset in init for new streams */

/* CABAC dialog gate — main thread sets to 1 while dialog is on-screen */
volatile int g_cabac_dialog_active = 0;

/* ============================================================================
 * Constants
 * ============================================================================*/

#define FRAME_POOL_SIZE     4
#define FRAME_BUF_SIZE      (PSP_LCD_STRIDE * PSP_LCD_HEIGHT * PIXEL_SIZE)

#define PACKET_BACKLOG_PSKIP_CAVLC     192u
#define PACKET_BACKLOG_PSKIP_CABAC      96u
#define PACKET_BACKLOG_FLUSH_CAVLC     384u
#define PACKET_BACKLOG_FLUSH_CABAC     224u
#define DECODER_DRAIN_BATCH_LIMIT      128
#define SW_DECODER_THREAD_PRIORITY     0x21

#define dec_log(fmt, ...) diag_log_write("DEC", fmt, ##__VA_ARGS__)

/* ============================================================================
 * Module State
 * ============================================================================*/

static SceUID           g_dec_thread_id = -1;
static SceUID           g_dec_sema = -1;
static volatile int     g_dec_running = 0;

/* These are now defined in openh264_decode.cpp */
extern int              g_saw_first_idr;
extern volatile int     g_idr_fully_decoded;
int                     g_decoder_ready = 0;

static int              g_frames_decoded = 0;
static int              g_frames_dropped = 0;
static FrameRingBuffer *g_frame_rb = NULL;
static PacketRingBuffer *g_packet_rb = NULL;

static void nal_scan_headers(const u8 *nal_data, int nal_len,
                             int *out_has_idr, int *out_has_sync_nal)
{
    int has_idr = 0;
    int has_sync_nal = 0;
    const u8 *p;
    const u8 *end;

    if (!nal_data || nal_len <= 4) {
        if (out_has_idr) *out_has_idr = 0;
        if (out_has_sync_nal) *out_has_sync_nal = 0;
        return;
    }

    p = nal_data;
    end = nal_data + nal_len;
    while (p + 4 < end) {
        int sc_len = 0;

        if (p[0] == 0 && p[1] == 0) {
            if (p[2] == 1) sc_len = 3;
            else if (p + 4 < end && p[2] == 0 && p[3] == 1) sc_len = 4;
        }

        if (sc_len > 0 && p + sc_len < end) {
            int nal_type = p[sc_len] & 0x1F;
            if (nal_type == 5) {
                has_idr = 1;
                has_sync_nal = 1;
                break;
            }
            if (nal_type == 7 || nal_type == 8) {
                has_sync_nal = 1;
            }

            /* Stop once we hit the first VCL NAL in non-IDR units.
             * This keeps scanning cheap on P-frames while still allowing
             * large SPS/PPS/SEI preambles before IDR in CABAC streams. */
            if (nal_type >= 1 && nal_type <= 5) {
                break;
            }

            p += sc_len;
        }

        p++;
    }

    if (out_has_idr) *out_has_idr = has_idr;
    if (out_has_sync_nal) *out_has_sync_nal = has_sync_nal;
}

static u32 decoder_pskip_threshold_packets(void)
{
    return g_cabac_detected ? PACKET_BACKLOG_PSKIP_CABAC
                            : PACKET_BACKLOG_PSKIP_CAVLC;
}

static u32 decoder_flush_threshold_packets(void)
{
    return g_cabac_detected ? PACKET_BACKLOG_FLUSH_CABAC
                            : PACKET_BACKLOG_FLUSH_CAVLC;
}

/* Timestamp of last decoded frame — read by main loop for latency display */
volatile u32 g_last_frame_decode_us = 0;

/* Per-frame decode duration (µs) — smoothed EMA for HUD display */
volatile u32 g_decode_time_us = 0;

/* Watchdog: set to sceKernelGetSystemTimeLow() before oh264_pipeline_decode_frame(),
 * cleared to 0 after it returns.  Main loop checks: if non-zero and elapsed > 3s,
 * the decode is hung and force-restart is triggered. */
volatile u32 g_decode_active_us = 0;

/* Alive counter: incremented on EVERY callback invocation (any return code).
 * Used by main-loop Mode B watchdog to distinguish "decoder alive but skipping
 * frames (REF-SKIP)" from "decoder thread truly stuck".  Without this,
 * the zero-artifact REF-SKIP policy causes false force_restarts. */
volatile int g_decoder_alive_counter = 0;

/* ============================================================================
 * push_sw_frame — Copy RGBA output to frame pool, push to ring
 * ============================================================================*/

static void push_sw_frame(u8 *rgba_frame)
{
    if (!g_frame_rb || !rgba_frame) return;

    /* Zero-copy: push orchestrator's RGBA double-buffer pointer directly.
     * The orchestrator alternates g_rgba_buf[0]/[1], so the previous
     * frame's buffer stays valid until the next-next decode (~186ms).
     * display_frame() calls sceKernelDcacheWritebackInvalidateAll()
     * + sceGuSync(0,0) before GPU upload, ensuring coherency. */
    u32 h = g_frame_rb->head;
    u32 n = (h + 1) % FRAME_RING_SLOTS;
    if (n != g_frame_rb->tail) {
        g_frame_rb->frame_data[h] = rgba_frame;
        g_frame_rb->head = n;
        g_frame_rb->frame_ready = 1;
        g_last_frame_decode_us = sceKernelGetSystemTimeLow();
    } else {
        static int s_drop_count = 0;
        s_drop_count++;
        if (s_drop_count <= 5 || (s_drop_count % 120) == 0) {
            dec_log("RING_FULL drop=%d h=%u t=%u\n",
                    s_drop_count, (unsigned)h, (unsigned)g_frame_rb->tail);
        }
    }

    g_frames_decoded++;
}

/* ============================================================================
 * RTP Reassembly Callback — Called when a complete H.264 access unit is ready
 *
 * This is invoked from rtp_reassembly_process_packet() when all fragments
 * of a frame have been collected into a contiguous Annex-B buffer.
 * ============================================================================*/

void rtp_frame_complete_callback(const u8 *nal_data, int nal_len)
{
    static u64 s_prev_done = 0;
    static int s_perf_count = 0;
    static int s_wait_count = 0;
    static int s_no_output_streak = 0;
    static int s_cb_count = 0;
    static u32 s_startup_pskip_count = 0;
    static u32 s_last_usb_yield_us = 0;
    const u8 *decode_nal;
    int decode_len;
    int decode_has_idr = 0;
    int decode_has_sync_nal = 0;

    if (!nal_data || nal_len <= 0) return;

    decode_nal = nal_data;
    decode_len = nal_len;

    nal_scan_headers(nal_data, nal_len, &decode_has_idr, &decode_has_sync_nal);

    {
        u64 now_pts = 0;
        sceRtcGetCurrentTick(&now_pts);
        safety_buffer_store_nal((u8 *)nal_data, (u32)nal_len, now_pts, (u8)decode_has_idr);
    }

    if (safety_buffer_get_state() == SAFETY_BUFFER_REWIND && safety_buffer_can_rewind()) {
        u32 rewind_len = 0;
        u64 rewind_pts = 0;
        u8 *rewind_nal = safety_buffer_rewind(&rewind_len, &rewind_pts);
        if (rewind_nal && rewind_len > 0) {
            decode_nal = rewind_nal;
            decode_len = (int)rewind_len;
            nal_scan_headers(decode_nal, decode_len, &decode_has_idr, &decode_has_sync_nal);
            dec_log("SAFETY: rewind replay len=%u pts=%u\n",
                    (unsigned)rewind_len, (unsigned)rewind_pts);
        }
    }

    /* Watchdog restart: reset stale callback counters so logging and
     * IDR request cadence start fresh after a pipeline reinit. */
    {
        extern volatile int g_decode_counters_reset_pending;
        if (g_decode_counters_reset_pending) {
            s_prev_done = 0;
            s_perf_count = 0;
            s_wait_count = 0;
            s_no_output_streak = 0;
            s_cb_count = 0;
            s_startup_pskip_count = 0;
            s_last_usb_yield_us = 0;
            g_decode_counters_reset_pending = 0;
        }
    }

    /* Pause gate: avoid blocking the decode callback while paused.
     * Blocking here stalls ring draining, which causes packet backlog,
     * queue overrun drops, and watchdog restarts on resume. */
    {
        extern volatile int g_decode_paused;
        static int s_pause_active = 0;
        static int s_pause_drop_count = 0;

        if (g_decode_paused) {
            if (!s_pause_active) {
                s_pause_active = 1;
                s_pause_drop_count = 0;
                dec_log("PAUSE: decode paused, dropping frames to avoid backlog\n");
            }

            s_pause_drop_count++;
            if (s_pause_drop_count <= 3 || (s_pause_drop_count % 120) == 0) {
                dec_log("PAUSE: dropped frame #%d len=%d\n",
                        s_pause_drop_count, decode_len);
            }
            return;
        }

        if (s_pause_active) {
            s_pause_active = 0;
            dec_log("PAUSE: resume detected, flushing stale state + requesting IDR\n");

            if (g_packet_rb) {
                g_packet_rb->tail = g_packet_rb->head;
            }
            rtp_reassembly_reset();
            rtp_fec_reset();
            oh264_pipeline_flush_buffers();
            {
                extern volatile unsigned int g_last_good_frame;
                g_last_good_frame = 0;
            }
            control_stream_request_idr();
            s_wait_count = 0;
            s_no_output_streak = 0;
        }
    }

    s_cb_count++;
    g_decoder_alive_counter++;
    /* Log every 120th callback to minimize I/O overhead in hot path */
    if (s_cb_count <= 3 || (s_cb_count % 120) == 0) {
        dec_log("CB#%d len=%d\n", s_cb_count, decode_len);
    }

    /* Time-based USB yield: yield 1ms every 500ms to keep psplink responsive
     * WITHOUT burning per-frame CPU. Old approach: 4ms/frame = 240ms/s at 60fps!
     * New approach: 1ms per 500ms = 2ms/s regardless of FPS. */
    {
        u32 now_us = sceKernelGetSystemTimeLow();
        if (now_us - s_last_usb_yield_us > 500000) { /* 500ms */
            sceKernelDelayThread(1000);  /* 1ms yield */
            s_last_usb_yield_us = now_us;
        }
    }

    /* Dump first 16 bytes to diagnose NAL structure (first 3 frames only) */
    if (s_cb_count <= 3) {
        char hex[49]; int hx = decode_len < 16 ? decode_len : 16;
        for (int hi = 0; hi < hx; hi++)
            snprintf(hex + hi * 3, 4, "%02X ", decode_nal[hi]);
        hex[hx * 3] = '\0';
        dec_log("CB#%d hex: %s\n", s_cb_count, hex);
    }

    /* P-frame skip on overload: when ring queue depth exceeds 256 packets
     * (~85 frames at 3 pkt/frame), skip P-frames and only decode IDR/SPS/PPS.
     * This lets the decoder "jump ahead" to the latest IDR instead of
     * decoding 85+ stale P-frames that will be immediately discarded.
     * Saves ~5-40ms per skipped frame of decode time on 333MHz PSP. */
    if (g_packet_rb) {
        u32 queued = (g_packet_rb->head + RING_BUFFER_SLOTS
                      - g_packet_rb->tail) % RING_BUFFER_SLOTS;
        u32 pskip_threshold = decoder_pskip_threshold_packets();

        if (queued > pskip_threshold && decode_len > 4) {
            if (!decode_has_sync_nal) {
                static u32 s_pskip_count = 0;
                s_pskip_count++;
                if (s_pskip_count <= 3 || (s_pskip_count % 50) == 0) {
                    dec_log("P-SKIP: q=%u>%u skip=%u len=%d waiting for sync\n",
                            (unsigned)queued, (unsigned)pskip_threshold,
                            (unsigned)s_pskip_count, decode_len);
                }
                return;  /* skip this P-frame */
            } else {
                /* IDR arrived while queue was overloaded — log recovery */
                dec_log("P-SKIP: sync NAL arrived, resuming decode (q=%u thr=%u)\n",
                        (unsigned)queued, (unsigned)pskip_threshold);
            }
        }
    }

    if (!g_saw_first_idr && !decode_has_sync_nal) {
        s_startup_pskip_count++;
        if (s_startup_pskip_count <= 3 || (s_startup_pskip_count % 60) == 0) {
            dec_log("STARTUP P-SKIP: skip=%u len=%d waiting for IDR/SPS\n",
                    (unsigned)s_startup_pskip_count, decode_len);
        }
        if ((s_startup_pskip_count % 30) == 1) {
            control_stream_request_idr();
        }
        g_frames_dropped++;
        return;
    } else if (!g_saw_first_idr && decode_has_sync_nal && s_startup_pskip_count > 0) {
        dec_log("STARTUP P-SKIP: sync NAL arrived after %u skips\n",
                (unsigned)s_startup_pskip_count);
        s_startup_pskip_count = 0;
    }

#ifdef MOONLIGHT_DEBUG_DUMP
    /* Debug-only raw dump: first frame >5KB (likely IDR) to ms0:/raw_dump.h264.
     * This captures data BEFORE CAVLC parsing so we can hex-inspect the full
     * frame even if SPS/PPS detection fails. */
    {
        static int raw_dumped = 0;
        if (!raw_dumped && nal_len > 5000) {
            SceUID fd = sceIoOpen("ms0:/raw_dump.h264",
                                  PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
            if (fd >= 0) {
                sceIoWrite(fd, nal_data, nal_len);
                sceIoClose(fd);
                dec_log("RAW DUMP: saved %d bytes to ms0:/raw_dump.h264\n", nal_len);
            }
            raw_dumped = 1;
        }
    }
#endif

    u64 t_cb_entry;
    sceRtcGetCurrentTick(&t_cb_entry);

    /* Check first PPS NAL for CABAC before feeding to OpenH264.
     * After first successful PPS check OR 10 frames (whichever first),
     * skip scanning entirely — saves ~50µs/frame of NAL traversal. */
    {
        static int s_cabac_check_count = 0;
        if (s_cb_count <= 1) s_cabac_check_count = 0;  /* reset on restart */
        if (!g_cabac_detected && s_cabac_check_count < 10) {
            check_nal_for_cabac(decode_nal, decode_len);
            s_cabac_check_count++;
        }
    }

    /* If CABAC dialog is on-screen, skip decoding entirely — the user
     * hasn't confirmed yet, so don't run the decoder/produce frames. */
    if (g_cabac_dialog_active) {
        return;
    }

    u8 *rgba_out = NULL;
    g_decode_active_us = sceKernelGetSystemTimeLow();
    int ret = oh264_pipeline_decode_frame(decode_nal, decode_len, &rgba_out);
    g_decode_active_us = 0;  /* decode finished — watchdog can relax */

    if (s_cb_count <= 3 || (s_cb_count % 120) == 0 || ret < 0) {
        dec_log("CB#%d ret=%d rgba=%s\n", s_cb_count, ret, rgba_out ? "OK" : "NULL");
    }

    if (ret == 0 && rgba_out) {
        push_sw_frame(rgba_out);
        s_startup_pskip_count = 0;
        s_wait_count = 0;  /* Reset — decode succeeded, SPS/PPS is valid */
        s_no_output_streak = 0;
        s_startup_pskip_count = 0;

        u64 t_cb_done;
        sceRtcGetCurrentTick(&t_cb_done);

        /* Update per-frame decode time (EMA α=0.2 for smoothing) */
        {
            u32 dec_us_raw = (u32)(t_cb_done - t_cb_entry);
            static float s_dec_ema = 0.0f;
            if (s_dec_ema < 1.0f) s_dec_ema = (float)dec_us_raw;
            else s_dec_ema = s_dec_ema * 0.8f + (float)dec_us_raw * 0.2f;
            g_decode_time_us = (u32)(s_dec_ema + 0.5f);
        }

        s_perf_count++;
        if (s_prev_done && (s_perf_count % 60) == 0) {
            u32 cycle_us = (u32)(t_cb_done - s_prev_done);
            u32 gap_us = (u32)(t_cb_entry - s_prev_done);
            u32 dec_us = (u32)(t_cb_done - t_cb_entry);
            u32 fps_x10 = (cycle_us > 0) ? (10000000u / cycle_us) : 0;
            dec_log("PERF n=%d cycle=%uus gap=%uus dec=%uus fps=%u.%u\n",
                    s_perf_count, cycle_us, gap_us, dec_us,
                    fps_x10 / 10, fps_x10 % 10);
        }
        s_prev_done = t_cb_done;

        /* Frame pacing DISABLED — at 256x144@30fps with 12-17ms decode
         * time, the PSP can barely sustain 15fps.  Any sleep here wastes
         * precious CPU cycles.  The main loop's "skip to newest" already
         * handles burst delivery by discarding stale frames, and
         * sceDisplayWaitVblankStart provides natural 60Hz pacing. */

        /* Flush stale RTP packets when decoder falls behind.
         * A single decode takes 100-500ms. At 15fps / ~3 pkt/frame,
         * that's 5-25 packets accumulating per decode — NORMAL.
         * Only flush when we are seriously behind (128+ packets ≈ 40+
         * frames at 3 pkt/frame ≈ 2.7 seconds of backlog).
         *
         * IMPORTANT: Do NOT invalidate reference frames here!
         * The just-decoded frame IS a valid reference.  Invalidating
         * it blocks ALL subsequent P-frames until the next IDR, which
         * Sunshine may not send for many seconds. Instead, keep the
         * reference valid, flush the stale packet backlog, and request
         * an IDR so the encoder can resync when convenient. */
        if (g_packet_rb) {
            u32 queued = (g_packet_rb->head + RING_BUFFER_SLOTS
                          - g_packet_rb->tail) % RING_BUFFER_SLOTS;
            u32 flush_threshold = decoder_flush_threshold_packets();

            if (queued > flush_threshold) {
                dec_log("queue overrun: %u stale packets (>%u), flushing\n",
                        (unsigned)queued, (unsigned)flush_threshold);
                g_packet_rb->tail = g_packet_rb->head;
                rtp_reassembly_reset();
                rtp_fec_reset();

                /* Flush OpenH264 decoder state so it can cleanly accept the
                 * next IDR.  Without this, the decoder stays in permanent error
                 * state after receiving corrupted partial NALs from the post-flush
                 * reassembly, and every callback returns ret=0 with the stale
                 * g_last_rgba → triggers another overrun → infinite loop. */
                oh264_pipeline_flush_buffers();

                /* Reset dedup counter so reassembly accepts fresh frames */
                {
                    extern volatile unsigned int g_last_good_frame;
                    g_last_good_frame = 0;
                }

                /* Request IDR — the skipped frames create a gap that
                 * P-frames cannot bridge cleanly.  An IDR will reset
                 * the reference chain.  Refs stay valid so P-frames
                 * that arrive before the IDR can still be decoded
                 * (they'll be blocky but visible). */
                control_stream_request_idr();
            }
        }
    } else if (ret == -5 || ret == -6) {
        /* -5: Waiting for SPS/PPS — not an error, just not ready yet
         * -6: P-frame without valid reference — expected after IDR failure
         * Request IDR every 30 consecutive failures so we don't wait forever. */
        s_wait_count++;
        s_no_output_streak++;
        if (s_wait_count <= 10 || (s_wait_count % 30) == 0) {
            dec_log("waiting: ret=%d (count=%d)\n", ret, s_wait_count);
            control_stream_request_idr();
        }
        if (ret == -5 && s_wait_count > 200) {
            /* After 200+ frames with no SPS/PPS, something is very wrong */
            dec_log("STUCK: %d frames without SPS/PPS, forcing IDR burst\n", s_wait_count);
            control_stream_request_idr();
        }
        if (!g_saw_first_idr && s_no_output_streak >= 24 && (s_no_output_streak % 24) == 0) {
            dec_log("STARTUP RECOVERY: no IDR after %d callbacks, flushing + IDR\n",
                    s_no_output_streak);
            if (g_packet_rb) {
                g_packet_rb->tail = g_packet_rb->head;
            }
            rtp_reassembly_reset();
            rtp_fec_reset();
            oh264_pipeline_reset_codec();
            {
                extern volatile unsigned int g_last_good_frame;
                g_last_good_frame = 0;
            }
            g_idr_fully_decoded = 0;
            g_saw_first_idr = 0;
            s_wait_count = 0;
            s_no_output_streak = 0;
            s_startup_pskip_count = 0;
            control_stream_request_idr();
        }
        /* Reset counter on any successful decode (handled above in ret==0 path) */
    } else if (ret < 0) {
        g_frames_dropped++;
        s_no_output_streak++;
        if (s_no_output_streak >= 18 && (s_no_output_streak == 18 || (s_no_output_streak % 60) == 0)) {
            dec_log("NO-OUTPUT RECOVERY: failures=%d ret=%d saw_idr=%d, codec reset + IDR\n",
                    s_no_output_streak, ret, g_saw_first_idr);
            if (g_packet_rb) {
                g_packet_rb->tail = g_packet_rb->head;
            }
            rtp_reassembly_reset();
            rtp_fec_reset();
            safety_buffer_clear();
            if (oh264_pipeline_reset_codec() < 0) {
                oh264_pipeline_flush_buffers();
            }
            {
                extern volatile unsigned int g_last_good_frame;
                g_last_good_frame = 0;
            }
            g_idr_fully_decoded = 0;
            g_saw_first_idr = 0;
            g_fec_requested_idr = 0;
            s_wait_count = 0;
            s_no_output_streak = 0;
            s_startup_pskip_count = 0;
            control_stream_request_idr();
        }
        if ((g_frames_dropped % 30) == 1) {
            dec_log("decode failed: %d (dropped=%d)\n", ret, g_frames_dropped);
        }
        /* Request IDR on decode failure — the current reference chain
         * may be broken, and a fresh keyframe resets everything.
         * Skip if FEC already requested IDR for this error event (C-2). */
        if (!g_fec_requested_idr) {
            control_stream_request_idr();
        }
        g_fec_requested_idr = 0;
    }
}

/* ============================================================================
 * Decoder Thread — Main loop
 *
 * Tight loop: drain packet ring → RTP reassembly → CAVLC+ME decode → display.
 * No JPEG queue. No sceMpeg. Single-purpose.
 * ============================================================================*/

static int sw_decoder_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;

    dec_log("SW decoder thread started (CAVLC+VFPU dual-core)\n");

    u32 last_heartbeat = 0;
    int loop_count = 0;

    while (g_dec_running) {
        int processed = 0;
        int batch = 0;
        loop_count++;

        /* Heartbeat every ~5 seconds */
        {
            u32 now = sceKernelGetSystemTimeLow() / 1000000;
            if (now - last_heartbeat >= 5) {
                u32 queued = 0;
                if (g_packet_rb)
                    queued = (g_packet_rb->head + RING_BUFFER_SLOTS
                              - g_packet_rb->tail) % RING_BUFFER_SLOTS;
                dec_log("HEARTBEAT loop=%d decoded=%d dropped=%d q=%u\n",
                        loop_count, g_frames_decoded, g_frames_dropped,
                        (unsigned)queued);
                last_heartbeat = now;
                diag_log_flush();
            }
        }

        /* Drain network packets through RTP reassembly.
         * rtp_reassembly_process_packet() will call back into
         * rtp_frame_complete_callback() when a full frame is ready. */
        while (g_packet_rb &&
               g_packet_rb->tail != g_packet_rb->head &&
               g_dec_running &&
               batch < DECODER_DRAIN_BATCH_LIMIT) {

            u16 pkt_len = g_packet_rb->slot_length[g_packet_rb->tail];
            if (pkt_len > 0) {
                /* Route through FEC first. If FEC consumes the packet
                 * (returns 1), it will call rtp_reassembly_process_packet()
                 * internally after frame recovery. If FEC returns 0,
                 * pass directly to reassembly (non-FEC or unrecognized). */
                if (!rtp_fec_add_packet(
                        g_packet_rb->slots[g_packet_rb->tail], pkt_len)) {
                    rtp_reassembly_process_packet(
                        g_packet_rb->slots[g_packet_rb->tail], pkt_len);
                }
                processed = 1;
            }
            g_packet_rb->tail =
                (g_packet_rb->tail + 1) % RING_BUFFER_SLOTS;
            batch++;
        }

        /* Ring overrun safety net: if the ring is >75% full after draining,
         * the consumer is falling behind the producer.  This happens when
         * ALL frames fail to decode (e.g. post-watchdog-restart waiting for
         * IDR) — the per-successful-decode flush in rtp_frame_complete_callback
         * never fires, so the ring silently fills until the producer drops
         * ALL incoming packets (including the IDR we need to recover).
         *
         * Fix: flush the ring and request IDR.  This loses buffered data but
         * makes room for the next IDR to arrive intact. */
        if (processed && g_packet_rb) {
            u32 queued = (g_packet_rb->head + RING_BUFFER_SLOTS
                          - g_packet_rb->tail) % RING_BUFFER_SLOTS;
            u32 flush_threshold = decoder_flush_threshold_packets();

            if (queued > flush_threshold) {
                dec_log("RING BACKLOG %u/1024 (>%u) -- flushing to prevent overflow\n",
                        (unsigned)queued, (unsigned)flush_threshold);
                g_packet_rb->tail = g_packet_rb->head;
                rtp_reassembly_reset();
                rtp_fec_reset();
                oh264_pipeline_flush_buffers();
                {
                    extern volatile unsigned int g_last_good_frame;
                    g_last_good_frame = 0;
                }
                control_stream_request_idr();
            }
        }

        /* Sleep if nothing to do — woken by sema signal from network thread.
         * Use 500ms timeout so heartbeats continue and decoder wakes quickly. */
        if (!processed && g_dec_running &&
            (!g_packet_rb ||
             g_packet_rb->tail == g_packet_rb->head)) {
            SceUInt timeout_us = 500 * 1000;  /* 500ms */
            sceKernelWaitSema(g_dec_sema, 1, &timeout_us);
        }
        /* No per-batch yield: USB access is handled by time-based yield in
         * rtp_frame_complete_callback() and the decode_paused mechanism.
         * The old 1ms yield per batch was costing 30-60ms/s at high FPS. */
    }

    dec_log("SW decoder thread exiting (decoded=%d dropped=%d)\n",
            g_frames_decoded, g_frames_dropped);
    return 0;
}

/* ============================================================================
 * Public API
 * ============================================================================*/

int sw_decoder_thread_init(FrameRingBuffer *rb)
{
    g_packet_rb = &g_shared.packet_ring;
    g_cabac_detected = 0;
    s_pps_checked = 0;     /* Allow re-detection on new stream */

    dec_log("Init: Software H.264 decode (CAVLC+VFPU dual-core)\n");

    /* Frame pool REMOVED — push_sw_frame uses zero-copy from OpenH264
     * double-buffer (g_rgba_buf[0/1]), saving ~2.2MB on PSP-1000. */
    dec_log("  Frame pool: DISABLED (zero-copy, saves %d KB)\n",
            (FRAME_POOL_SIZE * FRAME_BUF_SIZE) / 1024);

    /* Initialize unified resolution table from config — MUST happen before
     * oh264_pipeline_init() so all buffer allocations use consistent dimensions */
    stream_resolution_init(g_psp_config.width, g_psp_config.height);

    /* Initialize OpenH264 decode pipeline */
    int sw_res = oh264_pipeline_init();
    if (sw_res < 0) {
        dec_log("oh264_pipeline_init failed: %d\n", sw_res);
        return sw_res;
    }
    dec_log("  Pipeline ready (OpenH264 decoder, low-latency)\n");

    /* Initialize FEC recovery subsystem (RS tables + slot buffers) */
    rtp_fec_init();

    /* Start each stream with a clean rewind window. */
    safety_buffer_clear();

    /* Create synchronization semaphore */
    g_dec_sema = sceKernelCreateSema("sw_dec_sema", 0, 0, 64, NULL);
    if (g_dec_sema < 0) {
        dec_log("CreateSema failed: 0x%08X\n", (unsigned)g_dec_sema);
        oh264_pipeline_shutdown();
        return -2;
    }

    /* Start decoder thread */
    g_frame_rb = rb;
    g_dec_running = 1;
    g_dec_thread_id = sceKernelCreateThread("sw_dec",
                                             sw_decoder_thread,
                                             SW_DECODER_THREAD_PRIORITY,
                                             128 * 1024,  /* 128KB stack for VFPU recon */
                                             THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU,
                                             NULL);
    if (g_dec_thread_id < 0) {
        dec_log("CreateThread failed: 0x%08X\n", (unsigned)g_dec_thread_id);
        sceKernelDeleteSema(g_dec_sema);
        oh264_pipeline_shutdown();
        return -3;
    }
    sceKernelStartThread(g_dec_thread_id, 0, NULL);

    g_decoder_ready = 1;
    dec_log("Decoder init OK (sw_pipeline active, priority=0x%02X batch=%d)\n",
            SW_DECODER_THREAD_PRIORITY, DECODER_DRAIN_BATCH_LIMIT);
    return 0;
}

void sw_decoder_thread_shutdown(void)
{
    dec_log("Decoder shutdown begin\n");

    g_dec_running = 0;
    g_decoder_ready = 0;

    /* Wake decoder thread so it can exit */
    if (g_dec_sema >= 0) sceKernelSignalSema(g_dec_sema, 1);

    /* Wait for thread to finish */
    if (g_dec_thread_id >= 0) {
        SceUInt timeout = 2000000; /* 2s */
        int wait_ret = sceKernelWaitThreadEnd(g_dec_thread_id, &timeout);
        if (wait_ret < 0) {
            dec_log("Decoder shutdown wait failed: 0x%08X, forcing terminate\n",
                    (unsigned)wait_ret);
            sceKernelTerminateThread(g_dec_thread_id);
            sceKernelWaitThreadEnd(g_dec_thread_id, NULL);
        }
        sceKernelDeleteThread(g_dec_thread_id);
        g_dec_thread_id = -1;
    }
    if (g_dec_sema >= 0) {
        sceKernelDeleteSema(g_dec_sema);
        g_dec_sema = -1;
    }

    /* Shut down OpenH264 decode pipeline */
    oh264_pipeline_shutdown();

    /* Reset FEC state */
    rtp_fec_reset();

    /* Drop cached rewind data for the ended stream. */
    safety_buffer_clear();

    /* Frame pool REMOVED — zero-copy, nothing to free */

    g_saw_first_idr = 0;
    g_idr_fully_decoded = 0;
    g_frames_decoded = 0;
    g_frames_dropped = 0;

    dec_log("Decoder shutdown complete\n");
}

/* ============================================================================
 * sw_decoder_thread_force_restart — Watchdog recovery for decode pipeline stalls
 *
 * Called by main loop watchdog in two modes:
 * Mode A: OpenH264 infinite-loop — g_decode_active_us stuck >3s
 * Mode B: RTP/ring stall — no frames produced for >5s (idle >300)
 *
 * Forcibly terminates the stuck thread, abandons old OpenH264 context,
 * releases its resources, flushes ring state, reinits fresh, and starts
 * a new thread.
 * ============================================================================*/

void sw_decoder_thread_force_restart(void)
{
    dec_log("WATCHDOG: force-restarting decoder\n");
    diag_log_flush();

    /* 0. Log current heap pressure, but don't gate restart on it.
     * The stalled decoder, codec context, and thread stack are still live at
     * this point, so a pre-teardown free-memory check underestimates the
     * headroom available for restart and can suppress recovery entirely. */
    {
        SceSize free_mem = sceKernelTotalFreeMemSize();
        SceSize max_free = sceKernelMaxFreeMemSize();
        dec_log("WATCHDOG: pre-restart free=%uKB max_block=%uKB\n",
                (unsigned)(free_mem / 1024),
                (unsigned)(max_free / 1024));
    }

    /* 1. Kill the stuck decode thread */
    g_dec_running = 0;
    g_decoder_ready = 0;

    if (g_dec_thread_id >= 0) {
        sceKernelTerminateThread(g_dec_thread_id);
        {
            SceUInt timeout = 500000; /* 500ms grace after terminate */
            sceKernelWaitThreadEnd(g_dec_thread_id, &timeout);
        }
        sceKernelDeleteThread(g_dec_thread_id);
        g_dec_thread_id = -1;
        dec_log("WATCHDOG: old thread terminated\n");

        /* Let kernel fully clean up thread resources (stack, VFPU context).
         * sceKernelTerminateThread is a hard kill — without this delay, the
         * kernel's VFPU context tracking can become inconsistent, corrupting
         * VFPU state for other threads (including main thread's GU calls). */
        sceKernelDelayThread(2000);  /* 2ms — enough for kernel cleanup */
    }

    /* 2. Abandon old OpenH264 context and release resources.
     *    ME is killed and state is nulled inside. */
    oh264_pipeline_abandon();
    dec_log("WATCHDOG: pipeline abandoned\n");

    /* 2b. Re-measure headroom after teardown. This is the memory state that
     * actually matters for restart, because the dead decoder resources are
     * gone and the retry path below already handles init/create failures. */
    {
        SceSize free_mem = sceKernelTotalFreeMemSize();
        SceSize max_free = sceKernelMaxFreeMemSize();
        dec_log("WATCHDOG: post-abandon free=%uKB max_block=%uKB\n",
                (unsigned)(free_mem / 1024),
                (unsigned)(max_free / 1024));
    }

    /* 3. Allocate fresh OpenH264 codec context + RGBA buffers.
     *    oh264_pipeline_init skips ME PRX load (already loaded)
     *    but re-creates codec context, RGBA bufs.
        *    ME ctrl/params are freshly allocated. */
    int sw_res = oh264_pipeline_init();
    if (sw_res < 0) {
        dec_log("WATCHDOG: oh264_pipeline_init FAILED %d -- decoder offline\n", sw_res);
        diag_log_flush();
        return;
    }
    dec_log("WATCHDOG: pipeline reinit OK\n");

    /* 4. Reset RTP state so reassembly starts clean */
    rtp_reassembly_reset();
    rtp_fec_reset();
    safety_buffer_clear();
    dec_log("WATCHDOG: RTP/FEC reset\n");

    /* 5. Flush stale packets from PACKET ring buffer */
    if (g_packet_rb) {
        g_packet_rb->tail = g_packet_rb->head;
    }

    /* 5b. Flush stale frames from FRAME ring buffer.
     * Old frames hold RGBA pointers from the abandoned pipeline.
        * Displaying these after reinit is pointless — they show pre-crash
        * content. Flushing ensures the
     * main loop gets only freshly-decoded frames. */
    if (g_frame_rb) {
        g_frame_rb->tail = g_frame_rb->head;
        g_frame_rb->frame_ready = 0;
    }

    /* 6. Reset decode counters */
    g_frames_decoded = 0;
    g_frames_dropped = 0;
    g_saw_first_idr = 0;
    g_idr_fully_decoded = 0;
    g_decode_active_us = 0;

    /* 6b. Reset frame/corruption state on restart.
     * Keeping stale lgf across a decoder reset can lock reassembly into
     * dedup dropping when server frame IDs restart from a lower value.
     * Resetting these flags guarantees a clean recovery path after IDR. */
    {
        extern volatile unsigned int g_last_good_frame;
        g_last_good_frame = 0;
        g_refs_corrupted = 0;
        g_current_frame_is_corrupt = 0;
        g_fec_requested_idr = 0;
        dec_log("WATCHDOG: reset lgf/corruption flags for clean resync\n");
    }

    /* 6b2. Reset alive counter so watchdog detects fresh activity */
    g_decoder_alive_counter = 0;

    /* 6c. Signal callback/decode statics to reset on next entry.
     * me_stressed, consecutive_corrupt, s_wait_count etc. are function-level
     * statics that survive thread termination.  The flag tells the NEW
     * thread's first callback to clear them. */
    {
        extern volatile int g_decode_counters_reset_pending;
        g_decode_counters_reset_pending = 1;
    }
    dec_log("WATCHDOG: counters reset\n");

    /* 7. Request IDR BEFORE starting new thread.
     * Ensures fresh keyframe data is already in-flight when the new
     * decoder starts consuming.  Without this, the new decoder only
     * sees P-frames (useless without reference) until CTRL PING's
     * stall detection fires IDR ~5s later. */
    control_stream_request_idr();

    /* 8. Start fresh decode thread */
    g_dec_running = 1;
    g_dec_thread_id = sceKernelCreateThread("sw_dec",
                                             sw_decoder_thread,
                                             SW_DECODER_THREAD_PRIORITY,
                                             128 * 1024,
                                             THREAD_ATTR_USER | PSP_THREAD_ATTR_VFPU,
                                             NULL);
    if (g_dec_thread_id >= 0) {
        sceKernelStartThread(g_dec_thread_id, 0, NULL);
        g_decoder_ready = 1;
        dec_log("WATCHDOG: new decoder thread started (tid=0x%08X)\n",
                (unsigned)g_dec_thread_id);
    } else {
        dec_log("WATCHDOG: CreateThread failed 0x%08X -- decoder offline\n",
                (unsigned)g_dec_thread_id);
        g_dec_running = 0;
        g_decoder_ready = 0;

        /* Restart failed after pipeline reinit: release fresh resources so
         * we don't stay in a half-online state with decode buffers allocated. */
        oh264_pipeline_shutdown();
        rtp_reassembly_reset();
        rtp_fec_reset();
        safety_buffer_clear();
        g_decode_active_us = 0;
        g_decoder_alive_counter = 0;

        if (g_frame_rb) {
            g_frame_rb->tail = g_frame_rb->head;
            g_frame_rb->frame_ready = 0;
        }

        dec_log("WATCHDOG: restart cleanup complete, decoder remains offline\n");
    }
    dec_log("WATCHDOG: force_restart complete\n");
    diag_log_flush();
}

void sw_decoder_thread_wakeup(void)
{
    if (g_dec_sema >= 0) sceKernelSignalSema(g_dec_sema, 1);
}

int sw_decoder_thread_is_running(void)
{
    return g_dec_running;
}

/* Global CABAC detection flag — set by lightweight PPS NAL scanner below */
volatile int g_cabac_detected = 0;

int decoder_is_cabac_detected(void)
{
    return g_cabac_detected;
}

/* ============================================================================
 * Lightweight CABAC Detection — Scan raw NAL stream for PPS entropy mode
 *
 * Parses PPS NAL units (type 8) from Annex-B data to check
 * entropy_coding_mode_flag.  Only runs until the first PPS is seen.
 * No allocation, no state — just reads existing NAL buffer.
 * ============================================================================*/

/* Exp-Golomb unsigned decode (minimal inline version).
 * Returns 1 on success, 0 on parse failure. */
static int nal_read_ue(const u8 *data, int total_bits, int *bit_pos, unsigned int *out_val)
{
    int pos;
    int zeros = 0;
    unsigned int val = 0;

    if (!data || !bit_pos || !out_val || total_bits <= 0) {
        return 0;
    }

    pos = *bit_pos;

    while (pos < total_bits) {
        int byte_idx = pos >> 3;
        int bit_idx  = 7 - (pos & 7);
        if ((data[byte_idx] >> bit_idx) & 1) {
            break;
        }
        zeros++;
        pos++;
        if (zeros > 16) {
            return 0; /* sanity limit */
        }
    }

    if (pos >= total_bits) {
        return 0; /* stop bit missing */
    }
    pos++; /* consume stop bit */

    for (int i = 0; i < zeros; i++) {
        int byte_idx;
        int bit_idx;
        if (pos >= total_bits) {
            return 0;
        }
        byte_idx = pos >> 3;
        bit_idx  = 7 - (pos & 7);
        val = (val << 1) | ((data[byte_idx] >> bit_idx) & 1);
        pos++;
    }

    *out_val = ((1u << zeros) - 1u) + val;
    *bit_pos = pos;
    return 1;
}

static void check_nal_for_cabac(const u8 *nal_data, int nal_len)
{
    if (s_pps_checked || g_cabac_detected) return;

    if (!nal_data || nal_len <= 6) {
        return;
    }

    /* Scan for PPS NAL units (start code + NAL type 8) */
    for (int i = 0; i < nal_len - 5; i++) {
        int sc_len = 0;
        if (nal_data[i] == 0 && nal_data[i+1] == 0) {
            if (nal_data[i+2] == 1) sc_len = 3;
            else if (nal_data[i+2] == 0 && i + 3 < nal_len && nal_data[i+3] == 1) sc_len = 4;
        }
        if (sc_len == 0) continue;

        int nal_start = i + sc_len;
        if (nal_start >= nal_len) break;
        int nal_type = nal_data[nal_start] & 0x1F;

        if (nal_type == 8) {  /* PPS */
            int nal_end = nal_len;

            /* Find this NAL's end (start of next Annex-B start code or buffer end). */
            for (int j = nal_start + 1; j < nal_len - 3; j++) {
                if (nal_data[j] == 0 && nal_data[j + 1] == 0 &&
                    (nal_data[j + 2] == 1 ||
                     (j + 3 < nal_len && nal_data[j + 2] == 0 && nal_data[j + 3] == 1))) {
                    nal_end = j;
                    break;
                }
            }

            /* Parse: skip NAL header byte, read pic_parameter_set_id (ue),
             * seq_parameter_set_id (ue), then entropy_coding_mode_flag (1 bit) */
            int pps_data_start = nal_start + 1;
            int pps_payload_len = nal_end - pps_data_start;
            if (pps_payload_len <= 0) {
                s_pps_checked = 1;
                dec_log("PPS parse skipped: empty payload\n");
                return;
            }

            /* Convert NAL payload to RBSP by removing emulation-prevention bytes. */
            u8 rbsp[256];
            int rbsp_len = 0;
            for (int k = 0; k < pps_payload_len && rbsp_len < (int)sizeof(rbsp); k++) {
                if (k + 2 < pps_payload_len &&
                    nal_data[pps_data_start + k] == 0x00 &&
                    nal_data[pps_data_start + k + 1] == 0x00 &&
                    nal_data[pps_data_start + k + 2] == 0x03) {
                    rbsp[rbsp_len++] = 0x00;
                    rbsp[rbsp_len++] = 0x00;
                    k += 2;
                    continue;
                }
                rbsp[rbsp_len++] = nal_data[pps_data_start + k];
            }

            int total_bits = rbsp_len * 8;
            if (total_bits < 10) {
                s_pps_checked = 1;
                dec_log("PPS parse skipped: RBSP too short (%d bytes)\n", rbsp_len);
                return;
            }

            {
                int bit_pos = 0;
                unsigned int pps_id = 0;
                unsigned int sps_id = 0;
                int entropy_flag;
                int pic_order_present_flag;

                /* Parse must succeed through the first three PPS syntax elements.
                 * If not, keep scanning; malformed data must not trigger CABAC. */
                if (!nal_read_ue(rbsp, total_bits, &bit_pos, &pps_id) ||
                    !nal_read_ue(rbsp, total_bits, &bit_pos, &sps_id)) {
                    dec_log("PPS parse failed: invalid Exp-Golomb headers (rbsp=%d)\n", rbsp_len);
                    continue;
                }

                /* Need entropy_coding_mode_flag plus pic_order_present_flag bit. */
                if (bit_pos + 1 >= total_bits) {
                    dec_log("PPS parse failed: missing entropy/order flags (rbsp=%d)\n", rbsp_len);
                    continue;
                }

                {
                    int byte_idx = bit_pos >> 3;
                    int bit_idx  = 7 - (bit_pos & 7);
                    entropy_flag = (rbsp[byte_idx] >> bit_idx) & 1;
                    bit_pos++;
                }

                {
                    int byte_idx = bit_pos >> 3;
                    int bit_idx  = 7 - (bit_pos & 7);
                    pic_order_present_flag = (rbsp[byte_idx] >> bit_idx) & 1;
                }

                s_pps_checked = 1;
                dec_log("PPS parsed: pps_id=%u sps_id=%u entropy=%d pof=%d rbsp=%d\n",
                        pps_id, sps_id, entropy_flag, pic_order_present_flag, rbsp_len);

                if (entropy_flag) {
                    g_cabac_detected = 1;
                    dec_log("CABAC DETECTED in PPS NAL -- warning screen will show\n");
                } else {
                    dec_log("PPS checked: CAVLC confirmed (entropy_coding_mode=0)\n");
                }
                return;
            }
        }
    }
}

void sw_decoder_get_stats(u32 *decoded, u32 *dropped)
{
    if (decoded) *decoded = (u32)g_frames_decoded;
    if (dropped) *dropped = (u32)g_frames_dropped;
}
