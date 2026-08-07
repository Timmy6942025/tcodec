/*
 * bitstream.c — Bitstream reader/writer for TCodec
 *
 * Supports:
 *  - Raw bits (fixed-length)
 *  - Unsigned Exp-Golomb (ue)
 *  - Signed Exp-Golomb (se)
 *  - Byte alignment
 *  - CRC-16 computation for v1 bitstream error detection
 */

#include "tcodec_common.h"
#include <assert.h>

/* ── CRC-16 (CCITT) ──────────────────────────────────────────
 * Polynomial: 0x1021 (x^16 + x^12 + x^5 + 1)
 * Used for v1 bitstream error detection when TC_FLAG_CRC is set.
 * Covers frame header + CTU data (NOT the CRC bytes themselves).
 * ══════════════════════════════════════════════════════════════ */

uint16_t tc_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000)
                crc = (crc << 1) ^ 0x1021;
            else
                crc = crc << 1;
        }
    }
    return crc;
}

/* ── Writer ──────────────────────────────────────────────────── */

void tc_bs_writer_init(tc_bs_writer_t *w, uint8_t *buf, size_t capacity)
{
    w->buf      = buf;
    w->capacity = capacity;
    w->byte_pos = 0;
    w->bit_pos  = 0;
    if (capacity > 0) w->buf[0] = 0;
}

static void bs_ensure_capacity(tc_bs_writer_t *w, size_t need)
{
    TCODEC_UNUSED(w);
    TCODEC_UNUSED(need);
    /* In production, reallocate. For now, caller ensures capacity. */
}

void tc_bs_writer_write_bits(tc_bs_writer_t *w, uint32_t val, int nbits)
{
    assert(nbits >= 0 && nbits <= 32);
    if (nbits == 0) return;

    /* Mask to nbits */
    if (nbits < 32) val &= (1u << nbits) - 1;

    while (nbits > 0) {
        bs_ensure_capacity(w, w->byte_pos + 1);

        int avail = 8 - w->bit_pos;
        int write = tc_min(nbits, avail);

        /* Shift val so the top 'write' bits align with available slots */
        int shift = avail - write;
        uint8_t bits = (uint8_t)((val >> (nbits - write)) << shift);

        w->buf[w->byte_pos] |= bits;

        w->bit_pos += write;
        nbits -= write;

        if (w->bit_pos == 8) {
            w->bit_pos = 0;
            w->byte_pos++;
            if (w->byte_pos < w->capacity) w->buf[w->byte_pos] = 0;
        }
    }
}

void tc_bs_writer_write_ue(tc_bs_writer_t *w, uint32_t val)
{
    /* Exp-Golomb: val+1 encoded as (zeros)(1)(suffix)
     * val+1 in binary has floor(log2(val+1))+1 bits.
     * Prefix zeros = that count - 1. */
    uint32_t code = val + 1;
    int      bits = 0;
    uint32_t tmp  = code;
    while (tmp > 0) { bits++; tmp >>= 1; }
    int leading_zeros = bits - 1;

    tc_bs_writer_write_bits(w, 0, leading_zeros);    /* prefix zeros */
    tc_bs_writer_write_bits(w, code, bits);           /* 1 + suffix */
}

int tc_bs_se_bits(int32_t val)
{
    /* Bit cost of the signed Exp-Golomb code written by
     * tc_bs_writer_write_se(): map to unsigned then UE length. */
    uint32_t mapped;
    if (val > 0)       mapped = (uint32_t)(2 * val - 1);
    else if (val < 0)  mapped = (uint32_t)(-2 * val);
    else               mapped = 0;
    uint32_t code = mapped + 1;
    int bits = 0;
    while (code > 0) { bits++; code >>= 1; }
    return 2 * bits - 1;   /* leading_zeros + bits */
}

void tc_bs_writer_write_se(tc_bs_writer_t *w, int32_t val)
{
    /* Signed Exp-Golomb: map 0→0, 1→1, -1→2, 2→3, -2→4, ... */
    uint32_t mapped;
    if (val > 0)       mapped = (uint32_t)(2 * val - 1);
    else if (val < 0)  mapped = (uint32_t)(-2 * val);
    else               mapped = 0;
    tc_bs_writer_write_ue(w, mapped);
}

void tc_bs_writer_byte_align(tc_bs_writer_t *w)
{
    if (w->bit_pos > 0) {
        w->bit_pos = 0;
        w->byte_pos++;
        if (w->byte_pos < w->capacity) w->buf[w->byte_pos] = 0;
    }
}

size_t tc_bs_writer_bytes(tc_bs_writer_t *w)
{
    return w->byte_pos + (w->bit_pos > 0 ? 1 : 0);
}

/* ── Reader ──────────────────────────────────────────────────── */

void tc_bs_reader_init(tc_bs_reader_t *r, const uint8_t *buf, size_t size)
{
    r->buf      = buf;
    r->size     = size;
    r->byte_pos = 0;
    r->bit_pos  = 0;
    r->error    = 0;
}

uint32_t tc_bs_reader_read_bits(tc_bs_reader_t *r, int nbits)
{
    assert(nbits >= 0 && nbits <= 32);
    uint32_t val = 0;

    while (nbits > 0) {
        if (r->byte_pos >= r->size) {
            r->error = 1;
            return val;
        }

        int avail = 8 - r->bit_pos;
        int read  = tc_min(nbits, avail);

        uint8_t byte = r->buf[r->byte_pos];
        int     shift = avail - read;
        uint8_t bits = (byte >> shift) & ((1u << read) - 1);

        val = (val << read) | bits;

        r->bit_pos += read;
        nbits -= read;

        if (r->bit_pos == 8) {
            r->bit_pos = 0;
            r->byte_pos++;
        }
    }
    return val;
}

uint32_t tc_bs_reader_read_ue(tc_bs_reader_t *r)
{
    /* Count leading zeros */
    int leading_zeros = 0;
    int found_one = 0;
    while (r->byte_pos < r->size) {
        uint32_t bit = tc_bs_reader_read_bits(r, 1);
        if (bit == 1) { found_one = 1; break; }
        leading_zeros++;
        if (leading_zeros > 31) { r->error = 1; return 0; }
    }
    if (!found_one) { r->error = 1; return 0; }

    /* Read suffix (leading_zeros bits after the 1) */
    uint32_t suffix = 0;
    if (leading_zeros > 0) {
        suffix = tc_bs_reader_read_bits(r, leading_zeros);
    }

    return (1u << leading_zeros) - 1 + suffix;
}

int32_t tc_bs_reader_read_se(tc_bs_reader_t *r)
{
    uint32_t mapped = tc_bs_reader_read_ue(r);
    if (mapped == 0) return 0;
    if (mapped & 1)  return (int32_t)((mapped + 1) / 2);   /* positive */
    return -(int32_t)(mapped / 2);                           /* negative */
}

int tc_bs_reader_eof(tc_bs_reader_t *r)
{
    return r->byte_pos >= r->size;
}

/* ── Level constraints table ─────────────────────────────────
 * Maps level_idx → max resolution, DPB, bitrate.
 * Index 0 = auto (no constraints). See PROFILES.md §4.
 * ══════════════════════════════════════════════════════════════ */

static const tc_level_info_t level_table[] = {
    /*       max_w   max_h   max_dpb  max_bitrate */
    {        4096,   4096,   8,       0          },  /* 0: auto */
    {         320,    240,   1,       500000     },  /* 1: Level 1.0 */
    {         640,    480,   2,       2000000    },  /* 2: Level 1.1 */
    {        1280,    720,   2,       5000000    },  /* 3: Level 2.0 */
    {        1280,    720,   4,       10000000   },  /* 4: Level 2.1 */
    {        1920,   1080,   4,       20000000   },  /* 5: Level 3.0 */
    {        1920,   1080,   4,       40000000   },  /* 6: Level 3.1 */
    {        3840,   2160,   4,       80000000   },  /* 7: Level 4.0 */
    {        3840,   2160,   8,       160000000  },  /* 8: Level 4.1 */
};

const tc_level_info_t *tc_level_get_info(int level_idx)
{
    if (level_idx < 0 || level_idx > TC_LEVEL_MAX) return &level_table[0];
    return &level_table[level_idx];
}
