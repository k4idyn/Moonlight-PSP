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
#define BITRATE_DROP_KBPS    50    /* Drop 50 kbps per check when signal < 40% */
#define BITRATE_MIN_KBPS     100   /* Minimum bitrate floor (100 kbps) */
#define BITRATE_MAX_RECOVERY 50    /* Recover 50 kbps per check when signal > 60% */

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

#ifdef __cplusplus
}
#endif

#endif /* SIGNAL_STRENGTH_H */