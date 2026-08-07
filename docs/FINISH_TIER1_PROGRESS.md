# TCodec — Tier 1 Finish: Progress Checkpoint

Session start: clean build green except 1 test (`range_coder_full_encode_decode`), 42/43.

## Milestone 1 — DONE (commit "Phase 3: range coder end-to-end (43/43)")
Root causes found and fixed:

1. **Uninitialized intra reference slots** (`src/predict.c`, `tc_intra_get_ref`):
   angular modes read `ref[2*blk_size]` (the p1 interpolation slot) which was
   never filled → encode decisions AND predictions depended on stack garbage.
   Encoder ran non-deterministically (same input → different bitstream across
   runs; WPP-vs-sequential cross_mismatch=8371; entropy path misrecon).
   Fix: fill `ref_above[2*blk_size]` / `ref_left[2*blk_size]` with repeated last
   sample in every branch.
2. **Encoder/decoder MV-bound divergence** (`src/motion.c`, `src/decoder.c`):
   encoder could emit edge-block MVs (e.g. fx=120 on 128-wide frame, integer
   motion) that `tc_inter_predict` handles by plain copy, but the decoder's
   stricter check (`fx+blk+1 <= w`) rejected → pred=128 → recon mismatch.
   Fix: `tc_inter_predict` integer fast path now bounds-checks and falls back
   to the OOB-safe per-pixel clamped path; decoder calls it unconditionally
   (it is OOB-safe for any MV). Encoder recon == decoder output bit-exact.

## Historical verification
- Earlier milestone: the pre-v2 `make test` run reached **43/43 PASS**, with
  entropy frame sizes + PSNR improved (avgPSNR 28.7 → 30.7 dB; WPP
  cross_mismatch 8371 → 0). This is retained as milestone history, not the
  current test count.
- CLI `tcenc --entropy` twice → byte-identical output (deterministic).
- valgrind memcheck: 0 errors on entropy-coded encode path.

## Remaining work (from DONE criteria)
- D1: retain the reproducible `make soak-1080p` 300-frame 1080p check and
  separately expose the full in-process run as `make test-full`; broader
  cross-mode and Tier-1 evidence remains distinct from the bounded regression.
  ASan/UBSan build is blocked on this kernel (ASan runtime init fails) — use
  valgrind or another host.
- D2: more context models per TODO Phase 3 (last-nz band contexts, level
  classes, MV components separate x/y, transform size flag already present;
  separate DC/low/high models; adaptive MV residual) — measured BD-rate vs
  Exp-Golomb path on golden corpus at QP 22/30/42.
- D4: hierarchical B-frames, BD-rate measure vs P-only.
- D5: true DCT-II 4x4/8x8 wired into variance decision; NEON parity.
- D6: SAO + deblock; NEON parity.
- D7: RDO-lite lambda cost mode decision.
- D8: real corpus (>=10 clips, Xiph) vs x264/x265/SVT-AV1; negative BD-rate.
- D9: ARM decode >=30fps@1080p, >=60fps@720p (NEON, 4 threads).
- D10: packet-preserving `tcmux` transport and keyframe-aligned proprietary segments are implemented and tested; native `tcmux mp4mux/mp4demux` now preserves TCV packets in a private `tcv1` ISO-BMFF sample entry, while `tools/tcmux_mp4.sh` provides the separately tested playable H.264/fMP4 compatibility bridge. Stock-player native TCV playback remains future integration work.
- D11: docs (SPEC/BITSTREAM/PROFILES/BENCHMARKS/TODO/README) + FINISH_TIER1.md.
- D12: hygiene, final report.

## Notes
- The NEON deblock uses an ARM-specific weak-edge schedule; parity checks cover
  the supported scalar/NEON configurations and active end-to-end paths.
- ASan cannot start on this kernel (allocator CHECK failed) — valgrind is the
  sanitizer substitute until CI elsewhere.


## Milestone D4 — DONE (hierarchical B-frames, GOP 4 + bi-prediction)

### What was built
- **Bitstream**: v1 extension header byte now defined (present when flags &
  `TC_FLAG_EXT_HEADER`): bits 0-1 = frame code (0=KEY, 1=INTER/P, 2=BIDIR/B).
  Emitted on every frame of a B stream so the decoder activates display
  reorder from the first packet; non-B v1 streams are byte-identical to
  before (no ext byte).
- **Encoder**: POC-based reorder buffer (`enc->bf`), hierarchical GOP4
  schedule — coding order A(4k), A(4k+4), B(4k+2), B(4k+1), B(4k+3);
  anchors at POC%4==0. QP offsets: anchors +0, B frames +0 (measured best).
  `tc_encoder_encode` returns `TC_ERR_NEED_MORE` while the buffer fills;
  `tc_encoder_flush_tail` emits the tail as forward-only frames.
- **Bi-prediction (new)**: B blocks choose among fwd-only (ref_sel=0),
  bwd-only (1), and average-of-both (2, mirrored MV: `(fwd(mv) + bwd(-mv) +
  1) >> 1`). ref_sel is now 2 bits, coded once per inter block (skip/inter/
  merge) in a single range-coder context. Encoder adds the bi candidate to
  the motion search, the merge candidate list, and the skip decision; the
  decoder averages the same two interpolations → bit-exact parity.
- **Decoder**: display reorder (`dec->disp`): one display frame per decode
  call in POC order; `TC_ERR_NEED_MORE` while nothing is displayable;
  `tc_decoder_flush_tail` drains the rest; RAP resets reorder state for
  mid-stream seeks. WPP decode of B streams is pixel-identical to
  sequential (verified in suite test + CLI).

### Historical milestone verification
- Earlier milestone: the pre-v2 `make test` run reached **47/47 PASS** after
  adding `b_frames_hierarchical` (12-frame B stream, display-order assertion,
  NEED_MORE buffering, tail drain, and WPP-vs-sequential pixel parity). This is
  retained as milestone history, not the current test count.
- `tools/parity_check.sh`: scalar/NEON bit-identical (incl. entropy mode).
- valgrind on B-mode encode+decode (12 frames, WPP 1 vs 4 threads):
  0 errors, no leaks.
- Deterministic: repeated encodes byte-identical.

### RD measurement (football 352×288 30f, 30 frames, P-only vs -b)
| QP | P-only rate | B-mode rate | dR | P-only PSNR | B PSNR | dQ |
|----|-------------|-------------|-----|-------------|--------|-----|
| 22 | 619,824 B | 697,325 B | +12.5% | 29.68 | 30.48 | +0.79 dB |
| 30 | 231,871 B | 265,398 B | +14.5% | 25.36 | 25.91 | +0.55 dB |
| 42 |  81,077 B |  86,114 B |  +6.2% | 21.41 | 21.69 | +0.28 dB |

**BD-rate B-mode vs P-only: −0.8%** (B frames spend ~6-14% more rate than P
at the same QP — ref_sel signaling + dual-reference search — but return
+0.3..+0.8 dB PSNR; the hierarchy reallocates quality to anchors). The
infrastructure (reorder, signaling, bi-prediction, WPP parity, tail drain)
is complete; further gains are an entropy/mode-decision refinement task
(D2-style contexts for ref_sel, direct-mode skip) rather than architecture.

## Current validation checkpoint — August 2026

### Implemented in this checkpoint

- v2 luma SAO Band Offset is now normative and signaled per CTU when
  `TC_TOOL_SAO` is present. The bounded syntax is `present:1`, `band:5`,
  `offset_code:4`, with codes 0..14 mapping to offsets -7..+7; code 15 is
  reserved. v0/v1 syntax is unchanged.
- Scalar and ARM NEON SAO application kernels are present and included in
  scalar/NEON parity testing. The implemented subset is BO only; EO, chroma
  SAO, and restoration remain future work.
- Permanent v2 SAO tests cover header tool signaling, raw and range-coded
  round trips, non-CTU-aligned 96x80 input, and encoder/decoder reconstruction
  parity.

### Verified commands

- Current checkpoint: the ARM64/NEON release build, bounded fast regression,
  and standalone 1080p soak smoke pass. The full release unit binary remains
  available as `make test-full`; the default `make test` intentionally avoids
  duplicating the expensive 300-frame run and reports its one explicit skip.
- `./tools/test_tcmux.sh`: passed.
- `./tools/parity_check.sh`: scalar/NEON parity passed, including end-to-end
  bitstream checks.
- `make debug`: built successfully. `ASAN_OPTIONS=detect_leaks=0
  UBSAN_OPTIONS=halt_on_error=1 ./build/test_tcodec` could not start: the
  host ASan runtime aborts in `sanitizer_allocator_primary64.h:131` during
  allocator initialization. This is an environment/runtime blocker, not a
  passing sanitizer result.
- `python3 -m py_compile tools/rd_bench.py`: passed; `rd_bench.py` now creates
  the parent directory for `--out` automatically.
- `tools/soak_1080p.sh` was added and previously smoke-tested at 1920×1080 with
  exact source/decoded byte-count validation; the default is 300 frames. After
  current staging changes it requires rerun. This is a correctness runner, not
  evidence that D9 real-time throughput is met.
- `git diff --check`: passed after removing generated Python caches.
- A 10-frame 320x240 v2 profile run decoded all 10 frames with exact output
  size 1,152,000 bytes. Profile totals were parse 2.20 ms, coefficients
  14.98 ms, transforms 35.97 ms, motion 0.09 ms, chroma 16.97 ms, and
  deblock 4.21 ms for the run (timing totals, not a Tier-1 target result).
- A bounded 10-frame `bbb_nature` v2 QP32 benchmark produced finite metrics:
  174,332 bytes, 3,347.2 kbps, PSNR-Y 27.606 dB, SSIM 0.66189, 12.91
  decode fps. This is a smoke result, not a complete BD-rate curve.

### Tier-1 status remains incomplete

- D8: the required 10+ clip, multi-QP, baseline comparison does not show a
  negative BD-rate win; the bounded real-content result remains behind mature
  codecs and no win is claimed.
- D9: measured v2 decoding remains below 60 fps at 720p and 30 fps at 1080p;
  the actual profile data above is not ARM real-time evidence.
- D10: `TCMX`/`TCMF` remains a tested proprietary transport; native
  `tcmux mp4mux/mp4demux` also preserves TCV packets in a private `tcv1`
  ISO-BMFF sample entry, while `tools/tcmux_mp4.sh` provides the separately
  tested FFmpeg/ffplay-playable H.264/fMP4 compatibility bridge. Native `tcv1`
  is intentionally not stock-player decodable.
- D11: the core docs distinguish native private carriage from the playable
  compatibility bridge and now document the bounded/full test targets. D12:
  hygiene checks pass, but the implementation set is still uncommitted and the
  final clean-status/organized-commit exit gate remains open.

## Decoder optimization checkpoint — August 2026

- Added stride-aware zero-residual copies for v2 luma/chroma reconstruction,
  removing unnecessary inverse transforms and add/clip loops.
- Added shared fixed-point DC-only inverse-DCT helpers and decoder fast paths;
  permanent transform tests compare the shortcut against the full 4x4/8x8
  inverse transforms for DC values -2000..2000, including negative rounding.
- Added v2 odd-dimension rejection before quadtree allocation/reconstruction;
  this protects the 4:2:0 row-copy invariants. The encoder CLI still accepts
  such input, but the resulting stream is correctly rejected by the v2
  decoder rather than being silently misinterpreted.
- Profile buckets now include the new residual-copy operations.

### Measured decoder profile

Host: aarch64 Raspberry Pi-class Cortex-A72, 4 cores, NEON build, QP 32,
thread count 1, generated fixed-point gradient/chroma input.

| Resolution | Frames | Decode FPS | Output bytes | Result |
|---|---:|---:|---:|---|
| 1280x720 | 10 | 9.1 | 13,824,000 | exact frame-size validation |
| 1920x1080 | 5 | 4.1 | 15,552,000 | exact frame-size validation |

The results are reproducible profile measurements, but remain below the Tier-1
60 fps@720p and 30 fps@1080p targets. The dominant reported bucket remains
inverse transforms; no real-time claim is made.

### Current D8 smoke evidence

The actual corpus names `bbb_nature`, `sintel_action`, and `parkrun` produced
9 finite tcodecv2 rows (30 frames each, QPs 22/32/42, one thread, preset 1).
Example rows: bbb_nature QP32 = 3,398.8 kbps / 27.7300 dB PSNR-Y / 0.67982
SSIM; sintel_action QP32 = 2,367.0 kbps / 32.4797 dB / 0.85349 SSIM;
parkrun QP32 = 22,699.1 kbps / 23.9660 dB / 0.74997 SSIM. All metrics were
finite and all rows decoded 30 frames. This is still not the required
multi-codec BD-rate proof; no competitive win is claimed.
