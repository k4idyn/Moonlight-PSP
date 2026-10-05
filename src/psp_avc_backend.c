#include "psp_avc_backend.h"
#include "avc_sps_fixup.h"
#include "diag_log.h"
#include <pspkernel.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
extern volatile int g_avc_entropy_mode;
extern volatile int g_cabac_detected;
#ifndef PSP_AVC_SPS_LEVEL_CEILING
#define PSP_AVC_SPS_LEVEL_CEILING 0
#endif
#if PSP_AVC_SPS_LEVEL_CEILING != 0 && PSP_AVC_SPS_LEVEL_CEILING != 30
#error "PSP_AVC_SPS_LEVEL_CEILING only supports 0 or diagnostic level 30"
#endif
#if PSP_AVC_SPS_LEVEL_CEILING != 0 && defined(RETAIL_BUILD)
#error "PSP_AVC_SPS_LEVEL_CEILING is diagnostic-only and cannot ship in a retail build"
#endif
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC && !defined(RETAIL_BUILD)
#include <stdio.h>
#include "storage_paths.h"
#include "control_stream.h"
#define AVC_DIAG(...) diag_log_write("AVC", __VA_ARGS__)
/* Opt-in evidence only. Keep stream-time storage I/O outside this tap. */
volatile unsigned int g_avc_motion_capture_request;
#define MOTION_CAPTURE_CAPACITY 262144u
#define MOTION_CAPTURE_FRAMES 64u
typedef struct {
    unsigned int time_us, offset, bytes, idr;
} MotionCaptureFrame;
static unsigned char *s_motion_capture;
static size_t s_motion_capture_size;
static unsigned int s_motion_capture_count, s_motion_capture_state;
static MotionCaptureFrame s_motion_frames[MOTION_CAPTURE_FRAMES];

static int motion_capture_append_au(const AvcPacketizedAu *au,
                                    const unsigned char *avcc) {
    static const unsigned char prefix[4]={0,0,0,1};
    size_t at=0,needed=0,begin=s_motion_capture_size;
    MotionCaptureFrame *frame;
    if(!au || !avcc || !s_motion_capture ||
       s_motion_capture_count>=MOTION_CAPTURE_FRAMES) return -1;
    if(!s_motion_capture_count) {
        if(!au->has_idr || !au->sps || !au->pps ||
           !au->sps_size || !au->pps_size ||
           au->sps_size>1024 || au->pps_size>1024) return -1;
        needed=8+au->sps_size+au->pps_size;
    }
    while(at<au->avcc_size) {
        size_t n;
        if(au->avcc_size-at<4) return -1;
        n=((size_t)avcc[at]<<24)|((size_t)avcc[at+1]<<16)|
          ((size_t)avcc[at+2]<<8)|avcc[at+3];
        at+=4;
        if(!n || n>au->avcc_size-at || n>MOTION_CAPTURE_CAPACITY-4 ||
           needed>MOTION_CAPTURE_CAPACITY-4-n) return -1;
        needed+=4+n; at+=n;
    }
    if(!au->avcc_size || begin>MOTION_CAPTURE_CAPACITY ||
       needed>MOTION_CAPTURE_CAPACITY-begin) return -1;
    if(!s_motion_capture_count) {
        memcpy(s_motion_capture+begin,prefix,4); begin+=4;
        memcpy(s_motion_capture+begin,au->sps,au->sps_size); begin+=au->sps_size;
        memcpy(s_motion_capture+begin,prefix,4); begin+=4;
        memcpy(s_motion_capture+begin,au->pps,au->pps_size); begin+=au->pps_size;
    }
    for(at=0;at<au->avcc_size;) {
        size_t n=((size_t)avcc[at]<<24)|((size_t)avcc[at+1]<<16)|
                 ((size_t)avcc[at+2]<<8)|avcc[at+3];
        at+=4;
        memcpy(s_motion_capture+begin,prefix,4); begin+=4;
        memcpy(s_motion_capture+begin,avcc+at,n); begin+=n; at+=n;
    }
    frame=&s_motion_frames[s_motion_capture_count++];
    frame->time_us=sceKernelGetSystemTimeLow();
    frame->offset=(unsigned int)s_motion_capture_size;
    frame->bytes=(unsigned int)needed; frame->idr=!!au->has_idr;
    s_motion_capture_size=begin;
    return 0;
}

static void capture_motion_au(const AvcPacketizedAu *au,
                               const unsigned char *avcc) {
    if(g_avc_motion_capture_request && !s_motion_capture_state) {
        int idr_ret;
        g_avc_motion_capture_request=0;
        s_motion_capture=malloc(MOTION_CAPTURE_CAPACITY);
        s_motion_capture_state=s_motion_capture?1:3;
        idr_ret=s_motion_capture?control_stream_request_idr():-1;
        AVC_DIAG("motion capture armed capacity=%u idr_request=%d allocated=%d",
                 MOTION_CAPTURE_CAPACITY,idr_ret,s_motion_capture!=NULL);
    }
    if(s_motion_capture_state==1 && au->has_idr) s_motion_capture_state=2;
    if(s_motion_capture_state!=2) return;
    if(motion_capture_append_au(au,avcc)) {
        s_motion_capture_state=3;
        AVC_DIAG("motion capture stopped at bound frames=%u bytes=%u",
                 s_motion_capture_count,(unsigned)s_motion_capture_size);
    } else if(s_motion_capture_count==MOTION_CAPTURE_FRAMES) {
        s_motion_capture_state=3;
        AVC_DIAG("motion capture complete frames=%u bytes=%u",
                 s_motion_capture_count,(unsigned)s_motion_capture_size);
    }
}

static void flush_motion_capture(void) {
    FILE *f; int ok=1; unsigned int i;
    if(s_motion_capture_state) {
        f=fopen(MOONLIGHT_SAVE_DIR "/avc_motion_20261004_original.h264","wb");
        if(f) {
            if(s_motion_capture_size &&
               fwrite(s_motion_capture,1,s_motion_capture_size,f)!=s_motion_capture_size) ok=0;
            if(fclose(f)) ok=0;
        } else ok=0;
        f=fopen(MOONLIGHT_SAVE_DIR "/avc_motion_20261004_frames.csv","wb");
        if(f) {
            if(fprintf(f,"frame,time_us,offset,bytes,idr\n")<0) ok=0;
            for(i=0;i<s_motion_capture_count;i++) {
                MotionCaptureFrame *frame=&s_motion_frames[i];
                if(fprintf(f,"%u,%u,%u,%u,%u\n",i,frame->time_us,
                           frame->offset,frame->bytes,frame->idr)<0) ok=0;
            }
            if(fclose(f)) ok=0;
        } else ok=0;
        AVC_DIAG("motion capture flushed after session close frames=%u bytes=%u complete=%d ok=%d",
                 s_motion_capture_count,(unsigned)s_motion_capture_size,
                 s_motion_capture_count==MOTION_CAPTURE_FRAMES,ok);
    }
    free(s_motion_capture); s_motion_capture=NULL;
    s_motion_capture_size=0; s_motion_capture_count=0; s_motion_capture_state=0;
    g_avc_motion_capture_request=0;
}
static int s_first_live_au_captured;
static int s_first_live_idr_captured;
static unsigned int s_input_trace_count;
static void capture_first_live_au(const unsigned char *data,size_t size) {
    FILE *f;
    if(s_first_live_au_captured || !data || !size) return;
    s_first_live_au_captured=1;
    f=fopen(MOONLIGHT_SAVE_DIR "/avc_first_20260922_raw.h264","wb");
    if(!f) return;
    if(fwrite(data,1,size,f)!=size) { fclose(f); return; }
    fclose(f);
    AVC_DIAG("first live AU captured bytes=%u",(unsigned)size);
}
static void capture_failed_input(const unsigned char *data,size_t size) {
    static int captured;
    FILE *f;
    if(captured || !data || !size) return;
    captured=1;
    f=fopen(MOONLIGHT_SAVE_DIR "/avc_failure_20260922_raw.h264","wb");
    if(!f) return;
    if(fwrite(data,1,size,f)!=size) {
        fclose(f);
        return;
    }
    fclose(f);
    AVC_DIAG("raw failed AU captured bytes=%u",(unsigned)size);
}
static void capture_first_live_idr(const AvcPacketizedAu *au,
                                   const unsigned char *raw,size_t raw_size,
                                   const unsigned char *avcc) {
    static const unsigned char prefix[4]={0,0,0,1};
    size_t at=0,n; int ok=1;
    FILE *raw_file,*f;
    if(s_first_live_idr_captured || !au || !raw || !raw_size || !avcc ||
       !au->has_idr || !au->sps || !au->pps || !au->avcc_size) return;
    s_first_live_idr_captured=1;
    raw_file=fopen(MOONLIGHT_SAVE_DIR "/avc_first_20260922_idr_raw.h264","wb");
    if(raw_file) {
        if(fwrite(raw,1,raw_size,raw_file)!=raw_size) ok=0;
        if(fclose(raw_file)) ok=0;
    } else ok=0;
    f=fopen(MOONLIGHT_SAVE_DIR "/avc_first_20260922_idr.h264","wb");
    if(!f) { AVC_DIAG("first live IDR captured raw=%u derived=0 ok=%d",
                       (unsigned)raw_size,ok); return; }
    ok &= fwrite(prefix,1,4,f)==4;
    ok &= fwrite(au->sps,1,au->sps_size,f)==au->sps_size;
    ok &= fwrite(prefix,1,4,f)==4;
    ok &= fwrite(au->pps,1,au->pps_size,f)==au->pps_size;
    while(at+4<=au->avcc_size) {
        n=((size_t)avcc[at]<<24)|((size_t)avcc[at+1]<<16)|
          ((size_t)avcc[at+2]<<8)|avcc[at+3];
        at+=4;
        if(n>au->avcc_size-at) { ok=0; break; }
        ok &= fwrite(prefix,1,4,f)==4;
        ok &= fwrite(avcc+at,1,n,f)==n;
        at+=n;
    }
    if(at!=au->avcc_size) ok=0;
    if(fclose(f)) ok=0;
    AVC_DIAG("first live IDR captured raw=%u derived=%u ok=%d",
             (unsigned)raw_size,(unsigned)au->avcc_size,ok);
}
static void capture_failed_au(const AvcPacketizedAu *au,const unsigned char *data) {
    static int captured;
    static const unsigned char prefix[4]={0,0,0,1};
    size_t at=0; int ok=1;
    FILE *f;
    if(captured) return;
    captured=1;
    f=fopen(MOONLIGHT_SAVE_DIR "/avc_failure_20260922.h264","wb");
    if(!f) return;
    ok &= fwrite(prefix,1,4,f)==4;
    ok &= fwrite(au->sps,1,au->sps_size,f)==au->sps_size;
    ok &= fwrite(prefix,1,4,f)==4;
    ok &= fwrite(au->pps,1,au->pps_size,f)==au->pps_size;
    while(at+4<=au->avcc_size) {
        size_t n=((size_t)data[at]<<24)|((size_t)data[at+1]<<16)|((size_t)data[at+2]<<8)|data[at+3];
        at+=4;
        if(n>au->avcc_size-at) { ok=0; break; }
        ok &= fwrite(prefix,1,4,f)==4;
        ok &= fwrite(data+at,1,n,f)==n;
        at+=n;
    }
    if(at!=au->avcc_size) ok=0;
    if(fclose(f)) ok=0;
    AVC_DIAG("first failed AU captured ok=%d bytes=%u",ok,(unsigned)au->avcc_size);
}
#else
#define AVC_DIAG(...) ((void)0)
#define capture_first_live_au(data,size) ((void)0)
#define capture_failed_input(data,size) ((void)0)
#define capture_first_live_idr(au,raw,raw_size,avcc) ((void)0)
#define capture_motion_au(au,avcc) ((void)0)
#define flush_motion_capture() ((void)0)
#endif
static int session_has_resources(const PspAvcSession *s) {
    return s && (s->context || s->ddr || s->avcodec || s->vsh_loaded ||
                 s->vsh_started || s->helper_loaded || s->helper_started ||
                 s->initialized || s->created || s->ready || s->baseline_booted ||
                 s->main_profile);
}
int psp_avc_backend_open(PspAvcBackend *b,unsigned int width,unsigned int height,
                         const char *helper,int main_profile) {
    int r;
    if(!b || (main_profile!=0 && main_profile!=1) ||
       (main_profile && helper) || b->avcc || session_has_resources(&b->session) || !width || !height ||
       width>480 || height>272 || (width&1) || (height&1)) return -1;
    b->expected_width=width; b->expected_height=height;
    b->baseline_helper=helper; b->main_profile=!!main_profile; b->failed=0;
    b->first_idr_submitted=0;
    b->profile_change_pending=0;
    b->requested_main_profile=!!main_profile;
    b->source_entropy_mode=-1;
    b->avcc_uid=-1;
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC && !defined(RETAIL_BUILD)
    s_input_trace_count=0;
    AVC_DIAG("motion capture request=0x%08X",
             (unsigned int)&g_avc_motion_capture_request);
#endif
    avc_stream_input_init(&b->input,0);
    /* Complete the profile-specific firmware mode switch before live RTP.
       A CABAC session must use Sony's Main mode (3); Baseline/CAVLC uses mode
       4. Starting or switching this lazily from the RTP worker can hang in
       SceMediaEngineRpcWait on hardware. */
    r=psp_avc_session_open(&b->session,helper,b->main_profile);
    /* session_open preserves ownership flags if its rollback fails.  Keep
       them visible so the player can retry close instead of losing the only
       handles to live MPEG/ME resources. */
    if(r) return r;
    /* Match the standalone Sony probe: complete the firmware transition
     * before allocating the Annex-B conversion arena owned by the stream. */
    b->avcc=psp_avc_allocate_aligned(262144,64,"AvcInputArena",
                                     &b->avcc_uid);
    if(!b->avcc) {
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
        diag_log_write("AVC","AVCC input arena allocation failed bytes=%u free=%u largest=%u",
                       262144u,(unsigned)sceKernelTotalFreeMemSize(),
                       (unsigned)sceKernelMaxFreeMemSize());
        diag_log_flush();
#endif
        r=psp_avc_session_close(&b->session);
        if(r) return r;
        memset(b,0,sizeof(*b));
        return -1;
    }
    return 0;
}
int psp_avc_backend_decode(PspAvcBackend *b,const unsigned char *src,size_t size,
                           PspAvcPictures *pictures) {
    AvcPacketizedAu au; AvcStreamFormat format; int r;
    unsigned int original_refs=0;
    size_t normalized_sps_size=0;
    int source_changed,profile_converted=0,pps_cabac=0,pps_changed;
    unsigned int pps_id=0;
    if(pictures) pictures->count=0;
    if(!b || !b->avcc || b->failed || !pictures) return -1;
    capture_first_live_au(src,size);
    r=avc_stream_input_prepare(&b->input,src,size,b->avcc,262144,&au);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC && !defined(RETAIL_BUILD)
    /* Keep startup/error evidence without writing a log for every P-frame. */
    if (s_input_trace_count++ < 16 || r < 0 || au.has_idr) {
    AVC_DIAG("input result=%d src=%u sps=%u pps=%u avcc=%u idr=%d pred=%d need_idr=%d\n",
             r,(unsigned)size,(unsigned)b->input.sps_size,
             (unsigned)b->input.pps_size,(unsigned)au.avcc_size,
             au.has_idr,au.has_predictive,b->input.need_idr);
    }
#endif
    if(r) {
        if(r<0) {
            AVC_DIAG("input rejected=%d src=%u cached=%u/%u",
                     r,(unsigned)size,(unsigned)b->input.sps_size,
                     (unsigned)b->input.pps_size);
            capture_failed_input(src,size);
            b->failed=1;
        }
        return r;
    }
    capture_first_live_idr(&au,src,size,b->avcc);
    capture_motion_au(&au,b->avcc);
    if(au.sps_size>sizeof(b->source_sps)) {
        AVC_DIAG("SPS rejected: size=%u exceeds compatibility buffer",
                 (unsigned)au.sps_size);
        capture_failed_input(src,size);
        b->failed=1; return -1;
    }
    source_changed=au.sps_size!=b->source_sps_size ||
                   memcmp(au.sps,b->source_sps,au.sps_size)!=0;
    if(source_changed) {
        r=avc_sps_normalize_main(au.sps,au.sps_size,b->firmware_sps,
                                 sizeof(b->firmware_sps),
                                 &normalized_sps_size,&profile_converted);
        if(r) {
            AVC_DIAG("SPS profile rejected: profile_idc=%u normalize=%d size=%u",
                     au.sps_size>1?au.sps[1]:0,r,(unsigned)au.sps_size);
            capture_failed_input(src,size);
            b->failed=1; return -1;
        }
        r=avc_stream_format(b->firmware_sps,normalized_sps_size,&format);
        if(r || format.width!=b->expected_width ||
           format.height!=b->expected_height || format.crop_left ||
           format.crop_top) {
            AVC_DIAG("format rejected=%d source_profile=%u profile=%u level=%u refs=%u display=%ux%u coded=%ux%u crop=%u,%u au=%u sps=%u pps=%u",
                     r,au.sps_size>1?au.sps[1]:0,format.profile,format.level,
                     format.refs,format.width,format.height,format.coded_width,
                     format.coded_height,format.crop_left,format.crop_top,
                     (unsigned)au.avcc_size,(unsigned)au.sps_size,
                     (unsigned)au.pps_size);
            capture_failed_input(src,size);
            b->failed=1; return -1;
        }
        b->firmware_sps_size=normalized_sps_size;
        if(format.refs>1) {
            if(avc_sps_limit_refs(b->firmware_sps,b->firmware_sps_size,
                                  b->firmware_sps,sizeof(b->firmware_sps),
                                  &b->firmware_sps_size,&original_refs)) {
                AVC_DIAG("SPS reference rewrite rejected profile=%u refs=%u",
                         format.profile,format.refs);
                b->failed=1; return -1;
            }
            AVC_DIAG("SPS firmware compatibility refs=%u->1 bytes=%u->%u",
                     original_refs,(unsigned)normalized_sps_size,
                     (unsigned)b->firmware_sps_size);
        }
        memcpy(b->source_sps,au.sps,au.sps_size);
        b->source_sps_size=au.sps_size;
        b->source_high_profile=profile_converted;
        b->format=format;
    } else {
        format=b->format;
    }
    /* The first in-band PPS selects the coder tuning row before this access
       unit (and therefore its first IDR) is submitted to Sony. Validate the
       PPS against Main syntax for both Baseline/CAVLC and High/CABAC input;
       a requested mode or SDP profile is not proof of the actual bitstream. */
    if(au.pps_size>sizeof(b->source_pps)) {
        AVC_DIAG("PPS rejected: size=%u exceeds compatibility buffer",
                 (unsigned)au.pps_size);
        capture_failed_input(src,size);
        b->failed=1; return -1;
    }
    pps_changed=au.pps_size &&
        (au.pps_size!=b->source_pps_size ||
         memcmp(au.pps,b->source_pps,au.pps_size)!=0);
    if(pps_changed) {
        r=avc_pps_validate_main_ex(au.pps,au.pps_size,format.sps_id,
                                   &pps_cabac,&pps_id);
        if(r) {
            AVC_DIAG("PPS rejected before first IDR: Main syntax invalid result=%d size=%u profile=%u sps_id=%u",
                     r,(unsigned)au.pps_size,format.profile,format.sps_id);
            capture_failed_input(src,size);
            b->failed=1; return -1;
        }
        memcpy(b->source_pps,au.pps,au.pps_size);
        b->source_pps_size=au.pps_size;
        b->source_entropy_mode=pps_cabac;
        g_avc_entropy_mode=pps_cabac;
        g_cabac_detected=pps_cabac;
        AVC_DIAG("PPS parsed: pps_id=%u sps_id=%u entropy=%d profile=%u firmware_main=%d selected_tuning_row=%s",
                 pps_id,format.sps_id,pps_cabac,format.profile,b->main_profile,
                 pps_cabac?"hardware-main-cabac":"hardware-main-cavlc");
        if(profile_converted) {
            AVC_DIAG("SPS normalized High->Main; CABAC=%d level=%u refs=%u",
                     pps_cabac,format.level,format.refs);
        }
    }
    if(b->source_entropy_mode<0) {
        AVC_DIAG("PPS unavailable: refusing first IDR without actual entropy mode");
        capture_failed_input(src,size);
        b->failed=1; return -1;
    }
    {
        int requested_main=(format.profile!=66 || b->source_entropy_mode==1);
        if(requested_main!=b->main_profile) {
            if(b->first_idr_submitted) {
                AVC_DIAG("profile change rejected after first IDR: actual_profile=%u entropy=%d firmware_main=%d",
                         format.profile,b->source_entropy_mode,b->main_profile);
                b->failed=1; return -1;
            }
            b->requested_main_profile=requested_main;
            b->profile_change_pending=1;
            AVC_DIAG("profile switch requested before first IDR: actual_profile=%u entropy=%d firmware_main=%d requested_main=%d",
                     format.profile,b->source_entropy_mode,b->main_profile,
                     requested_main);
            return PSP_AVC_PROFILE_CHANGE_REQUIRED;
        }
    }
    if(!b->session.ready) {
        AVC_DIAG("submit skipped: session not ready");
        capture_failed_input(src,size);
        b->failed=1; return -1;
    }
    au.sps=b->firmware_sps;
    au.sps_size=b->firmware_sps_size;
#if PSP_AVC_SPS_LEVEL_CEILING > 0
    if (PSP_AVC_SPS_LEVEL_CEILING != 30 || format.profile != 66 ||
        b->firmware_sps_size < 4 || format.coded_width > 480 ||
        format.coded_height > 272) {
        AVC_DIAG("SPS level ceiling rejected profile=%u level=%u coded=%ux%u",
                 format.profile,format.level,format.coded_width,
                 format.coded_height);
        b->failed=1; return -1;
    }
    if (b->firmware_sps[3] > PSP_AVC_SPS_LEVEL_CEILING) {
        unsigned int macroblocks=(format.coded_width/16u)*
                                 (format.coded_height/16u);
        AvcStreamFormat clamped;
        unsigned int old_level=b->firmware_sps[3];
        if (macroblocks*60u > 40500u || macroblocks*4u > 8100u) {
            AVC_DIAG("SPS level ceiling unsafe level=%u coded=%ux%u mbs=%u",
                     PSP_AVC_SPS_LEVEL_CEILING,format.coded_width,
                     format.coded_height,macroblocks);
            b->failed=1; return -1;
        }
        b->firmware_sps[3]=PSP_AVC_SPS_LEVEL_CEILING;
        if (avc_stream_format(b->firmware_sps,b->firmware_sps_size,&clamped) ||
            clamped.level != PSP_AVC_SPS_LEVEL_CEILING ||
            clamped.profile != format.profile ||
            clamped.coded_width != format.coded_width ||
            clamped.coded_height != format.coded_height ||
            clamped.refs != 1) {
            b->firmware_sps[3]=(unsigned char)old_level;
            AVC_DIAG("SPS level ceiling validation failed");
            b->failed=1; return -1;
        }
        AVC_DIAG("SPS firmware level clamp=%u->%u coded=%ux%u mbs=%u",
                 old_level,(unsigned int)b->firmware_sps[3],
                 format.coded_width,format.coded_height,macroblocks);
    }
#endif
    au.sps=b->firmware_sps;
    au.sps_size=b->firmware_sps_size;
    b->format=format;
    r=psp_avc_submit(&b->session,&au,b->avcc,pictures);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC && !defined(RETAIL_BUILD)
    if(r) {
        capture_failed_input(src,size);
        capture_failed_au(&au,b->avcc);
    }
#endif
    if(r) b->failed=1;
    else if(au.has_idr) b->first_idr_submitted=1;
    return r;
}
int psp_avc_backend_switch_profile(PspAvcBackend *b,int main_profile) {
    int r;
    if(!b || !b->avcc || (main_profile!=0 && main_profile!=1) ||
       !b->profile_change_pending || b->first_idr_submitted ||
       main_profile!=b->requested_main_profile) return -1;
    r=psp_avc_session_switch_profile(&b->session,b->baseline_helper,
                                     main_profile);
    if(r) { b->failed=1; return r; }
    b->main_profile=main_profile;
    b->profile_change_pending=0;
    b->first_idr_submitted=0;
    avc_stream_input_invalidate(&b->input);
    AVC_DIAG("profile switch ready firmware_main=%d cached_entropy=%d; waiting for IDR",
             b->main_profile,b->source_entropy_mode);
    return 0;
}
int psp_avc_backend_reset(PspAvcBackend *b) {
    int r;
    if(!b || !b->avcc) return -1;
    b->failed=1;
    r=psp_avc_session_close(&b->session); if(r) return r;
    avc_stream_input_invalidate(&b->input);
    /* A fatal Sony decode error invalidates the firmware context.  Reopen the
     * complete session before accepting the next IDR; merely clearing the
     * software flag leaves session.ready false and guarantees a second
     * failure on the first recovered AU. */
    r=psp_avc_session_open(&b->session,b->baseline_helper,b->main_profile);
    if(r) return r;
    b->failed=0; return 0;
}
int psp_avc_backend_close(PspAvcBackend *b) {
    int r;
    if(!b) return -1;
    b->failed=1;
    r=psp_avc_session_close(&b->session); if(r) return r;
    if(b->avcc) {
        r=psp_avc_release_aligned(b->avcc,b->avcc_uid);
        if(r<0) return r;
        b->avcc=NULL;
        b->avcc_uid=-1;
    }
    flush_motion_capture();
    memset(b,0,sizeof(*b)); return 0;
}
