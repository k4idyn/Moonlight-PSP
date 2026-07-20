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
#include "rtp_reassembly.h"
#include "signal_strength.h"
#include "sw_decode_pipeline.h"

/* ── Globals from network_connect.c ──────────────────────────────── */
extern char          g_video_server_ip[64];
extern int           g_control_server_port;
extern unsigned int  g_control_connect_data;
extern unsigned char g_remote_input_key[16];
extern int           g_remote_input_key_valid;

/* me_running flag from main.c */
extern volatile int me_running;

/* Experimental recovery mode: only suppress soft IDR requests when we have
 * proven host-side intra-refresh is actually repainting the stream.  Baseline
 * PSP validation keeps this disabled so watchdog recovery can force IDR. */
volatile int g_intra_refresh_active = 0;

/* ── Connection Quality Monitoring state ─────────────────────────── */
static ConnQualityState s_conn_quality = { CONN_QUALITY_FAIR, 0, 0, 0, 0, 0 };
static u32 s_quality_prev_fec_recovered = 0;
static u32 s_quality_prev_fec_failed    = 0;
static u32 s_quality_prev_fec_dropped   = 0;
static u32 s_quality_prev_fec_attempts  = 0;
static u32 s_quality_prev_decoded       = 0;
static u32 s_quality_prev_time_us       = 0;

/* Phase 5.9: Quality-based BW report scaling (100=normal, 50=halve) */
#define IDR_FAST_RECOVERY_MIN_US 500000U
static int s_quality_bw_scale_pct = 100;
static u64 s_last_idr_tick = 0;
static int s_idr_count = 0;
static u32 s_idr_backoff_us = 500000;
static int s_idr_prev_decoded = 0;
static unsigned int s_intra_idr_suppressed = 0;
static volatile int s_server_hello_seen = 0;

/* Forward declaration (defined after control_stream_abort) */
static void update_connection_quality(u32 estimated_bw_bps);

#define ctrl_log(fmt, ...) diag_log_write("CTRL", fmt, ##__VA_ARGS__)

void control_stream_reset_idr_backoff(void)
{
    s_last_idr_tick = 0;
    s_idr_count = 0;
    s_idr_backoff_us = 500000;
    s_idr_prev_decoded = 0;
    s_intra_idr_suppressed = 0;
    ctrl_log("[IDR BACKOFF] reset\n");
}

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

/* Declared early for helpers that transmit before the state section. */
static int ctrl_socket = -1;

/* Shared UDP send helper with rate-limited diagnostics for lossy WiFi.
 * Logs first few failures quickly, then periodically, and logs recovery. */
static int ctrl_sendto_with_diag(const void *buf,
                                 int len,
                                 const struct sockaddr_in *dst,
                                 const char *tag,
                                 unsigned int *fail_count)
{
    int tx = (int)sceNetInetSendto(ctrl_socket, buf, len, MSG_DONTWAIT,
                                   (const struct sockaddr *)dst, sizeof(*dst));

    if (!fail_count) {
        return tx;
    }

    if (tx < 0) {
        unsigned int fails = ++(*fail_count);
        if (fails <= 3 || (fails % 100) == 0) {
            ctrl_log("[CTRL TX] %s send failed tx=%d errno=%d fails=%u\n",
                     tag ? tag : "unknown", tx, sceNetInetGetErrno(), fails);
        }
    } else if (*fail_count) {
        if (*fail_count >= 3) {
            ctrl_log("[CTRL TX] %s recovered after %u failures\n",
                     tag ? tag : "unknown", *fail_count);
        }
        *fail_count = 0;
    }

    return tx;
}

/* ── State ───────────────────────────────────────────────────────── */
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
static unsigned int   s_ping_tx_failures = 0;

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
 * Track received media bytes and report throughput as an ENet peer bandwidth
 * hint. This is measured separately from the RTSP bitrate budget because
 * Sunshine may not immediately map ENet bandwidth commands to RTP video. */
static u32 s_bw_last_bytes = 0;
static u32 s_bw_last_time_us = 0;
static u32 s_estimated_bw_bps = 0;  /* estimated incoming bandwidth (bytes/sec) */

#define BW_REPORT_INTERVAL_TICKS 10  /* 10 x 100 ms = 1 second */
#define BW_REPORT_EWMA_OLD_WEIGHT 3
#define BW_REPORT_EWMA_NEW_WEIGHT 1

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
            u32 rtt = now - e->send_time_us;
            e->acked = 1;
            /* Karn's algorithm: only measure RTT from non-retransmitted
             * packets — ACK for a retransmit is ambiguous (could be for
             * the original or the retransmit). */
            if (e->retries == 0) {
                if (rtt > 0 && rtt < 5000000) /* sanity: < 5s */
                    rtt_update(rtt);
            }
            if (channel == 0xFF) {
                ctrl_log("[CTRL BW] ack seq=%u rtt=%uus retries=%u\n",
                         (unsigned)seq, (unsigned)rtt, (unsigned)e->retries);
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
        {
            int tx = (int)sceNetInetSendto(sock, e->data, e->len, MSG_DONTWAIT,
                                           (const struct sockaddr *)dst, sizeof(*dst));
            if (tx < 0) {
#ifndef RETAIL_BUILD
                int err = sceNetInetGetErrno();
                if (e->retries < 3 || (e->retries % 8) == 0) {
                    ctrl_log("[CTRL RETX] send failed seq=%u ch=%u try=%u errno=%d\n",
                             (unsigned)e->seq, (unsigned)e->channel,
                             (unsigned)e->retries + 1, err);
                }
#endif
            } else {
                retransmitted++;
            }
        }
        e->send_time_us = now;
        e->retries++;
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
static int build_ack_header(unsigned char *buf, int buflen)
{
    unsigned char *p = buf;
    if (buflen < 4) return -1;

    /* Protocol header: PeerID + SessionID + SENT_TIME flag.
     * Reverting +1 logic: Sunshine expects the raw session_bits from VERIFY. */
    p = put_be16(p, (server_peer_id & 0x0FFF) |
                     ((unsigned short)(session_bits & 0x03) << 12) |
                     ENET_FLAG_SENT_TIME);
    p = put_be16(p, 0);

    return (int)(p - buf);
}

static int append_ack_command(unsigned char *buf, int buflen, int offset,
                              unsigned char channel_id,
                              unsigned short rel_seq,
                              unsigned short recv_time)
{
    unsigned char *p;
    if (offset < 4 || buflen - offset < 8) return -1;
    p = buf + offset;

    /* ACK command header (4 bytes: cmd, channel, sequence) */
    *p++ = ENET_CMD_ACK;   /* 1 */
    *p++ = channel_id;
    p = put_be16(p, 0);    /* ACK itself has sequence 0 */

    /* ACK payload (4 bytes: receivedReliableSeq, receivedSentTime) */
    p = put_be16(p, rel_seq);
    p = put_be16(p, recv_time);

    return (int)(p - buf);
}

static int build_ack_packet(unsigned char *buf, int buflen,
                            unsigned short peer_id_word,
                            unsigned char channel_id,
                            unsigned short rel_seq,
                            unsigned short recv_time)
{
    int ack_len;
    (void)peer_id_word;

    ack_len = build_ack_header(buf, buflen);
    if (ack_len < 0) return -1;
    return append_ack_command(buf, buflen, ack_len,
                              channel_id, rel_seq, recv_time);
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

        /* Nonblocking recv avoids holding the PSP socket lock while ctrl_ping
         * and recovery paths send on the same UDP socket. */
        ret = (int)sceNetInetRecvfrom(ctrl_socket,
                                          recv_buf,
                                          sizeof(recv_buf),
                                          MSG_DONTWAIT,
                                          (struct sockaddr *)&src,
                                          &src_len);

        if (ret <= 0 || ret < 4) {
            sceKernelDelayThread(2000);
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
                        int ack_tx = (int)sceNetInetSendto(ctrl_socket, ack_buf, ack_len, MSG_DONTWAIT,
                                         (struct sockaddr *)&server_addr, sizeof(server_addr));
                        /* Log ACK failures and DISCONNECT ACKs only */
                        if (ack_tx < 0) {
                            ctrl_log("[CTRL RX] ACK sent=%d for cmd=0x%02X ch=%u seq=%u errno=%d\n",
                                     ack_tx, cmd, channel_id, (unsigned)rel_seq,
                                     sceNetInetGetErrno());
                        } else if (cmd_num == ENET_CMD_DISCONNECT) {
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
#ifndef RETAIL_BUILD
                    {
                        unsigned int disc_data = (p + 8 <= end) ?
                            (((unsigned int)p[4] << 24) | ((unsigned int)p[5] << 16) |
                             ((unsigned int)p[6] << 8)  | (unsigned int)p[7]) : 0;
                        ctrl_log("[CTRL RX] DISCONNECT received from server! ch=%u seq=%u data=0x%08X\n",
                                 channel_id, (unsigned)rel_seq, disc_data);
                        ctrl_log("[CTRL RX] Raw disconnect bytes: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                                 p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
                    }
#endif
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
                                                ctrl_socket, reply_buf, reply_len, MSG_DONTWAIT,
                                                (const struct sockaddr *)&server_addr,
                                                sizeof(server_addr));
                                    /* Store in retransmit buffer for gap recovery */
                                    if (tx > 0) {
                                        unsigned char rch = 0;
                                        unsigned short rseq = 0;
                                        pkt_get_channel_seq(reply_buf, reply_len, &rch, &rseq);
                                        retx_store(reply_buf, reply_len, rch, rseq);
                                    }
                                    if (tx < 0) {
                                        ctrl_log("[CTRL RX] 0x010E server-hello: echoed %d bytes tx=%d errno=%d\n",
                                                 echo_len, tx, sceNetInetGetErrno());
                                    } else {
                                        ctrl_log("[CTRL RX] 0x010E server-hello: echoed %d bytes tx=%d\n",
                                                 echo_len, tx);
                                    }
                                    if (!s_server_hello_seen) {
                                        s_server_hello_seen = 1;
                                        if (!g_idr_fully_decoded && g_last_good_frame == 0) {
                                            ctrl_log("[CTRL RX] server-hello ready; forcing startup IDR\n");
                                            control_stream_reset_idr_backoff();
                                            control_stream_request_idr_startup();
                                        }
                                    }
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
            tx = ctrl_sendto_with_diag(pkt, pkt_len, &dst,
                                       "ctrl_ping", &s_ping_tx_failures);
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

        /* Bandwidth estimation: sample the real one-second media budget.
         * Hardware runs proved adaptive ENet BANDWIDTH_LIMIT hints can starve
         * Sunshine's RTP media path when the reported value falls below the
         * video+audio+FEC floor. Keep this path as telemetry only; RTSP and
         * Sunshine stream settings own encoder bitrate. */
        if (count > 0 && (count % BW_REPORT_INTERVAL_TICKS) == 0 && ctrl_socket >= 0) {
            extern volatile u32 g_fec_total_bytes_received;
            u32 now_bw = sceKernelGetSystemTimeLow();
            u32 bytes_now = g_fec_total_bytes_received;

            if (s_bw_last_time_us != 0) {
                u32 dt_us = now_bw - s_bw_last_time_us;
                u32 dbytes = bytes_now - s_bw_last_bytes;
                if (dt_us > 100000) { /* at least 100ms elapsed */
                    u32 instant_bw_bps = (u32)((u64)dbytes * 1000000ULL / (u64)dt_us);
                    if (s_estimated_bw_bps == 0) {
                        s_estimated_bw_bps = instant_bw_bps;
                    } else {
                        s_estimated_bw_bps =
                            (s_estimated_bw_bps * BW_REPORT_EWMA_OLD_WEIGHT +
                             instant_bw_bps * BW_REPORT_EWMA_NEW_WEIGHT) /
                            (BW_REPORT_EWMA_OLD_WEIGHT + BW_REPORT_EWMA_NEW_WEIGHT);
                    }

                    /* Compute adaptive telemetry only. The PSP must not send
                     * ENet BANDWIDTH_LIMIT here; the host already advertises
                     * unlimited peer bandwidth and encoder bitrate belongs to
                     * the RTSP/Sunshine stream settings path. */
                    {
                        u32 reported_bw = s_estimated_bw_bps;
                        int sig_br_kbps = signal_strength_get_bitrate();
                        int floor_kbps = ADAPT_STREAM_FLOOR_KBPS;
                        u32 min_reported_bw = (u32)ADAPT_STREAM_FLOOR_KBPS * 125U;
#define BW_REPORT_SCALE_PCT 100  /* report the real PSP transport budget */
                        reported_bw = (u32)((u64)reported_bw * BW_REPORT_SCALE_PCT / 100);
                        /* Phase 5.9: Apply quality-based BW scaling */
                        reported_bw = (u32)((u64)reported_bw * (u32)s_quality_bw_scale_pct / 100);
                        /* Cap by adaptive signal-strength controller target. */
                        if (sig_br_kbps > 0) {
                            u32 sig_cap_bps = (u32)sig_br_kbps * 125U;
                            floor_kbps = signal_strength_get_adaptive_floor_kbps(sig_br_kbps);
                            min_reported_bw = (u32)floor_kbps * 125U;
                            if (reported_bw > sig_cap_bps) {
                                reported_bw = sig_cap_bps;
                            }
                        }

                        /* Avoid collapse-to-zero feedback loops during stalls.
                         * If we report near-zero BW, Sunshine can keep sending
                         * tiny bursts forever and never recover decoder cadence.
                         * Hold a small PSP recovery floor, not the full adaptive
                         * cap, so loss recovery can actually reduce encoder work. */
                        if (reported_bw < min_reported_bw) {
                            reported_bw = min_reported_bw;
                        }

                        if (count <= 10 || (count % 50) == 0 || reported_bw < s_estimated_bw_bps) {
                            ctrl_log("[CTRL BW] inst=%ukbps ewma=%ukbps telemetry=%ukbps (x%d%% qscale=%d%% sig_cap=%dkbps floor=%dkbps) rto=%uus enet_hint=off\n",
                                     instant_bw_bps * 8 / 1000,
                                     s_estimated_bw_bps * 8 / 1000,
                                     reported_bw * 8 / 1000,
                                     BW_REPORT_SCALE_PCT,
                                     s_quality_bw_scale_pct,
                                     sig_br_kbps,
                                     floor_kbps,
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

        if (!g_idr_fully_decoded) {
            if (g_last_good_frame == 0 && (count % 5) == 0) {
                if ((count % 10) == 0) {
                    ctrl_log("[CTRL PING] startup IDR assist fast (lgf=0)\n");
                }
                control_stream_request_idr_startup();
            } else if (count % 50 == 0) { /* 50 x 100ms = 5 seconds */
                if (g_last_good_frame < 120) {
                    ctrl_log("[CTRL PING] startup IDR assist (lgf=%u)\n", g_last_good_frame);
                    control_stream_request_idr();
                }
            }
        }

        /* Periodic IDR refresh disabled for v1.0 stability.
         * Recovery is driven by explicit stall/drop signals only. */

        /* Stall recovery: if no new frames for a prolonged period, request IDR.
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
                if (stall_ticks == 80) { /* 80 × 100ms = 8 seconds */
                    if (s_conn_quality.frames_per_sec <= 8) {
                        ctrl_log("[CTRL PING] STALL lgf=%u for 8s, requesting IDR\n",
                                 g_last_good_frame);
                        g_idr_fully_decoded = 0;
                        control_stream_request_idr();
                    }
                }
                if (stall_ticks == 160) { /* 160 × 100ms = 16 seconds */
                    if (s_conn_quality.frames_per_sec <= 4) {
                        ctrl_log("[CTRL PING] STALL lgf=%u for 16s, requesting IDR (urgent)\n",
                                 g_last_good_frame);
                        g_idr_fully_decoded = 0;
                        control_stream_request_idr();
                    }
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
    unsigned char payload[23];  /* SS_FRAME_FEC_STATUS is 21 bytes, all BE */
    int pkt_len;
    static unsigned int s_fec_tx_failures = 0;

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

    return ctrl_sendto_with_diag(pkt, pkt_len, &g_server_addr,
                                 "fec_status", &s_fec_tx_failures);
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
    static unsigned int s_input_tx_failures = 0;

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

    ret = ctrl_sendto_with_diag(pkt, pkt_len, &dst,
                                "input", &s_input_tx_failures);
    return ret;
}

/* ══════════════════════════════════════════════════════════════════
 * Request IDR Frame
 * ══════════════════════════════════════════════════════════════════ */
int control_stream_request_idr(void)
{
    int startup_wait = (!g_idr_fully_decoded && g_last_good_frame == 0);
    int strict_idr_wait = rtp_reassembly_waiting_for_idr();

    /* Only suppress soft IDRs after host intra-refresh has been proven in
     * runtime evidence.  False positives leave PSP stuck on a frozen frame
     * because watchdog recovery cannot trigger a fresh reference. */
    if (g_intra_refresh_active && !startup_wait && !strict_idr_wait) {
        if (s_intra_idr_suppressed < 5 ||
            (s_intra_idr_suppressed % 60U) == 0) {
            ctrl_log("[CTRL] IDR request suppressed; intra-refresh active (count=%u)\n",
                     s_intra_idr_suppressed + 1U);
        }
        s_intra_idr_suppressed++;
        return 0;
    }

    /* Outside proven intra-refresh mode, do not suppress IDR requests just because
     * g_idr_fully_decoded is set.  The old guard ate all post-startup IDR
     * requests and made watchdog recovery impossible. */

    extern void rtp_reassembly_flush_partial_frame(void);

    unsigned char send_buf[128];
    /* IDR request payload: two zero bytes (matches Moonlight reference clients) */
    unsigned char idr_payload[] = { 0x00, 0x00 };
    int pkt_len;
    int ret_urgent = -1;
    static unsigned int s_idr_tx_failures = 0;

    if (ctrl_socket < 0 || !ctrl_running) {
        return -1;
    }

    /* Rate-limit IDR requests with exponential backoff.
     * Starts at 500ms, doubles on consecutive requests up to 2s max.
     * On successful IDR decode, gently relaxes (halves) instead of a hard
     * reset to avoid bursty request loops on borderline WiFi.
     * Only the first request bypasses throttle; later callers coalesce while
     * a keyframe request is already in flight. */
    {
        u64 now;
        sceRtcGetCurrentTick(&now);

        /* Relax backoff when an IDR decode succeeds */
        if (g_idr_fully_decoded && !s_idr_prev_decoded) {
#ifndef RETAIL_BUILD
            u32 prev_backoff = s_idr_backoff_us;
#endif
            if (s_idr_backoff_us > 500000)
                s_idr_backoff_us /= 2;
            if (s_idr_backoff_us < 500000)
                s_idr_backoff_us = 500000;
#ifndef RETAIL_BUILD
            ctrl_log("[IDR BACKOFF] relax %ums -> %ums (IDR decoded ok, count=%d)\n",
                     prev_backoff / 1000, s_idr_backoff_us / 1000, s_idr_count);
#endif
        }
        s_idr_prev_decoded = g_idr_fully_decoded;

        if (startup_wait && s_idr_backoff_us > 500000) {
            s_idr_backoff_us = 500000;
        }

        if (s_idr_count >= 1 && s_last_idr_tick != 0 && (now - s_last_idr_tick) < s_idr_backoff_us) {
            ctrl_log("[IDR BACKOFF] throttled (backoff=%ums count=%d)\n",
                     s_idr_backoff_us / 1000, s_idr_count);
            return 0; /* throttled — recent IDR already in flight */
        }
        s_last_idr_tick = now;
        s_idr_count++;

        /* Normal playback uses exponential backoff. Startup holds a fixed
         * 500 ms cadence because no incoming P-frame is usable before the
         * first SPS/IDR sync point.
         */
        if (!startup_wait && s_idr_count >= 2) {
#ifndef RETAIL_BUILD
            u32 prev_backoff = s_idr_backoff_us;
#endif
            s_idr_backoff_us *= 2;
            if (s_idr_backoff_us > 2000000)
                s_idr_backoff_us = 2000000;
#ifndef RETAIL_BUILD
            ctrl_log("[IDR BACKOFF] %ums -> %ums (count=%d)\n",
                     prev_backoff / 1000, s_idr_backoff_us / 1000, s_idr_count);
#endif
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
    ret_urgent = ctrl_sendto_with_diag(send_buf, pkt_len, &g_server_addr,
                                       "idr_req", &s_idr_tx_failures);
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

int control_stream_request_idr_force(void)
{
    u64 now;
    int startup_wait = (!g_idr_fully_decoded && g_last_good_frame == 0);

    sceRtcGetCurrentTick(&now);
    if (!startup_wait) {
        if (s_last_idr_tick != 0 && (now - s_last_idr_tick) < 2000000) {
            ctrl_log("[IDR BACKOFF] force coalesced post-startup (last=%ums ago count=%d)\n",
                     (u32)((now - s_last_idr_tick) / 1000), s_idr_count);
            return 0;
        }
        if (s_idr_backoff_us < 2000000) {
            s_idr_backoff_us = 2000000;
        }
        return control_stream_request_idr();
    }

    if (s_last_idr_tick != 0 && (now - s_last_idr_tick) < 500000) {
        ctrl_log("[IDR BACKOFF] force coalesced (last=%ums ago count=%d)\n",
                 (u32)((now - s_last_idr_tick) / 1000), s_idr_count);
        return 0;
    }

    /* Hard recovery still sends immediately when no keyframe request is in
     * flight, but it must not emit duplicate same-frame requests. */
    s_idr_count = 0;
    s_idr_backoff_us = 500000;
    return control_stream_request_idr();
}

int control_stream_request_idr_recovery_fast(void)
{
    u64 now;

    sceRtcGetCurrentTick(&now);
    if (s_last_idr_tick != 0 &&
        (now - s_last_idr_tick) < IDR_FAST_RECOVERY_MIN_US) {
        ctrl_log("[IDR BACKOFF] fast recovery coalesced (last=%ums ago count=%d)\n",
                 (u32)((now - s_last_idr_tick) / 1000), s_idr_count);
        return 0;
    }

    s_idr_count = 0;
    s_idr_backoff_us = IDR_FAST_RECOVERY_MIN_US;
    ctrl_log("[IDR BACKOFF] fast recovery request cadence=%ums\n",
             IDR_FAST_RECOVERY_MIN_US / 1000);
    return control_stream_request_idr();
}

int control_stream_request_idr_startup(void)
{
    if (g_idr_fully_decoded || g_last_good_frame != 0) {
        return 0;
    }

    return control_stream_request_idr_force();
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
    static unsigned int s_rfi_tx_failures = 0;

    if (ctrl_socket < 0 || !ctrl_running) {
        return -1;
    }

    /* Rate-limit RFI aggressively on PSP-1000 WiFi.  Do not discard the
     * skipped range: merge it and let the ping thread flush one bounded
     * RFI request after the interval, matching common-c's queue/merge shape
     * without adding another PSP worker thread. */
    /* Rate-limit RFI aggressively on PSP-1000 WiFi.  Dropped P-frames are
     * already concealed locally; RFI is useful, but frequent urgent sends
     * can create recovery bursts that cost more packets than they save. */
    {
        static u64 s_last_rfi_tick = 0;
        u64 now;
        sceRtcGetCurrentTick(&now);
        if (s_last_rfi_tick != 0 && (now - s_last_rfi_tick) < 500000) {
            return 0;
        }
        s_last_rfi_tick = now;
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

    ret_urgent = ctrl_sendto_with_diag(send_buf, pkt_len, &g_server_addr,
                                       "rfi_req", &s_rfi_tx_failures);
    if (ret_urgent > 0) {
        unsigned char rch = 0;
        unsigned short rseq = 0;
        pkt_get_channel_seq(send_buf, pkt_len, &rch, &rseq);
        retx_store(send_buf, pkt_len, rch, rseq);
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
    static struct sockaddr_in recv_src;
    static int have_recv_src;

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
    s_server_hello_seen = 0;

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
        int bind_ret = sceNetInetBind(ctrl_socket, (struct sockaddr *)&my_bind, sizeof(my_bind));
        if (bind_ret != 0) {
            ctrl_log("[CTRL] bind() to 57999 failed errno=%d\n", sceNetInetGetErrno());
            /* non-fatal, OS will assign an ephemeral port, but IP might be wrong on loopback */
        }
        {
            struct sockaddr_in local_addr;
            socklen_t local_len = sizeof(local_addr);
            memset(&local_addr, 0, sizeof(local_addr));
            if (sceNetInetGetsockname(ctrl_socket, (struct sockaddr *)&local_addr, &local_len) == 0) {
                ctrl_log("[CTRL] local UDP %s:%u advertised=57999 bind_ret=%d\n",
                         inet_ntoa(local_addr.sin_addr),
                         (unsigned)ntohs(local_addr.sin_port),
                         bind_ret);
            } else {
                ctrl_log("[CTRL] getsockname failed after bind errno=%d\n", sceNetInetGetErrno());
            }
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
    if (pkt_len >= 52) {
        ctrl_log("[CTRL] CONNECT pkt=%d bytes, connectID=%u connectData=%u data_be=%02X%02X%02X%02X\n",
                 pkt_len, my_connect_id, g_control_connect_data,
                 send_buf[pkt_len - 4], send_buf[pkt_len - 3],
                 send_buf[pkt_len - 2], send_buf[pkt_len - 1]);
    } else {
        ctrl_log("[CTRL] CONNECT build failed pkt=%d connectData=%u\n",
                 pkt_len, g_control_connect_data);
    }

    for (attempt = 0; attempt < 5; attempt++) {
        ret = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                    (struct sockaddr *)&dst, sizeof(dst));
        if (ret < 0) {
            ctrl_log("[CTRL] CONNECT sent=%d (attempt %d) errno=%d\n",
                     ret, attempt + 1, sceNetInetGetErrno());
        } else {
            ctrl_log("[CTRL] CONNECT sent=%d (attempt %d)\n", ret, attempt + 1);
        }

        /* Poll for response with MSG_DONTWAIT + manual 2s timeout.
         * sceNetInetRecv blocks forever on PSP UDP sockets even with
         * SO_RCVTIMEO set; non-blocking poll avoids the deadlock. */
        {
            int poll_loops;
            ret = -1;
            have_recv_src = 0;
            memset(&recv_src, 0, sizeof(recv_src));
            for (poll_loops = 0; poll_loops < 200; poll_loops++) {
                struct sockaddr_in src;
                socklen_t src_len = sizeof(src);
                int r = (int)sceNetInetRecvfrom(ctrl_socket, recv_buf,
                            sizeof(recv_buf), MSG_DONTWAIT,
                            (struct sockaddr *)&src, &src_len);
                if (r > 0) {
                    ret = r;
                    recv_src = src;
                    have_recv_src = 1;
                    break;
                }
                sceKernelDelayThread(10000); /* 10ms × 200 = 2s max */
            }
        }
        if (ret > 0) {
            if (have_recv_src) {
                ctrl_log("[CTRL] recv %d bytes from %s:%u\n",
                         ret, inet_ntoa(recv_src.sin_addr),
                         (unsigned)ntohs(recv_src.sin_port));
            } else {
                ctrl_log("[CTRL] recv %d bytes\n", ret);
            }
            if (parse_verify_connect(recv_buf, ret, &recv_sent_time) == 0)
                break;
        } else {
            if (attempt < 4) {
                ctrl_log("[CTRL] recv idle (attempt %d)\n", attempt + 1);
            } else {
                ctrl_log("[CTRL] recv timeout (attempt %d)\n", attempt + 1);
            }
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
    if (ret < 0) {
        ctrl_log("[CTRL] ACK sent=%d for VERIFY (triple-fire) errno=%d\n",
                 ret, sceNetInetGetErrno());
    } else {
        ctrl_log("[CTRL] ACK sent=%d for VERIFY (triple-fire)\n", ret);
    }

    sceKernelDelayThread(50000); /* 50 ms settle */

    /* ── Step 3: Send START_A/initial-IDR (type 0x0302, payload 00 00) ─ */
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
                                              CTRL_TYPE_START_A, sa_payload, 2, CTRL_CHANNEL_GENERIC);
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
        if (ret > 0) {
            unsigned char rch = 0;
            unsigned short rseq = 0;
            pkt_get_channel_seq(send_buf, pkt_len, &rch, &rseq);
            retx_store(send_buf, pkt_len, rch, rseq);
        }
        if (ret < 0) {
            ctrl_log("[CTRL] START_A sent=%d errno=%d\n", ret, sceNetInetGetErrno());
        } else {
            ctrl_log("[CTRL] START_A sent=%d ch=0\n", ret);
        }
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
        if (ret > 0) {
            unsigned char rch = 0;
            unsigned short rseq = 0;
            pkt_get_channel_seq(send_buf, pkt_len, &rch, &rseq);
            retx_store(send_buf, pkt_len, rch, rseq);
        }
        if (ret < 0) {
            ctrl_log("[CTRL] START_B sent=%d errno=%d\n", ret, sceNetInetGetErrno());
        } else {
            ctrl_log("[CTRL] START_B sent=%d ch=0\n", ret);
        }
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
    g_intra_refresh_active = 0;
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

/* Called from the ping thread every second to recompute quality.
 * Uses FEC counters, frame rate, and bandwidth estimate. */
static void update_connection_quality(u32 estimated_bw_bps)
{
    extern volatile u32 g_fec_packets_recovered;
    extern volatile u32 g_fec_packets_failed;
    extern volatile u32 g_fec_frames_dropped;
    extern volatile u32 g_fec_recovery_attempts;

    u32 now_us = sceKernelGetSystemTimeLow();
    u32 dt_us = (s_quality_prev_time_us != 0) ? (now_us - s_quality_prev_time_us) : 1000000;
    if (dt_us == 0) dt_us = 1;

    /* Delta FEC stats since last update */
    u32 d_recovered = g_fec_packets_recovered - s_quality_prev_fec_recovered;
    u32 d_failed    = g_fec_packets_failed    - s_quality_prev_fec_failed;
    u32 d_dropped   = g_fec_frames_dropped    - s_quality_prev_fec_dropped;
    u32 d_attempts  = g_fec_recovery_attempts - s_quality_prev_fec_attempts;

    /* Loss rate: keep packet-level FEC loss, then fold in frame-level
     * drops so quality accounting reflects what the user actually sees. */
    u32 total_fec_pkts = d_recovered + d_failed;
    u32 packet_loss_x10 = 0;
    u32 loss_rate_x10 = 0;
    if (total_fec_pkts > 0) {
        packet_loss_x10 = (d_failed * 1000) / total_fec_pkts;
    }

    /* FEC recovery success rate */
    u32 recovery_pct = 100;
    if (d_attempts > 0) {
        u32 d_success = (d_attempts > d_dropped) ? (d_attempts - d_dropped) : 0;
        recovery_pct = (d_success * 100) / d_attempts;
    }

    /* Frame rate is tracked for diagnostics/HUD only.  Do not feed decode-side
     * stalls back into transport quality classification or the network
     * controller will punish clean links for CPU/content bottlenecks. */
    u32 decoded_now = 0;
    u32 dropped_now = 0;
    sw_decoder_get_stats(&decoded_now, &dropped_now);
    u32 d_frames = decoded_now - s_quality_prev_decoded;
    u32 fps = (d_frames * 1000000) / dt_us;
    int transport_idle = 0;
    int startup_idle = 0;
    {
        u32 total_frame_events = d_frames + d_dropped;
        if (total_frame_events > 0) {
            u32 frame_loss_x10 = (d_dropped * 1000) / total_frame_events;
            u32 visible_delivery_pct = (d_frames * 100) / total_frame_events;

            loss_rate_x10 = packet_loss_x10;
            if (total_fec_pkts > 0 || packet_loss_x10 > 0 || d_attempts > 0) {
                if (frame_loss_x10 > loss_rate_x10) {
                    loss_rate_x10 = frame_loss_x10;
                }
                if (visible_delivery_pct < recovery_pct) {
                    recovery_pct = visible_delivery_pct;
                }
            }
        } else {
            loss_rate_x10 = packet_loss_x10;
        }
    }

    /* A no-traffic window after playback starts is not "perfect" just because
     * there were no failed FEC attempts. Before the first decoded frame, keep
     * the baseline cap stable instead of feeding a startup idle window back as
     * packet loss. */
    if (estimated_bw_bps < 4096 &&
        d_frames == 0 &&
        d_dropped == 0 &&
        total_fec_pkts == 0 &&
        d_attempts == 0) {
        if (decoded_now == 0) {
            startup_idle = 1;
            loss_rate_x10 = 0;
            recovery_pct = 100;
        } else {
            transport_idle = 1;
            loss_rate_x10 = 1000;
            recovery_pct = 0;
        }
    }

    /* Classify transport quality from transport signals only.
     * Decoder FPS is reported separately, but it must not drive bitrate
     * collapse when the network path is healthy. */
    ConnQuality q;
    if (startup_idle) {
        q = CONN_QUALITY_FAIR;       /* do not collapse BW before first frame */
    } else if (transport_idle) {
        q = CONN_QUALITY_CRITICAL;   /* no video/audio packets in this window */
    } else if (loss_rate_x10 < 10 && recovery_pct >= 99) {
        q = CONN_QUALITY_EXCELLENT;  /* <1.0% loss, near-perfect recovery */
    } else if (loss_rate_x10 < 30 && recovery_pct >= 97) {
        q = CONN_QUALITY_GOOD;       /* <3.0% loss */
    } else if (loss_rate_x10 < 80 && recovery_pct >= 90) {
        q = CONN_QUALITY_FAIR;       /* <8.0% loss */
    } else if (loss_rate_x10 < 150 && recovery_pct >= 75) {
        q = CONN_QUALITY_POOR;       /* <15% loss */
    } else {
        q = CONN_QUALITY_CRITICAL;   /* >15% loss */
    }

    if (d_dropped > 0 && total_fec_pkts > 0 && q < CONN_QUALITY_POOR) {
        q = CONN_QUALITY_POOR;
    }
    if (d_dropped >= 3 && total_fec_pkts > 0) {
        q = CONN_QUALITY_CRITICAL;
    }

    /* Phase 5.9: Quality transition hysteresis — require 3 consecutive
     * readings at a new level before committing the transition.
     * Prevents oscillation between states on borderline conditions. */
    {
        static ConnQuality s_pending_quality = CONN_QUALITY_FAIR;
        static int s_consecutive_at_pending = 0;
        static ConnQuality s_committed_quality = CONN_QUALITY_FAIR;
#ifndef RETAIL_BUILD
        static const char * const q_names[] = { "EXCELLENT", "GOOD", "FAIR", "POOR", "CRITICAL" };
#endif

        if (q == s_pending_quality) {
            s_consecutive_at_pending++;
        } else {
            s_pending_quality = q;
            s_consecutive_at_pending = 1;
        }

        {
            int required_consecutive = 3;
            if (q > s_committed_quality) {
                if (transport_idle || d_dropped >= 2)
                    required_consecutive = 1;
                else
                    required_consecutive = 2;
            }
            if (s_consecutive_at_pending >= required_consecutive && q != s_committed_quality) {
#ifndef RETAIL_BUILD
            ctrl_log("[QUALITY] committed: %s -> %s (consecutive=%d)\n",
                     q_names[s_committed_quality], q_names[q], s_consecutive_at_pending);
#endif

            /* Phase 5.9: Scale BW reports on quality transitions.
             * POOR/CRITICAL → halve reported BW to make server reduce encoding.
             * EXCELLENT/GOOD → restore to normal. */
            if (q >= CONN_QUALITY_CRITICAL)
                s_quality_bw_scale_pct = 60;
            else if (q >= CONN_QUALITY_POOR)
                s_quality_bw_scale_pct = 75;
            else
                s_quality_bw_scale_pct = 100;

            s_committed_quality = q;
            }
        }

        q = s_committed_quality;
    }

    /* Log raw quality state transitions (pre-hysteresis for diagnostics) */
#ifndef RETAIL_BUILD
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
#endif

    /* Periodic quality stats every 30s (30 updates at 1s interval) */
    {
        static u32 s_quality_log_count = 0;
        s_quality_log_count++;
        if (s_quality_log_count % 30 == 0) {
            ctrl_log("[QUALITY] q=%d loss=%u.%u%% fec=%u%% fps=%u bw=%ukbps d_rec=%u d_fail=%u d_drop=%u idle=%d\n",
                     (int)q, loss_rate_x10 / 10, loss_rate_x10 % 10,
                     recovery_pct, fps, estimated_bw_bps / 125,
                     d_recovered, d_failed, d_dropped, transport_idle);
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
    s_quality_prev_decoded       = decoded_now;
    s_quality_prev_time_us       = now_us;
}

/* ══════════════════════════════════════════════════════════════════
 * control_stream_stop
 * ══════════════════════════════════════════════════════════════════ */
void control_stream_stop(void)
{
    SceUInt timeout = 2000000; /* 2 seconds */

    ctrl_running = 0;
    g_intra_refresh_active = 0;

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

        {
            int disc_tx = (int)sceNetInetSendto(ctrl_socket, disc_pkt, (int)(p - disc_pkt), 0,
                                                (struct sockaddr *)&g_server_addr,
                                                sizeof(g_server_addr));
            if (disc_tx < 0) {
                ctrl_log("[CTRL] graceful ENet DISCONNECT send failed errno=%d\n",
                         sceNetInetGetErrno());
            }
        }

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
