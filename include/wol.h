/*
 * wol.h - Wake-on-LAN magic packet and confirmation UI
 *
 * Sends a standard IEEE 802.3 Wake-on-LAN magic packet (102 bytes) to the
 * broadcast address on UDP port 9, and presents a simple Yes/No confirmation
 * dialog so the user can trigger it from the host discovery screen.
 */

#ifndef WOL_H
#define WOL_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * wol_send_magic_packet - Broadcast a Wake-on-LAN magic packet.
 *
 * @mac_str  MAC address string in "XX:XX:XX:XX:XX:XX" format (hex, colons).
 *           Accepts both upper and lower-case hex digits.
 *
 * Builds the 102-byte magic payload (6 × 0xFF followed by 16 repetitions
 * of the 6-byte MAC) and sends it as a UDP broadcast to 255.255.255.255:9.
 *
 * Returns:
 *   0   on success (datagram sent; host may not wake if asleep too deep)
 *  -1   invalid MAC string (parse failed)
 *  -2   socket creation / send error
 */
int wol_send_magic_packet(const char *mac_str);

/*
 * wol_show_confirm - Draw a confirmation dialog and optionally send WOL.
 *
 * @host_name  Display name of the target host (fits in the dialog panel).
 * @mac_str    MAC address string, forwarded to wol_send_magic_packet().
 *
 * Shows a semi-transparent modal with:
 *   "Wake [host_name]?"
 *   [X] Send  [O] Cancel
 *
 * If the user presses Cross, calls wol_send_magic_packet() and draws a
 * "WOL Sent!" toast for approximately 2 seconds.
 *
 * Returns:
 *   1   WOL packet was sent (user confirmed)
 *   0   user cancelled
 */
int wol_show_confirm(const char *host_name, const char *mac_str);

#ifdef __cplusplus
}
#endif

#endif /* WOL_H */
