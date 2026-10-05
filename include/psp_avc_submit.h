#ifndef MOONLIGHT_PSP_AVC_SUBMIT_H
#define MOONLIGHT_PSP_AVC_SUBMIT_H
#include "psp_avc_session.h"
#include "avc_packetizer.h"
#include "avc_stream_format.h"
/* All three slots must be writable, distinct, 64-byte aligned allocations of
 * at least 512*272*4 bytes, not currently owned by the presenter. Caller owns
 * their lifetime. count is zero on failure (partially written pixels invalid).
 * format retains both display crop and coded macroblock dimensions. */
typedef struct { void *rgba[3]; int count; } PspAvcPictures;
int psp_avc_submit(PspAvcSession *s,const AvcPacketizedAu *au,
    void *avcc,PspAvcPictures *pictures);
/* End of stream, or immediately after an independently decoded IDR only.
 * Never drain between reference-dependent P frames. Caller enforces policy. */
int psp_avc_drain(PspAvcSession *s,const AvcStreamFormat *format,
                  PspAvcPictures *pictures);
#endif
