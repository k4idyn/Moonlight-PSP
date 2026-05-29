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
#include "runtime_telemetry.h"
#include "network_me_stats.h"

/* RTP packet parsing constants (for SPS detection in ring overflow handling) */
#define NET_RTP_FIXED_HEADER_SIZE 12
#define NET_RTP_FLAG_EXTENSION    0x10
#define NET_NV_VIDEO_PKT_SIZE     16

static int net_rtp_nv_offset(const u8 *packet, int packet_len)
{
    int off = NET_RTP_FIXED_HEADER_SIZE;
    if (!packet || packet_len < off)
        return -1;
    if (packet[0] & NET_RTP_FLAG_EXTENSION) {
        if (packet_len < off + 4)
            return -1;
        off += 4;
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
static volatile u32 s_video_ping_seq = 0;

static u32 next_video_ping_seq(void)
{
    u32 next = s_video_ping_seq + 1;
    s_video_ping_seq = next;
    return next;
}

/* Thread IDs */
static SceUID net_thread_id  = -1;
static SceUID ping_thread_id = -1;
static SceUID preplay_ping_thread_id = -1;
static volatile int preplay_ping_running = 0;
static u32 s_inline_ping_next_us = 0;
static u32 s_inline_ping_consec_sendfail = 0;
static int s_inline_ping_src_logged = 0;

/* WiFi power save: original state saved at init, restored at shutdown.
 * -1 = not yet read (don't restore). */
static int g_orig_wlan_powersave = -1;

/* Effective SO_RCVBUF cap observed at init. Dynamic tuning never targets
 * above this, which avoids repeated no-op/failed growth attempts on
 * low-memory PSP-1000 sessions. */
static int g_rcvbuf_cap = 128 * 1024;
static int g_runtime_rcvbuf_target = 0;

/* Keep video SS_PING at the documented low-work cadence once the RTSP startup
 * bursts have established routing. Control ENet still runs its own heartbeat;
 * this socket only needs to keep Sunshine's media endpoint fresh. */
#define VIDEO_PING_INTERVAL_US       500000U
#define VIDEO_PING_WIFI_CHECK_TICKS  2U   /* 1 second */
#define VIDEO_PING_SIGNAL_TICKS      4U   /* 2 seconds */
#define VIDEO_PING_RTCP_TICKS        10U  /* 5 seconds */
#define VIDEO_PING_ENOBUFS_TICKS     20U  /* 10 seconds */
#define VIDEO_RECV_THREAD_PRIORITY   0x12

/* PSP low-work mode: keep local RTCP loss/jitter telemetry, but avoid sending
 * video Receiver Reports to the host unless a test build explicitly enables
 * them. The control heartbeat and video SS_PING keep the session alive. */
#ifndef PSP_VIDEO_RTCP_RR_ENABLE
#define PSP_VIDEO_RTCP_RR_ENABLE     0
#endif

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
static u32 s_rtcp_base_ext_seq = 0;
static u32 s_rtcp_highest_ext_seq = 0;
static u32 s_rtcp_jitter_rtp = 0;       /* RFC3550 jitter in 90kHz RTP units */
static u32 s_rtcp_last_arrival_us = 0;
static u32 s_rtcp_last_rtp_timestamp = 0;
static u32 s_rtcp_ssrc = 0;
static u32 s_rtcp_last_ext_seq = 0;
static u32 s_rtcp_prev_rr_received = 0;
static u32 s_rtcp_prev_rr_lost_cum = 0;
static int s_rtcp_have_last_seq = 0;
static volatile u32 s_rtcp_last_interval_expected = 0;
static volatile u32 s_rtcp_last_interval_lost = 0;
static volatile u32 s_rtcp_last_fraction_lost_x10 = 0;

static u32 rtcp_us_to_rtp90k(u32 delta_us)
{
    return (u32)((((u64)delta_us) * 90000ULL + 500000ULL) / 1000000ULL);
}

static u32 rtcp_extend_seq(u16 seq, u32 reference_ext_seq)
{
    u32 candidate = (reference_ext_seq & 0xFFFF0000u) | seq;

    if (candidate + 0x8000u < reference_ext_seq) {
        candidate += 0x10000u;
    } else if (candidate > reference_ext_seq + 0x8000u && candidate >= 0x10000u) {
        candidate -= 0x10000u;
    }

    return candidate;
}

static u32 rtcp_fraction_lost_x10(u32 interval_lost, u32 expected_interval)
{
    if (expected_interval == 0) {
        return 0;
    }
    return (u32)(((u64)interval_lost * 1000ULL) / (u64)expected_interval);
}

void network_me_get_rtcp_stats(NetworkRtcpStats *out)
{
    u32 total_received;
    u32 total_lost;
    u32 interval_received;
    u32 interval_lost;
    u32 expected_interval;

    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    total_received = s_rtcp_pkts_received;
    if (s_rtcp_have_last_seq && s_rtcp_highest_ext_seq >= s_rtcp_base_ext_seq) {
        u32 expected_total = s_rtcp_highest_ext_seq - s_rtcp_base_ext_seq + 1;
        total_lost = (expected_total > total_received) ?
                     (expected_total - total_received) : 0;
    } else {
        total_lost = 0;
    }
    s_rtcp_pkts_lost_cum = total_lost;
    interval_received = total_received - s_rtcp_prev_rr_received;
    interval_lost = (total_lost >= s_rtcp_prev_rr_lost_cum) ?
                    (total_lost - s_rtcp_prev_rr_lost_cum) : 0;
    expected_interval = interval_received + interval_lost;

    out->packets_received = total_received;
    out->packets_lost_cum = total_lost;
    out->highest_ext_seq = s_rtcp_highest_ext_seq;
    out->jitter_rtp90k = s_rtcp_jitter_rtp;
    out->interval_expected = expected_interval;
    out->interval_lost = interval_lost;
    out->fraction_lost_x10 = expected_interval ?
        rtcp_fraction_lost_x10(interval_lost, expected_interval) :
        s_rtcp_last_fraction_lost_x10;
}

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

    /* Step 2: Reconnect to AP slot 1. The APCTL state can report 0 before
     * the driver is ready to accept another Connect call; hardware logs show
     * immediate reconnects failing with 0x80110601 and leaving sockets at
     * errno 125. Give the state machine a short settle window, then re-init
     * APCTL once if Connect still fails. */
    sceKernelDelayThread(750 * 1000);
    ret = sceNetApctlConnect(1);
    if (ret < 0) {
        int init_ret;
        net_log("[WIFI] sceNetApctlConnect(1) failed 0x%08X, reinitializing APCTL\n",
                (unsigned)ret);
        sceNetApctlTerm();
        sceKernelDelayThread(250 * 1000);
        init_ret = sceNetApctlInit(0x2000, 42);
        net_log("[WIFI] sceNetApctlInit ret=0x%08X\n", (unsigned)init_ret);
        if (init_ret < 0 && init_ret != (int)0x80410B01) {
            g_wifi_reconnecting = 0;
            return -1;
        }
        sceKernelDelayThread(500 * 1000);
        ret = sceNetApctlConnect(1);
        if (ret < 0) {
            net_log("[WIFI] sceNetApctlConnect(1) retry failed 0x%08X\n",
                    (unsigned)ret);
            g_wifi_reconnecting = 0;
            return -1;
        }
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
#ifdef RETAIL_BUILD
                wifi_try_reconnect();
#else
                net_log("[KEEPALIVE] WiFi disconnected ap_state=%d me_running=%d, reconnecting\n",
                        ap_state, me_running);
                int rc = wifi_try_reconnect();
                net_log("[KEEPALIVE] reconnect result=%d\n", rc);
#endif
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

void wifi_keepalive_abort(void)
{
    g_keepalive_running = 0;
    if (g_keepalive_thread_id >= 0) {
        sceKernelTerminateDeleteThread(g_keepalive_thread_id);
        g_keepalive_thread_id = -1;
    }
}

/* Forward declarations */
static int network_recv_thread(SceSize args, void *argp);
static int network_ping_thread(SceSize args, void *argp);
void network_me_shutdown(void);
void sw_decoder_thread_wakeup(void);
void network_me_set_power_save(int enable);
static void __attribute__((unused)) network_media_periodic_tick(void);

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

static void network_me_apply_preplay_socket_tuning(void)
{
    int rcvbuf = 128 * 1024;
    int sndbuf = 32768;
    int actual_rcvbuf = 0;
    socklen_t actual_len = sizeof(actual_rcvbuf);

#ifndef SO_SNDBUF
#define SO_SNDBUF 0x1001
#endif

    if (udp_socket < 0) {
        return;
    }

    if (sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                             &rcvbuf, sizeof(rcvbuf)) < 0) {
        net_log("[NET PREP] 128KB pre-PLAY rcvbuf set failed errno=%d\n",
                sceNetInetGetErrno());
    }

    if (sceNetInetGetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                             &actual_rcvbuf, &actual_len) == 0) {
        if (actual_rcvbuf > 0) {
            g_rcvbuf_cap = actual_rcvbuf;
        }
        net_log("[NET PREP] SO_RCVBUF pre-PLAY target=%dKB actual=%dKB\n",
                rcvbuf / 1024, actual_rcvbuf / 1024);
    } else {
        g_rcvbuf_cap = rcvbuf;
    }

    if (g_rcvbuf_cap < (128 * 1024)) {
        g_rcvbuf_cap = 128 * 1024;
    }

    sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_SNDBUF,
                         &sndbuf, sizeof(sndbuf));
}

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
            network_me_apply_preplay_socket_tuning();
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

/* Send a short video ping burst from the pre-bound receive socket so
 * Sunshine/Apollo can lock onto the correct client endpoint during ANNOUNCE
 * and PLAY startup. */
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
            ss_pkt.seq_be = htonl(next_video_ping_seq());
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
            ss_pkt.seq_be = htonl(next_video_ping_seq());
            tx = (int)sceNetInetSendto(udp_socket, &ss_pkt, sizeof(ss_pkt),
                                       0, (struct sockaddr *)&dst, sizeof(dst));
        } else {
            tx = (int)sceNetInetSendto(udp_socket, legacy_ping, 4,
                                       0, (struct sockaddr *)&dst, sizeof(dst));
        }

        if (tx > 0) {
            sent_ok++;
        } else {
            sent_fail++;
#ifndef RETAIL_BUILD
            int err = sceNetInetGetErrno();
            if (sent_fail <= 3 || (sent_fail % 10) == 0) {
                net_log("[PING] burst send failed #%d errno=%d (reason=%s)\n",
                        sent_fail, err, reason ? reason : "n/a");
            }
#endif
        }
        sceKernelDelayThread(30 * 1000); /* 30ms between pings */
    }
    net_log("[PING] burst complete total=%d ok=%d fail=%d (reason: %s)\n",
            count, sent_ok, sent_fail, reason ? reason : "n/a");
}

static int preplay_video_ping_thread(SceSize args, void *argp)
{
    struct sockaddr_in dst;
    SsPingPkt ss_pkt;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    u32 last_seq = 0;
    u32 fails = 0;

    (void)args;
    (void)argp;

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (uint8_t)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_video_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    net_log("[PREPLAY PING] started server=%s:%d payload=[%.16s]\n",
            g_video_server_ip, g_video_server_port, g_video_ping_payload);

    while (preplay_ping_running && udp_socket >= 0) {
        int sent;
        int err;

        u32 seq = next_video_ping_seq();
        last_seq = seq;
        if (g_video_ping_payload[0] != '\0') {
            memcpy(ss_pkt.payload, g_video_ping_payload, 16);
            ss_pkt.seq_be = htonl(seq);
            sent = (int)sceNetInetSendto(udp_socket, &ss_pkt, sizeof(ss_pkt),
                                         0, (struct sockaddr *)&dst,
                                         sizeof(dst));
        } else {
            sent = (int)sceNetInetSendto(udp_socket, legacy_ping,
                                         sizeof(legacy_ping), 0,
                                         (struct sockaddr *)&dst,
                                         sizeof(dst));
        }

        err = (sent < 0) ? sceNetInetGetErrno() : 0;
        if (seq <= 5 || err) {
            net_log("[PREPLAY PING] #%u sent=%d%s%d\n",
                    seq, sent, err ? " errno=" : "", err);
        }
        if (err) {
            fails++;
        }

        sceKernelDelayThread(500 * 1000);
    }

    net_log("[PREPLAY PING] exiting seq=%u fails=%u\n", last_seq, fails);
    sceKernelExitDeleteThread(0);
    return 0;
}

int network_me_start_preplay_pings(void)
{
    if (preplay_ping_thread_id >= 0) {
        return 0;
    }
    if (udp_socket < 0 || g_video_server_ip[0] == '\0') {
        net_log("[PREPLAY PING] unavailable socket=%d server='%s'\n",
                udp_socket, g_video_server_ip);
        return -1;
    }

    preplay_ping_running = 1;
    preplay_ping_thread_id = sceKernelCreateThread(
        "preplay_ping",
        preplay_video_ping_thread,
        0x18, 0x1000, PSP_THREAD_ATTR_USER, NULL);
    if (preplay_ping_thread_id < 0) {
        net_log("[PREPLAY PING] create failed %d\n", preplay_ping_thread_id);
        preplay_ping_running = 0;
        return -1;
    }

    sceKernelStartThread(preplay_ping_thread_id, 0, NULL);
    return 0;
}

void network_me_stop_preplay_pings(void)
{
    SceUInt timeout_us = 1000000;

    if (preplay_ping_thread_id < 0) {
        preplay_ping_running = 0;
        return;
    }

    preplay_ping_running = 0;
    sceKernelWaitThreadEnd(preplay_ping_thread_id, &timeout_us);
    sceKernelDeleteThread(preplay_ping_thread_id);
    preplay_ping_thread_id = -1;
}

/* net_log already defined at top of file via diag_log.h */

/*--------------------------------------------------------------------------
 * Phase 5.2: Update RTCP window stats and optionally send Receiver Report
 *--------------------------------------------------------------------------*/
static void send_rtcp_rr(void)
{
    u32 frac_lost = 0;
    u32 total_received;
    u32 total_lost;
    u32 interval_received;
    u32 interval_lost;
    u32 expected_interval;
    u32 cum_lost;
#if PSP_VIDEO_RTCP_RR_ENABLE
    unsigned char buf[32];
    struct sockaddr_in rtcp_dst;
#endif

    total_received = s_rtcp_pkts_received;
    if (s_rtcp_have_last_seq && s_rtcp_highest_ext_seq >= s_rtcp_base_ext_seq) {
        u32 expected_total = s_rtcp_highest_ext_seq - s_rtcp_base_ext_seq + 1;
        total_lost = (expected_total > total_received) ?
                     (expected_total - total_received) : 0;
    } else {
        total_lost = 0;
    }
    s_rtcp_pkts_lost_cum = total_lost;
    interval_received = total_received - s_rtcp_prev_rr_received;
    interval_lost = (total_lost >= s_rtcp_prev_rr_lost_cum) ?
                    (total_lost - s_rtcp_prev_rr_lost_cum) : 0;
    expected_interval = interval_received + interval_lost;

    if (expected_interval > 0) {
        frac_lost = (interval_lost * 256) / expected_interval;
    }
    if (frac_lost > 255) frac_lost = 255;
    cum_lost = total_lost;
    if (cum_lost > 0x7FFFFFU) {
        cum_lost = 0x7FFFFFU;
    }
    s_rtcp_last_interval_expected = expected_interval;
    s_rtcp_last_interval_lost = interval_lost;
    s_rtcp_last_fraction_lost_x10 =
        rtcp_fraction_lost_x10(interval_lost, expected_interval);

#if PSP_VIDEO_RTCP_RR_ENABLE
    if (udp_socket_rtcp < 0 || g_video_server_ip[0] == '\0') return;

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
    buf[16] = (u8)(s_rtcp_highest_ext_seq >> 24); buf[17] = (u8)(s_rtcp_highest_ext_seq >> 16);
    buf[18] = (u8)(s_rtcp_highest_ext_seq >> 8);  buf[19] = (u8)s_rtcp_highest_ext_seq;
    /* Jitter */
    buf[20] = (u8)(s_rtcp_jitter_rtp >> 24); buf[21] = (u8)(s_rtcp_jitter_rtp >> 16);
    buf[22] = (u8)(s_rtcp_jitter_rtp >> 8);  buf[23] = (u8)s_rtcp_jitter_rtp;
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
            net_log("[RTCP] RR send failed errno=%d\n", sceNetInetGetErrno());
        } else {
            s_rtcp_prev_rr_received = total_received;
            s_rtcp_prev_rr_lost_cum = total_lost;
            net_log("[RTCP] sent RR: frac_lost=%u cum_lost=%u jitter90k=%u\n",
                    (unsigned)frac_lost, (unsigned)cum_lost, (unsigned)s_rtcp_jitter_rtp);
        }
    }
#else
    {
        static u32 s_rtcp_suppressed_log_count = 0;
        s_rtcp_prev_rr_received = total_received;
        s_rtcp_prev_rr_lost_cum = total_lost;
        if (s_rtcp_suppressed_log_count < 3 || interval_lost != 0) {
            net_log("[RTCP] video RR suppressed: frac_lost=%u cum_lost=%u jitter90k=%u\n",
                    (unsigned)frac_lost, (unsigned)cum_lost, (unsigned)s_rtcp_jitter_rtp);
            s_rtcp_suppressed_log_count++;
        }
    }
#endif
}

/* Runtime media keepalive runs on the video receive thread.  The audio path
 * already avoids concurrent send/recv on one PSP DGRAM socket; keeping video
 * pings here applies the same rule to the GameStream RTP socket and removes a
 * source of scheduler/socket-lock jitter during 802.11b packet bursts. */
static void __attribute__((unused)) network_media_periodic_tick(void)
{
    struct sockaddr_in dst;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    SsPingPkt ss_pkt;
    int sent;
    int err;
    u32 now = sceKernelGetSystemTimeLow();
    uint32_t seq;

    if (s_inline_ping_next_us != 0 &&
        (int)(now - s_inline_ping_next_us) < 0) {
        return;
    }

    s_inline_ping_next_us = now + VIDEO_PING_INTERVAL_US;

    if (udp_socket < 0 || g_video_server_ip[0] == '\0') {
        return;
    }

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (uint8_t)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_video_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    seq = next_video_ping_seq();
    if (g_video_ping_payload[0] != '\0') {
        memcpy(ss_pkt.payload, g_video_ping_payload, 16);
        ss_pkt.seq_be = htonl(seq);
        sent = (int)sceNetInetSendto(udp_socket, &ss_pkt, sizeof(ss_pkt),
                                     0, (struct sockaddr *)&dst, sizeof(dst));
    } else {
        sent = (int)sceNetInetSendto(udp_socket, legacy_ping, sizeof(legacy_ping),
                                     0, (struct sockaddr *)&dst, sizeof(dst));
    }
    err = (sent < 0) ? sceNetInetGetErrno() : 0;

    if (err) {
        s_inline_ping_consec_sendfail++;
    } else {
        s_inline_ping_consec_sendfail = 0;
    }

    if (seq <= 5 || err) {
        net_log("[PING INLINE] #%u -> %s:%d sent=%d%s%d\n",
                seq, g_video_server_ip, g_video_server_port, sent,
                err ? " SENDFAIL errno=" : "", err);
    }

    if (!s_inline_ping_src_logged && seq >= 5) {
        struct sockaddr_in my_addr;
        socklen_t my_len = sizeof(my_addr);
        memset(&my_addr, 0, sizeof(my_addr));
        if (sceNetInetGetsockname(udp_socket,
                                  (struct sockaddr *)&my_addr, &my_len) == 0) {
            net_log("[PING INLINE] src-check: %s:%u me_running=%d\n",
                    inet_ntoa(my_addr.sin_addr),
                    (unsigned)ntohs(my_addr.sin_port), me_running);
        }
        s_inline_ping_src_logged = 1;
    }

    /* WiFi state check every 1 second. Force reconnect after about
     * 10 seconds of consecutive SENDFAILs to catch ENOBUFS stalls. */
    if ((seq % VIDEO_PING_WIFI_CHECK_TICKS) == 0 ||
        s_inline_ping_consec_sendfail == VIDEO_PING_ENOBUFS_TICKS) {
        int ap_state = -1;
        int force_reconnect =
            (s_inline_ping_consec_sendfail >= VIDEO_PING_ENOBUFS_TICKS);
        sceNetApctlGetState(&ap_state);

        if (ap_state != 4 || force_reconnect) {
            net_log("[PING INLINE] %s ap_state=%d at ping#%u sendfail=%u\n",
                    force_reconnect ? "WIFI_ENOBUFS" : "WIFI_DOWN",
                    ap_state, seq, s_inline_ping_consec_sendfail);

            if (s_session_state == SESSION_STATE_ACTIVE) {
                s_session_state = SESSION_STATE_WIFI_LOST;
                s_wifi_lost_time_us = sceKernelGetSystemTimeLow();
                net_log("[RESUME] session state: WIFI_LOST\n");
                network_me_set_power_save(1);
            }

            if (!g_wifi_reconnecting) {
                s_session_state = SESSION_STATE_RECONNECTING;
                net_log("[RESUME] session state: RECONNECTING\n");
                if (wifi_try_reconnect() == 0) {
                    net_log("[PING INLINE] WiFi restored at ping#%u, resuming\n",
                            seq);
                    s_inline_ping_consec_sendfail = 0;
                    s_session_state = SESSION_STATE_RESUMED;
                    net_log("[RESUME] session state: RESUMED\n");
                    network_me_set_power_save(0);
                    send_ping_burst_internal(3, "wifi_reconnect");
                    control_stream_request_idr();
                    s_session_state = SESSION_STATE_ACTIVE;
                } else {
                    net_log("[PING INLINE] WiFi reconnect failed, will retry\n");
                }
            }
        }
    }

    /* Dynamic SO_RCVBUF: compute the quality target every 5 seconds, but
     * only touch the live UDP socket if the capped target actually changes. */
    if ((seq % VIDEO_PING_RTCP_TICKS) == 0 && udp_socket >= 0) {
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

        if (g_runtime_rcvbuf_target <= 0)
            g_runtime_rcvbuf_target = g_rcvbuf_cap;

        if (target_rcvbuf != g_runtime_rcvbuf_target) {
            int actual_rcvbuf = 0;
            socklen_t actual_len = sizeof(actual_rcvbuf);
            net_log("[RCVBUF] %dKB -> %dKB (quality=%d)\n",
                    g_runtime_rcvbuf_target / 1024,
                    target_rcvbuf / 1024,
                    (int)cq.quality);
            sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                 &target_rcvbuf, sizeof(target_rcvbuf));
            g_runtime_rcvbuf_target = target_rcvbuf;
            if (sceNetInetGetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                     &actual_rcvbuf, &actual_len) == 0) {
                net_log("[RCVBUF] applied=%dKB\n", actual_rcvbuf / 1024);
            }
        }
    }

    if ((seq % VIDEO_PING_SIGNAL_TICKS) == 0) {
        SignalTrend trend = signal_strength_get_trend();
        if (trend == SIGNAL_TREND_DEGRADING) {
            static u32 s_proactive_idr_count = 0;
            s_proactive_idr_count++;
            if (s_proactive_idr_count <= 3 ||
                (s_proactive_idr_count % 10) == 0) {
                net_log("[WIFI] signal degrading, proactive IDR #%u\n",
                        s_proactive_idr_count);
            }
            control_stream_request_idr();
        }
    }

    if ((seq % VIDEO_PING_RTCP_TICKS) == 0) {
        send_rtcp_rr();
    }

    if (s_session_state == SESSION_STATE_WIFI_LOST ||
        s_session_state == SESSION_STATE_RECONNECTING) {
        u32 lost_elapsed = sceKernelGetSystemTimeLow() - s_wifi_lost_time_us;
        if (lost_elapsed > 10000000) {
            net_log("[RESUME] WiFi timeout 10s - aborting session\n");
            extern volatile int g_stream_status;
            g_stream_status = 1;
            me_running = 0;
        }
    }
}

/*--------------------------------------------------------------------------
 * Phase 5.5: WiFi power save management
 *--------------------------------------------------------------------------*/
void network_me_set_power_save(int enable)
{
#ifdef RETAIL_BUILD
    sceUtilitySetSystemParamInt(
        PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
        enable ? PSP_SYSTEMPARAM_WLAN_POWERSAVE_ON
               : PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF);
#else
    int ps_ret = sceUtilitySetSystemParamInt(
        PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
        enable ? PSP_SYSTEMPARAM_WLAN_POWERSAVE_ON
               : PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF);
    net_log("[WIFIPS] power save %s (ret=%d)\n",
            enable ? "ON" : "OFF", ps_ret);
#endif
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
#ifndef RETAIL_BUILD
    int prebound = (udp_socket >= 0);
#endif

    /* If already running, shut down first to ensure clean state */
    if (net_thread_id >= 0 || ping_thread_id >= 0) {
        network_me_shutdown();
    }

    /* Zero the ring buffer */
    memset(rb, 0, sizeof(PacketRingBuffer));
    s_rtcp_pkts_received = 0;
    s_rtcp_pkts_lost_cum = 0;
    s_rtcp_base_ext_seq = 0;
    s_rtcp_highest_ext_seq = 0;
    s_rtcp_jitter_rtp = 0;
    s_rtcp_last_arrival_us = 0;
    s_rtcp_last_rtp_timestamp = 0;
    s_rtcp_ssrc = 0;
    s_rtcp_last_ext_seq = 0;
    s_rtcp_prev_rr_received = 0;
    s_rtcp_prev_rr_lost_cum = 0;
    s_rtcp_have_last_seq = 0;
    s_rtcp_last_interval_expected = 0;
    s_rtcp_last_interval_lost = 0;
    s_rtcp_last_fraction_lost_x10 = 0;
    s_inline_ping_next_us = 0;
    s_inline_ping_consec_sendfail = 0;
    s_inline_ping_src_logged = 0;

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
#ifndef RETAIL_BUILD
    net_log("[NET INIT] bound local video port %u OK%s\n",
            (unsigned)local_port,
            prebound ? " (pre-bound for RTSP SETUP)" : "");
#endif

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
            g_runtime_rcvbuf_target = g_rcvbuf_cap;
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
#ifndef RETAIL_BUILD
    {
        SceSize max_free = sceKernelMaxFreeMemSize();
        SceSize total_free = sceKernelTotalFreeMemSize();
        net_log("[NET INIT] free mem: max_block=%u total=%u\n",
                (unsigned)max_free, (unsigned)total_free);
    }
#endif

    /* Spawn recv thread. */
    me_running = 1;

    net_thread_id = sceKernelCreateThread(
        "net_recv_thread",
        network_recv_thread,
        VIDEO_RECV_THREAD_PRIORITY, 0x8000, PSP_THREAD_ATTR_USER, NULL);
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
    net_log("[NET INIT] recv thread started id=%d priority=0x%02X\n",
            net_thread_id, VIDEO_RECV_THREAD_PRIORITY);

    ping_thread_id = sceKernelCreateThread(
        "net_ping_thread",
        network_ping_thread,
        0x18, 0x4000, PSP_THREAD_ATTR_USER, NULL);
    if (ping_thread_id < 0) {
        net_log("[NET INIT] ping thread create failed: %d\n", ping_thread_id);
        me_running = 0;
        if (udp_socket >= 0) {
            sceNetInetClose(udp_socket);
            udp_socket = -1;
        }
        if (udp_socket_rtcp >= 0) {
            sceNetInetClose(udp_socket_rtcp);
            udp_socket_rtcp = -1;
        }
        return;
    }
    sceKernelStartThread(ping_thread_id, 0, NULL);
    net_log("[NET INIT] ping thread started id=%d\n", ping_thread_id);

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
        uint32_t seq = next_video_ping_seq();

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

        /* WiFi state check every 1 second. Force reconnect after about
         * 10 seconds of consecutive SENDFAILs to catch ENOBUFS stalls. */
        if ((seq % VIDEO_PING_WIFI_CHECK_TICKS) == 0 ||
            consec_sendfail == VIDEO_PING_ENOBUFS_TICKS) {
            int ap_state = -1;
            int force_reconnect = (consec_sendfail >= VIDEO_PING_ENOBUFS_TICKS);
            sceNetApctlGetState(&ap_state);

            if (ap_state != 4 || force_reconnect) {
                net_log("[PING] %s ap_state=%d at ping#%u sendfail=%u\n",
                        force_reconnect ? "WIFI_ENOBUFS" : "WIFI_DOWN",
                        ap_state, seq, consec_sendfail);

                /* Phase 5.8: Mark session as WiFi lost */
                if (s_session_state == SESSION_STATE_ACTIVE) {
                    s_session_state = SESSION_STATE_WIFI_LOST;
                    s_wifi_lost_time_us = sceKernelGetSystemTimeLow();
                    net_log("[RESUME] session state: WIFI_LOST\n");
                    /* Phase 5.5: Enable power save during WiFi recovery */
                    network_me_set_power_save(1);
                }

                /* Attempt automatic WiFi reconnection */
                if (!g_wifi_reconnecting) {
                    s_session_state = SESSION_STATE_RECONNECTING;
                    net_log("[RESUME] session state: RECONNECTING\n");
                    int rc = wifi_try_reconnect();
                    if (rc == 0) {
                        net_log("[PING] WiFi restored at ping#%u, resuming\n", seq);
                        consec_sendfail = 0;
                        /* Phase 5.8: Resume session */
                        s_session_state = SESSION_STATE_RESUMED;
                        net_log("[RESUME] session state: RESUMED\n");
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

        /* Dynamic SO_RCVBUF: compute the quality target every 5 seconds, but
         * only touch the live UDP socket if the capped target actually changes.
         * On PSP-1000 the cap is usually 128KB, so repeated no-op
         * setsockopt/getsockopt calls are pure receive-path contention. */
        if ((seq % VIDEO_PING_RTCP_TICKS) == 0 && udp_socket >= 0) {
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

            if (g_runtime_rcvbuf_target <= 0)
                g_runtime_rcvbuf_target = g_rcvbuf_cap;

            if (target_rcvbuf != g_runtime_rcvbuf_target) {
                int actual_rcvbuf = 0;
                socklen_t actual_len = sizeof(actual_rcvbuf);
                net_log("[RCVBUF] %dKB -> %dKB (quality=%d)\n",
                        g_runtime_rcvbuf_target / 1024,
                        target_rcvbuf / 1024,
                        (int)cq.quality);
                sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                     &target_rcvbuf, sizeof(target_rcvbuf));
                g_runtime_rcvbuf_target = target_rcvbuf;
                if (sceNetInetGetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                         &actual_rcvbuf, &actual_len) == 0) {
                    net_log("[RCVBUF] applied=%dKB\n", actual_rcvbuf / 1024);
                }
            }
        }

        /* Phase 4: WiFi stutter compensation — proactive IDR on degrading signal */
        if ((seq % VIDEO_PING_SIGNAL_TICKS) == 0) {
            SignalTrend trend = signal_strength_get_trend();
            if (trend == SIGNAL_TREND_DEGRADING) {
                static u32 s_proactive_idr_count = 0;
                s_proactive_idr_count++;
                if (s_proactive_idr_count <= 3 || (s_proactive_idr_count % 10) == 0)
                    net_log("[WIFI] signal degrading, proactive IDR #%u\n",
                            s_proactive_idr_count);
                control_stream_request_idr();
            }
        }

        /* Phase 5.2: Send RTCP RR every 5 seconds. */
        if ((seq % VIDEO_PING_RTCP_TICKS) == 0) {
            send_rtcp_rr();
        }

        /* Phase 5.8: Session resume timeout check */
        if (s_session_state == SESSION_STATE_WIFI_LOST ||
            s_session_state == SESSION_STATE_RECONNECTING) {
            u32 lost_elapsed = sceKernelGetSystemTimeLow() - s_wifi_lost_time_us;
            if (lost_elapsed > 10000000) { /* 10 seconds */
                net_log("[RESUME] WiFi timeout 10s — aborting session\n");
                extern volatile int g_stream_status;
                g_stream_status = 1;
                me_running = 0;
            }
        }

        sceKernelDelayThread(VIDEO_PING_INTERVAL_US);
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
#ifndef RETAIL_BUILD
    u32 timeout_count = 0;
    const u32 MAX_ZERO_TIMEOUTS = 30;
#endif

    net_log("[NET] recv thread running\n");
    net_log("[NET] packet ring slots=%u (packet size=%u)\n", RING_BUFFER_SLOTS, MAX_PACKET_SIZE);

#ifndef RETAIL_BUILD
    u32 recv_hb_last = sceKernelGetSystemTimeLow() / 1000000;
    u32 recv_err_count = 0;
    u32 recv_total_timeout = 0;
#endif
    u32 consecutive_empty = 0;
    u32 last_data_us = sceKernelGetSystemTimeLow();
    u32 last_idle_ping_us = 0;
    int recv_hot_path_disabled = 0;

    /* Log actual bound address so we can verify Sunshine is sending here */
#ifndef RETAIL_BUILD
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
#endif

    while (me_running) {
        ssize_t n;
        u32 next_head;

        /* Legacy note: this path previously used blocking recvfrom with SO_RCVTIMEO.
         *
         * sceNetInetSelect() is broken in PPSSPP and unreliable on real PSP
         * hardware — it returns 0 (timeout) immediately without waiting,
         * which means recvfrom is never called and no packets are received.
         *
         * The control-stream handshake proves that blocking sceNetInetRecv
         * with SO_RCVTIMEO *does* work on PSP.  On timeout, recvfrom
         * returns -1 with errno EAGAIN/EWOULDBLOCK, letting us check
         * me_running each iteration. */
        /* Runtime path is intentionally non-blocking so the video ping thread
         * can keep the host routing media on this same UDP socket. */
        if (pkt_count != 0 && !recv_hot_path_disabled) {
            n = sceNetInetRecv(udp_socket, recv_buf, sizeof(recv_buf),
                               MSG_DONTWAIT);
            if (n <= 0) {
                int hot_err = sceNetInetGetErrno();
                if (hot_err != EAGAIN && hot_err != EWOULDBLOCK &&
                    hot_err != 0) {
                    recv_hot_path_disabled = 1;
                    net_log("[NET] recv hot path disabled errno=%d; using recvfrom\n",
                            hot_err);
                    from_len = sizeof(from_addr);
                    memset(&from_addr, 0, sizeof(from_addr));
                    from_addr.sin_len = (uint8_t)sizeof(from_addr);
                    n = sceNetInetRecvfrom(udp_socket, recv_buf, sizeof(recv_buf),
                                           MSG_DONTWAIT,
                                           (struct sockaddr *)&from_addr, &from_len);
                }
            }
        } else {
            from_len = sizeof(from_addr);
            memset(&from_addr, 0, sizeof(from_addr));
            from_addr.sin_len = (uint8_t)sizeof(from_addr); /* PSP BSD socket */
            n = sceNetInetRecvfrom(udp_socket, recv_buf, sizeof(recv_buf),
                                   MSG_DONTWAIT,
                                   (struct sockaddr *)&from_addr, &from_len);
        }

        if (n <= 0) {
            int err = sceNetInetGetErrno();
            if (err == EAGAIN || err == EWOULDBLOCK || err == 0) {
                u32 now_us = sceKernelGetSystemTimeLow();
                /* SO_RCVTIMEO fired — normal timeout.
                 * PPSSPP may not honor the 2s SO_RCVTIMEO and return
                 * immediately, causing this thread to spin at ~1700/sec.
                 * Yield to prevent CPU starvation of other threads. */
                #ifndef RETAIL_BUILD
                timeout_count++;
                recv_total_timeout++;
                #endif
                consecutive_empty++;
#ifndef RETAIL_BUILD
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
#endif
                if (pkt_count != 0) {
                    u32 idle_us = now_us - last_data_us;
                    if (idle_us >= 2500000u &&
                        (last_idle_ping_us == 0 ||
                         (now_us - last_idle_ping_us) >= 5000000u)) {
                        net_log("[NET] post-start video idle %ums after %u packets; ping burst\n",
                                (unsigned)(idle_us / 1000), pkt_count);
                        send_ping_burst_internal(3, "recv_idle");
                        last_idle_ping_us = sceKernelGetSystemTimeLow();
                    }
                }
                /* Before first media, keep the old conservative backoff.
                 * After video starts, poll more tightly so the PSP Wi-Fi
                 * driver/socket queue is drained before 802.11b bursts spill. */
                {
                    int delay_us;
                    if (pkt_count != 0) {
                        delay_us = (consecutive_empty < 8) ? 250 : 500;
                    } else if (consecutive_empty < 5) {
                        delay_us = 500;
                    } else if (consecutive_empty < 20) {
                        delay_us = 1000;
                    } else {
                        delay_us = 2000;
                    }
                    sceKernelDelayThread(delay_us);
                }
            } else {
#ifndef RETAIL_BUILD
                recv_err_count++;
                net_log("[NET] recv err=%d count=%u\n", err, recv_err_count);
#endif
            }
            /* Recv heartbeat every 5 seconds (during no-data periods) */
#ifndef RETAIL_BUILD
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
#endif
            continue;
        }

        pkt_count++;
        consecutive_empty = 0;
        u32 recv_now_us = sceKernelGetSystemTimeLow();
        last_data_us = recv_now_us;
#ifndef RETAIL_BUILD
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
            net_log("[NET] pkt_count=%u\n", pkt_count);
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

#endif
        if ((u32)n > MAX_PACKET_SIZE)
            continue;

        telemetry_accum_video_rx((u32)n);

        /* Phase 4: Jitter tracking — measure inter-packet arrival times */
        static u32 s_last_recv_us = 0;
        if (s_last_recv_us != 0) {
            u32 delta = recv_now_us - s_last_recv_us;
            signal_strength_report_jitter(delta);
        }
        s_last_recv_us = recv_now_us;

        /* Phase 5.2: Update RTCP receiver report statistics */
        {
            s_rtcp_pkts_received++;
            if ((u32)n >= 12) {
                u16 rtp_seq = (u16)((recv_buf[2] << 8) | recv_buf[3]);
                u32 rtp_timestamp = (u32)((recv_buf[4] << 24) | (recv_buf[5] << 16) |
                                          (recv_buf[6] << 8) | recv_buf[7]);
                u32 ssrc = (u32)((recv_buf[8] << 24) | (recv_buf[9] << 16) |
                                  (recv_buf[10] << 8) | recv_buf[11]);
                u32 ext_seq = s_rtcp_have_last_seq ?
                              rtcp_extend_seq(rtp_seq, s_rtcp_highest_ext_seq) :
                              (u32)rtp_seq;
                s_rtcp_ssrc = ssrc;
                if (!s_rtcp_have_last_seq) {
                    s_rtcp_base_ext_seq = ext_seq;
                    s_rtcp_highest_ext_seq = ext_seq;
                } else if (ext_seq > s_rtcp_highest_ext_seq) {
                    s_rtcp_highest_ext_seq = ext_seq;
                }
                s_rtcp_last_ext_seq = ext_seq;
                s_rtcp_have_last_seq = 1;
                if (s_rtcp_last_arrival_us != 0) {
                    u32 delta = recv_now_us - s_rtcp_last_arrival_us;
                    u32 arrival_delta_rtp = rtcp_us_to_rtp90k(delta);
                    u32 rtp_delta = rtp_timestamp - s_rtcp_last_rtp_timestamp;
                    s64 diff = (s64)arrival_delta_rtp - (s64)rtp_delta;
                    s64 adjust;

                    if (diff < 0) diff = -diff;

                    adjust = diff - (s64)s_rtcp_jitter_rtp;
                    if (adjust >= 0) {
                        s_rtcp_jitter_rtp += (u32)((adjust + 8) / 16);
                    } else {
                        u32 decay = (u32)(((-adjust) + 8) / 16);
                        if (decay > s_rtcp_jitter_rtp)
                            s_rtcp_jitter_rtp = 0;
                        else
                            s_rtcp_jitter_rtp -= decay;
                    }
                }
                s_rtcp_last_arrival_us = recv_now_us;
                s_rtcp_last_rtp_timestamp = rtp_timestamp;
            }
        }

        /* Packet priority detection for admission control */
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

        {
            u32 ring_used = (rb->head + RING_BUFFER_SLOTS - rb->tail) % RING_BUFFER_SLOTS;
            u32 ring_pct = (ring_used * 100) / RING_BUFFER_SLOTS;

            if (ring_pct > 90 && pkt_priority <= 0) {
                static u32 s_prio_drop_count = 0;
                s_prio_drop_count++;
                if (s_prio_drop_count <= 3 || (s_prio_drop_count % 100) == 0)
                    net_log("[PRIO] dropped FEC parity (ring %u%%) [#%u]\n",
                            ring_pct, s_prio_drop_count);
                telemetry_accum_video_drop((u32)n);
                continue;
            }
            if (ring_pct > 95 && pkt_priority <= 1) {
                static u32 s_prio_med_drop = 0;
                s_prio_med_drop++;
                if (s_prio_med_drop <= 3 || (s_prio_med_drop % 100) == 0)
                    net_log("[PRIO] dropped P-body (ring %u%%) [#%u]\n",
                            ring_pct, s_prio_med_drop);
                telemetry_accum_video_drop((u32)n);
                continue;
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
                telemetry_accum_video_drop((u32)n);

                /* Phase 5.3: Intermediate IDR request at 256 consecutive drops.
                 * Don't flush the ring (unsafe while decoder alive), just
                 * request a fresh keyframe so decoder can catch up. */
                if (s_drop_count == 256) {
                    net_log("[RING] intermediate flush at %u drops, requesting IDR\n",
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
        telemetry_accum_video_accept((u32)n);
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

    network_me_stop_preplay_pings();

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
