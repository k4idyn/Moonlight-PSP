#ifndef MOONLIGHT_PSP_AVC_BACKEND_H
#define MOONLIGHT_PSP_AVC_BACKEND_H
#include "avc_stream_input.h"
#include "avc_stream_format.h"
#include "psp_avc_submit.h"
typedef struct {
    PspAvcSession session;
    AvcStreamInput input;
    AvcStreamFormat format;
    unsigned char *avcc;
    int avcc_uid;
    unsigned char source_sps[1024];
    unsigned char source_pps[1024];
    unsigned char firmware_sps[1024] __attribute__((aligned(64)));
    size_t source_sps_size,source_pps_size,firmware_sps_size;
    int source_high_profile;
    int source_entropy_mode; /* -1 unknown, 0 CAVLC, 1 CABAC */
    const char *baseline_helper; /* immutable, valid until close */
    int main_profile;
    int first_idr_submitted;
    int profile_change_pending,requested_main_profile;
    unsigned int expected_width,expected_height;
    int failed;
} PspAvcBackend;
#ifndef PSP_AVC_PROFILE_CHANGE_REQUIRED
#define PSP_AVC_PROFILE_CHANGE_REQUIRED 4
#endif
/* Caller serializes all calls, owns three writable output slots and must not
 * run a custom ME worker. Zero initialize the backend before first open. */
int psp_avc_backend_open(PspAvcBackend *b,unsigned int width,unsigned int height,
                         const char *baseline_helper,int main_profile);
/* Zero means submitted successfully; pictures.count can still be zero.
 * Positive values are AVC_INPUT_PARAMETERS/NEED_IDR (no submission).
 * Negative errors require reset or shutdown, never blind continued decode.
 * Returned slots remain caller-owned and must be released by presentation
 * before they can be supplied again. No P-frame early drain is performed. */
int psp_avc_backend_decode(PspAvcBackend *b,const unsigned char *annexb,size_t size,
                           PspAvcPictures *pictures);
/* Switch only before the first accepted IDR, on the serialized UI thread. */
int psp_avc_backend_switch_profile(PspAvcBackend *b,int main_profile);
/* Must run on the decoder thread with no active firmware call. Discards
 * references and queued firmware output, preserves cached parameter sets. */
int psp_avc_backend_reset(PspAvcBackend *b);
/* Join decoder worker before close. Nonzero means resources retained. */
int psp_avc_backend_close(PspAvcBackend *b);
#endif
