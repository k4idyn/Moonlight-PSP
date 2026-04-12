# PSP Moonlight Pairing Implementation

This document describes the pairing logic implementation for the PSP Moonlight client, which enables secure pairing with Sunshine servers using the Moonlight protocol.

## Overview

The pairing module implements the full 5-step Moonlight/Sunshine pairing protocol using mbedTLS for SSL/TLS and cryptographic operations. It provides a `pairing_verify_pin()` function that validates the 4-digit PIN entered by the user against the server's challenge-response.

## Files

- **pairing.h** — Header file with API declarations and data structures
- **pairing.c** — Implementation of the pairing protocol

## Protocol Steps

The pairing protocol consists of 5 steps:

### Step 1: Send Salt and Client Certificate
- Client generates a random 16-byte salt
- Client loads its certificate (PEM format)
- Client sends HTTP GET to `/pair?phrase=getservercert&salt=<hex>&clientcert=<hex>`
- Server responds with its certificate (plaincert)

### Step 2: Send Client Challenge (PIN Verification)
- Client derives AES key from `SHA256(salt + PIN)`
- Client generates random 16-byte challenge
- Client encrypts challenge with AES-128-ECB
- Client sends HTTP GET to `/pair?clientchallenge=<hex>`
- Server decrypts and responds with `challengeresponse`
- **If PIN is incorrect, server returns paired=0**

### Step 3: Send Server Challenge Response
- Client extracts server challenge from decrypted response
- Client builds response: `server_challenge + cert_signature + client_secret`
- Client hashes response with SHA-256
- Client encrypts hash with AES key
- Client sends HTTP GET to `/pair?phrase=pairsecretreply&serverchallengeresp=<hex>`

### Step 4: Send Client Pairing Secret
- Client generates random 16-byte secret
- Client signs secret with private key (RSA-PKCS1-SHA256)
- Client sends HTTP GET to `/pair?clientpairingsecret=<hex>`
- Server verifies signature

### Step 5: Confirm Pairing (HTTPS)
- Client sends HTTPS GET to `/pair?phrase=pairchallenge`
- Server confirms pairing is complete
- **Note**: This step is optional for Sunshine (Step 4 is sufficient)

## API Reference

### Data Structures

```c
typedef struct {
    char server_address[64];
    unsigned short http_port;
    unsigned short https_port;
    char unique_id[32];
    char device_name[64];
    
    unsigned char salt[16];
    unsigned char aes_key[32];
    unsigned char challenge[16];
    unsigned char server_challenge[16];
    unsigned char client_secret[16];
    unsigned char server_secret[16];
    
    char *client_cert_hex;
    ServerCertificate server_cert;
    
    PairingState state;
    PairingResult result;
    char error_message[256];
    
    u32 start_time;
    u32 timeout_ms;
} PairingSession;
```

### Functions

#### `pairing_init()`
```c
int pairing_init(PairingSession *session, const char *server_ip,
                 unsigned short http_port, unsigned short https_port,
                 const char *unique_id, const char *device_name);
```
Initializes a pairing session with server details.

**Returns**: 0 on success, negative on error

#### `pairing_start()`
```c
int pairing_start(PairingSession *session);
```
Starts the pairing process by sending Step 1 request.

**Returns**: 0 on success, negative on error

#### `pairing_verify_pin()` ⭐
```c
PairingResult pairing_verify_pin(PairingSession *session, const char *pin);
```
**This is the main function that implements PIN verification.**

Performs Steps 2-5 of the pairing protocol:
1. Derives AES key from salt + PIN
2. Sends encrypted challenge
3. Verifies server response
4. Completes pairing

**Returns**:
- `PAIR_RESULT_SUCCESS` - PIN correct, pairing complete
- `PAIR_RESULT_INVALID_PIN` - PIN incorrect
- Other error codes on failure

#### `pairing_cancel()`
```c
void pairing_cancel(PairingSession *session);
```
Cancels an in-progress pairing.

#### `pairing_cleanup()`
```c
void pairing_cleanup(PairingSession *session);
```
Cleans up resources and clears sensitive data.

#### `pairing_get_state()`
```c
PairingState pairing_get_state(const PairingSession *session);
```
Returns current pairing state.

#### `pairing_get_result()`
```c
PairingResult pairing_get_result(const PairingSession *session);
```
Returns pairing result code.

#### `pairing_get_error()`
```c
const char *pairing_get_error(const PairingSession *session);
```
Returns error message string.

#### `pairing_is_complete()`
```c
int pairing_is_complete(const PairingSession *session);
```
Returns 1 if pairing completed successfully.

## Usage Example

```c
#include "pairing.h"

int main() {
    PairingSession session;
    PairingResult result;
    char pin[5] = "1234"; /* User-entered PIN */
    
    /* Initialize session */
    pairing_init(&session, "192.168.1.100", 47989, 47984,
                 "0123456789ABCDEF", "psp");
    
    /* Start pairing (Step 1) */
    if (pairing_start(&session) != 0) {
        printf("Failed to start pairing: %s\n", 
               pairing_get_error(&session));
        return -1;
    }
    
    /* Verify PIN (Steps 2-5) */
    result = pairing_verify_pin(&session, pin);
    
    if (result == PAIR_RESULT_SUCCESS) {
        printf("Pairing successful!\n");
    } else if (result == PAIR_RESULT_INVALID_PIN) {
        printf("Incorrect PIN\n");
    } else {
        printf("Pairing failed: %s\n", 
               pairing_get_error(&session));
    }
    
    /* Cleanup */
    pairing_cleanup(&session);
    return 0;
}
```

## Integration with Existing Code

The pairing module integrates with the existing PSP Moonlight codebase:

### With pairing_pin_ui.c
The `pairing_pin_ui.c` component displays the PIN on screen. The pairing module uses this PIN to verify with the server.

### With network_connect.c
The existing `network_connect.c` can be updated to use the new pairing module instead of the basic `sunshine_pair()` function.

## Certificate Requirements

The pairing module expects client certificates in PEM format:

- **client.pem** - Client certificate (PEM)
- **client.key** - Client private key (PEM)

These should be stored in `ms0:/PSP/GAME/Moonlight/`

## Security Features

1. **AES-128-ECB Encryption**: Challenge-response encryption
2. **SHA-256 Hashing**: Key derivation and signature verification
3. **RSA-PKCS1-SHA256**: Client authentication via certificate signing
4. **Random Salt Generation**: Uses PSP timer for entropy
5. **Sensitive Data Cleanup**: All keys and secrets cleared on cleanup

## Error Handling

The module provides detailed error messages via `pairing_get_error()`:

- Network errors (connection failed, timeout)
- Certificate errors (cannot load cert/key)
- Server errors (rejection, invalid response)
- PIN errors (incorrect PIN, invalid format)

## Timeout

Default timeout is 120 seconds (2 minutes). Can be adjusted via `session.timeout_ms`.

## Debug Logging

Enable debug output by defining `PAIR_LOG` macros. All pairing steps are logged with `[PAIR]` prefix.

## Dependencies

- **mbedTLS**: SSL/TLS and cryptographic operations
- **PSPSDK**: PSP system functions
- **Standard C library**: string.h, stdio.h, stdlib.h

## Build Integration

Add to Makefile:
```makefile
OBJS = ... pairing.o
LIBS = ... -lmbedtls -lmbedcrypto -lmbedx509
```

## Testing

Pairing is tested on real hardware. To verify a pairing flow:

1. Start Sunshine on the host PC
2. Launch PSP Moonlight and navigate to an unpaired host
3. The 4-digit PIN is displayed on the PSP
4. Enter it in Sunshine's web UI under Add Device
5. The PSP should transition to the Game Library on success

All pairing steps and errors are logged to `ms0:/moonlight_debug.log`.

## Troubleshooting

### "Cannot load client certificate"
- Ensure `client.pem` exists in `ms0:/PSP/GAME/Moonlight/`
- Check file permissions

### "Network error in Step 1"
- Verify Sunshine server is running
- Check server IP address and port
- Ensure PSP is connected to same network

### "PIN verification failed"
- Verify PIN matches what's shown on Sunshine web UI
- Check that PIN is exactly 4 digits
- Ensure server is in pairing mode

### "Server signature verification failed"
- Certificate mismatch between client and server
- Re-pair from scratch

## Known Limitations

- Step 5 (HTTPS confirm) is optional for Sunshine and is skipped after a successful Step 4.
- The client certificate and unique ID are stored in `ms0:/PSP/GAME/Moonlight/`. If these files are missing, all pairing and HTTPS requests will fail with an explicit error modal.
- TOFU (Trust-On-First-Use) certificate fingerprinting is stored per-host in `config.ini`. A fingerprint change on reconnect will block the connection and prompt re-pairing.
