/*
 * client_identity.h - Client certificate and unique ID management
 *
 * On first launch, generates:
 *   1. A 16-char uppercase hex unique ID (persisted in config.ini)
 *   2. A self-signed RSA-2048 client certificate (client.crt + client.key)
 *
 * All pairing, HTTPS, and unpair requests require these files.
 * Provides a "Reset Client Identity" action that deletes everything.
 */

#ifndef CLIENT_IDENTITY_H
#define CLIENT_IDENTITY_H

#ifdef __cplusplus
extern "C" {
#endif

/* File paths for certificate and key on Memory Stick */
#define CLIENT_CERT_PATH    "ms0:/PSP/GAME/Moonlight/client.crt"
#define CLIENT_KEY_PATH     "ms0:/PSP/GAME/Moonlight/client.key"
#define CLIENT_DIR_PATH     "ms0:/PSP/GAME/Moonlight"

/* Maximum length of the unique ID string (16 hex chars + null) */
#define CLIENT_UID_LEN      17
/* Maximum length of the UUID string (36 chars + null) */
#define CLIENT_UUID_LEN     37

/*
 * client_identity_ensure - Ensure client identity files exist.
 *
 * If client.crt, client.key, or client.uid are missing, generate them.
 * Otherwise, load from disk.
 *
 * @out_uid: Buffer of at least CLIENT_UID_LEN bytes to receive the unique ID.
 *
 * Returns 0 on success, -1 on failure (logged, modal shown).
 */
int client_identity_ensure(char *out_uid);

/*
 * client_identity_reset - Delete cert, key, and UID.
 *
 * Forces a clean re-pair on the next host connection.
 * Returns 0 on success, -1 on failure.
 */
int client_identity_reset(void);

/*
 * client_identity_get_cert_hex - Get the PEM certificate as hex string.
 *
 * Returns pointer to a static buffer, or NULL if identity not loaded.
 */
const char *client_identity_get_cert_hex(void);

/*
 * client_identity_get_key_pem - Get the PEM private key string.
 *
 * Returns pointer to a static buffer, or NULL if identity not loaded.
 */
const char *client_identity_get_key_pem(void);

/*
 * client_identity_get_uid - Get the generated unique ID string.
 *
 * Returns pointer to a static buffer, or fallback string.
 */
const char *client_identity_get_uid(void);

/*
 * client_identity_get_uuid - Get the generated UUID string.
 *
 * Returns pointer to a static buffer, or fallback string.
 */
const char *client_identity_get_uuid(void);

#ifdef __cplusplus
}
#endif

#endif /* CLIENT_IDENTITY_H */
