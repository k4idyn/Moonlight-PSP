/*
 * network_me.c - Network receive thread for PSP Moonlight
 *
 * Binds a UDP socket to an ephemeral port (port 0) and receives video
 * RTP packets into the lock-free ring buffer.  A companion ping thread
 * sends SS_PING datagrams to Sunshine's video server port every 500 ms;
 * Sunshine uses the source address of those pings to know where to send
 * RTP video (it ignores the X-GS-ClientPort we advertised in SETUP).
 *
 * Network stack init is done in network_connect.c:wifi_connect().
 */

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspsysmem.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psputility_sysparam.h>
#include <pspiofilemgr.h>
#include <psprtc.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "diag_log.h"
#define net_log(fmt, ...) diag_log_write("NET", fmt, ##__VA_ARGS__)

#include "shared.h"
#include "moonlight_ports.h"
#include "stream_crypto.h"
#include "control_stream.h"
#include "signal_strength.h"
#include "rtp_fec.h"

/* RTP packet parsing constants (for SPS detection in ring overflow handling) */
#define NET_RTP_FIXED_HEADER_SIZE 12
#define NET_RTP_FLAG_EXTENSION    0x10
#define NET_NV_VIDEO_PKT_SIZE     16

static u16 net_read_be16(const u8 *p)
{
    return (u16)(((u16)p[0] << 8) | p[1]);
}

static int net_rtp_nv_offset(const u8 *packet, int packet_len)
{
    int off = NET_RTP_FIXED_HEADER_SIZE;
    if (!packet || packet_len < off)
        return -1;
    if (packet[0] & NET_RTP_FLAG_EXTENSION) {
        u16 ext_words;
        if (packet_len < off + 4)
            return -1;
        ext_words = net_read_be16(packet + off + 2);
        off += 4 + (int)ext_words * 4;
    }
    if (packet_len < off + NET_NV_VIDEO_PKT_SIZE)
        return -1;
    return off;
}

/* Frame counter for loss stats reporting (control_stream.c) */
extern volatile unsigned int g_last_good_frame;

/* GU UI owns the framebuffer during normal runtime; avoid direct debug-screen writes. */
#define pspDebugScreenPrintf(...) ((void)0)

/* me_running flag defined in main.c */
extern volatile int me_running;

/* Video stream params filled by network_connect.c after RTSP SETUP */
extern char g_video_server_ip[64];     /* Sunshine IP, e.g. "10.0.0.73" */
extern int  g_video_server_port;       /* Sunshine video UDP port, default 47998 */
extern char g_video_ping_payload[17];  /* X-SS-Ping-Payload (16 chars) from SETUP */

/* NIC IP filled by network_connect.c (diagnostic only). */
extern char g_nic_ip[16];

/* SS_PING packet — matches moonlight-common-c Video.h */
typedef struct {
    char     payload[16]; /* X-SS-Ping-Payload from SETUP response */
    uint32_t seq_be;      /* big-endian sequence number */
} SsPingPkt;

/* Keep wire layout exact: 16-byte payload + BE32 sequence */
typedef char _ss_ping_size_check[(sizeof(SsPingPkt) == 20) ? 1 : -1];

/* UDP receive socket (shared with ping thread for sendto) */
static int udp_socket = -1;

/* Thread IDs */
static SceUID net_thread_id  = -1;
static SceUID ping_thread_id = -1;

/* WiFi power save: original state saved at init, restored at shutdown.
 * -1 = not yet read (don't restore). */
static int g_orig_wlan_powersave = -1;

/* Effective SO_RCVBUF cap observed at init. Dynamic tuning never targets
 * above this, which avoids repeated no-op/failed growth attempts on
 * low-memory PSP-1000 sessions. */
static int g_rcvbuf_cap = 128 * 1024;

/* Guard net.log writes across threads so logging can never stall startup. */
static volatile int net_log_lock = 0;

/*--------------------------------------------------------------------------
 * WiFi Auto-Reconnection State
 *
 * When the PSP's 802.11b WiFi drops during streaming, the ping thread
 * detects ap_state != 4 and triggers wifi_try_reconnect().  This
 * disconnects cleanly, re-associates with AP slot 1, and polls until
 * the connection is restored.  Existing UDP sockets (bound to INADDR_ANY)
 * remain valid — Sunshine re-learns our source address from resumed pings.
 *--------------------------------------------------------------------------*/
volatile int g_wifi_reconnecting = 0;              /* Other threads check this */
static u32   g_last_reconnect_time = 0;            /* Throttle reconnect attempts */
static int   g_reconnect_count = 0;                /* Total reconnect attempts */
static int   g_reconnect_success = 0;              /* Successful reconnects */
#define WIFI_RECONNECT_COOLDOWN_US  (500 * 1000)       /* 500ms min between attempts for instantaneous retry */
#define WIFI_RECONNECT_MAX_POLL     150                 /* 150 * 100ms = 15s timeout */

/* Phase 5.2: RTCP Receiver Report state */
static volatile u32 s_rtcp_pkts_received = 0;
static volatile u32 s_rtcp_pkts_lost_cum = 0;
static u32 s_rtcp_highest_seq = 0;
static u32 s_rtcp_jitter_us = 0;        /* EWMA inter-arrival jitter (us) */
static u32 s_rtcp_last_arrival_us = 0;
static u32 s_rtcp_ssrc = 0;
static u32 s_rtcp_last_seq = 0;
static int s_rtcp_have_last_seq = 0;

/* Phase 5.8: Session resume state */
#define SESSION_STATE_ACTIVE       0
#define SESSION_STATE_WIFI_LOST    1
#define SESSION_STATE_RECONNECTING 2
#define SESSION_STATE_RESUMED      3
static volatile int s_session_state = 0; /* SESSION_STATE_ACTIVE */
static u32 s_wifi_lost_time_us = 0;

/* Forward-declared: used by wifi_try_reconnect and keepalive thread */
static volatile int g_keepalive_running = 0;

static int wifi_try_reconnect(void)
{
    int ret;
    int state;
    int attempts = 0;
    u32 now = sceKernelGetSystemTimeLow();

    /* Cooldown: don't hammer reconnect if we just tried */
    if (g_last_reconnect_time != 0 &&
        (now - g_last_reconnect_time) < WIFI_RECONNECT_COOLDOWN_US) {
        return -1;
    }

    g_wifi_reconnecting = 1;
    g_reconnect_count++;
    g_last_reconnect_time = now;

    net_log("[WIFI] reconnect attempt #%d starting\n", g_reconnect_count);

    /* Step 1: Disconnect cleanly from current (broken) AP session */
    ret = sceNetApctlDisconnect();
    net_log("[WIFI] disconnect ret=0x%08X\n", (unsigned)ret);

    /* Step 1b: Poll until state reaches 0 (fully disconnected).
     * sceNetApctlDisconnect() is async — calling Connect() before
     * the state machine reaches 0 fails with 0x80410A80.
     * EXCEPTION: state < 0 means the AP driver has no connection at all,
     * so we can skip straight to Connect(). */
    {
        int disc_polls = 0;
        int disc_state = -1;
        sceNetApctlGetState(&disc_state);
        if (disc_state < 0) {
            /* AP driver reports no connection — skip disconnect polling */
            net_log("[WIFI] ap_state=%d (no session), skipping disconnect wait\n", disc_state);
        } else {
            while (disc_polls < 500 && (me_running || g_keepalive_running)) { /* 500 * 10ms = 5s max */
                sceNetApctlGetState(&disc_state);
                if (disc_state <= 0)
                    break;
                if (disc_polls == 0 || (disc_polls % 50) == 0)
                    net_log("[WIFI] waiting for disconnect: state=%d poll=%dms\n",
                            disc_state, disc_polls * 10);
                sceKernelDelayThread(10 * 1000); /* 10ms */
                disc_polls++;
            }
            net_log("[WIFI] disconnected in %dms (state=%d)\n",
                    disc_polls * 10, disc_state);
            if (disc_state > 0) {
                net_log("[WIFI] disconnect stuck at state=%d, aborting\n", disc_state);
                g_wifi_reconnecting = 0;
                return -1;
            }
        }
    }

    if (!me_running && !g_keepalive_running) {
        g_wifi_reconnecting = 0;
        return -1;
    }

    /* Step 2: Reconnect to AP slot 1 */
    ret = sceNetApctlConnect(1);
    if (ret < 0) {
        net_log("[WIFI] sceNetApctlConnect(1) failed 0x%08X\n", (unsigned)ret);
        g_wifi_reconnecting = 0;
        return -1;
    }

    /* Step 3: Poll for connected state (state == 4 = GOT_IP) */
    while (attempts < (WIFI_RECONNECT_MAX_POLL * 10) && (me_running || g_keepalive_running)) {
        ret = sceNetApctlGetState(&state);
        if (ret < 0) {
            net_log("[WIFI] GetState failed 0x%08X at poll #%d\n",
                    (unsigned)ret, attempts);
            g_wifi_reconnecting = 0;
            return -1;
        }

        if (state == 4) {
            /* Connected! Log the new IP address */
            union SceNetApctlInfo info;
            memset(&info, 0, sizeof(info));
            sceNetApctlGetInfo(8, &info); /* 8 = IP address */
            g_reconnect_success++;
            net_log("[WIFI] reconnected OK! IP=%s attempt=#%d poll=%dms successes=%d\n",
                    info.ip, g_reconnect_count, attempts * 10, g_reconnect_success);
            g_wifi_reconnecting = 0;
            return 0;
        }

        /* Log state transitions (silenced otherwise) */
        if (attempts == 0 || (attempts % 50) == 0) {
            net_log("[WIFI] polling state=%d at %dms\n", state, attempts * 10);
        }

        sceKernelDelayThread(10 * 1000); /* 10ms */
        attempts++;
    }

    net_log("[WIFI] reconnect timeout after %dms (state=%d)\n",
            attempts * 10, state);
    g_wifi_reconnecting = 0;
    return -1;
}

/*--------------------------------------------------------------------------
 * WiFi Keepalive Thread
 *
 * Runs globally from WiFi-connect until app exit. Checks AP state every
 * 5 seconds and triggers reconnect if WiFi drops during idle UI phases
 * (host discovery, app selection, etc.) where no streaming ping thread
 * is active. During active streaming (me_running == 1) the ping thread
 * already handles WiFi monitoring, so keepalive skips its check.
 *--------------------------------------------------------------------------*/
/* g_keepalive_running forward-declared above wifi_try_reconnect */
static SceUID       g_keepalive_thread_id = -1;

static int wifi_keepalive_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    net_log("[KEEPALIVE] thread started\n");

    while (g_keepalive_running) {
        /* Monitor WiFi during ALL phases — including streaming.
         * The ping thread also checks WiFi, but keepalive provides a
         * safety net with reconnect capability.  During streaming the
         * ping thread detects disconnect faster (1s vs 5s), but only
         * keepalive attempts the full reconnect sequence. */
        {
            int ap_state = -1;
            int ret = sceNetApctlGetState(&ap_state);

            if (ret >= 0 && ap_state == 0 && !g_wifi_reconnecting) {
                /* State 0 = fully disconnected (confirmed by API). Reconnect.
                 * Don't reconnect on ap_state < 0 (API error) or 1-3 (connecting). */
                net_log("[KEEPALIVE] WiFi disconnected ap_state=%d me_running=%d, reconnecting\n",
                        ap_state, me_running);
                int rc = wifi_try_reconnect();
                net_log("[KEEPALIVE] reconnect result=%d\n", rc);
            }
        }

        /* Sleep 3 seconds during streaming, 5 seconds during idle.
         * (in 500ms chunks so we can exit promptly) */
        {
            int i;
            int chunks = me_running ? 6 : 10;  /* 3s vs 5s */
            for (i = 0; i < chunks && g_keepalive_running; i++) {
                sceKernelDelayThread(500 * 1000);
            }
        }
    }

    net_log("[KEEPALIVE] thread exiting\n");
    sceKernelExitDeleteThread(0);
    return 0;
}

void wifi_keepalive_start(void)
{
    if (g_keepalive_thread_id >= 0) return; /* already running */

    g_keepalive_running = 1;
    g_keepalive_thread_id = sceKernelCreateThread(
        "wifi_keepalive", wifi_keepalive_thread,
        0x30,    /* priority — low, background task */
        0x1000,  /* 4KB stack */
        PSP_THREAD_ATTR_USER, NULL);

    if (g_keepalive_thread_id >= 0) {
        sceKernelStartThread(g_keepalive_thread_id, 0, NULL);
        net_log("[KEEPALIVE] started tid=0x%08X\n", g_keepalive_thread_id);
    } else {
        net_log("[KEEPALIVE] create failed 0x%08X\n", g_keepalive_thread_id);
        g_keepalive_running = 0;
    }
}

void wifi_keepalive_stop(void)
{
    if (g_keepalive_thread_id < 0) return;

    g_keepalive_running = 0;
    {
        SceUInt timeout_us = 6000000; /* 6 seconds max wait */
        sceKernelWaitThreadEnd(g_keepalive_thread_id, &timeout_us);
        sceKernelDeleteThread(g_keepalive_thread_id);
    }
    g_keepalive_thread_id = -1;
    net_log("[KEEPALIVE] stopped\n");
}

/* Forward declarations */
static int network_recv_thread(SceSize args, void *argp);
static int network_ping_thread(SceSize args, void *argp);
void network_me_shutdown(void);
void sw_decoder_thread_wakeup(void);

static int get_udp_local_port(unsigned short *out_port)
{
    struct sockaddr_in addr;
    socklen_t namelen = sizeof(addr);
    memset(&addr, 0, sizeof(addr));
    addr.sin_len = (uint8_t)sizeof(addr);
    if (sceNetInetGetsockname(udp_socket,
                              (struct sockaddr *)&addr, &namelen) != 0) {
        return -1;
    }
    if (out_port) {
        *out_port = ntohs(addr.sin_port);
    }
    return 0;
}

static int udp_socket_rtcp = -1;

/* Reserve and bind the video UDP socket early so RTSP can advertise the
 * real local client port we will receive on.
 *
 * We MUST bind consecutive ports (Data and Control) so Sunshine's RTCP
 * packets do not generate ICMP "Port Unreachable" responses. If they do,
 * Sunshine's WSASendMsg logs error 10022 and the stream breaks!
 */
int network_me_reserve_client_port(unsigned short *out_port)
{
    struct sockaddr_in addr;
    unsigned short local_port = 0;

    if (udp_socket >= 0) {
        if (get_udp_local_port(&local_port) == 0) {
            if (out_port) *out_port = local_port;
            return 0;
        }
        return -1;
    }

    /* Bind data socket to ephemeral port (port 0 — OS assigns the port),
     * then read the actual assigned port via getsockname() and bind RTCP
     * to port+1 explicitly.  This avoids any fixed-port collision with
     * Sunshine/Apollo on the same host while still guaranteeing consecutive
     * Data/RTCP ports are socket error 10022. */
    int attempt;
    for (attempt = 0; attempt < 20; attempt++) {
        udp_socket = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        udp_socket_rtcp = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (udp_socket < 0 || udp_socket_rtcp < 0) {
            if (udp_socket >= 0) sceNetInetClose(udp_socket);
            if (udp_socket_rtcp >= 0) sceNetInetClose(udp_socket_rtcp);
            net_log("[NET PREP] socket() failed: errno=%d\n", sceNetInetGetErrno());
            return -1;
        }

        int reuse = 1;
        sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sceNetInetSetsockopt(udp_socket_rtcp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        memset(&addr, 0, sizeof(addr));
        addr.sin_len    = (uint8_t)sizeof(addr);
        addr.sin_family = AF_INET;

        /* Always bind to INADDR_ANY (0.0.0.0).  This accepts packets on ALL
         * interfaces — NIC, loopback, etc.  On real PSP hardware this is the
         * only correct option (single Wi-Fi NIC). */
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = 0; /* ephemeral: let the OS assign the port */

        if (sceNetInetBind(udp_socket, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            net_log("[NET PREP] bind(port=0) failed: errno=%d\n", sceNetInetGetErrno());
            sceNetInetClose(udp_socket);
            sceNetInetClose(udp_socket_rtcp);
            udp_socket = -1;
            udp_socket_rtcp = -1;
            return -1;
        }

        /* Capture the OS-assigned port via getsockname() */
        if (get_udp_local_port(&local_port) != 0 || local_port == 0) {
            net_log("[NET PREP] getsockname() failed after ephemeral bind\n");
            sceNetInetClose(udp_socket);
            sceNetInetClose(udp_socket_rtcp);
            udp_socket = -1;
            udp_socket_rtcp = -1;
            return -1;
        }

        /* Avoid port 65535 since data_port + 1 would wrap to 0. */
        if (local_port == 65535) {
            sceNetInetClose(udp_socket);
            sceNetInetClose(udp_socket_rtcp);
            udp_socket = -1;
            udp_socket_rtcp = -1;
            continue;
        }

        /* Bind RTCP socket to the consecutive port (data_port + 1) */
        addr.sin_port = htons(local_port + 1);
        if (sceNetInetBind(udp_socket_rtcp, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            if (out_port) *out_port = local_port;
            net_log("[NET PREP] bound ephemeral consecutive ports %s:%d-%d\n",
                    inet_ntoa(addr.sin_addr), local_port, local_port + 1);
            return 0;
        }

        /* RTCP port P+1 was in use — release sockets and let the OS give
         * us a different ephemeral port on the next attempt. */
        sceNetInetClose(udp_socket);
        sceNetInetClose(udp_socket_rtcp);
        udp_socket = -1;
        udp_socket_rtcp = -1;
    }

    net_log("[NET PREP] Failed to bind consecutive ephemeral sockets!\n");
    return -1;
}

/* Send a short video ping burst immediately after RTSP PLAY so Sunshine can
 * lock onto the correct client endpoint before delayed init phases finish. */
int network_me_send_video_ping_burst(const char *server_ip,
                                     int server_port,
                                     const char *ping_payload)
{
    struct sockaddr_in dst;
    SsPingPkt ss_pkt;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    int i;
    int sent_any = 0;

    if (udp_socket < 0 || server_ip == NULL || server_port <= 0) {
        return -1;
    }

    net_log("[NET PREP] ping burst: server=%s:%d\n",
            server_ip, server_port);

    /* Log source port so we can confirm what Sunshine will reply to */
    {
        struct sockaddr_in my_addr;
        socklen_t my_len = sizeof(my_addr);
        memset(&my_addr, 0, sizeof(my_addr));
        if (sceNetInetGetsockname(udp_socket, (struct sockaddr *)&my_addr, &my_len) == 0) {
            net_log("[NET PREP] our UDP src port=%u ip=%s (Sunshine replies here)\n",
                    (unsigned)ntohs(my_addr.sin_port), inet_ntoa(my_addr.sin_addr));
        }
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (uint8_t)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)server_port);
    dst.sin_addr.s_addr = inet_addr(server_ip);

    for (i = 1; i <= 5; ++i) {
        int sent;
        int err;

        if (ping_payload && ping_payload[0] != '\0') {
            memcpy(ss_pkt.payload, ping_payload, 16);
            ss_pkt.seq_be = htonl((u32)i);
            sent = (int)sceNetInetSendto(udp_socket, &ss_pkt, sizeof(ss_pkt),
                                         0, (struct sockaddr *)&dst, sizeof(dst));
        } else {
            sent = (int)sceNetInetSendto(udp_socket, legacy_ping, 4,
                                         0, (struct sockaddr *)&dst, sizeof(dst));
        }
        err = (sent < 0) ? sceNetInetGetErrno() : 0;

        if (sent > 0) {
            sent_any = 1;
        }

        net_log("[NET PREP] priming ping #%d sent=%d%s to %s:%d payload=[%.16s]\n",
                i, sent, err ? " FAIL" : "", server_ip, server_port,
                (ping_payload && ping_payload[0]) ? ping_payload : "LEGACY-PING");
        if (err)
            net_log("[NET PREP] priming ping #%d errno=%d\n", i, err);
        sceKernelDelayThread(30 * 1000);
    }

    return sent_any ? 0 : -1;
}

/* Phase 5.6: Send a burst of pings (e.g. after WiFi reconnect) */
static void send_ping_burst_internal(int count, const char *reason)
{
    struct sockaddr_in dst;
    SsPingPkt ss_pkt;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    int i;
    int sent_ok = 0;
    int sent_fail = 0;

    if (udp_socket < 0) return;

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (uint8_t)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_video_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    for (i = 0; i < count; i++) {
        int tx;
        if (g_video_ping_payload[0] != '\0') {
            memcpy(ss_pkt.payload, g_video_ping_payload, 16);
            ss_pkt.seq_be = htonl((u32)(i + 1));
            tx = (int)sceNetInetSendto(udp_socket, &ss_pkt, sizeof(ss_pkt),
                                       0, (struct sockaddr *)&dst, sizeof(dst));
        } else {
            tx = (int)sceNetInetSendto(udp_socket, legacy_ping, 4,
                                       0, (struct sockaddr *)&dst, sizeof(dst));
        }

        if (tx > 0) {
            sent_ok++;
        } else {
            int err = sceNetInetGetErrno();
            sent_fail++;
            if (sent_fail <= 3 || (sent_fail % 10) == 0) {
                net_log("[PHASE5-PING] burst send failed #%d errno=%d (reason=%s)\n",
                        sent_fail, err, reason ? reason : "n/a");
            }
        }
        sceKernelDelayThread(30 * 1000); /* 30ms between pings */
    }
    net_log("[PHASE5-PING] burst complete total=%d ok=%d fail=%d (reason: %s)\n",
            count, sent_ok, sent_fail, reason ? reason : "n/a");
}

/* net_log already defined at top of file via diag_log.h */

/*--------------------------------------------------------------------------
 * Phase 5.2: Send RTCP Receiver Report (RR) on video RTCP port
 *--------------------------------------------------------------------------*/
static void send_rtcp_rr(void)
{
    unsigned char buf[32];
    u32 frac_lost = 0;
    u32 cum_lost;
    struct sockaddr_in rtcp_dst;

    if (udp_socket_rtcp < 0 || g_video_server_ip[0] == '\0') return;

    cum_lost = s_rtcp_pkts_lost_cum;
    if (s_rtcp_pkts_received + cum_lost > 0) {
        frac_lost = (cum_lost * 256) / (s_rtcp_pkts_received + cum_lost);
    }
    if (frac_lost > 255) frac_lost = 255;

    /* RTCP RR header: V=2, P=0, RC=1, PT=201, length=7 */
    buf[0]  = 0x81;  buf[1] = 201;
    buf[2]  = 0;     buf[3] = 7;
    /* Sender SSRC = 1 */
    buf[4]  = 0; buf[5] = 0; buf[6] = 0; buf[7] = 1;
    /* Report block: source SSRC */
    buf[8]  = (u8)(s_rtcp_ssrc >> 24); buf[9]  = (u8)(s_rtcp_ssrc >> 16);
    buf[10] = (u8)(s_rtcp_ssrc >> 8);  buf[11] = (u8)s_rtcp_ssrc;
    /* Fraction lost + cumulative lost (24-bit) */
    buf[12] = (u8)frac_lost;
    buf[13] = (u8)((cum_lost >> 16) & 0xFF);
    buf[14] = (u8)((cum_lost >> 8) & 0xFF);
    buf[15] = (u8)(cum_lost & 0xFF);
    /* Highest seq */
    buf[16] = (u8)(s_rtcp_highest_seq >> 24); buf[17] = (u8)(s_rtcp_highest_seq >> 16);
    buf[18] = (u8)(s_rtcp_highest_seq >> 8);  buf[19] = (u8)s_rtcp_highest_seq;
    /* Jitter */
    buf[20] = (u8)(s_rtcp_jitter_us >> 24); buf[21] = (u8)(s_rtcp_jitter_us >> 16);
    buf[22] = (u8)(s_rtcp_jitter_us >> 8);  buf[23] = (u8)s_rtcp_jitter_us;
    /* LSR = 0, DLSR = 0 (no SR received from server) */
    buf[24] = 0; buf[25] = 0; buf[26] = 0; buf[27] = 0;
    buf[28] = 0; buf[29] = 0; buf[30] = 0; buf[31] = 0;

    memset(&rtcp_dst, 0, sizeof(rtcp_dst));
    rtcp_dst.sin_len    = (uint8_t)sizeof(rtcp_dst);
    rtcp_dst.sin_family = AF_INET;
    rtcp_dst.sin_port   = htons((unsigned short)(g_video_server_port + 1));
    rtcp_dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    {
        int tx = (int)sceNetInetSendto(udp_socket_rtcp, buf, 32, 0,
                                       (struct sockaddr *)&rtcp_dst, sizeof(rtcp_dst));
        if (tx < 0) {
            net_log("[PHASE5-RTCP] RR send failed errno=%d\n", sceNetInetGetErrno());
        } else {
            net_log("[PHASE5-RTCP] sent RR: frac_lost=%u cum_lost=%u jitter=%u\n",
                    (unsigned)frac_lost, (unsigned)cum_lost, (unsigned)s_rtcp_jitter_us);
        }
    }
}

/*--------------------------------------------------------------------------
 * Phase 5.5: WiFi power save management
 *--------------------------------------------------------------------------*/
void network_me_set_power_save(int enable)
{
    int ps_ret = sceUtilitySetSystemParamInt(
        PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
        enable ? PSP_SYSTEMPARAM_WLAN_POWERSAVE_ON
               : PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF);
    net_log("[PHASE5-WIFIPS] power save %s (ret=%d)\n",
            enable ? "ON" : "OFF", ps_ret);
}

/*--------------------------------------------------------------------------
 * network_me_init - Create UDP socket and spawn receive thread
 *
 * Assumes network stack is already initialized by wifi_connect().
 *--------------------------------------------------------------------------*/
void network_me_init(PacketRingBuffer *rb)
{
    diag_log_write("NET", "network_me_init entry (rb=%p)\n", rb);
    int ret;
    unsigned short local_port;
    int prebound = (udp_socket >= 0);

    /* If already running, shut down first to ensure clean state */
    if (net_thread_id >= 0 || ping_thread_id >= 0) {
        network_me_shutdown();
    }

    /* Zero the ring buffer */
    memset(rb, 0, sizeof(PacketRingBuffer));

    /* Disable WiFi power save to eliminate 50-280ms periodic gap spikes.
     * 802.11b power save causes the radio to sleep between beacons, adding
     * multi-frame latency spikes every ~2 seconds.  Save the original
     * setting and restore it in network_me_shutdown(). */
    {
        int ps_val = -1;
        int ps_ret = sceUtilityGetSystemParamInt(
            PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE, &ps_val);
        if (ps_ret == 0) {
            g_orig_wlan_powersave = ps_val;
            net_log("[NET INIT] WLAN power save current=%d\n", ps_val);
            if (ps_val != PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF) {
                ps_ret = sceUtilitySetSystemParamInt(
                    PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
                    PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF);
                net_log("[NET INIT] WLAN power save disabled (ret=%d)\n",
                        ps_ret);
            }
        } else {
            net_log("[NET INIT] WLAN power save query failed (%d)\n", ps_ret);
        }
    }

    net_log("[NET INIT] starting, server=%s:%d\n",
            g_video_server_ip, g_video_server_port);

    ret = network_me_reserve_client_port(&local_port);
    if (ret < 0) {
        net_log("[NET INIT] reserve socket failed\n");
        return;
    }
    net_log("[NET INIT] socket fd=%d\n", udp_socket);
    net_log("[NET INIT] bound local video port %u OK%s\n",
            (unsigned)local_port,
            prebound ? " (pre-bound for RTSP SETUP)" : "");

    /* Enlarge receive buffer so video packets that arrive before the recv
     * thread starts reading are not silently dropped by the kernel.
     * PSP firmware caps this in practice, so set the known-good target
     * directly to avoid deterministic fallback fault churn. */
    {
        int rcvbuf = 128 * 1024;

        int opt_ret = sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                           &rcvbuf, sizeof(rcvbuf));
        if (opt_ret < 0) {
            net_log("[NET INIT] 128KB rcvbuf set failed (%d)\n", opt_ret);
        }

        {
            int actual_rcvbuf = 0;
            socklen_t actual_len = sizeof(actual_rcvbuf);
            if (sceNetInetGetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                     &actual_rcvbuf, &actual_len) == 0) {
                g_rcvbuf_cap = (actual_rcvbuf > 0) ? actual_rcvbuf : rcvbuf;
                net_log("[NET INIT] SO_RCVBUF target=%dKB actual=%dKB\n",
                        rcvbuf / 1024, actual_rcvbuf / 1024);
            } else {
                g_rcvbuf_cap = rcvbuf;
            }

            if (g_rcvbuf_cap < (128 * 1024))
                g_rcvbuf_cap = 128 * 1024;
        }
    }

    /* Enlarge send buffer for outbound pings — PSP default is tiny and
     * causes ENOBUFS (errno=105) after ~10s of streaming. */
    {
        int sndbuf = 32768;
#ifndef SO_SNDBUF
#define SO_SNDBUF 0x1001
#endif
        sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_SNDBUF,
                             &sndbuf, sizeof(sndbuf));
    }

    /* Blocking recv with 2-second timeout */
    {
        struct timeval tv;
        tv.tv_sec  = 2;
        tv.tv_usec = 0;
        sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVTIMEO,
                             &tv, sizeof(tv));
    }

    /* Log free memory before thread creation (debug) */
    {
        SceSize max_free = sceKernelMaxFreeMemSize();
        SceSize total_free = sceKernelTotalFreeMemSize();
        net_log("[NET INIT] free mem: max_block=%u total=%u\n",
                (unsigned)max_free, (unsigned)total_free);
    }

    /* Spawn recv and ping threads */
    me_running = 1;

    net_thread_id = sceKernelCreateThread(
        "net_recv_thread",
        network_recv_thread,
        0x12, 0x8000, PSP_THREAD_ATTR_USER, NULL);
    if (net_thread_id < 0) {
        net_log("[NET INIT] recv thread create failed: %d\n", net_thread_id);
        me_running = 0;
        sceNetInetClose(udp_socket); udp_socket = -1;
        if (udp_socket_rtcp >= 0) {
            sceNetInetClose(udp_socket_rtcp);
            udp_socket_rtcp = -1;
        }
        return;
    }
    sceKernelStartThread(net_thread_id, sizeof(rb), &rb);
    net_log("[NET INIT] recv thread started id=%d\n", net_thread_id);

    ping_thread_id = sceKernelCreateThread(
        "net_ping_thread",
        network_ping_thread,
        0x18, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (ping_thread_id < 0) {
        net_log("[NET INIT] ping thread create failed: %d\n", ping_thread_id);
        /* non-fatal, continue without pings but log the issue */
        ping_thread_id = -1;
    } else {
        sceKernelStartThread(ping_thread_id, 0, NULL);
        net_log("[NET INIT] ping thread started id=%d\n", ping_thread_id);
    }

    net_log("[NET INIT] done. ping_payload=[%.16s] server=%s:%d\n",
            g_video_ping_payload, g_video_server_ip, g_video_server_port);
}

/*--------------------------------------------------------------------------
 * network_ping_thread - Sends SS_PING packets to Sunshine every 500 ms.
 *
 * Sunshine ignores X-GS-ClientPort from RTSP SETUP and instead sends
 * video to the source port of the first ping it receives.  We must keep
 * pinging throughout the session so Sunshine knows we are alive.
 *--------------------------------------------------------------------------*/
static int network_ping_thread(SceSize args, void *argp)
{
    struct sockaddr_in dst;
    uint32_t seq = 0;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    SsPingPkt ss_pkt;

    net_log("[PING] thread started\n"
            "[PING]   server    = %s:%d\n"
            "[PING]   payload   = [%.16s]\n",
            g_video_server_ip, g_video_server_port,
            g_video_ping_payload);

    /* Log source port so we know what UDP port Sunshine will reply to */
    {
        struct sockaddr_in my_addr;
        socklen_t my_len = sizeof(my_addr);
        memset(&my_addr, 0, sizeof(my_addr));
        if (sceNetInetGetsockname(udp_socket, (struct sockaddr *)&my_addr, &my_len) == 0) {
            net_log("[PING] our src = %s:%u (Sunshine sends video TO here)\n",
                    inet_ntoa(my_addr.sin_addr), (unsigned)ntohs(my_addr.sin_port));
        } else {
            net_log("[PING] getsockname failed errno=%d\n", sceNetInetGetErrno());
        }
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (uint8_t)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_video_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    u32 consec_sendfail = 0;  /* Consecutive SENDFAIL counter */

    while (me_running) {
        int sent;
        int err;
        seq++;

        if (g_video_ping_payload[0] != '\0') {
            memcpy(ss_pkt.payload, g_video_ping_payload, 16);
            ss_pkt.seq_be = htonl(seq);
            sent = (int)sceNetInetSendto(udp_socket, &ss_pkt, sizeof(ss_pkt),
                                         0, (struct sockaddr *)&dst, sizeof(dst));
        } else {
            sent = (int)sceNetInetSendto(udp_socket, legacy_ping, 4,
                                         0, (struct sockaddr *)&dst, sizeof(dst));
        }
        err = (sent < 0) ? sceNetInetGetErrno() : 0;

        if (err) {
            consec_sendfail++;
        } else {
            consec_sendfail = 0;
        }

        /* Log first 5 pings and errors only (silenced hot-path for perf) */
        if (seq <= 5 || err) {
            net_log("[PING] #%u -> %s:%d sent=%d%s%d\n",
                    seq, g_video_server_ip, g_video_server_port, sent,
                    err ? " SENDFAIL errno=" : "",
                    err);
        }

        /* WiFi state check every 10 pings (1 second) — detect AP disconnect.
         * Also force reconnect after 100 consecutive SENDFAILs (~10 seconds)
         * to handle ENOBUFS (errno=105) where WiFi buffers are saturated
         * but ap_state still reports connected.  Threshold doubled from 50
         * because ping interval was halved to 100ms. */
        if ((seq % 10) == 0 || consec_sendfail == 100) {
            int ap_state = -1;
            int force_reconnect = (consec_sendfail >= 100);
            sceNetApctlGetState(&ap_state);

            if (ap_state != 4 || force_reconnect) {
                net_log("[PING] %s ap_state=%d at ping#%u sendfail=%u\n",
                        force_reconnect ? "WIFI_ENOBUFS" : "WIFI_DOWN",
                        ap_state, seq, consec_sendfail);

                /* Phase 5.8: Mark session as WiFi lost */
                if (s_session_state == SESSION_STATE_ACTIVE) {
                    s_session_state = SESSION_STATE_WIFI_LOST;
                    s_wifi_lost_time_us = sceKernelGetSystemTimeLow();
                    net_log("[PHASE5-RESUME] session state: WIFI_LOST\n");
                    /* Phase 5.5: Enable power save during WiFi recovery */
                    network_me_set_power_save(1);
                }

                /* Attempt automatic WiFi reconnection */
                if (!g_wifi_reconnecting) {
                    s_session_state = SESSION_STATE_RECONNECTING;
                    net_log("[PHASE5-RESUME] session state: RECONNECTING\n");
                    int rc = wifi_try_reconnect();
                    if (rc == 0) {
                        net_log("[PING] WiFi restored at ping#%u, resuming\n", seq);
                        consec_sendfail = 0;
                        /* Phase 5.8: Resume session */
                        s_session_state = SESSION_STATE_RESUMED;
                        net_log("[PHASE5-RESUME] session state: RESUMED\n");
                        /* Phase 5.5: Disable power save for streaming */
                        network_me_set_power_save(0);
                        /* Phase 5.6: Burst of 3 pings after reconnect */
                        send_ping_burst_internal(3, "wifi_reconnect");
                        /* Request IDR for clean video resume */
                        control_stream_request_idr();
                        /* Return to active state */
                        s_session_state = SESSION_STATE_ACTIVE;
                    } else {
                        net_log("[PING] WiFi reconnect failed, will retry at next check\n");
                    }
                }
            }
        }

        /* Log source port at ping #5 only (performance: silenced hot-path) */
        if (seq == 5) {
            struct sockaddr_in my_addr;
            socklen_t my_len = sizeof(my_addr);
            memset(&my_addr, 0, sizeof(my_addr));
            if (sceNetInetGetsockname(udp_socket, (struct sockaddr *)&my_addr, &my_len) == 0) {
                net_log("[PING] #%u src-check: %s:%u me_running=%d\n",
                        seq, inet_ntoa(my_addr.sin_addr),
                        (unsigned)ntohs(my_addr.sin_port), me_running);
            }
        }

        /* Dynamic SO_RCVBUF: every 50 pings (5s at 100ms), check connection
         * quality and enlarge the socket receive buffer when quality degrades.
         * CRITICAL/POOR → 512KB, FAIR → 384KB, GOOD/EXCELLENT → 256KB. */
        if ((seq % 50) == 0 && udp_socket >= 0) {
            ConnQualityState cq = control_stream_get_quality();
            int target_rcvbuf;
            if (cq.quality >= CONN_QUALITY_POOR) {
                target_rcvbuf = 512 * 1024;
            } else if (cq.quality >= CONN_QUALITY_FAIR) {
                target_rcvbuf = 384 * 1024;
            } else {
                target_rcvbuf = 256 * 1024;
            }
            if (target_rcvbuf > g_rcvbuf_cap)
                target_rcvbuf = g_rcvbuf_cap;

            {
                static int s_prev_rcvbuf = 0;
                if (s_prev_rcvbuf <= 0)
                    s_prev_rcvbuf = g_rcvbuf_cap;
                if (target_rcvbuf != s_prev_rcvbuf) {
                    net_log("[RCVBUF] %dKB -> %dKB (quality=%d)\n",
                            s_prev_rcvbuf / 1024, target_rcvbuf / 1024,
                            (int)cq.quality);
                    s_prev_rcvbuf = target_rcvbuf;
                }
            }
            sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                 &target_rcvbuf, sizeof(target_rcvbuf));

            {
                int actual_rcvbuf = 0;
                socklen_t actual_len = sizeof(actual_rcvbuf);
                if (sceNetInetGetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                         &actual_rcvbuf, &actual_len) == 0) {
                    net_log("[RCVBUF] applied=%dKB (target=%dKB q=%d)\n",
                            actual_rcvbuf / 1024,
                            target_rcvbuf / 1024,
                            (int)cq.quality);
                }
            }
        }

        /* Phase 4: WiFi stutter compensation — proactive IDR on degrading signal */
        if ((seq % 20) == 0) {
            SignalTrend trend = signal_strength_get_trend();
            if (trend == SIGNAL_TREND_DEGRADING) {
                static u32 s_proactive_idr_count = 0;
                s_proactive_idr_count++;
                if (s_proactive_idr_count <= 3 || (s_proactive_idr_count % 10) == 0)
                    net_log("[PHASE4-WIFI] signal degrading, proactive IDR #%u\n",
                            s_proactive_idr_count);
                control_stream_request_idr();
            }
        }

        /* Aggressive ping: 100ms interval for faster NAT keepalive and
         * earlier stall detection.  At 100ms, WiFi state checks fire
         * every 10 pings (1 second) and ENOBUFS detection at 50 pings
         * (5 seconds).  The extra 5 pings/sec cost ~100 bytes/sec
         * overhead — negligible vs video bandwidth. */

        /* Phase 5.2: Send RTCP RR every 5 seconds (50 pings at 100ms) */
        if ((seq % 50) == 0) {
            send_rtcp_rr();
        }

        /* Phase 5.8: Session resume timeout check */
        if (s_session_state == SESSION_STATE_WIFI_LOST ||
            s_session_state == SESSION_STATE_RECONNECTING) {
            u32 lost_elapsed = sceKernelGetSystemTimeLow() - s_wifi_lost_time_us;
            if (lost_elapsed > 10000000) { /* 10 seconds */
                net_log("[PHASE5-RESUME] WiFi timeout 10s — aborting session\n");
                extern volatile int g_stream_status;
                g_stream_status = 1;
                me_running = 0;
            }
        }

        sceKernelDelayThread(100000); /* 100 ms — aggressive for low-latency stall detection */
    }

    net_log("[PING] thread exiting (me_running=%d)\n", me_running);
    sceKernelExitDeleteThread(0);
    return 0;
}

/*--------------------------------------------------------------------------
 * network_me_abort - Force-unblock recv/ping threads by closing the socket.
 *
 * Called from the exit callback when the user presses Home.  Closing the
 * socket causes any blocking sceNetInetRecvfrom() to return immediately
 * with EBADF, so threads can check me_running and exit.
 *--------------------------------------------------------------------------*/
void network_me_abort(void)
{
    if (udp_socket >= 0) {
        sceNetInetClose(udp_socket);
        udp_socket = -1;
    }
    if (udp_socket_rtcp >= 0) {
        sceNetInetClose(udp_socket_rtcp);
        udp_socket_rtcp = -1;
    }
}

/*--------------------------------------------------------------------------
 * network_recv_thread - Receive loop (proper PSP thread signature)
 *--------------------------------------------------------------------------*/
static int network_recv_thread(SceSize args, void *argp)
{
    PacketRingBuffer *rb = *((PacketRingBuffer **)argp);
    u8 recv_buf[MAX_PACKET_SIZE];
    struct sockaddr_in from_addr;
    socklen_t from_len;
    u32 pkt_count = 0;
    u32 timeout_count = 0;
    const u32 MAX_ZERO_TIMEOUTS = 30;

    net_log("[NET] recv thread running\n");
    net_log("[NET] packet ring slots=%u (packet size=%u)\n", RING_BUFFER_SLOTS, MAX_PACKET_SIZE);

    u32 recv_hb_last = sceKernelGetSystemTimeLow() / 1000000;
    u32 recv_err_count = 0;
    u32 recv_total_timeout = 0;

    /* Log actual bound address so we can verify Sunshine is sending here */
    {
        struct sockaddr_in my_addr;
        socklen_t my_len = sizeof(my_addr);
        memset(&my_addr, 0, sizeof(my_addr));
        if (sceNetInetGetsockname(udp_socket, (struct sockaddr *)&my_addr, &my_len) == 0) {
            net_log("[NET] recv socket bound to %s:%u\n",
                    inet_ntoa(my_addr.sin_addr), (unsigned)ntohs(my_addr.sin_port));
        } else {
            net_log("[NET] getsockname failed errno=%d\n", sceNetInetGetErrno());
        }
    }

    while (me_running) {
        ssize_t n;
        u32 next_head;

        from_len = sizeof(from_addr);
        memset(&from_addr, 0, sizeof(from_addr));
        from_addr.sin_len = (uint8_t)sizeof(from_addr); /* PSP BSD socket */

        /* Blocking recvfrom with SO_RCVTIMEO (2 s, set in network_me_init).
         *
         * sceNetInetSelect() is broken in PPSSPP and unreliable on real PSP
         * hardware — it returns 0 (timeout) immediately without waiting,
         * which means recvfrom is never called and no packets are received.
         *
         * The control-stream handshake proves that blocking sceNetInetRecv
         * with SO_RCVTIMEO *does* work on PSP.  On timeout, recvfrom
         * returns -1 with errno EAGAIN/EWOULDBLOCK, letting us check
         * me_running each iteration. */
        n = sceNetInetRecvfrom(udp_socket, recv_buf, sizeof(recv_buf),
                               0, (struct sockaddr *)&from_addr, &from_len);

        if (n <= 0) {
            int err = sceNetInetGetErrno();
            if (err == EAGAIN || err == EWOULDBLOCK || err == 0) {
                /* SO_RCVTIMEO fired — normal timeout.
                 * PPSSPP may not honor the 2s SO_RCVTIMEO and return
                 * immediately, causing this thread to spin at ~1700/sec.
                 * Yield to prevent CPU starvation of other threads. */
                timeout_count++;
                recv_total_timeout++;
                if (pkt_count == 0) {
                    /* Only log sparingly: first 3, then every 1000th */
                    if (timeout_count <= 3 || (timeout_count % 1000) == 0) {
                        net_log("[NET] no packets yet (timeout#%u/%u)\n",
                                timeout_count, MAX_ZERO_TIMEOUTS);
                    }
                    if (timeout_count >= MAX_ZERO_TIMEOUTS
                        && (timeout_count % 5000) == 0) {
                        net_log("[NET] no video after %u timeouts — pings running, still waiting\n",
                                timeout_count);
                    }
                }
                /* Yield CPU for 2ms to prevent spinning when PPSSPP
                 * returns EAGAIN immediately instead of blocking. */
                sceKernelDelayThread(2000);
            } else {
                recv_err_count++;
                net_log("[NET] recv err=%d count=%u\n", err, recv_err_count);
            }
            /* Recv heartbeat every 5 seconds (during no-data periods) */
            {
                u32 now_s = sceKernelGetSystemTimeLow() / 1000000;
                if (now_s - recv_hb_last >= 5) {
                    int ap_state = -1;
                    extern int g_decoder_ready;
                    u32 rq = (rb->head + RING_BUFFER_SLOTS - rb->tail) % RING_BUFFER_SLOTS;
                    sceNetApctlGetState(&ap_state);
                    net_log("[NET] RECV_IDLE pkts=%u q=%u dec_rdy=%d to=%u err=%u ap=%d\n",
                            pkt_count, rq, g_decoder_ready,
                            recv_total_timeout, recv_err_count, ap_state);
                    recv_hb_last = now_s;
                }
            }
            continue;
        }

        pkt_count++;
        if (pkt_count == 1) {
            net_log("[NET] FIRST packet: %d bytes from %s:%u\n",
                    (int)n, inet_ntoa(from_addr.sin_addr),
                    (unsigned)ntohs(from_addr.sin_port));
            {
                char _hex[64]; int _h, _max = (int)n < 16 ? (int)n : 16;
                for (_h = 0; _h < _max; _h++)
                    snprintf(_hex + _h * 3, 4, "%02X ", recv_buf[_h]);
                net_log("[NET] first bytes: %s\n", _hex);
            }
        } else if (pkt_count == 10 || pkt_count == 50 || pkt_count == 100) {
            net_log("[NET] pkt_count=%u from %s:%u\n",
                    pkt_count, inet_ntoa(from_addr.sin_addr),
                    (unsigned)ntohs(from_addr.sin_port));
        }

        /* Periodic recv heartbeat (fires even when data flowing — every 10s) */
        {
            u32 now_s = sceKernelGetSystemTimeLow() / 1000000;
            if (now_s - recv_hb_last >= 10) {
                extern int g_decoder_ready;
                u32 rq = (rb->head + RING_BUFFER_SLOTS - rb->tail) % RING_BUFFER_SLOTS;
                net_log("[NET] RECV_HB pkts=%u q=%u dec_rdy=%d to=%u err=%u\n",
                        pkt_count, rq, g_decoder_ready,
                        recv_total_timeout, recv_err_count);
                recv_hb_last = now_s;
            }
        }

        if ((u32)n > MAX_PACKET_SIZE)
            continue;

        /* Phase 4: Jitter tracking — measure inter-packet arrival times */
        {
            static u32 s_last_recv_us = 0;
            u32 recv_now = sceKernelGetSystemTimeLow();
            if (s_last_recv_us != 0) {
                u32 delta = recv_now - s_last_recv_us;
                signal_strength_report_jitter(delta);
            }
            s_last_recv_us = recv_now;
        }

        /* Phase 5.2: Update RTCP receiver report statistics */
        {
            s_rtcp_pkts_received++;
            if ((u32)n >= 12) {
                u16 rtp_seq = (u16)((recv_buf[2] << 8) | recv_buf[3]);
                u32 ssrc = (u32)((recv_buf[8] << 24) | (recv_buf[9] << 16) |
                                  (recv_buf[10] << 8) | recv_buf[11]);
                s_rtcp_ssrc = ssrc;
                if (s_rtcp_have_last_seq) {
                    int gap = (int)(u16)(rtp_seq - s_rtcp_last_seq) - 1;
                    if (gap > 0 && gap < 1000)
                        s_rtcp_pkts_lost_cum += (u32)gap;
                }
                if (rtp_seq > (u16)s_rtcp_highest_seq ||
                    ((u16)s_rtcp_highest_seq > 0xF000 && rtp_seq < 0x1000)) {
                    s_rtcp_highest_seq = rtp_seq;
                }
                s_rtcp_last_seq = rtp_seq;
                s_rtcp_have_last_seq = 1;
                /* Inter-arrival jitter EWMA (microseconds) */
                {
                    u32 now_us = sceKernelGetSystemTimeLow();
                    if (s_rtcp_last_arrival_us != 0) {
                        u32 delta = now_us - s_rtcp_last_arrival_us;
                        int diff = (int)delta - (int)s_rtcp_jitter_us;
                        if (diff < 0) diff = -diff;
                        s_rtcp_jitter_us = s_rtcp_jitter_us +
                                           ((u32)diff - s_rtcp_jitter_us + 8) / 16;
                    }
                    s_rtcp_last_arrival_us = now_us;
                }
            }
        }

        /* Phase 4: Packet priority detection for admission control.
         * IDR (NRI=3): CRITICAL, SOF: HIGH, body: MEDIUM, FEC: LOW */
        {
            int pkt_priority = 1; /* default: medium */
            if ((u32)n >= NET_RTP_FIXED_HEADER_SIZE + NET_NV_VIDEO_PKT_SIZE) {
                int nv_off = net_rtp_nv_offset(recv_buf, (int)n);
                u8 nv_flags;
                u32 nv_fec_info;
                u32 nv_data_pkts;
                u32 nv_fec_idx;

                if (nv_off >= 0 && (u32)n >= (u32)(nv_off + NET_NV_VIDEO_PKT_SIZE)) {
                    nv_flags = recv_buf[nv_off + 8];
                    nv_fec_info = (u32)recv_buf[nv_off + 12] |
                                  ((u32)recv_buf[nv_off + 13] << 8) |
                                  ((u32)recv_buf[nv_off + 14] << 16) |
                                  ((u32)recv_buf[nv_off + 15] << 24);
                    nv_data_pkts = (nv_fec_info >> 22) & 0x3FF;
                    nv_fec_idx = (nv_fec_info >> 12) & 0x3FF;

                    /* NAL NRI check for IDR detection */
                    {
                        int payload_off = nv_off + NET_NV_VIDEO_PKT_SIZE;
                        if ((u32)n > (u32)(payload_off + 4)) {
                            int nal_off = payload_off;
                            if (recv_buf[payload_off] == 0x01)
                                nal_off = payload_off + 8;
                            else if ((u8)recv_buf[payload_off] == 0x81)
                                nal_off = payload_off + 44;
                            if ((u32)n > (u32)(nal_off + 5) &&
                                recv_buf[nal_off] == 0x00 && recv_buf[nal_off+1] == 0x00 &&
                                recv_buf[nal_off+2] == 0x00 && recv_buf[nal_off+3] == 0x01) {
                                u8 nri = (recv_buf[nal_off + 4] >> 5) & 0x03;
                                if (nri == 3) pkt_priority = 3; /* IDR */
                            }
                        }
                    }

                    /* SOF detection */
                    if (nv_flags & 0x04)
                        if (pkt_priority < 2) pkt_priority = 2;

                    /* FEC parity (fec_index >= data_packets) */
                    if (nv_data_pkts > 0 && nv_fec_idx >= nv_data_pkts)
                        pkt_priority = 0;
                }
            }

            /* Phase 4: Priority-based admission control */
            {
                u32 ring_used = (rb->head + RING_BUFFER_SLOTS - rb->tail) % RING_BUFFER_SLOTS;
                u32 ring_pct = (ring_used * 100) / RING_BUFFER_SLOTS;

                if (ring_pct > 90 && pkt_priority <= 0) {
                    static u32 s_prio_drop_count = 0;
                    s_prio_drop_count++;
                    if (s_prio_drop_count <= 3 || (s_prio_drop_count % 100) == 0)
                        net_log("[PHASE4-PRIO] dropped FEC parity (ring %u%%) [#%u]\n",
                                ring_pct, s_prio_drop_count);
                    continue;
                }
                if (ring_pct > 95 && pkt_priority <= 1) {
                    static u32 s_prio_med_drop = 0;
                    s_prio_med_drop++;
                    if (s_prio_med_drop <= 3 || (s_prio_med_drop % 100) == 0)
                        net_log("[PHASE4-PRIO] dropped P-body (ring %u%%) [#%u]\n",
                                ring_pct, s_prio_med_drop);
                    continue;
                }
            }
        }

        /* Video wire format: [RTP(12+ext)][NV_VIDEO_PACKET(16)][H264 data].
         * Decryption (if any) is handled downstream in FEC/reassembly. */

        next_head = (rb->head + 1) % RING_BUFFER_SLOTS;

        if (next_head == rb->tail) {
            /* Ring full — drop the incoming packet.
             *
             * CRITICAL: Never modify rb->tail from this thread while the
             * decoder (consumer) thread is ALIVE!  Both threads writing to
             * rb->tail is a data race that corrupts SPSC ring buffer
             * invariants and causes hard crashes on real PSP hardware.
             *
             * EXCEPTION: When the decoder is confirmed dead (g_decoder_ready==0),
             * no consumer exists, so the producer can safely flush.  Without
             * this emergency flush, a dead decoder → permanently full ring →
             * all packets (including IDR) dropped → unrecoverable deadlock.
             *
             * Reset the drop counter after flush so it doesn't grow unbounded. */
            {
                static u32 s_drop_count = 0;
                s_drop_count++;
                if (s_drop_count <= 3 || (s_drop_count % 200) == 0)
                    net_log("[NET] ring FULL — dropped pkt #%u\n", s_drop_count);

                /* Phase 5.3: Intermediate IDR request at 256 consecutive drops.
                 * Don't flush the ring (unsafe while decoder alive), just
                 * request a fresh keyframe so decoder can catch up. */
                if (s_drop_count == 256) {
                    net_log("[PHASE5-RING] intermediate flush at %u drops, requesting IDR\n",
                            s_drop_count);
                    control_stream_request_idr();
                }

                /* Emergency flush: decoder confirmed dead, ring permanently stuck.
                 * 2000 consecutive drops ≈ 6-7 seconds at 300 pkt/s.
                 * Safe because no consumer thread exists (g_decoder_ready == 0). */
                {
                    extern int g_decoder_ready;
                    if (s_drop_count >= 2000 && !g_decoder_ready) {
                        net_log("[NET] EMERGENCY: decoder dead, flushing ring (%u drops)\n",
                                s_drop_count);
                        rb->tail = rb->head;
                        s_drop_count = 0;
                        control_stream_request_idr();
                    }
                }
            }
            continue;
        }

        memcpy(rb->slots[rb->head], recv_buf, (u16)n);
        rb->slot_length[rb->head] = (u16)n;
        rb->head = next_head;
        sw_decoder_thread_wakeup();
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

/*--------------------------------------------------------------------------
 * network_me_shutdown - Stop receive thread and close socket
 *--------------------------------------------------------------------------*/
void network_me_shutdown(void)
{
    SceUInt timeout_us = 2000000; /* 2 seconds max wait per thread */

    /* Signals threads to exit loop */
    me_running = 0;

    /* Safely close the sockets FIRST to force threads to return from blocking calls.
     * On PSP, closing a socket while a thread is in recvfrom() will eventually 
     * cause it to return with an error, whereas sending a WAKE packet depends 
     * on localhost stability. */
    if (udp_socket >= 0) {
        sceNetInetClose(udp_socket);
        udp_socket = -1;
    }
    if (udp_socket_rtcp >= 0) {
        sceNetInetClose(udp_socket_rtcp);
        udp_socket_rtcp = -1;
    }

    if (ping_thread_id >= 0) {
        sceKernelWaitThreadEnd(ping_thread_id, &timeout_us);
        sceKernelDeleteThread(ping_thread_id);
        ping_thread_id = -1;
    }

    if (net_thread_id >= 0) {
        sceKernelWaitThreadEnd(net_thread_id, &timeout_us);
        sceKernelDeleteThread(net_thread_id);
        net_thread_id = -1;
    }

    /* Restore WiFi power save to its original state */
    if (g_orig_wlan_powersave >= 0) {
        sceUtilitySetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
                                    g_orig_wlan_powersave);
        net_log("[NET SHUTDOWN] WLAN power save restored to %d\n",
                g_orig_wlan_powersave);
        g_orig_wlan_powersave = -1;
    }
}
