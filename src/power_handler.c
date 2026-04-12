/*
 * power_handler.c - PSP Power Switch Handler for Moonlight
 */

#include <pspkernel.h>
#include <psppower.h>
#include <pspdebug.h>
#include <pspthreadman.h>
#include <pspnet_inet.h>
#include <pspiofilemgr.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

/* SO_NBIO is PSP-specific non-blocking socket option */
#ifndef SO_NBIO
#define SO_NBIO 0x1009
#endif

#include "power_handler.h"
#include "shared.h"
#include "client_identity.h"

extern void network_me_shutdown(void);
extern void network_me_init(PacketRingBuffer *rb);
extern void sw_decoder_thread_shutdown(void);
extern int sw_decoder_thread_init(FrameRingBuffer *rb);
extern void display_shutdown(void);
extern void display_init(void);
extern volatile int me_running;
extern volatile int g_is_paired;
extern SharedState g_shared;
extern void LiStopConnection(void);

static volatile PowerState g_power_state = POWER_STATE_AWAKE;
static int g_power_callback_id = -1;
static SceUID g_power_thread_id = -1;
static volatile int g_power_event = 0;
static SessionToken g_cached_token;
static PowerHandlerStats g_power_stats = {0};
static int g_rtsp_socket = -1;

static int load_session_token(void)
{
    SceUID fd;
    int ret;
    
    fd = sceIoOpen(SESSION_TOKEN_CACHE_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return -1;
    
    ret = sceIoRead(fd, &g_cached_token, sizeof(SessionToken));
    sceIoClose(fd);
    
    if (ret != sizeof(SessionToken)) return -1;
    if (!g_cached_token.is_valid) return -1;
    
    /* Use sceKernelGetSystemTimeWide (u64 us) to avoid 71-minute u32 wrap-around */
    u64 current_time = sceKernelGetSystemTimeWide() / 1000000ULL;
    if (current_time - (u64)g_cached_token.cache_timestamp > 3600ULL) return -1;
    
    return 0;
}

static int save_session_token(void)
{
    SceUID fd;
    int ret;
    
    sceIoMkdir("ms0:/moonlight", 0777);
    
    fd = sceIoOpen(SESSION_TOKEN_CACHE_PATH, PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
    if (fd < 0) return -1;
    
    g_cached_token.cache_timestamp = (u32)(sceKernelGetSystemTimeWide() / 1000000ULL);
    g_cached_token.is_valid = 1;
    
    ret = sceIoWrite(fd, &g_cached_token, sizeof(SessionToken));
    sceIoClose(fd);
    
    return (ret == sizeof(SessionToken)) ? 0 : -1;
}

static int rtsp_reconnect_play(int sock)
{
    char request[512];
    char response[1024];
    int ret;
    int total_recv = 0;
    
    snprintf(request, sizeof(request),
             "PLAY rtsp://%s:%d RTSP/1.0\r\n"
             "CSeq: 1\r\n"
             "User-Agent: psp-moonlight\r\n"
             "Session: %s\r\n"
             "Range: npt=now-\r\n\r\n",
             g_cached_token.server_address,
             g_cached_token.rtsp_port,
             g_cached_token.session_id);
    
    ret = sceNetInetSend(sock, request, strlen(request), 0);
    if (ret < 0) return -1;
    
    memset(response, 0, sizeof(response));
    while (total_recv < sizeof(response) - 1)
    {
        ret = sceNetInetRecv(sock, response + total_recv,
                             sizeof(response) - 1 - total_recv, 0);
        if (ret <= 0) break;
        total_recv += ret;
        if (strstr(response, "\r\n\r\n") != NULL) break;
        sceKernelDelayThread(1000);
    }
    
    return (strstr(response, "200") != NULL) ? 0 : -1;
}

static int quick_reconnect_with_timeout(int timeout_sec)
{
    int sock;
    int ret;
    struct sockaddr_in server_addr;
    struct timeval timeout;
    fd_set writefds;
    int optval;
    socklen_t optlen;
    
    sock = sceNetInetSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) return -1;
    
    optval = 1;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NBIO, &optval, sizeof(optval));
    
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_len    = (unsigned char)sizeof(server_addr);
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(g_cached_token.rtsp_port);
    server_addr.sin_addr.s_addr = inet_addr(g_cached_token.server_address);
    
    ret = sceNetInetConnect(sock, (struct sockaddr *)&server_addr, sizeof(server_addr));
    
    if (ret < 0)
    {
        int err = sceNetInetGetErrno();
        if (err != EINPROGRESS && err != EWOULDBLOCK)
        {
            sceNetInetClose(sock);
            return -1;
        }
        
        FD_ZERO(&writefds);
        FD_SET(sock, &writefds);
        timeout.tv_sec = timeout_sec;
        timeout.tv_usec = 0;
        
        ret = sceNetInetSelect(sock + 1, NULL, &writefds, NULL, &timeout);
        if (ret <= 0)
        {
            sceNetInetClose(sock);
            return -1;
        }
        
        optlen = sizeof(optval);
        sceNetInetGetsockopt(sock, SOL_SOCKET, SO_ERROR, &optval, &optlen);
        if (optval != 0)
        {
            sceNetInetClose(sock);
            return -1;
        }
    }
    
    optval = 0;
    sceNetInetSetsockopt(sock, SOL_SOCKET, SO_NBIO, &optval, sizeof(optval));
    
    return sock;
}

static void handle_suspend(void)
{
    g_power_state = POWER_STATE_SUSPENDING;
    g_power_stats.suspend_count++;
    g_power_stats.last_suspend_time = (u32)(sceKernelGetSystemTimeWide() / 1000ULL);
    
    power_handler_cache_session_token();
    me_running = 0;
    sceKernelDelayThread(100000);
    
    g_power_state = POWER_STATE_SUSPENDED;
}

static void handle_resume(void)
{
    g_power_state = POWER_STATE_RESUMING;
    g_power_stats.resume_count++;
    g_power_stats.last_resume_time = (u32)(sceKernelGetSystemTimeWide() / 1000ULL);
    
    if (g_power_stats.last_suspend_time > 0)
    {
        u32 duration = g_power_stats.last_resume_time - g_power_stats.last_suspend_time;
        g_power_stats.total_suspend_duration_ms += duration;
    }
    
    if (power_handler_quick_reconnect() == 0)
    {
        g_power_stats.quick_reconnect_success++;
        g_power_state = POWER_STATE_AWAKE;
    }
    else
    {
        g_power_stats.quick_reconnect_failed++;
        g_power_state = POWER_STATE_RESUME_FAILED;
    }
}

static int power_callback(int arg1, int arg2, void *common)
{
    if (arg1 == 1) g_power_event = 1;
    else if (arg1 == 2) g_power_event = 2;
    return 0;
}

static int power_thread(SceSize args, void *argp)
{
    g_power_callback_id = sceKernelCreateCallback("power_callback", power_callback, NULL);
    if (g_power_callback_id < 0) return -1;
    
    int ret = scePowerRegisterCallback(0, g_power_callback_id);
    if (ret < 0)
    {
        sceKernelDeleteCallback(g_power_callback_id);
        return -1;
    }
    
    while (1)
    {
        if (g_power_event == 1)
        {
            handle_suspend();
            g_power_event = 0;
        }
        else if (g_power_event == 2)
        {
            handle_resume();
            g_power_event = 0;
        }
        sceKernelDelayThread(100000);
    }
    return 0;
}

int power_handler_init(void)
{
    g_power_state = POWER_STATE_AWAKE;
    memset(&g_power_stats, 0, sizeof(PowerHandlerStats));
    memset(&g_cached_token, 0, sizeof(SessionToken));
    g_cached_token.is_valid = 0;
    
    g_power_thread_id = sceKernelCreateThread("power_thread", power_thread, 0x18, 4096, 0, NULL);
    if (g_power_thread_id < 0) return -1;
    
    int ret = sceKernelStartThread(g_power_thread_id, 0, NULL);
    if (ret < 0)
    {
        sceKernelDeleteThread(g_power_thread_id);
        return -1;
    }
    return 0;
}

void power_handler_shutdown(void)
{
    if (g_power_callback_id >= 0)
    {
        scePowerRegisterCallback(0, -1);
        sceKernelDeleteCallback(g_power_callback_id);
        g_power_callback_id = -1;
    }
    
    if (g_power_thread_id >= 0)
    {
        sceKernelTerminateThread(g_power_thread_id);
        sceKernelDeleteThread(g_power_thread_id);
        g_power_thread_id = -1;
    }
    
    if (g_rtsp_socket >= 0)
    {
        sceNetInetClose(g_rtsp_socket);
        g_rtsp_socket = -1;
    }
    
    g_power_state = POWER_STATE_AWAKE;
}

PowerState power_handler_get_state(void)
{
    return g_power_state;
}

int power_handler_is_suspended(void)
{
    return (g_power_state == POWER_STATE_SUSPENDED ||
            g_power_state == POWER_STATE_SUSPENDING ||
            g_power_state == POWER_STATE_RESUMING);
}

int power_handler_cache_session_token(void)
{
    strncpy(g_cached_token.server_address, "192.168.1.100", sizeof(g_cached_token.server_address) - 1);
    g_cached_token.rtsp_port = 48010;
    g_cached_token.http_port = 47989;
    strncpy(g_cached_token.unique_id, client_identity_get_uid(), sizeof(g_cached_token.unique_id) - 1);
    g_cached_token.width = 1280;
    g_cached_token.height = 720;
    g_cached_token.fps = 30;
    g_cached_token.bitrate = 1000;
    strncpy(g_cached_token.session_id, "session123", sizeof(g_cached_token.session_id) - 1);
    
    return save_session_token();
}

int power_handler_quick_reconnect(void)
{
    if (load_session_token() < 0) return -1;
    
    g_rtsp_socket = quick_reconnect_with_timeout(QUICK_RECONNECT_TIMEOUT_SEC);
    if (g_rtsp_socket < 0) return -1;
    
    int ret = rtsp_reconnect_play(g_rtsp_socket);
    if (ret < 0)
    {
        sceNetInetClose(g_rtsp_socket);
        g_rtsp_socket = -1;
        return -1;
    }
    
    me_running = 1;
    network_me_init(&g_shared.packet_ring);
    sw_decoder_thread_init(&g_shared.frame_ring);
    display_init();
    g_is_paired = 1;
    
    return 0;
}

void power_handler_invalidate_cache(void)
{
    memset(&g_cached_token, 0, sizeof(SessionToken));
    g_cached_token.is_valid = 0;
    sceIoRemove(SESSION_TOKEN_CACHE_PATH);
}

int power_handler_is_cache_valid(void)
{
    SceUID fd = sceIoOpen(SESSION_TOKEN_CACHE_PATH, PSP_O_RDONLY, 0);
    if (fd < 0) return 0;
    sceIoClose(fd);
    return (load_session_token() == 0) ? g_cached_token.is_valid : 0;
}

void power_handler_get_stats(PowerHandlerStats *stats)
{
    if (stats != NULL) memcpy(stats, &g_power_stats, sizeof(PowerHandlerStats));
}

void power_handler_reset_stats(void)
{
    memset(&g_power_stats, 0, sizeof(PowerHandlerStats));
}
