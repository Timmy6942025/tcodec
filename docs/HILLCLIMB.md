# TCodec Hill-Climb Log

Autonomous encoder hill climbing. Score = synthetic probe
(`tools/rd_probe.py`: 320×240 10-frame motion clip, QPs 27/32/37,
TOTAL_BYTES with AVG_PSNR guard ±0.5dB) plus real-content A/B spot checks.
Keep threshold: >0.5% byte win, PSNR within guard, 50/50 regression green.

## Session 1 (2026-09-06)

| Step | Change | Probe | Verdict |
|---|---|---|---|
| 0 | baseline (pre-climb) | 17585B @ 30.25dB | — |
| 1 | v2 RDO scale fix: `(SSE+(λ<<16)·bits)>>16` → `SSE+λ·bits`; SAO gate `λ·10>>16` (always 0) → `λ·10`; `best_cost` int→int64 | 16022B @ 32.06dB (-8.9%, +1.8dB) | KEEP (f7004c9) |
| 2 | λ 137→200 | 15783B @ 31.67dB (-1.5%, -0.39dB) | KEEP (f7004c9) |
| 3 | v2 inter 4×4 RDO (medium+, CU≤32); fixed missing outer 4×4 flag (encoder wrote 4/TU, decoders read 5 — first ever decodable v2 4×4) | 15547B @ 31.59dB (-1.5%, -0.08dB) | KEEP (4be6aad) |
| 4 | SAO model 10→4 bits, inter/merge header +2 bits, sr 32→48, λ→100, estimator ×0.5, JND high 1.25→1.5×, merge/intra 4×4 | all neutral or loss | REVERT |
| 5 | v2 skip candidate | -47% bytes BUT -6.6dB | REVERT — mechanism OK (perfect-only neutral), `lb` estimator too pessimistic vs range coder; calibrate estimator first (9e33724 note) |

Cumulative probe: 17585B@30.25dB → 15547B@31.59dB (-11.6%, +1.34dB).

Real-content A/B (`screen_ui` 720p 10fr, ultrafast QP32, pre-climb vs now):
102.46KB@38.01dB → 16.89KB@41.11dB (-83.5%, +3.1dB), both decode 10/10 exact bytes.
Old bits-only RDO picked near-arbitrary modes on sharp content; fixed RDO picks right ones.

## Session 2 (2026-09-06, hardened probe)

Probe gained a static noise patch + moving checkerboard (texture signal);
rebaselined: 29326B @ 25.49dB on current code. Process fix: Makefile had
no header dependencies, so `tcodec_common.h` trials silently ran stale —
added `$(wildcard $(INC_DIR)/*.h)` prerequisites (this also voided three
earlier deadzone no-signal readings).

| Step | Change | Probe (hardened) | Verdict |
|---|---|---|---|
| 6 | deadzone eff/3→eff/2 | +34.8%, +0.31dB | REVERT (wrong direction) |
| 7 | deadzone eff/3→eff/4 | 27685B @ 25.41dB (-5.6%, -0.08dB) | step |
| 8 | deadzone →eff/6 | 25363B @ 25.12dB (-8.4%/-0.29 step) | step |
| 9 | deadzone →eff/8 | 25014B @ 25.05dB (-1.4%/-0.07 step) | LOCK |
| 10 | deadzone →eff/10 | 24362B @ 24.92dB (cumulative -0.57dB, guard breached) | REVERT to /8 |
| 11 | λ 200→250 (retune on hardened point) | 24396B @ 25.02dB (-2.5%, -0.03dB) | KEEP |
| 12 | λ →300 | +0.8%, -0.03dB | REVERT (knee at 250) |
| 13 | JND high 1.25→1.5× | -2.5% for -0.23dB (0.092 dB/% vs 0.012 for λ — poor rate) | REVERT |
| — | chroma QP+1→+2 | -2.35% bytes but -1.1dB Cb at qp32, zero savings there | REVERT |
| — | weaker deblock t1 | byte-identical (voided by stale build; unretried) | REVERT |

Deadzone walk cumulative (hardened): 29326B@25.49 → 25014B@25.05 (-14.7%, -0.44dB).
With λ250: 24396B@25.02 (-16.8%, -0.47dB). Kept trades only (exchange-rate
checked: λ250 costs 0.012 dB/%; JND-1.5× at 0.092 dB/% rejected).
