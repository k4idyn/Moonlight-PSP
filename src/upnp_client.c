/*
 * upnp_client.c - Minimal UPnP IGD implementation for PSP Moonlight
 *
 * This module intentionally keeps dependencies small:
 *  - SSDP M-SEARCH for gateway discovery
 *  - HTTP GET for device description
 *  - SOAP AddPortMapping / DeletePortMapping for UDP stream ports
 *
 * It is best-effort only. All failures are non-fatal to the stream session.
 */

#include "upnp_client.h"

#include <pspkernel.h>
#include <pspnet_apctl.h>
#include <pspnet_inet.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

#include "diag_log.h"
#include "net_send.h"

#define upnp_log(fmt, ...) diag_log_write("UPNP", fmt, ##__VA_ARGS__)

#ifndef INADDR_NONE
#define INADDR_NONE ((unsigned long)0xFFFFFFFFUL)
#endif

#define UPNP_SSDP_MCAST            "239.255.255.250"
#define UPNP_SSDP_PORT             1900
#define UPNP_DISCOVERY_TIMEOUT_MS  1500
#define UPNP_CONNECT_TIMEOUT_MS    1200
#define UPNP_RECV_TIMEOUT_MS       1500
#define UPNP_MAX_RESPONSE          4096
#define UPNP_MAX_MAPPED_PORTS      8

static char s_control_url[256] = {0};
static char s_service_type[96] = {0};
static int s_have_control_endpoint = 0;

static unsigned short s_mapped_ports[UPNP_MAX_MAPPED_PORTS];
static int s_mapped_port_count = 0;

static char s_local_ip[16] = {0};

static int ascii_tolower(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

static int strncaseeq(const char *a, const char *b, int n)
{
    int i;
    if (!a || !b || n < 0) {
        return 0;
    }

    for (i = 0; i < n; i++) {
        if (ascii_tolower((unsigned char)a[i]) !=
            ascii_tolower((unsigned char)b[i])) {
            return 0;
        }
    }

    return 1;
}

static int parse_http_url(const char *url,
                          char *host,
                          int host_size,
                          unsigned short *port,
                          char *path,
                          int path_size)
{
    const char *scheme = "http://";
    const char *authority;
    const char *path_start;
    char authority_buf[128];
    int authority_len;
    char *port_sep;
    unsigned short parsed_port = 80;

    if (!url || !host || !path || host_size <= 1 || path_size <= 1) {
        return -1;
    }

    if (strncmp(url, scheme, strlen(scheme)) != 0) {
        return -1;
    }

    authority = url + strlen(scheme);
    path_start = strchr(authority, '/');
    if (!path_start) {
        path_start = authority + strlen(authority);
    }

    authority_len = (int)(path_start - authority);
    if (authority_len <= 0 || authority_len >= (int)sizeof(authority_buf)) {
        return -1;
    }

    memcpy(authority_buf, authority, authority_len);
    authority_buf[authority_len] = '\0';

    port_sep = strrchr(authority_buf, ':');
    if (port_sep) {
        char *endptr = NULL;
        long v;
        *port_sep = '\0';
        v = strtol(port_sep + 1, &endptr, 10);
        if (endptr == port_sep + 1 || v <= 0 || v > 65535) {
            return -1;
        }
        parsed_port = (unsigned short)v;
    }

    if (authority_buf[0] == '\0') {
        return -1;
    }

    strncpy(host, authority_buf, host_size - 1);
    host[host_size - 1] = '\0';

    if (path_start[0] != '\0') {
        strncpy(path, path_start, path_size - 1);
        path[path_size - 1] = '\0';
    } else {
        strncpy(path, "/", path_size - 1);
        path[path_size - 1] = '\0';
    }

    if (port) {
        *port = parsed_port;
    }

    return 0;
}

static int build_http_origin(const char *url, char *origin, int origin_size)
{
    char host[96];
    char path[192];
    unsigned short port = 80;

    if (!origin || origin_size <= 1) {
        return -1;
    }

    if (parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        return -1;
    }

    snprintf(origin, origin_size, "http://%s:%u", host, (unsigned)port);
    origin[origin_size - 1] = '\0';
    return 0;
}

static int http_status_code(const char *response)
{
    int code = 0;

    if (!response) {
        return -1;
    }

    if (sscanf(response, "HTTP/%*d.%*d %d", &code) == 1) {
        return code;
    }

    if (sscanf(response, "HTTP/%*d %d", &code) == 1) {
        return code;
    }

    return -1;
}

static int extract_http_header_value(const char *http,
                                     const char *header_name,
                                     char *out,
                                     int out_size)
{
    const char *p;
    int header_len;

    if (!http || !header_name || !out || out_size <= 1) {
        return -1;
    }

    header_len = (int)strlen(header_name);
    p = http;

    while (*p) {
        const char *line_end = strstr(p, "\r\n");
        int line_len;
        const char *colon;
        const char *value;
        int value_len;

        if (!line_end) {
            line_end = p + strlen(p);
        }

        line_len = (int)(line_end - p);
        if (line_len <= 0) {
            break;
        }

        colon = memchr(p, ':', (size_t)line_len);
        if (colon) {
            int key_len = (int)(colon - p);
            if (key_len == header_len && strncaseeq(p, header_name, key_len)) {
                value = colon + 1;
                while ((value - p) < line_len && (*value == ' ' || *value == '\t')) {
                    value++;
                }
                value_len = line_len - (int)(value - p);
                while (value_len > 0 &&
                       (value[value_len - 1] == ' ' || value[value_len - 1] == '\t')) {
                    value_len--;
                }

                if (value_len >= out_size) {
                    value_len = out_size - 1;
                }
                memcpy(out, value, (size_t)value_len);
                out[value_len] = '\0';
                return 0;
            }
        }

        if (*line_end == '\0') {
            break;
        }
        p = line_end + 2;
    }

    return -1;
}

static int extract_xml_value(const char *xml,
                             const char *tag,
                             char *out,
                             int out_size)
{
    char open_tag[48];
    char close_tag[52];
    const char *start;
    const char *end;
    int copy_len;

    if (!xml || !tag || !out || out_size <= 1) {
        return -1;
    }

    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    start = strstr(xml, open_tag);
    if (!start) {
        return -1;
    }
    start += strlen(open_tag);

    end = strstr(start, close_tag);
    if (!end || end <= start) {
        return -1;
    }

    copy_len = (int)(end - start);
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }

    memcpy(out, start, (size_t)copy_len);
    out[copy_len] = '\0';
    return 0;
}

static int extract_xml_value_between(const char *start,
                                     const char *end_bound,
                                     const char *tag,
                                     char *out,
                                     int out_size)
{
    char open_tag[48];
    char close_tag[52];
    const char *start_tag;
    const char *value_start;
    const char *value_end;
    int copy_len;

    if (!start || !end_bound || !tag || !out || out_size <= 1 || end_bound <= start) {
        return -1;
    }

    snprintf(open_tag, sizeof(open_tag), "<%s>", tag);
    snprintf(close_tag, sizeof(close_tag), "</%s>", tag);

    start_tag = strstr(start, open_tag);
    if (!start_tag || start_tag >= end_bound) {
        return -1;
    }

    value_start = start_tag + strlen(open_tag);
    value_end = strstr(value_start, close_tag);
    if (!value_end || value_end > end_bound || value_end <= value_start) {
        return -1;
    }

    copy_len = (int)(value_end - value_start);
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }

    memcpy(out, value_start, (size_t)copy_len);
    out[copy_len] = '\0';
    return 0;
}

static int tcp_connect_timeout(const char *ipv4, unsigned short port, int timeout_ms)
{
    int sock;
    int nb;
    int ret;
    struct sockaddr_in addr;

    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }

    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    memset(&addr, 0, sizeof(addr));
    addr.sin_len = (unsigned char)sizeof(addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(ipv4);
    if (addr.sin_addr.s_addr == INADDR_NONE) {
        sceNetInetClose(sock);
        return -1;
    }

    ret = sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr));
    if (ret < 0) {
        int err = sceNetInetGetErrno();
        if (err != EINPROGRESS && err != EALREADY &&
            err != EAGAIN && err != EWOULDBLOCK) {
            sceNetInetClose(sock);
            return -1;
        }
    }

    if (ret != 0) {
        fd_set wfds;
        struct timeval tv;
        int so_error = -1;
        socklen_t so_len = sizeof(so_error);

        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);

        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;

        ret = sceNetInetSelect(sock + 1, NULL, &wfds, NULL, &tv);
        if (ret <= 0) {
            sceNetInetClose(sock);
            return -1;
        }

        sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &so_len);
        if (so_error != 0) {
            sceNetInetClose(sock);
            return -1;
        }
    }

    nb = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    return sock;
}

static int tcp_send_all(int sock, const char *data, int data_len)
{
    int send_errno = 0;
    int send_off = 0;

    if (net_send_all_psp(sock, data, data_len,
                         0, 0, &send_errno, &send_off) != 0) {
        upnp_log("tcp send failed sent=%d/%d errno=%d\n",
                 send_off, data_len, send_errno);
        return -1;
    }

    return 0;
}

static int tcp_recv_with_timeout(int sock, char *out, int out_size, int timeout_ms)
{
    int total = 0;
    u32 start_ms;
    int nb = 1;

    if (!out || out_size <= 1) {
        return -1;
    }

    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    start_ms = sceKernelGetSystemTimeLow() / 1000;
    while (total < out_size - 1) {
        int ret = sceNetInetRecv(sock, out + total, out_size - 1 - total, 0);
        if (ret > 0) {
            total += ret;
            start_ms = sceKernelGetSystemTimeLow() / 1000;
            continue;
        }

        if (ret == 0) {
            break;
        }

        {
            int err = sceNetInetGetErrno();
            u32 now_ms = sceKernelGetSystemTimeLow() / 1000;

            if (err != EAGAIN && err != EWOULDBLOCK) {
                break;
            }

            if ((int)(now_ms - start_ms) > timeout_ms) {
                break;
            }

            sceKernelDelayThread(10 * 1000);
        }
    }

    out[total] = '\0';
    return total;
}

static int http_get_raw(const char *url, char *response, int response_size)
{
    char host[96];
    unsigned short port;
    char path[192];
    char request[512];
    int sock;
    int code;

    if (parse_http_url(url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        return -1;
    }

    sock = tcp_connect_timeout(host, port, UPNP_CONNECT_TIMEOUT_MS);
    if (sock < 0) {
        return -1;
    }

    snprintf(request, sizeof(request),
             "GET %s HTTP/1.1\r\n"
             "Host: %s:%u\r\n"
             "Connection: close\r\n"
             "\r\n",
             path, host, (unsigned)port);

    if (tcp_send_all(sock, request, (int)strlen(request)) < 0) {
        sceNetInetClose(sock);
        return -1;
    }

    if (tcp_recv_with_timeout(sock, response, response_size, UPNP_RECV_TIMEOUT_MS) <= 0) {
        sceNetInetClose(sock);
        return -1;
    }

    sceNetInetClose(sock);
    code = http_status_code(response);
    return code;
}

static int soap_action_request(const char *action,
                               const char *action_params,
                               char *response,
                               int response_size)
{
    char host[96];
    unsigned short port;
    char path[192];
    char soap_body[1536];
    char request[2304];
    int body_len;
    int req_len;
    int sock;
    int code;

    if (!s_have_control_endpoint) {
        return -1;
    }

    if (parse_http_url(s_control_url, host, sizeof(host), &port, path, sizeof(path)) < 0) {
        return -1;
    }

    body_len = snprintf(soap_body, sizeof(soap_body),
                        "<?xml version=\"1.0\"?>\r\n"
                        "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
                        "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">\r\n"
                        "<s:Body>\r\n"
                        "<u:%s xmlns:u=\"%s\">\r\n"
                        "%s"
                        "</u:%s>\r\n"
                        "</s:Body>\r\n"
                        "</s:Envelope>\r\n",
                        action, s_service_type, action_params, action);

    if (body_len <= 0 || body_len >= (int)sizeof(soap_body)) {
        return -1;
    }

    req_len = snprintf(request, sizeof(request),
                       "POST %s HTTP/1.1\r\n"
                       "Host: %s:%u\r\n"
                       "Content-Type: text/xml; charset=\"utf-8\"\r\n"
                       "SOAPAction: \"%s#%s\"\r\n"
                       "Connection: close\r\n"
                       "Content-Length: %d\r\n"
                       "\r\n"
                       "%s",
                       path,
                       host,
                       (unsigned)port,
                       s_service_type,
                       action,
                       body_len,
                       soap_body);

    if (req_len <= 0 || req_len >= (int)sizeof(request)) {
        return -1;
    }

    sock = tcp_connect_timeout(host, port, UPNP_CONNECT_TIMEOUT_MS);
    if (sock < 0) {
        return -1;
    }

    if (tcp_send_all(sock, request, req_len) < 0) {
        sceNetInetClose(sock);
        return -1;
    }

    if (tcp_recv_with_timeout(sock, response, response_size, UPNP_RECV_TIMEOUT_MS) <= 0) {
        sceNetInetClose(sock);
        return -1;
    }

    sceNetInetClose(sock);

    code = http_status_code(response);
    return code;
}

static void clear_control_endpoint_cache(void)
{
    s_have_control_endpoint = 0;
    s_control_url[0] = '\0';
    s_service_type[0] = '\0';
}

static int ssdp_discover_location(char *location, int location_size)
{
    static const char *search_targets[] = {
        "urn:schemas-upnp-org:service:WANIPConnection:1",
        "urn:schemas-upnp-org:service:WANPPPConnection:1",
        "urn:schemas-upnp-org:device:InternetGatewayDevice:1"
    };

    int sock;
    int nb;
    struct sockaddr_in dst;
    int i;
    int probes_sent = 0;
    u32 start_ms;

    if (!location || location_size <= 1) {
        return -1;
    }

    location[0] = '\0';

    sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        return -1;
    }

    nb = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NONBLOCK, &nb, sizeof(nb));

    memset(&dst, 0, sizeof(dst));
    dst.sin_len = (unsigned char)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port = htons(UPNP_SSDP_PORT);
    dst.sin_addr.s_addr = inet_addr(UPNP_SSDP_MCAST);

    for (i = 0; i < (int)(sizeof(search_targets) / sizeof(search_targets[0])); i++) {
        char req[320];
        int req_len;

        req_len = snprintf(req, sizeof(req),
                           "M-SEARCH * HTTP/1.1\r\n"
                           "HOST: 239.255.255.250:1900\r\n"
                           "MAN: \"ssdp:discover\"\r\n"
                           "MX: 1\r\n"
                           "ST: %s\r\n"
                           "\r\n",
                           search_targets[i]);

        if (req_len > 0 && req_len < (int)sizeof(req)) {
            int tx = (int)sceNetInetSendto(sock, req, req_len, 0,
                                           (struct sockaddr *)&dst, sizeof(dst));
            if (tx > 0) {
                probes_sent++;
            } else {
#ifndef RETAIL_BUILD
                int err = sceNetInetGetErrno();
                if (i < 2) {
                    upnp_log("SSDP probe send failed (st=%s errno=%d)\n",
                             search_targets[i], err);
                }
#endif
            }
        }
    }

    if (probes_sent <= 0) {
        sceNetInetClose(sock);
        return -1;
    }

    start_ms = sceKernelGetSystemTimeLow() / 1000;
    while ((sceKernelGetSystemTimeLow() / 1000) - start_ms <= UPNP_DISCOVERY_TIMEOUT_MS) {
        char resp[UPNP_MAX_RESPONSE];
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n;

        memset(&from, 0, sizeof(from));
        from.sin_len = (unsigned char)sizeof(from);
        n = sceNetInetRecvfrom(sock,
                               resp,
                               sizeof(resp) - 1,
                               0,
                               (struct sockaddr *)&from,
                               &from_len);
        if (n > 0) {
            resp[n] = '\0';
            if (extract_http_header_value(resp, "LOCATION", location, location_size) == 0) {
                sceNetInetClose(sock);
                return 0;
            }
        } else if (n < 0) {
            int err = sceNetInetGetErrno();
            if (err != EAGAIN && err != EWOULDBLOCK) {
                break;
            }
        }

        sceKernelDelayThread(20 * 1000);
    }

    sceNetInetClose(sock);
    return -1;
}

static int compose_control_url(const char *desc_url,
                               const char *url_base,
                               const char *control_path,
                               char *out,
                               int out_size)
{
    char origin[160];
    const char *base_ref;

    if (!desc_url || !control_path || !out || out_size <= 1) {
        return -1;
    }

    if (strncmp(control_path, "http://", 7) == 0) {
        strncpy(out, control_path, out_size - 1);
        out[out_size - 1] = '\0';
        return 0;
    }

    base_ref = (url_base && url_base[0]) ? url_base : desc_url;
    if (build_http_origin(base_ref, origin, sizeof(origin)) < 0) {
        return -1;
    }

    if (control_path[0] == '/') {
        snprintf(out, out_size, "%s%s", origin, control_path);
    } else {
        snprintf(out, out_size, "%s/%s", origin, control_path);
    }

    out[out_size - 1] = '\0';
    return 0;
}

static int parse_igd_description(const char *xml, const char *desc_url)
{
    const char *service_start;
    const char *service_end;
    char control_path[160];
    char service_type[96];
    char url_base[160];

    if (!xml || !desc_url) {
        return -1;
    }

    service_start = strstr(xml, "<serviceType>urn:schemas-upnp-org:service:WANIPConnection:");
    if (!service_start) {
        service_start = strstr(xml, "<serviceType>urn:schemas-upnp-org:service:WANPPPConnection:");
    }
    if (!service_start) {
        return -1;
    }

    service_end = strstr(service_start, "</service>");
    if (!service_end) {
        return -1;
    }

    if (extract_xml_value_between(service_start,
                                  service_end,
                                  "serviceType",
                                  service_type,
                                  sizeof(service_type)) < 0) {
        return -1;
    }

    if (extract_xml_value_between(service_start,
                                  service_end,
                                  "controlURL",
                                  control_path,
                                  sizeof(control_path)) < 0) {
        return -1;
    }

    url_base[0] = '\0';
    extract_xml_value(xml, "URLBase", url_base, sizeof(url_base));

    if (compose_control_url(desc_url,
                            url_base,
                            control_path,
                            s_control_url,
                            sizeof(s_control_url)) < 0) {
        return -1;
    }

    strncpy(s_service_type, service_type, sizeof(s_service_type) - 1);
    s_service_type[sizeof(s_service_type) - 1] = '\0';
    s_have_control_endpoint = 1;

    upnp_log("Using control URL: %s\n", s_control_url);
    upnp_log("Using service type: %s\n", s_service_type);
    return 0;
}

static int discover_control_endpoint(void)
{
    char location[256];
    char desc_resp[UPNP_MAX_RESPONSE];
    char *xml_body;
    int status;

    if (s_have_control_endpoint) {
        return 0;
    }

    if (ssdp_discover_location(location, sizeof(location)) < 0) {
        upnp_log("No UPnP gateway responded to SSDP discovery\n");
        return -1;
    }

    upnp_log("Gateway description URL: %s\n", location);

    status = http_get_raw(location, desc_resp, sizeof(desc_resp));
    if (status < 200 || status >= 300) {
        upnp_log("Gateway description fetch failed (HTTP %d)\n", status);
        return -1;
    }

    xml_body = strstr(desc_resp, "\r\n\r\n");
    if (xml_body) {
        xml_body += 4;
    } else {
        xml_body = desc_resp;
    }

    if (parse_igd_description(xml_body, location) < 0) {
        upnp_log("Failed to parse IGD service control URL\n");
        return -1;
    }

    return 0;
}

static int get_local_ipv4(char *out, int out_size)
{
    union SceNetApctlInfo info;

    if (!out || out_size <= 1) {
        return -1;
    }

    memset(&info, 0, sizeof(info));
    if (sceNetApctlGetInfo(8, &info) != 0 || info.ip[0] == '\0') {
        return -1;
    }

    strncpy(out, info.ip, out_size - 1);
    out[out_size - 1] = '\0';
    return 0;
}

static int parse_ipv4_octets(const char *ip,
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

static int is_public_ipv4_target(const char *host)
{
    unsigned int a, b, c, d;

    if (parse_ipv4_octets(host, &a, &b, &c, &d) < 0) {
        /* Unknown host format: don't block UPnP attempts. */
        return 1;
    }

    (void)c;
    (void)d;

    if (a == 10 || a == 127 || (a == 169 && b == 254)) {
        return 0;
    }

    if (a == 192 && b == 168) {
        return 0;
    }

    if (a == 172 && b >= 16 && b <= 31) {
        return 0;
    }

    if (a == 100 && b >= 64 && b <= 127) {
        return 0;
    }

    return 1;
}

static int add_unique_port(unsigned short *ports, int *count, int max_count, unsigned short port)
{
    int i;

    if (!ports || !count || *count >= max_count || port == 0) {
        return -1;
    }

    for (i = 0; i < *count; i++) {
        if (ports[i] == port) {
            return 0;
        }
    }

    ports[*count] = port;
    (*count)++;
    return 0;
}

static int upnp_add_udp_mapping(unsigned short port)
{
    char params[640];
    char response[UPNP_MAX_RESPONSE];
    char desc[48];
    int status;

    snprintf(desc, sizeof(desc), "MoonlightPSP-%u", (unsigned)port);

    snprintf(params, sizeof(params),
             "<NewRemoteHost></NewRemoteHost>\r\n"
             "<NewExternalPort>%u</NewExternalPort>\r\n"
             "<NewProtocol>UDP</NewProtocol>\r\n"
             "<NewInternalPort>%u</NewInternalPort>\r\n"
             "<NewInternalClient>%s</NewInternalClient>\r\n"
             "<NewEnabled>1</NewEnabled>\r\n"
             "<NewPortMappingDescription>%s</NewPortMappingDescription>\r\n"
             "<NewLeaseDuration>0</NewLeaseDuration>\r\n",
             (unsigned)port,
             (unsigned)port,
             s_local_ip,
             desc);

    status = soap_action_request("AddPortMapping", params, response, sizeof(response));
    if (status >= 200 && status < 300) {
        return 0;
    }

    /* 718/ConflictInMappingEntry means the mapping already exists. */
    if (strstr(response, "718") || strstr(response, "ConflictInMappingEntry")) {
        return 0;
    }

    upnp_log("AddPortMapping failed for UDP %u (HTTP %d)\n", (unsigned)port, status);
    return -1;
}

static int map_requested_ports(const unsigned short *requested_ports, int requested_count)
{
    int i;
    int mapped = 0;

    for (i = 0; i < requested_count; i++) {
        if (upnp_add_udp_mapping(requested_ports[i]) == 0) {
            if (s_mapped_port_count < UPNP_MAX_MAPPED_PORTS) {
                s_mapped_ports[s_mapped_port_count++] = requested_ports[i];
            }
            mapped++;
        }
    }

    return mapped;
}

static int upnp_delete_udp_mapping(unsigned short port)
{
    char params[320];
    char response[UPNP_MAX_RESPONSE];
    int status;

    snprintf(params, sizeof(params),
             "<NewRemoteHost></NewRemoteHost>\r\n"
             "<NewExternalPort>%u</NewExternalPort>\r\n"
             "<NewProtocol>UDP</NewProtocol>\r\n",
             (unsigned)port);

    status = soap_action_request("DeletePortMapping", params, response, sizeof(response));
    if (status >= 200 && status < 300) {
        return 0;
    }

    /* Removing a non-existent mapping is effectively success for cleanup. */
    if (strstr(response, "714") || strstr(response, "NoSuchEntryInArray")) {
        return 0;
    }

    upnp_log("DeletePortMapping failed for UDP %u (HTTP %d)\n", (unsigned)port, status);
    return -1;
}

int upnp_prepare_stream_mappings(const char *target_host,
                                 unsigned short video_port,
                                 int audio_enabled,
                                 unsigned short audio_port)
{
    unsigned short requested_ports[6];
    int requested_count = 0;
    int mapped = 0;

    /* Always clear any stale mappings from a previous stream first. */
    upnp_remove_stream_mappings();

    if (!target_host || !target_host[0] || video_port == 0) {
        return -1;
    }

    if (!is_public_ipv4_target(target_host)) {
        upnp_log("Skipping UPnP for private/LAN target %s\n", target_host);
        return 0;
    }

    if (get_local_ipv4(s_local_ip, sizeof(s_local_ip)) < 0) {
        upnp_log("Unable to read local PSP IP address\n");
        return -2;
    }

    if (discover_control_endpoint() < 0) {
        return -3;
    }

    add_unique_port(requested_ports, &requested_count,
                    (int)(sizeof(requested_ports) / sizeof(requested_ports[0])),
                    video_port);
    if (video_port < 65535) {
        add_unique_port(requested_ports, &requested_count,
                        (int)(sizeof(requested_ports) / sizeof(requested_ports[0])),
                        (unsigned short)(video_port + 1));
    }

    if (audio_enabled && audio_port > 0) {
        add_unique_port(requested_ports, &requested_count,
                        (int)(sizeof(requested_ports) / sizeof(requested_ports[0])),
                        audio_port);
        if (audio_port < 65535) {
            add_unique_port(requested_ports, &requested_count,
                            (int)(sizeof(requested_ports) / sizeof(requested_ports[0])),
                            (unsigned short)(audio_port + 1));
        }
    }

    mapped = map_requested_ports(requested_ports, requested_count);

    if (mapped <= 0) {
        upnp_log("Initial mapping attempt failed; rediscovering IGD endpoint\n");
        clear_control_endpoint_cache();
        if (discover_control_endpoint() == 0) {
            mapped = map_requested_ports(requested_ports, requested_count);
        }
    }

    if (mapped <= 0) {
        return -4;
    }

    upnp_log("Mapped %d UDP port(s) for remote streaming\n", mapped);
    return mapped;
}

void upnp_remove_stream_mappings(void)
{
    int i;

    if (s_mapped_port_count <= 0) {
        return;
    }

    if (!s_have_control_endpoint) {
        upnp_log("No cached control endpoint during cleanup; forgetting %d mapping entries\n",
                 s_mapped_port_count);
        s_mapped_port_count = 0;
        memset(s_mapped_ports, 0, sizeof(s_mapped_ports));
        return;
    }

    for (i = s_mapped_port_count - 1; i >= 0; i--) {
        upnp_delete_udp_mapping(s_mapped_ports[i]);
    }

    upnp_log("Removed %d UDP UPnP mapping(s)\n", s_mapped_port_count);
    s_mapped_port_count = 0;
    memset(s_mapped_ports, 0, sizeof(s_mapped_ports));
}
