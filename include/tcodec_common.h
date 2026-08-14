/*
 * tcodec_common.h — Internal shared declarations for TCodec
 *
 * Not part of the public API. Included by all internal source files.
 */

#ifndef TCODEC_COMMON_H
#define TCODEC_COMMON_H

#include "tcodec_types.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>

/* pthread is only needed for WPP thread pool in encoder/decoder.
 * Not required for single-threaded API usage. */
#if !defined(TCODEC_NO_THREADS)
#include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Compiler hints ───────────────────────────────────────────── */

#define TCODEC_UNUSED(x) ((void)(x))

/* ── Clip / min / max ────────────────────────────────────────── */

TCODEC_INLINE int tc_clip(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

TCODEC_INLINE int tc_min(int a, int b) { return a < b ? a : b; }
TCODEC_INLINE int tc_max(int a, int b) { return a > b ? a : b; }
TCODEC_INLINE int tc_abs(int v) { return v < 0 ? -v : v; }

/* ── Fixed-point constants ────────────────────────────────────── */

/* DCT uses 16-bit coefficients with 14-bit fractional precision.
 * Values are stored as int16_t, range [-16384, 16383]. */
#define TC_DCT_BITS      14
#define TC_DCT_SCALE     (1 << TC_DCT_BITS)   /* 16384 */

/* Quantization uses 16.16 fixed point for QP-to-scale mapping. */
#define TC_QUANT_BITS    16
#define TC_QUANT_SCALE   (1 << TC_QUANT_BITS)

/* ── Bitstream reader/writer ─────────────────────────────────── */

typedef struct tc_bs_writer {
    uint8_t *buf;
    size_t   capacity;
    size_t   byte_pos;       /* Current byte position */
    int      bit_pos;        /* Bit position within current byte (0..7) */
} tc_bs_writer_t;

typedef struct tc_bs_reader {
    const uint8_t *buf;
    size_t         size;
    size_t         byte_pos;
    int            bit_pos;
    int            error;       /* Set when a read runs past the buffer. */
} tc_bs_reader_t;

void tc_bs_writer_init(tc_bs_writer_t *w, uint8_t *buf, size_t capacity);
void tc_bs_writer_write_bits(tc_bs_writer_t *w, uint32_t val, int nbits);
void tc_bs_writer_write_ue(tc_bs_writer_t *w, uint32_t val);  /* Exp-Golomb */
void tc_bs_writer_write_se(tc_bs_writer_t *w, int32_t val);   /* Signed EG */
int tc_bs_se_bits(int32_t val);                              /* Signed EG bit count */
void tc_bs_writer_byte_align(tc_bs_writer_t *w);
size_t tc_bs_writer_bytes(tc_bs_writer_t *w);

void tc_bs_reader_init(tc_bs_reader_t *r, const uint8_t *buf, size_t size);
uint32_t tc_bs_reader_read_bits(tc_bs_reader_t *r, int nbits);
uint32_t tc_bs_reader_read_ue(tc_bs_reader_t *r);
int32_t  tc_bs_reader_read_se(tc_bs_reader_t *r);
int      tc_bs_reader_eof(tc_bs_reader_t *r);

/* ── CRC-16 (CCITT) for v1 bitstream error detection ────────── */

uint16_t tc_crc16(const uint8_t *data, size_t len);

/* ── v1 level constraints table ─────────────────────────────── */

typedef struct tc_level_info {
    int32_t  max_width;
    int32_t  max_height;
    int32_t  max_dpb;
    int64_t  max_bitrate;      /* bps */
} tc_level_info_t;

/* Level constraint lookup by level_idx (0 = auto/none) */
const tc_level_info_t *tc_level_get_info(int level_idx);

/* ── Frame buffer management ─────────────────────────────────── */

tc_frame_buf_t *tc_frame_alloc(int width, int height);
void            tc_frame_free(tc_frame_buf_t *frame);
void            tc_frame_copy(tc_frame_buf_t *dst, const tc_frame_buf_t *src);
tc_frame_buf_t *tc_frame_clone(const tc_frame_buf_t *src);

/* ── Color conversion ───────────────────────────────────────── */

void tc_rgb_to_ycbcr_internal(const uint8_t *rgb, int rgb_stride,
                               tc_pixel_t *y,  int stride_y,
                               tc_pixel_t *cb, int stride_cb,
                               tc_pixel_t *cr, int stride_cr,
                               int width, int height);

void tc_ycbcr_to_rgb_internal(const tc_pixel_t *y,  int stride_y,
                               const tc_pixel_t *cb, int stride_cb,
                               const tc_pixel_t *cr, int stride_cr,
                               uint8_t *rgb, int rgb_stride,
                               int width, int height);

/* ── Transform ───────────────────────────────────────────────── */

/* Forward DCT. Input: 8-bit pixels. Output: 16-bit coefficients.
 * For 4×4: in = 4×4 block, out = 4×4 coefficients
 * For 8×8: in = 8×8 block, out = 8×8 coefficients */
void tc_fdct4x4(const tc_pixel_t *TCODEC_RESTRICT in, int stride,
                tc_coeff_t *TCODEC_RESTRICT out);
void tc_fdct8x8(const tc_pixel_t *TCODEC_RESTRICT in, int stride,
                tc_coeff_t *TCODEC_RESTRICT out);

/* Inverse DCT. Input: 16-bit coefficients. Output: 8-bit pixels (clipped). */
void tc_idct4x4(const tc_coeff_t *TCODEC_RESTRICT in,
                tc_pixel_t *TCODEC_RESTRICT out, int stride);
void tc_idct8x8(const tc_coeff_t *TCODEC_RESTRICT in,
                tc_pixel_t *TCODEC_RESTRICT out, int stride);
void tc_fdct4x4_res(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                    tc_coeff_t *TCODEC_RESTRICT out);
void tc_idct4x4_res(const tc_coeff_t *TCODEC_RESTRICT in,
                    tc_coeff_t *TCODEC_RESTRICT out, int stride);
void tc_fdct8x8_res(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                    tc_coeff_t *TCODEC_RESTRICT out);
void tc_idct8x8_res(const tc_coeff_t *TCODEC_RESTRICT in,
                    tc_coeff_t *TCODEC_RESTRICT out, int stride);

/* Decoder residual-IDCT entry points.  On ARM these are the fully
 * vectorized NEON kernels; the scalar build provides exact aliases so
 * callers do not need architecture-specific source. */
void tc_idct4x4_neon(const tc_coeff_t *TCODEC_RESTRICT in,
                     tc_coeff_t *TCODEC_RESTRICT out, int stride);
void tc_idct8x8_neon(const tc_coeff_t *TCODEC_RESTRICT in,
                     tc_coeff_t *TCODEC_RESTRICT out, int stride);

/* Exact DC-only inverse-DCT values used by the v2 decoder fast path.
 * These are the same fixed-point two-pass operations as the full
 * residual transforms when every coefficient except DC is zero. */
TCODEC_INLINE int tc_idct_dc4_res(int dc)
{
    int v = (dc * 8192 + 8192) >> 14;
    return (v * 8192 + 8192) >> 14;
}

TCODEC_INLINE int tc_idct_dc8_res(int dc)
{
    int v = (dc * 5793 + 8192) >> 14;
    return (v * 5793 + 8192) >> 14;
}

/* Residual-mode Walsh-Hadamard Transform — self-inverting (H*H=n*I),
 * operates on signed residuals without ±128 level shift.
 * These are the primary functions used by the encoder/decoder pipeline.
 * Forward and inverse use the SAME butterfly (H is its own inverse). */
void tc_fwht4x4(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                 tc_coeff_t *TCODEC_RESTRICT out);
void tc_iwht4x4(const tc_coeff_t *TCODEC_RESTRICT in,
                 tc_coeff_t *TCODEC_RESTRICT out, int stride);
void tc_fwht8x8(const tc_coeff_t *TCODEC_RESTRICT in, int stride,
                 tc_coeff_t *TCODEC_RESTRICT out);
void tc_iwht8x8(const tc_coeff_t *TCODEC_RESTRICT in,
                 tc_coeff_t *TCODEC_RESTRICT out, int stride);

/* ── Quantization ────────────────────────────────────────────── */

/* Quantize transform coefficients. Returns number of non-zero coefficients. */
int tc_quantize(tc_coeff_t *TCODEC_RESTRICT coeffs, int n,
                int qp, int band);

/* Dequantize transform coefficients. */
void tc_dequantize(tc_coeff_t *TCODEC_RESTRICT coeffs, int n,
                   int qp, int band);

/* Add signed residuals to a 4×4 prediction block and clamp to pixels.
 * NEON builds use widening loads and saturating narrowing; scalar builds
 * use the same integer result as a reference implementation. */
void tc_recon_add_clip4x4(const tc_pixel_t *pred, int pred_stride,
                          const tc_coeff_t *res, tc_pixel_t *dst,
                          int dst_stride);
void tc_recon_add_dc4x4(const tc_pixel_t *pred, int pred_stride, int dc,
                        tc_pixel_t *dst, int dst_stride);
void tc_recon_add_clip8x8(const tc_pixel_t *pred, int pred_stride,
                          const tc_coeff_t *res, tc_pixel_t *dst,
                          int dst_stride);
void tc_recon_add_dc8x8(const tc_pixel_t *pred, int pred_stride, int dc,
                        tc_pixel_t *dst, int dst_stride);

/* Get quantization step size for given QP. */
int tc_qscale(int qp);
int tc_lambda(int qp);

/* Build the decoder's QP-local effective-scale lookup.  The second
 * dimension is intentionally 64 entries so frequency classification
 * can remain in the coefficient loop without recomputing quantizer math. */
void tc_build_eff_scale_table(int qp, int table[4][64]);

/* JND-based weight for a coefficient position in a band. */
int tc_jnd_weight(int band, int pos);

/* ── Shared quantizer primitives (encoder + decoder must agree) ──
 *
 * eff = JND-weighted quantizer step for a coefficient position.
 * Quantize uses a dead-zone offset of eff/3; reconstruction places the
 * level at the centroid of that bin (q·eff + eff/6) instead of the old
 * q·eff + eff/2, which overshot every level by eff/3 (≈2.3× the MSE of
 * a centred reconstruction).  Both sides use these helpers so the
 * rounding can never drift apart again.
 * ══════════════════════════════════════════════════════════════ */

TCODEC_INLINE int tc_eff_scale(int qp, int band, int pos)
{
    int eff = (tc_qscale(qp) * tc_jnd_weight(band, pos) + 4) >> 3;
    return eff < 1 ? 1 : eff;
}

TCODEC_INLINE int tc_quant_coeff(int c, int eff)
{
    int offset = eff / 3;
    if (c > 0) return  (c + offset) / eff;
    if (c < 0) return -((-c + offset) / eff);
    return 0;
}

TCODEC_INLINE int tc_dequant_coeff(int q, int eff)
{
    int bias = eff / 6;
    if (q > 0) return  q * eff + bias;
    if (q < 0) return  q * eff - bias;
    return 0;
}

/* ── Bitstream v2 quadtree geometry (shared enc/dec) ─────────────
 *
 * A CTU (64×64) is coded as a quadtree of coding units with sizes
 * 64/32/16/8.  Node bookkeeping is a flat array of 85 entries:
 *   depth 0 →  1 node  (base  0)
 *   depth 1 →  4 nodes (base  1)
 *   depth 2 → 16 nodes (base  5)
 *   depth 3 → 64 nodes (base 21)
 * (cx, cy) are 8×8-cell coordinates inside the CTU (0..7).
 * ══════════════════════════════════════════════════════════════ */

#define TC_QT_MAX_DEPTH 3
#define TC_QT_NODES     85
#define TC_QT_MIN_CU    8

/* 8×8 MV-grid cells per 64×64 CTU (encoder + decoder MV prediction). */
#define TC_MVGRID_STRIDE 8

TCODEC_INLINE int tc_qt_index(int depth, int cx, int cy)
{
    static const int base[4] = { 0, 1, 5, 21 };
    int shift = 3 - depth;                 /* cells per CU side = 1<<shift */
    return base[depth] + ((cy >> shift) << depth) + (cx >> shift);
}

/* Per-CU decision record produced by the v2 encoder RDO pass and
 * replayed by its write pass.  The decoder reads the same syntax
 * directly and needs no node array, but the type lives here so the
 * encoder can keep its quadtree scratch inside tc_encoder_t (no
 * static/shared buffers — safe for concurrent encoder instances).
 * Snapshots are the per-CU reconstruction; copies are row-wise
 * (frame strides are wider than a CU, so raw memcpy would read
 * garbage beyond the first row). */
typedef struct { int16_t dx, dy; uint8_t intra; } qt_mvcell_t;

typedef struct {
    uint8_t  split;
    uint8_t  skip;
    uint8_t  merge;
    uint8_t  intra;
    uint8_t  bi;
    uint8_t  dct_size;
    uint8_t  intra_mode;
    uint8_t  intra_cmode;
    uint8_t  ref_sel;
    int16_t  mvd_x;
    int16_t  mvd_y;
    uint8_t  ch_intra;
} qt_node_t;

/* ── Entropy coding (Exp-Golomb, tANS reserved) ─────────────── */

/* Encoder state — contexts removed (saved ~40KB), reserved for future ANS */
typedef struct tc_tans_enc {
    tc_bs_writer_t *bs;                  /* Output bitstream */
    uint32_t        state;               /* Reserved for ANS state */
} tc_tans_enc_t;

/* Decoder state — contexts removed (saved ~40KB), reserved for future ANS */
typedef struct tc_tans_dec {
    tc_bs_reader_t *bs;                  /* Input bitstream */
    uint32_t        state;               /* Reserved for ANS state */
} tc_tans_dec_t;

void tc_tans_enc_init(tc_tans_enc_t *e, tc_bs_writer_t *bs);
void tc_tans_enc_flush(tc_tans_enc_t *e);

void tc_tans_dec_init(tc_tans_dec_t *d, tc_bs_reader_t *bs);

/* ── Range coder (Phase 3: context-modeled arithmetic coding) ── */

/* Context state: single uint8_t per context.
 * Bits 0-5: state index (0-63)
 * Bit 7:    most probable symbol (MPS) value (0 or 1) */
typedef uint8_t tc_rc_ctx_t;

/* Range coder encoder state.
 * Writes bitstream via tc_bs_writer_t, normalizing when range < 2^24.
 * Uses 32-bit low with cache_byte + outstanding for carry handling — the
 * standard approach used by CABAC and other arithmetic coders. */
typedef struct tc_rc_enc {
    tc_bs_writer_t *bs;          /* Output bitstream */
    uint32_t        low;         /* Lower bound of current interval */
    uint32_t        range;       /* Width of current interval */
    int             cache_byte;  /* Buffered byte (-1 if none) */
    int             outstanding; /* Deferred 0xFF byte count */
} tc_rc_enc_t;

/* Range coder decoder state.
 * Reads from tc_bs_reader_t, normalizing when range < 2^24. */
typedef struct tc_rc_dec {
    tc_bs_reader_t *bs;          /* Input bitstream */
    uint32_t        low;         /* Offset within current interval */
    uint32_t        range;       /* Width of current interval */
} tc_rc_dec_t;

/* Engine init/flush */
void tc_rc_enc_init(tc_rc_enc_t *rc, tc_bs_writer_t *bs);
void tc_rc_enc_flush(tc_rc_enc_t *rc);
void tc_rc_dec_init(tc_rc_dec_t *rc, tc_bs_reader_t *bs);

/* Context management */
void tc_rc_ctx_init(tc_rc_ctx_t *ctx, int num_ctx);
void tc_rc_ctx_copy(tc_rc_ctx_t *dst, const tc_rc_ctx_t *src, int num_ctx);

/* Single bit encode/decode (context-modeled) */
void tc_rc_enc_bit(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx, int bit);
int  tc_rc_dec_bit(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx);

/* Multi-bit encode/decode (context per bit position) */
void     tc_rc_enc_bits(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx, int base_ctx,
                         uint32_t val, int nbits);
uint32_t tc_rc_dec_bits(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx,
                         int base_ctx, int nbits);

/* Context-modeled Exp-Golomb */
uint32_t tc_rc_enc_ue(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx,
                       int base_ctx, uint32_t val);
uint32_t tc_rc_dec_ue(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx, int base_ctx);

/* Context-modeled coefficient coding (replaces EG coeff coding) */
void tc_rc_enc_coeffs(tc_rc_enc_t *rc, tc_rc_ctx_t *ctx,
                       const tc_coeff_t *coeffs, int n,
                       tc_block_size_t dct_size);
void tc_rc_dec_coeffs(tc_rc_dec_t *rc, tc_rc_ctx_t *ctx,
                       tc_coeff_t *coeffs, int n,
                       tc_block_size_t dct_size);

/* Context indices (see range_coder.c for full enum) */
#define RC_CTX_BLOCK_MODE  0
#define RC_CTX_SKIP_FLAG   4
#define RC_CTX_INTRA_MODE  6
#define RC_CTX_DCT_SIZE    11
#define RC_CTX_REF_IDX     13
#define RC_CTX_MVD_SIGN    15
#define RC_CTX_MVD_X       17   /* separate x/y MVD magnitude contexts (D2) */
#define RC_CTX_MVD_Y       19
#define RC_CTX_SIG         21
#define RC_CTX_SIG_DC      29   /* DC coefficient significance (D2) */
#define RC_CTX_LAST        30
#define RC_CTX_LAST_DC     34   /* DC last-nz position (D2) */
#define RC_CTX_GT1         35
#define RC_CTX_GT1_DC      41   /* DC GT1 flag (D2) */
#define RC_CTX_GT2         42
#define RC_CTX_GT2_DC      44   /* DC GT2 flag (D2) */
#define RC_CTX_SIGN        45
#define RC_CTX_SIGN_DC     46   /* DC sign (D2) */
#define RC_CTX_LEVEL       47
#define RC_CTX_LEVEL_DC    53   /* DC level (D2) */
#define RC_CTX_REF_SEL     59   /* B-frame ref selection (0=fwd,1=bwd,2=bi) */
#define RC_CTX_MERGE_FLAG  61   /* merge (no MVD) vs explicit MVD */
#define RC_CTX_QT_SPLIT    62   /* quadtree split flag (v2, depth in sub-ctx) */
#define RC_CTX_SAO_TYPE    65   /* v2 SAO off/band flag */
#define RC_CTX_SAO_BAND    66   /* v2 SAO band position (5 bits) */
#define RC_CTX_SAO_OFFSET  71   /* v2 SAO signed offset */
#define RC_CTX_MAX         76

/* Frequency band classification for a zigzag position.
 * Reserved for future JND-weighted quantization per coefficient.
 * Currently quantize/dequantize always pass band=0. */
int tc_freq_band(int pos, int blk_size);

/* Coefficient coding helpers (Exp-Golomb path) */
void tc_tans_enc_coeffs(tc_tans_enc_t *e, const tc_coeff_t *coeffs, int n,
                         tc_block_size_t dct_size);
void tc_tans_dec_coeffs(tc_tans_dec_t *d, tc_coeff_t *coeffs, int n,
                         tc_block_size_t dct_size);

/* ── Intra prediction ─────────────────────────────────────────── */

/* Build reference samples from reconstructed neighbors.
 * ref_above[0..2n-1], ref_left[0..2n-1] for an n×n block.
 * If neighbors aren't available (top row / left col), repeat from DC. */
void tc_intra_get_ref(const tc_pixel_t *recon, int stride,
                      int x, int y, int blk_size, int frame_w, int frame_h,
                      tc_pixel_t *ref_above, tc_pixel_t *ref_left);

/* Intra predict a block. mode is tc_intra_mode_t.
 * dst stride is blk_size. */
void tc_intra_predict(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                      const tc_pixel_t *ref_above,
                      const tc_pixel_t *ref_left,
                      int blk_size, tc_intra_mode_t mode);

/* Rate-distortion cost for a candidate intra mode (fast SAD-based). */
tc_sad_t tc_intra_cost(const tc_pixel_t *orig, int orig_stride,
                        const tc_pixel_t *pred, int blk_size);

/* ── Inter prediction / motion estimation ────────────────────── */

/* Compute SAD between two n×n blocks. */
tc_sad_t tc_sad(const tc_pixel_t *a, int stride_a,
                const tc_pixel_t *b, int stride_b, int n);

/* Sub-pixel SAD (bilinear interpolation at quarter-pel). */
tc_sad_t tc_sad_subpel(const tc_pixel_t *ref, int ref_stride,
                       int mv_x_qpel, int mv_y_qpel,
                       const tc_pixel_t *orig, int orig_stride, int n);

/* Hierarchical hexagonal motion estimation.
 * ref_w, ref_h: reference frame dimensions for bounds checking.
 * center_x, center_y: search center (collocated position or median predictor).
 * search_range: max pixel displacement.
 * Returns best MV in quarter-pel and best SAD. */
tc_mv_s tc_motion_est(const tc_pixel_t *ref, int ref_stride,
                      int ref_w, int ref_h,
                      const tc_pixel_t *orig, int orig_stride,
                      int center_x, int center_y,
                      int blk_size, int search_range,
                      tc_sad_t *best_sad_out);

/* Inter predict: copy from reference at sub-pel position.
 * Uses 6-tap luma filter with bilinear fallback at frame edges.
 * ref_w, ref_h: reference frame dimensions for bounds-safe filtering. */
void tc_inter_predict(const tc_pixel_t *ref, int ref_stride,
                      int ref_w, int ref_h,
                      tc_mv_s mv,
                      tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                      int blk_size);

/* Chroma inter predict (bitstream v2).
 * The luma MV is expressed in quarter-luma-pel; on the 4:2:0 chroma
 * grid the same number is 1/8-chroma-pel, so the fractional part is
 * mv & 7 and the integer part mv >> 3 (H.264 chroma MC).  Bilinear,
 * integer-only, edge-clamped — identical on encoder and decoder.
 * ref_w/ref_h are the *chroma* plane dimensions. */
void tc_inter_predict_chroma(const tc_pixel_t *ref, int ref_stride,
                             int ref_w, int ref_h,
                             tc_mv_s mv,
                             tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                             int blk_size);

/* Decoder-only exact SIMD dispatch. Encoder callers must use the scalar
 * normative functions above so prediction decisions and recon remain
 * architecture-independent. */
void tc_inter_predict_decoder(const tc_pixel_t *ref, int ref_stride,
                              int ref_w, int ref_h,
                              tc_mv_s mv,
                              tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                              int blk_size);
void tc_inter_predict_chroma_decoder(const tc_pixel_t *ref, int ref_stride,
                                     int ref_w, int ref_h,
                                     tc_mv_s mv,
                                     tc_pixel_t *TCODEC_RESTRICT dst,
                                     int dst_stride, int blk_size);

void tc_inter_predict_neon(const tc_pixel_t *ref, int ref_stride,
                           int ref_w, int ref_h,
                           tc_mv_s mv,
                           tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                           int blk_size);
void tc_inter_predict_chroma_neon(const tc_pixel_t *ref, int ref_stride,
                                  int ref_w, int ref_h,
                                  tc_mv_s mv,
                                  tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                                  int blk_size);

/* Reference samples for bitstream v2 (above-right / below-left are
 * replicated instead of read — see predict.c for the rationale). */
void tc_intra_get_ref_v2(const tc_pixel_t *recon, int stride,
                         int x, int y, int blk_size, int frame_w, int frame_h,
                         tc_pixel_t *ref_above, tc_pixel_t *ref_left);

/* Chroma intra DC prediction from reconstructed neighbours (v2). */
void tc_intra_chroma_dc(const tc_pixel_t *recon_c, int stride,
                        int cx, int cy, int csize,
                        tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride);

/* ── Deblocking filter ───────────────────────────────────────── */

/* Filter one CTU's edges. Strength based on QP and boundary strength. */
void tc_deblock_ctu(tc_pixel_t *y,  int stride_y,
                    tc_pixel_t *cb, int stride_cb,
                    tc_pixel_t *cr, int stride_cr,
                    int ctu_x, int ctu_y, int qp);

/* v2 Sample Adaptive Offset (band-offset mode, luma). The application
 * kernel is shared by encoder and decoder and is bit-exact across scalar
 * and NEON builds. `band` is 0..31 and `offset` is -7..7. */
void tc_sao_ctu_luma(tc_pixel_t *y, int stride_y,
                     int x, int y0, int width, int height,
                     int band, int offset);

/* Select a single BO band/offset from the original and post-deblock
 * reconstruction. Returns 1 when the candidate reduces SSE, otherwise 0. */
int tc_sao_choose_bo(const tc_pixel_t *orig, int orig_stride,
                     const tc_pixel_t *recon, int recon_stride,
                     int x, int y, int width, int height,
                     int *band, int *offset);

/* ── Rate control ────────────────────────────────────────────── */

typedef struct tc_ratectl {
    tc_ratectrl_t method;
    int32_t       target_bitrate;     /* bps */
    double        rho;                /* Zero-fraction (ρ-domain) */
    double        rho_per_qp[64];     /* ρ(QP) lookup table */
    int32_t       qp;                 /* Current QP */
    int64_t       frame_bits_target;  /* Target bits per frame */
    int64_t       frame_bits_actual;  /* Actual bits for last frame */
    int64_t       total_bits;         /* Running total */
    int32_t       total_frames;
    double        buffer_level;       /* VBV buffer fullness (0..1) */
    double        buffer_size;        /* VBV buffer size in bits */
    int32_t       fps_num;
    int32_t       fps_den;
} tc_ratectl_t;

void tc_ratectl_init(tc_ratectl_t *rc, const tc_config_t *cfg);
void tc_ratectl_frame_start(tc_ratectl_t *rc, tc_frame_type_t type);
int  tc_ratectl_get_qp(tc_ratectl_t *rc);
void tc_ratectl_frame_end(tc_ratectl_t *rc, int64_t bits_used);

/* ── Thread pool for WPP ─────────────────────────────────────── */

#if !defined(TCODEC_NO_THREADS)

typedef void (*tc_wpp_row_func)(void *ctx, int row);

typedef struct tc_threadpool {
    pthread_t       *threads;
    int              num_threads;
    tc_wpp_row_func  func;
    void            *ctx;
    int              next_row;
    int              total_rows;
    int             *row_done;
    pthread_mutex_t  mutex;
    pthread_cond_t   work_cond;
    pthread_cond_t   done_cond;
    int              shutdown;
} tc_threadpool_t;

tc_threadpool_t *tc_threadpool_create(int num_threads);
void             tc_threadpool_destroy(tc_threadpool_t *pool);
void             tc_threadpool_run(tc_threadpool_t *pool,
                                    tc_wpp_row_func func, void *ctx,
                                    int total_rows);

#endif /* TCODEC_NO_THREADS */

/* ── Encoder internals ───────────────────────────────────────── */

typedef struct tc_encoder {
    tc_config_t       cfg;
    tc_frame_buf_t   *cur;             /* Current input frame */
    tc_frame_buf_t   *recon;           /* Reconstructed frame */
    tc_ref_entry_t    dpb[TC_REF_FRAMES]; /* Decoded picture buffer */
    tc_ratectl_t      rc;
    tc_tans_enc_t     tans;
    tc_rc_enc_t       rc_enc;         /* Range coder encoder (Phase 3) */
    tc_rc_ctx_t       rc_ctx[TC_NUM_CONTEXTS_RC]; /* Context model */
    int               use_entropy_coded; /* 1 = use RC, 0 = use EG */
    tc_bs_writer_t    bs;
    tc_ctu_info_t    *ctu_data;        /* Per-CTU coding info array */
    int32_t           num_ctu_cols;
    int32_t           num_ctu_rows;
    int32_t           frame_count;
    int32_t           force_keyframe;
    /* Bitstream v2 quadtree scratch (per-encoder, not static) */
    qt_node_t        *v2_node;         /* TC_QT_NODES decision records */
    qt_mvcell_t      *v2_grid;         /* TC_MVGRID_STRIDE² MV grid */
    /* Thread pool for WPP (only when threading enabled) */
#if !defined(TCODEC_NO_THREADS)
    tc_threadpool_t  *pool;
    /* Per-row bitstream buffers for WPP parallelism.
     * Each WPP thread writes to its own row_bs/row_tans,
     * then rows are merged into the main bitstream in order. */
    tc_bs_writer_t   *row_bs;
    tc_tans_enc_t    *row_tans;
    tc_rc_enc_t      *row_rc;         /* Per-row range coder (Phase 3) */
    tc_rc_ctx_t      *row_rc_ctx;     /* Flat: num_rows * TC_NUM_CONTEXTS_RC */
    uint8_t         **row_buf;         /* Per-row output buffer pointers */
    size_t           *row_buf_size;    /* Per-row output buffer sizes */
    int               num_threads;     /* Number of WPP worker threads */
    int               use_wpp;        /* 1 = use WPP, 0 = sequential */
#endif
    /* B-frame reorder state (D4). display-order input frames are
     * buffered here until a full GOP is available, then emitted in
     * coding order (anchor, B+2, B+1, B+3). */
    struct {
        tc_frame_buf_t *frame[8];        /* display-order buffer */
        int             poc[8];          /* display POC of each slot */
        int             n;               /* frames buffered */
        int             emit_pos;        /* next emission in schedule */
        int             b_mode;          /* cfg.enable_b_frames */
        int             next_input;      /* display index of next input */
    } bf;
    /* Stats */
    int32_t           total_bytes;
    int32_t           total_frames;
    double            sum_psnr;
    /* Bitstream output buffer */
    uint8_t          *out_buf;
    size_t            out_buf_size;
} tc_encoder_t;

/* ── Decoder internals ───────────────────────────────────────── */

typedef struct tc_decoder {
    int32_t           width;
    int32_t           height;
    tc_frame_buf_t   *cur;             /* Current reconstructed frame */
    tc_ref_entry_t    dpb[TC_REF_FRAMES];
    tc_tans_dec_t     tans;
    tc_rc_dec_t       rc_dec;         /* Range coder decoder (Phase 3) */
    tc_rc_ctx_t       rc_ctx[TC_NUM_CONTEXTS_RC]; /* Context model */
    int               use_entropy_coded; /* 1 = use RC, 0 = use EG */
    int               use_v2;           /* 1 = bitstream v2 quadtree path */
    int               cur_qp;           /* current frame QP (v2 decode) */
    int               profile_enabled;  /* component timing enabled */
    /* Reused v2 decode scratch. A decoder instance is single-consumer;
     * callers must not invoke decode concurrently. Keeping these outside
     * qt_dec_leaf avoids repeated large stack frames. */
    tc_pixel_t        v2_pred[TC_CTU_SIZE * TC_CTU_SIZE];
    tc_pixel_t        v2_cbuf[2][(TC_CTU_SIZE/2) * (TC_CTU_SIZE/2)];
    tc_pixel_t        v2_bipred_a[TC_CTU_SIZE * TC_CTU_SIZE];
    tc_pixel_t        v2_bipred_b[TC_CTU_SIZE * TC_CTU_SIZE];
    uint64_t          profile_parse_ns;
    uint64_t          profile_coeff_ns;
    uint64_t          profile_transform_ns;
    uint64_t          profile_motion_ns;
    uint64_t          profile_chroma_ns;
    uint64_t          profile_deblock_ns;
    uint64_t          profile_copy_ns;
    tc_bs_reader_t    bs;
    tc_ctu_info_t    *ctu_data;
    int32_t           num_ctu_cols;
    int32_t           num_ctu_rows;
    int32_t           prev_qp;
    /* Thread pool for WPP (only when threading enabled) */
#if !defined(TCODEC_NO_THREADS)
    tc_threadpool_t  *pool;
    void             *v2_pool;         /* Persistent v2 CTU worker pool */
    tc_bs_reader_t   *row_bs;          /* Per-row bitstream readers */
    tc_tans_dec_t    *row_tans;        /* Per-row tANS decoders */
    int               num_threads;     /* Number of WPP worker threads */
    int               use_wpp;        /* 1 = use WPP, 0 = sequential */
#endif
    /* Output packet info */
    tc_frame_header_t last_header;
    /* v1 bitstream validation results */
    int             last_crc_valid;   /* 1 = CRC OK or no CRC, 0 = CRC mismatch */
    /* B-frame display reorder (D4): decoded frames wait here until
     * their display POC turns up; out_frames holds pending outputs. */
    struct {
        tc_frame_buf_t *frames[TC_REF_FRAMES + 1];
        int             pocs[TC_REF_FRAMES + 1];
        int             n;
        int             next_display;    /* next POC to output */
        int             active;          /* B reorder active in this stream */
        tc_frame_buf_t *out;             /* scratch output frame */
    } disp;
} tc_decoder_t;

/* ── PSNR computation ────────────────────────────────────────── */

double tc_psnr(const tc_pixel_t *a, int stride_a,
               const tc_pixel_t *b, int stride_b,
               int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* TCODEC_COMMON_H */
