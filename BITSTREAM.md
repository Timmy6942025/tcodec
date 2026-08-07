# TCodec Bitstream Syntax — Versions 0, 1, and 2

**Status**: Working prototype. v0, v1, and v2 supported.
**Bitstream Versions**: 0 (legacy), 1 (default), 2 (quadtree)
**Magic**: `0x54 0x43 0x56` ("TCV")

---

## 1. Container Format

A TCodec bitstream file (`.tcv`) consists of a sequence of **packets**,
one per frame. Each packet is prefixed with a 4-byte little-endian size:

```
┌─────────────┐
│ uint32 size  │  4 bytes — size of the following frame data
├─────────────┤
│ Frame data   │  `size` bytes — one encoded frame
├─────────────┤
│ uint32 size  │  Next frame...
│ Frame data   │
│ ...          │
└─────────────┘
```

**NOTE**: The size prefix is written by the CLI tools (`tcenc`/`tcdec`),
not by the codec library itself. The library API operates on individual
frame packets without the size prefix.

---

## 2. Frame Data Layout

Each frame packet contains:

```
┌──────────────────────┐
│ Frame Header         │  12 bytes (v0) or 14 bytes (v1/v2)
├──────────────────────┤
│ [WPP entry points]   │  Present when TC_FLAG_WPP is set
├──────────────────────┤
│ CTU Row 0            │
│   CTU (0,0) data     │
│   CTU (1,0) data     │
│   ...                │
├──────────────────────┤
│ CTU Row 1            │
│   ...                │
├──────────────────────┤
│ ...                  │
├──────────────────────┤
│ [CRC-16]             │  2 bytes (v1/v2, when TC_FLAG_CRC is set)
└──────────────────────┘
```

CTUs are coded in **raster scan order**: left-to-right within a row,
top-to-bottom across rows. All blocks within a CTU are coded before
moving to the next CTU.

---

## 3. Frame Header

### 3.1 Version 0 Header (12 bytes = 96 bits)

| Bit Offset | Width | Field | Description |
|-----------|-------|-------|-------------|
| 0–7 | 8 | `magic[0]` | `0x54` ('T') |
| 8–15 | 8 | `magic[1]` | `0x43` ('C') |
| 16–23 | 8 | `magic[2]` | `0x56` ('V') |
| 24–31 | 8 | `version` | `0x00` (version 0) |
| 32–47 | 16 | `width` | Frame width in pixels (1–4096) |
| 48–63 | 16 | `height` | Frame height in pixels (1–4096) |
| 64–71 | 8 | `flags` | Bitfield (see §3.3) |
| 72–79 | 8 | `qp_delta` | Signed offset from default QP (32) |
| 80–87 | 8 | `frame_num` | Frame counter, low 8 bits |
| 88–95 | 8 | `reserved` | `0x00` (must be ignored by decoders) |

### 3.2 Version 1 Header (14 bytes = 112 bits)

| Bit Offset | Width | Field | Description |
|-----------|-------|-------|-------------|
| 0–7 | 8 | `magic[0]` | `0x54` ('T') |
| 8–15 | 8 | `magic[1]` | `0x43` ('C') |
| 16–23 | 8 | `magic[2]` | `0x56` ('V') |
| 24–31 | 8 | `version` | `0x01` (version 1) |
| 32–47 | 16 | `width` | Frame width in pixels (1–4096) |
| 48–63 | 16 | `height` | Frame height in pixels (1–4096) |
| 64–71 | 8 | `flags` | Bitfield (see §3.3, v1 adds RAP/CRC/EXT) |
| 72–79 | 8 | `qp_delta` | Signed offset from default QP (32) |
| 80–87 | 8 | `frame_num` | Frame counter, low 8 bits |
| 88–95 | 8 | `profile_level` | `(profile << 4) \| level_idx` |
| 96–111 | 16 | `tool_flags` | Which coding tools are active (see §3.4) |

After the header, the bitstream is **byte-aligned** (padding bits to the
next byte boundary).

### 3.3 Flags Byte

| Bit | Mask | Field | v0 | v1/v2 | Description |
|-----|------|-------|----|----|-------------|
| 7 | `0x80` | `key_frame` | ✅ | ✅ | 1 = I-frame (intra-only), 0 = P-frame |
| 6 | `0x40` | `wpp` | ✅ | ✅ | 1 = WPP row entry points present |
| 5 | `0x20` | `rap` | — | ✅ | 1 = Random Access Point (independent decode) |
| 4 | `0x10` | `crc` | — | ✅ | 1 = CRC-16 appended after CTU data |
| 3 | `0x08` | `ext_header` | — | ✅ | 1 = extension header sections follow |
| 2–3 | `0x0C` | `tile_cols_log2` | ✅ | ✅ | log2 of tile columns (0 = 1 col) |
| 0–1 | `0x03` | `tile_rows_log2` | ✅ | ✅ | log2 of tile rows (0 = 1 row) |

**NOTE**: v0 bits 5–3 are reserved (must be 0). v1 uses them for
RAP, CRC, and ext_header. This is backward-compatible because a v0
decoder never reads these bits, and a v1 decoder knows the version.

**NOTE**: Tile fields are coded but **not actually used** by the
encoder or decoder. The entire frame is processed as a single tile.
These bits are reserved for future tile-based parallelism.

### 3.4 Tool Flags (16 bits, v1/v2 header)

| Bit | Mask | Field | Profile Required | Description |
|-----|------|-------|-----------------|-------------|
| 0 | `0x0001` | `skip_merge` | baseline-mobile | Skip/merge inter modes |
| 1 | `0x0002` | `cfl_chroma` | baseline-mobile | Chroma-from-luma prediction |
| 2 | `0x0004` | `jnd_weighting` | baseline-mobile | JND band quantization weighting |
| 3 | `0x0008` | `median_mv_pred` | baseline-mobile | Median MV predictor + MVD coding |
| 4 | `0x0010` | `multi_ref` | streaming-main | Multiple reference frames |
| 5 | `0x0020` | `six_tap_interp` | streaming-main | 6-tap luma interpolation filter |
| 6 | `0x0040` | `entropy_coded` | streaming-main | Context-modeled range-coded entropy |
| 7 | `0x0080` | `dering` | streaming-main | Directional deringing (future) |
| 8 | `0x0100` | `sao` | streaming-main | v2 luma Band Offset; EO/restoration are reserved |
| 9 | `0x0200` | `grain_synthesis` | grain-cinema | Film grain synthesis (future) |
| 10 | `0x0400` | `bipred` | archive-high | Hierarchical B-frame bi-prediction |
| 11 | `0x0800` | `loop_restoration` | archive-high | Wiener-like restoration (future) |
| 12 | `0x1000` | `affine_motion` | archive-high | Affine motion model (future) |
| 13 | `0x2000` | `extended_part` | archive-high | Extended partition types (future) |
| 14 | `0x4000` | `context_reset` | — | Context model reset point (future) |
| 15 | — | *reserved* | — | Must be 0 |

A decoder for profile N silently ignores tools that are not in its
profile (it must not encounter them if the encoder is compliant).

### 3.5 Profile/Level Byte

The `profile_level` byte packs profile and level into one byte:

```
profile_level = (profile << 4) | (level_idx & 0x0F)
```

| Profile | Value | Name |
|---------|-------|------|
| 0 | `baseline-mobile` | Low-power ARM decoder |
| 1 | `streaming-main` | Primary streaming profile |
| 2 | `archive-high` | Offline / studio quality |
| 3 | `grain-cinema` | Film grain synthesis required |

| Level | Idx | Max Width | Max Height | Max DPB | Max Bitrate |
|-------|-----|-----------|------------|---------|-------------|
| Auto | 0 | — | — | — | — |
| 1.0 | 1 | 320 | 240 | 1 | 500 kbps |
| 1.1 | 2 | 640 | 480 | 2 | 2 Mbps |
| 2.0 | 3 | 1280 | 720 | 2 | 5 Mbps |
| 2.1 | 4 | 1280 | 720 | 4 | 10 Mbps |
| 3.0 | 5 | 1920 | 1080 | 4 | 20 Mbps |
| 3.1 | 6 | 1920 | 1080 | 4 | 40 Mbps |
| 4.0 | 7 | 3840 | 2160 | 4 | 80 Mbps |
| 4.1 | 8 | 3840 | 2160 | 8 | 160 Mbps |

### 3.6 Derived Fields

Decoders derive these from the header:

- `frame_type` = KEY if `key_frame` bit set, else INTER
- `qp` = `TC_QP_DEFAULT + (int8_t)qp_delta` = `32 + (int8_t)qp_delta`
  - Clamped to [TC_QP_MIN, TC_QP_MAX] = [0, 63] by the decoder
  - qp_delta is stored as uint8_t but was encoded as `(uint8_t)(int8_t)(qp - 32)`,
    so values > 127 represent negative deltas (QP < 32)
- `tile_cols` = `1 << tile_cols_log2`
- `tile_rows` = `1 << tile_rows_log2`
- `is_rap` = `(flags & TC_FLAG_RAP) != 0` (v1 only)
- `has_crc` = `(flags & TC_FLAG_CRC) != 0` (v1 only)
- `profile` = `(profile_level >> 4) & 0x0F` (v1 only, defaults to 0 for v0)
- `level_idx` = `profile_level & 0x0F` (v1 only, defaults to 0 for v0)

### 3.7 CRC-16 (v1/v2)

When `TC_FLAG_CRC` is set, a CRC-16 (CCITT, polynomial 0x1021) is
appended after the CTU data. The CRC covers all bytes from the start
of the frame data up to (but not including) the CRC itself.

- **Encoder**: Computes CRC over header + CTU data, appends 2 bytes (big-endian)
- **Decoder**: Validates CRC before decoding. On mismatch, sets
  `last_crc_valid = 0` but still decodes the frame (graceful degradation).
  Caller can check via `tc_decoder_crc_valid()`.

---

## 4. CTU Data

Each CTU contains data for all 8×8 blocks within it (8×8 = 64 blocks
per CTU). Blocks that extend beyond the frame boundaries are **skipped**
entirely (no data written or read).

### 4.1 Per-CTU Structure

```
For each 8×8 block (by = 0..7, bx = 0..7):
    if block is outside frame: skip
    dct_size_flag: 1 bit           (0 = 8×8 WHT, 1 = 4×4 WHT)
    block_data (see below)
```

### 4.2 Per-Block Data

#### 4.2.1 Mode Decision

**Key frames** (I-frames): No mode flag. All blocks are intra.

**Inter frames** (P-frames):
```
block_mode: 2 bits                 (0=skip, 1=inter, 2=intra, 3=merge)
```

| Value | Mode | Residual | MV Source |
|-------|------|----------|-----------|
| 0 | Skip | Zero | MVD + ref_idx signaled |
| 1 | Inter | Non-zero | MVD + ref_idx signaled |
| 2 | Intra | Non-zero | None (intra prediction) |
| 3 | Merge | Zero | Derived from median of spatial neighbors |

#### 4.2.2 Intra Block

Read only when `block_mode == 2` (intra) or on key frames.

```
intra_mode: 5 bits (unsigned)     (0..17, see intra mode table)
```

If `intra_mode >= 18`, decoder falls back to DC (mode 1).

| Value | Name | Direction | Displacement |
|-------|------|-----------|-------------|
| 0 | Planar | Bilinear | — |
| 1 | DC | Average | — |
| 2 | Angular NE | ~45° vertical | +1.0 px/row |
| 3 | Angular NNE | ~26° vertical | +0.5 px/row |
| 4 | Angular NNW | ~11° vertical | +0.19 px/row |
| 5 | Angular N (vert) | 0° vertical | 0 px/row |
| 6 | Angular NWW | ~-11° vertical | -0.19 px/row |
| 7 | Angular NW | ~-26° vertical | -0.5 px/row |
| 8 | Angular WN | ~-45° vertical | -1.0 px/row |
| 9 | Angular EN | ~45° horizontal | +1.0 px/col |
| 10 | Angular EEN | ~26° horizontal | +0.5 px/col |
| 11 | Angular EE | ~11° horizontal | +0.19 px/col |
| 12 | Angular E (horiz) | 0° horizontal | 0 px/col |
| 13 | Angular WW | ~-11° horizontal | -0.19 px/col |
| 14 | Angular WWN | ~-26° horizontal | -0.5 px/col |
| 15 | Angular WN (horiz) | ~-45° horizontal | -1.0 px/col |
| 16 | Angular NNW (horiz) | ~-56° horizontal | -1.5 px/col |
| 17 | Angular NNE (horiz) | ~56° horizontal | +1.5 px/col |

#### 4.2.3 Inter/Skip Block (mode 0 or 1)

Read only when `block_mode == 0` (skip) or `block_mode == 1` (inter).

```
ref_idx: 2 bits (unsigned)         Reference frame index (0..3)
mvd_x: se(v)  (signed Exp-Golomb)  Motion vector difference X
mvd_y: se(v)  (signed Exp-Golomb)  Motion vector difference Y
```

The actual motion vector is reconstructed as:
- `predictor_mv` = median of spatial neighbors (left, above, above-right)
  - Default: `{frame_x * 4, frame_y * 4}` (collocated) if no inter neighbors
- `mv.x = mvd_x + predictor_mv.x`  (quarter-pel)
- `mv.y = mvd_y + predictor_mv.y`  (quarter-pel)

MVD is coded **relative to the median predictor**, not the collocated
position. This produces significantly smaller MVDs when spatial
neighbors are available, since the predictor captures the local motion
field. The median computation uses the same neighbor selection and
sorting as merge mode (see §4.2.4).

**Skip blocks** (mode 0) carry the actual MV via MVD — skip means
zero residual, not zero MV. The decoder needs the correct MV to
produce the right prediction.

**Safety**: If `ref_idx >= TC_REF_FRAMES`, the decoder clamps to 0.

#### 4.2.4 Merge Block (mode 3)

No ref_idx or MVD is signaled. The MV is derived from the median
of spatial neighbors' MVs, identical to the predictor computation:

```
// No additional bits read for merge mode
mv = median(left.mv, above.mv, above_right.mv)
```

Neighbor selection rules:
1. Left neighbor: block at (bx-1, by) if bx > 0 and that block is not intra
2. Above neighbor: block at (bx, by-1) if by > 0 and that block is not intra
3. Above-right neighbor: block at (bx+1, by-1) if by > 0 and bx < 7 and not intra

If any neighbor is unavailable, it is replaced by another available
neighbor. If no inter neighbors exist, the default is the collocated
position `{frame_x * 4, frame_y * 4}`.

The median is computed by sorting the three x-components and taking
the middle value, then similarly for y-components.

Merge always uses reference frame index 0 (most recent DPB entry).

#### 4.2.5 Coefficient Data (Luma)

**If dct_size_flag = 0** (8×8 WHT):

```
coeff_data: 64 coefficients coded as one block
    last_nz_plus1: ue(v)    Last nonzero position + 1 (0 = all zero)
    for i = last_nz down to 0 (zigzag order):
        magnitude: ue(v)     Absolute coefficient value
        if magnitude > 0:
            sign: 1 bit      0 = positive, 1 = negative
```

**If dct_size_flag = 1** (4×4 WHT):

Four separate sub-blocks, each 16 coefficients, coded in order:
```
for sy = 0..1, sx = 0..1:
    sub_coeff_data: 16 coefficients (same format as above, 4×4 zigzag)
```

The 4×4 sub-blocks are extracted from the 8×8 residual in raster order
within the block:
- Sub-block (0,0): rows 0–3, cols 0–3
- Sub-block (1,0): rows 0–3, cols 4–7
- Sub-block (0,1): rows 4–7, cols 0–3
- Sub-block (1,1): rows 4–7, cols 4–7

#### 4.2.6 Coefficient Data (Chroma)

After luma, each 8×8 luma block has two chroma components (Cb, Cr).
Each chroma component is a 4×4 block:

```
for comp = 0..1:    (0 = Cb, 1 = Cr)
    chroma_coeff_data: 16 coefficients (4×4 zigzag, same format)
```

**Chroma QP** = `clip(luma_qp + 1, 0, 63)`

**Chroma prediction**:
- **Intra blocks**: CfL (Chroma-from-Luma) — uses reconstructed luma
  to predict chroma: `c_pred = c_dc + ((luma_val - luma_avg) >> 3)`,
  where `c_dc` is derived from neighboring chroma samples, `luma_avg`
  is the average of the corresponding 4×4 luma block, and alpha ≈ 0.125.
- **Inter/skip/merge blocks**: Fixed DC value of 128.

**NOTE**: Chroma always uses 4×4 WHT, regardless of the luma dct_size_flag.
Chroma always uses `tc_quantize()`/`tc_dequantize()` with `band=0` (no JND
weighting), unlike luma which applies JND weighting inline.

---

## 5. Coefficient Coding (tANS)

### 5.1 tANS Encoding

Coefficient data is encoded using tANS (tabled Asymmetric Numeral Systems)
via `tc_tans_enc_coeffs()` / `tc_tans_dec_coeffs()`. tANS provides better
compression than universal codes (Exp-Golomb) by adapting to the actual
coefficient distribution.

**Current status**: the legacy path uses the reserved tANS/Exp-Golomb
compatibility helpers, while the v1/v2 range-coded path uses context-modeled
binary coding for modes, motion, significance, levels, and selected syntax.
Further context specialization and measured compression gains remain planned.

### 5.2 Motion Vector Coding (Exp-Golomb)

MVD components (mvd_x, mvd_y) are still coded as signed Exp-Golomb (se(v)).
This is a less critical compression path since MVD values are typically
small after median predictor differencing.

### 5.3 Exp-Golomb Reference

Unsigned (ue):
```
code_num = val + 1
leading_zeros = floor(log2(code_num))
bit pattern: [0 × leading_zeros] [1] [suffix of leading_zeros bits]
```

| Value | Code | Bits |
|-------|------|------|
| 0 | `1` | 1 |
| 1 | `010` | 3 |
| 2 | `011` | 3 |
| 3 | `00100` | 5 |
| n | `0×floor(log2(n+1)) 1 (n+1)%2^floor(log2(n+1))` | 2×floor(log2(n+1))+1 |

Signed (se):

| ue value | se value |
|----------|----------|
| 0 | 0 |
| 1 | +1 |
| 2 | −1 |
| 3 | +2 |
| 4 | −2 |
| n | (−1)^(n+1) × ceil(n/2) |

Formula: `se = (mapped & 1) ? (mapped+1)/2 : -(mapped/2)`
where `mapped = ue(val)`.

---

## 6. Zigzag Scan Order

### 6.1 4×4 Zigzag

```
 0  1  4  8
 5  2  3  6
 9 12 13 10
 7 11 14 15
```

Position index → raster index mapping:
```c
{0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15}
```

### 6.2 8×8 Zigzag

```
 0  1  8 16  9  2  3 10
17 24 32 25 18 11  4  5
12 19 26 33 40 48 41 34
27 20 13  6  7 14 21 28
35 42 49 56 57 50 43 36
29 22 15 23 30 37 44 51
58 59 52 45 38 31 39 46
53 60 61 54 47 55 62 63
```

Position index → raster index mapping:
```c
{0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63}
```

---

## 7. Complete Bitstream Syntax (Pseudo-code)

### 7.1 v0 Bitstream

```
frame_packet_v0() {
    // Header (12 bytes)
    magic[0]       = u(8)    // 0x54
    magic[1]       = u(8)    // 0x43
    magic[2]       = u(8)    // 0x56
    version        = u(8)    // 0
    width          = u(16)
    height         = u(16)
    flags          = u(8)
    qp_delta       = u(8)
    frame_num      = u(8)
    reserved       = u(8)    // 0x00
    byte_align()

    // Derived
    is_key = (flags >> 7) & 1
    qp = clip(32 + qp_delta, 0, 63)

    // CTU data
    ctu_data_all()
}
```

### 7.2 v1 Bitstream

```
frame_packet_v1() {
    // Header (14 bytes)
    magic[0]       = u(8)    // 0x54
    magic[1]       = u(8)    // 0x43
    magic[2]       = u(8)    // 0x56
    version        = u(8)    // 1
    width          = u(16)
    height         = u(16)
    flags          = u(8)    // Includes RAP, CRC, EXT bits
    qp_delta       = u(8)
    frame_num      = u(8)
    profile_level  = u(8)    // (profile << 4) | level_idx
    tool_flags     = u(16)   // Active coding tools
    byte_align()

    // Derived
    is_key = (flags >> 7) & 1
    is_rap = (flags >> 5) & 1
    has_crc = (flags >> 4) & 1
    has_ext = (flags >> 3) & 1
    profile = profile_level >> 4
    level_idx = profile_level & 0x0F
    qp = clip(32 + qp_delta, 0, 63)

    // Extension header sections (if has_ext)
    while (has_ext) {
        ext_type = u(8)
        if (ext_type == 0) break   // TC_EXT_END
        ext_size = u(16)
        ext_payload(ext_size)
    }

    // WPP entry points (if flags.wpp)
    if (flags & 0x40) {
        wpp_entry_points()
    }

    // CTU data
    ctu_data_all()

    // CRC-16 (if has_crc)
    if (has_crc) {
        crc16 = u(16)   // Big-endian, covers header + CTU data
    }
}
```

### 7.3 CTU Data (common to v0 and v1)

```
ctu_data_all() {
    for (row = 0; row < num_ctu_rows; row++) {
        for (col = 0; col < num_ctu_cols; col++) {
            ctu_data(row, col)
        }
    }
}

ctu_data(row, col) {
    for (by = 0; by < 8; by++) {
        for (bx = 0; bx < 8; bx++) {
            blk_x = col * 64 + bx * 8
            blk_y = row * 64 + by * 8
            if (blk_x + 8 > width || blk_y + 8 > height)
                continue    // Skip out-of-bounds blocks

            // Mode decision (P-frames only)
            if (!is_key) {
                block_mode = u(2)   // 0=skip, 1=inter, 2=intra, 3=merge
            } else {
                block_mode = 2      // KEY frames: always intra
            }

            // Prediction data
            if (block_mode == 2) {         // intra
                intra_mode = u(5)   // 0..17
            } else if (block_mode == 3) {  // merge
                // MV derived from median of spatial neighbors
                // No ref_idx or MVD signaled
            } else {                       // skip (0) or inter (1)
                ref_idx = u(2)     // Reference frame index (0..3)
                mvd_x = se()        // Signed Exp-Golomb
                mvd_y = se()        // Signed Exp-Golomb
            }

            // Luma coefficients (skip/merge have no coefficients)
            if (block_mode == 0 || block_mode == 3) {
                // Skip/merge: zero residual, no coefficient data
            } else {
                dct_size_flag = u(1)   // 0=8×8, 1=4×4
                if (dct_size_flag == 1) {
                // Four 4×4 sub-blocks
                for (sy = 0; sy < 2; sy++) {
                    for (sx = 0; sx < 2; sx++) {
                        coeff_block(16)   // 4×4 zigzag
                    }
                }
            } else {
                coeff_block(64)      // 8×8 zigzag
                } // end if dct_size_flag
            } // end if block_mode

            // Chroma coefficients
            chroma_qp = clip(qp + 1, 0, 63)
            for (comp = 0; comp < 2; comp++) {
                coeff_block(16)      // 4×4 zigzag (Cb then Cr)
            }
        }
    }
}

coeff_block(n) {
    // Coefficients coded via tANS (see §5.1)
    // tc_tans_enc_coeffs / tc_tans_dec_coeffs
    // Contextless tANS with fixed probability tables
    tans_coeff_block(n)              // n coefficients in zigzag order
}
```

---

## 7.6 Version 2 Quadtree Payload

Version 2 keeps the 14-byte v1 header layout but changes only the payload
syntax. It is selected explicitly with `tc_config_t.use_v2 = 1` or the CLI
`--v2`; v1 remains the default. A v2 payload is sequential and must not set
`TC_FLAG_WPP`. Range-coded v2 resets its range state and all contexts at each
frame, then codes CTUs in raster order (left-to-right, top-to-bottom).

Each in-frame 64×64 CTU is a quadtree of 64, 32, 16, or 8 pixel CUs. A node
at depth 0..2 starts with one split flag. Split children are emitted in
raster order `(top-left, top-right, bottom-left, bottom-right)`; depth 3 is
always a leaf. Nodes wholly outside a partial frame have no syntax.

A leaf carries one intra flag. Intra leaves carry a 5-bit luma mode, a chroma
intra flag, optional 3-bit chroma mode, luma transform syntax, and two chroma
4×4 residual planes. Inter leaves carry skip or merge signaling; explicit
inter leaves carry a transform-size flag and signed Exp-Golomb MVD x/y. The
reference is `dpb[0]` for v2 P frames. The MV predictor is the median of the
available left, above, and above-right 8×8 MV-grid cells; intra cells are
unavailable. Merge and skip use the predictor directly; explicit inter adds
MVD to it. v2 B-frame emission is disabled by the encoder.

Luma residuals use either 8×8 or four 4×4 residual transforms, with the shared
JND-weighted quantizer (`tc_quant_coeff`/`tc_dequant_coeff`). Chroma uses
4:2:0 collocated motion compensation or neighbour-DC intra prediction and
4×4 residual transforms. Coefficients use the selected entropy path: the
range coder when `TC_TOOL_ENTROPY_CODED` is set, otherwise the legacy
Exp-Golomb/tANS-reserved path. Encoder and decoder use the same integer
transform, dequantization, clipping, and chroma helpers.

The encoder first performs integer RDO on a private reconstruction and then
replays the selected tree. Leaf snapshots are the pre-leaf reconstruction;
the MV grid is rebuilt incrementally during replay. The decoder mirrors this
raster traversal. Each complete in-frame CTU is deblocked before the next CTU
is decoded. When `TC_TOOL_SAO` is set, each CTU then carries `sao_present`
(1 bit). If present, it carries `sao_band` (5 bits, 0..31) and
`sao_offset_code` (4 bits, 0..14), where `offset = sao_offset_code - 7`.
Code 15 is reserved and invalid. The selected luma Band Offset is applied
after deblocking and is clipped to [0,255]; chroma is unchanged. The encoder
sets `TC_TOOL_SAO` for every v2 frame, including frames whose per-CTU flag is
zero. v0/v1 never consume this syntax. The frame payload is byte-aligned
after entropy flush, followed by CRC-16 when enabled. A v2 decoder rejects
WPP-marked packets, unsupported versions, underflow, and invalid v2 syntax
rather than routing it through the legacy block decoder.

## 8. Bitstream Compliance

### 8.1 Version Handling

| Version | Behavior |
|---------|----------|
| 0 | Accepted by all decoders (12-byte header) |
| 1 | Accepted by v1-aware decoders (14-byte header) |
| 2 | Accepted only by the explicit v2 quadtree decoder; sequential, WPP-disabled payload |
| 3+ | Rejected with `TC_ERR_BITSTREAM` |

### 8.2 Decoder Error Handling

Current decoder behavior on malformed bitstreams:

| Condition | Behavior |
|-----------|----------|
| Invalid magic | Return `TC_ERR_BITSTREAM` |
| Invalid version (>2) | Return `TC_ERR_BITSTREAM` |
| Unknown profile (>3) | Return `TC_ERR_BITSTREAM` |
| Level constraint exceeded | Return `TC_ERR_BITSTREAM` |
| EOF mid-header | Return `TC_ERR_EOF` |
| Invalid intra mode (≥18) | Legacy path clamps to DC; v2 path rejects/clamps only as a defensive fallback |

| `last_nz_plus1 > n` | Clamp to `n - 1` |
| Out-of-bounds MV | Fallback to DC(128) prediction — safe degradation |
| Invalid ref_idx (≥4) | Clamp to 0 |
| CRC mismatch | Set `last_crc_valid = 0`, continue decoding (graceful) |
| Truncated coefficient data | Returns a bitstream/EOF error through the bounded reader |

### 8.3 Known Bitstream Deficiencies

1. ~~No version negotiation~~ ✅ explicit v0/v1/v2 dispatch; versions >2 are rejected
2. ~~No CRC or checksum~~ ✅ v1: CRC-16 (CCITT) when TC_FLAG_CRC set
3. ~~No random access points~~ ✅ v1: TC_FLAG_RAP marks independent frames
4. **No error resilience**: A single bit error corrupts all subsequent data
5. ~~Fixed header size~~ ✅ v1: ext_header flag allows future extensions
6. **WPP entry points**: When TC_FLAG_WPP is set, an entry point table follows the header
7. **Reference frame signaling**: 4 DPB slots, ref_idx coded per block (2 bits)
8. ~~No bit-rate signaling~~ v1: level constraints imply max bitrate
9. ~~NEON/scalar output divergence~~ ✅ deblock and v2 SAO paths have scalar/NEON parity coverage
10. **WPP/sequential bitstream divergence**: WPP bitstreams byte-align per row (sequential do not)
11. ~~Tool flags informational only~~ ✅ v2 SAO syntax is consumed only when `TC_TOOL_SAO` is set
12. **Extension header not yet emitted**: TC_FLAG_EXT_HEADER is defined but encoder doesn't write extensions

---

## 9. Encoding Examples

### 9.1 v0 Frame (128×128, QP 32, P-frame)

```
Header (12 bytes):
  54 43 56 00   // Magic + version=0
  00 80 00 80   // Width=128, Height=128
  00 00 00 00   // flags=0 (P-frame), qp_delta=0 (QP=32), frame_num=0, reserved=0

CTU data...
```

### 9.2 v1 Frame (128×128, QP 32, I-frame, baseline-mobile, Level 1.1, CRC)

```
Header (14 bytes):
  54 43 56 01   // Magic + version=1
  00 80 00 80   // Width=128, Height=128
  B0 00 00 02   // flags=0xB0 (key+RAP+CRC), qp_delta=0, frame_num=0,
                // profile_level=0x02 (baseline-mobile, Level 1.1)
  00 3F         // tool_flags=0x003F (skip_merge+cfl+jnd+median_mv+six_tap)

CTU data...

CRC-16 (2 bytes):
  XX XX         // CRC-16 CCITT over header + CTU data
```

---

## 10. Version Migration Guide

### 10.1 Upgrading from v0 to v1

- v0 bitstreams are always decodable by v1-aware decoders
- v1 bitstreams are **not** decodable by v0-only decoders (version byte differs)
- The v1 header extends the v0 `reserved` byte into `profile_level`,
  and adds 2 bytes of `tool_flags` after it
- Set `cfg.bitstream_version = TC_VERSION_V1` to encode v1 (default)
- Set `cfg.bitstream_version = TC_VERSION_V0` for legacy compatibility

### 10.2 Adding Future v3 Features

When a new bitstream version is needed:
1. Increment the bitstream version constant beyond `TC_VERSION_V2`
2. Add new header fields after tool_flags (or use ext_header)
3. Update decoder's version dispatch in `read_frame_header()`
4. Update the applicable frame-header constant
5. Add v2-specific tests
6. Update this document
