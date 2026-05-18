/*
 * fec.c -- forward error correction based on Vandermonde matrices
 *
 * (C) 1997-98 Luigi Rizzo (luigi@iet.unipi.it)
 * (C) 2001 Alain Knaff (alain@knaff.lu)
 * (C) 2017 Iwan Timmer (irtimmer@gmail.com)
 *
 * Portions derived from code by Phil Karn (karn@ka9q.ampr.org),
 * Robert Morelos-Zaragoza (robert@spectra.eng.hawaii.edu) and Hari
 * Thirumoorthy (harit@spectra.eng.hawaii.edu), Aug 1995
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above
 *    copyright notice, this list of conditions and the following
 *    disclaimer in the documentation and/or other materials
 *    provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
 * PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY,
 * OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA,
 * OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR
 * TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 */

/* #include <stdio.h> -- not available on PSP */
#include <stdlib.h>
#include <string.h>
#include <pspkernel.h>
#include <pspthreadman.h>

/* #include <assert.h> */
#include "rs.h"

#ifdef _MSC_VER
#define NEED_ALLOCA
#define alloca(x) _alloca(x)
#endif

typedef unsigned char gf;

#define GF_BITS  8
#define GF_PP "101110001"
#define GF_SIZE ((1 << GF_BITS) - 1)

#define SWAP(a,b,t) {t tmp; tmp=a; a=b; b=tmp;}

/*
 * USE_GF_MULC, GF_MULC0(c) and GF_ADDMULC(x) can be used when multiplying
 * many numbers by the same constant. In this case the first
 * call sets the constant, and others perform the multiplications.
 * A value related to the multiplication is held in a local variable
 * declared with USE_GF_MULC . See usage in addmul1().
 */
#define USE_GF_MULC register gf * __gf_mulc_
#define GF_MULC0(c) __gf_mulc_ = &gf_mul_table[(c)<<8]
#define GF_ADDMULC(dst, x) dst ^= __gf_mulc_[x]
#define GF_MULC(dst, x) dst = __gf_mulc_[x]

#define gf_mul(x,y) gf_mul_table[(x<<8)+y]

/*
 * To speed up computations, we have tables for logarithm, exponent
 * multiplication and inverse of a number.
 */
static gf gf_exp[2*GF_SIZE];
static int gf_log[GF_SIZE + 1];
static gf inverse[GF_SIZE+1];
#ifdef _MSC_VER
static gf __declspec(align (256)) gf_mul_table[(GF_SIZE + 1)*(GF_SIZE + 1)];
#else
static gf gf_mul_table[(GF_SIZE + 1)*(GF_SIZE + 1)] __attribute__((aligned (256)));
#endif

/* Static RS workspaces (PSP-1000 deterministic memory policy).
 * We maintain a single active reed_solomon context in fixed storage. */
static reed_solomon s_rs_ctx;
static gf s_rs_vm_buf[DATA_SHARDS_MAX * DATA_SHARDS_MAX] __attribute__((unused));
static gf s_rs_top_buf[DATA_SHARDS_MAX * DATA_SHARDS_MAX];
static gf s_rs_matrix_buf[DATA_SHARDS_MAX * DATA_SHARDS_MAX];
static gf s_rs_parity_buf[DATA_SHARDS_MAX * DATA_SHARDS_MAX];
static SceUID s_rs_sem = -1;

/*
 * modnn(x) computes x % GF_SIZE, where GF_SIZE is 2**GF_BITS - 1,
 * without a slow divide.
 */
static inline gf modnn(int x) {
    while (x >= GF_SIZE) {
        x -= GF_SIZE;
        x = (x >> GF_BITS) + (x & GF_SIZE);
    }
    return x;
}

static void addmul(gf *dst1, gf *src1, gf c, int sz) {
    USE_GF_MULC;
    if (c != 0) {
        register gf *dst = dst1, *src = src1;
        gf *lim = &dst[sz];

        GF_MULC0(c);
        for (; dst < lim; dst++, src++)
            GF_ADDMULC(*dst, *src);
    }
}

static void mul(gf *dst1, gf *src1, gf c, int sz) {
    USE_GF_MULC;
    if (c != 0) {
        register gf *dst = dst1, *src = src1;
        gf *lim = &dst[sz];
        GF_MULC0(c);
        for (; dst < lim; dst++, src++)
            GF_MULC(*dst , *src);
    } else
        memset(dst1, 0, sz);
}

/* y = a.dot(b) */
static __attribute__((unused)) gf* multiply1(gf *a, int ar, int ac, gf *b, int br, int bc) {
    gf *new_m, tg;
    int r, c, i, ptr = 0;

    /* assert(ac == br); */
    if (ac != br || ar <= 0 || bc <= 0 || ar > DATA_SHARDS_MAX || bc > DATA_SHARDS_MAX) {
        return NULL;
    }

    new_m = s_rs_matrix_buf;
    memset(new_m, 0, (size_t)(ar * bc));

    /* this multiply is slow */
    for (r = 0; r < ar; r++) {
        for (c = 0; c < bc; c++) {
            tg = 0;
            for (i = 0; i < ac; i++)
                tg ^= gf_mul(a[r*ac+i], b[i*bc+c]);

            new_m[ptr++] = tg;
        }
    }

    return new_m;
}

static void init_mul_table(void) {
    int i, j;
    for (i=0; i< GF_SIZE+1; i++)
    for (j=0; j< GF_SIZE+1; j++)
        gf_mul_table[(i<<8)+j] = gf_exp[modnn(gf_log[i] + gf_log[j]) ] ;

    for (j=0; j< GF_SIZE+1; j++)
        gf_mul_table[j] = gf_mul_table[j<<8] = 0;
}

/*
 * initialize the data structures used for computations in GF.
 */
static void generate_gf(void) {
    int i;
    gf mask;

    mask = 1;
    gf_exp[GF_BITS] = 0;
    /*
     * first, generate the (polynomial representation of) powers of \alpha,
     * which are stored in gf_exp[i] = \alpha ** i .
     * At the same time build gf_log[gf_exp[i]] = i .
     * The first GF_BITS powers are simply bits shifted to the left.
     */
    for (i = 0; i < GF_BITS; i++, mask <<= 1) {
        gf_exp[i] = mask;
        gf_log[gf_exp[i]] = i;
        /*
        * If GF_PP[i] == 1 then \alpha ** i occurs in poly-repr
        * gf_exp[GF_BITS] = \alpha ** GF_BITS
        */
        if (GF_PP[i] == '1')
            gf_exp[GF_BITS] ^= mask;
    }
    /*
     * now gf_exp[GF_BITS] = \alpha ** GF_BITS is complete, so can als
     * compute its inverse.
     */
    gf_log[gf_exp[GF_BITS]] = GF_BITS;
    /*
     * Poly-repr of \alpha ** (i+1) is given by poly-repr of
     * \alpha ** i shifted left one-bit and accounting for any
     * \alpha ** GF_BITS term that may occur when poly-repr of
     * \alpha ** i is shifted.
     */
    mask = 1 << (GF_BITS - 1) ;
    for (i = GF_BITS + 1; i < GF_SIZE; i++) {
        if (gf_exp[i - 1] >= mask)
            gf_exp[i] = gf_exp[GF_BITS] ^ ((gf_exp[i - 1] ^ mask) << 1);
        else
            gf_exp[i] = gf_exp[i - 1] << 1;

        gf_log[gf_exp[i]] = i;
    }
    /*
     * log(0) is not defined, so use a special value
     */
    gf_log[0] = GF_SIZE;
    /* set the extended gf_exp values for fast multiply */
    for (i = 0; i < GF_SIZE; i++)
        gf_exp[i + GF_SIZE] = gf_exp[i];

    /*
     * again special cases. 0 has no inverse. This used to
     * be initialized to GF_SIZE, but it should make no difference
     * since noone is supposed to read from here.
     */
    inverse[0] = 0;
    inverse[1] = 1;
    for (i=2; i<=GF_SIZE; i++)
        inverse[i] = gf_exp[GF_SIZE-gf_log[i]];
}

/*
 * invert_mat() takes a matrix and produces its inverse
 * k is the size of the matrix.
 * (Gauss-Jordan, adapted from Numerical Recipes in C)
 * Return non-zero if singular.
 */
static int __attribute__((unused)) invert_mat(gf *src, int k) {
    gf c, *p;
    int irow, icol, row, col, i, ix;

    int error = 1;
    static int indxc[DATA_SHARDS_MAX];
    static int indxr[DATA_SHARDS_MAX];
    static int ipiv[DATA_SHARDS_MAX];
    static gf id_row[DATA_SHARDS_MAX];

    memset(id_row, 0, k*sizeof(gf));
    /*
     * ipiv marks elements already used as pivots.
     */
    for (i = 0; i < k; i++)
        ipiv[i] = 0;

    for (col = 0; col < k; col++) {
        gf *pivot_row;
        /*
         * Zeroing column 'col', look for a non-zero element.
         * First try on the diagonal, if it fails, look elsewhere.
         */
        irow = icol = -1;
        if (ipiv[col] != 1 && src[col*k + col] != 0) {
            irow = col;
            icol = col;
            goto found_piv;
        }
        for (row = 0; row < k; row++) {
            if (ipiv[row] != 1) {
                for (ix = 0; ix < k; ix++) {
                    if (ipiv[ix] == 0) {
                        if (src[row*k + ix] != 0) {
                            irow = row;
                            icol = ix;
                            goto found_piv;
                        }
                    } else if (ipiv[ix] > 1) {
                        (void)0;
                        goto fail;
                    }
                }
            }
        }
        if (icol == -1) {
            (void)0;
            goto fail ;
        }

        found_piv:
        ++(ipiv[icol]);
        /*
         * swap rows irow and icol, so afterwards the diagonal
         * element will be correct. Rarely done, not worth
         * optimizing.
         */
        if (irow != icol) {
            for (ix = 0; ix < k; ix++) {
                SWAP(src[irow*k + ix], src[icol*k + ix], gf);
            }
        }
        indxr[col] = irow;
        indxc[col] = icol;
        pivot_row = &src[icol*k];
        c = pivot_row[icol];
        if (c == 0) {
            (void)0;
            goto fail;
        } else if (c != 1 ) {
            /*
             * this is done often , but optimizing is not so
             * fruitful, at least in the obvious ways (unrolling)
             */
            c = inverse[ c ];
            pivot_row[icol] = 1;
            for (ix = 0; ix < k; ix++)
                pivot_row[ix] = gf_mul(c, pivot_row[ix]);
        }
        /*
         * from all rows, remove multiples of the selected row
         * to zero the relevant entry (in fact, the entry is not zero
         * because we know it must be zero).
         * (Here, if we know that the pivot_row is the identity,
         * we can optimize the addmul).
         */
        id_row[icol] = 1;
        if (memcmp(pivot_row, id_row, k*sizeof(gf)) != 0) {
            for (p = src, ix = 0 ; ix < k ; ix++, p += k) {
                if (ix != icol) {
                    c = p[icol];
                    p[icol] = 0;
                    addmul(p, pivot_row, c, k);
                }
            }
        }
        id_row[icol] = 0;
    }
    for (col = k-1 ; col >= 0 ; col-- ) {
        if (indxr[col] <0 || indxr[col] >= k)
            (void)0;
        else if (indxc[col] <0 || indxc[col] >= k)
            (void)0;
        else
            if (indxr[col] != indxc[col] ) {
                for (row = 0 ; row < k ; row++ )
                    SWAP( src[row*k + indxr[col]], src[row*k + indxc[col]], gf);
            }
    }
    error = 0;

    fail:
    return error ;
}

/*
 * Not check for input params
 * */
static __attribute__((unused)) gf* sub_matrix(gf* matrix, int rmin, int cmin, int rmax, int cmax,  int nrows, int ncols) {
    int i, j, ptr = 0;
    int rows = rmax - rmin;
    int cols = cmax - cmin;
    gf* new_m;

    (void)nrows;

    if (rows <= 0 || cols <= 0 || rows > DATA_SHARDS_MAX || cols > DATA_SHARDS_MAX) {
        return NULL;
    }

    /* First extraction in reed_solomon_new() is "top", second is "parity". */
    new_m = (rmin == 0) ? s_rs_top_buf : s_rs_parity_buf;
    memset(new_m, 0, (size_t)(rows * cols));

    for (i = rmin; i < rmax; i++) {
        for (j = cmin; j < cmax; j++) {
            new_m[ptr++] = matrix[i*ncols + j];
        }
    }

    return new_m;
}

/* copy from golang rs version */
static inline int code_some_shards(gf* matrixRows, gf** inputs, gf** outputs, int dataShards, int outputCount, int byteCount) {
    gf* in;
    int iRow, c;
    for (c = 0; c < dataShards; c++) {
        in = inputs[c];
        for (iRow = 0; iRow < outputCount; iRow++) {
            if (0 == c)
                mul(outputs[iRow], in, matrixRows[iRow*dataShards+c], byteCount);
            else
                addmul(outputs[iRow], in, matrixRows[iRow*dataShards+c], byteCount);
        }
    }

    return 0;
}

void reed_solomon_init(void) {
    generate_gf();
    init_mul_table();
    if (s_rs_sem < 0) {
        s_rs_sem = sceKernelCreateSema("rs_global", 0, 1, 1, NULL);
    }
}

void reed_solomon_global_lock(void) {
    if (s_rs_sem < 0) {
        reed_solomon_init();
    }
    if (s_rs_sem >= 0) {
        sceKernelWaitSema(s_rs_sem, 1, NULL);
    }
}

void reed_solomon_global_unlock(void) {
    if (s_rs_sem >= 0) {
        sceKernelSignalSema(s_rs_sem, 1);
    }
}

reed_solomon* reed_solomon_new(int data_shards, int parity_shards) {
    reed_solomon* rs = &s_rs_ctx;
    int i, j;

    memset(rs, 0, sizeof(*rs));
    rs->data_shards = data_shards;
    rs->parity_shards = parity_shards;
    rs->shards = (data_shards + parity_shards);

    if (rs->shards > DATA_SHARDS_MAX || data_shards <= 0 || parity_shards <= 0) {
        return NULL;
    }

    memset(s_rs_matrix_buf, 0, sizeof(s_rs_matrix_buf));
    memset(s_rs_parity_buf, 0, sizeof(s_rs_parity_buf));
    rs->m = s_rs_matrix_buf;
    rs->parity = s_rs_parity_buf;

    /* Sunshine/moonlight-common-c nanors parity matrix. */
    for (j = 0; j < parity_shards; j++) {
        gf *row = rs->parity + j * data_shards;
        for (i = 0; i < data_shards; i++) {
            row[i] = inverse[(parity_shards + i) ^ j];
        }
    }

    return rs;
}

void reed_solomon_release(reed_solomon* rs) {
    if (NULL != rs) {
        rs->m = NULL;
        rs->parity = NULL;
    }
}

/**
 * decode one shard
 * input:
 * rs
 * original data_blocks[rs->data_shards][block_size]
 * dec_fec_blocks[nr_fec_blocks][block_size]
 * fec_block_nos: fec pos number in original fec_blocks
 * erased_blocks: erased blocks in original data_blocks
 * nr_fec_blocks: the number of erased blocks
 * */
static gf s_dataDecodeMatrix[DATA_SHARDS_MAX*DATA_SHARDS_MAX];
static unsigned char* s_subShards[DATA_SHARDS_MAX];
static gf s_nano_erasures[DATA_SHARDS_MAX];
static gf s_nano_colperm[DATA_SHARDS_MAX];
static gf s_nano_rowperm[DATA_SHARDS_MAX];
static gf s_nano_marks[DATA_SHARDS_MAX];

static int nanors_invert_mat(gf *src, gf *wrk, unsigned char **dst,
                             int V0, int K, int T, gf *c, gf *d)
{
    int V0b = V0;
    int W = K - V0;
    int i, j, x, row;
    gf u;

    for (i = 0; i < W; i++) {
        int dr = d[i] * K;
        for (j = 0; j < W; j++)
            wrk[i * W + j] = src[dr + c[V0 + j]];
    }

    for (; V0 < K; V0++) {
        int dr = d[V0 - V0b] * K;
        for (row = 0; row < V0b; row++) {
            u = src[dr + c[row]];
            addmul(dst[c[V0]], dst[c[row]], u, T);
        }
    }

    for (x = 0; x < W; x++) {
        u = wrk[x * W + x];
        if (u == 0)
            return -1;
        u = inverse[u];
        mul(wrk + x * W + x, wrk + x * W + x, u, W);
        mul(dst[c[V0b + x]], dst[c[V0b + x]], u, T);
        for (row = x + 1; row < W; row++) {
            u = wrk[row * W + x];
            addmul(wrk + row * W, wrk + x * W, u, W);
            addmul(dst[c[V0b + row]], dst[c[V0b + x]], u, T);
        }
    }

    for (x = W - 1; x >= 0; x--) {
        unsigned char *from = dst[c[V0b + x]];
        for (row = 0; row < x; row++) {
            u = wrk[row * W + x];
            addmul(dst[c[V0b + row]], from, u, T);
        }
    }

    return 0;
}

static int nanors_decode_shards(reed_solomon* rs, unsigned char **data,
                                unsigned char *marks, int nr_shards,
                                int block_size)
{
    int ds = rs->data_shards;
    int gaps = 0;
    int i, j;

    if (nr_shards < rs->shards)
        return -1;

    for (i = 0; i < ds; i++) {
        if (marks[i])
            s_nano_erasures[gaps++] = (gf)i;
    }
    if (gaps == 0)
        return 0;

    for (i = 0, j = 0; i < ds - gaps; i++, j++) {
        while (j < ds && marks[j])
            j++;
        if (j >= ds)
            return -1;
        s_nano_colperm[i] = (gf)j;
    }
    for (i = 0, j = ds - gaps; i < gaps; i++, j++)
        s_nano_colperm[j] = s_nano_erasures[i];

    for (i = 0, j = ds; i < gaps; i++, j++) {
        while (j < nr_shards && marks[j])
            j++;
        if (j >= nr_shards)
            return -1;
        s_nano_rowperm[i] = (gf)(j - ds);
        memcpy(data[s_nano_erasures[i]], data[j], block_size);
    }

    return nanors_invert_mat(rs->parity, s_dataDecodeMatrix, data,
                             ds - gaps, ds, block_size,
                             s_nano_colperm, s_nano_rowperm);
}

static int reed_solomon_decode(reed_solomon* rs, unsigned char **data_blocks, int block_size, unsigned char **dec_fec_blocks, unsigned int *fec_block_nos, unsigned int *erased_blocks, int nr_fec_blocks) {
    int i;
    int total = rs->shards;

    if (total > DATA_SHARDS_MAX)
        return -1;

    for (i = 0; i < total; i++) {
        s_subShards[i] = NULL;
        s_nano_marks[i] = 1;
    }

    for (i = 0; i < rs->data_shards; i++) {
        s_subShards[i] = data_blocks[i];
        s_nano_marks[i] = 0;
    }

    for (i = 0; i < nr_fec_blocks; i++) {
        if (erased_blocks[i] < (unsigned int)rs->data_shards)
            s_nano_marks[erased_blocks[i]] = 1;
        if (fec_block_nos[i] < (unsigned int)rs->parity_shards) {
            s_subShards[rs->data_shards + fec_block_nos[i]] = dec_fec_blocks[i];
            s_nano_marks[rs->data_shards + fec_block_nos[i]] = 0;
        }
    }

    return nanors_decode_shards(rs, s_subShards, s_nano_marks,
                                total, block_size);
}

/**
 * encode a big size of buffer
 * input:
 * rs
 * nr_shards: must satisfy (0 == nr_shards % rs->shards)
 * shards[nr_shards][block_size]
 * */
int reed_solomon_encode(reed_solomon* rs, unsigned char** shards, int nr_shards, int block_size) {
    unsigned char** data_blocks;
    unsigned char** fec_blocks;
    int i, ds = rs->data_shards, ps = rs->parity_shards, ss = rs->shards;
    i = nr_shards / ss;
    data_blocks = shards;
    fec_blocks = &shards[(i*ds)];

    for (i = 0; i < nr_shards; i += ss) {
        code_some_shards(rs->parity, data_blocks, fec_blocks, rs->data_shards, rs->parity_shards, block_size);
        data_blocks += ds;
        fec_blocks += ps;
    }
    return 0;
}

/**
 * reconstruct a big size of buffer
 * input:
 * rs
 * nr_shards: must satisfy (0 == nr_shards % rs->data_shards)
 * shards[nr_shards][block_size]
 * marks[nr_shards] marks as errors
 * */
int reed_solomon_reconstruct(reed_solomon* rs, unsigned char** shards, unsigned char* marks, int nr_shards, int block_size) {
    static unsigned char *dec_fec_blocks[DATA_SHARDS_MAX];
    static unsigned int fec_block_nos[DATA_SHARDS_MAX];
    static unsigned int erased_blocks[DATA_SHARDS_MAX];
    unsigned char* fec_marks;
    unsigned char **data_blocks, **fec_blocks;
    int i, j, dn, pn, n;
    int ds = rs->data_shards;
    int ps = rs->parity_shards;
    int err = 0;

    data_blocks = shards;
    n = nr_shards / rs->shards;
    fec_marks = marks + n*ds; //after all data, is't fec marks
    fec_blocks = shards + n*ds;

    for (j = 0; j < n; j++) {
        dn = 0;
        for (i = 0; i < ds; i++) {
            if (marks[i])
                erased_blocks[dn++] = i;
        }
        if (dn > 0) {
            pn = 0;
            for (i = 0; i < ps && pn < dn; i++) {
                if (!fec_marks[i]) {
                    //got valid fec row
                    fec_block_nos[pn] = i;
                    dec_fec_blocks[pn] = fec_blocks[i];
                    pn++;
                }
            }

            if (dn == pn) {
                int ret = reed_solomon_decode(rs, data_blocks, block_size, dec_fec_blocks, fec_block_nos, erased_blocks, dn);
                if (ret != 0)
                    err = ret;
            } else
                err = -1;
        }
        data_blocks += ds;
        marks += ds;
        fec_blocks += ps;
        fec_marks += ps;
    }

    return err;
}
