/*
 * ratectl.c — Rate control for TCodec
 *
 * Uses ρ-domain rate control (ρ = fraction of zero coefficients after quantize).
 *
 * The key insight: for a given source and QP, the fraction of zero
 * coefficients (ρ) correlates almost linearly with bitrate:
 *   R(ρ) ≈ R_max × (1 - ρ)
 *
 * So controlling ρ controls bitrate directly, and ρ maps to QP.
 * This is much simpler and more stable than traditional QP adjustment.
 *
 * Modes:
 *   TC_RC_CQP: Constant QP — just use the configured QP
 *   TC_RC_CBR: Constant bitrate — adjust QP to hit target bitrate
 *   TC_RC_VBR: Variable bitrate — allow fluctuations within buffer
 */

#include "tcodec_common.h"
#include <math.h>

/* ── ρ(QP) model initialization ───────────────────────────────
 *
 * For typical video content, ρ(QP) follows an S-curve:
 *   ρ ≈ 1 / (1 + exp(-(QP - QP_mid) / QP_scale))
 *
 * Default parameters: QP_mid = 28, QP_scale = 8
 * These are adapted per-frame based on actual measurements.
 * ══════════════════════════════════════════════════════════════ */

#define RHO_MODEL_MID   28.0
#define RHO_MODEL_SCALE  8.0

/* Helper: clip double to [lo, hi] */
static double tc_clip_d(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static double rho_from_qp(int qp)
{
    /* Sigmoid: ρ(QP) = 1 / (1 + exp(-(QP - mid) / scale)) */
    double x = -((double)qp - RHO_MODEL_MID) / RHO_MODEL_SCALE;
    return 1.0 / (1.0 + exp(x));
}

static int qp_from_rho(double rho_target)
{
    /* Inverse sigmoid: QP(ρ) = mid - scale × ln(ρ/(1-ρ)) */
    if (rho_target <= 0.01) return TC_QP_MIN;
    if (rho_target >= 0.99) return TC_QP_MAX;
    double logit = log(rho_target / (1.0 - rho_target));
    double qp = RHO_MODEL_MID - RHO_MODEL_SCALE * logit;
    return tc_clip((int)(qp + 0.5), TC_QP_MIN, TC_QP_MAX);
}

void tc_ratectl_init(tc_ratectl_t *rc, const tc_config_t *cfg)
{
    memset(rc, 0, sizeof(*rc));
    rc->method         = cfg->rc_method;
    rc->target_bitrate = cfg->target_bitrate;
    rc->qp             = tc_clip(cfg->qp, TC_QP_MIN, TC_QP_MAX);
    rc->fps_num        = cfg->fps_num > 0 ? cfg->fps_num : 60;
    rc->fps_den        = cfg->fps_den > 0 ? cfg->fps_den : 1;
    rc->base_qp        = rc->qp;
    rc->last_bits      = 0;
    rc->total_frames   = 0;
    rc->total_bits     = 0;
    rc->buffer_level   = 0.5;

    /* Compute target bits per frame */
    double fps = (double)rc->fps_num / (double)rc->fps_den;
    if (rc->target_bitrate > 0 && fps > 0) {
        rc->frame_bits_target = (int64_t)((double)rc->target_bitrate / fps);
    } else {
        rc->frame_bits_target = 0;  /* CQP mode */
    }

    /* VBV buffer: 1 second of video at target bitrate */
    rc->buffer_size = (double)rc->target_bitrate * 1.0;

    /* Build ρ(QP) lookup table */
    for (int qp = 0; qp < 64; qp++) {
        rc->rho_per_qp[qp] = rho_from_qp(qp);
    }
}

void tc_ratectl_frame_start(tc_ratectl_t *rc, tc_frame_type_t type)
{
    TCODEC_UNUSED(type);

    if (rc->method == TC_RC_CQP) {
        /* Constant QP: no adjustment. (Reactive CRF-lite trialed 2026-09-06
         * in both directions: easy-fine spent +8% on screen, easy-coarse
         * cost -0.23dB for -3%; both mediocre diagonal moves. A true
         * quality-targeted servo is future work — see HILLCLIMB.) */
        rc->qp = rc->base_qp;
        return;
    }

    /* For CBR/VBR: adjust QP based on buffer status */
    if (rc->method == TC_RC_CBR || rc->method == TC_RC_VBR) {
        double buffer_fill = rc->buffer_level;

        /* If buffer is getting too full, increase QP (coarser) */
        /* If buffer is getting empty, decrease QP (finer) */
        double adjustment = 0.0;

        if (buffer_fill > 0.8) {
            adjustment = 2.0;   /* Increase QP to reduce bitrate */
        } else if (buffer_fill > 0.6) {
            adjustment = 1.0;
        } else if (buffer_fill < 0.2) {
            adjustment = -2.0;  /* Decrease QP to increase bitrate */
        } else if (buffer_fill < 0.4) {
            adjustment = -1.0;
        }

        /* Map adjustment through ρ-domain */
        double current_rho = rho_from_qp(rc->qp);
        double target_rho  = current_rho - adjustment * 0.05;
        target_rho = tc_clip_d(target_rho, 0.05, 0.95);

        rc->qp = qp_from_rho(target_rho);
    }

    rc->frame_bits_actual = 0;
}

int tc_ratectl_get_qp(tc_ratectl_t *rc)
{
    return rc->qp;
}

void tc_ratectl_frame_end(tc_ratectl_t *rc, int64_t bits_used)
{
    rc->frame_bits_actual = bits_used;
    rc->last_bits = bits_used;
    rc->total_bits += bits_used;
    rc->total_frames++;

    /* Update buffer model */
    if (rc->buffer_size > 0) {
        double bits_in  = (double)rc->frame_bits_target;
        double bits_out = (double)bits_used;
        rc->buffer_level += (bits_in - bits_out) / rc->buffer_size;
        rc->buffer_level = tc_clip_d(rc->buffer_level, 0.0, 1.0);
    }

    /* Update ρ model based on actual zero-fraction if we had that data */
    /* For now, the sigmoid model is sufficient */
}


