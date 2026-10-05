#ifndef MOONLIGHT_PSP_AVC_SESSION_H
#define MOONLIGHT_PSP_AVC_SESSION_H
#include <stddef.h>
#include <pspmpeg.h>
/* Exclusive, serialized ownership of Sony MPEG/ME. Never coexist with InitME.
 * Call close only after the decode worker has joined, never from its watchdog.
 * Zero-initialize before first use. Nonzero close means resources are retained:
 * do not free this object or initialize a software ME fallback in that case. */
typedef struct {
    SceMpeg mpeg;
    void *context, *ddr;
    int context_uid;
    int avcodec, vsh_loaded, vsh_started, helper_loaded, helper_started;
    int vsh_uid, helper_uid, initialized, created, ready;
    int baseline_booted;
    int main_profile;
    /* Diagnostic bridge reservation is borrowed; never pass it to free(). */
    int ddr_borrowed;
    /* sceMpegAvcDecode's iInit/status argument is in/out across AUs. */
    int decode_state;
    unsigned char ring[128] __attribute__((aligned(64)));
    unsigned char au[64] __attribute__((aligned(64)));
} PspAvcSession;
/* Prefer the C heap, then use a tracked PSP partition block when it cannot
 * satisfy an aligned AVC allocation. A UID of -1 means ordinary heap memory. */
void *psp_avc_allocate_aligned(size_t size,size_t alignment,const char *name,
                               int *allocation_uid);
int psp_avc_release_aligned(void *ptr,int allocation_uid);
/* helper_path selects the verified Baseline boot4 helper. NULL keeps Main boot3.
 * The preloaded-stack path borrows resident providers without unloading them;
 * otherwise close releases only the modules loaded by this session. */
int psp_avc_session_open(PspAvcSession *s,const char *helper_path,
                         int main_profile);
int psp_avc_session_close(PspAvcSession *s);
/* Recreate the MPEG/AVC context while changing Baseline/Main mode once.
 * Caller must be the UI thread with no decode or present operation active. */
int psp_avc_session_switch_profile(PspAvcSession *s,const char *helper_path,
                                   int main_profile);
#endif
