# TCodec v2.1 Syntax Batch — Design (2026-09-06)

These items all need bitstream additions. Batch them into ONE version bump
to avoid repeated compat breaks. Order by value/effort (item 0 first).

## 0. ~~Reference choice beats averaging~~ DONE 2026-09-08 (hill-climb 19)

B-frame selection mix on park water (1746 explicit inter leaves): fwd 30%,
bwd 40%, bi-average 3%, merge 27%. P multiref extended 2→4 refs (2-bit
`ref_idx`, same `MULTI_REF` flag): probe wins all QP, screen −9.4%/+0.61dB,
park −1.0%. Choice value confirmed; joint-search bi remains parked.

## 1. ~~v2 B-frame emission~~ DONE 2026-09-08/09 (hill-climb 18+24+40)

Shipped: poc refs, bi honored, averaged chroma, fwd/bwd/bi RDO, single-ref
merge (merge+bi=0 codepoint, no new syntax — old streams bit-identical).
Post-deblock audit: B neutral park (−0.9% BD), beats-P screen (single-merge
rescued scroll). Benchmarks stay P-only; B ladder retune deferred.

## 2. ~~WHT transform-type flag~~ REJECTED 2026-09-06

Legacy force-DCT is byte-identical to RDO (WHT never selected, even on
checkerboard) and forced-WHT measures +10%/-0.2dB: WHT is genuinely worse
here, not RDO blindness. No v2 work justified. Legacy keeps its harmless
RDO option (frozen path, not worth churning).

## 3. ~~Per-leaf CfL alpha signaling~~ DONE (adaptive sign/magnitude kept)

## 4. Chroma SAO + Edge Offset (medium — still open)

Luma BO exists and won big (−17.5% screen). Chroma BO (same pattern,
separate flags) + luma EO (directional) are natural followers.

## 5. Skip prerequisites (unblocks +10–15% static)

Four failed trials documented in HILLCLIMB sessions 4–8. Needs BOTH:
(a) chroma-aware RDO (winner chroma coded in decision), (b) fresh (MC)
skip chroma instead of stale (syntax-neutral change, but alters old
streams — batch here).

Update 2026-09-07 (5th trial + static-content forensics): even fresh-chroma
skip loses (+10%/−0.14dB) because zeroing merge's epsilon starves future
references — frozen-content drift is −3.1dB/30fr (requant walk) vs x264
perfectly flat (skip exact-copies). Skip fundamentally needs:
(a) chroma-aware RDO, (b) fresh skip chroma, (c) PROPAGATION awareness
(don't zero residuals that future frames need as reference).

Update 2026-09-08 (6th trial, HILLCLIMB 34 — drift premise dead): the
deblock rewrite eliminated frozen drift (+0.30dB/30fr), but skip-6
(fresh chroma + chroma-honest RDO + ρ=1 guard + tool bit) still lost:
ρ=0 −30.7%/−5.06dB, ρ=1 −22.6%/−3.35dB, near-exact gate never fires.
Measured cause: mvp-MC SSE averages 550/cu² even on FROZEN content
(MVP divergence — zero-MVD skip predicts poorly), and lb crud inflates
alternatives. Skip needs (d) decent zero-MVD prediction (better MVP)
on top of (a–c). All reverted; 7th failure total. NOT next.

Update 2026-09-09 (8th trial, HILLCLIMB 44 — mb-tree-lite + static gate):
per-CTU stability SAD → ρ(CTU) + static-CTU gate (S≤2000) + ρ3-uniform.
Kills: CTU-adaptive ρ −10%/−2.1dB; faces need finer-than-CTU gating
(vidyo −13%/−1.4dB at S≤20000, neutral at S≤2000); sita +6%, frozen
+5–19% (mvp-diverged static poisoning). Saves: screen −30%/−0.25dB
(scroll-coherent MVP only). Verdict: value confined to coherent motion;
needs MVP-temporal + per-block ρ (real mb-tree + lookahead). Design in
docs/MBTREE_DESIGN.md. Still NOT next (9th attempt needs new MVP).

## 6. Per-CTU QP deltas + mb-tree-lite (unblocks adaptive allocation)

Frame-QP heuristics (bit-ratio ±, quality servo) all move diagonally —
adaptation without lookahead/propagation can't win. Concrete design
(scoped 2026-09-07):
- Syntax: 2-bit `ctu_qp_delta` at CTU start (before split flag), values
  {−1, 0, +1, +2} (00/01/10/11), tool-gated by new `TC_TOOL_QP_DELTA`
  (bit 15 free). Applies to all leaves in the CTU (eff tables already
  built per-CTU in all 3 decode paths). ~60B/frame overhead at 720p.
- Heuristic (no RDO, 1 pass): per-CTU stability from residual-energy
  history (240 int64s in enc state); stable background → −1 (protect
  references), changing → 0/+1. Key uses: static UI vs scroll regions
  (screen), outer-B +2 refinement.
- Must earn ~2% overhead; validate screen (static regions) + park
  (uniform-motion control). This unblocks skip (#5) and proper CRF.

## Compatibility plan

- v2.1 = new payload version OR tool-gated optional syntax per item.
  Preference: tool-gated bits (decoder reads iff flag set; rejects
  unknown flags) so old v2 streams keep decoding.
- Golden v2 streams + conformance vectors must be captured BEFORE the
  bump (none exist today — create them first).
- BITSTREAM.md §7.6 documents each addition; bump the version tables.
