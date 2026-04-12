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
            session_bits   = p[6] & 0x03;       /* incomingSessionID: bits 0-1 */

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

                /* ── ACK (1): 8 bytes — RelSeq (2) + ReceivedSentTime (2) ── */
                if (cmd_num == ENET_CMD_ACK) {
                    if (p + 8 > end) break;
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

                /* ── BANDWIDTH_LIMIT (10): 12 bytes — ACK handled by Universal ── */
                if (cmd_num == ENET_CMD_BANDWIDTH_LIMIT) {
                    if (p + 12 > end) break;
                    p += 12;
                    continue;
                }

                /* ── THROTTLE_CONFIGURE (11): 16 bytes — ACK handled by Universal ── */
                if (cmd_num == ENET_CMD_THROTTLE_CONFIGURE) {
                    if (p + 16 > end) break;
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
            /* Single-fire: The server-hello echo fix (0x010E → server_addr)
             * eliminated the 5-second disconnect that necessitated triple-fire.
             * Triple-fire sent 30 UDP pkts/sec on control alone, causing ENOBUFS
             * after ~30s on PSP's tiny 802.11b socket buffer. Single-fire + the
             * working ENet ACK path gives reliable delivery without flooding. */
            tx = (int)sceNetInetSendto(ctrl_socket, pkt, pkt_len, 0,
                             (struct sockaddr *)&dst, sizeof(dst));
            if (count <= 5 || (count % 50) == 0 || tx < 0)
                ctrl_log("[CTRL PING] #%u pkt=%d tx=%d lgf=%u%s%d\n",
                         count, pkt_len, tx, g_last_good_frame,
                         tx < 0 ? " errno=" : "", tx < 0 ? sceNetInetGetErrno() : 0);
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
         * Only send every 5th ping (1/sec) to reduce ENOBUFS pressure. */
        if (g_last_good_frame > 0 && ctrl_crypto_ready && (count % 5) == 0) {
            control_stream_send_fec_status(
                g_last_good_frame,
                0,      /* highestReceivedSequenceNumber */
                0,      /* nextContiguousSequenceNumber */
                0,      /* missingPacketsBeforeHighestReceived */
                1,      /* totalDataPackets */
                0,      /* totalParityPackets */
                1,      /* receivedDataPackets */
                0,      /* receivedParityPackets */
                0,      /* fecPercentage */
                0,      /* multiFecBlockIndex */
                1       /* multiFecBlockCount */
            );
        }

        if (count % 25 == 0) { /* 25 × 200ms = 5 seconds */
            if (!g_idr_fully_decoded) {
                ctrl_log("[CTRL PING] IDR accumulation incomplete, requesting IDR...\n");
                control_stream_request_idr();
            }
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
                if (stall_ticks == 25) { /* 25 × 200ms = 5 seconds */
                    ctrl_log("[CTRL PING] STALL lgf=%u for 5s, requesting IDR\n",
                             g_last_good_frame);
                    g_idr_fully_decoded = 0;
                    control_stream_request_idr();
                }
                if (stall_ticks == 50) { /* 50 × 200ms = 10 seconds */
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

        sceKernelDelayThread(200000); /* 200 ms — halves outbound rate to avoid ENOBUFS */

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
                                              0x5502, payload, 21, 0x06);
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

    /* Once IDR accumulation is complete (all 510 MBs covered), suppress
     * further IDR requests.  Server-initiated periodic IDRs still arrive
     * but we skip displaying them.  This prevents the vicious cycle of
     * corrupt IDR → request → corrupt IDR that floods the link. */
    if (g_idr_fully_decoded) {
        return 0;
    }

    extern void rtp_reassembly_flush_partial_frame(void);

    unsigned char send_buf[128];
    /* IDR request payload: two zero bytes (matches Moonlight reference clients) */
    unsigned char idr_payload[] = { 0x00, 0x00 };
    int pkt_len;
    int ret_urgent = -1;

    if (ctrl_socket < 0 || !ctrl_running) {
        return -1;
    }

    /* Rate-limit IDR requests to max 2/sec (500ms throttle).
     * Without this, frame drops + FEC failures flood the server with
     * 10+ IDR requests in 19s, wasting bandwidth and confusing Sunshine.
     * First 5 requests bypass throttle for rapid-fire during initial connection. */
    {
        static u64 s_last_idr_tick = 0;
        static int s_idr_count = 0;
        u64 now;
        sceRtcGetCurrentTick(&now);
        if (s_idr_count >= 5 && s_last_idr_tick != 0 && (now - s_last_idr_tick) < 500000) {
            return 0; /* throttled — recent IDR already in flight */
        }
        s_last_idr_tick = now;
        s_idr_count++;
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

    /* Single send — the 500ms rate limiter + ENet ACK provides sufficient
     * reliability. Burst-sending 3x amplified the loss feedback loop (C-1). */
    ret_urgent = (int)sceNetInetSendto(ctrl_socket, send_buf, pkt_len, 0,
                                       (struct sockaddr *)&g_server_addr,
                                       sizeof(g_server_addr));

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

        /* Wait for response */
        ret = (int)sceNetInetRecv(ctrl_socket, recv_buf, sizeof(recv_buf), 0);
        if (ret > 0) {
            ctrl_log("[CTRL] recv %d bytes\n", ret);
            if (parse_verify_connect(recv_buf, ret, &recv_sent_time) == 0)
                break;
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
        0x11, 0x4000, PSP_THREAD_ATTR_USER, NULL);
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
        0x11, 0x4000, PSP_THREAD_ATTR_USER, NULL);
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
 * control_stream_stop
 * ══════════════════════════════════════════════════════════════════ */
void control_stream_stop(void)
{
    SceUInt timeout = 2000000; /* 2 seconds */

    ctrl_running = 0;

    /* IMPORTANT: Close the socket BEFORE waiting for threads.
     * This unblocks Recvfrom immediately on most PSP network stacks,
     * preventing a 2-second hang in WaitThreadEnd. */
    if (ctrl_socket >= 0) {
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
