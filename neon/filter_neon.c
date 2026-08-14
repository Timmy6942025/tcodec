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
 * D9: weak luma edges are vectorized in 8-pixel horizontal and
 * 4-pixel vertical batches. Mixed/strong decisions and chroma retain
 * the scalar-equivalent path because their per-lane branches and
 * in-place dependencies must remain byte-identical.
 */

#include "tcodec_common.h"

#if TCODEC_NEON
#include <arm_neon.h>

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

static int n_weak_filter8_horiz(tc_pixel_t *y, int stride,
                                int x, int row, int qp)
{
    uint8x8_t p1 = vld1_u8(y + (row - 2) * stride + x);
    uint8x8_t p0 = vld1_u8(y + (row - 1) * stride + x);
    uint8x8_t q0 = vld1_u8(y + row * stride + x);
    uint8x8_t q1 = vld1_u8(y + (row + 1) * stride + x);
    int16x8_t p1s = vreinterpretq_s16_u16(vmovl_u8(p1));
    int16x8_t p0s = vreinterpretq_s16_u16(vmovl_u8(p0));
    int16x8_t q0s = vreinterpretq_s16_u16(vmovl_u8(q0));
    int16x8_t q1s = vreinterpretq_s16_u16(vmovl_u8(q1));
    int16x8_t four = vdupq_n_s16(4);
    int16x8_t tc_v = vdupq_n_s16((int16_t)tc_clip(qp / 3, 1, 10));
    int16x8_t delta = vaddq_s16(vsubq_s16(vaddq_s16(vmulq_n_s16(p0s, 4),
                                                      vmulq_n_s16(q0s, 4)),
                                          vaddq_s16(p1s, q1s)), four);
    delta = vshrq_n_s16(delta, 3);
    delta = vmaxq_s16(vminq_s16(delta, tc_v), vnegq_s16(tc_v));
    int16x8_t p_out = vaddq_s16(p0s, delta);
    int16x8_t q_delta = vaddq_s16(vsubq_s16(vaddq_s16(vmulq_n_s16(q0s, 4),
                                                       vmulq_n_s16(p0s, 4)),
                                           vaddq_s16(q1s, p1s)), four);
    q_delta = vshrq_n_s16(q_delta, 3);
    q_delta = vmaxq_s16(vminq_s16(q_delta, tc_v), vnegq_s16(tc_v));
    int16x8_t q_out = vaddq_s16(q0s, q_delta);
    p_out = vmaxq_s16(vminq_s16(p_out, vdupq_n_s16(255)), vdupq_n_s16(0));
    q_out = vmaxq_s16(vminq_s16(q_out, vdupq_n_s16(255)), vdupq_n_s16(0));
    vst1_u8(y + (row - 1) * stride + x, vqmovun_s16(p_out));
    vst1_u8(y + row * stride + x, vqmovun_s16(q_out));
    return 1;
}

static int n_weak_filter4_vert(tc_pixel_t *y, int stride,
                               int x, int y_start, int qp)
{
    uint8_t p1a[8] = {0}, p0a[8] = {0}, q0a[8] = {0}, q1a[8] = {0};
    for (int lane = 0; lane < 4; lane++) {
        int row = y_start + lane;
        p1a[lane] = y[row * stride + x - 2];
        p0a[lane] = y[row * stride + x - 1];
        q0a[lane] = y[row * stride + x];
        q1a[lane] = y[row * stride + x + 1];
    }
    int16x8_t p1s = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p1a)));
    int16x8_t p0s = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(p0a)));
    int16x8_t q0s = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q0a)));
    int16x8_t q1s = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(q1a)));
    int16x8_t tc_v = vdupq_n_s16((int16_t)tc_clip(qp / 3, 1, 10));
    int16x8_t four = vdupq_n_s16(4);
    int16x8_t dp = vshrq_n_s16(vaddq_s16(vsubq_s16(vaddq_s16(vmulq_n_s16(p0s, 4),
                                                               vmulq_n_s16(q0s, 4)),
                                                   vaddq_s16(p1s, q1s)), four), 3);
    dp = vmaxq_s16(vminq_s16(dp, tc_v), vnegq_s16(tc_v));
    int16x8_t dq = vshrq_n_s16(vaddq_s16(vsubq_s16(vaddq_s16(vmulq_n_s16(q0s, 4),
                                                               vmulq_n_s16(p0s, 4)),
                                                   vaddq_s16(q1s, p1s)), four), 3);
    dq = vmaxq_s16(vminq_s16(dq, tc_v), vnegq_s16(tc_v));
    int16x8_t po = vmaxq_s16(vminq_s16(vaddq_s16(p0s, dp), vdupq_n_s16(255)), vdupq_n_s16(0));
    int16x8_t qo = vmaxq_s16(vminq_s16(vaddq_s16(q0s, dq), vdupq_n_s16(255)), vdupq_n_s16(0));
    uint8_t poa[8], qoa[8];
    vst1_u8(poa, vqmovun_s16(po));
    vst1_u8(qoa, vqmovun_s16(qo));
    for (int lane = 0; lane < 4; lane++) {
        int row = y_start + lane;
        y[row * stride + x - 1] = poa[lane];
        y[row * stride + x] = qoa[lane];
    }
    return 1;
}

static void n_filter_vert_edge(tc_pixel_t *y, int stride,
                               int x, int y_start, int height, int qp)
{
    for (int row = 0; row + 4 <= height; row += 4) {
        int all_weak = 1;
        for (int lane = 0; lane < 4; lane++) {
            int py = y_start + row + lane;
            int p0 = y[py * stride + x - 1];
            int p1 = y[py * stride + x - 2];
            int q0 = y[py * stride + x];
            int q1 = y[py * stride + x + 1];
            int s = n_edge_strength(p0, p1, q0, q1, qp);
            if (s == 0 || s >= 3) { all_weak = 0; break; }
        }
        if (all_weak) {
            n_weak_filter4_vert(y, stride, x, y_start + row, qp);
        } else {
            for (int lane = 0; lane < 4; lane++) {
                int py = y_start + row + lane;
                int p3 = y[py * stride + x - 4];
                int p2 = y[py * stride + x - 3];
                int p1 = y[py * stride + x - 2];
                int p0 = y[py * stride + x - 1];
                int q0 = y[py * stride + x];
                int q1 = y[py * stride + x + 1];
                int q2 = y[py * stride + x + 2];
                int q3 = y[py * stride + x + 3];
                int strength = n_edge_strength(p0,p1,q0,q1,qp);
                if (!strength) continue;
                int tc = tc_clip(qp / 3, 1, 10);
                if (strength >= 3 && tc_abs(p2-p0) < tc && tc_abs(p3-p0) < tc &&
                    tc_abs(q2-q0) < tc && tc_abs(q3-q0) < tc) {
                    y[py*stride+x-2] = n_strong_filter_p(p3,p2,p1,p0,q0);
                    y[py*stride+x-1] = n_strong_filter_p(p2,p1,p0,q0,q1);
                    y[py*stride+x] = n_strong_filter_q(p0,q0,q1,q2,q3);
                    y[py*stride+x+1] = n_strong_filter_q(p1,q0,q1,q2,q3);
                } else {
                    y[py*stride+x-1] = n_weak_filter(p1,p0,q0,q1,tc);
                    y[py*stride+x] = n_weak_filter(q1,q0,p0,p1,tc);
                }
            }
        }
    }
    for (int row = (height / 4) * 4; row < height; row++) {
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
    for (int col = 0; col + 8 <= width; col += 8) {
        int all_weak = 1;
        for (int lane = 0; lane < 8; lane++) {
            int px = x_start + col + lane;
            int p0 = y[(row - 1) * stride + px];
            int p1 = y[(row - 2) * stride + px];
            int q0 = y[row * stride + px];
            int q1 = y[(row + 1) * stride + px];
            if (n_edge_strength(p0, p1, q0, q1, qp) == 0 ||
                n_edge_strength(p0, p1, q0, q1, qp) >= 3) {
                all_weak = 0;
                break;
            }
        }
        if (all_weak) {
            n_weak_filter8_horiz(y, stride, x_start + col, row, qp);
        } else {
            for (int lane = 0; lane < 8; lane++) {
                int px = x_start + col + lane;
                int p3 = y[(row - 4) * stride + px];
                int p2 = y[(row - 3) * stride + px];
                int p1 = y[(row - 2) * stride + px];
                int p0 = y[(row - 1) * stride + px];
                int q0 = y[row * stride + px];
                int q1 = y[(row + 1) * stride + px];
                int q2 = y[(row + 2) * stride + px];
                int q3 = y[(row + 3) * stride + px];
                int strength = n_edge_strength(p0, p1, q0, q1, qp);
                if (!strength) continue;
                int tc = tc_clip(qp / 3, 1, 10);
                if (strength >= 3 && tc_abs(p2 - p0) < tc && tc_abs(p3 - p0) < tc &&
                    tc_abs(q2 - q0) < tc && tc_abs(q3 - q0) < tc) {
                    y[(row - 2) * stride + px] = n_strong_filter_p(p3,p2,p1,p0,q0);
                    y[(row - 1) * stride + px] = n_strong_filter_p(p2,p1,p0,q0,q1);
                    y[row * stride + px] = n_strong_filter_q(p0,q0,q1,q2,q3);
                    y[(row + 1) * stride + px] = n_strong_filter_q(p1,q0,q1,q2,q3);
                } else {
                    y[(row - 1) * stride + px] = n_weak_filter(p1,p0,q0,q1,tc);
                    y[row * stride + px] = n_weak_filter(q1,q0,p0,p1,tc);
                }
            }
        }
    }
    for (int col = (width / 8) * 8; col < width; col++) {
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

void tc_sao_ctu_luma(tc_pixel_t *y, int stride_y,
                     int x, int y0, int width, int height,
                     int band, int offset)
{
    int x1 = tc_min(x + TC_CTU_SIZE, width);
    int y1 = tc_min(y0 + TC_CTU_SIZE, height);
    band = tc_clip(band, 0, 31);
    offset = tc_clip(offset, -7, 7);
    uint8x16_t band_v = vdupq_n_u8((uint8_t)band);
    uint8x16_t off_v = vdupq_n_u8((uint8_t)(offset < 0 ? -offset : offset));
    for (int row = y0; row < y1; ++row) {
        int col = x;
        for (; col + 16 <= x1; col += 16) {
            uint8x16_t src = vld1q_u8(y + row * stride_y + col);
            uint8x16_t classes = vshrq_n_u8(src, 3);
            uint8x16_t mask = vceqq_u8(classes, band_v);
            uint8x16_t adjusted = offset < 0 ? vqsubq_u8(src, off_v) : vqaddq_u8(src, off_v);
            vst1q_u8(y + row * stride_y + col, vbslq_u8(mask, adjusted, src));
        }
        for (; col < x1; ++col) {
            tc_pixel_t *p = &y[row * stride_y + col];
            if ((*p >> 3) == band)
                *p = (tc_pixel_t)tc_clip((int)*p + offset, 0, 255);
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
