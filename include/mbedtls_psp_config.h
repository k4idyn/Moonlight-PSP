/*
 * mbedtls_psp_config.h - Minimal mbedTLS configuration for PSP Moonlight
 *
 * Enables only what is needed for RSA-2048 PKCS#1 v1.5 SHA-256 signing
 * (Moonlight pairing Step 4) and private key parsing from PEM.
 * All SSL/TLS, certificates, and ECC are excluded to minimise code size.
 */

#ifndef MBEDTLS_PSP_CONFIG_H
#define MBEDTLS_PSP_CONFIG_H

/* =========================================================================
 * Platform
 * ========================================================================= */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY         /* allow custom malloc/free */
#define MBEDTLS_MEMORY_BUFFER_ALLOC_C   /* fixed PSP TLS/keygen heap */

/* PSP has no /dev/urandom or CryptGenRandom */
#define MBEDTLS_NO_PLATFORM_ENTROPY

/* We supply mbedtls_hardware_poll() in psp_mbedtls_entropy.c */
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* =========================================================================
 * Core big-number arithmetic (required by RSA)
 * ========================================================================= */
#define MBEDTLS_BIGNUM_C

/* =========================================================================
 * Hash / digest
 * ========================================================================= */
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C

/* =========================================================================
 * Symmetric cipher  (AES required by CTR-DRBG; cipher layer for pk parse)
 * ========================================================================= */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CIPHER_MODE_CBC

/* =========================================================================
 * RSA
 * ========================================================================= */
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15               /* PKCS#1 v1.5 sign/verify */
#define MBEDTLS_GENPRIME                /* required for mbedtls_rsa_gen_key */

/* =========================================================================
 * ASN.1 / OID / PEM / Base64  (required for parsing PEM private keys)
 * ========================================================================= */
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_BASE64_C

/* =========================================================================
 * Public-key layer
 * ========================================================================= */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C

/* =========================================================================
 * Entropy / DRBG
 * ========================================================================= */
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C

/* =========================================================================
 * Error strings  (helpful for debug logging)
 * ========================================================================= */
#define MBEDTLS_ERROR_C

/* =========================================================================
 * SSL/TLS  (required for mutual-TLS /launch HTTPS request)
 * ========================================================================= */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C               /* TLS client only */
#define MBEDTLS_SSL_PROTO_TLS1_2        /* TLS 1.2 (Sunshine default) */
#define MBEDTLS_CIPHER_MODE_CBC         /* already defined above, but ensure */
#define MBEDTLS_GCM_C                   /* GCM ciphersuite support */

/* =========================================================================
 * X.509 certificate parsing  (required for client cert authentication)
 * ========================================================================= */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C

/* Allow direct struct field access (mbedTLS 3.x uses MBEDTLS_PRIVATE) */
#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* =========================================================================
 * Key Exchanges and Elliptic Curve (Required for connecting to modern TLS)
 * Modern Sunshine servers reject plain RSA key exchange and require ECDHE.
 * ========================================================================= */
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED

/* =========================================================================
 * Explicitly disabled
 * ========================================================================= */
/* #define MBEDTLS_NET_C       */
/* #define MBEDTLS_ECDSA_C     */

/* =========================================================================
 * X.509 certificate generation (required for client_identity.c)
 * Enables self-signed RSA-2048 cert creation on first launch.
 * ========================================================================= */
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_PEM_WRITE_C

#endif /* MBEDTLS_PSP_CONFIG_H */
