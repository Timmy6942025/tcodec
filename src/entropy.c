/*
 * entropy.c — Entropy coding for TCodec
 *
 * Uses Exp-Golomb + sign-bit coding for transform coefficients.
 * Context model is maintained for future ANS optimization.
 *
 * Coefficient coding:
 *  1. Code last_nonzero position via Exp-Golomb
 *  2. For each coefficient in reverse zigzag:
 *     a. Code magnitude via Exp-Golomb with context
 *     b. Code sign bit (raw)
 *
 * This is simpler than tANS but guaranteed correct with linear
 * bitstream reading. tANS can be added back later with proper
 * reverse-bitstream buffering.
 */

#include "tcodec_common.h"
#include <string.h>
/* Note: <math.h> was removed — it was only needed by the deleted ctx_init() */

/* ── Context model (reserved for future ANS use) ──────────── */
/* tc_tans_ctx_t is defined in tcodec_types.h but NOT allocated in
 * tc_tans_enc_t / tc_tans_dec_t (saves ~40KB per instance).
 * When ANS is integrated, contexts will be added back. */

/* ── Context selection (removed — was dead code) ────────────── */
/* tc_coeff_context() was only called by the removed tc_tans_enc_symbol
 * and tc_tans_dec_symbol functions. It will be reimplemented when
 * context-based ANS coding is integrated. */

/* ── Encoder init/flush ──────────────────────────────────────── */

void tc_tans_enc_init(tc_tans_enc_t *e, tc_bs_writer_t *bs)
{
    e->bs    = bs;
    e->state = 0;  /* Reserved for future ANS state */
}

void tc_tans_enc_flush(tc_tans_enc_t *e)
{
    /* No state to flush with Exp-Golomb coding */
    (void)e;
}

/* ── Single symbol encode (removed — was dead code) ─────────── */
/* tc_tans_enc_symbol has been removed. It maintained context adaptation
 * that was never consumed by the actual coefficient coding path
 * (tc_tans_enc_coeffs uses tc_bs_writer_write_ue directly).
 * This wasted ~40KB per encoder instance. When ANS is integrated,
 * context-based symbol coding will be reimplemented. */

/* ── Decoder init ────────────────────────────────────────────── */

void tc_tans_dec_init(tc_tans_dec_t *d, tc_bs_reader_t *bs)
{
    d->bs = bs;
    d->state = 0;  /* Reserved for future ANS state */
}

/* ── Single symbol decode (removed — was dead code) ─────────── */
/* tc_tans_dec_symbol has been removed. It maintained context adaptation
 * that was never consumed by the actual coefficient coding path
 * (tc_tans_dec_coeffs uses tc_bs_reader_read_ue directly).
 * This wasted ~40KB per decoder instance. */

/* ── Frequency band classification (reserved for future use) ── */
/* Currently quantize/dequantize always pass band=0. When per-coefficient
 * JND weighting is integrated, this will classify each zigzag position
 * into DC/low/mid/high frequency bands. */

int tc_freq_band(int pos, int blk_size)
{
    int total = blk_size * blk_size;
    if (pos == 0) return 0;            /* DC */
    if (pos < total / 4) return 1;     /* Low AC */
    if (pos < total / 2) return 2;     /* Mid AC */
    return 3;                           /* High AC */
}

/* ── Zigzag scan orders ──────────────────────────────────────── */

static const int zigzag4[16] = {
     0,  1,  4,  8,  5,  2,  3,  6,
     9, 12, 13, 10,  7, 11, 14, 15
};

static const int zigzag8[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

/* ── Coefficient encoding ──────────────────────────────────────
 *
 * Encode coefficients using:
 *  1. Last non-zero position (Exp-Golomb)
 *  2. Per-coefficient: magnitude (Exp-Golomb) + sign (1 bit)
 *
 * Magnitude is coded as (abs(coeff)) so 0 means coefficient=0.
 * Sign bit follows only for non-zero coefficients.
 * ══════════════════════════════════════════════════════════════ */

void tc_tans_enc_coeffs(tc_tans_enc_t *e, const tc_coeff_t *coeffs, int n,
                         tc_block_size_t dct_size)
{
    const int *zz = (dct_size == TC_BLOCK_4x4_ID) ? zigzag4 : zigzag8;

    /* Find last non-zero coefficient in zigzag order */
    int last_nz = -1;
    for (int i = n - 1; i >= 0; i--) {
        if (coeffs[zz[i]] != 0) {
            last_nz = i;
            break;
        }
    }

    /* Code last_nonzero position + 1 (0 = all zero block) */
    tc_bs_writer_write_ue(e->bs, (uint32_t)(last_nz + 1));

    if (last_nz < 0) return;   /* All zero block */

    /* Code coefficients in reverse zigzag order */
    for (int i = last_nz; i >= 0; i--) {
        int pos = zz[i];
        int c = coeffs[pos];
        int mag = tc_abs(c);

        /* Encode magnitude as Exp-Golomb */
        tc_bs_writer_write_ue(e->bs, (uint32_t)mag);

        /* Sign bit (only for non-zero) */
        if (mag > 0) {
            tc_bs_writer_write_bits(e->bs, (c < 0) ? 1 : 0, 1);
        }
    }
}

/* ── Coefficient decoding ────────────────────────────────────── */

void tc_tans_dec_coeffs(tc_tans_dec_t *d, tc_coeff_t *coeffs, int n,
                         tc_block_size_t dct_size)
{
    const int *zz = (dct_size == TC_BLOCK_4x4_ID) ? zigzag4 : zigzag8;

    /* Zero all coefficients first */
    memset(coeffs, 0, (size_t)n * sizeof(tc_coeff_t));

    /* Read last_nonzero position */
    uint32_t last_nz_plus1 = tc_bs_reader_read_ue(d->bs);
    if (last_nz_plus1 == 0) {
        return;  /* All zero block */
    }
    if (last_nz_plus1 > (uint32_t)n) {
        d->bs->error = 1;
        return;
    }
    int last_nz = (int)last_nz_plus1 - 1;

    /* Decode coefficients in reverse zigzag */
    for (int i = last_nz; i >= 0; i--) {
        int pos = zz[i];

        /* Read magnitude */
        uint32_t mag = tc_bs_reader_read_ue(d->bs);

        if (mag > 0) {
            if (mag > 32767u) {
                d->bs->error = 1;
                return;
            }
            /* Read sign bit */
            uint32_t sign = tc_bs_reader_read_bits(d->bs, 1);
            coeffs[pos] = (tc_coeff_t)(sign ? -(int)mag : (int)mag);
        }
        /* else coeffs[pos] already zero from memset */
    }
}
