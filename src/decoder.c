/*
 * decoder.c — Decoding pipeline for TCodec
 *
 * Pipeline (reverse of encoder):
 *   1. Parse frame header
 *   2. For each CTU:
 *      a. Read mode decision (2-bit: skip/inter/intra)
 *      b. For intra: read intra mode + intra predict
 *      c. For inter/skip: read MVD + inter predict
 *      d. For non-skip: read DCT size flag + coefficients
 *      e. Dequantize + inverse DCT → reconstruct
 *      f. Decode chroma (CfL for intra, DC for inter/skip)
 *      g. Deblocking filter
 *   3. Output reconstructed frame
 */

#include "tcodec_common.h"
#include <stdlib.h>
#include <string.h>
#include <assert.h>

/* ── Read frame header ──────────────────────────────────────── */

/* ── Dual-path decoding helpers (raw bits vs range coder) ─────
 *
 * When rc!=NULL (TC_TOOL_ENTROPY_CODED active), use context-modeled
 * range coder. When rc==NULL, use raw bitstream reads.
 * This avoids duplicating decode_block for the two paths.
 * ══════════════════════════════════════════════════════════════ */

static TCODEC_FORCEINLINE uint32_t dec_read_bits(
    tc_bs_reader_t *bs, tc_rc_dec_t *rc, tc_rc_ctx_t *ctx,
    int base_ctx, int nbits)
{
    if (rc) return tc_rc_dec_bits(rc, ctx, base_ctx, nbits);
    else    return tc_bs_reader_read_bits(bs, nbits);
}

static TCODEC_FORCEINLINE int32_t dec_read_se(
    tc_bs_reader_t *bs, tc_rc_dec_t *rc, tc_rc_ctx_t *ctx,
    int base_ctx)
{
    if (rc) {
        uint32_t mapped = tc_rc_dec_ue(rc, ctx, base_ctx);
        /* Invert the signed-to-unsigned mapping from encoder:
         *   0 → 0, 1 → 1, 2 → -1, 3 → 2, 4 → -2, ...
         *   val>0 → mapped=2*val-1, val<0 → mapped=-2*val */
        if (mapped == 0) return 0;
        if (mapped & 1) return (int32_t)((mapped + 1) >> 1);
        else            return -(int32_t)(mapped >> 1);
    } else {
        return tc_bs_reader_read_se(bs);
    }
}

static TCODEC_FORCEINLINE void dec_read_coeffs(
    tc_tans_dec_t *tans, tc_rc_dec_t *rc, tc_rc_ctx_t *rc_ctx,
    tc_coeff_t *coeffs, int n, tc_block_size_t dct_size)
{
    if (rc) tc_rc_dec_coeffs(rc, rc_ctx, coeffs, n, dct_size);
    else    tc_tans_dec_coeffs(tans, coeffs, n, dct_size);
}

/* ── Read frame header ──────────────────────────────────────── */

static tc_error_t read_frame_header(tc_bs_reader_t *bs, tc_frame_header_t *hdr)
{
    if (tc_bs_reader_eof(bs)) return TC_ERR_EOF;

    memset(hdr, 0, sizeof(*hdr));

    hdr->magic[0] = (uint8_t)tc_bs_reader_read_bits(bs, 8);
    hdr->magic[1] = (uint8_t)tc_bs_reader_read_bits(bs, 8);
    hdr->magic[2] = (uint8_t)tc_bs_reader_read_bits(bs, 8);

    if (hdr->magic[0] != TC_MAGIC_0 ||
        hdr->magic[1] != TC_MAGIC_1 ||
        hdr->magic[2] != TC_MAGIC_2) {
        return TC_ERR_BITSTREAM;
    }

    hdr->version    = (uint8_t)tc_bs_reader_read_bits(bs, 8);

    /* Version check: reject incompatible future versions.
     * We support v0 and v1. v2+ bitstreams may have different
     * header structure or coding tools we don't understand. */
    if (hdr->version > TC_VERSION_V1) {
        return TC_ERR_BITSTREAM;
    }

    hdr->width      = (uint16_t)tc_bs_reader_read_bits(bs, 16);
    hdr->height     = (uint16_t)tc_bs_reader_read_bits(bs, 16);
    hdr->flags      = (uint8_t)tc_bs_reader_read_bits(bs, 8);
    hdr->qp_delta   = (uint8_t)tc_bs_reader_read_bits(bs, 8);
    hdr->frame_num  = (uint8_t)tc_bs_reader_read_bits(bs, 8);

    /* Version-dependent header fields */
    if (hdr->version == TC_VERSION_V0) {
        /* v0: reserved byte (ignored) */
        tc_bs_reader_read_bits(bs, 8);
        hdr->profile_level = 0;
        hdr->tool_flags = 0;
        hdr->profile = TC_PROFILE_BASELINE_MOBILE;
        hdr->level_idx = TC_LEVEL_AUTO;
        hdr->is_rap = 0;
        hdr->has_crc = 0;
        hdr->has_ext_header = 0;
    } else {
        /* v1: profile_level byte + tool_flags (16 bits) */
        hdr->profile_level = (uint8_t)tc_bs_reader_read_bits(bs, 8);
        hdr->tool_flags    = (uint16_t)tc_bs_reader_read_bits(bs, 16);

        /* Extract profile and level from packed byte */
        hdr->profile   = (hdr->profile_level >> 4) & 0x0F;
        hdr->level_idx = hdr->profile_level & 0x0F;

        /* Derived v1 flags (must precede the extension read below) */
        hdr->is_rap       = (hdr->flags & TC_FLAG_RAP) ? 1 : 0;
        hdr->has_crc      = (hdr->flags & TC_FLAG_CRC) ? 1 : 0;
        hdr->has_ext_header = (hdr->flags & TC_FLAG_EXT_HEADER) ? 1 : 0;

        /* v1 extension header (D4): optional frame-type byte for
         * B-frame streams (written on every frame of such streams). */
        if (hdr->has_ext_header) {
            uint8_t fc = (uint8_t)tc_bs_reader_read_bits(bs, 8);
            if (fc == 0)      hdr->frame_type_ext = TC_FRAME_KEY;
            else if (fc == 2) hdr->frame_type_ext = TC_FRAME_BIDIR;
            else              hdr->frame_type_ext = TC_FRAME_INTER;
            hdr->has_type_ext = 1;
        }

        /* Validate profile */
        if (hdr->profile > TC_PROFILE_MAX) {
            /* Unknown profile — cannot decode safely */
            return TC_ERR_BITSTREAM;
        }


        /* Validate level constraints (if explicit level set) */
        if (hdr->level_idx > 0 && hdr->level_idx <= TC_LEVEL_MAX) {
            const tc_level_info_t *lvl = tc_level_get_info(hdr->level_idx);
            if (hdr->width > lvl->max_width || hdr->height > lvl->max_height) {
                return TC_ERR_BITSTREAM;  /* Exceeds level constraints */
            }
        }

        /* v0 reserved bits 5-3 must be 0 for v0 compatibility.
         * For v1, bits 5-3 are RAP/CRC/EXT — no additional validation needed
         * since they have well-defined meanings. */
    }

    /* Derived fields (common to v0 and v1). B-frames (D4) are
     * signaled per frame through the v1 tool flags (TC_TOOL_BIPRED);
     * the flags byte has no spare bits (tiles occupy bits 0-2). */
    if (hdr->has_type_ext) {
        hdr->frame_type = hdr->frame_type_ext;
    } else if (hdr->flags & TC_FLAG_KEY_FRAME) {
        hdr->frame_type = TC_FRAME_KEY;
    } else {
        hdr->frame_type = TC_FRAME_INTER;
    }
    /* qp_delta is stored as uint8_t but was encoded as (int8_t)(qp - TC_QP_DEFAULT).
     * Cast through int8_t to correctly handle signed values (QP < 32). */
    hdr->qp = (uint8_t)tc_clip(TC_QP_DEFAULT + (int8_t)hdr->qp_delta, TC_QP_MIN, TC_QP_MAX);
    hdr->tile_cols_log2 = (hdr->flags & TC_FLAG_TILE_C_MASK) >> 2;
    hdr->tile_rows_log2 = (hdr->flags & TC_FLAG_TILE_R_MASK);

    return TC_OK;
}

/* ── DPB reference lookup by POC (B-frames) — mirrors the encoder
 * exactly: forward = max POC < cur, backward = min POC > cur. */
static const tc_frame_buf_t *dpb_find_poc_lt(const tc_ref_entry_t *dpb, int poc)
{
    const tc_frame_buf_t *best = NULL;
    int best_p = -1;
    for (int i = 0; i < TC_REF_FRAMES; i++) {
        if (dpb[i].frame && dpb[i].poc >= 0 && dpb[i].poc < poc && dpb[i].poc > best_p) {
            best_p = dpb[i].poc;
            best = dpb[i].frame;
        }
    }
    return best;
}

static const tc_frame_buf_t *dpb_find_poc_gt(const tc_ref_entry_t *dpb, int poc)
{
    const tc_frame_buf_t *best = NULL;
    int best_p = 0x7FFFFFFF;
    for (int i = 0; i < TC_REF_FRAMES; i++) {
        if (dpb[i].frame && dpb[i].poc >= 0 && dpb[i].poc > poc && dpb[i].poc < best_p) {
            best_p = dpb[i].poc;
            best = dpb[i].frame;
        }
    }
    return best;
}

/* ── Decode one 8×8 block ───────────────────────────────────── */

/* Mode values matching encoder's 2-bit mode field */
#define TC_MODE_SKIP  0
#define TC_MODE_INTER 1
#define TC_MODE_INTRA 2
#define TC_MODE_MERGE 3

static void decode_block(tc_decoder_t *dec, int ctu_idx, int blk_idx,
                         int frame_x, int frame_y,
                         int qp, tc_frame_type_t frame_type, int frame_poc,
                         tc_bs_reader_t *bs, tc_tans_dec_t *tans,
                         tc_rc_dec_t *rc, tc_rc_ctx_t *rc_ctx)
{
    int blk_size = 8;
    int n_coeff  = 64;

    tc_pixel_t pred_block[64];
    tc_coeff_t dct_coeff[64];

    int is_skip  = 0;
    int is_intra = 1;  /* Default intra for key frames */
    tc_intra_mode_t intra_mode = TC_INTRA_DC;

    /* ── Read mode decision ────────────────────────────────── */
    int is_merge = 0;
    if (frame_type != TC_FRAME_KEY) {
        /* 2-bit mode: 0=skip, 1=inter, 2=intra, 3=merge */
        uint32_t mode = dec_read_bits(bs, rc, rc_ctx, RC_CTX_BLOCK_MODE, 2);
        switch (mode) {
        case TC_MODE_SKIP:
            is_skip  = 1;
            is_intra = 0;
            break;
        case TC_MODE_INTER:
            is_skip  = 0;
            is_intra = 0;
            break;
        case TC_MODE_INTRA:
            is_skip  = 0;
            is_intra = 1;
            break;
        case TC_MODE_MERGE:
            /* Merge: MV derived from spatial neighbors, no ref_idx/MVD */
            is_merge = 1;
            is_skip  = 1;  /* zero residual, like skip */
            is_intra = 0;
            break;
        }
    }

    /* ── Intra mode + prediction ───────────────────────────── */
    if (is_intra) {
        /* Read intra mode (5 bits = enough for 18 modes) */
        uint32_t im = dec_read_bits(bs, rc, rc_ctx, RC_CTX_INTRA_MODE, 5);
        if (im >= TC_INTRA_MODES) im = TC_INTRA_DC;
        intra_mode = (tc_intra_mode_t)im;

        /* Build reference samples from reconstructed frame */
        tc_pixel_t ref_above[32 + 1];
        tc_pixel_t ref_left[32 + 1];

        tc_intra_get_ref(dec->cur->y, dec->cur->stride_y,
                         frame_x, frame_y, blk_size,
                         dec->width, dec->height,
                         ref_above + 1, ref_left + 1);

        tc_intra_predict(pred_block, blk_size,
                         ref_above + 1, ref_left + 1,
                         blk_size, intra_mode);
    }

    /* ── Inter/skip/merge: derive MV + inter prediction ───── */
    tc_mv_s block_mv = {0, 0};  /* Store for ctu_data */
    uint8_t block_ref_idx = 0;   /* Store for ctu_data */
    if (!is_intra) {
        tc_mv_s mv;
        const tc_frame_buf_t *selected_ref;

        /* B-frames: ref selection for every inter-coded block
         * (skip / inter / merge) — 0 = forward, 1 = backward,
         * 2 = bidirectional average (D4). Context from neighbors. */
        int ref_sel = 0;
        if (frame_type == TC_FRAME_BIDIR) {
            ref_sel = (int)dec_read_bits(bs, rc, rc_ctx, RC_CTX_REF_SEL, 2);
            if (ref_sel > 2) ref_sel = 1;  /* Safety clamp */
        }

        if (is_merge) {
            /* Merge mode: MV derived from median of spatial neighbors.
             * No ref_idx or MVD is signaled — significant bitrate savings.
             * Must match encoder's median computation exactly. */
            mv = (tc_mv_s){frame_x * 4, frame_y * 4};  /* Default: collocated */
            if (dec->ctu_data) {
                tc_ctu_info_t *ctu = &dec->ctu_data[ctu_idx];
                /* Reconstruct bx, by from blk_idx for 8x8 blocks in 64x64 CTU */
                int bx_local = blk_idx % 8;
                int by_local = blk_idx / 8;
                tc_mv_s mv_a = {0,0}, mv_b = {0,0}, mv_c = {0,0};
                int have_a = 0, have_b = 0, have_c = 0;
                if (bx_local > 0) {
                    tc_block_info_t *left = &ctu->blocks[blk_idx - 1];
                    if (!left->is_intra) { mv_a = left->mv; have_a = 1; }
                }
                if (by_local > 0) {
                    tc_block_info_t *above = &ctu->blocks[blk_idx - 8];
                    if (!above->is_intra) { mv_b = above->mv; have_b = 1; }
                }
                if (by_local > 0 && bx_local < 7) {
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
                    mv.x = px;
                    mv.y = py;
                }
            }
            if (frame_type == TC_FRAME_BIDIR) {
                selected_ref = ref_sel ? dpb_find_poc_gt(dec->dpb, frame_poc)
                                       : dpb_find_poc_lt(dec->dpb, frame_poc);
            } else {
                selected_ref = dec->dpb[0].frame;  /* Merge uses ref 0 on P */
            }
        } else {
            /* Skip/inter: read ref_idx + MVD from bitstream.
             * ref_idx is only transmitted on P-frames; B-frames use
             * the ref_sel bit and POC-implied references. */
            uint32_t ref_idx = 0;
            if (frame_type != TC_FRAME_KEY && frame_type != TC_FRAME_BIDIR) {
                ref_idx = dec_read_bits(bs, rc, rc_ctx, RC_CTX_REF_IDX, 2);
                if (ref_idx >= TC_REF_FRAMES) ref_idx = 0;  /* Safety clamp */
            } else if (frame_type == TC_FRAME_BIDIR) {
                ref_idx = (uint32_t)ref_sel;
            }
            block_ref_idx = (uint8_t)ref_idx;  /* ref_sel for B-frames */

            /* Compute median MV predictor — must match encoder exactly.
             * MVD is coded relative to this predictor, not collocated.
             * This produces smaller MVDs when spatial neighbors are available. */
            tc_mv_s predictor_mv = {frame_x * 4, frame_y * 4};  /* Default: collocated */
            if (dec->ctu_data) {
                tc_ctu_info_t *ctu = &dec->ctu_data[ctu_idx];
                int bx_local = blk_idx % 8;
                int by_local = blk_idx / 8;
                tc_mv_s mv_a = {0,0}, mv_b = {0,0}, mv_c = {0,0};
                int have_a = 0, have_b = 0, have_c = 0;
                if (bx_local > 0) {
                    tc_block_info_t *left = &ctu->blocks[blk_idx - 1];
                    if (!left->is_intra) { mv_a = left->mv; have_a = 1; }
                }
                if (by_local > 0) {
                    tc_block_info_t *above = &ctu->blocks[blk_idx - 8];
                    if (!above->is_intra) { mv_b = above->mv; have_b = 1; }
                }
                if (by_local > 0 && bx_local < 7) {
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
                    predictor_mv.x = px;
                    predictor_mv.y = py;
                }
            }

            int32_t mvd_x = dec_read_se(bs, rc, rc_ctx, RC_CTX_MVD_X);
            int32_t mvd_y = dec_read_se(bs, rc, rc_ctx, RC_CTX_MVD_Y);
            mv = (tc_mv_s){ mvd_x + predictor_mv.x, mvd_y + predictor_mv.y };
            if (frame_type == TC_FRAME_BIDIR) {
                selected_ref = ref_sel ? dpb_find_poc_gt(dec->dpb, frame_poc)
                                       : dpb_find_poc_lt(dec->dpb, frame_poc);
            } else {
                selected_ref = dec->dpb[ref_idx].frame;
            }
        }

        block_mv = mv;  /* Save for ctu_data storage below */

        /* Inter predict. tc_inter_predict() is OOB-safe (per-pixel
         * clamping) for any MV, so we call it unconditionally for every
         * reference that exists. This guarantees decoder output matches
         * encoder recon bit-exactly: the encoder emits whatever MV it
         * finds, and both sides run the identical interpolation path.
         * ref_sel == 2 (B-frames): average of forward and mirrored
         * backward predictions — must match the encoder's bi path. */
        if (frame_type == TC_FRAME_BIDIR && ref_sel == 2) {
            const tc_frame_buf_t *rf = dpb_find_poc_lt(dec->dpb, frame_poc);
            const tc_frame_buf_t *rb = dpb_find_poc_gt(dec->dpb, frame_poc);
            if (rf && rb) {
                tc_pixel_t pa[64], pb[64];
                tc_inter_predict(rf->y, rf->stride_y,
                                 rf->width, rf->height,
                                 mv, pa, blk_size, blk_size);
                tc_mv_s mvb = { -mv.x, -mv.y };
                tc_inter_predict(rb->y, rb->stride_y,
                                 rb->width, rb->height,
                                 mvb, pb, blk_size, blk_size);
                for (int i = 0; i < n_coeff; i++) {
                    pred_block[i] = (tc_pixel_t)((pa[i] + pb[i] + 1) >> 1);
                }
            } else {
                memset(pred_block, 128, (size_t)n_coeff);
            }
        } else if (selected_ref) {
            tc_inter_predict(selected_ref->y, selected_ref->stride_y,
                             selected_ref->width, selected_ref->height,
                             mv, pred_block, blk_size, blk_size);
        } else {
            memset(pred_block, 128, (size_t)n_coeff);
        }
    }

    /* ── Reconstruct + decode coefficients ─────────────────── */
    if (is_skip) {
        /* Skip/merge = prediction only, no residual. Copy pred directly. */
        for (int r = 0; r < blk_size; r++) {
            for (int c = 0; c < blk_size; c++) {
                int px = frame_x + c;
                int py = frame_y + r;
                if (px < dec->width && py < dec->height) {
                    dec->cur->y[py * dec->cur->stride_y + px] = pred_block[r * blk_size + c];
                }
            }
        }
    } else {
        /* Non-skip: read transform flag (bit0 = WHT/DCT, bit1 = 4×4/8×8)
         * + coefficients + reconstruct */
        uint32_t transform_id = dec_read_bits(bs, rc, rc_ctx, RC_CTX_DCT_SIZE, 2);
        uint32_t dct_size_id = (transform_id >> 1) & 1;
        uint32_t transform_type = transform_id & 1;

        if (dct_size_id == TC_BLOCK_4x4_ID) {
            for (int sy = 0; sy < 2; sy++) {
                for (int sx = 0; sx < 2; sx++) {
                    tc_coeff_t sub[16];
                    dec_read_coeffs(tans, rc, rc_ctx, sub, 16, TC_BLOCK_4x4_ID);
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
                    else tc_idct4x4_res(sub, rec, 4);
                    for (int r = 0; r < 4; r++) {
                        for (int c = 0; c < 4; c++) {
                            int px = frame_x + sx * 4 + c;
                            int py = frame_y + sy * 4 + r;
                            if (px < dec->width && py < dec->height) {
                                int val = (int)pred_block[(sy * 4 + r) * blk_size + (sx * 4 + c)]
                                          + (int)rec[r * 4 + c];
                                dec->cur->y[py * dec->cur->stride_y + px] =
                                    (tc_pixel_t)tc_clip(val, 0, 255);
                            }
                        }
                    }
                }
            }
        } else {
            dec_read_coeffs(tans, rc, rc_ctx, dct_coeff, 64, TC_BLOCK_8x8_ID);
            for (int i = 0; i < 64; i++) {
                int band = tc_freq_band(i, 8);
                int w = tc_jnd_weight(band, i);
                int scale = tc_qscale(qp);
                int eff = (scale * w + 4) >> 3;
                if (eff < 1) eff = 1;
                if (dct_coeff[i] > 0) dct_coeff[i] = (tc_coeff_t)(dct_coeff[i] * eff + (eff >> 1));
                else if (dct_coeff[i] < 0) dct_coeff[i] = (tc_coeff_t)(dct_coeff[i] * eff - (eff >> 1));
            }
            tc_coeff_t rec[64];
            if (transform_type == 0) tc_iwht8x8(dct_coeff, rec, 8);
            else tc_idct8x8_res(dct_coeff, rec, 8);
            for (int r = 0; r < 8; r++) {
                for (int c = 0; c < 8; c++) {
                    int px = frame_x + c;
                    int py = frame_y + r;
                    if (px < dec->width && py < dec->height) {
                        int val = (int)pred_block[r * blk_size + c] + (int)rec[r * 8 + c];
                        dec->cur->y[py * dec->cur->stride_y + px] =
                            (tc_pixel_t)tc_clip(val, 0, 255);
                    }
                }
            }
        }
    } /* end else (non-skip reconstruct) */
    /* ── Decode chroma ───────────────────────────────────────
     * Must match encoder's chroma prediction:
     *   - Intra blocks: CfL (chroma from luma) using reconstructed luma
     *   - Inter/skip blocks: DC prediction (128)
     * Chroma always has 4×4 DCT + quantize with band=0 + QP+1. */
    {
        int cx = frame_x / 2;
        int cy = frame_y / 2;
        int chroma_qp = tc_clip(qp + 1, 0, 63);

        for (int comp = 0; comp < 2; comp++) {
            tc_pixel_t *chroma_recon = comp == 0 ? dec->cur->cb : dec->cur->cr;
            int c_stride = dec->cur->stride_c;

            /* Compute chroma prediction — must match encoder exactly */
            tc_pixel_t c_pred[16];
            if (is_intra && !is_skip && !is_merge) {
                /* CfL: Chroma from Luma — use reconstructed luma to predict chroma.
                 * This matches the encoder's CfL implementation exactly. */
                int luma_sum = 0;
                for (int r = 0; r < 4; r++) {
                    for (int c = 0; c < 4; c++) {
                        luma_sum += (int)dec->cur->y[(frame_y + r) * dec->cur->stride_y + (frame_x + c)];
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
                int alpha_shift = 3;  /* alpha ≈ 0.125 (matches encoder) */
                for (int i = 0; i < 16; i++) {
                    int luma_val = (int)dec->cur->y[(frame_y + (i / 4)) * dec->cur->stride_y + (frame_x + (i % 4))];
                    int cfl = c_dc + ((luma_val - luma_avg) >> alpha_shift);
                    c_pred[i] = (tc_pixel_t)tc_clip(cfl, 0, 255);
                }
            } else {
                /* DC prediction for inter/skip blocks */
                for (int i = 0; i < 16; i++) {
                    c_pred[i] = 128;
                }
            }

            /* Decode chroma coefficients.
             * IMPORTANT: tc_dequantize with band=0 applies simple (non-JND)
             * dequantization, which matches the encoder's chroma path that
             * uses tc_quantize with band=0. If JND is later added to chroma
             * in tc_quantize/tc_dequantize, this decoder path must be updated
             * to apply matching JND weighting (like the luma path does inline). */
            tc_coeff_t c_coeff[16];
            dec_read_coeffs(tans, rc, rc_ctx, c_coeff, 16, TC_BLOCK_4x4_ID);
            tc_dequantize(c_coeff, 16, chroma_qp, 0);

            tc_coeff_t c_rec[16];
            tc_iwht4x4(c_coeff, c_rec, 4);

            for (int r = 0; r < 4; r++) {
                for (int c = 0; c < 4; c++) {
                    int val = (int)c_pred[r * 4 + c] + (int)c_rec[r * 4 + c];
                    if (cx + c < dec->width / 2 && cy + r < dec->height / 2) {
                        chroma_recon[(cy + r) * c_stride + (cx + c)] =
                            (tc_pixel_t)tc_clip(val, 0, 255);
                    }
                }
            }
        }
    }

    /* ── Store block info for merge MV derivation ────────────
     * Must happen AFTER mode/MV is known so that subsequent
     * blocks in the same CTU can use this block's MV for
     * median predictor computation (matching encoder). */
    if (dec->ctu_data) {
        tc_block_info_t *bi = &dec->ctu_data[ctu_idx].blocks[blk_idx];
        bi->is_intra = (uint8_t)is_intra;
        bi->mv = block_mv;
        bi->intra_mode = intra_mode;
        bi->ref_idx = block_ref_idx;  /* Actual ref_idx for skip/inter, 0 for merge */
    }
}

/* ── Decode one CTU row ───────────────────────────────────── */

static void decode_row_impl(tc_decoder_t *dec, int row, int qp,
                             tc_frame_type_t frame_type, int frame_poc,
                             tc_bs_reader_t *bs, tc_tans_dec_t *tans,
                             tc_rc_dec_t *rc, tc_rc_ctx_t *rc_ctx)
{
    for (int col = 0; col < dec->num_ctu_cols; col++) {
        int ctu_x = col * TC_CTU_SIZE;
        int ctu_y = row * TC_CTU_SIZE;
        int ctu_idx = row * dec->num_ctu_cols + col;

        /* For each 8×8 block in CTU */
        for (int by = 0; by < TC_CTU_SIZE / 8; by++) {
            for (int bx = 0; bx < TC_CTU_SIZE / 8; bx++) {
                int blk_x = ctu_x + bx * 8;
                int blk_y = ctu_y + by * 8;

                if (blk_x + 8 > dec->width || blk_y + 8 > dec->height) continue;

                decode_block(dec, ctu_idx, by * 8 + bx, blk_x, blk_y,
                             qp, frame_type, frame_poc, bs, tans,
                             rc, rc_ctx);
            }
        }

        /* Deblock this CTU */
        if (ctu_x + TC_CTU_SIZE <= dec->width &&
            ctu_y + TC_CTU_SIZE <= dec->height) {
            tc_deblock_ctu(dec->cur->y, dec->cur->stride_y,
                           dec->cur->cb, dec->cur->stride_c,
                           dec->cur->cr, dec->cur->stride_c,
                           ctu_x, ctu_y, qp);
        }
    }
}

#if !defined(TCODEC_NO_THREADS)
/* WPP thread pool wrapper matching tc_wpp_row_func signature.
 * Each thread picks its per-row reader/tans by row index. */
typedef struct {
    tc_decoder_t   *dec;
    int             qp;
    tc_frame_type_t frame_type;
    int             frame_poc;
    tc_bs_reader_t *row_bs;      /* Per-row reader array */
    tc_tans_dec_t  *row_tans;    /* Per-row tANS decoder array */
    tc_rc_dec_t    *row_rc;      /* Per-row range coder (NULL if not entropy coded) */
    tc_rc_ctx_t    *row_rc_ctx;  /* Flat: num_rows * TC_NUM_CONTEXTS_RC */
} dec_wpp_ctx_t;

static void decode_row_wpp(void *ctx, int row)
{
    dec_wpp_ctx_t *wctx = (dec_wpp_ctx_t *)ctx;
    decode_row_impl(wctx->dec, row, wctx->qp, wctx->frame_type, wctx->frame_poc,
                    &wctx->row_bs[row], &wctx->row_tans[row],
                    wctx->row_rc ? &wctx->row_rc[row] : NULL,
                    wctx->row_rc_ctx ? &wctx->row_rc_ctx[row * TC_NUM_CONTEXTS_RC] : NULL);
}
#endif /* !TCODEC_NO_THREADS */

/* ── Decoder create/destroy ──────────────────────────────────── */

tc_decoder_t *tc_decoder_create(int32_t width, int32_t height)
{
    tc_decoder_t *dec = (tc_decoder_t *)calloc(1, sizeof(tc_decoder_t));
    if (!dec) return NULL;

    dec->width  = width;
    dec->height = height;
    dec->prev_qp = TC_QP_DEFAULT;

    if (width > 0 && height > 0) {
        dec->cur = tc_frame_alloc(width, height);
        if (!dec->cur) { free(dec); return NULL; }

        /* Allocate CTU info for merge MV derivation.
         * We don't know exact dimensions until first frame header,
         * so use the provided width/height as initial guess.
         * Will be reallocated if header dimensions differ. */
        dec->num_ctu_cols = (width  + TC_CTU_SIZE - 1) / TC_CTU_SIZE;
        dec->num_ctu_rows = (height + TC_CTU_SIZE - 1) / TC_CTU_SIZE;
        dec->ctu_data = (tc_ctu_info_t *)calloc(
            (size_t)dec->num_ctu_cols * dec->num_ctu_rows,
            sizeof(tc_ctu_info_t));
    }

    for (int i = 0; i < TC_REF_FRAMES; i++) {
        dec->dpb[i].frame = NULL;
        dec->dpb[i].poc = -1;
    }

#if !defined(TCODEC_NO_THREADS)
    /* Allocate thread pool and per-row readers for WPP.
     * Actual WPP activation depends on the per-frame header TC_FLAG_WPP,
     * but we pre-allocate the infrastructure at create time. */
    dec->num_threads = 4;  /* Default for ARM quad-core */
    dec->use_wpp = 0;      /* Will be set per-frame based on header flag */
    dec->pool = tc_threadpool_create(dec->num_threads);
    dec->row_bs   = NULL;  /* Allocated per-frame when WPP is active */
    dec->row_tans = NULL;
#endif

    return dec;
}

void tc_decoder_destroy(tc_decoder_t *dec)
{
    if (!dec) return;
    tc_frame_free(dec->cur);
    tc_frame_free(dec->disp.out);
    for (int i = 0; i < dec->disp.n; i++) {
        tc_frame_free(dec->disp.frames[i]);
    }
    for (int i = 0; i < TC_REF_FRAMES; i++) {
        tc_frame_free(dec->dpb[i].frame);
    }
    free(dec->ctu_data);
#if !defined(TCODEC_NO_THREADS)
    free(dec->row_bs);
    free(dec->row_tans);
    tc_threadpool_destroy(dec->pool);
#endif
    free(dec);
}

void tc_decoder_get_info(tc_decoder_t *dec, int32_t *width, int32_t *height)
{
    if (width)  *width  = dec->width;
    if (height) *height = dec->height;
}

int tc_decoder_crc_valid(tc_decoder_t *dec)
{
    if (!dec) return 0;
    return dec->last_crc_valid;
}

int tc_decoder_entropy_coded(tc_decoder_t *dec)
{
    if (!dec) return 0;
    return dec->use_entropy_coded;
}

/* ── Main decode function ────────────────────────────────────── */

tc_error_t tc_decoder_decode(tc_decoder_t *dec,
                              const uint8_t *data, size_t size,
                              const tc_pixel_t **y,  int *stride_y,
                              const tc_pixel_t **cb, int *stride_cb,
                              const tc_pixel_t **cr, int *stride_cr)
{
    /* Init bitstream reader */
    tc_bs_reader_init(&dec->bs, data, size);

    /* Read frame header */
    tc_frame_header_t hdr;
    tc_error_t err = read_frame_header(&dec->bs, &hdr);
    if (err != TC_OK) return err;

    dec->last_header = hdr;

    /* Update dimensions if auto-detect */
    if (dec->width == 0 || dec->height == 0) {
        dec->width  = hdr.width;
        dec->height = hdr.height;
    }

    /* Allocate/reallocate frame buffer */
    if (!dec->cur || dec->cur->width != hdr.width || dec->cur->height != hdr.height) {
        tc_frame_free(dec->cur);
        dec->cur = tc_frame_alloc(hdr.width, hdr.height);
        if (!dec->cur) return TC_ERR_MEMORY;

        /* Reallocate CTU info for new dimensions */
        dec->num_ctu_cols = (hdr.width  + TC_CTU_SIZE - 1) / TC_CTU_SIZE;
        dec->num_ctu_rows = (hdr.height + TC_CTU_SIZE - 1) / TC_CTU_SIZE;
        free(dec->ctu_data);
        dec->ctu_data = (tc_ctu_info_t *)calloc(
            (size_t)dec->num_ctu_cols * dec->num_ctu_rows,
            sizeof(tc_ctu_info_t));
        if (!dec->ctu_data) return TC_ERR_MEMORY;
    }

    int qp = hdr.qp;
    if (qp > TC_QP_MAX) qp = TC_QP_MAX;

    /* Init tANS decoder */
    tc_tans_dec_init(&dec->tans, &dec->bs);

    /* Determine if entropy coding is active for this frame.
     * TC_TOOL_ENTROPY_CODED is mutually exclusive with WPP
     * (serial bitstream; WPP+RC to be addressed in Phase 9). */
    int use_entropy_coded = 0;
    if (hdr.version == TC_VERSION_V1 && (hdr.tool_flags & TC_TOOL_ENTROPY_CODED)) {
        use_entropy_coded = 1;
    }
    dec->use_entropy_coded = use_entropy_coded;

    /* Set up range coder if entropy coded is active */
    tc_rc_dec_t  rc_dec_local;
    tc_rc_ctx_t *rc_ctx_ptr = NULL;
    tc_rc_dec_t *rc_ptr     = NULL;
    if (use_entropy_coded) {
        tc_rc_ctx_init(dec->rc_ctx, TC_NUM_CONTEXTS_RC);
        tc_rc_dec_init(&rc_dec_local, &dec->bs);
        rc_ctx_ptr = dec->rc_ctx;
        rc_ptr     = &rc_dec_local;
    }

    /* Clear CTU block info for new frame (stale MVs from previous frame
     * could cause incorrect merge MV derivation). */
    if (dec->ctu_data) {
        memset(dec->ctu_data, 0,
            (size_t)dec->num_ctu_cols * dec->num_ctu_rows * sizeof(tc_ctu_info_t));
    }

    /* CRC validation for v1 bitstreams with TC_FLAG_CRC set.
     * The CRC-16 covers header + CTU data (everything before the 2 CRC bytes).
     * The CRC is the last 2 bytes of the input. Validate before decoding
     * so the result is available to the caller via last_crc_valid.
     * Graceful degradation: we still decode the frame even on CRC mismatch,
     * but the caller can check last_crc_valid to detect corruption. */
    dec->last_crc_valid = 1;  /* Default: OK (no CRC or CRC matches) */
    if (hdr.version == TC_VERSION_V1 && hdr.has_crc) {
        if (size >= 2) {
            size_t crc_pos = size - 2;
            uint16_t stored_crc = (uint16_t)((data[crc_pos] << 8) | data[crc_pos + 1]);
            uint16_t computed_crc = tc_crc16(data, crc_pos);
            if (computed_crc != stored_crc) {
                dec->last_crc_valid = 0;
                /* CRC mismatch: corruption detected. We still decode
                 * the frame (graceful degradation) but expose the
                 * validation result to the caller. */
            }
        } else {
            dec->last_crc_valid = 0;  /* CRC requested but data too short */
        }
    }

    /* CTU grid */
    int num_ctu_rows = (hdr.height + TC_CTU_SIZE - 1) / TC_CTU_SIZE;
    /* Sanity check: header dimensions should match stored dimensions.
     * A mismatch could mean a malformed bitstream, but the entry point
     * table is indexed by row, so we use the header value for correctness.
     * Only assert in debug builds — release builds handle it gracefully. */
    assert(num_ctu_rows == dec->num_ctu_rows);

    /* Check for WPP entry point table in bitstream.
     * When TC_FLAG_WPP is set, the bitstream contains an entry point
     * table between header and row data. We MUST parse it even when
     * we can't use WPP dispatch (e.g., no thread pool), so the reader
     * is positioned correctly for sequential decoding. */
    int is_wpp = (hdr.flags & TC_FLAG_WPP) != 0;
    size_t row_data_start = 0;  /* Byte offset where row data begins */
    size_t row_offsets[64];     /* Max CTU rows: 4096/64 = 64 */
    int have_row_offsets = 0;

    if (is_wpp) {
        /* Parse entry point table — MUST happen regardless of whether
         * WPP dispatch is available, so the reader advances past it. */
        uint32_t num_offsets = tc_bs_reader_read_bits(&dec->bs, 16);
        if (num_offsets > 63) return TC_ERR_BITSTREAM;  /* Sanity check */

        row_offsets[0] = 0;  /* Row 0 always starts at beginning of row data */
        for (uint32_t i = 0; i < num_offsets && i < 63; i++) {
            row_offsets[i + 1] = (size_t)tc_bs_reader_read_bits(&dec->bs, 16);
        }

        /* Byte-align reader to skip padding after entry point table.
         * The entry point table starts at a byte-aligned position (after header),
         * and 16-bit reads preserve alignment. But there may be explicit
         * byte-align padding from the encoder. Advance to next byte boundary. */
        if (dec->bs.bit_pos > 0) {
            dec->bs.byte_pos++;
            dec->bs.bit_pos = 0;
        }
        row_data_start = dec->bs.byte_pos;

        /* Bounds-check row offsets against input size */
        for (int i = 0; i <= (int)num_offsets && i < num_ctu_rows; i++) {
            if (row_data_start + row_offsets[i] > size) {
                return TC_ERR_BITSTREAM;
            }
        }
        have_row_offsets = 1;
    }

    /* Decode CTU rows with WPP parallelism or sequential fallback */
    int use_wpp = 0;

#if !defined(TCODEC_NO_THREADS)
    use_wpp = is_wpp && have_row_offsets && dec->pool && num_ctu_rows > 1;

    if (use_wpp) {
        /* WPP mode: initialize per-row readers from entry point offsets
         * and dispatch via thread pool.
         *
         * IMPORTANT: wctx is read-only during tc_threadpool_run.
         * Worker threads only read dec/qp/frame_type/row_bs/row_tans —
         * they never modify wctx. This is safe for concurrent access. */

        /* Allocate per-row readers and tANS decoders for this frame */
        free(dec->row_bs);
        free(dec->row_tans);
        dec->row_bs   = (tc_bs_reader_t *)calloc((size_t)num_ctu_rows, sizeof(tc_bs_reader_t));
        dec->row_tans = (tc_tans_dec_t *)calloc((size_t)num_ctu_rows, sizeof(tc_tans_dec_t));
        if (!dec->row_bs || !dec->row_tans) {
            free(dec->row_bs);
            free(dec->row_tans);
            dec->row_bs = NULL;
            dec->row_tans = NULL;
            use_wpp = 0;  /* Fall back to sequential */
        } else {
            /* Initialize each row's reader to point at its data within
             * the input buffer. Each row's data starts at:
             *   row_data_start + row_offsets[row]
             * The size extends to the next row's start (or end of data). */
            for (int row = 0; row < num_ctu_rows; row++) {
                size_t row_start = row_data_start + row_offsets[row];
                size_t row_end;
                if (row + 1 < num_ctu_rows) {
                    row_end = row_data_start + row_offsets[row + 1];
                } else {
                    row_end = size;  /* Last row extends to end of data */
                }
                tc_bs_reader_init(&dec->row_bs[row],
                                  data + row_start,
                                  row_end - row_start);
                tc_tans_dec_init(&dec->row_tans[row], &dec->row_bs[row]);
            }

            /* Dispatch WPP via thread pool */
            dec_wpp_ctx_t wctx;
            wctx.dec        = dec;
            wctx.qp         = qp;
            wctx.frame_type = hdr.frame_type;
    wctx.frame_poc = hdr.frame_num;
            wctx.row_bs     = dec->row_bs;
            wctx.row_tans   = dec->row_tans;
            wctx.row_rc     = NULL;
            wctx.row_rc_ctx = NULL;
            tc_threadpool_run(dec->pool, decode_row_wpp, &wctx, num_ctu_rows);
        }
    }
#endif /* !TCODEC_NO_THREADS */

    if (!use_wpp) {
        /* Sequential: decode all rows from a single bitstream reader.
         *
         * For WPP bitstreams when threading is unavailable or WPP
         * allocation failed, we must skip the entry point table and
         * re-init the reader from row_data_start. Each row's data is
         * byte-aligned, so the sequential reader advances past row
         * boundaries correctly (padding bytes between rows are consumed
         * naturally as the reader moves forward). */
        if (is_wpp && have_row_offsets) {
            tc_bs_reader_init(&dec->bs, data + row_data_start, size - row_data_start);
            tc_tans_dec_init(&dec->tans, &dec->bs);
        }

        for (int row = 0; row < num_ctu_rows; row++) {
            decode_row_impl(dec, row, qp, hdr.frame_type, hdr.frame_num,
                            &dec->bs, &dec->tans,
                            rc_ptr, rc_ctx_ptr);
            /* WPP rows are byte-aligned — skip padding to reach
             * the next row's start. Without this, the reader would
             * misinterpret padding bits as block data.
             *
             * TODO: When real tANS is implemented (Phase 3), this
             * sequential WPP fallback must also re-init tANS per row
             * to match the encoder's per-row tANS entry points.
             * Currently safe because tANS is Exp-Golomb (stateless). */
            if (is_wpp && row + 1 < num_ctu_rows) {
                if (dec->bs.bit_pos > 0) {
                    dec->bs.byte_pos++;
                    dec->bs.bit_pos = 0;
                }
            }
        }
    }

    (void)have_row_offsets;
    (void)row_data_start;
    (void)is_wpp;
    (void)use_wpp;

    /* Update DPB: shift entries and insert new frame at slot 0.
     * IMPORTANT: Must free oldest entry BEFORE struct copy loop,
     * otherwise we'd free a frame that's still pointed to by dpb[i-1].
     * Keeps most recent frames for multi-reference support. */
    if (dec->dpb[TC_REF_FRAMES - 1].frame)
        tc_frame_free(dec->dpb[TC_REF_FRAMES - 1].frame);
    for (int i = TC_REF_FRAMES - 1; i > 0; i--) {
        dec->dpb[i] = dec->dpb[i - 1];
    }
    dec->dpb[0].frame = tc_frame_clone(dec->cur);
    dec->dpb[0].poc = hdr.frame_num;
    dec->dpb[0].qp_avg = (uint8_t)qp;

    dec->prev_qp = qp;

    /* B-frame display reorder (D4): once a B-frame stream is
     * detected (any frame carries the extension header), decoded
     * frames are buffered by display POC and released one per call
     * in display order. This matches the encoder's coding-order
     * emission (anchor-first) while presenting decode output in
     * presentation order. RAP/key frames reset the reorder window
     * (seek support). */
    if (hdr.has_ext_header) {
        int is_rap = (hdr.flags & TC_FLAG_RAP) != 0;
        if (is_rap && dec->disp.active) {
            for (int i = 0; i < dec->disp.n; i++) tc_frame_free(dec->disp.frames[i]);
            dec->disp.n = 0;
            dec->disp.next_display = (int)hdr.frame_num;
        }
        dec->disp.active = 1;
        if (dec->disp.n < TC_REF_FRAMES + 1) {
            dec->disp.frames[dec->disp.n] = tc_frame_clone(dec->cur);
            dec->disp.pocs[dec->disp.n] = (int)hdr.frame_num;
            dec->disp.n++;
        }
        /* Pop the next display frame if its turn has come. */
        for (int i = 0; i < dec->disp.n; i++) {
            if (dec->disp.pocs[i] == dec->disp.next_display) {
                if (!dec->disp.out) {
                    dec->disp.out = tc_frame_alloc(dec->width, dec->height);
                    if (!dec->disp.out) return TC_ERR_MEMORY;
                }
                tc_frame_copy(dec->disp.out, dec->disp.frames[i]);
                tc_frame_free(dec->disp.frames[i]);
                for (int k = i; k < dec->disp.n - 1; k++) {
                    dec->disp.frames[k] = dec->disp.frames[k + 1];
                    dec->disp.pocs[k]   = dec->disp.pocs[k + 1];
                }
                dec->disp.n--;
                dec->disp.next_display++;
                if (y)         *y        = dec->disp.out->y;
                if (stride_y)  *stride_y = dec->disp.out->stride_y;
                if (cb)        *cb       = dec->disp.out->cb;
                if (stride_cb) *stride_cb = dec->disp.out->stride_c;
                if (cr)        *cr       = dec->disp.out->cr;
                if (stride_cr) *stride_cr = dec->disp.out->stride_c;
                return TC_OK;
            }
        }
        /* No display frame ready yet (reorder buffering). */
        return TC_ERR_NEED_MORE;
    }

    /* Output */
    if (y)         *y        = dec->cur->y;
    if (stride_y)  *stride_y = dec->cur->stride_y;
    if (cb)        *cb       = dec->cur->cb;
    if (stride_cb) *stride_cb = dec->cur->stride_c;
    if (cr)        *cr       = dec->cur->cr;
    if (stride_cr) *stride_cr = dec->cur->stride_c;

    return TC_OK;
}

/* Drain display-order frames still buffered after the last packet of
 * a B-frame stream. Same output contract as tc_decoder_decode. */
tc_error_t tc_decoder_flush_tail(tc_decoder_t *dec,
                              const tc_pixel_t **y,  int *stride_y,
                              const tc_pixel_t **cb, int *stride_cb,
                              const tc_pixel_t **cr, int *stride_cr)
{
    if (!dec || !dec->disp.active) return TC_ERR_EOF;
    for (int i = 0; i < dec->disp.n; i++) {
        if (dec->disp.pocs[i] == dec->disp.next_display) {
            if (!dec->disp.out) {
                dec->disp.out = tc_frame_alloc(dec->width, dec->height);
                if (!dec->disp.out) return TC_ERR_MEMORY;
            }
            tc_frame_copy(dec->disp.out, dec->disp.frames[i]);
            tc_frame_free(dec->disp.frames[i]);
            for (int k = i; k < dec->disp.n - 1; k++) {
                dec->disp.frames[k] = dec->disp.frames[k + 1];
                dec->disp.pocs[k]   = dec->disp.pocs[k + 1];
            }
            dec->disp.n--;
            dec->disp.next_display++;
            if (y)         *y        = dec->disp.out->y;
            if (stride_y)  *stride_y = dec->disp.out->stride_y;
            if (cb)        *cb       = dec->disp.out->cb;
            if (stride_cb) *stride_cb = dec->disp.out->stride_c;
            if (cr)        *cr       = dec->disp.out->cr;
            if (stride_cr) *stride_cr = dec->disp.out->stride_c;
            return TC_OK;
        }
    }
    return TC_ERR_EOF;
}
