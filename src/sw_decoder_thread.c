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
#include <malloc.h>

#include "sw_decode_pipeline.h"
#include "stream_resolution.h"
#include "rtp_reassembly.h"
#include "rtp_fec.h"
#include "shared.h"
#include "diag_log.h"
#include "control_stream.h"
#include "settings_menu.h"  /* PspConfig */
#include "decode_flags.h"

extern PspConfig g_psp_config;

/* OpenH264-based decode pipeline (native low-latency H.264 decoder) */
extern int  oh264_pipeline_init(void);
extern void oh264_pipeline_shutdown(void);
extern int  oh264_pipeline_decode_frame(const u8 *nal_data, int nal_len, u8 **out_rgba);
extern void oh264_pipeline_invalidate_refs(void);
extern void oh264_pipeline_flush_buffers(void);
extern void oh264_pipeline_abandon(void);

/* Forward declaration — lightweight CABAC detection */
static void check_nal_for_cabac(const u8 *nal_data, int nal_len);

/* ============================================================================
 * Constants
 * ============================================================================*/

#define FRAME_POOL_SIZE     4
#define FRAME_BUF_SIZE      (PSP_LCD_STRIDE * PSP_LCD_HEIGHT * PIXEL_SIZE)

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

/* Timestamp of last decoded frame — read by main loop for latency display */
volatile u32 g_last_frame_decode_us = 0;

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
    static int s_cb_count = 0;
    static u32 s_last_usb_yield_us = 0;

    if (!nal_data || nal_len <= 0) return;

    /* Watchdog restart: reset stale callback counters so logging and
     * IDR request cadence start fresh after a pipeline reinit. */
    {
        extern volatile int g_decode_counters_reset_pending;
        if (g_decode_counters_reset_pending) {
            s_prev_done = 0;
            s_perf_count = 0;
            s_wait_count = 0;
            s_cb_count = 0;
            s_last_usb_yield_us = 0;
            g_decode_counters_reset_pending = 0;
        }
    }

    /* Pause gate: when g_decode_paused is set (via pspsh pokew), sleep in a
     * tight loop to free CPU for psplink USB commands (scrshot, meminfo).
     * The frame is NOT dropped — it will be decoded once resumed. */
    {
        extern volatile int g_decode_paused;
        while (g_decode_paused) {
            sceKernelDelayThread(5000);  /* 5ms sleep while paused */
        }
    }

    s_cb_count++;
    g_decoder_alive_counter++;
    /* Log every 120th callback to minimize I/O overhead in hot path */
    if (s_cb_count <= 3 || (s_cb_count % 120) == 0) {
        dec_log("CB#%d len=%d\n", s_cb_count, nal_len);
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
        char hex[49]; int hx = nal_len < 16 ? nal_len : 16;
        for (int hi = 0; hi < hx; hi++)
            snprintf(hex + hi * 3, 4, "%02X ", nal_data[hi]);
        hex[hx * 3] = '\0';
        dec_log("CB#%d hex: %s\n", s_cb_count, hex);
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
            check_nal_for_cabac(nal_data, nal_len);
            s_cabac_check_count++;
        }
    }

    u8 *rgba_out = NULL;
    g_decode_active_us = sceKernelGetSystemTimeLow();
    int ret = oh264_pipeline_decode_frame(nal_data, nal_len, &rgba_out);
    g_decode_active_us = 0;  /* decode finished — watchdog can relax */

    if (s_cb_count <= 3 || (s_cb_count % 120) == 0 || ret < 0) {
        dec_log("CB#%d ret=%d rgba=%s\n", s_cb_count, ret, rgba_out ? "OK" : "NULL");
    }

    if (ret == 0 && rgba_out) {
        push_sw_frame(rgba_out);
        s_wait_count = 0;  /* Reset — decode succeeded, SPS/PPS is valid */

        u64 t_cb_done;
        sceRtcGetCurrentTick(&t_cb_done);

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
            if (queued > 512) {
                dec_log("queue overrun: %u stale packets, flushing\n", (unsigned)queued);
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
        if (s_wait_count <= 10 || (s_wait_count % 30) == 0) {
            dec_log("waiting: ret=%d (count=%d)\n", ret, s_wait_count);
            control_stream_request_idr();
        }
        if (ret == -5 && s_wait_count > 200) {
            /* After 200+ frames with no SPS/PPS, something is very wrong */
            dec_log("STUCK: %d frames without SPS/PPS, forcing IDR burst\n", s_wait_count);
            control_stream_request_idr();
        }
        /* Reset counter on any successful decode (handled above in ret==0 path) */
    } else if (ret < 0) {
        g_frames_dropped++;
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
               batch < 512) {

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
            if (queued > 512) {
                dec_log("RING BACKLOG %u/1024 (>512) -- flushing to prevent overflow\n",
                        (unsigned)queued);
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
                                             0x1C,        /* Priority 28: same as audio, above old 32 */
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
    dec_log("Decoder init OK (sw_pipeline active)\n");
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
        sceKernelWaitThreadEnd(g_dec_thread_id, NULL);
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
 * Forcibly terminates the stuck thread, abandons old OpenH264 context
 * (leaking ~2MB), flushes ring, reinits fresh, starts new thread.
 * Limited to 5 restarts max (~10MB total leak on 24MB PSP).
 * ============================================================================*/

void sw_decoder_thread_force_restart(void)
{
    dec_log("WATCHDOG: force-restarting decoder\n");
    diag_log_flush();

    /* 0. Check free heap — each restart leaks ~2MB; abort if < 4MB remains */
    {
        SceSize free_mem = sceKernelTotalFreeMemSize();
        if (free_mem < 4 * 1024 * 1024) {
            dec_log("WATCHDOG: only %uKB free — skipping restart to avoid OOM\n",
                    (unsigned)(free_mem / 1024));
            diag_log_flush();
            return;
        }
        dec_log("WATCHDOG: %uKB free — proceeding with restart\n",
                (unsigned)(free_mem / 1024));
    }

    /* 1. Kill the stuck decode thread */
    g_dec_running = 0;
    g_decoder_ready = 0;

    if (g_dec_thread_id >= 0) {
        sceKernelTerminateThread(g_dec_thread_id);
        sceKernelDeleteThread(g_dec_thread_id);
        g_dec_thread_id = -1;
        dec_log("WATCHDOG: old thread terminated\n");

        /* Let kernel fully clean up thread resources (stack, VFPU context).
         * sceKernelTerminateThread is a hard kill — without this delay, the
         * kernel's VFPU context tracking can become inconsistent, corrupting
         * VFPU state for other threads (including main thread's GU calls). */
        sceKernelDelayThread(2000);  /* 2ms — enough for kernel cleanup */
    }

    /* 2. Abandon old OpenH264 context (leaks memory but avoids accessing
     *    corrupted state).  ME is killed and reinited inside. */
    oh264_pipeline_abandon();
    dec_log("WATCHDOG: pipeline abandoned\n");

    /* 3. Allocate fresh OpenH264 codec context + RGBA buffers.
     *    oh264_pipeline_init skips ME PRX load (already loaded)
     *    but re-creates codec context, RGBA bufs.
     *    ME ctrl/params are reused from abandon (not freed). */
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
    dec_log("WATCHDOG: RTP/FEC reset\n");

    /* 5. Flush stale packets from PACKET ring buffer */
    if (g_packet_rb) {
        g_packet_rb->tail = g_packet_rb->head;
    }

    /* 5b. Flush stale frames from FRAME ring buffer.
     * Old frames hold RGBA pointers from the abandoned pipeline.
     * Displaying these after reinit is safe (leaked, not freed) but
     * pointless — they show pre-crash content.  Flushing ensures the
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

    /* 6b. Preserve g_last_good_frame on restart.
     * The CTRL PING thread piggybacks FEC status using lgf + stall_advance.
     * If we reset lgf to 0, the piggybacked frame_index jumps backward
     * (e.g. from 1502 to 10), which the server interprets as a corrupted
     * client and permanently closes its send window.
     *
     * Instead, preserve lgf so the piggybacked frame_index continues
     * monotonically from lgf + stall_advance.  When the RTP layer receives
     * new frames after IDR, it will update lgf to the real frame index,
     * which will be higher than the preserved value. */
    {
        extern volatile unsigned int g_last_good_frame;
        dec_log("WATCHDOG: preserving lgf=%u (not resetting to 0)\n",
                g_last_good_frame);
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
                                             0x1C,
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

/* Exp-Golomb unsigned decode (minimal inline version) */
static unsigned int nal_read_ue(const u8 *data, int total_bits, int *bit_pos)
{
    int zeros = 0;
    while (*bit_pos < total_bits) {
        int byte_idx = *bit_pos >> 3;
        int bit_idx  = 7 - (*bit_pos & 7);
        if ((data[byte_idx] >> bit_idx) & 1) break;
        zeros++;
        (*bit_pos)++;
        if (zeros > 16) return 0; /* sanity limit */
    }
    (*bit_pos)++; /* skip the 1-bit */
    unsigned int val = 0;
    for (int i = 0; i < zeros; i++) {
        if (*bit_pos >= total_bits) return 0;
        int byte_idx = *bit_pos >> 3;
        int bit_idx  = 7 - (*bit_pos & 7);
        val = (val << 1) | ((data[byte_idx] >> bit_idx) & 1);
        (*bit_pos)++;
    }
    return (1u << zeros) - 1 + val;
}

static void check_nal_for_cabac(const u8 *nal_data, int nal_len)
{
    static int s_pps_checked = 0;
    if (s_pps_checked || g_cabac_detected) return;

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
            /* Parse: skip NAL header byte, read pic_parameter_set_id (ue),
             * seq_parameter_set_id (ue), then entropy_coding_mode_flag (1 bit) */
            int pps_data_start = nal_start + 1;
            int total_bits = (nal_len - pps_data_start) * 8;
            if (total_bits < 10) continue;

            const u8 *pps = nal_data + pps_data_start;
            int bit_pos = 0;
            nal_read_ue(pps, total_bits, &bit_pos);  /* pic_parameter_set_id */
            nal_read_ue(pps, total_bits, &bit_pos);  /* seq_parameter_set_id */

            /* Next bit = entropy_coding_mode_flag: 0=CAVLC, 1=CABAC */
            if (bit_pos < total_bits) {
                int byte_idx = bit_pos >> 3;
                int bit_idx  = 7 - (bit_pos & 7);
                int entropy_flag = (pps[byte_idx] >> bit_idx) & 1;

                s_pps_checked = 1;
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
