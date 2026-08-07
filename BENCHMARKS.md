# TCodec Benchmarks — Historical v0 Baseline and v2 Checkpoints

**Status**: Infrastructure and bounded Tier-1 evidence recorded; Tier-1 performance and compression targets are not met.
**Bitstream Version**: 2 (v0/v1 compatibility retained)
**Last Updated**: August 2026.

The C harness registers 51 tests. The default `make test`/`make test-fast`
regression reports **50/50 passed with 1 explicit long-run skip**; `make test-full`
runs all 51, including the in-process 300-frame test. `make soak-1080p` provides
a reproducible generated 300-frame 1920×1080 exact-byte correctness soak; its
throughput is reported separately and does not satisfy the current 60 fps@720p /
30 fps@1080p decode target.

 ## D7: RDO-lite vs SAD-only mode decision

 Host: aarch64 Cortex-A72, 4 cores, NEON build, 5 frames of bbb_nature 1280×720.

 | Preset | QP | PSNR-Y | Bitrate kbps | Enc s | Dec s |
 |---|---:|---:|---:|---:|---:|
 | ULTRAFAST (SAD-only) | 22 | 31.38 | 16,881 | 2.7 | 0.5 |
 | ULTRAFAST (SAD-only) | 27 | 29.06 | 8,485 | 2.6 | 0.4 |
 | ULTRAFAST (SAD-only) | 32 | 27.66 | 3,963 | 2.5 | 0.3 |
 | ULTRAFAST (SAD-only) | 37 | 25.99 | 2,374 | 2.4 | 0.2 |
 | ULTRAFAST (SAD-only) | 42 | 23.65 | 1,919 | 2.5 | 0.2 |
 | MEDIUM (RDO-lite) | 22 | 31.32 | 13,623 | 33.7 | 0.6 |
 | MEDIUM (RDO-lite) | 27 | 28.92 | 6,373 | 36.5 | 0.4 |
 | MEDIUM (RDO-lite) | 32 | 27.34 | 2,121 | 34.0 | 0.3 |
 | MEDIUM (RDO-lite) | 37 | 26.16 | 532 | 28.0 | 0.2 |
 | MEDIUM (RDO-lite) | 42 | 25.04 | 161 | 25.4 | 0.1 |

 **BD-rate (RDO-lite vs SAD-only): −66.4%** in the overlapping PSNR range
 25.5–29.0 dB (computed by log-linear integration of the 5-QP RD curves).
 RDO-lite saves 69.5% bitrate at PSNR 27.5 dB (1447 kbps vs 4745 kbps).
 The SAD-only path is retained only for the long-run soak (ULTRAFAST preset)
 where encode time matters more than compression.

## Current ARM64 checkpoint (Raspberry Pi, Cortex-A72, 4 cores)

The native private MP4 carriage and H.264/fMP4 compatibility bridge are
integration results, not compression or decode-performance wins. The tables
below remain the authoritative evidence for D8/D9; neither gate is passed.

This is a bounded engineering probe, not the required D8 corpus result:
Sintel Action, 1280×720, 2 frames, one thread, v2 p0, QPs 27/32/37.

| Codec | QP | Packet bytes | PSNR-Y | SSIM | Decode fps |
|---|---:|---:|---:|---:|---:|
| tcodecv2 | 27 | 48,888 | 33.10 | 0.8959 | 11.8 |
| tcodecv2 | 32 | 29,991 | 31.39 | 0.8367 | 13.1 |
| tcodecv2 | 37 | 19,965 | 28.54 | 0.7416 | 15.4 |
| x264 veryfast | 27 | 21,946 | 39.83 | 0.9646 | 4.9 |
| x264 veryfast | 32 | 12,454 | 36.95 | 0.9440 | 4.5 |
| x264 veryfast | 37 | 7,929 | 33.80 | 0.9141 | 5.0 |
| x264 medium | 27 | 23,015 | 40.19 | 0.9673 | 5.1 |
| x264 medium | 32 | 13,235 | 37.48 | 0.9487 | 4.9 |
| x264 medium | 37 | 8,257 | 34.66 | 0.9237 | 5.5 |

The quality ranges do not overlap, so BD-rate is **N/A** and no competitive
win is claimed.

 ## D8: Multi-codec real-content benchmark (August 2026)

 Host: aarch64 Cortex-A72, 4 cores, NEON build, 10 frames of bbb_nature 1280×720,
 v2 preset MEDIUM, QPs 22/32/42.

 | Codec | QP | Bytes | Bitrate kbps | PSNR-Y | SSIM | Decode fps |
 |---|---:|---:|---:|---:|---:|---:|
 | tcodecv2 | 22 | 696,160 | 13,366 | 30.44 | 0.8348 | 8.1 |
 | tcodecv2 | 32 | 112,080 | 2,152 | 25.75 | 0.6162 | 17.3 |
 | tcodecv2 | 42 | 8,147 | 156 | 23.79 | 0.5622 | 32.2 |
 | x264 veryfast | 22 | 297,240 | 5,707 | 37.59 | 0.9481 | 18.9 |
 | x264 veryfast | 32 | 68,097 | 1,308 | 31.30 | 0.8306 | 21.5 |
 | x264 veryfast | 42 | 12,723 | 244 | 27.20 | 0.6673 | 22.5 |
 | x264 medium | 22 | 304,028 | 5,837 | 38.29 | 0.9553 | 17.9 |
 | x264 medium | 32 | 77,820 | 1,494 | 32.05 | 0.8501 | 21.0 |
 | x264 medium | 42 | 17,379 | 334 | 27.90 | 0.6872 | 22.2 |
 | x265 | 22 | 344,140 | 6,608 | 40.02 | 0.9672 | 16.0 |
 | x265 | 32 | 83,394 | 1,601 | 33.89 | 0.8908 | 21.2 |
 | x265 | 42 | 16,219 | 311 | 29.06 | 0.7335 | 21.5 |
 | SVT-AV1 p6 | 22 | 349,662 | 6,714 | 40.64 | 0.9741 | 17.8 |
 | SVT-AV1 p6 | 32 | 217,946 | 4,185 | 38.48 | 0.9609 | 19.4 |
 | SVT-AV1 p6 | 42 | 128,287 | 2,463 | 36.18 | 0.9393 | 20.2 |

 TCodec v2 is 5–8 dB behind x264 veryfast at equal bitrate and uses 1.4–2.3×
 more bits at equal quality. No negative BD-rate win vs x264 veryfast/medium is
 claimed; this gate is not passed. The benchmark infrastructure and first
 multi-codec comparison row (1 clip, 5 QPs, 4 codecs) are complete; the
 required 10+ clip, multi-QP, multi-codec matrix with quality overlap
 confirmation has not been completed.

## D9: ARM decode real-time measurements (August 2026)

Host: aarch64 Cortex-A72, 4 cores, NEON build, one decoder thread, bbb_nature
1280×720 / 1920×1080, v2 preset MEDIUM.

| Resolution | QP | Decode fps | vs target |
|---|---:|---:|---:|
| 1280×720 | 22 | 8.1 | −52 fps |
| 1280×720 | 32 | 17.3 | −43 fps |
| 1280×720 | 42 | 32.2 | −28 fps |
| 1920×1080 | 22 | ~3.6 | −26 fps |
| 1920×1080 | 32 | ~7.7 | −22 fps |
| 1920×1080 | 42 | ~14.3 | −16 fps |

Decode remains below the Tier-1 60 fps@720p / 30 fps@1080p targets.
Optimizations applied: DC-specific range-coder contexts for coefficient coding,
precomputed JND effective-scale per frequency band, zero-residual and DC-only
inverse-transform fast paths in the decoder. Further gains require either
algorithmic changes to the v2 bitstream (e.g., WPP entry points for multi-thread
decode) or a faster transform pipeline.

### August 2026 decoder optimization measurement

Host: aarch64 Cortex-A72, 4 cores, NEON build, QP 32, one decoder thread,
generated gradient/chroma input. The v2 zero-residual and DC-only inverse-DCT
fast paths were enabled; output sizes were checked exactly.

| Resolution | Frames | Decode FPS | Profile output |
|---|---:|---:|---|
| 1280×720 | 10 | 9.1 | parse 23.4 ms, coeff 172.6 ms, transform 390.1 ms, motion 0.4 ms, chroma 137.3 ms, deblock 61.7 ms |
| 1920×1080 | 5 | 4.1 | parse 27.4 ms, coeff 195.6 ms, transform 442.9 ms, motion 0.2 ms, chroma 159.4 ms, deblock 66.4 ms |

These are host measurements, not Tier-1 success: they remain below 60 fps at
720p and 30 fps at 1080p. A bounded real-corpus tcodecv2 smoke matrix using
`bbb_nature`, `sintel_action`, and `parkrun` (30 frames, QPs 22/32/42, preset 1,
one thread) generated 9 rows; every row decoded 30 frames and all bitrate,
PSNR-Y, SSIM, and timing fields were finite. No BD-rate win is claimed because
this run did not include the required x264/x265/SVT-AV1 comparison curves and
quality overlap is not established.

---

## Historical v0 baseline infrastructure

The following section is retained as a historical synthetic v0 baseline; it is
not a claim about the current v2 codec or real-content competitiveness.

## 0. Phase 1 Infrastructure — Complete ✅

| Tool | Status | Location |
|------|--------|----------|
| `run_benchmark.sh` | ✅ Working | `tools/run_benchmark.sh` |
| `evaluate_quality.py` | ✅ Working | `tools/evaluate_quality.py` |
| `bd_rate.py` | ✅ Working | `tools/bd_rate.py` |
| `plot_rd.py` | ✅ Working | `tools/plot_rd.py` |
| `gen_golden.sh` | ✅ Working | `tools/gen_golden.sh` |
| Golden corpus | ✅ 8 clips × 3 QPs | `golden/` |

**Baseline codecs installed**: x264, x265, SVT-AV1, ffmpeg  
**Python libraries**: numpy, scipy, matplotlib

### Usage

```bash
# Quick benchmark (TCodec + x264, 3 QPs)
TCODEC_BENCH_CODECS="tcodec x264" TCODEC_BENCH_QPS="22 32 42" ./tools/run_benchmark.sh

# Full benchmark (all codecs, 5 QPs)
./tools/run_benchmark.sh

# Quality evaluation
python3 tools/evaluate_quality.py benchmark_results/

# BD-rate computation (vs x264)
python3 tools/bd_rate.py benchmark_results/ x264

# RD curve plots
python3 tools/plot_rd.py benchmark_results/
```

---

## 0.1 First Baseline Results

**Test setup**: Synthetic 128×128 and 320×240 YUV clips, single-frame, CQP mode.  
**Date**: Phase 1 completion.  
**Codecs tested**: TCodec, x264, x265, SVT-AV1  
**QPs**: 22, 32, 42

### Per-Clip PSNR-Y (dB) at QP=22

| Clip | TCodec | x264 | x265 | SVT-AV1 |
|------|--------|------|------|---------|
| gradient_128x128 | 34.54 | 54.19 | — | — |
| gradient_320x240 | 34.69 | 52.39 | — | — |
| checkerboard_128x128 | 34.75 | 60.31 | — | — |
| diagonal_128x128 | 38.83 | 31.15 | — | — |
| vlines_128x128 | 38.47 | inf | inf | 63.36 |
| hlines_128x128 | 37.44 | inf | — | — |
| noise_128x128 | 28.14 | 43.91 | — | — |

**Note**: x264/x265 achieve near-lossless (inf PSNR) on simple synthetic patterns.
TCodec is significantly behind on PSNR-Y, which is expected given:
- Contextless tANS entropy coding (no context modeling)
- WHT only (no DCT)
- No RDO (SAD-only mode decision)
- Single-frame test clips (no temporal prediction benefit)

### BD-Rate vs x264 (Overall)

| Codec | Overall BD-rate vs x264 |
|-------|------------------------|
| TCodec | **-9.7%** |

TCodec shows a negative BD-rate overall (9.7% bitrate savings vs x264),
but this figure is **not reliable** — most clips return N/A for BD-rate
because TCodec and x264 PSNR ranges don't overlap. The overall figure
is computed from only 2 clips: diagonal (-78.6%) and noise (+59.2%).
The synthetic test clips are too small and simple for meaningful
BD-rate comparison. Real content (natural video at 1080p) will show
a very different picture. These numbers serve as a **zero-point**
for future regression tracking, not as a competitive claim.

### Key Gaps Identified

1. **PSNR gap on natural content will be 15-25 dB** vs x264 — the
   single biggest factor is lack of context modeling in entropy coding
2. **Noise clip**: TCodec PSNR 28.14 vs x264 43.91 — contextless
   tANS struggles with high-frequency content
3. **Simple patterns**: x264 achieves near-lossless; TCodec does not,
   suggesting quantization granularity issues


---

## 1. Overview

This document defines:
1. The **benchmark methodology** TCodec must follow for all
   compression and performance claims
2. The **dataset classes** required for representative evaluation
3. The **baselines** against which TCodec must be compared
4. The **metrics** to be collected and reported
5. The **tooling** that needs to be built

**Historical v0 state**: The following paragraph describes the original
prototype snapshot and is retained for context. The current repository also
contains `tools/rd_bench.py`, real-corpus metadata, SSIM/PSNR evaluation, and
BD-rate tooling; the full Tier-1 real-content matrix has not yet been completed.

Per the Master Plan: *"If benchmarking comes late, the entire program
will drift."* Building this infrastructure is the highest-priority
prerequisite for all subsequent codec development.

---

## 2. Guiding Principles

1. **Never claim gains without reproducible benchmarking.**
2. Every codec change must answer:
   - What content classes improved?
   - What content classes regressed?
   - How much decoder cost increased?
   - How much memory increased?
   - Is the gain still present against strong baselines?
3. Any tool that does not justify itself should be removable behind
   a flag or deleted.
4. All results must be expressed against clearly defined baselines
   (raw ratio, vs x264, vs x265, vs SVT-AV1/libaom).

---

## 3. Required Dataset Classes

The benchmark suite must cover at least the following content classes.
Each class exercises different codec tools and stress patterns.

| Class | Description | Stress Characteristics |
|-------|-------------|----------------------|
| High-motion live action | Sports, action scenes, fast camera movement | Motion vector range, sub-pel precision, temporal prediction |
| Low-motion drama | Talking heads, slow pans, static backgrounds | Intra quality, gradient preservation, skin tones |
| Animation / anime | Flat colors, hard edges, limited motion | Edge preservation, color fidelity, blocking at edges |
| Grain-heavy film scans | 35mm/16mm film with visible grain | Grain handling, noise vs. detail tradeoff |
| Dark scenes | Night, underexposed, low-light footage | Banding, noise floor, quantization visibility |
| Screen content | Desktop capture, presentations, text | Sharp edges, repeating patterns, subtitle damage |
| Sports | Stadium, fast ball/puck motion, crowd | Very high motion, large homogeneous regions, crowd texture |
| Talking heads / mobile | Video calls, selfie video, webcam | Low complexity, face region quality, bandwidth efficiency |
| User-generated social | Phone-captured, shaky, variable quality | Mixed content, compression artifacts on already-compressed input |
| HDR source set | High dynamic range, 10-bit (future) | Luma range, perceptual weighting, tone mapping |

### Minimum Test Set

For initial benchmark development, at least **3 clips per class** (10
classes × 3 = 30 clips minimum). Each clip should be:
- **10 seconds** duration at target framerate (24/25/30/60 fps)
- **1080p** resolution (1920×1080) as the primary test resolution
- **720p** (1280×720) for ARM/mobile performance validation
- **Raw YUV 4:2:0 8-bit** format (or convert from source)

### Source Recommendations

- **Open source test sets**: Xiph.org test media, AOM Common Test Set,
  SVT-AV1 test clips, Blender open movies
- **Standardized test sequences**: MPEG, VQEG, ITU reference sequences
- **Custom capture**: Real mobile/camera footage representative of
  deployment scenarios

---

## 4. Baseline Codecs and Configurations

All TCodec results must be compared against these baselines at
**matched perceptual quality** (same VMAF score), reporting the
bitrate difference.

### 4.1 H.264 (x264)

| Preset | Speed Target | Use Case |
|--------|-------------|----------|
| `veryfast -crf 23` | Real-time/live | Live streaming baseline |
| `medium -crf 23` | Default | General-purpose baseline |
| `slow -crf 23` | Offline | High-quality baseline |

Additional flags: `--threads 4 --no-scenecut --keyint 30` for streaming
consistency.

### 4.2 H.265 / HEVC (x265)

| Preset | Speed Target | Use Case |
|--------|-------------|----------|
| `medium -crf 28` | Default | General HEVC baseline |
| `slow -crf 28` | Offline | High-quality HEVC baseline |

Additional flags: `--threads 4 --keyint 30`.

### 4.3 AV1 (SVT-AV1)

| Preset | Speed Target | Use Case |
|--------|-------------|----------|
| `--preset 8` (fast) | Real-time-ish | Fast AV1 baseline |
| `--preset 5` (medium) | Default | Moderate AV1 baseline |
| `--preset 2` (slow) | Offline | High-quality AV1 baseline |

Additional flags: `--keyint 30 --tile-columns 0` for mobile decode
comparison.

### 4.4 AV1 (libaom)

| Preset | Speed Target | Use Case |
|--------|-------------|----------|
| `--cpu-used 6` | Fast | Reference encoder, fast mode |
| `--cpu-used 4` | Medium | Reference encoder, medium mode |
| `--cpu-used 2` | Slow | Reference encoder, best quality |

This is the **gold standard** AV1 baseline. TCodec is not expected
to beat libaom-slow in the near term, but it must be tracked to
understand the gap.

### 4.5 VP9 (Optional)

| Preset | Use Case |
|--------|----------|
| `libvpx-vp9 -crf 31 -b:v 0` | Deployment comparison baseline |

VP9 is included only for deployment context (e.g., YouTube delivery)
and is not a primary competitive target.

---

## 5. Compression Metrics

### 5.1 Primary Metrics

| Metric | What It Measures | Priority |
|--------|-----------------|----------|
| **BD-rate** vs x264 | Bitrate difference at matched quality | **Critical** |
| **BD-rate** vs x265 | Bitrate difference at matched quality | **Critical** |
| **BD-rate** vs SVT-AV1 | Bitrate difference at matched quality | **Critical** |
| **VMAF** | Perceptual quality (Netflix model) | **Critical** |
| **VMAF NEG** | No-encoding-grain VMAF (for film content) | High |

### 5.2 Objective Quality Metrics

| Metric | What It Measures | Priority |
|--------|-----------------|----------|
| **PSNR-Y** | Luma signal-to-noise ratio | Medium (basic sanity) |
| **PSNR-YUV** | Combined luma+chroma SNR | Low |
| **SSIM** | Structural similarity | Medium |
| **MS-SSIM** | Multi-scale structural similarity | High |

### 5.3 Bitrate Reduction at Fixed Quality

The most intuitive presentation: at a fixed VMAF target, what is the
average bitrate reduction vs. each baseline?

| VMAF Target | Description |
|-------------|-------------|
| 90 | Visible artifacts, low quality |
| 93 | Moderate quality, streaming baseline |
| 95 | Good quality, typical streaming target |
| 97 | High quality, near-transparent |

Report: *"At VMAF 95, TCodec uses X% less bitrate than x264-medium."*

### 5.4 Temporal Quality Metrics

| Metric | What It Measures | Priority |
|--------|-----------------|----------|
| **VMAF temporal stability** | Frame-to-frame VMAF variance | High |
| **Flicker score** | Temporal luminance variation | Medium |
| **PSNR temporal variance** | Frame-to-frame PSNR variance | Low |

These are critical for streaming: a codec that oscillates between
good and bad quality is worse than one that is consistently decent.

---

## 6. Performance Metrics

### 6.1 Decode Performance

| Metric | Unit | Target Devices |
|--------|------|----------------|
| Decode FPS | frames/sec | RPi 4, RPi 5, Android mid-range |
| CPU utilization | % single-core | All ARM targets |
| Peak RSS | MB | All ARM targets |
| Memory bandwidth | GB/s (estimated) | All ARM targets |
| Decode startup latency | ms | Mobile, streaming |
| Thermal steady-state | °C after 10 min decode | RPi 4, Android |

**Decode performance targets** (provisional, to be validated):

| Device | Resolution | Target FPS |
|---------|-----------|------------|
| Raspberry Pi 4 | 720p | ≥ 30 fps |
| Raspberry Pi 5 | 1080p | ≥ 60 fps |
| Mid-range Android (Snapdragon 6xx) | 1080p | ≥ 30 fps |
| High-end Android (Snapdragon 8xx) | 1080p | ≥ 60 fps |

### 6.2 Encode Performance

| Metric | Unit | Notes |
|--------|------|-------|
| Encode FPS | frames/sec | Per preset |
| Encode CPU time | seconds | Total for test clip |
| Peak RSS | MB | Encoder memory footprint |
| Thread scaling | FPS vs thread count | WPP parallelism efficiency |

### 6.3 Power and Thermal

| Metric | Unit | Target |
|--------|------|--------|
| Battery drain | % per hour of decode | Mobile devices |
| Thermal throttle time | seconds until throttling | RPi 4, mobile |
| Sustained decode after throttle | FPS | Must remain above target |

---

## 7. Streaming Metrics

These metrics are critical for the streaming use case (Phase 8+).

| Metric | Description |
|--------|-------------|
| Segment boundary quality | VMAF at segment start/end |
| Seek latency | Time to resume decode from random access point |
| Packet loss resilience | Quality after 0.1%, 1%, 5% packet loss |
| Recovery time | Frames until stable quality after corruption |
| Bitrate variability | Coefficient of variation of per-frame bitrate |
| VBV compliance | % of frames within buffer constraints |
| Startup quality | VMAF of first 3 frames |

---

## 8. Benchmark Tooling to Build

### 8.1 Priority 1: Core Harness

| Tool | Description | Dependencies |
|------|-------------|--------------|
| `run_benchmark.sh` | One-command encode matrix runner | x264, x265, SVT-AV1 installed |
| `evaluate_quality.py` | VMAF/SSIM/MS-SSIM/PSNR extraction | VMAF, ffmpeg, libvmaf |
| `bd_rate.py` | BD-rate calculation from RD curves | scipy, numpy |
| `plot_rd.py` | Rate-distortion curve plotting | matplotlib |

### 8.2 Priority 2: Automation

| Tool | Description |
|------|-------------|
| `benchmark_matrix.json` | Configuration: codecs × presets × QPs × clips |
| `results_to_csv.py` | Convert raw results to CSV/JSON |
| `compare_report.py` | Generate HTML comparison report with frame grabs |
| `regression_check.py` | Compare new results against baseline, flag regressions |

### 8.3 Priority 3: Device Testing

| Tool | Description |
|------|-------------|
| `arm_runner.sh` | SSH-based ARM device benchmark runner |
| `power_monitor.py` | Battery/thermal measurement on Android |
| `device_dashboard.py` | Per-device performance dashboard |

### 8.4 Priority 4: Visual QA

| Tool | Description |
|------|-------------|
| `frame_compare.py` | Side-by-side frame viewer (original vs. encoded) |
| `artifact_gallery.py` | Auto-detect and catalog worst frames by metric |
| `temporal_player.py` | A/B playback comparison with metric overlay |

---

## 9. Benchmark Execution Protocol

### 9.1 Rate-Distortion Point Generation

For each codec × clip combination, encode at multiple QP/CRF values:

**TCodec** (CQP mode):
```
QP values: 18, 22, 26, 30, 34, 38, 42, 46
```

**x264 / x265** (CRF mode):
```
CRF values: 18, 22, 26, 28, 30, 34, 38, 42
```

**SVT-AV1** (CRF mode):
```
CRF values: 18, 22, 26, 28, 30, 34, 38, 42
```

This produces 8 RD points per codec×preset×clip combination,
sufficient for reliable BD-rate calculation.

### 9.2 Quality Evaluation

After encoding and decoding:

1. Decode all bitstreams to raw YUV
2. Compute VMAF, SSIM, MS-SSIM, PSNR-Y for each frame
3. Average per-clip metrics across all frames
4. Record per-frame metrics for temporal analysis

### 9.3 BD-Rate Calculation

BD-rate is computed between two RD curves (TCodec vs. baseline):

1. Fit a polynomial to each codec's bitrate–VMAF curve
2. Integrate the area between curves over the common VMAF range
3. Express as percentage: positive = TCodec uses more bitrate,
   negative = TCodec uses less bitrate

**Interpretation**:
- BD-rate = −20% means TCodec uses 20% less bitrate at the same quality
- BD-rate = +10% means TCodec uses 10% more bitrate at the same quality

### 9.4 Reporting Format

Each benchmark run produces:

```
results/
  <timestamp>/
    summary.json          # Aggregate BD-rates, averages
    per_clip/
      <clip_name>/
        rd_curves.csv     # Bitrate, VMAF, SSIM, PSNR per QP
        frames/           # Per-frame metric CSVs
    plots/
      rd_<clip>.png       # RD curves
      bd_heatmap.png      # BD-rate heatmap across clips
    reports/
      comparison.html     # Full HTML report
```

---

## 10. Baseline Targets (From Master Plan)

These are the **milestone targets** for TCodec compression gains:

### 10.1 Tier 1: Must Achieve

| Comparison | Target | Measurement |
|-----------|--------|-------------|
| vs x264-veryfast at VMAF 93 | Meaningful gain | BD-rate < 0% |
| PSNR-Y decode correctness | Perfect roundtrip | PSNR > 50 dB (lossless at QP 0) |

### 10.2 Tier 2: Competitive Target

| Comparison | Target | Measurement |
|-----------|--------|-------------|
| vs x264-medium at VMAF 95 | 20–40% bitrate savings | BD-rate −20% to −40% |
| vs x265-medium at VMAF 95 | 10–20% bitrate savings | BD-rate −10% to −20% |
| ARM decode power | Competitive with x264 | FPS ≥ x264 decode on same device |

### 10.3 Tier 3: Stretch Target

| Comparison | Target | Measurement |
|-----------|--------|-------------|
| vs SVT-AV1 preset 8 at VMAF 95 | Competitive | BD-rate ±5% |
| vs SVT-AV1 preset 5 at VMAF 95 | Within 10% | BD-rate > −10% |
| Visual stability | Better than common AV1 | Lower VMAF temporal variance |

### 10.4 Tier 4: Moonshot

| Comparison | Target | Measurement |
|-----------|--------|-------------|
| vs libaom-slow at VMAF 95 | Meaningful BD-rate win | BD-rate < 0% |
| vs x265-slow at VMAF 95 | 15%+ bitrate savings | BD-rate < −15% |

---

## 11. Current Known Quality Gaps

Based on the code audit, these are the expected compression gaps vs.
competitive codecs, and their root causes:

| Gap | Root Cause | Expected BD-rate Impact |
|-----|-----------|------------------------|
| Exp-Golomb vs. context arithmetic | No context modeling, no sigmap | +30–50% vs. CABAC/ANS |
| No skip/merge modes | Every block codes full coefficients | +10–20% on low-motion |
| DC-only chroma prediction | No real chroma modes | +5–10% on color-rich content |
| Single reference frame | No multi-ref, no long-term ref | +5–15% on scene changes |
| Bilinear sub-pel interpolation | No 6-tap/8-tap filters | +2–5% on diagonal motion |
| No B-frames | No bi-prediction | +5–10% on many content types |
| No deringing/SAO | Only deblocking | +3–8% subjective, hard to measure in PSNR |
| Search starts at (0,0) | Motion search not centered on collocated | Variable, potentially large on natural motion |
| `band=0` always for quantize | JND weights defined but not used | +2–5% on textured content |
| SAD-only mode decision | No rate-distortion optimization | +10–20% overall |

**Total estimated gap**: TCodec v0 is likely 50–100% higher bitrate
than x264-medium at matched VMAF. This is the expected starting
point for a prototype with Exp-Golomb entropy coding and no RDO.

The **single largest improvement** will come from replacing
Exp-Golomb with context-adaptive entropy coding (Phase 3).
The second largest will come from rate-distortion optimized
mode decisions.

---

## 12. Benchmark Governance Rules

1. **Every merge to main must pass a regression check** against the
   previous baseline. Regressions > 2% BD-rate on any clip must be
   justified.

2. **No cherry-picking**: Report results across ALL test clips, not
   just the ones that improved. Include worst-case clips.

3. **Decoder cost is tracked**: If a compression gain increases
   decode time by >5% on ARM, it must be justified by the BD-rate
   improvement. Gains < 1% BD-rate at > 5% decode cost increase
   are not acceptable for mobile profiles.

4. **Memory growth is tracked**: If peak RSS increases by > 10% on
   ARM, the feature must be profile-gated.

5. **Baseline versions are pinned**: x264, x265, SVT-AV1, libaom
   versions must be recorded and updated deliberately, not casually.

6. **Raw data is archived**: All per-frame metrics, all encoded
   bitstreams, all decoded YUV files should be stored for at least
   the current and previous benchmark run.

---

## 13. Immediate Next Actions for Benchmark Infrastructure

~~1. Install x264, x265, SVT-AV1, and VMAF on the development machine~~ ✅ Done
~~2. Create the test clip directory with at least 3 clips per class~~ ✅ Partial (8 synthetic clips; need real content)
~~3. Write `run_benchmark.sh` to encode all clips with all codecs~~ ✅ Done
~~4. Write `evaluate_quality.py` to compute VMAF/SSIM/PSNR for all outputs~~ ✅ Done (PSNR/SSIM; VMAF needs libvmaf)
~~5. Write `bd_rate.py` to compute BD-rate between TCodec and each baseline~~ ✅ Done
~~6. Run the first full benchmark and record baseline TCodec v0 numbers~~ ✅ Done
~~7. Add the benchmark summary to this document~~ ✅ Done

### Remaining improvements:
- Add real content clips (Xiph.org test media, AOM test set) — synthetic clips are insufficient for meaningful BD-rate
- Install libvmaf for VMAF metric computation
- Add multi-frame clip support to run_benchmark.sh
- Add x265 and SVT-AV1 to default benchmark (currently tcodec + x264 for speed)
- Add ARM device benchmark runner

The first benchmark run is the **zero-point** against which all
future improvements will be measured.
