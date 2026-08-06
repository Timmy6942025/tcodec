/*
 * tcodec.c — Public API entry points for TCodec
 *
 * Implements all functions declared in tcodec.h.
 * This is the only file that users need to link against.
 * All internal modules are compiled separately and linked together.
 */

#include "tcodec.h"
#include "tcodec_common.h"
#include <stdio.h>

/* ── Error strings ────────────────────────────────────────────── */

const char *tc_error_string(tc_error_t err)
{
    switch (err) {
        case TC_OK:            return "OK";
        case TC_ERR_MEMORY:    return "Out of memory";
        case TC_ERR_PARAM:     return "Invalid parameter";
        case TC_ERR_BITSTREAM: return "Bitstream error";
        case TC_ERR_EOF:       return "End of bitstream";
        case TC_ERR_INTERNAL:  return "Internal error";
        default:               return "Unknown error";
    }
}

/* ── PSNR computation ───────────────────────────────────────────
 *
 * PSNR = 10 × log10(255² / MSE)
 * MSE = (1/N) × Σ(a[i] - b[i])²
 *
 * Returns 0.0 if frames are identical (MSE=0).
 * ══════════════════════════════════════════════════════════════ */

double tc_psnr(const tc_pixel_t *a, int stride_a,
               const tc_pixel_t *b, int stride_b,
               int width, int height)
{
    int64_t sse = 0;
    for (int y = 0; y < height; y++) {
        const tc_pixel_t *ra = a + y * stride_a;
        const tc_pixel_t *rb = b + y * stride_b;
        for (int x = 0; x < width; x++) {
            int diff = (int)ra[x] - (int)rb[x];
            sse += diff * diff;
        }
    }

    if (sse == 0) return 100.0;  /* Perfect match */

    double mse = (double)sse / ((double)width * (double)height);
    return 10.0 * log10(255.0 * 255.0 / mse);
}

/* ── Color conversion public API ───────────────────────────────
 *
 * Implemented in color.c (which also provides the _internal variants
 * used by the encoder/decoder). NEON variants are in neon/color_neon.c.
 * ══════════════════════════════════════════════════════════════ */

/* ── Version info ──────────────────────────────────────────────── */

int tc_version(void)
{
    return TCODEC_VERSION_INT;
}

const char *tc_version_string(void)
{
    return TCODEC_VERSION_STRING;
}

/* ── Default configuration helper ─────────────────────────────── */

void tc_config_defaults(tc_config_t *cfg, int width, int height)
{
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->width       = width;
    cfg->height      = height;
    cfg->preset      = TC_PRESET_MEDIUM;
    cfg->rc_method   = TC_RC_CQP;
    cfg->qp          = TC_QP_DEFAULT;
    cfg->target_bitrate = 0;
    cfg->fps_num     = 60;
    cfg->fps_den     = 1;
    cfg->keyframe_interval = 30;
    cfg->threads     = 0;   /* Auto-detect */
    cfg->tile_cols   = 0;   /* Auto */
    cfg->tile_rows   = 0;   /* Auto */
    /* v1 bitstream fields */
    cfg->bitstream_version = TC_VERSION_V1;  /* Default: v1 */
    cfg->profile    = TC_PROFILE_BASELINE_MOBILE;
    cfg->level_idx  = TC_LEVEL_AUTO;         /* Auto */
    cfg->enable_crc = 0;                     /* CRC off by default */
    cfg->enable_entropy_coded = 0;           /* Range coder off by default */
}
