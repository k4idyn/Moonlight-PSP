/*
 * netconf_ui.c - WiFi Network Configuration UI for PSP Moonlight
 *
 * Shows the PSP built-in Access Point dialog (sceUtilityNetconfInitStart),
 * then polls sceNetApctlGetState() until the IP stack is fully up.
 * A UIManager spinner frame is drawn on every iteration so the screen
 * remains responsive.
 */

#include <pspkernel.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <psputility_netconf.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspnet_resolver.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <string.h>

#include "netconf_ui.h"
#include "ui_manager.h"
#include "diag_log.h"

/* Extern declarations for GE display control (from display_gpu.c) */
extern void display_init(void);
extern void display_shutdown(void);

/* Timeout waiting for IP after the dialog closes (microseconds) */
#define IP_ACQUIRE_TIMEOUT_US   (60 * 1000 * 1000)
/* Polling interval for post-dialog IP-wait loop (microseconds) */
#define POLL_INTERVAL_US        (50 * 1000)

/* Small GU command list used only for the dialog frame pump.
 * 16 KB is ample for a single sceGuClear call per frame. */
static unsigned int __attribute__((aligned(16))) s_netconf_gu_list[16 * 1024 / 4];

/* -------------------------------------------------------------------------
 * netconf_ui_run
 * ------------------------------------------------------------------------- */
int netconf_ui_run(void)
{
    pspUtilityNetconfData netconf;
    int status;
    int prev_status = -1;
    int ret;
    int net_inited = 0;
    int inet_inited = 0;
    int apctl_inited = 0;
    int resolver_inited = 0;

    diag_log_write("NET", "[NETCONF] start\n");

    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    diag_log_write("NET", "[NETCONF] load COMMON ret=0x%08X\n", (unsigned)ret);
    if (ret < 0 && ret != (int)0x80110F01) return ret;
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    diag_log_write("NET", "[NETCONF] load INET ret=0x%08X\n", (unsigned)ret);
    if (ret < 0 && ret != (int)0x80110F01) return ret;

    /* Use a larger memory pool (128KB) and standard stack sizes (4KB).
     * In Round 3, we make these non-fatal to avoid Permission errors if
     * the stack was already half-initialized by another component. */
    ret = sceNetInit(512 * 1024, 42, 4096, 42, 4096);
    diag_log_write("NET", "[NETCONF] sceNetInit ret=0x%08X\n", (unsigned)ret);
    if (ret >= 0) net_inited = 1;
    ret = sceNetInetInit();
    diag_log_write("NET", "[NETCONF] sceNetInetInit ret=0x%08X\n", (unsigned)ret);
    if (ret >= 0) inet_inited = 1;
    ret = sceNetApctlInit(0x2000, 42);
    diag_log_write("NET", "[NETCONF] sceNetApctlInit ret=0x%08X\n", (unsigned)ret);
    if (ret >= 0) apctl_inited = 1;
    ret = sceNetResolverInit();
    diag_log_write("NET", "[NETCONF] sceNetResolverInit ret=0x%08X\n", (unsigned)ret);
    if (ret >= 0) resolver_inited = 1;

    /* Build the dialog parameters ---------------------------------------- */
    memset(&netconf, 0, sizeof(netconf));
    netconf.base.size        = sizeof(netconf);
    netconf.base.language    = PSP_SYSTEMPARAM_LANGUAGE_ENGLISH;
    netconf.base.buttonSwap  = PSP_UTILITY_ACCEPT_CROSS;

    /* ROUND 3: In User Mode, utility thread priorities must be >= 40
     * on some firmware versions to avoid 0x80020193 (Illegal Permission). */
    netconf.base.graphicsThread = 40;
    netconf.base.accessThread   = 40;
    netconf.base.fontThread     = 40;
    netconf.base.soundThread    = 40;

    /* Use the last-used AP if available (FW >= 200), fallback to CONNECTAP */
    netconf.action = PSP_NETCONF_ACTION_CONNECTAP_LASTUSED;

    /* Launch the network configuration utility ---------------------------- */
    ret = sceUtilityNetconfInitStart(&netconf);
    diag_log_write("NET", "[NETCONF] sceUtilityNetconfInitStart ret=0x%08X\n",
                   (unsigned)ret);
    if (ret < 0) {
        goto fail;
    }

    /* Pump the dialog loop ------------------------------------------------ */
    while (1) {
        status = sceUtilityNetconfGetStatus();
        if (status != prev_status) {
            diag_log_write("NET", "[NETCONF] dialog status=%d\n", status);
            prev_status = status;
        }
        if (status == PSP_UTILITY_DIALOG_NONE) break;

        /* GU frame pump — PSP utility dialogs need this every iteration.
         * Required sequence (per PSP SDK):
         *   sceGuStart → sceGuClear → sceGuFinish → sceGuSync
         *   → sceUtilityXxxUpdate(1)   <- dialog renders itself here
         *   → sceDisplayWaitVblankStart → sceGuSwapBuffers
         * Without the GU frame pump the dialog only gets updated at the
         * old sceKernelDelayThread rate (50 ms = 20 Hz) giving "slow motion".
         * The vblank wait replaces the old sleep and provides 60 Hz pacing. */
        sceGuStart(GU_DIRECT, s_netconf_gu_list);
        sceGuClearColor(g_ui_bg_color);
        sceGuClear(GU_COLOR_BUFFER_BIT);
        sceGuFinish();
        sceGuSync(GU_SYNC_FINISH, GU_SYNC_WHAT_DONE);

        if (status == PSP_UTILITY_DIALOG_VISIBLE) {
            sceUtilityNetconfUpdate(1);
        } else if (status == PSP_UTILITY_DIALOG_QUIT) {
            sceUtilityNetconfShutdownStart();
        }

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
    }

    /* -----------------------------------------------------------------------
     * Check whether the user actually connected or cancelled.
     * --------------------------------------------------------------------- */
    {
        int apctl_state = 0;
        ret = sceNetApctlGetState(&apctl_state);
        diag_log_write("NET", "[NETCONF] post-dialog apctl ret=0x%08X state=%d\n",
                       (unsigned)ret, apctl_state);
        if (ret < 0) {
            goto fail;
        }

        if (apctl_state == 0) {
            ret = -1; /* Cancelled */
            goto fail;
        }
    }

    /* Wait for state 4 (IP fully assigned) with a 60-second timeout ------- */
    {
        SceUInt32 start = sceKernelGetSystemTimeLow();
        while (1) {
            int apctl_state = 0;
            ret = sceNetApctlGetState(&apctl_state);
            if (ret < 0) {
                goto fail;
            }
            if (apctl_state == 4) {
                diag_log_write("NET", "[NETCONF] connected, IP acquired\n");
                return 0;   /* Success - IP acquired */
            }

            /* Draw waiting frame */
            ui_begin_frame();
            ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
            ui_draw_header("Wi-Fi Setup");
            ui_draw_spinner(180, 136, "Acquiring IP address...");
            ui_end_frame();

            /* Check timeout */
            SceUInt32 elapsed = sceKernelGetSystemTimeLow() - start;
            if (elapsed >= IP_ACQUIRE_TIMEOUT_US) {
                ret = -3;
                goto fail;
            }

            sceKernelDelayThread(POLL_INTERVAL_US);
        }
    }

fail:
    diag_log_write("NET", "[NETCONF] failed ret=0x%08X\n", (unsigned)ret);
    if (ret < 0) {
        if (resolver_inited) {
            sceNetResolverTerm();
        }
        if (apctl_inited) {
            sceNetApctlTerm();
        }
        if (inet_inited) {
            sceNetInetTerm();
        }
        if (net_inited) {
            sceNetTerm();
        }
    }
    return ret;
}
