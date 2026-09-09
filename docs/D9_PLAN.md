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
   - motion edge dispatch: 21% of luma interp calls fall back to
     scalar (fx<2||fy<2 borders). Widen NEON with bit-exact clamped
     edges (TC_DISPATCH=1 meters; OOB safety is fuzz-gated).
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

## 5. Infra landed (hill-23 batch)

- `tc_decoder_set_threads()` + `tcdec -t 1..16` (default 4).
- TC_WAVESTAT (wait/work per frame), TC_DISPATCH (neon/scalar split),
  TC_BYTEBREAK (encoder byte shares). All env-gated, zero behavior
  change when unset.
