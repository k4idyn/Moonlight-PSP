/*
 * sw_cavlc.c - CAVLC Entropy Decoder for H.264 Baseline Profile
 *
 * Runs on the Main CPU (333MHz Allegrex). This is the "Front End" of the
 * asymmetric pipeline. CAVLC (Context-Adaptive Variable-Length Coding) is
 * strictly sequential — you cannot know where one codeword ends without
 * reading the previous one.
 *
 * Input:  Raw Annex-B H.264 bitstream from Sunshine (via RTP reassembly)
 * Output: SwMacroblockData array with DCT coefficients + motion vectors
 *         ready for the Media Engine's VFPU to reconstruct.
 *
 * Covers:
 *   - Exp-Golomb coded syntax elements (SPS, PPS, slice header)
 *   - CAVLC residual coefficient decoding
 *   - Macroblock layer parsing (mb_type, CBP, QP, intra modes, MVs)
 *   - NAL unit scanning and emulation prevention byte removal
 *
 * H.264 Spec References:
 *   - ITU-T H.264 / ISO 14496-10 (2003 base spec)
 *   - Section 7.3: NAL unit syntax
 *   - Section 7.4: Semantics
 *   - Section 9.2: CAVLC parsing
 *   - Table 9-5: coeff_token mapping
 */

#include <string.h>
#include <stdlib.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include "sw_decode_pipeline.h"
#include "diag_log.h"
#include "storage_paths.h"

/* Per-block verbose logging — set by frame decode around failure zone */
static int g_cavlc_blk_verbose = 0;
static u32 g_cavlc_frame_count = 0;

#define cavlc_log(fmt, ...) diag_log_write("CAVLC", fmt, ##__VA_ARGS__)

/* ============================================================================
 * Bitstream Reader — Cache-based, MIPS-optimized
 *
 * Loads up to 32 bits into a register for fast bit extraction.
 * MIPS CLZ instruction accelerates Exp-Golomb decoding.
 * ============================================================================*/

static inline void bs_refill(SwBitstream *bs)
{
    while (bs->bits_left <= 24 && bs->byte_pos < bs->size) {
        u32 bv = bs->data[bs->byte_pos++];
        bs->cache |= bv << (24 - bs->bits_left);
        bs->bits_left += 8;
    }
}

void bs_init(SwBitstream *bs, const u8 *data, int size)
{
    bs->data = data;
    bs->size = size;
    bs->byte_pos = 0;
    bs->cache = 0;
    bs->bits_left = 0;
    bs_refill(bs);
}

static inline int bs_eof(const SwBitstream *bs)
{
    return bs->bits_left <= 0 && bs->byte_pos >= bs->size;
}

static inline int bs_bits_remaining(const SwBitstream *bs)
{
    return bs->bits_left + (bs->size - bs->byte_pos) * 8;
}

/* Byte position approximation for logging */
static inline int bs_byte_offset(const SwBitstream *bs)
{
    return bs->byte_pos - (bs->bits_left + 7) / 8;
}

/* Read a single bit — hot path, inlined */
static inline u32 bs_read1(SwBitstream *bs)
{
    if (__builtin_expect(bs->bits_left <= 0, 0)) {
        bs_refill(bs);
        if (bs->bits_left <= 0) return 0;
    }
    u32 val = bs->cache >> 31;
    bs->cache <<= 1;
    bs->bits_left--;
    return val;
}

/* Read up to 25 bits from cache — MIPS shift pair */
static u32 bs_read(SwBitstream *bs, int n)
{
    if (n == 0) return 0;
    bs_refill(bs);
    if (__builtin_expect(n > bs->bits_left, 0)) {
        /* Straddles two refills — consume available then refill */
        int have = bs->bits_left;
        u32 hi = (have > 0) ? (bs->cache >> (32 - have)) : 0;
        bs->cache = 0;
        bs->bits_left = 0;
        n -= have;
        bs_refill(bs);
        if (n > bs->bits_left) n = bs->bits_left;
        u32 lo = bs->cache >> (32 - n);
        bs->cache <<= n;
        bs->bits_left -= n;
        return (hi << n) | lo;
    }
    u32 val;
    int shift_r = 32 - n;
    /* MIPS: extract top N bits, shift cache left by N
     * NOTE: +&r (early clobber) on cache prevents GCC from aliasing
     * [n] or [s] with [c], which would corrupt the shift amount. */
    __asm__ volatile(
        "srlv  %[v], %[c], %[s]  \n\t"
        "sllv  %[c], %[c], %[n]  \n\t"
        : [v] "=&r"(val), [c] "+&r"(bs->cache)
        : [s] "r"(shift_r), [n] "r"(n)
    );
    bs->bits_left -= n;
    return val;
}

/* Read unsigned Exp-Golomb coded value (9.1) — CLZ fast path */
static u32 bs_read_ue(SwBitstream *bs)
{
    bs_refill(bs);
    if (bs->bits_left <= 0) return 0;

    if (__builtin_expect(bs->cache != 0, 1)) {
        /* Fast path: MIPS CLZ to count leading zeros */
        int lz;
        __asm__ volatile("clz %0, %1" : "=r"(lz) : "r"(bs->cache));
        int total = lz + lz + 1;
        if (__builtin_expect(total <= bs->bits_left, 1)) {
            u32 code = bs->cache >> (32 - total);
            bs->cache <<= total;
            bs->bits_left -= total;
            return code - 1;
        }
        /* Codeword straddles cache boundary */
        if (lz + 1 <= bs->bits_left) {
            bs->cache <<= (lz + 1);
            bs->bits_left -= (lz + 1);
            if (lz == 0) return 0;
            u32 val = bs_read(bs, lz);
            return (1u << lz) - 1 + val;
        }
    }

    /* Slow path: near EOF or all-zero cache */
    int leading_zeros = 0;
    while (!bs_eof(bs) && bs_read1(bs) == 0) {
        leading_zeros++;
        if (leading_zeros > 31) return 0;
    }
    if (leading_zeros == 0) return 0;
    u32 val = bs_read(bs, leading_zeros);
    return (1u << leading_zeros) - 1 + val;
}

/* Read signed Exp-Golomb coded value (9.1.1) */
static s32 bs_read_se(SwBitstream *bs)
{
    u32 code = bs_read_ue(bs);
    if (code & 1) {
        return (s32)((code + 1) >> 1);
    } else {
        return -(s32)(code >> 1);
    }
}

/* Read trailing bits (byte-align) */
static void __attribute__((unused)) bs_read_trailing(SwBitstream *bs)
{
    bs_read1(bs); /* rbsp_stop_one_bit (should be 1) */
    /* Align: discard remaining bits in the current byte */
    int leftover = bs->bits_left & 7;
    if (leftover > 0) {
        bs->cache <<= leftover;
        bs->bits_left -= leftover;
    }
}

/* Skip n bits in the bitstream */
static void __attribute__((unused)) bs_skip(SwBitstream *bs, int n)
{
    while (n > 0) {
        bs_refill(bs);
        int eat = (n < bs->bits_left) ? n : bs->bits_left;
        bs->cache <<= eat;
        bs->bits_left -= eat;
        n -= eat;
        if (bs->bits_left <= 0 && bs->byte_pos >= bs->size) break;
    }
}

/* ============================================================================
 * NAL Unit Scanner — Finds start codes and removes emulation prevention bytes
 *
 * Sunshine sends Annex-B format: [00 00 00 01] [NAL header] [RBSP ...]
 * Within the RBSP, sequences [00 00 03 xx] contain emulation prevention
 * byte 0x03 which must be stripped to get the real RBSP data.
 * ============================================================================*/

typedef struct {
    const u8 *data;     /* Pointer to NAL payload (after start code) */
    int       size;     /* Size of NAL payload */
    u8        type;     /* NAL unit type (5 bits) */
    u8        ref_idc;  /* nal_ref_idc (2 bits) */
} SwNALUnit;

/* Remove emulation prevention bytes: 00 00 03 → 00 00 */
static int nal_remove_epb(const u8 *src, int src_len, u8 *dst, int dst_max)
{
    int si = 0, di = 0;
    while (si < src_len && di < dst_max) {
        if (si + 2 < src_len &&
            src[si] == 0x00 && src[si+1] == 0x00 && src[si+2] == 0x03) {
            if (di + 1 < dst_max) {
                dst[di++] = 0x00;
                dst[di++] = 0x00;
            }
            si += 3; /* Skip the 0x03 emulation prevention byte */
        } else {
            dst[di++] = src[si++];
        }
    }
    return di;
}

/* Scan Annex-B bitstream for NAL units. Returns count found. */
static int nal_scan_annexb(const u8 *data, int len,
                           SwNALUnit *units, int max_units)
{
    int count = 0;
    int i = 0;

    while (i < len - 2 && count < max_units) {
        /* Look for 3-byte or 4-byte start code */
        int sc_len = 0;
        if (data[i] == 0 && data[i+1] == 0 && data[i+2] == 1) {
            sc_len = 3;
        } else if (i + 3 < len && data[i] == 0 && data[i+1] == 0 &&
                   data[i+2] == 0 && data[i+3] == 1) {
            sc_len = 4;
        }

        if (sc_len > 0) {
            int nal_start = i + sc_len;
            if (nal_start >= len) break;

            /* Find end of this NAL (start of next, or end of buffer) */
            int nal_end = len;
            for (int j = nal_start + 1; j < len - 2; j++) {
                if (data[j] == 0 && data[j+1] == 0 &&
                    (data[j+2] == 1 ||
                     (j + 3 < len && data[j+2] == 0 && data[j+3] == 1))) {
                    nal_end = j;
                    break;
                }
            }

            u8 nal_header = data[nal_start];
            units[count].data = data + nal_start + 1; /* Skip NAL header byte */
            units[count].size = nal_end - nal_start - 1;
            units[count].type = nal_header & 0x1F;
            units[count].ref_idc = (nal_header >> 5) & 0x03;
            count++;

            i = nal_end;
        } else {
            i++;
        }
    }

    return count;
}

/* ============================================================================
 * SPS Parser (Sequence Parameter Set) — NAL Type 7
 *
 * We only parse fields needed for Baseline profile decode at 480x272.
 * Sunshine should send: profile_idc=66, level=21, 30x17 MBs.
 * ============================================================================*/

static int parse_sps(SwBitstream *bs, SwSPS *sps)
{
    memset(sps, 0, sizeof(SwSPS));

    sps->profile_idc = (u8)bs_read(bs, 8);
    sps->constraint_set_flags = (u8)bs_read(bs, 8);
    sps->level_idc = (u8)bs_read(bs, 8);

    u32 sps_id = bs_read_ue(bs); /* seq_parameter_set_id */
    (void)sps_id;

    /* High profile extensions (not expected for Baseline, but handle gracefully) */
    if (sps->profile_idc == 100 || sps->profile_idc == 110 ||
        sps->profile_idc == 122 || sps->profile_idc == 244 ||
        sps->profile_idc == 44  || sps->profile_idc == 83  ||
        sps->profile_idc == 86  || sps->profile_idc == 118) {
        sps->chroma_format_idc = (u8)bs_read_ue(bs);
        if (sps->chroma_format_idc == 3) {
            bs_read1(bs); /* separate_colour_plane_flag */
        }
        bs_read_ue(bs); /* bit_depth_luma_minus8 */
        bs_read_ue(bs); /* bit_depth_chroma_minus8 */
        bs_read1(bs);   /* qpprime_y_zero_transform_bypass */
        u32 seq_scaling_matrix = bs_read1(bs);
        if (seq_scaling_matrix) {
            int cnt = (sps->chroma_format_idc != 3) ? 8 : 12;
            for (int i = 0; i < cnt; i++) {
                if (bs_read1(bs)) { /* seq_scaling_list_present */
                    int size = (i < 6) ? 16 : 64;
                    int last = 8, next = 8;
                    for (int j = 0; j < size; j++) {
                        if (next != 0) {
                            s32 delta = bs_read_se(bs);
                            next = (last + delta + 256) % 256;
                        }
                        last = (next == 0) ? last : next;
                    }
                }
            }
        }
    } else {
        sps->chroma_format_idc = 1; /* 4:2:0 default for Baseline */
    }

    sps->log2_max_frame_num = (u8)(bs_read_ue(bs) + 4);
    sps->pic_order_cnt_type = (u8)bs_read_ue(bs);

    if (sps->pic_order_cnt_type == 0) {
        sps->log2_max_pic_order_cnt = (u8)(bs_read_ue(bs) + 4);
    } else if (sps->pic_order_cnt_type == 1) {
        bs_read1(bs);   /* delta_pic_order_always_zero */
        bs_read_se(bs); /* offset_for_non_ref_pic */
        bs_read_se(bs); /* offset_for_top_to_bottom_field */
        u32 n = bs_read_ue(bs);
        for (u32 i = 0; i < n; i++) bs_read_se(bs);
    }

    sps->num_ref_frames = (u8)bs_read_ue(bs);
    bs_read1(bs); /* gaps_in_frame_num_allowed */

    sps->pic_width_in_mbs = (u16)(bs_read_ue(bs) + 1);
    sps->pic_height_in_map_units = (u16)(bs_read_ue(bs) + 1);

    sps->frame_mbs_only_flag = (u8)bs_read1(bs);
    if (!sps->frame_mbs_only_flag) {
        bs_read1(bs); /* mb_adaptive_frame_field */
    }

    sps->direct_8x8_inference_flag = (u8)bs_read1(bs);

    sps->frame_cropping_flag = (u8)bs_read1(bs);
    if (sps->frame_cropping_flag) {
        sps->crop_left   = (u16)bs_read_ue(bs);
        sps->crop_right  = (u16)bs_read_ue(bs);
        sps->crop_top    = (u16)bs_read_ue(bs);
        sps->crop_bottom = (u16)bs_read_ue(bs);
    }

    sps->vui_parameters_present = (u8)bs_read1(bs);
    /* Skip VUI parsing — not needed for decode */

    sps->valid = 1;

    cavlc_log("SPS: profile=%d level=%d %dx%d MBs chroma=%d refs=%d\n",
              sps->profile_idc, sps->level_idc,
              sps->pic_width_in_mbs, sps->pic_height_in_map_units,
              sps->chroma_format_idc, sps->num_ref_frames);
    return 0;
}

/* ============================================================================
 * PPS Parser (Picture Parameter Set) — NAL Type 8
 * ============================================================================*/

static int parse_pps(SwBitstream *bs, SwPPS *pps, const SwSPS *sps)
{
    memset(pps, 0, sizeof(SwPPS));

    pps->pps_id = (u8)bs_read_ue(bs);
    pps->sps_id = (u8)bs_read_ue(bs);
    pps->entropy_coding_mode = (u8)bs_read1(bs); /* 0 = CAVLC */
    pps->pic_order_present_flag = (u8)bs_read1(bs);

    u32 num_slice_groups = bs_read_ue(bs) + 1;
    if (num_slice_groups > 1) {
        /* Baseline allows slice groups but Sunshine won't send them */
        cavlc_log("PPS: slice groups > 1 not supported\n");
        return -1;
    }

    pps->num_ref_idx_l0 = (u8)(bs_read_ue(bs) + 1);
    bs_read_ue(bs); /* num_ref_idx_l1_default_active_minus1 */

    pps->weighted_pred_flag = (u8)bs_read1(bs);
    pps->weighted_bipred_idc = (u8)bs_read(bs, 2);

    pps->pic_init_qp = (s8)(bs_read_se(bs) + 26);
    bs_read_se(bs); /* pic_init_qs_minus26 */
    pps->chroma_qp_index_offset = (s8)bs_read_se(bs);

    pps->deblocking_filter_control = (u8)bs_read1(bs);
    pps->constrained_intra_pred = (u8)bs_read1(bs);
    pps->redundant_pic_cnt_present = (u8)bs_read1(bs);

    cavlc_log("PPS: id=%d sps=%d entropy=%s qp=%d refs=%d\n",
              pps->pps_id, pps->sps_id,
              pps->entropy_coding_mode ? "CABAC" : "CAVLC",
              pps->pic_init_qp, pps->num_ref_idx_l0);

    if (pps->entropy_coding_mode != 0) {
        cavlc_log("ERROR: CABAC not supported, need CAVLC (Baseline)\n");
        extern volatile int g_cabac_detected;
        g_cabac_detected = 1;
        pps->valid = 0; /* Mark invalid so decoder won't use CABAC PPS */
        return -1;
    }

    pps->valid = 1;
    return 0;
}

/* ============================================================================
 * Slice Header Parser
 * ============================================================================*/

static int parse_slice_header(SwBitstream *bs, SwSliceHeader *sh,
                              const SwSPS *sps, const SwPPS *pps,
                              int nal_type, int nal_ref_idc)
{
    memset(sh, 0, sizeof(SwSliceHeader));

    sh->idr_flag = (nal_type == SW_NAL_IDR) ? 1 : 0;

    sh->first_mb_in_slice = bs_read_ue(bs);
    u32 slice_type_raw = bs_read_ue(bs);

    /* Map slice types: 0,5=P, 1,6=B, 2,7=I, 3,8=SP, 4,9=SI */
    if (slice_type_raw > 9) {
        cavlc_log("Invalid slice_type %u\n", slice_type_raw);
        return -1;
    }
    if (slice_type_raw >= 5) slice_type_raw -= 5;
    sh->slice_type = (u8)slice_type_raw;

    sh->pps_id = (u8)bs_read_ue(bs);
    sh->frame_num = (u16)(bs_read(bs, sps->log2_max_frame_num));

    if (sh->idr_flag) {
        sh->idr_pic_id = (u16)bs_read_ue(bs);
    }

    /* pic_order_cnt (type 0 only) */
    if (sps->pic_order_cnt_type == 0) {
        bs_read(bs, sps->log2_max_pic_order_cnt); /* pic_order_cnt_lsb */
        if (pps->pic_order_present_flag) {
            bs_read_se(bs); /* delta_pic_order_cnt_bottom — always present for
                             * progressive (field_pic_flag=0) per H.264 §7.3.3 */
        }
    }

    /* num_ref_idx_active_override + ref_pic_list_reordering */
    if (sh->slice_type == SW_SLICE_P) {
        sh->num_ref_idx_l0_active = pps->num_ref_idx_l0;
        /* H.264 7.3.3: num_ref_idx_active_override_flag */
        u32 override_flag = bs_read1(bs);
        if (override_flag) {
            sh->num_ref_idx_l0_active = (u8)(bs_read_ue(bs) + 1);
        }
        u32 ref_pic_list_reorder = bs_read1(bs);
        if (ref_pic_list_reorder) {
            u32 reorder_op;
            do {
                reorder_op = bs_read_ue(bs);
                if (reorder_op < 3) {
                    bs_read_ue(bs); /* abs_diff_pic_num_minus1 or long_term_pic_num */
                }
            } while (reorder_op != 3 && !bs_eof(bs));
        }
    }

    /* dec_ref_pic_marking */
    if (nal_ref_idc != 0) {
        if (sh->idr_flag) {
            bs_read1(bs); /* no_output_of_prior_pics */
            bs_read1(bs); /* long_term_reference_flag */
        } else {
            u32 adaptive = bs_read1(bs);
            if (adaptive) {
                u32 op;
                do {
                    op = bs_read_ue(bs);
                    if (op == 1 || op == 3) bs_read_ue(bs);
                    if (op == 2) bs_read_ue(bs);
                    if (op == 3 || op == 6) bs_read_ue(bs);
                    if (op == 4) bs_read_ue(bs);
                    if (op == 5) { /* Reset */ }
                } while (op != 0 && !bs_eof(bs));
            }
        }
    }

    /* slice_qp_delta */
    s32 qp_delta = bs_read_se(bs);
    sh->slice_qp = (s8)(pps->pic_init_qp + qp_delta);

    /* deblocking filter */
    if (pps->deblocking_filter_control) {
        sh->disable_deblocking_filter = (u8)bs_read_ue(bs);
        if (sh->disable_deblocking_filter != 1) {
            sh->slice_alpha_c0_offset = (s8)(bs_read_se(bs) << 1);
            sh->slice_beta_offset = (s8)(bs_read_se(bs) << 1);
        }
    }

    return 0;
}

/* ============================================================================
 * CAVLC Tables — H.264 Table 9-5 (coeff_token)
 *
 * Table-driven VLC decode using the canonical ITU-T H.264 Table 9-5 data.
 * Index: total_coeff * 4 + trailing_ones
 * Tables sourced from FFmpeg libavcodec/h264_cavlc.c (public domain reference).
 * ============================================================================*/

/* Table 9-5(a): nC = 0, 1 — code lengths (max 16 bits) */
static const u8 ct_len0[68] = {
     1, 0, 0, 0,     6, 2, 0, 0,     8, 6, 3, 0,     9, 8, 7, 5,
    10, 9, 8, 6,    11,10, 9, 7,    13,11,10, 8,    13,13,11, 9,
    13,13,13,10,    14,14,13,11,    14,14,14,13,    15,15,14,14,
    15,15,15,14,    16,15,15,15,    16,16,16,15,    16,16,16,16,
    16,16,16,16
};
static const u16 ct_bits0[68] = {
     1, 0, 0, 0,     5, 1, 0, 0,     7, 4, 1, 0,     7, 6, 5, 3,
     7, 6, 5, 3,     7, 6, 5, 4,    15, 6, 5, 4,    11,14, 5, 4,
     8,10,13, 4,    15,14, 9, 4,    11,10,13,12,    15,14, 9,12,
    11,10,13, 8,    15, 1, 9,12,    11,14,13, 8,     7,10, 9,12,
     4, 6, 5, 8
};

/* Table 9-5(b): nC = 2, 3 — code lengths (max 14 bits) */
static const u8 ct_len1[68] = {
     2, 0, 0, 0,     6, 2, 0, 0,     6, 5, 3, 0,     7, 6, 6, 4,
     8, 6, 6, 4,     8, 7, 7, 5,     9, 8, 8, 6,    11, 9, 9, 6,
    11,11,11, 7,    12,11,11, 9,    12,12,12,11,    12,12,12,11,
    13,13,13,12,    13,13,13,13,    13,14,13,13,    14,14,14,13,
    14,14,14,14
};
static const u16 ct_bits1[68] = {
     3, 0, 0, 0,    11, 2, 0, 0,     7, 7, 3, 0,     7,10, 9, 5,
     7, 6, 5, 4,     4, 6, 5, 6,     7, 6, 5, 8,    15, 6, 5, 4,
    11,14,13, 4,    15,10, 9, 4,    11,14,13,12,     8,10, 9, 8,
    15,14,13,12,    11,10, 9,12,     7,11, 6, 8,     9, 8,10, 1,
     7, 6, 5, 4
};

/* Table 9-5(c): nC = 4..7 — code lengths (max 10 bits) */
static const u8 ct_len2[68] = {
     4, 0, 0, 0,     6, 4, 0, 0,     6, 5, 4, 0,     6, 5, 5, 4,
     7, 5, 5, 4,     7, 5, 5, 4,     7, 6, 6, 4,     7, 6, 6, 4,
     8, 7, 7, 5,     8, 8, 7, 6,     9, 8, 8, 7,     9, 9, 8, 8,
     9, 9, 9, 8,    10, 9, 9, 9,    10,10,10,10,    10,10,10,10,
    10,10,10,10
};
static const u16 ct_bits2[68] = {
    15, 0, 0, 0,    15,14, 0, 0,    11,15,13, 0,     8,12,14,12,
    15,10,11,11,    11, 8, 9,10,     9,14,13, 9,     8,10, 9, 8,
    15,14,13,13,    11,14,10,12,    15,10,13,12,    11,14, 9,12,
     8,10,13, 8,    13, 7, 9,12,     9,12,11,10,     5, 8, 7, 6,
     1, 4, 3, 2
};

/* Table 9-5(d): nC >= 8 — 6-bit fixed-length codes (from FFmpeg) */
static const u8 ct_len3[68] = {
     6, 0, 0, 0,     6, 6, 0, 0,     6, 6, 6, 0,     6, 6, 6, 6,
     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,     6, 6, 6, 6,
     6, 6, 6, 6
};
static const u16 ct_bits3[68] = {
     3, 0, 0, 0,     0, 1, 0, 0,     4, 5, 6, 0,     8, 9,10,11,
    12,13,14,15,    16,17,18,19,    20,21,22,23,    24,25,26,27,
    28,29,30,31,    32,33,34,35,    36,37,38,39,    40,41,42,43,
    44,45,46,47,    48,49,50,51,    52,53,54,55,    56,57,58,59,
    60,61,62,63
};

/* Table 9-5(e): Chroma DC 4:2:0 — max 4 coefficients */
static const u8 ct_len_cdc[20] = {
     2, 0, 0, 0,     6, 1, 0, 0,     6, 6, 3, 0,     6, 7, 7, 6,
     6, 8, 8, 7
};
static const u16 ct_bits_cdc[20] = {
     1, 0, 0, 0,     7, 1, 0, 0,     4, 6, 1, 0,     3, 3, 2, 5,
     2, 3, 2, 0
};

/* Generic VLC table matcher: peek bits from cache, scan entries, match & consume */
static int match_coeff_token(SwBitstream *bs,
                             const u8 *len_tab, const u16 *bits_tab,
                             int max_tc, int max_bits,
                             int *total_coeff, int *trailing_ones)
{
    bs_refill(bs);
    int avail = bs->bits_left;
    int peek_n = (avail < max_bits) ? avail : max_bits;
    if (peek_n <= 0) {
        cavlc_log("coeff_token: no bits (avail=%d)\n", avail);
        return -1;
    }

    u32 code = bs->cache >> (32 - peek_n);

    for (int tc = 0; tc <= max_tc; tc++) {
        int to_max = (tc < 3) ? tc : 3;
        for (int to = 0; to <= to_max; to++) {
            int idx = tc * 4 + to;
            u8 len = len_tab[idx];
            if (len == 0 || len > peek_n) continue;
            u16 bits = bits_tab[idx];
            if ((code >> (peek_n - len)) == (u32)bits) {
                /* Consume matched bits directly from cache */
                bs->cache <<= len;
                bs->bits_left -= len;
                *total_coeff = tc;
                *trailing_ones = to;
                return 0;
            }
        }
    }
    cavlc_log("coeff_token: no match (peek=%d code=0x%04X max_tc=%d max_bits=%d byte~%d)\n",
              peek_n, code, max_tc, max_bits, bs_byte_offset(bs));
    return -1;
}

static int cavlc_decode_coeff_token(SwBitstream *bs, int nC,
                                    int *total_coeff, int *trailing_ones)
{
    *total_coeff = 0;
    *trailing_ones = 0;
    int ret;

    if (nC >= 8) {
        ret = match_coeff_token(bs, ct_len3, ct_bits3,
                                 16, 6, total_coeff, trailing_ones);
    } else if (nC == -1) {
        ret = match_coeff_token(bs, ct_len_cdc, ct_bits_cdc,
                                 4, 8, total_coeff, trailing_ones);
    } else if (nC < 2) {
        ret = match_coeff_token(bs, ct_len0, ct_bits0,
                                 16, 16, total_coeff, trailing_ones);
    } else if (nC < 4) {
        ret = match_coeff_token(bs, ct_len1, ct_bits1,
                                 16, 14, total_coeff, trailing_ones);
    } else {
        ret = match_coeff_token(bs, ct_len2, ct_bits2,
                                 16, 10, total_coeff, trailing_ones);
    }

    if (ret < 0) {
        cavlc_log("coeff_token failed: nC=%d tbl=%s\n",
                  nC, nC >= 8 ? "3(>=8)" : nC == -1 ? "cdc" :
                  nC < 2 ? "0(0-1)" : nC < 4 ? "1(2-3)" : "2(4-7)");
    }
    return ret;
}

/* ============================================================================
 * FFmpeg-ported CAVLC Level Decode — H.264 Section 9.2.2
 *
 * Ported directly from FFmpeg libavcodec/h264_cavlc.c decode_residual().
 * Uses pre-computed cavlc_level_tab for O(1) level lookup.
 * This is the EXACT algorithm used by FFmpeg for 20+ years.
 * ============================================================================*/

/* Pre-computed level decode lookup table — from FFmpeg init_cavlc_level_tab() */
#define LEVEL_TAB_BITS 8
static s8 cavlc_level_tab[7][1 << LEVEL_TAB_BITS][2];
static int cavlc_level_tab_inited = 0;

/* Portable log2 for unsigned int (matches FFmpeg av_log2) */
static inline int av_log2_c(unsigned int v)
{
    int n = 0;
    if (v & 0xffff0000) { v >>= 16; n += 16; }
    if (v & 0xff00)     { v >>= 8;  n += 8; }
    if (v & 0xf0)       { v >>= 4;  n += 4; }
    if (v & 0xc)        { v >>= 2;  n += 2; }
    if (v & 0x2)        {           n += 1; }
    return n;
}

/* Build the level lookup table — called once.
 * Exact copy of FFmpeg's init_cavlc_level_tab(). */
static void ffmpeg_init_level_tab(void)
{
    int suffix_length;
    unsigned int i;
    for (suffix_length = 0; suffix_length < 7; suffix_length++) {
        for (i = 0; i < (1u << LEVEL_TAB_BITS); i++) {
            int prefix = LEVEL_TAB_BITS - av_log2_c(2 * i);
            if (prefix + 1 + suffix_length <= LEVEL_TAB_BITS) {
                int level_code = (prefix << suffix_length) +
                    (int)(i >> (av_log2_c(i) - suffix_length)) -
                    (1 << suffix_length);
                int mask = -(level_code & 1);
                level_code = (((2 + level_code) >> 1) ^ mask) - mask;
                cavlc_level_tab[suffix_length][i][0] = (s8)level_code;
                cavlc_level_tab[suffix_length][i][1] = (s8)(prefix + 1 + suffix_length);
            } else if (prefix + 1 <= LEVEL_TAB_BITS) {
                cavlc_level_tab[suffix_length][i][0] = (s8)(prefix + 100);
                cavlc_level_tab[suffix_length][i][1] = (s8)(prefix + 1);
            } else {
                cavlc_level_tab[suffix_length][i][0] = (s8)(LEVEL_TAB_BITS + 100);
                cavlc_level_tab[suffix_length][i][1] = (s8)LEVEL_TAB_BITS;
            }
        }
    }
    cavlc_level_tab_inited = 1;
}

/* Peek N bits from cache without consuming — FFmpeg show_bits equivalent */
static inline u32 bs_show(SwBitstream *bs, int n)
{
    bs_refill(bs);
    return bs->cache >> (32 - n);
}

/* Consume N bits from cache without reading — FFmpeg skip_bits equivalent */
static inline void bs_skip_fast(SwBitstream *bs, int n)
{
    bs->cache <<= n;
    bs->bits_left -= n;
}

/* Read level_prefix using CLZ — matches FFmpeg get_level_prefix() exactly.
 * FFmpeg: log = 32 - av_log2(buf) = CLZ + 1.  skip(log), return log - 1.
 * So we must skip CLZ+1 (leading zeros + the terminating '1') and return CLZ. */
static inline int ffmpeg_get_level_prefix(SwBitstream *bs)
{
    bs_refill(bs);
    u32 buf = bs->cache;
    int lz;
    if (__builtin_expect(buf != 0, 1)) {
        __asm__ volatile("clz %0, %1" : "=r"(lz) : "r"(buf));
    } else {
        lz = 32;
    }
    /* Safety limit: H.264 prefix can't exceed ~28 */
    if (lz > 25) lz = 25;
    /* Skip the leading zeros AND the terminating '1' bit */
    bs_skip_fast(bs, lz + 1);
    return lz;
}

/* FFmpeg-exact level decode — ported from decode_residual() in h264_cavlc.c */
static int cavlc_decode_levels(SwBitstream *bs, int total_coeff,
                               int trailing_ones, s16 *levels)
{
    if (!cavlc_level_tab_inited) ffmpeg_init_level_tab();

    int suffix_length = (total_coeff > 10) & (trailing_ones < 3);

    /* Trailing ones: FFmpeg reads up to 3 sign bits at once */
    if (trailing_ones > 0) {
        bs_refill(bs);
        u32 sign_bits = bs->cache >> (32 - 3);
        bs_skip_fast(bs, trailing_ones);
        levels[0] = (s16)(1 - ((sign_bits & 4) >> 1));
        levels[1] = (s16)(1 - (sign_bits & 2));
        levels[2] = (s16)(1 - ((sign_bits & 1) << 1));
    }

    if (trailing_ones < total_coeff) {
        int mask, prefix;

        /* --- FIRST non-trailing coefficient (special handling) --- */
        int bitsi = (int)bs_show(bs, LEVEL_TAB_BITS);
        int level_code = (int)cavlc_level_tab[suffix_length][bitsi][0];
        bs_skip_fast(bs, (int)cavlc_level_tab[suffix_length][bitsi][1]);

        if (level_code >= 100) {
            /* Slow path: prefix too long for 8-bit lookup */
            prefix = level_code - 100;
            if (prefix == LEVEL_TAB_BITS)
                prefix += ffmpeg_get_level_prefix(bs);

            /* First coefficient: suffix_length is 0 or 1 */
            if (prefix < 14) {
                if (suffix_length)
                    level_code = (prefix << 1) + (int)bs_read1(bs);
                else
                    level_code = prefix;
            } else if (prefix == 14) {
                if (suffix_length)
                    level_code = (prefix << 1) + (int)bs_read1(bs);
                else
                    level_code = prefix + (int)bs_read(bs, 4);
            } else {
                /* prefix >= 15: FFmpeg uses level_code = 30 (NOT 15!)
                 * This matches H.264 spec: levelCode += 15 when
                 * prefix >= 15 && suffixLength == 0 */
                level_code = 30;
                if (prefix >= 16) {
                    if (prefix > 25 + 3) return -1;
                    level_code += (1 << (prefix - 3)) - 4096;
                }
                level_code += (int)bs_read(bs, prefix - 3);
            }

            if (trailing_ones < 3) level_code += 2;

            suffix_length = 2;
            mask = -(level_code & 1);
            levels[trailing_ones] = (s16)((((2 + level_code) >> 1) ^ mask) - mask);
        } else {
            /* Fast path: small level resolved from lookup table.
             * level_code is already the SIGNED level value.
             * Adjust: add +1 (positive) or -1 (negative) for first coeff */
            level_code += ((level_code >> 31) | 1) & -(trailing_ones < 3);
            suffix_length = 1 + ((unsigned)(level_code + 3) > 6U);
            levels[trailing_ones] = (s16)level_code;
        }

        /* --- REMAINING coefficients --- */
        static const unsigned int suffix_limit[7] = {
            0, 3, 6, 12, 24, 48, 0x7FFFFFFFU
        };

        for (int i = trailing_ones + 1; i < total_coeff; i++) {
            bitsi = (int)bs_show(bs, LEVEL_TAB_BITS);
            level_code = (int)cavlc_level_tab[suffix_length][bitsi][0];
            bs_skip_fast(bs, (int)cavlc_level_tab[suffix_length][bitsi][1]);

            if (level_code >= 100) {
                prefix = level_code - 100;
                if (prefix == LEVEL_TAB_BITS)
                    prefix += ffmpeg_get_level_prefix(bs);

                if (prefix < 15) {
                    level_code = (prefix << suffix_length) +
                                 (int)bs_read(bs, suffix_length);
                } else {
                    level_code = 15 << suffix_length;
                    if (prefix >= 16) {
                        if (prefix > 25 + 3) return -1;
                        level_code += (1 << (prefix - 3)) - 4096;
                    }
                    level_code += (int)bs_read(bs, prefix - 3);
                }
                mask = -(level_code & 1);
                level_code = (((2 + level_code) >> 1) ^ mask) - mask;
            }
            levels[i] = (s16)level_code;

            /* FFmpeg suffix_length update: increment if |level| > suffix_limit.
             * Must use abs(level_code): the unsigned trick only works with
             * non-negative values. Branchless abs via sign-extend XOR. */
            {
                int abs_level = (level_code ^ (level_code >> 31)) - (level_code >> 31);
                suffix_length += (unsigned int)abs_level > suffix_limit[suffix_length];
            }
        }
    }

    return 0;
}

/* ============================================================================
 * total_zeros and run_before — H.264 Tables 9-7, 9-9, 9-10
 *
 * Table-driven VLC decode using canonical ITU-T H.264 table data.
 * ============================================================================*/

/* Table 9-7: total_zeros VLC for 4x4 luma — indexed by [total_coeff-1][tz] */
static const u8 tz_len[15][16] = {
    {1,3,3,4,4,5,5,6,6,7,7,8,8,9,9,9},         /* total_coeff=1  */
    {3,3,3,3,3,4,4,4,4,5,5,6,6,6,6,0},          /* total_coeff=2  */
    {4,3,3,3,4,4,3,3,4,5,5,6,5,6,0,0},          /* total_coeff=3  */
    {5,3,4,4,3,3,3,4,3,4,5,5,5,0,0,0},          /* total_coeff=4  */
    {4,4,4,3,3,3,3,3,4,5,4,5,0,0,0,0},          /* total_coeff=5  */
    {6,5,3,3,3,3,3,3,4,3,6,0,0,0,0,0},          /* total_coeff=6  */
    {6,5,3,3,3,2,3,4,3,6,0,0,0,0,0,0},          /* total_coeff=7  */
    {6,4,5,3,2,2,3,3,6,0,0,0,0,0,0,0},          /* total_coeff=8  */
    {6,6,4,2,2,3,2,5,0,0,0,0,0,0,0,0},          /* total_coeff=9  */
    {5,5,3,2,2,2,4,0,0,0,0,0,0,0,0,0},          /* total_coeff=10 */
    {4,4,3,3,1,3,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=11 */
    {4,4,2,1,3,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=12 */
    {3,3,1,2,0,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=13 */
    {2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=14 */
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=15 */
};
static const u8 tz_bits[15][16] = {
    {1,3,2,3,2,3,2,3,2,3,2,3,2,3,2,1},          /* total_coeff=1  */
    {7,6,5,4,3,5,4,3,2,3,2,3,2,1,0,0},          /* total_coeff=2  */
    {5,7,6,5,4,3,4,3,2,3,2,1,1,0,0,0},          /* total_coeff=3  */
    {3,7,5,4,6,5,4,3,3,2,2,1,0,0,0,0},          /* total_coeff=4  */
    {5,4,3,7,6,5,4,3,2,1,1,0,0,0,0,0},          /* total_coeff=5  */
    {1,1,7,6,5,4,3,2,1,1,0,0,0,0,0,0},          /* total_coeff=6  */
    {1,1,5,4,3,3,2,1,1,0,0,0,0,0,0,0},          /* total_coeff=7  */
    {1,1,1,3,3,2,2,1,0,0,0,0,0,0,0,0},          /* total_coeff=8  */
    {1,0,1,3,2,1,1,1,0,0,0,0,0,0,0,0},          /* total_coeff=9  */
    {1,0,1,3,2,1,1,0,0,0,0,0,0,0,0,0},          /* total_coeff=10 */
    {0,1,1,2,1,3,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=11 */
    {0,1,1,1,1,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=12 */
    {0,1,1,1,0,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=13 */
    {0,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=14 */
    {0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},          /* total_coeff=15 */
};

/* Table 9-9(a): chroma DC total_zeros for 4:2:0 (max 4 coeffs) */
static const u8 tz_cdc_len[3][4] = {
    { 1, 2, 3, 3 },    /* total_coeff=1 */
    { 1, 2, 2, 0 },    /* total_coeff=2 */
    { 1, 1, 0, 0 },    /* total_coeff=3 */
};
static const u8 tz_cdc_bits[3][4] = {
    { 1, 1, 1, 0 },    /* tc=1: tz0="1", tz1="01", tz2="001", tz3="000" */
    { 1, 1, 0, 0 },    /* tc=2: tz0="1", tz1="01", tz2="00"             */
    { 1, 0, 0, 0 },    /* tc=3: tz0="1", tz1="0"                        */
};

static int cavlc_decode_total_zeros(SwBitstream *bs, int total_coeff,
                                    int max_coeffs)
{
    if (total_coeff >= max_coeffs) return 0;

    if (max_coeffs == 4) {
        /* Chroma DC: Table 9-9(a) */
        int tc_idx = total_coeff - 1;
        if (tc_idx < 0 || tc_idx >= 3) return 0;
        int max_tz = max_coeffs - total_coeff;
        int max_bits = 3;
        bs_refill(bs);
        int avail = bs->bits_left;
        int peek_n = (avail < max_bits) ? avail : max_bits;
        if (peek_n <= 0) return 0;
        u32 code = bs->cache >> (32 - peek_n);
        for (int tz = 0; tz <= max_tz; tz++) {
            u8 len = tz_cdc_len[tc_idx][tz];
            if (len == 0 || len > peek_n) continue;
            u8 bits = tz_cdc_bits[tc_idx][tz];
            if ((code >> (peek_n - len)) == (u32)bits) {
                bs->cache <<= len;
                bs->bits_left -= len;
                return tz;
            }
        }
        cavlc_log("total_zeros: no match cdc (tc=%d peek=%d code=0x%04X byte~%d)\n",
                  total_coeff, peek_n, code, bs_byte_offset(bs));
        return -1;
    }

    /* 4x4 luma: Table 9-7, indexed by total_coeff (1-15)
     * NOTE: Always use 16-total_coeff for VLC lookup range (matching FFmpeg).
     * The table is a complete prefix code for up to 16 coefficients.
     * For chroma AC (max_coeffs=15), the encoder still uses full table. */
    if (total_coeff < 1 || total_coeff > 15) return 0;
    int tc_idx = total_coeff - 1;
    int max_tz = 16 - total_coeff;  /* Full table range, not max_coeffs-based */
    int max_bits = 9;
    bs_refill(bs);
    int avail = bs->bits_left;
    int peek_n = (avail < max_bits) ? avail : max_bits;
    if (peek_n <= 0) return 0;
    u32 code = bs->cache >> (32 - peek_n);

    for (int tz = 0; tz <= max_tz && tz < 16; tz++) {
        u8 len = tz_len[tc_idx][tz];
        if (len == 0 || len > peek_n) continue;
        u8 bits = tz_bits[tc_idx][tz];
        if ((code >> (peek_n - len)) == (u32)bits) {
            bs->cache <<= len;
            bs->bits_left -= len;
            return tz;
        }
    }
    cavlc_log("total_zeros: no match (tc=%d max_c=%d peek=%d code=0x%04X byte~%d cache=%08X bl=%d)\n",
              total_coeff, max_coeffs, peek_n, code, bs_byte_offset(bs),
              bs->cache, bs->bits_left);
    return -1;
}

/* Table 9-10: run_before VLC — indexed by [min(zerosLeft-1, 6)][run_before] */
static const u8 run_len[7][16] = {
    {1,1, 0,0,0,0,0,0,0,0,0,0,0,0,0,0},        /* zL=1 */
    {1,2,2, 0,0,0,0,0,0,0,0,0,0,0,0,0},        /* zL=2 */
    {2,2,2,2, 0,0,0,0,0,0,0,0,0,0,0,0},        /* zL=3 */
    {2,2,2,3,3, 0,0,0,0,0,0,0,0,0,0,0},        /* zL=4 */
    {2,2,3,3,3,3, 0,0,0,0,0,0,0,0,0,0},        /* zL=5 */
    {2,3,3,3,3,3,3, 0,0,0,0,0,0,0,0,0},        /* zL=6 */
    {3,3,3,3,3,3,3,4,5,6,7,8,9,10,11,0},        /* zL>=7 */
};
static const u8 run_bits[7][16] = {
    {1,0, 0,0,0,0,0,0,0,0,0,0,0,0,0,0},        /* zL=1 */
    {1,1,0, 0,0,0,0,0,0,0,0,0,0,0,0,0},        /* zL=2 */
    {3,2,1,0, 0,0,0,0,0,0,0,0,0,0,0,0},        /* zL=3 */
    {3,2,1,1,0, 0,0,0,0,0,0,0,0,0,0,0},        /* zL=4 */
    {3,2,3,2,1,0, 0,0,0,0,0,0,0,0,0,0},        /* zL=5 */
    {3,0,1,3,2,5,4, 0,0,0,0,0,0,0,0,0},        /* zL=6 */
    {7,6,5,4,3,2,1,1,1,1,1,1,1,1,1,0},          /* zL>=7 */
};

static int cavlc_decode_run_before(SwBitstream *bs, int zeros_left)
{
    if (zeros_left <= 0) return 0;

    int idx = zeros_left - 1;
    if (idx > 6) idx = 6;
    int max_entries = (idx < 6) ? (idx + 2) : 15;
    int max_bits = (idx < 6) ? 3 : 11;

    bs_refill(bs);
    int avail = bs->bits_left;
    int peek_n = (avail < max_bits) ? avail : max_bits;
    if (peek_n <= 0) return 0;
    u32 code = bs->cache >> (32 - peek_n);

    for (int rb = 0; rb < max_entries; rb++) {
        u8 len = run_len[idx][rb];
        if (len == 0 || len > peek_n) continue;
        u8 bits = run_bits[idx][rb];
        if ((code >> (peek_n - len)) == (u32)bits) {
            bs->cache <<= len;
            bs->bits_left -= len;
            return rb;
        }
    }
    cavlc_log("run_before: no match (zL=%d peek=%d code=0x%04X byte~%d)\n",
              zeros_left, peek_n, code, bs_byte_offset(bs));
    return -1;
}

/* ============================================================================
 * CAVLC Residual Block Decode — Full 4x4 block coefficient parsing
 *
 * This is the core CAVLC function. For each 4x4 block, it outputs the
 * 16 DCT coefficients in zig-zag scan order.
 *
 * H.264 Section 9.2.1-9.2.4
 * ============================================================================*/

int cavlc_decode_block(SwBitstream *bs, s16 *coeffs, int nC,
                       int max_coeffs, int *out_nz)
{
    int total_coeff = 0;
    int trailing_ones = 0;
    int blk_start_byte = bs_byte_offset(bs);

    memset(coeffs, 0, max_coeffs * sizeof(s16));

    /* Step 1: coeff_token — gives total_coeff and trailing_ones */
    if (cavlc_decode_coeff_token(bs, nC, &total_coeff, &trailing_ones) < 0) {
        return -1;
    }

    *out_nz = total_coeff;
    if (total_coeff == 0) {
        if (g_cavlc_blk_verbose)
            cavlc_log("  blk nC=%d mc=%d → tc=0 @%d\n", nC, max_coeffs, blk_start_byte);
        return 0;
    }

    /* Step 2: Decode levels (coefficient magnitudes and signs) */
    s16 levels[16];
    memset(levels, 0, sizeof(levels));
    if (cavlc_decode_levels(bs, total_coeff, trailing_ones, levels) < 0) {
        cavlc_log("levels decode failed: nC=%d tc=%d to=%d byte~%d\n",
                  nC, total_coeff, trailing_ones, bs_byte_offset(bs));
        return -1;
    }

    /* Step 3: total_zeros */
    int total_zeros = 0;
    if (total_coeff < max_coeffs) {
        if (g_cavlc_blk_verbose && total_coeff > 0)
            cavlc_log("  pre_tz nC=%d mc=%d tc=%d to=%d byte~%d cache=%08X bl=%d\n",
                      nC, max_coeffs, total_coeff, trailing_ones,
                      bs_byte_offset(bs), bs->cache, bs->bits_left);
        total_zeros = cavlc_decode_total_zeros(bs, total_coeff, max_coeffs);
        if (total_zeros < 0) return -1;

        /* Clamp total_zeros to valid range for this block size.
         * VLC table covers 0..16-tc but chroma AC only has 15 positions. */
        int max_valid_tz = max_coeffs - total_coeff;
        if (total_zeros > max_valid_tz) {
            cavlc_log("  tz_clamp: tz=%d > max_valid=%d (mc=%d tc=%d) byte~%d\n",
                      total_zeros, max_valid_tz, max_coeffs, total_coeff,
                      bs_byte_offset(bs));
            total_zeros = max_valid_tz;
        }
    }

    if (g_cavlc_blk_verbose)
        cavlc_log("  blk nC=%d mc=%d tc=%d to=%d tz=%d @%d→%d\n",
                  nC, max_coeffs, total_coeff, trailing_ones,
                  total_zeros, blk_start_byte, bs_byte_offset(bs));

    /* Step 4: run_before for each coefficient */
    int run[16];
    memset(run, 0, sizeof(run));
    int zeros_left = total_zeros;
    for (int i = 0; i < total_coeff - 1; i++) {
        run[i] = cavlc_decode_run_before(bs, zeros_left);
        if (run[i] < 0) return -1;
        zeros_left -= run[i];
        if (zeros_left < 0) zeros_left = 0;
    }
    run[total_coeff - 1] = zeros_left;

    /* Step 5: Place coefficients in zig-zag scanned positions,
     * then descan to raster order for the IDCT.
     * Coefficients are in reverse scan order (highest frequency first) */

    /* H.264 Table 8-13: 4x4 frame zig-zag scan → raster index mapping.
     * zigzag position i maps to raster position zigzag_to_raster[i].
     * Raster index = row*4 + col for the 4x4 block. */
    static const u8 zigzag_to_raster[16] = {
        0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15
    };

    s16 zigzag_tmp[16];
    memset(zigzag_tmp, 0, sizeof(zigzag_tmp));

    int coeff_idx = total_zeros + total_coeff - 1;
    for (int i = 0; i < total_coeff; i++) {
        coeff_idx -= run[i];
        if (coeff_idx < 0 || coeff_idx >= max_coeffs) break;
        zigzag_tmp[coeff_idx] = levels[i];
        coeff_idx--;
    }

    /* Descan: zigzag order → raster order for IDCT consumption */
    if (max_coeffs == 16) {
        /* Full 4x4 block (regular luma, or I16x16 DC): zigzag[0..15] → raster */
        for (int i = 0; i < 16; i++) {
            coeffs[zigzag_to_raster[i]] = zigzag_tmp[i];
        }
    } else if (max_coeffs == 15) {
        /* AC-only block (I16x16 luma AC, chroma AC): DC decoded separately.
         * CAVLC index 0 corresponds to zigzag position 1 (first AC).
         * Place at raster positions using zigzag_to_raster[i+1]. */
        for (int i = 0; i < 15; i++) {
            coeffs[zigzag_to_raster[i + 1]] = zigzag_tmp[i];
        }
    } else {
        /* Small blocks (chroma DC, max_coeffs=4): trivial scan, no reorder */
        memcpy(coeffs, zigzag_tmp, max_coeffs * sizeof(s16));
    }

    return 0;
}

/* ============================================================================
 * Macroblock Layer CAVLC Decode — Parses one complete macroblock
 *
 * H.264 Section 7.3.5: Macroblock layer syntax
 * Called once per MB in raster scan order.
 * ============================================================================*/

/* H.264 Table 6-10a: 4x4 block index (8x8-grouped order) to (x,y) position */
static const u8 blk4x4_x[16] = {0,1,0,1, 2,3,2,3, 0,1,0,1, 2,3,2,3};
static const u8 blk4x4_y[16] = {0,0,1,1, 0,0,1,1, 2,2,3,3, 2,2,3,3};

/* Reverse mapping: (y, x) → block index in 8x8-grouped order */
static const u8 xy_to_blk[4][4] = {
    { 0,  1,  4,  5},   /* y=0 */
    { 2,  3,  6,  7},   /* y=1 */
    { 8,  9, 12, 13},   /* y=2 */
    {10, 11, 14, 15},   /* y=3 */
};

/* Predicted nC from above and left neighbors (Table 9-4) */
static int predict_nC(const SwMacroblockData *mbs, int mb_idx,
                      int block_idx, int mb_width, int comp)
{
    int mb_x = mb_idx % mb_width;
    int nA = -1; /* Left neighbor */
    int nB = -1; /* Above neighbor */

    if (comp == 0) { /* Luma — use H.264 8x8-grouped block positions */
        int bx = blk4x4_x[block_idx];
        int by = blk4x4_y[block_idx];

        /* Left neighbor at (bx-1, by) */
        if (bx > 0) {
            nA = mbs[mb_idx].nz_coeff_luma[xy_to_blk[by][bx - 1]];
        } else if (mb_x > 0) {
            nA = mbs[mb_idx - 1].nz_coeff_luma[xy_to_blk[by][3]];
        }

        /* Above neighbor at (bx, by-1) */
        if (by > 0) {
            nB = mbs[mb_idx].nz_coeff_luma[xy_to_blk[by - 1][bx]];
        } else if (mb_idx >= mb_width) {
            nB = mbs[mb_idx - mb_width].nz_coeff_luma[xy_to_blk[3][bx]];
        }
    } else if (comp == 3) { /* I16x16 Luma DC block */
        if (mb_x > 0) {
            const SwMacroblockData *left = &mbs[mb_idx - 1];
            if (left->mb_type == SW_MB_TYPE_I16x16) nA = left->nz_coeff_dc_y;
            else nA = 0;
        }
        if (mb_idx >= mb_width) {
            const SwMacroblockData *above = &mbs[mb_idx - mb_width];
            if (above->mb_type == SW_MB_TYPE_I16x16) nB = above->nz_coeff_dc_y;
            else nB = 0;
        }
    } else { /* Chroma (comp=1 for Cb, comp=2 for Cr) — 2x2 grid */
        const u8 *nz = (comp == 1) ? mbs[mb_idx].nz_coeff_cb
                                    : mbs[mb_idx].nz_coeff_cr;
        int cbx = block_idx & 1;
        int cby = block_idx >> 1;

        if (cbx > 0) {
            nA = nz[block_idx - 1];
        } else if (mb_x > 0) {
            const u8 *nz_left = (comp == 1) ? mbs[mb_idx - 1].nz_coeff_cb
                                             : mbs[mb_idx - 1].nz_coeff_cr;
            nA = nz_left[block_idx + 1];
        }

        if (cby > 0) {
            nB = nz[block_idx - 2];
        } else if (mb_idx >= mb_width) {
            const u8 *nz_above = (comp == 1) ? mbs[mb_idx - mb_width].nz_coeff_cb
                                              : mbs[mb_idx - mb_width].nz_coeff_cr;
            nB = nz_above[block_idx + 2];
        }
    }

    if (nA >= 0 && nB >= 0) return (nA + nB + 1) >> 1;
    if (nA >= 0) return nA;
    if (nB >= 0) return nB;
    return 0;
}

/* CBP tables for I and P macroblocks (Section 9.1.2) */
static const u8 cbp_intra_table[48] = {
    47, 31, 15, 0, 23, 27, 29, 30, 7, 11, 13, 14, 39, 43, 45, 46,
    16, 3, 5, 10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1, 2, 4,
    8, 17, 18, 20, 24, 6, 9, 22, 25, 32, 33, 34, 36, 40, 38, 41
};

static const u8 cbp_inter_table[48] = {
    0, 16, 1, 2, 4, 8, 32, 3, 5, 10, 12, 15, 47, 7, 11, 13,
    14, 6, 9, 31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41
};

/* ============================================================================
 * MV Prediction Helpers — H.264 Section 8.4.1.3 Median Prediction
 *
 * The bitstream carries MVDs (differences). Actual MV = MVD + MVP.
 * MVP is the median of left (A), above (B), and above-right (C) neighbors.
 * For P_SKIP: Section 8.4.1.1 adds shortcuts (if A or B is (0,0), use (0,0)).
 * ============================================================================*/

static inline s16 mv_median3(s16 a, s16 b, s16 c)
{
    if (a > b) { s16 t = a; a = b; b = t; }
    if (b > c) { s16 t = b; b = c; c = t; }
    if (a > b) { s16 t = a; a = b; b = t; }
    return b;
}

/* Get the MV of a neighbor partition overlapping a given block-grid position.
 * For P16x16: MV[0] covers the entire MB.
 * For P16x8:  MV[0]=top, MV[1]=bottom.
 * For P8x16:  MV[0]=left, MV[1]=right.
 * For P_SKIP: MV[0] is the predicted MV (already stored by CAVLC parser). */
static void get_neighbor_mv(const SwMacroblockData *nb,
                             int block_x4, int block_y4,
                             s16 *out_dx, s16 *out_dy)
{
    if (nb->mb_type >= SW_MB_TYPE_I4x4 && nb->mb_type <= SW_MB_TYPE_I16x16) {
        *out_dx = 0; *out_dy = 0;  /* Intra MBs have no MV */
        return;
    }
    if (nb->mb_type == SW_MB_TYPE_P16x8) {
        int part = (block_y4 >= 2) ? 1 : 0;
        *out_dx = nb->mv[part].dx;
        *out_dy = nb->mv[part].dy;
    } else if (nb->mb_type == SW_MB_TYPE_P8x16) {
        int part = (block_x4 >= 2) ? 1 : 0;
        *out_dx = nb->mv[part].dx;
        *out_dy = nb->mv[part].dy;
    } else {
        /* P16x16, P_SKIP, P8x8 → use MV[0] for simplicity */
        *out_dx = nb->mv[0].dx;
        *out_dy = nb->mv[0].dy;
    }
}

/* Compute MVP (median prediction) for P16x16 / P_SKIP from A, B, C neighbors.
 * mb_idx: current MB index, mb_width: MBs per row. */
static void predict_mv_16x16(const SwMacroblockData *mbs, int mb_idx,
                               int mb_width,
                               s16 *mvp_dx, s16 *mvp_dy)
{
    int mb_x = mb_idx % mb_width;
    int mb_y = mb_idx / mb_width;

    s16 ax = 0, ay = 0, bx = 0, by = 0, cx = 0, cy = 0;
    int has_a = 0, has_b = 0, has_c = 0;

    /* A = left */
    if (mb_x > 0) {
        has_a = 1;
        get_neighbor_mv(&mbs[mb_idx - 1], 3, 0, &ax, &ay);
    }
    /* B = above */
    if (mb_y > 0) {
        has_b = 1;
        get_neighbor_mv(&mbs[mb_idx - mb_width], 0, 3, &bx, &by);
    }
    /* C = above-right, or D = above-left if C unavailable */
    if (mb_y > 0 && mb_x < mb_width - 1) {
        has_c = 1;
        get_neighbor_mv(&mbs[mb_idx - mb_width + 1], 0, 3, &cx, &cy);
    } else if (mb_y > 0 && mb_x > 0) {
        has_c = 1;
        get_neighbor_mv(&mbs[mb_idx - mb_width - 1], 3, 3, &cx, &cy);
    }

    int count = has_a + has_b + has_c;
    if (count == 0) {
        *mvp_dx = 0; *mvp_dy = 0;
    } else if (count == 1) {
        if (has_a) { *mvp_dx = ax; *mvp_dy = ay; }
        else if (has_b) { *mvp_dx = bx; *mvp_dy = by; }
        else { *mvp_dx = cx; *mvp_dy = cy; }
    } else {
        *mvp_dx = mv_median3(ax, bx, cx);
        *mvp_dy = mv_median3(ay, by, cy);
    }
}

int cavlc_decode_macroblock(SwBitstream *bs, SwMacroblockData *mbs,
                            int mb_idx, int mb_width,
                            const SwSliceHeader *sh, const SwPPS *pps,
                            s8 *prev_qp)
{
    SwMacroblockData *mb = &mbs[mb_idx];
    memset(mb, 0, sizeof(SwMacroblockData));
    int mb_x = mb_idx % mb_width;

    /* Capture pre-MB cache state for diagnostics */
    u32 pre_cache = bs->cache >> 16; /* top 16 bits */
    int bits_at_mb_start = bs_bits_remaining(bs);

    /* Read mb_type */
    u32 mb_type_raw = bs_read_ue(bs);

    if (sh->slice_type == SW_SLICE_I) {
        /* I-slice mb_type: 0 = I_4x4, 1-24 = I_16x16 variants, 25 = I_PCM */
        if (mb_type_raw == 0) {
            mb->mb_type = SW_MB_TYPE_I4x4;
        } else if (mb_type_raw <= 24) {
            mb->mb_type = SW_MB_TYPE_I16x16;
            mb_type_raw -= 1;
            mb->intra16x16_mode = (u8)(mb_type_raw % 4);
            /* H.264 Table 7-11: CBP for I_16x16 */
            {
                u8 cbp_chroma = (u8)((mb_type_raw / 4) % 3);
                u8 cbp_luma   = (mb_type_raw >= 12) ? 0x0F : 0x00;
                mb->coded_block_pattern = cbp_luma | (cbp_chroma << 4);
            }
        } else {
            /* I_PCM — raw samples, very rare in streaming */
            cavlc_log("  MB#%d I-slice mb_type_raw=%u INVALID (cache=%08X bl=%d byte~%d)\n",
                      mb_idx, mb_type_raw, bs->cache, bs->bits_left, bs_byte_offset(bs));
            return -1;
        }
    } else {
        /* P-slice: 0 = P_L0_16x16, 1 = P_L0_L0_16x8, 2 = P_L0_L0_8x16,
                    3 = P_8x8, 4 = P_8x8ref0, 5+ = I-types (offset by 5) */
        if (mb_type_raw == 0) mb->mb_type = SW_MB_TYPE_P16x16;
        else if (mb_type_raw == 1) mb->mb_type = SW_MB_TYPE_P16x8;
        else if (mb_type_raw == 2) mb->mb_type = SW_MB_TYPE_P8x16;
        else if (mb_type_raw == 3 || mb_type_raw == 4) mb->mb_type = SW_MB_TYPE_P8x8;
        else if (mb_type_raw == 5) mb->mb_type = SW_MB_TYPE_I4x4;
        else if (mb_type_raw > 5 && mb_type_raw <= 29) {
            mb->mb_type = SW_MB_TYPE_I16x16;
            u32 adj = mb_type_raw - 6;
            mb->intra16x16_mode = (u8)(adj % 4);
            /* H.264 Table 7-11: CBP for I_16x16 in P-slice */
            {
                u8 cbp_chroma = (u8)((adj / 4) % 3);
                u8 cbp_luma   = (adj >= 12) ? 0x0F : 0x00;
                mb->coded_block_pattern = cbp_luma | (cbp_chroma << 4);
            }
        } else {
            return -1;
        }
    }

    /* Intra 4x4 prediction modes (H.264 §8.3.1.1) */
    if (mb->mb_type == SW_MB_TYPE_I4x4) {
        for (int i = 0; i < 16; i++) {
            int bx = blk4x4_x[i];
            int by = blk4x4_y[i];

            /* Determine predicted mode = min(modeA, modeB) */
            int modeA = SW_INTRA4_DC; /* left neighbor */
            int modeB = SW_INTRA4_DC; /* above neighbor */

            if (bx > 0) {
                modeA = mb->intra4x4_modes[xy_to_blk[by][bx - 1]];
            } else if (mb_x > 0) {
                const SwMacroblockData *left = &mbs[mb_idx - 1];
                if (left->mb_type == SW_MB_TYPE_I4x4)
                    modeA = left->intra4x4_modes[xy_to_blk[by][3]];
            }

            if (by > 0) {
                modeB = mb->intra4x4_modes[xy_to_blk[by - 1][bx]];
            } else if (mb_idx >= mb_width) {
                const SwMacroblockData *above = &mbs[mb_idx - mb_width];
                if (above->mb_type == SW_MB_TYPE_I4x4)
                    modeB = above->intra4x4_modes[xy_to_blk[3][bx]];
            }

            int predicted_mode = (modeA < modeB) ? modeA : modeB;

            u32 prev_flag = bs_read1(bs);
            if (prev_flag) {
                mb->intra4x4_modes[i] = (u8)predicted_mode;
            } else {
                int rem = (int)bs_read(bs, 3);
                mb->intra4x4_modes[i] = (u8)((rem < predicted_mode) ? rem : rem + 1);
            }
        }
        if (g_cavlc_blk_verbose) {
            int bits_after_modes = bs_bits_remaining(bs);
            cavlc_log("  i4x4_modes: %d bits  rem=%d byte~%d cache=%08X bl=%d\n",
                      bits_at_mb_start - bits_after_modes,
                      bits_after_modes, bs_byte_offset(bs),
                      bs->cache, bs->bits_left);
        }
    }

    /* Chroma prediction mode (for all intra MBs) */
    if (mb->mb_type == SW_MB_TYPE_I4x4 || mb->mb_type == SW_MB_TYPE_I16x16) {
        mb->chroma_pred_mode = (u8)bs_read_ue(bs);
        if (g_cavlc_blk_verbose) {
            int bits_after_cpred = bs_bits_remaining(bs);
            cavlc_log("  chroma_pred=%u  rem=%d\n",
                      (unsigned)mb->chroma_pred_mode, bits_after_cpred);
        }
    }

    /* Motion vectors for P macroblocks */
    if (mb->mb_type == SW_MB_TYPE_P16x16) {
        mb->ref_idx[0] = 0;
        if (sh->num_ref_idx_l0_active > 1) {
            mb->ref_idx[0] = (s8)bs_read_ue(bs);
        }
        /* Bitstream carries MVD (difference); actual MV = MVD + MVP */
        s16 mvd_dx = (s16)bs_read_se(bs);
        s16 mvd_dy = (s16)bs_read_se(bs);
        s16 mvp_dx, mvp_dy;
        predict_mv_16x16(mbs, mb_idx, mb_width, &mvp_dx, &mvp_dy);
        mb->mv[0].dx = mvd_dx + mvp_dx;
        mb->mv[0].dy = mvd_dy + mvp_dy;
    } else if (mb->mb_type == SW_MB_TYPE_P16x8) {
        for (int i = 0; i < 2; i++) {
            mb->ref_idx[i] = 0;
            if (sh->num_ref_idx_l0_active > 1) {
                mb->ref_idx[i] = (s8)bs_read_ue(bs);
            }
        }
        /* MVD + MVP for each partition (use 16x16 median as approximation) */
        s16 mvp_dx, mvp_dy;
        predict_mv_16x16(mbs, mb_idx, mb_width, &mvp_dx, &mvp_dy);
        for (int i = 0; i < 2; i++) {
            s16 mvd_dx = (s16)bs_read_se(bs);
            s16 mvd_dy = (s16)bs_read_se(bs);
            mb->mv[i].dx = mvd_dx + mvp_dx;
            mb->mv[i].dy = mvd_dy + mvp_dy;
        }
    } else if (mb->mb_type == SW_MB_TYPE_P8x16) {
        for (int i = 0; i < 2; i++) {
            mb->ref_idx[i] = 0;
            if (sh->num_ref_idx_l0_active > 1) {
                mb->ref_idx[i] = (s8)bs_read_ue(bs);
            }
        }
        /* MVD + MVP for each partition (use 16x16 median as approximation) */
        s16 mvp_dx, mvp_dy;
        predict_mv_16x16(mbs, mb_idx, mb_width, &mvp_dx, &mvp_dy);
        for (int i = 0; i < 2; i++) {
            s16 mvd_dx = (s16)bs_read_se(bs);
            s16 mvd_dy = (s16)bs_read_se(bs);
            mb->mv[i].dx = mvd_dx + mvp_dx;
            mb->mv[i].dy = mvd_dy + mvp_dy;
        }
    } else if (mb->mb_type == SW_MB_TYPE_P8x8) {
        /* Sub-macroblock types: 0=8x8(1MV), 1=8x4(2MV), 2=4x8(2MV), 3=4x4(4MV) */
        u32 sub_mb_type[4];
        for (int i = 0; i < 4; i++) {
            sub_mb_type[i] = bs_read_ue(bs);
            if (sub_mb_type[i] > 3) sub_mb_type[i] = 0;
        }
        /* Ref indices per 8x8 partition */
        for (int i = 0; i < 4; i++) {
            mb->ref_idx[i] = 0;
            if (sh->num_ref_idx_l0_active > 1) {
                mb->ref_idx[i] = (s8)bs_read_ue(bs);
            }
        }
        /* MVs: count depends on sub_mb_type partition structure
         * P_L0_8x8=0 → 1 MV, P_L0_8x4=1 → 2, P_L0_4x8=2 → 2, P_L0_4x4=3 → 4 */
        static const u8 sub_part_count[4] = {1, 2, 2, 4};
        s16 mvp_dx, mvp_dy;
        predict_mv_16x16(mbs, mb_idx, mb_width, &mvp_dx, &mvp_dy);
        int mv_slot = 0;
        for (int i = 0; i < 4; i++) {
            int nparts = sub_part_count[sub_mb_type[i]];
            for (int p = 0; p < nparts; p++) {
                s16 mvd_dx = (s16)bs_read_se(bs);
                s16 mvd_dy = (s16)bs_read_se(bs);
                if (mv_slot < 16) {
                    mb->mv[mv_slot].dx = mvd_dx + mvp_dx;
                    mb->mv[mv_slot].dy = mvd_dy + mvp_dy;
                    mv_slot++;
                }
            }
        }
    }

    /* Coded Block Pattern (for non-I16x16 types) */
    if (mb->mb_type != SW_MB_TYPE_I16x16) {
        u32 cbp_code = bs_read_ue(bs);
        if (g_cavlc_blk_verbose) {
            cavlc_log("  cbp_code=%u  rem=%d\n", cbp_code, bs_bits_remaining(bs));
        }
        if (cbp_code < 48) {
            if (mb->mb_type == SW_MB_TYPE_I4x4) {
                mb->coded_block_pattern = cbp_intra_table[cbp_code];
            } else {
                mb->coded_block_pattern = cbp_inter_table[cbp_code];
            }
        }
    }

    /* QP delta */
    u8 cbp = mb->coded_block_pattern;
    if (cbp > 0 || mb->mb_type == SW_MB_TYPE_I16x16) {
        mb->qp_delta = (s8)bs_read_se(bs);
        mb->qp_y = *prev_qp + mb->qp_delta;

        /* Clamp QP to valid range */
        if (mb->qp_y < 0) mb->qp_y = 0;
        if (mb->qp_y > 51) mb->qp_y = 51;
        *prev_qp = mb->qp_y;
        if (g_cavlc_blk_verbose) {
            int bits_after_qpd = bs_bits_remaining(bs);
            cavlc_log("  qp_delta=%d  hdr_bits=%d  rem=%d  byte~%d\n",
                      (int)mb->qp_delta,
                      bits_at_mb_start - bits_after_qpd,
                      bits_after_qpd, bs_byte_offset(bs));
        }
    } else {
        mb->qp_y = *prev_qp;
    }

    /* Per-MB compact summary for IDR drift tracking — only first/last 5 MBs
     * to avoid 510 log writes (significant I/O overhead on 333 MHz PSP) */
    if (sh->idr_flag && mb_idx < 25) {
        cavlc_log("  hdr: mbt_raw=%u type=%d cbp=0x%02X qpd=%d cache=%04X byte~%d\n",
                  mb_type_raw, (int)mb->mb_type, cbp,
                  (int)mb->qp_delta, pre_cache, bs_byte_offset(bs));
    }
    /* QP anomaly: flag absurd delta as drift indicator.
     * Valid range is -26..+25 per H.264 spec.  Only warn above 26. */
    if (mb->qp_delta > 26 || mb->qp_delta < -26) {
        cavlc_log("  WARNING: |qp_delta|=%d at MB#%d -- likely bitstream drift!\n",
                  (int)(mb->qp_delta < 0 ? -mb->qp_delta : mb->qp_delta), mb_idx);
    }

    /* ================================================================
     * CAVLC Residual Data — The Sequential Bottleneck
     *
     * This is why CAVLC cannot be parallelized: each block's nC
     * prediction depends on the previously-decoded neighbor blocks.
     * ================================================================*/

    /* Block-failure diagnostic: track which block we're decoding */
    const char *blk_label = "?";
    int blk_number = -1;

    #define CAVLC_FAIL_DUMP() do { \
        cavlc_log("  FAIL in %s (blk#%d) cbp=0x%02X\n", blk_label, blk_number, cbp); \
        cavlc_log("  cur nz_luma: %d %d %d %d  %d %d %d %d  %d %d %d %d  %d %d %d %d\n", \
            mb->nz_coeff_luma[0], mb->nz_coeff_luma[1], mb->nz_coeff_luma[2], mb->nz_coeff_luma[3], \
            mb->nz_coeff_luma[4], mb->nz_coeff_luma[5], mb->nz_coeff_luma[6], mb->nz_coeff_luma[7], \
            mb->nz_coeff_luma[8], mb->nz_coeff_luma[9], mb->nz_coeff_luma[10], mb->nz_coeff_luma[11], \
            mb->nz_coeff_luma[12], mb->nz_coeff_luma[13], mb->nz_coeff_luma[14], mb->nz_coeff_luma[15]); \
        if (mb_x > 0) { \
            const SwMacroblockData *lmb = &mbs[mb_idx - 1]; \
            cavlc_log("  left nz_luma: %d %d %d %d  %d %d %d %d  %d %d %d %d  %d %d %d %d\n", \
                lmb->nz_coeff_luma[0], lmb->nz_coeff_luma[1], lmb->nz_coeff_luma[2], lmb->nz_coeff_luma[3], \
                lmb->nz_coeff_luma[4], lmb->nz_coeff_luma[5], lmb->nz_coeff_luma[6], lmb->nz_coeff_luma[7], \
                lmb->nz_coeff_luma[8], lmb->nz_coeff_luma[9], lmb->nz_coeff_luma[10], lmb->nz_coeff_luma[11], \
                lmb->nz_coeff_luma[12], lmb->nz_coeff_luma[13], lmb->nz_coeff_luma[14], lmb->nz_coeff_luma[15]); \
        } \
        if (mb_idx >= mb_width) { \
            const SwMacroblockData *amb = &mbs[mb_idx - mb_width]; \
            cavlc_log("  above nz_luma: %d %d %d %d  %d %d %d %d  %d %d %d %d  %d %d %d %d\n", \
                amb->nz_coeff_luma[0], amb->nz_coeff_luma[1], amb->nz_coeff_luma[2], amb->nz_coeff_luma[3], \
                amb->nz_coeff_luma[4], amb->nz_coeff_luma[5], amb->nz_coeff_luma[6], amb->nz_coeff_luma[7], \
                amb->nz_coeff_luma[8], amb->nz_coeff_luma[9], amb->nz_coeff_luma[10], amb->nz_coeff_luma[11], \
                amb->nz_coeff_luma[12], amb->nz_coeff_luma[13], amb->nz_coeff_luma[14], amb->nz_coeff_luma[15]); \
        } \
    } while(0)

    /* I16x16 luma DC (if applicable) — uses block 0 nC per H.264 §9.2.1 */
    if (mb->mb_type == SW_MB_TYPE_I16x16) {
        int nz = 0;
        int nC = predict_nC(mbs, mb_idx, 0, mb_width, 3); /* comp=3 = Luma DC */
        blk_label = "luma_dc"; blk_number = -1;
        if (cavlc_decode_block(bs, mb->luma_dc, nC, 16, &nz) < 0) {
            CAVLC_FAIL_DUMP();
            return -1;
        }
        mb->nz_coeff_dc_y = (u8)nz;
    }

    /* Luma 4x4 residual blocks */
    u8 luma_cbp = cbp & 0x0F;
    for (int i8x8 = 0; i8x8 < 4; i8x8++) {
        if (!(luma_cbp & (1 << i8x8))) {
            /* No residual for this 8x8 block */
            for (int i4x4 = 0; i4x4 < 4; i4x4++) {
                int blk = i8x8 * 4 + i4x4;
                mb->nz_coeff_luma[blk] = 0;
            }
            continue;
        }

        for (int i4x4 = 0; i4x4 < 4; i4x4++) {
            int blk_idx = i8x8 * 4 + i4x4;
            int nz = 0;
            int nC = predict_nC(mbs, mb_idx, blk_idx, mb_width, 0);

            int max_c = (mb->mb_type == SW_MB_TYPE_I16x16) ? 15 : 16;
            blk_label = "luma"; blk_number = blk_idx;
            if (cavlc_decode_block(bs, mb->luma_coeff[blk_idx], nC, max_c, &nz) < 0) {
                CAVLC_FAIL_DUMP();
                return -1;
            }
            mb->nz_coeff_luma[blk_idx] = (u8)nz;
        }
    }

    /* Chroma residual */
    u8 chroma_cbp = (cbp >> 4) & 0x03;
    if (chroma_cbp > 0) {
        /* Chroma DC */
        int nz_dc;
        blk_label = "chroma_dc_cb"; blk_number = 0;
        if (cavlc_decode_block(bs, mb->chroma_dc_cb, -1, 4, &nz_dc) < 0) {
            CAVLC_FAIL_DUMP();
            return -1;
        }
        blk_label = "chroma_dc_cr"; blk_number = 1;
        if (cavlc_decode_block(bs, mb->chroma_dc_cr, -1, 4, &nz_dc) < 0) {
            CAVLC_FAIL_DUMP();
            return -1;
        }

        /* Chroma AC (if chroma_cbp == 2) */
        if (chroma_cbp == 2) {
            for (int i = 0; i < 4; i++) {
                int nC = predict_nC(mbs, mb_idx, i, mb_width, 1);
                int nz = 0;
                blk_label = "cb_ac"; blk_number = i;
                if (cavlc_decode_block(bs, mb->cb_coeff[i], nC, 15, &nz) < 0) {
                    CAVLC_FAIL_DUMP();
                    return -1;
                }
                mb->nz_coeff_cb[i] = (u8)nz;
            }
            for (int i = 0; i < 4; i++) {
                int nC = predict_nC(mbs, mb_idx, i, mb_width, 2);
                int nz = 0;
                blk_label = "cr_ac"; blk_number = i;
                if (cavlc_decode_block(bs, mb->cr_coeff[i], nC, 15, &nz) < 0) {
                    CAVLC_FAIL_DUMP();
                    return -1;
                }
                mb->nz_coeff_cr[i] = (u8)nz;
            }
        } else {
            /* chroma_cbp == 1: DC only, no AC — zero nz_coeff for neighbor prediction */
            memset(mb->nz_coeff_cb, 0, sizeof(mb->nz_coeff_cb));
            memset(mb->nz_coeff_cr, 0, sizeof(mb->nz_coeff_cr));
        }
    } else {
        /* chroma_cbp == 0: no chroma residual at all — zero everything */
        memset(mb->nz_coeff_cb, 0, sizeof(mb->nz_coeff_cb));
        memset(mb->nz_coeff_cr, 0, sizeof(mb->nz_coeff_cr));
    }

    #undef CAVLC_FAIL_DUMP
    return 0;
}

/* ============================================================================
 * Full Frame CAVLC Decode — Entry point for the Main CPU front-end
 *
 * Parses all NAL units in an Access Unit, decodes SPS/PPS/slice header,
 * then runs CAVLC for every macroblock in the slice.
 *
 * Output: SwPipelineState with mbs[] array fully populated.
 * ============================================================================*/

int cavlc_decode_frame(const u8 *annexb_data, int annexb_len,
                       SwPipelineState *state)
{
    /* Temporary RBSP buffer for emulation prevention byte removal */
    static u8 __attribute__((aligned(64))) rbsp_buf[SW_MAX_BITSTREAM];

    /* ── Pre-inject known SPS/PPS ──────────────────────────────────
     * The initial IDR consistently loses its first few WiFi packets
     * (SOF + SPS + PPS), leaving the decoder unable to parse ANY
     * frames ("Slice before SPS/PPS").  Inject once at first call
     * so P-frames can decode immediately with a zero reference. */
    {
        static int sps_pps_injected = 0;
        if (!sps_pps_injected && !state->sps.valid) {
            /* Bytes AFTER NAL type 0x67 — from validated idr_dump.h264
             * Profile=66 (Baseline) Level=30 pic=30x17 MBs (480x272) */
            static const u8 sps_raw[] = {
                0x42, 0x04, 0x1E, 0xDA, 0x07, 0x82, 0x3B, 0xFF,
                0x00, 0x01, 0x00, 0x01, 0x42, 0x0C, 0x0C, 0x0C,
                0x80, 0x00, 0x00, 0x03, 0x00, 0x80, 0x00, 0x00,
                0x0F, 0x47, 0x84, 0x42, 0x35
            };
            /* Bytes AFTER NAL type 0x68 — CAVLC, qp=26 */
            static const u8 pps_raw[] = { 0xCE, 0x3C, 0x80 };

            int sps_rbsp = nal_remove_epb(sps_raw, sizeof(sps_raw),
                                          rbsp_buf, SW_MAX_BITSTREAM);
            SwBitstream bs;
            bs_init(&bs, rbsp_buf, sps_rbsp);
            parse_sps(&bs, &state->sps);

            int pps_rbsp = nal_remove_epb(pps_raw, sizeof(pps_raw),
                                          rbsp_buf, SW_MAX_BITSTREAM);
            bs_init(&bs, rbsp_buf, pps_rbsp);
            parse_pps(&bs, &state->pps, &state->sps);

            if (state->sps.valid && state->pps.valid) {
                cavlc_log("PRE-INJECTED SPS/PPS: %dx%d MBs prof=%d lv=%d\n",
                          state->sps.pic_width_in_mbs,
                          state->sps.pic_height_in_map_units,
                          state->sps.profile_idc, state->sps.level_idc);
                sps_pps_injected = 1;
            }
        }
    }

    /* Scan for NAL units */
    SwNALUnit nals[32];
    int nal_count = nal_scan_annexb(annexb_data, annexb_len, nals, 32);
    if (nal_count <= 0) {
        cavlc_log("No NAL units found in %d bytes\n", annexb_len);
        return -1;
    }

    /* IDR frame dump DISABLED for performance.
     * Re-enable for debugging by setting idr_dump_enable = 1. */
    {
        static int idr_dumped = 0;
        int idr_dump_enable = 1;
        if (idr_dump_enable && !idr_dumped) {
            for (int nd = 0; nd < nal_count; nd++) {
                if (nals[nd].type == SW_NAL_IDR) {
                    moonlight_storage_ensure_data_dir();
                    SceUID fd = sceIoOpen(MOONLIGHT_SAVE_IDR_DUMP_PATH,
                                          PSP_O_WRONLY | PSP_O_CREAT | PSP_O_TRUNC, 0777);
                    if (fd >= 0) {
                        sceIoWrite(fd, annexb_data, annexb_len);
                        sceIoClose(fd);
                        cavlc_log("IDR DUMP: saved %d bytes to %s\n", annexb_len, MOONLIGHT_SAVE_IDR_DUMP_PATH);
                    } else {
                        cavlc_log("IDR DUMP: failed to open file (err=%d)\n", fd);
                    }
                    idr_dumped = 1;
                    break;
                }
            }
        }
    }

    /* Per-frame NAL count log silenced for performance */

    int slice_found = 0;

    for (int n = 0; n < nal_count; n++) {
        SwNALUnit *nal = &nals[n];

        /* Remove emulation prevention bytes */
        int rbsp_len = nal_remove_epb(nal->data, nal->size,
                                      rbsp_buf, SW_MAX_BITSTREAM);
        SwBitstream bs;
        bs_init(&bs, rbsp_buf, rbsp_len);

        switch (nal->type) {
        case SW_NAL_SPS:
            parse_sps(&bs, &state->sps);
            break;

        case SW_NAL_PPS:
            parse_pps(&bs, &state->pps, &state->sps);
            break;

        case SW_NAL_SLICE:
        case SW_NAL_IDR: {
            if (!state->sps.valid || !state->pps.valid) {
                cavlc_log("Slice before SPS/PPS — skipping\n");
                continue;
            }

            /* Parse slice header */
            parse_slice_header(&bs, &state->slice, &state->sps,
                             &state->pps, nal->type, nal->ref_idc);

            int mb_width = state->sps.pic_width_in_mbs;
            int mb_height = state->sps.pic_height_in_map_units;
            int total_mbs = mb_width * mb_height;
            if (total_mbs > SW_TOTAL_MBS) total_mbs = SW_TOTAL_MBS;

            s8 cur_qp = state->slice.slice_qp;

            int epb_count = nal->size - rbsp_len;
            /* Log slice info only for IDR frames and every 120th P-frame */
            if (state->slice.idr_flag ||
                (g_cavlc_frame_count % 120) == 0) {
                cavlc_log("Slice: type=%s idr=%d first_mb=%d total=%d qp=%d rbsp=%d epb=%d\n",
                          state->slice.slice_type == 0 ? "P" :
                          state->slice.slice_type == 2 ? "I" : "?",
                          state->slice.idr_flag,
                          state->slice.first_mb_in_slice,
                          total_mbs, (int)cur_qp, rbsp_len, epb_count);
            }
            g_cavlc_frame_count++;

            /* P_SKIP run decoding and CAVLC macroblock loop */
            int mb_idx = state->slice.first_mb_in_slice;
            int loop_iters = 0;
            int max_iters = total_mbs * 2 + 64; /* safety bound */
            int slice_decode_error = 0;
            int error_mb_idx = -1; /* MB where error concealment started */
            state->error_concealed = 0; /* Clear for this frame */
            state->real_mb_count = 0;   /* Will be set below */
            while (mb_idx < total_mbs && !bs_eof(&bs) && loop_iters < max_iters) {
                loop_iters++;

                /* Yield every 64 MBs to let psplink handle commands (scrshot etc.)
                 * sceKernelDelayThread(0) just reschedules without actual delay. */
                if ((mb_idx & 63) == 0 && mb_idx > 0)
                    sceKernelDelayThread(0);

                /* Check for RBSP trailing bits (end of slice).
                 * H.264 Section 7.2: more_rbsp_data() returns false when
                 * only trailing alignment bits remain. We need at least 9
                 * bits to parse even the smallest mb_type exp-golomb. */
                if (bs_bits_remaining(&bs) < 16) {
                    /* Likely end of this slice — remaining bits are
                     * just rbsp_stop_one_bit + alignment zeros.
                     * Next slice NAL will continue from here. */
                    /* End-of-slice log silenced for performance */
                    break;
                }

                /* P-slice skip run */
                if (state->slice.slice_type == SW_SLICE_P) {
                    u32 skip_run = bs_read_ue(&bs);
                    for (u32 s = 0; s < skip_run && mb_idx < total_mbs; s++) {
                        SwMacroblockData *mb = &state->mbs[mb_idx];
                        memset(mb, 0, sizeof(SwMacroblockData));
                        mb->mb_type = SW_MB_TYPE_PSKIP;
                        mb->skip_flag = 1;
                        mb->qp_y = cur_qp;

                        /* P_SKIP MV — H.264 Section 8.4.1.1:
                         * If A or B unavailable, or either non-intra neighbor
                         * has ref_idx==0 AND MV==(0,0), then MV = (0,0).
                         * Otherwise use median prediction (Section 8.4.1.3). */
                        int skip_mx = mb_idx % mb_width;
                        int skip_my = mb_idx / mb_width;
                        int have_a = (skip_mx > 0);
                        int have_b = (skip_my > 0);
                        s16 smvx = 0, smvy = 0;

                        if (!have_a || !have_b) {
                            /* Neighbor unavailable → zero MV */
                        } else {
                            const SwMacroblockData *nbA = &state->mbs[mb_idx - 1];
                            const SwMacroblockData *nbB = &state->mbs[mb_idx - mb_width];
                            int a_intra = (nbA->mb_type <= SW_MB_TYPE_I16x16);
                            int b_intra = (nbB->mb_type <= SW_MB_TYPE_I16x16);
                            int a_zero = 0, b_zero = 0;

                            /* Check if A has ref_idx==0 AND MV==(0,0) (skip for intra: conceptual ref=-1) */
                            if (!a_intra && nbA->ref_idx[0] == 0) {
                                s16 ax, ay;
                                get_neighbor_mv(nbA, 3, 0, &ax, &ay);
                                if (ax == 0 && ay == 0) a_zero = 1;
                            }
                            if (!b_intra && nbB->ref_idx[0] == 0) {
                                s16 bx, by;
                                get_neighbor_mv(nbB, 0, 3, &bx, &by);
                                if (bx == 0 && by == 0) b_zero = 1;
                            }

                            if (a_zero || b_zero) {
                                /* Shortcut → zero MV */
                            } else {
                                /* Full median prediction */
                                predict_mv_16x16(state->mbs, mb_idx, mb_width,
                                                 &smvx, &smvy);
                            }
                        }
                        mb->mv[0].dx = smvx;
                        mb->mv[0].dy = smvy;

                        mb_idx++;
                    }
                    if (mb_idx >= total_mbs) break;
                }

                /* Decode non-skipped macroblock */
                /* Verbose MB logging disabled — root cause confirmed as network
                 * corruption (FFmpeg also fails on same IDR dump at same MB). */
                g_cavlc_blk_verbose = 0;
                if (cavlc_decode_macroblock(&bs, state->mbs, mb_idx,
                                            mb_width, &state->slice,
                                            &state->pps, &cur_qp) < 0) {
                    /* Check if failure is near end of RBSP — likely a
                     * multi-slice boundary where this slice's data ends
                     * and the next slice NAL continues. */
                    int remaining = bs_bits_remaining(&bs);
                    if (remaining < 64) {
                        /* Near end of slice data — this MB belongs to
                         * the next slice NAL, not an actual decode error */
                        cavlc_log("Slice boundary at MB %d/%d (rem=%d bits) — continuing to next NAL\n",
                                  mb_idx, total_mbs, remaining);
                        break;
                    }
                    int mb_x = mb_idx % mb_width;
                    int mb_y = mb_idx / mb_width;
                    int fail_byte = bs_byte_offset(&bs);
                    cavlc_log("MB %d/%d (%d,%d) type=%d failed (byte~%d cache_left=%d rem=%d slice=%s qp=%d)\n",
                              mb_idx, total_mbs, mb_x, mb_y,
                              (int)state->mbs[mb_idx].mb_type,
                              fail_byte, bs.bits_left,
                              bs_bits_remaining(&bs),
                              state->slice.slice_type == 2 ? "I" : "P",
                              (int)cur_qp);
                    /* Hex dump around failure point */
                    if (state->slice.idr_flag && fail_byte > 0 && fail_byte < rbsp_len - 8) {
                        int d = (fail_byte > 8) ? fail_byte - 8 : 0;
                        cavlc_log("RBSP@%d: %02x %02x %02x %02x %02x %02x %02x %02x"
                                  " [%02x] %02x %02x %02x %02x %02x %02x %02x\n",
                                  d,
                                  rbsp_buf[d], rbsp_buf[d+1], rbsp_buf[d+2], rbsp_buf[d+3],
                                  rbsp_buf[d+4], rbsp_buf[d+5], rbsp_buf[d+6], rbsp_buf[d+7],
                                  rbsp_buf[fail_byte],
                                  rbsp_buf[fail_byte+1], rbsp_buf[fail_byte+2],
                                  rbsp_buf[fail_byte+3], rbsp_buf[fail_byte+4],
                                  rbsp_buf[fail_byte+5], rbsp_buf[fail_byte+6],
                                  rbsp_buf[fail_byte+7]);
                    }
                    slice_decode_error = 1;
                    error_mb_idx = mb_idx;
                    state->error_concealed = 1;
                    state->real_mb_count = mb_idx; /* Save real count before concealment */

                    /* Error concealment: fill remaining MBs with defaults
                     * so the frame can still be used as a reference.
                     * For I-slices (IDR): I_16x16_DC with zero residual
                     *   → Y/U/V planes stay at 128 (initialized before recon)
                     *   → Produces neutral gray in the error region.
                     * For P-slices: P_SKIP with MV=(0,0)
                     *   → Copies reference pixels (no motion compensation)
                     *   → Error region shows previous frame content.
                     * This is critical: without a usable reference from the
                     * first IDR, ALL subsequent P-frames are rejected. */
                    cavlc_log("Error concealment: filling MB %d..%d with defaults\n",
                              mb_idx, total_mbs - 1);
                    for (int ec = mb_idx; ec < total_mbs; ec++) {
                        SwMacroblockData *ecmb = &state->mbs[ec];
                        memset(ecmb, 0, sizeof(SwMacroblockData));
                        if (state->slice.slice_type == SW_SLICE_P) {
                            ecmb->mb_type = SW_MB_TYPE_PSKIP;
                            ecmb->skip_flag = 1;
                        } else {
                            ecmb->mb_type = SW_MB_TYPE_I16x16;
                            ecmb->intra16x16_mode = 2; /* DC prediction */
                        }
                        ecmb->qp_y = cur_qp;
                    }
                    mb_idx = total_mbs;
                    break;
                }
                g_cavlc_blk_verbose = 0;
                /* Light IDR progress: first 3, every 100th, and last 3 MBs */
                if (state->slice.idr_flag && (mb_idx < 3 || mb_idx % 100 == 0 || mb_idx >= total_mbs - 3)) {
                    cavlc_log("  MB#%d post: byte=%d type=%d\n",
                              mb_idx, bs_byte_offset(&bs),
                              (int)state->mbs[mb_idx].mb_type);
                }
                mb_idx++;
            }

            if (loop_iters >= max_iters) {
                cavlc_log("MB loop watchdog: %d iterations at mb_idx=%d/%d\n",
                          loop_iters, mb_idx, total_mbs);
            }

            state->mb_count = mb_idx;
            state->total_mbs_expected = total_mbs;
            /* real_mb_count: if error concealment ran, it was already set
             * to the pre-error count. If no error, set to full count. */
            if (!state->error_concealed) {
                state->real_mb_count = mb_idx;
            }
            slice_found = 1;

            /* Log partial decode but DON'T return -2 — error concealment
             * filled the remaining MBs, so the frame is usable as a
             * reference even if part of it is gray/repeated */
            if (slice_decode_error) {
                cavlc_log("Partial IDR: %d/%d real MBs, %d concealed (continuing)\n",
                          error_mb_idx, total_mbs,
                          total_mbs - error_mb_idx);
            }

            /* If more MBs remain, continue to next NAL for next slice */
            if (mb_idx < total_mbs) {
                cavlc_log("Slice done: %d/%d MBs, expecting continuation slice\n",
                          mb_idx, total_mbs);
            }
            /* Do NOT break — process remaining NAL units for multi-slice frames */
            break; /* break out of switch, not the for loop */
        }

        case SW_NAL_SEI:
        case SW_NAL_AUD:
            /* Skip — not needed for decode */
            break;

        default:
            break;
        }
    }

    if (!slice_found) {
        cavlc_log("No slice NAL found in access unit\n");
        return -1;
    }

    /* After processing all NALs, check if all MBs were decoded.
     * If error concealment ran, mb_count == total_mbs_expected. */
    if (state->mb_count < state->total_mbs_expected) {
        cavlc_log("Incomplete frame after all NALs: %d/%d MBs\n",
                  state->mb_count, state->total_mbs_expected);
    }

    return 0;
}
