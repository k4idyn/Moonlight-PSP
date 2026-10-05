#ifndef MOONLIGHT_AVC_STREAM_INPUT_H
#define MOONLIGHT_AVC_STREAM_INPUT_H
#include "avc_packetizer.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Serialized decoder-thread use only. Parameter sets survive network AU reuse.
 * This is input preparation, not SPS/slice validation or a firmware decoder. */
typedef struct {
    /* sceMpeg may hand these spans to the Media Engine.  Keep the cached
     * parameter sets on the same alignment as the standalone probe's
     * malloc_64 buffers; network AUs themselves are not alignment-stable. */
    unsigned char sps[1024] __attribute__((aligned(64)));
    unsigned char pps[1024] __attribute__((aligned(64)));
    size_t sps_size, pps_size;
    int need_idr, immediate_idr_only;
} AvcStreamInput;
enum { AVC_INPUT_READY=0, AVC_INPUT_PARAMETERS=1, AVC_INPUT_NEED_IDR=2,
       AVC_INPUT_INVALID=-1, AVC_INPUT_PARAMETER_CHANGE=-2 };
void avc_stream_input_init(AvcStreamInput *state,int immediate_idr_only);
/* On loss/error the caller must reset the firmware reference state separately.
 * Cached parameter sets remain valid; the next accepted picture must be IDR. */
void avc_stream_input_invalidate(AvcStreamInput *state);
/* Output parameter pointers belong to state. READY does not mean pixels exist.
 * Parameter changes are rejected once a sequence has started: recreate decoder
 * and input state before accepting a changed stream. No silent reconfiguration. */
int avc_stream_input_prepare(AvcStreamInput *state,const unsigned char *src,
    size_t size,unsigned char *dst,size_t capacity,AvcPacketizedAu *au);
#ifdef __cplusplus
}
#endif
#endif
