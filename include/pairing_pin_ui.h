/*
 * pairing_pin_ui.h - PSP UI component for displaying pairing PIN
 *
 * Displays a 4-digit pairing PIN centered on screen in large font
 * with instructional text below. Periodically checks for pairing
 * completion and triggers transition to Library Screen.
 */

#ifndef PAIRING_PIN_UI_H
#define PAIRING_PIN_UI_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Configuration
 *--------------------------------------------------------------------------*/
#define PIN_DIGITS              4
#define PIN_CHECK_INTERVAL_US   500000  /* Check isPaired every 500ms */

/*--------------------------------------------------------------------------
 * PairingPINState - State machine for the pairing PIN UI
 *--------------------------------------------------------------------------*/
typedef enum {
    PAIRING_PIN_STATE_ACTIVE,       /* Displaying PIN, waiting for pairing */
    PAIRING_PIN_STATE_PAIRED,       /* Pairing successful, transition ready */
    PAIRING_PIN_STATE_CANCELLED     /* User cancelled (pressed HOME/CIRCLE) */
} PairingPINState;

/*--------------------------------------------------------------------------
 * PairingPINUI - Main UI component structure
 *--------------------------------------------------------------------------*/
typedef struct {
    char            pin[PIN_DIGITS + 1];    /* 4-digit PIN + null terminator */
    volatile int   *isPaired;               /* Pointer to external paired flag */
    volatile int   *threadDone;             /* Pairing worker completion flag */
    PairingPINState state;                  /* Current UI state */
    u32             last_check_time;        /* Last time we checked isPaired */
    int             initialized;            /* Initialization flag */
} PairingPINUI;

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/**
 * pairing_pin_ui_init - Initialize the pairing PIN UI component
 *
 * @ui:         Pointer to PairingPINUI structure
 * @pin:        4-digit PIN string to display (must be null-terminated)
 * @isPaired:   Pointer to external bool/int that indicates pairing status
 *
 * Returns: 0 on success, negative on error
 */
int pairing_pin_ui_init(PairingPINUI *ui, const char *pin,
                        volatile int *isPaired, volatile int *threadDone);

/**
 * pairing_pin_ui_run - Run the pairing PIN UI loop
 *
 * Displays the PIN screen and periodically checks for pairing completion.
 * Blocks until pairing is complete or user cancels.
 *
 * @ui: Pointer to initialized PairingPINUI structure
 *
 * Returns: PAIRING_PIN_STATE_PAIRED if pairing succeeded
 *          PAIRING_PIN_STATE_CANCELLED if user cancelled
 */
PairingPINState pairing_pin_ui_run(PairingPINUI *ui);

/**
 * pairing_pin_ui_shutdown - Clean up the pairing PIN UI
 *
 * @ui: Pointer to PairingPINUI structure
 */
void pairing_pin_ui_shutdown(PairingPINUI *ui);

/**
 * pairing_pin_ui_get_state - Get current UI state
 *
 * @ui: Pointer to PairingPINUI structure
 *
 * Returns: Current PairingPINState
 */
PairingPINState pairing_pin_ui_get_state(const PairingPINUI *ui);

#ifdef __cplusplus
}
#endif

#endif /* PAIRING_PIN_UI_H */