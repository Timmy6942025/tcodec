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

## Session 3 (2026-09-06, estimator + skip re-trial)

| Step | Change | Probe (hardened) | Verdict |
|---|---|---|---|
| 14 | `lb` estimator ×1.5 (was undershooting range coder) | 23882B @ 25.08dB (-2.1%, +0.06dB — true win) | KEEP |
| 15 | estimator ×2 / ×1.75 | +0.6%/-0.04, -0.27%/-0.08 | REVERT (knee at ×1.5) |
| 16 | v2 skip re-trial (calibrated costs) | -40% bytes, -4dB | REVERT — estimator theory disproven; suspect MVP divergence, needs tracing |
| 17 | v2 perfect-match skip (dskip==0 → force) | probe -0.12%/-0.01; screen med +1.5%/-0.02 | REVERT — same staleness |
| 18 | v2 chroma-honest skip finalist + bounded-damage gate | -21%/-2.45dB; margin 2x still -2dB; bounded +7%/flat | REVERT — lbch charges ~17b per flat 4×4 block (estimator crud); even exact staleness compounds via refs (disabled-fires control byte-identical). Skip needs fresh chroma + honest bits |
| 19 | v2 skip with FRESH chroma (redefined semantics) + merge-only conversion | +10%/-0.14dB screen | REVERT — zeroing merge's epsilon starves future references; needs propagation awareness (mb-tree-lite). All code removed (untested dead paths) |
| — | JND high 1.25→1.5× (dual gate) | byte-identical on probe AND screen AND QP22 | REVERT — weight never binds at tested QPs; high bands quantize to zero either way |
| — | RDOQ trailing-zero (honest dq² vs recounted lb) | byte-identical (absurd-threshold control proves plumbing; deadzone already optimal) | REVERT |
| — | CfL alpha >>3→>>2 | flat bytes, Cr −0.41dB at qp37 (fixed alpha can't serve ± correlation) | REVERT — needs per-leaf alpha signaling (v2.1 syntax batch) |
| — | activity masking (variance-weighted RDO, x264-AQ direction) | probe +0.7%/-0.02dB; screen med -0.5%/+0.07dB/+0.0003 SSIM (all sub-threshold) | REVERTED twice — no measurable effect anywhere; perceptual hypothesis unconfirmed |
| — | per-CTU QP deltas + mb-tree-lite (FULL BUILD: syntax, classifier, 3 decode paths) | best variant -14%/-0.40dB sita (0.028 rate, borderline); screen +48% without warmup; frozen-degeneracy + recon-confound bugs found & fixed along the way | REVERTED — borderline diagonal + untested-path risk; design retained in V2_1_BATCH for corpus-driven retry |
| — | λ re-scan on full current code (150/250/300 vs 200) | 150 dominated; 250 −0.3%/−0.11dB (0.37 dB/%, poor); 300 worse both | KEEP 200 — knee confirmed |
| — | v2 WHT (legacy force-DCT/force-WHT on checkerboard) | force-DCT identical (WHT never selected); forced-WHT +10%/-0.2dB | WHT genuinely worse, not RDO blindness. PARKED entirely (remove from v2.1 batch); legacy keeps harmless RDO option |
| 25 | B-frame hierarchical QP ladder (anchors 0, mid-B +1, outer-B +2) | legacy v1 B on park: 1067KB@24.38 → 933KB@24.01 (-12.5%/-0.41dB); per-frame pyramid sane (B 0.3-0.7dB below anchors, std 0.63, no flicker); v1-B vs v1-P ≈ parity | KEEP as bugfix to documented D4 intent (was hardcoded 0); v2-B still parked (needs B-RDO, not just emission); suite 52/52 |
| 26 | slow preset audit: exhaustive intra INVERTED (+13%/-0.74dB screen, 49s vs 28s) — full RDO overfits estimator crud, pruning regularizes | extended top-4 to slow + v2 sr 32→64 for slow (legacy parity, large-motion insurance); park slow≈med (+0.6%, flat, same speed) | KEEP — ladder sane; suite pending |
| 19 | last-pos bit-model term (linear 2..10) | probe -1.5%/+0.06 TRUE WIN; screen med +2.1%/-0.05 | shape miscalibrated — replaced by coder mirror |
| 20 | last-pos term mirroring range coder (presence + trunc-unary + EG) | probe -2.3%/-0.02; screen med identical bytes, -0.02dB (neutral) | KEEP — strictly-better model, real-neutral |
| 24 | CfL adaptive sign (neighbour covariance, zero-bit) | probe -0.2% bytes, Cr −0.41→−0.03dB at qp37 (Cb/Cr up elsewhere); screen identical (no correlation, correct no-op) | KEEP — suite 52/52 |
| 27 | CfL adaptive magnitude (neighbour regression slope → shift 2..5, integer-only) | probe -0.18% bytes, Cr +0.3/+0.4dB at qp27/32; screen identical | KEEP — small, principled, real-neutral; suite 52/52 |
| 28 | global-motion extra ME center (coarse ±32px estimate, magnitude-gated, MVD stays mvp-relative — encoder-only, zero syntax/decoder risk) | probe -4.9%/+0.22dB; park -0.7%/flat (+23%→+17% encode time); screen/tree neutral | KEEP — search diversity wins where mvp misleads; coincidence/need gates trialed neutral-to-worse, kept simple; suite 52/52 |
| 29 | P multiref 2→4 refs (2-bit ref_idx, same MULTI_REF flag; old 1-bit streams break, none exist outside tests) | probe wins all QP (-0.6..-4%, qp32 +0.59dB); screen -9.4%/+0.61dB (older refs cleaner on static); park -1.0%/-0.01 (+12% enc) | KEEP — choice value confirmed again; suite 53/53 |
| 30 | explicit bi-average REMOVED (fwd/bwd choice + merge-average stay) | park -1.9%/-0.03, screen same-bytes/+0.10dB, -9% enc time; 3% selection was mirror-model noise | KEEP removal — joint search would be the real version (v2.1); suite 53/53 |
| 31 | preset-ladder timing audit (screen med QP32): full 40s → no-global 33s → no-global+2ref 26s | global +21% time for ~0 on screen; 4-ref +27% time for -9.4%/+0.62dB | KEEP both in medium (compression-first; wins earn costs). No ladder split |
| — | P/B path separation audit (feared shared merge/global miscalibration on B) | BIDIR and P branches fully separate (`} else if`, line 726); no bug | No change needed |
| — | B mode mix on park (temporary write-path counter, removed after) | 1738 inter leaves: fwd 32%, bwd 41%, merge-as-average 26%, explicit-bi 0% (already removed); no intra pathology | Healthy; confirms choice-not-averaging thesis |
| 32 | weak-filter kernel flat-identity fix (HEVC delta `9(q0-p0)-3(q1-p1)`, scalar + NEON×3 sites; mapping untouched) | probe −3.5%/+0.79dB; screen −23.9%/+0.65dB TRUE WIN; park −21.6%/+1.67dB (smooth water was maximally exposed) | KEEP — old kernel injected ±tc at every filtered flat boundary in-loop; parity ALL OK, suite 53/53 |
| — | deblock mapping inversion trial (small-diff→filter, big-diff→preserve) | probe −11%/+0.56dB BUT screen 3.09KB@42.86 → 9.59KB@27.55 (collapse) | REVERTED — with the broken kernel, filtering flat screen areas compounded +10 errors in-loop every frame; mapping retune only meaningful after trial 32; future work |
| 33 | deblock mapping to HEVC direction (retry of above, now with flat-identity kernel) | probe −27.8%/+3.70dB; screen −17.0%/+0.81dB TRUE WIN; park +2.2%/−0.10dB (flat/borderline — water has few sharp edges to save) | KEEP — old mapping 6-tap-blurred every real edge in-loop; strength-3 paths now unreachable (kept, reserved); parity ALL OK, suite 53/53 |
| 34 | skip-6: fresh-chroma skip + chroma-honest RDO + ρ=1 propagation guard + FRESH_SKIP tool bit (bit 15) + unknown-flag rejection | ρ=0: −30.7%/−5.06dB; ρ=1: −22.6%/−3.35dB; near-exact gate: byte-identical (never fires — mvp-MC SSE avg 550/cu² frozen vs 3627 probe, no clean split) | REVERT all (7th skip failure) — mvp divergence makes zero-MVD skip MC mediocre; lb crud inflates alternatives; without real mb-tree RDO can't price reference poisoning. Flag+rejection return with v2.1 batch |
| 35 | lb estimator structural fix (count only coded positions 0..last; both coders stop at last_nz — removes ~17 phantom bits/empty TU) | probe −0.8%/+0.10dB; screen −5.6%/+0.01dB; park +1.4%/−0.01dB; ducks +0.7%/−0.02dB (2nd nature clip confirms systematic) | REVERT — estimator-crud theory disproven as net win (cf. trial 16): the old phantom acted as a useful texture-coding deterrent on nature; honest lb codes more water noise for nothing. Real lever is λ/texture handling, not estimator |
| 28 | scene-cut orig-vs-recon BUG: false keyframes on dark content (all-30-keys at qp32 on frozen input!) + non-monotonic RDO | fixed with stored orig histograms (16-int state, QP-independent); sita qp32 556KB→225KB, K+29P, decodes | KEEP (bugfix); suite pending |
| 29 | static-content drift: -3.1dB/30fr frozen (requant walk) vs x264 perfectly flat (skip exact-copies) | diagnosis only | NEXT PROJECT: skip-6 with exact-copy stability (re-implement fresh-chroma skip, validate drift, not just rate) |
| 21 | CRF-lite reactive QP (CQP ±2 from trailing complexity, keyframes reset) | park nature -0.7/-1.2/-1.8% at flat PSNR+SSIM across QP27/32/37 | REVERTED (see 22): screen showed +8%/+0.18 (wrong direction); flipside -3.0%/-0.23 (0.077 dB/%). Both mediocre diagonals; needs quality servo, not bit-ratio heuristics |
| 22 | CRF-lite direction flip (easy-coarse) on screen | -3.0%/-0.23dB | REVERTED with 21 — back to fixed CQP; true quality-targeted servo queued |
| 23 | Quality servo (hold PSNR flat ±0.75dB, ±1 step, ±2 range) | park +0.7/+2.6/+5.2% bytes for +0.10/+0.09/+0.06dB | REVERTED — buys quality at poor rates; variance benefit unproven. Frame-QP heuristics can't beat fixed-QP without lookahead/propagation |
| — | legacy B vs P on park (v1+--bframes) | +0.35% bytes, -0.04dB — B buys nothing on current engine | v2-B PARKED indefinitely (needs B-RDO maturity first, not just emission) |

## Validation state

- Fast suite 52/52 green on every kept commit; full suite 53/53 green
  including the 300-frame 1080p in-process soak (final: all climbs).
- Golden manifest regenerated with climbed encoder (all 212 hashes move —
  decisions changed everywhere, decoders all pass).
- Real-content gates: `screen_ui` medium 10fr QP32 + `park_joy` nature
  checkpoint recorded in BENCHMARKS.md.
| — | full coder-faithful coeff model (sig/gt/sign/UE counts) | probe +0.9%/+0.10; screen med +4.7%/-0.20 | REVERT — fidelity without probability-skew awareness misranks; effective models need adaptive scaling |
| — | MVD bits ×0.75 (ctx-coded cheaper than EG) | probe +0.8%/+0.01 | REVERT — EG model fine at tested motion ranges |
| — | v2 WHT/DCT per-TU type bit (TRANSFORM_TYPE concept) | probe +2.2%/-0.68dB; encoder==decoder PSNR (no desync, pure economics) | REVERT — +1b/TU floor (~2-3%) exceeds WHT wins on balanced content. Viable shape is per-LEAF/CU type (see V2_1_BATCH) |
| — | search range 32→48 (color probe) | +0.9%, flat | REVERT (MV cost > gain on small motion) |
| — | search range 32→24 (color probe) | +0.2%, +0.25dB | REVERT — tempting but asymmetric risk: clips true motion >24px catastrophically on unseen content |
| — | qpel refinement to convergence (3 iters) | probe +0.4%/+0.04; park −0.15%/flat | REVERT — single pass already converges; no signal |
| — | fast-preset intra to 32px CUs | screen fast byte-identical | REVERT — no effect on tested content |
| — | SATD-based ME refinement | not built — reasoned rejection: RDO already evaluates exact SSE+bits post-ME, so SATD's typical SAD-pipeline gains don't apply here | PARKED |
| — | SPEC truth pass | v2 never had CfL (legacy-only); corrected §4.3, queued v2-CfL (prediction-only, no syntax needed) | DOC |

## Where the climb stands (2026-09-06, session 8: v2-CfL)

Quick-knob phase converged; structural phase opened with two real wins
(multiref ref_sel, v2-CfL). Remaining structural backlog: WHT
transform-type flag, chroma-aware RDO (unblocks skip), B-frame emission,
decode parallelism, 10-clip corpus validation. See TODO.md/MASTER_PLAN.md.

## Session 8 (v2 CfL — first chroma win)

`b_ch` was never set: `ch_intra`=1 path dead encoder-side (v2 intra chroma
always collocated MC). Activated with per-leaf chroma RDO (MC vs CfL-blended
DC, header-honest: CfL pays flag+3 cmode bits) on medium+ full-RDO path;
shared `tc_cfl_blend` helper (predict.c) mirrored in encoder replay, serial
and parallel decoders. Probe color patch finally gives chroma signal.

| Check | Result |
|---|---|
| probe | 44714@24.74 → 43646@24.74 (-2.4% bytes, Y flat, Cb/Cr +0.3–0.5dB TRUE WIN) |
| screen_ui med | 4.13KB@41.67 identical (correct RDO gating — no correlation, no fire) |
| new `test_v2_cfl` | correlated color, bit-exact recon, Cb PSNR gate |

## Session 10 (SAO model x10→x6 — biggest single tuning win)

Cheaper SAO signaling model enables more Band-Offset filtering. Bracketed:
x6 wins huge, x3 overshoots (more SAO bits than distortion saved).

| Check | Result |
|---|---|
| probe | +0.4% bytes, +0.04dB (neutral; SAO barely fires on synthetic) |
| screen_ui ultrafast | 16.89KB@41.11 → 16.15KB@41.26 (-4.4%, +0.15dB) |
| screen_ui medium | 4.11KB@41.82 → 3.39KB@42.28 (-17.5% bytes AND +0.46dB TRUE WIN) |
| decode | 10/10 exact bytes |
| — | ME diamond 4→8 iters | -0.29%, +0.03dB | REVERT |

Cumulative hardened: 29326B@25.49 → 23882B@25.08 (-18.5%, -0.41dB).

## Session 4 (2026-09-06, overfit reckoning — READ THIS BEFORE CLIMBING)

Real-content A/B (`screen_ui` 720p, QP32) exposed probe overfit. Screen numbers:

| Code | ultrafast 10fr | medium 10fr |
|---|---|---|
| pre-climb | 102.46KB @ 38.01dB | — |
| session-1 (RDO+λ200+4×4) | 16.89KB @ 41.11dB | — |
| +deadzone/8+λ250+est×1.5 | 18.77KB @ 39.83dB | — |
| dz/3+λ250 (ablation) | 17.27KB @ 41.15dB | — |
| dz/3+λ200+est×1.5 | 16.89KB @ 41.11dB | 4.39KB @ 40.85dB |
| dz/3+λ200+est×1.0 | — | 4.65KB @ 41.05dB |

Findings:
- Deadzone/8 (a -14.7% probe win) costs +8.7%/-1.3dB on structured content.
  Random-noise patches reward killing coefficients that text/edges need.
  REVERTED to /3.
- λ250 beats λ200 on probe (-2.5%/-0.03) but loses on screen (+2.2%/+0.04).
  REVERTED to 200 (conservative middle; optimum is content-dependent).
- Estimator ×1.5: probe-true-win (-3.5%/+0.24) but -0.2dB for -5.6% on screen
  medium. REVERTED — kept steps must be real-content-neutral-or-better.
- Ultrafast bypasses the estimator (SAD-proxy screening, fixed lb), so it
  cannot validate estimator changes; medium preset required.

New gates (hard lessons):
1. Every kept tuning must pass BOTH hardened probe AND `screen_ui` medium
   10fr QP32 A/B (53s — affordable, no excuses).
2. Random-noise probe patches discriminate poorly for structured content;
   a pseudo-text patch was added (bottom-left); a deadzone/8 negative control
   shows the same exchange rate as noise, so the screen A/B gate stays primary.
3. Exchange-rate rule: reject trades worse than ~0.03 dB per %byte.

Surviving gains (all real-validated): RDO scale fix, λ200, inter-4×4 RDO +
outer-flag fix, Makefile header deps, hardened probe, this log.

## Session 5 (2026-09-06, v2 second reference — first TRUE structural win)

Gated on `MULTI_REF` tool (medium+ preset, streaming-main+ profile): explicit
P-frame inter leaves carry one `ref_sel` bit (after merge flag) choosing
`dpb[0]`/`dpb[1]`; RDO evaluates both with honest bits; serial + parallel
decoder paths parse/reconstruct from the selected ref; DPB already shifts.

| Check | Result |
|---|---|
| gate-off probe | byte-identical (backward compat) |
| synthetic textpatch QP32 med | -11.4% bytes, -0.42dB (diagonal — probe pessimism) |
| `screen_ui` 720p 10fr med QP32 | 4.65KB@41.05 → 4.13KB@41.67dB (-11.2%, +0.62dB TRUE WIN) |
| decode | 10/10 exact bytes, both paths |
| new `test_v2_multiref` | in-process RDO + bit-exact recon check |

## Session 9 (medium intra pruning — speed at ~equal quality)

Medium did exhaustive 18-mode full-RDO intra (18 transforms/leaf). SAD-screen
to top-4, full RDO on those; slow keeps exhaustive. Top-6 trialed: worse on
screen (+3.4% — non-monotonic RDO chaos), so top-4 locked.

| Check | Result |
|---|---|
| probe | +1.9% bytes, -0.01dB Y (accepted speed price, documented) |
| screen_ui med | 4.13KB@41.67 → 4.11KB@41.82 (-0.5%, +0.15dB) + 53s→28s encode (1.9x) |

## Session 11 (v2 B-frames — first temporal-structure win)

Unblocked v2 B emission (`b_mode` for v2) with full BIDIR machinery repair:
poc-ordered refs everywhere (was slot-indexed desync under reorder),
honored `bi` flag (was ignored — always averaged), averaged chroma
(was single-ref), qt_leaf fwd/bwd/bi RDO + merge-as-average, GOP order +
bit-exact test_v2_bframes. No new syntax (flags existed, unused for v2).

| Check | Result |
|---|---|
| probe 3QP | ≈−19% bytes, −0.0..−0.9dB (diagonal; qp32 dip flagged) |
| screen_ui med QP32 | 3.39KB@42.26 → 2.79KB@42.49 (−17.7% AND +0.23dB TRUE WIN) |
| park nature QP32 | 581KB@25.55 → 468KB@25.24 (−19.6%, −0.31dB/−0.007 SSIM; +27% enc time) |
| BD-rate B-vs-P | park −10.12%, screen −16.14% (3-QP curves, current code both) |
| test_v2_bframes + suite | PASS, 53/53 |
| deblock trials 32+33 + full suite | probe −30%/+4.5dB cumul; screen −42%/+2.0dB @QP32; park −22%/+1.6dB @QP32; FULL 54/54 incl. 300fr soak |
