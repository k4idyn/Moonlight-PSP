/*
 * netconf_ui.h - WiFi Network Configuration UI for PSP Moonlight
 *
 * Uses the PSP's native Network Configuration Utility (sceUtilityNetconfInitStart)
 * to present the built-in WiFi connection dialog, then waits for the PSP's
 * IP stack to be fully ready before returning.
 */

#ifndef NETCONF_UI_H
#define NETCONF_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * netconf_ui_run - Show the WiFi network setup dialog and wait for connection.
 *
 * Presents the PSP's built-in Access Point selection dialog, then polls
 * sceNetApctlGetState() until state 4 (IP acquired).  Draws a "Connecting…"
 * spinner frame while the dialog is active so the screen is never blank.
 *
 * Timeout: 60 seconds after the dialog closes.
 *
 * Returns:
 *   0  on success (WiFi connected, IP assigned)
 *  -1  if user cancelled the dialog
 *  -2  if sceUtilityNetconfInitStart failed (modules probably not loaded)
 *  -3  if IP acquisition timed out (60 s)
 */
int netconf_ui_run(void);

#ifdef __cplusplus
}
#endif

#endif /* NETCONF_UI_H */
