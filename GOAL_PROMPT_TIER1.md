# GOAL PROMPT — Finish TCodec to Tier 1 (COMPLETE & WORKING)

You are a coding agent operating in /home/timmy/tcodec. Your objective is below.
You may not stop running until EVERY "DONE CRITERION" below is satisfied with
committed code and reproducible evidence. If you are resumed from a checkpoint,
first read docs/FINISH_TIER1_PROGRESS.md (if present) and TODO.md to recover state,
then continue until DONE.

## 1. OBJECTIVE

Finish TCodec to **Tier 1** exactly as defined in MASTER_PLAN.md — a stable,
deterministic, fully tested, real-content-usable video codec that today's
prototype is *not yet*:

- (a) stable, deterministic, fully tested bitstream (v1)
- (b) strong encode/decode correctness under fuzzing and long-run testing
- (c) meaningful compression gains vs naive H.264 presets at equal perceptual quality (*measured, not claimed)
- (d) real-time / near-real-time decode on this ARM-class hardware (this machine IS the target)
- (e) sensible containerization, streaming segmentation, and integration story

Everything below is the contract. Work through TODO.md (the repo's source of truth,
~223 items) and MASTER_PLAN.md; implement what Tier 1 needs, measure each step,
and don't stop until all of section 3 is true.

## 2. DONE CRITERIA — all must be true (then you stop)

**D0 — Baseline.** `make clean && make && make test` runs green at the START of your
session and you record the failing/state table for handoff. (Known now: 43 tests,
1 failing: `range_coder_full_encode_decode`. Fixing this is your first task.)

**D1 — 100% green tests.** Every test in test_tcodec.c passes; none skipped or
stubbed. Coverage includes: deterministic byte-exact output, fuzz of malformed
bitstreams (no crashes/hangs, graceful errors), long-run (>=300 frames, 1080p),
WPP threaded == sequential parity, v0/v1 bitstream, all entropy paths (legacy
Exp-Golomb *and* new range coder), DCT+WHT paths, B-frame paths, all profiles.

**D2 — Entropy coding finished.** `range_coder_full_encode_decode` passes
(encoder recon == decoder output, bit-exact) across the whole golden corpus at
QPs 22/30/42 and on long real clips. Phase 3 in TODO is complete: context
modeling for block mode, significance maps, last-nonzero (band contexts),
coefficient levels, MV components, skip/merge flags, transform-size flag;
separate DC/low/high-frequency models; adaptive MV residual coding. Old
Exp-Golomb/tANS path is either removed or 100% parity-tested. The only entropy
path in the default build is the context-modeled arithmetic coder. Delete all
`rc_debug*`/`rc_trace*` scratch files when done.

**D3 — Fuzz & correctness hardened.** Existing fuzzer extended: random/bit-flipped
inputs across NAL boundaries; decoder never crashes/hangs, always recovers or
aborts cleanly; no UB under sanitizers (build once with `-fsanitize=address,undefined`
and run the suite). Long-run test extended to >=300 frames at 1080p including
scene cuts, fades, black frames.

**D4 — B-frames implemented & paying.** Hierarchical B-frame support with 2
references in bitstream v1+. Decoder handles B properly (no broken display/
decode order). BD-rate gain of the B-frame build vs P-only build is measured and
reported (expect a few % on real content; tools/bd_rate.py).

**D5 — Transform cascade finished.** DCT-II (4x4/8x8) wired into mode selection
(variance → WHT vs DCT decision, matching spec & TODO Phase 4 intent). JND band weighting
applies to DCT and WHT identically on both sides (encoder quantizer + decoder dequant).
Scalar vs NEON bit-exact parity tests for every transform. `transform.o` no longer
contains unused "DCT exists but not wired" code.

**D6 — In-loop filtering complete.** SAO (offset mode) implemented on top of
deblock, scalar + NEON with bit-exact parity; decoder-side cost budgeted; blocking
artifact metrics improved on real content at mid/low bitrate (recorded).

**D7 — Real mode decision (RDO-lite).** Mode decision uses SSE/SAD-plus-`lambda`-
cost (lambda from QP) for skip/merge/inter/intra instead of pure SAD. Cost model
documented. BD-rate vs the old SAD-only build measured (keep if win; revert if
loss, and say so).

**D8 — Real-content benchmark proves (c).** New committed dir `benchmark_v2_real/`
with >=10 real clips (>=720p, >=15s each; film film/animation, nature, screen
content/UI, gaming, dark scene, grain, title cards — use Xiph media (Sintel, Tears
of Steel, Big Buck Bunny), public-domain/generative sources; network is available;
download into `benchmark_v2_real/raw/` and commit the manifest + hashes, not
necessarily the binaries). Encode with tcodec (presets fast/medium/slow, QPs
22–47) and baselines: **x264 --preset veryfast and medium**, x265 default,
SVT-AV1 presets 6/4 (installed: x264 0.164, x265 4.1, SVT-AV1 2.3.0, ffmpeg 7.1.5
with libaom/libsvtav1; SSIM filter available; VMAF may need install — use PSNR-Y
+ SSIM as primary metrics, VMAF if you get it working). Run tools/bd_rate.py to
produce BD-rate vs each baseline.
**REQUIRED RESULT (the Tier-1 proof):** tcodec shows a *negative* (winning) BD-rate
vs x264 veryfast/medium at equal PSNR-Y/SSIM on the real corpus. Numbers
vs x265/SVT-AV1 are recorded for honesty (not required to win). BENCHMARKS.md
rewritten with this data; tools/evaluate_quality.py + plot_rd.py outputs committed.

**D9 — ARM decode real-time.** On this machine (aarch64, 4 cores, ~7.6 GB RAM),
with the NEON build and 4 threads: decoder must sustain **>=30 fps at 1080p** and
**>=60 fps at 720p** on real clips from D8 (measure with tools/, record FPS + CPU%).
If it doesn't pass, you must optimize decode-side (profile hot loops, NEON the missing
kernels — e.g., 6-tap inter NEON, CA-luma NEON, etc. — WPP row scheduling) until
it does. This is non-negotiable for Tier 1 ((d) says near-real-time).

**D10 — Container/integration story.** `tools/tcmux.c` (mp4 mux/demux for .tcv) +
tests that roundtrip AVC1-epoch-style fragments; simple segmentation demo
(keyframe-aligned segments `seg_%04d.m4s` + playlist/variant manifest) that
ffplay/ffmpeg can play back across a disk directory (simulated-streaming);
README+streaming docs written (covers (e)). Everything in this criterion must be
reproducible from `make` + one script in tools/.

**D11 — Docs truthful & final.** SPEC.md, BITSTREAM.md, PROFILES.md, BENCHMARKS.md,
TODO.md, MASTER_PLAN.md, README.md all updated to match the code exactly
(bitstream version bumped as needed; v0 back-compat test keeps passing unless
killed deliberately with justification). TODO.md checkboxes: every applicable
item checked; MASTER_PLAN "Current State" rewritten. Golden corpus regenerated if
bitstream compat changes. `docs/FINISH_TIER1.md` written: final numbers,
what changed, how to reproduce.

**D12 — Repo hygiene + exit.** `git status` shows only intended changes; commits
organized per milestone with clear messages; no rc_* scratch, no build artifacts,
no giant binaries; `make` from clean works; then **write the final report and
STOP** — do not continue into Tier 2/3/4 (Tiers are separate future goals; scope
creep kills this goal). Final report message to the user: table of D1..D12 state,
BD-rate table vs x264/x265/SVT-AV1, decode fps table, list of commits.

## 3. WORK PLAN (order; gates between steps)

1. **Reproduce + fix the failing test** (range coder end-to-end). Use the rc_* traces;
   find the mismatch (encoder recon vs decoder output under entropy coding) — likely
   state init/carry/flush mis-sync between enc/dec. Commit: "Phase 3: range coder end-to-end (43/43)".
2. **Phase 3 — entropy contexts** (implement TODO's 17 entropy items; keep bit-exact
   roundtrip after each change; measure BD-rate delta vs legacy path on golden; commit per item).
3. **Phase 4 — transforms**: wire DCT-II (variance-selected WHT/DCT), JND parity, NEON parity.
4. **Phase 6 — temporal**: hierarchic B-frames (D4).
5. **Phase 5 — partition/spatial** only what shows BD-rate win (quad/halves refinement).
6. **Phase 7 — SAO** + verify deblock parity (D6).
7. **Phase 8 — rate control for real content** (multi-frame consistency, GF=lookahead-lite for Tier-1; keep it simple).
8. **Phase 9 — ARM decode**: profile with `perf`, NEON-real hot loops (SAD already done),
   then meet D9.
9. **Phase 10 — container**: tcmux + segmentation + ffrecplay demo (D10).
10. **Benchmark + docs + final report** (D8, D11, D12).

Every phase: measure the delta (BD-rate / fps) before commit and record in
BENCHMARKS.md / TODO.md. A tool that shows no win after implement → remove or
disable + explain (MASTER_PLAN principle: every tool must earn its keep).

## 4. NON-NEGOTIABLE CONSTRAINTS

- **Deterministic, purely algorithmic codec.** No ML / NN / learned / trained models
  anywhere (GOAL of project).
- **Integer-only, fixed-point coding path.** Decoder must be bit-exact across
  platforms: no float in encode→decode data path (bitstream codable).
- **ARM-first.** Every new kernel needs a NEON path; NEON path and scalar path
  must produce identical results (parity tests exist; extend them to new tools).
- **Decoder cost is a first-class budget.** Tool complexity must justify decoder
  hit (record per-tool decode delay in bench tables).
- **WPP wavefront parallelism preserved** (encoder rows, per-row bitstream paths);
  keep it working in threaded and sequential modes with byte-identical output.
- **Never silently change bitstream version.** Version bumps must update
  SPEC/BITSTREAM/PROFILES and golden files + compat tests.
- **No floating-point claims.** Every "we are faster/better/…" claim must be a
  number in BENCHMARKS.md reproducible by an agent/another dev.

## 5. EXTRA INFO (verified facts — trust this, it saves hours)

- **This machine is the target platform**: `aarch64`, 4 cores, 7.6 GiB RAM.
  The NEON build (`-march=armv8-a -DTCODEC_NEON=1`) is the default and compiles here.
  gcc 14.2, GNU Make 4.4.1. You profile on the real target — no cross-build needed.
- **Baselines already installed**: x264 0.164, x265 4.1 (8bit, NEON build info shows
  its cpu NEON too), SVT-AV1 2.3.0, ffmpeg 7.1.5 with libaom-av1 + libsvtav1 encoders;
  ffmpeg has SSIM filter; **VMAF: only `vmafmotion` filter is present — the main
  `vmaf` filter is NOT confirmed installed**; if you want VMAF, install libvmaf/ffmpeg vmaf
  filter or fall back to SSIM+PSNR-Y as primary metrics. python3 + numpy + scipy
  available; matplotlib available for tools/plot_rd.py.
- **Network works**: media.xiph.org reachable → you can fetch real clips for D8.
- **golden corpus is synthetic** (8 clips at 3 QPs; 4x4/gradient/checkerboard/diag/lines/
  noise). Great for regressions; USELESS for claiming competitiveness. Real claims
  come only from D8.
- Today, same-QP synthetic snapshots: tcodec PSNR ~10–25 dB below x264/x265/SVT-AV1;
  same-quality (30 dB target) tcodec needs roughly 2–4× the bitrate of x264, and on
  checkerboard-style content it can't even reach 30 dB (WHT's weakness). Read
  BENCHMARKS.md — the project already says its own first BD-rate (−9.7%) is "not
  reliable" and "a zero-point". Your job is to move that number to an honest,
  reliable negative win vs x264 on real footage.
- **In-progress uncommitted work** at repo root: `src/range_coder.c` + encoder/
  decoder wiring + 400 lines of new tests + `benchmark_v0/`, `benchmark_v1_*`,
  `rc_debug*`, `rc_trace*` — these are forensics for the failing entropy work; fix the
  failing test, then delete scratch files, integrate the tests/docs into the proper commit
  chain (tests belong in the same commit).
- **Known planning "surprise" set** (from README/TODO): WHT uses position-dependent
  dequant issue for DCT (ACT-4...); NEON deblock uses weak-only filter + 8px
  boundaries vs scalar's 4px — known parity gap, you must reconcile; WPP has per-row
  bit-alignment complexities; color NEON luma is 32-bit accumulation; ME search
  centered on median MV promoter's *predictor*; 4-checkBounds on reconstruction.
- **Tree layout**: `src/` codec core (15 files), `neon/` SIMD kernels, `include/`
  public+common headers, `tools/` (tcenc/tcdec wrappers, run_benchmark.sh,
  bd_rate.py, evaluate_quality.py, plot_rd.py, gen_golden.sh), `test/test_tcodec.c`
  (the whole suite ~43 tests), `golden/` corpus, `build/` artifacts, docs root as
  listed above.
- **Commit discipline**: after each of the numbered work-plan milestones, one
  clean commit with a clear message (`Phase 3: ...`). TODO.md checkbox status is
  the repo's "clock" — keep it accurate at the end of every working block. If you
  lose a session, the next agent must resume from TODO + git log + this file.
- **When stuck**: bisect to the smallest failing test, add an instrumented trace,
  then re-run — do not thrash. After 2 hours on one fix without progress, switch
  to the next milestone and return later. Never "paper over" a failing test.
- **Honesty requirement**: if a milestone turns out to cost more than a meaningful
  BD-rate win (D4/D5/D6/D7), you must say so in your final report instead of
  shipping it anyway — the design says "tools must earn their keep".

## 6. STOP CONDITION

Stop when D0–D12 are all verified **and** the final report is written to
`docs/FINISH_Tier1.md` and communicated. Do not gold-plate. This prompt's success
is: `make test` 100%, real-corpus BD-rate WIN vs x264 recorded, decode ≥30fps@1080p
on this machine, stream mux/segment demo plays, all docs accurate, clean repo.
