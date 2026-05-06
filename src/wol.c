/*
 * wol.c - Wake-on-LAN magic packet and confirmation UI
 *
 * Sends a standard 102-byte WOL magic packet via UDP broadcast and
 * provides a UIManager-based confirmation dialog with a success toast.
 */

#include <pspkernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pspctrl.h>
#include <pspthreadman.h>
#include <string.h>
#include <stdio.h>

#include "wol.h"
#include "ui_manager.h"
#include "diag_log.h"

#define wol_log(fmt, ...) diag_log_write("WOL", fmt, ##__VA_ARGS__)

/* WOL magic packet: 6 FF bytes + 16 repetitions of 6-byte MAC = 102 bytes */
#define WOL_PACKET_LEN  102
/* UDP port for WOL (port 9 = discard, industry standard for WOL) */
#define WOL_PORT        9
/* Toast display duration (~2 seconds at ~50 ms per loop = 40 iterations) */
#define TOAST_FRAMES    40
/* Frame delay */
#define FRAME_DELAY_US  (50 * 1000)

/* -------------------------------------------------------------------------
 * parse_mac - Parse "XX:XX:XX:XX:XX:XX" into 6-byte array.
 * Returns 1 on success, 0 on failure.
 * ------------------------------------------------------------------------- */
static int parse_mac(const char *mac_str, unsigned char mac[6])
{
    if (!mac_str) return 0;
    unsigned int b[6];
    int n = sscanf(mac_str, "%02x:%02x:%02x:%02x:%02x:%02x",
                   &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
    if (n != 6) return 0;
    int i;
    for (i = 0; i < 6; i++) mac[i] = (unsigned char)b[i];
    return 1;
}

/* -------------------------------------------------------------------------
 * wol_send_magic_packet
 * ------------------------------------------------------------------------- */
int wol_send_magic_packet(const char *mac_str)
{
    unsigned char mac[6];
    if (!parse_mac(mac_str, mac)) return -1;

    /* Build the magic payload --------------------------------------------- */
    unsigned char packet[WOL_PACKET_LEN];
    int i;
    /* First 6 bytes: all 0xFF */
    for (i = 0; i < 6; i++) packet[i] = 0xFF;
    /* Next 96 bytes: MAC repeated 16 times */
    for (i = 0; i < 16; i++) {
        memcpy(&packet[6 + i * 6], mac, 6);
    }

    /* Open UDP socket ----------------------------------------------------- */
    int sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -2;

    /* Enable broadcast ---------------------------------------------------- */
    int broadcast = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_BROADCAST,
                         &broadcast, sizeof(broadcast));

    /* Destination: broadcast on port 9 ------------------------------------ */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = (unsigned char)sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(WOL_PORT);
    addr.sin_addr.s_addr = inet_addr("255.255.255.255");

    int ret = sceNetInetSendto(sock, packet, WOL_PACKET_LEN, 0,
                               (struct sockaddr *)&addr, sizeof(addr));
    sceNetInetClose(sock);

    if (ret != WOL_PACKET_LEN) {
        wol_log("send magic packet failed ret=%d errno=%d\n",
                ret, sceNetInetGetErrno());
    }

    return (ret == WOL_PACKET_LEN) ? 0 : -2;
}

/* -------------------------------------------------------------------------
 * wol_show_confirm
 * ------------------------------------------------------------------------- */
int wol_show_confirm(const char *host_name, const char *mac_str)
{
    SceCtrlData pad;
    SceCtrlData prev_pad;
    memset(&prev_pad, 0, sizeof(prev_pad));
    int sent = 0;
    int toast_countdown = 0;
    const char *toast_text = "WOL Sent!";

    /* Build the prompt line: "Wake HOSTNAME?" */
    char prompt[64];
    snprintf(prompt, sizeof(prompt), "Wake %s?",
             (host_name && host_name[0]) ? host_name : "this PC");

    while (1) {
        sceCtrlPeekBufferPositive(&pad, 1);

        if (toast_countdown > 0) {
            /* ---- Toast phase — show "WOL Sent!" and count down ---- */
            toast_countdown--;

            ui_begin_frame();
            ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
            ui_draw_header("Wake-on-LAN");
            /* Toast: centred panel */
            ui_set_blend(1);
            ui_draw_rect_rounded(140, 116, 200, 40, 12, UI_COL_PANEL_DARK);
            ui_set_blend(0);
            ui_draw_border(140, 116, 200, 40, 2, UI_COL_BORDER_FOC);
            ui_draw_text_centered(140.0f, 200.0f, 132.0f, UI_COL_ACCENT, toast_text);
            ui_end_frame();

            if (toast_countdown == 0) {
                return sent ? 1 : 0;
            }
            sceKernelDelayThread(FRAME_DELAY_US);
            prev_pad = pad;
            continue;
        }

        /* ---- Confirmation dialog ---- */
        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Wake-on-LAN");

        /* Modal panel */
        ui_set_blend(1);
        ui_draw_rect_rounded(80, 96, 320, 80, 12, UI_COL_PANEL_DARK);
        ui_set_blend(0);
        ui_draw_border(80, 96, 320, 80, 2, UI_COL_BORDER_FOC);

        /* Prompt text */
        ui_draw_text_centered(80.0f, 320.0f, 116.0f, UI_COL_TEXT, prompt);
        /* Hint */
        ui_draw_footer_hint("{X}: Send  {O}: Cancel");

        ui_end_frame();

        /* Input ---- */
        int cross  = (pad.Buttons & PSP_CTRL_CROSS)  && !(prev_pad.Buttons & PSP_CTRL_CROSS);
        int circle = (pad.Buttons & PSP_CTRL_CIRCLE) && !(prev_pad.Buttons & PSP_CTRL_CIRCLE);

        if (cross) {
            sent = (wol_send_magic_packet(mac_str) == 0);
            toast_text = sent ? "WOL Sent!" : "WOL Failed";
            toast_countdown = TOAST_FRAMES;
        } else if (circle) {
            return 0;   /* Cancelled */
        }

        prev_pad = pad;
        sceKernelDelayThread(FRAME_DELAY_US);

        (void)sent; /* used implicitly via toast branch */
    }
}
