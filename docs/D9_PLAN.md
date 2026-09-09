# D9 Decode-Performance Plan (2026-09-08 measurements)

Target: 60fps@720p / 30fps@1080p (Tier-1). Current (this box, 4 cores):
park 720p30 qp32 multiref+B? no, P-only: t1/t2/t3/t4 = 16.9/21.3/17.0/18.0fps
Sintel 1080p10 qp32: t1/t2/t4 = 5.2/6.8/7.55fps (target 30 → 4× needed).
(single cold run read 9.9 — always warm up before measuring).

## 1. Component profile (park 720p, t1, 30fr)

| Component | Share |
|---|---|
| motion (subpel interp volume) | 25% |
| transform (idct) | 24% |
| coeff parse (serial range decoder) | 16% |
| deblock | 17% |
| chroma | 12% |
| parse (modes/headers) | 5–6% |

(Screen differs: motion 4%, coeff+transform 50%, deblock 24%.)

## 2. Thread scaling is broken (measured, not theory)

t1→t2 = 1.2×; t3/t4 flat-to-worse. WAVESTAT (TC_WAVESTAT=1): workers
idle ~40–60% of frame time (wait/work 0.5–1.4, high frame variance =
stragglers). Causes: serial v2_parse_ctu for the whole frame precedes
the wavefront; 12×20 grid ramp/drain; highly nonuniform CTU costs
(heavy water-inter stragglers stall diagonals). Fixing scaling alone
cannot reach 60fps (serial parse + Amdahl); it multiplies serial wins.

## 3. Ranked worklist

1. **Serial task cost (same work helps serial + starvation)**:
   - motion edge dispatch (D9-1, DESIGNED): 21% of luma interp calls
     fall back to the slow per-pixel scalar path (fx<2||fy<2 true
     edges; decoder NEON gate stricter than scalar can_6tap_full).
     Design: interior sub-rectangle satisfying NEON margins runs the
     existing NEON kernel; clamped L-fringe runs scalar (same per-pixel
     arithmetic = bit-exact by construction; OOB safety preserved).
     Est ~15% of motion (~4% total), zero RD risk. Gate: fps + parity
     + recon. TC_DISPATCH=1 meters.
   - entropy parse throughput: 21% combined; range-dec per-bin
     branches. Micro-opt + batching (needs careful measurement).
   - deblock: 17%, but reject-gating only helps flat content
     (water diffs legitimately fire weak) — do NOT chase for nature.
2. **Wavefront**: revisit after tasks cheapen (variance is the
   starvation driver). Consider parse/recon pipeline overlap.
3. **Encoder-side volume**: fewer nz (RDOQ hill-23 did this; helps
   coeff+transform+parse simultaneously), more skip/merge (parked).

## 4. Math

Serial 17fps → 60fps needs ~3.5×. No single item above exceeds ~1.2×.
D9 = multi-win program + (ideally) perf hardware (none on this box)
for kernel tuning. Do NOT attempt as drive-by trials; schedule a
dedicated kernel sprint after compression work lands.

Update 2026-09-09 (trial 47 verdict): motion edge-tiling REVERTED —
microbench proves interpolation is MEMORY-bound (NEON 6-tap ≈ scalar
per block; tiling worth ~1.3% total). SIMD kernel work on interp/idct
is largely futile here; D9 must come from VOLUME reduction (fewer nz:
RDOQ did this; skip would; B-frames do) + bandwidth/layout + (still)
wavefront stragglers. Parse-side (serial entropy) remains the one
compute-bound candidate. Coeff-parse core already force-inlined (prior
pass); further micro-opts ≈ 2–3% total. Real parse parallelism needs
per-row/CTU entry points (FORMAT change, v2.1 batch).

Update 2026-09-09 (trials 46/47): motion edge-tiling REVERTED — microbench
proves interpolation is MEMORY-bound (NEON 6-tap ≈ scalar per block;
tiling worth ~1.3% total). SIMD kernel work on interp/idct is largely
futile on this workload; D9 must come from VOLUME reduction (fewer nz:
RDOQ did this; skip would; B-frames do) + bandwidth/layout + (still)
wavefront stragglers. Parse-side (serial entropy) remains the one
compute-bound candidate.
Update 2 (same day): coeff-parse core ALREADY force-inlined (prior pass);
further micro-opts ≈ 10–20% of 16% ≈ 2–3% total. Real parse parallelism
needs per-row/CTU entry points (FORMAT change, v2.1 batch). D9 3.5× is
not reachable by kernels: needs (a) parse parallelism (format), (b)
volume cuts (encoder), (c) variance reduction. Multi-week program.

## 5. Infra landed (hill-23 batch)

- `tc_decoder_set_threads()` + `tcdec -t 1..16` (default 4).
- TC_WAVESTAT (wait/work per frame), TC_DISPATCH (neon/scalar split),
  TC_BYTEBREAK (encoder byte shares). All env-gated, zero behavior
  change when unset.
