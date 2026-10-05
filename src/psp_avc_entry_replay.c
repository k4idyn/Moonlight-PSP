/* Diagnostics-only differential: the real Moonlight image, heap, renderer and
 * 0x21/VFPU/16 KB decoder worker, without RTSP/control/video receive workers.
 * Keep the Sony session and displayed frame owned until the harness resets;
 * this probe makes no streaming or teardown acceptance claim. */
#include <pspkernel.h>
#include <stdio.h>
#include "psp_avc_player.h"
#include "diag_log.h"
#include "shared.h"
#include "config.h"
#include "ui_manager.h"

extern PspConfig g_psp_config;
extern void display_frame(void *frame);
extern void display_frame_finish(void);
static unsigned char sample[8192] __attribute__((aligned(64)));
static unsigned char prefix[2048] __attribute__((aligned(64)));
static unsigned int sample_bytes,prefix_bytes;
static volatile int replay_done,replay_result;

static unsigned int read_sample(const char *path,unsigned char *data,unsigned int cap) {
    FILE *f=fopen(path,"rb");
    size_t bytes;
    if(!f) return 0;
    bytes=fread(data,1,cap,f);
    if(ferror(f) || !feof(f)) bytes=0;
    fclose(f);
    return (unsigned int)bytes;
}
static int replay_worker(SceSize args,void *argp) {
    int i,r=-1;
    (void)args; (void)argp;
    for(i=0;i<3;i++) {
        r=psp_avc_player_decode(prefix,prefix_bytes);
        diag_log_write("AVC","entry replay prefix=%d result=%08X\n",i+1,(unsigned)r);
        diag_log_flush();
        if(r!=2) goto done;
    }
    for(i=0;i<2;i++) {
        diag_log_write("AVC","entry replay decode begin=%d thread=%08X stack_free=%d\n",
            i+1,(unsigned)sceKernelGetThreadId(),sceKernelCheckThreadStack());
        diag_log_flush();
        r=psp_avc_player_decode(sample,sample_bytes);
        diag_log_write("AVC","entry replay decode end=%d result=%08X\n",i+1,(unsigned)r);
        diag_log_flush();
        if(r!=0 && r!=3) goto done;
    }
done:
    replay_result=r;
    replay_done=1;
    return 0;
}
int psp_avc_entry_replay(void) {
    SceUID worker;
    int result,reported=0;
    unsigned int sequence=0;
    unsigned int decoded_at_us=0;
    void *frame=NULL;
    g_psp_config.width=300;
    g_psp_config.height=170;
    result=psp_avc_player_open(300,170,NULL);
    if(result) return result;
    sample_bytes=read_sample("host0:/experiments/avc_probe/avc_failure_bootbridge_vq_20260928_raw.h264",
        sample,sizeof(sample));
    prefix_bytes=read_sample("host0:/experiments/avc_probe/avc_first_au_bootbridge_vq_20260928_raw.h264",
        prefix,sizeof(prefix));
    diag_log_write("AVC","entry replay inputs sample=%u prefix=%u no RTSP workers\n",
        sample_bytes,prefix_bytes);
    diag_log_flush();
    if(sample_bytes!=2072 || prefix_bytes!=1032) return -1;
    worker=sceKernelCreateThread("AvcEntryReplay",replay_worker,33,16*1024,
        PSP_THREAD_ATTR_USER|PSP_THREAD_ATTR_VFPU,NULL);
    if(worker<0) return worker;
    result=sceKernelStartThread(worker,0,NULL);
    if(result<0) { sceKernelDeleteThread(worker); return result; }
    for(;;) {
        if(!frame) frame=psp_avc_player_take(&sequence,&decoded_at_us);
        if(frame) {
            display_frame(frame);
            display_frame_finish();
            if(!reported) {
                diag_log_write("AVC","entry replay frame displayed sequence=%u pixels=%08X hold until reset\n",
                    sequence,(unsigned)frame);
                diag_log_flush();
                reported=1;
            }
        } else {
            ui_begin_frame();
            ui_draw_gradient_bg(UI_COL_BG_TOP,UI_COL_BG_BOT);
            ui_draw_header("Hardware AVC probe");
            ui_draw_text_centered(0.0f,480.0f,130.0f,UI_COL_TEXT,
                replay_done?"Decode completed without a picture":"Waiting for hardware decode...");
            ui_end_frame();
        }
        if(replay_done==1) {
            diag_log_write("AVC","entry replay worker complete result=%08X displayed=%d\n",
                (unsigned)replay_result,reported);
            diag_log_flush();
            replay_done=2;
        }
        sceKernelDelayThread(10000);
    }
}
