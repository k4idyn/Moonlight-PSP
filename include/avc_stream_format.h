#ifndef MOONLIGHT_AVC_STREAM_FORMAT_H
#define MOONLIGHT_AVC_STREAM_FORMAT_H
#include <stddef.h>
typedef struct {
    unsigned int profile,level,refs,sps_id;
    unsigned int coded_width,coded_height,width,height,crop_left,crop_top;
} AvcStreamFormat;
/* SPS NAL including header, without AnnexB prefix. Bounded Baseline/Main,
 * progressive 4:2:0 format inspection for the tested PSP mode4 path.
 * Does not validate VUI, PPS or slice syntax and does not prove absence of B
 * pictures. Returns -1 malformed, -2 unsupported, zero on success. */
int avc_stream_format(const unsigned char *nal,size_t size,AvcStreamFormat *out);
#endif
