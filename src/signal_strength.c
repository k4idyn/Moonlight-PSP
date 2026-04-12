/*
 * signal_strength.c - Wi-Fi Signal Strength Monitor for PSP Moonlight
 *
 * Implements dynamic bitrate scaling based on WiFi signal quality.
 * Uses PSP wlan APIs to monitor signal strength and adjust streaming
 * parameters to prevent frame stutters during weak signal conditions.
 */

#include <pspkernel.h>
#include <pspwlan.h>
#include <pspnet_apctl.h>
#include <pspdebug.h>
#include <stdio.h>
#include <string.h>

#include "signal_strength.h"
#include "diag_log.h"

/* GU UI owns the framebuffer during normal runtime; avoid direct debug-screen writes. */
#define pspDebugScreenPrintf(...) ((void)0)

/* Log signal only every N updates to avoid spamming */
static int g_signal_log_counter = 0;
#define SIGNAL_LOG_INTERVAL 10  /* Log every 10 checks = 10 seconds */

/*============================================================================
 * PSP WiFi Signal APIs
 *============================================================================*/

/* The PSP SDK's sceNetApctlGetInfo uses a union type. Info code 5 returns
 * RSSI as an integer. We use the SDK-provided function signature directly
 * without redefining the info structure. */

/* Signal info code for RSSI */
#define SCE_NET_APCTL_INFO_RSSI  5

/*============================================================================
 * Constants
 *============================================================================*/

/* Icon display duration in microseconds (2 seconds) */
#define ICON_DISPLAY_DURATION_US  (2 * 1000 * 1000)

/* Signal check interval in microseconds (1 second) */
#define SIGNAL_CHECK_INTERVAL_US  (1 * 1000 * 1000)

/* RSSI to percentage conversion
 * Typical WiFi RSSI ranges:
 * - Excellent: -30 to -50 dBm -> 80-100%
 * - Good: -50 to -60 dBm -> 60-80%
 * - Fair: -60 to -70 dBm -> 40-60%
 * - Weak: -70 to -80 dBm -> 20-40%
 * - Critical: < -80 dBm -> 0-20%
 */
#define RSSI_MIN  (-90)   /* Minimum expected RSSI (worst signal) */
#define RSSI_MAX  (-30)   /* Maximum expected RSSI (best signal) */

/*============================================================================
 * Module State
 *============================================================================*/

static SignalState g_signal_state = { 0 };
static int g_initialized = 0;

/*============================================================================
 * Helper: Convert RSSI to Percentage
 *============================================================================*/
static int rssi_to_percent(int rssi)
{
    int percent;

    /* Clamp RSSI to expected range */
    if (rssi >= RSSI_MAX)
        return 100;
    if (rssi <= RSSI_MIN)
        return 0;

    /* Linear conversion from RSSI to percentage */
    percent = ((rssi - RSSI_MIN) * 100) / (RSSI_MAX - RSSI_MIN);

    /* Clamp to 0-100 */
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    return percent;
}

/*============================================================================
 * Helper: Read Current Signal Strength
 *============================================================================*/
static int read_signal_percent(void)
{
    union SceNetApctlInfo info;
    int ret;
    int rssi;

    memset(&info, 0, sizeof(info));

    /* Get RSSI from PSP WiFi subsystem (code 5 = RSSI as integer) */
    ret = sceNetApctlGetInfo(SCE_NET_APCTL_INFO_RSSI, &info);
    if (ret < 0)
    {
        /* If we can't read signal, assume moderate signal */
        return 50;
    }

    /* The union stores the RSSI in the first int member */
    rssi = *(int *)&info;
    return rssi_to_percent(rssi);
}

/*============================================================================
 * Public API Implementation
 *============================================================================*/

void signal_strength_init(int base_bitrate_kbps)
{
    memset(&g_signal_state, 0, sizeof(g_signal_state));

    g_signal_state.base_bitrate = base_bitrate_kbps;
    g_signal_state.current_bitrate = base_bitrate_kbps;
    g_signal_state.signal_percent = 50;  /* Default moderate signal */
    g_signal_state.wifi_icon_visible = 0;
    g_signal_state.icon_show_time = 0;
    g_signal_state.last_check_time = sceKernelGetSystemTimeLow();

    g_initialized = 1;

    pspDebugScreenPrintf("signal: initialized (base bitrate: %d kbps)\n",
                         base_bitrate_kbps);
}

int signal_strength_update(void)
{
    u32 current_time;
    int signal_percent;
    int bitrate_delta;

    if (!g_initialized)
        return g_signal_state.current_bitrate;

    current_time = sceKernelGetSystemTimeLow();

    /* Check if enough time has passed since last check */
    if ((current_time - g_signal_state.last_check_time) < SIGNAL_CHECK_INTERVAL_US)
    {
        return g_signal_state.current_bitrate;
    }

    g_signal_state.last_check_time = current_time;

    /* Read current signal strength */
    signal_percent = read_signal_percent();
    g_signal_state.signal_percent = signal_percent;

    /* Periodic signal strength log for diagnostics */
    g_signal_log_counter++;
    if (g_signal_log_counter >= SIGNAL_LOG_INTERVAL) {
        diag_log_write("SIG", "signal=%d%% bitrate=%dkbps (base=%d)\n",
                       signal_percent, g_signal_state.current_bitrate,
                       g_signal_state.base_bitrate);
        g_signal_log_counter = 0;
    }

    /* Adjust bitrate based on signal strength */
    if (signal_percent < SIGNAL_FAIR)
    {
        /* Signal below 40%: Drop bitrate by 50 kbps */
        bitrate_delta = BITRATE_DROP_KBPS;

        /* Apply bitrate reduction */
        g_signal_state.current_bitrate -= bitrate_delta;

        /* Enforce minimum bitrate floor */
        if (g_signal_state.current_bitrate < BITRATE_MIN_KBPS)
        {
            g_signal_state.current_bitrate = BITRATE_MIN_KBPS;
        }

        /* Show Wi-Fi warning icon */
        g_signal_state.wifi_icon_visible = 1;
        g_signal_state.icon_show_time = current_time;

        pspDebugScreenPrintf("signal: weak (%d%%) - bitrate: %d kbps\n",
                             signal_percent, g_signal_state.current_bitrate);
    }
    else if (signal_percent >= SIGNAL_GOOD)
    {
        /* Signal above 60%: Gradually recover bitrate */
        if (g_signal_state.current_bitrate < g_signal_state.base_bitrate)
        {
            /* Recover at 50 kbps per check */
            g_signal_state.current_bitrate += BITRATE_MAX_RECOVERY;

            /* Don't exceed base bitrate */
            if (g_signal_state.current_bitrate > g_signal_state.base_bitrate)
            {
                g_signal_state.current_bitrate = g_signal_state.base_bitrate;
            }
        }

        /* Hide Wi-Fi icon after timeout */
        if (g_signal_state.wifi_icon_visible)
        {
            if ((current_time - g_signal_state.icon_show_time) >= ICON_DISPLAY_DURATION_US)
            {
                g_signal_state.wifi_icon_visible = 0;
            }
        }
    }
    else
    {
        /* Signal between 40-60%: Maintain current bitrate */
        /* Still check icon timeout */
        if (g_signal_state.wifi_icon_visible)
        {
            if ((current_time - g_signal_state.icon_show_time) >= ICON_DISPLAY_DURATION_US)
            {
                g_signal_state.wifi_icon_visible = 0;
            }
        }
    }

    return g_signal_state.current_bitrate;
}

int signal_strength_get_current(void)
{
    if (!g_initialized)
        return 50;

    return g_signal_state.signal_percent;
}

int signal_strength_get_bitrate(void)
{
    if (!g_initialized)
        return BITRATE_MIN_KBPS;

    return g_signal_state.current_bitrate;
}

int signal_strength_should_show_icon(void)
{
    if (!g_initialized)
        return 0;

    return g_signal_state.wifi_icon_visible;
}

void signal_strength_shutdown(void)
{
    g_initialized = 0;
    memset(&g_signal_state, 0, sizeof(g_signal_state));
}