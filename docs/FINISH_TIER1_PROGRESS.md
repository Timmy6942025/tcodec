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

## Verified
- `make test`: **43/43 PASS**, entropy frame sizes + PSNR improved
  (avgPSNR 28.7 → 30.7 dB; wpp cross_mismatch 8371 → 0).
- CLI `tcenc --entropy` twice → byte-identical output (deterministic).
- valgrind memcheck: 0 errors on entropy-coded encode path.

## Remaining work (from DONE criteria)
- D1: extend fuzz to bit-flips across NAL boundaries; 1080p long-run >=300
  frames; WPP==sequential byte parity (test exists, extend to entropy flag?);
  ASan/UBSan build blocked on this kernel (ASan runtime init fails) — use
  valgrind.
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
- D10: tcmux MP4 container + streaming segments demo.
- D11: docs (SPEC/BITSTREAM/PROFILES/BENCHMARKS/TODO/README) + FINISH_TIER1.md.
- D12: hygiene, final report.

## Notes
- `TCODEC_NEON` deblock is weak-only + 8px boundaries vs scalar 4px — parity
  gap exists (documented in TODO ACT-5); tests pass on this machine which
  builds NEON by default.
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

### Verified
- `make test`: **47/47 PASS** (new `b_frames_hierarchical` test: 12-frame
  B stream, display-order assertion via nearest-frame matching, NEED_MORE
  buffering, tail drain, WPP-vs-sequential pixel parity).
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
