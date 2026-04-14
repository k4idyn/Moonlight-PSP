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

/* RTP packet parsing constants (for SPS detection in ring overflow handling) */
#define NET_RTP_FIXED_HEADER_SIZE 12
#define NET_RTP_FLAG_EXTENSION    0x10
#define NET_NV_VIDEO_PKT_SIZE     16

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
#define WIFI_RECONNECT_COOLDOWN_US  (3 * 1000 * 1000)  /* 3s min between attempts */
#define WIFI_RECONNECT_MAX_POLL     150                 /* 150 * 100ms = 15s timeout */

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
            while (disc_polls < 50 && (me_running || g_keepalive_running)) { /* 50 * 100ms = 5s max */
                sceNetApctlGetState(&disc_state);
                if (disc_state <= 0)
                    break;
                if (disc_polls == 0 || (disc_polls % 10) == 0)
                    net_log("[WIFI] waiting for disconnect: state=%d poll=%dms\n",
                            disc_state, disc_polls * 100);
                sceKernelDelayThread(100 * 1000); /* 100ms */
                disc_polls++;
            }
            net_log("[WIFI] disconnected in %dms (state=%d)\n",
                    disc_polls * 100, disc_state);
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
    while (attempts < WIFI_RECONNECT_MAX_POLL && (me_running || g_keepalive_running)) {
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
                    info.ip, g_reconnect_count, attempts * 100, g_reconnect_success);
            g_wifi_reconnecting = 0;
            return 0;
        }

        /* Log state transitions (silenced otherwise) */
        if (attempts == 0 || (attempts % 50) == 0) {
            net_log("[WIFI] polling state=%d at %dms\n", state, attempts * 100);
        }

        sceKernelDelayThread(100 * 1000); /* 100ms */
        attempts++;
    }

    net_log("[WIFI] reconnect timeout after %dms (state=%d)\n",
            attempts * 100, state);
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
extern void control_stream_request_idr(void);

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

    for (i = 1; i <= 3; ++i) {
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

/* net_log already defined at top of file via diag_log.h */

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
     * The IDR frame (Frame 0) typically spikes above 80 KB which completely
     * overflows a 64 KB buffer! Set to 256 KB to catch the initial HD burst. */
    {
        int rcvbuf = 256 * 1024;
        int opt_ret = sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                           &rcvbuf, sizeof(rcvbuf));
        if (opt_ret < 0) {
            net_log("[NET INIT] 256KB rcvbuf failed (%d), falling back to 128KB\n", opt_ret);
            rcvbuf = 128 * 1024;
            sceNetInetSetsockopt(udp_socket, SOL_SOCKET, SO_RCVBUF,
                                 &rcvbuf, sizeof(rcvbuf));
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
         * Also force reconnect after 50 consecutive SENDFAILs (~5 seconds)
         * to handle ENOBUFS (errno=105) where WiFi buffers are saturated
         * but ap_state still reports connected. */
        if ((seq % 10) == 0 || consec_sendfail == 50) {
            int ap_state = -1;
            int force_reconnect = (consec_sendfail >= 50);
            sceNetApctlGetState(&ap_state);

            if (ap_state != 4 || force_reconnect) {
                net_log("[PING] %s ap_state=%d at ping#%u sendfail=%u\n",
                        force_reconnect ? "WIFI_ENOBUFS" : "WIFI_DOWN",
                        ap_state, seq, consec_sendfail);

                /* Attempt automatic WiFi reconnection */
                if (!g_wifi_reconnecting) {
                    int rc = wifi_try_reconnect();
                    if (rc == 0) {
                        net_log("[PING] WiFi restored at ping#%u, resuming\n", seq);
                        consec_sendfail = 0;
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

        sceKernelDelayThread(200000); /* 200 ms — reduced from 100ms to ease ENOBUFS */
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