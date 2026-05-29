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
#include "moonlight_ports.h"
#include "moonlight_proto.h"
#include "client_identity.h"
#include "diag_log.h"
#include "net_send.h"
#include "signal_strength.h"
#include "control_stream.h"
#define pair_log(fmt, ...) diag_log_write("NET", fmt, ##__VA_ARGS__)

#ifndef CLIENT_CERT_SIG_LEN
#define CLIENT_CERT_SIG_LEN 256
#endif

extern int network_me_reserve_client_port(unsigned short *out_port);
extern int network_me_send_video_ping_burst(const char *server_ip,
                                            int server_port,
                                            const char *ping_payload);
extern void network_me_shutdown(void);

/* GU owns VRAM during UI flows; route legacy debug prints to the log only. */
#define pspDebugScreenPrintf(...) pair_log(__VA_ARGS__)

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
#include "upnp_client.h"

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

#ifndef PSP_VIDEO_FEC_PERCENT
#define PSP_VIDEO_FEC_PERCENT 35
#endif

/* Build-time stream controls. Defaults keep the source-selected low-work
 * values; explicit overrides make each binary's SDP policy reproducible. */
#ifndef PSP_VIDEO_FEC_MIN_REQUIRED
#define PSP_VIDEO_FEC_MIN_REQUIRED 1
#endif

#ifndef PSP_AUDIO_PACKET_DURATION_MS
#define PSP_AUDIO_PACKET_DURATION_MS 60
#endif

#ifndef PSP_LOW_AUDIO_CONFIGURED_ADD_KBPS
/* Baseline 144p/20 PSP-1000 video-budget correction. 112 kbps and the
 * measured full audio+FEC addback both raised useful video bits but regressed
 * packet survival on hardware, so the source default remains the stable floor. */
#define PSP_LOW_AUDIO_CONFIGURED_ADD_KBPS 96
#endif

#ifndef PSP_VIDEO_INTRA_REFRESH
/* -1 = auto, 0 = force disabled, 1 = force enabled */
#define PSP_VIDEO_INTRA_REFRESH -1
#endif

#define DEVICE_NAME         "roth"             /* Device name for pairing    */

extern PspConfig g_psp_config;

/* Buffer sizes */
#define HTTP_BUF_SIZE       4096
#define RTSP_BUF_SIZE       4096
#define SDP_BUF_SIZE        2048
#define RTSP_CONNECT_TIMEOUT_MS          1500
/* Match moonlight-common-c's 10s RTSP launch-race window. Sunshine can return
 * /launch 200 before the RTSP listener is reachable on weak PSP WiFi. */
#define RTSP_POST_SETUP_CONNECT_TIMEOUT_MS 10000
#define RTSP_SEND_TIMEOUT_MS             3000
#define RTSP_RECV_TIMEOUT_MS             15000
#define RTSP_DRAIN_TIMEOUT_MS            250
#define RTSP_CONNECT_MAX_RETRIES         2
#define RTSP_POST_SETUP_CONNECT_MAX_RETRIES 2
#define RTSP_CONNECT_RETRY_DELAY_MS      500
#define TLS_PIN_DIR         MOONLIGHT_SAVE_TLS_PIN_DIR
#define HTTPS_CONNECT_TIMEOUT_US    (5 * 1000 * 1000)
#define HTTPS_HANDSHAKE_TIMEOUT_US  (8 * 1000 * 1000)
#define HTTPS_IO_TIMEOUT_US         (6 * 1000 * 1000)

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
 * Parsed from "a=x-ss-general.encryptionSupported:<N>".  These values are
 * common-c wire values and must not drift from Sunshine's RTSP contract. */
#define SS_ENC_CONTROL_V2 0x01
#define SS_ENC_VIDEO      0x02
#define SS_ENC_AUDIO      0x04

#define ML_FF_FEC_STATUS    0x01
#define ML_FF_SESSION_ID_V1 0x02

#define RTSP_ERR_PLAY_SESSION_DEAD (-610)

static int g_encryption_supported = 0;
static int g_encryption_requested = 0;

/* avRiKeyId — first 4 bytes of the AV decryption IV for audio packets.
 * Set during /launch or /resume from the locally generated rikeyid. */
unsigned int g_av_ri_key_id = 0;

/* Whether audio AES-CBC encryption was negotiated with the server.
 * Set to 1 only if both the server and client agree on SS_ENC_AUDIO.
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
static int s_retry_appid = -1;
static char s_retry_host[16] = "";
static char s_retry_title[64] = "";

static int g_rtsp_port = SUNSHINE_RTSP_PORT_PRIMARY;
static char g_rtsp_connect_host[64] = DEFAULT_SUNSHINE_HOST;
static char g_rtsp_host_header[96] = DEFAULT_SUNSHINE_HOST;

static int parse_ipv4_literal(const char *ip,
                              unsigned int *a,
                              unsigned int *b,
                              unsigned int *c,
                              unsigned int *d)
{
    char tail;

    if (!ip || !a || !b || !c || !d) {
        return -1;
    }

    if (sscanf(ip, "%u.%u.%u.%u%c", a, b, c, d, &tail) != 4) {
        return -1;
    }

    if (*a > 255 || *b > 255 || *c > 255 || *d > 255) {
        return -1;
    }

    return 0;
}

static int ipv4_is_private_or_unroutable(const char *ip)
{
    unsigned int a, b, c, d;

    if (parse_ipv4_literal(ip, &a, &b, &c, &d) < 0) {
        return 0;
    }

    if (a == 0 || a == 10 || a == 127 || (a == 169 && b == 254)) {
        return 1;
    }

    if (a == 192 && b == 168) {
        return 1;
    }

    if (a == 172 && b >= 16 && b <= 31) {
        return 1;
    }

    if (a == 100 && b >= 64 && b <= 127) {
        return 1;
    }

    if (a >= 224 || (a == 255 && b == 255 && c == 255 && d == 255)) {
        return 1;
    }

    return 0;
}

static int ipv4_is_public_literal(const char *ip)
{
    unsigned int a, b, c, d;

    if (parse_ipv4_literal(ip, &a, &b, &c, &d) < 0) {
        return 0;
    }

    return !ipv4_is_private_or_unroutable(ip);
}

static int rtsp_should_force_selected_host(const char *session_host)
{
    return ipv4_is_public_literal(g_sunshine_host) &&
           ipv4_is_private_or_unroutable(session_host);
}

void network_connect_clear_retry_app(void)
{
    s_retry_appid = -1;
    s_retry_host[0] = '\0';
    s_retry_title[0] = '\0';
}

static void rtsp_update_host_header(void)
{
    /* Sunshine/GFE select the Opus bitrate tier from the RTSP Host header:
     * a real local address requests high-quality audio, while 0.0.0.0
     * requests the normal low-audio tier. Keep the TCP target
     * in g_rtsp_connect_host, but advertise the low-audio Host value so audio
     * stays inside the PSP-1000 802.11b budget.
     *
     * This mirrors moonlight-common-c's low-bitrate workaround for streams
     * below the high-audio threshold. */
    strncpy(g_rtsp_host_header, "0.0.0.0", sizeof(g_rtsp_host_header) - 1);
    g_rtsp_host_header[sizeof(g_rtsp_host_header) - 1] = '\0';
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
static int g_launch_wlan_ps_saved = -1;

void wifi_launch_disable_power_save(void)
{
    int ps_val = -1;
    int ret = sceUtilityGetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
                                          &ps_val);
    if (ret == 0) {
        if (g_launch_wlan_ps_saved < 0) {
            g_launch_wlan_ps_saved = ps_val;
        }
        if (ps_val != PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF) {
            ret = sceUtilitySetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
                                              PSP_SYSTEMPARAM_WLAN_POWERSAVE_OFF);
            pair_log("[WIFIPS] disabled for UI/launch (was=%d ret=%d)\n",
                     ps_val, ret);
        } else {
            pair_log("[WIFIPS] already off for UI/launch\n");
        }
    } else {
        pair_log("[WIFIPS] query failed for UI/launch (%d)\n", ret);
    }
}

static void wifi_launch_restore_power_save(void)
{
    if (g_launch_wlan_ps_saved >= 0) {
#ifdef RETAIL_BUILD
        sceUtilitySetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
                                    g_launch_wlan_ps_saved);
#else
        int ret = sceUtilitySetSystemParamInt(PSP_SYSTEMPARAM_ID_INT_WLAN_POWERSAVE,
                                              g_launch_wlan_ps_saved);
        pair_log("[WIFIPS] restored UI/launch power save=%d ret=%d\n",
                 g_launch_wlan_ps_saved, ret);
#endif
        g_launch_wlan_ps_saved = -1;
    }
}

static void rtsp_rewrite_target_authority(const char *host, int port)
{
    const char *scheme_end;
    const char *path_start;
    char path_part[160];
    char scheme[16];
    int scheme_len;

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

    scheme_len = (int)(scheme_end - g_rtsp_target_url);
    if (scheme_len <= 0 || scheme_len >= (int)sizeof(scheme)) {
        strcpy(scheme, "rtsp");
    } else {
        memcpy(scheme, g_rtsp_target_url, scheme_len);
        scheme[scheme_len] = '\0';
    }

    path_start = strchr(scheme_end + 3, '/');
    if (path_start && path_start[0]) {
        strncpy(path_part, path_start, sizeof(path_part) - 1);
        path_part[sizeof(path_part) - 1] = '\0';
    } else {
        path_part[0] = '\0';
    }

    snprintf(g_rtsp_target_url, sizeof(g_rtsp_target_url),
             "%s://%s:%d%s", scheme, host, port, path_part);
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
        if (rtsp_should_force_selected_host(authority)) {
            pair_log("[RTSP] sessionUrl host %s is not reachable from selected remote host %s; using selected host\n",
                     authority, g_sunshine_host);
            strncpy(g_rtsp_connect_host, g_sunshine_host, sizeof(g_rtsp_connect_host) - 1);
            g_rtsp_connect_host[sizeof(g_rtsp_connect_host) - 1] = '\0';
            rtsp_rewrite_target_authority(g_rtsp_connect_host, g_rtsp_port);
            return;
        }

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
        static unsigned int s_bio_send_failures = 0;
        int err = sceNetInetGetErrno();
        if (err == EAGAIN || err == EWOULDBLOCK)
            return MBEDTLS_ERR_SSL_WANT_WRITE;

        s_bio_send_failures++;
        if (s_bio_send_failures <= 3 || (s_bio_send_failures % 16) == 0) {
            pair_log("[TLS BIO] send failed ret=%d len=%u errno=%d fails=%u\n",
                     ret, (unsigned)len, err, s_bio_send_failures);
        }
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

static void psp_tcp_close_bounded(int sock, int abortive)
{
    if (sock < 0) {
        return;
    }

    if (abortive) {
        struct linger lg = { 1, 0 };
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    }

    {
        int nb_close = 1;
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK,
                             &nb_close, sizeof(nb_close));
    }

    sceNetInetClose(sock);
}

static void sha256_hex_upper(const unsigned char *in32, char *out65)
{
    static const char hex[] = "0123456789ABCDEF";
    int i;
    for (i = 0; i < 32; i++) {
        out65[i * 2]     = hex[(in32[i] >> 4) & 0x0F];
        out65[i * 2 + 1] = hex[in32[i] & 0x0F];
    }
    out65[64] = '\0';
}

static void sanitize_host_for_filename(const char *host, char *out, int out_size)
{
    int i, j;
    if (!out || out_size <= 1) return;

    j = 0;
    for (i = 0; host && host[i] != '\0' && j < out_size - 1; i++) {
        char c = host[i];
        int ok = ((c >= '0' && c <= '9') ||
                  (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  c == '.' || c == '-' || c == '_');
        out[j++] = ok ? c : '_';
    }
    out[j] = '\0';

    if (j == 0) {
        strncpy(out, "unknown", out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static int load_tls_pin_file(const char *path, char *out, int out_size)
{
    SceUID fd;
    int n;
    char *nl;

    if (!path || !out || out_size <= 1) return -1;

    fd = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;

    n = sceIoRead(fd, out, out_size - 1);
    sceIoClose(fd);
    if (n <= 0) return -1;

    out[n] = '\0';
    nl = strchr(out, '\n');
    if (nl) *nl = '\0';
    return 0;
}

static int save_tls_pin_file(const char *path, const char *pin_hex)
{
    SceUID fd;
    int len;

    if (!path || !pin_hex) return -1;

    fd = sceIoOpen(path, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;

    len = (int)strlen(pin_hex);
    if (sceIoWrite(fd, pin_hex, len) != len ||
        sceIoWrite(fd, "\n", 1) != 1) {
        sceIoClose(fd);
        return -1;
    }

    sceIoClose(fd);
    return 0;
}

/* Sunshine uses self-signed certs in many installs. To preserve compatibility
 * while improving security, we use TOFU pinning: first cert is stored per host,
 * subsequent connections must match the same SHA-256 fingerprint. */
static int tls_verify_or_store_pin(const char *host, mbedtls_ssl_context *ssl)
{
    const mbedtls_x509_crt *peer;
    unsigned char digest[32];
    char pin_hex[65];
    char saved_pin[80];
    char safe_host[64];
    char pin_path[160];

    if (!host || !ssl) return -1;

    peer = mbedtls_ssl_get_peer_cert(ssl);
    if (!peer || !peer->raw.p || peer->raw.len == 0) {
        pair_log("[TLS-PIN] host=%s missing peer cert; skipping pin validation (compat mode)\n", host);
        return 0;
    }

    mbedtls_sha256(peer->raw.p, peer->raw.len, digest, 0);
    sha256_hex_upper(digest, pin_hex);

    moonlight_storage_ensure_tls_pin_dir();

    sanitize_host_for_filename(host, safe_host, sizeof(safe_host));
    snprintf(pin_path, sizeof(pin_path), "%s/%s.sha256", TLS_PIN_DIR, safe_host);

    if (load_tls_pin_file(pin_path, saved_pin, sizeof(saved_pin)) == 0) {
        if (strcmp(saved_pin, pin_hex) != 0) {
            pair_log("[TLS-PIN] host=%s pin mismatch (possible MITM or cert rotation)\n", host);
            return -1;
        }
        return 0;
    }

    if (save_tls_pin_file(pin_path, pin_hex) != 0) {
        pair_log("[TLS-PIN] host=%s failed to save first-use pin\n", host);
        return -1;
    }

    pair_log("[TLS-PIN] host=%s stored first-use cert pin\n", host);
    return 0;
}

static const char *get_active_client_cert_hex(void)
{
    const char *cert_hex = client_identity_get_cert_hex();
    if (!cert_hex || cert_hex[0] == '\0') {
        pair_log("[IDENTITY] runtime client certificate unavailable\n");
        return NULL;
    }
    return cert_hex;
}

static const char *get_active_client_key_pem(void)
{
    const char *key_pem = client_identity_get_key_pem();
    if (!key_pem || key_pem[0] == '\0') {
        pair_log("[IDENTITY] runtime client private key unavailable\n");
        return NULL;
    }
    return key_pem;
}

static int get_active_client_cert_sig(unsigned char *sig_out, size_t sig_out_size)
{
    const char *runtime_cert_hex = client_identity_get_cert_hex();
    mbedtls_x509_crt cert;
    unsigned char pem_buf[1536];
    size_t hex_len;
    size_t pem_size;
    int ret = -1;

    if (!sig_out || sig_out_size < CLIENT_CERT_SIG_LEN) {
        return -1;
    }

    if (!runtime_cert_hex || runtime_cert_hex[0] == '\0') {
        pair_log("[PAIR] runtime cert unavailable for signature extraction\n");
        return -1;
    }

    hex_len = strlen(runtime_cert_hex);
    if ((hex_len % 2) != 0) {
        return -1;
    }
    pem_size = (hex_len / 2) + 1;
    if (pem_size > sizeof(pem_buf)) {
        pair_log("[PAIR] runtime cert too large for parse buffer (%u bytes)\n",
                 (unsigned)pem_size);
        return -1;
    }

    hex_to_bytes_lite(runtime_cert_hex, pem_buf, hex_len);
    pem_buf[pem_size - 1] = '\0';

    mbedtls_x509_crt_init(&cert);
    ret = mbedtls_x509_crt_parse(&cert, pem_buf, pem_size);
    if (ret != 0) {
        goto cleanup;
    }

    if (cert.sig.len != CLIENT_CERT_SIG_LEN) {
        pair_log("[PAIR] unexpected cert signature length: %u\n", (unsigned)cert.sig.len);
        ret = -1;
        goto cleanup;
    }

    memcpy(sig_out, cert.sig.p, CLIENT_CERT_SIG_LEN);
    ret = 0;

cleanup:
    mbedtls_x509_crt_free(&cert);
    return ret;
}

/*
 * https_launch_get - Perform HTTPS GET with client certificate via mbedTLS.
 *
 * Sunshine's /launch endpoint runs on the HTTPS server (port 47984) and
 * requires mutual TLS: the client MUST present the runtime identity
 * certificate that was registered during pairing. The PSP sceHttp* API does
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
    int tls_ready = 0;
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
    const char *active_cert_hex = get_active_client_cert_hex();
    const char *active_key_pem = get_active_client_key_pem();

    if (!active_cert_hex || !active_key_pem) {
        pair_log("[LAUNCH-TLS] runtime client identity unavailable\n");
        return -1;
    }

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

    /* Parse active PEM certificate from runtime identity.
     * Certificate is provided as hex-encoded PEM string. */
    {
        size_t hex_len = strlen(active_cert_hex);
        size_t pem_size = (hex_len / 2) + 1;
        char pem_buf[1536]; /* Typical Moonlight client cert is ~1KB */
        if (pem_size > sizeof(pem_buf)) { ret = -1; goto tls_cleanup; }

        hex_to_bytes_lite(active_cert_hex, (unsigned char*)pem_buf, hex_len);
        pem_buf[pem_size - 1] = '\0';

        ret = mbedtls_x509_crt_parse(&clicert, (unsigned char*)pem_buf, pem_size);
        /* Removed free(pem_buf) */
        if (ret != 0) {
            pair_log("[LAUNCH-TLS] client cert parse failed: -0x%04X\n", -ret);
            goto tls_cleanup;
        }
    }

    /* Parse runtime PEM private key */
    ret = mbedtls_pk_parse_key(&pkey,
                                (const unsigned char *)active_key_pem,
                                strlen(active_key_pem) + 1,
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
        tv.tv_sec  = HTTPS_CONNECT_TIMEOUT_US / 1000000;
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
        pair_log("[LAUNCH-TLS] connect timed out (%ds)\n",
                 HTTPS_CONNECT_TIMEOUT_US / 1000000);
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

    /* Keep Sunshine compatibility for self-signed deployments while still
     * capturing native verification flags; TOFU pinning remains mandatory. */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
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
        if (sceKernelGetSystemTimeLow() - hs_t > HTTPS_HANDSHAKE_TIMEOUT_US) {
            pair_log("[LAUNCH-TLS] handshake timed out (%ds)\n",
                     HTTPS_HANDSHAKE_TIMEOUT_US / 1000000);
            goto tls_cleanup;
        }
        sceKernelDelayThread(5000); /* Reduced from 10ms for speed */
    }
    pair_log("[LAUNCH-TLS] TLS handshake OK\n");
    tls_ready = 1;

    {
        unsigned int verify_flags = (unsigned int)mbedtls_ssl_get_verify_result(&ssl);
        if (verify_flags != 0) {
            pair_log("[LAUNCH-TLS] peer verify flags=0x%08X (pin policy enforced)\n",
                     verify_flags);
        }
    }

    if (tls_verify_or_store_pin(host, &ssl) != 0) {
        pair_log("[LAUNCH-TLS] pin verification failed\n");
        goto tls_cleanup;
    }

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
            if (sceKernelGetSystemTimeLow() - wr_t > HTTPS_IO_TIMEOUT_US) {
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
            if (sceKernelGetSystemTimeLow() - rd_t > HTTPS_IO_TIMEOUT_US) {
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
    if (tls_ready) {
        u32 close_start_us = sceKernelGetSystemTimeLow();
        int close_ret;
        do {
            close_ret = mbedtls_ssl_close_notify(&ssl);
            if (close_ret == 0) {
                break;
            }
            if (close_ret != MBEDTLS_ERR_SSL_WANT_READ &&
                close_ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;
            }
            sceKernelDelayThread(5000);
        } while (sceKernelGetSystemTimeLow() - close_start_us < 500000);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&clicert);
    mbedtls_pk_free(&pkey);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (sock >= 0) {
        int abortive = (ret < 0 || !tls_ready);
        if (abortive) {
            pair_log("[LAUNCH-TLS] abortive socket close after failed HTTPS transaction\n");
        }
        psp_tcp_close_bounded(sock, abortive);
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
    int tls_ready = 0;
    struct sockaddr_in addr;

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config  conf;
    mbedtls_x509_crt    clicert;
    mbedtls_pk_context  pkey;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_context  entropy;

    char request[1536];
    char header_buf[4096];
    unsigned char io_buf[2048];
    int  header_len = 0;
    int  header_done = 0;
    int  body_written = 0;
    int  body_len = -1;
    const char *active_cert_hex = get_active_client_cert_hex();
    const char *active_key_pem = get_active_client_key_pem();

    if (!active_cert_hex || !active_key_pem) {
        return -1;
    }

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
        size_t hex_len = strlen(active_cert_hex);
        size_t pem_len = hex_len / 2;
        unsigned char pem_buf[1536];
        if ((pem_len + 1) > sizeof(pem_buf)) { ret = -1; goto bin_cleanup; }
        hex_to_bytes_lite(active_cert_hex, pem_buf, hex_len);
        pem_buf[pem_len] = '\0';
        ret = mbedtls_x509_crt_parse(&clicert, pem_buf, pem_len + 1);
        if (ret != 0) { goto bin_cleanup; }
    }

    ret = mbedtls_pk_parse_key(&pkey,
                                (const unsigned char *)active_key_pem,
                                strlen(active_key_pem) + 1, NULL, 0);
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

    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { goto bin_cleanup; }

    /* Keep Sunshine compatibility for self-signed deployments while still
     * capturing native verification flags; TOFU pinning remains mandatory. */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
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
    tls_ready = 1;

    {
        unsigned int verify_flags = (unsigned int)mbedtls_ssl_get_verify_result(&ssl);
        if (verify_flags != 0) {
            pair_log("[LAUNCH-TLS] binary peer verify flags=0x%08X (pin policy enforced)\n",
                     verify_flags);
        }
    }

    if (tls_verify_or_store_pin(host, &ssl) != 0) {
        goto bin_cleanup;
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

    /* Stream response and copy only body bytes into caller buffer. */
    memset(header_buf, 0, sizeof(header_buf));
    if (resp_size > 0) {
        memset(resp, 0, resp_size);
    }

    u32 rd_t = sceKernelGetSystemTimeLow();
    while (1) {
        ret = mbedtls_ssl_read(&ssl, io_buf, sizeof(io_buf));
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (sceKernelGetSystemTimeLow() - rd_t > 5000000) break;
            sceKernelDelayThread(10000);
            continue;
        }
        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || ret == 0) break;
        if (ret < 0) {
            pair_log("[LAUNCH-TLS] binary read failed: -0x%04X\n", -ret);
            break;
        }

        if (!header_done) {
            int i;
            int search_start;

            if (header_len + ret > (int)sizeof(header_buf) - 1) {
                pair_log("[LAUNCH-TLS] binary response header too large (%d bytes)\n",
                         header_len + ret);
                ret = -1;
                goto bin_cleanup;
            }

            memcpy(header_buf + header_len, io_buf, ret);
            search_start = (header_len >= 3) ? (header_len - 3) : 0;
            header_len += ret;
            header_buf[header_len] = '\0';

            for (i = search_start; i < header_len - 3; i++) {
                if (header_buf[i] == '\r' && header_buf[i + 1] == '\n' &&
                    header_buf[i + 2] == '\r' && header_buf[i + 3] == '\n') {
                    int body_off = i + 4;
                    int available = header_len - body_off;
                    int copy_len = resp_size - body_written;
                    header_done = 1;

                    if (copy_len > available) copy_len = available;
                    if (copy_len > 0) {
                        memcpy(resp + body_written, header_buf + body_off, copy_len);
                        body_written += copy_len;
                    }
                    break;
                }
            }
        } else {
            int copy_len = resp_size - body_written;
            if (copy_len > ret) copy_len = ret;
            if (copy_len > 0) {
                memcpy(resp + body_written, io_buf, copy_len);
                body_written += copy_len;
            }
        }

        if (header_done && body_written >= resp_size) {
            break;
        }

        rd_t = sceKernelGetSystemTimeLow();
    }

    /* Fallback: no HTTP header delimiter found, keep captured bytes as-is. */
    if (!header_done) {
        body_len = (header_len > resp_size) ? resp_size : header_len;
        if (body_len > 0) {
            memcpy(resp, header_buf, body_len);
        }
    } else {
        body_len = body_written;
    }

    ret = body_len;

bin_cleanup:
    if (tls_ready) {
        u32 close_start_us = sceKernelGetSystemTimeLow();
        int close_ret;
        do {
            close_ret = mbedtls_ssl_close_notify(&ssl);
            if (close_ret == 0) {
                break;
            }
            if (close_ret != MBEDTLS_ERR_SSL_WANT_READ &&
                close_ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                break;
            }
            sceKernelDelayThread(5000);
        } while (sceKernelGetSystemTimeLow() - close_start_us < 500000);
    }
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&clicert);
    mbedtls_pk_free(&pkey);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (sock >= 0) {
        psp_tcp_close_bounded(sock, (ret < 0 || !tls_ready));
    }

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
    /* Prefer a clean relaunch by default.
     * Resume frequently starts mid-GOP without SPS/PPS on some hosts, which
     * can trap weak clients in prolonged no-output startup loops. */
    int selection = 1;

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
    char serverinfo_url[256];
    int serverinfo_unknown = 0;
    int status_code;
    int current_game;
    int ret;
    int rikeyid;
    int serverinfo_attempt;
    int serverinfo_https_attempt;

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
    serverinfo_unknown = 1;
    status_code = 0;

    /* App list, launch, resume, and cancel all use authenticated HTTPS on
     * Sunshine/Apollo. Probe serverinfo there first so a dead plain HTTP
     * port does not strand the PSP at "Requesting Stream...". */
    for (serverinfo_https_attempt = 1;
         serverinfo_https_attempt <= 2;
         serverinfo_https_attempt++) {
        resp[0] = '\0';
        pair_log("[SERVERINFO] GET https://%s:%d%s (attempt %d/2)\n",
                 g_sunshine_host, SUNSHINE_HTTPS_PORT, path,
                 serverinfo_https_attempt);
        ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                               path, resp, sizeof(resp));
        if (ret >= 0) {
            status_code = xml_get_status_code_attr(resp);
            if (status_code == 200) {
                serverinfo_unknown = 0;
                break;
            }
            pair_log("[SERVERINFO] HTTPS status=%d on attempt %d/2\n",
                     status_code, serverinfo_https_attempt);
        } else {
            pair_log("[SERVERINFO] HTTPS request failed on attempt %d/2\n",
                     serverinfo_https_attempt);
        }

        if (serverinfo_https_attempt < 2) {
            sceKernelDelayThread(750 * 1000);
        }
    }

    if (serverinfo_unknown) {
        snprintf(serverinfo_url, sizeof(serverinfo_url),
                 "http://%s:%d%s", g_sunshine_host, SUNSHINE_HTTP_PORT, path);

        for (serverinfo_attempt = 1;
             serverinfo_attempt <= 2;
             serverinfo_attempt++) {
            resp[0] = '\0';
            pair_log("[SERVERINFO] GET %s (fallback attempt %d/2)\n",
                     serverinfo_url, serverinfo_attempt);
            ret = http_pair_get(serverinfo_url, resp, sizeof(resp));
            if (ret >= 0) {
                status_code = xml_get_status_code_attr(resp);
                if (status_code == 200) {
                    serverinfo_unknown = 0;
                    break;
                }
                pair_log("[SERVERINFO] plain status=%d on attempt %d/2\n",
                         status_code, serverinfo_attempt);
            } else {
                pair_log("[SERVERINFO] plain request failed on attempt %d/2\n",
                         serverinfo_attempt);
            }

            if (serverinfo_attempt < 2) {
                sceKernelDelayThread(750 * 1000);
            }
        }
    }

    pair_log("[SERVERINFO] response body: %.300s\n", resp);

    current_game = 0;
    if (serverinfo_unknown) {
        pair_log("[SERVERINFO] currentgame unknown after retries; deferring /launch\n");
        return -1;
    } else {
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
    }

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
     * Step C: If an app is already running, ask the user whether to
     * resume it or quit+relaunch. We always show the popup (including
     * same-app relaunch) so session handling stays explicit.
     * ------------------------------------------------------------------ */
    if (current_game > 0) {
        pair_log("[LAUNCH] existing session detected (currentgame=%d, target=%d)\n",
                 current_game, target_appid);

        int user_choice;

        /* Ask user whether to quit current + launch new.
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

        if (user_choice == 0) {
            /* Resume existing session — send /resume instead of /launch */
            int surround_audio_info = AUDIO_STREAM_SURROUND_INFO;
            pair_log("[RESUME] user chose resume\n");
            snprintf(path, sizeof(path),
                     "/resume?uniqueid=%s&uuid=%s&rikey=%s&rikeyid=%d"
                     "&surroundAudioInfo=%d&continuousAudio=0",
                     CLIENT_UNIQUE_ID, client_identity_get_uuid(),
                     ri_key_hex, rikeyid, surround_audio_info);
            pair_log("[RESUME] audioEnabled=%d surroundAudioInfo=%d continuousAudio=0%s\n",
                     g_psp_config.audioEnabled, surround_audio_info,
                     g_psp_config.audioEnabled ? "" : " (client drain/drop only)");
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

            cancel_resp[0] = '\0';
            ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                   path, cancel_resp, sizeof(cancel_resp));
            if (ret < 0) {
                pair_log("[CANCEL] first attempt failed, waiting 3s for TCP recovery\n");
                sceKernelDelayThread(3000 * 1000);
                cancel_resp[0] = '\0';
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
        /* Defensive: keep width/height and resolutionIndex consistent.
         * If explicit width/height match a known resolution, sync the index to
         * that resolution. Only force width/height from the resolution row
         * when the current dimensions are invalid/unmapped. */
        {
            int ri = g_psp_config.resolutionIndex;
            int matched_ri = -1;
            int i;
            if (ri < 0 || ri >= RESOLUTION_COUNT) ri = 0;

            for (i = 0; i < RESOLUTION_COUNT; i++) {
                if (g_psp_config.width == RESOLUTION_WIDTHS[i] &&
                    g_psp_config.height == RESOLUTION_HEIGHTS[i]) {
                    matched_ri = i;
                    break;
                }
            }

            if (matched_ri >= 0) {
                if (ri != matched_ri) {
                    pair_log("[LAUNCH] FIXUP: syncing preset index %d -> %d for config %dx%d\n",
                             ri, matched_ri, g_psp_config.width, g_psp_config.height);
                    ri = matched_ri;
                    g_psp_config.resolutionIndex = matched_ri;
                }
            } else {
                int expected_w = RESOLUTION_WIDTHS[ri];
                int expected_h = RESOLUTION_HEIGHTS[ri];
                pair_log("[LAUNCH] FIXUP: unmapped config %dx%d, forcing preset[%d] %dx%d\n",
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
         * videoCapabilities=2: H.264 reference-frame invalidation capability
         * (common-c CAPABILITY_REFERENCE_FRAME_INVALIDATION_AVC). */
        {
            int sops_flag = (g_psp_config.controlMode == CONTROL_MODE_XBOX) ? 1 : 0;
            int rtsp_corever = g_psp_config.disableEncryption ? 0 : 1;
            int surround_audio_info = AUDIO_STREAM_SURROUND_INFO;
            snprintf(path, sizeof(path),
                     "/launch?uniqueid=%s&uuid=%s&appid=%d&mode=%dx%dx%d&sops=%d"
                     "&rikey=%s&rikeyid=%d&localAudioPlayMode=0&additionalStates=0"
                    "&surroundAudioInfo=%d&remoteControllersBitmap=1&gcmap=1"
                    "&continuousAudio=0&corever=%d&supportedVideoFormats=1&videoCapabilities=2"
                     "&videoEncoderSlicesPerFrame=1",
                     CLIENT_UNIQUE_ID, client_identity_get_uuid(),
                     target_appid, g_psp_config.width, g_psp_config.height, g_psp_config.fps,
                     sops_flag, ri_key_hex, rikeyid, surround_audio_info,
                     rtsp_corever);
            pair_log("[LAUNCH] rtsp corever=%d disableAVEncryption=%d audioEnabled=%d surroundAudioInfo=%d continuousAudio=0%s%s\n",
                     rtsp_corever, g_psp_config.disableEncryption,
                     g_psp_config.audioEnabled, surround_audio_info,
                     g_psp_config.disableEncryption ? " (plaintext RTSP)" : "",
                     g_psp_config.audioEnabled ? "" : " (client drain/drop only)");
        }

        pair_log("[LAUNCH] GET https://%s:%d%s\n",
                 g_sunshine_host, SUNSHINE_HTTPS_PORT, path);

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
    ret = sceNetInit(512 * 1024, 42, 4096, 42, 4096);
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
            wifi_launch_disable_power_save();
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
    wifi_launch_restore_power_save();
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

static void rtsp_drain_before_close(int sock);
static void rtsp_close_transaction_socket(int *sockp);

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
static void rtsp_apply_io_timeouts(int sock)
{
    struct timeval snd_to;
    struct timeval rcv_to;

    snd_to.tv_sec = RTSP_SEND_TIMEOUT_MS / 1000;
    snd_to.tv_usec = (RTSP_SEND_TIMEOUT_MS % 1000) * 1000;
    rcv_to.tv_sec = RTSP_RECV_TIMEOUT_MS / 1000;
    rcv_to.tv_usec = (RTSP_RECV_TIMEOUT_MS % 1000) * 1000;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_to, sizeof(snd_to));
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_to, sizeof(rcv_to));
}

static int rtsp_wait_socket(int sock, int want_write, int timeout_ms)
{
    fd_set fds;
    struct timeval tv;
    int ret;

    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = sceNetInetSelect(sock + 1,
                           want_write ? NULL : &fds,
                           want_write ? &fds : NULL,
                           NULL,
                           &tv);
    if (ret <= 0) {
        return ret;
    }
    return FD_ISSET(sock, &fds) ? 1 : 0;
}

static int rtsp_send_all_timeout(int sock, const char *data, int data_len,
                                 const char *label, int *out_sent)
{
    int sent = 0;
    u32 start_ms = sceKernelGetSystemTimeLow() / 1000;
    int nb = 1;

    if (out_sent) {
        *out_sent = 0;
    }

    rtsp_apply_io_timeouts(sock);
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    while (sent < data_len) {
        int remaining = data_len - sent;
        int chunk = remaining > 536 ? 536 : remaining;
        int ret;

        ret = rtsp_wait_socket(sock, 1, 50);
        if (ret < 0) {
            pair_log("[RTSP] %s send wait failed at %d/%d (errno %d)\n",
                     label ? label : "request", sent, data_len,
                     sceNetInetGetErrno());
            return -1;
        }
        if (ret == 0) {
            if ((sceKernelGetSystemTimeLow() / 1000) - start_ms > RTSP_SEND_TIMEOUT_MS) {
                pair_log("[RTSP] %s send timeout at %d/%d bytes\n",
                         label ? label : "request", sent, data_len);
                return -1;
            }
            continue;
        }

        ret = (int)sceNetInetSend(sock, data + sent, chunk, 0);

        if (ret > 0) {
            sent += ret;
            if (out_sent) {
                *out_sent = sent;
            }
            continue;
        }

        if ((sceKernelGetSystemTimeLow() / 1000) - start_ms > RTSP_SEND_TIMEOUT_MS) {
            pair_log("[RTSP] %s send timeout at %d/%d bytes\n",
                     label ? label : "request", sent, data_len);
            return -1;
        }

        if (ret < 0) {
            int err = sceNetInetGetErrno();
            if (err == EAGAIN || err == EWOULDBLOCK) {
                sceKernelDelayThread(1000);
                continue;
            }
            pair_log("[RTSP] %s send failed at %d/%d (errno %d)\n",
                     label ? label : "request", sent, data_len, err);
            return -1;
        }

        pair_log("[RTSP] %s send closed at %d/%d bytes\n",
                 label ? label : "request", sent, data_len);
        return -1;
    }

    pair_log("[RTSP] sent request bytes=%d encrypted=%d\n", sent, g_rtsp_encrypted);
    diag_log_flush();
    return 0;
}

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

    /* Send the request with a hard timeout. Blocking PSP TCP sends can hang
     * indefinitely on weak WiFi after RTSP connect succeeds, leaving the UI
     * stuck on "Requesting Stream" with no actionable log. */
    pair_log("[RTSP] send request len=%d encrypted=%d\n",
             (int)strlen(request), g_rtsp_encrypted);
    diag_log_flush();

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

        if (rtsp_send_all_timeout(sock, (const char *)encrypted_buf,
                                  plaintext_len + 24,
                                  "encrypted request", &sent) != 0) {
            return -1;
        }
    } else {
        req_len = (int)strlen(request);
        if (rtsp_send_all_timeout(sock, request, req_len,
                                  "request", &sent) != 0) {
            return -1;
        }
    }

    /* Debug screen printing is routed to the diagnostic log above. */

    /* Receive the response (select-bounded, non-blocking with timeout). */
    rtsp_apply_io_timeouts(sock);
    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    memset(response, 0, resp_size);
    start_ms = sceKernelGetSystemTimeLow() / 1000;

    while (total_recv < resp_size - 1)
    {
        if ((sceKernelGetSystemTimeLow() / 1000) - start_ms > RTSP_RECV_TIMEOUT_MS) {
            break;
        }

        {
            u32 now_ms = sceKernelGetSystemTimeLow() / 1000;
            int remaining_ms = RTSP_RECV_TIMEOUT_MS - (int)(now_ms - start_ms);
            int wait_ms = remaining_ms > 50 ? 50 : remaining_ms;
            if (wait_ms <= 0) {
                break;
            }
            ret = rtsp_wait_socket(sock, 0, wait_ms);
            if (ret < 0) {
                pair_log("[RTSP] recv wait failed (errno %d)\n",
                         sceNetInetGetErrno());
                return -1;
            }
            if (ret == 0) {
                continue;
            }
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

    if (header_end_pos >= 0 && content_length >= 0) {
        if (total_recv < header_end_pos + content_length) {
            pair_log("[RTSP] incomplete response body (%d/%d)\n",
                     total_recv - header_end_pos, content_length);
            return -1;
        }
    }

    if (total_recv <= 0) {
#ifndef RETAIL_BUILD
        u32 elapsed = (sceKernelGetSystemTimeLow() / 1000) - start_ms;
        pair_log("[RTSP] empty response (elapsed=%dms, timeout=%d)\n",
                 (int)elapsed, RTSP_RECV_TIMEOUT_MS);
#endif
        return -1;
    }

    return total_recv;
}

static void rtsp_drain_before_close(int sock)
{
    char drain_buf[128];
    u32 start_ms;
    int ret;

    /* moonlight-common-c reads RTSP-over-TCP until the host closes the
     * transaction socket. PSP TCP shutdown can block for seconds here, so keep
     * the compatibility drain very short and let close() finish asynchronously. */
    start_ms = sceKernelGetSystemTimeLow() / 1000;
    do {
        ret = sceNetInetRecv(sock, drain_buf, sizeof(drain_buf), 0);
        if (ret == 0) {
            return;
        }
        if (ret > 0) {
            start_ms = sceKernelGetSystemTimeLow() / 1000;
            continue;
        }
        {
            int err = sceNetInetGetErrno();
            if (err != EAGAIN && err != EWOULDBLOCK) {
                return;
            }
        }
        sceKernelDelayThread(10 * 1000);
    } while ((sceKernelGetSystemTimeLow() / 1000) - start_ms < RTSP_DRAIN_TIMEOUT_MS);
}

static void rtsp_close_transaction_socket(int *sockp)
{
    int sock;
    u32 close_start_us;
    u32 close_elapsed_us;

    if (!sockp || *sockp < 0) {
        return;
    }

    sock = *sockp;
    *sockp = -1;

    close_start_us = sceKernelGetSystemTimeLow();

    /* PSP's TCP shutdown path can linger for seconds on Sunshine RTSP
     * transaction sockets after we've already consumed the complete response.
     * Do not call shutdown() here; keep close as bounded cleanup so a launched
     * Apollo session cannot expire between SETUP and PLAY. Use a normal close:
     * abortive SO_LINGER/RST can poison Sunshine's per-launch RTSP session
     * after ANNOUNCE and make PLAY return empty/reset. */
    {
        int nb = 1;
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));
    }
    rtsp_drain_before_close(sock);
    sceNetInetClose(sock);
    close_elapsed_us = sceKernelGetSystemTimeLow() - close_start_us;
    if (close_elapsed_us > 100000) {
        pair_log("[RTSP] transaction close took %dus\n", (int)close_elapsed_us);
    }
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

static int rtsp_connect_port(const char *host, int port, int timeout_ms)
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
    timeout.tv_sec  = timeout_ms / 1000;
    timeout.tv_usec = (timeout_ms % 1000) * 1000;

    ret = sceNetInetSelect(sock + 1, NULL, &writefds, NULL, &timeout);
    if (ret <= 0) {
        pair_log("[RTSP] connect %s:%d timed out after %dms (select ret=%d, errno %d)\n",
                 host, port, timeout_ms, ret, sceNetInetGetErrno());
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
static int rtsp_connect_with_policy(const char *phase,
                                    int timeout_ms,
                                    int max_retries)
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

    if (timeout_ms <= 0) {
        timeout_ms = RTSP_CONNECT_TIMEOUT_MS;
    }
    if (max_retries <= 0) {
        max_retries = 1;
    }

    for (attempt = 0; attempt < max_retries; attempt++) {
        for (i = 0; i < num_ports; i++) {
            sock = rtsp_connect_port(g_rtsp_connect_host, ports[i], timeout_ms);
            if (sock >= 0) {
                g_rtsp_port = ports[i];
                rtsp_rewrite_target_authority(g_rtsp_connect_host, g_rtsp_port);
                return sock;
            }
        }
        if (attempt < max_retries - 1) {
            pair_log("[RTSP] %s connect attempt %d/%d failed, retrying in %dms...\n",
                     phase ? phase : "transaction",
                     attempt + 1, max_retries, RTSP_CONNECT_RETRY_DELAY_MS);
            sceKernelDelayThread(RTSP_CONNECT_RETRY_DELAY_MS * 1000);
        }
    }

    pair_log("[RTSP] %s connect exhausted (%d attempts, timeout=%dms)\n",
             phase ? phase : "transaction", max_retries, timeout_ms);
    return -1;
}

static int rtsp_connect_post_setup(const char *phase)
{
    return rtsp_connect_with_policy(phase,
                                    RTSP_POST_SETUP_CONNECT_TIMEOUT_MS,
                                    RTSP_POST_SETUP_CONNECT_MAX_RETRIES);
}

static void rtsp_prime_video_endpoint(const char *phase, int settle_us)
{
#ifndef RETAIL_BUILD
    const char *tag = phase ? phase : "video";

    if (network_me_send_video_ping_burst(g_video_server_ip,
                                         g_video_server_port,
                                         g_video_ping_payload) == 0) {
        pair_log("[RTSP] %s video ping burst sent to %s:%d\n",
                 tag, g_video_server_ip, g_video_server_port);
    } else {
        pair_log("[RTSP] WARN: %s video ping burst failed\n", tag);
    }
#else
    (void)phase;
    (void)network_me_send_video_ping_burst(g_video_server_ip,
                                           g_video_server_port,
                                           g_video_ping_payload);
#endif

    if (settle_us > 0) {
        sceKernelDelayThread((SceUInt)settle_us);
    }
}

static void rtsp_seed_media_server_ip(void)
{
    if (g_video_server_ip[0] != '\0') {
        return;
    }

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

    {
        char session_header[96];
        session_header[0] = '\0';
        if (g_rtsp_session_id[0]) {
            snprintf(session_header, sizeof(session_header),
                     "Session: %s\r\n", g_rtsp_session_id);
        }

    if (g_local_bind_ip[0] != '\0') {
        snprintf(request, sizeof(request),
                 "SETUP %s RTSP/1.0\r\n"
                 "CSeq: %d\r\n"
                 "User-Agent: psp-moonlight\r\n"
                 "X-GS-ClientVersion: %d\r\n"
                 "Host: %s\r\n"
                 "%s"
                 "Transport: unicast;X-GS-ClientPort=%d-%d;destination=%s\r\n"
                 "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                 "\r\n",
                 stream_id, rtsp_cseq++, CLIENT_VERSION, g_rtsp_host_header,
                 session_header,
                 client_port_lo, client_port_hi, g_local_bind_ip);
    } else {
        snprintf(request, sizeof(request),
                 "SETUP %s RTSP/1.0\r\n"
                 "CSeq: %d\r\n"
                 "User-Agent: psp-moonlight\r\n"
                 "X-GS-ClientVersion: %d\r\n"
                 "Host: %s\r\n"
                 "%s"
                 "Transport: unicast;X-GS-ClientPort=%d-%d\r\n"
                 "If-Modified-Since: Thu, 01 Jan 1970 00:00:00 GMT\r\n"
                 "\r\n",
                 stream_id, rtsp_cseq++, CLIENT_VERSION, g_rtsp_host_header,
                 session_header,
                 client_port_lo, client_port_hi);
    }
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
        int requested_bitrate_kbps = g_psp_config.bitrate > 0 ? g_psp_config.bitrate : 1600;
        int launch_bitrate_kbps = signal_strength_get_launch_bitrate_kbps(requested_bitrate_kbps);
        int transport_bitrate_kbps;
        int configured_bitrate_kbps;
        int minimum_bitrate_kbps;
        int packet_size = g_psp_config.packetSize > 0 ? g_psp_config.packetSize : DEFAULT_PACKET_SIZE;
        /* Resolution row array - authoritative source of stream dimensions. */
        int ri = g_psp_config.resolutionIndex;
        if (ri < 0 || ri >= RESOLUTION_COUNT) ri = 0;
        int stream_w = RESOLUTION_WIDTHS[ri];
        int stream_h = RESOLUTION_HEIGHTS[ri];
        int stream_fps = g_psp_config.fps > 0 ? g_psp_config.fps : 30;
        int client_refresh_x100 = stream_fps * 100;
        int audio_packet_duration_ms = 10;
        int fec_min_required_packets = 2;
        int requested_fec_percent = PSP_VIDEO_FEC_PERCENT;
        int video_fec_enabled = 1;
        int intra_refresh_enabled = 1;
        int suppress_soft_idr_for_intra_refresh = 0;

        /* Keep video FEC explicit. Normal PSP presets request repair packets,
         * while FEC=0 must stay a real low-work experiment that asks both the
         * host config and SDP sender path to stop adding video parity. */
        if (requested_fec_percent <= 0) {
            video_fec_enabled = 0;
            requested_fec_percent = 0;
        }
        if (requested_fec_percent > 255) {
            requested_fec_percent = 255;
        }

        /* Moonlight-common-c rounds packet size to 16-byte chunks and subtracts
         * encrypted video header overhead when video encryption is enabled. */
        packet_size -= (packet_size % 16);
        if (packet_size < MIN_STREAM_PACKET_SIZE) {
            packet_size = MIN_STREAM_PACKET_SIZE;
        }
        if (packet_size > MAX_STREAM_PACKET_SIZE) {
            packet_size = MAX_STREAM_PACKET_SIZE;
        }
        /* Keep Sunshine's configured and transport bitrate at the requested
         * preset value for startup/IDR behavior. The adaptive controller still
         * reports a lower PSP survival floor through minimumBitrateKbps and
         * control bandwidth reports, but capping the SDP transport target at
         * 80% caused media starvation on hardware. */
        transport_bitrate_kbps = launch_bitrate_kbps;
        if (transport_bitrate_kbps < 32) {
            transport_bitrate_kbps = 32;
        }
        configured_bitrate_kbps = launch_bitrate_kbps;
        minimum_bitrate_kbps = signal_strength_get_adaptive_floor_kbps(transport_bitrate_kbps);
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
        if (stream_w * stream_h <= 256 * 144 && stream_fps <= 30) {
            /* Hardware rerun: fecMin=2 at 320kbps/p1056 raised video FEC
             * overhead to about 61%, kept the same two unrecoverable frames,
             * and introduced audio underruns. Keep the low-work floor at one
             * parity packet for 144p and tune the host repair percentage
             * instead of doubling every small frame's parity burst. */
            fec_min_required_packets = 1;

            /* PSP-1000 low-work audio: keep Opus mono and a sparse packet
             * cadence. A 2026-05-17 40 ms rerun lowered burst size but
             * regressed failed-frame count and usable video, so 60 ms remains
             * the 144p baseline until a source fix removes the receive stall. */
            audio_packet_duration_ms = 60;

            /* A 2026-05-16 PSP-1000 hardware run rejected auto-disabling
             * intra refresh at 144p20: strict recovery regressed from one
             * unrecoverable/gap event to four, with no release-grade visual
             * improvement. Keep the default enabled unless build-forced. */
        }
        if (!g_psp_config.audioEnabled && configured_bitrate_kbps > 0) {
            /* There is no official client-side GameStream/Sunshine control
             * that stops audio RTP while leaving the host config untouched.
             * Audio Disabled is a local PSP work saver: keep audio SETUP/ping
             * for liveness, drain/drop RTP, and skip Opus/SRC/playback. Do
             * not add back audio budget here because the host may still send
             * the low-audio RTP stream. */
            pair_log("[RTSP] audio disabled: client drains/drops audio RTP; no video-budget addback\n");
        } else if (stream_w * stream_h <= 256 * 144 && stream_fps <= 30 &&
                   configured_bitrate_kbps > 0 && g_psp_config.audioEnabled) {
            int low_audio_add_kbps =
                PSP_LOW_AUDIO_CONFIGURED_ADD_KBPS * AUDIO_STREAM_CHANNELS;
            /* Sunshine treats x-ml-video.configuredBitrateKbps as a total
             * stream budget, then subtracts video FEC, low-quality audio, and
             * protocol overhead before setting the encoder bitrate. Add back a
             * bounded one-channel allowance. Hardware rejected 112 kbps and
             * full audio+FEC addback because both crossed the packet-survival
             * cliff before they fixed visible blockiness. */
            configured_bitrate_kbps += low_audio_add_kbps;
            pair_log("[RTSP] low-audio configured-bitrate addback=%dkbps\n",
                     low_audio_add_kbps);
        }
        if (PSP_VIDEO_FEC_MIN_REQUIRED >= 0) {
            fec_min_required_packets = PSP_VIDEO_FEC_MIN_REQUIRED;
            pair_log("[RTSP] build override fecMin=%d\n",
                     fec_min_required_packets);
        }
        if (fec_min_required_packets < 0) {
            fec_min_required_packets = 0;
        }
        if (fec_min_required_packets > 16) {
            fec_min_required_packets = 16;
        }
        if (!video_fec_enabled) {
            fec_min_required_packets = 0;
        }
        if (PSP_AUDIO_PACKET_DURATION_MS > 0) {
            int override_ms = PSP_AUDIO_PACKET_DURATION_MS;
            if (override_ms == 5 || override_ms == 10 ||
                override_ms == 20 || override_ms == 40 ||
                override_ms == 60) {
                audio_packet_duration_ms = override_ms;
                pair_log("[RTSP] build override audio packetDuration=%dms\n",
                         audio_packet_duration_ms);
            } else {
                pair_log("[RTSP] ignored unsupported audio packetDuration override=%dms\n",
                         override_ms);
            }
        }
        if (PSP_VIDEO_INTRA_REFRESH >= 0) {
            intra_refresh_enabled = PSP_VIDEO_INTRA_REFRESH ? 1 : 0;
            pair_log("[RTSP] build override intraRefresh=%d\n",
                     intra_refresh_enabled);
        }
        if (client_refresh_x100 < 100) {
            client_refresh_x100 = 100;
        }

        pair_log("[RTSP] negotiated bitrate requested=%d kbps launch=%d kbps transport=%d kbps min=%d kbps, resolution=%dx%d@%d\n",
             requested_bitrate_kbps, launch_bitrate_kbps,
             transport_bitrate_kbps, minimum_bitrate_kbps, stream_w, stream_h, stream_fps);
        pair_log("[RTSP] Sunshine configured bitrate=%d kbps (video budget request)\n",
                 configured_bitrate_kbps);
        if (PSP_VIDEO_FEC_PERCENT <= 0) {
            pair_log("[RTSP] requested FEC0 low-work mode; using fec.enable=0 repair=0 fecMin=0\n");
        }
        pair_log("[RTSP] using packetSize=%d fec=%s repair=%d fecMin=%d (enc_enabled=%d)\n",
                 packet_size, video_fec_enabled ? "on" : "off",
                 requested_fec_percent, fec_min_required_packets, enc_enabled);
        pair_log("[RTSP] audio packetDuration=%dms\n",
                 audio_packet_duration_ms);
        pair_log("[RTSP] clientRefreshRateX100=%d (matches maxFPS)\n",
                 client_refresh_x100);
        audio_thread_set_packet_duration_ms(audio_packet_duration_ms);
        g_intra_refresh_active = suppress_soft_idr_for_intra_refresh;
        pair_log("[RTSP] intraRefresh=%d, post-startup soft IDR suppression=%s\n",
                 intra_refresh_enabled,
                 suppress_soft_idr_for_intra_refresh ? "on" : "off");
        pair_log("[RTSP] low-audio Host header=%s (Sunshine normal audio tier)\n",
                 g_rtsp_host_header);
        pair_log("[STREAM CFG] %dx%d@%d bitrate=%d packet=%d\n",
                 stream_w, stream_h, stream_fps,
                 requested_bitrate_kbps, packet_size);

        {
            int announce_video_port = g_video_client_port > 0 ?
                                      g_video_client_port : MOONLIGHT_VIDEO_PORT;
            char audio_media_sdp[96];
            audio_media_sdp[0] = '\0';
            if (g_psp_config.audioEnabled && g_audio_rtsp_ok) {
                unsigned short announce_audio_port = 0;
                if (audio_thread_reserve_client_port(&announce_audio_port) == 0 &&
                    announce_audio_port > 0) {
                    snprintf(audio_media_sdp, sizeof(audio_media_sdp),
                             "m=audio %d\r\n"
                             "a=rtpmap:97 opus/48000/%d\r\n",
                             (int)announce_audio_port,
                             AUDIO_STREAM_CHANNELS);
                    pair_log("[RTSP] audio media enabled in SDP on port=%d\n",
                             (int)announce_audio_port);
                } else {
                    pair_log("[RTSP] WARN: audio port unavailable; omitting audio SDP\n");
                }
            } else if (!g_psp_config.audioEnabled) {
                pair_log("[RTSP] audio disabled by config; omitting audio SDP\n");
            } else {
                pair_log("[RTSP] audio SETUP unavailable; omitting audio SDP\n");
            }



            /* Canonical SDP for PSP Baseline (Profile 66, Level 2.1, CAVLC).
             * We explicitly set h264Profile:66 and entropyCodingMode:0 to force
             * Sunshine/Apollo to avoid CABAC/HighProfile which PSP cannot decode.
             *
             * COMPREHENSIVE SDP — aligned with moonlight-common-c SdpGenerator.c
             * plus PSP-specific optimizations for 802.11b WiFi:
             *
             *  BUG FIXES from prior version:
             *   - clientRefreshRateX100 now matches maxFPS; Sunshine rejects
             *     mismatched low-FPS RTSP metadata on some encoder paths.
             *   - minimumBitrateKbps was missing (server couldn't latch bitrate)
             *   - configuredBitrateKbps was missing (server couldn't adjust FEC)
             *
             *  NEW ATTRIBUTES (from reference moonlight-common-c):
             *   - timeoutLengthMs:7000         encoder timeout
             *   - framesWithInvalidRefThreshold:0  no tolerance for bad refs
             *   - fec.enable:1                 keep upstream FEC sequencing path
             *   - fec.repairPercent            requested repair percentage for hosts that honor it
             *   - bllFec.enable:0              disable BLL-FEC (worse on lossy nets)
             *   - drc.enable:0                 disable dynamic resolution changes
             *   - enableRecoveryMode:0         recovery mode breaks FEC queue
             *   - videoQualityScoreUpdateTime:5000  quality scoring interval
             *   - minimumBitrateKbps           adaptive floor for PSP WiFi recovery
             *   - configuredBitrateKbps        client-selected target bitrate
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
                /* --- Audio: mono, low quality, expanded to stereo locally --- */
                "a=x-nv-audio.surround.numChannels:%d\r\n"
                "a=x-nv-audio.surround.channelMask:%d\r\n"
                "a=x-nv-audio.surround.AudioQuality:0\r\n"
                "a=x-nv-audio.surround.enable:0\r\n"
                "a=x-nv-aqos.packetDuration:%d\r\n"
                /* --- Transport: ENet reliable UDP, no qWAVE DSCP --- */
                "a=x-nv-general.useReliableUdp:1\r\n"
                "a=x-nv-aqos.qosTrafficType:0\r\n"
                "a=x-nv-vqos[0].qosTrafficType:0\r\n"
                /* --- FEC: critical for 802.11b packet loss --- */
                "a=x-nv-vqos[0].fec.enable:%d\r\n"
                "a=x-nv-vqos[0].fec.repairPercent:%d\r\n"
                "a=x-nv-vqos[0].fec.minRequiredFecPackets:%d\r\n"
                "a=x-nv-vqos[0].bllFec.enable:0\r\n"
                /* --- Feature flags --- */
                "a=x-ml-general.featureFlags:%d\r\n"
                "a=x-nv-general.featureFlags:0\r\n"
                /* --- Encryption / codec / intra refresh --- */
                "a=x-ss-general.encryptionEnabled:%d\r\n"
                "a=x-ss-video[0].chromaSamplingType:0\r\n"
                "a=x-ss-video[0].intraRefresh:%d\r\n"
                /* --- Encoder constraints: profile + entropy coding --- */
                "a=x-nv-video[0].videoEncoderSlicesPerFrame:1\r\n"
                "a=x-nv-vqos[0].bitStreamFormat:0\r\n"
                "a=x-nv-video[0].maxNumReferenceFrames:1\r\n"
                "a=x-nv-video[0].h264Profile:%d\r\n"
                "a=x-nv-video[0].entropyCodingMode:%d\r\n"
                /* --- Color: BT.601 full range, SDR --- */
                "a=x-nv-video[0].encoderCscMode:1\r\n"
                "a=x-nv-video[0].dynamicRangeMode:0\r\n"
                /* --- Stream refresh: must match maxFPS for Sunshine/Apollo --- */
                "a=x-nv-video[0].clientRefreshRateX100:%d\r\n"
                /* --- Rate control: initial/max target with adaptive floor --- */
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
                /* --- Optional audio media line --- */
                "%s",
                CLIENT_VERSION,
                stream_w, stream_h, stream_fps,
                packet_size,
                AUDIO_STREAM_CHANNELS,
                AUDIO_STREAM_CHANNEL_MASK,
                audio_packet_duration_ms,
                video_fec_enabled,
                requested_fec_percent,
                fec_min_required_packets,
                (ML_FF_FEC_STATUS | ML_FF_SESSION_ID_V1),
                enc_enabled,
                intra_refresh_enabled,
                h264_profile, entropy_mode,
                client_refresh_x100,
                transport_bitrate_kbps, transport_bitrate_kbps,
                minimum_bitrate_kbps, transport_bitrate_kbps,
                configured_bitrate_kbps,
                announce_video_port,
                profile_level_id,
                audio_media_sdp);
            }

            if (sdp_len >= (int)sizeof(sdp_payload))
                sdp_len = (int)sizeof(sdp_payload) - 1;
            /* qosTrafficType:0 keeps Sunshine/Apollo's host-side qWAVE/DSCP path
             * disabled. The PSP cannot benefit from Windows QoS tagging, and the
             * extra host socket flow setup is wasted work for the low-bitrate PSP
             * path. */

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
    upnp_remove_stream_mappings();

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
        return RTSP_ERR_PLAY_SESSION_DEAD;

    pair_log("[RTSP] PLAY response: %.180s\n", response);

    /* Check for 200 OK */
    if (!rtsp_response_is_200(response))
    {
        pair_log("[RTSP] PLAY failed (no 200)\n");
        return RTSP_ERR_PLAY_SESSION_DEAD;
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
    int upnp_ret;
    char sdp_buf[SDP_BUF_SIZE];
    unsigned short upnp_video_port = 0;
    unsigned short upnp_audio_port = 0;

    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_RTSP);

    g_rtsp_session_id[0] = '\0';
    rtsp_cseq = 1;
    g_rtsp_enc_tx_seq = 0;  /* Reset encryption sequence for fresh RTSP attempt */
    g_video_client_port = 0;
    g_video_server_ip[0] = '\0';
    g_video_ping_payload[0] = '\0';
    g_audio_ping_payload[0] = '\0';

    /* ---------------------------------------------------------------
     * Per-command RTSP transactions. Apollo/Sunshine closes the RTSP TCP
     * socket after each response on this plaintext corever=0 path; hardware
     * logs show DESCRIBE receives EOF immediately if we try to reuse OPTIONS'
     * socket. Keep each command bounded and close quickly; SETUP retries are
     * still single-shot relaunch recovery points because each retry may bind a
     * fresh UDP receive socket.
     * --------------------------------------------------------------- */

    /* 1. OPTIONS */
    sock = rtsp_connect_post_setup("OPTIONS");
    if (sock < 0) {
        pair_log("[RTSP] OPTIONS connect failed; launched session RTSP listener is unreachable\n");
        return RTSP_ERR_PLAY_SESSION_DEAD;
    }
    ret = rtsp_options(sock);
    rtsp_close_transaction_socket(&sock);
    if (ret < 0) {
        pair_log("[RTSP] OPTIONS failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect_post_setup("OPTIONS retry");
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_options(sock);
        rtsp_close_transaction_socket(&sock);
        if (ret < 0) {
            pair_log("[RTSP] OPTIONS failed twice; launched session is dead, relaunch required\n");
            ret = RTSP_ERR_PLAY_SESSION_DEAD;
            goto rtsp_fail;
        }
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
    sock = rtsp_connect_post_setup("DESCRIBE");
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_describe(sock, sdp_buf, sizeof(sdp_buf));
    rtsp_close_transaction_socket(&sock);
    if (ret < 0) {
        pair_log("[RTSP] DESCRIBE failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect_post_setup("DESCRIBE retry");
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_describe(sock, sdp_buf, sizeof(sdp_buf));
        rtsp_close_transaction_socket(&sock);
        if (ret < 0) {
            pair_log("[RTSP] DESCRIBE failed twice; launched session is dead, relaunch required\n");
            ret = RTSP_ERR_PLAY_SESSION_DEAD;
            goto rtsp_fail;
        }
    }

    /* Parse stream IDs and Sunshine encryption feature masks from DESCRIBE SDP */
    rtsp_parse_stream_ids(sdp_buf);
    g_encryption_supported = 0;
    g_encryption_requested = 0;
    {
        const char *enc_tag = strstr(sdp_buf, "encryptionSupported:");
        if (enc_tag) {
            enc_tag += strlen("encryptionSupported:");
            g_encryption_supported = atoi(enc_tag);
        }
        enc_tag = strstr(sdp_buf, "encryptionRequested:");
        if (enc_tag) {
            enc_tag += strlen("encryptionRequested:");
            g_encryption_requested = atoi(enc_tag);
        }
        pair_log("[RTSP] encryptionSupported=0x%x requested=0x%x\n",
                 g_encryption_supported, g_encryption_requested);
    }
    pair_log("[RTSP] DESCRIBE done, delaying 100ms before SETUP...\n");
    sceKernelDelayThread(100 * 1000);

    /* 3a. SETUP audio. Sunshine still requires an audio SS_PING to keep the
     * session alive. With Audio Disabled, this remains a client-side low-work
     * path: the PSP performs audio SETUP/ping, drains/drops audio RTP, and
     * skips Opus/SRC/playback. */
    g_audio_rtsp_ok = 0;
    pair_log("[RTSP] connecting for SETUP audio%s...\n",
             g_psp_config.audioEnabled ? "" : " (keepalive-only)");
    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_VIDEO);
    sock = rtsp_connect_post_setup("SETUP audio");
    if (sock < 0) {
        pair_log("[RTSP] WARN: audio connect failed, continuing without audio\n");
    } else {
        ret = rtsp_setup_stream(sock, g_audio_stream_id);
        rtsp_close_transaction_socket(&sock);
        if (ret < 0) {
            pair_log("[RTSP] SETUP audio failed, retrying...\n");
            sceKernelDelayThread(500 * 1000);
            sock = rtsp_connect_post_setup("SETUP audio retry");
            if (sock >= 0) {
                ret = rtsp_setup_stream(sock, g_audio_stream_id);
                rtsp_close_transaction_socket(&sock);
            }
        }
        if (ret >= 0) {
            g_audio_rtsp_ok = 1;
            rtsp_seed_media_server_ip();
            if (audio_thread_start_ping_only() == 0) {
                pair_log("[RTSP] early audio ping started before SETUP video%s\n",
                         g_psp_config.audioEnabled ? "" : " (keepalive-only)");
            } else {
                pair_log("[RTSP] WARN: early audio ping failed before SETUP video\n");
            }
        } else {
            pair_log("[RTSP] WARN: audio SETUP failed after retry, continuing without audio\n");
        }
    }

    if (g_audio_rtsp_ok) {
        pair_log("[RTSP] SETUP audio done, delaying 50ms before SETUP video...\n");
        sceKernelDelayThread(50 * 1000);
    } else {
        pair_log("[RTSP] SETUP audio skipped/failed; continuing directly to SETUP video...\n");
    }

    /* 3b. SETUP video */
    sock = rtsp_connect_post_setup("SETUP video");
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_setup_stream(sock, g_video_stream_id);
    rtsp_close_transaction_socket(&sock);
    if (ret < 0) {
        pair_log("[RTSP] SETUP video failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect_post_setup("SETUP video retry");
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_setup_stream(sock, g_video_stream_id);
        rtsp_close_transaction_socket(&sock);
        if (ret < 0) {
            ret = RTSP_ERR_PLAY_SESSION_DEAD;
            goto rtsp_fail;
        }
    }

    sceKernelDelayThread(50 * 1000);

    /* 3c. SETUP control */
    sock = rtsp_connect_post_setup("SETUP control");
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_setup_stream(sock, g_control_stream_id);
    rtsp_close_transaction_socket(&sock);
    if (ret < 0) {
        pair_log("[RTSP] SETUP control failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect_post_setup("SETUP control retry");
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_setup_stream(sock, g_control_stream_id);
        rtsp_close_transaction_socket(&sock);
        if (ret < 0) {
            ret = RTSP_ERR_PLAY_SESSION_DEAD;
            goto rtsp_fail;
        }
    }

    /* Idempotent safety net for audio-enabled streams. */
    if (g_audio_rtsp_ok) {
        ret = audio_thread_start_ping_only();
        if (ret < 0) pair_log("[RTSP] WARN: audio ping failed\n");
    }

    upnp_video_port = (g_video_client_port > 0 && g_video_client_port <= 65535)
                          ? (unsigned short)g_video_client_port
                         : 0;
    if (g_audio_rtsp_ok) {
        audio_thread_reserve_client_port(&upnp_audio_port);
    }

    upnp_ret = upnp_prepare_stream_mappings(g_sunshine_host,
                                            upnp_video_port,
                                            g_audio_rtsp_ok,
                                            upnp_audio_port);
    if (upnp_ret > 0) {
        pair_log("[UPNP] mapped %d UDP ports for remote/hotspot stream\n", upnp_ret);
    } else if (upnp_ret == 0) {
        pair_log("[UPNP] skipped (private/LAN target)\n");
    } else {
        pair_log("[UPNP] unavailable (%d), continuing without mapping\n", upnp_ret);
    }

    /* 4. ANNOUNCE */
    {
        /* Negotiate encryption. SS_ENC_CONTROL_V2 is the low-overhead control
         * The PSP client does NOT implement SS_ENC_CONTROL_V2 (bit 2) —
         * requesting it causes the server to ECONNRESET on RTSP PLAY.
         * Track audio encryption separately for the audio recv loop.
         *
         * The PSP control stream is always Gen7Enc in control_stream.c, so
         * advertise SS_ENC_CONTROL_V2 whenever Sunshine supports it. The
         * disableEncryption flag disables video/audio AES, which is the
         * measurable steady-state PSP-1000 stream load. */
        int enc_to_use = 0;
        if (g_encryption_supported & SS_ENC_CONTROL_V2) {
            enc_to_use |= SS_ENC_CONTROL_V2;
        }

        g_audio_encryption_enabled = 0;
        if (!g_psp_config.disableEncryption) {
            if ((g_encryption_requested & SS_ENC_VIDEO) &&
                (g_encryption_supported & SS_ENC_VIDEO)) {
                enc_to_use |= SS_ENC_VIDEO;
            }
            if (g_psp_config.audioEnabled && g_audio_rtsp_ok &&
                (g_encryption_requested & SS_ENC_AUDIO) &&
                (g_encryption_supported & SS_ENC_AUDIO)) {
                enc_to_use |= SS_ENC_AUDIO;
                g_audio_encryption_enabled = 1;
            }
        } else {
            pair_log("[RTSP] AV encryption disabled by config; keeping RTSP/control compatibility control-v2=%d\n",
                     (enc_to_use & SS_ENC_CONTROL_V2) ? 1 : 0);
        }

        pair_log("[RTSP] enc_to_use=0x%x (audio_enc=%d server_supported=0x%x requested=0x%x disableConfig=%d)\n",
                 enc_to_use, g_audio_encryption_enabled,
                 g_encryption_supported, g_encryption_requested,
                 g_psp_config.disableEncryption);
        sock = rtsp_connect_post_setup("ANNOUNCE");
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_announce(sock, enc_to_use);
        rtsp_close_transaction_socket(&sock);
        if (ret < 0) {
            pair_log("[RTSP] ANNOUNCE failed, retrying...\n");
            sceKernelDelayThread(500 * 1000);
            sock = rtsp_connect_post_setup("ANNOUNCE retry");
            if (sock < 0) { ret = -1; goto rtsp_fail; }
            ret = rtsp_announce(sock, enc_to_use);
            rtsp_close_transaction_socket(&sock);
            if (ret < 0) goto rtsp_fail;
        }
    }

    /* Sunshine/Apollo creates the stream session from ANNOUNCE and immediately
     * starts waiting for SS_PING. Keep one low-cost video prime before PLAY,
     * then follow the common-c order and issue PLAY immediately. The audio
     * ping thread is already running here; for Audio Disabled it exists only
     * to satisfy Sunshine's session liveness check. */
    rtsp_prime_video_endpoint("pre-PLAY", 100 * 1000);

    /* Apollo/AMF has historically been sensitive to low-res refresh-mode
     * changes, so intraRefresh is negotiated explicitly above and can be
     * forced at build time when a hardware run proves the auto mode wrong. */

    /* 5. PLAY */
    sock = rtsp_connect_post_setup("PLAY");
    if (sock < 0) { ret = -1; goto rtsp_fail; }
    ret = rtsp_play(sock);
    if (ret < 0) {
        rtsp_close_transaction_socket(&sock);
        if (ret == RTSP_ERR_PLAY_SESSION_DEAD) {
            pair_log("[RTSP] PLAY reset/failed; launched session is dead, relaunch required\n");
            goto rtsp_fail;
        }
        pair_log("[RTSP] PLAY failed, retrying...\n");
        sceKernelDelayThread(500 * 1000);
        sock = rtsp_connect_post_setup("PLAY retry");
        if (sock < 0) { ret = -1; goto rtsp_fail; }
        ret = rtsp_play(sock);
        if (ret < 0) { rtsp_close_transaction_socket(&sock); goto rtsp_fail; }
    }
    /* Keep PLAY socket open for potential TEARDOWN later */
    g_rtsp_persistent_sock = sock;
    sock = -1;

    /* Keep the video endpoint fresh after PLAY returns. The audio SS_PING
     * thread is already running from SETUP audio; sending an extra 3-ping
     * audio burst here pulls audio+audio-FEC into the PSP before the first
     * video IDR has settled, which increases startup packet pressure on
     * PSP-1000 WiFi. */
    rtsp_prime_video_endpoint("post-PLAY", 0);

    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_READY);
    sceKernelDelayThread(100 * 1000);

    pair_log("[RTSP] session established successfully\n");
    diag_log_flush();
    return 0;

rtsp_fail:
    upnp_remove_stream_mappings();
    if (sock >= 0) rtsp_close_transaction_socket(&sock);
    if (g_audio_rtsp_ok) {
        audio_thread_shutdown();
        g_audio_rtsp_ok = 0;
    }
    if (g_video_client_port > 0) {
        network_me_shutdown();
        g_video_client_port = 0;
    }
    g_video_ping_payload[0] = '\0';
    g_audio_ping_payload[0] = '\0';
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

/* Debug log helper for pairing thread (writes to savedata moonlight_debug.log) */
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
static void http_pair_close_abortive(int sock)
{
    if (sock >= 0) {
        struct linger lg = { 1, 0 };
        sceNetInetSetsockopt(sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        sceNetInetClose(sock);
    }
}

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
            http_pair_close_abortive(sock);
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
        http_pair_close_abortive(sock);
        return -1;
    }

    nb = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    /* ---- Send request in three pieces (path can be several KB) ---------- */
    {
        const char *method   = "GET ";
        char        hdr[256];
        int         hdr_len, piece_len;
        int         send_errno, send_off;

        piece_len = 4; /* "GET " */
        send_errno = 0;
        send_off = 0;
        if (net_send_all_psp(sock, method, piece_len,
                             0, 0, &send_errno, &send_off) != 0) {
            pair_log("[HTTP] send failed at %d/%d errno=%d\n",
                     send_off, piece_len, send_errno);
            http_pair_close_abortive(sock);
            return -1;
        }

        piece_len = (int)strlen(path);
        send_errno = 0;
        send_off = 0;
        if (net_send_all_psp(sock, path, piece_len,
                             0, 0, &send_errno, &send_off) != 0) {
            pair_log("[HTTP] send path failed at %d/%d errno=%d\n",
                     send_off, piece_len, send_errno);
            http_pair_close_abortive(sock);
            return -1;
        }

        hdr_len = snprintf(hdr, sizeof(hdr),
                           " HTTP/1.0\r\nHost: %s:%d\r\n"
                           "User-Agent: PSPMoonlight/1.0\r\n"
                           "Connection: close\r\n\r\n",
                           host, port);
        send_errno = 0;
        send_off = 0;
        if (net_send_all_psp(sock, hdr, hdr_len,
                             0, 0, &send_errno, &send_off) != 0) {
            pair_log("[HTTP] send hdr failed at %d/%d errno=%d\n",
                     send_off, hdr_len, send_errno);
            http_pair_close_abortive(sock);
            return -1;
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

static void pairing_unpair_best_effort(const char *host, const char *pair_uuid)
{
    char url[256];
    char resp[256];

    if (!host || !host[0]) {
        return;
    }

    snprintf(url, sizeof(url),
             "http://%s:%d/unpair?uniqueid=%s&uuid=%s",
             host, SUNSHINE_HTTP_PORT, CLIENT_UNIQUE_ID,
             (pair_uuid && pair_uuid[0]) ? pair_uuid : client_identity_get_uuid());
    resp[0] = '\0';
    if (http_pair_get(url, resp, sizeof(resp)) == 0) {
        pair_log("[PAIR] cleanup /unpair response: %.80s\n", resp);
    } else {
        pair_log("[PAIR] cleanup /unpair failed\n");
    }
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
 * do_rsa_sign - RSA-PKCS#1v15-SHA256 sign 'client_secret' with client key.
 *
 * Parses active runtime client key,
 * SHA-256 hashes client_secret (16 bytes), signs the hash, and writes
 * the 256 byte signature into sig_out.
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
    const char *active_key_pem = get_active_client_key_pem();

    if (!active_key_pem) {
        return -1;
    }

    mbedtls_pk_init(&pk);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    mbedtls_entropy_init(&entropy);

    ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                 (const unsigned char *)"psp-moonlight", 13);
    if (ret != 0) goto cleanup;

    ret = mbedtls_pk_parse_key(&pk,
                                (const unsigned char *)active_key_pem,
                                strlen(active_key_pem) + 1, /* include NUL */
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
    char resp[4096];
    char url[8192];
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
    unsigned char client_cert_sig[CLIENT_CERT_SIG_LEN];
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
    const char   *active_cert_hex;
    int           pairing_success = 0;

    pair_log("[PAIR] Thread started host=%s pin=****\n", host);
    pair_uuid = client_identity_get_uuid();
    active_cert_hex = get_active_client_cert_hex();
    if (!pair_uuid || !pair_uuid[0] || !active_cert_hex || !active_cert_hex[0]) {
        pair_log("[PAIR] runtime identity unavailable\n");
        ta->result = -1;
        goto done;
    }

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
             pair_uuid, DEVICE_NAME, salt_hex, active_cert_hex);

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
    if (get_active_client_cert_sig(client_cert_sig, sizeof(client_cert_sig)) != 0) {
        pair_log("[PAIR] failed to load client certificate signature\n");
        ta->result = -5;
        goto done;
    }

    memcpy(crdata,        cr_dec + 32,        16); /* server_challenge */
    memcpy(crdata + 16,   client_cert_sig,    CLIENT_CERT_SIG_LEN); /* 256 */
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
    pair_log("[PAIR] Step 4 OK\n");

    /* Official Moonlight clients finish pairing with an authenticated HTTPS
     * pairchallenge using the newly registered client certificate.  Without
     * this, some Sunshine versions keep the web UI in a failed/half-paired
     * state even when the four plain HTTP challenge steps succeeded. */
    if (ta->cancel) goto done;
    pair_log("[PAIR] Step 5: HTTPS pairchallenge\n");
    snprintf(url, sizeof(url),
             "/pair?uniqueid=%s&uuid=%s&devicename=%s&updateState=1&phrase=pairchallenge",
             CLIENT_UNIQUE_ID, pair_uuid, DEVICE_NAME);

    resp[0] = '\0';
    ret = https_launch_get(host, SUNSHINE_HTTPS_PORT, url, resp, sizeof(resp));
    if (ret < 0) {
        pair_log("[PAIR] Step 5 HTTPS failed\n");
        ta->result = -11;
        goto done;
    }
    ret = xml_get_value_safe(resp, "paired", paired_val, sizeof(paired_val));
    if (ret < 0 || strcmp(paired_val, "1") != 0) {
        pair_log("[PAIR] Step 5 rejected (paired=%s)\n",
                 (ret < 0) ? "<missing>" : paired_val);
        ta->result = -12;
        goto done;
    }
    pair_log("[PAIR] Step 5 OK - pairing complete!\n");

    /* Pairing protocol succeeded */
    pairing_success = 1;
    *(ta->is_paired) = 1;
    ta->result = 0;
    strncpy(g_last_paired_host, host, sizeof(g_last_paired_host) - 1);
    g_last_paired_host[sizeof(g_last_paired_host) - 1] = '\0';
    if (config_add_paired_host(&g_psp_config, host) < 0) {
        pair_log("[PAIR] WARNING: failed to persist paired host %s\n", host);
    } else {
        pair_log("[PAIR] persisted paired host %s\n", host);
    }

done:
    if (!pairing_success) {
        pairing_unpair_best_effort(host, pair_uuid);
    }
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

    if (s_retry_appid >= 0 && strcmp(s_retry_host, g_sunshine_host) != 0) {
        network_connect_clear_retry_app();
    }

    /*--- Show Game Library so the user picks an app -------------------------*/
    {
        int selected_appid;
        const char *selected_title;

        if (s_retry_appid >= 0) {
            selected_appid = s_retry_appid;
            selected_title = s_retry_title[0] ? s_retry_title : game_grid_ui_get_selected_title();
            pair_log("[STEP 4] Retrying previous appid=%d (%s) after infrastructure failure\n",
                     selected_appid,
                     selected_title && selected_title[0] ? selected_title : "unknown");
        } else {
            selected_appid = game_grid_ui_run(g_sunshine_host);
            if (selected_appid == -3) {
                /* Empty game list -> probable stale pairing. Clear cached state
                 * so the next connect_to_sunshine() triggers a fresh PIN pair. */
                pair_log("[STEP 4] Game list empty - clearing stale pairing for %s\n",
                         g_sunshine_host);
                network_connect_clear_retry_app();
                g_is_paired = 0;
                g_last_paired_host[0] = '\0';
                return -1;  /* -1 triggers full re-pair flow in main.c */
            }
            if (selected_appid < 0) {
                /* User pressed Circle (back) - return to host menu */
                pair_log("[STEP 4] User cancelled game selection\n");
                network_connect_clear_retry_app();
                return -2;
            }
            selected_title = game_grid_ui_get_selected_title();
            s_retry_appid = selected_appid;
            strncpy(s_retry_host, g_sunshine_host, sizeof(s_retry_host) - 1);
            s_retry_host[sizeof(s_retry_host) - 1] = '\0';
            if (selected_title && selected_title[0]) {
                strncpy(s_retry_title, selected_title, sizeof(s_retry_title) - 1);
                s_retry_title[sizeof(s_retry_title) - 1] = '\0';
            } else {
                s_retry_title[0] = '\0';
            }
            pair_log("[STEP 4] User selected appid=%d\n", selected_appid);
        }

        /* Start the 60fps connection UI render thread */
        stream_connect_start();
        stream_connect_draw(selected_title, STREAM_PHASE_RTSP);

        /* Launch/resume stream session before RTSP handshake, as required by
         * Sunshine's /launch contract.  Retry once on failure — WiFi packet
         * loss can cause TLS handshake timeouts ~50% of the time. */
        int launch_ret = sunshine_launch_session(selected_appid);
        if (launch_ret != 0 && launch_ret != -401) {
            pair_log("[STEP 4] launch attempt 1 failed (%d), retrying after 2s...\n", launch_ret);
            sceKernelDelayThread(2000 * 1000);
            stream_connect_draw(selected_title, STREAM_PHASE_RTSP);
            launch_ret = sunshine_launch_session(selected_appid);
        }
        if (launch_ret == -401) {
            /* 401: stale pairing already cleared; signal re-pair needed */
            pair_log("[STEP 4] 401 - stale pairing cleared, need re-pair\n");
            network_connect_clear_retry_app();
            stream_connect_stop();
            return -1;  /* -1 triggers full re-pair flow in main.c */
        }
        if (launch_ret < 0) {
            pair_log("[STEP 4] launch step failed\n");
            stream_connect_stop();
            return -3;  /* -3 = infrastructure failure (retryable) */
        }
    }

    /*--- Step 2: RTSP Session -----------------------------------------------*/
    /* Give the server a moment to finish setting up the RTSP listener after
     * /launch.  500ms is enough — longer delays waste time on slow WiFi. */
    sceKernelDelayThread(500 * 1000);
    stream_connect_draw(game_grid_ui_get_selected_title(), STREAM_PHASE_CONTROL);
    ret = rtsp_session();
    if (ret == RTSP_ERR_PLAY_SESSION_DEAD) {
        pair_log("[RTSP] launched session RTSP not reachable yet; keeping same launch for retry\n");
        ret = -1;
    }
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
        if (ret == RTSP_ERR_PLAY_SESSION_DEAD) {
            pair_log("[RTSP] retry RTSP listener still unreachable; one final same-launch retry\n");
            ret = -1;
        }
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
            int cancel_ret;
            int cancel_attempt;
            snprintf(cancel_path, sizeof(cancel_path),
                     "/cancel?uniqueid=%s&uuid=%s",
                     CLIENT_UNIQUE_ID, client_identity_get_uuid());
            cancel_ret = -1;
            for (cancel_attempt = 1; cancel_attempt <= 2; cancel_attempt++) {
                pair_log("[CANCEL] sending %s (attempt %d/2)\n",
                         cancel_path, cancel_attempt);
                cancel_resp[0] = '\0';
                cancel_ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                              cancel_path, cancel_resp, sizeof(cancel_resp));
                if (cancel_ret >= 0) {
                    break;
                }
                if (cancel_attempt < 2) {
                    pair_log("[CANCEL] attempt %d failed (%d), retrying after 3s\n",
                             cancel_attempt, cancel_ret);
                    sceKernelDelayThread(3000 * 1000);
                }
            }
            if (cancel_ret >= 0) {
                pair_log("[CANCEL] response: %.120s\n", cancel_resp);
            } else {
                pair_log("[CANCEL] request failed (%d); host may keep stale RTSP session\n",
                         cancel_ret);
            }
        }
        pair_log("[CANCEL] waiting 8s for host RTSP/session cleanup\n");
        sceKernelDelayThread(8000 * 1000);
        /* Return -3 to distinguish RTSP failure from user cancel (-2) and
         * pairing failure (-1).  Pairing was already successful so
         * g_is_paired should stay set; -3 is retryable in main.c. */
        stream_connect_stop();
        return -3;
    }

    stream_connect_stop();
    network_connect_clear_retry_app();
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
        int cancel_ret;
        snprintf(cancel_path, sizeof(cancel_path),
                 "/cancel?uniqueid=%s&uuid=%s",
                 CLIENT_UNIQUE_ID, client_identity_get_uuid());
        pair_log("[CANCEL-THREAD] Sending explicit abort via %s\n", cancel_path);
        cancel_resp[0] = '\0';
        cancel_ret = https_launch_get(g_sunshine_host, SUNSHINE_HTTPS_PORT,
                                      cancel_path, cancel_resp, sizeof(cancel_resp));
        if (cancel_ret >= 0) {
            pair_log("[CANCEL-THREAD] explicit abort response: %.120s\n", cancel_resp);
        } else {
            pair_log("[CANCEL-THREAD] explicit abort request failed (%d)\n",
                     cancel_ret);
        }
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
