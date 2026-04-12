/*
 * stream_connect_ui.h - "Connecting to Stream" transition screen
 *
 * Runs a 60 fps background render thread that smoothly animates the progress
 * bar between phases.  Call stream_connect_start() before the first phase,
 * stream_connect_draw() to update the phase, and stream_connect_stop() when
 * the handshake is complete or cancelled.
 */

#ifndef STREAM_CONNECT_UI_H
#define STREAM_CONNECT_UI_H

#ifdef __cplusplus
extern "C" {
#endif

/* Connection phase identifiers ------------------------------------------- */
#define STREAM_PHASE_RTSP       0   /* "Requesting Stream..."                */
#define STREAM_PHASE_CONTROL    1   /* "Negotiating RTSP..."                 */
#define STREAM_PHASE_VIDEO      2   /* "Starting Playback..."                */
#define STREAM_PHASE_READY      3   /* "Stream Ready"                        */

/*
 * stream_connect_start - Spawn the 60 fps background render thread.
 *
 * Call once before the first stream_connect_draw().  Safe to call multiple
 * times (duplicate calls are no-ops).
 */
void stream_connect_start(void);

/*
 * stream_connect_draw - Update the displayed phase (non-blocking).
 *
 * @game_title  Display name of the selected game (may be NULL or "").
 * @phase       One of STREAM_PHASE_* constants above.
 *
 * The background thread smoothly sweeps the progress bar toward the target
 * percentage for the given phase.  This function just updates shared state
 * and returns immediately.
 */
void stream_connect_draw(const char *game_title, int phase);

/*
 * stream_connect_stop - Tear down the background render thread.
 *
 * Blocks until the thread has exited.  Safe to call even if the thread
 * was never started.
 */
void stream_connect_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* STREAM_CONNECT_UI_H */
