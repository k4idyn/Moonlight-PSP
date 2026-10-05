#include "psp_avc_player.h"
#include "psp_avc_backend.h"
#include "avc_frame_slots.h"
#include <pspkernel.h>
#include <malloc.h>
#include <stdlib.h>
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
#include "diag_log.h"
#define PLAYER_STEP(name) do { \
    diag_log_write("AVC", "player open: %s", name); \
    diag_log_flush(); \
} while (0)
#else
#define PLAYER_STEP(name) ((void)0)
#endif
static PspAvcBackend backend;
static AvcFrameSlots slots;
static unsigned char *pixels;
static int pixels_uid=-1;
static int sema=-1;
static unsigned int output_sequence;
static unsigned int player_width,player_height;
static volatile int profile_change_pending;
static volatile int profile_change_target;
static volatile int profile_decode_active;
#define SLOT_BYTES (512*272*4)
static int lock(void) { return sceKernelWaitSema(sema,1,NULL); }
static void unlock(void) { sceKernelSignalSema(sema,1); }
static int backend_has_resources(void) {
    const PspAvcSession *s=&backend.session;
    return backend.avcc || s->context || s->ddr || s->avcodec ||
           s->vsh_loaded || s->vsh_started || s->helper_loaded ||
           s->helper_started || s->initialized || s->created || s->ready ||
           s->baseline_booted || s->main_profile;
}
static int frames_idle(void) {
    int i,idle=1;
    if(sema<0) return 1;
    if(lock()<0) return 0;
    for(i=0;i<AVC_FRAME_SLOTS;i++) {
        if(slots.state[i]!=AVC_SLOT_FREE) { idle=0; break; }
    }
    unlock();
    return idle;
}
static int close_backend_for_reopen(void) {
    int r=psp_avc_backend_close(&backend);
    profile_change_pending=0;
    profile_decode_active=0;
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    if(r) {
        diag_log_write("AVC","stale backend close failed=%08X; ownership retained",
                       (unsigned)r);
        diag_log_flush();
    }
#endif
    return r;
}
int psp_avc_player_open_profile(unsigned int w,unsigned int h,const char *helper,
                                int main_profile) {
    int r,cleanup,reuse_buffers=0;
    if(!w || !h || w>480 || h>272 || (w&1) || (h&1) ||
       (main_profile!=0 && main_profile!=1) || (main_profile && helper)) return -1;
    psp_avc_player_log_state(main_profile?"open-request-main":"open-request-baseline");
    if((sema>=0)!=(pixels!=NULL)) return -1;
    if(sema>=0 || pixels) {
        /* The Sony session is resolution-independent (the MPEG surface is
         * created at the PSP's 512-pixel stride).  A session primed at entry
         * can therefore adopt the negotiated stream dimensions later without
         * another MediaEngine mode transition. */
        if(backend.avcc && !backend.failed && backend.session.ready &&
           backend.main_profile==!!main_profile &&
           backend.baseline_helper==helper) {
            backend.expected_width=w;
            backend.expected_height=h;
            player_width=w;
            player_height=h;
            psp_avc_player_log_state("open-reused-ready-session");
            return 0;
        }
        if(!frames_idle()) {
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
            diag_log_write("AVC","cannot repair stale session while frame slot is owned");
            diag_log_flush();
#endif
            return -1;
        }
        reuse_buffers=1;
        PLAYER_STEP("repair stale firmware session");
        r=close_backend_for_reopen();
        if(r) return r;
    } else if(backend_has_resources()) {
        /* A failed earlier open can retain firmware ownership even though
         * output buffers were never allocated.  Release it before retrying. */
        PLAYER_STEP("release partial firmware session");
        r=close_backend_for_reopen();
        if(r) return r;
    }
    PLAYER_STEP("open firmware session");
    r=psp_avc_backend_open(&backend,w,h,helper,main_profile);
    PLAYER_STEP(r ? "firmware session failed" : "firmware session ready");
    psp_avc_player_log_state("open-after-firmware");
    if(r) {
        cleanup=close_backend_for_reopen();
        (void)cleanup;
        return r;
    }
    profile_change_pending=0;
    profile_change_target=main_profile;

    if(reuse_buffers) {
        avc_frame_slots_init(&slots); output_sequence=0;
        player_width=w; player_height=h;
        return 0;
    }

    /* Keep all user allocations after the Sony session has completed its
     * MediaEngine transition.  The firmware probe is sensitive to heap and
     * semaphore setup performed immediately before BootAvcMode. */
    PLAYER_STEP("allocate frame pool");
    pixels=psp_avc_allocate_aligned(AVC_FRAME_SLOTS*SLOT_BYTES,64,
                                    "AvcFramePool",&pixels_uid);
    if(!pixels) {
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
        diag_log_write("AVC","frame pool allocation failed bytes=%u free=%u largest=%u",
                       (unsigned)(AVC_FRAME_SLOTS*SLOT_BYTES),
                       (unsigned)sceKernelTotalFreeMemSize(),
                       (unsigned)sceKernelMaxFreeMemSize());
        diag_log_flush();
#endif
        close_backend_for_reopen();
        return -1;
    }
    PLAYER_STEP("create frame semaphore");
    sema=sceKernelCreateSema("avc_frames",0,1,1,NULL);
    if(sema<0) {
        r=psp_avc_release_aligned(pixels,pixels_uid);
        if(r<0) return r;
        pixels=NULL; pixels_uid=-1;
        close_backend_for_reopen();
        return -1;
    }
    avc_frame_slots_init(&slots); output_sequence=0;
    player_width=w; player_height=h;
    return r;
}
int psp_avc_player_open(unsigned int w,unsigned int h,const char *helper) {
    return psp_avc_player_open_profile(w,h,helper,0);
}
int psp_avc_player_is_open(void) {
    return sema>=0 || pixels!=NULL || backend_has_resources();
}
int psp_avc_player_is_ready(void) {
    return sema>=0 && pixels!=NULL && backend.avcc &&
           !backend.failed && backend.session.ready;
}
int psp_avc_player_profile_change_pending(void) {
    return profile_change_pending;
}
int psp_avc_player_profile_change_ready(void) {
    return profile_change_pending && !profile_decode_active && frames_idle();
}
int psp_avc_player_requested_profile(void) {
    return profile_change_target;
}
int psp_avc_player_current_profile(void) {
    return backend.main_profile;
}
int psp_avc_player_apply_profile_change(void) {
    int r;
    if(!psp_avc_player_profile_change_ready()) return -1;
    r=psp_avc_backend_switch_profile(&backend,profile_change_target);
    if(r) return r;
    profile_change_pending=0;
    return 0;
}
void psp_avc_player_log_state(const char *phase) {
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC",
        "player state phase=%s open=%d ready=%d sema=%08X pixels=%08X avcc=%08X failed=%d main=%d session_main=%d baseline=%d session_ready=%d dimensions=%ux%u",
        phase?phase:"(unknown)",psp_avc_player_is_open(),
        psp_avc_player_is_ready(),(unsigned)sema,(unsigned)pixels,
        (unsigned)backend.avcc,backend.failed,backend.main_profile,
        backend.session.main_profile,backend.session.baseline_booted,
        backend.session.ready,backend.expected_width,backend.expected_height);
    diag_log_flush();
#else
    (void)phase;
#endif
}
int psp_avc_player_decode(const unsigned char *data,unsigned int size) {
    PspAvcPictures pictures; int reserved[3],i,r,publish;
    unsigned int ids[3],decoded_at_us[3];
    if(profile_change_pending) return PSP_AVC_PROFILE_CHANGE_REQUIRED;
    if(sema<0 || lock()<0) return -1;
    r=avc_frame_slots_reserve(&slots,reserved); unlock();
    /* Caller must retain this AU and retry; no input consumed on exhaustion. */
    if(r) return -3;
    for(i=0;i<3;i++) pictures.rgba[i]=pixels+reserved[i]*SLOT_BYTES;
    profile_decode_active=1;
    r=psp_avc_backend_decode(&backend,data,size,&pictures);
    if(r==PSP_AVC_PROFILE_CHANGE_REQUIRED) {
        profile_change_target=backend.requested_main_profile;
        profile_change_pending=1;
    }
    if(r) pictures.count=0;
    for(i=0;i<pictures.count;i++) {
        ids[i]=++output_sequence;
        decoded_at_us[i]=sceKernelGetSystemTimeLow();
    }
    if(lock()<0) { profile_decode_active=0; return -1; }
    publish=avc_frame_slots_publish(&slots,reserved,pictures.count,ids,
                                    decoded_at_us);
    unlock();
    profile_decode_active=0;
    if(publish) return -1;
    /* Positive backend values distinguish parameter-only/wait-IDR. */
    return r?r:(pictures.count?0:3);
}
void *psp_avc_player_take(unsigned int *sequence,unsigned int *decoded_at_us) {
    int index;
    if(sema<0 || !sequence || !decoded_at_us || lock()<0) return NULL;
    index=avc_frame_slots_take(&slots,sequence,decoded_at_us); unlock();
    return index<0?NULL:pixels+index*SLOT_BYTES;
}
int psp_avc_player_release(void *frame) {
    int i,r=-1;
    if(sema<0 || !frame || lock()<0) return -1;
    for(i=0;i<AVC_FRAME_SLOTS;i++) if(frame==pixels+i*SLOT_BYTES) {
        r=avc_frame_slots_release(&slots,i); break;
    }
    unlock(); return r;
}
int psp_avc_player_reset(void) {
    int r;
    if(sema<0) return -1;
    r=psp_avc_backend_reset(&backend);
    if(lock()<0) return -1;
    /* Drop queued stale output, but never revoke presenter's held pixels. */
    if(slots.queued>=0) {
        slots.state[slots.queued]=AVC_SLOT_FREE; slots.queued=-1;
    }
    unlock(); return r;
}
int psp_avc_player_close(void) {
    int i,r;
    PspAvcPictures tail;
    if(sema<0) {
        r=close_backend_for_reopen();
        if(r) return r;
        if(pixels) {
            r=psp_avc_release_aligned(pixels,pixels_uid);
            if(r<0) return r;
        }
        pixels=NULL; pixels_uid=-1; player_width=player_height=0;
        return 0;
    }
    if(!pixels) return -1;
    if(lock()<0) return -1;
    for(i=0;i<AVC_FRAME_SLOTS;i++)
        if(slots.state[i]==AVC_SLOT_HELD || slots.state[i]==AVC_SLOT_WRITING) {
            unlock(); return -1;
        }
    unlock();

    /* PMFPlayer shuts its AVC decoder down before deleting the MPEG session.
     * The hardware may still hold a delayed picture or stream work even after
     * the decode worker has joined. Stop AVC output, then flush the MPEG
     * streams so sceMpegDelete does not wait forever on the MediaEngine RPC.
     * At this point no presenter owns these buffers, so any final delayed
     * picture is intentionally discarded during shutdown. */
    if(backend.session.ready && backend.format.coded_width &&
       backend.format.coded_height) {
        for(i=0;i<AVC_FRAME_SLOTS;i++)
            tail.rgba[i]=pixels+i*SLOT_BYTES;
        tail.count=0;
        r=psp_avc_drain(&backend.session,&backend.format,&tail);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
        diag_log_write("AVC","player close decode-stop=%08X pictures=%d coded=%ux%u",
                       (unsigned)r,tail.count,backend.format.coded_width,
                       backend.format.coded_height);
        diag_log_flush();
#endif
        if(r) return r;
    }
    if(backend.session.created) {
        r=sceMpegFlushAllStream(&backend.session.mpeg);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
        diag_log_write("AVC","player close flush-all=%08X",(unsigned)r);
        diag_log_flush();
#endif
        if(r) return r;
    }
    r=psp_avc_backend_close(&backend); if(r) return r;

    /* Reuse the large output pool and synchronization object across stream
     * sessions. Sony firmware shutdown fragments the remaining user heap;
     * allocating this 1.67 MiB pool after the next mode transition can fail
     * even though the previous stream just released the same buffers. The
     * decoder and presenter have both been joined before close reaches here. */
    if(lock()<0) return -1;
    avc_frame_slots_init(&slots);
    output_sequence=0;
    unlock();
    player_width=player_height=0;
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","player close retained frame pool bytes=%u",
                   (unsigned)(AVC_FRAME_SLOTS*SLOT_BYTES));
    diag_log_flush();
#endif
    return 0;
}

int psp_avc_player_destroy(void) {
    int r=psp_avc_player_close();
    if(r) return r;
    if(sema>=0) {
        r=sceKernelDeleteSema(sema);
        if(r) return r;
        sema=-1;
    }
    if(pixels) {
        r=psp_avc_release_aligned(pixels,pixels_uid);
        if(r<0) return r;
    }
    pixels=NULL; pixels_uid=-1; player_width=player_height=0;
    avc_frame_slots_init(&slots);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","player destroy released retained frame pool");
    diag_log_flush();
#endif
    return 0;
}
