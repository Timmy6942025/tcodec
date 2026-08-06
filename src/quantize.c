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


/* ════════════════════════════════════════════════════════════════
 * Lagrange multiplier (RDO-lite / D7)
 *
 * Integer-only approximation of the H.264-style lambda curve:
 *   lambda = 0.85 · 2^((qp-12)/3)
 * returned as a Q8 fixed-point value (<< 8). Used by the encoder
 * for mode/transform decisions: cost = SSE + lambda·rate.
 * Decoder never needs this — it is encoder-side only.
 * ════════════════════════════════════════════════════════════════ */
int tc_lambda(int qp)
{
    int e = (qp - 12) / 3;
    if (e < 0) e = 0;
    if (e > 30) e = 30;
    return (217 << e) >> 8;   /* 0.85 << 8, then 2^e */
}
