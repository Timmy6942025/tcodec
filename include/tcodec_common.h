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
} tc_bs_reader_t;

void tc_bs_writer_init(tc_bs_writer_t *w, uint8_t *buf, size_t capacity);
void tc_bs_writer_write_bits(tc_bs_writer_t *w, uint32_t val, int nbits);
void tc_bs_writer_write_ue(tc_bs_writer_t *w, uint32_t val);  /* Exp-Golomb */
void tc_bs_writer_write_se(tc_bs_writer_t *w, int32_t val);   /* Signed EG */
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

/* Get quantization step size for given QP. */
int tc_qscale(int qp);

/* JND-based weight for a coefficient position in a band. */
int tc_jnd_weight(int band, int pos);

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
#define RC_CTX_MVD_MAG     17
#define RC_CTX_CSBF        21
#define RC_CTX_SIG         25
#define RC_CTX_LAST        33
#define RC_CTX_GT1         37
#define RC_CTX_GT2         43
#define RC_CTX_SIGN        45
#define RC_CTX_LEVEL       46
#define RC_CTX_MAX         52

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

/* ── Deblocking filter ───────────────────────────────────────── */

/* Filter one CTU's edges. Strength based on QP and boundary strength. */
void tc_deblock_ctu(tc_pixel_t *y,  int stride_y,
                    tc_pixel_t *cb, int stride_cb,
                    tc_pixel_t *cr, int stride_cr,
                    int ctu_x, int ctu_y, int qp);

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
    /* Stats */
    int64_t           total_bytes;
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
    tc_bs_reader_t    bs;
    tc_ctu_info_t    *ctu_data;
    int32_t           num_ctu_cols;
    int32_t           num_ctu_rows;
    int32_t           prev_qp;
    /* Thread pool for WPP (only when threading enabled) */
#if !defined(TCODEC_NO_THREADS)
    tc_threadpool_t  *pool;
    tc_bs_reader_t   *row_bs;          /* Per-row bitstream readers */
    tc_tans_dec_t    *row_tans;        /* Per-row tANS decoders */
    int               num_threads;     /* Number of WPP worker threads */
    int               use_wpp;        /* 1 = use WPP, 0 = sequential */
#endif
    /* Output packet info */
    tc_frame_header_t last_header;
    /* v1 bitstream validation results */
    int             last_crc_valid;   /* 1 = CRC OK or no CRC, 0 = CRC mismatch */
} tc_decoder_t;

/* ── PSNR computation ────────────────────────────────────────── */

double tc_psnr(const tc_pixel_t *a, int stride_a,
               const tc_pixel_t *b, int stride_b,
               int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* TCODEC_COMMON_H */
