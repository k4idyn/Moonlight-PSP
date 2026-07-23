/*
 * audio_thread.c - Moonlight Audio Receiver and Playback Thread
 *
 * Receives Moonlight/Sunshine RTP PCM audio on UDP port 48000 and plays
 * it back through the PSP SRC path at 48000 Hz. The host/decode path stays
 * mono; only the final DMA chunk is duplicated to stereo for PSP SRC.
 *
 * Thread model:
 *   Recv Thread  → decrypt → Opus decode → PCM staging → AudioRingBuffer
 *   Play Thread  ← ring_pop ← sceAudioOutputBlocking (hardware-paced)
 *   Ping Thread  → SS_PING every 500 ms → Sunshine audio port
 *
 * The recv and play threads MUST be separate because sceAudioOutputBlocking
 * blocks ~10.67 ms per DMA chunk, and sceNetInetRecv blocks up to the
 * socket timeout.  Combining both in one thread starves one operation
 * whenever the other blocks.
 */

#include "audio_thread.h"

#include <string.h>
#include <stdio.h>

/* CABAC dialog gate — decoder thread sets, main thread clears */
extern volatile int g_cabac_dialog_active;

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspaudio.h>
#include <pspnet_inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "moonlight_ports.h"
#include "stream_crypto.h"
#include "opus_decode_psp.h"
#include "diag_log.h"
#include "settings_menu.h"
#include "config.h"
#include "control_stream.h"
#include "runtime_telemetry.h"
#include "rs.h"

#include <pspiofilemgr.h>

#include <pspdebug.h>
/* GU UI owns the framebuffer during normal runtime; avoid direct debug-screen writes. */
#define pspDebugScreenPrintf(...) ((void)0)

extern PspConfig g_psp_config;

/*--------------------------------------------------------------------------
 * RTP header (RFC 3550 fixed 12-byte header only)
 *--------------------------------------------------------------------------*/
typedef struct {
    u8  version_p_x_cc;  /* V=2, P, X, CC */
    u8  marker_pt;       /* M, PT          */
    u16 seq;
    u32 timestamp;
    u32 ssrc;
} __attribute__((packed)) RtpHeader;

#define RTP_HDR_SIZE  sizeof(RtpHeader)
#define RTP_PT_PCMS16 10   /* PCM 16-bit signed, network byte order */
#define RTP_PT_AUDIO_OPUS 97
#define RTP_PT_AUDIO_FEC  127

#define AUDIO_RTP_FEC_DATA_SHARDS   4
#define AUDIO_RTP_FEC_PARITY_SHARDS 2
#define AUDIO_RTP_FEC_TOTAL_SHARDS  (AUDIO_RTP_FEC_DATA_SHARDS + AUDIO_RTP_FEC_PARITY_SHARDS)
#define AUDIO_RTP_FEC_CACHE_BLOCKS  8
#define AUDIO_RTP_FEC_PAYLOAD_MAX   AUDIO_MAX_RTP_SIZE
#define AUDIO_PENDING_PACKETS       8
#define AUDIO_REORDER_WINDOW_US     4000
#define AUDIO_REORDER_MAX_DRAINS    8
#ifndef PSP_AUDIO_PACKET_DURATION_MS
#define PSP_AUDIO_PACKET_DURATION_MS 60
#endif
/* Experimental low-work PSP path. It removes RTP-level audio parity
 * reconstruction while preserving parity accounting, but the first hardware
 * run froze after the video socket went idle. Keep the production baseline
 * on the proven path until a replacement passes VQ. */
#define AUDIO_RTP_FEC_DECODE_ENABLED 1

#if AUDIO_RTP_FEC_DECODE_ENABLED
typedef struct {
    u8  fecShardIndex;
    u8  payloadType;
    u16 baseSequenceNumber;
    u32 baseTimestamp;
    u32 ssrc;
} __attribute__((packed)) AudioFecHeader;

typedef struct {
    int valid;
    u16 base_seq;
    u32 base_ts;
    u32 ssrc;
    u8  payload_type;
    int block_size;
    u8  marks[AUDIO_RTP_FEC_TOTAL_SHARDS];
    u8  data[AUDIO_RTP_FEC_DATA_SHARDS][AUDIO_RTP_FEC_PAYLOAD_MAX];
    u8  fec[AUDIO_RTP_FEC_PARITY_SHARDS][AUDIO_RTP_FEC_PAYLOAD_MAX];
    u32 last_use_us;
} AudioRtpFecBlock;

typedef struct {
    int len;
    u8 data[AUDIO_MAX_RTP_SIZE];
} AudioPendingPacket;
#endif

/*--------------------------------------------------------------------------
 * Module state
 *--------------------------------------------------------------------------*/
static AudioRingBuffer s_ring;
static AudioStats      s_stats;

/* Phase 3.5: Persistent audio stats counters for RTP stats API */
static volatile u32 s_audio_pkts_received = 0;
static volatile u32 s_audio_pkts_decoded  = 0;
static volatile u32 s_audio_plc_total     = 0;
static volatile u32 s_audio_fec_total     = 0;
static volatile u32 s_last_audio_data_packet_us = 0;

static SceUID  s_audio_tid  = -1;
static int     s_audio_chan = -1;         /* SRC channel active flag (0=reserved, -1=none) */
static int     s_udp_sock   = -1;

static volatile int s_running = 0;
static volatile int s_teardown_started = 0;

/* Silence frame played on underrun — keeps DMA engine from garbage output */
static int16_t s_silence[AUDIO_CHUNK_SAMPLES * AUDIO_OUTPUT_CHANNELS]
    __attribute__((aligned(64)));

static int s_udp_sock_rtcp = -1;
static unsigned short s_bound_audio_port = 0;

/* PCM staging accumulator: decouples Opus frame size (e.g. 240 samples for
 * 5 ms Moonlight packets) from the PSP DMA chunk size (AUDIO_CHUNK_SAMPLES).
 * Samples are appended here after each Opus decode and drained in
 * AUDIO_CHUNK_SAMPLES-sized chunks into the ring buffer. */
#define AUDIO_STAGE_CAPACITY  (AUDIO_MAX_FRAME_SAMPLES * 4)
static int16_t s_pcm_stage[AUDIO_STAGE_CAPACITY * AUDIO_CHANNELS]
    __attribute__((aligned(64)));
static int     s_pcm_stage_count = 0; /* per-channel samples currently staged */

/* Phase 4: Time-stretch state — stores last good audio for gap concealment */
static int16_t s_last_good_pcm[AUDIO_MAX_FRAME_SAMPLES * AUDIO_CHANNELS];
static int     s_last_good_samples = 0;
#define TSTRETCH_MAX_PLC 5  /* max consecutive PLC frames for time-stretch (100ms) */

/* Static 64-byte-aligned buffer for sceAudioOutputBlocking DMA transfer.
 * Must NOT be on the stack — PSP audio DMA requires aligned addresses. */
static int16_t s_play_buf[AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS]
    __attribute__((aligned(64)));
static int16_t s_src_out_buf[AUDIO_CHUNK_SAMPLES * AUDIO_OUTPUT_CHANNELS]
    __attribute__((aligned(64)));
static u8 s_audio_fec_recover_buf[AUDIO_RTP_FEC_PAYLOAD_MAX]
    __attribute__((aligned(64)));
static u32 s_audio_rtp_fec_recovered = 0;
static u32 s_audio_rtp_fec_failed = 0;
#if AUDIO_RTP_FEC_DECODE_ENABLED
static AudioRtpFecBlock s_audio_fec_blocks[AUDIO_RTP_FEC_CACHE_BLOCKS]
    __attribute__((aligned(64)));
static AudioPendingPacket s_audio_pending[AUDIO_PENDING_PACKETS]
    __attribute__((aligned(64)));
static int s_audio_pending_head = 0;
static int s_audio_pending_count = 0;
static u32 s_audio_reorder_drains = 0;
static u32 s_audio_reorder_queued = 0;
static u32 s_audio_reorder_drops = 0;
static reed_solomon *s_audio_rtp_fec_rs = NULL;
#endif


/* Audio playback thread (drains ring at hardware rate) */
static SceUID s_play_tid = -1;

/* Audio ping thread */
static SceUID s_ping_tid = -1;
static volatile int s_ping_running = 0;
static volatile u32 s_audio_ping_seq = 0;

static u32 next_audio_ping_seq(void)
{
    u32 next = s_audio_ping_seq + 1;
    s_audio_ping_seq = next;
    return next;
}

static void stop_audio_ping_thread(void)
{
    s_ping_running = 0;
    if (s_ping_tid >= 0) {
        SceUInt timeout_us = 2000000;
        sceKernelWaitThreadEnd(s_ping_tid, &timeout_us);
        sceKernelDeleteThread(s_ping_tid);
        s_ping_tid = -1;
    }
}

/* Audio stream params from RTSP SETUP (network_connect.c) */
extern int  g_audio_server_port;
extern char g_audio_ping_payload[17];
extern char g_video_server_ip[64];

/* avRiKeyId from launch/resume — needed for per-packet audio IV */
extern unsigned int g_av_ri_key_id;

/* Whether the server encrypts audio with AES-CBC (set in network_connect.c) */
extern int g_audio_encryption_enabled;

/* Local bind IP from network_connect.c (empty = INADDR_ANY for real hardware) */
extern char g_local_bind_ip[16];

/* SS_PING packet format (same as video) */
typedef struct {
    char     payload[16];
    uint32_t seq_be;
} AudioSsPing;

typedef char _audio_ss_ping_size_check[(sizeof(AudioSsPing) == 20) ? 1 : -1];

#include "diag_log.h"
#define audio_log(fmt, ...) diag_log_write("AUDIO", fmt, ##__VA_ARGS__)

/*--------------------------------------------------------------------------
 * Ring-buffer helpers (lock-free SPSC)
 *--------------------------------------------------------------------------*/
/* Phase 5.4: Dynamic ring depth — adjustable between 32 and AUDIO_RING_SLOTS
 * based on connection quality. Lower depth = lower latency in good conditions. */
static volatile u32 s_effective_ring_depth = AUDIO_RING_SLOTS;

/* Phase 5.7: Separate audio crypto failure counter (threshold 100, not 30) */
static volatile u32 s_audio_crypto_fail_count = 0;
static volatile int s_audio_playback_started = 0;
static volatile int s_audio_packet_duration_ms = PSP_AUDIO_PACKET_DURATION_MS;

/* SYNC-001: RTP timestamp-driven drift control state.
 * We track source timeline from RTP timestamps and occasionally apply
 * one-chunk hold/drop corrections in playback when drift exceeds bounds. */
#define AUDIO_DRIFT_TARGET_CHUNKS              8
#define AUDIO_DRIFT_CORRECTION_PERIOD_CHUNKS   128
#define AUDIO_DRIFT_THRESHOLD_SAMPLES          (AUDIO_CHUNK_SAMPLES * 5)
#define AUDIO_PLC_LOW_WATER_CHUNKS             3
#define AUDIO_TIME_PLC_THRESHOLD_US            80000
#define AUDIO_PREBUFFER_TARGET_CHUNKS          80
#define AUDIO_EMPTY_HOLD_MAX_CHUNKS            12
#define AUDIO_SOURCE_IDLE_US                   750000
#define AUDIO_TIME_PLC_MAX_CONSEC_FRAMES       6
#define AUDIO_RTP_DRIFT_CORRECTION             0
#define AUDIO_TIME_STRETCH_CONCEALMENT         0
static volatile u32 s_rtp_anchor_ts = 0;
static volatile int s_rtp_anchor_valid = 0;
static volatile u32 s_rtp_elapsed_samples = 0;

static int audio_socket_rcvbuf_target(void)
{
    int duration_ms = s_audio_packet_duration_ms;

    if (duration_ms >= 40) {
        return 48 * 1024;
    }
    if (duration_ms >= 20) {
        return 64 * 1024;
    }
    return 128 * 1024;
}

static volatile u32 s_played_samples_total = 0;
static volatile u32 s_drift_drop_events = 0;
static volatile u32 s_drift_hold_events = 0;
static SceUID s_audio_sync_sem = -1;

static int audio_sync_lock(void)
{
    if (s_audio_sync_sem < 0) {
        return -1;
    }
    if (sceKernelWaitSema(s_audio_sync_sem, 1, NULL) < 0) {
        return -1;
    }
    return 0;
}

static void audio_sync_unlock(void)
{
    if (s_audio_sync_sem >= 0) {
        sceKernelSignalSema(s_audio_sync_sem, 1);
    }
}

static void audio_sync_reset_state(void)
{
    s_rtp_anchor_ts = 0;
    s_rtp_anchor_valid = 0;
    s_rtp_elapsed_samples = 0;
    s_played_samples_total = 0;
    s_drift_drop_events = 0;
    s_drift_hold_events = 0;
}

#if AUDIO_RTP_FEC_DECODE_ENABLED
static u16 audio_fec_base_seq(u16 seq)
{
    return (u16)(seq & ~(AUDIO_RTP_FEC_DATA_SHARDS - 1));
}

static void audio_rtp_fec_reset(void)
{
    memset(s_audio_fec_blocks, 0, sizeof(s_audio_fec_blocks));
    memset(s_audio_pending, 0, sizeof(s_audio_pending));
    s_audio_rtp_fec_recovered = 0;
    s_audio_rtp_fec_failed = 0;
    s_audio_pending_head = 0;
    s_audio_pending_count = 0;
    s_audio_reorder_drains = 0;
    s_audio_reorder_queued = 0;
    s_audio_reorder_drops = 0;
}

static AudioRtpFecBlock *audio_rtp_fec_get_block(u16 base_seq,
                                                 u32 base_ts,
                                                 u32 ssrc,
                                                 u8 payload_type,
                                                 int block_size)
{
    int i;
    int pick = -1;
    u32 oldest = 0;

    if (block_size <= 0 || block_size > AUDIO_RTP_FEC_PAYLOAD_MAX) {
        return NULL;
    }

    for (i = 0; i < AUDIO_RTP_FEC_CACHE_BLOCKS; i++) {
        AudioRtpFecBlock *b = &s_audio_fec_blocks[i];
        if (b->valid && b->base_seq == base_seq) {
            if (b->block_size != block_size ||
                b->payload_type != payload_type ||
                b->ssrc != ssrc) {
                b->valid = 0;
                break;
            }
            b->last_use_us = sceKernelGetSystemTimeLow();
            return b;
        }
    }

    for (i = 0; i < AUDIO_RTP_FEC_CACHE_BLOCKS; i++) {
        AudioRtpFecBlock *b = &s_audio_fec_blocks[i];
        if (!b->valid) {
            pick = i;
            break;
        }
        if (pick < 0 || (u32)(b->last_use_us - oldest) > 0x80000000u) {
            pick = i;
            oldest = b->last_use_us;
        }
    }

    if (pick < 0) {
        return NULL;
    }

    {
        AudioRtpFecBlock *b = &s_audio_fec_blocks[pick];
        b->valid = 1;
        b->base_seq = base_seq;
        b->base_ts = base_ts;
        b->ssrc = ssrc;
        b->payload_type = payload_type;
        b->block_size = block_size;
        b->last_use_us = sceKernelGetSystemTimeLow();
        memset(b->marks, 1, sizeof(b->marks));
        return b;
    }
}

static void audio_rtp_fec_cache_data(const RtpHeader *hdr,
                                     const u8 *payload,
                                     int payload_len)
{
    u16 seq = ntohs(hdr->seq);
    u32 ts = ntohl(hdr->timestamp);
    u32 ssrc = ntohl(hdr->ssrc);
    u16 base = audio_fec_base_seq(seq);
    int pos = (int)(u16)(seq - base);
    int step_ms = s_audio_packet_duration_ms;
    int step_samples;
    u32 base_ts;
    AudioRtpFecBlock *b;

    if (pos < 0 || pos >= AUDIO_RTP_FEC_DATA_SHARDS) {
        return;
    }
    if (step_ms < 5 || step_ms > 60) {
        step_ms = 10;
    }
    step_samples = (AUDIO_SAMPLE_RATE * step_ms) / 1000;
    if (step_samples <= 0) {
        return;
    }
    base_ts = ts - (u32)(pos * step_samples);
    b = audio_rtp_fec_get_block(base, base_ts, ssrc, RTP_PT_AUDIO_OPUS,
                                payload_len);
    if (!b || !b->marks[pos]) {
        return;
    }
    memcpy(b->data[pos], payload, (size_t)payload_len);
    b->marks[pos] = 0;
}

static void audio_rtp_fec_cache_parity(const u8 *payload, int payload_len)
{
    const AudioFecHeader *fh;
    u16 base_seq;
    u32 base_ts;
    u32 ssrc;
    int shard;
    int fec_len;
    AudioRtpFecBlock *b;

    if (payload_len < (int)sizeof(AudioFecHeader)) {
        return;
    }

    fh = (const AudioFecHeader *)payload;
    shard = (int)fh->fecShardIndex;
    if (shard < 0 || shard >= AUDIO_RTP_FEC_PARITY_SHARDS ||
        fh->payloadType != RTP_PT_AUDIO_OPUS) {
        return;
    }

    base_seq = ntohs(fh->baseSequenceNumber);
    if ((base_seq & (AUDIO_RTP_FEC_DATA_SHARDS - 1)) != 0) {
        return;
    }
    base_ts = ntohl(fh->baseTimestamp);
    ssrc = ntohl(fh->ssrc);
    fec_len = payload_len - (int)sizeof(AudioFecHeader);

    b = audio_rtp_fec_get_block(base_seq, base_ts, ssrc,
                                fh->payloadType, fec_len);
    if (!b || !b->marks[AUDIO_RTP_FEC_DATA_SHARDS + shard]) {
        return;
    }

    memcpy(b->fec[shard], payload + sizeof(AudioFecHeader), (size_t)fec_len);
    b->marks[AUDIO_RTP_FEC_DATA_SHARDS + shard] = 0;
}

static int audio_rtp_fec_complete(AudioRtpFecBlock *b)
{
    unsigned char *shards[AUDIO_RTP_FEC_TOTAL_SHARDS];
    unsigned char marks[AUDIO_RTP_FEC_TOTAL_SHARDS];
    int i;
    int available = 0;
    int missing_data = 0;
    reed_solomon *rs;
    int ret;

    if (!b || !b->valid) {
        return -1;
    }

    for (i = 0; i < AUDIO_RTP_FEC_TOTAL_SHARDS; i++) {
        marks[i] = b->marks[i];
        if (!marks[i]) available++;
        if (i < AUDIO_RTP_FEC_DATA_SHARDS && marks[i]) missing_data++;
    }
    if (missing_data == 0) {
        return 0;
    }
    if (available < AUDIO_RTP_FEC_DATA_SHARDS) {
        return -1;
    }

    for (i = 0; i < AUDIO_RTP_FEC_DATA_SHARDS; i++) {
        shards[i] = b->data[i];
    }
    for (i = 0; i < AUDIO_RTP_FEC_PARITY_SHARDS; i++) {
        shards[AUDIO_RTP_FEC_DATA_SHARDS + i] = b->fec[i];
    }

    reed_solomon_global_lock();
    if (!s_audio_rtp_fec_rs ||
        s_audio_rtp_fec_rs->data_shards != AUDIO_RTP_FEC_DATA_SHARDS ||
        s_audio_rtp_fec_rs->parity_shards != AUDIO_RTP_FEC_PARITY_SHARDS ||
        !s_audio_rtp_fec_rs->m ||
        !s_audio_rtp_fec_rs->parity) {
        s_audio_rtp_fec_rs = reed_solomon_new(AUDIO_RTP_FEC_DATA_SHARDS,
                                              AUDIO_RTP_FEC_PARITY_SHARDS);
    }
    rs = s_audio_rtp_fec_rs;
    if (rs) {
        static const unsigned char audio_parity[] =
            { 0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c };
        memcpy(rs->parity, audio_parity, sizeof(audio_parity));
        ret = reed_solomon_reconstruct(rs, shards, marks,
                                       AUDIO_RTP_FEC_TOTAL_SHARDS,
                                       b->block_size);
    } else {
        ret = -1;
    }
    reed_solomon_global_unlock();

    if (ret != 0) {
        s_audio_rtp_fec_failed++;
        return -1;
    }

    for (i = 0; i < AUDIO_RTP_FEC_DATA_SHARDS; i++) {
        if (b->marks[i]) {
            b->marks[i] = 0;
            s_audio_rtp_fec_recovered++;
        }
    }
    return 0;
}

static int audio_rtp_fec_recover_payload(u16 seq, u8 *out_payload,
                                         int *out_len)
{
    u16 base = audio_fec_base_seq(seq);
    int pos = (int)(u16)(seq - base);
    int i;
    int saw_block = 0;

    if (!out_payload || !out_len ||
        pos < 0 || pos >= AUDIO_RTP_FEC_DATA_SHARDS) {
        return -1;
    }

    for (i = 0; i < AUDIO_RTP_FEC_CACHE_BLOCKS; i++) {
        AudioRtpFecBlock *b = &s_audio_fec_blocks[i];
        if (!b->valid || b->base_seq != base) {
            continue;
        }
        saw_block = 1;
        if (audio_rtp_fec_complete(b) != 0 || b->marks[pos]) {
            static u32 s_audio_rtp_fec_miss_log_count = 0;
            s_audio_rtp_fec_miss_log_count++;
            if (s_audio_rtp_fec_miss_log_count <= 8 ||
                (s_audio_rtp_fec_miss_log_count % 100) == 0) {
                int available = 0;
                int missing_data = 0;
                int mi;
                for (mi = 0; mi < AUDIO_RTP_FEC_TOTAL_SHARDS; mi++) {
                    if (!b->marks[mi]) {
                        available++;
                    } else if (mi < AUDIO_RTP_FEC_DATA_SHARDS) {
                        missing_data++;
                    }
                }
                audio_log("[AUDIO RTP-FEC] miss seq=%u base=%u pos=%d avail=%d missing_data=%d block=%d total=%u failed=%u\n",
                          (unsigned)seq, (unsigned)base, pos, available,
                          missing_data, b->block_size,
                          (unsigned)s_audio_rtp_fec_recovered,
                          (unsigned)s_audio_rtp_fec_failed);
            }
            return -1;
        }
        memcpy(out_payload, b->data[pos], (size_t)b->block_size);
        *out_len = b->block_size;
        return 0;
    }

    if (!saw_block) {
        static u32 s_audio_rtp_fec_noblock_log_count = 0;
        s_audio_rtp_fec_noblock_log_count++;
        if (s_audio_rtp_fec_noblock_log_count <= 8 ||
            (s_audio_rtp_fec_noblock_log_count % 100) == 0) {
            audio_log("[AUDIO RTP-FEC] no block for seq=%u base=%u pos=%d total=%u failed=%u\n",
                      (unsigned)seq, (unsigned)base, pos,
                      (unsigned)s_audio_rtp_fec_recovered,
                      (unsigned)s_audio_rtp_fec_failed);
        }
    }
    return -1;
}

static int audio_pending_push(const u8 *data, int len)
{
    int tail;

    if (!data || len <= 0 || len > AUDIO_MAX_RTP_SIZE) {
        return -1;
    }
    if (s_audio_pending_count >= AUDIO_PENDING_PACKETS) {
        s_audio_reorder_drops++;
        return -1;
    }

    tail = (s_audio_pending_head + s_audio_pending_count) % AUDIO_PENDING_PACKETS;
    memcpy(s_audio_pending[tail].data, data, (size_t)len);
    s_audio_pending[tail].len = len;
    s_audio_pending_count++;
    s_audio_reorder_queued++;
    return 0;
}

static int audio_pending_pop(u8 *out, int *out_len)
{
    int len;

    if (!out || !out_len || s_audio_pending_count <= 0) {
        return 0;
    }

    len = s_audio_pending[s_audio_pending_head].len;
    if (len <= 0 || len > AUDIO_MAX_RTP_SIZE) {
        s_audio_pending_head = (s_audio_pending_head + 1) % AUDIO_PENDING_PACKETS;
        s_audio_pending_count--;
        return 0;
    }

    memcpy(out, s_audio_pending[s_audio_pending_head].data, (size_t)len);
    *out_len = len;
    s_audio_pending[s_audio_pending_head].len = 0;
    s_audio_pending_head = (s_audio_pending_head + 1) % AUDIO_PENDING_PACKETS;
    s_audio_pending_count--;
    return 1;
}

static int audio_rtp_payload_offset(const u8 *packet, int *packet_len)
{
    const RtpHeader *hdr;
    int data_offset = (int)RTP_HDR_SIZE;
    u8 cc;

    if (!packet || !packet_len || *packet_len <= (int)RTP_HDR_SIZE) {
        return -1;
    }

    hdr = (const RtpHeader *)packet;
    if (((hdr->version_p_x_cc >> 6) & 0x03) != 2) {
        return -1;
    }

    cc = hdr->version_p_x_cc & 0x0F;
    data_offset += cc * 4;
    if (data_offset > *packet_len) {
        return -1;
    }

    if (hdr->version_p_x_cc & 0x10) {
        if (*packet_len >= data_offset + 4) {
            u16 ext_words = (u16)((packet[data_offset + 2] << 8) |
                                  packet[data_offset + 3]);
            data_offset += 4 + ext_words * 4;
        } else {
            return -1;
        }
    }
    if (data_offset >= *packet_len) {
        return -1;
    }

    if (hdr->version_p_x_cc & 0x20) {
        u8 pad_len = packet[*packet_len - 1];
        if (pad_len > 0 && (int)pad_len <= (*packet_len - data_offset)) {
            *packet_len -= pad_len;
        }
    }
    if (data_offset >= *packet_len) {
        return -1;
    }
    return data_offset;
}

int audio_thread_queue_external_rtp(const void *packet, int len)
{
    const u8 *pkt = (const u8 *)packet;
    int packet_len = len;
    int data_offset;
    const RtpHeader *hdr;
    u8 pt;
    int ret;
    static u32 s_external_audio_queued = 0;
    static u32 s_external_audio_dropped = 0;

    if (!pkt || len <= (int)RTP_HDR_SIZE || len > AUDIO_MAX_RTP_SIZE) {
        return -1;
    }

    data_offset = audio_rtp_payload_offset(pkt, &packet_len);
    if (data_offset < 0) {
        return -1;
    }

    hdr = (const RtpHeader *)pkt;
    pt = hdr->marker_pt & 0x7F;
    if (pt == RTP_PT_AUDIO_OPUS) {
        s_last_audio_data_packet_us = sceKernelGetSystemTimeLow();
        telemetry_accum_audio_rx((u32)len);
        telemetry_accum_audio_data((u32)packet_len);
        audio_rtp_fec_cache_data(hdr, pkt + data_offset,
                                 packet_len - data_offset);
    } else if (pt == RTP_PT_AUDIO_FEC) {
        telemetry_accum_audio_rx((u32)len);
        telemetry_accum_audio_fec((u32)packet_len);
        audio_rtp_fec_cache_parity(pkt + data_offset,
                                   packet_len - data_offset);
    } else {
        return -1;
    }

    ret = audio_pending_push(pkt, len);
    if (ret == 0) {
        s_external_audio_queued++;
        if (s_external_audio_queued <= 8 ||
            (s_external_audio_queued % 128) == 0) {
            audio_log("[AUDIO ROUTE] queued PT=%d len=%d from video socket [#%u]\n",
                      pt, len, (unsigned)s_external_audio_queued);
        }
    } else {
        s_external_audio_dropped++;
        if (s_external_audio_dropped <= 8 ||
            (s_external_audio_dropped % 128) == 0) {
            audio_log("[AUDIO ROUTE] queue full PT=%d len=%d drops=%u\n",
                      pt, len, (unsigned)s_external_audio_dropped);
        }
    }
    return ret;
}

static int audio_reorder_drain_for_fec(int *post_audio_fec_seen)
{
    u8 late_buf[AUDIO_MAX_RTP_SIZE];
    int drained = 0;
    u32 start_us = sceKernelGetSystemTimeLow();

    while (drained < AUDIO_REORDER_MAX_DRAINS) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int received;
        int packet_len;
        int data_offset;
        u8 pt;
        const RtpHeader *hdr;

        memset(&from_addr, 0, sizeof(from_addr));
        from_addr.sin_len = (uint8_t)sizeof(from_addr);
        received = sceNetInetRecvfrom(s_udp_sock, late_buf, sizeof(late_buf),
                                      MSG_DONTWAIT,
                                      (struct sockaddr *)&from_addr,
                                      &from_len);
        if (received <= 0) {
            u32 now_us = sceKernelGetSystemTimeLow();
            if ((u32)(now_us - start_us) >= AUDIO_REORDER_WINDOW_US) {
                break;
            }
            sceKernelDelayThread(500);
            continue;
        }

        s_audio_pkts_received++;
        telemetry_accum_audio_rx((u32)received);
        packet_len = received;
        data_offset = audio_rtp_payload_offset(late_buf, &packet_len);
        if (data_offset < 0) {
            continue;
        }

        hdr = (const RtpHeader *)late_buf;
        pt = hdr->marker_pt & 0x7F;
        if (pt == RTP_PT_AUDIO_FEC) {
            telemetry_accum_audio_fec((u32)received);
            audio_rtp_fec_cache_parity(late_buf + data_offset,
                                       packet_len - data_offset);
            if (post_audio_fec_seen) {
                (*post_audio_fec_seen)++;
            }
            drained++;
        } else if (pt == RTP_PT_AUDIO_OPUS) {
            s_last_audio_data_packet_us = sceKernelGetSystemTimeLow();
            telemetry_accum_audio_data((u32)received);
            audio_rtp_fec_cache_data(hdr, late_buf + data_offset,
                                     packet_len - data_offset);
            if (audio_pending_push(late_buf, received) != 0) {
                break;
            }
            drained++;
        } else {
            if (post_audio_fec_seen) {
                (*post_audio_fec_seen)++;
            }
            drained++;
        }
    }

    if (drained > 0) {
        s_audio_reorder_drains += (u32)drained;
        if (s_audio_reorder_drains <= 16 ||
            (s_audio_reorder_drains % 128) == 0) {
            audio_log("[AUDIO REORDER] drained=%d queued=%u drops=%u pending=%d\n",
                      drained, (unsigned)s_audio_reorder_queued,
                      (unsigned)s_audio_reorder_drops, s_audio_pending_count);
        }
    }
    return drained;
}
#else
static void audio_rtp_fec_reset(void)
{
}

static void audio_rtp_fec_cache_data(const RtpHeader *hdr,
                                     const u8 *payload,
                                     int payload_len)
{
    (void)hdr;
    (void)payload;
    (void)payload_len;
}

static void audio_rtp_fec_cache_parity(const u8 *payload, int payload_len)
{
    (void)payload;
    (void)payload_len;
}

static int audio_rtp_fec_recover_payload(u16 seq, u8 *out_payload,
                                         int *out_len)
{
    (void)seq;
    (void)out_payload;
    (void)out_len;
    return -1;
}

static int audio_pending_pop(u8 *out, int *out_len)
{
    (void)out;
    (void)out_len;
    return 0;
}

int audio_thread_queue_external_rtp(const void *packet, int len)
{
    (void)packet;
    (void)len;
    return -1;
}

static int audio_reorder_drain_for_fec(int *post_audio_fec_seen)
{
    if (post_audio_fec_seen) {
        *post_audio_fec_seen = 0;
    }
    return 0;
}
#endif

static int ring_full(void)
{
    return ((s_ring.head - s_ring.tail) >= s_effective_ring_depth);
}

static int ring_empty(void)
{
    return (s_ring.head == s_ring.tail);
}

/*
 * ring_push_pcm - copy exactly AUDIO_CHUNK_SAMPLES×2 int16 from src into
 * the next free slot and advance head.  Returns 0 on success, -1 if full.
 */
static int ring_push_pcm(const int16_t *src)
{
    if (ring_full()) {
        return -1;
    }
    u32 slot = s_ring.head % AUDIO_RING_SLOTS;
    memcpy(s_ring.pcm[slot], src,
           AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
    /* publish: ordered write — MIPS weak-memory model requires explicit
       sceKernelDcacheWritebackAll() before signalling, but the sceAudio
       DMA cache-coherency is handled by the audio hardware path.
       A simple compiler barrier suffices for the SPSC ring. */
    __asm__ volatile("" ::: "memory");
    s_ring.head++;
    return 0;
}

/* ring_pop_pcm - copy one slot into dst and advance tail.  Returns 0 or -1. */
static int ring_pop_pcm(int16_t *dst)
{
    if (ring_empty()) {
        return -1;
    }
    u32 slot = s_ring.tail % AUDIO_RING_SLOTS;
    memcpy(dst, s_ring.pcm[slot],
           AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
    __asm__ volatile("" ::: "memory");
    s_ring.tail++;
    return 0;
}

static int audio_flush_stage_to_ring(void)
{
    while (s_pcm_stage_count >= AUDIO_CHUNK_SAMPLES) {
        int remaining;
        if (ring_push_pcm(s_pcm_stage) < 0) {
            return -1;
        }
        remaining = s_pcm_stage_count - AUDIO_CHUNK_SAMPLES;
        if (remaining > 0) {
            memmove(s_pcm_stage,
                    s_pcm_stage + AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS,
                    remaining * AUDIO_CHANNELS * sizeof(int16_t));
        }
        s_pcm_stage_count = remaining;
    }
    return 0;
}

static int audio_stage_append_and_flush(const int16_t *src, int samples)
{
    int waits = 0;

    if (samples <= 0 || samples > AUDIO_STAGE_CAPACITY) {
        return -1;
    }

    while (s_pcm_stage_count + samples > AUDIO_STAGE_CAPACITY) {
        if (audio_flush_stage_to_ring() == 0 &&
            s_pcm_stage_count + samples <= AUDIO_STAGE_CAPACITY) {
            break;
        }
        if (++waits > 24) {
            return -2;
        }
        sceKernelDelayThread(1000);
    }

    memcpy(s_pcm_stage + s_pcm_stage_count * AUDIO_CHANNELS,
           src,
           samples * AUDIO_CHANNELS * sizeof(int16_t));
    s_pcm_stage_count += samples;

    return audio_flush_stage_to_ring() == 0 ? 0 : 1;
}

/*--------------------------------------------------------------------------
 * Phase 4: Simple audio time-stretch via linear interpolation.
 *
 * Stretches input samples by factor out_samples/in_samples (e.g. 1.3x).
 * Integer-only linear interpolation between adjacent samples.
 * Channels are interleaved and stretched independently.
 *--------------------------------------------------------------------------*/
static void audio_time_stretch(const int16_t *in, int in_samples,
                               int16_t *out, int out_samples)
{
    int i, ch;
    for (i = 0; i < out_samples; i++) {
        int pos_num = i * in_samples;
        int pos_int = pos_num / out_samples;
        int frac_num = pos_num - pos_int * out_samples;
        if (pos_int >= in_samples - 1) {
            for (ch = 0; ch < AUDIO_CHANNELS; ch++)
                out[i * AUDIO_CHANNELS + ch] = in[(in_samples - 1) * AUDIO_CHANNELS + ch];
        } else {
            for (ch = 0; ch < AUDIO_CHANNELS; ch++) {
                int a = in[pos_int * AUDIO_CHANNELS + ch];
                int b = in[(pos_int + 1) * AUDIO_CHANNELS + ch];
                int interp = a + ((b - a) * frac_num) / out_samples;
                if (interp > 32767) interp = 32767;
                if (interp < -32768) interp = -32768;
                out[i * AUDIO_CHANNELS + ch] = (int16_t)interp;
            }
        }
    }
}

void audio_thread_set_packet_duration_ms(int duration_ms)
{
    if (duration_ms < 5) {
        duration_ms = 5;
    } else if (duration_ms > 60) {
        duration_ms = 60;
    }
    s_audio_packet_duration_ms = duration_ms;
    audio_log("[AUDIO] packet duration set to %dms\n", duration_ms);
}

static void audio_copy_faded_hold(int16_t *dst, const int16_t *src, int hold_index)
{
    int i;
    int scale = 256 - (hold_index * 24);
    int total = AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS;

    if (scale < 128) {
        scale = 128;
    }

    for (i = 0; i < total; i++) {
        dst[i] = (int16_t)((src[i] * scale) >> 8);
    }
}

static void audio_src_output_chunk(const int16_t *pcm)
{
    int i;
    for (i = 0; i < AUDIO_CHUNK_SAMPLES; i++) {
        int16_t sample = pcm[i];
        s_src_out_buf[i * 2 + 0] = sample;
        s_src_out_buf[i * 2 + 1] = sample;
    }
    sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_src_out_buf);
}

/*--------------------------------------------------------------------------
 * Audio Playback Thread
 *
 * Dedicated thread that continuously drains the ring buffer at the
 * hardware sample rate via sceAudioOutputBlocking().  Plays silence
 * on underrun to keep the DMA engine running smoothly.
 *
 * This must be separate from the recv/decode thread because
 * sceAudioOutputBlocking blocks ~10.67 ms per chunk (512/48000 Hz),
 * and sceNetInetRecv blocks up to the socket timeout.  Combining both
 * in one thread causes recv stalls during playback and playback gaps
 * during recv waits — resulting in silence or choppy audio.
 *--------------------------------------------------------------------------*/
static int audio_play_thread_func(SceSize args, void *argp)
{
    int play_count = 0;
    int underrun_count = 0;
    int empty_hold_count = 0;
    int consecutive_empty_holds = 0;
    int source_idle_silence_count = 0;
    int16_t last_play_chunk[AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS]
        __attribute__((aligned(64)));
    int have_last_play_chunk = 0;
    u32 drift_tick = 0;
    int drift_drop_count = 0;
    int drift_hold_count = 0;

    audio_log("[AUDIO PLAY] started, SRC chan active\n");

    /* Pre-buffer: wait until the ring has a PSP-1000 WiFi jitter cushion
     * before starting playback.  This absorbs WiFi jitter bursts on
     * 802.11b and prevents the play thread from draining silence while
     * recv is still filling. */
    {
        int prebuf_wait = 0;
        const u32 prebuf_target = AUDIO_PREBUFFER_TARGET_CHUNKS;
        while (s_running && (s_ring.head - s_ring.tail) < prebuf_target) {
            sceKernelDelayThread(2000); /* 2ms check */
            prebuf_wait++;
        }
        audio_log("[AUDIO PLAY] pre-buffered %u slots after %d waits\n",
                  (unsigned)(s_ring.head - s_ring.tail), prebuf_wait);
        s_audio_playback_started = 1;
    }

    while (s_running) {
        int drift_hold = 0;
        int sync_valid = 0;
        u32 sync_elapsed_samples = 0;
        u32 sync_played_samples = 0;

        /* While CABAC dialog is active, play silence and drain any
         * buffered audio so the user hears nothing until they confirm. */
        if (g_cabac_dialog_active) {
            ring_pop_pcm(s_play_buf); /* drain ring to discard */
            sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_silence);
            continue;
        }

        if (audio_sync_lock() == 0) {
            sync_valid = s_rtp_anchor_valid;
            sync_elapsed_samples = s_rtp_elapsed_samples;
            sync_played_samples = s_played_samples_total;
            audio_sync_unlock();
        }

        if (sync_valid && AUDIO_RTP_DRIFT_CORRECTION) {
            drift_tick++;
            if ((drift_tick % AUDIO_DRIFT_CORRECTION_PERIOD_CHUNKS) == 0) {
                int queued_chunks = (int)(s_ring.head - s_ring.tail);
                int timeline_samples = (int)sync_elapsed_samples - (int)sync_played_samples;
                int target_samples = AUDIO_DRIFT_TARGET_CHUNKS * AUDIO_CHUNK_SAMPLES;
                int drift_error = timeline_samples - target_samples;

                if (drift_error > AUDIO_DRIFT_THRESHOLD_SAMPLES &&
                    queued_chunks > (AUDIO_DRIFT_TARGET_CHUNKS + 1)) {
                    if (ring_pop_pcm(s_play_buf) == 0) {
                        s_stats.frames_dropped++;
                        drift_drop_count++;
                        s_drift_drop_events = (u32)drift_drop_count;
                        if (drift_drop_count <= 5 || (drift_drop_count % 50) == 0) {
                            audio_log("[AUDIO SYNC] drift-drop #%d err=%d queued=%d\n",
                                      drift_drop_count, drift_error, queued_chunks);
                        }
                    }
                } else if (drift_error < -AUDIO_DRIFT_THRESHOLD_SAMPLES &&
                           have_last_play_chunk &&
                           queued_chunks <= AUDIO_PLC_LOW_WATER_CHUNKS) {
                    drift_hold = 1;
                    drift_hold_count++;
                    s_drift_hold_events = (u32)drift_hold_count;
                    if (drift_hold_count <= 5 || (drift_hold_count % 50) == 0) {
                        audio_log("[AUDIO SYNC] drift-hold #%d err=%d queued=%d\n",
                                  drift_hold_count, drift_error, queued_chunks);
                    }
                }
            }
        }

        if (drift_hold) {
            memcpy(s_play_buf, last_play_chunk, sizeof(last_play_chunk));
            audio_src_output_chunk(s_play_buf);
            s_stats.frames_played++;
            play_count++;
            if (audio_sync_lock() == 0) {
                s_played_samples_total += AUDIO_CHUNK_SAMPLES;
                audio_sync_unlock();
            }
            continue;
        }

        u32 last_data_us = s_last_audio_data_packet_us;
        u32 now_us = sceKernelGetSystemTimeLow();
        int source_idle = (last_data_us != 0 &&
                           (u32)(now_us - last_data_us) >= AUDIO_SOURCE_IDLE_US);

        if (ring_pop_pcm(s_play_buf) == 0) {
            audio_src_output_chunk(s_play_buf);
            memcpy(last_play_chunk, s_play_buf, sizeof(last_play_chunk));
            have_last_play_chunk = 1;
            s_stats.frames_played++;
            play_count++;
            consecutive_empty_holds = 0;
        } else if (!source_idle &&
                   have_last_play_chunk &&
                   consecutive_empty_holds < AUDIO_EMPTY_HOLD_MAX_CHUNKS) {
            audio_copy_faded_hold(s_play_buf, last_play_chunk,
                                  consecutive_empty_holds);
            audio_src_output_chunk(s_play_buf);
            s_stats.frames_played++;
            play_count++;
            empty_hold_count++;
            consecutive_empty_holds++;
            if (empty_hold_count <= 5 || (empty_hold_count % 100) == 0) {
                audio_log("[AUDIO PLAY] empty-hold #%d consec=%d queued=%u\n",
                          empty_hold_count, consecutive_empty_holds,
                          (unsigned)(s_ring.head - s_ring.tail));
            }
        } else if (source_idle) {
            sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_silence);
            source_idle_silence_count++;
            consecutive_empty_holds = 0;
            if (source_idle_silence_count <= 5 ||
                (source_idle_silence_count % 1000) == 0) {
                audio_log("[AUDIO PLAY] source-idle silence #%d queued=%u idle=%uus\n",
                          source_idle_silence_count,
                          (unsigned)(s_ring.head - s_ring.tail),
                          (unsigned)(now_us - last_data_us));
            }
        } else {
            /* Ring empty — play silence via DMA to maintain continuous audio
             * output.  This avoids clicks from DMA restarts and keeps the
             * playback cadence at a steady 10.67 ms per iteration. */
            sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_silence);
            s_stats.underruns++;
            underrun_count++;
            consecutive_empty_holds++;
        }

        if (audio_sync_lock() == 0) {
            s_played_samples_total += AUDIO_CHUNK_SAMPLES;
            audio_sync_unlock();
        }

        if (play_count > 0 && (play_count == 1 || play_count == 50 || play_count == 500 || (play_count % 2000) == 0)) {
            audio_log("[AUDIO PLAY] played=%d holds=%d underruns=%d\n",
                      play_count, empty_hold_count, underrun_count);
        } else if (play_count == 0 && (underrun_count == 1 || underrun_count == 200 || (underrun_count % 5000) == 0)) {
            audio_log("[AUDIO PLAY] played=%d holds=%d underruns=%d\n",
                      play_count, empty_hold_count, underrun_count);
        }
    }

    /* Final silence flush to avoid click */
    sceAudioSRCOutputBlocking(PSP_AUDIO_VOLUME_MAX, s_silence);

    sceKernelExitThread(0);
    return 0;
}

/*--------------------------------------------------------------------------
 * Audio Ping Thread
 *
 * Sends SS_PING to Sunshine's audio port every 500 ms so it knows
 * where to send audio RTP.  Must start BEFORE RTSP PLAY per GFE 3.22
 * compatibility (Sunshine also requires it).
 *--------------------------------------------------------------------------*/
static int audio_ping_thread_func(SceSize args, void *argp)
{
    struct sockaddr_in dst;
    AudioSsPing pkt;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (unsigned char)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_audio_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    audio_log("[AUDIO PING] started, target=%s:%d\n",
              g_video_server_ip, g_audio_server_port);

    while (s_ping_running) {
        int sent;
        uint32_t seq = next_audio_ping_seq();

        if (g_audio_ping_payload[0] != '\0') {
            memcpy(pkt.payload, g_audio_ping_payload, 16);
            pkt.seq_be = htonl(seq);
            sent = (int)sceNetInetSendto(s_udp_sock, &pkt, sizeof(pkt), 0,
                                         (struct sockaddr *)&dst, sizeof(dst));
        } else {
            sent = (int)sceNetInetSendto(s_udp_sock, legacy_ping, 4, 0,
                                         (struct sockaddr *)&dst, sizeof(dst));
        }

        if (seq <= 3 || (seq % 20) == 0)
            audio_log("[AUDIO PING] #%u sent=%d\n", seq, sent);

        /* Diagnose persistent audio sendto failures */
        if (sent < 0 && (seq <= 5 || (seq % 20) == 0)) {
#ifndef RETAIL_BUILD
            int err = sceNetInetGetErrno();
            audio_log("[AUDIO PING] sendto errno=%d sock=%d port=%d ip=%s seq=%u\n",
                      err, s_udp_sock, g_audio_server_port, g_video_server_ip, seq);
#endif
        }

        sceKernelDelayThread(500000); /* 500 ms */
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

static int start_audio_ping_thread(void)
{
    int ret;

    if (s_ping_running && s_ping_tid >= 0) {
        return 0;
    }

    if (s_udp_sock < 0) {
        if (audio_thread_reserve_client_port(NULL) < 0) {
            audio_log("[AUDIO PING] reserve socket failed before ping start\n");
            return -1;
        }
    }

    s_ping_running = 1;
    s_ping_tid = sceKernelCreateThread(
        "audio_ping", audio_ping_thread_func,
        0x1C, 0x2000, PSP_THREAD_ATTR_USER, NULL);
    if (s_ping_tid < 0) {
        audio_log("[AUDIO PING] thread create failed: %d\n", s_ping_tid);
        s_ping_running = 0;
        s_ping_tid = -1;
        return -1;
    }

    ret = sceKernelStartThread(s_ping_tid, 0, NULL);
    if (ret < 0) {
        audio_log("[AUDIO PING] thread start failed: %d\n", ret);
        sceKernelDeleteThread(s_ping_tid);
        s_ping_tid = -1;
        s_ping_running = 0;
        return -1;
    }

    return 0;
}

/*--------------------------------------------------------------------------
 * RTP receive helpers
 *--------------------------------------------------------------------------*/

/* audio_thread_reserve_client_port - bind consecutive UDP sockets for audio
 *
 * Uses ephemeral port (port 0) so the OS assigns a free port, avoiding any
 * fixed-port collision with Sunshine/Apollo on the same host.  RTCP is then
 * bound explicitly to data_port+1 to guarantee consecutive Data/RTCP ports. */

int audio_thread_reserve_client_port(unsigned short *out_port)
{
    if (s_udp_sock >= 0) {
        if (out_port) *out_port = s_bound_audio_port;
        return 0;
    }

    /* Bind data socket to ephemeral port (port 0), capture assigned port via
     * getsockname(), then bind RTCP to port+1 explicitly so that consecutive
     * Data/RTCP ports are guaranteed without any fixed-port collision. */
    int attempt;
    for (attempt = 0; attempt < 20; attempt++) {
        s_udp_sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        s_udp_sock_rtcp = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (s_udp_sock < 0 || s_udp_sock_rtcp < 0) {
            if (s_udp_sock >= 0) sceNetInetClose(s_udp_sock);
            if (s_udp_sock_rtcp >= 0) sceNetInetClose(s_udp_sock_rtcp);
            return -1;
        }

        int reuse = 1;
        sceNetInetSetsockopt(s_udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sceNetInetSetsockopt(s_udp_sock_rtcp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        /* Size the audio socket for the negotiated packet cadence. The 60 ms
         * PSP baseline has a low packet rate and should not reserve the same
         * kernel network buffer as video. */
        {
            int requested_rcvbuf = audio_socket_rcvbuf_target();
            int rcvbuf = requested_rcvbuf;
            if (sceNetInetSetsockopt(s_udp_sock, SOL_SOCKET, SO_RCVBUF,
                                     &rcvbuf, sizeof(rcvbuf)) < 0) {
                rcvbuf = 32 * 1024;
                if (sceNetInetSetsockopt(s_udp_sock, SOL_SOCKET, SO_RCVBUF,
                                         &rcvbuf, sizeof(rcvbuf)) < 0) {
                    rcvbuf = 16 * 1024;
                    sceNetInetSetsockopt(s_udp_sock, SOL_SOCKET, SO_RCVBUF,
                                         &rcvbuf, sizeof(rcvbuf));
                }
            }
            {
                int actual_rcvbuf = 0;
                socklen_t actual_len = sizeof(actual_rcvbuf);
                if (sceNetInetGetsockopt(s_udp_sock, SOL_SOCKET, SO_RCVBUF,
                                         &actual_rcvbuf, &actual_len) == 0) {
                    audio_log("[AUDIO] SO_RCVBUF target=%dKB actual=%dKB packetDuration=%dms\n",
                              requested_rcvbuf / 1024,
                              actual_rcvbuf / 1024,
                              s_audio_packet_duration_ms);
                }
            }
        }

        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_len         = (u8)sizeof(local);
        local.sin_family      = AF_INET;
        if (g_local_bind_ip[0] != '\0') {
            local.sin_addr.s_addr = inet_addr(g_local_bind_ip);
        } else {
            local.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        local.sin_port = 0; /* ephemeral: let the OS assign the port */

        if (sceNetInetBind(s_udp_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            sceNetInetClose(s_udp_sock);
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock = -1;
            s_udp_sock_rtcp = -1;
            return -1;
        }

        /* Capture the OS-assigned port via getsockname() */
        struct sockaddr_in name;
        socklen_t namelen = sizeof(name);
        memset(&name, 0, sizeof(name));
        name.sin_len = (u8)sizeof(name);
        if (sceNetInetGetsockname(s_udp_sock, (struct sockaddr *)&name, &namelen) != 0
                || name.sin_port == 0) {
            sceNetInetClose(s_udp_sock);
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock = -1;
            s_udp_sock_rtcp = -1;
            return -1;
        }
        unsigned short data_port = ntohs(name.sin_port);

        /* Avoid port 65535 since data_port + 1 would wrap to 0. */
        if (data_port == 65535) {
            sceNetInetClose(s_udp_sock);
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock = -1;
            s_udp_sock_rtcp = -1;
            continue;
        }

        /* Bind RTCP socket to the consecutive port (data_port + 1) */
        local.sin_port = htons(data_port + 1);
        if (sceNetInetBind(s_udp_sock_rtcp, (struct sockaddr *)&local, sizeof(local)) == 0) {
            s_bound_audio_port = data_port;
            if (out_port) *out_port = s_bound_audio_port;
            return 0;
        }

        /* RTCP port P+1 was in use — retry for a different ephemeral port. */
        sceNetInetClose(s_udp_sock);
        sceNetInetClose(s_udp_sock_rtcp);
        s_udp_sock = -1;
        s_udp_sock_rtcp = -1;
    }

    return -1;
}

int audio_thread_start_ping_only(void)
{
    return start_audio_ping_thread();
}

/*--------------------------------------------------------------------------
 * audio_thread_send_ping_burst - Rapid 3-ping burst after RTSP PLAY
 *
 * Mirrors network_me_send_video_ping_burst().  Sends from the pre-bound
 * audio socket (s_udp_sock) so Sunshine knows where to reply.
 *--------------------------------------------------------------------------*/
int audio_thread_send_ping_burst(void)
{
    struct sockaddr_in dst;
    AudioSsPing pkt;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    int i, sent_any = 0;

    if (s_udp_sock < 0) return -1;

    /* Log our source port so we can confirm it matches RTSP client_port */
    {
        struct sockaddr_in my_addr;
        socklen_t my_len = sizeof(my_addr);
        memset(&my_addr, 0, sizeof(my_addr));
        if (sceNetInetGetsockname(s_udp_sock, (struct sockaddr *)&my_addr, &my_len) == 0) {
            audio_log("[AUDIO BURST] src port=%u (Sunshine sends audio TO here)\n",
                      (unsigned)ntohs(my_addr.sin_port));
        }
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (unsigned char)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_audio_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    for (i = 1; i <= 3; i++) {
        int sent;
        if (g_audio_ping_payload[0] != '\0') {
            memcpy(pkt.payload, g_audio_ping_payload, 16);
            pkt.seq_be = htonl(next_audio_ping_seq());
            sent = (int)sceNetInetSendto(s_udp_sock, &pkt, sizeof(pkt), 0,
                                         (struct sockaddr *)&dst, sizeof(dst));
        } else {
            sent = (int)sceNetInetSendto(s_udp_sock, legacy_ping, 4, 0,
                                         (struct sockaddr *)&dst, sizeof(dst));
        }
        if (sent > 0) sent_any = 1;
        audio_log("[AUDIO BURST] ping #%d sent=%d to %s:%d\n",
                  i, sent, g_video_server_ip, g_audio_server_port);
        if (sent < 0) {
            audio_log("[AUDIO BURST] ping #%d sendto errno=%d\n",
                      i, sceNetInetGetErrno());
        }
        sceKernelDelayThread(30 * 1000); /* 30 ms between pings */
    }

    return sent_any ? 0 : -1;
}

/*--------------------------------------------------------------------------
 * Audio receive/decode thread
 *
 * Receives encrypted Opus RTP packets from Sunshine, decrypts with AES-CBC,
 * decodes Opus to PCM S16LE, and pushes AUDIO_CHUNK_SAMPLES frames into
 * the ring buffer.  The separate playback thread drains the ring.
 *--------------------------------------------------------------------------*/
static int audio_thread_func(SceSize args, void *argp)
{
    u8 pkt_buf[AUDIO_MAX_RTP_SIZE];
    int16_t pcm_decode_buf[AUDIO_MAX_FRAME_SAMPLES * AUDIO_CHANNELS];
    int recv_count = 0;
    int decode_ok_count = 0;
    int recv_err_count = 0;
    int plc_count = 0;
    int fec_recover_count = 0; /* in-band FEC recoveries */

    /* Audio data and audio FEC share one RTP sequence space. Use RTP
     * timestamps for gap recovery so missing parity packets cannot create
     * false audio PLC/FEC work. */
    unsigned short last_audio_seq = 0;
    int have_last_audio_seq = 0;
    u32 last_audio_rtp_ts = 0;
    int have_last_audio_rtp = 0;
    int last_audio_frame_samples = 0;
    int audio_gap_diag_count = 0;

    audio_log("[AUDIO] thread started, sock=%d src_active=%d port=%d\n",
              s_udp_sock, (s_audio_chan >= 0 ? 1 : 0), (int)s_bound_audio_port);

    /* --- Inline ping state (replaces dedicated ping thread) ---
     * Pings MUST originate from s_udp_sock (bound to client_port) so the
     * server sends audio data back to the correct port.  A separate ping
     * socket uses an ephemeral port, causing the server's audio replies
     * to go to the wrong destination.  Doing sends and recvs on the SAME
     * socket in the SAME thread avoids the PSP kernel 0x80411101 error
     * that occurs when two threads access one DGRAM socket concurrently. */
    struct sockaddr_in ping_dst;
    AudioSsPing ping_pkt;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    u32 last_ping_us = sceKernelGetSystemTimeLow();

    memset(&ping_dst, 0, sizeof(ping_dst));
    ping_dst.sin_len    = (unsigned char)sizeof(ping_dst);
    ping_dst.sin_family = AF_INET;
    ping_dst.sin_port   = htons((unsigned short)g_audio_server_port);
    ping_dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    /* Stop the dedicated RTSP-phase ping thread now that recv thread owns
     * the socket.  Send one immediate ping first so there's no gap. */
    stop_audio_ping_thread();
    {
        uint32_t ping_seq = next_audio_ping_seq();
        int ps;
        if (g_audio_ping_payload[0] != '\0') {
            memcpy(ping_pkt.payload, g_audio_ping_payload, 16);
            ping_pkt.seq_be = htonl(ping_seq);
            ps = (int)sceNetInetSendto(s_udp_sock, &ping_pkt, sizeof(ping_pkt),
                                       0, (struct sockaddr *)&ping_dst, sizeof(ping_dst));
        } else {
            ps = (int)sceNetInetSendto(s_udp_sock, legacy_ping, 4, 0,
                                       (struct sockaddr *)&ping_dst, sizeof(ping_dst));
        }
        audio_log("[AUDIO] recv thread took over pings, initial sent=%d\n", ps);
        if (ps < 0) {
            audio_log("[AUDIO] initial inline ping sendto errno=%d\n",
                      sceNetInetGetErrno());
        }
        /* Log source port to confirm pings come from RTSP-negotiated port */
        {
            struct sockaddr_in my_addr;
            socklen_t my_len = sizeof(my_addr);
            memset(&my_addr, 0, sizeof(my_addr));
            if (sceNetInetGetsockname(s_udp_sock, (struct sockaddr *)&my_addr, &my_len) == 0) {
                audio_log("[AUDIO] inline ping src port=%u -> %s:%d\n",
                          (unsigned)ntohs(my_addr.sin_port),
                          g_video_server_ip, g_audio_server_port);
            }
        }
        last_ping_us = sceKernelGetSystemTimeLow();
    }

    int loop_count = 0;
    struct sockaddr_in from_addr;
    socklen_t from_len;
    int got_first_packet = 0;  /* fast ping cadence until first audio data */
    int consecutive_empty = 0; /* adaptive backoff for empty polls */
    u32 last_audio_recv_us = 0; /* timestamp of last decoded audio packet */
    int time_plc_count = 0;    /* time-based PLC injections */
    int time_plc_logged = 0;
    int consec_plc = 0;        /* consecutive PLC frames (for volume ducking) */
    int fade_in_frames = 0;    /* post-PLC fade-in counter (0 = no fade) */
    int fec_pending = 0;       /* set when gap detected; FEC decode after decrypt */
    int fec_between_audio = 0; /* RTP FEC/parity slots seen since last audio data */

    while (s_running) {
        loop_count++;

        if (s_pcm_stage_count >= AUDIO_CHUNK_SAMPLES &&
            audio_flush_stage_to_ring() < 0) {
            sceKernelDelayThread(1000);
            continue;
        }

        /* While CABAC dialog is on-screen, pause audio processing.
         * Keep pinging so the server doesn't drop us, but don't decode
         * or play audio — the user hasn't confirmed the stream yet. */
        if (g_cabac_dialog_active) {
            sceKernelDelayThread(50 * 1000);
            continue;
        }

        /* --- Inline ping: 100 ms until first audio data, then steady keepalive. --- */
        {
            u32 now_us = sceKernelGetSystemTimeLow();
            u32 elapsed = now_us - last_ping_us;
            u32 ping_interval = got_first_packet
                ? ((s_audio_packet_duration_ms >= 40) ? 1000000 : 500000)
                : 100000;
            if (elapsed >= ping_interval) {
                uint32_t ping_seq = next_audio_ping_seq();
                int ps;
                if (g_audio_ping_payload[0] != '\0') {
                    memcpy(ping_pkt.payload, g_audio_ping_payload, 16);
                    ping_pkt.seq_be = htonl(ping_seq);
                    ps = (int)sceNetInetSendto(s_udp_sock, &ping_pkt, sizeof(ping_pkt),
                                               0, (struct sockaddr *)&ping_dst, sizeof(ping_dst));
                } else {
                    ps = (int)sceNetInetSendto(s_udp_sock, legacy_ping, 4, 0,
                                               (struct sockaddr *)&ping_dst, sizeof(ping_dst));
                }
                if (ping_seq <= 3 || (ping_seq % 20) == 0)
                    audio_log("[AUDIO PING-INLINE] #%u sent=%d\n", ping_seq, ps);
                if (ps < 0 && (ping_seq <= 5 || (ping_seq % 20) == 0)) {
#ifndef RETAIL_BUILD
                    int err = sceNetInetGetErrno();
                    audio_log("[AUDIO PING-INLINE] errno=%d sock=%d seq=%u\n",
                              err, s_udp_sock, ping_seq);
#endif
                }
                last_ping_us = now_us;
            }
        }

        /* Use non-blocking recvfrom() with MSG_DONTWAIT.  On PSP:
         *  - recv() on unconnected DGRAM sockets never returns data at all.
         *  - Blocking recvfrom() receives data BUT holds a socket lock that
         *    prevents concurrent sendto() on the same socket.
         *  - Non-blocking recvfrom(MSG_DONTWAIT) receives data without
         *    holding any lock, and since pings are now inline in this same
         *    thread, there is no concurrent socket access at all. */
        int received;
        int from_pending = audio_pending_pop(pkt_buf, &received);
        if (!from_pending) {
            from_len = sizeof(from_addr);
            memset(&from_addr, 0, sizeof(from_addr));
            from_addr.sin_len = (uint8_t)sizeof(from_addr);

            received = sceNetInetRecvfrom(s_udp_sock, pkt_buf, sizeof(pkt_buf),
                                          MSG_DONTWAIT,
                                          (struct sockaddr *)&from_addr,
                                          &from_len);
        }
        if (received <= 0) {
            recv_err_count++;
            if (!g_psp_config.audioEnabled) {
                if (recv_err_count == 1 || (recv_err_count % 2000) == 0) {
                    audio_log("[AUDIO] drain idle count=%d loop=%d\n",
                              recv_err_count, loop_count);
                }
            } else {
                int no_data_log_interval =
                    (got_first_packet && s_audio_packet_duration_ms >= 40) ? 2000 : 500;
                if (recv_err_count == 1 || recv_err_count == 50 ||
                    (recv_err_count % no_data_log_interval) == 0) {
#ifndef RETAIL_BUILD
                    int err = sceNetInetGetErrno();
                    audio_log("[AUDIO] recv no-data err=%d count=%d loop=%d\n",
                              err, recv_err_count, loop_count);
#endif
                }
            }

            /* --- Time-based PLC injection ---
             * Emergency low-water concealment only. RTP sequence recovery
             * handles real packet loss when packets resume; injecting PLC
             * while the ring is healthy creates drift and repeated audio.
             *
             * Adaptive threshold based on connection quality:
             *   EXCELLENT/GOOD: 35ms (tighter — detect loss faster)
             *   FAIR:           45ms (original — tolerate WiFi jitter)
             *   POOR/CRITICAL:  60ms (loose — WiFi is very jittery)
             *
             * Only inject if we've already received at least one audio frame
             * (have_last_audio_seq) and the ring isn't full. */
            /* Low-work 40/60 ms audio runs with continuousAudio=0; a quiet
             * host may intentionally send no audio, so avoid synthetic PLC. */
            if (s_audio_packet_duration_ms < 40 &&
                s_audio_playback_started &&
                have_last_audio_seq && last_audio_recv_us != 0 && !ring_full()) {
                u32 now_plc = sceKernelGetSystemTimeLow();
                u32 gap_us = now_plc - last_audio_recv_us;
                u32 queued_chunks = s_ring.head - s_ring.tail;

                /* Dynamic PLC threshold */
                u32 plc_threshold_us;
                {
                    ConnQualityState cq = control_stream_get_quality();
                    if (cq.quality <= CONN_QUALITY_GOOD)
                        plc_threshold_us = 35000;  /* 35ms */
                    else if (cq.quality == CONN_QUALITY_FAIR)
                        plc_threshold_us = 45000;  /* 45ms */
                    else
                        plc_threshold_us = 60000;  /* 60ms */
                    if (plc_threshold_us < AUDIO_TIME_PLC_THRESHOLD_US)
                        plc_threshold_us = AUDIO_TIME_PLC_THRESHOLD_US;
                    {
                        u32 packet_us = (u32)s_audio_packet_duration_ms * 1000u;
                        u32 jitter_floor_us = packet_us * 3u;
                        if (packet_us >= 5000u && packet_us <= 60000u &&
                            plc_threshold_us < jitter_floor_us) {
                            plc_threshold_us = jitter_floor_us;
                        }
                    }

                    /* Keep full depth on PSP-1000 WiFi. Shrinking the ring to
                     * 32/48 slots caused underruns during recoverable bursts. */
                    {
                        u32 new_depth = AUDIO_RING_SLOTS;
                        if (new_depth != s_effective_ring_depth) {
                            audio_log("[AUDIO RING] depth adjusted: %u slots (quality=%d)\n",
                                      (unsigned)new_depth, (int)cq.quality);
                            s_effective_ring_depth = new_depth;
                        }
                    }

                    /* Log threshold change */
                    {
                        static u32 s_prev_plc_threshold = 45000;
                        if (plc_threshold_us != s_prev_plc_threshold) {
                            audio_log("[AUDIO PLC] threshold %ums -> %ums (quality=%d)\n",
                                      s_prev_plc_threshold / 1000,
                                      plc_threshold_us / 1000,
                                      (int)cq.quality);
                            s_prev_plc_threshold = plc_threshold_us;
                        }
                    }
                }

                if (queued_chunks <= AUDIO_PLC_LOW_WATER_CHUNKS &&
                    consec_plc < AUDIO_TIME_PLC_MAX_CONSEC_FRAMES &&
                    gap_us >= plc_threshold_us) {
                    int plc_sz = opus_psp_last_frame_size();
                    int plc_samples;

                    /* Phase 4: Time-stretch for short gaps (<100ms).
                     * Stretches last good audio by 1.3x using linear
                     * interpolation. Sounds more natural than Opus PLC. */
                    if (AUDIO_TIME_STRETCH_CONCEALMENT &&
                        consec_plc < TSTRETCH_MAX_PLC &&
                        s_last_good_samples > 0) {
                        int stretch_out = (s_last_good_samples * 13) / 10;
                        if (stretch_out > AUDIO_MAX_FRAME_SAMPLES)
                            stretch_out = AUDIO_MAX_FRAME_SAMPLES;
                        audio_time_stretch(s_last_good_pcm, s_last_good_samples,
                                           pcm_decode_buf, stretch_out);
                        plc_samples = stretch_out;
                        {
                            static u32 s_tstretch_count = 0;
                            s_tstretch_count++;
                            if (s_tstretch_count <= 5 || (s_tstretch_count % 200) == 0) {
                                audio_log("[AUDIO TSTR] stretch: %d -> %d samples (consec=%d) [#%u]\n",
                                          s_last_good_samples, stretch_out, consec_plc, s_tstretch_count);
                            }
                        }
                    } else {
                        plc_samples = opus_psp_decode(NULL, 0,
                                                     pcm_decode_buf, plc_sz);
                    }
                    if (plc_samples > 0) {
                        time_plc_count++;
                        s_audio_plc_total++;
                        consec_plc++;

                        /* Volume ducking: fade PLC output to reduce static.
                         * PLC synthesis degrades after 2-3 consecutive frames.
                         * Ramp down: 1st→87%, 2nd→68%, 3rd+→50%.
                         * Use fixed-point multiply (shift by 15). */
                        {
                            int16_t scale = (consec_plc <= 1) ? 28672 :  /* 87.5% */
                                            (consec_plc <= 2) ? 22528 :  /* 68.75% */
                                                                16384;   /* 50% */
                            int i;
                            int total = plc_samples * AUDIO_CHANNELS;
                            for (i = 0; i < total; i++) {
                                pcm_decode_buf[i] = (int16_t)((pcm_decode_buf[i] * scale) >> 15);
                            }
                        }

                        (void)audio_stage_append_and_flush(pcm_decode_buf, plc_samples);
                        /* Advance the timestamp so we inject one PLC per
                         * frame interval, not one per EAGAIN poll. */
                        {
                            u32 frame_us = (u32)(((u64)plc_samples * 1000000ULL) /
                                                 AUDIO_SAMPLE_RATE);
                            if (frame_us < 5000 || frame_us > 70000) {
                                frame_us = 10000;
                            }
                            if (frame_us >= gap_us) {
                                last_audio_recv_us = now_plc;
                            } else {
                                last_audio_recv_us += frame_us;
                            }
                        }
                        if (time_plc_logged < 10 || (time_plc_count % 200) == 0) {
                            audio_log("[AUDIO] time-PLC #%d gap=%uus samples=%d queued=%u\n",
                                      time_plc_count, gap_us, plc_samples,
                                      (unsigned)queued_chunks);
                            time_plc_logged++;
                        }
                    }
                }
            }

            /* Adaptive backoff. Keep startup and short-packet presets quick,
             * but once 40-60ms audio packets are flowing, stop polling the
             * PSP network stack like a 20ms stream. */
            {
                int delay_us;
                consecutive_empty++;
                if (!g_psp_config.audioEnabled) {
                    delay_us = 5000;      /* drain-only mode: prioritize video */
                } else if (got_first_packet && s_audio_packet_duration_ms >= 40) {
                    if (consecutive_empty < 3) {
                        delay_us = 1000;
                    } else if (consecutive_empty < 10) {
                        delay_us = 2000;
                    } else {
                        delay_us = 4000;
                    }
                } else if (consecutive_empty < 5) {
                    delay_us = 500;       /* 500µs — data likely imminent */
                } else if (consecutive_empty < 20) {
                    delay_us = 1000;      /* 1ms — normal inter-packet gap */
                } else {
                    delay_us = 2000;      /* 2ms — no data flowing, save CPU */
                }
                sceKernelDelayThread(delay_us);
            }
            continue;
        }
        recv_count++;
        consecutive_empty = 0;  /* reset adaptive backoff on successful recv */
        if (!from_pending) {
            s_audio_pkts_received++;  /* Phase 3.5: audio stats */
            telemetry_accum_audio_rx((u32)received);
        }
        /* Start fade-in ramp when transitioning from PLC to real audio.
         * Ramp over 3 frames (60ms) to smooth the transition and
         * prevent audible click artifacts at the PLC→real boundary. */
        if (consec_plc > 0) {
            static u32 s_plc_recovery_log_count = 0;
            s_plc_recovery_log_count++;
            if (s_plc_recovery_log_count <= 10 ||
                (s_plc_recovery_log_count % 100) == 0) {
                audio_log("[AUDIO PLC] recovery after %d consecutive PLC frames [#%u]\n",
                          consec_plc, s_plc_recovery_log_count);
            }
            fade_in_frames = 3;
        }
        consec_plc = 0;         /* reset PLC volume ducking on real data */

        if (recv_count == 1) {
            audio_log("[AUDIO] FIRST packet! len=%d after %d empty polls\n",
                      received, recv_err_count);
            got_first_packet = 1;  /* switch to 500 ms ping cadence */
        }

        if (!g_psp_config.audioEnabled) {
            /* Audio disabled: just drop the payload after receiving it */
            continue;
        }

        if ((u32)received <= RTP_HDR_SIZE) {
            continue;
        }

        const RtpHeader *hdr = (const RtpHeader *)pkt_buf;
        u8 version = (hdr->version_p_x_cc >> 6) & 0x03;
        if (version != 2) {
            continue;
        }

        /* Calculate actual data offset: fixed header + CSRC list + extension.
         * Sunshine video RTP has X=1, audio may also have extension.
         * Missing this causes extension bytes to be fed to Opus → decode fail. */
        int data_offset = (int)RTP_HDR_SIZE;
        {
            u8 cc = hdr->version_p_x_cc & 0x0F;
            data_offset += cc * 4;
            if (hdr->version_p_x_cc & 0x10) { /* X bit = extension present */
                if (received >= data_offset + 4) {
                    u16 ext_words = (u16)((pkt_buf[data_offset + 2] << 8) |
                                           pkt_buf[data_offset + 3]);
                    data_offset += 4 + ext_words * 4;
                } else {
                    continue; /* truncated extension */
                }
            }
        }

        /* Handle RTP padding (P bit): RFC 3550 §5.1 — if set, the last
         * byte of the payload indicates the number of padding bytes
         * (including itself) that should be stripped before decoding. */
        if (hdr->version_p_x_cc & 0x20) { /* P bit set */
            if (received > data_offset) {
                u8 pad_len = pkt_buf[received - 1];
                if (pad_len > 0 && (int)pad_len <= (received - data_offset)) {
                    received -= pad_len;
                }
            }
        }

        /* Log RTP payload type for diagnostics (first 10 packets) */
#ifndef RETAIL_BUILD
        {
            static int pt_log_count = 0;
            u8 pt = hdr->marker_pt & 0x7F;
            if (pt_log_count < 10) {
                audio_log("[AUDIO] RTP PT=%d seq=%d len=%d dataOff=%d X=%d P=%d\n",
                          pt, ntohs(hdr->seq), received, data_offset,
                          (hdr->version_p_x_cc >> 4) & 1,
                          (hdr->version_p_x_cc >> 5) & 1);
                pt_log_count++;
            }
        }
#endif

        /* Skip FEC/parity packets (PT=127). Only decode audio data
         * packets (PT=97), while caching RTP-level audio FEC for later
         * gap repair. Count FEC packets between audio packets for accurate
         * gap detection because audio and FEC share the same sequence space. */
        {
            u8 pt = hdr->marker_pt & 0x7F;
            if (pt == RTP_PT_AUDIO_OPUS) {
                s_last_audio_data_packet_us = sceKernelGetSystemTimeLow();
                if (!from_pending) {
                    telemetry_accum_audio_data((u32)received);
                    audio_rtp_fec_cache_data(hdr, pkt_buf + data_offset,
                                             received - data_offset);
                }
            } else if (pt == RTP_PT_AUDIO_FEC) {
                if (!from_pending) {
                    telemetry_accum_audio_fec((u32)received);
                    audio_rtp_fec_cache_parity(pkt_buf + data_offset,
                                               received - data_offset);
                }
            }
            if (pt != RTP_PT_AUDIO_OPUS) {
                fec_between_audio++;
                continue;
            }

            {
                u32 rtp_ts = ntohl(hdr->timestamp);
                if (audio_sync_lock() == 0) {
                    if (!s_rtp_anchor_valid) {
                        s_rtp_anchor_ts = rtp_ts;
                        s_rtp_elapsed_samples = 0;
                        s_played_samples_total = 0;
                        s_rtp_anchor_valid = 1;
                        audio_log("[AUDIO SYNC] RTP timeline anchor ts=%u\n", (unsigned)rtp_ts);
                    } else {
                        s_rtp_elapsed_samples = (u32)(rtp_ts - s_rtp_anchor_ts);
                    }
                    audio_sync_unlock();
                }

                /* --- PLC/FEC gap recovery for lost audio frames ---
                 * Audio RTP sequence gaps include audio FEC parity packets.
                 * Sunshine's low-rate audio path may also advance timestamps
                 * across those parity slots, so received FEC packets must be
                 * subtracted before declaring media loss.
                 *
                 * Opus in-band FEC: the CURRENT packet embeds a low-bitrate
                 * copy of the PREVIOUS frame.  For the last lost frame in a
                 * gap, we use FEC recovery (better quality than PLC).  For
                 * earlier lost frames we use standard PLC (NULL decode).
                 */
                unsigned short seq = ntohs(hdr->seq);
                int post_audio_fec_seen = 0;
                if (have_last_audio_seq && have_last_audio_rtp) {
                int total_gap = (int)(unsigned short)(seq - last_audio_seq) - 1;
                int media_gap = total_gap - fec_between_audio;
                u32 ts_delta = (u32)(rtp_ts - last_audio_rtp_ts);
                int expected_ts = s_audio_packet_duration_ms;
                int expected_samples = last_audio_frame_samples;
                int audio_gap = 0;
                if (media_gap < 0) {
                    media_gap = 0;
                }
                if (expected_ts < 5 || expected_ts > 60) {
                    expected_ts = 10;
                }
                if (expected_samples < 120 || expected_samples > AUDIO_MAX_FRAME_SAMPLES) {
                    expected_samples = opus_psp_last_frame_size();
                }
                if (expected_samples < 120 || expected_samples > AUDIO_MAX_FRAME_SAMPLES) {
                    expected_samples = (AUDIO_SAMPLE_RATE * expected_ts) / 1000;
                }

                /* Sunshine may advance audio RTP timestamps across FEC parity
                 * slots too. Use timestamps only as an upper bound, then clamp
                 * to RTP sequence slots not already explained by received FEC
                 * packets so parity does not become fake PLC work. RTP audio
                 * timestamps are in the 48 kHz sample clock, not milliseconds. */
                if (expected_samples > 0 &&
                    ts_delta > (u32)(expected_samples + expected_samples / 2)) {
                    int frames_elapsed = (int)((ts_delta + (u32)(expected_samples / 2)) /
                                               (u32)expected_samples);
                    audio_gap = frames_elapsed - 1;
                }
                if (audio_gap > media_gap) {
                    audio_gap = media_gap;
                }
                if (!s_teardown_started && audio_gap > 0 && media_gap > 0) {
                    int recovered_prefix = 0;
                    int g;
                    u16 missing_seq_start = (u16)(seq - (u16)media_gap);

                    /* RTP-level audio FEC often arrives immediately after the
                     * data shard that exposes the gap. Drain a tiny bounded
                     * window before declaring loss so we recover from parity
                     * already in flight without adding steady-state latency. */
                    if (audio_gap <= AUDIO_RTP_FEC_PARITY_SHARDS) {
                        (void)audio_reorder_drain_for_fec(&post_audio_fec_seen);
                    }

                    for (g = 0; g < audio_gap && g < media_gap; g++) {
                        u16 missing_seq = (u16)(missing_seq_start + g);
                        int rec_len = 0;
                        int rec_opus_len;
                        int rec_samples;

                        if (audio_rtp_fec_recover_payload(missing_seq,
                                                          s_audio_fec_recover_buf,
                                                          &rec_len) != 0) {
                            break;
                        }

                        rec_opus_len = rec_len;
                        if (g_audio_encryption_enabled) {
                            if ((rec_opus_len & 0x0F) != 0 ||
                                stream_crypto_decrypt_audio(s_audio_fec_recover_buf,
                                                            rec_opus_len,
                                                            missing_seq,
                                                            g_av_ri_key_id) != 0) {
                                break;
                            }
                            if (rec_opus_len > 0) {
                                unsigned char pad_val =
                                    s_audio_fec_recover_buf[rec_opus_len - 1];
                                if (pad_val >= 1 && pad_val <= 16 &&
                                    pad_val <= rec_opus_len) {
                                    rec_opus_len -= (int)pad_val;
                                }
                            }
                        }
                        if (rec_opus_len <= 0) {
                            break;
                        }

                        rec_samples = opus_psp_decode(s_audio_fec_recover_buf,
                                                      rec_opus_len,
                                                      pcm_decode_buf,
                                                      AUDIO_MAX_FRAME_SAMPLES);
                        if (rec_samples <= 0) {
                            break;
                        }

                        memcpy(s_last_good_pcm, pcm_decode_buf,
                               rec_samples * AUDIO_CHANNELS *
                               (int)sizeof(int16_t));
                        s_last_good_samples = rec_samples;
                        last_audio_frame_samples = rec_samples;
                        if (audio_stage_append_and_flush(pcm_decode_buf,
                                                         rec_samples) < -1) {
                            s_stats.frames_dropped++;
                            break;
                        }
                        recovered_prefix++;
                        fec_recover_count++;
                        s_audio_fec_total++;
                        s_audio_pkts_decoded++;
                    }

                    if (recovered_prefix > 0) {
                        static u32 s_audio_rtp_fec_log_count = 0;
                        s_audio_rtp_fec_log_count++;
                        if (s_audio_rtp_fec_log_count <= 8 ||
                            (s_audio_rtp_fec_log_count % 100) == 0) {
                            audio_log("[AUDIO RTP-FEC] recovered=%d remaining_gap=%d seq=%u total=%u failed=%u\n",
                                      recovered_prefix,
                                      audio_gap - recovered_prefix,
                                      (unsigned)missing_seq_start,
                                      (unsigned)s_audio_rtp_fec_recovered,
                                      (unsigned)s_audio_rtp_fec_failed);
                        }
                        audio_gap -= recovered_prefix;
                        if (audio_gap < 0) {
                            audio_gap = 0;
                        }
                    }
                }
                if (!s_teardown_started &&
                    audio_gap > 0 &&
                    (audio_gap_diag_count < 8 || (audio_gap_diag_count % 100) == 0)) {
#ifndef RETAIL_BUILD
                    const char *gap_tag =
                        (audio_gap == 1) ? "[AUDIO FEC-PENDING]" : "[AUDIO GAP]";
                    audio_log("%s ts_delta=%u expected_ts=%dms expected_samples=%d gap=%d seq_gap=%d media_gap=%d fec_seen=%d\n",
                              gap_tag, ts_delta, expected_ts, expected_samples,
                              audio_gap, total_gap, media_gap, fec_between_audio);
#endif
                    audio_gap_diag_count++;
                }
                /* Limit to 10 consecutive frames (200 ms) — beyond that
                 * Opus PLC/FEC quality degrades to silence anyway. */
                if (!s_teardown_started && audio_gap > 0 && audio_gap <= 10) {
                    int g;
                    /* Frames 1..N-1: standard PLC */
                    for (g = 0; g < audio_gap - 1; g++) {
                        int plc_samples = opus_psp_decode(NULL, 0,
                                                          pcm_decode_buf,
                                                          AUDIO_MAX_FRAME_SAMPLES);
                        if (plc_samples > 0) {
                            plc_count++;
                            s_audio_plc_total++;
                            (void)audio_stage_append_and_flush(pcm_decode_buf, plc_samples);
                        }
                    }
                    /* Last lost frame (N): FEC recovery from current pkt.
                     * We haven't decrypted/deframed the current packet yet,
                     * so we need to do it inline.  However, FEC decode uses
                     * the raw Opus data.  We'll defer FEC to after decryption
                     * below, using a flag. */
                    fec_pending = 1;
                }
                }
                last_audio_seq = seq;
                have_last_audio_seq = 1;
                last_audio_rtp_ts = rtp_ts;
                have_last_audio_rtp = 1;
                fec_between_audio = post_audio_fec_seen;
                last_audio_recv_us = sceKernelGetSystemTimeLow();
            }
        }

        unsigned char *enc_data = pkt_buf + data_offset;
        int enc_bytes = received - data_offset;

        if (enc_bytes <= 0) {
            s_stats.frames_dropped++;
            continue;
        }

        unsigned short seq = ntohs(hdr->seq);
        int decoded_samples;
        int opus_len = enc_bytes;
        unsigned char *opus_data = enc_data;

        /* Diagnostic: log first Opus TOC byte and length */
        {
            static int toc_log = 0;
            if (toc_log < 3 && enc_bytes > 0) {
                audio_log("[AUDIO] pkt first_byte=0x%02X enc_bytes=%d "
                          "enc_enabled=%d seq=%u\n",
                          enc_data[0], enc_bytes,
                          g_audio_encryption_enabled, seq);
                toc_log++;
            }
        }

        if (g_audio_encryption_enabled) {
            /* AES-CBC requires a 16-byte-aligned payload */
            if ((enc_bytes & 0x0F) != 0) {
                s_stats.frames_dropped++;
                continue;
            }

            /* Diagnostic: log raw bytes BEFORE decrypt and g_av_ri_key_id */
            {
                static int raw_log = 0;
                if (raw_log < 3) {
                    char hx[64]; int hl = enc_bytes < 16 ? enc_bytes : 16;
                    int hi;
                    for (hi = 0; hi < hl; hi++)
                        snprintf(hx + hi * 3, 4, "%02X ", enc_data[hi]);
                    hx[hl * 3] = '\0';
                    audio_log("[AUDIO] RAW pre-decrypt len=%d seq=%u hex: %s\n",
                              enc_bytes, seq, hx);
                    if (raw_log == 0) {
                        audio_log("[AUDIO] DECRYPT using g_av_ri_key_id=%u (0x%08X) &g_av_ri_key_id=%p\n",
                                  g_av_ri_key_id, g_av_ri_key_id, (void *)&g_av_ri_key_id);
                    }
                    raw_log++;
                }
            }

            if (stream_crypto_decrypt_audio(enc_data, enc_bytes,
                                            seq, g_av_ri_key_id) != 0) {
                /* Phase 5.7: Track audio crypto failures separately */
                s_audio_crypto_fail_count++;
                if (s_audio_crypto_fail_count <= 5 ||
                    (s_audio_crypto_fail_count % 50) == 0) {
                    audio_log("[AUDIO CRYPTO] audio decrypt fail #%u (threshold=%u)\n",
                              (unsigned)s_audio_crypto_fail_count, 100u);
                }
                if (s_audio_crypto_fail_count >= 100) {
                    audio_log("[AUDIO CRYPTO] WARNING: 100 consecutive audio decrypt failures\n");
                    s_audio_crypto_fail_count = 0;
                }
                /* Decryption failed: inject PLC to fill the gap smoothly */
                int plc_size = opus_psp_last_frame_size();
                decoded_samples = opus_psp_decode(NULL, 0, pcm_decode_buf, plc_size);
                s_audio_plc_total++;
                s_stats.frames_dropped++;
                fec_pending = 0;  /* can't do FEC without valid data */
                goto check_decoded;
            }

            /* Strip PKCS#7 padding after AES-CBC decryption.
             * The server pads Opus packets to a 16-byte boundary before
             * encrypting.  After decryption the last byte = pad length
             * (1..16).  Without stripping, the extra bytes corrupt every
             * Opus frame. */
            /* Phase 5.7: Reset audio crypto fail counter on success */
            s_audio_crypto_fail_count = 0;
            {
                unsigned char pad_val = enc_data[enc_bytes - 1];
                /* Log first few PKCS#7 pad values for diagnostics */
                {
                    static int pkcs_log = 0;
                    if (pkcs_log < 5) {
                        audio_log("[AUDIO] PKCS#7 pad_val=%d enc_bytes=%d opus_len=%d\n",
                                  (int)pad_val, enc_bytes, enc_bytes - (int)pad_val);
                        pkcs_log++;
                    }
                }
                if (pad_val >= 1 && pad_val <= 16 && pad_val <= enc_bytes) {
                    opus_len = enc_bytes - (int)pad_val;
                }
            }
            opus_data = enc_data;
        }
        /* else: audio is NOT encrypted — enc_data is raw Opus payload */

        /* Audio encryption is determined by RTSP negotiation (audio_enc flag).
         * The g_audio_encryption_enabled flag is set in network_connect.c based
         * on SS_ENC_AUDIO (bit 1) in encryptionSupported.  If the server does
         * not advertise audio encryption, audio payloads are raw Opus. */

        if (opus_len <= 0) {
            s_stats.frames_dropped++;
            continue;
        }

        /* Diagnostic: hex dump first Opus payload (after possible decryption) */
        {
            static int hex_log_count = 0;
            if (hex_log_count < 5) {
                char hx[64]; int hl = opus_len < 16 ? opus_len : 16;
                int hi;
                for (hi = 0; hi < hl; hi++)
                    snprintf(hx + hi * 3, 4, "%02X ", opus_data[hi]);
                hx[hl * 3] = '\0';
                audio_log("[AUDIO] OPUS payload len=%d hex: %s\n", opus_len, hx);
                hex_log_count++;
            }
        }

        /* --- Opus in-band FEC recovery ---
         * If gap was detected (fec_pending), use the current packet's
         * embedded FEC data to recover the last lost frame before doing
         * the normal decode of this packet.  This produces better audio
         * than PLC because it uses actual encoded data from the encoder. */
        if (fec_pending && opus_len > 0) {
            int fec_frame_size = opus_psp_last_frame_size();
            int fec_samples = opus_psp_decode_fec(opus_data, opus_len,
                                                  pcm_decode_buf, fec_frame_size);
            if (fec_samples > 0) {
                fec_recover_count++;
                s_audio_fec_total++;
                (void)audio_stage_append_and_flush(pcm_decode_buf, fec_samples);
                if (fec_recover_count <= 5 || (fec_recover_count % 200) == 0) {
                    audio_log("[AUDIO] FEC recover #%d samples=%d\n",
                              fec_recover_count, fec_samples);
                }
            } else {
                int plc_samples = opus_psp_decode(NULL, 0,
                                                  pcm_decode_buf,
                                                  AUDIO_MAX_FRAME_SAMPLES);
                audio_log("[AUDIO] Unable to recover audio with FEC (ret=%d); PLC samples=%d\n",
                          fec_samples, plc_samples);
                if (plc_samples > 0) {
                    plc_count++;
                    s_audio_plc_total++;
                    (void)audio_stage_append_and_flush(pcm_decode_buf, plc_samples);
                }
            }
            fec_pending = 0;
        }

        /* Decode Opus to PCM */
        decoded_samples = opus_psp_decode(opus_data, opus_len,
                                          pcm_decode_buf, AUDIO_MAX_FRAME_SAMPLES);

        /* Phase 4: Save last good decode for time-stretch gap concealment */
        if (decoded_samples > 0 && decoded_samples <= AUDIO_MAX_FRAME_SAMPLES) {
            memcpy(s_last_good_pcm, pcm_decode_buf,
                   decoded_samples * AUDIO_CHANNELS * (int)sizeof(int16_t));
            s_last_good_samples = decoded_samples;
        }

        if (decoded_samples <= 0) {
            /* Log decode failures (first 5, then every 500th) */
            {
                static int dec_fail_count = 0;
                dec_fail_count++;
                if (dec_fail_count <= 5 || (dec_fail_count % 500) == 0) {
                    audio_log("[AUDIO] opus_decode FAIL err=%d len=%d recv=%d fails=%d\n",
                              decoded_samples, opus_len, recv_count, dec_fail_count);
                }
            }
            /* Decode failed: use PLC to conceal the gap */
            int plc_size = opus_psp_last_frame_size();
            decoded_samples = opus_psp_decode(NULL, 0, pcm_decode_buf, plc_size);
            s_audio_plc_total++;
            if (decoded_samples <= 0) {
                s_stats.frames_dropped++;
                continue;
            }
        }

check_decoded:
        if (decoded_samples <= 0) {
            s_stats.frames_dropped++;
            continue;
        }
        decode_ok_count++;
        s_audio_pkts_decoded++;
        if (decoded_samples > 0 && decoded_samples <= AUDIO_MAX_FRAME_SAMPLES) {
            last_audio_frame_samples = decoded_samples;
        }
        if (decode_ok_count == 1 || decode_ok_count == 10 || (decode_ok_count % 500) == 0) {
            audio_log("[AUDIO] decode ok=%d samples=%d recv=%d drop=%d enc=%d plc=%d tplc=%d fec=%d\n",
                      decode_ok_count, decoded_samples, recv_count,
                      (int)s_stats.frames_dropped, g_audio_encryption_enabled,
                      plc_count, time_plc_count, fec_recover_count);
        }

        /* ── Post-PLC fade-in ramp ──────────────────────────────────
         * After PLC concealment, gradually ramp volume back to 100%
         * over 3 frames to prevent audible click at the transition.
         * Frame 3→75%, Frame 2→87%, Frame 1→93%, then full volume. */
        if (fade_in_frames > 0) {
            int16_t scale;
            int total_samps, si;
            switch (fade_in_frames) {
                case 3:  scale = 24576; break; /* 75% */
                case 2:  scale = 28672; break; /* 87.5% */
                case 1:  scale = 30720; break; /* 93.75% */
                default: scale = 32767; break;
            }
            total_samps = decoded_samples * AUDIO_CHANNELS;
            for (si = 0; si < total_samps; si++) {
                pcm_decode_buf[si] = (int16_t)((pcm_decode_buf[si] * scale) >> 15);
            }
            fade_in_frames--;
        }

        /* Accumulate decoded samples into the staging buffer, then flush
         * complete AUDIO_CHUNK_SAMPLES chunks into the ring buffer.
         * This decouples the Opus frame size (e.g. 240 for 5 ms @ 48 kHz)
         * from the PSP DMA chunk size (AUDIO_CHUNK_SAMPLES = 512). */
        if (audio_stage_append_and_flush(pcm_decode_buf, decoded_samples) < -1) {
            s_stats.frames_dropped++;
        }
    }

    sceKernelExitThread(0);
    return 0;
}

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

int audio_thread_init(const char *host_ip)
{
    if (s_running) {
        return 0;   /* already started */
    }

    s_teardown_started = 0;
    memset(&s_ring,    0, sizeof(s_ring));
    memset(&s_stats,   0, sizeof(s_stats));
    memset(s_silence,  0, sizeof(s_silence));
    memset(s_pcm_stage, 0, sizeof(s_pcm_stage));
    s_pcm_stage_count = 0;
    s_audio_playback_started = 0;
    s_last_audio_data_packet_us = 0;
    audio_rtp_fec_reset();

    if (s_audio_sync_sem < 0) {
        s_audio_sync_sem = sceKernelCreateSema("audio_sync_sem", 0, 1, 1, NULL);
        if (s_audio_sync_sem < 0) {
            diag_log_write("AUD", "audio sync semaphore creation failed\n");
            return -1;
        }
    }

    if (audio_sync_lock() == 0) {
        audio_sync_reset_state();
        audio_sync_unlock();
    } else {
        audio_sync_reset_state();
    }

    if (g_psp_config.audioEnabled) {
        diag_log_write("AUD", "initializing opus...\n");
        if (opus_psp_init(AUDIO_SAMPLE_RATE,
                          AUDIO_STREAM_CHANNELS,
                          AUDIO_STREAMS,
                          AUDIO_STREAM_COUPLED_STREAMS) < 0) {
            diag_log_write("AUD", "Opus decoder init failed\n");
            return -1;
        }
    }

    /* The audio UDP sockets should already be pre-bound by network_connect.c.
     * If they aren't for some reason, try to bind them now. */
    if (s_udp_sock < 0) {
        if (audio_thread_reserve_client_port(NULL) < 0) {
            /* Ensure opus is shutdown if we had initialized it */
            if (g_psp_config.audioEnabled) opus_psp_shutdown();
            return -1;
        }
    }

    /* No SO_RCVTIMEO needed — we use MSG_DONTWAIT with manual delay.
     * Blocking recvfrom() with SO_RCVTIMEO holds a socket lock that blocks
     * the ping thread from calling sendto() on the same socket.  The video
     * thread can use blocking recvfrom() because its pings go on a separate
     * socket. */

    /* Start audio ping thread if RTSP didn't already bring it up.
     * Moonlight-common-c starts audio pings before PLAY, but we keep the
     * init path idempotent so later startup can safely reuse the thread. */
    if (start_audio_ping_thread() < 0) {
        audio_log("[AUDIO] warning: audio ping thread unavailable during init\n");
    }

    if (g_psp_config.audioEnabled) {
        /* Reserve SRC (Sample Rate Converter) audio channel for native
         * 48 kHz output.  sceAudioChReserve() only outputs at 44.1 kHz
         * which pitch-shifts 48 kHz Opus audio down ~8% ("deep voice"). */
        int src_ret = sceAudioSRCChReserve(AUDIO_CHUNK_SAMPLES,
                                            AUDIO_SAMPLE_RATE,
                                            AUDIO_OUTPUT_CHANNELS);
        if (src_ret < 0) {
            audio_log("[AUDIO] SRC reserve failed once (%d); releasing stale SRC and retrying\n",
                      src_ret);
            sceAudioSRCChRelease();
            sceKernelDelayThread(20 * 1000);
            src_ret = sceAudioSRCChReserve(AUDIO_CHUNK_SAMPLES,
                                           AUDIO_SAMPLE_RATE,
                                           AUDIO_OUTPUT_CHANNELS);
        }
        if (src_ret < 0) {
            audio_log("[AUDIO] sceAudioSRCChReserve failed after retry (%d)\n",
                      src_ret);
            opus_psp_shutdown();
            return -2;
        }
        s_audio_chan = 0;  /* Mark SRC channel as active */

        audio_log("[AUDIO] SRC channel reserved: freq=%d samples=%d streamCh=%d outputCh=%d\n",
                  AUDIO_SAMPLE_RATE, AUDIO_CHUNK_SAMPLES,
                  AUDIO_CHANNELS, AUDIO_OUTPUT_CHANNELS);
    }

    s_running = 1;

    /* --- Recv/decode thread (fills ring buffer) --- */
    {
        int recv_prio = g_psp_config.audioEnabled ? 0x1A : 0x21;
        int recv_stack = g_psp_config.audioEnabled ? (32 * 1024) : (16 * 1024);
        if (!g_psp_config.audioEnabled) {
            audio_log("[AUDIO] disabled: low-priority UDP drain/ping thread\n");
        }
        s_audio_tid = sceKernelCreateThread(
            "audio_recv",
            audio_thread_func,
            recv_prio,
            recv_stack,      /* Opus needs 64 KB; drain-only needs little stack */
            PSP_THREAD_ATTR_USER, NULL);
    }

    if (s_audio_tid < 0) {
        audio_log("[AUDIO] recv thread create failed: %d\n", s_audio_tid);
        s_running = 0;
        stop_audio_ping_thread();
        if (s_audio_chan >= 0) {
            sceAudioSRCChRelease();
            s_audio_chan = -1;
        }
        if (s_udp_sock >= 0) {
            sceNetInetClose(s_udp_sock);
            s_udp_sock = -1;
        }
        if (s_udp_sock_rtcp >= 0) {
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock_rtcp = -1;
        }
        s_bound_audio_port = 0;
        if (g_psp_config.audioEnabled) opus_psp_shutdown();
        return -3;
    }

    if (sceKernelStartThread(s_audio_tid, 0, NULL) < 0) {
        audio_log("[AUDIO] recv thread start failed\n");
        s_running = 0;
        stop_audio_ping_thread();
        sceKernelDeleteThread(s_audio_tid);
        s_audio_tid = -1;
        if (s_audio_chan >= 0) {
            sceAudioSRCChRelease();
            s_audio_chan = -1;
        }
        if (s_udp_sock >= 0) {
            sceNetInetClose(s_udp_sock);
            s_udp_sock = -1;
        }
        if (s_udp_sock_rtcp >= 0) {
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock_rtcp = -1;
        }
        s_bound_audio_port = 0;
        if (g_psp_config.audioEnabled) opus_psp_shutdown();
        return -3;
    }

    if (g_psp_config.audioEnabled) {
        /* --- Playback thread (drains ring via sceAudioOutputBlocking) --- */
        s_play_tid = sceKernelCreateThread(
            "audio_play",
            audio_play_thread_func,
            0x1A,
            8 * 1024,           /* 8 KB stack — sceAudioOutputBlocking DMA needs headroom */
            PSP_THREAD_ATTR_USER, NULL);

        if (s_play_tid < 0) {
            audio_log("[AUDIO] play thread create failed: %d\n", s_play_tid);
            /* Recv thread will still fill the ring even without playback */
        } else if (sceKernelStartThread(s_play_tid, 0, NULL) < 0) {
            audio_log("[AUDIO] play thread start failed\n");
            sceKernelDeleteThread(s_play_tid);
            s_play_tid = -1;
        }
    }

    return 0;
}

void audio_thread_shutdown(void)
{
    s_teardown_started = 1;
    s_running = 0;
    s_ping_running = 0;
    s_audio_playback_started = 0;

    /* Stop audio ping thread first */
    stop_audio_ping_thread();

    /* Unblock threads by closing sockets FIRST */
    if (s_udp_sock >= 0) {
        sceNetInetClose(s_udp_sock);
        s_udp_sock = -1;
    }
    if (s_udp_sock_rtcp >= 0) {
        sceNetInetClose(s_udp_sock_rtcp);
        s_udp_sock_rtcp = -1;
    }

    if (s_audio_tid >= 0) {
        SceUInt timeout_us = 1000000;   /* 1 s */
        sceKernelWaitThreadEnd(s_audio_tid, &timeout_us);
        sceKernelDeleteThread(s_audio_tid);
        s_audio_tid = -1;
    }

    /* Stop playback thread — it will exit after s_running=0 and one
     * final sceAudioOutputBlocking completes (~11 ms). */
    if (s_play_tid >= 0) {
        SceUInt timeout_us = 1000000;   /* 1 s */
        sceKernelWaitThreadEnd(s_play_tid, &timeout_us);
        sceKernelDeleteThread(s_play_tid);
        s_play_tid = -1;
    }

    if (s_audio_chan >= 0) {
        sceAudioSRCChRelease();
        s_audio_chan = -1;
    }

    s_bound_audio_port = 0;

    if (audio_sync_lock() == 0) {
        audio_sync_reset_state();
        audio_sync_unlock();
    } else {
        audio_sync_reset_state();
    }

    if (s_audio_sync_sem >= 0) {
        sceKernelDeleteSema(s_audio_sync_sem);
        s_audio_sync_sem = -1;
    }

    /* Shut down Opus decoder */
    if (g_psp_config.audioEnabled) opus_psp_shutdown();
}

void audio_thread_begin_shutdown(void)
{
    s_teardown_started = 1;
    s_running = 0;
    s_ping_running = 0;
}

int audio_thread_is_running(void)
{
    return s_running;
}

void audio_thread_get_stats(AudioStats *out)
{
    if (out) {
        *out = s_stats;
    }
}

/* Phase 3.5: RTP Audio Stats API */
void rtp_get_audio_stats(RtpAudioStats *out)
{
    if (!out) return;
    out->packets_received = s_audio_pkts_received;
    out->packets_decoded  = s_audio_pkts_decoded;
    out->plc_count        = s_audio_plc_total;
    out->fec_recovered    = s_audio_fec_total;
    out->underruns        = s_stats.underruns;
    out->frames_played    = s_stats.frames_played;
}
