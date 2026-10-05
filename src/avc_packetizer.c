#include "avc_packetizer.h"
#include <string.h>
static size_t start_code(const unsigned char *p,size_t n,size_t at) {
    if(at>=n || n-at<3 || p[at] || p[at+1]) return 0;
    if(p[at+2]==1) return 3;
    if(n-at>=4 && !p[at+2] && p[at+3]==1) return 4;
    return 0;
}
int avc_packetize(const unsigned char *src,size_t n,unsigned char *dst,
                  size_t cap,AvcPacketizedAu *au) {
    size_t p=0,used=0;
    int seen=0;
    AvcPacketizedAu result;
    if(!src || !dst || !au || !n) return -1;
    memset(au,0,sizeof(*au)); memset(&result,0,sizeof(result));
    while(p<n && !start_code(src,n,p)) { if(src[p++]) return -2; }
    while(p<n) {
        size_t k=start_code(src,n,p),begin,end,len;
        unsigned int type;
        if(!k) return -2;
        begin=p+k; end=begin;
        while(end<n && !start_code(src,n,end)) ++end;
        p=end;
        /* Annex-B trailing_zero_8bits are outside the NAL payload. */
        while(end>begin && src[end-1]==0) --end;
        len=end-begin;
        if(!len || (src[begin]&0x80)) return -3;
        type=src[begin]&31;
        if(type==7) { result.sps=src+begin; result.sps_size=len; }
        else if(type==8) { result.pps=src+begin; result.pps_size=len; }
        else if(type==1 || type==5) {
            if(len>0xffffffffu || used>cap || cap-used<4 || len>cap-used-4) return -4;
            dst[used++]=(unsigned char)(len>>24); dst[used++]=(unsigned char)(len>>16);
            dst[used++]=(unsigned char)(len>>8); dst[used++]=(unsigned char)len;
            memcpy(dst+used,src+begin,len); used+=len;
            result.has_idr |= type==5; result.has_predictive |= type==1;
        } else if(type!=6 && type!=9 && type!=10 && type!=11 && type!=12) return -5;
        seen=1;
    }
    if(!seen || (result.has_idr && result.has_predictive)) return -6;
    result.avcc_size=used; *au=result;
    return 0;
}
