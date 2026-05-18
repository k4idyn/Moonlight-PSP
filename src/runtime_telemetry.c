/*
 * runtime_telemetry.c - One-second utilization windows for HUD/debug logging.
 */

#include <string.h>

#define RUNTIME_TELEMETRY_IMPLEMENTATION
#include "runtime_telemetry.h"

volatile u32 g_telemetry_cpu_last_us = 0;
volatile u32 g_telemetry_gpu_last_us = 0;
volatile u32 g_telemetry_me_last_us  = 0;

#ifdef RETAIL_BUILD

void telemetry_reset(void)
{
    g_telemetry_cpu_last_us = 0;
    g_telemetry_gpu_last_us = 0;
    g_telemetry_me_last_us = 0;
}

void telemetry_accum_cpu(u32 elapsed_us)
{
    (void)elapsed_us;
}

void telemetry_accum_gpu(u32 elapsed_us)
{
    (void)elapsed_us;
}

void telemetry_accum_me(u32 elapsed_us)
{
    (void)elapsed_us;
}

void telemetry_sample(u32 elapsed_us, u32 *cpu_pct, u32 *gpu_pct, u32 *me_pct)
{
    (void)elapsed_us;
    if (cpu_pct) {
        *cpu_pct = 0;
    }
    if (gpu_pct) {
        *gpu_pct = 0;
    }
    if (me_pct) {
        *me_pct = 0;
    }
}

void telemetry_accum_video_rx(u32 bytes) { (void)bytes; }
void telemetry_accum_video_accept(u32 bytes) { (void)bytes; }
void telemetry_accum_video_drop(u32 bytes) { (void)bytes; }
void telemetry_accum_video_data(u32 bytes) { (void)bytes; }
void telemetry_accum_video_fec(u32 bytes) { (void)bytes; }
void telemetry_accum_video_usable(u32 bytes) { (void)bytes; }
void telemetry_accum_audio_rx(u32 bytes) { (void)bytes; }
void telemetry_accum_audio_data(u32 bytes) { (void)bytes; }
void telemetry_accum_audio_fec(u32 bytes) { (void)bytes; }

void telemetry_sample_bandwidth(u32 elapsed_us, BandwidthTelemetry *out)
{
    (void)elapsed_us;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

#else

static volatile u32 s_cpu_accum_us = 0;
static volatile u32 s_gpu_accum_us = 0;
static volatile u32 s_me_accum_us  = 0;
static volatile u32 s_video_rx_bytes = 0;
static volatile u32 s_video_accept_bytes = 0;
static volatile u32 s_video_drop_bytes = 0;
static volatile u32 s_video_data_bytes = 0;
static volatile u32 s_video_fec_bytes = 0;
static volatile u32 s_video_usable_bytes = 0;
static volatile u32 s_audio_rx_bytes = 0;
static volatile u32 s_audio_data_bytes = 0;
static volatile u32 s_audio_fec_bytes = 0;
static volatile u32 s_video_packets = 0;
static volatile u32 s_audio_packets = 0;

static u32 pct_from_window(u32 work_us, u32 elapsed_us)
{
    unsigned long long v;

    if (elapsed_us == 0) {
        return 0;
    }

    v = (unsigned long long)work_us * 100ULL + (elapsed_us / 2);
    v /= elapsed_us;
    if (v > 100ULL) {
        v = 100ULL;
    }
    return (u32)v;
}

void telemetry_reset(void)
{
    s_cpu_accum_us = 0;
    s_gpu_accum_us = 0;
    s_me_accum_us = 0;
    s_video_rx_bytes = 0;
    s_video_accept_bytes = 0;
    s_video_drop_bytes = 0;
    s_video_data_bytes = 0;
    s_video_fec_bytes = 0;
    s_video_usable_bytes = 0;
    s_audio_rx_bytes = 0;
    s_audio_data_bytes = 0;
    s_audio_fec_bytes = 0;
    s_video_packets = 0;
    s_audio_packets = 0;
    g_telemetry_cpu_last_us = 0;
    g_telemetry_gpu_last_us = 0;
    g_telemetry_me_last_us = 0;
}

void telemetry_accum_cpu(u32 elapsed_us)
{
    if (elapsed_us > 1000000u) {
        elapsed_us = 1000000u;
    }
    g_telemetry_cpu_last_us = elapsed_us;
    s_cpu_accum_us += elapsed_us;
}

void telemetry_accum_gpu(u32 elapsed_us)
{
    if (elapsed_us > 1000000u) {
        elapsed_us = 1000000u;
    }
    g_telemetry_gpu_last_us = elapsed_us;
    s_gpu_accum_us += elapsed_us;
}

void telemetry_accum_me(u32 elapsed_us)
{
    if (elapsed_us > 1000000u) {
        elapsed_us = 1000000u;
    }
    g_telemetry_me_last_us = elapsed_us;
    s_me_accum_us += elapsed_us;
}

void telemetry_sample(u32 elapsed_us, u32 *cpu_pct, u32 *gpu_pct, u32 *me_pct)
{
    u32 cpu_us = s_cpu_accum_us;
    u32 gpu_us = s_gpu_accum_us;
    u32 me_us  = s_me_accum_us;

    s_cpu_accum_us = 0;
    s_gpu_accum_us = 0;
    s_me_accum_us = 0;

    if (cpu_pct) {
        *cpu_pct = pct_from_window(cpu_us, elapsed_us);
    }
    if (gpu_pct) {
        *gpu_pct = pct_from_window(gpu_us, elapsed_us);
    }
    if (me_pct) {
        *me_pct = pct_from_window(me_us, elapsed_us);
    }
}

static u32 kbps_from_bytes(u32 bytes, u32 elapsed_us)
{
    unsigned long long v;

    if (elapsed_us == 0) {
        return 0;
    }

    v = (unsigned long long)bytes * 8000000ULL;
    v += (elapsed_us / 2);
    v /= elapsed_us;
    v = (v + 500ULL) / 1000ULL;
    if (v > 0xFFFFFFFFULL) {
        v = 0xFFFFFFFFULL;
    }
    return (u32)v;
}

static u32 rate_from_count(u32 count, u32 elapsed_us)
{
    unsigned long long v;

    if (elapsed_us == 0) {
        return 0;
    }

    v = (unsigned long long)count * 1000000ULL + (elapsed_us / 2);
    v /= elapsed_us;
    if (v > 0xFFFFFFFFULL) {
        v = 0xFFFFFFFFULL;
    }
    return (u32)v;
}

void telemetry_accum_video_rx(u32 bytes)
{
    s_video_rx_bytes += bytes;
    s_video_packets++;
}

void telemetry_accum_video_accept(u32 bytes)
{
    s_video_accept_bytes += bytes;
}

void telemetry_accum_video_drop(u32 bytes)
{
    s_video_drop_bytes += bytes;
}

void telemetry_accum_video_data(u32 bytes)
{
    s_video_data_bytes += bytes;
}

void telemetry_accum_video_fec(u32 bytes)
{
    s_video_fec_bytes += bytes;
}

void telemetry_accum_video_usable(u32 bytes)
{
    s_video_usable_bytes += bytes;
}

void telemetry_accum_audio_rx(u32 bytes)
{
    s_audio_rx_bytes += bytes;
    s_audio_packets++;
}

void telemetry_accum_audio_data(u32 bytes)
{
    s_audio_data_bytes += bytes;
}

void telemetry_accum_audio_fec(u32 bytes)
{
    s_audio_fec_bytes += bytes;
}

void telemetry_sample_bandwidth(u32 elapsed_us, BandwidthTelemetry *out)
{
    u32 video_rx = s_video_rx_bytes;
    u32 video_accept = s_video_accept_bytes;
    u32 video_drop = s_video_drop_bytes;
    u32 video_data = s_video_data_bytes;
    u32 video_fec = s_video_fec_bytes;
    u32 video_usable = s_video_usable_bytes;
    u32 audio_rx = s_audio_rx_bytes;
    u32 audio_data = s_audio_data_bytes;
    u32 audio_fec = s_audio_fec_bytes;
    u32 video_packets = s_video_packets;
    u32 audio_packets = s_audio_packets;
    u32 payload_total;

    s_video_rx_bytes = 0;
    s_video_accept_bytes = 0;
    s_video_drop_bytes = 0;
    s_video_data_bytes = 0;
    s_video_fec_bytes = 0;
    s_video_usable_bytes = 0;
    s_audio_rx_bytes = 0;
    s_audio_data_bytes = 0;
    s_audio_fec_bytes = 0;
    s_video_packets = 0;
    s_audio_packets = 0;

    if (!out) {
        return;
    }

    out->video_rx_kbps = kbps_from_bytes(video_rx, elapsed_us);
    out->video_accept_kbps = kbps_from_bytes(video_accept, elapsed_us);
    out->video_drop_kbps = kbps_from_bytes(video_drop, elapsed_us);
    out->video_data_kbps = kbps_from_bytes(video_data, elapsed_us);
    out->video_fec_kbps = kbps_from_bytes(video_fec, elapsed_us);
    out->video_usable_kbps = kbps_from_bytes(video_usable, elapsed_us);
    out->audio_rx_kbps = kbps_from_bytes(audio_rx, elapsed_us);
    out->audio_data_kbps = kbps_from_bytes(audio_data, elapsed_us);
    out->audio_fec_kbps = kbps_from_bytes(audio_fec, elapsed_us);
    out->video_packets_s = rate_from_count(video_packets, elapsed_us);
    out->audio_packets_s = rate_from_count(audio_packets, elapsed_us);
    out->accept_pct = video_rx ? (u32)(((unsigned long long)video_accept * 100ULL) / video_rx) : 0;
    out->usable_rx_pct = video_rx ? (u32)(((unsigned long long)video_usable * 100ULL) / video_rx) : 0;
    payload_total = video_data + video_fec;
    out->fec_overhead_pct = payload_total ?
        (u32)(((unsigned long long)video_fec * 100ULL) / payload_total) : 0;
    out->audio_fec_overhead_pct = audio_rx ?
        (u32)(((unsigned long long)audio_fec * 100ULL) / audio_rx) : 0;
}

#endif /* RETAIL_BUILD */
