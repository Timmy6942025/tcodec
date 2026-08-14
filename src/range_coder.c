/*
 * range_coder.c — Arithmetic range coder for TCodec Phase 3
 *
 * Replaces Exp-Golomb coding with context-modeled arithmetic coding
 * when TC_TOOL_ENTROPY_CODED is active.
 *
 * ── Engine ──────────────────────────────────────────────────
 *
 * Both encoder and decoder use 32-bit low registers.
 * The encoder tracks carries via cache_byte/outstanding — the
 * standard approach used by CABAC and other arithmetic coders.
 *
 * Both normalize (shift 1 byte) when range < 2^24.
 *
 * ── Context model ───────────────────────────────────────────
 *
 * Each context is a single uint8_t.
 * Bits 0-5: state index (0-63)
 * Bit 7:    most probable symbol (MPS) value (0 or 1)
 *
 * The state index maps to an LPS probability via lps_prob_table[].
 * State transitions: MPS → state+1 (capped at 63); LPS → next_state_lps[].
 */

#include "tcodec_common.h"
#include <string.h>


/* ── Range coder constants ────────────────────────────────── */

#define RC_MIN_RANGE   (1u << 24)
#define RC_PROB_BITS   8
#define RC_FLUSH_BYTES 5

/* ── LPS probability table (state 0-63 → probability 0-255) ─ */

static const uint8_t lps_prob_table[64] = {
    128, 124, 120, 116, 111, 106, 101,  96,
     91,  86,  81,  76,  71,  66,  62,  58,
     54,  50,  46,  43,  39,  36,  33,  30,
     27,  24,  22,  20,  18,  16,  14,  12,
     10,   9,   8,   7,   6,   5,   4,   4,
      3,   3,   2,   2,   2,   1,   1,   1,
      1,   1,   1,   1,   1,   1,   1,   1,
      1,   1,   1,   1,   1,   1,   1,   1,
};

/* ── LPS state transition table ───────────────────────────── */

static const uint8_t next_state_lps[64] = {
     0,  0,  1,  2,  2,  4,  4,  5,
     6,  7,  8,  9,  9, 11, 11, 12,
    13, 14, 15, 16, 16, 18, 18, 19,
    19, 21, 21, 22, 22, 23, 23, 24,
    24, 25, 26, 26, 27, 27, 28, 28,
    29, 29, 30, 30, 30, 31, 32, 32,
    33, 33, 33, 34, 34, 35, 35, 35,
    36, 36, 36, 37, 37, 37, 38, 38,
};

/* ── Helpers: extract state and MPS from context byte ─────── */

static TCODEC_FORCEINLINE int ctx_state(tc_rc_ctx_t ctx) {
    return (int)(ctx & 0x3F);
}
static TCODEC_FORCEINLINE int ctx_mps(tc_rc_ctx_t ctx) {
    return (ctx >> 7) & 1;
}
static TCODEC_FORCEINLINE tc_rc_ctx_t ctx_make(int mps, int state) {
    return (tc_rc_ctx_t)(((mps & 1) << 7) | (state & 0x3F));
}

/* ══════════════════════════════════════════════════════════════
 *  Encoder (32-bit low — cache_byte/outstanding carry handling)
 * ══════════════════════════════════════════════════════════════ */

void tc_rc_enc_init(tc_rc_enc_t *rc, tc_bs_writer_t *bs)
{
    rc->bs         = bs;
    rc->low        = 0;
    rc->range      = 0xFFFFFFFFu;
    rc->cache_byte = -1;
    rc->outstanding = 0;
}

/* Shift out one byte from low.  Output is deferred for 0xFF
 * bytes that might be affected by a future carry. */
static void rc_enc_shift(tc_rc_enc_t *rc)
{
    uint8_t byte = (uint8_t)(rc->low >> 24);

    if (byte == 0xFF) {
        /* Defer: a future carry could turn this into 0x00 */
        rc->outstanding++;
    } else {
        /* Flush any deferred bytes before resetting cache */
        if (rc->cache_byte >= 0) {
            tc_bs_writer_write_bits(rc->bs, (uint32_t)rc->cache_byte, 8);
        }
        for (int i = 0; i < rc->outstanding; i++)
            tc_bs_writer_write_bits(rc->bs, 0xFFu, 8);
        rc->cache_byte  = (int)byte;
        rc->outstanding = 0;
    }

    rc->low   <<= 8;
    rc->range <<= 8;
}

static void rc_enc_normalize(tc_rc_enc_t *rc)
{
    while (rc->range < RC_MIN_RANGE) rc_enc_shift(rc);
}

void tc_rc_enc_bit(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx, int bit)
{
    int      state = ctx_state(*ctx);
    int      mps   = ctx_mps(*ctx);
    uint32_t prob  = lps_prob_table[state];
    uint32_t rLPS  = (rc->range >> RC_PROB_BITS) * prob;

    if (bit == mps) {
        rc->range -= rLPS;
        if (state < 63) *ctx = ctx_make(mps, state + 1);
    } else {
        /* 32-bit addition with overflow (carry) detection */
        uint32_t old_low = rc->low;
        rc->low   += rc->range - rLPS;
        rc->range  = rLPS;
        uint8_t ns = next_state_lps[state];
        if (state == 0) *ctx = ctx_make(mps ^ 1, ns);
        else            *ctx = ctx_make(mps, ns);

        /* If low overflowed, propagate carry through deferred bytes */
        if (rc->low < old_low) {
            if (rc->cache_byte >= 0) {
                /* Carry increments the last determined byte */
                tc_bs_writer_write_bits(rc->bs,
                    (uint32_t)(rc->cache_byte + 1), 8);
                /* Outstanding 0xFF bytes become 0x00 */
                for (int i = 0; i < rc->outstanding; i++)
                    tc_bs_writer_write_bits(rc->bs, 0x00u, 8);
            }
            rc->cache_byte  = -1;
            rc->outstanding = 0;
        }
    }

    /* Normalize AFTER the bit decision — both encoder and decoder
     * normalize at the same point, keeping arithmetic in lockstep.
     * Decoder init eagerly reads 4 bytes; with enough bits the
     * encoder outputs ≥4 normalization bytes before flush. */
    rc_enc_normalize(rc);
}

void tc_rc_enc_flush(tc_rc_enc_t *rc)
{
    /* Output any deferred bytes first */
    if (rc->cache_byte >= 0) {
        tc_bs_writer_write_bits(rc->bs, (uint32_t)rc->cache_byte, 8);
    }
    for (int i = 0; i < rc->outstanding; i++)
        tc_bs_writer_write_bits(rc->bs, 0xFFu, 8);
    rc->cache_byte  = -1;
    rc->outstanding = 0;

    /* Flush remaining 5 bytes of the 32-bit low register.
     * The decoder reads 4 bytes eagerly; 5 flush bytes ensure
     * at least 4 deterministic bytes are always available. */
    for (int i = 0; i < RC_FLUSH_BYTES; i++) {
        uint8_t b = (uint8_t)(rc->low >> 24);
        rc->low <<= 8;
        tc_bs_writer_write_bits(rc->bs, (uint32_t)b, 8);
    }
}

/* ── Context-modeled bit writing helpers ───────────────────── */

void tc_rc_enc_bits(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx, int base_ctx,
                    uint32_t val, int nbits)
{
    for (int i = 0; i < nbits; i++) {
        uint32_t bit = (val >> (nbits - 1 - i)) & 1;
        int cidx = base_ctx + i;
        if (cidx >= RC_CTX_MAX) cidx = RC_CTX_MAX - 1;
        tc_rc_enc_bit(rc, &ctx[cidx], (int)bit);
    }
}

uint32_t tc_rc_enc_ue(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx,
                      int base_ctx, uint32_t val)
{
    uint32_t code = val + 1;
    int bits = 0;
    uint32_t tmp = code;
    while (tmp > 0) { bits++; tmp >>= 1; }
    int leading_zeros = bits - 1;

    for (int i = 0; i < leading_zeros; i++) {
        int cidx = base_ctx + i;
        if (cidx >= RC_CTX_MAX) cidx = RC_CTX_MAX - 1;
        tc_rc_enc_bit(rc, &ctx[cidx], 0);
    }
    {
        int cidx = base_ctx + leading_zeros;
        if (cidx >= RC_CTX_MAX) cidx = RC_CTX_MAX - 1;
        tc_rc_enc_bit(rc, &ctx[cidx], 1);
    }
    for (int i = 0; i < leading_zeros; i++) {
        uint32_t bit = (code >> (leading_zeros - 1 - i)) & 1;
        int cidx = base_ctx + leading_zeros + 1 + i;
        if (cidx >= RC_CTX_MAX) cidx = RC_CTX_MAX - 1;
        tc_rc_enc_bit(rc, &ctx[cidx], (int)bit);
    }
    return (uint32_t)(leading_zeros * 2 + 1);
}

/* ── Coefficient encoding (context-modeled) ───────────────── */

void tc_rc_enc_coeffs(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx,
                      const tc_coeff_t *coeffs, int n,
                      tc_block_size_t dct_size)
{
    (void)dct_size;

    int last_nz = -1;
    for (int i = n - 1; i >= 0; i--) {
        if (coeffs[i] != 0) { last_nz = i; break; }
    }

    if (last_nz < 0) {
        tc_rc_enc_bit(rc, &ctx[RC_CTX_LAST], 0);
        return;
    }

    /* Signal that coefficients exist */
    tc_rc_enc_bit(rc, &ctx[RC_CTX_LAST], 1);

    /* Truncated unary prefix + EG suffix for last_nz */
    int prefix = (last_nz < 4) ? last_nz : 4;
    for (int i = 0; i < prefix; i++) {
        int cidx = RC_CTX_LAST + (i % 4);
        tc_rc_enc_bit(rc, &ctx[cidx], 0);
    }
    if (last_nz < 4) {
        int cidx = RC_CTX_LAST + (prefix % 4);
        tc_rc_enc_bit(rc, &ctx[cidx], 1);
    } else {
        tc_rc_enc_ue(rc, ctx, RC_CTX_LAST, (uint32_t)(last_nz - 4));
    }

    /* Coefficients in reverse order */
    int gt1_count = 0;
    for (int i = last_nz; i >= 0; i--) {
        int c = coeffs[i];
        int is_dc = (i == 0);
        int cidx_sig = is_dc ? RC_CTX_SIG_DC : RC_CTX_SIG + (i * 8 / n);
        if (cidx_sig >= RC_CTX_MAX) cidx_sig = RC_CTX_MAX - 1;

        if (c == 0) {
            tc_rc_enc_bit(rc, &ctx[cidx_sig], 0);
            continue;
        }

        tc_rc_enc_bit(rc, &ctx[cidx_sig], 1);

        int mag = tc_abs(c);

        /* GT1 flag */
        int cidx_gt1 = is_dc ? RC_CTX_GT1_DC : RC_CTX_GT1 + tc_min(gt1_count, 5);
        tc_rc_enc_bit(rc, &ctx[cidx_gt1], (mag > 1) ? 1 : 0);

        if (mag > 1) {
            int cidx_gt2 = is_dc ? RC_CTX_GT2_DC : RC_CTX_GT2 + tc_min(gt1_count, 1);
            tc_rc_enc_bit(rc, &ctx[cidx_gt2], (mag > 2) ? 1 : 0);

            if (mag > 2) {
                tc_rc_enc_ue(rc, ctx, is_dc ? RC_CTX_LEVEL_DC : RC_CTX_LEVEL, (uint32_t)(mag - 3));
            }
            gt1_count++;
            if (gt1_count > 5) gt1_count = 5;
        }

        /* Sign bit */
        int sign_bit = (c < 0) ? 1 : 0;
        tc_rc_enc_bit(rc, &ctx[is_dc ? RC_CTX_SIGN_DC : RC_CTX_SIGN], sign_bit);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Decoder (32-bit sliding window — reads bytes from stream)
 * ══════════════════════════════════════════════════════════════ */

void tc_rc_dec_init(tc_rc_dec_t *rc, tc_bs_reader_t *bs)
{
    rc->bs    = bs;
    rc->low   = 0;
    rc->range = 0xFFFFFFFFu;
    /* Eagerly read the first 4 bytes of the compressed stream.
     * In arithmetic coding the decoder's low register must contain
     * the compressed value before any bit decisions — otherwise
     * low=0 always compares < range-rLPS (always MPS).
     * These bytes were output by the encoder during normalization
     * of the first few bits (or from flush at end of stream). */
    for (int i = 0; i < 4; i++) {
        if (!tc_bs_reader_eof(rc->bs))
            rc->low = (rc->low << 8) | tc_bs_reader_read_bits(rc->bs, 8);
        else
            rc->low = rc->low << 8;
    }
}

static void rc_dec_normalize(tc_rc_dec_t *rc)
{
    while (rc->range < RC_MIN_RANGE) {
        rc->low   = (rc->low << 8) & 0xFFFFFFFFu;
        rc->range = (rc->range == 0) ? 0xFF : (rc->range << 8);
        if (!tc_bs_reader_eof(rc->bs))
            rc->low |= tc_bs_reader_read_bits(rc->bs, 8);
    }
}

static TCODEC_FORCEINLINE int rc_dec_bit_core(tc_rc_dec_t *rc,
                                                tc_rc_ctx_t *ctx)
{
    int state = ctx_state(*ctx);
    int mps = ctx_mps(*ctx);
    uint32_t rLPS = (rc->range >> RC_PROB_BITS) * lps_prob_table[state];
    int bit;
    if (rc->low < rc->range - rLPS) {
        rc->range -= rLPS;
        bit = mps;
        if (state < 63) *ctx = ctx_make(mps, state + 1);
    } else {
        rc->low -= rc->range - rLPS;
        rc->range = rLPS;
        bit = mps ^ 1;
        uint8_t ns = next_state_lps[state];
        *ctx = ctx_make(state == 0 ? (mps ^ 1) : mps, ns);
    }
    rc_dec_normalize(rc);
    return bit;
}

int tc_rc_dec_bit(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx)
{
    return rc_dec_bit_core(rc, ctx);
}

/* ── Context-modeled bit reading helpers ──────────────────── */

uint32_t tc_rc_dec_bits(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx,
                        int base_ctx, int nbits)
{
    uint32_t val = 0;
    for (int i = 0; i < nbits; i++) {
        int cidx = base_ctx + i;
        if (cidx >= RC_CTX_MAX) cidx = RC_CTX_MAX - 1;
        val = (val << 1) | (uint32_t)tc_rc_dec_bit(rc, &ctx[cidx]);
    }
    return val;
}

uint32_t tc_rc_dec_ue(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx, int base_ctx)
{
    int leading_zeros = 0;
    while (1) {
        int cidx = base_ctx + leading_zeros;
        if (cidx >= RC_CTX_MAX) cidx = RC_CTX_MAX - 1;
        if (tc_rc_dec_bit(rc, &ctx[cidx]) == 1) break;
        leading_zeros++;
        if (leading_zeros > 31) return 0;
    }
    uint32_t suffix = 0;
    for (int i = 0; i < leading_zeros; i++) {
        int cidx = base_ctx + leading_zeros + 1 + i;
        if (cidx >= RC_CTX_MAX) cidx = RC_CTX_MAX - 1;
        suffix = (suffix << 1) | (uint32_t)tc_rc_dec_bit(rc, &ctx[cidx]);
    }
    return (1u << leading_zeros) - 1 + suffix;
}

/* ── Coefficient decoding (context-modeled) ───────────────── */

void tc_rc_dec_coeffs(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx,
                      tc_coeff_t *coeffs, int n,
                      tc_block_size_t dct_size)
{
    (void)dct_size;
    memset(coeffs, 0, (size_t)n * sizeof(tc_coeff_t));

    /* Keep the context base local in this hot routine.  More importantly,
     * use the force-inlined core below instead of an out-of-line public
     * call for every significance/level/sign symbol; the arithmetic state
     * machine and context mutation remain exactly unchanged. */
    tc_rc_ctx_t *cbase = ctx;
    if (rc_dec_bit_core(rc, &cbase[RC_CTX_LAST]) == 0) return;

    int last_nz = 0, found = 0;
    for (int i = 0; i < 4; i++) {
        int cidx = RC_CTX_LAST + (i % 4);
        if (rc_dec_bit_core(rc, &cbase[cidx]) == 1) {
            last_nz = i;
            found = 1;
            break;
        }
    }
    if (!found) {
        uint32_t extra = tc_rc_dec_ue(rc, cbase, RC_CTX_LAST);
        if (extra > (uint32_t)n) {
            rc->bs->error = 1;
            return;
        }
        last_nz = 4 + (int)extra;
    }
    if (last_nz < 0 || last_nz >= n) {
        rc->bs->error = 1;
        return;
    }

    int gt1_count = 0;
    for (int i = last_nz; i >= 0; i--) {
        int is_dc = (i == 0);
        int cidx_sig = is_dc ? RC_CTX_SIG_DC : RC_CTX_SIG + (i * 8 / n);
        if (cidx_sig >= RC_CTX_MAX) cidx_sig = RC_CTX_MAX - 1;

        if (!rc_dec_bit_core(rc, &cbase[cidx_sig])) { coeffs[i] = 0; continue; }

        int cidx_gt1 = is_dc ? RC_CTX_GT1_DC : RC_CTX_GT1 + tc_min(gt1_count, 5);
        int gt1 = rc_dec_bit_core(rc, &cbase[cidx_gt1]);
        int mag;

        if (!gt1) {
            mag = 1;
        } else {
            int cidx_gt2 = is_dc ? RC_CTX_GT2_DC : RC_CTX_GT2 + tc_min(gt1_count, 1);
            int gt2 = rc_dec_bit_core(rc, &cbase[cidx_gt2]);
            if (!gt2) {
                mag = 2;
            } else {
                mag = 3 + (int)tc_rc_dec_ue(rc, cbase, is_dc ? RC_CTX_LEVEL_DC : RC_CTX_LEVEL);
            }
            gt1_count++;
            if (gt1_count > 5) gt1_count = 5;
        }

        if (mag > 32767) {
            rc->bs->error = 1;
            return;
        }
        int sign = rc_dec_bit_core(rc, &cbase[is_dc ? RC_CTX_SIGN_DC : RC_CTX_SIGN]);
        coeffs[i] = (tc_coeff_t)(sign ? -mag : mag);
    }
}

/* ── Context initialization ────────────────────────────────── */

void tc_rc_ctx_init(tc_rc_ctx_t *ctx, int num_ctx)
{
    memset(ctx, 0, (size_t)num_ctx * sizeof(tc_rc_ctx_t));
}

void tc_rc_ctx_copy(tc_rc_ctx_t *dst, const tc_rc_ctx_t *src, int num_ctx)
{
    memcpy(dst, src, (size_t)num_ctx * sizeof(tc_rc_ctx_t));
}
