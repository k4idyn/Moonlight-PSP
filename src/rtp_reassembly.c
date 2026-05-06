/*
 * rtp_reassembly.c - Video frame reassembly for Moonlight PSP
 *
 * This module implements a robust RTP depacketizer derived from the official
 * moonlight-common-c VideoDepacketizer.c. It handles Sunshine's dynamic
 * frame headers, NAL aggregation (STAP-A), and fragmentation (FU-A) while
 * respecting PSP-1000 memory constraints (static assembly buffers).
 */

#include "moonlight_types.h"
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

/* Reassembly state */
static u8  assembly_buffer[MAX_ASSEMBLY_SIZE] __attribute__((aligned(64)));
static u32 assembly_pos = 0;
static u32 current_frame_id = 0xFFFFFFFF;
static u16 expected_seq = 0;
static int reassembling = 0;
static int g_frame_had_gaps = 0;
static int g_saw_first_sof = 0;
static int g_frame_overflow = 0;
static unsigned int s_consec_gap_frames = 0; /* consecutive frames with seq gaps */

/* External dependencies */
extern int g_decoder_ready;
extern void rtp_frame_complete_callback(const u8 *nal_data, int nal_len);
extern volatile unsigned int g_last_good_frame;
/* g_idr_fully_decoded declared in decode_flags.h */

/* Host processing latency from Sunshine frame headers (microseconds) */
volatile u32 g_host_processing_us = 0;

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
        /* RTP extension header: 2-byte type + 2-byte length (in 32-bit words)
         * then length*4 bytes of extension data. Reference code:
         *   offset += 4; offset += BE16(&data[offset-2]) * 4; */
        if (packet_len < data_offset + 4) return;
        u16 ext_words = (u16)((packet[data_offset + 2] << 8) |
                               packet[data_offset + 3]);
        data_offset += 4 + ext_words * 4;
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

    /* Forward jump protection: if frame_id leaps >100 ahead, the counter
     * was likely poisoned by a corrupted RS-recovered packet.  Anchor it
     * to the current frame so that future frames have small, manageable
     * deltas.  Setting to 0 creates a deadlock: when g_idr_fully_decoded
     * is false (persistent WiFi loss), g_last_good can never re-advance
     * from 0, causing ALL subsequent frames to fail decode permanently. */
    if (g_last_good_frame != 0 && (s32)(frame_id - g_last_good_frame) > 100) {
        RTP_LOG("[RTP] Massive frame_id jump: %u -> %u (delta=%d), anchoring to current\n",
                g_last_good_frame, frame_id, (int)(frame_id - g_last_good_frame));
        g_last_good_frame = frame_id;
        control_stream_request_idr();
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

        /* SOF diagnostic silenced for performance (was per-frame) */

        /* Sunshine Frame Header skip (Verbatim logic from moonlight-common-c) */
        int header_skip = 0;
        if (payload_len >= 1) {
            if (payload[0] == 0x01) {
                header_skip = 8;
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
            } else if (payload_len >= 4 && payload[0] == 0 && payload[1] == 0 && payload[2] == 0 && payload[3] == 1) {
                /* Raw H.264 slice – no Sunshine header */
                header_skip = 0;
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

            /* Advance g_last_good_frame BEFORE the decode callback.
             * The callback blocks for the full CAVLC decode (up to 11s for
             * a complex IDR on the 333 MHz PSP).  The CTRL PING thread
             * piggybacks an FEC status on every keepalive so that Sunshine
             * sees frame progress independently of decode latency.
             * Updating lgf first keeps the two in sync.
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
                int force = (!g_idr_fully_decoded &&
                             (g_last_good_frame == 0 || delta > 10));
                /* Also force-advance for FEC-recovered frames — the data is
                 * complete, so the frame counter should reflect progress. */
                if (!force && g_fec_recovery_clean) {
                    force = 1;
                }
                if ((g_idr_fully_decoded || force) &&
                    (g_last_good_frame == 0 ||
                    (delta > 0 && delta <= 100))) {
                    g_last_good_frame = frame_id;
                } else {
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
                g_current_frame_is_corrupt = 1;

                /* Escalate only on sustained corruption. Single-gap frames are
                 * handled by decoder concealment plus RFI to avoid IDR storms. */
                if (s_consec_gap_frames >= 2 || g_last_good_frame == 0) {
                    g_refs_corrupted = 1;
                    g_idr_fully_decoded = 0;
                    diag_log_write("RTP", "frame %u has seq gaps -- refs corrupted (consec=%u), IDR",
                                   frame_id, s_consec_gap_frames);
                    control_stream_request_idr();
                    s_consec_gap_frames = 0;
                } else {
                    diag_log_write("RTP", "frame %u has seq gaps (consec=%u), RFI",
                                   frame_id, s_consec_gap_frames);
                    control_stream_request_rfi(frame_id, frame_id);
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

            rtp_frame_complete_callback(assembly_buffer, (int)assembly_pos);
        }
        reassembling = 0;
        assembly_pos = 0;
        g_frame_overflow = 0;
    }
}

void rtp_reassembly_reset(void) {
    assembly_pos = 0;
    current_frame_id = 0xFFFFFFFF;
    expected_seq = 0;
    reassembling = 0;
    g_frame_had_gaps = 0;
    g_frame_overflow = 0;
    g_saw_first_sof = 0;
}

void rtp_reassembly_flush_pre_ready_frames(void) {
    /* Clear any stashed data when decoder becomes ready */
    rtp_reassembly_reset();
}

void rtp_reassembly_flush_partial_frame(void) {
    rtp_reassembly_reset();
}
