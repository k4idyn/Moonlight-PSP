#ifndef MOONLIGHT_PSP_AVC_SPS_FIXUP_H
#define MOONLIGHT_PSP_AVC_SPS_FIXUP_H
#include <stddef.h>

/* Converts High-profile SPS data to Main only when every High-only feature
 * is disabled by the parameter sets (4:2:0, 8-bit, no scaling matrices or
 * transform bypass). The caller must also validate the PPS before decode.
 * Returns 0 on success and sets converted to 1 only for a High->Main rewrite. */
int avc_sps_normalize_main(const unsigned char *src,size_t src_size,
                           unsigned char *dst,size_t dst_capacity,
                           size_t *dst_size,int *converted);
/* Accept only a Main-compatible PPS with no High-profile PPS extension.
 * entropy_coding_mode receives 1 for CABAC, 0 for CAVLC. */
int avc_pps_validate_main(const unsigned char *nal,size_t size,
                          unsigned int expected_sps_id,
                          int *entropy_coding_mode);
int avc_pps_validate_main_ex(const unsigned char *nal,size_t size,
                             unsigned int expected_sps_id,
                             int *entropy_coding_mode,
                             unsigned int *pps_id);

/* Rewrites only max_num_ref_frames in a Baseline/Main SPS. The rest of the
 * RBSP, including VUI timing and cropping, is copied bit for bit. Returns 0
 * on success and leaves the caller's input untouched. */
int avc_sps_limit_refs(const unsigned char *src,size_t src_size,
                       unsigned char *dst,size_t dst_capacity,
                       size_t *dst_size,unsigned int *original_refs);
#endif
