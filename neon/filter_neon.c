/*
 * filter_neon.c — Deblocking filter for TCodec (NEON build)
 *
 * PARITY NOTE: NEON builds must produce bit-identical output to the
 * scalar build (D1/D5 requirement: "NEON must be bit-identical to
 * scalar"). The previous NEON deblock used weaker 8-boundary-only
 * filtering and produced different recon than the scalar path,
 * breaking encode bitstream parity across builds.
 *
 * This file now implements the EXACT scalar filter algorithm from
 * filter.c (guarded out of NEON builds), so encoder decisions and
 * decoded output are bit-identical regardless of TCODEC_NEON.
 *
 * TODO(D9): a true NEON vectorized deblock can be added later, but
 * it must produce byte-identical results, verified by
 * tools/parity_check.sh.
 */

#include "tcodec_common.h"

#if TCODEC_NEON

/* ════════════════════════════════════════════════════════════════
 * Bit-exact mirror of src/filter.c (scalar) — see that file for the
 * algorithm documentation.
 * ════════════════════════════════════════════════════════════════ */

static int n_edge_strength(int p0, int p1, int q0, int q1, int qp)
{
    int diff = tc_abs(p0 - q0);
    int threshold1 = tc_clip(qp / 4, 2, 16);
    int threshold2 = tc_clip(qp / 2, 4, 32);
    int threshold3 = tc_clip(qp,     8, 48);

    if (diff < threshold1) return 0;
    if (diff < threshold2) return 1;
    if (diff < threshold3) return 2;
    return 3;
}

static tc_pixel_t n_weak_filter(int p1, int p0, int q0, int q1, int tc)
{
    int delta = ((-p1 + 4 * p0 + 4 * q0 - q1 + 4) >> 3);
    delta = tc_clip(delta, -tc, tc);
    return (tc_pixel_t)tc_clip(p0 + delta, 0, 255);
}

static tc_pixel_t n_strong_filter_p(int p3, int p2, int p1, int p0, int q0)
{
    int val = (p3 - 5 * p2 + 20 * p1 + 20 * p0 - 5 * q0 + 16) >> 5;
    return (tc_pixel_t)tc_clip(val, 0, 255);
}

static tc_pixel_t n_strong_filter_q(int p0, int q0, int q1, int q2, int q3)
{
    int val = (p0 - 5 * q0 + 20 * q1 + 20 * q2 - 5 * q3 + 16) >> 5;
    return (tc_pixel_t)tc_clip(val, 0, 255);
}

static void n_filter_vert_edge(tc_pixel_t *y, int stride,
                               int x, int y_start, int height, int qp)
{
    for (int row = 0; row < height; row++) {
        int py = y_start + row;
        int p3 = y[py * stride + (x - 4)];
        int p2 = y[py * stride + (x - 3)];
        int p1 = y[py * stride + (x - 2)];
        int p0 = y[py * stride + (x - 1)];
        int q0 = y[py * stride + (x + 0)];
        int q1 = y[py * stride + (x + 1)];
        int q2 = y[py * stride + (x + 2)];
        int q3 = y[py * stride + (x + 3)];

        int strength = n_edge_strength(p0, p1, q0, q1, qp);
        if (strength == 0) continue;

        int tc = tc_clip(qp / 3, 1, 10);

        if (strength >= 3) {
            int cond_p = tc_abs(p2 - p0) < tc && tc_abs(p3 - p0) < tc;
            int cond_q = tc_abs(q2 - q0) < tc && tc_abs(q3 - q0) < tc;
            if (cond_p && cond_q) {
                y[py * stride + (x - 2)] = n_strong_filter_p(p3, p2, p1, p0, q0);
                y[py * stride + (x - 1)] = n_strong_filter_p(p2, p1, p0, q0, q1);
                y[py * stride + (x + 0)] = n_strong_filter_q(p0, q0, q1, q2, q3);
                y[py * stride + (x + 1)] = n_strong_filter_q(p1, q0, q1, q2, q3);
            } else {
                y[py * stride + (x - 1)] = n_weak_filter(p1, p0, q0, q1, tc);
                y[py * stride + (x + 0)] = n_weak_filter(q1, q0, p0, p1, tc);
            }
        } else {
            y[py * stride + (x - 1)] = n_weak_filter(p1, p0, q0, q1, tc);
            y[py * stride + (x + 0)] = n_weak_filter(q1, q0, p0, p1, tc);
        }
    }
}

static void n_filter_horiz_edge(tc_pixel_t *y, int stride,
                                int x_start, int row, int width, int qp)
{
    for (int col = 0; col < width; col++) {
        int px = x_start + col;
        int p3 = y[(row - 4) * stride + px];
        int p2 = y[(row - 3) * stride + px];
        int p1 = y[(row - 2) * stride + px];
        int p0 = y[(row - 1) * stride + px];
        int q0 = y[(row + 0) * stride + px];
        int q1 = y[(row + 1) * stride + px];
        int q2 = y[(row + 2) * stride + px];
        int q3 = y[(row + 3) * stride + px];

        int strength = n_edge_strength(p0, p1, q0, q1, qp);
        if (strength == 0) continue;

        int tc = tc_clip(qp / 3, 1, 10);

        if (strength >= 3) {
            int cond_p = tc_abs(p2 - p0) < tc && tc_abs(p3 - p0) < tc;
            int cond_q = tc_abs(q2 - q0) < tc && tc_abs(q3 - q0) < tc;
            if (cond_p && cond_q) {
                y[(row - 2) * stride + px] = n_strong_filter_p(p3, p2, p1, p0, q0);
                y[(row - 1) * stride + px] = n_strong_filter_p(p2, p1, p0, q0, q1);
                y[(row + 0) * stride + px] = n_strong_filter_q(p0, q0, q1, q2, q3);
                y[(row + 1) * stride + px] = n_strong_filter_q(p1, q0, q1, q2, q3);
            } else {
                y[(row - 1) * stride + px] = n_weak_filter(p1, p0, q0, q1, tc);
                y[(row + 0) * stride + px] = n_weak_filter(q1, q0, p0, p1, tc);
            }
        } else {
            y[(row - 1) * stride + px] = n_weak_filter(p1, p0, q0, q1, tc);
            y[(row + 0) * stride + px] = n_weak_filter(q1, q0, p0, p1, tc);
        }
    }
}

void tc_deblock_ctu(tc_pixel_t *y,  int stride_y,
                    tc_pixel_t *cb, int stride_cb,
                    tc_pixel_t *cr, int stride_cr,
                    int ctu_x, int ctu_y, int qp)
{
    /* Luma: same as scalar */
    for (int edge = 1; edge < TC_CTU_SIZE / 4; edge++) {
        int x = ctu_x + edge * 4;
        n_filter_vert_edge(y, stride_y, x, ctu_y, TC_CTU_SIZE, qp);
    }
    for (int edge = 1; edge < TC_CTU_SIZE / 4; edge++) {
        int row = ctu_y + edge * 4;
        n_filter_horiz_edge(y, stride_y, ctu_x, row, TC_CTU_SIZE, qp);
    }

    /* Chroma deblocking (same as scalar) */
    int chroma_qp = tc_clip(qp - 1, 0, 63);
    int c_tc = tc_clip(chroma_qp / 4, 1, 6);

    for (int edge = 1; edge < TC_CTU_SIZE / 8; edge++) {
        int cx = (ctu_x / 2) + edge * 4;
        int cy = ctu_y / 2;
        for (int row = 0; row < TC_CTU_SIZE / 2; row++) {
            int p0 = cb[(cy + row) * stride_cb + (cx - 1)];
            int q0 = cb[(cy + row) * stride_cb + cx];
            if (n_edge_strength(p0, p0, q0, q0, chroma_qp) > 0) {
                int delta = (q0 - p0 + 1) >> 1;
                delta = tc_clip(delta, -c_tc, c_tc);
                cb[(cy + row) * stride_cb + (cx - 1)] = (tc_pixel_t)tc_clip(p0 + delta, 0, 255);
                cb[(cy + row) * stride_cb + cx]       = (tc_pixel_t)tc_clip(q0 - delta, 0, 255);
            }
        }
    }

    for (int edge = 1; edge < TC_CTU_SIZE / 8; edge++) {
        int cx = ctu_x / 2;
        int cy = (ctu_y / 2) + edge * 4;
        for (int col = 0; col < TC_CTU_SIZE / 2; col++) {
            int p0 = cr[cy * stride_cr + (cx + col)];
            int q0 = cr[(cy + 1) * stride_cr + (cx + col)];
            int p0_cb = cb[cy * stride_cb + (cx + col)];
            int q0_cb = cb[(cy + 1) * stride_cb + (cx + col)];
            if (n_edge_strength(p0, p0, q0, q0, chroma_qp) > 0) {
                int delta = (q0 - p0 + 1) >> 1;
                delta = tc_clip(delta, -c_tc, c_tc);
                cr[cy * stride_cr + (cx + col)]        = (tc_pixel_t)tc_clip(p0 + delta, 0, 255);
                cr[(cy + 1) * stride_cr + (cx + col)]  = (tc_pixel_t)tc_clip(q0 - delta, 0, 255);
                delta = (q0_cb - p0_cb + 1) >> 1;
                delta = tc_clip(delta, -c_tc, c_tc);
                cb[cy * stride_cb + (cx + col)]       = (tc_pixel_t)tc_clip(p0_cb + delta, 0, 255);
                cb[(cy + 1) * stride_cb + (cx + col)]  = (tc_pixel_t)tc_clip(q0_cb - delta, 0, 255);
            }
        }
    }
}

#endif /* TCODEC_NEON */
