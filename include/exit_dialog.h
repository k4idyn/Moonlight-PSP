/*
 * exit_dialog.h - Quit/Exit confirmation dialog for PSP Moonlight
 */

#ifndef EXIT_DIALOG_H
#define EXIT_DIALOG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * exit_dialog_run - Show the exit confirmation dialog.
 *
 * Returns 1 when the user confirms exit or automation has requested app exit.
 * Returns 0 when the user dismisses the dialog and the caller should continue.
 */
int exit_dialog_run(void);

#ifdef __cplusplus
}
#endif

#endif /* EXIT_DIALOG_H */
