#include "psp_avc_submit.h"
#include "avc_stream_format.h"
#include <pspkernel.h>
#include <string.h>
#include <limits.h>
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
#include "diag_log.h"
#if defined(PROBE_VQ_MODE4_REPLAY)
#include <stdio.h>
extern FILE *mode0_session_log;
#endif
#define AVC_RESULT(stage) do { if(r) diag_log_write("AVC", "%s failed=%08X coded=%dx%d bytes=%u pictures=%d", stage, (unsigned)r, w, h, (unsigned)au->avcc_size, count); } while(0)
static unsigned int s_avc_submit_trace;
#if defined(PROBE_VQ_MODE4_REPLAY)
#define AVC_TRACE(...) do { \
    if (s_avc_submit_trace < 8u) { \
        diag_log_write("AVC", __VA_ARGS__); \
        if(mode0_session_log) { \
            fprintf(mode0_session_log,__VA_ARGS__); \
            fputc('\n',mode0_session_log); \
            fflush(mode0_session_log); \
        } \
        diag_log_flush(); \
        s_avc_submit_trace++; \
    } \
} while(0)
#else
#define AVC_TRACE(...) do { \
    if (s_avc_submit_trace < 8u) { \
        diag_log_write("AVC", __VA_ARGS__); \
        diag_log_flush(); \
        s_avc_submit_trace++; \
    } \
} while(0)
#endif
#else
#define AVC_RESULT(stage) ((void)0)
#define AVC_TRACE(...) ((void)0)
#endif
/* Firmware ABI verified in the standalone mode4 pixel-comparison probes. */
typedef struct { void *sps; int sps_size; void *pps; int pps_size;
    int prefix; void *data; int size; int mode; } Nal;
typedef struct { int reserved0[2]; int width,height; int reserved1[6]; } Info;
typedef struct { void *planes[8]; int reserved[3]; } Yuv;
typedef struct { int reserved0[4]; Info *info; int reserved1[6];
    Yuv *yuv; int reserved2[12]; } Detail;
typedef struct { int height_mb,width_mb,mode0,mode1; void *planes[8]; } Csc;
#define AVC_CSC_STRIDE_PIXELS 512u
#define AVC_CSC_MAX_HEIGHT   272u
#define AVC_CSC_MAX_BYTES    (AVC_CSC_STRIDE_PIXELS * AVC_CSC_MAX_HEIGHT * 4u)
extern int sceMpegGetAvcNalAu(SceMpeg*,Nal*,SceMpegAu*);
extern int sceMpegAvcDecodeDetail2(SceMpeg*,Detail**);
extern int sceMpegBaseCscAvc(void*,void*,int,Csc*);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC && !defined(RETAIL_BUILD)
extern int sceMpegAvcDecodeDetail(SceMpeg*,void*);
static void trace_decode_error(PspAvcSession *s,int result) {
    unsigned int detail[13];
    int detail_result;
    /* Verified against the live 6.61 MPEG/Videocodec/ME modules in the
     * September 30 Ghidra project. This API copies the saved codec detail;
     * it does not issue another MediaEngine RPC. Word 0 is written even
     * when no picture exists and the detail query returns an error.
     * The videocodec layer translates internal -3 to 0x80628001; the saved
     * word preserves the underlying ME result instead of losing that cause. */
    memset(detail,0xff,sizeof(detail));
    detail_result=sceMpegAvcDecodeDetail(&s->mpeg,detail);
    diag_log_write("AVC","decode error detail result=%08X query=%08X me_status=%08X context=%08X ddr=%08X mpeg=%08X",
        (unsigned)result,(unsigned)detail_result,detail[0],
        (unsigned)s->context,(unsigned)s->ddr,(unsigned)s->mpeg);
#if defined(PROBE_VQ_MODE4_REPLAY)
    if(mode0_session_log) {
        fprintf(mode0_session_log,"decode error detail result=%08X query=%08X me_status=%08X context=%08X ddr=%08X mpeg=%08X\n",
            (unsigned)result,(unsigned)detail_result,detail[0],
            (unsigned)s->context,(unsigned)s->ddr,(unsigned)s->mpeg);
        fflush(mode0_session_log);
    }
#endif
    diag_log_flush();
}
#else
#define trace_decode_error(s,result) ((void)0)
#endif
static int prepare(PspAvcSession *s,int w,int h,PspAvcPictures *p) {
    int i,j;
    if(!p) return -1;
    p->count=0;
    if(!s || !s->ready || w<=0 || w>480 || h<=0 || h>272 ||
       (w&15) || (h&15)) return -1;
    for(i=0;i<3;i++) {
        if(!p->rgba[i] || ((unsigned int)p->rgba[i]&63)) return -1;
        for(j=0;j<i;j++) {
            unsigned int a=(unsigned int)p->rgba[i],b=(unsigned int)p->rgba[j];
            if((a>b?a-b:b-a)<AVC_CSC_MAX_BYTES) return -1;
        }
    }
    return 0;
}
static int convert(PspAvcSession *s,const AvcStreamFormat *format,
                   PspAvcPictures *p,int count) {
    int i,r; Detail *d=NULL;
    unsigned int output_bytes;
    if(!format || count<0 || count>3) return -1;
    if(!count) return 0;
    /* CSC writes macroblock rows at a fixed 512-pixel destination stride.
     * Cache maintenance only needs to cover the padded coded height; flushing
     * the unused lower rows of a 300x170 or 360x204 stream wastes frame time. */
    if(!format->coded_height || format->coded_height>AVC_CSC_MAX_HEIGHT ||
       (format->coded_height&15u)) return -1;
    output_bytes=AVC_CSC_STRIDE_PIXELS*format->coded_height*4u;
    r=sceMpegAvcDecodeDetail2(&s->mpeg,&d);
    if(r || !d || !d->info || !d->yuv) return r?r:-1;
    for(i=0;i<count;i++) {
        Csc c;
        unsigned int info_w=(unsigned int)d->info[i].width;
        unsigned int info_h=(unsigned int)d->info[i].height;
        /* Firmware detail can report either the cropped visible dimensions
         * or the padded SPS dimensions. They describe the same macroblock
         * grid for right/bottom crop; do not compare them as if crop did not
         * exist. */
        if(!((info_w==format->width && info_h==format->height) ||
             (info_w==format->coded_width && info_h==format->coded_height))) {
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
            AVC_TRACE("detail dimensions rejected info=%ux%u display=%ux%u coded=%ux%u",
                info_w,info_h,format->width,format->height,
                format->coded_width,format->coded_height);
#endif
            return -1;
        }
        memset(&c,0,sizeof(c));
        c.width_mb=(int)((info_w+15u)>>4);
        c.height_mb=(int)((info_h+15u)>>4);
        memcpy(c.planes,d->yuv[i].planes,sizeof(c.planes));
        sceKernelDcacheWritebackInvalidateRange(p->rgba[i],output_bytes);
        r=sceMpegBaseCscAvc(p->rgba[i],NULL,AVC_CSC_STRIDE_PIXELS,&c);
        if(r) return r;
        sceKernelDcacheInvalidateRange(p->rgba[i],output_bytes);
    }
    p->count=count; return 0;
}
int psp_avc_submit(PspAvcSession *s,const AvcPacketizedAu *au,
    void *avcc,PspAvcPictures *p) {
    int r,count=0,w,h; Nal n; AvcStreamFormat format;
    if(p) p->count=0;
    if(!au || !avcc || !au->sps || !au->pps || !au->sps_size ||
       !au->pps_size || !au->avcc_size || au->sps_size>INT_MAX ||
       au->pps_size>INT_MAX || au->avcc_size>262144 ||
       (!au->has_idr && !au->has_predictive) ||
       (au->has_idr && au->has_predictive)) {
        AVC_TRACE("submit preflight rejected invalid AU or arguments");
        return -1;
    }
    if(avc_stream_format(au->sps,au->sps_size,&format)) {
        AVC_TRACE("submit preflight rejected malformed/unsupported SPS");
        return -1;
    }
    w=(int)format.coded_width; h=(int)format.coded_height;
    if(prepare(s,w,h,p)) {
        AVC_TRACE("submit preflight rejected buffers/session coded=%dx%d ready=%d",
                  w,h,s?s->ready:0);
        return -1;
    }
    if(w>480 || h>272 || format.crop_left || format.crop_top) {
        AVC_TRACE("submit preflight rejected coded/crop dimensions coded=%dx%d crop=%u,%u",
                  w,h,format.crop_left,format.crop_top);
        return -1;
    }
    if((format.profile==66 && !s->main_profile &&
        !(s->helper_started || s->baseline_booted)) ||
       (format.profile==77 && (!s->main_profile || s->baseline_booted ||
                               s->helper_started))) {
        AVC_TRACE("submit preflight profile mismatch profile=%u main=%d baseline=%d helper=%d",
                  format.profile,s->main_profile,s->baseline_booted,s->helper_started);
        return -1;
    }
    n.sps=(void*)au->sps; n.sps_size=au->sps_size;
    n.pps=(void*)au->pps; n.pps_size=au->pps_size;
    n.prefix=4; n.data=avcc; n.size=au->avcc_size; n.mode=au->has_idr?3:0;
    AVC_TRACE("submit begin coded=%dx%d sps=%u pps=%u avcc=%u idr=%d pred=%d mode=%d au=%08X",
              w,h,(unsigned)au->sps_size,(unsigned)au->pps_size,
              (unsigned)au->avcc_size,au->has_idr,au->has_predictive,
              n.mode,(unsigned)s->au);
    sceKernelDcacheWritebackInvalidateAll();
    AVC_TRACE("submit before GetAvcNalAu data=%08X size=%u",
              (unsigned)n.data,(unsigned)n.size);
    r=sceMpegGetAvcNalAu(&s->mpeg,&n,(SceMpegAu*)s->au);
    AVC_TRACE("submit after GetAvcNalAu result=%08X au_size=%u es=%u",
              (unsigned)r,(unsigned)((SceMpegAu*)s->au)->iAuSize,
              (unsigned)((SceMpegAu*)s->au)->iEsBuffer);
    AVC_RESULT("GetAvcNalAu");
    if(!r) {
        AVC_TRACE("submit before AvcDecode au_size=%u thread=%08X priority=%d stack_free=%d",
            (unsigned)((SceMpegAu*)s->au)->iAuSize,(unsigned)sceKernelGetThreadId(),
            sceKernelGetThreadCurrentPriority(),sceKernelCheckThreadStack());
        r=sceMpegAvcDecode(&s->mpeg,(SceMpegAu*)s->au,512,NULL,
                           &s->decode_state);
        count=s->decode_state;
        AVC_TRACE("submit after AvcDecode result=%08X pictures=%d",(unsigned)r,count);
        AVC_RESULT("AvcDecode");
        if(r) trace_decode_error(s,r);
    }
    if(!r) { r=convert(s,&format,p,count); AVC_RESULT("picture conversion"); }
    if(r) s->ready=0; /* Recreate after errors; never reuse suspect references. */
    return r;
}
int psp_avc_drain(PspAvcSession *s,const AvcStreamFormat *format,
                  PspAvcPictures *p) {
    int r,count=0;
    if(!format || prepare(s,(int)format->coded_width,
                           (int)format->coded_height,p)) return -1;
    sceKernelDcacheWritebackInvalidateAll();
    r=sceMpegAvcDecodeStop(&s->mpeg,512,p->rgba,&count);
    if(!r) r=convert(s,format,p,count);
    if(r) s->ready=0;
    return r;
}
