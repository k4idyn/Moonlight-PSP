/*
 * moonlight_proto.h - Moonlight protocol generation and version constants
 *
 * Targets Sunshine-compatible Moonlight Gen 7 semantics.
 * All pairing, challenge/response hashing, control channel behaviour,
 * and AES/session key derivation follows Gen 7 wire formats.
 *
 * If a future Sunshine version changes the handshake, version-gate
 * behind PROTOCOL_GENERATION rather than scattering conditionals.
 */

#ifndef MOONLIGHT_PROTO_H
#define MOONLIGHT_PROTO_H

/* Moonlight protocol generation — Gen 7 (Sunshine-compatible).
 * Do NOT mix Gen 5 packet formats with Gen 7 pairing/control packets.  */
#define PROTOCOL_GENERATION         7

/* Client version sent in X-GS-ClientVersion header during RTSP. */
#define CLIENT_VERSION              19

/* Device name advertised during pairing and serverinfo. */
#define MOONLIGHT_DEVICE_NAME       "psp"

/* ---- Packet type constants (Gen 7 wire format) ---- */

/* Controller input (18 bytes).
 * Layout: type(2) flags(2) buttons(2) ltrig(1) rtrig(1)
 *         lsx(2) lsy(2) rsx(2) rsy(2) */
#define PKT_TYPE_CONTROLLER_EVENT   0x0005

/* Mouse move (6 bytes).
 * Layout: type(2) deltaX(2) deltaY(2) */
#define PKT_TYPE_MOUSE_MOVE         0x0008

/* Mouse button (4 bytes).
 * Layout: type(2) action(1) button(1) */
#define PKT_TYPE_MOUSE_BUTTON       0x0005

/* ---- SDP / Codec constants ---- */

/* H.264 Baseline Profile Level 3.1 — max 720x480.
 * Sent in the ANNOUNCE SDP: a=fmtp:97 profile-level-id=42001f */
#define H264_PROFILE_LEVEL_ID       "420015"

/* ---- Pairing hash algorithm ---- */

/* Gen 7 uses SHA-256 for salt+PIN key derivation.
 * 0 = SHA-1 (legacy GFE), 1 = SHA-256 (Sunshine / Gen 7). */
#define PAIRING_HASH_SHA256         1

/* Aliases for clearer code */
#define MOONLIGHT_PROTOCOL_GENERATION  PROTOCOL_GENERATION
#define MOONLIGHT_CLIENT_VERSION       CLIENT_VERSION

#endif /* MOONLIGHT_PROTO_H */
