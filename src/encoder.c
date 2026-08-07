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
#include <stdio.h>
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
    else if (bs) tc_bs_writer_write_bits(bs, val, nbits);
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
    } else if (bs) {
        tc_bs_writer_write_se(bs, val);
    }
}

static TCODEC_FORCEINLINE void enc_write_coeffs(
    tc_bs_writer_t *bs, tc_tans_enc_t *tans,
    tc_rc_enc_t *rc, tc_rc_ctx_t *rc_ctx,
    const tc_coeff_t *coeffs, int n, tc_block_size_t dct_size)
{
    if (rc) tc_rc_enc_coeffs(rc, rc_ctx, coeffs, n, dct_size);
    else if (tans) tc_tans_enc_coeffs(tans, coeffs, n, dct_size);
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

/* ── DPB reference lookup by POC (B-frames) ─────────────────────
 * B-frames reference the nearest decoded frames on each side of
 * their display position: forward = max POC < cur, backward = min
 * POC > cur. Both encoder and decoder resolve references the same
 * way from the DPB ring, so no explicit ref index is transmitted. */
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

/* ════════════════════════════════════════════════════════════════
 * Bitstream v2 quadtree coding path (D5/D7)
 * ────────────────────────────────────────────────────────────────
 * A CTU (64×64) is coded as a quadtree of CUs with sizes 64/32/16/8.
 * Every CU is RD-selected among {skip, merge, inter, intra, bi}; each
 * luma residual transform unit is RD-selected between 4×4 and 8×8;
 * chroma uses 4×4 transforms with neighbor-DC intra or collocated
 * chroma motion compensation.  See docs/BITSTREAM.md §v2.
 *
 * The encoder runs two passes:
 *   1. qt_split (decide)  — recursive RD search, fills the decision
 *      tree and the reconstructed frame, writing nothing.
 *   2. qt_write (write)   — replays the decision tree and emits the
 *      exact same syntax, reconstructing identically for in-loop
 *      filters and subsequent CUs.
 *
 * Both passes share tc_encode_cu_*, so they cannot drift.
 * ══════════════════════════════════════════════════════════════ */

enum { QT_DECIDE = 0, QT_WRITE = 1, QT_REPLAY = 2 };

typedef struct {
    tc_encoder_t    *enc;
    int              ctu_x, ctu_y;
    int              qp, qp_c;
    int64_t          lambda;
    tc_frame_type_t  frame_type;
    int              poc;
    tc_bs_writer_t  *bs;
    tc_tans_enc_t   *tans;
    tc_rc_enc_t     *rc;
    tc_rc_ctx_t     *rc_ctx;
    qt_node_t       *node;
    qt_mvcell_t     *grid;
    /* Reused only by QT_DECIDE full-RDO leaves; fast leaves do not copy
     * these buffers. Keeping them in the CTU context avoids a large stack
     * allocation in every recursive leaf invocation. */
    uint8_t          pre_luma[TC_CTU_SIZE * TC_CTU_SIZE];
    uint8_t          pre_cb[(TC_CTU_SIZE / 2) * (TC_CTU_SIZE / 2)];
    uint8_t          pre_cr[(TC_CTU_SIZE / 2) * (TC_CTU_SIZE / 2)];
    qt_mvcell_t      pre_grid[TC_MVGRID_STRIDE * TC_MVGRID_STRIDE];
} qt_enc_t;

/* Row-wise copy helpers for the v2 quadtree path.  Frame strides are
 * wider than a CU (e.g. 320 for a 320-wide frame), so a raw memcpy of
 * cu*cu bytes from recon + py*stride + px would read one row plus
 * garbage from the neighbouring columns.  All snapshot save/restore
 * and split save/restore goes through these. */
static void qt_copy_rows(uint8_t *dst_row_major, const uint8_t *src, int src_stride,
                         int x, int y, int size)
{
    for (int r = 0; r < size; r++)
        memcpy(dst_row_major + (size_t)r * size,
               src + (size_t)(y + r) * src_stride + x, (size_t)size);
}

static void qt_paste_rows(uint8_t *dst, int dst_stride, int x, int y,
                          const uint8_t *src_row_major, int size)
{
    for (int r = 0; r < size; r++)
        memcpy(dst + (size_t)(y + r) * dst_stride + x,
               src_row_major + (size_t)r * size, (size_t)size);
}

/* Save/restore the in-frame portion of a CTU. Partial CTUs cannot use a
 * raw 64×64 copy because the frame allocation ends at width/height. */
static void qt_copy_rect(uint8_t *dst, int dst_stride,
                         const uint8_t *src, int src_stride,
                         int x, int y, int width, int height, int max_size)
{
    int w = tc_clip(width - x, 0, max_size);
    int h = tc_clip(height - y, 0, max_size);
    for (int r = 0; r < h; r++)
        memcpy(dst + (size_t)r * dst_stride,
               src + (size_t)(y + r) * src_stride + x, (size_t)w);
}

static void qt_paste_rect(uint8_t *dst, int dst_stride, int x, int y,
                          const uint8_t *src, int src_stride,
                          int width, int height, int max_size)
{
    int w = tc_clip(width - x, 0, max_size);
    int h = tc_clip(height - y, 0, max_size);
    for (int r = 0; r < h; r++)
        memcpy(dst + (size_t)(y + r) * dst_stride + x,
               src + (size_t)r * src_stride, (size_t)w);
}

static int count_coeff_bits(const tc_coeff_t *c, int n)
{
    int bits = 0;
    for (int i = 0; i < n; i++) {
        int v = c[i];
        if (v == 0) { bits += 1; continue; }
        int a = v < 0 ? -v : v;
        bits += 1 + 2 * (32 - __builtin_clz(a)) + 1;
    }
    return bits;
}

static tc_mv_s qt_mvp(qt_enc_t *e, int cx, int cy, const qt_mvcell_t *grid)
{
    tc_mv_s a={0,0}, b={0,0}, c={0,0};
    int ha=0, hb=0, hc=0;
    if (cx > 0)     { const qt_mvcell_t *m=&grid[cy*TC_MVGRID_STRIDE+(cx-1)]; if(!m->intra){a.x=m->dx;a.y=m->dy;ha=1;} }
    if (cy > 0)     { const qt_mvcell_t *m=&grid[(cy-1)*TC_MVGRID_STRIDE+cx]; if(!m->intra){b.x=m->dx;b.y=m->dy;hb=1;} }
    if (cy > 0 && cx < 7) { const qt_mvcell_t *m=&grid[(cy-1)*TC_MVGRID_STRIDE+(cx+1)]; if(!m->intra){c.x=m->dx;c.y=m->dy;hc=1;} }
    if (!ha && !hb && !hc) return (tc_mv_s){0,0};
    if (!ha) a = hb ? b : c;
    if (!hb) b = ha ? a : c;
    if (!hc) c = ha ? a : b;
    int px,py;
    { int t,x[3]={a.x,b.x,c.x}; if(x[0]>x[1]){t=x[0];x[0]=x[1];x[1]=t;} if(x[1]>x[2]){t=x[1];x[1]=x[2];x[2]=t;} if(x[0]>x[1]){t=x[0];x[0]=x[1];x[1]=t;} px=x[1]; }
    { int t,y[3]={a.y,b.y,c.y}; if(y[0]>y[1]){t=y[0];y[0]=y[1];y[1]=t;} if(y[1]>y[2]){t=y[1];y[1]=y[2];y[2]=t;} if(y[0]>y[1]){t=y[0];y[0]=y[1];y[1]=t;} py=y[1]; }
    return (tc_mv_s){px,py};
}

static int64_t qt_code_chroma(qt_enc_t *e, int px, int py, int cu,
                              uint8_t intra, const tc_pixel_t *pred[2],
                              int *bits_out, int write)
{
    tc_encoder_t *enc = e->enc;
    int qp = e->qp_c, cs = cu/2;
    int64_t distortion = 0;
    int bits = 0;
    const tc_pixel_t *orig[2] = { enc->cur->cb, enc->cur->cr };
    tc_pixel_t *rec[2]  = { enc->recon->cb, enc->recon->cr };
    int rs = enc->recon->stride_c;
    for (int comp = 0; comp < 2; comp++) {
        for (int ty = 0; ty < cs/4; ty++)
            for (int tx = 0; tx < cs/4; tx++) {
                int ox=tx*4, oy=ty*4;
                tc_coeff_t res[16];
                for (int y=0;y<4;y++) for (int x=0;x<4;x++)
                    res[y*4+x]=(tc_coeff_t)tc_clip(
                        (int)orig[comp][(py/2+oy+y)*rs+(px/2+ox+x)]-(int)pred[comp][(oy+y)*cs+(ox+x)],-512,511);
                 tc_coeff_t c4[16];
                 tc_fdct4x4_res(res,4,c4);
                 int eff4_band[4];
                 for (int band = 0; band < 4; band++) eff4_band[band] = tc_eff_scale(qp, band, 0);
                 int nz=0;
                 for (int i=0;i<16;i++){ int band=tc_freq_band(i,4); c4[i]=(tc_coeff_t)tc_quant_coeff(c4[i],eff4_band[band]); if(c4[i]) nz++; }
                 /* Coeffs follow the same entropy path as every other
                  * v2 syntax element (range coder when rc!=NULL, EG
                  * otherwise) so encoder and decoder can never drift. */
                 if (write) enc_write_coeffs(e->bs,e->tans,e->rc,e->rc_ctx, c4, 16, TC_BLOCK_4x4_ID);
                 else bits += 1 + 2*nz + count_coeff_bits(c4,16);
                 tc_coeff_t iq[16];
                 for (int i=0;i<16;i++){ int band=tc_freq_band(i,4); iq[i]=(tc_coeff_t)tc_dequant_coeff(c4[i],eff4_band[band]); }
                tc_coeff_t recs[16];
                tc_idct4x4_res(iq,recs,4);
                for (int y=0;y<4;y++) for (int x=0;x<4;x++){
                    int v=(int)pred[comp][(oy+y)*cs+(ox+x)]+recs[y*4+x];
                    tc_pixel_t rv=(tc_pixel_t)tc_clip(v,0,255);
                    rec[comp][(py/2+oy+y)*rs+(px/2+ox+x)]=rv;
                    int d=(int)orig[comp][(py/2+oy+y)*rs+(px/2+ox+x)]-rv; distortion += d*d;
                }
            }
    }
    *bits_out = bits;
    return distortion;
}

static int64_t qt_code_luma(qt_enc_t *e, int px, int py, int cu,
                            uint8_t dct_size, const tc_pixel_t *pred,
                            int *bits_out, int write)
{
    tc_encoder_t *enc = e->enc;
    int qp = e->qp, ntu = cu/8;
    int64_t distortion = 0;
    int bits = 0;
    const tc_pixel_t *orig = enc->cur->y + py*enc->cur->stride_y + px;
    int os = enc->cur->stride_y, rs = enc->recon->stride_y;
    tc_coeff_t coeffs[64*64];
    for (int ty=0;ty<ntu;ty++) for (int tx=0;tx<ntu;tx++){
        int ox=tx*8, oy=ty*8;
        tc_coeff_t res8[64];
        for (int y=0;y<8;y++) for (int x=0;x<8;x++)
            res8[y*8+x]=(tc_coeff_t)tc_clip((int)orig[(oy+y)*os+ox+x]-(int)pred[(oy+y)*cu+ox+x],-512,511);
        if (dct_size == TC_BLOCK_8x8_ID) {
            tc_coeff_t tu[64]; tc_fdct8x8_res(res8,8,tu);
            int eff8_band[4];
            for (int band = 0; band < 4; band++) eff8_band[band] = tc_eff_scale(qp, band, 0);
            int nz=0;
            for (int i=0;i<64;i++){ int band=tc_freq_band(i,8); tu[i]=(tc_coeff_t)tc_quant_coeff(tu[i],eff8_band[band]); coeffs[(ty*ntu+tx)*64+i]=tu[i]; if(tu[i]) nz++; }
            if (write) { enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_DCT_SIZE,TC_BLOCK_8x8_ID,1); enc_write_coeffs(e->bs,e->tans,e->rc,e->rc_ctx,coeffs+(ty*ntu+tx)*64,64,TC_BLOCK_8x8_ID); }
            else bits += 1 + 2*nz + count_coeff_bits(coeffs+(ty*ntu+tx)*64,64);
            tc_coeff_t iq[64];
            for (int i=0;i<64;i++){ int band=tc_freq_band(i,8); iq[i]=(tc_coeff_t)tc_dequant_coeff(coeffs[(ty*ntu+tx)*64+i],eff8_band[band]); }
            tc_idct8x8_res(iq,res8,8);
            for (int y=0;y<8;y++) for (int x=0;x<8;x++){ int v=(int)pred[(oy+y)*cu+ox+x]+res8[y*8+x]; tc_pixel_t rv=(tc_pixel_t)tc_clip(v,0,255); enc->recon->y[(py+oy+y)*rs+(px+ox+x)]=rv; int d=(int)orig[(oy+y)*os+ox+x]-rv; distortion+=d*d; }
        } else {
            int base=(ty*ntu+tx)*64;
            for (int q=0;q<4;q++){
                int sx=(q&1)*4, sy=(q&2)*2;
                tc_coeff_t res4[16];
                for (int y=0;y<4;y++) for (int x=0;x<4;x++) res4[y*4+x]=(tc_coeff_t)tc_clip((int)res8[(sy+y)*8+(sx+x)],-512,511);
                tc_coeff_t c4[16]; tc_fdct4x4_res(res4,4,c4);
                int eff4_band[4];
                for (int band = 0; band < 4; band++) eff4_band[band] = tc_eff_scale(qp, band, 0);
                int nz=0;
                for (int i=0;i<16;i++){ int band=tc_freq_band(i,4); c4[i]=(tc_coeff_t)tc_quant_coeff(c4[i],eff4_band[band]); coeffs[base+q*16+i]=c4[i]; if(c4[i]) nz++; }
                if (write) { enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_DCT_SIZE,TC_BLOCK_4x4_ID,1); enc_write_coeffs(e->bs,e->tans,e->rc,e->rc_ctx,coeffs+base+q*16,16,TC_BLOCK_4x4_ID); }
                else bits += 1 + 2*nz + count_coeff_bits(coeffs+base+q*16,16);
                tc_coeff_t iq4[16];
                for (int i=0;i<16;i++){ int band=tc_freq_band(i,4); iq4[i]=(tc_coeff_t)tc_dequant_coeff(c4[i],eff4_band[band]); }
                tc_coeff_t rec4[16];
                tc_idct4x4_res(iq4,rec4,4);
                for (int y=0;y<4;y++) for (int x=0;x<4;x++){ int v=(int)pred[(oy+sy+y)*cu+(ox+sx+x)]+rec4[y*4+x]; tc_pixel_t rv=(tc_pixel_t)tc_clip(v,0,255); enc->recon->y[(py+oy+sy+y)*rs+(px+ox+sx+x)]=rv; int d=(int)orig[(oy+sy+y)*os+(ox+sx+x)]-rv; distortion+=d*d; }
            }
        }
    }
    *bits_out = bits;
    return distortion;
}

static uint8_t qt_choose_dct(const tc_pixel_t *pred, int cu)
{
    int32_t sum=0,sumsq=0,n=cu*cu;
    for (int i=0;i<n;i++){ int v=pred[i]; sum+=v; sumsq+=v*v; }
    int var = sumsq/n - (sum/n)*(sum/n);
    return (var > VARIANCE_THRESHOLD) ? TC_BLOCK_4x4_ID : TC_BLOCK_8x8_ID;
}

static int64_t qt_leaf(qt_enc_t *e, int depth, int cx, int cy, int write)
{
    qt_node_t *nd = &e->node[tc_qt_index(depth,cx,cy)];
    int cu = 8 << (TC_QT_MAX_DEPTH-depth);
    int px = e->ctu_x + cx*8, py = e->ctu_y + cy*8;
    tc_encoder_t *enc = e->enc;
    tc_pixel_t pred[64*64];
    const tc_pixel_t *cpred[2]; tc_pixel_t cbuf[2][32*32];
    int fast_mode = (enc->cfg.preset <= TC_PRESET_FAST);
    if (!write && !fast_mode) {
        qt_copy_rect(e->pre_luma, TC_CTU_SIZE, enc->recon->y, enc->recon->stride_y,
                     px, py, enc->cfg.width, enc->cfg.height, cu);
        qt_copy_rect(e->pre_cb, TC_CTU_SIZE / 2, enc->recon->cb, enc->recon->stride_c,
                     px / 2, py / 2, enc->cfg.width / 2, enc->cfg.height / 2, cu / 2);
        qt_copy_rect(e->pre_cr, TC_CTU_SIZE / 2, enc->recon->cr, enc->recon->stride_c,
                     px / 2, py / 2, enc->cfg.width / 2, enc->cfg.height / 2, cu / 2);
        int cs0 = cu / 8;
        for (int gy = 0; gy < cs0; gy++)
            memcpy(e->pre_grid + gy * cs0,
                   e->grid + (cy + gy) * TC_MVGRID_STRIDE + cx,
                   (size_t)cs0 * sizeof(qt_mvcell_t));
    }
    cpred[0]=cbuf[0]; cpred[1]=cbuf[1];
    int bits_dummy;

    if (write != QT_DECIDE) {
        if (write == QT_WRITE)
            enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_QT_SPLIT+depth,nd->split,1);
        if (nd->split) return 0;
        uint8_t intra = nd->intra;
        if (write == QT_WRITE)
            enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_BLOCK_MODE,intra,1);
        if (intra) {
            if (write == QT_WRITE) {
                enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_INTRA_MODE,nd->intra_mode,5);
                enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_BLOCK_MODE,nd->ch_intra,1);
                if (nd->ch_intra) enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_INTRA_MODE,nd->intra_cmode,3);
            }
            /* Replay starts from the CTU state restored once by qt_split;
             * per-leaf snapshots are unnecessary because leaves are emitted
             * in the same raster order the decoder consumes them. */
            tc_pixel_t ra[2*64+1], rl[2*64+1];
            tc_intra_get_ref_v2(enc->recon->y, enc->recon->stride_y, px,py,cu, enc->cfg.width, enc->cfg.height, ra+1, rl+1);
            tc_intra_predict(pred, cu, ra+1, rl+1, cu, (tc_intra_mode_t)nd->intra_mode);
            qt_code_luma(e,px,py,cu,nd->dct_size,pred,&bits_dummy,write == QT_WRITE);
            if (nd->ch_intra) {
                tc_intra_chroma_dc(enc->recon->cb,enc->recon->stride_c,px/2,py/2,cu/2,cbuf[0],cu/2);
                tc_intra_chroma_dc(enc->recon->cr,enc->recon->stride_c,px/2,py/2,cu/2,cbuf[1],cu/2);
            } else {
                tc_mv_s mv = qt_mvp(e,cx,cy,e->grid); mv.x+=px*4; mv.y+=py*4;
                if (enc->dpb[0].frame) {
                    tc_inter_predict_chroma(enc->dpb[0].frame->cb,enc->dpb[0].frame->stride_c, enc->cfg.width/2,enc->cfg.height/2,mv,cbuf[0],cu/2,cu/2);
                    tc_inter_predict_chroma(enc->dpb[0].frame->cr,enc->dpb[0].frame->stride_c, enc->cfg.width/2,enc->cfg.height/2,mv,cbuf[1],cu/2,cu/2);
                } else {
                    tc_intra_chroma_dc(enc->recon->cb,enc->recon->stride_c,px/2,py/2,cu/2,cbuf[0],cu/2);
                    tc_intra_chroma_dc(enc->recon->cr,enc->recon->stride_c,px/2,py/2,cu/2,cbuf[1],cu/2);
                }
            }
            qt_code_chroma(e,px,py,cu,1,cpred,&bits_dummy,write == QT_WRITE);
        } else {
            if (write == QT_WRITE) {
                if (e->frame_type==TC_FRAME_BIDIR) { enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_REF_SEL,nd->ref_sel,1); enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_BLOCK_MODE,nd->bi,1); }
                if (nd->skip) { enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_SKIP_FLAG,1,1); }
                else if (nd->merge) { enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_SKIP_FLAG,0,1); enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_MERGE_FLAG,1,1); }
                else { enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_SKIP_FLAG,0,1); enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_MERGE_FLAG,0,1); enc_write_bits(e->bs,e->rc,e->rc_ctx,RC_CTX_DCT_SIZE,nd->dct_size,1); enc_write_se(e->bs,e->rc,e->rc_ctx,RC_CTX_MVD_X,nd->mvd_x); enc_write_se(e->bs,e->rc,e->rc_ctx,RC_CTX_MVD_Y,nd->mvd_y); }
            }
            tc_mv_s mvp = qt_mvp(e,cx,cy,e->grid);
            tc_mv_s mv = { mvp.x+px*4+nd->mvd_x, mvp.y+py*4+nd->mvd_y };
            if (nd->merge || nd->skip) { mv=mvp; mv.x+=px*4; mv.y+=py*4; }
            if (!nd->skip) {
                if (e->frame_type==TC_FRAME_BIDIR) {
                    const tc_frame_buf_t *r1 = nd->ref_sel?enc->dpb[1].frame:enc->dpb[0].frame;
                    const tc_frame_buf_t *r2 = nd->ref_sel?enc->dpb[0].frame:enc->dpb[1].frame;
                    tc_pixel_t t1[64*64],t2[64*64];
                    tc_inter_predict(r1->y,r1->stride_y,enc->cfg.width,enc->cfg.height,mv,t1,cu,cu);
                    tc_mv_s mv2={-mv.x,-mv.y}; tc_inter_predict(r2->y,r2->stride_y,enc->cfg.width,enc->cfg.height,mv2,t2,cu,cu);
                    for (int i=0;i<cu*cu;i++) pred[i]=(tc_pixel_t)((t1[i]+t2[i]+1)>>1);
                } else { tc_inter_predict(enc->dpb[0].frame->y,enc->dpb[0].frame->stride_y, enc->cfg.width,enc->cfg.height,mv,pred,cu,cu); }
                qt_code_luma(e,px,py,cu,nd->dct_size,pred,&bits_dummy,write == QT_WRITE);
                if (nd->ch_intra) {
                    tc_intra_chroma_dc(enc->recon->cb,enc->recon->stride_c,px/2,py/2,cu/2,cbuf[0],cu/2);
                    tc_intra_chroma_dc(enc->recon->cr,enc->recon->stride_c,px/2,py/2,cu/2,cbuf[1],cu/2);
                } else {
                    tc_inter_predict_chroma(enc->dpb[0].frame->cb,enc->dpb[0].frame->stride_c, enc->cfg.width/2,enc->cfg.height/2,mv,cbuf[0],cu/2,cu/2);
                    tc_inter_predict_chroma(enc->dpb[0].frame->cr,enc->dpb[0].frame->stride_c, enc->cfg.width/2,enc->cfg.height/2,mv,cbuf[1],cu/2,cu/2);
                }
                qt_code_chroma(e,px,py,cu,0,cpred,&bits_dummy,write == QT_WRITE);
            } else {
                tc_inter_predict(enc->dpb[0].frame->y,enc->dpb[0].frame->stride_y, enc->cfg.width,enc->cfg.height,mv,pred,cu,cu);
                for (int i=0;i<cu*cu;i++) enc->recon->y[(py+i/cu)*enc->recon->stride_y+(px+i%cu)]=pred[i];
            }
        }

        /* Rebuild the MV predictor grid incrementally during the write
         * pass.  The decision pass leaves e->grid containing the final
         * state of the whole CTU; using that state directly would make
         * later written leaves derive a different MVP from the decoder,
         * which consumes the tree in raster order. */
        {
            int cs = cu / 8;
            if (nd->intra) {
                for (int gy = 0; gy < cs; gy++)
                    for (int gx = 0; gx < cs; gx++)
                        e->grid[(cy + gy) * TC_MVGRID_STRIDE + (cx + gx)].intra = 1;
            } else {
                tc_mv_s gmvp = qt_mvp(e, cx, cy, e->grid);
                int base_x = gmvp.x + px * 4 + ((nd->merge || nd->skip) ? 0 : nd->mvd_x);
                int base_y = gmvp.y + py * 4 + ((nd->merge || nd->skip) ? 0 : nd->mvd_y);
                for (int gy = 0; gy < cs; gy++)
                    for (int gx = 0; gx < cs; gx++) {
                        qt_mvcell_t *g = &e->grid[(cy + gy) * TC_MVGRID_STRIDE + (cx + gx)];
                        g->intra = 0;
                        g->dx = (int16_t)(base_x - (px + gx * 8) * 4);
                        g->dy = (int16_t)(base_y - (py + gy * 8) * 4);
                    }
            }
        }
        return 0;
    }

    tc_pixel_t ra[2*64+1], rl[2*64+1];
    tc_intra_get_ref_v2(enc->recon->y, enc->recon->stride_y, px,py,cu, enc->cfg.width, enc->cfg.height, ra+1, rl+1);
    int best_cost = 0x7FFFFFFF;
    uint8_t b_intra=0,b_bi=0,b_refsel=0,b_dct=TC_BLOCK_8x8_ID,b_skip=0,b_merge=0,b_ch=0;
    int b_imode=1,b_cmode=0,b_mvdx=0,b_mvdy=0;

    b_skip=0; b_merge=0;  /* keyframes are intra-only */
    if (e->frame_type != TC_FRAME_KEY) {
        tc_mv_s mvp = qt_mvp(e,cx,cy,e->grid);
        /* v2 presets deliberately trade RDO breadth for predictable ARM
         * encode time. Fast uses a compact search; medium retains the
         * broader search used by the original v2 path. */
        int sr = (enc->cfg.preset <= TC_PRESET_FAST) ? 16 : 32;
        tc_mv_s center = { mvp.x+px*4, mvp.y+py*4 };
        tc_sad_t sad; tc_mv_s bm = tc_motion_est(enc->dpb[0].frame->y, enc->dpb[0].frame->stride_y, enc->cfg.width,enc->cfg.height, enc->cur->y+py*enc->cur->stride_y+px, enc->cur->stride_y, center.x>>2, center.y>>2, cu, sr, &sad);
        tc_mv_s disp = { bm.x-(mvp.x+px*4), bm.y-(mvp.y+py*4) };
        tc_inter_predict(enc->dpb[0].frame->y,enc->dpb[0].frame->stride_y, enc->cfg.width,enc->cfg.height,bm,pred,cu,cu);
        int lb = 0;
        int64_t dl;
        if (fast_mode) {
            /* Fast presets screen the single ME candidate without
             * reconstructing it.  The selected leaf is reconstructed
             * exactly once by QT_REPLAY below. */
            int64_t sad_proxy = tc_sad(enc->cur->y + py * enc->cur->stride_y + px,
                                       enc->cur->stride_y, pred, cu, cu);
            dl = (sad_proxy * sad_proxy) / (int64_t)(cu * cu);
            lb = 1 + 2;
        } else {
            dl = qt_code_luma(e,px,py,cu,TC_BLOCK_8x8_ID,pred,&lb,0);
        }
        int bits_inter = 1 + 1 + 1 + 1 + 2 + (tc_bs_se_bits(disp.x)+tc_bs_se_bits(disp.y)) + lb;
        int cost = (int)(((dl + e->lambda*bits_inter)>>16)&0x7FFFFFFF);
        if (cost < best_cost) { best_cost=cost; b_intra=0;b_skip=0;b_merge=0;b_dct=TC_BLOCK_8x8_ID; b_mvdx=disp.x;b_mvdy=disp.y;b_refsel=0;b_bi=0;b_ch=0; b_cmode=0;b_imode=1; }
        if (enc->cfg.preset >= TC_PRESET_MEDIUM) {
            tc_mv_s mm={mvp.x+px*4,mvp.y+py*4}; tc_inter_predict(enc->dpb[0].frame->y,enc->dpb[0].frame->stride_y, enc->cfg.width,enc->cfg.height,mm,pred,cu,cu);
            int64_t dl2 = qt_code_luma(e,px,py,cu,TC_BLOCK_8x8_ID,pred,&lb,0);
            int bits_merge = 1+1+1+1+1+1+lb;
            int cost2=(int)(((dl2+e->lambda*bits_merge)>>16)&0x7FFFFFFF);
            if (cost2<best_cost) { best_cost=cost2;b_intra=0;b_merge=1;b_skip=0;b_mvdx=disp.x;b_mvdy=disp.y; b_dct=TC_BLOCK_8x8_ID;b_ch=0;b_cmode=0;b_imode=1;b_refsel=0;b_bi=0; }
        }
    }

    /* Keyframes have no reference frame; always provide an intra
     * candidate, including the root 64x64 CU on ultrafast. */
    int try_intra = (e->frame_type == TC_FRAME_KEY) ||
                    (cu <= 16) || (enc->cfg.preset >= TC_PRESET_MEDIUM);
    if (try_intra) {
        /* Fast/medium v2 presets use a cheap SAD screen for intra modes;
         * only the winning predictor is transformed and reconstructed.
         * Medium/slow retain full residual RDO for every mode. */
        int fast_intra = enc->cfg.preset <= TC_PRESET_FAST;
        int mode_step = enc->cfg.preset == TC_PRESET_ULTRAFAST ? 6 :
                        (enc->cfg.preset == TC_PRESET_FAST ? 3 : 1);
        const tc_pixel_t *orig_luma = enc->cur->y + py * enc->cur->stride_y + px;
        int best_imode = b_imode;
        for (int m=0;m<TC_INTRA_MODES;m += mode_step){
            tc_intra_predict(pred,cu,ra+1,rl+1,cu,(tc_intra_mode_t)m);
            int lb = 0;
            int64_t dl;
            if (fast_intra) {
                int64_t sad = tc_sad(orig_luma, enc->cur->stride_y, pred, cu, cu);
                dl = (sad * sad) / (int64_t)(cu * cu);
                lb = 1 + 5 + 1 + 1;
            } else {
                dl = qt_code_luma(e,px,py,cu,TC_BLOCK_8x8_ID,pred,&lb,0);
            }
            int bits = 1 + 5 + 1 + 1 + lb;
            int cost=(int)(((dl+e->lambda*bits)>>16)&0x7FFFFFFF);
            if (cost<best_cost){ best_cost=cost;b_intra=1;best_imode=m;b_dct=TC_BLOCK_8x8_ID; b_skip=0;b_merge=0;b_mvdx=0;b_mvdy=0;b_ch=0;b_cmode=0;b_refsel=0;b_bi=0; }
        }
        if (b_intra)
            b_imode = best_imode;
    }

    nd->intra=b_intra; nd->skip=b_skip; nd->merge=b_merge; nd->bi=b_bi;
    nd->intra_mode=(uint8_t)b_imode; nd->intra_cmode=(uint8_t)b_cmode;
    nd->ch_intra=b_ch; nd->ref_sel=(uint8_t)b_refsel; nd->dct_size=b_dct;
    nd->mvd_x=(int16_t)b_mvdx; nd->mvd_y=(int16_t)b_mvdy;

    /* Candidate coding mutates recon while estimating distortion. Restore
     * the pre-leaf state and replay only the winning candidate so later
     * leaves see the same pixels as the decoder. Fast screening does not
     * mutate reconstruction, but still needs the same selected replay. */
    if (!write) {
        if (!fast_mode) {
            qt_paste_rect(enc->recon->y, enc->recon->stride_y, px, py,
                          e->pre_luma, TC_CTU_SIZE, enc->cfg.width, enc->cfg.height, cu);
            qt_paste_rect(enc->recon->cb, enc->recon->stride_c, px / 2, py / 2,
                          e->pre_cb, TC_CTU_SIZE / 2, enc->cfg.width / 2, enc->cfg.height / 2, cu / 2);
            qt_paste_rect(enc->recon->cr, enc->recon->stride_c, px / 2, py / 2,
                          e->pre_cr, TC_CTU_SIZE / 2, enc->cfg.width / 2, enc->cfg.height / 2, cu / 2);
            int cs0 = cu / 8;
            for (int gy = 0; gy < cs0; gy++)
                memcpy(e->grid + (cy + gy) * TC_MVGRID_STRIDE + cx,
                       e->pre_grid + gy * cs0,
                       (size_t)cs0 * sizeof(qt_mvcell_t));
        }
        /* QT_REPLAY reconstructs only: all syntax helpers receive
         * write == QT_WRITE (false), so the real coder state remains
         * untouched and replay cannot silently discard syntax. */
        nd->split = 0;
        qt_leaf(e, depth, cx, cy, QT_REPLAY);
    }

    int cs = cu/8;

    for (int y=0;y<cs;y++) for (int x=0;x<cs;x++){
        qt_mvcell_t *g=e->grid + (cy+y)*TC_MVGRID_STRIDE + (cx+x);
        if (b_intra) { g->intra=1; }
        else { g->intra=0; tc_mv_s mvp = qt_mvp(e,cx,cy,e->grid);
            g->dx = (int16_t)((mvp.x+px*4+ (b_merge||b_skip?0:b_mvdx)) - ((px+x*8)*4));
            g->dy = (int16_t)((mvp.y+py*4+ (b_merge||b_skip?0:b_mvdy)) - ((py+y*8)*4)); }
    }
    return best_cost;
}

static void qt_split(qt_enc_t *e, int depth, int cx, int cy, int write)
{
    qt_node_t *nd = &e->node[tc_qt_index(depth,cx,cy)];
    int cu = 8 << (TC_QT_MAX_DEPTH-depth);
    int px = e->ctu_x + cx*8, py = e->ctu_y + cy*8;
    int in_frame = (px+cu <= e->enc->cfg.width && py+cu <= e->enc->cfg.height);

    /* Child offset in 8×8 cells is half the parent side: 2^(2-depth).
     * (The old (q&2)<<(1-depth) form was UB at depth 2 and silently
     * dropped the bottom half of every 16×16 CU.) */
    if (!in_frame && cu > TC_QT_MIN_CU) {
        for (int q=0;q<4;q++) qt_split(e,depth+1,cx+((q&1)<<(2-depth)),cy+((q>>1)<<(2-depth)),write);
        return;
    }
    if (!in_frame) return;
    if (write == QT_WRITE) {
        qt_leaf(e, depth, cx, cy, QT_WRITE);
        /* A split flag was written: the leaf's syntax stops there and
         * the four children follow in raster order.  (Earlier versions
         * returned unconditionally here, silently dropping every
         * descendant's syntax and producing streams that decoded to
         * flat mid-gray.) */
        if (nd->split) {
            for (int q = 0; q < 4; q++)
                qt_split(e, depth + 1, cx + ((q & 1) << (2 - depth)),
                         cy + ((q >> 1) << (2 - depth)), QT_WRITE);
        }
        return;
    }

    if (cu > TC_QT_MIN_CU) {
        /* Fast v2 presets use a bounded partition structure instead of
         * evaluating both the parent and all children. This avoids the
         * recursive RDO multiplier on ARM while preserving normative
         * quadtree syntax and exact decoder replay. p0 uses 8x8 leaves;
         * p1 uses 16x16 leaves. */
        int fast_leaf_depth = (e->enc->cfg.preset == TC_PRESET_ULTRAFAST) ? 3 : 2;
        if (e->enc->cfg.preset <= TC_PRESET_FAST && depth < fast_leaf_depth) {
            nd->split = 1;
            for (int q = 0; q < 4; q++)
                qt_split(e, depth + 1,
                         cx + ((q & 1) << (2 - depth)),
                         cy + ((q >> 1) << (2 - depth)), 0);
            return;
        }

        uint8_t pre_l[64*64], pre_cb[32*32], pre_cr[32*32];
        uint8_t parent_l[64*64], parent_cb[32*32], parent_cr[32*32];
        int cs = cu / 8;
        qt_mvcell_t pre_grid[8*8], parent_grid[8*8];

        /* Save the state before either candidate.  Child RDO must not
         * inherit the parent's reconstruction, and a rejected split must
         * restore the parent's chosen reconstruction rather than the last
         * speculative child. */
        qt_copy_rows(pre_l, e->enc->recon->y, e->enc->recon->stride_y, px, py, cu);
        qt_copy_rows(pre_cb, e->enc->recon->cb, e->enc->recon->stride_c, px/2, py/2, cu/2);
        qt_copy_rows(pre_cr, e->enc->recon->cr, e->enc->recon->stride_c, px/2, py/2, cu/2);
        for (int gy = 0; gy < cs; gy++)
            memcpy(pre_grid + gy*cs, e->grid + (cy+gy)*TC_MVGRID_STRIDE + cx,
                   (size_t)cs * sizeof(qt_mvcell_t));

        int64_t parent = qt_leaf(e, depth, cx, cy, 0);
        qt_copy_rows(parent_l, e->enc->recon->y, e->enc->recon->stride_y, px, py, cu);
        qt_copy_rows(parent_cb, e->enc->recon->cb, e->enc->recon->stride_c, px/2, py/2, cu/2);
        qt_copy_rows(parent_cr, e->enc->recon->cr, e->enc->recon->stride_c, px/2, py/2, cu/2);
        for (int gy = 0; gy < cs; gy++)
            memcpy(parent_grid + gy*cs, e->grid + (cy+gy)*TC_MVGRID_STRIDE + cx,
                   (size_t)cs * sizeof(qt_mvcell_t));

        qt_paste_rows(e->enc->recon->y, e->enc->recon->stride_y, px, py, pre_l, cu);
        qt_paste_rows(e->enc->recon->cb, e->enc->recon->stride_c, px/2, py/2, pre_cb, cu/2);
        qt_paste_rows(e->enc->recon->cr, e->enc->recon->stride_c, px/2, py/2, pre_cr, cu/2);
        for (int gy = 0; gy < cs; gy++)
            memcpy(e->grid + (cy+gy)*TC_MVGRID_STRIDE + cx, pre_grid + gy*cs,
                   (size_t)cs * sizeof(qt_mvcell_t));

        int64_t child = 0;
        for (int q = 0; q < 4; q++)
            child += qt_leaf(e, depth + 1,
                             cx + ((q & 1) << (2 - depth)),
                             cy + ((q >> 1) << (2 - depth)), 0);

        if (child <= parent) {
            nd->split = 1;
            qt_paste_rows(e->enc->recon->y, e->enc->recon->stride_y, px, py, pre_l, cu);
            qt_paste_rows(e->enc->recon->cb, e->enc->recon->stride_c, px/2, py/2, pre_cb, cu/2);
            qt_paste_rows(e->enc->recon->cr, e->enc->recon->stride_c, px/2, py/2, pre_cr, cu/2);
            for (int gy = 0; gy < cs; gy++)
                memcpy(e->grid + (cy+gy)*TC_MVGRID_STRIDE + cx, pre_grid + gy*cs,
                       (size_t)cs * sizeof(qt_mvcell_t));
            for (int q = 0; q < 4; q++)
                qt_split(e, depth + 1,
                         cx + ((q & 1) << (2 - depth)),
                         cy + ((q >> 1) << (2 - depth)), 0);
        } else {
            nd->split = 0;
            qt_paste_rows(e->enc->recon->y, e->enc->recon->stride_y, px, py, parent_l, cu);
            qt_paste_rows(e->enc->recon->cb, e->enc->recon->stride_c, px/2, py/2, parent_cb, cu/2);
            qt_paste_rows(e->enc->recon->cr, e->enc->recon->stride_c, px/2, py/2, parent_cr, cu/2);
            for (int gy = 0; gy < cs; gy++)
                memcpy(e->grid + (cy+gy)*TC_MVGRID_STRIDE + cx, parent_grid + gy*cs,
                       (size_t)cs * sizeof(qt_mvcell_t));
        }
    } else { qt_leaf(e,depth,cx,cy,0); nd->split=0; }
}

static void encode_ctu_v2(tc_encoder_t *enc, int row, int col, int qp,
                          tc_frame_type_t frame_type, int poc,
                          tc_bs_writer_t *bs, tc_tans_enc_t *tans,
                          tc_rc_enc_t *rc, tc_rc_ctx_t *rc_ctx)
{
    qt_enc_t e; memset(&e,0,sizeof(e));
    e.enc=enc; e.ctu_x=col*TC_CTU_SIZE; e.ctu_y=row*TC_CTU_SIZE;
    e.qp=qp; e.qp_c=tc_clip(qp+1,0,63); e.lambda=(int64_t)tc_lambda(qp)<<16;
    e.frame_type=frame_type; e.poc=poc; e.bs=bs; e.tans=tans; e.rc=rc; e.rc_ctx=rc_ctx;
    e.node=enc->v2_node; e.grid=enc->v2_grid;
    memset(e.node, 0, (size_t)TC_QT_NODES * sizeof(qt_node_t));
    memset(e.grid, 0, (size_t)TC_MVGRID_STRIDE*TC_MVGRID_STRIDE * sizeof(qt_mvcell_t));
    for (int i=0;i<TC_MVGRID_STRIDE*TC_MVGRID_STRIDE;i++) e.grid[i].intra=1;

    /* Keep the CTU input state once for replay. This replaces the old
     * per-leaf reconstruction snapshots; the decision pass may mutate the
     * CTU many times, but the write pass needs the exact pre-CTU state. */
    uint8_t pre_luma[TC_CTU_SIZE * TC_CTU_SIZE];
    uint8_t pre_cb[(TC_CTU_SIZE/2) * (TC_CTU_SIZE/2)];
    uint8_t pre_cr[(TC_CTU_SIZE/2) * (TC_CTU_SIZE/2)];
    qt_copy_rect(pre_luma, TC_CTU_SIZE, enc->recon->y, enc->recon->stride_y,
                 e.ctu_x, e.ctu_y, enc->cfg.width, enc->cfg.height, TC_CTU_SIZE);
    qt_copy_rect(pre_cb, TC_CTU_SIZE / 2, enc->recon->cb, enc->recon->stride_c,
                 e.ctu_x / 2, e.ctu_y / 2, enc->cfg.width / 2, enc->cfg.height / 2, TC_CTU_SIZE / 2);
    qt_copy_rect(pre_cr, TC_CTU_SIZE / 2, enc->recon->cr, enc->recon->stride_c,
                 e.ctu_x / 2, e.ctu_y / 2, enc->cfg.width / 2, enc->cfg.height / 2, TC_CTU_SIZE / 2);

    qt_split(&e,0,0,0,0);
    qt_paste_rect(enc->recon->y, enc->recon->stride_y, e.ctu_x, e.ctu_y,
                  pre_luma, TC_CTU_SIZE, enc->cfg.width, enc->cfg.height, TC_CTU_SIZE);
    qt_paste_rect(enc->recon->cb, enc->recon->stride_c, e.ctu_x / 2, e.ctu_y / 2,
                  pre_cb, TC_CTU_SIZE / 2, enc->cfg.width / 2, enc->cfg.height / 2, TC_CTU_SIZE / 2);
    qt_paste_rect(enc->recon->cr, enc->recon->stride_c, e.ctu_x / 2, e.ctu_y / 2,
                  pre_cr, TC_CTU_SIZE / 2, enc->cfg.width / 2, enc->cfg.height / 2, TC_CTU_SIZE / 2);
    /* The write pass must start with the same empty predictor grid that
     * the decoder has before consuming this CTU.  The decision pass
     * leaves e.grid in its final state, which is not valid for replay. */
    memset(e.grid, 0, (size_t)TC_MVGRID_STRIDE * TC_MVGRID_STRIDE * sizeof(qt_mvcell_t));
    for (int i = 0; i < TC_MVGRID_STRIDE * TC_MVGRID_STRIDE; i++)
        e.grid[i].intra = 1;
    qt_split(&e,0,0,0,QT_WRITE);
    if (e.ctu_x+TC_CTU_SIZE <= enc->cfg.width && e.ctu_y+TC_CTU_SIZE <= enc->cfg.height)
        tc_deblock_ctu(enc->recon->y, enc->recon->stride_y, enc->recon->cb, enc->recon->stride_c, enc->recon->cr, enc->recon->stride_c, e.ctu_x, e.ctu_y, qp);

    /* v2 SAO syntax is emitted after deblocking, so the encoder and
     * decoder apply the identical post-deblock operation.  The decision
     * is deliberately a bounded one-band BO search; v0/v1 never carry it. */
    {
        int sao_band = 0, sao_offset = 0;
        int has_sao = tc_sao_choose_bo(enc->cur->y, enc->cur->stride_y,
                                       enc->recon->y, enc->recon->stride_y,
                                       e.ctu_x, e.ctu_y,
                                       enc->cfg.width, enc->cfg.height,
                                       &sao_band, &sao_offset);
        /* Account for the one-bit present flag, 5-bit band, and signed
         * offset syntax in the selection cost. The helper already ensures
         * the unfiltered SSE is strictly better before returning true. */
        if (has_sao) {
            int64_t no_sao_cost = 0;
            int64_t sao_cost = 0;
            int x1 = tc_min(e.ctu_x + TC_CTU_SIZE, enc->cfg.width);
            int y1 = tc_min(e.ctu_y + TC_CTU_SIZE, enc->cfg.height);
            for (int sy = e.ctu_y; sy < y1; ++sy) for (int sx = e.ctu_x; sx < x1; ++sx) {
                int o = enc->cur->y[sy * enc->cur->stride_y + sx];
                int r = enc->recon->y[sy * enc->recon->stride_y + sx];
                int d0 = o - r;
                int r1 = (tc_clip(r, 0, 255) >> 3) == sao_band ? tc_clip(r + sao_offset, 0, 255) : r;
                int d1 = o - r1;
                no_sao_cost += (int64_t)d0 * d0;
                sao_cost += (int64_t)d1 * d1;
            }
            if (sao_cost + ((int64_t)tc_lambda(qp) * 10 >> 16) >= no_sao_cost)
                has_sao = 0;
        }
        enc_write_bits(bs, rc, rc_ctx, RC_CTX_SAO_TYPE, (uint32_t)has_sao, 1);
        if (has_sao) {
            enc_write_bits(bs, rc, rc_ctx, RC_CTX_SAO_BAND, (uint32_t)sao_band, 5);
            /* Offset is a bounded mapped nibble: 0..14 maps to -7..+7;
             * 15 is reserved. Keeping this fixed-width avoids range-context
             * aliasing at the end of the 64-entry context bank. */
            enc_write_bits(bs, rc, rc_ctx, RC_CTX_SAO_OFFSET,
                           (uint32_t)(sao_offset + 7), 4);
            tc_sao_ctu_luma(enc->recon->y, enc->recon->stride_y,
                            e.ctu_x, e.ctu_y, enc->cfg.width, enc->cfg.height,
                            sao_band, sao_offset);
        }
    }
}

/* ── Encode one 8×8 block ───────────────────────────────────── */

static void encode_block(tc_encoder_t *enc, tc_ctu_info_t *ctu,
                         int blk_idx, int bx, int by,
                         int frame_x, int frame_y,
                         int qp, tc_frame_type_t frame_type, int frame_poc,
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

    static const tc_intra_mode_t fast_modes[] = {
        0, 1, 2, 5, 8, 9, 13, 17
    };
    for (int mi = 0; mi < 8; mi++) {
        int m = fast_modes[mi];
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
    if (best_intra_sad > 256) {
        for (int m = 0; m < TC_INTRA_MODES; m++) {
            int skip = 0;
            for (int k = 0; k < 8; k++)
                if (m == fast_modes[k]) { skip = 1; break; }
            if (skip) continue;
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

    tc_mv_s  merge_mv = {frame_x * 4, frame_y * 4};
    tc_sad_t merge_sad = 0x7FFFFFFF;
    int merge_ref_sel = 0;
    int merge_available = 0;

     if (frame_type != TC_FRAME_KEY && best_intra_sad > 32) {

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

        /* Candidate reference frames:
         *  - P-frames: DPB slots (multi-ref on slow preset only)
         *  - B-frames: forward ref (max POC < cur) and backward ref
         *    (min POC > cur); the 1-bit ref_sel selects the winner.
         * Baseline-mobile profile must never produce multi-ref P
         * bitstreams — a baseline-mobile decoder wouldn't know how
         * to handle ref_idx. */
        const tc_frame_buf_t *cand_ref[2];
        int cand_sel[2];
        int n_cand = 0;
        if (frame_type == TC_FRAME_BIDIR) {
            const tc_frame_buf_t *fwd = dpb_find_poc_lt(enc->dpb, frame_poc);
            const tc_frame_buf_t *bwd = dpb_find_poc_gt(enc->dpb, frame_poc);
            if (fwd) { cand_ref[n_cand] = fwd; cand_sel[n_cand] = 0; n_cand++; }
            if (bwd) { cand_ref[n_cand] = bwd; cand_sel[n_cand] = 1; n_cand++; }
        } else {
            int max_refs = (enc->cfg.preset == TC_PRESET_SLOW &&
                            enc->cfg.profile >= TC_PROFILE_STREAMING_MAIN) ? TC_REF_FRAMES : 1;
            for (int ri = 0; ri < max_refs && n_cand < 2; ri++) {
                if (enc->dpb[ri].frame) {
                    cand_ref[n_cand] = enc->dpb[ri].frame;
                    cand_sel[n_cand] = ri;
                    n_cand++;
                }
            }
        }
        for (int ci = 0; ci < n_cand; ci++) {
            const tc_frame_buf_t *rframe = cand_ref[ci];

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
                best_ref_idx = cand_sel[ci];
            }
        }

        /* B-frames: bidirectional average candidate (D4). The winning
         * MV is applied to both references (backward one mirrored) and
         * the averaged prediction competes against the single-ref
         * results; ref_sel = 2 signals it. */
        if (frame_type == TC_FRAME_BIDIR && n_cand == 2) {
            const tc_frame_buf_t *fwd = cand_ref[0];
            const tc_frame_buf_t *bwd = cand_ref[1];
            tc_pixel_t pa[64], pb[64];
            tc_inter_predict(fwd->y, fwd->stride_y,
                             fwd->width, fwd->height,
                             best_mv, pa, blk_size, blk_size);
            tc_mv_s mvb = { -best_mv.x, -best_mv.y };
            tc_inter_predict(bwd->y, bwd->stride_y,
                             bwd->width, bwd->height,
                             mvb, pb, blk_size, blk_size);
            tc_sad_t s = 0;
            for (int i2 = 0; i2 < blk_size * blk_size; i2++) {
                tc_pixel_t avg = (tc_pixel_t)((pa[i2] + pb[i2] + 1) >> 1);
                s += (tc_sad_t)abs((int)orig_block[i2] - (int)avg);
            }
            if (s < best_inter_sad) {
                best_inter_sad = s;
                best_ref_idx = 2;
            }
        }

        /* Generate inter prediction from best reference */
        if (best_inter_sad < best_intra_sad) {
            const tc_frame_buf_t *best_ref = NULL;
            if (frame_type == TC_FRAME_BIDIR && best_ref_idx == 2) {
                const tc_frame_buf_t *fwd = dpb_find_poc_lt(enc->dpb, frame_poc);
                const tc_frame_buf_t *bwd = dpb_find_poc_gt(enc->dpb, frame_poc);
                if (fwd && bwd) {
                    tc_pixel_t pa[64], pb[64];
                    tc_inter_predict(fwd->y, fwd->stride_y,
                                     fwd->width, fwd->height,
                                     best_mv, pa, blk_size, blk_size);
                    tc_mv_s mvb = { -best_mv.x, -best_mv.y };
                    tc_inter_predict(bwd->y, bwd->stride_y,
                                     bwd->width, bwd->height,
                                     mvb, pb, blk_size, blk_size);
                    for (int i2 = 0; i2 < blk_size * blk_size; i2++) {
                        inter_pred[i2] = (tc_pixel_t)((pa[i2] + pb[i2] + 1) >> 1);
                    }
                }
            } else if (frame_type == TC_FRAME_BIDIR) {
                best_ref = best_ref_idx ? dpb_find_poc_gt(enc->dpb, frame_poc)
                                        : dpb_find_poc_lt(enc->dpb, frame_poc);
            } else {
                best_ref = enc->dpb[best_ref_idx].frame;
            }
            if (best_ref) {
                tc_inter_predict(best_ref->y, best_ref->stride_y,
                                 best_ref->width, best_ref->height,
                                 best_mv, inter_pred, blk_size, blk_size);
                blk->is_intra = 0;
                blk->mv = best_mv;
                blk->ref_idx = (uint8_t)best_ref_idx;
                memcpy(pred_block, inter_pred, (size_t)n_coeff * sizeof(tc_pixel_t));
            } else if (frame_type == TC_FRAME_BIDIR && best_ref_idx == 2) {
                blk->is_intra = 0;
                blk->mv = best_mv;
                blk->ref_idx = 2;
                memcpy(pred_block, inter_pred, (size_t)n_coeff * sizeof(tc_pixel_t));
            }
        }

        /* Compute merge SAD: prediction quality at the merge MV position.
         * This determines whether merge mode (implicit MV, no ref_idx/MVD)
         * is a good choice. For B-frames the better of the two references
         * wins, and ref_sel is signaled. */
        if (merge_available) {
            for (int ci = 0; ci < n_cand; ci++) {
                const tc_frame_buf_t *rframe = cand_ref[ci];
                int mx = merge_mv.x >> 2;
                int my = merge_mv.y >> 2;
                if (mx >= 0 && my >= 0 &&
                    mx + blk_size <= rframe->width &&
                    my + blk_size <= rframe->height) {
                    tc_sad_t s = tc_sad(rframe->y + my * rframe->stride_y + mx,
                                        rframe->stride_y,
                                        orig_block, blk_size, blk_size);
                    if (s < merge_sad) {
                        merge_sad = s;
                        merge_ref_sel = cand_sel[ci];
                    }
                }
            }
            /* B-frames: bidirectional merge candidate (avg of both
             * references at the merge MV, backward mirrored). */
            if (frame_type == TC_FRAME_BIDIR && n_cand == 2) {
                const tc_frame_buf_t *fwd = cand_ref[0];
                const tc_frame_buf_t *bwd = cand_ref[1];
                tc_pixel_t pa[64], pb[64];
                tc_inter_predict(fwd->y, fwd->stride_y,
                                 fwd->width, fwd->height,
                                 merge_mv, pa, blk_size, blk_size);
                tc_mv_s mvb = { -merge_mv.x, -merge_mv.y };
                tc_inter_predict(bwd->y, bwd->stride_y,
                                 bwd->width, bwd->height,
                                 mvb, pb, blk_size, blk_size);
                tc_sad_t s = 0;
                for (int i2 = 0; i2 < blk_size * blk_size; i2++) {
                    tc_pixel_t avg = (tc_pixel_t)((pa[i2] + pb[i2] + 1) >> 1);
                    s += (tc_sad_t)abs((int)orig_block[i2] - (int)avg);
                }
                if (s < merge_sad) {
                    merge_sad = s;
                    merge_ref_sel = 2;
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
        if (frame_type == TC_FRAME_BIDIR && merge_ref_sel == 2) {
            const tc_frame_buf_t *fwd = dpb_find_poc_lt(enc->dpb, frame_poc);
            const tc_frame_buf_t *bwd = dpb_find_poc_gt(enc->dpb, frame_poc);
            if (fwd && bwd) {
                tc_pixel_t pa[64], pb[64];
                tc_inter_predict(fwd->y, fwd->stride_y,
                                 fwd->width, fwd->height,
                                 merge_mv, pa, blk_size, blk_size);
                tc_mv_s mvb = { -merge_mv.x, -merge_mv.y };
                tc_inter_predict(bwd->y, bwd->stride_y,
                                 bwd->width, bwd->height,
                                 mvb, pb, blk_size, blk_size);
                for (int i2 = 0; i2 < n_coeff; i2++) {
                    pred_block[i2] = (tc_pixel_t)((pa[i2] + pb[i2] + 1) >> 1);
                }
            } else {
                memset(pred_block, 128, (size_t)n_coeff);
            }
        } else {
            const tc_frame_buf_t *rframe = NULL;
            if (frame_type == TC_FRAME_BIDIR) {
                rframe = merge_ref_sel ? dpb_find_poc_gt(enc->dpb, frame_poc)
                                       : dpb_find_poc_lt(enc->dpb, frame_poc);
            } else {
                rframe = enc->dpb[0].frame;
            }
            if (rframe) {
                tc_inter_predict(rframe->y, rframe->stride_y,
                                 rframe->width, rframe->height,
                                 merge_mv, pred_block, blk_size, blk_size);
            } else {
                /* Safety: should never be NULL on inter frames, but if it is,
                 * fill prediction with 128 to avoid stale intra pred data. */
                memset(pred_block, 128, (size_t)n_coeff);
            }
        }
        blk->is_intra = 0;
        blk->mv = merge_mv;
        blk->ref_idx = (uint8_t)merge_ref_sel;
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

    /* B-frames: ref selection for every inter-coded block
     * (skip / inter / merge): 0 = forward, 1 = backward, 2 = average
     * of both (bidirectional). The context is derived from the
     * neighbors' refs (D4): class = ref_sel_ctx(). */
    if (frame_type == TC_FRAME_BIDIR && !blk->is_intra) {
        enc_write_bits(bs, rc, rc_ctx, RC_CTX_REF_SEL,
                       (uint32_t)(blk->ref_idx & 3), 2);
    }

    if (blk->is_intra && !is_skip && !is_merge) {
        enc_write_bits(bs, rc, rc_ctx, RC_CTX_INTRA_MODE, (uint32_t)blk->intra_mode, 5);
    }

    /* ref_idx + MVD: written for inter/skip only (NOT merge — merge
     * derives MV from spatial neighbors, saving ref_idx + MVD bits). */
    if (!blk->is_intra && !is_merge) {
        /* Write ref_idx (2 bits, supports up to 4 reference frames).
         * Written for inter/skip blocks on P-frames only — B-frames
         * transmit ref_sel instead (their refs are POC-implied). */
        if (frame_type != TC_FRAME_KEY && frame_type != TC_FRAME_BIDIR) {
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
    int poc;
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
        if (enc->cfg.use_v2) {
            encode_ctu_v2(enc, row, col, qp, rctx->frame_type, rctx->poc,
                          bs, tans, rc, rc_ctx);
            continue;
        }
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
                                 qp, rctx->frame_type, rctx->poc, bs, tans,
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

        /* v1 extension header (D4): one byte carrying the frame type
         * code (bits 0-1: 0=KEY, 1=INTER, 2=BIDIR), written on every
         * frame of a B-frame stream. The flags byte has no spare bits
         * (tiles occupy 0-2), so B/stream signaling lives here. */
        if (hdr->has_ext_header) {
            uint8_t fc = (hdr->frame_type == TC_FRAME_KEY) ? 0 :
                         (hdr->frame_type == TC_FRAME_BIDIR) ? 2 : 1;
            tc_bs_writer_write_bits(bs, fc, 8);
        }
    }

    tc_bs_writer_byte_align(bs);
}

/* ── Encoder create/destroy ──────────────────────────────────── */

tc_encoder_t *tc_encoder_create(const tc_config_t *config)
{
    if (!config) return NULL;
    if (config->use_v2 &&
        (config->width <= 0 || config->height <= 0 ||
         (config->width & 1) || (config->height & 1)))
        return NULL; /* v2 4:2:0 geometry must be positive and even */
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

    /* B-frame reorder state.  The v2 quadtree path does not implement
     * B-frame reference selection (its RDO only searches dpb[0] and the
     * BIDIR write path would dereference dpb[1] unconditionally), so
     * B-frames are disabled for v2 streams.  This is a documented
     * limitation; v2 syntax still reserves the ref_sel/bi bits. */
    enc->bf.b_mode = (config->enable_b_frames && !config->use_v2) ? 1 : 0;

    /* Bitstream v2 quadtree scratch (per-encoder, no shared statics) */
    if (config->use_v2) {
        enc->v2_node = (qt_node_t *)calloc((size_t)TC_QT_NODES, sizeof(qt_node_t));
        enc->v2_grid = (qt_mvcell_t *)calloc(
            (size_t)TC_MVGRID_STRIDE * TC_MVGRID_STRIDE, sizeof(qt_mvcell_t));
        if (!enc->v2_node || !enc->v2_grid) {
            tc_encoder_destroy(enc);
            return NULL;
        }
    }

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
     * with WPP — disable WPP when it is active.  v2 is also never
     * WPP-coded (sequential by design, like all range-coded streams). */
    enc->use_wpp = (enc->num_ctu_rows > 1 && num_threads > 1 &&
                    !enc->use_entropy_coded && !config->use_v2) ? 1 : 0;

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
    /* B-frame pending display buffer */
    for (int i = 0; i < enc->bf.n; i++) {
        tc_frame_free(enc->bf.frame[i]);
    }
    free(enc->ctu_data);
    free(enc->v2_node);
    free(enc->v2_grid);
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

tc_error_t tc_encoder_flush_tail(tc_encoder_t *enc, tc_packet_t *packet_out);
static int  bf_can_emit(const tc_encoder_t *enc);
static tc_error_t bf_emit_scheduled(tc_encoder_t *enc, tc_packet_t *out);

/* ── Main encode function ────────────────────────────────────── */

static tc_error_t encode_poc_frame(tc_encoder_t *enc,
                              const tc_frame_buf_t *frame,
                              int poc, int b_layer_qp_off,
                              tc_packet_t *packet_out)
{
    /* Source pixels: encode_block reads enc->cur, so the display
     * frame (bf buffer in B-mode) must be staged into enc->cur. */
    tc_frame_copy(enc->cur, frame);

    /* Determine frame type. With B-frames the POC (display position)
     * drives the decision: keyframes land on anchor positions that are
     * multiples of the keyframe interval. Without B-frames the legacy
     * path (frame_count == frame) is preserved exactly, including scene
     * cut detection (disabled in B-mode — the reorder buffer makes
     * per-frame cuts ambiguous). */
    int keyframe_interval = enc->cfg.keyframe_interval > 0
        ? enc->cfg.keyframe_interval : 30;
    int is_key;
    if (enc->bf.b_mode) {
        is_key = enc->force_keyframe || (poc == 0) || (poc % keyframe_interval == 0);
    } else {
        is_key = enc->force_keyframe || (enc->frame_count == 0);
        if (!is_key && (enc->frame_count % keyframe_interval == 0)) {
            is_key = 1;
        }
        if (!is_key && enc->frame_count > 0 && enc->dpb[0].frame != NULL) {
            double cut_dist = histogram_distance(
                frame->y, frame->stride_y,
                enc->dpb[0].frame->y, enc->dpb[0].frame->stride_y,
                enc->cfg.width, enc->cfg.height);
            if (cut_dist > SCENE_CUT_THRESHOLD) {
                is_key = 1;
            }
        }
    }
    enc->force_keyframe = 0;

    tc_frame_type_t frame_type;
    if (is_key)                 frame_type = TC_FRAME_KEY;
    else if (b_layer_qp_off > 0) frame_type = TC_FRAME_BIDIR;
    else                        frame_type = TC_FRAME_INTER;

    /* Rate control: hierarchical QP offsets for B layers (D4):
     * middle B (+2) one step above its anchors, outer B's (+1/+3)
     * two steps — a classic hierarchical quality ladder. */
    tc_ratectl_frame_start(&enc->rc, frame_type);
    int qp = tc_ratectl_get_qp(&enc->rc);
    if (frame_type == TC_FRAME_BIDIR) {
        qp = tc_clip(qp + b_layer_qp_off, 0, 63);
    }

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
    if (enc->cfg.use_v2) hdr.version = TC_VERSION_V2;
    hdr.width    = (uint16_t)enc->cfg.width;
    hdr.height   = (uint16_t)enc->cfg.height;
    hdr.flags    = is_key ? TC_FLAG_KEY_FRAME : 0;
#if !defined(TCODEC_NO_THREADS)
    if (enc->use_wpp) hdr.flags |= TC_FLAG_WPP;
#endif

    /* v1/v2 header fields (v0 keeps its 12-byte legacy header) */
    if (hdr.version != TC_VERSION_V0) {
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
        /* B-frames (D4): signaled per frame so the decoder knows the
         * block syntax carries the ref_sel bit. */
        if (frame_type == TC_FRAME_BIDIR) {
            tools |= TC_TOOL_BIPRED;
        }
        /* v2 always carries one bounded SAO-present flag per CTU. */
        if (enc->cfg.use_v2) {
            tools |= TC_TOOL_SAO;
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
        /* B-frame streams carry the per-frame type extension (D4):
         * written on every frame so the decoder activates display
         * reorder from the first packet. */
        hdr.has_ext_header = enc->bf.b_mode ? 1 : 0;
        if (hdr.has_ext_header) hdr.flags |= TC_FLAG_EXT_HEADER;
    }

    hdr.qp_delta  = (uint8_t)(int8_t)(qp - TC_QP_DEFAULT);
    hdr.frame_num = (uint8_t)(poc & 0xFF);   /* POC = display index */
    hdr.frame_type = frame_type;
    hdr.qp = (uint8_t)qp;

    write_frame_header(&enc->bs, &hdr);

    /* After scene cut keyframes, clear ALL DPB entries to avoid
     * referencing frames from before the cut. Key frames don't use
     * inter prediction, so clearing dpb[0] too is safe. If we only
     * cleared slots 1+, the DPB shift after encoding would copy the
     * stale dpb[0] (pre-cut frame) into dpb[1]. */
    if (is_key && poc > 0) {
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
    rctx.poc = poc;

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

    /* Append CRC-16 if the header carries TC_FLAG_CRC (v1/v2) */
    if (hdr.version != TC_VERSION_V0 && hdr.has_crc) {
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
    double psnr = tc_psnr(frame->y, frame->stride_y,
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
    enc->dpb[0].poc = poc;
    enc->dpb[0].qp_avg = (uint8_t)qp;

    /* Fill output packet */
    packet_out->data = enc->out_buf;
    packet_out->size = frame_bytes;
    packet_out->pts  = poc;
    packet_out->key_frame = is_key;

    enc->total_bytes += (int64_t)frame_bytes;
    enc->total_frames++;
    enc->frame_count++;

    return TC_OK;
}

/*
 * ── B-frame emission path (D4) ───────────────────────────────────
 * display-order inputs are buffered in enc->bf; the codec emits
 * packets in coding order so every reference is already decoded:
 *   A(0), then for group k=1,2,...: A(4k), B(4k-2), B(4k-3), B(4k-1).
 * B(4k-2) references {A(4k-4), A(4k)}, B(4k-3) references
 * {A(4k-4), B(4k-2)}, B(4k-1) references {B(4k-2), A(4k)} — the
 * hierarchical ladder. QP offsets: middle B +1, outer B's +2.
 */
static int bf_schedule_poc(int emit_pos)
{
    if (emit_pos == 0) return 0;
    int k  = (emit_pos + 3) / 4;
    int c  = (emit_pos - 1) % 4;
    const int off[4] = {0, -2, -3, -1};
    return 4 * k + off[c];
}

static int bf_sched_qp_off(int emit_pos)
{
    (void)emit_pos;
    return 0;  /* D4 tunable: B frames at anchor QP */
}

static void bf_push(tc_encoder_t *enc, const tc_frame_buf_t *frame, int poc)
{
    if (enc->bf.n >= 8) return;
    enc->bf.frame[enc->bf.n] = tc_frame_clone(frame);
    enc->bf.poc[enc->bf.n] = poc;
    enc->bf.n++;
}

static int bf_find(const tc_encoder_t *enc, int poc)
{
    for (int i = 0; i < enc->bf.n; i++)
        if (enc->bf.poc[i] == poc) return i;
    return -1;
}

static void bf_remove(tc_encoder_t *enc, int idx)
{
    tc_frame_free(enc->bf.frame[idx]);
    for (int i = idx; i < enc->bf.n - 1; i++) {
        enc->bf.frame[i] = enc->bf.frame[i + 1];
        enc->bf.poc[i] = enc->bf.poc[i + 1];
    }
    enc->bf.n--;
}

/*
 * ── Encode one frame (public entry) ──────────────────────────────
 */
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

    if (enc->bf.b_mode) {
        /* Buffer this display input, then emit the next coding-order
         * packet when its GOP is complete (TC_ERR_NEED_MORE signals
         * the caller to keep feeding frames; the CLI drains the tail
         * with tc_encoder_flush_tail after the last input). */
        bf_push(enc, enc->cur, enc->bf.next_input);
        enc->bf.next_input++;
        if (!bf_can_emit(enc)) return TC_ERR_NEED_MORE;
        return bf_emit_scheduled(enc, packet_out);
    }

    /* Legacy path: exactly one packet per input frame. */
    return encode_poc_frame(enc, enc->cur, enc->frame_count, 0, packet_out);
}

/*
 * ── Tail drain ───────────────────────────────────────────────────
 * Encodes buffered display frames that could not form a complete
 * GOP (the tail of the sequence) as forward-only P frames. Also used
 * as the regular emit path when B-frames are enabled.
 */
/* Position of the anchor slot (A(4k)) inside the current group:
 * emit_pos 1,5,9,... are anchor slots themselves. */
static int bf_anchor_slot(int emit_pos)
{
    if (emit_pos == 0) return 0;
    return emit_pos - ((emit_pos - 1) % 4);
}

/* Can the next scheduled packet be emitted? Its own POC must be
 * buffered, and the group anchor must be available: buffered for
 * the anchor slot itself, or already emitted (it lives in the DPB)
 * for the B slots that follow it. */
static int bf_can_emit(const tc_encoder_t *enc)
{
    if (enc->bf.n == 0) return 0;
    int p = enc->bf.emit_pos;
    int want = bf_schedule_poc(p);
    if (bf_find(enc, want) < 0) return 0;
    if (p == 0) return 1;
    if ((p - 1) % 4 == 0) return 1;         /* anchor slot: want == anchor */
    return enc->bf.emit_pos > bf_anchor_slot(p);
}

/* Emit the next scheduled packet (caller verified bf_can_emit). */
static tc_error_t bf_emit_scheduled(tc_encoder_t *enc, tc_packet_t *out)
{
    int p = enc->bf.emit_pos;
    int poc = bf_schedule_poc(p);
    int idx = bf_find(enc, poc);
    enc->bf.emit_pos++;
    tc_error_t r = encode_poc_frame(enc, enc->bf.frame[idx], poc,
                                    bf_sched_qp_off(p), out);
    bf_remove(enc, idx);
    return r;
}

tc_error_t tc_encoder_flush_tail(tc_encoder_t *enc, tc_packet_t *packet_out)
{
    if (!enc->bf.b_mode) return TC_ERR_EOF;

    /* 1) Keep advancing the schedule while it is satisfiable. */
    if (bf_can_emit(enc)) return bf_emit_scheduled(enc, packet_out);

    /* 2) End-of-stream: the remaining display frames cannot form a
     * complete GOP (its future anchor never arrived). Emit them as
     * forward-only P frames in display order. */
    if (enc->bf.n > 0) {
        int best = 0;
        for (int i = 1; i < enc->bf.n; i++)
            if (enc->bf.poc[i] < enc->bf.poc[best]) best = i;
        int pb = enc->bf.poc[best];
        tc_error_t r = encode_poc_frame(enc, enc->bf.frame[best], pb, 0,
                                        packet_out);
        bf_remove(enc, best);
        return r;
    }
    return TC_ERR_EOF;
}
