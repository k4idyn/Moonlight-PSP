/*
 * pairing.h - Moonlight/Sunshine pairing protocol implementation for PSP
 *
 * Implements the full 5-step pairing protocol using mbedTLS:
 * Step 1: Send salt + client certificate to get server certificate
 * Step 2: Send client challenge (encrypted with PIN-derived key)
 * Step 3: Send server challenge response
 * Step 4: Send client pairing secret (signed)
 * Step 5: Confirm pairing via HTTPS
 */

#ifndef PAIRING_H
#define PAIRING_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/*--------------------------------------------------------------------------
 * Configuration
 *--------------------------------------------------------------------------*/
#define PAIRING_SALT_SIZE       16
#define PAIRING_CHALLENGE_SIZE  16
#define PAIRING_SECRET_SIZE     16
#define PAIRING_PIN_DIGITS      4

/*--------------------------------------------------------------------------
 * Pairing States
 *--------------------------------------------------------------------------*/
typedef enum {
    PAIR_STATE_IDLE,
    PAIR_STATE_IN_PROGRESS,
    PAIR_STATE_WAITING_FOR_PIN,
    PAIR_STATE_CHALLENGE_SENT,
    PAIR_STATE_CHALLENGE_RECEIVED,
    PAIR_STATE_SECRET_SENT,
    PAIR_STATE_CONFIRMED,
    PAIR_STATE_COMPLETE,
    PAIR_STATE_FAILED,
    PAIR_STATE_CANCELLED
} PairingState;

/*--------------------------------------------------------------------------
 * Pairing Result Codes
 *--------------------------------------------------------------------------*/
typedef enum {
    PAIR_RESULT_SUCCESS = 0,
    PAIR_RESULT_FAILED = -1,
    PAIR_RESULT_INVALID_PIN = -2,
    PAIR_RESULT_SERVER_ERROR = -3,
    PAIR_RESULT_NETWORK_ERROR = -4,
    PAIR_RESULT_CERTIFICATE_ERROR = -5,
    PAIR_RESULT_TIMEOUT = -6,
    PAIR_RESULT_CANCELLED = -7
} PairingResult;

/*--------------------------------------------------------------------------
 * Server Certificate Structure
 *--------------------------------------------------------------------------*/
typedef struct {
    unsigned char *data;
    size_t length;
    char *subject;
    char *issuer;
} ServerCertificate;

/*--------------------------------------------------------------------------
 * Pairing Session Structure
 *--------------------------------------------------------------------------*/
typedef struct {
    char server_address[64];
    unsigned short http_port;
    unsigned short https_port;
    char unique_id[32];
    char device_name[64];
    
    /* Cryptographic state */
    unsigned char salt[PAIRING_SALT_SIZE];
    unsigned char aes_key[32];
    unsigned char challenge[PAIRING_CHALLENGE_SIZE];
    unsigned char server_challenge[PAIRING_CHALLENGE_SIZE];
    unsigned char client_secret[PAIRING_SECRET_SIZE];
    unsigned char server_secret[PAIRING_SECRET_SIZE];
    
    /* Certificates */
    char *client_cert_hex;
    ServerCertificate server_cert;
    
    /* State tracking */
    PairingState state;
    PairingResult result;
    char error_message[256];
    
    /* Timing */
    u32 start_time;
    u32 timeout_ms;
} PairingSession;

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

/**
 * pairing_init - Initialize a pairing session
 *
 * @session:     Pointer to PairingSession structure
 * @server_ip:   IP address of the Sunshine server
 * @http_port:   HTTP port (default 47989)
 * @https_port:  HTTPS port (default 47984)
 * @unique_id:   Unique device identifier
 * @device_name: Human-readable device name
 *
 * Returns: 0 on success, negative on error
 */
int pairing_init(PairingSession *session, const char *server_ip,
                 unsigned short http_port, unsigned short https_port,
                 const char *unique_id, const char *device_name);

/**
 * pairing_start - Begin the pairing process
 *
 * Sends Step 1 request with salt and client certificate.
 * The server will wait for PIN entry before responding.
 *
 * @session: Pointer to initialized PairingSession
 *
 * Returns: 0 on success, negative on error
 */
int pairing_start(PairingSession *session);

/**
 * pairing_verify_pin - Verify the PIN and continue pairing
 *
 * This function implements the verifyPairing() requirement:
 * - Derives AES key from salt + PIN
 * - Decrypts server challenge response
 * - Verifies the challenge matches
 * - Continues with Steps 3-5
 *
 * @session: Pointer to PairingSession in PAIR_STATE_WAITING_FOR_PIN
 * @pin:     4-digit PIN entered by the user
 *
 * Returns: PAIR_RESULT_SUCCESS if PIN is correct and pairing completes
 *          PAIR_RESULT_INVALID_PIN if PIN doesn't match
 *          Other PairingResult codes on failure
 */
PairingResult pairing_verify_pin(PairingSession *session, const char *pin);

/**
 * pairing_cancel - Cancel an in-progress pairing
 *
 * @session: Pointer to PairingSession
 */
void pairing_cancel(PairingSession *session);

/**
 * pairing_cleanup - Clean up pairing session resources
 *
 * @session: Pointer to PairingSession
 */
void pairing_cleanup(PairingSession *session);

/**
 * pairing_get_state - Get current pairing state
 *
 * @session: Pointer to PairingSession
 *
 * Returns: Current PairingState
 */
PairingState pairing_get_state(const PairingSession *session);

/**
 * pairing_get_result - Get pairing result
 *
 * @session: Pointer to PairingSession
 *
 * Returns: PairingResult code
 */
PairingResult pairing_get_result(const PairingSession *session);

/**
 * pairing_get_error - Get error message
 *
 * @session: Pointer to PairingSession
 *
 * Returns: Pointer to error message string (may be empty)
 */
const char *pairing_get_error(const PairingSession *session);

/**
 * pairing_is_complete - Check if pairing completed successfully
 *
 * @session: Pointer to PairingSession
 *
 * Returns: 1 if complete, 0 otherwise
 */
int pairing_is_complete(const PairingSession *session);

#ifdef __cplusplus
}
#endif

#endif /* PAIRING_H */