#include "avc_stream_input.h"
#include <string.h>
void avc_stream_input_init(AvcStreamInput *s,int immediate) {
    memset(s,0,sizeof(*s)); s->need_idr=1; s->immediate_idr_only=!!immediate;
}
void avc_stream_input_invalidate(AvcStreamInput *s) { s->need_idr=1; }
static int changed(const unsigned char *cached,size_t cached_size,
                   const unsigned char *next,size_t next_size) {
    return cached_size && next_size &&
        (cached_size!=next_size || memcmp(cached,next,next_size));
}
int avc_stream_input_prepare(AvcStreamInput *s,const unsigned char *src,
    size_t size,unsigned char *dst,size_t cap,AvcPacketizedAu *au) {
    AvcPacketizedAu parsed;
    if(!s || !au) return AVC_INPUT_INVALID;
    memset(au,0,sizeof(*au));
    if(avc_packetize(src,size,dst,cap,&parsed) ||
       parsed.sps_size>sizeof(s->sps) || parsed.pps_size>sizeof(s->pps)) {
        s->need_idr=1; return AVC_INPUT_INVALID;
    }
    /* Reject changes even after loss: old references may still be in firmware. */
    if(changed(s->sps,s->sps_size,parsed.sps,parsed.sps_size) ||
       changed(s->pps,s->pps_size,parsed.pps,parsed.pps_size)) {
        s->need_idr=1; return AVC_INPUT_PARAMETER_CHANGE;
    }
    if(parsed.sps_size) {
        memcpy(s->sps,parsed.sps,parsed.sps_size); s->sps_size=parsed.sps_size;
    }
    if(parsed.pps_size) {
        memcpy(s->pps,parsed.pps,parsed.pps_size); s->pps_size=parsed.pps_size;
    }
    if(!parsed.avcc_size) return AVC_INPUT_PARAMETERS;
    if(!s->sps_size || !s->pps_size ||
       ((s->need_idr || s->immediate_idr_only) && !parsed.has_idr)) {
        s->need_idr=1; return AVC_INPUT_NEED_IDR;
    }
    parsed.sps=s->sps; parsed.sps_size=s->sps_size;
    parsed.pps=s->pps; parsed.pps_size=s->pps_size;
    *au=parsed; s->need_idr=0;
    return AVC_INPUT_READY;
}
