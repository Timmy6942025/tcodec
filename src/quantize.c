/*
 * quantize.c — Quantization for TCodec
 *
 * Features:
 *  - QP-to-scale mapping following HEVC-style power-of-2 structure
 *  - JND (Just Noticeable Difference) perceptual weighting per band
 *  - Dead-zone quantization for sparse coefficients
 *  - Dequantization with exact inverse
 */

#include "tcodec_common.h"

#if TCODEC_NEON
#include <arm_neon.h>
#endif

/* ── QP → Scale table ──────────────────────────────────────────
 *
 * Following HEVC convention:
 *   QP 0..5:  scale = 2^(QP/6 - 4) (fine quantization)
 *   QP 6..63: doubles every 6 QP steps
 *
 * For integer arithmetic, we store the scale as a fixed-point value.
 * The step size doubles every 6 QP increments:
 *   QScale(QP) = 2^((QP - 4) / 6)  (approximately)
 *
 * We use a lookup table for QP 0..63.
 * ══════════════════════════════════════════════════════════════ */

static const int qscale_table[64] = {
    /* QP  0..7  */    1,   1,   1,   1,   1,   2,   2,   2,
    /* QP  8..15 */    3,   3,   4,   4,   5,   6,   7,   8,
    /* QP 16..23 */    9,  10,  12,  14,  16,  18,  21,  24,
    /* QP 24..31 */   28,  32,  37,  42,  49,  56,  64,  74,
    /* QP 32..39 */   85,  98, 112, 128, 149, 171, 197, 226,
    /* QP 40..47 */  256, 299, 343, 394, 453, 512, 598, 686,
    /* QP 48..55 */  788, 907,1024,1196,1373,1576,1814,2048,
    /* QP 56..63 */ 2392,2745,3153,3629,4096,4784,5490,6305,
};

int tc_qscale(int qp)
{
    if (qp < 0)  return qscale_table[0];
    if (qp > 63) return qscale_table[63];
    return qscale_table[qp];
}

void tc_build_eff_scale_table(int qp, int table[4][64])
{
    const int scale = tc_qscale(qp);
    for (int band = 0; band < 4; band++) {
        for (int pos = 0; pos < 64; pos++) {
            const int weight = tc_jnd_weight(band, pos);
            table[band][pos] = tc_max((scale * weight + 4) >> 3, 1);
        }
    }
}

/* ── JND Perceptual Weighting ──────────────────────────────────
 *
 * Human vision is less sensitive to high-frequency detail.
 * We weight quantization by frequency band:
 *
 *   Band 0 (DC):        weight 0.75  (preserve carefully)
 *   Band 1 (Low AC):    weight 0.90  (slightly less critical)
 *   Band 2 (Mid AC):    weight 1.00  (normal)
 *   Band 3 (High AC):   weight 1.20  (coarser quantization OK)
 *
 * Weights are stored as 8-bit fixed point (8 = 1.0×).
 * ══════════════════════════════════════════════════════════════ */

static const int jnd_weight_table[4] = {
    6,   /* DC: 0.75× → quantize less (6/8 = 0.75) */
    7,   /* Low AC: 0.875× */
    8,   /* Mid AC: 1.0× */
    10,  /* High AC: 1.25× → quantize more */
};

int tc_jnd_weight(int band, int pos)
{
    TCODEC_UNUSED(pos);
    if (band < 0) band = 0;
    if (band > 3) band = 3;
    return jnd_weight_table[band];
}

/* tc_freq_band() is defined in entropy.c — classifies zigzag position
 * into frequency band (DC/low/mid/high) for JND-weighted quantization. */

/* ── Dead-zone quantization ────────────────────────────────────
 *
 * Dead-zone: coefficients with magnitude < threshold/2 are set to 0.
 * This produces sparser output, better compression.
 *
 * Quantize:  q = sign(c) × (|c| + offset) / scale
 * Dequant:   c' = q × scale
 *
 * The offset is typically scale/3 for dead-zone (not scale/2).
 * ══════════════════════════════════════════════════════════════ */

int tc_quantize(tc_coeff_t *TCODEC_RESTRICT coeffs, int n,
                int qp, int band)
{
    int scale = tc_qscale(qp);
    int weight = tc_jnd_weight(band, 0);
    int effective_scale = (scale * weight + 4) >> 3;  /* Apply JND weight */
    if (effective_scale < 1) effective_scale = 1;

    int offset = effective_scale / 3;   /* Dead-zone offset */
    int nonzero = 0;

    for (int i = 0; i < n; i++) {
        int c = coeffs[i];
        if (c > 0) {
            int q = (c + offset) / effective_scale;
            coeffs[i] = (tc_coeff_t)q;
            if (q) nonzero++;
        } else if (c < 0) {
            int q = -((-c + offset) / effective_scale);
            coeffs[i] = (tc_coeff_t)q;
            if (q) nonzero++;
        } else {
            coeffs[i] = 0;
        }
    }
    return nonzero;
}

void tc_dequantize(tc_coeff_t *TCODEC_RESTRICT coeffs, int n,
                   int qp, int band)
{
    int scale = tc_qscale(qp);
    int weight = tc_jnd_weight(band, 0);
    int effective_scale = (scale * weight + 4) >> 3;
    if (effective_scale < 1) effective_scale = 1;

    /* Mid-point reconstruction: add half-scale to reduce systematic bias.
     * For quantize(c) = sign(c) * (|c|+offset)/scale,
     * dequantize(q) = q * scale + (scale >> 1) gives better rate-distortion. */
    for (int i = 0; i < n; i++) {
        if (coeffs[i] > 0) {
            coeffs[i] = (tc_coeff_t)(coeffs[i] * effective_scale + (effective_scale >> 1));
        } else if (coeffs[i] < 0) {
            coeffs[i] = (tc_coeff_t)(coeffs[i] * effective_scale - (effective_scale >> 1));
        }
    }
}

void tc_recon_add_clip4x4(const tc_pixel_t *pred, int pred_stride,
                          const tc_coeff_t *res, tc_pixel_t *dst,
                          int dst_stride)
{
#if TCODEC_NEON
    for (int row = 0; row < 4; row++) {
        uint8_t pa[8] = {0}, outa[8];
        int16_t ra[8] = {0};
        memcpy(pa, pred + row * pred_stride, 4);
        memcpy(ra, res + row * 4, 4 * sizeof(*ra));
        uint16x8_t p16 = vmovl_u8(vld1_u8(pa));
        int16x8_t r16 = vld1q_s16(ra);
        int32x4_t sum_lo = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(p16))),
            vmovl_s16(vget_low_s16(r16)));
        int32x4_t sum_hi = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(p16))),
            vmovl_s16(vget_high_s16(r16)));
        sum_lo = vmaxq_s32(vminq_s32(sum_lo, vdupq_n_s32(255)), vdupq_n_s32(0));
        sum_hi = vmaxq_s32(vminq_s32(sum_hi, vdupq_n_s32(255)), vdupq_n_s32(0));
        int16x8_t sum = vcombine_s16(vmovn_s32(sum_lo), vmovn_s32(sum_hi));
        vst1_u8(outa, vqmovun_s16(sum));
        memcpy(dst + row * dst_stride, outa, 4);
    }
#else
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int v = (int)pred[row * pred_stride + col] + (int)res[row * 4 + col];
            dst[row * dst_stride + col] = (tc_pixel_t)tc_clip(v, 0, 255);
        }
    }
#endif
}

void tc_recon_add_clip8x8(const tc_pixel_t *pred, int pred_stride,
                          const tc_coeff_t *res, tc_pixel_t *dst,
                          int dst_stride)
{
#if TCODEC_NEON
    for (int row = 0; row < 8; row++) {
        uint8_t pa[8], outa[8];
        memcpy(pa, pred + row * pred_stride, 8);
        uint16x8_t p16 = vmovl_u8(vld1_u8(pa));
        int16x8_t r16 = vld1q_s16(res + row * 8);
        int32x4_t sum_lo = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(p16))),
            vmovl_s16(vget_low_s16(r16)));
        int32x4_t sum_hi = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(p16))),
            vmovl_s16(vget_high_s16(r16)));
        sum_lo = vmaxq_s32(vminq_s32(sum_lo, vdupq_n_s32(255)), vdupq_n_s32(0));
        sum_hi = vmaxq_s32(vminq_s32(sum_hi, vdupq_n_s32(255)), vdupq_n_s32(0));
        int16x8_t sum = vcombine_s16(vmovn_s32(sum_lo), vmovn_s32(sum_hi));
        vst1_u8(outa, vqmovun_s16(sum));
        memcpy(dst + row * dst_stride, outa, 8);
    }
#else
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            dst[row * dst_stride + col] = (tc_pixel_t)tc_clip(
                (int)pred[row * pred_stride + col] + res[row * 8 + col], 0, 255);
#endif
}

void tc_recon_add_dc8x8(const tc_pixel_t *pred, int pred_stride, int dc,
                        tc_pixel_t *dst, int dst_stride)
{
#if TCODEC_NEON
    for (int row = 0; row < 8; row++) {
        uint8_t pa[8], outa[8];
        memcpy(pa, pred + row * pred_stride, 8);
        uint16x8_t p16 = vmovl_u8(vld1_u8(pa));
        int32x4_t sum_lo = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(p16))), vdupq_n_s32(dc));
        int32x4_t sum_hi = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(p16))), vdupq_n_s32(dc));
        sum_lo = vmaxq_s32(vminq_s32(sum_lo, vdupq_n_s32(255)), vdupq_n_s32(0));
        sum_hi = vmaxq_s32(vminq_s32(sum_hi, vdupq_n_s32(255)), vdupq_n_s32(0));
        int16x8_t sum = vcombine_s16(vmovn_s32(sum_lo), vmovn_s32(sum_hi));
        vst1_u8(outa, vqmovun_s16(sum));
        memcpy(dst + row * dst_stride, outa, 8);
    }
#else
    for (int row = 0; row < 8; row++)
        for (int col = 0; col < 8; col++)
            dst[row * dst_stride + col] = (tc_pixel_t)tc_clip(
                (int)pred[row * pred_stride + col] + dc, 0, 255);
#endif
}

void tc_recon_add_dc4x4(const tc_pixel_t *pred, int pred_stride, int dc,
                        tc_pixel_t *dst, int dst_stride)
{
#if TCODEC_NEON
    for (int row = 0; row < 4; row++) {
        uint8_t pa[8] = {0}, outa[8];
        memcpy(pa, pred + row * pred_stride, 4);
        uint16x8_t p16 = vmovl_u8(vld1_u8(pa));
        int32x4_t sum_lo = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_low_u16(p16))), vdupq_n_s32(dc));
        int32x4_t sum_hi = vaddq_s32(
            vreinterpretq_s32_u32(vmovl_u16(vget_high_u16(p16))), vdupq_n_s32(dc));
        sum_lo = vmaxq_s32(vminq_s32(sum_lo, vdupq_n_s32(255)), vdupq_n_s32(0));
        sum_hi = vmaxq_s32(vminq_s32(sum_hi, vdupq_n_s32(255)), vdupq_n_s32(0));
        int16x8_t sum = vcombine_s16(vmovn_s32(sum_lo), vmovn_s32(sum_hi));
        vst1_u8(outa, vqmovun_s16(sum));
        memcpy(dst + row * dst_stride, outa, 4);
    }
#else
    for (int row = 0; row < 4; row++) {
        for (int col = 0; col < 4; col++) {
            int v = (int)pred[row * pred_stride + col] + dc;
            dst[row * dst_stride + col] = (tc_pixel_t)tc_clip(v, 0, 255);
        }
    }
#endif
}


/* ════════════════════════════════════════════════════════════════
 * Lagrange multiplier (RDO)
 *
 * Derived from the quantizer step actually used by this codec rather
 * than from an H.264 QP number (TCodec's QP ladder is ~10 QP steps
 * coarser than H.264's at the same index, so the classic
 * 0.85·2^((qp-12)/3) curve under-estimated λ by ~18×, which made every
 * RD decision far too distortion-biased).
 *
 * H.264 relation:  λ_mode = 0.85·2^((QP-12)/3)  and  Qstep = 2^((QP-4)/6)
 *   ⇒ λ_mode = 0.85·2^(-8/3)·Qstep² ≈ 0.1336·Qstep²
 *
 * We evaluate that directly on tc_qscale(qp) (the unweighted step):
 *   λ = (qscale² · 137) >> 10        (137/1024 = 0.1338)
 *
 * Cost model used by the encoder:  cost = SSE + λ·bits   (integer).
 * Decoder never needs λ — it is encoder-side only and non-normative.
 * ════════════════════════════════════════════════════════════════ */
int tc_lambda(int qp)
{
    int64_t s = tc_qscale(qp);
    int64_t l = (s * s * 137) >> 10;
    if (l < 1) l = 1;
    if (l > (int64_t)1 << 30) l = (int64_t)1 << 30;
    return (int)l;
}
