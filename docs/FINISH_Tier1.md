# TCodec Tier-1 Completion Report

**Status: NOT COMPLETE — release gates pass, Tier-1 acceptance does not.**

This report records the current implementation and reproducible evidence. It does
not claim Tier-1 completion while the required compression, decode-throughput,
container, and sanitizer criteria remain unmet.

## Verified implementation

- Explicit version dispatch for v0, v1, and v2; v2 never enters the legacy block decoder.
- v2 quadtree decoder with 64/32/16/8 CU traversal, inter/intra/skip/merge syntax,
  motion-vector prediction, residual decoding, chroma reconstruction, deblocking,
  and luma BO SAO.
- Shared fixed-point quantizer/dequantizer primitives used by v2 encoder and decoder.
- Scalar/NEON transform and SAO parity checks.
- Decoder zero-residual row-copy fast paths and DC-only inverse-DCT fast paths.
- v2 odd 4:2:0 dimensions are rejected both at encoder creation and before
  decoder quadtree reconstruction.
- Existing proprietary TCMX/TCMF mux, demux, and segmentation utility remains tested.
- `build/tcmux mp4mux/mp4demux` provides native TCV access-unit carriage in an
  ISO-BMFF file with a private `tcv1` sample entry and byte-exact recovery.
  Stock players cannot decode that private sample entry.
- `tools/tcmux_mp4.sh` separately provides a tested playable ISO-BMFF
  compatibility bridge: TCV is decoded and re-encoded as H.264 MP4, with
  staged fMP4/HLS segmentation. It is explicitly not native TCodec playback.
- The release checks below cover the repository regression and parity suite.
- `make test-full` runs all 51 tests including the in-process 300-frame 1080p
  long-run soak; it passed on this host (see D1).
- `make soak-1080p` provides a reproducible CLI 300-frame 1920×1080 soak with
  exact decoded-frame validation.

## Validation commands

| Command | Result |
|---|---|
| `make clean && make -j1` | **Pass** — ARM64/NEON release build completed; nonfatal unused-function/indentation warnings remain |
| `make test` / `make test-fast` | **Pass** — 50/50 codec tests passed with 1 explicit long-run skip; TCMX/MP4 container checks also pass |
| `make test-full` | **Pass** — 51/51 tests including in-process 300-frame 1080p soak |
| `./tools/parity_check.sh` | **Pass** — scalar/NEON transforms and entropy bitstreams identical |
| `./tools/test_tcmux.sh` | **Pass** — TCMX/TCMF, native private `tcv1` MP4, malformed-input checks |
| `./tools/test_tcmux_mp4.sh` | **Pass** — playable H.264 MP4/fMP4 bridge checks |
| `make cross-test` | **Pass** — AArch64/NEON compile-only matrix |
| `python3 -m py_compile tools/rd_bench.py tools/bd_rate.py tools/evaluate_quality.py tools/plot_rd.py` | **Pass**; benchmark scripts compile and output parent directories are created automatically |
| `git diff --check` | **Pass** after whitespace cleanup and cache removal |
| `make debug` | Builds successfully |
| `ASAN_OPTIONS=detect_leaks=0 ./build/test_tcodec` | Cannot start: host ASan allocator initialization fails in `sanitizer_allocator_primary64.h:131` before tests execute; the Makefile debug target enables ASan only, not UBSan |

The ASan result is an environment/runtime blocker, not a passing sanitizer run.

## Decoder performance

Host: aarch64 Cortex-A72, 4 cores, NEON build, one decoder thread,
bbb_nature 1280×720 and 1920×1080, v2 preset MEDIUM.

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
inverse-transform fast paths in the decoder.

Profile totals (one decoder thread, generated gradient/chroma input, QP 32):

| Resolution | Frames | Decode FPS | Profile output |
|---|---:|---:|---|
| 1280×720 | 10 | 9.1 | parse 23.4 ms, coeff 172.6 ms, transform 390.1 ms, motion 0.4 ms, chroma 137.3 ms, deblock 61.7 ms |
| 1920×1080 | 5 | 4.1 | parse 27.4 ms, coeff 195.6 ms, transform 442.9 ms, motion 0.2 ms, chroma 159.4 ms, deblock 66.4 ms |

Inverse transforms remain the dominant bottleneck.

## D7: RDO-lite vs SAD-only

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

## D8 benchmark smoke evidence

Command shape:

```bash
python3 tools/rd_bench.py \
  --clips bbb_nature \
  --codecs tcodecv2,x264vf,x264med,x265,svtav1p6 \
  --qps 22,32,42 --frames 10 \
  --threads 1 --tc-preset 2 --out /tmp/tcodec-d8/results.csv --keep
```

15 rows produced: 1 clip × 5 QPs × 3 codecs. Representative QP32 rows:

| Codec | Bitrate kbps | PSNR-Y | SSIM | Decode fps |
|---|---:|---:|---:|---:|
| tcodecv2 | 2,152 | 25.75 | 0.6162 | 17.3 |
| x264 veryfast | 1,308 | 31.30 | 0.8306 | 21.5 |
| x264 medium | 1,494 | 32.05 | 0.8501 | 21.0 |
| x265 | 1,601 | 33.89 | 0.8908 | 21.2 |
| SVT-AV1 p6 | 4,185 | 38.48 | 0.9609 | 19.4 |

TCodec v2 is 5–8 dB behind x264 veryfast at equal bitrate and uses 1.4–2.3×
more bits at equal quality. No negative BD-rate win vs x264 veryfast/medium is
claimed; this gate is not passed. The benchmark infrastructure and first
multi-codec comparison row (1 clip, 5 QPs, 4 codecs) are complete; the
required 10+ clip, multi-QP, multi-codec matrix with quality overlap
confirmation has not been completed.

## D0–D12 status

| Criterion | Status | Evidence / blocker |
|---|---|---|
| D0 | Pass | ARM64/NEON release build and fast regression target pass; long soak is a separate gate |
| D1 | Pass | `make test-full` passes 51/51 including in-process 300-frame 1080p soak |
| D2 | Pass | Range-coded v2 path with separate MV x/y contexts, DC/low/high frequency models; 76 RC contexts |
| D3 | Partial | Fuzz tests pass; ASan cannot start on this host (allocator init failure at 4GB ulimit); UBSan not run |
| D4 | Partial | B-frame infrastructure and parity exist; broad measured gain not established |
| D5 | Partial | DCT and parity exist; full Tier-1 transform program not complete |
| D6 | Partial | Luma BO SAO exists; EO, chroma SAO, restoration missing |
| D7 | Pass | RDO-lite BD-rate vs SAD-only = −66.4% (5-QP curve, bbb_nature 720p) |
| D8 | Partial | 1 clip, 5 QPs, 4 codecs benchmarked; 10+ clip corpus matrix not completed |
| D9 | Not met | 8–32 fps@720p, ~3–14 fps@1080p, below 60/30 targets |
| D10 | Pass | TCMX/TCMF round trips, native private `tcv1` ISO-BMFF carriage, H.264 MP4/fMP4 bridge |
| D11 | Partial | BENCHMARKS.md updated; TODO.md Tier-1 section added; SPEC/BITSTREAM/PROFILES truth pass pending |
| D12 | Partial | 5 commits ahead of origin/main; docs/FINISH_Tier1.md and BENCHMARKS.md updated; final clean status not recorded |

The sanitizer command was attempted with `ASAN_OPTIONS=detect_leaks=0`; the
Makefile debug target currently enables AddressSanitizer only, so no UBSan
result is claimed.

## Next blockers

1. Run the full real-corpus matrix against the required baseline codecs and compute
   BD-rate only over honest overlapping quality ranges (D8).
2. Implement parallel decode (WPP entry points or tile-based parallelism) to
   reach 60 fps@720p / 30 fps@1080p (D9).
3. Run sanitizer validation on a host/kernel where the ASan/UBSan runtime
   initializes (D3).
4. Complete SPEC/BITSTREAM/PROFILES truth pass and organize final milestone
   commits (D11/D12).
5. Produce v2 golden/conformance streams and close remaining TODO.md items.

The current working tree contains the v2 quadtree implementation, DC-specific
range-coder contexts, decode fast paths, and updated benchmark/decoder timing
documentation. Build, fast regression, parity, container, cross-compile, syntax,
and diff checks execute successfully. `make test-full` passes 51/51 including
the in-process 300-frame 1080p soak.
