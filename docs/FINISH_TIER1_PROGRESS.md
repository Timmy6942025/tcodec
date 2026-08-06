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
