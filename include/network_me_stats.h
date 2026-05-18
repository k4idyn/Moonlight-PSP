/*
 * network_me_stats.h - Video RTP/RTCP receive telemetry.
 */
#ifndef NETWORK_ME_STATS_H
#define NETWORK_ME_STATS_H

#include <psptypes.h>

typedef struct {
    u32 packets_received;
    u32 packets_lost_cum;
    u32 highest_ext_seq;
    u32 jitter_rtp90k;
    u32 interval_expected;
    u32 interval_lost;
    u32 fraction_lost_x10; /* 0.1 percent units for the current RTCP window */
} NetworkRtcpStats;

void network_me_get_rtcp_stats(NetworkRtcpStats *out);

#endif /* NETWORK_ME_STATS_H */
