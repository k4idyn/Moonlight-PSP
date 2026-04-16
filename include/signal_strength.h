/*
 * signal_strength.h - Wi-Fi Signal Strength Monitor for PSP Moonlight
 *
 * Monitors PSP Wi-Fi signal quality and dynamically adjusts streaming bitrate
 * to prevent frame stutters when signal degrades.
 */

#ifndef SIGNAL_STRENGTH_H
#define SIGNAL_STRENGTH_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Signal Strength Constants
 *--------------------------------------------------------------------------*/

/* Signal quality thresholds (percentage) */
#define SIGNAL_EXCELLENT     80    /* 80-100%: Full bitrate */
#define SIGNAL_GOOD          60    /* 60-79%: Minor adjustment */
#define SIGNAL_FAIR          40    /* 40-59%: Moderate reduction */
#define SIGNAL_WEAK          20    /* 20-39%: Aggressive reduction */
#define SIGNAL_CRITICAL      0     /* 0-19%: Minimum bitrate */

/* Bitrate adjustment parameters */
#define BITRATE_DROP_KBPS    50    /* Legacy: Drop 50 kbps per check when signal < 40% */
#define BITRATE_MIN_KBPS     32    /* Minimum bitrate floor (32 kbps) */
#define BITRATE_MAX_RECOVERY 50    /* Legacy: Recover 50 kbps per check when signal > 60% */

/* PID controller tuning constants (fixed-point x1000 for integer math) */
#define PID_KP  300   /* Proportional gain (0.300) — fast response to error */
#define PID_KI  100   /* Integral gain (0.100) — eliminate steady-state offset */
#define PID_KD   50   /* Derivative gain (0.050) — damp oscillations */
#define PID_SCALE 1000 /* Fixed-point scale factor */

/* PID integral windup limits (in kbps * PID_SCALE) */
#define PID_INTEGRAL_MAX  (500 * PID_SCALE)   /* max integral accumulation */
#define PID_INTEGRAL_MIN  (-500 * PID_SCALE)  /* min integral accumulation */

/* Maximum bitrate change per update (anti-jitter, in kbps) */
#define PID_MAX_DELTA_KBPS  150

/* Phase 4: Adaptive bitrate constants */
#define ADAPT_FAST_DROP_THRESHOLD  3      /* consecutive drops to trigger halve */
#define ADAPT_SLOW_RECOVER_KBPS   50     /* kbps per second recovery rate (was 25) */
#define ADAPT_GREEN_HOLDOFF_US    (5 * 1000 * 1000) /* 5s green before recovery */
#define ADAPT_DEADZONE_PCT        15     /* +/-15% stability band */
#define ADAPT_CEILING_MAX_KBPS    4000   /* absolute bitrate ceiling */
#define ADAPT_CEILING_QUALITY_MULT 50    /* ceiling = quality * this */

/* Phase 4: RSSI/jitter history sizes */
#define SIGNAL_RSSI_HISTORY_SIZE  16
#define SIGNAL_JITTER_HISTORY_SIZE 16

/* Phase 4: Signal trend indicators */
typedef enum {
    SIGNAL_TREND_IMPROVING = 0,
    SIGNAL_TREND_STABLE    = 1,
    SIGNAL_TREND_DEGRADING = 2
} SignalTrend;

/*--------------------------------------------------------------------------
 * Signal Strength State Structure
 *--------------------------------------------------------------------------*/
typedef struct {
    int signal_percent;      /* Current signal strength (0-100%) */
    int current_bitrate;     /* Current bitrate in kbps */
    int base_bitrate;        /* Base bitrate from config */
    int wifi_icon_visible;   /* Whether to show Wi-Fi warning icon */
    u32 icon_show_time;      /* Timestamp when icon was shown */
    u32 last_check_time;     /* Last time signal was checked */

    /* PID controller state */
    int pid_error_prev;      /* previous error for derivative term */
    int pid_integral;        /* accumulated integral (fixed-point x PID_SCALE) */
    int pid_target_quality;  /* target quality setpoint (0-100) */

    /* Phase 4: Adaptive bitrate controller state */
    volatile int adapt_consecutive_drops; /* consecutive unrecoverable frames */
    u32 adapt_last_green_time;            /* timestamp when all signals went green */
    int adapt_in_recovery;                /* 1 = slow-recover active */
    int adapt_prev_bitrate;               /* for dead-zone check */

    /* Phase 4: WiFi jitter/trend tracking */
    int rssi_history[SIGNAL_RSSI_HISTORY_SIZE];
    int rssi_history_idx;
    int rssi_history_count;
    u32 jitter_samples[SIGNAL_JITTER_HISTORY_SIZE];
    int jitter_idx;
    int jitter_count;
    SignalTrend current_trend;
} SignalState;

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/*
 * signal_strength_init - Initialize signal strength monitor
 *
 * @base_bitrate_kbps: Initial bitrate from user configuration
 *
 * Initializes the signal monitoring subsystem.
 * Must be called after WiFi connection is established.
 */
void signal_strength_init(int base_bitrate_kbps);

/*
 * signal_strength_update - Check signal and adjust bitrate
 *
 * Call this periodically (e.g., every 1-2 seconds) during streaming.
 * Reads current WiFi signal quality and adjusts bitrate accordingly:
 * - Signal >= 40%: Gradually recover toward base bitrate
 * - Signal < 40%: Drop bitrate by 500 kbps (triggers Wi-Fi icon)
 *
 * Returns: Current adjusted bitrate in kbps
 */
int signal_strength_update(void);

/*
 * signal_strength_get_current - Get current signal percentage
 *
 * Returns: Signal strength as percentage (0-100)
 */
int signal_strength_get_current(void);

/*
 * signal_strength_get_bitrate - Get current adjusted bitrate
 *
 * Returns: Current bitrate in kbps
 */
int signal_strength_get_bitrate(void);

/*
 * signal_strength_should_show_icon - Check if Wi-Fi warning icon should display
 *
 * Returns: 1 if icon should be visible, 0 if hidden
 */
int signal_strength_should_show_icon(void);

/*
 * signal_strength_shutdown - Clean up signal strength monitor
 */
void signal_strength_shutdown(void);

/* Phase 4: Get signal quality trend */
SignalTrend signal_strength_get_trend(void);

/* Phase 4: Report unrecoverable frame drop (called by rtp_fec.c) */
void signal_strength_report_frame_drop(void);

/* Phase 4: Report successful frame delivery (called by rtp_fec.c) */
void signal_strength_report_frame_ok(void);

/* Phase 4: Report inter-packet arrival jitter (called by network_me.c) */
void signal_strength_report_jitter(u32 delta_us);

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_STRENGTH_H */