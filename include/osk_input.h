/*
 * osk_input.h - PSP On-Screen Keyboard wrapper for IP address entry
 *
 * Thin wrapper around sceUtilityOskInitStart that presents the PSP's
 * native on-screen keyboard configured for numeric/Latin input and
 * validates the result as an IPv4 address.
 */

#ifndef OSK_INPUT_H
#define OSK_INPUT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * osk_get_ip_input - Show the PSP native OSK and capture an IPv4 address.
 *
 * @out_ip   Buffer to receive the validated IP string (at least 16 bytes).
 * @max_len  Size of out_ip buffer (should be >= 16).
 *
 * Returns:
 *   0   on success — out_ip contains a valid IPv4 string, e.g. "192.168.1.5"
 *  -1   user cancelled the OSK
 *  -2   sceUtilityOskInitStart failed (modules not loaded)
 *  -3   text was entered but failed IPv4 validation
 */
int osk_get_ip_input(char *out_ip, int max_len);

/*
 * osk_get_resolution_input - Show a keypad for entering a custom resolution.
 *
 * Keypad replaces '.' with 'x' separator.  A "width x height" tooltip
 * is shown in the text bar and disappears once the user starts typing.
 *
 * @out_width   Receives parsed width (renderer-safe, mod-16 aligned).
 * @out_height  Receives parsed height (renderer-safe, mod-16 aligned).
 *
 * Returns:
 *   0   on success
 *  -1   user cancelled
 *  -3   invalid resolution entered
 */
int osk_get_resolution_input(int *out_width, int *out_height);

/*
 * osk_get_fps_input - Show a keypad for entering a custom FPS.
 *
 * @out_fps  Receives parsed FPS (>0, <=120).
 *
 * Returns:
 *   0   on success
 *  -1   user cancelled
 *  -3   invalid FPS entered
 */
int osk_get_fps_input(int *out_fps);

#ifdef __cplusplus
}
#endif

#endif /* OSK_INPUT_H */
