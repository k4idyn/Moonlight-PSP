/*
 * network_connect.c - Wi-Fi connection, Sunshine pairing, and RTSP session
 *
 * Step 1: Connect PSP to saved Wi-Fi access point
 * Step 2: HTTP GET to Sunshine pairing endpoint
 * Step 3: RTSP OPTIONS, DESCRIBE, SETUP, PLAY to start video stream
 */

#include <pspkernel.h>
#include <pspdebug.h>
#include <pspsdk.h>
#include <pspthreadman.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <psputility.h>
#include <psputility_netmodules.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <pspiofilemgr.h>
#include "config.h"
#include "settings_menu.h"

#include "pairing_pin_ui.h"
#include "stream_connect_ui.h"
#include "ui_manager.h"
#include "crypto_lite.h"
#include "cert_client.h"
#include "moonlight_ports.h"
#include "moonlight_proto.h"
#include "client_identity.h"
#include "diag_log.h"
#define pair_log(fmt, ...) diag_log_write("NET", fmt, ##__VA_ARGS__)

extern int network_me_reserve_client_port(unsigned short *out_port);
extern int network_me_send_video_ping_burst(const char *server_ip,
                                            int server_port,
                                            const char *ping_payload);

/* GU owns VRAM during UI flows; debug-screen writes cause visible corruption. */
#define pspDebugScreenPrintf(...) ((void)0)

#define MBEDTLS_CONFIG_FILE "mbedtls_psp_config.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/gcm.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "audio_thread.h"

#include <psprtc.h>

/* ============================================================================
 * Configuration
 * ============================================================================
 */
#define DEFAULT_SUNSHINE_HOST "192.168.1.100"
#define SUNSHINE_HTTP_PORT           MOONLIGHT_HTTP_PORT
#define SUNSHINE_HTTPS_PORT          MOONLIGHT_HTTPS_PORT
#define SUNSHINE_RTSP_PORT_PRIMARY   MOONLIGHT_RTSP_PORT
#define SUNSHINE_RTSP_PORT_FALLBACK  MOONLIGHT_RTSP_PORT_LEGACY
#define CLIENT_UNIQUE_ID    client_identity_get_uid() /* 16-char unique device ID   */
#define DEVICE_NAME         "roth"             /* Device name for pairing    */

extern PspConfig g_psp_config;

/* Buffer sizes */
#define HTTP_BUF_SIZE       4096
#define RTSP_BUF_SIZE       4096
#define SDP_BUF_SIZE        2048
#define RTSP_CONNECT_TIMEOUT_MS 10000
#define RTSP_RECV_TIMEOUT_MS    5000
#define RTSP_CONNECT_MAX_RETRIES 3

/* RTSP CSeq counter (increments per request) */
static int rtsp_cseq = 1;
static char g_sunshine_host[16] = DEFAULT_SUNSHINE_HOST;

/* Video stream parameters — filled during RTSP SETUP, consumed by network_me.c */
char g_video_server_ip[64]      = {0};   /* Sunshine IP, copied from g_sunshine_host */
int  g_video_server_port        = 47998; /* from server_port= in video SETUP response */
int  g_video_client_port        = 0;     /* local UDP video port reserved by network_me */
char g_video_ping_payload[17]   = {0};   /* X-SS-Ping-Payload from video SETUP (16 chars + NUL) */

/* Audio stream parameters — for audio ping thread */
int  g_audio_server_port        = 48000;
char g_audio_ping_payload[17]   = {0};
int  g_audio_rtsp_ok            = 0;   /* set to 1 when RTSP SETUP audio succeeds */

/* Control stream parameters — for ENet handshake */
int  g_control_server_port      = 47999;
unsigned int g_control_connect_data = 0;  /* X-SS-Connect-Data from control SETUP */
unsigned char g_remote_input_key[16] = {0};
int g_remote_input_key_valid = 0;

static int g_rtsp_encrypted = 0;
static int g_rtsp_last_resp_encrypted = 0;  /* Was last recv an encrypted frame? */
static uint32_t g_rtsp_enc_tx_seq = 0;
static mbedtls_gcm_context g_rtsp_gcm_ctx;

/* Encryption capability advertised by the server in DESCRIBE SDP.
 * Parsed from "a=x-ss-general.encryptionSupported:<N>".
 * Sunshine typically sends 5 = SS_ENC_VIDEO(1) | SS_ENC_CONTROL_V2(4). */
static int g_encryption_supported = 0;

/* avRiKeyId — first 4 bytes of the AV decryption IV for audio packets.
 * Set during /launch or /resume from the locally generated rikeyid. */
unsigned int g_av_ri_key_id = 0;

/* Whether audio AES-CBC encryption was negotiated with the server.
 * Set to 1 only if both the server and client agree on SS_ENC_AUDIO (bit 1).
 * When 0, audio RTP payloads are raw Opus and must NOT be decrypted. */
int g_audio_encryption_enabled = 0;

/* Local UDP bind IP — always empty for real PSP hardware (INADDR_ANY). */
char g_local_bind_ip[16] = {0};
char g_nic_ip[16] = {0};

void network_set_local_bind_ip(const char *ip)
{
    /* No-op for real hardware — always use INADDR_ANY. */
    (void)ip;
    memset(g_local_bind_ip, 0, sizeof(g_local_bind_ip));
}

static char g_rtsp_target_url[256] = "";
static char g_rtsp_session_id[64] = "";

static int g_rtsp_port = SUNSHINE_RTSP_PORT_PRIMARY;
static char g_rtsp_connect_host[64] = DEFAULT_SUNSHINE_HOST;
static char g_rtsp_host_header[96] = DEFAULT_SUNSHINE_HOST;

static void rtsp_update_host_header(void)
{
    /* Moonlight-common-c usually sends JUST the host in the Host: header. */
    strncpy(g_rtsp_host_header, g_rtsp_connect_host, sizeof(g_rtsp_host_header) - 1);
}


/*
 * network_auto_bind_for_loopback - DISABLED for real PSP hardware.
 *
 * Loopback detection was only needed for PPSSPP same-host debugging.
 * On real PSP hardware the target is always a remote LAN IP, so this
 * function is a no-op.  Retained to avoid breaking callers.
 *
 * Returns: 0 always (no action)
 */
int network_auto_bind_for_loopback(const char *target_ip)
{
    (void)target_ip;

    /* Fill g_nic_ip for diagnostic logging only */
    {
        union SceNetApctlInfo info;
        memset(&info, 0, sizeof(info));
        if (sceNetApctlGetInfo(8, &info) == 0 && info.ip[0] != '\0') {
            strncpy(g_nic_ip, info.ip, sizeof(g_nic_ip) - 1);
            g_nic_ip[sizeof(g_nic_ip) - 1] = '\0';
        }
    }

    return 0;
}

static char g_last_paired_host[16] = "";

static void rtsp_rewrite_target_authority(const char *host, int port)
{
    const char *scheme_end;
    const char *path_start;
    char path_part[160];

    if (!host || !host[0] || port <= 0) {
        return;
    }

    scheme_end = strstr(g_rtsp_target_url, "://");
    if (!scheme_end) {
        snprintf(g_rtsp_target_url, sizeof(g_rtsp_target_url),
                 "rtsp://%s:%d", host, port);
        rtsp_update_host_header();
        return;
    }

    path_start = strchr(scheme_end + 3, '/');
    if (path_start && path_start[0]) {
        strncpy(path_part, path_start, sizeof(path_part) - 1);
        path_part[sizeof(path_part) - 1] = '\0';
    } else {
        path_part[0] = '\0';
    }

    snprintf(g_rtsp_target_url, sizeof(g_rtsp_target_url),
             "rtsp://%s:%d%s", host, port, path_part);
    rtsp_update_host_header();
}

static int fill_random_bytes(unsigned char *out, size_t len);
static int http_pair_get(const char *url, char *resp, int resp_size);
static int xml_get_value_safe(const char *buf, const char *tag,
                              char *out, int out_size);

static int xml_get_status_code_attr(const char *buf)
{
    const char *p;
    char tmp[16];
    int i = 0;

    if (!buf) {
        return -1;
    }

    p = strstr(buf, "status_code=");
    if (!p) {
        return -1;
    }

    p += (int)strlen("status_code=");
    if (*p == '"' || *p == '\'') {
        char quote = *p++;
        while (*p && *p != quote && i < (int)sizeof(tmp) - 1) {
            tmp[i++] = *p++;
        }
    } else {
        while (*p && *p != ' ' && *p != '>' && i < (int)sizeof(tmp) - 1) {
            tmp[i++] = *p++;
        }
    }

    if (i == 0) {
        return -1;
    }

    tmp[i] = '\0';
    return atoi(tmp);
}

static void rtsp_set_default_target(void)
{
    strncpy(g_rtsp_connect_host, g_sunshine_host, sizeof(g_rtsp_connect_host) - 1);
    g_rtsp_connect_host[sizeof(g_rtsp_connect_host) - 1] = '\0';
    g_rtsp_port = SUNSHINE_RTSP_PORT_PRIMARY;

    snprintf(g_rtsp_target_url, sizeof(g_rtsp_target_url),
             "rtsp://%s:%d", g_sunshine_host, g_rtsp_port);
    rtsp_update_host_header();
}

static void rtsp_set_target_from_session_url(const char *session_url)
{
    const char *scheme_end;
    const char *auth_start;
    const char *path_start;
    char authority[96];
    int auth_len;
    char *port_sep;
    char *host_end;
    long parsed_port;

    if (!session_url || !session_url[0]) {
        rtsp_set_default_target();
        return;
    }

    strncpy(g_rtsp_target_url, session_url, sizeof(g_rtsp_target_url) - 1);
    g_rtsp_target_url[sizeof(g_rtsp_target_url) - 1] = '\0';

    strncpy(g_rtsp_connect_host, g_sunshine_host, sizeof(g_rtsp_connect_host) - 1);
    g_rtsp_connect_host[sizeof(g_rtsp_connect_host) - 1] = '\0';

    scheme_end = strstr(session_url, "://");
    auth_start = scheme_end ? (scheme_end + 3) : session_url;
    path_start = strchr(auth_start, '/');
    if (!path_start) {
        path_start = auth_start + strlen(auth_start);
    }

    auth_len = (int)(path_start - auth_start);
    if (auth_len <= 0 || auth_len >= (int)sizeof(authority)) {
        return;
    }

    memcpy(authority, auth_start, auth_len);
    authority[auth_len] = '\0';

    if (authority[0] == '[') {
        host_end = strchr(authority, ']');
        if (host_end) {
            int host_len = (int)(host_end - authority + 1);
            if (host_len > 0 && host_len < (int)sizeof(g_rtsp_connect_host)) {
                memcpy(g_rtsp_connect_host, authority, host_len);
                g_rtsp_connect_host[host_len] = '\0';
            }
            if (host_end[1] == ':') {
                parsed_port = strtol(host_end + 2, NULL, 10);
                if (parsed_port > 0 && parsed_port <= 65535) {
                    g_rtsp_port = (int)parsed_port;
                }
            }
        }
        rtsp_update_host_header();
        return;
    }

    port_sep = strrchr(authority, ':');
    if (port_sep) {
        *port_sep = '\0';
        parsed_port = strtol(port_sep + 1, NULL, 10);
        if (parsed_port > 0 && parsed_port <= 65535) {
            g_rtsp_port = (int)parsed_port;
        }
    }

    if (authority[0]) {
        strncpy(g_rtsp_connect_host, authority, sizeof(g_rtsp_connect_host) - 1);
        g_rtsp_connect_host[sizeof(g_rtsp_connect_host) - 1] = '\0';
    }

    rtsp_update_host_header();
}

/* mbedTLS net error codes — defined locally to avoid including net_sockets.h
 * which tries to pull in POSIX socket headers that don't exist on PSP. */
#ifndef MBEDTLS_ERR_NET_SEND_FAILED
#define MBEDTLS_ERR_NET_SEND_FAILED  -0x004E
#endif
#ifndef MBEDTLS_ERR_NET_RECV_FAILED
#define MBEDTLS_ERR_NET_RECV_FAILED  -0x004C
#endif

/*
 * PSP BIO callbacks for mbedTLS.
 *
 * mbedtls_net_send / mbedtls_net_recv assume POSIX sockets (send/recv),
 * which are not available on PSP.  These thin wrappers call
 * sceNetInetSend / sceNetInetRecv instead and translate errno values
 * into mbedTLS error codes that the TLS state machine understands.
 */
static int psp_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int sock = *(int *)ctx;
    int ret  = sceNetInetSend(sock, buf, (int)len, 0);
    if (ret < 0) {
        int err = sceNetInetGetErrno();
        if (err == EAGAIN || err == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

static int psp_bio_recv(void *ctx, unsigned char *buf, size_t len)
{
    int sock = *(int *)ctx;
    int ret  = sceNetInetRecv(sock, buf, (int)len, 0);
    if (ret < 0) {
        int err = sceNetInetGetErrno();
        if (err == EAGAIN || err == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_READ;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    return ret;
}

/*
 * https_launch_get - Perform HTTPS GET with client certificate via mbedTLS.
 *
 * Sunshine's /launch endpoint runs on the HTTPS server (port 47984) and
 * requires mutual TLS: the client MUST present the certificate that was
 * registered during pairing (cert_client.h).  The PSP sceHttp* API does
 * not support client-certificate authentication, so we do the TLS
 * handshake ourselves using mbedTLS which is already linked.
 *
 * host      : target IPv4 address string (e.g. "10.0.0.73")
 * port      : target HTTPS port (47984)
 * path      : full request path including query string
 * resp      : output buffer for the HTTP response body (XML)
 * resp_size : size of resp buffer
 *
 * Returns 0 on success (resp contains XML body), -1 on any error.
 */
int https_launch_get(const char *host, int port,
                             const char *path, char *resp, int resp_size)
{
    int sock = -1;
    int ret, nb;
    int connected = 0;
    struct sockaddr_in addr;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    clicert;
    mbedtls_pk_context  pkey;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;

    char request[1536];
    char raw_stack[512 + 4096]; /* Use stack to avoid heap contention with main thread */
    char *raw = raw_stack;
    int  raw_size = sizeof(raw_stack);
    int  total = 0;
    char *body;

    /* Removed raw = malloc(...) and if (!raw) check */

    /* --- init mbedTLS objects --- */
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&clicert);
    mbedtls_pk_init(&pkey);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)"psp-launch", 10);
    if (ret != 0) {
        pair_log("[LAUNCH-TLS] ctr_drbg seed failed: -0x%04X\n", -ret);
        goto tls_cleanup;
    }

    /* Parse embedded PEM certificate (decode from hex to PEM first).
     * g_client_cert_hex is the hex-encoded PEM string. */
    {
        size_t hex_len = strlen(g_client_cert_hex);
        size_t pem_size = (hex_len / 2) + 1;
        char pem_buf[1536]; /* Typical Moonlight client cert is ~1KB */
        if (pem_size > sizeof(pem_buf)) { ret = -1; goto tls_cleanup; }
        
        hex_to_bytes_lite(g_client_cert_hex, (unsigned char*)pem_buf, hex_len);
        pem_buf[pem_size - 1] = '\0';

        ret = mbedtls_x509_crt_parse(&clicert, (unsigned char*)pem_buf, pem_size);
        /* Removed free(pem_buf) */
        if (ret != 0) {
            pair_log("[LAUNCH-TLS] client cert parse failed: -0x%04X\n", -ret);
            goto tls_cleanup;
        }
    }

    /* Parse embedded PEM private key */
    ret = mbedtls_pk_parse_key(&pkey,
                                (const unsigned char *)g_client_key_pem,
                                strlen(g_client_key_pem) + 1,
                                NULL, 0);
    if (ret != 0) {
        pair_log("[LAUNCH-TLS] private key parse failed: -0x%04X\n", -ret);
        goto tls_cleanup;
    }

    /* --- open raw TCP socket via PSP inet --- */
    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        pair_log("[LAUNCH-TLS] socket failed (errno %d)\n", sceNetInetGetErrno());
        ret = -1; goto tls_cleanup;
    }
    {
        int reuse = 1;
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = (unsigned char)sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    /* Non-blocking connect with select()-based wait */
    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    ret = sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) {
        connected = 1;
    }
    if (ret < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EALREADY && e != EAGAIN && e != EWOULDBLOCK) {
            pair_log("[LAUNCH-TLS] connect failed immediately errno=%d\n", e);
            ret = -1; goto tls_cleanup;
        }
    }

    if (!connected) {
        fd_set wfds;
        struct timeval tv;
        int optval;
        socklen_t optlen;

        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        tv.tv_sec  = 10;
        tv.tv_usec = 0;

        ret = sceNetInetSelect(sock + 1, NULL, &wfds, NULL, &tv);
        if (ret > 0) {
            optval = -1;
            optlen = sizeof(optval);
            sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
            if (optval == 0) {
                connected = 1;
            } else {
                pair_log("[LAUNCH-TLS] connect SO_ERROR=%d\n", optval);
            }
        }
    }

    if (!connected) {
        pair_log("[LAUNCH-TLS] connect timed out (10s)\n");
        ret = -1;
        goto tls_cleanup;
    }

    /* Switch to non-blocking for asynchronous mbedTLS I/O with timeouts */
    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    pair_log("[LAUNCH-TLS] TCP connected to %s:%d\n", host, port);

    /* --- configure mbedTLS --- */
    ret = mbedtls_ssl_config_defaults(&conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        pair_log("[LAUNCH-TLS] ssl config defaults failed: -0x%04X\n", -ret);
        goto tls_cleanup;
    }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    /* Present client certificate for mutual TLS */
    ret = mbedtls_ssl_conf_own_cert(&conf, &clicert, &pkey);
    if (ret != 0) {
        pair_log("[LAUNCH-TLS] ssl_conf_own_cert failed: -0x%04X\n", -ret);
        goto tls_cleanup;
    }

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) {
        pair_log("[LAUNCH-TLS] ssl_setup failed: -0x%04X\n", -ret);
        goto tls_cleanup;
    }

    /* Set Server Name Indication (SNI) - required by Sunshine/GFE TLS servers */
    mbedtls_ssl_set_hostname(&ssl, host);

    /* Wire the PSP socket into mbedTLS via custom BIO callbacks.
     * PSP uses sceNetInetSend/Recv, not POSIX send/recv, so we can't use
     * mbedtls_net_send/recv.  We pass the socket fd directly as context. */
    {
        static int bio_sock;
        bio_sock = sock;
        mbedtls_ssl_set_bio(&ssl, &bio_sock, psp_bio_send, psp_bio_recv, NULL);
    }

    /* --- TLS handshake (this is where the client cert is presented) --- */
    pair_log("[LAUNCH-TLS] starting TLS handshake...\n");
    u32 hs_t = sceKernelGetSystemTimeLow();
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            pair_log("[LAUNCH-TLS] handshake failed: -0x%04X\n", -ret);
            goto tls_cleanup;
        }
        if (sceKernelGetSystemTimeLow() - hs_t > 20000000) {
            pair_log("[LAUNCH-TLS] handshake timed out (20s)\n");
            goto tls_cleanup;
        }
        sceKernelDelayThread(5000); /* Reduced from 10ms for speed */
    }
    pair_log("[LAUNCH-TLS] TLS handshake OK\n");

    /* --- send HTTP GET --- */
    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\n"
             "Host: %s:%d\r\n"
             "User-Agent: PSPMoonlight/1.0\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host, port);

    {
        int req_len = (int)strlen(request);
        int written = 0;
        u32 wr_t = sceKernelGetSystemTimeLow();
        while (written < req_len) {
            ret = mbedtls_ssl_write(&ssl,
                                    (const unsigned char *)request + written,
                                    req_len - written);
            if (ret > 0) { written += ret; wr_t = sceKernelGetSystemTimeLow(); continue; }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                pair_log("[LAUNCH-TLS] write failed: -0x%04X\n", -ret);
                goto tls_cleanup;
            }
            if (sceKernelGetSystemTimeLow() - wr_t > 10000000) {
                pair_log("[LAUNCH-TLS] write timed out\n");
                goto tls_cleanup;
            }
            sceKernelDelayThread(5000);
        }
    }

    /* --- read response --- */
    memset(raw, 0, raw_size);
    u32 rd_t = sceKernelGetSystemTimeLow();
    while (total < raw_size - 1) {
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)raw + total,
                                raw_size - 1 - total);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (sceKernelGetSystemTimeLow() - rd_t > 10000000) {
                pair_log("[LAUNCH-TLS] read timeout (%d bytes recv)\n", total);
                break;
            }
            sceKernelDelayThread(5000);
            continue;
        }
        if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            /* Common for HTTP/1.0 connections */
            break;
        }
        if (ret < 0) {
            pair_log("[LAUNCH-TLS] read failed: -0x%04X\n", -ret);
            break;
        }
        total += ret;
        rd_t = sceKernelGetSystemTimeLow();
    }
    raw[total] = '\0';
    pair_log("[LAUNCH-TLS] received %d bytes\n", total);

    /* Extract body (after \r\n\r\n) */
    body = strstr(raw, "\r\n\r\n");
    if (body) {
        body += 4;
        int act_len = total - (body - raw);
        if (act_len > resp_size - 1) act_len = resp_size - 1;
        if (act_len < 0) act_len = 0;
        memcpy(resp, body, act_len);
        resp[act_len] = '\0';
    } else {
        /* No header/body separator — copy the entire response */
        int act_len = total;
        if (act_len > resp_size - 1) act_len = resp_size - 1;
        if (act_len < 0) act_len = 0;
        memcpy(resp, raw, act_len);
        resp[act_len] = '\0';
    }

    ret = (total > 0) ? 0 : -1;

tls_cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&clicert);
    mbedtls_pk_free(&pkey);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (sock >= 0) {
        /* Force RST (not FIN) so kernel frees the address immediately.
         * Without this, the PSP TCP stack enters TIME_WAIT and returns
         * EADDRNOTAVAIL (errno 125) on ALL subsequent connect() calls. */
        struct linger lg = { 1, 0 };
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        sceNetInetClose(sock);
    }
    /* Removed free(raw) as it is now on stack */

    return ret;
}

/*
 * https_launch_get_binary - Download binary data via HTTPS with client cert.
 *
 * Same as https_launch_get but handles binary response bodies (e.g. PNG).
 * Uses memcpy instead of strncpy so null bytes are preserved.
 *
 * Returns: number of body bytes on success, -1 on error.
 */
int https_launch_get_binary(const char *host, int port,
                            const char *path, char *resp, int resp_size)
{
    int sock = -1;
    int ret, nb;
    int connected = 0;
    struct sockaddr_in addr;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    clicert;
    mbedtls_pk_context  pkey;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;

    char request[1536];
    char *raw = NULL;
    int  raw_size = resp_size + 4096; /* extra room for HTTP headers */
    int  total = 0;
    int  body_len = -1;

    raw = (char *)malloc(raw_size);
    if (!raw) return -1;

    /* --- init mbedTLS objects --- */
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&clicert);
    mbedtls_pk_init(&pkey);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)"psp-bin", 7);
    if (ret != 0) { goto bin_cleanup; }

    {
        size_t hex_len = strlen(g_client_cert_hex);
        size_t pem_len = hex_len / 2;
        unsigned char *pem_buf = (unsigned char *)malloc(pem_len + 1);
        if (!pem_buf) { ret = -1; goto bin_cleanup; }
        hex_to_bytes_lite(g_client_cert_hex, pem_buf, hex_len);
        pem_buf[pem_len] = '\0';
        ret = mbedtls_x509_crt_parse(&clicert, pem_buf, pem_len + 1);
        free(pem_buf);
        if (ret != 0) { goto bin_cleanup; }
    }

    ret = mbedtls_pk_parse_key(&pkey,
                                (const unsigned char *)g_client_key_pem,
                                strlen(g_client_key_pem) + 1, NULL, 0);
    if (ret != 0) { goto bin_cleanup; }

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) { ret = -1; goto bin_cleanup; }
    {
        int reuse = 1;
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = (unsigned char)sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    ret = sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) {
        connected = 1;
    }
    if (ret < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EALREADY && e != EAGAIN && e != EWOULDBLOCK) {
            ret = -1; goto bin_cleanup;
        }
    }

    if (!connected) {
        fd_set wfds;
        struct timeval tv;
        int optval;
        socklen_t optlen;

        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        tv.tv_sec  = 5;
        tv.tv_usec = 0;

        ret = sceNetInetSelect(sock + 1, NULL, &wfds, NULL, &tv);
        if (ret > 0) {
            optval = -1;
            optlen = sizeof(optval);
            sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
            if (optval == 0) {
                connected = 1;
            }
        }
    }

    if (!connected) {
        ret = -1;
        goto bin_cleanup;
    }

    nb = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { goto bin_cleanup; }

    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
    ret = mbedtls_ssl_conf_own_cert(&conf, &clicert, &pkey);
    if (ret != 0) { goto bin_cleanup; }

    ret = mbedtls_ssl_setup(&ssl, &conf);
    if (ret != 0) { goto bin_cleanup; }

    mbedtls_ssl_set_hostname(&ssl, host);

    {
        static int bio_sock_bin;
        bio_sock_bin = sock;
        mbedtls_ssl_set_bio(&ssl, &bio_sock_bin, psp_bio_send, psp_bio_recv, NULL);
    }

    /* --- TLS handshake --- */
    u32 hs_t = sceKernelGetSystemTimeLow();
    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
            ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            goto bin_cleanup;
        }
        if (sceKernelGetSystemTimeLow() - hs_t > 5000000) goto bin_cleanup;
        sceKernelDelayThread(10000);
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.0\r\nHost: %s:%d\r\n"
             "User-Agent: PSPMoonlight/1.0\r\nConnection: close\r\n\r\n",
             path, host, port);

    {
        int req_len = (int)strlen(request);
        int written = 0;
        u32 wr_t = sceKernelGetSystemTimeLow();
        while (written < req_len) {
            ret = mbedtls_ssl_write(&ssl, (const unsigned char *)request + written,
                                    req_len - written);
            if (ret > 0) { written += ret; wr_t = sceKernelGetSystemTimeLow(); continue; }
            if (ret != MBEDTLS_ERR_SSL_WANT_READ &&
                ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                goto bin_cleanup;
            }
            if (sceKernelGetSystemTimeLow() - wr_t > 5000000) goto bin_cleanup;
            sceKernelDelayThread(10000);
        }
    }

    /* Read full response (headers + binary body) */
    memset(raw, 0, raw_size);
    u32 rd_t = sceKernelGetSystemTimeLow();
    while (total < raw_size - 1) {
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)raw + total,
                                raw_size - 1 - total);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (sceKernelGetSystemTimeLow() - rd_t > 5000000) break;
            sceKernelDelayThread(10000);
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) break;
        total += ret;
        rd_t = sceKernelGetSystemTimeLow();
    }

    /* Find header/body boundary and copy binary body with memcpy */
    {
        int i;
        int header_end = -1;
        for (i = 0; i < total - 3; i++) {
            if (raw[i] == '\r' && raw[i+1] == '\n' &&
                raw[i+2] == '\r' && raw[i+3] == '\n') {
                header_end = i + 4;
                break;
            }
        }
        if (header_end >= 0) {
            body_len = total - header_end;
            if (body_len > resp_size) body_len = resp_size;
            memcpy(resp, raw + header_end, body_len);
        } else {
            body_len = (total > resp_size) ? resp_size : total;
            memcpy(resp, raw, body_len);
        }
    }

    ret = body_len;

bin_cleanup:
    mbedtls_ssl_close_notify(&ssl);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&clicert);
    mbedtls_pk_free(&pkey);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (sock >= 0) {
        struct linger lg = { 1, 0 };
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        sceNetInetClose(sock);
    }
    free(raw);

    return ret;
}

/* External reference to pairing status flag from main.c */
extern volatile int g_is_paired;

/* Forward declaration: game selection grid (game_grid_ui.cpp) */
extern int game_grid_ui_run(const char *host_ip);
extern const char *game_grid_ui_get_selected_title(void);

/* Returns: 0=resume, 1=quit current and relaunch, -1=back/cancel */
static int prompt_existing_session_action(void)
{
    int selection = 0;

    while (1) {
        UIEvent evt = ui_process_input();
        UiButton resume_btn;
        UiButton quit_btn;

        switch (evt) {
        case UI_EVT_LEFT:
        case UI_EVT_UP:
            selection = 0;
            break;
        case UI_EVT_RIGHT:
        case UI_EVT_DOWN:
            selection = 1;
            break;
        case UI_EVT_SELECT:
        case UI_EVT_START:
            return selection;
        case UI_EVT_BACK:
            return -1;
        default:
            break;
        }

        memset(&resume_btn, 0, sizeof(resume_btn));
        memset(&quit_btn, 0, sizeof(quit_btn));

        resume_btn.x = 70;
        resume_btn.y = 156;
        resume_btn.w = 150;
        resume_btn.h = 34;
        strncpy(resume_btn.label, "Resume", sizeof(resume_btn.label) - 1);
        resume_btn.focused = (selection == 0);

        quit_btn.x = 260;
        quit_btn.y = 156;
        quit_btn.w = 150;
        quit_btn.h = 34;
        strncpy(quit_btn.label, "Quit + Relaunch", sizeof(quit_btn.label) - 1);
        quit_btn.focused = (selection == 1);

        ui_begin_frame();
        ui_draw_gradient_bg(UI_COL_BG_TOP, UI_COL_BG_BOT);
        ui_draw_header("Existing Session Detected");
        ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 108.0f, UI_COL_TEXT,
                              "Sunshine already has a running stream.");
        ui_draw_text_centered(0.0f, (float)UI_SCREEN_W, 128.0f, UI_COL_TEXT_DIM,
                              "Resume it, or stop it and start a new one.");
        ui_draw_button(&resume_btn);
        ui_draw_button(&quit_btn);
        ui_draw_footer_hint("{LF}/{RF}: Choose  {X}: Confirm  {O}: Back");
        ui_end_frame();
    }
}

static int sunshine_launch_session(int target_appid)
{
    unsigned char ri_key[16];
    char ri_key_hex[33];
    char path[1024];
    char resp[4096];
    char cancel_resp[512];
    char session_url[256];
    char currentgame_val[16];
    char state_val[64];
    int status_code;
    int current_game;
    int ret;
    int rikeyid;

    /* ------------------------------------------------------------------
     * Step A: Query /serverinfo to check if an app is already streaming.
     *
     * Sunshine's /launch ALWAYS returns gamesession=1 on success — it does
     * NOT indicate an existing session.  The correct way (matching real
     * Moonlight + vita-moonlight) is to check /serverinfo for:
     *   <currentgame>N</currentgame>   (N > 0 means an app is running)
     *   <state>SUNSHINE_SERVER_BUSY</state>
     * ------------------------------------------------------------------ */
    snprintf(path, sizeof(path),
             "/serverinfo?uniqueid=%s&uuid=%s",
             CLIENT_UNIQUE_ID, client_identity_get_uuid());
    pair_log("[SERVERINFO] GET https://%s:%d%s\n",
             g_sunshine_host, SUNSHINE_HTTPS_PORT, path);

    ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                           path, resp, sizeof(resp));
    if (ret < 0) {
        sceKernelDelayThread(2000 * 1000);
        ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                               path, resp, sizeof(resp));
        if (ret < 0) {
            pair_log("[SERVERINFO] request failed\n");
            return -1;
        }
    }

    pair_log("[SERVERINFO] response body: %.300s\n", resp);

    status_code = xml_get_status_code_attr(resp);
    if (status_code != 200) {
        pair_log("[SERVERINFO] unexpected status=%d\n", status_code);

        if (status_code == 401) {
            extern PspConfig g_psp_config;
            pair_log("[SERVERINFO] 401 — clearing stale pairing\n");
            g_is_paired = 0;
            g_last_paired_host[0] = '\0';
            /* Remove current target from paired list */
            { int _pi; for (_pi = 0; _pi < g_psp_config.pairedHostCount; _pi++) {
                if (strcmp(g_psp_config.pairedHostIps[_pi], g_sunshine_host) == 0) {
                    int _pj; for (_pj = _pi; _pj < g_psp_config.pairedHostCount - 1; _pj++)
                        memcpy(g_psp_config.pairedHostIps[_pj], g_psp_config.pairedHostIps[_pj+1], 16);
                    memset(g_psp_config.pairedHostIps[g_psp_config.pairedHostCount - 1], 0, 16);
                    g_psp_config.pairedHostCount--; break;
                }
            }}
            saveConfig(&g_psp_config);
            return -401;
        }
        return -1;
    }

    current_game = 0;
    if (xml_get_value_safe(resp, "currentgame", currentgame_val,
                           sizeof(currentgame_val)) >= 0) {
        current_game = atoi(currentgame_val);
    }
    /* Sunshine keeps currentgame set even after streaming ends; only treat
     * it as active if the server state says BUSY (matching vita-moonlight). */
    if (current_game > 0) {
        if (xml_get_value_safe(resp, "state", state_val,
                               sizeof(state_val)) >= 0) {
            if (strstr(state_val, "_SERVER_BUSY") == NULL) {
                current_game = 0;
            }
        }
    }
    pair_log("[SERVERINFO] currentgame=%d\n", current_game);

    /* ------------------------------------------------------------------
     * Step B: Generate rikey / rikeyid for this session.
     * ------------------------------------------------------------------ */
    ret = fill_random_bytes(ri_key, sizeof(ri_key));
    if (ret != 0) {
        pair_log("[LAUNCH] random rikey failed: -0x%04X\n", -ret);
        return -1;
    }

    memcpy(g_remote_input_key, ri_key, sizeof(ri_key));
    g_remote_input_key_valid = 1;

    bytes_to_hex_lite(ri_key, ri_key_hex, sizeof(ri_key));

    ret = fill_random_bytes((unsigned char *)&rikeyid, sizeof(rikeyid));
    if (ret != 0) {
        pair_log("[LAUNCH] random rikeyid failed: -0x%04X\n", -ret);
        return -1;
    }
    rikeyid &= 0x7FFFFFFF; /* Ensure positive for some server checks */
    if (rikeyid == 0) {
        rikeyid = 1;
    }
    g_av_ri_key_id = (unsigned int)rikeyid;

    pair_log("[LAUNCH] rikeyid=%d (0x%08X) g_av_ri_key_id=%u (0x%08X) &g_av_ri_key_id=%p\n",
             rikeyid, (unsigned int)rikeyid, g_av_ri_key_id, g_av_ri_key_id, (void *)&g_av_ri_key_id);

    /* ------------------------------------------------------------------
     * Step C: If an app is already running, resume or quit+relaunch.
     * Same app → auto-resume (instant relaunch, no popup).
     * Different app → show Resume / Quit popup.
     * ------------------------------------------------------------------ */
    if (current_game > 0) {
        pair_log("[LAUNCH] existing session detected (currentgame=%d, target=%d)\n",
                 current_game, target_appid);

        int user_choice;

        if (current_game == target_appid) {
            /* Same app — auto-resume without popup for instant relaunch.
             * Loading UI thread keeps running with the RTSP phase text. */
            pair_log("[LAUNCH] same app, auto-resuming (skipping popup)\n");
            user_choice = 0;
        } else {
            /* Different app — ask user whether to quit current + launch new.
             * Stop the stream_connect render thread first to avoid two threads
             * calling ui_begin_frame simultaneously (causes GU corruption). */
            stream_connect_stop();

            user_choice = prompt_existing_session_action();

            if (user_choice == -1) {
                /* User cancelled — back to host/game menu */
                pair_log("[LAUNCH] user cancelled session action\n");
                return -1;
            }

            stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_RTSP);
        }

        if (user_choice == 0) {
            /* Resume existing session — send /resume instead of /launch */
            pair_log("[RESUME] user chose resume\n");
            snprintf(path, sizeof(path),
                     "/resume?uniqueid=%s&uuid=%s&rikey=%s&rikeyid=%d"
                     "&surroundAudioInfo=196610",
                     CLIENT_UNIQUE_ID, client_identity_get_uuid(),
                     ri_key_hex, rikeyid);
            pair_log("[RESUME] GET https://%s:%d%s\n",
                     g_sunshine_host, SUNSHINE_HTTPS_PORT, path);

            ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                   path, resp, sizeof(resp));
            if (ret < 0) {
                pair_log("[RESUME] TLS failed, retrying after 3s\n");
                sceKernelDelayThread(3000 * 1000);
                ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                       path, resp, sizeof(resp));
            }
            if (ret >= 0) {
                status_code = xml_get_status_code_attr(resp);
                pair_log("[RESUME] status_code=%d\n", status_code);
                if (status_code != 200) {
                    pair_log("[RESUME] rejected (%d), falling back to cancel+relaunch\n",
                             status_code);
                    user_choice = 1; /* Fall through to cancel path */
                } else {
                    /* Resume succeeded — skip /launch, go to Step E */
                    current_game = -1; /* sentinel: skip Step D */
                }
            } else {
                pair_log("[RESUME] request failed, falling back to cancel+relaunch\n");
                user_choice = 1;
            }
        }

        if (user_choice == 1) {
            /* Quit current session and relaunch */
            snprintf(path, sizeof(path), "/cancel?uniqueid=%s&uuid=%s",
                     CLIENT_UNIQUE_ID, client_identity_get_uuid());
            pair_log("[CANCEL] GET https://%s:%d%s\n",
                     g_sunshine_host, SUNSHINE_HTTPS_PORT, path);

            ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                   path, cancel_resp, sizeof(cancel_resp));
            if (ret < 0) {
                pair_log("[CANCEL] first attempt failed, waiting 3s for TCP recovery\n");
                sceKernelDelayThread(3000 * 1000);
                ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                       path, cancel_resp, sizeof(cancel_resp));
            }
            if (ret >= 0) {
                status_code = xml_get_status_code_attr(cancel_resp);
                pair_log("[CANCEL] status_code=%d\n", status_code);

                if (status_code == 401) {
                    extern PspConfig g_psp_config;
                    pair_log("[CANCEL] 401 — clearing stale pairing\n");
                    g_is_paired = 0;
                    g_last_paired_host[0] = '\0';
                    { int _pi; for (_pi = 0; _pi < g_psp_config.pairedHostCount; _pi++) {
                        if (strcmp(g_psp_config.pairedHostIps[_pi], g_sunshine_host) == 0) {
                            int _pj; for (_pj = _pi; _pj < g_psp_config.pairedHostCount - 1; _pj++)
                                memcpy(g_psp_config.pairedHostIps[_pj], g_psp_config.pairedHostIps[_pj+1], 16);
                            memset(g_psp_config.pairedHostIps[g_psp_config.pairedHostCount - 1], 0, 16);
                            g_psp_config.pairedHostCount--; break;
                        }
                    }}
                    saveConfig(&g_psp_config);
                    return -401;
                }
            } else {
                pair_log("[CANCEL] request failed, proceeding anyway\n");
            }

            /* Wait for Sunshine to release sockets/encoder.
             * 6s is needed — 3s was too short and caused RTSP ANNOUNCE
             * ECONNRESET (errno 104) after cancel+relaunch. */
            sceKernelDelayThread(6000 * 1000);
            current_game = 0;
        }
    }

    /* ------------------------------------------------------------------
     * Step D: /launch (fresh start) — only if we did not /resume above.
     * ------------------------------------------------------------------ */
    if (current_game == 0) {
        /* Defensive: force width/height from resolution array.
         * Fixes ghost stale-config bug where g_psp_config.width/height
         * can diverge from the selected resolution preset. */
        {
            int ri = g_psp_config.resolutionIndex;
            if (ri < 0 || ri >= RESOLUTION_COUNT) ri = 0;
            int expected_w = RESOLUTION_WIDTHS[ri];
            int expected_h = RESOLUTION_HEIGHTS[ri];
            if (g_psp_config.width != expected_w || g_psp_config.height != expected_h) {
                pair_log("[LAUNCH] FIXUP: config w=%d h=%d != preset[%d] %dx%d, forcing preset\n",
                         g_psp_config.width, g_psp_config.height, ri, expected_w, expected_h);
                g_psp_config.width  = expected_w;
                g_psp_config.height = expected_h;
            }
            pair_log("[LAUNCH] resolution=%dx%d@%d (preset[%d])\n",
                     g_psp_config.width, g_psp_config.height, g_psp_config.fps, ri);
        }

        /* sops (Server Optimize Playback Settings):
         *   Xbox mode  → sops=1: Apollo optimizes display for gaming
         *                (resolution matching, refresh rate, game mode)
         *   Browser mode → sops=0: leave desktop display untouched
         *                (better for text/desktop content)
         *
         * videoCapabilities=2: bit 1 = support CAVLC / fast decode */
        {
            int sops_flag = (g_psp_config.controlMode == CONTROL_MODE_XBOX) ? 1 : 0;
            snprintf(path, sizeof(path),
                     "/launch?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d&sops=%d"
                     "&rikey=%s&rikeyid=%d&localAudioPlayMode=0&additionalStates=0"
                     "&surroundAudioInfo=196610&remoteControllersBitmap=1&gcmap=1"
                     "&corever=1&supportedVideoFormats=1&videoCapabilities=2"
                     "&videoEncoderSlicesPerFrame=1",
                     CLIENT_UNIQUE_ID, client_identity_get_uuid(),
                     target_appid, g_psp_config.width, g_psp_config.height, g_psp_config.fps,
                     sops_flag, ri_key_hex, rikeyid);
        }

        pair_log("[LAUNCH] GET https://%s:%d%s\n",
                 g_sunshine_host, SUNSHINE_HTTPS_PORT, path);

        /* Allow PSP network stack to fully tear down the serverinfo TLS
         * session before opening a new one.  Without this gap the
         * second handshake times out on 802.11b WiFi. */
        sceKernelDelayThread(2000 * 1000);

        ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                               path, resp, sizeof(resp));
        if (ret < 0) {
            sceKernelDelayThread(2000 * 1000);
            ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                   path, resp, sizeof(resp));
            if (ret < 0) {
                pair_log("[LAUNCH] request failed\n");
                return -1;
            }
        }

        pair_log("[LAUNCH] response body: %.200s\n", resp);

        status_code = xml_get_status_code_attr(resp);
        pair_log("[LAUNCH] status_code=%d\n", status_code);
        if (status_code != 200) {
            pair_log("[LAUNCH] rejected by server (status=%d)\n", status_code);

            if (status_code == 401) {
                extern PspConfig g_psp_config;
                pair_log("[LAUNCH] 401 — clearing stale pairing\n");
                g_is_paired = 0;
                g_last_paired_host[0] = '\0';
                { int _pi; for (_pi = 0; _pi < g_psp_config.pairedHostCount; _pi++) {
                    if (strcmp(g_psp_config.pairedHostIps[_pi], g_sunshine_host) == 0) {
                        int _pj; for (_pj = _pi; _pj < g_psp_config.pairedHostCount - 1; _pj++)
                            memcpy(g_psp_config.pairedHostIps[_pj], g_psp_config.pairedHostIps[_pj+1], 16);
                        memset(g_psp_config.pairedHostIps[g_psp_config.pairedHostCount - 1], 0, 16);
                        g_psp_config.pairedHostCount--; break;
                    }
                }}
                saveConfig(&g_psp_config);
                return -401;
            }

            if (status_code == 400) {
                /* 400 "An app is already running" — race between
                 * serverinfo check and another client launching. */
                pair_log("[LAUNCH] 400 — app may have just started, "
                         "try /resume instead\n");
            }

            return -1;
        }
    }

    /* ------------------------------------------------------------------
     * Step E: Extract sessionUrl0 from whichever response succeeded.
     * ------------------------------------------------------------------ */
    if (xml_get_value_safe(resp, "sessionUrl0", session_url, sizeof(session_url)) < 0) {
        pair_log("[LAUNCH] no sessionUrl0 in response\n");
        return -1;
    }

    /* Check if server requires Encrypted RTSP (Gen 7.1.431+) */
    if (strstr(session_url, "rtspenc://")) {
        extern unsigned char g_remote_input_key[16];
        static int gcm_init_done = 0;
        pair_log("[LAUNCH] server requested encrypted RTSP (AES-GCM)\n");
        g_rtsp_encrypted = 1;
        g_rtsp_enc_tx_seq = 0;
        if (gcm_init_done) mbedtls_gcm_free(&g_rtsp_gcm_ctx);
        mbedtls_gcm_init(&g_rtsp_gcm_ctx);
        mbedtls_gcm_setkey(&g_rtsp_gcm_ctx, MBEDTLS_CIPHER_ID_AES, g_remote_input_key, 128);
        gcm_init_done = 1;
    } else {
        g_rtsp_encrypted = 0;
    }

    rtsp_set_target_from_session_url(session_url);
    pair_log("[LAUNCH] sessionUrl0=%s\n", g_rtsp_target_url);
    return 0;
}

void network_set_target_host(const char *host_ip)
{
    if (!host_ip || !host_ip[0]) {
        return;
    }

    strncpy(g_sunshine_host, host_ip, sizeof(g_sunshine_host) - 1);
    g_sunshine_host[sizeof(g_sunshine_host) - 1] = '\0';

    /* Pairing state is valid only when selected host exactly matches
     * the persisted last-paired host. */
    if (strcmp(g_last_paired_host, g_sunshine_host) == 0) {
        g_is_paired = 1;
    } else {
        g_is_paired = 0;
    }
}

/*
 * network_restore_paired_host - Restore persisted pairing state on boot.
 *
 * Called once at startup with the IP that was saved in config.ini.
 * Sets g_last_paired_host so that selecting the same host later will not
 * trigger a new pairing round.
 */
void network_restore_paired_host(const char *paired_ip)
{
    if (!paired_ip || !paired_ip[0]) {
        return;
    }

    strncpy(g_last_paired_host, paired_ip, sizeof(g_last_paired_host) - 1);
    g_last_paired_host[sizeof(g_last_paired_host) - 1] = '\0';
    g_is_paired = 1;
}

/*
 * network_get_paired_host - Return the IP of the last successfully paired host.
 *
 * Returns pointer to static buffer; valid until next call to network_set_target_host.
 */
const char *network_get_paired_host(void)
{
    return g_last_paired_host;
}

/* ============================================================================
 * Step 1: Wi-Fi Connection
 * ============================================================================
 */

/*
 * wifi_connect - Connect PSP to saved access point slot 1
 *
 * Uses sceNetApctlConnect(1) to initiate connection to the first saved AP,
 * then polls sceNetApctlGetState() until state reaches 4 (connected) or
 * an error occurs.
 *
 * Returns: 0 on success, negative PSP error code on failure
 */
int wifi_connect(void)
{
    int ret;
    int state;
    int prev_state = -1;
    int attempts = 0;
    const int max_attempts = 300; /* 300 * 100ms = 30 second timeout */

    pspDebugScreenPrintf("wifi: connecting to access point slot 1...\n");

    /*--- Load network modules -----------------------------------------------*/
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
    if (ret < 0 && ret != (int)0x80110F01) return ret;
    ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
    if (ret < 0 && ret != (int)0x80110F01) return ret;

    /*--- Initialize network stack (align with netconf_ui) -------------------*/
    ret = sceNetInit(128 * 1024, 42, 4096, 42, 4096);
    if (ret < 0 && ret != (int)0x80410201)
    {
        pspDebugScreenPrintf("wifi: sceNetInit failed (0x%08X)\n", ret);
        return ret;
    }

    ret = sceNetInetInit();
    if (ret < 0 && ret != (int)0x80410701)
    {
        pspDebugScreenPrintf("wifi: sceNetInetInit failed (0x%08X)\n", ret);
        return ret;
    }

    ret = sceNetApctlInit(0x2000, 42);
    if (ret < 0 && ret != (int)0x80410B01)
    {
        pspDebugScreenPrintf("wifi: sceNetApctlInit failed (0x%08X)\n", ret);
        return ret;
    }

    /*--- Connect to access point slot 1 -------------------------------------*/
    ret = sceNetApctlConnect(1);
    if (ret < 0)
    {
        pspDebugScreenPrintf("wifi: sceNetApctlConnect(1) failed (0x%08X)\n", ret);
        sceNetApctlTerm();
        sceNetInetTerm();
        sceNetTerm();
        return ret;
    }

    /*--- Poll connection state until connected ------------------------------*/
    /* States:
     *   0 = idle
     *   1 = scanning for AP
     *   2 = connecting to AP
     *   3 = getting IP address (DHCP)
     *   4 = connected
     */
    while (attempts < max_attempts)
    {
        ret = sceNetApctlGetState(&state);
        if (ret < 0)
        {
            pspDebugScreenPrintf("wifi: sceNetApctlGetState failed (0x%08X)\n", ret);
            sceNetApctlDisconnect();
            sceNetApctlTerm();
            sceNetInetTerm();
            sceNetTerm();
            return ret;
        }

        /* Print state change */
        if (state != prev_state)
        {
            prev_state = state;
        }

        /* Check if connected */
        if (state == 4)
        {
            /* Get our IP address for display */
            union SceNetApctlInfo info;
            memset(&info, 0, sizeof(info));
            sceNetApctlGetInfo(8, &info); /* 8 = SCE_NET_APCTL_INFO_IP */
            pspDebugScreenPrintf("wifi: connected! IP: %s\n", info.ip);
            return 0;
        }

        /* Wait 100ms before next poll */
        sceKernelDelayThread(100 * 1000);
        attempts++;
    }

    /* Timeout */
    pspDebugScreenPrintf("wifi: connection timeout after 30 seconds\n");
    sceNetApctlDisconnect();
    sceNetApctlTerm();
    sceNetInetTerm();
    sceNetTerm();
    return -1;
}

/*
 * wifi_disconnect - Cleanly disconnect from Wi-Fi and tear down network stack
 */
void wifi_disconnect(void)
{
    sceNetApctlDisconnect();
    sceNetApctlTerm();
    sceNetInetTerm();
    sceNetTerm();
    pspDebugScreenPrintf("wifi: disconnected\n");
}

/* ============================================================================
 * Step 3: RTSP Session (OPTIONS, DESCRIBE, SETUP, PLAY)
 * ============================================================================
 */

/*
 * rtsp_send_and_recv - Send an RTSP request and receive the response
 *
 * Uses a TCP socket to send raw RTSP text and read back the response.
 * This is simpler than the PSP HTTP library for RTSP's custom protocol.
 *
 * sock:        connected TCP socket
 * request:     full RTSP request string to send
 * response:    buffer to receive response into
 * resp_size:   size of response buffer
 *
 * Returns: number of bytes received, or negative on error
 */
static int rtsp_send_and_recv(int sock, const char *request,
                               char *response, int resp_size)
{
    int ret, nb;
    int req_len;
    int sent;
    int total_recv = 0;
    int header_end_pos = -1;
    int content_length = -1;
    u32 start_ms;
    u32 body_recv_ms = 0;  /* When DESCRIBE body first received (for secondary timeout) */

    g_rtsp_last_resp_encrypted = 0;

    /* Send the request on blocking socket and handle partial writes. */
    nb = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    if (g_rtsp_encrypted) {
        int plaintext_len = (int)strlen(request);
        unsigned char encrypted_buf[RTSP_BUF_SIZE + 32];
        unsigned char iv[12] = {0};
        unsigned char tag[16];
        uint32_t type_len;

        g_rtsp_enc_tx_seq++;
        iv[0] = (unsigned char)(g_rtsp_enc_tx_seq);
        iv[1] = (unsigned char)(g_rtsp_enc_tx_seq >> 8);
        iv[2] = (unsigned char)(g_rtsp_enc_tx_seq >> 16);
        iv[3] = (unsigned char)(g_rtsp_enc_tx_seq >> 24);
        iv[10] = 'C';
        iv[11] = 'R';

        mbedtls_gcm_crypt_and_tag(&g_rtsp_gcm_ctx, MBEDTLS_GCM_ENCRYPT, plaintext_len,
                                  iv, 12, NULL, 0, (unsigned char*)request,
                                  encrypted_buf + 24, 16, tag);

        type_len = 0x80000000 | plaintext_len;
        encrypted_buf[0] = (unsigned char)(type_len >> 24);
        encrypted_buf[1] = (unsigned char)(type_len >> 16);
        encrypted_buf[2] = (unsigned char)(type_len >> 8);
        encrypted_buf[3] = (unsigned char)(type_len);
        encrypted_buf[4] = (unsigned char)(g_rtsp_enc_tx_seq >> 24);
        encrypted_buf[5] = (unsigned char)(g_rtsp_enc_tx_seq >> 16);
        encrypted_buf[6] = (unsigned char)(g_rtsp_enc_tx_seq >> 8);
        encrypted_buf[7] = (unsigned char)(g_rtsp_enc_tx_seq);
        memcpy(encrypted_buf + 8, tag, 16);

        /* Send encrypted frame in MSS-sized chunks (MTU safety,
         * matching moonlight-common-c sendMtuSafe). */
        {
            int total_to_send = plaintext_len + 24;
            int bytes_sent = 0;
            while (bytes_sent < total_to_send) {
                int chunk = total_to_send - bytes_sent;
                if (chunk > 536) chunk = 536;  /* TCPv4 MSS */
                ret = sceNetInetSend(sock, encrypted_buf + bytes_sent, chunk, 0);
                if (ret <= 0) {
                    pair_log("[RTSP] encrypted send failed at %d/%d (errno %d)\n",
                             bytes_sent, total_to_send, sceNetInetGetErrno());
                    return -1;
                }
                bytes_sent += ret;
            }
        }
    } else {
        req_len = (int)strlen(request);
        sent = 0;
        while (sent < req_len) {
            ret = sceNetInetSend(sock, request + sent, req_len - sent, 0);
            if (ret > 0) {
                sent += ret;
                continue;
            }
            if (ret < 0) {
                int err = sceNetInetGetErrno();
                if (err == EAGAIN || err == EWOULDBLOCK) {
                    sceKernelDelayThread(1000);
                    continue;
                }
                pair_log("[RTSP] send failed (errno %d)\n", err);
                return ret;
            }
            pair_log("[RTSP] send returned 0 before full request write\n");
            return -1;
        }
    }

    /* Removed debug screen printf — interferes with GU rendering */

    /* Receive the response (non-blocking with timeout). */
    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    memset(response, 0, resp_size);
    start_ms = sceKernelGetSystemTimeLow() / 1000;

    while (total_recv < resp_size - 1)
    {
        if ((sceKernelGetSystemTimeLow() / 1000) - start_ms > RTSP_RECV_TIMEOUT_MS) {
            break;
        }

        ret = sceNetInetRecv(sock, response + total_recv,
                             resp_size - 1 - total_recv, 0);
        if (ret > 0)
        {
            total_recv += ret;
            response[total_recv] = '\0';

            if (g_rtsp_encrypted && total_recv >= 24) {
                uint32_t type_len = ((unsigned char)response[0] << 24) | ((unsigned char)response[1] << 16) | 
                                    ((unsigned char)response[2] << 8) | (unsigned char)response[3];
                if (type_len & 0x80000000) {
                    int len = (int)(type_len & 0x7FFFFFFF);
                    if (total_recv >= len + 24) {
                        unsigned char dec_buf[RTSP_BUF_SIZE];
                        unsigned char iv[12] = {0};
                        uint32_t seq = ((unsigned char)response[4] << 24) | ((unsigned char)response[5] << 16) | 
                                       ((unsigned char)response[6] << 8) | (unsigned char)response[7];
                        iv[0] = (unsigned char)(seq);
                        iv[1] = (unsigned char)(seq >> 8);
                        iv[2] = (unsigned char)(seq >> 16);
                        iv[3] = (unsigned char)(seq >> 24);
                        iv[10] = 'H';
                        iv[11] = 'R';

                        if (mbedtls_gcm_auth_decrypt(&g_rtsp_gcm_ctx, (size_t)len,
                                                     iv, 12, NULL, 0,
                                                     (unsigned char*)response + 8, 16,
                                                     (unsigned char*)response + 24,
                                                     dec_buf) == 0) {
                            memcpy(response, dec_buf, (size_t)len);
                            total_recv = len;
                            response[total_recv] = '\0';
                            g_rtsp_last_resp_encrypted = 1;
                            break;
                        }
                    }
                    continue; /* Need more data */
                }
            }

            if (header_end_pos < 0) {
                char *header_end = strstr(response, "\r\n\r\n");
                if (header_end) {
                    header_end_pos = (int)(header_end - response) + 4;
                    {
                        const char *cl = strstr(response, "Content-Length:");
                        if (cl) {
                            cl += 15;
                            while (*cl == ' ') cl++;
                            content_length = atoi(cl);
                            if (content_length < 0) {
                                content_length = -1;
                            }
                        }
                    }
                }
            }

            if (header_end_pos >= 0) {
                if (content_length >= 0) {
                    if (total_recv >= header_end_pos + content_length) {
                        break;
                    }
                } else if (!strstr(request, "DESCRIBE ")) {
                    break;
                } else {
                    /* DESCRIBE with no Content-Length: the server may not
                     * close the TCP connection promptly (especially if it
                     * doesn't do encrypted RTSP).  Use a 2-second secondary
                     * timeout after receiving the first body data. */
                    if (body_recv_ms == 0) {
                        body_recv_ms = sceKernelGetSystemTimeLow() / 1000;
                    }
                    if ((sceKernelGetSystemTimeLow() / 1000) - body_recv_ms > 2000) {
                        pair_log("[RTSP] DESCRIBE body timeout (no Content-Length), accepting %d bytes\n",
                                 total_recv);
                        break;
                    }
                }
            }
        }
        else if (ret == 0)
        {
            /* Connection closed */
            break;
        }
        else
        {
            int err = sceNetInetGetErrno();
            if (err != EAGAIN && err != EWOULDBLOCK) {
                pair_log("[RTSP] recv failed (errno %d)\n", err);
                return ret;
            }
            sceKernelDelayThread(10000);
        }
    }

    response[total_recv] = '\0';

    /* Drain any remaining data (e.g. server FIN) to prevent RST on close.
     * moonlight-common-c reads until recv returns 0 (FIN); we break early
     * after decrypting the encrypted response.  Without draining, close()
     * on a socket with unread FIN can send RST, potentially causing the
     * server to reject the next TCP connection. */
    if (g_rtsp_last_resp_encrypted) {
        char drain_buf[64];
        int drain_ret;
        sceKernelDelayThread(10 * 1000);  /* 10ms for FIN to arrive */
        do {
            drain_ret = sceNetInetRecv(sock, drain_buf, sizeof(drain_buf), 0);
        } while (drain_ret > 0);
    }

    if (header_end_pos >= 0 && content_length >= 0) {
        if (total_recv < header_end_pos + content_length) {
            pair_log("[RTSP] incomplete response body (%d/%d)\n",
                     total_recv - header_end_pos, content_length);
            return -1;
        }
    }

    if (total_recv <= 0) {
        u32 elapsed = (sceKernelGetSystemTimeLow() / 1000) - start_ms;
        pair_log("[RTSP] empty response (elapsed=%dms, timeout=%d)\n",
                 (int)elapsed, RTSP_RECV_TIMEOUT_MS);
        return -1;
    }

    return total_recv;
}

static int rtsp_response_is_200(const char *response)
{
    if (!response) {
        return 0;
    }

    if (strncmp(response, "RTSP/1.0 200", 12) == 0) {
        return 1;
    }
    if (strncmp(response, "RTSP/1.1 200", 12) == 0) {
        return 1;
    }

    return 0;
}

static int rtsp_response_status_code(const char *response)
{
    int code = 0;

    if (!response) {
        return 0;
    }

    if (sscanf(response, "RTSP/%*d.%*d %d", &code) == 1) {
        return code;
    }

    return 0;
}

static void rtsp_parse_session_id(const char *response)
{
    const char *line;
    const char *end;
    int len;

    g_rtsp_session_id[0] = '\0';

    if (!response) {
        return;
    }

    line = strstr(response, "Session:");
    if (!line) {
        return;
    }

    line += 8;
    while (*line == ' ') {
        line++;
    }

    end = strchr(line, ';');
    if (!end) {
        end = strstr(line, "\r\n");
    }
    if (!end) {
        end = line + strlen(line);
    }

    len = (int)(end - line);
    if (len <= 0 || len >= (int)sizeof(g_rtsp_session_id)) {
        return;
    }

    memcpy(g_rtsp_session_id, line, len);
    g_rtsp_session_id[len] = '\0';
}

static int rtsp_connect_port(const char *host, int port)
{
    int sock, ret, nb;
    struct sockaddr_in server_addr;
    struct timeval timeout;
    fd_set writefds;
    int optval;
    socklen_t optlen;

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        pair_log("[RTSP] socket creation failed (errno %d)\n", sceNetInetGetErrno());
        return -1;
    }

    /* Set non-blocking for async connect */
    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_len    = (unsigned char)sizeof(server_addr);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = inet_addr(host);

    ret = sceNetInetConnect(sock, (struct sockaddr *)&server_addr,
                            sizeof(server_addr));

    if (ret == 0) {
        /* Connected immediately (unlikely for non-blocking) */
        goto connected;
    }

    {
        int err = sceNetInetGetErrno();
        if (err != EINPROGRESS && err != EWOULDBLOCK &&
            err != EALREADY && err != EAGAIN) {
            pair_log("[RTSP] connect %s:%d failed immediately (errno %d)\n",
                     host, port, err);
            sceNetInetClose(sock);
            return -1;
        }
    }

    /* Wait for connection using select() — proper async connect pattern
     * (matches power_handler.c and moonlight-common-c connectTcpSocket) */
    FD_ZERO(&writefds);
    FD_SET(sock, &writefds);
    timeout.tv_sec  = RTSP_CONNECT_TIMEOUT_MS / 1000;
    timeout.tv_usec = (RTSP_CONNECT_TIMEOUT_MS % 1000) * 1000;

    ret = sceNetInetSelect(sock + 1, NULL, &writefds, NULL, &timeout);
    if (ret <= 0) {
        pair_log("[RTSP] connect %s:%d timed out (select ret=%d, errno %d)\n",
                 host, port, ret, sceNetInetGetErrno());
        sceNetInetClose(sock);
        return -1;
    }

    /* Verify connection actually succeeded via SO_ERROR */
    optval = -1;
    optlen = sizeof(optval);
    sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
    if (optval != 0) {
        pair_log("[RTSP] connect %s:%d failed (SO_ERROR=%d)\n",
                 host, port, optval);
        sceNetInetClose(sock);
        return -1;
    }

connected:
    /* Restore blocking mode */
    nb = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    /* Disable Nagle — flush ANNOUNCE body immediately */
    nb = 1;
    sceNetInetSetsockopt(sock, IPPROTO_TCP, 1 /* TCP_NODELAY */,
                         &nb, sizeof(nb));

    pair_log("[RTSP] connected to %s:%d\n", host, port);
    return sock;
}

/*
 * rtsp_connect - Open TCP connection to RTSP server with retry
 *
 * Sunshine may not start listening on the RTSP port immediately after
 * /launch returns HTTP 200.  Retry with backoff (matching
 * moonlight-common-c transactRtspMessageTcp behaviour).
 *
 * Returns: socket file descriptor, or negative on error
 */
static int rtsp_connect(void)
{
    int sock;
    int i, attempt;
    int ports[2];
    int num_ports = 0;

    if (g_rtsp_port > 0) {
        ports[num_ports++] = g_rtsp_port;
    }
    if (g_rtsp_port != SUNSHINE_RTSP_PORT_PRIMARY) {
        ports[num_ports++] = SUNSHINE_RTSP_PORT_PRIMARY;
    }

    for (attempt = 0; attempt < RTSP_CONNECT_MAX_RETRIES; attempt++) {
        for (i = 0; i < num_ports; i++) {
            sock = rtsp_connect_port(g_rtsp_connect_host, ports[i]);
            if (sock >= 0) {
                g_rtsp_port = ports[i];
                rtsp_rewrite_target_authority(g_rtsp_connect_host, g_rtsp_port);
                return sock;
            }
        }
        if (attempt < RTSP_CONNECT_MAX_RETRIES - 1) {
            pair_log("[RTSP] connect attempt %d/%d failed, retrying in 1s...\n",
                     attempt + 1, RTSP_CONNECT_MAX_RETRIES);
            sceKernelDelayThread(1000 * 1000); /* 1 second */
        }
    }

    pair_log("[RTSP] all %d connect attempts exhausted\n", RTSP_CONNECT_MAX_RETRIES);
    return -1;
}

/*
 * rtsp_options - Send RTSP OPTIONS request
 *
 * Returns: 0 on success, negative on error
 */
static int rtsp_options(int sock)
{
    char request[512];
    char response[RTSP_BUF_SIZE];
    int ret;

    pair_log("[RTSP] OPTIONS %s\n", g_rtsp_target_url);

    snprintf(request, sizeof(request),
             "OPTIONS %s RTSP/1.0\r\n"
             "CSeq: %d\r\n"
             "X-GS-ClientVersion: %d\r\n"
             "Host: %s\r\n"
             "User-Agent: Moonlight-common-c/v4.2.0 (Sunshine; Desktop)\r\n"
             "\r\n",
             g_rtsp_target_url,
             rtsp_cseq++,
             CLIENT_VERSION,
             g_rtsp_host_header);

    ret = rtsp_send_and_recv(sock, request, response, sizeof(response));
    if (ret < 0)
        return ret;

    pair_log("[RTSP] OPTIONS response: %.120s\n", response);

    /* Check for 200 OK */
    if (!rtsp_response_is_200(response))
    {
        pair_log("[RTSP] OPTIONS failed (no 200)\n");
        return -1;
    }

    return 0;
}

/*
 * rtsp_describe - Send RTSP DESCRIBE request to get SDP
 *
 * sdp_buf:     buffer to receive SDP description
 * sdp_size:    size of SDP buffer
 *
 * Returns: 0 on success, negative on error
 */
static int rtsp_describe(int sock, char *sdp_buf, int sdp_size)
{
    char request[512];
    char response[RTSP_BUF_SIZE];
    int ret;
    char *sdp_start;

    pair_log("[RTSP] DESCRIBE %s\n", g_rtsp_target_url);

    snprintf(request, sizeof(request),
             "DESCRIBE %s RTSP/1.0\r\n"
             "CSeq: %d\r\n"
             "User-Agent: psp-moonlight\r\n"
             "X-GS-ClientVersion: %d\r\n"
             "Host: %s\r\n"
             "Accept: application/sdp\r\n"
             "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
             "\r\n",
             g_rtsp_target_url,
             rtsp_cseq++,
             CLIENT_VERSION,
             g_rtsp_host_header);

    ret = rtsp_send_and_recv(sock, request, response, sizeof(response));
    if (ret < 0)
        return ret;

    {
        int _p = 0, _len = (int)strlen(response);
        pair_log("[RTSP] DESCRIBE resp (%d bytes):\n", _len);
        while (_p < _len) {
            char _chunk[200]; int _n = _len - _p;
            if (_n > 195) _n = 195;
            memcpy(_chunk, response + _p, _n); _chunk[_n] = '\0';
            pair_log("%.200s", _chunk); _p += _n;
        }
    }

    /* Check for 200 OK */
    if (!rtsp_response_is_200(response))
    {
        pair_log("[RTSP] DESCRIBE failed (no 200)\n");
        return -1;
    }

    /* Extract SDP body (after blank line) */
    sdp_start = strstr(response, "\r\n\r\n");
    if (sdp_start == NULL)
    {
        pair_log("[RTSP] DESCRIBE missing SDP body\n");
        return -1;
    }
    sdp_start += 4; /* Skip past \r\n\r\n */

    /* Copy SDP to output buffer */
    strncpy(sdp_buf, sdp_start, sdp_size - 1);
    sdp_buf[sdp_size - 1] = '\0';

    {
        int _p = 0, _len = (int)strlen(sdp_buf);
        pair_log("[RTSP] SDP (%d bytes):\n", _len);
        while (_p < _len) {
            char _chunk[200]; int _n = _len - _p;
            if (_n > 195) _n = 195;
            memcpy(_chunk, sdp_buf + _p, _n); _chunk[_n] = '\0';
            pair_log("%.200s", _chunk); _p += _n;
        }
    }
    return 0;
}

/*
 * rtsp_setup_stream - Send RTSP SETUP for a single stream
 *
 * Matches moonlight-common-c: use relative stream IDs (not absolute URLs)
 * and Sunshine's non-standard transport header "unicast;X-GS-ClientPort=...".
 *
 * stream_id: e.g. "streamid=audio/0/0", "streamid=video/0/0",
 *            "streamid=control/13/0"
 *
 * Returns: 0 on success, negative on error
 */
static int rtsp_setup_stream(int sock, const char *stream_id)
{
    char request[512];
    char response[RTSP_BUF_SIZE];
    int ret;

    pair_log("[RTSP] SETUP %s\n", stream_id);

    /* Determine per-stream client port: must match the UDP socket we bind locally */
    int client_port_lo, client_port_hi;
    if (strstr(stream_id, "audio")) {
        unsigned short prepared_port = 0;
        if (audio_thread_reserve_client_port(&prepared_port) == 0 && prepared_port > 0) {
            client_port_lo = (int)prepared_port;
            client_port_hi = (int)prepared_port + 1;
            pair_log("[RTSP] audio client_port=%d (reserved socket)\n", client_port_lo);
        } else {
            pair_log("[RTSP] ERROR: audio pre-bind failed; cannot continue RTSP SETUP\n");
            return -1;
        }
    } else if (strstr(stream_id, "video")) {
        unsigned short prepared_port = 0;
        if (network_me_reserve_client_port(&prepared_port) == 0 && prepared_port > 0) {
            client_port_lo = (int)prepared_port;
            if (prepared_port < 65535) {
                client_port_hi = (int)prepared_port + 1;
            } else {
                client_port_hi = (int)prepared_port;
            }
            g_video_client_port = client_port_lo;
            pair_log("[RTSP] video client_port=%d (reserved socket)\n", client_port_lo);
        } else {
            pair_log("[RTSP] ERROR: video pre-bind failed; cannot continue RTSP SETUP\n");
            return -1;
        }
    } else {
        /* control */
        client_port_lo = 57999; /* Avoid server's 47999 on localhost */
        client_port_hi = 58000;
    }

    if (g_local_bind_ip[0] != '\0') {
        snprintf(request, sizeof(request),
                 "SETUP %s RTSP/1.0\r\n"
                 "CSeq: %d\r\n"
                 "User-Agent: psp-moonlight\r\n"
                 "X-GS-ClientVersion: %d\r\n"
                 "Host: %s\r\n"
                 "Session: %s\r\n"
                 "Transport: unicast;X-GS-ClientPort=%d-%d;destination=%s\r\n"
                 "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                 "\r\n",
                 stream_id, rtsp_cseq++, CLIENT_VERSION, g_rtsp_host_header, g_rtsp_session_id[0] ? g_rtsp_session_id : "",
                 client_port_lo, client_port_hi, g_local_bind_ip);
    } else {
        snprintf(request, sizeof(request),
                 "SETUP %s RTSP/1.0\r\n"
                 "CSeq: %d\r\n"
                 "User-Agent: psp-moonlight\r\n"
                 "X-GS-ClientVersion: %d\r\n"
                 "Host: %s\r\n"
                 "Session: %s\r\n"
                 "Transport: unicast;X-GS-ClientPort=%d-%d\r\n"
                 "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                 "\r\n",
                 stream_id, rtsp_cseq++, CLIENT_VERSION, g_rtsp_host_header, g_rtsp_session_id[0] ? g_rtsp_session_id : "",
                 client_port_lo, client_port_hi);
    }

    ret = rtsp_send_and_recv(sock, request, response, sizeof(response));
    if (ret < 0)
        return ret;

    pair_log("[RTSP] SETUP response: %.180s\n", response);

    if (!rtsp_response_is_200(response)) {
        pair_log("[RTSP] SETUP %s failed (no 200)\n", stream_id);
        return -1;
    }

    rtsp_parse_session_id(response);
    if (g_rtsp_session_id[0]) {
        pair_log("[RTSP] Session ID: %s\n", g_rtsp_session_id);
    }

    /* Copy server IP for use by ping/control threads */
    if (g_video_server_ip[0] == '\0') {
        /* Prefer the authority host from sessionUrl0/RTSP target so UDP/control
         * pings follow the same endpoint as successful RTSP handshake.
         * Fall back to the originally selected host when unavailable. */
        if (g_rtsp_connect_host[0] != '\0' &&
            strchr(g_rtsp_connect_host, ':') == NULL &&
            g_rtsp_connect_host[0] != '[') {
            strncpy(g_video_server_ip, g_rtsp_connect_host,
                    sizeof(g_video_server_ip) - 1);
        } else {
            strncpy(g_video_server_ip, g_sunshine_host,
                    sizeof(g_video_server_ip) - 1);
        }
        g_video_server_ip[sizeof(g_video_server_ip) - 1] = '\0';
    }

    /* Parse server_port from Transport header (all streams) */
    {
        char *sp = strstr(response, "server_port=");
        if (sp) {
            int parsed_port = (int)strtol(sp + 12, NULL, 10);
            if (parsed_port > 0 && parsed_port < 65536) {
                if (strstr(stream_id, "video")) {
                    g_video_server_port = parsed_port;
                    pair_log("[RTSP] video server_port=%d\n", parsed_port);
                } else if (strstr(stream_id, "audio")) {
                    g_audio_server_port = parsed_port;
                    pair_log("[RTSP] audio server_port=%d\n", parsed_port);
                } else {
                    g_control_server_port = parsed_port;
                    pair_log("[RTSP] control server_port=%d\n", parsed_port);
                }
            }
        }
    }

    /* Parse X-SS-Ping-Payload (video and audio streams) */
    if (strstr(stream_id, "video") || strstr(stream_id, "audio")) {
        char *pp = strstr(response, "X-SS-Ping-Payload:");
        char *dest = strstr(stream_id, "video") ? g_video_ping_payload : g_audio_ping_payload;
        memset(dest, 0, 17);
        if (pp) {
            int i;
            pp += 18;
            while (*pp == ' ' || *pp == '\t') pp++;
            for (i = 0; i < 16 && pp[i] && pp[i] != '\r' && pp[i] != '\n'; i++)
                dest[i] = pp[i];
            if (i == 16) {
                pair_log("[RTSP] %s ping payload=%.16s\n",
                         strstr(stream_id, "video") ? "video" : "audio", dest);
            } else {
                memset(dest, 0, 17);
                pair_log("[RTSP] WARN: %s ping payload missing/short, using legacy ping\n",
                         strstr(stream_id, "video") ? "video" : "audio");
            }
        }
    }

    /* Parse X-SS-Connect-Data (control stream only) */
    if (strstr(stream_id, "control")) {
        char *cd = strstr(response, "X-SS-Connect-Data:");
        if (cd) {
            cd += 18;
            while (*cd == ' ' || *cd == '\t') cd++;
            g_control_connect_data = (unsigned int)strtoul(cd, NULL, 10);
            pair_log("[RTSP] control connect_data=%u\n", g_control_connect_data);
        }
    }

    return 0;
}

/*
 * rtsp_announce - Send RTSP ANNOUNCE with SDP video configuration
 *
 * Tells Sunshine what resolution/FPS/bitrate/codec to stream.
 * Must be sent after all three SETUP requests.
 * Target is "streamid=control/13/0" (GFE version 7+ / Sunshine).
 *
 * Returns: 0 on success, negative on error
 */
static int rtsp_announce(int sock, int enc_enabled)
{
    /* SDP payload matching moonlight-common-c SdpGenerator.c format exactly.
     * Order: v, o, s, attributes, t, m
     * Attribute format: "a=name:value \r\n" (NO space after colon; trailing
     * space before \r\n matches the reference encoder). */
    char sdp_payload[4096];
    int  sdp_len;
    char request[4096];
    char response[RTSP_BUF_SIZE];
    int ret;

    pair_log("[RTSP] ANNOUNCE streamid=control/13/0 (encryptionEnabled=%d)\n",
             enc_enabled);

    {
        extern PspConfig g_psp_config;
        int bitrate_kbps = g_psp_config.bitrate > 0 ? g_psp_config.bitrate : 1600;
        int packet_size = g_psp_config.packetSize > 0 ? g_psp_config.packetSize : 1392;
        /* Resolution from preset array — authoritative source of truth. */
        int ri = g_psp_config.resolutionIndex;
        if (ri < 0 || ri >= RESOLUTION_COUNT) ri = 0;
        int stream_w = RESOLUTION_WIDTHS[ri];
        int stream_h = RESOLUTION_HEIGHTS[ri];
        int stream_fps = g_psp_config.fps > 0 ? g_psp_config.fps : 30;
        
        /* Moonlight-common-c rounds packet size to 16-byte chunks and subtracts
         * encrypted video header overhead when video encryption is enabled. */
        packet_size -= (packet_size % 16);
        if (packet_size <= 0) {
            packet_size = 1024;
        }
        /* Subtract ENC_VIDEO_HEADER (32 bytes) from packetSize only when
         * SS_ENC_VIDEO (0x02) is negotiated — matching moonlight-common-c
         * SdpGenerator.c: "if (EncryptionFeaturesEnabled & SS_ENC_VIDEO)".
         * With enc_enabled=1 (SS_ENC_CONTROL_V2 only), this block is skipped
         * and packetSize is sent at full value. */
        if (enc_enabled & 0x02) {
            if (packet_size > 32) {
                packet_size -= 32;
            } else {
                packet_size = 16;
            }
            packet_size -= (packet_size % 16);
            if (packet_size <= 0) {
                packet_size = 16;
            }
        }

        pair_log("[RTSP] negotiated bitrate=%d kbps, resolution=%dx%d@%d\n",
                 bitrate_kbps, stream_w, stream_h, stream_fps);
        pair_log("[RTSP] using packetSize=%d (enc_enabled=%d)\n",
                 packet_size, enc_enabled);

        {
            int announce_video_port = g_video_client_port > 0 ?
                                      g_video_client_port : MOONLIGHT_VIDEO_PORT;
            unsigned short announce_audio_port = 0;
            audio_thread_reserve_client_port(&announce_audio_port);



            /* Canonical SDP for PSP Baseline (Profile 66, Level 2.1, CAVLC).
             * We explicitly set h264Profile:66 and entropyCodingMode:0 to force
             * Sunshine/Apollo to avoid CABAC/HighProfile which PSP cannot decode.
             *
             * COMPREHENSIVE SDP — aligned with moonlight-common-c SdpGenerator.c
             * plus PSP-specific optimizations for 802.11b WiFi:
             *
             *  BUG FIXES from prior version:
             *   - clientRefreshRateX100 was hardcoded 0, now reads config (6000=60Hz)
             *   - minimumBitrateKbps was missing (server couldn't latch bitrate)
             *   - configuredBitrateKbps was missing (server couldn't adjust FEC)
             *
             *  NEW ATTRIBUTES (from reference moonlight-common-c):
             *   - timeoutLengthMs:7000         encoder timeout
             *   - framesWithInvalidRefThreshold:0  no tolerance for bad refs
             *   - fec.enable:1                 explicitly enable FEC
             *   - bllFec.enable:0              disable BLL-FEC (worse on lossy nets)
             *   - drc.enable:0                 disable dynamic resolution changes
             *   - enableRecoveryMode:0         recovery mode breaks FEC queue
             *   - videoQualityScoreUpdateTime:5000  quality scoring interval
             *   - minimumBitrateKbps           latch bitrate (no server scaling)
             *   - configuredBitrateKbps        original bitrate for FEC calc
             */
            /* Profile/entropy selection: Baseline+CAVLC by default,
             * Main+CABAC only in test mode (config cabacTestMode=1). */
            {
            int h264_profile = g_psp_config.cabacTestMode ? 77 : 66;
            int entropy_mode = g_psp_config.cabacTestMode ? 1 : 0;
            const char *profile_level_id = g_psp_config.cabacTestMode ? "4de015" : "42e015";

            if (g_psp_config.cabacTestMode)
                pair_log("[SDP] CABAC test mode: profile=%d entropy=%d\n", h264_profile, entropy_mode);

            memset(sdp_payload, 0, sizeof(sdp_payload));
            sdp_len = snprintf(sdp_payload, sizeof(sdp_payload),
                "v=0\r\n"
                "o=android 0 %d IN IP4 127.0.0.1\r\n"
                "s=NVIDIA Streaming Client\r\n"
                "t=0 0\r\n"
                /* --- Video resolution / FPS / packet size --- */
                "a=x-nv-video[0].clientViewportWd:%d\r\n"
                "a=x-nv-video[0].clientViewportHt:%d\r\n"
                "a=x-nv-video[0].maxFPS:%d\r\n"
                "a=x-nv-video[0].packetSize:%d\r\n"
                /* --- Audio: stereo, low quality (PSP can't decode surround) --- */
                "a=x-nv-audio.surround.numChannels:2\r\n"
                "a=x-nv-audio.surround.channelMask:3\r\n"
                "a=x-nv-audio.surround.AudioQuality:0\r\n"
                "a=x-nv-aqos.packetDuration:20\r\n"
                /* --- Transport: ENet reliable UDP, no qWAVE DSCP --- */
                "a=x-nv-general.useReliableUdp:1\r\n"
                "a=x-nv-aqos.qosTrafficType:0\r\n"
                "a=x-nv-vqos[0].qosTrafficType:0\r\n"
                /* --- FEC: critical for 802.11b packet loss --- */
                "a=x-nv-vqos[0].fec.enable:1\r\n"
                "a=x-nv-vqos[0].fec.minRequiredFecPackets:5\r\n"
                "a=x-nv-vqos[0].bllFec.enable:0\r\n"
                /* --- Feature flags --- */
                "a=x-ml-general.featureFlags:0\r\n"
                "a=x-nv-general.featureFlags:0\r\n"
                /* --- Encryption / codec / intra refresh --- */
                "a=x-ss-general.encryptionEnabled:%d\r\n"
                "a=x-ss-video[0].chromaSamplingType:0\r\n"
                "a=x-ss-video[0].intraRefresh:1\r\n"
                /* --- Encoder constraints: profile + entropy coding --- */
                "a=x-nv-video[0].videoEncoderSlicesPerFrame:1\r\n"
                "a=x-nv-vqos[0].bitStreamFormat:0\r\n"
                "a=x-nv-video[0].maxNumReferenceFrames:1\r\n"
                "a=x-nv-video[0].h264Profile:%d\r\n"
                "a=x-nv-video[0].entropyCodingMode:%d\r\n"
                /* --- Color: BT.601 full range, SDR --- */
                "a=x-nv-video[0].encoderCscMode:1\r\n"
                "a=x-nv-video[0].dynamicRangeMode:0\r\n"
                /* --- Display refresh: 60Hz PSP LCD (was hardcoded 0 = BUG) --- */
                "a=x-nv-video[0].clientRefreshRateX100:%d\r\n"
                /* --- Rate control: latch bitrate (no dynamic scaling) --- */
                "a=x-nv-video[0].rateControlMode:4\r\n"
                "a=x-nv-video[0].initialBitrateKbps:%d\r\n"
                "a=x-nv-video[0].initialPeakBitrateKbps:%d\r\n"
                "a=x-nv-vqos[0].bw.minimumBitrateKbps:%d\r\n"
                "a=x-nv-vqos[0].bw.maximumBitrateKbps:%d\r\n"
                /* --- Configured bitrate for Sunshine FEC calculation --- */
                "a=x-ml-video.configuredBitrateKbps:%d\r\n"
                /* --- Encoder timeouts and error tolerance --- */
                "a=x-nv-video[0].timeoutLengthMs:7000\r\n"
                "a=x-nv-video[0].framesWithInvalidRefThreshold:0\r\n"
                /* --- Disable dynamic resolution and recovery mode --- */
                "a=x-nv-vqos[0].drc.enable:0\r\n"
                "a=x-nv-general.enableRecoveryMode:0\r\n"
                /* --- Quality scoring interval (ms) --- */
                "a=x-nv-vqos[0].videoQualityScoreUpdateTime:5000\r\n"
                /* --- Media line --- */
                "m=video %d\r\n"
                "a=rtpmap:96 H264/90000\r\n"
                "a=fmtp:96 packetization-mode=1;profile-level-id=%s\r\n"
                /* --- Audio media line (tells Sunshine where to send audio) --- */
                "m=audio %d\r\n"
                "a=rtpmap:97 opus/48000/2\r\n",
                CLIENT_VERSION,
                stream_w, stream_h, stream_fps,
                packet_size,
                enc_enabled,
                h264_profile, entropy_mode,
                g_psp_config.clientRefreshRateX100 > 0 ? g_psp_config.clientRefreshRateX100 : 6000,
                bitrate_kbps, bitrate_kbps,
                bitrate_kbps, bitrate_kbps,
                bitrate_kbps,
                announce_video_port,
                profile_level_id,
                (int)announce_audio_port);
            }

            if (sdp_len >= (int)sizeof(sdp_payload))
                sdp_len = (int)sizeof(sdp_payload) - 1;
            /* qosTrafficType:0 tells Apollo this is a remote connection (no DSCP tagging).
             * If omitted, Apollo defaults to 5/4 (local) which enables qWAVE and
             * causes WSASendMsg 10022 on the server's UDP send socket. */

            /* Log the full SDP body we are sending */
            {
                int _p = 0, _len = sdp_len;
                pair_log("[RTSP] ANNOUNCE SDP (%d bytes):\n", _len);
                while (_p < _len) {
                    char _chunk[200]; int _n = _len - _p;
                    if (_n > 195) _n = 195;
                    memcpy(_chunk, sdp_payload + _p, _n); _chunk[_n] = '\0';
                    pair_log("%.200s", _chunk); _p += _n;
                }
            }
        }

        snprintf(request, sizeof(request),
                 "ANNOUNCE streamid=control/13/0 RTSP/1.0\r\n"
                 "CSeq: %d\r\n"
                 "X-GS-ClientVersion: %d\r\n"
                 "Host: %s\r\n"
                 "Session: %s\r\n"
                 "Content-type: application/sdp\r\n"
                 "Content-length: %d\r\n"
                 "\r\n"
                 "%s",
                 rtsp_cseq++,
                 CLIENT_VERSION,
                 g_rtsp_host_header,
                 g_rtsp_session_id,
                 sdp_len,
                 sdp_payload);

        ret = rtsp_send_and_recv(sock, request, response, sizeof(response));
        if (ret < 0)
            return -510; /* ANNOUNCE transport/no-response failure */

        pair_log("[RTSP] ANNOUNCE response: %.180s\n", response);

        if (rtsp_response_is_200(response)) {
            return 0;
        }

        if (rtsp_response_status_code(response) == 500) {
            return -500;
        }

        pair_log("[RTSP] ANNOUNCE failed (no 200)\n");
        return -1;
    }

    return -1;
}

/* Persistent socket used across the whole session */
static int g_rtsp_persistent_sock = -1;

void rtsp_session_close(void)
{
    if (g_rtsp_persistent_sock >= 0) {
        /* Optional: send TEARDOWN before closing if the server is still there. */
        sceNetInetClose(g_rtsp_persistent_sock);
        g_rtsp_persistent_sock = -1;
    }
}

/*
 * rtsp_play - Send RTSP PLAY to start the stream
 *
 * Uses "/" as the target (GFE 3.22+/Sunshine single-PLAY format).
 * Session header is always included (set by SETUP audio).
 *
 * Returns: 0 on success, negative on error
 */
static int rtsp_play(int sock)
{
    char request[512];
    char response[RTSP_BUF_SIZE];
    int ret;

    pair_log("[RTSP] PLAY /\n");

    snprintf(request, sizeof(request),
             "PLAY / RTSP/1.0\r\n"
             "CSeq: %d\r\n"
             "User-Agent: psp-moonlight\r\n"
             "X-GS-ClientVersion: %d\r\n"
             "Host: %s\r\n"
             "Session: %s\r\n"
             "\r\n",
             rtsp_cseq++,
             CLIENT_VERSION,
             g_rtsp_host_header,
             g_rtsp_session_id);

    ret = rtsp_send_and_recv(sock, request, response, sizeof(response));
    if (ret < 0)
        return ret;

    pair_log("[RTSP] PLAY response: %.180s\n", response);

    /* Check for 200 OK */
    if (!rtsp_response_is_200(response))
    {
        pair_log("[RTSP] PLAY failed (no 200)\n");
        return -1;
    }

    pair_log("[RTSP] PLAY succeeded, stream started\n");
    return 0;
}

/* RTSP stream identifiers extracted from SDP */
static char g_audio_stream_id[64]   = "streamid=audio/0/0";
static char g_video_stream_id[64]   = "streamid=video/0/0";
static char g_control_stream_id[64] = "streamid=control/13/0";

static void rtsp_parse_stream_ids(const char *sdp)
{
    const char *p = sdp;
    const char *m_video = NULL;
    const char *m_audio = NULL;
    
    /* Hunt for video and audio blocks in SDP */
    while ((p = strstr(p, "m=")) != NULL) {
        if (strncmp(p, "m=video", 7) == 0) m_video = p;
        if (strncmp(p, "m=audio", 7) == 0) m_audio = p;
        p += 2;
    }

    if (m_video) {
        const char *ctrl = strstr(m_video, "a=control:");
        if (ctrl) {
            ctrl += 10;
            const char *end = strstr(ctrl, "\r\n");
            if (end) {
                int len = (int)(end - ctrl);
                if (len > 0 && len < 63) {
                    memcpy(g_video_stream_id, ctrl, len);
                    g_video_stream_id[len] = '\0';
                }
            }
        }
    }

    if (m_audio) {
        const char *ctrl = strstr(m_audio, "a=control:");
        if (ctrl) {
            ctrl += 10;
            const char *end = strstr(ctrl, "\r\n");
            if (end) {
                int len = (int)(end - ctrl);
                if (len > 0 && len < 63) {
                    memcpy(g_audio_stream_id, ctrl, len);
                    g_audio_stream_id[len] = '\0';
                }
            }
        }
    }
    
    pair_log("[RTSP] Parsed Video ID: %s, Audio ID: %s\n", 
             g_video_stream_id, g_audio_stream_id);
}

int rtsp_session(void)
{
    int sock = -1;
    int ret;
    char sdp_buf[SDP_BUF_SIZE];

    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_RTSP);

    g_rtsp_session_id[0] = '\0';
    rtsp_cseq = 1;
    g_rtsp_enc_tx_seq = 0;  /* Reset encryption sequence for fresh RTSP attempt */
    g_video_client_port = 0;
    g_video_server_ip[0] = '\0';
    g_video_ping_payload[0] = '\0';
    g_audio_ping_payload[0] = '\0';

    /* ---------------------------------------------------------------
     * Per-command TCP connections (matching moonlight-common-c).
     * Sunshine/GFE closes the TCP connection after each RTSP response.
     * Reusing a socket causes the next recv() to get an immediate FIN
     * ("empty response").  Create a fresh connection for every command.
     * --------------------------------------------------------------- */

    /* 1. OPTIONS */
    sock = rtsp_connect();
    if (sock < 0) { pair_log("[RTSP] OPTIONS connect failed\n"); return sock; }
    ret = rtsp_options(sock);
    sceNetInetClose(sock); sock = -1;
    if (ret < 0) {
        pair_log("[RTSP] OPTIONS failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect();
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_options(sock);
        sceNetInetClose(sock); sock = -1;
        if (ret < 0) goto rtsp_fail;
    }

    /* Auto-detect: if server responded plaintext to our encrypted OPTIONS,
     * it likely does not implement encrypted RTSP.  Switch to plaintext
     * for all subsequent commands to avoid 12s DESCRIBE timeouts and
     * SETUP ECONNRESET (errno 104) from the server. */
    if (g_rtsp_encrypted && !g_rtsp_last_resp_encrypted) {
        pair_log("[RTSP] server responded plaintext — disabling RTSP encryption\n");
        g_rtsp_encrypted = 0;
    }

    sceKernelDelayThread(100 * 1000);

    /* 2. DESCRIBE */
    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_CONTROL);
    memset(sdp_buf, 0, sizeof(sdp_buf));
    sock = rtsp_connect();
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_describe(sock, sdp_buf, sizeof(sdp_buf));
    sceNetInetClose(sock); sock = -1;
    if (ret < 0) {
        pair_log("[RTSP] DESCRIBE failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect();
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_describe(sock, sdp_buf, sizeof(sdp_buf));
        sceNetInetClose(sock); sock = -1;
        if (ret < 0) goto rtsp_fail;
    }

    /* Parse stream IDs and encryptionSupported from DESCRIBE SDP */
    rtsp_parse_stream_ids(sdp_buf);
    g_encryption_supported = 0;
    {
        const char *enc_tag = strstr(sdp_buf, "encryptionSupported:");
        if (enc_tag) {
            enc_tag += strlen("encryptionSupported:");
            g_encryption_supported = atoi(enc_tag);
        }
        pair_log("[RTSP] encryptionSupported=%d\n", g_encryption_supported);
    }

    pair_log("[RTSP] DESCRIBE done, delaying 100ms before SETUP...\n");
    sceKernelDelayThread(100 * 1000);

    /* 3a. SETUP audio (non-fatal — video continues if audio SETUP fails) */
    g_audio_rtsp_ok = 0;
    pair_log("[RTSP] connecting for SETUP audio...\n");
    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_VIDEO);
    sock = rtsp_connect();
    if (sock < 0) {
        pair_log("[RTSP] WARN: audio connect failed, continuing without audio\n");
    } else {
        ret = rtsp_setup_stream(sock, g_audio_stream_id);
        sceNetInetClose(sock); sock = -1;
        if (ret < 0) {
            pair_log("[RTSP] SETUP audio failed, retrying...\n");
            sceKernelDelayThread(500 * 1000);
            sock = rtsp_connect();
            if (sock >= 0) {
                ret = rtsp_setup_stream(sock, g_audio_stream_id);
                sceNetInetClose(sock); sock = -1;
            }
        }
        if (ret >= 0) {
            g_audio_rtsp_ok = 1;
            ret = audio_thread_start_ping_only();
            if (ret < 0) pair_log("[RTSP] WARN: audio ping failed\n");
        } else {
            pair_log("[RTSP] WARN: audio SETUP failed after retry, continuing without audio\n");
        }
    }

    pair_log("[RTSP] SETUP audio done, delaying 50ms before SETUP video...\n");
    sceKernelDelayThread(50 * 1000);

    /* 3b. SETUP video */
    sock = rtsp_connect();
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_setup_stream(sock, g_video_stream_id);
    sceNetInetClose(sock); sock = -1;
    if (ret < 0) {
        pair_log("[RTSP] SETUP video failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect();
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_setup_stream(sock, g_video_stream_id);
        sceNetInetClose(sock); sock = -1;
        if (ret < 0) goto rtsp_fail;
    }

    sceKernelDelayThread(50 * 1000);

    /* 3c. SETUP control */
    sock = rtsp_connect();
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_setup_stream(sock, g_control_stream_id);
    sceNetInetClose(sock); sock = -1;
    if (ret < 0) {
        pair_log("[RTSP] SETUP control failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect();
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_setup_stream(sock, g_control_stream_id);
        sceNetInetClose(sock); sock = -1;
        if (ret < 0) goto rtsp_fail;
    }

    sceKernelDelayThread(100 * 1000);

    /* 4. ANNOUNCE */
    {
        /* Negotiate encryption: only enable video encryption (bit 0).
         * The PSP client does NOT implement SS_ENC_CONTROL_V2 (bit 2) —
         * requesting it causes the server to ECONNRESET on RTSP PLAY.
         * Track audio encryption separately for the audio recv loop.
         *
         * When disableEncryption=1, force enc_to_use=0 and audio_enc=0
         * to skip AES-GCM/CBC on all AV streams (~5% CPU savings on
         * 333MHz PSP).  Control stream encryption stays active. */
        int enc_to_use = (g_encryption_supported & 1) ? 1 : 0;  /* SS_ENC_VIDEO only */
        g_audio_encryption_enabled = (g_encryption_supported & 2) ? 1 : 0;
        if (g_psp_config.disableEncryption) {
            enc_to_use = 0;
            g_audio_encryption_enabled = 0;
            pair_log("[RTSP] AV encryption DISABLED by config (control enc stays active)\n");
        }
        pair_log("[RTSP] enc_to_use=%d (audio_enc=%d server_supported=0x%x disableConfig=%d)\n",
                 enc_to_use, g_audio_encryption_enabled,
                 g_encryption_supported, g_psp_config.disableEncryption);
        sock = rtsp_connect();
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_announce(sock, enc_to_use);
        sceNetInetClose(sock); sock = -1;
        if (ret < 0) {
            pair_log("[RTSP] ANNOUNCE failed, retrying...\n");
            sceKernelDelayThread(500 * 1000);
            sock = rtsp_connect();
            if (sock < 0) { ret = -1; goto rtsp_fail; }
            ret = rtsp_announce(sock, enc_to_use);
            sceNetInetClose(sock); sock = -1;
            if (ret < 0) goto rtsp_fail;
        }
    }

    sceKernelDelayThread(1000 * 1000);  /* 1s: let server settle after ANNOUNCE */

    /* Note: intraRefresh:1 is in SDP but only works for HEVC on Sunshine.
     * For H.264, IDR requests are the only way to get keyframes.
     * g_intra_refresh_active stays 0 (default) so IDR requests work. */

    /* 5. PLAY */
    sock = rtsp_connect();
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_play(sock);
    if (ret < 0) {
        sceNetInetClose(sock); sock = -1;
        pair_log("[RTSP] PLAY failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect();
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_play(sock);
        if (ret < 0) { sceNetInetClose(sock); sock = -1; goto rtsp_fail; }
    }
    /* Keep PLAY socket open for potential TEARDOWN later */
    g_rtsp_persistent_sock = sock;
    sock = -1;

    /* Prime video endpoint */
    if (network_me_send_video_ping_burst(g_video_server_ip,
                                         g_video_server_port,
                                         g_video_ping_payload) == 0) {
        pair_log("[RTSP] sent initial video ping burst to %s:%d\n",
                 g_video_server_ip, g_video_server_port);
    }

    /* Prime audio endpoint — mirrors video burst so Sunshine locks onto
     * the client audio port immediately after PLAY. */
    if (g_audio_rtsp_ok) {
        if (audio_thread_send_ping_burst() == 0) {
            pair_log("[RTSP] sent initial audio ping burst to %s:%d\n",
                     g_video_server_ip, g_audio_server_port);
        }
    }

    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_READY);
    sceKernelDelayThread(100 * 1000);

    pair_log("[RTSP] session established successfully\n");
    diag_log_flush();
    return 0;

rtsp_fail:
    if (sock >= 0) sceNetInetClose(sock);
    diag_log_flush();
    return ret;
}

/* ============================================================================
 * Public API: Full connection sequence
 * ============================================================================
 */

/*
 * generate_random_pin - Generate a random 4-digit PIN
 *
 * pin_buf: Buffer to store the PIN (must be at least 5 bytes)
 */
static void generate_random_pin(char *pin_buf)
{
    /* Use current time as seed for pseudo-random generation */
    u32 seed = sceKernelGetSystemTimeLow();
    
    /* Generate 4 random digits */
    for (int i = 0; i < PIN_DIGITS; i++) {
        /* Simple LCG (Linear Congruential Generator) */
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF;
        pin_buf[i] = '0' + (seed % 10);
    }
    pin_buf[PIN_DIGITS] = '\0';
}

/* ============================================================================
 * Pairing Thread - Runs Moonlight pairing protocol in background
 * ============================================================================ */

/* Shared state between pairing thread and main thread */
typedef struct {
    const char *host;
    const char *pin;
    volatile int *is_paired;
    volatile int  thread_done;
    volatile int  cancel;     /* set by main thread to request cancellation */
    volatile int  result;     /* 0 = success, negative = error */
} PairingThreadArgs;

/* Debug log helper for pairing thread (writes to ms0:/moonlight_debug.log) */
#include "diag_log.h"
#define pair_log(fmt, ...) diag_log_write("NET", fmt, ##__VA_ARGS__)

/*
 * http_pair_get - Plain HTTP/1.0 GET using raw PSP inet sockets.
 * Replaces the former sceHttp-based implementation so no SceHttp.prx
 * import stub appears in the final ELF, fixing LIBRARY_NOTFOUND
 * (0x8002013C) on real PSP hardware.
 *
 * Parses the full URL "http://host:port/path?query", connects via
 * sceNetInetSocket, and returns the response body in resp.
 *
 * Returns : 0 on success, -1 on failure.
 */
static int http_pair_get(const char *url, char *resp, int resp_size)
{
    int  sock = -1;
    int  ret, nb, connected = 0;
    struct sockaddr_in addr;
    int  total = 0;
    char raw[4096];
    char *body;
    char host[64];
    int  port = SUNSHINE_HTTP_PORT;
    const char *path = "/";

    /* ---- Parse "http://host:port/path" ---------------------------------- */
    {
        const char *p = url;
        if (strncmp(p, "http://", 7) == 0) p += 7;
        {
            const char *co = strchr(p, ':');
            const char *sl = strchr(p, '/');
            if (co && sl && co < sl) {
                int hlen = (int)(co - p);
                if (hlen >= (int)sizeof(host)) hlen = (int)sizeof(host) - 1;
                memcpy(host, p, hlen); host[hlen] = '\0';
                port = atoi(co + 1);
                path = sl;
            } else if (co && (!sl || co < sl)) {
                int hlen = (int)(co - p);
                if (hlen >= (int)sizeof(host)) hlen = (int)sizeof(host) - 1;
                memcpy(host, p, hlen); host[hlen] = '\0';
                port = atoi(co + 1);
            } else if (sl) {
                int hlen = (int)(sl - p);
                if (hlen >= (int)sizeof(host)) hlen = (int)sizeof(host) - 1;
                memcpy(host, p, hlen); host[hlen] = '\0';
                path = sl;
            } else {
                strncpy(host, p, sizeof(host) - 1);
                host[sizeof(host) - 1] = '\0';
            }
        }
    }

    /* ---- Raw TCP socket ------------------------------------------------- */
    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        pair_log("[HTTP] socket failed errno=%d\n", sceNetInetGetErrno());
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_len         = (unsigned char)sizeof(addr);
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((unsigned short)port);
    addr.sin_addr.s_addr = inet_addr(host);

    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    ret = sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) connected = 1;
    if (ret < 0) {
        int e = sceNetInetGetErrno();
        if (e != EINPROGRESS && e != EAGAIN && e != EWOULDBLOCK) {
            pair_log("[HTTP] connect failed errno=%d\n", e);
            sceNetInetClose(sock);
            return -1;
        }
    }

    if (!connected) {
        fd_set wfds;
        struct timeval tv;
        int optval;
        socklen_t optlen;

        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        tv.tv_sec  = 5;
        tv.tv_usec = 0;

        ret = sceNetInetSelect(sock + 1, NULL, &wfds, NULL, &tv);
        if (ret > 0) {
            optval = -1;
            optlen = sizeof(optval);
            sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
            if (optval == 0) {
                connected = 1;
            } else {
                pair_log("[HTTP] connect SO_ERROR=%d\n", optval);
            }
        }
    }

    if (!connected) {
        pair_log("[HTTP] connect timed out %s:%d\n", host, port);
        sceNetInetClose(sock);
        return -1;
    }

    nb = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    /* ---- Send request in three pieces (path can be several KB) ---------- */
    {
        const char *method   = "GET ";
        char        hdr[256];
        int         hdr_len, written, piece_len;

        written = 0; piece_len = 4; /* "GET " */
        while (written < piece_len) {
            ret = (int)sceNetInetSend(sock, method + written,
                                      piece_len - written, 0);
            if (ret <= 0) {
                pair_log("[HTTP] send failed\n");
                sceNetInetClose(sock); return -1;
            }
            written += ret;
        }

        written = 0; piece_len = (int)strlen(path);
        while (written < piece_len) {
            ret = (int)sceNetInetSend(sock, path + written,
                                      piece_len - written, 0);
            if (ret <= 0) {
                pair_log("[HTTP] send path failed\n");
                sceNetInetClose(sock); return -1;
            }
            written += ret;
        }

        hdr_len = snprintf(hdr, sizeof(hdr),
                           " HTTP/1.0\r\nHost: %s:%d\r\n"
                           "User-Agent: PSPMoonlight/1.0\r\n"
                           "Connection: close\r\n\r\n",
                           host, port);
        written = 0;
        while (written < hdr_len) {
            ret = (int)sceNetInetSend(sock, hdr + written,
                                      hdr_len - written, 0);
            if (ret <= 0) {
                pair_log("[HTTP] send hdr failed\n");
                sceNetInetClose(sock); return -1;
            }
            written += ret;
        }
    }

    /* ---- Read response -------------------------------------------------- */
    memset(raw, 0, sizeof(raw));
    while (total < (int)sizeof(raw) - 1) {
        ret = (int)sceNetInetRecv(sock, raw + total,
                                   sizeof(raw) - 1 - total, 0);
        if (ret <= 0) break;
        total += ret;
    }
    raw[total] = '\0';
    sceNetInetClose(sock);

    pair_log("[HTTP] %d bytes from %s:%d\n", total, host, port);

    /* Strip HTTP headers */
    body = strstr(raw, "\r\n\r\n");
    if (body) {
        body += 4;
        strncpy(resp, body, resp_size - 1);
    } else {
        strncpy(resp, raw, resp_size - 1);
    }
    resp[resp_size - 1] = '\0';

    pair_log("[HTTP] body %.120s\n", resp);
    return (total > 0) ? 0 : -1;
}

/*
 * fill_random_bytes - Fill output buffer with bytes from mbedTLS CTR-DRBG.
 *
 * This avoids using predictable LCG output for pairing salt/challenge/secret.
 */
static int fill_random_bytes(unsigned char *out, size_t len)
{
    static int seeded = 0;
    static mbedtls_ctr_drbg_context drbg;
    static mbedtls_entropy_context entropy;
    int ret;

    if (!seeded) {
        mbedtls_ctr_drbg_init(&drbg);
        mbedtls_entropy_init(&entropy);
        ret = mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                    (const unsigned char *)"psp-moonlight-pair", 18);
        if (ret != 0) {
            return ret;
        }
        seeded = 1;
    }

    return mbedtls_ctr_drbg_random(&drbg, out, len);
}

/*
 * xml_get_value_safe - Extract a tag value WITHOUT modifying the source buffer.
 *
 * Copies the content of <tag>...</tag> into 'out' (NUL-terminated).
 * Returns the number of bytes copied, or -1 if the tag is not found or
 * the value is larger than out_size-1.
 *
 * Use this instead of the old xml_get_value (which wrote a NUL into 'buf',
 * corrupting subsequent strstr calls on the same buffer).
 */
static int xml_get_value_safe(const char *buf, const char *tag,
                               char *out, int out_size)
{
    char open[64], close[64];
    const char *s, *e;
    int len;

    snprintf(open,  sizeof(open),  "<%s>",  tag);
    snprintf(close, sizeof(close), "</%s>", tag);

    s = strstr(buf, open);
    if (!s) return -1;
    s += strlen(open);
    e = strstr(s, close);
    if (!e) return -1;

    len = (int)(e - s);
    if (len < 0 || len >= out_size) return -1;

    memcpy(out, s, len);
    out[len] = '\0';
    return len;
}

/*
 * do_rsa_sign - RSA-PKCS#1v15-SHA256 sign 'client_secret' with the embedded key.
 *
 * Parses g_client_key_pem, SHA-256 hashes client_secret (16 bytes), signs
 * the hash, and writes the 256 byte signature into sig_out.
 *
 * Returns 0 on success, non-zero mbedTLS error code on failure.
 */
static int do_rsa_sign(const unsigned char *client_secret,
                       unsigned char *sig_out, size_t *sig_len_out)
{
    mbedtls_pk_context       pk;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;
    unsigned char cs_hash[32];
    int ret;

    mbedtls_pk_init(&pk);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)"psp-moonlight", 13);
    if (ret != 0) goto cleanup;

    ret = mbedtls_pk_parse_key(&pk,
                                (const unsigned char *)g_client_key_pem,
                                strlen(g_client_key_pem) + 1, /* include NUL */
                                NULL, 0);
    if (ret != 0) goto cleanup;

    mbedtls_sha256(client_secret, 16, cs_hash, 0); /* 0 = SHA-256 (not SHA-224) */

    *sig_len_out = 256;
    ret = mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256,
                           cs_hash, 32,
                           sig_out, sig_len_out,
                           mbedtls_ctr_drbg_random, &ctr_drbg);

cleanup:
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return ret;
}

/*
 * pairing_thread_func - Background thread that runs the Moonlight pairing
 *
 * Protocol steps (all plain HTTP GET, crypto via crypto_lite.h + mbedTLS):
 *   Step 1: Send salt + client cert hex  → get server cert
 *   Step 2: Send AES-ECB encrypted challenge → get challengeresponse
 *   Step 3: Send SHA-256 challenge response hash (uses cert signature bytes)
 *   Step 4: Send client_secret + RSA-PKCS#1v15-SHA256 signature
 */
static int pairing_thread_func(SceSize args, void *argp)
{
    PairingThreadArgs *ta = *(PairingThreadArgs **)argp;
    const char *host = ta->host;
    const char *pin  = ta->pin;
    char *resp = NULL;
    char *url  = NULL;
    int ret;

    /* Declare all locals at the top to avoid goto-over-declaration issues */
    unsigned char salt[16];
    char          salt_hex[33];
    unsigned char aes_key[16];
    unsigned char challenge[16];
    unsigned char challenge_enc[16];
    char          challenge_hex[33];
    char          cr_hex[256];
    size_t        cr_hex_len;
    size_t        cr_bin_len;
    unsigned char cr_enc[64];
    unsigned char cr_dec[64];
    unsigned char client_secret[16];
    unsigned char crdata[288];
    unsigned char resp_hash[32];
    unsigned char resp_enc[32];
    char          resp_hex[65];
    unsigned char sig[256];
    size_t        slen;
    unsigned char cps[272];
    char          cps_hex[545];
    char          paired_val[8] = "";
    unsigned char salt_pin[20];
    unsigned char aes_key_full[32];
    const char   *pair_uuid;

    /* Allocate large buffers from heap (thread stack is 64 KB) */
    resp = (char *)malloc(4096);
    url  = (char *)malloc(8192);
    if (!resp || !url) {
        pair_log("[PAIR] malloc failed\n");
        ta->result = -1;
        ta->thread_done = 1;
        if (resp) free(resp);
        if (url)  free(url);
        return 0;
    }

    pair_log("[PAIR] Thread started host=%s pin=****\n", host);
    pair_uuid = client_identity_get_uuid();

    /* ---------- Random salt + AES key derivation ---------- */
    ret = fill_random_bytes(salt, 16);
    if (ret != 0) {
        pair_log("[PAIR] random salt failed: -0x%04X\n", -ret);
        ta->result = -1;
        goto done;
    }
    bytes_to_hex_lite(salt, salt_hex, 16);

    /* AES-128 key = first 16 bytes of SHA-256(salt[16] || pin[4]) */
    memcpy(salt_pin,      salt, 16);
    memcpy(salt_pin + 16, pin,  4);
    sha256_hash(salt_pin, 20, aes_key_full);
    memcpy(aes_key, aes_key_full, 16);

    /* ===== Step 1: getservercert ===== */
    if (ta->cancel) goto done;
    pair_log("[PAIR] Step 1: getservercert\n");
    snprintf(url, 8192,
             "http://%s:%d/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1"
             "&phrase=getservercert&salt=%s&clientcert=%s",
             host, SUNSHINE_HTTP_PORT, CLIENT_UNIQUE_ID,
             pair_uuid, DEVICE_NAME, salt_hex, g_client_cert_hex);

    ret = http_pair_get(url, resp, 4096);
    if (ret < 0) {
        /* First request right after network init can return 0 bytes. */
        sceKernelDelayThread(150 * 1000);
        ret = http_pair_get(url, resp, 4096);
    }
    if (ret < 0) {
        pair_log("[PAIR] Step 1 HTTP failed\n");
        ta->result = -1;
        goto done;
    }
    ret = xml_get_value_safe(resp, "paired", paired_val, sizeof(paired_val));
    if (ret < 0 || strcmp(paired_val, "1") != 0) {
        pair_log("[PAIR] Step 1 rejected (paired=%s)\n",
                 (ret < 0) ? "<missing>" : paired_val);
        ta->result = -2;
        goto done;
    }
    pair_log("[PAIR] Step 1 OK\n");

    /* ===== Step 2: clientchallenge ===== */
    if (ta->cancel) goto done;
    ret = fill_random_bytes(challenge, 16);
    if (ret != 0) {
        pair_log("[PAIR] random challenge failed: -0x%04X\n", -ret);
        ta->result = -3;
        goto done;
    }
    aes128_ecb_encrypt(challenge, 16, aes_key, challenge_enc);
    bytes_to_hex_lite(challenge_enc, challenge_hex, 16);

    pair_log("[PAIR] Step 2: clientchallenge\n");
    snprintf(url, 8192,
             "http://%s:%d/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1"
             "&clientchallenge=%s",
             host, SUNSHINE_HTTP_PORT, CLIENT_UNIQUE_ID,
             pair_uuid, DEVICE_NAME, challenge_hex);

    ret = http_pair_get(url, resp, 4096);
    if (ret < 0) {
        pair_log("[PAIR] Step 2 HTTP failed\n");
        ta->result = -3;
        goto done;
    }
    /* Extract challengeresponse FIRST — before any xml_get_value_safe call
     * on "paired" could theoretically overwrite our copy (safe version
     * doesn't modify buf, so order doesn't strictly matter, but keep
     * data extraction first as a defensive pattern). */
    if (xml_get_value_safe(resp, "challengeresponse",
                            cr_hex, sizeof(cr_hex)) < 0) {
        pair_log("[PAIR] Step 2: no <challengeresponse> tag\n");
        ta->result = -4;
        goto done;
    }
    ret = xml_get_value_safe(resp, "paired", paired_val, sizeof(paired_val));
    if (ret < 0 || strcmp(paired_val, "1") != 0) {
        pair_log("[PAIR] Step 2 rejected (PIN mismatch? paired=%s)\n",
                 (ret < 0) ? "<missing>" : paired_val);
        ta->result = -4;
        goto done;
    }
    pair_log("[PAIR] Step 2 OK, cr_hex=%.32s...\n", cr_hex);

    /* ===== Step 3: serverchallengeresp ===== */
    if (ta->cancel) goto done;

    /* Generate random 16-byte client secret */
    ret = fill_random_bytes(client_secret, 16);
    if (ret != 0) {
        pair_log("[PAIR] random client_secret failed: -0x%04X\n", -ret);
        ta->result = -5;
        goto done;
    }

    /* Decrypt the challengeresponse:
     *   format: AES-ECB-encrypt(SHA256_hash[32] || server_challenge[16])
     *   = 48 bytes ciphertext (3 blocks of 16) */
    cr_hex_len = strlen(cr_hex);
    cr_bin_len = cr_hex_len / 2;
    if (cr_bin_len < 48 || cr_bin_len > sizeof(cr_enc) || (cr_bin_len % 16) != 0) {
        pair_log("[PAIR] Step 3: bad cr_bin_len=%u (hex_len=%u)\n",
                 (unsigned)cr_bin_len, (unsigned)cr_hex_len);
        ta->result = -5;
        goto done;
    }
    hex_to_bytes_lite(cr_hex, cr_enc, cr_hex_len);
    aes128_ecb_decrypt(cr_enc, (int)cr_bin_len, aes_key, cr_dec);

    /* server_challenge = cr_dec[32..47]  (after the 32-byte SHA-256 hash)
     * Build: challenge_resp_data = server_challenge(16) ||
     *                              client_cert_sig(256) ||
     *                              client_secret(16)     = 288 bytes */
    memcpy(crdata,        cr_dec + 32,        16); /* server_challenge */
    memcpy(crdata + 16,   g_client_cert_sig,  CLIENT_CERT_SIG_LEN); /* 256 */
    memcpy(crdata + 272,  client_secret,      16);

    sha256_hash(crdata, 288, resp_hash);
    aes128_ecb_encrypt(resp_hash, 32, aes_key, resp_enc);
    bytes_to_hex_lite(resp_enc, resp_hex, 32);

    pair_log("[PAIR] Step 3: serverchallengeresp\n");
    snprintf(url, 8192,
             "http://%s:%d/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1"
             "&serverchallengeresp=%s",
             host, SUNSHINE_HTTP_PORT, CLIENT_UNIQUE_ID,
             pair_uuid, DEVICE_NAME, resp_hex);

    ret = http_pair_get(url, resp, 4096);
    if (ret < 0) {
        pair_log("[PAIR] Step 3 HTTP failed\n");
        ta->result = -6;
        goto done;
    }
    ret = xml_get_value_safe(resp, "paired", paired_val, sizeof(paired_val));
    if (ret < 0 || strcmp(paired_val, "1") != 0) {
        pair_log("[PAIR] Step 3 rejected (paired=%s)\n",
                 (ret < 0) ? "<missing>" : paired_val);
        ta->result = -7;
        goto done;
    }
    pair_log("[PAIR] Step 3 OK\n");

    /* ===== Step 4: clientpairingsecret (RSA sign) ===== */
    if (ta->cancel) goto done;
    pair_log("[PAIR] Step 4: RSA sign client_secret\n");

    slen = 256;
    ret = do_rsa_sign(client_secret, sig, &slen);
    if (ret != 0) {
        char errbuf[80];
        mbedtls_strerror(ret, errbuf, sizeof(errbuf));
        pair_log("[PAIR] Step 4: RSA sign failed -0x%04X: %s\n", -ret, errbuf);
        ta->result = -8;
        goto done;
    }
    pair_log("[PAIR] Step 4: sig_len=%u, building clientpairingsecret\n",
             (unsigned)slen);

    /* clientpairingsecret = client_secret(16) || rsa_sig(256) = 272 bytes */
    memcpy(cps,      client_secret, 16);
    memcpy(cps + 16, sig,           256);
    bytes_to_hex_lite(cps, cps_hex, 272);

    snprintf(url, 8192,
             "http://%s:%d/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1"
             "&clientpairingsecret=%s",
             host, SUNSHINE_HTTP_PORT, CLIENT_UNIQUE_ID,
             pair_uuid, DEVICE_NAME, cps_hex);

    ret = http_pair_get(url, resp, 4096);
    if (ret < 0) {
        pair_log("[PAIR] Step 4 HTTP failed\n");
        ta->result = -9;
        goto done;
    }
    ret = xml_get_value_safe(resp, "paired", paired_val, sizeof(paired_val));
    if (ret < 0 || strcmp(paired_val, "1") != 0) {
        pair_log("[PAIR] Step 4 rejected (paired=%s)\n",
                 (ret < 0) ? "<missing>" : paired_val);
        ta->result = -10;
        goto done;
    }
    pair_log("[PAIR] Step 4 OK - pairing complete!\n");

    /* Pairing protocol succeeded */
    *(ta->is_paired) = 1;
    ta->result = 0;

done:
    free(resp);
    free(url);
    ta->thread_done = 1;
    return 0;
}

/*
 * network_connect_all - Execute the complete connection sequence
 *
 * 1. Assume Wi-Fi is already connected by netconf_ui_run()
 * 2. Show pairing PIN UI and run pairing protocol in background thread
 * 3. Start RTSP video session
 *
 * Returns: 0 on success, negative on failure at any step
 */
int network_connect_all(void)
{
    int ret;
    int need_pairing;
    char pairing_pin[PIN_DIGITS + 1];
    PairingPINUI pin_ui;
    PairingPINState pin_state;

    /* Brief delay to let the network stack settle after the native dialog */
    sceKernelDelayThread(100 * 1000);

    need_pairing = !g_is_paired;

    if (need_pairing) {
        /*--- Step 1: Show Pairing PIN UI + run pairing thread --------------*/
        generate_random_pin(pairing_pin);

        {
            PairingThreadArgs pair_args;
            PairingThreadArgs *arg_ptr = &pair_args;
            SceUID pair_tid;

            pair_args.host        = g_sunshine_host;
            pair_args.pin         = pairing_pin;
            pair_args.is_paired   = &g_is_paired;
            pair_args.thread_done = 0;
            pair_args.cancel      = 0;
            pair_args.result      = 0;

            pair_tid = sceKernelCreateThread("pairing_thread",
                                             pairing_thread_func,
                                             0x18,
                                             0x10000,
                                             PSP_THREAD_ATTR_USER,
                                             NULL);
            if (pair_tid < 0) {
                pair_log("[PAIR] create thread failed: 0x%08X\n", pair_tid);
                return -1;
            }

            ret = sceKernelStartThread(pair_tid, sizeof(arg_ptr), &arg_ptr);
            if (ret < 0) {
                pair_log("[PAIR] start thread failed: 0x%08X\n", ret);
                sceKernelDeleteThread(pair_tid);
                return -1;
            }

            ret = pairing_pin_ui_init(&pin_ui, pairing_pin,
                                      &g_is_paired, &pair_args.thread_done);
            if (ret < 0) {
                pair_args.cancel = 1;
                {
                    int wait_ms;
                    for (wait_ms = 0; wait_ms < 3000 && !pair_args.thread_done; wait_ms += 50) {
                        sceKernelDelayThread(50 * 1000);
                    }
                }
                if (!pair_args.thread_done) {
                    pair_log("[PAIR] thread still alive after init fail — force killing\n");
                    /* sceKernelWaitThreadEnd has no timeout; if the HTTP
                     * request is hanging we would freeze here indefinitely.
                     * Force-terminate instead (terminates + deletes). */
                    sceKernelTerminateDeleteThread(pair_tid);
                } else {
                    sceKernelDeleteThread(pair_tid);
                }
                return ret;
            }

            pin_state = pairing_pin_ui_run(&pin_ui);
            pairing_pin_ui_shutdown(&pin_ui);

            if (pin_state == PAIRING_PIN_STATE_CANCELLED) {
                pair_args.cancel = 1;
                {
                    int wait_ms;
                    for (wait_ms = 0; wait_ms < 2000 && !pair_args.thread_done; wait_ms += 50) {
                        sceKernelDelayThread(50 * 1000);
                    }
                }
                if (!pair_args.thread_done) {
                    pair_log("[PAIR] thread still pending after user cancel — force killing\n");
                    /* Replacing sceKernelWaitThreadEnd(NULL) which had no
                     * timeout.  If Sunshine's HTTP response never arrives,
                     * the pairing thread is stuck in http_pair_get() and
                     * WaitThreadEnd blocks forever, freezing the PSP.
                     * sceKernelTerminateDeleteThread terminates the thread
                     * immediately regardless of its blocked syscall state. */
                    sceKernelTerminateDeleteThread(pair_tid);
                } else {
                    sceKernelDeleteThread(pair_tid);
                }
                return -1;
            }

            if (!pair_args.thread_done) {
                sceKernelWaitThreadEnd(pair_tid, NULL);
            }
            sceKernelDeleteThread(pair_tid);

            if (pin_state != PAIRING_PIN_STATE_PAIRED) {
                return -1;
            }
        }

        strncpy(g_last_paired_host, g_sunshine_host, sizeof(g_last_paired_host) - 1);
        g_last_paired_host[sizeof(g_last_paired_host) - 1] = '\0';
    } else {
        pair_log("[PAIR] Reusing existing pairing for host %s\n", g_sunshine_host);
    }

    /* Brief delay to let network settle after pairing */
    sceKernelDelayThread(100 * 1000);

    /*--- Show Game Library so the user picks an app -------------------------*/
    {
        int selected_appid = game_grid_ui_run(g_sunshine_host);
        if (selected_appid == -3) {
            /* Empty game list → probable stale pairing. Clear cached state
             * so the next connect_to_sunshine() triggers a fresh PIN pair. */
            pair_log("[STEP 4] Game list empty — clearing stale pairing for %s\n",
                     g_sunshine_host);
            g_is_paired = 0;
            g_last_paired_host[0] = '\0';
            return -1;  /* -1 triggers full re-pair flow in main.c */
        }
        if (selected_appid < 0) {
            /* User pressed Circle (back) — return to host menu */
            pair_log("[STEP 4] User cancelled game selection\n");
            return -2;
        }
        pair_log("[STEP 4] User selected appid=%d\n", selected_appid);

        /* Start the 60fps connection UI render thread */
        stream_connect_start();
        stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_RTSP);

        /* Launch/resume stream session before RTSP handshake, as required by
         * Sunshine's /launch contract.  Retry once on failure — WiFi packet
         * loss can cause TLS handshake timeouts ~50% of the time. */
        int launch_ret = sunshine_launch_session(selected_appid);
        if (launch_ret != 0 && launch_ret != -401) {
            pair_log("[STEP 4] launch attempt 1 failed (%d), retrying after 2s...\n", launch_ret);
            sceKernelDelayThread(2000 * 1000);
            stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_RTSP);
            launch_ret = sunshine_launch_session(selected_appid);
        }
        if (launch_ret == -401) {
            /* 401: stale pairing already cleared; signal re-pair needed */
            pair_log("[STEP 4] 401 - stale pairing cleared, need re-pair\n");
            stream_connect_stop();
            return -1;  /* -1 triggers full re-pair flow in main.c */
        }
        if (launch_ret < 0) {
            pair_log("[STEP 4] launch step failed\n");
            stream_connect_stop();
            return -2;
        }
    }

    /*--- Step 2: RTSP Session -----------------------------------------------*/
    /* Give the server a moment to finish setting up the RTSP listener after
     * /launch.  500ms is enough — longer delays waste time on slow WiFi. */
    sceKernelDelayThread(500 * 1000);
    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_CONTROL);
    ret = rtsp_session();
    if (ret < 0)
    {
        /* RTSP failed — retry with increasing backoff.  WiFi packet loss
         * can cause RTSP TCP connections to time out, and after a
         * CANCEL+LAUNCH the server may need several seconds to reset
         * its RTSP listener. */
        pair_log("[RTSP] attempt 1 failed, retrying after 1s...\n");
        sceKernelDelayThread(1000 * 1000);
        stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_CONTROL);
        ret = rtsp_session();
    }
    if (ret < 0)
    {
        pair_log("[RTSP] attempt 2 failed, retrying after 2s...\n");
        sceKernelDelayThread(2000 * 1000);
        stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_CONTROL);
        ret = rtsp_session();
    }
    if (ret < 0)
    {
        /* RTSP failed — tell Sunshine to tear down the session we just
         * launched, otherwise subsequent /launch calls may conflict. */
        {
            char cancel_path[256];
            char cancel_resp[512];
            snprintf(cancel_path, sizeof(cancel_path),
                     "/cancel?uniqueid=%s", CLIENT_UNIQUE_ID);
            pair_log("[CANCEL] sending %s\n", cancel_path);
            https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                             cancel_path, cancel_resp, sizeof(cancel_resp));
            pair_log("[CANCEL] response: %.120s\n", cancel_resp);
        }
        /* Return -2 to distinguish RTSP failure from pairing failure (-1).
         * Pairing was already successful so g_is_paired should stay set. */
        stream_connect_stop();
        return -2;
    }

    stream_connect_stop();
    return 0;
}

/* ============================================================================
 * Public API: Explictly cancel pending or active streams on the host
 * ============================================================================
 */
static int cancel_thread_func(SceSize args, void *argp)
{
    (void)args;
    (void)argp;

    if (g_sunshine_host[0]) {
        char cancel_path[256];
        char cancel_resp[512];
        snprintf(cancel_path, sizeof(cancel_path),
                 "/cancel?uniqueid=%s", CLIENT_UNIQUE_ID);
        pair_log("[CANCEL-THREAD] Sending explicit abort via %s\n", cancel_path);
        https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                         cancel_path, cancel_resp, sizeof(cancel_resp));
        pair_log("[CANCEL-THREAD] explicit abort response: %.120s\n", cancel_resp);
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

static SceUID g_cancel_tid = -1;

void network_cancel_stream_session(void)
{
    if (g_cancel_tid >= 0) {
        /* thread already in progress, don't overlap */
        return;
    }
    
    g_cancel_tid = sceKernelCreateThread("cancel_th",
                                        cancel_thread_func,
                                        0x18,
                                        0x8000,
                                        0,
                                        NULL);
    if (g_cancel_tid >= 0) {
        sceKernelStartThread(g_cancel_tid, 0, NULL);
    }
}

void network_wait_for_cancel_thread(void)
{
    if (g_cancel_tid >= 0) {
        SceUInt timeout = 3000000; /* 3s */
        sceKernelWaitThreadEnd(g_cancel_tid, &timeout);
        sceKernelDeleteThread(g_cancel_tid);
        g_cancel_tid = -1;
    }
}
