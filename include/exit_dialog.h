/*
 * exit_dialog.h - Quit/Exit confirmation dialog for PSP Moonlight
 *
 * Presents a modal "Exit Moonlight?" Yes/No prompt.  On confirmation it
 * performs a safe, ordered shutdown of all active subsystems and calls
 * sceKernelExitGame().
 */

#ifndef EXIT_DIALOG_H
#define EXIT_DIALOG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * exit_dialog_run - Show the exit confirmation dialog.
 *
 * Draws a centered "Exit Moonlight?" panel over the current frame.
 * Blocks until the user makes a choice.
 *
 * If the user presses Cross (Yes):
 *   - Calls hud_shutdown(), ui_manager_shutdown()
 *   - Calls end_stream_session() for network/ME cleanup
 *   - Calls sceKernelExitGame()  [does not return]
 *
 * If the user presses Circle (No):
 *   - Returns 0 immediately; the caller resumes normally.
 *
 * Returns:
 *   0   user chose "No" (dialog dismissed, caller continues)
 *       [never returns on "Yes" — sceKernelExitGame() is called]
 */
int exit_dialog_run(void);

#ifdef __cplusplus
}
#endif

#endif /* EXIT_DIALOG_H */
