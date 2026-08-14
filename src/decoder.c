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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

static uint64_t dec_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static TCODEC_FORCEINLINE void dec_profile_add(tc_decoder_t *dec,
                                                uint64_t *bucket,
                                                uint64_t start)
{
    if (dec->profile_enabled) {
        uint64_t elapsed = dec_now_ns() - start;
#if defined(__GNUC__) || defined(__clang__)
        __atomic_fetch_add(bucket, elapsed, __ATOMIC_RELAXED);
#else
        *bucket += elapsed;
#endif
    }
}

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
    /* Keep the hot context base in a register across the dispatch.  The
     * range decoder mutates it, so this is deliberately a local pointer,
     * not a copied context array. */
    tc_rc_ctx_t *ctx = rc_ctx;
    if (rc) {
        /* The range decoder touches this small context array for nearly
         * every coefficient bit.  Keep its base local and advertise the
         * upcoming read to ARM's data prefetcher; the scalar/EG path is
         * unchanged. */
#if defined(__GNUC__) || defined(__clang__)
        __builtin_prefetch(ctx, 0, 3);
#endif
        tc_rc_dec_coeffs(rc, ctx, coeffs, n, dct_size);
    }
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

    /* Version check: v0/v1/v2 are understood (v2 uses the v1 header
     * layout with the v2 quadtree payload); anything newer is a
     * future bitstream we must not reinterpret.  Each version is
     * dispatched to exactly one decoder path — never silently
     * upgraded or downgraded. */
    if (hdr->version > TC_VERSION_V2) {
        return TC_ERR_BITSTREAM;
    }

    hdr->width      = (uint16_t)tc_bs_reader_read_bits(bs, 16);
    hdr->height     = (uint16_t)tc_bs_reader_read_bits(bs, 16);

    /* Dimension sanity check (all versions): prevents a hostile header
     * from triggering huge allocations (uint16 can encode up to 65535,
     * but TC_MAX_WIDTH/HEIGHT bound the supported space). */
    if (hdr->width == 0 || hdr->height == 0 ||
        hdr->width > TC_MAX_WIDTH || hdr->height > TC_MAX_HEIGHT) {
        return TC_ERR_BITSTREAM;
    }
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

    if (bs->error) return TC_ERR_BITSTREAM;

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
                tc_inter_predict_decoder(rf->y, rf->stride_y,
                                         rf->width, rf->height,
                                         mv, pa, blk_size, blk_size);
                tc_mv_s mvb = { -mv.x, -mv.y };
                tc_inter_predict_decoder(rb->y, rb->stride_y,
                                         rb->width, rb->height,
                                         mvb, pb, blk_size, blk_size);
                for (int i = 0; i < n_coeff; i++) {
                    pred_block[i] = (tc_pixel_t)((pa[i] + pb[i] + 1) >> 1);
                }
            } else {
                memset(pred_block, 128, (size_t)n_coeff);
            }
        } else if (selected_ref) {
            tc_inter_predict_decoder(selected_ref->y, selected_ref->stride_y,
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

/* ════════════════════════════════════════════════════════════════
 * Bitstream v2 quadtree decoder
 * ────────────────────────────────────────────────────────────────
 * Mirrors the encoder's qt_* write pass bit-for-bit: same quadtree
 * geometry (tc_qt_index), same child order (z-order q=0..3), same
 * syntax order, same MV predictor, same quantizer primitives
 * (tc_eff_scale / tc_dequant_coeff) and the same pixel-domain
 * transforms (tc_idct4x4 / tc_idct8x8).  The decoder needs no node
 * snapshots: it reconstructs directly, which is exactly what the
 * encoder's write pass produces (its snapshot restores are no-ops
 * for the final reconstruction).
 * ══════════════════════════════════════════════════════════════ */

typedef struct {
    tc_decoder_t     *dec;
    int               ctu_x, ctu_y;
    int               qp, qp_c;
    tc_frame_type_t   frame_type;
    int               poc;
    tc_bs_reader_t   *bs;
    tc_tans_dec_t    *tans;
    tc_rc_dec_t      *rc;
    tc_rc_ctx_t      *rc_ctx;
    int               eff_scale[4][64];
    int               eff_c_scale[4][64];
    qt_mvcell_t       grid[TC_MVGRID_STRIDE * TC_MVGRID_STRIDE];
} qt_dec_t;

static TCODEC_FORCEINLINE uint32_t qt_read_bits(qt_dec_t *d, int base_ctx, int nbits)
{
    uint64_t start = d->dec->profile_enabled ? dec_now_ns() : 0;
    uint32_t value = dec_read_bits(d->bs, d->rc, d->rc_ctx, base_ctx, nbits);
    dec_profile_add(d->dec, &d->dec->profile_parse_ns, start);
    return value;
}

static TCODEC_FORCEINLINE int32_t qt_read_se(qt_dec_t *d, int base_ctx)
{
    uint64_t start = d->dec->profile_enabled ? dec_now_ns() : 0;
    int32_t value = dec_read_se(d->bs, d->rc, d->rc_ctx, base_ctx);
    dec_profile_add(d->dec, &d->dec->profile_parse_ns, start);
    return value;
}

/* MV predictor — must match encoder qt_mvp() exactly (median of the
 * left / above / above-right 8×8 MV-grid cells, in the same relative
 * MV convention). */
static tc_mv_s qt_dec_mvp(const qt_dec_t *d, int cx, int cy)
{
    tc_mv_s a={0,0}, b={0,0}, c={0,0};
    int ha=0, hb=0, hc=0;
    if (cx > 0)     { const qt_mvcell_t *m=&d->grid[cy*TC_MVGRID_STRIDE+(cx-1)]; if(!m->intra){a.x=m->dx;a.y=m->dy;ha=1;} }
    if (cy > 0)     { const qt_mvcell_t *m=&d->grid[(cy-1)*TC_MVGRID_STRIDE+cx]; if(!m->intra){b.x=m->dx;b.y=m->dy;hb=1;} }
    if (cy > 0 && cx < 7) { const qt_mvcell_t *m=&d->grid[(cy-1)*TC_MVGRID_STRIDE+(cx+1)]; if(!m->intra){c.x=m->dx;c.y=m->dy;hc=1;} }
    if (!ha && !hb && !hc) return (tc_mv_s){0,0};
    if (!ha) a = hb ? b : c;
    if (!hb) b = ha ? a : c;
    if (!hc) c = ha ? a : b;
    int px,py;
    { int t,x[3]={a.x,b.x,c.x}; if(x[0]>x[1]){t=x[0];x[0]=x[1];x[1]=t;} if(x[1]>x[2]){t=x[1];x[1]=x[2];x[2]=t;} if(x[0]>x[1]){t=x[0];x[0]=x[1];x[1]=t;} px=x[1]; }
    { int t,y[3]={a.y,b.y,c.y}; if(y[0]>y[1]){t=y[0];y[0]=y[1];y[1]=t;} if(y[1]>y[2]){t=y[1];y[1]=y[2];y[2]=t;} if(y[0]>y[1]){t=y[0];y[0]=y[1];y[1]=t;} py=y[1]; }
    return (tc_mv_s){px,py};
}

static const uint8_t qt_band4[16] = {
    0, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3
};

static const uint8_t qt_band8[64] = {
    0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3
};

/* Luma residual of one CU: per-8×8-TU transform-size flag + entropy-
 * coded coefficients, dequantized with the shared JND-weighted step
 * and inverted with the pixel-domain IDCT.  Mirrors qt_code_luma(). */
static void qt_dec_luma(qt_dec_t *d, int px, int py, int cu,
                        const tc_pixel_t *pred)
{
    tc_decoder_t *dec = d->dec;
    int ntu = cu/8;
    int rs = dec->cur->stride_y;
    for (int ty=0;ty<ntu;ty++) for (int tx=0;tx<ntu;tx++){
        int ox=tx*8, oy=ty*8;
        uint32_t dct = qt_read_bits(d, RC_CTX_DCT_SIZE, 1);
        if (dct == TC_BLOCK_8x8_ID) {
            tc_coeff_t tu[64], iq[64];
            uint64_t coeff_start = dec->profile_enabled ? dec_now_ns() : 0;
            dec_read_coeffs(d->tans, d->rc, d->rc_ctx, tu, 64, TC_BLOCK_8x8_ID);
            dec_profile_add(dec, &dec->profile_coeff_ns, coeff_start);
            const int (*eff_table)[64] = d->eff_scale;
            int nonzero = 0, dc_only = 1;
            for (int i=0;i<64;i++){
                int band=qt_band8[i];
                iq[i]=(tc_coeff_t)tc_dequant_coeff(tu[i],eff_table[band][i]);
                if (tu[i]) nonzero = 1;
                if (i != 0 && tu[i]) dc_only = 0;
            }
            if (!nonzero) {
                /* Zero residuals are common for v2 skip-like inter CUs.
                 * Copy rows directly instead of clearing a transform buffer
                 * and executing 64 add/clip operations. */
                uint64_t copy_start = dec->profile_enabled ? dec_now_ns() : 0;
                for (int y=0;y<8;y++)
                    memcpy(dec->cur->y + (py+oy+y)*rs + px+ox,
                           pred + (oy+y)*cu + ox, 8);
                dec_profile_add(dec, &dec->profile_copy_ns, copy_start);
            } else if (dc_only) {
                /* The fixed-point residual IDCT of a DC-only 8x8 TU is
                 * spatially constant.  Apply the exact two rounded 1-D
                 * operations used by ndct8_point(), once, then reconstruct
                 * the block without entering the 64-operation transform. */
                int v = tc_idct_dc8_res((int)iq[0]);
                tc_recon_add_dc8x8(pred + oy * cu + ox, cu, v,
                                   dec->cur->y + (py + oy) * rs + px + ox, rs);
            } else {
                tc_coeff_t res8[64];
                uint64_t transform_start = dec->profile_enabled ? dec_now_ns() : 0;
                tc_idct8x8_neon(iq,res8,8);
                dec_profile_add(dec, &dec->profile_transform_ns, transform_start);
                tc_recon_add_clip8x8(pred + oy * cu + ox, cu, res8,
                                     dec->cur->y + (py + oy) * rs + px + ox, rs);
            }
        } else {
            for (int q=0;q<4;q++){
                int sx=(q&1)*4, sy=(q&2)*2;
                /* The encoder emits one transform-size flag per 4×4
                 * sub-block (always 4×4 in practice); read to consume. */
                (void)qt_read_bits(d, RC_CTX_DCT_SIZE, 1);
                 tc_coeff_t c4[16], iq4[16];
                 uint64_t coeff_start = dec->profile_enabled ? dec_now_ns() : 0;
                 dec_read_coeffs(d->tans, d->rc, d->rc_ctx, c4, 16, TC_BLOCK_4x4_ID);
                 dec_profile_add(dec, &dec->profile_coeff_ns, coeff_start);
                 const int (*eff4_table)[64] = d->eff_scale;
                 int nonzero = 0, dc_only = 1;
                 for (int i=0;i<16;i++){
                     int band=qt_band4[i];
                     iq4[i]=(tc_coeff_t)tc_dequant_coeff(c4[i],eff4_table[band][i]);
                     if (c4[i]) nonzero = 1;
                     if (i != 0 && c4[i]) dc_only = 0;
                 }
                if (!nonzero) {
                    uint64_t copy_start = dec->profile_enabled ? dec_now_ns() : 0;
                    for (int y=0;y<4;y++)
                        memcpy(dec->cur->y + (py+oy+sy+y)*rs + px+ox+sx,
                               pred + (oy+sy+y)*cu + ox+sx, 4);
                    dec_profile_add(dec, &dec->profile_copy_ns, copy_start);
                } else if (dc_only) {
                    int v = tc_idct_dc4_res((int)iq4[0]);
                    tc_recon_add_dc4x4(pred + (oy + sy) * cu + ox + sx, cu, v,
                                       dec->cur->y + (py + oy + sy) * rs + px + ox + sx, rs);
                } else {
                    tc_coeff_t res4[16];
                    uint64_t transform_start = dec->profile_enabled ? dec_now_ns() : 0;
                    tc_idct4x4_neon(iq4,res4,4);
                    dec_profile_add(dec, &dec->profile_transform_ns, transform_start);
                    tc_recon_add_clip4x4(pred + (oy + sy) * cu + ox + sx, cu, res4,
                                          dec->cur->y + (py + oy + sy) * rs + px + ox + sx, rs);
                }
            }
        }
    }
}

/* Chroma residual of one CU: 4×4 transforms on both components, QP+1,
 * no transform-size flag.  Mirrors qt_code_chroma(). */
static void qt_dec_chroma(qt_dec_t *d, int px, int py, int cu,
                          const tc_pixel_t *pred[2])
{
    tc_decoder_t *dec = d->dec;
    int cs = cu/2;
    int rs = dec->cur->stride_c;
    tc_pixel_t *rec[2] = { dec->cur->cb, dec->cur->cr };
    for (int comp = 0; comp < 2; comp++) {
        for (int ty = 0; ty < cs/4; ty++)
            for (int tx = 0; tx < cs/4; tx++) {
                int ox=tx*4, oy=ty*4;
                tc_coeff_t c4[16], iq[16];
                uint64_t coeff_start = dec->profile_enabled ? dec_now_ns() : 0;
                dec_read_coeffs(d->tans, d->rc, d->rc_ctx, c4, 16, TC_BLOCK_4x4_ID);
                dec_profile_add(dec, &dec->profile_coeff_ns, coeff_start);
                /* Preserve the normative per-frequency-band chroma
                 * dequantization, but reuse the QP-local table built once
                 * for this CTU instead of calling tc_eff_scale per coeff. */
                const int (*eff_table)[64] = d->eff_c_scale;
                int nonzero = 0, dc_only = 1;
                for (int i=0;i<16;i++){
                    int band = qt_band4[i];
                    iq[i] = (tc_coeff_t)tc_dequant_coeff(c4[i], eff_table[band][0]);
                    if (c4[i]) nonzero = 1;
                    if (i != 0 && c4[i]) dc_only = 0;
                }
                if (!nonzero) {
                    uint64_t copy_start = dec->profile_enabled ? dec_now_ns() : 0;
                    for (int y=0;y<4;y++)
                        memcpy(rec[comp] + (py/2+oy+y)*rs + px/2+ox,
                               pred[comp] + (oy+y)*cs + ox, 4);
                    dec_profile_add(dec, &dec->profile_copy_ns, copy_start);
                } else if (dc_only) {
                    int v = ((int)iq[0] * 8192 + 8192) >> 14;
                    v = (v * 8192 + 8192) >> 14;
                    tc_recon_add_dc4x4(pred[comp] + oy * cs + ox, cs, v,
                                        rec[comp] + (py/2 + oy) * rs + px/2 + ox, rs);
                } else {
                    tc_coeff_t res[16];
                    uint64_t transform_start = dec->profile_enabled ? dec_now_ns() : 0;
                    tc_idct4x4_res(iq, res, 4);
                    dec_profile_add(dec, &dec->profile_transform_ns, transform_start);
                    tc_recon_add_clip4x4(pred[comp] + oy * cs + ox, cs, res,
                                          rec[comp] + (py/2 + oy) * rs + px/2 + ox, rs);
                }
            }
    }
}

/* Decode one leaf CU.  Reads the exact syntax the encoder's write
 * pass emits and reconstructs identically. */
static void qt_dec_leaf(qt_dec_t *d, int depth, int cx, int cy)
{
    int cu = 8 << (TC_QT_MAX_DEPTH - depth);
    int px = d->ctu_x + cx*8, py = d->ctu_y + cy*8;
    tc_decoder_t *dec = d->dec;
    tc_pixel_t *pred = dec->v2_pred;
    const tc_pixel_t *cpred[2];
    tc_pixel_t (*cbuf)[(TC_CTU_SIZE/2) * (TC_CTU_SIZE/2)] = dec->v2_cbuf;
    cpred[0]=cbuf[0]; cpred[1]=cbuf[1];
    int is_intra = (int)qt_read_bits(d, RC_CTX_BLOCK_MODE, 1);
    int skip = 0, merge = 0;
    int dct_size = TC_BLOCK_8x8_ID;
    int32_t mvd_x = 0, mvd_y = 0;

    if (is_intra) {
        uint32_t im = qt_read_bits(d, RC_CTX_INTRA_MODE, 5);
        if (im >= TC_INTRA_MODES) {
            d->bs->error = 1;
            im = TC_INTRA_DC;
        }
        uint32_t ch_intra = qt_read_bits(d, RC_CTX_BLOCK_MODE, 1);
        if (ch_intra) {
            /* reserved: intra chroma DC with explicit cmode (3 bits) */
            (void)qt_read_bits(d, RC_CTX_INTRA_MODE, 3);
        }
        tc_pixel_t ra[2*64+1], rl[2*64+1];
        tc_intra_get_ref_v2(dec->cur->y, dec->cur->stride_y, px, py, cu,
                            dec->width, dec->height, ra+1, rl+1);
        tc_intra_predict(pred, cu, ra+1, rl+1, cu, (tc_intra_mode_t)im);
        qt_dec_luma(d, px, py, cu, pred);
        uint64_t chroma_start = dec->profile_enabled ? dec_now_ns() : 0;
        if (ch_intra) {
            tc_intra_chroma_dc(dec->cur->cb, dec->cur->stride_c, px/2, py/2, cu/2, cbuf[0], cu/2);
            tc_intra_chroma_dc(dec->cur->cr, dec->cur->stride_c, px/2, py/2, cu/2, cbuf[1], cu/2);
        } else {
            /* Intra luma CUs carry collocated chroma motion compensation
             * from dpb[0] using the MVP-derived MV (v2 design); falls
             * back to neighbour-DC when no reference exists yet. */
            tc_mv_s cmv = qt_dec_mvp(d, cx, cy);
            cmv.x += px*4; cmv.y += py*4;
            const tc_frame_buf_t *r = dec->dpb[0].frame;
            if (r) {
                tc_inter_predict_chroma_decoder(r->cb, r->stride_c, dec->width/2, dec->height/2, cmv, cbuf[0], cu/2, cu/2);
                tc_inter_predict_chroma_decoder(r->cr, r->stride_c, dec->width/2, dec->height/2, cmv, cbuf[1], cu/2, cu/2);
            } else {
                tc_intra_chroma_dc(dec->cur->cb, dec->cur->stride_c, px/2, py/2, cu/2, cbuf[0], cu/2);
                tc_intra_chroma_dc(dec->cur->cr, dec->cur->stride_c, px/2, py/2, cu/2, cbuf[1], cu/2);
            }
        }
        qt_dec_chroma(d, px, py, cu, cpred);
        dec_profile_add(dec, &dec->profile_chroma_ns, chroma_start);
    } else {
        int ref_sel = 0;
        if (d->frame_type == TC_FRAME_BIDIR) {
            ref_sel = (int)qt_read_bits(d, RC_CTX_REF_SEL, 1);
            (void)qt_read_bits(d, RC_CTX_BLOCK_MODE, 1);  /* bi flag (reserved) */
            if (ref_sel > 1) d->bs->error = 1;
        }
        uint32_t skipf = qt_read_bits(d, RC_CTX_SKIP_FLAG, 1);
        if (skipf) {
            skip = 1;
        } else {
            uint32_t mergef = qt_read_bits(d, RC_CTX_MERGE_FLAG, 1);
            if (mergef) {
                merge = 1;
            } else {
                dct_size = (int)qt_read_bits(d, RC_CTX_DCT_SIZE, 1);
                mvd_x = qt_read_se(d, RC_CTX_MVD_X);
                mvd_y = qt_read_se(d, RC_CTX_MVD_Y);
                if (mvd_x < -32768 || mvd_x > 32767 ||
                    mvd_y < -32768 || mvd_y > 32767)
                    d->bs->error = 1;
            }
        }
        tc_mv_s mvp = qt_dec_mvp(d, cx, cy);
        tc_mv_s mv = { mvp.x + px*4, mvp.y + py*4 };
        if (!skip && !merge) { mv.x += mvd_x; mv.y += mvd_y; }

        /* Luma inter prediction: BIDIR frames average forward+mirrored
         * backward references (reserved syntax for v2 — the v2 encoder
         * does not emit B-frames); otherwise single dpb[0] prediction. */
            if (d->frame_type == TC_FRAME_BIDIR) {
            const tc_frame_buf_t *r1 = ref_sel ? dec->dpb[1].frame : dec->dpb[0].frame;
            const tc_frame_buf_t *r2 = ref_sel ? dec->dpb[0].frame : dec->dpb[1].frame;
            if (r1 && r2) {
                tc_pixel_t *t1 = dec->v2_bipred_a;
                tc_pixel_t *t2 = dec->v2_bipred_b;
                uint64_t motion_start = dec->profile_enabled ? dec_now_ns() : 0;
                tc_inter_predict_decoder(r1->y, r1->stride_y, dec->width, dec->height, mv, t1, cu, cu);
                tc_mv_s mv2 = { -mv.x, -mv.y };
                tc_inter_predict_decoder(r2->y, r2->stride_y, dec->width, dec->height, mv2, t2, cu, cu);
                for (int i=0;i<cu*cu;i++) pred[i]=(tc_pixel_t)((t1[i]+t2[i]+1)>>1);
                dec_profile_add(dec, &dec->profile_motion_ns, motion_start);
            } else if (r1) {
                tc_inter_predict_decoder(r1->y, r1->stride_y, dec->width, dec->height, mv, pred, cu, cu);
            } else {
                memset(pred, 128, (size_t)cu*cu);
            }
        } else {
            const tc_frame_buf_t *r = dec->dpb[0].frame;
            uint64_t motion_start = dec->profile_enabled ? dec_now_ns() : 0;
            if (r) tc_inter_predict_decoder(r->y, r->stride_y, dec->width, dec->height, mv, pred, cu, cu);
            else   memset(pred, 128, (size_t)cu * cu);
            dec_profile_add(dec, &dec->profile_motion_ns, motion_start);
        }

        if (!skip) {
            qt_dec_luma(d, px, py, cu, pred);
            uint64_t chroma_start = dec->profile_enabled ? dec_now_ns() : 0;
            /* Inter CUs never transmit a chroma-intra flag: chroma is
             * always collocated MC from dpb[0] with the luma MV. */
            const tc_frame_buf_t *r = dec->dpb[0].frame;
            if (r) {
                tc_inter_predict_chroma_decoder(r->cb, r->stride_c, dec->width/2, dec->height/2, mv, cbuf[0], cu/2, cu/2);
                tc_inter_predict_chroma_decoder(r->cr, r->stride_c, dec->width/2, dec->height/2, mv, cbuf[1], cu/2, cu/2);
            } else {
                tc_intra_chroma_dc(dec->cur->cb, dec->cur->stride_c, px/2, py/2, cu/2, cbuf[0], cu/2);
                tc_intra_chroma_dc(dec->cur->cr, dec->cur->stride_c, px/2, py/2, cu/2, cbuf[1], cu/2);
            }
            qt_dec_chroma(d, px, py, cu, cpred);
            dec_profile_add(dec, &dec->profile_chroma_ns, chroma_start);
        } else {
            /* Skip: luma prediction only; chroma is deliberately not
             * touched (matches the encoder — both sides hold the same
             * stale values, so reconstruction stays in lockstep). */
            uint64_t copy_start = dec->profile_enabled ? dec_now_ns() : 0;
            for (int y = 0; y < cu; y++)
                memcpy(dec->cur->y + (py + y) * dec->cur->stride_y + px,
                       pred + y * cu, (size_t)cu);
            dec_profile_add(dec, &dec->profile_copy_ns, copy_start);
        }
    }


    /* Update the MV grid exactly as the encoder does: intra cells mark
     * themselves unavailable; inter cells store the CU's absolute MV
     * minus the cell's own position, truncated to int16. */
    int cs = cu/8;
    for (int y=0;y<cs;y++) for (int x=0;x<cs;x++){
        qt_mvcell_t *g = &d->grid[(cy+y)*TC_MVGRID_STRIDE + (cx+x)];
        if (is_intra) {
            g->intra = 1;
        } else {
            g->intra = 0;
            tc_mv_s mvp = qt_dec_mvp(d, cx, cy);
            int base_x = mvp.x + px*4 + ((merge || skip) ? 0 : mvd_x);
            int base_y = mvp.y + py*4 + ((merge || skip) ? 0 : mvd_y);
            g->dx = (int16_t)(base_x - (px + x*8)*4);
            g->dy = (int16_t)(base_y - (py + y*8)*4);
        }
    }
    (void)dct_size;
}

/* Quadtree split recursion.  Nodes fully outside the frame are skipped
 * without any syntax (the encoder writes nothing for them); every
 * in-frame node carries a 1-bit split flag. */
static void qt_dec_split(qt_dec_t *d, int depth, int cx, int cy)
{
    int cu = 8 << (TC_QT_MAX_DEPTH - depth);
    int px = d->ctu_x + cx*8, py = d->ctu_y + cy*8;
    int in_frame = (px + cu <= d->dec->width && py + cu <= d->dec->height);

    /* Child offset in 8×8 cells is half the parent side: 2^(2-depth).
     * Only reachable at depth < 3 (min CU). */
    if (!in_frame && cu > TC_QT_MIN_CU) {
        for (int q=0;q<4;q++)
            qt_dec_split(d, depth+1, cx+((q&1)<<(2-depth)), cy+((q>>1)<<(2-depth)));
        return;
    }
    if (!in_frame) return;

    uint32_t split = qt_read_bits(d, RC_CTX_QT_SPLIT + depth, 1);
    /* A split flag at the minimum CU size is malformed. */
    if (split && cu == TC_QT_MIN_CU) {
        d->bs->error = 1;
        return;
    }
    if (split) {
        for (int q=0;q<4;q++)
            qt_dec_split(d, depth+1, cx+((q&1)<<(2-depth)), cy+((q>>1)<<(2-depth)));
    } else {
        qt_dec_leaf(d, depth, cx, cy);
    }
}

/* Decode one CTU (quadtree) and deblock it when fully in-frame. */
static void decode_ctu_v2(tc_decoder_t *dec, int row, int col, int qp,
                          tc_frame_type_t frame_type, int poc,
                          tc_bs_reader_t *bs, tc_tans_dec_t *tans,
                          tc_rc_dec_t *rc, tc_rc_ctx_t *rc_ctx)
{
    qt_dec_t d;
    memset(&d, 0, sizeof(d));
    d.dec = dec;
    d.ctu_x = col * TC_CTU_SIZE;
    d.ctu_y = row * TC_CTU_SIZE;
    d.qp = qp;
    d.qp_c = tc_clip(qp + 1, 0, 63);
    d.frame_type = frame_type;
    d.poc = poc;
    d.bs = bs; d.tans = tans; d.rc = rc; d.rc_ctx = rc_ctx;
    tc_build_eff_scale_table(qp, d.eff_scale);
    tc_build_eff_scale_table(d.qp_c, d.eff_c_scale);
    for (int i = 0; i < TC_MVGRID_STRIDE*TC_MVGRID_STRIDE; i++) d.grid[i].intra = 1;
    qt_dec_split(&d, 0, 0, 0);
    if (d.ctu_x + TC_CTU_SIZE <= dec->width && d.ctu_y + TC_CTU_SIZE <= dec->height) {
        uint64_t deblock_start = dec->profile_enabled ? dec_now_ns() : 0;
        tc_deblock_ctu(dec->cur->y, dec->cur->stride_y,
                       dec->cur->cb, dec->cur->stride_c,
                       dec->cur->cr, dec->cur->stride_c,
                       d.ctu_x, d.ctu_y, qp);
        dec_profile_add(dec, &dec->profile_deblock_ns, deblock_start);
    }

    /* v2 SAO is signaled by the frame tool flags. Older v2 streams
     * without the flag remain decodable and carry no SAO syntax. */
    if (dec->last_header.tool_flags & TC_TOOL_SAO) {
        uint32_t has_sao = qt_read_bits(&d, RC_CTX_SAO_TYPE, 1);
        if (has_sao > 1) { d.bs->error = 1; return; }
        if (has_sao) {
            uint32_t band = qt_read_bits(&d, RC_CTX_SAO_BAND, 5);
            /* Offset is the v2 bounded mapped nibble 0..14 (-7..+7).
             * 15 is reserved and must be rejected, not clamped. */
            uint32_t offset_code = qt_read_bits(&d, RC_CTX_SAO_OFFSET, 4);
            if (band > 31 || offset_code > 14) {
                d.bs->error = 1;
                return;
            }
            int32_t offset = (int32_t)offset_code - 7;
            tc_sao_ctu_luma(dec->cur->y, dec->cur->stride_y,
                            d.ctu_x, d.ctu_y, dec->width, dec->height,
                            (int)band, (int)offset);
        }
    }
}

static void decode_row_v2(tc_decoder_t *dec, int row, int qp,
                          tc_frame_type_t frame_type, int poc,
                          tc_bs_reader_t *bs, tc_tans_dec_t *tans,
                          tc_rc_dec_t *rc, tc_rc_ctx_t *rc_ctx)
{
    for (int col = 0; col < dec->num_ctu_cols; col++) {
        decode_ctu_v2(dec, row, col, qp, frame_type, poc, bs, tans, rc, rc_ctx);
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

/* ── Parsed v2 frame and dependency-safe reconstruction ─────────────
 *
 * Range-coded v2 syntax is serial, but its expensive pixel work is not.
 * Parse each CTU once into a compact command/coefficient store, then
 * reconstruct CTUs with a dependency scheduler.  This keeps the exact
 * bitstream and entropy state while allowing independent CTUs to use all
 * Cortex-A72 cores.  Intra prediction requires only the reconstructed
 * above and left CTUs; the scheduler therefore releases (row,col) after
 * (row-1,col) and (row,col-1) complete.
 * ──────────────────────────────────────────────────────────────── */

#define V2_CMD_MAX_COEFFS 16384 /* 64 min-CUs × (64 luma + 128 chroma) */

typedef struct {
    uint8_t valid, split, intra, ch_intra, skip, merge, ref_sel;
    uint8_t dct_size, intra_mode;
    uint16_t luma_flag_count;
    int32_t mvd_x, mvd_y, mv_x, mv_y;
    uint16_t luma_off, luma_count, chroma_off, chroma_count;
    uint8_t luma_flags[320];
} v2_cmd_leaf_t;

typedef struct {
    v2_cmd_leaf_t node[TC_QT_NODES];
    int16_t coeff[V2_CMD_MAX_COEFFS];
    uint16_t coeff_count;
    uint8_t sao_has, sao_band;
    int8_t sao_offset;
} v2_cmd_ctu_t;

typedef struct {
    tc_decoder_t *dec;
    v2_cmd_ctu_t *cmd;
    int ctu_x, ctu_y, qp;
    tc_frame_type_t frame_type;
    int poc;
    tc_bs_reader_t *bs;
    tc_tans_dec_t *tans;
    tc_rc_dec_t *rc;
    tc_rc_ctx_t *rc_ctx;
    qt_mvcell_t grid[TC_MVGRID_STRIDE * TC_MVGRID_STRIDE];
} v2_parse_ctx_t;

static TCODEC_FORCEINLINE uint32_t v2_parse_bits(v2_parse_ctx_t *p,
                                                  int base, int nbits)
{
    uint64_t start = p->dec->profile_enabled ? dec_now_ns() : 0;
    uint32_t v = dec_read_bits(p->bs, p->rc, p->rc_ctx, base, nbits);
    dec_profile_add(p->dec, &p->dec->profile_parse_ns, start);
    return v;
}

static TCODEC_FORCEINLINE int32_t v2_parse_se(v2_parse_ctx_t *p, int base)
{
    uint64_t start = p->dec->profile_enabled ? dec_now_ns() : 0;
    int32_t v = dec_read_se(p->bs, p->rc, p->rc_ctx, base);
    dec_profile_add(p->dec, &p->dec->profile_parse_ns, start);
    return v;
}

static int v2_cmd_push_coeffs(v2_parse_ctx_t *p, int n,
                               uint16_t *off_out)
{
    if ((int)p->cmd->coeff_count + n > V2_CMD_MAX_COEFFS) {
        p->bs->error = 1;
        return 0;
    }
    uint16_t off = p->cmd->coeff_count;
    uint64_t start = p->dec->profile_enabled ? dec_now_ns() : 0;
    dec_read_coeffs(p->tans, p->rc, p->rc_ctx,
                    p->cmd->coeff + off, n,
                    n == 64 ? TC_BLOCK_8x8_ID : TC_BLOCK_4x4_ID);
    dec_profile_add(p->dec, &p->dec->profile_coeff_ns, start);
    p->cmd->coeff_count = (uint16_t)(off + n);
    *off_out = off;
    return 1;
}

static void v2_parse_luma(v2_parse_ctx_t *p, v2_cmd_leaf_t *n, int cu)
{
    n->luma_off = p->cmd->coeff_count;
    n->luma_count = 0;
    n->luma_flag_count = 0;
    int ntu = cu / 8;
    for (int ty = 0; ty < ntu; ty++) for (int tx = 0; tx < ntu; tx++) {
        uint32_t dct = v2_parse_bits(p, RC_CTX_DCT_SIZE, 1);
        if (n->luma_flag_count < sizeof(n->luma_flags))
            n->luma_flags[n->luma_flag_count++] = (uint8_t)dct;
        if (dct == TC_BLOCK_8x8_ID) {
            uint16_t off;
            if (!v2_cmd_push_coeffs(p, 64, &off)) return;
            n->luma_count = (uint16_t)(n->luma_count + 64);
        } else {
            for (int q = 0; q < 4; q++) {
                uint32_t sub_dct = v2_parse_bits(p, RC_CTX_DCT_SIZE, 1);
                if (n->luma_flag_count < sizeof(n->luma_flags))
                    n->luma_flags[n->luma_flag_count++] = (uint8_t)sub_dct;
                uint16_t off;
                if (!v2_cmd_push_coeffs(p, 16, &off)) return;
                n->luma_count = (uint16_t)(n->luma_count + 16);
            }
        }
    }
}

static void v2_parse_chroma(v2_parse_ctx_t *p, v2_cmd_leaf_t *n, int cu)
{
    n->chroma_off = p->cmd->coeff_count;
    n->chroma_count = 0;
    int cs = cu / 2;
    for (int comp = 0; comp < 2; comp++)
        for (int ty = 0; ty < cs / 4; ty++)
            for (int tx = 0; tx < cs / 4; tx++) {
                uint16_t off;
                if (!v2_cmd_push_coeffs(p, 16, &off)) return;
                n->chroma_count = (uint16_t)(n->chroma_count + 16);
            }
}

static void v2_parse_leaf(v2_parse_ctx_t *p, int depth, int cx, int cy)
{
    int cu = 8 << (TC_QT_MAX_DEPTH - depth);
    int px = p->ctu_x + cx * 8, py = p->ctu_y + cy * 8;
    int idx = tc_qt_index(depth, cx, cy);
    v2_cmd_leaf_t *n = &p->cmd->node[idx];
    memset(n, 0, sizeof(*n));
    n->valid = 1;
    n->intra = (uint8_t)v2_parse_bits(p, RC_CTX_BLOCK_MODE, 1);
    if (n->intra) {
        uint32_t im = v2_parse_bits(p, RC_CTX_INTRA_MODE, 5);
        if (im >= TC_INTRA_MODES) { p->bs->error = 1; im = TC_INTRA_DC; }
        n->intra_mode = (uint8_t)im;
        n->ch_intra = (uint8_t)v2_parse_bits(p, RC_CTX_BLOCK_MODE, 1);
        if (n->ch_intra)
            (void)v2_parse_bits(p, RC_CTX_INTRA_MODE, 3);
    } else {
        if (p->frame_type == TC_FRAME_BIDIR) {
            n->ref_sel = (uint8_t)v2_parse_bits(p, RC_CTX_REF_SEL, 1);
            (void)v2_parse_bits(p, RC_CTX_BLOCK_MODE, 1);
            if (n->ref_sel > 1) p->bs->error = 1;
        }
        n->skip = (uint8_t)v2_parse_bits(p, RC_CTX_SKIP_FLAG, 1);
        if (!n->skip) {
            n->merge = (uint8_t)v2_parse_bits(p, RC_CTX_MERGE_FLAG, 1);
            if (!n->merge) {
                n->dct_size = (uint8_t)v2_parse_bits(p, RC_CTX_DCT_SIZE, 1);
                n->mvd_x = v2_parse_se(p, RC_CTX_MVD_X);
                n->mvd_y = v2_parse_se(p, RC_CTX_MVD_Y);
                if (n->mvd_x < -32768 || n->mvd_x > 32767 ||
                    n->mvd_y < -32768 || n->mvd_y > 32767)
                    p->bs->error = 1;
            }
        }
    }

    /* Same median predictor as qt_dec_mvp(), using the parser's private
     * syntax-only grid.  No reconstructed pixels are touched here. */
    tc_mv_s ma = {0,0}, mb = {0,0}, mc = {0,0};
    int ha = 0, hb = 0, hc = 0;
    if (cx > 0) { qt_mvcell_t *g = &p->grid[cy * TC_MVGRID_STRIDE + cx - 1];
        if (!g->intra) { ma.x = g->dx; ma.y = g->dy; ha = 1; } }
    if (cy > 0) { qt_mvcell_t *g = &p->grid[(cy - 1) * TC_MVGRID_STRIDE + cx];
        if (!g->intra) { mb.x = g->dx; mb.y = g->dy; hb = 1; } }
    if (cy > 0 && cx < 7) { qt_mvcell_t *g = &p->grid[(cy - 1) * TC_MVGRID_STRIDE + cx + 1];
        if (!g->intra) { mc.x = g->dx; mc.y = g->dy; hc = 1; } }
    if (!ha && !hb && !hc) { ma.x = ma.y = mb.x = mb.y = mc.x = mc.y = 0; }
    else {
        if (!ha) ma = hb ? mb : mc;
        if (!hb) mb = ha ? ma : mc;
        if (!hc) mc = ha ? ma : mb;
    }
    int vx[3] = { ma.x, mb.x, mc.x }, vy[3] = { ma.y, mb.y, mc.y };
    for (int k = 0; k < 2; k++) for (int j = k + 1; j < 3; j++) {
        if (vx[k] > vx[j]) { int t = vx[k]; vx[k] = vx[j]; vx[j] = t; }
        if (vy[k] > vy[j]) { int t = vy[k]; vy[k] = vy[j]; vy[j] = t; }
    }
    tc_mv_s mvp = { vx[1], vy[1] };
    n->mv_x = (int16_t)(mvp.x + px * 4 + ((n->skip || n->merge || n->intra) ? 0 : n->mvd_x));
    n->mv_y = (int16_t)(mvp.y + py * 4 + ((n->skip || n->merge || n->intra) ? 0 : n->mvd_y));

    if (!n->intra && !n->skip) {
        v2_parse_luma(p, n, cu);
        v2_parse_chroma(p, n, cu);
    } else if (n->intra) {
        v2_parse_luma(p, n, cu);
        v2_parse_chroma(p, n, cu);
    }

    int cs = cu / 8;
    for (int y = 0; y < cs; y++) for (int x = 0; x < cs; x++) {
        qt_mvcell_t *g = &p->grid[(cy + y) * TC_MVGRID_STRIDE + (cx + x)];
        if (n->intra) g->intra = 1;
        else {
            g->intra = 0;
            g->dx = (int16_t)(n->mv_x - (px + x * 8) * 4);
            g->dy = (int16_t)(n->mv_y - (py + y * 8) * 4);
        }
    }
}

static void v2_parse_split(v2_parse_ctx_t *p, int depth, int cx, int cy)
{
    int cu = 8 << (TC_QT_MAX_DEPTH - depth);
    int px = p->ctu_x + cx * 8, py = p->ctu_y + cy * 8;
    int in_frame = px + cu <= p->dec->width && py + cu <= p->dec->height;
    if (!in_frame && cu > TC_QT_MIN_CU) {
        for (int q = 0; q < 4; q++)
            v2_parse_split(p, depth + 1,
                           cx + ((q & 1) << (2 - depth)),
                           cy + ((q >> 1) << (2 - depth)));
        return;
    }
    if (!in_frame) return;
    int idx = tc_qt_index(depth, cx, cy);
    uint32_t split = v2_parse_bits(p, RC_CTX_QT_SPLIT + depth, 1);
    p->cmd->node[idx].split = (uint8_t)split;
    if (split && cu == TC_QT_MIN_CU) { p->bs->error = 1; return; }
    if (split) for (int q = 0; q < 4; q++)
        v2_parse_split(p, depth + 1,
                       cx + ((q & 1) << (2 - depth)),
                       cy + ((q >> 1) << (2 - depth)));
    else v2_parse_leaf(p, depth, cx, cy);
}

static void v2_parse_ctu(tc_decoder_t *dec, int row, int col, int qp,
                         tc_frame_type_t frame_type, int poc,
                         tc_bs_reader_t *bs, tc_tans_dec_t *tans,
                         tc_rc_dec_t *rc, tc_rc_ctx_t *rc_ctx,
                         v2_cmd_ctu_t *cmd)
{
    v2_parse_ctx_t p;
    memset(&p, 0, sizeof(p));
    p.dec = dec; p.cmd = cmd; p.ctu_x = col * TC_CTU_SIZE;
    p.ctu_y = row * TC_CTU_SIZE; p.qp = qp; p.frame_type = frame_type;
    p.poc = poc; p.bs = bs; p.tans = tans; p.rc = rc; p.rc_ctx = rc_ctx;
    for (int i = 0; i < TC_MVGRID_STRIDE * TC_MVGRID_STRIDE; i++) p.grid[i].intra = 1;
    v2_parse_split(&p, 0, 0, 0);
    if (dec->last_header.tool_flags & TC_TOOL_SAO) {
        uint32_t has = v2_parse_bits(&p, RC_CTX_SAO_TYPE, 1);
        cmd->sao_has = (uint8_t)has;
        if (has) {
            uint32_t band = v2_parse_bits(&p, RC_CTX_SAO_BAND, 5);
            uint32_t off = v2_parse_bits(&p, RC_CTX_SAO_OFFSET, 4);
            if (band > 31 || off > 14) { bs->error = 1; return; }
            cmd->sao_band = (uint8_t)band;
            cmd->sao_offset = (int8_t)off - 7;
        }
    }
}

static void v2_recon_luma(tc_decoder_t *dec, const v2_cmd_ctu_t *cmd,
                          const v2_cmd_leaf_t *n, int px, int py, int cu,
                          const int eff[4][64], const tc_pixel_t *pred)
{
    int rs = dec->cur->stride_y, fi = 0, ci = n->luma_off;
    int ntu = cu / 8;
    for (int ty = 0; ty < ntu; ty++) for (int tx = 0; tx < ntu; tx++) {
        int ox = tx * 8, oy = ty * 8;
        uint8_t dct = n->luma_flags[fi++];
        if (dct == TC_BLOCK_8x8_ID) {
            tc_coeff_t iq[64], res[64];
            int nonzero = 0, dc_only = 1;
            for (int i = 0; i < 64; i++) {
                int b = qt_band8[i];
                int q = cmd->coeff[ci++];
                iq[i] = (tc_coeff_t)tc_dequant_coeff(q, eff[b][i]);
                if (iq[i]) nonzero = 1;
                if (i && iq[i]) dc_only = 0;
            }
            if (!nonzero) {
                for (int y = 0; y < 8; y++) memcpy(dec->cur->y + (py + oy + y) * rs + px + ox,
                                                      pred + (oy + y) * cu + ox, 8);
            } else if (dc_only) {
                tc_recon_add_dc8x8(pred + oy * cu + ox, cu, tc_idct_dc8_res(iq[0]),
                                    dec->cur->y + (py + oy) * rs + px + ox, rs);
            } else {
                tc_idct8x8_neon(iq, res, 8);
                tc_recon_add_clip8x8(pred + oy * cu + ox, cu, res,
                                     dec->cur->y + (py + oy) * rs + px + ox, rs);
            }
        } else {
            for (int q = 0; q < 4; q++) {
                (void)n->luma_flags[fi++];
                tc_coeff_t iq[16], res[16];
                int nonzero = 0, dc_only = 1;
                for (int i = 0; i < 16; i++) {
                    int b = qt_band4[i];
                    int qv = cmd->coeff[ci++];
                    iq[i] = (tc_coeff_t)tc_dequant_coeff(qv, eff[b][i]);
                    if (iq[i]) nonzero = 1;
                    if (i && iq[i]) dc_only = 0;
                }
                int sx = (q & 1) * 4, sy = (q & 2) * 2;
                if (!nonzero) for (int y = 0; y < 4; y++)
                    memcpy(dec->cur->y + (py + oy + sy + y) * rs + px + ox + sx,
                           pred + (oy + sy + y) * cu + ox + sx, 4);
                else if (dc_only) tc_recon_add_dc4x4(pred + (oy + sy) * cu + ox + sx, cu,
                                                       tc_idct_dc4_res(iq[0]),
                                                       dec->cur->y + (py + oy + sy) * rs + px + ox + sx, rs);
                else {
                    tc_idct4x4_neon(iq, res, 4);
                    tc_recon_add_clip4x4(pred + (oy + sy) * cu + ox + sx, cu, res,
                                         dec->cur->y + (py + oy + sy) * rs + px + ox + sx, rs);
                }
            }
        }
    }
}

static void v2_recon_chroma(tc_decoder_t *dec, const v2_cmd_ctu_t *cmd,
                            const v2_cmd_leaf_t *n, int px, int py, int cu,
                            const int eff_c[4][64], const tc_pixel_t *pred[2])
{
    int cs = cu / 2, rs = dec->cur->stride_c, ci = n->chroma_off;
    tc_pixel_t *rec[2] = { dec->cur->cb, dec->cur->cr };
    for (int comp = 0; comp < 2; comp++) for (int ty = 0; ty < cs / 4; ty++)
        for (int tx = 0; tx < cs / 4; tx++) {
            int ox = tx * 4, oy = ty * 4;
            tc_coeff_t iq[16], res[16];
            int nonzero = 0, dc_only = 1;
            for (int i = 0; i < 16; i++) {
                int q = cmd->coeff[ci++];
                int b = qt_band4[i];
                iq[i] = (tc_coeff_t)tc_dequant_coeff(q, eff_c[b][0]);
                if (iq[i]) nonzero = 1;
                if (i && iq[i]) dc_only = 0;
            }
            if (!nonzero) for (int y = 0; y < 4; y++)
                memcpy(rec[comp] + (py / 2 + oy + y) * rs + px / 2 + ox,
                       pred[comp] + (oy + y) * cs + ox, 4);
            else if (dc_only) {
                int v = ((int)iq[0] * 8192 + 8192) >> 14;
                v = (v * 8192 + 8192) >> 14;
                tc_recon_add_dc4x4(pred[comp] + oy * cs + ox, cs, v,
                                    rec[comp] + (py / 2 + oy) * rs + px / 2 + ox, rs);
            }
            else {
                /* Chroma residuals use the same fixed-point DCT-II as the
                 * encoder.  WHT is a different transform and silently
                 * breaks bit-exact v2 reconstruction on AC coefficients. */
                tc_idct4x4_res(iq, res, 4);
                tc_recon_add_clip4x4(pred[comp] + oy * cs + ox, cs, res,
                                     rec[comp] + (py / 2 + oy) * rs + px / 2 + ox, rs);
            }
        }
}

static void v2_recon_leaf(tc_decoder_t *dec, const v2_cmd_ctu_t *cmd,
                          const v2_cmd_leaf_t *n, int px, int py, int cu,
                          const int eff[4][64], const int eff_c[4][64],
                          tc_frame_type_t frame_type, int poc)
{
    tc_pixel_t pred[TC_CTU_SIZE * TC_CTU_SIZE];
    tc_pixel_t cbuf[2][(TC_CTU_SIZE / 2) * (TC_CTU_SIZE / 2)];
    tc_pixel_t bip_a[TC_CTU_SIZE * TC_CTU_SIZE], bip_b[TC_CTU_SIZE * TC_CTU_SIZE];
    const tc_pixel_t *cpred[2] = { cbuf[0], cbuf[1] };
    int is_intra = n->intra;
    uint64_t motion_start = dec->profile_enabled ? dec_now_ns() : 0;
    if (is_intra) {
        tc_pixel_t ra[2 * 64 + 1], rl[2 * 64 + 1];
        tc_intra_get_ref_v2(dec->cur->y, dec->cur->stride_y, px, py, cu,
                            dec->width, dec->height, ra + 1, rl + 1);
        tc_intra_predict(pred, cu, ra + 1, rl + 1, cu,
                         (tc_intra_mode_t)n->intra_mode);
    } else {
        const tc_frame_buf_t *r = dec->dpb[0].frame;
        tc_mv_s mv = { n->mv_x, n->mv_y };
        if (frame_type == TC_FRAME_BIDIR) {
            const tc_frame_buf_t *r1 = n->ref_sel ? dec->dpb[1].frame : dec->dpb[0].frame;
            const tc_frame_buf_t *r2 = n->ref_sel ? dec->dpb[0].frame : dec->dpb[1].frame;
            if (r1 && r2) {
                tc_inter_predict_decoder(r1->y, r1->stride_y, r1->width, r1->height, mv, bip_a, cu, cu);
                tc_mv_s mv2 = { -mv.x, -mv.y };
                tc_inter_predict_decoder(r2->y, r2->stride_y, r2->width, r2->height, mv2, bip_b, cu, cu);
                for (int i = 0; i < cu * cu; i++) pred[i] = (tc_pixel_t)((bip_a[i] + bip_b[i] + 1) >> 1);
            } else if (r1) tc_inter_predict_decoder(r1->y, r1->stride_y, r1->width, r1->height, mv, pred, cu, cu);
            else memset(pred, 128, (size_t)cu * cu);
        } else if (r) tc_inter_predict_decoder(r->y, r->stride_y, r->width, r->height, mv, pred, cu, cu);
        else memset(pred, 128, (size_t)cu * cu);
        dec_profile_add(dec, &dec->profile_motion_ns, motion_start);
    }
    if (n->skip) {
        uint64_t copy_start = dec->profile_enabled ? dec_now_ns() : 0;
        for (int y = 0; y < cu; y++) memcpy(dec->cur->y + (py + y) * dec->cur->stride_y + px,
                                             pred + y * cu, (size_t)cu);
        dec_profile_add(dec, &dec->profile_copy_ns, copy_start);
        return;
    }
    uint64_t transform_start = dec->profile_enabled ? dec_now_ns() : 0;
    v2_recon_luma(dec, cmd, n, px, py, cu, eff, pred);
    dec_profile_add(dec, &dec->profile_transform_ns, transform_start);
    uint64_t chroma_start = dec->profile_enabled ? dec_now_ns() : 0;
    if (is_intra) {
        if (n->ch_intra) {
            tc_intra_chroma_dc(dec->cur->cb, dec->cur->stride_c, px / 2, py / 2, cu / 2, cbuf[0], cu / 2);
            tc_intra_chroma_dc(dec->cur->cr, dec->cur->stride_c, px / 2, py / 2, cu / 2, cbuf[1], cu / 2);
        } else {
            const tc_frame_buf_t *r = dec->dpb[0].frame;
            tc_mv_s mv = { n->mv_x, n->mv_y };
            if (r) {
                tc_inter_predict_chroma_decoder(r->cb, r->stride_c, r->width / 2, r->height / 2, mv, cbuf[0], cu / 2, cu / 2);
                tc_inter_predict_chroma_decoder(r->cr, r->stride_c, r->width / 2, r->height / 2, mv, cbuf[1], cu / 2, cu / 2);
            } else {
                tc_intra_chroma_dc(dec->cur->cb, dec->cur->stride_c, px / 2, py / 2, cu / 2, cbuf[0], cu / 2);
                tc_intra_chroma_dc(dec->cur->cr, dec->cur->stride_c, px / 2, py / 2, cu / 2, cbuf[1], cu / 2);
            }
        }
    } else {
        const tc_frame_buf_t *r = dec->dpb[0].frame;
        if (r) {
            tc_inter_predict_chroma_decoder(r->cb, r->stride_c, r->width / 2, r->height / 2,
                                             (tc_mv_s){ n->mv_x, n->mv_y }, cbuf[0], cu / 2, cu / 2);
            tc_inter_predict_chroma_decoder(r->cr, r->stride_c, r->width / 2, r->height / 2,
                                             (tc_mv_s){ n->mv_x, n->mv_y }, cbuf[1], cu / 2, cu / 2);
        } else {
            tc_intra_chroma_dc(dec->cur->cb, dec->cur->stride_c, px / 2, py / 2, cu / 2, cbuf[0], cu / 2);
            tc_intra_chroma_dc(dec->cur->cr, dec->cur->stride_c, px / 2, py / 2, cu / 2, cbuf[1], cu / 2);
        }
    }
    v2_recon_chroma(dec, cmd, n, px, py, cu, eff_c, cpred);
    dec_profile_add(dec, &dec->profile_chroma_ns, chroma_start);
}

static void v2_recon_split(tc_decoder_t *dec, const v2_cmd_ctu_t *cmd,
                           int depth, int cx, int cy,
                           const int eff[4][64], const int eff_c[4][64],
                           tc_frame_type_t frame_type, int poc,
                           int ctu_x, int ctu_y)
{
    int cu = 8 << (TC_QT_MAX_DEPTH - depth);
    int px = ctu_x + (cx * 8), py = ctu_y + (cy * 8);
    /* Parse-time geometry is CTU-local, but reconstruction must also
     * reject leaves beyond a partial frame edge. */
    int in_frame = (ctu_x + cx * 8 + cu <= dec->width &&
                    ctu_y + cy * 8 + cu <= dec->height);
    if (!in_frame && cu > TC_QT_MIN_CU) {
        for (int q = 0; q < 4; q++) v2_recon_split(dec, cmd, depth + 1,
            cx + ((q & 1) << (2 - depth)), cy + ((q >> 1) << (2 - depth)),
            eff, eff_c, frame_type, poc, ctu_x, ctu_y);
        return;
    }
    if (!in_frame) return;
    int idx = tc_qt_index(depth, cx, cy);
    if (cmd->node[idx].split) for (int q = 0; q < 4; q++)
        v2_recon_split(dec, cmd, depth + 1,
            cx + ((q & 1) << (2 - depth)), cy + ((q >> 1) << (2 - depth)),
            eff, eff_c, frame_type, poc, ctu_x, ctu_y);
    else {
        int absx = (cmd->node[idx].valid ? 0 : 0); (void)absx;
        /* The CTU origin is supplied by the caller through cmd-local fields
         * in v2_recon_ctu; this helper's cx/cy are local 8x8 coordinates. */
        v2_recon_leaf(dec, cmd, &cmd->node[idx],
                      px, py, cu, eff, eff_c, frame_type, poc);
    }
}

static void v2_recon_ctu(tc_decoder_t *dec, const v2_cmd_ctu_t *cmd,
                         int row, int col, int qp, tc_frame_type_t frame_type, int poc)
{
    /* Reconstruct helpers address local coordinates; offset the frame base
     * temporarily by using a CTU-origin wrapper below. */
    /* The helpers use absolute frame coordinates.  Their local coordinates
     * are translated by this small command-independent wrapper by passing a
     * CTU origin through the command traversal. */
    int ox = col * TC_CTU_SIZE, oy = row * TC_CTU_SIZE;
    int eff[4][64], eff_c[4][64];
    tc_build_eff_scale_table(qp, eff);
    tc_build_eff_scale_table(tc_clip(qp + 1, 0, 63), eff_c);
    /* Inline traversal with absolute coordinates to avoid mutable globals. */
    /* A local recursive lambda is not C; use the explicit worker below. */
    v2_recon_split(dec, cmd, 0, 0, 0, eff, eff_c, frame_type, poc, ox, oy);
    if (ox + TC_CTU_SIZE <= dec->width && oy + TC_CTU_SIZE <= dec->height) {
        uint64_t deblock_start = dec->profile_enabled ? dec_now_ns() : 0;
        tc_deblock_ctu(dec->cur->y, dec->cur->stride_y,
                       dec->cur->cb, dec->cur->stride_c,
                       dec->cur->cr, dec->cur->stride_c, ox, oy, qp);
        dec_profile_add(dec, &dec->profile_deblock_ns, deblock_start);
    }
    if (cmd->sao_has) tc_sao_ctu_luma(dec->cur->y, dec->cur->stride_y,
                                      ox, oy, dec->width, dec->height,
                                      cmd->sao_band, cmd->sao_offset);
}

#if !defined(TCODEC_NO_THREADS)
typedef struct {
    tc_decoder_t *dec;
    v2_cmd_ctu_t *cmds;
    int rows, cols, qp;
    tc_frame_type_t frame_type;
    int poc;
    uint8_t *done, *deps;
    int *ready;
    int ready_head, ready_tail;
    int remaining;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} v2_wave_ctx_t;

static void v2_ready_push(v2_wave_ctx_t *w, int task)
{
    w->ready[w->ready_tail++] = task;
}

static void *v2_wave_worker(void *opaque)
{
    v2_wave_ctx_t *w = (v2_wave_ctx_t *)opaque;
    for (;;) {
        int task;
        pthread_mutex_lock(&w->mutex);
        while (w->ready_head == w->ready_tail && w->remaining != 0)
            pthread_cond_wait(&w->cond, &w->mutex);
        if (w->remaining == 0) {
            pthread_mutex_unlock(&w->mutex);
            return NULL;
        }
        task = w->ready[w->ready_head++];
        pthread_mutex_unlock(&w->mutex);

        v2_recon_ctu(w->dec, &w->cmds[task], task / w->cols,
                     task % w->cols, w->qp, w->frame_type, w->poc);

        pthread_mutex_lock(&w->mutex);
        w->remaining--;
        int row = task / w->cols, col = task % w->cols;
        int released = 0;
        if (col + 1 < w->cols) {
            int next = task + 1;
            if (--w->deps[next] == 0) {
                v2_ready_push(w, next);
                released++;
            }
        }
        if (row + 1 < w->rows) {
            int next = task + w->cols;
            if (--w->deps[next] == 0) {
                v2_ready_push(w, next);
                released++;
            }
        }
        /* A completion either releases one or two CTUs, or finishes the
         * frame.  Wake exactly as many sleepers as there is new work; the
         * old unconditional broadcast woke every worker for every CTU and
         * made the dependency mutex the dominant scheduler cost.  The final
         * broadcast is required so sleepers observe remaining == 0. */
        if (w->remaining == 0)
            pthread_cond_broadcast(&w->cond);
        else if (released > 0 || w->ready_head != w->ready_tail)
            /* Wake one worker for each newly released CTU.  This keeps the
             * wavefront's ready queue from parking workers when two
             * successors become runnable at once. */
            for (int i = 0; i < (released > 0 ? released : 1); i++)
                pthread_cond_signal(&w->cond);
        pthread_mutex_unlock(&w->mutex);
    }
}
#endif

#if !defined(TCODEC_NO_THREADS)
typedef struct {
    pthread_t *threads;
    int count;
    int shutdown;
    v2_wave_ctx_t *ctx;
    int finished;
    unsigned gen;   /* Monotonic submission id; distinguishes reused ctx */
    pthread_mutex_t mutex;
    pthread_cond_t cond;
} v2_pool_t;

static void *v2_persistent_worker(void *opaque)
{
    v2_pool_t *pool = (v2_pool_t *)opaque;
    unsigned my_gen = 0;  /* Generation this worker last processed */
    for (;;) {
        pthread_mutex_lock(&pool->mutex);
        while (!pool->shutdown &&
               (pool->ctx == NULL || pool->gen == my_gen))
            pthread_cond_wait(&pool->cond, &pool->mutex);
        if (pool->shutdown) {
            pthread_mutex_unlock(&pool->mutex);
            return NULL;
        }
        v2_wave_ctx_t *ctx = pool->ctx;
        my_gen = pool->gen;
        pthread_mutex_unlock(&pool->mutex);

        v2_wave_worker(ctx);

        pthread_mutex_lock(&pool->mutex);
        pool->finished++;
        pthread_cond_broadcast(&pool->cond);
        pthread_mutex_unlock(&pool->mutex);
    }
}

static v2_pool_t *v2_pool_create(int count)
{
    if (count < 1) count = 1;
    if (count > 8) count = 8;
    v2_pool_t *pool = (v2_pool_t *)calloc(1, sizeof(*pool));
    if (!pool) return NULL;
    pool->threads = (pthread_t *)calloc((size_t)count, sizeof(*pool->threads));
    if (!pool->threads) { free(pool); return NULL; }
    pool->count = count;
    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);
    for (int i = 0; i < count; i++) {
        if (pthread_create(&pool->threads[i], NULL, v2_persistent_worker, pool) != 0) {
            pool->shutdown = 1;
            pthread_cond_broadcast(&pool->cond);
            for (int j = 0; j < i; j++) pthread_join(pool->threads[j], NULL);
            pthread_cond_destroy(&pool->cond);
            pthread_mutex_destroy(&pool->mutex);
            free(pool->threads);
            free(pool);
            return NULL;
        }
    }
    return pool;
}

static void v2_pool_destroy(v2_pool_t *pool)
{
    if (!pool) return;
    pthread_mutex_lock(&pool->mutex);
    pool->shutdown = 1;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    for (int i = 0; i < pool->count; i++) pthread_join(pool->threads[i], NULL);
    pthread_cond_destroy(&pool->cond);
    pthread_mutex_destroy(&pool->mutex);
    free(pool->threads);
    free(pool);
}

static void v2_pool_submit(v2_pool_t *pool, v2_wave_ctx_t *ctx)
{
    pthread_mutex_lock(&pool->mutex);
    pool->ctx = ctx;
    pool->gen++;          /* New generation wakes every idle worker */
    pool->finished = 0;
    pthread_cond_broadcast(&pool->cond);
    while (pool->finished != pool->count)
        pthread_cond_wait(&pool->cond, &pool->mutex);
    /* Every worker has returned from v2_wave_worker and detached from the
     * frame context before the caller frees its command storage. */
    pool->ctx = NULL;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
}
#endif

static int v2_decode_frame_parallel(tc_decoder_t *dec, int qp,
                                    tc_frame_type_t frame_type, int poc,
                                    tc_bs_reader_t *bs, tc_tans_dec_t *tans,
                                    tc_rc_dec_t *rc, tc_rc_ctx_t *rc_ctx)
{
    int rows = dec->num_ctu_rows, cols = dec->num_ctu_cols;
    size_t count = (size_t)rows * (size_t)cols;
    v2_cmd_ctu_t *cmds = (v2_cmd_ctu_t *)calloc(count, sizeof(*cmds));
    if (!cmds) return 0;
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++)
        v2_parse_ctu(dec, r, c, qp, frame_type, poc, bs, tans, rc, rc_ctx,
                      &cmds[r * cols + c]);
    if (bs->error) { free(cmds); return 0; }
#if defined(TCODEC_NO_THREADS)
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++)
        v2_recon_ctu(dec, &cmds[r * cols + c], r, c, qp, frame_type, poc);
#else
    v2_wave_ctx_t w;
    memset(&w, 0, sizeof(w)); w.dec = dec; w.cmds = cmds; w.rows = rows;
    w.cols = cols; w.qp = qp; w.frame_type = frame_type; w.poc = poc;
    w.deps = (uint8_t *)calloc(count, 1);
    w.ready = (int *)malloc(count * sizeof(*w.ready));
    if (!w.deps || !w.ready) { free(w.deps); free(w.ready); free(cmds); return 0; }
    for (int r = 0; r < rows; r++) for (int c = 0; c < cols; c++)
        w.deps[r * cols + c] = (uint8_t)((r > 0) + (c > 0));
    w.ready_head = w.ready_tail = 0;
    v2_ready_push(&w, 0);
    w.remaining = rows * cols;
    pthread_mutex_init(&w.mutex, NULL); pthread_cond_init(&w.cond, NULL);
    v2_pool_t *pool = (v2_pool_t *)dec->v2_pool;
    if (!pool) {
        int nt = dec->num_threads > 0 ? dec->num_threads : 1;
        pthread_t th[8]; if (nt > 8) nt = 8; int made = 0;
        for (int i = 0; i < nt; i++)
            if (pthread_create(&th[made], NULL, v2_wave_worker, &w) == 0) made++;
        if (!made) {
            pthread_mutex_destroy(&w.mutex); pthread_cond_destroy(&w.cond);
            free(w.deps); free(w.ready); free(cmds); return 0;
        }
        for (int i = 0; i < made; i++) pthread_join(th[i], NULL);
    } else {
        v2_pool_submit(pool, &w);
    }
    pthread_mutex_destroy(&w.mutex); pthread_cond_destroy(&w.cond);
    free(w.deps); free(w.ready);
#endif
    free(cmds);
    return 1;
}

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
    dec->v2_pool = v2_pool_create(dec->num_threads);
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
    v2_pool_destroy((v2_pool_t *)dec->v2_pool);
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

void tc_decoder_set_profile(tc_decoder_t *dec, int enabled)
{
    if (dec) dec->profile_enabled = enabled ? 1 : 0;
}

void tc_decoder_reset_profile(tc_decoder_t *dec)
{
    if (!dec) return;
    dec->profile_parse_ns = 0;
    dec->profile_coeff_ns = 0;
    dec->profile_transform_ns = 0;
    dec->profile_motion_ns = 0;
    dec->profile_chroma_ns = 0;
    dec->profile_deblock_ns = 0;
    dec->profile_copy_ns = 0;
}

void tc_decoder_get_profile(tc_decoder_t *dec,
                            uint64_t *parse_ns, uint64_t *coeff_ns,
                            uint64_t *transform_ns, uint64_t *motion_ns,
                            uint64_t *chroma_ns, uint64_t *deblock_ns,
                            uint64_t *copy_ns)
{
    if (!dec) return;
    if (parse_ns) *parse_ns = dec->profile_parse_ns;
    if (coeff_ns) *coeff_ns = dec->profile_coeff_ns;
    if (transform_ns) *transform_ns = dec->profile_transform_ns;
    if (motion_ns) *motion_ns = dec->profile_motion_ns;
    if (chroma_ns) *chroma_ns = dec->profile_chroma_ns;
    if (deblock_ns) *deblock_ns = dec->profile_deblock_ns;
    if (copy_ns) *copy_ns = dec->profile_copy_ns;
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

    /* v2 uses 4:2:0 chroma and row-wise CU copies; reject odd geometry
     * before allocating or entering the quadtree path. */
    if (hdr.version == TC_VERSION_V2 &&
        ((hdr.width & 1u) || (hdr.height & 1u))) {
        return TC_ERR_BITSTREAM;
    }

    /* Explicit per-frame decoder path dispatch: v2 payloads are never
     * handled by the legacy 8×8 block decoder and vice versa. */
    dec->use_v2 = (hdr.version == TC_VERSION_V2) ? 1 : 0;
    dec->cur_qp = hdr.qp;

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
     * (serial bitstream; WPP+RC to be addressed in Phase 9).
     * v2 (v1 header layout) carries the same tool flag and is
     * range-coded when the flag is set (the --v2 CLI forces it). */
    int use_entropy_coded = 0;
    if (hdr.version != TC_VERSION_V0 && (hdr.tool_flags & TC_TOOL_ENTROPY_CODED)) {
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
    if (hdr.version != TC_VERSION_V0 && hdr.has_crc) {
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

    /* v2 is a sequential (range-coded) format: WPP entry points are
     * never written by the v2 encoder.  A v2 stream claiming WPP is
     * malformed — refuse it instead of mis-parsing the payload. */
    if (dec->use_v2 && is_wpp) {
        return TC_ERR_BITSTREAM;
    }

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

    /* Mirror the encoder: after scene-cut keyframes (poc > 0) the DPB is
     * cleared so no pre-cut frame can be referenced.  Keyframes never
     * use inter prediction, but v2 intra leaves with ch_intra=0 fall
     * back to neighbour-DC chroma when dpb[0] is empty — encoder and
     * decoder must agree on that emptiness. */
    if (hdr.frame_type == TC_FRAME_KEY && hdr.frame_num > 0) {
        for (int i = 0; i < TC_REF_FRAMES; i++) {
            tc_frame_free(dec->dpb[i].frame);
            dec->dpb[i].frame = NULL;
            dec->dpb[i].poc = -1;
        }
    }

    int v2_parallel_done = 0;
    if (dec->use_v2) {
        /* v2 entropy remains serial, but parse results are reconstructed by
         * a dependency-safe wavefront across the four ARM cores. */
        if (!v2_decode_frame_parallel(dec, qp, hdr.frame_type, hdr.frame_num,
                                      &dec->bs, &dec->tans, rc_ptr, rc_ctx_ptr))
            return TC_ERR_BITSTREAM;
        v2_parallel_done = 1;
    }

    if (!use_wpp && !v2_parallel_done) {
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
            if (dec->use_v2) {
                /* Explicit v2 quadtree path — never the legacy decoder. */
                decode_row_v2(dec, row, qp, hdr.frame_type, hdr.frame_num,
                              &dec->bs, &dec->tans, rc_ptr, rc_ctx_ptr);
            } else {
                decode_row_impl(dec, row, qp, hdr.frame_type, hdr.frame_num,
                                &dec->bs, &dec->tans,
                                rc_ptr, rc_ctx_ptr);
            }
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

    if (dec->use_v2 && dec->bs.error)
        return TC_ERR_BITSTREAM;

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
