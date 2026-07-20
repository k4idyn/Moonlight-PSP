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
#include "rtp_reassembly.h"
#include "rs.h"
#include "diag_log.h"
#include "control_stream.h"
#include "sw_decode_pipeline.h"
#include "decode_flags.h"
#include "signal_strength.h"
#include "config.h"
#include "runtime_telemetry.h"

extern PspConfig g_psp_config;

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
 * Dropped frames are not submitted to OpenH264, so a single network loss
 * must not force a full IDR wait. Escalate only after sustained loss or
 * when the decoder has actually marked refs bad. */
static int s_consec_unrecoverable = 0;
#define UNRECOVERABLE_IDR_THRESHOLD 4

/* Catastrophic single-frame loss threshold — differentiated log message */
#define CATASTROPHIC_LOSS_THRESHOLD 3

/* ── Phase 4: Predictive Frame Drop — sliding window ──────────────
 * Track last 32 frame results to detect WiFi burst loss patterns. Prediction
 * is advisory only: recovery/drop decisions stay tied to actual packet and
 * FEC state so the predictor cannot create IDR storms or visible debug text. */
#define PRED_WINDOW_SIZE  32
#define PRED_RECENT_SIZE  8    /* recent window for burst detection */
#define PRED_BURST_THRESH 4   /* drops in recent window = burst active */

static u8  s_pred_window[PRED_WINDOW_SIZE]; /* 0=ok, 1=dropped */
static int s_pred_idx = 0;
static int s_pred_count = 0;
static u32 s_pred_log_count = 0;

/* ── RTP / NV_VIDEO_PACKET layout (matches moonlight-common-c) ───── */
#define FIXED_RTP_HEADER_SIZE 12
#define RTP_FLAG_EXTENSION    0x10
#define MAX_RTP_HEADER_SIZE   16
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

static inline void write_be16(u8 *p, u16 v)
{
    p[0] = (u8)(v >> 8);
    p[1] = (u8)(v);
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
static u8  g_received_from_network[FEC_MAX_PACKETS];
static u32 g_network_received_count = 0;
static u32 g_received_data_count = 0;
static u32 g_received_parity_count = 0;
static u32 g_highest_received_index = 0;
static u32 g_fec_percentage = 0;
static int g_frame_submitted = 0;
static int g_initialized = 0;

/* CABAC decode can drain RTP faster than PSP WiFi delivers reordered parity.
 * Hold the next frame briefly in FEC order instead of declaring the previous
 * frame unrecoverable as soon as a newer frame's first packet arrives. Wider
 * windows added latency without improving survival on hardware. */
#define CABAC_DEFERRED_PACKET_LIMIT 24
#define CABAC_DEFERRED_FRAME_SPAN_MAX 1U
static u32 s_deferred_frame = 0xFFFFFFFF;
static u32 s_deferred_count = 0;
static u32 s_deferred_overflow_count = 0;
static u8  s_deferred_packets[CABAC_DEFERRED_PACKET_LIMIT][MAX_PKT_SIZE];
static u16 s_deferred_lengths[CABAC_DEFERRED_PACKET_LIMIT];
static u8  s_deferred_replay_packet[MAX_PKT_SIZE];
static int s_replaying_deferred = 0;

/* Last FEC status echoed to server — used by CTRL PING piggyback */
volatile u16 g_fec_last_highest_seq     = 0;
volatile u16 g_fec_last_next_contig_seq = 0;
volatile u16 g_fec_last_data_pkts       = 0;
volatile u16 g_fec_last_parity_pkts     = 0;
volatile u16 g_fec_last_recv_data       = 0;
volatile u16 g_fec_last_recv_parity     = 0;
volatile u16 g_fec_last_missing_before_highest = 0;
volatile u8  g_fec_last_fec_pct         = 0;

/* ── Multi-FEC block support ───────────────────────────────────────
 * When Sunshine splits large frames into multiple FEC blocks
 * (multiFecBlocks > 1), each block is independently recoverable.
 * This is rare at PSP bitrates (<1Mbps) but supported for parity
 * with moonlight-common-c.  Single-block path is unchanged. */
#define MAX_FEC_MULTI_BLOCKS 4

typedef struct {
    u32 data_packets;     /* data shards in this block */
    u32 parity_packets;   /* parity shards */
    u32 total_packets;    /* data + parity */
    u32 received_count;   /* packets received for this block */
    u32 received_data_count; /* data shards received for this block */
    u32 fec_percentage;
    u32 base_slot;        /* starting slot index in g_slots[] */
    u32 max_slots;        /* max slots allocated for this block */
    int seen;             /* 1 = at least one packet seen */
    int recovered;        /* 1 = all data ready (arrived or RS'd) */
} fec_block_t;

static fec_block_t g_fec_blocks[MAX_FEC_MULTI_BLOCKS];
static u32 g_num_fec_blocks = 1;

static void note_current_frame_drop_for_recovery(void)
{
    if (g_last_good_frame != 0 && g_current_frame != 0xFFFFFFFF) {
        rtp_reassembly_note_frame_loss(g_current_frame, g_current_frame);
    }
}

static void request_current_frame_rfi(void)
{
    if (g_current_frame != 0xFFFFFFFF) {
        control_stream_request_rfi(g_current_frame, g_current_frame);
    }
}

/* Total video bytes received — for bandwidth estimation by control_stream.c */
volatile u32 g_fec_total_bytes_received = 0;
volatile u32 g_fec_packets_received = 0;

/* ── Aggregate FEC recovery statistics ───────────────────────────── */
volatile u32 g_fec_packets_recovered  = 0;  /* data packets RS-recovered */
volatile u32 g_fec_packets_failed     = 0;  /* RS recovery failures */
volatile u32 g_fec_frames_dropped     = 0;  /* frames dropped (unrecoverable) */
volatile u32 g_fec_recovery_attempts  = 0;  /* total RS recovery attempts */

/* Phase 3.6: Consecutive drop counter for IDR limit (120) */
volatile u32 g_consecutive_frame_drops = 0;

/* Reassembly callback */
extern void rtp_reassembly_process_packet(u8 *packet, int packet_len);

static void reset_frame_receive_status(void);
static void update_fec_status_cache(void);
static void maybe_send_fec_status(void);
static void mark_frame_packet_received(u32 index, int is_data_packet);
static int rtp_fec_add_packet_inner(const u8 *packet, int packet_len,
                                    int allow_defer);
static void drain_deferred_cabac_packets(void);
#ifndef RETAIL_BUILD
static void log_frame_receive_mask(u32 missing_data_packets);
static void log_late_frame_packet(u32 frame_index,
                                  u32 current_frame,
                                  s32 frame_delta,
                                  u16 seq,
                                  u32 fec_index,
                                  u32 data_pkts,
                                  u32 parity_pkts,
                                  u32 total_pkts);
#endif

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
    reset_frame_receive_status();
    g_current_frame = 0xFFFFFFFF;
    s_deferred_frame = 0xFFFFFFFF;
    s_deferred_count = 0;
    s_replaying_deferred = 0;
    g_initialized = 1;
    g_consecutive_frame_drops = 0;
    diag_log_write("FEC", "initialized\n");
}

/* ── Phase 3.5: Video Stats Getter ──────────────────────────────── */
void rtp_get_video_stats(RtpVideoStats *out)
{
    if (!out) return;
    out->packets_received  = g_fec_packets_received;
    out->packets_recovered = g_fec_packets_recovered;
    out->packets_failed    = g_fec_packets_failed;
    out->frames_dropped    = g_fec_frames_dropped;
    out->recovery_attempts = g_fec_recovery_attempts;
    out->bytes_received    = g_fec_total_bytes_received;
    out->consecutive_drops = g_consecutive_frame_drops;
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
    reset_frame_receive_status();
    g_fec_percentage = 0;
    g_frame_submitted = 0;
    g_fec_recovery_clean = 0;
    s_deferred_frame = 0xFFFFFFFF;
    s_deferred_count = 0;
    s_replaying_deferred = 0;
    s_consec_unrecoverable = 0;
    g_consecutive_frame_drops = 0;
    memset(s_pred_window, 0, sizeof(s_pred_window));
    s_pred_idx = 0;
    s_pred_count = 0;
    s_pred_log_count = 0;

    /* Reset multi-FEC block state */
    g_num_fec_blocks = 1;
    {
        int b;
        for (b = 0; b < MAX_FEC_MULTI_BLOCKS; b++) {
            g_fec_blocks[b].seen = 0;
            g_fec_blocks[b].recovered = 0;
            g_fec_blocks[b].received_count = 0;
            g_fec_blocks[b].received_data_count = 0;
        }
    }

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

static void reset_frame_receive_status(void)
{
    memset(g_received_from_network, 0, sizeof(g_received_from_network));
    g_network_received_count = 0;
    g_received_data_count = 0;
    g_received_parity_count = 0;
    g_highest_received_index = 0;
}

static void mark_frame_packet_received(u32 index, int is_data_packet)
{
    if (index >= FEC_MAX_PACKETS || g_received_from_network[index])
        return;

    g_received_from_network[index] = 1;
    if (g_network_received_count == 0 || index > g_highest_received_index)
        g_highest_received_index = index;
    g_network_received_count++;

    if (is_data_packet)
        g_received_data_count++;
    else
        g_received_parity_count++;
}

static u32 packet_frame_index(const u8 *packet, int packet_len)
{
    int data_offset = FIXED_RTP_HEADER_SIZE;
    const u8 *nv;

    if (!packet || packet_len < FIXED_RTP_HEADER_SIZE + NV_VIDEO_PKT_SIZE)
        return 0xFFFFFFFF;

    if (packet[0] & RTP_FLAG_EXTENSION) {
        if (packet_len < data_offset + 4)
            return 0xFFFFFFFF;
        data_offset += 4;
    }

    if (packet_len < data_offset + NV_VIDEO_PKT_SIZE)
        return 0xFFFFFFFF;

    nv = packet + data_offset;
    return read_le32(nv + 4);
}

static void refresh_deferred_front_frame(void)
{
    if (s_deferred_count == 0) {
        s_deferred_frame = 0xFFFFFFFF;
        return;
    }

    s_deferred_frame = packet_frame_index(s_deferred_packets[0],
                                          s_deferred_lengths[0]);
}

static u32 count_missing_current_data_packets(void)
{
    u32 missing = 0;
    u32 i;

    for (i = 0; i < g_data_packets && i < FEC_MAX_PACKETS; i++) {
        if (!g_slots[i].received)
            missing++;
    }

    return missing;
}

static int current_frame_needs_cabac_defer(void)
{
    u32 missing;

    if (!g_psp_config.cabacTestMode ||
        g_current_frame == 0xFFFFFFFF ||
        g_frame_submitted ||
        g_received_count == 0 ||
        g_data_packets == 0) {
        return 0;
    }

    if (g_num_fec_blocks > 1) {
        int b;
        for (b = 0; b < (int)g_num_fec_blocks; b++) {
            fec_block_t *blk = &g_fec_blocks[b];
            if (!blk->seen ||
                blk->received_data_count < blk->data_packets ||
                blk->received_count < blk->data_packets) {
                return 1;
            }
        }
        return 0;
    }

    missing = count_missing_current_data_packets();
    if (missing == 0)
        return 0;

    if (g_received_count < g_data_packets)
        return 1;

    return g_received_parity_count < missing;
}

static int defer_cabac_newer_packet(const u8 *packet, int packet_len,
                                    u32 frame_index)
{
    u32 span;

    if (packet_len <= 0 || packet_len > MAX_PKT_SIZE)
        return 0;

    if (s_deferred_count == 0)
        s_deferred_frame = frame_index;

    span = (g_current_frame != 0xFFFFFFFF &&
            (s32)(frame_index - g_current_frame) > 0) ?
           (u32)(frame_index - g_current_frame) : 0U;

    if (span == 0U ||
        span > CABAC_DEFERRED_FRAME_SPAN_MAX ||
        s_deferred_count >= CABAC_DEFERRED_PACKET_LIMIT) {
        s_deferred_overflow_count++;
#ifndef RETAIL_BUILD
        if (s_deferred_overflow_count <= 3 ||
            (s_deferred_overflow_count % 60) == 0) {
            diag_log_write("FEC",
                           "CABAC defer window full/span cur=%u new=%u span=%u pendingFrame=%u pending=%u overflow=%u\n",
                           g_current_frame, frame_index, span,
                           s_deferred_frame, s_deferred_count,
                           s_deferred_overflow_count);
        }
#endif
        return 0;
    }

    memcpy(s_deferred_packets[s_deferred_count], packet, packet_len);
    s_deferred_lengths[s_deferred_count] = (u16)packet_len;
    s_deferred_count++;

#ifndef RETAIL_BUILD
    if (s_deferred_count <= 3 || s_deferred_count == CABAC_DEFERRED_PACKET_LIMIT) {
        diag_log_write("FEC",
                       "CABAC deferred newer packet frame=%u while completing frame=%u span=%u pending=%u recv=%u/%u dataRecv=%u parityRecv=%u\n",
                       frame_index, g_current_frame, span, s_deferred_count,
                       g_received_count, g_data_packets,
                       g_received_data_count, g_received_parity_count);
    }
#endif
    return 1;
}

static void drop_submitted_deferred_frame_packets(void)
{
    while (s_deferred_count > 0 &&
           g_frame_submitted &&
           s_deferred_frame == g_current_frame) {
        u32 i;
        for (i = 1; i < s_deferred_count; i++) {
            memcpy(s_deferred_packets[i - 1],
                   s_deferred_packets[i],
                   s_deferred_lengths[i]);
            s_deferred_lengths[i - 1] = s_deferred_lengths[i];
        }
        s_deferred_count--;
        refresh_deferred_front_frame();
    }
}

static void drain_deferred_cabac_packets(void)
{
    if (s_replaying_deferred)
        return;

    s_replaying_deferred = 1;
    while (s_deferred_count > 0) {
        int can_replay = 0;
        int replay_len;
        u32 i;

        drop_submitted_deferred_frame_packets();
        if (s_deferred_count == 0)
            break;

        if (g_current_frame == 0xFFFFFFFF ||
            g_frame_submitted ||
            s_deferred_frame == g_current_frame) {
            can_replay = 1;
        }
        if (!can_replay)
            break;

        replay_len = (int)s_deferred_lengths[0];
        memcpy(s_deferred_replay_packet, s_deferred_packets[0], replay_len);
        for (i = 1; i < s_deferred_count; i++) {
            memcpy(s_deferred_packets[i - 1],
                   s_deferred_packets[i],
                   s_deferred_lengths[i]);
            s_deferred_lengths[i - 1] = s_deferred_lengths[i];
        }
        s_deferred_count--;
        refresh_deferred_front_frame();

        (void)rtp_fec_add_packet_inner(s_deferred_replay_packet,
                                       replay_len,
                                       0);
    }
    s_replaying_deferred = 0;
}

#ifndef RETAIL_BUILD
static void log_frame_receive_mask(u32 missing_data_packets)
{
    u32 mask0 = 0;
    u32 mask1 = 0;
    u32 mask2 = 0;
    u32 mask3 = 0;
    u32 limit = g_total_packets;
    u32 i;

    if (limit > FEC_MAX_PACKETS)
        limit = FEC_MAX_PACKETS;

    for (i = 0; i < limit; i++) {
        if (!g_received_from_network[i])
            continue;

        if (i < 32U)
            mask0 |= (1U << i);
        else if (i < 64U)
            mask1 |= (1U << (i - 32U));
        else if (i < 96U)
            mask2 |= (1U << (i - 64U));
        else if (i < 128U)
            mask3 |= (1U << (i - 96U));
    }

    diag_log_write("FEC",
                   "RXMAP frame=%u baseSeq=%u data=%u parity=%u total=%u recv=%u dataRecv=%u parityRecv=%u highest=%u missingData=%u rxmask=%08X%08X%08X%08X\n",
                   g_current_frame,
                   (unsigned)g_lowest_seq,
                   g_data_packets,
                   g_parity_packets,
                   g_total_packets,
                   g_received_count,
                   g_received_data_count,
                   g_received_parity_count,
                   g_highest_received_index,
                   missing_data_packets,
                   mask3, mask2, mask1, mask0);
}

static void log_late_frame_packet(u32 frame_index,
                                  u32 current_frame,
                                  s32 frame_delta,
                                  u16 seq,
                                  u32 fec_index,
                                  u32 data_pkts,
                                  u32 parity_pkts,
                                  u32 total_pkts)
{
    static u32 s_late_log_count = 0;

    if (frame_delta < -8 || frame_delta >= 0)
        return;

    s_late_log_count++;
    if (s_late_log_count <= 20 || (s_late_log_count % 100) == 0) {
        diag_log_write("FEC",
                       "LATEPKT frame=%u current=%u delta=%d seq=%u slot=%u data=%u parity=%u total=%u count=%u\n",
                       frame_index,
                       current_frame,
                       (int)frame_delta,
                       (unsigned)seq,
                       fec_index,
                       data_pkts,
                       parity_pkts,
                       total_pkts,
                       s_late_log_count);
    }
}
#endif

static void update_fec_status_cache(void)
{
    u32 scan_limit = g_total_packets;
    u32 next_idx = 0;
    u32 missing = 0;
    u32 i;

    if (scan_limit == 0 || scan_limit > FEC_MAX_PACKETS)
        scan_limit = FEC_MAX_PACKETS;
    if (g_network_received_count > 0 &&
        g_highest_received_index + 1 > scan_limit)
        scan_limit = g_highest_received_index + 1;

    if (g_network_received_count > 0) {
        while (next_idx < scan_limit && g_received_from_network[next_idx])
            next_idx++;

        for (i = 0; i < g_highest_received_index && i < scan_limit; i++) {
            if (!g_received_from_network[i])
                missing++;
        }

        g_fec_last_highest_seq =
            (u16)(g_lowest_seq + g_highest_received_index);
        g_fec_last_next_contig_seq = (u16)(g_lowest_seq + next_idx);
    } else {
        g_fec_last_highest_seq = g_lowest_seq;
        g_fec_last_next_contig_seq = g_lowest_seq;
    }

    g_fec_last_missing_before_highest =
        (u16)((missing > 0xFFFFU) ? 0xFFFFU : missing);
    g_fec_last_data_pkts = (u16)g_data_packets;
    g_fec_last_parity_pkts = (u16)g_parity_packets;
    g_fec_last_recv_data = (u16)g_received_data_count;
    g_fec_last_recv_parity = (u16)g_received_parity_count;
    g_fec_last_fec_pct = (u8)g_fec_percentage;
}

static void maybe_send_fec_status(void)
{
    static u32 s_fec_send_counter = 0;

    update_fec_status_cache();

    s_fec_send_counter++;
    if (s_fec_send_counter < 3)
        return;

    s_fec_send_counter = 0;
    control_stream_send_fec_status(
        g_current_frame,
        g_fec_last_highest_seq,
        g_fec_last_next_contig_seq,
        g_fec_last_missing_before_highest,
        g_fec_last_data_pkts,
        g_fec_last_parity_pkts,
        g_fec_last_recv_data,
        g_fec_last_recv_parity,
        g_fec_last_fec_pct,
        0,  /* multiFecBlockIndex */
        1   /* multiFecBlockCount */
    );
}

static int rtp_nv_offset_from_packet(const u8 *packet, int packet_len)
{
    int off = FIXED_RTP_HEADER_SIZE;
    if (!packet || packet_len < off)
        return -1;

    /* GameStream video packets match moonlight-common-c's fixed RTP
     * extension handling: the X bit means exactly one 4-byte extension
     * header before NV_VIDEO_PACKET. Do not trust the extension length word
     * here; recovered RTP headers may contain zero/stale extension fields
     * until we patch them from a real packet. */
    if (packet[0] & RTP_FLAG_EXTENSION) {
        if (packet_len < off + 4)
            return -1;
        off += 4;
    }
    if (packet_len < off + NV_VIDEO_PKT_SIZE)
        return -1;
    return off;
}

static int fec_configured_packet_size(void)
{
    int packet_size = g_psp_config.packetSize > 0 ?
                      g_psp_config.packetSize : DEFAULT_PACKET_SIZE;

    packet_size -= (packet_size % 16);
    if (packet_size < MIN_STREAM_PACKET_SIZE)
        packet_size = MIN_STREAM_PACKET_SIZE;
    if (packet_size > MAX_STREAM_PACKET_SIZE)
        packet_size = MAX_STREAM_PACKET_SIZE;
    return packet_size;
}

static int fec_recovery_symbol_size(void)
{
    int packet_size = fec_configured_packet_size();
    int receive_size = packet_size + MAX_RTP_HEADER_SIZE;

    if (receive_size > MAX_PKT_SIZE)
        receive_size = MAX_PKT_SIZE;
    if (receive_size < FIXED_RTP_HEADER_SIZE + NV_VIDEO_PKT_SIZE)
        receive_size = FIXED_RTP_HEADER_SIZE + NV_VIDEO_PKT_SIZE;
    return receive_size;
}

static int fec_recovered_data_len(const u8 *packet, int receive_size)
{
    int off = FIXED_RTP_HEADER_SIZE;
    int packet_size = fec_configured_packet_size();
    int len;

    if (packet && (packet[0] & RTP_FLAG_EXTENSION)) {
        off += 4;
    }

    len = off + packet_size;
    if (len > receive_size)
        len = receive_size;
    if (len > MAX_PKT_SIZE)
        len = MAX_PKT_SIZE;
    return len;
}

static const u8 *fec_slot_nv(u32 slot, int *nv_off_out)
{
    int nv_off;

    if (slot >= FEC_MAX_PACKETS || !g_slots[slot].received ||
        g_slots[slot].len <= 0) {
        return NULL;
    }

    nv_off = rtp_nv_offset_from_packet(g_slots[slot].data, g_slots[slot].len);
    if (nv_off < 0) {
        return NULL;
    }

    if (nv_off_out) {
        *nv_off_out = nv_off;
    }
    return g_slots[slot].data + nv_off;
}

static u32 fec_nv_stream_packet_index(const u8 *nv)
{
    return (read_le32(nv) >> 8) & 0x00FFFFFFU;
}

static void fec_patch_recovered_packet_header(u32 slot, u32 block_num,
                                              u32 last_block_num)
{
    const u8 *ref = NULL;
    u8 *dst;
    u8 *nv;
    int nv_off;
    u32 i;

    if (slot >= FEC_MAX_PACKETS || !g_slots[slot].received ||
        g_slots[slot].len < FIXED_RTP_HEADER_SIZE) {
        return;
    }

    for (i = 0; i < g_total_packets && i < FEC_MAX_PACKETS; i++) {
        if (i != slot && g_received_from_network[i] &&
            g_slots[i].received &&
            g_slots[i].len >= FIXED_RTP_HEADER_SIZE) {
            ref = g_slots[i].data;
            break;
        }
    }
    if (!ref) {
        for (i = 0; i < g_total_packets && i < FEC_MAX_PACKETS; i++) {
            if (i != slot && g_slots[i].received &&
                g_slots[i].len >= FIXED_RTP_HEADER_SIZE) {
                ref = g_slots[i].data;
                break;
            }
        }
    }
    if (!ref)
        return;

    dst = g_slots[slot].data;

    /* Align with moonlight-common-c recovered video packet handling. The
     * Reed-Solomon shard covers the video payload, but RTP header fields are
     * reconstructed from the surrounding frame state before the NV header is
     * parsed or handed to reassembly. */
    dst[0] = ref[0];
    dst[1] = (u8)((dst[1] & 0x80) | (ref[1] & 0x7F));
    write_be16(dst + 2, (u16)(g_lowest_seq + slot));
    memcpy(dst + 4, ref + 4, 8);

    nv_off = rtp_nv_offset_from_packet(dst, g_slots[slot].len);
    if (nv_off < 0)
        return;

    nv = dst + nv_off;
    write_le32(nv + 4, g_current_frame);
    nv[11] = (u8)((((last_block_num & 0x03U) << 2) |
                   (block_num & 0x03U)) << 4);
}

static void fec_log_recovered_packet_reject(u32 slot, const char *reason, u8 flags)
{
    static u32 s_reject_log_count = 0;

    s_reject_log_count++;
    if (s_reject_log_count <= 5 || (s_reject_log_count % 60) == 0) {
        int nv_off = -1;
        u32 spi = 0;
        u32 frame = 0;
        u32 fec_info = 0;
        u8 mf = 0;

        if (slot < FEC_MAX_PACKETS && g_slots[slot].received) {
            nv_off = rtp_nv_offset_from_packet(g_slots[slot].data,
                                               g_slots[slot].len);
            if (nv_off >= 0) {
                const u8 *nv = g_slots[slot].data + nv_off;
                spi = fec_nv_stream_packet_index(nv);
                frame = read_le32(nv + 4);
                mf = nv[11];
                fec_info = read_le32(nv + 12);
            }
        }

        diag_log_write("FEC",
                       "recovered packet reject frame=%u slot=%u flags=0x%02X reason=%s len=%d off=%d spi=%u nvFrame=%u mf=0x%02X fec=0x%08X\n",
                       g_current_frame, slot, flags,
                       reason ? reason : "invalid",
                       (slot < FEC_MAX_PACKETS) ? g_slots[slot].len : 0,
                       nv_off, spi, frame, mf, fec_info);
    }
}

static int fec_validate_recovered_data_packet(u32 slot, u32 data_packets)
{
    const u8 *nv;
    const u8 *adj_nv;
    u8 flags;

    nv = fec_slot_nv(slot, NULL);
    if (!nv) {
        fec_log_recovered_packet_reject(slot, "missing NV header", 0);
        return 0;
    }

    flags = nv[8];
    if (flags & ~(FLAG_SOF | FLAG_EOF | FLAG_CONTAINS_PIC)) {
        fec_log_recovered_packet_reject(slot, "invalid flag bits", flags);
        return 0;
    }

    if (slot == 0 && !(flags & FLAG_SOF)) {
        fec_log_recovered_packet_reject(slot, "missing SOF", flags);
        return 0;
    }
    if (slot != 0 && (flags & FLAG_SOF)) {
        fec_log_recovered_packet_reject(slot, "unexpected SOF", flags);
        return 0;
    }

    if (slot + 1 == data_packets) {
        if (!(flags & FLAG_EOF)) {
            fec_log_recovered_packet_reject(slot, "missing EOF", flags);
            return 0;
        }
    } else if (flags & FLAG_EOF) {
        fec_log_recovered_packet_reject(slot, "early EOF", flags);
        return 0;
    }

    if (slot > 0 && slot + 1 < data_packets &&
        !(flags & FLAG_CONTAINS_PIC)) {
        fec_log_recovered_packet_reject(slot, "missing picture data", flags);
        return 0;
    }

    if (slot > 0) {
        adj_nv = fec_slot_nv(slot - 1, NULL);
        if (adj_nv) {
            u32 prev_spi = fec_nv_stream_packet_index(adj_nv);
            u32 this_spi = fec_nv_stream_packet_index(nv);
            if (this_spi != ((prev_spi + 1U) & 0x00FFFFFFU)) {
                fec_log_recovered_packet_reject(slot, "SPI gap from previous", flags);
                return 0;
            }
        }
    }

    if (slot + 1 < data_packets) {
        adj_nv = fec_slot_nv(slot + 1, NULL);
        if (adj_nv) {
            u32 this_spi = fec_nv_stream_packet_index(nv);
            u32 next_spi = fec_nv_stream_packet_index(adj_nv);
            if (next_spi != ((this_spi + 1U) & 0x00FFFFFFU)) {
                fec_log_recovered_packet_reject(slot, "SPI gap to next", flags);
                return 0;
            }
        }
    }

    return 1;
}

static void drop_current_frame_strict(const char *reason, u32 missing)
{
    update_fec_status_cache();
    control_stream_send_fec_status(
        g_current_frame,
        g_fec_last_highest_seq,
        g_fec_last_next_contig_seq,
        g_fec_last_missing_before_highest,
        g_fec_last_data_pkts,
        g_fec_last_parity_pkts,
        g_fec_last_recv_data,
        g_fec_last_recv_parity,
        g_fec_last_fec_pct,
        0,
        1);

    g_fec_frames_dropped++;
    g_consecutive_frame_drops++;
    s_pred_window[s_pred_idx] = 1;
    s_pred_idx = (s_pred_idx + 1) % PRED_WINDOW_SIZE;
    if (s_pred_count < PRED_WINDOW_SIZE) s_pred_count++;
    signal_strength_report_frame_drop();

    diag_log_write("FEC", "frame %u dropped: %s (missing=%u recv=%u/%u total_dropped=%u consec=%u)\n",
                   g_current_frame, reason ? reason : "invalid",
                   missing, g_received_count, g_data_packets,
                   g_fec_frames_dropped, g_consecutive_frame_drops);

    g_fec_recovery_clean = 0;
    g_frame_submitted = 1;

    note_current_frame_drop_for_recovery();

    if (g_last_good_frame == 0 || g_refs_corrupted ||
        g_consecutive_frame_drops >= CONSECUTIVE_DROP_IDR_LIMIT) {
        g_idr_fully_decoded = 0;
        g_fec_requested_idr = 1;
        control_stream_request_idr();
        if (g_consecutive_frame_drops >= CONSECUTIVE_DROP_IDR_LIMIT)
            g_consecutive_frame_drops = 0;
    } else {
        request_current_frame_rfi();
    }
}

/* Dynamic sanity bound for FEC data packet count.
 * Uses configured bitrate/fps with a generous burst factor for IDR spikes,
 * while still rejecting obviously corrupted metadata. */
static u32 fec_max_reasonable_data_packets(void)
{
    u32 fps = (g_psp_config.fps > 0) ? (u32)g_psp_config.fps : 15U;
    u32 bitrate_kbps = (g_psp_config.bitrate > 0) ? (u32)g_psp_config.bitrate : 500U;
    u32 avg_bytes_per_frame = (bitrate_kbps * 1000U / 8U) / fps;
    u32 avg_pkts = avg_bytes_per_frame / 900U; /* conservative RTP payload estimate */
    u32 max_pkts;

    if (avg_pkts < 1U) avg_pkts = 1U;
    max_pkts = avg_pkts * 20U; /* allow large transient IDR bursts */
    if (max_pkts < 32U) max_pkts = 32U;
    if (max_pkts > (u32)FEC_MAX_PACKETS) max_pkts = (u32)FEC_MAX_PACKETS;
    return max_pkts;
}

/* Submit all buffered data packets (in order) to the reassembly layer */
static void submit_frame_packets(void)
{
    u32 i;
    int submitted = 0;

    rtp_reassembly_prepare_fec_frame(g_current_frame);

    /* ── Multi-block submission ─────────────────────────────────────── */
    if (g_num_fec_blocks > 1) {
        u32 b;
        for (b = 0; b < g_num_fec_blocks; b++) {
            fec_block_t *blk = &g_fec_blocks[b];
            if (!blk->seen) continue;
            for (i = 0; i < blk->data_packets && i < blk->max_slots; i++) {
                u32 slot = blk->base_slot + i;
                if (slot < FEC_MAX_PACKETS && g_slots[slot].received &&
                    g_slots[slot].len > 0) {
                    int nv_off = rtp_nv_offset_from_packet(g_slots[slot].data,
                                                           g_slots[slot].len);
                    if (nv_off >= 0 && g_slots[slot].len > nv_off + 8)
                        write_le32(g_slots[slot].data + nv_off + 4,
                                   g_current_frame);
                    rtp_reassembly_process_packet(g_slots[slot].data,
                                                  g_slots[slot].len);
                    submitted++;
                }
            }
        }
        g_frame_submitted = 1;
        maybe_send_fec_status();
        return;
    }

    /* ── Synthesize SOF when slot 0 is missing ─────────────────────
     * Missing SOF cannot be recovered deterministically, so do not feed
     * synthetic partial H.264 into the decoder. */
    if (!g_slots[0].received && g_data_packets > 1) {
        drop_current_frame_strict("missing SOF packet", 1);
        return;
    }

    for (i = 0; i < g_data_packets && i < FEC_MAX_PACKETS; i++) {
        if (g_slots[i].received && g_slots[i].len > 0) {
            /* Patch NV_VIDEO_PACKET frame_id to match the expected frame.
             * RS recovery can produce corrupted NV headers — the recovered
             * packet's frame_id might be garbage (e.g. 27392 instead of 13).
             * This prevents a single corrupted recovered packet from poisoning
             * the RTP reassembly's g_last_good_frame dedup counter. */
            int nv_off = rtp_nv_offset_from_packet(g_slots[i].data,
                                                   g_slots[i].len);
            if (nv_off >= 0 && g_slots[i].len > nv_off + 8) {
                write_le32(g_slots[i].data + nv_off + 4, g_current_frame);
            }

            rtp_reassembly_process_packet(g_slots[i].data, g_slots[i].len);
            submitted++;
        }
    }

    /* Per-frame FEC log silenced for performance */
    g_frame_submitted = 1;

    if (g_num_fec_blocks <= 1 && g_total_packets > 0) {
        rtp_reassembly_note_fec_frame_complete((u16)(g_lowest_seq + g_total_packets));
    }

    /* Periodic FEC summary every ~30s (900 frames at 30fps) */
#ifndef RETAIL_BUILD
    {
        static u32 s_fec_summary_count = 0;
        s_fec_summary_count++;
        if (s_fec_summary_count % 900 == 0) {
            u32 total_attempts = g_fec_recovery_attempts;
            u32 total_ok = g_fec_packets_recovered;
            u32 total_fail = g_fec_packets_failed;
            u32 total_drop = g_fec_frames_dropped;
            u32 pct = (total_attempts > 0 && total_attempts > total_fail) ?
                      ((total_attempts - total_fail) * 100 / total_attempts) :
                      (total_attempts > 0 ? 0 : 100);
            diag_log_write("FEC", "STATS: recovered=%u failed=%u dropped=%u attempts=%u success=%u%%\n",
                           total_ok, total_fail, total_drop, total_attempts, pct);
        }
    }
#endif

    /* Cache FEC values for CTRL PING piggyback (every frame) and send
     * periodic per-frame FEC status (0x5502) to Sunshine.
     *
     * THROTTLE RATIONALE: At 30fps, sending FEC status every frame = 30
     * UDP sends/sec on the CTRL socket.  Combined with CTRL PING (5/sec),
     * IDR requests, and RFI messages, this overwhelms the PSP's tiny
     * 802.11b socket send buffer after ~2 minutes.  When sends fail
     * silently (ENOBUFS), the server stops receiving ACKs and its
     * pending-frame counter grows until it hits the threshold and
     * permanently stops sending video (observed at ~130s in 5-min tests).
     *
     * Sending every 5th frame keeps the server informed while reducing
     * CTRL socket pressure on PSP-1000 WiFi. */
    {
        static u32 s_fec_send_counter = 0;
        {
            u32 highest_seq = g_lowest_seq;
            u32 next_contig = g_lowest_seq;
            u32 received_data = g_received_count;
            u32 received_parity = 0;

            if (g_received_count > 0)
                highest_seq = g_lowest_seq + g_received_count - 1;
            if (g_data_packets > 0)
                next_contig = g_lowest_seq + g_data_packets - 1;
            if (received_data > g_data_packets) {
                received_parity = received_data - g_data_packets;
                received_data = g_data_packets;
            }

            g_fec_last_highest_seq = (u16)highest_seq;
            g_fec_last_next_contig_seq = (u16)next_contig;
            g_fec_last_missing_before_highest = 0;
            g_fec_last_data_pkts = (u16)g_data_packets;
            g_fec_last_parity_pkts = (u16)g_parity_packets;
            g_fec_last_recv_data = (u16)received_data;
            g_fec_last_recv_parity = (u16)received_parity;
            g_fec_last_fec_pct = (u8)g_fec_percentage;
        }

        s_fec_send_counter++;
        if (s_fec_send_counter >= 5) {
            s_fec_send_counter = 0;
            control_stream_send_fec_status(
                g_current_frame,
                g_fec_last_highest_seq,
                g_fec_last_next_contig_seq,
                g_fec_last_missing_before_highest,
                g_fec_last_data_pkts,
                g_fec_last_parity_pkts,
                g_fec_last_recv_data,
                g_fec_last_recv_parity,
                g_fec_last_fec_pct,
                0,  /* multiFecBlockIndex */
                1   /* multiFecBlockCount */
            );
        }
    }
}

/* ── Multi-FEC block recovery ─────────────────────────────────────
 * Each FEC block is independently recoverable.  Iterate over all
 * blocks, attempt RS recovery for each, then submit the frame.
 * Only called when g_num_fec_blocks > 1. */
static void attempt_multi_block_recovery_and_submit(void)
{
    int b;
    int any_unrecoverable = 0;
    int any_rs_failed = 0;

    for (b = 0; b < (int)g_num_fec_blocks; b++) {
        fec_block_t *blk = &g_fec_blocks[b];
        u32 missing = 0;
        u32 i;

        if (!blk->seen || blk->total_packets == 0) {
            any_unrecoverable = 1;
            continue;
        }

        /* Count missing data packets in this block */
        for (i = 0; i < blk->data_packets && i < blk->max_slots; i++) {
            u32 slot = blk->base_slot + i;
            if (slot < FEC_MAX_PACKETS && !g_slots[slot].received)
                missing++;
        }

        if (missing == 0) {
            blk->recovered = 1;
            continue;
        }

        /* Check if we have enough packets for recovery */
        if (blk->received_count < blk->data_packets) {
            diag_log_write("FEC", "frame %u blk %d unrecoverable: %u/%u\n",
                           g_current_frame, b, blk->received_count,
                           blk->data_packets);
            any_unrecoverable = 1;
            continue;
        }

        /* RS recovery for this block */
        {
            reed_solomon *rs;
            unsigned char *packets[FEC_MAX_PACKETS];
            unsigned char marks[FEC_MAX_PACKETS];
            int receiveSize;
            int ret;

            receiveSize = fec_recovery_symbol_size();

            memset(marks, 1, blk->total_packets);
            memset(packets, 0, sizeof(unsigned char *) * blk->total_packets);

            for (i = 0; i < blk->total_packets && i < blk->max_slots; i++) {
                u32 slot = blk->base_slot + i;
                if (slot < FEC_MAX_PACKETS && g_slots[slot].received) {
                    packets[i] = g_slots[slot].data;
                    marks[i] = 0;
                    if (g_slots[slot].len < receiveSize)
                        memset(g_slots[slot].data + g_slots[slot].len, 0,
                               receiveSize - g_slots[slot].len);
                }
            }

            /* Assign recovery buffers for missing slots */
            for (i = 0; i < blk->total_packets && i < blk->max_slots; i++) {
                if (marks[i]) {
                    u32 slot = blk->base_slot + i;
                    if (slot < FEC_MAX_PACKETS) {
                        packets[i] = g_rec_storage[slot];
                        memset(packets[i], 0, receiveSize);
                    }
                }
            }

            /* RS context: rs.c uses one static context shared with audio RTP
             * FEC, so hold the global lock across configure+reconstruct and
             * verify the actual context shape before reusing the cached ptr. */
            reed_solomon_global_lock();
            if (s_cached_rs &&
                s_cached_data == blk->data_packets &&
                s_cached_parity == blk->parity_packets &&
                s_cached_rs->data_shards == (int)blk->data_packets &&
                s_cached_rs->parity_shards == (int)blk->parity_packets) {
                rs = s_cached_rs;
            } else {
                if (s_cached_rs) {
                    reed_solomon_release(s_cached_rs);
                    s_cached_rs = NULL;
                }
                rs = reed_solomon_new(blk->data_packets, blk->parity_packets);
                if (!rs) {
                    reed_solomon_global_unlock();
                    any_rs_failed = 1;
                    continue;
                }
                s_cached_rs = rs;
                s_cached_data = blk->data_packets;
                s_cached_parity = blk->parity_packets;
            }

            ret = reed_solomon_reconstruct(rs, packets, marks,
                                           blk->total_packets, receiveSize);
            reed_solomon_global_unlock();
            g_fec_recovery_attempts++;
            if (ret == 0) {
                for (i = 0; i < blk->data_packets && i < blk->max_slots; i++) {
                    u32 slot = blk->base_slot + i;
                    if (slot < FEC_MAX_PACKETS && marks[i] && packets[i]) {
                        int recovered_len;
                        memcpy(g_slots[slot].data, packets[i], receiveSize);
                        g_slots[slot].received = 1;
                        g_slots[slot].len = receiveSize;
                        fec_patch_recovered_packet_header(
                            slot, (u32)b,
                            (g_num_fec_blocks > 0) ? g_num_fec_blocks - 1 : 0);
                        recovered_len = fec_recovered_data_len(g_slots[slot].data,
                                                               receiveSize);
                        g_slots[slot].len = recovered_len;
                        g_fec_packets_recovered++;
                    }
                }
                blk->recovered = 1;
            } else {
                any_rs_failed = 1;
                g_fec_packets_failed += missing;
                diag_log_write("FEC", "frame %u blk %d RS failed (ret=%d)\n",
                               g_current_frame, b, ret);
            }
        }
    }

    if (any_unrecoverable || any_rs_failed) {
        drop_current_frame_strict(any_rs_failed ?
                                  "multi-block RS failed" :
                                  "multi-block missing packets",
                                  1);
        return;
    }

    s_consec_unrecoverable = 0;
    g_fec_recovery_clean = 1;

    submit_frame_packets();
}

/* Attempt RS recovery and submit */
static void attempt_recovery_and_submit(void)
{
    /* Delegate multi-block recovery to dedicated handler */
    if (g_num_fec_blocks > 1) {
        attempt_multi_block_recovery_and_submit();
        return;
    }

    /* Phase 4: Predictive frame drop — skip FEC if burst loss predicted */
    {
        int predicted = rtp_fec_get_predicted_loss();
        if (predicted >= PRED_BURST_THRESH) {
            s_pred_log_count++;
            if (s_pred_log_count <= 3 || (s_pred_log_count % 60) == 0) {
                diag_log_write("FEC", "burst advisory: predicted %d, continuing FEC\n", predicted);
            }
        }
    }

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
        g_consecutive_frame_drops = 0;  /* Phase 3.6: reset on success */
        g_fec_recovery_clean = 1;  /* No recovery needed, data is complete */
        /* Phase 4: Record success in prediction window */
        s_pred_window[s_pred_idx] = 0;
        s_pred_idx = (s_pred_idx + 1) % PRED_WINDOW_SIZE;
        if (s_pred_count < PRED_WINDOW_SIZE) s_pred_count++;
        signal_strength_report_frame_ok();
        submit_frame_packets();
        return;
    }

    /* Check if we have enough total packets for recovery */
    if (g_received_count < g_data_packets) {
        g_fec_packets_failed += missing;
        g_fec_frames_dropped++;
        g_consecutive_frame_drops++;
        /* Phase 4: Record drop in prediction window + adaptive bitrate */
        s_pred_window[s_pred_idx] = 1;
        s_pred_idx = (s_pred_idx + 1) % PRED_WINDOW_SIZE;
        if (s_pred_count < PRED_WINDOW_SIZE) s_pred_count++;
        signal_strength_report_frame_drop();
#ifndef RETAIL_BUILD
        log_frame_receive_mask(missing);
#endif
        diag_log_write("FEC", "frame %u unrecoverable: %u received < %u needed (missing %u) — DROPPING [total_dropped=%u consec=%u]\n",
                       g_current_frame, g_received_count, g_data_packets, missing, g_fec_frames_dropped, g_consecutive_frame_drops);
        update_fec_status_cache();
        control_stream_send_fec_status(
            g_current_frame,
            g_fec_last_highest_seq,
            g_fec_last_next_contig_seq,
            g_fec_last_missing_before_highest,
            g_fec_last_data_pkts,
            g_fec_last_parity_pkts,
            g_fec_last_recv_data,
            g_fec_last_recv_parity,
            g_fec_last_fec_pct,
            0,
            1);
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
        note_current_frame_drop_for_recovery();
        if (g_last_good_frame == 0) {
            /* No reference at all — must get an IDR to start decoding */
            g_idr_fully_decoded = 0;
            control_stream_request_idr();
            s_consec_unrecoverable = 0;
        } else if (g_refs_corrupted) {
            /* Decode pipeline is broken — need IDR to reset DPB */
            if (s_consec_unrecoverable >= UNRECOVERABLE_IDR_THRESHOLD ||
                (int)missing >= (CATASTROPHIC_LOSS_THRESHOLD * 2)) {
                diag_log_write("FEC", "IDR escalation: %d consecutive unrecoverable (refs corrupt)\n",
                               s_consec_unrecoverable);
                g_idr_fully_decoded = 0;
                control_stream_request_idr();
                s_consec_unrecoverable = 0;
            } else if (rtp_reassembly_waiting_for_idr()) {
                control_stream_request_idr();
            } else {
                control_stream_request_rfi(g_last_good_frame + 1, g_current_frame);
            }
        } else if (g_consecutive_frame_drops >= CONSECUTIVE_DROP_IDR_LIMIT) {
            /* Phase 3.6: 120 consecutive drops — force IDR (moonlight-common-c match) */
            diag_log_write("FEC", "IDR forced: %u consecutive drops >= %d limit\n",
                           g_consecutive_frame_drops, CONSECUTIVE_DROP_IDR_LIMIT);
            g_idr_fully_decoded = 0;
            control_stream_request_idr();
            g_consecutive_frame_drops = 0;
            s_consec_unrecoverable = 0;
        } else {
            /* Refs clean: request targeted RFI, not a full IDR. Baseline
             * testing showed continuing through isolated drops can poison
             * OpenH264 refs and collapse into no-output recovery. */
            if (rtp_reassembly_waiting_for_idr()) {
                control_stream_request_idr();
            } else {
                request_current_frame_rfi();
            }
            if (s_consec_unrecoverable >= UNRECOVERABLE_IDR_THRESHOLD ||
                (int)missing >= (CATASTROPHIC_LOSS_THRESHOLD * 2)) {
                g_refs_corrupted = 1;
                g_idr_fully_decoded = 0;
                rtp_reassembly_note_frame_loss(g_last_good_frame + 1, g_current_frame);
                diag_log_write("FEC", "refs marked corrupt after %d unrecoverable drops (missing=%u)\n",
                               s_consec_unrecoverable, missing);
            }
        }
        g_frame_submitted = 1; /* prevent timeout re-processing */
        return;
    }

    /* Reset consecutive counter on successful FEC recovery */
    s_consec_unrecoverable = 0;
    g_consecutive_frame_drops = 0;  /* Phase 3.6: reset on successful recovery */

    /* With intraRefresh enabled, every P-frame self-heals: the refresh
     * cycle progressively corrects any corruption within ~10 frames.
     * So we allow RS recovery even with low surplus — a wrong recovery
     * in one P-frame only affects a few MBs and gets auto-corrected.
     *
     * Previously we required surplus >= missing (run #48: 214 drops,
     * 0 recoveries — too aggressive for small intra-refresh frames).
     * Now: attempt RS whenever we have enough total packets. */

    /* We have enough packets — attempt RS recovery.
     *
     * SELECTIVE FEC SKIP: when parity packet loss exceeds 50%, the
     * chance of RS producing a correct result is low and the CPU cost
     * (~2-8ms per attempt on 333MHz) is wasted.  Skip the attempt
     * and drop the frame — the decoder's error concealment will
     * repeat the previous frame (smooth) vs corrupt artifacts. */
    {
#ifndef RETAIL_BUILD
        u32 parity_count = g_total_packets - g_data_packets;
#endif
        u32 parity_received = 0;
        u32 idx;
        for (idx = g_data_packets; idx < g_total_packets; idx++) {
            if (g_slots[idx].received) parity_received++;
        }
        if (parity_received < missing) {
            /* >75% parity lost — RS recovery is truly unreliable.
             * Lowered from 50% threshold: at 802.11b loss rates, even
             * partial parity (25-50%) gives RS a fair chance, and the
             * error-concealed result is better than a dropped frame.
             * Actual gate: one received parity shard per missing data shard. */
            g_fec_packets_failed += missing;
            g_fec_recovery_attempts++;
#ifndef RETAIL_BUILD
            {
                static u32 s_fec_skip_count = 0;
                s_fec_skip_count++;
                if (s_fec_skip_count <= 3 || (s_fec_skip_count % 60) == 0)
                    diag_log_write("FEC", "skip RS: frame %u parity %u/%u received, missing=%u [skip#%u]\n",
                                   g_current_frame, parity_received, parity_count,
                                   missing, s_fec_skip_count);
            }
#endif
            drop_current_frame_strict("insufficient parity for RS", missing);
            return;
        }
    }

    if (g_psp_config.width <= 256 && g_psp_config.height <= 144 &&
        g_psp_config.fps <= 20 && g_received_count <= g_data_packets) {
        static u32 s_exact_rs_count = 0;
        s_exact_rs_count++;
        if (s_exact_rs_count <= 3 || (s_exact_rs_count % 120) == 0) {
            diag_log_write("FEC",
                           "attempting exact-threshold low-res RS frame %u missing=%u recv=%u data=%u [try#%u]\n",
                           g_current_frame, missing, g_received_count,
                           g_data_packets, s_exact_rs_count);
        }
    }

    /* THROTTLED: log only first 3 + every 120th to avoid filling the
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

    receiveSize = fec_recovery_symbol_size();

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
        reed_solomon_global_lock();
        if (s_cached_rs &&
            s_cached_data == g_data_packets &&
            s_cached_parity == g_parity_packets &&
            s_cached_rs->data_shards == (int)g_data_packets &&
            s_cached_rs->parity_shards == (int)g_parity_packets) {
            rs = s_cached_rs;  /* reuse */
        } else {
            if (s_cached_rs) {
                reed_solomon_release(s_cached_rs);
                s_cached_rs = NULL;
            }
            rs = reed_solomon_new(g_data_packets, g_parity_packets);
            if (!rs) {
                reed_solomon_global_unlock();
                diag_log_write("FEC", "reed_solomon_new failed (%u, %u) -- refs corrupted\n",
                               g_data_packets, g_parity_packets);
                {
                    g_refs_corrupted = 1;
                    g_current_frame_is_corrupt = 1;
                }
                drop_current_frame_strict("RS allocator unavailable", missing);
                return;
            }
            s_cached_rs = rs;
            s_cached_data = g_data_packets;
            s_cached_parity = g_parity_packets;
        }
    }

    ret = reed_solomon_reconstruct(rs, packets, marks, g_total_packets, receiveSize);
    reed_solomon_global_unlock();
    /* Do NOT release rs — keep cached for next frame */

    g_fec_recovery_attempts++;

    if (ret == 0) {
        int recovered_invalid = 0;
        {
            static u32 s_recovery_ok_count = 0;
            s_recovery_ok_count++;
            if (s_recovery_ok_count <= 3 || (s_recovery_ok_count % 120) == 0)
                diag_log_write("FEC", "RS completed for frame %u (%u packets reconstructed)\n",
                               g_current_frame, missing);
        }

        /* Copy recovered packets into slots using moonlight-common-c sizing.
         *
         * RS reconstruction must use packetSize + MAX_RTP_HEADER_SIZE as the
         * symbol length, then recovered data packets are requeued at
         * dataOffset + packetSize.  Using the largest packet seen in the
         * frame can make RS "succeed" against the wrong symbol length and
         * produce subtle H.264 corruption that OpenH264 conceals as smear.
         *
         * Previous code tried to trim trailing zeros, but H.264 bitstreams
         * CAN legitimately contain runs of zero bytes (zero-valued DCT
         * coefficients, exp-Golomb coded zeros, etc.).  Trimming removed
         * real data, shifting the bitstream and causing CAVLC desync.
         */
        for (i = 0; i < g_data_packets; i++) {
            if (marks[i] && packets[i]) {
                int recovered_len;
                memcpy(g_slots[i].data, packets[i], receiveSize);
                g_slots[i].received = 1;
                g_slots[i].len = receiveSize;
                fec_patch_recovered_packet_header(i, 0, 0);
                recovered_len = fec_recovered_data_len(g_slots[i].data,
                                                       receiveSize);
                g_slots[i].len = recovered_len;
                if (!fec_validate_recovered_data_packet(i, g_data_packets)) {
                    g_slots[i].received = 0;
                    g_slots[i].len = 0;
                    recovered_invalid = 1;
                    break;
                }

                /* Slot-level recovery log silenced for performance.
                 * The frame-level summary above is sufficient. */
            }
        }

        /* FEC recovery is mathematically exact — if RS succeeded, the
         * recovered data is correct.  Don't request preemptive IDR here;
         * if the decode subsequently fails, the error path will request it.
         */
        if (recovered_invalid) {
            g_fec_packets_failed += missing;
            g_fec_recovery_clean = 0;
            drop_current_frame_strict("recovered packet invalid", missing);
            return;
        }
        /*
         * Removing this prevents IDR flooding (was triggering 10+ requests
         * per 19s of streaming in Run #17). */
        g_fec_packets_recovered += missing;
        g_fec_recovery_clean = 1;  /* RS succeeded — data is bit-perfect */
        /* Phase 4: Record success in prediction window */
        s_pred_window[s_pred_idx] = 0;
        s_pred_idx = (s_pred_idx + 1) % PRED_WINDOW_SIZE;
        if (s_pred_count < PRED_WINDOW_SIZE) s_pred_count++;
        signal_strength_report_frame_ok();
    } else {
        g_fec_packets_failed += missing;
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
        drop_current_frame_strict("RS recovery failed", missing);
        return;
    }

    /* Free allocated recovery buffers removed: now using static pool */

    /* Submit all data packets (including recovered ones) */
    submit_frame_packets();
}

/* ── Main packet processing ──────────────────────────────────────── */

static int rtp_fec_add_packet_inner(const u8 *packet, int packet_len,
                                    int allow_defer)
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
    u8 fec_block_num, multi_fec_total, multi_fec_blocks;

    if (!g_initialized || packet_len < FIXED_RTP_HEADER_SIZE + NV_VIDEO_PKT_SIZE)
        return 0; /* too small, let reassembly handle it */

    /* Track total bytes for bandwidth estimation */
    g_fec_total_bytes_received += (u32)packet_len;
    g_fec_packets_received++;

    /* Parse RTP header */
    data_offset = FIXED_RTP_HEADER_SIZE;
    if (packet[0] & RTP_FLAG_EXTENSION) {
        if (packet_len < data_offset + 4)
            return 0;
        data_offset += 4;
    }

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

    /* NV_VIDEO_PACKET layout: byte 10 is multiFecFlags, byte 11 is
     * multiFecBlocks. Bits 4-5 are the current block, bits 6-7 are the
     * last block index. */
    multi_fec_blocks = nv[11];
    fec_block_num = (multi_fec_blocks >> 4) & 0x03;
    multi_fec_total = ((multi_fec_blocks >> 6) & 0x03) + 1;
    if (fec_block_num >= MAX_FEC_MULTI_BLOCKS) return 0; /* out of range */

    /* Protection 1: Ignore massive Frame ID jumps in either direction
     * (noise/stray peer traffic/corrupted headers). */
    if (g_current_frame != 0xFFFFFFFF) {
        s32 frame_delta = (s32)(frame_index - g_current_frame);
        if (frame_delta < 0) {
#ifndef RETAIL_BUILD
            log_late_frame_packet(frame_index,
                                  g_current_frame,
                                  frame_delta,
                                  seq,
                                  fec_index,
                                  data_pkts,
                                  parity_pkts,
                                  total_pkts);
#endif
            return 1; /* stale/reordered packet from an already processed frame */
        }
        if (frame_delta > 1000 || frame_delta < -1000)
            return 1; /* consume as junk */
    }

    /* Protection 2: Basic sanity on metadata */
    {
        u32 max_reasonable_data = fec_max_reasonable_data_packets();
        if (data_pkts > FEC_MAX_PACKETS ||
            data_pkts > max_reasonable_data ||
            total_pkts > FEC_MAX_PACKETS ||
            fec_index >= FEC_MAX_PACKETS) {
            return 1; /* drop corrupted FEC packet */
        }
    }

    /* Protection 3: Reject impossible index ranges for valid FEC frames. */
    if (data_pkts > 0 && fec_index >= total_pkts) {
        return 1; /* consume as junk */
    }

    {
        int payload_bytes = packet_len - (data_offset + NV_VIDEO_PKT_SIZE);
        if (payload_bytes > 0) {
            if (data_pkts > 0 && fec_index >= data_pkts) {
                telemetry_accum_video_fec((u32)payload_bytes);
            } else {
                telemetry_accum_video_data((u32)payload_bytes);
            }
        }
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
process_frame_transition:
    if (frame_index != g_current_frame) {
        if (allow_defer &&
            frame_index > g_current_frame &&
            current_frame_needs_cabac_defer()) {
            if (defer_cabac_newer_packet(packet, packet_len, frame_index)) {
                return 1;
            }

            if (g_current_frame != 0xFFFFFFFF && !g_frame_submitted && g_received_count > 0) {
                attempt_recovery_and_submit();
            }
            drain_deferred_cabac_packets();
            goto process_frame_transition;
        }

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
        reset_frame_receive_status();
        g_frame_submitted = 0;

        /* Initialize per-block FEC state */
        g_num_fec_blocks = (multi_fec_total > MAX_FEC_MULTI_BLOCKS) ?
            MAX_FEC_MULTI_BLOCKS : multi_fec_total;
        {
            u32 spb = (g_num_fec_blocks > 1) ?
                FEC_MAX_PACKETS / g_num_fec_blocks : FEC_MAX_PACKETS;
            int b;
            for (b = 0; b < MAX_FEC_MULTI_BLOCKS; b++) {
                g_fec_blocks[b].base_slot = (u32)b * spb;
                g_fec_blocks[b].max_slots = spb;
                g_fec_blocks[b].data_packets = 0;
                g_fec_blocks[b].parity_packets = 0;
                g_fec_blocks[b].total_packets = 0;
                g_fec_blocks[b].received_count = 0;
                g_fec_blocks[b].received_data_count = 0;
                g_fec_blocks[b].fec_percentage = 0;
                g_fec_blocks[b].seen = 0;
                g_fec_blocks[b].recovered = 0;
            }
            /* Single-block: pre-populate block 0 from frame globals */
            if (g_num_fec_blocks == 1) {
                g_fec_blocks[0].data_packets = data_pkts;
                g_fec_blocks[0].parity_packets = parity_pkts;
                g_fec_blocks[0].total_packets = total_pkts;
                g_fec_blocks[0].fec_percentage = fec_pct;
                g_fec_blocks[0].seen = 1;
            }
        }

        /* Diagnostic: log any frame with many data packets (likely IDR) */
        if (data_pkts > 10) {
            diag_log_write("FEC", "LARGE frame %u: data=%u parity=%u total=%u fecPct=%u\n",
                           frame_index, data_pkts, parity_pkts, total_pkts, fec_pct);
        }
    }

    /* Compute slot index */
    if (g_num_fec_blocks > 1) {
        /* Multi-block: use per-block fec_index with block base offset */
        fec_block_t *blk = &g_fec_blocks[fec_block_num];
        if (!blk->seen) {
            blk->data_packets = data_pkts;
            blk->parity_packets = parity_pkts;
            blk->total_packets = total_pkts;
            blk->fec_percentage = fec_pct;
            blk->received_count = 0;
            blk->received_data_count = 0;
            blk->seen = 1;
        }
        if (fec_index >= blk->max_slots)
            return 0;
        index = blk->base_slot + fec_index;
    } else {
        /* Single-block: existing sequence-based indexing */
        index = (u16)(seq - g_lowest_seq);
        if (index >= g_total_packets)
            return 0;
    }
    if (index >= FEC_MAX_PACKETS)
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
        mark_frame_packet_received(
            index,
            (g_num_fec_blocks > 1) ?
                (fec_index < g_fec_blocks[fec_block_num].data_packets) :
                (index < g_data_packets));
        g_received_count++;
        /* Track per-block received count for multi-FEC */
        if (g_num_fec_blocks > 1) {
            g_fec_blocks[fec_block_num].received_count++;
            if (fec_index < g_fec_blocks[fec_block_num].data_packets)
                g_fec_blocks[fec_block_num].received_data_count++;
        }
    }

    /* Check if we have enough packets to attempt recovery/submit */
    if (!g_frame_submitted) {
        if (g_num_fec_blocks <= 1) {
            /* Only submit early once the data side is complete. Counting
             * parity here can feed a frame before its EOF data packet has
             * arrived, which later appears as a partial frame/RFI churn. */
            if (g_received_data_count >= g_data_packets)
                attempt_recovery_and_submit();
        } else {
            /* Multi-block: all blocks must have complete data before an
             * early submit. Missing data is recovered on frame transition. */
            int all_ready = 1;
            int b;
            for (b = 0; b < (int)g_num_fec_blocks; b++) {
                fec_block_t *blk = &g_fec_blocks[b];
                if (!blk->seen ||
                    blk->received_data_count < blk->data_packets) {
                    all_ready = 0;
                    break;
                }
            }
            if (all_ready)
                attempt_recovery_and_submit();
        }
    }

    return 1; /* consumed by FEC layer */
}

int rtp_fec_add_packet(const u8 *packet, int packet_len)
{
    int consumed;

    if (!g_initialized)
        rtp_fec_init();

    drain_deferred_cabac_packets();
    consumed = rtp_fec_add_packet_inner(packet, packet_len, 1);
    drain_deferred_cabac_packets();

    return consumed;
}

/*============================================================================
 * Phase 4: Predictive Frame Loss
 *============================================================================*/

int rtp_fec_get_predicted_loss(void)
{
    int recent_drops = 0;
    int i, idx;

    if (s_pred_count < PRED_RECENT_SIZE)
        return 0;

    /* Count drops in the most recent PRED_RECENT_SIZE frames */
    for (i = 0; i < PRED_RECENT_SIZE; i++) {
        idx = (s_pred_idx - 1 - i + PRED_WINDOW_SIZE) % PRED_WINDOW_SIZE;
        if (s_pred_window[idx])
            recent_drops++;
    }

    /* If drops >= threshold, predict continued burst loss.
     * WiFi 802.11b bursts are 50-200ms = ~2-6 frames at 30fps.
     * Predicted loss = recent_drops (extrapolate burst). */
    if (recent_drops >= PRED_BURST_THRESH)
        return recent_drops;

    return 0;
}
