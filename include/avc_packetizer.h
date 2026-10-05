#ifndef MOONLIGHT_AVC_PACKETIZER_H
#define MOONLIGHT_AVC_PACKETIZER_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Borrowed SPS/PPS pointers remain valid only while the input AU is alive.
 * Caller must cache parameter sets before releasing its reassembly buffer. */
typedef struct {
    const unsigned char *sps, *pps;
    size_t sps_size, pps_size, avcc_size;
    int has_idr, has_predictive;
} AvcPacketizedAu;
/* One complete Annex-B AU only. No truncation, no implicit frame splitting.
 * Returns zero on success; output is unusable on error. Buffers must not overlap. */
int avc_packetize(const unsigned char *src,size_t size,
                  unsigned char *dst,size_t capacity,AvcPacketizedAu *au);
#ifdef __cplusplus
}
#endif
#endif
