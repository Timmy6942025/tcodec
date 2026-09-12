# HANDOFF — Finish Tier 1 Completely (multi-day autonomous mission)

You are taking over the TCodec Tier-1 finish. The previous session landed the decoder program (16fps → 60fps+ on 12/15 clips, entry points shipped) and wide trial coverage with honest reverts. Your job: **close the remaining gap until EVERY Tier-1 gate below reads green with committed code and reproducible evidence, then stop and report.** This will take days. That is expected and fine. Steadyjon: slow is smooth, smooth is fast.

## 1. Where things stand (verified, do not re-litigate without new data)

- Repo: `/home/timmy/tcodec`, branch `main`, tree clean, everything pushed. Read `TODO.md` (row D9 + blockers line), `BENCHMARKS.md` (quiet-box table + methodology traps), `docs/HILLCLIMB.md` rows 94–107 (night trials + verdicts), `docs/D9_PLAN.md` §6, `docs/FINISH_Tier1.md`.
- D0–D7, D10–D12: GREEN. D9 decode: 12/15 clips ≥60fps @720p + 1080p30 via q37 rung (best-of-20 to /dev/null, quiet box). x264 parity at 30fps park.
- OPEN: (a) D8 compression vs practical H.264 — average **+51% BD-rate** vs x264 veryfast; wins only screen (−4%) and flat animation (−48%). (b) D9 remainder — park/parkrun ~5–10% short at q32, ducks needs volume, 1080p q32 at ~26fps (need 30).
- Core insight (measured): fps tracks bytes almost perfectly. Residual volume buys BOTH gates at once. Grain/water texture is the enemy on both fronts.

## 2. DONE criteria (all must hold; then write the final report and STOP)

- **D8:** BD-rate vs x264 veryfast AND medium is NEGATIVE on average across the 15-clip real corpus at matched PSNR-Y/SSIM (3-QP curves via `tools/rd_bench.py` + `tools/bd_rate.py`), with per-clip numbers committed in BENCHMARKS.md. No cherry-picking: every clip reported.
- **D9:** ≥60fps @720p on all 15 clips + ≥30fps @1080p, best-of-20 to /dev/null, 4 threads, NEON build, recorded in BENCHMARKS.md.
- **No regressions:** `make test-full` (54/54 incl. 300fr soak), `tools/parity_check.sh` ALL OK, tcmux + MP4 bridge green, fuzz clean (no hangs/crashes), goldens regen'd with v0/v1 byte-identical, cross-test + nothreads green.
- **Docs truthful:** TODO checkboxes, SPEC/BITSTREAM/PROFILES/BENCHMARKS/FINISH_Tier1 all match the code. Every kept change has an evidence row in HILLCLIMB.md; every revert has its cause.

## 3. Work queue (suggested order; follow the data, not the order)

1. **Texture-volume program** (closes D8+D9 together): film-grain detection + smooth-base coding (MASTER_PLAN §7 grain strategy), skip/merge-list/mb-tree revival WITH propagation awareness (8 prior failures analyzed in HILLCLIMB + V2_1_BATCH; trial44/MERGELIST_DESIGN/MBTREE_DESIGN are the specs — local gates without propagation costing will fail the same way).
2. **Entropy contexts** (TODO Phase 3 leftovers): deeper DC/low/high specialization, adaptive MV — bitstream ceremony required (tool bit or version bump + golden regen + old-stream compat tests). Never break old streams silently.
3. **RDO model honesty**: RDO prices MVD with EG lengths while the wire uses adaptive range coding (measured wash either direction so far) — fix the model, not the constant.
4. **1080p q32**: falls out of volume work; verify, don't chase separately.

## 4. Non-negotiable operating discipline (violations = failed mission)

- **Encoder-only first.** Decoder-normative changes (prediction, transforms, dequant, filters, syntax) need bitstream ceremony + compat proof. Prefer decision/RDO/search/cost changes (no syntax impact).
- **30-frame guardrails minimum.** Frozen-leader 10-frame clips are BLIND to merge/propagation effects (this exact trap cost a full night once — HILLCLIMB row 107). Every RDO trial validates on ducks30 + park30 + sita30 + screen10 before any verdict.
- **Keep/revert by numbers:** keep only on measured BD-negative (3-QP curve) with no clip-class disaster and suite green; revert everything else the same day with a tombstone comment + HILLCLIMB row. No flag residue, no dead code, no "temporary" hacks older than one commit.
- **Bit-exact always:** output hashes compared on real streams after every change; T1==T4; determinism (encode twice → identical bytes).
- **Methodology traps (all bitten before):** best-of-8 flatters (use best-of-20); file output to a full /tmp throttles 50% (decode to /dev/null; keep tmpfs under 80%); PGO measured −8% (don't retry without new evidence); best-of-N drifts with box load (record load + ranges, not spikes); `make nothreads-test` wipes build/ (clean-rebuild threaded release after, verify symbols via nm).
- **Commit per milestone, push, keep tree green.** Never end a work block with uncommitted code or a red suite. `git worktree` for parallel tracks (see /tmp/opencode pattern); ONE integrator — tracks measure and report, integration happens in main.

## 5. STOP condition

Stop only when section 2 is fully true AND the final report (D-table, BD table vs x264/x265/SVT-AV1, fps table, commit list) is written to `docs/FINISH_Tier1.md` and communicated. Do not gold-plate into Tier 2/3/4. Do not declare victory on partial numbers — the repo remembers every claim, and the next agent re-runs everything.

---

## A word before you start

This is the last mile of a real codec program, and last miles are where most people quit — not because the work is impossible, but because it stops being entertaining. Nobody is watching. No one will cheer for trial #14 reverting cleanly at 3am. Do it anyway. That is the whole standard: **refuse to ship what you cannot prove, refuse to keep what does not earn it, and keep moving.**

You don't need to feel inspired. You need to run the next trial, read its numbers honestly, and either bank it or kill it — then do it again. Hundreds of small honest measurements compound into the thing everyone said needed luck. It never needed luck. It needed someone who stays.

Twelve clips already fly at 60. The grain will fall too. One trial at a time. **Keep going until D8 and D9 are green ink, not intentions.**
