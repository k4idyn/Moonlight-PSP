/*
 * rtp_reassembly.c - Video frame reassembly for Moonlight PSP
 *
 * This module implements a robust RTP depacketizer derived from the official
 * moonlight-common-c VideoDepacketizer.c. It handles Sunshine's dynamic
 * frame headers, NAL aggregation (STAP-A), and fragmentation (FU-A) while
 * respecting PSP-1000 memory constraints (static assembly buffers).
 */

#include "moonlight_types.h"
#include <pspkernel.h>
#include <psptypes.h>
#include <string.h>
#include <stdio.h>
#include "shared.h"
#include "rtp_reassembly.h"
#include "safety_buffer.h"
#include "diag_log.h"
#include "sw_decode_pipeline.h"
#include "control_stream.h"
#include "decode_flags.h"
#include "signal_strength.h"
#include "settings_menu.h"
#include "runtime_telemetry.h"

#define RTP_LOG(fmt, ...) diag_log_write("RTP", fmt, ##__VA_ARGS__)

/* Moonlight protocol constants */
#define FIXED_RTP_HEADER_SIZE   12
#define NV_VIDEO_PKT_SIZE       16
#define FLAG_EXTENSION          0x10    /* RTP header extension bit (X) */
#define FLAG_CONTAINS_PIC_DATA  0x01
#define FLAG_EOF                0x02
#define FLAG_SOF                0x04

/* Maximum reassembled frame size (256KB is enough for PSP-tier bitrates) */
#define MAX_ASSEMBLY_SIZE       (256 * 1024)
#define RFI_WAIT_MAX_DROPS_CAP  4
#define IDR_WAIT_MAX_DROPS_CAP  60
#define RFI_WAIT_MIN_DROPS      4
#define IDR_WAIT_MIN_DROPS      20
#define IDR_WAIT_LOG_INTERVAL   30
#define RFI_WAIT_MIN_US         200000U
#define IDR_WAIT_MIN_US         2000000U
#define RFI_WAIT_REF_SPAN_MAX   6
#define CAVLC_PERF_RFI_WAIT_MIN_US      500000U
#define CAVLC_PERF_RFI_WAIT_SPAN_FRAMES  90U
#define CABAC_RFI_WAIT_MIN_US           400000U
#define CABAC_RFI_WAIT_SPAN_FRAMES       24U
#define CABAC_RFI_WAIT_MAX_DROPS         12U
#define CABAC_PERF_RFI_WAIT_MIN_US      450000U
#define CABAC_PERF_RFI_WAIT_SPAN_FRAMES  36U
#define CABAC_PERF_RFI_WAIT_MAX_DROPS    18U
#define CABAC_PERF_IDR_RETRY_MIN_US     500000U
#define CABAC_PERF_IDR_RETRY_DROPS          8U
#define CABAC_PERF_CONTIG_FRAME_SKIP_MAX    3U
#define CABAC_QUALITY_RFI_WAIT_MIN_US    650000U
#define CABAC_QUALITY_RFI_WAIT_SPAN_FRAMES 36U
#define CABAC_QUALITY_RFI_WAIT_MAX_DROPS 20U

/* Reassembly state */
static u8  assembly_buffer[MAX_ASSEMBLY_SIZE] __attribute__((aligned(64)));
static u32 assembly_pos = 0;
static u32 current_frame_id = 0xFFFFFFFF;
static u32 s_last_seen_frame_id = 0xFFFFFFFF;
static u16 expected_seq = 0;
static int reassembling = 0;
static int g_frame_had_gaps = 0;
static int g_saw_first_sof = 0;
static int g_frame_overflow = 0;
static unsigned int s_consec_gap_frames = 0; /* consecutive frames with seq gaps */
static int s_waiting_for_ref_inval_frame = 0;
static int s_waiting_for_idr_frame = 0;
static u32 s_ref_inval_start = 0;
static u32 s_ref_inval_end = 0;
static u32 s_idr_wait_loss_start = 0;
static u32 s_idr_wait_loss_end = 0;
static u32 s_ref_inval_wait_start_us = 0;
static u32 s_idr_wait_start_us = 0;
static u32 s_rfi_wait_drop_count = 0;
static u32 s_idr_wait_drop_count = 0;
static int s_current_frame_idr_recovery = 0;
static u32 s_current_idr_loss_start = 0;
static u32 s_current_idr_loss_end = 0;

/* External dependencies */
extern int g_decoder_ready;
extern void rtp_frame_complete_callback(const u8 *nal_data, int nal_len);
extern volatile unsigned int g_last_good_frame;
extern PspConfig g_psp_config;
/* g_idr_fully_decoded declared in decode_flags.h */

/* Host processing latency from Sunshine frame headers (microseconds) */
volatile u32 g_host_processing_us = 0;

static void rtp_start_idr_wait(u32 start_frame, u32 end_frame, u32 current_frame, const char *reason);
static u32 rtp_frame_span(u32 start_frame, u32 end_frame);

static int rtp_is_cavlc_performance_mode(void)
{
    return !g_psp_config.cabacTestMode &&
           g_psp_config.fps >= 30 &&
           g_psp_config.width <= 320 &&
           g_psp_config.height <= 180;
}

static int rtp_is_cabac_mode(void)
{
    return g_psp_config.cabacTestMode != 0;
}

static int rtp_is_cabac_performance_mode(void)
{
    return rtp_is_cabac_mode() &&
           !g_psp_config.audioEnabled &&
           g_psp_config.fps >= 30 &&
           g_psp_config.width <= 320 &&
           g_psp_config.height <= 180;
}

static int rtp_is_cabac_quality_audio_mode(void)
{
    return rtp_is_cabac_mode() &&
           g_psp_config.audioEnabled &&
           g_psp_config.width == 480 &&
           g_psp_config.height == 272 &&
           g_psp_config.fps > 0 &&
           g_psp_config.fps <= 10;
}

static void rtp_request_idr_force_for_mode(void)
{
    if (rtp_is_cabac_performance_mode()) {
        control_stream_request_idr_recovery_fast();
    } else {
        control_stream_request_idr_force();
    }
}

static const char *rtp_recovery_label(void) __attribute__((unused));
static const char *rtp_recovery_label(void)
{
    if (g_psp_config.cabacTestMode) {
        return "CABAC";
    }
    if (rtp_is_cavlc_performance_mode()) {
        return "CAVLC performance";
    }
    return "RTP";
}

static u32 rtp_rfi_wait_min_us(void)
{
    if (rtp_is_cavlc_performance_mode()) {
        return CAVLC_PERF_RFI_WAIT_MIN_US;
    }
    if (rtp_is_cabac_performance_mode()) {
        return CABAC_PERF_RFI_WAIT_MIN_US;
    }
    if (rtp_is_cabac_quality_audio_mode()) {
        return CABAC_QUALITY_RFI_WAIT_MIN_US;
    }
    if (rtp_is_cabac_mode()) {
        return CABAC_RFI_WAIT_MIN_US;
    }
    return RFI_WAIT_MIN_US;
}

static u32 rtp_rfi_wait_ref_span_max(void)
{
    if (rtp_is_cavlc_performance_mode()) {
        return CAVLC_PERF_RFI_WAIT_SPAN_FRAMES;
    }
    if (rtp_is_cabac_performance_mode()) {
        return CABAC_PERF_RFI_WAIT_SPAN_FRAMES;
    }
    if (rtp_is_cabac_quality_audio_mode()) {
        return CABAC_QUALITY_RFI_WAIT_SPAN_FRAMES;
    }
    if (rtp_is_cabac_mode()) {
        return CABAC_RFI_WAIT_SPAN_FRAMES;
    }
    return RFI_WAIT_REF_SPAN_MAX;
}

static int rtp_rfi_span_exceeds_window(u32 start_frame, u32 end_frame)
{
    if (rtp_is_cavlc_performance_mode()) {
        return 0;
    }
    return rtp_frame_span(start_frame, end_frame) > rtp_rfi_wait_ref_span_max();
}

static u32 rtp_frame_span(u32 start_frame, u32 end_frame)
{
    if (start_frame == 0 || (s32)(end_frame - start_frame) < 0) {
        return 1;
    }
    return (u32)(end_frame - start_frame) + 1U;
}

static u32 rtp_rfi_wait_max_drops(void)
{
    u32 fps = (g_psp_config.fps > 0) ? (u32)g_psp_config.fps : 20U;
    u32 limit = fps;

    if (rtp_is_cavlc_performance_mode()) {
        return 4U;
    }
    if (rtp_is_cabac_performance_mode()) {
        return CABAC_PERF_RFI_WAIT_MAX_DROPS;
    }
    if (rtp_is_cabac_quality_audio_mode()) {
        return CABAC_QUALITY_RFI_WAIT_MAX_DROPS;
    }
    if (rtp_is_cabac_mode()) {
        return CABAC_RFI_WAIT_MAX_DROPS;
    }

    if (limit < RFI_WAIT_MIN_DROPS) {
        limit = RFI_WAIT_MIN_DROPS;
    }
    if (limit > RFI_WAIT_MAX_DROPS_CAP) {
        limit = RFI_WAIT_MAX_DROPS_CAP;
    }
    return limit;
}

static u32 rtp_idr_wait_max_drops(void)
{
    u32 fps = (g_psp_config.fps > 0) ? (u32)g_psp_config.fps : 20U;
    u32 limit = fps * 2U;

    if (limit < IDR_WAIT_MIN_DROPS) {
        limit = IDR_WAIT_MIN_DROPS;
    }
    if (limit > IDR_WAIT_MAX_DROPS_CAP) {
        limit = IDR_WAIT_MAX_DROPS_CAP;
    }
    return limit;
}

static int rtp_wait_elapsed(u32 start_us, u32 min_us)
{
    if (start_us == 0) {
        return 0;
    }
    return (u32)(sceKernelGetSystemTimeLow() - start_us) >= min_us;
}

static void rtp_advance_decoded_frame(u32 frame_id)
{
    if (g_last_decode_output_ok && g_idr_fully_decoded &&
        !g_current_frame_is_corrupt &&
        (g_last_good_frame == 0 || (s32)(frame_id - g_last_good_frame) > 0)) {
        g_last_good_frame = frame_id;
    }
}

static void rtp_mark_waiting_for_ref_inval(u32 start_frame, u32 end_frame)
{
    if (start_frame == 0 || (s32)(end_frame - start_frame) < 0) {
        start_frame = end_frame;
    }
    if (rtp_rfi_span_exceeds_window(start_frame, end_frame) &&
        g_last_good_frame != 0) {
        diag_log_write("RTP", "%s RFI span %u-%u exceeds %u frames; entering IDR wait",
                       rtp_recovery_label(), start_frame, end_frame,
                       (unsigned)rtp_rfi_wait_ref_span_max());
        rtp_start_idr_wait(start_frame, end_frame, end_frame, "RFI span exceeds decoder window");
        return;
    }
    if (!s_waiting_for_ref_inval_frame) {
        s_ref_inval_start = start_frame;
        s_ref_inval_end = end_frame;
        s_waiting_for_ref_inval_frame = 1;
        s_rfi_wait_drop_count = 0;
        s_ref_inval_wait_start_us = sceKernelGetSystemTimeLow();
        diag_log_write("RTP", "%s waiting for RFI recovery frame after loss %u-%u",
                       rtp_recovery_label(), s_ref_inval_start, s_ref_inval_end);
    } else {
        if ((s32)(start_frame - s_ref_inval_start) < 0) {
            s_ref_inval_start = start_frame;
        }
        if ((s32)(end_frame - s_ref_inval_end) > 0) {
            s_ref_inval_end = end_frame;
        }
        if (rtp_rfi_span_exceeds_window(s_ref_inval_start, s_ref_inval_end) &&
            g_last_good_frame != 0) {
            diag_log_write("RTP", "%s RFI span widened to %u-%u over %u frames; entering IDR wait",
                           rtp_recovery_label(), s_ref_inval_start, s_ref_inval_end,
                           (unsigned)rtp_rfi_wait_ref_span_max());
            rtp_start_idr_wait(s_ref_inval_start, s_ref_inval_end, s_ref_inval_end,
                               "RFI span exceeds decoder window");
            return;
        }
    }
}

static void rtp_clear_ref_inval_wait(void)
{
    s_waiting_for_ref_inval_frame = 0;
    s_ref_inval_start = 0;
    s_ref_inval_end = 0;
    s_ref_inval_wait_start_us = 0;
    s_rfi_wait_drop_count = 0;
}

static void rtp_clear_idr_wait(void)
{
    s_waiting_for_idr_frame = 0;
    s_idr_wait_loss_start = 0;
    s_idr_wait_loss_end = 0;
    s_idr_wait_start_us = 0;
    s_idr_wait_drop_count = 0;
}

static void rtp_clear_current_idr_recovery(void)
{
    s_current_frame_idr_recovery = 0;
    s_current_idr_loss_start = 0;
    s_current_idr_loss_end = 0;
}

static void rtp_start_idr_wait(u32 start_frame, u32 end_frame, u32 current_frame, const char *reason)
{
    int was_waiting = s_waiting_for_idr_frame;

    if (start_frame == 0 || (s32)(end_frame - start_frame) < 0) {
        start_frame = end_frame;
    }

    if (!s_waiting_for_idr_frame) {
        s_idr_wait_loss_start = start_frame;
        s_idr_wait_loss_end = end_frame;
        s_idr_wait_drop_count = 0;
        s_idr_wait_start_us = sceKernelGetSystemTimeLow();
        diag_log_write("RTP", "switching to IDR wait after %s: loss=%u-%u current=%u",
                       reason ? reason : "recovery miss",
                       s_idr_wait_loss_start, s_idr_wait_loss_end, current_frame);
    } else {
        if ((s32)(start_frame - s_idr_wait_loss_start) < 0) {
            s_idr_wait_loss_start = start_frame;
        }
        if ((s32)(end_frame - s_idr_wait_loss_end) > 0) {
            s_idr_wait_loss_end = end_frame;
        }
    }

    s_waiting_for_idr_frame = 1;
    rtp_clear_ref_inval_wait();
    g_refs_corrupted = 1;
    g_idr_fully_decoded = 0;
    if (was_waiting) {
        control_stream_request_idr();
    } else {
        rtp_request_idr_force_for_mode();
    }
}

void rtp_reassembly_note_frame_loss(u32 start_frame, u32 end_frame)
{
    if (start_frame == 0 || (s32)(end_frame - start_frame) < 0) {
        start_frame = end_frame;
    }

    if (s_waiting_for_idr_frame) {
        if ((s32)(start_frame - s_idr_wait_loss_start) < 0) {
            s_idr_wait_loss_start = start_frame;
        }
        if ((s32)(end_frame - s_idr_wait_loss_end) > 0) {
            s_idr_wait_loss_end = end_frame;
        }
        return;
    }

    if ((g_refs_corrupted || !g_idr_fully_decoded) && g_last_good_frame == 0) {
        rtp_start_idr_wait(start_frame, end_frame, end_frame,
                           g_refs_corrupted ? "corrupt refs" : "pre-IDR loss");
        return;
    }

    rtp_mark_waiting_for_ref_inval(start_frame, end_frame);
}

int rtp_reassembly_waiting_for_idr(void)
{
    return s_waiting_for_idr_frame;
}

/*--------------------------------------------------------------------------
 * NAL Unit Helpers
 *--------------------------------------------------------------------------*/

static int is_annexb_start(const u8 *data, int len, int offset) {
    if (offset + 3 < len && data[offset] == 0 && data[offset+1] == 0 && data[offset+2] == 1)
        return 3;
    if (offset + 4 < len && data[offset] == 0 && data[offset+1] == 0 && data[offset+2] == 0 && data[offset+3] == 1)
        return 4;
    return 0;
}

static int payload_contains_idr_nal(const u8 *data, int len) {
    int i;

    if (data == NULL || len <= 0) {
        return 0;
    }

    for (i = 0; i < len; i++) {
        int sc = is_annexb_start(data, len, i);
        if (sc > 0 && i + sc < len) {
            u8 nal_type = data[i + sc] & 0x1F;
            if (nal_type == 5) {
                return 1;
            }
            i += sc;
        }
    }

    {
        u8 nal_type = data[0] & 0x1F;
        if (nal_type == 5) {
            return 1;
        }
        if (nal_type == 28 && len >= 2 && (data[1] & 0x80) && ((data[1] & 0x1F) == 5)) {
            return 1;
        }
    }

    return 0;
}

static inline void append_to_frame(const u8 *data, int len) {
    if (len <= 0) return;
    if (assembly_pos + (u32)len > MAX_ASSEMBLY_SIZE) {
        if (!g_frame_overflow) {
            diag_log_write("RTP", "assembly overflow: pos=%u + len=%d > %u -- dropping frame",
                           assembly_pos, len, (unsigned)MAX_ASSEMBLY_SIZE);
        }
        g_frame_overflow = 1;
        return;
    }
    memcpy(assembly_buffer + assembly_pos, data, len);
    assembly_pos += (u32)len;
}

static inline void append_start_code(void) {
    static const u8 sc[] = {0, 0, 0, 1};
    append_to_frame(sc, 4);
}

/*--------------------------------------------------------------------------
 * process_rtp_payload_verbatim
 * Logic derived from moonlight-common-c / processRtpPayload
 *--------------------------------------------------------------------------*/
void rtp_reassembly_process_packet(u8 *packet, int packet_len) {
    if (packet_len < FIXED_RTP_HEADER_SIZE) return;

    /* 1. Parse RTP Header */
    int data_offset = FIXED_RTP_HEADER_SIZE;
    if (packet[0] & FLAG_EXTENSION) {
        /* GameStream video uses the fixed 4-byte RTP extension layout used by
         * moonlight-common-c's RtpVideoQueue/VideoDepacketizer. Parsing the
         * extension length word here can misalign FEC-recovered packets. */
        if (packet_len < data_offset + 4) return;
        data_offset += 4;
    }

    if (packet_len < data_offset + NV_VIDEO_PKT_SIZE) return;

    u16 seq = (u16)((packet[2] << 8) | packet[3]);
    u8  marker = (packet[1] & 0x80) != 0;

    /* 2. Parse NV_VIDEO_PACKET (Little-Endian) */
    u8 *nv = packet + data_offset;
    u32 frame_id = (u32)nv[4] | ((u32)nv[5] << 8) | ((u32)nv[6] << 16) | ((u32)nv[7] << 24);
    u8  flags = nv[8];

    u8 *payload = nv + NV_VIDEO_PKT_SIZE;
    int payload_len = packet_len - (data_offset + NV_VIDEO_PKT_SIZE);

    /* Reject packets with unknown flag bits.
     * Per moonlight-common-c: only PIC_DATA(0x01), EOF(0x02), SOF(0x04)
     * should be set. FEC parity/recovery packets sometimes arrive with
     * stray bits (e.g. 0x2E, 0xB7) which corrupt frame assembly. */
    if (flags & ~(FLAG_CONTAINS_PIC_DATA | FLAG_EOF | FLAG_SOF)) {
        return;
    }

    /* First packet for a frame is SOF on FEC block 0. The current FEC
     * block lives in NV_VIDEO_PACKET.multiFecBlocks bits 4-5. */
    u8 fec_block_num = (nv[11] >> 4) & 0x03;
    int first_packet = (flags & FLAG_SOF) != 0 && fec_block_num == 0;
    int last_packet  = (flags & FLAG_EOF) != 0;

    /* Deduplication & Sequence Check
     * CRITICAL: Skip dedup until first IDR is fully decoded.
     * Before IDR decode, g_last_good_frame advances on every P-frame
     * (which all fail decode), causing FEC-recovered IDR frames with
     * older frame_ids to be dropped — permanently blocking decode. */
    if (g_idr_fully_decoded && g_last_good_frame != 0 && (s32)(frame_id - g_last_good_frame) <= 0) {
        /* Rate-limited log: once per unique dropped frame_id */
        static u32 last_dropped_fid = 0xFFFFFFFF;
        if (frame_id != last_dropped_fid) {
            RTP_LOG("[RTP] Dedup drop fid=%u (g_last_good=%u)\n",
                    frame_id, g_last_good_frame);
            last_dropped_fid = frame_id;
        }
        return;
    }

    u32 prev_seen_frame_id = s_last_seen_frame_id;
    s32 transport_frame_delta = 0;
    int transport_frame_gap = 0;
    int rtp_seq_gap_at_frame_boundary = 0;
    if (prev_seen_frame_id != 0xFFFFFFFF) {
        transport_frame_delta = (s32)(frame_id - prev_seen_frame_id);
        transport_frame_gap = transport_frame_delta > 1;
    }
    if (frame_id != current_frame_id && current_frame_id != 0xFFFFFFFF) {
        rtp_seq_gap_at_frame_boundary = (seq != expected_seq);
    }

    /* Forward jump protection must use transport progress, not decode output.
     * g_last_good_frame intentionally means "last displayable decoded frame";
     * on PSP-1000 OpenH264 can lag or legally return no-output for many access
     * units. Comparing incoming transport frame IDs against lgf turns normal
     * decode backlog into false loss windows and causes RFI storms.
     *
     * Frame-id gaps by themselves do not prove loss: Sunshine can intentionally
     * skip frame ids when pacing low-FPS streams, and N3DS-style smooth playback
     * accepts those packet-contiguous skips. Only treat a frame-id jump as
     * reference loss when the RTP sequence also jumped at the frame boundary. */
    if (transport_frame_delta > 100 &&
        rtp_seq_gap_at_frame_boundary &&
        !s_waiting_for_idr_frame &&
        !s_waiting_for_ref_inval_frame) {
        u32 loss_start = prev_seen_frame_id + 1;
        RTP_LOG("[RTP] Massive frame_id jump: %u -> %u (delta=%d), requesting RFI\n",
                prev_seen_frame_id, frame_id, (int)transport_frame_delta);
        rtp_mark_waiting_for_ref_inval(loss_start, frame_id);
        control_stream_request_rfi(loss_start, frame_id);
    }
    if (s_last_seen_frame_id == 0xFFFFFFFF ||
        (s32)(frame_id - s_last_seen_frame_id) > 0) {
        s_last_seen_frame_id = frame_id;
    }

    if (frame_id != current_frame_id) {
        if (reassembling && assembly_pos > 0) {
            RTP_LOG("[RTP] Dropping partial frame %u (switch to %u)\n", current_frame_id, frame_id);
            /* Partial frame loss is usually transient; prefer targeted RFI so
             * we avoid forcing frequent full-IDR bursts on borderline WiFi. */
            if (g_last_good_frame == 0) {
                control_stream_request_idr();
            } else {
                control_stream_request_rfi(current_frame_id, current_frame_id);
            }
            signal_strength_report_frame_drop();
        }
        assembly_pos = 0;
        current_frame_id = frame_id;
        reassembling = 0;
        g_frame_had_gaps = 0;
        g_frame_overflow = 0;
        g_saw_first_sof = 0;
        expected_seq = seq;
    }

    if (seq != expected_seq) {
        g_frame_had_gaps = 1;
        safety_buffer_handle_packet_loss();
    }
    expected_seq = (u16)(seq + 1);

    if (first_packet) {
        /* Guard against duplicate SOF for the same frame — only the
         * first SOF resets the assembly buffer. A second SOF (e.g. from
         * FEC recovery) would destroy data already accumulated. */
        if (g_saw_first_sof) {
            return;
        }
        reassembling = 1;
        assembly_pos = 0;
        g_saw_first_sof = 1;
        g_frame_overflow = 0;
        rtp_clear_current_idr_recovery();

        /* SOF diagnostic silenced for performance (was per-frame) */

        /* Sunshine Frame Header skip (Verbatim logic from moonlight-common-c) */
        int header_skip = 0;
        int frame_type = -1;
        if (payload_len >= 1) {
            if (payload[0] == 0x01) {
                header_skip = 8;
                if (payload_len >= 4) frame_type = payload[3];
                /* Parse host processing latency from Sunshine type-0x01 header.
                 * Bytes 4-7 contain the host encode duration in microseconds (LE32). */
                if (payload_len >= 8) {
                    u32 host_us = (u32)payload[4]
                                | ((u32)payload[5] << 8)
                                | ((u32)payload[6] << 16)
                                | ((u32)payload[7] << 24);
                    g_host_processing_us = host_us;
                    {
                        static int hpl_log = 0;
                        if (hpl_log < 5 || (hpl_log % 1000) == 0) {
                            RTP_LOG("host_proc_us=%u fid=%u\n", host_us, frame_id);
                        }
                        hpl_log++;
                    }
                }
            } else if (payload[0] == (u8)0x81) {
                header_skip = 44;
                if (payload_len >= 4) frame_type = payload[3];
            } else if (payload_len >= 4 && payload[0] == 0 && payload[1] == 0 && payload[2] == 0 && payload[3] == 1) {
                /* Raw H.264 slice – no Sunshine header */
                header_skip = 0;
            }
        }

        {
            const u8 *payload_after_header = payload;
            int payload_after_header_len = payload_len;
            int contains_idr = 0;
            int is_sync_frame;
            int is_ref_recovery_frame;
            int accept_after_rfi_timeout = 0;

            if (payload_after_header_len >= header_skip) {
                payload_after_header += header_skip;
                payload_after_header_len -= header_skip;
                contains_idr = payload_contains_idr_nal(payload_after_header, payload_after_header_len);
            }

            is_sync_frame = (frame_type == 2) || contains_idr;
            is_ref_recovery_frame = is_sync_frame || (frame_type == 4) || (frame_type == 5);

            if (transport_frame_gap &&
                !s_waiting_for_idr_frame &&
                !s_waiting_for_ref_inval_frame &&
                g_idr_fully_decoded && g_last_good_frame != 0) {
                u32 loss_start = prev_seen_frame_id + 1;
                u32 loss_end = frame_id - 1;
                if (rtp_is_cabac_performance_mode() &&
                    !rtp_seq_gap_at_frame_boundary &&
                    transport_frame_delta > 1 &&
                    (u32)(transport_frame_delta - 1) <= CABAC_PERF_CONTIG_FRAME_SKIP_MAX) {
                    static u32 s_cabac_contig_skip_count = 0;
                    s_cabac_contig_skip_count++;
                    if (s_cabac_contig_skip_count <= 8 ||
                        (s_cabac_contig_skip_count % 100U) == 0) {
                        diag_log_write("RTP",
                                       "CABAC performance frame-id skip %u -> %u has contiguous RTP; accepting fid=%u type=%d skipped=%u [count=%u]",
                                       prev_seen_frame_id, frame_id, frame_id,
                                       frame_type,
                                        (unsigned)(transport_frame_delta - 1),
                                        s_cabac_contig_skip_count);
                    }
                } else if (rtp_is_cabac_mode() && !is_ref_recovery_frame) {
                    static u32 s_cabac_gap_hold_count = 0;
                    s_cabac_gap_hold_count++;
                    if (s_cabac_gap_hold_count <= 8 ||
                        (s_cabac_gap_hold_count % 100U) == 0) {
                        diag_log_write("RTP",
                                       "CABAC pacing: frame-id gap %u -> %u (missing %u-%u) held before decode [count=%u]",
                                       prev_seen_frame_id, frame_id,
                                       loss_start, loss_end,
                                       s_cabac_gap_hold_count);
                    }
                    rtp_mark_waiting_for_ref_inval(loss_start, loss_end);
                    control_stream_request_rfi(loss_start, loss_end);
                    signal_strength_report_frame_drop();
                } else if (!rtp_seq_gap_at_frame_boundary) {
                    static u32 s_contiguous_skip_log_count = 0;
                    s_contiguous_skip_log_count++;
                    if (s_contiguous_skip_log_count <= 8 ||
                        (s_contiguous_skip_log_count % 100) == 0) {
                        diag_log_write("RTP", "frame-id skip %u -> %u has contiguous RTP; accepting fid=%u type=%d",
                                       prev_seen_frame_id, frame_id, frame_id, frame_type);
                    }
                } else if (is_ref_recovery_frame) {
                    diag_log_write("RTP", "transport gap %u-%u covered by recovery frame fid=%u type=%d",
                                    loss_start, loss_end, frame_id, frame_type);
                } else if (rtp_is_cavlc_performance_mode()) {
                    diag_log_write("RTP", "CAVLC performance transport frame gap %u -> %u (missing %u-%u), entering RFI wait",
                                   prev_seen_frame_id, frame_id, loss_start, loss_end);
                    rtp_mark_waiting_for_ref_inval(loss_start, loss_end);
                    control_stream_request_rfi(loss_start, loss_end);
                    signal_strength_report_frame_drop();
                } else {
                    diag_log_write("RTP", "%s transport frame gap %u -> %u (missing %u-%u), entering RFI wait",
                                   rtp_recovery_label(), prev_seen_frame_id, frame_id,
                                   loss_start, loss_end);
                    rtp_mark_waiting_for_ref_inval(loss_start, loss_end);
                    signal_strength_report_frame_drop();
                }
            }

            if (g_refs_corrupted && !s_waiting_for_idr_frame) {
                u32 loss_start = g_last_good_frame ? (g_last_good_frame + 1) : frame_id;
                rtp_start_idr_wait(loss_start, frame_id, frame_id,
                                   "decoder corrupt refs");
            }

            if (s_waiting_for_idr_frame) {
                if (is_sync_frame) {
                    diag_log_write("RTP", "IDR recovery frame accepted: fid=%u type=%d loss=%u-%u drops=%u",
                                   frame_id, frame_type, s_idr_wait_loss_start,
                                   s_idr_wait_loss_end, s_idr_wait_drop_count);
                    s_current_frame_idr_recovery = 1;
                    s_current_idr_loss_start = s_idr_wait_loss_start;
                    s_current_idr_loss_end = s_idr_wait_loss_end;
                    rtp_clear_idr_wait();
                } else {
                    s_idr_wait_drop_count++;
                    if (s_idr_wait_drop_count == rtp_idr_wait_max_drops() &&
                        !rtp_wait_elapsed(s_idr_wait_start_us, IDR_WAIT_MIN_US)) {
                        diag_log_write("RTP", "strict IDR wait held after %u drops; still inside %ums minimum",
                                       s_idr_wait_drop_count, IDR_WAIT_MIN_US / 1000U);
                    }
                    if (s_idr_wait_drop_count >= rtp_idr_wait_max_drops() &&
                        rtp_wait_elapsed(s_idr_wait_start_us, IDR_WAIT_MIN_US)) {
                        if ((s_idr_wait_drop_count % rtp_idr_wait_max_drops()) == 0) {
                            diag_log_write("RTP", "strict IDR wait: dropping P-frame fid=%u count=%u lgf=%u",
                                           frame_id, s_idr_wait_drop_count,
                                           (unsigned)g_last_good_frame);
                            control_stream_request_idr();
                        }
                    } else {
                        if (s_idr_wait_drop_count <= 3 ||
                            (s_idr_wait_drop_count % IDR_WAIT_LOG_INTERVAL) == 0) {
                            diag_log_write("RTP", "waiting for IDR frame: drop fid=%u type=%d loss=%u-%u count=%u",
                                           frame_id, frame_type, s_idr_wait_loss_start,
                                           s_idr_wait_loss_end, s_idr_wait_drop_count);
                        }
                    }
                    if (rtp_is_cabac_performance_mode()) {
                        if (s_idr_wait_drop_count == 1 ||
                            (rtp_wait_elapsed(s_idr_wait_start_us, CABAC_PERF_IDR_RETRY_MIN_US) &&
                             (s_idr_wait_drop_count % CABAC_PERF_IDR_RETRY_DROPS) == 0)) {
                            control_stream_request_idr_recovery_fast();
                        }
                    } else {
                        if (s_idr_wait_drop_count == 1 ||
                            (rtp_wait_elapsed(s_idr_wait_start_us, IDR_WAIT_MIN_US) &&
                             (s_idr_wait_drop_count % (IDR_WAIT_LOG_INTERVAL * 2)) == 0)) {
                            control_stream_request_idr_force();
                        } else if (rtp_wait_elapsed(s_idr_wait_start_us, IDR_WAIT_MIN_US) &&
                                   (s_idr_wait_drop_count % IDR_WAIT_LOG_INTERVAL) == 0) {
                            control_stream_request_idr();
                        }
                    }
                    reassembling = 0;
                    assembly_pos = 0;
                    g_saw_first_sof = 0;
                    g_fec_recovery_clean = 0;
                    /* These are deliberate decoder-protection skips while we
                     * wait for a sync frame, not fresh transport drops. */
                    return;
                }
            } else if (s_waiting_for_ref_inval_frame) {
                if (is_ref_recovery_frame) {
                    diag_log_write("RTP", "RFI recovery frame accepted: fid=%u type=%d loss=%u-%u",
                                   frame_id, frame_type, s_ref_inval_start, s_ref_inval_end);
                    rtp_clear_ref_inval_wait();
                    g_refs_corrupted = 0;
                    g_idr_fully_decoded = 1;
                } else {
                    s_rfi_wait_drop_count++;
                    if ((s32)(frame_id - s_ref_inval_end) > 0) {
                        s_ref_inval_end = frame_id;
                    }
                    if (rtp_rfi_span_exceeds_window(s_ref_inval_start, s_ref_inval_end)) {
                        diag_log_write("RTP", "%s RFI wait span %u-%u exceeds %u frames; entering IDR wait",
                                       rtp_recovery_label(),
                                       s_ref_inval_start, s_ref_inval_end,
                                       (unsigned)rtp_rfi_wait_ref_span_max());
                        rtp_start_idr_wait(s_ref_inval_start, s_ref_inval_end, frame_id,
                                           "RFI wait span exceeds decoder window");
                        reassembling = 0;
                        assembly_pos = 0;
                        g_saw_first_sof = 0;
                        g_fec_recovery_clean = 0;
                        return;
                    }
                    if (s_rfi_wait_drop_count <= 3 ||
                        (s_rfi_wait_drop_count % IDR_WAIT_LOG_INTERVAL) == 0) {
                        diag_log_write("RTP", "waiting for RFI frame: drop fid=%u type=%d loss=%u-%u count=%u",
                                        frame_id, frame_type, s_ref_inval_start,
                                        s_ref_inval_end, s_rfi_wait_drop_count);
                    }
                    {
                        unsigned int wait_max = rtp_rfi_wait_max_drops();
                        if (s_rfi_wait_drop_count == wait_max &&
                            !rtp_wait_elapsed(s_ref_inval_wait_start_us, rtp_rfi_wait_min_us())) {
                            diag_log_write("RTP", "%s RFI wait held after %u drops; still inside %ums minimum",
                                           rtp_recovery_label(), s_rfi_wait_drop_count,
                                           rtp_rfi_wait_min_us() / 1000U);
                        }
                        if (s_rfi_wait_drop_count >= wait_max &&
                            rtp_wait_elapsed(s_ref_inval_wait_start_us, rtp_rfi_wait_min_us())) {
                            u32 req_start = s_ref_inval_start;
                            u32 req_end = s_ref_inval_end;
                            if (req_end < req_start) {
                                req_end = req_start;
                            }
                            if (rtp_is_cabac_mode()) {
                                diag_log_write("RTP", "CABAC RFI wait timeout after %u drops; entering IDR wait loss=%u-%u current=%u",
                                               s_rfi_wait_drop_count, req_start, req_end, frame_id);
                                rtp_start_idr_wait(req_start, req_end, frame_id,
                                                   "CABAC RFI timeout");
                                reassembling = 0;
                                assembly_pos = 0;
                                g_saw_first_sof = 0;
                                g_fec_recovery_clean = 0;
                                return;
                            }
                            diag_log_write("RTP", "%s RFI wait timeout after %u drops; preserving refs and accepting fid=%u loss=%u-%u",
                                           rtp_recovery_label(), s_rfi_wait_drop_count, frame_id,
                                           req_start, req_end);
                            rtp_clear_ref_inval_wait();
                            g_refs_corrupted = 0;
                            g_idr_fully_decoded = 1;
                            control_stream_request_rfi(req_start, req_end);
                            accept_after_rfi_timeout = 1;
                        } else if (s_rfi_wait_drop_count == 1 ||
                                   ((rtp_is_cavlc_performance_mode() ||
                                     rtp_is_cabac_mode()) &&
                                    (s_rfi_wait_drop_count % 4U) == 0) ||
                                   (s_rfi_wait_drop_count % 10U) == 0) {
                            u32 req_end = s_ref_inval_end;
                            if (req_end < s_ref_inval_start) {
                                req_end = s_ref_inval_start;
                            }
                            control_stream_request_rfi(s_ref_inval_start, req_end);
                        }
                    }
                    if (!accept_after_rfi_timeout) {
                        reassembling = 0;
                        assembly_pos = 0;
                        g_saw_first_sof = 0;
                        g_fec_recovery_clean = 0;
                        /* Do not feed adaptive bitrate with each intentional
                         * RFI-wait P-frame skip; count only the original loss. */
                        return;
                    }
                }
            }
        }

        /* Log header detection for first 5 frames */
        {
            static int hdr_log_count = 0;
            if (hdr_log_count < 5) {
                char hx[49]; int hl = payload_len < 16 ? payload_len : 16;
                for (int hi = 0; hi < hl; hi++)
                    snprintf(hx + hi * 3, 4, "%02X ", payload[hi]);
                hx[hl * 3] = '\0';
                RTP_LOG("[RTP] SOF fid=%u hdr_byte=0x%02X skip=%d plen=%d hex: %s\n",
                        frame_id, payload_len > 0 ? payload[0] : 0xFF,
                        header_skip, payload_len, hx);
                hdr_log_count++;
            }
        }

        if (payload_len >= header_skip) {
            payload += header_skip;
            payload_len -= header_skip;
        } else {
            payload_len = 0;
        }
    }

    if (!reassembling || payload_len <= 0) return;

    /* Sunshine/Moonlight protocol: the H.264 data after the Sunshine header
     * is raw Annex-B formatted data split across multiple RTP packets.
     * Only the FIRST packet (SOF) starts a new Annex-B stream.
     * Continuation packets are just raw byte continuations — their first
     * byte is NOT a NAL type indicator and must NOT be parsed as FU-A/STAP-A.
     *
     * FU-A/STAP-A are RFC 6184 constructs that Sunshine does NOT use.
     * We only check for Annex-B start codes on the first packet. */
    if (first_packet) {
        int sc_len = is_annexb_start(payload, payload_len, 0);
        if (sc_len == 0) append_start_code();
    }
    telemetry_accum_video_usable((u32)payload_len);
    append_to_frame(payload, payload_len);

    /* 4. Complete Frame Delivery */
    if (last_packet || marker) {
        if (assembly_pos > 0 && reassembling) {
            if (g_frame_overflow) {
                /* Deterministic overflow policy: drop this frame and force
                 * recovery rather than decoding a truncated bitstream. */
                g_refs_corrupted = 1;
                g_current_frame_is_corrupt = 1;
                g_idr_fully_decoded = 0;
                diag_log_write("RTP", "frame %u dropped due to assembly overflow", frame_id);
                control_stream_request_idr();
                g_fec_recovery_clean = 0;
                s_consec_gap_frames = 0;
                reassembling = 0;
                assembly_pos = 0;
                g_frame_overflow = 0;
                return;
            }

            int drop_corrupt_frame = (g_frame_had_gaps && !g_fec_recovery_clean);

            /* Advance g_last_good_frame BEFORE the decode callback.
             * The callback blocks for the full CAVLC decode (up to 11s for
             * a complex IDR on the 333 MHz PSP).  Do not advance this counter
             * before the decoder returns a displayable output; false progress
             * makes stall detection and recovery less reliable.
             *
             * CRITICAL: Only advance after first IDR decoded, otherwise
             * every failing P-frame advances the counter and blocks
             * FEC-recovered IDR arrivals via dedup.
             *
             * EXCEPTION: When g_idr_fully_decoded is false, the dedup
             * check (above) is completely disabled, so advancing
             * g_last_good is safe — it only updates the progress counter
             * for CTRL PING keepalives.  Force-advance when the counter
             * is stale (delta > 50) to prevent the delta from reaching
             * 100 and triggering a massive-jump reset. */
            {
                int delta = (s32)(frame_id - g_last_good_frame);
                if (0 && !drop_corrupt_frame &&
                    g_idr_fully_decoded &&
                    (g_last_good_frame == 0 ||
                    (delta > 0 && delta <= 100))) {
                    g_last_good_frame = frame_id;
                } else if (0) {
                    RTP_LOG("[RTP] NOT advancing g_last_good=%u for fid=%u (delta=%d)\n",
                            g_last_good_frame, frame_id,
                            (int)(frame_id - g_last_good_frame));
                }
            }

            /* If packets were missing within this frame (seq gaps), the
             * assembled NAL data is incomplete.  OpenH264's error concealment
             * can "successfully" decode such data (ret=0) while producing
             * visible macroblocking.  Mark refs corrupted so the decoder
             * skips P-frames until a clean IDR resets references.
             *
             * EXCEPTION: When the FEC layer successfully recovered all
             * missing packets (g_fec_recovery_clean=1), the reassembled
             * data IS complete despite the seq gaps in the RTP stream.
             * RS recovery is mathematically exact — don't mark corrupt. */
            if (g_frame_had_gaps && !g_fec_recovery_clean) {
                s_consec_gap_frames++;
                g_current_frame_is_corrupt = 0;

                /* This frame is dropped below and never enters OpenH264, so
                 * the decoder DPB is still valid. Preserve refs and avoid
                 * forcing a post-startup IDR wait that can freeze the stream. */
                if (g_last_good_frame == 0) {
                    g_idr_fully_decoded = 0;
                    diag_log_write("RTP", "frame %u has seq gaps before first ref (consec=%u), IDR",
                                   frame_id, s_consec_gap_frames);
                    control_stream_request_idr_force();
                } else if (s_current_frame_idr_recovery) {
                    u32 loss_start = s_current_idr_loss_start ? s_current_idr_loss_start : (g_last_good_frame + 1);
                    diag_log_write("RTP", "IDR recovery frame %u had seq gaps, continuing IDR wait",
                                   frame_id);
                    rtp_start_idr_wait(loss_start, frame_id, frame_id, "gapped IDR recovery");
                } else {
                    diag_log_write("RTP", "frame %u has seq gaps (consec=%u), dropped before decode",
                                   frame_id, s_consec_gap_frames);
                    rtp_mark_waiting_for_ref_inval(g_last_good_frame + 1, frame_id);
                    control_stream_request_rfi(g_last_good_frame + 1, frame_id);
                    if (s_consec_gap_frames >= 4) {
                        g_refs_corrupted = 1;
                        g_idr_fully_decoded = 0;
                    }
                }
                signal_strength_report_frame_drop();
            } else if (g_frame_had_gaps && g_fec_recovery_clean) {
                /* FEC recovered — seq gaps are expected but data is clean */
                g_current_frame_is_corrupt = 0;
                s_consec_gap_frames = 0;
            } else {
                /* No gaps — reset consecutive counter */
                s_consec_gap_frames = 0;
            }
            /* Reset FEC clean flag for next frame */
            g_fec_recovery_clean = 0;
            rtp_clear_current_idr_recovery();

            if (!drop_corrupt_frame) {
                g_last_decode_output_ok = 0;
                g_decode_current_frame_id = frame_id;
                rtp_frame_complete_callback(assembly_buffer, (int)assembly_pos);
                rtp_advance_decoded_frame(frame_id);
            } else {
                diag_log_write("RTP", "frame %u dropped before decode due to unrecovered seq gaps",
                               frame_id);
            }
        }
        reassembling = 0;
        assembly_pos = 0;
        g_frame_overflow = 0;
    }
}

void rtp_reassembly_reset(void) {
    assembly_pos = 0;
    current_frame_id = 0xFFFFFFFF;
    s_last_seen_frame_id = 0xFFFFFFFF;
    expected_seq = 0;
    reassembling = 0;
    g_frame_had_gaps = 0;
    g_frame_overflow = 0;
    g_saw_first_sof = 0;
    s_consec_gap_frames = 0;
    rtp_clear_ref_inval_wait();
    rtp_clear_idr_wait();
    rtp_clear_current_idr_recovery();
}

void rtp_reassembly_flush_pre_ready_frames(void) {
    /* Clear any stashed data when decoder becomes ready */
    rtp_reassembly_reset();
}

void rtp_reassembly_flush_partial_frame(void) {
    rtp_reassembly_reset();
}

void rtp_reassembly_prepare_fec_frame(u32 frame_id) {
    if (current_frame_id != frame_id) {
        return;
    }

    if (reassembling && assembly_pos > 0) {
        RTP_LOG("[RTP] clearing stale partial frame %u before FEC submit\n",
                frame_id);
    }

    assembly_pos = 0;
    reassembling = 0;
    g_frame_had_gaps = 0;
    g_frame_overflow = 0;
    g_saw_first_sof = 0;
    expected_seq = 0;
}

void rtp_reassembly_note_fec_frame_complete(u16 next_seq_after_fec) {
#ifndef RETAIL_BUILD
    static u32 s_fec_seq_align_count = 0;
    if (expected_seq != next_seq_after_fec &&
        (s_fec_seq_align_count < 8 ||
         (s_fec_seq_align_count % 300U) == 0)) {
        diag_log_write("RTP",
                       "FEC boundary seq align expected=%u -> %u [count=%u]",
                       (unsigned)expected_seq,
                       (unsigned)next_seq_after_fec,
                       (unsigned)(s_fec_seq_align_count + 1U));
    }
    s_fec_seq_align_count++;
#endif
    expected_seq = next_seq_after_fec;
}
