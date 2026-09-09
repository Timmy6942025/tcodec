# mb-tree-lite + Skip-8 — Design (2026-09-09, trial 44)

## 0. Why now (7 skip failures converge)

Trials 5,16–19,34: RDO-picked skip always loses (up to −47%/−6.6dB)
EXCEPT near-exact gates that never fire. Root causes, all measured:
(a) lb crud inflates alternatives (trial 18), (b) stale chroma (trial 17),
(c) zeroing epsilon starves refs (trial 19), (d) mvp-MC SSE 550/cu² even
frozen — MVP divergence (trial 34), (e) uniform ρ=1 misprices (trial 34).
Single-frame RDO cannot price REFERENCE POISONING. x264's answer is
mb-tree (propagate future-reference value into current costs). This
design builds the minimal version, then re-introduces skip on top.

WINSHARE evidence: park intra 64% / merge 19% (RDO flat between them —
trial-35 honest-lb also neutral); global-center wins 0–2% everywhere
(global SYNTAX killed: GC estimates RDO-rejects even on aerial pan).
Static clips (vidyo +124%, sita-pattern) + frozen (+0.30dB drift, refs
stable) say: static+exact → skip territory, if priced right.

## 1. Stability state (encoder-only, no syntax)

Per-CTU stability S_c = SAD(orig CTU, co-located prev-orig CTU), new
1.4MB prev-orig buffer (memcpy per frame; negligible vs RDO).
Propagation factor ρ_c = clamp(k · S0/(S_c+S0), 0, ρmax), defaults
k=1, S0=50000, ρmax=2 (SWEEP all three coarsely).
Frozen S≈0 → ρ≈ρmax (protect refs); water S≫S0 → ρ≈0 (unchanged).

## 2. ρ economics (applies to zero/low-residual choices)

- Skip-8 candidate: cost = 2·dskip·(1+ρ_c) + λ·5 (trial-34 form with
  content-adaptive ρ; ρ=1-uniform failed, ρ(CTU) is the fix).
- Merge: cost = dlm·(1+0.5·ρ_c) + λ·bits (half weight: merge keeps
  residual, poisons less than skip).
- Explicit: unchanged (reference-quality anchor).
Effect: static+exact → skip wins ties; static+epsilon → explicit beats
merge (protects refs — trial-19 lesson priced in); moving → status quo.

## 3. Skip-8 stack (resurrected from trial 34, gated by ρ)

- Decision: mvp MC dpb[0], zero residual, FRESH MC chroma, distortion =
  luma SSE + chroma SSE, bits = 5. Eligible only if dskip ≤ gate AND ρ
  allows (RDO decides among eligible).
- Replay + serial + parallel decoders: fresh MC chroma on P-skip.
- Compat: FRESH_SKIP tool bit 15 + unknown-flag rejection (both reverted
  in trial 34, return here). Old streams: no skip-CU behavior change
  without the flag... NOTE: fresh chroma alters old-stream skip recon —
  flag-gated emission keeps old streams bit-exact (encoder only emits
  skip-8 when flag set; decoder without flag sees no such leaves...
  actually decoder behavior on flagged streams only. Old streams lack the
  flag → stale path → identical. Airtight.)
- BITSTREAM.md documents FRESH_SKIP.

## 4. Sweep + gates (in order; stop at first red)

S0 ∈ {10k, 50k, 200k} (k=1, ρmax=2 fixed): probe + screen-manual + park
qp32 each (~7 min/point). Keep best-S0 iff ALL THREE beat baseline
RDOQ (28513/29.46, 1.83KB/44.25, 443387/27.05); then parity + fast
suite; then keep/revert verdict. If no S0 wins → park mb-tree (record
sweep data), next: grain-without-vmaf spike or D9 kernels.

## 5. Risks

1. Stability SAD cost per frame (full-frame SAD ≈ 1.4M px ops — trivial).
2. ρ miscalibration → static-bytes regression (explicit over-wins).
   Gates catch (screen/frozen-manual + park).
3. Rejection + fuzz interplay (new error paths must not crash — suite).
4. MVP divergence (550/cu²) caps skip firing rate even when priced
   right — if skip fires <2%, verdict is "mechanism sound, MVP-bound"
   (still revert; MVP-temporal becomes the named next project).
