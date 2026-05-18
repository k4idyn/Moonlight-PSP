/*
 * runtime_telemetry.h - Lightweight non-retail hardware utilization counters.
 */

#ifndef RUNTIME_TELEMETRY_H
#define RUNTIME_TELEMETRY_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

extern volatile u32 g_telemetry_cpu_last_us;
extern volatile u32 g_telemetry_gpu_last_us;
extern volatile u32 g_telemetry_me_last_us;

typedef struct {
    u32 video_rx_kbps;       /* UDP video bytes received from socket */
    u32 video_accept_kbps;   /* UDP video bytes accepted into packet ring */
    u32 video_drop_kbps;     /* UDP video bytes dropped before decode */
    u32 video_data_kbps;     /* RTP/FEC data payload bytes */
    u32 video_fec_kbps;      /* RTP/FEC parity payload bytes */
    u32 video_usable_kbps;   /* H.264 payload bytes submitted to assembly */
    u32 audio_rx_kbps;       /* UDP audio bytes received */
    u32 audio_data_kbps;     /* Audio RTP PT=97 bytes */
    u32 audio_fec_kbps;      /* Audio RTP PT=127 parity bytes */
    u32 video_packets_s;     /* video packets received per second */
    u32 audio_packets_s;     /* audio packets received per second */
    u32 accept_pct;          /* accepted video bytes / raw video bytes */
    u32 usable_rx_pct;       /* usable H.264 bytes / raw video bytes */
    u32 fec_overhead_pct;    /* parity bytes / (data + parity bytes) */
    u32 audio_fec_overhead_pct; /* audio parity bytes / audio UDP bytes */
} BandwidthTelemetry;

void telemetry_reset(void);
void telemetry_accum_cpu(u32 elapsed_us);
void telemetry_accum_gpu(u32 elapsed_us);
void telemetry_accum_me(u32 elapsed_us);
void telemetry_sample(u32 elapsed_us, u32 *cpu_pct, u32 *gpu_pct, u32 *me_pct);
void telemetry_accum_video_rx(u32 bytes);
void telemetry_accum_video_accept(u32 bytes);
void telemetry_accum_video_drop(u32 bytes);
void telemetry_accum_video_data(u32 bytes);
void telemetry_accum_video_fec(u32 bytes);
void telemetry_accum_video_usable(u32 bytes);
void telemetry_accum_audio_rx(u32 bytes);
void telemetry_accum_audio_data(u32 bytes);
void telemetry_accum_audio_fec(u32 bytes);
void telemetry_sample_bandwidth(u32 elapsed_us, BandwidthTelemetry *out);

#if defined(RETAIL_BUILD) && !defined(RUNTIME_TELEMETRY_IMPLEMENTATION)
#undef telemetry_reset
#undef telemetry_accum_cpu
#undef telemetry_accum_gpu
#undef telemetry_accum_me
#undef telemetry_accum_video_rx
#undef telemetry_accum_video_accept
#undef telemetry_accum_video_drop
#undef telemetry_accum_video_data
#undef telemetry_accum_video_fec
#undef telemetry_accum_video_usable
#undef telemetry_accum_audio_rx
#undef telemetry_accum_audio_data
#undef telemetry_accum_audio_fec
#define telemetry_reset() ((void)0)
#define telemetry_accum_cpu(elapsed_us) ((void)(elapsed_us))
#define telemetry_accum_gpu(elapsed_us) ((void)(elapsed_us))
#define telemetry_accum_me(elapsed_us) ((void)(elapsed_us))
#define telemetry_accum_video_rx(bytes) ((void)(bytes))
#define telemetry_accum_video_accept(bytes) ((void)(bytes))
#define telemetry_accum_video_drop(bytes) ((void)(bytes))
#define telemetry_accum_video_data(bytes) ((void)(bytes))
#define telemetry_accum_video_fec(bytes) ((void)(bytes))
#define telemetry_accum_video_usable(bytes) ((void)(bytes))
#define telemetry_accum_audio_rx(bytes) ((void)(bytes))
#define telemetry_accum_audio_data(bytes) ((void)(bytes))
#define telemetry_accum_audio_fec(bytes) ((void)(bytes))
#define telemetry_sample(elapsed_us, cpu_pct, gpu_pct, me_pct) \
    do { \
        u32 *_cpu_pct = (cpu_pct); \
        u32 *_gpu_pct = (gpu_pct); \
        u32 *_me_pct = (me_pct); \
        (void)(elapsed_us); \
        if (_cpu_pct) *_cpu_pct = 0; \
        if (_gpu_pct) *_gpu_pct = 0; \
        if (_me_pct) *_me_pct = 0; \
    } while (0)
#define telemetry_sample_bandwidth(elapsed_us, out) \
    do { \
        BandwidthTelemetry *_bt = (out); \
        unsigned int _bt_i; \
        unsigned char *_bt_p; \
        (void)(elapsed_us); \
        if (_bt) { \
            _bt_p = (unsigned char *)_bt; \
            for (_bt_i = 0; _bt_i < sizeof(*_bt); _bt_i++) { \
                _bt_p[_bt_i] = 0; \
            } \
        } \
    } while (0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_TELEMETRY_H */
