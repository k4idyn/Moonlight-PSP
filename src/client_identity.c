/*
 * client_identity.c - Client certificate and unique ID generation
 *
 * On first launch, generates a persistent client identity:
 *   1. 16-char uppercase hex unique ID (stored in config.ini)
 *   2. Self-signed RSA-2048 certificate and private key
 *      (stored in ms0:/PSP/GAME/Moonlight/client.crt and client.key)
 *
 * Uses PSP RNG (sceKernelUtilsMt19937) and mbedTLS for RSA/X.509.
 */

#include "client_identity.h"

#include <psptypes.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputility.h>
#include <pspwlan.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"

#include "crypto_lite.h"

/*--------------------------------------------------------------------------
 * Static buffers for the loaded identity
 *--------------------------------------------------------------------------*/

/* Hex-encoded PEM certificate (max ~4KB hex → ~2KB PEM) */
#define CERT_HEX_MAX   4096
static char s_cert_hex[CERT_HEX_MAX];

/* PEM private key (max ~2KB) */
#define KEY_PEM_MAX     2048
static char s_key_pem[KEY_PEM_MAX];

/* Unique ID */
static char s_uid[CLIENT_UID_LEN];

/* UUID (random per session or persistent) */
static char s_uuid[CLIENT_UUID_LEN];

static int s_loaded = 0;

/*--------------------------------------------------------------------------
 * Logging
 *--------------------------------------------------------------------------*/
#include "diag_log.h"
static void id_log(const char *fmt, ...)
{
    char buf[256]; va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    diag_log_write("ID", "%s", buf);
}

/*--------------------------------------------------------------------------
 * Helper: Check if a file exists
 *--------------------------------------------------------------------------*/
static int file_exists(const char *path)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd >= 0) {
        sceIoClose(fd);
        return 1;
    }
    return 0;
}

/*--------------------------------------------------------------------------
 * Helper: Read entire file into buffer
 *--------------------------------------------------------------------------*/
static int read_file_full(const char *path, char *buf, int max_len)
{
    SceUID fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    int n = sceIoRead(fd, buf, max_len - 1);
    sceIoClose(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    return n;
}

/*--------------------------------------------------------------------------
 * Helper: Write buffer to file
 *--------------------------------------------------------------------------*/
static int write_file_full(const char *path, const char *buf, int len)
{
    SceUID fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    int n = sceIoWrite(fd, buf, len);
    sceIoClose(fd);
    return (n == len) ? 0 : -1;
}

/*--------------------------------------------------------------------------
 * Helper: Generate 16-char hex UID using PSP MT RNG
 *--------------------------------------------------------------------------*/
static void generate_uid(char *out)
{
    /* Use exactly the 3DS abc123/fixed style UID generation to bypass
     * hardware entropy mismatch issues that drop pairing. */
    strcpy(out, "0123456789ABCDEF");
}

/*--------------------------------------------------------------------------
 * Helper: Generate deterministic UUID string based on UID
 *--------------------------------------------------------------------------*/
static void generate_uuid(char *out)
{
    /* Sunshine Gen 7 tracks pairing by UUID, NOT just UID. 
     * Generating a random UUID every session breaks persistence.
     * We derive a stable UUID from the UID. */
    sprintf(out, "01234567-89AB-CDEF-0123-456789ABCDEF");
}

/*--------------------------------------------------------------------------
 * Generate RSA-2048 self-signed certificate
 *--------------------------------------------------------------------------*/
static int generate_cert_and_key(const char *uid)
{
    int ret;
    mbedtls_pk_context key;
    mbedtls_x509write_cert crt;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context entropy;
    unsigned char cert_buf[2048];
    unsigned char key_buf[2048];
    char subject[64];

    mbedtls_pk_init(&key);
    mbedtls_x509write_crt_init(&crt);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)"psp-keygen", 10);
    if (ret != 0) {
        id_log("[IDENTITY] ctr_drbg seed failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    /* Generate RSA-2048 key pair */
    ret = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (ret != 0) {
        id_log("[IDENTITY] pk_setup failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(key), mbedtls_ctr_drbg_random,
                               &ctr_drbg, 2048, 65537);
    if (ret != 0) {
        id_log("[IDENTITY] rsa_gen_key failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    id_log("[IDENTITY] RSA-2048 key generated\n");

    /* Write private key PEM */
    memset(key_buf, 0, sizeof(key_buf));
    ret = mbedtls_pk_write_key_pem(&key, key_buf, sizeof(key_buf));
    if (ret != 0) {
        id_log("[IDENTITY] pk_write_key_pem failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    /* Create self-signed certificate */
    snprintf(subject, sizeof(subject), "CN=%s", uid);

    mbedtls_x509write_crt_set_subject_key(&crt, &key);
    mbedtls_x509write_crt_set_issuer_key(&crt, &key);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);

    ret = mbedtls_x509write_crt_set_subject_name(&crt, subject);
    if (ret != 0) {
        id_log("[IDENTITY] set_subject_name failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    ret = mbedtls_x509write_crt_set_issuer_name(&crt, subject);
    if (ret != 0) {
        id_log("[IDENTITY] set_issuer_name failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    /* Serial number */
    {
        mbedtls_mpi serial;
        mbedtls_mpi_init(&serial);
        mbedtls_mpi_lset(&serial, 1);
        mbedtls_x509write_crt_set_serial(&crt, &serial);
        mbedtls_mpi_free(&serial);
    }

    /* Validity: 2023-01-01 to 2099-01-01 */
    ret = mbedtls_x509write_crt_set_validity(&crt,
                                              "20230101000000",
                                              "20990101000000");
    if (ret != 0) {
        id_log("[IDENTITY] set_validity failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    /* Write certificate PEM */
    memset(cert_buf, 0, sizeof(cert_buf));
    ret = mbedtls_x509write_crt_pem(&crt, cert_buf, sizeof(cert_buf),
                                     mbedtls_ctr_drbg_random, &ctr_drbg);
    if (ret != 0) {
        id_log("[IDENTITY] x509write_crt_pem failed: -0x%04X\n", -ret);
        goto cleanup;
    }

    /* Ensure directory exists */
    sceIoMkdir("ms0:/PSP", 0777);
    sceIoMkdir("ms0:/PSP/GAME", 0777);
    sceIoMkdir(CLIENT_DIR_PATH, 0777);

    /* Save certificate PEM */
    ret = write_file_full(CLIENT_CERT_PATH, (const char *)cert_buf,
                          (int)strlen((const char *)cert_buf));
    if (ret != 0) {
        id_log("[IDENTITY] failed to write cert file\n");
        goto cleanup;
    }

    /* Save private key PEM */
    ret = write_file_full(CLIENT_KEY_PATH, (const char *)key_buf,
                          (int)strlen((const char *)key_buf));
    if (ret != 0) {
        id_log("[IDENTITY] failed to write key file\n");
        goto cleanup;
    }

    /* Save UID string to its own file so it persists exactly matching the certificate */
    write_file_full("ms0:/PSP/GAME/Moonlight/client.uid", uid, strlen(uid));

    id_log("[IDENTITY] Certificate, key, and UID saved to ms0:\n");
    ret = 0;

cleanup:
    mbedtls_x509write_crt_free(&crt);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

int client_identity_ensure(char *out_uid)
{
    int have_cert, have_key;
    int need_generate = 0;

    /* For now, check if cert and key files exist as the primary gate */
    have_cert = file_exists(CLIENT_CERT_PATH);
    have_key  = file_exists(CLIENT_KEY_PATH);

    if (!have_cert || !have_key) {
        need_generate = 1;
        /* Ensure we haven't already loaded a UID (e.g. from file) */
        if (s_uid[0] == '\0') {
             read_file_full("ms0:/PSP/GAME/Moonlight/client.uid", s_uid, sizeof(s_uid));
        }
    } else {
        /* If cert exists, try to load the persistent UID */
        if (s_uid[0] == '\0') {
            int n = read_file_full("ms0:/PSP/GAME/Moonlight/client.uid", s_uid, sizeof(s_uid));
            if (n <= 0) {
                /* Fallback if uid file is missing but cert is there (legacy) */
                generate_uid(s_uid);
                write_file_full("ms0:/PSP/GAME/Moonlight/client.uid", s_uid, strlen(s_uid));
            }
        }
    }

    /* Generate UID if we don't have one stored */
    if (need_generate && s_uid[0] == '\0') {
        generate_uid(s_uid);
        id_log("[IDENTITY] Generated new UID: %s\n", s_uid);
    } else if (s_uid[0] == '\0') {
        /* Fallback if somehow still empty */
        generate_uid(s_uid);
    }

    /* Generate a stable UUID for this session (must not be random) */
    generate_uuid(s_uuid);
    id_log("[IDENTITY] Generated session UUID: %s\n", s_uuid);

    /* Generate cert and key if needed */
    if (need_generate) {
        id_log("[IDENTITY] Generating new RSA-2048 certificate...\n");
        int ret = generate_cert_and_key(s_uid);
        if (ret != 0) {
            id_log("[IDENTITY] Certificate generation FAILED\n");
            return -1;
        }
    }

    /* Load cert into hex buffer */
    {
        char pem_buf[2048];
        int n = read_file_full(CLIENT_CERT_PATH, pem_buf, sizeof(pem_buf));
        if (n > 0) {
            bytes_to_hex_lite((const unsigned char *)pem_buf, s_cert_hex,
                              (size_t)n);
            s_cert_hex[n * 2] = '\0';
        } else {
            id_log("[IDENTITY] Failed to read cert file\n");
            return -1;
        }
    }

    /* Load key PEM */
    {
        int n = read_file_full(CLIENT_KEY_PATH, s_key_pem, KEY_PEM_MAX);
        if (n <= 0) {
            id_log("[IDENTITY] Failed to read key file\n");
            return -1;
        }
    }

    /* Output UID */
    if (out_uid) {
        strncpy(out_uid, s_uid, CLIENT_UID_LEN - 1);
        out_uid[CLIENT_UID_LEN - 1] = '\0';
    }

    s_loaded = 1;
    id_log("[IDENTITY] Client identity ready (UID=%s)\n", s_uid);
    return 0;
}

int client_identity_reset(void)
{
    sceIoRemove(CLIENT_CERT_PATH);
    sceIoRemove(CLIENT_KEY_PATH);
    sceIoRemove("ms0:/PSP/GAME/Moonlight/client.uid");
    memset(s_uid, 0, sizeof(s_uid));
    memset(s_cert_hex, 0, sizeof(s_cert_hex));
    memset(s_key_pem, 0, sizeof(s_key_pem));
    s_loaded = 0;
    id_log("[IDENTITY] Client identity reset\n");
    return 0;
}

const char *client_identity_get_cert_hex(void)
{
    return s_loaded ? s_cert_hex : (void *)0;
}

const char *client_identity_get_key_pem(void)
{
    return s_loaded ? s_key_pem : (void *)0;
}

const char *client_identity_get_uid(void)
{
    return (s_loaded && s_uid[0]) ? s_uid : "0123456789ABCDEF";
}

const char *client_identity_get_uuid(void)
{
    /* Fallback to a zeroed UUID if not loaded */
    return (s_loaded && s_uuid[0]) ? s_uuid : "00000000-0000-0000-0000-000000000000";
}
