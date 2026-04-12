/*
 * audio_thread.c - Moonlight Audio Receiver and Playback Thread
 *
 * Receives Moonlight/Sunshine RTP PCM audio on UDP port 48000 and plays
 * it back through the PSP hardware audio channel at 48000 Hz stereo.
 *
 * Thread model:
 *   Network Thread → audio_push_frame() → AudioRingBuffer
 *                                                    ↓
 *                             sceAudioOutputBlocking (audio_thread)
 *
 * All sceAudio calls are confined to s_audio_tid so that
 * sceAudioOutputBlocking never blocks any other thread.
 */

#include "audio_thread.h"

#include <string.h>
#include <stdio.h>

#include <pspkernel.h>
#include <pspthreadman.h>
#include <pspaudio.h>
#include <pspnet_inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#include "moonlight_ports.h"
#include "stream_crypto.h"
#include "opus_decode_psp.h"
#include "diag_log.h"

#include <pspiofilemgr.h>

/*--------------------------------------------------------------------------
 * RTP header (RFC 3550 fixed 12-byte header only)
 *--------------------------------------------------------------------------*/
typedef struct {
    u8  version_p_x_cc;  /* V=2, P, X, CC */
    u8  marker_pt;       /* M, PT          */
    u16 seq;
    u32 timestamp;
    u32 ssrc;
} __attribute__((packed)) RtpHeader;

#define RTP_HDR_SIZE  sizeof(RtpHeader)
#define RTP_PT_PCMS16 10   /* PCM 16-bit signed, network byte order */

/*--------------------------------------------------------------------------
 * Module state
 *--------------------------------------------------------------------------*/
static AudioRingBuffer s_ring;
static AudioStats      s_stats;

static SceUID  s_audio_tid  = -1;
static int     s_audio_chan = -1;         /* sceAudio channel handle */
static int     s_udp_sock   = -1;
static volatile int s_running = 0;

/* Silence frame played on underrun — keeps DMA engine from garbage output */
static int16_t s_silence[AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS];

static int s_udp_sock_rtcp = -1;
static unsigned short s_bound_audio_port = 0;


/* Audio ping thread */
static SceUID s_ping_tid = -1;
static volatile int s_ping_running = 0;

static void stop_audio_ping_thread(void)
{
    s_ping_running = 0;
    if (s_ping_tid >= 0) {
        SceUInt timeout_us = 2000000;
        sceKernelWaitThreadEnd(s_ping_tid, &timeout_us);
        sceKernelDeleteThread(s_ping_tid);
        s_ping_tid = -1;
    }
}

/* Audio stream params from RTSP SETUP (network_connect.c) */
extern int  g_audio_server_port;
extern char g_audio_ping_payload[17];
extern char g_video_server_ip[64];

/* avRiKeyId from launch/resume — needed for per-packet audio IV */
extern unsigned int g_av_ri_key_id;

/* Local bind IP from network_connect.c (empty = INADDR_ANY for real hardware) */
extern char g_local_bind_ip[16];

/* SS_PING packet format (same as video) */
typedef struct {
    char     payload[16];
    uint32_t seq_be;
} AudioSsPing;

typedef char _audio_ss_ping_size_check[(sizeof(AudioSsPing) == 20) ? 1 : -1];

#include "diag_log.h"
#define audio_log(fmt, ...) diag_log_write("AUDIO", fmt, ##__VA_ARGS__)

/*--------------------------------------------------------------------------
 * Ring-buffer helpers (lock-free SPSC)
 *--------------------------------------------------------------------------*/
static int ring_full(void)
{
    return ((s_ring.head - s_ring.tail) >= AUDIO_RING_SLOTS);
}

static int ring_empty(void)
{
    return (s_ring.head == s_ring.tail);
}

/* 
 * ring_push_pcm - copy exactly AUDIO_CHUNK_SAMPLES×2 int16 from src into
 * the next free slot and advance head.  Returns 0 on success, -1 if full.
 */
static int ring_push_pcm(const int16_t *src)
{
    if (ring_full()) {
        return -1;
    }
    u32 slot = s_ring.head % AUDIO_RING_SLOTS;
    memcpy(s_ring.pcm[slot], src,
           AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
    /* publish: ordered write — MIPS weak-memory model requires explicit
       sceKernelDcacheWritebackAll() before signalling, but the sceAudio
       DMA cache-coherency is handled by the audio hardware path.
       A simple compiler barrier suffices for the SPSC ring. */
    __asm__ volatile("" ::: "memory");
    s_ring.head++;
    return 0;
}

/* ring_pop_pcm - copy one slot into dst and advance tail.  Returns 0 or -1. */
static int ring_pop_pcm(int16_t *dst)
{
    if (ring_empty()) {
        return -1;
    }
    u32 slot = s_ring.tail % AUDIO_RING_SLOTS;
    memcpy(dst, s_ring.pcm[slot],
           AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS * sizeof(int16_t));
    __asm__ volatile("" ::: "memory");
    s_ring.tail++;
    return 0;
}

/*--------------------------------------------------------------------------
 * Audio Ping Thread
 *
 * Sends SS_PING to Sunshine's audio port every 500 ms so it knows
 * where to send audio RTP.  Must start BEFORE RTSP PLAY per GFE 3.22
 * compatibility (Sunshine also requires it).
 *--------------------------------------------------------------------------*/
static int audio_ping_thread_func(SceSize args, void *argp)
{
    struct sockaddr_in dst;
    AudioSsPing pkt;
    char legacy_ping[] = { 0x50, 0x49, 0x4E, 0x47 };
    uint32_t seq = 0;

    memset(&dst, 0, sizeof(dst));
    dst.sin_len    = (unsigned char)sizeof(dst);
    dst.sin_family = AF_INET;
    dst.sin_port   = htons((unsigned short)g_audio_server_port);
    dst.sin_addr.s_addr = inet_addr(g_video_server_ip);

    audio_log("[AUDIO PING] started, target=%s:%d\n",
              g_video_server_ip, g_audio_server_port);

    while (s_ping_running) {
        int sent;
        seq++;

        if (g_audio_ping_payload[0] != '\0') {
            memcpy(pkt.payload, g_audio_ping_payload, 16);
            pkt.seq_be = htonl(seq);
            sent = (int)sceNetInetSendto(s_udp_sock, &pkt, sizeof(pkt), 0,
                                         (struct sockaddr *)&dst, sizeof(dst));
        } else {
            sent = (int)sceNetInetSendto(s_udp_sock, legacy_ping, 4, 0,
                                         (struct sockaddr *)&dst, sizeof(dst));
        }

        if (seq <= 3 || (seq % 20) == 0)
            audio_log("[AUDIO PING] #%u sent=%d\n", seq, sent);

        /* Diagnose persistent audio sendto failures */
        if (sent < 0 && seq <= 5) {
            int err = sceNetInetGetErrno();
            audio_log("[AUDIO PING] sendto errno=%d sock=%d port=%d ip=%s\n",
                      err, s_udp_sock, g_audio_server_port, g_video_server_ip);
        }

        sceKernelDelayThread(500000); /* 500 ms */
    }

    sceKernelExitDeleteThread(0);
    return 0;
}

static int start_audio_ping_thread(void)
{
    int ret;

    if (s_ping_running && s_ping_tid >= 0) {
        return 0;
    }

    if (s_udp_sock < 0) {
        if (audio_thread_reserve_client_port(NULL) < 0) {
            audio_log("[AUDIO PING] reserve socket failed before ping start\n");
            return -1;
        }
    }

    s_ping_running = 1;
    s_ping_tid = sceKernelCreateThread(
        "audio_ping", audio_ping_thread_func,
        0x1C, 0x2000, PSP_THREAD_ATTR_USER, NULL);
    if (s_ping_tid < 0) {
        audio_log("[AUDIO PING] thread create failed: %d\n", s_ping_tid);
        s_ping_running = 0;
        s_ping_tid = -1;
        return -1;
    }

    ret = sceKernelStartThread(s_ping_tid, 0, NULL);
    if (ret < 0) {
        audio_log("[AUDIO PING] thread start failed: %d\n", ret);
        sceKernelDeleteThread(s_ping_tid);
        s_ping_tid = -1;
        s_ping_running = 0;
        return -1;
    }

    return 0;
}

/*--------------------------------------------------------------------------
 * RTP receive helpers
 *--------------------------------------------------------------------------*/

/* audio_thread_reserve_client_port - bind consecutive UDP sockets for audio
 *
 * Uses ephemeral port (port 0) so the OS assigns a free port, avoiding any
 * fixed-port collision with Sunshine/Apollo on the same host.  RTCP is then
 * bound explicitly to data_port+1 to guarantee consecutive Data/RTCP ports. */

int audio_thread_reserve_client_port(unsigned short *out_port)
{
    if (s_udp_sock >= 0) {
        if (out_port) *out_port = s_bound_audio_port;
        return 0;
    }

    /* Bind data socket to ephemeral port (port 0), capture assigned port via
     * getsockname(), then bind RTCP to port+1 explicitly so that consecutive
     * Data/RTCP ports are guaranteed without any fixed-port collision. */
    int attempt;
    for (attempt = 0; attempt < 20; attempt++) {
        s_udp_sock = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        s_udp_sock_rtcp = sceNetInetSocket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (s_udp_sock < 0 || s_udp_sock_rtcp < 0) {
            if (s_udp_sock >= 0) sceNetInetClose(s_udp_sock);
            if (s_udp_sock_rtcp >= 0) sceNetInetClose(s_udp_sock_rtcp);
            return -1;
        }

        int reuse = 1;
        sceNetInetSetsockopt(s_udp_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        sceNetInetSetsockopt(s_udp_sock_rtcp, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in local;
        memset(&local, 0, sizeof(local));
        local.sin_len         = (u8)sizeof(local);
        local.sin_family      = AF_INET;
        if (g_local_bind_ip[0] != '\0') {
            local.sin_addr.s_addr = inet_addr(g_local_bind_ip);
        } else {
            local.sin_addr.s_addr = htonl(INADDR_ANY);
        }
        local.sin_port = 0; /* ephemeral: let the OS assign the port */

        if (sceNetInetBind(s_udp_sock, (struct sockaddr *)&local, sizeof(local)) != 0) {
            sceNetInetClose(s_udp_sock);
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock = -1;
            s_udp_sock_rtcp = -1;
            return -1;
        }

        /* Capture the OS-assigned port via getsockname() */
        struct sockaddr_in name;
        socklen_t namelen = sizeof(name);
        memset(&name, 0, sizeof(name));
        name.sin_len = (u8)sizeof(name);
        if (sceNetInetGetsockname(s_udp_sock, (struct sockaddr *)&name, &namelen) != 0
                || name.sin_port == 0) {
            sceNetInetClose(s_udp_sock);
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock = -1;
            s_udp_sock_rtcp = -1;
            return -1;
        }
        unsigned short data_port = ntohs(name.sin_port);

        /* Avoid port 65535 since data_port + 1 would wrap to 0. */
        if (data_port == 65535) {
            sceNetInetClose(s_udp_sock);
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock = -1;
            s_udp_sock_rtcp = -1;
            continue;
        }

        /* Bind RTCP socket to the consecutive port (data_port + 1) */
        local.sin_port = htons(data_port + 1);
        if (sceNetInetBind(s_udp_sock_rtcp, (struct sockaddr *)&local, sizeof(local)) == 0) {
            s_bound_audio_port = data_port;
            if (out_port) *out_port = s_bound_audio_port;
            return 0;
        }

        /* RTCP port P+1 was in use — retry for a different ephemeral port. */
        sceNetInetClose(s_udp_sock);
        sceNetInetClose(s_udp_sock_rtcp);
        s_udp_sock = -1;
        s_udp_sock_rtcp = -1;
    }

    return -1;
}

int audio_thread_start_ping_only(void)
{
    return start_audio_ping_thread();
}

/*--------------------------------------------------------------------------
 * Audio playback thread
 *
 * Receives encrypted Opus RTP packets from Sunshine, decrypts with AES-CBC,
 * decodes Opus to PCM S16LE, and feeds AUDIO_CHUNK_SAMPLES frames to
 * sceAudioOutputBlocking.
 *--------------------------------------------------------------------------*/
static int audio_thread_func(SceSize args, void *argp)
{
    u8 pkt_buf[AUDIO_MAX_RTP_SIZE];
    int16_t pcm_decode_buf[AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS];

    while (s_running) {
        /* --- receive one RTP datagram --- */
        int received = sceNetInetRecv(s_udp_sock, pkt_buf, sizeof(pkt_buf), 0);
        if (received <= 0) {
            continue;
        }

        /* Validate minimum RTP size */
        if ((u32)received <= RTP_HDR_SIZE) {
            continue;
        }

        const RtpHeader *hdr = (const RtpHeader *)pkt_buf;
        u8 version = (hdr->version_p_x_cc >> 6) & 0x03;
        if (version != 2) {
            continue;
        }

        unsigned char *enc_data = pkt_buf + RTP_HDR_SIZE;
        int enc_bytes = received - (int)RTP_HDR_SIZE;

        /* Decrypt audio payload with per-packet IV derived from rikeyid + rtp_seq */
        if ((enc_bytes & 0x0F) == 0 && enc_bytes > 0) {
            unsigned short seq = ntohs(hdr->seq);
            stream_crypto_decrypt_audio(enc_data, enc_bytes,
                                        seq, g_av_ri_key_id);
        }

        /* Decode Opus to PCM.  After decryption, enc_data contains the Opus
         * frame.  Decode into pcm_decode_buf (max AUDIO_CHUNK_SAMPLES). */
        int decoded_samples = opus_psp_decode(enc_data, enc_bytes,
                                              pcm_decode_buf,
                                              AUDIO_CHUNK_SAMPLES);
        if (decoded_samples <= 0) {
            s_stats.frames_dropped++;
            continue;
        }

        /* We require exactly AUDIO_CHUNK_SAMPLES per packet for the PSP DMA */
        if (decoded_samples != AUDIO_CHUNK_SAMPLES) {
            s_stats.frames_dropped++;
            continue;
        }

        /* Push into ring buffer */
        if (ring_push_pcm(pcm_decode_buf) < 0) {
            s_stats.frames_dropped++;
        }

        /* --- drain ring and play --- */
        int16_t play_buf[AUDIO_CHUNK_SAMPLES * AUDIO_CHANNELS];

        while (!ring_empty() && s_running) {
            if (ring_pop_pcm(play_buf) == 0) {
                sceAudioOutputBlocking(s_audio_chan, PSP_AUDIO_VOLUME_MAX, play_buf);
                s_stats.frames_played++;
            }
        }
    }

    /* Flush any remaining buffered audio with silence to avoid a click */
    sceAudioOutputBlocking(s_audio_chan, PSP_AUDIO_VOLUME_MAX, s_silence);

    sceKernelExitThread(0);
    return 0;
}

/*--------------------------------------------------------------------------
 * Public API
 *--------------------------------------------------------------------------*/

int audio_thread_init(const char *host_ip)
{
    if (s_running) {
        return 0;   /* already started */
    }

    memset(&s_ring,    0, sizeof(s_ring));
    memset(&s_stats,   0, sizeof(s_stats));
    memset(s_silence,  0, sizeof(s_silence));

    diag_log_write("AUD", "initializing opus...\n");
    if (opus_psp_init(AUDIO_SAMPLE_RATE, AUDIO_CHANNELS, 1, 1) < 0) {
        diag_log_write("AUD", "Opus decoder init failed\n");
        return -1;
    }

    /* The audio UDP sockets should already be pre-bound by network_connect.c. 
     * If they aren't for some reason, try to bind them now. */
    if (s_udp_sock < 0) {
        if (audio_thread_reserve_client_port(NULL) < 0) {
            return -1;
        }
    }

    /* Set 300 ms receive timeout on the main RTP socket so the thread can check s_running */
    {
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 300000;
        sceNetInetSetsockopt(s_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }

    /* Start audio ping thread if RTSP didn't already bring it up.
     * Moonlight-common-c starts audio pings before PLAY, but we keep the
     * init path idempotent so later startup can safely reuse the thread. */
    if (start_audio_ping_thread() < 0) {
        audio_log("[AUDIO] warning: audio ping thread unavailable during init\n");
    }

    /* Reserve stereo audio channel */
    s_audio_chan = sceAudioChReserve(PSP_AUDIO_NEXT_CHANNEL,
                                     AUDIO_CHUNK_SAMPLES,
                                     PSP_AUDIO_FORMAT_STEREO);
    if (s_audio_chan < 0) {
        /* Keep ping thread running so Sunshine can stream video even if
         * audio playback can't be initialized under PPSSPP. */
        audio_log("[AUDIO] sceAudioChReserve failed (%d). Keeping audio pings alive.\n",
                  s_audio_chan);
        opus_psp_shutdown();
        return -2;
    }

    s_running = 1;

    s_audio_tid = sceKernelCreateThread(
        "moonlight_audio",
        audio_thread_func,
        0x1A,               /* priority: higher than decoder(0x1C) to prevent audio stutter */
        16 * 1024,          /* 16 KB stack — adequate for sceAudio path */
        0, NULL);

    if (s_audio_tid < 0) {
        s_running = 0;
        stop_audio_ping_thread();
        sceAudioChRelease(s_audio_chan);
        s_audio_chan = -1;
        if (s_udp_sock >= 0) {
            sceNetInetClose(s_udp_sock);
            s_udp_sock = -1;
        }
        if (s_udp_sock_rtcp >= 0) {
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock_rtcp = -1;
        }
        s_bound_audio_port = 0;
        opus_psp_shutdown();
        return -3;
    }

    if (sceKernelStartThread(s_audio_tid, 0, NULL) < 0) {
        s_running = 0;
        stop_audio_ping_thread();
        sceKernelDeleteThread(s_audio_tid);
        s_audio_tid = -1;
        sceAudioChRelease(s_audio_chan);
        s_audio_chan = -1;
        if (s_udp_sock >= 0) {
            sceNetInetClose(s_udp_sock);
            s_udp_sock = -1;
        }
        if (s_udp_sock_rtcp >= 0) {
            sceNetInetClose(s_udp_sock_rtcp);
            s_udp_sock_rtcp = -1;
        }
        s_bound_audio_port = 0;
        opus_psp_shutdown();
        return -3;
    }

    return 0;
}

void audio_thread_shutdown(void)
{
    s_running = 0;
    s_ping_running = 0;

    /* Stop audio ping thread first */
    stop_audio_ping_thread();

    /* Unblock threads by closing sockets FIRST */
    if (s_udp_sock >= 0) {
        sceNetInetClose(s_udp_sock);
        s_udp_sock = -1;
    }
    if (s_udp_sock_rtcp >= 0) {
        sceNetInetClose(s_udp_sock_rtcp);
        s_udp_sock_rtcp = -1;
    }

    if (s_audio_tid >= 0) {
        SceUInt timeout_us = 1000000;   /* 1 s */
        sceKernelWaitThreadEnd(s_audio_tid, &timeout_us);
        sceKernelDeleteThread(s_audio_tid);
        s_audio_tid = -1;
    }

    if (s_audio_chan >= 0) {
        sceAudioChRelease(s_audio_chan);
        s_audio_chan = -1;
    }

    s_bound_audio_port = 0;

    /* Shut down Opus decoder */
    opus_psp_shutdown();
}

int audio_thread_is_running(void)
{
    return s_running;
}

void audio_thread_get_stats(AudioStats *out)
{
    if (out) {
        *out = s_stats;
    }
}
