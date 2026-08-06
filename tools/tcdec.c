/*
 * tcdec.c — TCodec CLI Decoder
 *
 * Usage: tcdec [options] input.tcv output.yuv
 *
 * Input:  TCodec bitstream (.tcv)
 * Output: Raw planar YCbCr 4:2:0 (or RGB with --rgb flag)
 *
 * Options:
 *   -w width     Override width (0=auto from bitstream)
 *   -h height    Override height (0=auto)
 *   --rgb        Output packed RGB24 instead of YCbCr
 *   -n frames    Decode N frames (0=all)
 *   -v           Verbose: print per-frame stats
 *   --check      Compare with reference YUV and compute PSNR
 */

#include "tcodec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "TCodec Decoder v%s\n"
        "Usage: %s [options] input.tcv output.yuv\n"
        "  -w W      Override width (0=auto)\n"
        "  -h H      Override height (0=auto)\n"
        "  --rgb     Output RGB24\n"
        "  -n N      Decode N frames (0=all)\n"
        "  -v        Verbose\n"
        "  --check   Compute PSNR against reference\n",
        TCODEC_VERSION_STRING, prog);
}


static void tc_write_yuv(FILE *fout,
                         const tc_pixel_t *y, int stride_y,
                         const tc_pixel_t *cb, int stride_cb,
                         const tc_pixel_t *cr, int stride_cr,
                         int w, int h, int is_rgb, uint8_t *rgb_buf)
{
    if (is_rgb) {
        tc_ycbcr_to_rgb(y, stride_y, cb, stride_cb, cr, stride_cr,
                        rgb_buf, w * 3, w, h);
        fwrite(rgb_buf, 1, (size_t)(w * h * 3), fout);
    } else {
        for (int row = 0; row < h; row++)
            fwrite(y + row * stride_y, 1, (size_t)w, fout);
        for (int row = 0; row < h / 2; row++)
            fwrite(cb + row * stride_cb, 1, (size_t)(w / 2), fout);
        for (int row = 0; row < h / 2; row++)
            fwrite(cr + row * stride_cr, 1, (size_t)(w / 2), fout);
    }
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    int width = 0, height = 0;
    int is_rgb = 0, max_frames = 0, verbose = 0, check_psnr = 0;
    const char *ref_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rgb") == 0) {
            is_rgb = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "--check") == 0 && i + 1 < argc) {
            check_psnr = 1;
            ref_path = argv[++i];
        } else if (argv[i][0] != '-') {
            if (!input_path) input_path = argv[i];
            else output_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!input_path || !output_path) {
        print_usage(argv[0]);
        return 1;
    }

    /* Create decoder */
    tc_decoder_t *dec = tc_decoder_create(width, height);
    if (!dec) {
        fprintf(stderr, "Error: failed to create decoder\n");
        return 1;
    }

    /* Open files */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        fprintf(stderr, "Error: cannot open input '%s'\n", input_path);
        tc_decoder_destroy(dec);
        return 1;
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        fprintf(stderr, "Error: cannot open output '%s'\n", output_path);
        fclose(fin);
        tc_decoder_destroy(dec);
        return 1;
    }

    /* Reference file for PSNR checking */
    FILE *fref = NULL;
    if (check_psnr && ref_path) {
        fref = fopen(ref_path, "rb");
        if (!fref) {
            fprintf(stderr, "Warning: cannot open reference '%s'\n", ref_path);
            check_psnr = 0;
        }
    }

    /* Allocate RGB conversion buffer */
    uint8_t *rgb_buf = NULL;
    if (is_rgb) {
        rgb_buf = (uint8_t *)malloc((size_t)(4096 * 4096 * 3));
        if (!rgb_buf) {
            fprintf(stderr, "Error: out of memory\n");
            fclose(fin); fclose(fout);
            tc_decoder_destroy(dec);
            return 1;
        }
    }

    /* Reference Y/Cb/Cr for PSNR checking */
    tc_pixel_t *ref_y = NULL, *ref_cb = NULL, *ref_cr = NULL;
    if (check_psnr) {
        ref_y  = (tc_pixel_t *)malloc((size_t)(4096 * 4096));
        ref_cb = (tc_pixel_t *)malloc((size_t)(2048 * 2048));
        ref_cr = (tc_pixel_t *)malloc((size_t)(2048 * 2048));
    }

    /* Decode loop */
    int frame_count = 0;
    double total_psnr = 0.0;
    clock_t start = clock();

    while (!feof(fin)) {
        if (max_frames > 0 && frame_count >= max_frames) break;

        /* Read packet size */
        uint32_t pkt_size;
        if (fread(&pkt_size, 4, 1, fin) != 1) break;
        if (pkt_size == 0 || pkt_size > 100 * 1024 * 1024) break;

        /* Read packet data */
        uint8_t *pkt_data = (uint8_t *)malloc(pkt_size);
        if (!pkt_data) break;
        if (fread(pkt_data, 1, pkt_size, fin) != pkt_size) {
            free(pkt_data);
            break;
        }

        /* Decode */
        const tc_pixel_t *y, *cb, *cr;
        int stride_y, stride_cb, stride_cr;

        tc_error_t err = tc_decoder_decode(dec, pkt_data, pkt_size,
                                            &y, &stride_y,
                                            &cb, &stride_cb,
                                            &cr, &stride_cr);
        free(pkt_data);

        if (err == TC_ERR_NEED_MORE) {
            /* B-frame reorder: no display frame ready yet — keep
             * feeding packets (output arrives via later calls and
             * the tail drain). */
            continue;
        }
        if (err != TC_OK) {
            fprintf(stderr, "Error: decode failed at frame %d: %s\n",
                    frame_count, tc_error_string(err));
            break;
        }

        /* Get decoded dimensions */
        int32_t dec_w, dec_h;
        tc_decoder_get_info(dec, &dec_w, &dec_h);

        /* Write output */
        tc_write_yuv(fout, y, stride_y, cb, stride_cb, cr, stride_cr,
                     dec_w, dec_h, is_rgb, rgb_buf);

        /* PSNR check */
        if (check_psnr && fref) {
            /* Read reference frame */
            size_t y_size = (size_t)(dec_w * dec_h);
            size_t c_size = (size_t)((dec_w / 2) * (dec_h / 2));
            if (fread(ref_y,  1, y_size,  fref) != y_size ||
                fread(ref_cb, 1, c_size,  fref) != c_size ||
                fread(ref_cr, 1, c_size,  fref) != c_size) {
                check_psnr = 0;  /* Reference exhausted */
            } else {
                double psnr = tc_psnr(ref_y, dec_w, y, stride_y, dec_w, dec_h);
                total_psnr += psnr;
                if (verbose) {
                    fprintf(stderr, "Frame %4d: PSNR = %.2f dB\n",
                            frame_count, psnr);
                }
            }
        }

        if (verbose) {
            fprintf(stderr, "Frame %4d: %dx%d decoded\n",
                    frame_count, dec_w, dec_h);
        }

        frame_count++;
    }

    /* Drain remaining display-order frames still buffered by the
     * B-frame reorder window (D4). */
    int32_t dec_w, dec_h;
    tc_decoder_get_info(dec, &dec_w, &dec_h);
    for (;;) {
        const tc_pixel_t *y, *cb, *cr;
        int stride_y, stride_cb, stride_cr;
        tc_error_t err = tc_decoder_flush_tail(dec, &y, &stride_y,
                                               &cb, &stride_cb,
                                               &cr, &stride_cr);
        if (err != TC_OK) break;
        tc_write_yuv(fout, y, stride_y, cb, stride_cb, cr, stride_cr,
                     dec_w, dec_h, is_rgb, rgb_buf);
        frame_count++;
        if (verbose) fprintf(stderr, "  drain frame %d\n", frame_count);
        if (max_frames > 0 && frame_count >= max_frames) break;
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    double fps_actual = (elapsed > 0) ? (frame_count / elapsed) : 0;

    fprintf(stderr, "\n── Decoding Complete ──\n");
    fprintf(stderr, "  Frames:   %d\n", frame_count);
    fprintf(stderr, "  FPS:      %.1f\n", fps_actual);
    fprintf(stderr, "  Time:     %.2f sec\n", elapsed);
    if (check_psnr && frame_count > 0) {
        fprintf(stderr, "  Avg PSNR: %.2f dB\n", total_psnr / frame_count);
    }

    /* Cleanup */
    fclose(fin);
    fclose(fout);
    if (fref) fclose(fref);
    tc_decoder_destroy(dec);
    free(rgb_buf);
    free(ref_y); free(ref_cb); free(ref_cr);

    return 0;
}
