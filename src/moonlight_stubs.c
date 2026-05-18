/*
 * moonlight_stubs.c - Stub functions for Moonlight-common-c library
 *
 * These stubs allow the PSP client to compile and run without the
 * actual moonlight-common-c library. Replace these with real
 * implementations when integrating the library.
 */

#include <pspdebug.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "diag_log.h"

#define stub_log(fmt, ...) diag_log_write("STUB", fmt, ##__VA_ARGS__)

/*============================================================================
 * PSP newlib fixups - isfinitef referenced by libm but not exported
 *============================================================================*/
int isfinitef(float f) {
    return __builtin_isfinite(f);
}

/*============================================================================
 * Connection Management Stubs
 *============================================================================*/

/*
 * LiStopConnection - Stop the Moonlight streaming connection
 *
 * Stub: Prints a debug message. In production, this sends a termination
 * packet to the Sunshine host and tears down the RTSP session.
 */
extern void network_cancel_stream_session(void);

void LiStopConnection(void)
{
    network_cancel_stream_session();
}

/*============================================================================
 * Mouse Input Stubs
 *============================================================================*/

/*
 * LiSendMouseMoveEvent - Send a relative mouse movement event
 *
 * @deltaX: Horizontal movement delta
 * @deltaY: Vertical movement delta
 *
 * Stub: No-op. In production, this sends the mouse delta to the host
 * via the Moonlight controller input channel.
 *
 * Returns: 0 (success)
 */
int LiSendMouseMoveEvent(short deltaX, short deltaY)
{
    (void)deltaX;
    (void)deltaY;
    return 0;
}

/*
 * LiSendMouseButtonEvent - Send a mouse button press/release event
 *
 * @action: 0x07 = press, 0x08 = release
 * @button: 0x01 = left, 0x02 = middle, 0x03 = right
 *
 * Stub: No-op. In production, this sends the button event to the host.
 *
 * Returns: 0 (success)
 */
int LiSendMouseButtonEvent(char action, int button)
{
    (void)action;
    (void)button;
    return 0;
}

/*============================================================================
 * Pairing API Stubs (mbedTLS not installed — stubbed for compile)
 *============================================================================*/

#include "pairing.h"
#include "pairing_pin_ui.h"

/*
 * pairing_init - Initialize a pairing session (stub)
 */
int pairing_init(PairingSession *session, const char *server_ip,
                 unsigned short http_port, unsigned short https_port,
                 const char *unique_id, const char *device_name)
{
    if (!session) return -1;
    memset(session, 0, sizeof(PairingSession));
    strncpy(session->server_address, server_ip ? server_ip : "", 63);
    session->http_port = http_port;
    session->https_port = https_port;
    strncpy(session->unique_id, unique_id ? unique_id : "", 31);
    strncpy(session->device_name, device_name ? device_name : "", 63);
    session->state = PAIR_STATE_IDLE;
    stub_log("pairing_init() OK\n");
    return 0;
}

/*
 * pairing_start - Begin pairing process (stub — returns network error)
 */
int pairing_start(PairingSession *session)
{
    if (!session) return -1;
    stub_log("pairing_start() unavailable\n");
    session->state = PAIR_STATE_FAILED;
    session->result = PAIR_RESULT_NETWORK_ERROR;
    snprintf(session->error_message, sizeof(session->error_message),
             "mbedTLS not installed — pairing unavailable");
    return PAIR_RESULT_NETWORK_ERROR;
}

/*
 * pairing_verify_pin - Verify PIN (stub)
 */
PairingResult pairing_verify_pin(PairingSession *session, const char *pin)
{
    (void)pin;
    if (!session) return PAIR_RESULT_FAILED;
    stub_log("pairing_verify_pin() unavailable\n");
    return PAIR_RESULT_FAILED;
}

void pairing_cancel(PairingSession *session)
{
    if (session) session->state = PAIR_STATE_CANCELLED;
}

void pairing_cleanup(PairingSession *session)
{
    if (session) {
        session->client_cert_hex = NULL;
    }
}

PairingState pairing_get_state(const PairingSession *session)
{
    return session ? session->state : PAIR_STATE_IDLE;
}

PairingResult pairing_get_result(const PairingSession *session)
{
    return session ? session->result : PAIR_RESULT_FAILED;
}

const char *pairing_get_error(const PairingSession *session)
{
    return session ? session->error_message : "No session";
}

int pairing_is_complete(const PairingSession *session)
{
    return (session && session->state == PAIR_STATE_COMPLETE) ? 1 : 0;
}

/*============================================================================
 * Pairing PIN UI functions are implemented in pairing_pin_ui.cpp
 *============================================================================*/
/* (stubs removed — real implementation linked from pairing_pin_ui.o) */
