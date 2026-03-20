#pragma once

typedef struct _CERT_KEY_PAIR {
    void *x509; // mbedtls_x509_crt
    void *pkey; // mbedtls_pk_context
    void *p12;  // mbedtls_ctr_drbg_context
    void *san_list; // mbedtls_x509_san_list
} CERT_KEY_PAIR, *PCERT_KEY_PAIR;

CERT_KEY_PAIR mkcert_generate(const char* device_name);
void mkcert_free(CERT_KEY_PAIR);
void mkcert_save(const char* certFile, const char* p12File, const char* keyPairFile, CERT_KEY_PAIR certKeyPair);
