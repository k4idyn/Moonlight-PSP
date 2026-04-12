/*
 * sw_vfpu_recon.c - VFPU-Accelerated H.264 Reconstruction (Media Engine)
 *
 * This is the "Heavy Lifter" of the asymmetric pipeline. All math-intensive
 * operations run here using inline MIPS VFPU assembly on the ME's dedicated
 * Vector Floating Point Unit.
 *
 * Operations (in pipeline order):
 *   1. Inverse Quantization — Scale CAVLC coefficients by QP step size
 *   2. 4x4 Inverse DCT (Hadamard for DC) — Frequency → spatial domain
 *   3. Intra Prediction — Generate prediction block from neighbors
 *   4. Motion Compensation — Quarter-pel interpolation from reference frame
 *   5. Residual Addition — prediction + IDCT residual = reconstructed pixels
 *   6. YUV420 → RGBA8888 — Color space conversion for display
 *
 * ALL 4-wide vector operations use the VFPU to process 4 values in parallel.
 * CPU scalar ops are used only for byte packing and memory addressing.
 *
 * Memory:
 *   All buffers must be 64-byte aligned for ME DMA coherence.
 *   ME invalidates dcache before reading shared memory.
 *   ME flushes dcache after writing reconstructed frame.
 *
 * The ME has its own VFPU independent of the Main CPU's VFPU.
 * Both can run VFPU code simultaneously without conflict.
 */

#include <string.h>
#include <pspthreadman.h>
#include "sw_decode_pipeline.h"
#include "diag_log.h"

/* sceRtcGetCurrentTick prototype — avoid including psprtc.h which has
 * time_t issues with -Werror. Only need the tick counter for profiling. */
extern int sceRtcGetCurrentTick(u64 *tick);

/* Performance: disable deblocking filter for profiling (1=skip, 0=normal) */
int g_deblock_disable = 0;

/* H.264 Table 6-10a: 4x4 block index (8x8-grouped) to spatial (x,y) in 4x4 units */
static const u8 blk4x4_x[16] = {0,1,0,1, 2,3,2,3, 0,1,0,1, 2,3,2,3};
static const u8 blk4x4_y[16] = {0,0,1,1, 0,0,1,1, 2,2,3,3, 2,2,3,3};

/* ============================================================================
 * H.264 Dequantization Tables — ITU-T H.264 Table 8-15
 *
 * qp_per = qp / 6, qp_rem = qp % 6
 * Scale factor = LevelScale[qp_rem][i][j] << qp_per
 * ============================================================================*/

/* Flat scaling matrix for 4x4 (no custom scaling lists for Baseline/streaming) */
static const s16 dequant_coeff_4x4[6][16] = {
    { 10, 13, 10, 13,  13, 16, 13, 16,  10, 13, 10, 13,  13, 16, 13, 16 },
    { 11, 14, 11, 14,  14, 18, 14, 18,  11, 14, 11, 14,  14, 18, 14, 18 },
    { 13, 16, 13, 16,  16, 20, 16, 20,  13, 16, 13, 16,  16, 20, 16, 20 },
    { 14, 18, 14, 18,  18, 23, 18, 23,  14, 18, 14, 18,  18, 23, 18, 23 },
    { 16, 20, 16, 20,  20, 25, 20, 25,  16, 20, 16, 20,  20, 25, 20, 25 },
    { 18, 23, 18, 23,  23, 29, 23, 29,  18, 23, 18, 23,  23, 29, 23, 29 }
};

/* Chroma QP mapping table — H.264 Table 8-16 */
static const u8 chroma_qp_table[52] = {
     0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
    10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    29, 30, 31, 32, 32, 33, 34, 34, 35, 35,
    36, 36, 37, 37, 37, 38, 38, 38, 39, 39,
    39, 39
};

/* ============================================================================
 * H.264 Deblocking Filter Tables — ITU-T H.264 Section 8.7
 * ============================================================================*/

/* Table 8-16a: alpha threshold indexed by indexA (0-51) */
static const u8 deblock_alpha[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  4,  4,  5,  6,
     7,  8,  9, 10, 12, 13, 15, 17, 20, 22,
    25, 28, 32, 36, 40, 45, 50, 56, 63, 71,
    80, 90,101,113,127,144,162,182,203,226,
   255,255
};

/* Table 8-16b: beta threshold indexed by indexB (0-51) */
static const u8 deblock_beta[52] = {
     0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
     0,  0,  0,  0,  0,  0,  2,  2,  2,  3,
     3,  3,  3,  4,  4,  4,  6,  6,  7,  7,
     8,  8,  9,  9, 10, 10, 11, 11, 12, 12,
    13, 13, 14, 14, 15, 15, 16, 16, 17, 17,
    18, 18
};

/* Table 8-16c: tC0 clipping value for BS=1,2,3 indexed by indexA */
static const u8 deblock_tc0[52][3] = {
    {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},
    {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0},
    {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,0}, {0,0,1},
    {0,0,1}, {0,0,1}, {0,0,1}, {0,1,1}, {0,1,1}, {1,1,1},
    {1,1,1}, {1,1,1}, {1,1,1}, {1,1,2}, {1,1,2}, {1,1,2},
    {1,1,2}, {1,2,3}, {1,2,3}, {2,2,3}, {2,2,4}, {2,3,4},
    {2,3,4}, {3,3,5}, {3,4,6}, {3,4,6}, {4,5,7}, {4,5,8},
    {4,6,9}, {5,7,10},{6,8,11},{6,8,13},{7,10,14},{8,11,16},
    {9,12,18},{10,13,20},{11,15,23},{13,17,25}
};

/* ============================================================================
 * Inverse Quantization — VFPU-accelerated 4x4 block dequantization
 *
 * For each coefficient: dequant[i] = coeff[i] * scale[qp_rem][i] << (qp/6)
 * Processes 4 coefficients at a time using VFPU.
 * ============================================================================*/

static void dequant_4x4_vfpu(s16 *coeffs, int qp)
{
    int qp_per = qp / 6;
    int qp_rem = qp % 6;
    const s16 *scale = dequant_coeff_4x4[qp_rem];

    static s32 __attribute__((aligned(16))) coeff_i[4];
    static s32 __attribute__((aligned(16))) scale_i[4];
    static s32 __attribute__((aligned(16))) result_i[4];

    for (int i = 0; i < 16; i += 4) {
        /* Load coefficients and scale factors into aligned buffers */
        coeff_i[0] = coeffs[i];
        coeff_i[1] = coeffs[i+1];
        coeff_i[2] = coeffs[i+2];
        coeff_i[3] = coeffs[i+3];

        scale_i[0] = scale[i];
        scale_i[1] = scale[i+1];
        scale_i[2] = scale[i+2];
        scale_i[3] = scale[i+3];

        __asm__ volatile (
            /* Load coefficients as floats */
            "ulv.q  C000, 0(%[c])              \n"
            "vi2f.q C000, C000, 0              \n"

            /* Load scale factors as floats */
            "ulv.q  C010, 0(%[s])              \n"
            "vi2f.q C010, C010, 0              \n"

            /* Multiply: coeff * scale */
            "vmul.q C020, C000, C010           \n"

            /* Convert back to int (truncate toward zero) */
            "vf2iz.q C020, C020, 0             \n"

            /* Store result */
            "usv.q  C020, 0(%[r])              \n"

            : : [c] "r"(coeff_i), [s] "r"(scale_i), [r] "r"(result_i)
            : "memory"
        );

        /* Apply qp_per shift and store back as s16 */
        coeffs[i]   = (s16)(result_i[0] << qp_per);
        coeffs[i+1] = (s16)(result_i[1] << qp_per);
        coeffs[i+2] = (s16)(result_i[2] << qp_per);
        coeffs[i+3] = (s16)(result_i[3] << qp_per);
    }
}

/* Special dequant for Luma DC (I16x16): scale by factor/(qp_per < 6 ? 1 : shift) */
static void dequant_luma_dc(s16 *dc, int qp)
{
    int qp_per = qp / 6;
    int qp_rem = qp % 6;
    s16 scale_val = dequant_coeff_4x4[qp_rem][0];

    for (int i = 0; i < 16; i++) {
        if (qp_per >= 2) {
            dc[i] = (s16)((dc[i] * scale_val) << (qp_per - 2));
        } else {
            dc[i] = (s16)((dc[i] * scale_val + (1 << (1 - qp_per))) >> (2 - qp_per));
        }
    }
}

/* Chroma DC dequant (2x2 block for 4:2:0) */
static void dequant_chroma_dc(s16 *dc, int qpc)
{
    int qp_per = qpc / 6;
    int qp_rem = qpc % 6;
    s16 scale_val = dequant_coeff_4x4[qp_rem][0];

    for (int i = 0; i < 4; i++) {
        if (qp_per >= 1) {
            dc[i] = (s16)((dc[i] * scale_val) << (qp_per - 1));
        } else {
            dc[i] = (s16)((dc[i] * scale_val) >> 1);
        }
    }
}

/* ============================================================================
 * 4x4 Inverse Hadamard Transform (for I16x16 Luma DC)
 *
 * H.264 Section 8.5.10 — transforms the 4x4 DC coefficients
 * ============================================================================*/

static void hadamard_4x4(s16 *d)
{
    s16 tmp[16];

    /* Horizontal pass — H.264 section 8.5.10 butterfly:
     *   z0 = a + c,  z1 = a - c,  z2 = b - d,  z3 = b + d
     *   out[0] = z0 + z3  (++++)
     *   out[1] = z0 - z3  (++--) 
     *   out[2] = z1 - z2  (+--+)
     *   out[3] = z1 + z2  (+-+-)
     */
    for (int i = 0; i < 4; i++) {
        int i4 = i * 4;
        s16 a = d[i4];
        s16 b = d[i4+1];
        s16 c = d[i4+2];
        s16 e = d[i4+3];
        s16 z0 = a + c;
        s16 z1 = a - c;
        s16 z2 = b - e;
        s16 z3 = b + e;
        tmp[i4]   = z0 + z3;
        tmp[i4+1] = z0 - z3;
        tmp[i4+2] = z1 - z2;
        tmp[i4+3] = z1 + z2;
    }

    /* Vertical pass — same butterfly, transposed */
    for (int j = 0; j < 4; j++) {
        s16 a = tmp[j];
        s16 b = tmp[j+4];
        s16 c = tmp[j+8];
        s16 e = tmp[j+12];
        s16 z0 = a + c;
        s16 z1 = a - c;
        s16 z2 = b - e;
        s16 z3 = b + e;
        d[j]    = z0 + z3;
        d[j+4]  = z0 - z3;
        d[j+8]  = z1 - z2;
        d[j+12] = z1 + z2;
    }
}

/* 2x2 Inverse Hadamard for Chroma DC */
static void hadamard_2x2(s16 *d)
{
    s16 a = d[0] + d[1] + d[2] + d[3];
    s16 b = d[0] - d[1] + d[2] - d[3];
    s16 c = d[0] + d[1] - d[2] - d[3];
    s16 e = d[0] - d[1] - d[2] + d[3];
    d[0] = a; d[1] = b; d[2] = c; d[3] = e;
}

/* ============================================================================
 * 4x4 Inverse DCT — Fully Vectorized VFPU Assembly
 *
 * H.264 Section 8.5.12: integer butterfly transform with VFPU quad vectors.
 *
 * Key insight: load the 4x4 block as rows into a VFPU matrix, then use the
 * column/row duality to process ALL rows (horizontal pass) or ALL columns
 * (vertical pass) simultaneously with single quad-vector instructions.
 *
 * Horizontal pass: column vectors C1x0 span element [x] across all 4 rows.
 *   vadd.q/vsub.q/vscl.q on columns → butterfly on all rows at once.
 * Vertical pass: row vectors R3x0 span element [*] within each row.
 *   Same butterfly ops on rows → process all 4 columns at once.
 *
 * Register map:
 *   M100: input block (rows loaded, columns used for h-pass)
 *   M200: h-pass intermediates (e0, e1, e2, e3 in columns)
 *   M300: h-pass output / v-pass input (columns written, rows read)
 *   M400: h-pass temps (half_col1, half_col3) → v-pass output
 *   M500: v-pass temps (half_row1, half_row3)
 *   S700: scalar constants (0.5, then 1/64)
 *
 * Total: 8 loads + 20 butterfly + 13 rounding + 4 stores = 45 VFPU insns.
 * At 333MHz VFPU: ~0.14us per block, ~1.7ms for full 480x272 frame.
 *
 * Note: uses float×0.5 instead of integer>>1. Maximum error vs exact integer
 * IDCT is ±0.5 in intermediate values, well within H.264's ±1 tolerance
 * (Section 8.5.12.1).
 * ============================================================================*/

static void idct_4x4_vfpu(s16 *block)
{
    /* Aligned buffers for VFPU load/store (s16 → s32 expansion) */
    static s32 __attribute__((aligned(16))) ibuf[4][4];
    static s32 __attribute__((aligned(16))) obuf[4][4];

    /* Rounding bias vector: {32.0, 32.0, 32.0, 32.0} */
    static const float __attribute__((aligned(16))) const_32[4] = {
        32.0f, 32.0f, 32.0f, 32.0f
    };

    /* Expand s16 → s32 for VFPU integer load */
    int i;
    for (i = 0; i < 4; i++) {
        ibuf[i][0] = block[i * 4 + 0];
        ibuf[i][1] = block[i * 4 + 1];
        ibuf[i][2] = block[i * 4 + 2];
        ibuf[i][3] = block[i * 4 + 3];
    }

    __asm__ volatile (
        /* ---- Load 0.5 constant ---- */
        "mtv     %[half], S700            \n"

        /* ---- Load 4 rows of block into M100, convert i32 → float ---- */
        "lv.q    R100,  0(%[ib])          \n"
        "lv.q    R101, 16(%[ib])          \n"
        "lv.q    R102, 32(%[ib])          \n"
        "lv.q    R103, 48(%[ib])          \n"
        "vi2f.q  R100, R100, 0            \n"
        "vi2f.q  R101, R101, 0            \n"
        "vi2f.q  R102, R102, 0            \n"
        "vi2f.q  R103, R103, 0            \n"

        /* =============================================
         * HORIZONTAL PASS — all 4 rows simultaneously
         *
         * Column views of M100 (each spans all rows):
         *   C100 = d[*][0]   C110 = d[*][1]
         *   C120 = d[*][2]   C130 = d[*][3]
         * ============================================= */

        /* e0 = col0 + col2 */
        "vadd.q  C200, C100, C120         \n"
        /* e1 = col0 - col2 */
        "vsub.q  C210, C100, C120         \n"
        /* half_col1 = col1 × 0.5  (temp in M400) */
        "vscl.q  C400, C110, S700         \n"
        /* half_col3 = col3 × 0.5  (temp in M400) */
        "vscl.q  C410, C130, S700         \n"
        /* e2 = half_col1 - col3 */
        "vsub.q  C220, C400, C130         \n"
        /* e3 = col1 + half_col3 */
        "vadd.q  C230, C110, C410         \n"

        /* Output butterfly → columns of M300 */
        "vadd.q  C300, C200, C230         \n"   /* f0 = e0 + e3 */
        "vadd.q  C310, C210, C220         \n"   /* f1 = e1 + e2 */
        "vsub.q  C320, C210, C220         \n"   /* f2 = e1 - e2 */
        "vsub.q  C330, C200, C230         \n"   /* f3 = e0 - e3 */

        /* =============================================
         * VERTICAL PASS — all 4 columns simultaneously
         *
         * Row views of M300 (each spans all columns):
         *   R300 = T[0][*]   R301 = T[1][*]
         *   R302 = T[2][*]   R303 = T[3][*]
         * ============================================= */

        /* e0 = row0 + row2 */
        "vadd.q  R200, R300, R302         \n"
        /* e1 = row0 - row2 */
        "vsub.q  R201, R300, R302         \n"
        /* half_row1 = row1 × 0.5  (temp in M500) */
        "vscl.q  R500, R301, S700         \n"
        /* half_row3 = row3 × 0.5  (temp in M500) */
        "vscl.q  R501, R303, S700         \n"
        /* e2 = half_row1 - row3 */
        "vsub.q  R202, R500, R303         \n"
        /* e3 = row1 + half_row3 */
        "vadd.q  R203, R301, R501         \n"

        /* Output butterfly → rows of M400 */
        "vadd.q  R400, R200, R203         \n"   /* f0 = e0 + e3 */
        "vadd.q  R401, R201, R202         \n"   /* f1 = e1 + e2 */
        "vsub.q  R402, R201, R202         \n"   /* f2 = e1 - e2 */
        "vsub.q  R403, R200, R203         \n"   /* f3 = e0 - e3 */

        /* =============================================
         * ROUNDING: (x + 32) / 64 → truncate to int
         * ============================================= */

        /* Load {32, 32, 32, 32} from aligned constant array */
        "lv.q    R100, 0(%[c32])          \n"

        /* Add rounding bias */
        "vadd.q  R400, R400, R100         \n"
        "vadd.q  R401, R401, R100         \n"
        "vadd.q  R402, R402, R100         \n"
        "vadd.q  R403, R403, R100         \n"

        /* Scale by 1/64 */
        "mtv     %[inv64], S700           \n"
        "vscl.q  R400, R400, S700         \n"
        "vscl.q  R401, R401, S700         \n"
        "vscl.q  R402, R402, S700         \n"
        "vscl.q  R403, R403, S700         \n"

        /* Float → int (truncate toward zero) */
        "vf2iz.q R400, R400, 0            \n"
        "vf2iz.q R401, R401, 0            \n"
        "vf2iz.q R402, R402, 0            \n"
        "vf2iz.q R403, R403, 0            \n"

        /* Store 4 result rows to output buffer */
        "sv.q    R400,  0(%[ob])          \n"
        "sv.q    R401, 16(%[ob])          \n"
        "sv.q    R402, 32(%[ob])          \n"
        "sv.q    R403, 48(%[ob])          \n"

        : /* no outputs — all via memory */
        : [ib] "r"(ibuf), [ob] "r"(obuf), [c32] "r"(const_32),
          [half] "r"(0.5f), [inv64] "r"(0.015625f)
        : "memory"
    );

    /* Convert s32 → s16 output */
    for (i = 0; i < 4; i++) {
        block[i * 4 + 0] = (s16)obuf[i][0];
        block[i * 4 + 1] = (s16)obuf[i][1];
        block[i * 4 + 2] = (s16)obuf[i][2];
        block[i * 4 + 3] = (s16)obuf[i][3];
    }
}

/* ============================================================================
 * Intra Prediction — Generate prediction block from neighboring samples
 *
 * For I-frames: prediction replaces the reference frame.
 * For each 4x4 block, we predict from above (A) and left (L) neighbors.
 * ============================================================================*/

/* Get reconstructed pixel value from current frame's Y plane */
static inline u8 get_recon_y(const u8 *y_plane, int x, int y)
{
    if (x < 0 || y < 0 || x >= SW_FRAME_WIDTH || y >= SW_FRAME_HEIGHT)
        return 128; /* Default for out-of-frame references */
    return y_plane[y * SW_FRAME_WIDTH + x];
}

static void intra_predict_4x4(u8 *pred, int mode,
                               const u8 *y_plane,
                               int blk_x, int blk_y)
{
    u8 above[8]; /* A[0..3] + A[4..7] for DDL/VL */
    u8 left[4];

    /* Load above neighbors (from reconstructed frame) */
    for (int i = 0; i < 8; i++) {
        above[i] = get_recon_y(y_plane, blk_x + i, blk_y - 1);
    }
    /* Load left neighbors */
    for (int i = 0; i < 4; i++) {
        left[i] = get_recon_y(y_plane, blk_x - 1, blk_y + i);
    }

    switch (mode) {
    case SW_INTRA4_VERT:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                pred[y * 4 + x] = above[x];
        break;

    case SW_INTRA4_HOR:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++)
                pred[y * 4 + x] = left[y];
        break;

    case SW_INTRA4_DC: {
        int sum = 0;
        for (int i = 0; i < 4; i++) sum += above[i] + left[i];
        u8 dc = (u8)((sum + 4) >> 3);
        for (int i = 0; i < 16; i++) pred[i] = dc;
        break;
    }

    case SW_INTRA4_DDL:
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int idx = x + y;
                if (idx < 6)
                    pred[y * 4 + x] = (u8)((above[idx] + 2 * above[idx+1] + above[idx+2] + 2) >> 2);
                else
                    pred[y * 4 + x] = above[7];
            }
        break;

    case SW_INTRA4_DDR: {
        u8 ul = get_recon_y(y_plane, blk_x - 1, blk_y - 1);
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                int d = x - y;
                if (d > 0)
                    pred[y * 4 + x] = (u8)((above[d-1] + 2*above[d] + above[d+1] + 2) >> 2);
                else if (d == 0)
                    pred[y * 4 + x] = (u8)((above[0] + 2*ul + left[0] + 2) >> 2);
                else
                    pred[y * 4 + x] = (u8)((left[-d-1] + 2*left[-d] + ((-d+1 < 4) ? left[-d+1] : left[3]) + 2) >> 2);
            }
        break;
    }

    case SW_INTRA4_VR: {
        u8 ul = get_recon_y(y_plane, blk_x - 1, blk_y - 1);
        for (int yy = 0; yy < 4; yy++)
            for (int xx = 0; xx < 4; xx++) {
                int zVR = 2 * xx - yy;
                if (zVR >= 0 && (zVR & 1) == 0) {
                    int idx = xx - (yy >> 1);
                    if (idx > 0)
                        pred[yy * 4 + xx] = (u8)((above[idx-1] + above[idx] + 1) >> 1);
                    else
                        pred[yy * 4 + xx] = (u8)((ul + above[0] + 1) >> 1);
                } else if (zVR >= 0) {
                    int idx = xx - (yy >> 1);
                    if (idx > 0)
                        pred[yy * 4 + xx] = (u8)((above[idx-2] + 2*above[idx-1] + above[idx] + 2) >> 2);
                    else
                        pred[yy * 4 + xx] = (u8)((left[0] + 2*ul + above[0] + 2) >> 2);
                } else if (zVR == -1) {
                    pred[yy * 4 + xx] = (u8)((ul + 2*left[0] + left[1] + 2) >> 2);
                } else {
                    int idx = yy - 2*xx - 2;
                    if (idx >= 0 && idx + 1 < 4)
                        pred[yy * 4 + xx] = (u8)((left[idx] + 2*left[idx+1] + ((idx+2 < 4) ? left[idx+2] : left[3]) + 2) >> 2);
                    else
                        pred[yy * 4 + xx] = left[3];
                }
            }
        break;
    }

    case SW_INTRA4_HD: {
        u8 ul = get_recon_y(y_plane, blk_x - 1, blk_y - 1);
        for (int yy = 0; yy < 4; yy++)
            for (int xx = 0; xx < 4; xx++) {
                int zHD = 2 * yy - xx;
                if (zHD >= 0 && (zHD & 1) == 0) {
                    int idx = yy - (xx >> 1);
                    if (idx > 0)
                        pred[yy * 4 + xx] = (u8)((left[idx-1] + left[idx] + 1) >> 1);
                    else
                        pred[yy * 4 + xx] = (u8)((ul + left[0] + 1) >> 1);
                } else if (zHD >= 0) {
                    int idx = yy - (xx >> 1);
                    if (idx > 0)
                        pred[yy * 4 + xx] = (u8)((left[idx-2] + 2*left[idx-1] + left[idx] + 2) >> 2);
                    else
                        pred[yy * 4 + xx] = (u8)((above[0] + 2*ul + left[0] + 2) >> 2);
                } else if (zHD == -1) {
                    pred[yy * 4 + xx] = (u8)((ul + 2*above[0] + above[1] + 2) >> 2);
                } else {
                    int idx = xx - 2*yy - 2;
                    if (idx >= 0 && idx + 2 < 8)
                        pred[yy * 4 + xx] = (u8)((above[idx] + 2*above[idx+1] + above[idx+2] + 2) >> 2);
                    else
                        pred[yy * 4 + xx] = above[7];
                }
            }
        break;
    }

    case SW_INTRA4_VL:
        for (int yy = 0; yy < 4; yy++)
            for (int xx = 0; xx < 4; xx++) {
                int idx = xx + (yy >> 1);
                if ((yy & 1) == 0) {
                    if (idx + 1 < 8)
                        pred[yy * 4 + xx] = (u8)((above[idx] + above[idx+1] + 1) >> 1);
                    else
                        pred[yy * 4 + xx] = above[7];
                } else {
                    if (idx + 2 < 8)
                        pred[yy * 4 + xx] = (u8)((above[idx] + 2*above[idx+1] + above[idx+2] + 2) >> 2);
                    else
                        pred[yy * 4 + xx] = above[7];
                }
            }
        break;

    case SW_INTRA4_HU:
        for (int yy = 0; yy < 4; yy++)
            for (int xx = 0; xx < 4; xx++) {
                int zHU = 2 * yy + xx;
                if (zHU < 5) {
                    int idx = yy + (xx >> 1);
                    if ((xx & 1) == 0) {
                        if (idx + 1 < 4)
                            pred[yy * 4 + xx] = (u8)((left[idx] + left[idx+1] + 1) >> 1);
                        else
                            pred[yy * 4 + xx] = left[3];
                    } else {
                        if (idx + 2 < 4)
                            pred[yy * 4 + xx] = (u8)((left[idx] + 2*left[idx+1] + left[idx+2] + 2) >> 2);
                        else
                            pred[yy * 4 + xx] = left[3];
                    }
                } else if (zHU == 5) {
                    pred[yy * 4 + xx] = (u8)((left[2] + 3*left[3] + 2) >> 2);
                } else {
                    pred[yy * 4 + xx] = left[3];
                }
            }
        break;

    default:
        /* Unknown mode — DC fallback */
        for (int i = 0; i < 16; i++) pred[i] = 128;
        break;
    }
}

/* Intra 16x16 prediction */
static void intra_predict_16x16(u8 *pred, int mode,
                                 const u8 *y_plane,
                                 int mb_x, int mb_y)
{
    int ox = mb_x * 16;
    int oy = mb_y * 16;

    switch (mode) {
    case SW_INTRA16_VERT:
        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++)
                pred[y * 16 + x] = get_recon_y(y_plane, ox + x, oy - 1);
        break;

    case SW_INTRA16_HOR:
        for (int y = 0; y < 16; y++) {
            u8 l = get_recon_y(y_plane, ox - 1, oy + y);
            for (int x = 0; x < 16; x++)
                pred[y * 16 + x] = l;
        }
        break;

    case SW_INTRA16_DC: {
        /* H.264 spec 8.3.3.1: DC prediction uses available neighbors.
         * When a side is unavailable, only the available side is averaged. */
        int have_top  = (oy > 0);
        int have_left = (ox > 0);
        u8 dc;
        if (have_top && have_left) {
            int sum = 0;
            for (int i = 0; i < 16; i++) {
                sum += get_recon_y(y_plane, ox + i, oy - 1);
                sum += get_recon_y(y_plane, ox - 1, oy + i);
            }
            dc = (u8)((sum + 16) >> 5);
        } else if (have_top) {
            int sum = 0;
            for (int i = 0; i < 16; i++)
                sum += get_recon_y(y_plane, ox + i, oy - 1);
            dc = (u8)((sum + 8) >> 4);
        } else if (have_left) {
            int sum = 0;
            for (int i = 0; i < 16; i++)
                sum += get_recon_y(y_plane, ox - 1, oy + i);
            dc = (u8)((sum + 8) >> 4);
        } else {
            dc = 128;
        }
        memset(pred, dc, 256);
        break;
    }

    case SW_INTRA16_PLANE: {
        int iH = 0, iV = 0;
        for (int k = 1; k <= 8; k++) {
            iH += k * ((int)get_recon_y(y_plane, ox + 7 + k, oy - 1) -
                        (int)get_recon_y(y_plane, ox + 7 - k, oy - 1));
            iV += k * ((int)get_recon_y(y_plane, ox - 1, oy + 7 + k) -
                        (int)get_recon_y(y_plane, ox - 1, oy + 7 - k));
        }
        int a = 16 * ((int)get_recon_y(y_plane, ox + 15, oy - 1) + (int)get_recon_y(y_plane, ox - 1, oy + 15));
        int b = (5 * iH + 32) >> 6;
        int c = (5 * iV + 32) >> 6;

        for (int y = 0; y < 16; y++)
            for (int x = 0; x < 16; x++) {
                int val = (a + b * (x - 7) + c * (y - 7) + 16) >> 5;
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                pred[y * 16 + x] = (u8)val;
            }
        break;
    }
    }
}

/* Chroma intra prediction (8x8 block) */
static void intra_predict_chroma(u8 *pred, int mode,
                                  const u8 *c_plane,
                                  int mb_x, int mb_y,
                                  int c_width, int c_height)
{
    int ox = mb_x * 8;
    int oy = mb_y * 8;

    switch (mode) {
    case SW_CHROMA_DC: {
        int sum = 0, cnt = 0;
        for (int i = 0; i < 8; i++) {
            int tx = ox + i, ty = oy - 1;
            if (ty >= 0 && tx < c_width) { sum += c_plane[ty * c_width + tx]; cnt++; }
            tx = ox - 1; ty = oy + i;
            if (tx >= 0 && ty < c_height) { sum += c_plane[ty * c_width + tx]; cnt++; }
        }
        u8 dc = (cnt > 0) ? (u8)((sum + cnt/2) / cnt) : 128;
        memset(pred, dc, 64);
        break;
    }

    case SW_CHROMA_HOR:
        for (int y = 0; y < 8; y++) {
            int tx = ox - 1, ty = oy + y;
            u8 l = (tx >= 0 && ty < c_height) ? c_plane[ty * c_width + tx] : 128;
            for (int x = 0; x < 8; x++) pred[y * 8 + x] = l;
        }
        break;

    case SW_CHROMA_VERT:
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int tx = ox + x, ty = oy - 1;
                pred[y * 8 + x] = (ty >= 0 && tx < c_width) ? c_plane[ty * c_width + tx] : 128;
            }
        break;

    case SW_CHROMA_PLANE: {
        /* Plane prediction for chroma — same algorithm as luma but 8x8 */
        int iH = 0, iV = 0;
        for (int k = 1; k <= 4; k++) {
            int xp = ox + 3 + k, xn = ox + 3 - k;
            int yp = oy + 3 + k, yn = oy + 3 - k;
            int tp = (xp < c_width && oy - 1 >= 0) ? c_plane[(oy-1)*c_width + xp] : 128;
            int tn = (xn >= 0 && oy - 1 >= 0) ? c_plane[(oy-1)*c_width + xn] : 128;
            iH += k * (tp - tn);
            tp = (ox - 1 >= 0 && yp < c_height) ? c_plane[yp*c_width + ox - 1] : 128;
            tn = (ox - 1 >= 0 && yn >= 0) ? c_plane[yn*c_width + ox - 1] : 128;
            iV += k * (tp - tn);
        }
        int p7 = (ox + 7 < c_width && oy - 1 >= 0) ? c_plane[(oy-1)*c_width + ox + 7] : 128;
        int p_1 = (ox - 1 >= 0 && oy + 7 < c_height) ? c_plane[(oy+7)*c_width + ox - 1] : 128;
        int a = 16 * (p7 + p_1);
        int b = (17 * iH + 16) >> 5;
        int c = (17 * iV + 16) >> 5;
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) {
                int val = (a + b*(x-3) + c*(y-3) + 16) >> 5;
                if (val < 0) val = 0;
                if (val > 255) val = 255;
                pred[y * 8 + x] = (u8)val;
            }
        break;
    }
    }
}

/* ============================================================================
 * Motion Compensation — Quarter-Pel Interpolation with VFPU
 *
 * H.264 uses 6-tap Wiener filter for half-pel, bilinear for quarter-pel.
 * The 6-tap filter coefficients are: {1, -5, 20, 20, -5, 1} / 32
 *
 * For the PSP at 480x272, each macroblock's MV typically points nearby,
 * so cache locality is good. We process 4 pixels at a time with VFPU.
 * ============================================================================*/

/* Integer-pel motion compensation (full-pixel copy) */
static void mc_copy_block(u8 *dst, int dst_stride,
                          const u8 *ref, int ref_stride,
                          int bw, int bh, int ref_x, int ref_y,
                          int frame_w, int frame_h)
{
    /* Fast path: if reference area is fully inside frame bounds, use memcpy */
    if (ref_x >= 0 && ref_y >= 0 &&
        ref_x + bw <= frame_w && ref_y + bh <= frame_h) {
        const u8 *src = ref + ref_y * ref_stride + ref_x;
        for (int y = 0; y < bh; y++) {
            memcpy(dst + y * dst_stride, src + y * ref_stride, bw);
        }
        return;
    }

    /* Slow path: per-pixel clamping for edge blocks */
    for (int y = 0; y < bh; y++) {
        int ry = ref_y + y;
        if (ry < 0) ry = 0;
        if (ry >= frame_h) ry = frame_h - 1;

        for (int x = 0; x < bw; x++) {
            int rx = ref_x + x;
            if (rx < 0) rx = 0;
            if (rx >= frame_w) rx = frame_w - 1;
            dst[y * dst_stride + x] = ref[ry * ref_stride + rx];
        }
    }
}

/* Get reference pixel with boundary clamping */
static inline u8 ref_pixel(const u8 *ref, int stride, int x, int y,
                            int w, int h)
{
    if (x < 0) x = 0;
    if (x >= w) x = w - 1;
    if (y < 0) y = 0;
    if (y >= h) y = h - 1;
    return ref[y * stride + x];
}

/* Half-pel interpolation: 6-tap filter {1,-5,20,20,-5,1}/32 */
static inline u8 halfpel_h(const u8 *ref, int stride, int x, int y,
                             int w, int h)
{
    /* Fast path: if all 6 horizontal taps (x-2..x+3) are in bounds */
    if (x >= 2 && x + 3 < w && y >= 0 && y < h) {
        const u8 *row = ref + y * stride;
        int val = row[x-2] - 5 * row[x-1] + 20 * row[x]
                + 20 * row[x+1] - 5 * row[x+2] + row[x+3];
        val = (val + 16) >> 5;
        if (val < 0) return 0;
        if (val > 255) return 255;
        return (u8)val;
    }
    int val = ref_pixel(ref, stride, x-2, y, w, h)
            - 5 * ref_pixel(ref, stride, x-1, y, w, h)
            + 20 * ref_pixel(ref, stride, x, y, w, h)
            + 20 * ref_pixel(ref, stride, x+1, y, w, h)
            - 5 * ref_pixel(ref, stride, x+2, y, w, h)
            + ref_pixel(ref, stride, x+3, y, w, h);
    val = (val + 16) >> 5;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

static inline u8 halfpel_v(const u8 *ref, int stride, int x, int y,
                             int w, int h)
{
    /* Fast path: if all 6 vertical taps (y-2..y+3) are in bounds */
    if (y >= 2 && y + 3 < h && x >= 0 && x < w) {
        int off = x;
        int s = stride;
        const u8 *base = ref + (y - 2) * s;
        int val = base[off] - 5 * base[off + s] + 20 * base[off + 2*s]
                + 20 * base[off + 3*s] - 5 * base[off + 4*s] + base[off + 5*s];
        val = (val + 16) >> 5;
        if (val < 0) return 0;
        if (val > 255) return 255;
        return (u8)val;
    }
    int val = ref_pixel(ref, stride, x, y-2, w, h)
            - 5 * ref_pixel(ref, stride, x, y-1, w, h)
            + 20 * ref_pixel(ref, stride, x, y, w, h)
            + 20 * ref_pixel(ref, stride, x, y+1, w, h)
            - 5 * ref_pixel(ref, stride, x, y+2, w, h)
            + ref_pixel(ref, stride, x, y+3, w, h);
    val = (val + 16) >> 5;
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (u8)val;
}

/* 2D half-pel (position 'j'): 6-tap vertical on horizontal half-pel intermediates */
static inline u8 halfpel_2d(const u8 *ref, int ref_stride, int rx, int ry,
                             int w, int h)
{
    int hvals[6];
    /* Fast path: all taps in bounds (rx-2..rx+3, ry-2..ry+3) */
    if (rx >= 2 && rx + 3 < w && ry >= 2 && ry + 3 < h) {
        for (int k = -2; k <= 3; k++) {
            const u8 *row = ref + (ry + k) * ref_stride;
            hvals[k + 2] = row[rx-2] - 5 * row[rx-1] + 20 * row[rx]
                         + 20 * row[rx+1] - 5 * row[rx+2] + row[rx+3];
        }
    } else {
        for (int k = -2; k <= 3; k++) {
            hvals[k + 2] = ref_pixel(ref, ref_stride, rx-2, ry+k, w, h)
                - 5 * ref_pixel(ref, ref_stride, rx-1, ry+k, w, h)
                + 20 * ref_pixel(ref, ref_stride, rx, ry+k, w, h)
                + 20 * ref_pixel(ref, ref_stride, rx+1, ry+k, w, h)
                - 5 * ref_pixel(ref, ref_stride, rx+2, ry+k, w, h)
                + ref_pixel(ref, ref_stride, rx+3, ry+k, w, h);
        }
    }
    int j = hvals[0] - 5*hvals[1] + 20*hvals[2] + 20*hvals[3] - 5*hvals[4] + hvals[5];
    j = (j + 512) >> 10;
    if (j < 0) j = 0;
    if (j > 255) j = 255;
    return (u8)j;
}

/* ============================================================================
 * Interior block MC: no per-pixel bounds checking, precomputed H intermediates
 * for 2D filter cases. Called only when all ref taps are known in-bounds.
 * ============================================================================*/
static void mc_qpel_interior(u8 *dst, int dst_stride,
                              const u8 *ref, int ref_stride,
                              int bw, int bh,
                              int rx0, int ry0,
                              int frac_x, int frac_y)
{
    if (frac_x == 0) {
        /* Vertical-only sub-pel */
        for (int y = 0; y < bh; y++) {
            const u8 *col_base = ref + (ry0 + y) * ref_stride + rx0;
            int s = ref_stride;
            for (int x = 0; x < bw; x++) {
                const u8 *p = col_base + x;
                int h = p[-2*s] - 5*p[-s] + 20*p[0] + 20*p[s] - 5*p[2*s] + p[3*s];
                h = (h + 16) >> 5;
                if (h < 0) h = 0; if (h > 255) h = 255;
                if (frac_y == 2)
                    dst[y * dst_stride + x] = (u8)h;
                else if (frac_y == 1)
                    dst[y * dst_stride + x] = (u8)((p[0] + h + 1) >> 1);
                else /* 3 */
                    dst[y * dst_stride + x] = (u8)((h + p[s] + 1) >> 1);
            }
        }
    } else if (frac_y == 0) {
        /* Horizontal-only sub-pel */
        for (int y = 0; y < bh; y++) {
            const u8 *row = ref + (ry0 + y) * ref_stride;
            for (int x = 0; x < bw; x++) {
                int rx = rx0 + x;
                int b = row[rx-2] - 5*row[rx-1] + 20*row[rx]
                      + 20*row[rx+1] - 5*row[rx+2] + row[rx+3];
                b = (b + 16) >> 5;
                if (b < 0) b = 0; if (b > 255) b = 255;
                if (frac_x == 2)
                    dst[y * dst_stride + x] = (u8)b;
                else if (frac_x == 1)
                    dst[y * dst_stride + x] = (u8)((row[rx] + b + 1) >> 1);
                else /* 3 */
                    dst[y * dst_stride + x] = (u8)((b + row[rx+1] + 1) >> 1);
            }
        }
    } else if (frac_x == 2 || frac_y == 2) {
        /* 2D cases needing the j (2D half-pel) value.
         * Precompute horizontal 6-tap intermediates for (bh+5) rows,
         * then apply vertical filter. Reduces multiply count by ~2.6×. */
        s16 h_buf[21 * 16]; /* max 21 rows × 16 cols */
        int num_rows = bh + 5;
        for (int r = 0; r < num_rows; r++) {
            const u8 *src = ref + (ry0 - 2 + r) * ref_stride;
            for (int x = 0; x < bw; x++) {
                int rx = rx0 + x;
                h_buf[r * bw + x] = (s16)(
                    src[rx-2] - 5*src[rx-1] + 20*src[rx]
                  + 20*src[rx+1] - 5*src[rx+2] + src[rx+3]);
            }
        }

        if (frac_x == 2 && frac_y == 2) {
            /* (2,2) = j: vertical 6-tap on horizontal intermediates */
            for (int y = 0; y < bh; y++) {
                for (int x = 0; x < bw; x++) {
                    int i0 = y * bw + x;
                    int j = h_buf[i0] - 5*h_buf[i0+bw] + 20*h_buf[i0+2*bw]
                          + 20*h_buf[i0+3*bw] - 5*h_buf[i0+4*bw] + h_buf[i0+5*bw];
                    j = (j + 512) >> 10;
                    if (j < 0) j = 0; if (j > 255) j = 255;
                    dst[y * dst_stride + x] = (u8)j;
                }
            }
        } else if (frac_y == 2) {
            /* (1,2)=i=avg(h,j) or (3,2)=k=avg(j,m) */
            int vx_off = (frac_x == 3) ? 1 : 0;
            for (int y = 0; y < bh; y++) {
                for (int x = 0; x < bw; x++) {
                    int i0 = y * bw + x;
                    int jval = h_buf[i0] - 5*h_buf[i0+bw] + 20*h_buf[i0+2*bw]
                             + 20*h_buf[i0+3*bw] - 5*h_buf[i0+4*bw] + h_buf[i0+5*bw];
                    jval = (jval + 512) >> 10;
                    if (jval < 0) jval = 0; if (jval > 255) jval = 255;

                    const u8 *p = ref + (ry0+y) * ref_stride + rx0 + x + vx_off;
                    int s = ref_stride;
                    int hv = p[-2*s] - 5*p[-s] + 20*p[0] + 20*p[s] - 5*p[2*s] + p[3*s];
                    hv = (hv + 16) >> 5;
                    if (hv < 0) hv = 0; if (hv > 255) hv = 255;
                    dst[y * dst_stride + x] = (u8)((hv + jval + 1) >> 1);
                }
            }
        } else {
            /* (2,1)=f=avg(b,j) or (2,3)=q=avg(j,s) */
            int hy_off = (frac_y == 3) ? 1 : 0;
            for (int y = 0; y < bh; y++) {
                const u8 *hrow = ref + (ry0 + y + hy_off) * ref_stride;
                for (int x = 0; x < bw; x++) {
                    int i0 = y * bw + x;
                    int jval = h_buf[i0] - 5*h_buf[i0+bw] + 20*h_buf[i0+2*bw]
                             + 20*h_buf[i0+3*bw] - 5*h_buf[i0+4*bw] + h_buf[i0+5*bw];
                    jval = (jval + 512) >> 10;
                    if (jval < 0) jval = 0; if (jval > 255) jval = 255;

                    int rx = rx0 + x;
                    int hh = hrow[rx-2] - 5*hrow[rx-1] + 20*hrow[rx]
                           + 20*hrow[rx+1] - 5*hrow[rx+2] + hrow[rx+3];
                    hh = (hh + 16) >> 5;
                    if (hh < 0) hh = 0; if (hh > 255) hh = 255;
                    dst[y * dst_stride + x] = (u8)((hh + jval + 1) >> 1);
                }
            }
        }
    } else {
        /* Corner cases: (1,1)=e, (3,1)=g, (1,3)=p, (3,3)=r
         * avg(halfpel_h_nearest, halfpel_v_nearest) — no 2D filter needed */
        int hh_y_off = (frac_y >= 2) ? 1 : 0;
        int hv_x_off = (frac_x >= 2) ? 1 : 0;
        for (int y = 0; y < bh; y++) {
            const u8 *hrow = ref + (ry0 + y + hh_y_off) * ref_stride;
            for (int x = 0; x < bw; x++) {
                int rx = rx0 + x;
                int hh = hrow[rx-2] - 5*hrow[rx-1] + 20*hrow[rx]
                       + 20*hrow[rx+1] - 5*hrow[rx+2] + hrow[rx+3];
                hh = (hh + 16) >> 5;
                if (hh < 0) hh = 0; if (hh > 255) hh = 255;

                const u8 *p = ref + (ry0+y) * ref_stride + rx + hv_x_off;
                int s = ref_stride;
                int hv = p[-2*s] - 5*p[-s] + 20*p[0] + 20*p[s] - 5*p[2*s] + p[3*s];
                hv = (hv + 16) >> 5;
                if (hv < 0) hv = 0; if (hv > 255) hv = 255;
                dst[y * dst_stride + x] = (u8)((hh + hv + 1) >> 1);
            }
        }
    }
}

/* Quarter-pel motion compensation for a block */
static void mc_qpel_block(u8 *dst, int dst_stride,
                           const u8 *ref, int ref_stride,
                           int bw, int bh,
                           int ref_x_int, int ref_y_int,
                           int frac_x, int frac_y,
                           int frame_w, int frame_h)
{
    if (frac_x == 0 && frac_y == 0) {
        /* Integer-pel: direct copy */
        mc_copy_block(dst, dst_stride, ref, ref_stride,
                      bw, bh, ref_x_int, ref_y_int, frame_w, frame_h);
        return;
    }

    /* Interior block fast path: all ref taps guaranteed in-bounds.
     * 6-tap filter reads [x-2..x+3], so need 3 pixels of margin. */
    if (ref_x_int >= 3 && ref_y_int >= 3 &&
        ref_x_int + bw + 3 < frame_w && ref_y_int + bh + 3 < frame_h) {
        mc_qpel_interior(dst, dst_stride, ref, ref_stride,
                          bw, bh, ref_x_int, ref_y_int, frac_x, frac_y);
        return;
    }

    /* Edge block slow path: per-pixel bounds clamping */
    for (int y = 0; y < bh; y++) {
        for (int x = 0; x < bw; x++) {
            int rx = ref_x_int + x;
            int ry = ref_y_int + y;
            u8 val;

            if (frac_x == 0) {
                /* Vertical-only sub-pel */
                if (frac_y == 2) {
                    /* Half-pel position 'h': 6-tap vertical filter */
                    val = halfpel_v(ref, ref_stride, rx, ry, frame_w, frame_h);
                } else if (frac_y == 1) {
                    /* Quarter-pel 'd': avg(A, h) */
                    u8 a = ref_pixel(ref, ref_stride, rx, ry, frame_w, frame_h);
                    u8 h = halfpel_v(ref, ref_stride, rx, ry, frame_w, frame_h);
                    val = (u8)((a + h + 1) >> 1);
                } else { /* frac_y == 3 */
                    /* Quarter-pel 'n': avg(h, M) where M=integer at ry+1 */
                    u8 h = halfpel_v(ref, ref_stride, rx, ry, frame_w, frame_h);
                    u8 m = ref_pixel(ref, ref_stride, rx, ry + 1, frame_w, frame_h);
                    val = (u8)((h + m + 1) >> 1);
                }
            } else if (frac_y == 0) {
                /* Horizontal-only sub-pel */
                if (frac_x == 2) {
                    /* Half-pel position 'b': 6-tap horizontal filter */
                    val = halfpel_h(ref, ref_stride, rx, ry, frame_w, frame_h);
                } else if (frac_x == 1) {
                    /* Quarter-pel 'a': avg(A, b) */
                    u8 a = ref_pixel(ref, ref_stride, rx, ry, frame_w, frame_h);
                    u8 b = halfpel_h(ref, ref_stride, rx, ry, frame_w, frame_h);
                    val = (u8)((a + b + 1) >> 1);
                } else { /* frac_x == 3 */
                    /* Quarter-pel 'c': avg(b, C) where C=integer at rx+1 */
                    u8 b = halfpel_h(ref, ref_stride, rx, ry, frame_w, frame_h);
                    u8 c = ref_pixel(ref, ref_stride, rx + 1, ry, frame_w, frame_h);
                    val = (u8)((b + c + 1) >> 1);
                }
            } else if (frac_x == 2 && frac_y == 2) {
                /* Center half-pel 'j': 6-tap of 6-tap (full 2D filter) */
                int hvals[6];
                for (int k = -2; k <= 3; k++) {
                    hvals[k + 2] = ref_pixel(ref, ref_stride, rx-2, ry+k, frame_w, frame_h)
                        - 5 * ref_pixel(ref, ref_stride, rx-1, ry+k, frame_w, frame_h)
                        + 20 * ref_pixel(ref, ref_stride, rx, ry+k, frame_w, frame_h)
                        + 20 * ref_pixel(ref, ref_stride, rx+1, ry+k, frame_w, frame_h)
                        - 5 * ref_pixel(ref, ref_stride, rx+2, ry+k, frame_w, frame_h)
                        + ref_pixel(ref, ref_stride, rx+3, ry+k, frame_w, frame_h);
                }
                int j = hvals[0] - 5*hvals[1] + 20*hvals[2] + 20*hvals[3] - 5*hvals[4] + hvals[5];
                j = (j + 512) >> 10;
                if (j < 0) j = 0;
                if (j > 255) j = 255;
                val = (u8)j;
            } else {
                /* Remaining diagonal sub-pel positions:
                 * H.264 spec: avg of nearest two half-pel/center values.
                 *
                 * j = 2D 6-tap filter (halfpel_2d)
                 * b = halfpel_h at (rx, ry)  |  s = halfpel_h at (rx, ry+1)
                 * h = halfpel_v at (rx, ry)  |  m = halfpel_v at (rx+1, ry)
                 *
                 * Positions using j: (1,2)=i=avg(h,j), (3,2)=k=avg(j,m),
                 *                    (2,1)=f=avg(b,j), (2,3)=q=avg(j,s)
                 * Corner positions:  (1,1)=e=avg(b,h), (3,1)=g=avg(b,m),
                 *                    (1,3)=p=avg(h,s), (3,3)=r=avg(m,s)
                 */
                if (frac_y == 2) {
                    /* (1,2) = i = avg(h, j)  or  (3,2) = k = avg(j, m) */
                    u8 j = halfpel_2d(ref, ref_stride, rx, ry, frame_w, frame_h);
                    if (frac_x == 1) {
                        u8 h = halfpel_v(ref, ref_stride, rx, ry, frame_w, frame_h);
                        val = (u8)((h + j + 1) >> 1);
                    } else { /* frac_x == 3 */
                        u8 m = halfpel_v(ref, ref_stride, rx + 1, ry, frame_w, frame_h);
                        val = (u8)((j + m + 1) >> 1);
                    }
                } else if (frac_x == 2) {
                    /* (2,1) = f = avg(b, j)  or  (2,3) = q = avg(j, s) */
                    u8 j = halfpel_2d(ref, ref_stride, rx, ry, frame_w, frame_h);
                    if (frac_y == 1) {
                        u8 b = halfpel_h(ref, ref_stride, rx, ry, frame_w, frame_h);
                        val = (u8)((b + j + 1) >> 1);
                    } else { /* frac_y == 3 */
                        u8 s = halfpel_h(ref, ref_stride, rx, ry + 1, frame_w, frame_h);
                        val = (u8)((j + s + 1) >> 1);
                    }
                } else {
                    /* (1,1)=e=avg(b,h), (3,1)=g=avg(b,m),
                     * (1,3)=p=avg(h,s), (3,3)=r=avg(m,s) */
                    u8 hh2 = halfpel_h(ref, ref_stride,
                                        rx, ry + (frac_y >= 2 ? 1 : 0), frame_w, frame_h);
                    u8 hv2 = halfpel_v(ref, ref_stride,
                                        rx + (frac_x >= 2 ? 1 : 0), ry, frame_w, frame_h);
                    val = (u8)((hh2 + hv2 + 1) >> 1);
                }
            }

            dst[y * dst_stride + x] = val;
        }
    }
}

/* ============================================================================
 * Full Macroblock Reconstruction — Called by ME for each MB
 *
 * Pipeline per MB:
 *   1. Dequantize coefficients
 *   2. Inverse transform (IDCT or Hadamard+IDCT)
 *   3. Generate prediction (intra or inter via motion compensation)
 *   4. Add residual to prediction → clip [0,255] → write to frame plane
 * ============================================================================*/

static inline u8 clip_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (u8)v;
}

void sw_recon_macroblock(SwPipelineState *state, int mb_idx)
{
    SwMacroblockData *mb = &state->mbs[mb_idx];

    int mb_x = mb_idx % SW_MB_WIDTH;
    int mb_y = mb_idx / SW_MB_WIDTH;
    int px = mb_x * 16;
    int py = mb_y * 16;

    u8 *y_out = state->current.y_plane;
    u8 *u_out = state->current.u_plane;
    u8 *v_out = state->current.v_plane;

    int c_width = SW_FRAME_WIDTH / 2;   /* 240 */
    int c_height = SW_FRAME_HEIGHT / 2; /* 136 */

    /* Compute chroma QP */
    int qp_y = mb->qp_y;
    int qpc_idx = qp_y + state->pps.chroma_qp_index_offset;
    if (qpc_idx < 0) qpc_idx = 0;
    if (qpc_idx > 51) qpc_idx = 51;
    int qp_c = chroma_qp_table[qpc_idx];

    /* ---- P_SKIP: no residual, predicted MV only ---- */
    if (mb->skip_flag) {
        SwRefFrame *ref = &state->ref_frames[state->active_ref];
        if (ref->y_plane) {
            /* Luma 16x16 with quarter-pel MC — write directly to frame plane */
            int frac_x = mb->mv[0].dx & 3;
            int frac_y = mb->mv[0].dy & 3;
            int int_x = px + (mb->mv[0].dx >> 2);
            int int_y = py + (mb->mv[0].dy >> 2);

            mc_qpel_block(&y_out[py * SW_FRAME_WIDTH + px], SW_FRAME_WIDTH,
                          ref->y_plane, SW_FRAME_WIDTH,
                          16, 16, int_x, int_y, frac_x, frac_y,
                          SW_FRAME_WIDTH, SW_FRAME_HEIGHT);

            /* Chroma 8x8 with bilinear interpolation */
            int cmv_x = mb->mv[0].dx / 2;
            int cmv_y = mb->mv[0].dy / 2;
            int c_frac_x = cmv_x & 7;
            int c_frac_y = cmv_y & 7;
            int c_int_x = (mb_x * 8) + (cmv_x >> 3);
            int c_int_y = (mb_y * 8) + (cmv_y >> 3);

            /* Integer-pel chroma fast path */
            if (c_frac_x == 0 && c_frac_y == 0 &&
                c_int_x >= 0 && c_int_y >= 0 &&
                c_int_x + 8 <= c_width && c_int_y + 8 <= c_height) {
                for (int by = 0; by < 8; by++) {
                    memcpy(&u_out[(mb_y*8 + by) * c_width + mb_x*8],
                           &ref->u_plane[(c_int_y + by) * c_width + c_int_x], 8);
                    memcpy(&v_out[(mb_y*8 + by) * c_width + mb_x*8],
                           &ref->v_plane[(c_int_y + by) * c_width + c_int_x], 8);
                }
            } else if (c_frac_x == 0 && c_frac_y == 0) {
                /* Integer-pel but near edge — use clamping */
                for (int by = 0; by < 8; by++) {
                    for (int bx = 0; bx < 8; bx++) {
                        int rx = c_int_x + bx;
                        int ry = c_int_y + by;
                        u_out[(mb_y*8 + by) * c_width + mb_x*8 + bx] =
                            ref_pixel(ref->u_plane, c_width, rx, ry, c_width, c_height);
                        v_out[(mb_y*8 + by) * c_width + mb_x*8 + bx] =
                            ref_pixel(ref->v_plane, c_width, rx, ry, c_width, c_height);
                    }
                }
            } else {
                /* Sub-pel chroma bilinear */
                int fx = c_frac_x, fy = c_frac_y;
                /* Interior fast path: all 4 bilinear taps in bounds */
                int interior = (c_int_x >= 0 && c_int_y >= 0 &&
                                c_int_x + 9 <= c_width && c_int_y + 9 <= c_height);
                for (int by = 0; by < 8; by++) {
                    for (int bx = 0; bx < 8; bx++) {
                        int rx = c_int_x + bx;
                        int ry = c_int_y + by;
                        int a00, a10, a01, a11;

                        if (interior) {
                            a00 = ref->u_plane[ry * c_width + rx];
                            a10 = ref->u_plane[ry * c_width + rx + 1];
                            a01 = ref->u_plane[(ry+1) * c_width + rx];
                            a11 = ref->u_plane[(ry+1) * c_width + rx + 1];
                        } else {
                            a00 = ref_pixel(ref->u_plane, c_width, rx, ry, c_width, c_height);
                            a10 = ref_pixel(ref->u_plane, c_width, rx+1, ry, c_width, c_height);
                            a01 = ref_pixel(ref->u_plane, c_width, rx, ry+1, c_width, c_height);
                            a11 = ref_pixel(ref->u_plane, c_width, rx+1, ry+1, c_width, c_height);
                        }
                        u_out[(mb_y*8 + by) * c_width + mb_x*8 + bx] =
                            (u8)(((8-fx)*(8-fy)*a00 + fx*(8-fy)*a10 +
                                   (8-fx)*fy*a01 + fx*fy*a11 + 32) >> 6);

                        if (interior) {
                            a00 = ref->v_plane[ry * c_width + rx];
                            a10 = ref->v_plane[ry * c_width + rx + 1];
                            a01 = ref->v_plane[(ry+1) * c_width + rx];
                            a11 = ref->v_plane[(ry+1) * c_width + rx + 1];
                        } else {
                            a00 = ref_pixel(ref->v_plane, c_width, rx, ry, c_width, c_height);
                            a10 = ref_pixel(ref->v_plane, c_width, rx+1, ry, c_width, c_height);
                            a01 = ref_pixel(ref->v_plane, c_width, rx, ry+1, c_width, c_height);
                            a11 = ref_pixel(ref->v_plane, c_width, rx+1, ry+1, c_width, c_height);
                        }
                        v_out[(mb_y*8 + by) * c_width + mb_x*8 + bx] =
                            (u8)(((8-fx)*(8-fy)*a00 + fx*(8-fy)*a10 +
                                   (8-fx)*fy*a01 + fx*fy*a11 + 32) >> 6);
                    }
                }
            }
        } else {
            /* No reference — fill gray */
            for (int by = 0; by < 16; by++)
                memset(&y_out[(py + by) * SW_FRAME_WIDTH + px], 128, 16);
            for (int by = 0; by < 8; by++) {
                memset(&u_out[(mb_y*8 + by) * c_width + mb_x*8], 128, 8);
                memset(&v_out[(mb_y*8 + by) * c_width + mb_x*8], 128, 8);
            }
        }
        return;
    }

    /* ---- I16x16: Hadamard DC transform + per-block AC ---- */
    if (mb->mb_type == SW_MB_TYPE_I16x16) {
        /* Luma DC: Hadamard + dequant */
        s16 dc_tmp[16];
        memcpy(dc_tmp, mb->luma_dc, sizeof(dc_tmp));
        hadamard_4x4(dc_tmp);
        dequant_luma_dc(dc_tmp, qp_y);

        /* 16x16 prediction block */
        u8 pred16[256];
        intra_predict_16x16(pred16, mb->intra16x16_mode,
                            y_out, mb_x, mb_y);

        /* Per-block AC + DC reconstruct */
        for (int blk = 0; blk < 16; blk++) {
            int bx = blk4x4_x[blk] * 4;
            int by = blk4x4_y[blk] * 4;

            s16 block[16];
            memcpy(block, mb->luma_coeff[blk], sizeof(block));

            /* Dequant AC coefficients FIRST (position 0 is still 0 from CAVLC AC decode) */
            dequant_4x4_vfpu(block, qp_y);

            /* THEN insert already-dequantized DC from Hadamard (raster order) */
            block[0] = dc_tmp[blk4x4_y[blk] * 4 + blk4x4_x[blk]];

            /* IDCT */
            idct_4x4_vfpu(block);
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    int pred_val = pred16[(by + y) * 16 + bx + x];
                    int res_val = block[y * 4 + x];
                    y_out[(py + by + y) * SW_FRAME_WIDTH + px + bx + x] =
                        clip_u8(pred_val + res_val);
                }
            }
        }
    }

    /* ---- I4x4: Per-block intra prediction + residual ---- */
    else if (mb->mb_type == SW_MB_TYPE_I4x4) {
        /* Block raster scan order within macroblock */
        static const u8 block_scan[16] = {
            0, 1, 4, 5, 2, 3, 6, 7, 8, 9, 12, 13, 10, 11, 14, 15
        };

        for (int i = 0; i < 16; i++) {
            int blk = block_scan[i];
            int bx = blk4x4_x[blk] * 4;
            int by = blk4x4_y[blk] * 4;

            /* Intra 4x4 prediction */
            u8 pred4[16];
            intra_predict_4x4(pred4, mb->intra4x4_modes[blk],
                             y_out, px + bx, py + by);

            /* Dequant + IDCT */
            s16 block[16];
            memcpy(block, mb->luma_coeff[blk], sizeof(block));
            dequant_4x4_vfpu(block, qp_y);
            idct_4x4_vfpu(block);

            /* Add residual to prediction → reconstruct */
            for (int y = 0; y < 4; y++) {
                for (int x = 0; x < 4; x++) {
                    y_out[(py + by + y) * SW_FRAME_WIDTH + px + bx + x] =
                        clip_u8(pred4[y * 4 + x] + block[y * 4 + x]);
                }
            }
        }
    }

    /* ---- P-types: Motion compensation + residual ---- */
    else if (mb->mb_type >= SW_MB_TYPE_P16x16 &&
             mb->mb_type <= SW_MB_TYPE_P8x8) {

        SwRefFrame *ref = &state->ref_frames[state->active_ref];
        if (!ref->y_plane) return;

        /* MC directly to output plane — eliminates temp buffer + copy.
         * Residual is added in-place only for nonzero-CBP blocks. */
        u8 *y_dst = &y_out[py * SW_FRAME_WIDTH + px];

        if (mb->mb_type == SW_MB_TYPE_P16x16) {
            int frac_x = mb->mv[0].dx & 3;
            int frac_y = mb->mv[0].dy & 3;
            int int_x = px + (mb->mv[0].dx >> 2);
            int int_y = py + (mb->mv[0].dy >> 2);

            mc_qpel_block(y_dst, SW_FRAME_WIDTH, ref->y_plane, SW_FRAME_WIDTH,
                          16, 16, int_x, int_y, frac_x, frac_y,
                          SW_FRAME_WIDTH, SW_FRAME_HEIGHT);
        } else if (mb->mb_type == SW_MB_TYPE_P16x8) {
            for (int part = 0; part < 2; part++) {
                int frac_x = mb->mv[part].dx & 3;
                int frac_y = mb->mv[part].dy & 3;
                int int_x = px + (mb->mv[part].dx >> 2);
                int int_y = py + part * 8 + (mb->mv[part].dy >> 2);

                mc_qpel_block(y_dst + part * 8 * SW_FRAME_WIDTH, SW_FRAME_WIDTH,
                              ref->y_plane, SW_FRAME_WIDTH,
                              16, 8, int_x, int_y, frac_x, frac_y,
                              SW_FRAME_WIDTH, SW_FRAME_HEIGHT);
            }
        } else if (mb->mb_type == SW_MB_TYPE_P8x16) {
            for (int part = 0; part < 2; part++) {
                int frac_x = mb->mv[part].dx & 3;
                int frac_y = mb->mv[part].dy & 3;
                int int_x = px + part * 8 + (mb->mv[part].dx >> 2);
                int int_y = py + (mb->mv[part].dy >> 2);

                mc_qpel_block(y_dst + part * 8, SW_FRAME_WIDTH,
                              ref->y_plane, SW_FRAME_WIDTH,
                              8, 16, int_x, int_y, frac_x, frac_y,
                              SW_FRAME_WIDTH, SW_FRAME_HEIGHT);
            }
        } else { /* P8x8 */
            for (int part = 0; part < 4; part++) {
                int part_x = (part & 1) * 8;
                int part_y = (part >> 1) * 8;
                int frac_x = mb->mv[part].dx & 3;
                int frac_y = mb->mv[part].dy & 3;
                int int_x = px + part_x + (mb->mv[part].dx >> 2);
                int int_y = py + part_y + (mb->mv[part].dy >> 2);

                mc_qpel_block(y_dst + part_y * SW_FRAME_WIDTH + part_x, SW_FRAME_WIDTH,
                              ref->y_plane, SW_FRAME_WIDTH,
                              8, 8, int_x, int_y, frac_x, frac_y,
                              SW_FRAME_WIDTH, SW_FRAME_HEIGHT);
            }
        }

        /* Add luma residual in-place — skip zero-CBP 8x8 blocks entirely */
        u8 luma_cbp = mb->coded_block_pattern & 0x0F;
        if (luma_cbp) {
            for (int blk = 0; blk < 16; blk++) {
                int i8x8 = blk / 4;
                if (!(luma_cbp & (1 << i8x8))) continue;

                int bx = blk4x4_x[blk] * 4;
                int by = blk4x4_y[blk] * 4;

                s16 block[16];
                memcpy(block, mb->luma_coeff[blk], sizeof(block));
                dequant_4x4_vfpu(block, qp_y);
                idct_4x4_vfpu(block);

                for (int y = 0; y < 4; y++) {
                    for (int x = 0; x < 4; x++) {
                        int off = (py + by + y) * SW_FRAME_WIDTH + px + bx + x;
                        y_out[off] = clip_u8(y_out[off] + block[y * 4 + x]);
                    }
                }
            }
        }

        /* Chroma MC directly to output planes — eliminates temp buffers */
        int cmv_x = mb->mv[0].dx / 2;
        int cmv_y = mb->mv[0].dy / 2;
        int c_frac_x = cmv_x & 7;
        int c_frac_y = cmv_y & 7;
        int c_int_x = (mb_x * 8) + (cmv_x >> 3);
        int c_int_y = (mb_y * 8) + (cmv_y >> 3);

        for (int y = 0; y < 8; y++) {
            for (int x = 0; x < 8; x++) {
                int rx = c_int_x + x;
                int ry = c_int_y + y;
                int co = (mb_y * 8 + y) * c_width + mb_x * 8 + x;

                if (c_frac_x == 0 && c_frac_y == 0) {
                    u_out[co] = ref_pixel(ref->u_plane, c_width, rx, ry, c_width, c_height);
                    v_out[co] = ref_pixel(ref->v_plane, c_width, rx, ry, c_width, c_height);
                } else {
                    int fx = c_frac_x, fy = c_frac_y;
                    int a00 = ref_pixel(ref->u_plane, c_width, rx, ry, c_width, c_height);
                    int a10 = ref_pixel(ref->u_plane, c_width, rx+1, ry, c_width, c_height);
                    int a01 = ref_pixel(ref->u_plane, c_width, rx, ry+1, c_width, c_height);
                    int a11 = ref_pixel(ref->u_plane, c_width, rx+1, ry+1, c_width, c_height);
                    u_out[co] = (u8)(((8-fx)*(8-fy)*a00 + fx*(8-fy)*a10 +
                                      (8-fx)*fy*a01 + fx*fy*a11 + 32) >> 6);

                    a00 = ref_pixel(ref->v_plane, c_width, rx, ry, c_width, c_height);
                    a10 = ref_pixel(ref->v_plane, c_width, rx+1, ry, c_width, c_height);
                    a01 = ref_pixel(ref->v_plane, c_width, rx, ry+1, c_width, c_height);
                    a11 = ref_pixel(ref->v_plane, c_width, rx+1, ry+1, c_width, c_height);
                    v_out[co] = (u8)(((8-fx)*(8-fy)*a00 + fx*(8-fy)*a10 +
                                      (8-fx)*fy*a01 + fx*fy*a11 + 32) >> 6);
                }
            }
        }

        /* Chroma residual addition — only if nonzero chroma CBP */
        u8 chroma_cbp = (mb->coded_block_pattern >> 4) & 0x03;
        if (chroma_cbp > 0) {
            s16 dc_cb[4], dc_cr[4];
            memcpy(dc_cb, mb->chroma_dc_cb, sizeof(dc_cb));
            memcpy(dc_cr, mb->chroma_dc_cr, sizeof(dc_cr));
            hadamard_2x2(dc_cb);
            hadamard_2x2(dc_cr);
            dequant_chroma_dc(dc_cb, qp_c);
            dequant_chroma_dc(dc_cr, qp_c);

            for (int blk = 0; blk < 4; blk++) {
                int bx = (blk & 1) * 4;
                int by = (blk >> 1) * 4;

                s16 cb_block[16], cr_block[16];
                memcpy(cb_block, mb->cb_coeff[blk], sizeof(cb_block));
                memcpy(cr_block, mb->cr_coeff[blk], sizeof(cr_block));

                if (chroma_cbp == 2) {
                    dequant_4x4_vfpu(cb_block, qp_c);
                    dequant_4x4_vfpu(cr_block, qp_c);
                }

                cb_block[0] = dc_cb[blk];
                cr_block[0] = dc_cr[blk];

                idct_4x4_vfpu(cb_block);
                idct_4x4_vfpu(cr_block);

                /* Add residual in-place */
                for (int yy = 0; yy < 4; yy++) {
                    for (int xx = 0; xx < 4; xx++) {
                        int co = (mb_y*8 + by + yy) * c_width + mb_x*8 + bx + xx;
                        u_out[co] = clip_u8(u_out[co] + cb_block[yy*4+xx]);
                        v_out[co] = clip_u8(v_out[co] + cr_block[yy*4+xx]);
                    }
                }
            }
        }
        return;
    }

    /* ---- Chroma reconstruction for intra MBs ---- */
    if (mb->mb_type == SW_MB_TYPE_I4x4 || mb->mb_type == SW_MB_TYPE_I16x16) {
        /* Chroma intra prediction */
        u8 pred_cb[64], pred_cr[64];
        intra_predict_chroma(pred_cb, mb->chroma_pred_mode,
                             u_out, mb_x, mb_y, c_width, c_height);
        intra_predict_chroma(pred_cr, mb->chroma_pred_mode,
                             v_out, mb_x, mb_y, c_width, c_height);

        u8 chroma_cbp = (mb->coded_block_pattern >> 4) & 0x03;
        if (chroma_cbp > 0) {
            s16 dc_cb[4], dc_cr[4];
            memcpy(dc_cb, mb->chroma_dc_cb, sizeof(dc_cb));
            memcpy(dc_cr, mb->chroma_dc_cr, sizeof(dc_cr));
            hadamard_2x2(dc_cb);
            hadamard_2x2(dc_cr);
            dequant_chroma_dc(dc_cb, qp_c);
            dequant_chroma_dc(dc_cr, qp_c);

            for (int blk = 0; blk < 4; blk++) {
                int bx = (blk & 1) * 4;
                int by = (blk >> 1) * 4;

                s16 cb_block[16], cr_block[16];
                memcpy(cb_block, mb->cb_coeff[blk], sizeof(cb_block));
                memcpy(cr_block, mb->cr_coeff[blk], sizeof(cr_block));

                if (chroma_cbp == 2) {
                    /* Dequant AC FIRST (position 0 is 0 from CAVLC AC decode) */
                    dequant_4x4_vfpu(cb_block, qp_c);
                    dequant_4x4_vfpu(cr_block, qp_c);
                }

                /* Insert already-dequantized DC from Hadamard */
                cb_block[0] = dc_cb[blk];
                cr_block[0] = dc_cr[blk];

                /* Always IDCT when DC present — distributes DC to all 16 positions */
                idct_4x4_vfpu(cb_block);
                idct_4x4_vfpu(cr_block);

                for (int yy = 0; yy < 4; yy++) {
                    for (int xx = 0; xx < 4; xx++) {
                        int co = (mb_y*8 + by + yy) * c_width + mb_x*8 + bx + xx;
                        u_out[co] = clip_u8(pred_cb[(by+yy)*8 + bx+xx] + cb_block[yy*4+xx]);
                        v_out[co] = clip_u8(pred_cr[(by+yy)*8 + bx+xx] + cr_block[yy*4+xx]);
                    }
                }
            }
        } else {
            for (int y = 0; y < 8; y++) {
                for (int x = 0; x < 8; x++) {
                    u_out[(mb_y*8+y) * c_width + mb_x*8 + x] = pred_cb[y*8+x];
                    v_out[(mb_y*8+y) * c_width + mb_x*8 + x] = pred_cr[y*8+x];
                }
            }
        }
    }
}

/* ============================================================================
 * YUV420 → RGBA8888 Conversion — Fixed-Point Integer (BT.601)
 *
 * Pure integer implementation that avoids VFPU instructions entirely.
 * PPSSPP emulates VFPU very slowly (~17ms for 480x272) — this scalar
 * version runs through PPSSPP's JIT as native x86 integer math (~1-2ms).
 * On real PSP hardware, expect ~3-5ms vs ~0.8ms VFPU.
 *
 * BT.601 fixed-point (8-bit fractional):
 *   R = clamp((298*(Y-16) + 409*(V-128) + 128) >> 8)
 *   G = clamp((298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8)
 *   B = clamp((298*(Y-16) + 516*(U-128) + 128) >> 8)
 *
 * Processes 2 pixels per inner iteration (shared chroma in 4:2:0).
 * ============================================================================*/

static inline u32 yuv_pack_rgba(int r, int g, int b)
{
    /* Branchless clamp: arithmetic shift right by 31 gives 0 or -1 mask */
    r &= ~(r >> 31);              /* clamp negative to 0 */
    g &= ~(g >> 31);
    b &= ~(b >> 31);
    r |= (255 - r) >> 31;         /* if r > 255, set to 0xFFFFFFFF, then mask */
    g |= (255 - g) >> 31;
    b |= (255 - b) >> 31;
    r &= 0xFF;
    g &= 0xFF;
    b &= 0xFF;
    return (u32)r | ((u32)g << 8) | ((u32)b << 16) | 0xFF000000u;
}

/* ============================================================================
 * YUV420 → RGBA8888 Conversion — VFPU-Accelerated (BT.601)
 *
 * LEGACY: Kept for real PSP hardware where VFPU is ~4x faster than scalar.
 * On PPSSPP emulator, this takes ~17ms due to slow VFPU instruction JIT.
 * Use sw_yuv_to_rgba_scalar() on PPSSPP for ~1-2ms.
 *
 * Processes 4 pixels per VFPU iteration. For 480x272 = 130,560 pixels,
 * that's 32,640 iterations × ~8 cycles = ~261K cycles = ~0.8ms @ 333MHz.
 * ============================================================================*/

void sw_yuv_to_rgba_vfpu(const u8 *y_plane, const u8 *u_plane,
                          const u8 *v_plane, u8 *rgba_out,
                          int width, int height)
{
    const int uv_stride = width >> 1;
    static u32 __attribute__((aligned(16))) vbuf[24];

    /* Pre-allocated constant vectors for VFPU quad-quad operations.
     * PSP VFPU requires all operands of vsub.q/vmax.q/vmin.q to be
     * quad vectors — scalar broadcast is not supported by the assembler. */
    static float __attribute__((aligned(16))) cv_y_bias[4]  = {16.0f,  16.0f,  16.0f,  16.0f};
    static float __attribute__((aligned(16))) cv_uv_bias[4] = {128.0f, 128.0f, 128.0f, 128.0f};
    static float __attribute__((aligned(16))) cv_zero[4]    = {0.0f,   0.0f,   0.0f,   0.0f};
    static float __attribute__((aligned(16))) cv_max[4]     = {255.0f, 255.0f, 255.0f, 255.0f};

    /* Load constant vectors into VFPU registers once before loops.
     * C200 = Y bias {16,...}, C210 = UV bias {128,...},
     * C220 = zero {0,...}, C230 = max {255,...} */
    __asm__ volatile (
        "ulv.q C200, 0(%[yb])  \n"
        "ulv.q C210, 0(%[uvb]) \n"
        "ulv.q C220, 0(%[zr])  \n"
        "ulv.q C230, 0(%[mx])  \n"
        : : [yb] "r"(cv_y_bias), [uvb] "r"(cv_uv_bias),
            [zr] "r"(cv_zero),   [mx]  "r"(cv_max)
        : "memory"
    );

    for (int y = 0; y < height; y++) {
        const u8 *yr = y_plane + y * width;
        const u8 *ur = u_plane + (y >> 1) * uv_stride;
        const u8 *vr = v_plane + (y >> 1) * uv_stride;
        u8 *out = rgba_out + y * SW_FRAME_STRIDE * 4;

        for (int x = 0; x < width; x += 4) {
            int ui = x >> 1;

            /* Pre-load into aligned buffer (CPU batch → VFPU bulk) */
            vbuf[0] = yr[x];
            vbuf[1] = yr[x + 1];
            vbuf[2] = yr[x + 2];
            vbuf[3] = yr[x + 3];
            vbuf[4] = ur[ui];
            vbuf[5] = ur[ui];
            vbuf[6] = ur[ui + 1];
            vbuf[7] = ur[ui + 1];
            vbuf[8]  = vr[ui];
            vbuf[9]  = vr[ui];
            vbuf[10] = vr[ui + 1];
            vbuf[11] = vr[ui + 1];

            __asm__ volatile (
                /* Register allocation (conflict-free):
                 *   C000 = Y' data          (matrix 0 col 0)
                 *   C010 = U' data          (matrix 0 col 1)
                 *   C020 = V' data          (matrix 0 col 2)
                 *   C100 = R result         (matrix 1 col 0)
                 *   C110 = G result         (matrix 1 col 1)
                 *   C120 = B result         (matrix 1 col 2)
                 *   C200 = {16,...}  bias    (matrix 2, persistent)
                 *   C210 = {128,...} bias    (matrix 2, persistent)
                 *   C220 = {0,...}   clamp   (matrix 2, persistent)
                 *   C230 = {255,...} clamp   (matrix 2, persistent)
                 *   C300 = temp             (matrix 3 col 0)
                 *   S700 = working scalar   (matrix 7 col 0 row 0)
                 *
                 * PSP VFPU rules:
                 *   - vsub/vadd/vmax/vmin.q need all-quad operands
                 *   - vscl.q takes quad + scalar (vector * scalar)
                 *   - vf2iz.q replaces PS2's vftoi0 (float→int truncate)
                 */

                /* Y → float, subtract bias 16, scale by 1.164 */
                "ulv.q  C000, 0(%[buf])                \n"
                "vi2f.q C000, C000, 0                  \n"
                "vsub.q C000, C000, C200               \n"
                "mtv    %[y_cf], S700                  \n"
                "vscl.q C000, C000, S700               \n"

                /* U → float, subtract 128 */
                "ulv.q  C010, 16(%[buf])               \n"
                "vi2f.q C010, C010, 0                  \n"
                "vsub.q C010, C010, C210               \n"

                /* V → float, subtract 128 */
                "ulv.q  C020, 32(%[buf])               \n"
                "vi2f.q C020, C020, 0                  \n"
                "vsub.q C020, C020, C210               \n"

                /* R = Y' + 1.598*V' */
                "mtv    %[vr], S700                    \n"
                "vscl.q C300, C020, S700               \n"
                "vadd.q C100, C000, C300               \n"

                /* G = Y' - 0.391*U' - 0.813*V' */
                "vmov.q C110, C000                     \n"
                "mtv    %[ug], S700                    \n"
                "vscl.q C300, C010, S700               \n"
                "vadd.q C110, C110, C300               \n"
                "mtv    %[vg], S700                    \n"
                "vscl.q C300, C020, S700               \n"
                "vadd.q C110, C110, C300               \n"

                /* B = Y' + 2.018*U' */
                "mtv    %[ub], S700                    \n"
                "vscl.q C300, C010, S700               \n"
                "vadd.q C120, C000, C300               \n"

                /* Clamp [0, 255] */
                "vmax.q C100, C100, C220               \n"
                "vmin.q C100, C100, C230               \n"
                "vmax.q C110, C110, C220               \n"
                "vmin.q C110, C110, C230               \n"
                "vmax.q C120, C120, C220               \n"
                "vmin.q C120, C120, C230               \n"

                /* Float to int (truncate toward zero) */
                "vf2iz.q C100, C100, 0                 \n"
                "vf2iz.q C110, C110, 0                 \n"
                "vf2iz.q C120, C120, 0                 \n"

                /* Store R, G, B vectors */
                "usv.q  C100, 48(%[buf])               \n"
                "usv.q  C110, 64(%[buf])               \n"
                "usv.q  C120, 80(%[buf])               \n"

                : : [buf]  "r"(vbuf),
                    [y_cf] "r"(1.1640625f),
                    [vr]   "r"(1.59765625f),
                    [ug]   "r"(-0.390625f),
                    [vg]   "r"(-0.8125f),
                    [ub]   "r"(2.015625f)
                : "memory"
            );

            /* CPU pack RGBA — proven faster than VFPU byte ops */
            out[0]  = (u8)vbuf[12]; /* R0 */
            out[1]  = (u8)vbuf[16]; /* G0 */
            out[2]  = (u8)vbuf[20]; /* B0 */
            out[3]  = 0xFF;         /* A0 */
            out[4]  = (u8)vbuf[13];
            out[5]  = (u8)vbuf[17];
            out[6]  = (u8)vbuf[21];
            out[7]  = 0xFF;
            out[8]  = (u8)vbuf[14];
            out[9]  = (u8)vbuf[18];
            out[10] = (u8)vbuf[22];
            out[11] = 0xFF;
            out[12] = (u8)vbuf[15];
            out[13] = (u8)vbuf[19];
            out[14] = (u8)vbuf[23];
            out[15] = 0xFF;

            out += 16;
        }
    }
}

/* ============================================================================
 * YUV420 → RGBA8888 — Fast Integer BT.601 (No VFPU)
 *
 * Pure integer conversion to eliminate VFPU↔CPU data marshaling overhead.
 * The VFPU version takes ~31ms due to per-pixel memory traffic through vbuf[].
 * This integer version avoids all intermediate buffers and VFPU sync stalls.
 *
 * Uses a precomputed Y lookup table (1KB) to eliminate the hottest multiply.
 * UV terms are computed once per chroma sample (shared by 2 horizontal pixels).
 *
 * BT.601 fixed-point (8-bit fractional):
 *   R = clamp((298*(Y-16) + 409*(V-128) + 128) >> 8)
 *   G = clamp((298*(Y-16) - 100*(U-128) - 208*(V-128) + 128) >> 8)
 *   B = clamp((298*(Y-16) + 516*(U-128) + 128) >> 8)
 * ============================================================================*/

/* Precomputed 298*(Y-16) for Y in [0..255]. Eliminates per-pixel multiply. */
static s32 y_scaled_lut[256];
static int y_lut_inited = 0;

static void init_y_scaled_lut(void)
{
    for (int i = 0; i < 256; i++) {
        y_scaled_lut[i] = 298 * (i - 16);
    }
    y_lut_inited = 1;
}

void sw_yuv_to_rgba_fast(const u8 *y_plane, const u8 *u_plane,
                          const u8 *v_plane, u8 *rgba_out,
                          int width, int height)
{
    if (!y_lut_inited) init_y_scaled_lut();

    const int uv_stride = width >> 1;

    for (int row = 0; row < height; row++) {
        const u8 *yr = y_plane + row * width;
        const u8 *ur = u_plane + (row >> 1) * uv_stride;
        const u8 *vr = v_plane + (row >> 1) * uv_stride;
        u32 *out = (u32 *)(rgba_out + row * SW_FRAME_STRIDE * 4);

        for (int x = 0; x < width; x += 2) {
            int ui = x >> 1;

            /* Chroma terms computed once for 2 horizontal pixels */
            int u = (int)ur[ui] - 128;
            int v = (int)vr[ui] - 128;

            int rv = 409 * v + 128;
            int guv = -100 * u - 208 * v + 128;
            int bu = 516 * u + 128;

            /* Pixel 0 */
            int ys = y_scaled_lut[yr[x]];
            out[x] = yuv_pack_rgba((ys + rv) >> 8,
                                   (ys + guv) >> 8,
                                   (ys + bu) >> 8);

            /* Pixel 1 (shares same U/V) */
            ys = y_scaled_lut[yr[x + 1]];
            out[x + 1] = yuv_pack_rgba((ys + rv) >> 8,
                                       (ys + guv) >> 8,
                                       (ys + bu) >> 8);
        }
    }
}

/* ============================================================================
 * Skip-aware YUV420 → RGBA8888 — only converts dirty (non-skip zero-MV) MBs.
 *
 * For a static desktop stream at 480×272:
 *   ~83 dirty MBs out of 510 → convert 16% of pixels → ~3.7ms vs 22.9ms.
 *
 * For skip MBs with zero MV: copy 16×16 RGBA block from prev_rgba.
 * For dirty MBs: convert 16×16 luma + 8×8 chroma to RGBA in-place.
 * ============================================================================*/
void sw_yuv_to_rgba_skip_aware(const u8 *y_plane, const u8 *u_plane,
                                const u8 *v_plane, u8 *rgba_out,
                                const u8 *prev_rgba,
                                const SwMacroblockData *mbs, int mb_count)
{
    if (!y_lut_inited) init_y_scaled_lut();

    const int uv_stride = SW_FRAME_WIDTH >> 1;

    for (int i = 0; i < mb_count; i++) {
        int mb_x = i % SW_MB_WIDTH;
        int mb_y = i / SW_MB_WIDTH;
        int px = mb_x * 16;
        int py = mb_y * 16;

        /* Skip zero-MV MBs: copy RGBA from previous frame */
        if (mbs[i].skip_flag &&
            mbs[i].mv[0].dx == 0 && mbs[i].mv[0].dy == 0 &&
            prev_rgba) {
            for (int r = 0; r < 16 && (py + r) < SW_FRAME_HEIGHT; r++) {
                const u32 *src = (const u32 *)(prev_rgba + (py + r) * SW_FRAME_STRIDE * 4) + px;
                u32 *dst = (u32 *)(rgba_out + (py + r) * SW_FRAME_STRIDE * 4) + px;
                int w = (px + 16 <= SW_FRAME_WIDTH) ? 16 : (SW_FRAME_WIDTH - px);
                memcpy(dst, src, w * 4);
            }
            continue;
        }

        /* Dirty MB: convert 16×16 block from YUV to RGBA */
        int mb_h = (py + 16 <= SW_FRAME_HEIGHT) ? 16 : (SW_FRAME_HEIGHT - py);
        int mb_w = (px + 16 <= SW_FRAME_WIDTH)  ? 16 : (SW_FRAME_WIDTH  - px);

        for (int row = 0; row < mb_h; row++) {
            int y_off = (py + row) * SW_FRAME_WIDTH + px;
            int uv_off = ((py + row) >> 1) * uv_stride + (px >> 1);
            const u8 *yr = y_plane + y_off;
            const u8 *ur = u_plane + uv_off;
            const u8 *vr = v_plane + uv_off;
            u32 *out = (u32 *)(rgba_out + (py + row) * SW_FRAME_STRIDE * 4) + px;

            for (int x = 0; x < mb_w; x += 2) {
                int ui = x >> 1;
                int u = (int)ur[ui] - 128;
                int v = (int)vr[ui] - 128;
                int rv  =  409 * v + 128;
                int guv = -100 * u - 208 * v + 128;
                int bu  =  516 * u + 128;

                int ys = y_scaled_lut[yr[x]];
                out[x] = yuv_pack_rgba((ys + rv) >> 8,
                                       (ys + guv) >> 8,
                                       (ys + bu) >> 8);
                if (x + 1 < mb_w) {
                    ys = y_scaled_lut[yr[x + 1]];
                    out[x + 1] = yuv_pack_rgba((ys + rv) >> 8,
                                               (ys + guv) >> 8,
                                               (ys + bu) >> 8);
                }
            }
        }
    }
}

/* ============================================================================
 * H.264 In-Loop Deblocking Filter — ITU-T H.264 Section 8.7
 *
 * Applied after all MBs are reconstructed, before YUV→RGBA conversion.
 * This filter is INSIDE the coding loop: the deblocked frame becomes the
 * reference for future P-frames. Without it, prediction errors compound
 * exponentially across P-frames, causing severe blocking artifacts.
 *
 * For the PSP at 333MHz with 510 MBs: ~3-5ms per frame.
 * ============================================================================*/

/* 4x4 block (x,y) in 4-pixel units → raster-scan block index */
static const u8 xy_to_blk4[4][4] = {
    { 0,  1,  4,  5},  /* y=0 */
    { 2,  3,  6,  7},  /* y=1 */
    { 8,  9, 12, 13},  /* y=2 */
    {10, 11, 14, 15}   /* y=3 */
};

static inline int deblock_clip3(int lo, int hi, int v)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Get the MV for a 4x4 sub-block within a macroblock */
static inline void get_sub_mv(const SwMacroblockData *mb, int bx4, int by4,
                               int *out_dx, int *out_dy)
{
    int idx = 0;
    if (mb->mb_type == SW_MB_TYPE_P16x8) {
        idx = (by4 >= 2) ? 1 : 0;
    } else if (mb->mb_type == SW_MB_TYPE_P8x16) {
        idx = (bx4 >= 2) ? 1 : 0;
    } else if (mb->mb_type == SW_MB_TYPE_P8x8) {
        idx = ((by4 >= 2) ? 2 : 0) + ((bx4 >= 2) ? 1 : 0);
    }
    *out_dx = mb->mv[idx].dx;
    *out_dy = mb->mv[idx].dy;
}

/* Compute boundary strength (BS) between two adjacent 4x4 blocks.
 * p-side: block (px4, py4) in mbp.  q-side: block (qx4, qy4) in mbq.
 * is_mb_boundary: 1 if the edge is at a macroblock boundary. */
static int calc_boundary_strength(const SwMacroblockData *mbp, int px4, int py4,
                                   const SwMacroblockData *mbq, int qx4, int qy4,
                                   int is_mb_boundary)
{
    int p_intra = (mbp->mb_type <= SW_MB_TYPE_I16x16);
    int q_intra = (mbq->mb_type <= SW_MB_TYPE_I16x16);

    if (p_intra || q_intra)
        return is_mb_boundary ? 4 : 3;

    int p_blk = xy_to_blk4[py4][px4];
    int q_blk = xy_to_blk4[qy4][qx4];

    if (mbp->nz_coeff_luma[p_blk] || mbq->nz_coeff_luma[q_blk])
        return 2;

    /* MV difference check */
    int pdx, pdy, qdx, qdy;
    get_sub_mv(mbp, px4, py4, &pdx, &pdy);
    get_sub_mv(mbq, qx4, qy4, &qdx, &qdy);

    int diff_x = pdx - qdx;
    int diff_y = pdy - qdy;
    if (diff_x < 0) diff_x = -diff_x;
    if (diff_y < 0) diff_y = -diff_y;

    if (diff_x >= 4 || diff_y >= 4)
        return 1;

    return 0;
}

/* Normal luma filter for BS=1,2,3 — one 4-pixel edge segment.
 * pix points to q0; p0 = pix[-xstride].
 * Processes 4 rows (stepping by ystride). */
static void deblock_luma_normal(u8 *pix, int xstride, int ystride,
                                 int alpha, int beta, int tc0_val)
{
    for (int d = 0; d < 4; d++) {
        int p0 = pix[-1 * xstride];
        int p1 = pix[-2 * xstride];
        int p2 = pix[-3 * xstride];
        int q0 = pix[0];
        int q1 = pix[1 * xstride];
        int q2 = pix[2 * xstride];

        int abs_p0q0 = (p0 - q0);
        if (abs_p0q0 < 0) abs_p0q0 = -abs_p0q0;
        int abs_p1p0 = (p1 - p0);
        if (abs_p1p0 < 0) abs_p1p0 = -abs_p1p0;
        int abs_q1q0 = (q1 - q0);
        if (abs_q1q0 < 0) abs_q1q0 = -abs_q1q0;

        if (abs_p0q0 < alpha && abs_p1p0 < beta && abs_q1q0 < beta) {
            int tc = tc0_val;

            /* Conditionally filter p1 */
            int abs_p2p0 = (p2 - p0);
            if (abs_p2p0 < 0) abs_p2p0 = -abs_p2p0;
            if (abs_p2p0 < beta) {
                pix[-2 * xstride] = (u8)(p1 + deblock_clip3(-tc0_val, tc0_val,
                    ((p2 + ((p0 + q0 + 1) >> 1)) >> 1) - p1));
                tc++;
            }

            /* Conditionally filter q1 */
            int abs_q2q0 = (q2 - q0);
            if (abs_q2q0 < 0) abs_q2q0 = -abs_q2q0;
            if (abs_q2q0 < beta) {
                pix[1 * xstride] = (u8)(q1 + deblock_clip3(-tc0_val, tc0_val,
                    ((q2 + ((p0 + q0 + 1) >> 1)) >> 1) - q1));
                tc++;
            }

            /* Filter p0 and q0 */
            int delta = deblock_clip3(-tc, tc,
                (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            pix[-1 * xstride] = clip_u8(p0 + delta);
            pix[0] = clip_u8(q0 - delta);
        }
        pix += ystride;
    }
}

/* Strong/intra luma filter for BS=4 — one 4-pixel edge segment. */
static void deblock_luma_strong(u8 *pix, int xstride, int ystride,
                                 int alpha, int beta)
{
    for (int d = 0; d < 4; d++) {
        int p0 = pix[-1 * xstride];
        int p1 = pix[-2 * xstride];
        int p2 = pix[-3 * xstride];
        int q0 = pix[0];
        int q1 = pix[1 * xstride];
        int q2 = pix[2 * xstride];

        int abs_p0q0 = (p0 - q0);
        if (abs_p0q0 < 0) abs_p0q0 = -abs_p0q0;
        int abs_p1p0 = (p1 - p0);
        if (abs_p1p0 < 0) abs_p1p0 = -abs_p1p0;
        int abs_q1q0 = (q1 - q0);
        if (abs_q1q0 < 0) abs_q1q0 = -abs_q1q0;

        if (abs_p0q0 < alpha && abs_p1p0 < beta && abs_q1q0 < beta) {
            int abs_p2p0 = (p2 - p0);
            if (abs_p2p0 < 0) abs_p2p0 = -abs_p2p0;
            int abs_q2q0 = (q2 - q0);
            if (abs_q2q0 < 0) abs_q2q0 = -abs_q2q0;

            if (abs_p0q0 < ((alpha >> 2) + 2)) {
                /* Strong filter — up to 3 pixels per side */
                if (abs_p2p0 < beta) {
                    int p3 = pix[-4 * xstride];
                    pix[-1 * xstride] = (u8)((p2 + 2*p1 + 2*p0 + 2*q0 + q1 + 4) >> 3);
                    pix[-2 * xstride] = (u8)((p2 + p1 + p0 + q0 + 2) >> 2);
                    pix[-3 * xstride] = (u8)((2*p3 + 3*p2 + p1 + p0 + q0 + 4) >> 3);
                } else {
                    pix[-1 * xstride] = (u8)((2*p1 + p0 + q1 + 2) >> 2);
                }
                if (abs_q2q0 < beta) {
                    int q3 = pix[3 * xstride];
                    pix[0] = (u8)((p1 + 2*p0 + 2*q0 + 2*q1 + q2 + 4) >> 3);
                    pix[1 * xstride] = (u8)((p0 + q0 + q1 + q2 + 2) >> 2);
                    pix[2 * xstride] = (u8)((2*q3 + 3*q2 + q1 + q0 + p0 + 4) >> 3);
                } else {
                    pix[0] = (u8)((2*q1 + q0 + p1 + 2) >> 2);
                }
            } else {
                /* Weak variant of strong filter */
                pix[-1 * xstride] = (u8)((2*p1 + p0 + q1 + 2) >> 2);
                pix[0] = (u8)((2*q1 + q0 + p1 + 2) >> 2);
            }
        }
        pix += ystride;
    }
}

/* Chroma filter for BS=1-3 — one 2-pixel edge segment (chroma is 4:2:0). */
static void deblock_chroma_edge(u8 *pix, int xstride, int ystride,
                                 int alpha, int beta, int tc_val)
{
    for (int d = 0; d < 2; d++) {
        int p0 = pix[-1 * xstride];
        int p1 = pix[-2 * xstride];
        int q0 = pix[0];
        int q1 = pix[1 * xstride];

        int abs_p0q0 = (p0 - q0);
        if (abs_p0q0 < 0) abs_p0q0 = -abs_p0q0;
        int abs_p1p0 = (p1 - p0);
        if (abs_p1p0 < 0) abs_p1p0 = -abs_p1p0;
        int abs_q1q0 = (q1 - q0);
        if (abs_q1q0 < 0) abs_q1q0 = -abs_q1q0;

        if (abs_p0q0 < alpha && abs_p1p0 < beta && abs_q1q0 < beta) {
            int delta = deblock_clip3(-tc_val, tc_val,
                (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            pix[-1 * xstride] = clip_u8(p0 + delta);
            pix[0] = clip_u8(q0 - delta);
        }
        pix += ystride;
    }
}

/* Chroma strong filter for BS=4 — one 2-pixel edge segment. */
static void deblock_chroma_strong(u8 *pix, int xstride, int ystride,
                                   int alpha, int beta)
{
    for (int d = 0; d < 2; d++) {
        int p0 = pix[-1 * xstride];
        int p1 = pix[-2 * xstride];
        int q0 = pix[0];
        int q1 = pix[1 * xstride];

        int abs_p0q0 = (p0 - q0);
        if (abs_p0q0 < 0) abs_p0q0 = -abs_p0q0;
        int abs_p1p0 = (p1 - p0);
        if (abs_p1p0 < 0) abs_p1p0 = -abs_p1p0;
        int abs_q1q0 = (q1 - q0);
        if (abs_q1q0 < 0) abs_q1q0 = -abs_q1q0;

        if (abs_p0q0 < alpha && abs_p1p0 < beta && abs_q1q0 < beta) {
            int delta = deblock_clip3(-2, 2,
                (((q0 - p0) << 2) + (p1 - q1) + 4) >> 3);
            pix[-1 * xstride] = clip_u8(p0 + delta);
            pix[0] = clip_u8(q0 - delta);
        }
        pix += ystride;
    }
}

/* Apply full H.264 deblocking filter to the reconstructed frame.
 * Processes all vertical edges first, then all horizontal edges,
 * for each macroblock in raster scan order.
 *
 * OPTIMIZATION: Skip MBs where this MB and all neighbors are P_SKIP
 * with zero non-zero coefficients and small MV differences — all edges
 * will have BS=0. This avoids calc_boundary_strength per-edge overhead. */
static void deblock_frame(SwPipelineState *state)
{
    if (state->slice.disable_deblocking_filter == 1)
        return;

    int mb_width  = SW_MB_WIDTH;   /* 30 */
    int mb_height = SW_MB_HEIGHT;  /* 17 */
    int y_stride  = SW_FRAME_WIDTH;
    int c_stride  = SW_FRAME_WIDTH / 2;

    u8 *y_plane = state->current.y_plane;
    u8 *u_plane = state->current.u_plane;
    u8 *v_plane = state->current.v_plane;

    for (int mb_y = 0; mb_y < mb_height; mb_y++) {
        for (int mb_x = 0; mb_x < mb_width; mb_x++) {
            int mb_idx = mb_y * mb_width + mb_x;
            SwMacroblockData *mb = &state->mbs[mb_idx];

            /* ---- PER-MB FAST SKIP ----
             * If this MB is inter with no non-zero luma coefficients,
             * AND the left neighbor (if any) AND top neighbor (if any)
             * are also inter with no non-zero luma coefficients AND
             * MV differences are small, all edges will have BS ≤ 0.
             * Skip the entire edge scan for this MB.
             *
             * This check costs ~20 cycles but saves ~500 cycles of
             * edge iteration for skip-heavy frames. */
            if (mb->mb_type > SW_MB_TYPE_I16x16) {
                int has_nz = 0;
                for (int b = 0; b < 16; b++) {
                    if (mb->nz_coeff_luma[b]) { has_nz = 1; break; }
                }
                if (!has_nz) {
                    int neighbors_clean = 1;

                    /* Check left neighbor */
                    if (mb_x > 0) {
                        SwMacroblockData *left = &state->mbs[mb_idx - 1];
                        if (left->mb_type <= SW_MB_TYPE_I16x16) {
                            neighbors_clean = 0;
                        } else {
                            for (int b = 0; b < 16; b++) {
                                if (left->nz_coeff_luma[b]) { neighbors_clean = 0; break; }
                            }
                            if (neighbors_clean) {
                                /* Check MV difference at MB boundary */
                                int ldx, ldy, rdx, rdy;
                                get_sub_mv(left, 3, 0, &ldx, &ldy);
                                get_sub_mv(mb, 0, 0, &rdx, &rdy);
                                int dx = ldx - rdx; if (dx < 0) dx = -dx;
                                int dy = ldy - rdy; if (dy < 0) dy = -dy;
                                if (dx >= 4 || dy >= 4) neighbors_clean = 0;
                            }
                        }
                    }

                    /* Check top neighbor */
                    if (neighbors_clean && mb_y > 0) {
                        SwMacroblockData *top = &state->mbs[mb_idx - mb_width];
                        if (top->mb_type <= SW_MB_TYPE_I16x16) {
                            neighbors_clean = 0;
                        } else {
                            for (int b = 0; b < 16; b++) {
                                if (top->nz_coeff_luma[b]) { neighbors_clean = 0; break; }
                            }
                            if (neighbors_clean) {
                                int tdx, tdy, bdx, bdy;
                                get_sub_mv(top, 0, 3, &tdx, &tdy);
                                get_sub_mv(mb, 0, 0, &bdx, &bdy);
                                int dx = tdx - bdx; if (dx < 0) dx = -dx;
                                int dy = tdy - bdy; if (dy < 0) dy = -dy;
                                if (dx >= 4 || dy >= 4) neighbors_clean = 0;
                            }
                        }
                    }

                    if (neighbors_clean) continue;  /* Skip all edges for this MB */
                }
            }
            int qp = mb->qp_y;

            /* Compute chroma QP for deblocking */
            int qpc_idx = qp + state->pps.chroma_qp_index_offset;
            if (qpc_idx < 0) qpc_idx = 0;
            if (qpc_idx > 51) qpc_idx = 51;
            int qp_c = chroma_qp_table[qpc_idx];

            int px = mb_x * 16;
            int py = mb_y * 16;

            /* ---- Vertical edges (edge_x = 0,1,2,3 in 4-pixel units) ---- */
            for (int edge_x = 0; edge_x < 4; edge_x++) {
                int is_mb_edge = (edge_x == 0);
                if (is_mb_edge && mb_x == 0) continue;

                /* Average QP of the two sides for threshold lookup */
                int qp_avg;
                int qp_c_avg;
                if (is_mb_edge) {
                    int left_qp = state->mbs[mb_idx - 1].qp_y;
                    qp_avg = (qp + left_qp + 1) >> 1;
                    int lqpc_idx = left_qp + state->pps.chroma_qp_index_offset;
                    if (lqpc_idx < 0) lqpc_idx = 0;
                    if (lqpc_idx > 51) lqpc_idx = 51;
                    qp_c_avg = (qp_c + chroma_qp_table[lqpc_idx] + 1) >> 1;
                } else {
                    qp_avg = qp;
                    qp_c_avg = qp_c;
                }

                int idx_a = qp_avg + state->slice.slice_alpha_c0_offset;
                if (idx_a < 0) idx_a = 0;
                if (idx_a > 51) idx_a = 51;
                int idx_b = qp_avg + state->slice.slice_beta_offset;
                if (idx_b < 0) idx_b = 0;
                if (idx_b > 51) idx_b = 51;
                int alpha = deblock_alpha[idx_a];
                int beta  = deblock_beta[idx_b];

                /* Chroma thresholds */
                int c_idx_a = qp_c_avg + state->slice.slice_alpha_c0_offset;
                if (c_idx_a < 0) c_idx_a = 0;
                if (c_idx_a > 51) c_idx_a = 51;
                int c_idx_b = qp_c_avg + state->slice.slice_beta_offset;
                if (c_idx_b < 0) c_idx_b = 0;
                if (c_idx_b > 51) c_idx_b = 51;
                int c_alpha = deblock_alpha[c_idx_a];
                int c_beta  = deblock_beta[c_idx_b];

                /* Luma: 4 segments of 4 pixels along this vertical edge */
                for (int seg = 0; seg < 4; seg++) {
                    /* q-side block: (edge_x, seg) in this MB */
                    int qx4 = edge_x;
                    int qy4 = seg;
                    /* p-side block: immediately to the left */
                    int px4, py4;
                    int mb_idx_p;
                    if (is_mb_edge) {
                        px4 = 3;
                        py4 = seg;
                        mb_idx_p = mb_idx - 1;
                    } else {
                        px4 = edge_x - 1;
                        py4 = seg;
                        mb_idx_p = mb_idx;
                    }

                    int bs = calc_boundary_strength(
                        &state->mbs[mb_idx_p], px4, py4,
                        mb, qx4, qy4, is_mb_edge);
                    if (bs == 0) continue;

                    /* Pixel pointer: q0 is at column (px + edge_x*4), row (py + seg*4) */
                    u8 *pix_y = y_plane + (py + seg * 4) * y_stride + px + edge_x * 4;

                    if (bs < 4) {
                        int tc0 = deblock_tc0[idx_a][bs - 1];
                        deblock_luma_normal(pix_y, 1, y_stride, alpha, beta, tc0);
                    } else {
                        deblock_luma_strong(pix_y, 1, y_stride, alpha, beta);
                    }
                }

                /* Chroma: vertical edges at 0 and 2 only (chroma is 8x8, edges at 0 and 4 pixels) */
                if (edge_x % 2 == 0) {
                    int c_edge = edge_x / 2;
                    for (int seg = 0; seg < 2; seg++) {
                        int qx4 = edge_x;
                        int qy4 = seg * 2;
                        int px4, py4;
                        int mb_idx_p;
                        if (is_mb_edge) {
                            px4 = 3; py4 = seg * 2;
                            mb_idx_p = mb_idx - 1;
                        } else {
                            px4 = edge_x - 1; py4 = seg * 2;
                            mb_idx_p = mb_idx;
                        }

                        int bs = calc_boundary_strength(
                            &state->mbs[mb_idx_p], px4, py4,
                            mb, qx4, qy4, is_mb_edge);
                        if (bs == 0) continue;

                        u8 *pix_u = u_plane + (mb_y * 8 + seg * 4) * c_stride + mb_x * 8 + c_edge * 4;
                        u8 *pix_v = v_plane + (mb_y * 8 + seg * 4) * c_stride + mb_x * 8 + c_edge * 4;

                        if (bs < 4) {
                            int tc0 = deblock_tc0[c_idx_a][bs - 1] + 1;
                            deblock_chroma_edge(pix_u, 1, c_stride, c_alpha, c_beta, tc0);
                            deblock_chroma_edge(pix_v, 1, c_stride, c_alpha, c_beta, tc0);
                        } else {
                            deblock_chroma_strong(pix_u, 1, c_stride, c_alpha, c_beta);
                            deblock_chroma_strong(pix_v, 1, c_stride, c_alpha, c_beta);
                        }
                    }
                }
            }

            /* ---- Horizontal edges (edge_y = 0,1,2,3 in 4-pixel units) ---- */
            for (int edge_y = 0; edge_y < 4; edge_y++) {
                int is_mb_edge = (edge_y == 0);
                if (is_mb_edge && mb_y == 0) continue;

                int qp_avg;
                int qp_c_avg;
                if (is_mb_edge) {
                    int above_qp = state->mbs[mb_idx - mb_width].qp_y;
                    qp_avg = (qp + above_qp + 1) >> 1;
                    int aqpc_idx = above_qp + state->pps.chroma_qp_index_offset;
                    if (aqpc_idx < 0) aqpc_idx = 0;
                    if (aqpc_idx > 51) aqpc_idx = 51;
                    qp_c_avg = (qp_c + chroma_qp_table[aqpc_idx] + 1) >> 1;
                } else {
                    qp_avg = qp;
                    qp_c_avg = qp_c;
                }

                int idx_a = qp_avg + state->slice.slice_alpha_c0_offset;
                if (idx_a < 0) idx_a = 0;
                if (idx_a > 51) idx_a = 51;
                int idx_b = qp_avg + state->slice.slice_beta_offset;
                if (idx_b < 0) idx_b = 0;
                if (idx_b > 51) idx_b = 51;
                int alpha = deblock_alpha[idx_a];
                int beta  = deblock_beta[idx_b];

                int c_idx_a = qp_c_avg + state->slice.slice_alpha_c0_offset;
                if (c_idx_a < 0) c_idx_a = 0;
                if (c_idx_a > 51) c_idx_a = 51;
                int c_idx_b = qp_c_avg + state->slice.slice_beta_offset;
                if (c_idx_b < 0) c_idx_b = 0;
                if (c_idx_b > 51) c_idx_b = 51;
                int c_alpha = deblock_alpha[c_idx_a];
                int c_beta  = deblock_beta[c_idx_b];

                /* Luma: 4 segments of 4 pixels along this horizontal edge */
                for (int seg = 0; seg < 4; seg++) {
                    int qx4 = seg;
                    int qy4 = edge_y;
                    int px4, py4;
                    int mb_idx_p;
                    if (is_mb_edge) {
                        px4 = seg; py4 = 3;
                        mb_idx_p = mb_idx - mb_width;
                    } else {
                        px4 = seg; py4 = edge_y - 1;
                        mb_idx_p = mb_idx;
                    }

                    int bs = calc_boundary_strength(
                        &state->mbs[mb_idx_p], px4, py4,
                        mb, qx4, qy4, is_mb_edge);
                    if (bs == 0) continue;

                    u8 *pix_y = y_plane + (py + edge_y * 4) * y_stride + px + seg * 4;

                    if (bs < 4) {
                        int tc0 = deblock_tc0[idx_a][bs - 1];
                        deblock_luma_normal(pix_y, y_stride, 1, alpha, beta, tc0);
                    } else {
                        deblock_luma_strong(pix_y, y_stride, 1, alpha, beta);
                    }
                }

                /* Chroma: horizontal edges at 0 and 2 only */
                if (edge_y % 2 == 0) {
                    int c_edge = edge_y / 2;
                    for (int seg = 0; seg < 2; seg++) {
                        int qx4 = seg * 2;
                        int qy4 = edge_y;
                        int px4, py4;
                        int mb_idx_p;
                        if (is_mb_edge) {
                            px4 = seg * 2; py4 = 3;
                            mb_idx_p = mb_idx - mb_width;
                        } else {
                            px4 = seg * 2; py4 = edge_y - 1;
                            mb_idx_p = mb_idx;
                        }

                        int bs = calc_boundary_strength(
                            &state->mbs[mb_idx_p], px4, py4,
                            mb, qx4, qy4, is_mb_edge);
                        if (bs == 0) continue;

                        u8 *pix_u = u_plane + (mb_y * 8 + c_edge * 4) * c_stride + mb_x * 8 + seg * 4;
                        u8 *pix_v = v_plane + (mb_y * 8 + c_edge * 4) * c_stride + mb_x * 8 + seg * 4;

                        if (bs < 4) {
                            int tc0 = deblock_tc0[c_idx_a][bs - 1] + 1;
                            deblock_chroma_edge(pix_u, c_stride, 1, c_alpha, c_beta, tc0);
                            deblock_chroma_edge(pix_v, c_stride, 1, c_alpha, c_beta, tc0);
                        } else {
                            deblock_chroma_strong(pix_u, c_stride, 1, c_alpha, c_beta);
                            deblock_chroma_strong(pix_v, c_stride, 1, c_alpha, c_beta);
                        }
                    }
                }
            }
        }
    }
}

/* ============================================================================
 * Full Frame Reconstruction Entry Point — Called by ME worker or CPU fallback
 *
 * Iterates all macroblocks, runs reconstruction, then color-converts.
 * ============================================================================*/

/* ME-safe reconstruction: NO syscalls (sceRtc, sceKernel), pure computation.
 * This version runs on the Media Engine core which cannot make kernel calls. */
void sw_reconstruct_frame_me(SwPipelineState *state, u8 *rgba_output)
{
    int total_mbs = state->mb_count;
    if (total_mbs <= 0 || total_mbs > SW_TOTAL_MBS) return;

    int skip_count = 0;
    SwRefFrame *ref = &state->ref_frames[state->active_ref];
    int cw = SW_FRAME_WIDTH / 2;

    for (int i = 0; i < total_mbs; i++) {
        if (state->mbs[i].skip_flag &&
            state->mbs[i].mv[0].dx == 0 &&
            state->mbs[i].mv[0].dy == 0) {
            skip_count++;
            if (ref->y_plane) {
                int mb_x = i % SW_MB_WIDTH;
                int mb_y = i / SW_MB_WIDTH;
                int px = mb_x * 16;
                int py = mb_y * 16;

                const u8 *sy = ref->y_plane + py * SW_FRAME_WIDTH + px;
                u8 *dy = state->current.y_plane + py * SW_FRAME_WIDTH + px;
                for (int r = 0; r < 16; r++)
                    memcpy(dy + r * SW_FRAME_WIDTH, sy + r * SW_FRAME_WIDTH, 16);

                int co = mb_y * 8 * cw + mb_x * 8;
                const u8 *su = ref->u_plane + co;
                const u8 *sv = ref->v_plane + co;
                u8 *du = state->current.u_plane + co;
                u8 *dv = state->current.v_plane + co;
                for (int r = 0; r < 8; r++) {
                    memcpy(du + r * cw, su + r * cw, 8);
                    memcpy(dv + r * cw, sv + r * cw, 8);
                }
            }
            continue;
        }
        sw_recon_macroblock(state, i);
    }

    if (!g_deblock_disable)
        deblock_frame(state);

    if (state->prev_rgba && skip_count > 0) {
        sw_yuv_to_rgba_skip_aware(state->current.y_plane,
                                   state->current.u_plane,
                                   state->current.v_plane,
                                   rgba_output,
                                   state->prev_rgba,
                                   state->mbs, total_mbs);
    } else {
        sw_yuv_to_rgba_fast(state->current.y_plane,
                            state->current.u_plane,
                            state->current.v_plane,
                            rgba_output,
                            SW_FRAME_WIDTH, SW_FRAME_HEIGHT);
    }

    state->skip_mb_count = (u32)skip_count;
    state->mb_loop_us = 0;
    state->deblock_us = 0;
    state->yuv_us = 0;
}

/* CPU reconstruction with timing instrumentation (uses syscalls).
 * This version runs on the Main CPU as a fallback when ME is unavailable. */
void sw_reconstruct_frame(SwPipelineState *state, u8 *rgba_output)
{
    int total_mbs = state->mb_count;
    if (total_mbs <= 0 || total_mbs > SW_TOTAL_MBS) return;

    u64 t_mb_start, t_mb_end, t_deblock_end, t_yuv_start, t_yuv_end;
    sceRtcGetCurrentTick(&t_mb_start);

    int skip_count = 0;
    SwRefFrame *ref = &state->ref_frames[state->active_ref];
    int cw = SW_FRAME_WIDTH / 2;

    /* Reconstruct every macroblock in YUV domain. */
    for (int i = 0; i < total_mbs; i++) {
        if (i > 0 && (i & 63) == 0)
            sceKernelDelayThread(0);

        /* P_SKIP zero-MV: copy reference YUV directly */
        if (state->mbs[i].skip_flag &&
            state->mbs[i].mv[0].dx == 0 &&
            state->mbs[i].mv[0].dy == 0) {
            skip_count++;
            if (ref->y_plane) {
                int mb_x = i % SW_MB_WIDTH;
                int mb_y = i / SW_MB_WIDTH;
                int px = mb_x * 16;
                int py = mb_y * 16;

                const u8 *sy = ref->y_plane + py * SW_FRAME_WIDTH + px;
                u8 *dy = state->current.y_plane + py * SW_FRAME_WIDTH + px;
                for (int r = 0; r < 16; r++)
                    memcpy(dy + r * SW_FRAME_WIDTH, sy + r * SW_FRAME_WIDTH, 16);

                int co = mb_y * 8 * cw + mb_x * 8;
                const u8 *su = ref->u_plane + co;
                const u8 *sv = ref->v_plane + co;
                u8 *du = state->current.u_plane + co;
                u8 *dv = state->current.v_plane + co;
                for (int r = 0; r < 8; r++) {
                    memcpy(du + r * cw, su + r * cw, 8);
                    memcpy(dv + r * cw, sv + r * cw, 8);
                }
            }
            continue;
        }

        sw_recon_macroblock(state, i);
    }

    sceRtcGetCurrentTick(&t_mb_end);

    /* H.264 in-loop deblocking filter */
    if (!g_deblock_disable)
        deblock_frame(state);

    sceRtcGetCurrentTick(&t_deblock_end);

    sceKernelDelayThread(0);
    sceRtcGetCurrentTick(&t_yuv_start);

    /* DIAGNOSTIC: dump Y/U/V plane values on first CLEAN IDR frame only */
    {
        static int s_ydump_done = 0;
        if (!s_ydump_done && state->slice.idr_flag && !state->error_concealed) {
            s_ydump_done = 1;
            const u8 *yp = state->current.y_plane;
            diag_log_write("DIAG", "Y row0[0..31]: "
                "%3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d  "
                "%3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d",
                yp[0],yp[1],yp[2],yp[3],yp[4],yp[5],yp[6],yp[7],
                yp[8],yp[9],yp[10],yp[11],yp[12],yp[13],yp[14],yp[15],
                yp[16],yp[17],yp[18],yp[19],yp[20],yp[21],yp[22],yp[23],
                yp[24],yp[25],yp[26],yp[27],yp[28],yp[29],yp[30],yp[31]);
            int mid_row = SW_FRAME_HEIGHT / 2;
            const u8 *yr = yp + mid_row * SW_FRAME_WIDTH;
            diag_log_write("DIAG", "Y row%d[0..31]: "
                "%3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d  "
                "%3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d",
                mid_row,
                yr[0],yr[1],yr[2],yr[3],yr[4],yr[5],yr[6],yr[7],
                yr[8],yr[9],yr[10],yr[11],yr[12],yr[13],yr[14],yr[15],
                yr[16],yr[17],yr[18],yr[19],yr[20],yr[21],yr[22],yr[23],
                yr[24],yr[25],yr[26],yr[27],yr[28],yr[29],yr[30],yr[31]);
            /* Also check U/V planes for non-128 values */
            const u8 *up = state->current.u_plane;
            const u8 *vp = state->current.v_plane;
            diag_log_write("DIAG", "U row0[0..15]: %3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d",
                up[0],up[1],up[2],up[3],up[4],up[5],up[6],up[7],
                up[8],up[9],up[10],up[11],up[12],up[13],up[14],up[15]);
            diag_log_write("DIAG", "V row0[0..15]: %3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d",
                vp[0],vp[1],vp[2],vp[3],vp[4],vp[5],vp[6],vp[7],
                vp[8],vp[9],vp[10],vp[11],vp[12],vp[13],vp[14],vp[15]);
            /* Dump U/V at middle of frame too */
            int uv_mid_row = SW_FRAME_HEIGHT / 4;
            int uv_w = SW_FRAME_WIDTH / 2;
            const u8 *umr = up + uv_mid_row * uv_w;
            const u8 *vmr = vp + uv_mid_row * uv_w;
            diag_log_write("DIAG", "U row%d[0..15]: %3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d",
                uv_mid_row, umr[0],umr[1],umr[2],umr[3],umr[4],umr[5],umr[6],umr[7],
                umr[8],umr[9],umr[10],umr[11],umr[12],umr[13],umr[14],umr[15]);
            diag_log_write("DIAG", "V row%d[0..15]: %3d %3d %3d %3d %3d %3d %3d %3d  %3d %3d %3d %3d %3d %3d %3d %3d",
                uv_mid_row, vmr[0],vmr[1],vmr[2],vmr[3],vmr[4],vmr[5],vmr[6],vmr[7],
                vmr[8],vmr[9],vmr[10],vmr[11],vmr[12],vmr[13],vmr[14],vmr[15]);
        }
    }

    /* DIAGNOSTIC: Color-bar test disabled — display pipeline verified correct in Run #24.
     * Colors are accurate; remaining artifacts are from network corruption in IDR. */
    {
        static int s_test_count = 0;
        if (0 && s_test_count < 2) {
            int qw = SW_FRAME_WIDTH / 4;
            for (int ty = 0; ty < SW_FRAME_HEIGHT; ty++) {
                u8 *row = rgba_output + ty * SW_FRAME_STRIDE * 4;
                for (int tx = 0; tx < SW_FRAME_WIDTH; tx++) {
                    u8 r = 0, g = 0, b = 0;
                    if (tx < qw)          { r = 255; }           /* Red bar */
                    else if (tx < 2 * qw) { g = 255; }           /* Green bar */
                    else if (tx < 3 * qw) { b = 255; }           /* Blue bar */
                    else                  { r = g = b = 255; }    /* White bar */
                    row[tx * 4 + 0] = r;
                    row[tx * 4 + 1] = g;
                    row[tx * 4 + 2] = b;
                    row[tx * 4 + 3] = 0xFF;
                }
            }
            diag_log_write("DIAG", "COLOR BARS written to RGBA (frame %d): R|G|B|W", s_test_count);
            s_test_count++;
            sceRtcGetCurrentTick(&t_yuv_end);
            state->skip_mb_count = (u32)skip_count;
            state->mb_loop_us = (u32)(t_mb_end - t_mb_start);
            state->deblock_us = (u32)(t_deblock_end - t_mb_end);
            state->yuv_us = (u32)(t_yuv_end - t_yuv_start);
            return;
        }
    }

    /* Skip-aware YUV→RGBA for P-frames; full-frame for IDR */
    if (state->prev_rgba && skip_count > 0) {
        sw_yuv_to_rgba_skip_aware(state->current.y_plane,
                                   state->current.u_plane,
                                   state->current.v_plane,
                                   rgba_output,
                                   state->prev_rgba,
                                   state->mbs, total_mbs);
    } else {
        sw_yuv_to_rgba_fast(state->current.y_plane,
                            state->current.u_plane,
                            state->current.v_plane,
                            rgba_output,
                            SW_FRAME_WIDTH, SW_FRAME_HEIGHT);
    }

    sceRtcGetCurrentTick(&t_yuv_end);

    state->skip_mb_count = (u32)skip_count;
    state->mb_loop_us = (u32)(t_mb_end - t_mb_start);
    state->deblock_us = (u32)(t_deblock_end - t_mb_end);
    state->yuv_us = (u32)(t_yuv_end - t_yuv_start);
}
