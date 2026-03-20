#include "mkcert.h"
#include <pspkernel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../modules/logger.h"

#include <mbedtls/x509_crt.h>
#include <mbedtls/x509_csr.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/md.h>
#include <mbedtls/rsa.h>
#include <mbedtls/bignum.h>
#include <mbedtls/error.h>
#include <mbedtls/pk.h>
#include <mbedtls/oid.h>

static int better_entropy_mkcert(void *data, unsigned char *output, size_t len) {
    (void)data;
    unsigned long long time;
    size_t i;
    for(i=0; i<len; i++) {
        time = sceKernelGetSystemTimeWide();
        output[i] = (unsigned char)((time >> (i % 8)) ^ (rand() % 256));
    }
    return 0;
}

static const int NUM_BITS = 2048;

CERT_KEY_PAIR mkcert_generate(const char* device_name) {
    (void)device_name;
    mbedtls_pk_context *pkey = malloc(sizeof(mbedtls_pk_context));
    mbedtls_x509write_cert *crt = malloc(sizeof(mbedtls_x509write_cert));
    mbedtls_entropy_context *entropy = malloc(sizeof(mbedtls_entropy_context));
    mbedtls_ctr_drbg_context *ctr_drbg = malloc(sizeof(mbedtls_ctr_drbg_context));
    mbedtls_mpi serial;

    mbedtls_pk_init(pkey);
    mbedtls_x509write_crt_init(crt);
    mbedtls_ctr_drbg_init(ctr_drbg);
    mbedtls_entropy_init(entropy);
    mbedtls_mpi_init(&serial);

    FILE *flog = fopen("moonlight_debug.log", "a");
    if (flog) { fprintf(flog, "Seeding DRBG...\n"); fflush(flog); }

    const char *pers = "cert_gen";
    int ret = mbedtls_ctr_drbg_seed(ctr_drbg, better_entropy_mkcert, entropy, (const unsigned char *)pers, strlen(pers));
    if (flog) { fprintf(flog, "DRBG seed result: %d\n", ret); fflush(flog); }

    mbedtls_pk_setup(pkey, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    
    if (flog) { fprintf(flog, "Generating 2048-bit RSA key...\n"); fflush(flog); }
    ret = mbedtls_rsa_gen_key(mbedtls_pk_rsa(*pkey), mbedtls_ctr_drbg_random, ctr_drbg, 2048, 65537);
    if (flog) { fprintf(flog, "RSA gen result: %d\n", ret); fclose(flog); }

    mbedtls_x509write_crt_set_subject_key(crt, pkey);
    mbedtls_x509write_crt_set_issuer_key(crt, pkey);

    char dn_buf[128];
    const char* standard_cn = "NVIDIA GameStream Client";
    snprintf(dn_buf, sizeof(dn_buf), "CN=%s", standard_cn);
    LOG_INFO(COMPONENT_NETWORK, "Generating certificate with %s", dn_buf);
    mbedtls_x509write_crt_set_subject_name(crt, dn_buf);
    mbedtls_x509write_crt_set_issuer_name(crt, dn_buf);

    mbedtls_x509write_crt_set_version(crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(crt, MBEDTLS_MD_SHA256);


    // Set Key Usage (Digital Signature, Key Encipherment)
    mbedtls_x509write_crt_set_key_usage(crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);

    // Set NS Cert Type (Client and Server) - Standard for Moonlight
    mbedtls_x509write_crt_set_ns_cert_type(crt, MBEDTLS_X509_NS_CERT_TYPE_SSL_CLIENT | MBEDTLS_X509_NS_CERT_TYPE_SSL_SERVER);

    // Set Basic Constraints (CA:FALSE) - Required by some modern validation
    mbedtls_x509write_crt_set_basic_constraints(crt, 0, -1);

    // 10 years validity
    mbedtls_x509write_crt_set_validity(crt, "20200101000000", "20301231235959");

    // achievement: absolute perfection - forensic 16-byte random serial (Standard for GFE/Apollo)
    unsigned char serial_bytes[16];
    for(int i=0; i<16; i++) serial_bytes[i] = (unsigned char)(rand() % 256);
    serial_bytes[0] &= 0x7F; // ensure positive
    mbedtls_x509write_crt_set_serial_raw(crt, serial_bytes, 16);

    // Forensic Recipe for Apollo: Standard Extensions
    // OID_EXTENDED_KEY_USAGE: 2.5.29.37
    // Value: Sequence containing OID_CLIENT_AUTH (1.3.6.1.5.5.7.3.2)
    // Client Auth OID in DER is: 06 08 2b 06 01 05 05 07 03 02
    // Full Sequence (30 0a): 30 0a 06 08 2b 06 01 05 05 07 03 02
    unsigned char eku_val[] = { 0x30, 0x0a, 0x06, 0x08, 0x2b, 0x06, 0x01, 0x05, 0x05, 0x07, 0x03, 0x02 };
    mbedtls_x509write_crt_set_extension(crt, MBEDTLS_OID_EXTENDED_KEY_USAGE, MBEDTLS_OID_SIZE(MBEDTLS_OID_EXTENDED_KEY_USAGE), 0, eku_val, sizeof(eku_val));

    // OID_SUBJECT_ALT_NAME: 2.5.29.17
    /* 3DS Clone: REMOVED SAN extension to match working handheld ports */

    CERT_KEY_PAIR pair;
    pair.x509 = crt;
    pair.pkey = pkey;
    pair.p12 = ctr_drbg; 
    pair.san_list = NULL; 
    
    mbedtls_mpi_free(&serial);
    // don't free entropy yet, mkcert_save needs it potentially? Nah it's fine.
    mbedtls_entropy_free(entropy);
    free(entropy);
    
    return pair;
}

void mkcert_free(CERT_KEY_PAIR certKeyPair) {
    if (certKeyPair.x509) {
        mbedtls_x509write_crt_free((mbedtls_x509write_cert*)certKeyPair.x509);
        free(certKeyPair.x509);
    }
    if (certKeyPair.pkey) {
        mbedtls_pk_free((mbedtls_pk_context*)certKeyPair.pkey);
        free(certKeyPair.pkey);
    }
    if (certKeyPair.p12) {
        mbedtls_ctr_drbg_free((mbedtls_ctr_drbg_context*)certKeyPair.p12);
        free(certKeyPair.p12);
    }
    if (certKeyPair.san_list) {
        free(certKeyPair.san_list);
    }
}

void mkcert_save(const char* certFile, const char* p12File, const char* keyPairFile, CERT_KEY_PAIR certKeyPair) {
    mbedtls_pk_context *pkey = (mbedtls_pk_context *)certKeyPair.pkey;
    mbedtls_x509write_cert *crt = (mbedtls_x509write_cert *)certKeyPair.x509;
    mbedtls_ctr_drbg_context *ctr_drbg = (mbedtls_ctr_drbg_context *)certKeyPair.p12;
    
    unsigned char output_buf[4096];
    
    // Write key
    memset(output_buf, 0, sizeof(output_buf));
    int key_ret = mbedtls_pk_write_key_pem(pkey, output_buf, sizeof(output_buf));
    if (key_ret == 0) {
        SceUID fd = sceIoOpen(keyPairFile, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, output_buf, strlen((char*)output_buf));
            sceIoClose(fd);
            sceIoSync("ms0:", 0);
            LOG_INFO(COMPONENT_NETWORK, "Saved private key to %s", keyPairFile);
        } else {
            LOG_ERROR(COMPONENT_NETWORK, "Failed to open %s for writing: 0x%08x", keyPairFile, fd);
        }
    } else {
        LOG_ERROR(COMPONENT_NETWORK, "mbedtls_pk_write_key_pem failed: %d", key_ret);
    }
    
    // Write cert (PEM)
    memset(output_buf, 0, sizeof(output_buf));
    int ret_pem = mbedtls_x509write_crt_pem(crt, output_buf, sizeof(output_buf), mbedtls_ctr_drbg_random, ctr_drbg);
    if (ret_pem == 0) {
        SceUID fd = sceIoOpen(certFile, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, output_buf, strlen((char*)output_buf));
            sceIoClose(fd);
            sceIoSync("ms0:", 0);
            LOG_INFO(COMPONENT_NETWORK, "Saved PEM certificate to %s", certFile);
        }
    }
    
    // Write cert (DER binary) - Absolute Perfection for Sunshine/GFE
    char derFile[1024];
    strncpy(derFile, certFile, 1023);
    char *dot = strrchr(derFile, '.');
    if (dot) strcpy(dot, ".der");
    else strcat(derFile, ".der");

    memset(output_buf, 0, sizeof(output_buf));
    int ret_der = mbedtls_x509write_crt_der(crt, output_buf, sizeof(output_buf), mbedtls_ctr_drbg_random, ctr_drbg);
    if (ret_der > 0) {
        // mbedtls writes from the end of the buffer backwards for DER!
        SceUID fd = sceIoOpen(derFile, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
        if (fd >= 0) {
            sceIoWrite(fd, output_buf + sizeof(output_buf) - ret_der, ret_der);
            sceIoClose(fd);
            sceIoSync("ms0:", 0);
            LOG_INFO(COMPONENT_NETWORK, "Saved DER certificate to %s (%d bytes)", derFile, ret_der);
        } else {
            LOG_ERROR(COMPONENT_NETWORK, "Failed to open %s for writing: 0x%08x", derFile, fd);
        }
    } else {
        LOG_ERROR(COMPONENT_NETWORK, "mbedtls_x509write_crt_der failed: %d", ret_der);
    }
    
    // We ignore P12 file for mbedtls. It's not strictly necessary for moonlight pairing.
    SceUID fd_p12 = sceIoOpen(p12File, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd_p12 >= 0) {
        sceIoWrite(fd_p12, "DUMMY", 5);
        sceIoClose(fd_p12);
        sceIoSync("ms0:", 0);
    }
}
