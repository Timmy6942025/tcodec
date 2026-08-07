/*
 * frame.c — Frame buffer management for TCodec
 *
 * Stride is always aligned to 64 bytes for NEON cache-line efficiency.
 */

#include "tcodec_common.h"
#include <stdlib.h>
#include <string.h>

/* Align stride up to next multiple of alignment */
static int align_stride(int width, int alignment)
{
    return ((width + alignment - 1) / alignment) * alignment;
}

tc_frame_buf_t *tc_frame_alloc(int width, int height)
{
    tc_frame_buf_t *f = (tc_frame_buf_t *)malloc(sizeof(tc_frame_buf_t));
    if (!f) return NULL;

    f->width    = width;
    f->height   = height;
    f->stride_y = align_stride(width, 64);       /* 64-byte aligned */
    f->stride_c = align_stride(width / 2, 64);   /* Chroma half-width */
    f->owned    = 1;
    f->poc      = 0;
    f->qp_avg   = TC_QP_DEFAULT;

    /* Luma: stride_y × height */
    f->y = (tc_pixel_t *)calloc((size_t)f->stride_y * height, 1);
    /* Cb/Cr: stride_c × (height/2) */
    size_t chroma_size = (size_t)f->stride_c * (height / 2);
    f->cb = (tc_pixel_t *)calloc(chroma_size, 1);
    f->cr = (tc_pixel_t *)calloc(chroma_size, 1);

    if (!f->y || !f->cb || !f->cr) {
        tc_frame_free(f);
        return NULL;
    }
    return f;
}

void tc_frame_free(tc_frame_buf_t *frame)
{
    if (!frame) return;
    if (frame->owned) {
        free(frame->y);
        free(frame->cb);
        free(frame->cr);
    }
    free(frame);
}

void tc_frame_copy(tc_frame_buf_t *dst, const tc_frame_buf_t *src)
{
    if (dst == src) return;   /* self-copy: avoid overlapping memcpy (UB) */

    int w  = tc_min(dst->width,  src->width);
    int h  = tc_min(dst->height, src->height);
    int cw = w / 2;
    int ch = h / 2;

    /* Luma */
    for (int row = 0; row < h; row++) {
        memcpy(dst->y + row * dst->stride_y,
               src->y + row * src->stride_y, w);
    }
    /* Cb */
    for (int row = 0; row < ch; row++) {
        memcpy(dst->cb + row * dst->stride_c,
               src->cb + row * src->stride_c, cw);
    }
    /* Cr */
    for (int row = 0; row < ch; row++) {
        memcpy(dst->cr + row * dst->stride_c,
               src->cr + row * src->stride_c, cw);
    }
}

tc_frame_buf_t *tc_frame_clone(const tc_frame_buf_t *src)
{
    tc_frame_buf_t *dst = tc_frame_alloc(src->width, src->height);
    if (!dst) return NULL;
    tc_frame_copy(dst, src);
    dst->poc    = src->poc;
    dst->qp_avg = src->qp_avg;
    return dst;
}
