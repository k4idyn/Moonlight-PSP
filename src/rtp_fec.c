/*
 * rtp_fec.c - Reed-Solomon FEC recovery for RTP video packets
 *
 * Implements per-frame packet buffering and RS recovery matching
 * moonlight-common-c's RtpVideoQueue.c approach, adapted for PSP.
 *
 * Flow:
 *  1. network_me.c receives RTP packet
 *  2. rtp_fec_add_packet() buffers it and checks for frame completion
 *  3. If all data packets arrived, submit them directly to rtp_reassembly
 *  4. If some are missing but enough parity exists, RS recover then submit
 *  5. If unrecoverable, DROP the frame and request IDR/RFI replacement
 */

#include <psptypes.h>
#include <string.h>
#include <stdlib.h>
#include <pspkernel.h>
#include "rtp_fec.h"
#include "rs.h"
#include "diag_log.h"
#include "control_stream.h"
#include "sw_decode_pipeline.h"
#include "decode_flags.h"

/* g_last_good_frame declared in decode_flags.h */

/* Flag: FEC layer already requested IDR for this error event (C-2) */
volatile int g_fec_requested_idr = 0;

/* Flag: FEC recovery was mathematically clean for the current frame.
 * When RS succeeds, the reconstructed data is bit-perfect.  The reassembly
 * layer can trust it and skip the seq-gap corruption check which produces
 * false positives due to recovered packet seq number ordering. */
volatile int g_fec_recovery_clean = 0;

/* H-2: Cached RS context (file-scoped so rtp_fec_reset can free it) */
static reed_solomon *s_cached_rs = NULL;
static u32 s_cached_data = 0;
static u32 s_cached_parity = 0;

/* Consecutive unrecoverable frame counter — escalate to IDR after threshold.
 * MUST be 1: any single dropped frame breaks the H.264 reference chain,
 * causing all subsequent P-frames to decode against a stale reference.
 * RFI is useless without intra-refresh (g_intra_refresh_active == 0). */
static int s_consec_unrecoverable = 0;
#define UNRECOVERABLE_IDR_THRESHOLD 1

/* Catastrophic single-frame loss threshold — differentiated log message */
#define CATASTROPHIC_LOSS_THRESHOLD 3

/* ── RTP / NV_VIDEO_PACKET layout (matches moonlight-common-c) ───── */
#define FIXED_RTP_HEADER_SIZE 12
#define RTP_FLAG_EXTENSION    0x10
#define NV_VIDEO_PKT_SIZE     16

#define FLAG_SOF              0x04
#define FLAG_EOF              0x02
#define FLAG_CONTAINS_PIC     0x01

/* Maximum packet payload size (from config) */
#define MAX_PKT_SIZE          1500  /* max MTU overhead for safe payloads */

static inline void write_le32(u8 *p, u32 v)
{
    p[0] = (u8)(v);
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* ── Per-frame FEC buffer ────────────────────────────────────────── */
typedef struct {
    u8  *data;         /* raw packet copy (RTP header + NV_VIDEO_PACKET + payload) */
    int  len;          /* total length */
    int  received;     /* 1 = we have this packet */
} fec_slot_t;

static fec_slot_t g_slots[FEC_MAX_PACKETS];
static u8  g_pkt_storage[FEC_MAX_PACKETS][MAX_PKT_SIZE]; /* primary storage */
static u8  g_rec_storage[FEC_MAX_PACKETS][MAX_PKT_SIZE]; /* recovery pool */

static u32 g_current_frame = 0xFFFFFFFF;
static u16 g_lowest_seq = 0;
static u32 g_data_packets = 0;
static u32 g_parity_packets = 0;
static u32 g_total_packets = 0;
static u32 g_received_count = 0;
static u32 g_fec_percentage = 0;
static int g_frame_submitted = 0;
static int g_initialized = 0;

/* Last FEC status echoed to server — used by CTRL PING piggyback */
volatile u16 g_fec_last_highest_seq     = 0;
volatile u16 g_fec_last_next_contig_seq = 0;
volatile u16 g_fec_last_data_pkts       = 0;
volatile u16 g_fec_last_parity_pkts     = 0;
volatile u16 g_fec_last_recv_data       = 0;
volatile u16 g_fec_last_recv_parity     = 0;
volatile u8  g_fec_last_fec_pct         = 0;

/* Reassembly callback */
extern void rtp_reassembly_process_packet(u8 *packet, int packet_len);

/* ── Init / Reset ────────────────────────────────────────────────── */

void rtp_fec_init(void)
{
    int i;
    reed_solomon_init();
    for (i = 0; i < FEC_MAX_PACKETS; i++) {
        g_slots[i].data = g_pkt_storage[i];
        g_slots[i].len = 0;
        g_slots[i].received = 0;
    }
    g_current_frame = 0xFFFFFFFF;
    g_initialized = 1;
    diag_log_write("FEC", "initialized\n");
}

void rtp_fec_reset(void)
{
    int i;
    for (i = 0; i < FEC_MAX_PACKETS; i++) {
        g_slots[i].len = 0;
        g_slots[i].received = 0;
    }
    g_current_frame = 0xFFFFFFFF;
    g_lowest_seq = 0;
    g_data_packets = 0;
    g_parity_packets = 0;
    g_total_packets = 0;
    g_received_count = 0;
    g_fec_percentage = 0;
    g_frame_submitted = 0;
    g_fec_recovery_clean = 0;
    s_consec_unrecoverable = 0;

    /* NOTE: g_fec_last_* cached values are NOT reset here.
     * These are used by the CTRL PING piggyback to report FEC status to
     * the server.  After a force_restart, lgf is preserved to avoid a
     * backward jump in piggybacked frame_index.  The cached sequence
     * numbers must also be preserved for consistency — resetting them to 0
     * while frame_index is 1452+ would confuse the server's flow control.
     * Fresh frames will update these values naturally via rtp_fec_submit. */

    /* H-2: Free cached RS context to prevent leak across reconnects */
    if (s_cached_rs) {
        reed_solomon_release(s_cached_rs);
        s_cached_rs = NULL;
        s_cached_data = 0;
        s_cached_parity = 0;
    }
}

/* ── Parse NV_VIDEO_PACKET fields ────────────────────────────────── */

static inline u32 read_le32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static inline u16 read_be16(const u8 *p)
{
    return (u16)((p[0] << 8) | p[1]);
}

/* Submit all buffered data packets (in order) to the reassembly layer */
static void submit_frame_packets(void)
{
    u32 i;
    int submitted = 0;

    /* ── Synthesize SOF when slot 0 is missing ─────────────────────
     * When the first few WiFi packets of a large frame (IDR) are lost,
     * no SOF flag reaches the reassembly layer, so it never starts
     * assembling. Synthesize a minimal SOF packet from the first
     * received slot's headers so reassembly can proceed. The SPS/PPS
     * are pre-injected in sw_cavlc.c, so the missing initial NAL data
     * is handled by error concealment. */
    if (!g_slots[0].received && g_data_packets > 1) {
        /* Find first received data slot to clone headers from */
        u32 donor = 0;
        for (i = 1; i < g_data_packets && i < FEC_MAX_PACKETS; i++) {
            if (g_slots[i].received && g_slots[i].len > 0) {
                donor = i;
                break;
            }
        }
        if (donor > 0) {
            /* Build synthetic SOF in slot 0's storage:
             * RTP header (12B) + NV_VIDEO_PACKET (16B) + Sunshine hdr (8B)
             * + Annex-B AUD (6B) = 42 bytes minimum */
            u8 *syn = g_slots[0].data;
            int nv_off_d = FIXED_RTP_HEADER_SIZE;
            if (g_slots[donor].data[0] & RTP_FLAG_EXTENSION)
                nv_off_d += 4;

            /* Copy RTP + NV headers from donor */
            int hdr_len = nv_off_d + NV_VIDEO_PKT_SIZE;
            if (hdr_len > MAX_PKT_SIZE) hdr_len = MAX_PKT_SIZE;
            memcpy(syn, g_slots[donor].data, hdr_len);

            /* Fix RTP header: remove extension flag, set seq to lowest */
            syn[0] = 0x80; /* V=2, no padding, no extension, no CSRC */
            syn[2] = (u8)(g_lowest_seq >> 8);
            syn[3] = (u8)(g_lowest_seq);

            /* Fix NV_VIDEO_PACKET: set SOF+PIC flags, fecBlockNum=0 */
            int nv_off = FIXED_RTP_HEADER_SIZE;
            write_le32(syn + nv_off + 4, g_current_frame);
            syn[nv_off + 8] = FLAG_SOF | FLAG_CONTAINS_PIC; /* flags */
            syn[nv_off + 11] = 0; /* multiFecBlocks = 0 */

            /* Sunshine header: type=0x01 (short), 8 bytes total.
             * The reassembly SOF handler will skip 8 bytes. */
            int payload_off = nv_off + NV_VIDEO_PKT_SIZE;
            syn[payload_off] = 0x01; /* hdr_byte */
            memset(syn + payload_off + 1, 0, 7); /* pad rest of sunshine hdr */

            /* After sunshine header skip, append Annex-B AUD NAL.
             * AUD (type=9) is always the first NAL in Sunshine's stream. */
            int h264_off = payload_off + 8;
            syn[h264_off + 0] = 0x00;
            syn[h264_off + 1] = 0x00;
            syn[h264_off + 2] = 0x00;
            syn[h264_off + 3] = 0x01;
            syn[h264_off + 4] = 0x09; /* AUD NAL type */
            syn[h264_off + 5] = 0x10; /* primary_pic_type=0 (I-slice) */

            g_slots[0].len = h264_off + 6;
            g_slots[0].received = 1;

            diag_log_write("FEC", "frame %u: synthesized SOF (slot 0 missing, donor=%u, len=%d)\n",
                           g_current_frame, donor, g_slots[0].len);
        }
    }

    for (i = 0; i < g_data_packets && i < FEC_MAX_PACKETS; i++) {
        if (g_slots[i].received && g_slots[i].len > 0) {
            /* Patch NV_VIDEO_PACKET frame_id to match the expected frame.
             * RS recovery can produce corrupted NV headers — the recovered
             * packet's frame_id might be garbage (e.g. 27392 instead of 13).
             * This prevents a single corrupted recovered packet from poisoning
             * the RTP reassembly's g_last_good_frame dedup counter. */
            int nv_off = FIXED_RTP_HEADER_SIZE;
            if (g_slots[i].data[0] & RTP_FLAG_EXTENSION)
                nv_off += 4;
            if (g_slots[i].len > nv_off + 8) {
                write_le32(g_slots[i].data + nv_off + 4, g_current_frame);
            }

            rtp_reassembly_process_packet(g_slots[i].data, g_slots[i].len);
            submitted++;
        }
    }

    /* Per-frame FEC log silenced for performance */
    g_frame_submitted = 1;

    /* Cache FEC values for CTRL PING piggyback (every frame) and send
     * per-frame FEC status (0x5502) to Sunshine every 3rd frame.
     *
     * THROTTLE RATIONALE: At 30fps, sending FEC status every frame = 30
     * UDP sends/sec on the CTRL socket.  Combined with CTRL PING (5/sec),
     * IDR requests, and RFI messages, this overwhelms the PSP's tiny
     * 802.11b socket send buffer after ~2 minutes.  When sends fail
     * silently (ENOBUFS), the server stops receiving ACKs and its
     * pending-frame counter grows until it hits the threshold and
     * permanently stops sending video (observed at ~130s in 5-min tests).
     *
     * Sending every 3rd frame (10/sec) still gives the server timely
     * ACKs while reducing CTRL socket pressure 3x.  The CTRL PING
     * piggyback (1/sec) provides additional redundancy. */
    {
        static u32 s_fec_send_counter = 0;
        u16 highest_seq = (u16)(g_lowest_seq + g_received_count - 1);
        u16 next_contig = (u16)(g_lowest_seq + g_data_packets - 1);
        u16 received_data = (u16)(g_received_count > g_parity_packets ?
                                  g_received_count - g_parity_packets : g_received_count);
        u16 received_parity = (u16)(g_received_count > received_data ?
                                    g_received_count - received_data : 0);

        /* Always cache — CTRL PING piggyback reads these */
        g_fec_last_highest_seq     = highest_seq;
        g_fec_last_next_contig_seq = next_contig;
        g_fec_last_data_pkts       = (u16)g_data_packets;
        g_fec_last_parity_pkts     = (u16)g_parity_packets;
        g_fec_last_recv_data       = received_data;
        g_fec_last_recv_parity     = received_parity;
        g_fec_last_fec_pct         = (u8)g_fec_percentage;

        s_fec_send_counter++;
        if (s_fec_send_counter >= 3) {
            s_fec_send_counter = 0;
            control_stream_send_fec_status(
                g_current_frame,
                highest_seq,
                next_contig,
                0,
                (u16)g_data_packets,
                (u16)g_parity_packets,
                received_data,
                received_parity,
                (u8)g_fec_percentage,
                0,  /* multiFecBlockIndex */
                1   /* multiFecBlockCount */
            );
        }
    }
}

/* Attempt RS recovery and submit */
static void attempt_recovery_and_submit(void)
{
    u32 missing = 0;
    u32 i;
    int receiveSize;
    int ret;
    reed_solomon *rs;
    unsigned char *packets[FEC_MAX_PACKETS];
    unsigned char  marks[FEC_MAX_PACKETS];

    if (g_data_packets == 0 || g_total_packets == 0 ||
        g_total_packets > FEC_MAX_PACKETS) {
        submit_frame_packets();
        return;
    }

    /* Count missing data packets */
    for (i = 0; i < g_data_packets; i++) {
        if (!g_slots[i].received)
            missing++;
    }

    if (missing == 0) {
        /* All data arrived — just submit */
        s_consec_unrecoverable = 0;
        g_fec_recovery_clean = 1;  /* No recovery needed, data is complete */
        submit_frame_packets();
        return;
    }

    /* Check if we have enough total packets for recovery */
    if (g_received_count < g_data_packets) {
        diag_log_write("FEC", "frame %u unrecoverable: %u received < %u needed (missing %u) — DROPPING\n",
                       g_current_frame, g_received_count, g_data_packets, missing);
        /* DROP the frame entirely.  Submitting partial data causes the
         * CAVLC decoder to hit invalid VLC codes mid-NAL (Run045: IDR
         * frame 7 had 18/49 packets missing, producing 31KB of corrupt
         * bitstream that even OpenH264 couldn't decode).  The decoder's
         * error concealment will repeat the previous reference frame,
         * which looks far better than block-corruption artifacts.
         *
         * IMPORTANT: Do NOT set g_refs_corrupted here.  The dropped frame
         * is never fed to the decoder, so OpenH264's DPB (decoded picture
         * buffer) remains intact.  The encoder's reference chain diverges
         * for 1 frame, but OpenH264's error concealment handles this
         * gracefully (mild motion compensation artifacts vs 60-frame
         * REF-SKIP freeze cycle at 480x272 where IDRs are too rare to
         * clear corruption).
         *
         * History: VQ#15fps had 10 unrecoverable drops, each triggering
         * g_refs_corrupted=1 → 135 REF-SKIPs (4s freeze per cycle).
         * Only 63 frames displayed in 200s.  With this fix, OpenH264
         * decodes continuously with occasional 1-frame artifacts. */
        s_consec_unrecoverable++;

        /* Only escalate to IDR when the decode pipeline is actually broken
         * (g_refs_corrupted) or we have no reference at all.
         *
         * When g_refs_corrupted==0, the dropped frame never entered OpenH264
         * so the DPB is intact.  Requesting IDR wastes bandwidth: at 480x272
         * IDRs are 17-20 data packets (23KB) which almost never survive
         * 802.11b packet loss.  Each failed IDR delivery consumes WiFi
         * bandwidth that should carry P-frames.
         *
         * VQ#15fps-v2: 102 unrecoverable drops triggered 123 IDR requests,
         * 38 "IDR accumulation incomplete" cycles, 0 IDRs delivered.
         * The wasted bandwidth from failed IDR deliveries contributed to
         * progressive quality degradation over the 200s test. */
        if (g_last_good_frame == 0) {
            /* No reference at all — must get an IDR to start decoding */
            g_idr_fully_decoded = 0;
            control_stream_request_idr();
            s_consec_unrecoverable = 0;
        } else if (g_refs_corrupted) {
            /* Decode pipeline is broken — need IDR to reset DPB */
            if (s_consec_unrecoverable >= UNRECOVERABLE_IDR_THRESHOLD) {
                diag_log_write("FEC", "IDR escalation: %d consecutive unrecoverable (refs corrupt)\n",
                               s_consec_unrecoverable);
            } else if ((int)missing >= CATASTROPHIC_LOSS_THRESHOLD) {
                diag_log_write("FEC", "IDR escalation: catastrophic loss (%u missing, refs corrupt)\n",
                               missing);
            }
            g_idr_fully_decoded = 0;
            control_stream_request_idr();
            s_consec_unrecoverable = 0;
        } else {
            /* Refs clean — dropped frame is harmless, don't request IDR.
             * Use lightweight RFI instead: tells the encoder not to
             * reference the dropped frame(s), preventing reference
             * divergence without burning WiFi on a full IDR. */
            control_stream_request_rfi(g_last_good_frame + 1, g_current_frame);
            s_consec_unrecoverable = 0;
        }
        g_frame_submitted = 1; /* prevent timeout re-processing */
        return;
    }

    /* Reset consecutive counter on successful FEC recovery */
    s_consec_unrecoverable = 0;

    /* With intraRefresh enabled, every P-frame self-heals: the refresh
     * cycle progressively corrects any corruption within ~10 frames.
     * So we allow RS recovery even with low surplus — a wrong recovery
     * in one P-frame only affects a few MBs and gets auto-corrected.
     *
     * Previously we required surplus >= missing (run #48: 214 drops,
     * 0 recoveries — too aggressive for small intra-refresh frames).
     * Now: attempt RS whenever we have enough total packets. */

    /* We have enough packets — attempt RS recovery.
     * THROTTLED: log only first 3 + every 120th to avoid filling the
     * 4KB diag_log buffer every ~2s and triggering a synchronous ms0:
     * write that blocks ALL threads for 50-200ms (root cause of lag
     * spikes at 15fps streaming). */
    {
        static u32 s_recovery_attempt_count = 0;
        s_recovery_attempt_count++;
        if (s_recovery_attempt_count <= 3 || (s_recovery_attempt_count % 120) == 0)
            diag_log_write("FEC", "recovering frame %u: %u missing data, %u total received\n",
                           g_current_frame, missing, g_received_count);
    }

    receiveSize = 0;
    for (i = 0; i < g_total_packets; i++) {
        if (g_slots[i].received && g_slots[i].len > receiveSize) {
            receiveSize = g_slots[i].len;
        }
    }
    if (receiveSize == 0) receiveSize = MAX_PKT_SIZE;

    /* Build packet array and marks */
    memset(marks, 1, g_total_packets);
    memset(packets, 0, sizeof(packets));

    for (i = 0; i < g_total_packets; i++) {
        if (g_slots[i].received) {
            packets[i] = g_slots[i].data;
            marks[i] = 0;
            /* Pad short packets to receiveSize */
            if (g_slots[i].len < receiveSize) {
                memset(g_slots[i].data + g_slots[i].len, 0,
                       receiveSize - g_slots[i].len);
            }
        }
    }

    /* Use pre-allocated recovery buffers from g_rec_storage */
    for (i = 0; i < g_total_packets; i++) {
        if (marks[i]) {
            packets[i] = g_rec_storage[i];
            memset(packets[i], 0, receiveSize);
        }
    }

    /* Use cached RS context to avoid per-frame malloc/free.
     * Heap fragmentation from repeated small allocations can destabilize
     * the PSP's limited 12MB heap over long streaming sessions. */
    {
        if (s_cached_rs &&
            s_cached_data == g_data_packets &&
            s_cached_parity == g_parity_packets) {
            rs = s_cached_rs;  /* reuse */
        } else {
            if (s_cached_rs) {
                reed_solomon_release(s_cached_rs);
                s_cached_rs = NULL;
            }
            rs = reed_solomon_new(g_data_packets, g_parity_packets);
            if (!rs) {
                diag_log_write("FEC", "reed_solomon_new failed (%u, %u) -- refs corrupted\n",
                               g_data_packets, g_parity_packets);
                {
                    g_refs_corrupted = 1;
                    g_current_frame_is_corrupt = 1;
                }
                submit_frame_packets();
                return;
            }
            s_cached_rs = rs;
            s_cached_data = g_data_packets;
            s_cached_parity = g_parity_packets;
        }
    }

    ret = reed_solomon_reconstruct(rs, packets, marks, g_total_packets, receiveSize);
    /* Do NOT release rs — keep cached for next frame */

    if (ret == 0) {
        {
            static u32 s_recovery_ok_count = 0;
            s_recovery_ok_count++;
            if (s_recovery_ok_count <= 3 || (s_recovery_ok_count % 120) == 0)
                diag_log_write("FEC", "recovery SUCCESS for frame %u (%u packets recovered)\n",
                               g_current_frame, missing);
        }

        /* Copy recovered packets into slots at full receiveSize.
         *
         * FEC recovery produces packets padded to receiveSize.  Non-last
         * data packets are genuinely receiveSize.  The LAST data packet
         * may have trailing zero padding, but those zeros end up at the
         * END of the assembled Annex-B frame (after all valid NAL data),
         * where the H.264 parser ignores them (RBSP trailing bits / NAL
         * boundary detection stops reading before the padding).
         *
         * Previous code tried to trim trailing zeros, but H.264 bitstreams
         * CAN legitimately contain runs of zero bytes (zero-valued DCT
         * coefficients, exp-Golomb coded zeros, etc.).  Trimming removed
         * real data, shifting the bitstream and causing CAVLC desync.
         */
        for (i = 0; i < g_data_packets; i++) {
            if (marks[i] && packets[i]) {
                memcpy(g_slots[i].data, packets[i], receiveSize);
                g_slots[i].len = receiveSize;
                g_slots[i].received = 1;

                /* Slot-level recovery log silenced for performance.
                 * The frame-level summary above is sufficient. */
            }
        }

        /* FEC recovery is mathematically exact — if RS succeeded, the
         * recovered data is correct.  Don't request preemptive IDR here;
         * if the decode subsequently fails, the error path will request it.
         * Removing this prevents IDR flooding (was triggering 10+ requests
         * per 19s of streaming in Run #17). */
        g_fec_recovery_clean = 1;  /* RS succeeded — data is bit-perfect */
    } else {
        diag_log_write("FEC", "recovery FAILED for frame %u (ret=%d) -- refs corrupted\n",
                       g_current_frame, ret);

        g_fec_recovery_clean = 0;  /* Recovery failed — data is partial */

        /* RS failure means partial data will be submitted with gaps.
         * Mark refs corrupted so the decoder skips P-frames until
         * a clean IDR resets the reference chain. */
        {
            g_refs_corrupted = 1;
            g_current_frame_is_corrupt = 1;
        }

        /* Recovery failed — always escalate to IDR.
         * RFI is useless: the corrupted frame already broke the
         * reference chain, and intra-refresh is always disabled. */
        {
            g_idr_fully_decoded = 0;
        }
        control_stream_request_idr();
    }

    /* Free allocated recovery buffers removed: now using static pool */

    /* Submit all data packets (including recovered ones) */
    submit_frame_packets();
}

/* ── Main packet processing ──────────────────────────────────────── */

int rtp_fec_add_packet(const u8 *packet, int packet_len)
{
    /* Guard: auto-init if caller forgot rtp_fec_init() */
    if (!g_initialized)
        rtp_fec_init();

    int data_offset;
    u32 frame_index, fec_info, fec_index;
    u32 data_pkts, fec_pct, parity_pkts, total_pkts;
    u16 seq;
    u32 index;
    const u8 *nv;

    if (!g_initialized || packet_len < FIXED_RTP_HEADER_SIZE + NV_VIDEO_PKT_SIZE)
        return 0; /* too small, let reassembly handle it */

    /* Parse RTP header */
    data_offset = FIXED_RTP_HEADER_SIZE;
    if (packet[0] & RTP_FLAG_EXTENSION)
        data_offset += 4;

    if (packet_len < data_offset + NV_VIDEO_PKT_SIZE)
        return 0;

    seq = read_be16(packet + 2);
    nv = packet + data_offset;

    /* Parse NV_VIDEO_PACKET */
    frame_index = read_le32(nv + 4);  /* frameIndex */
    fec_info    = read_le32(nv + 12); /* fecInfo */

    /* Extract FEC parameters from fecInfo (matches moonlight-common-c) */
    data_pkts  = (fec_info & 0xFFC00000) >> 22;
    fec_index  = (fec_info & 0x3FF000) >> 12;
    fec_pct    = (fec_info & 0xFF0) >> 4;
    parity_pkts = (data_pkts * fec_pct + 99) / 100;
    total_pkts = data_pkts + parity_pkts;

    /* Protection 1: Ignore massive Frame ID jumps (noise/stray peer traffic) */
    if (g_current_frame != 0xFFFFFFFF && (s32)(frame_index - g_current_frame) > 1000) {
        return 1; /* consume as junk */
    }

    /* Protection 2: Basic sanity on metadata */
    if (data_pkts > FEC_MAX_PACKETS || fec_index >= FEC_MAX_PACKETS) {
        return 1; /* drop corrupted FEC pkt */
    }

    /* Debug: log first few packets to verify FEC parsing */
    {
        static int fec_debug_count = 0;
        if (fec_debug_count < 5) {
            diag_log_write("FEC", "pkt seq=%u frame=%u fecInfo=0x%08X data=%u fecIdx=%u pct=%u parity=%u total=%u\n",
                           seq, frame_index, fec_info, data_pkts, fec_index, fec_pct, parity_pkts, total_pkts);
            fec_debug_count++;
        }
    }

    /* Sanity check */
    if (data_pkts == 0 || total_pkts > FEC_MAX_PACKETS || total_pkts == 0) {
        /* Bypassing FEC... */
        if (fec_index >= data_pkts && data_pkts > 0) {
            /* Drop parity packets — these must NOT reach reassembly */
            return 1; 
        }
        /* If it's a data packet or data_pkts is 0 (non-FEC data), let reassembly handle.
         * But if data_pkts is 0, we still want reassembly's FID jump check to protect us. */
        return 0; 
    }

    /* Frame transition — if new frame, submit previous and reset */
    if (frame_index != g_current_frame) {
        if (g_current_frame != 0xFFFFFFFF && !g_frame_submitted && g_received_count > 0) {
            attempt_recovery_and_submit();
        }

        /* Reset for new frame */
        u32 j;
        for (j = 0; j < FEC_MAX_PACKETS; j++) {
            g_slots[j].len = 0;
            g_slots[j].received = 0;
        }
        g_current_frame = frame_index;
        g_lowest_seq = (u16)(seq - fec_index);
        g_data_packets = data_pkts;
        g_parity_packets = parity_pkts;
        g_total_packets = total_pkts;
        g_fec_percentage = fec_pct;
        g_received_count = 0;
        g_frame_submitted = 0;

        /* Diagnostic: log any frame with many data packets (likely IDR) */
        if (data_pkts > 10) {
            diag_log_write("FEC", "LARGE frame %u: data=%u parity=%u total=%u fecPct=%u\n",
                           frame_index, data_pkts, parity_pkts, total_pkts, fec_pct);
        }
    }

    /* Compute slot index from sequence number */
    index = (u16)(seq - g_lowest_seq);
    if (index >= FEC_MAX_PACKETS || index >= g_total_packets)
        return 0; /* out of range */

    /* Store packet if we don't already have it */
    if (!g_slots[index].received) {
        if (packet_len <= MAX_PKT_SIZE) {
            memcpy(g_slots[index].data, packet, packet_len);
            g_slots[index].len = packet_len;
        } else {
            /* Truncate — shouldn't happen at our configured packet sizes */
            memcpy(g_slots[index].data, packet, MAX_PKT_SIZE);
            g_slots[index].len = MAX_PKT_SIZE;
        }
        g_slots[index].received = 1;
        g_received_count++;
    }

    /* Check if we have enough data packets (or enough total for recovery) to submit now. */
    if (!g_frame_submitted && g_received_count >= g_data_packets) {
        attempt_recovery_and_submit();
    }

    return 1; /* consumed by FEC layer */
}
