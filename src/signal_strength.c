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
#include "control_stream.h"
#include "rtp_fec.h"

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

int signal_strength_get_launch_bitrate_kbps(int configured_bitrate_kbps)
{
    int launch_bitrate_kbps = configured_bitrate_kbps > 0 ? configured_bitrate_kbps : 1600;

    launch_bitrate_kbps = (launch_bitrate_kbps * 50 + 99) / 100;
    if (launch_bitrate_kbps < BITRATE_MIN_KBPS)
        launch_bitrate_kbps = BITRATE_MIN_KBPS;

    return launch_bitrate_kbps;
}

void signal_strength_init(int base_bitrate_kbps)
{
    memset(&g_signal_state, 0, sizeof(g_signal_state));

    g_signal_state.base_bitrate = base_bitrate_kbps;
    g_signal_state.current_bitrate = base_bitrate_kbps;
    g_signal_state.signal_percent = 50;  /* Default moderate signal */
    g_signal_state.wifi_icon_visible = 0;
    g_signal_state.icon_show_time = 0;
    g_signal_state.last_check_time = sceKernelGetSystemTimeLow();

    /* PID controller: target 75% quality setpoint for release-stability bias. */
    g_signal_state.pid_error_prev = 0;
    g_signal_state.pid_integral = 0;
    g_signal_state.pid_target_quality = 75;

    /* Phase 4: Adaptive bitrate controller init */
    g_signal_state.adapt_consecutive_drops = 0;
    g_signal_state.adapt_last_green_time = 0;
    g_signal_state.adapt_in_recovery = 0;
    g_signal_state.adapt_prev_bitrate = base_bitrate_kbps;
    g_signal_state.rssi_history_idx = 0;
    g_signal_state.rssi_history_count = 0;
    g_signal_state.jitter_idx = 0;
    g_signal_state.jitter_count = 0;
    g_signal_state.current_trend = SIGNAL_TREND_STABLE;

    g_initialized = 1;

    pspDebugScreenPrintf("signal: initialized (base bitrate: %d kbps)\n",
                         base_bitrate_kbps);
}

int signal_strength_update(void)
{
    u32 current_time;
    int signal_percent;
    int composite_quality;
    int cq_score = 50;
    int fec_score = 50;
    int error, derivative, pid_output, delta_kbps;

    if (!g_initialized)
        return g_signal_state.current_bitrate;

    current_time = sceKernelGetSystemTimeLow();

    /* Check if enough time has passed since last check */
    if ((current_time - g_signal_state.last_check_time) < SIGNAL_CHECK_INTERVAL_US)
    {
        return g_signal_state.current_bitrate;
    }

    g_signal_state.last_check_time = current_time;

    /* ── Multi-signal composite quality (0-100) ────────────────────
     * Blend RSSI with FEC loss/recovery stats and connection quality
     * for a more accurate picture than RSSI alone. Weights:
     *   RSSI: 40%  — physical layer signal
     *   Connection quality: 30% — FEC-based transport assessment
     *   FEC recovery rate: 30% — RS codec effectiveness
     * All values are 0-100 scale. */
    signal_percent = read_signal_percent();
    g_signal_state.signal_percent = signal_percent;

    /* Phase 4: Record RSSI into history for trend detection */
    g_signal_state.rssi_history[g_signal_state.rssi_history_idx] = signal_percent;
    g_signal_state.rssi_history_idx =
        (g_signal_state.rssi_history_idx + 1) % SIGNAL_RSSI_HISTORY_SIZE;
    if (g_signal_state.rssi_history_count < SIGNAL_RSSI_HISTORY_SIZE)
        g_signal_state.rssi_history_count++;

    /* Phase 4: Compute signal trend from RSSI history + jitter */
    if (g_signal_state.rssi_history_count >= 4) {
        int recent_avg = 0, older_avg = 0;
        int recent_n = 0, older_n = 0;
        int half = g_signal_state.rssi_history_count / 2;
        int ti;
        for (ti = 0; ti < g_signal_state.rssi_history_count; ti++) {
            int tidx = (g_signal_state.rssi_history_idx
                        - g_signal_state.rssi_history_count + ti
                        + SIGNAL_RSSI_HISTORY_SIZE) % SIGNAL_RSSI_HISTORY_SIZE;
            if (ti < half) {
                older_avg += g_signal_state.rssi_history[tidx];
                older_n++;
            } else {
                recent_avg += g_signal_state.rssi_history[tidx];
                recent_n++;
            }
        }
        if (older_n > 0) older_avg /= older_n;
        if (recent_n > 0) recent_avg /= recent_n;
        {
            u32 avg_jitter = 0;
            if (g_signal_state.jitter_count > 0) {
                u32 jsum = 0;
                int ji;
                for (ji = 0; ji < g_signal_state.jitter_count; ji++)
                    jsum += g_signal_state.jitter_samples[ji];
                avg_jitter = jsum / (u32)g_signal_state.jitter_count;
            }
            {
                SignalTrend prev_trend = g_signal_state.current_trend;
                if (recent_avg > older_avg + 5 && avg_jitter < 50000)
                    g_signal_state.current_trend = SIGNAL_TREND_IMPROVING;
                else if (recent_avg < older_avg - 5 || avg_jitter > 100000)
                    g_signal_state.current_trend = SIGNAL_TREND_DEGRADING;
                else
                    g_signal_state.current_trend = SIGNAL_TREND_STABLE;
                if (g_signal_state.current_trend != prev_trend) {
                    pspDebugScreenPrintf("[PHASE4-WIFI] trend: %s\n",
                        g_signal_state.current_trend == SIGNAL_TREND_IMPROVING ? "IMPROVING" :
                        g_signal_state.current_trend == SIGNAL_TREND_DEGRADING ? "DEGRADING" : "STABLE");
                    diag_log_write("SIG", "[PHASE4-WIFI] trend: %s (recent=%d older=%d jitter=%u)\n",
                        g_signal_state.current_trend == SIGNAL_TREND_IMPROVING ? "IMPROVING" :
                        g_signal_state.current_trend == SIGNAL_TREND_DEGRADING ? "DEGRADING" : "STABLE",
                        recent_avg, older_avg, avg_jitter);
                }
            }
        }
    }

    {
        ConnQualityState cq = control_stream_get_quality();

        /* Map ConnQuality enum to 0-100 scale */
        switch (cq.quality) {
            case CONN_QUALITY_EXCELLENT: cq_score = 100; break;
            case CONN_QUALITY_GOOD:      cq_score = 75;  break;
            case CONN_QUALITY_FAIR:      cq_score = 50;  break;
            case CONN_QUALITY_POOR:      cq_score = 25;  break;
            case CONN_QUALITY_CRITICAL:  cq_score = 0;   break;
            default:                     cq_score = 50;  break;
        }

        /* FEC recovery percentage is already 0-100 */
        fec_score = (int)cq.fec_recovery_pct;

        /* Weighted composite: RSSI 20% + conn quality 45% + FEC recovery 35%.
         * Favor transport/decode health over raw RSSI to reduce optimistic bitrate.
         */
        composite_quality = (signal_percent * 20 + cq_score * 45 + fec_score * 35) / 100;
    }

    /* ── PID controller ────────────────────────────────────────────
     * error = target - actual (positive = quality below target → reduce bitrate)
     * Negative output = reduce bitrate, positive output = increase bitrate */
    error = composite_quality - g_signal_state.pid_target_quality;

    /* Integral accumulation with anti-windup clamping */
    g_signal_state.pid_integral += error;
    {
        int windup = 0;
        if (g_signal_state.pid_integral > PID_INTEGRAL_MAX) {
            g_signal_state.pid_integral = PID_INTEGRAL_MAX;
            windup = 1;
        }
        if (g_signal_state.pid_integral < PID_INTEGRAL_MIN) {
            g_signal_state.pid_integral = PID_INTEGRAL_MIN;
            windup = -1;
        }
        if (windup != 0) {
            static u32 s_windup_count = 0;
            s_windup_count++;
            if (s_windup_count <= 3 || (s_windup_count % 30) == 0)
                diag_log_write("SIG", "PID integral windup %s (clamped to %d) [#%u]\n",
                               windup > 0 ? "MAX" : "MIN",
                               g_signal_state.pid_integral, s_windup_count);
        }
    }

    /* Derivative (change in error) */
    derivative = error - g_signal_state.pid_error_prev;
    g_signal_state.pid_error_prev = error;

    /* PID output in fixed-point (kbps * PID_SCALE) */
    pid_output = (PID_KP * error) +
                 (PID_KI * g_signal_state.pid_integral / PID_SCALE) +
                 (PID_KD * derivative);

    /* Convert to kbps delta */
    delta_kbps = pid_output / PID_SCALE;

    /* Anti-jitter: clamp maximum change per update */
    if (delta_kbps > PID_MAX_DELTA_KBPS)
        delta_kbps = PID_MAX_DELTA_KBPS;
    if (delta_kbps < -PID_MAX_DELTA_KBPS)
        delta_kbps = -PID_MAX_DELTA_KBPS;

    /* Apply delta */
    g_signal_state.current_bitrate += delta_kbps;

    /* Clamp to valid range */
    if (g_signal_state.current_bitrate < BITRATE_MIN_KBPS) {
        g_signal_state.current_bitrate = BITRATE_MIN_KBPS;
        {
            static u32 s_min_clamp_count = 0;
            s_min_clamp_count++;
            if (s_min_clamp_count <= 3 || (s_min_clamp_count % 30) == 0)
                diag_log_write("SIG", "bitrate hit MIN floor %dkbps [#%u]\n",
                               BITRATE_MIN_KBPS, s_min_clamp_count);
        }
    }
    if (g_signal_state.current_bitrate > g_signal_state.base_bitrate) {
        g_signal_state.current_bitrate = g_signal_state.base_bitrate;
        {
            static u32 s_max_clamp_count = 0;
            s_max_clamp_count++;
            if (s_max_clamp_count <= 3 || (s_max_clamp_count % 30) == 0)
                diag_log_write("SIG", "bitrate hit MAX ceiling %dkbps [#%u]\n",
                               g_signal_state.base_bitrate, s_max_clamp_count);
        }
    }

    /* ── Phase 4: Adaptive Bitrate Overlay ────────────────────────
     * Fast-drop on consecutive drops, slow-recover when green,
     * dynamic ceiling, dead-zone to prevent oscillation. */
    {
        int adapt_ceiling;
        int adapt_floor = BITRATE_MIN_KBPS;

        /* Dynamic ceiling: min(4000, signal_quality * 50) */
        adapt_ceiling = composite_quality * ADAPT_CEILING_QUALITY_MULT;
        if (adapt_ceiling > ADAPT_CEILING_MAX_KBPS)
            adapt_ceiling = ADAPT_CEILING_MAX_KBPS;
        if (adapt_ceiling < adapt_floor)
            adapt_ceiling = adapt_floor;

        /* Fast-drop: 3+ consecutive unrecoverable → halve bitrate */
        if (g_signal_state.adapt_consecutive_drops >= ADAPT_FAST_DROP_THRESHOLD) {
            int halved = g_signal_state.current_bitrate / 2;
            if (halved < adapt_floor) halved = adapt_floor;
            if (halved < g_signal_state.current_bitrate) {
                pspDebugScreenPrintf("[PHASE4-ADAPT] fast-drop: %d->%d kbps\n",
                                    g_signal_state.current_bitrate, halved);
                diag_log_write("SIG", "[PHASE4-ADAPT] fast-drop: %d -> %d kbps (drops=%d)\n",
                               g_signal_state.current_bitrate, halved,
                               g_signal_state.adapt_consecutive_drops);
                g_signal_state.current_bitrate = halved;
            }
            g_signal_state.adapt_consecutive_drops = 0;
            g_signal_state.adapt_in_recovery = 0;
            g_signal_state.adapt_last_green_time = 0;
        }

        /* Slow-recover: +25kbps/s when all signals green for 5s */
        if (composite_quality >= SIGNAL_GOOD &&
            g_signal_state.adapt_consecutive_drops == 0) {
            if (g_signal_state.adapt_last_green_time == 0)
                g_signal_state.adapt_last_green_time = current_time;
            if ((current_time - g_signal_state.adapt_last_green_time)
                >= ADAPT_GREEN_HOLDOFF_US) {
                int old_br = g_signal_state.current_bitrate;
                g_signal_state.current_bitrate += ADAPT_SLOW_RECOVER_KBPS;
                if (!g_signal_state.adapt_in_recovery) {
                    pspDebugScreenPrintf("[PHASE4-ADAPT] slow-recover start\n");
                    diag_log_write("SIG", "[PHASE4-ADAPT] slow-recover: %d -> %d kbps\n",
                                   old_br, g_signal_state.current_bitrate);
                    g_signal_state.adapt_in_recovery = 1;
                }
            }
        } else {
            g_signal_state.adapt_last_green_time = 0;
            if (g_signal_state.adapt_in_recovery) {
                diag_log_write("SIG", "[PHASE4-ADAPT] recovery paused (q=%d drops=%d)\n",
                               composite_quality, g_signal_state.adapt_consecutive_drops);
                g_signal_state.adapt_in_recovery = 0;
            }
        }

        /* IDR avoidance: proactive reduction when quality trending down */
        if (g_signal_state.current_trend == SIGNAL_TREND_DEGRADING) {
            int reduction = g_signal_state.current_bitrate / 10;
            if (reduction < 25) reduction = 25;
            g_signal_state.current_bitrate -= reduction;
            pspDebugScreenPrintf("[PHASE4-ADAPT] IDR-avoid: -%d kbps\n", reduction);
        }

        /* Dead-zone: +/-15% stability band */
        {
            int prev = g_signal_state.adapt_prev_bitrate;
            int diff = g_signal_state.current_bitrate - prev;
            int threshold = (prev * ADAPT_DEADZONE_PCT) / 100;
            if (threshold < 10) threshold = 10;
            if (diff > -threshold && diff < threshold && diff != 0) {
                g_signal_state.current_bitrate = prev;
            } else {
                g_signal_state.adapt_prev_bitrate = g_signal_state.current_bitrate;
            }
        }

        /* Final clamp to adaptive floor/ceiling */
        if (g_signal_state.current_bitrate < adapt_floor)
            g_signal_state.current_bitrate = adapt_floor;
        if (g_signal_state.current_bitrate > adapt_ceiling)
            g_signal_state.current_bitrate = adapt_ceiling;
        if (g_signal_state.current_bitrate > g_signal_state.base_bitrate)
            g_signal_state.current_bitrate = g_signal_state.base_bitrate;
    }

    /* ── WiFi icon management ──────────────────────────────────────
     * Show icon when composite quality is poor */
    if (composite_quality < SIGNAL_FAIR) {
        g_signal_state.wifi_icon_visible = 1;
        g_signal_state.icon_show_time = current_time;
    } else if (g_signal_state.wifi_icon_visible) {
        if ((current_time - g_signal_state.icon_show_time) >= ICON_DISPLAY_DURATION_US) {
            g_signal_state.wifi_icon_visible = 0;
        }
    }

    /* Periodic logging */
    g_signal_log_counter++;
    if (g_signal_log_counter >= SIGNAL_LOG_INTERVAL) {
        diag_log_write("SIG", "PID: rssi=%d%% cq=%d%% fec=%d%% composite=%d%% err=%d I=%d delta=%dkbps bitrate=%dkbps\n",
                       signal_percent,
                       cq_score,
                       fec_score,
                       composite_quality, error,
                       g_signal_state.pid_integral / PID_SCALE,
                       delta_kbps, g_signal_state.current_bitrate);
        g_signal_log_counter = 0;
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

/*============================================================================
 * Phase 4: Adaptive Bitrate + WiFi Trend API
 *============================================================================*/

SignalTrend signal_strength_get_trend(void)
{
    if (!g_initialized)
        return SIGNAL_TREND_STABLE;
    return g_signal_state.current_trend;
}

void signal_strength_report_frame_drop(void)
{
    if (!g_initialized)
        return;
    g_signal_state.adapt_consecutive_drops++;
}

void signal_strength_report_frame_ok(void)
{
    if (!g_initialized)
        return;
    g_signal_state.adapt_consecutive_drops = 0;
}

void signal_strength_report_jitter(u32 delta_us)
{
    if (!g_initialized)
        return;
    g_signal_state.jitter_samples[g_signal_state.jitter_idx] = delta_us;
    g_signal_state.jitter_idx =
        (g_signal_state.jitter_idx + 1) % SIGNAL_JITTER_HISTORY_SIZE;
    if (g_signal_state.jitter_count < SIGNAL_JITTER_HISTORY_SIZE)
        g_signal_state.jitter_count++;
}