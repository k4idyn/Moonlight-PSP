#include "psp_avc_session.h"
#include <pspkernel.h>
#include <pspmodulemgr.h>
#include <pspsysmem.h>
#include <psputility.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
#include "diag_log.h"
#if defined(PSP_AVC_BOOT_MODE_BRIDGE) && PSP_AVC_BOOT_MODE_BRIDGE
extern int AvcBootBridgeBootAvcMode(int mode);
#define AVC_BOOT_MODE_SWITCH AvcBootBridgeBootAvcMode
#if defined(PSP_AVC_LOW_DDR) && PSP_AVC_LOW_DDR
extern void *AvcBootBridgeGetAvcLowDdr(void);
#endif
#else
extern int BootAvcMode(int mode);
#define AVC_BOOT_MODE_SWITCH BootAvcMode
#endif
extern int AvcFixupModuleImports(SceUID consumer, SceUID provider);
#if defined(PSP_AVC_PPA_STACK_PRELOADED) && PSP_AVC_PPA_STACK_PRELOADED
static SceUID find_preloaded_vsh_module(void) {
    SceUID ids[96];
    int count=0,i,r;
    r=sceKernelGetModuleIdList(ids,96,&count);
    if(r<0) return r;
    if(count>96) count=96;
    for(i=0;i<count;i++) {
        SceKernelModuleInfo info;
        memset(&info,0,sizeof(info));
        info.size=sizeof(info);
        if(sceKernelQueryModuleInfo(ids[i],&info)==0 &&
           strncmp(info.name,"sceMpegVsh_library",sizeof(info.name))==0) {
            return ids[i];
        }
    }
    return -1;
}
#endif
#if defined(PROBE_VQ_MODE4_REPLAY)
#include <stdio.h>
extern FILE *mode0_session_log;
#define AVC_STEP(name) do { \
    diag_log_write("AVC", "session open: %s", name); \
    if(mode0_session_log) { \
        fprintf(mode0_session_log,"session open: %s\n",name); \
        fflush(mode0_session_log); \
    } \
} while(0)
#else
#define AVC_STEP(name) do { \
    diag_log_write("AVC", "session open: %s", name); \
    diag_log_flush(); \
} while (0)
#endif
#else
#define AVC_STEP(name) ((void)0)
#endif
extern int kuKernelLoadModule(const char *,int,void *);

void *psp_avc_allocate_aligned(size_t size,size_t alignment,const char *name,
                               int *allocation_uid) {
    void *ptr,*base;
    SceUID uid;
    size_t allocation_size;
    unsigned int aligned;
    if(!allocation_uid || !size || !alignment ||
       (alignment&(alignment-1))!=0) return NULL;
    *allocation_uid=-1;
    ptr=memalign(alignment,size);
    if(ptr) return ptr;
    if(size>(size_t)0xFFFFFFFFu-(alignment-1)) return NULL;
    allocation_size=size+alignment-1;
    uid=sceKernelAllocPartitionMemory(PSP_MEMORY_PARTITION_USER,
        name?name:"MoonlightAVC",PSP_SMEM_High,(SceSize)allocation_size,NULL);
    if(uid<0) {
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
        diag_log_write("AVC","aligned allocation failed name=%s bytes=%u align=%u result=%08X",
            name?name:"MoonlightAVC",(unsigned)size,(unsigned)alignment,(unsigned)uid);
        diag_log_flush();
#endif
        return NULL;
    }
    base=sceKernelGetBlockHeadAddr(uid);
    if(!base) {
        sceKernelFreePartitionMemory(uid);
        return NULL;
    }
    aligned=((unsigned int)base+(unsigned int)alignment-1u)&
            ~((unsigned int)alignment-1u);
    *allocation_uid=uid;
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","aligned allocation fallback name=%s uid=%08X base=%08X ptr=%08X bytes=%u align=%u",
        name?name:"MoonlightAVC",(unsigned)uid,(unsigned)base,aligned,
        (unsigned)size,(unsigned)alignment);
    diag_log_flush();
#endif
    return (void*)aligned;
}

int psp_avc_release_aligned(void *ptr,int allocation_uid) {
    if(!ptr) return 0;
    if(allocation_uid>=0) return sceKernelFreePartitionMemory(allocation_uid);
    free(ptr);
    return 0;
}

static int psp_avc_session_close_internal(PspAvcSession *s,int restore_mode) {
    int r,status=0;
    if(!s) return -1;
    s->ready=0;
    /* No concurrent firmware calls are permitted by the API contract. Delete
       discards buffered pictures; stream-end presentation is a separate step. */
    if(s->created) { sceMpegDelete(&s->mpeg); s->created=0; }
    if(s->initialized) { sceMpegFinish(); s->initialized=0; }
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    if(restore_mode && s->baseline_booted) {
        r=AVC_BOOT_MODE_SWITCH(3); if(r<0) return r;
        s->baseline_booted=0;
    }
#endif
    if(s->helper_started) {
        r=sceKernelStopModule(s->helper_uid,0,NULL,&status,NULL);
        if(r<0 || status<0) return r<0?r:status;
        s->helper_started=0;
    }
    if(s->helper_loaded) {
        r=sceKernelUnloadModule(s->helper_uid); if(r<0) return r;
        s->helper_loaded=0;
    }
    if(s->vsh_started) {
        status=0;
        r=sceKernelStopModule(s->vsh_uid,0,NULL,&status,NULL);
        if(r<0 || status<0) return r<0?r:status;
        s->vsh_started=0;
    }
    if(s->vsh_loaded) {
        r=sceKernelUnloadModule(s->vsh_uid); if(r<0) return r;
        s->vsh_loaded=0;
    }
    if(s->avcodec) {
        r=sceUtilityUnloadAvModule(0); if(r<0) return r;
        s->avcodec=0;
    }
    if(s->context) {
        r=psp_avc_release_aligned(s->context,s->context_uid);
        if(r<0) return r;
        s->context_uid=-1;
        s->context=NULL;
    }
    if(!s->ddr_borrowed) free(s->ddr);
    memset(s,0,sizeof(*s));
    return 0;
}

int psp_avc_session_close(PspAvcSession *s) {
    return psp_avc_session_close_internal(s,1);
}

int psp_avc_session_switch_profile(PspAvcSession *s,const char *helper_path,
                                   int main_profile) {
    int r;
    if(!s || (main_profile!=0 && main_profile!=1) ||
       (main_profile && helper_path)) return -1;
    r=psp_avc_session_close_internal(s,0);
    if(r<0) return r;
    return psp_avc_session_open(s,helper_path,main_profile);
}

int psp_avc_session_open(PspAvcSession *s,const char *helper_path,
                         int main_profile) {
    int r,size=0,cleanup;
#if !defined(PSP_AVC_PPA_STACK_PRELOADED) || !PSP_AVC_PPA_STACK_PRELOADED
    int status=0;
#endif
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC && \
    (!defined(PSP_AVC_PPA_STACK_PRELOADED) || !PSP_AVC_PPA_STACK_PRELOADED)
    SceKernelLMOption load_option;
#endif
    unsigned int version=sceKernelDevkitVersion();
    if(!s || (main_profile!=0 && main_profile!=1) ||
       (main_profile && helper_path) || s->context || s->ddr || s->avcodec || s->vsh_loaded ||
       s->helper_loaded || s->initialized || s->created || s->ready ||
       s->baseline_booted || s->main_profile) return -1;
    if(version!=0x06060010 && version!=0x06060110) return -1;
    s->decode_state=0;
    s->context_uid=-1;
#if defined(PSP_AVC_PPA_STACK_PRELOADED) && PSP_AVC_PPA_STACK_PRELOADED
    /* The diagnostic harness starts the PMPlayer-style codec stack before
     * loading this consumer PRX. Its sceMpeg imports are therefore resolved
     * by the PSP loader against the resident exports; skip duplicate module
     * loads and the late import-table rewrite used by the normal path. */
    AVC_STEP("adopt PPA-preloaded AVC codec stack");
    r=find_preloaded_vsh_module();
    diag_log_write("AVC","preloaded mpeg_vsh module uid=%08X",(unsigned)r);
    diag_log_flush();
    if(r<0) goto fail;
    s->vsh_uid=r;
    /* The bootstrap owns these providers. A session owns its MPEG context
     * and ME mode, but must retain the providers for the next stream and
     * for this PRX's already-resolved sceMpeg imports. Leave their unload
     * flags clear; only the process-level owner may release them. */
#elif defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    AVC_STEP("load avcodec");
    r=sceUtilityLoadAvModule(0); if(r<0) return r;
    s->avcodec=1;
    AVC_STEP("load mpeg_vsh");
    /* Match the known-good PMPlayer bootstrap: mpeg_vsh must be loaded into
     * user partition 2 before its privileged imports are fixed up. */
    memset(&load_option,0,sizeof(load_option));
    load_option.size=sizeof(load_option);
    load_option.mpidtext=PSP_MEMORY_PARTITION_USER;
    load_option.mpiddata=PSP_MEMORY_PARTITION_USER;
    r=kuKernelLoadModule("flash0:/kd/mpeg_vsh.prx",0,&load_option);
    if(r<0) goto fail;
    s->vsh_uid=r; s->vsh_loaded=1;
    AVC_STEP("start mpeg_vsh");
    r=sceKernelStartModule(s->vsh_uid,0,NULL,&status,NULL);
    if(r<0) goto fail;
    s->vsh_started=1;
    if(status<0) { r=status; goto fail; }
    AVC_STEP("fix Moonlight sceMpeg imports from mpeg_vsh");
    /* The kernel helper patches Moonlight's late-bound imports from the
     * freshly loaded mpeg_vsh export table. It also rebinds the same import
     * stubs after a prior stream unloaded and reloaded this provider. */
    r=AvcFixupModuleImports(sceKernelGetModuleId(),s->vsh_uid);
    diag_log_write("AVC","mpeg_vsh import rebind result=%d provider=%08X",
                   r,(unsigned)s->vsh_uid);
    diag_log_flush();
    if(r<1) {
        if(r==0) r=-1;
        goto fail;
    }
#else
    if(helper_path) {
        AVC_STEP("load Baseline profile helper");
        r=kuKernelLoadModule(helper_path,0,NULL); if(r<0) goto fail;
        s->helper_uid=r; s->helper_loaded=1; status=0;
        AVC_STEP("start Baseline profile helper");
        r=sceKernelStartModule(s->helper_uid,0,NULL,&status,NULL);
        if(r<0) goto fail;
        s->helper_started=1;
        if(status<0) { r=status; goto fail; }
    }
#endif
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    /* The direct firmware transition must precede RTSP/audio workers. Both
     * the normal late-fixup path and the PPA-preloaded diagnostic path use
     * the same mode switch and priority. */
    (void)helper_path;
    {
        int caller_priority=sceKernelGetThreadCurrentPriority();
        int changed=0;
        if(caller_priority>=0 && caller_priority<33 &&
           sceKernelChangeThreadPriority(0,33)>=0) changed=1;
        AVC_STEP(main_profile ? "switch Main mode at priority 33" :
                                "switch Baseline mode at priority 33");
        r=AVC_BOOT_MODE_SWITCH(main_profile ? 3 : 4);
        diag_log_write("AVC","firmware profile boot mode=%d result=%08X",
            main_profile?3:4,(unsigned)r);
        diag_log_flush();
        if(changed) {
            int restore=sceKernelChangeThreadPriority(0,caller_priority);
            if(restore<0 && r>=0) r=restore;
        }
        if(r<0) goto fail;
    }
    s->baseline_booted=!main_profile;
    s->main_profile=!!main_profile;
#endif
    AVC_STEP("mpeg init");
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","DDR heap before sceMpegInit free=%u largest=%u",
        (unsigned)sceKernelTotalFreeMemSize(),
        (unsigned)sceKernelMaxFreeMemSize());
    diag_log_flush();
#endif
    r=sceMpegInit(); if(r<0) goto fail;
    s->initialized=1;
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","DDR heap after sceMpegInit free=%u largest=%u",
        (unsigned)sceKernelTotalFreeMemSize(),
        (unsigned)sceKernelMaxFreeMemSize());
    diag_log_flush();
#endif
    AVC_STEP("allocate DDR");
#if defined(PSP_AVC_LOW_DDR) && PSP_AVC_LOW_DDR
    s->ddr=AvcBootBridgeGetAvcLowDdr();
    s->ddr_borrowed=1;
    diag_log_write("AVC","borrow early DDR reservation address=%08X bytes=%u",
        (unsigned)s->ddr,0x200000u);
    diag_log_flush();
    if(s->ddr!=(void*)0x08C00000) { r=-1; goto fail; }
#else
    s->ddr=memalign(0x400000,0x200000);
#endif
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    if(!s->ddr) {
        diag_log_write("AVC","DDR allocation failed align=%u bytes=%u free=%u largest=%u",
            0x400000u,0x200000u,(unsigned)sceKernelTotalFreeMemSize(),
            (unsigned)sceKernelMaxFreeMemSize());
        diag_log_flush();
        r=-1; goto fail;
    }
#else
    if(!s->ddr) { r=-1; goto fail; }
#endif
    memset(s->ddr,0,0x200000);
    /* PMPlayer Advance selects MPEG mode 4 for streams up to 480x272.
     * Keep the query and create mode aligned with BootAvcMode(4). */
    AVC_STEP("mpeg query memory mode=4");
    size=sceMpegQueryMemSize(4);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","sceMpegQueryMemSize mode=4 result=%08X signed=%d free=%u largest=%u",
        (unsigned)size,size,(unsigned)sceKernelTotalFreeMemSize(),
        (unsigned)sceKernelMaxFreeMemSize());
    diag_log_flush();
#endif
    if(size<=0) { r=size?size:-1; goto fail; }
    s->context=psp_avc_allocate_aligned((size_t)size,64,"AvcMpegCtx",
                                        &s->context_uid);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","MPEG context allocation ptr=%08X uid=%08X bytes=%u",
        (unsigned)s->context,(unsigned)s->context_uid,(unsigned)size);
    diag_log_flush();
#endif
    if(!s->context) { r=-1; goto fail; }
    memset(s->context,0,size); memset(s->ring,0,sizeof(s->ring));
    AVC_STEP("mpeg create");
    r=sceMpegCreate(&s->mpeg,s->context,size,
        (SceMpegRingbuffer*)s->ring,512,4,(int)s->ddr);
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","sceMpegCreate mode=4 result=%08X context=%08X bytes=%u ddr=%08X",
        (unsigned)r,(unsigned)s->context,(unsigned)size,(unsigned)s->ddr);
    diag_log_flush();
#endif
    if(r<0) goto fail;
    s->created=1;
    {
        SceMpegAvcMode mode;
        /* PMFPlayer and the PSPSDK ABI use -1 plus the GE pixel format.
         * Without this explicit mode the firmware may retain the previous
         * VSH output mode even though DecodeDetail2/CSC are used below. */
        mode.iUnk0 = -1;
        mode.iPixelFormat = SCE_MPEG_AVC_FORMAT_8888;
        AVC_STEP("set AVC output mode");
        r=sceMpegAvcDecodeMode(&s->mpeg,&mode);
        if(r<0) goto fail;
    }
    memset(s->au,0xff,sizeof(s->au));
    r=sceMpegInitAu(&s->mpeg,(char*)s->ddr+0x10000,(SceMpegAu*)s->au);
    if(r<0) goto fail;
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","session buffers context=%08X bytes=%u ddr=%08X mpeg=%08X au=%08X",
        (unsigned)s->context,(unsigned)size,(unsigned)s->ddr,
        (unsigned)s->mpeg,(unsigned)s->au);
    diag_log_flush();
#endif
    AVC_STEP("ready");
    s->ready=1; return 0;
fail:
#if defined(PSP_HARDWARE_AVC) && PSP_HARDWARE_AVC
    diag_log_write("AVC","session open failure result=%08X context=%08X ddr=%08X initialized=%d created=%d",
        (unsigned)r,(unsigned)s->context,(unsigned)s->ddr,
        s->initialized,s->created);
    diag_log_flush();
#endif
    cleanup=psp_avc_session_close(s);
    /* Preserve ownership flags when cleanup fails; caller must inspect/retry
       close before fallback, even when open itself returned an error. */
    return cleanup<0?cleanup:r;
}
