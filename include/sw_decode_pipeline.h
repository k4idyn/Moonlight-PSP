/*
 * sw_decode_pipeline.h - Asymmetric Dual-Core Software H.264 Decode Pipeline
 *
 * Architecture:
 *   Main CPU ("Front End"):
 *     - Network ingestion (UDP/RTP)
 *     - NAL unit parsing & bitstream scanning
 *     - CAVLC entropy decoding (strictly sequential)
 *     - Dumps coefficients + motion vectors to shared memory
 *     - Signals Media Engine via interrupt
 *
 *   Media Engine ("Heavy Lifter"):
 *     - Inverse Quantization (VFPU assembly)
 *     - 4x4 / 8x8 Inverse DCT (VFPU assembly)
 *     - Motion Compensation with reference frame (VFPU assembly)
 *     - YUV420 → RGBA8888 color conversion (VFPU assembly)
 *     - Writes final pixels to framebuffer
 *
 * Memory Layout:
 *   All shared buffers are 64-byte aligned for ME DMA.
 *   CPU flushes dcache before signaling ME.
 *   ME invalidates dcache before reading.
 *
 * This bypasses sceMpeg entirely — no MPEG-PS wrapping needed.
 * Raw Annex-B from Sunshine → CAVLC parse → coefficients → VFPU math → pixels.
 */

#ifndef SW_DECODE_PIPELINE_H
#define SW_DECODE_PIPELINE_H

#include <psptypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Constants
 * ============================================================================*/

#define SW_MB_WIDTH         30      /* 480 / 16 */
#define SW_MB_HEIGHT        17      /* 272 / 16 */
#define SW_TOTAL_MBS        (SW_MB_WIDTH * SW_MB_HEIGHT)  /* 510 */
#define SW_MAX_REFS         2       /* Baseline profile: 1 ref + 1 spare */
#define SW_FRAME_WIDTH      480
#define SW_FRAME_HEIGHT     272
#define SW_FRAME_STRIDE     512     /* GPU-compatible pitch (must be >= texture width, power of 2) */

/* Maximum bitstream size we handle (256KB matches existing staging buffer) */
#define SW_MAX_BITSTREAM    (256 * 1024)

/* Macroblock partition types (H.264 Baseline) */
#define SW_MB_TYPE_I4x4     0
#define SW_MB_TYPE_I16x16   1
#define SW_MB_TYPE_P16x16   2
#define SW_MB_TYPE_P16x8    3
#define SW_MB_TYPE_P8x16    4
#define SW_MB_TYPE_P8x8     5
#define SW_MB_TYPE_PSKIP    6

/* Intra 4x4 prediction modes */
#define SW_INTRA4_VERT      0
#define SW_INTRA4_HOR       1
#define SW_INTRA4_DC        2
#define SW_INTRA4_DDL       3
#define SW_INTRA4_DDR       4
#define SW_INTRA4_VR        5
#define SW_INTRA4_HD        6
#define SW_INTRA4_VL        7
#define SW_INTRA4_HU        8

/* Intra 16x16 prediction modes */
#define SW_INTRA16_VERT     0
#define SW_INTRA16_HOR      1
#define SW_INTRA16_DC       2
#define SW_INTRA16_PLANE    3

/* Intra chroma prediction modes */
#define SW_CHROMA_DC        0
#define SW_CHROMA_HOR       1
#define SW_CHROMA_VERT      2
#define SW_CHROMA_PLANE     3

/* NAL unit types */
#define SW_NAL_SLICE        1
#define SW_NAL_IDR          5
#define SW_NAL_SEI          6
#define SW_NAL_SPS          7
#define SW_NAL_PPS          8
#define SW_NAL_AUD          9

/* Slice types */
#define SW_SLICE_P          0
#define SW_SLICE_B          1
#define SW_SLICE_I          2

/* ============================================================================
 * Motion Vector
 * ============================================================================*/
typedef struct {
    s16 dx;     /* Horizontal displacement in quarter-pel units */
    s16 dy;     /* Vertical displacement in quarter-pel units */
} __attribute__((aligned(4))) SwMotionVector;

/* ============================================================================
 * Macroblock Data — Output of CAVLC entropy decode (Main CPU)
 *                   Input to VFPU reconstruction (Media Engine)
 *
 * 64-byte aligned for ME DMA coherence.
 * ============================================================================*/
typedef struct {
    /* Classification */
    u8  mb_type;                       /* SW_MB_TYPE_* */
    u8  intra16x16_mode;               /* Intra 16x16 pred mode (if I16x16) */
    u8  chroma_pred_mode;              /* Chroma prediction mode */
    u8  coded_block_pattern;           /* CBP for luma (4 bits) + chroma (2 bits) */
    s8  qp_y;                          /* Luma QP (0-51) */
    s8  qp_delta;                      /* QP delta from previous MB */
    u8  skip_flag;                     /* 1 if P_SKIP */
    u8  transform_8x8;                 /* Always 0 for Baseline profile */

    /* Intra 4x4 prediction modes (16 sub-blocks, each 0-8) */
    u8  intra4x4_modes[16];

    /* DCT Coefficients — CAVLC decoded, pre-dequantization
     * Luma: 16 blocks of 4x4 = 256 coefficients
     * Cb:    4 blocks of 4x4 =  64 coefficients
     * Cr:    4 blocks of 4x4 =  64 coefficients
     * Total: 384 s16 = 768 bytes per macroblock */
    s16 luma_coeff[16][16];            /* [block_idx][coeff_idx] zig-zag order */
    s16 cb_coeff[4][16];               /* Cb chroma coefficients */
    s16 cr_coeff[4][16];               /* Cr chroma coefficients */

    /* Luma DC coefficients for I16x16 (4x4 DC block) */
    s16 luma_dc[16];

    /* Chroma DC coefficients (2x2 blocks for each Cb, Cr) */
    s16 chroma_dc_cb[4];
    s16 chroma_dc_cr[4];

    /* Motion vectors: up to 16 for P8x8 sub-partitions (4x 4x4 = 16 MVs) */
    SwMotionVector mv[16];             /* Quarter-pel motion vectors */
    s8  ref_idx[4];                    /* Reference frame indices */

    /* Non-zero coefficient counts per 4x4 block (for CAVLC neighbor prediction) */
    u8  nz_coeff_luma[16];             /* Non-zero count per luma 4x4 block */
    u8  nz_coeff_cb[4];               /* Non-zero count per Cb 4x4 block */
    u8  nz_coeff_cr[4];               /* Non-zero count per Cr 4x4 block */
    u8  nz_coeff_dc_y;                /* Non-zero count of I16x16 luma DC block */
    u8  _pad_nz[3];                   /* Padding for alignment */

} __attribute__((aligned(64))) SwMacroblockData;

/* ============================================================================
 * Sequence Parameter Set (SPS) — Parsed from NAL Type 7
 * ============================================================================*/
typedef struct {
    u8  profile_idc;                   /* Should be 66 for Baseline */
    u8  level_idc;                     /* e.g. 21 for Level 2.1 */
    u8  constraint_set_flags;
    u8  chroma_format_idc;             /* 1 = 4:2:0 */
    u8  log2_max_frame_num;
    u8  pic_order_cnt_type;
    u8  log2_max_pic_order_cnt;
    u8  num_ref_frames;
    u16 pic_width_in_mbs;              /* 30 for 480px */
    u16 pic_height_in_map_units;       /* 17 for 272px */
    u8  frame_mbs_only_flag;
    u8  direct_8x8_inference_flag;
    u8  frame_cropping_flag;
    u8  vui_parameters_present;
    u16 crop_left, crop_right;
    u16 crop_top, crop_bottom;
    u8  valid;
} SwSPS;

/* ============================================================================
 * Picture Parameter Set (PPS) — Parsed from NAL Type 8
 * ============================================================================*/
typedef struct {
    u8  pps_id;
    u8  sps_id;
    u8  entropy_coding_mode;           /* 0 = CAVLC (Baseline), 1 = CABAC */
    u8  pic_order_present_flag;
    u8  num_ref_idx_l0;
    u8  weighted_pred_flag;
    u8  weighted_bipred_idc;
    s8  pic_init_qp;
    s8  chroma_qp_index_offset;
    u8  deblocking_filter_control;
    u8  constrained_intra_pred;
    u8  redundant_pic_cnt_present;
    u8  valid;
} SwPPS;

/* ============================================================================
 * Slice Header — Parsed per-slice from Main CPU
 * ============================================================================*/
typedef struct {
    u32 first_mb_in_slice;
    u8  slice_type;                    /* SW_SLICE_P, SW_SLICE_I, etc. */
    u8  pps_id;
    u16 frame_num;
    u8  idr_flag;
    u16 idr_pic_id;
    s8  slice_qp;                      /* pic_init_qp + slice_qp_delta */
    u8  disable_deblocking_filter;
    s8  slice_alpha_c0_offset;
    s8  slice_beta_offset;
    u8  num_ref_idx_l0_active;
} SwSliceHeader;

/* ============================================================================
 * Reference Frame Buffer — Stored in shared memory for ME access
 * ============================================================================*/
typedef struct {
    u8 *y_plane;       /* Luma plane: 480 * 272 bytes, 64-byte aligned */
    u8 *u_plane;       /* Cb plane: 240 * 136 bytes, 64-byte aligned */
    u8 *v_plane;       /* Cr plane: 240 * 136 bytes, 64-byte aligned */
    u16 frame_num;
    u8  used;
    u8  _pad;
} __attribute__((aligned(16))) SwRefFrame;

/* ============================================================================
 * Decoded Picture Buffer — Current frame YUV planes
 * ============================================================================*/
typedef struct {
    u8 *y_plane;       /* 480 * 272, 64-byte aligned */
    u8 *u_plane;       /* 240 * 136, 64-byte aligned */
    u8 *v_plane;       /* 240 * 136, 64-byte aligned */
} __attribute__((aligned(16))) SwCurrentFrame;

/* ============================================================================
 * Shared Pipeline State — Visible to both Main CPU and Media Engine
 *
 * Main CPU writes macroblock data, sets mb_ready_count.
 * ME reads macroblock data, performs reconstruction.
 * All access must go through dcache flush/invalidate.
 * ============================================================================*/
typedef struct {
    /* Macroblock array — filled by Main CPU CAVLC decoder */
    SwMacroblockData mbs[SW_TOTAL_MBS];

    /* Slice parameters for current frame */
    SwSliceHeader slice;

    /* Stream parameters (persistent across frames) */
    SwSPS sps;
    SwPPS pps;

    /* Reference frames */
    SwRefFrame ref_frames[SW_MAX_REFS];
    int active_ref;                    /* Index of current reference frame */

    /* Current decode target */
    SwCurrentFrame current;

    /* Synchronization */
    volatile int mb_count;             /* MBs actually decoded in current frame */
    volatile int total_mbs_expected;   /* Total MBs expected (width*height) */
    volatile int frame_ready;          /* 1 = CPU finished entropy decode */
    volatile int me_done;              /* 1 = ME finished reconstruction */
    volatile int pipeline_active;      /* 0 = shutdown signal */

    /* Error concealment tracking */
    int error_concealed;               /* 1 = CAVLC error concealment was used */
    int real_mb_count;                 /* MBs genuinely decoded before error (excl concealment) */

    /* Statistics */
    u32 frames_decoded;
    u32 frames_dropped;
    u32 cavlc_time_us;                 /* Last frame CPU time */
    u32 recon_time_us;                 /* Last frame ME VFPU time */

    /* Per-phase timing (set by sw_reconstruct_frame) */
    u32 mb_loop_us;                    /* MB reconstruction loop */
    u32 deblock_us;                    /* Deblocking filter */
    u32 yuv_us;                        /* YUV→RGBA conversion */
    u32 skip_mb_count;                 /* P_SKIP zero-MV MBs this frame */

    /* Incremental YUV→RGBA: previous RGBA buffer for delta-only conversion.
     * Set by orchestrator before reconstruction; NULL for IDR frames. */
    u8 *prev_rgba;
} __attribute__((aligned(64))) SwPipelineState;

/* ============================================================================
 * Bitstream Reader — Used by CAVLC entropy decoder on Main CPU
 *
 * Cache-based reader: loads up to 32 bits into a register for fast
 * bit extraction.  MIPS CLZ accelerates exp-Golomb decoding.
 * ============================================================================*/
typedef struct {
    const u8 *data;
    int size;           /* Total bytes in source buffer */
    int byte_pos;       /* Next byte to load from data[] */
    u32 cache;          /* Cached bits, MSB-aligned (32-bit window) */
    int bits_left;      /* Valid bits remaining in cache (0-32) */
} SwBitstream;

/* ============================================================================
 * Public API — Called from stream session / decoder orchestration
 * ============================================================================*/

/**
 * Initialize the software decode pipeline.
 * Allocates shared memory, reference frames, starts ME worker thread.
 * @return 0 on success, negative on error
 */
int sw_pipeline_init(void);

/**
 * Shut down the pipeline, kill ME thread, free all memory.
 */
void sw_pipeline_shutdown(void);

/**
 * Decode a complete H.264 access unit (one frame's worth of NAL units).
 * Called from the decoder thread after RTP reassembly.
 *
 * 1. Parses NALs (SPS, PPS, slice header) on Main CPU
 * 2. Runs CAVLC entropy decode for all MBs on Main CPU
 * 3. Flushes shared memory, signals ME
 * 4. ME performs IDCT + motion comp + color conversion via VFPU
 * 5. Returns pointer to RGBA8888 frame buffer
 *
 * @param nal_data   Raw Annex-B H.264 bitstream
 * @param nal_len    Length in bytes
 * @param out_rgba   Output: pointer to RGBA8888 frame (480*272*4)
 * @return 0 on success, negative on error
 */
int sw_pipeline_decode_frame(const u8 *nal_data, int nal_len, u8 **out_rgba);

/**
 * Invalidate all reference frames -- forces decoder to wait for IDR.
 * Call after a queue flush to prevent stale-reference corruption.
 */
void sw_pipeline_invalidate_refs(void);

/**
 * Get pipeline statistics.
 */
void sw_pipeline_get_stats(u32 *decoded, u32 *dropped,
                           u32 *cpu_us, u32 *me_us);

/**
 * Check if the pipeline is initialized and ready.
 */
int sw_pipeline_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* SW_DECODE_PIPELINE_H */
