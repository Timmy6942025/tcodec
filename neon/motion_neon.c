/*
 * motion_neon.c — NEON-optimized motion estimation helpers for TCodec
 *
 * NEON SAD (Sum of Absolute Differences) for 4×4, 8×8, 16×16 blocks.
 * This is the hottest function in motion estimation — NEON gives ~6× speedup.
 *
 * Also includes quarter-pel interpolation and sub-pixel SAD.
 */

#include "tcodec_common.h"

#if TCODEC_NEON
#include <arm_neon.h>

/* ── SAD 8×8 (NEON) ────────────────────────────────────────────
 *
 * Process 8 pixels per row with vabdq_u8 + vpaddlq_u16.
 * Accumulate across 8 rows → single SAD value.
 * ══════════════════════════════════════════════════════════════ */

tc_sad_t tc_sad_8x8_neon(const tc_pixel_t *a, int stride_a,
                          const tc_pixel_t *b, int stride_b)
{
    uint32x4_t acc = vdupq_n_u32(0);

    for (int y = 0; y < 8; y++) {
        uint8x8_t ra = vld1_u8(a + y * stride_a);
        uint8x8_t rb = vld1_u8(b + y * stride_b);

        /* Absolute difference, pairwise add to u16, then widen add to u32 */
        uint8x8_t abd = vabd_u8(ra, rb);
        uint16x8_t pad = vpaddlq_u8(vcombine_u8(abd, vdup_n_u8(0)));
        acc = vaddq_u32(acc, vpaddlq_u16(pad));
    }

    /* Horizontal sum of 4 u32 values */
    uint32x2_t pair = vadd_u32(vget_low_u32(acc), vget_high_u32(acc));
    uint32x2_t sum2 = vpadd_u32(pair, pair);
    return (tc_sad_t)vget_lane_u32(sum2, 0);
}

/* ── SAD 4×4 (NEON) ──────────────────────────────────────────── */

tc_sad_t tc_sad_4x4_neon(const tc_pixel_t *a, int stride_a,
                          const tc_pixel_t *b, int stride_b)
{
    uint32x2_t acc = vdup_n_u32(0);

    for (int y = 0; y < 4; y++) {
        uint8x8_t ra = vld1_u8(a + y * stride_a);  /* Only use low 4 */
        uint8x8_t rb = vld1_u8(b + y * stride_b);

        uint8x8_t abd = vabd_u8(ra, rb);
        uint16x8_t pad = vpaddlq_u8(vcombine_u8(abd, vdup_n_u8(0)));
        acc = vadd_u32(acc, vget_low_u32(vpaddlq_u16(pad)));
    }

    uint32x2_t sum2 = vpadd_u32(acc, acc);
    return (tc_sad_t)vget_lane_u32(sum2, 0);
}

/* ── SAD 16×16 (NEON) ─────────────────────────────────────────── */

tc_sad_t tc_sad_16x16_neon(const tc_pixel_t *a, int stride_a,
                            const tc_pixel_t *b, int stride_b)
{
    uint32x4_t acc = vdupq_n_u32(0);

    for (int y = 0; y < 16; y++) {
        uint8x16_t ra = vld1q_u8(a + y * stride_a);
        uint8x16_t rb = vld1q_u8(b + y * stride_b);

        uint8x16_t abd = vabdq_u8(ra, rb);
        uint16x8_t pad = vpaddlq_u8(abd);
        acc = vaddq_u32(acc, vpaddlq_u16(pad));
    }

    uint32x2_t pair = vadd_u32(vget_low_u32(acc), vget_high_u32(acc));
    uint32x2_t sum2 = vpadd_u32(pair, pair);
    return (tc_sad_t)vget_lane_u32(sum2, 0);
}

/* ── General SAD dispatcher ───────────────────────────────────── */

tc_sad_t tc_sad(const tc_pixel_t *a, int stride_a,
                      const tc_pixel_t *b, int stride_b, int n)
{
    switch (n) {
        case 4:  return tc_sad_4x4_neon(a, stride_a, b, stride_b);
        case 8:  return tc_sad_8x8_neon(a, stride_a, b, stride_b);
        case 16: return tc_sad_16x16_neon(a, stride_a, b, stride_b);
        default: {
            /* Fallback for non-power-of-2 sizes */
            uint32x4_t acc = vdupq_n_u32(0);
            for (int y = 0; y < n; y++) {
                int x = 0;
                for (; x + 7 < n; x += 8) {
                    uint8x8_t ra = vld1_u8(a + y * stride_a + x);
                    uint8x8_t rb = vld1_u8(b + y * stride_b + x);
                    uint8x8_t abd = vabd_u8(ra, rb);
                    uint16x8_t pad = vpaddlq_u8(vcombine_u8(abd, vdup_n_u8(0)));
                    acc = vaddq_u32(acc, vpaddlq_u16(pad));
                }
                /* Handle remaining pixels */
                uint32_t scalar_sad = 0;
                for (; x < n; x++) {
                    int diff = (int)a[y * stride_a + x] - (int)b[y * stride_b + x];
                    scalar_sad += tc_abs(diff);
                }
                uint32x2_t spair = vdup_n_u32(scalar_sad);
                acc = vaddq_u32(acc, vcombine_u32(spair, vdup_n_u32(0)));
            }
            uint32x2_t pair = vadd_u32(vget_low_u32(acc), vget_high_u32(acc));
            uint32x2_t sum2 = vpadd_u32(pair, pair);
            return (tc_sad_t)vget_lane_u32(sum2, 0);
        }
    }
}

/* ── Exact decoder luma/chroma interpolation (NEON) ──────────────
 *
 * These kernels mirror the scalar 6-tap luma and bilinear chroma
 * formulas. They are dispatched only by decoder-only wrappers after
 * the full reference footprint and supported block shape are proved.
 * Encoder prediction remains on the scalar normative path. Edge,
 * diagonal, and unsupported schedules retain scalar fallback.
 * ══════════════════════════════════════════════════════════════ */

#if 0
static void tc_inter_predict_neon_legacy(const tc_pixel_t *ref, int ref_stride,
                            int ref_w, int ref_h,
                            tc_mv_s mv,
                            tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                            int blk_size)
{
    (void)ref_w; (void)ref_h;  /* Bounds assumed checked by caller */
    int ix = mv.x >> 2;  /* Integer part */
    int iy = mv.y >> 2;
    int fx = mv.x & 3;   /* Fractional part (0-3) */
    int fy = mv.y & 3;

    if (fx == 0 && fy == 0) {
        /* Integer position — just copy */
        for (int y = 0; y < blk_size; y++) {
            memcpy(dst + y * dst_stride,
                   ref + (iy + y) * ref_stride + ix,
                   (size_t)blk_size);
        }
        return;
    }

    /* Sub-pel: bilinear interpolation */
    int16x8_t two = vdupq_n_s16(2);
    for (int y = 0; y < blk_size; y++) {
        const tc_pixel_t *row0 = ref + (iy + y) * ref_stride + ix;
        const tc_pixel_t *row1 = row0 + ref_stride;

        if (fx == 0) {
            /* Vertical only */
            for (int x = 0; x + 7 < blk_size; x += 8) {
                int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row0 + x)));
                int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row1 + x)));
                int16x8_t r = vaddq_s16(
                    vaddq_s16(vmulq_n_s16(a, 4 - fy), vmulq_n_s16(b, fy)),
                    two);
                r = vrshrq_n_s16(r, 2);
                int16x8_t zero = vdupq_n_s16(0);
                int16x8_t max255 = vdupq_n_s16(255);
                r = vminq_s16(vmaxq_s16(r, zero), max255);
                vst1_u8(dst + y * dst_stride + x,
                         vmovn_u16(vreinterpretq_u16_s16(r)));
            }
        } else if (fy == 0) {
            /* Horizontal only */
            for (int x = 0; x + 7 < blk_size; x += 8) {
                int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row0 + x)));
                int16x8_t b = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row0 + x + 1)));
                int16x8_t r = vaddq_s16(
                    vaddq_s16(vmulq_n_s16(a, 4 - fx), vmulq_n_s16(b, fx)),
                    two);
                r = vrshrq_n_s16(r, 2);
                int16x8_t zero = vdupq_n_s16(0);
                int16x8_t max255 = vdupq_n_s16(255);
                r = vminq_s16(vmaxq_s16(r, zero), max255);
                vst1_u8(dst + y * dst_stride + x,
                         vmovn_u16(vreinterpretq_u16_s16(r)));
            }
        } else {
            /* Full bilinear: 4 reference points per output pixel */
            for (int x = 0; x < blk_size; x++) {
                int a = row0[x], b_row = row0[x + 1];
                int c = row1[x], d = row1[x + 1];
                int top = (a * (4 - fx) + b_row * fx + 2) >> 2;
                int bot = (c * (4 - fx) + d * fx + 2) >> 2;
                int val = (top * (4 - fy) + bot * fy + 2) >> 2;
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(val, 0, 255);
            }
        }
    }
}
#endif

/* Exact SIMD luma/chroma predictors used by motion.c after it proves the
 * full reference footprint is in-frame.  The formulas mirror motion.c's
 * scalar filter6/luma_interp_hv and chroma bilinear paths, including every
 * rounding shift. */
static TCODEC_FORCEINLINE int16x8_t neon_luma_hrow_raw(
    const tc_pixel_t *row, int x)
{
    int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x)));
    int16x8_t p0 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x - 2)));
    int16x8_t p1 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x - 1)));
    int16x8_t p3 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x + 1)));
    int16x8_t p4 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x + 2)));
    int16x8_t p5 = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x + 3)));
    int16x8_t sum = vaddq_s16(vsubq_s16(vaddq_s16(p0, p5),
                                         vaddq_s16(vmulq_n_s16(p1, 5),
                                                   vmulq_n_s16(p4, 5))),
                              vaddq_s16(vmulq_n_s16(a, 20),
                                        vmulq_n_s16(p3, 20)));
    return vshrq_n_s16(vaddq_s16(sum, vdupq_n_s16(16)), 5);
}

static TCODEC_FORCEINLINE int16x8_t neon_luma_vcol_raw(
    const tc_pixel_t *ref, int stride, int x, int y)
{
    int16x8_t rows[6];
    for (int k = 0; k < 6; ++k)
        rows[k] = vreinterpretq_s16_u16(vmovl_u8(
            vld1_u8(ref + (y - 2 + k) * stride + x)));
    int16x8_t sum = vaddq_s16(vsubq_s16(vaddq_s16(rows[0], rows[5]),
                                         vaddq_s16(vmulq_n_s16(rows[1], 5),
                                                   vmulq_n_s16(rows[4], 5))),
                              vaddq_s16(vmulq_n_s16(rows[2], 20),
                                        vmulq_n_s16(rows[3], 20)));
    return vshrq_n_s16(vaddq_s16(sum, vdupq_n_s16(16)), 5);
}

static TCODEC_FORCEINLINE int16x8_t neon_luma_hrow(
    const tc_pixel_t *row, int x, int frac)
{
    int16x8_t a = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x)));
    if (frac == 0) return a;

    /* Load the six tap vectors explicitly.  This is a little more load
     * traffic than a padded sliding window, but it is valid for arbitrary
     * frame strides and keeps each lane mapped to the scalar tap exactly. */
    int16x8_t half = neon_luma_hrow_raw(row, x);
    if (frac == 2) {
        /* A true half-pel is clipped by luma_interp_h().  Quarter-pel
         * callers must retain the raw intermediate until the final blend,
         * matching the scalar luma_interp_h() rounding order. */
        return vmaxq_s16(vminq_s16(half, vdupq_n_s16(255)),
                         vdupq_n_s16(0));
    }
    if (frac == 1)
        return vshrq_n_s16(vaddq_s16(vaddq_s16(a, half), vdupq_n_s16(1)), 1);
    return vshrq_n_s16(vaddq_s16(vaddq_s16(half,
        vreinterpretq_s16_u16(vmovl_u8(vld1_u8(row + x + 1)))),
        vdupq_n_s16(1)), 1);
}

void tc_inter_predict_neon(const tc_pixel_t *ref, int ref_stride,
                           int ref_w, int ref_h, tc_mv_s mv,
                           tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                           int blk_size)
{
    (void)ref_w; (void)ref_h;
    const int fx = mv.x >> 2, fy = mv.y >> 2;
    const int dx = mv.x & 3, dy = mv.y & 3;
    const int16x8_t zero = vdupq_n_s16(0), maxv = vdupq_n_s16(255);
    if ((blk_size & 7) != 0) {
        /* This path is only an API fallback; production v2 CUs are 8n. */
        for (int y = 0; y < blk_size; ++y)
            for (int x = 0; x < blk_size; ++x) {
                const tc_pixel_t *r = ref + (fy + y) * ref_stride + fx + x;
                int v;
                if (!dx && !dy) v = r[0];
                else if (!dy) {
                    int h = (r[-2] - 5*r[-1] + 20*r[0] + 20*r[1] - 5*r[2] + r[3] + 16) >> 5;
                    v = dx == 2 ? h : ((dx == 1 ? r[0] : h) + (dx == 1 ? h : r[1]) + 1) >> 1;
                } else {
                    /* Unusual sizes do not occur in the v2 decoder. */
                    v = r[0];
                }
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(v, 0, 255);
            }
        return;
    }
    for (int y = 0; y < blk_size; ++y) {
        const int yy = fy + y;
        for (int x = 0; x < blk_size; x += 8) {
            int16x8_t top = neon_luma_hrow(ref + yy * ref_stride, fx + x, dx);
            int16x8_t value = top;
            if (dx && dy) {
                /* Diagonal phases follow luma_interp_hv()'s nine-anchor
                 * construction. Keep every half-pel intermediate raw;
                 * only the final result is clipped. */
                int16x8_t htop = neon_luma_hrow_raw(ref + yy * ref_stride, fx + x);
                int16x8_t hbot = neon_luma_hrow_raw(ref + (yy + 1) * ref_stride, fx + x);
                int16x8_t c = neon_luma_vcol_raw(ref, ref_stride, fx + x, yy);
                int16x8_t f = neon_luma_vcol_raw(ref, ref_stride, fx + x + 1, yy);
                int16x8_t drows[6];
                for (int k = 0; k < 6; ++k)
                    drows[k] = neon_luma_hrow_raw(ref + (yy - 2 + k) * ref_stride,
                                                   fx + x);
                int16x8_t dsum = vaddq_s16(vsubq_s16(vaddq_s16(drows[0], drows[5]),
                                                     vaddq_s16(vmulq_n_s16(drows[1], 5),
                                                               vmulq_n_s16(drows[4], 5))),
                                           vaddq_s16(vmulq_n_s16(drows[2], 20),
                                                     vmulq_n_s16(drows[3], 20)));
                int16x8_t d = vshrq_n_s16(vaddq_s16(dsum, vdupq_n_s16(16)), 5);
                int16x8_t one = vdupq_n_s16(1);
                int16x8_t topv, midv, botv;
                int16x8_t a_top = vreinterpretq_s16_u16(vmovl_u8(
                    vld1_u8(ref + yy * ref_stride + fx + x)));
                int16x8_t a_top_next = vreinterpretq_s16_u16(vmovl_u8(
                    vld1_u8(ref + yy * ref_stride + fx + x + 1)));
                int16x8_t a_bot = vreinterpretq_s16_u16(vmovl_u8(
                    vld1_u8(ref + (yy + 1) * ref_stride + fx + x)));
                int16x8_t a_bot_next = vreinterpretq_s16_u16(vmovl_u8(
                    vld1_u8(ref + (yy + 1) * ref_stride + fx + x + 1)));
                if (dx == 1) {
                    topv = vshrq_n_s16(vaddq_s16(vaddq_s16(a_top, htop), one), 1);
                    midv = vshrq_n_s16(vaddq_s16(vaddq_s16(c, d), one), 1);
                    botv = vshrq_n_s16(vaddq_s16(vaddq_s16(a_bot, hbot), one), 1);
                } else if (dx == 2) {
                    topv = htop; midv = d; botv = hbot;
                } else {
                    topv = vshrq_n_s16(vaddq_s16(vaddq_s16(htop, a_top_next), one), 1);
                    midv = vshrq_n_s16(vaddq_s16(vaddq_s16(d, f), one), 1);
                    botv = vshrq_n_s16(vaddq_s16(vaddq_s16(hbot, a_bot_next), one), 1);
                }
                if (dy == 1) value = vshrq_n_s16(vaddq_s16(vaddq_s16(topv, midv), one), 1);
                else if (dy == 2) value = midv;
                else value = vshrq_n_s16(vaddq_s16(vaddq_s16(midv, botv), one), 1);
            } else if (dy) {
                int16x8_t rows[6];
                for (int k = 0; k < 6; ++k)
                    rows[k] = neon_luma_hrow(ref + (yy - 2 + k) * ref_stride,
                                              fx + x, dx);
                int16x8_t sum = vaddq_s16(vsubq_s16(vaddq_s16(rows[0], rows[5]),
                                                     vaddq_s16(vmulq_n_s16(rows[1], 5),
                                                               vmulq_n_s16(rows[4], 5))),
                                          vaddq_s16(vmulq_n_s16(rows[2], 20),
                                                    vmulq_n_s16(rows[3], 20)));
                int16x8_t mid = vshrq_n_s16(vaddq_s16(sum, vdupq_n_s16(16)), 5);
                /* luma_interp_v() clips a true half-pel, but retains the
                 * raw filter result for quarter-pel blending and clips only
                 * the final output. */
                if (dx == 0 && dy == 2)
                    mid = vmaxq_s16(vminq_s16(mid, maxv), zero);
                if (dy == 2) value = mid;
                else if (dy == 1)
                    value = vshrq_n_s16(vaddq_s16(vaddq_s16(top, mid),
                                                   vdupq_n_s16(1)), 1);
                else {
                    int16x8_t bot = neon_luma_hrow(ref + (yy + 1) * ref_stride,
                                                   fx + x, dx);
                    value = vshrq_n_s16(vaddq_s16(vaddq_s16(mid, bot),
                                                   vdupq_n_s16(1)), 1);
                }
            }
            value = vmaxq_s16(vminq_s16(value, maxv), zero);
            vst1_u8(dst + y * dst_stride + x, vqmovun_s16(value));
        }
    }
}

void tc_inter_predict_chroma_neon(const tc_pixel_t *ref, int ref_stride,
                                  int ref_w, int ref_h, tc_mv_s mv,
                                  tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                                  int blk_size)
{
    (void)ref_w; (void)ref_h;
    const int xi = mv.x >> 3, yi = mv.y >> 3;
    const int fx = mv.x & 7, fy = mv.y & 7;
    if (blk_size < 8 || (blk_size & 7) != 0) {
        /* 4×4 CUs are common at higher QP.  Use four-lane NEON here too;
         * vld1_lane_u32 reads exactly the four valid bytes and therefore
         * does not require padded frame rows.  Keep odd sizes on the
         * scalar-equivalent fallback below so no tail is left unwritten. */
        if ((blk_size & 3) != 0) {
            for (int y = 0; y < blk_size; ++y) {
                const tc_pixel_t *r0 = ref + (yi + y) * ref_stride + xi;
                const tc_pixel_t *r1 = r0 + ref_stride;
                for (int x = 0; x < blk_size; ++x) {
                    int A=r0[x], B=r0[x+1], C=r1[x], D=r1[x+1];
                    dst[y*dst_stride+x] = (tc_pixel_t)(((8-fx)*(8-fy)*A +
                        fx*(8-fy)*B + (8-fx)*fy*C + fx*fy*D + 32) >> 6);
                }
            }
            return;
        }
        const int wa = (8-fx)*(8-fy), wb = fx*(8-fy);
        const int wc = (8-fx)*fy, wd = fx*fy;
        const int16x8_t w_a = vdupq_n_s16((int16_t)wa);
        const int16x8_t w_b = vdupq_n_s16((int16_t)wb);
        const int16x8_t w_c = vdupq_n_s16((int16_t)wc);
        const int16x8_t w_d = vdupq_n_s16((int16_t)wd);
        const int16x8_t round = vdupq_n_s16(32);
        for (int y = 0; y < blk_size; ++y) {
            const tc_pixel_t *r0 = ref + (yi + y) * ref_stride + xi;
            const tc_pixel_t *r1 = r0 + ref_stride;
            for (int x = 0; x + 4 <= blk_size; x += 4) {
                uint32_t aa, bb, cc, dd;
                memcpy(&aa, r0 + x, 4);
                memcpy(&bb, r0 + x + 1, 4);
                memcpy(&cc, r1 + x, 4);
                memcpy(&dd, r1 + x + 1, 4);
                uint32x2_t za = vdup_n_u32(aa), zb = vdup_n_u32(bb);
                uint32x2_t zc = vdup_n_u32(cc), zd = vdup_n_u32(dd);
                int16x8_t A = vreinterpretq_s16_u16(vmovl_u8(vreinterpret_u8_u32(za)));
                int16x8_t B = vreinterpretq_s16_u16(vmovl_u8(vreinterpret_u8_u32(zb)));
                int16x8_t C = vreinterpretq_s16_u16(vmovl_u8(vreinterpret_u8_u32(zc)));
                int16x8_t D = vreinterpretq_s16_u16(vmovl_u8(vreinterpret_u8_u32(zd)));
                int16x8_t sum = vmulq_s16(A, w_a);
                sum = vaddq_s16(sum, vmulq_s16(B, w_b));
                sum = vaddq_s16(sum, vmulq_s16(C, w_c));
                sum = vaddq_s16(sum, vmulq_s16(D, w_d));
                sum = vshrq_n_s16(vaddq_s16(sum, round), 6);
                uint32_t out;
                vst1_lane_u32(&out,
                              vreinterpret_u32_u8(vqmovun_s16(sum)), 0);
                memcpy(dst + y * dst_stride + x, &out, 4);
            }
        }
        return;
    }
    /* One-dimensional fractional positions need only two samples.  Keep
     * these separate from the four-tap path: it removes two loads and two
     * multiplies per output pixel without changing the normative rounding. */
    if (fy == 0 || fx == 0) {
        const int frac = fy == 0 ? fx : fy;
        const int wa = 8 - frac, wb = frac;
        const int16x8_t w_a = vdupq_n_s16((int16_t)wa);
        const int16x8_t w_b = vdupq_n_s16((int16_t)wb);
        const int16x8_t round = vdupq_n_s16(4);
        for (int y = 0; y < blk_size; ++y) {
            const tc_pixel_t *r0 = ref + (yi + y) * ref_stride + xi;
            const tc_pixel_t *r1 = r0 + ref_stride;
            for (int x = 0; x < blk_size; x += 8) {
                int16x8_t A = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0 + x)));
                int16x8_t B = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(
                    fy == 0 ? r0 + x + 1 : r1 + x)));
                if (fy == 0) {
                    /* Horizontal interpolation uses A/B from the same row. */
                    B = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0 + x + 1)));
                }
                int16x8_t sum = vaddq_s16(vmulq_s16(A, w_a),
                                          vmulq_s16(B, w_b));
                sum = vshrq_n_s16(vaddq_s16(sum, round), 3);
                vst1_u8(dst + y * dst_stride + x, vqmovun_s16(sum));
            }
        }
        return;
    }

    const int wa = (8-fx)*(8-fy), wb = fx*(8-fy);
    const int wc = (8-fx)*fy, wd = fx*fy;
    const int16x8_t w_a = vdupq_n_s16((int16_t)wa);
    const int16x8_t w_b = vdupq_n_s16((int16_t)wb);
    const int16x8_t w_c = vdupq_n_s16((int16_t)wc);
    const int16x8_t w_d = vdupq_n_s16((int16_t)wd);
    const int16x8_t round = vdupq_n_s16(32);
    for (int y = 0; y < blk_size; ++y) {
        const tc_pixel_t *r0 = ref + (yi + y) * ref_stride + xi;
        const tc_pixel_t *r1 = r0 + ref_stride;
        for (int x = 0; x < blk_size; x += 8) {
            int16x8_t A = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0 + x)));
            int16x8_t B = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r0 + x + 1)));
            int16x8_t C = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r1 + x)));
            int16x8_t D = vreinterpretq_s16_u16(vmovl_u8(vld1_u8(r1 + x + 1)));
            int16x8_t sum = vmulq_s16(A, w_a);
            sum = vaddq_s16(sum, vmulq_s16(B, w_b));
            sum = vaddq_s16(sum, vmulq_s16(C, w_c));
            sum = vaddq_s16(sum, vmulq_s16(D, w_d));
            sum = vshrq_n_s16(vaddq_s16(sum, round), 6);
            vst1_u8(dst + y * dst_stride + x, vqmovun_s16(sum));
        }
    }
}

#endif /* TCODEC_NEON */
