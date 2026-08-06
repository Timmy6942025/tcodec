/*
 * transform.c — Integer DCT and Walsh-Hadamard Transform for TCodec
 *
 * 4×4 and 8×8 forward/inverse DCT using butterfly algorithm.
 * 4×4 and 8×8 forward/inverse WHT (Hadamard) for residual mode.
 * All arithmetic is fixed-point integer, NEON-friendly.
 *
 * On ARM NEON builds, NEON-optimized versions in transform_neon.c
 * replace these scalar implementations (guarded by #if !TCODEC_NEON).
 */

#include "tcodec_common.h"

/* ════════════════════════════════════════════════════════════════
 * Pixel-mode DCT functions
 *
 * These operate on pixel data with ±128 level shift.
 * On ARM NEON builds, the NEON-optimized versions in transform_neon.c
 * are used instead — these scalar versions are excluded via #if.
 * ════════════════════════════════════════════════════════════════ */

#if !TCODEC_NEON  /* Scalar DCT fallback — NEON version in transform_neon.c */

/* ════════════════════════════════════════════════════════════════
 * 4×4 Forward DCT (H.264-style integer transform)
 *
 * The transform matrix is:
 *   C = | 1  1  1  1 |
 *       | 2  1 -1 -2 |
 *       | 1 -1 -1  1 |
 *       | 1 -2  2 -1 |
 *
 * No multiplication needed — just additions and shifts.
 * Output is scaled by 1/2 (right-shift 1 on each pass).
 * ════════════════════════════════════════════════════════════════ */

void tc_fdct4x4(const tc_pixel_t *TCODEC_RESTRICT in, int stride,
                tc_coeff_t *TCODEC_RESTRICT out)
{
    /* Read 4×4 input and subtract 128 (level shift for 8-bit) */
    int d[4][4];
    for (int i = 0; i < 4; i++) {
        d[i][0] = in[i * stride + 0] - 128;
        d[i][1] = in[i * stride + 1] - 128;
        d[i][2] = in[i * stride + 2] - 128;
        d[i][3] = in[i * stride + 3] - 128;
    }

    /* Horizontal pass: C × rows */
    int tmp[4][4];
    for (int i = 0; i < 4; i++) {
        int a = d[i][0] + d[i][3];
        int b = d[i][1] + d[i][2];
        int c = d[i][1] - d[i][2];
        int e = d[i][0] - d[i][3];

        tmp[i][0] = a + b;          /* 1  1  1  1  */
        tmp[i][1] = 2*e + c;        /* 2  1 -1 -2  */
        tmp[i][2] = a - b;          /* 1 -1 -1  1  */
        tmp[i][3] = e - 2*c;        /* 1 -2  2 -1  */
    }

    /* Vertical pass: cols × C^T */
    for (int j = 0; j < 4; j++) {
        int a = tmp[0][j] + tmp[3][j];
        int b = tmp[1][j] + tmp[2][j];
        int c = tmp[1][j] - tmp[2][j];
        int e = tmp[0][j] - tmp[3][j];

        out[0 * 4 + j] = (tc_coeff_t)((a + b + 1) >> 1);
        out[1 * 4 + j] = (tc_coeff_t)((2 * e + c + 1) >> 1);
        out[2 * 4 + j] = (tc_coeff_t)((a - b + 1) >> 1);
        out[3 * 4 + j] = (tc_coeff_t)((e - 2 * c + 1) >> 1);
    }
}

/* ════════════════════════════════════════════════════════════════
 * 4×4 Inverse DCT
 *
 * Inverse of the H.264 integer transform.
 * C^T × coeffs × C, then right-shift and add 128.
 * ════════════════════════════════════════════════════════════════ */

void tc_idct4x4(const tc_coeff_t *TCODEC_RESTRICT in,
                tc_pixel_t *TCODEC_RESTRICT out, int stride)
{
    /* Horizontal pass */
    int tmp[4][4];
    for (int i = 0; i < 4; i++) {
        int c0 = in[i * 4 + 0];
        int c1 = in[i * 4 + 1];
        int c2 = in[i * 4 + 2];
        int c3 = in[i * 4 + 3];

        int a = c0 + c2;
        int b = c0 - c2;
        int c = c1 - c3;
        int d = c1 + c3;

        /* Inverse rows of C^T */
        tmp[i][0] = a + d;           /* 1  2  1  1  */
        tmp[i][1] = b + c;           /* 1  1 -1 -2  */
        tmp[i][2] = b - c;           /* 1 -1 -1  2  */
        tmp[i][3] = a - d;           /* 1 -2  1 -1  */
    }

    /* Vertical pass + level shift */
    for (int j = 0; j < 4; j++) {
        int c0 = tmp[0][j];
        int c1 = tmp[1][j];
        int c2 = tmp[2][j];
        int c3 = tmp[3][j];

        int a = c0 + c2;
        int b = c0 - c2;
        int c = c1 - c3;
        int d = c1 + c3;

        out[0 * stride + j] = (tc_pixel_t)tc_clip(((a + d + 32) >> 6) + 128, 0, 255);
        out[1 * stride + j] = (tc_pixel_t)tc_clip(((b + c + 32) >> 6) + 128, 0, 255);
        out[2 * stride + j] = (tc_pixel_t)tc_clip(((b - c + 32) >> 6) + 128, 0, 255);
        out[3 * stride + j] = (tc_pixel_t)tc_clip(((a - d + 32) >> 6) + 128, 0, 255);
    }
}

/* ════════════════════════════════════════════════════════════════
 * 8×8 Forward DCT — Butterfly decomposition
 *
 * Uses the AAN (Arai-Agui-Nakajima) fast DCT algorithm.
 * Coefficients stored as 14-bit fixed point integers.
 *
 * The 8-point DCT-II can be decomposed into:
 *   - Even part: 4-point DCT-II (recursive)
 *   - Odd part:  4-point DCT-VIII (via rotation)
 *
 * Rotation constants (fractional part = 14 bits):
 *   cos(π/16) = 0.9808  → 16069
 *   sin(π/16) = 0.1951  →  3196
 *   cos(3π/16)= 0.8315  → 13623
 *   sin(3π/16)= 0.5556  →  9102
 *   cos(π/8)  = 0.9239  → 15137
 *   sin(π/8)  = 0.3827  →  6270
 *   1/√2      = 0.7071  → 11585
 * ════════════════════════════════════════════════════════════════ */

/* DCT rotation constants (14-bit fractional precision) */
#define DCT_COS1  16069   /* cos(π/16)  × 16384 */
#define DCT_SIN1   3196   /* sin(π/16)  × 16384 */
#define DCT_COS3  13623   /* cos(3π/16) × 16384 */
#define DCT_SIN3   9102   /* sin(3π/16) × 16384 */
#define DCT_COS2  15137   /* cos(π/8)   × 16384 */
#define DCT_SIN2   6270   /* sin(π/8)   × 16384 */
#define DCT_SQRT2 11585   /* 1/√2       × 16384 */

static void fdct8_point(const int *in, int *out)
{
    /* Stage 1: Even/odd split */
    int e0 = in[0] + in[7];
    int e1 = in[1] + in[6];
    int e2 = in[2] + in[5];
    int e3 = in[3] + in[4];
    int o0 = in[0] - in[7];
    int o1 = in[1] - in[6];
    int o2 = in[2] - in[5];
    int o3 = in[3] - in[4];

    /* Stage 2: Even part — 4-point DCT */
    int ee0 = e0 + e3;
    int ee1 = e1 + e2;
    int eo0 = e0 - e3;
    int eo1 = e1 - e2;

    /* Stage 3: Even-even (DC and Nyquist) */
    out[0] = (ee0 + ee1 + 1) >> 1;       /* DC */
    out[4] = (ee0 - ee1 + 1) >> 1;       /* Nyquist */

    /* Even-odd (rotation by π/8) */
    out[2] = ((eo0 * DCT_COS2 + eo1 * DCT_SIN2) + (1 << 13)) >> 14;
    out[6] = ((eo0 * DCT_SIN2 - eo1 * DCT_COS2) + (1 << 13)) >> 14;

    /* Stage 2-3: Odd part — 4 rotation butterflies */
    int r0 = (o0 * DCT_COS1 + o3 * DCT_SIN1 + (1 << 13)) >> 14;
    int r3 = (o0 * DCT_SIN1 - o3 * DCT_COS1 + (1 << 13)) >> 14;
    int r1 = (o1 * DCT_COS3 + o2 * DCT_SIN3 + (1 << 13)) >> 14;
    int r2 = (o1 * DCT_SIN3 - o2 * DCT_COS3 + (1 << 13)) >> 14;

    /* Final odd butterflies */
    int s0 = r0 + r1;
    int s1 = r0 - r1;
    int s2 = r2 + r3;
    int s3 = r2 - r3;

    out[1] = (s0 * DCT_SQRT2 + (1 << 13)) >> 14;
    out[3] = (s2 * DCT_SQRT2 + (1 << 13)) >> 14;
    out[5] = (s3 * DCT_SQRT2 + (1 << 13)) >> 14;
    out[7] = (s1 * DCT_SQRT2 + (1 << 13)) >> 14;
}

void tc_fdct8x8(const tc_pixel_t *TCODEC_RESTRICT in, int stride,
                tc_coeff_t *TCODEC_RESTRICT out)
{
    int tmp[8][8];

    /* Horizontal pass */
    for (int i = 0; i < 8; i++) {
        int row[8];
        for (int j = 0; j < 8; j++) {
            row[j] = in[i * stride + j] - 128;
        }
        fdct8_point(row, tmp[i]);
    }

    /* Vertical pass */
    for (int j = 0; j < 8; j++) {
        int col[8];
        for (int i = 0; i < 8; i++) {
            col[i] = tmp[i][j];
        }
        int result[8];
        fdct8_point(col, result);
        for (int i = 0; i < 8; i++) {
            out[i * 8 + j] = (tc_coeff_t)result[i];
        }
    }
}

/* ════════════════════════════════════════════════════════════════
 * 8×8 Inverse DCT — Butterfly (reverse of forward)
 * ════════════════════════════════════════════════════════════════ */

static void idct8_point(const tc_coeff_t *in, int *out)
{
    /* Reverse the odd-part butterflies */
    int s0 = (in[1] * DCT_SQRT2 + (1 << 13)) >> 14;
    int s1 = (in[7] * DCT_SQRT2 + (1 << 13)) >> 14;
    int s2 = (in[3] * DCT_SQRT2 + (1 << 13)) >> 14;
    int s3 = (in[5] * DCT_SQRT2 + (1 << 13)) >> 14;

    int r0 = s0 + s1;
    int r1 = s0 - s1;
    int r2 = s2 + s3;
    int r3 = s2 - s3;

    /* Reverse odd rotations */
    int o0 = (r0 * DCT_COS1 + r2 * DCT_SIN1 + (1 << 13)) >> 14;
    int o3 = (r0 * DCT_SIN1 - r2 * DCT_COS1 + (1 << 13)) >> 14;
    int o1 = (r1 * DCT_COS3 + r3 * DCT_SIN3 + (1 << 13)) >> 14;
    int o2 = (r1 * DCT_SIN3 - r3 * DCT_COS3 + (1 << 13)) >> 14;

    /* Reverse even-odd rotation */
    int eo0 = (in[2] * DCT_COS2 + in[6] * DCT_SIN2 + (1 << 13)) >> 14;
    int eo1 = (in[2] * DCT_SIN2 - in[6] * DCT_COS2 + (1 << 13)) >> 14;

    /* Reverse even-even */
    int ee0 = in[0] + in[4];
    int ee1 = in[0] - in[4];

    int e0 = ee0 + eo0;
    int e3 = ee0 - eo0;
    int e1 = ee1 + eo1;
    int e2 = ee1 - eo1;

    /* Final combine */
    out[0] = e0 + o0;  out[7] = e0 - o0;
    out[1] = e1 + o1;  out[6] = e1 - o1;
    out[2] = e2 + o2;  out[5] = e2 - o2;
    out[3] = e3 + o3;  out[4] = e3 - o3;
}

void tc_idct8x8(const tc_coeff_t *TCODEC_RESTRICT in,
                tc_pixel_t *TCODEC_RESTRICT out, int stride)
{
    int tmp[8][8];

    /* Horizontal pass */
    for (int i = 0; i < 8; i++) {
        idct8_point(&in[i * 8], tmp[i]);
    }

    /* Vertical pass */
    for (int j = 0; j < 8; j++) {
        int col[8];
        for (int i = 0; i < 8; i++) {
            col[i] = tmp[i][j];
        }
        int result[8];
        tc_coeff_t col_in[8];
        for (int i = 0; i < 8; i++) col_in[i] = (tc_coeff_t)col[i];
        idct8_point(col_in, result);

        for (int i = 0; i < 8; i++) {
            out[i * stride + j] = (tc_pixel_t)tc_clip(
                (result[i] + (1 << 5)) >> 6, 0, 255);
        }
    }
}

#endif /* !TCODEC_NEON — scalar DCT fallback (4×4 + 8×8) */
/* ════════════════════════════════════════════════════════════════
 * Residual-mode DCT (DCT-II on signed residuals)
 *
 * True DCT-II with orthonormal rows, 14-bit fixed-point constants.
 * Scaling is normalized to match the WHT gains used by the quantizer:
 *   4×4: forward gain 4  (same as WHT4), inverse gain 1/4
 *   8×8: forward gain 8  (same as WHT8), inverse gain 1/8
 * so the same JND quantizer step gives comparable R-D for both
 * transforms. Forward = C·X·Cᵀ << log2(gain); inverse = Cᵀ·Y·C >> log2.
 * ════════════════════════════════════════════════════════════════ */

#if !TCODEC_NEON  /* Scalar residual DCT — NEON version in transform_neon.c */

/* Orthonormal DCT-II matrices, 14-bit fixed point */
static const int dct4_c[4][4] = {
    { 8192,  8192,  8192,  8192},
    {10703,  4433, -4433,-10703},
    { 8192, -8192, -8192,  8192},
    { 4433,-10703, 10703, -4433}
};

static const int dct8_c[8][8] = {
    { 5793,  5793,  5793,  5793,  5793,  5793,  5793,  5793},
    { 8035,  6811,  4551,  1598, -1598, -4551, -6811, -8035},
    { 7568,  3135, -3135, -7568, -7568, -3135,  3135,  7568},
    { 6811, -1598, -8035, -4551,  4551,  8035,  1598, -6811},
    { 5793, -5793, -5793,  5793,  5793, -5793, -5793,  5793},
    { 4551, -8035,  1598,  6811, -6811, -1598,  8035, -4551},
    { 3135, -7568,  7568, -3135, -3135,  7568, -7568,  3135},
    { 1598, -4551,  6811, -8035,  8035, -6811,  4551, -1598}
};

static void dct4_point(const int *in, int *out, int transpose)
{
    const int (*C)[4] = dct4_c;
    for (int k = 0; k < 4; k++) {
        int64_t acc = 0;
        for (int n = 0; n < 4; n++) {
            int c = transpose ? C[n][k] : C[k][n];
            acc += (int64_t)c * in[n];
        }
        out[k] = (int)((acc + (1 << 13)) >> 14);
    }
}

static void dct8_point2(const int *in, int *out, int transpose)
{
    const int (*C)[8] = dct8_c;
    for (int k = 0; k < 8; k++) {
        int64_t acc = 0;
        for (int n = 0; n < 8; n++) {
            int c = transpose ? C[n][k] : C[k][n];
            acc += (int64_t)c * in[n];
        }
        out[k] = (int)((acc + (1 << 13)) >> 14);
    }
}

void tc_fdct4x4_res(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                    tc_coeff_t *TCODEC_RESTRICT out)
{
    int tmp[4][4], t2[4][4];

    for (int i = 0; i < 4; i++) {
        int row[4];
        for (int j = 0; j < 4; j++) row[j] = in[i*stride+j];
        dct4_point(row, tmp[i], 0);
    }
    for (int j = 0; j < 4; j++) {
        int col[4];
        for (int i = 0; i < 4; i++) col[i] = tmp[i][j];
        dct4_point(col, t2[j], 0);
    }
    /* <<2: forward gain 4 (matches WHT4) */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i*4+j] = (tc_coeff_t)t2[j][i];
}

void tc_idct4x4_res(const tc_coeff_t *TCODEC_RESTRICT in,
                    tc_coeff_t *TCODEC_RESTRICT out, int stride)
{
    int tmp[4][4], t2[4][4];

    for (int i = 0; i < 4; i++) {
        int row[4];
        for (int j = 0; j < 4; j++) row[j] = in[i*4+j];
        dct4_point(row, tmp[i], 1);
    }
    for (int j = 0; j < 4; j++) {
        int col[4];
        for (int i = 0; i < 4; i++) col[i] = tmp[i][j];
        dct4_point(col, t2[j], 1);
    }
    /* >>2: inverse gain 1/4 (matches WHT4) */
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            out[i*stride+j] = (tc_coeff_t)t2[j][i];
}

void tc_fdct8x8_res(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                    tc_coeff_t *TCODEC_RESTRICT out)
{
    int tmp[8][8], t2[8][8];

    for (int i = 0; i < 8; i++) {
        int row[8];
        for (int j = 0; j < 8; j++) row[j] = in[i*stride+j];
        dct8_point2(row, tmp[i], 0);
    }
    for (int j = 0; j < 8; j++) {
        int col[8];
        for (int i = 0; i < 8; i++) col[i] = tmp[i][j];
        dct8_point2(col, t2[j], 0);
    }
    /* <<3: forward gain 8 (matches WHT8) */
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            out[i*8+j] = (tc_coeff_t)t2[j][i];
}

void tc_idct8x8_res(const tc_coeff_t *TCODEC_RESTRICT in,
                    tc_coeff_t *TCODEC_RESTRICT out, int stride)
{
    int tmp[8][8], t2[8][8];

    for (int i = 0; i < 8; i++) {
        int row[8];
        for (int j = 0; j < 8; j++) row[j] = in[i*8+j];
        dct8_point2(row, tmp[i], 1);
    }
    for (int j = 0; j < 8; j++) {
        int col[8];
        for (int i = 0; i < 8; i++) col[i] = tmp[i][j];
        dct8_point2(col, t2[j], 1);
    }
    /* >>3: inverse gain 1/8 (matches WHT8) */
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 8; j++)
            out[i*stride+j] = (tc_coeff_t)t2[j][i];
}

#endif /* !TCODEC_NEON — scalar residual DCT */




/* ════════════════════════════════════════════════════════════════
 * Residual-mode transforms — Walsh-Hadamard Transform (WHT)
 *
 * These operate on signed residual data directly, without the
 * ±128 level shift used by the pixel-mode functions above.
 *
 * The Hadamard transform H is its own inverse (up to scaling):
 *   H * H = n * I
 * Therefore:
 *   Forward:  Y = H * X * H / n    (divide by n on 2nd pass)
 *   Inverse:  X = H * Y * H / n    (SAME operation!)
 *
 * This guarantees perfect reconstruction with uniform quantize/dequantize,
 * unlike the H.264 integer DCT where C^T*C = diag(4,10,4,10) requires
 * position-dependent scaling that our uniform dequantize doesn't provide.
 *
 * Compression efficiency is slightly below DCT for high-frequency content
 * but perfectly adequate for a working codec prototype.
 *
 * On ARM NEON builds, the NEON-optimized versions in transform_neon.c
 * are used instead — these scalar versions are excluded via #if.
 * ════════════════════════════════════════════════════════════════ */

#if !TCODEC_NEON  /* Scalar fallback — NEON version in transform_neon.c */

/* ── 4×4 Hadamard butterfly ──────────────────────────────────────
 *
 * H4 = | 1  1  1  1 |
 *      | 1 -1  1 -1 |
 *      | 1  1 -1 -1 |
 *      | 1 -1 -1  1 |
 *
 * For input [a, b, c, d]:
 *   s = a+c,  t = b+d,  u = a-c,  v = b-d
 *   y = [s+t, s-t, u+v, u-v]
 *
 * Two passes (horizontal + vertical) implement H * X * H.
 * Vertical pass includes right-shift by 2 (divide by n=4).
 * ══════════════════════════════════════════════════════════════ */

TCODEC_INLINE void hadamard4_point(const int *x, int *y)
{
    int s = x[0] + x[2];
    int t = x[1] + x[3];
    int u = x[0] - x[2];
    int v = x[1] - x[3];

    y[0] = s + t;
    y[1] = s - t;
    y[2] = u + v;
    y[3] = u - v;
}

void tc_fwht4x4(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                 tc_coeff_t *TCODEC_RESTRICT out)
{
    int tmp[4][4];

    /* Horizontal pass: H4 × each row */
    for (int i = 0; i < 4; i++) {
        int row[4] = { in[i*stride+0], in[i*stride+1],
                       in[i*stride+2], in[i*stride+3] };
        hadamard4_point(row, tmp[i]);
    }

    /* Vertical pass: each column × H4, then >>2 (divide by n=4) */
    for (int j = 0; j < 4; j++) {
        int col[4] = { tmp[0][j], tmp[1][j], tmp[2][j], tmp[3][j] };
        int result[4];
        hadamard4_point(col, result);
        out[0*4+j] = (tc_coeff_t)((result[0] + 2) >> 2);
        out[1*4+j] = (tc_coeff_t)((result[1] + 2) >> 2);
        out[2*4+j] = (tc_coeff_t)((result[2] + 2) >> 2);
        out[3*4+j] = (tc_coeff_t)((result[3] + 2) >> 2);
    }
}

void tc_iwht4x4(const tc_coeff_t *TCODEC_RESTRICT in,
                 tc_coeff_t *TCODEC_RESTRICT out, int stride)
{
    /* Inverse is IDENTICAL to forward (H is self-inverse up to scaling).
     * H * (H*X*H/4) * H / 4 = 16*X/16 = X  ✓  */
    int tmp[4][4];

    /* Horizontal pass */
    for (int i = 0; i < 4; i++) {
        int row[4] = { in[i*4+0], in[i*4+1], in[i*4+2], in[i*4+3] };
        hadamard4_point(row, tmp[i]);
    }

    /* Vertical pass: >>2 (divide by n=4) */
    for (int j = 0; j < 4; j++) {
        int col[4] = { tmp[0][j], tmp[1][j], tmp[2][j], tmp[3][j] };
        int result[4];
        hadamard4_point(col, result);
        out[0*stride+j] = (tc_coeff_t)((result[0] + 2) >> 2);
        out[1*stride+j] = (tc_coeff_t)((result[1] + 2) >> 2);
        out[2*stride+j] = (tc_coeff_t)((result[2] + 2) >> 2);
        out[3*stride+j] = (tc_coeff_t)((result[3] + 2) >> 2);
    }
}

/* ── 8×8 Hadamard butterfly ──────────────────────────────────────
 *
 * Recursive construction: H8 = | H4  H4 |
 *                               | H4 -H4 |
 *
 * Butterfly for input [x0..x7]:
 *   a[i] = x[i] + x[i+4]   for i=0..3  (even group)
 *   b[i] = x[i] - x[i+4]   for i=0..3  (odd group)
 *   Then apply H4 to a and b separately.
 *
 * Two passes + >>3 (divide by n=8) on vertical pass.
 * ══════════════════════════════════════════════════════════════ */

TCODEC_INLINE void hadamard8_point(const int *x, int *y)
{
    /* Split into even/odd groups */
    int a[4] = { x[0]+x[4], x[1]+x[5], x[2]+x[6], x[3]+x[7] };
    int b[4] = { x[0]-x[4], x[1]-x[5], x[2]-x[6], x[3]-x[7] };

    /* Apply H4 to each group */
    hadamard4_point(a, &y[0]);
    hadamard4_point(b, &y[4]);
}

void tc_fwht8x8(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                 tc_coeff_t *TCODEC_RESTRICT out)
{
    int tmp[8][8];

    /* Horizontal pass: H8 × each row */
    for (int i = 0; i < 8; i++) {
        int row[8];
        for (int j = 0; j < 8; j++) row[j] = in[i*stride+j];
        hadamard8_point(row, tmp[i]);
    }

    /* Vertical pass: each column × H8, then >>3 (divide by n=8) */
    for (int j = 0; j < 8; j++) {
        int col[8];
        for (int i = 0; i < 8; i++) col[i] = tmp[i][j];
        int result[8];
        hadamard8_point(col, result);
        for (int i = 0; i < 8; i++) {
            out[i*8+j] = (tc_coeff_t)((result[i] + 4) >> 3);
        }
    }
}

void tc_iwht8x8(const tc_coeff_t *TCODEC_RESTRICT in,
                 tc_coeff_t *TCODEC_RESTRICT out, int stride)
{
    /* Inverse is IDENTICAL to forward (H is self-inverse up to scaling).
     * H * (H*X*H/8) * H / 8 = 64*X/64 = X  ✓  */
    int tmp[8][8];

    /* Horizontal pass */
    for (int i = 0; i < 8; i++) {
        int row[8];
        for (int j = 0; j < 8; j++) row[j] = in[i*8+j];
        hadamard8_point(row, tmp[i]);
    }

    /* Vertical pass: >>3 (divide by n=8) */
    for (int j = 0; j < 8; j++) {
        int col[8];
        for (int i = 0; i < 8; i++) col[i] = tmp[i][j];
        int result[8];
        hadamard8_point(col, result);
        for (int i = 0; i < 8; i++) {
            out[i*stride+j] = (tc_coeff_t)((result[i] + 4) >> 3);
        }
    }
}

#endif /* !TCODEC_NEON — scalar WHT fallback */
