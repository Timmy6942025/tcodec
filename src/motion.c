/*
 * motion.c — Motion estimation & inter prediction for TCodec
 *
 * Features:
 *  - Hierarchical hexagonal search (3-step: coarse → medium → fine)
 *  - Quarter-pel precision with 6-tap luma interpolation
 *  - Bilinear chroma interpolation (4:2:0 half-pel)
 *  - Early termination based on SAD thresholds
 *  - Median predictor from spatial MV neighbors
 *
 * Luma interpolation: 6-tap filter [1, -5, 20, 20, -5, 1] / 32
 *   — matches H.264/AVC luma half-pel filter quality
 *   — requires 3 extra pixels of border (positions -2..+3)
 *   — for quarter-pel: bilinear blend of two half-pel values
 *
 * Chroma interpolation: bilinear (simple averaging)
 *   — chroma is 4:2:0 so half-pel precision is sufficient
 *
 * The hexagonal search pattern:
 *   Step 1: Large hex (±16 pixels), 7 points
 *   Step 2: Small hex (±4 pixels), 7 points
 *   Step 3: Diamond refinement (±1 pixel), 5 points
 *   Step 4: Sub-pel refinement (quarter-pel around best)
 *
 * This gives excellent rate-distortion with ~40 search points
 * vs. ~200+ for full search, ideal for ARM real-time.
 */

#include "tcodec_common.h"
#include <string.h>

/* ── SAD computation ─────────────────────────────────────────── */

#if TCODEC_NEON  /* NEON version in motion_neon.c replaces this */
extern tc_sad_t tc_sad(const tc_pixel_t *a, int stride_a,
                const tc_pixel_t *b, int stride_b, int n);
#else
tc_sad_t tc_sad(const tc_pixel_t *a, int stride_a,
                const tc_pixel_t *b, int stride_b, int n)
{
    tc_sad_t sad = 0;
    for (int y = 0; y < n; y++) {
        const tc_pixel_t *ra = a + y * stride_a;
        const tc_pixel_t *rb = b + y * stride_b;
        for (int x = 0; x < n; x++) {
            sad += tc_abs((int)ra[x] - (int)rb[x]);
        }
    }
    return sad;
}
#endif /* TCODEC_NEON */

/* ── 6-tap luma interpolation filter ──────────────────────────
 *
 * H.264/AVC-style luma half-pel filter:
 *   Taps: [1, -5, 20, 20, -5, 1] with divisor 32
 *   Input positions: p-2, p-1, p0, p1, p2, p3
 *   Output: filtered value at half-pel position (p0 + p1) / 2
 *
 * Quarter-pel: bilinear blend of two adjacent half-pel values
 *   qpel_pos = (half_a + half_b + 1) / 2
 *
 * This gives significantly sharper motion compensation than
 * plain bilinear, especially for diagonal edges and textures.
 * ══════════════════════════════════════════════════════════════ */

/* 6-tap horizontal half-pel filter: positions at offset -2..+3 from fx */
TCODEC_INLINE int filter6_h(const tc_pixel_t *row, int fx)
{
    /* fx points to the leftmost of the two integer pixels straddling
     * the half-pel position. The 6-tap filter covers fx-2..fx+3. */
    return (1 * (int)row[fx - 2]
          - 5 * (int)row[fx - 1]
          + 20 * (int)row[fx]
          + 20 * (int)row[fx + 1]
          - 5 * (int)row[fx + 2]
          + 1 * (int)row[fx + 3]
          + 16) >> 5;   /* +16 for rounding, >>5 = /32 */
}

/* 6-tap vertical half-pel filter: 6 rows centered at fy,fy+1 */
TCODEC_INLINE int filter6_v(const tc_pixel_t *ref, int stride, int fx, int fy)
{
    return (1 * (int)ref[(fy - 2) * stride + fx]
          - 5 * (int)ref[(fy - 1) * stride + fx]
          + 20 * (int)ref[(fy)     * stride + fx]
          + 20 * (int)ref[(fy + 1) * stride + fx]
          - 5 * (int)ref[(fy + 2) * stride + fx]
          + 1 * (int)ref[(fy + 3) * stride + fx]
          + 16) >> 5;
}

/* 6-tap diagonal half-pel: vertical filter of horizontal half-pel values */
TCODEC_INLINE int filter6_hv(const tc_pixel_t *ref, int stride, int fx, int fy)
{
    /* Compute horizontal half-pel at each of the 6 vertical positions */
    int h0 = filter6_h(ref + (fy - 2) * stride, fx);
    int h1 = filter6_h(ref + (fy - 1) * stride, fx);
    int h2 = filter6_h(ref + (fy)     * stride, fx);
    int h3 = filter6_h(ref + (fy + 1) * stride, fx);
    int h4 = filter6_h(ref + (fy + 2) * stride, fx);
    int h5 = filter6_h(ref + (fy + 3) * stride, fx);
    /* Then vertical 6-tap on those half-pel values */
    return (1 * h0 - 5 * h1 + 20 * h2 + 20 * h3 - 5 * h4 + 1 * h5 + 16) >> 5;
}

/* ── Bilinear interpolation (used for chroma) ──────────────────── */

TCODEC_INLINE tc_pixel_t interp_qpel(
    tc_pixel_t a, tc_pixel_t b, int frac)
{
    /* frac: 0=integer, 1=1/4, 2=1/2, 3=3/4 */
    return (tc_pixel_t)tc_clip((a * (4 - frac) + b * frac + 2) / 4, 0, 255);
}

/* ── Get interpolated luma pixel at quarter-pel position ────────
 *
 * Quarter-pel derivation (matching H.264):
 *   Half-pel positions: 6-tap filter
 *   Quarter-pel positions: bilinear blend of adjacent half/integer positions
 *
 * For horizontal quarter-pel:
 *   dx=0: integer pixel (no filter)
 *   dx=2: half-pel (6-tap horizontal)
 *   dx=1: avg(integer, half-pel)
 *   dx=3: avg(half-pel, next integer)
 *
 * Same logic for vertical. Diagonal = horizontal + vertical.
 * ══════════════════════════════════════════════════════════════ */

static int luma_interp_h(const tc_pixel_t *ref, int stride,
                          int fx, int fy, int dx)
{
    /* Horizontal interpolation at integer vertical position fy.
     * fx is the integer x position (left of the half-pel).
     * dx is the quarter-pel fraction (0,1,2,3). */
    const tc_pixel_t *row = ref + fy * stride;
    switch (dx) {
    case 0:
        return (int)row[fx];
    case 2: {
        /* Half-pel: 6-tap filter */
        int v = filter6_h(row, fx);
        return tc_clip(v, 0, 255);
    }
    case 1: {
        /* 1/4 pel: average of integer and half-pel */
        int a = (int)row[fx];
        int b = filter6_h(row, fx);
        return tc_clip((a + b + 1) / 2, 0, 255);
    }
    case 3: {
        /* 3/4 pel: average of half-pel and next integer */
        int a = filter6_h(row, fx);
        int b = (int)row[fx + 1];
        return tc_clip((a + b + 1) / 2, 0, 255);
    }
    default:
        return (int)row[fx];
    }
}

static int luma_interp_v(const tc_pixel_t *ref, int stride,
                          int fx, int fy, int dy)
{
    /* Vertical interpolation at integer horizontal position fx.
     * fy is the integer y position (above the half-pel).
     * dy is the quarter-pel fraction. */
    switch (dy) {
    case 0:
        return (int)ref[fy * stride + fx];
    case 2: {
        int v = filter6_v(ref, stride, fx, fy);
        return tc_clip(v, 0, 255);
    }
    case 1: {
        int a = (int)ref[fy * stride + fx];
        int b = filter6_v(ref, stride, fx, fy);
        return tc_clip((a + b + 1) / 2, 0, 255);
    }
    case 3: {
        int a = filter6_v(ref, stride, fx, fy);
        int b = (int)ref[(fy + 1) * stride + fx];
        return tc_clip((a + b + 1) / 2, 0, 255);
    }
    default:
        return (int)ref[fy * stride + fx];
    }
}

static int luma_interp_hv(const tc_pixel_t *ref, int stride,
                           int fx, int fy, int dx, int dy)
{
    /* Diagonal quarter-pel: correct H.264 approach using 9 anchor values.
     *
     * We compute horizontally-filtered values at 3 vertical rows
     * (top=fy, mid=fy+0.5, bot=fy+1), then vertically interpolate.
     * This correctly handles ALL 16 (dx,dy) combinations.
     *
     * Anchor values:
     *   a00..a11 = 4 integer corner pixels
     *   b = h-half at row fy,   e = h-half at row fy+1
     *   c = v-half at col fx,   f = v-half at col fx+1
     *   d = hv-half (diagonal) = filter6_hv
     */
    int a00 = ref[fy * stride + fx];
    int a10 = ref[fy * stride + fx + 1];
    int a01 = ref[(fy + 1) * stride + fx];
    int a11 = ref[(fy + 1) * stride + fx + 1];

    int b = filter6_h(ref + fy * stride, fx);           /* h-half at top row */
    int e = filter6_h(ref + (fy + 1) * stride, fx);   /* h-half at bottom row */
    int c = filter6_v(ref, stride, fx, fy);            /* v-half at left col */
    int f = filter6_v(ref, stride, fx + 1, fy);        /* v-half at right col */
    int d = filter6_hv(ref, stride, fx, fy);           /* hv-half diagonal */

    /* Horizontal interpolation at top row (fy) */
    int top;
    switch (dx) {
    case 0:  top = a00; break;
    case 1:  top = (a00 + b + 1) >> 1; break;
    case 2:  top = b; break;
    case 3:  top = (b + a10 + 1) >> 1; break;
    default: top = a00; break;
    }

    /* Horizontal interpolation at half-pel vertical row (fy+0.5) */
    int mid;
    switch (dx) {
    case 0:  mid = c; break;
    case 1:  mid = (c + d + 1) >> 1; break;
    case 2:  mid = d; break;
    case 3:  mid = (d + f + 1) >> 1; break;
    default: mid = c; break;
    }

    /* Horizontal interpolation at bottom row (fy+1) */
    int bot;
    switch (dx) {
    case 0:  bot = a01; break;
    case 1:  bot = (a01 + e + 1) >> 1; break;
    case 2:  bot = e; break;
    case 3:  bot = (e + a11 + 1) >> 1; break;
    default: bot = a01; break;
    }

    /* Vertical interpolation between the three rows */
    int result;
    switch (dy) {
    case 0:  result = top; break;
    case 1:  result = (top + mid + 1) >> 1; break;
    case 2:  result = mid; break;
    case 3:  result = (mid + bot + 1) >> 1; break;
    default: result = top; break;
    }

    return tc_clip(result, 0, 255);
}

/* ── Get interpolated pixel with bounds checking ────────────────
 *
 * The 6-tap filter requires pixels at offsets -2..+3 from the
 * integer position. If these are out of frame bounds, we fall
 * back to bilinear interpolation (which only needs -1..+2).
 * ══════════════════════════════════════════════════════════════ */

static tc_pixel_t get_interp_luma(const tc_pixel_t *ref, int stride,
                                   int ref_w, int ref_h,
                                   int fx, int fy, int dx, int dy)
{
    /* Check if 6-tap filter is safe (need fx-2..fx+3, fy-2..fy+3) */
    int can_6tap = (fx >= 2 && fx + 3 < ref_w &&
                    fy >= 2 && fy + 3 < ref_h);

    if (can_6tap) {
        if (dx == 0 && dy == 0) {
            return ref[fy * stride + fx];
        } else if (dy == 0) {
            return (tc_pixel_t)luma_interp_h(ref, stride, fx, fy, dx);
        } else if (dx == 0) {
            return (tc_pixel_t)luma_interp_v(ref, stride, fx, fy, dy);
        } else {
            return (tc_pixel_t)luma_interp_hv(ref, stride, fx, fy, dx, dy);
        }
    }

    /* Fallback: bilinear interpolation (only needs 1 extra pixel) */
    if (fx >= 0 && fy >= 0 && fx + 1 < ref_w && fy + 1 < ref_h) {
        tc_pixel_t p00 = ref[fy * stride + fx];
        tc_pixel_t p10 = ref[fy * stride + fx + 1];
        tc_pixel_t p01 = ref[(fy + 1) * stride + fx];
        tc_pixel_t p11 = ref[(fy + 1) * stride + fx + 1];
        tc_pixel_t top = interp_qpel(p00, p10, dx);
        tc_pixel_t bot = interp_qpel(p01, p11, dx);
        return interp_qpel(top, bot, dy);
    }

    /* Last resort: nearest pixel */
    int nx = tc_clip(fx + (dx >= 2 ? 1 : 0), 0, ref_w - 1);
    int ny = tc_clip(fy + (dy >= 2 ? 1 : 0), 0, ref_h - 1);
    return ref[ny * stride + nx];
}

/* ── Sub-pel SAD ─────────────────────────────────────────────── */

tc_sad_t tc_sad_subpel(const tc_pixel_t *ref, int ref_stride,
                       int mv_x_qpel, int mv_y_qpel,
                       const tc_pixel_t *orig, int orig_stride, int n)
{
    int fx = mv_x_qpel >> 2;        /* Integer part */
    int fy = mv_y_qpel >> 2;
    int dx = mv_x_qpel & 3;         /* Quarter-pel fraction */
    int dy = mv_y_qpel & 3;

    /* We need ref dimensions for bounds-safe 6-tap filter.
     * For sub-pel SAD during motion estimation, we use the
     * 6-tap filter when possible, bilinear fallback at edges.
     * Since we don't have ref_w/ref_h here, use a simpler approach:
     * just do bilinear for sub-pel SAD (the quality difference
     * in SAD comparison is negligible). */
    tc_sad_t sad = 0;
    for (int y = 0; y < n; y++) {
        const tc_pixel_t *o = orig + y * orig_stride;
        for (int x = 0; x < n; x++) {
            int px = fx + x;
            int py = fy + y;
            /* Bilinear for SAD — sufficient for mode decision */
            tc_pixel_t p00 = ref[py * ref_stride + px];
            tc_pixel_t p10 = ref[py * ref_stride + px + 1];
            tc_pixel_t p01 = ref[(py + 1) * ref_stride + px];
            tc_pixel_t p11 = ref[(py + 1) * ref_stride + px + 1];
            tc_pixel_t top = interp_qpel(p00, p10, dx);
            tc_pixel_t bot = interp_qpel(p01, p11, dx);
            tc_pixel_t pred = interp_qpel(top, bot, dy);
            sad += tc_abs((int)o[x] - (int)pred);
        }
    }
    return sad;
}

/* ── Hexagonal search pattern ────────────────────────────────── */

typedef struct { int dx; int dy; } search_point_t;

static const search_point_t hex_large[] = {
    {0,0}, {-2,-4}, {2,-4}, {-4,0}, {4,0}, {-2,4}, {2,4}
};
#define HEX_LARGE_COUNT 7

static const search_point_t hex_small[] = {
    {0,0}, {-1,-2}, {1,-2}, {-2,0}, {2,0}, {-1,2}, {1,2}
};
#define HEX_SMALL_COUNT 7

static const search_point_t diamond[] = {
    {0,0}, {0,-1}, {-1,0}, {1,0}, {0,1}
};
#define DIAMOND_COUNT 5

/* Quarter-pel refinement offsets */
static const search_point_t qpel_refine[] = {
    {0,0}, {1,0}, {-1,0}, {0,1}, {0,-1},
    {1,1}, {-1,1}, {1,-1}, {-1,-1}
};
#define QPEL_COUNT 9

/* ── SAD with bounds checking ────────────────────────────────── */

static tc_sad_t sad_at(const tc_pixel_t *ref, int ref_stride, int ref_w, int ref_h,
                       const tc_pixel_t *orig, int orig_stride, int blk_size,
                       int mx, int my)
{
    if (mx < 0 || my < 0 || mx + blk_size > ref_w || my + blk_size > ref_h)
        return 0x7FFFFFFF;

    return tc_sad(ref + my * ref_stride + mx, ref_stride,
                  orig, orig_stride, blk_size);
}

/* ── Hierarchical hexagonal motion estimation ────────────────── */

tc_mv_s tc_motion_est(const tc_pixel_t *ref, int ref_stride,
                      int ref_w, int ref_h,
                      const tc_pixel_t *orig, int orig_stride,
                      int center_x, int center_y,
                      int blk_size, int search_range,
                      tc_sad_t *best_sad_out)
{
    center_x = tc_clip(center_x, 0, ref_w - blk_size);
    center_y = tc_clip(center_y, 0, ref_h - blk_size);

    tc_mv_s best_mv = {(tc_mv_t)(center_x * 4), (tc_mv_t)(center_y * 4)};
    tc_sad_t best_sad = sad_at(ref, ref_stride, ref_w, ref_h,
                                orig, orig_stride, blk_size, center_x, center_y);

    int step1_scale = tc_max(1, search_range / 4);
    for (int iter = 0; iter < 2; iter++) {
        int improved = 0;
        for (int p = 1; p < HEX_LARGE_COUNT; p++) {
            int mx = best_mv.x / 4 + hex_large[p].dx * step1_scale;
            int my = best_mv.y / 4 + hex_large[p].dy * step1_scale;
            tc_sad_t s = sad_at(ref, ref_stride, ref_w, ref_h,
                                orig, orig_stride, blk_size, mx, my);
            if (s < best_sad) {
                best_sad = s;
                best_mv.x = mx * 4;
                best_mv.y = my * 4;
                improved = 1;
            }
        }
        if (!improved) break;
    }

    for (int iter = 0; iter < 3; iter++) {
        int improved = 0;
        for (int p = 1; p < HEX_SMALL_COUNT; p++) {
            int mx = best_mv.x / 4 + hex_small[p].dx;
            int my = best_mv.y / 4 + hex_small[p].dy;
            tc_sad_t s = sad_at(ref, ref_stride, ref_w, ref_h,
                                orig, orig_stride, blk_size, mx, my);
            if (s < best_sad) {
                best_sad = s;
                best_mv.x = mx * 4;
                best_mv.y = my * 4;
                improved = 1;
            }
        }
        if (!improved) break;
    }

    for (int iter = 0; iter < 4; iter++) {
        int improved = 0;
        for (int p = 1; p < DIAMOND_COUNT; p++) {
            int mx = best_mv.x / 4 + diamond[p].dx;
            int my = best_mv.y / 4 + diamond[p].dy;
            tc_sad_t s = sad_at(ref, ref_stride, ref_w, ref_h,
                                orig, orig_stride, blk_size, mx, my);
            if (s < best_sad) {
                best_sad = s;
                best_mv.x = mx * 4;
                best_mv.y = my * 4;
                improved = 1;
            }
        }
        if (!improved) break;
    }

    for (int p = 1; p < QPEL_COUNT; p++) {
        int qx = best_mv.x + qpel_refine[p].dx;
        int qy = best_mv.y + qpel_refine[p].dy;
        /* Bounds check: sub-pel SAD needs fx..fx+blk_size+1 and fy..fy+blk_size+1
         * to be valid for bilinear interpolation. Skip out-of-bounds candidates. */
        int fx = qx >> 2;
        int fy = qy >> 2;
        if (fx < 0 || fy < 0 || fx + blk_size + 1 > ref_w || fy + blk_size + 1 > ref_h)
            continue;
        tc_sad_t s = tc_sad_subpel(ref, ref_stride, qx, qy,
                                    orig, orig_stride, blk_size);
        if (s < best_sad) {
            best_sad = s;
            best_mv.x = qx;
            best_mv.y = qy;
        }
    }

    if (best_sad_out) *best_sad_out = best_sad;
    return best_mv;
}

/* ── Inter prediction (motion compensation) ────────────────────
 *
 * Luma: 6-tap filter with bilinear fallback at frame edges
 * Chroma: bilinear interpolation (called separately from decoder)
 * ══════════════════════════════════════════════════════════════ */

void tc_inter_predict(const tc_pixel_t *ref, int ref_stride,
                      int ref_w, int ref_h,
                      tc_mv_s mv,
                      tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                      int blk_size)
{
    int fx = mv.x >> 2;           /* Integer part */
    int fy = mv.y >> 2;
    int dx = mv.x & 3;            /* Quarter-pel fraction */
    int dy = mv.y & 3;

    if (dx == 0 && dy == 0) {
        /* Integer-pel: fast path, just copy — but only when the whole
         * block is inside the frame. Otherwise fall through to the
         * per-pixel safe path (get_interp_luma clamps to frame edges),
         * keeping encoder recon and decoder output bit-identical for
         * edge MVs (and OOB MVs from fuzzed bitstreams never read
         * out of bounds). */
        if (fx >= 0 && fy >= 0 &&
            fx + blk_size <= ref_w && fy + blk_size <= ref_h) {
            for (int y = 0; y < blk_size; y++) {
                memcpy(dst + y * dst_stride,
                       ref + (fy + y) * ref_stride + fx,
                       (size_t)blk_size);
            }
            return;
        }
        /* Fall through: safe per-pixel path for out-of-bounds MVs */
        for (int y = 0; y < blk_size; y++)
            for (int x = 0; x < blk_size; x++)
                dst[y * dst_stride + x] =
                    get_interp_luma(ref, ref_stride, ref_w, ref_h,
                                    fx + x, fy + y, 0, 0);
        return;
    }

    /* Sub-pel interpolation.
     *
     * Optimization: check if the entire block can use the 6-tap filter.
     * The 6-tap filter needs fx-2..fx+blk_size+3 and fy-2..fy+blk_size+3
     * to be valid. If so, we skip per-pixel bounds checking.
     * This is the common case — most MVs point well inside the frame. */
    /* The 6-tap filter at position (fx+x, fy+y) needs pixels at
     * (fx+x-2..fx+x+3, fy+y-2..fy+y+3). The maximum x is fx+blk_size-1,
     * so the rightmost needed pixel is fx+blk_size-1+3 = fx+blk_size+2.
     * This must be < ref_w (a valid index). Same for y. */
    int can_6tap_full = (fx >= 2 && fx + blk_size + 2 < ref_w &&
                         fy >= 2 && fy + blk_size + 2 < ref_h);

    if (can_6tap_full) {
        /* Fast path: entire block is 6-tap safe — no per-pixel checks */
        if (dy == 0) {
            for (int y = 0; y < blk_size; y++)
                for (int x = 0; x < blk_size; x++)
                    dst[y * dst_stride + x] =
                        (tc_pixel_t)luma_interp_h(ref, ref_stride, fx + x, fy + y, dx);
        } else if (dx == 0) {
            for (int y = 0; y < blk_size; y++)
                for (int x = 0; x < blk_size; x++)
                    dst[y * dst_stride + x] =
                        (tc_pixel_t)luma_interp_v(ref, ref_stride, fx + x, fy + y, dy);
        } else {
            for (int y = 0; y < blk_size; y++)
                for (int x = 0; x < blk_size; x++)
                    dst[y * dst_stride + x] =
                        (tc_pixel_t)luma_interp_hv(ref, ref_stride, fx + x, fy + y, dx, dy);
        }
    } else {
        /* Slow path: some pixels may be near frame edges — per-pixel bounds
         * checking via get_interp_luma (6-tap → bilinear → nearest fallback). */
        for (int y = 0; y < blk_size; y++) {
            for (int x = 0; x < blk_size; x++) {
                dst[y * dst_stride + x] =
                    get_interp_luma(ref, ref_stride, ref_w, ref_h,
                                    fx + x, fy + y, dx, dy);
            }
        }
    }
}

/* ── Chroma inter prediction (bitstream v2) ────────────────────
 *
 * 4:2:0 chroma MC at 1/8-pel precision.  The luma MV is an absolute
 * quarter-luma-pel position; the chroma sample grid is half the luma
 * resolution, so the same integer is an absolute 1/8-chroma-pel
 * position:  int = mv >> 3, frac = mv & 7.
 *
 * Bilinear, integer-only, edge-clamped:
 *   p = ((8-fx)(8-fy)A + fx(8-fy)B + (8-fx)fy·C + fx·fy·D + 32) >> 6
 * ══════════════════════════════════════════════════════════════ */

void tc_inter_predict_chroma(const tc_pixel_t *ref, int ref_stride,
                             int ref_w, int ref_h,
                             tc_mv_s mv,
                             tc_pixel_t *TCODEC_RESTRICT dst, int dst_stride,
                             int blk_size)
{
    int xi = mv.x >> 3, yi = mv.y >> 3;
    int fx = mv.x & 7,  fy = mv.y & 7;

    if (fx == 0 && fy == 0 &&
        xi >= 0 && yi >= 0 &&
        xi + blk_size <= ref_w && yi + blk_size <= ref_h) {
        for (int r = 0; r < blk_size; r++)
            memcpy(dst + r * dst_stride, ref + (yi + r) * ref_stride + xi,
                   (size_t)blk_size);
        return;
    }

    for (int r = 0; r < blk_size; r++) {
        int y0 = tc_clip(yi + r,     0, ref_h - 1);
        int y1 = tc_clip(yi + r + 1, 0, ref_h - 1);
        const tc_pixel_t *row0 = ref + y0 * ref_stride;
        const tc_pixel_t *row1 = ref + y1 * ref_stride;
        for (int c = 0; c < blk_size; c++) {
            int x0 = tc_clip(xi + c,     0, ref_w - 1);
            int x1 = tc_clip(xi + c + 1, 0, ref_w - 1);
            int A = row0[x0], B = row0[x1];
            int C = row1[x0], D = row1[x1];
            int v = ((8 - fx) * (8 - fy) * A + fx * (8 - fy) * B +
                     (8 - fx) * fy * C + fx * fy * D + 32) >> 6;
            dst[r * dst_stride + c] = (tc_pixel_t)v;
        }
    }
}
