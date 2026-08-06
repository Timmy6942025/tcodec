/*
 * encoder.c — Encoding pipeline for TCodec
 *
 * Pipeline:
 *   1. Frame header write
 *   2. For each CTU (with WPP parallelism):
 *      a. Intra prediction (18 modes, SAD-select best)
 *      b. Motion estimation (hierarchical hex search, inter frames)
 *      c. Mode decision (intra vs inter vs skip, simplified RDO)
 *      d. Forward DCT → Quantize → tANS encode
 *      e. Inverse quantize → Inverse DCT → Reconstruct
 *      f. Deblocking filter
 *   3. Rate control feedback
 */

#include "tcodec_common.h"
#include <stdlib.h>
#include <string.h>

/* Forward declaration — defined later in this file */
void tc_encoder_destroy(tc_encoder_t *enc);

/* ── Dual-path encoding helpers (raw bits vs range coder) ─────
 *
 * When rc!=NULL (TC_TOOL_ENTROPY_CODED active), use context-modeled
 * range coder. When rc==NULL, use raw bitstream writes.
 * This avoids duplicating encode_block for the two paths.
 * ══════════════════════════════════════════════════════════════ */

static TCODEC_FORCEINLINE void enc_write_bits(
    tc_bs_writer_t *bs, tc_rc_enc_t *rc, tc_rc_ctx_t *ctx,
    int base_ctx, uint32_t val, int nbits)
{
    if (rc) tc_rc_enc_bits(rc, ctx, base_ctx, val, nbits);
    else    tc_bs_writer_write_bits(bs, val, nbits);
}

static TCODEC_FORCEINLINE void enc_write_se(
    tc_bs_writer_t *bs, tc_rc_enc_t *rc, tc_rc_ctx_t *ctx,
    int base_ctx, int32_t val)
{
    if (rc) {
        /* Map signed to unsigned, then context-coded EG */
        uint32_t mapped;
        if (val > 0)       mapped = (uint32_t)(2 * val - 1);
        else if (val < 0)  mapped = (uint32_t)(-2 * val);
        else               mapped = 0;
        tc_rc_enc_ue(rc, ctx, base_ctx, mapped);
    } else {
        tc_bs_writer_write_se(bs, val);
    }
}

static TCODEC_FORCEINLINE void enc_write_coeffs(
    tc_bs_writer_t *bs, tc_tans_enc_t *tans,
    tc_rc_enc_t *rc, tc_rc_ctx_t *rc_ctx,
    const tc_coeff_t *coeffs, int n, tc_block_size_t dct_size)
{
    if (rc) tc_rc_enc_coeffs(rc, rc_ctx, coeffs, n, dct_size);
    else    tc_tans_enc_coeffs(tans, coeffs, n, dct_size);
    (void)bs;
}

/* ── Variance-based DCT size selection ─────────────────────────
 *
 * For each 8×8 block, compute variance:
 *   High variance → 4×4 DCT (preserve detail)
 *   Low variance  → 8×8 DCT (better energy compaction)
 *
 * Threshold tuned for 20:1 compression target.
 * ══════════════════════════════════════════════════════════════ */

#define VARIANCE_THRESHOLD 512   /* Above this → 4×4 DCT */

static int block_variance(const tc_pixel_t *y, int stride, int blk_size)
{
    int32_t sum = 0, sum_sq = 0;
    int n = blk_size * blk_size;

    for (int y0 = 0; y0 < blk_size; y0++) {
        for (int x0 = 0; x0 < blk_size; x0++) {
            int v = y[y0 * stride + x0];
            sum += v;
            sum_sq += v * v;
        }
    }

    /* variance = (sum_sq - sum^2/n) / n = E[X^2] - E[X]^2 */
    int64_t mean = sum / n;
    int64_t var = sum_sq / n - mean * mean;
    return (int)var;
}

/* ── Scene cut detection ───────────────────────────────────────
 *
 * Compare current frame histogram against previous frame.
 * Large histogram change indicates a scene cut → force keyframe.
 * Uses a simple chi-squared distance on 16-bin luma histograms.
 * ══════════════════════════════════════════════════════════════ */

#define HIST_BINS 16
#define SCENE_CUT_THRESHOLD 0.5

static double histogram_distance(const tc_pixel_t *cur, int cur_stride,
                                  const tc_pixel_t *prev, int prev_stride,
                                  int width, int height)
{
    int hist_cur[HIST_BINS] = {0};
    int hist_prev[HIST_BINS] = {0};
    int total = width * height;

    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            int bin_c = cur[row * cur_stride + col] * HIST_BINS / 256;
            int bin_p = prev[row * prev_stride + col] * HIST_BINS / 256;
            if (bin_c >= HIST_BINS) bin_c = HIST_BINS - 1;
            if (bin_p >= HIST_BINS) bin_p = HIST_BINS - 1;
            hist_cur[bin_c]++;
            hist_prev[bin_p]++;
        }
    }

    double dist = 0.0;
    for (int i = 0; i < HIST_BINS; i++) {
        double expected = (hist_prev[i] + hist_cur[i]) / 2.0;
        if (expected > 0) {
            double diff = (double)hist_cur[i] - (double)hist_prev[i];
            dist += (diff * diff) / expected;
        }
    }
    return dist / (double)total;
}

/* ── Encode one 8×8 block ───────────────────────────────────── */

static void encode_block(tc_encoder_t *enc, tc_ctu_info_t *ctu,
                         int blk_idx, int bx, int by,
                         int frame_x, int frame_y,
                         int qp, tc_frame_type_t frame_type,
                         tc_bs_writer_t *bs, tc_tans_enc_t *tans,
                         tc_rc_enc_t *rc, tc_rc_ctx_t *rc_ctx)
{
    tc_block_info_t *blk = &ctu->blocks[blk_idx];

    /* Always process 8×8 blocks for prediction/residual. */
    int blk_size = 8;
    int n_coeff  = 64;

    tc_pixel_t orig_block[64];    /* Max 8×8 */
    tc_pixel_t pred_block[64];
    tc_coeff_t residual[64];
    tc_coeff_t dct_out[64];

    /* Extract original block from current frame */
    for (int y0 = 0; y0 < blk_size; y0++) {
        memcpy(orig_block + y0 * blk_size,
               enc->cur->y + (frame_y + y0) * enc->cur->stride_y + frame_x,
               (size_t)blk_size);
    }

    /* ── Intra prediction ──────────────────────────────────── */
    tc_pixel_t ref_above[32 + 1];  /* Max 2×8 + 1 */
    tc_pixel_t ref_left[32 + 1];

    tc_intra_get_ref(enc->recon->y, enc->recon->stride_y,
                     frame_x, frame_y, blk_size,
                     enc->cfg.width, enc->cfg.height,
                     ref_above + 1, ref_left + 1);

    tc_intra_mode_t best_intra = TC_INTRA_DC;
    tc_sad_t best_intra_sad = 0x7FFFFFFF;

    /* Try all 18 intra modes */
    for (int m = 0; m < TC_INTRA_MODES; m++) {
        tc_pixel_t tmp_pred[64];
        tc_intra_predict(tmp_pred, blk_size,
                         ref_above + 1, ref_left + 1,
                         blk_size, (tc_intra_mode_t)m);
        tc_sad_t sad = tc_intra_cost(orig_block, blk_size, tmp_pred, blk_size);
        if (sad < best_intra_sad) {
            best_intra_sad = sad;
            best_intra = (tc_intra_mode_t)m;
        }
    }

    blk->intra_mode = best_intra;
    blk->is_intra = 1;  /* Default: intra */

    /* Re-generate best prediction */
    tc_intra_predict(pred_block, blk_size,
                     ref_above + 1, ref_left + 1,
                     blk_size, best_intra);

    /* ── Inter prediction (P-frames only) ────────────────────
     * Search multiple reference frames and pick the best.
     * Also computes merge_mv (median of spatial neighbors) for merge mode.
     * ref_idx is signaled in the bitstream for multi-ref support. */
    tc_sad_t best_inter_sad = 0x7FFFFFFF;
    tc_mv_s  best_mv = {0, 0};
    tc_pixel_t inter_pred[64];
    int best_ref_idx = 0;

    /* Merge MV: median of spatial neighbors (used for merge mode
     * and as ME search center). Computed once, reused for all refs. */
    tc_mv_s  merge_mv = {frame_x * 4, frame_y * 4};  /* Default: collocated */
    tc_sad_t merge_sad = 0x7FFFFFFF;
    int merge_available = 0;

    if (frame_type != TC_FRAME_KEY) {
        /* Compute median MV predictor from spatial neighbors.
         * Used as ME search center AND as merge mode candidate MV. */
        if (blk_idx > 0) {
            tc_mv_s mv_a = {0,0}, mv_b = {0,0}, mv_c = {0,0};
            int have_a = 0, have_b = 0, have_c = 0;
            if (bx > 0) {
                tc_block_info_t *left = &ctu->blocks[blk_idx - 1];
                if (!left->is_intra) { mv_a = left->mv; have_a = 1; }
            }
            if (by > 0) {
                tc_block_info_t *above = &ctu->blocks[blk_idx - 8];
                if (!above->is_intra) { mv_b = above->mv; have_b = 1; }
            }
            if (by > 0 && bx < 7) {
                tc_block_info_t *ar = &ctu->blocks[blk_idx - 7];
                if (!ar->is_intra) { mv_c = ar->mv; have_c = 1; }
            }
            if (have_a || have_b || have_c) {
                if (!have_a) mv_a = have_b ? mv_b : mv_c;
                if (!have_b) mv_b = have_a ? mv_a : mv_c;
                if (!have_c) mv_c = have_a ? mv_a : mv_b;
                int px, py;
                { int a=mv_a.x,b=mv_b.x,c=mv_c.x; if(a>b){int t=a;a=b;b=t;} if(b>c){int t=b;b=c;c=t;} if(a>b){int t=a;a=b;b=t;} px=b; }
                { int a=mv_a.y,b=mv_b.y,c=mv_c.y; if(a>b){int t=a;a=b;b=t;} if(b>c){int t=b;b=c;c=t;} if(a>b){int t=a;a=b;b=t;} py=b; }
                merge_mv.x = px;
                merge_mv.y = py;
                merge_available = 1;
            }
        }

        /* Search range depends on preset */
        int search_range = 32;
        if (enc->cfg.preset == TC_PRESET_ULTRAFAST)  search_range = 16;
        if (enc->cfg.preset == TC_PRESET_SLOW)       search_range = 64;

        /* Try each reference frame in DPB.
         * Only search multiple refs on SLOW preset AND streaming-main+ profile.
         * Baseline-mobile profile must never produce multi-ref bitstreams —
         * a baseline-mobile decoder wouldn't know how to handle ref_idx. */
        int max_refs = (enc->cfg.preset == TC_PRESET_SLOW &&
                        enc->cfg.profile >= TC_PROFILE_STREAMING_MAIN) ? TC_REF_FRAMES : 1;
        for (int ri = 0; ri < max_refs; ri++) {
            const tc_frame_buf_t *rframe = enc->dpb[ri].frame;
            if (!rframe) continue;

            /* Search center from merge MV (median predictor) */
            int center_x = tc_clip(merge_mv.x / 4, 0, rframe->width - blk_size);
            int center_y = tc_clip(merge_mv.y / 4, 0, rframe->height - blk_size);

            tc_sad_t ref_sad;
            tc_mv_s ref_mv = tc_motion_est(rframe->y, rframe->stride_y,
                                            rframe->width, rframe->height,
                                            orig_block, blk_size,
                                            center_x, center_y,
                                            blk_size, search_range,
                                            &ref_sad);

            if (ref_sad < best_inter_sad) {
                best_inter_sad = ref_sad;
                best_mv = ref_mv;
                best_ref_idx = ri;
            }
        }

        /* Generate inter prediction from best reference */
        if (best_inter_sad < best_intra_sad) {
            const tc_frame_buf_t *best_ref = enc->dpb[best_ref_idx].frame;
            tc_inter_predict(best_ref->y, best_ref->stride_y,
                             best_ref->width, best_ref->height,
                             best_mv, inter_pred, blk_size, blk_size);
            blk->is_intra = 0;
            blk->mv = best_mv;
            blk->ref_idx = (uint8_t)best_ref_idx;
            memcpy(pred_block, inter_pred, (size_t)n_coeff * sizeof(tc_pixel_t));
        }

        /* Compute merge SAD: prediction quality at the merge MV position.
         * This determines whether merge mode (implicit MV, no MVD/ref_idx)
         * is a good choice — significant bitrate savings when the median
         * predictor is accurate. */
        if (merge_available) {
            const tc_frame_buf_t *rframe = enc->dpb[0].frame;
            if (rframe) {
                int mx = merge_mv.x >> 2;
                int my = merge_mv.y >> 2;
                if (mx >= 0 && my >= 0 &&
                    mx + blk_size <= rframe->width &&
                    my + blk_size <= rframe->height) {
                    merge_sad = tc_sad(rframe->y + my * rframe->stride_y + mx,
                                       rframe->stride_y,
                                       orig_block, blk_size, blk_size);
                }
            }
        }
    }

    /* ── Compute residual ──────────────────────────────────── */
    for (int i = 0; i < n_coeff; i++) {
        residual[i] = (tc_coeff_t)((int)orig_block[i] - (int)pred_block[i]);
    }

    /* ── Variance-based DCT size selection ────────────────── */
    int dct_size_id;
    {
        int var = block_variance(orig_block, blk_size, blk_size);
        dct_size_id = (var > VARIANCE_THRESHOLD) ? TC_BLOCK_4x4_ID : TC_BLOCK_8x8_ID;
    }
    blk->dct_size = (tc_block_size_t)dct_size_id;

    /* ── Forward transform + quantize (RDO-lite: WHT vs DCT-II) ─
     * Both candidates are forward-transformed and quantized with
     * identical JND band weighting. Cost is PIXEL-domain SSE
     * (dequant + inverse transform + prediction vs original) plus
     * λ·rate — the honest RDO-lite cost (D5/D7), so the transform
     * choice maximizes real reconstruction quality per bit.
     * transform_id bits: bit0 = type (0 WHT, 1 DCT), bit1 = size
     * (0 4×4, 1 8×8) — legacy WHT ids 0/1 map to the old 4×4/8×8. */
    int transform_type = 0;
    int transform_id = 0;
    int all_zero = 1;  /* Track if all quantized coefficients are zero */
    {
        tc_coeff_t winner_cand[64];
        long long best_cost = (long long)1 << 62;
        int best_nz = 0;
        int qp_lambda = tc_lambda(qp);

        for (int t = 0; t < 2; t++) {          /* 0=WHT, 1=DCT-II */
            tc_coeff_t cand[64], dq[64];
            int nz = 0;
            long long dist = 0;

            if (dct_size_id == TC_BLOCK_4x4_ID) {
                for (int sy = 0; sy < 2; sy++) {
                    for (int sx = 0; sx < 2; sx++) {
                        tc_coeff_t sub_in[16], sub_out[16], sub_dq[16], rec[16];
                        for (int r = 0; r < 4; r++)
                            for (int c = 0; c < 4; c++)
                                sub_in[r * 4 + c] = residual[(sy * 4 + r) * 8 + (sx * 4 + c)];
                        if (t == 0) tc_fwht4x4(sub_in, 4, sub_out);
                        else        tc_fdct4x4_res(sub_in, 4, sub_out);
                        for (int i = 0; i < 16; i++) {
                            int band = tc_freq_band(i, 4);
                            int w = tc_jnd_weight(band, i);
                            int scale = tc_qscale(qp);
                            int eff = (scale * w + 4) >> 3;
                            if (eff < 1) eff = 1;
                            int offset = eff / 3;
                            int c = sub_out[i];
                            int qv = 0;
                            if (c > 0) qv = (c + offset) / eff;
                            else if (c < 0) qv = -((-c + offset) / eff);
                            sub_out[i] = (tc_coeff_t)qv;
                            if (qv != 0) nz++;
                            if (qv > 0) sub_dq[i] = (tc_coeff_t)(qv * eff + (eff >> 1));
                            else if (qv < 0) sub_dq[i] = (tc_coeff_t)(qv * eff - (eff >> 1));
                            else sub_dq[i] = 0;
                        }
                        if (t == 0) tc_iwht4x4(sub_dq, rec, 4);
                        else        tc_idct4x4_res(sub_dq, rec, 4);
                        /* Pixel-domain SSE against original (in-bounds) */
                        for (int r = 0; r < 4; r++) {
                            for (int c = 0; c < 4; c++) {
                                int px = frame_x + sx * 4 + c;
                                int py = frame_y + sy * 4 + r;
                                if (px < enc->recon->width && py < enc->recon->height) {
                                    int val = (int)pred_block[(sy * 4 + r) * blk_size + (sx * 4 + c)]
                                              + (int)rec[r * 4 + c];
                                    long long d = (long long)orig_block[(sy * 4 + r) * blk_size + (sx * 4 + c)]
                                                  - (long long)tc_clip(val, 0, 255);
                                    dist += d * d;
                                }
                            }
                        }
                        for (int r = 0; r < 4; r++)
                            for (int c = 0; c < 4; c++)
                                cand[(sy * 4 + r) * 8 + (sx * 4 + c)] = sub_out[r * 4 + c];
                    }
                }
            } else {
                tc_coeff_t rec[64];
                if (t == 0) tc_fwht8x8(residual, 8, cand);
                else        tc_fdct8x8_res(residual, 8, cand);
                for (int i = 0; i < 64; i++) {
                    int band = tc_freq_band(i, 8);
                    int w = tc_jnd_weight(band, i);
                    int scale = tc_qscale(qp);
                    int eff = (scale * w + 4) >> 3;
                    if (eff < 1) eff = 1;
                    int offset = eff / 3;
                    int c = cand[i];
                    int qv = 0;
                    if (c > 0) qv = (c + offset) / eff;
                    else if (c < 0) qv = -((-c + offset) / eff);
                    cand[i] = (tc_coeff_t)qv;
                    if (qv != 0) nz++;
                    if (qv > 0) dq[i] = (tc_coeff_t)(qv * eff + (eff >> 1));
                    else if (qv < 0) dq[i] = (tc_coeff_t)(qv * eff - (eff >> 1));
                    else dq[i] = 0;
                }
                if (t == 0) tc_iwht8x8(dq, rec, 8);
                else        tc_idct8x8_res(dq, rec, 8);
                for (int r = 0; r < 8; r++) {
                    for (int c = 0; c < 8; c++) {
                        int px = frame_x + c;
                        int py = frame_y + r;
                        if (px < enc->recon->width && py < enc->recon->height) {
                            int val = (int)pred_block[r * blk_size + c] + (int)rec[r * 8 + c];
                            long long d = (long long)orig_block[r * blk_size + c]
                                          - (long long)tc_clip(val, 0, 255);
                            dist += d * d;
                        }
                    }
                }
            }

            /* RDO-lite cost: pixel SSE + λ·rate (rate ∝ non-zero count + flag) */
            long long cost = dist + (long long)qp_lambda * (4LL * nz + 2);
            if (cost < best_cost || (cost == best_cost && t == 0)) {
                best_cost = cost;
                best_nz = nz;
                transform_type = t;
                memcpy(winner_cand, cand, sizeof(cand));
            }
        }

        all_zero = (best_nz == 0);
        transform_id = (dct_size_id == TC_BLOCK_8x8_ID ? 2 : 0) | transform_type;
        memcpy(dct_out, winner_cand, sizeof(tc_coeff_t) * 64);
    }

    /* ── Mode decision: merge > skip > inter > intra ──────────
     * Merge (mode 3): zero residual, MV derived from spatial neighbors.
     *   No ref_idx or MVD signaled — significant bitrate savings.
     * Skip (mode 0): zero residual, MV explicitly signaled via MVD.
     * Inter (mode 1): non-zero residual, MV signaled via MVD.
     * Intra (mode 2): intra prediction, no MV. */
    int is_merge = 0;
    int is_skip = 0;
    if (!blk->is_intra && frame_type != TC_FRAME_KEY) {
        /* Merge: use median predictor MV if it gives good SAD.
         * Prefer merge over skip when merge_sad is competitive because
         * merge saves the ref_idx (2 bits) + MVD (often 5-15 bits). */
        if (all_zero && merge_available && merge_sad < (tc_sad_t)(qp * qp)) {
            is_merge = 1;
        } else if (all_zero) {
            is_skip = 1;
        } else if (best_inter_sad < (tc_sad_t)(qp * qp / 2)) {
            /* Very low SAD even with some residual — skip for compression gain */
            is_skip = 1;
            memset(dct_out, 0, sizeof(tc_coeff_t) * 64);
        }
    }

    /* For merge: override prediction with merge MV (from median predictor) */
    if (is_merge) {
        const tc_frame_buf_t *rframe = enc->dpb[0].frame;
        if (rframe) {
            tc_inter_predict(rframe->y, rframe->stride_y,
                             rframe->width, rframe->height,
                             merge_mv, pred_block, blk_size, blk_size);
        } else {
            /* Safety: dpb[0] should never be NULL on P-frames, but if it is,
             * fill prediction with 128 to avoid stale intra pred data. */
            memset(pred_block, 128, (size_t)n_coeff);
        }
        blk->is_intra = 0;
        blk->mv = merge_mv;
        blk->ref_idx = 0;
    }

    /* ── Write mode decision to bitstream ──────────────────── */
    if (frame_type != TC_FRAME_KEY) {
        /* Mode: 0=skip, 1=inter, 2=intra, 3=merge (2 bits) */
        if (is_merge) {
            enc_write_bits(bs, rc, rc_ctx, RC_CTX_BLOCK_MODE, 3, 2);
        } else if (is_skip) {
            enc_write_bits(bs, rc, rc_ctx, RC_CTX_BLOCK_MODE, 0, 2);
        } else if (blk->is_intra) {
            enc_write_bits(bs, rc, rc_ctx, RC_CTX_BLOCK_MODE, 2, 2);
        } else {
            enc_write_bits(bs, rc, rc_ctx, RC_CTX_BLOCK_MODE, 1, 2);
        }
    }

    if (blk->is_intra && !is_skip && !is_merge) {
        enc_write_bits(bs, rc, rc_ctx, RC_CTX_INTRA_MODE, (uint32_t)blk->intra_mode, 5);
    }

    /* ref_idx + MVD: written for inter/skip only (NOT merge — merge
     * derives MV from spatial neighbors, saving ref_idx + MVD bits). */
    if (!blk->is_intra && !is_merge) {
        /* Write ref_idx (2 bits, supports up to 4 reference frames).
         * Only written for inter/skip blocks on P-frames. */
        if (frame_type != TC_FRAME_KEY) {
            enc_write_bits(bs, rc, rc_ctx, RC_CTX_REF_IDX, (uint32_t)blk->ref_idx, 2);
        }
        /* Write MVD for inter/skip blocks.
         * Skip blocks carry the actual MV — skip means zero residual,
         * not zero MV. The decoder needs the correct MV to produce
         * the right prediction. */
        /* MVD coded relative to median predictor (not collocated).
         * This produces smaller MVDs when spatial neighbors are available,
         * matching the decoder's predictor_mv derivation exactly. */
        int32_t mvd_x = blk->mv.x - merge_mv.x;
        int32_t mvd_y = blk->mv.y - merge_mv.y;
        enc_write_se(bs, rc, rc_ctx, RC_CTX_MVD_X, mvd_x);
        enc_write_se(bs, rc, rc_ctx, RC_CTX_MVD_Y, mvd_y);
    }

    /* ── Reconstruct + encode coefficients ──────────────────── */
    if (is_merge || is_skip) {
        /* Zero residual: copy prediction directly into recon buffer.
         * Must still reconstruct so that subsequent intra prediction,
         * deblocking, and DPB are correct. */
        for (int r = 0; r < blk_size; r++) {
            for (int c = 0; c < blk_size; c++) {
                int px = frame_x + c, py = frame_y + r;
                if (px < enc->recon->width && py < enc->recon->height) {
                    enc->recon->y[py * enc->recon->stride_y + px] = pred_block[r * blk_size + c];
                }
            }
        }
    } else {
        /* Non-skip/non-merge: write DCT flag, encode coefficients, reconstruct */
        enc_write_bits(bs, rc, rc_ctx, RC_CTX_DCT_SIZE, (uint32_t)transform_id, 2);

        /* ── tANS / Range-coder encode coefficients ────────── */
        if (dct_size_id == TC_BLOCK_4x4_ID) {
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    tc_coeff_t sub[16];
                    for (int r = 0; r < 4; r++) {
                        for (int c = 0; c < 4; c++) {
                            sub[r * 4 + c] = dct_out[(sy * 4 + r) * 8 + (sx * 4 + c)];
                        }
                    }
                    enc_write_coeffs(bs, tans, rc, rc_ctx, sub, 16, TC_BLOCK_4x4_ID);
                }
            }
        } else {
            enc_write_coeffs(bs, tans, rc, rc_ctx, dct_out, n_coeff, TC_BLOCK_8x8_ID);
        }

        /* ── Reconstruct (dequantize + IDCT + add prediction) ── */
        if (dct_size_id == TC_BLOCK_4x4_ID) {
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    tc_coeff_t sub[16];
                    for (int r = 0; r < 4; r++) {
                        for (int c = 0; c < 4; c++) {
                            sub[r * 4 + c] = dct_out[(sy * 4 + r) * 8 + (sx * 4 + c)];
                        }
                    }
                    /* Dequantize with per-coefficient JND band weighting */
                    for (int i = 0; i < 16; i++) {
                        int band = tc_freq_band(i, 4);
                        int w = tc_jnd_weight(band, i);
                        int scale = tc_qscale(qp);
                        int eff = (scale * w + 4) >> 3;
                        if (eff < 1) eff = 1;
                        if (sub[i] > 0) sub[i] = (tc_coeff_t)(sub[i] * eff + (eff >> 1));
                        else if (sub[i] < 0) sub[i] = (tc_coeff_t)(sub[i] * eff - (eff >> 1));
                    }
                    tc_coeff_t rec[16];
                    if (transform_type == 0) tc_iwht4x4(sub, rec, 4);
                    else                     tc_idct4x4_res(sub, rec, 4);
                    for (int r = 0; r < 4; r++) {
                        for (int c = 0; c < 4; c++) {
                            int px = frame_x + sx * 4 + c;
                            int py = frame_y + sy * 4 + r;
                            if (px < enc->recon->width && py < enc->recon->height) {
                                int val = (int)pred_block[(sy * 4 + r) * blk_size + (sx * 4 + c)] + (int)rec[r * 4 + c];
                                enc->recon->y[py * enc->recon->stride_y + px] = (tc_pixel_t)tc_clip(val, 0, 255);
                            }
                        }
                    }
                }
            }
        } else {
            /* Dequantize with per-coefficient JND band weighting */
            for (int i = 0; i < 64; i++) {
                int band = tc_freq_band(i, 8);
                int w = tc_jnd_weight(band, i);
                int scale = tc_qscale(qp);
                int eff = (scale * w + 4) >> 3;
                if (eff < 1) eff = 1;
                if (dct_out[i] > 0) dct_out[i] = (tc_coeff_t)(dct_out[i] * eff + (eff >> 1));
                else if (dct_out[i] < 0) dct_out[i] = (tc_coeff_t)(dct_out[i] * eff - (eff >> 1));
            }
            tc_coeff_t rec[64];
            if (transform_type == 0) tc_iwht8x8(dct_out, rec, 8);
            else tc_idct8x8_res(dct_out, rec, 8);
            for (int r = 0; r < 8; r++) {
                for (int c = 0; c < 8; c++) {
                    int px = frame_x + c;
                    int py = frame_y + r;
                    if (px < enc->recon->width && py < enc->recon->height) {
                        int val = (int)pred_block[r * blk_size + c] + (int)rec[r * 8 + c];
                        enc->recon->y[py * enc->recon->stride_y + px] = (tc_pixel_t)tc_clip(val, 0, 255);
                    }
                }
            }
        }
    } /* end else (non-skip/non-merge reconstruct) */

    /* ── Encode chroma (always) ─────────────────────────────── */
    /* Chroma uses 4×4 DCT with chroma-from-luma (CfL) prediction
     * when luma is intra. For inter/skip/merge, use DC prediction. */
    int cx = frame_x / 2;
    int cy = frame_y / 2;
    for (int comp = 0; comp < 2; comp++) {
        tc_pixel_t *chroma_orig = comp == 0 ? enc->cur->cb : enc->cur->cr;
        tc_pixel_t *chroma_recon = comp == 0 ? enc->recon->cb : enc->recon->cr;
        int c_stride = enc->cur->stride_c;

        tc_pixel_t c_orig[16], c_pred[16];
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                c_orig[r * 4 + c] = chroma_orig[(cy + r) * c_stride + (cx + c)];
            }
        }

        /* Chroma prediction */
        if (blk->is_intra && !is_skip && !is_merge) {
            /* CfL (Chroma from Luma): use reconstructed luma to predict chroma.
             * Simple linear model: c_pred = alpha * luma_avg + beta
             * where luma_avg is the average of the corresponding reconstructed
             * luma 4×4 block, and alpha/beta are derived from the DC mode. */
            int luma_sum = 0;
            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    luma_sum += (int)enc->recon->y[(frame_y + r) * enc->recon->stride_y + (frame_x + c)];
                }
            }
            int luma_avg = luma_sum / 16;
            /* DC prediction from chroma reference samples */
            int c_ref_sum = 0;
            int c_ref_count = 0;
            /* Top row */
            if (cy > 0) {
                for (int c = 0; c < 4; c++) {
                    c_ref_sum += (int)chroma_recon[(cy - 1) * c_stride + (cx + c)];
                    c_ref_count++;
                }
            }
            /* Left col */
            if (cx > 0) {
                for (int r = 0; r < 4; r++) {
                    c_ref_sum += (int)chroma_recon[(cy + r) * c_stride + (cx - 1)];
                    c_ref_count++;
                }
            }
            int c_dc = c_ref_count > 0 ? (c_ref_sum + c_ref_count / 2) / c_ref_count : 128;
            /* Blend DC prediction with luma correlation */
            int alpha_shift = 3;  /* alpha ≈ 0.125 (weak correlation) */
            for (int i = 0; i < 16; i++) {
                int luma_val = (int)enc->recon->y[(frame_y + (i / 4)) * enc->recon->stride_y + (frame_x + (i % 4))];
                int cfl = c_dc + ((luma_val - luma_avg) >> alpha_shift);
                c_pred[i] = (tc_pixel_t)tc_clip(cfl, 0, 255);
            }
        } else {
            /* DC prediction for inter/skip blocks */
            for (int i = 0; i < 16; i++) {
                c_pred[i] = 128;
            }
        }

        tc_coeff_t c_res[16], c_dct[16];
        for (int i = 0; i < 16; i++) c_res[i] = (tc_coeff_t)(c_orig[i] - c_pred[i]);

        /* DCT + quantize + encode (residual-mode) */
        tc_fwht4x4(c_res, 4, c_dct);
        tc_quantize(c_dct, 16, tc_clip(qp + 1, 0, 63), 0);
        enc_write_coeffs(bs, tans, rc, rc_ctx, c_dct, 16, TC_BLOCK_4x4_ID);

        /* Reconstruct chroma */
        tc_dequantize(c_dct, 16, tc_clip(qp + 1, 0, 63), 0);
        tc_coeff_t c_rec[16];
        tc_iwht4x4(c_dct, c_rec, 4);
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                int val = (int)c_pred[r * 4 + c] + (int)c_rec[r * 4 + c];
                if (cx + c < enc->recon->width / 2 && cy + r < enc->recon->height / 2) {
                    chroma_recon[(cy + r) * c_stride + (cx + c)] = (tc_pixel_t)tc_clip(val, 0, 255);
                }
            }
        }
    }
}

/* ── Bitstream merge helper ─────────────────────────────────────
 *
 * Merge a row's per-row bitstream buffer into the main output buffer
 * at the bit level. This preserves bit-exact output whether WPP
 * (per-row buffers) or sequential (single buffer) is used.
 * ══════════════════════════════════════════════════════════════ */

static void merge_row_bitstream(tc_bs_writer_t *main_bs,
                                 const uint8_t *row_buf, size_t row_bytes,
                                 int row_bit_pos)
{
    /* Copy full bytes first */
    for (size_t i = 0; i < row_bytes; i++) {
        tc_bs_writer_write_bits(main_bs, row_buf[i], 8);
    }

    /* Copy remaining partial byte (bit_pos bits from the MSB side) */
    if (row_bit_pos > 0) {
        uint8_t partial = row_buf[row_bytes];
        int shift = 8 - row_bit_pos;
        uint8_t bits = partial >> shift;
        tc_bs_writer_write_bits(main_bs, bits, row_bit_pos);
    }
}

/* ── Encode one CTU row (WPP unit) ──────────────────────────── */

typedef struct {
    tc_encoder_t *enc;
    int qp;
    tc_frame_type_t frame_type;
#if !defined(TCODEC_NO_THREADS)
    int use_wpp;       /* 1 = use per-row buffers, 0 = use main buffer */
#endif
    tc_rc_enc_t  *rc;        /* Range coder for this row (NULL if not entropy coded) */
    tc_rc_ctx_t  *rc_ctx;    /* Contexts for this row */
} enc_row_ctx_t;

static void encode_row(void *ctx, int row)
{
    enc_row_ctx_t *rctx = (enc_row_ctx_t *)ctx;
    tc_encoder_t *enc = rctx->enc;
    int qp = rctx->qp;

    /* Select bitstream writer and tANS encoder for this row.
     * When WPP is active, each row uses its own buffer to allow
     * parallel writes. When sequential, all rows share the main buffer.
     *
     * Note: WPP mode resets tANS state per row (standard WPP entry point
     * design, matching HEVC). Currently invisible since tANS is Exp-Golomb,
     * but will cause a small compression regression when real tANS is
     * implemented (Phase 3) — each row re-initializes its probability tables. */
    tc_bs_writer_t *bs;
    tc_tans_enc_t  *tans;
    tc_rc_enc_t    *rc     = rctx->rc;
    tc_rc_ctx_t    *rc_ctx = rctx->rc_ctx;
#if !defined(TCODEC_NO_THREADS)
    if (rctx->use_wpp) {
        bs   = &enc->row_bs[row];
        tans = &enc->row_tans[row];
        /* Zero per-row buffer for deterministic encoding */
        memset(enc->row_buf[row], 0, enc->row_buf_size[row]);
        tc_bs_writer_init(bs, enc->row_buf[row], enc->row_buf_size[row]);
        tc_tans_enc_init(tans, bs);
        /* Init range coder for this row if entropy coded */
        if (rc && rc_ctx) {
            tc_rc_enc_init(rc, bs);
        }
    } else
#endif
    {
        bs   = &enc->bs;
        tans = &enc->tans;
    }

    for (int col = 0; col < enc->num_ctu_cols; col++) {
        int ctu_x = col * TC_CTU_SIZE;
        int ctu_y = row * TC_CTU_SIZE;
        int ctu_idx = row * enc->num_ctu_cols + col;
        tc_ctu_info_t *ctu = &enc->ctu_data[ctu_idx];
        ctu->row = row;
        ctu->col = col;

        /* For each 8×8 block in CTU */
        for (int by = 0; by < TC_CTU_SIZE / 8; by++) {
            for (int bx = 0; bx < TC_CTU_SIZE / 8; bx++) {
                int blk_x = ctu_x + bx * 8;
                int blk_y = ctu_y + by * 8;
                int blk_idx = by * (TC_CTU_SIZE / 8) + bx;

                /* Only process within frame bounds */
                if (blk_x + 8 <= enc->cfg.width && blk_y + 8 <= enc->cfg.height) {
                    /* Encode the block — DCT size flag is now inside encode_block
                     * (written only for non-skip blocks) */
                    encode_block(enc, ctu, blk_idx, bx, by,
                                 blk_x, blk_y,
                                 qp, rctx->frame_type, bs, tans,
                                 rc, rc_ctx);
                }
            }
        }

        /* Deblock this CTU */
        if (ctu_x + TC_CTU_SIZE <= enc->cfg.width &&
            ctu_y + TC_CTU_SIZE <= enc->cfg.height) {
            tc_deblock_ctu(enc->recon->y, enc->recon->stride_y,
                           enc->recon->cb, enc->recon->stride_c,
                           enc->recon->cr, enc->recon->stride_c,
                           ctu_x, ctu_y, qp);
        }
    }

#if !defined(TCODEC_NO_THREADS)
    /* Flush per-row tANS state and byte-align.
     * WPP rows must be byte-aligned so the decoder can locate
     * row boundaries via byte offsets in the entry point table.
     * This makes WPP bitstreams differ from sequential ones
     * (sequential rows are NOT byte-aligned between boundaries),
     * but the TC_FLAG_WPP flag in the header tells the decoder
     * which format to expect. */
    if (rctx->use_wpp) {
        tc_tans_enc_flush(tans);
        tc_bs_writer_byte_align(bs);
    }
#endif
}

/* ── Frame header write ──────────────────────────────────────── */

static void write_frame_header(tc_bs_writer_t *bs, tc_frame_header_t *hdr)
{
    /* Common fields (v0 and v1) */
    tc_bs_writer_write_bits(bs, hdr->magic[0], 8);
    tc_bs_writer_write_bits(bs, hdr->magic[1], 8);
    tc_bs_writer_write_bits(bs, hdr->magic[2], 8);
    tc_bs_writer_write_bits(bs, hdr->version, 8);
    tc_bs_writer_write_bits(bs, hdr->width, 16);
    tc_bs_writer_write_bits(bs, hdr->height, 16);
    tc_bs_writer_write_bits(bs, hdr->flags, 8);
    tc_bs_writer_write_bits(bs, hdr->qp_delta, 8);
    tc_bs_writer_write_bits(bs, hdr->frame_num, 8);

    /* v0: reserved byte; v1: profile_level + tool_flags */
    if (hdr->version == TC_VERSION_V0) {
        tc_bs_writer_write_bits(bs, 0, 8);  /* reserved */
    } else {
        /* v1: profile_level byte = (profile << 4) | level_idx */
        tc_bs_writer_write_bits(bs, hdr->profile_level, 8);
        /* v1: tool_flags (16 bits) */
        tc_bs_writer_write_bits(bs, hdr->tool_flags, 16);
    }

    tc_bs_writer_byte_align(bs);
}

/* ── Encoder create/destroy ──────────────────────────────────── */

tc_encoder_t *tc_encoder_create(const tc_config_t *config)
{
    tc_encoder_t *enc = (tc_encoder_t *)calloc(1, sizeof(tc_encoder_t));
    if (!enc) return NULL;

    enc->cfg = *config;

    /* Allocate frames */
    enc->cur   = tc_frame_alloc(config->width, config->height);
    enc->recon = tc_frame_alloc(config->width, config->height);
    if (!enc->cur || !enc->recon) {
        tc_encoder_destroy(enc);
        return NULL;
    }

    /* CTU grid dimensions */
    enc->num_ctu_cols = (config->width  + TC_CTU_SIZE - 1) / TC_CTU_SIZE;
    enc->num_ctu_rows = (config->height + TC_CTU_SIZE - 1) / TC_CTU_SIZE;

    /* Allocate CTU info */
    enc->ctu_data = (tc_ctu_info_t *)calloc(
        (size_t)enc->num_ctu_cols * enc->num_ctu_rows,
        sizeof(tc_ctu_info_t));

    /* Output buffer (generous initial size) */
    enc->out_buf_size = (size_t)config->width * config->height * 3;  /* Worst case */
    enc->out_buf = (uint8_t *)calloc(enc->out_buf_size, 1);  /* zeroed for determinism */

    /* Init bitstream writer */
    tc_bs_writer_init(&enc->bs, enc->out_buf, enc->out_buf_size);

    /* Init tANS encoder */
    tc_tans_enc_init(&enc->tans, &enc->bs);

    /* Init entropy coding mode */
    enc->use_entropy_coded = config->enable_entropy_coded ? 1 : 0;

    /* Init rate control */
    tc_ratectl_init(&enc->rc, config);

    /* Init DPB */
    for (int i = 0; i < TC_REF_FRAMES; i++) {
        enc->dpb[i].frame = NULL;
        enc->dpb[i].poc = -1;
    }

#if !defined(TCODEC_NO_THREADS)
    /* Determine thread count */
    int num_threads = config->threads;
    if (num_threads <= 0) num_threads = 4;  /* Default for ARM quad-core */
    enc->num_threads = num_threads;

    /* Use WPP when there are multiple CTU rows and >1 thread.
     * Entropy coding (range coder, Phase 3) is not yet compatible
     * with WPP — disable WPP when it is active. */
    enc->use_wpp = (enc->num_ctu_rows > 1 && num_threads > 1 && !enc->use_entropy_coded) ? 1 : 0;

    if (enc->use_wpp) {
        /* Create thread pool */
        enc->pool = tc_threadpool_create(num_threads);
        if (!enc->pool) {
            /* Fallback: no WPP, still functional */
            enc->use_wpp = 0;
        }
    } else {
        enc->pool = NULL;
    }

    /* Allocate per-row bitstream buffers (used by WPP or as scratch) */
    int max_rows = enc->num_ctu_rows;
    enc->row_bs       = (tc_bs_writer_t *)calloc((size_t)max_rows, sizeof(tc_bs_writer_t));
    enc->row_tans     = (tc_tans_enc_t *)calloc((size_t)max_rows, sizeof(tc_tans_enc_t));
    enc->row_buf      = (uint8_t **)calloc((size_t)max_rows, sizeof(uint8_t *));
    enc->row_buf_size = (size_t *)calloc((size_t)max_rows, sizeof(size_t));

    if (enc->row_bs && enc->row_buf && enc->row_buf_size) {
        /* Per-row buffer: generous size per row (worst case = 1 row's share of output) */
        size_t per_row_size = enc->out_buf_size / (size_t)tc_max(max_rows, 1) + 256;
        for (int i = 0; i < max_rows; i++) {
            enc->row_buf[i] = (uint8_t *)calloc(per_row_size, 1);
            enc->row_buf_size[i] = per_row_size;
            if (!enc->row_buf[i]) {
                /* Out of memory for per-row buffers — fall back to sequential.
                 * Also destroy the pool to avoid idle worker threads. */
                enc->use_wpp = 0;
                tc_threadpool_destroy(enc->pool);
                enc->pool = NULL;
                break;
            }
        }
    } else {
        /* Allocation failed — fall back to sequential, destroy pool */
        enc->use_wpp = 0;
        tc_threadpool_destroy(enc->pool);
        enc->pool = NULL;
    }
#else
    (void)config->threads;
#endif

    return enc;
}

void tc_encoder_destroy(tc_encoder_t *enc)
{
    if (!enc) return;
    tc_frame_free(enc->cur);
    tc_frame_free(enc->recon);
    for (int i = 0; i < TC_REF_FRAMES; i++) {
        tc_frame_free(enc->dpb[i].frame);
    }
    free(enc->ctu_data);
    free(enc->out_buf);
#if !defined(TCODEC_NO_THREADS)
    /* Free per-row bitstream buffers */
    if (enc->row_buf) {
        for (int i = 0; i < enc->num_ctu_rows; i++) {
            free(enc->row_buf[i]);
        }
    }
    free(enc->row_buf);
    free(enc->row_buf_size);
    free(enc->row_bs);
    free(enc->row_tans);
    /* Destroy thread pool */
    tc_threadpool_destroy(enc->pool);
#endif
    free(enc);
}

void tc_encoder_force_keyframe(tc_encoder_t *enc)
{
    enc->force_keyframe = 1;
}

void tc_encoder_get_stats(tc_encoder_t *enc,
                           int64_t *total_bytes,
                           int32_t *total_frames,
                           double  *avg_psnr)
{
    if (total_bytes)  *total_bytes  = enc->total_bytes;
    if (total_frames) *total_frames = enc->total_frames;
    if (avg_psnr)     *avg_psnr = (enc->total_frames > 0)
        ? enc->sum_psnr / enc->total_frames : 0.0;
}

/* ── Main encode function ────────────────────────────────────── */

tc_error_t tc_encoder_encode(tc_encoder_t *enc,
                              const tc_pixel_t *y,  int stride_y,
                              const tc_pixel_t *cb, int stride_cb,
                              const tc_pixel_t *cr, int stride_cr,
                              tc_packet_t *packet_out)
{
    /* Copy input to internal frame */
    for (int row = 0; row < enc->cfg.height; row++) {
        memcpy(enc->cur->y + row * enc->cur->stride_y,
               y + row * stride_y, (size_t)enc->cfg.width);
    }
    for (int row = 0; row < enc->cfg.height / 2; row++) {
        memcpy(enc->cur->cb + row * enc->cur->stride_c,
               cb + row * stride_cb, (size_t)(enc->cfg.width / 2));
        memcpy(enc->cur->cr + row * enc->cur->stride_c,
               cr + row * stride_cr, (size_t)(enc->cfg.width / 2));
    }

    /* Determine frame type */
    int is_key = enc->force_keyframe || (enc->frame_count == 0);
    int keyframe_interval = enc->cfg.keyframe_interval > 0
        ? enc->cfg.keyframe_interval : 30;

    if (!is_key && (enc->frame_count % keyframe_interval == 0)) {
        is_key = 1;
    }

    /* Scene cut detection: compare current vs previous frame */
    if (!is_key && enc->frame_count > 0 && enc->dpb[0].frame != NULL) {
        double cut_dist = histogram_distance(
            enc->cur->y, enc->cur->stride_y,
            enc->dpb[0].frame->y, enc->dpb[0].frame->stride_y,
            enc->cfg.width, enc->cfg.height);
        if (cut_dist > SCENE_CUT_THRESHOLD) {
            is_key = 1;
        }
    }

    enc->force_keyframe = 0;

    tc_frame_type_t frame_type = is_key ? TC_FRAME_KEY : TC_FRAME_INTER;

    /* Rate control */
    tc_ratectl_frame_start(&enc->rc, frame_type);
    int qp = tc_ratectl_get_qp(&enc->rc);

    /* Reset bitstream writer.
     * Zero the output buffer for deterministic encoding — even though
     * the writer zeros bytes as it advances, memset ensures no stale
     * data from a previous frame can affect alignment padding. */
    memset(enc->out_buf, 0, enc->out_buf_size);
    tc_bs_writer_init(&enc->bs, enc->out_buf, enc->out_buf_size);
    tc_tans_enc_init(&enc->tans, &enc->bs);

    /* Clear CTU block info for new frame — stale MVs from the previous
     * frame could cause non-deterministic merge MV derivation at the
     * start of each CTU (before blocks are overwritten). */
    if (enc->ctu_data) {
        memset(enc->ctu_data, 0,
            (size_t)enc->num_ctu_cols * enc->num_ctu_rows * sizeof(tc_ctu_info_t));
    }

    /* Write frame header */
    tc_frame_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic[0] = TC_MAGIC_0;
    hdr.magic[1] = TC_MAGIC_1;
    hdr.magic[2] = TC_MAGIC_2;
    hdr.version  = enc->cfg.bitstream_version;
    hdr.width    = (uint16_t)enc->cfg.width;
    hdr.height   = (uint16_t)enc->cfg.height;
    hdr.flags    = is_key ? TC_FLAG_KEY_FRAME : 0;
#if !defined(TCODEC_NO_THREADS)
    if (enc->use_wpp) hdr.flags |= TC_FLAG_WPP;
#endif

    /* v1-specific header fields */
    if (hdr.version == TC_VERSION_V1) {
        /* Random Access Point: all key frames are RAPs.
         * RAP frames can be decoded independently — no reference
         * to prior frames is needed. Essential for seek/seeking. */
        if (is_key) hdr.flags |= TC_FLAG_RAP;

        /* CRC: append error detection if enabled */
        if (enc->cfg.enable_crc) hdr.flags |= TC_FLAG_CRC;

        /* Profile and level */
        uint8_t profile = enc->cfg.profile;
        uint8_t level_idx = enc->cfg.level_idx;
        if (profile > TC_PROFILE_MAX) profile = TC_PROFILE_BASELINE_MOBILE;
        if (level_idx > TC_LEVEL_MAX) level_idx = TC_LEVEL_AUTO;
        hdr.profile_level = (uint8_t)((profile << 4) | (level_idx & 0x0F));
        hdr.profile   = profile;
        hdr.level_idx = level_idx;

        /* Tool flags: signal which coding tools are ACTUALLY used by
         * the encoder for this frame. The decoder needs this to know
         * which syntax elements to expect. Profile compliance means
         * the encoder must not use tools outside the profile, but the
         * tool_flags field reflects actual usage, not profile capability. */
        uint16_t tools = 0;
        /* These tools are always active in the current encoder: */
        tools |= TC_TOOL_SKIP_MERGE;      /* Skip/merge inter modes */
        tools |= TC_TOOL_CFL_CHROMA;      /* Chroma-from-luma prediction */
        tools |= TC_TOOL_JND_WEIGHTING;   /* JND band quantization weighting */
        tools |= TC_TOOL_MEDIAN_MV_PRED;  /* Median MV predictor + MVD coding */
        tools |= TC_TOOL_SIX_TAP_INTERP;  /* 6-tap luma interpolation (always used) */
        /* Multi-reference: only when SLOW preset AND profile allows it.
         * Profile compliance: baseline-mobile decoders must never encounter
         * multi-ref syntax in their bitstreams. */
        if (enc->cfg.preset == TC_PRESET_SLOW &&
            profile >= TC_PROFILE_STREAMING_MAIN) {
            tools |= TC_TOOL_MULTI_REF;
        }
        /* Context-modeled entropy coding (Phase 3): active when config says so */
        if (enc->use_entropy_coded) {
            tools |= TC_TOOL_ENTROPY_CODED;
        }
        /* Future tools (not yet implemented):
         * TC_TOOL_DERINGING      — directional deringing (Phase 7)
         * TC_TOOL_SAO            — sample adaptive offset (Phase 7)
         * TC_TOOL_GRAIN_SYNTHESIS — film grain synthesis (Phase 7)
         * TC_TOOL_BIPRED         — bi-prediction (Phase 6)
         */

        hdr.tool_flags = tools;
        hdr.is_rap       = is_key ? 1 : 0;
        hdr.has_crc      = enc->cfg.enable_crc ? 1 : 0;
        hdr.has_ext_header = 0;  /* No extension headers yet */
    }

    hdr.qp_delta  = (uint8_t)(int8_t)(qp - TC_QP_DEFAULT);
    hdr.frame_num = (uint8_t)(enc->frame_count & 0xFF);
    hdr.frame_type = frame_type;
    hdr.qp = (uint8_t)qp;

    write_frame_header(&enc->bs, &hdr);

    /* After scene cut keyframes, clear ALL DPB entries to avoid
     * referencing frames from before the cut. Key frames don't use
     * inter prediction, so clearing dpb[0] too is safe. If we only
     * cleared slots 1+, the DPB shift after encoding would copy the
     * stale dpb[0] (pre-cut frame) into dpb[1]. */
    if (is_key && enc->frame_count > 0) {
        for (int i = 0; i < TC_REF_FRAMES; i++) {
            if (enc->dpb[i].frame) {
                tc_frame_free(enc->dpb[i].frame);
                enc->dpb[i].frame = NULL;
                enc->dpb[i].poc = -1;
            }
        }
    }

    /* Encode CTU rows with WPP parallelism or sequential fallback */
    enc_row_ctx_t rctx;
    rctx.enc = enc;
    rctx.qp = qp;
    rctx.frame_type = frame_type;

    /* Set up range coder state if entropy coded is active.
     * Contexts are initialized fresh per frame (not persisted). */
    tc_rc_enc_t  rc_enc_local;
    tc_rc_ctx_t *rc_ctx_ptr = NULL;
    tc_rc_enc_t *rc_ptr     = NULL;
    if (enc->use_entropy_coded) {
        tc_rc_ctx_init(enc->rc_ctx, TC_NUM_CONTEXTS_RC);
        tc_rc_enc_init(&rc_enc_local, &enc->bs);
        rc_ctx_ptr = enc->rc_ctx;
        rc_ptr     = &rc_enc_local;
    }
    rctx.rc     = rc_ptr;
    rctx.rc_ctx = rc_ctx_ptr;

#if !defined(TCODEC_NO_THREADS)
    rctx.use_wpp = enc->use_wpp;

    if (enc->use_wpp) {
        /* WPP: each row gets its own byte-aligned bitstream buffer.
         * An entry point table is written between header and row data
         * so the decoder can locate each row for parallel decoding.
         *
         * Bitstream layout (WPP):
         *   [header] [entry_point_table] [row0 data] [row1 data] ...
         *
         * Entry point table:
         *   num_offsets (16 bits) = num_ctu_rows - 1
         *   offset[0..N-2] (16 bits each) = byte offset from row data
         *       start to each row's start (row 0 is always at offset 0)
         *   [byte-align padding]
         *
         * IMPORTANT: rctx is read-only during tc_threadpool_run.
         * Worker threads only read enc/qp/frame_type/use_wpp — they
         * never modify rctx. This is safe for concurrent access. */

        /* Write entry point table (placeholder offsets, filled after merge) */
        int num_offsets = enc->num_ctu_rows - 1;
        tc_bs_writer_write_bits(&enc->bs, (uint32_t)num_offsets, 16);
        size_t offset_table_start = enc->bs.byte_pos;
        for (int i = 0; i < num_offsets; i++) {
            tc_bs_writer_write_bits(&enc->bs, 0, 16);  /* Placeholder */
        }
        tc_bs_writer_byte_align(&enc->bs);
        size_t row_data_start = enc->bs.byte_pos;

        /* Run WPP */
        tc_threadpool_run(enc->pool, encode_row, &rctx, enc->num_ctu_rows);

        /* Merge per-row bitstreams via memcpy (rows are byte-aligned).
         * Track the byte offset of each row for the entry point table. */
        size_t row_offsets[64];  /* Max CTU rows: 4096/64 = 64 */
        for (int row = 0; row < enc->num_ctu_rows; row++) {
            row_offsets[row] = enc->bs.byte_pos - row_data_start;
            size_t row_bytes = enc->row_bs[row].byte_pos;  /* bit_pos=0 (byte-aligned) */
            if (row_bytes > 0) {
                memcpy(enc->out_buf + enc->bs.byte_pos,
                       enc->row_buf[row], row_bytes);
            }
            enc->bs.byte_pos += row_bytes;
            /* bit_pos stays 0 — rows are byte-aligned */
        }

        /* Fill in entry point table with actual offsets (direct buffer write) */
        for (int i = 0; i < num_offsets; i++) {
            uint16_t off = (uint16_t)row_offsets[i + 1];
            enc->out_buf[offset_table_start + i * 2]     = (uint8_t)(off >> 8);
            enc->out_buf[offset_table_start + i * 2 + 1] = (uint8_t)(off & 0xFF);
        }

        /* No byte-align needed — last row is already byte-aligned */
    } else
#endif
    {
        /* Sequential: all rows write to main bitstream */
        for (int row = 0; row < enc->num_ctu_rows; row++) {
            encode_row(&rctx, row);
        }

        /* Flush tANS (shared across all rows) */
        tc_tans_enc_flush(&enc->tans);
        /* Flush range coder if active */
        if (rc_ptr) tc_rc_enc_flush(rc_ptr);
        tc_bs_writer_byte_align(&enc->bs);
    }

    /* WPP+RC not yet supported: flush per-row range coders before merge.
     * When WPP+RC is implemented (Phase 9), per-row flush happens per-row
     * and contexts are propagated between rows. */
    (void)rc_ptr;

    /* Append CRC-16 if v1 with TC_FLAG_CRC set */
    if (hdr.version == TC_VERSION_V1 && hdr.has_crc) {
        /* CRC covers header + CTU data (everything up to but not
         * including the CRC bytes themselves). Compute from the
         * output buffer and append the 2-byte CRC. */
        size_t payload_bytes = tc_bs_writer_bytes(&enc->bs);
        uint16_t crc = tc_crc16(enc->out_buf, payload_bytes);
        tc_bs_writer_write_bits(&enc->bs, (uint32_t)(crc >> 8), 8);
        tc_bs_writer_write_bits(&enc->bs, (uint32_t)(crc & 0xFF), 8);
    }

    /* Update rate control */
    size_t frame_bytes = tc_bs_writer_bytes(&enc->bs);
    tc_ratectl_frame_end(&enc->rc, (int64_t)frame_bytes * 8);

    /* Compute PSNR */
    double psnr = tc_psnr(enc->cur->y, enc->cur->stride_y,
                           enc->recon->y, enc->recon->stride_y,
                           enc->cfg.width, enc->cfg.height);
    enc->sum_psnr += psnr;

    /* Update DPB: shift entries and insert new frame at slot 0.
     * IMPORTANT: Must free oldest entry BEFORE struct copy loop,
     * otherwise we'd free a frame still pointed to by dpb[i-1].
     * This keeps the most recent frames in the DPB for multi-ref.
     * Oldest entry (slot TC_REF_FRAMES-1) is evicted. */
    if (enc->dpb[TC_REF_FRAMES - 1].frame)
        tc_frame_free(enc->dpb[TC_REF_FRAMES - 1].frame);
    for (int i = TC_REF_FRAMES - 1; i > 0; i--) {
        enc->dpb[i] = enc->dpb[i - 1];
    }
    enc->dpb[0].frame = tc_frame_clone(enc->recon);
    enc->dpb[0].poc = enc->frame_count;
    enc->dpb[0].qp_avg = (uint8_t)qp;

    /* Fill output packet */
    packet_out->data = enc->out_buf;
    packet_out->size = frame_bytes;
    packet_out->pts  = enc->frame_count;
    packet_out->key_frame = is_key;

    enc->total_bytes += (int64_t)frame_bytes;
    enc->total_frames++;
    enc->frame_count++;

    return TC_OK;
}
