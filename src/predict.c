/*
 * predict.c — Intra prediction for TCodec
 *
 * 18 intra prediction modes:
 *   0: Planar  (bilinear interpolation from boundaries)
 *   1: DC      (average of reference samples)
 *   2..8: Vertical angular (directional prediction along 7 angles)
 *   9..17: Horizontal angular (directional prediction along 9 angles)
 *
 * Reference samples come from:
 *   - Above: reconstructed pixels directly above the block
 *   - Left:  reconstructed pixels to the left of the block
 *   - If unavailable (frame boundary), use DC substitution
 *
 * All operations are integer-only, NEON-friendly.
 */

#include "tcodec_common.h"
#include <string.h>

/* ── Reference sample collection ─────────────────────────────── */

void tc_intra_get_ref(const tc_pixel_t *recon, int stride,
                      int x, int y, int blk_size, int frame_w, int frame_h,
                      tc_pixel_t *ref_above, tc_pixel_t *ref_left)
{
    /* Default value when no reference available */
    tc_pixel_t default_val = 128;

    /* Above row: positions (x-1, y-1) .. (x+2*blk_size-1, y-1)
     * But we need x-1 to x+2*blk_size-2 (2*blk_size - 1 samples above + top-right)
     * Simplified: we get blk_size*2 samples from above */
    int have_above = (y > 0);
    int have_left  = (x > 0);

    if (have_above) {
        const tc_pixel_t *above_row = recon + (y - 1) * stride;
        /* Top-left corner */
        if (have_left) {
            ref_left[blk_size] = above_row[x - 1];  /* Store above-left in ref_left[n] */
        }
        /* Above samples */
        for (int i = 0; i < 2 * blk_size; i++) {
            int px = x + i;
            if (px < frame_w) {
                ref_above[i] = above_row[px];
            } else {
                /* Repeat last available */
                ref_above[i] = above_row[frame_w - 1];
            }
        }
        /* Fill the last interpolation slot (angular modes clip idx+1 up to
         * 2*blk_size). Repeat the last sample so no uninitialized reads. */
        ref_above[2 * blk_size] = ref_above[2 * blk_size - 1];
        if (have_left) {
            ref_above[-1] = above_row[x - 1];
        } else {
            ref_above[-1] = ref_above[0];
        }
    } else {
        for (int i = 0; i < 2 * blk_size; i++) {
            ref_above[i] = default_val;
        }
        ref_above[2 * blk_size] = default_val;
        ref_above[-1] = default_val;
    }

    /* Left column: positions (x-1, y-1) .. (x-1, y+2*blk_size-2) */
    if (have_left) {
        for (int i = 0; i < 2 * blk_size; i++) {
            int py = y + i;
            if (py < frame_h) {
                ref_left[i] = recon[py * stride + (x - 1)];
            } else {
                ref_left[i] = recon[(frame_h - 1) * stride + (x - 1)];
            }
        }
        /* Fill the last interpolation slot (angular modes read idx+1 up to
         * 2*blk_size). Repeat the last sample value. */
        ref_left[2 * blk_size] = ref_left[2 * blk_size - 1];
        /* Above-left is shared with ref_above[-1] */
        if (have_above) {
            ref_left[-1] = recon[(y - 1) * stride + (x - 1)];
        } else {
            ref_left[-1] = ref_left[0];
        }
    } else {
        for (int i = 0; i < 2 * blk_size; i++) {
            ref_left[i] = default_val;
        }
        ref_left[2 * blk_size] = default_val;
        ref_left[-1] = default_val;
    }

    /* If neither available, use DC */
    if (!have_above && !have_left) {
        for (int i = 0; i < 2 * blk_size; i++) {
            ref_above[i] = default_val;
            ref_left[i]  = default_val;
        }
        ref_above[2 * blk_size] = default_val;
        ref_left[2 * blk_size]  = default_val;
        ref_above[-1] = default_val;
        ref_left[-1]  = default_val;
    }
}

/* ── Planar prediction (Mode 0) ────────────────────────────────
 *
 * Bilinear interpolation from top and left boundaries:
 *   pred[x][y] = (top[x] * (N-y) + left[y] * (N-x) + top_left * x * y + N) / (2*N)
 * where N = block_size.
 *
 * This smoothly blends horizontal and vertical predictions.
 * ══════════════════════════════════════════════════════════════ */

static void intra_planar(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                         const tc_pixel_t *ref_above,
                         const tc_pixel_t *ref_left,
                         int blk_size)
{
    int N = blk_size;
    (void)N;  /* Used implicitly in the angular prediction modes */

    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int v_left  = ref_left[y + 1];      /* Left column */
            int v_top   = ref_above[x + 1];     /* Top row */
            int v_tl    = ref_above[0];         /* Top-left */

            int pred = (v_top * (N - 1 - y) + v_left * (N - 1 - x)
                        + v_tl * (x + y) + N) / (2 * N);
            dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
        }
    }
}

/* ── DC prediction (Mode 1) ──────────────────────────────────── */

static void intra_dc(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                     const tc_pixel_t *ref_above,
                     const tc_pixel_t *ref_left,
                     int blk_size)
{
    int sum = 0;
    int count = 0;

    for (int i = 0; i < blk_size; i++) {
        sum += ref_above[i + 1];
        sum += ref_left[i + 1];
        count += 2;
    }

    int dc_val = (sum + (count >> 1)) / count;

    for (int y = 0; y < blk_size; y++) {
        for (int x = 0; x < blk_size; x++) {
            dst[y * dst_stride + x] = (tc_pixel_t)dc_val;
        }
    }
}

/* ── Vertical angular prediction (Modes 2..8) ──────────────────
 *
 * Directional prediction projecting onto the above reference row:
 *   Mode 2: angle ≈ 45° NE   (intraRightUp)
 *   Mode 3: angle ≈ 26° NNE  (intraUpRight)
 *   Mode 4: angle ≈ 11° NNW
 *   Mode 5: angle =  0°  N   (vertical)
 *   Mode 6: angle ≈ -11° NWW
 *   Mode 7: angle ≈ -26° NW  (intraUpLeft)
 *   Mode 8: angle ≈ -45° WN  (intraLeftUp)
 *
 * Each pixel (x,y) is projected back along the prediction direction
 * to find the reference position on the above row or left column,
 * then interpolated from the reference samples.
 *
 * Integer math with 1/32 pixel precision.
 * ══════════════════════════════════════════════════════════════ */

/* Displacement per row, in 1/32 pixel units.
 * Positive = projects to above-right, negative = projects to left. */
static const int angular_disp_vert[7] = {
     32,    /* Mode 2:  1.0  px/row (45°)    */
     16,    /* Mode 3:  0.5  px/row (26°)    */
      6,    /* Mode 4:  0.19 px/row (11°)    */
      0,    /* Mode 5:  0    px/row (0° = vertical) */
     -6,    /* Mode 6: -0.19 px/row (-11°)   */
    -16,    /* Mode 7: -0.5  px/row (-26°)   */
    -32,    /* Mode 8: -1.0  px/row (-45°)   */
};

static void intra_angular_vert(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                               const tc_pixel_t *ref_above,
                               const tc_pixel_t *ref_left,
                               int blk_size, int mode_idx)
{
    int disp = angular_disp_vert[mode_idx - 2];  /* Index into displacement table */
    int frac_bits = 5;                            /* 1/32 pixel precision */
    int max_idx = 2 * blk_size;                   /* Max ref sample index */

    for (int y = 0; y < blk_size; y++) {
        for (int x = 0; x < blk_size; x++) {
            /* Project (x,y) along direction to find reference position */
            int ref_pos_x = (x << frac_bits) + y * disp;

            if (disp >= 0 || ref_pos_x >= 0) {
                /* Project onto above row */
                int abs_pos = ref_pos_x >> frac_bits;
                int frac    = ref_pos_x & ((1 << frac_bits) - 1);
                int idx0 = abs_pos + 1;  /* +1 because ref_above[0] = above-left */

                int p0 = ref_above[tc_clip(idx0, 0, max_idx)];
                int p1 = ref_above[tc_clip(idx0 + 1, 0, max_idx)];

                int pred = p0 + (((p1 - p0) * frac + (1 << (frac_bits - 1))) >> frac_bits);
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
            } else {
                /* Project onto left column */
                int abs_pos = (-ref_pos_x) >> frac_bits;
                int frac    = (-ref_pos_x) & ((1 << frac_bits) - 1);
                int idx0 = abs_pos + 1;

                int p0 = ref_left[tc_clip(idx0, 0, max_idx)];
                int p1 = ref_left[tc_clip(idx0 + 1, 0, max_idx)];

                int pred = p0 + (((p1 - p0) * frac + (1 << (frac_bits - 1))) >> frac_bits);
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
            }
        }
    }
}

/* ── Horizontal angular prediction (Modes 9..17) ────────────────
 *
 * Directional prediction projecting onto the left reference column.
 * Mirror of vertical angular modes but with the primary reference
 * on the left column and displacement applied per column.
 *
 *   Mode 9:  angle ≈ 45° EN   (going right projects down-left)
 *   Mode 10: angle ≈ 26° EEN
 *   Mode 11: angle ≈ 11° EE(N)
 *   Mode 12: angle =  0°  E   (horizontal)
 *   Mode 13: angle ≈ -11° WW(N)
 *   Mode 14: angle ≈ -26° WWN
 *   Mode 15: angle ≈ -45° WN  (going right projects up-left)
 *   Mode 16: angle ≈ -56° NNW (steep near-vertical, horiz-biased)
 *   Mode 17: angle ≈  56° NNE (steep near-vertical, horiz-biased)
 *
 * Each pixel (x,y) is projected back along the prediction direction
 * to find the reference position on the left column or above row.
 * Integer math with 1/32 pixel precision.
 * ══════════════════════════════════════════════════════════════ */

/* Displacement per column, in 1/32 pixel units.
 * Positive = going right projects further down the left column.
 * Negative = going right projects further up the left column. */
static const int angular_disp_horiz[9] = {
     32,    /* Mode 9:  1.0  px/col (45° EN)    */
     16,    /* Mode 10: 0.5  px/col (26° EEN)   */
      6,    /* Mode 11: 0.19 px/col (11° EE)    */
      0,    /* Mode 12: 0    px/col (0° = horizontal) */
     -6,    /* Mode 13: -0.19 px/col (-11° WW)  */
    -16,    /* Mode 14: -0.5  px/col (-26° WWN) */
    -32,    /* Mode 15: -1.0  px/col (-45° WN)  */
    -48,    /* Mode 16: -1.5  px/col (-56° NNW) */
     48,    /* Mode 17: 1.5   px/col (56° NNE) */
};

static void intra_angular_horiz(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                                const tc_pixel_t *ref_above,
                                const tc_pixel_t *ref_left,
                                int blk_size, int mode_idx)
{
    int disp = angular_disp_horiz[mode_idx - 9];  /* Index into displacement table */
    int frac_bits = 5;                             /* 1/32 pixel precision */
    int max_idx = 2 * blk_size;                    /* Max ref sample index */

    for (int y = 0; y < blk_size; y++) {
        for (int x = 0; x < blk_size; x++) {
            /* Project (x,y) along direction to find reference position.
             * For horizontal modes, the primary reference is the left column.
             * ref_pos_y = position along the left column (in 1/32 units). */
            int ref_pos_y = (y << frac_bits) + x * disp;

            if (disp >= 0 || ref_pos_y >= 0) {
                /* Project onto left column */
                int abs_pos = ref_pos_y >> frac_bits;
                int frac    = ref_pos_y & ((1 << frac_bits) - 1);
                int idx0 = abs_pos + 1;  /* +1 because ref_left[0] = above-left */

                int p0 = ref_left[tc_clip(idx0, 0, max_idx)];
                int p1 = ref_left[tc_clip(idx0 + 1, 0, max_idx)];

                int pred = p0 + (((p1 - p0) * frac + (1 << (frac_bits - 1))) >> frac_bits);
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
            } else {
                /* Projection goes above the left column — use above row */
                int abs_pos = (-ref_pos_y) >> frac_bits;
                int frac    = (-ref_pos_y) & ((1 << frac_bits) - 1);
                int idx0 = abs_pos + 1;

                int p0 = ref_above[tc_clip(idx0, 0, max_idx)];
                int p1 = ref_above[tc_clip(idx0 + 1, 0, max_idx)];

                int pred = p0 + (((p1 - p0) * frac + (1 << (frac_bits - 1))) >> frac_bits);
                dst[y * dst_stride + x] = (tc_pixel_t)tc_clip(pred, 0, 255);
            }
        }
    }
}

/* ── Top-level intra prediction dispatcher ───────────────────── */

void tc_intra_predict(tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                      const tc_pixel_t *ref_above,
                      const tc_pixel_t *ref_left,
                      int blk_size, tc_intra_mode_t mode)
{
    switch (mode) {
    case TC_INTRA_PLANAR:
        intra_planar(dst, dst_stride, ref_above, ref_left, blk_size);
        break;
    case TC_INTRA_DC:
        intra_dc(dst, dst_stride, ref_above, ref_left, blk_size);
        break;
    default:
        if (mode >= TC_INTRA_ANGULAR_START && mode < TC_INTRA_ANGULAR_VERT_END) {
            /* Vertical angular modes 2..8 */
            intra_angular_vert(dst, dst_stride, ref_above, ref_left, blk_size, mode);
        } else if (mode >= TC_INTRA_ANGULAR_HORIZ_START && mode < TC_INTRA_MAX) {
            /* Horizontal angular modes 9..17 */
            intra_angular_horiz(dst, dst_stride, ref_above, ref_left, blk_size, mode);
        } else {
            /* Fallback to DC for invalid modes */
            intra_dc(dst, dst_stride, ref_above, ref_left, blk_size);
        }
        break;
    }
}

/* ── SAD cost for mode decision ──────────────────────────────── */

tc_sad_t tc_intra_cost(const tc_pixel_t *orig, int orig_stride,
                        const tc_pixel_t *pred, int blk_size)
{
    tc_sad_t sad = 0;
    for (int y = 0; y < blk_size; y++) {
        for (int x = 0; x < blk_size; x++) {
            int diff = (int)orig[y * orig_stride + x] - (int)pred[y * blk_size + x];
            sad += tc_abs(diff);
        }
    }
    return sad;
}

/* ── Chroma intra DC prediction (bitstream v2) ──────────────────
 *
 * Mean of the reconstructed samples directly above and to the left of
 * the chroma block; 128 when neither neighbour exists.  Identical on
 * encoder and decoder (both run on the same reconstruction).
 * ══════════════════════════════════════════════════════════════ */

void tc_intra_chroma_dc(const tc_pixel_t *recon_c, int stride,
                        int cx, int cy, int csize,
                        tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride)
{
    int sum = 0, n = 0;
    if (cy > 0) {
        for (int i = 0; i < csize; i++) { sum += recon_c[(cy - 1) * stride + cx + i]; n++; }
    }
    if (cx > 0) {
        for (int i = 0; i < csize; i++) { sum += recon_c[(cy + i) * stride + cx - 1]; n++; }
    }
    int dc = n ? (sum + n / 2) / n : 128;
    for (int r = 0; r < csize; r++)
        for (int c = 0; c < csize; c++)
            dst[r * dst_stride + c] = (tc_pixel_t)dc;
}

/* ── Reference sample collection, bitstream v2 ──────────────────
 *
 * Same layout as tc_intra_get_ref() (index -1 = above-left corner,
 * 0..2N-1 = reference run, 2N = interpolation guard) but the samples
 * beyond the block (above-right / below-left) are *replicated* rather
 * than read from the frame.
 *
 * Reason: with quadtree coding units the z-order scan makes the
 * above-right region of a CU sometimes belong to a CU that has not
 * been coded yet.  Reading it would make the result depend on stale
 * buffer contents — fine as long as encoder and decoder happen to
 * hold identical stale data, but not something a bitstream should
 * ever rely on.  Replication is deterministic and costs very little
 * (only the 45° NE angular mode loses information).
 * ══════════════════════════════════════════════════════════════ */

void tc_intra_get_ref_v2(const tc_pixel_t *recon, int stride,
                         int x, int y, int blk_size, int frame_w, int frame_h,
                         tc_pixel_t *ref_above, tc_pixel_t *ref_left)
{
    int have_above = (y > 0);
    int have_left  = (x > 0);
    tc_pixel_t tl;

    if (have_above && have_left) tl = recon[(y - 1) * stride + (x - 1)];
    else if (have_above)         tl = recon[(y - 1) * stride + x];
    else if (have_left)          tl = recon[y * stride + (x - 1)];
    else                         tl = 128;

    if (have_above) {
        const tc_pixel_t *ar = recon + (y - 1) * stride;
        for (int i = 0; i < blk_size; i++) {
            int px = x + i;
            ref_above[i] = ar[px < frame_w ? px : frame_w - 1];
        }
    } else {
        tc_pixel_t v = have_left ? recon[y * stride + (x - 1)] : (tc_pixel_t)128;
        for (int i = 0; i < blk_size; i++) ref_above[i] = v;
    }
    for (int i = blk_size; i <= 2 * blk_size; i++)
        ref_above[i] = ref_above[blk_size - 1];

    if (have_left) {
        for (int i = 0; i < blk_size; i++) {
            int py = y + i;
            ref_left[i] = recon[(py < frame_h ? py : frame_h - 1) * stride + (x - 1)];
        }
    } else {
        tc_pixel_t v = have_above ? ref_above[0] : (tc_pixel_t)128;
        for (int i = 0; i < blk_size; i++) ref_left[i] = v;
    }
    for (int i = blk_size; i <= 2 * blk_size; i++)
        ref_left[i] = ref_left[blk_size - 1];

    ref_above[-1] = tl;
    ref_left[-1]  = tl;
}
