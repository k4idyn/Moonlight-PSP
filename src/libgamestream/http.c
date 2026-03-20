/*
 * http.c - PSP-native HTTP client for Moonlight
 * 
 * Replaces libcurl for both HTTP and HTTPS on PSP.
 * Uses raw TCP sockets for HTTP, MbedTLS for HTTPS.
 * This is required because libcurl may not function correctly on PSP hardware.
 */

#include "http.h"
#include "errors.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pspkernel.h>
#include <pspnet_resolver.h>
#include <pspnet_apctl.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <errno.h>
#include <sys/socket.h>
#include "PlatformSockets.h"
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/debug.h>
#include <unistd.h>

/* ---------- Configuration ---------- */
#define HTTP_CONNECT_TIMEOUT_S  10
#define HTTP_READ_TIMEOUT_S     90
#define LOG_FILE "ms0:/moonlight_debug.log"

/* ---------- Static state ---------- */
static char g_certPath[4096];
static char g_keyPath[4096];
static int  g_logLevel;

/* ---------- Helpers ---------- */

#include "../modules/logger.h"
#define LOGF(fmt, ...) LOG_INFO(COMPONENT_NETWORK, fmt, ##__VA_ARGS__)

/* Dummy entropy for MbedTLS – PSP has no /dev/urandom */
static int dummy_entropy(void *data, unsigned char *output, size_t len) {
    (void)data;
    uint32_t t = sceKernelGetSystemTimeLow();
    for (size_t i = 0; i < len; i++) {
        output[i] = (unsigned char)((t >> (i & 3) * 8) ^ i ^ 0xAA);
    }
    return 0;
}

#define MBEDTLS_ERR_NET_SEND_FAILED  -0x004E
#define MBEDTLS_ERR_NET_RECV_FAILED  -0x004C

static int psp_mbed_send(void *ctx, const unsigned char *buf, size_t len) {
    int fd = *(int*)ctx;
    int ret = send(fd, buf, len, 0);
    if (ret < 0) {
        int err = (int)LastSocketError();
        if (err == 11) return MBEDTLS_ERR_SSL_WANT_WRITE;
        LOGF("send() failed errno=%d", err);
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

static int psp_mbed_recv(void *ctx, unsigned char *buf, size_t len) {
    int fd = *(int*)ctx;
    int ret = recv(fd, buf, len, 0);
    if (ret < 0) {
        int err = (int)LastSocketError();
        if (err == 11) return MBEDTLS_ERR_SSL_WANT_READ;
        LOGF("recv() failed errno=%d", err);
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

/* Parse "scheme://host:port/path" into host, port, path.
   Returns 0 on success, -1 on error. */
static int parse_url(const char *url, char *host, size_t hlen,
                     int *port, char *path, size_t plen, int *is_https)
{
    const char *p = url;
    *is_https = 0;
    *port = 80;

    if (strncmp(p, "https://", 8) == 0) {
        *is_https = 1;
        *port = 47984; /* Moonlight HTTPS default */
        p += 8;
    } else if (strncmp(p, "http://", 7) == 0) {
        *port = 47989; /* Moonlight HTTP default */
        p += 7;
    } else {
        return -1;
    }

    /* find optional :port */
    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');

    if (colon && (!slash || colon < slash)) {
        size_t hl = colon - p;
        if (hl >= hlen) hl = hlen - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
        *port = atoi(colon + 1);
    } else if (slash) {
        size_t hl = slash - p;
        if (hl >= hlen) hl = hlen - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
    } else {
        size_t hl = strlen(p);
        if (hl >= hlen) hl = hlen - 1;
        memcpy(host, p, hl);
        host[hl] = '\0';
    }

    if (slash) {
        snprintf(path, plen, "%s", slash + 1);
    } else {
        path[0] = '\0';
    }

    return 0;
}

/* Append bytes to HTTP_DATA, growing the buffer */
static int append_data(PHTTP_DATA data, const char *buf, int len) {
    char *newmem = realloc(data->memory, data->size + len + 1);
    if (!newmem) return -1;
    data->memory = newmem;
    memcpy(data->memory + data->size, buf, len);
    data->size += len;
    data->memory[data->size] = '\0';
    return 0;
}

/* Strip HTTP response headers, leaving only the body */
static void strip_headers(PHTTP_DATA data) {
    if (!data->memory || data->size <= 0) return;
    char *body = strstr(data->memory, "\r\n\r\n");
    if (!body) body = strstr(data->memory, "\n\n");
    if (body) {
        body += (body[0] == '\r') ? 4 : 2;
        int body_len = data->size - (int)(body - data->memory);
        char *new_mem = malloc(body_len + 1);
        if (new_mem) {
            memcpy(new_mem, body, body_len);
            new_mem[body_len] = '\0';
            free(data->memory);
            data->memory = new_mem;
            data->size = body_len;
        }
    }
}

/* ---------- Connect (TCP) ---------- */
static SOCKET open_tcp(const char *host, int port) {
    struct sockaddr_storage addr;
    memset(&addr, 0, sizeof(addr));
    SOCKADDR_LEN addrLen = sizeof(addr);

    LOGF("Resolving host '%s' port %d ...", host, port);
    if (resolveHostName(host, AF_INET, 0, &addr, &addrLen) != 0) {
        LOGF("resolveHostName FAILED errno=%d", (int)LastSocketError());
        gs_error = "DNS resolution failed";
        return -1;
    }
    LOGF("resolveHostName OK");

    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        LOGF("socket() FAILED errno=%d", (int)LastSocketError());
        gs_error = "TCP socket creation failed";
        return -1;
    }

    ((struct sockaddr_in*)&addr)->sin_port = htons((uint16_t)port);

    /* Disable Nagle's algorithm - critical for PSP WiFi */
    int flag = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(flag));

    /* Allow socket reuse to combat TIME_WAIT exhaustion, but don't force RST which angers the server */
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char*)&reuse, sizeof(reuse));

    /* Set timeouts */
    int connect_timeout_us = HTTP_CONNECT_TIMEOUT_S * 1000 * 1000;
    int read_timeout_us = HTTP_READ_TIMEOUT_S * 1000 * 1000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char*)&connect_timeout_us, sizeof(connect_timeout_us));
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&read_timeout_us, sizeof(read_timeout_us));

    /* N3DS Stability Secret: Significantly increase socket buffers to handle Wi-Fi jitter.
     * 128KB is a safe middle ground for the PSP-1000's 32MB limit. */
    int sock_buf_size = 128 * 1024;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char*)&sock_buf_size, sizeof(sock_buf_size));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, (char*)&sock_buf_size, sizeof(sock_buf_size));

    LOGF("Connecting TCP to %s:%d (blocking, timeout=%ds)...", host, port, HTTP_CONNECT_TIMEOUT_S);
    if (connect(s, (struct sockaddr*)&addr, addrLen) < 0) {
        LOGF("connect() FAILED errno=%d", (int)LastSocketError());
        close(s);
        gs_error = "TCP connect failed (check server IP/port and firewall)";
        return -1;
    }
    LOGF("TCP connect SUCCESS to %s:%d", host, port);
    return s;
}

/* ---------- HTTP over plain TCP ---------- */
static int http_raw_request(const char *host, int port, const char *path,
                             PHTTP_DATA data)
{
    SOCKET s = open_tcp(host, port);
    if (s < 0) return GS_FAILED;

    /* Set a generous read timeout */
    int rtimeout = HTTP_READ_TIMEOUT_S * 1000 * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&rtimeout, sizeof(rtimeout));

    /* Build GET request – must be large enough for pairing URLs with cert hex */
    int req_size = strlen(path) + 256;
    char *req = malloc(req_size);
    if (!req) { close(s); return GS_OUT_OF_MEMORY; }
    int reqlen = snprintf(req, req_size,
        "GET /%s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "Accept: */*\r\n"
        "\r\n",
        path, host, port);

    LOGF("Sending HTTP GET (reqlen=%d)", reqlen);
    int total_sent = 0;
    while (total_sent < reqlen) {
        int n = send(s, req + total_sent, reqlen - total_sent, 0);
        if (n < 0) {
            int err = (int)LastSocketError();
            if (err == 11) { /* EAGAIN */
                sceKernelDelayThread(5000);
                continue;
            }
            LOGF("send() FAILED errno=%d", err);
            free(req);
            close(s);
            gs_error = "HTTP send failed";
            return GS_FAILED;
        }
        if (n == 0) break;
        total_sent += n;
    }
    LOGF("send() loop complete: %d bytes sent", total_sent);
    free(req);

    /* Give PSP WiFi hardware time to flush the packet before reading */
    sceKernelDelayThread(100000); /* 100ms */

    /* Read response */
    char buf[1024];
    int received = 0;
    while (1) {
        int n = recv(s, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            if (append_data(data, buf, n) < 0) {
                close(s);
                return GS_OUT_OF_MEMORY;
            }
            received += n;
        } else if (n == 0) {
            break; /* Connection closed by server */
        } else {
            int err = (int)LastSocketError();
            if (err == 11 || err == 116) { /* EAGAIN or ETIMEDOUT */
                sceKernelDelayThread(5000); /* 5ms */
                continue;
            }
            LOGF("recv() exited with errno=%d (received %d bytes so far)", err, received);
            break;
        }
    }
    close(s);

    LOGF("HTTP response received: %d bytes total", data->size);

    if (data->size == 0) {
        gs_error = "Empty HTTP response";
        return GS_FAILED;
    }

    /* Check HTTP status line (200 OK expected) */
    if (strncmp(data->memory, "HTTP/", 5) == 0) {
        int status_code = 0;
        sscanf(data->memory, "HTTP/%*s %d", &status_code);
        LOGF("HTTP status code: %d", status_code);
        if (status_code != 200) {
            LOGF("Non-200 HTTP status, failing");
            gs_error = "HTTP server returned non-200 status";
            return GS_FAILED;
        }
    }

    strip_headers(data);
    return GS_OK;
}

/* ---------- HTTPS over MbedTLS ---------- */
static int https_tls_request(const char *host, int port, const char *path,
                              PHTTP_DATA data)
{
    SOCKET s = open_tcp(host, port);
    if (s < 0) return GS_FAILED;

    /* Set recv timeout for TLS */
    int rtimeout = HTTP_READ_TIMEOUT_S * 1000 * 1000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char*)&rtimeout, sizeof(rtimeout));

    /* Allocate MbedTLS structures on heap to avoid stack overflow */
    mbedtls_entropy_context  *entropy  = malloc(sizeof(mbedtls_entropy_context));
    mbedtls_ctr_drbg_context *ctr_drbg = malloc(sizeof(mbedtls_ctr_drbg_context));
    mbedtls_ssl_context      *ssl      = malloc(sizeof(mbedtls_ssl_context));
    mbedtls_ssl_config       *conf     = malloc(sizeof(mbedtls_ssl_config));
    mbedtls_x509_crt         *clicert  = malloc(sizeof(mbedtls_x509_crt));
    mbedtls_pk_context       *pkey     = malloc(sizeof(mbedtls_pk_context));

    if (!entropy || !ctr_drbg || !ssl || !conf || !clicert || !pkey) {
        LOGF("malloc failed for MbedTLS structures");
        free(entropy); free(ctr_drbg); free(ssl);
        free(conf); free(clicert); free(pkey);
        close(s);
        gs_error = "Out of memory for SSL";
        return GS_OUT_OF_MEMORY;
    }

    mbedtls_ssl_init(ssl);
    mbedtls_ssl_config_init(conf);
    mbedtls_x509_crt_init(clicert);
    mbedtls_pk_init(pkey);
    mbedtls_entropy_init(entropy);
    mbedtls_ctr_drbg_init(ctr_drbg);

    int ret;

#define TLS_CLEANUP() do { \
    if (clicert) { mbedtls_x509_crt_free(clicert); free(clicert); clicert = NULL; } \
    if (pkey) { mbedtls_pk_free(pkey); free(pkey); pkey = NULL; } \
    if (ssl) { mbedtls_ssl_free(ssl); free(ssl); ssl = NULL; } \
    if (conf) { mbedtls_ssl_config_free(conf); free(conf); conf = NULL; } \
    if (ctr_drbg) { mbedtls_ctr_drbg_free(ctr_drbg); free(ctr_drbg); ctr_drbg = NULL; } \
    if (entropy) { mbedtls_entropy_free(entropy); free(entropy); entropy = NULL; } \
    close(s); \
} while(0)

    if ((ret = mbedtls_ctr_drbg_seed(ctr_drbg, dummy_entropy, entropy, NULL, 0)) != 0) {
        LOGF("ctr_drbg_seed failed: -0x%x", -ret);
        TLS_CLEANUP();
        gs_error = "SSL DRBG seed failed";
        return GS_FAILED;
    }

    if ((ret = mbedtls_x509_crt_parse_file(clicert, g_certPath)) != 0) {
        LOGF("x509_crt_parse_file failed '%s': -0x%x", g_certPath, -ret);
        TLS_CLEANUP();
        gs_error = "Failed to load client certificate";
        return GS_FAILED;
    }

    if ((ret = mbedtls_pk_parse_keyfile(pkey, g_keyPath, NULL, mbedtls_ctr_drbg_random, ctr_drbg)) != 0) {
        LOGF("pk_parse_keyfile failed '%s': -0x%x", g_keyPath, -ret);
        TLS_CLEANUP();
        gs_error = "Failed to load private key";
        return GS_FAILED;
    }

    mbedtls_ssl_config_defaults(conf,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);

    mbedtls_ssl_conf_max_version(conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_min_version(conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_authmode(conf, MBEDTLS_SSL_VERIFY_NONE); /* Server uses self-signed cert */

    if ((ret = mbedtls_ssl_conf_own_cert(conf, clicert, pkey)) != 0) {
        LOGF("ssl_conf_own_cert failed: -0x%x", -ret);
        TLS_CLEANUP();
        gs_error = "SSL cert config failed";
        return GS_FAILED;
    }

    mbedtls_ssl_conf_rng(conf, mbedtls_ctr_drbg_random, ctr_drbg);
    /* Allow all default ciphers (including ECDHE which modern Sunshine requires) */

    if ((ret = mbedtls_ssl_setup(ssl, conf)) != 0) {
        LOGF("ssl_setup failed: -0x%x", -ret);
        TLS_CLEANUP();
        gs_error = "SSL setup failed";
        return GS_FAILED;
    }

    mbedtls_ssl_set_bio(ssl, &s, psp_mbed_send, psp_mbed_recv, NULL);

    int mbed_ret;
    unsigned int hs_start = sceKernelGetSystemTimeLow();
    unsigned int hs_timeout_us = 60 * 1000 * 1000; /* 60 seconds */
    unsigned int last_log_time = hs_start;
    while ((mbed_ret = mbedtls_ssl_handshake(ssl)) != 0) {
        if (mbed_ret != MBEDTLS_ERR_SSL_WANT_READ && mbed_ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            LOGF("TLS handshake FAILED: -0x%x", -mbed_ret);
            TLS_CLEANUP();
            gs_error = "TLS handshake failed";
            return GS_FAILED;
        }
        
        unsigned int now = sceKernelGetSystemTimeLow();
        if ((now - last_log_time) > 10 * 1000 * 1000) {
            LOGF("TLS handshake still working... (%d seconds elapsed)", (now - hs_start) / 1000000);
            last_log_time = now;
        }
        
        /* Wall-clock timeout to prevent infinite spin */
        if ((now - hs_start) > hs_timeout_us) {
            LOGF("TLS handshake TIMED OUT after 60s");
            TLS_CLEANUP();
            gs_error = "TLS handshake timed out";
            return GS_FAILED;
        }
        
        /* Yield to other threads to prevent system lockup during heavy crypto */
        sceKernelDelayThread(10000); 
    }
    LOGF("TLS handshake SUCCESS in %d seconds!", (sceKernelGetSystemTimeLow() - hs_start) / 1000000);

    /* Send GET request – must be large enough for pairing URLs with cert hex */
    int req_size = strlen(path) + 256;
    char *req = malloc(req_size);
    if (!req) { TLS_CLEANUP(); return GS_OUT_OF_MEMORY; }
    int reqlen = snprintf(req, req_size,
        "GET /%s HTTP/1.0\r\n"
        "Host: %s:%d\r\n"
        "Connection: close\r\n"
        "Accept: */*\r\n"
        "\r\n",
        path, host, port);

    LOGF("Sending HTTPS GET (reqlen=%d)", reqlen);
    int total_sent = 0;
    while (total_sent < reqlen) {
        int n = mbedtls_ssl_write(ssl, (unsigned char*)(req + total_sent), reqlen - total_sent);
        if (n < 0) {
            if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE) {
                sceKernelDelayThread(5000);
                continue;
            }
            free(req);
            LOGF("ssl_write FAILED: -0x%x", -n);
            TLS_CLEANUP();
            gs_error = "HTTPS send failed";
            return GS_FAILED;
        }
        if (n == 0) break;
        total_sent += n;
    }
    LOGF("ssl_write loop complete: %d bytes sent", total_sent);
    free(req);

    /* Read response */
    char buf[1024];
    while (1) {
        ret = mbedtls_ssl_read(ssl, (unsigned char*)buf, sizeof(buf) - 1);
        if (ret > 0) {
            buf[ret] = '\0';
            if (append_data(data, buf, ret) < 0) {
                TLS_CLEANUP();
                return GS_OUT_OF_MEMORY;
            }
        } else if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            usleep(5000);
            continue;
        } else {
            /* ret == 0 (EOF) or negative (error) - both mean we're done reading */
            LOGF("TLS read loop exit: ret=%d, total=%d bytes", ret, data->size);
            break;
        }
    }

    mbedtls_ssl_close_notify(ssl);
    TLS_CLEANUP();

    if (data->size == 0) {
        LOGF("HTTPS returned empty body");
        gs_error = "Empty HTTPS response";
        return GS_FAILED;
    }

    strip_headers(data);
    LOGF("HTTPS response body: %d bytes", data->size);
    return GS_OK;
}

/* ---------- Public API ---------- */

int http_init(const char *keyDirectory, int logLevel) {
    g_logLevel = logLevel;
    snprintf(g_certPath, sizeof(g_certPath), "%s/%s", keyDirectory, CERTIFICATE_FILE_NAME);
    snprintf(g_keyPath,  sizeof(g_keyPath),  "%s/%s", keyDirectory, KEY_FILE_NAME);
    LOGF("http_init: cert='%s' key='%s'", g_certPath, g_keyPath);
    return GS_OK;
}

int http_request(char *url, PHTTP_DATA data) {
    char host[256];
    int  port;
    char path[8192];
    int  is_https;

    LOGF("http_request: %s", url);

    /* Reset output buffer */
    if (data->size > 0) {
        free(data->memory);
        data->memory = malloc(1);
        if (!data->memory) return GS_OUT_OF_MEMORY;
        data->size = 0;
    }
    if (data->memory) data->memory[0] = '\0';

    if (parse_url(url, host, sizeof(host), &port, path, sizeof(path), &is_https) != 0) {
        LOGF("parse_url FAILED for: %s", url);
        gs_error = "Malformed URL";
        return GS_FAILED;
    }

    LOGF("Parsed: scheme=%s host='%s' port=%d path='%s'",
         is_https ? "HTTPS" : "HTTP", host, port, path);

    if (port <= 0 || port > 65535) {
        LOGF("Invalid port %d – aborting", port);
        gs_error = "Invalid port in URL";
        return GS_FAILED;
    }

    int ret;
    if (is_https) {
        ret = https_tls_request(host, port, path, data);
    } else {
        /* Retry plain HTTP up to 3 times - PSP WiFi has transient 0-byte recv issues */
        int attempt;
        for (attempt = 0; attempt < 3; attempt++) {
            if (attempt > 0) {
                LOGF("Retrying HTTP request (attempt %d/3)...", attempt + 1);
                sceKernelDelayThread(500000); /* 500ms between retries */
                /* Reset output buffer for retry */
                if (data->size > 0) {
                    free(data->memory);
                    data->memory = malloc(1);
                    if (!data->memory) return GS_OUT_OF_MEMORY;
                    data->memory[0] = '\0';
                    data->size = 0;
                }
            }
            ret = http_raw_request(host, port, path, data);
            if (ret == GS_OK) break;
        }
    }

    if (ret == GS_OK) {
        LOGF("http_request SUCCESS for %s", url);
    } else {
        LOGF("http_request FAILED for %s (gs_error=%s)", url, gs_error ? gs_error : "NULL");
    }
    return ret;
}

void http_cleanup(void) {
    /* Nothing to clean up – no global CURL state */
}

PHTTP_DATA http_create_data(void) {
    PHTTP_DATA data = malloc(sizeof(HTTP_DATA));
    if (!data) return NULL;
    data->memory = malloc(1);
    if (!data->memory) { free(data); return NULL; }
    data->memory[0] = '\0';
    data->size = 0;
    return data;
}

void http_free_data(PHTTP_DATA data) {
    if (data) {
        if (data->memory) free(data->memory);
        free(data);
    }
}
