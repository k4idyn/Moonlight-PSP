/*
 * client_identity.c - Client certificate and unique ID generation
 *
 * On first launch, generates a persistent client identity:
 *   1. 16-char uppercase hex unique ID (stored in config.ini)
 *   2. Self-signed RSA-2048 certificate and private key
 *      (stored in ms0:/PSP/SAVEDATA/Moonlight/client.crt and client.key)
 *
 * Uses PSP RNG (sceKernelUtilsMt19937) and mbedTLS for RSA/X.509.
 */

#include "client_identity.h"

#include <psptypes.h>
#include <pspkernel.h>
#include <pspiofilemgr.h>
#include <psputility.h>
#include <pspwlan.h>
#include <psprtc.h>
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

/* Persisted UID path */
#define CLIENT_UID_PATH MOONLIGHT_SAVE_DIR "/client.uid"

/* Legacy UID used by older builds that generated fixed identity values. */
#define LEGACY_UID_VALUE "0123456789ABCDEF"

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

static int is_hex_char(char c)
{
    return ((c >= '0' && c <= '9') ||
            (c >= 'a' && c <= 'f') ||
            (c >= 'A' && c <= 'F'));
}

static int is_valid_uid_16_hex(const char *uid)
{
    int i;
    if (!uid) return 0;
    for (i = 0; i < 16; i++) {
        if (!is_hex_char(uid[i])) return 0;
    }
    return uid[16] == '\0';
}

static void uid_to_uppercase(char *uid)
{
    int i;
    for (i = 0; i < 16; i++) {
        if (uid[i] >= 'a' && uid[i] <= 'f') {
            uid[i] = (char)(uid[i] - 'a' + 'A');
        }
    }
}

static int load_uid_from_file(char *out_uid)
{
    int n;
    char tmp[32];
    if (!out_uid) return -1;
    n = read_file_full(CLIENT_UID_PATH, tmp, sizeof(tmp));
    if (n <= 0) return -1;

    /* Remove trailing newline if present (manual edits). */
    {
        char *nl = strchr(tmp, '\n');
        if (nl) *nl = '\0';
    }

    if (!is_valid_uid_16_hex(tmp)) {
        return -1;
    }

    strncpy(out_uid, tmp, CLIENT_UID_LEN - 1);
    out_uid[CLIENT_UID_LEN - 1] = '\0';
    uid_to_uppercase(out_uid);
    return 0;
}

static void bytes_to_upper_hex_16(const unsigned char *in8, char *out16)
{
    static const char hex_upper[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 8; i++) {
        out16[i * 2]     = hex_upper[(in8[i] >> 4) & 0x0F];
        out16[i * 2 + 1] = hex_upper[in8[i] & 0x0F];
    }
    out16[16] = '\0';
}

/*--------------------------------------------------------------------------
 * Helper: Generate 16-char hex UID using PSP MT RNG
 *--------------------------------------------------------------------------*/
static void generate_uid(char *out)
{
    unsigned char digest[32];
    unsigned char seed[64];
    unsigned char mac[6];
    u64 rtc_tick = 0;
    u64 sys_wide = 0;
    u32 sys_low = 0;
    int pos = 0;

    memset(seed, 0, sizeof(seed));
    memset(mac, 0, sizeof(mac));

    /* Mix stable device info + runtime jitter into a one-time UID seed. */
    if (sceWlanGetEtherAddr(mac) == 0) {
        memcpy(seed + pos, mac, sizeof(mac));
        pos += (int)sizeof(mac);
    }

    sceRtcGetCurrentTick(&rtc_tick);
    sys_wide = sceKernelGetSystemTimeWide();
    sys_low  = sceKernelGetSystemTimeLow();

    memcpy(seed + pos, &rtc_tick, sizeof(rtc_tick));
    pos += (int)sizeof(rtc_tick);
    memcpy(seed + pos, &sys_wide, sizeof(sys_wide));
    pos += (int)sizeof(sys_wide);
    memcpy(seed + pos, &sys_low, sizeof(sys_low));
    pos += (int)sizeof(sys_low);

    sha256_hash(seed, (size_t)pos, digest);
    bytes_to_upper_hex_16(digest, out);
}

/*--------------------------------------------------------------------------
 * Helper: Generate deterministic UUID string based on UID
 *--------------------------------------------------------------------------*/
static void generate_uuid(const char *uid, char *out)
{
    unsigned char digest[32];
    unsigned char uuid[16];
    const char *stable_uid = uid ? uid : LEGACY_UID_VALUE;
    size_t uid_len = strlen(stable_uid);

    sha256_hash((const unsigned char *)stable_uid, uid_len, digest);
    memcpy(uuid, digest, sizeof(uuid));

    /* RFC 4122 variant + version 5 shape from deterministic hash bytes. */
    uuid[6] = (unsigned char)((uuid[6] & 0x0F) | 0x50);
    uuid[8] = (unsigned char)((uuid[8] & 0x3F) | 0x80);

    snprintf(out, CLIENT_UUID_LEN,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
             uuid[0], uuid[1], uuid[2], uuid[3],
             uuid[4], uuid[5],
             uuid[6], uuid[7],
             uuid[8], uuid[9],
             uuid[10], uuid[11], uuid[12], uuid[13], uuid[14], uuid[15]);
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
    moonlight_storage_ensure_data_dir();

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
    write_file_full(CLIENT_UID_PATH, uid, strlen(uid));

    id_log("[IDENTITY] Certificate, key, and UID saved to savedata\n");
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
             load_uid_from_file(s_uid);
        }
    } else {
        /* If cert exists, try to load the persistent UID */
        if (s_uid[0] == '\0') {
            if (load_uid_from_file(s_uid) != 0) {
                /* Legacy migration path: preserve old fixed UID behavior
                 * when cert/key already exist but UID file is missing. */
                strcpy(s_uid, LEGACY_UID_VALUE);
                write_file_full(CLIENT_UID_PATH, s_uid, strlen(s_uid));
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
    generate_uuid(s_uid, s_uuid);
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
    sceIoRemove(CLIENT_UID_PATH);
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
    if (s_uid[0] == '\0') {
        if (load_uid_from_file(s_uid) != 0) {
            generate_uid(s_uid);
        }
    }
    return s_uid;
}

const char *client_identity_get_uuid(void)
{
    if (s_uuid[0] == '\0') {
        generate_uuid(client_identity_get_uid(), s_uuid);
    }
    return s_uuid;
}
