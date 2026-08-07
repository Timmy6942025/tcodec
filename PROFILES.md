# TCodec Profiles and Levels — Versions 0, 1, and 2

**Status**: v1 profiles/levels and the explicit v2 quadtree payload are implemented and enforced where applicable.
**Bitstream Versions**: 0 (no profile/level), 1 (profiles + levels + tool flags), 2 (explicit quadtree payload)
**Last Updated**: v2 quadtree payload and luma SAO Band Offset are implemented; EO/restoration remain reserved.

---

## 1. Overview

Profiles define **which coding tools a decoder must support**. Levels define
**performance and memory constraints**. Together they allow a decoder to
announce its capabilities and for encoders to produce bitstreams that will
decode successfully on a given class of device.

**Current state**: Profiles and levels are defined, signaled in the v1/v2
frame header, and validated by the decoder. The encoder populates profile/level
and tool flags based on the configured profile and actual tool usage. v0
bitstreams have no profile/level fields and default to baseline-mobile. v2
adds the explicit quadtree payload while retaining the v1 header layout.

This document serves as:
1. A record of the **current tool inventory** and its activation status
2. The **implemented** profile/level structure for bitstream version 1
3. A **preset reference** for the existing encoder presets

---

## 2. Current Tool Inventory

### 2.1 Coding Tools Active in Pipeline

| Tool | Code Location | Notes |
|------|--------------|-------|
| 18-mode intra prediction | `predict.c` | Planar, DC, 7 vertical angular, 9 horizontal angular |
| WHT 4×4 / 8×8 transform | `transform.c` / `transform_neon.c` | Self-inverse, NEON dispatched |
| Variance-based transform size | `encoder.c` | Threshold = 512 |
| Entropy coding | `entropy.c` / `range_coder.c` | Legacy Exp-Golomb/tANS-reserved path and context-modeled range-coded path selected by tool flag |
| Hierarchical hex motion search | `motion.c` | ±16/32/64 range by preset |
| 6-tap luma interpolation | `motion.c` | H.264-style half-pel + bilinear quarter-pel |
| Multi-reference inter prediction | `encoder.c` | 4 DPB slots; SLOW preset searches all |
| Skip/merge inter modes | `encoder.c` / `decoder.c` | 2-bit mode field: skip/inter/intra/merge |
| Median MV predictor | `encoder.c` / `decoder.c` | Search center + MVD predictor + merge MV |
| Deblocking filter | `filter.c` / `filter_neon.c` | Scalar guarded; NEON replaces on ARM |
| ρ-domain rate control | `ratectl.c` | CQP, CBR, VBR modes |
| CfL chroma prediction | `encoder.c` / `decoder.c` | Chroma-from-luma for intra; DC(128) for inter |
| JND band weighting | `encoder.c` / `decoder.c` | Applied inline per-coefficient for luma |
| Scene cut detection | `encoder.c` | Chi-squared histogram, forces keyframe |
| NEON SAD | `motion_neon.c` | Replaces scalar via `#if TCODEC_NEON` |
| NEON color conversion | `color_neon.c` | 32-bit accumulation luma; scalar chroma/inverse |

### 2.2 Coding Tools Present but Inactive

| Tool | Code Location | Status | Reason |
|------|--------------|--------|--------|
| Pixel-mode DCT helpers | `transform.c` / `transform_neon.c` | **Reserved** | v2 uses the residual DCT-II path; pixel-mode syntax is not signaled |
| Thread pool / WPP | `threadpool.c`, `encoder.c`, `decoder.c` | **Active for legacy v1** | Range-coded/v2 payloads are sequential; legacy WPP has entry points and parity coverage |
| NEON inter predict | `motion_neon.c` | **Quality-gated** | Scalar six-tap path remains authoritative where the NEON kernel would diverge |

### 2.3 Coding Tools Not Implemented

| Tool | Master Plan Phase | Priority |
|------|------------------|----------|
| Deeper context specialization for entropy coding | Phase 3 | Critical research gap |
| Deringing / CDEF | Phase 7 | Medium |
| Loop restoration | Phase 7 | Low |
| Film grain synthesis | Phase 7 | Medium (film content) |
| Real lookahead | Phase 8 | High |
| VBV-validated rate control | Phase 8 | Medium |
| Bi-prediction / B-frames | Phase 6 | Implemented in the v1 extension path; broad RD benefit remains unestablished |
| Affine / global motion | Phase 6+ | Low |
| QTBT/binary partition extension | Phase 5 | Medium; v2 quadtree 64/32/16/8 is implemented |
| Extended chroma intra modes (DM, planar, angular) | Phase 5 | Medium |
| Content-adaptive transform selection | Phase 4 | Medium |
| Scaling matrices / freq-dependent quant | Phase 4 | Medium |
| 8-tap interpolation filters | Phase 6 | Medium |

---

## 3. Profiles (Implemented in v1)

These profiles are **implemented** in the v1 bitstream. The encoder
populates tool flags based on actual tool usage; the decoder reads
them and validates profile compliance.

### 3.1 Profile 0: `baseline-mobile` (TC_PROFILE_BASELINE_MOBILE = 0)

**Target**: Raspberry Pi, mid-range Android phones, low-power embedded  
**Decoder complexity**: Minimal — no tools that require significant
per-block decision logic or memory.

**v1 tool flags signaled** (always active in current encoder):
- `TC_TOOL_SKIP_MERGE` — Skip/merge inter modes
- `TC_TOOL_CFL_CHROMA` — Chroma-from-luma prediction
- `TC_TOOL_JND_WEIGHTING` — JND band quantization weighting
- `TC_TOOL_MEDIAN_MV_PRED` — Median MV predictor + MVD coding
- `TC_TOOL_SIX_TAP_INTERP` — 6-tap luma interpolation

| Tool | Supported |
|------|-----------|
| 18-mode intra prediction | ✅ |
| WHT 4×4 / 8×8 | ✅ |
| tANS coefficient coding | ✅ |
| Single-reference inter | ✅ |
| 6-tap luma interpolation | ✅ |
| Deblocking filter | ✅ |
| CfL chroma prediction | ✅ |
| Skip / merge modes | ✅ |
| JND band weighting | ✅ |
| Median MV predictor | ✅ |
| Multi-reference | ❌ |
| Bi-prediction | ❌ (profile-gated; archive-high/v1 extension path only) |
| 8-tap interpolation | ❌ |
| v2 luma SAO Band Offset | ✅ (v2 only; EO/restoration planned) |
| Loop restoration | ❌ |
| Film grain synthesis | ❌ |
| Affine motion | ❌ |
| Extended partition types | ❌ |

**Rationale**: This is the deployment profile for the ARM decoder
optimization work (Phase 9). Every tool included must be measurable
in NEON cycle count and must not push decode FPS below target on
Raspberry Pi 4.

### 3.2 Profile 1: `streaming-main` (TC_PROFILE_STREAMING_MAIN = 1)

**Target**: Streaming delivery to smart TVs, tablets, laptops  
**Decoder complexity**: Moderate — adds tools that improve streaming
quality at modest decode cost.

**v1 tool flags signaled** (baseline-mobile tools +):
- `TC_TOOL_MULTI_REF` — Multi-reference inter prediction (SLOW preset)

| Tool | Supported |
|------|-----------|
| All baseline-mobile tools | ✅ |
| Context-adaptive entropy coding | ✅ (range-coded baseline; deeper specialization planned) |
| CfL chroma prediction | ✅ |
| Multiple reference frames (2–4) | ✅ |
| 6-tap interpolation | ✅ |
| Skip / merge modes | ✅ |
| Deringing filter | ❌ (PLANNED — Phase 7) |
| v2 SAO Band Offset | ✅ (v2 only; EO/restoration planned) |
| Rate control: CBR + capped VBR | ✅ |
| Scene cut detection | ✅ |
| Film grain synthesis | ❌ (PLANNED — Phase 7) |
| Bi-prediction | ❌ (profile-gated; archive-high/v1 extension path only) |
| Loop restoration | ❌ |
| Affine motion | ❌ |

**Rationale**: This is the primary commercial profile. It must
beat practical H.264 streaming encodes on bitrate at matched VMAF,
and approach HEVC-class compression on many content types.

### 3.3 Profile 2: `archive-high` (TC_PROFILE_ARCHIVE_HIGH = 2)

**Target**: Offline encoding, studio archive, post-production  
**Decoder complexity**: High — no real-time decode requirement on ARM.
Used on workstations and servers.

**v1 tool flags**: Same as streaming-main (additional tools PLANNED for later phases).

| Tool | Supported |
|------|-----------|
| All streaming-main tools | ✅ |
| Bi-prediction (B-frames) | ✅ baseline infrastructure; broad gain not established |
| 8-tap interpolation | ❌ (PLANNED — Phase 6) |
| Loop restoration | ❌ (PLANNED — Phase 7) |
| Affine motion | ❌ (PLANNED — Phase 6+) |
| Extended partition types | ❌ (PLANNED — Phase 5) |
| Scaling matrices | ❌ (PLANNED — Phase 4) |
| Encoder-side learned guidance | ❌ (PLANNED) |
| Deep lookahead | ❌ (PLANNED — Phase 8) |

**Rationale**: Offline encoding can afford much higher encoder
complexity. The decoder runs on capable hardware. The goal is
maximum compression quality, not real-time ARM decode.

### 3.4 Profile 3: `grain-cinema` (TC_PROFILE_GRAIN_CINEMA = 3)

**Target**: Movie and prestige TV streaming with film grain  
**Decoder complexity**: Similar to streaming-main plus grain synthesis.

**v1 tool flags**: Same as streaming-main (grain tools PLANNED for Phase 7).

| Tool | Supported |
|------|-----------|
| All streaming-main tools | ✅ |
| Film grain synthesis | ❌ (PLANNED — Phase 7, required in profile) |
| Grain-aware quantization | ❌ (PLANNED — Phase 4) |
| Per-scene perceptual targeting | ❌ (PLANNED — Phase 8) |

**Rationale**: Film grain is one of the highest-leverage areas for
bandwidth savings in movie streaming. Coding grain literally is
extremely expensive. This profile mandates grain synthesis support
and encoder-side grain detection/removal.

---

## 4. Levels (Implemented in v1)

Levels constrain **memory, throughput, and resolution** to ensure
decoders can handle the bitstream within their resource budget.

**Implemented** in v1 bitstream. The decoder validates frame dimensions
and bitrate against the level table in `bitstream.c`. Level index is
signaled in the `profile_level` header byte (lower 4 bits).

### 4.1 Level Parameters

Each level defines limits on:

| Parameter | Description |
|-----------|-------------|
| Max picture width | Pixels |
| Max picture height | Pixels |
| Max DPB slots | Reference frames stored simultaneously |
| Max bitrate | Coded bits per second |

### 4.2 Level Table (Implemented in `bitstream.c`)

| Level | Index | Max Width | Max Height | Max DPB | Max Bitrate | Target Device |
|-------|-------|-----------|------------|---------|-------------|---------------|
| Auto | 0 | 4096 | 4096 | 8 | Unlimited | No constraints |
| 1.0 | 1 | 320 | 240 | 1 | 500 kbps | IoT / very low power |
| 1.1 | 2 | 640 | 480 | 2 | 2 Mbps | Raspberry Pi 2 |
| 2.0 | 3 | 1280 | 720 | 2 | 5 Mbps | Raspberry Pi 4 |
| 2.1 | 4 | 1280 | 720 | 4 | 10 Mbps | Mid-range Android |
| 3.0 | 5 | 1920 | 1080 | 4 | 20 Mbps | Raspberry Pi 5, good phone |
| 3.1 | 6 | 1920 | 1080 | 4 | 40 Mbps | High-end phone, tablet |
| 4.0 | 7 | 3840 | 2160 | 4 | 80 Mbps | Smart TV, laptop |
| 4.1 | 8 | 3840 | 2160 | 8 | 160 Mbps | Desktop, server |

**Decoder validation**: When a v1 bitstream has a level other than Auto,
the decoder checks that frame dimensions don't exceed the level's max
width/height. CRC mismatch is also detected and reported via
`tc_decoder_crc_valid()`.

**Notes**:
- Level 3.0 and 3.1 share the same sample rate but differ in bitrate ceiling
- DPB slots may be further constrained by profile (baseline-mobile: 1–2)
- These levels are provisional and must be validated with real decode
  measurements on target hardware (Phase 9)

---

## 5. Encoder Presets

Presets control **encoder search effort and tool exploration depth**.
They do not affect which tools the decoder must support (that's the
profile's job). Presets are **already implemented** in the codebase.

### 5.1 Defined Presets

| Preset | Value | Search Range | Description |
|--------|-------|-------------|-------------|
| `TC_PRESET_ULTRAFAST` | 0 | 16 px | Minimal search, fastest encode |
| `TC_PRESET_FAST` | 1 | 32 px | Moderate search, good speed |
| `TC_PRESET_MEDIUM` | 2 | 32 px | Default balance |
| `TC_PRESET_SLOW` | 3 | 64 px | Deep search, best quality |
| `TC_PRESET_VERYSLOW` | — | 128 px | **NOT YET IMPLEMENTED.** Research/offline mode from Master Plan. |

### 5.2 What Presets Currently Affect

The only encoder behavior that currently varies by preset is **motion
search range**:

```c
int search_range = 32;                              // default (medium/fast)
if (enc->cfg.preset == TC_PRESET_ULTRAFAST) search_range = 16;
if (enc->cfg.preset == TC_PRESET_SLOW)      search_range = 64;
```

**NOTE**: The Master Plan defines a 5th preset, `veryslow-research`,
which is not yet implemented in the codebase. It will be added as
`TC_PRESET_VERYSLOW` (value 4) when the encoder gains RDO, lookahead,
and deep search capabilities.

### 5.3 What Presets Should Affect (Future)

| Behavior | Ultrafast | Fast | Medium | Slow | Veryslow |
|----------|-----------|------|---------|------|----------|
| Motion search range | 16 px | 32 px | 32 px | 64 px | 128 px |
| Sub-pel refinement | None | Quarter-pel | Quarter-pel | Eighth-pel |
| Intra mode search | DC only | Top-3 modes | All 9 modes | All 9 + RDO |
| Transform size search | 8×8 always | Variance-based | Variance-based | Full RD select |
| Rate-distortion optimize | No | No | Fast prune | Full search |
| Lookahead frames | 0 | 0 | 5 | 15+ |
| Scene cut detection | Off | On | On | On + adaptive |
| Keyframe placement | Fixed interval | Fixed interval | Content-adaptive | Optimal |
| Quantization granularity | Frame-level | Frame-level | CTU-level | Block-level |
| AQ / perceptual masking | Off | Off | Basic | Full |
| Thread count | 1 | Auto | Auto | Auto |
| B-frame decision | Off | Off | Off | On (if profile allows) | On (if profile allows) |

### 5.4 Preset–Profile Interaction

- Encoder presets choose **how hard to search**
- Decoder profiles choose **which tools are available to search over**
- A slow preset with baseline-mobile profile will search deeply within
  the mobile tool subset
- A medium preset with archive-high profile will use all tools but
  with moderate search effort

This separation is essential: it means mobile decoders never need to
handle tools outside their profile, regardless of what preset the
encoder used.

---

## 6. Profile Signaling (Implemented in v1)

The v1 frame header includes profile/level and tool flags. See BITSTREAM.md §3.2
for the full header layout.

### 6.1 Header Fields (v1)

```
Byte Offset  Field           Description
───────────  ──────────────  ──────────────────────
0–2          magic           0x54 0x43 0x56 ("TCV")
3            version         0x01 (version 1)
4–5          width           Frame width (16-bit LE)
6–7          height          Frame height (16-bit LE)
8            flags           Bitfield: key(1) wpp(1) rap(1) crc(1) ext(1) tile_c(2) tile_r(2)
9            qp_delta        Signed offset from QP 32
10           frame_num       Frame counter (low 8 bits)
11           profile_level   (profile << 4) | (level_idx & 0x0F)
12–13        tool_flags      16-bit tool flag bitfield
```

### 6.2 Tool Flags (v1, 16-bit)

Signaled in every v1 frame. Indicates which coding tools are **actually used**
by the encoder for this frame.

```
Bit   Field                  Profile Required       Constant
───   ─────────────────────  ───────────────        ───────────────
0     skip_merge              baseline-mobile+       TC_TOOL_SKIP_MERGE
1     cfl_chroma              baseline-mobile+       TC_TOOL_CFL_CHROMA
2     jnd_weighting           baseline-mobile+       TC_TOOL_JND_WEIGHTING
3     median_mv_pred          baseline-mobile+       TC_TOOL_MEDIAN_MV_PRED
4     multi_ref               streaming-main+       TC_TOOL_MULTI_REF
5     six_tap_interp          streaming-main+       TC_TOOL_SIX_TAP_INTERP
6     entropy_coded           streaming-main+       TC_TOOL_ENTROPY_CODED
7     deringing               streaming-main+       (PLANNED Phase 7)
8     sao                     streaming-main+       TC_TOOL_SAO (v2 luma BO)
9     grain_synthesis         grain-cinema           (PLANNED Phase 7)
10    bipred                  archive-high          TC_TOOL_BIPRED
11    loop_restoration        archive-high          (PLANNED Phase 7)
12    affine_motion           archive-high          (PLANNED Phase 6+)
13    extended_partition      archive-high          (PLANNED Phase 5)
14    context_reset           —                     TC_TOOL_CONTEXT_RESET
15    reserved                —                      Must be 0
```

A decoder for profile N silently ignores tool flags that are not in its
profile (it must not encounter them if the encoder is compliant).

### 6.3 Flags Field (v1 additions)

The flags byte in v1 adds two new bits:

```
Bit   Field     Description
───   ───────   ──────────────────────────────────────────────
2     rap       Random Access Point — frame can be decoded independently
3     crc       CRC-16 appended after frame data for error detection
4     ext       Extended header follows (future use, not yet implemented)
```

---

## 7. Backward Compatibility Strategy

When new tools or profiles are introduced:

1. **Bitstream version increments** — v0 decoders reject v1+; v1-aware decoders accept v0/v1; v2-aware decoders explicitly dispatch v0, v1, or v2 and reject v3+
2. **New profile values** — decoders that only know profile 0–3 reject unknown profiles
3. **Tool flags** — decoders only parse tools their profile supports; unknown flag bits are ignored
4. **Optional syntax sections** — guarded by flag bits; decoders that don't support a tool skip its syntax section entirely
5. **v0 → v1 → v2 migration** — v0 frames (12-byte header) decode correctly on v1/v2-aware decoders; v1 frames retain their 14-byte syntax; v2 requires the explicit v2 decoder path and is never silently treated as v1

This ensures that:
- A baseline-mobile decoder never needs to understand archive-high syntax
- A new tool can be added without breaking old decoders
- Bitstream conformance can be tested per-profile

---

## 8. Conformance Testing (Proposed)

For each profile, a conformance test suite should verify:

1. **Decode correctness**: All conformance bitstreams decode to
   reference outputs within tolerance
2. **Error handling**: Malformed bitstreams are rejected cleanly
3. **Resource limits**: Level constraints are respected (memory,
   throughput)
4. **Tool subset**: No tools outside the profile are exercised
5. **Interoperability**: Bitstreams from different encoder versions
   produce identical decoder output (deterministic)

Conformance bitstreams should be organized as:

```
conformance/
  baseline-mobile/
    level-1.0/
    level-2.0/
    ...
  streaming-main/
    level-2.0/
    level-3.0/
    ...
  archive-high/
    level-4.0/
    ...
  grain-cinema/
    level-3.0/
    ...
```

Each directory should contain:
- Source YUV files
- Encoded bitstreams
- Expected decoded output (MD5 hashes or binary frames)
- Encoding parameters used
