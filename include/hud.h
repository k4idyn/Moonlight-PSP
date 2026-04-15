/*
 * hud.h - Heads-Up Display overlay for PSP Moonlight streaming
 *
 * Provides an alpha-blended overlay triggered by Home/Note buttons.
 * Displays streaming statistics (latency, packet loss) and a quit option.
 */

#ifndef HUD_H
#define HUD_H

#include <psptypes.h>

/*============================================================================
 * HUD Statistics Structure
 *============================================================================*/
typedef struct {
    int   latency_ms;       /* Current latency in milliseconds */
    float fps;              /* Display frames per second */
    float packet_loss_pct;  /* Video packet loss percentage */
    float fec_recovery_pct; /* FEC recovery success percentage */
    int   battery_pct;      /* PSP battery percentage (0-100) */
    int   host_proc_ms;     /* Host-side encode latency in ms */
} HudStats;

/*============================================================================
 * Public API
 *============================================================================*/

/*
 * hud_init - Initialize the HUD subsystem
 *
 * Must be called after display_init() to ensure GU is ready.
 */
void hud_init(void);

/*
 * hud_update_stats - Update the displayed statistics
 *
 * @stats: Pointer to stats structure with current values
 *
 * Call this periodically to refresh the displayed latency and loss values.
 */
void hud_update_stats(const HudStats *stats);

/*
 * hud_render - Render the HUD overlay if active
 *
 * Should be called once per frame AFTER drawing the video frame.
 * Only renders if the HUD is currently visible.
 */
void hud_render(void);

/*
 * hud_handle_input - Process controller input for HUD toggling
 *
 * @buttons: Current button state from sceCtrlPeekBufferPositive
 *
 * Returns: 1 if quit was selected (caller should end session), 0 otherwise
 *
 * Toggles HUD visibility on Home/Note press.
 * Handles menu navigation when HUD is visible.
 */
int hud_handle_input(u32 buttons);

/*
 * hud_is_visible - Check if HUD is currently showing
 *
 * Returns: 1 if visible, 0 if hidden
 */
int hud_is_visible(void);

/*
 * hud_shutdown - Clean up HUD resources
 */
void hud_shutdown(void);

/*
 * hud_show_wifi_icon - Display a brief Wi-Fi warning icon
 *
 * Call this when signal strength drops to show a yellow Wi-Fi icon
 * in the top-left corner of the HUD for 2 seconds.
 */
void hud_show_wifi_icon(void);

/*
 * hud_should_show_wifi_icon - Check if Wi-Fi icon should be displayed
 *
 * Returns: 1 if icon should be visible, 0 if hidden
 */
int hud_should_show_wifi_icon(void);
/*
 * hud_show_rewind_icon - Display a rewind/pause icon for buffering
 *
 * Call this when packet loss is detected to show a buffering indicator
 * in the center of the screen for 2 seconds.
 */
void hud_show_rewind_icon(void);

/*
 * hud_hide_rewind_icon - Immediately hide the rewind/pause icon
 */
void hud_hide_rewind_icon(void);

/*
 * hud_should_show_rewind_icon - Check if rewind icon should be displayed
 *
 * Returns: 1 if icon should be visible, 0 if hidden
 */
int hud_should_show_rewind_icon(void);

#endif /* HUD_H */
