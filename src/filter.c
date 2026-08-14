/*
 * filter.c — Deblocking filter for TCodec
 *
 * Simple but effective deblocking filter applied after reconstruction.
 * Inspired by HEVC's deblocking, but simplified for ARM performance:
 *
 *  - Only filter block boundaries (4×4 or 8×8 edges)
 *  - Edge strength (0-3) determined by:
 *    0: No filter (flat on both sides)
 *    1: Weak filter (small discontinuity)
 *    2: Medium filter
 *    3: Strong filter (large discontinuity, likely visible)
 *
 *  - For strength 1-2: 4-tap filter on boundary pixels
 *  - For strength 3:   6-tap strong filter (HEVC-style)
 *
 *  - Decision: strength based on QP and pixel difference at boundary
 */

#include "tcodec_common.h"

/* ── Edge strength decision ──────────────────────────────────── */

static int edge_strength(int p0, int p1, int q0, int q1, int qp)
{
    int diff = tc_abs(p0 - q0);
    int threshold1 = tc_clip(qp / 4, 2, 16);   /* Small threshold */
    int threshold2 = tc_clip(qp / 2, 4, 32);   /* Medium threshold */
    int threshold3 = tc_clip(qp,     8, 48);    /* Strong threshold */

    if (diff < threshold1) return 0;   /* No filtering needed */
    if (diff < threshold2) return 1;   /* Weak */
    if (diff < threshold3) return 2;   /* Medium */
    return 3;                           /* Strong */
}

/* ── Weak filter (4-tap) ───────────────────────────────────────
 *
 * Modify boundary pixels p0 and q0:
 *   p0' = p0 + tc_clip((-p1 + 4*p0 + 4*q0 - q1 + 4) >> 3, -tc, tc)
 *   q0' = q0 + tc_clip((-q1 + 4*q0 + 4*p0 - p1 + 4) >> 3, -tc, tc)
 *
 * tc = clipping threshold based on QP.
 * ══════════════════════════════════════════════════════════════ */

TCODEC_INLINE tc_pixel_t weak_filter(int p1, int p0, int q0, int q1, int tc)
{
    int delta = ((-p1 + 4 * p0 + 4 * q0 - q1 + 4) >> 3);
    delta = tc_clip(delta, -tc, tc);
    return (tc_pixel_t)tc_clip(p0 + delta, 0, 255);
}

/* ── Strong filter (6-tap, HEVC-style) ─────────────────────────
 *
 * For strong filtering, modify 3 pixels on each side:
 *   p2', p1', p0' on the left, q0', q1', q2' on the right.
 *
 * Using 6-tap filter: [1, -5, 20, 20, -5, 1] / 32
 * ══════════════════════════════════════════════════════════════ */

TCODEC_INLINE tc_pixel_t strong_filter_p(
    int p3, int p2, int p1, int p0, int q0)
{
    int val = (p3 - 5 * p2 + 20 * p1 + 20 * p0 - 5 * q0 + 16) >> 5;
    return (tc_pixel_t)tc_clip(val, 0, 255);
}

TCODEC_INLINE tc_pixel_t strong_filter_q(
    int p0, int q0, int q1, int q2, int q3)
{
    int val = (p0 - 5 * q0 + 20 * q1 + 20 * q2 - 5 * q3 + 16) >> 5;
    return (tc_pixel_t)tc_clip(val, 0, 255);
}

/* ── Filter a vertical edge between two 4×4 blocks ──────────── */

static void filter_vert_edge(tc_pixel_t *y, int stride,
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

        int strength = edge_strength(p0, p1, q0, q1, qp);
        if (strength == 0) continue;

        int tc = tc_clip(qp / 3, 1, 10);

        if (strength >= 3) {
            /* Strong filter: modify 3 pixels on each side */
            int cond_p = tc_abs(p2 - p0) < tc && tc_abs(p3 - p0) < tc;
            int cond_q = tc_abs(q2 - q0) < tc && tc_abs(q3 - q0) < tc;
            if (cond_p && cond_q) {
                y[py * stride + (x - 2)] = strong_filter_p(p3, p2, p1, p0, q0);
                y[py * stride + (x - 1)] = strong_filter_p(p2, p1, p0, q0, q1);
                y[py * stride + (x + 0)] = strong_filter_q(p0, q0, q1, q2, q3);
                y[py * stride + (x + 1)] = strong_filter_q(p1, q0, q1, q2, q3);
            } else {
                /* Fall back to weak */
                y[py * stride + (x - 1)] = weak_filter(p1, p0, q0, q1, tc);
                y[py * stride + (x + 0)] = weak_filter(q1, q0, p0, p1, tc);
            }
        } else {
            /* Weak filter: modify 1 pixel on each side */
            y[py * stride + (x - 1)] = weak_filter(p1, p0, q0, q1, tc);
            y[py * stride + (x + 0)] = weak_filter(q1, q0, p0, p1, tc);
        }
    }
}

/* ── Filter a horizontal edge between two 4×4 blocks ────────── */

static void filter_horiz_edge(tc_pixel_t *y, int stride,
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

        int strength = edge_strength(p0, p1, q0, q1, qp);
        if (strength == 0) continue;

        int tc = tc_clip(qp / 3, 1, 10);

        if (strength >= 3) {
            int cond_p = tc_abs(p2 - p0) < tc && tc_abs(p3 - p0) < tc;
            int cond_q = tc_abs(q2 - q0) < tc && tc_abs(q3 - q0) < tc;
            if (cond_p && cond_q) {
                y[(row - 2) * stride + px] = strong_filter_p(p3, p2, p1, p0, q0);
                y[(row - 1) * stride + px] = strong_filter_p(p2, p1, p0, q0, q1);
                y[(row + 0) * stride + px] = strong_filter_q(p0, q0, q1, q2, q3);
                y[(row + 1) * stride + px] = strong_filter_q(p1, q0, q1, q2, q3);
            } else {
                y[(row - 1) * stride + px] = weak_filter(p1, p0, q0, q1, tc);
                y[(row + 0) * stride + px] = weak_filter(q1, q0, p0, p1, tc);
            }
        } else {
            y[(row - 1) * stride + px] = weak_filter(p1, p0, q0, q1, tc);
            y[(row + 0) * stride + px] = weak_filter(q1, q0, p0, p1, tc);
        }
    }
}

/* ── Filter one CTU ──────────────────────────────────────────── */

#if TCODEC_NEON  /* NEON version in filter_neon.c replaces this.
                    Its all-weak luma edge kernels use widening loads and
                    saturating narrowing; mixed/strong decisions stay on
                    the exact scalar arithmetic path. */
extern void tc_deblock_ctu(tc_pixel_t *y,  int stride_y,
                    tc_pixel_t *cb, int stride_cb,
                    tc_pixel_t *cr, int stride_cr,
                    int ctu_x, int ctu_y, int qp);
#else
void tc_deblock_ctu(tc_pixel_t *y,  int stride_y,
                    tc_pixel_t *cb, int stride_cb,
                    tc_pixel_t *cr, int stride_cr,
                    int ctu_x, int ctu_y, int qp)
{
    /* Filter vertical edges within the CTU */
    for (int edge = 1; edge < TC_CTU_SIZE / 4; edge++) {
        int x = ctu_x + edge * 4;
        filter_vert_edge(y, stride_y, x, ctu_y, TC_CTU_SIZE, qp);
    }

    /* Filter horizontal edges within the CTU */
    for (int edge = 1; edge < TC_CTU_SIZE / 4; edge++) {
        int row = ctu_y + edge * 4;
        filter_horiz_edge(y, stride_y, ctu_x, row, TC_CTU_SIZE, qp);
    }

    /* Chroma deblocking (8×8 boundaries only) */
    int chroma_qp = tc_clip(qp - 1, 0, 63);

    /* Vertical chroma edges */
    for (int edge = 1; edge < TC_CTU_SIZE / 8; edge++) {
        int cx = (ctu_x / 2) + edge * 4;
        int cy = ctu_y / 2;
        for (int row = 0; row < TC_CTU_SIZE / 2; row++) {
            int p0 = cb[(cy + row) * stride_cb + (cx - 1)];
            int q0 = cb[(cy + row) * stride_cb + cx];
            int strength = edge_strength(p0, p0, q0, q0, chroma_qp);
            if (strength > 0) {
                int tc_val = tc_clip(chroma_qp / 4, 1, 6);
                int delta = (q0 - p0 + 1) >> 1;
                delta = tc_clip(delta, -tc_val, tc_val);
                cb[(cy + row) * stride_cb + (cx - 1)] = (tc_pixel_t)tc_clip(p0 + delta, 0, 255);
                cb[(cy + row) * stride_cb + cx]      = (tc_pixel_t)tc_clip(q0 - delta, 0, 255);
            }
        }
    }

    /* Horizontal chroma edges */
    for (int edge = 1; edge < TC_CTU_SIZE / 8; edge++) {
        int cx = ctu_x / 2;
        int cy = (ctu_y / 2) + edge * 4;
        for (int col = 0; col < TC_CTU_SIZE / 2; col++) {
            int p0 = cr[cy * stride_cr + (cx + col)];
            int q0 = cr[(cy + 1) * stride_cr + (cx + col)];
            /* Similar for cb */
            int p0_cb = cb[cy * stride_cb + (cx + col)];
            int q0_cb = cb[(cy + 1) * stride_cb + (cx + col)];
            int strength = edge_strength(p0, p0, q0, q0, chroma_qp);
            if (strength > 0) {
                int tc_val = tc_clip(chroma_qp / 4, 1, 6);
                int delta = (q0 - p0 + 1) >> 1;
                delta = tc_clip(delta, -tc_val, tc_val);
                cr[cy * stride_cr + (cx + col)]       = (tc_pixel_t)tc_clip(p0 + delta, 0, 255);
                cr[(cy + 1) * stride_cr + (cx + col)]  = (tc_pixel_t)tc_clip(q0 - delta, 0, 255);
                delta = (q0_cb - p0_cb + 1) >> 1;
                delta = tc_clip(delta, -tc_val, tc_val);
                cb[cy * stride_cb + (cx + col)]       = (tc_pixel_t)tc_clip(p0_cb + delta, 0, 255);
                cb[(cy + 1) * stride_cb + (cx + col)]  = (tc_pixel_t)tc_clip(q0_cb - delta, 0, 255);
            }
        }
    }
}
#endif /* TCODEC_NEON — scalar deblock fallback */

/* ── v2 SAO: deterministic luma band-offset mode ───────────────
 * The encoder chooses parameters from original vs. post-deblock
 * reconstruction; the decoder applies the signaled parameters only.
 * Chroma remains intentionally unchanged in this first normative mode. */

static int sao_band_of(int sample)
{
    return tc_clip(sample, 0, 255) >> 3;
}

#if !TCODEC_NEON
void tc_sao_ctu_luma(tc_pixel_t *y, int stride_y,
                     int x, int y0, int width, int height,
                     int band, int offset)
{
    int x1 = tc_min(x + TC_CTU_SIZE, width);
    int y1 = tc_min(y0 + TC_CTU_SIZE, height);
    band = tc_clip(band, 0, 31);
    offset = tc_clip(offset, -7, 7);
    for (int row = y0; row < y1; ++row) {
        for (int col = x; col < x1; ++col) {
            tc_pixel_t *p = &y[row * stride_y + col];
            if (sao_band_of(*p) == band)
                *p = (tc_pixel_t)tc_clip((int)*p + offset, 0, 255);
        }
    }
}
#endif /* !TCODEC_NEON: scalar SAO application */

int tc_sao_choose_bo(const tc_pixel_t *orig, int orig_stride,
                     const tc_pixel_t *recon, int recon_stride,
                     int x, int y, int width, int height,
                     int *band, int *offset)
{
    int64_t base_sse = 0;
    int64_t best_sse = 0;
    int best_band = 0, best_offset = 0;
    int x1 = tc_min(x + TC_CTU_SIZE, width);
    int y1 = tc_min(y + TC_CTU_SIZE, height);
    int64_t sum[32] = {0};
    int64_t count[32] = {0};

    for (int row = y; row < y1; ++row) {
        for (int col = x; col < x1; ++col) {
            int o = orig[row * orig_stride + col];
            int r = recon[row * recon_stride + col];
            int d = o - r;
            base_sse += (int64_t)d * d;
            int b = sao_band_of(r);
            sum[b] += d;
            count[b]++;
        }
    }
    best_sse = base_sse;
    for (int b = 0; b < 32; ++b) {
        if (!count[b]) continue;
        int candidate = (int)((sum[b] + (sum[b] >= 0 ? count[b] / 2 : -count[b] / 2)) / count[b]);
        candidate = tc_clip(candidate, -7, 7);
        if (!candidate) continue;
        int64_t sse = 0;
        for (int row = y; row < y1; ++row) {
            for (int col = x; col < x1; ++col) {
                int o = orig[row * orig_stride + col];
                int r = recon[row * recon_stride + col];
                if (sao_band_of(r) == b) r = tc_clip(r + candidate, 0, 255);
                int d = o - r;
                sse += (int64_t)d * d;
            }
        }
        if (sse < best_sse) {
            best_sse = sse;
            best_band = b;
            best_offset = candidate;
        }
    }
    if (best_sse >= base_sse) return 0;
    *band = best_band;
    *offset = best_offset;
    return 1;
}
