/*
 * stream_session.h - Stream session management for PSP Moonlight
 *
 * Provides functions to properly end a streaming session and return to main menu.
 */

#ifndef STREAM_SESSION_H
#define STREAM_SESSION_H

/*
 * stream_session_set_input_socket - Store the input socket for cleanup
 *
 * @sock: The UDP socket used for controller input
 *
 * This should be called after input_init() to allow proper socket cleanup
 * during session termination.
 */
void stream_session_set_input_socket(int sock);

/*
 * abort_stream_to_menu - Gracefully stop streaming without killing the app
 * 
 * Safely tears down the network, decoder, audio, and HUD threads, freeing
 * memory without destroying the GU display or exiting to the PSP OS, enabling
 * a clean jump back to the Host Discovery loop.
 */
void abort_stream_to_menu(void);

/*
 * end_stream_session - Gracefully end the current streaming session
 *
 * This function:
 * 1. Sends a termination packet to the Host PC via LiStopConnection()
 * 2. Stops the Media Engine
 * 3. Closes the input socket
 * 4. Shuts down the HUD
 * 5. Shuts down the network receive thread
 * 6. Shuts down the decoder
 * 7. Shuts down the display
 * 8. Disconnects from WiFi
 * 9. Returns to the PSP main menu
 *
 * Should be called when the user selects Quit from the HUD.
 */
void end_stream_session(void);

#endif /* STREAM_SESSION_H */
