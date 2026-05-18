# PSP Moonlight Pairing Implementation

This document describes the pairing logic used by PSP Moonlight to pair with Sunshine servers.

## Overview

PSP Moonlight uses the Moonlight/Sunshine challenge-response pairing protocol with mbedTLS for certificate, RSA, AES, and HTTPS work. The active implementation lives in `src/network_connect.c` and is driven from the pairing PIN UI.

v1.2 completes the authenticated HTTPS confirmation step after the signed pairing secret. This matches the behavior expected by current Sunshine builds and avoids leaving the host in a half-paired state.

## Files

- `src/network_connect.c` - active pairing, HTTPS, RTSP, launch, and cleanup flow
- `src/pairing_pin_ui.cpp` - PIN display and user-facing pairing screen
- `include/client_identity.h` / `src/client_identity.c` - client certificate, key, UUID, and identity storage
- `include/net_send.h` / `src/net_send.c` - socket send helpers used by HTTP/HTTPS paths

## Protocol Steps

### Step 1: Send Salt and Client Certificate

- Client generates a random 16-byte salt.
- Client loads or creates its certificate and private key.
- Client sends HTTP GET to `/pair?phrase=getservercert&salt=<hex>&clientcert=<hex>`.
- Server responds with its certificate.

### Step 2: Send Client Challenge

- Client derives an AES key from `SHA256(salt + PIN)`.
- Client generates a random challenge.
- Client encrypts the challenge with AES-128-ECB.
- Client sends HTTP GET to `/pair?clientchallenge=<hex>`.
- Server response verifies whether the PIN was accepted.

### Step 3: Send Server Challenge Response

- Client decrypts the server challenge.
- Client builds the server challenge response.
- Client encrypts the response hash with the pairing AES key.
- Client sends HTTP GET to `/pair?phrase=pairsecretreply&serverchallengeresp=<hex>`.

### Step 4: Send Client Pairing Secret

- Client generates a random secret.
- Client signs it with the client private key.
- Client sends HTTP GET to `/pair?clientpairingsecret=<hex>`.
- Server verifies the signature and registers the client certificate.

### Step 5: Confirm Pairing Over HTTPS

- Client sends authenticated HTTPS GET to `/pair?phrase=pairchallenge` using the newly registered client certificate.
- Server confirms that pairing is complete.
- The PSP marks the host as paired only after this confirmation succeeds.

## Failure Cleanup

If pairing fails or the user cancels before completion, PSP Moonlight sends a best-effort `/unpair` request for the current client identity. This prevents stale partial device entries from causing later PIN attempts to fail.

If a later paired-host request fails because the host-side device entry is stale, the host is removed from the local paired-host list and the user can pair again.

## Certificate Storage

Client identity files are stored in the Moonlight folder on the Memory Stick:

- `client.crt` - client certificate
- `client.key` - client private key
- `client.uid` - stable Moonlight unique ID used to derive the UUID
- `tls_pins/` - first-use HTTPS certificate pins per host

Deleting these files forces the PSP to create a new identity and pair again.

## User Flow

1. Select an unpaired host.
2. The PSP displays a 4-digit PIN.
3. Enter the PIN in Sunshine's Add Device / PIN page.
4. The PSP completes all pairing protocol steps.
5. The game library loads when pairing succeeds.

## Troubleshooting

### PIN is rejected

- Re-enter the current PIN exactly as shown.
- Remove stale PSP device entries from Sunshine and try again.
- Verify the PSP and host are on the same reachable network.

### Host appears paired but app list fails

- Unpair the host from PSP Moonlight or Sunshine.
- Pair again so both sides agree on the current client certificate.

### HTTPS or certificate errors

- Verify Sunshine port `47984` is reachable.
- Delete the PSP Moonlight certificate files only if you intend to pair from scratch.

## Security Notes

- The pairing flow uses a PIN-derived AES challenge response.
- The client signs its pairing secret with its private key.
- The final confirmation is authenticated HTTPS using the newly registered client certificate.
- Sensitive pairing data is cleared from temporary buffers after use where practical.
