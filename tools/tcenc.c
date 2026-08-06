/*
 * tcenc.c — TCodec CLI Encoder
 *
 * Usage: tcenc [options] -o output.tcv input.yuv
 *
 * Input:  Raw planar YCbCr 4:2:0 (or RGB with --rgb flag)
 * Output: TCodec bitstream (.tcv)
 *
 * Options:
 *   -w width     Frame width (required)
 *   -h height    Frame height (required)
 *   -f fps       Frame rate (default 30)
 *   -q qp        Quantization parameter 0-63 (default 32)
 *   -r bitrate   Target bitrate in kbps (CBR mode)
 *   -p preset    Preset: 0=ultrafast, 1=fast, 2=medium, 3=slow
 *   -k interval  Keyframe interval (default 30)
 *   -t threads   Number of threads (default 4)
 *   --rgb        Input is packed RGB24 instead of YCbCr
 *   -n frames    Number of frames to encode (0=all)
 *   -v           Verbose: print per-frame stats
 *   --bs-version V Bitstream version: 0 or 1 (default 1)
 *   --profile P  Profile: 0=baseline-mobile 1=streaming-main 2=archive-high 3=grain-cinema
 *   --level L     Level index: 0=auto 1..8 (default 0)
 *   --crc         Enable CRC-16 error detection (v1 only)
 */

#include "tcodec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
        "TCodec Encoder v%s\n"
        "Usage: %s [options] -o output.tcv input.yuv\n"
        "  -w W      Width (required)\n"
        "  -h H      Height (required)\n"
        "  -f FPS    Frame rate (default 30)\n"
        "  -q QP     Quantization parameter 0-63 (default 32)\n"
        "  -r KBPS   Target bitrate in kbps (enables CBR)\n"
        "  -p PRESET 0=ultrafast 1=fast 2=medium 3=slow (default 2)\n"
        "  -k KF     Keyframe interval (default 30)\n"
        "  -t THR    Threads (default 4)\n"
        "  --rgb     Input is RGB24\n"
        "  -n N      Encode N frames (0=all)\n"
        "  -v        Verbose\n"
        "  --bs-version V Bitstream version 0 or 1 (default 1)\n"
        "  --profile P     Profile 0-3 (default 0=baseline-mobile)\n"
        "  --level L       Level 0-8 (default 0=auto)\n"
        "  --crc           Enable CRC-16 (v1 only)\n"        "  --entropy       Use context-modeled range coder (v1 only)\n",
        TCODEC_VERSION_STRING, prog);
}

int main(int argc, char **argv)
{
    const char *input_path = NULL;
    const char *output_path = NULL;
    int width = 0, height = 0;
    int fps = 30, qp = 32, preset = 2;
    int keyframe_interval = 30, threads = 4;
    int target_bitrate_kbps = 0;
    int is_rgb = 0, max_frames = 0, verbose = 0;
    int bitstream_version = -1;  /* -1 = use default from tc_config_defaults */
    int profile = -1;
    int level = -1;
    int enable_crc = 0;
    int enable_entropy = 0;

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-w") == 0 && i + 1 < argc) {
            width = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            height = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            fps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-q") == 0 && i + 1 < argc) {
            qp = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
            target_bitrate_kbps = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            preset = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-k") == 0 && i + 1 < argc) {
            keyframe_interval = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--rgb") == 0) {
            is_rgb = 1;
        } else if (strcmp(argv[i], "--bs-version") == 0 && i + 1 < argc) {
            bitstream_version = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--profile") == 0 && i + 1 < argc) {
            profile = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--level") == 0 && i + 1 < argc) {
            level = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--crc") == 0) {
            enable_crc = 1;
        } else if (strcmp(argv[i], "--entropy") == 0) {
            enable_entropy = 1;
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (argv[i][0] != '-') {
            input_path = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!input_path || !output_path || width <= 0 || height <= 0) {
        print_usage(argv[0]);
        return 1;
    }

    /* Configure encoder using defaults (includes v1 bitstream fields) */
    tc_config_t cfg;
    tc_config_defaults(&cfg, width, height);
    cfg.preset = (tc_preset_t)preset;
    cfg.qp = qp;
    cfg.fps_num = fps;
    cfg.fps_den = 1;
    cfg.keyframe_interval = keyframe_interval;
    cfg.threads = threads;

    /* Override v1 bitstream options if specified */
    if (bitstream_version >= 0) {
        if (bitstream_version > TC_VERSION_V1) {
            fprintf(stderr, "Error: bitstream version must be 0 or 1\n");
            return 1;
        }
        cfg.bitstream_version = (uint8_t)bitstream_version;
    }
    if (profile >= 0) {
        if (profile > TC_PROFILE_MAX) {
            fprintf(stderr, "Error: profile must be 0-%d\n", TC_PROFILE_MAX);
            return 1;
        }
        cfg.profile = (uint8_t)profile;
    }
    if (level >= 0) {
        if (level > TC_LEVEL_MAX) {
            fprintf(stderr, "Error: level must be 0-%d\n", TC_LEVEL_MAX);
            return 1;
        }
        cfg.level_idx = (uint8_t)level;
    }
    if (enable_crc) {
        if (cfg.bitstream_version == TC_VERSION_V0) {
            fprintf(stderr, "Warning: --crc has no effect with v0 bitstream (CRC is v1-only)\n");
        }
        cfg.enable_crc = 1;
    }
    if (enable_entropy) {
        if (cfg.bitstream_version == TC_VERSION_V0) {
            fprintf(stderr, "Warning: --entropy has no effect with v0 bitstream (RC is v1-only)\n");
        }
        cfg.enable_entropy_coded = 1;
    }

    if (target_bitrate_kbps > 0) {
        cfg.rc_method = TC_RC_CBR;
        cfg.target_bitrate = target_bitrate_kbps * 1000;
    } else {
        cfg.rc_method = TC_RC_CQP;
    }

    /* Create encoder */
    tc_encoder_t *enc = tc_encoder_create(&cfg);
    if (!enc) {
        fprintf(stderr, "Error: failed to create encoder\n");
        return 1;
    }

    /* Open files */
    FILE *fin = fopen(input_path, "rb");
    if (!fin) {
        fprintf(stderr, "Error: cannot open input '%s'\n", input_path);
        tc_encoder_destroy(enc);
        return 1;
    }

    FILE *fout = fopen(output_path, "wb");
    if (!fout) {
        fprintf(stderr, "Error: cannot open output '%s'\n", output_path);
        fclose(fin);
        tc_encoder_destroy(enc);
        return 1;
    }

    /* Allocate frame buffers */
    int y_size = width * height;
    int c_size = (width / 2) * (height / 2);
    int frame_size_yuv = y_size + 2 * c_size;
    int frame_size_rgb = width * height * 3;

    tc_pixel_t *y_buf  = (tc_pixel_t *)malloc((size_t)y_size);
    tc_pixel_t *cb_buf = (tc_pixel_t *)malloc((size_t)c_size);
    tc_pixel_t *cr_buf = (tc_pixel_t *)malloc((size_t)c_size);
    uint8_t    *rgb_buf = is_rgb ? (uint8_t *)malloc((size_t)frame_size_rgb) : NULL;

    if (!y_buf || !cb_buf || !cr_buf || (is_rgb && !rgb_buf)) {
        fprintf(stderr, "Error: out of memory\n");
        fclose(fin); fclose(fout);
        tc_encoder_destroy(enc);
        free(y_buf); free(cb_buf); free(cr_buf); free(rgb_buf);
        return 1;
    }

    /* Temp RGB→YCbCr conversion frame (if needed) */
    tc_pixel_t *conv_y  = NULL;
    tc_pixel_t *conv_cb = NULL;
    tc_pixel_t *conv_cr = NULL;
    int conv_stride_y = width;
    int conv_stride_c = width / 2;
    if (is_rgb) {
        conv_y  = (tc_pixel_t *)malloc((size_t)(width * height));
        conv_cb = (tc_pixel_t *)malloc((size_t)((width / 2) * (height / 2)));
        conv_cr = (tc_pixel_t *)malloc((size_t)((width / 2) * (height / 2)));
        if (!conv_y || !conv_cb || !conv_cr) {
            fprintf(stderr, "Error: out of memory for conversion frame\n");
            fclose(fin); fclose(fout);
            tc_encoder_destroy(enc);
            free(y_buf); free(cb_buf); free(cr_buf); free(rgb_buf);
            free(conv_y); free(conv_cb); free(conv_cr);
            return 1;
        }
    }

    /* Encode loop */
    int frame_count = 0;
    clock_t start = clock();

    while (!feof(fin)) {
        if (max_frames > 0 && frame_count >= max_frames) break;

        if (is_rgb) {
            /* Read RGB and convert */
            size_t read = fread(rgb_buf, 1, (size_t)frame_size_rgb, fin);
            if (read < (size_t)frame_size_rgb) break;

            tc_rgb_to_ycbcr(rgb_buf, width * 3,
                            conv_y, conv_stride_y,
                            conv_cb, conv_stride_c,
                            conv_cr, conv_stride_c,
                            width, height);

            y_buf  = conv_y;
            cb_buf = conv_cb;
            cr_buf = conv_cr;
        } else {
            /* Read planar YCbCr 4:2:0 */
            size_t read = 0;
            read += fread(y_buf,  1, (size_t)y_size,  fin);
            read += fread(cb_buf, 1, (size_t)c_size,  fin);
            read += fread(cr_buf, 1, (size_t)c_size,  fin);
            if (read < (size_t)frame_size_yuv) break;
        }

        tc_packet_t pkt;
        tc_error_t err = tc_encoder_encode(enc,
                                            y_buf, width,
                                            cb_buf, width / 2,
                                            cr_buf, width / 2,
                                            &pkt);
        if (err != TC_OK) {
            fprintf(stderr, "Error: encode failed at frame %d: %s\n",
                    frame_count, tc_error_string(err));
            break;
        }

        /* Write packet */
        uint32_t pkt_size = (uint32_t)pkt.size;
        fwrite(&pkt_size, 4, 1, fout);
        fwrite(pkt.data, 1, pkt.size, fout);

        if (verbose) {
            fprintf(stderr, "Frame %4d: %6zu bytes, %s, QP=%d\n",
                    frame_count, pkt.size,
                    pkt.key_frame ? "KEY" : "inter",
                    cfg.qp);
        }

        frame_count++;
    }

    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;

    /* Print summary */
    int64_t total_bytes;
    int32_t total_frames;
    double avg_psnr;
    tc_encoder_get_stats(enc, &total_bytes, &total_frames, &avg_psnr);

    double bitrate = (elapsed > 0) ? (total_bytes * 8.0 / elapsed) : 0;
    double fps_actual = (elapsed > 0) ? (total_frames / elapsed) : 0;

    fprintf(stderr, "\n── Encoding Complete ──\n");
    fprintf(stderr, "  Frames:    %d\n", total_frames);
    fprintf(stderr, "  Size:      %.2f KB\n", total_bytes / 1024.0);
    fprintf(stderr, "  Bitrate:   %.1f kbps\n", bitrate / 1000.0);
    fprintf(stderr, "  FPS:       %.1f\n", fps_actual);
    fprintf(stderr, "  Avg PSNR:  %.2f dB\n", avg_psnr);
    fprintf(stderr, "  Time:      %.2f sec\n", elapsed);
    fprintf(stderr, "  Compression: %.1f:1\n",
            (double)frame_size_yuv * total_frames / (double)total_bytes);

    /* Cleanup */
    fclose(fin);
    fclose(fout);
    tc_encoder_destroy(enc);
    free(rgb_buf);
    free(conv_y); free(conv_cb); free(conv_cr);
    free(y_buf); free(cb_buf); free(cr_buf);

    return 0;
}
