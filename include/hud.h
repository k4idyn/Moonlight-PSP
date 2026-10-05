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
    int   latency_ms;       /* Smoothed decode-to-present age in milliseconds */
    int   latency_valid;
    float fps;              /* Display frames per second */
    float packet_loss_pct;  /* Video packet loss percentage */
    int   fec_active;       /* Host RTP metadata reported parity in the sample */
    int   fec_metric_valid; /* 1 only when this sample saw recovery outcomes */
    float fec_recovery_pct; /* recovered/(recovered+failed), otherwise undefined */
    u32   fec_attempts;
    u32   fec_recovered_packets;
    u32   fec_failed_packets;
    int   battery_pct;      /* PSP battery percentage; negative when unavailable */
    int   host_proc_us;     /* Sunshine frame-header duration in microseconds */
    int   host_proc_valid;  /* 1 only after a frame header supplied the value */
    int   decode_us;        /* Most recent Sony/OpenH264 decode-call wall time */
    int   decode_valid;
    int   cpu_pct;          /* Software-decoder CPU time share; unavailable for Sony AVC */
    int   gu_share_pct;     /* GU submit/sync wall-time share, not GPU-core utilization */
    int   me_pct;           /* Software Media Engine work share; unavailable for Sony AVC */
    u32   gu_submit_sync_us;/* Most recent GU submit + sync wall time */
    int   gu_frame_valid;
    int   ram_free_kb;      /* Current free RAM in KB */
    int   ram_largest_kb;   /* Largest free memory block in KB */
    int   ram_free_delta_kb;/* Stream-start free RAM minus current free RAM */
    int   bw_rx_kbps;       /* Raw UDP media bandwidth */
    int   bw_usable_kbps;   /* H.264 bytes accepted by reassembly */
    int   bw_audio_kbps;    /* Raw UDP audio bandwidth */
    int   bw_drop_kbps;     /* Video bytes dropped before decode */
    int   bw_usable_pct;    /* H.264 usable bytes / raw video bytes */
    int   bw_video_packets_s; /* Video packets per second */
    int   audio_enabled;
    int   audio_active;
    u32   audio_frames_played;
    u32   audio_empty_holds;
    u32   audio_underruns;
    u32   audio_ring_drops;
    u32   audio_plc;
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
 * hud_overlay_visible - Check if the HUD overlay itself should be drawn.
 *
 * hud_is_visible() also stays true during post-toggle input cooldown, while
 * this returns only the real overlay state.
 */
int hud_overlay_visible(void);

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
