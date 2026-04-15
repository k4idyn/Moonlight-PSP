/*
 * control_stream.c - Minimal ENet control channel for Sunshine
 *
 * Implements a raw-UDP ENet handshake to port 47999 (or whatever
 * server_port the control SETUP returned).  This is the MINIMUM needed
 * to make Sunshine start encoding and streaming video:
 *
 *   1.  ENet CONNECT  (with X-SS-Connect-Data as connectData)
 *   2.  Receive VERIFY_CONNECT, send ACK
 *   3.  Send START_A  (type 0x0305, payload 00 00)
 *   4.  Send START_B  (type 0x0307, payload 00)
 *   5.  Periodic ping (type 0x0200) every 100 ms in a background thread
 *
 * All multi-byte ENet fields are big-endian on the wire.
 */

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspnet_inet.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <pspiofilemgr.h>
#include <psprtc.h>

#define MBEDTLS_CONFIG_FILE "mbedtls_psp_config.h"
#include "mbedtls/gcm.h"

#include "control_stream.h"
#include "diag_log.h"
#include "shared.h"
#include "decode_flags.h"

/* ── Globals from network_connect.c ──────────────────────────────── */
extern char          g_video_server_ip[64];
extern int           g_control_server_port;
extern unsigned int  g_control_connect_data;
extern unsigned char g_remote_input_key[16];
extern int           g_remote_input_key_valid;

/* me_running flag from main.c */
extern volatile int me_running;

/* When intra-refresh is active, IDR frames are massive (65KB+) and can't
 * survive lossy 802.11b WiFi.  This flag suppresses IDR requests so
 * the server only sends small intra-refresh P-frames (~1KB each). */
volatile int g_intra_refresh_active = 0;

/* ── Connection Quality Monitoring state ─────────────────────────── */
static ConnQualityState s_conn_quality = { CONN_QUALITY_FAIR, 0, 0, 0, 0, 0 };
static u32 s_quality_prev_fec_recovered = 0;
static u32 s_quality_prev_fec_failed    = 0;
static u32 s_quality_prev_fec_dropped   = 0;
static u32 s_quality_prev_fec_attempts  = 0;
static u32 s_quality_prev_lgf           = 0;
static u32 s_quality_prev_time_us       = 0;

/* Phase 5.9: Quality-based BW report scaling (100=normal, 50=halve) */
static int s_quality_bw_scale_pct = 100;

/* Forward declaration (defined after control_stream_abort) */
static void update_connection_quality(u32 estimated_bw_bps);

#define ctrl_log(fmt, ...) diag_log_write("CTRL", fmt, ##__VA_ARGS__)

/* ── ENet protocol constants ─────────────────────────────────────── */
#define ENET_PEERID_NONE       0x0FFF
#define ENET_FLAG_COMPRESSED   0x4000
#define ENET_FLAG_SENT_TIME    0x8000
#define ENET_FLAG_MASK         (ENET_FLAG_COMPRESSED | ENET_FLAG_SENT_TIME)
#define ENET_SESSION_MASK      0x3000
#define ENET_CMD_ACK           1
#define ENET_CMD_CONNECT       2
#define ENET_CMD_VERIFY        3
#define ENET_CMD_DISCONNECT    4
#define ENET_CMD_PING          5
#define ENET_CMD_SEND_RELIABLE 6
#define ENET_CMD_SEND_UNRELIABLE    7
#define ENET_CMD_SEND_FRAGMENT      8
#define ENET_CMD_SEND_UNSEQUENCED   9
#define ENET_CMD_BANDWIDTH_LIMIT    10
#define ENET_CMD_THROTTLE_CONFIGURE 11
#define ENET_CMD_SEND_UNRELIABLE_FRAGMENT 12
#define ENET_CMD_FLAG_ACK      0x80

/* Sunshine Gen7Enc control packet types (little-endian on the wire).
 * Gen7Enc (version >= 7.1.431 with encrypted control) uses 0x0302 for
 * the combined Start-A / Request-IDR-Frame message.  Our earlier 0x0305
 * was the Gen7-unencrypted type which Sunshine silently ignores when
 * encrypted control is active, causing it to never start encoding. */
#define CTRL_TYPE_RFI_REQ      0x0301  /* Reference Frame Invalidation request (Gen7Enc) */
#define CTRL_TYPE_IDR_REQ      0x0302
#define CTRL_TYPE_START_A      0x0302
#define CTRL_TYPE_START_B      0x0307
#define CTRL_TYPE_PERIODIC     0x0200
#define CTRL_TYPE_INPUT        0x0206

#define CTRL_CHANNEL_GENERIC      0x00
#define CTRL_CHANNEL_URGENT       0x01
#define CTRL_CHANNEL_COUNT        0x30

/* ── Packet builders ─────────────────────────────────────────────── */

/* Write big-endian u16 at p, return p+2 */
static unsigned char *put_be16(unsigned char *p, unsigned short v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)(v);
    return p + 2;
}

/* Write big-endian u32 at p, return p+4 */
static unsigned char *put_be32(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >>  8);
    p[3] = (unsigned char)(v);
    return p + 4;
}

/* Read big-endian u16 */
static unsigned short get_be16(const unsigned char *p)
{
    return (unsigned short)((p[0] << 8) | p[1]);
}

static unsigned short get_le16(const unsigned char *p)
{
    return (unsigned short)(p[0] | ((unsigned short)p[1] << 8));
}

static unsigned int get_le32(const unsigned char *p)
{
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

/* ── State ───────────────────────────────────────────────────────── */
static int           ctrl_socket = -1;
static SceUID        ctrl_thread_id = -1;
static SceUID        ctrl_recv_thread_id = -1;
static SceUID        ctrl_seq_sem_id = -1;
static SceUID        ctrl_gcm_sem_id = -1;
static volatile int  ctrl_running = 0;

/* Peer state learned from VERIFY_CONNECT */
static unsigned short server_peer_id = 0;
static unsigned char  session_bits   = 0;   /* 2-bit session in bits 12-13 */
/* ENet tracks reliable sequence numbers independently for each channel.
 * A per-channel array prevents cross-channel seq pollution (e.g. pings
 * on ch0x00 advancing the counter that input on ch0x10 also uses). */
static unsigned short reliable_seq_per_ch[CTRL_CHANNEL_COUNT]; /* zeroed at init, starts at 1 */
static unsigned int   next_ctrl_enc_seq = 0; /* sequence for control encryption */
static int            ctrl_crypto_ready = 0;
static mbedtls_gcm_context ctrl_gcm_ctx;

/* connectID we generate — must match VERIFY_CONNECT echo */
static unsigned int  my_connect_id = 0;

/* Frame counter incremented by network_me.c on each successfully received frame */
volatile unsigned int g_last_good_frame = 0;

/* Set when Sunshine sends DISCONNECT — ping thread uses this for graceful drain */
static volatile int g_received_disconnect = 0;

/* Heartbeat timestamp updated by ctrl_ping_thread every iteration (B-4) */
volatile u32 g_ctrl_ping_heartbeat_us = 0;

/* Address of control server to send IDR requests */
static struct sockaddr_in g_server_addr;

/* ── Retransmission ring buffer ──────────────────────────────────
 * ENet requires strict sequential delivery of reliable commands per channel.
 * On lossy 802.11b WiFi, a single lost reliable packet permanently blocks
 * the server's dispatch queue for that channel — subsequent packets are
 * ACKed (proving receipt) but never dispatched (no RECEIVE event fires).
 * After 10 seconds with no RECEIVE events, Sunshine's pingTimeout expires
 * and it kills the session.
 *
 * This ring buffer stores recently sent reliable packets so we can
 * retransmit any that the server hasn't ACKed within 300ms. */
#define RETX_SLOTS      32
#define RETX_PKT_MAX    128   /* max packet size for retransmit storage */
#define RETX_TIMEOUT_US 300000  /* 300ms before first retransmit */
#define RETX_MAX_TRIES  5      /* max retransmissions per packet */

typedef struct {
    unsigned char  data[RETX_PKT_MAX];
    int            len;
    unsigned char  channel;
    unsigned short seq;
    u32            send_time_us;  /* sceKernelGetSystemTimeLow */
    unsigned char  retries;
    volatile unsigned char acked; /* set by recv thread */
    unsigned char  active;        /* 1 = slot in use */
} retx_entry_t;

static retx_entry_t retx_ring[RETX_SLOTS];
static int retx_head = 0;  /* next slot to write */

/* ── RTT-adaptive retransmit timeout (Jacobson/Karels) ──────────
 * Replaces the fixed 300ms timeout with one that adapts to actual
 * network conditions.  On LAN, RTO drops to ~50ms for faster gap
 * recovery.  On lossy WiFi with jitter, it stretches appropriately.
 * This closes the "adaptive timing" gap vs. ENet's built-in RTT. */
static u32 s_srtt_us = 0;           /* smoothed RTT (µs) */
static u32 s_rttvar_us = 150000;    /* RTT variance (µs), init 150ms */
static u32 s_rto_us = 300000;       /* computed retransmit timeout (µs) */
static int s_rtt_initialized = 0;

#define RTO_MIN_US    50000    /* 50ms floor — below is unrealistic for PSP WiFi */
#define RTO_MAX_US   1000000   /* 1s ceiling */

/* Update smoothed RTT from a sample.  Jacobson/Karels algorithm.
 * Only called with non-retransmitted samples (Karn's algorithm). */
static void rtt_update(u32 rtt_sample_us)
{
    if (!s_rtt_initialized) {
        s_srtt_us = rtt_sample_us;
        s_rttvar_us = rtt_sample_us / 2;
        s_rtt_initialized = 1;
    } else {
        /* RTTVAR = (3/4)*RTTVAR + (1/4)*|RTT - SRTT| */
        int delta = (int)rtt_sample_us - (int)s_srtt_us;
        if (delta < 0) delta = -delta;
        s_rttvar_us = (3 * s_rttvar_us + (u32)delta) / 4;
        /* SRTT = (7/8)*SRTT + (1/8)*RTT */
        s_srtt_us = (7 * s_srtt_us + rtt_sample_us) / 8;
    }
    s_rto_us = s_srtt_us + 4 * s_rttvar_us;
    if (s_rto_us < RTO_MIN_US) s_rto_us = RTO_MIN_US;
    if (s_rto_us > RTO_MAX_US) s_rto_us = RTO_MAX_US;
}

/* ── Bandwidth estimation ──────────────────────────────────────────
 * Track received video bytes and report throughput to the server via
 * ENet BANDWIDTH_LIMIT.  This closes the "no bandwidth estimation"
 * gap vs. ENet's built-in bandwidth tracking. */
static u32 s_bw_last_bytes = 0;
static u32 s_bw_last_time_us = 0;
static u32 s_estimated_bw_bps = 0;  /* estimated incoming bandwidth (bytes/sec) */

/* Server-provided throttle and bandwidth parameters */
static u32 s_server_incoming_bw = 0;
static u32 s_server_outgoing_bw = 0;
static u32 s_throttle_interval = 5000;
static u32 s_throttle_accel = 2;
static u32 s_throttle_decel = 2;

/* Store a sent reliable packet for retransmission.
 * Called from the ping thread (and recv thread for 0x010E echo). */
static void retx_store(const unsigned char *pkt, int pkt_len,
                        unsigned char channel, unsigned short seq)
{
    retx_entry_t *e;
    if (pkt_len <= 0 || pkt_len > RETX_PKT_MAX) return;
    e = &retx_ring[retx_head];
    memcpy(e->data, pkt, (size_t)pkt_len);
    e->len = pkt_len;
    e->channel = channel;
    e->seq = seq;
    e->send_time_us = sceKernelGetSystemTimeLow();
    e->retries = 0;
    e->acked = 0;
    e->active = 1;
    retx_head = (retx_head + 1) % RETX_SLOTS;
}

/* Mark a reliable packet as ACKed. Called from the recv thread. */
static void retx_ack(unsigned char channel, unsigned short seq)
{
    int i;
    u32 now = sceKernelGetSystemTimeLow();
    for (i = 0; i < RETX_SLOTS; i++) {
        retx_entry_t *e = &retx_ring[i];
        if (e->active && !e->acked && e->channel == channel && e->seq == seq) {
            e->acked = 1;
            /* Karn's algorithm: only measure RTT from non-retransmitted
             * packets — ACK for a retransmit is ambiguous (could be for
             * the original or the retransmit). */
            if (e->retries == 0) {
                u32 rtt = now - e->send_time_us;
                if (rtt > 0 && rtt < 5000000) /* sanity: < 5s */
                    rtt_update(rtt);
            }
            return;
        }
    }
}

/* Retransmit un-ACKed reliable packets. Called from the ping thread.
 * Returns the number of packets retransmitted. */
static int retx_scan(int sock, const struct sockaddr_in *dst)
{
    int i, retransmitted = 0;
    u32 now = sceKernelGetSystemTimeLow();

    for (i = 0; i < RETX_SLOTS; i++) {
        retx_entry_t *e = &retx_ring[i];
        if (!e->active || e->acked) continue;

        /* Check if enough time has passed since last send (RTT-adaptive) */
        if ((now - e->send_time_us) < s_rto_us) continue;

        if (e->retries >= RETX_MAX_TRIES) {
            /* Give up — mark inactive to free the slot */
            e->active = 0;
            continue;
        }

        /* Retransmit */
        sceNetInetSendto(sock, e->data, e->len, 0,
                         (const struct sockaddr *)dst, sizeof(*dst));
        e->send_time_us = now;
        e->retries++;
        retransmitted++;
    }
    return retransmitted;
}

/* Clear all retransmit slots. Called during init/stop. */
static void retx_clear(void)
{
    int i;
    for (i = 0; i < RETX_SLOTS; i++) {
        retx_ring[i].active = 0;
        retx_ring[i].acked = 0;
    }
    retx_head = 0;
    /* Reset RTT estimator for new session */
    s_srtt_us = 0;
    s_rttvar_us = 150000;
    s_rto_us = 300000;
    s_rtt_initialized = 0;
    /* Reset bandwidth estimator */
    s_bw_last_bytes = 0;
    s_bw_last_time_us = 0;
    s_estimated_bw_bps = 0;
}

/* Per-channel reliable sequence allocation.
 * ENet tracks reliable sequence numbers independently for each channel.
 * Using a single global counter caused Sunshine to reject messages on
 * channel 1 (URGENT) because their seq was far ahead of expected. */
static int alloc_reliable_seq_ch(unsigned short *out_seq, unsigned char channel)
{
    int wait_ret;
    SceUInt timeout = 500000; /* 500ms — generous for memory-only operation */

    if (!out_seq || ctrl_seq_sem_id < 0) {
        return -1;
    }
    if (channel >= CTRL_CHANNEL_COUNT) {
        return -1;
    }

    wait_ret = sceKernelWaitSema(ctrl_seq_sem_id, 1, &timeout);
    if (wait_ret < 0) {
        ctrl_log("[CTRL] seq alloc timeout/error: 0x%08X ch=%u\n", wait_ret, channel);
        return -1;
    }

    *out_seq = reliable_seq_per_ch[channel]++;
    sceKernelSignalSema(ctrl_seq_sem_id, 1);
    return 0;
}

static int alloc_ctrl_enc_seq(unsigned int *out_seq)
{
    int wait_ret;
    SceUInt timeout = 500000; /* 500ms */

    if (!out_seq || ctrl_seq_sem_id < 0) {
        return -1;
    }

    wait_ret = sceKernelWaitSema(ctrl_seq_sem_id, 1, &timeout);
    if (wait_ret < 0) {
        return -1;
    }

    *out_seq = next_ctrl_enc_seq++;
    sceKernelSignalSema(ctrl_seq_sem_id, 1);
    return 0;
}

static int lock_gcm_sem(void)
{
    if (ctrl_gcm_sem_id < 0) {
        return -1;
    }
    SceUInt timeout = 200000; /* 200ms — GCM ops take <1ms */
    return sceKernelWaitSema(ctrl_gcm_sem_id, 1, &timeout);
}

static void unlock_gcm_sem(void)
{
    if (ctrl_gcm_sem_id >= 0) {
        sceKernelSignalSema(ctrl_gcm_sem_id, 1);
    }
}

/* ── Build ENet CONNECT packet ───────────────────────────────────── */
static int build_connect_packet(unsigned char *buf, int buflen)
{
    unsigned char *p = buf;
    u64 tick;

    if (buflen < 52) return -1;  /* 4 header + 48 command */

    sceRtcGetCurrentTick(&tick);
    my_connect_id = (unsigned int)(tick & 0xFFFFFFFFu);

    /* Protocol header: peerID=0x8FFF (no peer + SENT_TIME), sentTime=0 */
    p = put_be16(p, ENET_PEERID_NONE | ENET_FLAG_SENT_TIME);
    p = put_be16(p, 0);  /* sentTime */

    /* CONNECT command (48 bytes) */
    *p++ = ENET_CMD_CONNECT | ENET_CMD_FLAG_ACK;  /* 0x82 */
    *p++ = 0xFF;                                   /* channelID */
    p = put_be16(p, 1);                            /* reliableSequenceNumber */
    p = put_be16(p, 0);                            /* outgoingPeerID = 0 */
    *p++ = 0xFF;                                   /* incomingSessionID */
    *p++ = 0xFF;                                   /* outgoingSessionID */
    p = put_be32(p, 1392);                         /* mtu */
    p = put_be32(p, 32768);                        /* windowSize */
    p = put_be32(p, CTRL_CHANNEL_COUNT);           /* channelCount = 0x30 */
    p = put_be32(p, 0);                            /* incomingBandwidth */
    p = put_be32(p, 0);                            /* outgoingBandwidth */
    p = put_be32(p, 5000);                         /* packetThrottleInterval */
    p = put_be32(p, 2);                            /* packetThrottleAcceleration */
    p = put_be32(p, 2);                            /* packetThrottleDeceleration */
    p = put_be32(p, my_connect_id);                /* connectID */
    p = put_be32(p, g_control_connect_data);       /* data = X-SS-Connect-Data */

    return (int)(p - buf);  /* should be 52 */
}

/* ── Build ACK for a received reliable command ───────────────────── */
static int build_ack_packet(unsigned char *buf, int buflen,
                            unsigned short peer_id_word,
                            unsigned char channel_id,
                            unsigned short rel_seq,
                            unsigned short recv_time)
{
    unsigned char *p = buf;
    (void)peer_id_word;
    if (buflen < 12) return -1; /* 4 header + 8 ACK command */

    /* Protocol header: PeerID + SessionID + SENT_TIME flag.
     * Reverting +1 logic: Sunshine expects the raw session_bits from VERIFY. */
    p = put_be16(p, (server_peer_id & 0x0FFF) |
                     ((unsigned short)(session_bits & 0x03) << 12) |
                     ENET_FLAG_SENT_TIME);
    p = put_be16(p, 0);

    /* ACK command header (4 bytes: cmd, channel, sequence) */
    *p++ = ENET_CMD_ACK;   /* 1 */
    *p++ = channel_id;
    p = put_be16(p, 0);    /* ACK itself has sequence 0 */

    /* ACK payload (4 bytes: receivedReliableSeq, receivedSentTime) */
    p = put_be16(p, rel_seq);
    p = put_be16(p, recv_time);

    return (int)(p - buf); /* 12 bytes total */
}

/* ── Build a reliable message on a specified ENet channel ─────────── */
static int build_reliable_raw(unsigned char *buf, int buflen,
                              const unsigned char *raw_payload, int raw_len,
                              unsigned char channel)
{
    unsigned char *p = buf;
    unsigned short rel_seq;
    /* 4 (packet header) + 6 (SEND_RELIABLE cmd) + payloadData */
    if (buflen < 10 + 4 + raw_len) return -1;
    if (alloc_reliable_seq_ch(&rel_seq, channel) < 0) return -1;

    /* ENet Protocol Header (4 bytes: PeerID + SessionBits + Flags, plus SentTime) */
    p = put_be16(p, (server_peer_id & 0x0FFF) |
                     ((unsigned short)(session_bits & 0x03) << 12) |
                     ENET_FLAG_SENT_TIME);
    p = put_be16(p, 0); /* sentTime = 0 */

    /* SEND_RELIABLE command (6 bytes) */
    *p++ = ENET_CMD_SEND_RELIABLE | ENET_CMD_FLAG_ACK; /* 0x86 */
    *p++ = channel;
    p = put_be16(p, rel_seq);
    p = put_be16(p, (unsigned short)raw_len);

    if (raw_len > 0) {
        memcpy(p, raw_payload, raw_len);
        p += raw_len;
    }

    return (int)(p - buf);
}

/* Extract channel and reliable sequence from a built reliable packet.
 * The packet layout is: [2-byte header][2-byte sentTime][cmd][channel][2-byte seq]... */
static void pkt_get_channel_seq(const unsigned char *pkt, int pkt_len,
                                unsigned char *out_ch, unsigned short *out_seq)
{
    if (pkt_len >= 8) {
        *out_ch  = pkt[5]; /* channel byte in SEND_RELIABLE command */
        *out_seq = (unsigned short)((pkt[6] << 8) | pkt[7]);
    } else {
        *out_ch = 0;
        *out_seq = 0;
    }
}

/* Unsequenced group counter for SEND_UNSEQUENCED messages */
static unsigned short g_unsequenced_group = 1;

/* ── Build an unsequenced message on a specified ENet channel ────── */
static int build_unsequenced_raw(unsigned char *buf, int buflen,
                                 const unsigned char *raw_payload, int raw_len,
                                 unsigned char channel)
{
    unsigned char *p = buf;
    /* 4 (packet header) + 8 (SEND_UNSEQUENCED cmd) + payloadData */
    if (buflen < 12 + raw_len) return -1;

    /* ENet Protocol Header (4 bytes) */
    p = put_be16(p, (server_peer_id & 0x0FFF) |
                     ((unsigned short)(session_bits & 0x03) << 12) |
                     ENET_FLAG_SENT_TIME);
    p = put_be16(p, 0); /* sentTime = 0 */

    /* SEND_UNSEQUENCED command (8 bytes): no ACK flag */
    *p++ = ENET_CMD_SEND_UNSEQUENCED; /* 0x09 */
    *p++ = channel;
    p = put_be16(p, 0); /* reliableSequenceNumber = 0 for unsequenced */
    p = put_be16(p, g_unsequenced_group++);
    p = put_be16(p, (unsigned short)raw_len);

    if (raw_len > 0) {
        memcpy(p, raw_payload, raw_len);
        p += raw_len;
    }

    return (int)(p - buf);
}

/* ── Build ENet BANDWIDTH_LIMIT command ───────────────────────────────
 * Reports our estimated bandwidth to the server so Sunshine can
 * adjust encoding bitrate.  ENet protocol-level command (not encrypted). */
static int build_bandwidth_limit(unsigned char *buf, int buflen,
                                  unsigned int incoming_bw,
                                  unsigned int outgoing_bw)
{
    unsigned char *p = buf;
    if (buflen < 16) return -1; /* 4 header + 12 command */

    /* ENet Protocol Header */
    p = put_be16(p, (server_peer_id & 0x0FFF) |
                     ((unsigned short)(session_bits & 0x03) << 12) |
                     ENET_FLAG_SENT_TIME);
    p = put_be16(p, 0); /* sentTime */

    /* BANDWIDTH_LIMIT command (12 bytes): no ACK flag */
    *p++ = ENET_CMD_BANDWIDTH_LIMIT; /* 10 */
    *p++ = 0xFF;                      /* channelID = 0xFF */
    p = put_be16(p, 0);              /* reliableSequenceNumber = 0 */
    p = put_be32(p, incoming_bw);     /* incomingBandwidth (bytes/sec) */
    p = put_be32(p, outgoing_bw);     /* outgoingBandwidth (bytes/sec) */

    return (int)(p - buf);
}

/* ── Build encrypted control msg sent as unsequenced ─────────────── */
static int build_encrypted_unsequenced_msg(unsigned char *buf, int buflen,
                                           unsigned short msg_type,
                                           const unsigned char *payload, int pay_len,
                                           unsigned char channel)
{
    unsigned char encrypted_blob[128];
    unsigned char plaintext[96];
    unsigned char iv[12];
    size_t plain_len;
    unsigned short enc_len;
    unsigned int seq;
    int ret;

    if (!ctrl_crypto_ready) return -1;
    if (pay_len < 0 || pay_len > (int)(sizeof(plaintext) - 4)) return -1;

    plain_len = (size_t)(4 + pay_len);
    if (alloc_ctrl_enc_seq(&seq) < 0) return -1;

    /* V2 plaintext: LE16 type + LE16 payloadLength + payload */
    plaintext[0] = (unsigned char)(msg_type & 0xFF);
    plaintext[1] = (unsigned char)(msg_type >> 8);
    plaintext[2] = (unsigned char)(pay_len & 0xFF);
    plaintext[3] = (unsigned char)((unsigned int)pay_len >> 8);
    if (pay_len > 0)
        memcpy(plaintext + 4, payload, (size_t)pay_len);

    /* Encrypted control header: LE16 type=0x0001, LE16 length, LE32 seq */
    enc_len = (unsigned short)(4 + 16 + plain_len);
    encrypted_blob[0] = 0x01;
    encrypted_blob[1] = 0x00;
    encrypted_blob[2] = (unsigned char)(enc_len & 0xFF);
    encrypted_blob[3] = (unsigned char)(enc_len >> 8);
    encrypted_blob[4] = (unsigned char)(seq & 0xFF);
    encrypted_blob[5] = (unsigned char)((seq >> 8) & 0xFF);
    encrypted_blob[6] = (unsigned char)((seq >> 16) & 0xFF);
    encrypted_blob[7] = (unsigned char)((seq >> 24) & 0xFF);

    memset(iv, 0, sizeof(iv));
    iv[0] = encrypted_blob[4];
    iv[1] = encrypted_blob[5];
    iv[2] = encrypted_blob[6];
    iv[3] = encrypted_blob[7];
    iv[10] = (unsigned char)'C';
    iv[11] = (unsigned char)'C';

    if (lock_gcm_sem() < 0) return -1;

    ret = mbedtls_gcm_crypt_and_tag(&ctrl_gcm_ctx,
                                    MBEDTLS_GCM_ENCRYPT,
                                    plain_len,
                                    iv, sizeof(iv),
                                    NULL, 0,
                                    plaintext,
                                    encrypted_blob + 24,
                                    16,
                                    encrypted_blob + 8);
    unlock_gcm_sem();
    if (ret != 0) return -1;

    return build_unsequenced_raw(buf, buflen, encrypted_blob,
                                 (int)(8 + 16 + plain_len), channel);
}

static int build_encrypted_control_msg(unsigned char *buf, int buflen,
                                       unsigned short msg_type,
                                       const unsigned char *payload, int pay_len,
                                       unsigned char channel)
{
    unsigned char encrypted_blob[128];
    unsigned char plaintext[96];
    unsigned char iv[12];
    size_t plain_len;
    unsigned short enc_len;
    unsigned int seq;
    int ret;

    if (!ctrl_crypto_ready) {
        return -1;
    }
    if (pay_len < 0 || pay_len > (int)(sizeof(plaintext) - 4)) {
        return -1;
    }

    plain_len = (size_t)(4 + pay_len);
    if (alloc_ctrl_enc_seq(&seq) < 0) {
        return -1;
    }

    /* V2 plaintext: LE16 type + LE16 payloadLength + payload */
    plaintext[0] = (unsigned char)(msg_type & 0xFF);
    plaintext[1] = (unsigned char)(msg_type >> 8);
    plaintext[2] = (unsigned char)(pay_len & 0xFF);
    plaintext[3] = (unsigned char)((unsigned int)pay_len >> 8);
    if (pay_len > 0) {
        memcpy(plaintext + 4, payload, (size_t)pay_len);
    }

    /* Encrypted control header (AAD): LE16 type=0x0001, LE16 length, LE32 seq */
    enc_len = (unsigned short)(4 + 16 + plain_len);
    encrypted_blob[0] = 0x01;
    encrypted_blob[1] = 0x00;
    encrypted_blob[2] = (unsigned char)(enc_len & 0xFF);
    encrypted_blob[3] = (unsigned char)(enc_len >> 8);
    encrypted_blob[4] = (unsigned char)(seq & 0xFF);
    encrypted_blob[5] = (unsigned char)((seq >> 8) & 0xFF);
    encrypted_blob[6] = (unsigned char)((seq >> 16) & 0xFF);
    encrypted_blob[7] = (unsigned char)((seq >> 24) & 0xFF);

    memset(iv, 0, sizeof(iv));
    iv[0] = encrypted_blob[4];
    iv[1] = encrypted_blob[5];
    iv[2] = encrypted_blob[6];
    iv[3] = encrypted_blob[7];
    iv[10] = (unsigned char)'C';
    iv[11] = (unsigned char)'C';

    /* Output layout: [8-byte header][16-byte GCM tag][ciphertext] */
    if (lock_gcm_sem() < 0) {
        return -1;
    }

    ret = mbedtls_gcm_crypt_and_tag(&ctrl_gcm_ctx,
                                    MBEDTLS_GCM_ENCRYPT,
                                    plain_len,
                                    iv, sizeof(iv),
                                    NULL, 0,
                                    plaintext,
                                    encrypted_blob + 24,
                                    16,
                                    encrypted_blob + 8);
    unlock_gcm_sem();
    if (ret != 0) {
        ctrl_log("[CTRL] encrypt failed: -0x%04X\n", -ret);
        return -1;
    }

    return build_reliable_raw(buf, buflen, encrypted_blob,
                              (int)(8 + 16 + plain_len), channel);
}

/* ── Parse VERIFY_CONNECT from server ────────────────────────────── */
static int parse_verify_connect(const unsigned char *data, int len,
                                unsigned short *out_sent_time)
{
    const unsigned char *p;
    unsigned short hdr_peer;
    unsigned char cmd;

    ctrl_log("[CTRL] parse_verify_connect entry: len=%d\n", len);

    if (len < 48) return -1;  /* 4 header + 44 VERIFY_CONNECT minimum */

    /* Protocol header */
    hdr_peer = get_be16(data);
    *out_sent_time = get_be16(data + 2);

    /* There may be an ACK command first (for our CONNECT), skip it */
    p = data + 4;
    while (p + 4 <= data + len) {
        cmd = *p;
        unsigned char cmd_num = cmd & 0x0F;

        if (cmd_num == ENET_CMD_ACK) {
            ctrl_log("[CTRL] skipping ACK...\n");
            if (p + 8 > data + len) {
                ctrl_log("[CTRL] malformed ACK in VERIFY packet\n");
                return -1;
            }
            p += 8;
            continue;
        }

        if (cmd_num == ENET_CMD_VERIFY) {
            ctrl_log("[CTRL] found VERIFY at offset %ld\n", (long)(p - data));
            /* VERIFY_CONNECT is 44 bytes (command header + body) */
            if (p + 44 > data + len) {
                ctrl_log("[CTRL] truncated VERIFY_CONNECT (need 44, have %ld)\n", (long)(data + len - p));
                return -1;
            }
            ctrl_log("[CTRL] indices validated, reading contents...\n");
            ctrl_log("[CTRL] extracting peer and session bits...\n");
            server_peer_id = get_be16(p + 4);  /* outgoingPeerID */
            session_bits   = p[6] & 0x03;       /* incomingSessionID: what server validates on our packets */

            ctrl_log("[CTRL] verifying connectID...\n");
            unsigned int verify_cid = ((unsigned int)p[40] << 24) |
                                      ((unsigned int)p[41] << 16) |
                                      ((unsigned int)p[42] <<  8) |
                                      ((unsigned int)p[43]);
            if (verify_cid != my_connect_id) {
                ctrl_log("[CTRL] connectID mismatch: sent=%u got=%u\n",
                         my_connect_id, verify_cid);
                return -2;
            }

            ctrl_log("[CTRL] VERIFY_CONNECT ok: server_peer=%u session=%u\n",
                     (unsigned)server_peer_id, (unsigned)session_bits);
            return 0;
        }

        /* Unknown command — bail */
        break;
    }

    return -1;
}

static int decode_control_message(const unsigned char *data, int data_len,
                                  unsigned short *out_type,
                                  const unsigned char **out_payload,
                                  int *out_payload_len,
                                  unsigned char *scratch,
                                  int scratch_size)
{
    unsigned short marker;

    if (!data || data_len < 4 || !out_type || !out_payload || !out_payload_len) {
        return -1;
    }

    marker = get_le16(data);
    if (marker == 0x0001) {
        unsigned short enc_len;
        unsigned int seq;
        const unsigned char *tag;
        const unsigned char *ciphertext;
        int ciphertext_len;
        unsigned char iv[12];
        int ret;

        if (!scratch || scratch_size <= 0 || data_len < 24) {
            return -1;
        }

        enc_len = get_le16(data + 2);
        if (enc_len < 24 || (int)enc_len + 4 > data_len) {
            return -1;
        }

        seq = get_le32(data + 4);
        tag = data + 8;
        ciphertext = data + 24;
        ciphertext_len = (int)enc_len - 20;

        if (ciphertext_len <= 0 || ciphertext_len > scratch_size) {
            return -1;
        }

        memset(iv, 0, sizeof(iv));
        iv[0] = (unsigned char)(seq & 0xFF);
        iv[1] = (unsigned char)((seq >> 8) & 0xFF);
        iv[2] = (unsigned char)((seq >> 16) & 0xFF);
        iv[3] = (unsigned char)((seq >> 24) & 0xFF);
        iv[10] = (unsigned char)'H';
        iv[11] = (unsigned char)'C';

        if (lock_gcm_sem() < 0) {
            return -1;
        }

        ret = mbedtls_gcm_auth_decrypt(&ctrl_gcm_ctx,
                                       (size_t)ciphertext_len,
                                       iv, sizeof(iv),
                                       NULL, 0,
                                       tag, 16,
                                       ciphertext,
                                       scratch);
        unlock_gcm_sem();
        if (ret != 0 || ciphertext_len < 4) {
            return -1;
        }

        *out_type = get_le16(scratch);
        *out_payload_len = (int)get_le16(scratch + 2);
        if (*out_payload_len < 0 || *out_payload_len > ciphertext_len - 4) {
            return -1;
        }

        *out_payload = scratch + 4;
        return 0;
    }

    *out_type = marker;
    *out_payload = data + 2;
    *out_payload_len = data_len - 2;
    return 0;
}

static int ctrl_recv_thread(SceSize args, void *argp)
{
    unsigned char recv_buf[1024];
    unsigned char ack_buf[32];
    unsigned char scratch[256];
    struct sockaddr_in server_addr;
    (void)0;  /* rx_log_count removed — all control packets are now logged */

    /* Build server address once — always send ACKs to the known server
     * address rather than back to &src from recvfrom.  On PSP, recvfrom
     * may not populate sin_port reliably, which would cause all ACKs to
     * go to port 0 (nowhere) and Apollo to time out after 5 seconds. */
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_len    = (unsigned char)sizeof(server_addr);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port   = htons((unsigned short)g_control_server_port);
    server_addr.sin_addr.s_addr = inet_addr(g_video_server_ip);
    g_server_addr = server_addr; /* Save for IDR requests outside this thread */

    ctrl_log("[CTRL RX] thread started, server=%s:%d\n",
             g_video_server_ip, g_control_server_port);

    while (ctrl_running && me_running) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        int ret;

        /* Blocking recvfrom with SO_RCVTIMEO (2 s, set in control_stream_start).
         *
         * sceNetInetSelect() returns 0 immediately on PSP (both PPSSPP and
         * real hardware), which prevents recvfrom from ever being called.
         * The control handshake proves blocking recv with SO_RCVTIMEO works.
         * On timeout, recvfrom returns -1 with EAGAIN, letting us check
         * me_running each iteration for clean exit on Home press. */
        ret = (int)sceNetInetRecvfrom(ctrl_socket,
                                          recv_buf,
                                          sizeof(recv_buf),
                                          0,
                                          (struct sockaddr *)&src,
                                          &src_len);

        if (ret <= 0 || ret < 4) {
            continue;
        }

        {
            const unsigned char *p;
            const unsigned char *end = recv_buf + ret;
            unsigned short peer_header = get_be16(recv_buf);
            unsigned short header_flags = (unsigned short)(peer_header & ENET_FLAG_MASK);
            unsigned short recv_sent_time = 0;
            int header_size = (header_flags & ENET_FLAG_SENT_TIME) ? 4 : 2;

            if (ret < header_size) {
                continue;
            }

            if (header_flags & ENET_FLAG_COMPRESSED) {
                ctrl_log("[CTRL RX] unsupported compressed ENet header flags=0x%04X\n",
                         header_flags);
                continue;
            }

            if (header_flags & ENET_FLAG_SENT_TIME) {
                recv_sent_time = get_be16(recv_buf + 2);
            }

            p = recv_buf + header_size;

            while (p + 4 <= end) {
                unsigned char cmd = p[0];
                unsigned char channel_id = p[1];
                unsigned short rel_seq = get_be16(p + 2);
                unsigned char cmd_num = (unsigned char)(cmd & 0x0F);
                int need_ack = (cmd & ENET_CMD_FLAG_ACK) ? 1 : 0;

                /* Log important commands + every 50th packet for rate monitoring */
                {
                    static unsigned int rx_count = 0;
                    rx_count++;
                    if (cmd_num == ENET_CMD_DISCONNECT || cmd_num == ENET_CMD_VERIFY ||
                        cmd_num == ENET_CMD_SEND_RELIABLE || (rx_count % 50) == 0) {
                        ctrl_log("[CTRL RX] cmd=0x%02X ch=%u seq=%u ack=%d peer=%u cnt=%u\n",
                                 cmd, channel_id, (unsigned)rel_seq, need_ack,
                                 (unsigned)(peer_header & 0x0FFF), rx_count);
                    }
                }

                /* ── Universal ACK Handler ─────────────────────────
                 * ACK every reliable command the server sends.
                 * CRITICAL: Sunshine/ENet ignores ACKs if peerID or session bits are wrong.
                 * Use the peerID/session bits from the packet header we just received. */
                if (need_ack) {
                    int ack_len = build_ack_packet(ack_buf,
                                                   sizeof(ack_buf),
                                                   peer_header, 
                                                   channel_id,
                                                   rel_seq,
                                                   recv_sent_time);
                    if (ack_len > 0) {
                        /* FIXED: Use server_addr instead of src — PSP recvfrom
                         * does not reliably populate sin_port, so ACKs were
                         * going to port 0 (nowhere) and Sunshine timed us out. */
                        int ack_tx = (int)sceNetInetSendto(ctrl_socket, ack_buf, ack_len, 0,
                                         (struct sockaddr *)&server_addr, sizeof(server_addr));
                        /* Log ACK failures and DISCONNECT ACKs only */
                        if (ack_tx < 0 || cmd_num == ENET_CMD_DISCONNECT) {
                            ctrl_log("[CTRL RX] ACK sent=%d for cmd=0x%02X ch=%u seq=%u\n",
                                     ack_tx, cmd, channel_id, (unsigned)rel_seq);
                        }
                    }
                }

                /* ── ACK (1): 8 bytes — server acknowledges our reliable cmd ── */
                if (cmd_num == ENET_CMD_ACK) {
                    if (p + 8 > end) break;
                    {
                        /* Parse the ACKed sequence from the payload.
                         * ENet puts receivedReliableSeq at command offset 4-5. */
                        unsigned short acked_seq = get_be16(p + 4);
                        retx_ack(channel_id, acked_seq);
                    }
                    p += 8;
                    continue;
                }

                /* ── CONNECT (2): 48 bytes — shouldn't happen post-handshake ── */
                if (cmd_num == ENET_CMD_CONNECT) {
                    if (p + 48 > end) break;
                    p += 48;
                    continue;
                }

                /* ── VERIFY_CONNECT (3): 44 bytes — skip post-handshake ── */
                if (cmd_num == ENET_CMD_VERIFY) {
                    if (p + 44 > end) break;
                    /* Already ACKed above if requested */
                    p += 44;
                    continue;
                }

                /* ── DISCONNECT (4): 8 bytes — server is disconnecting us ── */
                if (cmd_num == ENET_CMD_DISCONNECT) {
                    if (p + 8 > end) break;
                    {
                        unsigned int disc_data = (p + 8 <= end) ?
                            (((unsigned int)p[4] << 24) | ((unsigned int)p[5] << 16) |
                             ((unsigned int)p[6] << 8)  | (unsigned int)p[7]) : 0;
                        ctrl_log("[CTRL RX] DISCONNECT received from server! ch=%u seq=%u data=0x%08X\n",
                                 channel_id, (unsigned)rel_seq, disc_data);
                        ctrl_log("[CTRL RX] Raw disconnect bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                    }
                    /* Don't kill the session immediately.  Sunshine may
                     * disconnect because the IDR decode took longer than
                     * its 10-second frame-ACK timeout.  The decode thread
                     * may still be processing queued frames that are ready
                     * for display.  Set a flag and let the ping thread
                     * handle a graceful shutdown after a drain window. */
                    ctrl_log("[CTRL RX] Ignoring disconnect — letting frame pipeline drain\n");
                    g_received_disconnect = 1;
                    p += 8;
                    break;
                }

                /* ── PING (5): 4 bytes — skip, ACK handled by Universal ── */
                if (cmd_num == ENET_CMD_PING) {
                    p += 4;
                    continue;
                }

                /* ── SEND_RELIABLE (6): 6 + dataLength bytes ─────── */
                if (cmd_num == ENET_CMD_SEND_RELIABLE) {
                    unsigned short payload_len;
                    const unsigned char *payload;

                    if (p + 6 > end) break;

                    payload_len = get_be16(p + 4);
                    if (p + 6 + payload_len > end) break;

                    /* ACK handled by Universal Handler above */

                    payload = p + 6;
                    /* RELIABLE message logging silenced for performance */
                    if (payload_len >= 4) {
                        unsigned short msg_type;
                        const unsigned char *msg_payload;
                        int msg_payload_len;

                        if (decode_control_message(payload,
                                                   payload_len,
                                                   &msg_type,
                                                   &msg_payload,
                                                   &msg_payload_len,
                                                   scratch,
                                                   sizeof(scratch)) == 0) {
                            /* Decoded type log silenced for performance */

                            /* 0x010E = Sunshine Gen7 server-hello / stream-config notification.
                             * Sunshine sends this reliably and retransmits until it gets an
                             * application-level ACK that echoes its full payload back verbatim.
                             * Sending a 1-byte empty reply caused infinite retransmission — the
                             * server stayed stuck and never emitted an IDR keyframe. */
                            if (msg_type == 0x010E) {
                                unsigned char reply_buf[128];
                                /* Echo the server's exact payload (29 bytes) back unchanged */
                                const unsigned char *echo_pay = msg_payload;
                                int echo_len = msg_payload_len;
                                /* Fallback: if payload is empty/missing, send a zero byte */
                                unsigned char fallback[1] = { 0x00 };
                                if (echo_len <= 0 || !echo_pay) {
                                    echo_pay = fallback; echo_len = 1;
                                }
                                int reply_len = build_encrypted_control_msg(
                                                    reply_buf, sizeof(reply_buf),
                                                    0x010E, echo_pay, echo_len, 0x00);
                                if (reply_len > 0) {
                                    /* FIXED: Use server_addr instead of &src.
                                     * PSP recvfrom does not populate sin_port reliably,
                                     * so &src has port=0 and the echo goes nowhere.
                                     * Without the app-level echo, Sunshine retransmits
                                     * hello indefinitely then disconnects after 5s. */
                                    int tx = (int)sceNetInetSendto(
                                                ctrl_socket, reply_buf, reply_len, 0,
                                                (const struct sockaddr *)&server_addr,
                                                sizeof(server_addr));
                                    /* Store in retransmit buffer for gap recovery */
                                    if (tx > 0) {
                                        unsigned char rch = 0;
                                        unsigned short rseq = 0;
                                        pkt_get_channel_seq(reply_buf, reply_len, &rch, &rseq);
                                        retx_store(reply_buf, reply_len, rch, rseq);
                                    }
                                    ctrl_log("[CTRL RX] 0x010E server-hello: echoed %d bytes tx=%d\n",
                                             echo_len, tx);
                                } else {
                                    ctrl_log("[CTRL RX] 0x010E server-hello: reply build failed\n");
                                }
                            } else if (msg_type == 0x0100 || msg_type == 0x0109) {
                                unsigned int reason = 0;

                                if (msg_type == 0x0109 && msg_payload_len >= 4) {
                                    reason = ((unsigned int)msg_payload[0] << 24) |
                                             ((unsigned int)msg_payload[1] << 16) |
                                             ((unsigned int)msg_payload[2] << 8) |
                                             (unsigned int)msg_payload[3];
                                } else if (msg_payload_len >= 2) {
                                    reason = (unsigned int)get_le16(msg_payload);
                                }

                                ctrl_log("[CTRL RX] TERMINATION type=0x%04X reason=0x%08X — setting me_running=0\n",
                                         msg_type, reason);
                                g_stream_status = (msg_type == 0x0109) ? 2 : 1; /* 2=Paused, 1=Stopped */
                                me_running = 0;
                            } else {
                                /* Log all other unhandled types with full payload hex */
                                int _pi;
                                ctrl_log("[CTRL RX] unhandled type=0x%04X plen=%d payload:",
                                         msg_type, msg_payload_len);
                                for (_pi = 0; _pi < msg_payload_len && _pi < 16; _pi++)
                                    ctrl_log(" %02X", msg_payload[_pi]);
                                ctrl_log("\n");
                            }
                        } else {
                            ctrl_log("[CTRL RX] decode_control_message failed for payload_len=%u\n", payload_len);
                        }
                    }

                    p += 6 + payload_len;
                    continue;
                }

                /* ── SEND_UNRELIABLE (7): 8 + dataLength bytes ───── */
                if (cmd_num == ENET_CMD_SEND_UNRELIABLE) {
                    unsigned short payload_len;
                    if (p + 8 > end) break;
                    payload_len = get_be16(p + 6);
                    if (p + 8 + payload_len > end) break;
                    p += 8 + payload_len;
                    continue;
                }

                /* ── SEND_FRAGMENT (8): 24 + dataLength bytes ─────── */
                if (cmd_num == ENET_CMD_SEND_FRAGMENT) {
                    unsigned short payload_len;
                    if (p + 24 > end) break;
                    payload_len = get_be16(p + 4);
                    if (p + 24 + payload_len > end) break;
                    /* ACK handled by Universal Handler */
                    p += 24 + payload_len;
                    continue;
                }

                /* ── SEND_UNSEQUENCED (9): 8 + dataLength bytes ──── */
                if (cmd_num == ENET_CMD_SEND_UNSEQUENCED) {
                    unsigned short payload_len;
                    if (p + 8 > end) break;
                    payload_len = get_be16(p + 6);
                    if (p + 8 + payload_len > end) break;
                    p += 8 + payload_len;
                    continue;
                }

                /* ── BANDWIDTH_LIMIT (10): 12 bytes — parse and store ── */
                if (cmd_num == ENET_CMD_BANDWIDTH_LIMIT) {
                    if (p + 12 > end) break;
                    s_server_incoming_bw = ((unsigned int)p[4] << 24) |
                                           ((unsigned int)p[5] << 16) |
                                           ((unsigned int)p[6] << 8) |
                                            (unsigned int)p[7];
                    s_server_outgoing_bw = ((unsigned int)p[8] << 24) |
                                           ((unsigned int)p[9] << 16) |
                                           ((unsigned int)p[10] << 8) |
                                            (unsigned int)p[11];
                    ctrl_log("[CTRL RX] BANDWIDTH_LIMIT server_in=%u server_out=%u\n",
                             s_server_incoming_bw, s_server_outgoing_bw);
                    p += 12;
                    continue;
                }

                /* ── THROTTLE_CONFIGURE (11): 16 bytes — parse and store ── */
                if (cmd_num == ENET_CMD_THROTTLE_CONFIGURE) {
                    if (p + 16 > end) break;
                    s_throttle_interval = ((unsigned int)p[4] << 24) |
                                          ((unsigned int)p[5] << 16) |
                                          ((unsigned int)p[6] << 8) |
                                           (unsigned int)p[7];
                    s_throttle_accel = ((unsigned int)p[8] << 24) |
                                       ((unsigned int)p[9] << 16) |
                                       ((unsigned int)p[10] << 8) |
                                        (unsigned int)p[11];
                    s_throttle_decel = ((unsigned int)p[12] << 24) |
                                       ((unsigned int)p[13] << 16) |
                                       ((unsigned int)p[14] << 8) |
                                        (unsigned int)p[15];
                    ctrl_log("[CTRL RX] THROTTLE_CONFIGURE interval=%u accel=%u decel=%u\n",
                             s_throttle_interval, s_throttle_accel, s_throttle_decel);
                    p += 16;
                    continue;
                }

                /* ── SEND_UNRELIABLE_FRAGMENT (12): 24 + dataLength ── */
                if (cmd_num == ENET_CMD_SEND_UNRELIABLE_FRAGMENT) {
                    unsigned short payload_len;
                    if (p + 24 > end) break;
                    payload_len = get_be16(p + 4);
                    if (p + 24 + payload_len > end) break;
                    p += 24 + payload_len;
                    continue;
                }

                /* Unknown command — log and bail out of this packet */
                ctrl_log("[CTRL RX] unknown cmd=0x%02X\n", cmd);
                break;
            }
        }
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

/* ── Periodic control ping thread ────────────────────────────────── */
static int ctrl_ping_thread(SceSize args, void *argp)
{
    unsigned char pkt[128];
    /* Periodic ping payload: LE16 length=4, LE32 timestamp=0, pad to 8 bytes */
    unsigned char ping_payload[8];
    int pkt_len;
    struct sockaddr_in dst;
    unsigned int count = 0;

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (unsigned char)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_control_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    ctrl_log("[CTRL PING] thread started\n");

    while (ctrl_running && me_running) {
        /* Update heartbeat for main loop watchdog (B-4) */
        g_ctrl_ping_heartbeat_us = sceKernelGetSystemTimeLow();

        /* Build periodic ping: type 0x0200, payload = {04 00 00 00 00 00 00 00} */
        memset(ping_payload, 0, sizeof(ping_payload));
        ping_payload[0] = 0x04;  /* LE16: length = 4 */
        ping_payload[1] = 0x00;

        pkt_len = build_encrypted_control_msg(pkt, sizeof(pkt),
                              CTRL_TYPE_PERIODIC,
                              ping_payload, 8, 0x00);
        if (pkt_len > 0) {
            int tx = 0;
            unsigned char pkt_ch = 0;
            unsigned short pkt_seq = 0;
            /* Single-fire + retransmit: rely on the retransmit buffer to fill
             * gaps from packet loss, rather than triple-firing every ping. */
            tx = (int)sceNetInetSendto(ctrl_socket, pkt, pkt_len, 0,
                             (struct sockaddr *)&dst, sizeof(dst));
            /* Store in retransmit buffer for gap recovery */
            if (tx > 0) {
                pkt_get_channel_seq(pkt, pkt_len, &pkt_ch, &pkt_seq);
                retx_store(pkt, pkt_len, pkt_ch, pkt_seq);
            }
            if (count <= 5 || (count % 100) == 0 || tx < 0) {
                if (tx < 0)
                    ctrl_log("[CTRL PING] #%u pkt=%d tx=%d lgf=%u errno=%d\n",
                             count, pkt_len, tx, g_last_good_frame, sceNetInetGetErrno());
                else
                    ctrl_log("[CTRL PING] #%u pkt=%d tx=%d lgf=%u\n",
                             count, pkt_len, tx, g_last_good_frame);
            }
        } else {
            if (count <= 5)
                ctrl_log("[CTRL PING] #%u build_failed\n", count);
        }
        count++;

        /* ── Per-frame FEC status (0x5502) piggyback ───────────────
         * Sunshine tracks client health via per-frame FEC status updates.
         * The decode thread sends one when FEC submits a frame, but during
         * long IDR decodes (11s on 333 MHz PSP) the decode thread blocks
         * and no status updates reach the server.  By piggybacking a
         * status update on every keepalive ping, Sunshine sees frame
         * progress even while the IDR is still being decoded.  This
         * prevents the 10-second "no frame ACK" disconnect.
         *
         * CRITICAL: Use the cached values from rtp_fec.c (g_fec_last_*)
         * instead of zeros.  Sending highestReceivedSequenceNumber=0
         * confuses Sunshine's flow control — the server interprets it as
         * "client received no packets" and eventually stops sending video.
         *
         * STALL RECOVERY: When lgf is frozen (no new frames decoded),
         * advance the piggybacked frame_index by 1 each second.  This
         * prevents the server's pending-frame counter from growing past
         * its threshold and permanently stopping video.  The server sees
         * "client is slowly catching up" and keeps its send window open.
         * Real frames resume once the IDR arrives and decoding restarts.
         *
         * Send every 5th ping (2/sec) to keep Sunshine's send window
         * open during decode stalls. Was 1/sec but server's pending-frame
         * counter outpaced our acks. */
        if (ctrl_crypto_ready && (count % 5) == 0) {
            extern volatile u16 g_fec_last_highest_seq;
            extern volatile u16 g_fec_last_next_contig_seq;
            extern volatile u16 g_fec_last_data_pkts;
            extern volatile u16 g_fec_last_parity_pkts;
            extern volatile u16 g_fec_last_recv_data;
            extern volatile u16 g_fec_last_recv_parity;
            extern volatile u8  g_fec_last_fec_pct;

            /* Stall recovery: advance frame_index when lgf is frozen.
             * This keeps the server's send window open.
             * s_stall_advance resets to 0 when lgf changes.
             *
             * Advance rate: +60/sec (2x framerate) to give the server
             * extra headroom in its pending-frame window.  At 1x (30/sec)
             * the gap grew during decode stalls and killed the stream.
             * Cap at 7200 (120s worth at 60/sec) to prevent overflow. */
            static unsigned int s_piggy_last_lgf = 0;
            static unsigned int s_stall_advance = 0;
            unsigned int piggy_frame;

            if (g_last_good_frame != s_piggy_last_lgf) {
                /* lgf advanced — reset stall advance */
                s_piggy_last_lgf = g_last_good_frame;
                s_stall_advance = 0;
            } else {
                /* lgf frozen — advance by 30 each half-second (60/sec)
                 * to outpace the server's frame counter (~30fps) and keep
                 * Sunshine's send window comfortably open during stalls. */
                if (s_stall_advance < 7200)
                    s_stall_advance += 30;
            }
            piggy_frame = g_last_good_frame + s_stall_advance;

            control_stream_send_fec_status(
                piggy_frame,
                g_fec_last_highest_seq,      /* real highestReceivedSequenceNumber */
                g_fec_last_next_contig_seq,  /* real nextContiguousSequenceNumber */
                0,                           /* missingPacketsBeforeHighestReceived */
                g_fec_last_data_pkts > 0 ? g_fec_last_data_pkts : 1,
                g_fec_last_parity_pkts,
                g_fec_last_recv_data > 0 ? g_fec_last_recv_data : 1,
                g_fec_last_recv_parity,
                g_fec_last_fec_pct,
                0,      /* multiFecBlockIndex */
                1       /* multiFecBlockCount */
            );
        }

        /* ── Bandwidth estimation: compute throughput over 5s window ───
         * Reports incoming bandwidth to the server via ENet BANDWIDTH_LIMIT
         * so Sunshine can adjust encoding bitrate.  This closes the
         * "no bandwidth estimation" gap vs. ENet's built-in tracking. */
        if (count > 0 && (count % 50) == 0 && ctrl_socket >= 0) {
            extern volatile u32 g_fec_total_bytes_received;
            u32 now_bw = sceKernelGetSystemTimeLow();
            u32 bytes_now = g_fec_total_bytes_received;

            if (s_bw_last_time_us != 0) {
                u32 dt_us = now_bw - s_bw_last_time_us;
                u32 dbytes = bytes_now - s_bw_last_bytes;
                if (dt_us > 100000) { /* at least 100ms elapsed */
                    s_estimated_bw_bps = (u32)((u64)dbytes * 1000000ULL / (u64)dt_us);

                    /* Send BANDWIDTH_LIMIT to server.
                     * Apply bandwidth report scaling (BW_REPORT_SCALE_PCT):
                     *   100 = report exact measured bandwidth (default)
                     *   120 = inflate 20% → server sends higher quality
                     *    80 = deflate 20% → smaller frames, faster decode
                     * Inflation risks WiFi congestion; deflation reduces quality
                     * but lowers latency.  Tuned for 802.11b headroom. */
                    {
                        unsigned char bw_buf[20];
                        u32 reported_bw = s_estimated_bw_bps;
#define BW_REPORT_SCALE_PCT 115  /* inflate 15% to prevent premature server downgrade */
                        reported_bw = (u32)((u64)reported_bw * BW_REPORT_SCALE_PCT / 100);
                        /* Phase 5.9: Apply quality-based BW scaling */
                        reported_bw = (u32)((u64)reported_bw * (u32)s_quality_bw_scale_pct / 100);
                        int bw_len = build_bandwidth_limit(bw_buf, sizeof(bw_buf),
                                                           reported_bw, 0);
                        if (bw_len > 0) {
                            sceNetInetSendto(ctrl_socket, bw_buf, bw_len, 0,
                                             (struct sockaddr *)&dst, sizeof(dst));
                        }

                        if (count <= 10 || (count % 300) == 0) {
                            ctrl_log("[CTRL BW] raw=%ukbps scaled=%ukbps (x%d%%) rto=%uus\n",
                                     s_estimated_bw_bps * 8 / 1000,
                                     reported_bw * 8 / 1000,
                                     BW_REPORT_SCALE_PCT,
                                     s_rto_us);
                        }
                    }
                }
            }
            s_bw_last_bytes = bytes_now;
            s_bw_last_time_us = now_bw;

            /* Update connection quality metrics alongside bandwidth */
            update_connection_quality(s_estimated_bw_bps);
        }

        if (count % 50 == 0) { /* 50 × 100ms = 5 seconds */
            if (!g_idr_fully_decoded) {
                ctrl_log("[CTRL PING] IDR accumulation incomplete, requesting IDR...\n");
                control_stream_request_idr();
            }
        }

        /* Periodic IDR refresh: every 60 seconds during normal streaming,
         * force-request an IDR to reset the H.264 reference chain.
         * At low bitrate on 802.11b WiFi, quantization errors accumulate
         * in P-frame references (especially without HEVC intra-refresh).
         * Without periodic IDR, the picture progressively washes out.
         *
         * Only fires when g_idr_fully_decoded is set (normal streaming).
         * Must clear g_idr_fully_decoded before requesting, otherwise
         * the request is silently suppressed by control_stream_request_idr().
         *
         * Reduced from 10s to 60s to prevent IDR flood on lossy WiFi.
         * Frequent IDR requests cause Sunshine to produce large keyframes
         * that are more vulnerable to partial loss, creating a vicious
         * cycle: lost IDR → request another → lost again → server gives up. */
        if (count > 0 && (count % 600) == 0 && g_idr_fully_decoded) {
            ctrl_log("[CTRL PING] Periodic IDR refresh (60s)\n");
            g_idr_fully_decoded = 0;
            control_stream_request_idr();
        }

        /* Stall recovery: if no new frames in 5 seconds, request IDR.
         * The host may have stopped sending because it missed the ack
         * for the next frame, or a control message was lost.
         *
         * CRITICAL: Must reset g_idr_fully_decoded before requesting IDR,
         * otherwise the request is silently suppressed and the stream
         * can NEVER recover from a stall. */
        {
            static unsigned int stall_lgf = 0;
            static unsigned int stall_ticks = 0;
            if (g_last_good_frame > 0 && g_last_good_frame == stall_lgf) {
                stall_ticks++;
                if (stall_ticks == 50) { /* 50 × 100ms = 5 seconds */
                    ctrl_log("[CTRL PING] STALL lgf=%u for 5s, requesting IDR\n",
                             g_last_good_frame);
                    g_idr_fully_decoded = 0;
                    control_stream_request_idr();
                }
                if (stall_ticks == 100) { /* 100 × 100ms = 10 seconds */
                    ctrl_log("[CTRL PING] STALL lgf=%u for 10s, requesting IDR (urgent)\n",
                             g_last_good_frame);
                    g_idr_fully_decoded = 0;
                    control_stream_request_idr();
                }
            } else {
                stall_lgf = g_last_good_frame;
                stall_ticks = 0;
            }
        }

        /* ── Retransmit scan: resend un-ACKed reliable packets ─────
         * This fills gaps from WiFi packet loss, preventing the server's
         * ENet dispatch queue from permanently stalling on a missing seq.
         * Runs every ping cycle (100ms). Only retransmits packets that
         * haven't been ACKed within 300ms, up to RETX_MAX_TRIES times. */
        if (ctrl_socket >= 0) {
            int retx_count = retx_scan(ctrl_socket, &dst);
            if (retx_count > 0 && (count <= 5 || (count % 100) == 0)) {
                ctrl_log("[CTRL PING] retransmitted %d packets\n", retx_count);
            }
        }

        sceKernelDelayThread(100000); /* 100 ms — matches moonlight-common-c ping interval */

        /* Graceful drain: after receiving DISCONNECT, keep running for
         * up to 5 seconds so the decode thread can process queued frames
         * and push them to the display ring.  Then cleanly shut down. */
        if (g_received_disconnect) {
            static int drain_ticks = 0;
            drain_ticks++;
            if (drain_ticks >= 50) {  /* 50 × 100ms = 5 seconds */
                ctrl_log("[CTRL PING] frame drain window expired — shutting down\n");
                g_stream_status = 1;
                me_running = 0;
            }
        }
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * control_stream_send_fec_status - Send per-frame FEC status (0x5502)
 * Sunshine uses this to track client frame health. Sent as unsequenced.
 * ══════════════════════════════════════════════════════════════════ */
int control_stream_send_fec_status(unsigned int frame_index,
                                   unsigned short highest_seq,
                                   unsigned short next_contig_seq,
                                   unsigned short missing_before_highest,
                                   unsigned short total_data,
                                   unsigned short total_parity,
                                   unsigned short received_data,
                                   unsigned short received_parity,
                                   unsigned char  fec_pct,
                                   unsigned char  multi_fec_idx,
                                   unsigned char  multi_fec_cnt)
{
    unsigned char pkt[192];
    unsigned char payload[23];  /* SS_FRAME_FEC_STATUS is 23 bytes, all BE */
    int pkt_len;

    if (!ctrl_crypto_ready || ctrl_socket < 0) return -1;

    /* All fields big-endian per Sunshine protocol */
    payload[0]  = (unsigned char)(frame_index >> 24);
    payload[1]  = (unsigned char)(frame_index >> 16);
    payload[2]  = (unsigned char)(frame_index >> 8);
    payload[3]  = (unsigned char)(frame_index);
    payload[4]  = (unsigned char)(highest_seq >> 8);
    payload[5]  = (unsigned char)(highest_seq);
    payload[6]  = (unsigned char)(next_contig_seq >> 8);
    payload[7]  = (unsigned char)(next_contig_seq);
    payload[8]  = (unsigned char)(missing_before_highest >> 8);
    payload[9]  = (unsigned char)(missing_before_highest);
    payload[10] = (unsigned char)(total_data >> 8);
    payload[11] = (unsigned char)(total_data);
    payload[12] = (unsigned char)(total_parity >> 8);
    payload[13] = (unsigned char)(total_parity);
    payload[14] = (unsigned char)(received_data >> 8);
    payload[15] = (unsigned char)(received_data);
    payload[16] = (unsigned char)(received_parity >> 8);
    payload[17] = (unsigned char)(received_parity);
    payload[18] = fec_pct;
    payload[19] = multi_fec_idx;
    payload[20] = multi_fec_cnt;

    pkt_len = build_encrypted_unsequenced_msg(pkt, sizeof(pkt),
                                              0x5502, payload, 21, CTRL_CHANNEL_GENERIC);
    if (pkt_len <= 0) return -1;

    return (int)sceNetInetSendto(ctrl_socket, pkt, pkt_len, 0,
                                 (struct sockaddr *)&g_server_addr,
                                 sizeof(g_server_addr));
}

/* ══════════════════════════════════════════════════════════════════
 * control_stream_send_input - Send input through encrypted control channel
 *
 * Uses UNSEQUENCED delivery: the PSP's lightweight ENet lacks retransmission,
 * so a single lost reliable packet causes the server to buffer ALL subsequent
 * input on that channel forever.  Unsequenced bypasses ordered delivery,
 * letting each packet be processed independently — lost packets are simply
 * skipped and the next one carries full current state.
 * ══════════════════════════════════════════════════════════════════ */
int control_stream_send_input(const unsigned char *payload, int payload_len,
                              unsigned char channel)
{
    unsigned char pkt[128];
    int pkt_len;
    struct sockaddr_in dst;
    int ret = 0;

    if (!ctrl_crypto_ready || ctrl_socket < 0) {
        return -1;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (unsigned char)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_control_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    pkt_len = build_encrypted_unsequenced_msg(pkt, sizeof(pkt),
                          CTRL_TYPE_INPUT,
                          payload, payload_len, channel);
    if (pkt_len <= 0) {
        return -1;
    }

    ret = (int)sceNetInetSendto(ctrl_socket, pkt, pkt_len, 0,
                                (struct sockaddr *)&dst, sizeof(dst));
    return ret;
}

/* ══════════════════════════════════════════════════════════════════
 * Request IDR Frame
 * ══════════════════════════════════════════════════════════════════ */
int control_stream_request_idr(void)
{
    /* With intra-refresh active, suppress IDR requests.  IDR keyframes are
     * 65KB+ (63 packets) and WiFi drops 20-50%.  Intra-refresh P-frames
     * are ~1KB (1-2 packets), easily recovered by FEC, and progressively
     * repaint the picture within ~10 frames.  Requesting IDR here creates
     * a vicious cycle: corrupt IDR → request another → corrupt again. */
    if (g_intra_refresh_active) {
        return 0;
    }

    /* NOTE: We intentionally do NOT suppress IDR requests when
     * g_idr_fully_decoded is set.  moonlight-common-c sends IDR requests
     * freely whenever recovery is needed.  Our old guard here silently
     * ate all IDR requests after the first successful decode, making
     * watchdog recovery impossible (server only sends P-frames without
     * an IDR request).  The rate limiter below (2/sec) is sufficient
     * to prevent flooding. */

    extern void rtp_reassembly_flush_partial_frame(void);

    unsigned char send_buf[128];
    /* IDR request payload: two zero bytes (matches Moonlight reference clients) */
    unsigned char idr_payload[] = { 0x00, 0x00 };
    int pkt_len;
    int ret_urgent = -1;

    if (ctrl_socket < 0 || !ctrl_running) {
        return -1;
    }

    /* Rate-limit IDR requests with exponential backoff.
     * Starts at 500ms, doubles on each consecutive request up to 4s max.
     * Resets to 500ms when a new IDR decode succeeds (g_idr_fully_decoded
     * goes from 0→1).  This prevents flooding Sunshine during bad WiFi
     * while still allowing rapid recovery when signal improves.
     * First 3 requests bypass throttle for rapid-fire during initial connection. */
    {
        static u64 s_last_idr_tick = 0;
        static int s_idr_count = 0;
        static u32 s_idr_backoff_us = 500000; /* start at 500ms */
        static int s_idr_prev_decoded = 0;
        u64 now;
        sceRtcGetCurrentTick(&now);

        /* Reset backoff when an IDR decode succeeds */
        if (g_idr_fully_decoded && !s_idr_prev_decoded) {
            ctrl_log("[IDR BACKOFF] reset to 500ms (IDR decoded ok, count=%d)\n", s_idr_count);
            s_idr_backoff_us = 500000; /* reset to 500ms */
        }
        s_idr_prev_decoded = g_idr_fully_decoded;

        if (s_idr_count >= 3 && s_last_idr_tick != 0 && (now - s_last_idr_tick) < s_idr_backoff_us) {
            ctrl_log("[IDR BACKOFF] throttled (backoff=%ums count=%d)\n",
                     s_idr_backoff_us / 1000, s_idr_count);
            return 0; /* throttled — recent IDR already in flight */
        }
        s_last_idr_tick = now;
        s_idr_count++;

        /* Exponential backoff: double interval after each send, cap at 4s */
        if (s_idr_count >= 3) {
            u32 prev_backoff = s_idr_backoff_us;
            s_idr_backoff_us *= 2;
            if (s_idr_backoff_us > 4000000)
                s_idr_backoff_us = 4000000;
            ctrl_log("[IDR BACKOFF] %ums -> %ums (count=%d)\n",
                     prev_backoff / 1000, s_idr_backoff_us / 1000, s_idr_count);
        }
    }

    /* CTRL_TYPE_IDR_REQ (0x0302) is the correct Gen7Enc IDR request.
     * In Gen7Enc, IDR requests must be sent over the URGENT channel (0x01).
     * This matches packetTypesGen7Enc[0] in moonlight-common-c's ControlStream.c. */
    pkt_len = build_encrypted_control_msg(send_buf, sizeof(send_buf),
                                          CTRL_TYPE_IDR_REQ, idr_payload, 2, CTRL_CHANNEL_URGENT);
    if (pkt_len <= 0) {
        ctrl_log("[CTRL] IDR request build failed\n");
        return -2;
    }

    /* Single send + retransmit buffer handles gap recovery on lossy WiFi. */
    ret_urgent = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                       (struct sockaddr *)&g_server_addr,
                                       sizeof(g_server_addr));
    /* Store in retransmit buffer for gap recovery */
    if (ret_urgent > 0) {
        unsigned char rch = 0;
        unsigned short rseq = 0;
        pkt_get_channel_seq(send_buf, pkt_len, &rch, &rseq);
        retx_store(send_buf, pkt_len, rch, rseq);
    }

    ctrl_log("[CTRL] IDR request (0x0302) urgent=%d\n", ret_urgent);
    /* removed destructive flush: rtp_reassembly_flush_partial_frame(); */
    return (ret_urgent > 0) ? 0 : -3;
}

/* ══════════════════════════════════════════════════════════════════
 * Request Reference Frame Invalidation (RFI)
 *
 * Tells Sunshine that frames [startFrame..endFrame] were lost so the
 * encoder avoids referencing them.  Sunshine responds with a recovery
 * P-frame instead of a full IDR, which is smaller and keeps the
 * existing reference chain intact.
 *
 * Payload: SS_RFI_REQUEST = { firstFrameIndex LE32, reserved LE32,
 *                             lastFrameIndex LE32, reserved[3] LE32 }
 * Total = 24 bytes, sent on CTRL_CHANNEL_URGENT (0x01).
 * ══════════════════════════════════════════════════════════════════ */
int control_stream_request_rfi(unsigned int start_frame, unsigned int end_frame)
{
    unsigned char send_buf[128];
    unsigned char rfi_payload[24];
    int pkt_len;
    int ret_urgent = -1;

    if (ctrl_socket < 0 || !ctrl_running) {
        return -1;
    }

    /* Rate-limit same as IDR: max 2/sec */
    {
        static u64 s_last_rfi_tick = 0;
        static int s_rfi_count = 0;
        u64 now;
        sceRtcGetCurrentTick(&now);
        if (s_rfi_count >= 5 && s_last_rfi_tick != 0 && (now - s_last_rfi_tick) < 500000) {
            return 0;
        }
        s_last_rfi_tick = now;
        s_rfi_count++;
    }

    /* Build SS_RFI_REQUEST payload (24 bytes, all LE32) */
    memset(rfi_payload, 0, sizeof(rfi_payload));
    rfi_payload[0]  = (unsigned char)(start_frame);
    rfi_payload[1]  = (unsigned char)(start_frame >> 8);
    rfi_payload[2]  = (unsigned char)(start_frame >> 16);
    rfi_payload[3]  = (unsigned char)(start_frame >> 24);
    /* bytes 4-7: reserved = 0 */
    rfi_payload[8]  = (unsigned char)(end_frame);
    rfi_payload[9]  = (unsigned char)(end_frame >> 8);
    rfi_payload[10] = (unsigned char)(end_frame >> 16);
    rfi_payload[11] = (unsigned char)(end_frame >> 24);
    /* bytes 12-23: reserved = 0 */

    pkt_len = build_encrypted_control_msg(send_buf, sizeof(send_buf),
                                          CTRL_TYPE_RFI_REQ, rfi_payload, 24, CTRL_CHANNEL_URGENT);
    if (pkt_len <= 0) {
        ctrl_log("[CTRL] RFI request build failed\n");
        return -2;
    }

    for (int r = 0; r < 3; r++) {
        ret_urgent = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                           (struct sockaddr *)&g_server_addr,
                                           sizeof(g_server_addr));
        sceKernelDelayThread(1000);
    }

    ctrl_log("[CTRL] RFI request (0x0301) frames %u-%u urgent=%d\n",
             start_frame, end_frame, ret_urgent);
    return (ret_urgent > 0) ? 0 : -3;
}


/* ══════════════════════════════════════════════════════════════════
 * control_stream_start - Main entry point
 * ══════════════════════════════════════════════════════════════════ */
int control_stream_start(void)
{
    static struct sockaddr_in dst;
    static unsigned char send_buf[64];
    static unsigned char recv_buf[128];
    static int pkt_len, ret, attempt;
    static unsigned short recv_sent_time;
    static struct timeval tv;

    recv_sent_time = 0;

    /* Idempotency guard: stop any existing instances before restarting. */
    if (ctrl_socket >= 0 || ctrl_recv_thread_id >= 0) {
        control_stream_stop();
    }

    ctrl_log("[CTRL] starting, server=%s:%d connect_data=%u\n",
             g_video_server_ip, g_control_server_port, g_control_connect_data);

    /* Reset frame counter so a new session starts at frame index 0.
     * Prevents stale frame IDs from causing initial video to be dropped. */
    g_last_good_frame = 0;
    g_received_disconnect = 0;

    if (!g_remote_input_key_valid) {
        ctrl_log("[CTRL] missing remote input key (launch rikey)\n");
        return -1;
    }

    mbedtls_gcm_init(&ctrl_gcm_ctx);
    ret = mbedtls_gcm_setkey(&ctrl_gcm_ctx, MBEDTLS_CIPHER_ID_AES,
                             g_remote_input_key, 128);
    if (ret != 0) {
        ctrl_log("[CTRL] gcm_setkey failed: -0x%04X\n", -ret);
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        return -1;
    }
    ctrl_crypto_ready = 1;
    next_ctrl_enc_seq = 0;
    /* ENet reliable seqs start at 1 for each channel */
    {
        int i;
        for (i = 0; i < CTRL_CHANNEL_COUNT; i++)
            reliable_seq_per_ch[i] = 1;
    }
    retx_clear(); /* Reset retransmit buffer for new session */

    ctrl_seq_sem_id = sceKernelCreateSema("ctrl_seq_sem", 0, 1, 1, NULL);
    if (ctrl_seq_sem_id < 0) {
        ctrl_log("[CTRL] seq semaphore create failed: %d\n", (int)ctrl_seq_sem_id);
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        ctrl_crypto_ready = 0;
        return -1;
    }

    ctrl_gcm_sem_id = sceKernelCreateSema("ctrl_gcm_sem", 0, 1, 1, NULL);
    if (ctrl_gcm_sem_id < 0) {
        ctrl_log("[CTRL] gcm semaphore create failed: %d\n", (int)ctrl_gcm_sem_id);
        sceKernelDeleteSema(ctrl_seq_sem_id);
        ctrl_seq_sem_id = -1;
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        ctrl_crypto_ready = 0;
        return -1;
    }

    /* Create UDP socket */
    ctrl_socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (ctrl_socket < 0) {
        ctrl_log("[CTRL] socket() failed\n");
        sceKernelDeleteSema(ctrl_gcm_sem_id);
        ctrl_gcm_sem_id = -1;
        sceKernelDeleteSema(ctrl_seq_sem_id);
        ctrl_seq_sem_id = -1;
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        ctrl_crypto_ready = 0;
        return -1;
    }

    /* Bind control socket to advertised client port 57999 and alias IP if needed */
    {
        extern char g_local_bind_ip[16];
        struct sockaddr_in my_bind;
        int reuse = 1;
        int rcvbuf = 16384;
        int sndbuf = 32768;
        sceNetInetSetsockopt(ctrl_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sceNetInetSetsockopt(ctrl_socket, SOL_SOCKET, 0x1002 /* SO_RCVBUF */, &rcvbuf, sizeof(rcvbuf));
        sceNetInetSetsockopt(ctrl_socket, SOL_SOCKET, 0x1001 /* SO_SNDBUF */, &sndbuf, sizeof(sndbuf));
        
        memset(&my_bind, 0, sizeof(my_bind));
        my_bind.sin_len    = (unsigned char)sizeof(my_bind);
        my_bind.sin_family = AF_INET;
        if (g_local_bind_ip[0] != '\0') {
            my_bind.sin_addr.s_addr = inet_addr(g_local_bind_ip);
        } else {
            my_bind.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        my_bind.sin_port   = htons(57999);
        if (sceNetInetBind(ctrl_socket, (struct sockaddr *)&my_bind, sizeof(my_bind)) != 0) {
            ctrl_log("[CTRL] bind() to 57999 failed\n");
            /* non-fatal, OS will assign an ephemeral port, but IP might be wrong on loopback */
        }
    }

    /* 2 second receive timeout */
    tv.tv_sec  = 2;
    tv.tv_usec = 0;
    sceNetInetSetsockopt(ctrl_socket, SOL_SOCKET, 0x1006 /* SO_RCVTIMEO */,
                         &tv, sizeof(tv));

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (unsigned char)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_control_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    /* ── Step 1: Send ENet CONNECT, wait for VERIFY_CONNECT ─────── */
    pkt_len = build_connect_packet(send_buf, sizeof(send_buf));
    ctrl_log("[CTRL] CONNECT pkt=%d bytes, connectID=%u\n", pkt_len, my_connect_id);

    for (attempt = 0; attempt < 5; attempt++) {
        ret = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                    (struct sockaddr *)&dst, sizeof(dst));
        ctrl_log("[CTRL] CONNECT sent=%d (attempt %d)\n", ret, attempt + 1);

        /* Poll for response with MSG_DONTWAIT + manual 2s timeout.
         * sceNetInetRecv blocks forever on PSP UDP sockets even with
         * SO_RCVTIMEO set; non-blocking poll avoids the deadlock. */
        {
            int poll_loops;
            ret = -1;
            for (poll_loops = 0; poll_loops < 200; poll_loops++) {
                struct sockaddr_in src;
                socklen_t src_len = sizeof(src);
                int r = (int)sceNetInetRecvfrom(ctrl_socket, recv_buf,
                            sizeof(recv_buf), MSG_DONTWAIT,
                            (struct sockaddr *)&src, &src_len);
                if (r > 0) { ret = r; break; }
                sceKernelDelayThread(10000); /* 10ms × 200 = 2s max */
            }
        }
        if (ret > 0) {
            ctrl_log("[CTRL] recv %d bytes\n", ret);
            if (parse_verify_connect(recv_buf, ret, &recv_sent_time) == 0)
                break;
        } else {
            ctrl_log("[CTRL] recv timeout (attempt %d)\n", attempt + 1);
        }
    }

    if (attempt >= 5) {
        ctrl_log("[CTRL] VERIFY_CONNECT timeout after 5 attempts\n");
        sceNetInetClose(ctrl_socket);
        ctrl_socket = -1;
        sceKernelDeleteSema(ctrl_gcm_sem_id);
        ctrl_gcm_sem_id = -1;
        sceKernelDeleteSema(ctrl_seq_sem_id);
        ctrl_seq_sem_id = -1;
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        ctrl_crypto_ready = 0;
        return -2;
    }

    /* ── Step 2: ACK the VERIFY_CONNECT ─────────────────────────── */
    pkt_len = build_ack_packet(send_buf, sizeof(send_buf),
                               server_peer_id, 0xFF, 1, recv_sent_time);
    /* Triple-fire the VERIFY_CONNECT ACK to ensure host enters streaming state */
    for (int r = 0; r < 3; r++) {
        ret = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                    (struct sockaddr *)&dst, sizeof(dst));
    }
    ctrl_log("[CTRL] ACK sent=%d for VERIFY (triple-fire)\n", ret);

    sceKernelDelayThread(50000); /* 50 ms settle */

    /* ── Step 3: Send START_A (type 0x0305, payload 00 00) ──────── */
    {
        unsigned char sa_payload[] = { 0x00, 0x00 };
        /* Reset all per-channel reliable sequences for start of stream */
        {
            int i;
            for (i = 0; i < CTRL_CHANNEL_COUNT; i++)
                reliable_seq_per_ch[i] = 1;
        }
        retx_clear(); /* Reset retransmit buffer after seq reset */
        pkt_len = build_encrypted_control_msg(send_buf, sizeof(send_buf),
                                              CTRL_TYPE_START_A, sa_payload, 2, CTRL_CHANNEL_URGENT);
        if (pkt_len <= 0) {
            ctrl_log("[CTRL] START_A build failed\n");
            sceNetInetClose(ctrl_socket);
            ctrl_socket = -1;
            sceKernelDeleteSema(ctrl_gcm_sem_id);
            ctrl_gcm_sem_id = -1;
            sceKernelDeleteSema(ctrl_seq_sem_id);
            ctrl_seq_sem_id = -1;
            mbedtls_gcm_free(&ctrl_gcm_ctx);
            ctrl_crypto_ready = 0;
            return -3;
        }
        ret = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                    (struct sockaddr *)&dst, sizeof(dst));
        ctrl_log("[CTRL] START_A sent=%d\n", ret);
    }

    sceKernelDelayThread(50000); /* 50 ms */

    /* ── Step 4: Send START_B (type 0x0307, payload 00) ─────────── */
    {
        unsigned char sb_payload[] = { 0x00 };
        pkt_len = build_encrypted_control_msg(send_buf, sizeof(send_buf),
                                              CTRL_TYPE_START_B, sb_payload, 1, 0x00);
        if (pkt_len <= 0) {
            ctrl_log("[CTRL] START_B build failed\n");
            sceNetInetClose(ctrl_socket);
            ctrl_socket = -1;
            sceKernelDeleteSema(ctrl_gcm_sem_id);
            ctrl_gcm_sem_id = -1;
            sceKernelDeleteSema(ctrl_seq_sem_id);
            ctrl_seq_sem_id = -1;
            mbedtls_gcm_free(&ctrl_gcm_ctx);
            ctrl_crypto_ready = 0;
            return -4;
        }
        ret = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                    (struct sockaddr *)&dst, sizeof(dst));
        ctrl_log("[CTRL] START_B sent=%d\n", ret);
    }

    /* ── Step 5: Start control receive + periodic ping threads ───── */
    ctrl_running = 1;

    ctrl_recv_thread_id = sceKernelCreateThread(
        "ctrl_recv", ctrl_recv_thread,
        0x18, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (ctrl_recv_thread_id >= 0) {
        ret = sceKernelStartThread(ctrl_recv_thread_id, 0, NULL);
        if (ret < 0) {
            ctrl_log("[CTRL] recv thread start failed: %d\n", ret);
            sceKernelDeleteThread(ctrl_recv_thread_id);
            ctrl_recv_thread_id = -1;
            ctrl_running = 0;
            sceNetInetClose(ctrl_socket);
            ctrl_socket = -1;
            sceKernelDeleteSema(ctrl_gcm_sem_id);
            ctrl_gcm_sem_id = -1;
            sceKernelDeleteSema(ctrl_seq_sem_id);
            ctrl_seq_sem_id = -1;
            mbedtls_gcm_free(&ctrl_gcm_ctx);
            ctrl_crypto_ready = 0;
            return -5;
        }
    } else {
        ctrl_log("[CTRL] recv thread create failed: %d\n", (int)ctrl_recv_thread_id);
        ctrl_running = 0;
        sceNetInetClose(ctrl_socket);
        ctrl_socket = -1;
        sceKernelDeleteSema(ctrl_gcm_sem_id);
        ctrl_gcm_sem_id = -1;
        sceKernelDeleteSema(ctrl_seq_sem_id);
        ctrl_seq_sem_id = -1;
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        ctrl_crypto_ready = 0;
        return -5;
    }

    ctrl_thread_id = sceKernelCreateThread(
        "ctrl_ping", ctrl_ping_thread,
        0x18, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (ctrl_thread_id >= 0) {
        ret = sceKernelStartThread(ctrl_thread_id, 0, NULL);
        if (ret < 0) {
            ctrl_log("[CTRL] ping thread start failed: %d\n", ret);
            sceKernelDeleteThread(ctrl_thread_id);
            ctrl_thread_id = -1;
            ctrl_running = 0;
            if (ctrl_socket >= 0) {
                sceNetInetClose(ctrl_socket);
                ctrl_socket = -1;
            }
            if (ctrl_recv_thread_id >= 0) {
                sceKernelWaitThreadEnd(ctrl_recv_thread_id, NULL);
                sceKernelDeleteThread(ctrl_recv_thread_id);
                ctrl_recv_thread_id = -1;
            }
            sceKernelDeleteSema(ctrl_gcm_sem_id);
            ctrl_gcm_sem_id = -1;
            sceKernelDeleteSema(ctrl_seq_sem_id);
            ctrl_seq_sem_id = -1;
            mbedtls_gcm_free(&ctrl_gcm_ctx);
            ctrl_crypto_ready = 0;
            return -5;
        }
        ctrl_log("[CTRL] ping thread started id=%d OK\n", ctrl_thread_id);
    } else {
        ctrl_log("[CTRL] ping thread create failed: %d\n", (int)ctrl_thread_id);
        ctrl_running = 0;
        if (ctrl_socket >= 0) {
            sceNetInetClose(ctrl_socket);
            ctrl_socket = -1;
        }
        if (ctrl_recv_thread_id >= 0) {
            sceKernelWaitThreadEnd(ctrl_recv_thread_id, NULL);
            sceKernelDeleteThread(ctrl_recv_thread_id);
            ctrl_recv_thread_id = -1;
        }
        sceKernelDeleteSema(ctrl_gcm_sem_id);
        ctrl_gcm_sem_id = -1;
        sceKernelDeleteSema(ctrl_seq_sem_id);
        ctrl_seq_sem_id = -1;
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        ctrl_crypto_ready = 0;
        return -5;
    }

    /* ── Step 5b: Request initial IDR frame ────────────────────── */
    /* Moved AFTER recv+ping threads are running so the full network
     * path is ready when Sunshine responds with the IDR frame. */
    control_stream_request_idr();

    ctrl_log("[CTRL] handshake complete\n");
    return 0;
}

/* ══════════════════════════════════════════════════════════════════
 * control_stream_abort - Close socket to unblock recv threads for exit.
 * ══════════════════════════════════════════════════════════════════ */
void control_stream_abort(void)
{
    ctrl_running = 0;
    if (ctrl_socket >= 0) {
        sceNetInetClose(ctrl_socket);
        ctrl_socket = -1;
    }
}

/* ══════════════════════════════════════════════════════════════════
 * Connection Quality Monitoring
 * ══════════════════════════════════════════════════════════════════ */

ConnQualityState control_stream_get_quality(void)
{
    return s_conn_quality;
}

/* Called from the ping thread every 5 seconds to recompute quality.
 * Uses FEC counters, frame rate, and bandwidth estimate. */
static void update_connection_quality(u32 estimated_bw_bps)
{
    extern volatile u32 g_fec_packets_recovered;
    extern volatile u32 g_fec_packets_failed;
    extern volatile u32 g_fec_frames_dropped;
    extern volatile u32 g_fec_recovery_attempts;

    u32 now_us = sceKernelGetSystemTimeLow();
    u32 dt_us = (s_quality_prev_time_us != 0) ? (now_us - s_quality_prev_time_us) : 5000000;
    if (dt_us == 0) dt_us = 1;

    /* Delta FEC stats since last update */
    u32 d_recovered = g_fec_packets_recovered - s_quality_prev_fec_recovered;
    u32 d_failed    = g_fec_packets_failed    - s_quality_prev_fec_failed;
    u32 d_dropped   = g_fec_frames_dropped    - s_quality_prev_fec_dropped;
    u32 d_attempts  = g_fec_recovery_attempts - s_quality_prev_fec_attempts;

    /* Loss rate: failed / (recovered + failed), scaled x10 for 0.1% precision */
    u32 total_fec_pkts = d_recovered + d_failed;
    u32 loss_rate_x10 = 0;
    if (total_fec_pkts > 0) {
        loss_rate_x10 = (d_failed * 1000) / total_fec_pkts;
    }

    /* FEC recovery success rate */
    u32 recovery_pct = 100;
    if (d_attempts > 0) {
        u32 d_success = (d_attempts > d_dropped) ? (d_attempts - d_dropped) : 0;
        recovery_pct = (d_success * 100) / d_attempts;
    }

    /* Frame rate: delta frames / delta time */
    u32 d_frames = g_last_good_frame - s_quality_prev_lgf;
    u32 fps = (d_frames * 1000000) / dt_us;

    /* Classify quality */
    ConnQuality q;
    if (loss_rate_x10 < 10 && d_dropped == 0) {
        q = CONN_QUALITY_EXCELLENT;  /* <1.0% loss */
    } else if (loss_rate_x10 < 30 && d_dropped <= 1) {
        q = CONN_QUALITY_GOOD;       /* <3.0% loss */
    } else if (loss_rate_x10 < 80 && d_dropped <= 3) {
        q = CONN_QUALITY_FAIR;       /* <8.0% loss */
    } else if (loss_rate_x10 < 150) {
        q = CONN_QUALITY_POOR;       /* <15% loss */
    } else {
        q = CONN_QUALITY_CRITICAL;   /* >15% loss */
    }

    /* Phase 5.9: Quality transition hysteresis — require 3 consecutive
     * readings at a new level before committing the transition.
     * Prevents oscillation between states on borderline conditions. */
    {
        static ConnQuality s_pending_quality = CONN_QUALITY_FAIR;
        static int s_consecutive_at_pending = 0;
        static ConnQuality s_committed_quality = CONN_QUALITY_FAIR;
        static const char * const q_names[] = { "EXCELLENT", "GOOD", "FAIR", "POOR", "CRITICAL" };

        if (q == s_pending_quality) {
            s_consecutive_at_pending++;
        } else {
            s_pending_quality = q;
            s_consecutive_at_pending = 1;
        }

        if (s_consecutive_at_pending >= 3 && q != s_committed_quality) {
            ctrl_log("[PHASE5-QUALITY] transition: %s -> %s (consecutive=%d)\n",
                     q_names[s_committed_quality], q_names[q], s_consecutive_at_pending);

            /* Phase 5.9: Scale BW reports on quality transitions.
             * POOR/CRITICAL → halve reported BW to make server reduce encoding.
             * EXCELLENT/GOOD → restore to normal. */
            if (q >= CONN_QUALITY_POOR)
                s_quality_bw_scale_pct = 50;
            else
                s_quality_bw_scale_pct = 100;

            s_committed_quality = q;
        }

        q = s_committed_quality;
    }

    /* Log raw quality state transitions (pre-hysteresis for diagnostics) */
    {
        static ConnQuality s_prev_q = CONN_QUALITY_FAIR;
        static const char * const q_names[] = { "EXCELLENT", "GOOD", "FAIR", "POOR", "CRITICAL" };
        if (q != s_prev_q) {
            ctrl_log("[QUALITY] %s -> %s (loss=%u.%u%% fec_ok=%u%% fps=%u bw=%ukbps)\n",
                     q_names[s_prev_q], q_names[q],
                     loss_rate_x10 / 10, loss_rate_x10 % 10,
                     recovery_pct, fps, estimated_bw_bps / 125);
            s_prev_q = q;
        }
    }

    /* Periodic quality stats every 30s (6 updates at 5s interval) */
    {
        static u32 s_quality_log_count = 0;
        s_quality_log_count++;
        if (s_quality_log_count % 6 == 0) {
            ctrl_log("[QUALITY] q=%d loss=%u.%u%% fec=%u%% fps=%u bw=%ukbps d_rec=%u d_fail=%u d_drop=%u\n",
                     (int)q, loss_rate_x10 / 10, loss_rate_x10 % 10,
                     recovery_pct, fps, estimated_bw_bps / 125,
                     d_recovered, d_failed, d_dropped);
        }
    }

    /* Update snapshot */
    s_conn_quality.quality          = q;
    s_conn_quality.loss_rate_pct    = loss_rate_x10;
    s_conn_quality.fec_recovery_pct = recovery_pct;
    s_conn_quality.frames_per_sec   = fps;
    s_conn_quality.bw_estimate_bps  = estimated_bw_bps;

    /* Save previous counters */
    s_quality_prev_fec_recovered = g_fec_packets_recovered;
    s_quality_prev_fec_failed    = g_fec_packets_failed;
    s_quality_prev_fec_dropped   = g_fec_frames_dropped;
    s_quality_prev_fec_attempts  = g_fec_recovery_attempts;
    s_quality_prev_lgf           = g_last_good_frame;
    s_quality_prev_time_us       = now_us;
}

/* ══════════════════════════════════════════════════════════════════
 * control_stream_stop
 * ══════════════════════════════════════════════════════════════════ */
void control_stream_stop(void)
{
    SceUInt timeout = 2000000; /* 2 seconds */

    ctrl_running = 0;

    /* ── Graceful ENet DISCONNECT with linger ──────────────────────
     * Send a proper ENet DISCONNECT command before closing the socket.
     * This tells Sunshine we're leaving intentionally (vs. a crash/timeout),
     * allowing it to release encoder resources immediately instead of
     * waiting for the 10-second ENet timeout.
     * Best-effort: if the send fails, we still close the socket. */
    if (ctrl_socket >= 0) {
        unsigned char disc_pkt[16];
        unsigned char *p = disc_pkt;
        /* ENet protocol header (4 bytes) */
        p = put_be16(p, (server_peer_id & 0x0FFF) |
                        ((unsigned short)session_bits << 12));
        p = put_be16(p, 0); /* sentTime = 0 */
        /* ENet DISCONNECT command (8 bytes) */
        *p++ = ENET_CMD_DISCONNECT | ENET_CMD_FLAG_ACK; /* commandType */
        *p++ = 0xFF; /* channelID */
        p = put_be16(p, 0); /* reliableSeq = 0 for disconnect */
        p = put_be32(p, 0); /* data = 0 */

        sceNetInetSendto(ctrl_socket, disc_pkt, (int)(p - disc_pkt), 0,
                         (struct sockaddr *)&g_server_addr,
                         sizeof(g_server_addr));

        /* Brief linger: give the DISCONNECT packet time to reach Sunshine.
         * 50ms is enough for a single UDP packet on 802.11b even at
         * worst-case 1Mbps (~0.1ms per 1500B packet). */
        sceKernelDelayThread(50000);

        ctrl_log("[CTRL] sent graceful ENet DISCONNECT\n");
        sceNetInetClose(ctrl_socket);
        ctrl_socket = -1;
    }

    if (ctrl_recv_thread_id >= 0) {
        sceKernelWaitThreadEnd(ctrl_recv_thread_id, &timeout);
        sceKernelDeleteThread(ctrl_recv_thread_id);
        ctrl_recv_thread_id = -1;
    }
    if (ctrl_thread_id >= 0) {
        timeout = 2000000;
        sceKernelWaitThreadEnd(ctrl_thread_id, &timeout);
        sceKernelDeleteThread(ctrl_thread_id);
        ctrl_thread_id = -1;
    }

    if (ctrl_crypto_ready) {
        mbedtls_gcm_free(&ctrl_gcm_ctx);
        ctrl_crypto_ready = 0;
    }
    if (ctrl_gcm_sem_id >= 0) {
        sceKernelDeleteSema(ctrl_gcm_sem_id);
        ctrl_gcm_sem_id = -1;
    }
    if (ctrl_seq_sem_id >= 0) {
        sceKernelDeleteSema(ctrl_seq_sem_id);
        ctrl_seq_sem_id = -1;
    }
}
