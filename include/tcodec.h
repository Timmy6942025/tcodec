/*
 * tcodec.h — Public API for TCodec video codec
 *
 * This is the only header users need to include.
 * All encoding/decoding operations are exposed here.
 */

#ifndef TCODEC_H
#define TCODEC_H

#include "tcodec_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Version ─────────────────────────────────────────────────── */

#define TCODEC_VERSION_MAJOR  0
#define TCODEC_VERSION_MINOR  1
#define TCODEC_VERSION_PATCH  0
#define TCODEC_VERSION_INT    ((0 << 16) | (1 << 8) | 0)
#define TCODEC_VERSION_STRING "0.1.0"

/* ── Encoder API ─────────────────────────────────────────────── */

typedef struct tc_encoder tc_encoder_t;

/* Create an encoder with the given configuration.
 * Returns NULL on error. */
tc_encoder_t *tc_encoder_create(const tc_config_t *config);

/* Destroy the encoder and free all resources. */
void tc_encoder_destroy(tc_encoder_t *enc);

/* Encode one frame (planar YCbCr 4:2:0 input).
 * Output packet.data is valid until next encode call or destroy.
 * Returns TC_OK on success. */
tc_error_t tc_encoder_encode(tc_encoder_t *enc,
                              const tc_pixel_t *y,  int stride_y,
                              const tc_pixel_t *cb, int stride_cb,
                              const tc_pixel_t *cr, int stride_cr,
                              tc_packet_t *packet_out);

/* Force the next frame to be a key frame. */
void tc_encoder_force_keyframe(tc_encoder_t *enc);

/* Get encoder statistics. */
void tc_encoder_get_stats(tc_encoder_t *enc,
                           int64_t *total_bytes,
                           int32_t *total_frames,
                           double  *avg_psnr);

/* ── Decoder API ─────────────────────────────────────────────── */

typedef struct tc_decoder tc_decoder_t;

/* Create a decoder. Width/height = 0 means auto-detect from bitstream. */
tc_decoder_t *tc_decoder_create(int32_t width, int32_t height);

/* Destroy the decoder and free all resources. */
void tc_decoder_destroy(tc_decoder_t *dec);

/* Decode one packet into a frame (planar YCbCr 4:2:0 output).
 * Output frame pointers are valid until next decode call or destroy.
 * Returns TC_OK on success. */
tc_error_t tc_decoder_decode(tc_decoder_t *dec,
                              const uint8_t *data, size_t size,
                              const tc_pixel_t **y,  int *stride_y,
                              const tc_pixel_t **cb, int *stride_cb,
                              const tc_pixel_t **cr, int *stride_cr);

/* Get decoded frame dimensions. */
void tc_decoder_get_info(tc_decoder_t *dec,
                          int32_t *width, int32_t *height);

/* Get CRC validation result from last decoded frame.
 * Returns 1 if CRC was OK or no CRC was present, 0 if CRC mismatch.
 * Only meaningful for v1 bitstreams with TC_FLAG_CRC set. */
int tc_decoder_crc_valid(tc_decoder_t *dec);

/* Check if last decoded frame used context-modeled entropy coding.
 * Returns 1 if range coder was active, 0 if Exp-Golomb was used.
 * Only meaningful for v1 bitstreams with TC_TOOL_ENTROPY_CODED set. */
int tc_decoder_entropy_coded(tc_decoder_t *dec);

/* ── Utility functions ───────────────────────────────────────── */

/* Get human-readable error string. */
const char *tc_error_string(tc_error_t err);

/* Get version info. */
int tc_version(void);
const char *tc_version_string(void);

/* Initialize config with default values for given resolution. */
void tc_config_defaults(tc_config_t *cfg, int32_t width, int32_t height);

/* Compute PSNR between two luma planes. */
double tc_psnr(const tc_pixel_t *a, int stride_a,
               const tc_pixel_t *b, int stride_b,
               int width, int height);

/* RGB → YCbCr 4:2:0 conversion (packed RGB → planar YCbCr). */
tc_error_t tc_rgb_to_ycbcr(const uint8_t *rgb, int rgb_stride,
                            tc_pixel_t *y,  int stride_y,
                            tc_pixel_t *cb, int stride_cb,
                            tc_pixel_t *cr, int stride_cr,
                            int width, int height);

/* YCbCr 4:2:0 → RGB conversion (planar YCbCr → packed RGB). */
tc_error_t tc_ycbcr_to_rgb(const tc_pixel_t *y,  int stride_y,
                            const tc_pixel_t *cb, int stride_cb,
                            const tc_pixel_t *cr, int stride_cr,
                            uint8_t *rgb, int rgb_stride,
                            int width, int height);

#ifdef __cplusplus
}
#endif

#endif /* TCODEC_H */
