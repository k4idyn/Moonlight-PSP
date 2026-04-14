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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

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
#define RECV_TIMEOUT_MS      2000
#define HTTP_FALLBACK_TIMEOUT_US 1500000
#define PROBE_MIN_DISPLAY_MS 350
#define MAX_RESPONSE_SIZE    2048
#define MAX_VISIBLE_HOSTS    5
#define CLIENT_UNIQUE_ID     client_identity_get_uid()

static HostPC g_hosts[MAX_HOSTS];
static int g_host_count = 0;
static int g_selected_index = 0;
static int g_first_visible_index = 0;
static u32 g_prev_buttons = 0;

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

    g_host_count = 0;
    for (index = 0; index < config_get_manual_host_count(); index++) {
        if (config_get_manual_host(index, &entry) == 0) {
            addOrUpdateHost(entry.ip, entry.ip, entry.mac, 0);
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
            sceNetInetClose(sock);
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
            sceNetInetClose(sock);
            return -1;
        }

        optval = -1;
        optlen = sizeof(optval);
        sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
        if (optval != 0) {
            sceNetInetClose(sock);
            return -1;
        }
    }

    /* Switch to blocking for request/response */
    nb = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    /* Send the HTTP GET /serverinfo request */
    snprintf(request, sizeof(request),
             "GET /serverinfo?uniqueid=%s&uuid=%s HTTP/1.0\r\n"
             "Host: %s:%d\r\n\r\n",
             CLIENT_UNIQUE_ID, client_identity_get_uuid(), host->ip, HTTP_PORT);

    ret = sceNetInetSend(sock, request, strlen(request), 0);
    if (ret <= 0) {
        sceNetInetClose(sock);
        return -1;
    }

    /* Read response (non-blocking + timeout) */
    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

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
    sceNetInetClose(sock);

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
 * scanNetwork - Probe every known (manual) host via HTTP on port 47989.
 *
 * Apollo / Sunshine / GameStream hosts expose /serverinfo on this port.
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
    u32 bg = selected ? UI_COL_CARD_SEL : UI_COL_PANEL;
    u32 border = selected ? UI_COL_BORDER_FOC : UI_COL_BORDER;
    const int dot_x   = HOST_LIST_X + 8;
    const int text_x  = HOST_LIST_X + 26;
    const int status_x = HOST_LIST_X + 320;
    const int status_w = HOST_ITEM_W - (status_x - HOST_LIST_X) - 8;

    ui_set_blend(1);
    int t = 1;
    ui_draw_rect_rounded(HOST_LIST_X, y, HOST_ITEM_W, HOST_ITEM_H, 8, border);
    ui_draw_rect_rounded(HOST_LIST_X + t, y + t, HOST_ITEM_W - 2*t, HOST_ITEM_H - 2*t, 8 - t, bg);
    ui_set_blend(0);
    ui_draw_rect(dot_x, y + 11, 10, 16, statusColor(host->status));

    ui_draw_text((float)text_x, (float)(y + 12), UI_COL_TEXT, host->name);
    ui_draw_text((float)text_x, (float)(y + 26), UI_COL_TEXT_DIM, host->ip);
    ui_draw_text_centered((float)status_x, (float)status_w, (float)(y + 16),
                          selected ? UI_COL_TEXT_FOCUS : UI_COL_TEXT_DIM,
                          statusText(host->status));
}

void host_discovery_init(void)
{
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    loadManualHosts();
    scanNetwork();
    clampSelectionWindow();
    g_prev_buttons = 0;
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
        /* Re-draw and return immediately so we start fresh */
        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Host Discovery");
        {
            int item_y_tmp = HOST_LIST_Y;
            int idx_tmp;
            for (idx_tmp = g_first_visible_index;
                 idx_tmp < g_host_count && idx_tmp < g_first_visible_index + MAX_VISIBLE_HOSTS;
                 idx_tmp++) {
                drawHostItem(idx_tmp, item_y_tmp, idx_tmp == g_selected_index);
                item_y_tmp += HOST_ITEM_H + HOST_GAP;
            }
        }
        ui_draw_footer_hint("{X}: Sel  {SQ}: Scan  {TR}: Add  {SE}: WOL  {L}+{R}: Del  {ST}: Exit");
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

    ui_begin_frame();
    ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
    ui_draw_header("Host Discovery");

    if (g_host_count == 0) {
        ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 108.0f, UI_COL_TEXT, "No hosts found");
        ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 132.0f, UI_COL_TEXT_DIM,
                              "Press [] to scan or /\\ to add manually");
    } else {
        item_y = HOST_LIST_Y;
        for (index = g_first_visible_index;
             index < g_host_count && index < g_first_visible_index + MAX_VISIBLE_HOSTS;
             index++) {
            drawHostItem(index, item_y, index == g_selected_index);
            item_y += HOST_ITEM_H + HOST_GAP;
        }

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

    ui_draw_footer_hint("{X}: Sel  {SQ}: Scan  {TR}: Add  {SE}: WOL  {L}+{R}: Del  {ST}: Exit");
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