/*
 * host_discovery.c - Host Discovery UI for PSP Moonlight
 *
 * Renders a vertical host list using the shared UIManager layer.
 * Supports:
 *   - D-pad up/down navigation
 *   - Cross to select any non-locked host
 *   - Square to rescan the LAN
 *   - Triangle to add a host manually through the PSP OSK
 *   - Select to send WOL to an offline saved host with a MAC address
 *   - Start to open the exit confirmation dialog
 */

#include <pspkernel.h>
#include <pspctrl.h>
#include <pspthreadman.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <math.h>

#include "host_discovery.h"
#include "config.h"
#include "stream_crypto.h"
#include "client_identity.h"
#include "osk_input.h"
#include "wol.h"
#include "exit_dialog.h"
#include "ui_manager.h"
#include "diag_log.h"

#define MAX_HOSTS            20
#define HOST_LIST_X          20
#define HOST_LIST_Y          52
#define HOST_ITEM_W          440
#define HOST_ITEM_H          38
#define HOST_GAP             6
#define HTTP_PORT            47989
#define CONNECT_TIMEOUT_MS   3000
#define SEND_TIMEOUT_MS      1000
#define RECV_TIMEOUT_MS      2000
#define HTTP_FALLBACK_TIMEOUT_US 1500000
#define PROBE_MIN_DISPLAY_MS 350
#define MAX_RESPONSE_SIZE    2048
#define MAX_VISIBLE_HOSTS    5
#define CLIENT_UNIQUE_ID     client_identity_get_uid()
#define SCAN_CONNECT_TIMEOUT_US 15000    /* 15ms per host (LAN SYN-ACK < 2ms) */

static HostPC g_hosts[MAX_HOSTS];
static int g_host_count = 0;
static int g_selected_index = 0;
static int g_first_visible_index = 0;
static u32 g_prev_buttons = 0;

/* Smooth-scroll animation state (mirrors settings_menu.c) */
static float s_host_scroll_curr   = 0.0f;   /* current camera (lerps)           */
static float s_host_target_camera = 0.0f;   /* integer camera from clamp logic  */
static float s_host_focus_anim    = 0.0f;   /* lerps toward selected for pop    */
static u32   s_host_last_anim_us  = 0;

static void closeProbeTcpSocket(int sock)
{
    if (sock >= 0) {
        struct linger lg = { 1, 0 };
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        sceNetInetClose(sock);
    }
}

static void clampSelectionWindow(void)
{
    if (g_host_count <= 0) {
        g_selected_index = 0;
        g_first_visible_index = 0;
        return;
    }

    if (g_selected_index < 0) {
        g_selected_index = 0;
    } else if (g_selected_index >= g_host_count) {
        g_selected_index = g_host_count - 1;
    }

    if (g_first_visible_index < 0) {
        g_first_visible_index = 0;
    }

    if (g_selected_index < g_first_visible_index) {
        g_first_visible_index = g_selected_index;
    }

    if (g_selected_index >= g_first_visible_index + MAX_VISIBLE_HOSTS) {
        g_first_visible_index = g_selected_index - (MAX_VISIBLE_HOSTS - 1);
    }

    {
        int max_first = g_host_count - MAX_VISIBLE_HOSTS;
        if (max_first < 0) {
            max_first = 0;
        }
        if (g_first_visible_index > max_first) {
            g_first_visible_index = max_first;
        }
    }
}

static int parseDiscoveryResponse(const char *response, int response_len,
                                  char *hostname, int hostname_size)
{
    const char *ptr;
    const char *end;
    char buf[MAX_RESPONSE_SIZE];
    int copy_len;

    if (!response || response_len <= 0) {
        return 0;
    }

    copy_len = response_len < MAX_RESPONSE_SIZE - 1 ? response_len : MAX_RESPONSE_SIZE - 1;
    memcpy(buf, response, copy_len);
    buf[copy_len] = '\0';
    hostname[0] = '\0';

    ptr = strstr(buf, "<Hostname>");
    if (ptr) {
        ptr += 10;
        end = strstr(ptr, "</Hostname>");
        if (end) {
            int len = (int)(end - ptr);
            if (len > 0 && len < hostname_size) {
                strncpy(hostname, ptr, len);
                hostname[len] = '\0';
                return 1;
            }
        }
    }

    ptr = strstr(buf, "<hostname>");
    if (ptr) {
        ptr += 10;
        end = strstr(ptr, "</hostname>");
        if (end) {
            int len = (int)(end - ptr);
            if (len > 0 && len < hostname_size) {
                strncpy(hostname, ptr, len);
                hostname[len] = '\0';
                return 1;
            }
        }
    }

    ptr = strstr(buf, "\"Hostname\"");
    if (ptr) {
        ptr += 10;
        while (*ptr == ' ' || *ptr == ':' || *ptr == '"') {
            ptr++;
        }
        end = ptr;
        while (*end && *end != '"' && *end != ',' && *end != '}') {
            end++;
        }
        if (end > ptr) {
            int len = (int)(end - ptr);
            if (len < hostname_size) {
                strncpy(hostname, ptr, len);
                hostname[len] = '\0';
                return 1;
            }
        }
    }

    return 0;
}

static int findHostIndexByIp(const char *ip)
{
    int index;

    for (index = 0; index < g_host_count; index++) {
        if (strcmp(g_hosts[index].ip, ip) == 0) {
            return index;
        }
    }

    return -1;
}

static void addOrUpdateHost(const char *ip, const char *name,
                            const char *mac, int status)
{
    int index;
    HostPC *host;

    if (!ip || !ip[0]) {
        return;
    }

    index = findHostIndexByIp(ip);
    if (index < 0) {
        if (g_host_count >= MAX_HOSTS) {
            return;
        }
        index = g_host_count++;
        memset(&g_hosts[index], 0, sizeof(g_hosts[index]));
        strncpy(g_hosts[index].ip, ip, sizeof(g_hosts[index].ip) - 1);
        g_hosts[index].ip[sizeof(g_hosts[index].ip) - 1] = '\0';
    }

    host = &g_hosts[index];
    if (name && name[0]) {
        strncpy(host->name, name, sizeof(host->name) - 1);
        host->name[sizeof(host->name) - 1] = '\0';
    } else if (!host->name[0]) {
        strncpy(host->name, ip, sizeof(host->name) - 1);
        host->name[sizeof(host->name) - 1] = '\0';
    }

    if (mac && mac[0]) {
        strncpy(host->mac, mac, sizeof(host->mac) - 1);
        host->mac[sizeof(host->mac) - 1] = '\0';
    }

    host->status = status;
}

static void loadManualHosts(void)
{
    int index;
    ManualHostEntry entry;
    extern PspConfig g_psp_config;

    g_host_count = 0;
    for (index = 0; index < config_get_manual_host_count(); index++) {
        if (config_get_manual_host(index, &entry) == 0) {
            addOrUpdateHost(entry.ip, entry.ip, entry.mac, 0);
            /* Set paired status from config so offline hosts show correct state */
            {
                int idx = findHostIndexByIp(entry.ip);
                if (idx >= 0)
                    g_hosts[idx].paired = config_is_host_paired(&g_psp_config, entry.ip);
            }
        }
    }
}

/* -------------------------------------------------------------------------
 * httpProbeHost - Send HTTP GET /serverinfo to a single host and update it.
 * Uses non-blocking TCP connect with a short timeout so discovery never stalls
 * the UI for long periods on offline/unreachable hosts.
 * Returns 0 on success (host responded), -1 on failure (offline).
 * ------------------------------------------------------------------------- */
static int httpProbeHost(HostPC *host)
{
    int sock, ret, total, nb;
    int req_len;
    int sent;
    struct sockaddr_in addr;
    char request[256];
    static char response[MAX_RESPONSE_SIZE];
    u32 start_ms;

    diag_log_write("DISC", "probing %s:%d...\n", host->ip, HTTP_PORT);

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        diag_log_write("DISC", "socket() failed\n");
        return -1;
    }

    /* Non-blocking connect so we can enforce a timeout */
    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = (unsigned char)sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(HTTP_PORT);
    addr.sin_addr.s_addr = inet_addr(host->ip);

    ret = sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        int err = sceNetInetGetErrno();
        if (err != EINPROGRESS && err != EALREADY &&
            err != EAGAIN && err != EWOULDBLOCK) {
            closeProbeTcpSocket(sock);
            return -1;
        }
    }

    if (ret != 0) {
        fd_set wfds;
        struct timeval tv;
        int optval;
        socklen_t optlen;

        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        tv.tv_sec  = CONNECT_TIMEOUT_MS / 1000;
        tv.tv_usec = (CONNECT_TIMEOUT_MS % 1000) * 1000;

        ret = sceNetInetSelect(sock + 1, NULL, &wfds, NULL, &tv);
        if (ret <= 0) {
            closeProbeTcpSocket(sock);
            return -1;
        }

        optval = -1;
        optlen = sizeof(optval);
        sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
        if (optval != 0) {
            closeProbeTcpSocket(sock);
            return -1;
        }
    }

    /* Send the HTTP GET /serverinfo request */
    snprintf(request, sizeof(request),
             "GET /serverinfo?uniqueid=%s&uuid=%s HTTP/1.0\r\n"
             "Host: %s:%d\r\n\r\n",
             CLIENT_UNIQUE_ID, client_identity_get_uuid(), host->ip, HTTP_PORT);

    /* Keep non-blocking mode through send/recv so discovery cannot hang
     * the UI thread if the network stack stalls after connect. */
    req_len = (int)strlen(request);
    sent = 0;
    start_ms = sceKernelGetSystemTimeLow() / 1000;
    while (sent < req_len) {
        ret = sceNetInetSend(sock, request + sent, req_len - sent, 0);
        if (ret > 0) {
            sent += ret;
            continue;
        }
        if (ret == 0) {
            break;
        }

        {
            int err = sceNetInetGetErrno();
            if (err != EAGAIN && err != EWOULDBLOCK) {
                diag_log_write("DISC", "probe %s: send err=%d\n", host->ip, err);
                closeProbeTcpSocket(sock);
                return -1;
            }
        }

        if ((sceKernelGetSystemTimeLow() / 1000) - start_ms > SEND_TIMEOUT_MS) {
            diag_log_write("DISC", "probe %s: send timeout\n", host->ip);
            closeProbeTcpSocket(sock);
            return -1;
        }
        sceKernelDelayThread(10000);
    }

    if (sent < req_len) {
        diag_log_write("DISC", "probe %s: short send (%d/%d)\n", host->ip, sent, req_len);
        closeProbeTcpSocket(sock);
        return -1;
    }

    /* Read response (non-blocking + timeout) */

    total    = 0;
    start_ms = sceKernelGetSystemTimeLow() / 1000;
    while (total < MAX_RESPONSE_SIZE - 1) {
        if ((sceKernelGetSystemTimeLow() / 1000) - start_ms > RECV_TIMEOUT_MS) break;

        ret = sceNetInetRecv(sock, response + total,
                             MAX_RESPONSE_SIZE - 1 - total, 0);
        if (ret > 0) {
            total += ret;
        } else if (ret == 0) {
            break;  /* Server closed connection (HTTP/1.0 end-of-body) */
        } else {
            int err = sceNetInetGetErrno();
            if (err != EAGAIN && err != EWOULDBLOCK) break;
            sceKernelDelayThread(10000); /* 10 ms */
        }
    }
    response[total] = '\0';
    closeProbeTcpSocket(sock);

    diag_log_write("DISC", "probe %s got %d bytes\n", host->ip, total);
    if (total <= 0) {
        diag_log_write("DISC", "probe %s: no response (OFFLINE)\n", host->ip);
        return -1;
    }

    /* --- Validate and parse /serverinfo response ------------------------ */
    {
        int looks_like_serverinfo;
        char hostname[32];
        looks_like_serverinfo =
            (strstr(response, "status_code=\"200\"") != NULL) ||
            (strstr(response, "\"status_code\":200") != NULL) ||
            (strstr(response, "<root") != NULL) ||
            (strstr(response, "<hostname>") != NULL) ||
            (strstr(response, "<Hostname>") != NULL);

        if (!looks_like_serverinfo) {
            return -1;
        }

        if (parseDiscoveryResponse(response, total,
                                   hostname, sizeof(hostname))) {
            strncpy(host->name, hostname, sizeof(host->name) - 1);
            host->name[sizeof(host->name) - 1] = '\0';
        }
        host->status = 1; /* Online */
    }

    /* Paired indicator: use config-based paired-host list because the plain
     * HTTP /serverinfo endpoint (port 47989) cannot verify the TLS client
     * certificate and always returns PairStatus=0.  The HTTPS endpoint
     * (port 47984) does verify, but requires full TLS — too heavy for a
     * discovery probe on PSP hardware. */
    {
        extern PspConfig g_psp_config;
        host->paired = config_is_host_paired(&g_psp_config, host->ip);
    }

    /* Extract <mac> tag for Wake-on-LAN */
    {
        const char *mp = strstr(response, "<mac>");
        if (mp) {
            const char *me;
            mp += 5;
            me = strstr(mp, "</mac>");
            if (me && (me - mp) > 0 &&
                (me - mp) < (int)sizeof(host->mac)) {
                int ml = (int)(me - mp);
                strncpy(host->mac, mp, ml);
                host->mac[ml] = '\0';
            }
        }
    }

    return 0;
}


/* -------------------------------------------------------------------------
 * mdnsDiscoverHosts - Discover Sunshine/GameStream hosts via mDNS.
 *
 * Sends an mDNS multicast query for _nvstream._tcp.local. (the service
 * type advertised by Sunshine / GameStream / Apollo) and listens for
 * responses for 2 seconds.  Each responding host's IP is extracted from
 * the source address and probed via HTTP for full server info.
 *
 * This replaces the old sequential TCP subnet scan (254 hosts * timeout).
 * mDNS discovery is near-instant on LAN.
 * ------------------------------------------------------------------------- */
static void mdnsDiscoverHosts(void)
{
    int sock, ret, nb;
    struct sockaddr_in mcast_addr, bind_addr, from_addr;
    socklen_t from_len;
    u32 start_ms;
    int found = 0;
    int resent = 0;

    /* mDNS query: _nvstream._tcp.local. type PTR class IN+QU */
    static const unsigned char mdns_query[] = {
        0x00, 0x00,  /* Transaction ID (0 for mDNS) */
        0x00, 0x00,  /* Flags: standard query */
        0x00, 0x01,  /* Questions: 1 */
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  /* AN=0, NS=0, AR=0 */
        /* _nvstream._tcp.local. */
        9, '_','n','v','s','t','r','e','a','m',
        4, '_','t','c','p',
        5, 'l','o','c','a','l',
        0,
        0x00, 0x0C,  /* Type: PTR (12) */
        0x80, 0x01   /* Class: IN + QU (unicast-response requested) */
    };

    sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        diag_log_write("DISC", "mdns: socket() failed\n");
        return;
    }

    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    /* Bind to port 5353 so we also receive multicast responses */
    memset(&bind_addr, 0, sizeof(bind_addr));
    bind_addr.sin_len    = (unsigned char)sizeof(bind_addr);
    bind_addr.sin_family = AF_INET;
    bind_addr.sin_port   = htons(5353);
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    ret = sceNetInetBind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
    diag_log_write("DISC", "mdns: bind ret=%d (err=%d)\n",
                   ret, ret < 0 ? sceNetInetGetErrno() : 0);

    /* Join mDNS multicast group (best effort — unicast QU still works) */
    {
        struct ip_mreq mreq;
        int join_ret;
        mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        join_ret = sceNetInetSetsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                                        &mreq, sizeof(mreq));
        diag_log_write("DISC", "mdns: multicast join ret=%d (err=%d)\n",
                       join_ret, join_ret < 0 ? sceNetInetGetErrno() : 0);
    }

    /* Send query to mDNS multicast group */
    memset(&mcast_addr, 0, sizeof(mcast_addr));
    mcast_addr.sin_len    = (unsigned char)sizeof(mcast_addr);
    mcast_addr.sin_family = AF_INET;
    mcast_addr.sin_port   = htons(5353);
    mcast_addr.sin_addr.s_addr = inet_addr("224.0.0.251");

    ret = sceNetInetSendto(sock, mdns_query, sizeof(mdns_query), 0,
                           (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
    diag_log_write("DISC", "mdns: query sent (%d bytes, err=%d)\n",
                   ret, ret < 0 ? sceNetInetGetErrno() : 0);

    /* Show discovery indicator */
    ui_begin_frame();
    ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
    ui_draw_header("Host Discovery");
    ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 120.0f,
                          UI_COL_TEXT, "Discovering hosts...");
    ui_end_frame();

    /* Listen for responses for 2 seconds */
    start_ms = sceKernelGetSystemTimeLow() / 1000;
    while ((sceKernelGetSystemTimeLow() / 1000) - start_ms < 2000) {
        unsigned char buf[512];

        /* Re-send query at ~1s for reliability */
        if (!resent && (sceKernelGetSystemTimeLow() / 1000) - start_ms >= 1000) {
#ifdef RETAIL_BUILD
            sceNetInetSendto(sock, mdns_query, sizeof(mdns_query), 0,
                             (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
#else
            int retry_tx = sceNetInetSendto(sock, mdns_query, sizeof(mdns_query), 0,
                                            (struct sockaddr *)&mcast_addr, sizeof(mcast_addr));
            diag_log_write("DISC", "mdns: retry query sent (%d bytes, err=%d)\n",
                           retry_tx, retry_tx < 0 ? sceNetInetGetErrno() : 0);
#endif
            resent = 1;
        }

        from_len = sizeof(from_addr);
        memset(&from_addr, 0, sizeof(from_addr));
        ret = sceNetInetRecvfrom(sock, buf, sizeof(buf), 0,
                                 (struct sockaddr *)&from_addr, &from_len);

        if (ret > 12 && (buf[2] & 0x80)) {
            /* DNS response (QR bit set) — source IP is a Sunshine host */
            char ip_str[16];
            strncpy(ip_str, inet_ntoa(from_addr.sin_addr),
                    sizeof(ip_str) - 1);
            ip_str[sizeof(ip_str) - 1] = '\0';

            if (findHostIndexByIp(ip_str) < 0) {
                diag_log_write("DISC", "mdns: found host at %s\n", ip_str);
                addOrUpdateHost(ip_str, ip_str, NULL, 0);
                {
                    int idx = findHostIndexByIp(ip_str);
                    if (idx >= 0) {
                        if (httpProbeHost(&g_hosts[idx]) == 0) {
                            found++;
                        }
                    }
                }
            }
        } else if (ret < 0) {
            int err = sceNetInetGetErrno();
            if (err == EAGAIN || err == EWOULDBLOCK) {
                sceKernelDelayThread(50000); /* 50ms between polls */
                continue;
            }
            diag_log_write("DISC", "mdns: recv error %d\n", err);
            break;
        }
    }

    /* Leave multicast group (best effort) */
    {
        struct ip_mreq mreq;
        mreq.imr_multiaddr.s_addr = inet_addr("224.0.0.251");
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        sceNetInetSetsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP,
                             &mreq, sizeof(mreq));
    }

    sceNetInetClose(sock);
    diag_log_write("DISC", "mdns: done, found=%d new hosts\n", found);
}

/* -------------------------------------------------------------------------
 * quickSubnetScan - Sequential TCP port scan for Sunshine on port 47989.
 *
 * Scans the local /24 subnet one IP at a time using the same non-blocking
 * connect + select pattern as httpProbeHost (proven on PSP hardware).
 * Each host is tried with a short timeout.  Discovered hosts are saved to
 * config so they persist across rescans without repeating the full scan.
 *
 * Interruptible: Circle button cancels the scan.
 * Returns the number of new hosts found.
 * ------------------------------------------------------------------------- */
static int quickSubnetScan(void)
{
    union SceNetApctlInfo info;
    u32 local_ip, subnet;
    int ip_idx, found = 0;
    char local_ip_str[16];

    /* Get PSP's local IP address */
    if (sceNetApctlGetInfo(8, &info) != 0 || info.ip[0] == '\0') {
        diag_log_write("DISC", "subnet: cannot get local IP\n");
        return 0;
    }
    strncpy(local_ip_str, info.ip, sizeof(local_ip_str) - 1);
    local_ip_str[sizeof(local_ip_str) - 1] = '\0';
    local_ip = ntohl(inet_addr(local_ip_str));
    subnet   = local_ip & 0xFFFFFF00;

    diag_log_write("DISC", "subnet: scanning %d.%d.%d.x\n",
                   (int)((subnet >> 24) & 0xFF),
                   (int)((subnet >> 16) & 0xFF),
                   (int)((subnet >> 8) & 0xFF));

    for (ip_idx = 1; ip_idx <= 254; ip_idx++) {
        u32 target = subnet | (u32)ip_idx;
        struct in_addr ia;
        char ip_str[16];
        int sock, nb, ret;
        struct sockaddr_in addr;

        if (target == local_ip) continue;

        ia.s_addr = htonl(target);
        strncpy(ip_str, inet_ntoa(ia), sizeof(ip_str) - 1);
        ip_str[sizeof(ip_str) - 1] = '\0';

        /* Skip already-known hosts */
        if (findHostIndexByIp(ip_str) >= 0) continue;

        /* Check cancel and draw progress every 8 IPs */
        if ((ip_idx & 7) == 0) {
            SceCtrlData pad;
            sceCtrlPeekBufferPositive(&pad, 1);
            if (pad.Buttons & PSP_CTRL_CIRCLE) {
                diag_log_write("DISC", "subnet: cancelled by user\n");
                break;
            }
            {
                char msg[64];
                snprintf(msg, sizeof(msg), "Scanning subnet... (%d/254)", ip_idx);
                ui_begin_frame();
                ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
                ui_draw_header("Host Discovery");
                ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 120.0f,
                                      UI_COL_TEXT, msg);
                ui_end_frame();
            }
        }

        sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) continue;

        nb = 1;
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

        memset(&addr, 0, sizeof(addr));
        addr.sin_len    = (unsigned char)sizeof(addr);
        addr.sin_family = AF_INET;
        addr.sin_port   = htons(HTTP_PORT);
        addr.sin_addr.s_addr = htonl(target);

        ret = sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr));

        if (ret != 0) {
            /* Non-blocking connect in progress — wait with select (single fd) */
            fd_set wfds;
            struct timeval tv;
            int optval;
            socklen_t optlen;

            FD_ZERO(&wfds);
            FD_SET(sock, &wfds);
            tv.tv_sec  = 0;
            tv.tv_usec = SCAN_CONNECT_TIMEOUT_US;

            ret = sceNetInetSelect(sock + 1, NULL, &wfds, NULL, &tv);
            if (ret <= 0) {
                closeProbeTcpSocket(sock);
                continue;
            }

            optval = -1;
            optlen = sizeof(optval);
            sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
            if (optval != 0) {
                closeProbeTcpSocket(sock);
                continue;
            }
        }

        /* Connected — this is a Sunshine host */
        closeProbeTcpSocket(sock);
        diag_log_write("DISC", "subnet: port open on %s\n", ip_str);

        config_add_manual_host(ip_str, NULL);
        addOrUpdateHost(ip_str, ip_str, NULL, 0);
        {
            int idx = findHostIndexByIp(ip_str);
            if (idx >= 0 && httpProbeHost(&g_hosts[idx]) == 0) {
                found++;
            }
        }
    }

    diag_log_write("DISC", "subnet: done, found=%d new hosts\n", found);
    return found;
}

/* -------------------------------------------------------------------------
 * scanNetwork - Probe every known (manual) host via HTTP on port 47989,
 * then discover new Sunshine/GameStream hosts via mDNS and subnet scan.
 *
 * Apollo / Sunshine / GameStream hosts expose /serverinfo on this port.
 * After known-host probing, mDNS multicast discovery and a batched TCP
 * subnet scan find new hosts.
 * A scanning indicator is drawn for each host so the UI stays responsive.
 * ------------------------------------------------------------------------- */
static void scanNetwork(void)
{
    int i;

    for (i = 0; i < g_host_count; i++) {
        u32 probe_start_ms;
        int probe_ok = -1;

        /* Draw a per-host scanning indicator */
        {
            char msg[64];
            snprintf(msg, sizeof(msg), "Probing %s ... (%d/%d)",
                     g_hosts[i].ip, i + 1, g_host_count);
            ui_begin_frame();
            ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
            ui_draw_header("Host Discovery");
            ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 120.0f,
                                  UI_COL_TEXT, msg);
            ui_end_frame();
        }

        probe_start_ms = sceKernelGetSystemTimeLow() / 1000;
        probe_ok = httpProbeHost(&g_hosts[i]);
        if (probe_ok < 0) {
            g_hosts[i].status = 0;
        }

        {
            u32 probe_elapsed_ms = (sceKernelGetSystemTimeLow() / 1000) - probe_start_ms;
            if (probe_elapsed_ms < PROBE_MIN_DISPLAY_MS) {
                sceKernelDelayThread((PROBE_MIN_DISPLAY_MS - probe_elapsed_ms) * 1000);
            }
        }
    }

    /* After probing known hosts, discover new ones via mDNS */
    mdnsDiscoverHosts();

    if (g_host_count == 0) {
        g_selected_index = 0;
    }
    clampSelectionWindow();
}

static const char *statusText(int status)
{
    switch (status) {
        case 1: return "Online";
        case 2: return "Locked";
        default: return "Offline";
    }
}

static u32 statusColor(int status)
{
    /* Semantic status colours — kept fixed across themes for clarity */
    switch (status) {
        case 1: return 0xFF50C878u;  /* Green  — Online  */
        case 2: return UI_COL_ACCENT; /* Theme accent — Locked */
        default: return UI_COL_TEXT_DIM; /* Dim text — Offline */
    }
}

static void drawHostItem(int index, int y, int selected)
{
    HostPC *host = &g_hosts[index];

    /* Focus pop: the selected host card grows 2% while the focus animation
     * lerps, identical to the settings-menu card pop. */
    float dist  = fabsf((float)index - s_host_focus_anim);
    float scale = (dist < 1.0f) ? (1.0f + (1.0f - dist) * 0.02f) : 1.0f;
    int item_h  = (int)((float)HOST_ITEM_H * scale);
    int item_w  = (int)((float)HOST_ITEM_W * scale);

    int ry = y - (item_h - HOST_ITEM_H) / 2;
    int rx = HOST_LIST_X - (item_w - HOST_ITEM_W) / 2;

    u32 bg = selected ? UI_COL_CARD_SEL : UI_COL_PANEL;
    u32 border = selected ? UI_COL_BORDER_FOC : UI_COL_BORDER;
    const int dot_x    = rx + 8;
    const int text_x   = rx + 26;
    const int status_x = rx + 320;
    const int status_w = item_w - 320 - 8;
    float text_scale   = selected ? 0.50f : 0.45f;

    /* 3-layer drop shadow — grows +2px when selected for hover effect */
    ui_set_blend(1);
    {
        int so = selected ? 1 : 0;
        ui_draw_rect_rounded(rx + 3 + so, ry + 3 + so, item_w, item_h, 12, 0x18000000u);
        ui_draw_rect_rounded(rx + 2 + so, ry + 2 + so, item_w, item_h, 12, 0x28000000u);
        ui_draw_rect_rounded(rx + 1 + so, ry + 1 + so, item_w, item_h, 12, 0x38000000u);
    }

    int t = selected ? 2 : 1;
    ui_draw_rect_rounded(rx, ry, item_w, item_h, 12, border);
    ui_draw_rect_rounded(rx + t, ry + t, item_w - 2*t, item_h - 2*t, 12 - t, bg);
    ui_set_blend(0);
    ui_draw_rect_rounded(dot_x, ry + 11, 10, 16, 5, statusColor(host->status));

    /* Left side: name + IP, vertically centered as two rows.
     * intraFont y = baseline; ascenders ~7px above, descenders ~2px below
     * at 0.45 scale.  Two rows w/ 14px spacing ≈ 23px total text height.
     * Pill = 38px → (38-23)/2 ≈ 7-8px padding → baselines at +16/+30. */
    ui_draw_text_scaled((float)text_x, (float)(ry + 16), UI_COL_TEXT, host->name, text_scale);
    ui_draw_text_scaled((float)text_x, (float)(ry + 30), UI_COL_TEXT_DIM, host->ip, text_scale);

    /* Right side: status + paired label, vertically centered as two rows.
     * Paired/Unpaired colour is vivid only when the row is selected;
     * non-selected rows use the dim text colour for both states.
     * Only show paired label for online hosts — offline hosts can't
     * be verified and "Unpaired" would be misleading. */
    ui_draw_text_centered((float)status_x, (float)status_w, (float)(ry + 16),
                          selected ? UI_COL_TEXT_FOCUS : UI_COL_TEXT_DIM,
                          statusText(host->status));
    if (host->status > 0) {
        u32 pair_col = UI_COL_TEXT_DIM;
        if (selected)
            pair_col = host->paired ? 0xFF55EE55u : 0xFF8888CCu;
        ui_draw_text_centered((float)status_x, (float)status_w, (float)(ry + 30),
                              pair_col,
                              host->paired ? "Paired" : "Unpaired");
    }
}

void host_discovery_init(void)
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    loadManualHosts();
    scanNetwork();

    /* Subnet scan is NOT run automatically — it exhausts the PSP socket pool
     * and causes ENOMEM (errno 12) on the subsequent TLS connect.  mDNS +
     * HTTP probe are sufficient for auto-discovery.  The user can still
     * trigger a full subnet scan manually via the Square button. */

    clampSelectionWindow();
    g_prev_buttons = 0;

    /* Reset smooth-scroll state so the list starts at the top */
    s_host_scroll_curr   = 0.0f;
    s_host_target_camera = 0.0f;
    s_host_focus_anim    = 0.0f;
    s_host_last_anim_us  = 0;
}

int renderHostDiscoveryList(void)
{
    SceCtrlData pad;
    int index;
    int item_y;

    sceCtrlPeekBufferPositive(&pad, 1);
    { extern volatile unsigned int g_remote_buttons;
      pad.Buttons |= g_remote_buttons; g_remote_buttons = 0; }
    {
        u32 pressed = pad.Buttons & ~g_prev_buttons;
        if (pressed) {
            diag_log_write("UI", "HOST btn=0x%04X t=%u\n",
                           (unsigned)pressed, sceKernelGetSystemTimeLow() / 1000);
        }
    }

    /* Auto-select DISABLED — manual navigation only via RemoteJoy */

    if ((pad.Buttons & PSP_CTRL_UP) && !(g_prev_buttons & PSP_CTRL_UP)) {
        if (g_selected_index > 0) {
            g_selected_index--;
            clampSelectionWindow();
        }
    }

    if ((pad.Buttons & PSP_CTRL_DOWN) && !(g_prev_buttons & PSP_CTRL_DOWN)) {
        if (g_selected_index < g_host_count - 1) {
            g_selected_index++;
            clampSelectionWindow();
        }
    }

    if ((pad.Buttons & PSP_CTRL_SQUARE) && !(g_prev_buttons & PSP_CTRL_SQUARE)) {
        scanNetwork();
        quickSubnetScan();
        /* Let PSP reclaim socket resources before user can select a host */
        sceKernelDelayThread(500 * 1000);   /* 500 ms cooldown */
        clampSelectionWindow();
    }

    /* L+R together: delete selected host with confirmation dialog */
    if (g_host_count > 0 &&
        (pad.Buttons & PSP_CTRL_LTRIGGER) && (pad.Buttons & PSP_CTRL_RTRIGGER) &&
        (!(g_prev_buttons & PSP_CTRL_LTRIGGER) || !(g_prev_buttons & PSP_CTRL_RTRIGGER))) {
        /* Show confirmation screen */
        HostPC *target = &g_hosts[g_selected_index];
        int confirmed = 0;
        SceCtrlData cpd, cprev;
        sceCtrlPeekBufferPositive(&cpd, 1);
        memcpy(&cprev, &cpd, sizeof(cpd));
        while (1) {
            char msg[80];
            u32 cp;
            memcpy(&cprev, &cpd, sizeof(cpd));
            sceCtrlPeekBufferPositive(&cpd, 1);
            { extern volatile unsigned int g_remote_buttons;
              cpd.Buttons |= g_remote_buttons; g_remote_buttons = 0; }
            cp = cpd.Buttons & ~cprev.Buttons;
            if (cp & PSP_CTRL_CROSS) { confirmed = 1; break; }
            if (cp & PSP_CTRL_CIRCLE) { break; }
            ui_begin_frame();
            ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
            ui_draw_header("Delete Host?");
            snprintf(msg, sizeof(msg), "Remove \"%s\" (%s)?", target->name, target->ip);
            ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 110.0f, UI_COL_TEXT, msg);
            ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 150.0f, UI_COL_TEXT_DIM,
                                  "X: Delete    O: Cancel");
            ui_end_frame();
        }
        if (confirmed) {
            config_delete_manual_host(target->ip);
            /* Remove from runtime list */
            {
                int di;
                for (di = g_selected_index; di < g_host_count - 1; di++) {
                    g_hosts[di] = g_hosts[di + 1];
                }
                memset(&g_hosts[g_host_count - 1], 0, sizeof(HostPC));
                g_host_count--;
            }
            clampSelectionWindow();
        }
        /* Flush button state */
        sceCtrlPeekBufferPositive(&pad, 1);
        g_prev_buttons = pad.Buttons;
        return -1;
    }

    if ((pad.Buttons & PSP_CTRL_TRIANGLE) && !(g_prev_buttons & PSP_CTRL_TRIANGLE)) {
        char ip[16];
        if (osk_get_ip_input(ip, sizeof(ip)) == 0) {
            if (config_add_manual_host(ip, "") == 0) {
                addOrUpdateHost(ip, ip, NULL, 0);
                clampSelectionWindow();
                /* Probe the newly added host to check online status */
                {
                    int probe_idx = findHostIndexByIp(ip);
                    if (probe_idx >= 0) {
                        if (httpProbeHost(&g_hosts[probe_idx]) < 0) {
                            g_hosts[probe_idx].status = 0;
                        }
                    }
                }
            }
        }
        /* Flush button state after returning from osk to prevent
         * stale Start press from triggering exit dialog */
        sceCtrlPeekBufferPositive(&pad, 1);
        g_prev_buttons = pad.Buttons;
        /* Re-draw and return immediately so we start fresh.
         * Snap the smooth-scroll camera so the list is stable. */
        s_host_target_camera = (float)g_first_visible_index;
        s_host_scroll_curr   = s_host_target_camera;
        s_host_focus_anim    = (float)g_selected_index;

        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Host Discovery");
        ui_set_scissor(0, 30, 480, 214);
        {
            int idx_tmp;
            for (idx_tmp = 0; idx_tmp < g_host_count; idx_tmp++) {
                int item_y_tmp = HOST_LIST_Y + (int)(((float)idx_tmp - s_host_scroll_curr)
                                 * (float)(HOST_ITEM_H + HOST_GAP));
                if (item_y_tmp + HOST_ITEM_H <= 30 || item_y_tmp >= 244) continue;
                drawHostItem(idx_tmp, item_y_tmp, idx_tmp == g_selected_index);
            }
        }
        ui_clear_scissor();
        ui_draw_footer_hint("{X}: Sel  {SQ}: Scan  {TR}: Add  {O}: Back  {L}+{R}: Del  {SE}: WOL  {ST}: Exit");
        ui_end_frame();
        return -1;
    }

    if ((pad.Buttons & PSP_CTRL_SELECT) && !(g_prev_buttons & PSP_CTRL_SELECT)) {
        if (g_host_count > 0 && g_hosts[g_selected_index].status == 0 && g_hosts[g_selected_index].mac[0]) {
            wol_show_confirm(g_hosts[g_selected_index].name, g_hosts[g_selected_index].mac);
        }
    }

    if ((pad.Buttons & PSP_CTRL_START) && !(g_prev_buttons & PSP_CTRL_START)) {
        exit_dialog_run();
    }

    /* Circle: go back to settings menu */
    if ((pad.Buttons & PSP_CTRL_CIRCLE) && !(g_prev_buttons & PSP_CTRL_CIRCLE)) {
        g_prev_buttons = pad.Buttons;
        return -2;  /* Signal: back to settings */
    }

    ui_begin_frame();
    ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
    ui_draw_header("Host Discovery");

    if (g_host_count == 0) {
        ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 108.0f, UI_COL_TEXT, "No hosts found");
        ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 132.0f, UI_COL_TEXT_DIM,
                              "Press [] to scan or /\\ to add manually");
    } else {
        /* ---- Smooth-scroll animation (matches settings_menu.c) ---- */
        {
            u32 now_us = sceKernelGetSystemTimeLow();
            float dt = 1.0f;
            if (s_host_last_anim_us != 0) {
                u32 elapsed = now_us - s_host_last_anim_us;
                dt = (float)elapsed / 16667.0f;
                if (dt > 4.0f) dt = 4.0f;
                if (dt < 0.1f) dt = 0.1f;
            }
            s_host_last_anim_us = now_us;

            s_host_target_camera = (float)g_first_visible_index;
            s_host_scroll_curr  += (s_host_target_camera - s_host_scroll_curr) * 0.15f * dt;
            s_host_focus_anim   += ((float)g_selected_index - s_host_focus_anim) * 0.20f * dt;
        }

        ui_set_scissor(0, 30, 480, 214);
        for (index = 0; index < g_host_count; index++) {
            item_y = HOST_LIST_Y + (int)(((float)index - s_host_scroll_curr)
                     * (float)(HOST_ITEM_H + HOST_GAP));
            if (item_y + HOST_ITEM_H <= 30 || item_y >= 244) continue;
            drawHostItem(index, item_y, index == g_selected_index);
        }
        ui_clear_scissor();

        if (g_first_visible_index > 0) {
            ui_draw_text((float)(HOST_LIST_X + HOST_ITEM_W - 12),
                         (float)(HOST_LIST_Y - 2),
                         UI_COL_TEXT_DIM,
                         "^");
        }
        if (g_first_visible_index + MAX_VISIBLE_HOSTS < g_host_count) {
            ui_draw_text((float)(HOST_LIST_X + HOST_ITEM_W - 12),
                         (float)(HOST_LIST_Y + MAX_VISIBLE_HOSTS * (HOST_ITEM_H + HOST_GAP) - 2),
                         UI_COL_TEXT_DIM,
                         "v");
        }
    }

    ui_draw_footer_hint("{X}: Sel  {SQ}: Scan  {TR}: Add  {O}: Back  {L}+{R}: Del  {SE}: WOL  {ST}: Exit");
    ui_end_frame();

    if ((pad.Buttons & PSP_CTRL_CROSS) && !(g_prev_buttons & PSP_CTRL_CROSS)) {
        g_prev_buttons = pad.Buttons;
        if (g_host_count > 0 && g_hosts[g_selected_index].status != 2) {
            diag_log_write("UI", "HOST selected idx=%d t=%u\n",
                           g_selected_index, sceKernelGetSystemTimeLow() / 1000);
            return g_selected_index;
        }
        return -1;
    }

    g_prev_buttons = pad.Buttons;
    return -1;
}

HostPC* host_discovery_get_selected(void)
{
    if (g_host_count <= 0 || g_selected_index < 0 || g_selected_index >= g_host_count) {
        return NULL;
    }

    return &g_hosts[g_selected_index];
}

void host_discovery_mark_online(const char *ip)
{
    int index;

    if (!ip || !ip[0]) {
        return;
    }

    index = findHostIndexByIp(ip);
    if (index >= 0) {
        g_hosts[index].status = 1;
    }
}

void host_discovery_shutdown(void)
{
    memset(g_hosts, 0, sizeof(g_hosts));
    g_host_count = 0;
    g_selected_index = 0;
    g_first_visible_index = 0;
    g_prev_buttons = 0;
}
